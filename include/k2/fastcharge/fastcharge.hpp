// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive fast-charge detection driver: watches the D+/D- levels (through the ADC driver) and VBUS
// (through the power monitor) and names the legacy charging protocol of the source. It is passive except
// for one narrow case: when the K2 sits alone on a 5 V source and both lines read its own 0.6 V idle
// drive, it pulls D+ to 0 V for one poll interval (about 20 ms, once per attach) and reads whether D-
// follows (a BC1.2 short), through dpdm::dcp_probe_begin() / dcp_probe_end(); see dcp_check_allowed()
// for every condition. init() only checks the four switch pins (PA7/PB0/PB1/PB2) are in the idle state.
// The idle state itself drives both lines to a 0.6 V level (k2::dpdm), it does not leave them floating.
// The one exception is the inline VOOC monitor (k2/fastcharge/dpsniff.hpp): while a far sink draws through the
// K2 (with or without a PD contract) and nothing else owns the lines, poll() lets dpsniff float them (Sniff inputs, never driven)
// and decode the VOOC exchange; it puts the 0.6 V idle back when the device leaves.
// The decision logic is in fastcharge_classify.hpp.
#pragma once
#include <optional>

#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/fastcharge/vooc_decode.hpp"

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

// read() as the remote (GET_STATE fc) and the shell report it: Protocol::Svooc while a proven VOOC session is live
// (report_label), the same word Home shows. read() itself stays the passive classifier's verdict.
Protocol label();

// The inline VOOC monitor's live view for the screen (vooc::live_of): a proven VOOC session now, and its last raw
// 0x82 reply word.
vooc::Live vooc_live();

}  // namespace k2::fastcharge
