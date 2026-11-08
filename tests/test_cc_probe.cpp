// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::ccprobe: argument validation, the passive-only refusal rules, the sample plan and the abort rule of
// `debug cc-probe` (pure logic, no hardware). Expected values are literals from the FUSB302 SWITCHES0 layout:
// bit0 PDWN1, bit1 PDWN2, bit2 MEAS_CC1, bit3 MEAS_CC2, bit4 VCONN_CC1, bit5 VCONN_CC2, bit6/7 pull-up enables.
#include "k2/pd/cc_probe.hpp"
#include "test_util.h"

using namespace k2::ccprobe;

static Args args(std::uint8_t sw0, std::uint8_t pc, std::uint16_t ms = 500, std::uint16_t modes = kModesKeep) {
    Args a;
    a.sw0 = sw0; a.pc = pc; a.ms = ms; a.modes = modes;
    return a;
}

static void test_args_the_experiment() {
    // VCONN_CC2 closed (0x20) with the CC2 measure switch (0x08), PC13 low, 500 ms.
    CHECK_EQ(int(check_args(args(0x28, 0))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0x18, 0))), int(ArgError::None));   // VCONN_CC1 + MEAS_CC2
    CHECK_EQ(int(check_args(args(0x00, 7))), int(ArgError::None));   // nothing closed: any PC levels
    CHECK_EQ(int(check_args(args(0x0C, 5))), int(ArgError::None));   // both measure switches, no VCONN
}

static void test_args_vconn_needs_pc13_low() {
    CHECK_EQ(int(check_args(args(0x20, 1))), int(ArgError::VconnRail));   // PC13 high powers the rail
    CHECK_EQ(int(check_args(args(0x10, 7))), int(ArgError::VconnRail));
    CHECK_EQ(int(check_args(args(0x30, 3))), int(ArgError::VconnRail));
    CHECK_EQ(int(check_args(args(0x20, 6))), int(ArgError::None));        // bit0 clear: PC13 low
    // PC13 must stay a driven output while a VCONN switch is closed: a floating or pulled pin is not "low".
    CHECK_EQ(int(check_args(args(0x20, 0, 500, 0x334))), int(ArgError::VconnRail));   // PC13 floating input
    CHECK_EQ(int(check_args(args(0x20, 0, 500, 0x338))), int(ArgError::VconnRail));   // PC13 input with pull
    CHECK_EQ(int(check_args(args(0x20, 0, 500, 0x330))), int(ArgError::VconnRail));   // PC13 analog
    CHECK_EQ(int(check_args(args(0x20, 0, 500, 0x333))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0x20, 0, 500, 0x337))), int(ArgError::None));        // open drain, still driven low
    // The mode of PC13 does not matter when no VCONN switch is closed.
    CHECK_EQ(int(check_args(args(0x08, 0, 500, 0x334))), int(ArgError::None));
}

static void test_args_pdwn_with_vconn_same_line() {
    CHECK_EQ(int(check_args(args(0x20 | 0x02, 0))), int(ArgError::VconnPdwn));   // VCONN_CC2 + PDWN2
    CHECK_EQ(int(check_args(args(0x10 | 0x01, 0))), int(ArgError::VconnPdwn));   // VCONN_CC1 + PDWN1
    CHECK_EQ(int(check_args(args(0x20 | 0x01, 0))), int(ArgError::None));        // Rd on the other line is fine
    CHECK_EQ(int(check_args(args(0x10 | 0x02, 0))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0x30 | 0x02, 0))), int(ArgError::VconnPdwn));
}

static void test_args_pullups_refused() {
    CHECK_EQ(int(check_args(args(0x40, 0))), int(ArgError::PullUp));   // PU_EN1: the K2 never drives an Rp
    CHECK_EQ(int(check_args(args(0x80, 0))), int(ArgError::PullUp));
    CHECK_EQ(int(check_args(args(0x08 | 0x80, 0))), int(ArgError::PullUp));
}

static void test_args_ranges() {
    CHECK_EQ(int(check_args(args(0, 8))), int(ArgError::Pc));
    CHECK_EQ(int(check_args(args(0, 0, 9))), int(ArgError::Ms));
    CHECK_EQ(int(check_args(args(0, 0, 10))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0, 0, 5000))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0, 0, 5001))), int(ArgError::Ms));
    CHECK_EQ(int(check_args(args(0, 0, 0))), int(ArgError::Ms));
    CHECK_EQ(int(check_args(args(0, 0, 500, 0x888))), int(ArgError::None));
    CHECK_EQ(int(check_args(args(0, 0, 500, 0x5333))), int(ArgError::Modes));   // above 12 bits, and not "keep"
    CHECK_EQ(int(check_args(args(0, 0, 500, 0x3F3))), int(ArgError::Modes));    // nibble F is no pin mode
    CHECK_EQ(int(check_args(args(0, 0, 500, 0x321))), int(ArgError::Modes));    // 1 and 2 are input/output with 10 MHz slew: not used here
}

static Ctx passive() {
    Ctx c;
    c.vbus_mv = 0;
    return c;
}

static void test_refusal_passive_is_allowed() {
    CHECK_EQ(int(refusal(passive())), int(Refusal::None));
    Ctx c = passive();
    c.vbus_mv = 999;
    CHECK_EQ(int(refusal(c)), int(Refusal::None));
}

static void test_refusal_each_condition() {
    Ctx c = passive(); c.attached = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Attached));
    c = passive(); c.contract = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Contract));
    c = passive(); c.rd = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Rd));
    c = passive(); c.engine_running = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Running));
    c = passive(); c.vconn_on = true;
    CHECK_EQ(int(refusal(c)), int(Refusal::Running));
    c = passive(); c.vbus_mv = 1000;
    CHECK_EQ(int(refusal(c)), int(Refusal::VbusHigh));      // below 1 V only: 1000 mV is not below
    c = passive(); c.vbus_mv = 5000;
    CHECK_EQ(int(refusal(c)), int(Refusal::VbusHigh));
    c = passive(); c.vbus_mv = -1;
    CHECK_EQ(int(refusal(c)), int(Refusal::VbusUnknown));   // a monitor that is offline cannot prove 0 V
}

static void test_abort_rule() {
    CHECK_EQ(vbus_abort(0, 0x00), false);
    CHECK_EQ(vbus_abort(1000, 0x00), false);                // above 1 V, strictly
    CHECK_EQ(vbus_abort(1001, 0x00), true);
    CHECK_EQ(vbus_abort(-1, 0x00), false);                  // no reading: the chip's own VBUS flag decides
    CHECK_EQ(vbus_abort(0, 0x80), true);                    // STATUS0 bit7 VBUSOK (above ~4 V)
    CHECK_EQ(vbus_abort(0, 0x7F), false);                   // every other STATUS0 bit is not VBUS
}

static void test_plan() {
    Plan p = plan(500);
    CHECK_EQ(p.n, 4);
    CHECK_EQ(p.at_ms[0], 1); CHECK_EQ(p.at_ms[1], 10); CHECK_EQ(p.at_ms[2], 100); CHECK_EQ(p.at_ms[3], 500);
    p = plan(5000);
    CHECK_EQ(p.n, 4);
    CHECK_EQ(p.at_ms[3], 5000);
    p = plan(101);
    CHECK_EQ(p.n, 4);
    CHECK_EQ(p.at_ms[2], 100); CHECK_EQ(p.at_ms[3], 101);
    p = plan(100);                                           // the end sample is the +100 ms sample
    CHECK_EQ(p.n, 3);
    CHECK_EQ(p.at_ms[0], 1); CHECK_EQ(p.at_ms[1], 10); CHECK_EQ(p.at_ms[2], 100);
    p = plan(50);
    CHECK_EQ(p.n, 3);
    CHECK_EQ(p.at_ms[0], 1); CHECK_EQ(p.at_ms[1], 10); CHECK_EQ(p.at_ms[2], 50);
    p = plan(10);
    CHECK_EQ(p.n, 2);
    CHECK_EQ(p.at_ms[0], 1); CHECK_EQ(p.at_ms[1], 10);
}

static void test_measure_sw0() {
    // The sample cycles the measure switch through both lines; everything else in the requested value stays.
    CHECK_EQ(measure_sw0(0x28, 1), 0x24);    // VCONN_CC2 kept, MEAS_CC2 -> MEAS_CC1
    CHECK_EQ(measure_sw0(0x28, 2), 0x28);
    CHECK_EQ(measure_sw0(0x20, 1), 0x24);    // no measure switch requested
    CHECK_EQ(measure_sw0(0x0C, 2), 0x08);    // both requested: exactly the one asked
    CHECK_EQ(measure_sw0(0x31, 2), 0x39);    // PDWN1 and both VCONN bits survive
}

int main() {
    test_args_the_experiment();
    test_args_vconn_needs_pc13_low();
    test_args_pdwn_with_vconn_same_line();
    test_args_pullups_refused();
    test_args_ranges();
    test_refusal_passive_is_allowed();
    test_refusal_each_condition();
    test_abort_rule();
    test_plan();
    test_measure_sw0();
    return test_report("test_cc_probe");
}
