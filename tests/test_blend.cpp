// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the glyph alpha blend (RGB565, 4-bit alpha).
#include "test_util.h"
#include "k2/ui/blend.hpp"

using namespace k2;

static void test_endpoints() {
    CHECK_EQ(blend565(0xFFE0, 0x0000, 0), 0x0000);   // alpha 0 = background
    CHECK_EQ(blend565(0xFFE0, 0x0000, 15), 0xFFE0);  // alpha 15 = foreground
    CHECK_EQ(blend565(0x07FF, 0x1082, 0), 0x1082);
    CHECK_EQ(blend565(0x07FF, 0x1082, 15), 0x07FF);
}

static void test_midpoints() {
    // White over black at 8/15: R 31*8/15 = 16.5 -> 17, G 63*8/15 = 33.6 -> 34, B 17.
    CHECK_EQ(blend565(0xFFFF, 0x0000, 8), (17 << 11) | (34 << 5) | 17);
    // Black over white at 8/15 leaves 7/15 of white: R 14.5 -> 14, G 29.4 -> 29, B 14.
    CHECK_EQ(blend565(0x0000, 0xFFFF, 8), (14 << 11) | (29 << 5) | 14);
    // Pure red text over a pure blue background keeps channels independent.
    uint16_t m = blend565(0xF800, 0x001F, 5);        // R 31*5/15 = 10.3 -> 10, B 31*10/15 = 20.7 -> 21
    CHECK_EQ(m >> 11, 10);
    CHECK_EQ(m & 0x1F, 21);
    CHECK_EQ((m >> 5) & 0x3F, 0);
}

static void test_monotonic_and_lut() {
    uint16_t lut[16];
    make_blend_lut(0xFFFF, 0x0000, lut);
    CHECK_EQ(lut[0], 0x0000);
    CHECK_EQ(lut[15], 0xFFFF);
    for (int a = 1; a < 16; a++) {
        CHECK_EQ(lut[a] >= lut[a - 1], 1);
        CHECK_EQ(lut[a], blend565(0xFFFF, 0x0000, a));
    }
}

int main() {
    test_endpoints();
    test_midpoints();
    test_monotonic_and_lut();
    return test_report("test_blend");
}
