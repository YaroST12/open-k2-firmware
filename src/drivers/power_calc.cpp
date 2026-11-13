// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Power monitor conversions, see k2/drivers/power_calc.hpp.
#include "k2/drivers/power_calc.hpp"

namespace k2::power {

namespace {

// Raw counts at 10.0 V and at 2.0 A per chip, and the current clamp in mA.
struct Scale { std::int32_t counts_10v, counts_2a, clamp_ma; };

constexpr Scale scale(Chip chip) {
    switch (chip) {
    case Chip::Ina228: return {204800, 307200, 54000};
    case Chip::Ina226: return {1024000, 307200, 26660};
    case Chip::Ina237: return {409600, 614400, 54000};
    default:           return {0, 0, 0};
    }
}

// Exact x / d for any 64-bit x and 1 <= d <= 65535 using 32-bit UDIV/reciprocal only.
std::uint64_t udiv64_by_u16(std::uint64_t x, std::uint16_t d) {
    const std::uint32_t hi = static_cast<std::uint32_t>(x >> 32);
    const std::uint32_t lo = static_cast<std::uint32_t>(x);
    if (hi == 0) return lo / d;
    const std::uint32_t q_hi = hi / d;
    std::uint32_t rem = hi % d;
    const std::uint32_t t1 = (rem << 16) | (lo >> 16);
    const std::uint32_t q1 = t1 / d;
    rem = t1 % d;
    const std::uint32_t t2 = (rem << 16) | (lo & 0xFFFFu);
    const std::uint32_t q2 = t2 / d;
    return (static_cast<std::uint64_t>(q_hi) << 32) | (q1 << 16) | q2;
}

}  // namespace

const char* name(Chip chip) {
    switch (chip) {
    case Chip::Ina228: return "INA228";
    case Chip::Ina226: return "INA226";
    case Chip::Ina237: return "INA237";
    default:           return "NONE";
    }
}

std::int32_t max_vbus_mv(Chip chip) {
    switch (chip) {
    case Chip::Ina226: return 36000;
    case Chip::Ina228:
    case Chip::Ina237: return 85000;
    default:           return 0;
    }
}

std::int32_t max_request_mv(Chip chip) {
    switch (chip) {
    case Chip::Ina226: return 36000;
    case Chip::Ina228:
    case Chip::Ina237: return 48000;
    default:           return 0;
    }
}

std::int32_t implied_shunt_uohm(Chip chip) {
    // Shunt-voltage LSB after the raw-count alignment shift, in pV * 1000:
    //   INA228 / INA226: 19,531,250 -> 19,531,250 / (1000 * 2 * 1,000,000) = 5 / 512.
    //   INA237:           9,765,625 ->  9,765,625 / (1000 * 2 * 1,000,000) = 5 / 1024.
    const std::int32_t c = scale(chip).counts_2a;
    switch (chip) {
    case Chip::Ina228:
    case Chip::Ina226: return (c * 5) >> 9;
    case Chip::Ina237: return (c * 5) >> 10;
    default:           return 0;
    }
}

char model_badge(Chip chip) {
    switch (chip) {
    case Chip::Ina228: return '8';
    case Chip::Ina237: return '7';
    case Chip::Ina226: return '6';
    default:           return '?';
    }
}

const char* format_model(std::span<char> out, Chip chip, unsigned version) {
    if (out.empty()) return out.data();
    const unsigned v = version > 99 ? 99 : version;
    const char text[] = {'N', model_badge(chip), '.', static_cast<char>('0' + v / 10),
                         static_cast<char>('0' + v % 10)};
    std::size_t n = 0;
    for (; n < sizeof text && n + 1 < out.size(); n++) out[n] = text[n];
    out[n] = '\0';
    return out.data();
}

int reg_bytes(Chip chip) { return chip == Chip::Ina228 ? 3 : 2; }

std::int32_t align_shunt(Chip chip, std::uint32_t reg) {
    if (chip == Chip::Ina228) {
        // 20-bit two's complement in bits 23..4; the result is that value times 4.
        return static_cast<std::int32_t>((reg & 0x00FFFFF0u) << 8) >> 10;
    }
    return static_cast<std::int16_t>(reg) * 128;
}

std::int32_t align_vbus(Chip chip, std::uint32_t reg) {
    if (chip == Chip::Ina228) return static_cast<std::int32_t>((reg >> 2) & 0x003FFFFCu);
    return static_cast<std::int32_t>((reg & 0xFFFFu) * 128u);
}

std::int32_t raw_to_mv(Chip chip, std::int32_t raw_v) {
    // Reduced 10000 / counts_10v: INA228 = 25/512, INA226 = 5/512, INA237 = 25/1024 (all power-of-two denominators).
    std::uint32_t mul = 0;
    unsigned shift = 0;
    switch (chip) {
    case Chip::Ina228: mul = 25; shift = 9; break;
    case Chip::Ina226: mul = 5;  shift = 9; break;
    case Chip::Ina237: mul = 25; shift = 10; break;
    default:           return 0;
    }
    const std::uint32_t mag = magnitude(raw_v);
    const std::uint32_t mv = static_cast<std::uint32_t>((std::uint64_t{mag} * mul + (1u << (shift - 1))) >> shift);
    return raw_v < 0 ? -static_cast<std::int32_t>(mv) : static_cast<std::int32_t>(mv);
}

std::int32_t raw_to_ma(Chip chip, std::int32_t raw_i) {
    const Scale s = scale(chip);
    if (s.counts_2a == 0) return 0;
    const std::uint32_t mag = magnitude(raw_i);
    // Reduced 2000 / counts_2a: 5/768 (INA228/INA226) or 5/1536 (INA237).
    // Any |raw_i| >= 20,000,000 gives >= 65,104 mA, exceeding the 54,000 mA max clamp.
    std::int32_t ma;
    if (mag >= 20'000'000u) {
        ma = s.clamp_ma;
    } else {
        const std::uint32_t den = (chip == Chip::Ina237) ? 1536u : 768u;
        const std::uint32_t rounded = (mag * 5u + den / 2u) / den;
        ma = rounded > static_cast<std::uint32_t>(s.clamp_ma) ? s.clamp_ma : static_cast<std::int32_t>(rounded);
    }
    return raw_i < 0 ? -ma : ma;
}

std::int32_t mw(std::int32_t vbus_mv, std::int32_t current_ma) {
    const std::uint64_t prod = std::uint64_t{magnitude(vbus_mv)} * magnitude(current_ma);
    const std::int32_t mag = static_cast<std::int32_t>(udiv64_by_u16(prod, 1000u));
    return (vbus_mv < 0) != (current_ma < 0) ? -mag : mag;
}

std::int32_t apply_deadband(std::int32_t current_ma) {
    return magnitude(current_ma) < static_cast<std::uint32_t>(kDeadbandMa) ? 0 : current_ma;
}

Flow flow_of(std::int32_t current_ma) {
    const std::int32_t i = apply_deadband(current_ma);
    return i > 0 ? Flow::Forward : i < 0 ? Flow::Reverse : Flow::Idle;
}

std::uint32_t magnitude(std::int32_t v) {
    return v < 0 ? 0u - static_cast<std::uint32_t>(v) : static_cast<std::uint32_t>(v);
}

void Average::push(std::int32_t v) {
    buf_[head_] = v;
    head_ = (head_ + 1) % kAvgLen;
    if (count_ < kAvgLen) count_++;
}

std::int32_t Average::mean() const {
    if (count_ == 0) return 0;
    std::int64_t sum = 0;
    for (std::size_t i = 0; i < count_; i++) sum += buf_[i];
    const std::uint64_t mag = sum < 0 ? static_cast<std::uint64_t>(-sum) : static_cast<std::uint64_t>(sum);
    const std::uint64_t q = (count_ == kAvgLen) ? (mag >> 7) : udiv64_by_u16(mag, static_cast<std::uint16_t>(count_));
    return sum < 0 ? -static_cast<std::int32_t>(q) : static_cast<std::int32_t>(q);
}

}  // namespace k2::power
