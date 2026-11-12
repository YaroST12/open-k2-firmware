// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the UI component kit (include/k2/ui/kit.hpp): the pure style tables, the progress rule, RowList (geometry,
// the opening focus row, caches, thumb, lane), VerdictCell, AlertBand, ActionCell, ProgressBar, KvTable and InfoBlock.
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/kit.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_viewport.hpp"
#include <algorithm>
#include <string>
#include <vector>

using namespace k2;

namespace {
constexpr uint16_t kBase = 0x1234;   // the unpainted panel colour: anything the kit writes differs from it
const Surface kSurf{0, 0, 224, 240};

std::vector<uint16_t> snap() { return std::vector<uint16_t>(g_panel, g_panel + 240 * 240); }
int changed_in(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b, int ya, int yb) {
    int n = 0;
    for (int y = ya; y < yb; y++)
        for (int x = 0; x < 240; x++) n += a[y * 240 + x] != b[y * 240 + x];
    return n;
}
uint16_t px(int x, int y) { return g_panel[y * 240 + x]; }
int ink_in(int xa, int xb, int ya, int yb, uint16_t bg) {   // pixels differing from `bg` in the box
    int n = 0;
    for (int y = ya; y < yb; y++)
        for (int x = xa; x < xb; x++) n += px(x, y) != bg;
    return n;
}

struct Model {
    std::string text[12], tail[12], note[12];
    int n = 0;
    kit::Marker marker = kit::Marker::Radio;
    int on = -1;      // the picked row (radio / check)
    bool notes = true;
};
void fill_model(Model &m, int n) {
    m.n = n;
    for (int i = 0; i < n; i++) {
        m.text[i] = "Row " + std::to_string(i);
        m.tail[i] = std::to_string(i * 3);
        m.note[i] = m.notes ? "note " + std::to_string(i) : "";
    }
}
void row_fn(void *ctx, int i, bool focused, kit::RowSpec &out) {
    auto *m = static_cast<Model *>(ctx);
    out.marker = m->marker;
    out.state = static_cast<uint8_t>(i == m->on);
    out.text = m->text[i];
    out.tail = m->tail[i];
    out.note = focused ? std::string_view(m->note[i]) : std::string_view();
}
}  // namespace

static void test_tables() {
    using kit::Verdict;
    CHECK_EQ(kit::kWindowRows, 7);
    CHECK_EQ(kit::kBodyY + kit::kWindowRows * kit::kRowH + kit::kFocusExtra <= kFooterY, 1);
    // pass: green tint + green ink; fail: solid red + black ink; running: the pending look (no fill, faint ink) + a kFocus dot
    CHECK_EQ(kit::verdict_style(Verdict::Pass).fill, kOkT);
    CHECK_EQ(kit::verdict_style(Verdict::Pass).ink, kOk);
    CHECK_EQ(kit::verdict_style(Verdict::Fail).fill, kError);
    CHECK_EQ(kit::verdict_style(Verdict::Fail).ink, kBg);
    CHECK_EQ(kit::verdict_style(Verdict::Running).dotted, 1);
    CHECK_EQ(kit::verdict_style(Verdict::Running).dot, kFocus);
    CHECK_EQ(kit::verdict_style(Verdict::Running).filled, 0);
    CHECK_EQ(kit::verdict_style(Verdict::Running).ink, kit::verdict_style(Verdict::Pending).ink);
    for (Verdict o : {Verdict::Pass, Verdict::Fail, Verdict::Partial, Verdict::Skip, Verdict::None, Verdict::Pending, Verdict::Stopped})
        CHECK_EQ(kit::verdict_style(o).dotted, 0);       // only the cell under test has the dot: the shape cue is its own
    // partial: amber tint + amber ink (never the red of a fail, never the green of a pass)
    CHECK_EQ(kit::verdict_style(Verdict::Partial).fill, kCautionT);
    CHECK_EQ(kit::verdict_style(Verdict::Partial).ink, kCaution);
    CHECK_EQ(kit::verdict_style(Verdict::Partial).filled, 1);
    CHECK_EQ(kit::verdict_style(Verdict::Partial).dotted, 0);
    // none, skip and stopped never get a fill and never go red; pending is fainter than they are
    for (Verdict v : {Verdict::None, Verdict::Skip, Verdict::Stopped}) {
        CHECK_EQ(kit::verdict_style(v).filled, 0);
        CHECK_EQ(kit::verdict_style(v).ink, kDim);
    }
    CHECK_EQ(kit::verdict_style(Verdict::Pending).ink, kFaint);
    CHECK_EQ(kit::verdict_style(Verdict::Pending).filled, 0);
    CHECK_EQ(kit::row_text_y(26, 16), 6);
}

static void test_progress_rule() {
    lcd::fill(kBase);
    kit::draw_progress_rule(kSurf, 2);
    for (int row : {20, 21}) {
        CHECK_EQ(px(0, row), kFocus);
        CHECK_EQ(px(73, row), kFocus);
        CHECK_EQ(px(75, row), kFocus);
        CHECK_EQ(px(148, row), kFocus);
        CHECK_EQ(px(150, row), kRule);
        CHECK_EQ(px(223, row), kRule);
        CHECK_EQ(px(74, row), kBase);   // the gap between segments is not touched
    }
    CHECK_EQ(px(100, 19), kBase);
    CHECK_EQ(px(100, 22), kBase);
    lcd::fill(kBase);
    kit::draw_progress_rule(kSurf, 3);
    CHECK_EQ(px(223, 20), kFocus);
    lcd::fill(kBase);
    kit::draw_top_bar(kSurf, "Tests", 1, "PAS");
    CHECK_EQ(px(10, 21), kFocus);
    CHECK_EQ(px(100, 21), kRule);
    CHECK_EQ(px(2, 2), kBar);
    // A two-step wizard: two segments over the same 224 px, 0..110 and 112..223, the gap at 111.
    lcd::fill(kBase);
    kit::draw_progress_rule(kSurf, 1, 2);
    for (int row : {20, 21}) {
        CHECK_EQ(px(0, row), kFocus);
        CHECK_EQ(px(110, row), kFocus);
        CHECK_EQ(px(111, row), kBase);
        CHECK_EQ(px(112, row), kRule);
        CHECK_EQ(px(223, row), kRule);
    }
    lcd::fill(kBase);
    kit::draw_top_bar(kSurf, "Review", 2, "PAS", false, 2);
    CHECK_EQ(px(10, 21), kFocus);
    CHECK_EQ(px(200, 21), kFocus);
    CHECK_EQ(px(111, 21), kBase);
}

static void test_row_list_geometry() {
    Model m; fill_model(m, 5);
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(2); l.draw(1);
    CHECK_EQ(l.visible(), 7);
    const int y2 = 24 + 2 * 26;                        // row 2 top
    CHECK_EQ(px(1, y2 + 3), kFocus);                    // the cyan bar at x0..2
    CHECK_EQ(px(2, y2 + 39), kFocus);                   // runs the whole 40 px (hairline excluded)
    CHECK_EQ(px(150, y2 + 39), kFocusT);                // the open row is one tinted block
    CHECK_EQ(px(150, y2 + 1), kFocusT);
    CHECK_EQ(px(150, y2), kBg);                         // Fieldbook: no hairlines, the row's top line is the ground
    CHECK_EQ(px(100, y2 + 40), kBg);                    // row 3 starts 40 px below row 2: the rows below shifted by 14
    CHECK_EQ(px(1, y2 + 40 + 3) != kFocus, 1);
    CHECK_EQ(px(100, 24 + 26), kBg);                    // rows above keep the 26 px pitch
    CHECK_EQ(ink_in(28, 120, y2 + 24, y2 + 36, kFocusT) > 0, 1);   // the 12 px note
    CHECK_EQ(ink_in(28, 120, 24 + 26 + 14, 24 + 26 + 26, kBg) > 0, 1);   // an unfocused row has its text, no note below it
    // Past the last row the rest of the list rect is plain kBg, the footer band below is untouched.
    CHECK_EQ(px(100, 24 + 5 * 26 + 14 + 10), kBg);
    CHECK_EQ(px(100, kFooterY), kBase);
}

// A short list (5 rows in a 7 row window): the open row 4 runs over the first empty slot. Reached by walking down or bound
// there directly, it is one solid block with its full-height bar, and the empty slots below it stay plain background.
static void test_last_row_opens_over_empty_slot() {
    for (int walk = 0; walk < 2; walk++) {
        Model m; fill_model(m, 5);
        lcd::fill(kBase);
        kit::RowList l;
        l.init(kSurf, kit::kBodyY, row_fn, &m);
        l.set_count(m.n);
        if (walk) for (int s = 0; s < 5; s++) {
            l.select(s); l.draw(1 + s);
            const int ys = 24 + s * 26;
            for (int dy : {1, 20, 39}) CHECK_EQ(px(1, ys + dy), kFocus);   // every step: the open row's bar is whole
        }
        else { l.select(4); l.draw(1); }
        const int y4 = 24 + 4 * 26;
        for (int dy : {1, 20, 39}) {
            CHECK_EQ(px(1, y4 + dy), kFocus);
            CHECK_EQ(px(5, y4 + dy), kFocusT);
            CHECK_EQ(px(150, y4 + dy), kFocusT);
        }
        CHECK_EQ(px(100, y4 + 40 + 10), kBg);
    }
}

static void test_row_without_note_stays_26() {
    Model m; m.notes = false; fill_model(m, 5);
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(2); l.draw(1);
    const int y2 = 24 + 2 * 26;
    CHECK_EQ(px(1, y2 + 3), kFocus);
    CHECK_EQ(px(100, y2 + 26), kBg);                    // the next row follows at the plain pitch
    CHECK_EQ(px(1, y2 + 25), kFocus);
    CHECK_EQ(px(1, y2 + 26 + 3) != kFocus, 1);
    // Moving the focus from a noteless to a noted row and back leaves no stale pixels in the 14 px strip.
    for (auto &n : m.note) n.clear();
    m.note[3] = "note 3";
    l.select(3); l.draw(2);
    l.select(1); l.draw(3);
    CHECK_EQ(px(100, 24 + 5 * 26 + 10), kBg);
    CHECK_EQ(px(100, 24 + 7 * 26), kBg);   // below the last row of 5: nothing of the open row remains
}

static void test_row_list_caches() {
    Model m; fill_model(m, 5);
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(1); l.draw(1);
    const unsigned pxn = g_lcd_pixels, win = g_lcd_windows;
    for (int i = 0; i < 5; i++) l.draw(10 + i);
    CHECK_EQ(g_lcd_pixels, pxn);                        // idle: nothing
    CHECK_EQ(g_lcd_windows, win);
    auto a = snap();
    l.select(2); l.draw(20);                            // a focus move: rows 1..4 change (the 14 px shift), row 0 does not
    auto b = snap();
    CHECK_EQ(changed_in(a, b, 24, 24 + 26), 0);
    CHECK_EQ(changed_in(a, b, 24 + 26, 24 + 5 * 26 + 14) > 0, 1);
    CHECK_EQ(changed_in(a, b, 24 + 5 * 26 + 14, 240), 0);
    a = snap();
    m.tail[0] = "99";                                   // a value change repaints its own row only
    l.draw(30);
    b = snap();
    CHECK_EQ(changed_in(a, b, 24 + 26, 240), 0);
    CHECK_EQ(changed_in(a, b, 24, 24 + 26) > 0, 1);
    // invalidate reaches the cache: the next draw repaints the same pixels
    a = snap();
    l.invalidate(); l.draw(40);
    b = snap();
    CHECK_EQ(changed_in(a, b, 0, 240), 0);
    CHECK_EQ(g_lcd_pixels > pxn, 1);
}

static void test_markers() {
    Model m; fill_model(m, 4); m.on = 1;
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(0); l.draw(1);
    const int cy1 = 24 + 40 + 7 + 6;                    // row 1 marker centre (row 0 is the open 40 px row)
    CHECK_EQ(px(14, cy1), kFocus);                      // the picked radio's dot
    CHECK_EQ(px(14, 24 + 40 + 26 + 7 + 6), kBg);        // row 2 (off): hollow, the centre is the row base
    CHECK_EQ(px(8, 24 + 40 + 26 + 7 + 6) != kBg, 1);    // ... with a ring (dim)
    // radio: a square. On is a solid block, off a hollow frame (a shape cue, not only colour). Every pixel of the 12 px square is
    // rewritten on each state, so painting over the other state, or over junk, leaves exactly the fresh drawing.
    for (int pass = 0; pass < 2; pass++) {
        for (int st : {1, 0, 1}) {
            lcd::fill(pass ? kError : kBase);   // junk under the square
            kit::RowSpec r; r.marker = kit::Marker::Radio; r.state = static_cast<uint8_t>(st);
            kit::draw_marker(kSurf, 8, 100, r, kBg);
            int solid = 0, frame = 0, inner = 0;
            for (int yy = 0; yy < kit::kMarkSize; yy++)
                for (int xx = 0; xx < kit::kMarkSize; xx++) {
                    const uint16_t p = px(8 + xx, 100 + yy);
                    const bool edge = xx == 0 || yy == 0 || xx == kit::kMarkSize - 1 || yy == kit::kMarkSize - 1;
                    if (p == (st ? kFocus : kDim)) (edge ? frame : solid)++;
                    else if (!edge && p == kBg) inner++;
                }
            CHECK_EQ(frame, 4 * kit::kMarkSize - 4);                                            // the frame is whole
            CHECK_EQ(solid, st ? (kit::kMarkSize - 2) * (kit::kMarkSize - 2) : 0);             // on: solid
            CHECK_EQ(inner, st ? 0 : (kit::kMarkSize - 2) * (kit::kMarkSize - 2));             // off: hollow
        }
    }
    // three-state box: part = a bar, on = a square, off = outline only
    for (int st : {0, 1, 2}) {
        lcd::fill(kBase);
        kit::RowSpec r; r.marker = kit::Marker::Box3; r.state = static_cast<uint8_t>(st);
        kit::draw_marker(kSurf, 8, 100, r, kBg);
        CHECK_EQ(px(8, 100), st ? kFocus : kDim);       // the outline
        CHECK_EQ(px(11, 105) != kBg, st >= 1);          // the centre row carries ink for part and on
        CHECK_EQ(px(11, 103) != kBg, st == 2);          // the top of the inner square only for on
    }
    lcd::fill(kBase);
    kit::RowSpec c; c.marker = kit::Marker::Check; c.state = 1;
    kit::draw_marker(kSurf, 8, 100, c, kBg);
    CHECK_EQ(ink_in(10, 18, 102, 110, kBg) > 0, 1);
    lcd::fill(kBase);
    kit::RowSpec v; v.marker = kit::Marker::Verdict; v.state = static_cast<uint8_t>(kit::Verdict::Fail);
    kit::draw_marker(kSurf, 8, 100, v, kBg);
    CHECK_EQ(px(8, 100), kError);
}

static void test_tail_and_chevron() {
    Model m; fill_model(m, 3);
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(0); l.draw(1);
    const int y1 = 24 + 40;                             // row 1 (below the open row)
    CHECK_EQ(ink_in(215, 221, y1 + 2, y1 + 24, kBg), 0);   // the value ends at x214
    CHECK_EQ(ink_in(198, 215, y1 + 2, y1 + 24, kBg) > 0, 1);
    lcd::fill(kBase);
    kit::RowSpec r; r.chevron = true;
    kit::draw_drill(kSurf, 214, 100, kDim);
    CHECK_EQ(ink_in(208, 215, 95, 105, kBase) > 0, 1);
    CHECK_EQ(ink_in(215, 224, 95, 105, kBase), 0);
}

static void test_thumb_and_lane() {
    Model m; fill_model(m, 12);
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, row_fn, &m);
    l.set_count(m.n); l.select(0); l.draw(1);
    CHECK_EQ(l.state().overflows(), 1);
    CHECK_EQ(px(222, 100) != kBase, 1);                 // track / thumb in the lane
    for (int i = 1; i < 12; i++) { l.select(i); l.draw(10 + 200 * i); }
    l.draw(5000);
    CHECK_EQ(l.state().offset, 5);                      // 12 rows, window of 7
    CHECK_EQ(px(100, kFooterY), kBase);                 // the footer band is never touched
    CHECK_EQ(px(222, kFooterY), kBase);
    CHECK_EQ(px(222, 24 + 7 * 26 + 14 - 1) != kBase, 1);   // the track ends where the rect ends, above the footer
    // A list that fits runs its row base under the lane: no notch next to the focused row.
    Model s; fill_model(s, 3);
    lcd::fill(kBase);
    kit::RowList f;
    f.init(kSurf, kit::kBodyY, row_fn, &s);
    f.set_count(s.n); f.select(1); f.draw(1);
    CHECK_EQ(px(222, 24 + 26 + 20), kFocusT);
    CHECK_EQ(px(222, 24 + 5), kBg);
}

static void test_verdict_cell() {
    using kit::Verdict;
    lcd::fill(kBase);
    kit::draw_verdict(kSurf, 10, 100, 30, 22, Verdict::Pass, "5V", kBg);
    CHECK_EQ(px(11, 101), kOkT);
    kit::draw_verdict(kSurf, 50, 100, 30, 22, Verdict::Fail, "5V", kBg);
    CHECK_EQ(px(51, 101), kError);
    kit::draw_verdict(kSurf, 90, 100, 30, 22, Verdict::None, "", kBg);
    CHECK_EQ(px(91, 101), kBg);                         // no fill: the caller's base
    // Running: the pending cell (no fill, no outline) plus a 5 px kFocus dot in the top-right corner, clear of the label
    {
        const kit::CellRect cell{130, 100, 30, 22};
        const kit::CellRect d = kit::run_dot_box(cell);
        CHECK_EQ(d.w, kit::kRunDotSize);
        CHECK_EQ(d.x + d.w, 130 + 30 - kit::kRunDotInsetX);  // inset from the right edge
        CHECK_EQ(d.y, 100 + kit::kRunDotInsetY);             // ... and from the top
        lcd::fill(kBase);
        kit::draw_verdict(kSurf, 130, 100, 30, 22, Verdict::Pending, "9V", kBg);
        const auto pending = snap();
        lcd::fill(kBase);
        g_lcd_pixels = 0;
        kit::draw_verdict(kSurf, 130, 100, 30, 22, Verdict::Running, "9V", kBg);
        CHECK_EQ(g_lcd_pixels, 30u * 22u + unsigned(kit::dot_px(Verdict::Running, true)));   // the cell once, the dot over it
        CHECK_EQ(px(130, 100), kBg);                     // no outline, no fill: the caller's base
        CHECK_EQ(px(131, 101), kBg);
        for (int yy = d.y; yy < d.y + d.h; yy++)
            for (int xx = d.x; xx < d.x + d.w; xx++) CHECK_EQ(px(xx, yy), kFocus);
        CHECK_EQ(px(d.x - 1, d.y + 2) != kFocus, 1);     // exactly the box
        CHECK_EQ(px(d.x + d.w, d.y + 2) != kFocus, 1);
        CHECK_EQ(px(d.x + 2, d.y + d.h) != kFocus, 1);
        CHECK_EQ(changed_in(pending, snap(), 0, 240), kit::kRunDotPx);   // the dot is the only difference from Pending
        // the label is intact beside it (the dot's box does not touch the text of a cell this wide)
        CHECK_EQ(ink_in(131, d.x - 1, 101, 121, kBg) > 0, 1);
        // wiping the dot gives the Pending cell back, pixel for pixel
        kit::draw_run_dot(kSurf, cell, false);
        CHECK_EQ(changed_in(pending, snap(), 0, 240), 0);
        kit::draw_run_dot(kSurf, cell, true);
        CHECK_EQ(px(d.x, d.y), kFocus);
    }
    // A label-less Running cell: the dot centred (the glyph of a 12 px marker), nothing else but the base.
    {
        lcd::fill(kBase);
        g_lcd_pixels = 0;
        kit::draw_verdict(kSurf, 100, 150, 12, 12, Verdict::Running, "", kBg);
        CHECK_EQ(g_lcd_pixels, 12u * 12u);
        int dots = 0;
        for (int yy = 150; yy < 162; yy++)
            for (int xx = 100; xx < 112; xx++) dots += px(xx, yy) == kFocus;
        CHECK_EQ(dots, kit::kRunDotPx);
        CHECK_EQ(px(105, 155) == kFocus, 1);            // the middle
        CHECK_EQ(px(100, 150), kBg);
    }
    // Partial: amber tint, and a 2 px amber bar along the bottom edge that a pass does not have (the shape cue, not the colour).
    lcd::fill(kBase);
    kit::draw_verdict(kSurf, 10, 100, 30, 22, Verdict::Partial, "5-9", kBg);
    CHECK_EQ(px(11, 101), kCautionT);
    CHECK_EQ(px(11, 119), kCautionT);
    CHECK_EQ(px(11, 120), kCaution);
    CHECK_EQ(px(38, 121), kCaution);
    kit::draw_verdict(kSurf, 50, 100, 30, 22, Verdict::Pass, "5-9", kBg);
    CHECK_EQ(px(51, 121), kOkT);
    // Without a label it is a tilde: four 2 px steps (mid, up, down, mid) on the amber tint.
    kit::draw_verdict(kSurf, 100, 150, 12, 12, Verdict::Partial, "", kBg);
    CHECK_EQ(px(102, 155), kCaution);   // column 0 bar
    CHECK_EQ(px(102, 153), kCautionT);
    CHECK_EQ(px(104, 153), kCaution);   // column 1: the crest
    CHECK_EQ(px(104, 155), kCautionT);
    CHECK_EQ(px(106, 157), kCaution);   // column 2: the trough
    CHECK_EQ(px(106, 155), kCautionT);
    CHECK_EQ(px(108, 155), kCaution);   // column 3
    CHECK_EQ(px(101, 151), kCautionT);  // the rest of the cell is the tint
    // Re-painting a cell with a style without fill wipes the old fill.
    kit::draw_verdict(kSurf, 10, 100, 30, 22, Verdict::Skip, "", kBg);
    CHECK_EQ(px(11, 101), kBg);
    // The text sits inside the cell and never outside it.
    CHECK_EQ(ink_in(10, 40, 100, 122, kBg) > 0, 1);
    CHECK_EQ(px(9, 110), kBase);
    CHECK_EQ(px(40, 110), kBase);
}

// Skip is the one verdict that strikes its label through (the shape cue; Pending is faint with no strike, None is dim with no
// strike): a 1 px kDim line across the text at the font's x-height centre, the text width plus 1 px each side, clipped to the cell.
// The line is painted over the text, so a Skip label costs its width + 2 px more than the cell's area.
static void test_skip_strike() {
    using kit::Verdict;
    struct Case { int16_t h; int16_t tw; int16_t hole_y; int16_t strike_dy; };
    // 22 px cell: S16 (adv 8, ascent 12, x-height 7 rows 5..11 of the line: centre row ascent - 1 - 7 / 2 = 8 below the top);
    // 18 px cell: S12 (adv 6, ascent 9, x-height 5 rows 4..8: centre row 9 - 1 - 5 / 2 = 6).
    const Case cases[] = {{22, 24, 103, 8}, {18, 18, 103, 6}};
    for (const Case &c : cases) {
        const int16_t hx = static_cast<int16_t>(70 + (34 - c.tw) / 2), sy = static_cast<int16_t>(c.hole_y + c.strike_dy);
        lcd::fill(kBase);
        g_lcd_pixels = 0;
        kit::draw_verdict(kSurf, 70, 100, 34, c.h, Verdict::Skip, "12V", kBg);
        CHECK_EQ(g_lcd_pixels, 34u * unsigned(c.h) + unsigned(c.tw + 2));
        for (int x = hx - 1; x < hx + c.tw + 1; x++) CHECK_EQ(px(x, sy), kDim);   // the line, text width + 1 px each side
        CHECK_EQ(px(hx - 2, sy), kBg);                                            // and no longer
        CHECK_EQ(px(hx + c.tw + 1, sy), kBg);
        CHECK_EQ(px(hx - 1, sy - 1), kBg);                                        // one pixel thick
        CHECK_EQ(px(hx - 1, sy + 1), kBg);
        CHECK_EQ(px(69, sy), kBase);                                              // inside the cell box
        CHECK_EQ(px(104, sy), kBase);
        // Every other verdict leaves the pad pixel at its base: no strike, whatever its ink.
        const struct { Verdict v; uint16_t base; } others[] = {{Verdict::Pending, kBg}, {Verdict::Pass, kOkT}, {Verdict::Fail, kError},
                                                              {Verdict::Partial, kCautionT}, {Verdict::None, kBg}, {Verdict::Stopped, kBg}};
        for (const auto &o : others) {
            lcd::fill(kBase);
            g_lcd_pixels = 0;
            kit::draw_verdict(kSurf, 70, 100, 34, c.h, o.v, "12V", kBg);
            CHECK_EQ(g_lcd_pixels, 34u * unsigned(c.h));
            CHECK_EQ(px(hx - 1, sy), o.base);
            CHECK_EQ(px(hx + c.tw, sy), o.base);
        }
    }
    // A label that fills the cell: the strike stops at the cell's edges, nothing lands outside.
    lcd::fill(kBase);
    kit::draw_verdict(kSurf, 70, 100, 88, 22, Verdict::Skip, "not offered", kBg);
    CHECK_EQ(px(70, 111), kDim);
    CHECK_EQ(px(157, 111), kDim);
    CHECK_EQ(px(69, 111), kBase);
    CHECK_EQ(px(158, 111), kBase);
    // A label-less Skip is the dash glyph it always was: no extra pixels.
    lcd::fill(kBase);
    g_lcd_pixels = 0;
    kit::draw_verdict(kSurf, 70, 100, 12, 12, Verdict::Skip, "", kBg);
    CHECK_EQ(g_lcd_pixels, 144u);
    // The card's struck name: S12 "PD fixed" (8 cells of 6 px) at (20, 100): the line on the x-height centre (row 100 + 6), the
    // text width + 1 px each side, in the ink, and it costs the text's own pixels plus those 50.
    lcd::fill(kBase);
    kit::draw_struck_text(kSurf, kLabelFont, 20, 100, "PD fixed", kDim, kBg);
    for (int x = 19; x < 20 + 48 + 1; x++) CHECK_EQ(px(x, 106), kDim);
    CHECK_EQ(px(18, 106), kBase);
    CHECK_EQ(px(69, 106), kBase);
    CHECK_EQ(px(19, 105), kBase);
    CHECK_EQ(px(19, 107), kBase);
    // The cost model: a strike is width + 2 px, nothing for any other verdict or an empty label.
    CHECK_EQ(kit::strike_px(Verdict::Skip, "12V", 34, 22), 26);
    CHECK_EQ(kit::strike_px(Verdict::Skip, "12V", 34, 18), 20);
    CHECK_EQ(kit::strike_px(Verdict::Skip, "not offered", 88, 22), 88);
    CHECK_EQ(kit::strike_px(Verdict::Skip, "", 34, 22), 0);
    CHECK_EQ(kit::strike_px(Verdict::None, "12V", 34, 22), 0);
    CHECK_EQ(kit::strike_px(Verdict::Pending, "12V", 34, 22), 0);
}

static void test_alert_band() {
    lcd::fill(kBase);
    kit::draw_alert_band(kSurf, 30, false, "CAUTION");
    CHECK_EQ(px(200, 33), kCaution);
    CHECK_EQ(px(0, 30 + kit::kAlertH - 1), kCaution);
    {   // the hazard stripe under the band: ink and band-colour spans on every row, then nothing
        for (int r = 0; r < kit::kAlertStripeH; r++) {
            CHECK_EQ(ink_in(0, 224, 30 + kit::kAlertH + r, 30 + kit::kAlertH + r + 1, kCaution) > 0, 1);
            int fill = 0;
            for (int x = 0; x < 224; x++) fill += px(x, 30 + kit::kAlertH + r) == kCaution;
            CHECK_EQ(fill >= 106 && fill <= 118, 1);   // about half of each row: diagonal bars 12 px apart
        }
        CHECK_EQ(px(100, 30 + kit::kAlertH + kit::kAlertStripeH), kBase);
    }
    CHECK_EQ(ink_in(32, 120, 34, 60, kCaution) > 0, 1);   // the word, in black ink
    kit::draw_alert_band(kSurf, 30, true, "STOPPED");
    CHECK_EQ(px(200, 33), kError);
    // The warning triangle: apex row narrow, base row wide.
    lcd::fill(kBase);
    kit::draw_warn_triangle(kSurf, 10, 100, 20, kBg, kCaution);
    CHECK_EQ(ink_in(10, 30, 119, 120, kBase), 20);
    CHECK_EQ(ink_in(10, 30, 100, 101, kBase) <= 2, 1);
}

static void test_action_cell() {
    kit::ActionCell a;
    a.init(kSurf, 156, 64);
    lcd::fill(kBase);
    a.draw_idle("START", "no load on the port");
    CHECK_EQ(px(0, 156), kBg);                          // the ticket: ground gutters of kTicketInset px
    CHECK_EQ(px(7, 190), kBg);
    CHECK_EQ(px(216, 190), kBg);
    CHECK_EQ(px(8, 156), kTicketFrame);                 // Classic: the 2 px cyan frame (Paper, Night: none)
    CHECK_EQ(px(9, 157), kTicketFrame);
    CHECK_EQ(px(215, 219), kTicketFrame);
    CHECK_EQ(px(13, 160), kTicket);
    const unsigned pxn = g_lcd_pixels;
    a.draw_idle("START", "no load on the port");        // cached
    CHECK_EQ(g_lcd_pixels, pxn);
    a.draw_armed("CONFIRM", "start in 5s window", 1000);
    CHECK_EQ(px(13, 160), kCaution);
    CHECK_EQ(px(100, 219), kBg);                        // the 4 px bar spans the full width
    CHECK_EQ(px(100, 216), kBg);
    CHECK_EQ(px(100, 215), kCaution);
    a.draw_armed("CONFIRM", "start in 5s window", 500);   // drained to half: the right half is amber again
    CHECK_EQ(px(50, 217), kBg);
    CHECK_EQ(px(200, 217), kCaution);
    const unsigned before = g_lcd_pixels;
    a.draw_armed("CONFIRM", "start in 5s window", 500);
    CHECK_EQ(g_lcd_pixels, before);
    a.draw_armed("CONFIRM", "start in 5s window", 400);   // only the vacated columns repaint
    CHECK_EQ(g_lcd_pixels - before < 30 * 8, 1);
    a.invalidate();
    a.draw_idle("START", "");
    CHECK_EQ(px(13, 160), kTicket);
}

static void test_progress_bar() {
    kit::ProgressBar p;
    p.init(kSurf, 10, 100, 100);
    lcd::fill(kBase);
    p.set(1, 4);
    CHECK_EQ(px(10, 100), kFocus);
    CHECK_EQ(px(34, 102), kFocus);
    CHECK_EQ(px(35, 101), kRule);
    CHECK_EQ(px(109, 100), kRule);
    CHECK_EQ(px(10, 103), kBase);                       // 3 px tall
    const unsigned n = g_lcd_pixels;
    p.set(1, 4);
    CHECK_EQ(g_lcd_pixels, n);                          // between steps only
    CHECK_EQ(p.cost(1, 4), 0);                          // nothing to paint costs nothing
    const int c = p.cost(2, 4);
    const unsigned m = g_lcd_pixels;
    p.set(2, 4);
    CHECK_EQ(c > 0 && g_lcd_pixels - m <= static_cast<unsigned>(c), true);   // the quoted cost bounds what the bar paints
    p.set(4, 4);
    CHECK_EQ(px(109, 101), kFocus);
    p.set(0, 0);
    CHECK_EQ(px(10, 100), kRule);
}

static void test_kv_and_info() {
    kit::KvLayout lay;
    const char *const labels[3] = {"Tests", "Peak", "Cable"};
    lcd::fill(kBase);
    kit::kv_static(kSurf, lay, labels, 3);
    CHECK_EQ(px(100, 24), kBg);                         // receipt rows: white space, no hairlines
    CHECK_EQ(px(100, 24 + 26), kBg);
    CHECK_EQ(px(100, 24 + 3 * 26), kBg);                // the closing line is ground too
    CHECK_EQ(px(100, 24 + 4 * 26), kBase);
    CHECK_EQ(ink_in(6, 60, 24 + 1, 24 + 26, kBg) > 0, 1);   // key
    {   // the dot leader from the key to the value column, 2 x 2 px dots
        int dots = 0;
        for (int y = 24; y < 24 + 26; y++) for (int x = 50; x < lay.value_x - 4; x++) dots += px(x, y) == kFaint;
        CHECK_EQ(dots > 0 && dots % 4 == 0, 1);
    }
    Field cells[3];
    kit::kv_cells(kSurf, lay, cells, 3);
    cells[1].draw("20V 3.25A");
    CHECK_EQ(ink_in(72, 150, 24 + 26 + 1, 24 + 2 * 26, kBg) > 0, 1);
    CHECK_EQ(ink_in(72, 150, 24 + 1, 24 + 26, kBase), 0);   // the other cells are not touched
    // Per-cell widths: a right-aligned cell narrowed to 5 of 18 chars keeps its right edge and leaves the left of the box alone.
    lcd::fill(kBase);
    const uint8_t narrow[3] = {18, 5, 18};
    kit::KvLayout rl = lay; rl.align = Align::Right;
    kit::kv_cells(kSurf, rl, cells, 3, narrow);
    cells[1].draw("12345");
    const int right = rl.value_x + 18 * kSmall.adv, left5 = right - 5 * kSmall.adv;
    CHECK_EQ(ink_in(left5, right, 24 + 26 + 1, 24 + 2 * 26, kBg) > 0, 1);
    CHECK_EQ(ink_in(rl.value_x, left5, 24 + 26 + 1, 24 + 2 * 26, kBase), 0);
    lcd::fill(kBase);
    kit::kv_row(kSurf, lay, 24, "Header", "PD3.1", kText);
    CHECK_EQ(px(100, 24), kBg);
    CHECK_EQ(ink_in(72, 120, 25, 49, kBg) > 0, 1);
    lcd::fill(kBase);
    kit::draw_info_row(kSurf, 50, "PD", "20V5A");
    CHECK_EQ(ink_in(4, 20, 50, 70, kBg) > 0, 1);
    CHECK_EQ(ink_in(52, 100, 50, 70, kBg) > 0, 1);
    CHECK_EQ(px(100, 70), kBase);
}

// ---- wave 2: ResultGrid, PassBandGauge, HeroReadout --------------------------------------------------------------------------
static void test_result_row() {
    using kit::ResultCell;
    using kit::Verdict;
    // Volt-only row: 26 px, 4 cells flex over x61..223, the strip right-aligns its label, the rule sits at x60.
    kit::ResultRowSpec volts;
    volts.n = 2;
    CHECK_EQ(kit::result_row_h(volts), 26);
    volts.section = true;
    CHECK_EQ(kit::result_row_h(volts), 28);   // the 2 px section rule counts
    kit::ResultRowSpec cur;
    cur.n = 1;
    cur.cells[0].current = "3A";
    CHECK_EQ(kit::result_row_h(cur), 40);

    const ResultCell vc[4] = {{Verdict::Pass, "5V", {}}, {Verdict::Fail, "9V", {}}, {Verdict::Skip, "12V", {}}, {Verdict::Running, "20V", {}}};
    lcd::fill(kBase);
    kit::draw_result_row(kSurf, 100, 26, "QC2", vc, 4);
    CHECK_EQ(px(60, 110), kBg);                         // no label rule: white space
    CHECK_EQ(px(2, 110), kBg);                          // strip is not focused
    CHECK_EQ(ink_in(6, 56, 100, 125, kBg) > 0, 1);      // the 12 px label
    CHECK_EQ(px(65, 102), kOkT);                        // pass cell: tint
    CHECK_EQ(px(61, 100), kOkT);                        // the first cell starts at x61
    CHECK_EQ(px(107, 102), kError);                     // fail cell: solid red (n=4: 163/4 = 41 px pitch, cell 2 at x102)
    CHECK_EQ(px(101, 110), kBg);                        // the 1 px gap between cells
    CHECK_EQ(px(223, 110), kBg);                        // the right edge stays clear
    CHECK_EQ(px(150, 125), kBg);                        // the last line of a row is never painted over
    CHECK_EQ(px(183, 110), kBg);                        // running: the pending look, no fill, no ring
    CHECK_EQ(px(184, 110), kBg);
    {   // ... and the dot in the cell's top-right corner
        int dots = 0, left = 224, top = 240;
        for (int yy = 100; yy < 126; yy++)
            for (int xx = 0; xx < 224; xx++)
                if (px(xx, yy) == kFocus) { dots++; left = std::min(left, xx); top = std::min(top, yy); }
        CHECK_EQ(dots, kit::kRunDotPx);                 // one 5 x 5 box, in the 4th cell (x184..), on the row's top edge
        CHECK_EQ(left >= 184, 1);
        CHECK_EQ(top, 100 + kit::kRunDotInsetY);
    }
    CHECK_EQ(px(60, 126), kBase);                       // nothing below the row

    // Focused unit: tint, 3 px bar and a cyan label.
    lcd::fill(kBase);
    kit::draw_result_row(kSurf, 100, 26, "QC2", vc, 4, true);
    CHECK_EQ(px(1, 110), kFocus);
    CHECK_EQ(px(30, 102), kFocusT);
    CHECK_EQ(px(59, 110), kFocusT);
    CHECK_EQ(px(60, 110), kBg);

    // Section rule: two kRule lines on the row's top, the content starts under them.
    lcd::fill(kBase);
    kit::draw_result_row(kSurf, 100, 28, "QC2", vc, 4, false, true);
    CHECK_EQ(px(30, 100), kRule);
    CHECK_EQ(px(220, 101), kRule);
    CHECK_EQ(px(65, 102), kOkT);

    // With currents: verdict cells 24 px, under them one bracket per run of equal current.
    const ResultCell cc[4] = {{Verdict::Pass, "5V", "3A"}, {Verdict::Pass, "9V", "3A"}, {Verdict::Fail, "12V", "3A"}, {Verdict::None, "20V", "5A"}};
    lcd::fill(kBase);
    kit::draw_result_row(kSurf, 100, 40, "USB PD", cc, 4);
    CHECK_EQ(px(65, 100 + 23), kOkT);                   // cells are 24 px tall
    CHECK_EQ(px(65, 100 + 24), kBg);                    // and the bracket zone below is plain
    CHECK_EQ(ink_in(61, 224, 100 + 28, 100 + 40, kBg) > 0, 1);   // current labels and brackets
    CHECK_EQ(px(63, 100 + 31), kFaint);                   // the run 5V 9V 12V: bracket line left end
    CHECK_EQ(px(63, 100 + 29), kFaint);                   // its 3 px end tick
    CHECK_EQ(px(63, 100 + 28), kFaint);                   // ... 3 px above the line
    CHECK_EQ(px(63, 100 + 27), kBg);
    // Neutral: the bracket under the failed 12V is dim, never red.
    int reds = 0;
    for (int y = 100 + 24; y < 100 + 40; y++)
        for (int x = 61; x < 224; x++) reds += px(x, y) == kError;
    CHECK_EQ(reds, 0);
    CHECK_EQ(px(100, 100 + 39), kBg);

    // A lone never-ran cell spells its reason (a 16 px label that fits the 162 px cell).
    const ResultCell lone[1] = {{Verdict::None, "not offered", {}}};
    lcd::fill(kBase);
    kit::draw_result_row(kSurf, 100, 26, "Huawei", lone, 1);
    CHECK_EQ(ink_in(61, 224, 100, 125, kBg) > 0, 1);
    CHECK_EQ(px(100, 110) == kError, 0);
}

// A redraw is bus time: a verdict cell and a result row stream every pixel of their box exactly once (no wipe followed by a
// repaint of the same area), whatever the verdicts, labels, currents, focus and section rule.
static void test_result_rows_stream_each_pixel_once() {
    using kit::ResultCell;
    using kit::Verdict;
    const Verdict all[] = {Verdict::Pending, Verdict::Running, Verdict::Pass, Verdict::Fail, Verdict::None, Verdict::Skip, Verdict::Stopped,
                           Verdict::Partial};
    for (const Verdict v : all) {
        for (const std::string_view label : {std::string_view{}, std::string_view{"20V"}}) {
            for (const int16_t h : {12, 24, 25}) {
                if (h == 12 && !label.empty()) continue;   // a 12 px cell is the row marker: it never carries a label
                lcd::fill(kBase);
                g_lcd_pixels = 0;
                kit::draw_verdict(kSurf, 70, 100, 34, h, v, label, kBg);
                // A Skip label is struck through: the line (text width 24 + 2 px) is painted twice; a labelled Running cell's
                // dot is painted over its corner (a label-less one leaves a hole for its dot).
                CHECK_EQ(g_lcd_pixels, 34u * unsigned(h) + (v == Verdict::Skip && !label.empty() ? 26u : 0u) +
                                           unsigned(kit::dot_px(v, !label.empty())));
            }
        }
    }
    const ResultCell volts[5] = {{Verdict::Pass, "5V", {}}, {Verdict::Fail, "9V", {}}, {Verdict::Skip, "12V", {}},
                                 {Verdict::Running, "15V", {}}, {Verdict::Pending, "20V", {}}};
    const ResultCell cur[5] = {{Verdict::Pass, "5V", "3A"}, {Verdict::Pass, "9V", "3A"}, {Verdict::Fail, "12V", "3A"},
                               {Verdict::None, "15V", "5.25A"}, {Verdict::Running, "20V", "5.25A"}};
    const ResultCell mixed[3] = {{Verdict::Pass, "5V", "3A"}, {Verdict::Pass, "9V", {}}, {Verdict::Pass, "12V", "2.25A"}};
    const ResultCell scp[3] = {{Verdict::Pass, "5V", {}}, {Verdict::Fail, "10V", "2.25A"}, {Verdict::Fail, "4.5V", "5A"}};
    const ResultCell lone[1] = {{Verdict::None, "not offered", {}}};
    // strike: the struck "12V" of the row (26 px), 0 = no Skip cell in it; dot: the Running cell's dot (25 px), 0 = none in it
    struct Case { const ResultCell *cells; int n; const char *label; unsigned strike, dot; };
    const Case cases[] = {{volts, 5, "QC2", 26, 25}, {volts, 2, "Huawei", 0, 0}, {cur, 5, "USB PD", 0, 25}, {cur, 3, "PPS", 0, 0},
                          {mixed, 3, "SPR AVS", 0, 0}, {scp, 3, "SCP", 0, 0}, {lone, 1, "Huawei", 0, 0}, {volts, 0, "Nothing", 0, 0},
                          {volts, 5, "", 26, 25}};
    for (const Case &c : cases) {
        kit::ResultRowSpec spec;
        spec.n = static_cast<uint8_t>(c.n);
        for (int i = 0; i < c.n; i++) spec.cells[i] = c.cells[i];
        for (const bool focus : {false, true}) {
            for (const bool section : {false, true}) {
                spec.section = section;
                const int16_t h = kit::result_row_h(spec);
                lcd::fill(kBase);
                g_lcd_pixels = 0;
                kit::draw_result_row(kSurf, 100, h, c.label, c.cells, c.n, focus, section);
                CHECK_EQ(g_lcd_pixels, 224u * unsigned(h) + c.strike + c.dot);
                CHECK_EQ(px(150, 100 + h), kBase);   // nothing below the row
                unsigned left = 0;   // and no pixel of the row stays unpainted: the repaint needs no erase
                for (int yy = 100; yy < 100 + h; yy++)
                    for (int xx = 0; xx < 224; xx++) left += px(xx, yy) == kBase;
                if (left) printf("  uncovered %u: %s n=%d focus=%d section=%d\n", left, c.label, c.n, focus, section);
                CHECK_EQ(left, 0u);
                unsigned spill = 0;   // nor does any pixel land outside the row
                for (int yy = 0; yy < 240; yy++)
                    for (int xx = 0; xx < 224; xx++) spill += (yy < 100 || yy >= 100 + h) && px(xx, yy) != kBase;
                if (spill) printf("  spill %u: %s n=%d focus=%d section=%d\n", spill, c.label, c.n, focus, section);
                CHECK_EQ(spill, 0u);
            }
        }
    }
}

// The landscape rotations stretch the 224x240 content onto 240x224 while glyphs keep their native size, so a glyph no
// longer fills the box that was left open for it. A row repainted alone must still cover its whole mapped box.
static void test_result_rows_cover_their_box_when_scaled() {
    using kit::ResultCell;
    using kit::Verdict;
    const ResultCell cells[3] = {{Verdict::Pass, "5V", {}}, {Verdict::Fail, "10V", "2.25A"}, {Verdict::Running, "4.5V", "5A"}};
    const Rect dest[] = {{0, 16, 240, 224}, {0, 0, 240, 224}, {16, 0, 224, 240}, {16, 8, 208, 224}};
    for (const Rect &d : dest) {
        for (const bool focus : {false, true}) {
            for (const bool section : {false, true}) {
                kit::ResultRowSpec spec;
                spec.n = 3;
                for (int i = 0; i < 3; i++) spec.cells[i] = cells[i];
                spec.section = section;
                const int16_t h = kit::result_row_h(spec);
                lcd::fill(kBase);
                {
                    ContentViewport viewport(d);
                    kit::draw_result_row(kSurf, 100, h, "SCP", cells, 3, focus, section);
                }
                const Rect box = [&] { ContentViewport viewport(d); return ContentViewport::mapped_rect({0, 100, 224, h}); }();
                unsigned left = 0;
                for (int yy = box.y; yy < box.y + box.h; yy++)
                    for (int xx = box.x; xx < box.x + box.w; xx++) left += px(xx, yy) == kBase;
                if (left) printf("  scaled %d,%d %dx%d focus=%d section=%d: %u unpainted\n", d.x, d.y, d.w, d.h, focus, section, left);
                CHECK_EQ(left, 0u);
            }
        }
    }
}

namespace {
struct GridModel {
    int n = 0;
    int heights_cur[16] = {};   // 1 = the row carries a current (40 px)
    uint8_t pass[16] = {};
};
void grid_fn(void *ctx, int i, kit::ResultRowSpec &out) {
    auto *m = static_cast<GridModel *>(ctx);
    out.label = "Row";
    out.n = 2;
    out.cells[0] = {m->pass[i] == 3   ? kit::Verdict::Skip
                    : m->pass[i] == 2 ? kit::Verdict::Partial
                    : m->pass[i]      ? kit::Verdict::Pass
                                      : kit::Verdict::Pending,
                    "5V", m->heights_cur[i] ? "3A" : ""};
    out.cells[1] = {kit::Verdict::Pending, "9V", m->heights_cur[i] ? "3A" : ""};
    out.section = i == 3;
}
}  // namespace

static void test_result_grid() {
    GridModel m;
    m.n = 8;
    for (int i = 0; i < 8; i++) m.heights_cur[i] = i % 2;   // 26, 40, 26, 40 (+2 section), ...
    kit::ResultGrid g;
    lcd::fill(kBase);
    g.init(kSurf, 100, 222, grid_fn, &m);
    g.set_count(m.n);
    g.draw();
    // 26 + 40 + 26 + 42 = 134 > 122: three whole rows fit, the 4th does not draw.
    CHECK_EQ(g.visible(), 3);
    CHECK_EQ(g.row_y(0), 100);
    CHECK_EQ(g.row_y(1), 126);
    CHECK_EQ(g.row_y(2), 166);
    CHECK_EQ(g.row_y(3), -1);
    CHECK_EQ(px(100, 192 + 5), kBg);                    // the strip under the last whole row is kBg, not left unpainted
    CHECK_EQ(px(100, 221), kBg);
    CHECK_EQ(px(100, 222), kBase);                      // the footer band is not touched
    // An idle redraw writes nothing.
    auto before = snap();
    g.draw();
    CHECK_EQ(changed_in(before, snap(), 0, 240), 0);
    // One cell changes: only its row repaints.
    before = snap();
    m.pass[1] = 1;
    g.draw();
    CHECK_EQ(changed_in(before, snap(), 100, 126) + changed_in(before, snap(), 166, 240), 0);
    CHECK_EQ(changed_in(before, snap(), 126, 166) > 0, 1);
    // Pixel scrolling: showing row 4 moves the window just far enough, and the section rule of the top row is dropped.
    g.show(4);
    CHECK_EQ(g.first() > 0, 1);
    g.draw();
    CHECK_EQ(g.row_y(4) >= 100, 1);
    CHECK_EQ(g.row_y(4) + 26 <= 222, 1);
    g.show(0);
    CHECK_EQ(g.first(), 0);
    // A different ctx restarts at the top.
    GridModel m2 = m;
    g.show(5);
    g.init(kSurf, 100, 222, grid_fn, &m2);
    CHECK_EQ(g.first(), 0);
}

// draw_some: pieces of at most a budget, a verdict-only change repaints just its cells, `prefer` goes first.
static void test_result_grid_budgeted() {
    GridModel m;
    m.n = 8;
    for (int i = 0; i < 8; i++) m.heights_cur[i] = i % 2;
    kit::ResultGrid g;
    lcd::fill(kBase);
    g.init(kSurf, 100, 222, grid_fn, &m);
    g.set_count(m.n);
    // A 1 px budget still admits the first piece (progress), then defers the rest.
    unsigned n0 = g_lcd_pixels;
    int spent = g.draw_some(1);
    CHECK_EQ(spent, 224 * 26);
    CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
    CHECK_EQ(g.pending(), true);
    // Budget = one 26 px row + a 40 px row: two rows per call.
    int calls = 0;
    while (g.pending() && calls < 10) { g.draw_some(224 * 66); calls++; }
    CHECK_EQ(calls <= 3, 1);
    CHECK_EQ(g.pending(), false);
    // Every call stayed inside budget (or was a single piece).
    // Idle: nothing.
    CHECK_EQ(g.draw_some(224 * 66), 0);
    CHECK_EQ(g.pending(), false);
    // The delta path: only the changed cell's pixels, identical to what a whole repaint paints.
    m.pass[1] = 1;
    n0 = g_lcd_pixels;
    spent = g.draw_some(1 << 20);
    CHECK_EQ(spent > 0, 1);
    CHECK_EQ(spent < 224 * 40 / 2, 1);                  // a cell, not a row
    CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
    const auto delta = snap();
    lcd::fill(kBase);
    kit::ResultGrid g2;
    g2.init(kSurf, 100, 222, grid_fn, &m);
    g2.set_count(m.n);
    g2.draw();
    CHECK_EQ(changed_in(delta, snap(), 0, 240), 0);     // identical pixels
    // `prefer` paints first: with room for one row only, the preferred (later) row wins over the earlier changed one.
    m.pass[0] = 1; m.pass[2] = 1;
    lcd::fill(kBase);
    kit::ResultGrid g3;
    g3.init(kSurf, 100, 222, grid_fn, &m);
    g3.set_count(m.n);
    g3.draw_some(1, 2);
    CHECK_EQ(g3.pending(), true);
    CHECK_EQ(px(100, 166 + 5) != kBase, 1);             // row 2 (y166..) painted
    CHECK_EQ(px(100, 100 + 5), kBase);                  // row 0 waits
    // A Partial cell goes through the delta path like any verdict (3 bits per cell hold it): the pixels equal a whole repaint,
    // the bar included, and what it costs is the cell, not the row.
    GridModel mp;
    mp.n = 8;
    for (int i = 0; i < 8; i++) mp.heights_cur[i] = i % 2;
    lcd::fill(kBase);
    kit::ResultGrid gp;
    gp.init(kSurf, 100, 222, grid_fn, &mp);
    gp.set_count(mp.n);
    gp.draw();
    mp.pass[1] = 2;
    n0 = g_lcd_pixels;
    spent = gp.draw_some(1 << 20);
    CHECK_EQ(spent > 0 && spent < 224 * 40 / 2, true);
    CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
    const auto pdelta = snap();
    lcd::fill(kBase);
    kit::ResultGrid gq;
    gq.init(kSurf, 100, 222, grid_fn, &mp);
    gq.set_count(mp.n);
    gq.draw();
    CHECK_EQ(changed_in(pdelta, snap(), 0, 240), 0);
    // A Skip cell's strike is in the cost model, on both paths. Row 0 is 26 px (a 25 px verdict part, so the 16 px label): its
    // first cell is 81 px wide ("5V" is 16 px, a strike of 18). The verdict-only repaint costs the cell's area plus the strike.
    mp.pass[0] = 3;
    n0 = g_lcd_pixels;
    spent = gp.draw_some(1 << 20);
    CHECK_EQ(spent, 81 * 25 + 18);
    CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
    // The whole row of a fresh grid (the first piece of a 1 px budget) costs the row's area plus the same strike.
    lcd::fill(kBase);
    kit::ResultGrid gs;
    gs.init(kSurf, 100, 222, grid_fn, &mp);
    gs.set_count(mp.n);
    n0 = g_lcd_pixels;
    spent = gs.draw_some(1);
    CHECK_EQ(spent, 224 * 26 + 18);
    CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
    // A budget that fits the row's area but not its strike defers it when strict.
    lcd::fill(kBase);
    kit::ResultGrid gt;
    gt.init(kSurf, 100, 222, grid_fn, &mp);
    gt.set_count(mp.n);
    gt.draw_some(224 * 26, -1, true);
    CHECK_EQ(gt.pending(), true);
    CHECK_EQ(px(100, 105), kBase);                      // row 0 waits (the same budget without the strike would paint it)
    lcd::fill(kBase);
    kit::ResultGrid gu;
    mp.pass[0] = 0;
    gu.init(kSurf, 100, 222, grid_fn, &mp);
    gu.set_count(mp.n);
    gu.draw_some(224 * 26, -1, true);
    CHECK_EQ(px(100, 105) != kBase, 1);
}

namespace {
// Rows of 1..5 labelled cells; the cells before `run` (counted over all rows) are Pass, `run` is Running, the rest Pending.
struct DotModel {
    int rows = 0;
    uint8_t n[16] = {};
    int run = 0;
    bool cur = false;   // a finished cell shows its current (its row grows taller: the shape changes, not the core)
    int cell0(int row) const { int c = 0; for (int i = 0; i < row; i++) c += n[i]; return c; }
};
void dot_fn(void *ctx, int i, kit::ResultRowSpec &out) {
    static const char *const kLabels[5] = {"5V", "9V", "12V", "15V", "20V"};
    auto *m = static_cast<DotModel *>(ctx);
    out.label = "Row";
    out.n = m->n[i];
    out.section = i == 2;
    const int base = m->cell0(i);
    for (int k = 0; k < out.n; k++) {
        const int g = base + k;
        out.cells[k] = {g < m->run ? kit::Verdict::Pass : g == m->run ? kit::Verdict::Running : kit::Verdict::Pending, kLabels[(g + k) % 5], m->cur && g < m->run ? "1.5A" : ""};
    }
}
int focus_px(int y0, int y1) {   // kFocus pixels in the grid's rows
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 61; x < 224; x++) n += px(x, y) == kFocus;
    return n;
}
}  // namespace

// sync_dots: the dot leaves the cell that was under test and lands on the one that is, for kRunDotPx each (a cell's worth when
// the dot sits against the label), with no budget and no repaint of the rest; once the window has followed and draw_some has
// painted the finished cell's verdict, the pixels equal a fresh paint of the same window.
static void test_result_grid_sync_dots() {
    DotModel m;
    m.rows = 6;
    const uint8_t shape[6] = {5, 3, 4, 1, 2, 5};   // every cell count a row of the Running grid can have: 1..5
    for (int i = 0; i < 6; i++) m.n[i] = shape[i];
    int total = 0;
    for (int i = 0; i < 6; i++) total += shape[i];
    auto row_of = [&](int cell) { int c = 0, i = 0; for (; i < m.rows - 1; i++) { if (cell < c + m.n[i]) break; c += m.n[i]; } return i; };
    kit::ResultGrid g;
    lcd::fill(kBase);
    g.init(kSurf, 100, 222, dot_fn, &m);
    g.set_count(m.rows);
    g.draw();
    CHECK_EQ(focus_px(100, 222), kit::kRunDotPx);        // one dot on screen, on the running cell
    CHECK_EQ(g.sync_dots(), 0);                          // nothing moved: nothing to do
    for (int step = 1; step < total; step++) {
        m.run = step;
        const int want = row_of(step);
        // 1. the sync alone: the dot has left the old cell and sits on the new one when that row is on screen as it is
        const unsigned n0 = g_lcd_pixels;
        const int spent = g.sync_dots();
        CHECK_EQ(g_lcd_pixels - n0, unsigned(spent));
        CHECK_EQ(spent > 0 && spent < 224 * 26 / 2, true);
        CHECK_EQ(focus_px(100, 222), g.row_y(want) >= 0 ? kit::kRunDotPx : 0);
        // 2. then what the page does: the window follows, the rows and cells paint
        g.anchor_bottom(want);
        g.draw_some(1 << 20);
        CHECK_EQ(focus_px(100, 222), kit::kRunDotPx);
        const auto got = snap();
        lcd::fill(kBase);
        kit::ResultGrid f;
        f.init(kSurf, 100, 222, dot_fn, &m);
        f.set_count(m.rows);
        f.set_first(g.first());
        f.draw();
        CHECK_EQ(changed_in(got, snap(), 0, 240), 0);   // the same pixels as a grid painted fresh
        std::copy(got.begin(), got.end(), g_panel);     // and back to the live grid's panel
    }
}

// The same with finished cells that gain a current (their rows change shape in the very tick the step moves): the dot still
// leaves the old cell and lands on the new one on the sync alone, and never shows twice.
static void test_result_grid_sync_dots_while_rows_change_shape() {
    DotModel m;
    m.rows = 4; m.cur = true;
    const uint8_t shape[4] = {3, 3, 3, 3};
    for (int i = 0; i < 4; i++) m.n[i] = shape[i];
    kit::ResultGrid g;
    lcd::fill(kBase);
    g.init(kSurf, 100, 222, dot_fn, &m);
    g.set_count(m.rows);
    g.draw();
    for (int step = 1; step < 12; step++) {
        m.run = step;
        const int want = step / 3;
        g.sync_dots();
        const int on = focus_px(100, 222);
        CHECK_EQ(on <= kit::kRunDotPx, true);                                    // never a second dot, in the stale rows
        CHECK_EQ(on == 0 && g.row_y(want) >= 0 && g.row_y(want) + 26 <= 222, false);   // and one when its row is on screen
        g.anchor_bottom(want);
        g.draw_some(1 << 20);
        CHECK_EQ(focus_px(100, 222), kit::kRunDotPx);
    }
}

// anchor_bottom: the window ends on the row, scrolling counts pixels (rows of 26 / 40 px, a 2 px rule above row 3), it needs no
// blank above, and it scrolls back up for an earlier row (the result depends on the index alone).
static void test_result_grid_anchor_bottom() {
    GridModel m;
    m.n = 8;
    for (int i = 0; i < 8; i++) m.heights_cur[i] = i % 2;   // 26 40 26 42 26 40 26 40 px: the window is 122 px
    kit::ResultGrid g;
    lcd::fill(kBase);
    g.init(kSurf, 100, 222, grid_fn, &m);
    g.set_count(m.n);
    struct { int index, first; } cases[] = {{0, 0}, {2, 0}, {3, 1}, {4, 2}, {5, 3}, {6, 4}, {7, 5}, {2, 0}, {7, 5}};
    for (const auto &c : cases) {
        g.anchor_bottom(c.index);
        CHECK_EQ(g.first(), c.first);
        g.draw();
        const int y = g.row_y(c.index);
        CHECK_EQ(y >= 100, true);                         // visible, whole
        CHECK_EQ(y + (c.index % 2 ? 40 : 26) + (c.index == 3 ? 2 : 0) <= 222, true);   // never below the window
    }
    g.anchor_bottom(7);
    g.draw();
    // The last visible row is the one under test whenever the rows above it fill the window.
    CHECK_EQ(g.visible() > 0, true);
    CHECK_EQ(g.first() + g.visible() - 1, 7);
    g.anchor_bottom(99);                                  // out of range: unchanged
    CHECK_EQ(g.first(), 5);
}

static void test_pass_band() {
    // Scale 0..30 V over x6..216 (w 210): 7 px per volt; window for 20 V = 19..21 V, the needle at 20 V.
    lcd::fill(kBase);
    const int np = kit::draw_pass_band(kSurf, 6, 100, 210, 20000, 20000, 30000);
    CHECK_EQ(np, 6 + 140);
    CHECK_EQ(px(np, 100), kText);                       // needle: 2 px wide, full 12 px tall
    CHECK_EQ(px(np + 1, 111), kText);
    CHECK_EQ(px(np + 2, 105), kOk);                     // inside the window core
    CHECK_EQ(px(np - 20, 105), kRule);                  // track outside it
    CHECK_EQ(px(np - 20, 100), kBg);                    // 12 px strip, the track is 4 px in the middle
    CHECK_EQ(px(6 + 133, 103), kOkT);                   // window starts at 19 V = x139: tint 8 px high
    CHECK_EQ(px(6 + 132, 103), kBg);
    CHECK_EQ(px(100, 99), kBase);                       // nothing above or below the strip
    CHECK_EQ(px(100, 112), kBase);
    // Moving the needle repaints only the two old and the two new columns.
    auto before = snap();
    const int np2 = kit::draw_pass_band(kSurf, 6, 100, 210, 5000, 20000, 30000, np);
    CHECK_EQ(np2, 6 + 35);
    int cols = 0;
    for (int x = 0; x < 240; x++) {
        bool any = false;
        for (int y = 96; y < 116; y++) any = any || g_panel[y * 240 + x] != before[y * 240 + x];
        cols += any;
    }
    CHECK_EQ(cols <= 4, 1);
    CHECK_EQ(px(np, 100), kBg);                         // the old needle is gone
    CHECK_EQ(px(np, 105), kOk);                         // and its column shows the window again
    CHECK_EQ(px(np2, 100), kText);
    // One scale for the run (0..22 V): the window moves with the target. 5 V +-5 % is x51..x56, 6 columns: no widening.
    lcd::fill(kBase);
    kit::draw_pass_band(kSurf, 6, 100, 210, 0, 5000, 22000);
    const int mid5 = 6 + 210 * 5000 / 22000;            // x53
    int ok5 = 0;
    for (int x = 0; x < 240; x++) ok5 += px(x, 105) == kOk;
    CHECK_EQ(ok5, 6);
    CHECK_EQ(px(mid5, 105), kOk);
    // 3 V on a 36 V (EPR) scale: +-5 % is x22..x24, 3 columns, widened to kPassBandMinW centred on the target (x23).
    lcd::fill(kBase);
    kit::draw_pass_band(kSurf, 6, 100, 210, 0, 3000, 36000);
    int ok3 = 0;
    for (int x = 0; x < 240; x++) ok3 += px(x, 105) == kOk;
    CHECK_EQ(ok3, kit::kPassBandMinW);
    CHECK_EQ(px(6 + 210 * 3000 / 36000, 105), kOk);
    lcd::fill(kBase);
    kit::draw_pass_band(kSurf, 6, 100, 210, 0, 20000, 22000);
    CHECK_EQ(px(mid5, 105), kRule);                     // the 20 V window is elsewhere
    CHECK_EQ(px(6 + 210 * 20000 / 22000, 105), kOk);
    // Unchanged needle: nothing is written.
    before = snap();
    CHECK_EQ(kit::draw_pass_band(kSurf, 6, 100, 210, 5000, 20000, 30000, np2), np2);
    CHECK_EQ(changed_in(before, snap(), 0, 240), 0);
    // Out of scale values clamp onto the track.
    lcd::fill(kBase);
    CHECK_EQ(kit::draw_pass_band(kSurf, 6, 100, 210, 99000, 20000, 30000), 6 + 208);
    CHECK_EQ(kit::draw_pass_band(kSurf, 6, 100, 210, -5, 20000, 30000), 6);
}

// The sweep row's gauge: the swept range as two dotted lines (the APDO's min and max), a marker for the setpoint that moves along
// it, the white needle for VBUS. Scale 0..30 V over x6..216 (7 px per volt), strip at y100.
static void test_range_gauge() {
    const auto g = kit::range_geom(6, 210, 20000, 12000, 3300, 21000, 30000);
    CHECK_EQ(g.lo, 6 + 23);        // 3.3 V
    CHECK_EQ(g.hi, 6 + 147);       // 21 V
    CHECK_EQ(g.mark, 6 + 84);      // the 12 V setpoint, mid-range
    CHECK_EQ(g.needle, 6 + 140);
    // the marker's 6 px cap stays inside the strip at both ends of the scale
    CHECK_EQ(kit::range_geom(6, 210, 0, 0, 0, 30000, 30000).mark, 6 + 2);
    CHECK_EQ(kit::range_geom(6, 210, 0, 30000, 0, 30000, 30000).mark, 6 + 210 - 4);
    CHECK_EQ(kit::range_geom(6, 210, 99000, 12000, 3300, 21000, 30000).needle, 6 + 208);   // the needle clamps like the pass band's
    CHECK_EQ(kit::range_geom(6, 210, -5, 12000, 3300, 21000, 30000).needle, 6);

    lcd::fill(kBase);
    const auto pos = kit::draw_range_gauge(kSurf, 6, 100, 210, 20000, 12000, 3300, 21000, 30000);
    CHECK_EQ(pos.mark, g.mark); CHECK_EQ(pos.needle, g.needle);
    // dotted lines at the range's min and max: 2 px wide, 2 rows on / 2 off, 12 px tall
    for (const int x : {int(g.lo), int(g.lo) + 1, int(g.hi) - 1, int(g.hi)}) {
        CHECK_EQ(px(x, 100), kOk); CHECK_EQ(px(x, 101), kOk);
        CHECK_EQ(px(x, 102), kBg); CHECK_EQ(px(x, 103), kBg);
        CHECK_EQ(px(x, 104), kOk); CHECK_EQ(px(x, 105), kOk);
        CHECK_EQ(px(x, 106), kRule); CHECK_EQ(px(x, 107), kRule);   // the gap rows show the track under the line
        CHECK_EQ(px(x, 108), kOk); CHECK_EQ(px(x, 109), kOk);
        CHECK_EQ(px(x, 110), kBg); CHECK_EQ(px(x, 111), kBg);
    }
    CHECK_EQ(px(g.lo - 1, 100), kBg);                    // nothing left of the min line but the plain track
    CHECK_EQ(px(g.lo - 1, 105), kRule);
    CHECK_EQ(px(g.hi + 1, 105), kRule);
    CHECK_EQ(px(10, 105), kRule); CHECK_EQ(px(10, 101), kBg);
    CHECK_EQ(px(210, 105), kRule);
    // the range between the lines is tinted (8 px high), the strip is 12 px
    CHECK_EQ(px(60, 103), kOkT); CHECK_EQ(px(60, 108), kOkT);
    CHECK_EQ(px(60, 100), kBg); CHECK_EQ(px(60, 111), kBg);
    // the setpoint marker: a 6 px cap on the strip's top and bottom edges, the tint between them
    CHECK_EQ(px(g.mark, 100), kFocus); CHECK_EQ(px(g.mark - 2, 102), kFocus); CHECK_EQ(px(g.mark + 3, 100), kFocus);
    CHECK_EQ(px(g.mark - 3, 100), kBg); CHECK_EQ(px(g.mark + 4, 100), kBg);
    CHECK_EQ(px(g.mark, 111), kFocus); CHECK_EQ(px(g.mark, 109), kFocus);
    CHECK_EQ(px(g.mark, 105), kOkT);
    // the needle: 2 px, the full height, drawn over everything
    CHECK_EQ(px(g.needle, 100), kText); CHECK_EQ(px(g.needle + 1, 111), kText); CHECK_EQ(px(g.needle, 105), kText);
    CHECK_EQ(px(100, 99), kBase); CHECK_EQ(px(100, 112), kBase);   // nothing above or below the strip

    // Moving the setpoint repaints only the old and the new marker columns (6 px each); the old one is gone.
    const auto before = snap();
    const auto next = kit::draw_range_gauge(kSurf, 6, 100, 210, 20000, 15000, 3300, 21000, 30000, pos);
    CHECK_EQ(next.mark, 6 + 105); CHECK_EQ(next.needle, pos.needle);
    CHECK_EQ(px(g.mark, 100), kBg); CHECK_EQ(px(g.mark, 111), kBg); CHECK_EQ(px(g.mark, 105), kOkT);
    CHECK_EQ(px(next.mark, 100), kFocus); CHECK_EQ(px(next.mark, 111), kFocus);
    int cols = 0;
    for (int x = 0; x < 240; x++) {
        bool any = false;
        for (int y = 96; y < 116; y++) any = any || g_panel[y * 240 + x] != before[y * 240 + x];
        cols += any;
    }
    CHECK_EQ(cols <= 12, 1);
    // Moving the needle through the marker's place repaints the marker over the track, the needle on top.
    const auto n2 = kit::draw_range_gauge(kSurf, 6, 100, 210, 15000, 15000, 3300, 21000, 30000, next);
    CHECK_EQ(n2.needle, 6 + 105);
    CHECK_EQ(px(n2.needle, 100), kText); CHECK_EQ(px(n2.needle - 2, 100), kFocus);   // the cap's edge beside the needle
    CHECK_EQ(px(next.needle, 105), kOkT);                                             // the old needle column is the tinted range again
    // An unchanged reading writes nothing.
    const auto still = snap();
    kit::draw_range_gauge(kSurf, 6, 100, 210, 15000, 15000, 3300, 21000, 30000, n2);
    CHECK_EQ(changed_in(still, snap(), 0, 240), 0);
}

static void test_hero() {
    Field f;
    kit::HeroSpec h;
    h.digits = &kBig;
    h.x = 196; h.y = 54; h.chars = 5; h.color = kVolt; h.unit = "V";
    lcd::fill(kBase);
    kit::draw_hero(kSurf, f, h, "5.08", true);
    CHECK_EQ(ink_in(196 - 5 * 24, 196, 54, 84, kBase) > 0, 1);   // the digit box
    CHECK_EQ(ink_in(199, 215, 54, 90, kBase) > 0, 1);            // the unit behind it, 3 px gap
    CHECK_EQ(ink_in(197, 199, 54, 90, kBase), 0);                // ... and the gap stays untouched
    CHECK_EQ(ink_in(0, 196 - 5 * 24, 54, 84, kBase), 0);
    // Only the changed characters repaint.
    const auto before = snap();
    kit::draw_hero(kSurf, f, h, "5.09");
    int cols = 0;
    for (int x = 0; x < 240; x++) {
        bool any = false;
        for (int y = 54; y < 90; y++) any = any || g_panel[y * 240 + x] != before[y * 240 + x];
        cols += any;
    }
    CHECK_EQ(cols > 0 && cols <= 24, 1);
    const auto same = snap();
    kit::draw_hero(kSurf, f, h, "5.09");
    CHECK_EQ(changed_in(same, snap(), 0, 240), 0);
}

namespace {
struct ColModel { bool pill = false; };
void col_fn(void *ctx, int i, bool, kit::RowSpec &out) {
    const auto *m = static_cast<ColModel *>(ctx);
    out.marker = kit::Marker::Radio;
    out.text = "Clone 123";
    out.col2 = i == 0 ? std::string_view("50V 5A") : std::string_view();
    if (m->pill && i == 0) { out.tail = "ARMED"; out.tail_color = kCaution; out.tail_tint = kCautionT; }
}
}  // namespace

static void test_col2_and_pill() {
    ColModel m;
    lcd::fill(kBase);
    kit::RowList l;
    l.init(kSurf, kit::kBodyY, col_fn, &m);
    l.set_count(2); l.select(1); l.draw(1);
    const int y0 = kit::kBodyY;
    // col2 sits right of x104; a 9-char name ends by x100, the gap stays clear
    CHECK_EQ(ink_in(104, 150, y0 + 2, y0 + 24, kBg) > 0, 1);
    CHECK_EQ(ink_in(100, 104, y0 + 2, y0 + 24, kBg), 0);
    CHECK_EQ(ink_in(28, 100, y0 + 2, y0 + 24, kBg) > 0, 1);
    // the second row has no col2: its name runs on past x104
    CHECK_EQ(ink_in(110, 190, y0 + 26 + 2, y0 + 26 + 24, kBg) > 0, 1);
    // a pill tail: tint behind the text, right edge at x214
    m.pill = true;
    l.invalidate(); l.draw(2);
    CHECK_EQ(px(177, y0 + 13), kCautionT);
    CHECK_EQ(ink_in(215, 224, y0 + 2, y0 + 24, kBg), 0);
    // same content again paints nothing
    const auto same = snap();
    l.draw(3);
    CHECK_EQ(changed_in(same, snap(), 0, 240), 0);
}

int main() {
    test_col2_and_pill();
    test_result_row();
    test_result_rows_stream_each_pixel_once();
    test_result_rows_cover_their_box_when_scaled();
    test_result_grid();
    test_result_grid_budgeted();
    test_result_grid_sync_dots();
    test_result_grid_sync_dots_while_rows_change_shape();
    test_result_grid_anchor_bottom();
    test_pass_band();
    test_range_gauge();
    test_hero();
    test_tables();
    test_progress_rule();
    test_row_list_geometry();
    test_last_row_opens_over_empty_slot();
    test_row_without_note_stays_26();
    test_row_list_caches();
    test_markers();
    test_tail_and_chevron();
    test_thumb_and_lane();
    test_verdict_cell();
    test_skip_strike();
    test_alert_band();
    test_action_cell();
    test_progress_bar();
    test_kv_and_info();
    return test_report("test_kit");
}
