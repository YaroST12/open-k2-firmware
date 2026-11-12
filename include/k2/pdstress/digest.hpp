// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress result digest: what the K2's result screens show of a result blob (result.hpp): the run's header numbers, the
// per-sequence flags and the event counters of every sequence. It is read through a callback in small pieces, so neither the live
// run's result nor a saved one is copied whole into RAM (the blob is ~1.4 KB, the digest ~360 B, the read buffer 32 B).
// The verdict of a sequence row (clean / issues / not run / not applicable) is decided here, once, for the page and the tests.
// Pure code; tests/test_pdstress_digest.cpp.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pdstress/dist.hpp"
#include "k2/pdstress/engine.hpp"
#include "k2/pdstress/plan.hpp"
#include "k2/pdstress/result.hpp"
#include "k2/pdstress/score.hpp"

namespace k2::pdstress::digest {

// Copies blob bytes [off, off + out.size()) into `out`; returns how many it had (0 at or past the end).
using Reader = std::size_t (*)(void* ctx, std::size_t off, std::span<std::uint8_t> out);

struct Digest {
    bool valid = false;
    Stop stop = Stop::None;
    std::uint8_t reboots = 0, flags = 0, pace = 0;
    std::uint16_t mask = 0, cycles_pct = 0, transitions = 0;
    std::uint32_t elapsed_ms = 0, score_cp = 0;
    std::array<std::uint8_t, kMaxSeqs> seq_flags{};   // result::SeqFlag bits, the Na reason in bits 4..7
    std::array<Counts, kMaxSeqs> counts{};
};

// What a sequence row says. NotSelected rows are not shown; Na = the charger never offered what it needs (the reason is
// na_of); NotRun = picked and applicable, but the run ended before it (or its list changed under it).
enum class Verdict : std::uint8_t { Clean, Issues, NotRun, Na, NotSelected };

struct Tally {
    unsigned clean = 0, issues = 0, skip = 0;   // skip = NotRun + Na
};

namespace detail {
inline std::uint16_t u16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | p[1] << 8); }
inline std::uint32_t u32(const std::uint8_t* p) { return u16(p) | std::uint32_t(u16(p + 2)) << 16; }
inline bool read_all(Reader rd, void* ctx, std::size_t off, std::span<std::uint8_t> out) { return rd(ctx, off, out) == out.size(); }
}  // namespace detail

// Fills `d` from the blob `rd` serves. False (d.valid false) when the blob is short (any byte up to kTotalBytes missing) or of
// another layout.
[[gnu::noinline]] inline bool load(Digest& d, Reader rd, void* ctx) {
    d = Digest{};
    std::array<std::uint8_t, 32> b{};
    if (!detail::read_all(rd, ctx, 0, b)) return false;
    if (b[0] != result::kVersion || b[2] != kSeqCount || detail::u16(&b[30]) != kSeqWords) return false;
    d.stop = static_cast<Stop>(b[1]);
    d.reboots = b[4];
    d.flags = b[5];
    d.pace = b[6];
    d.mask = detail::u16(&b[8]);
    d.cycles_pct = detail::u16(&b[10]);
    d.transitions = detail::u16(&b[14]);
    d.elapsed_ms = detail::u32(&b[20]);
    d.score_cp = detail::u32(&b[24]);
    if (!detail::read_all(rd, ctx, result::kFlagsOff, std::span<std::uint8_t>(d.seq_flags))) return false;
    constexpr std::size_t kCountBytes = kCountWords * 2;
    static_assert(kCountBytes <= sizeof b, "one sequence's counters fit the read buffer");
    for (std::size_t q = 0; q < kSeqCount; ++q) {
        const auto chunk = std::span<std::uint8_t>(b).first(kCountBytes);
        if (!detail::read_all(rd, ctx, result::kHeaderBytes + q * kSeqWords * 2, chunk)) return false;
        Counts& c = d.counts[q];
        c.transitions = detail::u16(&b[0]);
        for (std::size_t e = 0; e < kEventCount; ++e) c.n[e] = detail::u16(&b[2 + 2 * e]);
    }
    if (!detail::read_all(rd, ctx, result::kTotalBytes - 1, std::span<std::uint8_t>(b).first(1))) return false;   // the whole blob is there
    d.valid = true;
    return true;
}

// Every event of a sequence, saturated at 0xFFFF.
constexpr std::uint32_t events(const Counts& c) {
    std::uint32_t n = 0;
    for (const auto v : c.n) n += v;
    return n > 0xFFFF ? 0xFFFF : n;
}

constexpr Na na_of(const Digest& d, Seq s) { return static_cast<Na>(d.seq_flags[static_cast<std::size_t>(s)] >> 4); }

constexpr Verdict verdict(const Digest& d, Seq s) {
    const std::size_t i = static_cast<std::size_t>(s);
    const std::uint8_t f = d.seq_flags[i];
    const Na na = na_of(d, s);
    if (na == Na::NotSelected) return Verdict::NotSelected;
    if (na != Na::None) return Verdict::Na;
    if (f & result::kSeqGivenUp) return Verdict::Issues;   // the K2 rebooted in it more than once: never clean
    if (!(f & result::kSeqDone)) return Verdict::NotRun;
    return events(d.counts[i]) ? Verdict::Issues : Verdict::Clean;
}

constexpr Tally tally(const Digest& d) {
    Tally t;
    for (std::size_t i = 0; i < kSeqCount; ++i) {
        switch (verdict(d, static_cast<Seq>(i))) {
            case Verdict::Clean: ++t.clean; break;
            case Verdict::Issues: ++t.issues; break;
            case Verdict::NotRun:
            case Verdict::Na: ++t.skip; break;
            case Verdict::NotSelected: break;
        }
    }
    return t;
}

// The sequences a result shows a row for (every picked one, in id order); returns the count.
constexpr std::size_t rows(const Digest& d, std::span<Seq, kMaxSeqs> out) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kSeqCount; ++i)
        if (verdict(d, static_cast<Seq>(i)) != Verdict::NotSelected) out[n++] = static_cast<Seq>(i);
    return n;
}

}  // namespace k2::pdstress::digest
