// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// One-time move of a wide volume (145 blocks, the layout before the ring logs) into the ring layout: littlefs at
// flash::kFsVolumeBlocks (111) plus the report-card and Black Box rings in blocks 111..144 (k2/drivers/flash.hpp).
// littlefs spreads its blocks over the whole volume, so on a used unit the top 34 blocks hold live data, and it shrinks a
// volume only when its top blocks are unused. So the volume is emptied in place: under a fence at 111
// (k2::fs::Fs::fence: a program or an erase there answers as a bad block, and littlefs writes the data into another block)
// the kept files are rewritten and every metadata pair compacted until nothing at or above 111 is in use, then the
// superblock is shrunk to 111 (one atomic commit) and the ring range erased. Pure logic over k2::fs::Fs and the ring seam
// (ringlog::Io), host-tested with a power cut at every program and erase (tests/test_fs_migrate.cpp); the device binding
// is in src/drivers/fs_flash.cpp.
//
// migrate(), on a mounted wide volume, at boot before any store reads it:
//   1. fence at 111; delete /cards and /bb (their contents move to the rings, which start empty). The rest must fit 111
//      blocks with kMarginBlocks to spare (used_blocks()); if not, /tcap and /log.txt go too; if still not, Deferred: the
//      volume stays wide and the rings stay off. The journals are never dropped.
//   2. settle the leftovers of an interrupted run in every directory: a "<name>~" file is a copy cut short and is
//      removed (or, its original gone, which only the final rename does, renamed back).
//   3. while blocks at or above 111 are in use, one pass: every file larger than an inline record (the second pass:
//      every file, for a small file that kept a data block) is copied to "<name>~" and renamed over the original, then
//      every metadata pair is compacted twice (Fs::compact: the first round writes each pair's other block, the second
//      the block it left). Still in use after two passes: Stuck, the volume stays wide (tried again at the next boot).
//      No directory is moved or renamed: a rename across directories leaves a pending move in the global state, and a
//      cut there hides an entry until littlefs repairs it.
//   4. shrink to 111, fence off, erase the ring range.
// Power cuts: littlefs commits each step atomically and every step can run again (a copy rewrites the same bytes), so the
// next boot finds a wide volume with every kept file intact and runs migrate() again, or a volume of 111 whose ring range
// may still hold old littlefs blocks, which the rings read as dirty sectors and erase. A kept name must not end in kMark.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/fs.hpp"
#include "k2/core/ringlog.hpp"

namespace k2::fsmigrate {

inline constexpr std::uint32_t kMarginBlocks = 8;   // free blocks the kept files must leave in the 111-block volume
inline constexpr char kMark = '~';                  // suffix of a file's copy while it is written
inline constexpr const char* kDropped[] = {"/cards", "/bb"};             // never kept
inline constexpr const char* kDroppedIfTight[] = {"/tcap", "/log.txt"};  // dropped only when the rest does not fit

enum class Outcome : std::uint8_t { None, Migrated, Deferred, Stuck, Failed };

struct Result {
    Outcome outcome = Outcome::None;
    bool tight = false;             // the kDroppedIfTight paths went
    bool resumed = false;           // leftovers of an interrupted run were found
    unsigned passes = 0;            // rewrite and compaction passes (0: nothing above 111 was in use)
    unsigned files = 0;             // files rewritten
    std::uint32_t used_blocks = 0;  // the volume's used blocks after the deletions
    std::uint32_t above = 0;        // blocks at or above 111 in use before the rewrite (Stuck: after it)
    unsigned erased = 0;            // ring-range sectors erased after the shrink
    fs::Err err = fs::Err::Ok;      // Failed: the first file-system error
};

// Steps 1-3 on a mounted wide volume (None for any other volume). `buf`: the copy buffer (at least 256 bytes). The run
// must be open (the volume writes blocks >= 90, the erase covers the ring range).
Result migrate(fs::Fs& f, ringlog::Io& io, std::span<std::uint8_t> buf);

}  // namespace k2::fsmigrate
