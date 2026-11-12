// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools hub v2, pure logic: which of the tools can do its full job right now, the tag and the
// footer reason for it. Every verdict is derived from the same setup_hint() the tool's own page uses,
// so the hub never disagrees with the refusal inside the tool.
#pragma once
#include <cstdint>
#include "k2/core/build_config.h"
#include "k2/ui/ui_pdtrig.hpp"

namespace k2 {

enum class ToolLevel : uint8_t { Ready, Caution, Blocked };

struct Readiness {
    ToolLevel level = ToolLevel::Ready;
    const char *tag = "READY";       // at most 6 characters, shown in the selected row's pill
    const char *reason = "Ready";    // footer sentence, at most 28 characters
};

constexpr int kToolSlots = 12;       // the per-tool tables: PD Stress is the last slot, so a K2_PDSTRESS=0 build only shows one row less
constexpr int kToolCount = K2_PDSTRESS ? 12 : 11;   // rows shown: FC trigger, PD trigger, Spoof, Cable R, Connection, Black Box, PD diagnosis, Report cards, Diag, Device twins, Loop probe, PD Stress (ToolsPage::tool_menu maps to Menu)
constexpr int kToolSpoof = 2;        // the Spoof row opens Connection on the Cable tab's Spoof list, not a menu of its own
constexpr int kToolLoopProbe = 10;   // the HID-cable loop probe (needs the power monitor, like Cable R)
constexpr int kToolPdStress = 11;    // renegotiation stress: a PD charger, the power monitor, nothing on the output
constexpr int kToolRows = 11;        // one row per tool (the list scrolls: not all of them fit above the footer at once)

// Port-neutral setup hint under the tool name (at most 24 characters; never says which port).
const char *tool_hint(int tool);

// What the tool is hurt by when the far port is occupied (SetupState::inline_mode). Load: it draws or
// asks for power (the tag is LOAD, blocked). Inline: it only talks to the cable or the partner, so a
// device downstream hides or hijacks the far end (tag INLINE, caution). Nothing: passive or load-tolerant.
enum class ToolHurt : uint8_t { Nothing, Load, Inline };
ToolHurt tool_hurt_by(int tool);

// `pm_online`: the power monitor answers (Cable R needs it).
Readiness tool_readiness(int tool, const SetupState &st, bool pm_online);

}  // namespace k2
