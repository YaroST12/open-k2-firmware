// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The blocked-paint meter (include/k2/core/gap_meter.hpp): the longest continuous time the charger-test page wanted to paint
// and could not, because the engine was inside a window the LCD idle hook does not serve. The page notes every tick.
#include "k2/core/gap_meter.hpp"
#include "test_util.h"

using namespace k2::gap;

static void test_a_window_runs_from_the_first_blocked_tick_to_the_first_free_one() {
    BlockMeter m;
    m.note(100, false); m.note(120, false);
    CHECK_EQ(m.peak(120), 0u);
    m.note(140, true); m.note(160, true); m.note(180, true);
    m.note(200, false);
    CHECK_EQ(m.peak(200), 60u);   // 140 .. 200
    m.note(220, false);
    CHECK_EQ(m.peak(220), 60u);   // a closed window stays
}

static void test_an_open_window_counts_up_to_now() {
    BlockMeter m;
    m.note(1000, true); m.note(1020, true);
    CHECK_EQ(m.peak(1020), 20u);
    CHECK_EQ(m.peak(2500), 1500u);   // the row ended while the page was still blocked: read at the row's end
}

static void test_the_longest_of_several_windows_wins() {
    BlockMeter m;
    m.note(0, true); m.note(100, false);       // 100
    m.note(200, true); m.note(500, false);     // 300
    m.note(600, true); m.note(650, false);     // 50
    CHECK_EQ(m.peak(700), 300u);
}

static void test_a_row_window_and_the_run_window_are_separate() {
    BlockMeter m;
    m.begin_run(0); m.begin_window(0);
    m.note(10, true); m.note(1410, false);       // row 1: 1400 ms
    CHECK_EQ(m.peak(1500), 1400u);
    CHECK_EQ(m.run_peak(1500), 1400u);
    m.begin_window(1500);                          // row 2 begins
    m.note(1600, true); m.note(1700, false);       // 100 ms
    CHECK_EQ(m.peak(1800), 100u);                  // the row reads its own worst
    CHECK_EQ(m.run_peak(1800), 1400u);             // the run keeps the worst of all rows
    m.begin_run(2000);                             // the next run starts clean
    CHECK_EQ(m.run_peak(2000), 0u);
}

static void test_a_window_open_at_a_row_start_counts_from_there() {
    BlockMeter m;
    m.note(0, true);
    m.begin_window(500);   // the open window is the new row's from now on
    CHECK_EQ(m.peak(700), 200u);
}

static void test_a_wrapping_clock_still_measures() {
    BlockMeter m;
    m.note(0xFFFFFF00u, true);
    m.note(0x00000100u, false);
    CHECK_EQ(m.peak(0x200), 0x200u);
}

int main() {
    test_a_window_runs_from_the_first_blocked_tick_to_the_first_free_one();
    test_an_open_window_counts_up_to_now();
    test_the_longest_of_several_windows_wins();
    test_a_row_window_and_the_run_window_are_separate();
    test_a_window_open_at_a_row_start_counts_from_there();
    test_a_wrapping_clock_still_measures();
    return test_report("test_gap_meter");
}
