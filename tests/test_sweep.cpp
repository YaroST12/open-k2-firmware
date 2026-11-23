// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The deep-sweep engine (k2/chargertest/sweep.hpp): the point plan per range kind, the point current, the total
// point count for the Tests-list line, and the summary of the recorded points - including the FH22 fold-back shape
// (a steady out-of-band point is recorded, not a stop; the summary names the peak and the fold-back).
#include "test_util.h"
#include "k2/chargertest/sweep.hpp"
#include <cstdio>
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
// An SPR AVS APDO (PD 3.2, nibble 0xE): min 9 V always; the 20 V form when the low 10 bits are non-zero (the current
// there in 10 mA units), else 15 V max with the current in bits 19:10.
static pd::Pdo spr_avs_apdo(unsigned ma_above15_10ma, unsigned ma_below15_10ma) {
    return pd::decode_pdo(0xE0000000u | (ma_below15_10ma << 10) | ma_above15_10ma);
}
// An EPR AVS APDO (nibble 0xD): min/max in 100 mV, PDP in watts (the current limit is PDP-derived).
static pd::Pdo epr_avs_apdo(unsigned min_mv, unsigned max_mv, unsigned pdp_w) {
    return pd::decode_pdo(0xD0000000u | ((max_mv / 100u) << 17) | ((min_mv / 100u) << 8) | pdp_w);
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

// The shared rule (k2/pd/prog_track.hpp) on the recorded FH22 sweeps (tests/fixtures/sweep): settled VBUS up to 95 mV low to 19.9 V,
// then 20.0 V gives 19.40 V - a fold. The AVS run folds at its last point; a clean source (UGREEN AVS) names nothing.
static std::vector<int16_t> fixture_err(const char* path, unsigned& lo_mv) {
    std::vector<int16_t> err;
    std::FILE* f = std::fopen(path, "r");
    CHECK_COND(f != nullptr);
    char line[96];
    lo_mv = 0;
    while (f && std::fgets(line, sizeof line, f)) {
        int r = 0, s = 0;
        if (std::sscanf(line, "%d,%d", &r, &s) != 2) continue;
        if (err.empty()) lo_mv = unsigned(r);
        err.push_back(int16_t(s - r));
    }
    if (f) std::fclose(f);
    return err;
}
static void test_recorded_sweeps_fold_or_stay_clean() {
    unsigned lo = 0;
    auto err = fixture_err("tests/fixtures/sweep/fh22_pps_5000_21000.csv", lo);
    CHECK_EQ(lo, 5000u); CHECK_EQ(err.size(), 161u);
    auto s = sw::summarize(err, lo);
    CHECK_EQ(s.fold_from_mv, 20000u); CHECK_EQ(s.fold_mv, 19400u);
    CHECK_EQ(s.peak_mv, 19815u); CHECK_EQ(s.peak_at_mv, 19900u);
    char buf[64];
    CHECK_STR(std::string(buf, sw::format_summary(s, buf)).c_str(), "peak 19.82 V at 19.9 V, folds back to 19.4 V from 20.0 V");
    // The same record split in two parts (the runner's two rows, joined at an arbitrary point) gives the same finding.
    const auto s2 = sw::summarize({err.data(), 40}, lo, {err.data() + 40, err.size() - 40}, lo + 40 * sw::kStepMv);
    CHECK_EQ(s2.fold_from_mv, 20000u); CHECK_EQ(s2.fold_mv, 19400u);
    err = fixture_err("tests/fixtures/sweep/fh22_avs_9000_20000.csv", lo);
    CHECK_EQ(lo, 9000u); CHECK_EQ(err.size(), 111u);
    s = sw::summarize(err, lo);
    CHECK_EQ(s.fold_from_mv, 20000u); CHECK_EQ(s.fold_mv, 19423u);
    err = fixture_err("tests/fixtures/sweep/ugreen_avs_9000_20000.csv", lo);
    s = sw::summarize(err, lo);
    CHECK_EQ(s.fold_mv, 0u); CHECK_EQ(s.fold_from_mv, 0u);
    CHECK_STR(std::string(buf, sw::format_summary(s, buf)).c_str(), "peak 20.07 V at 20.0 V");
}
// Summary::track_err_mv is the worst |err| before the first fold's onset, so a fold of any depth does not read as a tracking failure
// (the Results chip grades a caution by it); worst_err keeps the whole record. Without a fold it is the whole record's worst |err|.
static void test_track_err_stops_at_the_fold_onset() {
    auto s = sw::summarize(std::vector<int16_t>{0, 0, 0, 0, -400, -400, -400}, 5000);
    CHECK_EQ(s.fold_from_mv, 5400u);
    CHECK_EQ(s.track_err_mv, 0u); CHECK_EQ(s.worst_err, -400);          // tracked, then fell 400 mV: the fold is not tracking
    s = sw::summarize(std::vector<int16_t>{-250, -300, -250, -900, -900}, 5000);
    CHECK_EQ(s.fold_from_mv, 5300u);
    CHECK_EQ(s.track_err_mv, 300u); CHECK_EQ(s.worst_err, -900);        // the offset before the onset counts, the fold's depth does not
    s = sw::summarize(std::vector<int16_t>{-400, -400, -400}, 5000);
    CHECK_EQ(s.fold_mv, 0u);
    CHECK_EQ(s.track_err_mv, 400u);                                    // no fold: every point is tracking
    s = sw::summarize(std::vector<int16_t>{20, 450, 450}, 5000);
    CHECK_EQ(s.track_err_mv, 450u);                                    // an excess counts as much as a shortfall
    CHECK_EQ(sw::summarize({}, 5000).track_err_mv, 0u);
    // The recorded FH22 sweeps: tracks within 100 mV, then the fold (1.5 V deep) that used to turn the chip red.
    unsigned lo = 0;
    for (const char* f : {"tests/fixtures/sweep/fh22_pps_5000_21000.csv", "tests/fixtures/sweep/fh22_avs_9000_20000.csv"}) {
        const auto err = fixture_err(f, lo);
        s = sw::summarize(err, lo);
        CHECK_EQ(s.fold_from_mv, 20000u);
        CHECK_EQ(s.track_err_mv <= 100u, true);
        CHECK_EQ(unsigned(std::abs(s.worst_err)) > 300u, s.fold_mv < 19500u);   // the PPS fold is deep, the AVS one is 340 mV
    }
    // A clean source: no fold, the worst error of the record.
    const auto clean = fixture_err("tests/fixtures/sweep/ugreen_avs_9000_20000.csv", lo);
    s = sw::summarize(clean, lo);
    CHECK_EQ(s.track_err_mv, unsigned(std::abs(s.worst_err)));
    // A split sweep (low part recorded after the high one) is judged over the points in setpoint order, like the fold itself.
    const auto whole_err = fixture_err("tests/fixtures/sweep/fh22_pps_5000_21000.csv", lo);
    const auto merged = sw::summarize({whole_err.data(), 40}, lo, {whole_err.data() + 40, whole_err.size() - 40}, lo + 40 * sw::kStepMv);
    CHECK_EQ(merged.track_err_mv, sw::summarize(whole_err, lo).track_err_mv);
    // A fold that begins in the second part: the first part's points are all before the onset.
    const auto a = std::vector<int16_t>{-300, -300};
    const auto b = std::vector<int16_t>{0, 0, -900};
    const auto j = sw::summarize(a, 4800, b, 5000);   // 4.8 4.9 | 5.0 5.1 5.2 (measured 4500 4600 5000 5100 4300)
    CHECK_EQ(j.fold_from_mv, 5200u);
    CHECK_EQ(j.track_err_mv, 300u);
}
// The margin: a dip of up to 50 mV is meter noise and the DNL, not a fold (it used to flag every falling point).
static void test_a_small_dip_is_not_a_fold() {
    std::vector<int16_t> err{0, 0, -40, 0, 0};   // measured 5000 5100 5160 5300 5400: a 40 mV dip below the trend, still rising
    CHECK_EQ(sw::summarize(err, 5000).fold_mv, 0u);
    std::vector<int16_t> flat{0, 0, -100, 0, 0}; // measured 5000 5100 5100 5300: equal, not falling
    CHECK_EQ(sw::summarize(flat, 5000).fold_mv, 0u);
    std::vector<int16_t> dip{0, 0, -151, 0, 0};  // 5000 5100 5049 5300: 51 mV below the step before
    CHECK_EQ(sw::summarize(dip, 5000).fold_from_mv, 5200u);
}
// A jump ahead (VBUS above the request, the output catching up) names nothing: only a source that falls behind is a finding,
// and at 100 mV steps that is always a fold.
static void test_catching_up_is_not_a_finding() {
    CHECK_EQ(sw::summarize(std::vector<int16_t>{-400, -60, -60, -60, -60}, 5000).fold_mv, 0u);   // a first point 400 mV short, then tracking
    CHECK_EQ(sw::summarize(std::vector<int16_t>{0, 0, 300, 300, 300}, 5000).fold_mv, 0u);        // VBUS 300 mV above the request from 5.2 V
}

// ROADMAP "AVS deep sweep": the point plan per range kind. Kind::Avs sweeps the AVS APDOs: SPR AVS in slots 1..7
// (9-20 V every 100 mV = 111 points), EPR AVS in slots 8+ when the EPR list is in hand, capped at the monitor ceiling.
static void test_avs_plan_spr_only() {
    const auto caps = caps_with({pd::decode_pdo((100u << 10) | 300), spr_avs_apdo(200, 50)});   // SPR AVS 9-20 V, slot 2
    CHECK_EQ(sw::plan_total(caps, 36000, sw::Kind::Avs), 111u);       // 9.0..20.0 V every 100 mV
    CHECK_EQ(sw::plan_total(caps, 20000, sw::Kind::Avs), 111u);       // a 20 V cable still covers the 20 V top
    CHECK_EQ(sw::plan_total(caps, 36000, sw::Kind::Pps), 0u);         // no PPS APDO: the PPS sweep counts nothing
    CHECK_EQ(sw::plan_total(caps, 36000), 0u);                        // the default kind is Pps
    const auto avs15 = caps_with({spr_avs_apdo(0, 100)});              // SPR AVS 9-15 V form (low bits zero)
    CHECK_EQ(sw::plan_total(avs15, 36000, sw::Kind::Avs), 61u);        // 9.0..15.0 V
    // no AVS offered at all: the Tests-list entry is hidden (count 0)
    CHECK_EQ(sw::plan_total(caps_with({pd::decode_pdo((100u << 10) | 300), pps_apdo(3300, 21000, 5000)}), 36000, sw::Kind::Avs), 0u);
    CHECK_EQ(sw::plan_total(pd::SourceCaps{}, 36000, sw::Kind::Avs), 0u);
}
static void test_avs_plan_spr_plus_epr_capped_at_the_ceiling() {
    pd::SourceCaps caps = caps_with({pd::decode_pdo((100u << 10) | 300), spr_avs_apdo(200, 50)});
    caps.epr = true; caps.num_pdos = 10;
    caps.pdos[9] = epr_avs_apdo(15000, 48000, 140);                    // EPR AVS 15..48 V, PDP 140 W (slot 10)
    CHECK_EQ(sw::plan_total(caps, 36000, sw::Kind::Avs), 111u + 211u);   // the EPR part caps at the 36 V monitor: 15.0..36.0 V
    CHECK_EQ(sw::plan_total(caps, 48000, sw::Kind::Avs), 111u + 331u);   // a 48 V monitor sweeps to the APDO top
    // an EPR AVS APDO whose range starts above the cap contributes nothing (a 20 V cable, an APDO from 25 V)
    pd::SourceCaps hi = caps; hi.pdos[9] = epr_avs_apdo(25000, 48000, 140);
    CHECK_EQ(sw::plan_total(hi, 36000, sw::Kind::Avs), 111u + 111u);   // 25..36 V at the monitor ceiling
    CHECK_EQ(sw::plan_total(hi, 20000, sw::Kind::Avs), 111u);            // under a 20 V cap the EPR part is empty
    // the EPR slots are not swept from an SPR list
    pd::SourceCaps spr = caps; spr.epr = false; spr.num_pdos = 7;
    CHECK_EQ(sw::plan_total(spr, 36000, sw::Kind::Avs), 111u);
    // the shared record budget still caps the total
    pd::SourceCaps big; big.valid = true; big.spec_rev = 2; big.epr = true; big.num_pdos = 11;
    for (unsigned i = 0; i < 7; ++i) big.pdos[i] = spr_avs_apdo(200, 50);
    for (unsigned i = 7; i < 11; ++i) big.pdos[i] = epr_avs_apdo(15000, 48000, 140);
    CHECK_EQ(sw::plan_total(big, 36000, sw::Kind::Avs), sw::kMaxPoints);
    // the capped variant applies the monitor ceiling and the cable limit (no 21 V PPS bump for AVS)
    CHECK_EQ(sw::plan_total_capped(caps, 36000, 20000, sw::Kind::Avs), 111u + 51u);   // EPR part caps at the 20 V cable: 15..20 V
    CHECK_EQ(sw::plan_total_capped(caps, 36000, 50000, sw::Kind::Avs), 111u + 211u);
}
static void test_avs_point_current_never_above_the_pdp_limit_or_1a() {
    const auto epr = epr_avs_apdo(15000, 48000, 140);                  // PDP 140 W at 48 V -> 2916 mA
    CHECK_EQ(epr.max_ma, 2916u);
    CHECK_EQ(sw::point_ma(epr, 15000, 5000), 1000u);                   // the 1 A sweep cap binds
    CHECK_EQ(sw::point_ma(epr, 36000, 3000), 1000u);
    const auto low_pdp = epr_avs_apdo(15000, 45000, 45);               // PDP 45 W -> 1000 mA
    CHECK_EQ(sw::point_ma(low_pdp, 15000, 5000), 1000u);
    const auto tiny = epr_avs_apdo(15000, 45000, 20);                  // PDP 20 W -> 444 mA
    CHECK_EQ(sw::point_ma(tiny, 20000, 5000), 444u);                   // never above the PDP-derived limit
    const auto thin = epr_avs_apdo(15000, 45000, 140);
    CHECK_EQ(sw::point_ma(thin, 20000, 300), 300u);                    // never above the cable limit
    const auto spr = spr_avs_apdo(200, 50);                            // SPR AVS: 500 mA at/below 15 V, 2 A above
    CHECK_EQ(sw::point_ma(spr, 12000, 5000), 500u);                    // the SPR AVS <= 15 V current tier
    CHECK_EQ(sw::point_ma(spr, 18000, 5000), 1000u);                   // above 15 V: the 1 A cap binds (2 A offered)
}
// The FH22 AVS shape (host-side reference 2026-11-10, runs/.../fh22-avs-sweep.tsv): all 111 steps 9.0..20.0 V follow,
// the offset -94 mV at 9.2 V grows to -150 mV at 19.9 V, peak 19.75 V, and 20.0 V gives 19.66 V - it folds onto its
// fixed-20 V level. The summary reports the peak and the fold-back exactly like the PPS sweep; every point is recorded.
static void test_fh22_avs_sweep_shape() {
    std::vector<int16_t> err;
    for (unsigned i = 0; i <= 110; ++i) {
        const unsigned set = 9000 + i * 100;
        int e;
        if (set <= 19900) e = -int(94 + (i > 2 ? i - 2 : 0) * 56.0 / 107.0);   // -94 mV at 9.2 V growing to -150 mV at 19.9 V
        else e = -336;                  // 20.0 V -> 19.66 V: folds onto the fixed-20 V level
        err.push_back(int16_t(e));
    }
    CHECK_EQ(err.size(), 111u);
    const auto s = sw::summarize(err, 9000);
    CHECK_EQ(s.points, 111u);                       // all 111 steps recorded - the fold-back is not a stop
    CHECK_EQ(s.peak_mv, 19750u); CHECK_EQ(s.peak_at_mv, 19900u);
    CHECK_EQ(s.fold_from_mv, 20000u); CHECK_EQ(s.fold_mv, 19664u);
    CHECK_EQ(s.min_err, -336); CHECK_EQ(s.worst_mv, 20000u);   // the fold is the worst point
    CHECK_EQ(s.out_of_band, false);                 // 19.66 V is still within the 5 % band of 20.0 V
    char buf[64];
    const size_t n = sw::format_summary(s, buf);
    CHECK_STR(std::string(buf, n).c_str(), "peak 19.75 V at 19.9 V, folds back to 19.66 V from 20.0 V");
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

// The band a point is judged against: below the setpoint by more than max(100 mV, 5 %). One-sided: above is never out.
static void test_out_of_band_is_one_sided_100mv_or_5_percent() {
    CHECK_EQ(sw::out_of_band(3300, 3300), false);
    CHECK_EQ(sw::out_of_band(3300, 3200), false);    // 100 mV floor: 3300 / 20 = 165 mV is the wider side
    CHECK_EQ(sw::out_of_band(3300, 3135), false);    // exactly 165 mV below
    CHECK_EQ(sw::out_of_band(3300, 3134), true);
    CHECK_EQ(sw::out_of_band(12000, 11400), false);  // 5 % of 12 V = 600 mV
    CHECK_EQ(sw::out_of_band(12000, 11399), true);
    CHECK_EQ(sw::out_of_band(12000, 19420), false);  // above the setpoint: the ceiling's business, never the point's
    CHECK_EQ(sw::out_of_band(1000, 900), false);     // set / 20 = 50 mV: the 100 mV floor wins
    CHECK_EQ(sw::out_of_band(1000, 899), true);
    // summarize flags the same points
    std::vector<int16_t> ok{-150, -150}, bad{0, -700};   // setpoints 12000, 12100: 5 % is 600 / 605 mV
    CHECK_EQ(sw::summarize(ok, 12000).out_of_band, false);
    CHECK_EQ(sw::summarize(bad, 12000).out_of_band, true);
}
// The per-point line: requested, measured and the error (measured - setpoint) with its sign.
static void test_format_point_reads_req_got_and_the_error() {
    char b[sw::kPointTextMax];
    auto text = [&](unsigned set, int got) { return std::string(b, sw::format_point(set, got, b)); };
    CHECK_STR(text(12300, 12150).c_str(), "req 12.30V  got 12.15V  err-150mV");
    CHECK_STR(text(3300, 3300).c_str(), "req 3.30V  got 3.30V  err+0mV");
    CHECK_STR(text(5000, 5040).c_str(), "req 5.00V  got 5.04V  err+40mV");
    CHECK_STR(text(21000, 19420).c_str(), "req 21.00V  got 19.42V  err-1580mV");
    CHECK_STR(text(20000, 100).c_str(), "req 20.00V  got 0.10V  err-9999mV");   // a collapsed output: the error is clamped to 4 digits
    CHECK_EQ(text(36000, 36000).size() < sw::kPointTextMax, true);
    CHECK_EQ(text(36000, 36000).size() <= 34u, true);   // fits the 34-char cell
    CHECK_STR(text(12000, -50).c_str(), "req 12.00V  got 0.00V  err-9999mV");   // a negative reading shows as 0
}
// The live view of a row: the last measured point is the setpoint the runner stored (the points need not sit on one grid).
static void test_live_derives_the_last_point() {
    sw::Live l;
    CHECK_EQ(l.last_set_mv(), 0u); CHECK_EQ(l.last_measured_mv(), 0);
    l.lo_mv = 5000; l.hi_mv = 11000; l.n = 4; l.last_mv = 5300; l.last_err = -150;
    CHECK_EQ(l.last_set_mv(), 5300u);
    CHECK_EQ(l.last_measured_mv(), 5150);
}

// User decision 2026-11-14: the sweep of one APDO is split at 5 V (the part from 5 V up runs in the PD unit, the part below
// in PdLow). The two parts cover the APDO's points exactly once.
static void test_part_range_splits_at_5v() {
    const auto pr = sw::point_range(3300, 11000, 36000);
    const auto lo = sw::part_range(pr, true), hi = sw::part_range(pr, false);
    CHECK_EQ(lo.lo, 3300u); CHECK_EQ(lo.hi, 4900u);
    CHECK_EQ(hi.lo, 5000u); CHECK_EQ(hi.hi, 11000u);
    CHECK_EQ(sw::point_count(lo) + sw::point_count(hi), sw::point_count(pr));   // 17 + 61 = 78
    const auto all_high = sw::point_range(5000, 21000, 36000);
    CHECK_EQ(sw::point_count(sw::part_range(all_high, true)), 0u);
    CHECK_EQ(sw::point_count(sw::part_range(all_high, false)), 161u);
    const auto all_low = sw::point_range(3300, 4500, 36000);
    CHECK_EQ(sw::point_count(sw::part_range(all_low, true)), 13u);
    CHECK_EQ(sw::point_count(sw::part_range(all_low, false)), 0u);
}

// The card and the results strip keep one sweep per APDO: View::apdo merges the two parts. The oracle is the summary of the
// whole, unsplit track (the parts are recorded apart: the high part first, the low part at the end of the run).
static void test_apdo_view_merges_both_parts_like_one_track() {
    std::vector<int16_t> whole;   // 3.3..21 V: a low part 60 mV short, out of band at its first point, then the FH22 shape
    const unsigned tail[] = {19664, 19503, 19423, 19422, 19421, 19421, 19420, 19419, 19420, 19420, 19421};
    unsigned ti = 0;
    for (unsigned set = 3300; set <= 21000; set += 100) {
        const int measured = set < 5000 ? int(set) - (set == 3300 ? 400 : 60) : set <= 19900 ? int(set) - 150 : int(tail[ti++]);
        whole.push_back(int16_t(measured - int(set)));
    }
    CHECK_EQ(whole.size(), 178u);
    const auto want = sw::summarize(whole, 3300);
    std::vector<int16_t> rec(whole.begin() + 17, whole.end());   // the high part (161 points) runs first ...
    rec.insert(rec.end(), whole.begin(), whole.begin() + 17);    // ... the low part (17 points) at the end of the run
    sw::RowView rows[2];
    rows[0].pos = 7; rows[0].alg = 4; rows[0].lo_mv = 5000; rows[0].hi_mv = 21000; rows[0].first = 0; rows[0].n = 161;
    rows[0].sum = sw::summarize({rec.data(), 161}, 5000);
    rows[1].pos = 7; rows[1].alg = 4; rows[1].lo_mv = 3300; rows[1].hi_mv = 4900; rows[1].first = 161; rows[1].n = 17;
    rows[1].sum = sw::summarize({rec.data() + 161, 17}, 3300);
    const sw::View v{{rows, 2}, {rec.data(), rec.size()}};
    CHECK_EQ(v.find(4, 7, false) == &rows[0], true);
    CHECK_EQ(v.find(4, 7, true) == &rows[1], true);
    const auto m = v.apdo(4, 7);
    CHECK_EQ(m.has_value(), true);
    if (m) {
        CHECK_EQ(unsigned(m->lo_mv), 3300u); CHECK_EQ(unsigned(m->hi_mv), 21000u); CHECK_EQ(unsigned(m->n), 178u);
        const auto& g = m->sum;
        CHECK_EQ(g.points, want.points);
        CHECK_EQ(g.min_err, want.min_err); CHECK_EQ(g.max_err, want.max_err);
        CHECK_EQ(g.worst_err, want.worst_err); CHECK_EQ(g.worst_mv, want.worst_mv);
        CHECK_EQ(g.peak_mv, want.peak_mv); CHECK_EQ(g.peak_at_mv, want.peak_at_mv);
        CHECK_EQ(g.fold_mv, want.fold_mv); CHECK_EQ(g.fold_from_mv, want.fold_from_mv);
        CHECK_EQ(g.out_of_band, want.out_of_band);
        CHECK_EQ(g.peak_mv, 19750u); CHECK_EQ(g.fold_from_mv, 20000u);   // the FH22 numbers survive the merge
    }
    // One part alone is the APDO's sweep as it stands; another APDO has none.
    const sw::View high_only{{rows, 1}, {rec.data(), 161}};
    const auto h = high_only.apdo(4, 7);
    CHECK_EQ(h.has_value(), true);
    if (h) { CHECK_EQ(unsigned(h->lo_mv), 5000u); CHECK_EQ(unsigned(h->n), 161u); CHECK_EQ(h->sum.peak_mv, 19750u); }
    CHECK_EQ(v.apdo(4, 6).has_value(), false);
}

// User decision 2026-11-16: a fold is a caution the results show, so its note is a pure sentence of the summary: the level it
// folds to (rounded to 0.1 V) and the last setpoint that still tracked (the setpoint before the first one the rule names).
static void test_format_fold_names_the_level_and_the_last_good_setpoint() {
    char b[sw::kFoldTextMax];
    auto text = [&](const sw::Summary& s) { return std::string(b, sw::format_fold(s, b)); };
    unsigned lo = 0;
    auto err = fixture_err("tests/fixtures/sweep/fh22_pps_5000_21000.csv", lo);   // (lo is set by the call: not an argument of the same call)
    auto s = sw::summarize(err, lo);
    CHECK_STR(text(s).c_str(), "folds to 19.4V above 19.9V");        // 19400 mV from the 20.0 V request
    err = fixture_err("tests/fixtures/sweep/fh22_avs_9000_20000.csv", lo);
    s = sw::summarize(err, lo);
    CHECK_STR(text(s).c_str(), "folds to 19.4V above 19.9V");        // 19423 mV
    sw::Summary rounds;
    rounds.points = 111; rounds.fold_mv = 19664; rounds.fold_from_mv = 20000;
    CHECK_STR(text(rounds).c_str(), "folds to 19.7V above 19.9V");    // nearest 0.1 V, not truncated
    rounds.fold_mv = 19649;
    CHECK_STR(text(rounds).c_str(), "folds to 19.6V above 19.9V");
    rounds.fold_mv = 9050; rounds.fold_from_mv = 12100;
    CHECK_STR(text(rounds).c_str(), "folds to 9.1V above 12V");      // a whole volt drops its ".0", as the card's put_volts does
    rounds.fold_mv = 19960; rounds.fold_from_mv = 21000;
    CHECK_STR(text(rounds).c_str(), "folds to 20V above 20.9V");
    rounds.fold_mv = 36000; rounds.fold_from_mv = 36000;              // the widest levels still fit the cap
    CHECK_EQ(sw::format_fold(rounds, b) < sw::kFoldTextMax, true);
    // A clean track has no note.
    CHECK_EQ(sw::format_fold(sw::Summary{}, b), 0u);
    err = fixture_err("tests/fixtures/sweep/ugreen_avs_9000_20000.csv", lo);
    s = sw::summarize(err, lo);
    CHECK_EQ(sw::format_fold(s, b), 0u);
}
// A track BREAK (the offset grew without a fall) is not a fold: only a gap in the points makes one at 100 mV steps (the two
// segments of a split sweep that do not join), and it names no fold level, so it never reads "folds to".
static void test_a_break_is_not_a_fold() {
    std::vector<int16_t> a{0, 0};          // 5000, 5100 tracking
    std::vector<int16_t> b{-500, -500};    // 5600, 5700 read 5100 / 5200: 500 mV behind a 500 mV step, not lower than before
    const auto s = sw::summarize(a, 5000, b, 5600);
    CHECK_EQ(s.fold_mv, 0u); CHECK_EQ(s.fold_from_mv, 0u);
    char buf[sw::kFoldTextMax];
    CHECK_EQ(sw::format_fold(s, buf), 0u);
    // the same gap with a real fall is a fold
    std::vector<int16_t> c{-700, -700};    // 5600, 5700 read 4900 / 5000: below the 5100 before
    const auto f = sw::summarize(a, 5000, c, 5600);
    CHECK_EQ(f.fold_from_mv, 5600u); CHECK_EQ(f.fold_mv, 4900u);
}

// ---- sweep depth (Settings "Sweep depth"): the point sequence of a row, the phone windows, the located fold ----
static std::vector<unsigned> seq(const sw::Plan& p) {
    std::vector<unsigned> v;
    for (unsigned mv = p.first(); mv; mv = p.next(mv)) v.push_back(mv);
    return v;
}
// part: 0 = the part from 5 V up, 1 = the low part, 2 = the whole range
static sw::Plan plan_of(sw::Depth d, sw::Kind k, unsigned min_mv, unsigned max_mv, unsigned cap, unsigned wins = 0, int part_sel = 2) {
    const auto whole = sw::point_range(min_mv, max_mv, cap);
    const auto part = part_sel == 2 ? whole : sw::part_range(whole, part_sel == 1);
    return sw::make_plan(d, k, part, std::min(max_mv, cap), wins);
}
static void test_depth_100_is_the_old_plan() {
    const auto s = seq(plan_of(sw::Depth::Step100, sw::Kind::Pps, 3300, 11000, 36000));
    CHECK_EQ(s.size(), 78u); CHECK_EQ(s.front(), 3300u); CHECK_EQ(s.back(), 11000u);
    for (size_t i = 1; i < s.size(); ++i) CHECK_EQ(s[i] - s[i - 1], 100u);
    CHECK_EQ(sw::plan_points(plan_of(sw::Depth::Step100, sw::Kind::Pps, 3300, 11000, 36000)), 78u);
}
static void test_depth_200_is_a_subset_of_100_plus_the_top() {
    const auto s = seq(plan_of(sw::Depth::Step200, sw::Kind::Pps, 3300, 21000, 36000));
    CHECK_EQ(s.size(), 90u);                       // 3.3 .. 20.9 V every 200 mV (89), then the 21.0 V top
    CHECK_EQ(s.front(), 3300u); CHECK_EQ(s.back(), 21000u);
    CHECK_EQ(s[s.size() - 2], 20900u);
    for (size_t i = 1; i + 1 < s.size(); ++i) CHECK_EQ(s[i] - s[i - 1], 200u);
    CHECK_EQ(s[s.size() - 1] - s[s.size() - 2], 100u);
    for (unsigned mv : s) CHECK_EQ((mv - 3300u) % 100u, 0u);   // every point is a record cell
    // a range that ends on the 200 mV grid adds nothing
    CHECK_EQ(seq(plan_of(sw::Depth::Step200, sw::Kind::Pps, 5000, 9000, 36000)).back(), 9000u);
    CHECK_EQ(seq(plan_of(sw::Depth::Step200, sw::Kind::Pps, 5000, 9000, 36000)).size(), 21u);
    // AVS 200 mV too
    CHECK_EQ(seq(plan_of(sw::Depth::Step200, sw::Kind::Avs, 9000, 20000, 36000)).size(), 56u);   // 9.0 .. 20.0 V, the top on the grid
}
static void test_the_apdo_top_is_always_a_point() {
    for (auto d : {sw::Depth::Step100, sw::Depth::Step200, sw::Depth::Phone, sw::Depth::Step20}) {
        CHECK_EQ(seq(plan_of(d, sw::Kind::Pps, 3300, 21000, 36000)).back(), 21000u);
        CHECK_EQ(seq(plan_of(d, sw::Kind::Pps, 3300, 21000, 12300)).back(), 12300u);   // the monitor / cable cap is the top
        CHECK_EQ(seq(plan_of(d, sw::Kind::Avs, 9000, 20000, 36000)).back(), 20000u);
    }
    // an APDO top between two cells but on the 20 mV grid is the exact last point (and off the record's cell grid)
    const auto p = plan_of(sw::Depth::Step100, sw::Kind::Pps, 5000, 11020, 36000);
    CHECK_EQ(seq(p).back(), 11020u); CHECK_EQ(sw::plan_cell(p, 11020), -1); CHECK_EQ(sw::plan_cell(p, 11000), 60);
    // not on the 20 mV grid: the floor
    CHECK_EQ(seq(plan_of(sw::Depth::Step20, sw::Kind::Pps, 5000, 11010, 36000)).back(), 11000u);
    // the split parts each end on their own top
    CHECK_EQ(seq(plan_of(sw::Depth::Step200, sw::Kind::Pps, 3300, 21000, 36000, 0, 1)).back(), 4900u);
    CHECK_EQ(seq(plan_of(sw::Depth::Step200, sw::Kind::Pps, 3400, 21000, 36000, 0, 1)).back(), 4900u);
}
static void test_depth_20_steps_every_pps_quantum_but_avs_stays_100() {
    const auto s = seq(plan_of(sw::Depth::Step20, sw::Kind::Pps, 3300, 21000, 36000));
    CHECK_EQ(s.size(), 886u);
    for (size_t i = 1; i < s.size(); ++i) CHECK_EQ(s[i] - s[i - 1], 20u);
    CHECK_EQ(seq(plan_of(sw::Depth::Step20, sw::Kind::Avs, 9000, 20000, 36000)).size(), 111u);
    CHECK_EQ(sw::plan_cell(plan_of(sw::Depth::Step20, sw::Kind::Pps, 3300, 21000, 36000), 3400), 1);
    CHECK_EQ(sw::plan_cell(plan_of(sw::Depth::Step20, sw::Kind::Pps, 3300, 21000, 36000), 3320), -1);
}
static void test_phone_depth_is_20mv_inside_the_two_windows_only() {
    const auto s = seq(plan_of(sw::Depth::Phone, sw::Kind::Pps, 3300, 21000, 36000, 3));
    CHECK_EQ(s.size(), 498u);                    // 178 cells, 30 + 50 of them replaced by 150 + 250 twenty-mV points
    for (size_t i = 1; i < s.size(); ++i) {
        const bool fine = (s[i - 1] >= 8000 && s[i - 1] < 11000) || (s[i - 1] >= 14000 && s[i - 1] < 19000);
        CHECK_EQ(s[i] - s[i - 1], fine ? 20u : 100u);
    }
    // a window the APDO does not own (wins bit clear) is swept at 100 mV; AVS never has windows
    CHECK_EQ(seq(plan_of(sw::Depth::Phone, sw::Kind::Pps, 3300, 21000, 36000, 0)).size(), 178u);
    CHECK_EQ(seq(plan_of(sw::Depth::Phone, sw::Kind::Pps, 3300, 21000, 36000, 2)).size(), 178u - 50u + 250u);
    CHECK_EQ(seq(plan_of(sw::Depth::Phone, sw::Kind::Avs, 9000, 20000, 36000, 3)).size(), 111u);
    // the cap clips the window: 11.0 V ceiling ends inside neither
    const auto c = seq(plan_of(sw::Depth::Phone, sw::Kind::Pps, 3300, 21000, 9000, 3));
    CHECK_EQ(c.back(), 9000u); CHECK_EQ(c.size(), 47u + 51u);   // 3.3..7.9 V (47 cells), then 8.0..9.0 V at 20 mV (51)
}
static void test_phone_windows_go_to_the_apdo_with_the_most_current() {
    const auto small = pps_apdo(3300, 11000, 3000), big = pps_apdo(3300, 21000, 5000), mid = pps_apdo(3300, 21000, 3000);
    // 8-11 V: both cover it, the 5 A one wins; 14-19 V: only the big one reaches
    const auto caps = caps_with({pd::decode_pdo(0x0001912c), small, big});
    CHECK_EQ(sw::phone_wins(caps, 36000, 1), 0u);   // the fixed 5 V PDO has no window
    CHECK_EQ(sw::phone_wins(caps, 36000, 2), 0u);
    CHECK_EQ(sw::phone_wins(caps, 36000, 3), 3u);
    // equal current: the wider overlap wins (the 21 V one covers more of 14-19 V than the 11 V one covers of 8-11 V: both fully)
    const auto tie = caps_with({small, mid});
    CHECK_EQ(sw::phone_wins(tie, 36000, 1), 1u);    // 8-11 V: tie on current and overlap -> the first APDO
    CHECK_EQ(sw::phone_wins(tie, 36000, 2), 2u);    // 14-19 V: only the 21 V one reaches
    // the monitor / cable cap clips what an APDO can reach: a 12 V cap leaves no 14-19 V window at all
    CHECK_EQ(sw::phone_wins(caps, 12000, 3), 1u);
    // a partial overlap still counts (an APDO that stops at 9 V covers 1 V of the 8-11 V window)
    const auto part = caps_with({pps_apdo(3300, 9000, 3000)});
    CHECK_EQ(sw::phone_wins(part, 36000, 1), 1u);
}
// The FH22 fold sits in the last 100 mV (19.9 V gives 19.75 V; 20.0 V gives 19.66 V; then 19.42 V): a 200 mV sweep has to catch it
// (track_limit(200) = 150: 200 mV request step, the output moved +14 mV, so 186 behind is a Break at 20.0 V; 20.2 V is the Fold).
static int fh22_level(int req) {
    if (req <= 19980) return req - 150;
    return req == 20000 ? 19664 : req == 20100 ? 19503 : req <= 20200 ? 19423 : 19420;
}
static void test_fh22_fold_is_caught_at_200mv() {
    const auto p = plan_of(sw::Depth::Step200, sw::Kind::Pps, 5000, 21000, 36000);
    pd::TrackScan scan;
    std::vector<int16_t> rec(161, sw::kHole);
    for (unsigned mv = p.first(); mv; mv = p.next(mv)) {
        scan.feed(int(mv), fh22_level(int(mv)));
        if (scan.found() && scan.req_mv() == int(mv) && scan.kind() == pd::Track::Break) CHECK_EQ(mv, 20000u);
        const int c = sw::plan_cell(p, mv);
        CHECK_EQ(c >= 0, true);
        rec[size_t(c)] = int16_t(fh22_level(int(mv)) - int(mv));
    }
    CHECK_EQ(scan.found(), true);
    CHECK_EQ(scan.kind(), pd::Track::Break);           // 20.0 V: +14 mV for a 200 mV step
    const auto s = sw::summarize(rec, 5000);
    CHECK_EQ(s.points, 81u);                           // 5.0..21.0 V every 200 mV
    CHECK_EQ(s.peak_mv, 19664u); CHECK_EQ(s.peak_at_mv, 20000u);
    CHECK_EQ(s.fold_from_mv, 20000u);                  // the break the 20.2 V point falls away from is the fold's onset
    CHECK_EQ(s.fold_ok_mv, 19800u);
    CHECK_EQ(s.fold_mv, 19420u);
    char buf[sw::kFoldTextMax];
    CHECK_STR(std::string(buf, sw::format_fold(s, buf)).c_str(), "folds to 19.4V above 19.8V");
    // a break that stays level is no fold
    std::vector<int16_t> flat(rec.begin(), rec.end());
    for (size_t i = 76; i < flat.size(); ++i) if (flat[i] != sw::kHole) flat[i] = int16_t(19664 - int(5000 + 100 * i));
    CHECK_EQ(sw::summarize(flat, 5000).fold_mv, 0u);
}
// The located fold: Refiner re-sweeps between the last good and the first bad request at 100 mV, then 20 mV.
static void test_refiner_locates_the_fh22_fold_to_20mv() {
    sw::Refiner r;
    r.begin(sw::Kind::Pps, pd::Track::Break, 19800, fh22_level(19800), 20000, fh22_level(20000));
    std::vector<unsigned> asked;
    while (!r.done() && asked.size() < 20) {
        const unsigned mv = r.next();
        CHECK_EQ(mv != 0, true);
        asked.push_back(mv);
        r.feed(fh22_level(int(mv)));
    }
    const std::vector<unsigned> want = {19800, 19900, 19900, 19920, 19940, 19960, 19980};   // recheck, 100 mV, recheck, 20 mV x4
    CHECK_EQ(asked.size(), want.size());
    for (size_t i = 0; i < want.size() && i < asked.size(); ++i) CHECK_EQ(asked[i], want[i]);
    CHECK_EQ(r.done(), true);
    const auto& l = r.result();
    CHECK_EQ(l.found, true);
    CHECK_EQ(l.kind, pd::Track::Fold);
    CHECK_EQ(l.ok_mv, 19980u); CHECK_EQ(l.ok_level_mv, 19830u);
    CHECK_EQ(l.bad_mv, 20000u); CHECK_EQ(l.bad_level_mv, 19664u);
}
static void test_refiner_gives_up_when_the_good_point_no_longer_tracks_and_skips_a_20mv_gap() {
    sw::Refiner r;   // the re-measured last good point has itself folded: a latched state, nothing to locate
    r.begin(sw::Kind::Pps, pd::Track::Fold, 19900, 19750, 20000, 19420);
    CHECK_EQ(r.next(), 19900u);
    r.feed(19420);
    CHECK_EQ(r.done(), true); CHECK_EQ(r.result().found, false);
    // two points one quantum apart are already located
    sw::Refiner q;
    q.begin(sw::Kind::Pps, pd::Track::Fold, 19980, 19830, 20000, 19420);
    CHECK_EQ(q.done(), true); CHECK_EQ(q.result().found, true); CHECK_EQ(q.result().ok_mv, 19980u);
    // AVS: 100 mV is the floor, so a 100 mV pair is located and a 200 mV pair takes one recheck and one point
    sw::Refiner a;
    a.begin(sw::Kind::Avs, pd::Track::Fold, 19800, 19650, 20000, 19664 - 400);
    CHECK_EQ(a.next(), 19800u); a.feed(19650);
    CHECK_EQ(a.next(), 19900u); a.feed(19750);
    CHECK_EQ(a.done(), true); CHECK_EQ(a.result().ok_mv, 19900u); CHECK_EQ(a.result().bad_mv, 20000u);
}

// A fold located by Refiner survives the merge of the APDO's two parts (the merge re-reads the cells and would lose it).
static void test_apdo_view_keeps_a_located_fold() {
    std::vector<int16_t> rec;
    for (unsigned set = 5000; set <= 21000; set += 100) rec.push_back(int16_t(fh22_level(int(set)) - int(set)));
    std::vector<int16_t> low(5, 0);
    sw::RowView rows[2];
    rows[0].pos = 7; rows[0].alg = 4; rows[0].lo_mv = 5000; rows[0].hi_mv = 21000; rows[0].first = 0; rows[0].n = 161;
    rows[0].sum = sw::summarize(rec, 5000);
    rows[0].sum.fold_located = true; rows[0].sum.fold_ok_mv = 19980; rows[0].sum.fold_from_mv = 20000; rows[0].sum.fold_mv = 19419;
    rows[1].pos = 7; rows[1].alg = 4; rows[1].lo_mv = 3300; rows[1].hi_mv = 4900; rows[1].first = 161; rows[1].n = 5;
    rows[1].sum = sw::summarize(low, 3300);
    std::vector<int16_t> all = rec; all.insert(all.end(), low.begin(), low.end());
    const sw::View v{{rows, 2}, {all.data(), all.size()}};
    const auto m = v.apdo(4, 7);
    CHECK_EQ(m.has_value(), true);
    if (!m) return;
    CHECK_EQ(m->sum.fold_located, true); CHECK_EQ(m->sum.fold_ok_mv, 19980u); CHECK_EQ(m->sum.fold_from_mv, 20000u);
    char b[sw::kFoldTextMax];
    CHECK_STR(std::string(b, sw::format_fold(m->sum, b)).c_str(), "folds to 19.4V above 20V");
}

int main() {
    test_apdo_view_keeps_a_located_fold();
    test_depth_100_is_the_old_plan();
    test_depth_200_is_a_subset_of_100_plus_the_top();
    test_the_apdo_top_is_always_a_point();
    test_depth_20_steps_every_pps_quantum_but_avs_stays_100();
    test_phone_depth_is_20mv_inside_the_two_windows_only();
    test_phone_windows_go_to_the_apdo_with_the_most_current();
    test_fh22_fold_is_caught_at_200mv();
    test_refiner_locates_the_fh22_fold_to_20mv();
    test_refiner_gives_up_when_the_good_point_no_longer_tracks_and_skips_a_20mv_gap();
    test_format_fold_names_the_level_and_the_last_good_setpoint();
    test_a_break_is_not_a_fold();
    test_part_range_splits_at_5v();
    test_apdo_view_merges_both_parts_like_one_track();
    test_out_of_band_is_one_sided_100mv_or_5_percent();
    test_format_point_reads_req_got_and_the_error();
    test_live_derives_the_last_point();
    test_point_plan_covers_the_apdo_in_100mv_steps();
    test_point_current_is_1a_never_above_the_offer_or_cable();
    test_plan_total_counts_every_pps_apdo_within_the_budget();
    test_avs_plan_spr_only();
    test_avs_plan_spr_plus_epr_capped_at_the_ceiling();
    test_avs_point_current_never_above_the_pdp_limit_or_1a();
    test_summary_of_a_clean_track_names_peak_and_errors();
    test_fh22_fold_back_is_recorded_not_a_stop();
    test_recorded_sweeps_fold_or_stay_clean();
    test_track_err_stops_at_the_fold_onset();
    test_a_small_dip_is_not_a_fold();
    test_catching_up_is_not_a_finding();
    test_fh22_avs_sweep_shape();
    test_summary_of_nothing_and_format_mv();
    return test_report("test_sweep");
}
