// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive fast-charge detection driver: watches the D+/D- levels (through the ADC driver) and VBUS
// (through the power monitor) and names the legacy charging protocol of the source. It never drives
// the D+/D- switch pins (PA7/PB0/PB1/PB2); init() only checks they are in the idle state.
// The decision logic is in fastcharge_classify.hpp.
#pragma once
#include <optional>

#include "k2/fastcharge/fastcharge_classify.hpp"

namespace k2::fastcharge {

// Check the D+/D- switch pins read back as the idle state and log "[FC] idle ..." with the
// mismatch mask (see idle_mismatch). Call after the driver that configures those pins has run.
// Returns the mismatch mask, 0 when all four pins are idle.
unsigned init();

// Log the "[FC] idle ..." line again with fresh pin levels; main repeats it in the heartbeat so a
// monitor attached after boot still sees it. Returns the mismatch mask.
unsigned log_idle();

// Feed one sample of D+, D- and VBUS into the window; call every ~20 ms (one second of history).
void poll();

// Current verdict. Protocol::Pending until the window is full; NoInput without VBUS.
Protocol read();

}  // namespace k2::fastcharge
