// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The "Shell access" band: which remote requests drive the K2 (remote::shell_control, one table next to the dispatcher)
// and when the band shows, holds and clears (k2/core/shell_toast.hpp). Pure logic; the paint is in test_ui_capture.
#include "test_util.h"
#include "k2/core/remote_proto.hpp"
#include "k2/core/shell_toast.hpp"

using namespace k2;
using remote::Cmd;

static bool ctl(Cmd c, std::initializer_list<uint8_t> payload = {}) {
    return remote::shell_control(c, std::span<const uint8_t>(payload.begin(), payload.size()));
}

// A new command id must pick a side: every id in the one table is in the rule table exactly once.
static void test_every_command_is_classified() {
    for (const Cmd c : remote::kCmdIds) {
        int n = 0;
        for (const auto& r : remote::kShellRules) n += r.cmd == c;
        CHECK_EQ(n, 1);
    }
    CHECK_EQ(remote::shell_rules_cover_all(), true);
}

static void test_read_only_commands_never_show() {
    for (const Cmd c : {Cmd::Ping, Cmd::GetState, Cmd::GetPdos, Cmd::GetEmarker, Cmd::GetIdentity, Cmd::Log, Cmd::Screenshot,
                        Cmd::FlashDump, Cmd::BootInfo, Cmd::Memory, Cmd::UsbLink, Cmd::PdMatch, Cmd::PdTriggerStatus,
                        Cmd::AdcRaw, Cmd::ChargerHunt, Cmd::Diagnose, Cmd::LoopProbe, Cmd::TraceHid}) {
        CHECK_EQ(ctl(c), false);
        CHECK_EQ(ctl(c, {0}), false);
        CHECK_EQ(ctl(c, {1, 2, 3}), false);
    }
}

static void test_driving_commands_show() {
    for (const Cmd c : {Cmd::Button, Cmd::Action, Cmd::SetPage, Cmd::Reboot, Cmd::DfuEnter, Cmd::FactoryWipe, Cmd::DebugFault,
                        Cmd::PdRequest, Cmd::PdReset, Cmd::PdRelease, Cmd::PdAttach, Cmd::PdHandback, Cmd::Stop,
                        Cmd::ChargerUnits, Cmd::EmarkerProbe, Cmd::Lab, Cmd::DebugLink, Cmd::DebugPdos, Cmd::DebugPdMsg,
                        Cmd::DebugCcMatrix, Cmd::DebugCcProbe}) {
        CHECK_EQ(ctl(c), true);
        CHECK_EQ(ctl(c, {1}), true);
    }
}

// Sub-ops: the status reads are quiet, the changes show; an empty payload never reads p[0].
static void test_sub_ops_split_by_op_byte() {
    CHECK_EQ(ctl(Cmd::Power), false);
    CHECK_EQ(ctl(Cmd::Power, {1, 80}), true);
    CHECK_EQ(ctl(Cmd::ChargerSweep, {1}), true);
    CHECK_EQ(ctl(Cmd::ChargerLevel, {2}), true);
    CHECK_EQ(ctl(Cmd::ChargerCable), false);                      // reads the next START's cable back
    CHECK_EQ(ctl(Cmd::ChargerCable, {1, 5, 0}), true);
    CHECK_EQ(ctl(Cmd::ChargerSvoocCap), false);
    CHECK_EQ(ctl(Cmd::ChargerSvoocCap, {1}), true);
    CHECK_EQ(ctl(Cmd::PdStress), false);
    CHECK_EQ(ctl(Cmd::PdStress, {remote::kPdStressStatus}), false);
    CHECK_EQ(ctl(Cmd::PdStress, {remote::kPdStressStart, 1}), true);
    CHECK_EQ(ctl(Cmd::PdStress, {remote::kPdStressRead, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::PdStress, {remote::kPdStressStop}), true);
    CHECK_EQ(ctl(Cmd::PdSpec, {remote::kPdSpecStatus}), false);
    CHECK_EQ(ctl(Cmd::PdSpec, {remote::kPdSpecStart, 1}), true);
    CHECK_EQ(ctl(Cmd::PdSpec, {remote::kPdSpecRead, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::PdLimit, {0}), false);
    CHECK_EQ(ctl(Cmd::PdLimit, {1, 1, 0, 0, 0}), true);
    CHECK_EQ(ctl(Cmd::PdLimit, {2}), true);
    CHECK_EQ(ctl(Cmd::Lease, {0}), false);
    CHECK_EQ(ctl(Cmd::Lease, {1, 30, 0}), true);
    CHECK_EQ(ctl(Cmd::IdentifyDevice, {0}), false);
    CHECK_EQ(ctl(Cmd::IdentifyDevice, {1}), true);
    CHECK_EQ(ctl(Cmd::PdReread, {0}), false);
    CHECK_EQ(ctl(Cmd::PdReread, {1, 31}), true);
    CHECK_EQ(ctl(Cmd::Store, {remote::kStoreList}), false);
    CHECK_EQ(ctl(Cmd::Store, {remote::kStoreSaveEmarker}), true);
    CHECK_EQ(ctl(Cmd::Store, {remote::kStoreSaveLog}), true);
    CHECK_EQ(ctl(Cmd::Cards, {remote::kCardsList}), false);
    CHECK_EQ(ctl(Cmd::Cards, {remote::kCardsGet, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::Cards, {remote::kCardsRename, 0}), true);
    CHECK_EQ(ctl(Cmd::Cards, {remote::kCardsDelete, 0}), true);
    CHECK_EQ(ctl(Cmd::EmLib, {remote::kEmList}), false);
    CHECK_EQ(ctl(Cmd::EmLib, {remote::kEmGet, 0}), false);
    CHECK_EQ(ctl(Cmd::EmLib, {remote::kEmSave}), true);
    CHECK_EQ(ctl(Cmd::EmLib, {remote::kEmDelete, 0}), true);
    CHECK_EQ(ctl(Cmd::EmLib, {remote::kEmSpoof, remote::kEmSpoofArm}), true);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwList}), false);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwStatus}), false);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwResult}), false);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwPredict, 0}), false);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwRun, 0, 0}), true);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwLearn, 0}), true);
    CHECK_EQ(ctl(Cmd::Twin, {remote::kTwCancel}), true);
    CHECK_EQ(ctl(Cmd::BlackBox, {remote::kBbRead}), false);
    CHECK_EQ(ctl(Cmd::BlackBox, {remote::kBbStatus}), false);
    CHECK_EQ(ctl(Cmd::BlackBox, {remote::kBbClear}), true);
    CHECK_EQ(ctl(Cmd::BlackBox, {remote::kBbFreeze, 1}), true);
    CHECK_EQ(ctl(Cmd::TransCap, {remote::kTcStatus}), false);
    CHECK_EQ(ctl(Cmd::TransCap, {remote::kTcLive}), false);
    CHECK_EQ(ctl(Cmd::TransCap, {remote::kTcArm}), true);
    CHECK_EQ(ctl(Cmd::TransCap, {remote::kTcTrigger}), true);
    CHECK_EQ(ctl(Cmd::Ufcs, {remote::kUfcsStatus}), false);
    CHECK_EQ(ctl(Cmd::Ufcs, {remote::kUfcsRead, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::Ufcs, {remote::kUfcsSet}), true);
    CHECK_EQ(ctl(Cmd::Ufcs, {remote::kUfcsProbe}), true);
    CHECK_EQ(ctl(Cmd::QRTest, {remote::kQrTestList}), false);
    CHECK_EQ(ctl(Cmd::QRTest, {remote::kQrTestShow, 1}), true);
    CHECK_EQ(ctl(Cmd::QRTest, {remote::kQrTestStop}), true);
    CHECK_EQ(ctl(Cmd::CableR), false);
    CHECK_EQ(ctl(Cmd::CableR, {remote::kCableStatus}), false);
    CHECK_EQ(ctl(Cmd::CableR, {remote::kCablePassive}), true);
    CHECK_EQ(ctl(Cmd::AppleRead, {0}), false);
    CHECK_EQ(ctl(Cmd::AppleRead, {1}), true);
    CHECK_EQ(ctl(Cmd::OppoProbe, {0, 0}), false);
    CHECK_EQ(ctl(Cmd::OppoProbe, {1, 0}), true);
    CHECK_EQ(ctl(Cmd::MiCableAuth, {0}), false);
    CHECK_EQ(ctl(Cmd::MiCableAuth, {1, 1}), true);
    CHECK_EQ(ctl(Cmd::DebugVconn, {0}), false);
    CHECK_EQ(ctl(Cmd::DebugVconn, {1}), true);
    CHECK_EQ(ctl(Cmd::DebugEprInfo, {0}), false);
    CHECK_EQ(ctl(Cmd::DebugEprInfo, {1}), true);
    CHECK_EQ(ctl(Cmd::Battery, {0}), false);                      // status
    CHECK_EQ(ctl(Cmd::Battery, {6}), false);                      // list
    CHECK_EQ(ctl(Cmd::Battery, {7, 0}), false);                   // read
    CHECK_EQ(ctl(Cmd::Battery, {1}), true);                       // start
    CHECK_EQ(ctl(Cmd::Battery, {2}), true);
    CHECK_EQ(ctl(Cmd::Battery, {4}), true);
}

// FC: the status, the VOOC monitor and the SVOOC status / log are reads; the triggers and the SVOOC start / stop drive.
static void test_fc_ops() {
    CHECK_EQ(ctl(Cmd::Fc), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcStatus}), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcReset}), true);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcScan, 1}), true);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcTrigger, 0, 0, 0, 0, 0}), true);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcVooc, 0}), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcVooc, 0, 1}), true);                 // switches the monitor on
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcSvooc}), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcSvooc, remote::kSvoocStatus, 0}), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcSvooc, remote::kSvoocLog, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcSvooc, remote::kSvoocStart, 1}), true);
    CHECK_EQ(ctl(Cmd::Fc, {remote::kFcSvooc, remote::kSvoocStop}), true);
}

// The charger test shares one command id between its reads and its START / CANCEL; the legacy 9-byte read can begin
// with the same byte values and must stay a read.
static void test_charger_test_form() {
    CHECK_EQ(ctl(Cmd::ChargerTest), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSchema2}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestViewReq}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestNowReq}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSpecReq, 0}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSpecDetailReq, 0}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSpecApdoReq, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSpecRowsReq, 0, 0}), false);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestStartReq}), true);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestStartReq, remote::kCtStartConfirm}), true);
    CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestCancelReq}), true);
    for (const uint8_t first : {remote::kChargerTestStartReq, remote::kChargerTestCancelReq}) {
        CHECK_EQ(ctl(Cmd::ChargerTest, {first, 0, 0, 0, 0, 0, 0, 0, 0}), false);       // legacy rows read: start index, run, revision
        CHECK_EQ(ctl(Cmd::ChargerTest, {remote::kChargerTestSchema2, first, 0, 0, 0, 0, 0, 0, 0, 0}), false);
    }
}

// Timing. The band shows from the first control request, a run the shell started holds it, otherwise it clears 5 s after
// the last control request; a local press is not an input here, so it can never clear a held band.
static void test_shows_on_first_control_and_clears_after_5s() {
    shell::Band b;
    CHECK_EQ(b.shown(), false);
    CHECK_EQ(b.tick(1000, false), false);
    CHECK_EQ(b.control(2000), true);                 // the edge: the caller repaints
    CHECK_EQ(b.shown(), true);
    CHECK_EQ(b.control(2500), false);                // no edge on a refresh: no repaint
    CHECK_EQ(b.tick(7499, false), false);
    CHECK_EQ(b.shown(), true);
    CHECK_EQ(b.tick(7500, false), true);             // 5 s after the last control request (2500)
    CHECK_EQ(b.shown(), false);
    CHECK_EQ(b.tick(9000, false), false);
}

static void test_every_control_extends_the_window() {
    shell::Band b;
    b.control(0);
    for (uint32_t t = 4000; t <= 40000; t += 4000) {
        b.control(t);
        CHECK_EQ(b.tick(t + 1, false), false);
        CHECK_EQ(b.shown(), true);
    }
    CHECK_EQ(b.tick(40000 + 5000, false), true);
}

static void test_a_run_the_shell_started_holds_the_band() {
    shell::Band b;
    b.control(1000);
    CHECK_EQ(b.tick(1100, true), false);             // the run starts inside the window: the shell owns it
    CHECK_EQ(b.tick(1000 + 5000, true), false);
    CHECK_EQ(b.tick(600000, true), false);
    CHECK_EQ(b.shown(), true);
    CHECK_EQ(b.tick(600100, false), false);          // the run ends: another 5 s
    CHECK_EQ(b.shown(), true);
    CHECK_EQ(b.tick(600100 + 4999, false), false);
    CHECK_EQ(b.tick(600100 + 5000, false), true);
    CHECK_EQ(b.shown(), false);
}

static void test_a_run_started_at_the_keys_does_not_hold() {
    shell::Band b;
    CHECK_EQ(b.tick(1000, true), false);             // a local run: nothing shown, nothing owned
    b.control(2000);                                 // the shell only reads the results with a control-class poll
    CHECK_EQ(b.tick(2100, true), false);             // the run was already going: not the shell's
    CHECK_EQ(b.tick(7000, true), true);
    CHECK_EQ(b.shown(), false);
}

static void test_control_during_a_shell_run_changes_nothing_visible() {
    shell::Band b;
    b.control(0);
    b.tick(10, true);
    CHECK_EQ(b.control(20000), false);
    CHECK_EQ(b.tick(20001, true), false);
    CHECK_EQ(b.tick(30000, false), false);
    CHECK_EQ(b.tick(35000, false), true);
}

static void test_a_new_run_after_the_band_cleared_is_not_owned() {
    shell::Band b;
    b.control(0);
    b.tick(1, true);
    b.tick(100, false);
    CHECK_EQ(b.tick(5100, false), true);
    CHECK_EQ(b.tick(6000, true), false);             // a local start later on
    CHECK_EQ(b.shown(), false);
    CHECK_EQ(b.tick(60000, true), false);
}

static void test_clock_wraps_around() {
    shell::Band b;
    const uint32_t t0 = 0xFFFFFFFFu - 1000;
    CHECK_EQ(b.control(t0), true);
    CHECK_EQ(b.tick(t0 + 4999, false), false);       // wraps past zero inside the window
    CHECK_EQ(b.tick(t0 + 5000, false), true);
}

static void test_reset_hides_without_an_edge_report() {
    shell::Band b;
    b.control(0);
    b.reset();
    CHECK_EQ(b.shown(), false);
    CHECK_EQ(b.tick(1, true), false);
    CHECK_EQ(b.tick(9000, true), false);
    CHECK_EQ(b.shown(), false);
}

int main() {
    test_every_command_is_classified();
    test_read_only_commands_never_show();
    test_driving_commands_show();
    test_sub_ops_split_by_op_byte();
    test_fc_ops();
    test_charger_test_form();
    test_shows_on_first_control_and_clears_after_5s();
    test_every_control_extends_the_window();
    test_a_run_the_shell_started_holds_the_band();
    test_a_run_started_at_the_keys_does_not_hold();
    test_control_during_a_shell_run_changes_nothing_visible();
    test_a_new_run_after_the_band_cleared_is_not_owned();
    test_clock_wraps_around();
    test_reset_hides_without_an_edge_report();
    return test_report("test_shell_toast");
}
