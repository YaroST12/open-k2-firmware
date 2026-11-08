// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <cstdint>
#include <optional>
#include "k2/ui/orientation.hpp"

namespace k2::accelerometer {

// LIS2DH12/SC7A20-compatible part at 0x18 on PB8/PB9, not a gyroscope.
class Driver {
public:
    static constexpr uint32_t kPollMs = 100, kRetryMs = 1000;
    bool init(uint32_t now_ms);
    // True only when a sample or failed attempt is available; false between bounded polls.
    bool poll(uint32_t now_ms);
    bool online() const { return online_; }
    std::optional<orientation::Gravity> read() const { return sample_; }

private:
    bool online_ = false;
    uint32_t last_ms_ = 0;
    std::optional<orientation::Gravity> sample_;
};

}  // namespace k2::accelerometer
