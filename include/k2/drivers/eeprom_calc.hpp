// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure addressing rules of the read-only 24Cxx EEPROM dump (k2/drivers/eeprom.hpp, remote command DebugEeprom). Host-tested
// in tests/test_eeprom_calc.cpp. 8-bit device address 0xA0 = 7-bit 0x50. The 24C01..24C16 take a one-byte word address and
// carry address bits 8..10 in the low bits of the device address (a 24C16 answers at all of 0x50..0x57); the 24C32 and larger
// take two word-address bytes (high first). `sel` adds the chip's A2..A0 pin levels to the device address.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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

// ---- Page writes: the settings store and the restore (k2/drivers/eeprom.hpp) ----
// A 24C16 page is 16 bytes; a page write wraps inside its page, so every write here is one whole, aligned page. Linear offsets
// 0x000..0x7FF on the INA bus (one-byte word address, select 0): locate(1, 0, off, kPage) gives the wire address.
inline constexpr std::size_t kPage = 16;
using Page = std::array<std::uint8_t, kPage>;
constexpr bool page_ok(std::uint16_t off) { return off % kPage == 0 && off < kSpan1; }

// The settings store owns 0x500..0x7DF: an area of the 24C16 that holds 0xFF on every unit read so far (two dumps of a 36 V unit,
// before and after a calibration repair) and that nothing else on the part writes. store_write() refuses any other page.
inline constexpr std::uint16_t kStoreBegin = 0x500;
inline constexpr std::uint16_t kStoreEnd = 0x7E0;
constexpr bool in_store(std::uint16_t off) { return page_ok(off) && off >= kStoreBegin && off + kPage <= kStoreEnd; }

// Untouched = Same, Mismatch, ReadFail, Refused, Busy (nothing was written). WriteFail, Timeout and Verify mean a write was issued.
enum class PageErr : std::uint8_t {
    Ok = 0,         // written and read back equal to the new bytes
    Same = 1,       // the page already holds the new bytes: nothing written
    Mismatch = 2,   // compare-and-swap: the page differs from the expected bytes (before[] says what is there)
    ReadFail = 3,
    WriteFail = 4,
    Timeout = 5,
    Verify = 6,     // the readback differs from the new bytes, or could not be read (after_valid says which)
    Refused = 7,    // the offset is not an aligned page (or not in the store): the bus was not touched
    Busy = 8,       // a test owns the port: the bus was not touched
};

struct PageResult {
    PageErr err = PageErr::ReadFail;
    bool before_valid = false, after_valid = false;
    Page before{}, after{};
};

// Chip as for cal_clear, with linear offsets: read(off, span), write(off, span) (one page write), wait_ready(). expected == nullptr:
// no compare (the store's own write: the page is read, and written only when it differs).
template <class Chip>
PageResult page_put(Chip &c, std::uint16_t off, const Page *expected, const Page &next) {
    PageResult r;
    if (!page_ok(off)) { r.err = PageErr::Refused; return r; }
    r.before_valid = c.read(off, std::span<std::uint8_t>(r.before));
    if (!r.before_valid) return r;
    if (expected != nullptr && r.before != *expected) { r.err = PageErr::Mismatch; return r; }
    if (r.before == next) { r.err = PageErr::Same; r.after = r.before; r.after_valid = true; return r; }
    const bool wrote = c.write(off, std::span<const std::uint8_t>(next));
    const bool ready = c.wait_ready();
    r.after_valid = c.read(off, std::span<std::uint8_t>(r.after));
    r.err = !wrote ? PageErr::WriteFail : !ready ? PageErr::Timeout : r.after_valid && r.after == next ? PageErr::Ok : PageErr::Verify;
    return r;
}

// ---- Store slots ----
// One slot = one page: 'K' '2' id len payload[kSlotPayload] crc8. len <= kSlotPayload, unused payload bytes 0xFF, crc8 (poly 0x07,
// init 0) over bytes 0..14. A blank page (0xFF) or any damage decodes as absent. Slot n lives at kStoreBegin + 16 * (n - 1).
inline constexpr std::size_t kSlotPayload = 11;
enum class Slot : std::uint8_t { UsbLink = 1, Spoof = 2 };
constexpr std::uint16_t slot_off(Slot s) { return std::uint16_t(kStoreBegin + kPage * (std::uint8_t(s) - 1)); }
static_assert(in_store(slot_off(Slot::Spoof)));

constexpr std::uint8_t crc8(std::span<const std::uint8_t> d) {
    std::uint8_t c = 0;
    for (std::uint8_t b : d) {
        c ^= b;
        for (int i = 0; i < 8; i++) c = std::uint8_t(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

constexpr Page slot_encode(Slot s, std::span<const std::uint8_t> payload) {
    Page p{};
    for (auto &b : p) b = 0xFF;
    const std::size_t n = payload.size() < kSlotPayload ? payload.size() : kSlotPayload;
    p[0] = 'K'; p[1] = '2'; p[2] = std::uint8_t(s); p[3] = std::uint8_t(n);
    for (std::size_t i = 0; i < n; i++) p[4 + i] = payload[i];
    p[kPage - 1] = crc8(std::span<const std::uint8_t>(p.data(), kPage - 1));
    return p;
}

// The payload length, or -1 when the page is not a valid slot s. out receives the payload (at most out.size() bytes).
constexpr int slot_decode(Slot s, const Page &p, std::span<std::uint8_t> out) {
    if (p[0] != 'K' || p[1] != '2' || p[2] != std::uint8_t(s) || p[3] > kSlotPayload) return -1;
    if (crc8(std::span<const std::uint8_t>(p.data(), kPage - 1)) != p[kPage - 1]) return -1;
    for (std::size_t i = 0; i < p[3] && i < out.size(); i++) out[i] = p[4 + i];
    return p[3];
}

}  // namespace k2::eeprom
