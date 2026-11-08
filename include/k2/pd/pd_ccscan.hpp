// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// CC line choice for the attach scan. The two CC analog switches (PC14/PC15) decide which lines reach
// the FUSB302; the chip always measures on CC2 and the switches route the live line there. The
// driver samples every switch setting and every FUSB302 line, this picks the one that sees a source.
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace k2::pd {

// Switch masks in scan order: both closed first (the old behaviour), then one switch at a time.
inline constexpr std::array<std::uint8_t, 3> kCcScanMasks = {3, 1, 2};   // bit0 = CC1 switch, bit1 = CC2 switch

struct CcSample {
    std::uint8_t mask;      // switches closed while measuring
    std::uint8_t line;      // FUSB302 line measured: 1 or 2
    bool comp;              // STATUS0 COMP: above the ~210 mV threshold (an Rp is there)
    std::uint8_t bc_lvl;    // STATUS0 BC_LVL 0..3 (Rp current advertisement)
};

struct CcChoice {
    bool found = false;
    std::uint8_t mask = 3;  // unchanged default when nothing was found
    std::uint8_t line = 0;
};

// A sample is isolated when no other line of the same switch setting sees the Rp as well: a shorted
// pair (both switches closed, one connector CC reaching both lines) cannot say which line is live.
constexpr bool isolated(std::span<const CcSample> all, const CcSample& s) {
    for (const CcSample& o : all)
        if (o.mask == s.mask && o.line != s.line && o.comp) return false;
    return true;
}

// Isolated samples with COMP set come first (highest BC_LVL, then scan order); only when none is
// isolated the highest BC_LVL over all of them decides, first wins a tie.
constexpr CcChoice choose_cc(std::span<const CcSample> samples) {
    CcChoice best;
    int best_lvl = -1;
    for (const CcSample& s : samples) {
        if (!s.comp || !isolated(samples, s) || int(s.bc_lvl) <= best_lvl) continue;
        best = {true, s.mask, s.line};
        best_lvl = int(s.bc_lvl);
    }
    if (best.found) return best;
    for (const CcSample& s : samples) {
        if (!s.comp || int(s.bc_lvl) <= best_lvl) continue;
        best = {true, s.mask, s.line};
        best_lvl = int(s.bc_lvl);
    }
    return best;
}

// Self-powered boot (a key mode pinned Rd): the charger is also the K2's supply, so the Rd the K2 pins must reach the
// connector. The sweep may find a one-switch routing; that is a means of telling the lines apart, and the final routing
// closes both. A plain boot keeps the scan's choice.
constexpr std::uint8_t self_power_mask(std::uint8_t found, bool pinned) { return pinned ? std::uint8_t(3) : found; }

// The one-switch sweep opens a switch and with it the Rd a charger-powered K2 hangs on. It is only worth it when the
// closed-pair samples (mask 3) cannot name one live line: tied (both lines see the Rp) or nothing seen at all.
constexpr bool sweep_needed(std::span<const CcSample> closed_pair) {
    unsigned live = 0;
    for (const CcSample& s : closed_pair)
        if (s.mask == 3 && s.comp) ++live;
    return live != 1;
}

// A key mode's pinned line (`pin`: SWITCHES0 bits, 1 = CC1, 2 = CC2) when the closed-pair samples see an Rp on it, else 0.
// The charger attached through that line (the key's hardware Rd and our pin are the only Rd it saw), so on a direct plug, where
// BOTH lines see the charger's Rp, the pinned line is the active one. Taking the other line moves our Rd away from the pin the
// charger is attached on: releasing the key is then a detach (hardware 2026-11-06, boot QR: SWITCHES0 0x02 -> 0x05 at 1 s).
constexpr std::uint8_t pinned_live_line(std::uint8_t pin, std::span<const CcSample> closed_pair) {
    const std::uint8_t line = (pin & 3) == 1 ? 1 : (pin & 3) == 2 ? 2 : 0;
    if (line == 0) return 0;
    for (const CcSample& s : closed_pair)
        if (s.mask == 3 && s.line == line && s.comp) return line;
    return 0;
}

// The Rd window on a CC net, in mV: a source's Rp over a sink's Rd divides down to ~0.2-2.0 V,
// an open Rp (or VCONN) reads ~3.3 V, an Ra / grounded line reads low. The live wire is the one
// net inside the window while the other net is open or unknown: exactly that pair names the line.
// Both nets in the window (a shorted pair), neither in it, or a low reading on the other net
// (Ra / short: not one-live-plus-one-open) are ambiguous and name nothing. -1 = unknown counts as
// not-in-window and never as low.
inline constexpr std::int32_t kRdWindowLoMv = 200, kRdWindowHiMv = 2000;
constexpr std::uint8_t rd_window_line(std::int32_t cc1_mv, std::int32_t cc2_mv) {
    const bool in1 = cc1_mv >= kRdWindowLoMv && cc1_mv <= kRdWindowHiMv;
    const bool in2 = cc2_mv >= kRdWindowLoMv && cc2_mv <= kRdWindowHiMv;
    if (in1 == in2) return 0;
    const std::int32_t other = in1 ? cc2_mv : cc1_mv;
    if (other >= 0 && other < kRdWindowLoMv) return 0;
    return in1 ? 1 : 2;
}

// Debug option (pd-reread --line N): only line `forced` (1/2) may be chosen; 0 = the normal choice. Isolated samples of
// that line first, then any comp sample of it; the highest BC_LVL wins, the first one breaks a tie.
constexpr CcChoice choose_cc_forced(std::span<const CcSample> samples, std::uint8_t forced) {
    if (forced == 0) return choose_cc(samples);
    CcChoice best;
    int best_lvl = -1;
    for (const CcSample& s : samples) {
        if (s.line != forced || !s.comp || !isolated(samples, s) || int(s.bc_lvl) <= best_lvl) continue;
        best = {true, s.mask, s.line};
        best_lvl = int(s.bc_lvl);
    }
    if (best.found) return best;
    for (const CcSample& s : samples) {
        if (s.line != forced || !s.comp || int(s.bc_lvl) <= best_lvl) continue;
        best = {true, s.mask, s.line};
        best_lvl = int(s.bc_lvl);
    }
    return best;
}

}  // namespace k2::pd
