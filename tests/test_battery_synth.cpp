// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Synthetic battery runs: the recorder fed made-up V/I streams (constant power, ramps, CC/CV taper, pauses that trip the end
// rule, both signs and wirings, gaps, a continue, a round trip, runs split in two) checked against closed-form integrals; the
// efficiency of separate runs (view::pair); and a replay of the real curves of 2026-11-24/25 (runs/battery, ids 1..6).
//
// Tolerances, from the arithmetic rather than tuned: constant segments integrate exactly (trapezoid of a constant; mW, mA
// whole numbers), so they are checked to 1 uWh / 1 uAh (the uJ -> uWh cut). A ramp's samples are rounded to 1 mV / 1 mA and
// each step's power is cut to 1 mW: under 1 mWh per hour, plus the dead band (|I| < 20 mA counts 0). A pause edge sampled
// every second is one trapezoid of half a step's power x 1 s, counted as such.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "k2/battery/battery.hpp"
#include "k2/core/fs.hpp"
#include "k2/core/ringlog.hpp"
#include "k2/ui/battery_view.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

#define CHECK(c) CHECK_COND(c)

using namespace k2::battery;
namespace view = k2::battery::view;

#define NEAR(actual, expected, tol) do { \
    const double a_ = double(actual), e_ = double(expected); \
    g_checks++; \
    if (std::fabs(a_ - e_) > double(tol)) { g_failures++; \
        std::printf("FAIL %s:%d: %s = %.0f, expected %.0f +- %.0f\n", __FILE__, __LINE__, #actual, a_, e_, double(tol)); } \
} while (0)

namespace {

struct Sample { std::int32_t mv, ma; };

// Samples every dt_ms from t0 to t1 inclusive; f(t_ms) gives the sample. Returns t1.
template <class F>
std::uint32_t feed(Recorder &r, std::uint32_t t0, std::uint32_t t1, std::uint32_t dt_ms, F f) {
    for (std::uint32_t t = t0; t <= t1; t += dt_ms) {
        const Sample s = f(t);
        r.sample(t, s.mv, s.ma, true);
    }
    return t1;
}

Sums sums_of(const Recorder &r) {
    Record rec;
    r.to_record(rec);
    return sums(rec);
}

Record record_of(const Recorder &r) {
    Record rec;
    r.to_record(rec);
    return rec;
}

constexpr double kUwhPerWs = 1e6 / 3600.0;

// ---- closed forms ----------------------------------------------------------------------------------------------------------

// 20 V x 3 A for an hour, sampled every 250 ms: 60 Wh, 3 Ah exactly.
void test_constant_power() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 3'600'000, 250, [](std::uint32_t) { return Sample{20000, 3000}; });
    r.end_phase();
    const Sums s = sums_of(r);
    NEAR(s.uwh_in, 60'000'000, 1);
    NEAR(s.uah_in, 3'000'000, 1);
    CHECK_EQ(s.uwh_out, 0);
    CHECK_EQ(s.secs, 3600);
}

// V 5 -> 20 V and I 0 -> 3 A together over an hour (a PD step-up with a load ramp): E = T x int_0^1 (5 + 15x)(3x) dx = 22.5 Wh,
// Q = 1.5 Ah. The dead band drops the first ~24 s (|I| < 20 mA): <= 20 mA x 5.1 V x 24 s = 0.7 mWh.
void test_ramp() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 3'600'000, 1000, [](std::uint32_t t) {
        const double x = t / 3'600'000.0;
        return Sample{std::int32_t(std::lround(5000 + 15000 * x)), std::int32_t(std::lround(3000 * x))};
    });
    r.end_phase();
    const Sums s = sums_of(r);
    NEAR(s.uwh_in, 22'500'000, 2'000);   // 1 mWh of rounding + 0.7 mWh of dead band
    NEAR(s.uah_in, 1'500'000, 300);      // the dead band: 20 mA x 24 s / 2 = 67 uAh, + rounding
    CHECK_EQ(s.secs, 3600);
}

// CC/CV: 3 A for 30 min, then I = 3 A x e^(-t/600 s) at 20 V, sampled to the nearest mA. The end rule (50 mA for 60 s)
// starts its quiet at the first sample under 50 mA (49 mA: t = ceil(600 ln(3000 / 49.5)) = 2463 s) and ends 60 s later; the
// phase time stops where the quiet began; the energy runs to the end (the tail stays over the 20 mA dead band).
// E = 30 Wh + 20 V x 3 A x 600 s x (1 - e^(-tq/600)) / 3600, tq = 2463 + 60 s.
void test_cc_cv_taper() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    const auto f = [](std::uint32_t t) {
        if (t <= 1'800'000) return Sample{20000, 3000};
        const double u = (t - 1'800'000) / 1000.0;
        return Sample{20000, std::int32_t(std::lround(3000 * std::exp(-u / 600)))};
    };
    for (std::uint32_t t = 0; r.state() == State::Measuring && t < 30'000'000; t += 1000) r.sample(t, f(t).mv, f(t).ma, true);
    CHECK_EQ(int(r.state()), int(State::Done));
    CHECK_EQ(int(r.phase(0).end), int(End::Auto));
    const Sums s = sums_of(r);
    const double tq = std::ceil(600 * std::log(3000 / 49.5));
    const double e = (30.0 + 20 * 3 * 600 * (1 - std::exp(-(tq + 60) / 600)) / 3600) * 1e6;
    NEAR(s.uwh_in, e, 0.0005 * e);   // rounding to 1 mA over 2517 s: <= 7 mWh; trapezoid on the exponential: 2e-7
    NEAR(s.secs, 1800 + tq, 0);
}

// A discharge pauses longer than the end time: the phase ends (Auto) where the pause began, what flows after is not counted.
// Same port: the bank's outflow is the reverse current. 20 V x 2 A for 600 s = 6.667 Wh, + the edge step 600 -> 601 s
// (40 W -> 0 in one sample: 20 Ws).
void test_pause_trips_end_rule() {
    Recorder r;
    r.start(Mode::Discharge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 1'200'000, 1000, [](std::uint32_t t) {
        const bool on = t <= 600'000 || t >= 800'000;   // 199 s off: over the 60 s end time
        return Sample{20000, on ? -2000 : 0};
    });
    CHECK_EQ(int(r.state()), int(State::Done));
    CHECK_EQ(int(r.phase(0).end), int(End::Auto));
    const Sums s = sums_of(r);
    NEAR(s.uwh_out, (40.0 * 600 + 20) * kUwhPerWs, 1);
    CHECK_EQ(s.uwh_in, 0);
    CHECK_EQ(s.secs, 601);   // the quiet began at the first sample under the threshold (601 s)
}

// A pause shorter than the end time: the phase goes on, the pause is in its time; two edges of 20 Ws each.
void test_short_pause_keeps_measuring() {
    Recorder r;
    r.start(Mode::Discharge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 1'230'000, 1000, [](std::uint32_t t) {
        const bool on = t <= 600'000 || t >= 630'000;   // 29 s off
        return Sample{20000, on ? -2000 : 0};
    });
    CHECK_EQ(int(r.state()), int(State::Measuring));
    r.end_phase();
    const Sums s = sums_of(r);
    NEAR(s.uwh_out, (40.0 * 1200 + 2 * 20) * kUwhPerWs, 1);
    CHECK_EQ(s.secs, 1230);
}

// Either wiring, either sign: the phase's own column takes what flowed. Moved: the bank on the charger's port, forward current;
// same port: reverse. 20 V x 2 A for an hour = 40 Wh out, nothing in.
void test_sign_and_wiring() {
    for (int k = 0; k < 4; k++) {
        const Wiring w = k & 1 ? Wiring::Moved : Wiring::SamePort;
        const std::int32_t ma = k & 2 ? 2000 : -2000;
        Recorder r;
        r.start(Mode::Discharge, w, Config{}, 1);
        feed(r, 0, 3'600'000, 1000, [&](std::uint32_t) { return Sample{20000, ma}; });
        r.end_phase();
        const Sums s = sums_of(r);
        NEAR(s.uwh_out, 40'000'000, 1);
        CHECK_EQ(s.uwh_in, 0);
        NEAR(s.uah_out, 2'000'000, 1);
    }
}

// A 10 s stall of the loop (no samples) is a gap: not integrated, one gap, its time in the phase's time.
void test_gap() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 600'000, 1000, [](std::uint32_t) { return Sample{20000, 1000}; });
    feed(r, 610'000, 1'210'000, 1000, [](std::uint32_t) { return Sample{20000, 1000}; });
    r.end_phase();
    const Sums s = sums_of(r);
    NEAR(s.uwh_in, 20.0 * 1200 * kUwhPerWs, 1);
    CHECK_EQ(r.phase(0).gaps, 1);
    CHECK_EQ(s.secs, 1210);
}

// The end rule closed a charge in a pause; Continue reopens it and the second stretch adds to it.
void test_continue_adds() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    const auto on = [](std::uint32_t t) { return Sample{20000, t % 1'000'000 <= 600'000 ? 2000 : 0}; };
    feed(r, 0, 700'000, 1000, on);   // 600 s on, then the end rule (quiet from 601 s, ends at 661 s)
    CHECK_EQ(int(r.state()), int(State::Done));
    CHECK(r.reopen());
    feed(r, 1'000'000, 1'700'000, 1000, on);
    CHECK_EQ(int(r.state()), int(State::Done));
    const Sums s = sums_of(r);
    NEAR(s.uwh_in, 2 * (40.0 * 600 + 20) * kUwhPerWs, 1);
    CHECK_EQ(s.secs, 2 * 601);
    CHECK_EQ(r.resumes(), 1);
    CHECK_EQ(r.gap_s(), 1'000 - 661);   // the stop: from the end at 661 s to the first sample at 1000 s
}

// A round trip, same port: 60 Wh in, 48 Wh out: 80.0 %. The discharge starts by itself 5 s into the outflow, those seconds
// counted from the quiet sample before it (an edge of 48 W x 1 s / 2).
void test_round_trip_efficiency() {
    Recorder r;
    r.start(Mode::RoundTrip, Wiring::SamePort, Config{}, 1);
    std::uint32_t t = feed(r, 0, 3'600'000, 1000, [](std::uint32_t) { return Sample{20000, 3000}; });
    r.end_phase();
    t = feed(r, t + 1000, t + 10'000, 1000, [](std::uint32_t) { return Sample{20000, 0}; });   // the swap: quiet
    CHECK_EQ(int(r.state()), int(State::Between));
    const std::uint32_t t0 = t + 1000;
    feed(r, t0, t0 + 3'600'000, 1000, [](std::uint32_t) { return Sample{20000, -2400}; });
    CHECK_EQ(int(r.state()), int(State::Measuring));
    r.finish();
    const Record rec = record_of(r);
    CHECK(has_efficiency(rec));
    const Sums s = sums(rec);
    NEAR(s.uwh_in, 60'000'000, 1);
    NEAR(s.uwh_out, (48.0 * 3600 + 24) * kUwhPerWs, 1);
    CHECK_EQ(ratio_permille(s.uwh_out, s.uwh_in), 800);
}

// ---- the efficiency of separate runs ----------------------------------------------------------------------------------------

view::Shown run(std::uint16_t id, Mode m, std::uint64_t uwh, bool lost = false) {
    Record r;
    r.id = id;
    r.mode = m;
    r.phases = m == Mode::RoundTrip ? 2 : 1;
    r.ph[0] = PhaseRec{m == Mode::Discharge ? Kind::Discharge : Kind::Charge, lost ? End::Lost : End::Auto, 0, 3600};
    (m == Mode::Discharge ? r.ph[0].uwh_out : r.ph[0].uwh_in) = std::uint32_t(uwh);
    if (m == Mode::RoundTrip) r.ph[1] = PhaseRec{Kind::Discharge, End::Auto, 0, 3600, 0, std::uint32_t(uwh * 8 / 10)};
    return view::shown(r, true);
}

// The newest stretch of one kind and the stretch of the other kind just before it, each summed (a phase split over runs):
// Wh out of the discharges / Wh in of the charges, in either order.
void test_pair() {
    {   // charge split in two, then the discharge (newest first)
        const view::Shown s[] = {run(7, Mode::Discharge, 24'000'000), run(6, Mode::Charge, 10'000'000), run(5, Mode::Charge, 20'000'000)};
        const view::Pair p = view::pair(s, 3);
        CHECK_EQ(p.eff, 800);
        CHECK_EQ(p.uwh_in, 30'000'000);
        CHECK_EQ(p.uwh_out, 24'000'000);
        CHECK_EQ(p.lo, 5);
        CHECK_EQ(p.hi, 7);
    }
    {   // the discharge first (a full bank emptied), then its recharge
        const view::Shown s[] = {run(9, Mode::Charge, 30'000'000), run(8, Mode::Discharge, 24'000'000)};
        CHECK_EQ(view::pair(s, 2).eff, 800);
    }
    {   // one kind only, a round trip between, a cut-short run between: no pair
        const view::Shown a[] = {run(3, Mode::Charge, 1'000'000), run(2, Mode::Charge, 1'000'000)};
        CHECK_EQ(view::pair(a, 2).eff, kNoRatio);
        const view::Shown b[] = {run(3, Mode::Discharge, 1'000'000), run(2, Mode::RoundTrip, 1'000'000), run(1, Mode::Charge, 1'000'000)};
        CHECK_EQ(view::pair(b, 3).eff, kNoRatio);
        const view::Shown c[] = {run(3, Mode::Discharge, 1'000'000), run(2, Mode::Charge, 1'000'000, true)};
        CHECK_EQ(view::pair(c, 2).eff, kNoRatio);
        CHECK_EQ(view::pair(c, 0).eff, kNoRatio);
    }
    {   // an empty run (a false start) neither counts nor breaks the stretch; the third stretch is not summed in
        const view::Shown s[] = {run(9, Mode::Discharge, 8'000'000), run(8, Mode::Discharge, 0), run(7, Mode::Charge, 10'000'000),
                                 run(6, Mode::Discharge, 50'000'000)};
        const view::Pair p = view::pair(s, 4);
        CHECK_EQ(p.eff, 800);
        CHECK_EQ(p.lo, 7);
        CHECK_EQ(p.hi, 9);
    }
}

// ---- the real runs ------------------------------------------------------------------------------------------------------------

struct Pt { std::int32_t mv, ma; };
struct Real {
    std::uint16_t id;
    Mode mode;
    Wiring wiring;
    std::uint16_t interval_s;
    std::uint32_t secs, uwh, uah;   // as recorded (the phase's own column)
    const Pt *pts;
    std::size_t n;
};
// discharge-2026-11-24-id3: discharge, moved, v1, 3170 s, recorded 30994272 uWh
constexpr Pt kId3Pts[] = {
    {18010, -2016}, {20150, -1856}, {20150, -1968}, {20120, -1308}, {20150, -1892}, {20150, -1868}, {20140, -1752}, {20150, -1824},
    {20150, -1920}, {20140, -1844}, {20140, -1840}, {20150, -2044}, {20170, -2180}, {20160, -2140}, {20150, -1872}, {20150, -1996},
    {20160, -2024}, {20150, -1856}, {20160, -2008}, {20150, -1872}, {20170, -2160}, {20160, -2108}, {20150, -1996}, {20160, -2132},
    {20140, -1540}, {20130, -1448}, {20130, -1428}, {20120, -1416}, {20120, -1408}, {20120, -1408}, {20120, -1380}, {20120, -1364},
    {20120, -1360}, {20120, -1400}, {20120, -1396}, {20120, -1400}, {20120, -1400}, {20150, -1936}, {20130, -1616}, {12750, -884},
};
constexpr Real kId3 = {3, Mode::Discharge, Wiring::Moved, 80, 3170, 30994272u, 1538604u, kId3Pts, std::size(kId3Pts)};
// charge-2026-11-24-id4: charge, same-port, v1, 3006 s, recorded 23804464 uWh
constexpr Pt kId4Pts[] = {
    {19520, 3260}, {18970, 2736}, {19770, 1360}, {19770, 1360}, {19780, 1356}, {19780, 1356}, {19780, 1356}, {19780, 1356},
    {19780, 1356}, {19780, 1356}, {19780, 1356}, {19780, 1356}, {19770, 1356}, {19780, 1356}, {19780, 1356}, {19770, 1356},
    {19770, 1356}, {19770, 1356}, {19780, 1356}, {19780, 1356}, {19770, 1356}, {19780, 1356}, {19780, 1356}, {19780, 1356},
    {19780, 1356}, {19780, 1356}, {19770, 1356}, {19770, 1356}, {19770, 1356}, {19770, 1356}, {19750, 1356}, {19760, 1356},
    {19760, 1356}, {19770, 1356}, {19770, 1356}, {19770, 1356}, {19770, 1356}, {11210, 772},
};
constexpr Real kId4 = {4, Mode::Charge, Wiring::SamePort, 80, 3006, 23804464u, 1205674u, kId4Pts, std::size(kId4Pts)};
// charge-2026-11-24-id5: charge, same-port, v1, 1592 s, recorded 13075722 uWh
constexpr Pt kId5Pts[] = {
    {19080, 3200}, {19300, 3268}, {19290, 3268}, {19290, 3268}, {19290, 3268}, {19290, 3268}, {19290, 3264}, {19310, 3152},
    {19360, 2788}, {19400, 2528}, {19430, 2340}, {19450, 2188}, {19470, 2060}, {19480, 1948}, {19500, 1848}, {19510, 1752},
    {19530, 1648}, {19540, 1552}, {19560, 1456}, {19570, 1356}, {19580, 1264}, {19600, 1164}, {19610, 1068}, {19630, 976},
    {19640, 884}, {19650, 796}, {19660, 716}, {19680, 640}, {19690, 568}, {19690, 508}, {19700, 456}, {19710, 404},
    {19720, 360}, {19720, 300}, {19730, 248}, {19740, 216}, {19740, 184}, {19750, 164}, {19750, 144}, {19760, 100},
    {19770, 0},
};
constexpr Real kId5 = {5, Mode::Charge, Wiring::SamePort, 40, 1592, 13075722u, 673138u, kId5Pts, std::size(kId5Pts)};
// charge-2026-11-25-id6: charge, same-port, v2, 3011 s, recorded 19404226 uWh
constexpr Pt kId6Pts[] = {
    {19790, 1360}, {19790, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360},
    {19780, 1360}, {19780, 1360}, {19780, 1356}, {19780, 1360}, {19780, 1360}, {19790, 1360}, {19780, 1360}, {19780, 1360},
    {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360}, {19780, 1360},
    {19790, 1360}, {19780, 1360}, {19780, 1360}, {19790, 1360}, {19790, 1348}, {19810, 1152}, {19830, 932}, {19860, 744},
    {19880, 584}, {19900, 452}, {19910, 352}, {19920, 240}, {19930, 172}, {19950, 84},
};
constexpr Real kId6 = {6, Mode::Charge, Wiring::SamePort, 80, 3011, 19404226u, 980452u, kId6Pts, std::size(kId6Pts)};

// Each curve point is an interval's average V and I: held for its interval, then the bank stops. The recorder must take the
// phase's own column, end by the rule, and land within 1.5 % of what the K2 recorded from the full-rate samples (measured
// 0.04..0.8 %: V and I are averaged apart, quantised to 10 mV / 4 mA, and the last part-interval is not a point).
void test_real_replay() {
    for (const Real *x : {&kId3, &kId4, &kId5, &kId6}) {
        Recorder r;
        r.start(x->mode, x->wiring, Config{}, x->id);
        std::uint32_t t = 0;
        for (std::size_t i = 0; i < x->n; i++)
            t = feed(r, t, t + x->interval_s * 1000u - 1000, 1000, [&](std::uint32_t) { return Sample{x->pts[i].mv, x->pts[i].ma}; }) + 1000;
        feed(r, t, t + 70'000, 1000, [](std::uint32_t) { return Sample{5000, 0}; });
        CHECK_EQ(int(r.state()), int(State::Done));
        CHECK_EQ(int(r.phase(0).end), int(End::Auto));
        const Sums s = sums_of(r);
        const bool chg = x->mode == Mode::Charge;
        NEAR(chg ? s.uwh_in : s.uwh_out, x->uwh, 0.015 * x->uwh);
        NEAR(chg ? s.uah_in : s.uah_out, x->uah, 0.015 * x->uah);
        CHECK_EQ(chg ? s.uwh_out : s.uwh_in, 0);
    }
}

struct Vol {
    k2test::RamDisk disk{32};
    k2::fs::Fs fs{disk, 32};
    Vol() { bool f = false; fs.mount_or_format(f); }
};

// A version 1 file (an older firmware's), as that firmware wrote it: 246 bytes, no flag bits.
std::vector<std::uint8_t> v1_bytes(const Record &r) {
    std::uint8_t b[kRecordBytes];
    encode(r, b);
    std::vector<std::uint8_t> v(b, b + kRecordBytesV1);
    v[2] = 1;
    v[4] &= 0x7F;
    v[16] &= 0x7F;
    v[40] &= 0x7F;
    const std::uint16_t crc = k2::ringlog::crc16(0xFFFF, {v.data(), kRecordBytesV1 - 2});
    v[kRecordBytesV1 - 2] = std::uint8_t(crc);
    v[kRecordBytesV1 - 1] = std::uint8_t(crc >> 8);
    return v;
}

Record stored(std::uint16_t id, Mode m, End e, std::uint32_t secs, std::uint32_t uwh_in, std::uint32_t uwh_out, std::uint32_t uah_in,
              std::uint32_t uah_out) {
    Record r;
    r.id = id;
    r.mode = m;
    r.wiring = m == Mode::Discharge ? Wiring::Moved : Wiring::SamePort;
    r.phases = 1;
    r.ph[0] = PhaseRec{m == Mode::Discharge ? Kind::Discharge : Kind::Charge, e, 0, secs, uwh_in, uwh_out, uah_in, uah_out};
    return r;
}

// The runs of 2026-11-24/25 as the K2 holds them, written in order with the newest kKeep kept. Ids 1..3 are version 1
// discharges from before the in/out fix (05313146): their energy sits in the "in" column. The Saved runs view shows the
// efficiency of the newest charge stretch against the discharge stretch before it.
void test_real_runs_pair() {
    Vol v;
    struct Step { std::uint16_t id; bool v1; Record r; std::uint16_t eff; };
    const Step steps[] = {
        {1, true, stored(1, Mode::Discharge, End::Manual, 68, 522'982, 0, 25'926, 0), kNoRatio},
        {2, true, stored(2, Mode::Discharge, End::Manual, 4, 0, 0, 0, 0), kNoRatio},
        {3, true, stored(3, Mode::Discharge, End::Auto, 3170, 30'994'272, 0, 1'538'604, 0), kNoRatio},
        {4, true, stored(4, Mode::Charge, End::Auto, 3006, 23'804'464, 0, 1'205'674, 0), 1302},   // 30.994 / 23.804: half charged
        {5, true, stored(5, Mode::Charge, End::Auto, 1592, 13'075'722, 0, 673'138, 0), 840},      // 30.994 / 36.880
        {6, false, stored(6, Mode::Charge, End::Auto, 3011, 19'404'226, 0, 980'452, 0), kNoRatio}, // the discharge is pruned
    };
    for (const Step &st : steps) {
        store::prune(v.fs, store::kKeep - 1);
        if (st.v1) {   // where that firmware kept it (the root); the boot moves it into /bt
            CHECK_EQ(int(v.fs.write_file(store::old_path_of(st.id).data(), v1_bytes(st.r))), int(k2::fs::Err::Ok));
            CHECK_EQ(store::migrate(v.fs), 1);
        } else {
            CHECK(store::save(v.fs, st.r));
        }
        std::uint16_t ids[store::kKeep];
        const int n = store::list(v.fs, ids, store::kKeep);
        view::Shown s[store::kKeep];
        for (int i = 0; i < n; i++) {
            Record r;
            CHECK(store::load(v.fs, ids[i], r));
            s[i] = view::shown(r, true);
        }
        const view::Pair p = view::pair(s, n);
        CHECK_EQ(p.eff, st.eff);
        if (st.id == 5) {
            CHECK_EQ(p.uwh_out, 30'994'272);
            CHECK_EQ(p.uwh_in, 36'880'186);
            CHECK_EQ(p.lo, 3);
            CHECK_EQ(p.hi, 5);
        }
    }
}

}  // namespace

int main() {
    test_constant_power();
    test_ramp();
    test_cc_cv_taper();
    test_pause_trips_end_rule();
    test_short_pause_keeps_measuring();
    test_sign_and_wiring();
    test_gap();
    test_continue_adds();
    test_round_trip_efficiency();
    test_pair();
    test_real_replay();
    test_real_runs_pair();
    return test_report("test_battery_synth");
}
