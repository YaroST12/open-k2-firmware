// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The deep-sweep engine (k2/chargertest/sweep.hpp): the point plan per range kind, the point current, the total
// point count for the Tests-list line, and the summary of the recorded points - including the FH22 fold-back shape
// (a steady out-of-band point is recorded, not a stop; the summary names the peak and the fold-back).
#include "test_util.h"
#include "k2/chargertest/sweep.hpp"
#include <string>
#include <vector>

using namespace k2;
namespace sw = k2::ctsweep;

// A PPS (SPR augmented) APDO raw word: max_mv in 100 mV at bits 17-24, min_mv in 100 mV at bits 8-15, current in 50 mA at bits 0-6.
static pd::Pdo pps_apdo(unsigned min_mv, unsigned max_mv, unsigned max_ma) {
    return pd::decode_pdo(0xc0000000u | ((max_mv / 100u) << 17) | ((min_mv / 100u) << 8) | (max_ma / 50u));
}
static pd::SourceCaps caps_with(std::initializer_list<pd::Pdo> pdos) {
    pd::SourceCaps c;
    c.valid = true; c.spec_rev = 2; c.active_cc = 1;
    unsigned n = 0;
    for (const auto& p : pdos) { if (n >= c.pdos.size()) break; c.pdos[n++] = p; }
    c.num_pdos = uint8_t(n);
    return c;
}

static void test_point_plan_covers_the_apdo_in_100mv_steps() {
    const auto r = sw::point_range(3300, 11000, 36000);
    CHECK_EQ(r.lo, 3300u); CHECK_EQ(r.hi, 11000u);
    CHECK_EQ(sw::point_count(r), 78u);            // ~77 points for 3.3-11 V
    CHECK_EQ(sw::point_mv(r, 0), 3300u);
    CHECK_EQ(sw::point_mv(r, sw::point_count(r) - 1), 11000u);
    const auto wide = sw::point_range(3300, 21000, 36000);
    CHECK_EQ(sw::point_count(wide), 178u);        // ~180 points for 3.3-21 V
    // the monitor ceiling caps the top: a range above it stops at the ceiling
    const auto capped = sw::point_range(5000, 48000, 36000);
    CHECK_EQ(capped.hi, 36000u);
    CHECK_EQ(sw::point_count(capped), 311u);
    // nothing fits under the cap / under the minimum
    CHECK_EQ(sw::point_count(sw::point_range(40000, 48000, 36000)), 0u);
    CHECK_EQ(sw::point_range(3300, 11000, 36000).empty(), false);
    // boundaries land on the 100 mV grid even off it
    const auto off = sw::point_range(3350, 10960, 36000);
    CHECK_EQ(off.lo, 3400u); CHECK_EQ(off.hi, 10900u);
    CHECK_EQ(sw::point_count(off), 76u);
}

static void test_point_current_is_1a_never_above_the_offer_or_cable() {
    const auto five_a = pps_apdo(3300, 21000, 5000);
    CHECK_EQ(sw::point_ma(five_a, 11000, 3000), 1000u);     // 1 A: the offer (5 A) and the cable (3 A) allow it
    CHECK_EQ(sw::point_ma(five_a, 11000, 5000), 1000u);
    const auto weak = pps_apdo(3300, 11000, 500);
    CHECK_EQ(sw::point_ma(weak, 5000, 3000), 500u);         // never above the APDO current
    const auto thin_cable = pps_apdo(3300, 11000, 5000);
    CHECK_EQ(sw::point_ma(thin_cable, 5000, 300), 300u);    // never above the cable limit
}

static void test_plan_total_counts_every_pps_apdo_within_the_budget() {
    const auto caps = caps_with({pd::decode_pdo((100u << 10) | 300), pps_apdo(3300, 11000, 5000), pps_apdo(5000, 21000, 3200)});
    // slot 1 is fixed (not swept): 78 + 161 points
    CHECK_EQ(sw::plan_total(caps, 36000), 239u);
    // the ceiling binds: 3.3-21 V capped at 20 V reads 151 points
    CHECK_EQ(sw::plan_total(caps, 20000), 78u + 151u);
    // no list / no PPS: no sweep
    CHECK_EQ(sw::plan_total(pd::SourceCaps{}, 36000), 0u);
    const auto no_pps = caps_with({pd::decode_pdo((100u << 10) | 300)});
    CHECK_EQ(sw::plan_total(no_pps, 36000), 0u);
    // the record budget caps the total: seven 3.3-21 V APDOs would be 7*178, the plan stops at kMaxPoints
    pd::SourceCaps big; big.valid = true; big.spec_rev = 2;
    big.num_pdos = 7;
    for (unsigned i = 0; i < 7; ++i) big.pdos[i] = pps_apdo(3300, 21000, 5000);
    CHECK_EQ(sw::plan_total(big, 36000), sw::kMaxPoints);
}

static void test_summary_of_a_clean_track_names_peak_and_errors() {
    std::vector<int16_t> err;
    for (unsigned set = 5000; set <= 21000; set += 100) err.push_back(int16_t(set - 150 - int(set)));   // -150 mV throughout
    const auto s = sw::summarize(err, 5000);
    CHECK_EQ(s.points, 161u);
    CHECK_EQ(s.min_err, -150); CHECK_EQ(s.max_err, -150); CHECK_EQ(s.worst_err, -150);
    CHECK_EQ(s.worst_mv, 5000u);                       // the first of the tied worst points
    CHECK_EQ(s.peak_mv, 20850u); CHECK_EQ(s.peak_at_mv, 21000u);
    CHECK_EQ(s.fold_mv, 0u);                           // VBUS never fell while the request rose
    CHECK_EQ(s.out_of_band, false);                    // -150 mV is inside the 5 % band
    char buf[64];
    const size_t n = sw::format_summary(s, buf);
    CHECK_STR(std::string(buf, n).c_str(), "peak 20.85 V at 21.0 V");
}

// The FH22 shape (runs/2026-11-10-merge2-hw/fh22-sweep.tsv, pattern in docs/CHARGER_QUIRKS.md): tracks about -150 mV
// up to 19.75 V delivered at the 19.9 V request, then every request >= 20.0 V folds back to 19.42 V. The sweep does
// NOT stop at the first out-of-band point: all 161 points are recorded, the summary names the peak and the fold-back.
static void test_fh22_fold_back_is_recorded_not_a_stop() {
    std::vector<int16_t> err;
    const unsigned tail[] = {19664, 19503, 19423, 19422, 19421, 19421, 19420, 19419, 19420, 19420, 19421};
    unsigned ti = 0;
    for (unsigned set = 5000; set <= 21000; set += 100) {
        const int measured = set <= 19900 ? int(set) - 150 : int(tail[ti++]);
        err.push_back(int16_t(measured - int(set)));
    }
    CHECK_EQ(err.size(), 161u);
    const auto s = sw::summarize(err, 5000);
    CHECK_EQ(s.points, 161u);                          // recorded to the top: no stop at 20.0 V
    CHECK_EQ(s.peak_mv, 19750u); CHECK_EQ(s.peak_at_mv, 19900u);
    CHECK_EQ(s.fold_from_mv, 20000u);                  // the first falling point (19664 < 19750)
    CHECK_EQ(s.fold_mv, 19419u);                       // the level it folds back to
    CHECK_EQ(s.out_of_band, true);                     // the tail sits below the band edge, steady
    CHECK_EQ(s.worst_err, -1579); CHECK_EQ(s.worst_mv, 21000u);   // 19421 - 21000
    CHECK_EQ(s.min_err, -1579); CHECK_EQ(s.max_err, -150);
    char buf[64];
    const size_t n = sw::format_summary(s, buf);
    CHECK_STR(std::string(buf, n).c_str(), "peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V");
}

static void test_summary_of_nothing_and_format_mv() {
    const auto s = sw::summarize({}, 5000);
    CHECK_EQ(s.points, 0u); CHECK_EQ(s.peak_mv, 0u);
    char buf[64];
    CHECK_STR(std::string(buf, sw::format_summary(s, buf)).c_str(), "no points");
    char v[12];
    CHECK_STR(std::string(v, sw::format_mv(19748, v)).c_str(), "19.75");
    CHECK_STR(std::string(v, sw::format_mv(19900, v)).c_str(), "19.9");
    CHECK_STR(std::string(v, sw::format_mv(20000, v)).c_str(), "20.0");
    CHECK_STR(std::string(v, sw::format_mv(19419, v)).c_str(), "19.42");
    CHECK_STR(std::string(v, sw::format_mv(5000, v)).c_str(), "5.0");
    // a rising track with a single dip reports the fold-back from that point
    std::vector<int16_t> err{0, 0, -500, 0, 0};   // setpoints 5000..5400, measured dips at 5200
    const auto d = sw::summarize(err, 5000);
    CHECK_EQ(d.fold_from_mv, 5200u); CHECK_EQ(d.fold_mv, 4700u); CHECK_EQ(d.peak_mv, 5400u);
}

int main() {
    test_point_plan_covers_the_apdo_in_100mv_steps();
    test_point_current_is_1a_never_above_the_offer_or_cable();
    test_plan_total_counts_every_pps_apdo_within_the_budget();
    test_summary_of_a_clean_track_names_peak_and_errors();
    test_fh22_fold_back_is_recorded_not_a_stop();
    test_summary_of_nothing_and_format_mv();
    return test_report("test_sweep");
}
