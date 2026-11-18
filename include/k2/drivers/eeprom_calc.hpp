// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure addressing rules of the read-only 24Cxx EEPROM dump (k2/drivers/eeprom.hpp, remote command DebugEeprom). Host-tested
// in tests/test_eeprom_calc.cpp. 8-bit device address 0xA0 = 7-bit 0x50. The 24C01..24C16 take a one-byte word address and
// carry address bits 8..10 in the low bits of the device address (a 24C16 answers at all of 0x50..0x57); the 24C32 and larger
// take two word-address bytes (high first). `sel` adds the chip's A2..A0 pin levels to the device address.
#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/drivers/power_calc.hpp"

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

// ---- The one write: clearing a corrupt calibration area (K2_EEPROM_CAL_CLEAR) ----
// Bytes 0x04..0x0F of the first 24C16 block (device 0x50, select 0, one-byte word address) hold three int32 trims. This is not
// a write API: the address, the length and the value written are the constants below, and the only thing a caller supplies is
// the 12 bytes it expects to find there (compare-and-swap). Everything else, including the bus (the INA bus, in eeprom.cpp), is
// fixed at build time. cal_clear() is pure over a Chip so a host test can count what it does on the wire.
inline constexpr std::uint8_t kCalDev7 = kBase7;                 // select 0, block 0
inline constexpr std::uint8_t kCalWord = 0x04;
inline constexpr std::size_t kCalLen = 12;
inline constexpr std::uint8_t kCalPage = 16;                     // 24C16 page size: a page write wraps inside its 16 bytes
static_assert(kCalWord % kCalPage + kCalLen <= kCalPage, "the repair must stay inside one page");
inline constexpr std::uint32_t kCalWriteMs = 10;                 // longest wait for the write cycle (the part needs about 5 ms)
inline constexpr std::array<std::uint8_t, kCalLen> kCalZero = {};

// Read-only classification of the three little-endian settings EEPROM calibration trims. Unknown is a monitor chip
// outside the set whose reference scales are known; Absent is a read that did not reach the EEPROM. The trims are never
// applied to this firmware's measurements: they are reported so a user can notice a settings-store inconsistency.
enum class CalState : std::uint8_t { Unknown = 0, Absent = 1, Ok = 2, Corrupt = 3 };

struct CalInfo {
    CalState state = CalState::Unknown;
    std::int32_t voltage_gain = 0;
    std::int32_t current_zero = 0;
    std::int32_t current_gain = 0;
};

[[gnu::always_inline]] constexpr std::int32_t cal_i32(std::span<const std::uint8_t, kCalLen> bytes,
                                                        std::size_t off) {
    const std::uint32_t u = std::uint32_t(bytes[off]) | (std::uint32_t(bytes[off + 1]) << 8) |
                            (std::uint32_t(bytes[off + 2]) << 16) | (std::uint32_t(bytes[off + 3]) << 24);
    return std::bit_cast<std::int32_t>(u);
}

constexpr bool cal_scales(power::Chip chip, std::int32_t &voltage, std::int32_t &current) {
    switch (chip) {
    case power::Chip::Ina228: voltage = 204'800; current = 307'200; return true;
    case power::Chip::Ina226: voltage = 1'024'000; current = 307'200; return true;
    case power::Chip::Ina237: voltage = 409'600; current = 614'400; return true;
    case power::Chip::None: return false;
    }
    return false;
}

constexpr bool outside_symmetric(std::int32_t value, std::int32_t limit) {
    return value > limit || value < -limit;   // no abs(INT32_MIN)
}

constexpr CalInfo classify_cal(power::Chip chip, bool present, std::span<const std::uint8_t, kCalLen> bytes) {
    if (!present) return {CalState::Absent, 0, 0, 0};
    CalInfo out;
    out.voltage_gain = cal_i32(bytes, 0);
    out.current_zero = cal_i32(bytes, 4);
    out.current_gain = cal_i32(bytes, 8);
    std::int32_t voltage = 0, current = 0;
    if (!cal_scales(chip, voltage, current)) {
        out.state = CalState::Unknown;
        return out;
    }
    // A gain denominator at or below zero is included by the 10% test. The symmetric bound avoids signed overflow for
    // malformed input such as INT32_MIN, so this path never has to form full_scale + trim.
    out.state = outside_symmetric(out.voltage_gain, voltage / 10) ||
                outside_symmetric(out.current_zero, current / 100) ||
                outside_symmetric(out.current_gain, current / 10)
                    ? CalState::Corrupt : CalState::Ok;
    return out;
}

// Untouched = Mismatch, ReadFail, Busy (nothing was written). WriteFail, Timeout and Verify mean a write was issued: the area
// may hold anything, read it again.
enum class CalErr : std::uint8_t {
    Ok = 0,         // read back all zero
    Mismatch = 1,   // the area differs from the expected bytes: refused, nothing written (before[] says what is there)
    ReadFail = 2,   // the first read NACKed: nothing compared, nothing written
    WriteFail = 3,  // the write was NACKed (maybe part way)
    Timeout = 4,    // the write cycle did not end within kCalWriteMs
    Verify = 5,     // the readback is not all zero, or could not be read (after_valid says which)
    Busy = 6,       // a charger test owns the port: nothing touched the bus
};

struct CalResult {
    CalErr err = CalErr::ReadFail;
    bool before_valid = false, after_valid = false;
    std::uint8_t before[kCalLen] = {};
    std::uint8_t after[kCalLen] = {};
};

// Chip: bool read(word, span<u8>), bool write(word, span<const u8>) (one page write, STOP at the end), bool wait_ready() (true
// when the write cycle ended, bounded by kCalWriteMs). A failed read never reaches the compare.
template <class Chip>
CalResult cal_clear(Chip &c, std::span<const std::uint8_t, kCalLen> expected) {
    CalResult r;
    r.before_valid = c.read(kCalWord, std::span<std::uint8_t>(r.before));
    if (!r.before_valid) return r;                                // err stays ReadFail
    for (std::size_t i = 0; i < kCalLen; i++)
        if (r.before[i] != expected[i]) { r.err = CalErr::Mismatch; return r; }
    const bool wrote = c.write(kCalWord, std::span<const std::uint8_t>(kCalZero));
    const bool ready = c.wait_ready();                            // always look again after a write was attempted
    r.after_valid = c.read(kCalWord, std::span<std::uint8_t>(r.after));
    bool zero = r.after_valid;
    for (std::size_t i = 0; zero && i < kCalLen; i++) zero = r.after[i] == 0;
    r.err = !wrote ? CalErr::WriteFail : !ready ? CalErr::Timeout : zero ? CalErr::Ok : CalErr::Verify;
    return r;
}

}  // namespace k2::eeprom
