// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Inline-load guard (pure, host-tested): the K2 sits between a charger and a real sink. Anything
// that drives D+/D- or changes the source voltage can break a live PD contract or upset a phone that
// is already charging, so every active operation asks inline_reason() first.
//
//  PdContract  - the FUSB302 sees a CC line with a decoded Source_Capabilities (a PD contract or at
//                least PD traffic): the D+/D- classifier must say "PD" and no trigger may start.
//  DataDevice  - VBUS is present (>= 4.0 V) and more than 50 mA flows:
//                a sink with a data connection is attached.
//
// One source of truth: k2::pd::Link (pd_link.hpp) owns "inline". It is sticky (stays set until the
// detach, so an idle laptop at 13 mA does not slip through) and counts a foreign sink Request.
// The UI publishes its verdict with link::set_inline_mode(); Probe::sticky_inline carries it here and
// wins over the instantaneous current check below, which stays as the fallback before the first tick.
// The current threshold is pd::kInlineCurrentMa, not a second copy.
#pragma once
#include <cstdint>
#include "k2/drivers/power_calc.hpp"

#include "k2/pd/pd_link.hpp"

namespace k2::link {

inline constexpr int kInlineCurrentMa = pd::kInlineCurrentMa;
inline constexpr int kVbusPresentMv = 4000;

enum class Reason : std::uint8_t { None, PdContract, DataDevice };

struct Probe {
    bool vbus_known = false;      // false while the power monitor is offline
    int vbus_mv = 0;
    bool current_known = false;
    int current_ma = 0;           // signed; only the magnitude counts
    bool pd_active = false;       // FUSB302: CC attached and PD decoded (Source_Capabilities, or a Request when the caps were missed)
    bool cc_active = false;       // FUSB302: a CC line is attached (caps may be missed)
    bool sticky_inline = false;   // pd::Link::inline_mode(), published by the UI
    pd::InlineCause inline_cause = pd::InlineCause::None;   // pd::Link::inline_cause(), published with it
    int self_supply_ma = 0;       // the K2's own draw through the shunt when it runs on this VBUS (no USB host), pd::LinkInput
};

constexpr Reason inline_reason(const Probe& p) {
    if (p.pd_active) return Reason::PdContract;
    if (p.sticky_inline) return Reason::DataDevice;
    if (!p.vbus_known || p.vbus_mv < kVbusPresentMv || !p.current_known) return Reason::None;
    const int mag = power::abs_sat(p.current_ma);
    return mag > kInlineCurrentMa + (p.self_supply_ma > 0 ? p.self_supply_ma : 0) ? Reason::DataDevice : Reason::None;
}

// A far sink draws through the K2 (the pd::Link latch, or more than kInlineCurrentMa above the K2's own supply), whether
// or not a PD contract is decoded: the inline_reason() ladder with the contract step left out. The inline VOOC monitor
// asks this, not inline_reason(): the phone on the USB-C port holds a 5 V PD contract and runs SuperVOOC on D+/D- at
// the same time.
constexpr bool far_sink_draws(Probe p) {
    p.pd_active = false;
    return inline_reason(p) == Reason::DataDevice;
}

constexpr int kPdLikelyMinMv = pd::kVSafe5MaxMv;   // above 5.5 V only a negotiated contract or a legacy trigger

// CC attached, VBUS above 5.5 V and current flowing, but no Source_Capabilities seen (the K2 booted
// mid-contract): a PD contract almost certainly runs. Reported as "PD?" until the caps are decoded.
constexpr bool pd_likely(const Probe& p) {
    if (p.pd_active || !p.cc_active || !p.vbus_known || !p.current_known) return false;
    const int mag = power::abs_sat(p.current_ma);
    return p.vbus_mv > kPdLikelyMinMv && mag > kInlineCurrentMa;
}

constexpr const char* name(Reason r) {
    switch (r) {
        case Reason::None: return "none";
        case Reason::PdContract: return "PD contract";
        case Reason::DataDevice: return "load attached";
    }
    return "?";
}

}  // namespace k2::link
