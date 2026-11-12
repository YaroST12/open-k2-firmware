// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Every glyph of every shipped font, decoded through the renderer (render_row, render_row_span in
// windows, render_column), must equal the uncompressed level matrix the generator rasterised
// (tools/font_levels.txt, written by tools/gen_font.py: an independent source of the pixels, not
// the coded tables), so the run-length coding in src/ui/fonts/*.cpp loses nothing.
#include "test_util.h"
#include "k2/ui/text.hpp"
#include "k2/ui/font_aa.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace k2;

namespace {

struct RefGlyph {
    uint32_t cp = 0;
    int w = 0, h = 0, left = 0, top = 0;
    std::vector<std::string> rows;   // one hex digit per pixel
};
struct RefFont {
    int px = 0, count = 0, adv = 0, ascent = 0, descent = 0, height = 0;
    std::vector<RefGlyph> glyphs;
};

std::vector<RefFont> load_reference() {
    std::ifstream in("tools/font_levels.txt");
    if (!in) in.open("../tools/font_levels.txt");
    std::vector<RefFont> fonts;
    std::string line;
    int rows_left = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (rows_left > 0) {
            fonts.back().glyphs.back().rows.push_back(line);
            rows_left--;
            continue;
        }
        std::istringstream t(line);
        std::string kind;
        t >> kind;
        if (kind == "font") {
            RefFont f;
            t >> f.px >> f.count >> f.adv >> f.ascent >> f.descent >> f.height;
            fonts.push_back(f);
        } else if (kind == "glyph") {
            RefGlyph g;
            t >> g.cp >> g.w >> g.h >> g.left >> g.top;
            rows_left = g.h;
            fonts.back().glyphs.push_back(g);
        }
    }
    return fonts;
}

std::string utf8(uint32_t cp) {
    std::string s;
    if (cp < 0x80) s += static_cast<char>(cp);
    else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
    else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

const Font *font_for(int px) {
    switch (px) {
        case 12: return &font_12;
        case 16: return &font_16;
        case 24: return &font_24;
        case 48: return &font_48;
        case 64: return &font_64;
    }
    return nullptr;
}

// Expected level at box-relative (gx, gy), 0 outside the ink box.
int level_at(const RefGlyph &g, int gx, int gy) {
    if (gx < 0 || gx >= g.w || gy < 0 || gy >= g.h) return 0;
    const char c = g.rows[gy][gx];
    return c <= '9' ? c - '0' : c - 'A' + 10;
}

void check_font(const RefFont &rf) {
    const Font *f = font_for(rf.px);
    CHECK_COND(f != nullptr);
    if (!f) return;
    CHECK_EQ(f->count, rf.count);
    CHECK_EQ(f->adv, rf.adv);
    CHECK_EQ(f->ascent, rf.ascent);
    CHECK_EQ(f->descent, rf.descent);
    CHECK_EQ(f->height, rf.height);
    CHECK_EQ(f->px, rf.px);
    CHECK_COND(f->zmax >= 1 && f->zmax <= 15);
    CHECK_EQ(static_cast<int>(rf.glyphs.size()), rf.count);

    uint16_t lut[16];
    for (int i = 0; i < 16; i++) lut[i] = static_cast<uint16_t>(i);   // identity: the output is the level
    std::vector<uint16_t> whole(static_cast<size_t>(f->adv) + 4), part(whole.size());
    std::vector<uint16_t> col(static_cast<size_t>(f->height) + 4);
    int bad_rows = 0, bad_windows = 0, bad_cols = 0, bad_table = 0;

    for (size_t k = 0; k < rf.glyphs.size(); k++) {
        const RefGlyph &g = rf.glyphs[k];
        const Glyph &fg = f->glyphs[k];
        bad_table += find_glyph(*f, g.cp) != &fg;                     // the lookup (dense run + sparse tail) finds it
        bad_table += k < f->dense ? g.cp != 0x20 + k : f->codepoints[k - f->dense] != g.cp;
        bad_table += fg.w != g.w || fg.h != g.h || fg.left != g.left || fg.top != g.top;
        const std::string s = utf8(g.cp);

        for (int r = 0; r < f->height; r++) {
            const int gy = r - (f->ascent - g.top);
            render_row_span(*f, s, r, lut, whole.data(), 0, f->adv);
            for (int x = 0; x < f->adv; x++) bad_rows += whole[x] != level_at(g, x - g.left, gy);
            // Windows of 1, 3 and 5 columns at every start (the callers stream a row in chunks).
            for (int win : {1, 3, 5}) {
                for (int x0 = 0; x0 < f->adv; x0++) {
                    const int n = render_row_span(*f, s, r, lut, part.data(), x0, win);
                    for (int x = 0; x < n; x++) bad_windows += part[x] != whole[x0 + x];
                }
            }
        }
        for (int c = 0; c < f->adv; c++) {
            render_column(*f, s, c, lut, col.data());
            for (int y = 0; y < f->height; y++) bad_cols += col[y] != level_at(g, c - g.left, y - (f->ascent - g.top));
        }
    }
    CHECK_EQ(bad_table, 0);
    CHECK_EQ(bad_rows, 0);
    CHECK_EQ(bad_windows, 0);
    CHECK_EQ(bad_cols, 0);
}

}  // namespace

int main() {
    const std::vector<RefFont> fonts = load_reference();
    CHECK_EQ(static_cast<int>(fonts.size()), 5);
    g_missing_glyphs = 0;
    for (const RefFont &rf : fonts) check_font(rf);
    CHECK_EQ(g_missing_glyphs, 0u);   // every reference glyph was found, none fell back to '?'
    return test_report("test_font_decode");
}
