// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/home_cell.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"

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
    if (col == c.sw->cursor && x == x_of_col(col)) return c.cursor_line;   // the thin cursor line; the old trace stays beside it
    const int r = c.sw->rows[col];
    if (r == 0) return c.bg;
    const int k = c.geom.t1 - y + 1;     // 1 at the bottom sweep row
    return k < r ? c.tint : k == r ? c.line : c.bg;
}

namespace {
// with_text=false leaves the Text items out: the stretched-rotation compositor stretches the rest of the
// row to panel pixels first and lays the glyphs over it at their native size.
void compose_row_impl(const Cell &c, int y, int x0, int x1, uint16_t *out, bool with_text) {
    for (int x = x0; x <= x1; x++) out[x - x0] = bg_at(c, x, y);
    for (const Item &it : c.items) {
        if (!with_text && it.kind == ItemKind::Text) continue;
        put_item_row(it, y, x0, x1, out);
    }
}

// One text item's glyph box in panel pixels (native size, origin mapped like gfx_text).
struct TextBox {
    const Item *it;
    Rect r;
};
constexpr size_t kMaxTextBoxes = 16;
// Panel pixels composed and streamed at a time (160 B of stack instead of a 480 B full row).
constexpr int kRowChunk = 80;

// A landscape rotation maps the 224x240 logical content onto a 240x224 panel box with a non-integer,
// non-square stretch, while a glyph keeps its native pixel size. The rectangle is therefore composed per
// PANEL row and written once. The written region is the stretched rectangle plus the native boxes of the
// glyphs inside it (a glyph is taller than its stretched box, so a changed digit also owns the rows below it),
// clipped to the cell. Per panel row the logical row that owns it (source_y) is composed without text, widened
// to panel columns in place (source_x, right to left), and the glyph boxes covering the row are overlaid in
// item order. Every panel pixel then depends on its own state only, so a repaint strip equals the same
// pixels of a full-frame paint, and nothing is erased that a glyph does not cover again.
// `row` holds kRowChunk pixels.
void paint_stretched(const Cell &c, int x0, int y0, int x1, int y1, uint16_t *row) {
    const Rect pr = ContentViewport::mapped_rect({static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                                  static_cast<int16_t>(x1 - x0 + 1), static_cast<int16_t>(y1 - y0 + 1)});
    const Rect cp = ContentViewport::mapped_rect({0, c.geom.top, kW, static_cast<int16_t>(c.geom.bottom - c.geom.top + 1)});
    if (pr.w <= 0 || pr.h <= 0 || cp.w <= 0 || cp.h <= 0) return;
    int rx0 = pr.x, ry0 = pr.y, rx1 = pr.x + pr.w - 1, ry1 = pr.y + pr.h - 1;
    TextBox boxes[kMaxTextBoxes];
    size_t nb = 0;
    for (const Item &it : c.items) {
        if (it.kind != ItemKind::Text || !it.font || nb == kMaxTextBoxes) continue;
        const Font &f = *it.font;
        const int tw = text_width(f, it.text);
        const Rect full = ContentViewport::native_rect({it.x, it.y, static_cast<int16_t>(tw), static_cast<int16_t>(f.height)});
        if (full.w <= 0 || full.h <= 0) continue;
        boxes[nb++] = {&it, full};
        // The part of this glyph box inside the logical rect lands at native size from the mapped box origin.
        const int ix0 = it.x > x0 ? it.x : x0, ix1 = it.x + tw - 1 < x1 ? it.x + tw - 1 : x1;
        const int iy0 = it.y > y0 ? it.y : y0, iy1 = it.y + f.height - 1 < y1 ? it.y + f.height - 1 : y1;
        if (ix0 > ix1 || iy0 > iy1) continue;
        const int nx = full.x + (ix0 - it.x), ny = full.y + (iy0 - it.y);
        const int nx1 = nx + (ix1 - ix0) < full.x + full.w - 1 ? nx + (ix1 - ix0) : full.x + full.w - 1;
        const int ny1 = ny + (iy1 - iy0) < full.y + full.h - 1 ? ny + (iy1 - iy0) : full.y + full.h - 1;
        if (nx > nx1 || ny > ny1) continue;
        if (nx < rx0) rx0 = nx;
        if (ny < ry0) ry0 = ny;
        if (nx1 > rx1) rx1 = nx1;
        if (ny1 > ry1) ry1 = ny1;
    }
    if (rx0 < cp.x) rx0 = cp.x;
    if (ry0 < cp.y) ry0 = cp.y;
    if (rx1 > cp.x + cp.w - 1) rx1 = cp.x + cp.w - 1;
    if (ry1 > cp.y + cp.h - 1) ry1 = cp.y + cp.h - 1;
    const int w = rx1 - rx0 + 1;
    if (w <= 0 || w > 240 || ry0 > ry1) return;
    const int lx0 = ContentViewport::source_x(rx0), lx1 = ContentViewport::source_x(rx1);
    if (lx0 < 0 || lx1 < lx0) return;
    ContentViewport::Pause unscaled;     // the writes below are panel pixels
    lcd::stream_begin(static_cast<uint16_t>(rx0), static_cast<uint16_t>(ry0), static_cast<uint16_t>(rx1), static_cast<uint16_t>(ry1));
    for (int py = ry0; py <= ry1; py++) {
        const int ly = ContentViewport::source_y(py);
        // The row goes out in windows of at most kRowChunk panel pixels (consecutive stream_pixels calls continue
        // the open window); every pixel depends on its own column only, so the windows equal one full-width pass.
        for (int cx0 = rx0; cx0 <= rx1;) {
            int cx1 = cx0 + kRowChunk - 1 < rx1 ? cx0 + kRowChunk - 1 : rx1;
            const int clo = ContentViewport::source_x(cx0);
            // The logical columns behind the window must fit the buffer too (a down-scaling viewport would not).
            while (cx1 > cx0 && ContentViewport::source_x(cx1) - clo + 1 > kRowChunk) cx1--;
            const int chi = ContentViewport::source_x(cx1);
            const int cw = cx1 - cx0 + 1;
            if (ly < 0 || clo < 0 || chi < clo) {   // no logical row/column owns it (cannot happen inside the cell's mapped area)
                for (int i = 0; i < cw; i++) row[i] = c.bg;
            } else {
                compose_row_impl(c, ly, clo, chi, row, false);
                for (int i = cw - 1; i >= 0; i--) {
                    const int lx = ContentViewport::source_x(cx0 + i);
                    row[i] = row[(lx < clo || lx > chi ? clo : lx) - clo];
                }
            }
            for (size_t b = 0; b < nb; b++) {
                const Rect &r = boxes[b].r;
                if (py < r.y || py >= r.y + r.h || r.x > cx1 || r.x + r.w - 1 < cx0) continue;
                Item glyphs = *boxes[b].it;
                glyphs.x = r.x;
                glyphs.y = r.y;
                put_text_row(glyphs, py, cx0, r.x + r.w - 1 < cx1 ? r.x + r.w - 1 : cx1, row);
            }
            lcd::stream_pixels({row, static_cast<size_t>(cw)});
            cx0 = cx1 + 1;
        }
    }
    lcd::stream_end();
}
}  // namespace

void compose_row(const Cell &c, int y, int x0, int x1, uint16_t *out) {
    compose_row_impl(c, y, x0, x1, out, true);
}

void paint(const Cell &c, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (x1 > kW - 1) x1 = kW - 1;
    if (y0 < c.geom.top) y0 = c.geom.top;
    if (y1 > c.geom.bottom) y1 = c.geom.bottom;
    if (x0 > x1 || y0 > y1) return;
    uint16_t row[kRowChunk];
    if (!ContentViewport::stretched()) {
        lcd::stream_begin(static_cast<uint16_t>(x0), static_cast<uint16_t>(y0), static_cast<uint16_t>(x1), static_cast<uint16_t>(y1));
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x += kRowChunk) {
                const int cx1 = x + kRowChunk - 1 < x1 ? x + kRowChunk - 1 : x1;
                compose_row(c, y, x, cx1, row);
                lcd::stream_pixels({row, static_cast<size_t>(cx1 - x + 1)});
            }
        }
        lcd::stream_end();
        return;
    }
    paint_stretched(c, x0, y0, x1, y1, row);
}

}  // namespace k2::home
