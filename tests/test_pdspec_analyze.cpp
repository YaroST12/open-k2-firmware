// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the timing analysis (k2/pdspec/analyze.hpp) over transition captures: synthetic traces with hand-worked expected
// values, the capture stream round trip (encode -> decode -> analyze, the path a recorded fixture takes) and the live Hard Reset / detach
// tracker. Recorded captures from the hardware go under tests/fixtures/pdspec/ and are read by the fixture cases at the end.
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "k2/pd/transcap_calc.hpp"
#include "k2/pdspec/analyze.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdspec;
using transcap::Mark;
using transcap::Marker;
using transcap::Sample;

namespace {

constexpr std::uint32_t kT0 = 1'000'000;   // the Request's TX start in every synthetic trace (us)

// A synthetic capture: one sample per millisecond from `from_ms` to `to_ms` (relative to kT0), VBUS from `f(ms)`.
struct Trace {
    std::vector<Sample> s;
    std::vector<Marker> m;
    template <class F>
    void fill(int from_ms, int to_ms, F f) {
        for (int ms = from_ms; ms <= to_ms; ++ms)
            s.push_back({kT0 + std::uint32_t(ms * 1000), std::uint16_t(f(ms)), 0, 0});
    }
    void mark(int ms, Mark code) { m.push_back({kT0 + std::uint32_t(ms * 1000), std::uint8_t(code), 0, 0}); }
    transcap::View view() const { return {s, m}; }
};

Edges edges(int accept_ms, int psrdy_ms) {
    Edges e;
    e.req_us = kT0;
    e.accept_us = kT0 + std::uint32_t(accept_ms * 1000);
    e.psrdy_us = kT0 + std::uint32_t(psrdy_ms * 1000);
    e.accept = true;
    e.psrdy = psrdy_ms >= 0;
    return e;
}

// 5 V -> 9 V, clean: Accept at 10 ms, VBUS starts 30 ms after it and rises 100 mV/ms to 9 V (40 ms), PS_RDY at 130 ms.
void clean_up_step() {
    Trace t;
    t.fill(-50, 1500, [](int ms) { return ms < 40 ? 5000 : ms < 80 ? 5000 + (ms - 40) * 100 : 9000; });
    const TransitionOut o = analyze_transition(t.view(), {edges(10, 130), 5000, 9000});
    // Departure: a tenth of the step (400 mV) is first exceeded at 45 ms (500 mV), 35 ms after the Accept.
    CHECK_COND(o.start.seen);
    CHECK_EQ(o.start.us, 35000u);
    CHECK_EQ(o.start.gap_us, 1000u);
    // vSrcNew of 9 V is 8550..9450 mV: 8600 mV at 76 ms is the first sample inside, and VBUS stays there: 66 ms after the Accept.
    CHECK_COND(o.band.seen);
    CHECK_EQ(o.band.us, 66000u);
    CHECK_COND(o.psrdy_known);
    CHECK_EQ(o.vbus_at_psrdy_mv, 9000);
    CHECK_EQ(o.peak_mv, 9000);
    CHECK_COND(o.final_valid);
    CHECK_EQ(o.final_mv, 9000);
    CHECK_EQ(o.max_gap_us, 1000u);
}

// 20 V -> 5 V on an unloaded source: PS_RDY 90 ms after the Accept while VBUS falls at 10 V/s: VBUS at PS_RDY is 19300 mV, far
// above 5.25 V, and the band is not reached inside the window (the fall needs 1475 ms from 30 ms after the Accept).
void slow_fall_with_early_ps_rdy() {
    Trace t;
    t.fill(-50, 1500, [](int ms) { return ms < 40 ? 20000 : 20000 - (ms - 40) * 10; });
    const TransitionOut o = analyze_transition(t.view(), {edges(10, 100), 20000, 5000});
    CHECK_COND(o.psrdy_known);
    CHECK_EQ(o.vbus_at_psrdy_mv, 19400);
    CHECK_COND(!o.band.seen);
    CHECK_EQ(o.last_mv, 5400);          // 1500 ms: 20000 - 1460 * 10
    CHECK_EQ(o.last_us, 1490000u);      // the window ends 1490 ms after the Accept
    CHECK_COND(!o.final_valid);
    CHECK_EQ(o.peak_mv, 5400);          // a down-step reports the lowest VBUS
    // Departure: a tenth of 15 V is 1500 mV, first exceeded at 191 ms (1510 mV down): 181 ms after the Accept.
    CHECK_EQ(o.start.us, 181000u);
}

// 5 V -> 9 V that overshoots to 10.1 V for a few ms and settles: the peak is reported; the band entry is the sample after which VBUS
// stays inside (the overshoot leaves the band, so the entry is after it).
void overshoot() {
    Trace t;
    t.fill(-50, 1500, [](int ms) {
        if (ms < 40) return 5000;
        if (ms < 80) return 5000 + (ms - 40) * 100;
        if (ms < 85) return 10100;
        return 9000;
    });
    const TransitionOut o = analyze_transition(t.view(), {edges(10, 200), 5000, 9000});
    CHECK_EQ(o.peak_mv, 10100);
    CHECK_EQ(o.band.us, 75000u);   // 85 ms, the first sample back inside
}

// A 20 ms hole in the sampling (a long paint) right where VBUS departs: the departure is seen at the first sample after the hole, and
// the gap before it is its uncertainty.
void gap_is_the_uncertainty() {
    Trace t;
    t.fill(-50, 39, [](int) { return 5000; });
    t.fill(60, 1500, [](int ms) { return ms < 80 ? 5000 + (ms - 40) * 100 : 9000; });
    const TransitionOut o = analyze_transition(t.view(), {edges(10, 130), 5000, 9000});
    CHECK_EQ(o.start.us, 50000u);     // 60 ms
    CHECK_EQ(o.start.gap_us, 21000u); // 39 ms -> 60 ms
    CHECK_EQ(o.max_gap_us, 21000u);
}

// No PS_RDY (the source took the step without one): VBUS at PS_RDY is unknown, the rest is measured.
void no_ps_rdy() {
    Trace t;
    t.fill(-50, 1500, [](int ms) { return ms < 40 ? 5000 : ms < 80 ? 5000 + (ms - 40) * 100 : 9000; });
    const TransitionOut o = analyze_transition(t.view(), {edges(10, -1), 5000, 9000});
    CHECK_COND(!o.psrdy_known);
    CHECK_COND(o.band.seen);
}

// The edges from the capture's own markers: the first Request at or after the start, then its Accept and PS_RDY.
void edges_from_markers() {
    Trace t;
    t.fill(-50, 300, [](int) { return 5000; });
    t.mark(-20, Mark::TxRequest);   // an older Request: before the start, not this transition's
    t.mark(-10, Mark::Accept);
    t.mark(0, Mark::TxRequest);
    t.mark(12, Mark::Accept);
    t.mark(140, Mark::PsRdy);
    const Edges e = find_edges(t.view(), kT0 - 1000);
    CHECK_EQ(e.req_us, kT0);
    CHECK_COND(e.accept);
    CHECK_EQ(e.accept_us, kT0 + 12000);
    CHECK_COND(e.psrdy);
    CHECK_EQ(e.psrdy_us, kT0 + 140000);
    Trace r;   // a Reject instead of an Accept: no Accept, no PS_RDY
    r.fill(0, 10, [](int) { return 5000; });
    r.mark(0, Mark::TxRequest);
    r.mark(5, Mark::Reject);
    const Edges f = find_edges(r.view(), kT0);
    CHECK_COND(!f.accept);
    CHECK_COND(f.rejected);
    CHECK_COND(!f.psrdy);
}

// A PPS step of 20 mV: VBUS 5000 mV, moves to 5020 mV 12 ms after the Accept with +-2 mV of noise; the final level is the mean of the
// last 50 ms, the settle time the first sample from which VBUS stays within 10 mV of it.
void pps_step() {
    Trace t;
    t.fill(-20, 300, [](int ms) {
        const int noise = (ms % 3) - 1;   // -1, 0, +1, ... (x2 below)
        return (ms < 22 ? 5000 : 5020) + 2 * noise;
    });
    const StepOut o = analyze_step(t.view(), kT0 + 10000);
    CHECK_COND(o.valid);
    CHECK_EQ(o.final_mv, 5020);
    CHECK_COND(o.settled);
    CHECK_EQ(o.settle.us, 12000u);   // 22 ms: the first sample at 5018..5022 with all later ones within 10 mV
}

// The Hard Reset path, fed live at poll resolution: VBUS 20 V falls under vSafe0V 400 ms after the reset, stays dark until 1200 ms, is
// back above vSafe5V at 1300 ms; the capabilities arrive at 1420 ms.
void reset_track() {
    ResetTrack r;
    r.start(5000);
    for (std::uint32_t ms = 0; ms <= 2000; ms += 5) {
        std::int32_t mv = 20000;
        if (ms >= 100) mv = ms < 400 ? 20000 - std::int32_t(ms - 100) * 65 : 0;
        if (ms >= 400 && ms < 1200) mv = 300;
        if (ms >= 1200) mv = ms < 1300 ? 1000 + std::int32_t(ms - 1200) * 30 : 5050;
        r.feed(5000 + ms, mv, ms >= 1420);
    }
    const ResetTimes t = r.times();
    CHECK_COND(t.off_seen);
    CHECK_EQ(t.off_ms, 400u);       // 825 mV at 395 ms, 300 mV at 400 ms: the first sample under 800 mV
    CHECK_COND(t.up_seen);
    CHECK_EQ(t.dark_ms, 800u);      // 400 -> 1200 ms (first sample above 800 mV after the dark)
    CHECK_COND(t.on_seen);
    CHECK_EQ(t.turn_on_ms, 100u);   // 1200 -> 1300 ms (5050 mV, the first at or above 4750 mV)
    CHECK_COND(t.caps_seen);
    CHECK_EQ(t.caps_ms, 120u);      // 1300 -> 1420 ms
    CHECK_EQ(t.gap_ms, 5u);
}

// A capture through the persisted stream (the form a recorded fixture has): encode a Recorder, decode it, analyze the view.
void stream_round_trip() {
    static transcap::Recorder<2048, 64> rec;
    rec.arm();
    rec.trigger(kT0 - 50000, transcap::Reason::Manual);
    rec.mark(kT0, Mark::TxRequest);
    rec.mark(kT0 + 10000, Mark::Accept);
    rec.mark(kT0 + 130000, Mark::PsRdy);
    for (int ms = -50; ms <= 300; ++ms) {
        const int v = ms < 40 ? 5000 : ms < 80 ? 5000 + (ms - 40) * 100 : 9000;
        rec.push({kT0 + std::uint32_t(ms * 1000), std::uint16_t(v), 0, 0});
    }
    std::vector<std::uint8_t> buf(64 * 1024);
    const std::size_t n = transcap::encode(rec, buf);
    CHECK_COND(n > 0);
    std::vector<Sample> sm(4096);
    std::vector<Marker> mk(64);
    transcap::Decoded d;
    CHECK_COND(transcap::decode({buf.data(), n}, sm, mk, d));
    const transcap::View v{{sm.data(), d.n_samples}, {mk.data(), d.n_markers}};
    const Edges e = find_edges(v, kT0 - 1000);
    CHECK_COND(e.accept && e.psrdy);
    const TransitionOut o = analyze_transition(v, {e, 5000, 9000});
    CHECK_EQ(o.band.us, 66000u);
    CHECK_EQ(o.start.us, 35000u);
}

}  // namespace

int main() {
    clean_up_step();
    slow_fall_with_early_ps_rdy();
    overshoot();
    gap_is_the_uncertainty();
    no_ps_rdy();
    edges_from_markers();
    pps_step();
    reset_track();
    stream_round_trip();
    return test_report("test_pdspec_analyze");
}
