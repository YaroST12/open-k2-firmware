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

// raw * num / den, rounded half away from zero.
constexpr std::int64_t scale_round(std::int64_t raw, std::int64_t num, std::int64_t den) {
    const std::int64_t p = raw * num;
    return (p >= 0 ? p + den / 2 : p - den / 2) / den;
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
    //   INA228 78.125 nV << 2 = 19.53125 nV, INA237 1.25 uV >> ... / 128 = 9.765625 nV,
    //   INA226 2.5 uV / 128 = 19.53125 nV.
    std::int64_t lsb_pv_x1000 = 0;
    switch (chip) {
    case Chip::Ina228: lsb_pv_x1000 = 19531250; break;
    case Chip::Ina237: lsb_pv_x1000 = 9765625; break;
    case Chip::Ina226: lsb_pv_x1000 = 19531250; break;
    default: return 0;
    }
    // R = (counts_2a * LSB) / 2 A; pV / A = pOhm; /1e6 -> uOhm.
    return static_cast<std::int32_t>(std::int64_t{scale(chip).counts_2a} * lsb_pv_x1000 / (1000 * 2 * 1000000));
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
    const std::int32_t den = scale(chip).counts_10v;
    return den == 0 ? 0 : static_cast<std::int32_t>(scale_round(raw_v, 10000, den));
}

std::int32_t raw_to_ma(Chip chip, std::int32_t raw_i) {
    const Scale s = scale(chip);
    if (s.counts_2a == 0) return 0;
    const std::int64_t ma = scale_round(raw_i, 2000, s.counts_2a);
    const std::int64_t lim = s.clamp_ma;
    return static_cast<std::int32_t>(ma > lim ? lim : ma < -lim ? -lim : ma);
}

std::int32_t mw(std::int32_t vbus_mv, std::int32_t current_ma) {
    return static_cast<std::int32_t>(std::int64_t{vbus_mv} * current_ma / 1000);
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
    return static_cast<std::int32_t>(sum / static_cast<std::int64_t>(count_));
}

}  // namespace k2::power
