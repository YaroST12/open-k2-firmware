// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check result (k2/pdspec/result.hpp): the judging rule with the sample bracket, the aggregate, the score and the blob layout.
#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "k2/pdspec/result.hpp"
#include "test_util.h"

using namespace k2::pdspec;

namespace {

void bracket_counts_in_the_measurements_favour() {
    // Accept -> PS_RDY limit 325 ms: 330 ms seen with a 6 ms gap before it may have been 324.x ms: not a fail; with a 4 ms gap it is.
    CHECK_COND(passes(Kind::Max, {330000, 6000, 0, 325000}));
    CHECK_COND(!passes(Kind::Max, {330000, 4000, 0, 325000}));
    CHECK_EQ(excess(Kind::Max, {330000, 4000, 0, 325000}), 1000);
    // A minimum (tSrcTransition 25 ms): the departure seen at 26 ms is at most 26 ms: passes; at 24 ms it certainly started early.
    CHECK_COND(passes(Kind::Min, {26000, 3000, 25000, 0}));
    CHECK_COND(!passes(Kind::Min, {24000, 3000, 25000, 0}));
    // A range: both edges.
    CHECK_COND(passes(Kind::Range, {13000, 5, 12000, 15000}));
    CHECK_COND(!passes(Kind::Range, {11000, 5, 12000, 15000}));
    CHECK_COND(!passes(Kind::Range, {15100, 5, 12000, 15000}));
}

void aggregate_keeps_the_worst() {
    Agg a;
    CHECK_COND(a.verdict() == Verdict::Skip);
    a.add(Kind::Max, {200000, 1000, 0, 325000, 1, 2});
    a.add(Kind::Max, {300000, 1000, 0, 325000, 2, 3});   // closest to the limit while all pass
    CHECK_COND(a.verdict() == Verdict::Pass);
    CHECK_EQ(a.worst, 300000);
    CHECK_EQ(int(a.a), 2);
    a.add(Kind::Max, {400000, 1000, 0, 325000, 3, 1});
    a.add(Kind::Max, {340000, 1000, 0, 325000, 4, 1});
    CHECK_COND(a.verdict() == Verdict::Fail);
    CHECK_EQ(int(a.n), 4);
    CHECK_EQ(int(a.fails), 2);
    CHECK_EQ(a.worst, 400000);
    CHECK_EQ(int(a.a), 3);
    CHECK_EQ(a.hi, 325000);
}

// An Accept from the ms fallback, 20 ms with a 5 ms bracket against 17 ms, passes on its bracket: it is kept at the limit, so the
// worst never reads over a limit the verdict passed (hardware 2026-11-19); a clean 16 ms later is not worse than it.
void a_pass_on_its_bracket_is_kept_at_its_limit() {
    Agg a;
    a.add(Kind::Max, {12000, 0, 0, 17000, 1, 1});
    a.add(Kind::Max, {20000, 5000, 0, 17000, 2, 2});
    CHECK_COND(a.verdict() == Verdict::Pass);
    CHECK_EQ(a.worst, 17000);
    CHECK_EQ(int(a.a), 2);
    a.add(Kind::Max, {16000, 0, 0, 17000, 3, 3});
    CHECK_EQ(int(a.a), 2);
    a.add(Kind::Max, {23000, 5000, 0, 17000, 4, 4});   // over even at its low end
    CHECK_COND(a.verdict() == Verdict::Fail);
    CHECK_EQ(a.worst, 23000);
}

void score_is_weight_times_fails() {
    std::array<Agg, kChecks> agg{};
    CHECK_EQ(int(score(agg)), 0);
    agg[std::size_t(Check::FallTime)].fails = 3;       // weight 2
    agg[std::size_t(Check::HrSafe0V)].fails = 1;       // weight 5
    CHECK_EQ(int(score(agg)), 11);
}

// A check whose limit is inferred (no figure in the specification behind it) is reported, never judged: the measured value and the
// count over the inferred limit stay in the result, the verdict reads "unverified" and it adds nothing to the score.
void inferred_limits_are_unverified() {
    // Since the Type-C rows came in (docs/PD_TYPEC_BC12_SPEC_REFERENCE.md) the detach pair is judged; the 20 mV PPS settle (under the
    // meter's resolution) is the one unverified check left.
    CHECK_COND(def(Check::PpsSettle).inferred);
    for (std::size_t k = 0; k < kChecks; ++k) CHECK_COND(def(Check(k)).inferred == (Check(k) == Check::PpsSettle));
    CHECK_COND(std::string_view(verdict_name(Verdict::Unverified)) == "unverified");
    Agg slow;
    slow.add(Kind::Max, {40000, 1000, 0, 25000});
    CHECK_COND(verdict(Check::PpsSettle, slow) == Verdict::Unverified);
    CHECK_EQ(int(slow.fails), 1);   // the count over the inferred limit is kept
    CHECK_COND(verdict(Check::PpsBand, slow) == Verdict::Fail);
    CHECK_COND(verdict(Check::PpsSettle, Agg{}) == Verdict::Skip);   // nothing measured is still a skip
    std::array<Agg, kChecks> agg{};
    agg[std::size_t(Check::PpsSettle)].fails = 2;      // weight 1, unverified
    CHECK_EQ(int(score(agg)), 0);
    agg[std::size_t(Check::HrSafe0V)].fails = 1;       // weight 5, sourced
    CHECK_EQ(int(score(agg)), 5);
    // The blob carries the verdict the report shows.
    std::array<Agg, kChecks> b_agg{};
    b_agg[std::size_t(Check::PpsSettle)] = slow;
    BlobHead h;
    Timers t;
    const BlobReader r(h, b_agg, t, {}, {});
    std::array<std::uint8_t, 1> v{};
    CHECK_EQ(int(r.read(kBlobHeader + std::size_t(Check::PpsSettle) * kAggBytes, v)), 1);
    CHECK_EQ(int(v[0]), int(Verdict::Unverified));
}

void defs_are_complete() {
    for (const auto& d : kDefs) {
        CHECK_COND(d.key != nullptr && d.label != nullptr);
        CHECK_COND(std::string_view(d.label).size() <= 13);
    }
    CHECK_COND(std::string_view(def(Check::SourceReset).key) == "source_reset");
    // PSC1 readers see the first 21 checks in their old order; the new ones are appended.
    CHECK_EQ(int(Check::SourceReset) + 1, int(kChecksPsc1));
    CHECK_COND(std::string_view(def(Check::ReattachCaps).key) == "reattach_caps");
    // The verified limits are judged: the detach pair (Type-C) and the programmable band / PS_RDY; the 20 mV settle stays unverified.
    for (const Check c : {Check::DetachSafe0V, Check::Reattach, Check::PpsBand, Check::PpsPsRdy, Check::PpsStatus, Check::AvsBand})
        CHECK_COND(!def(c).inferred);
    CHECK_COND(def(Check::PpsSettle).inferred);
    CHECK_COND(avs_check(Check::AvsTrack) && !avs_check(Check::PpsTrack));
    for (std::size_t k = 0; k < kChecks; ++k) CHECK_COND(avs_check(Check(k)) == (std::string_view(kDefs[k].key).substr(0, 4) == "avs_"));
}

void blob_layout() {
    BlobHead h;
    h.state = 3; h.stop = 1; h.n_trans = 2; h.n_pass = 1; h.score = 0x1234; h.elapsed_s = 300;
    h.caps_note = CapsNote::PpsNoAvs; h.opts = kOptFull20;
    std::array<Agg, kChecks> agg{};
    agg[1].add(Kind::Max, {400000, 0, 0, 325000, 4, 1});
    Timers t;
    t.pps_ka_ms = 13500;
    std::array<TransRec, 2> tr{};
    tr[1].from = 4; tr[1].to = 1; tr[1].band_ms = 900;
    std::array<PassRec, 1> pp{};
    pp[0].pos = 5; pp[0].cls = 2; pp[0].steps = 770; pp[0].lo_mv = 3300; pp[0].status = kPsOmf; pp[0].track = 1;
    const BlobReader r(h, agg, t, tr, pp);
    CHECK_EQ(int(r.size()), int(blob_len(2, 1)));
    std::vector<std::uint8_t> b(r.size());
    CHECK_EQ(int(r.read(0, b)), int(b.size()));
    CHECK_EQ(int(b[0]), int('P'));
    CHECK_EQ(int(b[3]), int('2'));
    CHECK_EQ(int(b[7]), 1);
    CHECK_EQ(int(b[8] | b[9] << 8), 0x1234);
    CHECK_EQ(int(b[13]), int(kChecks));
    CHECK_EQ(int(b[14]), int(CapsNote::PpsNoAvs));
    CHECK_EQ(int(b[15]), int(kOptFull20));
    const std::size_t a1 = kBlobHeader + kAggBytes;
    CHECK_EQ(int(b[a1]), int(Verdict::Fail));
    CHECK_EQ(int(b[a1 + 2]), 4);
    CHECK_EQ(int(b[a1 + 4]), 1);
    CHECK_EQ(int(b[a1 + 8] | b[a1 + 9] << 8 | b[a1 + 10] << 16), 400000);
    const std::size_t to = kBlobHeader + kChecks * kAggBytes;
    CHECK_EQ(int(b[to] | b[to + 1] << 8), 13500);
    const std::size_t r1 = to + kTimerBytes + kTransBytes;
    CHECK_EQ(int(b[r1]), 4);
    CHECK_EQ(int(b[r1 + 1]), 1);
    CHECK_EQ(int(b[r1 + 10] | b[r1 + 11] << 8), 900);
    const std::size_t p0 = to + kTimerBytes + 2 * kTransBytes;
    CHECK_EQ(int(b[p0]), 5);
    CHECK_EQ(int(b[p0 + 1]), 2);
    CHECK_EQ(int(b[p0 + 2] | b[p0 + 3] << 8), 770);
    CHECK_EQ(int(b[p0 + 4] | b[p0 + 5] << 8), 3300);
    CHECK_EQ(int(b[p0 + 26]), int(kPsOmf));
    CHECK_EQ(int(b[p0 + 27]), 1);   // the fold / track break flag
    CHECK_EQ(int(b.size()), int(p0 + sizeof(PassRec)));
    // Chunked reads give the same bytes.
    std::array<std::uint8_t, 48> c{};
    CHECK_EQ(int(r.read(48, c)), 48);
    for (std::size_t i = 0; i < 48; ++i) CHECK_EQ(int(c[i]), int(b[48 + i]));
    CHECK_EQ(int(r.read(r.size(), c)), 0);
}

}  // namespace

int main() {
    bracket_counts_in_the_measurements_favour();
    aggregate_keeps_the_worst();
    a_pass_on_its_bracket_is_kept_at_its_limit();
    score_is_weight_times_fails();
    inferred_limits_are_unverified();
    defs_are_complete();
    blob_layout();
    return test_report("test_pdspec_result");
}
