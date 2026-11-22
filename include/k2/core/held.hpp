// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// What the Charger and Cable tabs show once the charger or cable is quiet: what the tests, a Refresh, a cable read or the
// charger's own announcements delivered stays until a renegotiation or another cable replaces it (k2/pd/pd_held.hpp).
// Nothing here transmits.
#pragma once
#include <cstdint>
#include <span>

#include "k2/pd/cable_full.hpp"
#include "k2/pd/emk_variant.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_held.hpp"

namespace k2::held {

void poll();   // main loop: keeps the driver's lists
void poll(std::uint32_t now_ms, bool inline_mode, bool probed = false);
void reset();   // forget the held offers and cable (host tests start from nothing)
void clear_for_reset();
// A new attach ends the last session: its offers go at once (not at the first new caps), its cable too unless a cable
// read since the detach is fresher (the link's cable_stale).
void attach_edge(bool cable_stale);

// The live caps when there are any, else the held ones (the debug fake list always wins).
const pd::SourceCaps &shown_caps();
bool showing_held();   // the live caps are gone, the held ones are shown
// The EPR PDOs: the live EPR list when it has one, else the held one.
std::span<const std::uint32_t> epr_raw();
pd::OfferSource offer_source();
bool offer_throttled();
std::uint32_t live_offer_mw();
void mark_charger_probed();

// The cable the Cable tab shows: the last read one, until a read returns a different cable (k2/pd/pd_held.hpp).
const pd::Emarker &cable_em();
const cable_full::Full &cable_extra();
emk_variant::Answered cable_answered();
bool cable_thunderbolt();   // the held cable advertised SVID 0x8087
pd::CableSource cable_source();
void mark_cable_probed();

}  // namespace k2::held
