// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The device twin's one-line texts, pure: the screen, the CDC shell and the host tool's table print exactly these.
//
//   summary   what the device would get, in the Home protocol-row style:
//             "27W PPS"  "36W 12V"  (the contract meets what the device wants)
//             "60W of 96W"          (it gets less than it wants)
//             "5V only"             (the device would sit on the 5 V level)
//             "no contract"         (the charger offers it nothing, or the list is unusable)
//   note      what limits it: "full", "limited by the cable", "limited by this charger", "the device takes no more"
//   proof     the live verdict: "27W PPS ok 9.0V", "refused", "out of band 7.4V"
//
// Every function writes into the caller's span, never beyond it, always terminated; the return value is the length.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/twin/live.hpp"
#include "k2/twin/predict.hpp"

namespace k2::twin::present {

namespace detail {

// Appends text and numbers into a span, cutting at the end (one byte stays for the terminator).
struct Out {
    std::span<char> b;
    std::size_t n = 0;
    explicit Out(std::span<char> s) : b(s) { if (!b.empty()) b[0] = '\0'; }
    void ch(char c) {
        if (n + 1 < b.size()) { b[n++] = c; b[n] = '\0'; }
    }
    void str(const char* s) { while (*s) ch(*s++); }
    void num(std::uint32_t v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v && k < 10);
        while (k) ch(t[--k]);
    }
    // 9012 mV -> "9.0V" (one decimal, rounded).
    void volts(std::uint32_t mv) {
        const std::uint32_t d = (mv + 50) / 100;
        num(d / 10);
        ch('.');
        num(d % 10);
        ch('V');
    }
    int len() const { return int(n); }
};

constexpr std::uint32_t whole_w(std::uint32_t mw) { return (mw + 500) / 1000; }

}  // namespace detail

inline int summary(const Outcome& o, std::span<char> out) {
    detail::Out w(out);
    if (o.kind == Kind::None) {
        w.str("no contract");
    } else if (o.kind == Kind::FiveV) {
        w.str("5V only");
    } else {
        w.num(detail::whole_w(o.mw));
        w.str("W");
        // "of NW" when the device wants visibly more than it gets; a rounding difference is not a shortfall.
        if (detail::whole_w(o.want_mw) > detail::whole_w(o.mw)) {
            w.str(" of ");
            w.num(detail::whole_w(o.want_mw));
            w.str("W");
        } else if (o.kind == Kind::Pps) {
            w.str(" PPS");
        } else {
            w.ch(' ');
            w.num((o.mv + 500u) / 1000u);
            w.ch('V');
        }
    }
    return w.len();
}

constexpr const char* note(Limiter l) {
    switch (l) {
        case Limiter::None: return "full";
        case Limiter::Device: return "the device takes no more";
        case Limiter::Charger: return "limited by this charger";
        case Limiter::Cable: return "limited by the cable";
    }
    return "?";
}

// The device would get all it needs: a contract exists, no capability mismatch, and nothing the charger or the cable holds
// back (a device that takes no more than it is given counts as full).
constexpr bool is_full(const Outcome& o) {
    return o.kind != Kind::None && !o.mismatch && (o.limiter == Limiter::None || o.limiter == Limiter::Device);
}

// The Device twins page footer (at most 27 cells): the one fact about this charger and the stored devices.
//   "No twin yet. Learn one"      the library is empty
//   "No charger: no predictions"  no Source_Capabilities to predict from
//   "3 of 5 full on charger"      `full` of `twins` devices would get all they need
inline int footer(unsigned twins, unsigned full, bool have_caps, std::span<char> out) {
    detail::Out w(out);
    if (twins == 0) {
        w.str("No twin yet. Learn one");
    } else if (!have_caps) {
        w.str("No charger: no predictions");
    } else {
        w.num(full);
        w.str(" of ");
        w.num(twins);
        w.str(" full on charger");
    }
    return w.len();
}

inline int proof(live::Verdict v, std::uint16_t measured_mv, const Outcome& o, std::span<char> out) {
    detail::Out w(out);
    if (v == live::Verdict::Granted) {
        char s[16];
        const int n = summary(o, s);
        (void)n;
        w.str(s);
        w.str(" ok ");
        w.volts(measured_mv);
        return w.len();
    }
    w.str(live::name(v));
    if (v == live::Verdict::OutOfBand && measured_mv) {
        w.ch(' ');
        w.volts(measured_mv);
    }
    return w.len();
}

}  // namespace k2::twin::present
