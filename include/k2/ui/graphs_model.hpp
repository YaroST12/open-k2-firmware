// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Graphs page's pure model (lab runs/ui-home/graphs-lab.html v5, approved 2026-11-14): band geometry, the fixed Y
// scales, the Wh scale, the header and span texts, the W limit of a contract and the info line's layout. No hardware,
// no drawing: tests/test_graphs_model.cpp covers it.
//
// Logical content 224 x 240. Four stacked full-width bands from y0 (W, V, A, Wh), 54 rows each with a 1 px rule between
// them; each band is a header row (font 16 line box, 16 rows) over its graph (x4..223, 110 columns of 2 px). The info line
// sits behind a 2 px rule at y219.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include "k2/ui/home_model.hpp"
#include "k2/ui/trend.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2::graphs {

enum class Band : uint8_t { W, V, A, Wh };
inline constexpr int kBands = 4;

// ---- geometry ---------------------------------------------------------------------------------
// The page draws in panel pixels (no ContentViewport stretch, so a 1 px line or rule never falls between two mapped rows):
// Geom lays the bands out on the content's panel size, 224 x 240 upright, 240 x 224 in the landscape rotations, where the
// bands lose rows (graphs of 33 instead of 37) and the 110 columns spread over 236 px (2 or 3 px each).
inline constexpr int kCols = trend::Trend::kPoints;          // 110 points
inline constexpr int16_t kX0 = 4, kColW = 2;                 // graph x4..223 upright
inline constexpr int16_t kHeadRows = 16;                     // the header row: one font 16 line box
inline constexpr int16_t kInfoRows = 21;                     // 2 px rule, 2 ground rows, the font 16 line, 1 ground row
inline constexpr int16_t kLabelBase = 11, kHeadBase = 12;    // baselines under the band top: font 12 label / span, font 16 group
inline constexpr int16_t kLabelX = 4, kWordX = 4, kProtoX = 52;

struct Geom {
    int16_t w = 224, h = 240;   // the content in panel pixels
    constexpr int16_t info_rule_y() const { return static_cast<int16_t>(h - kInfoRows); }
    constexpr int16_t info_top() const { return static_cast<int16_t>(h - 17); }
    constexpr int16_t band_rows() const { return static_cast<int16_t>((info_rule_y() - (kBands - 1)) / kBands); }
    constexpr int16_t band_y0(int k) const { return static_cast<int16_t>(k * (band_rows() + 1)); }
    constexpr int16_t band_y1(int k) const { return static_cast<int16_t>(band_y0(k) + band_rows() - 1); }
    constexpr int16_t graph_y0(int k) const { return static_cast<int16_t>(band_y0(k) + kHeadRows); }
    constexpr int16_t graph_y1(int k) const { return static_cast<int16_t>(band_y1(k) - 1); }   // a ground row above the rule
    constexpr int16_t graph_rows() const { return static_cast<int16_t>(band_rows() - kHeadRows - 1); }
    constexpr int16_t graph_w() const { return static_cast<int16_t>(w - kX0); }
    // Left x of column c (0..kCols; kCols = one past the last column).
    constexpr int16_t col_x(int c) const { return static_cast<int16_t>(kX0 + c * graph_w() / kCols); }
    constexpr int16_t right() const { return static_cast<int16_t>(w - 5); }   // right-aligned texts end here (x219 upright)
    constexpr int16_t centre() const { return static_cast<int16_t>(w / 2); }
    // Left x of a font 16 group of a number of characters, centred on the content width.
    constexpr int16_t group_x(int chars) const { return static_cast<int16_t>(centre() - chars * 8 / 2); }
};
inline constexpr Geom kUpright{};
constexpr int16_t band_y0(int k) { return kUpright.band_y0(k); }
constexpr int16_t band_y1(int k) { return kUpright.band_y1(k); }
constexpr int16_t graph_y0(int k) { return kUpright.graph_y0(k); }
constexpr int16_t graph_y1(int k) { return kUpright.graph_y1(k); }
inline constexpr int16_t kInfoRuleY = kUpright.info_rule_y(), kInfoTop = kUpright.info_top();
inline constexpr int16_t kSpanRight = kUpright.right(), kElRight = kUpright.right(), kCentreX = kUpright.centre();
static_assert(kUpright.band_rows() == 54 && band_y0(1) == 55, "four 54-row bands, a 1 px rule between them");
static_assert(kInfoRuleY == 219 && kInfoTop == 223 && kSpanRight == 219 && kCentreX == 112, "the lab's upright layout");
static_assert(kUpright.col_x(kCols) - 1 == 223 && kUpright.col_x(1) - kUpright.col_x(0) == kColW, "2 px columns to x223");
static_assert(band_y1(kBands - 1) + 1 == kInfoRuleY, "the info rule follows the last band");
static_assert(Geom{240, 224}.band_y1(kBands - 1) + 1 == Geom{240, 224}.info_rule_y(), "landscape: no gap above the rule");

// ---- scales (10 mW / 10 mV / 10 mA, the trend's units) -----------------------------------------
// Fixed 0..max, from pick lists: W covers the data and the largest limit (whole W) in the window, V the data + 2 %, A the
// data + 5 %; the last pick holds past it.
uint32_t scale_w10(uint32_t data_max10, uint32_t limit_max_w);
uint32_t scale_v10(uint32_t data_max10);
uint32_t scale_a10(uint32_t data_max10);
// The smallest 1, 2 or 5 x 10^k that is >= x (x >= 1).
uint32_t nice_step(uint32_t x);
// The Wh graph's top above its first point, in the tier's unit (unit_mwh mWh): a whole nice step of at least a third of
// the rise max_units and at least 5 mWh (one unit for the coarser tiers).
uint32_t wh_top(uint32_t max_units, uint32_t unit_mwh);
// Rows above the graph bottom (0 .. h-1) of v on a 0..top scale, rounded and clamped. 32-bit: v is clamped to top, and a
// top above 2^24 drops 8 bits from both, so v * 2 (h - 1) + top stays below 2^31 for any h up to 64.
constexpr int level(uint32_t v, uint32_t top, int h) {
    if (top == 0 || h < 2) return 0;
    if (v >= top) return h - 1;
    if (top > 0xFFFFFFu) { v >>= 8; top >>= 8; }
    return static_cast<int>((v * static_cast<uint32_t>(h - 1) * 2u + top) / (2u * top));
}

// ---- texts ------------------------------------------------------------------------------------
// The span in whole units: "10s", "60s", "100s", "10m", "1h40m", "2h".
int span_text(std::span<char> out, uint32_t seconds);
// The live value, fixed width: V and Wh five characters ("20.01", "1.234"), W and A four ("64.2", "3.21"); |milli|.
int value_text(Band b, int32_t milli, std::span<char> out);
constexpr int value_chars(Band b) { return b == Band::V || b == Band::Wh ? 5 : 4; }
constexpr const char *unit_text(Band b) { return b == Band::W ? "W" : b == Band::V ? "V" : b == Band::A ? "A" : "Wh"; }
// "/20V", "/100W" (whole watts), "/3.25A"; "/--" without a ceiling; empty for Wh.
void max_text(Band b, uint32_t max_milli, StrBuf<8> &out);
// Left x of a font 16 group of a number of characters, centred on the upright content width (x112).
constexpr int16_t group_x(int chars) { return kUpright.group_x(chars); }

// ---- the contract's W limit ------------------------------------------------------------------
// The PD level x its current (home::contract_mw); under PPS the requested level (the range's top until one is known),
// so the dotted limit follows a PPS ramp. 0 = none known.
uint32_t limit_mw(const home::ProtoInfo &p);

// ---- the info line -----------------------------------------------------------------------------
// The diagnosis word at x4, then the protocol row at x52 when the word fits in front of it (six characters), else one
// space after the word; the elapsed time right-aligned to x219. On a collision the word wins over the protocol
// (proto_x -1) and the protocol over the elapsed time (elapsed false). Counts in font 16 characters.
struct InfoLayout { int16_t proto_x; bool elapsed; };
InfoLayout info_layout(size_t word_chars, size_t proto_chars, size_t elapsed_chars, int16_t right = kElRight);

}  // namespace k2::graphs
