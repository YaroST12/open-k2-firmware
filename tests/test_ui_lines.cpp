// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lines page v2 pure logic: bar widths against the 0.4 V / 2.0 V ticks, the Rp advertisement read off
// CC, and the verdict pills for each D+/D- classifier result.
#include <string>
#include "test_util.h"
#include "k2/ui/ui_lines.hpp"

using namespace k2;
using fastcharge::Protocol;

static void test_bars() {
    CHECK_EQ(kLinesBarW, 112);
    CHECK_EQ(lines_bar_px(0), 0);
    CHECK_EQ(lines_bar_px(-50), 0);
    CHECK_EQ(lines_bar_px(600), 20);        // mockup: D+ 0.60 V fills 20 px
    CHECK_EQ(lines_bar_px(1680), 57);       // CC1 1.68 V
    CHECK_EQ(lines_bar_px(3300), 112);
    CHECK_EQ(lines_bar_px(5000), 112);      // clamps
    CHECK_EQ(lines_bar_px(10) >= 2, 1);     // a visible stub for any positive reading
    CHECK_EQ(lines_tick_px(400), 13);       // x 117 in the mockup = 104 + 13
    CHECK_EQ(lines_tick_px(2000), 67);      // x 171
    // The bars are monotonic.
    int prev = 0;
    for (int mv = 0; mv <= 3400; mv += 10) { CHECK_EQ(lines_bar_px(mv) >= prev, 1); prev = lines_bar_px(mv); }
}

static std::string rp(int cc1, int cc2, int active = 0, bool have = true) {
    StrBuf<24> s;
    rp_text(s, have, static_cast<std::uint8_t>(active), cc1, cc2);
    return std::string(s.view());
}

static void test_rp() {
    CHECK_STR(rp(0, 1680).c_str(), "3.0A on CC2");
    CHECK_STR(rp(1680, 0).c_str(), "3.0A on CC1");
    CHECK_STR(rp(900, 0).c_str(), "1.5A on CC1");
    CHECK_STR(rp(400, 0).c_str(), "USB on CC1");
    CHECK_STR(rp(0, 0).c_str(), "none on CC");
    CHECK_STR(rp(150, 120).c_str(), "none on CC");
    CHECK_STR(rp(2300, 0).c_str(), "? on CC1");   // above the 3 A window: not a sane Rp
    CHECK_STR(rp(0, 0, 0, false).c_str(), "--");
    // Both pins live (a debug-accessory or a 5.1k pair): the higher one is reported.
    CHECK_STR(rp(400, 900).c_str(), "1.5A on CC2");
    // The named wire is the active CC line (the one the PD engine listens on), even when it is the
    // lower-voltage one; with no active line the higher-voltage wire is reported.
    CHECK_STR(rp(408, 520, 1).c_str(), "USB on CC1");
    CHECK_STR(rp(408, 520, 2).c_str(), "USB on CC2");
    CHECK_STR(rp(408, 520, 0).c_str(), "USB on CC2");
    // The band follows the active wire's own level, not the other wire's.
    CHECK_STR(rp(408, 1680, 1).c_str(), "USB on CC1");
    CHECK_STR(rp(408, 1680, 2).c_str(), "3.0A on CC2");
}

static void test_mcu() {
    StrBuf<24> s;
    mcu_text(s, true, 324);  CHECK_STR(s.c_str(), "32\xC2\xB0" "C");
    StrBuf<24> t;
    mcu_text(t, true, 325);  CHECK_STR(t.c_str(), "33\xC2\xB0" "C");
    StrBuf<24> u;
    mcu_text(u, true, -35);  CHECK_STR(u.c_str(), "-4\xC2\xB0" "C");
    StrBuf<24> w;
    mcu_text(w, false, 300); CHECK_STR(w.c_str(), "--");
}

static void test_verdicts() {
    LinesVerdict v = lines_verdict(Protocol::Dcp5V, true);
    CHECK_STR(v.main.c_str(), "DCP short");
    CHECK_STR(v.extra.c_str(), "");
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Ok));
    CHECK_STR(v.footer.c_str(), "D+ = D-: dedicated charger");

    v = lines_verdict(Protocol::Apple24, true);
    CHECK_STR(v.main.c_str(), "Divider");
    CHECK_STR(v.extra.c_str(), "Apple 2.4A");
    v = lines_verdict(Protocol::Qc2_9V, true);
    CHECK_STR(v.main.c_str(), "QC2.0");
    CHECK_STR(v.extra.c_str(), "9V");
    v = lines_verdict(Protocol::Qc2_20V, true);
    CHECK_STR(v.extra.c_str(), "20V");
    v = lines_verdict(Protocol::Qc3, true);
    CHECK_STR(v.main.c_str(), "QC3.0");
    v = lines_verdict(Protocol::Pending, true);
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Dim));
    v = lines_verdict(Protocol::NoInput, true);
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Caution));
    v = lines_verdict(Protocol::PdLikely, false);
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Caution));

    // Every verdict fits its slots: pill 8 chars of room in the card, footer 26 chars at 7 px.
    for (int decoded = 0; decoded <= 1; decoded++)
        for (int p = 0; p <= static_cast<int>(Protocol::PdLikely); p++) {
            v = lines_verdict(static_cast<Protocol>(p), decoded != 0);
            CHECK_EQ(v.main.view().size() > 0 && v.main.view().size() <= 10, 1);
            CHECK_EQ(v.extra.view().size() <= 11, 1);
            CHECK_EQ(v.footer.view().size() > 0 && v.footer.view().size() <= 26, 1);
            // main + extra pills side by side fit the 224 px card (x 10 + pills + gap).
            CHECK_EQ(10 + pill_width(static_cast<int>(v.main.view().size())) + 6 +
                     pill_width(static_cast<int>(v.extra.view().size())) <= 220, 1);
        }
}

static void test_verdict_pd_decoded() {
    // The green PD chip follows a decoded PD message, not just the line levels: without a decoded
    // contract or source caps, a PD-family classifier result stays a neutral "PD?" caution.
    LinesVerdict v = lines_verdict(Protocol::Pd, false);
    CHECK_STR(v.main.c_str(), "PD?");
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Caution));
    CHECK_STR(v.footer.c_str(), "No caps seen");
    v = lines_verdict(Protocol::Pd, true);
    CHECK_STR(v.main.c_str(), "PD");
    CHECK_EQ(static_cast<int>(v.tone), static_cast<int>(Tone::Ok));
    CHECK_STR(v.footer.c_str(), "PD: see Who's attached");

    // The other PD-family verdicts follow the same rule.
    for (Protocol p : {Protocol::PdPe, Protocol::PdMtk}) {
        const LinesVerdict no = lines_verdict(p, false);
        CHECK_STR(no.main.c_str(), "PD?");
        CHECK_EQ(static_cast<int>(no.tone), static_cast<int>(Tone::Caution));
        const LinesVerdict yes = lines_verdict(p, true);
        CHECK_EQ(static_cast<int>(yes.tone), static_cast<int>(Tone::Ok));
        CHECK_EQ(yes.main.view().size() > 2, 1);   // "PD PE" / "PD MTK", not the plain PD chip
    }

    // Non-PD protocols are unchanged by the flag.
    for (int p = 0; p <= static_cast<int>(Protocol::PdLikely); p++) {
        const Protocol pr = static_cast<Protocol>(p);
        if (pr == Protocol::Pd || pr == Protocol::PdPe || pr == Protocol::PdMtk) continue;
        const LinesVerdict a = lines_verdict(pr, false), b = lines_verdict(pr, true);
        CHECK_STR(a.main.c_str(), b.main.c_str());
        CHECK_STR(a.extra.c_str(), b.extra.c_str());
        CHECK_STR(a.footer.c_str(), b.footer.c_str());
        CHECK_EQ(static_cast<int>(a.tone), static_cast<int>(b.tone));
    }
}

int main() {
    test_bars();
    test_rp();
    test_mcu();
    test_verdicts();
    test_verdict_pd_decoded();
    return test_report("test_ui_lines");
}
