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

enum class OfferSource : std::uint8_t { None, Sniffed, Probed };
enum class CableSource : std::uint8_t { None, Sniffed, Probed };
inline constexpr std::uint32_t kOfferSettleMs = 800;

// Maximum power (mW) advertised across a SourceCaps list (32-bit safe, no 64-bit arithmetic).
inline std::uint32_t max_caps_mw(const SourceCaps &c) {
    if (!c.valid) return 0;
    std::uint32_t best = 0;
    const std::size_t n = std::min<std::size_t>(c.num_pdos, kMaxPdos);
    for (std::size_t i = 0; i < n; i++) {
        const Pdo &p = c.pdos[i];
        std::uint32_t mw = p.max_mw;
        if (mw == 0 && p.max_mv > 0 && p.max_ma > 0)
            mw = std::uint32_t(p.max_mv) * p.max_ma / 1000u;
        if (mw == 0 && p.type == PdoType::Unknown && p.raw != 0)
            mw = decode_pdo(p.raw).max_mw;
        if (mw > best) best = mw;
    }
    return best;
}

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
            if (source_ == OfferSource::None) source_ = OfferSource::Sniffed;
        }
        capture_epr(epr_live);
    }

    // Time-aware, anti-flicker, high-water-mark capture.
    void capture_settled(const SourceCaps &live, const SourceCaps &epr_live,
                         std::uint32_t now_ms, bool inline_mode, bool probed = false) {
        if (!live.valid || live.num_pdos == 0) {
            pending_valid_ = false;
            pending_committed_ = false;
            capture_epr(epr_live);
            return;
        }
        if (probed) {
            if (!caps_.valid || !spr_same(caps_, live)) epr_n_ = 0;
            if (!offers_same(caps_, live)) caps_ = live;
            source_ = OfferSource::Probed;
            live_mw_ = 0;
            pending_valid_ = false;
            pending_committed_ = false;
            capture_epr(epr_live);
            return;
        }
        if (!inline_mode && !caps_.valid) {
            epr_n_ = 0;
            caps_ = live;
            source_ = OfferSource::Sniffed;
            live_mw_ = 0;
            pending_valid_ = false;
            pending_committed_ = false;
            capture_epr(epr_live);
            return;
        }
        if (!pending_valid_ || !offers_same(pending_, live)) {
            pending_ = live;
            pending_since_ = now_ms;
            pending_valid_ = true;
            pending_committed_ = false;
        } else if (!pending_committed_ &&
                   static_cast<std::uint32_t>(now_ms - pending_since_) >= kOfferSettleMs) {
            pending_committed_ = true;
            const std::uint32_t cand_mw = max_caps_mw(pending_);
            const std::uint32_t cur_mw = max_caps_mw(caps_);
            if (caps_.valid && cur_mw > 0 && cand_mw > 0 && cand_mw < cur_mw) {
                live_mw_ = cand_mw;
            } else {
                if (!caps_.valid || !spr_same(caps_, pending_)) epr_n_ = 0;
                if (!offers_same(caps_, pending_)) caps_ = pending_;
                live_mw_ = 0;
                if (source_ == OfferSource::None) source_ = OfferSource::Sniffed;
            }
        }
        capture_epr(epr_live);
    }

    const SourceCaps &caps() const { return caps_; }
    std::span<const std::uint32_t> epr() const { return {epr_, epr_n_}; }
    OfferSource source() const { return source_; }
    void mark_probed() { if (caps_.valid) source_ = OfferSource::Probed; }
    std::uint32_t live_mw() const { return live_mw_; }
    bool throttled() const { return caps_.valid && live_mw_ > 0 && live_mw_ < max_caps_mw(caps_); }
    void clear() { *this = {}; }

private:
    void capture_epr(const SourceCaps &epr_live) {
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

    SourceCaps caps_{};
    std::uint32_t epr_[kMaxPdos]{};
    std::uint8_t epr_n_ = 0;
    OfferSource source_ = OfferSource::None;
    std::uint32_t live_mw_ = 0;
    SourceCaps pending_{};
    std::uint32_t pending_since_ = 0;
    bool pending_valid_ = false;
    bool pending_committed_ = false;
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
    void capture(const Emarker &live, const cable_full::Full &full, emk_variant::Answered answered, bool probed = false) {
        if (!live.valid) return;   // the driver's copies are cleared on detach and when a read starts
        if (!em_.valid || !cable_same(em_, live)) {
            em_ = live;
            full_ = {};
            answered_ = answered;
            source_ = (probed || (full.valid && full.p1.svids_ack)) ? CableSource::Probed : CableSource::Sniffed;
        } else {
            if (answered != answered_ && answered != emk_variant::Answered{}) answered_ = answered;
            if (probed || (full.valid && full.p1.svids_ack)) source_ = CableSource::Probed;
            else if (source_ == CableSource::None) source_ = CableSource::Sniffed;
        }
        if (full.valid && (full.p1.svids_ack || !full_.valid) && !full_same(full_, full)) full_ = full;
    }
    const Emarker &em() const { return em_; }
    const cable_full::Full &full() const { return full_; }
    emk_variant::Answered answered() const { return answered_; }
    CableSource source() const { return source_; }
    void mark_probed() { if (em_.valid) source_ = CableSource::Probed; }
    void clear() { *this = {}; }
    bool thunderbolt() const {
        for (std::size_t i = 0; i < full_.p1.nsvid && i < cable_full::kMaxSvids; i++)
            if (full_.p1.svid[i] == cable_full::kSvidTbt) return true;
        return false;
    }

private:
    Emarker em_{};
    cable_full::Full full_{};
    emk_variant::Answered answered_{};
    CableSource source_ = CableSource::None;
};

}  // namespace k2::pd
