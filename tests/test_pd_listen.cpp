// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The lab "listen attach" state machine (k2/pd/pd_listen.hpp): refusals, the window, the end reasons. That nothing is ever
// transmitted is proven on the wire side (tests/test_fusb302_scan.cpp, the listen cases).
#include "k2/pd/pd_listen.hpp"
#include "test_util.h"

using namespace k2::pd;

static void test_secs_default_and_limits() {
    CHECK_EQ(int(listen_secs(0)), 10);
    CHECK_EQ(int(listen_secs(7)), 7);
    CHECK_EQ(int(kListenMaxSecs), 30);
    Listen l;
    CHECK_EQ(int(l.start(0, 31, kListenPortAuto, {})), int(ListenRefusal::BadArg));   // above the maximum
    CHECK_EQ(int(l.start(0, 255, kListenPortAuto, {})), int(ListenRefusal::BadArg));
    CHECK_EQ(int(l.start(0, 5, 3, {})), int(ListenRefusal::BadArg));                   // no such port label
    CHECK_EQ(l.running(), false);                                                       // a refused start changes nothing
    CHECK_EQ(int(l.end()), int(ListenEnd::None));
    CHECK_EQ(int(l.start(0, 30, kListenPortTop, {})), int(ListenRefusal::None));       // the maximum itself is fine
    CHECK_EQ(int(l.secs()), 30);
    Listen d;
    CHECK_EQ(int(d.start(0, 0, kListenPortAuto, {})), int(ListenRefusal::None));       // 0 = the default
    CHECK_EQ(int(d.secs()), 10);
}

static void test_refusals_each_have_their_own_reason_and_start_nothing() {
    struct Case { ListenFacts f; ListenRefusal want; };
    const Case cases[] = {
        {{.ports_busy = true}, ListenRefusal::Busy},          // a charger test, PD Stress, a twin run, a trigger
        {{.inline_mode = true}, ListenRefusal::Inline},       // an inline sink
        {{.rd_pinned = true}, ListenRefusal::Pinned},         // the K2 lives off this port
        {{.spoof_armed = true}, ListenRefusal::Spoof},        // the spoof's Auto-CRC would answer the cable
    };
    for (const Case& c : cases) {
        Listen l;
        CHECK_EQ(int(l.start(100, 10, kListenPortBottom, c.f)), int(c.want));
        CHECK_EQ(l.running(), false);
        CHECK_EQ(int(l.end()), int(ListenEnd::None));
        CHECK_EQ(l.tick(100000), false);                                    // never starts to run by itself
        CHECK_EQ(int(l.left_s(100)), 0);
    }
    // busy wins over the rest: the run's owner is the most useful thing to name
    Listen l;
    CHECK_EQ(int(l.start(0, 10, 0, {.ports_busy = true, .inline_mode = true, .rd_pinned = true})), int(ListenRefusal::Busy));
    CHECK_EQ(int(l.start(0, 10, 0, {.inline_mode = true, .rd_pinned = true})), int(ListenRefusal::Inline));
    CHECK_EQ(int(l.start(0, 10, 0, {.rd_pinned = true, .spoof_armed = true})), int(ListenRefusal::Pinned));
    // refusal names exist for each
    for (int i = 0; i <= int(ListenRefusal::Running); i++) CHECK_COND(listen_refusal_name(ListenRefusal(i))[0] != '?');
}

static void test_window_times_out_and_counts_down() {
    Listen l;
    CHECK_EQ(int(l.start(5000, 10, kListenPortBottom, {})), int(ListenRefusal::None));
    CHECK_EQ(l.running(), true);
    CHECK_EQ(int(l.port()), int(kListenPortBottom));
    CHECK_EQ(int(l.left_s(5000)), 10);
    CHECK_EQ(int(l.left_s(5001)), 10);      // rounded up
    CHECK_EQ(int(l.left_s(13999)), 2);
    CHECK_EQ(int(l.left_s(14001)), 1);
    CHECK_EQ(l.tick(14999), true);          // one millisecond short
    CHECK_EQ(l.running(), true);
    CHECK_EQ(l.tick(15000), false);         // the window is over
    CHECK_EQ(l.running(), false);
    CHECK_EQ(int(l.end()), int(ListenEnd::Done));
    CHECK_EQ(int(l.left_s(15000)), 0);
    CHECK_EQ(l.tick(16000), false);         // and stays over
    CHECK_EQ(int(l.end()), int(ListenEnd::Done));
}

static void test_the_millisecond_clock_wraps() {
    Listen l;
    const std::uint32_t t0 = 0xFFFFFF00u;   // the window straddles the 32-bit wrap
    CHECK_EQ(int(l.start(t0, 1, 0, {})), int(ListenRefusal::None));
    CHECK_EQ(l.tick(t0 + 999), true);
    CHECK_EQ(l.tick(t0 + 1000), false);
    CHECK_EQ(int(l.end()), int(ListenEnd::Done));
}

static void test_stop_and_lost_end_it_at_once_and_a_second_start_is_refused_while_running() {
    Listen l;
    CHECK_EQ(int(l.start(0, 10, 0, {})), int(ListenRefusal::None));
    CHECK_EQ(int(l.start(1, 10, 0, {})), int(ListenRefusal::Running));
    CHECK_EQ(l.running(), true);
    l.stop();
    CHECK_EQ(l.running(), false);
    CHECK_EQ(int(l.end()), int(ListenEnd::Stopped));
    l.lost();                                // already over: the first reason stands
    CHECK_EQ(int(l.end()), int(ListenEnd::Stopped));
    CHECK_EQ(int(l.start(2, 10, 0, {})), int(ListenRefusal::None));   // free again
    l.lost();
    CHECK_EQ(int(l.end()), int(ListenEnd::Lost));
    CHECK_EQ(l.tick(3), false);
    Listen idle;                              // stopping what never ran is a no-op
    idle.stop();
    CHECK_EQ(int(idle.end()), int(ListenEnd::None));
}

static void test_frames_count_and_reset_with_the_next_start() {
    Listen l;
    CHECK_EQ(int(l.start(0, 1, 0, {})), int(ListenRefusal::None));
    for (int i = 0; i < 5; i++) l.frame();
    CHECK_EQ(int(l.frames()), 5);
    l.tick(1000);
    CHECK_EQ(int(l.frames()), 5);            // the count outlives the window: the status reads it afterwards
    CHECK_EQ(int(l.start(2000, 1, 0, {})), int(ListenRefusal::None));
    CHECK_EQ(int(l.frames()), 0);
    for (int i = 0; i < 70000; i++) l.frame();
    CHECK_EQ(int(l.frames()), 0xFFFF);       // saturates
}

int main() {
    test_secs_default_and_limits();
    test_refusals_each_have_their_own_reason_and_start_nothing();
    test_window_times_out_and_counts_down();
    test_the_millisecond_clock_wraps();
    test_stop_and_lost_end_it_at_once_and_a_second_start_is_refused_while_running();
    test_frames_count_and_reset_with_the_next_start();
    return test_report("test_pd_listen");
}
