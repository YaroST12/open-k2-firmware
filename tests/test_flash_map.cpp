// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The flash regions of every storage module must be disjoint; k2/drivers/flash_map.hpp asserts it at compile
// time, this test adds the addresses the docs promise.
#include <cstdio>

#include "k2/drivers/flash_map.hpp"
#include "test_util.h"

using namespace k2;

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
    return test_report("test_flash_map");
}
