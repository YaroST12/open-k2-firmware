// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered idle settings: /idle.cfg, 11 bytes "K2ID", brightness, dim brightness, dim_after s (u16 LE), off_after s
// (u16 LE), check (XOR of the ten bytes and 0xA5). Anything damaged = defaults. The wake-on-remote flag is not stored.
// Its own file: /prefs.cfg is only decoded at exactly eight bytes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/core/fs.hpp"
#include "k2/core/idle.hpp"

namespace k2::idle_cfg {

inline constexpr const char* kPath = "/idle.cfg";
inline constexpr std::array<std::uint8_t, 4> kMagic{'K', '2', 'I', 'D'};
inline constexpr std::size_t kSize = 11;

constexpr std::uint8_t check(const std::array<std::uint8_t, kSize>& r) {
    std::uint8_t x = 0xA5;
    for (std::size_t i = 0; i + 1 < kSize; i++) x = static_cast<std::uint8_t>(x ^ r[i]);
    return x;
}

constexpr std::array<std::uint8_t, kSize> encode(const idle::Config& c) {
    std::array<std::uint8_t, kSize> r{kMagic[0], kMagic[1], kMagic[2], kMagic[3], c.brightness, c.dim_brightness,
                                     static_cast<std::uint8_t>(c.dim_after_s), static_cast<std::uint8_t>(c.dim_after_s >> 8),
                                     static_cast<std::uint8_t>(c.off_after_s), static_cast<std::uint8_t>(c.off_after_s >> 8), 0};
    r[kSize - 1] = check(r);
    return r;
}

// The persisted part of a Config is equal.
constexpr bool same(const idle::Config& a, const idle::Config& b) {
    return a.brightness == b.brightness && a.dim_brightness == b.dim_brightness && a.dim_after_s == b.dim_after_s &&
           a.off_after_s == b.off_after_s;
}

constexpr idle::Config decode(const std::array<std::uint8_t, kSize>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++)
        if (r[i] != kMagic[i]) return {};
    if (r[kSize - 1] != check(r) || r[4] > idle::kMaxPct || r[5] > idle::kMaxPct) return {};
    idle::Config c;
    c.brightness = r[4];
    c.dim_brightness = r[5];
    c.dim_after_s = static_cast<std::uint16_t>(r[6] | (r[7] << 8));
    c.off_after_s = static_cast<std::uint16_t>(r[8] | (r[9] << 8));
    return c;
}

inline idle::Config load(fs::Fs& f) {
    std::array<std::uint8_t, kSize> r{};
    std::size_t n = 0;
    if (f.read_file(kPath, r, n) != fs::Err::Ok || n != r.size()) return {};
    return decode(r);
}

inline bool save(fs::Fs& f, const idle::Config& c) {
    const auto r = encode(c);
    return f.write_file(kPath, r) == fs::Err::Ok;
}

}  // namespace k2::idle_cfg
