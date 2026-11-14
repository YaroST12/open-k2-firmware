// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home strips: three history tiers (10 s, 60 s, 10 min), window cycling and per-strip auto-range.
#include "test_util.h"
#include "k2/ui/trend.hpp"

#include <cstdlib>

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
    CHECK_EQ(t.latest(Window::S60, Qty::V), 1011);          // mid ring (545 ms columns): the 1100 ms sample
    CHECK_EQ(t.latest(Window::S60, Qty::A), 61);
    CHECK_EQ(t.latest(Window::M10, Qty::V), 1011);          // slow ring still empty: the mid ring stands in
    Trend f;
    f.push(0, 900, 40, 0);
    CHECK_EQ(f.latest(Window::S60, Qty::V), 900);            // mid ring filled by the first push too
}

// ---- the Graphs page's history (110 points per tier, the contract-max ring, the Wh ring) ----------------------------------

// Every tier holds 110 points, one per Graphs column: 91 ms, 545 ms and 5455 ms apart, so a full ring spans the window
// (10 s, 60 s, 10 min). V carries the push time here (ms / 10), so a column's value says when it was taken.
static void test_tiers_hold_110_points_over_the_window() {
    Trend t;
    uint32_t ms = 0;
    for (; ms <= 15000; ms += 20) t.push(ms, ms / 10, 0, 0);
    ms -= 20;
    uint16_t c[110];
    CHECK_EQ(t.columns(Window::S10, Qty::V, c, 110), 110);
    const int newest = static_cast<int>(c[109]) * 10;     // the last column due: at most one period old
    CHECK_EQ(newest <= static_cast<int>(ms) && newest > static_cast<int>(ms) - 91, true);
    CHECK_EQ(std::abs(static_cast<int>(c[0]) * 10 - (newest - 109 * 91)) <= 20, true);   // 109 periods back, give or take a tick
    for (; ms <= 70000; ms += 20) t.push(ms, ms / 10, 0, 0);
    ms -= 20;
    CHECK_EQ(t.columns(Window::S60, Qty::V, c, 110), 110);
    CHECK_EQ(std::abs(static_cast<int>(c[0]) * 10 - (static_cast<int>(c[109]) * 10 - 109 * 545)) <= 20, true);
}

// seq() counts the columns a tier took: the Graphs page scrolls when it moves. The first push starts the fast tier.
static void test_seq_counts_columns() {
    Trend t;
    CHECK_EQ(t.seq(Window::S10), 0u);
    for (uint32_t ms = 0; ms <= 9100; ms += 10) t.push(ms, 500, 0, 0);
    CHECK_EQ(t.seq(Window::S10), 101u);          // 0, 91, ..., 9100
    CHECK_EQ(t.seq(Window::S60), 16u);           // 545 .. 8720
    CHECK_EQ(t.seq(Window::M10), 1u);            // 5455
    t.clear();
    CHECK_EQ(t.seq(Window::S10), 0u);
}

// The contract's maximum power is kept per column, beside the data: a renegotiation steps it, columns without a sample
// read 0 (no limit).
static void test_limit_ring_follows_the_columns() {
    Trend t;
    uint32_t ms = 0;
    for (; ms < 5000; ms += 20) t.push(ms, 2000, 300, 6000, 100);
    for (; ms < 6000; ms += 20) t.push(ms, 1500, 300, 4500, 45);
    uint8_t lim[110];
    uint16_t c[110];
    const int have = t.columns(Window::S10, Qty::W, c, 110);
    CHECK_EQ(t.limits(Window::S10, lim, 110), have);
    CHECK_EQ(lim[109], 45);
    CHECK_EQ(lim[109 - 12], 100);                // a second back: before the renegotiation
    CHECK_EQ(lim[0], 0);                         // no sample yet in the oldest column
    CHECK_EQ(c[0], kNone);
    t.push(ms + 100, 1500, 300, 4500, 300);      // above 255 W: clamped, never wrapped
    CHECK_EQ(t.limits(Window::S10, lim, 110), have + 1);
    CHECK_EQ(lim[109], 255);
}

// The Wh ring takes a point every 10th column of its tier, so it spans ten windows (100 s, 10 min, 1 h 40 min) in the same
// 110 points. Points read relative to the oldest one shown, in the tier's unit (1, 10, 100 mWh).
static void test_energy_every_tenth_column() {
    CHECK_EQ(Trend::energy_unit_mwh(Window::S10), 1u);
    CHECK_EQ(Trend::energy_unit_mwh(Window::S60), 10u);
    CHECK_EQ(Trend::energy_unit_mwh(Window::M10), 100u);
    Trend t;
    uint16_t e[110];
    CHECK_EQ(t.energy(Window::S10, e, 110), 0);
    CHECK_EQ(e[109], kNone);
    // 36 W: 10 mWh per second, 1 mWh every 100 ms; the meter starts at 5 Wh
    for (uint32_t ms = 0; ms <= 20000; ms += 20) t.push(ms, 2000, 180, 3600, 36, 5000 + ms / 100);
    const uint32_t cols = t.seq(Window::S10);
    const int pts = t.energy(Window::S10, e, 110);
    CHECK_EQ(pts, static_cast<int>((cols + 9) / 10));   // the 1st, 11th, 21st ... column
    CHECK_EQ(e[110 - pts], 0);                          // the oldest shown point is the reference
    CHECK_EQ(e[109 - pts], kNone);
    // ten columns of 91 ms apart: 9.1 mWh per point
    const int step = e[109] - e[108];
    CHECK_EQ(step >= 8 && step <= 10, true);
    CHECK_EQ(std::abs(static_cast<int>(e[109]) - (pts - 1) * 91 / 10) <= 2, true);
}

// The ring stores 16-bit values that wrap, read as differences: a session far past 65.5 Wh, or a 100 Wh session on the
// 10 min tier, never overflows or steps backwards.
static void test_energy_survives_a_long_session() {
    Trend t;
    // 65.0 Wh on the meter, +4 mWh per 20 ms push (720 W): the fast tier's mWh unit wraps at 65.535 Wh inside the window
    uint32_t mwh = 65000;
    for (uint32_t ms = 0; ms <= 60000; ms += 20, mwh += 4) t.push(ms, 2000, 300, 6000, 100, mwh);
    uint16_t e[110];
    const int n = t.energy(Window::S10, e, 110);
    CHECK_EQ(n, 66);                                    // 60 s of 91 ms columns, every 10th
    for (int i = 110 - n + 1; i < 110; i++) CHECK_EQ(e[i] > e[i - 1], true);
    CHECK_EQ(std::abs(static_cast<int>(e[109]) - 65 * 182) <= 200, true);   // 65 points of 910 ms at 200 mWh/s
    // 100 Wh over 1 h 40 min (60 W) on the slow tier: 1000 units of 100 mWh across the window
    Trend s;
    for (uint32_t ms = 0; ms <= 6100000; ms += 100) s.push(ms, 2000, 300, 6000, 100, ms / 60);
    const int m = s.energy(Window::M10, e, 110);
    CHECK_EQ(m > 100, true);
    for (int i = 110 - m + 1; i < 110; i++) CHECK_EQ(e[i] >= e[i - 1], true);
    CHECK_EQ(std::abs(static_cast<int>(e[109]) - (m - 1) * 5455 * 10 / 60 / 100) <= 2, true);
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
    test_tiers_hold_110_points_over_the_window();
    test_seq_counts_columns();
    test_limit_ring_follows_the_columns();
    test_energy_every_tenth_column();
    test_energy_survives_a_long_session();
    return test_report("test_trend");
}
