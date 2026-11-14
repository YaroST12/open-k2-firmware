// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the plan: which levels a Source_Capabilities list gives the checks (pure, host-tested in tests/test_pdspec_plan.cpp).
//
// Fixed transitions: one walk through every ordered pair of fixed levels (up and down), starting and ending at 5 V, so each pair is
// measured exactly once and no unmeasured step sits between two measured ones (an Euler circuit of the complete directed graph).
// EPR fixed levels at or below the K2's monitor ceiling join the walk when the cable proof is there.
// PPS: per APDO three set points (low, middle, high), each stepped kPpsSteps x 20 mV up and the same back down.
#pragma once

#include <array>
#include <cstdint>

#include "k2/pd/pd.hpp"

namespace k2::pdspec {

inline constexpr std::size_t kMaxNodes = 8;                       // fixed levels in the walk (7 SPR + 1 EPR)
inline constexpr std::size_t kMaxWalk = kMaxNodes * (kMaxNodes - 1) + 1;
inline constexpr std::size_t kMaxApdos = 4;                       // PPS APDOs stepped (the rest are not measured)
inline constexpr std::size_t kPointsPerApdo = 3;
inline constexpr std::uint8_t kPpsSteps = 5;                      // 20 mV steps up, then as many down
inline constexpr std::int32_t kPpsStepMv = 20;
inline constexpr std::int32_t kPpsLowMv = 5000;                   // the low set point is never under 5 V (the K2's own rail)

struct Levels {
    std::array<std::uint8_t, kMaxNodes> fixed{};   // positions, ascending voltage, fixed[0] = the 5 V PDO
    std::uint8_t n_fixed = 0;
    std::uint8_t top_spr = 0;                       // the highest SPR fixed position
    std::uint8_t epr = 0;                           // the lowest EPR fixed position at or below the cap (0 = none usable)
    bool epr_offered = false;                       // EPR in PDO 1, or an EPR fixed level in the list
    std::array<std::uint8_t, kMaxApdos> pps{};      // SPR PPS positions whose window reaches 5 V..cap
    std::uint8_t n_pps = 0;
};

// cap_mv: the monitor ceiling; epr_proof: the cable proof EPR levels need. EPR levels above the cap are left out.
inline Levels scan(const pd::SourceCaps& caps, std::uint32_t cap_mv, bool epr_proof) {
    Levels L;
    if (!caps.valid) return L;
    const std::size_t n = caps.num_pdos < caps.pdos.size() ? caps.num_pdos : caps.pdos.size();
    if (n > 0 && caps.pdos[0].type == pd::PdoType::Fixed && (caps.pdos[0].raw >> 23 & 1)) L.epr_offered = true;
    for (std::size_t i = 0; i < n; ++i) {
        const pd::Pdo& d = caps.pdos[i];
        const auto pos = std::uint8_t(i + 1);
        if (d.type == pd::PdoType::Fixed && d.max_mv <= cap_mv) {
            if (pos <= 7) {
                if (L.n_fixed < kMaxNodes - 1) {
                    std::size_t k = L.n_fixed++;
                    while (k > 0 && caps.pdos[L.fixed[k - 1] - 1].max_mv > d.max_mv) { L.fixed[k] = L.fixed[k - 1]; --k; }
                    L.fixed[k] = pos;
                }
                if (L.top_spr == 0 || d.max_mv >= caps.pdos[L.top_spr - 1].max_mv) L.top_spr = pos;
            } else {
                L.epr_offered = true;
                if (epr_proof && (L.epr == 0 || d.max_mv < caps.pdos[L.epr - 1].max_mv)) L.epr = pos;
            }
        } else if (d.type == pd::PdoType::Fixed && pos >= 8) {
            L.epr_offered = true;
        } else if (d.type == pd::PdoType::Pps && pos <= 7 && L.n_pps < kMaxApdos && d.max_mv >= kPpsLowMv + kPpsSteps * kPpsStepMv &&
                   std::uint32_t(d.min_mv + kPpsSteps * kPpsStepMv) <= cap_mv) {
            L.pps[L.n_pps++] = pos;
        }
    }
    if (L.epr) L.fixed[L.n_fixed++] = L.epr;   // the EPR level is the walk's highest node
    return L;
}

// The walk: node indices into Levels::fixed. Every ordered pair (a, b), a != b, appears exactly once as consecutive entries; the walk
// starts and ends at node 0 (5 V). Length n*(n-1)+1, or 0 with fewer than two nodes. Hierholzer's algorithm on fixed arrays.
struct Walk {
    std::array<std::uint8_t, kMaxWalk> node{};
    std::uint8_t len = 0;
};

inline Walk make_walk(std::uint8_t n) {
    Walk w;
    if (n < 2 || n > kMaxNodes) return w;
    std::array<std::uint8_t, kMaxNodes> next{};     // per node: the next out-edge target to use (skipping itself)
    std::array<std::uint8_t, kMaxWalk> stack{};
    std::size_t sp = 0;
    stack[sp++] = 0;
    std::array<std::uint8_t, kMaxWalk> out{};
    std::size_t no = 0;
    while (sp > 0) {
        const std::uint8_t v = stack[sp - 1];
        if (next[v] == v) ++next[v];
        if (next[v] < n) {
            const std::uint8_t u = next[v]++;
            if (next[v] == v) ++next[v];
            stack[sp++] = u;
        } else {
            out[no++] = v;
            --sp;
        }
    }
    // Hierholzer emits the circuit in reverse.
    for (std::size_t i = 0; i < no; ++i) w.node[i] = out[no - 1 - i];
    w.len = std::uint8_t(no);
    return w;
}

// The PPS set points of an APDO (mV, a multiple of 20): low = max(min, 5 V), high = min(max, cap) less the room for the up-steps,
// middle between them. Fewer than three when the window is narrow (duplicates dropped).
struct Points {
    std::array<std::uint16_t, kPointsPerApdo> mv{};
    std::uint8_t n = 0;
};

inline Points pps_points(const pd::Pdo& d, std::uint32_t cap_mv) {
    Points p;
    const std::int32_t room = kPpsSteps * kPpsStepMv;
    std::int32_t lo = d.min_mv > kPpsLowMv ? d.min_mv : kPpsLowMv;
    lo = (lo + kPpsStepMv - 1) / kPpsStepMv * kPpsStepMv;
    std::int32_t top = d.max_mv < std::int32_t(cap_mv) ? d.max_mv : std::int32_t(cap_mv);
    std::int32_t hi = (top - room) / kPpsStepMv * kPpsStepMv;
    if (hi < lo) return p;
    const std::int32_t mid = (lo + hi) / 2 / kPpsStepMv * kPpsStepMv;
    for (const std::int32_t v : {lo, mid, hi}) {
        if (p.n > 0 && p.mv[p.n - 1] == v) continue;
        p.mv[p.n++] = std::uint16_t(v);
    }
    return p;
}

}  // namespace k2::pdspec
