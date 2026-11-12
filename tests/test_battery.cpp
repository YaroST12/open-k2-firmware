// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the battery efficiency recorder (k2/battery/battery.hpp): the integrator with both directions, the phase-end
// rule, the round trip, the curve's decimation, the record format, the efficiency, the store and the boot after a reset.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <vector>

#include "k2/battery/battery.hpp"
#include "k2/battery/battery_wire.hpp"
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

// ---- the store and the service, on a RAM volume ---------------------------------------------------------------------------

struct Vol {
    k2test::RamDisk disk{32};
    k2::fs::Fs fs{disk, 32};
    Vol() {
        bool formatted = false;
        fs.mount_or_format(formatted);
    }
};

Record done_record(std::uint16_t id) {
    Record r;
    r.mode = Mode::Charge;
    r.state = RunState::Done;
    r.phases = 1;
    r.id = id;
    r.interval_s = Curve::kFirstIntervalS;
    r.ph[0] = PhaseRec{Kind::Charge, End::Auto, 0, 100, id * 1000u, 0, id * 200u, 0};
    return r;
}

// One file per run, the newest first; prune keeps the newest; next_id follows the highest.
void test_store_list_prune() {
    Vol v;
    std::uint16_t ids[8];
    CHECK_EQ(store::list(v.fs, ids, 8), 0);
    CHECK_EQ(store::next_id(v.fs), 1);
    for (std::uint16_t id = 1; id <= 6; id++) CHECK_EQ(store::save(v.fs, done_record(id)), true);
    CHECK_EQ(store::list(v.fs, ids, 8), 6);
    CHECK_EQ(ids[0], 6);
    CHECK_EQ(ids[5], 1);
    CHECK_EQ(store::list(v.fs, ids, 2), 2);   // the newest two
    CHECK_EQ(ids[0], 6);
    CHECK_EQ(ids[1], 5);
    store::prune(v.fs, 4);
    CHECK_EQ(store::list(v.fs, ids, 8), 4);
    CHECK_EQ(ids[3], 3);
    Record r;
    CHECK_EQ(store::load(v.fs, 3, r), true);
    CHECK_EQ(r.ph[0].uwh_in, 3000u);
    CHECK_EQ(store::load(v.fs, 1, r), false);
    CHECK_EQ(store::next_id(v.fs), 7);
    CHECK_COND(std::string(store::path_of(65535).data()) == "/bt65535.rec");
    // the runs share the root with the settings files and the other stores' directories: only bt<id>.rec counts
    const std::uint8_t b[4] = {1, 2, 3, 4};
    CHECK_EQ(int(v.fs.write_file("/usb.cfg", b)), int(k2::fs::Err::Ok));
    CHECK_EQ(int(v.fs.write_file("/bt9.rec.tmp", b)), int(k2::fs::Err::Ok));
    CHECK_EQ(int(v.fs.write_file("/bt.rec", b)), int(k2::fs::Err::Ok));
    CHECK_EQ(int(v.fs.mkdir("/bt8")), int(k2::fs::Err::Ok));
    CHECK_EQ(store::list(v.fs, ids, 8), 4);
    CHECK_EQ(ids[0], 6);
}

std::uint32_t serve(Service &s, std::uint32_t t, std::uint32_t ms, std::int32_t mv, std::int32_t ma, bool may_write = true,
                    std::uint32_t step = 1000) {
    for (std::uint32_t e = 0; e < ms; e += step, t += step) s.poll(t, mv, ma, true, may_write);
    return t;
}

// The record is written at the start and at the end, never while the phase measures; a write the caller does not allow
// waits.
void test_service_writes_at_boundaries() {
    Vol v;
    Service::Checkpoint ck{};
    Service s;
    CHECK_EQ(int(s.boot(&v.fs, &ck, false)), int(Service::Boot::Nothing));
    CHECK_EQ(int(s.start(Mode::Charge, Wiring::SamePort, false)), int(Recorder::Start::NoMonitor));
    CHECK_EQ(int(s.start(Mode::Charge, Wiring::SamePort, true)), int(Recorder::Start::Ok));
    CHECK_EQ(s.rec().id(), 1);
    std::uint32_t t = serve(s, 0, 2000, 5000, 1000, false);   // a charger test owns the flash: no write yet
    Record r;
    CHECK_EQ(store::load(v.fs, 1, r), false);
    t = serve(s, t, 1000, 5000, 1000);
    CHECK_EQ(store::load(v.fs, 1, r), true);
    CHECK_EQ(int(r.state), int(RunState::Measuring));
    v.disk.reset_counters();
    t = serve(s, t, 600'000, 5000, 1000);                     // ten minutes measuring: the flash is left alone
    CHECK_EQ(v.disk.progs + v.disk.erases, 0u);
    t = serve(s, t, 61'000, 5000, 0);                          // the end rule closes it; the record is written
    CHECK_EQ(int(s.rec().state()), int(State::Done));
    CHECK_EQ(store::load(v.fs, 1, r), true);
    CHECK_EQ(int(r.state), int(RunState::Done));
    CHECK_EQ(int(r.ph[0].end), int(End::Auto));
    CHECK_COND(r.ph[0].uwh_in > 830'000 && r.ph[0].uwh_in <= 838'000);   // 5 W x 603 s
    CHECK_EQ(s.write_failed(), false);
    (void)t;
}

// Every new run is a new file; only the newest kKeep stay.
void test_service_keeps_newest() {
    Vol v;
    Service::Checkpoint ck{};
    Service s;
    s.boot(&v.fs, &ck, false);
    std::uint32_t t = 0;
    for (int i = 0; i < 6; i++) {
        CHECK_EQ(int(s.start(Mode::Discharge, Wiring::SamePort, true)), int(Recorder::Start::Ok));
        t = serve(s, t, 10'000, 5000, -500);
        s.rec().finish();
        t = serve(s, t, 1000, 5000, 0);
    }
    std::uint16_t ids[8];
    CHECK_EQ(store::list(v.fs, ids, 8), store::kKeep);
    CHECK_EQ(ids[0], 6);
    Record r;
    CHECK_EQ(store::load(v.fs, 6, r), true);
    CHECK_EQ(int(r.ph[0].end), int(End::Manual));
    CHECK_COND(r.ph[0].uwh_out > 0);
}

// A warm reset keeps the SRAM checkpoint: the run goes on with its totals (the gap counted, not integrated).
void test_service_warm_resume() {
    Vol v;
    Service::Checkpoint ck{};
    std::uint64_t before = 0;
    {
        Service s;
        s.boot(&v.fs, &ck, false);
        s.start(Mode::Charge, Wiring::SamePort, true);
        serve(s, 0, 600'500, 5000, 1000);
        before = s.rec().phase(0).uj_in;
    }
    Service s;
    CHECK_EQ(int(s.boot(&v.fs, &ck, true)), int(Service::Boot::ResumedRam));
    CHECK_EQ(int(s.rec().state()), int(State::Measuring));
    CHECK_EQ(s.rec().resumes(), 1);
    CHECK_EQ(s.rec().phase(0).gaps, 1);
    CHECK_COND(s.rec().phase(0).uj_in <= before && s.rec().phase(0).uj_in + 5'000'000 >= before);   // within the last second
    // the same SRAM after a cold boot (or a corrupted checkpoint) does not count
    Service c;
    ck.crc ^= 1;
    CHECK_EQ(int(c.boot(&v.fs, &ck, true)), int(Service::Boot::Lost));
}

// A run that ended is not resumed by a later warm reset (the checkpoint follows the end, even between its 1 s ticks).
void test_service_warm_after_end() {
    Vol v;
    Service::Checkpoint ck{};
    {
        Service s;
        s.boot(&v.fs, &ck, false);
        s.start(Mode::Discharge, Wiring::SamePort, true);
        std::uint32_t t = serve(s, 0, 10'500, 5000, -1000, true, 500);   // the last tick at 10 s
        s.rec().finish();
        s.poll(t + 100, 5000, 0, true, true);                             // one poll, before the next tick
    }
    Service s;
    CHECK_EQ(int(s.boot(&v.fs, &ck, true)), int(Service::Boot::Nothing));
    CHECK_EQ(int(s.rec().state()), int(State::Idle));
}

// A cold boot cannot resume a measuring phase: its totals since the last write are gone. The phase is marked Lost, the run
// closed and written; a round trip waiting between its phases waits again.
void test_service_cold_boot() {
    Vol v;
    Service::Checkpoint ck{};
    {
        Service s;
        s.boot(&v.fs, &ck, false);
        s.start(Mode::Charge, Wiring::SamePort, true);
        serve(s, 0, 60'000, 5000, 1000);
    }
    {
        Service s;
        CHECK_EQ(int(s.boot(&v.fs, &ck, false)), int(Service::Boot::Lost));
        CHECK_EQ(int(s.rec().state()), int(State::Done));
        serve(s, 0, 1000, 0, 0);
        Record r;
        CHECK_EQ(store::load(v.fs, 1, r), true);
        CHECK_EQ(int(r.state), int(RunState::Done));
        CHECK_EQ(int(r.ph[0].end), int(End::Lost));
    }
    {
        Service s;   // nothing open any more
        CHECK_EQ(int(s.boot(&v.fs, &ck, false)), int(Service::Boot::Nothing));
        CHECK_EQ(int(s.start(Mode::RoundTrip, Wiring::Moved, true)), int(Recorder::Start::Ok));
        CHECK_EQ(s.rec().id(), 2);
        std::uint32_t t = serve(s, 0, 600'000, 5000, 1000);
        t = serve(s, t, 61'000, 5000, 0);
        CHECK_EQ(int(s.rec().state()), int(State::Between));
        serve(s, t, 2000, 0, 0);
    }
    Service s;
    CHECK_EQ(int(s.boot(&v.fs, &ck, false)), int(Service::Boot::ResumedBetween));
    CHECK_EQ(int(s.rec().state()), int(State::Between));
    CHECK_EQ(s.rec().id(), 2);
    // the bank went to the input port: its outflow held 5 s starts the discharge (the power cut was the swap)
    std::uint32_t t = serve(s, 0, 7000, 5000, 1000);
    CHECK_EQ(int(s.rec().state()), int(State::Measuring));
    CHECK_EQ(s.rec().phases(), 2);
    (void)t;
}

// Without a volume (or a full one) the recorder still measures; the write failure shows.
void test_service_no_volume() {
    Service::Checkpoint ck{};
    Service s;
    CHECK_EQ(int(s.boot(nullptr, &ck, false)), int(Service::Boot::Nothing));
    CHECK_EQ(int(s.start(Mode::Charge, Wiring::SamePort, true)), int(Recorder::Start::Ok));
    CHECK_EQ(s.rec().id(), 1);
    serve(s, 0, 5000, 5000, 1000);
    CHECK_EQ(int(s.rec().state()), int(State::Measuring));
    CHECK_COND(s.rec().phase(0).uj_in > 0);
}

// ---- the remote protocol (Cmd::Battery) -----------------------------------------------------------------------------------

struct Rsp {
    std::uint8_t b[64]{};
    std::size_t n = 0;
    std::uint16_t u16(std::size_t i) const { return std::uint16_t(b[i] | b[i + 1] << 8); }
    std::uint32_t u32(std::size_t i) const { return u16(i) | std::uint32_t(u16(i + 2)) << 16; }
};

Rsp call(Service &s, std::initializer_list<std::uint8_t> req, bool monitor = true) {
    const std::vector<std::uint8_t> p(req);
    CHECK_EQ(p.size(), wire::req_len(p[0]));
    Rsp r;
    r.n = wire::handle(s, monitor, p, std::span<std::uint8_t>(r.b, 58));
    return r;
}

void test_wire() {
    Vol v;
    Service::Checkpoint ck{};
    Service s;
    s.boot(&v.fs, &ck, false);
    CHECK_EQ(wire::req_len(8), 0u);
    Rsp r = call(s, {wire::kStatus});
    CHECK_EQ(r.n, wire::kStatusBytes);
    CHECK_EQ(r.b[0], 0);                     // idle
    CHECK_EQ(r.u16(8), 50);                  // end_ma
    CHECK_EQ(r.u16(10), 60);                 // end_s
    CHECK_EQ(r.u16(36), kNoRatio);
    CHECK_EQ(r.b[38], 0);                    // runs stored
    r = call(s, {wire::kStart, 0, 0}, false);
    CHECK_EQ(r.b[4], int(wire::Result::NoMonitor));
    CHECK_EQ(r.b[0], 0);
    r = call(s, {wire::kStart, 3, 0});
    CHECK_EQ(r.b[4], int(wire::Result::BadArg));
    r = call(s, {wire::kConfig, 5, 0, 60, 0, 0});           // 5 mA is under the floor
    CHECK_EQ(r.b[4], int(wire::Result::BadArg));
    r = call(s, {wire::kConfig, 100, 0, 30, 0, 1});
    CHECK_EQ(r.b[4], int(wire::Result::Ok));
    CHECK_EQ(r.u16(8), 100);
    CHECK_EQ(r.u16(10), 30);
    CHECK_EQ(r.b[39], 1);                    // the next run's wiring: moved
    r = call(s, {wire::kEndPhase});
    CHECK_EQ(r.b[4], int(wire::Result::NotNow));
    r = call(s, {wire::kStart, 2, 0});       // round trip, same port
    CHECK_EQ(r.b[4], int(wire::Result::Ok));
    CHECK_EQ(r.b[0], 1);
    CHECK_EQ(r.b[1], 2);
    CHECK_EQ(r.b[2], 0);
    CHECK_EQ(r.b[3], 1);
    CHECK_EQ(r.u16(6), 1);
    CHECK_EQ(r.b[5] & wire::kFlagMonitor, wire::kFlagMonitor);
    CHECK_EQ(call(s, {wire::kStart, 0, 0}).b[4], int(wire::Result::Busy));
    std::uint32_t t = serve(s, 0, 3600'000 + 1000, 5000, 2000);   // 10 Wh in
    r = call(s, {wire::kStatus});
    CHECK_COND(r.u32(20) >= 9'990'000 && r.u32(20) <= 10'000'000);
    CHECK_COND(r.u32(12) >= 3599 && r.u32(12) <= 3601);
    CHECK_EQ(call(s, {wire::kNext}).b[4], int(wire::Result::NotNow));
    r = call(s, {wire::kEndPhase});
    CHECK_EQ(r.b[0], 2);                     // between
    r = call(s, {wire::kNext});
    CHECK_EQ(r.b[0], 1);
    CHECK_EQ(r.b[3], 2);
    t = serve(s, t, 3600'000 + 1000, 5000, -1600);                 // 8 Wh out
    r = call(s, {wire::kStatus});
    CHECK_COND(r.u16(36) >= 795 && r.u16(36) <= 800);              // so far
    CHECK_EQ(r.b[5] & wire::kFlagFinalEff, 0);
    r = call(s, {wire::kFinish});
    CHECK_EQ(r.b[0], 3);
    CHECK_EQ(r.b[5] & wire::kFlagFinalEff, wire::kFlagFinalEff);
    CHECK_EQ(r.b[5] & wire::kFlagUnwritten, wire::kFlagUnwritten);
    serve(s, t, 1000, 5000, 0);
    r = call(s, {wire::kStatus});
    CHECK_EQ(r.b[5] & wire::kFlagUnwritten, 0);
    CHECK_EQ(r.b[38], 1);
    r = call(s, {wire::kList});
    CHECK_EQ(r.b[0], 1);
    CHECK_EQ(r.u16(1), 1);
    // the stored record, chunked: off(2) total(2) n(1) data
    std::uint8_t file[kRecordBytes];
    std::size_t got = 0;
    for (std::uint16_t off = 0;;) {
        r = call(s, {wire::kRead, 1, 0, std::uint8_t(off), std::uint8_t(off >> 8)});
        CHECK_EQ(r.u16(0), off);
        CHECK_EQ(r.u16(2), kRecordBytes);
        const std::size_t n = r.b[4];
        CHECK_EQ(r.n, 5 + n);
        if (n == 0) break;
        std::memcpy(file + got, r.b + 5, n);
        got += n;
        off = std::uint16_t(off + n);
    }
    CHECK_EQ(got, kRecordBytes);
    Record rec;
    CHECK_EQ(decode(std::span<const std::uint8_t>(file, got), rec), true);
    CHECK_EQ(has_efficiency(rec), true);
    // id 0 = the run in RAM (the same run here); an unknown id has no bytes
    r = call(s, {wire::kRead, 0, 0, 0, 0});
    CHECK_EQ(r.u16(2), kRecordBytes);
    CHECK_EQ(std::memcmp(r.b + 5, file, wire::kChunk), 0);
    r = call(s, {wire::kRead, 9, 0, 0, 0});
    CHECK_EQ(r.u16(2), 0);
    CHECK_EQ(r.b[4], 0);
    // K2_DUMP: the record for the Python decoder's test (tests/test_battery_rec.py)
    if (const char *dir = std::getenv("K2_DUMP")) {
        const std::string path = std::string(dir) + "/battery_rec.hex";
        if (FILE *f = std::fopen(path.c_str(), "w")) {
            for (std::size_t i = 0; i < got; i++) std::fprintf(f, "%02x", file[i]);
            std::fputc('\n', f);
            std::fclose(f);
        }
    }
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
    test_store_list_prune();
    test_service_writes_at_boundaries();
    test_service_keeps_newest();
    test_service_warm_resume();
    test_service_warm_after_end();
    test_service_cold_boot();
    test_service_no_volume();
    test_wire();
    return test_report("test_battery");
}
