// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cable / connector resistance maths (k2::cable_r): the passive two-point meter and the
// 8-wire Kelvin formulas.
// Expected values are worked by hand from R = dV / dI, not recomputed with the code's formula.
#include "test_util.h"
#include "k2/pd/cable_r.hpp"

using namespace k2::cable_r;

static void test_two_point() {
    Meter m;
    CHECK_EQ(m.estimate().has_value(), 0);
    CHECK_EQ(static_cast<int>(grade(m.estimate())), static_cast<int>(Grade::Waiting));

    // Idle at 5.10 V / 100 mA, then the load ramps to 1.60 A and the bus sags to 4.80 V:
    // dV = 300 mV over dI = 1500 mA = 0.2 ohm.
    m.sample(5100, 100);
    CHECK_EQ(m.has_idle(), 1);
    CHECK_EQ(m.estimate().has_value(), 0);
    m.sample(4800, 1600);
    CHECK_EQ(m.estimate().has_value(), 1);
    CHECK_EQ(m.estimate()->mohm, 200);
    CHECK_EQ(m.estimate()->idle_mv, 5100);
    CHECK_EQ(m.estimate()->idle_ma, 100);
    CHECK_EQ(m.estimate()->load_mv, 4800);
    CHECK_EQ(m.estimate()->load_ma, 1600);
    CHECK_EQ(static_cast<int>(grade(m.estimate())), static_cast<int>(Grade::Good));

    // Reverse current counts by magnitude.
    Meter r;
    r.sample(5000, -50);
    r.sample(4900, -2050);   // 100 mV / 2.0 A = 50 mohm
    CHECK_EQ(r.estimate()->mohm, 50);
    CHECK_EQ(static_cast<int>(grade(r.estimate())), static_cast<int>(Grade::Excellent));
}

static void test_rounding_and_grades() {
    Meter m;
    m.sample(5000, 0);
    m.sample(4900, 3000);   // 100 / 3.0 = 33.33 -> 33
    CHECK_EQ(m.estimate()->mohm, 33);
    Meter n;
    n.sample(5000, 0);
    n.sample(4900, 1500);   // 66.67 -> 67 (nearest)
    CHECK_EQ(n.estimate()->mohm, 67);

    // Grade bands: <150 excellent, <300 good, <500 fair, else poor.
    Estimate e{};
    e.mohm = 149; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Excellent));
    e.mohm = 150; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Good));
    e.mohm = 299; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Good));
    e.mohm = 300; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Fair));
    e.mohm = 499; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Fair));
    e.mohm = 500; CHECK_EQ(static_cast<int>(grade(e)), static_cast<int>(Grade::Poor));
    CHECK_STR(grade_name(Grade::Waiting), "WAITING FOR LOAD");
    CHECK_STR(grade_name(Grade::Excellent), "EXCELLENT");
    CHECK_STR(grade_name(Grade::Poor), "POOR");
}

static void test_guards() {
    // Load step below 1 A: no estimate yet.
    Meter a;
    a.sample(5000, 100);
    a.sample(4950, 1000);   // dI = 900 mA
    CHECK_EQ(a.estimate().has_value(), 0);
    a.sample(4900, 1100);   // dI = 1000 mA exactly: accepted, 100 / 1.0 = 100 mohm
    CHECK_EQ(a.estimate()->mohm, 100);

    // A load that never had an idle reference (device already drawing 300 mA when plugged in).
    Meter b;
    b.sample(5000, 300);
    b.sample(4800, 2000);
    CHECK_EQ(b.estimate().has_value(), 0);
    CHECK_EQ(b.has_idle(), 0);

    // Voltage rose under load (source regulating up, not a cable drop): rejected.
    Meter c;
    c.sample(5000, 0);
    c.sample(5010, 2000);
    CHECK_EQ(c.estimate().has_value(), 0);

    // More than 2.5 V of drop is not a cable.
    Meter d;
    d.sample(20000, 0);
    d.sample(17000, 2000);
    CHECK_EQ(d.estimate().has_value(), 0);

    // VBUS below the 4.5 V presence level is never an idle reference.
    Meter e;
    e.sample(4000, 0);
    CHECK_EQ(e.has_idle(), 0);
}

static void test_reset_events() {
    Meter m;
    m.sample(5100, 100);
    m.sample(4800, 1600);
    CHECK_EQ(m.estimate().has_value(), 1);
    // Unplug (VBUS gone): the result and the reference are dropped.
    m.sample(0, 0);
    CHECK_EQ(m.estimate().has_value(), 0);
    CHECK_EQ(m.has_idle(), 0);

    // The source steps to another PDO (5 V -> 9 V) between two samples: the old reference is void.
    Meter p;
    p.sample(5100, 100);
    p.sample(9000, 1600);   // a 3.9 V jump, not a cable drop
    CHECK_EQ(p.estimate().has_value(), 0);
    CHECK_EQ(p.has_idle(), 0);
    p.sample(9100, 80);     // new idle at 9 V
    p.sample(8800, 1580);   // 300 mV / 1.5 A = 200 mohm
    CHECK_EQ(p.estimate()->mohm, 200);

    Meter q;
    q.sample(5100, 100);
    q.sample(4800, 1600);
    q.reset();
    CHECK_EQ(q.estimate().has_value(), 0);
    CHECK_EQ(q.has_idle(), 0);
}

static void test_idle_follows_latest() {
    // The idle point is the latest light-load sample, so a slow sag at idle does not poison it.
    Meter m;
    m.sample(5100, 100);
    m.sample(5090, 120);
    m.sample(4790, 1620);   // 300 mV / 1.5 A
    CHECK_EQ(m.estimate()->idle_mv, 5090);
    CHECK_EQ(m.estimate()->mohm, 200);
}

static void test_kelvin() {
    // 1.0 A through the cable. Sense voltages in mV: V_cc2 = 5120, V_dm = 5070, V_cc1 = 5000.
    // R_total = (5120 - 5000) / 1 A = 120 mohm; R_pos = (5120 - 5070) = 50 mohm; R_gnd = (5070 - 5000) = 70 mohm.
    auto k = kelvin(5000, 5120, 5070, 1000);
    CHECK_EQ(k.has_value(), 1);
    CHECK_EQ(k->total_mohm, 120);
    CHECK_EQ(k->pos_mohm, 50);
    CHECK_EQ(k->gnd_mohm, 70);
    CHECK_EQ(k->total_mohm, k->pos_mohm + k->gnd_mohm);

    // 2.0 A halves each figure; current sign does not matter.
    k = kelvin(5000, 5120, 5070, -2000);
    CHECK_EQ(k->total_mohm, 60);
    CHECK_EQ(k->pos_mohm, 25);
    CHECK_EQ(k->gnd_mohm, 35);

    // Below the 0.45 A floor there is no figure; exactly 450 mA is accepted.
    CHECK_EQ(kelvin(5000, 5120, 5070, 449).has_value(), 0);
    CHECK_EQ(kelvin(5000, 5045, 5020, 450).has_value(), 1);   // 45 mV / 0.45 A = 100 mohm
    CHECK_EQ(kelvin(5000, 5045, 5020, 450)->total_mohm, 100);
}

static void test_kelvin_guards_and_notes() {
    // No fixture: CC1 / CC2 / D- float or read 0 V. Zero everywhere is a 0 mohm "result" no one wants,
    // but a reversed ordering (CC2 below D-, or D- below CC1) cannot be a drop and is refused.
    CHECK_EQ(kelvin(5000, 4900, 4950, 1000).has_value(), 0);   // total negative
    CHECK_EQ(kelvin(5000, 5100, 4950, 1000).has_value(), 0);   // gnd leg negative
    CHECK_EQ(kelvin(5000, 5100, 5150, 1000).has_value(), 0);   // pos leg negative
    CHECK_EQ(kelvin(0, 3300, 1650, 1000).has_value(), 0);      // 3.3 V between CC2 and CC1 is not a cable
    CHECK_EQ(kelvin(5000, 7500, 6000, 1000).has_value(), 1);   // exactly 2.5 V total is the largest accepted drop
    CHECK_EQ(kelvin(5000, 7501, 6000, 1000).has_value(), 0);
    CHECK_EQ(kelvin(0, 0, 0, 2000).has_value(), 0);            // no fixture: pins read 0 V, not a 0 mohm cable
    CHECK_EQ(kelvin(5000, 5000, 5000, 2000).has_value(), 0);   // no drop at all: sense wires not connected
    CHECK_EQ(kelvin(5000, 5001, 5000, 2000).has_value(), 1);   // 1 mV is a (tiny) real drop

    // Grades follow the total, like the passive bands: 100 mohm excellent, 200 good, 600 poor.
    CHECK_EQ(static_cast<int>(grade(kelvin(5000, 5100, 5050, 1000))), static_cast<int>(Grade::Excellent));
    CHECK_EQ(static_cast<int>(grade(kelvin(5000, 5200, 5100, 1000))), static_cast<int>(Grade::Good));
    CHECK_EQ(static_cast<int>(grade(kelvin(5000, 5600, 5300, 1000))), static_cast<int>(Grade::Poor));
    CHECK_EQ(static_cast<int>(grade(std::optional<Kelvin>())), static_cast<int>(Grade::Waiting));
    CHECK_EQ(static_cast<int>(grade_mohm(149)), static_cast<int>(Grade::Excellent));
    CHECK_EQ(static_cast<int>(grade_mohm(500)), static_cast<int>(Grade::Poor));

    // The on-screen note differs per mode, says what the figure includes, and fits 21 characters (240 px / 11 px).
    CHECK_STR(note(Mode::Passive, 0), "Passive: includes the");
    CHECK_STR(note(Mode::Passive, 1), "K2's own path.");
    CHECK_STR(note(Mode::Kelvin, 0), "Sense wires: K2's own");
    CHECK_STR(note(Mode::Kelvin, 1), "path not included.");
    for (Mode m : {Mode::Passive, Mode::Kelvin})
        for (int l = 0; l < 2; l++) CHECK_EQ(__builtin_strlen(note(m, l)) <= 21, 1);
}

static const char *fm(char *b, unsigned v) { fmt_mohm(std::span<char>(b, 16), v); return b; }

static void test_formatting() {
    char b[16];
    CHECK_STR(fm(b, 200), "200 m\xCE\xA9");   // U+03A9 in UTF-8
    CHECK_STR(fm(b, 33), "33 m\xCE\xA9");
    CHECK_STR(fm(b, 999), "999 m\xCE\xA9");
    CHECK_STR(fm(b, 1000), "1.00 \xCE\xA9");   // 1 ohm and up
    CHECK_STR(fm(b, 1250), "1.25 \xCE\xA9");
    CHECK_STR(fm(b, 12345), "12.3 \xCE\xA9");
}

int main() {
    test_two_point();
    test_rounding_and_grades();
    test_guards();
    test_reset_events();
    test_idle_follows_latest();
    test_kelvin();
    test_kelvin_guards_and_notes();
    test_formatting();
    return test_report("test_cable_r");
}
