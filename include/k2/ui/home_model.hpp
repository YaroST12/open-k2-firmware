// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c pure model: layout constants, alarm thresholds, number and protocol-row formatting, the sweep
// column maths. No hardware access, no drawing: tests/test_home_model.cpp covers all of it.
//
// Logical content is 224 x 240 (the button strip is outside). Rows are inclusive pixel rows.
// Layout (2c, user decision 2026-11-12): the hero cell y0..73 (W, V or A at 64 px), a 2 px rule, the ledger rows
// x0..139 (the two other quantities at 48 px, y76..135 and y137..193, a 1 px rule at y136), a 1 px rule at x140, two
// card cells x141..223,
// a 2 px rule at y194 and the info block below it.
#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include "k2/pd/diag.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2::home {

// ---- geometry ---------------------------------------------------------------------------------
constexpr int16_t kW = 224;                 // logical content width
constexpr int kCols = 110;                  // hero sweep columns, 2 px each: x 4..223
constexpr int kLedgerCols = 66;             // ledger sweep columns: x 4..135
constexpr int16_t kColX0 = 4;

// A measurement cell: the full cell rows (status edge, inversion), the sweep rows inside it.
struct CellGeom {
    int16_t top, bottom;                    // cell rows
    int16_t t0, t1;                         // sweep rows (inclusive)
    constexpr int span() const { return t1 - t0; }
};
// The sweep runs from 14 rows under the cell top (clear of the label line) to the bar, or 2 rows above the bottom.
constexpr CellGeom cell_geom(int16_t top, int16_t bottom, bool bar) {
    return {top, bottom, static_cast<int16_t>(top + 14), static_cast<int16_t>(bottom - (bar ? 7 : 2))};
}

constexpr int16_t kHeroY0 = 0, kHeroY1 = 73;
constexpr int16_t kRule1Y = 74, kRule3Y = 194;                 // 2 px rules under the hero and above the info block
// The 1 px rule between the rows sits on y136, not y135: the landscape mapping squeezes 240 logical rows into 224 and
// drops y135 entirely (test_ui_home_landscape checks every rule keeps a physical row).
constexpr int16_t kRowY0[2] = {76, 137}, kRowY1[2] = {135, 193};   // ledger rows and card cells
constexpr int16_t kRowRuleY = 136;                             // 1 px rule between the rows (both columns)
constexpr int16_t kLedgerX1 = 139, kVRuleX = 140, kCardX0 = 141;
constexpr int16_t kInfoTagX = 4, kInfoValX = 52, kInfoRightX = 220, kInfoRow1Base = 213, kInfoRow2Base = 235;

// ---- contract bar ------------------------------------------------------------------------------
// 3 px against the contract maximum, 90 % tick: hero x10..214 at its row 68, a ledger row x6..133, 3 rows over its bottom.
struct BarGeom { int16_t x0, x1, y; };
constexpr int16_t kBarH = 3;
constexpr BarGeom kHeroBar{10, 214, static_cast<int16_t>(kHeroY1 - 5)};
constexpr BarGeom ledger_bar(int row) { return {6, 133, static_cast<int16_t>(kRowY1[row] - 3)}; }

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
constexpr int bar_track(BarGeom b) { return b.x1 - b.x0 + 1; }
// Filled width of the bar in px (0 = empty, the whole track = at or over the limit).
constexpr int bar_fill_w(uint32_t abs, uint32_t max, BarGeom b) {
    if (max == 0 || abs == 0) return 0;
    const uint64_t n = static_cast<uint64_t>(abs) * static_cast<uint32_t>(b.x1 - b.x0);
    const int w = static_cast<int>((n * 2 + max) / (2ull * max)) + 1;   // round((x1 - x0) f) + 1
    return w > bar_track(b) ? bar_track(b) : w;
}
// x of the 90 % tick.
constexpr int16_t bar_tick_x(BarGeom b) {
    return static_cast<int16_t>(b.x0 + (b.x1 - b.x0) * 9 / 10 + ((b.x1 - b.x0) * 9 % 10 >= 5 ? 1 : 0));
}

// ---- sweep -------------------------------------------------------------------------------------
// Full scale of a cell in 10 mV / 10 mA / 10 mW units: the contract maximum x 1.15 (20 V / 5 A / 60 W when unknown).
constexpr uint32_t scale_v10(uint32_t max_mv) { return (max_mv ? max_mv : 20000u) * 115u / 1000u; }
constexpr uint32_t scale_a10(uint32_t max_ma) { return (max_ma ? max_ma : 5000u) * 115u / 1000u; }
constexpr uint32_t scale_w10(uint32_t max_mw) { return static_cast<uint32_t>(static_cast<uint64_t>(max_mw ? max_mw : 60000u) * 115u / 1000u); }
// Rows covered by a sample, counted from the cell bottom of the sweep area: 0 = no column,
// else 1 .. span+1 (the top row of the column is its level line).
constexpr int sweep_rows(uint32_t v10, uint32_t scale10, int span) {
    if (v10 <= 1 || v10 == 0xFFFF || scale10 == 0) return 0;
    const uint32_t v = v10 > scale10 ? scale10 : v10;
    return static_cast<int>((static_cast<uint64_t>(v) * static_cast<uint32_t>(span) * 2 + scale10) / (2ull * scale10)) + 1;
}
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
    // `mv` and `shown_ma` are live readings, not announced levels (an inline VOOC session): both print with one decimal
    // ("SVOOC 9.0V3.6A"). `shown_ma` is never a rating, so it drives no limit bar, scale or alarm (that is `ma`).
    bool measured = false;
    uint32_t shown_ma = 0;
};

struct ProtoText {
    StrBuf<6> tag;
    StrBuf<20> value;
};
void format_proto(const ProtoInfo &p, ProtoText &out);

// The info block's first row when the PD diagnosis has nothing to say (format_diag returns false): the two analog pairs
// the page already measures, so the bar is never empty. "CC1/2" with "1.62/0.07V" (outside a session), or "D+/D-" with
// "0.60/0.58V" (live, a session without PD). A negative reading (no ADC sample) shows "-.--".
struct LinesText {
    StrBuf<6> tag;
    StrBuf<16> value;
};
void format_lines(bool data_lines, int32_t a_mv, int32_t b_mv, LinesText &out);

// ---- cell state --------------------------------------------------------------------------------
enum class Mode : uint8_t { NoMeter, Idle, Live, Stale };

// The info block's first row while a session is live or stale: the Black Box diagnosis word (pd::diag::screen::word, at
// most 16 characters) under the "Diag" tag, distinct from row 2's protocol tag. The tone is the worst finding's: red for
// a Fail, amber for a Warn, dim when nothing is wrong (never green), grey on a stale screen. Returns false where the
// lines say more: outside a session (Idle, NoMeter) and for a session without PD (a legacy charger).
inline constexpr std::string_view kDiagTag = "Diag";
enum class DiagTone : uint8_t { Dim, Caution, Error, Stale };
struct DiagText {
    StrBuf<17> value;
    DiagTone tone = DiagTone::Dim;
};
bool format_diag(Mode m, const pd::diag::Report &r, DiagText &out);
// The power meter has not answered YET: the first seconds of a boot, while the chip is still being found (a key-mode boot lives on a
// VBUS that is still ramping when the first probe runs, and the main loop is busy with the charger link for seconds). The page says
// "starting" in a neutral colour; only a meter still missing after the grace is an error. Uptime in ms (the Ui clock starts at boot).
inline constexpr uint32_t kMeterGraceMs = 6000;
constexpr bool meter_starting(bool pm_online, uint32_t uptime_ms) { return !pm_online && uptime_ms < kMeterGraceMs; }
// The one exception badge in the VBUS cell's upper right. Normal operation shows the dim "max NNV".
// A held Rd pin (pd-reread --pin, a boot pin) gets the badge too ("Rd pin CC2"): a pin still set must be
// visible on the Home status line; only the OVER alarm keeps precedence over it.
enum class Pill : uint8_t { None, Last, NoVbus, Over, Pin };
constexpr Pill pill_of(Mode m, Alarm a, std::uint8_t pin = 0) {
    if (m == Mode::Live && a == Alarm::Over) return Pill::Over;
    if (pin == 1 || pin == 2) return Pill::Pin;
    return m == Mode::Stale ? Pill::Last : m == Mode::Idle ? Pill::NoVbus : Pill::None;
}
// "max 20V" for the VBUS cell and "max 5A" for the IBUS cell, from the contract or protocol maximum,
// never from the live reading. Returns false (and leaves `out` empty) when the source's maximum is
// unknown or nothing is live: an unplugged tester never prints a stale "max 1.0V".
bool max_volts_text(Mode m, const ProtoInfo &p, StrBuf<12> &out);
bool max_amps_text(Mode m, const ProtoInfo &p, StrBuf<12> &out);
// The same for any quantity: "max 100W" from a maximum in milli-units.
bool max_text(Mode m, uint32_t max_milli, char unit, StrBuf<12> &out);
// Alarm line of a hero cell: "92% OF 5A" / "93% OF 100W" (Near) or "OVER 5A LIMIT" (Over); a ledger row's short form
// (`brief`): "92%" or "OVER". 20 bytes: the longest line, "OVER 3.25A LIMIT", is 16 characters.
void alarm_text(Alarm a, uint32_t abs, uint32_t max, StrBuf<20> &out, char unit = 'A', bool brief = false);

}  // namespace k2::home
