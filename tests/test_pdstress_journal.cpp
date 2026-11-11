// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress journal: the record round trip, corruption, the loss budget (a sequence is given up after two reboots, the run after four),
// the stale cases (finished, other charger, BACK held), the counts that survive, and the store seam with a fake that can fail.
#include <array>
#include <cstdint>
#include <vector>

#include "k2/pdstress/engine.hpp"
#include "k2/pdstress/journal.hpp"
#include "test_util.h"

using namespace k2::pdstress;
namespace jr = k2::pdstress::journal;
namespace pd = k2::pd;

namespace {

struct FakeStore final : jr::Store {
    std::vector<std::uint8_t> bytes;
    bool have = false, fail_write = false;
    int writes = 0, removes = 0;
    bool read(std::span<std::uint8_t> out, std::size_t& n) override {
        if (!have) return false;
        n = bytes.size() < out.size() ? bytes.size() : out.size();
        for (std::size_t i = 0; i < n; ++i) out[i] = bytes[i];
        return true;
    }
    bool write(std::span<const std::uint8_t> in) override {
        if (fail_write) return false;
        bytes.assign(in.begin(), in.end());
        have = true;
        ++writes;
        return true;
    }
    void remove() override { have = false; bytes.clear(); ++removes; }
};

jr::Image sample() {
    jr::Image im;
    im.opt.mask = 0x07FF;
    im.opt.cycles_pct = 50;
    im.opt.pace = 2;
    im.opt.max_ms = 600000;
    im.opt.max_transitions = 900;
    im.resume.next = 3;
    im.resume.losses = 1;
    im.resume.elapsed_ms = 123456;
    im.resume.seq_losses[3] = 1;
    im.resume.given_up[7] = 1;
    im.next_seq = std::uint8_t(Seq::FixedPps);
    im.caps_crc = 0xBEEF;
    for (std::size_t s = 0; s < kMaxSeqs; ++s)
        for (std::size_t w = 0; w < kCountWords; ++w) im.counts[s][w] = std::uint16_t(s * 100 + w);
    return im;
}

void round_trip() {
    const jr::Image a = sample();
    std::array<std::uint8_t, jr::kMaxBytes> buf{};
    const std::size_t n = jr::encode(a, buf);
    CHECK_COND(n > 0 && n <= jr::kMaxBytes);
    CHECK_EQ(n, jr::kFrameBytes);   // fixed size: one small record per write
    jr::Image b;
    CHECK_COND(jr::decode(std::span<const std::uint8_t>(buf.data(), n), b));
    CHECK_EQ(b.opt.mask, 0x07FF);
    CHECK_EQ(b.opt.cycles_pct, 50);
    CHECK_EQ(b.opt.pace, 2);
    CHECK_EQ(b.opt.max_ms, 600000);
    CHECK_EQ(b.opt.max_transitions, 900);
    CHECK_EQ(b.resume.next, 3);
    CHECK_EQ(b.resume.losses, 1);
    CHECK_EQ(b.resume.elapsed_ms, 123456);
    CHECK_EQ(b.resume.seq_losses[3], 1);
    CHECK_EQ(b.resume.given_up[7], 1);
    CHECK_EQ(b.next_seq, std::uint8_t(Seq::FixedPps));
    CHECK_EQ(b.caps_crc, 0xBEEF);
    CHECK_EQ(b.finished, false);
    for (std::size_t s = 0; s < kMaxSeqs; ++s)
        for (std::size_t w = 0; w < kCountWords; ++w) CHECK_EQ(b.counts[s][w], std::uint16_t(s * 100 + w));
    CHECK_COND(!jr::encode(a, std::span<std::uint8_t>(buf.data(), 10)));   // too small
}

void corruption_is_refused() {
    std::array<std::uint8_t, jr::kMaxBytes> buf{};
    const std::size_t n = jr::encode(sample(), buf);
    jr::Image b;
    for (std::size_t i = 0; i < n; ++i) {   // every single flipped byte is caught (header, payload or CRC)
        auto c = buf;
        c[i] ^= 0x10;
        CHECK_COND(!jr::decode(std::span<const std::uint8_t>(c.data(), n), b));
    }
    CHECK_COND(!jr::decode(std::span<const std::uint8_t>(buf.data(), n - 1), b));   // truncated
    CHECK_COND(!jr::decode(std::span<const std::uint8_t>(buf.data(), 0), b));
    auto v = buf;
    v[2] = 99;   // another version: refused even with a matching CRC
    const std::uint16_t crc = k2::bbstore::crc16(0xFFFF, std::span<const std::uint8_t>(v.data(), n - 2));
    v[n - 2] = std::uint8_t(crc);
    v[n - 1] = std::uint8_t(crc >> 8);
    CHECK_COND(!jr::decode(std::span<const std::uint8_t>(v.data(), n), b));
    v[2] = jr::kVersion;   // the same patch with the right version is accepted again: the CRC recomputation was sound
    const std::uint16_t ok = k2::bbstore::crc16(0xFFFF, std::span<const std::uint8_t>(v.data(), n - 2));
    v[n - 2] = std::uint8_t(ok);
    v[n - 1] = std::uint8_t(ok >> 8);
    CHECK_COND(jr::decode(std::span<const std::uint8_t>(v.data(), n), b));
}

void counts_move_between_record_and_image() {
    std::array<std::int16_t, 640> mem{};
    Record rec(mem);
    rec.clear();
    bump_transition(rec.rec(2));
    bump_transition(rec.rec(2));
    bump_event(rec.rec(2), Event::RejectRx);
    add_sample(rec.rec(2), Metric::Accept, 40);
    bump_event(rec.rec(5), Event::HardResetRx);
    jr::Image im;
    jr::capture_counts(im, rec);
    CHECK_EQ(im.counts[2][0], 2);
    CHECK_EQ(im.counts[2][1 + std::size_t(Event::RejectRx)], 1);
    CHECK_EQ(im.counts[5][1 + std::size_t(Event::HardResetRx)], 1);
    // A fresh record gets the counts back, and the distributions are empty but valid (min at the top: a new sample sets it).
    std::array<std::int16_t, 640> mem2{};
    Record r2(mem2);
    jr::apply_counts(im, r2);
    CHECK_EQ(counts_of(r2.rec(2)).transitions, 2);
    CHECK_EQ(counts_of(r2.rec(2)).get(Event::RejectRx), 1);
    CHECK_EQ(counts_of(r2.rec(5)).get(Event::HardResetRx), 1);
    CHECK_EQ(summary(r2.rec(2), Metric::Accept).n, 0);
    add_sample(r2.rec(2), Metric::Accept, 70);
    CHECK_EQ(summary(r2.rec(2), Metric::Accept).min, 70);
    CHECK_EQ(r2.total(kMaxSeqs).transitions, 2);
}

jr::Image running_at(std::uint8_t next, std::uint8_t seq) {
    jr::Image im;
    im.resume.next = next;
    im.next_seq = seq;
    im.caps_crc = 0x1234;
    im.counts[std::size_t(Seq::Ladder)][0] = 60;   // the first sequence finished with 60 transitions
    return im;
}

void first_loss_retries_the_sequence() {
    jr::Image im = running_at(1, std::uint8_t(Seq::Slam));
    const jr::Verdict v = jr::derive(im, 0x1234, false);
    CHECK_EQ(int(v), int(jr::Verdict::Resume));
    CHECK_EQ(im.resume.losses, 1);
    CHECK_EQ(im.resume.next, 1);   // the sequence that was running is repeated
    CHECK_EQ(im.resume.seq_losses[std::size_t(Seq::Slam)], 1);
    CHECK_EQ(im.resume.given_up[std::size_t(Seq::Slam)], 0);
    // The loss is an event of the sequence it hit: a contract lost.
    CHECK_EQ(im.counts[std::size_t(Seq::Slam)][1 + std::size_t(Event::ContractLost)], 1);
    CHECK_EQ(im.counts[std::size_t(Seq::Ladder)][0], 60);   // finished work is kept
}

void second_loss_in_one_sequence_gives_it_up() {
    jr::Image im = running_at(1, std::uint8_t(Seq::Slam));
    CHECK_EQ(int(jr::derive(im, 0x1234, false)), int(jr::Verdict::Resume));
    CHECK_EQ(int(jr::derive(im, 0x1234, false)), int(jr::Verdict::Resume));
    CHECK_EQ(im.resume.losses, 2);
    CHECK_EQ(im.resume.seq_losses[std::size_t(Seq::Slam)], kMaxSeqLosses);
    CHECK_EQ(im.resume.given_up[std::size_t(Seq::Slam)], 1);
    CHECK_EQ(im.counts[std::size_t(Seq::Slam)][1 + std::size_t(Event::ContractLost)], 2);
    // A different sequence starts with a clean per-sequence budget.
    im.resume.next = 2;
    im.next_seq = std::uint8_t(Seq::SameReq);
    CHECK_EQ(int(jr::derive(im, 0x1234, false)), int(jr::Verdict::Resume));
    CHECK_EQ(im.resume.seq_losses[std::size_t(Seq::SameReq)], 1);
    CHECK_EQ(im.resume.given_up[std::size_t(Seq::SameReq)], 0);
}

void run_budget() {
    jr::Image im = running_at(1, std::uint8_t(Seq::Slam));
    // Four reboots are the budget: the fifth boot ends the run (spread over sequences so none is given up first).
    const Seq order[] = {Seq::Slam, Seq::SameReq, Seq::Reannounce, Seq::FixedPps};
    for (unsigned i = 0; i < kMaxRunLosses - 1; ++i) {
        im.resume.next = std::uint8_t(1 + i);
        im.next_seq = std::uint8_t(order[i]);
        CHECK_EQ(int(jr::derive(im, 0x1234, false)), int(jr::Verdict::Resume));
    }
    CHECK_EQ(im.resume.losses, kMaxRunLosses - 1);
    im.resume.next = 4;
    im.next_seq = std::uint8_t(order[3]);
    CHECK_EQ(int(jr::derive(im, 0x1234, false)), int(jr::Verdict::RunLost));   // the 4th loss spends the budget
    CHECK_EQ(im.resume.losses, kMaxRunLosses);
}

void stale_cases() {
    jr::Image fin = running_at(1, std::uint8_t(Seq::Slam));
    fin.finished = true;
    CHECK_EQ(int(jr::derive(fin, 0x1234, false)), int(jr::Verdict::Stale));
    CHECK_EQ(fin.resume.losses, 0);   // a stale journal is not touched
    jr::Image other = running_at(1, std::uint8_t(Seq::Slam));
    CHECK_EQ(int(jr::derive(other, 0x4321, false)), int(jr::Verdict::Stale));   // another charger: never resume on it
    jr::Image held = running_at(1, std::uint8_t(Seq::Slam));
    CHECK_EQ(int(jr::derive(held, 0x1234, true)), int(jr::Verdict::Stale));     // BACK held at power-up
    // A bad sequence id is stale too (a journal of a newer firmware).
    jr::Image bad = running_at(1, 99);
    CHECK_EQ(int(jr::derive(bad, 0x1234, false)), int(jr::Verdict::Stale));
}

pd::SourceCaps list(std::initializer_list<std::uint32_t> raw) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    c.rx_count = 1;
    for (auto r : raw) c.pdos[c.num_pdos++] = pd::decode_pdo(r);
    return c;
}

void caps_crc_follows_the_list() {
    pd::SourceCaps a = list({0x0001912C, 0x0002D12C, 0x0003C12C});
    pd::SourceCaps b = a;
    CHECK_EQ(jr::caps_crc(a), jr::caps_crc(b));
    b.pdos[2].raw = 0x0003C12D;
    CHECK_COND(jr::caps_crc(a) != jr::caps_crc(b));
    b = a;
    b.num_pdos = 2;
    CHECK_COND(jr::caps_crc(a) != jr::caps_crc(b));
    b = a;
    b.rx_count = 77;   // a re-announce of the same list is the same charger
    CHECK_EQ(jr::caps_crc(a), jr::caps_crc(b));
}

void journal_over_a_store() {
    FakeStore st;
    jr::Journal j(st);
    jr::Image out;
    CHECK_COND(!j.load(out));   // nothing there
    CHECK_COND(j.save(sample()));
    CHECK_EQ(st.writes, 1);
    CHECK_COND(j.load(out));
    CHECK_EQ(out.resume.elapsed_ms, 123456);
    // A broken store file is "no journal", not a crash.
    st.bytes[10] ^= 0xFF;
    CHECK_COND(!j.load(out));
    // A failing write is reported (the run goes on without a journal).
    st.fail_write = true;
    CHECK_COND(!j.save(sample()));
    st.fail_write = false;
    j.clear();
    CHECK_EQ(st.removes, 1);
    CHECK_COND(!j.load(out));
}

// The whole story: boundary writes, a reboot, the derived resume feeds the engine and the finished work is not repeated.
struct Sim : Env {
    pd::SourceCaps c;
    Snapshot snapshot() override { Snapshot s; s.attached = true; s.state = pd::SinkState::Ready; s.pos = 1; s.mv = 5000; return s; }
    const pd::SourceCaps& caps() override { return c; }
    std::uint32_t cap_mv() override { return 36000; }
    bool epr_proof() override { return false; }
    const char* busy() override { return nullptr; }
    pd::Err request(const pd::Choice&) override { return pd::Err::Busy; }
    bool get_caps() override { return false; }
    bool soft_reset() override { return false; }
    void release() override {}
};

void resume_feeds_the_engine() {
    Sim sim;
    sim.c = list({0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C, 0xC1A4323C});   // the RG100 list
    std::array<std::int16_t, 640> mem{};
    Record rec(mem);
    rec.clear();
    // Before the reboot: the ladder finished (60 transitions, one Reject), the next sequence was running.
    for (int i = 0; i < 60; ++i) bump_transition(rec.rec(std::size_t(Seq::Ladder)));
    bump_event(rec.rec(std::size_t(Seq::Ladder)), Event::RejectRx);
    jr::Image im;
    im.opt.mask = seq_bit(Seq::Ladder) | seq_bit(Seq::Slam);
    im.resume.next = 1;
    im.next_seq = std::uint8_t(Seq::Slam);
    im.caps_crc = jr::caps_crc(sim.c);
    jr::capture_counts(im, rec);
    FakeStore st;
    jr::Journal j(st);
    CHECK_COND(j.save(im));
    // Reboot: nothing but the store survives.
    jr::Image back;
    CHECK_COND(j.load(back));
    CHECK_EQ(int(jr::derive(back, jr::caps_crc(sim.c), false)), int(jr::Verdict::Resume));
    std::array<std::int16_t, 640> mem2{};
    Record rec2(mem2);
    jr::apply_counts(back, rec2);
    Engine eng(sim, rec2);
    CHECK_EQ(int(eng.start(back.opt, 1000, &back.resume)), int(StartResult::Ok));
    CHECK_EQ(eng.seq_index(), 1);
    CHECK_COND(eng.seq() == Seq::Slam);
    CHECK_EQ(eng.reboots(), 1);
    CHECK_EQ(counts_of(rec2.rec(std::size_t(Seq::Ladder))).transitions, 60);
    CHECK_EQ(counts_of(rec2.rec(std::size_t(Seq::Ladder))).get(Event::RejectRx), 1);
    CHECK_EQ(counts_of(rec2.rec(std::size_t(Seq::Slam))).get(Event::ContractLost), 1);   // the loss stays on the sequence it hit
}

}  // namespace

int main() {
    round_trip();
    corruption_is_refused();
    counts_move_between_record_and_image();
    first_loss_retries_the_sequence();
    second_loss_in_one_sequence_gives_it_up();
    run_budget();
    stale_cases();
    caps_crc_follows_the_list();
    journal_over_a_store();
    resume_feeds_the_engine();
    return test_report("test_pdstress_journal");
}
