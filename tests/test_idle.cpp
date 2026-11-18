// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The idle machine (k2/core/idle.hpp): pure timing and activity rules for the screen dim / off / low-clock levers. The firmware
// glue (the backlight, the panel, the HCLK divider, which remote commands count as activity) is in src/core/idle_hw.cpp; this
// pins the arithmetic: the timeouts and their 0 = never, every activity source, the swallowed wake press, the force overrides,
// the low-clock gate and the 32-bit clock wrap.
#include "test_util.h"
#include "k2/core/idle.hpp"

using namespace k2;

static idle::Inputs quiet(uint32_t now) {   // nothing attached: VBUS 0
    idle::Inputs in;
    in.now_ms = now;
    in.vbus_known = true;
    in.vbus_mv = 0;
    in.ibus_ma = 0;
    return in;
}

static idle::Inputs attached(uint32_t now) {   // a source on the measured port, nothing flowing
    idle::Inputs in = quiet(now);
    in.vbus_mv = 5000;
    return in;
}

static void test_defaults_dim_then_off() {
    idle::Machine m;
    CHECK_EQ(m.config().dim_after_s, 60);
    CHECK_EQ(m.config().off_after_s, 300);
    CHECK_EQ(m.config().brightness, 100);
    CHECK_EQ(m.config().dim_brightness, 30);
    CHECK_EQ(m.tick(quiet(0)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(59999)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(60000)).state == idle::State::Dim, true);
    CHECK_EQ(m.tick(quiet(299999)).state == idle::State::Dim, true);
    CHECK_EQ(m.tick(quiet(300000)).state == idle::State::Off, true);
    CHECK_EQ(m.tick(quiet(900000)).state == idle::State::Off, true);
}

static void test_outputs_per_state() {
    idle::Machine m;
    idle::Output o = m.tick(quiet(0));
    CHECK_EQ(o.brightness, 100);
    CHECK_EQ(o.panel_on, true);
    CHECK_EQ(o.paint, true);
    CHECK_EQ(o.low_clock_ok, false);
    o = m.tick(quiet(60000));
    CHECK_EQ(o.brightness, 30);        // dim: the content stays, only the backlight drops
    CHECK_EQ(o.panel_on, true);
    CHECK_EQ(o.paint, true);
    CHECK_EQ(o.low_clock_ok, true);    // and nobody is looking: the core runs at the low clock, as in Off
    o = m.tick(quiet(300000));
    CHECK_EQ(o.brightness, 0);
    CHECK_EQ(o.panel_on, false);
    CHECK_EQ(o.paint, false);
    CHECK_EQ(o.low_clock_ok, true);
}

static void test_dim_never_brighter_than_the_brightness() {
    idle::Machine m;
    idle::Config c = m.config();
    c.brightness = 20;
    c.dim_brightness = 60;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(0)).brightness, 20);
    CHECK_EQ(m.tick(quiet(60000)).brightness, 20);
}

// The Display page changes the brightness while Active: the very next tick carries it (the glue applies it each pass).
static void test_brightness_change_applies_at_once() {
    idle::Machine m;
    CHECK_EQ(m.tick(quiet(0)).brightness, 100);
    idle::Config c = m.config();
    c.brightness = 40;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(1000)).brightness, 40);
}

static void test_zero_means_never() {
    idle::Machine m;
    idle::Config c = m.config();
    c.dim_after_s = 0;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(0)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(200000)).state == idle::State::Active, true);   // no dim step
    CHECK_EQ(m.tick(quiet(300000)).state == idle::State::Off, true);

    idle::Machine n;
    c = n.config();
    c.off_after_s = 0;
    n.set_config(c);
    CHECK_EQ(n.tick(quiet(0)).state == idle::State::Active, true);
    CHECK_EQ(n.tick(quiet(60000)).state == idle::State::Dim, true);
    CHECK_EQ(n.tick(quiet(7200000)).state == idle::State::Dim, true);     // never off

    idle::Machine p;
    c = p.config();
    c.dim_after_s = 0;
    c.off_after_s = 0;
    p.set_config(c);
    CHECK_EQ(p.tick(quiet(0)).state == idle::State::Active, true);
    CHECK_EQ(p.tick(quiet(86400000)).state == idle::State::Active, true);
}

static void test_off_before_dim_goes_straight_to_off() {
    idle::Machine m;
    idle::Config c = m.config();
    c.dim_after_s = 100;
    c.off_after_s = 50;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(0)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(49999)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(50000)).state == idle::State::Off, true);
}

static void test_press_in_active_is_delivered_and_restarts_the_timer() {
    idle::Machine m;
    m.tick(quiet(0));
    m.tick(quiet(50000));
    CHECK_EQ(m.press(50000), true);
    CHECK_EQ(m.tick(quiet(109999)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(110000)).state == idle::State::Dim, true);
}

static void test_press_in_dim_and_off_only_wakes() {
    idle::Machine m;
    m.tick(quiet(0));
    m.tick(quiet(60000));
    CHECK_EQ(m.press(61000), false);                 // swallowed: it only woke the screen
    CHECK_EQ(m.tick(quiet(61000)).state == idle::State::Active, true);
    CHECK_EQ(m.press(62000), true);                  // the next one is a normal press
    m.tick(quiet(400000));
    CHECK_EQ(m.state() == idle::State::Off, true);
    CHECK_EQ(m.press(400010), false);
    CHECK_EQ(m.tick(quiet(400020)).state == idle::State::Active, true);
}

static void test_press_between_ticks_sees_the_state_at_that_moment() {
    idle::Machine m;
    m.tick(quiet(0));
    // no tick between 0 and the press: the press itself advances the clock
    CHECK_EQ(m.press(70000), false);
    CHECK_EQ(m.tick(quiet(70020)).state == idle::State::Active, true);
}

static void test_wake_flag_is_reported_once_when_leaving_off() {
    idle::Machine m;
    m.tick(quiet(0));
    idle::Output o = m.tick(quiet(300000));
    CHECK_EQ(o.woke, false);
    CHECK_EQ(m.press(300100), false);
    o = m.tick(quiet(300120));
    CHECK_EQ(o.woke, true);                          // the UI repaints everything on this edge
    o = m.tick(quiet(300140));
    CHECK_EQ(o.woke, false);
    // Dim -> Active is not a wake from Off
    m.tick(quiet(300140 + 60000));
    CHECK_EQ(m.state() == idle::State::Dim, true);
    CHECK_EQ(m.press(360200), false);
    CHECK_EQ(m.tick(quiet(360220)).woke, false);
}

static void test_vbus_step_counts_as_activity() {
    idle::Machine m;
    m.tick(attached(0));                             // the first sample only sets the reference
    idle::Inputs in = quiet(30000);
    in.vbus_mv = 5200;                               // 200 mV: below max(250 mV, 2.5 %)
    m.tick(in);
    in = quiet(60000);
    in.vbus_mv = 5200;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);   // 200 mV did not restart the timer
    in = quiet(61000);
    in.vbus_mv = 5300;                               // 300 mV from the 5 V reference
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in = quiet(120999);
    in.vbus_mv = 5300;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 121000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
}

static void test_vbus_threshold_scales_to_2p5_percent_at_high_voltage() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    in.vbus_mv = 20000;
    m.tick(in);
    in.now_ms = 60000;
    in.vbus_mv = 20400;                              // 400 mV < 2.5 % of 20 V (500 mV)
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in.now_ms = 60020;
    in.vbus_mv = 20600;                              // 600 mV from the 20 V reference
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
}

static void test_vbus_appearing_and_disappearing_wake() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    in.vbus_mv = 0;
    m.tick(in);
    in.now_ms = 60000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in.now_ms = 60020;
    in.vbus_mv = 5000;                               // a charger arrives
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 360020;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);   // attached: Dim, never Off
    in.now_ms = 360040;
    in.vbus_mv = 0;                                  // and leaves
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
}

static void test_vbus_slow_drift_adds_up_against_the_reference() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    m.tick(in);
    uint32_t t = 0;
    int32_t mv = 5000;
    bool woke = false;
    for (int i = 0; i < 20; ++i) {                   // 20 steps of 20 mV: each far below the threshold, together 400 mV
        t += 4000;
        mv += 20;
        in.now_ms = t;
        in.vbus_mv = mv;
        idle::State s = m.tick(in).state;
        if (mv - 5000 < 250) CHECK_EQ(s == idle::State::Active || s == idle::State::Dim, true);
        if (m.idle_s() == 0 && mv - 5000 >= 250) woke = true;
    }
    CHECK_EQ(woke, true);                            // the drift moved the reference once it passed 250 mV
}

static void test_vbus_unknown_is_ignored() {
    idle::Machine m;
    m.tick(quiet(0));
    idle::Inputs in = quiet(60000);
    in.vbus_known = false;
    in.vbus_mv = 0;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in = quiet(60020);                               // the monitor answers again with the old value: no step
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
}

static void test_bus_current_counts_as_activity() {
    idle::Machine m;
    m.tick(quiet(0));
    idle::Inputs in = quiet(60000);
    in.ibus_ma = 50;                                 // not above 50 mA
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in.now_ms = 60020;
    in.ibus_ma = 51;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 200000;
    in.ibus_ma = -400;                               // the sign does not matter
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 200000 + 59999;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);   // load still there: it never idles
}

static void test_pd_event_counts_as_activity() {
    idle::Machine m;
    m.tick(quiet(0));
    idle::Inputs in = quiet(60000);
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in.now_ms = 60020;
    in.pd_event = true;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 360020;
    in.pd_event = false;
    CHECK_EQ(m.tick(in).state == idle::State::Off, true);
    in.now_ms = 360040;
    in.pd_event = true;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
}

static void test_busy_never_idles_and_restarts_the_timer_when_it_ends() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    in.busy = true;
    for (uint32_t t = 0; t <= 1000000; t += 20000) {
        in.now_ms = t;
        CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    }
    in.busy = false;
    in.now_ms = 1000000 + 59999;
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    in.now_ms = 1000000 + 60000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
}

static void test_alarm_holds_the_screen_awake() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    in.alarm = true;
    for (uint32_t t = 0; t <= 600000; t += 20000) {
        in.now_ms = t;
        CHECK_EQ(m.tick(in).state == idle::State::Active, true);
    }
    in.now_ms = 600020;
    in.alarm = false;
    m.tick(in);
    in.now_ms = 600020 + 60000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
    in.now_ms += 20;
    in.alarm = true;                                 // an alarm while dimmed brings the screen back
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);
}

static void test_remote_activity_wakes_only_with_the_flag() {
    idle::Machine m;
    m.tick(quiet(0));
    m.tick(quiet(60000));
    CHECK_EQ(m.config().wake_on_remote, true);       // default: a state-changing remote command wakes
    m.remote_activity(60100);
    CHECK_EQ(m.tick(quiet(60120)).state == idle::State::Active, true);
    idle::Config c = m.config();
    c.wake_on_remote = false;
    m.set_config(c);
    m.tick(quiet(120120));
    CHECK_EQ(m.state() == idle::State::Dim, true);
    m.remote_activity(120200);
    CHECK_EQ(m.tick(quiet(120220)).state == idle::State::Dim, true);
    m.tick(quiet(420220));
    m.remote_activity(420230);
    CHECK_EQ(m.tick(quiet(420240)).state == idle::State::Off, true);
}

static void test_force_overrides() {
    idle::Machine m;
    m.tick(quiet(0));
    m.force(idle::Force::Off);
    idle::Output o = m.tick(quiet(1000));
    CHECK_EQ(o.state == idle::State::Off, true);
    CHECK_EQ(o.panel_on, false);
    m.force(idle::Force::Dim);
    CHECK_EQ(m.tick(quiet(1020)).state == idle::State::Dim, true);
    m.force(idle::Force::Active);
    CHECK_EQ(m.tick(quiet(1000000)).state == idle::State::Active, true);   // held awake far past the timeouts
    m.force(idle::Force::Auto);
    CHECK_EQ(m.tick(quiet(1000020)).state == idle::State::Active, true);   // auto restarts from the last activity
    CHECK_EQ(m.tick(quiet(1000020 + 60000)).state == idle::State::Dim, true);
}

static void test_force_off_survives_activity_but_a_press_clears_it() {
    idle::Machine m;
    m.tick(quiet(0));
    m.force(idle::Force::Off);
    idle::Inputs in = quiet(1000);
    in.ibus_ma = 900;
    in.pd_event = true;
    CHECK_EQ(m.tick(in).state == idle::State::Off, true);    // a forced state is the caller's measurement setup
    m.remote_activity(1100);
    CHECK_EQ(m.tick(quiet(1120)).state == idle::State::Off, true);
    CHECK_EQ(m.press(1200), false);                          // a press only wakes, and drops the force
    CHECK_EQ(m.force_mode() == idle::Force::Auto, true);
    CHECK_EQ(m.tick(quiet(1220)).state == idle::State::Active, true);
}

static void test_force_active_delivers_the_press() {
    idle::Machine m;
    m.force(idle::Force::Active);
    m.tick(quiet(0));
    CHECK_EQ(m.press(500), true);
    CHECK_EQ(m.force_mode() == idle::Force::Active, true);
}

static void test_attached_stops_at_dim() {
    idle::Machine m;
    m.tick(attached(0));
    CHECK_EQ(m.tick(attached(59999)).state == idle::State::Active, true);
    idle::Output o = m.tick(attached(60000));
    CHECK_EQ(o.state == idle::State::Dim, true);
    CHECK_EQ(o.brightness, 30);
    o = m.tick(attached(300000));
    CHECK_EQ(o.state == idle::State::Dim, true);     // the off timeout passed: still Dim with a source attached
    CHECK_EQ(o.panel_on, true);
    CHECK_EQ(o.paint, true);
    CHECK_EQ(o.low_clock_ok, true);
    CHECK_EQ(m.tick(attached(3600000)).state == idle::State::Dim, true);
    CHECK_EQ(m.tick(attached(86400000)).state == idle::State::Dim, true);
}

static void test_detach_wakes_then_goes_off() {
    idle::Machine m;
    m.tick(attached(0));
    CHECK_EQ(m.tick(attached(400000)).state == idle::State::Dim, true);
    CHECK_EQ(m.tick(quiet(400020)).state == idle::State::Active, true);   // unplugging is activity
    CHECK_EQ(m.tick(quiet(459999)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(460020)).state == idle::State::Dim, true);
    idle::Output o = m.tick(quiet(700020));
    CHECK_EQ(o.state == idle::State::Off, true);
    CHECK_EQ(o.low_clock_ok, true);
    CHECK_EQ(m.tick(attached(700040)).state == idle::State::Active, true);   // plugging in wakes, and the timer restarts
    CHECK_EQ(m.tick(attached(1100040)).state == idle::State::Dim, true);
}

static void test_presence_level_is_the_ui_live_level() {
    idle::Machine m;
    idle::Inputs in = quiet(0);
    in.vbus_mv = 900;                                // under 1 V: a leak or a sagging dead source, not attached
    m.tick(in);
    in.now_ms = 300000;
    CHECK_EQ(m.tick(in).state == idle::State::Off, true);
    in.now_ms = 300020;
    in.vbus_mv = 1000;                               // 1 V and up counts as attached (a step below the 250 mV rule: the
    CHECK_EQ(m.tick(in).state == idle::State::Active, true);   // crossing itself is the activity)
    in.now_ms = 700000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
}

static void test_unknown_vbus_never_goes_off() {
    idle::Machine m;
    idle::Inputs in;                                 // no power monitor reading at all
    in.now_ms = 0;
    m.tick(in);
    in.now_ms = 400000;
    CHECK_EQ(m.tick(in).state == idle::State::Dim, true);
}

static void test_unknown_after_a_reading_keeps_the_last_presence() {
    idle::Machine m;
    m.tick(quiet(0));                                // known: nothing attached
    idle::Inputs in;
    in.now_ms = 400000;                              // the meter drops out
    CHECK_EQ(m.tick(in).state == idle::State::Off, true);
}

static void test_attached_force_off_and_off_before_dim() {
    idle::Machine m;
    m.tick(attached(0));
    m.force(idle::Force::Off);                       // an explicit bench pin still blanks the panel
    CHECK_EQ(m.tick(attached(20)).state == idle::State::Off, true);
    m.force(idle::Force::Auto);
    idle::Config c = m.config();
    c.dim_after_s = 120;
    c.off_after_s = 100;                             // the off step comes first: attached, it lands on Dim
    m.set_config(c);
    m.tick(attached(100));
    CHECK_EQ(m.tick(attached(100100)).state == idle::State::Dim, true);
    CHECK_EQ(m.tick(attached(200000)).state == idle::State::Dim, true);
}

static void test_low_clock_gate() {
    idle::Machine m;
    m.tick(quiet(0));
    idle::Inputs in = quiet(300000);
    CHECK_EQ(m.tick(in).low_clock_ok, true);
    in.now_ms = 300020;
    in.pd_negotiating = true;                        // a contract in progress: full clock
    CHECK_EQ(m.tick(in).low_clock_ok, false);
    CHECK_EQ(m.state() == idle::State::Off, true);   // the screen stays off, only the clock is held up
    in.now_ms = 300040;
    in.pd_negotiating = false;
    CHECK_EQ(m.tick(in).low_clock_ok, true);
    in.busy = true;                                  // busy wakes and never lowers the clock
    in.now_ms = 300060;
    idle::Output o = m.tick(in);
    CHECK_EQ(o.low_clock_ok, false);
    CHECK_EQ(o.state == idle::State::Active, true);
    m.force(idle::Force::Dim);
    in.busy = false;
    in.now_ms = 300080;
    CHECK_EQ(m.tick(in).low_clock_ok, true);         // Dim takes the same low clock
    in.pd_negotiating = true;
    in.now_ms = 300100;
    CHECK_EQ(m.tick(in).low_clock_ok, false);        // with the same blocker
    m.force(idle::Force::Active);
    in.pd_negotiating = false;
    in.now_ms = 300120;
    CHECK_EQ(m.tick(in).low_clock_ok, false);        // never in Active
}

static void test_dim_low_clock_ends_with_any_activity_in_the_same_tick() {
    idle::Machine m;
    m.tick(attached(0));
    CHECK_EQ(m.tick(attached(60000)).low_clock_ok, true);
    idle::Inputs in = attached(60020);
    in.pd_event = true;
    idle::Output o = m.tick(in);
    CHECK_EQ(o.state == idle::State::Active, true);
    CHECK_EQ(o.low_clock_ok, false);                 // full clock before the PD work is handled
    CHECK_EQ(m.tick(attached(120020)).low_clock_ok, true);
    in = attached(120040);
    in.ibus_ma = 60;
    CHECK_EQ(m.tick(in).low_clock_ok, false);
    CHECK_EQ(m.tick(attached(180040)).low_clock_ok, true);
    in = attached(180060);
    in.vbus_mv = 9000;
    o = m.tick(in);
    CHECK_EQ(o.state == idle::State::Active, true);
    CHECK_EQ(o.low_clock_ok, false);
    m.tick(attached(240060));
    CHECK_EQ(m.tick(attached(300060)).low_clock_ok, true);
    CHECK_EQ(m.press(300080), false);                // the wake press
    CHECK_EQ(m.tick(attached(300100)).low_clock_ok, false);
}

// A remote 0 % is a runtime level: the machine honours it (the saved copy is idle_cfg's business, tests/test_idle_cfg.cpp).
static void test_zero_brightness_is_honoured_at_runtime() {
    idle::Machine m;
    idle::Config c = m.config();
    c.brightness = 0;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(0)).brightness, 0);
    CHECK_EQ(m.tick(quiet(60000)).brightness, 0);    // dim never above the brightness
    c.brightness = 50;
    c.dim_brightness = 0;
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(60020)).brightness, 0);
}

static void test_low_clock_is_off_during_a_forced_off_with_a_negotiation() {
    idle::Machine m;
    m.tick(quiet(0));
    m.force(idle::Force::Off);
    idle::Inputs in = quiet(10);
    in.pd_negotiating = true;
    CHECK_EQ(m.tick(in).low_clock_ok, false);
    in.pd_negotiating = false;
    in.now_ms = 30;
    CHECK_EQ(m.tick(in).low_clock_ok, true);
}

static void test_idle_seconds_and_config_change() {
    idle::Machine m;
    m.tick(quiet(0));
    m.tick(quiet(12345));
    CHECK_EQ(m.idle_s(), 12);
    idle::Config c = m.config();
    c.dim_after_s = 10;                              // shortening the timeout applies to the idle time already spent
    m.set_config(c);
    CHECK_EQ(m.tick(quiet(12365)).state == idle::State::Dim, true);
}

static void test_clock_wrap_and_long_gaps() {
    idle::Machine m;
    m.tick(quiet(0xFFFFFFFFu - 30000u));
    CHECK_EQ(m.tick(quiet(0xFFFFFFFFu - 1u)).state == idle::State::Active, true);
    CHECK_EQ(m.tick(quiet(40000u)).state == idle::State::Dim, true);        // 70 s after the first tick, across the wrap
    CHECK_EQ(m.tick(quiet(310000u)).state == idle::State::Off, true);
    // a very long idle never wraps back to Active: the idle time saturates
    idle::Machine n;
    n.tick(quiet(0));
    uint32_t t = 0;
    for (int i = 0; i < 6; ++i) {
        t += 0x40000000u;                            // 12 days per step, five and a half times the 2^32 ms wrap
        CHECK_EQ(n.tick(quiet(t)).state == idle::State::Off, true);
    }
}

int main() {
    test_defaults_dim_then_off();
    test_outputs_per_state();
    test_dim_never_brighter_than_the_brightness();
    test_brightness_change_applies_at_once();
    test_zero_means_never();
    test_off_before_dim_goes_straight_to_off();
    test_press_in_active_is_delivered_and_restarts_the_timer();
    test_press_in_dim_and_off_only_wakes();
    test_press_between_ticks_sees_the_state_at_that_moment();
    test_wake_flag_is_reported_once_when_leaving_off();
    test_vbus_step_counts_as_activity();
    test_vbus_threshold_scales_to_2p5_percent_at_high_voltage();
    test_vbus_appearing_and_disappearing_wake();
    test_vbus_slow_drift_adds_up_against_the_reference();
    test_vbus_unknown_is_ignored();
    test_bus_current_counts_as_activity();
    test_pd_event_counts_as_activity();
    test_busy_never_idles_and_restarts_the_timer_when_it_ends();
    test_alarm_holds_the_screen_awake();
    test_remote_activity_wakes_only_with_the_flag();
    test_force_overrides();
    test_force_off_survives_activity_but_a_press_clears_it();
    test_force_active_delivers_the_press();
    test_attached_stops_at_dim();
    test_detach_wakes_then_goes_off();
    test_presence_level_is_the_ui_live_level();
    test_unknown_vbus_never_goes_off();
    test_unknown_after_a_reading_keeps_the_last_presence();
    test_attached_force_off_and_off_before_dim();
    test_low_clock_gate();
    test_low_clock_is_off_during_a_forced_off_with_a_negotiation();
    test_dim_low_clock_ends_with_any_activity_in_the_same_tick();
    test_zero_brightness_is_honoured_at_runtime();
    test_idle_seconds_and_config_change();
    test_clock_wrap_and_long_gaps();
    return test_report("test_idle");
}
