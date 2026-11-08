// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure LCD link timing and DMA chunk planning (include/k2/drivers/lcd_timing.hpp). The expected values
// are worked out by hand from the wire: a pixel is one 16-bit SPI frame, so it takes
// 16 * divider / 120 MHz (APB1), e.g. 1.067 us at DIV8.
#include "k2/drivers/lcd_timing.hpp"
#include "test_util.h"
#include <cstdio>
#include <initializer_list>

using namespace k2::lcd;

static void test_wire_time() {
    // 240x240 at the shipped DIV8: 57600 px * 16 bit * 8 / 120 MHz = 61.44 ms (the perf notes quoted ~123 ms
    // from the old 60 MHz assumption).
    CHECK_EQ(wire_us(57600, SpiDiv::Div8), 61440);
    CHECK_EQ(wire_us(57600, SpiDiv::Div4), 30720);
    CHECK_EQ(wire_us(57600, SpiDiv::Div2), 15360);
    // One service interval of 2048 px is the "~4.4 ms" the driver comment promises at DIV8.
    CHECK_EQ(wire_us(2048, SpiDiv::Div8), 2184);
    CHECK_EQ(wire_us(0, SpiDiv::Div8), 0);
    // No 32-bit overflow on a large count (4e9 px is far past any frame, 64-bit math).
    CHECK_EQ(wire_us(1000000000u, SpiDiv::Div2) > 0, true);
}

static void test_divider_field() {
    // CTRL1 BR field (bits 5:3): 0b000 = /2, 0b001 = /4, 0b010 = /8.
    CHECK_EQ(br_field(SpiDiv::Div2), 0x00);
    CHECK_EQ(br_field(SpiDiv::Div4), 0x08);
    CHECK_EQ(br_field(SpiDiv::Div8), 0x10);
}

static void test_dma_chunks() {
    // The DMA count register is 16 bit: a transfer is cut into pieces of at most 65535 units.
    CHECK_EQ(dma_chunk(0), 0u);
    CHECK_EQ(dma_chunk(1), 1u);
    CHECK_EQ(dma_chunk(57600), 57600u);       // a full screen fits one chunk
    CHECK_EQ(dma_chunk(65535), 65535u);
    CHECK_EQ(dma_chunk(65536), 65535u);
    CHECK_EQ(dma_chunk(200000), 65535u);
    // Walking a count down chunk by chunk sums back to the count and never exceeds the limit.
    for (std::uint32_t total : {1u, 255u, 65535u, 65536u, 131070u, 131071u, 400000u}) {
        std::uint32_t left = total, sum = 0, pieces = 0;
        while (left) {
            const std::uint32_t n = dma_chunk(left);
            CHECK_EQ(n >= 1 && n <= 0xFFFF, true);
            left -= n; sum += n; pieces++;
        }
        CHECK_EQ(sum, total);
        CHECK_EQ(pieces, (total + 0xFFFE) / 0xFFFF);
    }
}

static void test_dma_threshold() {
    // Tiny runs stay on the polled path: the DMA setup is ~10 register writes, about the cost of
    // pushing a handful of pixels.
    CHECK_EQ(use_dma(1), false);
    CHECK_EQ(use_dma(kDmaMinPixels - 1), false);
    CHECK_EQ(use_dma(kDmaMinPixels), true);
    CHECK_EQ(use_dma(57600), true);
}

int main() {
    test_wire_time();
    test_divider_field();
    test_dma_chunks();
    test_dma_threshold();
    return test_report("test_lcd_timing");
}
