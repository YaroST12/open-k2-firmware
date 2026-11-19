// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The per-move programmable judge (k2/pdspec/prog_judge.hpp) over hand-built step analyses: which check each measurement goes to, its
// limit (the small / large step rule, vPpsValid / vAvsValid, the transient, vPpsNew / vAvsNew) and its place, worked from the spec values.
#include <cstdint>

#include "k2/pdspec/prog_judge.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdspec;

namespace {

StepOut step(std::int32_t final_mv, std::uint32_t band_us, std::uint32_t gap_us, std::int32_t peak_mv, std::uint32_t out_us = 0) {
    StepOut o;
    o.valid = true;
    o.final_mv = final_mv;
    o.band = {band_us, gap_us, true};
    o.peak_mv = peak_mv;
    o.out_us = out_us;
    return o;
}

// A 100 mV PPS move up (class 1): small step, band against 25 ms, overshoot against band_hi + 100 mV, transient against 150 ms.
void pps_small_up() {
    const ProgMeas m = measure_prog({6, false, 1, 9000, 9100}, step(9095, 30000, 2000, 9600, 0));
    CHECK_COND(m.up);
    CHECK_COND(!m.large);
    CHECK_EQ(unsigned(m.a), unsigned(6 | 1 << 4));
    CHECK_EQ(unsigned(m.b), 91u);
    CHECK_EQ(unsigned(m.band_c), unsigned(Check::PpsBand));
    CHECK_EQ(m.band.value, 30000);
    CHECK_EQ(m.band.gap, 2000);
    CHECK_EQ(m.band.hi, 25000);
    CHECK_COND(!passes(Kind::Max, m.band));          // 28 ms is past 25 ms even with the 2 ms bracket
    CHECK_EQ(unsigned(m.over_c), unsigned(Check::PpsOvershoot));
    CHECK_EQ(m.over.hi, 9100 + 455 + 100);           // vPpsNew 5 % of 9.1 V, then vPpsValid
    CHECK_COND(passes(Kind::Range, m.over));
    CHECK_COND(m.trans_known);
    CHECK_EQ(m.trans.hi, 150000);
    CHECK_COND(m.level_known);
    CHECK_EQ(unsigned(m.acc_c), unsigned(Check::PpsAccuracy));
    CHECK_EQ(m.acc.lo, 9100 - 455);
    CHECK_EQ(m.acc.hi, 9100 + 455);
}

// The 20 mV class judges the settle (tPpsSrcTransSmall, unverified) instead of the band.
void pps_settle_class() {
    StepOut o = step(9020, 0, 1000, 9030);
    o.settle = {8000, 1000, true};
    o.settled = true;
    const ProgMeas m = measure_prog({6, false, 0, 9000, 9020}, o);
    CHECK_EQ(unsigned(m.band_c), unsigned(Check::PpsSettle));
    CHECK_EQ(m.band.value, 8000);
    CHECK_EQ(m.band.hi, 25000);
}

// A 2 V AVS move down (a slam, large): band against 325 ms, the dip against band_lo - 500 mV, transient 5 ms, place marks the way down.
void avs_large_down() {
    const ProgMeas m = measure_prog({7, true, kSlam, 20000, 18000}, step(18010, 200000, 1000, 16000, 7000));
    CHECK_COND(!m.up);
    CHECK_COND(m.large);
    CHECK_EQ(unsigned(m.a), unsigned(7 | kSlam << 4 | 0x80));
    CHECK_EQ(unsigned(m.band_c), unsigned(Check::AvsBand));
    CHECK_EQ(m.band.hi, 325000);
    CHECK_EQ(unsigned(m.over_c), unsigned(Check::AvsOvershoot));
    CHECK_EQ(m.over.lo, 18000 - 900 - 500);
    CHECK_COND(!passes(Kind::Range, m.over));        // 16.0 V is under 16.6 V
    CHECK_EQ(unsigned(m.trans_c), unsigned(Check::AvsTransient));
    CHECK_EQ(m.trans.hi, 5000);
    CHECK_COND(!passes(Kind::Max, m.trans));
}

// A move that never entered the band and never settled: the band reads never, no transient, no level.
void never_in_band() {
    StepOut o;
    o.valid = true;
    o.final_mv = 8000;
    o.peak_mv = 8000;
    const ProgMeas m = measure_prog({6, false, 1, 9000, 9100}, o);
    CHECK_EQ(m.band.value, kNever);
    CHECK_COND(!m.trans_known);
    CHECK_COND(!m.level_known);
}

}  // namespace

int main() {
    pps_small_up();
    pps_settle_class();
    avs_large_down();
    never_in_band();
    return test_report("test_pdspec_judge");
}
