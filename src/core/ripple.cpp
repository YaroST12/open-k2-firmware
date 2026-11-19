// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure 32-bit VBUS ripple analysis and oscilloscope column projection (k2/core/ripple.hpp).
#include "k2/core/ripple.hpp"

#include <algorithm>
#include "k2/drivers/adc_calc.hpp"

namespace k2::ripple {

namespace {

std::uint32_t isqrt32(std::uint32_t n) {
    std::uint32_t x = 0, bit = 1u << 30;
    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= x + bit) {
            n -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return x;
}

std::uint32_t span_samples_for(std::uint8_t timebase_idx, std::uint32_t period_ns, std::size_t total) {
    if (period_ns == 0 || total == 0) return 0;
    const std::uint8_t tb = timebase_idx < kTimebaseCount ? timebase_idx : kDefaultTimebase;
    const std::uint32_t window_us = std::uint32_t{kTimebases[tb].us_per_div} * kHorizDivs;
    // window_us * 1000 fits in uint32_t (max 8000 * 1000 = 8,000,000).
    const std::uint32_t n = (window_us * 1000u + period_ns / 2u) / period_ns;
    if (n < 2u) return static_cast<std::uint32_t>(std::min<std::size_t>(total, 2u));
    return static_cast<std::uint32_t>(std::min<std::size_t>(n, total));
}

}  // namespace

std::int32_t delta_to_mv(std::int32_t delta_counts, std::uint16_t vref_raw) {
    const bool neg = delta_counts < 0;
    const std::uint32_t mag = static_cast<std::uint32_t>(neg ? -delta_counts : delta_counts);
    std::uint32_t mv = 0;
    if (vref_raw != 0) {
        // mag * kVrefMv * kGainNum / (vref_raw * kGainDen), rounded.
        const std::uint32_t num = mag * adc::kVrefMv * static_cast<std::uint32_t>(kGainNum);
        const std::uint32_t den = std::uint32_t{vref_raw} * static_cast<std::uint32_t>(kGainDen);
        mv = (num + den / 2u) / den;
    } else {
        const std::uint32_t num = mag * static_cast<std::uint32_t>(kNominalVddMv * kGainNum);
        const std::uint32_t den = adc::kFullScale * static_cast<std::uint32_t>(kGainDen);
        mv = (num + den / 2u) / den;
    }
    return neg ? -static_cast<std::int32_t>(mv) : static_cast<std::int32_t>(mv);
}

std::uint16_t resolve_mv_per_div(std::uint8_t scale_idx, std::int32_t peak_mv) {
    if (scale_idx > 0 && scale_idx < kScaleCount) return kScaleMv[scale_idx];
    const std::int32_t abs_peak = peak_mv < 0 ? -peak_mv : peak_mv;
    // Plot has +-2 vertical divisions; keep ~15% headroom so peaks don't clip the frame.
    constexpr std::uint16_t kAutoSteps[] = {10, 20, 50, 100, 200, 500, 1000};
    for (const std::uint16_t step : kAutoSteps) {
        const std::int32_t cap = static_cast<std::int32_t>(step) * 2 * 85 / 100;
        if (abs_peak <= cap) return step;
    }
    return 1000;
}

Stats analyze(std::span<const std::uint16_t> samples, std::uint32_t period_ns, std::uint16_t vref_raw,
              std::uint8_t timebase_idx, Trigger trig) {
    Stats st;
    if (samples.size() <= kSkipSamples + 2 || period_ns == 0) return st;
    const auto usable = samples.subspan(kSkipSamples);
    st.valid = true;

    std::uint16_t lo = usable[0], hi = usable[0];
    std::uint32_t sum = 0;
    for (const std::uint16_t v : usable) {
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
    }
    const std::uint32_t n = static_cast<std::uint32_t>(usable.size());
    st.mean_raw = static_cast<std::uint16_t>((sum + n / 2u) / n);
    st.pp_raw = static_cast<std::uint16_t>(hi - lo);
    st.below_floor = st.pp_raw <= kNoiseFloorCounts;
    st.min_ac_mv = delta_to_mv(static_cast<std::int32_t>(lo) - st.mean_raw, vref_raw);
    st.max_ac_mv = delta_to_mv(static_cast<std::int32_t>(hi) - st.mean_raw, vref_raw);
    st.vpp_mv = delta_to_mv(static_cast<std::int32_t>(st.pp_raw), vref_raw);

    std::uint32_t sum_sq = 0;
    for (const std::uint16_t v : usable) {
        const std::int32_t d = static_cast<std::int32_t>(v) - st.mean_raw;
        sum_sq += static_cast<std::uint32_t>(d * d);
    }
    const std::uint32_t rms_counts = isqrt32((sum_sq + n / 2u) / n);
    st.vrms_mv = delta_to_mv(static_cast<std::int32_t>(rms_counts), vref_raw);

    // Rising zero-crossings with hysteresis (1/6 of p-p, minimum 3 counts) for both frequency and trigger.
    const std::int32_t hyst = std::max<std::int32_t>(3, st.pp_raw / 6);
    const std::uint32_t win = span_samples_for(timebase_idx, period_ns, usable.size());
    const std::uint32_t max_trig = usable.size() > win ? static_cast<std::uint32_t>(usable.size() - win) : 0u;

    bool armed = false;
    std::uint32_t crossings = 0;
    std::uint32_t first_cross = 0, last_cross = 0;
    bool trig_found = false;

    for (std::uint32_t i = 0; i < n; i++) {
        const std::int32_t d = static_cast<std::int32_t>(usable[i]) - st.mean_raw;
        if (!armed) {
            if (d <= -hyst) armed = true;
        } else if (d >= hyst) {
            armed = false;
            if (crossings == 0) first_cross = i;
            last_cross = i;
            crossings++;
            if (trig == Trigger::Auto && !trig_found && i <= max_trig) {
                st.trigger_idx = static_cast<std::uint16_t>(i);
                trig_found = true;
            }
        }
    }

    if (!st.below_floor && crossings >= 2 && last_cross > first_cross) {
        const std::uint32_t cycles = crossings - 1u;
        const std::uint32_t span = last_cross - first_cross;
        const std::uint32_t fs_hz = 1'000'000'000u / period_ns;
        st.freq_hz = (cycles * fs_hz + span / 2u) / span;
    }
    return st;
}

void project(std::span<const std::uint16_t> samples, std::uint32_t period_ns, std::uint16_t vref_raw,
             const Stats &stats, std::uint8_t timebase_idx, std::uint16_t mv_per_div,
             std::uint8_t plot_h, std::span<ColSpan> cols) {
    if (cols.empty() || plot_h < 2) return;
    const std::uint8_t mid = static_cast<std::uint8_t>(plot_h / 2u);
    if (!stats.valid || samples.size() <= kSkipSamples + 2 || period_ns == 0 || mv_per_div == 0) {
        for (ColSpan &c : cols) c = {mid, mid};
        return;
    }

    const auto usable = samples.subspan(kSkipSamples);
    const std::uint32_t win = span_samples_for(timebase_idx, period_ns, usable.size());
    const std::uint32_t start = std::min<std::uint32_t>(stats.trigger_idx,
        usable.size() > win ? static_cast<std::uint32_t>(usable.size() - win) : 0u);
    const std::int32_t half_fs_mv = static_cast<std::int32_t>(mv_per_div) * (kVertDivs / 2);
    const std::int32_t half_px = static_cast<std::int32_t>(plot_h - 1) / 2;
    const std::uint32_t w = static_cast<std::uint32_t>(cols.size());

    auto to_y = [&](std::uint16_t raw) -> std::uint8_t {
        const std::int32_t mv = delta_to_mv(static_cast<std::int32_t>(raw) - stats.mean_raw, vref_raw);
        std::int32_t y = half_px - (mv * half_px) / half_fs_mv;
        if (y < 0) y = 0;
        if (y >= plot_h) y = plot_h - 1;
        return static_cast<std::uint8_t>(y);
    };

    std::uint8_t prev_lo = 0, prev_hi = 0;
    for (std::uint32_t x = 0; x < w; x++) {
        std::uint32_t i0 = start + (x * win) / w;
        std::uint32_t i1 = start + ((x + 1u) * win) / w;
        if (i1 <= i0) i1 = i0 + 1u;
        if (i0 >= usable.size()) i0 = static_cast<std::uint32_t>(usable.size() - 1);
        if (i1 > usable.size()) i1 = static_cast<std::uint32_t>(usable.size());

        std::uint8_t ylo = 255, yhi = 0;
        for (std::uint32_t i = i0; i < i1; i++) {
            const std::uint8_t y = to_y(usable[i]);
            if (y < ylo) ylo = y;
            if (y > yhi) yhi = y;
        }
        // Connect to previous column so steep edges form a continuous trace.
        if (x > 0) {
            if (ylo > prev_hi + 1) ylo = static_cast<std::uint8_t>(prev_hi + 1);
            if (yhi + 1 < prev_lo) yhi = static_cast<std::uint8_t>(prev_lo - 1);
        }
        cols[x] = {ylo, yhi};
        prev_lo = ylo;
        prev_hi = yhi;
    }
}

void fmt_vpp(const Stats &s, StrBuf<16> &out) {
    out.clear();
    if (!s.valid) { out.put("--"); return; }
    if (s.below_floor) { out.put("< floor"); return; }
    if (s.vpp_mv >= 1000) {
        out.put_fixed(s.vpp_mv, 2).put(" Vpp");
    } else {
        out.put_u32(static_cast<std::uint32_t>(s.vpp_mv)).put(" mVpp");
    }
}

void fmt_vrms(const Stats &s, StrBuf<16> &out) {
    out.clear();
    if (!s.valid || s.below_floor) { out.put("--"); return; }
    out.put_u32(static_cast<std::uint32_t>(s.vrms_mv)).put(" mVrms");
}

void fmt_freq(const Stats &s, StrBuf<16> &out) {
    out.clear();
    if (!s.valid || s.below_floor || s.freq_hz == 0) { out.put("--"); return; }
    if (s.freq_hz >= 100000u) {
        out.put_u32((s.freq_hz + 500u) / 1000u).put(" kHz");
    } else if (s.freq_hz >= 1000u) {
        out.put_fixed(static_cast<std::int32_t>(s.freq_hz), 1).put(" kHz");
    } else {
        out.put_u32(s.freq_hz).put(" Hz");
    }
}

}  // namespace k2::ripple
