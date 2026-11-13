// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ADC front-end conversions, see k2/drivers/adc_calc.hpp.
#include "k2/drivers/adc_calc.hpp"

#include "k2/core/hot.h"

namespace k2::adc {

namespace {

// 10k / B3950 NTC resistance in ohms, -20 .. 120 C in 5 C steps. R = 10k * exp(3950 * (1/T - 1/298.15)).
K2_HOT_DATA constexpr std::array<std::uint32_t, 29> kNtcOhm = {
    105385, 77898, 58246, 44026, 33621, 25925, 20175, 15837, 12535, 10000, 8037, 6506, 5301, 4348, 3588, 2978, 2486, 2086, 1760, 1492, 1270, 1087, 934, 805, 698, 606, 529, 463, 407};
constexpr int kNtcFirstC = -20, kNtcStepC = 5;
constexpr std::uint32_t kPullUpOhm = 10000;

}  // namespace

K2_HOT void average_sweeps(std::span<const std::uint16_t> dma, Raw& out) {
    out.fill(0);
    const std::size_t sweeps = dma.size() / kSlots;
    if (sweeps == 0) return;
    std::array<std::uint32_t, kSlots> sum{};
    for (std::size_t i = 0; i < sweeps * kSlots; i++) sum[i % kSlots] += dma[i];
    for (std::size_t s = 0; s < kSlots; s++)
        out[s] = static_cast<std::uint16_t>((sum[s] + sweeps / 2) / sweeps);
}

K2_HOT std::int32_t to_mv(std::uint16_t raw, std::uint16_t vref_raw) {
    if (vref_raw == 0) return 0;
    return static_cast<std::int32_t>((std::uint32_t{raw} * kVrefMv + vref_raw / 2u) / vref_raw);
}

K2_HOT std::int32_t vdd_mv(std::uint16_t vref_raw) {
    if (vref_raw == 0) return 0;
    return static_cast<std::int32_t>((kFullScale * kVrefMv + vref_raw / 2u) / vref_raw);
}

K2_HOT Temp ntc_temp(std::uint16_t raw) {
    Temp t;
    // Within 1 % of either rail the divider is shorted or open, not measuring.
    if (raw < 41 || raw > kFullScale - 41) return t;
    // R = Rpu * raw / (4095 - raw)
    const std::uint32_t den = kFullScale - raw;
    const std::uint32_t ohm = (kPullUpOhm * raw + den / 2u) / den;
    t.valid = true;
    if (ohm >= kNtcOhm.front()) { t.deci_c = kNtcFirstC * 10; return t; }
    if (ohm <= kNtcOhm.back()) { t.deci_c = static_cast<std::int16_t>((kNtcFirstC + kNtcStepC * (static_cast<int>(kNtcOhm.size()) - 1)) * 10); return t; }
    std::size_t i = 0;
    while (ohm < kNtcOhm[i + 1]) i++;   // kNtcOhm[i] >= ohm > kNtcOhm[i+1]
    const std::uint32_t span = kNtcOhm[i] - kNtcOhm[i + 1];
    const std::uint32_t into = kNtcOhm[i] - ohm;
    t.deci_c = static_cast<std::int16_t>((kNtcFirstC + static_cast<int>(i) * kNtcStepC) * 10 +
                                         static_cast<int>(into * (kNtcStepC * 10) / span));
    return t;
}

K2_HOT Temp mcu_temp(std::uint16_t raw, std::uint16_t vref_raw) {
    Temp t;
    if (vref_raw == 0) return t;
    // deci-degrees above 25 C = (Vsense - V25) [mV] * 10000 / |slope| [uV/C] (the slope is negative
    // in (V25 - Vsense) / slope), rounded half away from zero
    const std::int32_t mv_diff = to_mv(raw, vref_raw) - kMcuV25Mv;
    if (mv_diff < -1000 || mv_diff > 1000) return t;
    const std::int32_t num = mv_diff * 10000;
    constexpr std::int32_t half = kMcuSlopeUvPerC / 2;
    const std::int32_t d = (num >= 0 ? num + half : num - half) / kMcuSlopeUvPerC;
    const std::int32_t deci = 250 + d;
    if (deci < -400 || deci > 1500) return t;
    t.deci_c = static_cast<std::int16_t>(deci);
    t.valid = true;
    return t;
}

K2_HOT Reading make_reading(const Raw& avg) {
    Reading r;
    r.raw = avg;
    r.vref_raw = avg[Vref];
    r.vdd_mv = vdd_mv(avg[Vref]);
    r.dp_mv = to_mv(avg[Dp], avg[Vref]);
    r.dm_mv = to_mv(avg[Dm], avg[Vref]);
    r.cc1_mv = to_mv(avg[Cc1], avg[Vref]);
    r.cc2_mv = to_mv(avg[Cc2], avg[Vref]);
    // Scaled before rounding, not to_mv() * 11, so the rounding error is not multiplied by 11.
    r.aux_mv = avg[Vref] == 0 ? 0 : static_cast<std::int32_t>(
        (std::uint32_t{avg[Aux]} * (kVrefMv * kAuxDivider) + avg[Vref] / 2u) / avg[Vref]);
    r.temp = ntc_temp(avg[Ntc]);
    r.mcu = mcu_temp(avg[Mcu], avg[Vref]);
    return r;
}

K2_HOT std::uint32_t sample_period_ns(std::uint8_t code, std::uint32_t adc_hz) {
    // Sample cycles x 2 (1.5, 7.5, 13.5, 28.5, 41.5, 55.5, 71.5, 239.5) so they stay integers.
    constexpr std::array<std::uint32_t, 8> kHalfCycles = {3, 15, 27, 57, 83, 111, 143, 479};
    if (adc_hz == 0) return 0;
    if (code > 7) code = 7;
    // 500,000,000 = 1,953,125 * 256; max (kHalfCycles[7] + 25) * 1,953,125 = 984,375,000 < 2^30.
    const std::uint32_t num = (kHalfCycles[code] + 25u) * 1'953'125u;
    std::uint32_t q = num / adc_hz;
    std::uint32_t r = num % adc_hz;
    for (int i = 0; i < 8; i++) {
        q <<= 1;
        if (r >= adc_hz - r) {
            r = r - (adc_hz - r);
            q |= 1u;
        } else {
            r <<= 1;
        }
    }
    return q;
}

Stats summarize(std::span<const std::uint16_t> samples) {
    Stats s;
    if (samples.empty()) return s;
    s.min = s.max = samples[0];
    std::uint32_t sum = 0;
    for (const std::uint16_t v : samples) {
        if (v < s.min) s.min = v;
        if (v > s.max) s.max = v;
        sum += v;
    }
    s.mean = static_cast<std::uint16_t>(sum / static_cast<std::uint32_t>(samples.size()));
    return s;
}

}  // namespace k2::adc
