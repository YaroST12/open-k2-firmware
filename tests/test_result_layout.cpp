// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Results screen layout (k2/chargertest/result_layout.hpp): chips, tones, collapsing, wrapping. Expected PDO decodings were
// computed by hand from the PD 3.1 field layouts (the Baseus CJ21 run, tests/fixtures/charger_run3_baseus.json).
#include "test_util.h"
#include "k2/chargertest/result_layout.hpp"
#include "k2/chargertest/oem_watts.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include <algorithm>
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
// The PD block (one label "PD" on its first row; continuation rows have none): fixed chips first (kind 0), then range chips.
static const rl::Row* pd_row(const rl::Layout& l) { return find(l, "PD"); }
static std::string fixed_chips(const rl::Layout& l) {
    std::string out;
    for (size_t i = 0; i < l.row_count; ++i) {
        if (!l.rows[i].pd) continue;
        for (unsigned k = l.rows[i].first; k < unsigned(l.rows[i].first) + l.rows[i].count; ++k) {
            if (l.tokens[k].kind != 0) break;
            if (!out.empty()) out += ' ';
            out += l.tokens[k].text;
        }
    }
    return out;
}

// The grid row of one PPS / AVS APDO (the sweep's chip lives there), and its words.
// A range chip lives in the PD block now: a Row-shaped view of it (first = token index, count = chip plus its glued tags).
static const rl::Row* sweep_row_of(const rl::Layout& l, uint8_t pos, A a = A::Pps) {
    static rl::Row pool[16];
    static unsigned next = 0;
    for (size_t i = 0; i < l.row_count; ++i) {
        if (!l.rows[i].pd) continue;
        for (unsigned k = l.rows[i].first; k < unsigned(l.rows[i].first) + l.rows[i].count; ++k) {
            const rl::Token& t = l.tokens[k];
            if (t.kind != rl::kind_of(a) || t.mv != pos || t.glue) continue;
            rl::Row& r = pool[next++ % 16];
            r = l.rows[i]; r.first = uint16_t(k); r.pos = pos; r.algorithm = a; r.count = 1;
            while (k + r.count < unsigned(l.rows[i].first) + l.rows[i].count && l.tokens[k + r.count].glue) ++r.count;
            return &r;
        }
    }
    return nullptr;
}
static std::string sweep_chips(const rl::Layout& l, uint8_t pos, A a = A::Pps) {
    const rl::Row* r = sweep_row_of(l, pos, a);
    if (!r) return "(no row)";
    std::string out;
    for (unsigned k = r->first; k < unsigned(r->first) + r->count; ++k) { if (!out.empty()) out += ' '; out += l.tokens[k].text; }
    return out;
}
static size_t rows_labelled(const rl::Layout& l, const char* label) {
    size_t n = 0;
    for (size_t i = 0; i < l.row_count; ++i) n += std::string(l.rows[i].label) == label;
    return n;
}
static void test_fixed_pd_chips_carry_advertised_current() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    const size_t r = row_of(l, "PD");
    CHECK_STR(fixed_chips(l).c_str(), "5V 3A 9V 3A 12V 3A 15V 3A 20V 3.35A");
    CHECK_EQ(int(l.tokens[l.rows[r].first].tone), int(rl::Tone::Pass));
    CHECK_EQ(l.tokens[l.rows[r].first].chip, true);
}
static void test_failed_and_skipped_fixed_chips() {
    auto s = baseus(); s[1].verdict = V::Fail; s[2].verdict = V::Skip;
    rl::Layout l; rl::build(l, s);
    const auto& r = l.rows[row_of(l, "PD")];
    CHECK_EQ(int(l.tokens[r.first + 1].tone), int(rl::Tone::Fail));
    CHECK_EQ(int(l.tokens[r.first + 2].tone), int(rl::Tone::Skip));   // a skip is its own tone (grey dash), not "some passed"
}
// The plan runs the fixed rows from the top voltage down (2026-11-17); the grid still reads left to right, ascending, with a chip's
// reason tag glued behind it, and each chip keeps its own level (the Running view finds the step in flight by it).
static void test_fixed_chips_read_ascending_whatever_the_plan_order() {
    std::vector<ct::Step> s;
    const uint32_t fixed[] = {167874860u, 184620u, 246060u, 307500u, 409935u};
    const unsigned mv[] = {5000, 9000, 12000, 15000, 20000};
    for (int i = 4; i >= 0; --i) s.push_back(step(A::Pd3Fixed, mv[i], V::Pass, uint8_t(i + 1), fixed[i]));   // 20 V first, 5 V last
    s[0] = step(A::Pd3Fixed, 20000, V::Skip, 5, fixed[4], ct::Reason::CableLimit);   // a tagged chip travels with its voltage
    s[4].verdict = V::Running;
    rl::Layout l; rl::build(l, s);
    const size_t r = row_of(l, "PD");
    CHECK_STR(fixed_chips(l).c_str(), "5V 3A 9V 3A 12V 3A 15V 3A 20V 3.35A cable");
    CHECK_EQ(int(l.tokens[l.rows[r].first].mv), 5000);
    CHECK_EQ(int(l.tokens[l.rows[r].first].tone), int(rl::Tone::Pending));   // the 5 V row in flight is the first chip
    CHECK_EQ(int(l.tokens[l.rows[r].first + 4].mv), 20000);
    CHECK_EQ(int(l.tokens[l.rows[r].first + 4].tone), int(rl::Tone::Skip));
    CHECK_EQ(l.tokens[l.rows[r].first + 5].glue, true);
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
    const rl::Row* r6 = sweep_row_of(l, 6);               // one chip per APDO, slot 6 first, slot 7 right after
    const rl::Row* r7 = sweep_row_of(l, 7);
    CHECK_EQ(r6 != nullptr && r7 != nullptr, true);
    if (!r6 || !r7) return;
    CHECK_STR(sweep_chips(l, 6).c_str(), "5-11V 5A");
    CHECK_EQ(r6->count, 1);
    CHECK_EQ(l.tokens[r6->first].chip, true);
    CHECK_EQ(l.tokens[r6->first].kind, rl::kKindPps);
    CHECK_EQ(l.tokens[r6->first].mv, 6);                                    // a range chip's mv is its PDO slot
    CHECK_EQ(int(l.tokens[r6->first].tone), int(rl::Tone::Partial));        // a step failed and one skipped
    CHECK_STR(sweep_chips(l, 7).c_str(), "5-21V 3.2A");
    CHECK_EQ(int(l.tokens[r7->first].tone), int(rl::Tone::Pass));           // every step of the second PDO passed
    CHECK_EQ(r6->first < r7->first, true);
}
// ---- A deep sweep has no row of its own: its APDO's range chip carries it (an "S" mark, the worst of steps and sweep) ------
static ct::Step sweep_step(A a, uint8_t pos, uint32_t pdo, V v) {
    ct::Step s = step(a, 0, v, pos, pdo);
    s.detail = ct::kDetailSweep;
    s.avs = a == A::SprAvs || a == A::EprAvs;
    return s;
}
// The PPS sweep is no row of its own: its APDO's range chip ("5-21V 3.2A", the advertised range, as before) carries a small "S"
// (Token::sweep) and the worst verdict of the range steps and the sweep. Slot 6 ran no sweep: no S.
static void test_a_pps_sweep_merges_into_its_apdos_range_chip() {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pending));
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(rows_labelled(l, "Sweep"), 0u);                                        // no Sweep row
    CHECK_EQ(rows_labelled(l, "PPS"), 0u);                                          // the block's label is "PD"; the kind lives in the chip
    CHECK_EQ(rows_labelled(l, "PD"), 1u);
    const rl::Row* r = sweep_row_of(l, 7);
    CHECK_EQ(r != nullptr, true);
    if (!r) return;
    CHECK_EQ(r->count, 1);                                                          // one chip, not one per volt
    CHECK_STR(sweep_chips(l, 7).c_str(), "5-21V 3.2A");
    const rl::Token& k = l.tokens[r->first];
    CHECK_EQ(k.chip, true);
    CHECK_EQ(k.sweep, true);                                                        // the S mark
    CHECK_EQ(k.mv, 7);                                                              // a range chip's mv is its PDO slot (no per-level dot)
    CHECK_EQ(int(k.tone), int(rl::Tone::Pending));                                  // the sweep has not run: the Running dot sits here
    CHECK_EQ(l.tokens[sweep_row_of(l, 6)->first].sweep, false);                     // slot 6 ran none
}
// The AVS rows merge the same way: SPR AVS and EPR AVS, one chip, the S mark, no Sweep row.
static void test_an_avs_sweep_merges_into_its_apdos_range_chip() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    ct::Step range = step(A::SprAvs, 9000, V::Pass, 3, 0xE004B145u); range.avs = true;
    s.push_back(range);
    s.push_back(sweep_step(A::SprAvs, 3, 0xE004B145u, V::Pass));
    ct::Step erange = step(A::EprAvs, 15000, V::Pass, 9, 0xE004B145u); erange.avs = true;
    s.push_back(erange);
    s.push_back(sweep_step(A::EprAvs, 9, 0xE004B145u, V::Fail));
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(rows_labelled(l, "Sweep"), 0u);
    CHECK_EQ(rows_labelled(l, "SPR AVS"), 0u);                                      // the kind word is in the chip, not the label
    CHECK_EQ(rows_labelled(l, "EPR AVS"), 0u);
    CHECK_EQ(rows_labelled(l, "PD"), 1u);
    const rl::Row* sp = sweep_row_of(l, 3, A::SprAvs);
    const rl::Row* ep = sweep_row_of(l, 9, A::EprAvs);
    CHECK_EQ(sp != nullptr && ep != nullptr, true);
    if (!sp || !ep) return;
    CHECK_EQ(sp->count, 1);
    CHECK_EQ(l.tokens[sp->first].sweep, true);
    CHECK_EQ(int(l.tokens[sp->first].tone), int(rl::Tone::Pass));
    CHECK_EQ(l.tokens[ep->first].sweep, true);
    CHECK_EQ(int(l.tokens[ep->first].tone), int(rl::Tone::Fail));                   // the failed sweep wins over the passing step
    CHECK_EQ(sweep_chips(l, 3, A::SprAvs).compare(0, 5, "9-20V"), 0);
}
// Default run (2026-11-14): every PPS APDO now gets its sweep, split at 5 V into two steps (the part from 5 V up after the APDO's
// checks, the part below at the end of the plan, PdLow). Three 3.3 V APDOs at once, with the widest current words a real APDO
// gives (4.75 A), build without a single cut: ONE row per APDO owns the range steps and both sweep parts, its chip names the range.
static void test_default_sweep_rows_fit_without_a_cut() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    const uint32_t apdo[] = {0xC0000000u | (110u << 17) | (33u << 8) | 100u,    // 3.3-11 V 5 A
                             0xC0000000u | (160u << 17) | (33u << 8) | 95u,     // 3.3-16 V 4.75 A
                             0xC0000000u | (210u << 17) | (33u << 8) | 95u};    // 3.3-21 V 4.75 A
    for (unsigned k = 0; k < 3; ++k) {
        const uint8_t pos = uint8_t(5 + k);
        s.push_back(step(A::Pps, 3300, V::Pass, pos, apdo[k]));
        s.push_back(step(A::Pps, 21000, V::Pass, pos, apdo[k]));
        s.push_back(sweep_step(A::Pps, pos, apdo[k], V::Pass));
        s.back().mv = 5000;   // the part from 5 V up
    }
    for (unsigned k = 0; k < 3; ++k) {   // the parts below 5 V: the PdLow tail of the plan
        s.push_back(sweep_step(A::Pps, uint8_t(5 + k), apdo[k], V::Pending));
        s.back().mv = 3300;
    }
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(l.cuts, 0);
    for (unsigned k = 0; k < 3; ++k) {
        const uint8_t pos = uint8_t(5 + k);
        const rl::Row* r = sweep_row_of(l, pos);
        CHECK_EQ(r != nullptr, true);
        if (!r) continue;
        CHECK_EQ(r->count, 1);                              // one chip for both parts
        CHECK_EQ(l.tokens[r->first].sweep, true);
        CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Pending));   // the low part is still to run
        const std::string chip = sweep_chips(l, pos);
        CHECK_EQ(chip.compare(0, 4, "3.3-"), 0);
        CHECK_EQ(chip.find("..."), std::string::npos);
        std::printf("[sweep-chip] pos %u: %s\n", unsigned(pos), chip.c_str());
    }
    CHECK_EQ(rows_labelled(l, "Sweep"), 0u);
}
// The sweep's part of the chip's colour: pending until it ran, nothing (the range steps decide) when skipped, red when the source
// was lost, and green / amber by the worst error of the recorded points (<= 100 mV green, above amber). Slot 7's range steps all
// pass, so the chip reads the sweep alone. A PASSING sweep is never red: red is
// a failed step (user decision 2026-11-16: the chip never contradicts the step's verdict, so the header cannot say "0 fail" over
// a red cell).
static void test_a_sweep_chip_takes_its_tone_from_the_sweep() {
    const auto tone_of = [](V v, std::vector<int16_t> pts) {
        auto s = baseus();
        s.push_back(sweep_step(A::Pps, 7, 3248763456u, v));
        ctsweep::RowView rw; rw.pos = 7; rw.alg = uint8_t(A::Pps); rw.lo_mv = 5000; rw.hi_mv = 21000; rw.n = uint16_t(pts.size());
        rw.sum = ctsweep::summarize(pts, 5000);   // the runner closes every row with its summary
        ctsweep::View view; view.rows = std::span<const ctsweep::RowView>(&rw, 1); view.points = pts;
        rl::Layout l; rl::build(l, s, {}, view);
        const rl::Row* r = sweep_row_of(l, 7);
        return r ? int(l.tokens[r->first].tone) : -1;
    };
    CHECK_EQ(tone_of(V::Pending, {}), int(rl::Tone::Pending));
    CHECK_EQ(tone_of(V::Running, {-20, 30}), int(rl::Tone::Pending));    // in flight: Pending keeps the Running dot on the chip
    CHECK_EQ(tone_of(V::Skip, {}), int(rl::Tone::Pass));                 // a skipped sweep adds nothing: the passing steps stand
    CHECK_EQ(tone_of(V::Pass, {-20, 30, -90}), int(rl::Tone::Pass));
    CHECK_EQ(tone_of(V::Pass, {-20, -250, 30}), int(rl::Tone::Partial));
    CHECK_EQ(tone_of(V::Pass, {-20, 450, 30}), int(rl::Tone::Partial));  // a passing sweep is never red, however far off
    CHECK_EQ(tone_of(V::Pass, {-20, -1500, 30}), int(rl::Tone::Partial));
    CHECK_EQ(tone_of(V::Fail, {-20, 30}), int(rl::Tone::Fail));          // a lost source, the points so far notwithstanding
}
// A voltage fold is a caution whatever its depth (user decision 2026-11-16): a sweep whose only deviation is the fold region reads
// amber, never red, so the chip never contradicts a header that says 0 fail. The chip is graded by the tracking error BEFORE the
// fold onset (ctsweep::Summary::track_err_mv): the points after the first fold are the fold, not tracking. A real tracking failure
// before it is amber as well: no passing sweep is red (the verdict of the step is Pass; the chip never contradicts it).
static int fold_chip_tone(std::vector<int16_t> pts, ct::Reason reason, unsigned want_fold_from_mv, std::vector<ct::Step>* run = nullptr) {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass));
    s.back().reason = reason;
    ctsweep::RowView rw; rw.pos = 7; rw.alg = uint8_t(A::Pps); rw.lo_mv = 5000; rw.hi_mv = 21000; rw.n = uint16_t(pts.size());
    rw.sum = ctsweep::summarize(pts, 5000);
    CHECK_EQ(unsigned(rw.sum.fold_from_mv), want_fold_from_mv);   // the vector means what the test says it means
    ctsweep::View view; view.rows = std::span<const ctsweep::RowView>(&rw, 1); view.points = pts;
    rl::Layout l; rl::build(l, s, {}, view);
    if (run) *run = s;
    const rl::Row* r = sweep_row_of(l, 7);
    return r ? int(l.tokens[r->first].tone) : -1;
}
static void test_a_fold_only_sweep_is_amber_at_any_depth() {
    const auto F = ct::Reason::Folded;
    const int amber = int(rl::Tone::Partial);
    // The fold alone: the output tracks to 5.3 V, then falls 400 mV (and stays) - a caution, not a failure.
    CHECK_EQ(fold_chip_tone({0, 0, 0, 0, -400, -400, -400}, F, 5400), amber);
    // The FH22 shape: tracks within 100 mV, folds 1.5 V under the request from the first folded point on.
    CHECK_EQ(fold_chip_tone({-20, -30, -20, -1200, -1500, -1600}, F, 5300), amber);
    // A mild fold stays amber as before (the caution is never green).
    CHECK_EQ(fold_chip_tone({0, 0, 0, -200, -200}, F, 5300), amber);
    // Tracking error before the fold is amber too: the step passed, so the chip is never red (the old rule graded it red).
    CHECK_EQ(fold_chip_tone({-400, -400, -400, -400, -900, -900}, F, 5400), amber);
    CHECK_EQ(fold_chip_tone({-250, -250, -250, -900, -900}, F, 5300), amber);   // 100..300 mV before the fold: amber, as ever
    // Without the caution the chip is graded by every recorded point (a sweep that never got a Folded reason): amber above 100 mV
    // (a deviation caution), never red on a Pass step.
    CHECK_EQ(fold_chip_tone({0, 0, 0, 0, -400, -400, -400}, ct::Reason::None, 5400), amber);
    CHECK_EQ(fold_chip_tone({-400, -400, -400, -400}, ct::Reason::None, 0), amber);   // no fold at all: a steady 400 mV offset
    CHECK_EQ(fold_chip_tone({-20, 450, 450, 450}, ct::Reason::None, 0), amber);        // a steady 450 mV excess
    CHECK_EQ(fold_chip_tone({-20, 30, -90}, ct::Reason::None, 0), int(rl::Tone::Pass));   // within 100 mV: green
    // The run says the same everywhere: no failed step, the fold step counted as a caution, so the chip may not be red.
    std::vector<ct::Step> run;
    CHECK_EQ(fold_chip_tone({0, 0, 0, 0, -400, -400, -400}, F, 5400, &run), amber);
    rl::Tally t;
    for (const auto& y : run) if (ct::is_sweep(y)) (rl::caution(y) ? ++t.capped : y.verdict == V::Fail ? ++t.fail : ++t.pass);
    CHECK_EQ(t.fail, 0u);
    CHECK_EQ(t.capped, 1u);
}
// User decision 2026-11-18: a step-walk miss (a Pass that carries Reason::StepResolution) is a caution on the APDO's range chip, never a
// chip of its own and never red. A journal from before (the same reason on a Fail) keeps its verdict's colour.
static void test_a_step_walk_miss_is_an_amber_caution_on_the_range_chip() {
    auto clean = baseus();
    rl::Layout l0; rl::build(l0, clean);
    const rl::Row* r0 = sweep_row_of(l0, 7);
    CHECK_EQ(r0 != nullptr, true);
    if (!r0) return;
    CHECK_EQ(int(l0.tokens[r0->first].tone), int(rl::Tone::Pass));
    auto s = clean;
    ct::Step w = step(A::Pps, 13000, V::Pass, 7, 3248763456u, ct::Reason::StepResolution);   // the walk row: a range step of slot 7
    w.detail = 100;
    s.push_back(w);
    CHECK_EQ(rl::caution(w), true);
    CHECK_STR(rl::verdict_word(w), "CAUTION");
    rl::Layout l; rl::build(l, s);
    const rl::Row* r = sweep_row_of(l, 7);
    CHECK_EQ(r != nullptr, true);
    if (!r) return;
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Partial));      // amber: the range chip carries it
    CHECK_EQ(l.token_count, l0.token_count);                             // no chip of its own
    ct::Step old = w; old.verdict = V::Fail;                              // an older journal: the Fail keeps its words
    CHECK_EQ(rl::caution(old), false);
    CHECK_STR(rl::verdict_word(old), "FAIL");
    rl::Tally t; for (const auto& y : s) if (y.pos == 7 && y.algorithm == A::Pps) (rl::caution(y) ? ++t.capped : y.verdict == V::Fail ? ++t.fail : ++t.pass);
    CHECK_EQ(t.capped, 1u); CHECK_EQ(t.fail, 0u);
}
// A Deep transition check that broke a spec limit (a Pass that carries Reason::ProgCheck on a level row) is the same caution: the range
// chip goes amber, no chip of its own, and the footer must not read it as a fold ("folds to ...").
static void test_a_transition_check_flag_is_an_amber_caution_on_the_range_chip() {
    auto s = baseus();
    rl::Layout l0; rl::build(l0, s);
    const rl::Row* r0 = sweep_row_of(l0, 7);
    CHECK_EQ(r0 != nullptr, true);
    if (!r0) return;
    size_t at = s.size();
    for (size_t i = 0; i < s.size(); ++i) if (s[i].algorithm == A::Pps && s[i].pos == 7 && s[i].mv == 13000 && !ct::is_sweep(s[i])) at = i;
    CHECK_EQ(at < s.size(), true);
    if (at == s.size()) return;
    s[at].reason = ct::Reason::ProgCheck;
    s[at].detail = 1;
    CHECK_EQ(rl::caution(s[at]), true);
    CHECK_STR(rl::verdict_word(s[at]), "CAUTION");
    rl::Layout l; rl::build(l, s);
    const rl::Row* r = sweep_row_of(l, 7);
    CHECK_EQ(r != nullptr, true);
    if (!r) return;
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Partial));
    CHECK_EQ(l.token_count, l0.token_count);
    const auto rc = rl::reach(s, A::Pps, 7);
    CHECK_EQ(rc.folded, false);   // not a fold: the footer stays on the reach words
    CHECK_EQ(int(rc.tone), int(rl::Tone::Partial));
    const auto* c = rl::step_caution(s, A::Pps, 7);
    CHECK_EQ(c != nullptr, true);
    if (c) { CHECK_EQ(int(c->reason), int(ct::Reason::ProgCheck)); }
    s[at].verdict = V::Fail;   // only a pass can be a caution
    CHECK_EQ(rl::caution(s[at]), false);
}
// A voltage fold the sweep found is a CAUTION: neither a pass nor a fail (User decision 2026-11-16, FH22). The sweep row and the fixed rows
// of the APDO that sit on the folded side keep their Pass verdict and carry Reason::Folded; every surface reads them amber, the
// band says CAUTION, and the tally counts them in its own (capped) column - never pass, never fail.
static void test_a_fold_is_a_caution_not_a_pass_and_not_a_fail() {
    auto s = baseus();
    for (size_t i = 0; i < s.size(); ++i) if (s[i].algorithm == A::Pps && s[i].pos == 7 && s[i].mv == 21000) s[i].reason = ct::Reason::Folded, s[i].measured_mv = 19428;
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass));
    s.back().reason = ct::Reason::Folded;
    CHECK_STR(rl::verdict_word(s.back()), "CAUTION");
    CHECK_EQ(rl::caution(s.back()), true);
    CHECK_EQ(rl::caution(step(A::Pps, 13000, V::Pass, 7)), false);
    CHECK_EQ(rl::caution(step(A::Pps, 13000, V::Fail, 7, 0, ct::Reason::Folded)), false);   // only a pass can be a caution
    rl::Layout l; rl::build(l, s);
    const rl::Row* r = sweep_row_of(l, 7);
    CHECK_EQ(r != nullptr, true);
    if (!r) return;
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Partial));                   // amber (tilde), not green and not red
    CHECK_EQ(int(rl::mark(l.tokens[r->first].tone)), int(rl::mark(rl::Tone::Partial)));
    // the APDO's range row (slot 7): some levels passed, one is a caution -> amber; slot 6 has no fold
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Partial));
    CHECK_EQ(l.tokens[r->first].sweep, true);
    // the tally never calls a caution a pass or a fail
    auto reach = rl::reach(s, A::Pps, 7);
    CHECK_EQ(reach.folded, true);
    CHECK_EQ(reach.fold_mv, 19428);
    CHECK_EQ(int(reach.tone), int(rl::Tone::Partial));
    CHECK_EQ(reach.capped, false);                                                     // not a cap: nothing failed
    CHECK_STR(rl::reach_footer(reach).c_str(), "folds to 19.4V");
    CHECK_EQ(rl::reach_footer(reach).len <= 18, true);
    // a clean slot reads as it did
    CHECK_EQ(rl::reach(s, A::Pps, 6).folded, false);
    // a caution rolled with passes is amber; a caution next to a failure never hides the failure
    CHECK_EQ(int(rl::roll_up(rl::Tally{.pass = 3, .capped = 1})), int(rl::Tone::Partial));
    CHECK_EQ(int(rl::roll_up(rl::Tally{.fail = 1, .capped = 1})), int(rl::Tone::Partial));
    // a fold on a sweep that otherwise tracked clean still reads amber; without any caution the same APDO is green
    s.back().reason = ct::Reason::None;
    for (auto& y : s) if (y.reason == ct::Reason::Folded) y.reason = ct::Reason::None;
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Pass));
}
// A passing sweep whose worst error passes 100 mV is a CAUTION by the same rule as a fold (user decision 2026-11-16): the step keeps
// its Pass verdict, the sweep view says it deviates, every count and word reads it as a caution (amber, "CAUTION"), the chip is amber
// and never red, so a header that counts "0 fail" never sits over a red cell. A fail stays a fail, a clean pass stays a pass.
static ctsweep::RowView sweep_rowview(uint8_t pos, unsigned lo_mv, unsigned hi_mv, std::vector<int16_t>& pts, unsigned first) {
    ctsweep::RowView rw; rw.pos = pos; rw.alg = uint8_t(A::Pps); rw.lo_mv = uint16_t(lo_mv); rw.hi_mv = uint16_t(hi_mv);
    rw.first = uint16_t(first); rw.n = uint16_t(pts.size());
    rw.sum = ctsweep::summarize(pts, lo_mv);
    return rw;
}
static void test_a_deviating_sweep_is_a_caution_never_red() {
    const auto want = [](std::vector<int16_t> pts, V v, bool caution, const char* word) {
        auto s = baseus();
        s.push_back(sweep_step(A::Pps, 7, 3248763456u, v));
        s.back().mv = 5000;
        auto rw = sweep_rowview(7, 5000, 21000, pts, 0);
        ctsweep::View view; view.rows = std::span<const ctsweep::RowView>(&rw, 1); view.points = pts;
        CHECK_EQ(rl::caution(s.back(), view), caution);
        CHECK_EQ(rl::deviates(s.back(), view), caution);
        CHECK_STR(rl::verdict_word(s.back(), view), word);
        CHECK_EQ(rl::caution(s.back()), false);                       // the step alone carries no reason: the view decides
        CHECK_EQ(rl::caution(s[0], view), false);                     // a fixed row is never a deviating sweep
        rl::Layout l; rl::build(l, s, {}, view);
        const rl::Row* r = sweep_row_of(l, 7);
        CHECK_EQ(r != nullptr, true);
        if (!r) return;
        const rl::Tone tn = l.tokens[r->first].tone;
        const rl::Tone expect = v == V::Fail ? rl::Tone::Fail : v == V::Running ? rl::Tone::Pending
                              : caution ? rl::Tone::Partial : rl::Tone::Pass;   // a skipped sweep leaves the passing steps' Pass
        CHECK_EQ(int(tn), int(expect));
        // the tally the strip's footer and the header use: a caution is counted apart, red appears only with a failed step
        rl::Tally t;
        for (const auto& y : s) if (ct::is_sweep(y)) (rl::caution(y, view) ? ++t.capped : y.verdict == V::Fail ? ++t.fail : ++t.pass);
        CHECK_EQ(t.fail, v == V::Fail ? 1u : 0u);
        CHECK_EQ(t.capped, caution ? 1u : 0u);
    };
    want({-20, 30, -90}, V::Pass, false, "PASS");           // within 100 mV
    want({-20, 100, 30}, V::Pass, false, "PASS");           // exactly 100 mV is still inside
    want({-20, -250, 30}, V::Pass, true, "CAUTION");        // 100..300 mV
    want({-20, 450, 30}, V::Pass, true, "CAUTION");         // above 300 mV: still a caution, not a fail (the old chip was red here)
    want({-20, -1500, 30}, V::Pass, true, "CAUTION");
    want({-20, 450, 30}, V::Fail, false, "FAIL");           // a failed step is no caution; it stays red
    want({-20, 450, 30}, V::Running, false, "RUNNING");
    want({-20, 450, 30}, V::Skip, false, "SKIP");
}
// A split sweep (the part from 5 V up and the part below, two steps in one strip row): the parts share one APDO verdict. Either
// part's deviation makes the APDO's chip amber; a failed part keeps the chip red; and each Pass part reads as a caution when the
// APDO deviates, so the counts agree with the one chip.
static void test_a_split_sweep_deviation_reads_the_same_on_both_parts() {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass)); s.back().mv = 5000;   // the part from 5 V up
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass)); s.back().mv = 3300;   // the part below
    std::vector<int16_t> hi = {-20, 30, -40}, lo = {10, -350};   // the deviation sits in the low part
    std::vector<int16_t> all = hi; all.insert(all.end(), lo.begin(), lo.end());
    ctsweep::RowView rws[2] = {sweep_rowview(7, 5000, 21000, hi, 0), sweep_rowview(7, 3300, 4900, lo, 3)};
    ctsweep::View view; view.rows = std::span<const ctsweep::RowView>(rws, 2); view.points = all;
    const size_t n = s.size();
    CHECK_EQ(rl::caution(s[n - 2], view), true);    // the high part is clean itself, but the chip is one APDO's: amber
    CHECK_EQ(rl::caution(s[n - 1], view), true);
    rl::Layout l; rl::build(l, s, {}, view);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Partial));
    s[n - 1].verdict = V::Fail;                      // the low part failed: red, and the clean-looking high part is a caution only
    rl::build(l, s, {}, view);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Fail));
    CHECK_EQ(rl::caution(s[n - 1], view), false);
}
// The chip is the WORST of the APDO's range steps and its sweep (fail > partial / caution > pass; pending while anything is
// pending; skip only when every part skipped). Slot 6 of the fixture has a failed and a skipped step among passes (Partial).
static void test_the_merged_chip_is_the_worst_of_its_steps_and_its_sweep() {
    const auto tone6 = [](V sweep_verdict, ct::Reason sweep_reason = ct::Reason::None) {
        auto s = baseus();
        s.push_back(sweep_step(A::Pps, 6, 3235656292u, sweep_verdict));
        s.back().reason = sweep_reason;
        rl::Layout l; rl::build(l, s);
        return int(l.tokens[sweep_row_of(l, 6)->first].tone);
    };
    const auto tone7 = [](V sweep_verdict, ct::Reason sweep_reason = ct::Reason::None) {
        auto s = baseus();
        s.push_back(sweep_step(A::Pps, 7, 3248763456u, sweep_verdict));
        s.back().reason = sweep_reason;
        rl::Layout l; rl::build(l, s);
        return int(l.tokens[sweep_row_of(l, 7)->first].tone);
    };
    // slot 6: the steps alone read Partial (a failed and a skipped step beside the passes)
    CHECK_EQ(tone6(V::Pass), int(rl::Tone::Partial));                       // a clean sweep does not lift a partial APDO
    CHECK_EQ(tone6(V::Fail), int(rl::Tone::Fail));                          // a lost source is worse than a partial range
    CHECK_EQ(tone6(V::Pass, ct::Reason::Folded), int(rl::Tone::Partial));
    CHECK_EQ(tone6(V::Running), int(rl::Tone::Pending));
    // slot 7: every step passed
    CHECK_EQ(tone7(V::Pass), int(rl::Tone::Pass));
    CHECK_EQ(tone7(V::Pass, ct::Reason::Folded), int(rl::Tone::Partial));   // a fold is a caution
    CHECK_EQ(tone7(V::Fail), int(rl::Tone::Fail));
    CHECK_EQ(tone7(V::Pending), int(rl::Tone::Pending));
    // the range steps worse than the sweep: a clean sweep never hides a failed APDO
    std::vector<ct::Step> s;
    for (unsigned t : {5000u, 8000u}) s.push_back(step(A::Pps, t, V::Fail, 6, 3235656292u));
    s.push_back(sweep_step(A::Pps, 6, 3235656292u, V::Pass));
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 6)->first].tone), int(rl::Tone::Fail));
    // a capped step (the source plateaued) is a partial, the clean sweep leaves it so
    s.clear();
    s.push_back(step(A::Pps, 5000, V::Pass, 6, 3235656292u));
    s.push_back(step(A::Pps, 11000, V::Fail, 6, 3235656292u, ct::Reason::Capped));
    s.push_back(sweep_step(A::Pps, 6, 3235656292u, V::Pass));
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 6)->first].tone), int(rl::Tone::Partial));
    // both parts of a split sweep count: one failed part fails the chip
    s.clear();
    s.push_back(step(A::Pps, 5000, V::Pass, 6, 3235656292u));
    s.push_back(sweep_step(A::Pps, 6, 3235656292u, V::Pass)); s.back().mv = 5000;
    s.push_back(sweep_step(A::Pps, 6, 3235656292u, V::Fail)); s.back().mv = 3300;
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 6)->first].tone), int(rl::Tone::Fail));
}
// A sweep that never ran (every part skipped) neither marks the chip nor lowers it; the chip is Skip only when the range steps
// are skipped too, and then it carries the skip tag as before.
static void test_a_skipped_sweep_leaves_no_mark_and_no_verdict() {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Skip));
    s.back().reason = ct::Reason::CableLimit;
    rl::Layout l; rl::build(l, s);
    const rl::Token& k = l.tokens[sweep_row_of(l, 7)->first];
    CHECK_EQ(k.sweep, false);
    CHECK_EQ(int(k.tone), int(rl::Tone::Pass));
    // everything skipped (range and sweep): grey, no mark, the reason beside the chip
    s.clear();
    s.push_back(step(A::Pps, 5000, V::Skip, 7, 3248763456u, ct::Reason::CableLimit));
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Skip));
    s.back().reason = ct::Reason::CableLimit;
    rl::build(l, s);
    const rl::Row* r = sweep_row_of(l, 7);
    CHECK_EQ(r != nullptr, true);
    if (!r) return;
    CHECK_EQ(int(l.tokens[r->first].tone), int(rl::Tone::Skip));
    CHECK_EQ(l.tokens[r->first].sweep, false);
    CHECK_STR(sweep_chips(l, 7).c_str(), "5-21V 3.2A cable");
    CHECK_EQ(l.cuts, 0);
}
// Opening the PD block lists every PD step: fixed levels, both APDOs' range steps and their sweep parts, in ONE Steps page; no other
// row owns a PD step, and the block owns no step of another family.
static void test_the_pd_block_owns_every_pd_step_and_the_sweeps() {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass)); s.back().mv = 5000;
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass)); s.back().mv = 3300;
    const ct::Step& hi = s[s.size() - 2];
    const ct::Step& lo = s.back();
    rl::Layout l; rl::build(l, s);
    const rl::Row& blk = l.rows[row_of(l, "PD")];
    CHECK_EQ(rl::row_has(blk, hi), true);
    CHECK_EQ(rl::row_has(blk, lo), true);
    CHECK_EQ(rl::row_has(blk, step(A::Pps, 13000, V::Pass, 7)), true);
    CHECK_EQ(rl::row_has(blk, s[0]), true);                                         // a fixed step too
    CHECK_EQ(rl::row_has(blk, step(A::Qc2, 5000, V::Pass)), false);
    size_t owners = 0;
    for (size_t i = 0; i < l.row_count; ++i) owners += rl::row_has(l.rows[i], hi);
    CHECK_EQ(owners, 1u);                                                           // exactly one row per sweep step
    size_t listed = 0;
    for (const auto& x : s) listed += rl::row_has(blk, x);
    CHECK_EQ(listed, size_t(std::count_if(s.begin(), s.end(), [](const ct::Step& x) { return x.group == ct::Group::Pd; })));
    // the same through the compact grid the page keeps
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    rl::build_compact(g, rl::kGridMetrics, lines, s);
    size_t cowners = 0;
    for (size_t i = 0; i < g.row_count; ++i) {
        cowners += rl::row_has(g.rows[i], hi);
    }
    CHECK_EQ(cowners, 1u);
}
// The compact grid regenerates a row's tokens on demand (row_tokens): the merged chip keeps its mark and verdict there, and the
// label of a range row never reads "Sweep".
static void test_a_compact_row_regenerates_the_merged_chip_with_its_mark() {
    auto s = baseus();
    s.push_back(sweep_step(A::Pps, 7, 3248763456u, V::Pass));
    s.back().reason = ct::Reason::Folded;
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    rl::build_compact(g, rl::kGridMetrics, lines, s);
    int found = 0;
    for (size_t r = 0; r < g.row_count; ++r) {
        CHECK_EQ(std::string(rl::row_label(g, r)) == "Sweep", false);
        if (g.rows[r].kind != rl::CompactKind::Pd) continue;
        rl::RowTokens rt;
        rl::row_tokens(g, r, s, {}, rt);
        for (unsigned i = 0; i < rt.count; ++i) {
            if (rt.toks[i].kind != rl::kKindPps || rt.toks[i].mv != 7) continue;
            ++found;
            CHECK_STR(rt.toks[i].text, "5-21V 3.2A");
            CHECK_EQ(rt.toks[i].sweep, true);
            CHECK_EQ(int(rt.toks[i].tone), int(rl::Tone::Partial));
        }
    }
    CHECK_EQ(found, 1);
    CHECK_EQ(g.cuts, 0);
}
// The mark keeps m.sweep (12 px) beside the volts (the kit draws a 6 px "S" a fixed 6 px right of the text), so the grid cell
// and the chip width grow by that; a chip without the mark keeps its width.
static void test_the_sweep_mark_widens_the_chip_by_its_gap_and_glyph() {
    rl::Token t; std::strcpy(t.text, "5-21V 3.2A"); t.chip = true;
    rl::Token u = t; u.sweep = true;
    const rl::Metrics m = rl::kGridMetrics;
    CHECK_EQ(m.sweep, 12);
    CHECK_EQ(rl::cell_width(u, m), rl::cell_width(t, m) + m.sweep);
    CHECK_EQ(rl::token_width(u, m), rl::token_width(t, m) + m.sweep);
    CHECK_EQ(rl::cell_width(t, m), 5 * m.adv + 2 * m.pad);                          // the volts only: "5-21V"
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
// ---- A Light run lists what it did not ask for: "advertised" / "not asked" steps read Noted, never pass or struck -------------
static void test_light_run_chips_read_noted() {
    using T = rl::Tally;
    CHECK_EQ(int(rl::roll_up(T{.noted = 2})), int(rl::Tone::Noted));             // only listed: its own tone
    CHECK_EQ(int(rl::roll_up(T{.pass = 1, .noted = 2})), int(rl::Tone::Pass));     // a measured level beside listed ones
    CHECK_EQ(int(rl::roll_up(T{.skip = 1, .noted = 1})), int(rl::Tone::Skip));
    CHECK_EQ(int(rl::roll_up(T{.noted = 1, .pending = 1})), int(rl::Tone::Pending));
    CHECK_EQ(rl::mark(rl::Tone::Noted) != rl::mark(rl::Tone::Pass) && rl::mark(rl::Tone::Noted) != rl::mark(rl::Tone::Skip) &&
             rl::mark(rl::Tone::Noted) != rl::mark(rl::Tone::Dim), true);          // a shape of its own
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    s.push_back(step(A::Pd3Fixed, 9000, V::Skip, 2, 184620u, ct::Reason::Advertised));
    s.push_back(step(A::Pps, 11000, V::Skip, 6, 3235656292u, ct::Reason::Advertised));
    s.push_back(step(A::Qc2, 5000, V::Pass));
    for (unsigned mv : {9000u, 12000u}) s.push_back(step(A::Qc2, mv, V::Skip, 0, 0, ct::Reason::LightLevel));
    rl::Layout l; rl::build(l, s);
    const auto& pd = l.rows[row_of(l, "PD")];
    CHECK_EQ(int(l.tokens[pd.first].tone), int(rl::Tone::Pass));        // 5 V was asked and measured
    CHECK_EQ(int(l.tokens[pd.first + 1].tone), int(rl::Tone::Pass));    // 9 V is advertised by the source: the pass look, no request went out
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 6)->first].tone), int(rl::Tone::Pass));    // an advertised range reads green too
    const auto& qc = l.rows[row_of(l, "QC2")];
    CHECK_STR(chips(l, row_of(l, "QC2")).c_str(), "5V 9V 12V");
    CHECK_EQ(int(l.tokens[qc.first].tone), int(rl::Tone::Pass));
    CHECK_EQ(int(l.tokens[qc.first + 1].tone), int(rl::Tone::Noted));
    CHECK_EQ(int(l.tokens[qc.first + 2].tone), int(rl::Tone::Noted));
    // The Step detail band names a listing for what it is, never SKIP: LISTED (the source advertised it), NOT RUN (a Light
    // run did not ask it); a skip for any other reason stays SKIP.
    CHECK_STR(rl::verdict_word(s[1]), "LISTED");
    CHECK_STR(rl::verdict_word(s[4]), "NOT RUN");
    CHECK_STR(rl::verdict_word(s[0]), "PASS");
    CHECK_STR(rl::verdict_word(step(A::Pd3Fixed, 9000, V::Skip, 2, 184620u, ct::Reason::CableLimit)), "SKIP");
    CHECK_STR(rl::verdict_word(step(A::Pps, 11000, V::Fail, 6, 3235656292u, ct::Reason::Capped)), "CAPPED");
    CHECK_STR(rl::verdict_word(step(A::Pps, 11000, V::Fail, 6, 3235656292u, ct::Reason::Timeout)), "FAIL");
    CHECK_EQ(rl::noted(s[1]) && rl::noted(s[4]) && !rl::noted(s[0]), true);
    // A PD offer the source advertises is green (advertised()); a legacy level nothing advertises, and a non-PD listing (a UFCS mode,
    // an SCP level), stay Noted: the green is the PD family's "we have it", never a measured pass.
    CHECK_EQ(rl::advertised(s[1]) && rl::advertised(s[2]) && !rl::advertised(s[0]) && !rl::advertised(s[4]), true);
    const auto ufcs = step(A::Ufcs, 9000, V::Skip, 0, 0, ct::Reason::Advertised);
    CHECK_EQ(rl::advertised(ufcs), false);
    CHECK_EQ(rl::noted(ufcs), true);
    rl::Layout lu; rl::build(lu, std::vector<ct::Step>{step(A::Ufcs, 5000, V::Pass), ufcs});
    CHECK_EQ(int(lu.tokens[lu.rows[row_of(lu, "UFCS")].first + 1].tone), int(rl::Tone::Noted));
    // a step the port's own 5 V cut spoiled twice is a plain skip: no verdict, but never "listed" and never a pass or fail
    const auto cut = step(A::Qc2, 5000, V::Skip, 0, 0, ct::Reason::PortCuts);
    CHECK_STR(rl::verdict_word(cut), "SKIP");
    CHECK_EQ(rl::noted(cut), false);
}
static void test_a_capped_range_chip_is_amber_and_keeps_the_advertised_range() {
    auto s = fh22();
    rl::Layout l; rl::build(l, s);
    CHECK_STR(sweep_chips(l, 7).c_str(), "5-21V 3.2A");
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Partial));
    // the same chip without the cap reads green
    s[1].verdict = V::Pass; s[1].reason = ct::Reason::None;
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Pass));
}
static void test_a_range_with_nothing_passed_stays_red_and_all_skipped_grey() {
    auto s = fh22();
    for (auto& x : s) { x.verdict = V::Fail; x.reason = ct::Reason::Timeout; x.measured_mv = 0; }
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Fail));
    for (auto& x : s) { x.verdict = V::Skip; x.reason = ct::Reason::Request; }
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Skip));
    // a capped step with nothing else passed is still amber: the source delivered something below the target
    s = fh22();
    for (auto& x : s) if (x.reason != ct::Reason::Capped) { x.verdict = V::Fail; x.reason = ct::Reason::Timeout; }
    rl::build(l, s);
    CHECK_EQ(int(l.tokens[sweep_row_of(l, 7)->first].tone), int(rl::Tone::Partial));
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
// ROADMAP "PPS coverage" item 1: a low edge the source cannot reach (it advertises 3.3 V, regulates at 5 V) is a
// Floored fail, and the Results footer names the floor ("floor at 5V"), like "caps 19.4V" names a ceiling.
// A row's footer (Results and its Steps page): "<row label>: n pass, m caution". The non-zero counts in the order pass, fail, caution,
// listed, skip; a Light run's listing reads "listed". The label goes when a pass, fail or caution count would not fit beside it;
// a listed or skip count that does not fit is left off and the label stays.
static void test_family_footer_words() {
    rl::FamilyTally t;
    t.pass = 3; t.caution = 1;
    CHECK_STR(rl::family_footer("PPS", t).c_str(), "PPS: 3 pass, 1 caution");
    t = {}; t.pass = 4;
    CHECK_STR(rl::family_footer("PPS", t).c_str(), "PPS: 4 pass");
    t = {}; t.pass = 2; t.fail = 1; t.caution = 1;
    CHECK_STR(rl::family_footer("PPS", t).c_str(), "2 pass, 1 fail, 1 caution");   // 30 cells with the label: the label goes
    t = {}; t.pass = 20; t.fail = 3; t.caution = 4; t.skip = 5;
    CHECK_STR(rl::family_footer("QC3", t).c_str(), "20 pass, 3 fail, 4 caution");   // the skip count would pass the band: the line ends there
    t = {}; t.pass = 2; t.fail = 3; t.skip = 5;
    CHECK_STR(rl::family_footer("QC3", t).c_str(), "QC3: 2 pass, 3 fail, 5 skip");
    t = {}; t.fail = 2; t.caution = 1;
    CHECK_STR(rl::family_footer("SPR AVS", t).c_str(), "SPR AVS: 2 fail, 1 caution");
    t = {}; t.listed = 5;
    CHECK_STR(rl::family_footer("PD", t).c_str(), "PD: 5 listed");
    t = {}; t.pass = 2; t.skip = 3;
    CHECK_STR(rl::family_footer("PD", t).c_str(), "PD: 2 pass, 3 skip");
    t = {}; t.pass = 1; t.fail = 1; t.skip = 3;
    CHECK_STR(rl::family_footer("UFCS", t).c_str(), "UFCS: 1 pass, 1 fail");   // 28 cells with the skip count: it is left off, the label stays
    t = {}; t.pass = 11; t.fail = 12; t.skip = 13;
    CHECK_STR(rl::family_footer("SPR AVS", t).c_str(), "SPR AVS: 11 pass, 12 fail");
    t = {};
    CHECK_STR(rl::family_footer("PD", t).c_str(), "PD: no steps");
    t = {}; t.pass = 99; t.caution = 99;
    CHECK_STR(rl::family_footer("ABCDEFGHI", t).c_str(), "99 pass, 99 caution");   // 30 cells with the label: the label goes
}
// Whatever the counts, the footer fits the 27 cells of the footer band (the UI's kFooterCells) next to a 9-character label.
static void test_family_footer_fits_the_band() {
    static_assert(rl::kFooterChars == 27);
    const unsigned vals[] = {0, 1, 9, 10, 99, 100};
    for (const unsigned a : vals) for (const unsigned b : vals) for (const unsigned c : vals) for (const unsigned d : vals) {
        rl::FamilyTally t; t.pass = a; t.fail = b; t.caution = c; t.skip = d; t.listed = a ? 0 : d;
        const auto f = rl::family_footer("ABCDEFGHI", t);
        CHECK_EQ(f.len <= rl::kFooterChars, true);
        CHECK_EQ(f.len > 0, true);
    }
}
static void test_reach_footer_names_the_floor() {
    const uint32_t pdo = 3248763456u;   // PPS 3.3-21V, slot 7
    std::vector<ct::Step> all{step(A::Pps, 3300, V::Fail, 7, pdo, ct::Reason::Floored), step(A::Pps, 13000, V::Pass, 7, pdo)};
    all[0].measured_mv = 5000;   // the level the source held instead of the 3.3 V target
    const auto r = rl::reach(all, A::Pps);
    CHECK_EQ(r.floored, true);
    CHECK_EQ(r.floor_mv, 5000);
    CHECK_STR(text_of(r, true).c_str(), "floor at 5V");
    CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
    rl::Reach manual; manual.adv_mv = 11000; manual.floored = true; manual.floor_mv = 5000; manual.tone = rl::Tone::Fail;
    CHECK_STR(rl::reach_footer(manual).c_str(), "floor at 5V");
    CHECK_EQ(9 + rl::reach_footer(manual).len <= 27, true);
}
// A Light run lists a range without asking for it (Reason::Advertised): the footer says "advertised", never a shortfall
// such as "none reached", and the range's tone is Pass (the source has it: a green footer, never amber).
static void test_reach_footer_of_a_listed_range_says_advertised() {
    const uint32_t pdo = 3248763456u;   // PPS 5-21V, slot 7
    for (const A a : {A::Pps, A::SprAvs, A::EprAvs}) {
        std::vector<ct::Step> all{step(a, 5000, V::Skip, 7, pdo, ct::Reason::Advertised), step(a, 21000, V::Skip, 7, pdo, ct::Reason::Advertised)};
        const auto r = rl::reach(all, a);
        CHECK_EQ(int(r.tone), int(rl::Tone::Pass));
        CHECK_EQ(r.listed, true);
        CHECK_EQ(r.lo_mv, 0);   // nothing was measured: no "reached" span
        CHECK_STR(text_of(r, true).c_str(), "advertised");
        CHECK_EQ(9 + rl::reach_footer(r).len <= 27, true);
    }
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
// ---- The PD block: one label, fixed levels first, then each PPS range, then each AVS range -------------------------------------
static std::vector<ct::Step> mixed_pd() {   // the plan lists the AVS kinds first on purpose: the block orders by kind, not by plan
    const uint32_t v28 = (560u << 10) | 500;
    std::vector<ct::Step> s;
    ct::Step er = step(A::EprAvs, 15000, V::Pass, 9, 0xE004B145u); er.avs = true;
    ct::Step sr = step(A::SprAvs, 9000, V::Pass, 5, 0xE004B145u); sr.avs = true;
    s.push_back(er); s.push_back(sr);
    s.push_back(step(A::Epr, 28000, V::Pass, 8, v28));
    for (unsigned t : {5000u, 11000u}) s.push_back(step(A::Pps, t, V::Pass, 6, 3235656292u));
    const uint32_t fixed[] = {167874860u, 184620u, 307500u, 409935u};
    const unsigned mv[] = {5000, 9000, 15000, 20000};
    for (int i = 3; i >= 0; --i) s.push_back(step(A::Pd3Fixed, mv[i], V::Pass, uint8_t(i + 1), fixed[i]));
    return s;
}
static void test_the_pd_block_is_one_row_fixed_then_pps_then_avs() {
    rl::Layout l; rl::build(l, mixed_pd());
    CHECK_EQ(rows_labelled(l, "PD"), 1u);
    for (const char* w : {"PPS", "SPR AVS", "EPR AVS", "EPR", "PD3.x"}) CHECK_EQ(find(l, w) == nullptr, true);
    size_t blocks = 0;
    for (size_t i = 0; i < l.row_count; ++i) blocks += l.rows[i].pd;
    CHECK_EQ(blocks, 1u);                                                           // every chip sits in the one row
    CHECK_STR(fixed_chips(l).c_str(), "5V 3A 9V 3A 15V 3A 20V 3.35A 28V 5A");      // EPR levels that ran are volt chips in the run
    const rl::Row& b = *pd_row(l);
    unsigned kinds[16] = {}, n = 0;
    for (unsigned k = b.first; k < unsigned(b.first) + b.count; ++k) if (!l.tokens[k].glue) kinds[n++] = l.tokens[k].kind;
    CHECK_EQ(n, 8u);
    const unsigned want[] = {0, 0, 0, 0, 0, rl::kKindPps, rl::kKindSprAvs, rl::kKindEprAvs};
    for (unsigned i = 0; i < 8 && i < n; ++i) CHECK_EQ(kinds[i], want[i]);
    CHECK_EQ(sweep_row_of(l, 6) != nullptr && sweep_row_of(l, 5, A::SprAvs) != nullptr && sweep_row_of(l, 9, A::EprAvs) != nullptr, true);
    CHECK_STR(sweep_chips(l, 5, A::SprAvs).c_str(), "9-20V 3.25A");
    CHECK_STR(rl::kind_word(rl::kKindPps), "PPS");
    CHECK_STR(rl::kind_word(rl::kKindSprAvs), "AVS");
    CHECK_STR(rl::kind_word(rl::kKindEprAvs), "AVS");
    CHECK_STR(rl::kind_word(0), "");
    CHECK_EQ(l.cuts, 0u);
    // the wrap: the label sits on the first line only, a range chip never shares a line, the fixed run wraps by itself
    std::array<rl::Line, rl::kMaxLines> lines;
    const size_t nl = rl::wrap(l, rl::kGridMetrics, lines);
    size_t heads = 0, range_lines = 0;
    for (size_t i = 0; i < nl; ++i) {
        if (!l.rows[lines[i].row].pd) continue;
        heads += lines[i].head;
        unsigned ranges = 0, fixed = 0;
        for (unsigned k = lines[i].first; k < unsigned(lines[i].first) + lines[i].count; ++k) if (!l.tokens[k].glue) (l.tokens[k].kind ? ranges : fixed)++;
        CHECK_EQ(ranges <= 1 && (ranges == 0 || fixed == 0), true);
        range_lines += ranges;
    }
    CHECK_EQ(heads, 1u);
    CHECK_EQ(range_lines, 3u);
}
// A source with no PD at all: every PD step is "not advertised", the block has no chip, and the word "PD" folds into "No reply".
static void test_a_source_without_pd_reads_pd_under_no_reply() {
    std::vector<ct::Step> s;
    s.push_back(step(A::Pd3Fixed, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    s.push_back(step(A::Epr, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    s.push_back(step(A::Pps, 0, V::Skip, 0, 0, ct::Reason::NoOffer));
    s.push_back(step(A::Qc2, 5000, V::Pass));
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(pd_row(l) == nullptr, true);
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "PD");
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    rl::build_compact(g, rl::kGridMetrics, lines, s);
    bool found = false;
    for (size_t r = 0; r < g.row_count; ++r) {
        if (std::string(rl::row_label(g, r)) != "No reply") continue;
        rl::RowTokens rt; rl::row_tokens(g, r, s, {}, rt);
        found = rt.count == 1 && std::string(rt.toks[0].text) == "PD";
    }
    CHECK_EQ(found, true);
}
// Every PD step of a Custom all-on plan (both fixed kinds, EPR, PPS, both AVS kinds, their sweeps) is owned by exactly one row, in
// the Layout and in the compact grid: OK on the block opens them all, none is orphaned.
static void test_no_pd_step_is_left_without_an_owner() {
    auto s = mixed_pd();
    s.push_back(step(A::Pd2Fixed, 5000, V::Pass, 1, 167874860u));
    s.push_back(sweep_step(A::Pps, 6, 3235656292u, V::Pass));
    s.push_back(sweep_step(A::SprAvs, 5, 0xE004B145u, V::Pass));
    s.push_back(sweep_step(A::EprAvs, 9, 0xE004B145u, V::Fail));
    s.push_back(step(A::Epr, 36000, V::Skip, 9, (720u << 10) | 500, ct::Reason::OverCap));   // dropped from the chips, still owned
    s.push_back(step(A::Qc2, 5000, V::Pass));
    rl::Layout l; rl::build(l, s);
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    rl::build_compact(g, rl::kGridMetrics, lines, s);
    for (const auto& x : s) {
        size_t owners = 0, cowners = 0;
        for (size_t i = 0; i < l.row_count; ++i) owners += rl::row_has(l.rows[i], x);
        for (size_t i = 0; i < g.row_count; ++i) cowners += rl::row_has(g.rows[i], x);
        CHECK_EQ(owners, 1u);
        CHECK_EQ(cowners, 1u);
    }
}
// The widest block the Running grid holds (no folding: every tag stays): five SPR levels, three EPR levels, two PPS ranges and an
// SPR AVS range, all skipped and tagged: 11 chips with 11 tags, the size kMaxRowTokens is made for. Not a token is cut.
static void test_the_widest_pd_block_has_no_cuts() {
    std::vector<ct::Step> s;
    const unsigned mv[] = {5000, 9000, 12000, 15000, 20000};
    for (int i = 0; i < 5; ++i) s.push_back(step(A::Pd3Fixed, mv[i], V::Skip, uint8_t(i + 1), 167874860u, ct::Reason::CableLimit));
    for (unsigned v : {28000u, 36000u, 48000u}) s.push_back(step(A::Epr, v, V::Skip, 8, (v / 50u << 10) | 500, ct::Reason::OverCap));
    for (uint8_t p : {5, 6}) s.push_back(step(A::Pps, 0, V::Skip, p, 3235656292u, ct::Reason::CableLimit));
    s.push_back(step(A::SprAvs, 0, V::Skip, 4, 0xE004B145u, ct::Reason::CableLimit));
    for (auto& x : s) if (x.algorithm == A::SprAvs) x.avs = true;
    rl::OemNotes run; run.fold = false;
    rl::Layout l; rl::build(l, s, run);
    CHECK_EQ(l.cuts, 0u);
    const rl::Row& b = *pd_row(l);
    CHECK_EQ(unsigned(b.count), 22u);
    std::array<rl::Line, rl::kMaxLines> lines;
    rl::wrap(l, rl::kGridMetrics, lines);
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> cl;
    rl::build_compact(g, rl::kGridMetrics, cl, s, run);
    CHECK_EQ(g.cuts, 0u);
    for (size_t r = 0; r < g.row_count; ++r) {
        if (g.rows[r].kind != rl::CompactKind::Pd) continue;
        rl::RowTokens rt; rl::row_tokens(g, r, s, {}, rt);
        CHECK_EQ(unsigned(rt.count), 22u);   // the same 22 tokens, none dropped by the row buffer
    }
}
// A kind the source never offered is dropped from the block: no "SPR AVS" / "EPR AVS" row, no "None" word, no chip of that kind.
static void test_a_range_not_offered_is_dropped() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    CHECK_EQ(find(l, "SPR AVS") == nullptr && find(l, "EPR AVS") == nullptr && find(l, "EPR") == nullptr, true);
    for (size_t i = 0; i < l.row_count; ++i) CHECK_EQ(chips(l, i).find("None") == std::string::npos, true);
    for (size_t i = 0; i < l.token_count; ++i) CHECK_EQ(l.tokens[i].kind == rl::kKindSprAvs || l.tokens[i].kind == rl::kKindEprAvs, false);
    CHECK_EQ(find(l, "No reply") != nullptr && chips(l, row_of(l, "No reply")).find("EPR") == std::string::npos, true);
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
    CHECK_STR(chips(l, row_of(l, "Skipped")).c_str(), "QC2 QC3");   // the EPR check lives on the PD block's Steps page
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
    CHECK_STR(fixed_chips(l).c_str(), "28V 5A");   // the Results grid keeps the level that ran; skipped ones live on the Steps page
    rl::OemNotes run; run.fold = false;             // the Running grid keeps its structure and the tags
    rl::build(l, s, run);
    CHECK_STR(fixed_chips(l).c_str(), "28V 5A 36V 5A >36V 48V 5A >36V");
    const auto& row = *pd_row(l);
    CHECK_EQ(l.tokens[row.first + 2].chip, false);
    CHECK_EQ(l.tokens[row.first + 2].glue, true);
    CHECK_EQ(int(l.tokens[row.first + 2].tone), int(rl::Tone::Caution));
    // a refused entry, a cable limit and the 48 V monitor name themselves
    s = {step(A::Epr, 28000, V::Skip, 8, v28, ct::Reason::EprRefused), step(A::Epr, 36000, V::Skip, 9, v36, ct::Reason::CableLimit),
         step(A::Epr, 48000, V::Skip, 10, v48, ct::Reason::OverCap)};
    s[2].detail = ct::kDetailMonitor48;
    rl::build(l, s, run);
    CHECK_STR(fixed_chips(l).c_str(), "28V 5A refused 36V 5A cable 48V 5A >48V");
    rl::build(l, s);
    CHECK_EQ(pd_row(l) == nullptr, true);   // nothing ran: no block, the word "PD" reads under Skipped
    CHECK_STR(chips(l, row_of(l, "Skipped")).c_str(), "PD");
    // an EPR AVS range that was skipped carries its tag too
    s = {step(A::EprAvs, 15000, V::Skip, 11, 0xD0000000u | (480u << 17) | (150u << 8) | 140, ct::Reason::EprRefused)};
    s[0].detail = ct::kDetailEprFail + 3;
    rl::build(l, s, run);
    CHECK_STR(sweep_chips(l, 11, A::EprAvs).c_str(), "15-48V 2.91A refused");   // the old 12-byte chip cut this to "15-48V 2.91"
    rl::build(l, s);
    CHECK_EQ(sweep_row_of(l, 11, A::EprAvs) == nullptr, true);
}
// The positions with a range line on the Results grid (the spec section's split rule reads them): the chips build() keeps, so a
// skipped-only EPR AVS range (folded, no line) is not one.
static uint16_t chip_positions(const rl::Layout& l) {
    uint16_t m = 0;
    for (size_t i = 0; i < l.token_count; ++i) if (l.tokens[i].kind) m |= uint16_t(1u << l.tokens[i].mv);
    return m;
}
static void test_range_positions_are_the_range_chips() {
    auto s = mixed_pd();
    rl::Layout l; rl::build(l, s);
    CHECK_EQ(unsigned(rl::range_positions(s)), unsigned(1u << 5 | 1u << 6 | 1u << 9));
    CHECK_EQ(unsigned(rl::range_positions(s)), unsigned(chip_positions(l)));
    s.push_back(step(A::EprAvs, 15000, V::Skip, 11, 0xD0000000u | (480u << 17) | (150u << 8) | 140, ct::Reason::EprRefused));
    rl::build(l, s);
    CHECK_EQ(unsigned(rl::range_positions(s)), unsigned(chip_positions(l)));
    CHECK_EQ(rl::range_positions(s) >> 11 & 1u, 0u);
    s.push_back(step(A::SprAvs, 9000, V::Skip, 12, 0xE004B145u, ct::Reason::Request));   // a skipped SPR AVS range keeps its chip
    rl::build(l, s);
    CHECK_EQ(unsigned(rl::range_positions(s)), unsigned(chip_positions(l)));
    CHECK_EQ(rl::range_positions(baseus()), uint16_t(1u << 6 | 1u << 7));
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
    const size_t pr = row_of(p, "PD");
    CHECK_EQ(int(p.tokens[p.rows[pr].first + 1].mv), 9000);
    CHECK_EQ(int(p.tokens[sweep_row_of(p, 6)->first].mv), 6);   // a range chip names its slot, not a level
}
static void test_absent_protocols_fold_into_one_row() {
    rl::Layout l; auto s = baseus(); rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "FCP UFCS");   // the words follow the plan's order (here: the fixture's)
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
    CHECK_EQ(find(l, "EPR") == nullptr, true);          // a PD kind the source does not offer is dropped from the block
    CHECK_EQ(find(l, "QC2") < find(l, "FCP"), true);    // plan order (the fixture runs QC2 first), not "failed units last"
    CHECK_EQ(find(l, "No reply") == nullptr, true);     // and no word is left to fold
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
    // Then the block's PPS slot 6 "PPS 5-11V 5A" on a line of its own and slot 7 "PPS 5-21V 3.2A" on the next: a range chip
    // never shares a line, and the continuation lines carry no label (head false).
    CHECK_EQ(lines[3].row, 1); CHECK_EQ(lines[3].count, 1); CHECK_EQ(lines[3].head, false);
    CHECK_EQ(lines[4].row, 1); CHECK_EQ(lines[4].count, 1); CHECK_EQ(lines[4].head, false);
    CHECK_EQ(l.tokens[lines[3].first].kind, rl::kKindPps); CHECK_EQ(l.tokens[lines[3].first].mv, 6);
    CHECK_EQ(l.tokens[lines[4].first].mv, 7);
    CHECK_EQ(lines[5].row, 2); CHECK_EQ(lines[5].count, 2); CHECK_EQ(lines[5].head, true);   // No reply: FCP UFCS
    CHECK_EQ(n, 7);   // header, PD 2 + 1 + 1, No reply 1, Skipped 1
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
    CHECK_EQ(pd < row_of(l, "PD"), true);
    CHECK_EQ(row_of(l, "PD") < qc, true);
    CHECK_EQ(qc < row_of(l, "QC2"), true);
    CHECK_EQ(find(l, "OEM / PD") == nullptr, true);       // no notes -> no OEM / PD section
    CHECK_EQ(find(l, "Xiaomi") == nullptr, true);         // no Mi auth and no Xiaomi row -> no Xiaomi section
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
// The SuperVOOC ceiling stage's result is a passive fact row of the same section: the highest 0x86 ask the session went on
// under ("6.3A claimed", the charger's accepted ceiling, never a drawn current), the words of svooc_ceiling::format_cap.
static void test_svooc_ceiling_note_row() {
    rl::Layout none;
    rl::build(none, baseus());
    CHECK_EQ(find(none, "SV cap") == nullptr, true);
    rl::OemNotes notes;
    notes.svooc_cap = "6.3A claimed";
    rl::Layout l;
    rl::build(l, baseus(), notes);
    const rl::Row* r = find(l, "SV cap");
    CHECK_EQ(r != nullptr, true);
    CHECK_STR(chips(l, row_of(l, "SV cap")).c_str(), "6.3A claimed");
    {   // the longest words (the no-load stop) are laid out whole: the phrase wraps, nothing is cut
        rl::OemNotes stop;
        stop.svooc_cap = "stopped at 12.0s, <=11.0V, 10.9A 120W claimed";
        rl::Layout s;
        rl::build(s, baseus(), stop);
        CHECK_STR(chips(s, row_of(s, "SV cap")).c_str(), "stopped at 12.0s, <=11.0V, 10.9A 120W claimed");
        stop.svooc_cap = "aborted: off-script, <=11.1V, 10.9A 121W claimed";   // the longest abort phrase (48 characters)
        rl::Layout a;
        rl::build(a, baseus(), stop);
        CHECK_STR(chips(a, row_of(a, "SV cap")).c_str(), "aborted: off-script, <=11.1V, 10.9A 121W claimed");
        CHECK_EQ(s.cuts + a.cuts, 0u);
    }
    {   // the ident of a Light / Medium run (no stage): its own row, beside the legacy rows' "detected"
        rl::OemNotes id;
        id.vooc_ident = "0xAC";
        rl::Layout v;
        rl::build(v, baseus(), id);
        CHECK_STR(chips(v, row_of(v, "VOOC id")).c_str(), "0xAC");
        CHECK_EQ(find(v, "SV cap") == nullptr, true);
        CHECK_EQ(v.cuts, 0u);
    }
    CHECK_EQ(int(r->algorithm), int(A::Count));
    notes.scp_cap = "20V 6A";
    rl::Layout both;
    rl::build(both, baseus(), notes);
    CHECK_EQ(find(both, "SV cap") != nullptr && find(both, "SCP cap") != nullptr, true);
    notes.headers = false;
    rl::Layout dense;
    rl::build(dense, baseus(), notes);
    CHECK_EQ(find(dense, "SV cap") != nullptr, true);
    CHECK_EQ(dense.cuts, 0u);
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
// The Xiaomi unit's steps: its chain row (no level; Step::detail = how the chain ended) and a mode it unlocked, at its new level.
static ct::Step mi_chain(V v, uint8_t code) {
    ct::Step c = step(A::Pd3Fixed, 0, v); c.hunt = true; c.reason = ct::Reason::MiChain; c.detail = code;
    return c;
}
static ct::Step mi_mode(A a, unsigned mv, unsigned ma, V v, unsigned pos, uint32_t pdo) {
    ct::Step h = step(a, mv, v, pos, pdo); h.hunt = true; h.ma = uint16_t(ma);
    return h;
}
// Row labels in layout order, "*" for a section header ("Xiaomi" -> "*Xiaomi"), to compare whole layouts at once.
static std::string row_order(const rl::Layout& l) {
    std::string out;
    for (size_t i = 0; i < l.row_count; ++i) { if (i) out += ","; if (l.rows[i].header) out += "*"; out += l.rows[i].label; }
    return out;
}
// The Xiaomi unit is one row ("Xiaomi", Row::probe) in the Xiaomi section: one chip per mode the chain unlocked by the watts of the level the row asked ("55W", never the advertised list of the PD rows), which stay out of the USB PD chips.
static void test_the_xiaomi_unit_is_one_row_in_its_section() {
    auto s = baseus();
    s.push_back(mi_chain(V::Pass, k2::mi_unlock::kCodeVerified));
    s.push_back(mi_mode(A::Pps, 11000, 5000, V::Pass, 6, 3235656292u));
    s.push_back(mi_mode(A::Pd3Fixed, 20000, 5000, V::Fail, 5, 409935u));
    rl::Layout l; rl::build(l, s);
    const size_t header = row_of(l, "Xiaomi"), mi = header + 1;
    CHECK_EQ(l.rows[header].header, true);
    CHECK_STR(l.rows[mi].label, "Xiaomi");
    CHECK_EQ(l.rows[mi].probe, true);
    CHECK_EQ(mi > row_of(l, "PD"), true);   // the row stands where the plan runs it: after the PD block
    CHECK_STR(chips(l, mi).c_str(), "55W 100W");   // each mode by its watts, no auth chip
    CHECK_EQ(int(l.tokens[l.rows[mi].first].tone), int(rl::Tone::Pass));
    CHECK_EQ(int(l.tokens[l.rows[mi].first + 1].tone), int(rl::Tone::Fail));
    for (const auto& x : s) CHECK_EQ(rl::row_has(l.rows[mi], x), x.hunt);   // the row owns every Xiaomi step and nothing else
    // The PD chips keep the advertised currents and pass counts, untouched by the Xiaomi rows.
    CHECK_STR(fixed_chips(l).c_str(), "5V 3A 9V 3A 12V 3A 15V 3A 20V 3.35A");
    CHECK_STR(sweep_chips(l, 6).c_str(), "5-11V 5A");
    CHECK_STR(sweep_chips(l, 7).c_str(), "5-21V 3.2A");
}
// The rows follow the step list: the chain row closes the Pd unit, so the Xiaomi row stands after the PD rows and before the next
// unit. With the row in hand the "Mi Turbo" auth fact is not repeated (the "auth" chip says it).
static void test_the_xiaomi_row_stands_where_the_plan_runs_it() {
    std::vector<ct::Step> t{step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u), step(A::Pps, 11000, V::Pass, 6, 3235656292u),
                            mi_chain(V::Pass, k2::mi_unlock::kCodeVerified), mi_mode(A::Pps, 11000, 5000, V::Pass, 6, 3235656292u),
                            step(A::Qc2, 9000, V::Pass)};
    rl::OemNotes notes; notes.mi_auth = true;
    rl::Layout l; rl::build(l, t, notes);
    CHECK_STR(row_order(l).c_str(), "*USB PD,PD,*Xiaomi,Xiaomi,*QC/UFCS,QC2");
    rl::OemNotes dense; dense.headers = false; dense.mi_auth = true;
    rl::build(l, t, dense);
    CHECK_STR(row_order(l).c_str(), "PD,Xiaomi,QC2");
    // A run with no Xiaomi row (the Pd unit not selected) keeps the passive auth fact.
    std::vector<ct::Step> u{step(A::Qc2, 9000, V::Pass)};
    rl::build(l, u, notes);
    CHECK_STR(row_order(l).c_str(), "*QC/UFCS,QC2,*Xiaomi,Mi Turbo");
}
// The chain on the row: unlocked modes alone (no auth chip); a finished chain with nothing raised is one passed "auth" chip; a chain
// that could not run is one skipped "auth" chip (its reason stays on the steps page); a mode the cable cannot carry is a neutral skip.
static void test_the_xiaomi_row_says_how_the_chain_ended() {
    rl::Layout l;
    const auto only = [&](std::vector<ct::Step> s) { rl::build(l, s); return chips(l, row_of(l, "Xiaomi") + 1); };
    const auto first = [&](rl::Tone t) { const rl::Token& k = l.tokens[l.rows[row_of(l, "Xiaomi") + 1].first]; return k.chip && k.tone == t; };
    for (const uint8_t code : {k2::mi_unlock::kCodeNoSvid, k2::mi_unlock::kCodeNoAnswer, k2::mi_unlock::chain_code(true, false, 5, 0)}) {
        CHECK_STR(only({mi_chain(V::Skip, code)}).c_str(), "auth");
        CHECK_EQ(first(rl::Tone::Skip), true);
    }
    CHECK_STR(only({mi_chain(V::Pass, k2::mi_unlock::kCodeFinished)}).c_str(), "auth");
    CHECK_EQ(first(rl::Tone::Pass), true);
    CHECK_STR(only({mi_chain(V::Pending, 0)}).c_str(), "auth");   // the Running grid, before the chain row's turn
    CHECK_EQ(first(rl::Tone::Pending), true);
    ct::Step cut = mi_mode(A::Pps, 11000, 6000, V::Skip, 6, 0xC0DC3278u); cut.reason = ct::Reason::CableLimit;
    CHECK_STR(only({mi_chain(V::Pass, k2::mi_unlock::kCodeVerified), cut}).c_str(), "66W cable");
    CHECK_EQ(int(l.tokens[l.rows[row_of(l, "Xiaomi") + 1].first].tone) != int(rl::Tone::Fail), true);
}

// ---- summarize(): the best passing mode per protocol family, as words with volts and amps ---------------------------
static ct::Step hunt(unsigned mv, unsigned ma, V v) {   // a Xiaomi mode row at its new level
    ct::Step s = step(A::Pd3Fixed, mv, v); s.hunt = true; s.ma = uint16_t(ma);
    return s;
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
    CHECK_STR(v[0].text, "Mi 120W");
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
    CHECK_STR(summary_of(s)[0].text, "VOOC ok");   // the family's word, never a version ("SVOOC65")
    s.push_back(seen);
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);                  // one OPPO family line: on a tie the more capable protocol wins
    CHECK_STR(v[0].text, "VOOC seen");
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
    // OEM watts (oem_watts::chip): the volts, then the level's watts at the mode's advertised current (3 A)
    CHECK_STR(chips(l, row_of(l, "UFCS")).c_str(), "5V 15W 8V 24W 11V 33W 12.5V 37.5W 20V 60W");   // the step back to 5 V is the same chip
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
    CHECK_STR(chips(l, row_of(l, "UFCS")).c_str(), "5V 20W 5.5V 22W 8.2V 32.8W 11V 44W");
    // Two modes at different currents: each level reads its own mode's, so each level's watts follow its mode.
    std::vector<ct::Step> two = {in_mode(5000, V::Pass, 3400, 11000, 1000, 3250), in_mode(11000, V::Pass, 3400, 11000, 1000, 3250),
                                 in_mode(21000, V::Pass, 11000, 21000, 1000, 5000)};
    rl::Layout t; rl::build(t, two);
    CHECK_STR(chips(t, row_of(t, "UFCS")).c_str(), "5V 16.3W 11V 35.8W 21V 105W");
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
    const rl::Row* r = pd_row(l);
    CHECK_EQ(r != nullptr, true);
    CHECK_EQ(chips(l, row_of(l, "PD")).find("needs load") != std::string::npos, true);
    for (unsigned i = r->first; i < unsigned(r->first) + r->count; ++i) CHECK_EQ(int(l.tokens[i].tone) != int(rl::Tone::Fail), true);
    CHECK_EQ(l.cuts, 0u);
    std::vector<ct::Step> all_skipped{step(A::Pps, 13000, V::Skip, 7, pdo, ct::Reason::NeedsLoad)};
    rl::Layout m; rl::build(m, all_skipped);
    CHECK_EQ(chips(m, row_of(m, "PD")).find("needs load") != std::string::npos, true);
    CHECK_EQ(int(m.tokens[pd_row(m)->first].tone), int(rl::Tone::Skip));
}
// The Running view's progress bar counts rows the run executes: plan-time skips (a Light run's advertised PD rows, a protocol the
// charger never offered) are not part of the work, so a fresh run starts at 0 and the bar never starts half full.
static void test_run_progress_counts_executed_rows_only() {
    std::vector<ct::Step> s;
    for (unsigned i = 0; i < 5; ++i) s.push_back(step(A::Pd3Fixed, 9000 + i, V::Skip, uint8_t(i + 2), 1u, ct::Reason::Advertised));
    for (unsigned i = 0; i < 5; ++i) s.push_back(step(A::Fcp, 5000 + i, V::Pending));
    rl::Progress p = rl::run_progress(s);
    CHECK_EQ(p.done, 0u); CHECK_EQ(p.total, 5u);                       // 10 rows, 5 planned away: empty bar, 5 to run
    s[5].verdict = V::Pass;
    p = rl::run_progress(s); CHECK_EQ(p.done, 1u); CHECK_EQ(p.total, 5u);
    s[6].verdict = V::Fail;
    p = rl::run_progress(s); CHECK_EQ(p.done, 2u); CHECK_EQ(p.total, 5u);
    s[7].verdict = V::Running;                                           // the row under test is work still to do
    p = rl::run_progress(s); CHECK_EQ(p.done, 2u); CHECK_EQ(p.total, 5u);
    s[8].verdict = V::Skip; s[8].reason = ct::Reason::Request;           // a runtime skip leaves the work: the total shrinks
    p = rl::run_progress(s); CHECK_EQ(p.done, 2u); CHECK_EQ(p.total, 4u);
    s[9].verdict = V::Pass; s[9].reason = ct::Reason::Folded;            // a caution is a pass that ran
    p = rl::run_progress(s); CHECK_EQ(p.done, 3u); CHECK_EQ(p.total, 4u);
    std::vector<ct::Step> none{step(A::Pd3Fixed, 9000, V::Skip, 2, 1u, ct::Reason::Advertised)};
    p = rl::run_progress(none); CHECK_EQ(p.done, 0u); CHECK_EQ(p.total, 0u);   // nothing to run: empty, never a division by zero
    p = rl::run_progress({}); CHECK_EQ(p.done, 0u); CHECK_EQ(p.total, 0u);
}
// OEM levels by their watts (oem_watts.hpp). The VOOC family's three units (VOOC, SVOOC, SVOOC65) share one detection: with the
// Results' level notes they read as two level rows, the detection chip leading the VOOC row, then one chip per level in the claimed
// look (Noted), not reached (Dim) or not tested (Skip). The ident never proves a level: a Medium run tests none.
static std::vector<ct::Step> vooc_detected() {
    std::vector<ct::Step> s;
    for (A a : {A::Vooc, A::SuperVooc, A::SuperVooc65}) {
        auto x = step(a, 5000, V::Pass);
        x.detail = uint8_t(fc::Fail::Detected);
        s.push_back(x);
    }
    s.push_back(step(A::Pd3Fixed, 5000, V::Pass, 1, 167874860u));
    return s;
}
static std::string tones(const rl::Layout& l, size_t row) {
    std::string out;
    for (unsigned i = l.rows[row].first; i < unsigned(l.rows[row].first) + l.rows[row].count; ++i) {
        const rl::Tone t = l.tokens[i].tone;
        out += t == rl::Tone::Pass ? 'P' : t == rl::Tone::Noted ? 'N' : t == rl::Tone::Dim ? 'D' : t == rl::Tone::Skip ? 'S' : '?';
    }
    return out;
}
static void test_vooc_family_reads_as_watts_levels() {
    namespace ow = oem_watts;
    namespace sc = svooc_ceiling;
    rl::OemNotes n;
    n.vooc_watts = true;
    n.vooc_claims = ow::claims(ow::Family::Vooc, {0x06D, sc::End::Stopped, 11020, 3300, 0});   // hardware: 120 W claimed
    rl::Layout l;
    rl::build(l, vooc_detected(), n);
    CHECK_EQ(find(l, "SVOOC65") == nullptr, true);   // no version label
    CHECK_STR(chips(l, row_of(l, "VOOC")).c_str(), "detected 20W");
    CHECK_STR(tones(l, row_of(l, "VOOC")).c_str(), "PS");   // the detection passed; 20 W (5 V 4 A) is never asked
    CHECK_STR(chips(l, row_of(l, "SVOOC")).c_str(), "50W 65W 100W 120W");
    CHECK_STR(tones(l, row_of(l, "SVOOC")).c_str(), "NNNN");
    CHECK_EQ(row_of(l, "OPPO") < row_of(l, "VOOC") && row_of(l, "VOOC") < row_of(l, "SVOOC"), true);
    CHECK_EQ(l.cuts, 0u);
    for (size_t r = 0; r < l.row_count; ++r) {   // the Layout rows own the same steps as the compact rows below
        if (!l.rows[r].levels) continue;
        const auto st = vooc_detected();
        CHECK_EQ(rl::row_has(l.rows[r], st[0]) && rl::row_has(l.rows[r], st[2]) && !rl::row_has(l.rows[r], st[3]), true);
    }
    // Every family step stays reachable from both rows (the Steps drill-in lists the detection).
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    const auto steps = vooc_detected();
    const size_t ln = rl::build_compact(g, rl::kGridMetrics, lines, steps, n);
    CHECK_EQ(ln >= 3u, true);
    int level_rows = 0;
    for (size_t r = 0; r < g.row_count; ++r) {
        if (g.rows[r].kind != rl::CompactKind::Levels) continue;
        ++level_rows;
        for (size_t i = 0; i < 3; ++i) CHECK_EQ(rl::row_has(g.rows[r], steps[i]), true);
        CHECK_EQ(rl::row_has(g.rows[r], steps[3]), false);
        CHECK_EQ(rl::row_section(g, r), int(rl::Section::Oppo));
        rl::RowTokens rt;
        rl::row_tokens(g, r, steps, {}, rt);
        const std::string want = std::string(rl::row_label(g, r)) == "VOOC" ? "detected 20W" : "50W 65W 100W 120W";
        std::string got;
        for (unsigned i = 0; i < rt.count; ++i) { if (i) got += ' '; got += rt.toks[i].text; }
        CHECK_STR(got.c_str(), want.c_str());
    }
    CHECK_EQ(level_rows, 2);
    {   // a Medium run: the ident alone, every level not tested
        rl::OemNotes m;
        m.vooc_watts = true;
        rl::Layout v;
        rl::build(v, vooc_detected(), m);
        CHECK_STR(tones(v, row_of(v, "SVOOC")).c_str(), "SSSS");
        CHECK_STR(tones(v, row_of(v, "VOOC")).c_str(), "PS");
    }
    {   // a ladder that stood at 6.3 A at 11 V (69 W): 50 and 65 W claimed, 100 and 120 W not reached
        rl::OemNotes p;
        p.vooc_watts = true;
        p.vooc_claims = ow::claims(ow::Family::Vooc, {0x03F, sc::End::Judged, 11000, 0, 0});
        rl::Layout v;
        rl::build(v, vooc_detected(), p);
        CHECK_STR(tones(v, row_of(v, "SVOOC")).c_str(), "NNDD");
    }
    {   // the Running grid (no level notes) keeps the plan's unit rows
        rl::Layout v;
        rl::build(v, vooc_detected(), {});
        CHECK_EQ(find(v, "SVOOC65") != nullptr, true);
    }
}
// A family that never answered folds as one word, never three ("VOOC SVOOC SVOOC65").
static void test_a_silent_vooc_family_folds_as_one_word() {
    std::vector<ct::Step> s;
    auto lead = step(A::Vooc, 5000, V::Skip, 0, 0, ct::Reason::Request);   // a silent family is skipped, not failed
    lead.detail = uint8_t(fc::Fail::NoAnswer);
    s.push_back(lead);
    for (A a : {A::SuperVooc, A::SuperVooc65}) {
        auto x = step(a, 5000, V::Skip, 0, 0, ct::Reason::Request);
        x.detail = uint8_t(fc::Fail::NoAnswer);
        s.push_back(x);
    }
    s.push_back(step(A::Fcp, 5000, V::Fail));
    rl::OemNotes n;
    n.vooc_watts = true;
    rl::Layout l;
    rl::build(l, s, n);
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "VOOC FCP");
    {   // only SVOOC65 planned: the family's word all the same
        std::vector<ct::Step> o = {s[2], s[3]};
        rl::Layout v;
        rl::build(v, o, n);
        CHECK_STR(chips(v, row_of(v, "No reply")).c_str(), "VOOC FCP");
    }
    {   // the plain grid (no level notes) folds the family as one word too
        rl::Layout v;
        rl::build(v, s);
        CHECK_EQ(chips(v, row_of(v, "No reply")).find("SVOOC") == std::string::npos, true);
    }
    CHECK_EQ(find(l, "Skipped") == nullptr, true);
    CHECK_EQ(find(l, "SVOOC") == nullptr, true);
    // is_no_answer: only a family skip that carries the engine's word (a plain skip, a fail and a non-family step are not)
    CHECK_EQ(rl::is_no_answer(s[0]), true);
    CHECK_EQ(rl::is_no_answer(s[2]), true);
    auto fl = s[0]; fl.verdict = V::Fail;
    CHECK_EQ(rl::is_no_answer(fl), false);
    auto pl = s[0]; pl.detail = 0;
    CHECK_EQ(rl::is_no_answer(pl), false);
    auto ot = step(A::Fcp, 5000, V::Skip, 0, 0, ct::Reason::Request); ot.detail = uint8_t(fc::Fail::NoAnswer);
    CHECK_EQ(rl::is_no_answer(ot), false);
    rl::CompactGrid g; std::array<rl::CompactLine, rl::kMaxLines> lines;
    rl::build_compact(g, rl::kGridMetrics, lines, s, n);
    for (size_t r = 0; r < g.row_count; ++r) {
        if (std::string(rl::row_label(g, r)) != "No reply") continue;
        rl::RowTokens rt;
        rl::row_tokens(g, r, s, {}, rt);
        CHECK_EQ(int(rt.count), 2);
        CHECK_STR(rt.toks[0].text, "VOOC");
    }
}
// A protocol that never answered its first request (VIVO / UFCS no response, AFC / FCP / SCP unsupported) was not offered: its rows are
// skipped, not failed, and fold as "No reply". A charger that answered and then failed a later step keeps its red cells.
static void test_a_silent_protocol_folds_as_no_reply_and_a_heard_one_stays_red() {
    std::vector<ct::Step> s;
    auto add = [&](A a, unsigned mv, fc::Fail why) { auto x = step(a, mv, V::Skip, 0, 0, ct::Reason::Request); x.detail = uint8_t(why); s.push_back(x); };
    for (unsigned mv : {5000u, 9000u, 11000u, 5000u}) add(A::Vivo, mv, fc::Fail::NoResponse);
    add(A::Ufcs, 5000, fc::Fail::NoResponse);
    for (A a : {A::Afc, A::Fcp, A::Scp}) add(a, 5000, fc::Fail::Unsupported);
    for (A a : {A::Qc2, A::Qc3, A::Qc3Plus, A::Ufcs}) add(a, 5000, fc::Fail::NoHandshake);
    for (const auto& x : s) CHECK_EQ(rl::is_no_answer(x), true);
    rl::Layout l;
    rl::build(l, s);
    CHECK_EQ(find(l, "Skipped") == nullptr, true);
    const std::string nr = chips(l, row_of(l, "No reply"));
    for (const char* name : {"VIVO", "UFCS", "AFC", "FCP", "SCP", "QC2", "QC3", "QC3+"}) CHECK_EQ(nr.find(name) != std::string::npos, true);
    {   // a plain skip (no engine word) and a fail are not "no reply"; nor is a QC2 row that failed after the unit was heard
        auto plain = s[0]; plain.detail = 0;
        CHECK_EQ(rl::is_no_answer(plain), false);
        auto fl = s[0]; fl.verdict = V::Fail;
        CHECK_EQ(rl::is_no_answer(fl), false);
        auto qc = step(A::Qc2, 9000, V::Fail, 0, 0, ct::Reason::Request); qc.detail = uint8_t(fc::Fail::NoHandshake);
        CHECK_EQ(rl::is_no_answer(qc), false);
    }
    {   // answered at 5 V, then the 9 V request got no frame: a real fail, shown as the unit's own failed chip, never folded
        std::vector<ct::Step> h = {step(A::Vivo, 5000, V::Pass), step(A::Vivo, 9000, V::Fail, 0, 0, ct::Reason::Request),
                                   step(A::Vivo, 11000, V::Skip, 0, 0, ct::Reason::Request)};
        h[1].detail = uint8_t(fc::Fail::NoResponse);
        h[2].detail = uint8_t(fc::Fail::NoResponse);
        CHECK_EQ(rl::is_no_answer(h[1]), false);
        rl::Layout v;
        rl::build(v, h);
        CHECK_EQ(find(v, "No reply") == nullptr, true);
        CHECK_EQ(find(v, "VIVO") != nullptr, true);
    }
    {   // the Running grid (no fold): a silent unit is one dim "No reply" cell on its own row once its detection step settled; a
        // unit still pending, or one that answered and failed later, keeps its level cells
        std::vector<ct::Step> r;
        for (unsigned mv : {5000u, 9000u, 11000u}) { auto x = step(A::Vivo, mv, V::Skip, 0, 0, ct::Reason::Request); x.detail = uint8_t(fc::Fail::NoResponse); r.push_back(x); }
        for (A a : {A::Qc2, A::Qc3}) for (unsigned mv : {5000u, 9000u}) { auto x = step(a, mv, V::Skip, 0, 0, ct::Reason::Request); x.detail = uint8_t(fc::Fail::NoHandshake); r.push_back(x); }
        r.push_back(step(A::Ufcs, 5000, V::Pending));
        r.push_back(step(A::Ufcs, 9000, V::Pending));
        rl::OemNotes run; run.fold = false; run.headers = false;
        rl::Layout l; rl::build(l, r, run);
        for (const char* name : {"VIVO", "QC2", "QC3"}) CHECK_STR(chips(l, row_of(l, name)).c_str(), "No reply");
        CHECK_EQ(chips(l, row_of(l, "UFCS")).find("No reply") == std::string::npos, true);
        std::vector<ct::Step> h = {step(A::Vivo, 5000, V::Pass), step(A::Vivo, 9000, V::Fail, 0, 0, ct::Reason::Request)};
        h[1].detail = uint8_t(fc::Fail::NoResponse);
        rl::Layout hl; rl::build(hl, h, run);
        CHECK_EQ(chips(hl, row_of(hl, "VIVO")).find("No reply") == std::string::npos, true);
    }
}
// Units skipped on open D+/D- (the one cause, Reason::LinesOpen) fold as their own "No D+/D-" word, never "Skipped" or "No reply";
// a PD source (its rows are not LinesOpen) and a real no-reply are unchanged.
static void test_open_dpdm_units_fold_as_one_cause() {
    std::vector<ct::Step> s;
    for (A a : {A::Qc2, A::Qc3, A::Ufcs, A::Vivo}) s.push_back(step(a, 5000, V::Skip, 0, 0, ct::Reason::LinesOpen));
    s.push_back(step(A::Qc2, 9000, V::Skip, 0, 0, ct::Reason::LinesOpen));
    auto quiet = step(A::Afc, 5000, V::Skip, 0, 0, ct::Reason::Request); quiet.detail = uint8_t(fc::Fail::Unsupported);
    s.push_back(quiet);   // a genuine no-reply beside it stays "No reply"
    s.push_back(step(A::Fcp, 5000, V::Pass));
    for (const auto& x : s) CHECK_EQ(rl::is_lines_open(x), x.reason == ct::Reason::LinesOpen);
    rl::Layout l;
    rl::build(l, s);
    const std::string od = chips(l, row_of(l, "No D+/D-"));
    for (const char* name : {"QC2", "QC3", "UFCS", "VIVO"}) CHECK_EQ(od.find(name) != std::string::npos, true);
    CHECK_EQ(od.find("AFC") == std::string::npos, true);
    CHECK_STR(chips(l, row_of(l, "No reply")).c_str(), "AFC");
    CHECK_EQ(find(l, "Skipped") == nullptr, true);
    CHECK_EQ(int(find(l, "No D+/D-")->label_tone), int(rl::Tone::Caution));
    {   // the Running grid (no fold): the unit's own row says the cause in one word
        rl::OemNotes run; run.fold = false; run.headers = false;
        rl::Layout r; rl::build(r, s, run);
        CHECK_STR(chips(r, row_of(r, "QC2")).c_str(), "No D+/D-");
    }
    {   // short lines (the check read them fine): the rows keep their own reasons, no cause word
        std::vector<ct::Step> p = {step(A::Qc2, 5000, V::Pass), step(A::Qc3, 5000, V::Skip, 0, 0, ct::Reason::Disabled)};
        rl::Layout v; rl::build(v, p);
        CHECK_EQ(find(v, "No D+/D-") == nullptr, true);
    }
}
// SCP names the limit it writes: its chip reads the level's watts (10 V x 2.25 A = 22.5 W) where it used to read the current; the
// handshake row names none and keeps its volts. The summary line words the best SCP level the same way.
static void test_scp_chips_and_summary_read_watts() {
    std::vector<ct::Step> s = {step(A::Scp, 5000, V::Pass), step(A::Scp, 10000, V::Pass), step(A::Scp, 4500, V::Pass)};
    s[1].ma = 2250; s[2].ma = 5000;
    rl::Layout l;
    rl::build(l, s);
    CHECK_STR(chips(l, row_of(l, "SCP")).c_str(), "5V 10V 22.5W 4.5V 22.5W");
    const auto v = summary_of(s);
    CHECK_EQ(v.size(), 1u);
    CHECK_STR(v[0].text, "SCP 22.5W");
    CHECK_EQ(v[0].mw, 0u);   // a claimed figure ranks below every measured PD level, as before
    // The top volts are not the top watts: 9 V x 2 A (18 W) under 5 V x 4.5 A (22.5 W); the line names the higher claim.
    std::vector<ct::Step> w = {step(A::Scp, 9000, V::Pass), step(A::Scp, 5000, V::Pass)};
    w[0].ma = 2000; w[1].ma = 4500;
    CHECK_STR(summary_of(w)[0].text, "SCP 22.5W");
    // FCP 12 V names no current: an SCP claim of the same family leads it
    std::vector<ct::Step> h = {step(A::Fcp, 12000, V::Pass), step(A::Scp, 10000, V::Pass)};
    h[1].ma = 2250;
    CHECK_STR(summary_of(h)[0].text, "SCP 22.5W");
}
// A level row's footer: the top claimed level, else the span not reached (the ladder ran) or not tested; at most 27 characters.
static void test_levels_footer_names_the_top_claim() {
    namespace ow = oem_watts;
    namespace sc = svooc_ceiling;
    rl::CompactRow r;
    r.kind = rl::CompactKind::Levels;
    r.first = 1; r.count = 4;   // SVOOC 50 .. 120W
    r.mv = ow::claims(ow::Family::Vooc, {0x06D, sc::End::Stopped, 11020, 3300, 0});
    CHECK_STR(rl::levels_footer(r, "SVOOC").c_str(), "SVOOC: up to 120W claimed");
    r.mv = ow::claims(ow::Family::Vooc, {0x03F, sc::End::Judged, 11000, 0, 0});
    CHECK_STR(rl::levels_footer(r, "SVOOC").c_str(), "SVOOC: up to 65W claimed");
    r.mv = ow::claims(ow::Family::Vooc, {0x028, sc::End::Cap, 11000, 0, 0});   // 44 W: below every level
    CHECK_STR(rl::levels_footer(r, "SVOOC").c_str(), "SVOOC: 50-120W not reached");
    r.mv = 0;
    CHECK_STR(rl::levels_footer(r, "SVOOC").c_str(), "SVOOC: 50-120W not tested");
    CHECK_STR(rl::levels_footer(r, "LONGWORD").c_str(), "50-120W not tested");   // 28 with the label: the label goes
    r.first = 0; r.count = 1;   // VOOC 20W
    CHECK_STR(rl::levels_footer(r, "VOOC").c_str(), "VOOC: 20W not tested");
    for (uint16_t p : {uint16_t(0), uint16_t(0x5555), uint16_t(0xAAAA)})
        for (uint8_t f = 0; f < 5; ++f) {
            r.first = f; r.count = uint8_t(5 - f); r.mv = p;
            CHECK_EQ(rl::levels_footer(r, "SVOOC").len <= rl::kFooterChars, true);
        }
    r.kind = rl::CompactKind::Legacy;
    CHECK_EQ(int(rl::levels_footer(r, "SCP").len), 0);
}

int main() {
    test_run_progress_counts_executed_rows_only();
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
    test_the_xiaomi_unit_is_one_row_in_its_section(); test_the_xiaomi_row_stands_where_the_plan_runs_it();
    test_the_xiaomi_row_says_how_the_chain_ended();
    test_empty_sections_are_omitted();
    test_a_section_that_folds_away_loses_its_header();
    test_section_of_and_vendor_names();
    test_oppo_svid_note_row();
    test_scp_capability_note_row();
    test_svooc_ceiling_note_row();
    test_long_passive_labels_split_into_words_and_never_cut();
    test_wrap_emits_each_header_as_its_own_line();
    test_note_row_is_appended_as_plain_words();
    test_headers_can_be_left_out();
    test_fixed_pd_chips_carry_advertised_current();
    test_failed_and_skipped_fixed_chips();
    test_fixed_chips_read_ascending_whatever_the_plan_order();
    test_marks_give_each_verdict_its_own_shape();
    test_chip_width_includes_the_mark_cell();
    test_pps_one_coloured_chip_per_range();
    test_a_pps_sweep_merges_into_its_apdos_range_chip();
    test_an_avs_sweep_merges_into_its_apdos_range_chip();
    test_the_merged_chip_is_the_worst_of_its_steps_and_its_sweep();
    test_a_skipped_sweep_leaves_no_mark_and_no_verdict();
    test_the_pd_block_owns_every_pd_step_and_the_sweeps();
    test_a_compact_row_regenerates_the_merged_chip_with_its_mark();
    test_the_sweep_mark_widens_the_chip_by_its_gap_and_glyph();
    test_a_sweep_chip_takes_its_tone_from_the_sweep();
    test_a_fold_only_sweep_is_amber_at_any_depth();
    test_a_fold_is_a_caution_not_a_pass_and_not_a_fail(); test_a_step_walk_miss_is_an_amber_caution_on_the_range_chip(); test_a_transition_check_flag_is_an_amber_caution_on_the_range_chip();
    test_a_deviating_sweep_is_a_caution_never_red();
    test_a_split_sweep_deviation_reads_the_same_on_both_parts();
    test_roll_up_is_one_table();
    test_a_capped_range_chip_is_amber_and_keeps_the_advertised_range();
    test_a_range_with_nothing_passed_stays_red_and_all_skipped_grey();
    test_a_legacy_chip_with_a_pass_and_a_fail_is_amber();
    test_reach_says_what_the_source_delivered();
    test_reach_footer_says_needs_load_not_not_run();
    test_family_footer_words();
    test_family_footer_fits_the_band();
    test_reach_footer_names_the_floor();
    test_reach_footer_of_a_listed_range_says_advertised();
    test_row_reach_picks_the_range_that_is_not_clean();
    test_reach_words_fit_the_footer_next_to_a_label();
    test_the_pd_block_is_one_row_fixed_then_pps_then_avs();
    test_a_source_without_pd_reads_pd_under_no_reply();
    test_no_pd_step_is_left_without_an_owner();
    test_the_widest_pd_block_has_no_cuts();
    test_a_range_not_offered_is_dropped();
    test_cable_limited_epr_rows_fold_instead_of_showing_a_bare_label();
    test_epr_levels_are_chips_with_the_skip_reason();
    test_legacy_chips_collapse_distinct_voltages();
    test_range_positions_are_the_range_chips();
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
    test_default_sweep_rows_fit_without_a_cut();
    test_light_run_chips_read_noted();
    test_vooc_family_reads_as_watts_levels();
    test_a_silent_vooc_family_folds_as_one_word();
    test_a_silent_protocol_folds_as_no_reply_and_a_heard_one_stays_red();
    test_open_dpdm_units_fold_as_one_cause();
    test_scp_chips_and_summary_read_watts();
    test_levels_footer_names_the_top_claim();
    return test_report("test_result_layout");
}
