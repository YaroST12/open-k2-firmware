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
// Timer part: the wire's deadlines (svooc_wire.hpp deadline(): the D- release kHoldUs after a reply's last rise, a cut
// slot, a frame gone quiet, a slot that never came) run from a one-shot timer interrupt (k2/drivers/oneshot.hpp), armed
// after every edge and every tick, at the same NVIC priority as the edge ISR. So the line timing does not depend on the
// main loop: a stalled one (a full repaint, a flash write) no longer holds D- into the adapter's next command.
// Main-loop part: poll() every pass (via fctrig::poll) and service() about every ms (the LCD idle hook, via
// fctrig::service_ufcs) tick the wire as well (the fallback); poll() also runs the guards and hands the latest VBUS
// sample to the wire for the closed-loop 0x82 word (svooc_script.hpp), so the guards still want a main loop that runs:
// it writes no flash while running() (no Black Box save, no ring erase; src/core/main_loop.cpp). Report::poll_gap_max_ms
// shows the longest gap it had, Snapshot::stall which stage it was and when (k2/core/stage_meter.hpp), and the
// rel_* counters who let D- go after each reply.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/stage_meter.hpp"
#include "k2/fastcharge/svooc_run.hpp"
#include "k2/fastcharge/svooc_wire.hpp"

namespace k2::svooc_trig {

// Starts the run (svooc_run.hpp refusal() on the live facts). `ports_busy` is the caller's: a charger test, a PD Stress
// run or another tool that owns the ports; the glue adds the fast-charge engine, scan, probe, Black Box arm / stream and
// a line not at the passive idle. The passive VOOC monitor is no obstacle: once every other check passes the trigger
// stops it (dpsniff::monitor_yield) and takes the lines; fastcharge::poll() keeps it from re-arming while running() and
// lets it resume after the run. cap_s: 0 = the full kCapMs, else 1..20 s.
// usb_a: the operator's waiver of the far-sink refusal for a legacy A port (svooc_run.hpp; refused when PD is active).
// params: the responder's operator knobs (svooc_script.hpp Params: the 0x86 current target, the constant or closed-loop
// 0x82 word, the escape); clamped there, the effective values are in Snapshot::params. No guard depends on them.
svooc::Refusal start(bool confirmed, bool ports_busy, std::uint32_t cap_s, bool usb_a = false,
                     const svooc::Params& params = {});
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
    svooc::Params params{};                          // the effective (clamped) responder parameters
    svooc::Escape escape = svooc::Escape::Off;       // what came of the escape (Params::esc_at_s)
    std::uint32_t escape_t_ms = 0;                   // when its 0x07F was sent, ms since the wake
    std::uint16_t rec_edges = 0;                     // the run's Black Box edge recording (dpsniff::status()): edges kept
    std::uint8_t rec_end = 0;                        // and how it ended (edges::End; 0 = still recording)
    stage::Worst stall{};                            // the main loop's longest stage since the wake (frozen at the release)
    std::uint32_t rel_timer = 0, rel_loop = 0, rel_edge = 0;   // who let D- go: the timer, the main loop, the next frame
};
Snapshot snapshot();   // a copy, interrupts masked for the wire's part

// The command>reply pairs of the latest wake (at most svooc::kLogPairs), from index `from`, with the monitor's VBUS when
// each pair closed. Returns the rows written (out.size() at most); the list stays readable after the run until the next
// start.
std::size_t read_pairs(std::uint16_t from, std::span<svooc::PairRow> out);

}  // namespace k2::svooc_trig
