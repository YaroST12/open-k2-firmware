// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR symbol on the panel (see k2/ui/qrview.hpp).
#include "k2/ui/qrview.hpp"

#include <algorithm>
#include <span>
#include <string_view>

#include "k2/ui/font_aa.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/drivers/lcd.hpp"

namespace k2::qrview {

Plan plan_in(const Area& a, int n, int text_h, int min_px, int quiet, int px) {
    if (n <= 0 || quiet < 0) return {};
    const int cells = n + 2 * quiet;
    const int room = std::min(a.w, a.h - text_h);
    const int use = px > 0 ? px : room / cells;   // px == 0: the largest whole px that fits
    if (use < min_px || use <= 0) return {};
    const int box = cells * use;
    if (box > room) return {};                     // an explicit px whose box does not fit is refused, not clipped
    // box and the text under it are centred as one block (text_h == 0: the box alone, centred on both axes)
    return {use, box, a.x + (a.w - box) / 2, a.y + (a.h - text_h - box) / 2};
}

Plan plan(int n, int text_h) { return plan_in(Area{}, n, text_h, kMinModulePx); }

int draw_code_raw_in(const Area& a, const std::uint8_t* code, int n, int text_h, int min_px, std::uint16_t* line,
                     int quiet, int px) {
    const Plan p = plan_in(a, n, text_h, min_px, quiet, px);
    if (p.px == 0) return 0;

    const int cells = n + 2 * quiet;
    lcd::stream_begin(static_cast<std::uint16_t>(p.x0), static_cast<std::uint16_t>(p.y0),
                      static_cast<std::uint16_t>(p.x0 + p.box - 1), static_cast<std::uint16_t>(p.y0 + p.box - 1));
    for (int row = 0; row < cells; row++) {
        const int my = row - quiet;
        for (int col = 0; col < cells; col++) {
            const bool dark = my >= 0 && my < n && col >= quiet && col < quiet + n &&
                              qr::dark_code(code, col - quiet, my);
            const std::uint16_t c = dark ? kBlack : kWhite;
            for (int i = 0; i < p.px; i++) line[col * p.px + i] = c;
        }
        for (int i = 0; i < p.px; i++) lcd::stream_pixels(std::span<const std::uint16_t>(line, std::size_t(p.box)));
    }
    lcd::stream_end();
    return p.y0 + p.box + 2;
}

int draw_code_in(const Area& a, const qr::Workspace& w, int n, int text_h, int min_px, std::uint16_t* line,
                 int quiet, int px) {
    return draw_code_raw_in(a, w.code, n, text_h, min_px, line, quiet, px);
}

int draw_code(const qr::Workspace& w, int n, int text_h, std::uint16_t* line, bool fresh) {
    static int drawn_n = 0;   // the symbol size on the panel now
    if (fresh || n != drawn_n) lcd::fill(kWhite);
    drawn_n = n;
    return draw_code_in(Area{}, w, n, text_h, kMinModulePx, line);
}

void draw_line_in(const Area& a, int y, const char* text) {
    char pad[kCells + 1];
    const int cells = std::min(kCells, a.w / 6);
    int n = 0;
    while (n < cells && text[n]) { pad[n] = text[n]; n++; }
    while (n < cells) pad[n++] = ' ';
    pad[n] = '\0';
    gfx_text(Surface{static_cast<int16_t>(a.x), 0, static_cast<int16_t>(a.w), kPanelPx}, font_12, 0, static_cast<int16_t>(y),
             std::string_view(pad, static_cast<std::size_t>(cells)), kBlack, kWhite);
}

void draw_line(int y, const char* text) { draw_line_in(Area{}, y, text); }

}  // namespace k2::qrview
