// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// On-device correctness check and DWT cycle benchmark of the hot paths (see docs/MATH_SELFTEST.md).
//
// Every check calls the production function, feeds it deterministic input laundered through keep() so the compiler cannot
// fold the loop, validates the result against an independent reference or invariants, and hashes everything it saw into
// one FNV sum. A deterministic check passes only when its sum equals the golden below (harvested on the host from this very
// code, so a device FAIL is a real host/target difference); the two float checks (fpu_sqrt, one_euro) are tolerance checks.
// No check touches hardware: drawing goes to a RAM PixelSink (the previous lcd::Sink is put back), buffers come from the scratch
// arena, and each check runs with IRQs masked.
#include "k2/core/math_selftest.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "k2/core/fmt.hpp"
#include "k2/core/hot.h"
#include "k2/core/math_opt.hpp"
#include "k2/core/scratch.hpp"
#include "k2/drivers/adc_calc.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/drivers/power_calc.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vdm.hpp"
#include "k2/pd/pdtrace.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/pixel_sink.hpp"
#include "k2/ui/readout_fmt.hpp"
#include "k2/ui/smoothing.hpp"
#include "k2/ui/text.hpp"

#if defined(__arm__) && !defined(__unix__)
#include "tusb.h"
#define K2_SELFTEST_TUFIFO 1
#else
#define K2_SELFTEST_TUFIFO 0
#endif

// The cold-probe section sits at the very end of .text (link.ld), above the 256 KB zero-wait window.
#if defined(__arm__)
#define K2_COLD_PROBE [[gnu::section(".cold_probe")]]
#else
#define K2_COLD_PROBE
#endif

namespace k2::selftest {
namespace {

using u64 = std::uint64_t;  // k2-allow-64bit
using i64 = std::int64_t;   // k2-allow-64bit
using kit::Box;
using kit::fill_except;

constexpr std::uint32_t kFnvBasis = 2166136261u;
constexpr std::uintptr_t kZeroWaitEnd = 0x08040000u;

// What a check hands back: its sum, and the highest address among the production functions it called (zero-wait check).
struct Out {
    std::uint32_t crc = 0;
    std::uintptr_t top = 0;
};

template <class F>
inline void touch(Out& o, F* fn) {
    const auto a = reinterpret_cast<std::uintptr_t>(fn);
    if (a > o.top) o.top = a;
}

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

K2_HOT inline std::uint32_t fnv_u64(std::uint32_t h, u64 v) {
    return fnv_step(fnv_step(h, static_cast<std::uint32_t>(v)), static_cast<std::uint32_t>(v >> 32));
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

// ---------------------------------------------------------------------------------------------------------------------
// scale_div, adc_calc, power_calc: the original arithmetic checks

K2_HOT bool check_scale_div(Out& o) {
    bool ok = true;
    char buf[20];
    // Verify exact arithmetic across readout::power_half (32-bit and high-power cases) & fmt3.
    const std::size_t n1 = readout::power_half(buf, readout::PowerKind::Pd, 20000u, 5000u, "");
    ok = ok && n1 == 7 && std::strcmp(buf, "PD 100W") == 0;
    const std::size_t n2 = readout::power_half(buf, readout::PowerKind::Pd, 48000u, 5000u, "");
    ok = ok && n2 == 7 && std::strcmp(buf, "PD 240W") == 0;
    const std::size_t n3 = readout::power_half(buf, readout::PowerKind::Plain, 5000u, 3000u, "");
    ok = ok && n3 == 6 && std::strcmp(buf, "5V 15W") == 0;

    std::uint32_t acc = kFnvBasis;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const std::uint32_t mv = keep(5000u + (i & 15u) * 2800u);
        const std::uint32_t ma = keep(500u + (i & 7u) * 750u);
        const std::size_t len = readout::power_half(buf, readout::PowerKind::Pd, mv, ma, "");
        acc = fnv_step(acc, static_cast<std::uint32_t>(len));
        acc = fnv_str(acc, buf);
    }
    o.crc = acc;
    return ok && acc != 0u;
}

K2_HOT bool check_adc(Out& o) {
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
    std::uint32_t acc = kFnvBasis;
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
    o.crc = acc;
    return ok;
}

K2_HOT bool check_power(Out& o) {
    bool ok = true;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina226) == power::kShuntUohm;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina228) == power::kShuntUohm;
    ok = ok && power::implied_shunt_uohm(power::Chip::Ina237) == power::kShuntUohm;
    ok = ok && power::mw(19100, 3250) == 62075;
    ok = ok && power::mw(20000, -1500) == -30000;

    std::uint32_t acc = kFnvBasis;
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

    // The 64-bit slow paths. mV x mA above 2^32 reaches udiv64_by_u16 with hi != 0 (the loop above never does).
    ok = ok && power::mw(keep(85000), keep(54000)) == 4'590'000;
    ok = ok && power::mw(keep(-85000), keep(54000)) == -4'590'000;
    ok = ok && power::mw(keep(80000), keep(-60000)) == -4'800'000;
    for (std::uint32_t i = 0; i < 16u; ++i) {
        const std::int32_t mv = keep(static_cast<std::int32_t>(40000u + i * 3000u));
        const std::int32_t ma = keep(static_cast<std::int32_t>(30000u + i * 1500u));
        const std::int32_t w = power::mw(mv, ma);
        const u64 prod = u64{static_cast<std::uint32_t>(mv)} * static_cast<std::uint32_t>(ma);
        const u64 q = static_cast<std::uint32_t>(w);
        ok = ok && q * 1000u <= prod && prod - q * 1000u < 1000u;   // exact floor, checked without a 64-bit divide
        acc = fnv_step(acc, static_cast<std::uint32_t>(w));
    }

    // The raw_to_ma clamp branch (|raw| >= 20'000'000 gives the chip limit directly) and the rounding clamp below it.
    struct MaRow {
        power::Chip chip;
        std::int32_t raw, want;
    };
    static constexpr MaRow kMa[] = {
        {power::Chip::Ina226, 25'000'000, 26660},   {power::Chip::Ina226, -25'000'000, -26660},
        {power::Chip::Ina226, 10'000'000, 26660},   {power::Chip::Ina228, 9'000'000, 54000},
        {power::Chip::Ina237, 19'000'000, 54000},   {power::Chip::Ina237, -30'000'000, -54000},
        {power::Chip::Ina226, 3'072'000, 20000},
    };
    for (const MaRow& r : kMa) {
        const std::int32_t got = power::raw_to_ma(r.chip, keep(r.raw));
        ok = ok && got == r.want;
        acc = fnv_step(acc, static_cast<std::uint32_t>(got));
    }

    // Average: a sum past 2^32 (udiv64_by_u16 with hi != 0), a negative one, and the full 128-deep ring (shift path).
    power::Average avg;
    for (int k = 0; k < 5; ++k) avg.push(keep(std::int32_t{1'073'741'824}));
    ok = ok && avg.count() == 5 && avg.mean() == 1'073'741'824;
    acc = fnv_step(acc, static_cast<std::uint32_t>(avg.mean()));
    avg.reset();
    for (int k = 0; k < 5; ++k) avg.push(keep(std::int32_t{-1'500'000'000}));
    ok = ok && avg.mean() == -1'500'000'000;
    acc = fnv_step(acc, static_cast<std::uint32_t>(avg.mean()));
    avg.reset();
    for (std::int32_t k = 0; k < 128; ++k) avg.push(keep(k * 1000));
    ok = ok && avg.count() == power::kAvgLen && avg.mean() == 63500;
    acc = fnv_step(acc, static_cast<std::uint32_t>(avg.mean()));

    touch(o, &power::mw);
    touch(o, &power::raw_to_ma);
    o.crc = acc;
    return ok;
}

// math_opt: the 64-bit divide helpers the loops above stay clear of.
K2_HOT bool check_math_opt(Out& o) {
    struct Div {
        u64 n;
        std::uint32_t d;
    };
    static constexpr Div kDiv[] = {
        {0xFFFFFFFE00000001ull, 0xFFFFFFFFu}, {0x123456789ABCDEF0ull, 1000003u}, {1ull << 40, 3u},
        {(1ull << 32) + 5u, 7u},             {~0ull, 1u},                        {1ull << 63, 0xFFFFFFFFu},
        {5'000'000'000ull, 65535u},          {0x1'0000'0000ull, 0x10000u},
    };
    bool ok = true;
    std::uint32_t acc = kFnvBasis;
    for (const Div& c : kDiv) {
        const u64 n = keep(c.n);
        const std::uint32_t d = keep(c.d);
        const u64 q = math::div_u64_u32(n, d);
        const u64 qs = math::div_u64_u32_slow(n, d);
        const u64 back = q * d;
        ok = ok && q == qs && back <= n && n - back < d;   // exact floor, checked without a 64-bit divide
        acc = fnv_u64(acc, q);
    }
    ok = ok && math::div_u64_u32(keep(u64{7}), 0u) == 0 && math::div_u64_u32_slow(keep(5'000'000'000ull), 0u) == 0;
    ok = ok && math::div_u64_u32_slow(keep(u64{3}), 7u) == 0 && math::div_u64_u32_slow(keep(u64{100}), 7u) == 14;

    ok = ok && math::div_i64_to_i32(keep(i64{-5'000'000'000}), 3) == -1'666'666'666;
    ok = ok && math::div_i64_to_i32(keep(i64{4'000'000'000}), 7) == 571'428'571;
    ok = ok && math::div_i64_to_i32(keep(i64{-9}), 2) == -4;
    ok = ok && math::mul_div_i32(keep(-2'000'000'000), 5, 7) == -1'428'571'428;
    ok = ok && math::mul_div_u32(keep(0xFFFFFFFFu), 0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFFu;
    ok = ok && math::mw_of(keep(85000u), keep(54000u)) == 4'590'000u;
    ok = ok && math::div_u64_to_u32(keep(5'000'000'000ull), 65535u) == 76295u;
    acc = fnv_step(acc, math::mw_of(keep(85000u), keep(54000u)));
    acc = fnv_step(acc, static_cast<std::uint32_t>(math::mul_div_i32(keep(-2'000'000'000), 5, 7)));

    touch(o, &math::div_u64_u32_slow);   // header-inline and not pinned: expect zw=0 (side issue)
    o.crc = acc;
    return ok;
}

K2_HOT bool check_readout_fmt(Out& o) {
    bool ok = true;
    char buf[16];
    ok = ok && readout::fmt3(buf, 15000, 'V') == 5 && std::strcmp(buf, "15.0V") == 0;
    ok = ok && readout::fmt3(buf, 5080, 'V') == 5 && std::strcmp(buf, "5.08V") == 0;
    ok = ok && readout::fmt3(buf, 2040, 'A') == 5 && std::strcmp(buf, "2.04A") == 0;
    ok = ok && readout::fmt3(buf, 620, 'W') == 5 && std::strcmp(buf, "0.62W") == 0;
    ok = ok && readout::fmt3(buf, 100000, 'W') == 4 && std::strcmp(buf, "100W") == 0;

    std::uint32_t acc = kFnvBasis;
    for (std::uint32_t i = 0; i < 32u; ++i) {
        const long long v = keep(static_cast<std::uint32_t>(350u + i * 4321u));
        const std::size_t n = readout::fmt3(buf, v, 'W');
        acc = fnv_step(acc, static_cast<std::uint32_t>(n));
        acc = fnv_str(acc, buf);
    }
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// float checks (tolerance, no golden: the target FPU and the host may round differently)

K2_HOT bool check_fpu_sqrt(Out& o) {
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
    o.crc = qsum;
    return ok;
}

K2_HOT bool check_one_euro(Out& o) {
    smoothing::Readout ro;
    ro.set(smoothing::Preset::Low);
    std::uint32_t acc = kFnvBasis;
    for (std::uint32_t k = 0; k < 32u; ++k) {
        const std::int32_t jitter = static_cast<std::int32_t>(k & 1u ? 15 : -15);
        ro.push(keep(5000 + jitter), keep(2000 - jitter), k * 10u);
        acc = fnv_step(acc, static_cast<std::uint32_t>(ro.vbus_mv()));
        acc = fnv_step(acc, static_cast<std::uint32_t>(ro.current_ma()));
    }
    // Host finals: 5003 mV / 1997 mA. The filter state is float, so a tolerance (not a golden) of +-3 judges the target.
    const bool ok = ro.vbus_mv() >= 5000 && ro.vbus_mv() <= 5006 &&
                    ro.current_ma() >= 1994 && ro.current_ma() <= 2000;
    o.crc = acc;
    return ok;
}

K2_HOT bool check_blend565(Out& o) {
    constexpr std::uint16_t kColors[6] = {0x0000u, 0xFFFFu, 0xF800u, 0x07E0u, 0x001Fu, 0x52AAu};
    bool ok = true;
    auto ref_ch = [](unsigned f, unsigned b, unsigned a) { return (f * a + b * (15u - a) + 7u) / 15u; };
    std::uint32_t acc = kFnvBasis;
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
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Drawing into RAM. The text / fill paths stream into the lcd sink; here the sink is a PixelSink over a scratch buffer, and
// the previous sink (normally none) is put back on every exit, so nothing reaches the panel.

PixelSink* g_target = nullptr;

K2_HOT void sink_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) { g_target->window(x0, y0, x1, y1); }
K2_HOT void sink_pixels(const std::uint16_t* px, std::uint32_t n) { g_target->push(px, n); }
K2_HOT void sink_fill(std::uint16_t c, std::uint32_t n) { g_target->fill(c, n); }
constexpr lcd::Sink kRamSink{sink_begin, sink_pixels, sink_fill};

struct SinkGuard {
    const lcd::Sink* prev;
    explicit SinkGuard(PixelSink& t) : prev(lcd::get_sink()) {
        g_target = &t;
        lcd::set_sink(&kRamSink);
    }
    ~SinkGuard() {
        lcd::set_sink(prev);
        g_target = nullptr;
    }
    SinkGuard(const SinkGuard&) = delete;
    SinkGuard& operator=(const SinkGuard&) = delete;
};

constexpr std::uint16_t kSentinel = 0xA5A5u;
struct Px {
    std::uint16_t v[1440];
};

// One text run drawn at (0,0) of an exactly sized surface, fg white on bg black: every pixel must be one of the 16 blend levels
// (nothing left of the sentinel, so every cell was written), both ends of the ramp must occur, and no glyph may be missing.
K2_HOT bool run_text(std::uint32_t& acc, const Font& f, std::string_view text, unsigned cps, bool rot, TextDir dir, Px& buf) {
    const unsigned w = rot ? f.height : cps * f.adv;
    const unsigned h = rot ? cps * f.adv : f.height;
    const unsigned n = w * h;
    if (n > sizeof(buf.v) / sizeof(buf.v[0])) return false;
    for (unsigned i = 0; i < n; ++i) buf.v[i] = kSentinel;
    PixelSink sink;
    sink.attach(buf.v, static_cast<int>(w), 0, static_cast<int>(h));
    const std::uint32_t missing0 = g_missing_glyphs;
    {
        const SinkGuard guard(sink);
        const Surface s{0, 0, static_cast<std::int16_t>(w), static_cast<std::int16_t>(h)};
        if (rot) gfx_text_rot(s, f, 0, 0, text, 0xFFFFu, 0u, dir);
        else gfx_text(s, f, 0, 0, text, 0xFFFFu, 0u);
    }
    std::uint16_t lut[16]{};
    make_blend_lut(0xFFFFu, 0u, lut);
    bool ok = g_missing_glyphs == missing0;
    unsigned fg = 0, bg = 0;
    std::uint16_t prev = lut[0];
    for (unsigned i = 0; i < n; ++i) {
        const std::uint16_t px = buf.v[i];
        if (px != prev) {   // runs of one level are the common case: look the level up only on a change
            bool known = false;
            for (const std::uint16_t l : lut) known = known || l == px;
            ok = ok && known;
            prev = px;
        }
        fg += px == lut[15];
        bg += px == lut[0];
        acc = fnv_step(acc, px);
    }
    return ok && fg > 0 && bg > 0;
}

K2_HOT bool check_text_aa(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    Px& buf = scratch::as<Px>();
    std::uint32_t acc = kFnvBasis;
    bool ok = true;
    // 10 code points: ASCII, the degree sign and the ohm sign (the two-byte UTF-8 path and the compressed font decode).
    ok = ok && run_text(acc, font_16, "9.0V 3A \xC2\xB0\xCE\xA9", 10, false, TextDir::Ccw, buf);
    ok = ok && run_text(acc, font_12, "PDO 20V 5A", 10, false, TextDir::Ccw, buf);
    ok = ok && run_text(acc, font_16, "Vbus", 4, true, TextDir::Ccw, buf);
    ok = ok && run_text(acc, font_16, "Vbus", 4, true, TextDir::Cw, buf);
    touch(o, &gfx_text);
    touch(o, &gfx_text_rot);
    o.crc = acc;
    return ok;
}

K2_HOT bool check_text_big(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    Px& buf = scratch::as<Px>();
    std::uint32_t acc = kFnvBasis;
    bool ok = true;
    ok = ok && run_text(acc, font_24, "5.00V", 5, false, TextDir::Ccw, buf);
    ok = ok && run_text(acc, font_48, "-1", 2, false, TextDir::Ccw, buf);
    ok = ok && run_text(acc, font_64, "8", 1, false, TextDir::Ccw, buf);
    touch(o, &gfx_text);
    o.crc = acc;
    return ok;
}

K2_HOT bool check_gfx_fill(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    Px& buf = scratch::as<Px>();
    constexpr int kW = 100, kH = 12;
    const Surface s{0, 0, kW, kH};
    bool ok = true;
    std::uint32_t acc = kFnvBasis;

    // fill_except: a ground with holes (overlapping, clipped at the right and at the top-left).
    {
        for (int i = 0; i < kW * kH; ++i) buf.v[i] = kSentinel;
        PixelSink sink;
        sink.attach(buf.v, kW, 0, kH);
        static constexpr Box kHoles[4] = {{10, 2, 20, 4}, {25, 3, 10, 6}, {90, 8, 30, 10}, {-5, -3, 8, 6}};
        {
            const SinkGuard guard(sink);
            fill_except(s, Box{0, 0, kW, kH}, kHoles, 4, 0x1234u);
        }
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                bool hole = false;
                for (const Box& b : kHoles) hole = hole || (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
                const std::uint16_t want = hole ? kSentinel : 0x1234u;
                ok = ok && buf.v[y * kW + x] == want;
                acc = fnv_step(acc, buf.v[y * kW + x]);
            }
        }
    }
    // gfx_fill: three rectangles, the last two cut by the surface edges, applied in order.
    {
        for (int i = 0; i < kW * kH; ++i) buf.v[i] = kSentinel;
        PixelSink sink;
        sink.attach(buf.v, kW, 0, kH);
        struct R {
            int x, y, w, h;
            std::uint16_t c;
        };
        static constexpr R kR[3] = {{5, 1, 30, 4, 0x07E0u}, {-4, 9, 20, 6, 0xF800u}, {95, 0, 20, 3, 0x001Fu}};
        {
            const SinkGuard guard(sink);
            for (const R& r : kR) {
                gfx_fill(s, static_cast<std::int16_t>(keep(r.x)), static_cast<std::int16_t>(keep(r.y)),
                         static_cast<std::int16_t>(r.w), static_cast<std::int16_t>(r.h), r.c);
            }
        }
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                std::uint16_t want = kSentinel;
                for (const R& r : kR) {
                    if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) want = r.c;
                }
                ok = ok && buf.v[y * kW + x] == want;
                acc = fnv_step(acc, buf.v[y * kW + x]);
            }
        }
    }
    touch(o, &gfx_fill);
    touch(o, &fill_except);
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// memcpy / memmove / memset / strlen: small and ~1 KB, aligned and not. The reference is a volatile byte loop (a plain one
// would be turned back into a mem* call).

K2_HOT bool check_mem_ops(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    static_assert(scratch::kSize >= 3432);
    std::uint8_t* const a = scratch::arena();
    std::uint8_t* const b = a + 1716;
    volatile std::uint8_t* const va = a;
    volatile std::uint8_t* const vb = b;
    auto pat = [](unsigned k) { return static_cast<std::uint8_t>(k * 37u + 11u); };
    constexpr unsigned kGuard = 8;
    std::uint32_t acc = kFnvBasis;
    bool ok = true;
    auto eat = [&](std::uint8_t v) { acc = (acc ^ v) * 16777619u; };

    struct Cp {
        unsigned n, so, d;
    };
    static constexpr Cp kCp[] = {{7, 0, 0}, {33, 1, 2}, {255, 3, 1}, {1024, 0, 0}, {1024, 1, 3}};
    for (const Cp& c : kCp) {
        for (unsigned k = 0; k < c.so + c.n; ++k) va[k] = pat(k + c.n);
        for (unsigned k = 0; k < c.d + c.n + kGuard; ++k) vb[k] = 0xEE;
        std::memcpy(b + keep(c.d), a + keep(c.so), keep(c.n));
        for (unsigned k = 0; k < c.d + c.n + kGuard; ++k) {
            const bool in = k >= c.d && k < c.d + c.n;
            const std::uint8_t want = in ? pat(c.so + (k - c.d) + c.n) : std::uint8_t{0xEE};
            const std::uint8_t got = vb[k];
            ok = ok && got == want;
            eat(got);
        }
    }

    struct Set {
        unsigned n, off;
        std::uint8_t val;
    };
    static constexpr Set kSet[] = {{9, 1, 0x5A}, {1024, 0, 0xC3}, {1021, 3, 0x7E}};
    for (const Set& c : kSet) {
        for (unsigned k = 0; k < c.off + c.n + kGuard; ++k) vb[k] = 0x11;
        std::memset(b + keep(c.off), keep(static_cast<int>(c.val)), keep(c.n));
        for (unsigned k = 0; k < c.off + c.n + kGuard; ++k) {
            const bool in = k >= c.off && k < c.off + c.n;
            const std::uint8_t got = vb[k];
            ok = ok && got == (in ? c.val : std::uint8_t{0x11});
            eat(got);
        }
    }

    constexpr unsigned kMv = 590, kSpan = 600;
    for (unsigned k = 0; k < kSpan; ++k) va[k] = pat(k);
    std::memmove(a + keep(3u), a + keep(0u), keep(kMv));   // overlap, destination above the source: copied backwards
    for (unsigned k = 0; k < kSpan; ++k) {
        const std::uint8_t want = k < 3 ? pat(k) : k < 3 + kMv ? pat(k - 3) : pat(k);
        const std::uint8_t got = va[k];
        ok = ok && got == want;
        eat(got);
    }
    for (unsigned k = 0; k < kSpan; ++k) va[k] = pat(k);
    std::memmove(a + keep(0u), a + keep(5u), keep(kMv));   // overlap, destination below the source: copied forwards
    for (unsigned k = 0; k < kSpan; ++k) {
        const std::uint8_t want = k < kMv ? pat(k + 5) : pat(k);
        const std::uint8_t got = va[k];
        ok = ok && got == want;
        eat(got);
    }

    for (unsigned off : {1u, 3u}) {
        for (unsigned k = 0; k < 40; ++k) vb[off + k] = static_cast<std::uint8_t>('a' + k % 26);
        vb[off + 40] = 0;
        const std::size_t n = std::strlen(reinterpret_cast<const char*>(b + keep(off)));
        ok = ok && n == 40;
        eat(static_cast<std::uint8_t>(n));
    }
    vb[0] = 0;
    ok = ok && std::strlen(reinterpret_cast<const char*>(b + keep(0u))) == 0;

    touch(o, static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memcpy));
    touch(o, static_cast<void* (*)(void*, const void*, std::size_t)>(&std::memmove));
    touch(o, static_cast<void* (*)(void*, int, std::size_t)>(&std::memset));
    touch(o, static_cast<std::size_t (*)(const char*)>(&std::strlen));
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// fmt::format against a 32-bit reference formatter

// Reference for one integer conversion: digits, then a sign and padding (zero padding puts the sign first).
std::size_t ref_num(char* out, bool neg, std::uint32_t mag, unsigned base, unsigned width, bool zero) {
    char d[12];
    unsigned nd = 0;
    do {
        const unsigned r = mag % base;
        d[nd++] = static_cast<char>(r < 10 ? '0' + r : 'A' + (r - 10));
        mag /= base;
    } while (mag);
    const unsigned len = nd + (neg ? 1u : 0u);
    unsigned pad = width > len ? width - len : 0u;
    std::size_t k = 0;
    if (zero) {
        if (neg) out[k++] = '-';
        while (pad--) out[k++] = '0';
    } else {
        while (pad--) out[k++] = ' ';
        if (neg) out[k++] = '-';
    }
    while (nd) out[k++] = d[--nd];
    out[k] = '\0';
    return k;
}

template <class... A>
bool fmt_is(const char* want, const char* f, A... a) {
    char b[48];
    const std::size_t n = fmt::format(b, f, a...);
    return n == std::strlen(want) && std::strcmp(b, want) == 0;
}

K2_HOT bool check_fmt_line(Out& o) {
    bool ok = true;
    ok = ok && fmt_is("-5", "%d", keep(-5));
    ok = ok && fmt_is("   42|", "%5d|", keep(42));
    ok = ok && fmt_is("00042", "%05d", keep(42));
    ok = ok && fmt_is("-0042", "%05d", keep(-42));
    ok = ok && fmt_is("  -42", "%5d", keep(-42));
    ok = ok && fmt_is("4000000000 BEEF 1F Q%", "%u %x %X %c%%", keep(4000000000u), keep(0xBEEFu), keep(0x1Fu), static_cast<int>('Q'));
    ok = ok && fmt_is("abc|(null)", "%s|%s", "abc", static_cast<const char*>(nullptr));
    ok = ok && fmt_is("DEADBEEF", "%08X", keep(0xDEADBEEFu));
    ok = ok && fmt_is("-2147483648", "%d", keep(-2147483647 - 1));
    {
        char small[8];
        const std::size_t n = fmt::format(small, "%d%d", keep(12345), keep(678901));
        ok = ok && n == 7 && std::strcmp(small, "1234567") == 0;   // cut to size-1, NUL-terminated
    }

    std::uint32_t acc = kFnvBasis;
    char got[64], want[64];
    for (std::uint32_t i = 0; i < 24u; ++i) {
        const int mv = keep(static_cast<int>(-5000 + static_cast<int>(i) * 1237));
        const int ma = keep(static_cast<int>(i) * 313 - 2000);
        const std::uint32_t hex = keep(0x1F2E3Du * (i + 1u));
        const char* const s = (i & 1u) ? "on" : "off";
        const char c = static_cast<char>('a' + i % 26u);
        const std::size_t n = fmt::format(got, "%2u. %5d mV %4d mA %06X %s%c", i, mv, ma, hex, s, c);
        // The reference, piece by piece.
        std::size_t k = ref_num(want, false, i, 10, 2, false);
        want[k++] = '.';
        want[k++] = ' ';
        k += ref_num(want + k, mv < 0, mv < 0 ? 0u - static_cast<std::uint32_t>(mv) : static_cast<std::uint32_t>(mv), 10, 5, false);
        std::memcpy(want + k, " mV ", 4);
        k += 4;
        k += ref_num(want + k, ma < 0, ma < 0 ? 0u - static_cast<std::uint32_t>(ma) : static_cast<std::uint32_t>(ma), 10, 4, false);
        std::memcpy(want + k, " mA ", 4);
        k += 4;
        k += ref_num(want + k, false, hex, 16, 6, true);
        want[k++] = ' ';
        for (const char* p = s; *p; ++p) want[k++] = *p;
        want[k++] = c;
        want[k] = '\0';
        ok = ok && n == k && std::strcmp(got, want) == 0;
        acc = fnv_step(acc, static_cast<std::uint32_t>(n));
        acc = fnv_str(acc, got);
    }
    touch(o, &fmt::format);
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// PD codec: Source_Capabilities decode, PDO / RDO decode, VDM and e-marker parsing

constexpr std::uint32_t kPdoFixed5 = (100u << 10) | 300u | (1u << 23);      // 5 V 3 A, EPR mode capable
constexpr std::uint32_t kPdoFixed9 = (180u << 10) | 300u;                   // 9 V 3 A
constexpr std::uint32_t kPdoFixed15 = (300u << 10) | 300u;                  // 15 V 3 A
constexpr std::uint32_t kPdoFixed20 = (400u << 10) | 500u;                  // 20 V 5 A
constexpr std::uint32_t kPdoPps = (3u << 30) | (210u << 17) | (33u << 8) | 100u;   // 3.3..21 V 5 A
constexpr std::uint32_t kPdoEprAvs = (3u << 30) | (1u << 28) | (300u << 17) | (150u << 8) | 140u;   // 15..30 V, 140 W
constexpr std::uint32_t kPdoSprAvs15 = (3u << 30) | (2u << 28) | (300u << 10);
constexpr std::uint32_t kPdoSprAvs20 = (3u << 30) | (2u << 28) | (300u << 10) | 500u;
constexpr std::uint32_t kFivePdos[5] = {kPdoFixed5, kPdoFixed9, kPdoFixed15, kPdoFixed20, kPdoPps};

inline void put32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

std::uint16_t caps_header(unsigned n, unsigned id) {
    return static_cast<std::uint16_t>(1u | (2u << 6) | (n << 12) | ((id & 7u) << 9));
}

// SourceCaps built from the first n of kFivePdos through the production decoder.
void make_caps(pd::SourceCaps& caps, unsigned n, unsigned id) {
    std::uint8_t payload[28];
    for (unsigned i = 0; i < n; ++i) put32(payload + 4 * i, kFivePdos[i]);
    pd::decode_source_caps(caps, caps_header(n, id), std::span<const std::uint8_t>(payload, 4u * n), 1);
}

K2_HOT bool check_pd_codec(Out& o) {
    bool ok = true;
    std::uint32_t acc = kFnvBasis;

    // Source_Capabilities with 1..5 PDOs, reusing one SourceCaps (rx_count counts, the unused slots are cleared).
    pd::SourceCaps caps;
    for (unsigned i = 0; i < 24u; ++i) {
        const unsigned n = 1u + keep(i) % 5u;
        make_caps(caps, n, keep(i));
        ok = ok && caps.valid && caps.num_pdos == n && caps.rx_count == i + 1u && caps.spec_rev == 2 && caps.active_cc == 1;
        for (unsigned k = 0; k < n; ++k) {
            const pd::Pdo& p = caps.pdos[k];
            const std::uint32_t raw = kFivePdos[k];
            ok = ok && p.raw == raw;
            if (k < 4) {
                ok = ok && p.type == pd::PdoType::Fixed && p.max_mv == ((raw >> 10) & 0x3FFu) * 50u && p.max_ma == (raw & 0x3FFu) * 10u;
            } else {
                ok = ok && p.type == pd::PdoType::Pps && p.max_mv == ((raw >> 17) & 0xFFu) * 100u &&
                     p.min_mv == ((raw >> 8) & 0xFFu) * 100u && p.max_ma == (raw & 0x7Fu) * 50u;
            }
            acc = fnv_step(acc, p.max_mv);
            acc = fnv_step(acc, p.max_ma);
            acc = fnv_step(acc, p.min_mv);
        }
        ok = ok && (n == pd::kMaxPdos || caps.pdos[n].raw == 0);
    }

    // PDO flavours the five above do not cover.
    const pd::Pdo epr = pd::decode_pdo(keep(kPdoEprAvs));
    ok = ok && epr.type == pd::PdoType::Avs && epr.max_mv == 30000 && epr.min_mv == 15000 && epr.max_ma == 4666;
    const pd::Pdo avs15 = pd::decode_pdo(keep(kPdoSprAvs15)), avs20 = pd::decode_pdo(keep(kPdoSprAvs20));
    ok = ok && avs15.type == pd::PdoType::Avs && avs15.max_mv == 15000 && avs15.min_mv == 9000 && avs15.max_ma == 3000;
    ok = ok && avs20.type == pd::PdoType::Avs && avs20.max_mv == 20000 && avs20.max_ma == 5000;
    const pd::Pdo f20 = pd::decode_pdo(kPdoFixed20);
    ok = ok && pd::current_limit_ma(f20, keep<std::uint16_t>(12000)) == 5000;
    acc = fnv_step(acc, epr.max_ma);
    acc = fnv_step(acc, avs15.max_mv);
    acc = fnv_step(acc, avs20.max_ma);
    acc = fnv_step(acc, pd::current_limit_ma(avs15, keep<std::uint16_t>(12000)));
    acc = fnv_step(acc, pd::current_limit_ma(avs20, keep<std::uint16_t>(18000)));

    // RDO round trips through the encoder and the decoder.
    {
        const pd::Pdo f15 = pd::decode_pdo(kPdoFixed15), pps = pd::decode_pdo(kPdoPps);
        const std::uint32_t r1 = pd::rdo_fixed(3, keep<std::uint16_t>(3000), 3000, true);
        const pd::Rdo d1 = pd::decode_rdo(r1, &f15);
        ok = ok && d1.pos == 3 && d1.kind == pd::PdoType::Fixed && d1.mv == 15000 && d1.op_ma == 3000 && d1.max_ma == 3000 &&
             (d1.flags & pd::kRdoFEpr) != 0;
        const std::uint32_t r2 = pd::rdo_pps(5, keep<std::uint16_t>(9000), 2000, false);
        const pd::Rdo d2 = pd::decode_rdo(r2, &pps);
        ok = ok && d2.pos == 5 && d2.kind == pd::PdoType::Pps && d2.mv == 9000 && d2.op_ma == 2000 && (d2.flags & pd::kRdoFEpr) == 0;
        acc = fnv_step(fnv_step(acc, r1), r2);
    }

    // Vendor_Defined messages: Discover Identity, SVIDs and Modes ACKs.
    {
        constexpr std::uint32_t kAck = (1u << 15) | (1u << 6);
        pd::VdmInfo info;
        std::uint8_t m[16];
        put32(m, (0xFF00u << 16) | kAck | 1u);
        put32(m + 4, 0x05ACu);
        put32(m + 8, 0u);
        put32(m + 12, (static_cast<std::uint32_t>(keep<std::uint16_t>(0x7517)) << 16) | 0x0001u);
        pd::parse_vdm(info, std::span<const std::uint8_t>(m, 16));
        ok = ok && info.identity.valid && info.identity.vid == 0x05AC && info.identity.pid == 0x7517 &&
             info.identity.model != nullptr && info.identity.model == pd::apple_model(0x7517);
        std::uint8_t sv[12];
        put32(sv, (0xFF00u << 16) | kAck | 2u);
        put32(sv + 4, (0x05ACu << 16) | 0x2E87u);
        put32(sv + 8, 0xFF01u << 16);
        pd::parse_vdm(info, std::span<const std::uint8_t>(sv, 12));
        ok = ok && info.num_svids == 3 && info.svids[0] == 0x05AC && info.svids[1] == 0x2E87 && info.svids[2] == 0xFF01;
        std::uint8_t md[8];
        put32(md, (0x2E87u << 16) | kAck | 3u);
        put32(md + 4, 0x42u);
        pd::parse_vdm(info, std::span<const std::uint8_t>(md, 8));
        ok = ok && info.modes_svid == 0x2E87 && info.num_modes == 1 && info.modes[0] == 0x42;
        acc = fnv_step(acc, info.identity.vid);
        acc = fnv_step(acc, info.identity.pid);
        acc = fnv_step(acc, info.num_svids);
        acc = fnv_step(acc, info.modes[0]);
    }

    // An e-marker: a 5 A / 50 V passive cable.
    {
        std::uint8_t b[20];
        put32(b, (0xFF00u << 16) | (1u << 15) | (1u << 6) | 1u);
        put32(b + 4, (3u << 27) | 0x2E87u);
        put32(b + 8, 0u);
        put32(b + 12, 0u);
        put32(b + 16, 0x640u | (2u << 18) | (1u << 13) | (1u << 17));
        pd::Emarker e = pd::parse_emarker(std::span<const std::uint8_t>(b, 20), keep(5u));
        ok = ok && e.valid && e.max_current_a == 5 && e.max_voltage_v == 50 && e.epr_capable && e.max_power_w == 240;
        e.spec_rev = 2;
        const pd::CableLimits lim = pd::cable_limits(e);
        ok = ok && lim.observed && lim.max_ma == 5000 && lim.max_mv == 50000;
        acc = fnv_step(acc, e.max_power_w);
        acc = fnv_step(acc, lim.max_mv);
    }

    touch(o, &pd::decode_source_caps);
    touch(o, &pd::decode_pdo);
    touch(o, &pd::decode_rdo);
    touch(o, &pd::parse_vdm);
    o.crc = acc;
    return ok;
}

// PD sink planning: Request decisions over a SourceCaps, RDO encoders, lookup and the Sink Capabilities Extended block.
K2_HOT bool check_pd_sink(Out& o) {
    bool ok = true;
    std::uint32_t acc = kFnvBasis;

    pd::SourceCaps caps;   // the five PDOs, then an EPR AVS PDO at position 8
    make_caps(caps, 5, 1);
    caps.pdos[7] = pd::decode_pdo(kPdoEprAvs);
    caps.num_pdos = 8;
    pd::SourceCaps none;   // no caps: nothing to plan

    struct Row {
        std::uint8_t pos;
        std::uint16_t mv, ma;
        bool hunt;
        std::uint32_t cap;
        pd::Err err;
        std::uint16_t want_mv, want_ma;   // 0 = not checked
    };
    static constexpr Row kRows[] = {
        {1, 0, 0, false, 20000, pd::Err::Ok, 5000, 3000},
        {3, 0, 2000, false, 20000, pd::Err::Ok, 15000, 2000},
        {3, 0, 0, false, 20000, pd::Err::Ok, 15000, 3000},
        {4, 0, 0, false, 9000, pd::Err::OverCap, 0, 0},
        {4, 0, 0, false, 20000, pd::Err::Ok, 20000, 5000},
        {4, 0, 6000, true, 20000, pd::Err::Ok, 20000, 6000},
        {4, 0, 12000, true, 20000, pd::Err::Ok, 20000, 10230},
        {5, 5000, 2000, false, 20000, pd::Err::Ok, 5000, 2000},
        {5, 30000, 0, false, 36000, pd::Err::Ok, 21000, 5000},
        {5, 1000, 500, false, 36000, pd::Err::Ok, 3300, 500},
        {5, 0, 0, false, 12000, pd::Err::Ok, 12000, 5000},
        {6, 0, 0, false, 36000, pd::Err::BadPos, 0, 0},
        {0, 0, 0, false, 36000, pd::Err::BadPos, 0, 0},
        {9, 0, 0, false, 36000, pd::Err::BadPos, 0, 0},
        {8, 24000, 0, false, 36000, pd::Err::Ok, 24000, 5000},
        {8, 29950, 0, false, 36000, pd::Err::Ok, 29900, 0},
        {8, 5000, 0, false, 36000, pd::Err::Ok, 15000, 0},
    };
    for (const Row& r : kRows) {
        pd::Choice c;
        c.pos = r.pos;
        c.mv = keep(r.mv);
        c.ma = r.ma;
        c.hunt = r.hunt;
        const pd::Plan p = pd::plan_request(caps, c, keep(r.cap), false);
        ok = ok && p.err == r.err;
        if (r.err == pd::Err::Ok) {
            ok = ok && p.mv != 0 && p.ma != 0 && p.pos == r.pos && (p.rdo >> 28) == r.pos;
            ok = ok && (r.want_mv == 0 || p.mv == r.want_mv) && (r.want_ma == 0 || p.ma == r.want_ma);
            ok = ok && p.needs_epr == (r.pos >= 8) && (p.rdo & (1u << 22)) != 0;   // caps1 offers EPR: every RDO says so
            const pd::Pdo* pdo = &caps.pdos[r.pos - 1];
            const pd::Rdo d = pd::decode_rdo(p.rdo, pdo);
            ok = ok && d.pos == r.pos && d.mv == p.mv;
        }
        acc = fnv_step(acc, static_cast<std::uint32_t>(p.err));
        acc = fnv_step(acc, p.mv);
        acc = fnv_step(acc, p.ma);
        acc = fnv_step(acc, p.rdo);
    }
    {
        pd::Choice c;
        c.pos = 3;
        c.mismatch = true;
        const pd::Plan p = pd::plan_request(caps, c, 20000, false);
        ok = ok && p.err == pd::Err::Ok && (p.rdo & (1u << 26)) != 0;
        pd::Choice z;
        z.pos = 1;
        ok = ok && pd::plan_request(none, z, 20000, false).err == pd::Err::NoCaps;
        acc = fnv_step(acc, p.rdo);
    }

    // The held-level lookup and the monitor cap.
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(15000), 3000, pd::PdoType::Fixed).pos == 3;
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(12000), 3000, pd::PdoType::Fixed).pos == 0;
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(9000), 3000, pd::PdoType::Pps).pos == 5;
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(25000), 3000, pd::PdoType::Pps).pos == 0;
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(20000), 3000, pd::PdoType::Avs).pos == 8;
    ok = ok && pd::lookup_held(caps, keep<std::uint16_t>(40000), 3000, pd::PdoType::Avs).pos == 0;
    ok = ok && pd::request_cap_mv(keep(20000), 36000) == 20000 && pd::request_cap_mv(keep(0), 36000) == 5000;
    ok = ok && pd::request_cap_mv(keep(40000), 36000) == 36000 && pd::request_cap_mv(keep(3000), 36000) == 5000;

    // The RDO encoders against the bit layout, over a sweep.
    for (unsigned k = 0; k < 24u; ++k) {
        const std::uint16_t mv = static_cast<std::uint16_t>(3300u + keep(k) * 600u);
        const std::uint16_t ma = static_cast<std::uint16_t>(500u + (k % 9u) * 500u);
        const std::uint32_t rf = pd::rdo_fixed(static_cast<std::uint8_t>(1 + k % 7), ma, ma, (k & 1u) != 0);
        ok = ok && (rf >> 28) == 1 + k % 7 && ((rf >> 10) & 0x3FFu) == ma / 10u && (rf & 0x3FFu) == ma / 10u &&
             ((rf >> 22) & 1u) == (k & 1u);
        const std::uint32_t rp = pd::rdo_pps(5, mv, ma, false);
        ok = ok && ((rp >> 9) & 0xFFFu) == mv / 20u && (rp & 0x7Fu) == ma / 50u;
        const std::uint16_t avs_mv = static_cast<std::uint16_t>(15000u + (k % 16u) * 900u);
        const std::uint32_t ra = pd::rdo_avs(8, avs_mv, ma);
        ok = ok && ((ra >> 9) & 0xFFFu) == (avs_mv / 100u) * 4u && (ra & 0x7Fu) == ma / 50u;
        acc = fnv_step(fnv_step(fnv_step(acc, rf), rp), ra);
    }

    // The Sink Capabilities Extended block the sink answers a PD 3.2 charger with.
    {
        const std::array<std::uint8_t, 24> s = pd::make_skedb();
        std::array<std::uint8_t, 24> want{};
        want[0] = 0x16; want[1] = 0x07; want[2] = 0x60; want[3] = 0x50;
        want[8] = 1; want[9] = 1; want[10] = 1; want[17] = 0x23; want[18] = 1; want[19] = 15; want[20] = 33;
        ok = ok && s == want;
        for (const std::uint8_t byte : s) acc = fnv_step(acc, byte);
    }

    touch(o, &pd::plan_request);   // not K2_HOT: zw may be 0 (side issue)
    touch(o, &pd::lookup_held);
    touch(o, &pd::rdo_fixed);
    touch(o, &pd::rdo_pps);
    touch(o, &pd::rdo_avs);
    touch(o, &pd::make_skedb);
    o.crc = acc;
    return ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// PD trace ring and collapse on a local ring (the live black box is not touched)

K2_HOT bool check_pdtrace_ring(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    struct Trace {
        pdtrace::Ring<1024> ring;
        pdtrace::Collapse<1024> col;
    };
    Trace& t = scratch::make<Trace>();
    bool ok = true;
    std::uint32_t acc = kFnvBasis;
    using Span = std::span<const std::uint8_t>;

    // Wrap: 150 ten-byte records into 1024 bytes keep the newest 102, in order, nothing torn.
    for (std::uint32_t i = 0; i < 150u; ++i) {
        const std::uint32_t ts = 5000u + i * 250u;
        std::uint8_t pl[4];
        put32(pl, ts * 3u + 1u);
        ok = ok && t.ring.push(pdtrace::Type::Vbus, ts, Span(pl, 4));
    }
    {
        const std::uint32_t resident = (t.ring.head() - t.ring.tail()) / 10u;
        ok = ok && (t.ring.head() - t.ring.tail()) % 10u == 0 && resident == 102 &&
             t.ring.records_pushed() - t.ring.records_dropped() == resident;
        std::uint32_t cursor = t.ring.tail();
        std::uint32_t expect_i = 150u - resident;
        while (cursor != t.ring.head()) {
            std::uint8_t rec[10];
            const auto r = t.ring.read(cursor, std::span<std::uint8_t>(rec, 10));
            const std::uint32_t ts = rec[2] | rec[3] << 8 | rec[4] << 16 | static_cast<std::uint32_t>(rec[5]) << 24;
            const std::uint32_t pl = rec[6] | rec[7] << 8 | rec[8] << 16 | static_cast<std::uint32_t>(rec[9]) << 24;
            ok = ok && r.bytes == 10 && rec[0] == 10 && rec[1] == static_cast<std::uint8_t>(pdtrace::Type::Vbus) &&
                 ts == 5000u + expect_i * 250u && pl == ts * 3u + 1u;
            acc = fnv_step(fnv_step(acc, ts), pl);
            cursor = r.next;
            ++expect_i;
            if (r.bytes == 0) { ok = false; break; }
        }
        ok = ok && expect_i == 150u;
    }

    // Collapse: six identical Source_Capabilities rounds fold into one record and one RxRepeat.
    t.ring.clear();
    auto wr = [&](pdtrace::Type ty, std::uint32_t ts, Span p) { return t.ring.push(ty, ts, p); };
    for (unsigned r = 0; r < 6u; ++r) {
        const std::uint32_t ts = 100000u + keep(r) * 1000u;
        const std::uint8_t poll[4] = {1, 2, 3, 4};
        t.col.record(t.ring, pdtrace::Type::Poll, ts, Span(poll, 4), wr);
        std::uint8_t rx[11];
        const unsigned h = 1u | (2u << 6) | (1u << 12) | (r << 9);
        rx[0] = 0xE0;
        rx[1] = static_cast<std::uint8_t>(h);
        rx[2] = static_cast<std::uint8_t>(h >> 8);
        put32(rx + 3, kPdoFixed5);
        put32(rx + 7, 0xC0DE0000u + r * 0x01010101u);   // the CRC differs every round and is ignored
        t.col.record(t.ring, pdtrace::Type::Rx, ts + 10u, Span(rx, 11), wr);
        const std::uint8_t irq[3] = {0, 0, 0};
        t.col.record(t.ring, pdtrace::Type::Irq, ts + 20u, Span(irq, 3), wr);
        const std::uint8_t mark[5] = {pdtrace::kMarkCaps, 1, 0, 3, 0};
        t.col.record(t.ring, pdtrace::Type::Mark, ts + 30u, Span(mark, 5), wr);
    }
    const std::uint8_t vb[4] = {9, 9, 9, 9};
    t.col.record(t.ring, pdtrace::Type::Vbus, 200000u, Span(vb, 4), wr);
    const std::uint32_t head_before = t.ring.head();
    t.col.flush(wr);
    ok = ok && t.ring.head() == head_before;   // nothing was held back any more
    {
        static constexpr pdtrace::Type kWant[] = {pdtrace::Type::Poll, pdtrace::Type::Rx, pdtrace::Type::Irq,
                                                  pdtrace::Type::Mark, pdtrace::Type::RxRepeat, pdtrace::Type::Vbus};
        static constexpr std::uint8_t kLen[] = {10, 17, 9, 11, 10, 10};
        std::uint32_t cursor = t.ring.tail();
        std::size_t idx = 0;
        while (cursor != t.ring.head() && idx < 8) {
            std::uint8_t rec[41];
            const auto r = t.ring.read(cursor, std::span<std::uint8_t>(rec, sizeof rec));
            ok = ok && r.bytes != 0 && idx < 6 && rec[0] == kLen[idx] && rec[1] == static_cast<std::uint8_t>(kWant[idx]);
            if (kWant[idx % 6] == pdtrace::Type::RxRepeat) ok = ok && rec[6] == 5 && rec[7] == 0;   // five folded frames
            for (std::size_t k = 0; k < rec[0]; ++k) acc = fnv_step(acc, rec[k]);
            if (r.bytes == 0) { ok = false; break; }
            cursor += rec[0];
            ++idx;
        }
        ok = ok && idx == 6 && t.ring.head() - t.ring.tail() == 67;
        ok = ok && t.ring.head() - t.ring.cursor_for_last(30) == 20;
    }
    o.crc = acc;
    return ok;
}

#if K2_SELFTEST_TUFIFO
// TinyUSB's FIFO (the HID log / CDC path): config, wrap-around write and read, overflow refused.
K2_HOT bool check_tu_fifo(Out& o) {
    if (scratch::held()) return false;
    const scratch::Lease lease("selftest");
    struct Buf {
        tu_fifo_t f;
        std::uint8_t store[256];
        std::uint8_t in[300];
        std::uint8_t out[300];
    };
    Buf& b = scratch::as<Buf>();
    auto pat = [](unsigned k) { return static_cast<std::uint8_t>(k * 37u + 11u); };
    for (unsigned k = 0; k < 300u; ++k) {
        b.in[k] = pat(k);
        b.out[k] = 0;
    }
    bool ok = true;
    std::uint32_t acc = kFnvBasis;
    tu_fifo_config(&b.f, b.store, 256, false);
    ok = ok && tu_fifo_write_n(&b.f, b.in, keep<std::uint16_t>(200)) == 200;
    ok = ok && tu_fifo_read_n(&b.f, b.out, keep<std::uint16_t>(80)) == 80;
    ok = ok && tu_fifo_write_n(&b.f, b.in + 200, 100) == 100;   // wraps the storage
    ok = ok && tu_fifo_count(&b.f) == 220;
    ok = ok && tu_fifo_write_n(&b.f, b.in + 100, 100) == 36;   // only the room left is taken
    ok = ok && tu_fifo_count(&b.f) == 256 && tu_fifo_remaining(&b.f) == 0;
    ok = ok && tu_fifo_read_n(&b.f, b.out + 80, 256) == 256;
    for (unsigned j = 0; j < 256u; ++j) {
        const std::uint8_t want = j < 120 ? pat(80 + j) : j < 220 ? pat(200 + (j - 120)) : pat(100 + (j - 220));
        ok = ok && b.out[80 + j] == want;
        acc = fnv_step(acc, b.out[80 + j]);
    }
    ok = ok && tu_fifo_empty(&b.f) && tu_fifo_count(&b.f) == 0;
    touch(o, &tu_fifo_write_n_access_mode);
    touch(o, &tu_fifo_read_n_access_mode);
    o.crc = acc;
    return ok;
}
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Placement canary: the same integer loop run from the zero-wait window and from the far end of flash (link.ld puts
// .cold_probe past 0x08040000). Identical sums; the cycle counts show what the wait states cost.

[[gnu::always_inline]] inline std::uint32_t ws_body(std::uint32_t h, std::uint32_t n) {
#pragma GCC unroll 16
    for (std::uint32_t i = 0; i < n; ++i) {
        h = h * 1664525u + 1013904223u;
        h ^= h >> 13;
        h += i;
    }
    return h;
}

K2_HOT [[gnu::noinline]] std::uint32_t ws_fast_fn(std::uint32_t n) { return ws_body(0xC0FFEEu, n); }
[[gnu::noinline]] K2_COLD_PROBE std::uint32_t ws_slow_fn(std::uint32_t n) { return ws_body(0xC0FFEEu, n); }

bool placed_ok(bool fast, std::uintptr_t addr) {
#if defined(__arm__)
    return fast ? addr < kZeroWaitEnd : addr >= kZeroWaitEnd;
#else
    (void)fast;
    (void)addr;
    return true;
#endif
}

K2_HOT bool check_ws_fast(Out& o) {
    o.crc = ws_fast_fn(keep(4096u));
    touch(o, &ws_fast_fn);
    return placed_ok(true, reinterpret_cast<std::uintptr_t>(&ws_fast_fn));
}

K2_HOT bool check_ws_slow(Out& o) {
    o.crc = ws_slow_fn(keep(4096u));
    touch(o, &ws_slow_fn);
    return placed_ok(false, reinterpret_cast<std::uintptr_t>(&ws_slow_fn));
}

}  // namespace

K2_HOT bool run_math(WriteFn write, void* ctx, Summary* out) {
    enable_dwt();

    // gold: the expected sum of a deterministic check; tol: a float check, judged by its own tolerance.
    struct Case {
        const char* name;
        bool (*fn)(Out&);
        std::uint32_t gold;
        bool tol;
    };
    static constexpr Case kCases[] = {
        {"scale_div", check_scale_div, 0x7EF9EC51u, false},
        {"adc_calc", check_adc, 0xEE89B01Du, false},
        {"power_calc", check_power, 0xB2F07723u, false},
        {"math_opt", check_math_opt, 0x74B742B7u, false},
        {"readout_fmt", check_readout_fmt, 0x2E7FF5D7u, false},
        {"fpu_sqrt", check_fpu_sqrt, 0u, true},    // float: tolerance
        {"one_euro", check_one_euro, 0u, true},    // float: tolerance
        {"blend565", check_blend565, 0x2B0E04E3u, false},
        {"text_aa", check_text_aa, 0x447FE66Eu, false},
        {"text_big", check_text_big, 0x3C2A04BFu, false},
        {"gfx_fill", check_gfx_fill, 0x041E7430u, false},
        {"mem_ops", check_mem_ops, 0x8B0882C5u, false},
        {"fmt_line", check_fmt_line, 0x0F171019u, false},
        {"pd_codec", check_pd_codec, 0xDBF8DC4Du, false},
        {"pd_sink", check_pd_sink, 0x5FEDCFA6u, false},
        {"pdtrace_ring", check_pdtrace_ring, 0xD2BDBEB5u, false},
#if K2_SELFTEST_TUFIFO
        {"tu_fifo", check_tu_fifo, 0x100A6715u, false},   // device only: computed from the pattern, not run on the host
#endif
        {"ws_fast", check_ws_fast, 0x7B1CE946u, false},
        {"ws_slow", check_ws_slow, 0x7B1CE946u, false},
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
        Out res;
        res.top = reinterpret_cast<std::uintptr_t>(c.fn);
#if defined(__arm__)
        std::uint32_t primask;
        __asm volatile("mrs %0, primask\n cpsid i" : "=r"(primask)::"memory");
#endif
        const std::uint32_t t0 = read_cycles();
        const bool ran = c.fn(res);
        const std::uint32_t cyc = read_cycles() - t0;
#if defined(__arm__)
        __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
#endif
        total_cycles += cyc;
        const bool ok = ran && (c.tol || res.crc == c.gold);
        if (ok) ++passed;
        char name_pad[13];
        const std::size_t nl = std::strlen(c.name);
        for (std::size_t i = 0; i < 12u; ++i) name_pad[i] = i < nl ? c.name[i] : ' ';
        name_pad[12] = '\0';
        const unsigned zw = (res.top != 0 && res.top < kZeroWaitEnd) ? 1u : 0u;
        emit(fmt::format(line, "  [%s] %s sum=0x%08X  cyc=%u zw=%u\r\n", ok ? "PASS" : "FAIL", name_pad, res.crc, cyc, zw));
        if (ran && !c.tol && res.crc != c.gold) emit(fmt::format(line, "         want=0x%08X\r\n", c.gold));
    }

    constexpr std::uint8_t kTotal = static_cast<std::uint8_t>(sizeof(kCases) / sizeof(kCases[0]));
    emit(fmt::format(line, "%s %u/%u checks, total_cyc=%u\r\n",
                     passed == kTotal ? "PASS" : "FAIL", unsigned(passed), unsigned(kTotal), total_cycles));

    if (out) *out = {passed, kTotal, total_cycles};
    return passed == kTotal;
}

}  // namespace k2::selftest
