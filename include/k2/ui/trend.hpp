// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// History behind the three Home strips (V, A, W), pure and host-tested (tests/test_trend.cpp).
//
// Three tiers share one push(): 100 ms samples for the 10 s window, 1 s samples for the 60 s window
// and 10 s averages for the 10 min window. Values are stored in 10 mV / 10 mA / 10 mW units (uint16,
// so 655 V / 655 A / 655 W). Each strip is auto-ranged to its own min/max by range().
#pragma once
#include <cstdint>
#include <memory>

namespace k2::trend {

enum class Window : uint8_t { S10, S60, M10 };
enum class Qty : uint8_t { V, A, W };
inline constexpr uint16_t kNone = 0xFFFF;   // column with no sample yet

constexpr Window next(Window w) { return w == Window::S10 ? Window::S60 : w == Window::S60 ? Window::M10 : Window::S10; }
constexpr const char *label(Window w) { return w == Window::S10 ? "10s" : w == Window::S60 ? "60s" : "10m"; }

struct Range {
    uint16_t dmin = 0, dmax = 0;   // data min / max (the labels)
    uint16_t lo = 0, hi = 0;       // plotted span, at least the quantity's minimum span
    bool valid = false;
};

// Smallest plotted span: 1 V, 0.2 A, 1 W (10 units), so noise on a flat signal stays flat.
constexpr uint16_t min_span(Qty q) { return q == Qty::A ? 20 : 100; }

constexpr Range range(const uint16_t *c, int n, Qty q) {
    Range r;
    for (int i = 0; i < n; i++) {
        if (c[i] == kNone) continue;
        if (!r.valid) { r.dmin = r.dmax = c[i]; r.valid = true; }
        if (c[i] < r.dmin) r.dmin = c[i];
        if (c[i] > r.dmax) r.dmax = c[i];
    }
    if (!r.valid) return r;
    r.lo = r.dmin;
    r.hi = r.dmax;
    const int span = r.hi - r.lo, floor = min_span(q);
    if (span < floor) {
        int lo = r.lo - (floor - span) / 2;
        if (lo < 0) lo = 0;
        r.lo = static_cast<uint16_t>(lo);
        r.hi = static_cast<uint16_t>(lo + floor);
    }
    return r;
}

// Pixel level 0..h-1 of v inside r; no sample = 0.
constexpr int level(uint16_t v, const Range &r, int h) {
    if (v == kNone || !r.valid || r.hi <= r.lo || h < 2) return 0;
    const int d = (v < r.lo ? r.lo : v > r.hi ? r.hi : v) - r.lo;
    return d * (h - 1) / (r.hi - r.lo);
}

class Trend {
public:
    static constexpr int kFast = 100, kMid = 60, kSlow = 60;
    void clear() { std::construct_at(this); }   // in place: `*this = Trend{}` builds a ~1.4 KB temporary on the stack
    // Time-ordered; v10 / a10 / w10 in 10 mV / 10 mA / 10 mW.
    void push(uint32_t now_ms, uint32_t v10, uint32_t a10, uint32_t w10) {
        const uint16_t s[3] = {clamp(v10), clamp(a10), clamp(w10)};
        if (!started_) { started_ = true; mid_ms_ = now_ms; slow_ms_ = now_ms; fast_ms_ = now_ms - kFastMs; }
        if (now_ms - fast_ms_ >= kFastMs) { fast_ms_ = now_ms; put(fast_, kFast, nf_, hf_, s); }
        if (now_ms - mid_ms_ >= kMidMs) { mid_ms_ += kMidMs * ((now_ms - mid_ms_) / kMidMs); put(mid_, kMid, nm_, hm_, s); }
        for (int q = 0; q < 3; q++) acc_[q] += s[q];
        acc_n_++;
        if (now_ms - slow_ms_ >= kSlowMs) {
            slow_ms_ += kSlowMs * ((now_ms - slow_ms_) / kSlowMs);
            uint16_t avg[3];
            for (int q = 0; q < 3; q++) avg[q] = static_cast<uint16_t>(acc_[q] / acc_n_);
            put(slow_, kSlow, ns_, hs_, avg);
            acc_[0] = acc_[1] = acc_[2] = 0;
            acc_n_ = 0;
        }
    }
    // `cols` columns, oldest first, newest at the right; kNone where the window holds no sample.
    // Returns the number of columns that hold data.
    int columns(Window w, Qty q, uint16_t *out, int cols) const {
        const uint16_t (*ring)[3] = w == Window::S10 ? fast_ : w == Window::S60 ? mid_ : slow_;
        const int size = w == Window::S10 ? kFast : w == Window::S60 ? kMid : kSlow;
        const int n = w == Window::S10 ? nf_ : w == Window::S60 ? nm_ : ns_;
        const int head = w == Window::S10 ? hf_ : w == Window::S60 ? hm_ : hs_;
        int have = 0;
        for (int c = 0; c < cols; c++) {
            const int back = (cols - 1 - c) * size / cols;   // 0 = newest sample
            if (back >= n) { out[c] = kNone; continue; }
            out[c] = ring[(head - 1 - back + 2 * size) % size][static_cast<int>(q)];
            have++;
        }
        return have;
    }

    // Newest sample of the window's ring; the faster rings stand in while it is still empty (the
    // 10 min tier gets its first sample after 10 s). kNone when nothing was pushed at all.
    uint16_t latest(Window w, Qty q) const {
        const int i = static_cast<int>(q);
        if (w == Window::M10 && ns_ > 0) return slow_[(hs_ + kSlow - 1) % kSlow][i];
        if (w != Window::S10 && nm_ > 0) return mid_[(hm_ + kMid - 1) % kMid][i];
        return nf_ > 0 ? fast_[(hf_ + kFast - 1) % kFast][i] : kNone;
    }

private:
    static constexpr uint32_t kFastMs = 100, kMidMs = 1000, kSlowMs = 10000;
    static constexpr uint16_t clamp(uint32_t v) { return v > 0xFFFE ? 0xFFFE : static_cast<uint16_t>(v); }
    static void put(uint16_t (*ring)[3], int size, int &n, int &head, const uint16_t *s) {
        for (int q = 0; q < 3; q++) ring[head][q] = s[q];
        head = (head + 1) % size;
        if (n < size) n++;
    }
    uint16_t fast_[kFast][3] = {}, mid_[kMid][3] = {}, slow_[kSlow][3] = {};
    int nf_ = 0, nm_ = 0, ns_ = 0, hf_ = 0, hm_ = 0, hs_ = 0;
    uint32_t fast_ms_ = 0, mid_ms_ = 0, slow_ms_ = 0, acc_[3] = {}, acc_n_ = 0;
    bool started_ = false;
};

}  // namespace k2::trend
