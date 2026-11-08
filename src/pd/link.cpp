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
    return c.valid && c.active_cc != 0;
}

Probe probe_now() {
    Probe p;
    const auto& c = fusb302::caps();
    p.pd_active = c.valid && c.active_cc != 0;
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

}  // namespace k2::link
