// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The SuperVOOC world for the host world replay (tests/test_world_svooc.cpp): a SUPERVOOC adapter on the K2's USB-A D+/D-,
// the real trigger glue (src/fastcharge/svooc_trig.cpp) with its real wire, responder and run guards, and the interrupts that
// drive them (the D+/D- EXTI and the one-shot timer) on the world's clock (tests/world_sim.h IrqSource). The fakes are the lines,
// the adapter and the interrupt controller, never the firmware:
//
//   - Lines: D+ is the adapter's (the K2 drives it only for the wake burst); D- is the K2's while it drives it (dpdm::set_dm),
//     else the adapter's in its own command frames, else idle high (floating). Every level change while the trigger listens is
//     an EXTI edge; one ISR clears every pending edge (dpsniff isr_body), and reads the levels and the cycle counter at entry.
//   - Adapter (svooc_wire.hpp framing, reverse-engineered from the OnePlus 15 + SUPERVOOC 120 W captures): after the wake
//     burst D+ low for kIdentDelayMs, then each command frame as 9 low pulses (30 us wide, 460 / 540 us apart, D- bit k 28 us
//     after fall k, the trailing 0), its reply slot 9 ms after the command's start (9 pulses; D- latched 10 us before each next
//     fall, the ninth 300 us after the last rise). The command list and times are hardware run 8's (2026-11-12) up to the
//     0x86, then 0x82 every 44 ms. Before each command it checks that the K2 has let D- go: if not, it ends the session (no
//     more clock, VBUS back to 5 V), as the charger did on the bench.
//   - VBUS: run 8's (5.27 V, the ramp to 11.1 V after the handshake), back to 5.27 V 300 ms after the session ends.
//   - Main-loop stalls (the experiment): a stretch of firmware time at a given ms after the wake, with interrupts open (a long
//     paint) or masked (a flash erase), injected in the shell poll (stage "shell" in the stage meter).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wsvooc {

inline constexpr double kIdentDelayMs = 540;   // wake burst end -> first ident's rise (the 2026-11-11 capture)

struct Config {
    bool timer = true;          // the one-shot timer fires; false = the firmware before it (only the main loop lets D- go)
    double stall_at_ms = -1;    // a main-loop stall this long after the wake burst (firmware ms); < 0: none
    double stall_ms = 0;
    bool stall_masked = false;  // with interrupts masked (a flash erase) instead of open (a long paint)
    double isr_entry_us = 0.3;  // interrupt entry to the handler's first cycle-counter read
};

struct Pair {
    std::uint8_t cmd = 0;
    std::uint16_t word = 0;     // the nine bits the adapter latched in the slot
    double t_ms = 0;            // the command's start, ms after the wake burst
};

struct Result {
    double wake_end_ms = -1;          // firmware ms the wake burst ended (-1: no wake)
    std::vector<Pair> pairs;          // every command the adapter sent and what it latched
    bool ended = false;               // the adapter ended the session
    double ended_ms = -1;             // when (ms after the wake)
    std::string why;
    unsigned contention = 0;          // adapter and K2 drove D- to different levels at once
    double hold_max_us = 0;           // the longest time from a slot's last rise to the K2 letting D- go
    double hold_max_at_ms = 0;
    unsigned edges = 0, timer_shots = 0;
};

void install(const Config& c);    // after world::start: the lines, the adapter, the interrupt source
void uninstall();
const Result& result();
double wake_ms();                 // the wake burst's end (firmware ms), -1 before it

}  // namespace wsvooc
