// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the UI v2 primitives: pure geometry first, then pixels on the fake panel.
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include <array>
#include <algorithm>
#include <string.h>
#include <string>
#include <vector>

using namespace k2;

static const Surface kScr = {0, 0, 240, 240};
static void clear(uint16_t c = 0) { for (auto &p : g_panel) p = c; }
static uint16_t px(int x, int y) { return g_panel[y * LCD_WIDTH + x]; }

static void test_single_pass_updates() {
    const Surface content{0, 0, 224, 240};
    clear();
    gfx_fill(content, 0, kFooterY, content.w, 240 - kFooterY, kOkT);   // Fieldbook Ok fact line: tint, 4 px bar, ink text
    gfx_fill(content, 0, kFooterY, kFooterBarW, 240 - kFooterY, kOk);
    gfx_text(content, kSmall, kFooterTextX, kFooterY + 1, "All subsystems up", kInk, kOkT);
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> expected;
    std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
    draw_footer(content, FooterTone::Caution, "Another status");
    g_lcd_pixels = g_lcd_windows = 0;
    draw_footer(content, FooterTone::Ok, "All subsystems up");
    CHECK_EQ(g_lcd_pixels, 224u * 18u);
    CHECK_EQ(g_lcd_windows, 1u);
    CHECK_EQ(std::equal(expected.begin(), expected.end(), std::begin(g_panel)), true);

    clear();
    gfx_fill(content, 140, 0, content.w - 140, kBarH - 1, kBar);   // band rows, not the rule row
    gfx_fill(content, kMastVoltKeyX, kMastKeyY, kMastKeyW, kMastKeyH, kSpotVolt);   // each value led by its colour key, ink text
    gfx_fill(content, kMastAmpKeyX, kMastKeyY, kMastKeyW, kMastKeyH, kSpotAmp);
    gfx_text(content, kLabelFont, kMastVoltX, kBarTextY, "20.0V", kInk, kBar);
    gfx_text(content, kLabelFont, kMastAmpX, kBarTextY, "2.99A", kInk, kBar);
    std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
    draw_top_readout(content, StatusReadout::Stale, "9.00V", "");   // the LAST pill, then the live readout again
    draw_top_readout(content, StatusReadout::Live, "20.0V", "2.99A");
    CHECK_EQ(std::equal(expected.begin(), expected.end(), std::begin(g_panel)), true);
}


static void test_geometry() {
    // Hand-computed circle arcs: r=5 cuts 3,1,1,0,0 pixels; r=3 cuts 1,0,0; r<=1 is square.
    const int r5[] = {3, 1, 1, 0, 0};
    for (int i = 0; i < 5; i++) CHECK_EQ(round_inset(5, i), r5[i]);
    const int r3[] = {1, 0, 0};
    for (int i = 0; i < 3; i++) CHECK_EQ(round_inset(3, i), r3[i]);
    CHECK_EQ(round_inset(1, 0), 0);
    CHECK_EQ(round_inset(0, 0), 0);
    CHECK_EQ(round_inset(6, 0), 4);   // 26 px badge corner: centres within 2.4 px of the arc axis
    // Bottom rows mirror the top; a radius larger than half the height is clamped.
    CHECK_EQ(round_row_inset(3, 14, 13), 1);
    CHECK_EQ(round_row_inset(3, 14, 7), 0);
    CHECK_EQ(round_row_inset(5, 4, 0), round_inset(2, 0));
    CHECK_EQ(pill_width(0), 8);
    CHECK_EQ(pill_width(5), 38);
    CHECK_EQ(gauge_fill_px(160, 0, 100), 0);
    CHECK_EQ(gauge_fill_px(160, 50, 100), 80);
    CHECK_EQ(gauge_fill_px(160, 1, 100), 2);      // visible sliver
    CHECK_EQ(gauge_fill_px(160, 300, 100), 160);  // clamped
    CHECK_EQ(gauge_fill_px(160, 5, 0), 0);
    CHECK_EQ(chevron_left(true, 0), 4);
    CHECK_EQ(chevron_left(true, 5), 0);
    CHECK_EQ(chevron_left(false, 0), 0);          // down is the mirror image
    CHECK_EQ(chevron_left(false, 5), 4);
    // Sparkline: sample 3 after 1 at level 2 is the trace, level 0 is fill, level 4 is empty.
    CHECK_EQ((int)spark_cell(2, 3, 1), (int)Cell::Line);
    CHECK_EQ((int)spark_cell(0, 3, 3), (int)Cell::Fill);
    CHECK_EQ((int)spark_cell(4, 3, 3), (int)Cell::None);
    CHECK_EQ((int)spark_cell(3, 3, 3), (int)Cell::Line);
}

static void test_fill_round() {
    clear();
    fill_round(kScr, 10, 20, 14, 14, 3, 0x1234);
    CHECK_EQ(px(10, 20), 0);          // corner pixel cut
    CHECK_EQ(px(11, 20), 0x1234);
    CHECK_EQ(px(10, 21), 0x1234);
    CHECK_EQ(px(23, 20), 0);          // right corner cut
    CHECK_EQ(px(22, 20), 0x1234);
    CHECK_EQ(px(10, 33), 0);          // bottom-left cut
    CHECK_EQ(px(17, 27), 0x1234);
    CHECK_EQ(px(24, 27), 0);          // nothing outside
    CHECK_EQ(px(17, 34), 0);
}

static void test_frame_round() {
    clear();
    frame_round(kScr, 0, 0, 26, 26, 6, 2, 0xAAAA);
    CHECK_EQ(px(13, 0), 0xAAAA);      // top edge, 2 px thick
    CHECK_EQ(px(13, 1), 0xAAAA);
    CHECK_EQ(px(13, 2), 0);           // hollow inside
    CHECK_EQ(px(0, 13), 0xAAAA);
    CHECK_EQ(px(1, 13), 0xAAAA);
    CHECK_EQ(px(2, 13), 0);
    CHECK_EQ(px(25, 13), 0xAAAA);
    CHECK_EQ(px(13, 25), 0xAAAA);
    CHECK_EQ(px(0, 0), 0);            // rounded corner
    clear();
    frame_round(kScr, 0, 0, 18, 8, 3, 1, 0xBBBB);
    CHECK_EQ(px(9, 0), 0xBBBB);
    CHECK_EQ(px(9, 1), 0);
    CHECK_EQ(px(0, 4), 0xBBBB);
    CHECK_EQ(px(1, 4), 0);
}

static void test_icon() {
    clear(0x0001);
    draw_icon(kScr, 5, 7, Icon::Check, 0xFFFF, 0x0001);
    // check mask row 1 is "...........#": only the rightmost pixel is set.
    CHECK_EQ(px(5 + 11, 7 + 1), 0xFFFF);
    CHECK_EQ(px(5 + 10, 7 + 1), 0x0001);
    // row 4 is "#.......##..": pixels 0, 8, 9.
    CHECK_EQ(px(5, 7 + 4), 0xFFFF);
    CHECK_EQ(px(5 + 1, 7 + 4), 0x0001);
    CHECK_EQ(px(5 + 8, 7 + 4), 0xFFFF);
    CHECK_EQ(px(5 + 9, 7 + 4), 0xFFFF);
    CHECK_EQ(px(5 + 12, 7 + 4), 0x0001);  // outside the box: the clear colour
    CHECK_EQ(kIconCount, 28);
    CHECK_EQ(static_cast<int>(Icon::Battery) + 1, kIconCount);   // Battery is the last icon
}

// The fact line's tone table (Fieldbook): Info = the ground with a rule on top and ink2 text; Ok / Caution = the tint with
// a 4 px bar of the tone and ink text; Error = solid red with the on-red ink.
static void test_footer_tones() {
    struct Row { FooterTone tone; uint16_t fill; uint16_t text; uint16_t bar; };
    const Row kTable[] = {
        {FooterTone::Info, kBg, kInk2, 0},
        {FooterTone::Ok, kOkT, kInk, kOk},
        {FooterTone::Caution, kCautionT, kInk, kCaution},
        {FooterTone::Error, kError, kErrInk, 0},
    };
    for (const Row &r : kTable) {
        const FooterStyle s = footer_style(r.tone);
        CHECK_EQ(s.fill, r.fill);
        CHECK_EQ(s.text, r.text);
        CHECK_EQ(s.bar, r.bar);
    }
    CHECK_EQ(footer_style(FooterTone::Info).rule, kRule);
    // Only the Ok tone may carry the ok fill or the ok bar.
    for (FooterTone t : {FooterTone::Info, FooterTone::Caution, FooterTone::Error}) {
        CHECK_EQ(footer_style(t).fill == kOkT, false);
        CHECK_EQ(footer_style(t).bar == kOk, false);
    }
    // footer_tone_of maps a status colour a caller holds; anything else is neutral.
    CHECK_EQ(static_cast<int>(footer_tone_of(kOk)), static_cast<int>(FooterTone::Ok));
    CHECK_EQ(static_cast<int>(footer_tone_of(kCaution)), static_cast<int>(FooterTone::Caution));
    CHECK_EQ(static_cast<int>(footer_tone_of(kError)), static_cast<int>(FooterTone::Error));
    CHECK_EQ(static_cast<int>(footer_tone_of(kDim)), static_cast<int>(FooterTone::Info));
    CHECK_EQ(static_cast<int>(footer_tone_of(kVolt)), static_cast<int>(FooterTone::Info));
    // The drawn band carries the bar (or the fill) at its left edge and the fill under the right margin, with no icon.
    for (const Row &r : kTable) {
        clear(0x0003);
        draw_footer(kScr, r.tone, "State. Action");
        CHECK_EQ(px(1, 230), r.bar ? r.bar : r.fill);
        CHECK_EQ(px(6, 230), r.fill);
        CHECK_EQ(px(223, 239), r.fill);
    }
}

static void test_pill() {
    clear();
    int w = draw_pill(kScr, 20, 30, "PD3.0", kEnergy, kEnergyT, false);
    CHECK_EQ(w, 38);
    CHECK_EQ(px(20, 30), 0);                 // r=3 corner cut
    CHECK_EQ(px(21, 30), kEnergyT);          // tint fill
    CHECK_EQ(px(20 + 42, 30 + 13) , 0);      // bottom-right corner cut
    CHECK_EQ(px(20 + 43, 30 + 5), 0);        // nothing beyond the width
    clear();
    draw_pill(kScr, 20, 30, "CC1", kVolt, kVoltT, true);
    CHECK_EQ(px(21, 30), kVolt);             // solid fill
    clear();
    draw_pill(kScr, 20, 30, "A", kAmp, kAmpT, false);
    bool glyph = false;                      // some pixel carries the text colour
    for (int y = 30; y < 44; y++) for (int x = 20; x < 20 + pill_width(1); x++) if (px(x, y) == kAmp) glyph = true;
    CHECK_EQ(glyph, 1);
}

static void test_gauge() {
    clear();
    draw_gauge(kScr, 10, 50, 100, 3, 40, kAmp);
    CHECK_EQ(px(10, 50), kAmp);
    CHECK_EQ(px(49, 52), kAmp);
    CHECK_EQ(px(50, 51), kRule);
    CHECK_EQ(px(109, 50), kRule);
    CHECK_EQ(px(110, 50), 0);
    // Shrinking repaints the vacated span as track without redrawing the rest.
    draw_gauge(kScr, 10, 50, 100, 3, 20, kAmp, 40);
    CHECK_EQ(px(29, 51), kAmp);
    CHECK_EQ(px(30, 51), kRule);
    CHECK_EQ(px(49, 51), kRule);
    // Growing paints only the new span in the fill colour.
    draw_gauge(kScr, 10, 50, 100, 3, 60, kAmp, 20);
    CHECK_EQ(px(69, 51), kAmp);
    CHECK_EQ(px(70, 51), kRule);
}

static void test_triangle() {
    // Height 9: the width grows 1..5 down to the middle row and shrinks again; right points right.
    CHECK_EQ(tri_row_w(9, 0), 1);
    CHECK_EQ(tri_row_w(9, 4), 5);
    CHECK_EQ(tri_row_w(9, 8), 1);
    CHECK_EQ(tri_row_w(9, 6), 3);
    CHECK_EQ(tri_width(9), 5);
    clear();
    draw_triangle(kScr, 50, 60, 9, true, kVolt);
    CHECK_EQ(px(50, 60), kVolt);             // top row: one pixel at the base
    CHECK_EQ(px(51, 60), 0);
    CHECK_EQ(px(54, 64), kVolt);             // apex on the middle row
    CHECK_EQ(px(55, 64), 0);
    CHECK_EQ(px(50, 68), kVolt);
    CHECK_EQ(px(51, 68), 0);
    CHECK_EQ(px(50, 69), 0);                 // 9 rows only
    clear();
    draw_triangle(kScr, 50, 60, 9, false, kVolt);   // points left: the base is on the right
    CHECK_EQ(px(54, 60), kVolt);
    CHECK_EQ(px(53, 60), 0);
    CHECK_EQ(px(50, 64), kVolt);
    CHECK_EQ(px(49, 64), 0);
}

static void test_chevron_ports() {
    clear();
    draw_chevron(kScr, 100, 100, true, kFlow);
    CHECK_EQ(px(104, 100), kFlow);           // apex
    CHECK_EQ(px(105, 100), kFlow);
    CHECK_EQ(px(103, 100), 0);
    CHECK_EQ(px(100, 104), kFlow);           // arm end, row 4
    CHECK_EQ(px(109, 104), kFlow);
    CHECK_EQ(px(105, 104), 0);               // hollow between the arms
    CHECK_EQ(px(100, 106), 0);               // 6 rows only
    clear();
    draw_chevron(kScr, 100, 100, false, kFlow);
    CHECK_EQ(px(100, 100), kFlow);           // down: arms on top, apex at the bottom
    CHECK_EQ(px(104, 105), kFlow);
    CHECK_EQ(px(100, 105), 0);

    clear();
    draw_port_male(kScr, 10, 10, kText, kBg);
    CHECK_EQ(px(10, 10), 0);                 // r=3 corner
    CHECK_EQ(px(11, 10), kText);
    CHECK_EQ(px(8 + 10, 10 + 3), kBg);       // slot
    CHECK_EQ(px(10, 14), kText);
    clear();
    draw_port_female(kScr, 10, 10, kText);
    CHECK_EQ(px(19, 10), kText);             // top edge
    CHECK_EQ(px(19, 11), 0);                 // hollow
    CHECK_EQ(px(14, 13), kText);             // tongue
}

static void test_sparkline() {
    uint8_t s[8] = {0, 0, 2, 4, 4, 3, 3, 1};
    clear(0x0002);
    draw_sparkline(kScr, 30, 60, 8, 5, s, kVolt, kVoltT, kBg);
    // Column 3 has height 4: top row (level 4) is the trace, level 3..0 beneath it is the trace
    // join from column 2 (2->4 covers levels 2..4), so level 1 is fill.
    CHECK_EQ(px(30 + 3, 60), kVolt);         // level 4
    CHECK_EQ(px(30 + 3, 60 + 3), kVoltT);    // level 1: fill
    CHECK_EQ(px(30 + 3, 60 + 4), kVoltT);    // level 0: fill
    CHECK_EQ(px(30 + 0, 60 + 4), kVolt);     // flat zero trace on the bottom row
    CHECK_EQ(px(30 + 0, 60), kBg);           // above the trace: background (or a grid line)
    CHECK_EQ(px(30 + 8, 60), 0x0002);        // outside the box untouched
}

static void test_range_trace() {
    CHECK_EQ(range_row(0, 20), 19);          // bottom
    CHECK_EQ(range_row(255, 20), 0);         // top
    uint8_t lo[4] = {0, 128, 255, 255}, hi[4] = {0, 255, 255, 0};   // col 3: lo > hi, no data
    clear(0x0002);
    draw_range_trace(kScr, 30, 60, 4, 2, 21, lo, hi, 2, kOk, kSparkNone, kError, kVolt, kBg);
    CHECK_EQ(px(30, 60 + 20), kVolt);        // column 0: one pixel at the bottom
    CHECK_EQ(px(31, 60 + 20), kVolt);        // doubled in x
    CHECK_EQ(px(30, 60 + 19), kBg);          // empty above (row 19 is not a grid row of h=21)
    CHECK_EQ(px(32, 60), kVolt);             // column 1: band from the middle to the top
    CHECK_EQ(px(32, 60 + 10), kVolt);
    CHECK_EQ(px(32, 60 + 12), kBg);
    CHECK_EQ(px(34, 60 + 10), kOk);          // the marked column outside its band
    CHECK_EQ(px(34, 60), kVolt);             // inside the band the trace wins
    CHECK_EQ(px(36, 60 + 3), kBg);           // no-data column draws nothing
    CHECK_EQ(px(38, 60), 0x0002);            // outside the box untouched
}

static void test_footer() {
    clear(0x0003);
    draw_footer(kScr, FooterTone::Ok, "All subsystems up");
    CHECK_EQ(px(100, kFooterY), kOkT);            // band y 222..239, full content width, filled by the tone
    CHECK_EQ(px(223, 239), kOkT);
    CHECK_EQ(px(0, kFooterY - 1), 0x0003);        // nothing above the band
    bool text = false, bar = true, gap = false;
    for (int y = kFooterY; y < 240; y++) {
        for (int x = 0; x < kFooterBarW; x++) bar &= px(x, y) == kOk;
        for (int x = kFooterBarW; x < kFooterTextX; x++) if (px(x, y) != kOkT) gap = true;
        for (int x = kFooterTextX; x < 223; x++) if (px(x, y) != kOkT) text = true;
    }
    CHECK_EQ(bar, 1);                             // the tone bar x0..3, no icon
    CHECK_EQ(gap, 0);                             // then plain fill up to the text
    CHECK_EQ(text, 1);                            // the text starts at x 8
    bool early = false;
    for (int y = kFooterY; y < 240; y++) for (int x = 8; x < 16; x++) if (px(x, y) != kOkT) early = true;
    CHECK_EQ(early, 1);
}

static void test_footer_fit() {
    // Too long for the band: it is cut with "..." and nothing is painted past the content edge. (This is the device's last
    // resort; the fixture is deliberately over long, so it is outside the no-truncation gate.)
    trunc::Pause allowed;
    const char *longs[] = {"No VBUS. Plug a source in and wait for the PD contract to settle",
                           "Unload, then add 1A to the dummy load on the other port"};
    for (const char *t : longs) {
        clear(0x0003);
        draw_footer(kScr, FooterTone::Info, t);
        int last = -1;
        for (int y = kFooterY + 1; y < 240; y++) for (int x = 0; x < 224; x++) if (px(x, y) != kBg) last = x > last ? x : last;
        CHECK_EQ(last >= 100, 1);
        CHECK_EQ(last < 224, 1);
    }
    // A text that fits is untouched: 26 characters at x 8 end near x 216.
    clear(0x0003);
    draw_footer(kScr, FooterTone::Info, "D+/D- dividers: Apple 2.4A");
    int last = -1;
    for (int y = kFooterY + 1; y < 240; y++) for (int x = 20; x < 224; x++) if (px(x, y) != kBg) last = x > last ? x : last;
    CHECK_EQ(last > 194 && last < 218, 1);
}

static void test_role_badge() {
    char buf[11];
    CHECK_STR(std::string(role_badge(0, buf)).c_str(), "PAS");        // passive inline, no pin
    CHECK_STR(std::string(role_badge(1, buf)).c_str(), "Rd pin CC1");
    CHECK_STR(std::string(role_badge(2, buf)).c_str(), "Rd pin CC2");  // a leftover lab pin names its line
    CHECK_STR(std::string(role_badge(3, buf)).c_str(), "PAS");         // never both lines: anything else is passive
    clear(0x0004);
    draw_status_left(kScr, false, Icon::Bolt, "Home", 132, role_badge(2, buf));   // the badge fits the bar
    bool badge_ink = false;
    for (int y = 0; y < kBarH - 1; y++) for (int x = 67; x < 132; x++) if (px(x, y) == kDim) badge_ink = true;
    CHECK_EQ(badge_ink, 1);
}

static void test_status_bar() {
    CHECK_EQ(kBarH, 21);                       // 20 px masthead + the 1 px accent rule below it

    clear(0x0004);
    draw_status_left(kScr, false, Icon::Bolt, "Home");
    bool volt = false;
    for (int y = 0; y < kBarH - 1; y++) for (int x = kMastTitleX; x < 48; x++) if (px(x, y) == kInk) volt = true;
    CHECK_EQ(volt, 1);                         // the page name on the left
    CHECK_EQ(px(100, 1), kBar);                // the whole band is the bar colour
    bool hairline = true;
    for (int x = 0; x < 224; x++) hairline &= px(x, kBarH - 1) == kAccent;
    CHECK_EQ(hairline, 1);                     // the accent rule below the masthead
    CHECK_EQ(px(100, kBarH), 0x0004);          // nothing below the rule

    clear(0x0004);
    std::array<char, 8> v5{}, v3{};            // fixed slots: same start x for any value
    draw_status_left(kScr, false, Icon::Bolt, "X");   // the rule belongs to the title pass
    draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A");
    volt = false;
    bool amp = false, vkey = true, akey = true;
    for (int y = 0; y < kBarH; y++) {
        for (int x = kMastVoltX; x < kMastVoltX + 30; x++) if (px(x, y) == kInk) volt = true;
        for (int x = kMastAmpX; x < kMastAmpX + 30; x++) if (px(x, y) == kInk) amp = true;
    }
    for (int y = kMastKeyY; y < kMastKeyY + kMastKeyH; y++) for (int x = 0; x < kMastKeyW; x++) {
        vkey &= px(kMastVoltKeyX + x, y) == kSpotVolt;   // each value led by its colour key
        akey &= px(kMastAmpKeyX + x, y) == kSpotAmp;
    }
    CHECK_EQ(volt, 1); CHECK_EQ(amp, 1); CHECK_EQ(vkey, 1); CHECK_EQ(akey, 1);
    hairline = true;
    for (int x = 140; x < 224; x++) hairline &= px(x, kBarH - 1) == kAccent;
    CHECK_EQ(hairline, 1);                     // the readout passes never touch the rule row
    CHECK_EQ(px(100, 0), kBar);                // the left of the bar belongs to the title

    clear(0x0004);
    draw_top_readout(kScr, StatusReadout::Stale, "20.0V", "1.52A");
    bool stale = false, pill = false;
    for (int y = 0; y < kBarH; y++) {
        for (int x = 140; x < 224; x++) {
            if (px(x, y) == kStale) stale = true;
        }
        for (int x = 190; x < 218; x++) if (px(x, y) == kSpotVolt) pill = true;
    }
    CHECK_EQ(stale, 1);                        // the LAST pill in the stale grey, right-aligned
    CHECK_EQ(pill, 0);                         // the frozen readout makes room and hides

    clear(0x0004);
    draw_top_readout(kScr, StatusReadout::None, "", "");
    bool any = false;
    for (int x = 140; x < 224; x++) for (int y = 0; y < kBarH - 1; y++) if (px(x, y) != kBar) any = true;
    CHECK_EQ(any, 0);
}

// The status bar's direction symbol: one static physical chevron in the slot x 140..149 beside the
// volts. Live and not idle only; never in the stale or empty bar. Position and shape follow the
// rotation (a quarter turn is 6 wide x 10 tall); the pixels equal the shared row mask.
static void test_status_bar_flow() {
    for (uint8_t r = 0; r < 4; ++r) for (Arrow a : {Arrow::Up, Arrow::Down}) {
        clear(0x0004);
        draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", a, r);
        const bool toward_top = a == Arrow::Up;
        const int w = physical_chevron_w(r), h = physical_chevron_h(r);
        const int x0 = 140 + (10 - w) / 2, y0 = kBarTextY + (12 - h) / 2;
        int n = 0;
        for (int y = 0; y < kBarH - 1; y++) for (int x = 140; x < 152; x++) {
            const bool in = x >= x0 && x < x0 + w && y >= y0 && y < y0 + h;
            const bool mask = in && (physical_chevron_row(toward_top, r, y - y0) >> (x - x0) & 1);
            CHECK_EQ(px(x, y) == kInk, mask);
            n += mask;
        }
        CHECK_EQ(n, 22);                        // apex row 2 px plus five rows of 4 px: the same shape at every rotation
    }
    // Rotation 0: "up" has its apex on the top row, "down" on the bottom row.
    clear(0x0004);
    draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", Arrow::Up, 0);
    int top = 0, bot = 0;
    for (int x = 140; x < 150; x++) { top += px(x, 7) == kInk; bot += px(x, 12) == kInk; }
    CHECK_EQ(top, 2); CHECK_EQ(bot, 4);
    clear(0x0004);
    draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", Arrow::Down, 0);
    top = bot = 0;
    for (int x = 140; x < 150; x++) { top += px(x, 7) == kInk; bot += px(x, 12) == kInk; }
    CHECK_EQ(top, 4); CHECK_EQ(bot, 2);
    // Idle, stale and empty bars draw no direction.
    for (StatusReadout m : {StatusReadout::Live, StatusReadout::Stale, StatusReadout::None}) {
        clear(0x0004);
        draw_top_readout(kScr, m, "20.0V", "1.52A", m == StatusReadout::Live ? Arrow::Idle : Arrow::Up, 0);
        int flow = 0;
        for (int y = 0; y < kBarH; y++) for (int x = 140; x < 150; x++) flow += px(x, y) == kInk;
        CHECK_EQ(flow, 0);
    }
}

// The live readout through ReadoutShown repaints only the cells that changed: volts cells at kVoltX + 6i, amps at
// kAmpX + 6i (right-aligned), chevron slot x 140..149. The pixels end equal to a whole repaint of the new reading.
static void test_readout_delta() {
    ReadoutShown a, b;
    ReadoutShown::set(a.volts, "20.0V"); ReadoutShown::set(a.amps, "1.52A");
    b = a;
    CHECK_EQ(readout_delta(a, b).n, 0);                    // the same reading: nothing to send
    ReadoutShown::set(b.volts, "20.1V");                   // one digit: cell 3 of the volts slot, kVoltX+18..+24
    ReadoutDelta d = readout_delta(a, b);
    CHECK_EQ(d.n, 1);
    CHECK_EQ(d.span[0].x0, ReadoutShown::kVoltX + 18); CHECK_EQ(d.span[0].x1, ReadoutShown::kVoltX + 24);
    ReadoutShown::set(b.amps, "1.6A");                     // shorter (right-aligned): cells 0..3 ("1.52A" -> " 1.6A")
    b.flow = Arrow::Up;
    d = readout_delta(a, b);
    CHECK_EQ(d.n, 3);
    CHECK_EQ(d.span[0].x0, ReadoutShown::kChevX); CHECK_EQ(d.span[0].x1, ReadoutShown::kChevX + 10);
    CHECK_EQ(d.span[2].x0, ReadoutShown::kAmpX); CHECK_EQ(d.span[2].x1, ReadoutShown::kAmpX + 24);
    CHECK_EQ(ReadoutShown::set(b.volts, "123456"), false);       // does not fit 5 cells
    CHECK_EQ(ReadoutShown::set(b.volts, "\xC3\x97"), false);     // a multi-byte character

    // Pixels: whole paint of reading 1, delta to reading 2 == whole paint of reading 2; only the changed cells were sent.
    for (uint8_t r = 0; r < 4; ++r) {
        clear(0x0004);
        draw_status_left(kScr, false, Icon::Bolt, "X");
        ReadoutShown shown;
        draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", Arrow::Idle, r, shown);
        CHECK_EQ(shown.valid, true);
        const unsigned before = g_lcd_pixels;
        draw_top_readout(kScr, StatusReadout::Live, "20.1V", "1.6A", Arrow::Down, r, shown);
        CHECK_EQ(g_lcd_pixels - before, (10u + 6u + 24u) * 12u);   // three spans, the 12 glyph rows only
        std::vector<uint16_t> got(g_panel, g_panel + 240 * 240);
        clear(0x0004);
        draw_status_left(kScr, false, Icon::Bolt, "X");
        draw_top_readout(kScr, StatusReadout::Live, "20.1V", "1.6A", Arrow::Down, r);
        int diff = 0;
        for (int i = 0; i < 240 * 240; i++) diff += got[i] != g_panel[i];
        CHECK_EQ(diff, 0);
    }
    // Not live, or another rotation: the whole band (kChevX..kScr.w x (kBarH - 1), rows above the hairline).
    ReadoutShown shown;
    draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", Arrow::Idle, 0, shown);
    unsigned before = g_lcd_pixels;
    draw_top_readout(kScr, StatusReadout::Live, "20.0V", "1.52A", Arrow::Idle, 1, shown);
    CHECK_EQ(g_lcd_pixels - before, static_cast<unsigned>((kScr.w - ReadoutShown::kChevX) * (kBarH - 1)));
    before = g_lcd_pixels;
    draw_top_readout(kScr, StatusReadout::Stale, "20.0V", "1.52A", Arrow::Idle, 1, shown);
    CHECK_EQ(g_lcd_pixels - before, static_cast<unsigned>((kScr.w - ReadoutShown::kChevX) * (kBarH - 1)));
}

// Tool template: the flow diagram column and the checklist card (geometry, then pixels).
static void test_tool_widgets() {
    // Geometry of the 72 px diagram: ports 5 px inside the card, roles beside them, the line between.
    const FlowGeom g = flow_geom(72);
    CHECK_EQ(g.top_port_y, 5);
    CHECK_EQ(g.bottom_port_y, 59);
    CHECK_EQ(g.line_y, 17);
    CHECK_EQ(g.line_h, 38);
    CHECK_EQ(g.caption_y, 30);
    CHECK_EQ(flow_geom(96).bottom_port_y, 83);   // a taller card keeps the top and moves the bottom
    CHECK_EQ(flow_chevrons(72), 4);
    CHECK_EQ(flow_chevrons(96), 7);
    CHECK_EQ(checklist_icon_y(0), 6);
    CHECK_EQ(checklist_text_y(2), 48);
    CHECK_EQ(checklist_text_y(1) - checklist_text_y(0), 22);

    clear();
    FlowSpec f;
    f.top_role = "K2"; f.bottom_role = "DEV";
    f.top_color = kFocus; f.top_tint = kFocusT; f.bottom_color = kOk; f.bottom_tint = kOkT;
    f.flow = Arrow::Idle; f.caption = "VCON"; f.caption_color = kFocus;
    draw_flow_diagram(kScr, 0, 22, 72, f);
    CHECK_EQ(px(1, 40), kSurface);              // the card
    CHECK_EQ(px(2, 22 + 3), kSurface);          // corner inset
    CHECK_EQ(px(14, 22 + 17 + 3), kFaint);      // the no-flow line (Idle)
    CHECK_EQ(px(12, 22 + 5 + 1), kFocus);       // male plug body (top port at x 6, y 27)
    CHECK_EQ(px(6, 22 + 59 + 3), kOk);          // female port frame, left edge
    CHECK_EQ(px(30, 22 + 2 + 5), kFocusT);      // top role pill fill, between its text glyphs

    clear();
    f.flow = Arrow::Up; f.caption = "";
    draw_flow_diagram(kScr, 0, 22, 72, f);
    bool chev = false, line = false;
    for (int y = 22 + 17; y < 22 + 55; y++)
        for (int x = 8; x < 22; x++) { if (px(x, y) == kFlow) chev = true; if (px(x, y) == kFaint) line = true; }
    CHECK_EQ(chev, 1);
    CHECK_EQ(line, 0);

    clear();
    const CheckItem it[3] = {{"Device seen", true}, {"No charger", false}, {"No live sink", false}};
    draw_checklist(kScr, 68, 22, 156, 72, kCaution, it, 3);
    CHECK_EQ(px(69, 22 + 2), kSurface);
    CHECK_EQ(px(69, 22 + 4 + 2), kCaution);     // accent bar x 68, y 26 .. 89
    CHECK_EQ(px(70, 22 + 4 + 2), kCaution);
    CHECK_EQ(px(71, 22 + 4 + 2), kSurface);
    bool ok = false, bad = false;
    for (int y = 22 + 6; y < 22 + 18; y++)
        for (int x = 76; x < 88; x++) if (px(x, y) == kOk) ok = true;
    for (int y = 22 + 28; y < 22 + 40; y++)
        for (int x = 76; x < 88; x++) if (px(x, y) == kCaution) bad = true;
    CHECK_EQ(ok, 1);
    CHECK_EQ(bad, 1);
    // A met row is dim, an open one is full-white text.
    bool dim = false, white = false;
    for (int y = 22 + 4; y < 22 + 21; y++) for (int x = 92; x < 150; x++) { if (px(x, y) == kDim) dim = true; if (px(x, y) == kText) white = true; }
    CHECK_EQ(dim, 1);
    CHECK_EQ(white, 0);
    for (int y = 22 + 26; y < 22 + 43; y++) for (int x = 92; x < 150; x++) if (px(x, y) == kText) white = true;
    CHECK_EQ(white, 1);

    // Step tile: 22 x 22, done = ok fill, to do = caution fill, the number on it.
    clear();
    draw_step_tile(kScr, 6, 30, 2, false);
    CHECK_EQ(px(6 + 11, 30 + 2), kCaution);
    CHECK_EQ(px(6, 30), 0);                      // rounded corner
    clear();
    draw_step_tile(kScr, 6, 30, 1, true);
    CHECK_EQ(px(6 + 11, 30 + 2), kOk);
}

static void test_fit_text() {
    trunc::Pause pause;   // the cuts here are the point of the test, not copy that overflowed
    char buf[kFitBuf];
    CHECK_EQ(fit_text(kLabelFont, "short", 60, buf) == std::string_view("short"), 1);
    // 60 px = 10 cells of 6 px: ten characters fit as they are, eleven become seven plus "...".
    CHECK_EQ(fit_text(kLabelFont, "0123456789", 60, buf) == std::string_view("0123456789"), 1);
    CHECK_EQ(fit_text(kLabelFont, "01234567890", 60, buf) == std::string_view("0123456..."), 1);
    CHECK_EQ(fit_text(kLabelFont, "0123456789", 12, buf) == std::string_view("0..."), 0);  // 2 cells: dots only, never past the width
    CHECK_EQ(fit_text(kLabelFont, "0123456789", 12, buf).size(), 2);
    CHECK_EQ(fit_text(kLabelFont, "abc", 0, buf).size(), 0);
}

// Middle truncation: a name that does not fit keeps its start and its last four characters, so two long
// names that differ only at the end stay distinguishable (F7).
static void test_fit_text_mid() {
    char buf[kFitBuf];
    auto mid = [&](std::string_view s, int px) { return std::string(fit_text_mid(kLabelFont, s, px, buf)); };
    CHECK_EQ(mid("short", 60) == "short", 1);
    CHECK_EQ(mid("0123456789", 60) == "0123456789", 1);               // exactly fits: untouched
    // 60 px = 10 cells: 4 head + ".." + 4 tail.
    CHECK_STR(mid("Very long name b987", 60).c_str(), "Very..b987");
    CHECK_STR(mid("Very long name b988", 60).c_str(), "Very..b988");   // the tails differ, so the results differ
    CHECK_EQ(mid("Very long name b987", 60) != mid("Very long name b988", 60), 1);
    // The result never exceeds the width, whatever the cell count.
    for (int px = 0; px <= 90; px += 6) {
        const std::string r = mid("Anker 735 Charger Prime Edition", px);
        CHECK_EQ(static_cast<int>(r.size()) <= px / 6, 1);
    }
    // Too narrow for head + dots + tail: falls back to the plain start-and-dots cut.
    CHECK_STR(mid("0123456789", 36).c_str(), "012...");
    // UTF-8: whole sequences only, tail starts on a character boundary.
    CHECK_STR(mid("\xC3\xA4\xC3\xB6\xC3\xBC-long-name-\xC3\xA4\xC3\xB6\xC3\xBC", 60).c_str(),
              "\xC3\xA4\xC3\xB6\xC3\xBC-..-\xC3\xA4\xC3\xB6\xC3\xBC");
}

int main() {
    test_single_pass_updates();
    test_fit_text();
    test_fit_text_mid();
    test_footer_tones();
    test_role_badge();
    test_status_bar();
    test_status_bar_flow();
    test_readout_delta();
    test_footer();
    test_footer_fit();
    test_geometry();
    test_fill_round();
    test_frame_round();
    test_icon();
    test_pill();
    test_gauge();
    test_triangle();
    test_chevron_ports();
    test_sparkline();
    test_range_trace();
    test_tool_widgets();
    return test_report("test_ui_widgets");
}
