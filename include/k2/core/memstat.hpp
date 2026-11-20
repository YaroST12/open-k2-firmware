// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Memory readout: RAM stack headroom (application and interrupt stacks), the static RAM budget, the littlefs volume and
// the firmware image against the application flash region. No heap exists, so nothing is reported for one.
//
// Where the numbers come from (src/drivers/memstat.cpp):
//   stack low-water  the painted-stack scan (startup.cpp paints both stacks at reset; the first dirty word marks the deepest
//                    excursion since boot), refreshed at most once a second from the main loop
//   stack now        the stack pointer at that same poll, against the MPU guard
//   RAM static       linker symbols (.data + .noinit + overlay + .bss)
//   image, flash     linker symbols (end of the loaded image, the FLASH region's length)
//   littlefs         Fs::used_blocks(), which walks the whole tree (tens of ms), so the poll only asks when the cached figure
//                    is stale, the link is quiet (the main loop's flash-erase gate) and 2 s passed since the last walk
// Everything here and in the UI, the remote and the shell reads the cached Snapshot; none of them scans or walks.
//
// The row texts and levels are pure, so the K2 screen, the shell and the host tests share one definition.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::memstat {

// Warnings only for what the user can act on (user decision 2026-11-14): the file storage, where cards, journals and saved
// cables can be deleted. The stack figures are plain numbers. Thresholds are the free share of the littlefs volume: amber
// under kFsCautionPct, red under kFsErrorPct (exclusive). Mirrored in tools/k2_remote.py and tools/web/k2proto.js.
inline constexpr std::uint32_t kFsCautionPct = 20, kFsErrorPct = 10;

enum class Level : std::uint8_t { Ok, Caution, Error };

constexpr Level level_for_fs(std::uint32_t free_blocks, std::uint32_t blocks) {
    if (blocks == 0) return Level::Ok;
    if (free_blocks > blocks) free_blocks = blocks;   // the volume is a few hundred blocks: 32-bit products cannot overflow
    const std::uint32_t f = free_blocks * 100;
    return f < blocks * kFsErrorPct ? Level::Error : f < blocks * kFsCautionPct ? Level::Caution : Level::Ok;
}

struct Snapshot {
    std::uint32_t ram_total = 0;       // SRAM bytes
    std::uint32_t ram_static = 0;      // .data + .noinit + overlay + .bss
    std::uint32_t psp_size = 0;        // application stack: _eguard.._psp_top
    std::uint32_t psp_now = 0;         // free bytes under the stack pointer at the last poll (main-loop depth)
    std::uint32_t psp_low = 0;         // free bytes at the deepest excursion since boot (painted-stack scan)
    std::uint32_t msp_size = 0;        // interrupt stack: _psp_top.._estack
    std::uint32_t msp_low = 0;         // free bytes at its deepest excursion since boot
    std::uint32_t fs_blocks = 0;       // littlefs volume size, in blocks
    std::uint32_t fs_used = 0;         // blocks in use (valid only when fs_valid)
    std::uint32_t image_bytes = 0;     // application flash start to the end of the loaded image
    std::uint32_t flash_bytes = 0;     // the application flash region
    std::uint16_t fs_block_bytes = 0;  // 2048
    bool fs_valid = false;             // fs_used has been measured since boot
    bool ok = false;                   // the poll has run once: the other fields mean something
    bool operator==(const Snapshot&) const = default;
};

// ---- the screen / shell rows ----------------------------------------------------------------------------------------

enum class Row : std::uint8_t { StackNow, StackLow, IrqLow, RamStatic, FsUsed, FsFree, Image, ImageFree, Heap, Count };
inline constexpr int kRows = static_cast<int>(Row::Count);
// Widest label 10 chars (font 12, 6 px a cell); widest value 16 chars (font 16, 8 px a cell): "266 KB (133 blk)", "62.9 of 96.0 KB".
inline constexpr std::size_t kLabelMax = 10, kValueMax = 16, kFooterMax = 27;

const char* row_label(Row r);
// Writes the value with its units; returns the length (never more than min(out.size(), kValueMax)). "--" while unknown.
std::size_t row_text(Row r, const Snapshot& s, std::span<char> out);
// Only the FS free row carries a level (exception colour only), from storage_level(); every other row is Ok.
Level row_level(Row r, const Snapshot& s);
// The storage level: Ok until the volume has been walked (fs_valid) and the poll has run.
Level storage_level(const Snapshot& s);

// The footer: a fact and its tone level: "Stack never below 7500 B" (Ok), "Storage low: delete cards" (Caution),
// "Storage full: delete cards" (Error).
std::size_t footer_text(const Snapshot& s, std::span<char> out);
Level footer_level(const Snapshot& s);

// Free bytes of the volume / of the application flash region, for the wire clients and the shell.
constexpr std::uint32_t fs_used_bytes(const Snapshot& s) { return s.fs_used * s.fs_block_bytes; }
constexpr std::uint32_t fs_free_blocks(const Snapshot& s) { return s.fs_used > s.fs_blocks ? 0 : s.fs_blocks - s.fs_used; }
constexpr std::uint32_t image_free_bytes(const Snapshot& s) { return s.image_bytes > s.flash_bytes ? 0 : s.flash_bytes - s.image_bytes; }

// ---- the device side (src/drivers/memstat.cpp; the host tests stub it, tests/ui_stubs.cpp) ----------------------------------

// Main loop, every slow pass. Refreshes the stack figures at most once a second; walks the littlefs tree only when `may_walk`
// (no port owner, quiet link: the same gate as a flash erase) and the cached figure is stale.
void poll(std::uint32_t now_ms, bool may_walk);
// The cached figures; a plain copy, no scan.
Snapshot snapshot();
// Non-destructive free-block scrub on the littlefs volume; returns true on success and sets erased.
bool scrub_storage(std::uint32_t& erased);

}  // namespace k2::memstat
