// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress record: the per-sequence numbers kept in a flat int16 array, no heap and no per-sample storage. One sequence's record is
// kSeqWords words: the transition count, one counter per Event, and for each of four metrics the exact min and max plus an 8-bin
// histogram. Median and p95 are read off the histogram (the upper edge of the bin the rank falls in, clamped to [min, max]), so they are
// bin-resolution figures; min and max are exact. Pure code (tests/test_pdstress_dist.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pdstress/score.hpp"

namespace k2::pdstress {

// What is measured per transition.
enum class Metric : std::uint8_t {
    Accept,  // Request TX_START -> Accept RX edge, units of 0.1 ms
    PsRdy,   // Accept RX edge -> PS_RDY RX edge, units of 0.1 ms
    Band,    // Request TX_START -> VBUS first inside the +-5 % band of the target, ms
    Over,    // largest |VBUS - target| from the first in-band sample to the end of the watch window, mV
    Count
};
inline constexpr std::size_t kMetricCount = static_cast<std::size_t>(Metric::Count);
inline constexpr std::size_t kBins = 8;

constexpr const char* metric_name(Metric m) {
    switch (m) {
        case Metric::Accept: return "accept";
        case Metric::PsRdy: return "psrdy";
        case Metric::Band: return "band";
        case Metric::Over: return "over";
        case Metric::Count: break;
    }
    return "?";
}
constexpr const char* metric_unit(Metric m) {
    switch (m) {
        case Metric::Accept:
        case Metric::PsRdy: return "0.1ms";
        case Metric::Band: return "ms";
        case Metric::Over: return "mV";
        case Metric::Count: break;
    }
    return "?";
}

// Upper edges of bins 0..6 (inclusive); bin 7 is everything above. The edges sit on the spec limits that matter for the metric.
inline constexpr std::array<std::array<std::uint16_t, kBins - 1>, kMetricCount> kEdges = {{
    {50, 100, 200, 300, 400, 520, 1000},         // Accept: 5, 10, 20, 30, 40, 52 (the limit), 100 ms
    {500, 1000, 2000, 3000, 4500, 5500, 9200},   // PsRdy: 50, 100, 200, 300, 450, 550 (SPR limit), 920 (EPR limit) ms
    {50, 100, 200, 300, 450, 600, 1000},         // Band: ms
    {10, 25, 50, 100, 250, 500, 1000},           // Over: mV
}};

inline constexpr std::size_t kMetricWords = 2 + kBins;                          // min, max, bins
inline constexpr std::size_t kCountWords = 1 + kEventCount;                     // transitions, events
inline constexpr std::size_t kSeqWords = kCountWords + kMetricCount * kMetricWords;
inline constexpr std::size_t kMaxSeqs = 12;
inline constexpr std::size_t kRecordWords = kMaxSeqs * kSeqWords;               // the whole run, must fit the reused 640-word buffer
static_assert(kRecordWords <= 640, "the PD Stress record reuses the deep-sweep point buffer (ctsweep::kMaxPoints words)");

// A sequence record is a span of kSeqWords int16 words; the accessors treat them as unsigned 16-bit.
using Rec = std::span<std::int16_t, kSeqWords>;
using CRec = std::span<const std::int16_t, kSeqWords>;

inline std::uint16_t word(CRec r, std::size_t i) { return static_cast<std::uint16_t>(r[i]); }
inline void put(Rec r, std::size_t i, std::uint32_t v) { r[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(v > 0xFFFF ? 0xFFFF : v)); }

constexpr std::size_t metric_base(Metric m) { return kCountWords + static_cast<std::size_t>(m) * kMetricWords; }

inline void reset(Rec r) {
    for (auto& w : r) w = 0;
    for (std::size_t m = 0; m < kMetricCount; ++m) put(r, metric_base(Metric(m)), 0xFFFF);   // min starts at the top
}

inline Counts counts_of(CRec r) {
    Counts c;
    c.transitions = word(r, 0);
    for (std::size_t i = 0; i < kEventCount; ++i) c.n[i] = word(r, 1 + i);
    return c;
}

inline void bump_transition(Rec r) { if (word(r, 0) != 0xFFFF) put(r, 0, word(r, 0) + 1u); }
inline void bump_event(Rec r, Event e, std::uint32_t by = 1) {
    const std::size_t i = 1 + static_cast<std::size_t>(e);
    put(r, i, std::uint32_t(word(r, i)) + by);
}

constexpr std::size_t bin_of(Metric m, std::uint32_t v) {
    const auto& e = kEdges[static_cast<std::size_t>(m)];
    for (std::size_t b = 0; b < e.size(); ++b)
        if (v <= e[b]) return b;
    return kBins - 1;
}

inline void add_sample(Rec r, Metric m, std::uint32_t v) {
    const std::size_t base = metric_base(m);
    const std::uint32_t s = v > 0xFFFF ? 0xFFFF : v;
    if (s < word(r, base)) put(r, base, s);
    if (s > word(r, base + 1)) put(r, base + 1, s);
    const std::size_t bi = base + 2 + bin_of(m, s);
    if (word(r, bi) != 0xFFFF) put(r, bi, word(r, bi) + 1u);
}

struct Summary {
    std::uint16_t n = 0, min = 0, med = 0, p95 = 0, max = 0;   // all 0 when n == 0
};

inline Summary summary(CRec r, Metric m) {
    Summary s;
    const std::size_t base = metric_base(m);
    std::uint32_t n = 0;
    for (std::size_t b = 0; b < kBins; ++b) n += word(r, base + 2 + b);
    if (n == 0) return s;
    s.n = std::uint16_t(n > 0xFFFF ? 0xFFFF : n);
    s.min = word(r, base);
    s.max = word(r, base + 1);
    const auto& e = kEdges[static_cast<std::size_t>(m)];
    auto at_rank = [&](std::uint32_t rank) {
        std::uint32_t cum = 0;
        for (std::size_t b = 0; b < kBins; ++b) {
            cum += word(r, base + 2 + b);
            if (cum >= rank) {
                std::uint32_t v = b < e.size() ? e[b] : s.max;
                if (v > s.max) v = s.max;
                if (v < s.min) v = s.min;
                return std::uint16_t(v);
            }
        }
        return s.max;
    };
    s.med = at_rank((n + 1) / 2);
    s.p95 = at_rank((95 * n + 99) / 100);
    return s;
}

// A run's records: one Rec per sequence slot inside a caller-owned int16 buffer.
class Record {
public:
    explicit Record(std::span<std::int16_t> buf) : buf_(buf) {}
    bool ok() const { return buf_.size() >= kRecordWords; }
    void clear() { for (std::size_t i = 0; i < kMaxSeqs; ++i) reset(rec(i)); }
    Rec rec(std::size_t seq) { return Rec(buf_.subspan(seq * kSeqWords, kSeqWords)); }
    CRec rec(std::size_t seq) const { return CRec(buf_.subspan(seq * kSeqWords, kSeqWords)); }
    // The raw words of the whole run (the result blob streams them).
    std::span<const std::int16_t> words() const { return buf_.first(kRecordWords); }
    // The run as one set of counters.
    Counts total(std::size_t n_seqs) const {
        Counts t;
        for (std::size_t i = 0; i < n_seqs && i < kMaxSeqs; ++i) t.add(counts_of(rec(i)));
        return t;
    }

private:
    std::span<std::int16_t> buf_;
};

}  // namespace k2::pdstress
