// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 pure model: layout constants, alarm thresholds, number and protocol-row formatting, the sweep
// column maths. No hardware access, no drawing: tests/test_home_model.cpp covers all of it.
//
// Logical content is 224 x 240 (the button strip is outside). Rows are inclusive pixel rows.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include "k2/ui/ui_calc.hpp"

namespace k2::home {

// ---- geometry ---------------------------------------------------------------------------------
constexpr int16_t kW = 224;                 // logical content width
constexpr int kCols = 110;                  // sweep columns, 2 px each: x 4..223
constexpr int16_t kColX0 = 4;
constexpr int kGapCols = 4;                 // blank columns ahead of the sweep cursor

// A measurement cell: the full cell rows (status edge, inversion), the sweep rows inside it.
struct CellGeom {
    int16_t top, bottom;                    // cell rows
    int16_t t0, t1;                         // sweep rows (inclusive)
    constexpr int span() const { return t1 - t0; }
};
constexpr CellGeom kVoltCell{0, 69, 2, 69};
constexpr CellGeom kAmpCell{72, 141, 74, 133};

constexpr int16_t kRule1Y = 70, kRule2Y = 142, kRule3Y = 194;   // 2 px rules
constexpr int16_t kBandY0 = 144, kBandY1 = 193;
constexpr int16_t kBandMidX = 126, kBandSplitY = 168;
constexpr int16_t kBarX0 = 10, kBarX1 = 214, kBarY = 136, kBarH = 3;
constexpr int16_t kInfoTagX = 4, kInfoValX = 52, kInfoRightX = 220, kInfoRow1Base = 213, kInfoRow2Base = 235;

// ---- alarms ------------------------------------------------------------------------------------
enum class Alarm : uint8_t { None, Near, Over };
// |I| against the contract's current limit: Near from 90 %, Over above 100 %. No limit known = None.
constexpr Alarm alarm_of(uint32_t abs_ma, uint32_t max_ma) {
    if (max_ma == 0) return Alarm::None;
    if (abs_ma > max_ma) return Alarm::Over;
    return static_cast<uint64_t>(abs_ma) * 10 >= static_cast<uint64_t>(max_ma) * 9 ? Alarm::Near : Alarm::None;
}
constexpr uint32_t pct_of(uint32_t abs_ma, uint32_t max_ma) {
    return max_ma == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(abs_ma) * 100 + max_ma / 2) / max_ma);
}

// ---- limit bar ---------------------------------------------------------------------------------
constexpr int kBarTrack = kBarX1 - kBarX0 + 1;   // 205 px
// Filled width of the bar in px (0 = empty, kBarTrack = at or over the limit).
constexpr int bar_fill_w(uint32_t abs_ma, uint32_t max_ma) {
    if (max_ma == 0 || abs_ma == 0) return 0;
    const uint64_t n = static_cast<uint64_t>(abs_ma) * (kBarX1 - kBarX0);
    const int w = static_cast<int>((n * 2 + max_ma) / (2ull * max_ma)) + 1;   // round(204 f) + 1
    return w > kBarTrack ? kBarTrack : w;
}
// x of the 90 % tick.
constexpr int16_t bar_tick_x() { return static_cast<int16_t>(kBarX0 + (kBarX1 - kBarX0) * 9 / 10 + ((kBarX1 - kBarX0) * 9 % 10 >= 5 ? 1 : 0)); }

// ---- sweep -------------------------------------------------------------------------------------
// Full scale of a cell in 10 mV / 10 mA units: the contract maximum x 1.15 (20 V / 5 A when unknown).
constexpr uint32_t scale_v10(uint32_t max_mv) { return (max_mv ? max_mv : 20000u) * 115u / 1000u; }
constexpr uint32_t scale_a10(uint32_t max_ma) { return (max_ma ? max_ma : 5000u) * 115u / 1000u; }
// Rows covered by a sample, counted from the cell bottom of the sweep area: 0 = no column,
// else 1 .. span+1 (the top row of the column is its level line).
constexpr int sweep_rows(uint32_t v10, uint32_t scale10, int span) {
    if (v10 <= 1 || v10 == 0xFFFF || scale10 == 0) return 0;
    const uint32_t v = v10 > scale10 ? scale10 : v10;
    return static_cast<int>((static_cast<uint64_t>(v) * static_cast<uint32_t>(span) * 2 + scale10) / (2ull * scale10)) + 1;
}
// Column 0..109 is blank (cursor column included) while it is within the gap ahead of the cursor.
constexpr bool in_gap(int col, int cursor) { return ((col - cursor + kCols) % kCols) < kGapCols; }
constexpr int col_of_x(int x) { return (x - kColX0) / 2; }              // valid for x in 4..223
constexpr int16_t x_of_col(int col) { return static_cast<int16_t>(kColX0 + col * 2); }
// Milliseconds between sweep steps per window (a window spans kCols steps): 10 s -> 91 ms, 60 s -> 545 ms,
// 10 min -> 5455 ms.
constexpr uint32_t step_ms(int window_index) { return window_index == 0 ? 91u : window_index == 1 ? 545u : 5455u; }

// ---- numbers -----------------------------------------------------------------------------------
// Hero V / A: five characters, the most decimals that fit ("5.123", "20.03", "100.0"), |value|.
int fmt_hero(std::span<char> out, int32_t milli);
// W hero: four characters ("9.99", "99.9", "140").
int fmt_watts(std::span<char> out, int32_t milli);
// Wh / Ah: "9.99", "12.3", "123", then kilo with one decimal "1.2k", "12k". Input in milli-units.
int fmt_energy(std::span<char> out, int32_t milli);
// Splits at the decimal point: int_part = text before it, rest = '.' and the fraction (empty when none).
struct DecSplit { std::string_view int_part, rest; };
constexpr DecSplit split_decimal(std::string_view s) {
    const size_t p = s.find('.');
    return p == std::string_view::npos ? DecSplit{s, {}} : DecSplit{s.substr(0, p), s.substr(p)};
}
// Elapsed time under the info block: "3:07", "59:59", then "1:05h", "10h05" (never over five characters, capped 99h59).
int fmt_elapsed(std::span<char> out, uint32_t secs);
// A level without a unit letter and without trailing zeros, two decimals at most: "20", "3.3", "9.25".
int fmt_level(std::span<char> out, uint32_t milli);

// ---- protocol row ------------------------------------------------------------------------------
enum class ProtoKind : uint8_t { None, PdFixed, PdEpr, Pps, Avs, Qc2, Qc3, Fcp, Scp, Ufcs, Svooc, TypeC, Dcp, Unknown, Detecting };

// What the firmware knows about the source. Unknown numbers are 0.
struct ProtoInfo {
    ProtoKind kind = ProtoKind::None;
    uint32_t mv = 0;          // fixed level (PD, EPR, QC2, FCP, SCP, SVOOC, CC, DCP)
    uint32_t lo_mv = 0, hi_mv = 0;   // range (PPS, AVS, QC3, UFCS)
    uint32_t ma = 0;          // current rating (limit); 0 unknown
    uint32_t mw = 0;          // AVS power rating
    uint32_t max_mv = 0;      // the source's highest voltage ("max 20V")
};

struct ProtoText {
    StrBuf<6> tag;
    StrBuf<20> value;
};
void format_proto(const ProtoInfo &p, ProtoText &out);

// ---- cell state --------------------------------------------------------------------------------
enum class Mode : uint8_t { NoMeter, Idle, Live, Stale };
// The one exception badge in the VBUS cell's upper right. Normal operation shows the dim "max NNV".
enum class Pill : uint8_t { None, Last, NoVbus, Over };
constexpr Pill pill_of(Mode m, Alarm a) {
    return m == Mode::Stale ? Pill::Last : m == Mode::Idle ? Pill::NoVbus : (m == Mode::Live && a == Alarm::Over) ? Pill::Over : Pill::None;
}
// "max 20V" for the VBUS cell and "max 5A" for the IBUS cell, from the contract or protocol maximum,
// never from the live reading. Returns false (and leaves `out` empty) when the source's maximum is
// unknown or nothing is live: an unplugged tester never prints a stale "max 1.0V".
bool max_volts_text(Mode m, const ProtoInfo &p, StrBuf<12> &out);
bool max_amps_text(Mode m, const ProtoInfo &p, StrBuf<12> &out);
// IBUS alarm line: "92% OF 5A" (Near) or "OVER 5A LIMIT" (Over).
void alarm_text(Alarm a, uint32_t abs_ma, uint32_t max_ma, StrBuf<16> &out);

}  // namespace k2::home
