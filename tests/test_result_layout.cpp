// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Results screen layout (k2/chargertest/result_layout.hpp): chips, tones, collapsing, wrapping. Expected PDO decodings were
// computed by hand from the PD 3.1 field layouts (the Baseus CJ21 run, tests/fixtures/charger_run3_baseus.json).
#include "test_util.h"
#include "k2/chargertest/result_layout.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include <cstring>
#include <string>
#include <vector>

using namespace k2;
namespace ct = k2::charger_test;
namespace rl = k2::result_layout;
using A = ct::Algorithm;
using V = ct::Verdict;

static ct::Step step(A a, unsigned mv, V v, uint8_t pos = 0, uint32_t pdo = 0, ct::Reason r = ct::Reason::None) {
    ct::Step s; s.algorithm = a; s.group = ct::group(a); s.mv = uint16_t(mv); s.verdict = v; s.pos = pos; s.pdo = pdo; s.reason = r;
    return s;
}
static std::vector<ct::Step> baseus() {
    std::vector<ct::Step> s;
    const uint32_t fixed[] = {167874860u, 184620u, 246060u, 307500u, 409935u};
    const unsigned mv[] = {5000, 9000, 12000, 15000, 20000};
    for (int i = 0; i < 5; ++i) s.push_back(step(A::Pd3Fixed, mv[i], V::Pass, uint8_t(i + 1), fixed[i]));
    s.push_back(step(A::Epr, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    s.push_back(step(A::SprAvs, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    for (unsigned t : {5000u, 8000u, 11000u}) s.push_back(step(A::Pps, t, V::Pass, 6, 3235656292u));
    s.push_back(step(A::Pps, 8000, V::Fail, 6, 3235656292u));
    s.push_back(step(A::Pps, 5000, V::Skip, 6, 3235656292u, ct::Reason::Request));
    for (unsigned t : {5000u, 13000u, 21000u}) s.push_back(step(A::Pps, t, V::Pass, 7, 3248763456u));
    s.push_back(step(A::EprAvs, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    for (A a : {A::Qc2, A::Qc3}) s.push_back(step(a, 5000, V::Skip, 0, 0, ct::Reason::PdActive));
    s.push_back(step(A::Fcp, 5000, V::Fail)); s.push_back(step(A::Fcp, 9000, V::Skip, 0, 0, ct::Reason::Request));
    s.push_back(step(A::Ufcs, 5000, V::Fail));
    return s;
}
static std::string chips(const rl::Layout& l, size_t row) {
    std::string out;
    for (unsigned i = l.rows[row].first; i < unsigned(l.rows[row].first) + l.rows[row].count; ++i) {
        if (!out.empty()) out += ' ';
        out += l.tokens[i].text;
    }
    return out;
}
static const rl::Row* find(const rl::Layout& l, const char* label) {
    for (size_t i = 0; i < l.row_count; ++i) if (std::string(l.rows[i].label) == label) return &l.rows[i];
    return nullptr;
}
static size_t row_of(const rl::Layout& l, const char* label) { return size_t(find(l, label) - l.rows.data()); }

static void test_fixed_pd_chips_carry_advertised_current() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    const size_t r = row_of(l, "PD3.x");
    CHECK_STR(chips(l, r).c_str(), "5V 3A 9V 3A 12V 3A 15V 3A 20V 3.35A");
    CHECK_EQ(int(l.tokens[l.rows[r].first].tone), int(rl::Tone::Pass));
    CHECK_EQ(l.tokens[l.rows[r].first].chip, true);
}
static void test_failed_and_skipped_fixed_chips() {
    auto s = baseus(); s[1].verdict = V::Fail; s[2].verdict = V::Skip;
    rl::Layout l; rl::build(l, s);
    const auto& r = l.rows[row_of(l, "PD3.x")];
    CHECK_EQ(int(l.tokens[r.first + 1].tone), int(rl::Tone::Fail));
    CHECK_EQ(int(l.tokens[r.first + 2].tone), int(rl::Tone::Skip));   // a skip is its own tone (grey dash), not "some passed"
}
static void test_marks_give_each_verdict_its_own_shape() {
    CHECK_EQ(int(rl::mark(rl::Tone::Pass)), 1);   // check
    CHECK_EQ(int(rl::mark(rl::Tone::Fail)), 2);   // cross
    CHECK_EQ(rl::mark(rl::Tone::Skip), '-');
    CHECK_EQ(rl::mark(rl::Tone::Partial), '~');
    CHECK_EQ(rl::mark(rl::Tone::Pending), '.');
    CHECK_EQ(rl::mark(rl::Tone::Dim), ' ');
    // Pass, fail and skip never share a shape, so the verdict survives a colour-blind or washed-out panel.
    CHECK_EQ(rl::mark(rl::Tone::Pass) != rl::mark(rl::Tone::Fail) && rl::mark(rl::Tone::Fail) != rl::mark(rl::Tone::Skip) &&
             rl::mark(rl::Tone::Pass) != rl::mark(rl::Tone::Skip), true);
}
static void test_chip_width_includes_the_mark_cell() {
    rl::Token chip; std::strcpy(chip.text, "20V3A"); chip.chip = true;
    rl::Token word = chip; word.chip = false;
    const rl::Metrics m;
    CHECK_EQ(rl::token_width(chip, m), 5 * m.adv + 2 * m.pad + m.mark);
    CHECK_EQ(rl::token_width(word, m), 5 * m.adv);   // a plain word has no mark
}
static void test_pps_one_coloured_chip_per_range() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    const size_t r = row_of(l, "PPS");                    // one row per APDO: slot 6 first, slot 7 right after
    CHECK_STR(chips(l, r).c_str(), "5-11V 5A");
    const auto& row = l.rows[r];
    CHECK_EQ(row.count, 1);
    CHECK_EQ(row.pos, 6);
    CHECK_EQ(l.tokens[row.first].chip, true);
    CHECK_EQ(int(l.tokens[row.first].tone), int(rl::Tone::Partial));       // a step failed and one skipped
    const size_t r2 = r + 1;
    CHECK_STR(l.rows[r2].label, "PPS");
    CHECK_EQ(l.rows[r2].pos, 7);
    CHECK_STR(chips(l, r2).c_str(), "5-21V 3.2A");
    CHECK_EQ(int(l.tokens[l.rows[r2].first].tone), int(rl::Tone::Pass));    // every step of the second PDO passed
}
// ---- The tone rules (one table for fixed, range and legacy chips) ------------------------------------------------------------
//   a step still to run  -> Pending      nothing sampled (all skipped) -> Skip (grey, the tag says why)
//   every sampled step passed -> Pass    some passed, or capped -> Partial     none passed -> Fail
static void test_roll_up_is_one_table() {
    using T = rl::Tally;
    CHECK_EQ(int(rl::roll_up(T{.pass = 3})), int(rl::Tone::Pass));
    CHECK_EQ(int(rl::roll_up(T{.pass = 2, .fail = 1})), int(rl::Tone::Partial));
    CHECK_EQ(int(rl::roll_up(T{.pass = 2, .capped = 1})), int(rl::Tone::Partial));
    CHECK_EQ(int(rl::roll_up(T{.capped = 1, .skip = 2})), int(rl::Tone::Partial));      // capped alone still reached something
    CHECK_EQ(int(rl::roll_up(T{.pass = 1, .skip = 2})), int(rl::Tone::Partial));        // a step that never ran: not fully covered
    CHECK_EQ(int(rl::roll_up(T{.fail = 2})), int(rl::Tone::Fail));
    CHECK_EQ(int(rl::roll_up(T{.fail = 1, .skip = 2})), int(rl::Tone::Fail));
    CHECK_EQ(int(rl::roll_up(T{.skip = 3})), int(rl::Tone::Skip));
    CHECK_EQ(int(rl::roll_up(T{.pass = 1, .pending = 1})), int(rl::Tone::Pending));    // a step still to run wins
    CHECK_EQ(int(rl::roll_up(T{})), int(rl::Tone::Pending));
}
// The Baseus FH22: PPS 5-21V 5A whose converter stops at about 19.4V. 13V passed, 21V plateaued at 19.4V (Capped), and the
// chain went on: 13V, 9V and 5V passed. The chip keeps the advertised range, amber, and the row says what was reached.
static std::vector<ct::Step> fh22() {
    std::vector<ct::Step> s;
    const uint32_t pdo = 3248763456u;   // PPS 5-21V 3.2A, slot 7 of the CJ21 run
    const unsigned mv[] = {13000, 21000, 13000, 9000, 5000};
    for (unsigned m : mv) s.push_back(step(A::Pps, m, V::Pass, 7, pdo));
    s[0].measured_mv = 12965;
    s[1].verdict = V::Fail; s[1].reason = ct::Reason::Capped; s[1].measured_mv = 19419;
    s[2].measured_mv = 12970; s[3].measured_mv = 8990; s[4].measured_mv = 4990;
    return s;
}
static void test_a_capped_range_chip_is_amber_and_keeps_the_advertised_range() {
    auto s = fh22();
    rl::Layout l; rl::build(l, s);
    const size_t r = row_of(l, "PPS");
    CHECK_STR(chips(l, r).c_str(), "5-21V 3.2A");
    CHECK_EQ(int(l.tokens[l.rows[r].first].tone), int(rl::Tone::Partial));
    // the same chip without the cap reads green
    s[1].verdict = V::Pass; s[1].reason = ct::Reason::None;
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "PPS")].first].tone), int(rl::Tone::Pass));
}
static void test_a_range_with_nothing_passed_stays_red_and_all_skipped_grey() {
    auto s = fh22();
    for (auto& x : s) { x.verdict = V::Fail; x.reason = ct::Reason::Timeout; x.measured_mv = 0; }
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "PPS")].first].tone), int(rl::Tone::Fail));
    for (auto& x : s) { x.verdict = V::Skip; x.reason = ct::Reason::Request; }
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "PPS")].first].tone), int(rl::Tone::Skip));
    // a capped step with nothing else passed is still amber: the source delivered something below the target
    s = fh22();
    for (auto& x : s) if (x.reason != ct::Reason::Capped) { x.verdict = V::Fail; x.reason = ct::Reason::Timeout; }
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "PPS")].first].tone), int(rl::Tone::Partial));
}
static void test_a_legacy_chip_with_a_pass_and_a_fail_is_amber() {
    std::vector<ct::Step> s;
    for (unsigned mv : {5000u, 9000u, 12000u, 9000u, 5000u}) s.push_back(step(A::Qc3, mv, V::Pass));
    s[3].verdict = V::Fail;                               // the second 9V step (the way back down) failed
    rl::Layout l; rl::build(l, s);
    const auto& r = l.rows[row_of(l, "QC3")];
    CHECK_STR(chips(l, row_of(l, "QC3")).c_str(), "5V 9V 12V");
    CHECK_EQ(int(l.tokens[r.first].tone), int(rl::Tone::Pass));
    CHECK_EQ(int(l.tokens[r.first + 1].tone), int(rl::Tone::Partial));
    CHECK_EQ(int(l.tokens[r.first + 2].tone), int(rl::Tone::Pass));
}
static std::string text_of(const rl::Reach& r, bool footer) {
    const auto t = footer ? rl::reach_footer(r) : rl::reach_span(r);
    return std::string(t.view());
}
static void test_reach_says_what_the_source_delivered() {
    auto s = fh22();
    auto r = rl::reach(s, A::Pps);
    CHECK_EQ(int(r.tone), int(rl::Tone::Partial));
    CHECK_EQ(r.capped, true);
    CHECK_EQ(r.lo_mv, 5000); CHECK_EQ(r.hi_mv, 19419); CHECK_EQ(r.adv_mv, 21000);
    CHECK_STR(text_of(r, true).c_str(), "caps 19.4V of 21V");
    CHECK_STR(text_of(r, false).c_str(), "5-19.4V of 21V");
    // every step passed: the whole range was reached
    for (auto& x : s) { x.verdict = V::Pass; x.reason = ct::Reason::None; }
    r = rl::reach(s, A::Pps);
    CHECK_EQ(int(r.tone), int(rl::Tone::Pass));
    CHECK_STR(text_of(r, true).c_str(), "reached 5-21V");
    CHECK_STR(text_of(r, false).c_str(), "5-21V");
    // a plain failure above some passes: the highest level that passed
    s = fh22();
    s[1].reason = ct::Reason::Timeout; s[1].measured_mv = 0;
    s[2].verdict = s[3].verdict = s[4].verdict = V::Skip;
    r = rl::reach(s, A::Pps, 7);
    CHECK_EQ(r.capped, false); CHECK_EQ(r.lo_mv, 13000); CHECK_EQ(r.hi_mv, 13000);
    CHECK_STR(text_of(r, true).c_str(), "max 13V of 21V");
    // nothing passed / nothing ran
    for (auto& x : s) { x.verdict = V::Fail; x.measured_mv = 0; }
    CHECK_STR(text_of(rl::reach(s, A::Pps), true).c_str(), "none reached");
    for (auto& x : s) x.verdict = V::Skip;
    CHECK_STR(text_of(rl::reach(s, A::Pps), true).c_str(), "not run");
    CHECK_EQ(rl::reach(s, A::Qc2).adv_mv, 0);   // an algorithm with no steps: empty
}
// pattern pps-off-at-no-load: a range whose levels ended "needs load" never reads "not run" (that word is for a range nothing ran
// on): the Results footer says why. Some levels passed first: the footer keeps the highest one that did.
static void test_reach_footer_says_needs_load_not_not_run() {
    const uint32_t pdo = 3248763456u;   // PPS 5-21V, slot 7
    for (const A a : {A::Pps, A::SprAvs, A::EprAvs}) {
        std::vector<ct::Step> all{step(a, 13000, V::Skip, 7, pdo, ct::Reason::NeedsLoad), step(a, 21000, V::Skip, 7, pdo, ct::Reason::NeedsLoad)};
        auto r = rl::reach(all, a);
        CHECK_EQ(int(r.tone), int(rl::Tone::Skip));
        CHECK_STR(text_of(r, true).c_str(), "needs load");
        // never run (a plain skip) still reads "not run"
        for (auto& x : all) x.reason = ct::Reason::NoOffer;
        CHECK_STR(text_of(rl::reach(all, a), true).c_str(), "not run");
    }
    std::vector<ct::Step> mixed{step(A::Pps, 5000, V::Pass, 7, pdo), step(A::Pps, 13000, V::Skip, 7, pdo, ct::Reason::NeedsLoad),
                                step(A::Pps, 21000, V::Skip, 7, pdo, ct::Reason::NeedsLoad)};
    const auto m = rl::reach(mixed, A::Pps);
    CHECK_EQ(int(m.tone), int(rl::Tone::Partial));
    CHECK_STR(text_of(m, true).c_str(), "needs load >5V");   // the levels above the highest one that passed need a load
    CHECK_EQ(9 + rl::reach_footer(m).len <= 27, true);
}
// A row with two ranges reports the one that is not clean (the first), so the footer never hides a cap behind a green sibling.
static void test_row_reach_picks_the_range_that_is_not_clean() {
    auto s = baseus();                                    // PPS slot 6 has a fail, slot 7 is clean
    const auto r = rl::reach(s, A::Pps);
    CHECK_EQ(r.adv_mv, 11000);
    CHECK_EQ(int(r.tone), int(rl::Tone::Partial));
    for (auto& x : s) if (x.algorithm == A::Pps && x.pos == 6) { x.verdict = V::Pass; x.reason = ct::Reason::None; }
    CHECK_EQ(rl::reach(s, A::Pps).adv_mv, 21000);        // all clean: the last range
}
// The longest words the footer may print after a 9-character label ("SPR AVS: ") fit the 27-cell footer.
static void test_reach_words_fit_the_footer_next_to_a_label() {
    rl::Reach r; r.lo_mv = 15000; r.adv_mv = 48000; r.hi_mv = 36400; r.capped = true; r.tone = rl::Tone::Partial;
    CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
    r.capped = false;
    CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
    r.tone = rl::Tone::Pass; r.hi_mv = 48000;
    CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
    r.tone = rl::Tone::Partial; r.lo_mv = 3300; r.hi_mv = 20100; r.adv_mv = 20100;
    CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
}
static void test_a_range_not_offered_reads_none() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "SPR AVS")).c_str(), "None");
    CHECK_STR(chips(l, row_of(l, "EPR AVS")).c_str(), "None");
}
// An EPR-capable charger on a plain cable: the EPR / EPR AVS steps are skipped for the cable limit and carry no voltage
// and no PDO, so a row of their own would be a bare label (a blank-looking line next to PPS). They fold into "Skipped".
static void test_cable_limited_epr_rows_fold_instead_of_showing_a_bare_label() {
    auto s = baseus();
    s[5] = step(A::Epr, 0, V::Skip, 0, 0, ct::Reason::CableLimit);
    for (auto& x : s) if (x.algorithm == A::EprAvs) x = step(A::EprAvs, 0, V::Skip, 0, 0, ct::Reason::CableLimit);
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(find(l, "EPR") == nullptr, true);
    CHECK_EQ(find(l, "EPR AVS") == nullptr, true);
    CHECK_STR(chips(l, row_of(l, "Skipped")).c_str(), "EPR EPR AVS QC2 QC3");
    for (size_t r = 0; r < l.row_count; ++r) if (!l.rows[r].header) CHECK_EQ(l.rows[r].count > 0, true);   // no row is a label alone
    std::array<rl::Line, rl::kMaxLines> lines;
    const size_t n = rl::wrap(l, rl::Metrics{}, lines);
    for (size_t i = 0; i < n; ++i) if (!l.rows[lines[i].row].header) CHECK_EQ(lines[i].count > 0, true);   // and no line is blank
}
// One chip per EPR fixed level the source offers. A level the K2 does not run says why in a tag glued to its chip, so the page
// reads "28V 5A 36V 5A >36V": the verdict colour alone never explains a skip.
static void test_epr_levels_are_chips_with_the_skip_reason() {
    const uint32_t v28 = (560u << 10) | 500, v36 = (720u << 10) | 500, v48 = (960u << 10) | 500;
    std::vector<ct::Step> s{step(A::Epr, 28000, V::Pass, 8, v28),
                            step(A::Epr, 36000, V::Skip, 9, v36, ct::Reason::OverCap),
                            step(A::Epr, 48000, V::Skip, 10, v48, ct::Reason::OverCap)};
    s[1].detail = s[2].detail = ct::kDetailMonitor36;
    rl::Layout l; rl::build(l, s);
    const size_t r = row_of(l, "EPR");
    CHECK_STR(chips(l, r).c_str(), "28V 5A 36V 5A >36V 48V 5A >36V");
    const auto& row = l.rows[r];
    CHECK_EQ(l.tokens[row.first + 2].chip, false);
    CHECK_EQ(l.tokens[row.first + 2].glue, true);
    CHECK_EQ(int(l.tokens[row.first + 2].tone), int(rl::Tone::Caution));
    // a refused entry, a cable limit and the 48 V monitor name themselves
    s = {step(A::Epr, 28000, V::Skip, 8, v28, ct::Reason::EprRefused), step(A::Epr, 36000, V::Skip, 9, v36, ct::Reason::CableLimit),
         step(A::Epr, 48000, V::Skip, 10, v48, ct::Reason::OverCap)};
    s[2].detail = ct::kDetailMonitor48;
    rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "EPR")).c_str(), "28V 5A refused 36V 5A cable 48V 5A >48V");
    // an EPR AVS range that was skipped carries its tag too
    s = {step(A::EprAvs, 15000, V::Skip, 11, 0xD0000000u | (480u << 17) | (150u << 8) | 140, ct::Reason::EprRefused)};
    s[0].detail = ct::kDetailEprFail + 3;
    rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "EPR AVS")).c_str(), "15-48V 2.91A refused");   // the old 12-byte chip cut this to "15-48V 2.91"
}
static void test_legacy_chips_collapse_distinct_voltages() {
    std::vector<ct::Step> s;
    for (unsigned mv : {5000u, 9000u, 20000u, 9000u, 5000u}) s.push_back(step(A::Qc3, mv, V::Pass));
    s[2].verdict = V::Fail;
    rl::Layout l; rl::build(l, s);
    const size_t r = row_of(l, "QC3");
    CHECK_STR(chips(l, r).c_str(), "5V 9V 20V");
    CHECK_EQ(int(l.tokens[l.rows[r].first + 2].tone), int(rl::Tone::Fail));
    CHECK_EQ(int(l.tokens[l.rows[r].first + 1].tone), int(rl::Tone::Pass));
}
// A legacy chip names its level: the Running grid finds the chip of the step in flight by it, since the chip of 5 V stays pending
// while its later (down) step is left. Fixed PD chips carry their level too; a range chip has none.
static void test_chips_carry_their_level() {
    std::vector<ct::Step> s;
    for (unsigned mv : {5000u, 9000u, 12000u, 9000u, 5000u}) s.push_back(step(A::Qc3, mv, V::Pass));
    s[0].verdict = V::Pass; s[1].verdict = V::Running; s[2].verdict = V::Pending; s[3].verdict = V::Pending; s[4].verdict = V::Pending;
    rl::Layout l; rl::build(l, s);
    const size_t r = row_of(l, "QC3");
    CHECK_EQ(int(l.tokens[l.rows[r].first].mv), 5000);
    CHECK_EQ(int(l.tokens[l.rows[r].first + 1].mv), 9000);
    CHECK_EQ(int(l.tokens[l.rows[r].first + 2].mv), 12000);
    CHECK_EQ(int(l.tokens[l.rows[r].first].tone), int(rl::Tone::Pending));   // 5 V: its down step is still to come
    rl::Layout p; auto b = baseus(); rl::build(p, b);
    const size_t pr = row_of(p, "PD3.x");
    CHECK_EQ(int(p.tokens[p.rows[pr].first + 1].mv), 9000);
    const size_t pps = row_of(p, "PPS");
    CHECK_EQ(int(p.tokens[p.rows[pps].first].mv), 0);
}
static void test_absent_protocols_fold_into_one_row() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "EPR FCP UFCS");   // the words follow the plan's order (here: the fixture's)
    CHECK_EQ(int(find(l, "No reply")->label_tone), int(rl::Tone::Dim));
    CHECK_STR(chips(l, row_of(l, "Skipped")).c_str(), "QC2 QC3");
    CHECK_EQ(int(find(l, "Skipped")->label_tone), int(rl::Tone::Caution));
    CHECK_EQ(find(l, "EPR") == nullptr, true);
    CHECK_EQ(find(l, "QC2") == nullptr, true);
}
static void test_a_legacy_unit_keeps_its_row_when_folding_is_off() {
    // The Running grid: a failed or skipped legacy unit stays where the plan put it, so the rows above the running one stay put.
    auto s = baseus();
    rl::OemNotes oem; oem.fold = false;
    rl::Layout l; rl::build(l, s, oem);
    CHECK_EQ(find(l, "FCP") != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "FCP")).c_str(), "5V 9V");
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "FCP")].first].tone), int(rl::Tone::Fail));
    CHECK_EQ(find(l, "QC2") != nullptr, true);          // skipped-only units keep their rows too
    CHECK_EQ(find(l, "UFCS") != nullptr, true);
    CHECK_EQ(find(l, "EPR") == nullptr, true);          // a PD protocol the source does not offer still folds
    CHECK_EQ(find(l, "QC2") < find(l, "FCP"), true);    // plan order (the fixture runs QC2 first), not "failed units last"
    CHECK_EQ(find(l, "No reply") != nullptr, true);     // ...into the trailing words row
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "EPR");
}
static void test_a_protocol_with_a_pass_is_never_folded() {
    std::vector<ct::Step> s{step(A::Afc, 9000, V::Pass), step(A::Afc, 12000, V::Fail)};
    rl::Layout l; rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "AFC")).c_str(), "9V 12V");
    CHECK_EQ(find(l, "No reply") == nullptr, true);
}
static void test_pending_steps_are_dim() {
    std::vector<ct::Step> s{step(A::Afc, 9000, V::Pending), step(A::Afc, 12000, V::Running)};
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(int(l.tokens[0].tone), int(rl::Tone::Pending));
}
static void test_revision_rows_are_ignored() {
    std::vector<ct::Step> s{step(A::Pd30Revision, 0, V::Skip, 0, 0, ct::Reason::RevisionUntestable)};
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(l.row_count, 0);
}
static void test_wrap_breaks_between_chips_and_keeps_range_with_its_chip() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    std::array<rl::Line, rl::kMaxLines> lines;
    const rl::Metrics m;   // 6 px glyphs, 2 px padding, 2 px gap, an 11 px mark cell per chip, 158 px
    const size_t n = rl::wrap(l, m, lines);
    // Row 0 is the "USB PD" header (its own line; the Huawei and QC/UFCS sections hold only folded rows, so they show no
    // header), then PD3.x: "5V 3A"(45) "9V 3A"(45) "12V 3A"(51) = 145 fits, "15V 3A" (51)
    // would make 198; the second line holds "15V 3A" and "20V 3.35A" (69).
    CHECK_EQ(lines[0].head, true); CHECK_EQ(lines[0].count, 0); CHECK_EQ(l.rows[lines[0].row].header, true);
    CHECK_EQ(lines[1].head, true); CHECK_EQ(lines[1].count, 3);
    CHECK_EQ(lines[2].head, false); CHECK_EQ(lines[2].count, 2); CHECK_EQ(lines[2].row, 1);
    // SPR AVS "None" (row 2), then PPS slot 6 "5-11V 5A"(63) on its own line (row 3), then PPS slot 7 "5-21V 3.2A"(75)
    // (row 4): one row per APDO, each chip on its row's line.
    CHECK_EQ(lines[3].row, 2); CHECK_EQ(lines[3].count, 1);
    CHECK_EQ(lines[4].row, 3); CHECK_EQ(lines[4].count, 1); CHECK_EQ(lines[4].head, true);
    CHECK_EQ(lines[5].row, 4); CHECK_EQ(lines[5].count, 1); CHECK_EQ(lines[5].head, true);
    CHECK_EQ(n, 9);   // header, PD3.x 2, SPR AVS 1, PPS 1+1, EPR AVS 1, No reply 1 (FCP EPR UFCS), Skipped 1
}
static void test_wrap_never_splits_a_glued_pair() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    std::array<rl::Line, rl::kMaxLines> lines;
    rl::Metrics m; m.avail = 120;   // too narrow for both PPS pairs on one line
    const size_t n = rl::wrap(l, m, lines);
    for (size_t i = 0; i < n; ++i) {
        if (lines[i].count && l.tokens[lines[i].first].glue) { CHECK_EQ(0, 1); }   // a line never starts with a glued token
    }
    CHECK_EQ(n >= 5, true);
}
static void test_wrap_max_groups_caps_the_chips_per_line() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    std::array<rl::Line, rl::kMaxLines> lines;
    rl::Metrics m; m.avail = 1000; m.max_groups = 2;   // plenty of width: only the cap breaks lines
    const size_t n = rl::wrap(l, m, lines);
    bool broke = false;
    for (size_t i = 0; i < n; ++i) {
        int groups = 0;
        for (unsigned t = lines[i].first; t < unsigned(lines[i].first) + lines[i].count; ++t) groups += !l.tokens[t].glue;
        CHECK_EQ(groups <= 2, true);
        broke = broke || (!lines[i].head && lines[i].count);
    }
    CHECK_EQ(broke, true);   // a row of five fixed PDOs needs a second and a third line
    rl::Metrics free_m; free_m.avail = 1000;
    std::array<rl::Line, rl::kMaxLines> wide;
    CHECK_EQ(rl::wrap(l, free_m, wide) < n, true);   // the default (0) keeps every chip on one line
}
static void test_wrap_cells_are_equal_and_hold_the_widest_volt_text() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    std::array<rl::Line, rl::kMaxLines> lines;
    rl::Metrics m; m.adv = 8; m.pad = 2; m.gap = 1; m.avail = 163; m.max_groups = 5; m.cells = true;
    const size_t n = rl::wrap(l, m, lines);
    for (size_t i = 0; i < n; ++i) {
        int groups = 0, widest = 0;
        for (unsigned t = lines[i].first; t < unsigned(lines[i].first) + lines[i].count; ++t) {
            if (l.tokens[t].glue) continue;
            ++groups;
            widest = std::max(widest, rl::cell_width(l.tokens[t], m));
        }
        CHECK_EQ(groups <= 5, true);
        CHECK_EQ(groups == 0 || groups * (widest + m.gap) - m.gap <= m.avail, true);   // every cell holds its volt text
    }
    // A range chip ("3.3-11V", 7 chars) is too wide for a crowded line: no line carries it with more than two others.
    for (size_t i = 0; i < n; ++i) {
        int groups = 0, widest = 0;
        for (unsigned t = lines[i].first; t < unsigned(lines[i].first) + lines[i].count; ++t)
            if (!l.tokens[t].glue) { ++groups; widest = std::max(widest, rl::cell_width(l.tokens[t], m)); }
        if (widest >= 7 * 8) CHECK_EQ(groups <= 2, true);
    }
}
static void test_empty_run_is_empty() {
    rl::Layout l; rl::build(l, {});
    std::array<rl::Line, rl::kMaxLines> lines;
    CHECK_EQ(rl::wrap(l, {}, lines), 0);
}
// A fact row ("QC5  VID 05C6  PPS 3.3-21V") is appended after the protocol rows: plain words, a collapsed row.
static void test_note_row_is_appended_as_plain_words() {
    rl::Layout l; rl::build(l, baseus());
    const size_t rows = l.row_count;
    rl::add_note(l, "QC5", rl::Tone::Caution, {"VID 05C6", "PPS 3.3-21V"});
    CHECK_EQ(l.row_count, rows + 1);
    CHECK_STR(l.rows[rows].label, "QC5");
    CHECK_EQ(int(l.rows[rows].algorithm), int(A::Count));   // OK on it opens the whole step list, like "No reply"
    CHECK_EQ(chips(l, rows) == "VID 05C6 PPS 3.3-21V", true);
    CHECK_EQ(l.tokens[l.rows[rows].first].chip, false);
    std::array<rl::Line, rl::kMaxLines> lines;
    const size_t n = rl::wrap(l, {}, lines);
    CHECK_EQ(lines[n - 1].row, rows);                       // 8 + 11 characters fit one line of 170 px
    CHECK_EQ(lines[n - 1].count, 2);
    // A full layout drops the note instead of overflowing.
    rl::Layout full{}; full.row_count = rl::kMaxRows; full.token_count = rl::kMaxTokens;
    rl::add_note(full, "QC5", rl::Tone::Caution, {"VID 05C6"});
    CHECK_EQ(full.row_count, rl::kMaxRows);
    CHECK_EQ(full.token_count, rl::kMaxTokens);
}
// The results tab is split into sections in the default run order: Huawei, Samsung, OPPO, Xiaomi, OEM / PD (label
// facts), USB PD (CC protocols), QC/UFCS (standard D+/D-).
static std::vector<ct::Step> every_family() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Fcp, 9000, V::Pass));
    s.push_back(step(A::Afc, 9000, V::Pass));
    s.push_back(step(A::Vooc, 5000, V::Pass));
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    s.push_back(step(A::Qc2, 9000, V::Pass));
    return s;   // the default run order: the layout keeps the plan's order and cuts a section wherever the family changes
}
static void test_results_split_into_sections() {
    rl::Layout l; rl::build(l, every_family());
    CHECK_EQ(l.rows[0].header, true);
    CHECK_STR(l.rows[0].label, "Huawei");
    const size_t samsung = row_of(l, "Samsung"), oppo = row_of(l, "OPPO"), pd = row_of(l, "USB PD"), qc = row_of(l, "QC/UFCS");
    CHECK_EQ(l.rows[samsung].header && l.rows[oppo].header && l.rows[pd].header && l.rows[qc].header, true);
    CHECK_EQ(row_of(l, "FCP") < samsung, true);
    CHECK_EQ(samsung < row_of(l, "AFC"), true);
    CHECK_EQ(row_of(l, "AFC") < oppo, true);
    CHECK_EQ(oppo < row_of(l, "VOOC"), true);
    CHECK_EQ(row_of(l, "VOOC") < pd, true);
    CHECK_EQ(pd < row_of(l, "PD3.x"), true);
    CHECK_EQ(row_of(l, "PD3.x") < qc, true);
    CHECK_EQ(qc < row_of(l, "QC2"), true);
    CHECK_EQ(find(l, "OEM / PD") == nullptr, true);       // no notes -> no OEM / PD section
    CHECK_EQ(find(l, "Xiaomi") == nullptr, true);         // no Mi auth and no 6 A probe -> no Xiaomi section
}
static void test_oem_notes_get_their_own_section_between_xiaomi_and_usb_pd() {
    rl::Layout l; rl::build(l, baseus(), {"QC5", "VID 05C6", "Lenovo", true});
    const size_t xiaomi = row_of(l, "Xiaomi");
    CHECK_EQ(l.rows[xiaomi].header, true);
    CHECK_STR(l.rows[xiaomi + 1].label, "Mi Turbo");
    CHECK_STR(chips(l, xiaomi + 1).c_str(), "auth ok");
    const size_t oem = row_of(l, "OEM / PD");
    CHECK_EQ(l.rows[oem].header, true);
    CHECK_EQ(xiaomi < oem, true);
    CHECK_EQ(oem < row_of(l, "USB PD"), true);
    CHECK_STR(l.rows[oem + 1].label, "QC5");
    CHECK_STR(chips(l, oem + 1).c_str(), "VID 05C6");
    CHECK_STR(l.rows[oem + 2].label, "OEM");
    CHECK_STR(chips(l, oem + 2).c_str(), "Lenovo");
}
static void test_empty_sections_are_omitted() {
    std::vector<ct::Step> legacy_only{step(A::Qc2, 5000, V::Pass)};
    rl::Layout l; rl::build(l, legacy_only);
    CHECK_EQ(find(l, "USB PD") == nullptr, true);
    CHECK_EQ(find(l, "Huawei") == nullptr, true);
    CHECK_EQ(find(l, "QC/UFCS") != nullptr, true);
    std::vector<ct::Step> pd_only{step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u)};
    rl::Layout p; rl::build(p, pd_only);
    CHECK_EQ(find(p, "QC/UFCS") == nullptr, true);
    CHECK_EQ(find(p, "USB PD") != nullptr, true);
}
// A family whose protocols all fold into "No reply" / "Skipped" shows no header of its own: a bare header above the next
// header reads as an empty family (most chargers answer none of FCP / AFC / VOOC).
static void test_a_section_that_folds_away_loses_its_header() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Fcp, 5000, V::Fail));
    s.push_back(step(A::Afc, 5000, V::Skip, 0, 0, ct::Reason::PdActive));
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(find(l, "Huawei") == nullptr, true);
    CHECK_EQ(find(l, "Samsung") == nullptr, true);
    CHECK_STR(l.rows[0].label, "USB PD");
    CHECK_EQ(row_of(l, "No reply") > 0, true);
    CHECK_EQ(row_of(l, "Skipped") > 0, true);
}
static void test_section_of_and_vendor_names() {
    CHECK_EQ(int(rl::section_of(A::Pd3Fixed)), int(rl::Section::UsbPd));
    CHECK_EQ(int(rl::section_of(A::Pps)), int(rl::Section::UsbPd));
    CHECK_EQ(int(rl::section_of(A::EprAvs)), int(rl::Section::UsbPd));
    CHECK_EQ(int(rl::section_of(A::Fcp)), int(rl::Section::Huawei));
    CHECK_EQ(int(rl::section_of(A::Scp)), int(rl::Section::Huawei));
    CHECK_EQ(int(rl::section_of(A::Afc)), int(rl::Section::Samsung));
    CHECK_EQ(int(rl::section_of(A::Vooc)), int(rl::Section::Oppo));
    CHECK_EQ(int(rl::section_of(A::SuperVooc65)), int(rl::Section::Oppo));
    CHECK_EQ(int(rl::section_of(A::Vivo)), int(rl::Section::Oppo));
    CHECK_EQ(int(rl::section_of(A::Qc2)), int(rl::Section::Qc));
    CHECK_EQ(int(rl::section_of(A::Qc3ClassB)), int(rl::Section::Qc));
    CHECK_EQ(int(rl::section_of(A::Ufcs)), int(rl::Section::Qc));
    CHECK_STR(rl::vendor_oem_name(0x17EF), "Lenovo");
    CHECK_STR(rl::vendor_oem_name(0x03F0), "HP");
    CHECK_STR(rl::vendor_oem_name(0x2717), "Xiaomi");
    CHECK_STR(rl::vendor_oem_name(0x2B01), "Xiaomi");
    CHECK_EQ(rl::vendor_oem_name(0x1234), nullptr);
}
// The OPPO SVID note is a passive fact row of the OEM section, on its own: it opens the section by itself and names no
// protocol result.
static void test_oppo_svid_note_row() {
    rl::Layout none;
    rl::build(none, baseus());
    CHECK_EQ(find(none, "OPPO") == nullptr, true);
    rl::OemNotes notes;
    notes.oppo_svid = true;
    rl::Layout l;
    rl::build(l, baseus(), notes);
    const rl::Row* r = find(l, "OPPO");
    CHECK_EQ(r != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "OPPO")).c_str(), "SVID 0x22D9 (discovery only)");
    CHECK_EQ(int(r->algorithm), int(A::Count));
    // Beside a QC label it shares the OEM section: one header, both rows.
    notes.qc_label = "QC5";
    rl::Layout both;
    rl::build(both, baseus(), notes);
    CHECK_EQ(find(both, "QC5") != nullptr && find(both, "OPPO") != nullptr, true);
    // The no-header grid keeps the row.
    notes.headers = false;
    rl::Layout dense;
    rl::build(dense, baseus(), notes);
    CHECK_EQ(find(dense, "OPPO") != nullptr, true);
}
// The adapter's SCP readout is a passive fact row of the same section (the words of report_card::scp_note after its label); it
// opens the section by itself.
static void test_scp_capability_note_row() {
    rl::Layout none;
    rl::build(none, baseus());
    CHECK_EQ(find(none, "SCP cap") == nullptr, true);
    rl::OemNotes notes;
    notes.scp_cap = "20V 6A";
    rl::Layout l;
    rl::build(l, baseus(), notes);
    const rl::Row* r = find(l, "SCP cap");
    CHECK_EQ(r != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "SCP cap")).c_str(), "20V 6A");
    CHECK_EQ(int(r->algorithm), int(A::Count));
    notes.oppo_svid = true;
    rl::Layout both;
    rl::build(both, baseus(), notes);
    CHECK_EQ(find(both, "SCP cap") != nullptr && find(both, "OPPO") != nullptr, true);
    notes.headers = false;
    rl::Layout dense;
    rl::build(dense, baseus(), notes);
    CHECK_EQ(find(dense, "SCP cap") != nullptr, true);
}
// A passive label longer than the 8-character row label and the 15-character token is split into words, never cut
// (2026-11-08: "Samsung" read "via PPS 5-2" on the K2). The first word is the row label; the rest wraps in the grid.
static void test_long_passive_labels_split_into_words_and_never_cut() {
    auto row_for = [&](const char* name, const char* why, rl::Layout& l, const char* label) {
        rl::OemNotes notes;
        notes.qc_label = name;
        notes.qc_why = why;
        rl::build(l, baseus(), notes);
        CHECK_EQ(int(l.cuts), 0);
        CHECK_EQ(find(l, label) != nullptr, true);
        return chips(l, row_of(l, label));
    };
    rl::Layout a, b, c, d;
    CHECK_STR(row_for("Samsung SFC 2.0", "via PPS 5-20V 5A", a, "Samsung").c_str(), "SFC 2.0 via PPS 5-20V 5A");
    CHECK_STR(row_for("Samsung SFC 3.0", "via PPS 3.3-21V 6.25A", b, "Samsung").c_str(), "SFC 3.0 via PPS 3.3-21V 6.25A");
    CHECK_STR(row_for("Moto TurboPower", "via PPS 5-20V 6.25A", c, "Moto").c_str(), "TurboPower via PPS 5-20V 6.25A");
    CHECK_STR(row_for("QC4+", "Qualcomm VID, PPS 3.3-21V", d, "QC4+").c_str(), "Qualcomm VID, PPS 3.3-21V");
    // Each token holds whole words (a range stays with its current: "5-20V 5A" is one token) and every token is within 15.
    const rl::Row& r = a.rows[row_of(a, "Samsung")];
    CHECK_EQ(int(r.count), 3);   // "SFC 2.0" | "via PPS" | "5-20V 5A"
    CHECK_STR(a.tokens[r.first + 1].text, "via PPS");
    CHECK_STR(a.tokens[r.first + 2].text, "5-20V 5A");
    // A word that cannot fit a field is counted, not silently shortened.
    rl::OemNotes bad;
    bad.qc_label = "Overlong-label x";
    rl::Layout e;
    rl::build(e, baseus(), bad);
    CHECK_EQ(int(e.cuts) > 0, true);
    bad.qc_label = "QC5";
    bad.qc_why = "via-a-word-of-twenty-chars";
    rl::build(e, baseus(), bad);
    CHECK_EQ(int(e.cuts) > 0, true);
}
static void test_wrap_emits_each_header_as_its_own_line() {
    rl::Layout l; rl::build(l, baseus(), {"QC5", "VID 05C6", nullptr, false});
    std::array<rl::Line, rl::kMaxLines> lines;
    const size_t n = rl::wrap(l, {}, lines);
    unsigned headers = 0;
    for (size_t i = 0; i < l.row_count; ++i) headers += l.rows[i].header;
    CHECK_EQ(headers, 2u);   // OEM / PD, USB PD (Huawei and QC/UFCS fold away entirely: FCP / UFCS never answered)
    for (size_t i = 0; i < n; ++i) {
        if (l.rows[lines[i].row].header) { CHECK_EQ(lines[i].count, 0); CHECK_EQ(lines[i].head, true); }
    }
}
// The dense Results grid leaves the section separators out: every row stays, no row is a header, every line is a unit.
static void test_headers_can_be_left_out() {
    rl::OemNotes notes;
    notes.headers = false;
    rl::Layout with, without;
    rl::build(with, baseus());
    rl::build(without, baseus(), notes);
    unsigned header_rows = 0, rows_with = 0;
    for (size_t i = 0; i < with.row_count; ++i) { header_rows += with.rows[i].header; rows_with += !with.rows[i].header; }
    CHECK_EQ(header_rows > 0, true);
    CHECK_EQ(without.row_count, rows_with);
    for (size_t i = 0; i < without.row_count; ++i) CHECK_EQ(without.rows[i].header, false);
    std::array<rl::Line, rl::kMaxLines> lines;
    const size_t n = rl::wrap(without, {}, lines);
    for (size_t i = 0; i < n; ++i) CHECK_EQ(lines[i].count > 0, true);
}
static ct::Step hunt_step(A a, unsigned mv, V v, unsigned pos, uint32_t pdo) {
    ct::Step h = step(a, mv, v, pos, pdo); h.hunt = true; h.ma = 6000;
    return h;
}
// Row labels in layout order, "*" for a section header ("Xiaomi" -> "*Xiaomi"), to compare whole layouts at once.
static std::string row_order(const rl::Layout& l) {
    std::string out;
    for (size_t i = 0; i < l.row_count; ++i) { if (i) out += ","; if (l.rows[i].header) out += "*"; out += l.rows[i].label; }
    return out;
}
// The 6 A hunt probes are rows of their own ("Mi 6A", one per probed level) in the Xiaomi section, always labelled with the probed
// 6 A (never the advertised PDO max), and stay out of the USB PD fixed/PPS chips.
static void test_hunt_rows_live_in_the_xiaomi_section() {
    auto s = baseus();
    s.push_back(hunt_step(A::Pd3Fixed, 20000, V::Fail, 5, 409935u));
    s.push_back(hunt_step(A::Pps, 11000, V::Pass, 6, 3235656292u));
    rl::Layout l; rl::build(l, s);
    const size_t xiaomi = row_of(l, "Xiaomi");
    CHECK_EQ(l.rows[xiaomi].header, true);
    const size_t r20 = row_of(l, "Mi 6A"), r11 = r20 + 1;
    CHECK_EQ(r20 > xiaomi, true);
    CHECK_EQ(r20 > row_of(l, "PPS"), true);   // the probes stand where the plan runs them: after the PD rows they follow
    CHECK_STR(l.rows[r11].label, "Mi 6A");
    CHECK_EQ(l.rows[r20].probe && l.rows[r11].probe, true);
    CHECK_EQ(l.rows[r20].mv, 20000); CHECK_EQ(l.rows[r11].mv, 11000);
    CHECK_STR(chips(l, r20).c_str(), "20V 6A");
    CHECK_STR(chips(l, r11).c_str(), "11V 6A");
    CHECK_EQ(int(l.tokens[l.rows[r20].first].tone), int(rl::Tone::Fail));
    CHECK_EQ(int(l.tokens[l.rows[r11].first].tone), int(rl::Tone::Pass));
    // The PD chips keep the advertised currents and pass counts, untouched by the hunt rows.
    CHECK_STR(chips(l, row_of(l, "PD3.x")).c_str(), "5V 3A 9V 3A 12V 3A 15V 3A 20V 3.35A");
    const size_t pps = row_of(l, "PPS");   // one row per APDO: slot 6, then slot 7
    CHECK_STR(chips(l, pps).c_str(), "5-11V 5A");
    CHECK_STR(chips(l, pps + 1).c_str(), "5-21V 3.2A");
}
// The rows follow the step list: a probe the planner runs between two PD rows stands between them, and a section header repeats
// where the family changes (no hand-kept order puts the Xiaomi block ahead of the PD rows).
static void test_mi_probe_rows_stand_where_the_plan_runs_them() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    s.push_back(hunt_step(A::Pd3Fixed, 20000, V::Pass, 5, 409935u));
    s.push_back(step(A::Pps, 11000, V::Pass, 6, 3235656292u));
    s.push_back(hunt_step(A::Pps, 11000, V::Pass, 6, 3235656292u));
    s.push_back(step(A::Qc2, 9000, V::Pass));
    rl::Layout l; rl::build(l, s);
    CHECK_STR(row_order(l).c_str(), "*USB PD,PD3.x,*Xiaomi,Mi 6A,*USB PD,PPS,*Xiaomi,Mi 6A,*QC/UFCS,QC2");
    // the dense Results / Running grids leave the headers out: the rows alone are the plan's order
    rl::OemNotes dense; dense.headers = false;
    rl::build(l, s, dense);
    CHECK_STR(row_order(l).c_str(), "PD3.x,Mi 6A,PPS,Mi 6A,QC2");
    CHECK_EQ(l.rows[1].mv, 20000); CHECK_EQ(l.rows[3].mv, 11000);
    // The usual plan (the probes after the PD rows): one Xiaomi section, the auth fact in front of the probes.
    std::vector<ct::Step> t{step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u), step(A::Pps, 11000, V::Pass, 6, 3235656292u),
                            hunt_step(A::Pps, 11000, V::Pass, 6, 3235656292u), hunt_step(A::Pps, 20000, V::Fail, 7, 3235656292u),
                            step(A::Qc2, 9000, V::Pass)};
    rl::OemNotes notes; notes.mi_auth = true;
    rl::build(l, t, notes);
    CHECK_STR(row_order(l).c_str(), "*USB PD,PD3.x,PPS,*Xiaomi,Mi Turbo,Mi 6A,Mi 6A,*QC/UFCS,QC2");
}
// The grid order is the executed order, APDO by APDO: the planner emits a 6 A hunt right after the sample steps of the
// APDO that covers the probed level, so with two PPS APDOs the 11 V probe runs between APDO 1 and APDO 2. One grid row
// per APDO puts the probe row between the two PPS rows; a single PPS row holding every APDO's chip would read the probe
// after the whole row. (ROADMAP 2026-11-09, "a PPS row shows a probe after all its APDOs".)
static void test_probe_between_two_apdos_stands_between_their_rows() {
    const uint32_t pdo6 = 3235656292u;   // PPS 3.3-11V 5A, slot 6
    const uint32_t pdo7 = 3248763456u;   // PPS 5-21V 3.2A, slot 7
    std::vector<ct::Step> s;
    for (unsigned t : {5000u, 8000u, 11000u}) s.push_back(step(A::Pps, t, V::Pass, 6, pdo6));
    s.push_back(hunt_step(A::Pps, 11000, V::Pass, 6, pdo6));   // the plan's order: after APDO 1's steps, before APDO 2's
    for (unsigned t : {5000u, 13000u, 21000u}) s.push_back(step(A::Pps, t, V::Pass, 7, pdo7));
    rl::Layout l; rl::build(l, s);
    CHECK_STR(row_order(l).c_str(), "*USB PD,PPS,*Xiaomi,Mi 6A,*USB PD,PPS");
    // the dense Results / Running grids leave the headers out: the rows alone are the plan's order
    rl::OemNotes dense; dense.headers = false;
    rl::build(l, s, dense);
    CHECK_STR(row_order(l).c_str(), "PPS,Mi 6A,PPS");
    CHECK_EQ(l.rows[1].probe && l.rows[1].mv == 11000, true);
    // Every step's row sits at or after the previous step's row: the grid never reorders the run it shows.
    auto row_of_step = [&](const ct::Step& x) {
        for (size_t r = 0; r < l.row_count; ++r) if (rl::row_has(l.rows[r], x)) return int(r);
        return -1;
    };
    int prev = -1;
    bool mono = true;
    for (const auto& x : s) {
        const int r = row_of_step(x);
        if (r < 0 || (prev >= 0 && r < prev)) mono = false;
        prev = r;
    }
    CHECK_EQ(mono, true);
    // The APDO rows keep their own chips: "5-11V 5A" before the probe, "5-21V 3.2A" after it.
    CHECK_STR(chips(l, 0).c_str(), "5-11V 5A");
    CHECK_STR(chips(l, 1).c_str(), "11V 6A");
    CHECK_STR(chips(l, 2).c_str(), "5-21V 3.2A");
}

// ---- summarize(): the best passing mode per protocol family, as words with volts and amps ---------------------------
static ct::Step hunt(unsigned mv, unsigned ma, V v) {
    ct::Step s = step(A::Pd3Fixed, mv, v); s.hunt = true; s.ma = uint16_t(ma);
    return s;
}
// A 6 A row the source never unlocked is a neutral skip with a word next to its chip, never a red fail.
static void test_hunt_rows_the_source_did_not_unlock_read_locked() {
    ct::Step a = hunt(20000, 6000, V::Skip); a.reason = ct::Reason::NotUnlocked; a.detail = ct::kDetailNoChain;
    ct::Step b = hunt(11000, 6000, V::Skip); b.reason = ct::Reason::NotUnlocked; b.detail = ct::kDetailCableCap;
    rl::Layout l; rl::build(l, std::vector<ct::Step>{a, b});
    const size_t r20 = row_of(l, "Mi 6A");
    CHECK_STR(chips(l, r20).c_str(), "20V 6A locked");
    CHECK_STR(chips(l, r20 + 1).c_str(), "11V 6A cable");
    CHECK_EQ(int(l.tokens[l.rows[r20].first].tone) != int(rl::Tone::Fail), true);
    CHECK_EQ(int(l.tokens[l.rows[r20 + 1].first].tone) != int(rl::Tone::Fail), true);
}
static std::string line_text(const std::vector<rl::SummaryLine>& v, size_t i) { return v[i].text; }
static std::vector<rl::SummaryLine> summary_of(const std::vector<ct::Step>& s, size_t max = 3) {
    std::vector<rl::SummaryLine> out(max);
    out.resize(rl::summarize(s, out));
    return out;
}
static void test_summary_picks_the_highest_wattage_passing_pd_mode() {
    auto s = baseus();
    s[4].verdict = V::Fail;                  // 20V 3.35A failed: the 15V 3A level stays the best fixed mode
    for (size_t i : {7, 8, 9, 12, 13, 14}) s[i].verdict = V::Fail;   // the PPS passes that would beat it fail too
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);
    CHECK_STR(v[0].text, "PD 15V 3A");
    CHECK_EQ(v[0].mw, 45000u);
}
static void test_summary_pps_uses_the_requested_step_and_names_the_mode() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 9000, V::Pass, 1, 184620u));        // 9V 3A = 27 W
    auto pps = step(A::Pps, 11000, V::Pass, 2, 3235656292u); pps.ma = 5000;   // 11V x 5A = 55 W
    s.push_back(pps);
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);
    CHECK_STR(v[0].text, "PPS 11V 5A");
}
static void test_summary_one_line_per_family_ranked_by_watts() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 20000, V::Pass, 1, 409935u));        // 20V 3.35A = 67 W
    s.push_back(hunt(20000, 6000, V::Pass));                           // 120 W
    s.push_back(hunt(11000, 6000, V::Fail));
    s.push_back(step(A::Qc2, 9000, V::Pass)); s.push_back(step(A::Qc3, 12000, V::Pass));
    s.push_back(step(A::Afc, 9000, V::Pass));
    s.push_back(step(A::Fcp, 9000, V::Fail));
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 3u);
    CHECK_STR(v[0].text, "Mi 20V 6A");
    CHECK_STR(v[1].text, "PD 20V 3.35A");
    CHECK_STR(v[2].text, "QC3 12V");        // no amps in the data model for a D+/D- mode: volts only, never invented
    CHECK_EQ(v[2].mw, 0u);
    CHECK_EQ(summary_of(s, 5).size(), 4u);   // AFC 9V is the fourth family
    CHECK_STR(line_text(summary_of(s, 5), 3).c_str(), "AFC 9V");
}
static void test_summary_ignores_failed_skipped_and_pending_steps() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Qc3, 12000, V::Fail)); s.push_back(step(A::Qc2, 9000, V::Skip)); s.push_back(step(A::Afc, 9000, V::Pending));
    CHECK_EQ(summary_of(s).size(), 0u);
    CHECK_EQ(summary_of({}).size(), 0u);
}
static void test_summary_names_a_handshake_without_a_voltage() {
    std::vector<ct::Step> s;
    auto hs = step(A::Vooc, 5000, V::Pass); hs.detail = uint8_t(fc::Fail::NoVoltage);
    auto seen = step(A::SuperVooc, 5000, V::Pass); seen.detail = uint8_t(fc::Fail::Detected);
    s.push_back(hs);
    CHECK_STR(summary_of(s)[0].text, "VOOC ok");
    s.push_back(seen);
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);                  // one OPPO family line: on a tie the more capable protocol wins
    CHECK_STR(v[0].text, "SVOOC seen");
}
static void test_summary_class_b_folds_into_the_qc_family() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Qc3, 12000, V::Pass)); s.push_back(step(A::Qc3ClassB, 20000, V::Pass));
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);
    CHECK_STR(v[0].text, "QC3B 20V");
}
// A Class A charger never reached 20 V: the QC2B / QC3B row stays, with a neutral "Class A" chip, never a red fail and
// never folded into the caution "Skipped" row.
static void test_class_b_on_a_class_a_charger_reads_neutral() {
    std::vector<ct::Step> s{step(A::Qc2, 12000, V::Pass), step(A::Qc2ClassB, 20000, V::Skip, 0, 0, ct::Reason::ClassAOnly),
                            step(A::Qc3, 12000, V::Pass), step(A::Qc3ClassB, 20000, V::Skip, 0, 0, ct::Reason::ClassAOnly)};
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(find(l, "Skipped") == nullptr, true);
    const rl::Row* r = find(l, "QC2B");
    CHECK_EQ(r != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "QC2B")).c_str(), "20V Class A");
    for (unsigned i = r->first; i < unsigned(r->first) + r->count; ++i) {
        CHECK_EQ(int(l.tokens[i].tone) != int(rl::Tone::Fail), true);
        CHECK_EQ(l.tokens[i].tone == rl::Tone::Dim || l.tokens[i].tone == rl::Tone::Skip, true);   // both draw dim
    }
    CHECK_EQ(find(l, "QC3B") != nullptr, true);
}
static void test_class_b_pass_reads_as_a_normal_pass() {
    std::vector<ct::Step> s{step(A::Qc2ClassB, 20000, V::Pass)};
    rl::Layout l; rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "QC2B")).c_str(), "20V");
    CHECK_EQ(int(l.tokens[find(l, "QC2B")->first].tone), int(rl::Tone::Pass));
}
// A charger without QC3+ (no movement, or QC3-sized steps): its 5 V baseline row passed and the raised rows are skipped
// "no QC3+". The row is one neutral "no QC3+" word: no green 5V chip, no red, not folded into the caution "Skipped" row.
static void test_qc3plus_without_response_reads_no_qc3plus() {
    std::vector<ct::Step> s{step(A::Qc3Plus, 5000, V::Pass),
                            step(A::Qc3Plus, 9000, V::Skip, 0, 0, ct::Reason::NoQc3Plus), step(A::Qc3Plus, 12000, V::Skip, 0, 0, ct::Reason::NoQc3Plus),
                            step(A::Qc3Plus, 9000, V::Skip, 0, 0, ct::Reason::NoQc3Plus), step(A::Qc3Plus, 5000, V::Skip, 0, 0, ct::Reason::NoQc3Plus)};
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(find(l, "Skipped") == nullptr, true);
    const rl::Row* r = find(l, "QC3+");
    CHECK_EQ(r != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "QC3+")).c_str(), "no QC3+");
    CHECK_EQ(int(r->count), 1);
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Dim));
    CHECK_EQ(l.tokens[r->first].chip, false);
}
// A real QC3+ charger (every row passed, or a row that really failed) keeps its voltage chips.
static void test_a_real_qc3plus_charger_keeps_its_chips() {
    std::vector<ct::Step> s;
    for (unsigned mv : {5000u, 9000u, 12000u, 9000u, 5000u}) s.push_back(step(A::Qc3Plus, mv, V::Pass));
    rl::Layout l; rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "QC3+")).c_str(), "5V 9V 12V");
    CHECK_EQ(int(l.tokens[find(l, "QC3+")->first].tone), int(rl::Tone::Pass));
    s[2] = step(A::Qc3Plus, 12000, V::Fail);   // moved, then missed the band: still a failure, not "no QC3+"
    rl::Layout f; rl::build(f, s);
    CHECK_EQ(std::string(chips(f, row_of(f, "QC3+"))).find("no QC3+") == std::string::npos, true);
}
// The 5 V QC3+ baseline of a charger without QC3+ never makes "QC3+ 5V" the QC family's summary line.
static void test_summary_ignores_the_baseline_of_a_charger_without_qc3plus() {
    std::vector<ct::Step> s{step(A::Qc3Plus, 5000, V::Pass), step(A::Qc3Plus, 9000, V::Skip, 0, 0, ct::Reason::NoQc3Plus)};
    CHECK_EQ(summary_of(s).size(), 0u);
    s.push_back(step(A::Qc3, 5000, V::Pass));   // a QC3 pass at the same level keeps the family, labelled QC3
    CHECK_STR(line_text(summary_of(s), 0).c_str(), "QC3 5V");
    CHECK_EQ(summary_of({step(A::Qc3Plus, 5000, V::Pass), step(A::Qc3Plus, 9000, V::Pass)}).size(), 1u);   // a real QC3+ charger counts
}
// UFCS rows planned from the charger's Output_Capabilities: each chip is one level with the top current of the mode it samples,
// the same "5V 3A" words as a PD fixed row, so the grid draws one bracket per run of equal current.
static ct::Step in_mode(unsigned mv, V v, int lo_mv, int hi_mv, int lo_ma, int hi_ma) {
    ct::Step s = step(A::Ufcs, mv, v);
    s.pdo = ct::pack_ufcs_mode(lo_mv, hi_mv, lo_ma, hi_ma);
    s.continuous = mv != 5000;
    return s;
}
// CUKTECH-like: 5-11 V 3 A and 5-20 V 3 A (the levels the planner picks: 5 / 8 / 11 V of the first mode, 12.5 / 20 V of the second).
static std::vector<ct::Step> cuktech_ufcs() {
    return {in_mode(5000, V::Pass, 5000, 11000, 1000, 3000), in_mode(8000, V::Pass, 5000, 11000, 1000, 3000),
            in_mode(11000, V::Pass, 5000, 11000, 1000, 3000), in_mode(12500, V::Pass, 5000, 20000, 1000, 3000),
            in_mode(20000, V::Fail, 5000, 20000, 1000, 3000), in_mode(5000, V::Pass, 5000, 11000, 1000, 3000)};
}
static void test_ufcs_chips_carry_the_mode_current() {
    rl::Layout l; rl::build(l, cuktech_ufcs());
    CHECK_STR(chips(l, row_of(l, "UFCS")).c_str(), "5V 3A 8V 3A 11V 3A 12.5V 3A 20V 3A");   // the step back to 5 V is the same chip
    const auto& r = l.rows[row_of(l, "UFCS")];
    CHECK_EQ(int(l.tokens[r.first].tone), int(rl::Tone::Pass));
    CHECK_EQ(int(l.tokens[r.first + 4].tone), int(rl::Tone::Fail));
}
// FH22-like: one 5.5-11 V mode at 4 A (UFCS 11 V 4 A = 44 W); 5 V is below the mode, the baseline row reads it at the mode's current.
static void test_ufcs_chips_follow_the_modes_current_when_it_changes() {
    std::vector<ct::Step> s = {in_mode(5000, V::Pass, 5500, 11000, 1000, 4000), in_mode(5500, V::Pass, 5500, 11000, 1000, 4000),
                               in_mode(8200, V::Pass, 5500, 11000, 1000, 4000), in_mode(11000, V::Pass, 5500, 11000, 1000, 4000),
                               in_mode(5000, V::Pass, 5500, 11000, 1000, 4000)};
    rl::Layout l; rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "UFCS")).c_str(), "5V 4A 5.5V 4A 8.2V 4A 11V 4A");
    // Two modes at different currents: each level reads its own mode's, so the grid brackets 3A and 5A apart.
    std::vector<ct::Step> two = {in_mode(5000, V::Pass, 3400, 11000, 1000, 3250), in_mode(11000, V::Pass, 3400, 11000, 1000, 3250),
                                 in_mode(21000, V::Pass, 11000, 21000, 1000, 5000)};
    rl::Layout t; rl::build(t, two);
    CHECK_STR(chips(t, row_of(t, "UFCS")).c_str(), "5V 3.25A 11V 3.25A 21V 5A");
}
// A plan row made before the charger's list was read (or one that never got it) has no mode: volts only, as before.
static void test_ufcs_chips_without_a_mode_stay_volts_only() {
    std::vector<ct::Step> s = {step(A::Ufcs, 5000, V::Pass), step(A::Ufcs, 9000, V::Pass), step(A::Ufcs, 11000, V::Pass)};
    rl::Layout l; rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "UFCS")).c_str(), "5V 9V 11V");
}
// pattern pps-off-at-no-load: a raised PPS level the source accepted and dropped at no load is skipped "needs load": the range
// chip is amber (some levels ran) or neutral (none did), carries the words, and nothing on the row is a red fail.
static void test_pps_needs_load_reads_as_a_skip_not_a_fail() {
    const uint32_t pdo = 3248763456u;
    std::vector<ct::Step> s{step(A::Pps, 5000, V::Pass, 7, pdo), step(A::Pps, 13000, V::Skip, 7, pdo, ct::Reason::NeedsLoad),
                            step(A::Pps, 21000, V::Skip, 7, pdo, ct::Reason::NeedsLoad)};
    rl::Layout l; rl::build(l, s);
    const rl::Row* r = find(l, "PPS");
    CHECK_EQ(r != nullptr, true);
    CHECK_EQ(chips(l, row_of(l, "PPS")).find("needs load") != std::string::npos, true);
    for (unsigned i = r->first; i < unsigned(r->first) + r->count; ++i) CHECK_EQ(int(l.tokens[i].tone) != int(rl::Tone::Fail), true);
    CHECK_EQ(l.cuts, 0u);
    std::vector<ct::Step> all_skipped{step(A::Pps, 13000, V::Skip, 7, pdo, ct::Reason::NeedsLoad)};
    rl::Layout m; rl::build(m, all_skipped);
    CHECK_EQ(chips(m, row_of(m, "PPS")).find("needs load") != std::string::npos, true);
    CHECK_EQ(int(m.tokens[find(m, "PPS")->first].tone), int(rl::Tone::Skip));
}
int main() {
    test_pps_needs_load_reads_as_a_skip_not_a_fail();
    test_ufcs_chips_carry_the_mode_current();
    test_ufcs_chips_follow_the_modes_current_when_it_changes();
    test_ufcs_chips_without_a_mode_stay_volts_only();
    test_summary_ignores_the_baseline_of_a_charger_without_qc3plus();
    test_qc3plus_without_response_reads_no_qc3plus();
    test_a_real_qc3plus_charger_keeps_its_chips();
    test_class_b_on_a_class_a_charger_reads_neutral();
    test_class_b_pass_reads_as_a_normal_pass();
    test_results_split_into_sections();
    test_oem_notes_get_their_own_section_between_xiaomi_and_usb_pd();
    test_hunt_rows_live_in_the_xiaomi_section(); test_mi_probe_rows_stand_where_the_plan_runs_them();
    test_probe_between_two_apdos_stands_between_their_rows();
    test_hunt_rows_the_source_did_not_unlock_read_locked();
    test_empty_sections_are_omitted();
    test_a_section_that_folds_away_loses_its_header();
    test_section_of_and_vendor_names();
    test_oppo_svid_note_row();
    test_scp_capability_note_row();
    test_long_passive_labels_split_into_words_and_never_cut();
    test_wrap_emits_each_header_as_its_own_line();
    test_note_row_is_appended_as_plain_words();
    test_headers_can_be_left_out();
    test_fixed_pd_chips_carry_advertised_current();
    test_failed_and_skipped_fixed_chips();
    test_marks_give_each_verdict_its_own_shape();
    test_chip_width_includes_the_mark_cell();
    test_pps_one_coloured_chip_per_range();
    test_roll_up_is_one_table();
    test_a_capped_range_chip_is_amber_and_keeps_the_advertised_range();
    test_a_range_with_nothing_passed_stays_red_and_all_skipped_grey();
    test_a_legacy_chip_with_a_pass_and_a_fail_is_amber();
    test_reach_says_what_the_source_delivered();
    test_reach_footer_says_needs_load_not_not_run();
    test_row_reach_picks_the_range_that_is_not_clean();
    test_reach_words_fit_the_footer_next_to_a_label();
    test_a_range_not_offered_reads_none();
    test_cable_limited_epr_rows_fold_instead_of_showing_a_bare_label();
    test_epr_levels_are_chips_with_the_skip_reason();
    test_legacy_chips_collapse_distinct_voltages();
    test_chips_carry_their_level();
    test_absent_protocols_fold_into_one_row();
    test_a_legacy_unit_keeps_its_row_when_folding_is_off();
    test_a_protocol_with_a_pass_is_never_folded();
    test_pending_steps_are_dim();
    test_revision_rows_are_ignored();
    test_wrap_breaks_between_chips_and_keeps_range_with_its_chip();
    test_wrap_never_splits_a_glued_pair();
    test_wrap_max_groups_caps_the_chips_per_line();
    test_wrap_cells_are_equal_and_hold_the_widest_volt_text();
    test_empty_run_is_empty();
    test_summary_picks_the_highest_wattage_passing_pd_mode();
    test_summary_pps_uses_the_requested_step_and_names_the_mode();
    test_summary_one_line_per_family_ranked_by_watts();
    test_summary_ignores_failed_skipped_and_pending_steps();
    test_summary_names_a_handshake_without_a_voltage();
    test_summary_class_b_folds_into_the_qc_family();
    return test_report("test_result_layout");
}
