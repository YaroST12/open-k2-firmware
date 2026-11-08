// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 compositor: masks, sweep columns, the pixel under an item, text blending against the sweep,
// the wrapped repaint strip and the digit field's changed-character range.
#include <string>
#include "fake_lcd.h"
#include "test_util.h"
#include "k2/ui/blend.hpp"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/home_cell.hpp"
#include "k2/ui/text.hpp"

using namespace k2;
using namespace k2::home;

static constexpr uint16_t kBgC = 0x0000, kTint = 0x0123, kLine = 0x07E0, kCur = 0x4208, kInk = 0xFFFF;

static void test_arrow_masks() {
    // Up: a two pixel tip on the top row, the shaft only on the bottom row.
    CHECK_EQ(arrow_hit(0, 10, 0), 1);
    CHECK_EQ(arrow_hit(0, 11, 0), 1);
    CHECK_EQ(arrow_hit(0, 9, 0), 0);
    CHECK_EQ(arrow_hit(0, 8, 21), 1);
    CHECK_EQ(arrow_hit(0, 13, 21), 1);
    CHECK_EQ(arrow_hit(0, 7, 21), 0);
    CHECK_EQ(arrow_hit(0, 3, 9), 1);                  // head spans 16 px at its base
    CHECK_EQ(arrow_hit(0, 2, 9), 0);
    // The other three directions are quarter turns of it.
    CHECK_EQ(arrow_hit(1, 0, 10), 1);                 // left: tip on the left column
    CHECK_EQ(arrow_hit(1, 21, 8), 1);                 // shaft end on the right column
    CHECK_EQ(arrow_hit(2, 10, 21), 1);                // down: tip on the bottom row
    CHECK_EQ(arrow_hit(2, 8, 0), 1);
    CHECK_EQ(arrow_hit(3, 21, 10), 1);                // right: tip on the right column
    CHECK_EQ(arrow_hit(3, 0, 8), 1);
    CHECK_EQ(arrow_hit(0, -1, 0), 0);
    CHECK_EQ(arrow_hit(0, 22, 0), 0);
    // Dashes lie along the port axis.
    CHECK_EQ(arrow_hit(kDashV, 10, 6), 1);
    CHECK_EQ(arrow_hit(kDashV, 5, 10), 0);
    CHECK_EQ(arrow_hit(kDashH, 6, 10), 1);
    CHECK_EQ(arrow_hit(kDashH, 10, 5), 0);
}

static void test_arrow_follows_rotation() {
    CHECK_EQ(arrow_dir(true, 0), 0);                  // forward at rotation 0 points up
    CHECK_EQ(arrow_dir(false, 0), 2);
    CHECK_EQ(arrow_dir(true, 1), 1);
    CHECK_EQ(arrow_dir(true, 2), 2);
    CHECK_EQ(arrow_dir(false, 3), 1);
    CHECK_EQ(dash_dir(0), kDashV);
    CHECK_EQ(dash_dir(1), kDashH);
    CHECK_EQ(dash_dir(2), kDashV);
    CHECK_EQ(dash_dir(3), kDashH);
}

static void test_warn_mask() {
    CHECK_EQ(warn_hit(5, 0), 1);                      // apex
    CHECK_EQ(warn_hit(4, 0), 0);
    CHECK_EQ(warn_hit(0, 10), 1);                     // base corners
    CHECK_EQ(warn_hit(10, 10), 1);
    CHECK_EQ(warn_hit(0, 5), 0);
    CHECK_EQ(warn_hit(5, 5), 2);                      // the exclamation stem
    CHECK_EQ(warn_hit(5, 7), 1);                      // gap between stem and dot
    CHECK_EQ(warn_hit(5, 8), 2);                      // the dot
    CHECK_EQ(warn_hit(11, 10), 0);
}

static void test_sweep_cursor_and_load() {
    Sweep s;
    CHECK_EQ(s.step(7), 0);
    CHECK_EQ(s.rows[0], 7);
    CHECK_EQ(s.cursor, 1);
    for (int i = 1; i < kCols; i++) s.step(static_cast<uint8_t>(i));
    CHECK_EQ(s.cursor, 0);                            // wrapped
    CHECK_EQ(s.step(9), 0);
    CHECK_EQ(s.rows[0], 9);

    const uint8_t lv[3] = {4, 5, 6};
    s.load(lv);
    CHECK_EQ(s.rows[0], 4);
    CHECK_EQ(s.rows[2], 6);
    CHECK_EQ(s.rows[3], 0);
    CHECK_EQ(s.cursor, 3);
    uint8_t many[kCols];
    for (auto &m : many) m = 1;
    s.load(many);
    CHECK_EQ(s.cursor, kCols - kGapCols);             // the gap stays free
}

static void test_strip_wraps() {
    Strip a = strip_of(10);
    CHECK_EQ(a.n, 1);
    CHECK_EQ(a.r[0].x0, 24);
    CHECK_EQ(a.r[0].x1, 24 + 4 * 2 + 1);              // columns 10..14
    Strip b = strip_of(kCols - 2);                    // columns 108, 109, then 0, 1, 2
    CHECK_EQ(b.n, 2);
    CHECK_EQ(b.r[0].x0, x_of_col(108));
    CHECK_EQ(b.r[0].x1, 223);
    CHECK_EQ(b.r[1].x0, 4);
    CHECK_EQ(b.r[1].x1, x_of_col(2) + 1);
    CHECK_EQ(strip_of(kCols - 4).n, 2);               // 106..109 then column 0: the first to wrap
    CHECK_EQ(strip_of(kCols - 5).n, 1);               // 105..109 still fits
    Strip c = strip_of(10, 3);                        // three steps in a row: columns 10..16
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.r[0].x0, x_of_col(10));
    CHECK_EQ(c.r[0].x1, x_of_col(16) + 1);
    CHECK_EQ(strip_of(kCols - 5, 2).n, 2);            // 105..109 plus column 0 after two steps
}

static Cell sweep_cell(Sweep &sw) {
    Cell c;
    c.geom = kVoltCell;
    c.bg = kBgC;
    c.sweep = true;
    c.tint = kTint;
    c.line = kLine;
    c.cursor_line = kCur;
    c.sw = &sw;
    return c;
}

static void test_bg_at_sweep_columns() {
    Sweep sw;
    sw.cursor = 20;
    sw.rows[10] = 5;
    sw.rows[21] = 9;                                  // inside the gap: stays blank
    sw.rows[30] = 3;
    const Cell c = sweep_cell(sw);
    const int x = x_of_col(10);
    CHECK_EQ(bg_at(c, x, 69), kTint);                 // bottom sweep row: level 1 of 5
    CHECK_EQ(bg_at(c, x + 1, 66), kTint);             // both pixels of the 2 px column
    CHECK_EQ(bg_at(c, x, 65), kLine);                 // k == rows: the 1 px top line
    CHECK_EQ(bg_at(c, x, 64), kBgC);
    CHECK_EQ(bg_at(c, x, 1), kBgC);                   // above the sweep rows
    CHECK_EQ(bg_at(c, 3, 69), kBgC);                  // left of the first column
    CHECK_EQ(bg_at(c, x_of_col(9), 69), kBgC);        // no sample
    CHECK_EQ(bg_at(c, x_of_col(20), 40), kCur);       // left pixel of the cursor column
    CHECK_EQ(bg_at(c, x_of_col(20) + 1, 40), kBgC);
    CHECK_EQ(bg_at(c, x_of_col(20), 1), kBgC);        // the cursor line stays inside the sweep rows
    CHECK_EQ(bg_at(c, x_of_col(21), 69), kBgC);       // gap columns are blank
    CHECK_EQ(bg_at(c, x_of_col(23), 69), kBgC);
    CHECK_EQ(bg_at(c, x_of_col(30), 69), kTint);      // after the gap the data is back
    // Wrap: cursor at 108 keeps columns 109, 0, 1 blank.
    sw.cursor = 108;
    sw.rows[0] = 9;
    CHECK_EQ(bg_at(c, x_of_col(0), 69), kBgC);
    CHECK_EQ(bg_at(c, x_of_col(1), 69), kBgC);
    sw.rows[2] = 4;
    CHECK_EQ(bg_at(c, x_of_col(2), 69), kTint);       // first column after the wrapped gap
    // An inverted cell draws no sweep at all.
    Cell inv = c;
    inv.sweep = false;
    inv.bg = 0xF800;
    CHECK_EQ(bg_at(inv, x, 69), 0xF800);
}

static uint16_t expect_blend(uint16_t fg, uint16_t bg, unsigned cov) {
    return cov == 0 ? bg : cov >= 15 ? fg : blend565(fg, bg, cov);
}

static void test_text_blends_against_the_sweep() {
    Sweep sw;
    sw.cursor = 100;
    for (int i = 0; i < 90; i++) sw.rows[i] = static_cast<uint8_t>(10 + i % 30);
    const Cell flat = [] { Cell c; c.geom = kVoltCell; c.bg = kBgC; return c; }();
    Cell swept = sweep_cell(sw);

    const Font &f = font_24;
    const std::string_view s = "5.1";
    const Item items[1] = {text(f, 30, 20, s, kInk)};
    swept.items = items;
    Cell plain = flat;
    plain.items = items;

    uint16_t kCov[16];
    for (unsigned a = 0; a < 16; a++) kCov[a] = static_cast<uint16_t>(a);
    uint16_t full_s[kW], full_p[kW], part[kW];
    int partial_pixels = 0;
    for (int y = 18; y < 20 + f.height + 1; y++) {
        compose_row(swept, y, 0, kW - 1, full_s);
        compose_row(plain, y, 0, kW - 1, full_p);
        uint16_t cov[kW] = {};
        const int ty = y - 20;
        if (ty >= 0 && ty < f.height) render_row(f, s, ty, kCov, cov + 30, static_cast<int>(s.size()) * f.adv);
        for (int x = 0; x < kW; x++) {
            CHECK_EQ(full_p[x], expect_blend(kInk, kBgC, cov[x]));
            CHECK_EQ(full_s[x], expect_blend(kInk, bg_at(swept, x, y), cov[x]));
            if (cov[x] > 0 && cov[x] < 15) partial_pixels++;
        }
        // A clipped range returns the same pixels as the full row.
        compose_row(swept, y, 33, 61, part);
        for (int x = 33; x <= 61; x++) CHECK_EQ(part[x - 33], full_s[x]);
    }
    CHECK_EQ(partial_pixels > 20, 1);                 // the test saw anti-aliased edge pixels
}

static void test_items_layer_in_order() {
    Sweep sw;
    const Item items[3] = {
        fill(10, 5, 20, 3, 0x1111),
        fill(15, 6, 4, 1, 0x2222),                    // later item wins
        text(font_16, 12, 5, " ", kInk),              // a space paints nothing
    };
    Cell c = sweep_cell(sw);
    c.sweep = false;
    c.items = items;
    uint16_t row[kW];
    compose_row(c, 6, 0, kW - 1, row);
    CHECK_EQ(row[9], kBgC);
    CHECK_EQ(row[10], 0x1111);
    CHECK_EQ(row[15], 0x2222);
    CHECK_EQ(row[18], 0x2222);
    CHECK_EQ(row[19], 0x1111);
    CHECK_EQ(row[29], 0x1111);
    CHECK_EQ(row[30], kBgC);
    compose_row(c, 8, 0, kW - 1, row);
    CHECK_EQ(row[12], kBgC);                          // below the fill
    // Clipping a fill at the edges of the requested range.
    uint16_t cut[8];
    compose_row(c, 5, 8, 15, cut);
    CHECK_EQ(cut[0], kBgC);
    CHECK_EQ(cut[2], 0x1111);
    CHECK_EQ(cut[7], 0x1111);
}

static void test_arrow_and_warn_items() {
    const Item items[2] = {arrow(50, 100, 0, 0x1357), warn(10, 74, 0x2468, 0x0001)};
    Cell c;
    c.geom = kAmpCell;
    c.bg = kBgC;
    c.items = items;
    uint16_t row[kW];
    compose_row(c, 100, 0, kW - 1, row);              // the arrow's top row
    CHECK_EQ(row[50 + 10], 0x1357);
    CHECK_EQ(row[50 + 9], kBgC);
    compose_row(c, 74 + 5, 0, kW - 1, row);           // the warning's stem row
    CHECK_EQ(row[10 + 5], 0x0001);
    CHECK_EQ(row[10 + 3], 0x2468);
}

static void test_paint_streams_the_composed_rows() {
    Sweep sw;
    sw.cursor = 50;
    for (int i = 0; i < 40; i++) sw.rows[i] = static_cast<uint8_t>(5 + i);
    const Item items[1] = {text(font_16, 8, 6, "VBUS", kInk)};
    Cell c = sweep_cell(sw);
    c.items = items;
    for (auto &p : g_panel) p = 0xAAAA;
    const unsigned before = g_lcd_windows;
    paint(c, 4, 2, 60, 30);
    CHECK_EQ(g_lcd_windows - before, 1);
    uint16_t row[kW];
    for (int y = 2; y <= 30; y++) {
        compose_row(c, y, 4, 60, row);
        for (int x = 4; x <= 60; x++) CHECK_EQ(g_panel[y * LCD_WIDTH + x], row[x - 4]);
    }
    CHECK_EQ(g_panel[1 * LCD_WIDTH + 10], 0xAAAA);    // nothing outside the rectangle
    CHECK_EQ(g_panel[2 * LCD_WIDTH + 61], 0xAAAA);
    // The rectangle is clipped to the cell rows.
    for (auto &p : g_panel) p = 0xAAAA;
    paint(c, 0, 60, 223, 100);
    CHECK_EQ(g_panel[69 * LCD_WIDTH + 100] != 0xAAAA, 1);
    CHECK_EQ(g_panel[70 * LCD_WIDTH + 100], 0xAAAA);
}

static void test_digit_field_changed_range() {
    DigitField f;
    f.init(font_64, 38, 24, 5);
    int16_t a = 0, b = 0;
    CHECK_EQ(f.set("5.123", a, b), 1);                // first draw: the whole field
    CHECK_EQ(a, 38);
    CHECK_EQ(b, 38 + 5 * 32 - 1);
    CHECK_EQ(f.set("5.123", a, b), 0);                // nothing changed
    CHECK_EQ(f.set("5.124", a, b), 1);                // the last digit
    CHECK_EQ(a, 38 + 4 * 32);
    CHECK_EQ(b, 38 + 5 * 32 - 1);
    CHECK_EQ(f.set("5.224", a, b), 1);                // an inner digit only
    CHECK_EQ(a, 38 + 2 * 32);
    CHECK_EQ(b, 38 + 3 * 32 - 1);
    CHECK_EQ(f.set("6.230", a, b), 1);                // first and last change: one span from first to last
    CHECK_EQ(a, 38);
    CHECK_EQ(b, 38 + 5 * 32 - 1);
    CHECK_EQ(f.set("9.9", a, b), 1);                  // right aligned and padded
    CHECK_STR(std::string(f.view()).c_str(), "  9.9");
    f.invalidate();
    CHECK_EQ(f.set("  9.9", a, b), 1);
    CHECK_EQ(a, 38);
    CHECK_EQ(f.right(), 38 + 5 * 32 - 1);
}

int main() {
    test_arrow_masks();
    test_arrow_follows_rotation();
    test_warn_mask();
    test_sweep_cursor_and_load();
    test_strip_wraps();
    test_bg_at_sweep_columns();
    test_text_blends_against_the_sweep();
    test_items_layer_in_order();
    test_arrow_and_warn_items();
    test_paint_streams_the_composed_rows();
    test_digit_field_changed_range();
    return test_report("test_home_cell");
}
