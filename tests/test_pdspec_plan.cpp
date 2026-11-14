// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check plan (k2/pdspec/plan.hpp): the levels a list offers, the walk over every ordered pair of fixed levels, the PPS points.
#include <array>
#include <cstdint>

#include "k2/pdspec/plan.hpp"
#include "test_util.h"

using namespace k2;
using namespace k2::pdspec;

namespace {

pd::Pdo fixed(std::uint16_t mv, bool epr_bit = false) {
    pd::Pdo d;
    d.type = pd::PdoType::Fixed;
    d.min_mv = d.max_mv = mv;
    d.max_ma = 3000;
    d.raw = (1u << 26) | (epr_bit ? 1u << 23 : 0u) | (std::uint32_t(mv / 50) << 10) | 300;
    return d;
}
pd::Pdo pps(std::uint16_t lo, std::uint16_t hi) {
    pd::Pdo d;
    d.type = pd::PdoType::Pps;
    d.min_mv = lo;
    d.max_mv = hi;
    d.max_ma = 3000;
    d.raw = 0xC0000000u;
    return d;
}
pd::SourceCaps caps(std::initializer_list<pd::Pdo> l) {
    pd::SourceCaps c;
    c.valid = true;
    for (const auto& d : l) c.pdos[c.num_pdos++] = d;
    return c;
}

void walk_covers_every_ordered_pair_once() {
    for (std::uint8_t n = 2; n <= kMaxNodes; ++n) {
        const Walk w = make_walk(n);
        CHECK_EQ(int(w.len), n * (n - 1) + 1);
        CHECK_EQ(int(w.node[0]), 0);
        CHECK_EQ(int(w.node[w.len - 1]), 0);
        std::array<std::array<int, kMaxNodes>, kMaxNodes> seen{};
        for (std::size_t i = 0; i + 1 < w.len; ++i) {
            CHECK_COND(w.node[i] != w.node[i + 1]);
            seen[w.node[i]][w.node[i + 1]]++;
        }
        for (std::uint8_t a = 0; a < n; ++a)
            for (std::uint8_t b = 0; b < n; ++b) CHECK_EQ(seen[a][b], a == b ? 0 : 1);
    }
    CHECK_EQ(int(make_walk(1).len), 0);
}

void walk_of_four_levels() {
    // 5/9/15/20 V: 12 measured transitions, the first one leaves 5 V.
    const Walk w = make_walk(4);
    const std::array<int, 13> want{0, 1, 0, 2, 0, 3, 1, 2, 1, 3, 2, 3, 0};
    for (std::size_t i = 0; i < want.size(); ++i) CHECK_EQ(int(w.node[i]), want[i]);
}

void levels_of_a_typical_list() {
    // 5/9/15/20 V + PPS 3.3-21 V + a second PPS 3.3-11 V, a 36 V K2: four fixed nodes ascending, two APDOs, no EPR.
    const auto c = caps({fixed(5000), fixed(9000), fixed(15000), fixed(20000), pps(3300, 21000), pps(3300, 11000)});
    const Levels L = scan(c, 36000, false);
    CHECK_EQ(int(L.n_fixed), 4);
    CHECK_EQ(int(L.fixed[0]), 1);
    CHECK_EQ(int(L.fixed[3]), 4);
    CHECK_EQ(int(L.top_spr), 4);
    CHECK_EQ(int(L.n_pps), 2);
    CHECK_EQ(int(L.pps[0]), 5);
    CHECK_COND(!L.epr_offered);
    CHECK_EQ(int(L.epr), 0);
}

void levels_unsorted_and_capped() {
    // A list out of order (12 V after 15 V) and a 20 V level above a 15 V ceiling: sorted by voltage, the 20 V left out.
    const auto c = caps({fixed(5000), fixed(15000), fixed(12000), fixed(20000)});
    const Levels L = scan(c, 15000, false);
    CHECK_EQ(int(L.n_fixed), 3);
    CHECK_EQ(int(L.fixed[1]), 3);   // 12 V
    CHECK_EQ(int(L.fixed[2]), 2);   // 15 V
    CHECK_EQ(int(L.top_spr), 2);
}

void epr_needs_the_proof_and_the_ceiling() {
    pd::SourceCaps c = caps({fixed(5000, true), fixed(9000), fixed(15000), fixed(20000)});
    c.pdos[7] = fixed(28000);
    c.pdos[8] = fixed(36000);
    c.pdos[9] = fixed(48000);
    c.num_pdos = 10;
    const Levels no_proof = scan(c, 36000, false);
    CHECK_COND(no_proof.epr_offered);
    CHECK_EQ(int(no_proof.epr), 0);
    CHECK_EQ(int(no_proof.n_fixed), 4);
    const Levels proof = scan(c, 36000, true);
    CHECK_EQ(int(proof.epr), 8);           // 28 V, the lowest EPR level
    CHECK_EQ(int(proof.n_fixed), 5);
    CHECK_EQ(int(proof.fixed[4]), 8);
    CHECK_EQ(int(proof.top_spr), 4);
}

void pps_points_of_windows() {
    const Points a = pps_points(pps(3300, 21000), 36000);
    CHECK_EQ(int(a.n), 3);
    CHECK_EQ(int(a.mv[0]), 5000);
    CHECK_EQ(int(a.mv[2]), 20900);          // 21000 less 5 x 20 mV
    CHECK_EQ(int(a.mv[1]), 12940);          // (5000 + 20900) / 2 = 12950, down to a multiple of 20
    const Points b = pps_points(pps(3300, 5900), 36000);
    CHECK_EQ(int(b.n), 3);
    CHECK_EQ(int(b.mv[0]), 5000);
    CHECK_EQ(int(b.mv[1]), 5400);           // (5000 + 5800) / 2
    CHECK_EQ(int(b.mv[2]), 5800);           // 5900 less 5 x 20 mV
    const Points c = pps_points(pps(3300, 5080), 36000);   // no room for five steps above 5 V
    CHECK_EQ(int(c.n), 0);
    const Points d = pps_points(pps(5010, 21000), 12000);  // odd minimum rounded up, the cap below the window's top
    CHECK_EQ(int(d.mv[0]), 5020);
    CHECK_EQ(int(d.mv[2]), 11900);
}

}  // namespace

int main() {
    walk_covers_every_ordered_pair_once();
    walk_of_four_levels();
    levels_of_a_typical_list();
    levels_unsorted_and_capped();
    epr_needs_the_proof_and_the_ceiling();
    pps_points_of_windows();
    return test_report("test_pdspec_plan");
}
