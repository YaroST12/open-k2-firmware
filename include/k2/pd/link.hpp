// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Live view of what is plugged in behind the K2: is a PD contract running, is a sink drawing load?
// Owns the decision for the passive classifier (verdict "PD" / "USB DATA") and the active fast-charge
// triggers (refuse unless explicitly overridden). It only reads other drivers through their APIs
// (fusb302 caps, power monitor); the rule itself is the pure link_guard.hpp.
#pragma once
#include "k2/pd/link_guard.hpp"

namespace k2::link {

// FUSB302 holds a CC line and a decoded Source_Capabilities.
bool pd_active();

// The UI feeds pd::Link::inline_mode() here every tick; it is the owner of the "inline" verdict.
void set_inline_mode(bool on);

// Live probe of the FUSB302 and the power monitor.
Probe probe_now();

// CC attached, VBUS > 5.5 V, load flowing, caps not decoded: a PD contract almost certainly runs.
bool pd_likely_now();

// Reason an active operation must not touch the lines now; Reason::None when the output is idle.
Reason inline_now();

}  // namespace k2::link
