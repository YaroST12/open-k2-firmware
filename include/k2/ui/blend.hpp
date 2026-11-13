// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Anti-aliased glyph blending in RGB565 with 4-bit alpha (0 = background, 15 = foreground).
#pragma once
#include <cstdint>

#include "k2/core/hot.h"

namespace k2 {

K2_HOT constexpr uint16_t blend565(uint16_t fg, uint16_t bg, unsigned a) {
    auto mix = [a](unsigned f, unsigned b) {
        const unsigned x = f * a + b * (15u - a) + 7u;
        return (x * 4369u + 4368u) >> 16;   // exact x / 15 for all 0..999 without UDIV
    };
    unsigned r = mix(fg >> 11, bg >> 11);
    unsigned g = mix((fg >> 5) & 0x3F, (bg >> 5) & 0x3F);
    unsigned b = mix(fg & 0x1F, bg & 0x1F);
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

K2_HOT constexpr void make_blend_lut(uint16_t fg, uint16_t bg, uint16_t (&lut)[16]) {
    for (unsigned a = 0; a < 16; a++) lut[a] = blend565(fg, bg, a);
}

}  // namespace k2
