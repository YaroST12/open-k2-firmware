// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What the Charger and Cable tabs keep after the driver forgets it. Same charger = the same SPR offers (PDO positions 1..7);
// the EPR PDOs belong to the charger whose SPR offers they were read with. Same cable = the same Discover Identity words.
// New data replaces the held only when its content differs, so a sleeping charger, a dropped Rd or a detach changes nothing.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/cable_full.hpp"
#include "k2/pd/emk_variant.hpp"
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

// The same cable: the same ID header, certification, product and cable VDO words (not the answer's bookkeeping).
inline bool cable_same(const Emarker &a, const Emarker &b) {
    return a.valid == b.valid && a.vid == b.vid && a.pid == b.pid && a.num_vdos == b.num_vdos && a.raw_vdos == b.raw_vdos;
}

inline bool plug_same(const cable_full::Plug &a, const cable_full::Plug &b) {
    return a.svids_ack == b.svids_ack && a.nsvid == b.nsvid && a.mode_ack == b.mode_ack && a.svid == b.svid && a.mode == b.mode;
}
inline bool full_same(const cable_full::Full &a, const cable_full::Full &b) {
    return a.valid == b.valid && plug_same(a.p1, b.p1) && a.id2_ack == b.id2_ack && a.id2_n == b.id2_n && a.id2 == b.id2 &&
           plug_same(a.p2, b.p2);
}

// The cable's e-marker and its full read (SVIDs, modes, SOP''). A different cable replaces both; a re-read of the same cable
// replaces the full read only when it fetched the SVIDs, so an identity-only or refused re-read keeps what was learned.
class HeldCable {
public:
    void capture(const Emarker &live, const cable_full::Full &full, emk_variant::Answered answered) {
        if (!live.valid) return;   // the driver's copies are cleared on detach and when a read starts
        if (!em_.valid || !cable_same(em_, live)) {
            em_ = live;
            full_ = {};
            answered_ = answered;
        } else if (answered != answered_ && answered != emk_variant::Answered{}) {
            answered_ = answered;
        }
        if (full.valid && (full.p1.svids_ack || !full_.valid) && !full_same(full_, full)) full_ = full;
    }
    const Emarker &em() const { return em_; }
    const cable_full::Full &full() const { return full_; }
    emk_variant::Answered answered() const { return answered_; }
    bool thunderbolt() const {
        for (std::size_t i = 0; i < full_.p1.nsvid && i < cable_full::kMaxSvids; i++)
            if (full_.p1.svid[i] == cable_full::kSvidTbt) return true;
        return false;
    }

private:
    Emarker em_{};
    cable_full::Full full_{};
    emk_variant::Answered answered_{};
};

}  // namespace k2::pd
