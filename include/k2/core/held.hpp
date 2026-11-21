// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The offers the Charger tab shows after the charger is quiet: what the tests, a Refresh or the charger's own announcements
// delivered stays until a renegotiation brings different ones (k2/pd/pd_held.hpp). Nothing here transmits.
#pragma once
#include <cstdint>
#include <span>

#include "k2/pd/pd.hpp"

namespace k2::held {

void poll();   // main loop: keeps the driver's lists

// The live caps when there are any, else the held ones (the debug fake list always wins).
const pd::SourceCaps &shown_caps();
bool showing_held();   // the live caps are gone, the held ones are shown
// The EPR PDOs: the live EPR list when it has one, else the held one.
std::span<const std::uint32_t> epr_raw();

}  // namespace k2::held
