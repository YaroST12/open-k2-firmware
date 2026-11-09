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

// ---- the two summary rows of the Sink tab, worded by who holds the number down ----
// Both fit the 19-cell value column of the Connection page in every case (checked in tests/test_pd_tabs.cpp).
// `warn` is set only when the CABLE is what holds the link below what the other parties could do: that is the one
// abnormal reading (ISA-101); a sink that draws less than it is offered is the normal state and stays neutral.
struct Cat {   // a bounded append into a caller's buffer, NUL-terminated by end()
    std::span<char> out;
    std::size_t n = 0;
    constexpr void p(char ch) { if (n + 1 < out.size()) out[n++] = ch; }
    constexpr void s(std::string_view v) { for (char ch : v) p(ch); }
    constexpr void w(std::int32_t x) { n += fmt_watts(out.subspan(n), x); }
    constexpr void r(std::int32_t x) { n += fmt_rate(out.subspan(n), x); }
    constexpr std::size_t end() { if (!out.empty()) out[n] = '\0'; return n; }
};

// "Pwr": "18W of 100W" (the sink draws part of the offer), "60W by cable" (the cable holds it down; "?" after it when
// the cable is only assumed to be 3 A), "100W" (one party known, or all agree), "?" (nothing known).
constexpr std::size_t power_line(std::span<char> out, const Terms &t, bool cable_assumed, bool &warn) {
    Cat c{out};
    warn = false;
    const Limited lim = limit(t);
    if (lim.value < 0) { c.s("?"); return c.end(); }
    c.w(lim.value);
    if (lim.limit == Who::Cable) {
        c.s(" by cable");
        if (cable_assumed) c.s("?");
        else warn = true;
    } else if (lim.limit == Who::Sink) {
        c.s(" of ");
        c.w(t.source >= 0 ? t.source : t.cable);
    }
    return c.end();
}

// The Type-C default budget of a link without a decoded PD contract: 5 V x the advertised current
// (`typec_ma`), labelled as a budget, never as a measurement: "up to 2.5W" (500 mA), "up to 7.5W"
// (1.5 A), "up to 15W" (3 A). Exact -- 5 V x mA is a whole mW, so the fraction prints without any
// rounding that changes the number ("up to 6.17W" for an odd 1234 mA). "?" when no current is
// advertised. Every output fits the 19-cell value column of the Connection page.
constexpr std::size_t budget_line(std::span<char> out, std::uint32_t typec_ma) {
    Cat c{out};
    const std::uint32_t mw = 5u * typec_ma;
    if (mw == 0) { c.s("?"); return c.end(); }
    c.s("up to ");
    auto digits = [&](std::uint32_t v) {
        char t[10]; int k = 0;
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        while (k) c.p(t[--k]);
    };
    digits(mw / 1000);
    const std::uint32_t rem = mw % 1000;
    if (rem != 0) {
        c.p('.');
        c.p(static_cast<char>('0' + rem / 100));
        if (rem % 100 != 0) c.p(static_cast<char>('0' + (rem / 10) % 10));
    }
    c.p('W');
    return c.end();
}

// "Data": "5G of 10G" (the device runs below what the cable carries), "5G by cable" (the cable is the slower one),
// "10G cable" (only the cable is known), "480M" (the device alone, or both agree), "?"; " +DP" when DisplayPort runs.
// Terms: `cable` = the e-marker's speed, `sink` = the device's speed (source is not used).
constexpr std::size_t data_line(std::span<char> out, const Terms &t, bool dp, bool &warn) {
    Cat c{out};
    warn = false;
    Terms d;
    d.cable = t.cable;
    d.sink = t.sink;
    const Limited lim = limit(d);
    if (lim.value < 0) { c.s("?"); return c.end(); }
    c.r(lim.value);
    if (lim.limit == Who::Cable) { c.s(" by cable"); warn = true; }
    else if (lim.limit == Who::Sink) { c.s(" of "); c.r(d.cable); }
    else if (d.sink < 0) c.s(" cable");
    if (dp) c.s(" +DP");
    return c.end();
}

// The e-marker's speed name without its filler, to leave room for the lane and Thunderbolt tags on the Cable tab:
// "USB 3.2 Gen2 (10G)" -> "3.2 Gen2 10G", "USB4 Gen3 (40G)" -> "USB4 40G", "Thunderbolt (40G)" -> "TB 40G",
// "USB 2.0 (480M)" -> "2.0 480M". An unknown wording is copied as it is, only without parentheses.
constexpr std::size_t speed_short(std::span<char> out, std::string_view s) {
    Cat c{out};
    auto starts = [&](std::string_view p) { return s.substr(0, p.size()) == p; };
    if (starts("Thunderbolt")) { c.s("TB"); s.remove_prefix(11); }
    else if (starts("USB ")) s.remove_prefix(4);
    const bool usb4 = starts("USB4");
    if (usb4) { c.s("USB4"); s.remove_prefix(4); }
    for (std::size_t i = 0; i < s.size(); i++) {
        if (usb4 && (s.substr(i, 5) == " Gen3" || s.substr(i, 5) == " Gen4")) { i += 4; continue; }
        if (s[i] == '(' || s[i] == ')') continue;
        c.s(s.substr(i, 1));
    }
    return c.end();
}

}  // namespace k2::pdtabs
