// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home flow-rail animation: pure mapping from measured power and the allowed maximum to the rail's
// density (bright heads) and speed, plus the fixed-point phase accumulator. No hardware, host-testable.
#pragma once
#include <cstdint>

namespace k2::flow_anim {

inline constexpr int kChevrons = 9;                      // rail length (ui_pages.hpp kRailChevrons)
inline constexpr int kTiers[6] = {15, 60, 100, 140, 165, 240};   // W: the PD power classes
inline constexpr int kDefaultMaxW = 15;                  // no PD contract / advertised maximum

struct Params {
    uint8_t heads;       // bright heads on the rail, 0 = no motion
    uint16_t speed_q8;   // chevrons per second, 24.8 fixed point
};

// Index of the smallest tier that holds max_w (0 = 15 W ... 5 = 240 W); above 240 W stays 5.
constexpr int tier_index(int max_w) {
    for (int i = 0; i < 6; i++) if (max_w <= kTiers[i]) return i;
    return 5;
}

// |power| as permille of the allowed maximum, clamped 0..1000; max_w <= 0 means the 15 W default.
constexpr int load_permille(int32_t power_mw, int max_w) {
    const int64_t mag = power_mw < 0 ? -static_cast<int64_t>(power_mw) : power_mw;
    const int64_t den = static_cast<int64_t>(max_w > 0 ? max_w : kDefaultMaxW) * 1000;
    const int64_t pm = mag * 1000 / den;
    return pm > 1000 ? 1000 : static_cast<int>(pm);
}

// Density and speed for a load. Speed runs from 2 chevrons/s to a ceiling of 6 + tier chevrons/s linearly
// with load, so the same fraction of a bigger charger looks a little busier. Heads: 1, 2 above 25 %,
// 3 above 60 %, 4 above 85 % on 140 W and bigger. Zero power = no motion.
constexpr Params compute(int32_t power_mw, int max_w) {
    const int f = load_permille(power_mw, max_w);
    if (power_mw == 0) return {0, 0};
    const int tier = tier_index(max_w > 0 ? max_w : kDefaultMaxW);
    const int top = 6 + tier;
    const int heads = 1 + (f >= 250) + (f >= 600) + (tier >= 3 && f >= 850);
    const int speed = 2 * 256 + (top - 2) * 256 * f / 1000;
    return {static_cast<uint8_t>(heads), static_cast<uint16_t>(speed)};
}

// Phase accumulator over one ring (kChevrons << 8, 24.8). Monotonic, wraps once per lap.
constexpr uint32_t advance(uint32_t phase_q8, uint32_t speed_q8, uint32_t dt_ms) {
    return (phase_q8 + speed_q8 * dt_ms / 1000) % (static_cast<uint32_t>(kChevrons) << 8);
}

// Brightness of chevron k (0 = top): 0 dim, 1 mid, 2 bright. `heads` heads are spread evenly round the
// ring, each with a two-step trail behind it, and move toward the top or the bottom.
constexpr int rail_level(int k, uint32_t phase_q8, int heads, bool toward_top) {
    const uint32_t ring = static_cast<uint32_t>(kChevrons) << 8;
    int best = 0;
    for (int i = 0; i < heads; i++) {
        const int pos = static_cast<int>(((phase_q8 + static_cast<uint32_t>(i) * ring / heads) % ring) >> 8);
        const int head = toward_top ? kChevrons - 1 - pos : pos;
        const int d = toward_top ? k - head : head - k;   // steps behind the head
        const int lv = d >= 0 && d < 3 ? 2 - d : 0;
        if (lv > best) best = lv;
    }
    return best;
}

// ---- status-bar chevrons: three chevrons and one gap in a ring of four ----
inline constexpr int kBarChevrons = 3;
inline constexpr int kBarRing = kBarChevrons + 1;

// Same phase accumulator as advance(), over the four-step bar ring.
constexpr uint32_t advance_bar(uint32_t phase_q8, uint32_t speed_q8, uint32_t dt_ms) {
    return (phase_q8 + speed_q8 * dt_ms / 1000) % (static_cast<uint32_t>(kBarRing) << 8);
}

// Brightness 0 dim, 1 mid, 2 bright of bar chevron k (0..2, left to right in the bar's own frame). One
// bright head runs round the ring toward higher k when `forward`, toward lower k otherwise; `heads`
// (Params::heads) lengthens the trail from two chevrons to three at high load.
constexpr int bar_level(int k, uint32_t phase_q8, int heads, bool forward) {
    const int pos = static_cast<int>((phase_q8 >> 8) % kBarRing);
    const int head = forward ? pos : kBarChevrons - 1 - pos;
    const int d = forward ? head - k : k - head;   // steps behind the head
    const int trail = heads >= 3 ? 3 : 2;
    return d >= 0 && d < trail ? 2 - d : 0;
}

}  // namespace k2::flow_anim
