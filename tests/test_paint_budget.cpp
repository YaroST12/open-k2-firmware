// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The per-pass paint budget (include/k2/ui/ui_types.hpp PaintBudget): a pixel cap in cost units, solid fills weighing a
// quarter of a text pixel (they only cost wire time), and an optional guard on the measured time of the pass, so a pass
// stops asking for work once its time is nearly spent. The guard reads an injected microsecond clock.
#include "k2/ui/ui_types.hpp"
#include "test_util.h"

using k2::PaintBudget;

namespace {

uint32_t g_clock_us = 0;
uint32_t clock_us() { return g_clock_us; }

void test_pixel_cap_alone() {
    PaintBudget b;
    CHECK_EQ(b.cap, PaintBudget::kDefaultPx);
    CHECK_COND(b.admit(3000));
    CHECK_COND(!b.admit(2000));          // 5000 > 4500
    CHECK_COND(b.more);
    PaintBudget big;
    CHECK_COND(big.admit(9000, true));   // the first item always goes, however large
    CHECK_COND(!big.admit(1, true));
}

void test_fills_weigh_a_quarter() {
    PaintBudget b;
    int strips = 0;
    while (b.admit(3840, true, true)) strips++;   // a 16-row strip clear: 960 units each
    CHECK_EQ(strips, 4);                          // 4 x 960 = 3840 <= 4500; a fifth would pass the cap
    CHECK_COND(b.more);
    PaintBudget t;
    int text = 0;
    while (t.admit(3840, true)) text++;
    CHECK_EQ(text, 1);                            // as text, only the forced first item fits
}

void test_time_guard_refuses_what_would_end_late() {
    g_clock_us = 50'000;
    PaintBudget b;
    b.arm(clock_us, 8000);
    CHECK_COND(b.admit(1000, true));              // first item: always
    g_clock_us += 2300;                           // it took what the model says
    CHECK_COND(b.admit(1000, true));              // 2.3 ms spent + 1000 px x 2.3 us = 4.6 ms <= 8 ms
    g_clock_us += 2300;
    CHECK_COND(b.admit(1000, true));              // 4.6 + 2.3 = 6.9 ms
    g_clock_us += 2300;
    CHECK_COND(!b.admit(1000, true));             // 6.9 + 2.3 = 9.2 ms > 8 ms: stop here
    CHECK_COND(b.more);
}

void test_time_guard_counts_work_the_budget_never_saw() {
    g_clock_us = 1'000'000;
    PaintBudget b;
    b.arm(clock_us, 8000);
    g_clock_us += 7900;                           // the pass did 7.9 ms of other work before its first paint call
    CHECK_COND(b.admit(100, true));               // forced: a pass always makes progress
    CHECK_COND(!b.admit(100, true));              // but nothing else fits behind it
}

void test_time_guard_fill_is_cheaper() {
    g_clock_us = 0;
    PaintBudget b;
    b.arm(clock_us, 8000);
    CHECK_COND(b.admit(3840, true, true));
    g_clock_us += 2000;                           // a strip's wire time at the shipped divider
    CHECK_COND(b.admit(3840, true, true));        // 2 ms + 960 units x 2.3 us = 4.2 ms
    g_clock_us += 2000;
    CHECK_COND(b.admit(3840, true, true));        // 4 ms + 2.2 = 6.2 ms
    g_clock_us += 2000;
    CHECK_COND(!b.admit(3840, true, true));       // 6 ms + 2.2 = 8.2 ms
}

void test_time_guard_wraps_with_the_clock() {
    g_clock_us = 0xFFFFFF00u;                     // the 32-bit microsecond counter wraps: only differences count
    PaintBudget b;
    b.arm(clock_us, 8000);
    CHECK_COND(b.admit(500, true));
    g_clock_us += 1000;                           // now past the wrap
    CHECK_COND(b.admit(500, true));
}

void test_left_follows_the_tighter_bound() {
    g_clock_us = 0;
    PaintBudget b;
    CHECK_EQ(b.left(), PaintBudget::kDefaultPx);
    b.arm(clock_us, 8000);
    CHECK_EQ(b.left(), 3478);                     // 8000 us / 2.3 us per unit
    g_clock_us += 6000;
    CHECK_EQ(b.left(), 869);                      // 2000 / 2.3
    g_clock_us += 3000;
    CHECK_EQ(b.left(), 0);                        // past the limit
}

}  // namespace

int main() {
    test_pixel_cap_alone();
    test_fills_weigh_a_quarter();
    test_time_guard_refuses_what_would_end_late();
    test_time_guard_counts_work_the_budget_never_saw();
    test_time_guard_fill_is_cheaper();
    test_time_guard_wraps_with_the_clock();
    test_left_follows_the_tighter_bound();
    return test_report("test_paint_budget");
}
