// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
// The PDO choice of `pd attach` (remote.cpp), pure so the host tests pin it.
//
// Order, best first:
//   1. a fixed PDO at exactly the target (5 V on PDO 1, 9 V on a 9 V PDO);
//   2. a PPS / AVS range that holds the target (the target as its output voltage);
//   3. the lowest fixed PDO above the target, at most 1 V above it;
//   4. above 5 V only: the highest fixed PDO below the target;
//   5. PDO 1.
// Hardware 2026-11-11 (NOVOO RG100, caps 5/9/15/20 V fixed + PPS 5-21 V): a second `pd attach` at 5 V picked the PPS
// APDO (PDO 5) because the old scoring ranked every PPS range above every fixed level. A fixed level the source names
// is the plain contract and wins; a range is only for a voltage no fixed PDO offers.
// A Choice ma of 0 keeps its pd_sink.hpp meaning (the PDO maximum); target_ma passes through unchanged.
#include <cstdint>
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::pd {

constexpr std::uint16_t kAttachFixedSlackMv = 1000;  // rule 3: how far above the target a fixed PDO may sit

constexpr Choice pick_attach_pdo(const SourceCaps& caps, std::uint16_t target_mv, std::uint16_t target_ma,
                                 std::int32_t cap_mv) {
    Choice none{};
    if (!caps.valid || caps.num_pdos == 0 || cap_mv <= 0) return none;
    if (target_mv > cap_mv) target_mv = static_cast<std::uint16_t>(cap_mv);
    const unsigned n = caps.num_pdos < kMaxPdos ? caps.num_pdos : kMaxPdos;
    const auto pos = [](unsigned i) { return static_cast<std::uint8_t>(i + 1); };

    for (unsigned i = 0; i < n; i++) {  // 1. exact fixed
        const Pdo& p = caps.pdos[i];
        if (p.type == PdoType::Fixed && fits_monitor(p.max_mv, cap_mv) && p.max_mv == target_mv)
            return {pos(i), 0, target_ma};
    }
    for (unsigned i = 0; i < n; i++) {  // 2. PPS / AVS range
        const Pdo& p = caps.pdos[i];
        if (p.type != PdoType::Pps && p.type != PdoType::Avs) continue;
        if (!fits_monitor(p.max_mv, cap_mv)) continue;
        if (target_mv >= p.min_mv && target_mv <= p.max_mv) return {pos(i), target_mv, target_ma};
    }
    Choice best{};
    std::uint16_t best_mv = 0;
    for (unsigned i = 0; i < n; i++) {  // 3. lowest fixed just above
        const Pdo& p = caps.pdos[i];
        if (p.type != PdoType::Fixed || !fits_monitor(p.max_mv, cap_mv)) continue;
        if (p.max_mv > target_mv && p.max_mv <= target_mv + kAttachFixedSlackMv && (best.pos == 0 || p.max_mv < best_mv)) {
            best = {pos(i), 0, target_ma};
            best_mv = p.max_mv;
        }
    }
    if (best.pos != 0) return best;
    if (target_mv > 5000) {  // 4. highest fixed below
        for (unsigned i = 0; i < n; i++) {
            const Pdo& p = caps.pdos[i];
            if (p.type != PdoType::Fixed || !fits_monitor(p.max_mv, cap_mv) || p.max_mv > target_mv) continue;
            if (p.max_mv > best_mv) {
                best = {pos(i), 0, target_ma};
                best_mv = p.max_mv;
            }
        }
        if (best.pos != 0) return best;
    }
    if (fits_monitor(caps.pdos[0].max_mv, cap_mv)) return {1, 0, target_ma};  // 5. PDO 1
    return none;
}

}  // namespace k2::pd
