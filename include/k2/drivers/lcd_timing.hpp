// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// LCD link timing and DMA chunk planning: pure constexpr code over values, so host tests can
// cover it (the driver in src/drivers/lcd.cpp owns the registers).
#pragma once
#include <cstdint>

namespace k2::lcd {

// SPI3 clock divider off APB1 (120 MHz): /8 = 15 MHz ships; /4 = 30 MHz is a runtime lab switch (`lcd spi 4`, not
// persisted); /2 = 60 MHz is for a hardware signal-integrity check on a specific board only.
enum class SpiDiv : std::uint8_t { Div2 = 2, Div4 = 4, Div8 = 8 };

inline constexpr std::uint32_t kApb1Hz = 120'000'000;

// CTRL1 BR field (bits 5:3) for a divider: 0b000 = /2, 0b001 = /4, 0b010 = /8.
constexpr std::uint16_t br_field(SpiDiv div) {
    switch (div) {
    case SpiDiv::Div2: return 0u << 3;
    case SpiDiv::Div4: return 1u << 3;
    default:           return 2u << 3;
    }
}

// Time on the wire for `px` RGB565 pixels, in microseconds: one pixel is one 16-bit SPI frame
// (16 * divider APB1 cycles). Command and window overhead is not included.
constexpr std::uint32_t wire_us(std::uint32_t px, SpiDiv div) {
    return static_cast<std::uint32_t>(std::uint64_t{px} * 16u * static_cast<unsigned>(div) * 1'000'000u /
                                      (std::uint64_t{kApb1Hz}));
}

// Service interval, in pixels, between two usb::service() / idle-hook rounds while a stream is pushed by hand: a wire-time
// budget (about 2.2 ms), so the pixel count scales with the divider. 2048 px at the shipped /8.
constexpr std::uint32_t service_pixels(SpiDiv div) { return 2048u * 8u / static_cast<unsigned>(div); }

// The same while a DMA transfer runs and the CPU only waits: about 0.5 ms of wire time (468 px at /8).
constexpr std::uint32_t dma_service_px(SpiDiv div) { return 3750u / static_cast<unsigned>(div); }

// The runtime lab switch (`lcd spi <n>`) offers /8 (shipped) and /4 only; /2 stays a compile-time experiment.
constexpr bool lab_spi_div(unsigned value, SpiDiv& out) {
    if (value == 4) { out = SpiDiv::Div4; return true; }
    if (value == 8) { out = SpiDiv::Div8; return true; }
    return false;
}

// DMA transfer size: the count register is 16 bit, so a longer run goes out in pieces.
inline constexpr std::uint32_t kDmaMaxCount = 0xFFFF;
constexpr std::uint32_t dma_chunk(std::uint32_t remaining) {
    return remaining < kDmaMaxCount ? remaining : kDmaMaxCount;
}

// Below this many pixels the DMA setup (about ten register writes) costs as much as pushing the
// pixels by hand, so short runs stay polled.
inline constexpr std::uint32_t kDmaMinPixels = 32;
constexpr bool use_dma(std::uint32_t px) { return px >= kDmaMinPixels; }

}  // namespace k2::lcd
