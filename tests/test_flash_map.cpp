// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The flash regions of every storage module must be disjoint; k2/drivers/flash_map.hpp asserts it at compile
// time, this test adds the addresses the docs promise.
#include <cstdio>

#include "k2/drivers/flash_map.hpp"
#include "test_util.h"

using namespace k2;

namespace {

std::uint16_t blank_rd(std::uint32_t) { return 0xFFFF; }
// A reader that shows our old e-marker library signature "K2EL" at the start of every sector.
std::uint16_t emlib_rd(std::uint32_t a) {
    if (a % flash::kSectorSize == 0) return 0x324B;   // "K2"
    if (a % flash::kSectorSize == 2) return 0x4C45;   // "EL"
    return 0xFFFF;
}

// The grown volume: blocks 0..89 where they always were, blocks 90..144 in the blank run 0x080E4000..0x080FF7FF
// (55 sectors: 0x1B800 bytes).
void test_grow_block_map() {
    CHECK_EQ(flash::kFsBlocks, 90u);
    CHECK_EQ(flash::kFsGrownBlocks, 145u);
    CHECK_EQ(flash::kFsRunBase, 0x080E4000u);
    CHECK_EQ(flash::kFsRunEnd, 0x080FF800u);
    CHECK_EQ(flash::fs_block_addr(0), 0x080B3000u);
    CHECK_EQ(flash::fs_block_addr(89), 0x080DF800u);
    CHECK_EQ(flash::fs_block_addr(90), 0x080E4000u);
    CHECK_EQ(flash::fs_block_addr(91), 0x080E4800u);
    CHECK_EQ(flash::fs_block_addr(144), 0x080FF000u);
    CHECK_EQ(flash::fs_block_addr(145), 0u);             // past the end: no address
    for (std::uint32_t i = 0; i < flash::kFsGrownBlocks; i++) {
        const std::uint32_t a = flash::fs_block_addr(i);
        CHECK_EQ(a % flash::kSectorSize, 0u);
        CHECK_EQ(a >= 0x080E0000u && a < 0x080E4000u, 0);   // the original-data pages are never a block
        CHECK_EQ(flash::kept_page(a), 0);
    }
    // The two kept pages: the old e-marker record page and the unknown page at the top.
    CHECK_EQ(flash::kept_page(0x080E3800), 1);
    CHECK_EQ(flash::kept_page(0x080E3802), 1);
    CHECK_EQ(flash::kept_page(0x080FF800), 1);
    CHECK_EQ(flash::kept_page(0x080FFFFE), 1);
    CHECK_EQ(flash::kept_page(0x080FF000), 0);
}

// The run is writable only once opened (a proven grow or a proven blank format); the kept pages and the original-data
// pages never are, not even with the run open or a signature of ours on them.
void test_run_guard() {
    for (std::uint32_t a : {0x080E4000u, 0x080F0000u, 0x080FF000u}) {
        CHECK_EQ(flash::fs_sector(a), 0);
        CHECK_EQ(flash::fs_sector(a, true), 1);
        CHECK_EQ(flash::store_allowed(a, 2), 0);
        CHECK_EQ(flash::store_allowed(a, 2, true), 1);
        CHECK_EQ(flash::erase_allowed(a, blank_rd), 0);
        CHECK_EQ(flash::erase_allowed(a, blank_rd, true), 1);
    }
    CHECK_EQ(flash::store_allowed(0x080FF000, 0x800, true), 1);
    CHECK_EQ(flash::store_allowed(0x080FF000, 0x802, true), 0);    // a span may not run into the kept top page
    CHECK_EQ(flash::store_allowed(0x080DF800, 0x1000, true), 0);   // nor across the gap from the main range
    for (std::uint32_t a : {0x080E0000u, 0x080E2800u, 0x080E3000u, 0x080E3800u, 0x080FF800u}) {
        CHECK_EQ(flash::fs_sector(a, true), 0);
        CHECK_EQ(flash::store_allowed(a, 2, true), 0);
        CHECK_EQ(flash::erase_allowed(a, blank_rd, true), 0);
        CHECK_EQ(flash::erase_allowed(a, emlib_rd, true), 0);
    }
    // A legacy e-marker sector below the kept page is still erasable on its proof; the kept page itself never.
    CHECK_EQ(flash::erase_allowed(0x080FF000, emlib_rd), 1);
    CHECK_EQ(flash::erase_allowed(0x080FF800, emlib_rd), 0);
}

}  // namespace

int main() {
    CHECK_EQ(store::kSingleAddr, 0x080E3000u);
    CHECK_EQ(store::kLibraryAddr, 0x080E3800u);
    CHECK_EQ(flash::kStoreBase, 0x080E4000u);
    CHECK_EQ(usbcfg::kLegacyAddr, 0x080E6000u);
    CHECK_EQ(uiprefs::kLegacyAddr, 0x080E6800u);
    CHECK_EQ(flashmap::kCardsLegacyBase, 0x080E7000u);
    CHECK_EQ(flashmap::kCardsLegacyEnd, 0x080E8000u);
    CHECK_EQ(flashmap::kBbLegacyBase, 0x080E8000u);
    CHECK_EQ(flashmap::kJournalLegacyBase, 0x080F8000u);
    CHECK_EQ(flashmap::kEmLegacyBase, 0x080FC000u);
    CHECK_EQ(flash::kStoreEnd, 0x08100000u);
    CHECK_EQ(flashmap::regions_ok(), 1);
    // The littlefs region: 90 blocks of 2 KB ending exactly where the native device storage starts, 180 KB.
    CHECK_EQ(flash::kFsBase, 0x080B3000u);
    CHECK_EQ(flash::kFsEnd, 0x080E0000u);
    CHECK_EQ(flash::kNativeBase, 0x080E0000u);
    CHECK_EQ((flash::kFsEnd - flash::kFsBase) / flash::kSectorSize, 90u);
    CHECK_EQ(flash::store_sector_allowed(flash::kFsBase), 1);
    CHECK_EQ(flash::store_sector_allowed(flash::kFsEnd - flash::kSectorSize), 1);
    CHECK_EQ(flash::store_allowed(flash::kFsBase - 2, 2), 0);        // application space below it
    CHECK_EQ(flash::store_allowed(flash::kFsEnd - 2, 4), 0);         // a span may not run into the native area
    // The native pages are not writable through the store guard, and not erasable either.
    for (std::uint32_t a : {0x080E0000u, 0x080E2800u, 0x080E3000u, 0x080E3800u}) {
        CHECK_EQ(flash::store_allowed(a, 2), 0);
        CHECK_EQ(flash::store_sector_allowed(a), 0);
        CHECK_EQ(flash::fs_sector(a), 0);
        CHECK_EQ(flash::legacy_candidate(a), 0);
    }
    CHECK_EQ(flash::fs_sector(0x080DF800), 1);
    CHECK_EQ(flash::legacy_candidate(0x080E4000), 1);                 // candidates need a content proof on top
    CHECK_EQ(flash::legacy_candidate(0x080FF800), 1);
    CHECK_EQ(flash::legacy_candidate(0x08100000), 0);
    CHECK_EQ(flash::store_sector_allowed(0x080E4000), 0);             // no static grant for the legacy range
    // No program ever lands at or above the native area.
    CHECK_EQ(flash::store_allowed(0x080E4000, 2), 0);
    CHECK_EQ(flash::store_allowed(0x080FF800, 2), 0);
    test_grow_block_map();
    test_run_guard();
    return test_report("test_flash_map");
}
