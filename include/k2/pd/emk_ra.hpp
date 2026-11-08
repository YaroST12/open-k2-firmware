// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Top-plug e-marker spoof: the Ra mode (pure rules, host-tested in tests/test_emk_ra.cpp; the pin and register work
// is in src/drivers/fusb302.cpp, fusb302::ra_set).
//
// Measured on hardware (2026-11-06, `debug cc-probe` sweeps): with a charger waiting (Rp on both CC pins) and the K2
// passive, driving PC13 high pulls the CC1 net to ~322 mV, the same level a real e-marked cable's Ra gives. So PC13 is
// an Ra on CC1 (~1 kohm to ground), not a VCONN supply switch. The mode holds PC13 high and presents Rd on CC2 alone:
// the source sees a sink plus a powered cable, supplies VCONN on its Ra-side pin and sends SOP' Discover Identity on
// the CC2 line, which the FUSB302 (ENSOP1 on, spoof armed) answers. The chip's own VCONN switches stay open.
#pragma once
#include <cstdint>

#include "k2/pd/pd_ccmode.hpp"

namespace k2::emkra {

// The only line the attach scan may take while the Ra is on CC1 (CC1 reads ~322 mV, above the scan's 210 mV threshold).
inline constexpr std::uint8_t kLine = 2;

// PC13 level: the Ra mode, or the (experimental, remote-only) VCONN rail control.
constexpr bool pc13_high(bool vconn_rail, bool ra) { return vconn_rail || ra; }

struct Ctx {
    bool inline_sink = false;   // a real sink is attached downstream
    bool contract = false;      // a contract or negotiation is running
    bool busy = false;          // identify / probe / re-read / prepare is running
    // The boot mode's pinned Rd bits (bit0 = CC1, bit1 = CC2; 0 = none). PLUS pins CC2 alone: the very line this mode
    // presents Rd on, so a charger-powered run can spoof. A pin that reaches CC1 (OK key, or PLUS's Rd moved there when
    // the charger sits on the other orientation) collides with the Ra and refuses.
    std::uint8_t pinned_rd = 0;
    bool vconn = false;         // a VCONN role (the experiment) is running or applied
};

enum class Refuse : std::uint8_t { None, Inline, Contract, Busy, PinnedRd, Vconn };

constexpr Refuse check(const Ctx& c) {
    if (c.inline_sink) return Refuse::Inline;
    if (c.contract) return Refuse::Contract;
    if (c.busy) return Refuse::Busy;
    if (c.pinned_rd & 1) return Refuse::PinnedRd;
    if (c.vconn) return Refuse::Vconn;
    return Refuse::None;
}

constexpr const char* refuse_name(Refuse r) {
    switch (r) {
        case Refuse::None: return "ok";
        case Refuse::Inline: return "inline";
        case Refuse::Contract: return "contract";
        case Refuse::Busy: return "busy";
        case Refuse::PinnedRd: return "pinned-rd";
        case Refuse::Vconn: return "vconn";
    }
    return "?";
}

// Rd lines a run start presents: with the mode on, CC2 alone (CC1 carries the Ra); otherwise the plain run-start rule.
constexpr std::uint8_t run_rd_lines(bool ra, bool inline_sink, std::uint8_t cc) {
    if (!ra) return pd::start_rd_lines(inline_sink, cc);
    return inline_sink ? std::uint8_t(0) : std::uint8_t(2);
}

// A detach ends the mode, except inside a charger-test run that owns it: its units detach and re-attach on purpose.
constexpr bool survives_detach(bool run_owned) { return run_owned; }

// The chip's VCONN switches never close while the Ra is on CC1 (its supply on that net would fight the Ra).
constexpr bool vconn_switch_allowed(bool ra) { return !ra; }

}  // namespace k2::emkra
