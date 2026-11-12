// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_tools.hpp"
#include "k2/pd/pd_emarker_plan.hpp"
#include "k2/ui/text/tools.hpp"

namespace k2 {

const char *tool_hint(int tool) { return text::tools::kHints[tool < 0 || tool >= kToolCount ? 0 : tool]; }

// Each tool owns its predicate (a function of its own) and declares what it is hurt by. Whether the far
// port is occupied is the same fact for all of them (SetupState::inline_mode); what it means differs:
//   Load:   the tool draws or asks for power, so a sink downstream would take what it asks for.
//   Inline: the tool only talks to the cable or the partner, so something downstream hides or hijacks the far end.
//   Nothing: the tool is passive, or measures under the user's own load.
namespace {
using namespace text::tools;

Readiness fc_trigger(const SetupState &st, bool) {   // no on-device inline override
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    return {};
}

Readiness pd_trigger(const SetupState &st, bool) {   // refuses without a PD source or with a load
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    if (!setup_hint(Tool::PdTrigger, st).ready) return {ToolLevel::Blocked, kTagNoPd, kNoPd};
    return {};
}

Readiness cable_r(const SetupState &, bool pm_online) {   // reads V and I from the power monitor; wants a load
    if (!pm_online) return {ToolLevel::Blocked, kTagNoPm, kNoPm};
    return {};
}

// Connection (reads what the engine holds; its active actions carry their own inline confirm), Black Box and PD diagnosis
// (passive reads), Report cards (read-only) and Diag (counters).
Readiness always_ready(const SetupState &, bool) { return {}; }

Readiness spoof(const SetupState &st, bool) {   // refused while a live sink is inline (Auto-CRC is off then)
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLiveSink};
    return {};
}

// PD Stress asks the charger for every level it offers: nothing may sit on the output, it reads VBUS and IBUS from the power
// monitor, and it needs a PD source list (the run itself refuses the same three; this only tags the row).
Readiness pd_stress(const SetupState &st, bool pm_online) {
    if (st.inline_mode) return {ToolLevel::Blocked, kTagLoad, kLoadAttached};
    if (!pm_online) return {ToolLevel::Blocked, kTagNoPm, kNoPm};
    if (!setup_hint(Tool::PdTrigger, st).ready) return {ToolLevel::Blocked, kTagNoPd, kNoPd};
    return {};
}

struct ToolRule {
    ToolHurt hurt;
    Readiness (*check)(const SetupState &, bool pm_online);
};

constexpr ToolRule kRules[kToolSlots] = {
    {ToolHurt::Load, fc_trigger},        // 0 FC trigger
    {ToolHurt::Load, pd_trigger},        // 1 PD trigger
    {ToolHurt::Load, spoof},             // 2 Spoof (shortcut into Cable read's Spoof list)
    {ToolHurt::Nothing, cable_r},        // 3 Cable R
    {ToolHurt::Nothing, always_ready},   // 4 Charger ID (reads; Refresh confirms on its own)
    {ToolHurt::Nothing, always_ready},   // 5 Cable read (its read carries the inline confirm)
    {ToolHurt::Nothing, always_ready},   // 6 Device ID (Identify carries the inline confirm)
    {ToolHurt::Nothing, always_ready},   // 7 Black Box
    {ToolHurt::Nothing, always_ready},   // 8 PD diagnosis (passive: reads the Black Box records)
    {ToolHurt::Nothing, always_ready},   // 9 Report cards
    {ToolHurt::Nothing, always_ready},   // 10 Diag
    {ToolHurt::Nothing, always_ready},   // 11 Device twins (the live proof refuses a live sink itself)
    {ToolHurt::Nothing, cable_r},        // 12 Loop probe (every reading comes from the power monitor; it refuses above 5.5 V itself)
    {ToolHurt::Load, pd_stress},         // 13 PD Stress
};

}  // namespace

ToolHurt tool_hurt_by(int tool) {
    return tool < 0 || tool >= kToolCount ? ToolHurt::Nothing : kRules[tool].hurt;
}

Readiness tool_readiness(int tool, const SetupState &st, bool pm_online) {
    if (tool < 0 || tool >= kToolCount) return {};
    return kRules[tool].check(st, pm_online);
}

}  // namespace k2
