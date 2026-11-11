// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress score: a penalty, 0 = ideal. Every event the run sees has one weight; the score is the weighted event total per 100
// transitions. Pure code over counters (host-tested in tests/test_pdstress_score.cpp). The weights are one constexpr table: retune it here
// and nowhere else.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::pdstress {

// Everything the scorer counts. Order is the wire / journal order: append only.
enum class Event : std::uint8_t {
    AcceptLate,      // Request -> Accept slower than the spec limit (pd::spec::kAcceptLimitMs)
    PsRdyLate,       // Accept -> PS_RDY slower than tPSTransition
    PsRdyOutOfBand,  // PS_RDY arrived while VBUS was still outside the +-5 % band of the target
    WaitRx,          // Wait from the source for an advertised level
    RejectRx,        // Reject from the source for an advertised level
    CapsReannounce,  // Source_Capabilities nobody asked for, inside the contract
    VbusBand,        // VBUS overshoot, or out of the band after the source's settle time
    SoftResetRx,     // Soft_Reset from the source
    HardResetRx,     // Hard Reset from the source
    VbusDropout,     // VBUS fell below the dropout line during the run
    ContractLost,    // the contract ended without a Hard Reset from the source (timeout, no GoodCRC, K2 Hard Reset)
    NoRecovery,      // VBUS did not come back without a replug: the run stops here
    Count
};
inline constexpr std::size_t kEventCount = static_cast<std::size_t>(Event::Count);

// Penalty points per event. PROVISIONAL, see docs/PD_STRESS.md: starting values (operator decision 2026-11-11), to be settled after
// validation runs on several chargers. This is the only table; tools/pd_stress.py reads it for its default weights, and
// `pd_stress.py rescore` re-scores saved results under other weights without a rebuild.
inline constexpr std::array<std::uint16_t, kEventCount> kWeights = {
    1,    // AcceptLate
    1,    // PsRdyLate
    2,    // PsRdyOutOfBand
    2,    // WaitRx
    2,    // RejectRx
    2,    // CapsReannounce
    3,    // VbusBand
    5,    // SoftResetRx
    20,   // HardResetRx
    30,   // VbusDropout
    30,   // ContractLost
    100,  // NoRecovery
};

constexpr const char* event_name(Event e) {
    switch (e) {
        case Event::AcceptLate: return "accept_late";
        case Event::PsRdyLate: return "psrdy_late";
        case Event::PsRdyOutOfBand: return "psrdy_oob";
        case Event::WaitRx: return "wait";
        case Event::RejectRx: return "reject";
        case Event::CapsReannounce: return "caps_unsol";
        case Event::VbusBand: return "vbus_band";
        case Event::SoftResetRx: return "soft_reset";
        case Event::HardResetRx: return "hard_reset";
        case Event::VbusDropout: return "dropout";
        case Event::ContractLost: return "contract_lost";
        case Event::NoRecovery: return "no_recovery";
        case Event::Count: break;
    }
    return "?";
}

// Counters of one sequence or of the whole run. `transitions` = Requests (and Get_Source_Cap / Soft_Reset exchanges) that went out.
struct Counts {
    std::uint16_t transitions = 0;
    std::array<std::uint16_t, kEventCount> n{};
    std::uint16_t get(Event e) const { return n[static_cast<std::size_t>(e)]; }
    void add(const Counts& o) {   // saturating
        auto sat = [](std::uint32_t v) { return std::uint16_t(v > 0xFFFF ? 0xFFFF : v); };
        transitions = sat(std::uint32_t(transitions) + o.transitions);
        for (std::size_t i = 0; i < kEventCount; ++i) n[i] = sat(std::uint32_t(n[i]) + o.n[i]);
    }
};

// A run shorter than this still divides by it, so one early event in a handful of transitions does not read as hundreds of points.
inline constexpr std::uint32_t kMinTransitions = 10;

// Weighted event total (no normalisation).
constexpr std::uint32_t penalty_points(const Counts& c) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < kEventCount; ++i) sum += std::uint32_t(c.n[i]) * kWeights[i];
    return sum;
}

// Points one event kind contributed (count x weight): the per-kind breakdown the result carries.
constexpr std::uint32_t event_points(const Counts& c, Event e) {
    return std::uint32_t(c.get(e)) * kWeights[static_cast<std::size_t>(e)];
}

// The score in hundredths of a point: penalty per 100 transitions, x100 (1234 = 12.34). 0 = ideal.
constexpr std::uint32_t score_cp(const Counts& c) {
    const std::uint64_t t = c.transitions < kMinTransitions ? kMinTransitions : c.transitions;
    const std::uint64_t cp = std::uint64_t(penalty_points(c)) * 100u * 100u / t;
    return cp > 0xFFFFFFFFull ? 0xFFFFFFFFu : std::uint32_t(cp);
}

// True when the counts say the run cannot go on without a replug.
constexpr bool run_stops(const Counts& c) { return c.get(Event::NoRecovery) != 0; }

}  // namespace k2::pdstress
