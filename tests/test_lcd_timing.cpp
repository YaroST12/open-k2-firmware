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

static void test_lab_divider() {   // the runtime switch offers 2, 4 and 8
    SpiDiv d = SpiDiv::Div8;
    CHECK_EQ(lab_spi_div(4, d), true);
    CHECK_EQ(d, SpiDiv::Div4);
    CHECK_EQ(lab_spi_div(8, d), true);
    CHECK_EQ(d, SpiDiv::Div8);
    CHECK_EQ(lab_spi_div(2, d), true);    // 60 MHz: a RAM-only lab value for a visual check
    CHECK_EQ(d, SpiDiv::Div2);
    d = SpiDiv::Div8;
    CHECK_EQ(lab_spi_div(0, d), false);
    CHECK_EQ(lab_spi_div(1, d), false);
    CHECK_EQ(lab_spi_div(3, d), false);
    CHECK_EQ(lab_spi_div(16, d), false);
    CHECK_EQ(d, SpiDiv::Div8);            // a refusal leaves the value alone
}

static void test_backlight_pwm() {
    // 24 kHz off the timer clock: 240 MHz -> 10000 counts, and it follows the AHB divider (60 MHz -> 2500).
    CHECK_EQ(backlight_period(240'000'000), 10000u);
    CHECK_EQ(backlight_period(120'000'000), 5000u);
    CHECK_EQ(backlight_period(60'000'000), 2500u);
    CHECK_EQ(backlight_period(30'000'000), 1250u);
    CHECK_EQ(backlight_period(1'000), 1u);          // never zero
    CHECK_EQ(backlight_period(4'000'000'000u), 65536u);   // the 16-bit counter's limit
    // Duty: pct of the period, 100 = never low (compare value above the top), 0 = never high.
    CHECK_EQ(backlight_ccr(0, 10000), 0u);
    CHECK_EQ(backlight_ccr(30, 10000), 3000u);
    CHECK_EQ(backlight_ccr(50, 10000), 5000u);
    CHECK_EQ(backlight_ccr(99, 10000), 9900u);
    CHECK_EQ(backlight_ccr(100, 10000), 10000u);
    CHECK_EQ(backlight_ccr(250, 10000), 10000u);    // a request above 100 is 100
    CHECK_EQ(backlight_ccr(1, 2500), 25u);
    CHECK_EQ(backlight_ccr(30, 65536), 19660u);     // no 32-bit overflow at the top of the range
    CHECK_EQ(backlight_arr(10000), 9999u);
    CHECK_EQ(backlight_arr(1), 0u);
}

static void test_panel_sequence() {
    PanelSeq s;
    CHECK_EQ(s.state(), PanelSeq::State::Awake);
    CHECK_EQ(s.poll(0), PanelSeq::Action::None);
    // Sleep: DISPOFF + SLPIN go out at once.
    s.request(false);
    CHECK_EQ(s.poll(1000), PanelSeq::Action::Sleep);
    CHECK_EQ(s.state(), PanelSeq::State::Asleep);
    CHECK_EQ(s.poll(1001), PanelSeq::Action::None);
    // Wake: SLPOUT only 120 ms after SLPIN, DISPON 120 ms after SLPOUT.
    s.request(true);
    CHECK_EQ(s.poll(1050), PanelSeq::Action::None);
    CHECK_EQ(s.poll(1119), PanelSeq::Action::None);
    CHECK_EQ(s.poll(1120), PanelSeq::Action::SlpOut);
    CHECK_EQ(s.state(), PanelSeq::State::Waking);
    CHECK_EQ(s.poll(1239), PanelSeq::Action::None);
    CHECK_EQ(s.awake(), false);
    CHECK_EQ(s.poll(1240), PanelSeq::Action::DispOn);
    CHECK_EQ(s.awake(), true);
    CHECK_EQ(s.poll(1241), PanelSeq::Action::None);
    // A sleep request while waking waits for the 120 ms after SLPOUT, then puts the panel back to sleep.
    s.request(false);
    CHECK_EQ(s.poll(2000), PanelSeq::Action::Sleep);
    s.request(true);
    CHECK_EQ(s.poll(2120), PanelSeq::Action::SlpOut);
    s.request(false);
    CHECK_EQ(s.poll(2200), PanelSeq::Action::None);
    CHECK_EQ(s.poll(2240), PanelSeq::Action::Sleep);
    CHECK_EQ(s.state(), PanelSeq::State::Asleep);
    // The millisecond counter wraps: the gaps are unsigned differences.
    PanelSeq w;
    w.request(false);
    CHECK_EQ(w.poll(0xFFFFFFF0u), PanelSeq::Action::Sleep);
    w.request(true);
    CHECK_EQ(w.poll(0xFFFFFFF0u + 100), PanelSeq::Action::None);
    CHECK_EQ(w.poll(0xFFFFFFF0u + 120), PanelSeq::Action::SlpOut);
    // A request that changes nothing does nothing.
    PanelSeq a;
    a.request(true);
    CHECK_EQ(a.poll(5), PanelSeq::Action::None);
    CHECK_EQ(a.awake(), true);
}

static void test_boot_divider() {   // /4 (30 MHz) at every boot since 2026-11-12
    CHECK_EQ(kBootSpiDiv, SpiDiv::Div4);
}

int main() {
    test_service_pacer();
    test_lab_divider();
    test_boot_divider();
    test_backlight_pwm();
    test_panel_sequence();
    test_wire_time();
    test_divider_field();
    test_dma_chunks();
    test_dma_threshold();
    return test_report("test_lcd_timing");
}
