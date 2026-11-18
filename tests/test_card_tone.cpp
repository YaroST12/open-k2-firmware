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
    CHECK_EQ(int(n.grey), 6);          // QC2 not advertised + AFC FCP SCP VOOC (one family) VIVO
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

// A Light run measured only 5 V: its card lists what the charger offers and what answered. What the PD family advertises (the PD
// group and the listed ranges) reads pass (green: "we have it", nothing was requested); a legacy protocol that answered at 5 V stays
// Noted ("detected"); a failure is still a failure.
static void test_light_card_pd_is_green_legacy_is_noted() {
    rc::Card c = cardfx::fh22_full();
    c.level = charger_test::Level::Light;
    for (unsigned i = 0; i < c.n_ranges; ++i) { c.range[i].max_dv = 0; c.range[i].fail_dv = 0; }   // ranges only listed
    c.group[0] = {rc::GV::Pass, 5000, 0, 0};
    c.group[1] = {rc::GV::Pass, 5000, 0, 0};   // QC2 answered and held 5 V
    CHECK_EQ(int(tn::group(c, 0)), int(Tone::Pass));
    CHECK_EQ(int(tn::group(c, 1)), int(Tone::Noted));
    for (unsigned i = 0; i < c.n_ranges; ++i) CHECK_EQ(int(tn::range(c, c.range[i])), int(Tone::Pass));
    CHECK_EQ(int(tn::card(c)), int(Tone::Pass));
    const tn::Counts n = tn::counts(c);
    CHECK_EQ(int(n.pass), 1 + int(c.n_ranges));
    CHECK_EQ(int(n.noted), 1);
    // a card with no PD at all keeps the Noted head: only legacy protocols answered
    rc::Card legacy = c;
    legacy.group[0] = {rc::GV::None, 0, 0, 0};
    legacy.n_ranges = 0;
    CHECK_EQ(int(tn::card(legacy)), int(Tone::Noted));
    c.group[2] = {rc::GV::Fail, 0, 0, uint8_t(charger_test::Reason::Timeout), 5000};   // answered, then lost 5 V
    c.group[2].max_mv = 0;
    CHECK_EQ(int(tn::group(c, 0)), int(Tone::Pass));
    // the same card at Medium reads as before
    c.level = charger_test::Level::Medium;
    CHECK_EQ(int(tn::group(c, 0)), int(Tone::Pass));
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Skip));
}

// A voltage fold the sweep found is a caution: amber, counted with the capped rows, never green and never red. A range that also
// failed keeps its own verdict (the fold does not turn a failure into a caution).
static void test_a_fold_is_a_caution() {
    rc::Card c = cardfx::fh22_full();
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Pass));
    c.sweep[0].fold_dv = 194;
    c.sweep[0].fold_from_dv = 200;
    CHECK_EQ(int(tn::folded(c, c.range[0])), 1);
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Partial));
    const tn::Counts n = tn::counts(c);
    CHECK_EQ(int(n.pass), 1);          // the fixed levels; the AVS range is now amber, the PPS range was already
    CHECK_EQ(int(n.partial), 2);
    CHECK_EQ(int(n.fail), 0);
    CHECK_EQ(int(tn::card(c)), int(Tone::Partial));
    // The depth of the fold never turns the caution red: the card holds the worst error (here the clamp, 1.6 V down at 21 V) and the
    // tone still reads the fold alone, as the device's sweep chip does (Results: amber at any depth).
    c.sweep[0].min_err = -127; c.sweep[0].worst_err = -127; c.sweep[0].worst_dv = 210;
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Partial));
    CHECK_EQ(int(tn::counts(c).fail), 0);
    CHECK_EQ(int(tn::card(c)), int(Tone::Partial));
    // PPS: a plateau with a fold stays what it was (amber), the fold adds no mark of its own.
    c.sweep[1].fold_dv = 194;
    c.sweep[1].fold_from_dv = 200;
    CHECK_EQ(int(tn::folded(c, c.range[1])), 0);
    CHECK_EQ(int(tn::range(c, c.range[1])), int(Tone::Partial));
    // A range that failed with nothing reached stays red whatever the sweep saw.
    rc::Range r = c.range[0];
    r.fail_dv = 100; r.max_dv = 0;
    CHECK_EQ(int(tn::range(c, r)), int(Tone::Fail));
    // A clean sweep (no fold stored, tracked within 100 mV) stays green; the same error with no fold is the other caution (deviates).
    c.sweep[0].fold_dv = 0; c.sweep[0].fold_from_dv = 0;
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Partial));
    c.sweep[0].min_err = -40; c.sweep[0].worst_err = -40;
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Pass));
}

// A passing sweep that tracked worse than 100 mV is a caution by the same rule (the device's chip is amber, never red, on a Pass
// step): the card stores the worst error clamped to +/-127 mV, so 101..127 is "deviates". A fold dominates (already amber); a failed
// range keeps its own verdict; a card without a sweep block never deviates.
static void test_a_tracking_deviation_is_a_caution() {
    rc::Card c = cardfx::fh22_full();
    c.sweep[0] = {};
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Pass));            // no sweep stored: as before
    c.sweep[0].peak_dv = 210; c.sweep[0].peak_at_dv = 210;
    c.sweep[0].worst_err = -100; c.sweep[0].min_err = -100;
    CHECK_EQ(int(tn::deviates(c, c.range[0])), 0);                        // exactly 100 mV is inside
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Pass));
    c.sweep[0].worst_err = -101;
    CHECK_EQ(int(tn::deviates(c, c.range[0])), 1);
    CHECK_EQ(int(tn::caution(c, c.range[0])), 1);
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Partial));
    c.sweep[0].worst_err = 127;                                           // the clamp (a 450 mV excess): amber, never red
    CHECK_EQ(int(tn::range(c, c.range[0])), int(Tone::Partial));
    CHECK_EQ(int(tn::counts(c).fail), 0);
    CHECK_EQ(int(tn::card(c)), int(Tone::Partial));
    rc::Range r = c.range[0];                                             // a range that failed keeps its verdict, no caution
    r.fail_dv = 100; r.max_dv = 0;
    CHECK_EQ(int(tn::deviates(c, r)), 0);
    CHECK_EQ(int(tn::range(c, r)), int(Tone::Fail));
}

// The VOOC family's three groups (one shared detection) count as one: its lead, the first pass, else the first fail.
static void test_the_vooc_family_counts_once() {
    rc::Card c = cardfx::fh22_full();
    using G = charger_test::Group;
    c.group[unsigned(G::Vooc)] = {rc::GV::Skip, 0, 0, uint8_t(charger_test::Reason::NeedsLoad)};
    c.group[unsigned(G::SuperVooc)] = {rc::GV::Pass, 5000, uint8_t(fc::Fail::Detected), 0};
    c.group[unsigned(G::SuperVooc65)] = {rc::GV::Pass, 5000, uint8_t(fc::Fail::Detected), 0};
    CHECK_EQ(rc::vooc_lead(c), unsigned(G::SuperVooc));
    CHECK_EQ(rc::folded(c, unsigned(G::Vooc)), true);
    CHECK_EQ(rc::folded(c, unsigned(G::SuperVooc)), false);
    CHECK_EQ(rc::folded(c, unsigned(G::SuperVooc65)), true);
    CHECK_EQ(rc::folded(c, unsigned(G::Ufcs)), false);
    CHECK_EQ(std::string_view(rc::group_word(unsigned(G::SuperVooc65))) == "VOOC", true);
    CHECK_EQ(std::string_view(rc::group_word(unsigned(G::Ufcs))) == "UFCS", true);
    const tn::Counts n = tn::counts(c);
    CHECK_EQ(int(n.pass), 3);          // PD fixed, AVS, the VOOC family once
    // A card that holds no VOOC group has no lead and folds nothing.
    rc::Card e;
    CHECK_EQ(rc::vooc_lead(e), rc::kGroups);
    CHECK_EQ(rc::folded(e, unsigned(G::Vooc)), true);   // nothing to show anyway: a None group is never listed
}

// A family that never clocked after either wake is a skip (grey), not a fail: it reads "no answer", never red.
static void test_a_silent_vooc_family_is_grey_not_a_fail() {
    rc::Card c;
    using G = charger_test::Group;
    for (G g : {G::Vooc, G::SuperVooc, G::SuperVooc65})
        c.group[unsigned(g)] = {rc::GV::Skip, 0, uint8_t(fc::Fail::NoAnswer), uint8_t(charger_test::Reason::Request)};
    CHECK_EQ(int(tn::group(c, unsigned(G::Vooc))), int(Tone::Skip));
    const tn::Counts n = tn::counts(c);
    CHECK_EQ(int(n.fail), 0);
    CHECK_EQ(int(n.grey), 1);   // one family, one chip
    CHECK_STR(rc::group_why(unsigned(G::SuperVooc), c.group[unsigned(G::SuperVooc)]), "no answer to either wake, unloaded");
}

int main() {
    test_a_tracking_deviation_is_a_caution();
    test_a_fold_is_a_caution();
    test_fh22();
    test_rules();
    test_light_card_pd_is_green_legacy_is_noted();
    test_the_vooc_family_counts_once();
    test_a_silent_vooc_family_is_grey_not_a_fail();
    return test_report("card_tone");
}
