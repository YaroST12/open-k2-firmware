// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// E-marker result view, pure logic: the verdict card texts and the key / value rows of the cable
// details. Rows are built from a pd::Emarker alone, so the host tests cover every cable shape.
#pragma once
#include <cstdint>
#include "k2/pd/pd.hpp"
#include "k2/ui/ui_calc.hpp"

namespace k2 {

constexpr int kEmKvMax = 20;
constexpr int kEmKeyChars = 9;     // key column width in S16 characters
constexpr int kEmValChars = 11;    // value column width (right-aligned)

// "5A", "50V", "240W": the three headline numbers of the verdict card.
void emarker_amps_text(const pd::Emarker &e, StrBuf<8> &out);
void emarker_volts_text(const pd::Emarker &e, StrBuf<8> &out);
void emarker_watts_text(const pd::Emarker &e, StrBuf<8> &out);

// Short speed tag for the pill: "USB2.0", "USB3.2 G1", "USB3.2 G2", "USB4 G3", "USB4 G4", "TBT".
void emarker_speed_pill(const pd::Emarker &e, StrBuf<12> &out);

// Number of key / value rows below the card, `tbt` = the Thunderbolt SVID was seen.
int emarker_kv_count(const pd::Emarker &e, bool tbt);
// Row `i` (0 .. count-1): key at most 9 characters, value at most 11.
void emarker_kv(const pd::Emarker &e, bool tbt, int i, StrBuf<12> &key, StrBuf<16> &val);

}  // namespace k2
