// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Settings > Display: the brightness step, the two pick lists and the text of the values (pure, host-tested).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/idle.hpp"
#include "k2/core/strpack.hpp"

namespace k2::uidisp {

inline constexpr int kStepPct = 10, kMinPct = idle::kMinSavedPct;
inline constexpr int kMaxPct = idle::kMaxPct;

// The pick lists (0 = never). A value set from the remote that is not listed shows as text and leaves every radio off.
inline constexpr std::array<std::uint16_t, 5> kDimAfter{0, 30, 60, 120, 300};
inline constexpr std::array<std::uint16_t, 5> kOffAfter{0, 120, 300, 600, 1800};
inline constexpr std::array<const char*, 5> kDimNames{K2_P("Never"), "30 s", "60 s", K2_P("2 min"), K2_P("5 min")};
inline constexpr std::array<const char*, 5> kOffNames{K2_P("Never"), K2_P("2 min"), K2_P("5 min"), K2_P("10 min"), K2_P("30 min")};

// One 10 % step up (dir > 0) or down, from any level; stays in 10..100.
constexpr std::uint8_t step_brightness(std::uint8_t cur, int dir) {
    int v = cur;
    v = dir > 0 ? (v / kStepPct + 1) * kStepPct : ((v + kStepPct - 1) / kStepPct - 1) * kStepPct;
    return static_cast<std::uint8_t>(v < kMinPct ? kMinPct : v > kMaxPct ? kMaxPct : v);
}

constexpr bool can_step(std::uint8_t cur, int dir) { return step_brightness(cur, dir) != cur; }

constexpr int index_of(std::span<const std::uint16_t> options, std::uint16_t secs) {
    for (std::size_t i = 0; i < options.size(); i++)
        if (options[i] == secs) return static_cast<int>(i);
    return -1;
}

namespace detail {
constexpr std::size_t put_num(std::span<char> out, std::size_t n, unsigned v) {
    char d[5];
    int k = 0;
    do { d[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v && k < 5);
    while (k && n + 1 < out.size()) out[n++] = d[--k];
    return n;
}
constexpr std::size_t put_str(std::span<char> out, std::size_t n, std::string_view s) {
    if !consteval {
        strpack::for_chars(s, [&](char c) { if (n + 1 < out.size()) out[n++] = c; });
        return n;
    }
    for (char c : s) if (n + 1 < out.size()) out[n++] = c;
    return n;
}
}  // namespace detail

// "Never", "30 s", "2 min", "1 h" (brief: "off", "30s", "2m", "1h" for the Settings row). From 100 minutes on it counts hours, rounded down.
constexpr std::size_t format_span(std::span<char> out, std::uint16_t secs, bool brief) {
    if (out.empty()) return 0;
    std::size_t n = 0;
    if (secs == 0) {
        n = detail::put_str(out, n, brief ? "off" : K2_P("Never"));
    } else if (secs < 120) {
        n = detail::put_num(out, n, secs);
        n = detail::put_str(out, n, brief ? "s" : " s");
    } else {
        const unsigned m = secs / 60u;
        const bool hours = m >= 100 || m % 60 == 0;
        n = detail::put_num(out, n, hours ? m / 60 : m);
        n = detail::put_str(out, n, hours ? (brief ? "h" : " h") : (brief ? "m" : " min"));
    }
    out[n] = '\0';
    return n;
}

constexpr std::size_t format_percent(std::span<char> out, unsigned pct) {
    if (out.empty()) return 0;
    std::size_t n = detail::put_num(out, 0, pct);
    n = detail::put_str(out, n, "%");
    out[n] = '\0';
    return n;
}

// The Settings row's tail: "100%, 60s/5m" (brightness, dim after / screen off after).
constexpr std::size_t format_summary(std::span<char> out, const idle::Config& c) {
    if (out.empty()) return 0;
    std::size_t n = detail::put_num(out, 0, c.brightness);
    n = detail::put_str(out, n, "%, ");
    char t[8];
    n = detail::put_str(out, n, std::string_view(t, format_span(t, c.dim_after_s, true)));
    n = detail::put_str(out, n, "/");
    n = detail::put_str(out, n, std::string_view(t, format_span(t, c.off_after_s, true)));
    out[n] = '\0';
    return n;
}

}  // namespace k2::uidisp
