// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 pure model: alarms, limit bar, sweep maths, number formats, protocol row, exception badge.
#include <string>
#include "test_util.h"
#include "k2/ui/home_model.hpp"
#include "diag_fixture.h"

using namespace k2;
using namespace k2::home;

static void test_alarm_thresholds() {
    CHECK_EQ(static_cast<int>(alarm_of(4499, 5000)), static_cast<int>(Alarm::None));
    CHECK_EQ(static_cast<int>(alarm_of(4500, 5000)), static_cast<int>(Alarm::Near));   // 90 % is Near
    CHECK_EQ(static_cast<int>(alarm_of(5000, 5000)), static_cast<int>(Alarm::Near));   // at the limit is not over it
    CHECK_EQ(static_cast<int>(alarm_of(5001, 5000)), static_cast<int>(Alarm::Over));
    CHECK_EQ(static_cast<int>(alarm_of(9000, 0)), static_cast<int>(Alarm::None));      // unknown limit: no alarm
    CHECK_EQ(pct_of(4620, 5000), 92);
    CHECK_EQ(pct_of(0, 0), 0);
}

static void test_limit_bar() {
    CHECK_EQ(bar_fill_w(0, 5000), 0);
    CHECK_EQ(bar_fill_w(10, 0), 0);
    CHECK_EQ(bar_fill_w(5000, 5000), kBarTrack);
    CHECK_EQ(bar_fill_w(9000, 5000), kBarTrack);      // over the limit: full track, never past it
    CHECK_EQ(bar_fill_w(2500, 5000), 103);            // round(204 / 2) + 1
    CHECK_EQ(bar_tick_x(), 194);
}

static void test_sweep_maths() {
    CHECK_EQ(scale_v10(20000), 2300);                 // contract max x 1.15, in 10 mV
    CHECK_EQ(scale_a10(5000), 575);
    CHECK_EQ(scale_v10(0), 2300);                     // unknown contract: 20 V
    const int span = kVoltCell.span();
    CHECK_EQ(span, 67);
    CHECK_EQ(sweep_rows(0, 2300, span), 0);
    CHECK_EQ(sweep_rows(1, 2300, span), 0);           // 10 mV or less: no column, like the mockup
    CHECK_EQ(sweep_rows(0xFFFF, 2300, span), 0);      // kNone
    CHECK_EQ(sweep_rows(2300, 2300, span), span + 1);
    CHECK_EQ(sweep_rows(9999, 2300, span), span + 1); // clipped at full scale
    CHECK_EQ(sweep_rows(1150, 2300, span), 35);       // half scale: round(33.5) + 1
    CHECK_EQ(kAmpCell.span(), 59);
    CHECK_EQ(sweep_rows(575, 575, kAmpCell.span()), 60);
}

static void test_sweep_columns() {
    CHECK_EQ(x_of_col(0), 4);
    CHECK_EQ(x_of_col(109), 222);
    CHECK_EQ(col_of_x(223), 109);
    CHECK_EQ(static_cast<int>(step_ms(0)), 91);       // 110 columns over 10 s, 60 s, 10 min
    CHECK_EQ(static_cast<int>(step_ms(1)), 545);
    CHECK_EQ(static_cast<int>(step_ms(2)), 5455);
}

static void test_hero_numbers() {
    char b[16];
    int n = fmt_hero(b, 20030);
    CHECK_STR(b, "20.03");
    CHECK_EQ(n, 5);
    fmt_hero(b, 5123); CHECK_STR(b, "5.123");
    fmt_hero(b, -1830); CHECK_STR(b, "1.830");        // magnitude only: direction is the arrow's job
    fmt_hero(b, 0); CHECK_STR(b, "0.000");
    fmt_hero(b, 9060); CHECK_STR(b, "9.060");
    fmt_hero(b, 36000); CHECK_STR(b, "36.00");
    fmt_hero(b, 100400); CHECK_STR(b, "100.4");
}

static void test_watts_and_energy() {
    char b[16];
    fmt_watts(b, 36600); CHECK_STR(b, "36.6");
    fmt_watts(b, 9990); CHECK_STR(b, "9.99");
    fmt_watts(b, 99960); CHECK_STR(b, "100");         // rounding up must not overflow the 4 characters
    fmt_watts(b, 140000); CHECK_STR(b, "140");
    fmt_watts(b, 0); CHECK_STR(b, "0.00");
    fmt_energy(b, 1234); CHECK_STR(b, "1.23");
    fmt_energy(b, 12345); CHECK_STR(b, "12.3");
    fmt_energy(b, 123456); CHECK_STR(b, "123");
    fmt_energy(b, 1234567); CHECK_STR(b, "1.2k");
    fmt_energy(b, 12345678); CHECK_STR(b, "12k");
    fmt_energy(b, -500); CHECK_STR(b, "0.50");
    const DecSplit a = split_decimal("12.3");
    CHECK_STR(std::string(a.int_part).c_str(), "12");
    CHECK_STR(std::string(a.rest).c_str(), ".3");
    const DecSplit z = split_decimal("123");
    CHECK_STR(std::string(z.int_part).c_str(), "123");
    CHECK_EQ(z.rest.size(), 0);
    const DecSplit k = split_decimal("1.2k");
    CHECK_STR(std::string(k.rest).c_str(), ".2k");
}

static void test_elapsed() {
    char b[16];
    fmt_elapsed(b, 0); CHECK_STR(b, "0:00");
    fmt_elapsed(b, 187); CHECK_STR(b, "3:07");
    fmt_elapsed(b, 3599); CHECK_STR(b, "59:59");
    fmt_elapsed(b, 3900); CHECK_STR(b, "1:05h");
    fmt_elapsed(b, 9u * 3600 + 59 * 60 + 59); CHECK_STR(b, "9:59h");
    fmt_elapsed(b, 10u * 3600 + 5 * 60); CHECK_STR(b, "10h05");        // the info row's field is five characters wide
    fmt_elapsed(b, 99u * 3600 + 5000); CHECK_STR(b, "99h59");
    for (uint32_t s : {0u, 59u, 3599u, 3600u, 36000u, 99u * 3600u, 1000u * 3600u}) {
        const int n = fmt_elapsed(b, s);
        CHECK_EQ(n <= 5, true);
    }
}

static void test_level() {
    char b[16];
    fmt_level(b, 20000); CHECK_STR(b, "20");
    fmt_level(b, 3300); CHECK_STR(b, "3.3");
    fmt_level(b, 9250); CHECK_STR(b, "9.25");
    fmt_level(b, 6500); CHECK_STR(b, "6.5");
    fmt_level(b, 2250); CHECK_STR(b, "2.25");
    fmt_level(b, 21000); CHECK_STR(b, "21");
}

static ProtoText fmt(ProtoInfo p) {
    ProtoText t;
    format_proto(p, t);
    return t;
}

static void test_protocol_row() {
    ProtoText t = fmt({.kind = ProtoKind::PdFixed, .mv = 20000, .ma = 5000});
    CHECK_STR(t.tag.c_str(), "PD");
    CHECK_STR(t.value.c_str(), "20V5A");
    t = fmt({.kind = ProtoKind::PdEpr, .mv = 28000, .ma = 5000});
    CHECK_STR(t.tag.c_str(), "EPR");
    CHECK_STR(t.value.c_str(), "28V5A");
    t = fmt({.kind = ProtoKind::Pps, .lo_mv = 3300, .hi_mv = 21000, .ma = 5000});
    CHECK_STR(t.tag.c_str(), "PPS");
    CHECK_STR(t.value.c_str(), "3.3-21V 5A");
    t = fmt({.kind = ProtoKind::Pps, .lo_mv = 9000, .hi_mv = 9000});   // a keep-alive with no caps: one level, "PPS 9V"
    CHECK_STR(t.tag.c_str(), "PPS");
    CHECK_STR(t.value.c_str(), "9V");
    t = fmt({.kind = ProtoKind::Avs, .lo_mv = 15000, .hi_mv = 28000, .mw = 140000});
    CHECK_STR(t.tag.c_str(), "AVS");
    CHECK_STR(t.value.c_str(), "15-28V 140W");
    t = fmt({.kind = ProtoKind::Avs, .lo_mv = 12600, .hi_mv = 12600});   // an own contract's level: "AVS 12.6V"
    CHECK_STR(t.tag.c_str(), "AVS");
    CHECK_STR(t.value.c_str(), "12.6V");
    t = fmt({.kind = ProtoKind::Qc2, .mv = 9000});
    CHECK_STR(t.tag.c_str(), "QC2");
    CHECK_STR(t.value.c_str(), "9V");
    t = fmt({.kind = ProtoKind::Qc3, .lo_mv = 3600, .hi_mv = 12000});
    CHECK_STR(t.tag.c_str(), "QC3");
    CHECK_STR(t.value.c_str(), "3.6-12V");
    t = fmt({.kind = ProtoKind::Fcp, .mv = 9000, .ma = 2000});
    CHECK_STR(t.value.c_str(), "9V2A");
    t = fmt({.kind = ProtoKind::Scp, .mv = 10000, .ma = 2250});
    CHECK_STR(t.tag.c_str(), "SCP");
    CHECK_STR(t.value.c_str(), "10V2.25A");
    t = fmt({.kind = ProtoKind::Ufcs, .lo_mv = 5000, .hi_mv = 20000, .ma = 5000});
    CHECK_STR(t.tag.c_str(), "UFCS");
    CHECK_STR(t.value.c_str(), "5-20V 5A");
    t = fmt({.kind = ProtoKind::Svooc, .mv = 10000, .ma = 6500});
    CHECK_STR(t.tag.c_str(), "SVOOC");
    CHECK_STR(t.value.c_str(), "10V6.5A");
    t = fmt({.kind = ProtoKind::TypeC, .mv = 5000, .ma = 3000});
    CHECK_STR(t.tag.c_str(), "CC");
    CHECK_STR(t.value.c_str(), "5V3A");
    t = fmt({.kind = ProtoKind::Dcp, .mv = 5000, .ma = 1500});
    CHECK_STR(t.tag.c_str(), "DCP");
    CHECK_STR(t.value.c_str(), "5V1.5A");
    t = fmt({.kind = ProtoKind::None});
    CHECK_STR(t.tag.c_str(), "--");
    CHECK_STR(t.value.c_str(), "No source");
    t = fmt({.kind = ProtoKind::Detecting});
    CHECK_STR(t.tag.c_str(), "..");
    CHECK_STR(t.value.c_str(), "Detecting");
}

static void test_exception_badge() {
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::None)), static_cast<int>(Pill::None));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::Near)), static_cast<int>(Pill::None));   // near: the cell speaks, no pill
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::Over)), static_cast<int>(Pill::Over));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Stale, Alarm::Over)), static_cast<int>(Pill::Last));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Idle, Alarm::None)), static_cast<int>(Pill::NoVbus));
    CHECK_EQ(static_cast<int>(pill_of(Mode::NoMeter, Alarm::None)), static_cast<int>(Pill::None));
    // a held Rd pin gets the badge ("Rd pin CCx") on every mode except a live OVER alarm
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::None, 2)), static_cast<int>(Pill::Pin));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Idle, Alarm::None, 1)), static_cast<int>(Pill::Pin));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Stale, Alarm::None, 2)), static_cast<int>(Pill::Pin));
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::Over, 2)), static_cast<int>(Pill::Over));   // the alarm keeps precedence
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::None, 3)), static_cast<int>(Pill::None));    // never both lines
    CHECK_EQ(static_cast<int>(pill_of(Mode::Live, Alarm::None, 0)), static_cast<int>(Pill::None));
}

// Regression: unplugged, the old info row printed "max 1.0V 3A" from the live VBUS reading.
static void test_max_text_never_comes_from_live_vbus() {
    StrBuf<12> s;
    ProtoInfo pd{.kind = ProtoKind::PdFixed, .mv = 20000, .ma = 5000, .max_mv = 20000};
    CHECK_EQ(max_volts_text(Mode::Live, pd, s), 1);
    CHECK_STR(s.c_str(), "max 20V");
    StrBuf<12> a;
    CHECK_EQ(max_amps_text(Mode::Live, pd, a), 1);
    CHECK_STR(a.c_str(), "max 5A");
    StrBuf<12> idle;
    CHECK_EQ(max_volts_text(Mode::Idle, ProtoInfo{}, idle), 0);        // idle: no contract, nothing to print
    CHECK_STR(idle.c_str(), "");
    StrBuf<12> stale;
    CHECK_EQ(max_volts_text(Mode::Stale, pd, stale), 0);               // unplugged: the LAST pill owns the corner
    CHECK_STR(stale.c_str(), "");
    StrBuf<12> unk;
    ProtoInfo qc{.kind = ProtoKind::Qc2, .mv = 9000, .max_mv = 0};
    CHECK_EQ(max_volts_text(Mode::Live, qc, unk), 0);                  // unknown maximum: print nothing, never a guess
    StrBuf<12> amps_unk;
    CHECK_EQ(max_amps_text(Mode::Live, qc, amps_unk), 0);
    StrBuf<12> frac;
    ProtoInfo svooc{.kind = ProtoKind::Svooc, .mv = 10000, .ma = 6500, .max_mv = 10000};
    max_amps_text(Mode::Live, svooc, frac);
    CHECK_STR(frac.c_str(), "max 6.5A");
}

static void test_alarm_text() {
    StrBuf<16> s;
    alarm_text(Alarm::Near, 4620, 5000, s);
    CHECK_STR(s.c_str(), "92% OF 5A");
    StrBuf<16> o;
    alarm_text(Alarm::Over, 5340, 5000, o);
    CHECK_STR(o.c_str(), "OVER 5A LIMIT");
    StrBuf<16> n;
    alarm_text(Alarm::None, 100, 5000, n);
    CHECK_STR(n.c_str(), "");
}

// Row 1 of the info block: the Black Box diagnosis word under "Diag" while a PD session is live or stale, toned by the
// worst finding (red Fail, amber Warn, dim otherwise, grey when stale; never green); the lines otherwise.
static void test_diag_row() {
    CHECK_STR(std::string(kDiagTag).c_str(), "Diag");
    const pd::diag::Report case1 = diag_fixture::case1();
    CHECK_EQ(case1.empty, false);
    DiagText d;
    CHECK_EQ(format_diag(Mode::Live, case1, d), true);
    CHECK_STR(d.value.c_str(), "Power short");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Error));
    CHECK_EQ(format_diag(Mode::Stale, case1, d), true);
    CHECK_STR(d.value.c_str(), "Power short");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Stale));
    // Outside a session the row keeps the lines (CC1/CC2 at rest, no meter).
    CHECK_EQ(format_diag(Mode::Idle, case1, d), false);
    CHECK_EQ(format_diag(Mode::NoMeter, case1, d), false);
    // The same findings as Warn: amber.
    pd::diag::Report warn = case1;
    for (unsigned i = 0; i < warn.n; i++)
        if (warn.f[i].sev == pd::diag::Sev::Fail) warn.f[i].sev = pd::diag::Sev::Warn;
    CHECK_EQ(format_diag(Mode::Live, warn, d), true);
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Caution));
    // A healthy inline session: dim, never green.
    const pd::diag::Report inl = diag_fixture::report("tests/fixtures/bbdiag/legion_inline.raw");
    CHECK_EQ(format_diag(Mode::Live, inl, d), true);
    CHECK_STR(d.value.c_str(), "Link healthy");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Dim));
    // No session yet: the state in words.
    CHECK_EQ(format_diag(Mode::Live, pd::diag::Report{}, d), true);
    CHECK_STR(d.value.c_str(), "No PD session");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Dim));
    // A session without PD (a legacy charger): the D+/D- lines tell more.
    pd::diag::Report legacy;
    legacy.empty = false;
    legacy.no_pd = true;
    CHECK_EQ(format_diag(Mode::Live, legacy, d), false);
}

int main() {
    test_alarm_thresholds();
    test_limit_bar();
    test_sweep_maths();
    test_sweep_columns();
    test_hero_numbers();
    test_watts_and_energy();
    test_elapsed();
    test_level();
    test_protocol_row();
    test_exception_badge();
    test_max_text_never_comes_from_live_vbus();
    test_alarm_text();
    test_diag_row();
    return test_report("test_home_model");
}
