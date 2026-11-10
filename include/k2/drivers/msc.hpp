// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// USB mass-storage glue (Drive modes): the TinyUSB MSC callbacks served from k2::drive::Drive, a
// read-only FAT16 volume synthesized per sector read. No state is held in RAM besides the drive's
// file table; poll() rebuilds it after a store change (k2::storage::generation()).
#pragma once

namespace k2::msc {

// Build the drive and load the stored records. Call once, after usb::init().
void init();

// Rebuild the volume when the flash store changed. Call from the main loop.
void poll();

// Serve the READ10 TinyUSB handed over (the callback only stashes it, see k2/drivers/msc_async.hpp): read the
// sectors and complete the transfer. Call from the top of the main loop, never from inside usb::service() or
// anything nested under a paint: rendering a file sector is ~800 B of stack that would sit on top of the page.
// True when a read was completed (the caller then runs usb::poll() again to send the data).
bool io();

}  // namespace k2::msc
