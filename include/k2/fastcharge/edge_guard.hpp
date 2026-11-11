// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Edge-storm guard of the D+/D- EXTI ISR (dpsniff), pure (tests/test_edge_guard.cpp). It only counts cycle stamps: no
// pin, register or driver is in reach (tests/test_vooc_decode_isolation.py checks the includes).
//
// Why: the ISR takes one interrupt per D+ or D- edge. A USB 2.0 data link through the K2 (PC -> K2 -> phone or drive)
// toggles the lines at MHz rates, the EXTI pending bit is set again before the handler returns, the handler tail-chains
// and the main loop (PD, USB, LCD) stops. Every user of the ISR is guarded: the inline VOOC monitor (automatic) and the
// Black Box `sniff arm` / `sniff stream` (explicit), because they share the handler.
//
// The numbers (reverse-engineered from runs/2026-11-11-op15-svooc/cap1.edges, 81,980 interrupts over 95 s):
//   VOOC peaks at 9 interrupts in any 1 ms (the phone's wake burst: both lines every ~300 us) and 1,340 in any 1 s
//   (handshake plus the 0x82 poll); steady ~1,030 edges/s.
//   - short window 1 ms, more than 32 interrupts trips (3.5x the VOOC peak): catches a back-to-back ISR train within
//     kStormWorstIrqs = 65 interrupts, whatever the window's phase (32 in the running window + 33 in the next);
//   - long window 2^28 cycles (1,118 ms), more than 4,096 trips (3x the VOOC peak per second): catches lighter traffic
//     that stays under 32 per ms (start-of-frame packets every 1 ms) and caps what passes the guard to about
//     3,700 interrupts/s on average.
// Both are O(1) in the ISR: two unsigned differences, two increments, two compares; no division (the long window is a
// power of two cycles only to keep its constant readable).
#pragma once
#include <array>
#include <cstdint>

namespace k2::edges {

inline constexpr std::uint32_t kGuardCyclesPerMs = 240000;   // the DWT counter at 240 MHz
inline constexpr std::uint32_t kStormShortCycles = kGuardCyclesPerMs;
inline constexpr std::uint32_t kStormShortMax = 32;
inline constexpr std::uint32_t kStormLongShift = 28;
inline constexpr std::uint32_t kStormLongCycles = 1u << kStormLongShift;
inline constexpr std::uint32_t kStormLongMax = 4096;
inline constexpr std::uint32_t kStormWorstIrqs = 2 * kStormShortMax + 1;   // interrupts before the mask, worst phase

// The window that tripped: interrupts counted in it and the cycles from its start to the trip.
struct Storm {
    std::uint32_t edges = 0;
    std::uint32_t cycles = 0;
    std::uint32_t ms() const {   // rounded up, at least 1 (main loop only: it divides)
        const std::uint32_t m = (cycles + kGuardCyclesPerMs - 1) / kGuardCyclesPerMs;
        return m ? m : 1;
    }
};

// One per ISR. hit() runs in the ISR; reset() and tick() in the main loop with the ISR masked.
class StormGuard {
public:
    void reset(std::uint32_t t) { *this = StormGuard{}; s0_ = l0_ = t; }
    // One interrupt at cycle `t`. True = storm: the caller masks its EXTI lines now. Stays true until reset().
    bool hit(std::uint32_t t) {
        if (tripped_) return true;
        expire(t);
        ++sn_;
        ++ln_;
        if (sn_ > kStormShortMax) trip(sn_, t - s0_);
        else if (ln_ > kStormLongMax) trip(ln_, t - l0_);
        return tripped_;
    }
    // Main loop: close the windows whose time has passed, so a count from 2^32 cycles (17.9 s) ago cannot alias.
    void tick(std::uint32_t now) { if (!tripped_) expire(now); }
    bool tripped() const { return tripped_; }
    Storm last() const { return last_; }

private:
    void expire(std::uint32_t t) {
        if (t - s0_ >= kStormShortCycles) { s0_ = t; sn_ = 0; }
        if (t - l0_ >= kStormLongCycles) { l0_ = t; ln_ = 0; }
    }
    void trip(std::uint32_t n, std::uint32_t cycles) { tripped_ = true; last_ = {n, cycles}; }

    std::uint32_t s0_ = 0, l0_ = 0, sn_ = 0, ln_ = 0;
    bool tripped_ = false;
    Storm last_{};
};

// The inline monitor's back-off after a storm (main loop, milliseconds): paused 1 s, 5 s, then 30 s for every further
// storm; the lines stay floating and unmasked only for a probe. A probe that sees no storm for kProbeMs (longer than one
// long window) is quiet lines: the monitor runs again. kCalmMs of running without a storm forgets the back-off level.
inline constexpr std::array<std::uint32_t, 3> kBackoffMs{1000, 5000, 30000};
inline constexpr std::uint32_t kProbeMs = 2000;
inline constexpr std::uint32_t kCalmMs = 60000;

class Backoff {
public:
    enum class State : std::uint8_t { Run, Paused, Probe };
    void reset() { *this = Backoff{}; }
    // A storm at `now`: pause. Returns the wait in ms.
    std::uint32_t trip(std::uint32_t now) {
        calm(now);
        if (level_ < kBackoffMs.size()) ++level_;
        wait_ = kBackoffMs[level_ - 1];
        since_ = now;
        state_ = State::Paused;
        if (storms_ < 0xFFFF) ++storms_;
        return wait_;
    }
    // Main loop. True once when the pause is over: the caller unmasks the lines for a probe.
    bool poll(std::uint32_t now) {
        if (state_ == State::Paused && now - since_ >= wait_) { state_ = State::Probe; since_ = now; return true; }
        if (state_ == State::Probe && now - since_ >= kProbeMs) { state_ = State::Run; since_ = now; }
        calm(now);
        return false;
    }
    State state() const { return state_; }
    bool paused() const { return state_ == State::Paused; }
    std::uint16_t storms() const { return storms_; }
    std::uint32_t retry_in(std::uint32_t now) const {
        if (state_ != State::Paused) return 0;
        const std::uint32_t gone = now - since_;
        return gone >= wait_ ? 0 : wait_ - gone;
    }

private:
    void calm(std::uint32_t now) { if (state_ == State::Run && level_ && now - since_ >= kCalmMs) level_ = 0; }

    State state_ = State::Run;
    std::uint8_t level_ = 0;
    std::uint16_t storms_ = 0;
    std::uint32_t since_ = 0, wait_ = 0;
};

// The readout (remote, shell): the monitor's pause state and the last storm.
struct GuardInfo {
    bool paused = false;
    std::uint16_t storms = 0;      // storms since boot (monitor and sniff)
    std::uint16_t edges = 0;       // interrupts in the window that tripped last
    std::uint16_t ms = 0;          // that window's length up to the trip
    std::uint32_t retry_ms = 0;    // the monitor's wait left while paused
};

}  // namespace k2::edges
