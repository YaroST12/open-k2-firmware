// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress result digest (pdstress/digest.hpp): the header numbers and per-sequence counters a result screen shows, read back
// from a real result blob (result.hpp), and the verdict of each sequence row.
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>

#include "k2/pdstress/digest.hpp"
#include "test_util.h"

using namespace k2::pdstress;

namespace {
std::array<std::int16_t, 640> g_buf{};

struct Src {
    const result::Info* info;
    const Record* rec;
    std::size_t cut = result::kTotalBytes;   // bytes the reader serves (a short blob below this)
};
std::size_t reader(void* ctx, std::size_t off, std::span<std::uint8_t> out) {
    const auto* s = static_cast<const Src*>(ctx);
    if (off >= s->cut) return 0;
    if (out.size() > s->cut - off) out = out.first(s->cut - off);
    return result::read(*s->info, *s->rec, off, out);
}

constexpr std::uint8_t flags(std::uint8_t bits, Na na = Na::None) { return std::uint8_t(bits | static_cast<std::uint8_t>(na) << 4); }

// A run of five picked sequences: Ladder clean, Slam with events, SameReq given up, FixedPps never offered, PpsSteps not reached.
struct Run {
    Record rec{std::span<std::int16_t>(g_buf)};
    result::Info info{};
    Run() {
        rec.clear();
        for (int k = 0; k < 4; ++k) bump_transition(rec.rec(0));
        for (int k = 0; k < 6; ++k) bump_transition(rec.rec(1));
        bump_event(rec.rec(1), Event::WaitRx);
        bump_event(rec.rec(1), Event::WaitRx);
        bump_event(rec.rec(1), Event::HardResetRx);
        info.stop = Stop::Cancelled;
        info.finished = true;
        info.reboots = 2;
        info.n_order = 4;
        info.transitions = 10;
        info.elapsed_ms = 123456;
        info.opt.mask = seq_bit(Seq::Ladder) | seq_bit(Seq::Slam) | seq_bit(Seq::SameReq) | seq_bit(Seq::FixedPps) | seq_bit(Seq::PpsSteps);
        info.opt.cycles_pct = 50;
        info.opt.pace = 1;
        for (auto& f : info.seq_flags) f = flags(result::kSeqSkipped, Na::NotSelected);
        info.seq_flags[0] = flags(result::kSeqDone);
        info.seq_flags[1] = flags(result::kSeqDone);
        info.seq_flags[2] = flags(result::kSeqGivenUp | result::kSeqSkipped);
        info.seq_flags[4] = flags(result::kSeqSkipped, Na::NoPps);
        info.seq_flags[5] = 0;
    }
};

void loads_the_header() {
    Run r;
    Src s{&r.info, &r.rec};
    digest::Digest d;
    CHECK_COND(digest::load(d, reader, &s));
    CHECK_COND(d.valid);
    CHECK_EQ(d.stop, Stop::Cancelled);
    CHECK_EQ(d.reboots, 2);
    CHECK_EQ(d.flags, result::kFinished);
    CHECK_EQ(d.pace, 1);
    CHECK_EQ(d.mask, r.info.opt.mask);
    CHECK_EQ(d.cycles_pct, 50);
    CHECK_EQ(d.transitions, 10);
    CHECK_EQ(d.elapsed_ms, 123456u);
    CHECK_EQ(d.score_cp, score_cp(r.rec.total(kMaxSeqs)));
    CHECK_COND(d.score_cp > 0);
}

void loads_per_sequence_counters() {
    Run r;
    Src s{&r.info, &r.rec};
    digest::Digest d;
    digest::load(d, reader, &s);
    CHECK_EQ(d.counts[0].transitions, 4);
    CHECK_EQ(digest::events(d.counts[0]), 0u);
    CHECK_EQ(d.counts[1].transitions, 6);
    CHECK_EQ(d.counts[1].get(Event::WaitRx), 2);
    CHECK_EQ(d.counts[1].get(Event::HardResetRx), 1);
    CHECK_EQ(digest::events(d.counts[1]), 3u);
}

void verdicts_and_tally() {
    Run r;
    Src s{&r.info, &r.rec};
    digest::Digest d;
    digest::load(d, reader, &s);
    CHECK_EQ(digest::verdict(d, Seq::Ladder), digest::Verdict::Clean);
    CHECK_EQ(digest::verdict(d, Seq::Slam), digest::Verdict::Issues);
    CHECK_EQ(digest::verdict(d, Seq::SameReq), digest::Verdict::Issues);   // given up after reboots
    CHECK_EQ(digest::verdict(d, Seq::Reannounce), digest::Verdict::NotSelected);
    CHECK_EQ(digest::verdict(d, Seq::FixedPps), digest::Verdict::Na);
    CHECK_EQ(digest::na_of(d, Seq::FixedPps), Na::NoPps);
    CHECK_EQ(digest::verdict(d, Seq::PpsSteps), digest::Verdict::NotRun);
    const digest::Tally t = digest::tally(d);
    CHECK_EQ(t.clean, 1u);
    CHECK_EQ(t.issues, 2u);
    CHECK_EQ(t.skip, 2u);
    std::array<Seq, kMaxSeqs> rows{};
    const std::size_t n = digest::rows(d, rows);
    CHECK_EQ(n, 5u);
    CHECK_EQ(rows[0], Seq::Ladder);
    CHECK_EQ(rows[3], Seq::FixedPps);
    CHECK_EQ(rows[4], Seq::PpsSteps);
}

void refuses_short_or_foreign_blobs() {
    Run r;
    Src s{&r.info, &r.rec, result::kTotalBytes - 1};
    digest::Digest d;
    CHECK_COND(!digest::load(d, reader, &s));
    CHECK_COND(!d.valid);
    auto zeros = [](void*, std::size_t, std::span<std::uint8_t> out) -> std::size_t {
        for (auto& b : out) b = 0;
        return out.size();
    };
    CHECK_COND(!digest::load(d, zeros, nullptr));   // version 0
}

}  // namespace

int main() {
    loads_the_header();
    loads_per_sequence_counters();
    verdicts_and_tally();
    refuses_short_or_foreign_blobs();
    return test_report("pdstress_digest");
}
