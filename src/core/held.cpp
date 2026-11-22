// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/held.hpp"

#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/pd/pd_held.hpp"
#include "k2/pd/pd_source.hpp"

namespace k2::held {
namespace {
pd::HeldOffers g_held;
pd::HeldCable g_cable;
std::uint32_t g_session = 0;
}

void poll(std::uint32_t now_ms, bool inline_mode, bool probed) {
    const pd::SourceCaps &live = fusb302::caps();
    const std::uint32_t sess = fusb302::session();
    if (live.valid && live.num_pdos > 0 && sess != g_session) {
        g_held.clear();   // a brand-new attach session delivered caps: never let the previous charger's peak block it
        g_session = sess;
    }
    g_held.capture_settled(live, fusb302::epr_info_list(), now_ms, inline_mode, probed);
    g_cable.capture(fusb302::emarker(), fusb302::cable_extra(), fusb302::cable_answered(), probed);
}

void poll() {
    poll(systick::millis(), fusb302::inline_mode());
}

bool showing_held() { return g_held.caps().valid && !pd::is_fake() && !pd::active_caps().valid; }

const pd::SourceCaps &shown_caps() {
    const pd::SourceCaps &live = pd::active_caps();
    if (pd::is_fake()) return live;
    if (g_held.caps().valid && (!live.valid || fusb302::session() == g_session)) return g_held.caps();
    return live.valid ? live : g_held.caps();
}

std::span<const std::uint32_t> epr_raw() {
    static std::uint32_t live[pd::kMaxPdos];
    const pd::SourceCaps &l = fusb302::epr_info_list();
    if (l.valid && l.num_pdos > 0) {
        std::size_t n = 0;
        for (std::size_t i = 0; i < l.num_pdos && i < pd::kMaxPdos; i++) live[n++] = l.pdos[i].raw;
        return {live, n};
    }
    return g_held.epr();
}

pd::OfferSource offer_source() { return g_held.source(); }
bool offer_throttled() { return g_held.throttled(); }
std::uint32_t live_offer_mw() { return g_held.live_mw(); }
void mark_charger_probed() { g_held.mark_probed(); }

void reset() {
    g_held.clear();
    g_cable.clear();
    g_session = fusb302::session();
}

void clear_for_reset() {
    g_held.clear();
    g_cable.clear();
    g_session = fusb302::session();
}

void attach_edge(bool cable_stale) {
    g_held.clear();
    if (cable_stale) g_cable.clear();
    g_session = fusb302::session();
}

const pd::Emarker &cable_em() { return g_cable.em(); }
const cable_full::Full &cable_extra() { return g_cable.full(); }
emk_variant::Answered cable_answered() { return g_cable.answered(); }
bool cable_thunderbolt() { return g_cable.thunderbolt(); }
pd::CableSource cable_source() { return g_cable.source(); }
void mark_cable_probed() { g_cable.mark_probed(); }

}  // namespace k2::held
