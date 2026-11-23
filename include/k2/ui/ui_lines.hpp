// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lines page v2, pure logic: bar geometry for the four analog lines, the Rp advertisement read off
// CC and the verdict pills for each D+/D- classifier result. No drawing here.
#pragma once
#include <cstdint>
#include "k2/core/math_opt.hpp"
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/ui/ui_calc.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

constexpr int kLinesBarW = 104;          // track width, 0 .. 3.3 V (208 px twin-rail canvas)
constexpr int32_t kLinesFullMv = 3300;
constexpr int lines_bar_px(int32_t mv) {
    if (mv <= 0) return 0;
    if (mv >= kLinesFullMv) return kLinesBarW;
    const int px = static_cast<int>(math::mul_div_i32(kLinesBarW, mv, kLinesFullMv));
    return px < 2 ? 2 : px;
}
// Tick offset inside the track (0.4 V BC1.2 detect level, 2.0 V upper QC level).
constexpr int lines_tick_px(int32_t mv) { return static_cast<int>(math::mul_div_i32(kLinesBarW, mv, kLinesFullMv)); }
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
// pd_decoded: a PD message was actually decoded on CC (a contract or source caps), not just inferred
// from the line levels. The green PD chip appears only then; PD-family verdicts without it fall back
// to a neutral "PD?" caution.
LinesVerdict lines_verdict(fastcharge::Protocol p, bool pd_decoded);

// The D+/D- row: what the data lines carry, from fastcharge::lines() (the pair alone, never a PD summary). A PD-family
// result there is a band's fallback, no handshake on the pair: "none". `dim` when the row holds no fact.
struct DpDmWord {
    const char *text;
    bool dim;
};
DpDmWord dpdm_word(fastcharge::Protocol lines);

}  // namespace k2
