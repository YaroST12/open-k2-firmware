// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the battery efficiency recorder (k2/battery/battery.hpp): the integrator with both directions, the phase-end
// rule, the round trip, the curve's decimation, the record format, the efficiency, the store and the boot after a reset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "k2/battery/battery.hpp"
#include "k2/core/fs.hpp"
#include "ramdisk.hpp"
#include "test_util.h"

using namespace k2::battery;

namespace {

constexpr std::uint64_t kUjPerWh = 3'600'000'000ull, kUcPerAh = 3'600'000'000ull;

// 5 V at 1 A forward for an hour in 1 s steps: 5 Wh and 1 Ah, all forward.
void test_integrate_forward_hour() {
    Flow f;
    for (int i = 0; i < 3600; i++) integrate(f, 5000, 1000, 5000, 1000, 1000);
    CHECK_EQ(f.uj_fwd, 5 * kUjPerWh);
    CHECK_EQ(f.uc_fwd, 1 * kUcPerAh);
    CHECK_EQ(f.uj_rev, 0u);
    CHECK_EQ(f.uc_rev, 0u);
}

// The same backwards lands in the reverse counters, never subtracted from the forward ones.
void test_integrate_reverse() {
    Flow f;
    integrate(f, 5000, 1000, 5000, 1000, 1000);    // 5 J forward
    integrate(f, 9000, -2000, 9000, -2000, 500);   // 18 W x 0.5 s = 9 J back
    CHECK_EQ(f.uj_fwd, 5'000'000u);
    CHECK_EQ(f.uj_rev, 9'000'000u);
    CHECK_EQ(f.uc_fwd, 1'000'000u);
    CHECK_EQ(f.uc_rev, 1'000'000u);
}

// A step from +1 A to -1 A at 5 V crosses zero half way: a quarter of the rectangle each way (the triangles), nothing cancels.
void test_integrate_zero_crossing() {
    Flow f;
    integrate(f, 5000, 1000, 5000, -1000, 1000);
    CHECK_EQ(f.uj_fwd, 1'250'000u);   // 5 W x 0.5 s / 2
    CHECK_EQ(f.uj_rev, 1'250'000u);
    CHECK_EQ(f.uc_fwd, 250'000u);     // 1 A x 0.5 s / 2
    CHECK_EQ(f.uc_rev, 250'000u);
    // off centre: +3 A to -1 A crosses at 3/4 of the step
    Flow g;
    integrate(g, 4000, 3000, 4000, -1000, 400);
    CHECK_EQ(g.uc_fwd, 450'000u);     // 3 A x 300 ms / 2
    CHECK_EQ(g.uc_rev, 50'000u);      // 1 A x 100 ms / 2
}

void test_deadband() {
    CHECK_EQ(deadband(19), 0);
    CHECK_EQ(deadband(-19), 0);
    CHECK_EQ(deadband(20), 20);
    CHECK_EQ(deadband(-25), -25);
}

// Feeds (mv, ma) every `step` ms from t for `ms`; returns the time after.
std::uint32_t feed(Recorder &r, std::uint32_t t, std::uint32_t ms, std::int32_t mv, std::int32_t ma, std::uint32_t step = 100) {
    for (std::uint32_t e = 0; e < ms; e += step, t += step) r.sample(t, mv, ma, true);
    return t;
}

// Charge: forward current is energy into the bank. The phase ends by itself once |I| stayed under 50 mA for 60 s, and its
// time stops where the quiet began.
void test_charge_auto_end() {
    Recorder r;
    CHECK_EQ(int(r.start(Mode::Charge, Wiring::SamePort, Config{}, 7)), int(Recorder::Start::Ok));
    CHECK_EQ(int(r.state()), int(State::Measuring));
    CHECK_EQ(r.dirty(), true);
    r.clean();
    std::uint32_t t = feed(r, 0, 3601'000, 5000, 2000, 1000);       // 10 W for an hour (the first sample primes)
    t = feed(r, t, 59'000, 5000, 30, 1000);                          // under the threshold, not long enough yet
    CHECK_EQ(int(r.state()), int(State::Measuring));
    t = feed(r, t, 2'000, 5000, 30, 1000);
    CHECK_EQ(int(r.state()), int(State::Done));
    CHECK_EQ(r.dirty(), true);
    const Phase &p = r.phase(0);
    CHECK_EQ(int(p.end), int(End::Auto));
    CHECK_EQ(p.ms, 3601'000u);                                        // to the first quiet sample: the tail is not phase time
    CHECK_COND(p.uj_in >= 10 * kUjPerWh && p.uj_in < 10 * kUjPerWh + kUjPerWh / 100);   // + the 30 mA tail
    CHECK_EQ(p.uj_out, 0u);
    CHECK_COND(p.uc_in >= 2 * kUcPerAh);
    (void)t;
}

// The end rule waits for the current to reach the threshold first: a phase started before the bank is plugged in does not end.
void test_end_rule_needs_flow_first() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 1);
    feed(r, 0, 600'000, 5000, 0, 1000);
    CHECK_EQ(int(r.state()), int(State::Measuring));
    // dips shorter than the window do not end it
    std::uint32_t t = feed(r, 600'000, 10'000, 5000, 500, 1000);
    t = feed(r, t, 30'000, 5000, 10, 1000);
    t = feed(r, t, 10'000, 5000, 500, 1000);
    t = feed(r, t, 30'000, 5000, 10, 1000);
    CHECK_EQ(int(r.state()), int(State::Measuring));
    CHECK_EQ(r.end_phase(), true);   // by hand
    CHECK_EQ(int(r.state()), int(State::Done));
    CHECK_EQ(int(r.phase(0).end), int(End::Manual));
    CHECK_EQ(r.end_phase(), false);
    (void)t;
}

// Same port, discharge: the bank on the output side feeds back, reverse current = out of the bank.
void test_discharge_same_port_reverse() {
    Recorder r;
    r.start(Mode::Discharge, Wiring::SamePort, Config{}, 2);
    feed(r, 0, 3600'000 + 1000, 5000, -1000, 1000);
    const Phase &p = r.phase(0);
    CHECK_EQ(int(p.kind), int(Kind::Discharge));
    CHECK_EQ(p.uj_out, 5 * kUjPerWh);
    CHECK_EQ(p.uc_out, 1 * kUcPerAh);
    CHECK_EQ(p.uj_in, 0u);
}

// Moved, discharge: the bank sits on the input side, forward current = out of the bank.
void test_discharge_moved_forward() {
    Recorder r;
    r.start(Mode::Discharge, Wiring::Moved, Config{}, 3);
    feed(r, 0, 3600'000 + 1000, 9000, 1000, 1000);
    CHECK_EQ(r.phase(0).uj_out, 9 * kUjPerWh);
    CHECK_EQ(r.phase(0).uj_in, 0u);
}

// Gaps: a step longer than kGapMs is counted, not integrated; offline samples prime again.
void test_gap_not_integrated() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 4);
    r.sample(0, 5000, 1000, true);
    r.sample(1000, 5000, 1000, true);         // 5 J
    r.sample(11000, 5000, 1000, true);        // 10 s gap: not integrated
    r.sample(12000, 5000, 1000, false);       // offline
    r.sample(13000, 5000, 1000, true);        // primes
    r.sample(14000, 5000, 1000, true);        // 5 J
    CHECK_EQ(r.phase(0).uj_in, 10'000'000u);
    CHECK_EQ(r.phase(0).gaps, 1u);
}

// Idle noise inside the dead band counts nothing.
void test_deadband_noise_counts_nothing() {
    Recorder r;
    r.start(Mode::Charge, Wiring::SamePort, Config{}, 5);
    std::uint32_t t = 0;
    for (int i = 0; i < 1000; i++, t += 100) r.sample(t, 5000, i % 2 ? 15 : -15, true);
    CHECK_EQ(r.phase(0).uj_in + r.phase(0).uj_out, 0u);
}

// Round trip, same port: charge (forward in), the charger leaves (quiet), a load takes from the bank (reverse); the discharge
// starts by itself once the outflow held 5 s, and those 5 s count. Efficiency = Wh out / Wh in.
void test_round_trip_same_port() {
    Recorder r;
    r.start(Mode::RoundTrip, Wiring::SamePort, Config{.end_ma = 100, .end_s = 30}, 9);
    std::uint32_t t = feed(r, 0, 3601'000, 5000, 2000, 1000);   // 10 Wh in
    t = feed(r, t, 31'000, 5000, 0, 1000);                       // full: quiet 30 s
    CHECK_EQ(int(r.state()), int(State::Between));
    CHECK_EQ(int(r.phase(0).end), int(End::Auto));
    r.clean();
    t = feed(r, t, 3'000, 5000, -1000, 1000);                    // outflow, not held yet
    CHECK_EQ(int(r.state()), int(State::Between));
    t = feed(r, t, 1'000, 5000, 0, 1000);                        // stopped: the 3 s are dropped
    CHECK_EQ(r.phase(1).uj_out, 0u);
    t = feed(r, t, 6'000, 5000, -1600, 1000);                    // 8 W out, held 5 s
    CHECK_EQ(int(r.state()), int(State::Measuring));
    CHECK_EQ(r.phases(), 2);
    CHECK_EQ(r.dirty(), true);
    t = feed(r, t, 3600'000 - 6'000, 5000, -1600, 1000);
    t = feed(r, t, 31'000, 5000, 0, 1000);
    CHECK_EQ(int(r.state()), int(State::Done));
    const Phase &d = r.phase(1);
    CHECK_EQ(int(d.kind), int(Kind::Discharge));
    // the hour of 8 W counted from the first outflow sample (the first step ramps from 0 A)
    CHECK_COND(d.uj_out > 8 * kUjPerWh - 8 * kUjPerWh / 1000 && d.uj_out <= 8 * kUjPerWh);
    Record rec;
    r.to_record(rec);
    CHECK_EQ(has_efficiency(rec), true);
    const Sums s = sums(rec);
    const std::uint16_t eff = ratio_permille(s.uwh_out, s.uwh_in);
    CHECK_COND(eff >= 798 && eff <= 800);    // 8 / 10, the ramp step a hair under
    (void)t;
}

// Moved wiring: after the charge the outflow is forward, and it only starts the discharge once VBUS went away (the bank was
// unplugged to move it); a top-up trickle with the charger still on does not.
void test_round_trip_moved_needs_unplug() {
    Recorder r;
    r.start(Mode::RoundTrip, Wiring::Moved, Config{}, 10);
    std::uint32_t t = feed(r, 0, 60'000, 5000, 1000, 1000);
    CHECK_EQ(r.end_phase(), true);                               // by hand, still charging
    CHECK_EQ(int(r.state()), int(State::Between));
    t = feed(r, t, 20'000, 5000, 1000, 1000);                    // forward flow, no unplug: no discharge
    CHECK_EQ(int(r.state()), int(State::Between));
    t = feed(r, t, 2'000, 0, 0, 1000);                           // unplugged
    t = feed(r, t, 7'000, 5000, 1000, 1000);                     // on the input side now, feeding a load
    CHECK_EQ(int(r.state()), int(State::Measuring));
    CHECK_COND(r.phase(1).uj_out > 0);
    CHECK_EQ(r.phase(1).uj_in, 0u);
    (void)t;
}

// OK in Between starts the discharge at once; finish() in Between closes the run with the charge alone (no efficiency).
void test_round_trip_manual() {
    Recorder a;
    a.start(Mode::RoundTrip, Wiring::SamePort, Config{}, 11);
    feed(a, 0, 10'000, 5000, 1000, 1000);
    a.end_phase();
    CHECK_EQ(a.next(), true);
    CHECK_EQ(int(a.state()), int(State::Measuring));
    CHECK_EQ(a.next(), false);
    Recorder b;
    b.start(Mode::RoundTrip, Wiring::SamePort, Config{}, 12);
    feed(b, 0, 10'000, 5000, 1000, 1000);
    b.end_phase();
    CHECK_EQ(b.finish(), true);
    CHECK_EQ(int(b.state()), int(State::Done));
    Record rec;
    b.to_record(rec);
    CHECK_EQ(rec.phases, 1);
    CHECK_EQ(has_efficiency(rec), false);
    CHECK_EQ(int(b.start(Mode::Charge, Wiring::SamePort, Config{}, 13)), int(Recorder::Start::Ok));   // a new run replaces it
    CHECK_EQ(int(b.start(Mode::Charge, Wiring::SamePort, Config{}, 14)), int(Recorder::Start::Busy));
    CHECK_EQ(int(Recorder{}.start(Mode::Charge, Wiring::SamePort, Config{.end_ma = 5}, 1)), int(Recorder::Start::BadArg));
}

// The curve: 10 s points until 60 are full, then pairs merge and the interval doubles, so a long run always fits.
void test_curve_decimation() {
    Curve c;
    for (int i = 0; i < 60; i++) c.add(10'000, 5000 + 100 * i, 1000);
    CHECK_EQ(c.n, 60);
    CHECK_EQ(c.interval_s, 10);
    c.mark_split();                          // the discharge would start at point 60
    CHECK_EQ(c.split, 60);
    c.add(10'000, 9000, -2000);              // 10 s more: the buffer folds to 30 points of 20 s; the open point waits for 20 s
    CHECK_EQ(c.n, 30);
    CHECK_EQ(c.interval_s, 20);
    CHECK_EQ(c.split, 30);
    std::int32_t mv, ma;
    c.point(0, mv, ma);
    CHECK_EQ(mv, 5050);                      // (5000 + 5100) / 2
    CHECK_EQ(ma, 1000);
    c.add(10'000, 9000, -2000);
    CHECK_EQ(c.n, 31);
    c.point(30, mv, ma);
    CHECK_EQ(mv, 9000);
    CHECK_EQ(ma, -2000);
    // ten hours in 1 s steps still fit
    Curve d;
    for (int i = 0; i < 36000; i++) d.add(1000, 12000, 3000);
    CHECK_COND(d.n <= Curve::kPoints && d.n > Curve::kPoints / 2);
    CHECK_EQ(std::uint32_t(d.n) * d.interval_s <= 36000u, true);
    CHECK_EQ(d.interval_s, 640);
}

// Packing keeps 10 mV and 4 mA and clamps at the field limits.
void test_curve_pack() {
    std::uint8_t p[3];
    std::int32_t mv, ma;
    Curve::pack(p, 20010, -4996);
    Curve::unpack(p, mv, ma);
    CHECK_EQ(mv, 20010);
    CHECK_EQ(ma, -4996);
    Curve::pack(p, 99999, 99999);
    Curve::unpack(p, mv, ma);
    CHECK_EQ(mv, 40950);
    CHECK_EQ(ma, 8188);
}

// The record: every field round-trips, the CRC catches a flipped bit, a short or foreign buffer is refused.
void test_record_round_trip() {
    Record r;
    r.mode = Mode::RoundTrip;
    r.wiring = Wiring::Moved;
    r.state = RunState::Between;
    r.phases = 2;
    r.resumes = 3;
    r.id = 513;
    r.cfg = Config{.end_ma = 120, .end_s = 300};
    r.interval_s = 40;
    r.points = 2;
    r.split = 1;
    r.ph[0] = PhaseRec{Kind::Charge, End::Auto, 2, 7200, 51'000'000, 12, 10'200'000, 3};
    r.ph[1] = PhaseRec{Kind::Discharge, End::Lost, 0, 60, 0, 44'000'000, 0, 8'800'000};
    Curve::pack(&r.pts[0], 5000, 2000);
    Curve::pack(&r.pts[3], 9000, -1500);
    std::uint8_t buf[kRecordBytes];
    CHECK_EQ(encode(r, buf), kRecordBytes);
    CHECK_EQ(buf[0], 'B');
    CHECK_EQ(buf[1], 'R');
    Record d;
    CHECK_EQ(decode(buf, d), true);
    CHECK_EQ(int(d.mode), int(Mode::RoundTrip));
    CHECK_EQ(int(d.wiring), int(Wiring::Moved));
    CHECK_EQ(int(d.state), int(RunState::Between));
    CHECK_EQ(d.phases, 2);
    CHECK_EQ(d.resumes, 3);
    CHECK_EQ(d.id, 513);
    CHECK_EQ(d.cfg.end_ma, 120);
    CHECK_EQ(d.cfg.end_s, 300);
    CHECK_EQ(d.interval_s, 40);
    CHECK_EQ(d.points, 2);
    CHECK_EQ(d.split, 1);
    CHECK_EQ(int(d.ph[0].end), int(End::Auto));
    CHECK_EQ(d.ph[0].gaps, 2);
    CHECK_EQ(d.ph[0].secs, 7200u);
    CHECK_EQ(d.ph[0].uwh_in, 51'000'000u);
    CHECK_EQ(d.ph[0].uwh_out, 12u);
    CHECK_EQ(d.ph[0].uah_in, 10'200'000u);
    CHECK_EQ(int(d.ph[1].kind), int(Kind::Discharge));
    CHECK_EQ(int(d.ph[1].end), int(End::Lost));
    CHECK_EQ(d.ph[1].uwh_out, 44'000'000u);
    CHECK_EQ(d.ph[1].uah_out, 8'800'000u);
    CHECK_EQ(std::memcmp(d.pts.data(), r.pts.data(), 6), 0);
    buf[20] ^= 1;
    CHECK_EQ(decode(buf, d), false);
    buf[20] ^= 1;
    CHECK_EQ(decode(std::span<const std::uint8_t>(buf, kRecordBytes - 1), d), false);
    buf[2] = 9;   // a version this build does not know
    CHECK_EQ(decode(buf, d), false);
    CHECK_EQ(encode(r, std::span<std::uint8_t>(buf, 10)), 0u);
}

// Efficiency: tenths of a percent, rounded; nothing in = no ratio.
void test_ratio() {
    CHECK_EQ(ratio_permille(873, 1000), 873);
    CHECK_EQ(ratio_permille(2, 3), 667);
    CHECK_EQ(ratio_permille(5, 0), kNoRatio);
    CHECK_EQ(ratio_permille(12'000'000'000ull, 10'000'000'000ull), 1200);
}

}  // namespace

int main() {
    test_integrate_forward_hour();
    test_integrate_reverse();
    test_integrate_zero_crossing();
    test_deadband();
    test_charge_auto_end();
    test_end_rule_needs_flow_first();
    test_discharge_same_port_reverse();
    test_discharge_moved_forward();
    test_gap_not_integrated();
    test_deadband_noise_counts_nothing();
    test_round_trip_same_port();
    test_round_trip_moved_needs_unplug();
    test_round_trip_manual();
    test_curve_decimation();
    test_curve_pack();
    test_record_round_trip();
    test_ratio();
    return test_report("test_battery");
}
