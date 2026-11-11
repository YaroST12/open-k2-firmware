// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_tools.hpp"
#include "k2/pd/pd_emarker_plan.hpp"

namespace k2 {

const char *tool_hint(int tool) {
    static constexpr const char *kHints[kToolCount] = {
        "charger on top or bottom", "PD charger, either port", "act as a saved cable", "charger + cable + load",
        "source, cable and device", "passive PD recorder", "saved charger results", "USB, PD and system state",
        "will it charge my device", "cable into own HID port"};
    return kHints[tool < 0 || tool >= kToolCount ? 0 : tool];
}

// Each tool owns its predicate (a function of its own) and declares what it is hurt by. Whether the far
// port is occupied is the same fact for all of them (SetupState::inline_mode); what it means differs:
//   Load:   the tool draws or asks for power, so a sink downstream would take what it asks for.
//   Inline: the tool only talks to the cable or the partner, so something downstream hides or hijacks the far end.
//   Nothing: the tool is passive, or measures under the user's own load.
namespace {

Readiness fc_trigger(const SetupState &st, bool) {   // no on-device inline override
    if (st.inline_mode) return {ToolLevel::Blocked, "LOAD", "Load attached: unplug it"};
    return {};
}

Readiness pd_trigger(const SetupState &st, bool) {   // refuses without a PD source or with a load
    if (st.inline_mode) return {ToolLevel::Blocked, "LOAD", "Load attached: unplug it"};
    if (!setup_hint(Tool::PdTrigger, st).ready) return {ToolLevel::Blocked, "NO PD", "No PD source on either port"};
    return {};
}

Readiness cable_r(const SetupState &, bool pm_online) {   // reads V and I from the power monitor; wants a load
    if (!pm_online) return {ToolLevel::Blocked, "NO PM", "Power meter not found"};
    return {};
}

// Connection (reads what the engine holds; its active actions carry their own inline confirm), Black Box (passive
// reads), Report cards (read-only) and Diag (counters).
Readiness always_ready(const SetupState &, bool) { return {}; }

Readiness spoof(const SetupState &st, bool) {   // refused while a live sink is inline (Auto-CRC is off then)
    if (st.inline_mode) return {ToolLevel::Blocked, "LOAD", "Live sink: unplug load"};
    return {};
}

struct ToolRule {
    ToolHurt hurt;
    Readiness (*check)(const SetupState &, bool pm_online);
};

constexpr ToolRule kRules[kToolCount] = {
    {ToolHurt::Load, fc_trigger},        // 0 FC trigger
    {ToolHurt::Load, pd_trigger},        // 1 PD trigger
    {ToolHurt::Load, spoof},             // 2 Spoof (shortcut into Connection's Cable tab)
    {ToolHurt::Nothing, cable_r},        // 3 Cable R
    {ToolHurt::Nothing, always_ready},   // 4 Connection
    {ToolHurt::Nothing, always_ready},   // 5 Black Box
    {ToolHurt::Nothing, always_ready},   // 6 Report cards
    {ToolHurt::Nothing, always_ready},   // 7 Diag
    {ToolHurt::Nothing, always_ready},   // 8 Device twins (the live proof refuses a live sink itself)
    {ToolHurt::Nothing, cable_r},        // 9 Loop probe (every reading comes from the power monitor; it refuses above 5.5 V itself)
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
