// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Where the K2's own supply comes from, see k2/pd/power_source.hpp.
#include "k2/pd/power_source.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::power_source {

namespace {
Latch g_latch;
}

void poll(std::uint32_t now_ms) {
    const bool was_decided = g_latch.decided(), was_bus = g_latch.bus_powered();
    g_latch.update(now_ms, power::vbus_fast_mv().value_or(-1), usb::configured());
    if (g_latch.decided() && (!was_decided || was_bus != g_latch.bus_powered()))
        K2LOG("[PWR] supply: %s\n", g_latch.bus_powered() ? "bus-powered (no host, VBUS present)" : "host or none");
}
bool decided() { return g_latch.decided(); }
bool bus_powered() { return g_latch.bus_powered(); }

}  // namespace k2::power_source
