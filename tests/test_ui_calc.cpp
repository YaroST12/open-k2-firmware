// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the pure UI helpers: energy integration and number formatting.
#include "test_util.h"
#include <string>
#include <string.h>
#include <stdint.h>
#include "k2/ui/ui_calc.hpp"

using namespace k2;

static void test_energy() {
    EnergyMeter e;
    e.reset(1000);
    CHECK_EQ(e.mwh(), 0);

    // The first sample only sets the baseline.
    e.update(1000, 5000);
    CHECK_EQ(e.mwh(), 0);

    // 1 W for one hour in 1 s steps = 1 Wh = 1000 mWh.
    EnergyMeter h;
    h.reset(0);
    for (uint32_t t = 0; t <= 3600u * 1000u; t += 1000) h.update(t, 1000);
    CHECK_EQ(h.mwh(), 1000);

    // Trapezoid: ramp 0 -> 2000 mW over 1 s averages 1000 mW; 3.6 s of that is 1 mWh.
    EnergyMeter r;
    r.reset(0);
    r.update(0, 0);
    r.update(1800, 2000);          // avg 1000 mW * 1.8 s = 1800 mJ = 0.5 mWh -> truncates to 0
    CHECK_EQ(r.mwh(), 0);
    r.update(3600, 2000);          // + 2000 mW * 1.8 s -> total 5400 mJ = 1.5 mWh
    CHECK_EQ(r.mwh(), 1);
    r.update(7200, 2000);          // + 7200 mJ -> 12600 mJ = 3.5 mWh
    CHECK_EQ(r.mwh(), 3);

    // Millisecond counter wrap: 512 ms across the wrap at 10 W = 5120 mJ.
    EnergyMeter w;
    w.reset(0xFFFFFF00u);
    w.update(0xFFFFFF00u, 10000);
    w.update(0x00000100u, 10000);
    CHECK_EQ(w.microjoules(), 5120000);

    // Reverse power (current flowing back) is ignored: forward energy only.
    EnergyMeter n;
    n.reset(0);
    n.update(0, -3600);
    n.update(1000, -3600);
    CHECK_EQ(n.microjoules(), 0);
    CHECK_EQ(n.mwh(), 0);

    // Reset clears energy and restarts the clock.
    h.reset(5'000'000);
    CHECK_EQ(h.mwh(), 0);
    CHECK_EQ(h.elapsed_s(5'000'000), 0);
}

static void test_charge() {
    // 1 A for one hour in 1 s steps = 1000 mAh; the energy meter keeps working beside it.
    EnergyMeter h;
    h.reset(0);
    for (uint32_t t = 0; t <= 3600u * 1000u; t += 1000) h.update(t, 5000, 1000);
    CHECK_EQ(h.mah(), 1000);
    CHECK_EQ(h.mwh(), 5000);
    // Trapezoid on the current: 0 -> 2000 mA over 3.6 s = 1 mAh... (1000 mA * 3.6 s = 1 mAh)
    EnergyMeter r;
    r.reset(0);
    r.update(0, 0, 0);
    r.update(3600, 0, 2000);
    CHECK_EQ(r.mah(), 1);
    CHECK_EQ(r.microcoulombs(), 3'600'000);
    // Reverse current is not charge drawn: it counts as 0, like reverse power.
    EnergyMeter n;
    n.reset(0);
    n.update(0, -3600, -1000);
    n.update(3600, -3600, -1000);
    CHECK_EQ(n.mah(), 0);
    // The two-argument update leaves the charge alone; reset clears it.
    EnergyMeter e;
    e.reset(0);
    e.update(0, 1000);
    e.update(3600, 1000);
    CHECK_EQ(e.mah(), 0);
    r.reset(10);
    CHECK_EQ(r.mah(), 0);
}

static void test_hold_card_math() {
    CHECK_EQ(hold_secs_left(0, 5000), 5);
    CHECK_EQ(hold_secs_left(1000, 5000), 4);
    CHECK_EQ(hold_secs_left(2000, 5000), 3);   // the mockup: 3 s in -> "Reboot in 3"
    CHECK_EQ(hold_secs_left(2001, 5000), 3);
    CHECK_EQ(hold_secs_left(4999, 5000), 1);
    CHECK_EQ(hold_secs_left(5000, 5000), 0);
    CHECK_EQ(hold_fill_px(0, 5000, 172), 0);
    CHECK_EQ(hold_fill_px(2000, 5000, 172), 68);   // mockup fill width
    CHECK_EQ(hold_fill_px(5000, 5000, 172), 172);
    CHECK_EQ(hold_fill_px(9000, 5000, 172), 172);
}

// OK + Back held together for 3 s reboots: the card from 1 s, the reboot once at 3 s, release cancels, and the chord
// stays active (its single-key events suppressed) until both keys are up.
static void test_reboot_chord() {
    using S = RebootChord::Step;
    RebootChord c;
    CHECK_EQ(c.active() ? 1 : 0, 0);
    CHECK_EQ(static_cast<int>(c.step(0)), static_cast<int>(S::None));
    c.keys(true, false, 100);                       // OK alone: no chord
    CHECK_EQ(c.active() ? 1 : 0, 0);
    CHECK_EQ(static_cast<int>(c.step(5000)), static_cast<int>(S::None));
    c.keys(true, true, 400);                        // Back joins: the chord starts now
    CHECK_EQ(c.active() ? 1 : 0, 1);
    CHECK_EQ(c.held_ms(1300), 900u);
    CHECK_EQ(static_cast<int>(c.step(1399)), static_cast<int>(S::None));
    CHECK_EQ(static_cast<int>(c.step(1400)), static_cast<int>(S::Card));   // kHoldCardMs in
    CHECK_EQ(hold_secs_left(c.held_ms(1400), kChordRebootMs), 2);
    CHECK_EQ(static_cast<int>(c.step(3399)), static_cast<int>(S::Card));
    CHECK_EQ(static_cast<int>(c.step(3400)), static_cast<int>(S::Reboot));
    CHECK_EQ(static_cast<int>(c.step(3500)), static_cast<int>(S::Card));   // fires once
    // Release one key: cancelled, no card, but still active until the other is up too.
    c.keys(false, true, 3600);
    CHECK_EQ(static_cast<int>(c.step(9000)), static_cast<int>(S::None));
    CHECK_EQ(c.active() ? 1 : 0, 1);
    c.keys(true, true, 3700);                       // pressed again: a new chord, a fresh 3 s
    CHECK_EQ(static_cast<int>(c.step(5000)), static_cast<int>(S::Card));
    CHECK_EQ(static_cast<int>(c.step(6699)), static_cast<int>(S::Card));
    c.keys(false, false, 6699);                     // released before 3 s: cancelled, inactive
    CHECK_EQ(c.active() ? 1 : 0, 0);
    CHECK_EQ(static_cast<int>(c.step(7000)), static_cast<int>(S::None));
    CHECK_EQ(c.held_ms(7000), 0u);
    // Back first, then OK: the same chord.
    c.keys(false, true, 8000);
    CHECK_EQ(c.active() ? 1 : 0, 0);
    c.keys(true, true, 8200);
    CHECK_EQ(static_cast<int>(c.step(11199)), static_cast<int>(S::Card));
    CHECK_EQ(static_cast<int>(c.step(11200)), static_cast<int>(S::Reboot));
    // A late first step (a stalled loop) still reboots: the time is held time, not a window.
    RebootChord d;
    d.keys(true, true, 0);
    CHECK_EQ(static_cast<int>(d.step(20000)), static_cast<int>(S::Reboot));
}

static void test_chevron_and_throttle() {
    // 11 chevrons, one step per 250 ms: Up walks toward the top (index 0), Down toward the bottom.
    CHECK_EQ(chevron_bright(0, 11, true), 10);
    CHECK_EQ(chevron_bright(249, 11, true), 10);
    CHECK_EQ(chevron_bright(250, 11, true), 9);
    CHECK_EQ(chevron_bright(2750, 11, true), 10);    // wraps after 11 steps
    CHECK_EQ(chevron_bright(0, 11, false), 0);
    CHECK_EQ(chevron_bright(500, 11, false), 2);
    // Throttle: first value passes, then one per 250 ms; at most 4 changes a second.
    Throttle t;
    int passes = 0;
    for (uint32_t ms = 1000; ms < 2000; ms += 20) passes += t.pass(ms) ? 1 : 0;
    CHECK_EQ(passes, 4);
    t.reset();
    CHECK_EQ(t.pass(2010) ? 1 : 0, 1);
    CHECK_EQ(t.pass(2100) ? 1 : 0, 0);
    CHECK_EQ(t.pass(2260) ? 1 : 0, 1);
    Throttle wrap;                                       // millis() wrap
    CHECK_EQ(wrap.pass(0xFFFFFF00u) ? 1 : 0, 1);
    CHECK_EQ(wrap.pass(0x10u) ? 1 : 0, 1);
}

static void test_age_and_tracker() {
    char b[16];
    auto age = [&](uint32_t s) { int n = fmt_age(b, s); b[n] = 0; return std::string(b); };
    CHECK_STR(age(0).c_str(), "0:00");
    CHECK_STR(age(42).c_str(), "0:42");
    CHECK_STR(age(60).c_str(), "1:00");
    CHECK_STR(age(599).c_str(), "9:59");
    CHECK_STR(age(600).c_str(), "9:59+");
    CHECK_STR(age(86400).c_str(), "9:59+");
    LiveTracker t;
    CHECK_EQ(t.seen() ? 1 : 0, 0);
    t.note(1000, false);
    CHECK_EQ(t.seen() ? 1 : 0, 0);                    // never live: not stale
    t.note(2000, true);
    t.note(3000, true);
    t.note(4000, false);
    CHECK_EQ(t.seen() ? 1 : 0, 1);
    CHECK_EQ(static_cast<int>(t.age_s(46000)), 43);   // since the last live sample at 3000 ms
    t.clear();
    CHECK_EQ(t.seen() ? 1 : 0, 0);
}

static void test_watt_history() {
    WattHistory h;
    uint8_t lv[WattHistory::kLen];
    // Empty history draws nothing: every level 0.
    h.scaled(lv, 20);
    for (uint8_t v : lv) CHECK_EQ(v, 0);
    // One sample per second: calls inside the same second are ignored.
    h.push(1000, 10000);
    h.push(1500, 99999);
    CHECK_EQ(h.count(), 1);
    h.push(2000, 20000);
    CHECK_EQ(h.count(), 2);
    h.scaled(lv, 20);
    // Newest sample on the right; 10 W -> bottom, 20 W -> top of the 0..19 range.
    CHECK_EQ(lv[WattHistory::kLen - 1], 19);
    CHECK_EQ(lv[WattHistory::kLen - 2], 0);
    // A flat trace sits mid-height instead of exploding noise.
    WattHistory f;
    for (uint32_t t = 0; t < 10; t++) f.push(1000 * t, 5000 + static_cast<int32_t>(t % 2) * 10);
    f.scaled(lv, 20);
    for (int i = WattHistory::kLen - 10; i < WattHistory::kLen; i++) CHECK_EQ(lv[i] >= 7 && lv[i] <= 12, 1);
    // The ring forgets the oldest sample once full.
    WattHistory r;
    for (uint32_t t = 0; t < WattHistory::kLen + 5; t++) r.push(1000 * t, 1000 * static_cast<int32_t>(t));
    CHECK_EQ(r.count(), WattHistory::kLen);
    r.scaled(lv, 20);
    CHECK_EQ(lv[0], 0);
    CHECK_EQ(lv[WattHistory::kLen - 1], 19);
    // Reverse power plots as 0; clear() empties the ring.
    WattHistory n;
    n.push(0, -5000);
    n.push(1000, 4000);
    n.scaled(lv, 20);
    CHECK_EQ(lv[WattHistory::kLen - 2], 0);
    n.clear();
    CHECK_EQ(n.count(), 0);
}

static void test_fit_and_ohms() {
    char b[16];
    // Keeps the most decimals that fit the width: V, A, W and Wh never overflow their D48/S16 field.
    {   // Wh/Ah stacked: same decimals, so the dots line up
        char w[16], a[16];
        CHECK_EQ(fmt_energy_pair(w, a, 1234, 56, 7), 3);  CHECK_STR(w, "  1.234"); CHECK_STR(a, "  0.056");
        CHECK_EQ(fmt_energy_pair(w, a, 1234567, 56, 7), 2); CHECK_STR(w, "1234.57"); CHECK_STR(a, "   0.06");
    }
    fmt_fit(b, 20010, 5, 2); CHECK_STR(b, "20.01");
    fmt_fit(b, 5120, 5, 2);  CHECK_STR(b, " 5.12");
    fmt_fit(b, 2990, 5, 3);  CHECK_STR(b, "2.990");
    fmt_fit(b, 12345, 5, 3); CHECK_STR(b, "12.35");     // one digit more integer part: 3 decimals do not fit
    fmt_fit(b, 99990, 5, 2); CHECK_STR(b, "99.99");
    fmt_fit(b, 123450, 5, 2); CHECK_STR(b, "123.5");
    fmt_fit(b, 240000, 5, 2); CHECK_STR(b, "240.0");
    fmt_fit(b, 1234567, 5, 2); CHECK_STR(b, " 1235");
    fmt_fit(b, 123456789, 5, 2); CHECK_STR(b, "-----");
    fmt_fit(b, -2990, 5, 3); CHECK_STR(b, "2.990");     // magnitude only
    fmt_fit(b, 12345, 6, 3); CHECK_STR(b, "12.345");
    // Load resistance = V / I, from a 50 mA floor.
    fmt_ohms(b, 20000, 2990); CHECK_STR(b, "6.69");
    fmt_ohms(b, 5000, 1000);  CHECK_STR(b, "5.00");
    fmt_ohms(b, 20000, -2990); CHECK_STR(b, "6.69");    // reverse: same magnitude
    fmt_ohms(b, 20000, 1000); CHECK_STR(b, "20.0");
    fmt_ohms(b, 20000, 80);   CHECK_STR(b, " 250");
    fmt_ohms(b, 20000, 49);   CHECK_STR(b, "  --");     // below the floor: open circuit
    fmt_ohms(b, 20000, 0);    CHECK_STR(b, "  --");
    fmt_ohms(b, 0, 2000);     CHECK_STR(b, "0.00");
    fmt_ohms(b, 20000, INT32_MIN); CHECK_STR(b, "0.00");   // INA garbage: no UB, ~0 ohm
}

static void test_elapsed() {
    EnergyMeter e;
    e.reset(1000);
    CHECK_EQ(e.elapsed_s(1000), 0);
    CHECK_EQ(e.elapsed_s(13500), 12);
    CHECK_EQ(e.elapsed_s(1000u + 3'661'000u), 3661);
}

static void test_fixed() {
    char b[16];
    fmt_fixed(b, 20030, 2, 6);  CHECK_STR(b, " 20.03");
    fmt_fixed(b, 1482, 3, 6);   CHECK_STR(b, " 1.482");
    fmt_fixed(b, -1482, 3, 6);  CHECK_STR(b, "-1.482");
    fmt_fixed(b, 29683, 2, 6);  CHECK_STR(b, " 29.68");
    fmt_fixed(b, 0, 3, 6);      CHECK_STR(b, " 0.000");
    fmt_fixed(b, 5, 3, 0);      CHECK_STR(b, "0.005");
    fmt_fixed(b, 1999, 0, 0);   CHECK_STR(b, "2");
    // Rounding: half away from zero, on the magnitude.
    fmt_fixed(b, 1485, 2, 0);   CHECK_STR(b, "1.49");
    fmt_fixed(b, 1484, 2, 0);   CHECK_STR(b, "1.48");
    fmt_fixed(b, -1485, 2, 0);  CHECK_STR(b, "-1.49");
    fmt_fixed(b, 9996, 2, 0);   CHECK_STR(b, "10.00");
    // A negative value that rounds to zero loses its sign.
    fmt_fixed(b, -4, 2, 0);     CHECK_STR(b, "0.00");
    // Exactly the width: no padding. Too wide: dashes fill the field.
    fmt_fixed(b, 240000, 2, 6); CHECK_STR(b, "240.00");
    fmt_fixed(b, 1000000, 2, 6); CHECK_STR(b, "------");
    fmt_fixed(b, -10000, 3, 6); CHECK_STR(b, "------");
    // Extremes must not overflow.
    fmt_fixed(b, INT32_MIN, 3, 0); CHECK_STR(b, "-2147483.648");
    // Return value is the length written.
    CHECK_EQ(fmt_fixed(b, 20030, 2, 6), 6);
    // Small buffer: output truncated, always terminated.
    char s[4];
    fmt_fixed(s, 20030, 2, 6);  CHECK_STR(s, " 20");
}

static void test_u32_compact() {
    char b[16];
    fmt_u32_compact(b, 0);               CHECK_STR(b, "0");
    fmt_u32_compact(b, 9999);            CHECK_STR(b, "9999");
    fmt_u32_compact(b, 10000);           CHECK_STR(b, "10K");
    fmt_u32_compact(b, 12999);           CHECK_STR(b, "12K");        // floors, never rounds up
    fmt_u32_compact(b, 9999999);         CHECK_STR(b, "9999K");
    fmt_u32_compact(b, 10000000);        CHECK_STR(b, "10M");
    fmt_u32_compact(b, 4294967295u);     CHECK_STR(b, "4294M");
    CHECK_EQ(fmt_u32_compact(b, 4294967295u), 5);                    // returns the length written
    StrBuf<24> s;
    s.put_u32_compact(10000).put(" / ").put_u32_compact(4294967295u);
    CHECK_STR(s.c_str(), "10K / 4294M");
}

// Diag counters: the compact form up to 999M, then a capped ">999M": a count that high is saturated or
// has wrapped, so it is never printed as a live number.
static void test_count_capped() {
    char b[16];
    fmt_count_capped(b, 0);             CHECK_STR(b, "0");
    fmt_count_capped(b, 12345);         CHECK_STR(b, "12K");
    fmt_count_capped(b, 999999999u);    CHECK_STR(b, "999M");
    fmt_count_capped(b, 1000000000u);   CHECK_STR(b, ">999M");
    fmt_count_capped(b, 4294967295u);   CHECK_STR(b, ">999M");
    CHECK_EQ(fmt_count_capped(b, 4294967295u), 5);
    CHECK_EQ(count_overflowed(999999999u), 0);
    CHECK_EQ(count_overflowed(1000000000u), 1);
}

static void test_u32_hms_hex() {
    char b[16];
    fmt_u32(b, 0, 0);          CHECK_STR(b, "0");
    fmt_u32(b, 1234, 6);       CHECK_STR(b, "  1234");
    fmt_u32(b, 4294967295u, 0); CHECK_STR(b, "4294967295");
    fmt_u32(b, 12345678, 4);   CHECK_STR(b, "----");   // too wide for the field
    fmt_hms(b, 0);             CHECK_STR(b, "0:00:00");
    fmt_hms(b, 765);           CHECK_STR(b, "0:12:45");
    fmt_hms(b, 3661);          CHECK_STR(b, "1:01:01");
    fmt_hms(b, 43200 + 189);   CHECK_STR(b, "12:03:09");
    fmt_hex(b, 0x2280, 4);     CHECK_STR(b, "2280");
    fmt_hex(b, 0xAB, 4);       CHECK_STR(b, "00AB");
    fmt_hex(b, 0x05AC1002u, 8); CHECK_STR(b, "05AC1002");
}

static void test_strbuf() {
    StrBuf<24> s;
    s.put("V ").put_fixed(20030, 2, 6).put("V").put_char('!');
    CHECK_STR(s.c_str(), "V  20.03V!");
    CHECK_EQ(s.view().size(), 10);

    StrBuf<24> h;
    h.put("0x").put_hex(0xAB, 4).put_char(' ').put_u32(42, 5).put_char(' ').put_hms(3661);
    CHECK_STR(h.c_str(), "0x00AB    42 1:01:01");

    // Overflow truncates and stays terminated; later puts are no-ops.
    StrBuf<6> t;
    t.put("abcdefgh").put("zz");
    CHECK_STR(t.c_str(), "abcde");
    t.clear();
    CHECK_STR(t.c_str(), "");
    t.put("xy");
    CHECK_STR(t.c_str(), "xy");
}

static void test_energy_forward_only() {
    EnergyMeter m;
    m.reset(0);
    m.update(0, 0);
    m.update(1000, 3600000);       /* 3600 W for 1 s, trapezoid from 0 W: 1800 W s = 0.5 Wh */
    CHECK_EQ(m.mwh(), 500);
    const int32_t before = m.mwh();
    m.update(2000, -3600000);      /* reverse power adds nothing and subtracts nothing */
    m.update(3000, -3600000);
    CHECK_EQ(m.mwh() >= before, 1);            /* never goes down on reverse power */
    EnergyMeter r;
    r.reset(0);
    r.update(0, -5000);
    r.update(10000, -5000);
    CHECK_EQ(r.microjoules(), 0);
}

static void test_abs_formatters() {
    char t[24];
    fmt_abs_fixed(t, -1520, 3, 6);  CHECK_STR(t, " 1.520");
    fmt_abs_fixed(t, 1520, 3, 6);   CHECK_STR(t, " 1.520");
    fmt_abs_fixed(t, -2, 3, 6);     CHECK_STR(t, " 0.002");
    fmt_abs_fixed(t, 0, 3, 6);      CHECK_STR(t, " 0.000");
    fmt_abs_fixed(t, -30456, 2, 6); CHECK_STR(t, " 30.46");
    fmt_abs_fixed(t, INT32_MIN, 0, 0); CHECK_STR(t, "2147484");
    /* never a '-' for values that fit the width */
    for (int32_t v : {-1, -9, -10, -11, -999, -1000, -1999, -12345, -99999}) {
        fmt_abs_fixed(t, v, 3, 8);
        CHECK_EQ(strchr(t, '-') == nullptr, 1);
        StrBuf<16> b;
        b.put_abs_fixed(v * 1000, 0, 0);
        CHECK_EQ(strchr(b.c_str(), '-') == nullptr, 1);
    }
}

// Home numbers: V, A and W share one decimal count, so right-aligned they line up on the dot. The
// 145 px field holds 5 characters: 2 decimals up to 99.99 W, 1 decimal from 100 W (hysteresis).
static void test_home_decimals() {
    CHECK_EQ(home_decimals(0, 2), 2);
    CHECK_EQ(home_decimals(INT32_MIN, 2), 1);   // saturates, no INT32_MIN negation
    CHECK_EQ(home_decimals(99000, 2), 2);
    CHECK_EQ(home_decimals(99990, 2), 1);       // would round to "100.00": 6 characters
    CHECK_EQ(home_decimals(-120000, 2), 1);     // magnitude, flow direction is not a sign
    CHECK_EQ(home_decimals(98500, 1), 1);       // stays until clearly below
    CHECK_EQ(home_decimals(97000, 1), 2);
    for (int dec : {2, 1}) {
        char v[16], a[16], w[16];
        const int32_t mw = dec == 2 ? 30456 : 123456;
        fmt_home(v, 20030, dec, 5); fmt_home(a, 1520, dec, 5); fmt_home(w, mw, dec, 5);
        // The dot sits at the same index in all three right-aligned strings.
        CHECK_EQ(strchr(v, '.') - v, strchr(a, '.') - a);
        CHECK_EQ(strchr(v, '.') - v, strchr(w, '.') - w);
        CHECK_EQ(int(strlen(v)), 5); CHECK_EQ(int(strlen(a)), 5); CHECK_EQ(int(strlen(w)), 5);
    }
    char t[16];
    fmt_home(t, 20030, 2, 5);  CHECK_STR(t, "20.03");
    fmt_home(t, 1520, 2, 5);   CHECK_STR(t, " 1.52");
    fmt_home(t, 5100, 2, 5);   CHECK_STR(t, " 5.10");
    fmt_home(t, 123456, 1, 5); CHECK_STR(t, "123.5");
    fmt_home(t, 20030, 1, 5);  CHECK_STR(t, " 20.0");
    fmt_home(t, 5100, 1, 5);   CHECK_STR(t, "  5.1");
    fmt_home(t, -240000, 1, 5); CHECK_STR(t, "240.0");
}

int main() {
    test_home_decimals();
    test_energy_forward_only();
    test_abs_formatters();
    test_energy();
    test_charge();
    test_fit_and_ohms();
    test_hold_card_math();
    test_reboot_chord();
    test_chevron_and_throttle();
    test_age_and_tracker();
    test_watt_history();
    test_elapsed();
    test_fixed();
    test_u32_compact();
    test_count_capped();
    test_u32_hms_hex();
    test_strbuf();
    return test_report("test_ui_calc");
}
