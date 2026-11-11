// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress planner: what runs on which Source_Capabilities, the length and the steps of every sequence, and the standing rule that nothing
// is ever planned outside the advertised levels (fixed positions, APDO windows, current, the monitor ceiling).
#include <string_view>

#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pdstress/plan.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdstress;

namespace {

pd::SourceCaps from_raw(std::initializer_list<std::uint32_t> raw) {
    pd::SourceCaps c;
    c.valid = true;
    c.spec_rev = 2;
    for (auto r : raw) c.pdos[c.num_pdos++] = pd::decode_pdo(r);
    return c;
}

// NOVOO RG100: 5/9/15/20 V 3 A + PPS 5-21 V 3 A.
pd::SourceCaps rg100() { return from_raw({0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C, 0xC1A4323C}); }
// 5/9/15/20 V + PPS 3.3-11 V 3 A + PPS 3.3-21 V 5 A (the PPS window starts below 5 V).
pd::SourceCaps lowpps() { return from_raw({0x0001912C, 0x0002D12C, 0x0004B12C, 0x000640C8, 0xC0DC2164, 0xC1A4213C}); }
// 5 V 3 A only.
pd::SourceCaps only5() { return from_raw({0x0001912C}); }
// 5/9/15/20 V + 28 V EPR fixed at position 8 (positions 5..7 empty are not modelled: the list is packed, so use the real layout).
pd::SourceCaps with_epr() {
    pd::SourceCaps c = from_raw({0x0001912C, 0x0002D12C, 0x0004B12C, 0x0006412C});
    for (int i = c.num_pdos; i < 7; i++) c.pdos[i] = pd::Pdo{};
    c.num_pdos = 8;
    c.pdos[7] = pd::decode_pdo((560u << 10) | 500u);   // 28 V 5 A (EPR fixed)
    return c;
}

void scan_levels() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    CHECK_EQ(L.n_fixed, 4);
    CHECK_EQ(L.fixed[0], 1);
    CHECK_EQ(L.fixed[3], 4);
    CHECK_EQ(L.top, 4);
    CHECK_EQ(L.pps, 5);
    CHECK_EQ(L.avs, 0);
    CHECK_EQ(L.epr28, 0);
    // A lower ceiling drops the levels above it.
    const Levels L12 = scan(c, 12000);
    CHECK_EQ(L12.n_fixed, 2);
    CHECK_EQ(L12.top, 2);
    // No valid list: nothing.
    const Levels none = scan(pd::SourceCaps{}, 36000);
    CHECK_EQ(none.n_fixed, 0);
    const Levels e = scan(with_epr(), 36000);
    CHECK_EQ(e.epr28, 8);
    CHECK_EQ(e.n_fixed, 4);
}

void applicability_rg100() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    for (Seq s : {Seq::Ladder, Seq::Slam, Seq::SameReq, Seq::Reannounce, Seq::FixedPps, Seq::PpsSteps, Seq::CurrentOnly, Seq::SoftReset, Seq::Mismatch})
        CHECK_EQ(int(not_applicable(s, c, L, false, false)), int(Na::None));
    CHECK_EQ(int(not_applicable(Seq::AvsSteps, c, L, false, false)), int(Na::NoAvs));
    CHECK_EQ(int(not_applicable(Seq::Epr, c, L, true, false)), int(Na::NoEpr));
    // PPS 5-21 V starts at 5 V: nothing below 5 V to probe.
    CHECK_EQ(int(not_applicable(Seq::PpsLow, c, L, false, false)), int(Na::PpsAbove5V));
}

void applicability_other() {
    {
        const auto c = lowpps();
        const Levels L = scan(c, 36000);
        CHECK_EQ(int(not_applicable(Seq::PpsLow, c, L, false, false)), int(Na::None));
        CHECK_EQ(int(not_applicable(Seq::PpsLow, c, L, false, true)), int(Na::SelfPowered));   // a K2 on the charger would brown out
    }
    {
        const auto c = only5();
        const Levels L = scan(c, 36000);
        CHECK_EQ(int(not_applicable(Seq::Ladder, c, L, false, false)), int(Na::NoLevels));
        CHECK_EQ(int(not_applicable(Seq::Slam, c, L, false, false)), int(Na::NoLevels));
        CHECK_EQ(int(not_applicable(Seq::FixedPps, c, L, false, false)), int(Na::NoPps));
        CHECK_EQ(int(not_applicable(Seq::Reannounce, c, L, false, false)), int(Na::None));
        CHECK_EQ(int(not_applicable(Seq::SoftReset, c, L, false, false)), int(Na::None));
    }
    {
        const auto c = with_epr();
        const Levels L = scan(c, 36000);
        CHECK_EQ(int(not_applicable(Seq::Epr, c, L, false, false)), int(Na::NoEprProof));
        CHECK_EQ(int(not_applicable(Seq::Epr, c, L, true, false)), int(Na::None));
        const Levels L24 = scan(c, 24000);   // 28 V above the ceiling
        CHECK_EQ(int(not_applicable(Seq::Epr, c, L24, true, false)), int(Na::NoEpr));
    }
    for (std::size_t i = 0; i < 10; ++i) CHECK_COND(std::string_view(na_name(Na(i))) != "");
}

void default_mask() {
    CHECK_COND(!(kDefaultSeqMask & seq_bit(Seq::Epr)));   // P9 is opt-in
    CHECK_COND(kDefaultSeqMask & seq_bit(Seq::Ladder));
    CHECK_COND(kDefaultSeqMask & seq_bit(Seq::SoftReset));
    CHECK_COND(kDefaultSeqMask & seq_bit(Seq::PpsLow));
    CHECK_COND(kDefaultSeqMask & seq_bit(Seq::Mismatch));
    CHECK_COND(kSeqs[std::size_t(Seq::Epr)].opt_in);
    CHECK_EQ(kAllSeqMask, (1u << kSeqCount) - 1);
}

void cycles_scale() {
    CHECK_EQ(scaled_cycles(Seq::Ladder, 100), 10);
    CHECK_EQ(scaled_cycles(Seq::Ladder, 50), 5);
    CHECK_EQ(scaled_cycles(Seq::Ladder, 10), 1);
    CHECK_EQ(scaled_cycles(Seq::Ladder, 1), 1);    // never zero
    CHECK_EQ(scaled_cycles(Seq::SameReq, 200), 100);
    CHECK_EQ(scaled_cycles(Seq::PpsSteps, 100), 50);
}

void lengths_rg100() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    CHECK_EQ(length(Seq::Ladder, c, L, 10, false, false), 60);        // 10 cycles x up 3 + down 3
    CHECK_EQ(length(Seq::Slam, c, L, 40, false, false), 80);
    CHECK_EQ(length(Seq::SameReq, c, L, 50, false, false), 102);      // (1 move + 50 repeats) at 9 V and at 20 V
    CHECK_EQ(length(Seq::Reannounce, c, L, 20, false, false), 20);
    CHECK_EQ(length(Seq::FixedPps, c, L, 10, false, false), 20);
    CHECK_EQ(length(Seq::PpsSteps, c, L, 50, false, false), 401);     // the initial move + 4 sizes x 50 cycles x up and down
    CHECK_EQ(length(Seq::CurrentOnly, c, L, 10, false, false), 20);
    CHECK_EQ(length(Seq::SoftReset, c, L, 10, false, false), 10);
    CHECK_EQ(length(Seq::Mismatch, c, L, 10, false, false), 20);
    CHECK_EQ(length(Seq::AvsSteps, c, L, 30, false, false), 0);       // not applicable: zero steps
    CHECK_EQ(length(Seq::PpsLow, c, L, 10, false, false), 0);
    CHECK_EQ(length(Seq::Epr, c, L, 5, true, false), 0);
}

// Every step of every sequence stays inside what the source advertised, on every caps list.
void never_outside_the_list() {
    const pd::SourceCaps lists[] = {rg100(), lowpps(), only5(), with_epr()};
    for (const auto& c : lists) {
        const Levels L = scan(c, 36000);
        for (std::size_t si = 0; si < kSeqCount; ++si) {
            const Seq s = Seq(si);
            const std::uint16_t cyc = 3;
            const std::uint16_t n = length(s, c, L, cyc, true, false);
            for (std::uint16_t i = 0; i < n; ++i) {
                const auto st = step(s, i, c, L, cyc, true, false);
                CHECK_COND(st.has_value());
                if (!st) continue;
                if (st->kind != StepKind::Request) continue;
                const pd::Plan p = pd::plan_request(c, st->choice, 36000, false);
                CHECK_EQ(int(p.err), int(pd::Err::Ok));
                const pd::Pdo& d = c.pdos[st->choice.pos - 1];
                if (d.type == pd::PdoType::Fixed) {
                    CHECK_COND(d.max_mv <= 36000);
                    CHECK_EQ(st->target_mv, d.max_mv);
                    CHECK_COND(p.ma <= d.max_ma);
                } else {
                    CHECK_COND(st->target_mv >= d.min_mv && st->target_mv <= d.max_mv);
                    CHECK_COND(st->target_mv <= 36000);
                    CHECK_EQ(p.mv, st->target_mv);   // the sink would not clamp it
                    CHECK_COND(p.ma <= d.max_ma);
                    if (d.type == pd::PdoType::Pps) CHECK_EQ(st->target_mv % 20, 0);
                    if (d.type == pd::PdoType::Avs) CHECK_EQ(st->target_mv % 100, 0);
                }
                CHECK_COND(!st->choice.hunt);   // never more than advertised
            }
            CHECK_COND(!step(s, n, c, L, cyc, true, false).has_value());   // nothing past the end
        }
    }
}

void ladder_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    // From 5 V: 9, 15, 20, then back 15, 9, 5 V.
    const std::uint8_t want[] = {2, 3, 4, 3, 2, 1, 2, 3, 4};
    for (int i = 0; i < 9; ++i) {
        const auto st = step(Seq::Ladder, std::uint16_t(i), c, L, 10, false, false);
        CHECK_COND(st.has_value());
        if (st) CHECK_EQ(st->choice.pos, want[i]);
        if (st) CHECK_EQ(int(st->pace), int(Pace::Settled));
    }
}

void slam_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    const auto a = step(Seq::Slam, 0, c, L, 40, false, false);
    const auto b = step(Seq::Slam, 1, c, L, 40, false, false);
    CHECK_EQ(a->choice.pos, 4);
    CHECK_EQ(b->choice.pos, 1);
    // First half settled, second half ready-paced.
    CHECK_EQ(int(step(Seq::Slam, 39, c, L, 40, false, false)->pace), int(Pace::Settled));
    CHECK_EQ(int(step(Seq::Slam, 40, c, L, 40, false, false)->pace), int(Pace::Ready));
    CHECK_EQ(int(step(Seq::Slam, 79, c, L, 40, false, false)->pace), int(Pace::Ready));
}

void same_request_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    // Block 1: 9 V (position 2); block 2: the top level. The first of each block is the settled move, the rest repeat it ready-paced.
    CHECK_EQ(step(Seq::SameReq, 0, c, L, 50, false, false)->choice.pos, 2);
    CHECK_EQ(int(step(Seq::SameReq, 0, c, L, 50, false, false)->pace), int(Pace::Settled));
    CHECK_EQ(int(step(Seq::SameReq, 1, c, L, 50, false, false)->pace), int(Pace::Ready));
    CHECK_EQ(step(Seq::SameReq, 50, c, L, 50, false, false)->choice.pos, 2);
    CHECK_EQ(step(Seq::SameReq, 51, c, L, 50, false, false)->choice.pos, 4);
    CHECK_EQ(int(step(Seq::SameReq, 51, c, L, 50, false, false)->pace), int(Pace::Settled));
    CHECK_EQ(step(Seq::SameReq, 101, c, L, 50, false, false)->choice.pos, 4);
}

void kind_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    const auto g = step(Seq::Reannounce, 0, c, L, 20, false, false);
    CHECK_EQ(int(g->kind), int(StepKind::GetCaps));
    CHECK_EQ(int(g->pace), int(Pace::Spaced));
    const auto r = step(Seq::SoftReset, 0, c, L, 10, false, false);
    CHECK_EQ(int(r->kind), int(StepKind::SoftReset));
    const auto e = step(Seq::Epr, 0, with_epr(), scan(with_epr(), 36000), 5, true, false);
    CHECK_EQ(int(e->kind), int(StepKind::EprEnter));
}

void fixed_pps_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    // Same voltage on the fixed PDO and on the APDO: 9 V fixed, then PPS at 9 V.
    const auto a = step(Seq::FixedPps, 0, c, L, 10, false, false);
    const auto b = step(Seq::FixedPps, 1, c, L, 10, false, false);
    CHECK_EQ(int(c.pdos[a->choice.pos - 1].type), int(pd::PdoType::Fixed));
    CHECK_EQ(int(c.pdos[b->choice.pos - 1].type), int(pd::PdoType::Pps));
    CHECK_EQ(a->target_mv, b->target_mv);
}

void pps_step_sizes() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    const std::uint16_t cyc = 2;
    const std::uint16_t n = length(Seq::PpsSteps, c, L, cyc, false, false);
    CHECK_EQ(n, 1 + 4 * 2 * 2);
    // The first step is the settled move to mid window, then the steps alternate up by the size and back, size by size.
    const auto first = step(Seq::PpsSteps, 0, c, L, cyc, false, false);
    CHECK_EQ(int(first->pace), int(Pace::Settled));
    const std::uint32_t v0 = first->target_mv;
    CHECK_EQ(v0 % 20, 0);
    const std::uint16_t sizes[] = {20, 100, 500, 2000};
    for (int k = 0; k < 4; ++k) {
        const auto up = step(Seq::PpsSteps, std::uint16_t(1 + k * 4), c, L, cyc, false, false);
        const auto dn = step(Seq::PpsSteps, std::uint16_t(2 + k * 4), c, L, cyc, false, false);
        CHECK_EQ(up->target_mv, v0 + sizes[k]);
        CHECK_EQ(dn->target_mv, v0);
        CHECK_EQ(int(up->pace), int(Pace::Ready));
    }
}

void current_only_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    const auto half = step(Seq::CurrentOnly, 0, c, L, 10, false, false);
    const auto full = step(Seq::CurrentOnly, 1, c, L, 10, false, false);
    CHECK_EQ(half->choice.pos, full->choice.pos);
    CHECK_EQ(half->target_mv, full->target_mv);   // the voltage does not move
    CHECK_EQ(half->choice.ma, 1500);              // half of the advertised 3 A
    CHECK_EQ(full->choice.ma, 3000);
}

void mismatch_steps() {
    const auto c = rg100();
    const Levels L = scan(c, 36000);
    const auto m = step(Seq::Mismatch, 0, c, L, 10, false, false);
    const auto p = step(Seq::Mismatch, 1, c, L, 10, false, false);
    CHECK_COND(m->choice.mismatch);
    CHECK_COND(!p->choice.mismatch);
    CHECK_EQ(m->choice.pos, 4);
    CHECK_EQ(p->choice.pos, 1);
    CHECK_EQ(m->choice.ma, 0);   // full advertised current: the request is fully served, there is no real mismatch
}

void pps_low_steps() {
    const auto c = lowpps();
    const Levels L = scan(c, 36000);
    const auto lo = step(Seq::PpsLow, 0, c, L, 10, false, false);
    const auto hi = step(Seq::PpsLow, 1, c, L, 10, false, false);
    CHECK_COND(lo->target_mv < 5000);
    CHECK_COND(lo->target_mv >= c.pdos[L.pps - 1].min_mv);
    CHECK_EQ(hi->target_mv, 5000);
}

// The Capability Mismatch bit is RDO bit 26 and nothing else changes in the RDO.
void rdo_mismatch_bit() {
    const auto c = rg100();
    pd::Choice a;
    a.pos = 4;
    pd::Choice b = a;
    b.mismatch = true;
    const pd::Plan pa = pd::plan_request(c, a, 36000, false);
    const pd::Plan pb = pd::plan_request(c, b, 36000, false);
    CHECK_EQ(int(pa.err), int(pd::Err::Ok));
    CHECK_EQ(int(pb.err), int(pd::Err::Ok));
    CHECK_EQ(pb.rdo, pa.rdo | (1u << 26));
    CHECK_EQ(pa.rdo & (1u << 26), 0);
    CHECK_EQ(pb.mv, pa.mv);
    CHECK_EQ(pb.ma, pa.ma);
    // The decoder reads it back as the mismatch flag.
    CHECK_COND(pd::decode_rdo(pb.rdo, &c.pdos[3]).flags & pd::kRdoFMismatch);
    CHECK_COND(!(pd::decode_rdo(pa.rdo, &c.pdos[3]).flags & pd::kRdoFMismatch));
    // Also on an APDO.
    pd::Choice p;
    p.pos = 5;
    p.mv = 9000;
    p.mismatch = true;
    CHECK_COND(pd::plan_request(c, p, 36000, false).rdo & (1u << 26));
    // An invalid request stays invalid with the bit set.
    pd::Choice bad;
    bad.pos = 9;
    bad.mismatch = true;
    CHECK_EQ(int(pd::plan_request(c, bad, 36000, false).err), int(pd::Err::BadPos));
}

void table_sanity() {
    for (std::size_t i = 0; i < kSeqCount; ++i) {
        CHECK_COND(std::string_view(kSeqs[i].name) != "");
        CHECK_COND(kSeqs[i].cycles > 0);
    }
    CHECK_EQ(kSeqs[std::size_t(Seq::Ladder)].cycles, 10);
    CHECK_EQ(kSeqs[std::size_t(Seq::Slam)].cycles, 40);
    CHECK_EQ(kSeqs[std::size_t(Seq::SameReq)].cycles, 50);
    CHECK_EQ(kSeqs[std::size_t(Seq::PpsSteps)].cycles, 50);
    CHECK_EQ(kReadyMs, 100);   // the operator's pacing: next Request 100 ms after PS_RDY
    CHECK_COND(kSeqs[std::size_t(Seq::FixedPps)].pps && kSeqs[std::size_t(Seq::PpsSteps)].pps && kSeqs[std::size_t(Seq::PpsLow)].pps);
    CHECK_COND(!kSeqs[std::size_t(Seq::AvsSteps)].pps);
}

}  // namespace

int main() {
    scan_levels();
    applicability_rg100();
    applicability_other();
    default_mask();
    cycles_scale();
    lengths_rg100();
    never_outside_the_list();
    ladder_steps();
    slam_steps();
    same_request_steps();
    kind_steps();
    fixed_pps_steps();
    pps_step_sizes();
    current_only_steps();
    mismatch_steps();
    pps_low_steps();
    rdo_mismatch_bit();
    table_sanity();
    return test_report("test_pdstress_plan");
}
