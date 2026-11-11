// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Internal flash controller driver, restricted to ONE job: invalidating the application's
// bootloader marker so the bootloader stays in DFU after the next reset.
//
// The write path is an allow-list. The only addresses that can ever reach the controller are
// the two half-words of the marker word below, and the only value is 0x0000 (a pure bit-clear,
// so no erase is involved and the page can always be programmed 1 -> 0). Everything else,
// including the bootloader region 0x08000000..0x080037FF, is refused in code (Status::Refused)
// before any register is touched. The policy and the guarded programmer are plain templates
// over a tiny hardware seam so host tests can check them (tests/test_flash.cpp).
#pragma once
#include <array>
#include <concepts>
#include <cstdint>
#include <span>

#include "k2/core/legacy_proof.hpp"

namespace k2::flash {

inline constexpr std::uint32_t kBootloaderEnd = 0x08003800;  // the bootloader owns everything below
inline constexpr std::uint32_t kPageBase = 0x08004000;       // the 2 KB page holding all marker words
inline constexpr std::uint32_t kPageEnd = 0x08004800;

// The word that gets cleared. Currently the model signature "K200" at 0x08004010.
// Alternative (change this one constant, nothing else): the commit marker 0x08004400.
inline constexpr std::uint32_t kDfuMarkerAddr = 0x08004010;
inline constexpr std::uint16_t kClearValue = 0x0000;

// The two half-words of the marker word are the whole allow-list.
inline constexpr std::array<std::uint32_t, 2> kAllowed{kDfuMarkerAddr, kDfuMarkerAddr + 2};

static_assert(kDfuMarkerAddr % 4 == 0, "marker must be word aligned");
static_assert(kAllowed[0] >= kBootloaderEnd + 0x800, "the allow-list must stay clear of the bootloader and the vector table");
static_assert(kAllowed[0] >= kPageBase && kAllowed[1] + 2 <= kPageEnd, "the marker must sit in the app meta page");
static_assert(kClearValue == 0, "only pure bit-clears are allowed (no erase needed)");

constexpr bool allowed(std::uint32_t addr, std::uint16_t value) {
    if (value != kClearValue) return false;
    for (std::uint32_t a : kAllowed)
        if (a == addr) return true;
    return false;
}

enum class Status : std::uint8_t {
    Ok,
    Refused,        // not on the allow-list: nothing was written
    Busy,           // controller still busy / clock not ready after the timeout
    KeyFail,        // unlock sequence did not release the controller
    ProgramError,   // STS reported a program error (PRGMFLR)
    WriteProtect,   // STS reported a write-protect error (EPPERR)
    VerifyFailed,   // controller reported success but the half-word reads back differently
};

// The hardware seam: program one half-word, read one half-word.
template <class Hw>
concept HwOps = requires(Hw& hw, std::uint32_t addr, std::uint16_t v) {
    { hw.program(addr, v) } -> std::same_as<Status>;
    { hw.read(addr) } -> std::convertible_to<std::uint16_t>;
};

// Programs one allow-listed half-word and checks the readback.
template <HwOps Hw>
Status program_guarded(Hw& hw, std::uint32_t addr, std::uint16_t value) {
    if (!allowed(addr, value)) return Status::Refused;
    const Status st = hw.program(addr, value);
    if (st != Status::Ok) return st;
    return hw.read(addr) == value ? Status::Ok : Status::VerifyFailed;
}

// Clears the whole marker word (both half-words); stops at the first failure.
template <HwOps Hw>
Status clear_marker(Hw& hw) {
    for (std::uint32_t a : kAllowed) {
        const Status st = program_guarded(hw, a, kClearValue);
        if (st != Status::Ok) return st;
    }
    return Status::Ok;
}

// ---- Store regions --------------------------------------------------------------------------
//
// A second, separate policy for the app-owned stores (the marker allow-list above is unchanged):
//   file system  0x080B3000..0x080E0000  180 KB, 90 blocks of 2 KB: littlefs (k2/core/fs.hpp); erase and program
//   native       0x080E0000..0x08100000  128 KB: the device storage that was on the K2 before this firmware (settings,
//                e-marker records and other data). NEVER programmed, except the grown volume's run once it is
//                open (below). An erase is allowed only for a page proven to be one of OUR old raw stores
//                (k2/core/legacy_proof.hpp: the page carries that store's signature) inside the legacy candidate
//                range below, or for a run sector once the run is open.
//   legacy       0x080E4000..0x08100000  the candidate range of those old stores (56 sectors)
//   run          0x080E4000..0x080FF800  the grown volume's blocks 90..144 (55 sectors), closed until proven (below)
// The native pages below 0x080E4000 (settings 0x080E0000, e-marker records 0x080E3000 and 0x080E3800) are
// outside every erasable range, and the kept pages 0x080E3800 and 0x080FF800 are refused whatever they hold. The
// bootloader, the 0x08003000 info page, the vector table and the application stay unreachable too: anything not
// allowed is refused (Status::Refused) before a register is touched.
inline constexpr std::uint32_t kFsBase = 0x080B3000, kFsEnd = 0x080E0000;
inline constexpr std::uint32_t kNativeBase = 0x080E0000, kNativeEnd = 0x08100000;
inline constexpr std::uint32_t kStoreBase = legacy::kBase;   // legacy candidate range, erasable only per proof
inline constexpr std::uint32_t kStoreEnd = legacy::kEnd;
inline constexpr std::uint32_t kSectorSize = 0x800;          // 2 KB erase unit

static_assert(kFsBase % kSectorSize == 0 && kFsEnd % kSectorSize == 0, "region is sector aligned");
static_assert(kStoreBase % kSectorSize == 0 && kStoreEnd % kSectorSize == 0, "region is sector aligned");
static_assert(legacy::kSector == kSectorSize, "the proof and the controller use the same sector size");
static_assert(kFsBase > kPageEnd + 0x1000, "the store region must stay far above the application meta page");
static_assert(kFsEnd == kNativeBase, "the file system ends exactly where the native storage area starts");
static_assert(kStoreBase >= kNativeBase + 0x4000, "the native settings and e-marker pages 0x080E0000..0x080E3FFF are never candidates");
static_assert(kStoreEnd == kNativeEnd, "the legacy range ends at the end of the flash");

// ---- The grown volume ------------------------------------------------------------------------
//
// The volume may grow from kFsBlocks to kFsGrownBlocks into the run 0x080E4000..0x080FF7FF (55 sectors, 110 KB) at
// the top of the native area: blank on the units seen so far. Blocks 0..89 stay in kFsBase..kFsEnd; block 90 + i is
// the run's sector i. The run is writable only after the firmware has OPENED it (open_fs_run() below): at boot,
// when the volume's superblock already says kFsGrownBlocks, or when every page of the run read blank (or the grow
// intent survived a power cut, k2/core/fs_grow.hpp). Until then every erase or program there is refused like the rest
// of the native area. Two pages are kept forever, whatever they hold and whatever proof might claim them:
//   0x080E3800  the old e-marker record page (native)
//   0x080FF800  the top page, "5A 00 00 A5" at its start on this unit: unknown owner
// and the original-data pages 0x080E0000..0x080E3FFF are never a block.
inline constexpr std::uint32_t kFsBlocks = (kFsEnd - kFsBase) / kSectorSize;           // 90
inline constexpr std::uint32_t kFsRunBase = 0x080E4000, kFsRunEnd = 0x080FF800;
inline constexpr std::uint32_t kFsRunBlocks = (kFsRunEnd - kFsRunBase) / kSectorSize;  // 55
inline constexpr std::uint32_t kFsGrownBlocks = kFsBlocks + kFsRunBlocks;              // 145
inline constexpr std::array<std::uint32_t, 2> kKeptPages{0x080E3800, 0x080FF800};

// Address of volume block i, 0 past the grown end.
constexpr std::uint32_t fs_block_addr(std::uint32_t i) {
    if (i < kFsBlocks) return kFsBase + i * kSectorSize;
    if (i < kFsGrownBlocks) return kFsRunBase + (i - kFsBlocks) * kSectorSize;
    return 0;
}
// True for any address inside one of the kept pages.
constexpr bool kept_page(std::uint32_t addr) {
    for (std::uint32_t p : kKeptPages)
        if (addr >= p && addr < p + kSectorSize) return true;
    return false;
}

constexpr bool fs_block_map_ok() {
    for (std::uint32_t i = 0; i < kFsGrownBlocks; i++) {
        const std::uint32_t a = fs_block_addr(i);
        if (a == 0 || a % kSectorSize != 0 || kept_page(a)) return false;
        if (a < kFsBase || a >= kNativeEnd) return false;                     // never the application or past the flash
        if (a >= kNativeBase && a < kNativeBase + 0x4000) return false;       // never the original-data pages
        if (i > 0 && a <= fs_block_addr(i - 1)) return false;                 // ascending, no block twice
    }
    return fs_block_addr(kFsGrownBlocks) == 0;
}
static_assert(kFsRunBase % kSectorSize == 0 && kFsRunEnd % kSectorSize == 0, "the run is sector aligned");
static_assert(kFsBlocks == 90 && kFsRunBlocks == 55 && kFsGrownBlocks == 145);
static_assert(kFsRunBase == kNativeBase + 0x4000, "the run starts right above the original-data pages");
static_assert(kFsRunEnd == kKeptPages[1] && kKeptPages[1] + kSectorSize == kNativeEnd, "the run ends below the kept top page");
static_assert(kKeptPages[0] < kFsRunBase && kKeptPages[0] >= kFsEnd, "the old e-marker record page is outside both ranges");
static_assert(fs_block_map_ok(), "every block is a sector of its own, outside the kept and original-data pages");

// A sector of the run (whether the run is open is the caller's business).
constexpr bool fs_run_sector(std::uint32_t addr) {
    return addr % kSectorSize == 0 && addr >= kFsRunBase && addr < kFsRunEnd;
}

// A half-word aligned span of `len` bytes (a multiple of 2) fully inside the file system range, or fully inside
// the run when it is open (programming is allowed nowhere else).
constexpr bool store_allowed(std::uint32_t addr, std::uint32_t len, bool run_open = false) {
    if (addr % 2 != 0 || len % 2 != 0) return false;
    if (addr >= kFsBase && addr < kFsEnd) return len <= kFsEnd - addr;
    if (run_open && addr >= kFsRunBase && addr < kFsRunEnd) return len <= kFsRunEnd - addr;
    return false;
}
// A sector of the file system: erased and programmed freely (it is ours). With the run open, a run sector too.
constexpr bool fs_sector(std::uint32_t addr, bool run_open = false) {
    return (addr % kSectorSize == 0 && addr >= kFsBase && addr < kFsEnd) || (run_open && fs_run_sector(addr));
}
// A sector of the legacy candidate range: erasable only when the content proves it is one of our old stores.
constexpr bool legacy_candidate(std::uint32_t addr) {
    return addr % kSectorSize == 0 && addr >= kStoreBase && addr < kStoreEnd;
}
// The static part of the erase policy, without the content proof (used where no content is at hand).
constexpr bool store_sector_allowed(std::uint32_t addr) { return fs_sector(addr); }

// The hardware seam of the store: HwOps plus a sector erase.
template <class Hw>
concept StoreHwOps = HwOps<Hw> && requires(Hw& hw, std::uint32_t addr) {
    { hw.erase(addr) } -> std::same_as<Status>;
};

// The full erase decision: a file system sector (a run sector when the run is open), or a legacy candidate sector
// whose content proves it is ours. Never a kept page. `rd(addr)` reads a half-word.
template <class Rd>
bool erase_allowed(std::uint32_t addr, Rd&& rd, bool run_open = false) {
    if (kept_page(addr)) return false;
    return fs_sector(addr, run_open) || (legacy_candidate(addr) && legacy::ours(addr, rd));
}

// Erases one allowed sector and checks that it reads back blank.
template <StoreHwOps Hw>
Status store_erase_guarded(Hw& hw, std::uint32_t addr, bool run_open = false) {
    if (!erase_allowed(addr, [&hw](std::uint32_t a) -> std::uint16_t { return hw.read(a); }, run_open)) return Status::Refused;
    const Status st = hw.erase(addr);
    if (st != Status::Ok) return st;
    for (std::uint32_t a = addr; a < addr + kSectorSize; a += 2)
        if (hw.read(a) != 0xFFFF) return Status::VerifyFailed;
    return Status::Ok;
}

// Programs `data` at a half-word aligned address; an odd last byte is padded with 0xFF. Half-words
// that are 0xFFFF are skipped (the sector is erased already). Everything is read back at the end,
// so a half-word that was not blank beforehand shows up as VerifyFailed.
template <StoreHwOps Hw>
Status store_program_guarded(Hw& hw, std::uint32_t addr, std::span<const std::uint8_t> data, bool run_open = false) {
    const std::uint32_t len = static_cast<std::uint32_t>((data.size() + 1) & ~std::size_t{1});
    if (!store_allowed(addr, len, run_open)) return Status::Refused;
    auto half = [&](std::size_t i) -> std::uint16_t {
        const std::uint16_t lo = data[i];
        const std::uint16_t hi = i + 1 < data.size() ? data[i + 1] : 0xFF;
        return static_cast<std::uint16_t>(lo | (hi << 8));
    };
    for (std::size_t i = 0; i < data.size(); i += 2) {
        const std::uint16_t v = half(i);
        if (v == 0xFFFF) continue;
        const Status st = hw.program(addr + static_cast<std::uint32_t>(i), v);
        if (st != Status::Ok) return st;
    }
    for (std::size_t i = 0; i < data.size(); i += 2)
        if (hw.read(addr + static_cast<std::uint32_t>(i)) != half(i)) return Status::VerifyFailed;
    return Status::Ok;
}

// ---- Real controller (src/drivers/flash.cpp) --------------------------------------------------------

// Programs the marker word to zero on the real flash. Interrupts are off for the few tens of
// microseconds of each program; the programming code itself runs from RAM.
Status clear_marker();

// True while the marker word still holds a non-zero half-word (the app would still be recognised).
bool marker_present();

// Store region on the real flash (policy above). Interrupts are off for each program (~50 us)
// and each erase (a few tens of ms); the controller code runs from RAM. Bank 2 (>= 0x08080000)
// uses its own register set.
Status store_erase(std::uint32_t addr);
Status store_program(std::uint32_t addr, std::span<const std::uint8_t> data);
// Opens the run kFsRunBase..kFsRunEnd to store_erase/store_program for the rest of this boot. Only the volume's boot
// sequence calls it, and only on proof (k2/core/fs_grow.hpp); there is no way to close it again short of a reset.
void open_fs_run();
bool fs_run_open();
const std::uint8_t* store_view(std::uint32_t addr);       // memory-mapped read access
std::uint8_t store_bank();                                // register set that worked: 2, 1, or 0 = unknown

}  // namespace k2::flash
