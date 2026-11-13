// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2/core/math_opt.hpp: the 64-bit-by-32-bit helpers that keep libgcc's 64-bit divide out of the image. The runtime paths
// (not the `if consteval` ones) are what the firmware runs, so these are called with runtime values and compared with a
// reference done in unsigned 64-bit arithmetic only.
#include <cstdint>
#include <limits>

#include "k2/core/math_opt.hpp"
#include "test_util.h"

using namespace k2;

namespace {

// The value the helper must return for a 64-bit magnitude: the quotient's low 32 bits, negated modulo 2^32 for a negative
// numerator. Never negates a signed value, so it is defined for INT64_MIN too.
std::int32_t ref_i64_to_i32(std::int64_t num, std::int32_t den) {
    const bool neg = num < 0;
    const std::uint64_t mag = neg ? 0 - static_cast<std::uint64_t>(num) : static_cast<std::uint64_t>(num);
    const std::uint32_t q = static_cast<std::uint32_t>(mag / static_cast<std::uint32_t>(den));
    return neg ? static_cast<std::int32_t>(0u - q) : static_cast<std::int32_t>(q);
}

// Runtime values on purpose (volatile): the helper must take its runtime branch, as in the firmware.
void test_div_i64_to_i32_extremes() {
    volatile std::int64_t mins[] = {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::min() + 1,
                                    std::numeric_limits<std::int64_t>::max(), -(std::int64_t{1} << 40), std::int64_t{1} << 40,
                                    -5'000'000'000LL, 5'000'000'000LL};
    const std::int32_t dens[] = {1, 2, 7, 1000, 65535, std::numeric_limits<std::int32_t>::max()};
    for (const std::int64_t n : mins)
        for (const std::int32_t d : dens) CHECK_EQ(math::div_i64_to_i32(n, d), ref_i64_to_i32(n, d));
}

void test_div_i64_to_i32_small_and_bad_den() {
    volatile std::int64_t n = -7;
    CHECK_EQ(math::div_i64_to_i32(n, 2), -3);   // truncates toward zero
    n = 7;
    CHECK_EQ(math::div_i64_to_i32(n, 2), 3);
    CHECK_EQ(math::div_i64_to_i32(n, 0), 0);    // a non-positive denominator is refused with 0
    CHECK_EQ(math::div_i64_to_i32(n, -3), 0);
}

void test_unsigned_helpers() {
    volatile std::uint64_t big = 0x123456789ABCDEF0ull;
    CHECK_EQ(math::div_u64_u32(big, 1000u), big / 1000u);
    CHECK_EQ(math::div_u64_u32(big, 0u), 0);
    CHECK_EQ(math::mul_div_u32(4000000000u, 3u, 7u), static_cast<std::uint32_t>(std::uint64_t{4000000000u} * 3u / 7u));
    CHECK_EQ(math::mw_of(20000u, 3000u), 60000u);
}

}  // namespace

int main() {
    test_div_i64_to_i32_extremes();
    test_div_i64_to_i32_small_and_bad_den();
    test_unsigned_helpers();
    return test_report("test_math_opt");
}
