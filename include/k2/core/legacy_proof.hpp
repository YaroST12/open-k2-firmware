// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ownership proof for the legacy raw-store pages 0x080E4000..0x08100000.
//
// That range lies inside the device storage area (0x080E0000..0x080FFEFF), which also holds data this firmware does not
// own. Earlier firmware of this project wrote its own raw
// stores there. They have moved onto the littlefs volume, but the old pages may still hold data. A page of this
// range may only be erased when it carries the signature of one of THOSE stores. A page with anything else on it
// is never touched (k2/drivers/flash.hpp refuses the erase).
//
// The proof reads half-words through a reader `rd(addr) -> uint16_t` (memory mapped on the device, a fake in the
// host tests), so it is pure and testable. Every proof stands on its own page (or on the page before it, whose
// erase comes later: wipe order is descending), so a power cut in the middle of the wipe never strands or
// misjudges a page.
//
// Old stores (the formats are from the firmware history before the littlefs move; "inferred" provenance):
//   0x080E4000  2 sectors   log snapshot       "K2LG", u32 text length (<= 4088), text; the second sector holds
//                                              text only when the length exceeds 2040, so it is proven by the first
//   0x080E5000  1 sector    capture area       reserved, never written: never proven, never erased
//   0x080E6000  1 sector    USB config         "K2US", flags, ~flags, 0, 0
//   0x080E6800  1 sector    UI preferences     "K2UP", flags, ~flags, 0, 0
//   0x080E7000  2 sectors   report cards       "K2RC" at the sector start
//   0x080E8000  32 sectors  Black Box ring     "K2BB" at the sector start
//   0x080F8000  8 sectors   charger journal    no sector header: a whole CRC-valid record inside the sector
//   0x080FC000  8 sectors   e-marker library   "K2EL" at the sector start
#pragma once
#include <cstdint>

namespace k2::legacy {

inline constexpr std::uint32_t kSector = 0x800;
inline constexpr std::uint32_t kBase = 0x080E4000, kEnd = 0x08100000;
inline constexpr std::uint32_t kSectors = (kEnd - kBase) / kSector;   // 56

inline constexpr std::uint32_t kLogAddr = 0x080E4000, kLogSectors = 2, kLogHeader = 8, kLogMaxText = kLogSectors * kSector - kLogHeader;
inline constexpr std::uint32_t kLegacyUsbCfg = 0x080E6000, kLegacyUiPrefs = 0x080E6800;
inline constexpr std::uint32_t kCardsBase = 0x080E7000, kCardsEnd = 0x080E8000;
inline constexpr std::uint32_t kBlackBoxBase = 0x080E8000, kBlackBoxEnd = 0x080F8000;
inline constexpr std::uint32_t kJournalBase = 0x080F8000, kJournalEnd = 0x080FC000;
inline constexpr std::uint32_t kEmLibBase = 0x080FC000, kEmLibEnd = 0x08100000;

static_assert(kBase % kSector == 0 && kEnd % kSector == 0 && kSectors == 56);
static_assert(kCardsEnd == kBlackBoxBase && kBlackBoxEnd == kJournalBase && kJournalEnd == kEmLibBase && kEmLibEnd == kEnd);

namespace detail {

// "ab" as a little-endian half-word.
constexpr std::uint16_t pair(char a, char b) { return static_cast<std::uint16_t>(static_cast<unsigned char>(a) | (static_cast<unsigned char>(b) << 8)); }

template <class Rd>
bool magic(std::uint32_t addr, Rd&& rd, char a, char b, char c, char d) {
    return rd(addr) == pair(a, b) && rd(addr + 2) == pair(c, d);
}

// Flags byte followed by its complement, then two zero bytes (the 8-byte USB config / UI prefs record).
template <class Rd>
bool flag_record(std::uint32_t addr, Rd&& rd, char a, char b, char c, char d) {
    if (!magic(addr, rd, a, b, c, d)) return false;
    const std::uint16_t f = rd(addr + 4);
    return static_cast<std::uint8_t>(f) == static_cast<std::uint8_t>(~(f >> 8)) && rd(addr + 6) == 0;
}

template <class Rd>
bool blank(std::uint32_t sector, Rd&& rd) {
    for (std::uint32_t a = sector; a < sector + kSector; a += 2)
        if (rd(a) != 0xFFFF) return false;
    return true;
}

constexpr std::uint16_t crc_step(std::uint16_t crc, std::uint8_t b) {
    crc = static_cast<std::uint16_t>(crc ^ (b << 8));
    for (int i = 0; i < 8; i++) crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
    return crc;
}

template <class Rd>
std::uint8_t byte_at(std::uint32_t addr, Rd&& rd) {
    const std::uint16_t h = rd(addr & ~1u);
    return static_cast<std::uint8_t>((addr & 1) ? h >> 8 : h);
}

// A journal record: A5, type 1..5, even payload length <= 30, 0, payload, CRC-16/CCITT (init FFFF) over all of it.
// It must lie completely inside [sector, sector + kSector).
template <class Rd>
bool journal_record_at(std::uint32_t sector, std::uint32_t off, Rd&& rd) {
    const std::uint32_t a = sector + off;
    if (byte_at(a, rd) != 0xA5) return false;
    const std::uint8_t type = byte_at(a + 1, rd), len = byte_at(a + 2, rd);
    if (type < 1 || type > 5 || len % 2 != 0 || len > 30 || byte_at(a + 3, rd) != 0) return false;
    if (off + 4u + len + 2u > kSector) return false;
    std::uint16_t crc = 0xFFFF;
    for (std::uint32_t i = 0; i < 4u + len; i++) crc = crc_step(crc, byte_at(a + i, rd));
    return crc == (byte_at(a + 4 + len, rd) | (byte_at(a + 5 + len, rd) << 8));
}

template <class Rd>
bool journal_sector(std::uint32_t sector, Rd&& rd) {
    for (std::uint32_t off = 0; off + 6 <= kSector; off += 2)
        if (journal_record_at(sector, off, rd)) return true;
    return false;
}

}  // namespace detail

// True when the sector at `addr` (kSector aligned, inside kBase..kEnd) carries the signature of one of this
// project's old raw stores. Anything else, a blank sector included, is not ours.
template <class Rd>
bool ours(std::uint32_t addr, Rd&& rd) {
    if (addr % kSector != 0 || addr < kBase || addr >= kEnd) return false;
    using namespace detail;
    if (addr == kLogAddr) {
        if (!magic(addr, rd, 'K', '2', 'L', 'G')) return false;
        const std::uint32_t len = rd(addr + 4) | (static_cast<std::uint32_t>(rd(addr + 6)) << 16);
        return len <= kLogMaxText;
    }
    if (addr == kLogAddr + kSector) {   // proven by the header sector, and only when the text really spills over
        if (!magic(kLogAddr, rd, 'K', '2', 'L', 'G')) return false;
        const std::uint32_t len = rd(kLogAddr + 4) | (static_cast<std::uint32_t>(rd(kLogAddr + 6)) << 16);
        return len > kSector - kLogHeader && len <= kLogMaxText && !blank(addr, rd);
    }
    if (addr == kLegacyUsbCfg) return flag_record(addr, rd, 'K', '2', 'U', 'S');
    if (addr == kLegacyUiPrefs) return flag_record(addr, rd, 'K', '2', 'U', 'P');
    if (addr >= kCardsBase && addr < kCardsEnd) return magic(addr, rd, 'K', '2', 'R', 'C');
    if (addr >= kBlackBoxBase && addr < kBlackBoxEnd) return magic(addr, rd, 'K', '2', 'B', 'B');
    if (addr >= kJournalBase && addr < kJournalEnd) return journal_sector(addr, rd);
    if (addr >= kEmLibBase && addr < kEmLibEnd) return magic(addr, rd, 'K', '2', 'E', 'L');
    return false;   // the reserved capture area 0x080E5000.. and every other sector
}

}  // namespace k2::legacy
