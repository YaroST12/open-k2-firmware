// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The one-call `stop` (k2/core/stop.hpp): what it cancels, in which ORDER, and what it reports. The order is the safety
// property: a charger test is cancelled before the FC release starts (the test owns the link and would re-trigger), the FC
// trigger is stopped before PD is released (the FC release window measures the 5 V that the PD release must reach), and the
// spoof is disarmed last (it only stops answering an e-marker read; it never holds voltage).
#include "test_util.h"
#include "k2/core/stop.hpp"
#include <string>

using namespace k2;

namespace {

struct Env {
    bool test = false, fc = false, pd = false, spoof = false;   // what is active
    int32_t vbus = 5000;
    bool release_busy = false;
    std::string calls;
    bool test_active() { return test; }
    bool fc_active() { return fc; }
    bool pd_held() { return pd; }
    bool spoof_active() { return spoof; }
    void cancel_test() { calls += "test;"; test = false; }
    void stop_fc() { calls += "fc;"; fc = false; release_busy = true; }
    void release_pd() { calls += "pd;"; pd = false; }
    void disarm_spoof() { calls += "spoof;"; spoof = false; }
    int32_t vbus_mv() { calls += "vbus;"; return vbus; }
    bool fc_release_busy() { calls += "busy;"; return release_busy; }
};

}  // namespace

static void test_stop_order_and_report_everything_active() {
    Env e;
    e.test = e.fc = e.pd = e.spoof = true;
    e.vbus = 9000;
    const stop::Report r = stop::stop_all(e);
    // the four actions in the fixed order, then the measurements (after the actions, so the reply is the state afterwards)
    CHECK_STR(e.calls.c_str(), "test;fc;pd;spoof;vbus;busy;");
    CHECK_EQ(r.active, stop::kTest | stop::kFc | stop::kPd | stop::kSpoof);
    CHECK_EQ(r.vbus_mv, 9000);
    CHECK_EQ(r.fc_release_busy, true);   // stop_fc started the measured release window
}

static void test_stop_idle_device_still_runs_the_release_path() {
    Env e;
    const stop::Report r = stop::stop_all(e);
    CHECK_EQ(r.active, 0);
    CHECK_EQ(r.vbus_mv, 5000);
    // the PD release is unconditional (a no-op at 5 V in the driver); the others are skipped when idle
    CHECK_STR(e.calls.c_str(), "pd;vbus;busy;");
    CHECK_EQ(r.fc_release_busy, false);
}

static void test_stop_flags_are_what_was_active_before() {
    Env e;
    e.fc = true;
    e.spoof = true;
    const stop::Report r = stop::stop_all(e);
    CHECK_EQ(r.active, stop::kFc | stop::kSpoof);   // not the state after: the test and PD bits stay clear
    CHECK_STR(e.calls.c_str(), "fc;pd;spoof;vbus;busy;");
}

static void test_stop_flag_bits_are_wire_stable() {
    CHECK_EQ(stop::kTest, 1);
    CHECK_EQ(stop::kFc, 2);
    CHECK_EQ(stop::kPd, 4);
    CHECK_EQ(stop::kSpoof, 8);
}

int main() {
    test_stop_order_and_report_everything_active();
    test_stop_idle_device_still_runs_the_release_path();
    test_stop_flags_are_what_was_active_before();
    test_stop_flag_bits_are_wire_stable();
    return test_report("test_stop");
}
