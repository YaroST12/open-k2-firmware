// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PPS / AVS transition check (k2/chargertest/prog_check.hpp) over synthetic captures: overshoot past the valid margin, band entry
// against the step limits, the leave-and-return transient, accuracy of the final level, creep over the hold, and the not-measured cases.
#include <cstdint>
#include <vector>

#include "k2/chargertest/prog_check.hpp"
#include "k2/pd/transcap_calc.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::charger_test::progcheck;
using transcap::Mark;
using transcap::Marker;
using transcap::Sample;

namespace {

constexpr std::uint32_t kT0 = 1'000'000;   // the Request's TX start (us)
constexpr std::uint32_t kHold = 1'000'000;

// One sample per millisecond from -50 ms to 1500 ms; Request at 0, Accept at 10 ms, PS_RDY at `rdy_ms` (none when negative).
struct Trace {
    std::vector<Sample> s;
    std::vector<Marker> m;
    template <class F>
    explicit Trace(F f, int rdy_ms = 100, bool accept = true, int end_ms = 1500) {
        for (int ms = -50; ms <= end_ms; ++ms) s.push_back({kT0 + std::uint32_t(ms * 1000), std::uint16_t(f(ms)), 0, 0});
        mark(0, Mark::TxRequest);
        if (accept) mark(10, Mark::Accept);
        if (accept && rdy_ms >= 0) mark(rdy_ms, Mark::PsRdy);
    }
    void mark(int ms, Mark code) { m.push_back({kT0 + std::uint32_t(ms * 1000), std::uint8_t(code), 0, 0}); }
    transcap::View view() const { return {s, m}; }
};

// 5 V to `to` on a clean ramp of `slope` mV/ms starting 30 ms after the Accept, then flat.
auto ramp(int from, int to, int slope) {
    return [=](int ms) {
        const int t = ms - 40;
        if (t <= 0) return from;
        const int v = from < to ? from + t * slope : from - t * slope;
        return from < to ? (v > to ? to : v) : (v < to ? to : v);
    };
}

In pps(int from, int to) { return {false, from, to, kHold}; }
In avs(int from, int to) { return {true, from, to, kHold}; }

void clean_step_is_measured_and_quiet() {
    Trace t(ramp(5000, 9000, 100));
    const Out o = measure(t.view(), pps(5000, 9000));
    CHECK_COND(o.measured);
    CHECK_EQ(unsigned(o.flags), 0u);
    CHECK_EQ(o.over_mv, 0);
    CHECK_EQ(o.err_mv, 0);
    CHECK_EQ(o.out_us, 0);
    CHECK_COND(o.creep_known);
    CHECK_EQ(o.creep_mv, 0);
}

void overshoot_counts_past_the_valid_margin() {
    // vPpsNew of 9 V reaches 9450 mV; vPpsValid adds 100 mV: 9500 is inside, 9600 is not. The peak lasts 3 ms.
    for (const int peak : {9500, 9600}) {
        Trace t([&](int ms) { return ms >= 80 && ms < 83 ? peak : ramp(5000, 9000, 100)(ms); });
        const Out o = measure(t.view(), pps(5000, 9000));
        CHECK_COND(o.measured);
        CHECK_EQ(o.over_mv, peak - 9450);
        CHECK_EQ(unsigned(o.flags & kOver), peak > 9500 ? unsigned(kOver) : 0u);
    }
}

void undershoot_on_a_down_step_counts_the_same_way() {
    // 9 V to 5 V: the band floor is 4750 mV, the margin 100 mV: 4600 mV is past it.
    Trace t([](int ms) { return ms >= 80 && ms < 83 ? 4600 : ramp(9000, 5000, 100)(ms); });
    const Out o = measure(t.view(), pps(9000, 5000));
    CHECK_EQ(o.over_mv, 150);
    CHECK_EQ(unsigned(o.flags & kOver), unsigned(kOver));
}

void avs_margin_is_500_mv() {
    // vAvsNew of 20 V reaches 21000 mV; vAvsValid adds 500 mV.
    for (const int peak : {21400, 21600}) {
        Trace t([&](int ms) { return ms >= 80 && ms < 83 ? peak : ramp(15000, 20000, 100)(ms); });
        const Out o = measure(t.view(), avs(15000, 20000));
        CHECK_COND(o.measured);
        CHECK_EQ(unsigned(o.flags & kOver), peak > 21500 ? unsigned(kOver) : 0u);
    }
}

void late_band_entry_is_judged_against_the_step_size() {
    // 5 V to 9 V is a large step (275 ms); the band (8550 mV) is 3550 mV up, and the
    // ramp starts 30 ms after the Accept: 20 mV/ms enters the band 208 ms after it (in time), 10 mV/ms 385 ms after (late).
    Trace fast(ramp(5000, 9000, 20));
    CHECK_EQ(unsigned(measure(fast.view(), pps(5000, 9000)).flags & kLate), 0u);
    Trace slow(ramp(5000, 9000, 10), 100, true, 1900);
    const Out o = measure(slow.view(), pps(5000, 9000));
    CHECK_COND(o.measured);
    CHECK_EQ(unsigned(o.flags & kLate), unsigned(kLate));
    CHECK_COND(o.band_us > 275'000);
    // A 500 mV step is small (25 ms): 5000 to 5500 on 2 mV/ms enters the band (5225 mV) 143 ms after the Accept.
    Trace small(ramp(5000, 5500, 2));
    CHECK_EQ(unsigned(measure(small.view(), pps(5000, 5500)).flags & kLate), unsigned(kLate));
    // An EPR AVS APDO (position 8 and up) has the EPR large-step limit (760 ms): 21.5 to 28 V on 10 mV/ms enters the band (26.6 V)
    // about 540 ms after the Accept, late for an SPR AVS APDO (325 ms), in time for an EPR one.
    Trace epr(ramp(21500, 28000, 10), 100, true, 1900);
    In e = avs(21500, 28000);
    CHECK_EQ(unsigned(measure(epr.view(), e).flags & kLate), unsigned(kLate));
    e.pos = 9;
    CHECK_EQ(unsigned(measure(epr.view(), e).flags & kLate), 0u);
}

void leaving_the_band_again_is_the_transient() {
    // PPS: 200 ms outside the band after entering it (limit 150 ms).
    Trace t([](int ms) { return ms >= 150 && ms < 350 ? 7000 : ramp(5000, 9000, 100)(ms); });
    const Out p = measure(t.view(), pps(5000, 9000));
    CHECK_COND(p.out_us >= 199'000);
    CHECK_EQ(unsigned(p.flags & kTransient), unsigned(kTransient));
    // The same 100 ms is inside the PPS limit ...
    Trace t2([](int ms) { return ms >= 150 && ms < 250 ? 7000 : ramp(5000, 9000, 100)(ms); });
    CHECK_EQ(unsigned(measure(t2.view(), pps(5000, 9000)).flags & kTransient), 0u);
    // ... and past the AVS limit of 5 ms.
    Trace t3([](int ms) { return ms >= 150 && ms < 160 ? 17000 : ramp(15000, 20000, 100)(ms); });
    CHECK_EQ(unsigned(measure(t3.view(), avs(15000, 20000)).flags & kTransient), unsigned(kTransient));
}

void a_final_level_off_the_request_is_the_accuracy() {
    // The tail sits 460 mV above 9 V: past +-5 % (450 mV).
    Trace t(ramp(5000, 9460, 100));
    const Out o = measure(t.view(), pps(5000, 9000));
    CHECK_EQ(o.err_mv, 460);
    CHECK_EQ(unsigned(o.flags & kOff), unsigned(kOff));
    Trace ok(ramp(5000, 9400, 100));
    CHECK_EQ(unsigned(measure(ok.view(), pps(5000, 9000)).flags & kOff), 0u);
}

void creep_is_the_hold_drift_and_never_a_flag() {
    // 30 mV over the second half of the window: the last 50 ms against the 50 ms that started one hold earlier.
    Trace t([](int ms) { return ms < 500 ? ramp(5000, 9000, 100)(ms) : 9000 + (ms - 500) * 30 / 1000; });
    const Out o = measure(t.view(), pps(5000, 9000));
    CHECK_COND(o.creep_known);
    CHECK_COND(o.creep_mv >= 25 && o.creep_mv <= 31);
    CHECK_EQ(unsigned(o.flags), 0u);
}

void psrdy_lag_is_signed() {
    // The band at 66 ms after the Accept, PS_RDY 120 ms after it: VBUS was there 54 ms before PS_RDY.
    Trace t(ramp(5000, 9000, 100), 130);
    CHECK_EQ(measure(t.view(), pps(5000, 9000)).lag_us, 66'000 - 120'000);
    Trace late(ramp(5000, 9000, 100), 20);   // PS_RDY 10 ms after the Accept, VBUS 56 ms behind it
    CHECK_EQ(measure(late.view(), pps(5000, 9000)).lag_us, 66'000 - 10'000);
    Trace none(ramp(5000, 9000, 100), -1);
    CHECK_COND(!measure(none.view(), pps(5000, 9000)).lag_known);
}

void slew_is_the_steepest_sample_step() {
    Trace t(ramp(5000, 9000, 100));
    CHECK_EQ(measure(t.view(), pps(5000, 9000)).slew_mv_ms, 100);
}

void no_accept_or_a_short_window_is_not_measured() {
    Trace none(ramp(5000, 9000, 100), 100, false);
    const Out a = measure(none.view(), pps(5000, 9000));
    CHECK_COND(!a.measured);
    CHECK_EQ(unsigned(a.flags), 0u);
    Trace shortw(ramp(5000, 9000, 100), 100, true, 40);   // no 50 ms tail after the Accept
    CHECK_COND(!measure(shortw.view(), pps(5000, 9000)).measured);
    const std::vector<Sample> s;
    const std::vector<Marker> m;
    CHECK_COND(!measure(transcap::View{s, m}, pps(5000, 9000)).measured);
}

void the_old_level_is_read_from_the_capture() {
    // from 0: the 20 ms before the Accept say 9 V, the request is 5 V: a down-step, so its undershoot (4600 mV) counts.
    Trace t([](int ms) { return ms >= 80 && ms < 83 ? 4600 : ramp(9000, 5000, 100)(ms); });
    const Out o = measure(t.view(), pps(0, 5000));
    CHECK_EQ(o.over_mv, 150);
    CHECK_EQ(unsigned(o.flags & kOver), unsigned(kOver));
}

void flags_name_themselves() {
    CHECK_STR(flag_text(kOver), "overshoot");
    CHECK_STR(flag_text(kLate), "slow rise");
    CHECK_STR(flag_text(kTransient), "transient");
    CHECK_STR(flag_text(kOff), "level off");
    CHECK_STR(flag_text(kOver | kLate), "overshoot");   // the first one set names the row; the rest are in the detail
}

}  // namespace

int main() {
    clean_step_is_measured_and_quiet();
    overshoot_counts_past_the_valid_margin();
    undershoot_on_a_down_step_counts_the_same_way();
    avs_margin_is_500_mv();
    late_band_entry_is_judged_against_the_step_size();
    leaving_the_band_again_is_the_transient();
    a_final_level_off_the_request_is_the_accuracy();
    creep_is_the_hold_drift_and_never_a_flag();
    psrdy_lag_is_signed();
    slew_is_the_steepest_sample_step();
    no_accept_or_a_short_window_is_not_measured();
    the_old_level_is_read_from_the_capture();
    flags_name_themselves();
    return test_report("prog_check");
}
