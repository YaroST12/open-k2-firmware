// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Hot-path GraphsPage column painters placed in the zero-wait flash window (K2_HOT).
#include "k2/ui/ui_graphs.hpp"

#include <span>
#include "k2/core/hot.h"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"

namespace k2 {

K2_HOT void GraphsPage::paint_col(int k, int c, const Col &now, int r0, int r1) const {
    const Surface &S = ctx_.screen;
    const int x0 = g_.col_x(c), cw = g_.col_x(c + 1) - x0;
    uint16_t y = static_cast<uint16_t>(S.y + g_.graph_y0(k) + r0), h = static_cast<uint16_t>(r1 - r0 + 1);
    if (!lcd::clip_to_band(y, h)) return;
    r0 = y - S.y - g_.graph_y0(k);
    r1 = r0 + h - 1;
    const uint16_t line = tone(static_cast<home::Mode>(mode_), band_color(k));
    const uint16_t dot = mode_ == static_cast<uint8_t>(home::Mode::Stale) ? kStale : kText;
    const int dx0 = dot_x0();
    uint16_t buf[4 * 40];
    int n = 0;
    lcd::stream_begin(static_cast<uint16_t>(S.x + x0), y, static_cast<uint16_t>(S.x + x0 + cw - 1), static_cast<uint16_t>(y + h - 1));
    for (int r = r0; r <= r1; r++) {
        uint16_t px = kBg;
        if (now.lim == r) px = kWattL;
        if (now.lo != kNoRow && now.lo <= r && r <= now.hi) px = line;
        const bool dot_row = now.dot != kNoRow && r >= now.dot && r <= now.dot + 2;
        for (int i = 0; i < cw; i++) {
            const int x = x0 + i;
            buf[n++] = dot_row && x >= dx0 && x <= dx0 + 2 ? dot : px;
            if (n == static_cast<int>(sizeof buf / sizeof *buf)) { lcd::stream_pixels({buf, static_cast<size_t>(n)}); n = 0; }
        }
    }
    if (n) lcd::stream_pixels({buf, static_cast<size_t>(n)});
    lcd::stream_end();
}

K2_HOT bool GraphsPage::paint_graph(int k) {
    Col now[graphs::kCols];
    want(k, now);
    const int gh = g_.graph_rows();
    for (int c = 0; c < graphs::kCols; c++) {
        Col &was = col_[k][c];
        const Col &is = now[c];
        int r0 = gh, r1 = -1;
        auto take = [&](int a, int b) { if (a < r0) r0 = a; if (b > r1) r1 = b; };
        if (was.lo == kUnknown) {
            take(0, gh - 1);
        } else {
            if (was.lo != is.lo || was.hi != is.hi) {
                if (was.lo != kNoRow) take(was.lo, was.hi);
                if (is.lo != kNoRow) take(is.lo, is.hi);
            }
            if (was.lim != is.lim) {
                if (was.lim != kNoRow) take(was.lim, was.lim);
                if (is.lim != kNoRow) take(is.lim, is.lim);
            }
            if (was.dot != is.dot) {
                if (was.dot != kNoRow) take(was.dot, was.dot + 2);
                if (is.dot != kNoRow) take(is.dot, is.dot + 2);
            }
        }
        if (r1 < r0) continue;
        if (!admit(ctx_.budget, (g_.col_x(c + 1) - g_.col_x(c)) * (r1 - r0 + 1))) return false;
        paint_col(k, c, is, r0, r1);
        was = is;
    }
    return true;
}

}  // namespace k2
