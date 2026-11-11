// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The D+/D- switch network driver's passive idle and the safety rules every idle and probe state must keep:
// never D+ in the HVDCP entry window, never a QC2 voltage code.
#include "k2/drivers/dpdm.hpp"
#include "test_util.h"

using namespace k2;

// "[FC] idle ... mismatch=" carries one bit per line that is not at the passive idle.
static void test_mismatch_reports_each_line() {
    using dpdm::Drive; using dpdm::Pair;
    CHECK_EQ(dpdm::mismatch(dpdm::kIdle), 0u);
    CHECK_EQ(dpdm::mismatch(Pair{Drive::HiZ, Drive::HiZ}), 0u);
    CHECK_EQ(dpdm::mismatch(Pair{Drive::V0p6, Drive::HiZ}), 1u);
    CHECK_EQ(dpdm::mismatch(Pair{Drive::HiZ, Drive::V0}), 2u);
    CHECK_EQ(dpdm::mismatch(Pair{Drive::V0p6, Drive::V0p6}), 3u);   // the pre-2026-11-11 idle
    CHECK_EQ(dpdm::mismatch(Pair{Drive::Sniff, Drive::Sniff}), 3u);  // dpsniff owns the lines
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

// The QC2 voltage codes and the HVDCP entry window, from docs/agents/QC_RESEARCH.md section 1-2.
static void test_qc2_codes_and_entry_window() {
    using dpdm::Drive; using dpdm::Pair;
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V0p6, Drive::V0}), true);      // 5 V
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V3p3, Drive::V0p6}), true);    // 9 V
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V0p6, Drive::V0p6}), true);    // 12 V
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V3p3, Drive::V3p3}), true);    // 20 V
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::HiZ, Drive::HiZ}), false);
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V0, Drive::V0p6}), false);     // D+ low: the adapter's 5 V fallback
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::HiZ, Drive::V0p6}), false);
    CHECK_EQ(dpdm::qc2_code(Pair{Drive::V0, Drive::HiZ}), false);
    CHECK_EQ(dpdm::hvdcp_entry_level(Drive::V0p6), true);
    CHECK_EQ(dpdm::hvdcp_entry_level(Drive::V2p0), true);              // the window's top edge counts
    CHECK_EQ(dpdm::hvdcp_entry_level(Drive::V0), false);
    CHECK_EQ(dpdm::hvdcp_entry_level(Drive::V3p3), false);
    for (const Drive d : {Drive::HiZ, Drive::Listen, Drive::Sniff}) CHECK_EQ(dpdm::hvdcp_entry_level(d), false);
}

// Hardware 2026-11-11 (CUKTECH 10 USB-A): an idle that holds D+ at 0.6 V for ~1.25 s is the HVDCP entry handshake,
// and 0.6/0.6 is the QC2 12 V code, so a D- dip and return raised VBUS to 11.75 V with nothing requested. The
// passive idle drives neither line: both pin pairs are analog inputs.
static void test_idle_never_enters_hvdcp_or_forms_a_qc2_code() {
    CHECK_EQ(dpdm::hvdcp_entry_level(dpdm::kIdle.dp), false);
    CHECK_EQ(dpdm::qc2_code(dpdm::kIdle), false);
    CHECK_EQ(dpdm::dp_pins(dpdm::kIdle.dp).kind == dpdm::Kind::Analog, true);
    CHECK_EQ(dpdm::dm_pins(dpdm::kIdle.dm).kind == dpdm::Kind::Analog, true);
}

// The active DCP check's steps never drive D+ into the entry window and never form a QC2 code; the first step
// pulls D+ low, the HVDCP exit level, so a charger left in HVDCP by anything earlier falls back to 5 V.
static void test_dcp_probe_steps_are_safe() {
    for (const auto& p : dpdm::kDcpProbe) {
        CHECK_EQ(dpdm::hvdcp_entry_level(p.dp), false);
        CHECK_EQ(dpdm::qc2_code(p), false);
    }
    CHECK_EQ(dpdm::kDcpProbe[0].dp == dpdm::Drive::V0, true);
}

int main() {
    test_qc2_codes_and_entry_window();
    test_idle_never_enters_hvdcp_or_forms_a_qc2_code();
    test_dcp_probe_steps_are_safe();
    test_dp_drive_truth_table();
    test_dm_drive_truth_table();
    test_mismatch_reports_each_line();
    return test_report("test_dpdm");
}
