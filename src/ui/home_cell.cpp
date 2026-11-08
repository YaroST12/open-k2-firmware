// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_cell.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"

namespace k2::home {
namespace {

// Identity look-up table: render_row() then returns the 0..15 coverage of each pixel, which is
// blended against the pixel already in the row instead of one fixed background.
constexpr uint16_t kCoverage[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

// The 22 x 22 arrow pointing up: a 16 px wide head over a 6 px shaft.
bool up_hit(int x, int y) {
    if (x < 0 || y < 0 || x >= kArrowBox || y >= kArrowBox) return false;
    const int u = x * 2 - (kArrowBox - 1);
    const int a = u < 0 ? -u : u;     // odd, 1 at the centre
    if (y <= 9 && a <= 1 + y * 14 / 9) return true;
    return y >= 8 && a <= 5;
}

void put_text_row(const Item &it, int y, int x0, int x1, uint16_t *out) {
    const Font &f = *it.font;
    const int ty = y - it.y;
    if (ty < 0 || ty >= f.height) return;
    uint16_t cov[64];
    for (size_t k = 0; k < it.text.size(); k++) {
        const int cx = it.x + static_cast<int>(k) * f.adv;
        if (cx > x1 || cx + f.adv - 1 < x0) continue;
        if (it.text[k] == ' ') continue;
        const int n = render_row(f, it.text.substr(k, 1), ty, kCoverage, cov, f.adv);
        for (int i = 0; i < n; i++) {
            const int x = cx + i;
            if (x < x0 || x > x1 || cov[i] == 0) continue;
            uint16_t &px = out[x - x0];
            px = cov[i] >= 15 ? it.color : blend565(it.color, px, cov[i]);
        }
    }
}

void put_item_row(const Item &it, int y, int x0, int x1, uint16_t *out) {
    switch (it.kind) {
    case ItemKind::Fill:
        if (y < it.y || y >= it.y + it.h) return;
        for (int x = it.x < x0 ? x0 : it.x, e = (it.x + it.w - 1) > x1 ? x1 : (it.x + it.w - 1); x <= e; x++) out[x - x0] = it.color;
        break;
    case ItemKind::Text:
        if (it.font) put_text_row(it, y, x0, x1, out);
        break;
    case ItemKind::Arrow:
        if (y < it.y || y >= it.y + kArrowBox) return;
        for (int x = it.x < x0 ? x0 : it.x, e = (it.x + kArrowBox - 1) > x1 ? x1 : (it.x + kArrowBox - 1); x <= e; x++)
            if (arrow_hit(it.aux, x - it.x, y - it.y)) out[x - x0] = it.color;
        break;
    case ItemKind::Warn:
        if (y < it.y || y >= it.y + kWarnBox) return;
        for (int x = it.x < x0 ? x0 : it.x, e = (it.x + kWarnBox - 1) > x1 ? x1 : (it.x + kWarnBox - 1); x <= e; x++) {
            const int h = warn_hit(x - it.x, y - it.y);
            if (h) out[x - x0] = h == 2 ? it.ink : it.color;
        }
        break;
    }
}

}  // namespace

bool arrow_hit(uint8_t aux, int x, int y) {
    constexpr int n = kArrowBox;
    if (x < 0 || y < 0 || x >= n || y >= n) return false;
    switch (aux & 7) {
    case 0: return up_hit(x, y);
    case 1: return up_hit(n - 1 - y, x);        // left: the tip at small x
    case 2: return up_hit(n - 1 - x, n - 1 - y);
    case 3: return up_hit(y, n - 1 - x);        // right
    case kDashV: return x >= 9 && x <= 12 && y >= 5 && y <= 16;
    case kDashH: return y >= 9 && y <= 12 && x >= 5 && x <= 16;
    default: return false;
    }
}

int warn_hit(int x, int y) {
    if (x < 0 || y < 0 || x >= kWarnBox || y >= kWarnBox) return 0;
    const int u = x * 2 - (kWarnBox - 1);
    const int a = u < 0 ? -u : u;
    if (a > y) return 0;
    if (x >= 4 && x <= 6 && ((y >= 4 && y <= 6) || y == 8)) return 2;
    return 1;
}

uint16_t bg_at(const Cell &c, int x, int y) {
    if (!c.sweep || !c.sw || y < c.geom.t0 || y > c.geom.t1 || x < kColX0) return c.bg;
    const int col = col_of_x(x);
    if (col >= kCols) return c.bg;
    if (in_gap(col, c.sw->cursor)) return (col == c.sw->cursor && x == x_of_col(col)) ? c.cursor_line : c.bg;
    const int r = c.sw->rows[col];
    if (r == 0) return c.bg;
    const int k = c.geom.t1 - y + 1;     // 1 at the bottom sweep row
    return k < r ? c.tint : k == r ? c.line : c.bg;
}

void compose_row(const Cell &c, int y, int x0, int x1, uint16_t *out) {
    for (int x = x0; x <= x1; x++) out[x - x0] = bg_at(c, x, y);
    for (const Item &it : c.items) put_item_row(it, y, x0, x1, out);
}

void paint(const Cell &c, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (x1 > kW - 1) x1 = kW - 1;
    if (y0 < c.geom.top) y0 = c.geom.top;
    if (y1 > c.geom.bottom) y1 = c.geom.bottom;
    if (x0 > x1 || y0 > y1) return;
    uint16_t row[kW];
    lcd::stream_begin(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1), static_cast<uint16_t>(y1));
    for (int y = y0; y <= y1; y++) {
        compose_row(c, y, x0, x1, row);
        lcd::stream_pixels({row, static_cast<size_t>(x1 - x0 + 1)});
    }
    lcd::stream_end();
}

}  // namespace k2::home
