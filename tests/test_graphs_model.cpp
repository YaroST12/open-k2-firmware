// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Graphs page's pure model: band geometry, the fixed Y scales and their pick lists, the Wh scale, the header and
// span texts, the W limit of a contract and the info line's layout (include/k2/ui/graphs_model.hpp).
#include "test_util.h"
#include "k2/ui/graphs_model.hpp"

#include <string>
#include <string_view>

using namespace k2;
using namespace k2::graphs;

static std::string str(const char *s, int n) { return std::string(s, static_cast<size_t>(n)); }

// Four bands of 54 rows from y0, a 1 px rule between them, the info line's 2 px rule at y219: nothing overlaps.
static void test_band_geometry() {
    CHECK_EQ(band_y0(0), 0);
    CHECK_EQ(band_y1(3), 218);
    for (int k = 0; k < kBands; k++) {
        CHECK_EQ(graph_y0(k), band_y0(k) + 16);          // under the font 16 header row
        CHECK_EQ(graph_y1(k) < band_y1(k), true);
        if (k < kBands - 1) CHECK_EQ(band_y1(k) + 2, band_y0(k + 1));   // the rule on band_y1 + 1
    }
    CHECK_EQ(kInfoRuleY, band_y1(3) + 1);
    CHECK_EQ(kX0 + kCols * kColW - 1, 223);
}

// Landscape (240 x 224 panel pixels): the bands lose rows, the columns spread over 236 px, nothing overlaps.
static void test_landscape_geometry() {
    const Geom g{240, 224};
    CHECK_EQ(g.info_rule_y(), 203);
    CHECK_EQ(g.band_rows(), 50);
    CHECK_EQ(g.graph_rows(), 33);
    CHECK_EQ(g.band_y1(3) + 1, g.info_rule_y());
    CHECK_EQ(g.info_top() + 16, 223);                    // the info line's box ends a row above the bottom
    CHECK_EQ(g.col_x(0), 4);
    CHECK_EQ(g.col_x(kCols), 240);
    for (int c = 0; c < kCols; c++) {
        const int w = g.col_x(c + 1) - g.col_x(c);
        CHECK_EQ(w == 2 || w == 3, true);
    }
    CHECK_EQ(g.right(), 235);
    CHECK_EQ(g.group_x(10), 80);
}

// Fixed 0..max scales from the lab's pick lists (10 mW / 10 mV / 10 mA units, the trend's): W covers the data and every
// limit in the window, V the data + 2 %, A the data + 5 %. Past the last pick the last one holds.
static void test_scale_picks() {
    CHECK_EQ(scale_w10(0, 0), 1000u);                    // 10 W with nothing to show
    CHECK_EQ(scale_w10(6420, 0), 6500u);                 // 64.2 W -> 65 W
    CHECK_EQ(scale_w10(6420, 100), 10000u);              // a 100 W contract in the window -> 100 W
    CHECK_EQ(scale_w10(30000, 0), 24000u);               // over 240 W: the top pick
    CHECK_EQ(scale_v10(2001), 2100u);                    // 20.01 V (+2 % = 20.41 V) -> 21 V
    CHECK_EQ(scale_v10(2060), 2900u);                    // 20.6 V + 2 % > 21 V -> 29 V
    CHECK_EQ(scale_v10(500), 600u);
    CHECK_EQ(scale_a10(321), 350u);                      // 3.21 A + 5 % = 3.37 A -> 3.5 A
    CHECK_EQ(scale_a10(0), 50u);
    CHECK_EQ(scale_a10(2000), 1000u);
}

// The Wh scale starts at the window's first point (0): a nice step (1, 2, 5 x 10^k) of at least a third of the rise,
// never under 5 mWh, and the top on a whole step.
static void test_wh_scale() {
    CHECK_EQ(nice_step(1), 1u);
    CHECK_EQ(nice_step(3), 5u);
    CHECK_EQ(nice_step(11), 20u);
    CHECK_EQ(nice_step(500), 500u);
    CHECK_EQ(nice_step(501), 1000u);
    CHECK_EQ(wh_top(0, 1), 5u);                          // flat: one 5 mWh step
    CHECK_EQ(wh_top(0, 100), 1u);                        // 100 mWh units: one unit
    CHECK_EQ(wh_top(1234, 1), 1500u);                    // step 500 (a third is 411.3): 3 steps
    CHECK_EQ(wh_top(90, 10), 100u);                      // step 50, two steps
}

// Rows above the graph bottom: 0 at zero, h - 1 at the top of the scale, rounded, clamped.
static void test_level() {
    CHECK_EQ(level(0, 2100, 37), 0);
    CHECK_EQ(level(2100, 2100, 37), 36);
    CHECK_EQ(level(2001, 2100, 37), 34);                 // 20.01 / 21 * 36 = 34.3
    CHECK_EQ(level(5000, 2100, 37), 36);
    CHECK_EQ(level(100, 0, 37), 0);
    CHECK_EQ(level(1500000000u, 3000000000u, 37), 18);  // a top above 2^24 stays in 32 bits: half is row 18
    CHECK_EQ(level(0xFFFFFFFFu, 0xFFFFFFFFu, 64), 63);
}

// Whole units only: 10s, 60s, 10m; the Wh band ten times that: 100s, 10m, 1h40m.
static void test_span_text() {
    char b[8];
    CHECK_STR(str(b, span_text(b, 10)).c_str(), "10s");
    CHECK_STR(str(b, span_text(b, 60)).c_str(), "60s");
    CHECK_STR(str(b, span_text(b, 100)).c_str(), "100s");
    CHECK_STR(str(b, span_text(b, 600)).c_str(), "10m");
    CHECK_STR(str(b, span_text(b, 6000)).c_str(), "1h40m");
    CHECK_STR(str(b, span_text(b, 7200)).c_str(), "2h");
}

// The header: a fixed-width value in the band's units, then the contract's ceiling after a slash ("/--" without one).
static void test_header_texts() {
    char b[8];
    CHECK_STR(str(b, value_text(Band::V, 20010, b)).c_str(), "20.01");
    CHECK_STR(str(b, value_text(Band::W, 64200, b)).c_str(), "64.2");
    CHECK_STR(str(b, value_text(Band::A, 3210, b)).c_str(), "3.21");
    CHECK_STR(str(b, value_text(Band::Wh, 1234, b)).c_str(), "1.234");
    CHECK_EQ(value_chars(Band::V), 5);
    CHECK_EQ(value_chars(Band::W), 4);
    StrBuf<8> m;
    max_text(Band::V, 20000, m);   CHECK_STR(m.view().data(), "/20V");
    max_text(Band::W, 99600, m);   CHECK_STR(m.view().data(), "/100W");
    max_text(Band::A, 3250, m);    CHECK_STR(m.view().data(), "/3.25A");
    max_text(Band::A, 0, m);       CHECK_STR(m.view().data(), "/--");
    max_text(Band::Wh, 5000, m);   CHECK_STR(m.view().data(), "");
    CHECK_EQ(std::string_view(unit_text(Band::Wh)) == "Wh", true);
    CHECK_EQ(group_x(5 + 1 + 4), 72);                    // "20.01V/20V" centred on x112
}

// The W limit of a contract: the PD level x its current; under PPS the requested level, so the limit follows the ramp.
static void test_limit_mw() {
    home::ProtoInfo p;
    p.kind = home::ProtoKind::PdFixed; p.mv = 20000; p.ma = 5000; p.max_mv = 20000;
    CHECK_EQ(limit_mw(p), 100000u);
    p.kind = home::ProtoKind::Pps; p.lo_mv = 3300; p.hi_mv = 21000; p.mv = 9000; p.ma = 5000;
    CHECK_EQ(limit_mw(p), 45000u);
    p.mv = 0;
    CHECK_EQ(limit_mw(p), 105000u);                      // no level yet: the range's top
    p.kind = home::ProtoKind::Qc3;
    CHECK_EQ(limit_mw(p), 0u);                           // no current rating: no limit
}

// The info line: the diagnosis word at x4, the protocol row at x52 when the word fits in front of it, else one space after
// the word; the elapsed time right-aligned to x219. The word wins over the protocol, the protocol over the elapsed time.
static void test_info_layout() {
    InfoLayout l = info_layout(4, 8, 4);                 // "Fine", "PD 20V5A", "3:07"
    CHECK_EQ(l.proto_x, 52);
    CHECK_EQ(l.elapsed, true);
    l = info_layout(10, 8, 4);                           // "Hard reset": the protocol moves behind it
    CHECK_EQ(l.proto_x, 4 + 11 * 8);
    CHECK_EQ(l.elapsed, true);
    l = info_layout(16, 14, 5);                          // a long finding: no room for the protocol
    CHECK_EQ(l.proto_x, -1);
    CHECK_EQ(l.elapsed, true);
    l = info_layout(4, 17, 5);                           // "PPS 3.3-21V 3.25A": it keeps its place, the elapsed time goes
    CHECK_EQ(l.proto_x, 52);
    CHECK_EQ(l.elapsed, false);
    l = info_layout(9, 0, 0);                            // "No source"
    CHECK_EQ(l.proto_x, -1);
}

int main() {
    test_band_geometry();
    test_landscape_geometry();
    test_scale_picks();
    test_wh_scale();
    test_level();
    test_span_text();
    test_header_texts();
    test_limit_mw();
    test_info_layout();
    return test_report("test_graphs_model");
}
