// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home strips: three history tiers (10 s, 60 s, 10 min), window cycling and per-strip auto-range.
#include "test_util.h"
#include "k2/ui/trend.hpp"

using namespace k2::trend;

static void test_window_cycle() {
    CHECK_EQ(static_cast<int>(next(Window::S10)), static_cast<int>(Window::S60));
    CHECK_EQ(static_cast<int>(next(Window::S60)), static_cast<int>(Window::M10));
    CHECK_EQ(static_cast<int>(next(Window::M10)), static_cast<int>(Window::S10));
    CHECK_STR(label(Window::S10), "10s");
    CHECK_STR(label(Window::S60), "60s");
    CHECK_STR(label(Window::M10), "10m");
}

static void test_empty_is_all_none() {
    Trend t;
    uint16_t c[8];
    CHECK_EQ(t.columns(Window::S60, Qty::V, c, 8), 0);
    for (uint16_t v : c) CHECK_EQ(v, kNone);
}

static void test_fast_window_samples_every_100ms() {
    Trend t;
    for (uint32_t ms = 0; ms < 3000; ms += 100) t.push(ms, 2000 + ms / 100, 100, 5000);   // V ramps 1 step per push
    uint16_t c[10];
    // 30 samples seen; 10 columns over 10 s: the newest column is the newest sample, the oldest 7 s back
    CHECK_EQ(t.columns(Window::S10, Qty::V, c, 10), 3);   // columns holding data: 3 s of the 10 s window
    CHECK_EQ(c[9], 2000 + 29);
    CHECK_EQ(c[0], kNone);
    CHECK_EQ(c[6], kNone);
    CHECK_EQ(c[7] != kNone, 1);
}

static void test_push_rate_limits() {
    Trend t;
    t.push(0, 1000, 0, 0);
    t.push(40, 3000, 0, 0);      // inside 100 ms: ignored for the fast tier
    uint16_t c[100];
    t.columns(Window::S10, Qty::V, c, 100);
    CHECK_EQ(c[99], 1000);
}

static void test_mid_and_slow_tiers() {
    Trend t;
    for (uint32_t ms = 0; ms <= 125000; ms += 100) t.push(ms, 1000, 100, 100);
    uint16_t c[60];
    CHECK_EQ(t.columns(Window::S60, Qty::A, c, 60), 60);     // 60 s of 1 Hz history, full
    CHECK_EQ(c[0], 100);
    // slow tier: 10 s buckets, 12 full buckets by 125 s
    int n = t.columns(Window::M10, Qty::W, c, 60);
    CHECK_EQ(n > 0 && n <= 13, 1);
    CHECK_EQ(c[59], 100);
}

static void test_slow_tier_averages_its_bucket() {
    Trend t;
    // alternate 1000 and 3000 every 100 ms for 10.1 s: the bucket average is 2000
    for (uint32_t ms = 0; ms <= 10100; ms += 100) t.push(ms, (ms / 100) % 2 ? 3000 : 1000, 0, 0);
    uint16_t c[60];
    t.columns(Window::M10, Qty::V, c, 60);
    CHECK_EQ(c[59] >= 1900 && c[59] <= 2100, 1);
}

static void test_clamps_value() {
    Trend t;
    t.push(0, 4000000000u, 0, 0);
    uint16_t c[4];
    t.columns(Window::S10, Qty::V, c, 4);
    CHECK_EQ(c[3], 0xFFFE);
}

static void test_range_flat_signal_keeps_minimum_span() {
    uint16_t c[5] = {2000, 2001, 2000, 1999, 2000};
    Range r = range(c, 5, Qty::V);
    CHECK_EQ(r.valid, 1);
    CHECK_EQ(r.dmin, 1999);
    CHECK_EQ(r.dmax, 2001);
    CHECK_EQ(r.hi - r.lo, 100);                     // 1 V floor: noise stays flat
    CHECK_EQ(level(2000, r, 28), 13);              // centred
    CHECK_EQ(level(r.lo, r, 28), 0);
    CHECK_EQ(level(r.hi, r, 28), 27);
}

static void test_range_follows_each_strip() {
    uint16_t c[4] = {500, 800, 2000, kNone};       // 5 V .. 20 V
    Range r = range(c, 4, Qty::V);
    CHECK_EQ(r.dmin, 500);
    CHECK_EQ(r.dmax, 2000);
    CHECK_EQ(r.lo, 500);
    CHECK_EQ(r.hi, 2000);
    CHECK_EQ(level(800, r, 31), 6);                // (300*30)/1500
}

static void test_range_empty_and_floor_at_zero() {
    uint16_t c[3] = {kNone, kNone, kNone};
    CHECK_EQ(range(c, 3, Qty::W).valid, 0);
    uint16_t d[2] = {2, 3};                        // near 0: the padded window must not go negative
    Range r = range(d, 2, Qty::W);
    CHECK_EQ(r.lo, 0);
    CHECK_EQ(r.hi - r.lo, 100);
    CHECK_EQ(level(kNone, r, 28), 0);
}

// The Home sweep takes one new sample per step: the newest of the window's own ring, falling back to
// the faster rings while the slower one has not collected its first sample yet.
static void test_latest_per_window() {
    Trend t;
    CHECK_EQ(t.latest(Window::S10, Qty::V), kNone);
    CHECK_EQ(t.latest(Window::M10, Qty::A), kNone);
    for (int i = 0; i < 12; i++) t.push(i * 100u, 1000 + i, 50 + i, 0);   // 0 .. 1100 ms
    CHECK_EQ(t.latest(Window::S10, Qty::V), 1011);          // fast ring: newest 100 ms sample
    CHECK_EQ(t.latest(Window::S60, Qty::V), 1010);          // mid ring: the 1000 ms sample
    CHECK_EQ(t.latest(Window::S60, Qty::A), 60);
    CHECK_EQ(t.latest(Window::M10, Qty::V), 1010);          // slow ring still empty: the mid ring stands in
    Trend f;
    f.push(0, 900, 40, 0);
    CHECK_EQ(f.latest(Window::S60, Qty::V), 900);            // mid ring filled by the first push too
}

int main() {
    test_latest_per_window();
    test_window_cycle();
    test_empty_is_all_none();
    test_fast_window_samples_every_100ms();
    test_push_rate_limits();
    test_mid_and_slow_tiers();
    test_slow_tier_averages_its_bucket();
    test_clamps_value();
    test_range_flat_signal_keeps_minimum_span();
    test_range_follows_each_strip();
    test_range_empty_and_floor_at_zero();
    return test_report("test_trend");
}
