// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The diagnosis text writer, shared by the full lines (diag.cpp: "20 V 3.25 A") and the K2 screen's compact form
// (diag_screen.cpp: "20V3.25A", units without a space as the screen's copy rules ask). Internal to the two.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::pd::diag::detail {

struct Out {
    std::span<char> b;
    std::size_t n = 0;
    bool tight = false;   // the screen form: no space before a unit, "12m05s" durations
    void c(char ch) { if (n + 1 < b.size()) { b[n++] = ch; b[n] = 0; } }
    void s(const char* t) { while (t && *t) c(*t++); }
    void cap(const char* t) {   // capitalised first letter
        if (!t || !*t) return;
        c(*t >= 'a' && *t <= 'z' ? char(*t - 32) : *t);
        s(t + 1);
    }
    void unit(const char* t) { if (!tight) c(' '); s(t); }
    void u(std::uint32_t v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) c(t[--k]);
    }
    // milli-units with up to `dec` decimals, trailing zeros dropped ("20", "3.25", "19.3")
    void milli(std::int32_t v, int dec) {
        if (v < 0) { c('-'); v = -v; }
        std::uint32_t scale = dec >= 2 ? 10 : dec == 1 ? 100 : 1000;
        const std::uint32_t q = (std::uint32_t(v) + scale / 2) / scale;   // in units of 10^-dec
        const std::uint32_t div = dec >= 2 ? 100 : dec == 1 ? 10 : 1;
        u(q / div);
        std::uint32_t f = q % div;
        if (f == 0 || div == 1) return;
        c('.');
        if (div == 100) { c(char('0' + f / 10)); if (f % 10) c(char('0' + f % 10)); }
        else c(char('0' + f));
    }
    void volts(std::int32_t mv) { milli(mv, 1); unit("V"); }
    void volts2(std::int32_t mv) { milli(mv, 2); unit("V"); }   // a band edge: 18.99 V, not 19 V
    void amps(std::int32_t ma) {
        if (ma < 100) { u(std::uint32_t(ma < 0 ? 0 : ma)); unit("mA"); return; }
        milli(ma, 2);
        unit("A");
    }
    void watts(std::int32_t mw) {
        if (mw < 10000) milli(mw, 1); else u(std::uint32_t(mw + 500) / 1000);
        unit("W");
    }
    void two(std::uint32_t v) { c(char('0' + v / 10 % 10)); c(char('0' + v % 10)); }
    void clock(std::uint32_t ms) {   // m:ss, or h:mm:ss
        const std::uint32_t t = ms / 1000;
        if (t >= 3600) { u(t / 3600); c(':'); two(t / 60 % 60); }
        else u(t / 60);
        c(':');
        two(t % 60);
    }
    void dur(std::uint32_t ms) {
        const std::uint32_t t = ms / 1000;
        if (tight) {   // "45s", "12m05s", "1h05m"
            if (t < 60) { u(t); c('s'); }
            else if (t < 3600) { u(t / 60); c('m'); two(t % 60); c('s'); }
            else { u(t / 3600); c('h'); two(t / 60 % 60); c('m'); }
            return;
        }
        if (t < 60) { u(t); s(" s"); return; }
        if (t < 3600) { u(t / 60); s(" min "); u(t % 60); s(" s"); return; }
        u(t / 3600); s(" h "); u(t / 60 % 60); s(" min");
    }
};

}  // namespace k2::pd::diag::detail
