// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::emk_attached: how the e-marker scan is laid out when a source is attached (Rd kept up, one orientation) and when
// the cable is alone (both orientations, no Rd), and the record of the last read.
#include "k2/pd/emk_attached.hpp"
#include "k2/pd/pd_phy.hpp"
#include "test_util.h"

using namespace k2;
namespace ea = k2::emk_attached;
namespace phy = k2::pd::phy;

static ea::Facts facts(std::uint8_t active, std::uint8_t sw0, std::int32_t vbus_mv, bool caps, bool sink_busy) {
    ea::Facts f;
    f.active_cc = active;
    f.sw0 = sw0;
    f.vbus_mv = vbus_mv;
    f.caps_valid = caps;
    f.sink_busy = sink_busy;
    return f;
}

static void test_the_hardware_state_selects_attached() {
    // 2026-11-11, NOVOO behind the Baseus 100 W cable after `pd attach`: SW0 0x05 (PDWN1 + MEAS_CC1), the power monitor
    // 5023 mV, caps valid, the sink Ready. (STATUS0 VBUSOK read 0 and the passive contract flag was 0: neither is an input.)
    CHECK_EQ(ea::attached(facts(1, 0x05, 5023, true, true)), true);
}

static void test_attached_needs_rd_on_the_live_line_and_any_one_sign_of_a_source() {
    const std::uint8_t rd1 = phy::kPdwn1 | phy::kMeasCc1, rd2 = phy::kPdwn2 | phy::kMeasCc2;
    // Each sign alone is enough.
    CHECK_EQ(ea::attached(facts(1, rd1, 5023, false, false)), true);    // the power monitor at vSafe5V
    CHECK_EQ(ea::attached(facts(2, rd2, -1, true, false)), true);       // valid source caps of this attach
    CHECK_EQ(ea::attached(facts(1, rd1, 0, false, true)), true);        // the sink engine is not idle
    // The vSafe5V minimum is 4.75 V.
    CHECK_EQ(ea::attached(facts(1, rd1, 4750, false, false)), true);
    CHECK_EQ(ea::attached(facts(1, rd1, 4749, false, false)), false);
    // No active line: a cable alone.
    CHECK_EQ(ea::attached(facts(0, rd1, 5023, true, true)), false);
    // Rd missing on the active line: nothing to keep.
    CHECK_EQ(ea::attached(facts(1, phy::kPdwn2, 5023, true, true)), false);
    CHECK_EQ(ea::attached(facts(2, 0, 5023, true, true)), false);
    // Rd up but no sign of a source: a charger that waits is not feeding anything yet (power monitor unknown or dark).
    CHECK_EQ(ea::attached(facts(1, rd1, -1, false, false)), false);
    CHECK_EQ(ea::attached(facts(1, rd1, 0, false, false)), false);
}

static void test_alone_plan_is_the_two_orientations_as_before() {
    const ea::Plan p = ea::plan(false, 0);
    CHECK_EQ(p.attached, false);
    CHECK_EQ(unsigned(p.count), 2u);
    CHECK_EQ(unsigned(p.steps[0].orientation), 1u);
    CHECK_EQ(unsigned(p.steps[0].sw0), 0x24u);   // VCONN_CC2 + MEAS_CC1
    CHECK_EQ(unsigned(p.steps[0].sw1), 0x25u);   // BMC on CC1, Auto-CRC
    CHECK_EQ(unsigned(p.steps[1].orientation), 2u);
    CHECK_EQ(unsigned(p.steps[1].sw0), 0x18u);   // VCONN_CC1 + MEAS_CC2
    CHECK_EQ(unsigned(p.steps[1].sw1), 0x26u);
    CHECK_EQ(p.pc13, true);
}

static void test_attached_plan_is_one_orientation_with_rd_on_the_live_line() {
    const ea::Plan a = ea::plan(true, 1);
    CHECK_EQ(a.attached, true);
    CHECK_EQ(unsigned(a.count), 1u);
    CHECK_EQ(unsigned(a.steps[0].orientation), 1u);   // A: BMC on CC1, VCONN on the idle CC2
    CHECK_EQ(unsigned(a.steps[0].sw0), 0x25u);        // PDWN1 + MEAS_CC1 + VCONN_CC2
    CHECK_EQ(unsigned(a.steps[0].sw1), 0x25u);
    const ea::Plan b = ea::plan(true, 2);
    CHECK_EQ(unsigned(b.count), 1u);
    CHECK_EQ(unsigned(b.steps[0].orientation), 2u);   // B: BMC on CC2, VCONN on the idle CC1
    CHECK_EQ(unsigned(b.steps[0].sw0), 0x1Au);        // PDWN2 + MEAS_CC2 + VCONN_CC1
    CHECK_EQ(unsigned(b.steps[0].sw1), 0x26u);
}

static void test_attached_plan_never_puts_vconn_on_the_live_line() {
    for (std::uint8_t active = 1; active <= 2; active++) {
        const ea::Plan p = ea::plan(true, active);
        const std::uint8_t live_vconn = active == 1 ? phy::kVconnCc1 : phy::kVconnCc2;
        const std::uint8_t live_pdwn = active == 1 ? phy::kPdwn1 : phy::kPdwn2;
        for (unsigned i = 0; i < p.count; i++) {
            CHECK_EQ(unsigned(p.steps[i].sw0 & live_vconn), 0u);
            CHECK_EQ(unsigned(p.steps[i].sw0 & live_pdwn), unsigned(live_pdwn));
        }
    }
}

static void test_pc13_stays_off_the_live_cc1_line() {
    // PC13 high is an Ra on the CC1 net: on a live CC1 it would make the source see Ra beside Rd.
    CHECK_EQ(ea::plan(true, 1).pc13, false);
    CHECK_EQ(ea::plan(true, 2).pc13, true);
}

static void test_read_record() {
    ea::ReadInfo r;
    CHECK_EQ(unsigned(r.mode), unsigned(ea::Mode::None));
    CHECK_EQ(r.acked_no_answer(false), false);
    r.mode = ea::Mode::Attached;
    r.acks = 1;
    CHECK_EQ(r.acked_no_answer(false), true);    // acknowledged, never answered
    CHECK_EQ(r.acked_no_answer(true), false);    // answered: not a silent cable
    r.acks = 0;
    CHECK_EQ(r.acked_no_answer(false), false);
    CHECK_EQ(ea::mode_text(ea::Mode::Alone) == std::string_view("alone"), true);
    CHECK_EQ(ea::mode_text(ea::Mode::Attached) == std::string_view("attached"), true);
    CHECK_EQ(ea::mode_text(ea::Mode::None) == std::string_view("none"), true);
}

int main() {
    test_the_hardware_state_selects_attached();
    test_attached_needs_rd_on_the_live_line_and_any_one_sign_of_a_source();
    test_alone_plan_is_the_two_orientations_as_before();
    test_attached_plan_is_one_orientation_with_rd_on_the_live_line();
    test_attached_plan_never_puts_vconn_on_the_live_line();
    test_pc13_stays_off_the_live_cc1_line();
    test_read_record();
    return test_report("test_emk_attached");
}
