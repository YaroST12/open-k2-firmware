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
    // an EPR AVS APDO wholly above the ceiling contributes nothing ("above the K2's monitor")
    pd::SourceCaps hi = caps; hi.pdos[9] = epr_avs_apdo(40000, 48000, 140);
    CHECK_EQ(sw::plan_total(hi, 36000, sw::Kind::Avs), 111u);
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
        if (i == 0) e = -2668;          // the 5 V contract's tail at the first point (6332 mV)
        else if (i == 1) e = -805;      // 8295 mV
        else if (set <= 19900) e = -int(94 + (i - 2) * 56.0 / 107.0);   // -94 mV at 9.2 V growing to -150 mV at 19.9 V
        else e = -336;                  // 20.0 V -> 19.66 V: folds onto the fixed-20 V level
        err.push_back(int16_t(e));
    }
    CHECK_EQ(err.size(), 111u);
    const auto s = sw::summarize(err, 9000);
    CHECK_EQ(s.points, 111u);                       // all 111 steps recorded - the fold-back is not a stop
    CHECK_EQ(s.peak_mv, 19750u); CHECK_EQ(s.peak_at_mv, 19900u);
    CHECK_EQ(s.fold_from_mv, 20000u); CHECK_EQ(s.fold_mv, 19664u);
    CHECK_EQ(s.min_err, -2668); CHECK_EQ(s.worst_mv, 9000u);   // the ramp artifact is recorded too
    CHECK_EQ(s.out_of_band, true);
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

int main() {
    test_point_plan_covers_the_apdo_in_100mv_steps();
    test_point_current_is_1a_never_above_the_offer_or_cable();
    test_plan_total_counts_every_pps_apdo_within_the_budget();
    test_avs_plan_spr_only();
    test_avs_plan_spr_plus_epr_capped_at_the_ceiling();
    test_avs_point_current_never_above_the_pdp_limit_or_1a();
    test_summary_of_a_clean_track_names_peak_and_errors();
    test_fh22_fold_back_is_recorded_not_a_stop();
    test_fh22_avs_sweep_shape();
    test_summary_of_nothing_and_format_mv();
    return test_report("test_sweep");
}
