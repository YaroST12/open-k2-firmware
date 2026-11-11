// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The D+/D- EXTI edge-storm guard (k2/fastcharge/edge_guard.hpp): a USB data link through the K2 toggles D+/D- at MHz
// rates, and one interrupt per edge would starve the main loop. The guard trips within a fixed number of interrupts on
// such a train, never on the VOOC traffic of the two cap1 slices, and the monitor backs off 1 s, 5 s, 30 s (cap).
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "k2/fastcharge/edge_guard.hpp"
#include "k2/fastcharge/edges.hpp"
#include "test_util.h"

using namespace k2;
using edges::Backoff;
using edges::StormGuard;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

// Every recorded word is one interrupt (the recorder keeps the repeats the two lines' race makes).
std::vector<std::uint32_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> w;
    for (std::size_t i = 0; i + 6 <= b.size();) {
        const std::size_t n = b[i];
        if (n < 6 || i + n > b.size()) break;
        if (b[i + 1] == 15) {
            edges::Edge e[64];
            std::uint8_t idx = 0;
            const std::size_t k = edges::decode_chunk({b.data() + i + 6, n - 6}, e, idx);
            for (std::size_t j = 0; j < k; ++j) w.push_back((e[j].units << 6) | e[j].state);
        }
        i += n;
    }
    return w;
}

// Interrupts every `spacing` cycles from `start` until the guard trips; returns how many it took (0 = never, within `max`).
std::uint32_t storm(StormGuard& g, std::uint32_t start, std::uint32_t spacing, std::uint32_t max) {
    std::uint32_t t = start;
    for (std::uint32_t n = 1; n <= max; ++n, t += spacing)
        if (g.hit(t)) return n;
    return 0;
}

}  // namespace

// A full-speed USB train (12 Mbit/s: an edge every 20 cycles at 240 MHz, faster than the ISR can run, so in practice the
// ISR runs back to back at its own length) trips the short window within kStormWorstIrqs interrupts, whatever the phase
// of the window when the traffic starts: at most kStormShortMax in the window that was running plus kStormShortMax + 1
// in the next.
static void test_usb_train_trips_within_the_bound() {
    CHECK_EQ(edges::kStormWorstIrqs, 65u);
    std::uint32_t worst = 0;
    for (const std::uint32_t spacing : {20u, 150u, 600u}) {
        for (std::uint32_t off = 0; off < edges::kStormShortCycles; off += 997) {
            StormGuard g;
            g.reset(1000);
            const std::uint32_t n = storm(g, 1000 + off, spacing, 10000);
            CHECK(n > edges::kStormShortMax);
            CHECK(n <= edges::kStormWorstIrqs);
            if (n > worst) worst = n;
            CHECK(g.last().edges > edges::kStormShortMax);
            CHECK(g.last().edges <= edges::kStormShortMax + 1);
            CHECK(g.last().cycles < edges::kStormShortCycles);
            CHECK_EQ(g.last().ms(), 1u);
        }
    }
    CHECK_EQ(worst, edges::kStormWorstIrqs);
    std::printf("usb train: worst %u interrupts before the lines are masked\n", unsigned(worst));
    // Once tripped it stays tripped until reset (the ISR masks its lines on the first true).
    StormGuard g;
    g.reset(0);
    CHECK(storm(g, 0, 20, 100) != 0);
    CHECK(g.hit(5000));
    g.reset(10000);
    CHECK(!g.hit(10020));
}

// Lighter data traffic that never fills the short window (start-of-frame packets: a burst of interrupts every 1 ms) is
// caught by the long window: more than kStormLongMax interrupts in one 2^28-cycle window (1,118 ms).
static void test_sof_like_traffic_trips_the_long_window() {
    StormGuard g;
    g.reset(0);
    std::uint32_t t = 0, n = 0;
    bool tripped = false;
    for (int ms = 0; ms < 3000 && !tripped; ++ms) {
        for (int k = 0; k < 12 && !tripped; ++k) { ++n; tripped = g.hit(t + std::uint32_t(k) * 150u); }
        t += edges::kGuardCyclesPerMs;
    }
    CHECK(tripped);
    CHECK(n <= 2 * edges::kStormLongMax + 1);
    CHECK_EQ(g.last().edges, edges::kStormLongMax + 1);
    CHECK(g.last().ms() >= 340 && g.last().ms() <= 1119);
    std::printf("sof-like 12 interrupts/ms: tripped after %u interrupts, %u in %u ms\n", unsigned(n),
                unsigned(g.last().edges), unsigned(g.last().ms()));
}

// The VOOC traffic of cap1 never trips it: the phone's wake burst (40 clocks on both lines), the handshake, the steady
// 0x82 poll, the unplug and the second session. Peak in the full capture: 9 interrupts in any 1 ms, 1,340 in any 1 s.
static void test_vooc_captures_do_not_trip() {
    for (const char* path : {"tests/fixtures/op15_svooc_entry.edges", "tests/fixtures/op15_svooc_replug.edges"}) {
        const auto w = load(path);
        CHECK(w.size() > 2000);
        StormGuard g;
        g.reset(w.front() & ~63u);
        std::uint32_t t = w.front() & ~63u;
        int trips = 0;
        for (const std::uint32_t x : w) {
            const std::uint32_t at = x & ~63u;
            while (at - t > edges::kGuardCyclesPerMs) { t += edges::kGuardCyclesPerMs; g.tick(t); }
            trips += g.hit(at) ? 1 : 0;
            t = at;
        }
        CHECK_EQ(trips, 0);
    }
    // The phone's wake burst twice in a row (80 steps of 300 us, D+ and D- 2 us apart: 320 interrupts in 48 ms).
    StormGuard g;
    g.reset(0);
    std::uint32_t t = 0;
    int trips = 0;
    for (int i = 0; i < 160; ++i) {
        trips += g.hit(t) ? 1 : 0;
        trips += g.hit(t + 480) ? 1 : 0;
        t += 300 * 240;
    }
    CHECK_EQ(trips, 0);
}

// The main loop's tick closes windows whose time has passed, so a count left over from a window 2^32 cycles (17.9 s)
// ago cannot alias into a new one.
static void test_long_silence_does_not_alias() {
    StormGuard g;
    g.reset(0);
    std::uint32_t t = 0;
    for (std::uint32_t i = 0; i < 4000; ++i, t += 30000) CHECK(!g.hit(t));
    for (int ms = 0; ms < 18000; ++ms) { t += edges::kGuardCyclesPerMs; g.tick(t); }
    const std::uint32_t again = 0u + 4000u * 30000u + 18000u * edges::kGuardCyclesPerMs;
    CHECK_EQ(t, again);
    int trips = 0;
    for (std::uint32_t i = 0; i < 200; ++i) trips += g.hit(t + i * 30000) ? 1 : 0;
    CHECK_EQ(trips, 0);
}

// The monitor's back-off: paused 1 s, then a probe; a storm in the probe pauses 5 s, then 30 s, and 30 s from then on.
static void test_backoff_sequence() {
    Backoff b;
    CHECK(b.state() == Backoff::State::Run);
    CHECK(!b.paused());
    CHECK_EQ(b.trip(100), 1000u);
    CHECK(b.paused());
    CHECK_EQ(b.retry_in(600), 500u);
    CHECK(!b.poll(1099));
    CHECK(b.poll(1100));                     // the pause is over: re-enable the lines for a probe
    CHECK(b.state() == Backoff::State::Probe);
    CHECK(!b.paused());
    CHECK(!b.poll(1200));                    // reported once
    CHECK_EQ(b.trip(1300), 5000u);           // traffic again in the probe
    CHECK(b.poll(6300));
    CHECK_EQ(b.trip(6400), 30000u);
    CHECK(b.poll(36400));
    CHECK_EQ(b.trip(36500), 30000u);         // the cap
    CHECK_EQ(b.storms(), 4u);
    CHECK_EQ(b.retry_in(36500), 30000u);
    b.reset();                               // the monitor stopped (unplug): a new device starts from 1 s
    CHECK(b.state() == Backoff::State::Run);
    CHECK_EQ(b.trip(0), 1000u);
}

// Quiet lines: a probe that passes kProbeMs without a storm runs the monitor again; after kCalmMs of running the next
// storm starts from 1 s again, before that it keeps growing.
static void test_quiet_lines_reenable() {
    Backoff b;
    CHECK_EQ(b.trip(0), 1000u);
    CHECK(b.poll(1000));
    CHECK(!b.poll(1000 + edges::kProbeMs - 1));
    CHECK(b.state() == Backoff::State::Probe);
    CHECK(!b.poll(1000 + edges::kProbeMs));
    CHECK(b.state() == Backoff::State::Run);
    CHECK_EQ(b.trip(10000), 5000u);          // a storm soon after: still growing
    CHECK(b.poll(15000));
    b.poll(15000 + edges::kProbeMs);
    CHECK(b.state() == Backoff::State::Run);
    b.poll(15000 + edges::kProbeMs + edges::kCalmMs);
    CHECK_EQ(b.trip(15000 + edges::kProbeMs + edges::kCalmMs + 1), 1000u);   // calm long enough: back to 1 s
    CHECK_EQ(b.storms(), 3u);
}

int main() {
    test_usb_train_trips_within_the_bound();
    test_sof_like_traffic_trips_the_long_window();
    test_vooc_captures_do_not_trip();
    test_long_silence_does_not_alias();
    test_backoff_sequence();
    test_quiet_lines_reenable();
    return test_report("test_edge_guard");
}
