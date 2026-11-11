// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The main-loop stall counters (include/k2/core/loop_stats.hpp): a gap histogram noted at the top of every main-loop pass and at
// every 1 ms PD service, and the longest pass per page with the pixels it painted. `debug loop` reads them; the world replay
// reads the same counters on the host.
#include "k2/core/loop_stats.hpp"
#include "test_util.h"

using namespace k2::loopstat;

static void test_bucket_edges_are_inclusive_upper_bounds() {
    CHECK_EQ(Gaps::bucket(0), 0u);
    CHECK_EQ(Gaps::bucket(1), 0u);     // a healthy pass: SysTick ends the wfi every ms
    CHECK_EQ(Gaps::bucket(2), 1u);
    CHECK_EQ(Gaps::bucket(5), 2u);
    CHECK_EQ(Gaps::bucket(6), 3u);
    CHECK_EQ(Gaps::bucket(20), 4u);
    CHECK_EQ(Gaps::bucket(21), 5u);
    CHECK_EQ(Gaps::bucket(100), 6u);
    CHECK_EQ(Gaps::bucket(101), 7u);   // the long Home repaint lands here (~150 ms)
    CHECK_EQ(Gaps::bucket(100000), 7u);
}

static void test_the_first_note_only_sets_the_reference() {
    Gaps g;
    g.note(500);
    CHECK_EQ(g.samples, 0u);
    CHECK_EQ(g.max_ms, 0u);
    g.note(501); g.note(502); g.note(652);
    CHECK_EQ(g.samples, 3u);
    CHECK_EQ(g.hist[0], 2u);
    CHECK_EQ(g.hist[7], 1u);
    CHECK_EQ(g.max_ms, 150u);
}

static void test_reset_keeps_the_reference_so_the_next_gap_still_counts() {
    Gaps g;
    g.note(0); g.note(40);
    g.reset();
    CHECK_EQ(g.samples, 0u);
    CHECK_EQ(g.max_ms, 0u);
    CHECK_EQ(g.hist[5], 0u);
    g.note(70);   // 30 ms since the last note before the reset
    CHECK_EQ(g.samples, 1u);
    CHECK_EQ(g.hist[5], 1u);
    CHECK_EQ(g.max_ms, 30u);
}

static void test_the_clock_wrap_gives_a_small_gap() {
    Gaps g;
    g.note(0xFFFFFFFFu);
    g.note(1);
    CHECK_EQ(g.max_ms, 2u);
}

static void test_a_page_keeps_its_longest_pass_and_the_pixels_of_that_pass() {
    PageWorst w;
    w.note(0, 3000, 100);
    w.note(0, 151000, 57600);   // the Home repaint
    w.note(0, 20000, 60000);    // more pixels but shorter: not the worst pass
    CHECK_EQ(w.get(0).us, 151000u);
    CHECK_EQ(w.get(0).px, 57600u);
    CHECK_EQ(w.get(1).us, 0u);   // Detail never ran
    CHECK_EQ(w.worst_page, 0u);
    w.note(3, 200000, 1000);
    CHECK_EQ(w.worst_page, 3u);
    CHECK_EQ(w.worst.us, 200000u);
}

static void test_a_page_outside_the_slots_lands_in_the_last_one() {
    PageWorst w;
    w.note(200, 5000, 7);
    CHECK_EQ(w.get(kPageSlots - 1).us, 5000u);
    CHECK_EQ(w.get(200).us, 5000u);
    CHECK_EQ(w.worst_page, 200u);   // the page index itself is reported
}

static void test_stats_reset_clears_all_and_counts_passes() {
    Stats s;
    s.loop.note(0); s.loop.note(10);
    s.service.note(0); s.service.note(3);
    s.note_pass(2, 9000, 11);
    s.note_pass(2, 1000, 0);
    CHECK_EQ(s.passes, 2u);
    s.reset(1234);
    CHECK_EQ(s.passes, 0u);
    CHECK_EQ(s.since_ms, 1234u);
    CHECK_EQ(s.loop.samples, 0u);
    CHECK_EQ(s.service.max_ms, 0u);
    CHECK_EQ(s.pages.get(2).us, 0u);
    CHECK_EQ(s.pages.worst.us, 0u);
}

int main() {
    test_bucket_edges_are_inclusive_upper_bounds();
    test_the_first_note_only_sets_the_reference();
    test_reset_keeps_the_reference_so_the_next_gap_still_counts();
    test_the_clock_wrap_gives_a_small_gap();
    test_a_page_keeps_its_longest_pass_and_the_pixels_of_that_pass();
    test_a_page_outside_the_slots_lands_in_the_last_one();
    test_stats_reset_clears_all_and_counts_passes();
    return test_report("test_loop_stats");
}
