// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The tones of a saved report card (k2/chargertest/card_tone.hpp): the Results grid's table, applied to the FH22 card #18 as the
// hardware run left it. tests/test_cardpage.mjs pins the web page to the same card.
#include "card_fixtures.h"
#include "k2/chargertest/card_tone.hpp"
#include "test_util.h"

using namespace k2;
namespace rc = report_card;
namespace tn = report_card::tone;
using result_layout::Tone;

static void test_fh22() {
    const rc::Card c = cardfx::fh22_full();
    CHECK_EQ(int(c.n_ranges), 2);
    // Fixed levels pass; the group failed only through the PPS range.
    CHECK_EQ(int(tn::group(c, 0)), int(Tone::Pass));
    CHECK_EQ(int(tn::capped(c)), 1);
    // AVS 9-20 V passed at 19.406 V, inside the tolerance: green, not "stopped short".
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Pass));
    // PPS 5-21 V plateaued at 19.4 V: amber (capped), never red.
    CHECK_EQ(int(tn::range(c, c.range[1])), int(Tone::Partial));
    // Never offered: grey for all seven.
    for (unsigned g = 1; g < rc::kGroups; ++g) {
        if (c.group[g].verdict == rc::GV::None) continue;
        CHECK_EQ(int(tn::group(c, g)), int(Tone::Skip));
    }
    const tn::Counts n = tn::counts(c);
    CHECK_EQ(int(n.pass), 2);
    CHECK_EQ(int(n.partial), 1);
    CHECK_EQ(int(n.fail), 0);
    CHECK_EQ(int(n.grey), 8);          // QC2 not advertised + AFC FCP SCP VOOC SVOOC SVOOC65 VIVO
    CHECK_EQ(int(tn::card(c)), int(Tone::Partial));
}

static void test_rules() {
    rc::Card c = cardfx::fh22_full();
    // A protocol that answered and then failed a higher level is amber.
    c.group[2] = {rc::GV::Fail, 9000, 0, uint8_t(charger_test::Reason::Timeout), 12000};
    CHECK_EQ(int(tn::group(c, 2)), int(Tone::Partial));
    // An AVS range whose top was never exercised (36 of 48 V in spirit) is amber; nothing measured is grey.
    rc::Range r = c.range[0];
    r.max_dv = 140;
    CHECK_EQ(int(tn::range(c, r)), int(Tone::Partial));
    r.max_dv = 0;
    CHECK_EQ(int(tn::range(c, r)), int(Tone::Skip));
    // A range that failed with nothing reached is red.
    r.fail_dv = 100;
    CHECK_EQ(int(tn::range(c, r)), int(Tone::Fail));
    // The same failure under a source that plateaued is "capped" (amber), under a plain failure pass + fail (amber): both amber.
    rc::Range p = c.range[1];
    CHECK_EQ(int(tn::range(c, p)), int(Tone::Partial));
    c.group[0].reason = uint8_t(charger_test::Reason::Timeout);
    CHECK_EQ(int(tn::range(c, p)), int(Tone::Partial));
    // A card of one failing fixed level and nothing passing is red; one with a level passing is amber.
    rc::Card f;
    f.group[0] = {rc::GV::Fail, 0, 0, uint8_t(charger_test::Reason::Timeout), 5000};
    CHECK_EQ(int(tn::card(f)), int(Tone::Fail));
    f.group[0].max_mv = 9000;
    CHECK_EQ(int(tn::card(f)), int(Tone::Partial));
    // An all-pass card is green; a card of only unanswered protocols is grey, not red.
    rc::Card ok;
    ok.group[0] = {rc::GV::Pass, 20000, 0, 0};
    CHECK_EQ(int(tn::card(ok)), int(Tone::Pass));
    rc::Card grey;
    grey.group[3] = {rc::GV::Fail, 0, 0, uint8_t(charger_test::Reason::Timeout), 5000};
    CHECK_EQ(int(tn::card(grey)), int(Tone::Skip));
}

int main() {
    test_fh22();
    test_rules();
    return test_report("card_tone");
}
