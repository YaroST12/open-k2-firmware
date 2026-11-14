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

void score_is_weight_times_fails() {
    std::array<Agg, kChecks> agg{};
    CHECK_EQ(int(score(agg)), 0);
    agg[std::size_t(Check::FallTime)].fails = 3;       // weight 2
    agg[std::size_t(Check::HrSafe0V)].fails = 1;       // weight 5
    CHECK_EQ(int(score(agg)), 11);
}

void defs_are_complete() {
    for (const auto& d : kDefs) {
        CHECK_COND(d.key != nullptr && d.label != nullptr);
        CHECK_COND(std::string_view(d.label).size() <= 13);
    }
    CHECK_COND(std::string_view(def(Check::SourceReset).key) == "source_reset");
}

void blob_layout() {
    BlobHead h;
    h.state = 3; h.stop = 1; h.n_trans = 2; h.n_pps = 1; h.score = 0x1234; h.elapsed_s = 300;
    std::array<Agg, kChecks> agg{};
    agg[1].add(Kind::Max, {400000, 0, 0, 325000, 4, 1});
    Timers t;
    t.pps_ka_ms = 13500;
    std::array<TransRec, 2> tr{};
    tr[1].from = 4; tr[1].to = 1; tr[1].band_ms = 900;
    std::array<PpsRec, 1> pp{};
    pp[0].set_mv = 5000;
    const BlobReader r(h, agg, t, tr, pp);
    CHECK_EQ(int(r.size()), int(blob_len(2, 1)));
    std::vector<std::uint8_t> b(r.size());
    CHECK_EQ(int(r.read(0, b)), int(b.size()));
    CHECK_EQ(int(b[0]), int('P'));
    CHECK_EQ(int(b[3]), int('1'));
    CHECK_EQ(int(b[8] | b[9] << 8), 0x1234);
    const std::size_t a1 = kBlobHeader + kAggBytes;
    CHECK_EQ(int(b[a1]), int(Verdict::Fail));
    CHECK_EQ(int(b[a1 + 2]), 4);
    CHECK_EQ(int(b[a1 + 4]), 1);
    CHECK_EQ(int(b[a1 + 8] | b[a1 + 9] << 8 | b[a1 + 10] << 16), 400000);
    const std::size_t to = kBlobHeader + kChecks * kAggBytes;
    CHECK_EQ(int(b[to] | b[to + 1] << 8), 13500);
    const std::size_t r1 = to + kTimerBytes + sizeof(TransRec);
    CHECK_EQ(int(b[r1]), 4);
    CHECK_EQ(int(b[r1 + 1]), 1);
    CHECK_EQ(int(b[r1 + 10] | b[r1 + 11] << 8), 900);
    const std::size_t p0 = to + kTimerBytes + 2 * sizeof(TransRec);
    CHECK_EQ(int(b[p0 + 4] | b[p0 + 5] << 8), 5000);
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
    score_is_weight_times_fails();
    defs_are_complete();
    blob_layout();
    return test_report("test_pdspec_result");
}
