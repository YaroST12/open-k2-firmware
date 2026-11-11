// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress distributions: the fixed-size record (no per-sample storage), min/median/p95/max per sequence, saturation, and the buffer budget.
#include <array>
#include <cstdint>
#include <string_view>

#include "k2/pdstress/dist.hpp"
#include "test_util.h"

using namespace k2::pdstress;

namespace {

void budget() {
    // The whole run fits the deep sweep's 640-word point buffer (1280 B), so the tool adds no .bss.
    CHECK_COND(kRecordWords <= 640);
    CHECK_EQ(kSeqWords, kCountWords + kMetricCount * kMetricWords);
    CHECK_EQ(kCountWords, 13);
    CHECK_EQ(kMetricWords, 10);
}

void empty_record() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    CHECK_COND(r.ok());
    r.clear();
    for (std::size_t m = 0; m < kMetricCount; ++m) {
        const Summary s = summary(r.rec(0), Metric(m));
        CHECK_EQ(s.n, 0);
        CHECK_EQ(s.max, 0);
    }
    const Counts c = counts_of(r.rec(3));
    CHECK_EQ(c.transitions, 0);
    CHECK_EQ(r.total(kMaxSeqs).transitions, 0);
    std::array<std::int16_t, 100> small{};
    CHECK_COND(!Record(small).ok());
}

void samples() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    r.clear();
    Rec a = r.rec(0);
    for (std::uint32_t v : {30u, 50u, 40u, 45u, 60u}) add_sample(a, Metric::Accept, v);
    Summary s = summary(a, Metric::Accept);
    CHECK_EQ(s.n, 5);
    CHECK_EQ(s.min, 30);
    CHECK_EQ(s.max, 60);
    CHECK_COND(s.med >= s.min && s.med <= s.max);
    CHECK_COND(s.p95 >= s.med && s.p95 <= s.max);
    // The other metrics of the same sequence and the other sequences are untouched.
    CHECK_EQ(summary(a, Metric::PsRdy).n, 0);
    CHECK_EQ(summary(r.rec(1), Metric::Accept).n, 0);
}

void median_and_p95_are_ordered() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    r.clear();
    Rec a = r.rec(2);
    for (std::uint32_t i = 1; i <= 100; ++i) add_sample(a, Metric::PsRdy, i * 10);   // 0.1 ms units: 1 ms .. 100 ms
    const Summary s = summary(a, Metric::PsRdy);
    CHECK_EQ(s.n, 100);
    CHECK_EQ(s.min, 10);
    CHECK_EQ(s.max, 1000);
    CHECK_COND(s.med <= s.p95);
    CHECK_COND(s.p95 <= s.max);
    CHECK_COND(s.med >= s.min);
    // Bin resolution: the median of 1..100 ms sits in the upper half of the range, p95 near the top.
    CHECK_COND(s.p95 >= 500);
}

void single_value() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    r.clear();
    Rec a = r.rec(5);
    add_sample(a, Metric::Over, 120);
    const Summary s = summary(a, Metric::Over);
    CHECK_EQ(s.n, 1);
    CHECK_EQ(s.min, 120);
    CHECK_EQ(s.med, 120);
    CHECK_EQ(s.p95, 120);
    CHECK_EQ(s.max, 120);
}

void saturation() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    r.clear();
    Rec a = r.rec(0);
    add_sample(a, Metric::Band, 1000000);   // clamps to 0xFFFF
    CHECK_EQ(summary(a, Metric::Band).max, 0xFFFF);
    for (int i = 0; i < 3; ++i) bump_transition(a);
    CHECK_EQ(counts_of(a).transitions, 3);
    bump_event(a, Event::WaitRx, 70000);
    CHECK_EQ(counts_of(a).get(Event::WaitRx), 0xFFFF);
    bump_event(a, Event::WaitRx);
    CHECK_EQ(counts_of(a).get(Event::WaitRx), 0xFFFF);
}

void counts_and_total() {
    std::array<std::int16_t, 640> buf{};
    Record r(buf);
    r.clear();
    bump_transition(r.rec(0));
    bump_transition(r.rec(0));
    bump_transition(r.rec(4));
    bump_event(r.rec(0), Event::RejectRx);
    bump_event(r.rec(4), Event::RejectRx);
    bump_event(r.rec(4), Event::HardResetRx);
    const Counts c0 = counts_of(r.rec(0));
    CHECK_EQ(c0.transitions, 2);
    CHECK_EQ(c0.get(Event::RejectRx), 1);
    const Counts t = r.total(kMaxSeqs);
    CHECK_EQ(t.transitions, 3);
    CHECK_EQ(t.get(Event::RejectRx), 2);
    CHECK_EQ(t.get(Event::HardResetRx), 1);
    CHECK_EQ(r.total(1).transitions, 2);
}

void bins() {
    // Bin edges ascend per metric and every value lands in a bin.
    for (std::size_t m = 0; m < kMetricCount; ++m) {
        const auto& e = kEdges[m];
        for (std::size_t i = 1; i < e.size(); ++i) CHECK_COND(e[i] > e[i - 1]);
        CHECK_EQ(bin_of(Metric(m), 0), 0);
        CHECK_EQ(bin_of(Metric(m), 0xFFFF), kBins - 1);
        CHECK_COND(std::string_view(metric_name(Metric(m))) != "?");
        CHECK_COND(std::string_view(metric_unit(Metric(m))) != "?");
    }
}

}  // namespace

int main() {
    budget();
    empty_record();
    samples();
    median_and_p95_are_ordered();
    single_value();
    saturation();
    counts_and_total();
    bins();
    return test_report("test_pdstress_dist");
}
