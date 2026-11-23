// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The words for one charger-test step, in one place: the Running / Steps rows, the Detail heading, the Results chips and
// summary lines and the report card text all call these, so a row reads the same on the K2 and on the host
// (tools/step_label.py mirrors it; the golden vectors are tests/fixtures/step_labels.txt).
//
//   "<protocol> <V> <A>"   PD 9V 3A   PPS 20V 5A   EPR 28V 5A   SCP 10V 2.25A
//   "Mi <W>"               Mi 55W     Mi 66W        a Xiaomi mode by its power level (mi_watts); the PDO before and after the
//                          chain is the Detail page's and the report's (mi_unlock::mode_text)
//   "<protocol> <V>"       AVS 15V    QC3 12V       VOOC 5V      (no current the step is known to carry: none is invented)
//   "PPS 11→8V"            a ramp row: the previous target of the same offer, then this one (the arrow is U+2192, in the
//                          12 px font; one glyph cell, three bytes)
//   "PD3.1 rev"            a revision row; a plan row with no offer or voltage reads the bare protocol ("PPS", "PD3.x")
// The measured voltage is the row's own column, never part of the label. The PDO position is in the Detail page only.
//
// Amps: a PD fixed or PPS row shows the offer's advertised current (the row itself requests at most 1 A), except the
// full-current PPS row (top voltage at the APDO's whole current), which shows what it requests, cable-clamped ("PPS 11V 3A"
// on an unread cable); SCP the limit it writes; AVS offers and the other D+/D- modes show volts only. A Xiaomi mode row (a level the
// 0x2717 chain unlocked) shows no volts or amps: its watts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "k2/chargertest/charger_test.hpp"

namespace k2::step_label {

inline constexpr size_t kMax = 32;       // buffer bytes of one text, with the terminator
// The Steps row text is this many 16 px glyph cells (8 px each) at most: it starts at kit::kTextX and ends a gap before the
// measured-voltage column ("36.0V", 5 cells, right-aligned to kit::kTailX). Every label the plan can produce fits (swept, and the
// room derived from the kit constants, in tests/test_step_label.cpp).
inline constexpr unsigned kRowCells = 17;

struct Text {
    char s[kMax] = {};
    size_t len = 0;
    const char* c_str() const { return s; }
    std::string_view view() const { return {s, len}; }
};

// The protocol word: "PD", "PPS", "EPR", "AVS", "Mi" (a Xiaomi mode), "Xiaomi" (the chain row), "QC3+", "SVOOC", "PD3.1 rev"; "PD3.x" / "PD2.0" / "EPR" / "AVS" for a
// plan row that holds no offer.
Text protocol(const charger_test::Step&);
// The numbers: "20V 5A", "12V", "11→8V", "10V 2.25A"; empty when the step has no voltage. `prev` = the plan row before this one
// (nullptr when unknown): a continuous row of the same algorithm and PDO position reads "from→to".
Text params(const charger_test::Step&, const charger_test::Step* prev = nullptr);
// protocol + ' ' + params ("PPS 20V 5A"), or the bare protocol when there are no params.
Text label(const charger_test::Step&, const charger_test::Step* prev = nullptr);
// The Running header's name inside the PD spec stage, from the engine's live point: "PPS 8.02V", "AVS 21V", "PD 9V", "EPR 28V",
// "Hard Reset", "PD spec" (no level yet). Volts only, at most kSpecCells cells.
inline constexpr unsigned kSpecCells = 10;
Text spec_label(const charger_test::SpecPoint&);
// The Running header's name in an interlude (charger_test::Interlude): "Charger ID" (the identity probe), "SVOOC ceiling", the spec
// stage's point (spec_label); empty for None. At most kInterludeCells cells: the 12 px name font of the left column (runlay::kNameCellsSmall).
inline constexpr unsigned kInterludeCells = 13;
Text interlude_label(charger_test::Interlude, const charger_test::SpecPoint&);
// The current the label shows, in mA (0 = none; a Xiaomi mode shows watts instead).
unsigned amps_ma(const charger_test::Step&);
// The power level a Xiaomi mode row is named by: the volts it asks times the current it asks, rounded to whole watts (11 V 5 A ->
// 55, 11 V 6 A -> 66); 0 for any other row. The one place the Mi watts are worded (the OEM watts labels may join it).
unsigned mi_watts(const charger_test::Step&);
// A UFCS step planned from the charger's Output_Capabilities names its mode (charger_test.hpp pack_ufcs_mode). ufcs_amps_ma is the
// mode's top current in mA (the Results chip words it: "9V 3A"; 0 = no mode), ufcs_range the whole mode for the Detail page:
// "5-20V 0.5-3A" ("3.4-11V 3A" when the mode has one current); empty without a mode. label() / params() / amps_ma() stay
// volts only for UFCS (the Steps and Running rows name the step, the Detail page and the chips say what the mode offers).
inline constexpr size_t kUfcsRangeCells = 16;   // the Detail page's value column; ufcs_range never reads longer (swept in the tests)
unsigned ufcs_amps_ma(const charger_test::Step&);
Text ufcs_level(const charger_test::Step&);   // the chip: params() plus the mode's top current, "9V 3A", "12.5V 3.25A"
Text ufcs_range(const charger_test::Step&);
// An advertised range, the Results chip of a PPS / AVS offer: "3.3-21V 5A", "15-28V 5A" (a power-only offer reads "9-20V 60W");
// empty for no offer.
Text range(uint32_t pdo);
// The same words from numbers (the report card keeps a range as 100 mV units and a current, not the PDO).
Text range(unsigned min_mv, unsigned max_mv, unsigned ma, unsigned mw = 0);

}  // namespace k2::step_label
