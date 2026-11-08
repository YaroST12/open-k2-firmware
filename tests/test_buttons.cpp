// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the debounce + short/long press detector (time and pin state in, event out).
#include "test_util.h"
#include "k2/drivers/buttons.hpp"

using namespace k2;

// Feed the detector `level` every `step` ms over [from, to] inclusive; return the events seen.
struct Seen { int shorts = 0, longs = 0, holds = 0; uint32_t long_at = 0, short_at = 0, hold_at = 0; };

static void run(ButtonDetector &d, Seen &s, bool level, uint32_t from, uint32_t to, uint32_t step = 5) {
    for (uint32_t t = from; t <= to; t += step) {
        switch (d.update(level, t)) {
        case ButtonEvent::Short: s.shorts++; s.short_at = t; break;
        case ButtonEvent::Long:  s.longs++;  s.long_at = t;  break;
        case ButtonEvent::Hold:  s.holds++;  s.hold_at = t;  break;
        case ButtonEvent::None:  break;
        }
    }
}

static void test_short_press() {
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    run(d, s, true, 1000, 1199);      // 200 ms press
    CHECK_EQ(d.pressed(), 1);
    CHECK_EQ(s.shorts, 0);            // reported on release, not on press
    run(d, s, false, 1200, 1400);
    CHECK_EQ(s.shorts, 1);
    CHECK_EQ(s.longs, 0);
    CHECK_EQ(d.pressed(), 0);
}

static void test_long_press_exact() {
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    run(d, s, true, 1000, 1495);      // held 495 ms: nothing yet
    CHECK_EQ(s.longs, 0);
    CHECK_EQ(d.update(true, 1499) == ButtonEvent::None, 1);   // 499 ms
    CHECK_EQ(d.update(true, 1500) == ButtonEvent::Long, 1);   // exactly 500 ms from the first edge
    run(d, s, true, 1505, 1900);
    run(d, s, false, 1905, 2200);
    CHECK_EQ(s.longs, 0);             // already consumed above, none repeated
    CHECK_EQ(s.shorts, 0);            // a long press is never also a short press
}

static void test_no_repeat_while_held() {
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    run(d, s, true, 1000, 10000, 10);
    CHECK_EQ(s.longs, 1);
    CHECK_EQ(s.long_at, 1500);
    run(d, s, false, 10010, 10200, 10);
    CHECK_EQ(s.longs, 1);
    CHECK_EQ(s.shorts, 0);
}

static void test_hold_press() {
    // Long still fires at 500 ms; Hold fires once, 5000 ms after the first edge (the reboot gesture).
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    run(d, s, true, 1000, 5995);                 // held 4995 ms
    CHECK_EQ(s.longs, 1);
    CHECK_EQ(s.holds, 0);
    CHECK_EQ(d.update(true, 5999) == ButtonEvent::None, 1);   // 4999 ms: no hold yet
    CHECK_EQ(d.update(true, 6000) == ButtonEvent::Hold, 1);   // exactly 5000 ms
    run(d, s, true, 6005, 12000);
    CHECK_EQ(s.holds, 0);                        // none repeated by the loop above (one was consumed directly)
    run(d, s, false, 12005, 12300);
    CHECK_EQ(s.shorts, 0);
    CHECK_EQ(s.longs, 1);

    // Released at 4990 ms: a Long (it passed 500 ms) but no Hold.
    ButtonDetector e; Seen t;
    run(e, t, false, 0, 990);
    run(e, t, true, 1000, 5990);
    run(e, t, false, 5995, 6300);
    CHECK_EQ(t.longs, 1);
    CHECK_EQ(t.holds, 0);

    // A second press after a hold works again from scratch.
    run(d, s, true, 13000, 13600);
    CHECK_EQ(s.longs, 2);
}

static void test_bounce() {
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    // Contact bounce: flips every 5 ms for 15 ms, then settles pressed.
    d.update(true, 1000); d.update(false, 1005); d.update(true, 1010); d.update(false, 1015);
    run(d, s, true, 1020, 1300);
    run(d, s, false, 1305, 1320);
    // Release bounce.
    d.update(true, 1325); d.update(false, 1330);
    run(d, s, false, 1335, 1500);
    CHECK_EQ(s.shorts, 1);            // exactly one short press despite both bounces
    CHECK_EQ(s.longs, 0);

    // A glitch shorter than the debounce time is ignored.
    ButtonDetector g; Seen t;
    run(g, t, false, 0, 100);
    run(g, t, true, 105, 120);        // 15 ms
    run(g, t, false, 125, 400);
    CHECK_EQ(t.shorts, 0);
    CHECK_EQ(t.longs, 0);
}

static void test_long_time_measured_from_first_edge_after_bounce() {
    ButtonDetector d; Seen s;
    run(d, s, false, 0, 990);
    d.update(true, 1000); d.update(false, 1004);           // first touch bounces off
    run(d, s, true, 1008, 3000, 4);                         // real press starts at 1008
    CHECK_EQ(s.longs, 1);
    CHECK_EQ(s.long_at, 1508);
}

static void test_counter_wrap() {
    ButtonDetector d;
    uint32_t t = 0xFFFFFF00u;
    for (int i = 0; i < 20; i++, t += 4) d.update(false, t);
    int longs = 0;
    uint32_t press_t = t;                       // press starts just before the wrap
    for (int i = 0; i < 200; i++, t += 4) {
        if (d.update(true, t) == ButtonEvent::Long) { longs++; CHECK_EQ(t - press_t, 500); }
    }
    CHECK_EQ(longs, 1);
}

int main() {
    test_short_press();
    test_long_press_exact();
    test_no_repeat_while_held();
    test_hold_press();
    test_bounce();
    test_long_time_measured_from_first_edge_after_bounce();
    test_counter_wrap();
    return test_report("test_buttons");
}
