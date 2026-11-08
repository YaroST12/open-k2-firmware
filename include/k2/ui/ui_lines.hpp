// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lines page v2, pure logic: bar geometry for the four analog lines, the Rp advertisement read off
// CC and the verdict pills for each D+/D- classifier result. No drawing here.
#pragma once
#include <cstdint>
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

constexpr int kLinesBarW = 112;          // track width, 0 .. 3.3 V
constexpr int32_t kLinesFullMv = 3300;
constexpr int lines_bar_px(int32_t mv) { return gauge_fill_px(kLinesBarW, mv, kLinesFullMv); }
// Tick offset inside the track (0.4 V BC1.2 detect level, 2.0 V upper QC level).
constexpr int lines_tick_px(int32_t mv) { return static_cast<int>(static_cast<int64_t>(kLinesBarW) * mv / kLinesFullMv); }
constexpr int32_t kLinesTickLowMv = 400, kLinesTickHighMv = 2000;

// "3.0A on CC1" (the page draws the "Rp" label once, at the left): the source's current advertisement as a CC pin voltage window (USB-C vRd).
// Names the active CC wire (fusb302::active_cc(), 0 = none) and its own level, even when it is the lower-voltage one; without an active line, falls back to the higher-voltage wire.
void rp_text(StrBuf<24> &out, bool have_reading, std::uint8_t active_cc, int32_t cc1_mv, int32_t cc2_mv);

// "32\xC2\xB0"C" (whole degrees, round half away from zero) or "--" without a die reading; the page draws the "MCU" label.
void mcu_text(StrBuf<24> &out, bool valid, int32_t deci_c);

enum class Tone : uint8_t { Ok, Info, Caution, Dim };

struct LinesVerdict {
    StrBuf<12> main;     // solid pill: the decoded result ("DCP short")
    StrBuf<12> extra;    // tint pill: the detail ("Apple 2.4A"); empty when there is none
    Tone tone = Tone::Dim;
    StrBuf<28> footer;   // one line, <= 26 characters
};
LinesVerdict lines_verdict(fastcharge::Protocol p);

}  // namespace k2
