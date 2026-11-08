// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_calc.hpp"
#include "k2/drivers/power_calc.hpp"

namespace k2 {

void EnergyMeter::reset(uint32_t now_ms) {
    uj_ = 0;
    uc_ = 0;
    start_ms_ = now_ms;
    last_ms_ = now_ms;
    last_mw_ = 0;
    last_ma_ = 0;
    primed_ = false;
}

void EnergyMeter::update(uint32_t now_ms, int32_t power_mw) { update(now_ms, power_mw, 0); }

void EnergyMeter::update(uint32_t now_ms, int32_t power_mw, int32_t current_ma) {
    if (power_mw < 0) power_mw = 0;  // forward energy only
    if (current_ma < 0) current_ma = 0;
    if (primed_) {
        uint32_t dt = now_ms - last_ms_;  // wrap-safe
        uj_ += (static_cast<int64_t>(last_mw_) + power_mw) * dt / 2;
        uc_ += (static_cast<int64_t>(last_ma_) + current_ma) * dt / 2;
    }
    primed_ = true;
    last_ms_ = now_ms;
    last_mw_ = power_mw;
    last_ma_ = current_ma;
}

void WattHistory::push(uint32_t now_ms, int32_t power_mw) {
    if (have_ && now_ms - last_ms_ < 1000) return;
    have_ = true;
    last_ms_ = now_ms;
    mw_[head_] = power_mw < 0 ? 0 : power_mw;
    head_ = (head_ + 1) % kLen;
    if (n_ < kLen) n_++;
}

void WattHistory::scaled(uint8_t *out, int h) const {
    for (int i = 0; i < kLen; i++) out[i] = 0;
    if (n_ == 0 || h < 2) return;
    int32_t lo = INT32_MAX, hi = 0;
    for (int i = 0; i < n_; i++) {
        const int32_t v = mw_[(head_ - 1 - i + 2 * kLen) % kLen];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    int32_t range = hi - lo;
    if (range < 1000) { lo = lo - (1000 - range) / 2; range = 1000; if (lo < 0) lo = 0; }
    for (int i = 0; i < n_; i++) {   // i = 0 is the oldest kept sample
        const int32_t v = mw_[(head_ - n_ + i + 2 * kLen) % kLen];
        int64_t lv = static_cast<int64_t>(v - lo) * (h - 1) / range;
        if (lv < 0) lv = 0;
        if (lv > h - 1) lv = h - 1;
        out[kLen - n_ + i] = static_cast<uint8_t>(lv);
    }
}

namespace {

// Copy a built string into out, truncating, always terminating. Returns the length.
int emit(std::span<char> out, const char *s, int n) {
    if (out.empty()) return 0;
    int room = static_cast<int>(out.size()) - 1;
    if (n > room) n = room;
    for (int i = 0; i < n; i++) out[i] = s[i];
    out[n] = '\0';
    return n;
}

// Right-align tmp[0..n) in width; overflow becomes dashes.
int emit_aligned(std::span<char> out, const char *tmp, int n, int width) {
    char buf[24];
    int len = 0;
    if (width > 0 && n > width) {
        for (; len < width && len < 23; len++) buf[len] = '-';
    } else {
        for (; len < width - n && len < 23; len++) buf[len] = ' ';
        for (int i = 0; i < n && len < 23; i++) buf[len++] = tmp[i];
    }
    return emit(out, buf, len);
}

}  // namespace

int fmt_fixed(std::span<char> out, int32_t milli, int decimals, int width) {
    if (decimals < 0) decimals = 0;
    if (decimals > 3) decimals = 3;
    bool neg = milli < 0;
    uint32_t mag = neg ? 0u - static_cast<uint32_t>(milli) : static_cast<uint32_t>(milli);
    uint32_t scale = 1;
    for (int i = decimals; i < 3; i++) scale *= 10;
    if (scale > 1) mag = (mag + scale / 2) / scale;     // half away from zero on the magnitude
    if (mag == 0) neg = false;

    char digits[12];
    int nd = 0;
    uint32_t v = mag;
    do { digits[nd++] = static_cast<char>('0' + v % 10); v /= 10; } while (v != 0 || nd <= decimals);

    char tmp[16];
    int n = 0;
    if (neg) tmp[n++] = '-';
    for (int i = nd - 1; i >= 0; i--) {
        tmp[n++] = digits[i];
        if (i == decimals && decimals > 0) tmp[n++] = '.';
    }
    return emit_aligned(out, tmp, n, width);
}

int fmt_abs_fixed(std::span<char> out, int32_t milli, int decimals, int width) {
    const int32_t mag = k2::power::abs_sat(milli);
    return fmt_fixed(out, mag, decimals, width);
}

int fmt_fit(std::span<char> out, int32_t milli, int width, int max_decimals) {
    char t[16];
    for (int d = max_decimals; d > 0; d--)
        if (fmt_abs_fixed(t, milli, d, 0) <= width) return fmt_abs_fixed(out, milli, d, width);
    return fmt_abs_fixed(out, milli, 0, width);
}

int fmt_energy_pair(std::span<char> wh, std::span<char> ah, int32_t mwh, int32_t mah, int width) {
    char t[16];
    int d = 3;
    while (d > 0 && (fmt_abs_fixed(t, mwh, d, 0) > width || fmt_abs_fixed(t, mah, d, 0) > width)) d--;
    fmt_abs_fixed(wh, mwh, d, width);
    fmt_abs_fixed(ah, mah, d, width);
    return d;
}

int home_decimals(int32_t power_mw, int prev_decimals) {
    const int32_t w = k2::power::abs_sat(power_mw);
    if (prev_decimals == 1) return w < 98000 ? 2 : 1;
    return w >= 99990 ? 1 : 2;
}

int fmt_home(std::span<char> out, int32_t milli, int decimals, int width) {
    return fmt_abs_fixed(out, milli, decimals, width);
}

int fmt_age(std::span<char> out, uint32_t secs) {
    char t[8];
    int n = 0;
    if (secs >= 600) {
        for (char ch : {'9', ':', '5', '9', '+'}) t[n++] = ch;
    } else {
        t[n++] = static_cast<char>('0' + secs / 60);
        t[n++] = ':';
        t[n++] = static_cast<char>('0' + (secs % 60) / 10);
        t[n++] = static_cast<char>('0' + secs % 10);
    }
    if (static_cast<size_t>(n) > out.size()) n = static_cast<int>(out.size());
    for (int i = 0; i < n; i++) out[i] = t[i];
    return n;
}

int fmt_ohms(std::span<char> out, int32_t vbus_mv, int32_t current_ma) {
    const int32_t i = k2::power::abs_sat(current_ma);
    if (i < 50) return emit(out, "  --", 4);
    const int64_t milli_ohm = static_cast<int64_t>(vbus_mv) * 1000 / i;   // mV / mA = ohm, x1000
    if (milli_ohm > 9'999'999) return emit(out, "  --", 4);
    return fmt_fit(out, static_cast<int32_t>(milli_ohm), 4, 2);
}

void StrBufRef::put(std::string_view s) {
    for (char c : s) {
        if (n + 1 >= cap) break;
        b[n++] = c;
    }
    b[n] = '\0';
}
void StrBufRef::put_u32(uint32_t v, int width) {
    char t[16];
    fmt_u32(t, v, width);
    put(t);
}
void StrBufRef::put_u32_compact(uint32_t v) {
    char t[8];
    const int len = fmt_u32_compact(t, v);
    put({t, static_cast<size_t>(len)});
}
void StrBufRef::put_count_capped(uint32_t v) {
    char t[8];
    const int len = fmt_count_capped(t, v);
    put({t, static_cast<size_t>(len)});
}
void StrBufRef::put_fixed(int32_t milli, int decimals, int width) {
    char t[16];
    fmt_fixed(t, milli, decimals, width);
    put(t);
}
void StrBufRef::put_abs_fixed(int32_t milli, int decimals, int width) {
    char t[16];
    fmt_abs_fixed(t, milli, decimals, width);
    put(t);
}
void StrBufRef::put_hex(uint32_t v, int digits) {
    char t[16];
    fmt_hex(t, v, digits);
    put(t);
}
void StrBufRef::put_hms(uint32_t seconds) {
    char t[16];
    fmt_hms(t, seconds);
    put(t);
}
void StrBufRef::put_age(uint32_t seconds) {
    char t[8];
    const int len = fmt_age(t, seconds);
    put({t, static_cast<size_t>(len)});
}

int fmt_u32(std::span<char> out, uint32_t value, int width) {
    char digits[12];
    int nd = 0;
    do { digits[nd++] = static_cast<char>('0' + value % 10); value /= 10; } while (value != 0);
    char tmp[12];
    for (int i = 0; i < nd; i++) tmp[i] = digits[nd - 1 - i];
    return emit_aligned(out, tmp, nd, width);
}

int fmt_u32_compact(std::span<char> out, uint32_t value) {
    char t[8];
    int n = fmt_u32(t, value < 10000u ? value : value < 10000000u ? value / 1000u : value / 1000000u, 0);
    if (value >= 10000u) t[n++] = value < 10000000u ? 'K' : 'M';
    return emit(out, t, n);
}

int fmt_count_capped(std::span<char> out, uint32_t value) {
    if (count_overflowed(value)) return emit(out, ">999M", 5);
    return fmt_u32_compact(out, value);
}

int fmt_hms(std::span<char> out, uint32_t seconds) {
    uint32_t h = seconds / 3600, m = seconds / 60 % 60, s = seconds % 60;
    char tmp[16];
    int n = 0;
    char hd[10];
    int nh = 0;
    do { hd[nh++] = static_cast<char>('0' + h % 10); h /= 10; } while (h != 0);
    while (nh > 0) tmp[n++] = hd[--nh];
    tmp[n++] = ':'; tmp[n++] = static_cast<char>('0' + m / 10); tmp[n++] = static_cast<char>('0' + m % 10);
    tmp[n++] = ':'; tmp[n++] = static_cast<char>('0' + s / 10); tmp[n++] = static_cast<char>('0' + s % 10);
    return emit(out, tmp, n);
}

int fmt_hex(std::span<char> out, uint32_t value, int digits) {
    if (digits < 1) digits = 1;
    if (digits > 8) digits = 8;
    char tmp[8];
    for (int i = 0; i < digits; i++) {
        uint32_t nib = (value >> (4 * (digits - 1 - i))) & 0xF;
        tmp[i] = static_cast<char>(nib < 10 ? '0' + nib : 'A' + nib - 10);
    }
    return emit(out, tmp, digits);
}

}  // namespace k2
