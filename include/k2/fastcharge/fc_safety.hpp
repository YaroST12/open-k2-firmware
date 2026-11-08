// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Voltage safety rules shared by every fast-charge trigger (pure): the active band around a target
// and the abort guard. All values in millivolts.
#pragma once
#include <algorithm>
#include <cstdint>

namespace k2::fc {

// Any request above this needs an explicit confirmation from the user.
inline constexpr int kConfirmAboveMv = 5000;

// Measured VBUS counts as "reached the target" inside [85 %, 110 %] of it.
constexpr bool in_band(int target_mv, int vbus_mv) {
    return vbus_mv >= target_mv * 85 / 100 && vbus_mv <= target_mv * 110 / 100;
}

// Abort rule: VBUS above target + 10 % (the caller passes the highest target of the last second,
// so a falling request does not trip it) or above the ceiling.
constexpr bool over_guard(int guard_target_mv, int vbus_mv) { return vbus_mv > guard_target_mv * 110 / 100; }

// A request is clamped to the ceiling; 0 when there is no ceiling (no power monitor).
constexpr int clamp_to_ceiling(int mv, int ceiling_mv) { return ceiling_mv <= 0 ? 0 : std::min(mv, ceiling_mv); }

}  // namespace k2::fc
