// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 pure model: alarms, limit bar, sweep maths, number formats, protocol row, exception badge.
#include <cstdio>
#include <initializer_list>
#include <string>
#include "test_util.h"
#include "k2/pd/diag_screen.hpp"
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
    CHECK_EQ(bar_fill_w(0, 5000, kHeroBar), 0);
    CHECK_EQ(bar_fill_w(10, 0, kHeroBar), 0);
    CHECK_EQ(bar_track(kHeroBar), 205);
    CHECK_EQ(bar_fill_w(5000, 5000, kHeroBar), 205);
    CHECK_EQ(bar_fill_w(9000, 5000, kHeroBar), 205);   // over the limit: full track, never past it
    CHECK_EQ(bar_fill_w(2500, 5000, kHeroBar), 103);   // round(204 / 2) + 1
    CHECK_EQ(bar_tick_x(kHeroBar), 194);
    CHECK_EQ(kHeroBar.y, 68);                          // 3 rows inside the hero cell, clear of the 64 px digits (baseline 63)
    const BarGeom l = ledger_bar(0);
    CHECK_EQ(bar_track(l), 128);
    CHECK_EQ(bar_fill_w(2500, 5000, l), 65);           // round(127 / 2) + 1
    CHECK_EQ(bar_tick_x(l), 120);
    CHECK_EQ(l.y, 132);
    CHECK_EQ(ledger_bar(1).y, 190);
}

// 2c geometry: hero y0..73, 2 px rule, ledger rows y76..135 / y137..193 with a 1 px rule between, the vertical rule
// at x140, the info block under the rule at y194. Every row of the content belongs to exactly one band.
static void test_layout() {
    CHECK_EQ(kRule1Y, kHeroY1 + 1);
    CHECK_EQ(kRowY0[0], kRule1Y + 2);
    CHECK_EQ(kRowRuleY, kRowY1[0] + 1);
    CHECK_EQ(kRowY0[1], kRowRuleY + 1);
    CHECK_EQ(kRule3Y, kRowY1[1] + 1);
    CHECK_EQ(kVRuleX, kLedgerX1 + 1);
    CHECK_EQ(kCardX0, kVRuleX + 1);
    CHECK_EQ(x_of_col(kLedgerCols - 1) + 1, 135);      // the ledger sweep ends inside its column
    const CellGeom h = cell_geom(kHeroY0, kHeroY1, true);
    CHECK_EQ(h.t0, 14);
    CHECK_EQ(h.t1, 66);
    CHECK_EQ(cell_geom(kHeroY0, kHeroY1, false).t1, 71);
    CHECK_EQ(cell_geom(kRowY0[1], kRowY1[1], true).t1, 186);
}

static void test_sweep_maths() {
    CHECK_EQ(scale_v10(20000), 2300);                 // contract max x 1.15, in 10 mV
    CHECK_EQ(scale_a10(5000), 575);
    CHECK_EQ(scale_v10(0), 2300);                     // unknown contract: 20 V
    CHECK_EQ(scale_w10(100000), 11500);               // 100 W x 1.15 in 10 mW
    CHECK_EQ(scale_w10(0), 6900);                     // unknown: 60 W
    const int span = cell_geom(kHeroY0, kHeroY1, false).span();
    CHECK_EQ(span, 57);
    CHECK_EQ(sweep_rows(0, 2300, span), 0);
    CHECK_EQ(sweep_rows(1, 2300, span), 0);           // 10 mV or less: no column, like the mockup
    CHECK_EQ(sweep_rows(0xFFFF, 2300, span), 0);      // kNone
    CHECK_EQ(sweep_rows(2300, 2300, span), span + 1);
    CHECK_EQ(sweep_rows(9999, 2300, span), span + 1); // clipped at full scale
    CHECK_EQ(sweep_rows(1150, 2300, span), 30);       // half scale: round(28.5) + 1
    const int ls = cell_geom(kRowY0[0], kRowY1[0], true).span();
    CHECK_EQ(ls, 38);
    CHECK_EQ(sweep_rows(575, 575, ls), 39);
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

static void test_idle_wait() {
    ProtoText t;
    format_idle_wait(raguard::WaitKind::ChargerWaiting, t);
    CHECK_STR(t.tag.c_str(), "--");
    CHECK_STR(t.value.c_str(), "Charger waiting");
    CHECK_EQ(t.value.view().size() <= 16u, true);

    format_idle_wait(raguard::WaitKind::DualRolePort, t);
    CHECK_STR(t.tag.c_str(), "--");
    CHECK_STR(t.value.c_str(), "Dual-role port");
    CHECK_EQ(t.value.view().size() <= 16u, true);

    format_idle_wait(raguard::WaitKind::NoSource, t);
    CHECK_STR(t.tag.c_str(), "--");
    CHECK_STR(t.value.c_str(), "No source");

    format_idle_wait(raguard::WaitKind::Unknown, t);
    CHECK_STR(t.tag.c_str(), "--");
    CHECK_STR(t.value.c_str(), "No source");
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
    StrBuf<20> s;
    alarm_text(Alarm::Near, 4620, 5000, s);
    CHECK_STR(s.c_str(), "92% OF 5A");
    StrBuf<20> o;
    alarm_text(Alarm::Over, 5340, 5000, o);
    CHECK_STR(o.c_str(), "OVER 5A LIMIT");
    StrBuf<20> n;
    alarm_text(Alarm::None, 100, 5000, n);
    CHECK_STR(n.c_str(), "");
    // The W hero: the same lines against the contract power.
    alarm_text(Alarm::Near, 93000, 100000, s, 'W');
    CHECK_STR(s.c_str(), "93% OF 100W");
    alarm_text(Alarm::Over, 141000, 140000, o, 'W');
    CHECK_STR(o.c_str(), "OVER 140W LIMIT");
    alarm_text(Alarm::Over, 3400, 3250, o);
    CHECK_STR(o.c_str(), "OVER 3.25A LIMIT");   // the longest line: 16 characters, never cut
    // A ledger row's short form.
    alarm_text(Alarm::Near, 93000, 100000, s, 'W', true);
    CHECK_STR(s.c_str(), "93%");
    alarm_text(Alarm::Over, 5340, 5000, o, 'A', true);
    CHECK_STR(o.c_str(), "OVER");
    StrBuf<12> m;
    CHECK_EQ(max_text(Mode::Live, 100000, 'W', m), 1);
    CHECK_STR(m.c_str(), "max 100W");
    CHECK_EQ(max_text(Mode::Stale, 100000, 'W', m), 0);
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

// The LAST snapshot keeps a reading only when it held for a window: a falling VBUS or a released contract is no snapshot.
static void test_steady_snapshot() {
    ProtoInfo pd{}; pd.kind = ProtoKind::PdFixed; pd.mv = 5000; pd.ma = 3000; pd.max_mv = 20000;
    CHECK_EQ(steady_snapshot(5104, 5098, pd, pd), 1);
    CHECK_EQ(steady_snapshot(5104, 4700, pd, pd), 1);    // within 10 %: a load's sag is still the session
    CHECK_EQ(steady_snapshot(5104, 3323, pd, pd), 0);    // VBUS falling through the display filter
    CHECK_EQ(steady_snapshot(20000, 5100, pd, pd), 0);   // a step down: the next window settles it
    ProtoInfo bare = pd; bare.ma = 0; bare.max_mv = 0;   // the contract released, caps still held: "PD 5V"
    CHECK_EQ(steady_snapshot(5104, 5100, pd, bare), 0);
    ProtoInfo v = pd; v.kind = ProtoKind::Svooc; v.measured = true; v.mv = 9000; v.ma = 0;
    ProtoInfo w = v; w.mv = 9100;
    CHECK_EQ(steady_snapshot(9000, 9100, v, w), 1);      // a measured level may move
    CHECK_EQ(steady_snapshot(-5000, -5050, pd, pd), 1);  // reverse flow reads as a magnitude
}


// ---- Home row 1: an event word falls back after one dwell, a condition word stays ----------------------------------------
namespace dd {
using pd::diag::Code;
using pd::diag::Sev;
static pd::diag::Finding fnd(Sev s, Code c, std::uint32_t t, std::int32_t a = 1) {
    pd::diag::Finding f;
    f.sev = s; f.code = c; f.t_ms = t; f.a = a;
    return f;
}
static pd::diag::Report rep(std::initializer_list<pd::diag::Finding> fs, std::uint32_t start_us = 1000) {
    pd::diag::Report r;
    r.empty = false; r.live = true; r.power = pd::diag::kPowerOk; r.start_us = start_us;
    for (const auto &f : fs) r.f[r.n++] = f;
    return r;
}
// The word the row would show without finding `i`: the invariant an expired event must meet.
static std::string word_without(const pd::diag::Report &r, unsigned i) {
    pd::diag::Report c = r;
    for (unsigned k = i; k + 1 < c.n; k++) c.f[k] = c.f[k + 1];
    c.n--;
    DiagText d;
    format_diag(Mode::Live, c, d);
    return d.value.c_str();
}
}  // namespace dd

static void test_diag_event_classes() {
    using pd::diag::Code;
    namespace sc = pd::diag::screen;
    // something that happened: falls back after a dwell
    for (Code c : {Code::OverDraw, Code::OverSustained, Code::InputCuts, Code::HardReset, Code::Reattach, Code::SoftReset,
                   Code::Rejected, Code::WaitLoop, Code::NoAccept, Code::NoPsRdy, Code::LateAccept, Code::LatePsRdy,
                   Code::ManyRequests, Code::VbusLost, Code::VbusLow, Code::VbusHigh, Code::NoGoodCrc, Code::Retries,
                   Code::TxFail, Code::RxBad, Code::ModeNak, Code::EnterUsbRefused, Code::EprFailed})
        CHECK_EQ(sc::is_event(c), true);
    // the session as it stands: stays while it holds
    for (Code c : {Code::PowerShort, Code::MismatchFlag, Code::Pinned, Code::FiveVoltOnly, Code::CapsUnanswered, Code::CableCap,
                   Code::NoMismatchFlag, Code::Draw, Code::DrawReduced, Code::ContractUnseen, Code::NoContract,
                   Code::ActiveSink, Code::SwapRefused, Code::VdmLoop, Code::CableSilent})
        CHECK_EQ(sc::is_event(c), false);
    int events = 0;
    for (int c = 0; c <= static_cast<int>(Code::CableSilent); c++) events += sc::is_event(static_cast<Code>(c));
    CHECK_EQ(events, 23);
}

static void test_diag_word_hide() {
    using namespace dd;
    namespace sc = pd::diag::screen;
    const pd::diag::Report r = rep({fnd(Sev::Fail, Code::HardReset, 500), fnd(Sev::Warn, Code::PowerShort, 0),
                                    fnd(Sev::Info, Code::Draw, 0)});
    char w[24];
    Sev sev;
    sc::word(r, w, sev);
    CHECK_STR(w, "Hard reset");
    CHECK_EQ(static_cast<int>(sev), static_cast<int>(Sev::Fail));
    sc::word(r, w, sev, 1u);          // the hard reset hidden: the next Fail/Warn finding
    CHECK_STR(w, "Power short");
    CHECK_EQ(static_cast<int>(sev), static_cast<int>(Sev::Warn));
    sc::word(r, w, sev, 3u);          // both hidden: an Info finding never takes the word, the state does
    CHECK_STR(w, "Healthy");
    CHECK_EQ(static_cast<int>(sev), static_cast<int>(Sev::Ok));
}

static void test_diag_dwell() {
    using namespace dd;
    DiagText d;
    DiagDwell dw;
    const pd::diag::Report r = rep({fnd(Sev::Fail, Code::HardReset, 500), fnd(Sev::Warn, Code::PowerShort, 0)});
    const std::uint32_t t0 = 5000;
    CHECK_EQ(dw.update(r, t0), 0u);
    format_diag(Mode::Live, r, d, dw.update(r, t0 + 100));
    CHECK_STR(d.value.c_str(), "Hard reset");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Error));
    CHECK_EQ(dw.update(r, t0 + kDiagEventDwellMs - 1), 0u);       // one dwell: still the event
    const std::uint16_t gone = dw.update(r, t0 + kDiagEventDwellMs);
    CHECK_EQ(gone, 1u);
    format_diag(Mode::Live, r, d, gone);
    CHECK_STR(d.value.c_str(), word_without(r, 0).c_str());          // exactly the row without the event
    CHECK_STR(d.value.c_str(), "Power short");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Caution));
    CHECK_EQ(dw.update(r, t0 + 600000), 1u);                         // a condition never times out
    // A second hard reset (the count moves): the word comes back for one more dwell.
    pd::diag::Report r2 = r;
    r2.f[0].a = 2;
    const std::uint32_t t1 = t0 + 700000;
    CHECK_EQ(dw.update(r2, t1), 0u);
    CHECK_EQ(dw.update(r2, t1 + kDiagEventDwellMs - 1), 0u);
    CHECK_EQ(dw.update(r2, t1 + kDiagEventDwellMs), 1u);
    // An event alone falls back to the session's state word, dim.
    DiagDwell solo;
    const pd::diag::Report s = rep({fnd(Sev::Warn, Code::SoftReset, 200)}, 2000);
    solo.update(s, 100);
    format_diag(Mode::Live, s, d, solo.update(s, 100 + kDiagEventDwellMs));
    CHECK_STR(d.value.c_str(), "Healthy");
    CHECK_EQ(static_cast<int>(d.tone), static_cast<int>(DiagTone::Dim));
    // The same finding in a new session (another start) is a new event.
    const pd::diag::Report s2 = rep({fnd(Sev::Warn, Code::SoftReset, 200)}, 3000);
    CHECK_EQ(solo.update(s2, 100 + kDiagEventDwellMs + 1), 0u);
    // Two events: each keeps its own clock; the second shows once the first has gone.
    DiagDwell two;
    const pd::diag::Report e2 = rep({fnd(Sev::Fail, Code::HardReset, 500), fnd(Sev::Warn, Code::Rejected, 900)});
    two.update(e2, 0);
    format_diag(Mode::Live, e2, d, two.update(e2, kDiagEventDwellMs));
    CHECK_STR(d.value.c_str(), "Healthy");   // both were first seen together, so both have had their dwell
    DiagDwell late;
    late.update(rep({fnd(Sev::Fail, Code::HardReset, 500)}), 0);
    const std::uint16_t m = late.update(e2, kDiagEventDwellMs);     // the reject appeared later: it still shows
    CHECK_EQ(m, 1u);
    format_diag(Mode::Live, e2, d, m);
    CHECK_STR(d.value.c_str(), "Request rejected");
    // A recorded session (the stress fixture: soft resets) settles on a word that names no event once the dwell has passed.
    const pd::diag::Report st = diag_fixture::report("tests/fixtures/bbdiag/stress_soft_resets.raw");
    DiagDwell sd;
    sd.update(st, 0);
    const std::uint16_t sm = sd.update(st, kDiagEventDwellMs);
    format_diag(Mode::Live, st, d, sm);
    for (unsigned i = 0; i < st.n; i++) {
        if (st.f[i].sev > Sev::Warn) break;
        CHECK_EQ(((sm >> i) & 1u) != 0, pd::diag::screen::is_event(st.f[i].code));
    }
    std::printf("stress fixture: %s after the dwell (mask 0x%x)\n", d.value.c_str(), unsigned(sm));
}

// The armed PD limit follows the contract on the protocol row ("PD 9V3A lim27W"); the value never passes kProtoChars,
// the trailing current or power part goes first.
static void test_protocol_limit_suffix() {
    ProtoText t = fmt({.kind = ProtoKind::PdFixed, .mv = 9000, .ma = 3000});
    format_limit(t, ProtoKind::PdFixed, 27);
    CHECK_STR(t.tag.c_str(), "PD");
    CHECK_STR(t.value.c_str(), "9V3A lim27W");
    t = fmt({.kind = ProtoKind::PdFixed, .mv = 20000, .ma = 3250});
    format_limit(t, ProtoKind::PdFixed, 100);
    CHECK_STR(t.value.c_str(), "20V lim100W");        // the current is dropped to make room
    t = fmt({.kind = ProtoKind::Pps, .lo_mv = 3300, .hi_mv = 21000, .ma = 5000});
    format_limit(t, ProtoKind::Pps, 100);
    CHECK_STR(t.value.c_str(), "3.3-21 lim100W");      // the current and then the unit go, never the limit
    t = fmt({.kind = ProtoKind::Pps, .lo_mv = 5000, .hi_mv = 11000, .ma = 3000});
    format_limit(t, ProtoKind::Pps, 27);
    CHECK_STR(t.value.c_str(), "5-11V lim27W");
    t = fmt({.kind = ProtoKind::Avs, .lo_mv = 15000, .hi_mv = 28000, .mw = 140000});
    format_limit(t, ProtoKind::Avs, 100);
    CHECK_STR(t.value.c_str(), "15-28V lim100W");
    t = fmt({.kind = ProtoKind::PdFixed, .mv = 9250, .ma = 3250});
    format_limit(t, ProtoKind::PdFixed, 100);
    CHECK_STR(t.value.c_str(), "9.25V lim100W");       // a fixed level with odd numbers loses its current too
    // Other protocols and no limit: untouched.
    t = fmt({.kind = ProtoKind::Qc3, .lo_mv = 3600, .hi_mv = 12000});
    format_limit(t, ProtoKind::Qc3, 27);
    CHECK_STR(t.value.c_str(), "3.6-12V");
    t = fmt({.kind = ProtoKind::PdFixed, .mv = 9000, .ma = 3000});
    format_limit(t, ProtoKind::PdFixed, std::string_view{"9V2.5"});
    CHECK_STR(t.value.c_str(), "9V3A lim9V2.5");
    t = fmt({.kind = ProtoKind::Pps, .lo_mv = 3300, .hi_mv = 21000, .ma = 5000});
    format_limit(t, ProtoKind::Pps, std::string_view{"PPS11"});
    CHECK_EQ(t.value.view().size() <= static_cast<size_t>(kProtoChars), true);
    CHECK_EQ(t.value.view().find("PPS11") != std::string_view::npos, true);
    t = fmt({.kind = ProtoKind::PdFixed, .mv = 9000, .ma = 3000});
    format_limit(t, ProtoKind::PdFixed, 0);
    CHECK_STR(t.value.c_str(), "9V3A");
    // Every limit-capable kind, widest contracts and limits: never over the row.
    const ProtoInfo wide[] = {
        {.kind = ProtoKind::PdFixed, .mv = 20500, .ma = 3250},
        {.kind = ProtoKind::PdEpr, .mv = 48000, .ma = 5000},
        {.kind = ProtoKind::Pps, .lo_mv = 3300, .hi_mv = 21000, .ma = 5000},
        {.kind = ProtoKind::Pps, .lo_mv = 5000, .hi_mv = 21000, .ma = 3250},
        {.kind = ProtoKind::Avs, .lo_mv = 15000, .hi_mv = 48000, .mw = 240000},
        {.kind = ProtoKind::Avs, .lo_mv = 12600, .hi_mv = 12600, .mw = 140000},
    };
    for (const ProtoInfo &p : wide) {
        for (uint32_t w : {5u, 27u, 100u}) {
            t = fmt(p);
            format_limit(t, p.kind, w);
            CHECK_EQ(t.value.view().size() <= static_cast<size_t>(kProtoChars), true);
            CHECK_EQ(t.value.view().find("lim") != std::string_view::npos, true);
        }
    }
}

int main() {
    test_protocol_limit_suffix();
    test_alarm_thresholds();
    test_limit_bar();
    test_layout();
    test_sweep_maths();
    test_sweep_columns();
    test_hero_numbers();
    test_watts_and_energy();
    test_elapsed();
    test_level();
    test_protocol_row();
    test_idle_wait();
    test_steady_snapshot();
    test_exception_badge();
    test_max_text_never_comes_from_live_vbus();
    test_alarm_text();
    test_diag_row();
    test_diag_event_classes();
    test_diag_word_hide();
    test_diag_dwell();
    return test_report("test_home_model");
}
