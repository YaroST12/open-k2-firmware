// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// "Factory wipe": erases everything this firmware stored in flash and leaves all other content alone, so the K2
// can be flashed with a different firmware container afterwards. Pure sequence over a small environment seam, so host tests cover the order
// and the stop rules (tests/test_factory_wipe.cpp). The real environment is in src/core/remote.cpp
// (remote command FACTORY_WIPE, tools/k2_remote.py factory-wipe).
//
// What is erased: every 2 KB sector of the littlefs volume (k2::flash kFsBase..kFsEnd, 90 sectors), plus the
// sectors of the legacy raw-store range (kStoreBase..kStoreEnd, 56 candidates) that carry the signature of one
// of our old stores (k2/core/legacy_proof.hpp). The device storage area (0x080E0000 up) is
// never touched except for those proven pages: unknown content stays, byte for byte. The bootloader, the
// 0x08003000 info page and the application are not in the list, and k2::flash refuses them anyway.
//
// Order: unmount the volume (nothing writes after the erase), pick the legacy sectors that are ours (before
// the first erase, since erasing a page can remove the proof of another), erase the volume ascending and then
// the legacy sectors descending through the guarded store erase, read every listed sector back (all 0xFF),
// then clear this firmware's backup registers. The first failing erase stops the run; a failed verify is
// reported with the count and the first bad sector. In both cases the backup registers stay and the caller
// must NOT enter DFU.
#pragma once
#include <concepts>
#include <cstddef>
#include <cstdint>

#include "k2/drivers/flash.hpp"
#include "k2/core/legacy_proof.hpp"

namespace k2::factory_wipe {

inline constexpr std::uint32_t kFsSectors = (flash::kFsEnd - flash::kFsBase) / flash::kSectorSize;          // 90
inline constexpr std::uint32_t kLegacySectors = (flash::kStoreEnd - flash::kStoreBase) / flash::kSectorSize;   // 56 candidates
inline constexpr std::uint32_t kMaxSectors = kFsSectors + kLegacySectors;                                   // 146
static_assert(kMaxSectors <= 0xFFFF, "the counts travel as u16");
static_assert(kLegacySectors <= 64, "the proven legacy sectors fit one 64-bit mask");

// Address of file system sector i (ascending); 0 past the end.
constexpr std::uint32_t fs_sector_addr(std::size_t i) {
    return i < kFsSectors ? flash::kFsBase + static_cast<std::uint32_t>(i) * flash::kSectorSize : 0;
}

enum class Err : std::uint8_t {
    Ok = 0,           // everything erased and read back blank: the caller may enter DFU
    EraseFailed = 1,  // the controller (or the erase read-back) failed: Result::first_fail, status
    VerifyFailed = 2, // erased, but a sector does not read blank: Result::failed, first_fail
};

struct Result {
    Err err = Err::Ok;
    std::uint16_t sectors = 0;              // size of the list: volume plus proven legacy sectors
    std::uint16_t erased = 0;               // erases that succeeded
    std::uint16_t verified = 0;             // sectors that read back all 0xFF
    std::uint16_t failed = 0;               // sectors that did not (verify pass)
    std::uint32_t first_fail = 0;           // address of the first failing sector, 0 when none
    flash::Status status = flash::Status::Ok;   // controller status of the failing erase
};

// Environment of the sequence (real one in src/core/remote.cpp, a fake in the host tests).
template <class E>
concept Env = requires(E& e, const E& ce, std::uint32_t addr) {
    e.unmount();                                                // the volume: no writes after this
    { ce.read16(addr) } -> std::same_as<std::uint16_t>;         // memory-mapped half-word read (ownership proof)
    { e.erase(addr) } -> std::same_as<flash::Status>;           // the guarded store erase
    { e.blank(addr) } -> std::same_as<bool>;                    // the whole sector reads 0xFF
    e.clear_backup();                                           // this firmware's backup registers only
};

template <Env E>
Result run(E& e) {
    Result r;
    e.unmount();

    // Legacy sectors that are ours, decided before anything is erased (bit i = kStoreBase + i sectors).
    std::uint64_t mine = 0;
    unsigned n_legacy = 0;
    auto rd = [&e](std::uint32_t a) -> std::uint16_t { return e.read16(a); };
    for (std::uint32_t i = 0; i < kLegacySectors; i++) {
        if (legacy::ours(flash::kStoreBase + i * flash::kSectorSize, rd)) {
            mine |= std::uint64_t{1} << i;
            n_legacy++;
        }
    }
    r.sectors = static_cast<std::uint16_t>(kFsSectors + n_legacy);

    // Visit order: the volume ascending, then the legacy sectors descending (the log's second sector is
    // proven by its first, so it goes first).
    auto at = [&](std::size_t k) -> std::uint32_t {
        if (k < kFsSectors) return fs_sector_addr(k);
        std::size_t left = k - kFsSectors;                       // 0 = the highest proven legacy sector
        for (std::uint32_t i = kLegacySectors; i-- > 0;) {
            if (!(mine >> i & 1)) continue;
            if (left-- == 0) return flash::kStoreBase + i * flash::kSectorSize;
        }
        return 0;
    };

    for (std::size_t k = 0; k < r.sectors; k++) {
        const std::uint32_t a = at(k);
        const flash::Status st = e.erase(a);
        if (st != flash::Status::Ok) {
            r.err = Err::EraseFailed;
            r.first_fail = a;
            r.status = st;
            return r;
        }
        r.erased++;
    }
    for (std::size_t k = 0; k < r.sectors; k++) {
        const std::uint32_t a = at(k);
        if (e.blank(a)) {
            r.verified++;
        } else {
            if (r.failed == 0) r.first_fail = a;
            r.failed++;
        }
    }
    if (r.failed != 0) {
        r.err = Err::VerifyFailed;
        return r;
    }
    e.clear_backup();
    return r;
}

}  // namespace k2::factory_wipe
