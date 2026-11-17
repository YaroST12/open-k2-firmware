// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>
#include <optional>

#include "k2/core/bss_zero.hpp"

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
    // The one instance is Ui::gravity_ in the .bss global Ui.
    uint8_t rotation_ K2_BSS_ZERO;
    uint8_t candidate_ K2_BSS_ZERO;
    bool pending_ K2_BSS_ZERO;
    uint32_t since_ms_ K2_BSS_ZERO;
    uint32_t last_ms_ K2_BSS_ZERO;
};

}  // namespace k2::orientation
