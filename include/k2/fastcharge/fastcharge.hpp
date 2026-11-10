// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive fast-charge detection driver: watches the D+/D- levels (through the ADC driver) and VBUS
// (through the power monitor) and names the legacy charging protocol of the source. It is passive except
// for one narrow case: when the K2 sits alone on a 5 V source and both lines read its own 0.6 V idle
// drive, it pulls D+ to 0 V for one poll interval (about 20 ms, once per attach) and reads whether D-
// follows (a BC1.2 short), through dpdm::dcp_probe_begin() / dcp_probe_end(); see dcp_check_allowed()
// for every condition. init() only checks the four switch pins (PA7/PB0/PB1/PB2) are in the idle state.
// The idle state itself drives both lines to a 0.6 V level (k2::dpdm), it does not leave them floating.
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
