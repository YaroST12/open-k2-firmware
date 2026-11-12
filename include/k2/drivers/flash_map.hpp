// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The flash map in one place: every region the firmware reads or writes above the application,
// checked at compile time for overlap. Each module keeps its own address constant; this header only
// lists them and asserts that the regions are sector aligned, ordered and disjoint, that the file system
// is the only writable one, that the legacy rows lie inside the legacy candidate range of k2::flash, and
// that the native rows lie outside both. The table is documented in docs/ARCHITECTURE.md ("Flash map").
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/drivers/flash.hpp"
#include "k2/pd/store.hpp"
#include "k2/ui/uiprefs.hpp"
#include "k2/drivers/usbcfg.hpp"

namespace k2::flashmap {

// What the firmware may do to a region:
//   Fs        the littlefs volume: erase and program
//   Legacy    an old raw store of this project: read, and erase of a page only when it carries that store's
//             signature (k2/core/legacy_proof.hpp); never programmed
//   Native    the device storage that was on the K2 before this firmware: read only, never erased or programmed
enum class Access : std::uint8_t { Fs, Legacy, Native };

struct Region {
    const char *name;
    std::uint32_t begin;
    std::uint32_t end;       // one past the last byte
    Access access;
};

inline constexpr std::uint32_t kBbLegacyBase = 0x080E8000;      // the old raw Black Box ring, now /bb
inline constexpr std::uint32_t kJournalLegacyBase = 0x080F8000;   // the old raw charger journal, now /jr
inline constexpr std::uint32_t kEmLegacyBase = 0x080FC000;   // the old raw e-marker library, now /emk on littlefs
inline constexpr std::uint32_t kCardsLegacyBase = 0x080E7000, kCardsLegacyEnd = 0x080E8000;   // the old raw report cards, now /cards
inline constexpr std::uint32_t kUsbCfgEnd = usbcfg::kLegacyAddr + flash::kSectorSize;
inline constexpr std::uint32_t kNativeSettings = 0x080E0000;   // the native user settings, up to the e-marker records

// Ascending address order.
inline constexpr std::array<Region, 11> kRegions{{
    {"littlefs", flash::kFsBase, flash::kFsEnd, Access::Fs},
    {"native settings", kNativeSettings, store::kSingleAddr, Access::Native},
    {"native single e-marker", store::kSingleAddr, store::kSingleAddr + flash::kSectorSize, Access::Native},
    {"native e-marker library", store::kLibraryAddr, store::kLibraryAddr + flash::kSectorSize, Access::Native},
    {"log snapshot + capture area (legacy)", flash::kStoreBase, usbcfg::kLegacyAddr, Access::Legacy},
    {"USB config (legacy)", usbcfg::kLegacyAddr, kUsbCfgEnd, Access::Legacy},
    {"UI preferences (legacy)", uiprefs::kLegacyAddr, uiprefs::kLegacyAddr + flash::kSectorSize, Access::Legacy},
    {"report cards (legacy)", kCardsLegacyBase, kCardsLegacyEnd, Access::Legacy},
    {"Black Box ring (legacy)", kBbLegacyBase, kJournalLegacyBase, Access::Legacy},
    {"charger journal (legacy)", kJournalLegacyBase, kEmLegacyBase, Access::Legacy},
    {"e-marker library (legacy)", kEmLegacyBase, flash::kStoreEnd, Access::Legacy},
}};

constexpr bool regions_ok() {
    for (std::size_t i = 0; i < kRegions.size(); i++) {
        const Region &r = kRegions[i];
        if (r.begin % flash::kSectorSize != 0 || r.end % flash::kSectorSize != 0 || r.begin >= r.end) return false;
        if (i > 0 && kRegions[i - 1].end > r.begin) return false;   // ordered, no overlap
        switch (r.access) {
        case Access::Fs:
            if (r.begin < flash::kFsBase || r.end > flash::kFsEnd) return false;
            break;
        case Access::Legacy:
            if (r.begin < flash::kStoreBase || r.end > flash::kStoreEnd) return false;
            break;
        case Access::Native:   // outside the file system and outside the legacy candidates
            if (r.begin < flash::kNativeBase || r.end > flash::kStoreBase) return false;
            break;
        }
    }
    return true;
}
static_assert(kBbLegacyBase + 32 * flash::kSectorSize == kJournalLegacyBase,
              "the charger journal sits between the Black Box ring and the legacy e-marker library");
static_assert(kCardsLegacyBase == uiprefs::kLegacyAddr + flash::kSectorSize && kCardsLegacyEnd == kBbLegacyBase,
              "the report cards fill the gap between the UI preferences and the Black Box ring");
static_assert(kBbLegacyBase == legacy::kBlackBoxBase && kJournalLegacyBase == legacy::kJournalBase && kEmLegacyBase == legacy::kEmLibBase &&
              kCardsLegacyBase == legacy::kCardsBase && usbcfg::kLegacyAddr == legacy::kLegacyUsbCfg && uiprefs::kLegacyAddr == legacy::kLegacyUiPrefs,
              "the map and the ownership proof describe the same legacy stores");
static_assert(regions_ok(), "flash regions overlap, are misaligned or leave their range");

// The grown volume's run (k2::flash::fs_block_addr, blocks 90..144). It lies over the legacy rows above: on a unit whose
// run held our old raw stores, the legacy wipe empties it first, and the volume grows into it only once every page reads
// blank (k2/core/fs_grow.hpp); on any other content the volume stays at 90 blocks. Not part of kRegions, which stays
// disjoint.
inline constexpr Region kFsRun{"littlefs grown run", flash::kFsRunBase, flash::kFsRunEnd, Access::Fs};
inline constexpr Region kKeptRecordPage{"kept: old e-marker record page", flash::kKeptPages[0], flash::kKeptPages[0] + flash::kSectorSize, Access::Native};
inline constexpr Region kKeptTopPage{"kept: top page, unknown owner", flash::kKeptPages[1], flash::kKeptPages[1] + flash::kSectorSize, Access::Native};
static_assert(kFsRun.begin == kRegions[4].begin, "the run starts with the legacy candidate range");
static_assert(kFsRun.end == kKeptTopPage.begin && kKeptTopPage.end == flash::kStoreEnd, "the run ends below the kept top page");
static_assert(kKeptRecordPage.begin == store::kLibraryAddr, "the kept record page is the native e-marker library page");
static_assert(kKeptRecordPage.end <= kFsRun.begin && kRegions[0].end <= kKeptRecordPage.begin, "the kept record page is in no volume range");
static_assert(flash::fs_block_addr(flash::kFsBlocks - 1) + flash::kSectorSize == kRegions[0].end &&
                  flash::fs_block_addr(flash::kFsBlocks) == kFsRun.begin &&
                  flash::fs_block_addr(flash::kFsGrownBlocks - 1) + flash::kSectorSize == kFsRun.end,
              "blocks 0..89 fill the littlefs row, blocks 90..144 the run");
// The run in the ring layout: littlefs blocks 90..110, then the report-card ring and the Black Box ring (k2/core/ringlog.hpp)
// up to the kept top page. All three are run sectors, writable only with the run open.
inline constexpr Region kCardRing{"report-card ring", flash::kCardRingBase, flash::kBbRingBase, Access::Fs};
inline constexpr Region kBbRing{"Black Box ring", flash::kBbRingBase, flash::kFsRunEnd, Access::Fs};
static_assert(kCardRing.begin == flash::fs_block_addr(flash::kFsVolumeBlocks - 1) + flash::kSectorSize && kCardRing.begin > kFsRun.begin &&
                  kCardRing.end == kBbRing.begin && kBbRing.end == kFsRun.end,
              "the rings follow the volume's last block, back to back, up to the end of the run");
static_assert(kRegions.front().begin == flash::kFsBase && kRegions.back().end == flash::kStoreEnd, "the map covers the flash up to its end");
static_assert(kRegions[1].begin == flash::kFsEnd, "the native area starts where the file system ends");

}  // namespace k2::flashmap
