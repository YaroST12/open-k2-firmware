// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>
#include <optional>

namespace k2::orientation {

// Screen axes in the physical upright posture: x right, y down, z out of the screen.
struct Gravity { int16_t x, y, z; };

// Gravity must be supplied in mg, already mapped from verified sensor axes to screen axes.
std::optional<uint8_t> classify(Gravity gravity);

class Tracker {
public:
    static constexpr uint32_t kStableMs = 300;
    std::optional<uint8_t> update(std::optional<Gravity> gravity, uint32_t now_ms);
    uint8_t rotation() const { return rotation_; }

private:
    uint8_t rotation_ = 0;
    uint8_t candidate_ = 0;
    bool pending_ = false;
    uint32_t since_ms_ = 0;
    uint32_t last_ms_ = 0;
};

}  // namespace k2::orientation
