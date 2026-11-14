// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// History behind Home's strips and the Graphs page (V, A, W), pure and host-tested (tests/test_trend.cpp).
//
// Three tiers share one push(), one per window (10 s, 60 s, 10 min). Each holds kPoints = 110 columns, one per Graphs
// column (2 px each across 220 px), taken every 91 ms, 545 ms and 5455 ms (the window / 110, home::step_ms): the fast and
// middle tiers keep the sample of that moment, the slow tier the average of its bucket. Values are stored in
// 10 mV / 10 mA / 10 mW units (uint16, so 655 V / 655 A / 655 W). Home resamples a window onto its own strip widths with
// columns(); each strip is auto-ranged to its own min/max by range().
//
// Beside the data every tier keeps, per column, the contract's maximum power in whole watts (the Graphs page's dotted
// limit line: it steps on a renegotiation and follows a PPS level), and every 10th column one point of the session's
// energy (the Wh graph: ten windows in the same 110 points). Energy is stored as 16 bits that wrap, in a unit per tier
// (1, 10, 100 mWh), and read back as differences from the oldest point shown: no session length overflows it, and a
// window never holds more than 6.5 kWh of difference.
#pragma once
#include <cstdint>
#include <memory>

namespace k2::trend {

enum class Window : uint8_t { S10, S60, M10 };
enum class Qty : uint8_t { V, A, W };
inline constexpr uint16_t kNone = 0xFFFF;   // column with no sample yet

constexpr Window next(Window w) { return w == Window::S10 ? Window::S60 : w == Window::S60 ? Window::M10 : Window::S10; }
constexpr Window prev(Window w) { return w == Window::S10 ? Window::M10 : w == Window::S60 ? Window::S10 : Window::S60; }
static_assert(prev(next(Window::S10)) == Window::S10 && prev(next(Window::M10)) == Window::M10);
constexpr const char *label(Window w) { return w == Window::S10 ? "10s" : w == Window::S60 ? "60s" : "10m"; }
// Milliseconds between two columns of a tier: the window over 110 columns.
constexpr uint32_t period_ms(Window w) { return w == Window::S10 ? 91u : w == Window::S60 ? 545u : 5455u; }
// The window in seconds (10, 60, 600).
constexpr uint32_t window_s(Window w) { return w == Window::S10 ? 10u : w == Window::S60 ? 60u : 600u; }

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
    static constexpr int kPoints = 110;          // columns per tier
    static constexpr int kEnergyEvery = 10;      // a Wh point every 10th column of a tier
    static constexpr int kFast = kPoints, kMid = kPoints, kSlow = kPoints;
    void clear() { std::construct_at(this); }   // in place: `*this = Trend{}` builds a ~3 KB temporary on the stack
    // Time-ordered; v10 / a10 / w10 in 10 mV / 10 mA / 10 mW, limit_w the contract's maximum in whole watts (0 = none),
    // mwh the session's energy so far (EnergyMeter::mwh()).
    void push(uint32_t now_ms, uint32_t v10, uint32_t a10, uint32_t w10, uint32_t limit_w = 0, uint32_t mwh = 0) {
        const uint16_t s[3] = {clamp(v10), clamp(a10), clamp(w10)};
        const uint8_t lim = static_cast<uint8_t>(limit_w > 255 ? 255 : limit_w);
        if (!started_) {
            started_ = true;
            t_[0].due_ms = now_ms - period_ms(Window::S10);   // the fast tier takes the first sample at once
            t_[1].due_ms = now_ms;
            t_[2].due_ms = now_ms;
        }
        for (int q = 0; q < 3; q++) acc_[q] += s[q];
        acc_n_++;
        for (int w = 0; w < 3; w++) {
            Tier &t = t_[w];
            const uint32_t p = period_ms(static_cast<Window>(w));
            if (now_ms - t.due_ms < p) continue;
            t.due_ms += p * ((now_ms - t.due_ms) / p);   // a gap (the trace froze while stale) is skipped, not filled
            if (w == 2) {
                uint16_t avg[3];
                for (int q = 0; q < 3; q++) avg[q] = static_cast<uint16_t>(acc_[q] / acc_n_);
                put(t, avg, lim, mwh / energy_unit_mwh(Window::M10));
                acc_[0] = acc_[1] = acc_[2] = 0;
                acc_n_ = 0;
            } else {
                put(t, s, lim, mwh / energy_unit_mwh(static_cast<Window>(w)));
            }
        }
    }
    // `cols` columns, oldest first, newest at the right; kNone where the window holds no sample.
    // Returns the number of columns that hold data.
    int columns(Window w, Qty q, uint16_t *out, int cols) const {
        const Tier &t = t_[static_cast<int>(w)];
        int have = 0;
        for (int c = 0; c < cols; c++) {
            const int back = (cols - 1 - c) * kPoints / cols;   // 0 = newest sample
            if (back >= t.n) { out[c] = kNone; continue; }
            out[c] = t.s[at(t.head, back)][static_cast<int>(q)];
            have++;
        }
        return have;
    }
    // The contract maximum (W) of the same columns; 0 where columns() has no sample. Returns the columns with a sample.
    int limits(Window w, uint8_t *out, int cols) const {
        const Tier &t = t_[static_cast<int>(w)];
        int have = 0;
        for (int c = 0; c < cols; c++) {
            const int back = (cols - 1 - c) * kPoints / cols;
            if (back >= t.n) { out[c] = 0; continue; }
            out[c] = t.lim[at(t.head, back)];
            have++;
        }
        return have;
    }
    // The tier's Wh points, oldest first, newest at the right, each the energy since the oldest point shown, in
    // energy_unit_mwh(w); kNone where there is no point yet. Returns the number of points.
    int energy(Window w, uint16_t *out, int cols) const {
        const Tier &t = t_[static_cast<int>(w)];
        const int first = cols - 1 < t.en - 1 ? cols - 1 : t.en - 1;   // `back` of the oldest point shown
        const uint16_t ref = t.en ? t.e[at(t.ehead, first)] : 0;
        int have = 0;
        for (int c = 0; c < cols; c++) {
            const int back = cols - 1 - c;
            if (back >= t.en || back >= kPoints) { out[c] = kNone; continue; }
            const uint16_t d = static_cast<uint16_t>(t.e[at(t.ehead, back)] - ref);
            out[c] = d == kNone ? static_cast<uint16_t>(kNone - 1) : d;
            have++;
        }
        return have;
    }
    static constexpr uint32_t energy_unit_mwh(Window w) { return w == Window::S10 ? 1u : w == Window::S60 ? 10u : 100u; }
    // Columns the tier has taken since clear(): the Graphs page scrolls when it moves.
    uint32_t seq(Window w) const { return t_[static_cast<int>(w)].seq; }

    // Newest sample of the window's ring; the faster rings stand in while it is still empty (the
    // 10 min tier gets its first sample after 5.5 s). kNone when nothing was pushed at all.
    uint16_t latest(Window w, Qty q) const {
        const int i = static_cast<int>(q);
        const Tier &f = t_[0], &m = t_[1], &s = t_[2];
        if (w == Window::M10 && s.n > 0) return s.s[at(s.head, 0)][i];
        if (w != Window::S10 && m.n > 0) return m.s[at(m.head, 0)][i];
        return f.n > 0 ? f.s[at(f.head, 0)][i] : kNone;
    }

private:
    struct Tier {
        uint16_t s[kPoints][3] = {};   // V, A, W per column
        uint8_t lim[kPoints] = {};     // contract maximum, W
        uint16_t e[kPoints] = {};      // energy points (wrapping, energy_unit_mwh)
        uint8_t n = 0, head = 0;       // columns held, next column slot
        uint8_t en = 0, ehead = 0;     // energy points held, next point slot
        uint32_t due_ms = 0;           // the time the last column was due
        uint32_t seq = 0;              // columns taken
    };
    static constexpr uint16_t clamp(uint32_t v) { return v > 0xFFFE ? 0xFFFE : static_cast<uint16_t>(v); }
    static constexpr int at(int head, int back) { return (head - 1 - back + 2 * kPoints) % kPoints; }
    static void put(Tier &t, const uint16_t *s, uint8_t lim, uint32_t energy) {
        for (int q = 0; q < 3; q++) t.s[t.head][q] = s[q];
        t.lim[t.head] = lim;
        t.head = static_cast<uint8_t>((t.head + 1) % kPoints);
        if (t.n < kPoints) t.n++;
        if (t.seq % kEnergyEvery == 0) {
            t.e[t.ehead] = static_cast<uint16_t>(energy);
            t.ehead = static_cast<uint8_t>((t.ehead + 1) % kPoints);
            if (t.en < kPoints) t.en++;
        }
        t.seq++;
    }
    Tier t_[3];
    uint32_t acc_[3] = {}, acc_n_ = 0;
    bool started_ = false;
};

}  // namespace k2::trend
