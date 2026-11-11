// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive fast-charge detection driver: watches the D+/D- levels (through the ADC driver) and VBUS
// (through the power monitor) and names the legacy charging protocol of the source. It is passive except
// for one narrow case: when the K2 sits alone on a 5 V source and the pair reads quiet (equal, steady, at
// or below 0.79 V), it plays the three dpdm::kDcpProbe steps, one per poll (about 60 ms, once per attach),
// and reads whether D- follows a pulled-low D+ (a BC1.2 short), through dpdm::dcp_probe_step() /
// dcp_probe_end(); see dcp_check_allowed() for every condition. No step drives D+ into the HVDCP entry
// window or forms a QC2 code. init() only checks both lines are at the passive idle.
// The passive idle releases both lines (k2::dpdm kIdle, all four switch pins analog): the K2 drives nothing.
// The inline VOOC monitor (k2/fastcharge/dpsniff.hpp) also keeps them undriven: while a far sink draws through the
// K2 (with or without a PD contract) and nothing else owns the lines, poll() lets dpsniff take them as Sniff
// inputs and decode the VOOC exchange; it puts the passive idle back when the device leaves.
// The decision logic is in fastcharge_classify.hpp.
#pragma once
#include <optional>

#include "k2/fastcharge/fastcharge_classify.hpp"
#include "k2/fastcharge/vooc_decode.hpp"

namespace k2::fastcharge {

// Check both D+/D- lines are at the passive idle (dpdm::drive() against dpdm::kIdle) and log
// "[FC] idle dp=.. dm=.. mismatch=.." (bit0 D+, bit1 D-). Call after dpdm::init().
// Returns the mismatch mask, 0 when both lines are idle.
unsigned init();

// Log the "[FC] idle ..." line again with the lines' current drive; main repeats it in the heartbeat so a
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
