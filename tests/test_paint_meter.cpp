// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Paint cost counters (include/k2/core/paint_meter.hpp): pixels sent to the panel, core cycles inside the lcd stream calls
// and inside file-system flash writes, read as running totals and as a window that a charger-test row opens and closes.
#include "k2/core/paint_meter.hpp"
#include "test_util.h"

using namespace k2::paint;

static void test_cycles_to_ms() {
    // 240 MHz: 240000 cycles are one millisecond, the remainder is cut (the totals are kept in cycles, so nothing drifts).
    CHECK_EQ(ms_of(0), 0u);
    CHECK_EQ(ms_of(239'999), 0u);
    CHECK_EQ(ms_of(240'000), 1u);
    CHECK_EQ(ms_of(240'000ull * 5000 + 17), 5000u);
    // A 64-bit total never wraps inside a day of painting: 240e6 * 86400 = 2.07e13 cycles.
    CHECK_EQ(ms_of(240'000'000ull * 86400), 86'400'000u);
}

static void test_totals() {
    Meter m;
    CHECK_EQ(m.total().px, 0u);
    m.add_px(5723);
    m.add_px(1);
    m.add_lcd(480'000);
    m.add_fs(240'000);
    const Reading t = m.total();
    CHECK_EQ(t.px, 5724u);
    CHECK_EQ(t.paint_ms, 2u);
    CHECK_EQ(t.fs_ms, 1u);
}

static void test_no_truncation_drift() {
    // 1000 calls of 240 cycles (1 us each) are exactly one millisecond; per-call truncation would read 0.
    Meter m;
    for (int i = 0; i < 1000; i++) m.add_lcd(240);
    CHECK_EQ(m.total().paint_ms, 1u);
}

static void test_window() {
    Meter m;
    m.add_px(1000); m.add_lcd(240'000 * 7); m.add_fs(240'000 * 3);
    m.begin_window();
    CHECK_EQ(m.window().px, 0u);
    CHECK_EQ(m.window().paint_ms, 0u);
    m.add_px(250); m.add_lcd(240'000 * 2 + 100); m.add_fs(240'000);
    m.add_px(50);  m.add_lcd(240'000 * 2);
    const Reading w = m.window();
    CHECK_EQ(w.px, 300u);
    CHECK_EQ(w.paint_ms, 4u);     // 4.0004 ms
    CHECK_EQ(w.fs_ms, 1u);
    // The totals keep running and a new window starts from zero again.
    CHECK_EQ(m.total().px, 1300u);
    m.begin_window();
    CHECK_EQ(m.window().px, 0u);
    m.add_px(7);
    CHECK_EQ(m.window().px, 7u);
    CHECK_EQ(m.total().px, 1307u);
}

static void test_window_saturates_for_a_log_line() {
    // The log line carries 32-bit values; a window cannot overflow in practice, but the 64-bit to 32-bit step saturates.
    Meter m;
    m.begin_window();
    m.add_px(0xFFFFFFFFu);
    m.add_px(10);
    CHECK_EQ(m.window().px, 0xFFFFFFFFu);
}

int main() {
    test_cycles_to_ms();
    test_totals();
    test_no_truncation_drift();
    test_window();
    test_window_saturates_for_a_log_line();
    return test_report("test_paint_meter");
}
