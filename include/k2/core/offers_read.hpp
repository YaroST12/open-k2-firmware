// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Charger tab's automatic offers read on the real hardware: runs the pure plan of k2/pd/pd_offers.hpp and hands each action to its
// sender (fusb302::reread_start(0) for the attach, the sink's EPR_Get_Source_Cap gate, fusb302::reread_release_now()). The page keeps
// it armed while the tab shows; one read per presence of the charger. What it read is held here (the release detaches, which
// clears the driver's lists) and shown through shown_caps() / epr_raw() until the charger is gone for good.
#pragma once
#include <cstdint>
#include <span>

#include "k2/core/rescan.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_offers.hpp"

namespace k2::offers {

// The Charger tab is shown (page update, every pass): keeps the read armed for a moment.
void want(std::uint32_t now);
// A Refresh just ran rescan::request() and got `r`: its survey reads the offers, so the automatic read does not follow it.
void on_refresh(rescan::Result r);
// Main loop. `present`: a charger is attached or waiting (steady Rp); `charger_test`: a charger test owns the line.
void poll(bool charger_test, bool present);

bool running();
pd::OrWhy why();   // why the last attempt was refused (None when it was not)

// The live caps when there are any, else the ones the read held (the fake list always wins).
const pd::SourceCaps& shown_caps();
// The EPR PDOs of the source: the live EPR list when it has one, else the held one.
std::span<const std::uint32_t> epr_raw();

}  // namespace k2::offers
