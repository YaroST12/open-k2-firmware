// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Connection page's Refresh on the real hardware: binds the pure rules of k2/core/rescan_bridge.hpp to the sink, the FUSB302 and
// the Rd intent marker. A live explicit contract is refreshed with a Get_Source_Cap through the sink's gate (no CC change, no
// dark); everything else cycles CC as before, and a K2 that lives on the charger's VBUS writes the Rescan marker first so its
// comeback boot keeps the Rd and lands back on Connection -> Source (docs/ROADMAP.md, "Refresh kills a self-powered K2").
#pragma once
#include <cstdint>

namespace k2::rescan {

enum class Result : std::uint8_t {
    Passive,  // inline or PD-silent: fusb302::rescan() ran, which does nothing on the wire there
    Asked,    // a Get_Source_Cap went into the sink's gate; poll() watches for the answer
    Cycled,   // CC was cycled (the K2 survived it, or it is the host-powered / bench-powered kind)
    Busy,     // a negotiation or another rescan is in flight, or the marker could not be written: nothing happened, ask again
};

// The Refresh action. Never blocks beyond the CC cycle itself.
Result request();
// Main loop: feeds the Get_Source_Cap flow. A source that stays silent turns into the (bridged) cycle here.
void poll();
// A Refresh's Get_Source_Cap is still waiting for its answer (or for the sink's gate); false once it was answered, gave up or
// fell back to the CC cycle.
bool active();

}  // namespace k2::rescan
