// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the crossed-CC detector (raguard::CrossTrace): a cable's CC wire lands on a different K2 line than the
// other cable's, so a charger's Rp and a phone's toggling meet no partner. The traces are the levels measured on the bench
// rig (Anker IQ4 -> C-C cable -> K2 -> C-C cable -> Pixel left, VBUS dark, 2026-11-10) and the negatives that must NOT read
// crossed: a charger alone, a dual-role phone alone.
#include "k2/pd/cc_crossed.hpp"
#include "test_util.h"

using k2::raguard::CrossKind;
using k2::raguard::CrossTrace;

namespace {

constexpr std::uint32_t kTick = 20;   // the UI tick

// The Pixel's toggling line on the rig: Rp (3.3 V) x3, open, Rd-ish 1.2 V, 1.8 V x2 per 160 ms cycle.
constexpr std::int32_t kPixelCycle[8] = {3300, 3300, 3300, 0, 0, 1200, 1800, 1800};
std::int32_t pixel(std::uint32_t t) { return kPixelCycle[(t / kTick) % 8]; }
// The Anker's Rp loaded by the Pixel cable's e-marker Ra: steady about 500 mV with a little jitter.
std::int32_t loaded(std::uint32_t t) { return 480 + std::int32_t((t / kTick) % 5) * 20; }
// A steady charger Rp (the ADC saturates at the rail) with jitter.
std::int32_t rp(std::uint32_t t) { return 3300 - std::int32_t((t / kTick) % 3) * 10; }
// A dual-role phone alone: Rp 3.3 V on, open off, every 100 ms.
std::int32_t drp(std::uint32_t t) { return (t / 100) % 2 == 0 ? 3300 : 0; }

template <class F1, class F2>
CrossKind run(std::uint32_t t0, std::uint32_t t1, F1 f1, F2 f2, CrossTrace *keep = nullptr) {
    CrossTrace local;
    CrossTrace &tr = keep ? *keep : local;
    std::uint32_t t = t0;
    for (; t <= t1; t += kTick) tr.add(t, f1(t), f2(t));
    return tr.classify(t - kTick);
}

}  // namespace

// Case 1 (the rig): CC2 steady 0.2-0.8 V (Rp loaded by the cable's Ra) and CC1 toggling = crossed.
static void test_rig_trace_is_crossed() {
    CHECK_EQ(int(run(0, 1400, pixel, loaded)), int(CrossKind::Crossed));
    // Mirrored: the toggling line on CC2.
    CHECK_EQ(int(run(0, 1400, loaded, pixel)), int(CrossKind::Crossed));
}

// Case 2: a cable without an e-marker: the charger's steady Rp on one line, the phone toggling on the other.
static void test_plain_cable_is_crossed() {
    CHECK_EQ(int(run(0, 1400, pixel, rp)), int(CrossKind::Crossed));
    CHECK_EQ(int(run(0, 1400, rp, drp)), int(CrossKind::Crossed));
}

// No verdict before the lines have been observed for a second.
static void test_needs_a_second_of_observation() {
    CrossTrace tr;
    CHECK_EQ(int(run(0, 500, pixel, loaded, &tr)), int(CrossKind::None));
    CHECK_EQ(int(run(520, 900, pixel, loaded, &tr)), int(CrossKind::None));
    CHECK_EQ(int(run(920, 1400, pixel, loaded, &tr)), int(CrossKind::Crossed));
    tr.clear();                                   // a gate dropped (VBUS, Rd of ours): the observation starts over
    CHECK_EQ(int(tr.classify(1500)), int(CrossKind::None));
    CHECK_EQ(int(run(1500, 2300, pixel, loaded, &tr)), int(CrossKind::None));
}

// Case 3: Rp-vs-Ra on one line and nothing on the other: a hint only.
static void test_ra_line_with_nothing_on_the_other_is_a_hint() {
    CHECK_EQ(int(run(0, 1400, [](std::uint32_t) { return 0; }, loaded)), int(CrossKind::Hint));
    CHECK_EQ(int(run(0, 1400, loaded, [](std::uint32_t) { return 1707; })), int(CrossKind::Hint));   // the idle CC2 net
}

// Negative: a charger alone (Rp on one line, the other idle; or a receptacle charger with Rp on both) is not crossed.
static void test_charger_alone_is_not_crossed() {
    CHECK_EQ(int(run(0, 3000, [](std::uint32_t) { return 0; }, rp)), int(CrossKind::None));
    CHECK_EQ(int(run(0, 3000, rp, rp)), int(CrossKind::None));
    CHECK_EQ(int(run(0, 3000, rp, [](std::uint32_t) { return 1707; })), int(CrossKind::None));
}

// Negative: a dual-role phone alone toggles on its line; the other line is idle (or both toggle together).
static void test_drp_phone_alone_is_not_crossed() {
    CHECK_EQ(int(run(0, 3000, drp, [](std::uint32_t) { return 0; })), int(CrossKind::None));
    CHECK_EQ(int(run(0, 3000, drp, [](std::uint32_t) { return 1707; })), int(CrossKind::None));
    CHECK_EQ(int(run(0, 3000, drp, drp)), int(CrossKind::None));
    CHECK_EQ(int(run(0, 3000, pixel, pixel)), int(CrossKind::None));   // the Pixel alone: both lines cycle together
}

// Nothing plugged in, or an unplug in the middle of the window (one edge, not a toggle).
static void test_nothing_and_a_single_edge_are_not_crossed() {
    const auto zero = [](std::uint32_t) { return 0; };
    CHECK_EQ(int(run(0, 3000, zero, zero)), int(CrossKind::None));
    const auto unplugged = [](std::uint32_t t) { return t < 1000 ? 3300 : 0; };
    CHECK_EQ(int(run(0, 2000, unplugged, loaded)), int(CrossKind::None));
}

int main() {
    test_rig_trace_is_crossed();
    test_plain_cable_is_crossed();
    test_needs_a_second_of_observation();
    test_ra_line_with_nothing_on_the_other_is_a_hint();
    test_charger_alone_is_not_crossed();
    test_drp_phone_alone_is_not_crossed();
    test_nothing_and_a_single_edge_are_not_crossed();
    return test_report("test_cc_crossed");
}
