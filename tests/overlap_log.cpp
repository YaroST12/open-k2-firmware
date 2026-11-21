// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Overlap guard: which text draws still show ink on the frame, and did another draw overpaint part of it?
// gfx_text / gfx_text_rot report every draw (box in panel pixels); the fake LCD keeps, per panel pixel, the stream
// window that wrote it last (g_lcd_owner). At the end of a frame each draw's ink pixels (coverage of half or more) are
// looked up there: ink still owned by the draw is alive, ink owned by another text draw is damage. A draw with no ink left
// is dead (repainted over: a field updating in place, a scrolled row) and is dropped.
#include "overlap_log.h"
#include "fake_lcd.h"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/trunc.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
using namespace k2;

struct Rec {
    const Font *f;
    std::string text;
    int kx, ky, nx, ny, w, h, dir;
    unsigned id;
    bool ok;
    unsigned field;
};

std::vector<Rec> &recs() { static std::vector<Rec> r; return r; }

void on_text(const Font *f, std::string_view text, int kx, int ky, int nx, int ny, int w, int h, int dir, bool ok, unsigned field) {
    if (w <= 0 || h <= 0 || text.empty()) return;
    recs().push_back({f, std::string(text), kx, ky, nx, ny, w, h, dir, g_lcd_windows, ok, field});
}

// Ink mask of one draw: the pixels that carry at least half coverage, row-major w x h.
void ink_of(const Rec &r, std::vector<unsigned char> &ink) {
    uint16_t lut[16];
    for (int i = 0; i < 16; ++i) lut[i] = i >= 8 ? 1 : 0;
    ink.assign(static_cast<size_t>(r.w) * r.h, 0);
    uint16_t line[256];
    if (r.dir < 0) {
        for (int y = 0; y < r.h; ++y)
            for (int x0 = 0; x0 < r.w; x0 += 256) {
                const int n = render_row_span(*r.f, r.text, y, lut, line, x0, std::min(256, r.w - x0));
                for (int i = 0; i < n; ++i) ink[static_cast<size_t>(y) * r.w + x0 + i] = line[i] != 0;
            }
    } else {
        for (int sy = 0; sy < r.h; ++sy) {
            render_rotated_row(*r.f, r.text, static_cast<TextDir>(r.dir), sy, lut, line);
            for (int sx = 0; sx < r.w; ++sx) ink[static_cast<size_t>(sy) * r.w + sx] = line[sx] != 0;
        }
    }
}

bool covers(const Rec &a, const Rec &b) {
    return a.nx <= b.nx && a.ny <= b.ny && a.nx + a.w >= b.nx + b.w && a.ny + a.h >= b.ny + b.h;
}

std::string describe(const Rec &r) {
    char b[160];
    std::snprintf(b, sizeof b, "\"%s\" (%d,%d %dx%d)", r.text.c_str(), r.nx, r.ny, r.w, r.h);
    return b;
}

// K2_OVERLAP_DUMP=dir [K2_OVERLAP_MATCH=text]: the panel of the first frame whose hit mentions the text (any hit without
// one) goes to dir/k2_ov_<n>.panel for tools/panel2png.py.
void dump_frame(const std::string &what) {
    static int count = 0;
    const char *dir = std::getenv("K2_OVERLAP_DUMP"), *match = std::getenv("K2_OVERLAP_MATCH");
    if (!dir || (match && what.find(match) == std::string::npos) || count >= 12) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/k2_ov_%03d.panel", dir, count++);
    if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    std::fprintf(stderr, "%s: %s\n", path, what.c_str());
}

void check_frame() {
    std::vector<Rec> &v = recs();
    std::vector<unsigned char> ink;
    std::vector<Rec> alive;
    for (const Rec &r : v) {
        ink_of(r, ink);
        int intact = 0;
        std::vector<std::pair<unsigned, int>> hit;   // owner window -> pixels of this draw's ink it took over
        for (int y = 0; y < r.h; ++y)
            for (int x = 0; x < r.w; ++x) {
                if (!ink[static_cast<size_t>(y) * r.w + x]) continue;
                const int px = r.nx + x, py = r.ny + y;
                if (px < 0 || py < 0 || px >= LCD_WIDTH || py >= LCD_HEIGHT) continue;
                const unsigned own = g_lcd_owner[py * LCD_WIDTH + px];
                if (own == r.id) { ++intact; continue; }
                auto it = std::find_if(hit.begin(), hit.end(), [&](const auto &p) { return p.first == own; });
                if (it == hit.end()) hit.push_back({own, 1}); else ++it->second;
            }
        if (!intact) continue;   // repainted over: dead
        alive.push_back(r);
        for (const auto &[own, n] : hit) {
            if (n < 2) continue;
            const Rec *by = nullptr;
            for (const Rec &o : v) if (o.id == own) by = &o;
            // A deliberate composite (toast over the page, text over a trend) is legal against ordinary text, but two composites
            // of one kind still must not overlap each other (a narrower toast leaving the old pill's ends).
            if (!by || by->ok != r.ok) continue;
            if (r.field && by->field == r.field) continue;   // a Field pads and diffs its own cells
            // The same field redrawing over itself is an update, but only when the new box covers the old one: a shorter
            // text at the same origin leaves the old tail on screen.
            if (by->f == r.f && by->kx == r.kx && by->ky == r.ky && covers(*by, r)) continue;
            const std::string what = describe(r) + " overpainted by " + describe(*by) + ", " + std::to_string(n) + " px";
            trunc::note(trunc::Kind::Overlap, what, 0);
            dump_frame(what);
        }
    }
    v.swap(alive);
}

}  // namespace

void overlap_reset() {
    recs().clear();
    std::fill(std::begin(g_lcd_owner), std::end(g_lcd_owner), 0u);
}

void overlap_enable() {
    trunc::set_text_hook(on_text);
    trunc::set_frame_hook(check_frame);
}
