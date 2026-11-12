// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The file system on the real flash: littlefs over 0x080B3000..0x080E0000 (90 blocks of 2 KB, k2/drivers/flash.hpp),
// grown to 111 blocks into the run 0x080E4000..0x080FF7FF when that run is proven blank (k2/core/fs_grow.hpp); the rest of
// the run holds the report-card ring and the Black Box ring (k2/core/ringlog.hpp, card_ring()/bb_ring()). The volume and
// the rings never touch the original-data pages 0x080E0000..0x080E3FFF or the two kept pages (0x080E3800, 0x080FF800).
// init() mounts it at boot; a mount failure (first boot of this firmware, a volume of the old 96-block layout, or a
// damaged volume) formats it, at 111 blocks when the run reads blank, else at 90, logs the event and carries on. A volume
// of the layout before the rings (145 blocks) is emptied above block 111 and shrunk once (k2/core/fs_migrate.hpp) before
// any store reads it.
// Every store reaches its files through instance().
#pragma once
#include "k2/core/fs.hpp"
#include "k2/core/ringlog.hpp"
#include "k2/drivers/flash.hpp"

namespace k2::fs {

inline constexpr std::uint32_t kFlashBlocks = flash::kFsBlocks;         // 90
inline constexpr std::uint32_t kGrownBlocks = flash::kFsVolumeBlocks;   // 111
inline constexpr std::uint32_t kWideBlocks = flash::kFsGrownBlocks;     // 145: the layout before the rings, shrunk at boot

Fs& instance();                 // the one volume; usable once init() has run
Err init(bool& formatted);      // mount (a wide volume: move it into the ring layout), else format and mount
bool formatted_at_boot();       // init() had to format

// The ring logs in blocks 111..144 of the run. Usable once rings_ready(): the volume has 111 blocks and the run is open
// (grow() scans them at its end). Before that, or on a volume that stays at 90 or 145 blocks, they are not scanned and
// every append fails.
ringlog::Ring& card_ring();
ringlog::Ring& bb_ring();
bool rings_ready();

// One-time cleanup of the pre-littlefs raw stores: erases exactly the pages of flash::kStoreBase..kStoreEnd
// that carry the signature of one of our old stores (k2/core/legacy_proof.hpp), nothing else; other content in
// that range is left untouched. Call after the stores that import legacy pages (usbcfg, uiprefs) have run. A
// marker file /.legacy makes it run once; returns the number of sectors erased (0 when already done). Never runs on
// a grown volume (the run is ours then).
unsigned wipe_legacy();
// True once wipe_legacy() has finished (or the volume was formatted on a blank run): the legacy pages are gone and the
// run may be volume blocks, so nothing may import from the old raw addresses any more.
bool legacy_done();

// Grows the volume to 111 blocks when it is proven safe (k2/core/fs_grow.hpp); call once per boot after
// wipe_legacy(). Logs "[FS] grown 90 -> 111 blocks" or why it did not grow, then scans the rings when the volume has 111.
void grow();
// The run belongs to the volume this boot (grown, being grown, or formatted grown): the factory wipe erases it too.
bool grown();

}  // namespace k2::fs
