// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD limiter Hard Reset comeback record: lets a bus-powered K2 survive a Hard Reset triggered while applying or updating
// a PD limit and re-arm passive limit injection early on the next boot before the charger's first Source_Capabilities.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/core/bootmode.hpp"
#include "k2/pd/pd_limiter.hpp"

namespace k2::limcomeback {

inline constexpr const char* kPath = "/lim.rec";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'L', 'M'};
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::uint8_t kSalt = 0xA7;
inline constexpr std::size_t kSize = 24;   // 4 magic + u8 ver + u8 flags + 3*(u16 mv, u16 ma) + u32 limit_mw + u8 check

constexpr std::uint8_t checksum(std::span<const std::uint8_t> body) {
    std::uint8_t x = kSalt;
    for (const std::uint8_t b : body) x ^= b;
    return x;
}

inline std::array<std::uint8_t, kSize> encode(const pd::LimConfig& c) {
    std::array<std::uint8_t, kSize> r{};
    for (std::size_t i = 0; i < 4; i++) r[i] = kMagic[i];
    r[4] = kVersion;
    const pd::LimTech t[3] = {c.fixed, c.pps, c.avs};
    for (std::size_t i = 0; i < 3; i++) {
        if (t[i].enabled) r[5] |= static_cast<std::uint8_t>(1u << i);
        r[6 + i * 4] = static_cast<std::uint8_t>(t[i].max_mv & 0xFF);
        r[7 + i * 4] = static_cast<std::uint8_t>(t[i].max_mv >> 8);
        r[8 + i * 4] = static_cast<std::uint8_t>(t[i].max_ma & 0xFF);
        r[9 + i * 4] = static_cast<std::uint8_t>(t[i].max_ma >> 8);
    }
    for (std::size_t i = 0; i < 4; i++) r[18 + i] = static_cast<std::uint8_t>((c.limit_mw >> (8 * i)) & 0xFF);
    r[23] = checksum({r.data() + 4, kSize - 5});
    return r;
}

inline std::optional<pd::LimConfig> decode(std::span<const std::uint8_t> r) {
    if (r.size() != kSize) return std::nullopt;
    for (std::size_t i = 0; i < 4; i++)
        if (r[i] != kMagic[i]) return std::nullopt;
    if (r[4] != kVersion || r[22] != 0 || (r[5] & ~0x07u) != 0) return std::nullopt;
    if (checksum(r.subspan(4, kSize - 5)) != r[23]) return std::nullopt;
    pd::LimConfig c{};
    pd::LimTech* const dst[3] = {&c.fixed, &c.pps, &c.avs};
    for (std::size_t i = 0; i < 3; i++) {
        dst[i]->enabled = (r[5] & (1u << i)) != 0;
        dst[i]->max_mv = static_cast<std::uint16_t>(r[6 + i * 4] | (r[7 + i * 4] << 8));
        dst[i]->max_ma = static_cast<std::uint16_t>(r[8 + i * 4] | (r[9 + i * 4] << 8));
    }
    for (std::size_t i = 0; i < 4; i++) c.limit_mw |= static_cast<std::uint32_t>(r[18 + i]) << (8 * i);
    if (!pd::lim_config_valid(c)) return std::nullopt;
    return c;
}

constexpr bool should_restore(bootmode::Mode keys, bool warm, bool has_record, bool canary_ok) {
    return keys == bootmode::Mode::Plain && !warm && has_record && canary_ok;
}

// Firmware bindings (src/core/lim_comeback.cpp; host UI stubs implement light no-ops/fakes in tests/ui_stubs.cpp).
bool save(const pd::LimConfig& cfg);
void clear();
bool begin(bootmode::Mode keys, bool warm);
bool pending();
const pd::LimConfig& config();
void consume();

}  // namespace k2::limcomeback
