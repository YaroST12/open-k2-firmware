// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Active fast-charge trigger driver: binds the pure engine (fc_engine.hpp) to the hardware. It
// reads VBUS from the power monitor and D+/D- from the ADC, and drives the lines through k2::dpdm.
// Default state is passive idle; nothing is triggered until start() is called with a confirmation
// for any target above 5 V. stop() / any error returns the output to 5 V and the passive idle state.
#pragma once
#include <cstdint>

#include <span>

#include "k2/fastcharge/accp.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/fastcharge/fc_scan.hpp"
#include "k2/fastcharge/ufcs_probe.hpp"
#include "k2/fastcharge/ufcs_tune.hpp"

namespace k2::fctrig {

using fc::Fail;
using fc::Proto;
using fc::State;

// A PD contract or a load behind the K2 (k2::link) refuses with Fail::Inline; `allow_inline` is the
// explicit override and never stands in for `confirmed` (needed above 5 V).
Fail start(Proto p, int target_mv, bool confirmed, bool allow_inline = false, int scp_ma = accp::kScpInitMa, int ufcs_ma = 0);
Fail set_target(int target_mv, bool confirmed, int ufcs_ma = 0);
void stop();       // BACK / leaving the page / remote reset: discharge to 5 V, then passive idle

// Call every main-loop iteration: runs the engine and the dpdm reset hold; logs state changes
// as "[FC] ...".
void poll();

// The UFCS message exchange only (engine.service_ufcs()); called about once per ms from the LCD idle hook so a source's
// 10 ms ACK window holds through a redraw. Does nothing while no UFCS trigger runs.
void service_ufcs();

// The engine is inside a window only the main loop serves (Engine::paint_critical): the page paints nothing then.
bool paint_critical();

// One-click auto-detect: FCP, SCP, AFC, QC2, QC3, VOOC and QC3+ in turn, never above 9 V, back to 5 V after
// each step. Needs `confirmed`; Busy while a trigger runs. stop() aborts it.
Fail scan_start(bool confirmed, bool allow_inline = false);
bool scan_running();
bool scan_done();
std::size_t scan_step();                         // index of the running step (0..kScanSteps-1)
Fail scan_fail();
std::span<const fc::ScanResult> scan_results();

// Raw ACCP bus access for the protocol lab (shell). Same physical exchange as the engine (3 tries by
// default); no state checks, the caller owns the safety rules. Leaves D+ 0.6 V, D- 0 V.
struct LabXfer {
    bool ok;               // the exchange completed (slave pings and a full RX frame); frame validity is the caller's
    std::uint8_t tries;
    std::uint32_t us;      // wall time of all tries
};
LabXfer lab_ping(int retries);
LabXfer lab_xfer(std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx, int retries);   // rx.size() <= 3

// UFCS tuner and probe (k2/fastcharge/ufcs_tune.hpp, k2/fastcharge/ufcs_probe.hpp). The tuner is RAM only and reset at boot;
// it shapes the engine's UFCS frames and the probe. The probe runs only the handshake and the opener
// exchange at 5 V, never raises the voltage and never sends a Request; it is polled by poll().
const ufcs::Tuner& ufcs_tuner();
enum class TuneSet : std::uint8_t { Ok, Range, Busy };   // Busy: a probe is running
TuneSet ufcs_set(ufcs::Field f, std::uint32_t v);
bool ufcs_tuner_reset();                                  // false while a probe runs
// Busy when a trigger, scan or the edge sniffer runs, Resetting while the previous probe's bus reset hold
// lasts, Inline / NotFiveVolts / NoMonitor from the probe's own guards.
ufcs::ProbeStart ufcs_probe_start();
bool ufcs_probe_running();
bool ufcs_probe_done();
void ufcs_probe_stop();
std::span<const std::uint8_t> ufcs_probe_blob();          // layout in ufcs_probe.hpp
const ufcs::ProbeResult& ufcs_probe_result();

struct Status {
    State state;
    Proto proto;
    Fail fail;          // the engine failure, or the reason the last start was refused while Idle
    int target_mv;
    int tries;          // VOOC family wake trains sent so far, else 0
    int vbus_mv;        // live INA VBUS in every state (the engine value only if the monitor is offline)
    int adapter_mv = 0; // SCP: the adapter's own voltage readback of this session (0 = not read yet, or not SCP)
};
Status status();
// What the engine's SCP / UFCS sessions read from the charger (fc::Caps): the planner of the charger test sizes later rows from it.
const fc::Caps& caps();

}  // namespace k2::fctrig
