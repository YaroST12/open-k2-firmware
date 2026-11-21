// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger's offers, kept for the Charger tab. Same charger = the same SPR offers (PDO positions 1..7); the EPR PDOs
// belong to the charger whose SPR offers they were read with. A new list replaces the held one only when its content differs,
// so a sleeping charger, a dropped Rd or a detach changes nothing.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pd.hpp"

namespace k2::pd {

constexpr std::size_t kSprPdos = 7;   // positions 1..7 are the SPR offers; 8 on are EPR

// The PDOs of a and b are the same offers. Not the message header (its id changes every message) or the receive counter.
inline bool offers_same(const SourceCaps &a, const SourceCaps &b) {
    if (a.valid != b.valid || a.num_pdos != b.num_pdos || a.epr != b.epr) return false;
    for (std::size_t i = 0; i < a.num_pdos && i < kMaxPdos; i++)
        if (a.pdos[i].raw != b.pdos[i].raw) return false;
    return true;
}

// Same SPR part: an EPR-built list starts with the same seven positions as the charger's SPR one.
inline bool spr_same(const SourceCaps &a, const SourceCaps &b) {
    const std::size_t na = std::min<std::size_t>(a.num_pdos, kSprPdos), nb = std::min<std::size_t>(b.num_pdos, kSprPdos);
    if (na != nb) return false;
    for (std::size_t i = 0; i < na; i++)
        if (a.pdos[i].raw != b.pdos[i].raw) return false;
    return true;
}

class HeldOffers {
public:
    // `live`: the source's current caps, `epr_live`: its EPR list (valid only when it was read). Call on every pass.
    void capture(const SourceCaps &live, const SourceCaps &epr_live) {
        if (live.valid && live.num_pdos > 0) {
            if (!caps_.valid || !spr_same(caps_, live)) epr_n_ = 0;   // another charger, or a renegotiation: its EPR list is not ours
            if (!offers_same(caps_, live)) caps_ = live;
        }
        if (epr_live.valid && epr_live.num_pdos > 0) {
            std::uint32_t raw[kMaxPdos];
            const std::size_t n = std::min<std::size_t>(epr_live.num_pdos, kMaxPdos);
            for (std::size_t i = 0; i < n; i++) raw[i] = epr_live.pdos[i].raw;
            if (n != epr_n_ || !std::equal(raw, raw + n, epr_)) {
                std::copy(raw, raw + n, epr_);
                epr_n_ = static_cast<std::uint8_t>(n);
            }
        }
    }
    const SourceCaps &caps() const { return caps_; }
    std::span<const std::uint32_t> epr() const { return {epr_, epr_n_}; }

private:
    SourceCaps caps_{};
    std::uint32_t epr_[kMaxPdos]{};
    std::uint8_t epr_n_ = 0;
};

}  // namespace k2::pd
