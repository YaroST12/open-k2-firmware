// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Readout formatting shared by the Home page and the status bar (pure, no hardware, host-tested in
// tests/test_readout_fmt.cpp).
//
// Rule: three total digits, decimal point floats, magnitudes only (a direction is never a sign), and
// no milli units: 15.0V  5.08V  2.04A  0.12A  30.6W  100W  0.62W.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/math_opt.hpp"

namespace k2::readout {

namespace detail {
struct Out {
    std::span<char> s;
    std::size_t n = 0;
    constexpr void put(char c) { if (n + 1 < s.size()) s[n++] = c; }
    constexpr void put(std::string_view v) { for (char c : v) put(c); }
    constexpr void digits(uint32_t v, int width) {   // zero padded to `width`
        char t[10];
        int k = 0;
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
        for (int i = k; i < width; i++) put('0');
        while (k) put(t[--k]);
    }
    constexpr std::size_t end() { if (!s.empty()) s[n] = '\0'; return n; }
};
}  // namespace detail

// |milli| (thousandths of a unit) as three total digits plus `unit` (0 = none). Returns the length;
// the text is truncated to the buffer and NUL terminated. Rounds half up; a carry (9.995 -> 10.0)
// moves to the next, coarser format so the digit count stays three.
constexpr std::size_t fmt3(std::span<char> out, long long milli, char unit) {
    detail::Out o{out};
    uint64_t v = static_cast<uint64_t>(milli < 0 ? -milli : milli);
    if (v < 9995) {                                   // d.dd
        const uint32_t c = (static_cast<uint32_t>(v) + 5u) / 10u;
        o.digits(c / 100, 1); o.put('.'); o.digits(c % 100, 2);
    } else if (v < 99950) {                           // dd.d
        const uint32_t d = (static_cast<uint32_t>(v) + 50u) / 100u;
        o.digits(d / 10, 1); o.put('.'); o.digits(d % 10, 1);
    } else {                                          // ddd (and wider, never expected)
        o.digits(math::div_u64_to_u32(v + 500u, 1000u), 1);
    }
    if (unit) o.put(unit);
    return o.end();
}

// "19.1V" "x" "2.04A" "=" "39.0W": the status-bar formula; x is U+00D7 (in the 12 and 16 px fonts).
constexpr std::size_t formula(std::span<char> out, long long mv, long long ma, long long mw) {
    char a[12], b[12], c[12];
    fmt3(a, mv, 'V'); fmt3(b, ma, 'A'); fmt3(c, mw, 'W');
    detail::Out o{out};
    o.put(a); o.put("\xC3\x97"); o.put(b); o.put('='); o.put(c);
    return o.end();
}

// Rated power in whole watts ("100W", "45W"), for the negotiated row's power half.
constexpr std::size_t rated_watts(std::span<char> out, uint32_t mw) {
    detail::Out o{out};
    o.digits((mw + 500) / 1000, 1);
    o.put('W');
    return o.end();
}

enum class PowerKind : uint8_t { None, Pd, Legacy, Plain };

// Power half of the negotiated row. Pd: contract mv x ma -> "PD 100W". Legacy: detected fast-charge
// protocol `name` at the measured `mv` -> "QC3 9V". Plain: no PD and no protocol, the Rp current
// advertised on CC (ma, 0 = unknown) -> "5V 15W" / "5V". None: empty (shown as "?").
constexpr std::size_t power_half(std::span<char> out, PowerKind kind, uint32_t mv, uint32_t ma, std::string_view name) {
    detail::Out o{out};
    char w[8];
    switch (kind) {
    case PowerKind::Pd:
        rated_watts(w, math::mw_of(mv, ma));
        o.put("PD "); o.put(w);
        break;
    case PowerKind::Legacy:
        o.put(name); o.put(' '); o.digits((mv + 500) / 1000, 1); o.put('V');
        break;
    case PowerKind::Plain:
        o.digits((mv + 500) / 1000, 1); o.put('V');
        if (ma) { rated_watts(w, math::mw_of(mv, ma)); o.put(' '); o.put(w); }
        break;
    case PowerKind::None: break;
    }
    return o.end();
}

// "data / power"; an empty or "--" half is "?" so both halves always show.
constexpr std::size_t negotiated_row(std::span<char> out, std::string_view data, std::string_view power) {
    detail::Out o{out};
    o.put(data.empty() || data == "--" || data == "Unkn" ? std::string_view("?") : data);
    o.put(" / ");
    o.put(power.empty() ? std::string_view("?") : power);
    return o.end();
}

}  // namespace k2::readout
