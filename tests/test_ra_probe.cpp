// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::raprobe: the silent-cable-chip probe's pure rules: the refusal order, the argument check, the fixed sample plan,
// the Rp-level guard that keeps VCONN off a line a source is on, the abort rule and the SWITCHES0 of each phase.
// Pure logic, no hardware (the wire sequence is tested in tests/test_fusb302_scan.cpp, the verdict in tests/test_ra_class.cpp).
#include "k2/pd/ra_probe.hpp"
#include "test_util.h"

using namespace k2::raprobe;

static Ctx passive() {
    Ctx c;
    c.vbus_mv = 0;
    return c;
}

static void test_refusal_passive_is_allowed() {
    CHECK_EQ(int(refusal(passive())), int(Refusal::None));
    Ctx c = passive();
    c.vbus_mv = 999;
    CHECK_EQ(int(refusal(c)), int(Refusal::None));   // 1 V and above refuses
}

static void test_refusal_each_condition_and_order() {
    Ctx c = passive(); c.inline_sink = true;      CHECK_EQ(int(refusal(c)), int(Refusal::Inline));
    c = passive(); c.contract = true;             CHECK_EQ(int(refusal(c)), int(Refusal::Contract));
    c = passive(); c.rd = true;                   CHECK_EQ(int(refusal(c)), int(Refusal::Rd));
    c = passive(); c.ra_mode = true;              CHECK_EQ(int(refusal(c)), int(Refusal::RaMode));
    c = passive(); c.engine_running = true;       CHECK_EQ(int(refusal(c)), int(Refusal::Running));
    c = passive(); c.vconn_on = true;             CHECK_EQ(int(refusal(c)), int(Refusal::Running));
    c = passive(); c.vbus_mv = -1;                CHECK_EQ(int(refusal(c)), int(Refusal::VbusUnknown));
    c = passive(); c.vbus_mv = 1000;              CHECK_EQ(int(refusal(c)), int(Refusal::VbusHigh));
    c = passive(); c.vbus_mv = 5000;              CHECK_EQ(int(refusal(c)), int(Refusal::VbusHigh));
    c = passive(); c.vbus_flag = true;            CHECK_EQ(int(refusal(c)), int(Refusal::VbusHigh));   // the chip sees VBUS, the monitor does not
    // The first failing condition names the reason: inline before contract before Rd, and so on.
    c = passive(); c.inline_sink = true; c.contract = true; c.rd = true; c.vbus_mv = 5000;
    CHECK_EQ(int(refusal(c)), int(Refusal::Inline));
    c = passive(); c.contract = true; c.rd = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Contract));
    c = passive(); c.rd = true; c.vbus_mv = 5000;
    CHECK_EQ(int(refusal(c)), int(Refusal::Rd));
    c = passive(); c.engine_running = true; c.vbus_mv = -1;
    CHECK_EQ(int(refusal(c)), int(Refusal::Running));
}

static void test_refusal_ignores_active_cc_latch() {
    // The empty port's "dark attach" latches active_cc = 2 with nothing connected: the probe must not read that latch.
    Ctx c = passive();
    CHECK_EQ(int(refusal(c)), int(Refusal::None));
}

static void test_args() {
    CHECK_EQ(int(check_args(Args{0})), int(ArgError::None));
    CHECK_EQ(int(check_args(Args{6})), int(ArgError::None));   // PC14 and PC15 closed during the VCONN phases
    CHECK_EQ(int(check_args(Args{2})), int(ArgError::None));
    CHECK_EQ(int(check_args(Args{1})), int(ArgError::VconnRail));   // PC13 high is an Ra on CC1: never with VCONN
    CHECK_EQ(int(check_args(Args{7})), int(ArgError::VconnRail));
    CHECK_EQ(int(check_args(Args{8})), int(ArgError::Pc));
    CHECK_EQ(int(check_args(Args{0xFF})), int(ArgError::Pc));
    CHECK_EQ(int(Args{}.vconn_pc), int(kVconnPcDefault));
    CHECK_EQ(int(kVconnPcDefault), 0);                           // the condition of every measured number
}

static void test_plan_is_fixed() {
    CHECK_EQ(int(kPhases), 3);
    CHECK_EQ(int(kSamplesPerPhase), 3);
    CHECK_EQ(int(kSampleMs[0]), 2);
    CHECK_EQ(int(kSampleMs[1]), 10);
    CHECK_EQ(int(kSampleMs[2]), 100);
    CHECK_EQ(int(kMaxSamples), 9);
    // 3 phases x 100 ms plus one settle gap (between the two VCONN phases).
    CHECK_EQ(int(total_ms()), 3 * 100 + int(kSettleMs));
    CHECK_EQ(int(sample_index(kPhaseIdle, 0)), 0);
    CHECK_EQ(int(sample_index(kPhaseVconn1, 0)), 3);
    CHECK_EQ(int(sample_index(kPhaseVconn2, 2)), 8);
}

static void test_phase_sw0_and_pins() {
    namespace phy = k2::pd::phy;
    CHECK_EQ(int(phase_sw0(kPhaseIdle, 0x0C)), 0x0C);               // idle: whatever the chip had (no Rd: refused earlier)
    CHECK_EQ(int(phase_sw0(kPhaseVconn1, 0x0C)), int(phy::kVconnCc1));   // 0x10, no measure bit, as measured
    CHECK_EQ(int(phase_sw0(kPhaseVconn2, 0x0C)), int(phy::kVconnCc2));   // 0x20
    CHECK_EQ(int(phase_pc(kPhaseIdle, 6, 0)), 6);                   // idle phase: the snapshot levels
    CHECK_EQ(int(phase_pc(kPhaseVconn1, 6, 0)), 0);                 // VCONN phases: the requested levels
    CHECK_EQ(int(phase_pc(kPhaseVconn2, 6, 6)), 6);
}

static void test_source_level_guard() {
    // Measured idle levels (2026-11-11): the empty port 6 / 1692 mV, a chipless cable 6 / 1672, the Baseus cable's Ra on
    // CC1 0 / 1662 and on CC2 6 / 0. None is a source.
    CHECK_EQ(source_level(6, 1692), false);
    CHECK_EQ(source_level(6, 1672), false);
    CHECK_EQ(source_level(0, 1662), false);
    CHECK_EQ(source_level(6, 0), false);
    CHECK_EQ(source_level(0, 2), false);          // PLUS held: CC2 clamped, no source
    // A waiting charger: an open Rp reads the ADC ceiling on CC1, a 1.5 A / 3 A Rp over a cable Ra lands in 0.4-1.4 V.
    CHECK_EQ(source_level(3294, 3294), true);
    CHECK_EQ(source_level(3294, 1692), true);
    CHECK_EQ(source_level(6, 3294), true);
    CHECK_EQ(source_level(900, 1692), true);
    CHECK_EQ(source_level(6, 900), true);
    CHECK_EQ(source_level(322, 1692), false);     // just under the CC1 limit: a cable's own Ra with a charger's bias off
    CHECK_EQ(source_level(kSourceLowMaxMv, 1692), true);   // the limit itself counts as a source
    CHECK_EQ(source_level(6, kBiasMinMv - 1), true);
    CHECK_EQ(source_level(6, kBiasMaxMv + 1), true);
    CHECK_EQ(source_level(6, kBiasMinMv), false);
    CHECK_EQ(source_level(6, kBiasMaxMv), false);
}

static void test_abort_rule() {
    CHECK_EQ(vbus_abort(0, 0), false);
    CHECK_EQ(vbus_abort(1000, 0), false);        // above, not at, the limit
    CHECK_EQ(vbus_abort(1001, 0), true);
    CHECK_EQ(vbus_abort(-1, 0x80), true);        // the chip's VBUS comparator
    CHECK_EQ(vbus_abort(-1, 0x7F), false);
}

static void test_raw_to_mv() {
    CHECK_EQ(int(raw_to_mv(0)), 0);
    CHECK_EQ(int(raw_to_mv(2114)), 1703);        // the CC2 idle bias, raw ch5
    CHECK_EQ(int(raw_to_mv(4095)), 3299);
    CHECK_EQ(int(raw_to_mv(kNoRaw)), -1);
}

int main() {
    test_refusal_passive_is_allowed();
    test_refusal_each_condition_and_order();
    test_refusal_ignores_active_cc_latch();
    test_args();
    test_plan_is_fixed();
    test_phase_sw0_and_pins();
    test_source_level_guard();
    test_abort_rule();
    test_raw_to_mv();
    return test_report("test_ra_probe");
}
