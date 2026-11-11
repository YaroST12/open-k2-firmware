// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Engine tests: QC2 / QC3 / QC3+ against a fake HVDCP charger on a fake clock, and every safety rule.
#include <cstdio>
#include <cstring>

#include "fc_fake.hpp"

using namespace k2::fc;
using W = std::pair<std::uint8_t, std::uint8_t>;

static int g_checks = 0, g_failed = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_failed++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static void refusals() {
    FakeCharger c;
    Engine e(c);
    c.ceiling = 0;
    CHECK(e.start(Proto::Qc2, 5000, false) == Fail::NoMonitor);
    c.ceiling = 36000;
    CHECK(e.start(Proto::Qc2, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Qc3, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Qc2, 7000, true) == Fail::Unsupported);
    c.ceiling = 12000;
    CHECK(e.start(Proto::Qc2, 20000, true) == Fail::OverLimit);
    CHECK(c.set_calls == 0 && c.resets == 0);   // refusals never touch the pins
    CHECK(e.state() == State::Idle);
    // 5 V needs no confirmation
    CHECK(e.start(Proto::Qc2, 5000, false) == Fail::None);
    CHECK(e.state() == State::Negotiating);
    CHECK(e.start(Proto::Qc2, 5000, false) == Fail::Busy);
}

// A PD contract or a sink drawing load behind the K2: nothing starts unless explicitly overridden.
static void inline_guard() {
    using k2::link::Reason;
    FakeCharger c;
    Engine e(c);
    c.inline_now = Reason::PdContract;
    CHECK(e.start(Proto::Qc2, 5000, false) == Fail::Inline);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::Inline);      // the voltage confirm is not the override
    CHECK(e.start(Proto::Vooc, 5000, false) == Fail::Inline);
    CHECK(c.set_calls == 0 && c.resets == 0 && e.state() == State::Idle);   // pins untouched
    c.inline_now = Reason::DataDevice;
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::Inline);
    CHECK(e.start(Proto::Afc, 9000, false, true) == Fail::NeedConfirm);     // the override is no confirm either
    CHECK(e.start(Proto::Vooc, 5000, false, true) == Fail::NeedConfirm);    // the override needs the confirm at 5 V too
    CHECK(c.set_calls == 0);
    CHECK(e.start(Proto::Qc2, 9000, true, true) == Fail::None);   // both given: runs
    CHECK(e.state() == State::Negotiating);
    e.stop();
    c.inline_now = Reason::None;
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    CHECK(std::string_view(name(Fail::Inline)) == "PD or load active");
}

// The HID-cable loop 5 V lock: no level above 5 V starts or is retargeted to; a running session above 5 V is aborted (both
// lines to 0 V, the protocol's own return to 5 V); 5 V sessions still run.
static void hv_lock_guard() {
    {
        FakeCharger c;
        Engine e(c);
        c.hv_lock = true;
        CHECK(e.start(Proto::Qc2, 9000, true) == Fail::OverLimit);
        CHECK(e.start(Proto::Qc3, 9000, true) == Fail::OverLimit);
        CHECK(e.start(Proto::Qc3, 5400, true) == Fail::OverLimit);   // QC3 / SCP are programmable: only 5 V itself
        CHECK(e.start(Proto::Afc, 9000, true) == Fail::OverLimit);
        CHECK(e.start(Proto::Fcp, 9000, true) == Fail::OverLimit);
        CHECK(e.start(Proto::Scp, 9000, true) == Fail::OverLimit);
        CHECK(e.start(Proto::Ufcs, 9000, true) == Fail::OverLimit);
        CHECK(c.set_calls == 0 && c.resets == 0 && e.state() == State::Idle);   // pins untouched
        CHECK(e.start(Proto::Qc2, 5000, false) == Fail::None);                  // 5 V runs
        c.run(e, 3000);
        CHECK(e.state() == State::Active);
        CHECK(e.set_target(9000, true) == Fail::OverLimit);                     // no retarget above 5 V
        CHECK(c.vbus_mv() == 5000);
    }
    {   // Locked while a QC2 session holds 9 V: aborted on the next poll, VBUS back at 5 V.
        FakeCharger c;
        Engine e(c);
        CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
        c.run(e, 3000);
        CHECK(e.state() == State::Active && c.vbus_mv() == 9000);
        const int resets = c.resets;
        c.hv_lock = true;
        e.poll();
        CHECK(e.state() == State::Failed);
        CHECK(e.fail() == Fail::OverLimit);
        CHECK(c.resets == resets + 1);
        c.run(e, 3000);
        CHECK(c.vbus_mv() == 5000);
    }
}

static void qc2_9v() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);
    CHECK(c.dp == Drive::V3p3 && c.dm == Drive::V0p6);
    CHECK(e.reached_mv() == 9000);
}

static void qc2_slow_ramp() {
    FakeCharger c;
    c.qc2_delay_ms = 4000;
    Engine e(c);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    c.run(e, 5000);
    CHECK(e.state() == State::Negotiating);
    c.run(e, 1000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);

    FakeCharger stuck;
    stuck.qc2_delay_ms = 20000;
    Engine timeout(stuck);
    CHECK(timeout.start(Proto::Qc2, 9000, true) == Fail::None);
    stuck.run(timeout, 15000);
    CHECK(timeout.state() == State::Failed && timeout.fail() == Fail::NoBand);
    CHECK(stuck.resets > 0);
}

static void no_hvdcp() {
    FakeCharger c;
    c.hvdcp = false;
    Engine e(c);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    c.run(e, 3500);
    CHECK(e.state() == State::Failed);
    CHECK(e.fail() == Fail::NoHandshake);
    CHECK(c.resets >= 1);
}

// What is not an HVDCP: a DCP / BC1.2 port (D+ and D- shorted for good), an Apple divider (D- sits near 2.7 V) and a bare
// source (D- reads 0 V). None of them opens a short 1.25 s after D+ goes to 0.6 V, so no QC2 / QC3 / QC3+ target at any level,
// 5 V included, may end in anything but "no handshake" with the output still at 5 V and no D+/D- level change.
static void plain_sources_are_not_qc() {
    struct Src { const char* what; bool hvdcp; int dm_fixed_mv; };
    for (const Src s : {Src{"dcp short", false, -1}, Src{"apple divider", true, 2700}, Src{"bare pin", true, 0}}) {
        for (const auto [p, mv] : {std::pair{Proto::Qc2, 5000}, {Proto::Qc2, 9000}, {Proto::Qc2, 12000},
                                   {Proto::Qc3, 5000}, {Proto::Qc3, 9000}, {Proto::Qc3Plus, 5000}, {Proto::Qc3Plus, 9000}}) {
            FakeCharger c;
            c.hvdcp = s.hvdcp; c.dm_fixed_mv = s.dm_fixed_mv;
            Engine e(c);
            CHECK(e.start(p, mv, true) == Fail::None);
            c.run(e, 3500);
            if (e.state() != State::Failed || e.fail() != Fail::NoHandshake) std::printf("  source: %s, proto %d, %d mV\n", s.what, int(p), mv);
            CHECK(e.state() == State::Failed);
            CHECK(e.fail() == Fail::NoHandshake);
            CHECK(c.vbus_mv() == 5000);
            CHECK(c.pulses_up == 0 && c.pulses_down == 0);
        }
    }
}

// The reverse: a charger that really shorts, then opens the short (the delay a real HVDCP shows) is accepted at 5 V too.
static void hvdcp_5v_needs_the_handshake_and_passes_with_it() {
    for (const auto p : {Proto::Qc2, Proto::Qc3, Proto::Qc3Plus}) {
        FakeCharger c;
        Engine e(c);
        CHECK(e.start(p, 5000, false) == Fail::None);
        c.run(e, 1000);
        CHECK(e.state() == State::Negotiating);   // the short has not opened yet: nothing proven, nothing passed
        c.run(e, 6000);
        CHECK(e.state() == State::Active);
        CHECK(c.open);
        CHECK(c.vbus_mv() >= 4750 && c.vbus_mv() <= 5250);
    }
}

static void overvoltage_aborts() {
    FakeCharger c;
    c.stuck_at_12v = true;
    Engine e(c);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    c.run(e, 3000);
    CHECK(e.state() == State::Failed);
    CHECK(e.fail() == Fail::Overvoltage);
    CHECK(c.resets >= 1 && c.dp == Drive::V0 && c.dm == Drive::V0);
}

static void ceiling_abort() {
    FakeCharger c;
    c.ceiling = 8000;                         // below the 9 V the charger will deliver
    Engine e(c);
    CHECK(e.start(Proto::Qc2, 5000, false) == Fail::None);
    c.run(e, 2500);
    CHECK(e.state() == State::Active);
    c.vbus_pending = c.vbus = 8500;           // charger misbehaves above the ceiling
    c.apply_at = 0;
    c.run(e, 50);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
}

static void stop_resets() {
    FakeCharger c;
    Engine e(c);
    e.start(Proto::Qc2, 9000, true);
    c.run(e, 3000);
    const int r = c.resets;
    e.stop();
    CHECK(e.state() == State::Idle);
    CHECK(c.resets == r + 1);
    CHECK(c.dp == Drive::V0 && c.dm == Drive::V0);
    e.stop();                                  // idempotent and still safe
    CHECK(e.state() == State::Idle);
}

static void qc2_retarget() {
    FakeCharger c;
    Engine e(c);
    e.start(Proto::Qc2, 9000, true);
    c.run(e, 3000);
    CHECK(e.set_target(12000, false) == Fail::NeedConfirm);
    CHECK(e.set_target(12000, true) == Fail::None);
    c.run(e, 500);
    CHECK(e.state() == State::Active && c.vbus_mv() == 12000);
    CHECK(e.set_target(5000, false) == Fail::None);   // stepping down is allowed
    c.run(e, 3000);
    CHECK(e.state() == State::Active && c.vbus_mv() == 5000);   // no false overvoltage trip
}

static void qc3_walk() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 2000);   // handshake at ~1.3 s, then a 1000 ms wait: no pulse or baseline yet
    CHECK(c.pulses_up == 0 && c.pulses_down == 0 && !c.continuous);
    c.run(e, 4000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 8700 && c.vbus_mv() <= 9150);   // the asymmetric pass band: -300 / +150 mV
    CHECK(c.pulses_up >= 15);
    CHECK(e.set_target(7000, true) == Fail::None);
    c.run(e, 4000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 6700 && c.vbus_mv() <= 7150);
    CHECK(c.pulses_down >= 8);
}

static void qc3_clamped_to_ceiling() {
    FakeCharger c;
    c.ceiling = 9000;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 12000, true) == Fail::None);
    CHECK(e.target_mv() == 9000);
    int worst = 0;
    for (int i = 0; i < 8000; i++) {
        c.run(e, 1);
        if (c.vbus > worst) worst = c.vbus;
    }
    CHECK(worst <= 9150);
    CHECK(e.state() == State::Active);
}

static void qc3_class_b() {
    FakeCharger c;
    c.qc3_max_mv = 20000;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 20000, true) == Fail::None);
    CHECK(e.target_mv() == 20000);
    c.run(e, 10000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 19700 && c.vbus_mv() <= 20150);
    CHECK(e.set_target(5000, true) == Fail::None);
    c.run(e, 10000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 4700 && c.vbus_mv() <= 5150);
    e.stop(); c.run(e, 1000);
    CHECK(e.start(Proto::Qc3Plus, 20000, true) == Fail::None);
    CHECK(e.target_mv() == 12000);
}

static void qc3_class_a_cannot_reach_20v() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 20000, true) == Fail::None);
    c.run(e, 15000);
    CHECK(e.state() == State::Failed);
    CHECK(e.fail() == Fail::NoBand);
    CHECK(c.resets > 0);
}

// A Class A charger asked for 20 V climbs to its 12 V top and then ignores every pulse: two batches with zero movement while
// VBUS sits in the 12 V band end the session as NoBand, not the 12 s band timeout (the verdict and its reason are the same).
static void qc3_class_a_dead_batches_end_the_20v_ramp_early() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 20000, true) == Fail::None);
    std::uint32_t at_12v = 0, failed_at = 0;
    for (int i = 0; i < 20000 && !failed_at; i++) {
        c.run(e, 1);
        if (!at_12v && c.vbus_mv() >= 11400) at_12v = c.now;
        if (e.state() == State::Failed) failed_at = c.now;
    }
    CHECK(failed_at != 0);
    CHECK(e.fail() == Fail::NoBand);
    CHECK(at_12v != 0 && failed_at - at_12v <= 3500);   // the 12 s timeout would end it ~12 s after the start
    CHECK(c.resets > 0);
}
// The rule is for the 12 V band only: a Class B charger asked for 20 V is never cut off, however long the ramp takes.
static void qc3_class_b_slow_ramp_is_not_cut_off() {
    FakeCharger c;
    c.qc3_max_mv = 20000; c.qc3_delay_ms = 400; c.slew_mv_per_s = 2500;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 20000, true) == Fail::None);
    c.run(e, 14000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 19700);
}

// A QC3+ charger that does not behave like the idealised one: the closed loop must follow the measured VBUS,
// never overshoot the target by more than a few steps, and still arrive.
static int run_qc3p(int step, int delay_ms, int slew, int target, int& worst, Fail& fail, bool& active, int& end_mv) {
    FakeCharger c;
    c.plus_step_mv = step; c.qc3_delay_ms = delay_ms; c.slew_mv_per_s = slew;
    Engine e(c);
    if (e.start(Proto::Qc3Plus, target, true) != Fail::None) return -1;
    worst = 0;
    for (int i = 0; i < 40000; i++) {
        c.run(e, 1);
        if (c.vbus > worst) worst = c.vbus;
        if (e.state() == State::Failed) break;
    }
    fail = e.fail(); active = e.state() == State::Active; end_mv = c.vbus_mv();
    return c.pulses_up;
}

static void qc3plus_realistic_chargers() {
    struct Case { int step, delay, slew; } cases[] = {{20, 20, 0}, {20, 200, 4000}, {20, 200, 1000}, {20, 400, 700}, {25, 200, 1000}, {10, 200, 1000}, {40, 200, 1000}};
    for (const auto& k : cases) {
        int worst, end; Fail f; bool active;
        run_qc3p(k.step, k.delay, k.slew, 9000, worst, f, active, end);
        // The loop aims at target - 100 mV and never passes the +150 mV edge of the band, whatever the pulse size.
        if (!(active && f == Fail::None && worst <= 9150 && end >= 8800 && end <= 9000))
            std::printf("  case step=%d delay=%d slew=%d: worst=%d end=%d fail=%d active=%d\n", k.step, k.delay, k.slew, worst, end, int(f), active);
        CHECK(active && f == Fail::None);
        CHECK(worst <= 9150);
        CHECK(end >= 8800 && end <= 9000);
    }
}


static void qc3_coarse_step_chargers() {
    // A charger whose real pulse is ~2x nominal (about 425 mV) must still settle inside the pass band (-300 / +150 mV).
    struct Case { int step, delay, slew; } cases[] = {{200, 20, 0}, {425, 20, 0}, {425, 200, 1000}, {350, 150, 1000}, {500, 20, 0}, {450, 300, 700}, {640, 20, 0}};
    for (const auto& k : cases) {
        FakeCharger c; c.qc3_step_mv = k.step; c.qc3_delay_ms = k.delay; c.slew_mv_per_s = k.slew;
        Engine e(c);
        CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
        int worst = 0;
        for (int i = 0; i < 40000 && e.state() != State::Failed; i++) { c.run(e, 1); worst = std::max(worst, c.vbus_mv()); }
        if (e.state() != State::Active) std::printf("  coarse step=%d delay=%d slew=%d: state=%d fail=%d worst=%d end=%d\n", k.step, k.delay, k.slew, int(e.state()), int(e.fail()), worst, c.vbus_mv());
        CHECK(e.state() == State::Active);
        CHECK(worst <= 9150);
        CHECK(c.vbus_mv() >= 8700 && c.vbus_mv() <= 9150);
    }
    // A pulse of 600 or 700 mV cannot land inside a 450 mV wide band: the loop takes the closer level, never runs
    // away, and the row ends as "voltage not reached" instead of an overvoltage.
    for (int step : {600, 700}) {
        FakeCharger c; c.qc3_step_mv = step; c.qc3_delay_ms = 100; c.slew_mv_per_s = 1000;
        Engine e(c);
        CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
        int worst = 0;
        for (int i = 0; i < 40000 && e.state() != State::Failed; i++) { c.run(e, 1); worst = std::max(worst, c.vbus_mv()); }
        CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);
        CHECK(worst <= 9300);
    }
}

// The wait after a batch is for a stable VBUS, not a fixed 40 ms: near the target the next pulse goes out only once
// the last one has shown (slow charger: output starts moving 200 ms after the pulse and slews at 1 V/s).
static void qc3_waits_for_a_stable_vbus_near_the_target() {
    FakeCharger c; c.qc3_delay_ms = 200; c.slew_mv_per_s = 1000;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 20000);
    CHECK(e.state() == State::Active);
    CHECK(!c.qc3_batches.empty());
    int near = 0;
    for (std::size_t i = 1; i < c.qc3_batches.size(); i++) {
        const auto& b = c.qc3_batches[i];
        if (b.vbus + b.pending < 9000 - 700) continue;   // far away: batches are still pipelined
        // Within the last four pulses a batch leaves only from a settled level: the previous one has shown by then.
        ++near;
        CHECK(b.t - c.qc3_batches[i - 1].t >= 250);
        CHECK(b.pending == 0);
    }
    CHECK(near >= 1);
}

static void qc3_last_steps_are_single_pulses() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 8000);
    CHECK(e.state() == State::Active);
    int singles_at_the_end = 0;
    for (const auto& b : c.qc3_batches) if (b.vbus >= 9000 - 500 && (b.n == 1 || b.n == -1)) ++singles_at_the_end;
    CHECK(singles_at_the_end >= 1);
    for (const auto& b : c.qc3_batches) if (b.vbus >= 9000 - 500) CHECK(b.n == 1 || b.n == -1);
}

// Open-loop bound: a level the expected pulse count cannot explain aborts the row.
static void qc3_open_loop_bound_aborts_a_runaway() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 8000);
    CHECK(e.state() == State::Active);
    c.vbus = c.vbus_pending = 9500;   // the output jumps without any pulse (still inside the 110 % guard)
    c.apply_at = 0;
    c.run(e, 50);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
}

// QC3+ that never leaves 5 V: no QC3+ response, reported as such (not an overvoltage and not a timeout).
static void qc3plus_without_response_is_no_response() {
    FakeCharger c; c.plus_step_mv = 0;
    Engine e(c);
    CHECK(e.start(Proto::Qc3Plus, 9000, true) == Fail::None);
    c.run(e, 12000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(c.vbus_mv() <= 5100);
}

// The 5 V row of a continuous-mode unit (REMAX 10K USB-A, 2026-11-08): the bank idles at 5.176 V, 26 mV over the +150 mV pass band,
// and ignores the one down pulse the loop sends. The loop used to send that pulse every 500 ms for the whole 12 s row and end it
// as a timeout at 5.175 V. A level inside the vSafe5V window that did not follow the pulse is the 5 V level this row asks for.
static void qc3_5v_inside_vsafe5_that_ignores_the_pulse_is_done() {
    for (const Proto p : {Proto::Qc3, Proto::Qc3Plus}) {
        FakeCharger c; c.qc3_step_mv = 0; c.plus_step_mv = 0; c.rest_mv = c.vbus = c.vbus_pending = 5176;
        Engine e(c);
        CHECK(e.start(p, 5000, true) == Fail::None);
        c.run(e, 6000);
        CHECK(e.state() == State::Active);
        CHECK(c.qc3_batches.size() <= 1);   // the loop does not keep pulsing a charger that does not answer
        CHECK(c.vbus_mv() == 5176);
    }
}

// A charger that does follow the pulse still gets its 5 V row corrected into the pass band: the exemption is only for a
// level the pulses cannot move.
static void qc3_5v_above_the_band_is_stepped_down_when_the_charger_answers() {
    FakeCharger c; c.rest_mv = c.vbus = c.vbus_pending = 5300;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 5000, true) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 4700 && c.vbus_mv() <= 5150);
}

// Outside the vSafe5V window nothing is "at 5 V": a silent charger that sits at 5.6 V keeps timing out.
static void qc3_5v_outside_vsafe5_that_ignores_the_pulse_still_times_out() {
    FakeCharger c; c.qc3_step_mv = 0; c.rest_mv = c.vbus = c.vbus_pending = 5600;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 5000, true) == Fail::None);
    c.run(e, 20000);
    CHECK(e.state() == State::Failed);
}

// QC3 plain keeps the timeout for a charger whose pulses do nothing.
static void qc3_without_response_keeps_the_timeout() {
    FakeCharger c; c.qc3_step_mv = 0;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 15000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);
}

// QC3+ approaches in smaller batches: the last batch before the target is at most the 10 pulse tail.
static void qc3plus_batches_shrink_near_the_target() {
    FakeCharger c; c.qc3_delay_ms = 200; c.slew_mv_per_s = 1000;
    Engine e(c);
    CHECK(e.start(Proto::Qc3Plus, 9000, true) == Fail::None);
    c.run(e, 30000);
    CHECK(e.state() == State::Active);
    int far_max = 0, near_max = 0;
    for (const auto& b : c.qc3_batches) {
        const int mag = b.n < 0 ? -b.n : b.n;
        if (b.vbus < 9000 - 1500) far_max = std::max(far_max, mag);
        else near_max = std::max(near_max, mag);
    }
    CHECK(far_max == 50);
    CHECK(near_max <= 50);
    CHECK(c.qc3_batches.back().n <= 10 && c.qc3_batches.back().n >= -10);
}

static void qc3_deadband_never_swallows_a_suite_miss() {
    // A learned 425 mV step must not widen the dead band past the suite's +-300 mV pass band.
    CHECK(k2::qc::pulses_for(k2::qc::Mode::Qc3, -350, 600) != 0);
    CHECK(k2::qc::pulses_for(k2::qc::Mode::Qc3, 350, 600) != 0);
    CHECK(k2::qc::pulses_for(k2::qc::Mode::Qc3, -100, 200) == 0);
}

static void qc3plus_walk() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3Plus, 9000, true) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() >= 8860 && c.vbus_mv() <= 8940);   // aims at target - 100 mV, 20 mV granularity
    CHECK(c.pulses_up >= 190);
}

// A charger that answers QC3+ pulses with QC3-sized steps (hardware 2026-11-08: a power bank moved ~180 mV per 40 us pulse)
// does not do QC3+: the first batch measures the step, the session ends as "no response" (the runner shows a neutral
// "no QC3+"), VBUS is never ramped on and never reaches an overvoltage.
static void qc3plus_coarse_step_is_no_qc3plus() {
    for (const int step : {120, 180, 200}) {
        FakeCharger c; c.plus_step_mv = step; c.qc3_delay_ms = 100; c.slew_mv_per_s = 1000; c.qc3_max_mv = 20000;
        Engine e(c);
        CHECK(e.start(Proto::Qc3Plus, 12000, true) == Fail::None);
        int worst = 0;
        for (int i = 0; i < 20000 && e.state() != State::Failed; i++) { c.run(e, 1); worst = std::max(worst, c.vbus); }
        CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
        CHECK(worst <= 5000 + 10 * step + 100);   // the probe batch (10 pulses) is all that was ever sent
        CHECK(c.pulses_up <= 10);
        CHECK(c.coarse_logged == 1 && c.coarse_step > 100 && c.coarse_step <= step + 20);
    }
}

// True QC3+ pulses (20 mV, up to 40 mV on a slow charger) ramp 9 V -> 12 V and settle inside the pass band.
static void qc3plus_fine_step_ramps_9_to_12() {
    for (const int step : {20, 40}) {
        FakeCharger c; c.plus_step_mv = step; c.qc3_delay_ms = 200; c.slew_mv_per_s = 1000;
        Engine e(c);
        CHECK(e.start(Proto::Qc3Plus, 9000, true) == Fail::None);
        c.run(e, 15000);
        CHECK(e.state() == State::Active);
        CHECK(e.set_target(12000, true) == Fail::None);
        int worst = 0;
        for (int i = 0; i < 20000 && e.state() != State::Failed; i++) { c.run(e, 1); worst = std::max(worst, c.vbus); }
        CHECK(e.state() == State::Active);
        CHECK(c.vbus_mv() >= 11700 && c.vbus_mv() <= 12150);
        CHECK(worst <= 12150);
    }
}

// A real overvoltage on a QC3+ session still aborts: past the 110 % guard, and past what the pulses sent can explain.
static void qc3plus_real_overvoltage_still_aborts() {
    for (const int jump : {9500, 14000}) {
        FakeCharger c; c.qc3_max_mv = 20000;
        Engine e(c);
        CHECK(e.start(Proto::Qc3Plus, 9000, true) == Fail::None);
        c.run(e, 8000);
        CHECK(e.state() == State::Active);
        c.vbus = c.vbus_pending = jump;   // the output jumps without any pulse
        c.apply_at = 0;
        c.run(e, 50);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
    }
}

static void afc_9v_and_12v() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Afc, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Afc, 5000, true) == Fail::Unsupported);
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);
    CHECK(c.sent.size() == 3 && c.sent[0] == 0x46 && c.sent[2] == 0x46);
    CHECK(c.dp == Drive::V0p6);
    CHECK(e.set_target(12000, false) == Fail::NeedConfirm);
    CHECK(e.set_target(12000, true) == Fail::None);
    c.run(e, 500);
    CHECK(e.state() == State::Active && c.vbus_mv() == 12000);
    CHECK(c.sent.size() == 6 && c.sent[3] == 0x79);
}

static void afc_no_answer_fails() {
    FakeCharger c;
    c.afc_ok = false;
    Engine e(c);
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);   // no ping answered: not an AFC charger
    CHECK(c.resets >= 1);
}

// Pattern qc-follows-ping-level: a source that never answers the first AFC exchange gets one retry after the lines sat low for at
// least 100 ms, then the session ends; it does not run three sends on top of three wire attempts each.
static void afc_ends_on_the_first_unanswered_exchange() {
    FakeCharger c;
    c.afc_ok = false;
    Engine e(c);
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
    CHECK(c.xfer_times.size() == 2);
    CHECK(c.xfer_times.size() == 2 && c.xfer_times[1] - c.xfer_times[0] >= 100);
    CHECK(c.afc_log.size() == 2);
    for (const auto& l : c.afc_log) CHECK(l.tx == 0x46 && l.rx < 0);   // nothing heard: no reply byte
    CHECK(c.resets >= 1);
}

// One lost exchange is not the end: the retry that gets a slave ping starts the normal three sends, and every exchange is logged
// with the byte sent, the byte heard and VBUS.
static void afc_retry_after_a_missed_ping_goes_on() {
    FakeCharger c;
    c.afc_fail_first = 1;
    Engine e(c);
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
    c.run(e, 2000);
    CHECK(e.state() == State::Active && c.vbus_mv() == 9000);
    CHECK(c.xfer_times.size() == 4);   // the miss, then the three sends
    CHECK(c.afc_log.size() == 4);
    CHECK(c.afc_log.size() == 4 && c.afc_log[0].rx < 0 && c.afc_log[1].rx == 0 && c.afc_log[1].tx == 0x46);
    CHECK(c.afc_log.size() == 4 && c.afc_log[0].vbus > 0 && c.afc_log[3].n == 4);   // n counts the exchanges of the session from 1
    // an answered first exchange is followed by three plain sends, no retry
    FakeCharger d;
    Engine f(d);
    CHECK(f.start(Proto::Afc, 12000, true) == Fail::None);
    d.run(f, 2000);
    CHECK(f.state() == State::Active && d.xfer_times.size() == 3 && d.afc_log.size() == 3);
}

static FakeCharger* fcp_charger(FakeCharger& c) {
    c.accp_charger = true;
    c.regs[accp::kRegFcpCaps] = 1;
    return &c;
}

static void fcp_9v() {
    FakeCharger c;
    fcp_charger(c);
    Engine e(c);
    CHECK(e.start(Proto::Fcp, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Fcp, 10000, true) == Fail::Unsupported);
    CHECK(e.start(Proto::Fcp, 9000, true) == Fail::None);
    c.run(e, 3500);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);
    // register sequence: read 0x21, write 0x2C=90, 0x2D=20, 0x2B=1
    CHECK(!c.reads.empty() && c.reads[0] == 0x21);
    CHECK(c.writes.size() == 3);
    CHECK((c.writes[0] == W{0x2C, 90}));
    CHECK((c.writes[1] == W{0x2D, 20}));
    CHECK((c.writes[2] == W{0x2B, 1}));
    CHECK(e.set_target(12000, true) == Fail::None);
    c.run(e, 600);
    CHECK(e.state() == State::Active && c.vbus_mv() == 12000);
    // The retarget of a live session does not read the caps register again (the charger-test FCP 9 / 12 V rows rely on it).
    int caps_reads = 0;
    for (const auto r : c.reads) caps_reads += r == accp::kRegFcpCaps;
    CHECK(caps_reads == 1);
    CHECK((c.writes.back() == W{0x2B, 1}));
}

// Real Huawei FCP charger (no-name 30 W, HW 2026-11-03): it answers 0x21/0x2C/0x2B but refuses the 0x2D
// current write with a NACK; the voltage still switches. A refused current write must not end the session.
static void fcp_current_register_refused_still_switches() {
    FakeCharger c;
    fcp_charger(c);
    c.nack_write[accp::kRegFcpIout] = true;
    Engine e(c);
    CHECK(e.start(Proto::Fcp, 9000, true) == Fail::None);
    c.run(e, 3500);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);
}

static void fcp_without_caps_fails() {
    FakeCharger c;
    fcp_charger(c);
    c.regs[accp::kRegFcpCaps] = 0;
    Engine e(c);
    e.start(Proto::Fcp, 9000, true);
    c.run(e, 3500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
    CHECK(c.writes.empty());   // nothing was committed to a charger without FCP caps
    CHECK(c.resets >= 1);
}

static void scp_flow() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x10;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 9000, false) == Fail::NeedConfirm);
    CHECK(e.start(Proto::Scp, 9000, true) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Active);
    CHECK(c.vbus_mv() == 9000);
    CHECK(c.regs[0xA0] == 0xC0 && c.regs[0xA1] == 0x06);
    CHECK(c.regs[0xB2] == accp::hi8(3500) && c.regs[0xB3] == accp::lo8(3500));
    // keep-alive: the charger watchdog (500 ms) never expires while the engine runs
    c.run(e, 3000);
    CHECK(e.state() == State::Active && c.vbus_mv() == 9000);
    CHECK(e.adapter_mv() >= 8900 && e.adapter_mv() <= 9100);
    CHECK(e.set_target(7000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Active && c.vbus_mv() == 7000);
}

// The adapter stops answering during the keep-alive. "Source lost" only when VBUS fell below 85 % of the target;
// a 4.5 V (or any) SCP level that is still held just reports no response.
static void scp_keep_alive_loss_is_relative_to_the_target() {
    for (int held = 0; held < 2; ++held) {
        FakeCharger c;
        fcp_charger(c);
        c.scp_charger = true;
        c.regs[accp::kRegScpType0] = 0x10;
        Engine e(c);
        e.start(Proto::Scp, 9000, true);
        c.run(e, 6000);
        CHECK(e.state() == State::Active);
        c.afc_ok = false;
        c.hold_vbus = held;
        if (!held) { c.vbus = c.vbus_pending = 2000; }
        c.run(e, 2000);
        CHECK(e.state() == State::Failed);
        CHECK(e.fail() == (held ? Fail::NoResponse : Fail::SourceLost));
    }
}

// SCP detection flow: ping, Reg 0x80 bit 4, Reg 0x7E bit 4 clear, limits 0x90..0x97. With a zero current
// request only those reads run: the adapter is "detected" and nothing is written (no 0xA0 enable, no V/I set).
static void scp_detect_only_reads_without_writing() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x10;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Detected);
    CHECK(c.writes.empty());
}
// No ACCP answer at all (FCP gets no response either, e.g. a PD-only USB-C charger): the SCP detect row must
// fail with NoResponse, never pass as "detected".
static void scp_detect_needs_a_valid_ack() {
    {
        FakeCharger c;
        fcp_charger(c);
        c.scp_charger = true;
        c.regs[accp::kRegScpType0] = 0xFF;   // would pass bit 4 if a read were accepted
        c.afc_ok = false;                    // every xfer fails
        Engine e(c);
        CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
        c.run(e, 8000);
        CHECK(e.state() == State::Failed);
        CHECK(e.fail() != Fail::Detected);
    }
}
static void scp_detect_rejects_an_all_ones_type_byte() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0xFF;   // a line that reads all ones is not an SCP adapter
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
}
static void scp_detect_only_still_rejects_a_non_scp_adapter() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x00;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
}
// Anker Prime 100W A2343 USB-A, measured 2026-11-03: Reg 0x80 = 0x90, Reg 0x7E = 0xB8 (bit 4 set). A real SCP
// adapter; only Reg 0x80 bit 4 gates SCP, 0x7E is logged.
static void scp_real_anker_bytes_are_detected() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x90;
    c.regs[accp::kRegScpType1] = 0xB8;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Detected);
}
// Anker Prime A2343 (2026-11-03): the 16-bit Reg 0xB0/0xB2 writes are stored but ignored; the output follows the 8-bit
// Reg 0xCA = (mV - 3000) / 10 and 0xCB = mA / 50 (CA=250 -> Reg 0xB8/B9 read 5500 and VBUS rose to 5.47 V).
static void scp_low_voltage_writes_the_8bit_set_registers() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x90;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5500, true, false, 5000) == Fail::None);
    c.run(e, 6000);
    CHECK(c.regs[0xCA] == 250);
    CHECK(c.regs[0xCB] == 100);
}

// The current limit written before the target voltage is the one the caller asked for (default 3.5 A).
static void scp_current_follows_the_request() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x10;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 10000, true, false, 2250) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Active && c.vbus_mv() == 10000);
    CHECK(c.regs[0xB2] == accp::hi8(2250) && c.regs[0xB3] == accp::lo8(2250));
}

// The limit registers read in the detect session are kept as the adapter's capabilities (3.3..12 V, up to 2.5 A: read from
// a 12 V USB-A port on hardware); a following session may then ask above the old 11 V cap but never past what was reported,
// and its current limit is held to the reported maximum.
static void set_scp_limits(FakeCharger& c, std::uint8_t vmin, std::uint8_t vmax, std::uint8_t imax) {
    c.regs[accp::kRegScpType0] = 0x10;
    c.regs[0x92] = vmin;
    c.regs[0x93] = vmax;
    c.regs[0x94] = 0x5E;
    c.regs[0x95] = imax;
}
static void scp_caps_are_kept_from_the_detect_session() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    set_scp_limits(c, 0xA1, 0xCC, 0x99);
    Engine e(c);
    CHECK(!e.caps().scp.valid);
    CHECK(e.start(Proto::Scp, 5000, false, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Detected);
    CHECK(e.caps().scp.valid);
    CHECK(e.caps().scp.min_mv == 3300);
    CHECK(e.caps().scp.max_mv == 12000);
    CHECK(e.caps().scp.max_ma == 2500);
    CHECK(c.writes.empty());
}
static void scp_asks_up_to_the_reported_maximum_and_current() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    set_scp_limits(c, 0xA1, 0xCC, 0x99);
    Engine e(c);
    e.start(Proto::Scp, 5000, false, false, 0);
    c.run(e, 6000);
    CHECK(e.start(Proto::Scp, 12000, true, false, 6000) == Fail::None);   // 12 V is past the old 11 V cap
    c.run(e, 6000);
    CHECK(e.state() == State::Active && c.vbus_mv() == 12000);
    CHECK(e.target_mv() == 12000);
    CHECK(c.regs[0xB2] == accp::hi8(2500) && c.regs[0xB3] == accp::lo8(2500));   // 6 A asked, 2.5 A reported
}
static void scp_without_caps_keeps_the_old_cap_and_current() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x10;   // the limit registers read 0: no capability list
    Engine e(c);
    CHECK(e.start(Proto::Scp, 12000, true, false, 6000) == Fail::None);
    CHECK(e.target_mv() == 11000);
    c.run(e, 6000);
    CHECK(!e.caps().scp.valid);
    CHECK(c.regs[0xB2] == accp::hi8(6000) && c.regs[0xB3] == accp::lo8(6000));
}
// Caps of an earlier adapter are stale for the next session: it re-reads them and refuses a target above its own maximum
// before anything is written.
static void scp_session_rereads_caps_and_refuses_above_them() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    set_scp_limits(c, 0xA1, 0xCC, 0x99);
    Engine e(c);
    e.start(Proto::Scp, 5000, false, false, 0);
    c.run(e, 6000);
    set_scp_limits(c, 0xA2, 0xB7, 0xB2);   // another adapter: 3.4..5.5 V, 5 A
    CHECK(e.start(Proto::Scp, 9000, true, false, 2250) == Fail::None);   // validated against the old caps
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::OverLimit);
    CHECK(e.caps().scp.max_mv == 5500);
    CHECK(c.writes.empty());
}

// Anker Prime A2343: the 4.5 V SCP row never moved VBUS (it stayed at 5.11 V) and was reported as "overvoltage".
// A down-target with VBUS unchanged is "voltage not reached", not overvoltage.
static void scp_down_target_with_unchanged_vbus_is_not_overvoltage() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x90;
    c.vbus = c.vbus_pending = 5110;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 4500, true, false, 5000) == Fail::None);
    for (int n = 0; n < 1500 && e.state() == State::Negotiating; ++n) { c.vbus = c.vbus_pending = 5110; c.run(e, 10); }
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoBand);   // "voltage not reached"
    // A real rise above the start level is still caught once the adapter has answered (before its first answer the same rise is
    // a QC-only source following the lines: unacked_rise_is_unsupported_*).
    FakeCharger d;
    fcp_charger(d);
    d.scp_charger = true;
    d.regs[accp::kRegScpType0] = 0x90;
    d.vbus = d.vbus_pending = 5110;
    Engine g(d);
    CHECK(g.start(Proto::Scp, 4500, true, false, 5000) == Fail::None);
    d.run(g, 4000);
    CHECK(!d.reads.empty());   // the type register was read and answered
    CHECK(g.state() == State::Negotiating || g.state() == State::Active);
    d.vbus = d.vbus_pending = 6500;
    d.run(g, 100);
    CHECK(g.fail() == Fail::Overvoltage);
}

static void scp_not_supported() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x00;
    Engine e(c);
    e.start(Proto::Scp, 5000, false);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
}

// A QC-only bank reads the D+/D- levels of AFC / FCP / SCP as QC requests and raises VBUS on its own, but never answers the
// protocol's own exchange. Whatever VBUS did before the first answer, the protocol is not supported: the row must not read as an
// overvoltage or a missing response. The bus is still dropped at once (the reset is the safety action, only the label changes).
static void unacked_rise_is_unsupported_afc() {
    for (const int rise : {6700, 9690, 12000}) {
        FakeCharger c;
        c.afc_ok = false;
        c.dpdm_rise_mv = rise;
        Engine e(c);
        CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
        c.run(e, 1500);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
        CHECK(c.resets >= 1 && c.dp == Drive::V0 && c.dm == Drive::V0);
        CHECK(c.vbus_mv() <= 5100);   // the rise was cut: the bank falls back to 5 V once the lines are dropped
    }
}

static void unacked_rise_is_unsupported_fcp() {
    for (const int level : {5000, 9000, 12000}) {
        FakeCharger c;
        c.dpdm_rise_mv = 9690;   // accp_charger stays false: no register frame is ever answered
        Engine e(c);
        CHECK(e.start(Proto::Fcp, level, true) == Fail::None);
        c.run(e, 3500);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
        CHECK(c.resets >= 1 && c.writes.empty());
        CHECK(c.vbus_mv() <= 5100);
    }
}

static void unacked_rise_is_unsupported_scp() {
    FakeCharger c;
    c.dpdm_rise_mv = 6700;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 5000, true, false, 0) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
    CHECK(c.resets >= 1 && c.writes.empty());
}

// The HVDCP handshake is the first thing FCP / SCP do: a charger that never opens the short has not acknowledged anything.
static void unacked_no_handshake_is_unsupported_fcp_scp() {
    for (const auto p : {Proto::Fcp, Proto::Scp}) {
        FakeCharger c;
        c.hvdcp = false;
        Engine e(c);
        CHECK(e.start(p, 9000, true) == Fail::None);
        c.run(e, 8000);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Unsupported);
        CHECK(c.resets >= 1);
    }
}

// The monitor ceiling is not a classification question: VBUS above it is an overvoltage with or without an answer.
static void unacked_rise_above_the_ceiling_is_still_overvoltage() {
    FakeCharger c;
    c.afc_ok = false;
    c.ceiling = 9500;   // the 9 V level is allowed, the bank's 9.69 V is not
    c.dpdm_rise_mv = 9690;
    Engine e(c);
    CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
    c.run(e, 1500);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage);
    CHECK(c.resets >= 1);
}

// A charger that did answer keeps its real classification: an overshoot after the acknowledgement is an overvoltage.
static void acked_overshoot_is_still_overvoltage() {
    {
        FakeCharger c;
        Engine e(c);
        CHECK(e.start(Proto::Afc, 9000, true) == Fail::None);
        c.run(e, 1500);
        CHECK(e.state() == State::Active);
        c.vbus = c.vbus_pending = 15000;
        c.apply_at = 0;
        c.run(e, 50);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage && c.resets >= 1);
    }
    {
        FakeCharger c;
        fcp_charger(c);
        Engine e(c);
        CHECK(e.start(Proto::Fcp, 9000, true) == Fail::None);
        c.run(e, 3500);
        CHECK(e.state() == State::Active);
        c.vbus = c.vbus_pending = 15000;
        c.apply_at = 0;
        c.run(e, 50);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage && c.resets >= 1);
    }
}

static void scp_clamped_to_ceiling() {
    FakeCharger c;
    fcp_charger(c);
    c.scp_charger = true;
    c.regs[accp::kRegScpType0] = 0x10;
    c.ceiling = 8000;
    Engine e(c);
    CHECK(e.start(Proto::Scp, 11000, true) == Fail::None);
    CHECK(e.target_mv() == 8000);
}

// D- is never driven by a VOOC session: only released (HiZ during the wake, Sniff while listening). The end-of-session
// reset goes through reset_bus(), not set_dm.
static bool dm_never_driven(const FakeCharger& c) {
    for (const auto d : c.dm_log) if (d != Drive::HiZ && d != Drive::Sniff) return false;
    return true;
}

// Detection: the first ident the adapter clocks ends the session as "detected" in the same poll, with nothing answered
// (operator rule 2026-11-11: no reply that could make a SuperVOOC adapter switch to its high-current output). One ident
// covers the family, so the three protocols behave the same.
static void vooc_ident_is_detected_and_nothing_is_answered() {
    for (auto p : {Proto::Vooc, Proto::SuperVooc, Proto::SuperVooc65}) {
        for (std::uint8_t ident : {0xA8, 0xAC, 0xA6}) {
            FakeCharger c;
            Engine e(c);
            CHECK(e.start(p, 9000, false) == Fail::None);   // the request is forced to 5 V
            CHECK(e.target_mv() == 5000);
            CHECK(c.dp == Drive::Listen && c.dm == Drive::Sniff);
            c.vooc_cmds = {ident, 0x98, 0x98, 0x82};
            while (c.vooc_i == 0 && c.now < 20000) c.run(e, 1);
            // the poll that decoded the ident already ended the session and released the lines
            CHECK(e.state() == State::Failed && e.fail() == Fail::Detected);
            CHECK(c.resets == 1);
            CHECK(c.now == c.vooc_last_rx_ms);
            c.run(e, 5000);
            CHECK(c.vooc_i == 1);                 // nothing after the ident was even read
            CHECK(dm_never_driven(c));
            CHECK(c.vbus_mv() == 5000);
        }
    }
}

// The wake: try 1 is the phone's burst from the 2026-11-11 capture (D+ 40 x 300 us high / 300 us low, D- released so the
// adapter's D+/D- short carries it, ending low), try 2 the 13-period train of the 2026-11-02 work, try 3 the burst again.
static void vooc_wake_forms_alternate() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Vooc, 5000, false) == Fail::None);
    c.run(e, 30000);
    CHECK(c.vooc_wakes.size() == 3);
    if (c.vooc_wakes.size() != 3) return;
    const auto& burst = c.vooc_wakes[0];
    CHECK(burst.size() == kVoocBurstSteps);
    std::uint32_t us = 0;
    for (std::size_t i = 0; i < burst.size(); ++i) {
        CHECK(burst[i].dm == Drive::HiZ && burst[i].us == 300);
        CHECK(burst[i].dp == (i % 2 == 0 ? Drive::V3p3 : Drive::V0));
        us += burst[i].us;
    }
    CHECK(us == 24000 && burst.back().dp == Drive::V0);
    const auto& train = c.vooc_wakes[1];
    CHECK(train.size() == kVoocTrainSteps);
    std::uint32_t tus = 0;
    for (const auto& s : train) { tus += s.us; CHECK(s.dm == Drive::HiZ); }
    CHECK(tus == 13 * 3536 + 1500);
    CHECK(c.vooc_wakes[2].size() == kVoocBurstSteps);
    CHECK(dm_never_driven(c));
}

// An all-ones frame (D- floating high under a clock nobody answers, the no-phone run of 2026-11-11) is nothing heard:
// the session ends as "needs load" like a charger that never clocks.
static void vooc_all_ones_frame_is_nothing_heard() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::SuperVooc, 5000, false) == Fail::None);
    c.vooc_raw = {0x1FF, 0x1FF, 0x1FF, 0x1FF};
    c.run(e, 30000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NeedsLoad);
    CHECK(c.vooc_raw_i == 4 && c.wakes == 3);
}

// Frames that are not an ident (the adapter is mid-session, or noise) are heard but never answered: "no response".
static void vooc_other_commands_are_never_answered() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::SuperVooc, 5000, false) == Fail::None);
    c.vooc_cmds = {0x55, 0x12, 0x98, 0x82, 0x88, 0x88, 0x88};
    c.run(e, 30000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NoResponse);
    CHECK(dm_never_driven(c) && c.resets == 1 && c.vbus_mv() == 5000);
}

// VBUS above 5.5 V at any point (listening, resting between wakes) aborts and releases; 5.5 V itself does not.
static void vooc_vbus_above_5v5_aborts() {
    for (std::uint32_t at : {100u, 2500u}) {   // VoocListen, VoocRest
        FakeCharger c;
        Engine e(c);
        CHECK(e.start(Proto::SuperVooc65, 5000, false) == Fail::None);
        c.vbus = c.vbus_pending = 5500;
        c.run(e, at);
        CHECK(e.state() == State::Negotiating);
        c.vbus = c.vbus_pending = 5600;
        c.run(e, 1);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage && c.resets == 1);
    }
}

// Nothing of this runs inline (a phone or a PD contract behind the K2): refused even with the override, and a session that
// sees one appear ends at once.
static void vooc_never_inline() {
    using k2::link::Reason;
    for (auto p : {Proto::Vooc, Proto::SuperVooc, Proto::SuperVooc65}) {
        FakeCharger c;
        Engine e(c);
        c.inline_now = Reason::PdContract;
        CHECK(e.start(p, 5000, true, true) == Fail::Inline);
        CHECK(c.set_calls == 0 && c.wakes == 0);
        c.inline_now = Reason::None;
        CHECK(e.start(p, 5000, false) == Fail::None);
        c.run(e, 50);
        c.inline_now = Reason::DataDevice;
        c.run(e, 1);
        CHECK(e.state() == State::Failed && e.fail() == Fail::Inline && c.resets == 1);
    }
}

// stop() mid-listen releases the lines and goes idle.
static void vooc_stop_releases() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::SuperVooc, 5000, false) == Fail::None);
    c.run(e, 200);
    e.stop();
    CHECK(e.state() == State::Idle && c.resets == 1);
}

// The adapter's clock is intermittent: a missed wake is retried after a rest, up to three tries.
static void vooc_wake_retries_until_the_adapter_clocks() {
    FakeCharger c;
    c.vooc_wake_needed = 2;
    c.vooc_cmds = {0xA8, 0xAC};
    Engine e(c);
    CHECK(e.start(Proto::Vooc, 5000, false) == Fail::None);
    CHECK(e.vooc_tries() == 1);
    c.run(e, 5000);
    CHECK(c.wakes == 2 && e.vooc_tries() == 2);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Detected);
}

static void vooc_wake_gives_up_after_three_tries() {
    for (auto p : {Proto::Vooc, Proto::SuperVooc, Proto::SuperVooc65}) {
        FakeCharger c;
        Engine e(c);
        CHECK(e.start(p, 5000, false) == Fail::None);
        c.run(e, 30000);
        CHECK(e.state() == State::Failed && e.fail() == Fail::NeedsLoad);
        CHECK(c.wakes == 3 && e.vooc_tries() == 3);
    }
}

static void vooc_silent_charger() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Vooc, 5000, false) == Fail::None);
    c.run(e, 2900);
    CHECK(e.state() == State::Negotiating);
    c.run(e, 30000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NeedsLoad);
    CHECK(c.resets == 1);
}

static void svooc_silent_needs_load() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::SuperVooc, 5000, false) == Fail::None);
    c.run(e, 30000);
    CHECK(e.state() == State::Failed && e.fail() == Fail::NeedsLoad);
}

static void vooc_overvoltage_aborts() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Vooc, 5000, false) == Fail::None);
    c.vbus = c.vbus_pending = 14000;   // an adapter that jumps up on its own
    c.run(e, 5);
    CHECK(e.state() == State::Failed && e.fail() == Fail::Overvoltage && c.resets == 1);
}

// After a QC session the lines sit at 0 V (the charger falls back to 5 V) and a new HVDCP detection waits the 500 ms hold
// from that reset, even when the caller starts again at once; a start after a long rest is not delayed.
static void qc_redetect_waits_the_500ms_hold() {
    FakeCharger c;
    Engine e(c);
    CHECK(e.start(Proto::Qc3, 9000, true) == Fail::None);
    c.run(e, 6000);
    CHECK(e.state() == State::Active && c.vbus > 8500);
    e.stop();
    CHECK(c.resets == 1);
    c.run(e, 120);
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    const std::uint32_t t0 = c.now;
    bool floated = false;
    while (c.now - t0 < 300) { c.run(e, 1); floated |= c.dp == Drive::HiZ; }   // 420 ms after the reset
    CHECK(!floated && c.dp == Drive::V0 && c.dm == Drive::V0);
    c.run(e, 200);                                                             // past the 500 ms hold
    CHECK(c.dp != Drive::V0 || e.state() == State::Active);
    c.run(e, 6000);
    CHECK(e.state() == State::Active && c.vbus == 9000);

    FakeCharger d;
    Engine f(d);
    CHECK(f.start(Proto::Qc2, 9000, true) == Fail::None);
    d.run(f, 6000);
    f.stop();
    d.run(f, 3000);                                                            // a long rest: no extra wait
    CHECK(f.start(Proto::Qc2, 9000, true) == Fail::None);
    d.run(f, 120);
    CHECK(d.dp != Drive::V0);

    // The same hold stands before the opening of AFC, VOOC, UFCS and vivo: their first move (the D+ 0.6 V settle, the wake train,
    // the UART claim) waits for it, the lines stay at 0 V meanwhile; a start after a long rest opens at once.
    struct Probe : FakeCharger {
        int ufcs_opens = 0, vivo_opens = 0;
        bool ufcs_open() override { ++ufcs_opens; return false; }
        bool vivo_open() override { ++vivo_opens; return false; }
    };
    for (const Proto p : {Proto::Afc, Proto::Vooc, Proto::Ufcs, Proto::Vivo}) {
        for (const bool rested : {false, true}) {
            Probe g;
            Engine h(g);
            CHECK(h.start(Proto::Qc3, 9000, true) == Fail::None);
            g.run(h, 6000);
            h.stop();
            g.run(h, rested ? 3000 : 120);
            const auto opened = [&] {
                return !g.sent.empty() || g.wakes > 0 || g.ufcs_opens > 0 || g.vivo_opens > 0 || g.dp == Drive::V0p6;
            };
            CHECK(h.start(p, p == Proto::Afc ? 9000 : 5000, true) == Fail::None);
            const std::uint32_t u0 = g.now;
            bool early = false;
            while (g.now - u0 < 300) { g.run(h, 1); early |= opened(); }   // 420 ms after the reset
            if (rested) {
                CHECK(early);
            } else {
                CHECK(!early && g.dp == Drive::V0 && g.dm == Drive::V0 && h.state() == State::Negotiating);
                g.run(h, 250);                                              // past the 500 ms hold
                CHECK(opened());
            }
        }
    }
}

// The windows in which the main loop must not be held by a redraw: the ones the engine times itself from poll() (the FCP / SCP
// handshake pulses, the VOOC receive slices, the UFCS handshake, the vivo UART frames). Everything the idle hook serves (the QC2 /
// QC3 / QC3+ handshake included), and the settle / loop phases, are not.
static void paint_critical_follows_the_phase() {
    FakeCharger c;
    Engine e(c);
    CHECK(!e.paint_critical());   // idle
    CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
    CHECK(!e.paint_critical());   // the QC2 handshake is served by service_hs(): the screen keeps moving
    bool critical_seen = false;
    for (int i = 0; i < 3000 && e.state() != State::Active; i++) {
        c.run(e, 1);
        critical_seen = critical_seen || e.paint_critical();
    }
    CHECK(!critical_seen);
    CHECK(e.state() == State::Active);
    e.stop();
    CHECK(!e.paint_critical());   // Idle again

    // FCP keeps its handshake window blocked (the same HVDCP steps, then a register script that is not served from the hook).
    FakeCharger f;
    Engine fe(f);
    CHECK(fe.start(Proto::Fcp, 9000, true) == Fail::None);
    CHECK(fe.paint_critical());
    bool fcp_blocked_to_drop = true;
    for (int i = 0; i < 1400; i++) { f.run(fe, 1); fcp_blocked_to_drop = fcp_blocked_to_drop && fe.paint_critical(); }
    CHECK(fcp_blocked_to_drop);   // discharge, float, bias and the wait for the charger's drop (about 1.4 s)

    // SCP: the 1 s at 0 V is a pure wait (a late edge is harmless), the bias and wake train are blocked.
    FakeCharger s;
    Engine se(s);
    CHECK(se.start(Proto::Scp, 9000, true) == Fail::None);
    bool reset_free = true;
    for (int i = 0; i < 900; i++) { s.run(se, 1); reset_free = reset_free && !se.paint_critical(); }
    CHECK(reset_free);
    for (int i = 0; i < 300; i++) s.run(se, 1);
    CHECK(se.paint_critical());   // HsbBias, then the wake train

    // The policy by protocol (the UI asks it before the engine starts).
    CHECK(!Engine::blocks_paint(Proto::Qc2) && !Engine::blocks_paint(Proto::Qc3) && !Engine::blocks_paint(Proto::Qc3Plus));
    CHECK(!Engine::blocks_paint(Proto::Afc));
    CHECK(Engine::blocks_paint(Proto::Fcp) && Engine::blocks_paint(Proto::Scp) && Engine::blocks_paint(Proto::Vooc));
    CHECK(Engine::blocks_paint(Proto::SuperVooc) && Engine::blocks_paint(Proto::SuperVooc65));
    CHECK(Engine::blocks_paint(Proto::Ufcs) && Engine::blocks_paint(Proto::Vivo));

    FakeCharger v;
    Engine vo(v);
    CHECK(vo.start(Proto::Vooc, 5000, false) == Fail::None);
    bool listening = false;
    for (int i = 0; i < 200 && !listening; i++) { v.run(vo, 1); listening = vo.paint_critical(); }
    CHECK(listening);   // VoocListen: 15 ms slices, a frame between two of them is lost
}

// A charger that logs every pin edge with the fake clock, and can re-enter the engine's hook entry from inside a pin write.
struct EdgeLog : FakeCharger {
    struct Edge { std::uint32_t t; int pin; int drive; };
    std::vector<Edge> edges;
    Engine* reenter = nullptr;
    void set_dp(Drive d) override { edges.push_back({now, 0, int(d)}); FakeCharger::set_dp(d); if (reenter) reenter->service_hs(); }
    void set_dm(Drive d) override { edges.push_back({now, 1, int(d)}); FakeCharger::set_dm(d); if (reenter) reenter->service_hs(); }
};
// The idle hook runs about every 2.2 ms of wire time inside a stream (0.5 ms during a DMA wait); a draw's own work between two rounds
// stretches that, so the model gives the hook a round every kHookGapMs of a paint, 3 ms.
static constexpr unsigned kHookGapMs = 3;
// Start `p` and run `horizon` ms of the main loop. paint_ms = 0: poll() every millisecond (the reference). Otherwise every loop pass
// is preceded by a paint of paint_ms ms that holds the main loop (nothing but the hook runs, one round per kHookGapMs).
static std::vector<EdgeLog::Edge> hs_edges(Proto p, unsigned paint_ms, unsigned horizon, State* end = nullptr, bool reenter = false) {
    EdgeLog c;
    Engine e(c);
    if (reenter) c.reenter = &e;
    CHECK(e.start(p, 9000, true) == Fail::None);
    while (c.now < horizon) {
        for (unsigned i = 1; i <= paint_ms; i++) {
            c.now++;
            c.update_open();
            if (i % kHookGapMs == 0) e.service_hs();
        }
        c.now++;
        c.update_open();
        e.poll();
    }
    if (end) *end = e.state();
    return c.edges;
}
// The same pins in the same order, every wait at least as long as the reference's and at most one hook gap (plus the one poll
// that is already there) longer. The wait for the charger's own drop is polled every 10 ms, so it may land on another grid point.
static void same_pins_and_waits(const std::vector<EdgeLog::Edge>& ref, const std::vector<EdgeLog::Edge>& got, unsigned horizon) {
    std::size_t n = 0;
    while (n < ref.size() && ref[n].t + 40 < horizon) ++n;   // the edges the painted run had time to reach as well
    CHECK(n >= 6);
    CHECK(got.size() >= n);
    for (std::size_t i = 0; i < n && i < got.size(); i++) {
        CHECK(got[i].pin == ref[i].pin && got[i].drive == ref[i].drive);
        if (i == 0) continue;
        const int r = int(ref[i].t - ref[i - 1].t), g = int(got[i].t - got[i - 1].t);
        const bool polled = r >= 1000;   // the drop wait and the 1 s of the QC3 baseline entry
        CHECK(g - r >= (polled ? -10 : 0));
        CHECK(g - r <= int(kHookGapMs) + (polled ? 10 : 0) + 1);
    }
}
// Every Hs phase (discharge, float, bias, wait for the drop) and the hand-over to the QC2 levels / the QC3 baseline, under paints of
// 7 ms (a text row) and 20 ms (the largest row repaint): the hook keeps the handshake on its timing, the pin sequence does not move.
static void service_hs_keeps_the_handshake_on_time_under_paints() {
    struct Case { Proto p; unsigned horizon; } const cases[] = {{Proto::Qc2, 1700}, {Proto::Qc3, 2700}, {Proto::Qc3Plus, 2700}};
    for (const Case& k : cases) {
        State s0{}, s7{}, s20{};
        const auto ref = hs_edges(k.p, 0, k.horizon, &s0);
        const auto p7 = hs_edges(k.p, 7, k.horizon, &s7);
        const auto p20 = hs_edges(k.p, 20, k.horizon, &s20);
        same_pins_and_waits(ref, p7, k.horizon);
        same_pins_and_waits(ref, p20, k.horizon);
        CHECK(s7 == s0 && s20 == s0);
        CHECK(s0 == State::Negotiating || s0 == State::Active);
    }
}
// The hook entry reached from inside poll() (a pin write that ends up in a draw) does nothing: no second step of the phase.
static void service_hs_is_guarded_against_poll() {
    const auto plain = hs_edges(Proto::Qc2, 0, 1700);
    const auto nested = hs_edges(Proto::Qc2, 0, 1700, nullptr, true);
    CHECK(nested.size() == plain.size());
    for (std::size_t i = 0; i < plain.size() && i < nested.size(); i++)
        CHECK(nested[i].t == plain[i].t && nested[i].pin == plain[i].pin && nested[i].drive == plain[i].drive);
}
// What the hook never does: serve a protocol that keeps its window blocked (FCP), run a phase past the handshake, abort.
static void service_hs_serves_only_the_qc_handshake() {
    {   // FCP: the hook alone never moves a pin past the start
        EdgeLog c; Engine e(c);
        CHECK(e.start(Proto::Fcp, 9000, true) == Fail::None);
        const std::size_t n0 = c.edges.size();
        for (int i = 0; i < 400; i++) { c.now++; c.update_open(); e.service_hs(); }
        CHECK(c.edges.size() == n0);
    }
    {   // QC2 after the handshake: the settle phase is poll()'s
        EdgeLog c; Engine e(c);
        CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
        for (int i = 0; i < 1800; i++) { c.now++; c.update_open(); e.poll(); }
        CHECK(e.state() == State::Active);
        const std::size_t n0 = c.edges.size();
        for (int i = 0; i < 200; i++) { c.now++; c.update_open(); e.service_hs(); }
        CHECK(c.edges.size() == n0 && e.state() == State::Active);
    }
    {   // a charger that never drops D-: the hook waits past the timeout and leaves the verdict to poll()
        EdgeLog c; Engine e(c);
        c.hvdcp = false;
        CHECK(e.start(Proto::Qc2, 9000, true) == Fail::None);
        for (int i = 0; i < 3500; i++) { c.now++; c.update_open(); e.service_hs(); }
        CHECK(e.state() == State::Negotiating);
        for (int i = 0; i < 20; i++) { c.now++; c.update_open(); e.poll(); }
        CHECK(e.state() == State::Failed);
    }
}

// The [FC] state line (fctrig::poll): the detection pass reads "VOOC detected vbus=...", not "failed ... reason=detected",
// because the row passes; a real failure keeps "failed" and its reason.
static void state_log_words() {
    const auto is = [](const char* a, const char* b) { return std::strcmp(a, b) == 0; };
    CHECK(is(log_name(State::Failed, Fail::Detected), "detected"));
    CHECK(!log_is_failure(State::Failed, Fail::Detected));
    CHECK(is(log_name(State::Failed, Fail::NoResponse), "failed"));
    CHECK(log_is_failure(State::Failed, Fail::NoResponse));
    CHECK(is(log_name(State::Active, Fail::None), "active"));
    CHECK(!log_is_failure(State::Active, Fail::None));
    CHECK(is(log_name(State::Idle, Fail::Detected), "idle"));   // only a failed state turns into "detected"
    CHECK(is(log_name(State::Negotiating, Fail::None), "negotiating"));
}

int main() {
    hv_lock_guard();
    state_log_words();
    paint_critical_follows_the_phase();
    service_hs_keeps_the_handshake_on_time_under_paints();
    service_hs_is_guarded_against_poll();
    service_hs_serves_only_the_qc_handshake();
    qc_redetect_waits_the_500ms_hold();
    refusals();
    inline_guard();
    qc2_9v();
    qc2_slow_ramp();
    no_hvdcp();
    plain_sources_are_not_qc();
    hvdcp_5v_needs_the_handshake_and_passes_with_it();
    overvoltage_aborts();
    ceiling_abort();
    stop_resets();
    qc2_retarget();
    qc3_class_b();
    qc3_class_a_cannot_reach_20v();
    qc3_class_a_dead_batches_end_the_20v_ramp_early();
    qc3_class_b_slow_ramp_is_not_cut_off();
    qc3_walk();
    qc3_clamped_to_ceiling();
    qc3plus_walk();
    qc3plus_coarse_step_is_no_qc3plus();
    qc3plus_fine_step_ramps_9_to_12();
    qc3plus_real_overvoltage_still_aborts();
    qc3plus_realistic_chargers();
    qc3_coarse_step_chargers();
    qc3_waits_for_a_stable_vbus_near_the_target();
    qc3_last_steps_are_single_pulses();
    qc3_open_loop_bound_aborts_a_runaway();
    qc3plus_without_response_is_no_response();
    qc3_5v_inside_vsafe5_that_ignores_the_pulse_is_done();
    qc3_5v_above_the_band_is_stepped_down_when_the_charger_answers();
    qc3_5v_outside_vsafe5_that_ignores_the_pulse_still_times_out();
    qc3_without_response_keeps_the_timeout();
    qc3plus_batches_shrink_near_the_target();
    qc3_deadband_never_swallows_a_suite_miss();
    afc_9v_and_12v();
    afc_no_answer_fails();
    afc_ends_on_the_first_unanswered_exchange();
    afc_retry_after_a_missed_ping_goes_on();
    fcp_9v();
    fcp_without_caps_fails();
    fcp_current_register_refused_still_switches();
    scp_flow();
    scp_keep_alive_loss_is_relative_to_the_target();
    scp_detect_only_reads_without_writing();
    scp_detect_only_still_rejects_a_non_scp_adapter();
    scp_detect_rejects_an_all_ones_type_byte();
    scp_detect_needs_a_valid_ack();
    scp_real_anker_bytes_are_detected();
    scp_low_voltage_writes_the_8bit_set_registers();
    scp_current_follows_the_request();
    scp_caps_are_kept_from_the_detect_session();
    scp_asks_up_to_the_reported_maximum_and_current();
    scp_without_caps_keeps_the_old_cap_and_current();
    scp_session_rereads_caps_and_refuses_above_them();
    scp_not_supported(); scp_down_target_with_unchanged_vbus_is_not_overvoltage();
    unacked_rise_is_unsupported_afc();
    unacked_rise_is_unsupported_fcp();
    unacked_rise_is_unsupported_scp();
    unacked_no_handshake_is_unsupported_fcp_scp();
    unacked_rise_above_the_ceiling_is_still_overvoltage();
    acked_overshoot_is_still_overvoltage();
    scp_clamped_to_ceiling();
    vooc_ident_is_detected_and_nothing_is_answered();
    vooc_wake_forms_alternate();
    vooc_all_ones_frame_is_nothing_heard();
    vooc_other_commands_are_never_answered();
    vooc_vbus_above_5v5_aborts();
    vooc_never_inline();
    vooc_stop_releases();
    vooc_silent_charger();
    vooc_wake_retries_until_the_adapter_clocks();
    vooc_wake_gives_up_after_three_tries();
    svooc_silent_needs_load();
    vooc_overvoltage_aborts();
    std::printf("test_fc_engine: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
