// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The D+/D- switch network driver's idle state is a fixed value that the fast-charge
// classifier checks at boot: the two must agree, or "[FC] idle ... mismatch" fires on a healthy board.
#include "k2/drivers/dpdm.hpp"
#include "k2/fastcharge/fastcharge_classify.hpp"
#include "test_util.h"

using namespace k2;

static void test_passive_idle_levels() {
    CHECK_EQ(dpdm::kPassiveIdle.pa7, true);
    CHECK_EQ(dpdm::kPassiveIdle.pb0, false);
    CHECK_EQ(dpdm::kPassiveIdle.sw1, true);
    CHECK_EQ(dpdm::kPassiveIdle.sw2, false);
}

static void test_idle_agrees_with_classifier_check() {
    const auto& l = dpdm::kPassiveIdle;
    CHECK_EQ(fastcharge::idle_mismatch(l.pa7, l.pb0, l.sw1, l.sw2), 0u);
}

static void test_mismatch_reports_each_pin() {
    const auto& l = dpdm::kPassiveIdle;
    CHECK_EQ(dpdm::mismatch({!l.pa7, l.pb0, l.sw1, l.sw2}), 1u);
    CHECK_EQ(dpdm::mismatch({l.pa7, !l.pb0, l.sw1, l.sw2}), 2u);
    CHECK_EQ(dpdm::mismatch({l.pa7, l.pb0, !l.sw1, l.sw2}), 4u);
    CHECK_EQ(dpdm::mismatch({l.pa7, l.pb0, l.sw1, !l.sw2}), 8u);
    CHECK_EQ(dpdm::mismatch(l), 0u);
    // The old i2c.cpp idle state (PB2 high) is the one mismatch the log used to show.
    CHECK_EQ(dpdm::mismatch({true, false, true, true}), 8u);
}

// Spec 1.2 / 1.3: each drive primitive is a (mode, PA7/PB1 level, PB0/PB2 level) triple.
static void test_dp_drive_truth_table() {
    using dpdm::Drive; using dpdm::Kind;
    auto d = [](Drive x) { return dpdm::dp_pins(x); };
    CHECK_EQ(d(Drive::HiZ).kind == Kind::Analog, true);
    CHECK_EQ(d(Drive::V0).kind == Kind::Out && !d(Drive::V0).a && !d(Drive::V0).b, true);
    CHECK_EQ(d(Drive::V0p6).kind == Kind::Out && d(Drive::V0p6).a && !d(Drive::V0p6).b, true);
    CHECK_EQ(d(Drive::V2p0).kind == Kind::Out && !d(Drive::V2p0).a && d(Drive::V2p0).b, true);
    CHECK_EQ(d(Drive::V3p3).kind == Kind::Out && d(Drive::V3p3).a && d(Drive::V3p3).b, true);
    CHECK_EQ(d(Drive::Listen).kind == Kind::In, true);   // VOOC clock: PA7 input, PB0 analog
}

static void test_dm_drive_truth_table() {
    using dpdm::Drive; using dpdm::Kind;
    auto d = [](Drive x) { return dpdm::dm_pins(x); };
    CHECK_EQ(d(Drive::HiZ).kind == Kind::Analog, true);
    CHECK_EQ(d(Drive::V0).kind == Kind::Out && !d(Drive::V0).a && !d(Drive::V0).b, true);
    CHECK_EQ(d(Drive::V0p6).kind == Kind::Out && d(Drive::V0p6).a && !d(Drive::V0p6).b, true);
    CHECK_EQ(d(Drive::V2p0).kind == Kind::Out && !d(Drive::V2p0).a && d(Drive::V2p0).b, true);
    CHECK_EQ(d(Drive::V3p3).kind == Kind::Out && d(Drive::V3p3).a && d(Drive::V3p3).b, true);
    // ACCP RX: PB1 low output, PB2 input with pull-down.
    CHECK_EQ(d(Drive::RxPulldown).kind == Kind::RxPull && !d(Drive::RxPulldown).a, true);
    CHECK_EQ(d(Drive::Listen).kind == Kind::Analog, true);   // D- has no listen state: released
}

// The passive idle state is both lines at the 0.6 V level (PA7=1 PB0=0 | PB1=1 PB2=0).
static void test_idle_is_06v_on_both_lines() {
    using dpdm::Drive;
    const auto dp = dpdm::dp_pins(Drive::V0p6);
    const auto dm = dpdm::dm_pins(Drive::V0p6);
    CHECK_EQ(dp.a == dpdm::kPassiveIdle.pa7 && dp.b == dpdm::kPassiveIdle.pb0, true);
    CHECK_EQ(dm.a == dpdm::kPassiveIdle.sw1 && dm.b == dpdm::kPassiveIdle.sw2, true);
}

int main() {
    test_dp_drive_truth_table();
    test_dm_drive_truth_table();
    test_idle_is_06v_on_both_lines();
    test_passive_idle_levels();
    test_idle_agrees_with_classifier_check();
    test_mismatch_reports_each_pin();
    return test_report("test_dpdm");
}
