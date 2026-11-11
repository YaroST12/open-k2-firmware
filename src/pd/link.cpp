// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/link.hpp"

#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::link {

namespace {
bool g_sticky_inline = false;
}

void set_inline_mode(bool on) { g_sticky_inline = on; }

bool pd_active() {
    const auto& c = fusb302::caps();
    // A decoded Request counts too: the K2 may have booted mid-contract and missed the caps, but a
    // sink's Request on CC is PD traffic - the D+/D- classifier must not read it as a legacy mode
    // ("PD MTK" at 9 V on an inline link, ROADMAP 2026-11-10).
    return (c.valid && c.active_cc != 0) || fusb302::contract().valid;
}

Probe probe_now() {
    Probe p;
    const auto& c = fusb302::caps();
    p.pd_active = (c.valid && c.active_cc != 0) || fusb302::contract().valid;
    p.cc_active = c.active_cc != 0;
    p.sticky_inline = g_sticky_inline;
    p.self_supply_ma = usb::configured() ? 0 : pd::kSelfSupplyAllowanceMa;   // no USB host: the K2 may run on this VBUS
    if (const auto m = power::read()) {
        p.vbus_known = p.current_known = true;
        p.vbus_mv = m->vbus_mv;
        p.current_ma = m->current_ma;
    }
    return p;
}

bool pd_likely_now() { return pd_likely(probe_now()); }

Reason inline_now() { return inline_reason(probe_now()); }

bool far_sink_now() { return far_sink_draws(probe_now()); }

}  // namespace k2::link
