// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD page logic (pure, host-tested in tests/test_pd_tabs.cpp): the Source / Cable / Sink tab state, the
// double-OK gate before an active query while inline, and the three-party limiter behind the match strip.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace k2::pdtabs {

enum class Tab : std::uint8_t { Source, Cable, Sink };
inline constexpr int kTabs = 3;
constexpr Tab next(Tab t) { return static_cast<Tab>((static_cast<int>(t) + 1) % kTabs); }
constexpr Tab prev(Tab t) { return static_cast<Tab>((static_cast<int>(t) + kTabs - 1) % kTabs); }

// OK on the PD page. Not inline: refresh at once. Inline (a real sink is downstream): the first OK arms
// "Query live link? OK=yes", a second OK within kConfirmMs asks, anything later arms again.
enum class OkStep : std::uint8_t { Refresh, Confirm, Query };
inline constexpr std::uint32_t kConfirmMs = 4000;
struct OkGate {
    std::uint32_t until = 0;   // 0 = not armed
    constexpr bool armed(std::uint32_t now) const { return until != 0 && static_cast<std::int32_t>(until - now) > 0; }
    constexpr OkStep press(bool inline_mode, std::uint32_t now) {
        if (!inline_mode) { until = 0; return OkStep::Refresh; }
        if (armed(now)) { until = 0; return OkStep::Query; }
        until = now + kConfirmMs;
        if (until == 0) until = 1;
        return OkStep::Confirm;
    }
    constexpr void reset() { until = 0; }
};

// ---- the match strip: three parties, the smallest known term limits ----
enum class Who : std::uint8_t { None, Source, Cable, Sink };
inline constexpr std::int32_t kUnknown = -1;
struct Terms { std::int32_t source = kUnknown, cable = kUnknown, sink = kUnknown; };
struct Limited {
    std::int32_t value = kUnknown;   // the smallest known term, kUnknown when none is known
    Who limit = Who::None;           // the party holding it below another known term; None when all known terms agree
};
constexpr Limited limit(const Terms &t) {
    const std::int32_t v[3] = {t.source, t.cable, t.sink};
    Limited r;
    int known = 0, lo = -1;
    for (int i = 0; i < 3; i++) {
        if (v[i] < 0) continue;
        known++;
        if (lo < 0 || v[i] < v[lo]) lo = i;
    }
    if (lo < 0) return r;
    r.value = v[lo];
    bool smaller = false;
    for (int i = 0; i < 3; i++) if (v[i] >= 0 && v[i] > v[lo]) smaller = true;
    if (known > 1 && smaller) r.limit = static_cast<Who>(lo + 1);
    return r;
}

// Data rate in Mbit/s as a short word: "480M" "5G" "10G" "20G" "40G" "80G", "?" when unknown.
constexpr std::size_t fmt_rate(std::span<char> out, std::int32_t mbps) {
    std::size_t n = 0;
    auto put = [&](char c) { if (n + 1 < out.size()) out[n++] = c; };
    auto num = [&](std::uint32_t v) {
        char t[10]; int k = 0;
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    };
    if (mbps < 0) put('?');
    else if (mbps >= 1000) { num(static_cast<std::uint32_t>(mbps / 1000)); put('G'); }
    else { num(static_cast<std::uint32_t>(mbps)); put('M'); }
    if (!out.empty()) out[n] = '\0';
    return n;
}
// Watts as an integer with "W": "100W", "?" when unknown.
constexpr std::size_t fmt_watts(std::span<char> out, std::int32_t w) {
    std::size_t n = 0;
    auto put = [&](char c) { if (n + 1 < out.size()) out[n++] = c; };
    if (w < 0) put('?');
    else {
        char t[10]; int k = 0;
        std::uint32_t v = static_cast<std::uint32_t>(w);
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
        put('W');
    }
    if (!out.empty()) out[n] = '\0';
    return n;
}

// Cable speed field of the e-marker (Cable VDO 1 bits 2:0) in Mbit/s; Intel 0x8087 cables from Gen2 up are Thunderbolt 40G.
constexpr std::int32_t cable_mbps(std::uint8_t bits, std::uint16_t vid) {
    if (vid == 0x8087 && bits >= 2) return 40000;
    switch (bits) {
    case 1: return 5000;
    case 2: return 10000;
    case 3: return 40000;
    case 4: return 80000;
    default: return 480;
    }
}

}  // namespace k2::pdtabs
