// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Remembered idle settings: /idle.cfg, 11 bytes "K2ID", brightness, dim brightness, dim_after s (u16 LE), off_after s
// (u16 LE), check (XOR of the ten bytes and 0xA5). Anything damaged = defaults. The wake-on-remote flag is not stored.
// The two levels are stored and read back as 10..100: a live 0 % (remote) never reaches the file, see persisted().
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

constexpr std::uint8_t visible(std::uint8_t pct) { return pct < idle::kMinSavedPct ? idle::kMinSavedPct : pct; }

constexpr std::array<std::uint8_t, kSize> encode(const idle::Config& c) {
    std::array<std::uint8_t, kSize> r{kMagic[0], kMagic[1], kMagic[2], kMagic[3], visible(c.brightness), visible(c.dim_brightness),
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

// What to save for a live config: a level under the minimum is a runtime-only override (remote 0 %), so the stored level stays.
constexpr idle::Config persisted(const idle::Config& live, const idle::Config& saved) {
    idle::Config p = live;
    if (live.brightness < idle::kMinSavedPct) p.brightness = saved.brightness;
    if (live.dim_brightness < idle::kMinSavedPct) p.dim_brightness = saved.dim_brightness;
    return p;
}

constexpr idle::Config decode(const std::array<std::uint8_t, kSize>& r) {
    for (std::size_t i = 0; i < kMagic.size(); i++)
        if (r[i] != kMagic[i]) return {};
    if (r[kSize - 1] != check(r) || r[4] > idle::kMaxPct || r[5] > idle::kMaxPct) return {};
    idle::Config c;
    c.brightness = visible(r[4]);   // an older build could save 0 %
    c.dim_brightness = visible(r[5]);
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
