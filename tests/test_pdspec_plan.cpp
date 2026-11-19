// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check plan (k2/pdspec/plan.hpp): the levels a list offers, the walk over every ordered pair of fixed levels, the PPS points.
#include <array>
#include <cstdint>
#include <vector>

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
pd::Pdo avs(std::uint16_t hi) {   // SPR AVS: 9 V up to 15 or 20 V
    pd::Pdo d;
    d.type = pd::PdoType::Avs;
    d.min_mv = 9000;
    d.max_mv = hi;
    d.max_ma = 3250;
    d.raw = 0xD0000000u;
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
    CHECK_EQ(int(L.n_prog), 2);
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

// The programmable windows: the SPR PPS APDOs (list order), then the SPR AVS APDO; each one from its minimum (the floor when it is
// higher) to its maximum (the ceiling when it is lower), on its own resolution.
void windows_of_a_list() {
    const auto c = caps({fixed(5000), fixed(9000), fixed(15000), fixed(20000), avs(20000), pps(3300, 21000), pps(3300, 11000)});
    const Levels L = scan(c, 36000, false);
    CHECK_EQ(int(L.n_prog), 3);
    CHECK_EQ(int(L.prog[0].pos), 6);
    CHECK_COND(L.prog[0].kind == ProgKind::Pps);
    CHECK_EQ(int(L.prog[0].lo), 3300);
    CHECK_EQ(int(L.prog[0].hi), 21000);
    CHECK_EQ(int(L.prog[1].pos), 7);
    CHECK_EQ(int(L.prog[1].hi), 11000);
    CHECK_EQ(int(L.prog[2].pos), 5);
    CHECK_COND(L.prog[2].kind == ProgKind::Avs);
    CHECK_EQ(int(L.prog[2].lo), 9000);
    CHECK_EQ(int(L.prog[2].hi), 20000);
    CHECK_EQ(int(L.n_pps), 2);
    CHECK_EQ(int(L.pps[0]), 6);
    CHECK_EQ(int(L.n_avs), 1);
    // A 12 V ceiling and a 5 V floor (a self-powered K2): both ends clamped, odd values rounded inwards to the resolution.
    const auto d = caps({fixed(5000), pps(3310, 21000), avs(15000)});
    const Levels M = scan(d, 12050, false, 5010);
    CHECK_EQ(int(M.prog[0].lo), 5020);
    CHECK_EQ(int(M.prog[0].hi), 12040);
    CHECK_EQ(int(M.prog[1].lo), 9000);
    CHECK_EQ(int(M.prog[1].hi), 12000);
    // A window narrower than kMinSpanMv is not swept.
    const Levels N = scan(caps({fixed(5000), pps(3300, 4000)}), 36000, false);
    CHECK_EQ(int(N.n_prog), 0);
    CHECK_EQ(int(N.n_pps), 0);
}

// EPR AVS (a Deep run hands the APDO over, with the cable proof): its window after the SPR ones, capped at the ceiling; with every slot taken
// the AVS windows keep theirs and the PPS ones give way from the end (the keep-alive list keeps every PPS APDO).
pd::Pdo epr_avs(std::uint16_t hi) {
    pd::Pdo d = avs(hi);
    d.min_mv = 15000;
    d.max_ma = 5000;
    return d;
}
void epr_avs_window() {
    pd::SourceCaps c = caps({fixed(5000, true), fixed(9000), fixed(15000), fixed(20000), pps(3300, 21000)});
    c.pdos[7] = fixed(28000);
    c.pdos[8] = epr_avs(28000);
    c.num_pdos = 9;
    CHECK_EQ(int(scan(c, 36000, true).n_prog), 1);                                // the standalone run: SPR windows only
    CHECK_EQ(int(scan(c, 36000, false, kHostFloorMv, &c.pdos[8], 9).n_prog), 1);  // no cable proof
    const Levels L = scan(c, 36000, true, kHostFloorMv, &c.pdos[8], 9);
    CHECK_EQ(int(L.n_prog), 2);
    CHECK_EQ(int(L.prog[1].pos), 9);
    CHECK_COND(L.prog[1].kind == ProgKind::Avs);
    CHECK_EQ(int(L.prog[1].lo), 15000);
    CHECK_EQ(int(L.prog[1].hi), 28000);
    CHECK_EQ(int(L.n_avs), 0);   // the caps note counts the SPR list
    CHECK_EQ(int(L.epr), 8);
    c.pdos[8] = epr_avs(48000);
    const Levels M = scan(c, 36000, true, kHostFloorMv, &c.pdos[8], 9);
    CHECK_EQ(int(M.prog[1].hi), 36000);
    pd::SourceCaps f = caps({fixed(5000, true), fixed(20000), avs(20000), pps(3300, 11000), pps(3300, 16000), pps(3300, 21000)});
    f.pdos[7] = fixed(28000);
    f.pdos[8] = epr_avs(28000);
    f.num_pdos = 9;
    const Levels F = scan(f, 36000, true, kHostFloorMv, &f.pdos[8], 9);
    CHECK_EQ(int(F.n_prog), 4);
    CHECK_EQ(int(F.prog[0].pos), 4);
    CHECK_EQ(int(F.prog[1].pos), 5);
    CHECK_EQ(int(F.prog[2].pos), 3);
    CHECK_EQ(int(F.prog[3].pos), 9);
    CHECK_EQ(int(F.n_pps), 3);
    const Levels G = scan(f, 36000, true);   // without EPR AVS: as before, three PPS and the SPR AVS
    CHECK_EQ(int(G.n_prog), 4);
    CHECK_EQ(int(G.prog[2].pos), 6);
    CHECK_EQ(int(G.prog[3].pos), 3);
    // The run hands the APDO over by itself (the live list is the SPR one again once the run left EPR mode); a fixed PDO or an SPR
    // position is no EPR AVS window.
    const pd::SourceCaps spr = caps({fixed(5000, true), fixed(20000), pps(3300, 21000)});
    const pd::Pdo ea = epr_avs(28000);
    CHECK_EQ(int(scan(spr, 36000, true, kHostFloorMv, &ea, 9).n_prog), 2);
    CHECK_EQ(int(scan(spr, 36000, true, kHostFloorMv, &ea, 9).epr), 0);   // the EPR fixed walk node stays out
    const pd::Pdo fx = fixed(28000);
    CHECK_EQ(int(scan(spr, 36000, true, kHostFloorMv, &fx, 8).n_prog), 1);
    CHECK_EQ(int(scan(spr, 36000, true, kHostFloorMv, &ea, 3).n_prog), 1);
}

// The measured moves of one pass, in order.
std::vector<Move> moves(Sweep s) {
    std::vector<Move> v;
    Move m;
    while (s.next(m)) v.push_back(m);
    return v;
}
Prog window(std::int32_t lo, std::int32_t hi, ProgKind k = ProgKind::Pps) { return {1, k, lo, hi}; }

// SPT.6 E.2.k: per step size, up from Vmin while a whole step fits, then the rest up to Vmax; down the same way to Vmin.
void full_sweep_up_then_down() {
    const auto v = moves(make_sweep(window(3300, 11000), 1, false));   // 100 mV
    CHECK_EQ(int(v.size()), 154);
    CHECK_EQ(int(v[0].mv), 3400);
    CHECK_EQ(int(v[76].mv), 11000);
    CHECK_EQ(int(v[77].mv), 10900);
    CHECK_EQ(int(v[153].mv), 3300);
    for (const Move& m : v) CHECK_COND(m.measured);
    CHECK_EQ(int(measured_moves(make_sweep(window(3300, 11000), 1, false))), 154);
}

void a_step_that_does_not_fit_ends_on_the_rest() {
    const auto v = moves(make_sweep(window(3300, 11000), 2, false));   // 500 mV: 15 steps to 10.8 V, then 200 mV to 11 V
    CHECK_EQ(int(v.size()), 32);
    CHECK_EQ(int(v[14].mv), 10800);
    CHECK_EQ(int(v[15].mv), 11000);
    CHECK_EQ(int(v[16].mv), 10500);
    CHECK_EQ(int(v[30].mv), 3500);
    CHECK_EQ(int(v[31].mv), 3300);
}

void slams_are_two_moves() {
    const auto v = moves(make_sweep(window(3300, 11000), 3, false));
    CHECK_EQ(int(v.size()), 2);
    CHECK_EQ(int(v[0].mv), 11000);
    CHECK_EQ(int(v[1].mv), 3300);
    CHECK_COND(v[0].measured && v[1].measured);
}

// 20 mV: three windows of kWindowSteps steps (low, middle, top) up and the same down; the moves between windows are not measured.
void twenty_mv_windows() {
    const auto v = moves(make_sweep(window(3300, 11000), 0, false));
    CHECK_EQ(int(v.size()), 64);
    int measured = 0;
    for (const Move& m : v) measured += m.measured;
    CHECK_EQ(measured, 60);
    CHECK_EQ(int(v[0].mv), 3320);
    CHECK_EQ(int(v[9].mv), 3500);
    CHECK_COND(!v[10].measured);
    CHECK_EQ(int(v[10].mv), 7040);         // (3300 + 11000) / 2 - 100, down to 20 mV
    CHECK_EQ(int(v[11].mv), 7060);
    CHECK_COND(!v[21].measured);
    CHECK_EQ(int(v[21].mv), 10800);
    CHECK_EQ(int(v[31].mv), 11000);
    CHECK_EQ(int(v[32].mv), 10980);        // the top window goes down at once
    CHECK_EQ(int(v[42].mv), 7240);         // up to the middle window's top, not measured
    CHECK_COND(!v[42].measured);
    CHECK_EQ(int(v[63].mv), 3300);
    CHECK_EQ(int(measured_moves(make_sweep(window(3300, 11000), 0, false))), 60);
    // The full 20 mV sweep (the option), and a window too narrow for three windows: the whole range.
    CHECK_EQ(int(measured_moves(make_sweep(window(3300, 11000), 0, true))), 770);
    CHECK_EQ(int(measured_moves(make_sweep(window(3300, 3800), 0, false))), 50);
}

// SPR AVS: 100 mV resolution, step sizes 100, 500 and 1000 mV (vAvsSmallStep), then the slams.
void avs_steps() {
    CHECK_EQ(step_mv(ProgKind::Avs, 0), 100);
    CHECK_EQ(step_mv(ProgKind::Avs, 2), 1000);
    CHECK_EQ(step_mv(ProgKind::Pps, 0), 20);
    CHECK_EQ(int(measured_moves(make_sweep(window(9000, 20000, ProgKind::Avs), 0, false))), 220);   // no 20 mV windows for AVS
    CHECK_EQ(int(measured_moves(make_sweep(window(9000, 20000, ProgKind::Avs), 2, false))), 22);
}

// prog_unify-b: a window the charger test swept gets a short cover in the stage: a few large steps up from the minimum and back (500 mV
// PPS, 1 V AVS), then the resolution step at both ends (down onto the minimum, up onto the maximum), then the slams as before.
std::vector<std::int32_t> mvs(const std::vector<Move>& v, bool measured) {
    std::vector<std::int32_t> o;
    for (const Move& m : v) o.push_back(m.measured == measured ? m.mv : -m.mv);
    return o;
}
void cover_passes() {
    using V = std::vector<std::int32_t>;
    CHECK_COND(mvs(moves(make_cover_sweep(window(3300, 11000), 2)), true) == V({3800, 4300, 4800, 4300, 3800, 3300}));
    CHECK_COND(mvs(moves(make_cover_sweep(window(3300, 11000), 0)), true) ==
               V({-3400, 3380, 3360, 3340, 3320, 3300, -10900, 10920, 10940, 10960, 10980, 11000, -3300}));
    CHECK_COND(mvs(moves(make_cover_sweep(window(9000, 20000, ProgKind::Avs), 2)), true) == V({10000, 11000, 12000, 11000, 10000, 9000}));
    CHECK_COND(mvs(moves(make_cover_sweep(window(9000, 20000, ProgKind::Avs), 0)), true) ==
               V({-9500, 9400, 9300, 9200, 9100, 9000, -19500, 19600, 19700, 19800, 19900, 20000, -9000}));
    // A narrow window: the large steps stop at its maximum.
    CHECK_COND(mvs(moves(make_cover_sweep(window(3300, 4300), 2)), true) == V({3800, 4300, 3800, 3300}));
    CHECK_COND(mvs(moves(make_cover_sweep(window(3300, 11000), 3)), true) == V({11000, 3300}));
    CHECK_EQ(int(moves(make_cover_sweep(window(3300, 11000), 1)).size()), 0);   // no middle step class in a cover
    CHECK_EQ(int(cover_moves(window(3300, 11000))), 6 + 10 + 2);
}

// The family's measured moves: the time budget of a run is about this many capture windows.
void family_moves() {
    const Levels L = scan(caps({fixed(5000), fixed(9000), fixed(12000), fixed(15000), fixed(20000), pps(3300, 11000)}), 36000, false);
    CHECK_EQ(int(family_moves(L, false)), 60 + 154 + 32 + 2);
    CHECK_EQ(int(family_moves(L, true)), 770 + 154 + 32 + 2);
}

}  // namespace

int main() {
    walk_covers_every_ordered_pair_once();
    walk_of_four_levels();
    levels_of_a_typical_list();
    levels_unsorted_and_capped();
    epr_needs_the_proof_and_the_ceiling();
    windows_of_a_list();
    full_sweep_up_then_down();
    a_step_that_does_not_fit_ends_on_the_rest();
    slams_are_two_moves();
    twenty_mv_windows();
    avs_steps();
    family_moves();
    cover_passes();
    epr_avs_window();
    return test_report("test_pdspec_plan");
}
