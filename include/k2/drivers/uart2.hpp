// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USART2 on PA2 (TX, wired to D-) / PA3 (RX, wired to D+) for UFCS / VIVO: 115200 8N2 (STOP field 0x2000),
// RX through DMA1 channel 6 into a circular ring, TX polled. PA2/PA3 are also the ADC's CC1/CC2
// inputs, so claim() takes them from the ADC (scan stopped, pins released) and release() hands
// them back; the two are never active together. Nothing is transmitted by claim() itself.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::uart2 {

inline constexpr std::uint32_t kBaud = 115200;
inline constexpr std::size_t kRingBytes = 260;

// Line settings of claim(): the defaults are 115200 with 2 stop bits (what VIVO uses). The UFCS
// tuner passes its own (the standard's 8N1 by default) and can try other framings; the baud divider is APB1 120 MHz / baud (integer).
struct Config {
    std::uint32_t baud = kBaud;
    std::uint8_t stop_bits = 2;   // 1 or 2
};

// Stop the ADC scan, switch PA2 to alternate push-pull and PA3 to floating input, start USART2 and
// the RX DMA ring. False if already claimed.
bool claim(const Config& cfg = {});
// Stop USART2/DMA, return PA2/PA3 to analog and restart the ADC scan.
void release();
bool claimed();

// Send bytes (blocking, at most a few ms for a frame). False when not claimed or on timeout.
bool tx(std::span<const std::uint8_t> bytes);
// Move received bytes into `out`; returns the count (0 when none). Overruns drop the oldest bytes.
std::size_t rx(std::span<std::uint8_t> out);
// True once after the RX line went idle following traffic (end of a frame).
bool idle_seen();

// Receiver error flags in the USART status register.
inline constexpr std::uint32_t kStsPerr = 1u << 0, kStsFerr = 1u << 1, kStsNerr = 1u << 2, kStsRoerr = 1u << 3;
inline constexpr std::uint32_t kStsIdlef = 1u << 4, kStsRdbf = 1u << 5;
inline constexpr std::uint32_t kStsErrMask = kStsPerr | kStsFerr | kStsNerr | kStsRoerr;

// Receiver errors seen since claim(): a lower bound, because the DMA clears a flag when it moves the
// next byte and rx() only samples the status once per call.
struct Errors {
    std::uint32_t framing = 0, noise = 0, overrun = 0, parity = 0;
    constexpr std::uint32_t total() const { return framing + noise + overrun + parity; }
    constexpr bool operator==(const Errors&) const = default;
};
// Count each error flag when it newly appears in `sts` (flags are sticky until the next byte, so one
// that stays set is not counted again). `prev` is the error bits of the previous sample.
constexpr void tally_errors(std::uint32_t sts, std::uint32_t& prev, Errors& e) {
    const std::uint32_t fresh = sts & kStsErrMask & ~prev;
    if (fresh & kStsFerr) e.framing++;
    if (fresh & kStsNerr) e.noise++;
    if (fresh & kStsRoerr) e.overrun++;
    if (fresh & kStsPerr) e.parity++;
    prev = sts & kStsErrMask;
}
// The totals since claim().
Errors errors();

}  // namespace k2::uart2
