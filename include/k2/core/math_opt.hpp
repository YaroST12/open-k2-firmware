// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Cortex-M4F 32-bit math fast-paths that avoid pulling libgcc 64-bit division routines
// (_udivmoddi4 / __aeabi_uldivmod / __aeabi_ldivmod) while keeping exact 64-bit range on overflow.
#pragma once
#include <cstdint>

namespace k2::math {

#if defined(__GNUC__) || defined(__clang__)
[[gnu::noinline]]
#endif
inline std::uint64_t div_u64_u32_slow(std::uint64_t num, std::uint32_t den) {
    if (den == 0) return 0;
    if (num < den) return 0;
    if (num <= 0xFFFFFFFFull) return static_cast<std::uint32_t>(num) / den;
    if (den == 1) return num;
#if defined(__GNUC__) || defined(__clang__)
    const int shift = __builtin_clzll(den) - __builtin_clzll(num);
#else
    int shift = 0;
    std::uint64_t probe = den;
    while ((probe << 1) <= num && shift < 63) {
        probe <<= 1;
        ++shift;
    }
#endif
    std::uint64_t q = 0;
    std::uint64_t div = std::uint64_t{den} << shift;
    for (int i = shift; i >= 0; --i) {
        q <<= 1;
        if (num >= div) {
            num -= div;
            q |= 1u;
        }
        div >>= 1;
    }
    return q;
}

constexpr std::uint64_t div_u64_u32(std::uint64_t num, std::uint32_t den) {
    if (den == 0) return 0;
    if consteval {
        return num / den;
    }
    if (num <= 0xFFFFFFFFull) return static_cast<std::uint32_t>(num) / den;
    return div_u64_u32_slow(num, den);
}

constexpr std::uint32_t div_u64_to_u32(std::uint64_t num, std::uint32_t den) {
    return static_cast<std::uint32_t>(div_u64_u32(num, den));
}

constexpr std::uint32_t mul_div_u32(std::uint32_t a, std::uint32_t b, std::uint32_t den) {
    return div_u64_to_u32(std::uint64_t{a} * b, den);
}

constexpr std::int32_t div_i64_to_i32(std::int64_t num, std::int32_t den) {
    if (den <= 0) return 0;
    if consteval {
        return static_cast<std::int32_t>(num / den);
    }
    if (num >= INT32_MIN && num <= INT32_MAX) return static_cast<std::int32_t>(num) / den;
    const bool neg = num < 0;
    const std::uint64_t mag = neg ? static_cast<std::uint64_t>(-num) : static_cast<std::uint64_t>(num);
    const std::uint32_t q = static_cast<std::uint32_t>(div_u64_u32_slow(mag, static_cast<std::uint32_t>(den)));
    return neg ? static_cast<std::int32_t>(0u - q) : static_cast<std::int32_t>(q);
}

// Truncating toward zero (matches C++ `int64_t(a) * b / den` for positive den).
constexpr std::int32_t mul_div_i32(std::int32_t a, std::int32_t b, std::int32_t den) {
    return div_i64_to_i32(std::int64_t{a} * b, den);
}

constexpr std::uint32_t mw_of(std::uint32_t mv, std::uint32_t ma) {
    return mul_div_u32(mv, ma, 1000u);
}

}  // namespace k2::math
