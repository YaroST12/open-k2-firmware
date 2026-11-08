// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Qualcomm Quick Charge pure logic:the QC2.0 voltage table, the HVDCP handshake window,
// the QC3.0 / QC3+ pulse trains as timed event lists, replayed against a fake clock.
#include <vector>

#include "k2/fastcharge/fc_safety.hpp"
#include "k2/fastcharge/fc_seq.hpp"
#include "k2/fastcharge/qc.hpp"
#include "test_util.h"

using namespace k2;
using dpdm::Drive;

// Fake clock sink: records (time_us, dp, dm) at every level change.
struct FakeIo {
    std::uint32_t now = 0;
    struct Ev { std::uint32_t t; Drive dp, dm; };
    std::vector<Ev> ev;
    void set(Drive dp, Drive dm) { ev.push_back({now, dp, dm}); }
    void wait_us(std::uint32_t us) { now += us; }
};

static void test_qc2_levels_match_spec_table() {
    // Spec 3.2: 5 V D+0.6/D-0, 9 V D+3.3/D-0.6, 12 V D+0.6/D-0.6, 20 V D+3.3/D-3.3.
    auto l5 = qc::qc2_levels(5000), l9 = qc::qc2_levels(9000), l12 = qc::qc2_levels(12000), l20 = qc::qc2_levels(20000);
    CHECK_EQ(l5.has_value() && l5->dp == Drive::V0p6 && l5->dm == Drive::V0, true);
    CHECK_EQ(l9.has_value() && l9->dp == Drive::V3p3 && l9->dm == Drive::V0p6, true);
    CHECK_EQ(l12.has_value() && l12->dp == Drive::V0p6 && l12->dm == Drive::V0p6, true);
    CHECK_EQ(l20.has_value() && l20->dp == Drive::V3p3 && l20->dm == Drive::V3p3, true);
    CHECK_EQ(qc::qc2_levels(15000).has_value(), false);
    CHECK_EQ(qc::qc2_levels(0).has_value(), false);
}

static void test_qc2_pin_levels_match_spec_gpio_column() {
    // GPIO column of the table: 9 V is PA7=1 PB0=1 | PB1=1 PB2=0.
    const auto l = qc::qc2_levels(9000);
    const auto dp = dpdm::dp_pins(l->dp), dm = dpdm::dm_pins(l->dm);
    CHECK_EQ(dp.a && dp.b && dm.a && !dm.b, true);
}

static void test_handshake_window() {
    CHECK_EQ(qc::handshake_initial_ok(349), false);
    CHECK_EQ(qc::handshake_initial_ok(350), true);
    CHECK_EQ(qc::handshake_initial_ok(600), true);
    CHECK_EQ(qc::handshake_initial_ok(1150), true);
    CHECK_EQ(qc::handshake_initial_ok(1151), false);
    CHECK_EQ(qc::handshake_dropped(600, 251), false);   // fell only 349 mV
    CHECK_EQ(qc::handshake_dropped(600, 250), true);    // fell 350 mV
}

static void test_qc3_up_pulse_is_dplus_burst_to_3v3() {
    fc::StepList<8> sl;
    CHECK_EQ(qc::pulse_train(sl, qc::Mode::Qc3, qc::Dir::Up, 1), 1);
    // D+ 3.3 V for 2250 us with D- parked at 3.3 V, then back to the 0.6 V / 3.3 V baseline.
    CHECK_EQ(sl.n, 2);
    CHECK_EQ(sl.steps[0].dp == Drive::V3p3 && sl.steps[0].dm == Drive::V3p3 && sl.steps[0].us == 2250, true);
    CHECK_EQ(sl.steps[1].dp == Drive::V0p6 && sl.steps[1].dm == Drive::V3p3, true);
}

static void test_qc3_down_pulse_is_dminus_dip_to_0v6() {
    fc::StepList<8> sl;
    qc::pulse_train(sl, qc::Mode::Qc3, qc::Dir::Down, 1);
    CHECK_EQ(sl.steps[0].dp == Drive::V0p6 && sl.steps[0].dm == Drive::V0p6 && sl.steps[0].us == 2250, true);
    CHECK_EQ(sl.steps[1].dp == Drive::V0p6 && sl.steps[1].dm == Drive::V3p3, true);
}

static void test_qc3plus_pulses_are_short() {
    fc::StepList<8> sl;
    qc::pulse_train(sl, qc::Mode::Qc3Plus, qc::Dir::Up, 1);
    CHECK_EQ(sl.steps[0].us, 40);
    CHECK_EQ(sl.steps[1].us, 50);
}

// Measured on a power bank (Xiaomi 20K, the lab sweep of 2026-11-07): QC3+ down pulses of 40/100/150 us moved VBUS
// not at all, 175 us and longer stepped it down; up pulses move it from 40 us. The down pulse needs a wider hold.
static void test_qc3plus_down_pulse_is_wider_than_up() {
    fc::StepList<8> up, down;
    qc::pulse_train(up, qc::Mode::Qc3Plus, qc::Dir::Up, 1);
    qc::pulse_train(down, qc::Mode::Qc3Plus, qc::Dir::Down, 1);
    CHECK_EQ(up.steps[0].us, qc::kQc3PlusPulseUs);
    CHECK_EQ(down.steps[0].us, qc::kQc3PlusDownPulseUs);
    CHECK_EQ(down.steps[0].dp == Drive::V0p6 && down.steps[0].dm == Drive::V0p6, true);
    CHECK_EQ(down.steps[1].us, qc::kQc3PlusGapUs);
    CHECK_EQ(qc::kQc3PlusDownPulseUs >= 175, true);
    // A full 50-pulse batch is one blocking list: it stays under the 20 ms the main loop tolerates.
    CHECK_EQ(50 * (qc::kQc3PlusDownPulseUs + qc::kQc3PlusGapUs) < 20000, true);
}

static void test_pulse_train_on_fake_clock() {
    fc::StepList<16> sl;
    CHECK_EQ(qc::pulse_train(sl, qc::Mode::Qc3, qc::Dir::Up, 3), 3);
    FakeIo io;
    fc::replay(sl.span(), io);
    // Three pulses: each a rise at t, back to baseline at t+2250, next rise at t+2250+gap.
    CHECK_EQ(io.ev.size(), 6);
    CHECK_EQ(io.ev[0].t, 0);
    CHECK_EQ(io.ev[1].t, 2250);
    CHECK_EQ(io.ev[2].t, 2250 + qc::kQc3GapUs);
    CHECK_EQ(io.ev[4].t, 2 * (2250 + qc::kQc3GapUs));
    CHECK_EQ(io.now, 3 * (2250 + qc::kQc3GapUs));
    CHECK_EQ(io.ev[4].dp == Drive::V3p3, true);
}

static void test_pulse_train_is_capped_by_capacity() {
    fc::StepList<4> sl;                       // room for 2 pulses
    CHECK_EQ(qc::pulse_train(sl, qc::Mode::Qc3, qc::Dir::Up, 10), 2);
    CHECK_EQ(sl.n, 4);
}

static void test_pulses_for_error() {
    // Closed loop on measured VBUS: one pulse per 200 mV (QC3.0) / 20 mV (QC3+); inside the dead band none.
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3, 4000), 20);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3, -1000), -5);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3, 149), 0);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3, -149), 0);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3, 150), 1);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3Plus, 100), 5);
    CHECK_EQ(qc::pulses_for(qc::Mode::Qc3Plus, 29), 0);
}

static void test_target_clamp_and_band() {
    CHECK_EQ(qc::clamp_qc3_mv(2000, 36000), 3600);
    CHECK_EQ(qc::clamp_qc3_mv(20000, 36000), 20000);
    CHECK_EQ(qc::clamp_qc3_mv(24000, 36000), 20000);
    CHECK_EQ(qc::clamp_qc3_mv(9000, 8000), 8000);      // chip ceiling wins
    CHECK_EQ(qc::clamp_qc3_mv(9000, 0), 0);            // no power monitor: nothing allowed
    CHECK_EQ(fc::in_band(9000, 8800), true);
    CHECK_EQ(fc::in_band(9000, 7000), false);          // still at the 5 V level
    CHECK_EQ(fc::in_band(9000, 9901), false);          // over target + 10 %
    CHECK_EQ(fc::over_guard(9000, 9900), false);
    CHECK_EQ(fc::over_guard(9000, 9901), true);
}

static void test_settle_tracker_needs_three_spaced_samples() {
    qc::Settle s;
    CHECK_EQ(s.feed(0, 5000), false);
    CHECK_EQ(s.feed(10, 5000), false);       // closer than the sample spacing: not a new sample
    CHECK_EQ(s.feed(30, 5000), false);
    CHECK_EQ(s.feed(40, 5000), false);       // still only two distinct samples
    CHECK_EQ(s.feed(60, 5000), true);        // third spaced sample inside 50 mV
    CHECK_EQ(s.feed(90, 5060), false);       // 60 mV spread: still moving
    CHECK_EQ(s.feed(120, 5060), false);
    CHECK_EQ(s.feed(150, 5060), true);
    s.reset();
    CHECK_EQ(s.feed(180, 5060), false);      // a reset forgets the history
}

static void test_settle_tracker_rejects_a_slow_slew() {
    // 1 V/s slew: 30 mV per sample, 60 mV across three samples.
    qc::Settle s;
    bool stable = false;
    for (int i = 0; i < 12; i++) stable = stable || s.feed(std::uint32_t(i) * 30, 5000 + i * 30);
    CHECK_EQ(stable, false);
}

static void test_pass_band_is_asymmetric() {
    CHECK_EQ(qc::in_pass_band(8700, 9000), true);
    CHECK_EQ(qc::in_pass_band(8699, 9000), false);
    CHECK_EQ(qc::in_pass_band(9150, 9000), true);
    CHECK_EQ(qc::in_pass_band(9151, 9000), false);
}

static void test_plan_stops_below_target() {
    // From 5 V to 9 V in 200 mV steps: stop at the first level at or under target - 100 mV (tie goes to the lower one).
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 5000, 9000, 200), 19);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 8800, 9000, 200), 0);     // inside the band: hold
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 8700, 9000, 200), 0);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 9100, 9000, 200), 0);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 8600, 9000, 200), 1);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 9200, 9000, 200), -1);    // above +150: one pulse back down
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 9000, 7000, 200), -10);
}

static void test_plan_with_coarse_steps() {
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 5000, 9000, 450), 9);     // 9050 is the closest in band
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 8500, 9000, 700), 1);     // nothing lands in the band: the closer level
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3, 9200, 9000, 700), 0);     // going back would be worse: hold
}

static void test_plan_qc3plus_keeps_the_fine_loop() {
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 5000, 9000, 20), 195);   // to 8900: target - 100 mV
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 8900, 9000, 20), 0);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 8880, 9000, 20), 0);     // inside the 30 mV dead band of the aim
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 8860, 9000, 20), 2);
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 9100, 9000, 20), 0);     // above the aim but inside the band: hold
    CHECK_EQ(qc::plan_pulses(qc::Mode::Qc3Plus, 9200, 9000, 20), -15);   // over +150: back to 8900
}

static void test_batches_leave_single_pulses_for_the_end() {
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 19, 4), 4);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 6, 4), 4);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 5, 4), 3);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 4, 4), 2);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 3, 4), 1);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 2, 4), 1);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3, 1, 4), 1);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, 200, 50), 50);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, 60, 50), 30);       // smaller batches near the target
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, 24, 50), 12);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, 12, 50), 10);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, 10, 50), 10);
    CHECK_EQ(qc::batch_pulses(qc::Mode::Qc3Plus, -3, 50), 3);
}

static void test_open_loop_limit() {
    CHECK_EQ(qc::open_loop_limit_mv(5000, 9000, 200), 9200);   // 20 pulses expected, one step of slack
    CHECK_EQ(qc::open_loop_limit_mv(5000, 9000, 700), 9900);   // 6 pulses of 700
    CHECK_EQ(qc::open_loop_limit_mv(9000, 7000, 200), 9200);   // going down: never above the start + a step
    CHECK_EQ(qc::open_loop_limit_mv(5000, 5000, 200), 5200);
    CHECK_EQ(qc::open_loop_limit_mv(5000, 9000, 20), 9100);    // QC3+: 200 pulses of 20 mV, slack = the 100 mV noise floor
}

int main() {
    test_settle_tracker_needs_three_spaced_samples();
    test_settle_tracker_rejects_a_slow_slew();
    test_pass_band_is_asymmetric();
    test_plan_stops_below_target();
    test_plan_with_coarse_steps();
    test_plan_qc3plus_keeps_the_fine_loop();
    test_batches_leave_single_pulses_for_the_end();
    test_open_loop_limit();
    test_qc2_levels_match_spec_table();
    test_qc2_pin_levels_match_spec_gpio_column();
    test_handshake_window();
    test_qc3_up_pulse_is_dplus_burst_to_3v3();
    test_qc3_down_pulse_is_dminus_dip_to_0v6();
    test_qc3plus_pulses_are_short();
    test_qc3plus_down_pulse_is_wider_than_up();
    test_pulse_train_on_fake_clock();
    test_pulse_train_is_capped_by_capacity();
    test_pulses_for_error();
    test_target_clamp_and_band();
    return test_report("test_qc");
}
