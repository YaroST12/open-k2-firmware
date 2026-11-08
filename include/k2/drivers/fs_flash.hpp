// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The file system on the real flash: littlefs over 0x080B3000..0x080E0000 (90 blocks of 2 KB, k2/drivers/flash.hpp).
// The volume ends below the native device storage area (0x080E0000..0x080FFEFF), which
// this code never writes. init() mounts it at boot; a mount failure (first boot of this firmware, a volume of
// the old 96-block layout, or a damaged volume) formats it, logs the event and carries on. Formatting touches
// only 0x080B3000..0x080DFFFF. Every store reaches its files through instance().
#pragma once
#include "k2/core/fs.hpp"

namespace k2::fs {

inline constexpr std::uint32_t kFlashBlocks = 90;

Fs& instance();                 // the one volume; usable once init() has run
Err init(bool& formatted);      // mount, else format and mount
bool formatted_at_boot();       // init() had to format

// One-time cleanup of the pre-littlefs raw stores: erases exactly the pages of flash::kStoreBase..kStoreEnd
// that carry the signature of one of our old stores (k2/core/legacy_proof.hpp), nothing else; other content in
// that range is left untouched. Call after the stores that import legacy pages (usbcfg, uiprefs) have run. A
// marker file /.legacy makes it run once; returns the number of sectors erased (0 when already done).
unsigned wipe_legacy();

}  // namespace k2::fs
