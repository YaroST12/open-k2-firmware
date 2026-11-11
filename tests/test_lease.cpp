// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The HID dead-man lease (k2/core/lease.hpp): pure timing and clear rules. The firmware wiring (which requests arm, the real
// stop) is in remote.cpp; this pins the arithmetic: arm, refresh, expiry, unplug, the clear conditions and the 32-bit clock wrap.
#include "test_util.h"
#include "k2/core/lease.hpp"

using namespace k2;

static lease::Event tick(lease::Lease& l, uint32_t now, bool host = true, bool safe = false, bool test = false) {
    return l.tick(now, host, safe, test);
}

static void test_idle_lease_never_fires() {
    lease::Lease l;
    CHECK_EQ(tick(l, 1000000, false, false, true) == lease::Event::None, true);   // nothing armed: even an unplug is nothing
    CHECK_EQ(l.info(5).armed, false);
    CHECK_EQ(l.info(5).left_s, 0);
}

static void test_arm_then_expire_after_the_default_lease() {
    lease::Lease l;
    l.arm(1000);
    CHECK_EQ(l.info(1000).armed, true);
    CHECK_EQ(l.info(1000).length_s, 30);
    CHECK_EQ(l.info(1000).left_s, 30);
    CHECK_EQ(tick(l, 1000 + 29999) == lease::Event::None, true);
    CHECK_EQ(l.info(1000 + 29999).left_s, 1);                  // rounds up: never reads 0 while armed
    CHECK_EQ(tick(l, 1000 + 30000) == lease::Event::Expired, true);
    CHECK_EQ(l.info(1000 + 30000).armed, false);               // the event fires once
    CHECK_EQ(l.info(1000 + 30000).ended == lease::End::Expired, true);
    CHECK_EQ(tick(l, 1000 + 60000) == lease::Event::None, true);
}

static void test_any_request_refreshes_the_lease() {
    lease::Lease l;
    l.arm(0);
    for (uint32_t t = 10000; t <= 100000; t += 10000) {
        l.touch(t);
        CHECK_EQ(tick(l, t) == lease::Event::None, true);
    }
    CHECK_EQ(tick(l, 100000 + 29999) == lease::Event::None, true);
    CHECK_EQ(tick(l, 100000 + 30000) == lease::Event::Expired, true);
}

static void test_touch_without_an_armed_lease_arms_nothing() {
    lease::Lease l;
    l.touch(500);
    CHECK_EQ(l.info(500).armed, false);
    CHECK_EQ(tick(l, 500 + 100000) == lease::Event::None, true);
}

static void test_unplug_stops_at_once_and_beats_the_timer() {
    lease::Lease l;
    l.arm(0);
    CHECK_EQ(tick(l, 1, false) == lease::Event::HostGone, true);   // 1 ms after arming, no waiting for 30 s
    CHECK_EQ(l.info(1).armed, false);
    CHECK_EQ(l.info(1).ended == lease::End::HostGone, true);
    l.arm(2);
    CHECK_EQ(tick(l, 2 + 31000, false) == lease::Event::HostGone, true);   // both true: the reason is the unplug
}

static void test_set_length_applies_to_the_next_arm_only() {
    lease::Lease l;
    l.set_length(100, 5);
    l.arm(200);
    CHECK_EQ(l.info(200).length_s, 5);
    CHECK_EQ(tick(l, 200 + 4999) == lease::Event::None, true);
    CHECK_EQ(tick(l, 200 + 5000) == lease::Event::Expired, true);
    l.arm(10000);                                               // consumed: back to the default
    CHECK_EQ(l.info(10000).length_s, 30);
}

static void test_set_length_zero_is_the_default_and_pending_goes_stale() {
    lease::Lease l;
    l.set_length(0, 0);
    l.arm(10);
    CHECK_EQ(l.info(10).length_s, 30);
    l.clear();
    l.set_length(100, 7);
    l.arm(100 + lease::kPendingMs);                             // a refused command left it behind: too old to apply
    CHECK_EQ(l.info(100 + lease::kPendingMs).length_s, 30);
    l.clear();
    l.set_length(1000, 7);
    l.arm(1000 + lease::kPendingMs - 1);                        // still fresh
    CHECK_EQ(l.info(1000 + lease::kPendingMs - 1).length_s, 7);
}

static void test_set_length_clamps_to_the_maximum() {
    lease::Lease l;
    l.set_length(0, 60000);
    l.arm(0);
    CHECK_EQ(l.info(0).length_s, lease::kMaxSeconds);
    CHECK_EQ(lease::kMaxSeconds, 600);
}

static void test_set_length_while_armed_changes_the_running_lease() {
    lease::Lease l;
    l.arm(0);
    l.set_length(20000, 60);                                    // also a request: refreshes
    CHECK_EQ(l.info(20000).length_s, 60);
    CHECK_EQ(tick(l, 20000 + 59999) == lease::Event::None, true);
    CHECK_EQ(tick(l, 20000 + 60000) == lease::Event::Expired, true);
    l.arm(100000);
    l.set_length(100001, 0);                                    // 0 goes back to the default
    CHECK_EQ(l.info(100001).length_s, 30);
}

static void test_second_arm_keeps_the_running_length() {
    lease::Lease l;
    l.set_length(0, 45);
    l.arm(0);
    l.arm(10000);                                               // a second arming command without --lease
    CHECK_EQ(l.info(10000).length_s, 45);
    CHECK_EQ(l.info(10000).left_s, 45);                         // and it refreshed
}

static void test_clear_conditions() {
    lease::Lease l;
    l.arm(0);
    l.clear();                                                  // stop / release / reset
    CHECK_EQ(l.info(1).armed, false);
    CHECK_EQ(l.info(1).ended == lease::End::Released, true);
    CHECK_EQ(tick(l, 100000) == lease::Event::None, true);

    l.arm(0);                                                   // a verified 5 V: held for kSafeHoldMs, no stop needed
    CHECK_EQ(tick(l, 1000, true, true) == lease::Event::None, true);
    CHECK_EQ(l.info(1000).armed, true);
    CHECK_EQ(tick(l, 1000 + lease::kSafeHoldMs - 1, true, true) == lease::Event::None, true);
    CHECK_EQ(l.info(1000 + lease::kSafeHoldMs - 1).armed, true);
    CHECK_EQ(tick(l, 1000 + lease::kSafeHoldMs, true, true) == lease::Event::None, true);
    CHECK_EQ(l.info(1000 + lease::kSafeHoldMs).armed, false);
    CHECK_EQ(l.info(1000 + lease::kSafeHoldMs).ended == lease::End::Settled, true);
}

static void test_settled_needs_an_unbroken_run_of_safe_samples() {
    lease::Lease l;
    l.arm(0);
    l.touch(2000);
    tick(l, 2000, true, true);
    tick(l, 5000, true, false);                                 // negotiating: the run restarts
    l.touch(6000);
    tick(l, 6000, true, true);
    tick(l, 6000 + lease::kSafeHoldMs - 1, true, true);
    CHECK_EQ(l.info(6000 + lease::kSafeHoldMs - 1).armed, true);
    tick(l, 6000 + lease::kSafeHoldMs, true, true);
    CHECK_EQ(l.info(6000 + lease::kSafeHoldMs).armed, false);
}

static void test_a_new_arm_restarts_the_safe_run() {
    lease::Lease l;
    l.arm(0);
    tick(l, 100, true, true);
    l.arm(4000);                                                // re-armed (a second attach): the old safe run does not count
    tick(l, lease::kSafeHoldMs + 100, true, true);
    CHECK_EQ(l.info(lease::kSafeHoldMs + 100).armed, true);
}

static void test_charger_test_is_not_host_owned_voltage() {
    lease::Lease l;
    l.arm(0);
    CHECK_EQ(tick(l, 100, true, false, true) == lease::Event::None, true);   // a test took the ports: no stop, ever
    CHECK_EQ(l.info(100).armed, false);
    CHECK_EQ(l.info(100).ended == lease::End::TestRunning, true);
    CHECK_EQ(tick(l, 100000, true, false, true) == lease::Event::None, true);
    l.arm(200000);                                              // a test running while the cable is pulled: still no stop
    CHECK_EQ(tick(l, 200001, false, false, true) == lease::Event::None, true);
    CHECK_EQ(l.info(200001).ended == lease::End::TestRunning, true);
    l.arm(300000);                                              // and a lease that already ran out: the test wins too
    CHECK_EQ(tick(l, 300000 + 31000, true, false, true) == lease::Event::None, true);
}

static void test_clock_wraps_around() {
    lease::Lease l;
    const uint32_t t0 = 0xFFFFFFFFu - 5000;                     // 5 s before the 32-bit millisecond clock wraps
    l.arm(t0);
    CHECK_EQ(tick(l, t0 + 20000) == lease::Event::None, true);  // now small: 14999 ms past the wrap
    CHECK_EQ(l.info(t0 + 20000).left_s, 10);
    l.touch(t0 + 20000);
    CHECK_EQ(tick(l, t0 + 20000 + 29999) == lease::Event::None, true);
    CHECK_EQ(tick(l, t0 + 20000 + 30000) == lease::Event::Expired, true);

    l.arm(0xFFFFFF00u);                                         // armed at the very top of the range
    CHECK_EQ(tick(l, 0xFFFFFFFFu) == lease::Event::None, true);
    CHECK_EQ(tick(l, 0xFFFFFF00u + 30000u) == lease::Event::Expired, true);
}

int main() {
    test_idle_lease_never_fires();
    test_arm_then_expire_after_the_default_lease();
    test_any_request_refreshes_the_lease();
    test_touch_without_an_armed_lease_arms_nothing();
    test_unplug_stops_at_once_and_beats_the_timer();
    test_set_length_applies_to_the_next_arm_only();
    test_set_length_zero_is_the_default_and_pending_goes_stale();
    test_set_length_clamps_to_the_maximum();
    test_set_length_while_armed_changes_the_running_lease();
    test_second_arm_keeps_the_running_length();
    test_clear_conditions();
    test_settled_needs_an_unbroken_run_of_safe_samples();
    test_a_new_arm_restarts_the_safe_run();
    test_charger_test_is_not_host_owned_voltage();
    test_clock_wraps_around();
    return test_report("test_lease");
}
