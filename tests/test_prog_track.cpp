// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The voltage fold / track break finding on an upward programmable sweep (k2/pd/prog_track.hpp): the thresholds and their
// edges, the DNL zero step, a noisy sample, and the recorded sweeps (FH22 folds and breaks, the IQ4 and UGREEN sweeps are clean).
#include "test_util.h"
#include "k2/pd/prog_track.hpp"
#include <cstdio>
#include <vector>

using namespace k2;
using pd::Track;

struct Pt { int req, settled; };

// A recorded sweep: "requested_mv,settled_mv" rows, '#' lines are the provenance header.
static std::vector<Pt> load(const char* path) {
    std::vector<Pt> v;
    std::FILE* f = std::fopen(path, "r");
    CHECK_COND(f != nullptr);
    if (!f) return v;
    char line[96];
    while (std::fgets(line, sizeof line, f)) {
        int r = 0, s = 0;
        if (std::sscanf(line, "%d,%d", &r, &s) == 2) v.push_back({r, s});
    }
    std::fclose(f);
    return v;
}
// The first finding of a run over the points, every `every`-th point (the 100 mV record read as a coarser class).
static pd::TrackScan scan(const std::vector<Pt>& pts, unsigned every = 1) {
    pd::TrackScan s;
    for (std::size_t i = 0; i < pts.size(); i += every) s.feed(pts[i].req, pts[i].settled);
    return s;
}

// A clean step goes up by about the request. Equal and small negatives are the DNL -1 LSB case (a 20 mV request may give 0 mV);
// the fold margin is "more than 50 mV below the previous settled", so 50 itself passes and 51 does not.
static void test_fold_edges() {
    CHECK_EQ(int(pd::track_step(7000, 7003, 7020, 7023)), int(Track::None));    // +20, tracking
    CHECK_EQ(int(pd::track_step(7000, 7003, 7020, 7003)), int(Track::None));    // 0 mV: DNL -1 LSB
    CHECK_EQ(int(pd::track_step(7000, 7003, 7020, 6983)), int(Track::None));    // -20 mV: a zero step plus -1 LSB
    CHECK_EQ(int(pd::track_step(7000, 7003, 7020, 6953)), int(Track::None));    // -50 mV exactly: the margin, not past it
    CHECK_EQ(int(pd::track_step(7000, 7003, 7020, 6952)), int(Track::Fold));    // -51 mV
    CHECK_EQ(int(pd::track_step(19900, 19815, 20000, 19400)), int(Track::Fold));
}
// A noisy single sample (the monitor reaches +-13 mV; the settled value is a tail mean, so less): two of them in opposite
// directions are -26 mV at the worst, well inside the margin.
static void test_noise_is_not_a_fold() {
    CHECK_EQ(int(pd::track_step(7000, 7013, 7020, 7007)), int(Track::None));    // -6 mV
    CHECK_EQ(int(pd::track_step(7000, 7013, 7020, 6987)), int(Track::None));    // -26 mV between samples
    CHECK_EQ(int(pd::track_step(7000, 7013, 7020, 6987 - 20)), int(Track::None));   // a zero step as well: -46 mV
}
// The break: the offset (request - settled) grows by more than 150 mV between neighbours without a fold. At the 20 and 100 mV
// classes a fall-behind that big is already a fold, so a break needs 500 mV and up (a flat step is one). An offset that
// shrinks (the output catching up, a first point still settling) is never a finding.
static void test_break_edges() {
    CHECK_EQ(int(pd::track_step(10000, 9950, 10500, 10450)), int(Track::None));   // 500 up, tracking
    CHECK_EQ(int(pd::track_step(10000, 9950, 10500, 10300)), int(Track::None));   // 350 up: the offset moved by exactly 150
    CHECK_EQ(int(pd::track_step(10000, 9950, 10500, 10299)), int(Track::Break));  // 349 up: 151
    CHECK_EQ(int(pd::track_step(10000, 9950, 10500, 9950)), int(Track::Break));   // flat for a 500 mV request
    CHECK_EQ(int(pd::track_step(10000, 9950, 10100, 9951)), int(Track::None));    // 100 mV request, +1: offset grew by 99
    CHECK_EQ(int(pd::track_step(10000, 9950, 10100, 9900)), int(Track::None));    // -50: the fold margin edge, offset grew by 150
    CHECK_EQ(int(pd::track_step(10000, 9950, 10100, 10201)), int(Track::None));   // 251 up: it caught up, not a finding
    CHECK_EQ(int(pd::track_step(3300, 2900, 3400, 3340)), int(Track::None));      // a low first point 400 mV short, then tracking
}
// The scored-check form (PD Spec): behind > limit(step) is exactly "a finding", over every step size and move.
static void test_behind_and_limit_agree_with_track_step() {
    CHECK_EQ(pd::track_limit(20), 70);
    CHECK_EQ(pd::track_limit(100), 150);
    CHECK_EQ(pd::track_limit(500), 150);
    CHECK_EQ(pd::track_behind(19980, 19900, 20000, 19419), 501);   // the FH22 20 mV step: -481 mV
    CHECK_EQ(pd::track_behind(7000, 7003, 7020, 7023), 0);          // tracking
    CHECK_EQ(pd::track_behind(7000, 7003, 7100, 7500), -397);       // the output caught up
    for (int step : {20, 40, 100, 500, 1000, 2000})
        for (int delta = -2600; delta <= 2600; ++delta) {
            const bool finding = pd::track_behind(10000, 9950, 10000 + step, 9950 + delta) > pd::track_limit(step);
            CHECK_EQ(finding, pd::track_step(10000, 9950, 10000 + step, 9950 + delta) != Track::None);
        }
}
// Only an upward request is judged.
static void test_downward_is_skipped() {
    CHECK_EQ(int(pd::track_step(20000, 19400, 19900, 19815)), int(Track::None));
    CHECK_EQ(int(pd::track_step(20000, 19400, 20000, 19000)), int(Track::None));   // the same request again
}

static void test_scan_keeps_the_first() {
    pd::TrackScan s;
    CHECK_COND(!s.found());
    s.feed(100, 100);                       // the first point only primes
    s.feed(200, 200);
    CHECK_COND(!s.found());
    s.feed(300, 100);                       // a fold: 200 -> 100
    s.feed(400, 400);
    s.feed(500, 100);                       // a second one is not reported
    CHECK_COND(s.found());
    CHECK_EQ(int(s.kind()), int(Track::Fold));
    CHECK_EQ(s.req_mv(), 300);
    CHECK_EQ(s.from_mv(), 200);
    CHECK_EQ(s.to_mv(), 100);
    CHECK_EQ(s.step_mv(), 100);
    s.clear();
    CHECK_COND(!s.found());
    s.feed(500, 500);                       // after clear() the next point primes again: no finding from 100 -> 500 history
    CHECK_COND(!s.found());
}

// FH22 (recorded 2026-11-15): tracks within 95 mV below to 19.9 V, then every request >= 20.0 V folds to 19.4 V.
static void test_fh22_pps_100mv_folds_at_20v() {
    const auto pts = load("tests/fixtures/sweep/fh22_pps_5000_21000.csv");
    CHECK_EQ(int(pts.size()), 161);
    const auto s = scan(pts);
    CHECK_COND(s.found());
    CHECK_EQ(int(s.kind()), int(Track::Fold));
    CHECK_EQ(s.req_mv(), 20000);
    CHECK_EQ(s.from_mv(), 19815);
    CHECK_EQ(s.to_mv(), 19400);
    CHECK_EQ(s.step_mv(), 100);
    // The same sweep up to 19.9 V alone is clean.
    const std::vector<Pt> below(pts.begin(), pts.begin() + 150);
    CHECK_EQ(below.back().req, 19900);
    CHECK_COND(!scan(below).found());
}
// The 20 mV class: the logged worst step of the PD Spec run, -481 mV between 19.98 V (settled 19.90 V) and 20.00 V (19.419 V).
static void test_fh22_pps_20mv_fold() {
    const std::vector<Pt> pts = {{19900, 19820}, {19920, 19840}, {19940, 19860}, {19960, 19880}, {19980, 19900}, {20000, 19419}, {20020, 19422}};
    const auto s = scan(pts);
    CHECK_EQ(int(s.kind()), int(Track::Fold));
    CHECK_EQ(s.req_mv(), 20000);
    CHECK_EQ(s.from_mv(), 19900);
    CHECK_EQ(s.to_mv(), 19419);
    CHECK_EQ(s.step_mv(), 20);
}
// The coarse classes read from the same record: at 500 and 1000 mV the folded level is not below the previous one by 50 mV
// (19.41 V -> 19.40 V, 18.9 V -> 19.4 V), but the offset jumps: a break at 20.0 V.
static void test_fh22_pps_coarse_classes_break_at_20v() {
    const auto pts = load("tests/fixtures/sweep/fh22_pps_5000_21000.csv");
    const auto s5 = scan(pts, 5);     // 5.0 5.5 ... 20.0
    CHECK_EQ(int(s5.kind()), int(Track::Break));
    CHECK_EQ(s5.req_mv(), 20000);
    CHECK_EQ(s5.from_mv(), 19410);
    CHECK_EQ(s5.to_mv(), 19400);
    CHECK_EQ(s5.step_mv(), 500);
    const auto s10 = scan(pts, 10);   // 5.0 6.0 ... 21.0
    CHECK_EQ(int(s10.kind()), int(Track::Break));
    CHECK_EQ(s10.req_mv(), 20000);
    CHECK_EQ(s10.step_mv(), 1000);
}
// The FH22 AVS sweep folds the same way at its last point: 20.0 V is the fixed-20 V setpoint (19.42 V). The points below it are clean.
static void test_fh22_avs_folds_at_20v() {
    const auto fh = load("tests/fixtures/sweep/fh22_avs_9000_20000.csv");
    CHECK_EQ(int(fh.size()), 111);
    const auto s = scan(fh);
    CHECK_EQ(int(s.kind()), int(Track::Fold));
    CHECK_EQ(s.req_mv(), 20000);
    CHECK_EQ(s.from_mv(), 19810);
    CHECK_EQ(s.to_mv(), 19423);
    CHECK_COND(!scan(std::vector<Pt>(fh.begin(), fh.end() - 1)).found());
}
// A clean source never flags, at any step class: the UGREEN AVS sweep (9-20 V, within 70 mV of the request throughout).
static void test_clean_sweeps_do_not_flag() {
    const auto ug = load("tests/fixtures/sweep/ugreen_avs_9000_20000.csv");
    CHECK_EQ(int(ug.size()), 111);
    CHECK_COND(!scan(ug).found());
    CHECK_COND(!scan(ug, 5).found());
    CHECK_COND(!scan(ug, 10).found());
}

int main() {
    test_fold_edges();
    test_noise_is_not_a_fold();
    test_break_edges();
    test_downward_is_skipped();
    test_behind_and_limit_agree_with_track_step();
    test_scan_keeps_the_first();
    test_fh22_pps_100mv_folds_at_20v();
    test_fh22_pps_20mv_fold();
    test_fh22_pps_coarse_classes_break_at_20v();
    test_fh22_avs_folds_at_20v();
    test_clean_sweeps_do_not_flag();
    return test_report("test_prog_track");
}
