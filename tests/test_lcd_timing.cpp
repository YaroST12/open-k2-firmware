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

static void test_service_pacer() {
    // The paint service cadence (docs/ARCH_REWORK.md E1) runs on elapsed time: one round once kServiceUs has passed since
    // the last round ended, however many pixels or windows went out in between. Clock in microseconds here.
    CHECK_EQ(kServiceUs, 1000u);
    ServicePacer p(kServiceUs);
    p.finish(5000);                       // a round ended at t = 5 ms
    CHECK_EQ(p.start(5001), false);       // a short window right after: not due
    CHECK_EQ(p.start(5999), false);
    CHECK_EQ(p.start(6000), true);        // 1 ms later: due, and the round is open
    // While a round is open (the hook or usb::service reached a stream call) no nested round starts, timed or forced.
    CHECK_EQ(p.start(9000), false);
    CHECK_EQ(p.force(), false);
    p.finish(7500);                       // a long round (1.5 ms): the interval counts from its end
    CHECK_EQ(p.start(8499), false);
    CHECK_EQ(p.start(8500), true);
    p.finish(8600);
    // lcd::pump() serves unconditionally (outside a stream), and that round restarts the interval too.
    CHECK_EQ(p.force(), true);
    p.finish(8700);
    CHECK_EQ(p.start(9600), false);
    CHECK_EQ(p.start(9700), true);
    p.finish(9700);
    // Many small windows: a check every 300 us yields one round per 1.2 ms (the first check at or past 1 ms).
    unsigned rounds = 0;
    for (std::uint32_t t = 9700; t <= 9700 + 12000; t += 300)
        if (p.start(t)) { rounds++; p.finish(t); }
    CHECK_EQ(rounds, 10u);
    // Wrap-safe: the device clock is a 32-bit cycle counter that wraps every ~17.9 s at 240 MHz.
    ServicePacer w(240000);               // 1 ms of core cycles
    w.finish(0xFFFFFF00u);
    CHECK_EQ(w.start(0xFFFFFFFFu), false);
    CHECK_EQ(w.start(240000u - 0x101u), false);   // 239 999 cycles after the stamp, across the wrap
    CHECK_EQ(w.start(240000u - 0x100u), true);    // exactly 1 ms
}

static void test_lab_divider() {   // the runtime switch offers 4 and 8 only
    SpiDiv d = SpiDiv::Div8;
    CHECK_EQ(lab_spi_div(4, d), true);
    CHECK_EQ(d, SpiDiv::Div4);
    CHECK_EQ(lab_spi_div(8, d), true);
    CHECK_EQ(d, SpiDiv::Div8);
    CHECK_EQ(lab_spi_div(2, d), false);   // 60 MHz is not offered
    CHECK_EQ(lab_spi_div(0, d), false);
    CHECK_EQ(lab_spi_div(16, d), false);
    CHECK_EQ(d, SpiDiv::Div8);            // a refusal leaves the value alone
}

int main() {
    test_service_pacer();
    test_lab_divider();
    test_wire_time();
    test_divider_field();
    test_dma_chunks();
    test_dma_threshold();
    return test_report("test_lcd_timing");
}
