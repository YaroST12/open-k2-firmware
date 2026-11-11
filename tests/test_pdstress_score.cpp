// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress score: one test per event kind, the normalisation per 100 transitions, the transition floor, the per-kind breakdown the result
// carries and the "run stops" case.
#include <string_view>

#include "k2/pdstress/score.hpp"
#include "test_util.h"

using namespace k2::pdstress;

namespace {

Counts with(Event e, unsigned n, unsigned transitions = 100) {
    Counts c;
    c.transitions = std::uint16_t(transitions);
    c.n[static_cast<std::size_t>(e)] = std::uint16_t(n);
    return c;
}

void weights_table() {
    // The operator's starting weights (2026-11-11), one table.
    CHECK_EQ(kWeights[std::size_t(Event::AcceptLate)], 1);
    CHECK_EQ(kWeights[std::size_t(Event::PsRdyLate)], 1);
    CHECK_EQ(kWeights[std::size_t(Event::PsRdyOutOfBand)], 2);
    CHECK_EQ(kWeights[std::size_t(Event::WaitRx)], 2);
    CHECK_EQ(kWeights[std::size_t(Event::RejectRx)], 2);
    CHECK_EQ(kWeights[std::size_t(Event::CapsReannounce)], 2);
    CHECK_EQ(kWeights[std::size_t(Event::VbusBand)], 3);
    CHECK_EQ(kWeights[std::size_t(Event::SoftResetRx)], 5);
    CHECK_EQ(kWeights[std::size_t(Event::HardResetRx)], 20);
    CHECK_EQ(kWeights[std::size_t(Event::VbusDropout)], 30);
    CHECK_EQ(kWeights[std::size_t(Event::ContractLost)], 30);
    CHECK_EQ(kWeights[std::size_t(Event::NoRecovery)], 100);
    CHECK_EQ(kEventCount, 12);
}

// One test per event kind: a single event in 100 transitions scores its weight (x100 in hundredths).
void per_event_kind() {
    for (std::size_t i = 0; i < kEventCount; ++i) {
        const Event e = Event(i);
        const Counts c = with(e, 1);
        CHECK_EQ(penalty_points(c), kWeights[i]);
        CHECK_EQ(score_cp(c), std::uint32_t(kWeights[i]) * 100u);
        CHECK_EQ(event_points(c, e), kWeights[i]);
        CHECK_COND(std::string_view(event_name(e)) != "?");
    }
    CHECK_EQ(score_cp(with(Event::AcceptLate, 1)), 100);
    CHECK_EQ(score_cp(with(Event::PsRdyLate, 1)), 100);
    CHECK_EQ(score_cp(with(Event::PsRdyOutOfBand, 1)), 200);
    CHECK_EQ(score_cp(with(Event::WaitRx, 1)), 200);
    CHECK_EQ(score_cp(with(Event::RejectRx, 1)), 200);
    CHECK_EQ(score_cp(with(Event::CapsReannounce, 1)), 200);
    CHECK_EQ(score_cp(with(Event::VbusBand, 1)), 300);
    CHECK_EQ(score_cp(with(Event::SoftResetRx, 1)), 500);
    CHECK_EQ(score_cp(with(Event::HardResetRx, 1)), 2000);
    CHECK_EQ(score_cp(with(Event::VbusDropout, 1)), 3000);
    CHECK_EQ(score_cp(with(Event::ContractLost, 1)), 3000);
    CHECK_EQ(score_cp(with(Event::NoRecovery, 1)), 10000);
}

void clean_is_zero() {
    Counts c;
    c.transitions = 500;
    CHECK_EQ(penalty_points(c), 0);
    CHECK_EQ(score_cp(c), 0);
    CHECK_EQ(score_cp(Counts{}), 0);
}

void normalisation() {
    // Per 100 transitions: 2 resets in 200 transitions = 1 per 100.
    CHECK_EQ(score_cp(with(Event::HardResetRx, 2, 200)), 2000);
    CHECK_EQ(score_cp(with(Event::HardResetRx, 1, 400)), 500);
    CHECK_EQ(score_cp(with(Event::HardResetRx, 1, 50)), 4000);
    // Mixed events add before dividing: 3 waits (6) + 1 soft reset (5) + 2 band (6) = 17 over 340 transitions = 5.00.
    Counts c;
    c.transitions = 340;
    c.n[std::size_t(Event::WaitRx)] = 3;
    c.n[std::size_t(Event::SoftResetRx)] = 1;
    c.n[std::size_t(Event::VbusBand)] = 2;
    CHECK_EQ(penalty_points(c), 17);
    CHECK_EQ(score_cp(c), 500);
    // Large counts do not overflow.
    Counts big;
    big.transitions = 0xFFFF;
    for (auto& n : big.n) n = 0xFFFF;
    CHECK_COND(score_cp(big) > 0);
}

void transition_floor() {
    // A run shorter than kMinTransitions divides by kMinTransitions: one dropout in 3 transitions is not 1000 points.
    CHECK_EQ(kMinTransitions, 10);
    CHECK_EQ(score_cp(with(Event::VbusDropout, 1, 3)), 30 * 100 * 100 / 10);
    CHECK_EQ(score_cp(with(Event::VbusDropout, 1, 10)), 30 * 100 * 100 / 10);
    CHECK_EQ(score_cp(with(Event::VbusDropout, 1, 0)), 30 * 100 * 100 / 10);
}

void breakdown() {
    // The result carries points and count per kind; they add up to the penalty total.
    Counts c;
    c.transitions = 120;
    c.n[std::size_t(Event::AcceptLate)] = 4;
    c.n[std::size_t(Event::WaitRx)] = 3;
    c.n[std::size_t(Event::HardResetRx)] = 1;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < kEventCount; ++i) sum += event_points(c, Event(i));
    CHECK_EQ(sum, penalty_points(c));
    CHECK_EQ(event_points(c, Event::AcceptLate), 4);
    CHECK_EQ(event_points(c, Event::WaitRx), 6);
    CHECK_EQ(event_points(c, Event::HardResetRx), 20);
    CHECK_EQ(event_points(c, Event::VbusDropout), 0);
}

void saturating_add() {
    Counts a, b;
    a.transitions = 60000;
    b.transitions = 60000;
    a.n[0] = 0xFFFF;
    b.n[0] = 5;
    a.add(b);
    CHECK_EQ(a.transitions, 0xFFFF);
    CHECK_EQ(a.n[0], 0xFFFF);
    Counts x, y;
    x.transitions = 10; y.transitions = 32;
    x.n[3] = 1; y.n[3] = 2;
    x.add(y);
    CHECK_EQ(x.transitions, 42);
    CHECK_EQ(x.n[3], 3);
}

void run_stops_case() {
    // "No recovery without a replug": the run stops; nothing else does.
    CHECK_COND(run_stops(with(Event::NoRecovery, 1)));
    for (std::size_t i = 0; i < kEventCount; ++i) {
        if (Event(i) == Event::NoRecovery) continue;
        CHECK_COND(!run_stops(with(Event(i), 5)));
    }
    CHECK_COND(!run_stops(Counts{}));
    // The score of a stopped run still counts the 100 points (per 100 transitions).
    CHECK_EQ(score_cp(with(Event::NoRecovery, 1, 100)), 10000);
}

}  // namespace

int main() {
    weights_table();
    per_event_kind();
    clean_is_zero();
    normalisation();
    transition_floor();
    breakdown();
    saturating_add();
    run_stops_case();
    return test_report("test_pdstress_score");
}
