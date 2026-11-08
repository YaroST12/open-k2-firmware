// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for ListState (clamping, offset, visible range, scrollbar thumb) and the ListView
// widget (callback per visible row, redraws only the list rect, indicator only on overflow).
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/list_view.hpp"
#include <vector>
#include <algorithm>

using namespace k2;

static void test_state_basics() {
    ListState s; s.visible = 6;
    s.set_count(3);
    CHECK_EQ(s.overflows(), 0); CHECK_EQ(s.max_offset(), 0);
    CHECK_EQ(s.first(), 0); CHECK_EQ(s.end(), 3);
    CHECK_EQ(s.scroll(1), 0);                 // nothing to scroll
    s.set_count(7);
    CHECK_EQ(s.overflows(), 1); CHECK_EQ(s.max_offset(), 1);
    CHECK_EQ(s.first(), 0); CHECK_EQ(s.end(), 6);
}

static void test_clamp_no_wrap() {
    ListState s; s.visible = 6; s.set_count(10);
    CHECK_EQ(s.scroll(-1), 0); CHECK_EQ(s.offset, 0);          // top: stays
    CHECK_EQ(s.scroll(1), 1);  CHECK_EQ(s.offset, 1);
    CHECK_EQ(s.scroll(100), 1); CHECK_EQ(s.offset, 4);          // clamped to max_offset
    CHECK_EQ(s.first(), 4); CHECK_EQ(s.end(), 10);
    CHECK_EQ(s.scroll(1), 0);  CHECK_EQ(s.offset, 4);           // bottom: stays, no wrap
    CHECK_EQ(s.scroll(-100), 1); CHECK_EQ(s.offset, 0);
}

static void test_count_shrink_clamps_offset() {
    ListState s; s.visible = 6; s.set_count(10);
    s.scroll(4);
    CHECK_EQ(s.set_count(8), 1);              // offset 4 -> 2: reports the change
    CHECK_EQ(s.offset, 2);
    CHECK_EQ(s.set_count(4), 1);              // no longer overflows
    CHECK_EQ(s.offset, 0);
    CHECK_EQ(s.set_count(4), 0);
}

static void test_thumb() {
    ListState s; s.visible = 6; s.set_count(12);
    int pos, len;
    s.thumb(120, pos, len);
    CHECK_EQ(len, 60); CHECK_EQ(pos, 0);       // half the list visible: half the track
    s.scroll(6);
    s.thumb(120, pos, len);
    CHECK_EQ(pos + len, 120);                  // bottom of the list: thumb touches the track end
    s.set_count(1000); s.scroll(-1000);
    s.thumb(120, pos, len);
    CHECK_EQ(len, 6);                          // minimum thumb length
}

struct Calls { std::vector<int> idx; std::vector<int> hi; int n = 0; };

static void row_cb(void *ctx, int index, const Rect &r, bool highlighted, Field &f) {
    auto *c = static_cast<Calls *>(ctx);
    c->idx.push_back(index);
    c->hi.push_back(highlighted);
    c->n++;
    char t[8]; t[0] = static_cast<char>('A' + index); t[1] = 0;
    f.draw(t);
    (void)r;
}

static bool outside_untouched(const Rect &r, uint16_t marker) {
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
            const bool in = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
            if (!in && g_panel[y * 240 + x] != marker) return false;
        }
    return true;
}

static void test_widget() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    ListView lv;
    Calls c;
    lv.init(surf, 0, 74, 20, 24, 6, font_16, row_cb, &c);
    c = Calls{};
    lv.set_count(7);                                   // the count change paints the visible rows
    CHECK_EQ(c.n, 6);                                  // exactly the visible rows
    CHECK_EQ(c.idx.front(), 0); CHECK_EQ(c.idx.back(), 5);
    // The list owns the rect x 0..224, y 74..218; nothing else on screen changed.
    CHECK_EQ(outside_untouched({0, 74, 224, 144}, 0x1234), 1);
    // The scrollbar appeared because the list overflows (right 4 px of the rect).
    CHECK_EQ(g_panel[(74 + 2) * 240 + 222] != 0x1234 && g_panel[(74 + 2) * 240 + 222] != 0x0000, 1);

    c = Calls{};
    lv.draw();
    CHECK_EQ(c.n, 0);                                  // nothing changed: no row repainted

    c = Calls{};
    CHECK_EQ(lv.scroll(1), 1);                         // scrolling redraws the rows itself
    CHECK_EQ(c.n, 6);
    CHECK_EQ(c.idx.front(), 1); CHECK_EQ(c.idx.back(), 6);
    CHECK_EQ(outside_untouched({0, 74, 224, 144}, 0x1234), 1);

    c = Calls{};
    CHECK_EQ(lv.scroll(1), 0);                         // at the end: clamped, no redraw
    CHECK_EQ(c.n, 0);
    CHECK_EQ(lv.state().offset, 1);

    // Highlight is passed through to the callback; only the slots whose highlight changed repaint.
    c = Calls{};
    lv.set_highlight(3);
    lv.draw();
    CHECK_EQ(c.n, 1);
    CHECK_EQ(c.idx.front(), 3);
    CHECK_EQ(c.hi[0], 1);                              // the slot showing index 3
    c = Calls{};
    lv.draw();
    CHECK_EQ(c.n, 0);                                  // settled: nothing to repaint
}

static void test_no_indicator_without_overflow() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    ListView lv;
    Calls c;
    lv.init(surf, 0, 74, 20, 24, 6, font_16, row_cb, &c);
    c = Calls{};
    lv.set_count(5);
    CHECK_EQ(lv.state().overflows(), 0);
    CHECK_EQ(c.n, 5);                                  // the empty sixth slot is blanked, not asked
    // Nothing is drawn in the scrollbar column.
    int bar = 0;
    for (int y = 74; y < 218; y++) bar += g_panel[y * 240 + 222] != 0x1234 && g_panel[y * 240 + 222] != 0;
    CHECK_EQ(bar, 0);
    CHECK_EQ(lv.scroll(1), 0);
}

// An optional per-row content signature: a row whose signature changed repaints (its base fill and
// its callback) while its index and highlight stay put; unchanged rows are not asked at all.
static uint32_t g_row_sig[16];
static uint32_t row_sig_cb(void *, int index) { return g_row_sig[index]; }

static void test_row_signature() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    ListView lv;
    Calls c;
    for (auto &v : g_row_sig) v = 7;
    lv.init(surf, 0, 74, 20, 24, 6, font_16, row_cb, &c);
    lv.set_row_sig(row_sig_cb, &c);
    c = Calls{};
    lv.set_count(5);
    CHECK_EQ(c.n, 5);
    c = Calls{};
    lv.draw();
    CHECK_EQ(c.n, 0);                                  // signatures unchanged: nothing repainted
    g_row_sig[2] = 8;
    c = Calls{};
    g_lcd_pixels = 0;
    lv.draw();
    CHECK_EQ(c.n, 1); CHECK_EQ(c.idx.front(), 2);      // exactly the row whose content changed
    CHECK_EQ(g_lcd_pixels < 2u * 221 * 24, true);      // one base fill plus its text, no other row touched
    c = Calls{};
    lv.draw();
    CHECK_EQ(c.n, 0);                                  // and only once
    lv.invalidate();                                   // a forced repaint still repaints every row
    c = Calls{};
    lv.draw();
    CHECK_EQ(c.n, 5);
}

// Opaque rows with a rule: the list draws the one-pixel rule at the row top and the base below it, in the row's colours
// (focused: the tint), and writes the row's first line once instead of twice.
static void row_empty_cb(void *, int, const Rect &, bool, Field &) {}
static void test_row_rule() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    ListView lv;
    lv.init(surf, 0, 74, 20, 24, 6, font_16, row_empty_cb, nullptr);
    lv.set_row_bg(0x0001, 0x0002, 0x0003);
    lv.set_highlight(1);
    g_lcd_pixels = 0;
    lv.set_count(5);
    CHECK_EQ(g_panel[74 * 240 + 10], 0x0003);            // row 0: rule
    CHECK_EQ(g_panel[75 * 240 + 10], 0x0001);            // row 0: base below it
    CHECK_EQ(g_panel[(74 + 24) * 240 + 10], 0x0003);     // row 1 (focused): rule over the tint
    CHECK_EQ(g_panel[(75 + 24) * 240 + 10], 0x0002);
    const unsigned with_rule = g_lcd_pixels;
    ListView plain;
    plain.init(surf, 0, 74, 20, 24, 6, font_16, row_empty_cb, nullptr);
    plain.set_row_bg(0x0001, 0x0002);
    plain.set_highlight(1);
    g_lcd_pixels = 0;
    plain.set_count(5);
    CHECK_EQ(with_rule, g_lcd_pixels);                   // the rule replaces the row's first base line: no extra pixels
    lv.clear_row_bg();                                   // back to the plain look: no rule
    lv.invalidate();
    lv.draw();
    CHECK_EQ(g_panel[74 * 240 + 10], 0x0000);
}

// Expanding focus: the highlighted row is `extra` px taller and the rows below shift; a focus move repaints exactly
// the rows whose position or highlight changed; the rect stays rows * row_h + extra tall.
struct Spans { std::vector<std::pair<int, int>> v; };   // (y, h) per callback
static void row_span_cb(void *ctx, int, const Rect &r, bool, Field &) {
    static_cast<Spans *>(ctx)->v.push_back({r.y, r.h});
}
static void test_focus_extra() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    Spans sp;
    ListView lv;
    lv.init(surf, 0, 24, 20, 26, 7, font_16, row_span_cb, &sp);
    lv.set_row_bg(0x0001, 0x0002, 0x0003);
    lv.set_focus_extra(14);
    lv.set_full_lane(true);
    lv.set_highlight(2);
    lv.set_count(5, false);
    lv.draw();
    CHECK_EQ(sp.v.size(), 5u);
    CHECK_EQ(sp.v[2].first, 24 + 2 * 26);
    CHECK_EQ(sp.v[2].second, 26 + 14);                    // the focused row is 40 px
    CHECK_EQ(sp.v[3].first, 24 + 3 * 26 + 14);            // rows below it shift down
    CHECK_EQ(g_panel[(24 + 2 * 26 + 35) * 240 + 10], 0x0002);   // the focus tint fills the expansion
    CHECK_EQ(g_panel[(24 + 3 * 26 + 14) * 240 + 10], 0x0003);   // next row's rule right under it
    CHECK_EQ(g_panel[(24 + 2 * 26 + 35) * 240 + 223], 0x0002);  // no overflow: the lane carries the row base
    // Focus one down: row 2 shrinks and row 3 grows and moves up 14; row 4 sits at +14 either way, so it stays untouched.
    sp.v.clear();
    lv.set_highlight(3);
    lv.draw();
    CHECK_EQ(sp.v.size(), 2u);
    CHECK_EQ(sp.v[0].first, 24 + 2 * 26);
    CHECK_EQ(sp.v[0].second, 26);
    CHECK_EQ(sp.v[1].first, 24 + 3 * 26);
    CHECK_EQ(sp.v[1].second, 26 + 14);
    // A settled list repaints nothing.
    sp.v.clear();
    g_lcd_pixels = 0;
    lv.draw();
    CHECK_EQ(sp.v.size(), 0u);
    CHECK_EQ(g_lcd_pixels, 0u);
}

// A highlight outside the window leaves the expansion strip unclaimed: wiped once, not repeatedly.
static void test_focus_extra_tail() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    Spans sp;
    ListView lv;
    lv.init(surf, 0, 24, 20, 26, 3, font_16, row_span_cb, &sp);
    lv.set_row_bg(0x0001, 0x0002);
    lv.set_focus_extra(14);
    lv.set_highlight(-1);
    lv.set_count(3, false);
    lv.draw();
    CHECK_EQ(g_panel[(24 + 3 * 26 + 5) * 240 + 10], 0x0000);   // the 14 px under the last row were wiped
    g_lcd_pixels = 0;
    lv.draw();
    CHECK_EQ(g_lcd_pixels, 0u);
}

// Overflow with the full lane: the thumb keeps the lane, rows stop 3 px short; leaving overflow returns the lane to the rows.
static void test_full_lane_overflow() {
    lcd::fill(0x1234);
    const Surface surf{0, 0, 224, 240};
    Spans sp;
    ListView lv;
    lv.init(surf, 0, 24, 20, 26, 3, font_16, row_span_cb, &sp);
    lv.set_row_bg(0x0001, 0x0002);
    lv.set_full_lane(true);
    lv.set_highlight(0);
    lv.set_count(6, false);
    lv.draw();
    CHECK_EQ(g_panel[(24 + 10) * 240 + 222] != 0x0002, 1);     // the thumb / track, not the row base
    lv.set_count(2);
    CHECK_EQ(g_panel[(24 + 10) * 240 + 222], 0x0002);          // the focused row base runs under the lane now
    CHECK_EQ(g_panel[(24 + 26 + 10) * 240 + 222], 0x0001);
}

int main() {
    test_focus_extra();
    test_focus_extra_tail();
    test_full_lane_overflow();
    test_row_rule();
    test_row_signature();
    test_state_basics();
    test_clamp_no_wrap();
    test_count_shrink_clamps_offset();
    test_thumb();
    test_widget();
    test_no_indicator_without_overflow();
    return test_report("test_list_view");
}
