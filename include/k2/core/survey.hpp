// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "Who's attached" on the real hardware: runs the pure plan of k2/pd/pd_survey.hpp. One Refresh on the Connection page starts it
// (nothing active is ever sent before that); the main loop polls it and it hands each action to its sender:
//   Caps   the Refresh's own Get_Source_Cap (core/rescan.cpp, already asked by the page): Got when a Source_Capabilities came back
//   Wire   fusb302::survey_wire_start: Discover Identity / SVIDs / Modes, Get_Source_Cap_Extended, Get_Manufacturer_Info (the
//          pd::Reread sequencer, survey order, 220 ms between messages)
//   Epr    the sink's EPR_Get_Source_Cap through its one-message gate (fusb302::epr_info_request)
//   Cable  a check of the e-marker the engine sniffed; nothing is sent (the VCONN read stays its own tool)
// The view is forgotten when the attach session changes (a detach or a new source).
#pragma once
#include "k2/core/rescan.hpp"
#include "k2/pd/pd_survey.hpp"

namespace k2::survey {

// A Refresh just ran rescan::request() and got `r`. Builds the survey's gating from the live state and starts the run; a Busy
// request starts nothing. No contract (a CC cycle, PD-silent) leaves every field NotAsked; a sink inline blocks them all.
void on_refresh(rescan::Result r, bool charger_test);
// Main loop. `charger_test`: a charger test owns the line (a run in progress is abandoned, never sent over it).
void poll(bool charger_test);

const pd::Survey& state();
bool running();

}  // namespace k2::survey
