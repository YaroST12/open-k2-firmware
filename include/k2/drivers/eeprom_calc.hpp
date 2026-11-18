// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure addressing rules of the read-only 24Cxx EEPROM dump (k2/drivers/eeprom.hpp, remote command DebugEeprom). Host-tested
// in tests/test_eeprom_calc.cpp. 8-bit device address 0xA0 = 7-bit 0x50. The 24C01..24C16 take a one-byte word address and
// carry address bits 8..10 in the low bits of the device address (a 24C16 answers at all of 0x50..0x57); the 24C32 and larger
// take two word-address bytes (high first). `sel` adds the chip's A2..A0 pin levels to the device address.
#pragma once
#include <cstddef>
#include <cstdint>

namespace k2::eeprom {

inline constexpr std::uint8_t kBase7 = 0x50;       // first device address; 0x50..0x57 are probed
inline constexpr std::uint8_t kSlots = 8;
inline constexpr std::size_t kMaxChunk = 32;       // bytes per read request; divides 256 so a 32-byte stride never crosses a block
inline constexpr std::uint32_t kSpan1 = 0x800;     // one-byte addressing: 8 blocks of 256 bytes (24C16)
inline constexpr std::uint32_t kSpan2 = 0x10000;   // two-byte addressing: 16-bit word address (up to 24C512)

// The probe reads kProbeReads windows of kProbeBytes at word address 0 with one-byte addressing from the lowest device that
// answers. A one-byte chip returns the same bytes every time; a two-byte chip takes the byte as the high address and keeps its
// old low pointer, so its windows advance. The host compares them (tools/k2_remote.py eeprom_classify).
inline constexpr std::size_t kProbeReads = 4;
inline constexpr std::size_t kProbeBytes = 4;

enum class Err : std::uint8_t { Ok = 0, Nack = 1, Busy = 2 };

// What the probe found: bit i of ack_mask = device 0x50 + i answered an address-only write; dev7 = the lowest one (0 = none);
// sample = kProbeReads windows of kProbeBytes, one after the other (zero when nothing answered).
struct Probe {
    std::uint8_t ack_mask = 0;
    std::uint8_t dev7 = 0;
    std::uint8_t sample[kProbeReads * kProbeBytes] = {};
};

struct Addr {
    std::uint8_t dev7 = 0;
    std::uint8_t nword = 0;
    std::uint8_t word[2] = {};
};

// Where `len` bytes at linear offset `off` land. False = refused before the bus is touched: unknown `abytes` (1 or 2), sel
// above 7, len 0 or above kMaxChunk, the read leaves the array, or (one-byte) it crosses a 256-byte block or the device address
// would leave 0x50..0x57.
constexpr bool locate(std::uint8_t abytes, std::uint8_t sel, std::uint16_t off, std::size_t len, Addr &out) {
    if (sel >= kSlots || len == 0 || len > kMaxChunk) return false;
    if (abytes == 1) {
        if (std::uint32_t(off) + len > kSpan1 || (off & 0xFF) + len > 256) return false;
        const unsigned dev = kBase7 + sel + (off >> 8);
        if (dev >= kBase7 + kSlots) return false;
        out = {std::uint8_t(dev), 1, {std::uint8_t(off & 0xFF), 0}};
        return true;
    }
    if (abytes == 2) {
        if (std::uint32_t(off) + len > kSpan2) return false;
        out = {std::uint8_t(kBase7 + sel), 2, {std::uint8_t(off >> 8), std::uint8_t(off & 0xFF)}};
        return true;
    }
    return false;
}

}  // namespace k2::eeprom
