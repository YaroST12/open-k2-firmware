// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure helpers for the UI: energy integration and number formatting. No hardware access.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/core/math_opt.hpp"

namespace k2 {

// Integrates forward power over time (trapezoid rule) in microjoules. Negative (reverse) power
// counts as 0: energy only accumulates, it is never subtracted.
// mW * ms = uJ, so no scaling is needed. The first update after reset() only sets the baseline.
class EnergyMeter {
public:
    void reset(uint32_t now_ms);
    void update(uint32_t now_ms, int32_t power_mw);
    // Also integrates the forward current (mA * ms = uC, 1 mAh = 3.6e6 uC); reverse counts as 0.
    void update(uint32_t now_ms, int32_t power_mw, int32_t current_ma);
    int64_t microcoulombs() const { return uc_; }
    int32_t mah() const { return math::div_i64_to_i32(uc_, 3'600'000); }
    int64_t microjoules() const { return uj_; }
    int32_t mwh() const { return math::div_i64_to_i32(uj_, 3'600'000); }  // 1 mWh = 3.6e6 uJ
    uint32_t elapsed_s(uint32_t now_ms) const { return (now_ms - start_ms_) / 1000u; }

private:
    int64_t uj_ = 0;
    int64_t uc_ = 0;
    uint32_t start_ms_ = 0;
    uint32_t last_ms_ = 0;
    int32_t last_mw_ = 0;
    int32_t last_ma_ = 0;
    bool primed_ = false;
};

// Reboot HoldCard arithmetic: the OK + Back chord held for held_ms of total_ms. The card shows from kHoldCardMs.
constexpr uint32_t kHoldCardMs = 1000;
constexpr int hold_secs_left(uint32_t held_ms, uint32_t total_ms) {
    return held_ms >= total_ms ? 0 : static_cast<int>((total_ms - held_ms + 999) / 1000);
}
constexpr int hold_fill_px(uint32_t held_ms, uint32_t total_ms, int track_px) {
    return held_ms >= total_ms ? track_px : static_cast<int>(math::mul_div_u32(held_ms, static_cast<uint32_t>(track_px), total_ms));
}

// The reboot gesture: OK and Back held together for kChordRebootMs, on every page and menu. The HoldCard shows from
// kHoldCardMs; releasing either key cancels. Once both keys are down the chord is active until BOTH are up again, and
// the Ui drops the single-key OK / Back events meanwhile (like the Minus + Plus rotation chord), so neither key's long
// action fires on the way into or out of it.
constexpr uint32_t kChordRebootMs = 3000;
class RebootChord {
public:
    enum class Step : uint8_t { None, Card, Reboot };
    void keys(bool ok, bool back, uint32_t now_ms) {
        if (ok && back) {
            if (!both_) { both_ = true; active_ = true; fired_ = false; since_ms_ = now_ms; }
        } else {
            both_ = false;
            if (!ok && !back) active_ = false;
        }
    }
    bool active() const { return active_; }
    uint32_t held_ms(uint32_t now_ms) const { return both_ ? now_ms - since_ms_ : 0; }
    // Card while the chord has been held kHoldCardMs or more; Reboot once, at kChordRebootMs.
    Step step(uint32_t now_ms) {
        const uint32_t h = held_ms(now_ms);
        if (!both_ || h < kHoldCardMs) return Step::None;
        if (h >= kChordRebootMs && !fired_) { fired_ = true; return Step::Reboot; }
        return Step::Card;
    }
private:
    uint32_t since_ms_ = 0;
    bool both_ = false, active_ = false, fired_ = false;
};

// Home flow rail: one chevron of `count` is bright, and it steps every kChevStepMs toward the sink.
// Up (power flows bottom to top): index 0 is the top chevron, so the bright one walks 'count-1 .. 0'.
constexpr uint32_t kChevStepMs = 250;
constexpr int chevron_bright(uint32_t now_ms, int count, bool up) {
    const int step = static_cast<int>((now_ms / kChevStepMs) % static_cast<uint32_t>(count));
    return up ? count - 1 - step : step;
}

// Display filter: lets a value change reach the screen at most once per period (the first one passes).
class Throttle {
public:
    explicit constexpr Throttle(uint32_t period_ms = 250) : period_(period_ms) {}
    bool pass(uint32_t now_ms) {
        if (primed_ && now_ms - last_ < period_) return false;
        primed_ = true;
        last_ = now_ms;
        return true;
    }
    void reset() { primed_ = false; }

private:
    uint32_t period_;
    uint32_t last_ = 0;
    bool primed_ = false;
};

// The last kLen seconds of forward power for the Home sparkline: one sample per second, newest on
// the right. scaled() maps them to levels 0..h-1 with the trace's own min..max, but never zooms
// in on less than 1 W of range (so noise on a flat load stays flat).
class WattHistory {
public:
    static constexpr int kLen = 58;   // drawn 2 px wide each = the 116 px sparkline
    void clear() { n_ = 0; head_ = 0; have_ = false; }
    void push(uint32_t now_ms, int32_t power_mw);
    int count() const { return n_; }
    void scaled(uint8_t *out, int h) const;   // kLen levels, oldest first; empty slots are 0

private:
    int32_t mw_[kLen] = {};
    int n_ = 0, head_ = 0;     // head_ = index of the next write
    uint32_t last_ms_ = 0;
    bool have_ = false;
};

// All formatters write a NUL-terminated string (truncated to fit) and return the length written.
// Fixed point from thousandths: decimals 0..3, right-aligned in `width` (0 = no padding);
// a value that does not fit the width is shown as dashes.
int fmt_fixed(std::span<char> out, int32_t milli, int decimals, int width);
int fmt_abs_fixed(std::span<char> out, int32_t milli, int decimals, int width);  // |value|, never '-'
// |value| with up to `max_decimals`, fewer when the digits would not fit `width` (right-aligned).
int fmt_fit(std::span<char> out, int32_t milli, int width, int max_decimals);
// Home V / A / W: one decimal count for all three so the right-aligned numbers line up on the dot.
// The field holds 5 characters: 2 decimals, 1 from 100 W (hysteresis: back to 2 below 98 W).
int home_decimals(int32_t power_mw, int prev_decimals);
int fmt_home(std::span<char> out, int32_t milli, int decimals, int width);   // |value|, right-aligned
// Wh and Ah as two right-aligned fields of `width` with the SAME decimals (the most both fit), so
// the decimal points line up when the two lines are stacked. Returns the decimals used (0..3).
int fmt_energy_pair(std::span<char> wh, std::span<char> ah, int32_t mwh, int32_t mah, int width);
// Load resistance V / I as a 4 character field: "6.69", "20.0", " 250"; "  --" under 50 mA.
int fmt_ohms(std::span<char> out, int32_t vbus_mv, int32_t current_ma);

// Age of the frozen data as "m:ss", capped at "9:59+" (UI design 6.6). Returns the length.
int fmt_age(std::span<char> out, uint32_t secs);

// Remembers when VBUS was last present, for the stale (LAST) state of Home and the status bar.
class LiveTracker {
public:
    void note(uint32_t now_ms, bool live) {
        if (!live) return;
        seen_ = true;
        last_ms_ = now_ms;
    }
    void clear() { seen_ = false; }                       // OK on Home / replug accepted
    bool seen() const { return seen_; }
    uint32_t age_s(uint32_t now_ms) const { return seen_ ? (now_ms - last_ms_) / 1000u : 0u; }

private:
    bool seen_ = false;
    uint32_t last_ms_ = 0;
};
int fmt_u32(std::span<char> out, uint32_t value, int width);
// Counter in at most 5 chars: < 10000 exact, < 10 000 000 floor thousands + 'K', else floor millions + 'M'.
int fmt_u32_compact(std::span<char> out, uint32_t value);
// Same, capped: above 999 999 999 the count prints ">999M" (a wrapped or garbage counter is never shown
// as a live-looking number). count_overflowed() tells the caller to draw it in a neutral colour.
constexpr uint32_t kCountMax = 999999999u;
constexpr bool count_overflowed(uint32_t value) { return value > kCountMax; }
int fmt_count_capped(std::span<char> out, uint32_t value);
int fmt_hms(std::span<char> out, uint32_t seconds);         // H:MM:SS
int fmt_hex(std::span<char> out, uint32_t value, int digits);  // upper case, zero padded

// StrBuf's out-of-line body, shared by every capacity N: StrBuf<N> forwards (buffer, N, length)
// here, so flash holds one copy of each formatter wrapper instead of one per N.
struct StrBufRef {
    char *b;
    size_t cap;
    size_t &n;
    void put(std::string_view s);  // appends what fits into b[0..cap-2], keeps b NUL-terminated
    void put_u32(uint32_t v, int width);
    void put_u32_compact(uint32_t v);
    void put_count_capped(uint32_t v);
    void put_fixed(int32_t milli, int decimals, int width);
    void put_abs_fixed(int32_t milli, int decimals, int width);
    void put_hex(uint32_t v, int digits);
    void put_hms(uint32_t seconds);
    void put_age(uint32_t seconds);
};

// Fixed-capacity string builder (no heap). Output is always NUL-terminated and truncated to fit.
template <size_t N>
class StrBuf {
public:
    StrBuf() { b_[0] = '\0'; }
    void clear() { n_ = 0; b_[0] = '\0'; }
    StrBuf &put(std::string_view s) {
        ref().put(s);
        return *this;
    }
    StrBuf &put_char(char c) {
        if (n_ + 1 < N) { b_[n_++] = c; b_[n_] = '\0'; }
        return *this;
    }
    StrBuf &put_u32(uint32_t v, int width = 0) {
        ref().put_u32(v, width);
        return *this;
    }
    StrBuf &put_u32_compact(uint32_t v) {
        ref().put_u32_compact(v);
        return *this;
    }
    StrBuf &put_count_capped(uint32_t v) {
        ref().put_count_capped(v);
        return *this;
    }
    StrBuf &put_fixed(int32_t milli, int decimals, int width = 0) {
        ref().put_fixed(milli, decimals, width);
        return *this;
    }
    StrBuf &put_abs_fixed(int32_t milli, int decimals, int width = 0) {
        ref().put_abs_fixed(milli, decimals, width);
        return *this;
    }
    StrBuf &put_hex(uint32_t v, int digits) {
        ref().put_hex(v, digits);
        return *this;
    }
    StrBuf &put_hms(uint32_t seconds) {
        ref().put_hms(seconds);
        return *this;
    }
    StrBuf &put_age(uint32_t seconds) {
        ref().put_age(seconds);
        return *this;
    }
    std::string_view view() const { return {b_, n_}; }
    const char *c_str() const { return b_; }

private:
    StrBufRef ref() { return {b_, N, n_}; }
    char b_[N];
    size_t n_ = 0;
};

}  // namespace k2
