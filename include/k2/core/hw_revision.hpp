// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Read-only hardware revision facts: the accelerometer identity, monitor chip and the decoded info-page minor.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/drivers/power_calc.hpp"

namespace k2::hw_revision {

inline constexpr std::size_t kTableBytes = 256;
inline constexpr std::size_t kPageBytes = 1024;

struct DecodedMinor {
    bool serial_valid = false;
    std::uint8_t minor = 0;
};

// Pure decoder. Every page byte is first substituted through table; only the read-only serial and minor fields
// are interpreted here. The serial itself never leaves the decoder.
constexpr DecodedMinor decode_minor(std::span<const std::uint8_t> table,
                                    std::span<const std::uint8_t> page) {
    if (table.size() < kTableBytes || page.size() < kPageBytes) return {};
    bool blank = page[0xE4] == 0xFF;
    for (std::size_t i = 0xFA; i <= 0xFF && blank; ++i) blank = page[i] == 0xFF;
    if (blank) return {};

    std::uint32_t serial = 0;
    for (std::size_t i = 0; i < 6; ++i) {
        const std::uint8_t digit = table[page[0xFF - i]];
        if (digit < '0' || digit > '9') return {};
        serial = serial * 10u + static_cast<unsigned>(digit - '0');
    }
    const std::uint8_t b = table[page[0xE4]];
    const std::uint8_t minor = serial < 100200u ? 1
                             : serial >= 200000u && serial <= 200040u ? 3
                             : static_cast<std::uint8_t>(b + 2u);
    return {true, minor};
}

struct Info {
    std::uint8_t accelerometer_id = 0;
    power::Chip monitor = power::Chip::None;
    bool minor_valid = false;
    std::uint8_t minor = 0;
};

// Boot-time capture from the read-only bootloader table and info page. No writes and no serial exposure.
[[gnu::noinline]] void init(std::uint8_t accelerometer_id, power::Chip monitor);
Info current();
std::string_view text();  // Cached "Q6.03" (or "N?.--" when facts are missing).

}  // namespace k2::hw_revision
