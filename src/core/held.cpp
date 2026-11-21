// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/held.hpp"

#include "k2/drivers/fusb302.hpp"
#include "k2/pd/pd_held.hpp"
#include "k2/pd/pd_source.hpp"

namespace k2::held {
namespace {
pd::HeldOffers g_held;
}

void poll() { g_held.capture(fusb302::caps(), fusb302::epr_info_list()); }

bool showing_held() { return g_held.caps().valid && !pd::is_fake() && !pd::active_caps().valid; }

const pd::SourceCaps &shown_caps() {
    const pd::SourceCaps &live = pd::active_caps();
    return !live.valid && g_held.caps().valid && !pd::is_fake() ? g_held.caps() : live;
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

}  // namespace k2::held
