// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger, the firmware glue: binds the pure parts (svooc_script.hpp the replies, svooc_wire.hpp the
// bits, svooc_run.hpp the guards) to the D+/D- edge ISR (k2/fastcharge/dpsniff.hpp hook), the line driver (k2::dpdm),
// the power monitor, the FUSB302 and the USB link. Reachable from the HID remote only (`k2_remote.py fc svooc-trigger
// --confirm`): no UI entry, and the remote's dead-man lease holds while it runs (an expiry or a lost host calls stop()).
//
// NO LOAD: the K2 stands in for a phone on D+/D- while nothing draws current; the adapter may raise VBUS, no power
// flows. The guards and the exit check are svooc_run.hpp's; this file only gathers the facts and carries out each svooc::Next.
//
// ISR part (the hook, every D+/D- edge): Wire::edge() and at most one dpdm::set_dm(). A bit is set kBitDelayUs (50 us)
// after the D+ rise it belongs to: the ISR busy-waits from the edge's own cycle stamp, so the wait is 50 us minus the
// interrupt latency and never longer. Latency budget: all interrupts run at the same NVIC priority (none is set), so
// the EXTI handler may wait behind a USB or SysTick handler (a few to tens of us) or a masked section in the main
// loop (dpdm::play masks up to 20 ms, but nothing else plays steps while the trigger owns the lines). A late bit is
// still in time while it lands before the adapter latches it, just before the next fall (0.43-0.51 ms after the
// rise): ~380 us of slack. Command bits are taken from the D- state the previous edge left (svooc_wire.hpp), so a late
// ISR does not misread them either. No logging in the ISR.
// Main-loop part: poll() every pass (via fctrig::poll) and service() about every ms (the LCD idle hook, via
// fctrig::service_ufcs) let go of D- after the last bit's hold and expire slots; poll() also runs the guards.
#pragma once
#include <cstdint>

#include "k2/fastcharge/svooc_run.hpp"
#include "k2/fastcharge/svooc_wire.hpp"

namespace k2::svooc_trig {

// Starts the run (svooc_run.hpp refusal() on the live facts). `ports_busy` is the caller's: a charger test, a PD Stress
// run or another tool that owns the ports; the glue adds the fast-charge engine, scan, probe, sniffer, monitor and a
// line not at the passive idle. cap_s: 0 = the full kCapMs, else 1..20 s.
svooc::Refusal start(bool confirmed, bool ports_busy, std::uint32_t cap_s);
void stop();         // stop answering, lines to passive idle (reason Stopped); nothing while not running
void poll();         // main loop
void service();      // ~1 ms: the wire's tick only
bool running();      // the lines are held, or the exit watch still runs: nothing else may take D+/D-
bool lines_held();

struct Snapshot {
    svooc::Stage stage = svooc::Stage::Idle;
    svooc::Report report{};
    svooc::Phase phase = svooc::Phase::Ident;
    std::uint8_t off_run = 0;
    bool driving = false;
    svooc::Log log{};
};
Snapshot snapshot();   // a copy, interrupts masked for the wire's part

}  // namespace k2::svooc_trig
