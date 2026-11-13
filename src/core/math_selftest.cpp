// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/core/math_selftest.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "k2/core/fmt.hpp"
#include "k2/core/hot.h"
#include "k2/drivers/adc_calc.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/readout_fmt.hpp"
#include "k2/ui/smoothing.hpp"

namespace k2::selftest {
namespace {

K2_HOT inline std::uint32_t fnv_step(std::uint32_t h, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        h ^= (v & 0xFFu);
        h *= 16777619u;
        v >>= 8;
    }
    return h;
}

K2_HOT inline std::uint32_t fnv_str(std::uint32_t h, const char* s) {
    while (*s) {
        h ^= static_cast<std::uint8_t>(*s++);
        h *= 16777619u;
    }
    return h;
}

template <typename T>
inline T keep(T v) {
#if defined(__GNUC__) || defined(__clang__)
    if constexpr (sizeof(T) <= 4) {
        asm volatile("" : "+r"(v) :: "memory");
        return v;
    }
#endif
    volatile T vv = v;
    return vv;
}

inline void enable_dwt() {
#if defined(__arm__) && !defined(__unix__)
    auto* const demcr = reinterpret_cast<volatile std::uint32_t*>(0xE000EDFCu);
    auto* const dwt_ctrl = reinterpret_cast<volatile std::uint32_t*>(0xE0001000u);
    *demcr |= (1u << 24);
    *dwt_ctrl |= 1u;
#endif
}

inline std::uint32_t read_cycles() {
#if defined(__arm__) && !defined(__unix__)
    return *reinterpret_cast<volatile std::uint32_t*>(0xE0001004u);
#else
    return 0;
#endif
}

K2_HOT bool check_scale_div(std::uint32_t& crc_out) {
    bool ok = true;
    char buf[20];
    // Verify exact arithmetic across readout::power_half (32-bit and high-power cases) & fmt3.
    const std::size_t n1 = readout::power_half(buf, readout::PowerKind::Pd, 20000u, 5000u, "");
    ok = ok && n1 == 7 && std::strcmp(buf, "PD 100W") == 0;
    const std::size_t n2 = readout::power_half(buf, readout::PowerKind::Pd, 48000u, 5000u, "");
    ok = ok && n2 == 7 && std::strcmp(buf, "PD 240W") == 0;
    const std::size_t n3 = readout::power_half(buf, readout::PowerKind::Plain, 5000u, 3000u, "");
    ok = ok && n3 == 6 && std::strcmp(buf, "5V 15W") == 0;

    std::uint32_t acc = 2166136261u;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const std::uint32_t mv = keep(5000u + (i & 15u) * 2800u);
        const std::uint32_t ma = keep(500u + (i & 7u) * 750u);
        const std::size_t len = readout::power_half(buf, readout::PowerKind::Pd, mv, ma, "");
        acc = fnv_step(acc, static_cast<std::uint32_t>(len));
        acc = fnv_str(acc, buf);
    }
    crc_out = acc;
    return ok && acc != 0u;
}

K2_HOT bool check_adc(std::uint32_t& crc_out) {
    bool ok = true;
    ok = ok && adc::to_mv(2048u, 2400u) == 1024;
    ok = ok && adc::vdd_mv(1489u) == 3300;
    ok = ok && !adc::ntc_temp(20u).valid && !adc::ntc_temp(4080u).valid;
    const adc::Temp t25 = adc::ntc_temp(2048u);
    ok = ok && t25.valid && t25.deci_c >= 245 && t25.deci_c <= 255;

    std::uint16_t raw_buf[adc::kSlots * 4];
    for (std::size_t s = 0; s < 4; ++s) {
        for (std::size_t c = 0; c < adc::kSlots; ++c) {
            raw_buf[s * adc::kSlots + c] = static_cast<std::uint16_t>(1400u + c * 150u + s);
        }
    }
    std::uint32_t acc = 2166136261u;
    for (std::uint32_t i = 0; i < 32u; ++i) {
        raw_buf[adc::Vref] = keep(static_cast<std::uint16_t>(1480u + (i & 7u)));
        adc::Raw avg{};
        adc::average_sweeps(raw_buf, avg);
        const adc::Reading rd = adc::make_reading(avg);
        const std::uint32_t ns = adc::sample_period_ns(static_cast<std::uint8_t>(i & 7u), 15'000'000u);
        acc = fnv_step(acc, static_cast<std::uint32_t>(rd.aux_mv));
        acc = fnv_step(acc, static_cast<std::uint32_t>(rd.temp.deci_c));
        acc = fnv_step(acc, ns);
    }
    crc_out = acc;
    return ok;
}

K2_HOT bool check_power(std::uint32_t& crc_out) {
    bool ok = true;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina226) == power::kShuntUohm;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina228) == power::kShuntUohm;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina237) == power::kShuntUohm;
    ok = ok && power::mw(19100, 3250) == 62075;
    ok = ok && power::mw(20000, -1500) == -30000;

    std::uint32_t acc = 2166136261u;
    constexpr power::Chip kChips[3] = {power::Chip::Ina226, power::Chip::Ina228, power::Chip::Ina237};
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const power::Chip ch = kChips[i % 3u];
        const std::int32_t rv = keep(static_cast<std::int32_t>(512000 + i * 4096u));
        const std::int32_t ri = keep(static_cast<std::int32_t>(153600 + i * 2048u) * ((i & 1u) ? 1 : -1));
        const std::int32_t mv = power::raw_to_mv(ch, rv);
        const std::int32_t ma = power::raw_to_ma(ch, ri);
        const std::int32_t w = power::mw(mv, ma);
        acc = fnv_step(acc, static_cast<std::uint32_t>(mv));
        acc = fnv_step(acc, static_cast<std::uint32_t>(ma));
        acc = fnv_step(acc, static_cast<std::uint32_t>(w));
    }
    crc_out = acc;
    return ok;
}

K2_HOT bool check_readout_fmt(std::uint32_t& crc_out) {
    bool ok = true;
    char buf[16];
    ok = ok && readout::fmt3(buf, 15000, 'V') == 5 && std::strcmp(buf, "15.0V") == 0;
    ok = ok && readout::fmt3(buf, 5080, 'V') == 5 && std::strcmp(buf, "5.08V") == 0;
    ok = ok && readout::fmt3(buf, 2040, 'A') == 5 && std::strcmp(buf, "2.04A") == 0;
    ok = ok && readout::fmt3(buf, 620, 'W') == 5 && std::strcmp(buf, "0.62W") == 0;
    ok = ok && readout::fmt3(buf, 100000, 'W') == 4 && std::strcmp(buf, "100W") == 0;

    std::uint32_t acc = 2166136261u;
    for (std::uint32_t i = 0; i < 32u; ++i) {
        const long long v = keep(static_cast<std::uint32_t>(350u + i * 4321u));
        const std::size_t n = readout::fmt3(buf, v, 'W');
        acc = fnv_step(acc, static_cast<std::uint32_t>(n));
        acc = fnv_str(acc, buf);
    }
    crc_out = acc;
    return ok;
}

K2_HOT bool check_fpu_sqrt(std::uint32_t& crc_out) {
    auto close = [](float a, float b) { return std::fabs(a - b) <= 1e-4f; };
    bool ok = true;
    ok = ok && close(std::sqrt(keep(3.0f) * 3.0f + keep(4.0f) * 4.0f), 5.0f);
    ok = ok && close(std::sqrt(keep(5.0f) * 5.0f + keep(12.0f) * 12.0f), 13.0f);
    ok = ok && close(std::sqrt(keep(8.0f) * 8.0f + keep(15.0f) * 15.0f), 17.0f);
    ok = ok && close(std::sqrt(keep(0.6f) * 0.6f + keep(0.8f) * 0.8f), 1.0f);

    float sum = 0.0f;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const float dx = keep(0.25f + static_cast<float>(i & 7u) * 1.5f);
        const float dy = keep(0.50f + static_cast<float>(i >> 3) * 1.25f);
        sum += std::sqrt(dx * dx + dy * dy);
    }
    const std::uint32_t qsum = static_cast<std::uint32_t>(sum * 16.0f + 0.5f);
    ok = ok && std::fabs(sum - 512.71765f) < 0.01f;
    crc_out = qsum;
    return ok;
}

K2_HOT bool check_one_euro(std::uint32_t& crc_out) {
    smoothing::Readout ro;
    ro.set(smoothing::Preset::Low);
    std::uint32_t acc = 2166136261u;
    for (std::uint32_t k = 0; k < 32u; ++k) {
        const std::int32_t jitter = static_cast<std::int32_t>(k & 1u ? 15 : -15);
        ro.push(keep(5000 + jitter), keep(2000 - jitter), k * 10u);
        acc = fnv_step(acc, static_cast<std::uint32_t>(ro.vbus_mv()));
        acc = fnv_step(acc, static_cast<std::uint32_t>(ro.current_ma()));
    }
    const bool ok = ro.vbus_mv() >= 4900 && ro.vbus_mv() <= 5100 &&
                    ro.current_ma() >= 1900 && ro.current_ma() <= 2100;
    crc_out = acc;
    return ok;
}

K2_HOT bool check_blend565(std::uint32_t& crc_out) {
    constexpr std::uint16_t kColors[6] = {0x0000u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu, 0x52AAu};
    bool ok = true;
    auto ref_ch = [](unsigned f, unsigned b, unsigned a) { return (f * a + b * (15u - a) + 7u) / 15u; };
    std::uint32_t acc = 2166136261u;
    for (unsigned i = 0; i < 6u; ++i) {
        const std::uint16_t fg = keep(kColors[i]);
        const std::uint16_t bg = keep(kColors[(i + 2u) % 6u]);
        std::uint16_t lut[16]{};
        make_blend_lut(fg, bg, lut);
        ok = ok && lut[0] == bg && lut[15] == fg;
        for (unsigned a = 0; a < 16u; ++a) {
            const unsigned er = ref_ch(fg >> 11, bg >> 11, a);
            const unsigned eg = ref_ch((fg >> 5) & 0x3Fu, (bg >> 5) & 0x3Fu, a);
            const unsigned eb = ref_ch(fg & 0x1Fu, bg & 0x1Fu, a);
            const std::uint16_t expected = static_cast<std::uint16_t>((er << 11) | (eg << 5) | eb);
            if (lut[a] != expected) ok = false;
            acc = fnv_step(acc, lut[a]);
        }
    }
    crc_out = acc;
    return ok;
}

}  // namespace

K2_HOT bool run_math(WriteFn write, void* ctx, Summary* out) {
    enable_dwt();

    struct Case {
        const char* name;
        bool (*fn)(std::uint32_t&);
    };
    static constexpr Case kCases[] = {
        {"scale_div", check_scale_div},
        {"adc_calc", check_adc},
        {"power_calc", check_power},
        {"readout_fmt", check_readout_fmt},
        {"fpu_sqrt", check_fpu_sqrt},
        {"one_euro", check_one_euro},
        {"blend565", check_blend565},
    };

#if defined(__OPTIMIZE_SIZE__)
    constexpr const char* kOpt = "Os";
#elif defined(__OPTIMIZE__)
    constexpr const char* kOpt = "O2+";
#else
    constexpr const char* kOpt = "O0";
#endif
#ifdef __NO_MATH_ERRNO__
    constexpr const char* kErrno = "off";
#else
    constexpr const char* kErrno = "on";
#endif
#if defined(__arm__)
    constexpr const char* kArch = "m4f";
#else
    constexpr const char* kArch = "host";
#endif

    char line[96];
    auto emit = [&](std::size_t n) {
        if (write && n) write(std::string_view(line, n), ctx);
    };

    emit(fmt::format(line, "math selftest (arch=%s opt=%s math_errno=%s):\r\n", kArch, kOpt, kErrno));

    std::uint8_t passed = 0;
    std::uint32_t total_cycles = 0;
    for (const Case& c : kCases) {
        std::uint32_t crc = 0;
        const std::uint32_t t0 = read_cycles();
        const bool ok = c.fn(crc);
        const std::uint32_t cyc = read_cycles() - t0;
        total_cycles += cyc;
        if (ok) ++passed;
        char name_pad[12];
        const std::size_t nl = std::strlen(c.name);
        for (std::size_t i = 0; i < 11u; ++i) name_pad[i] = i < nl ? c.name[i] : ' ';
        name_pad[11] = '\0';
        emit(fmt::format(line, "  [%s] %s sum=0x%08X  cyc=%u\r\n", ok ? "PASS" : "FAIL", name_pad, crc, cyc));
    }

    constexpr std::uint8_t kTotal = static_cast<std::uint8_t>(sizeof(kCases) / sizeof(kCases[0]));
    emit(fmt::format(line, "%s %u/%u checks, total_cyc=%u\r\n",
                     passed == kTotal ? "PASS" : "FAIL", unsigned(passed), unsigned(kTotal), total_cycles));

    if (out) *out = {passed, kTotal, total_cycles};
    return passed == kTotal;
}

}  // namespace k2::selftest
