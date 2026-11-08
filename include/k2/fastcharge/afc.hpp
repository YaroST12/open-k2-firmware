// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Samsung AFC command bytes: (V_code << 4) | I_code, sent as one byte three times
// 10 ms apart after the ping exchange. Pure.
#pragma once
#include <cstdint>
#include <optional>

namespace k2::afc {

inline constexpr int kSends = 3;
inline constexpr std::uint32_t kSendGapMs = 10;

// V_code 0x4 = 9 V, 0x7 = 12 V; I_code 0x6 = 1.65 A, 0x9 = 2.10 A. 5 V is the reset, so no byte.
constexpr std::optional<std::uint8_t> afc_byte(int mv) {
    switch (mv) {
    case 9000:  return std::uint8_t{0x46};
    case 12000: return std::uint8_t{0x79};
    default:    return std::nullopt;
    }
}

}  // namespace k2::afc
