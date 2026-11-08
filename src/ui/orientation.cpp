// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/orientation.hpp"

namespace k2::orientation {
namespace {
int32_t magnitude(int16_t v) { return v < 0 ? -static_cast<int32_t>(v) : v; }
}

std::optional<uint8_t> classify(Gravity g) {
    const int32_t x = magnitude(g.x), y = magnitude(g.y), z = magnitude(g.z);
    const int32_t dominant = x > y ? x : y;
    const int32_t other = x > y ? y : x;
    const int64_t norm = static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y +
                         static_cast<int64_t>(z) * z;
    // Reject face-up/down, diagonals, free fall and shocks rather than selecting a noisy quadrant.
    if (dominant < 650 || dominant > 1250 || dominant - other < 250 || z > 650 ||
        norm < 700 * 700 || norm > 1300 * 1300)
        return std::nullopt;
    if (x > y) return g.x > 0 ? 3 : 1;
    return g.y > 0 ? 0 : 2;
}

std::optional<uint8_t> Tracker::update(std::optional<Gravity> gravity, uint32_t now_ms) {
    std::optional<uint8_t> next;
    if (gravity) next = classify(*gravity);
    const bool gap = pending_ && now_ms - last_ms_ > 150;
    last_ms_ = now_ms;
    if (!next || *next == rotation_) {
        pending_ = false;
        return std::nullopt;
    }
    if (!pending_ || gap || candidate_ != *next) {
        pending_ = true;
        candidate_ = *next;
        since_ms_ = now_ms;
    } else if (now_ms - since_ms_ >= kStableMs) {
        rotation_ = candidate_;
        pending_ = false;
        return rotation_;
    }
    return std::nullopt;
}
}  // namespace k2::orientation
