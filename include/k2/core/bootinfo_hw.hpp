// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Collects the boot-info Snapshot (k2/core/bootinfo.hpp) from the drivers' own APIs. RAM only; nothing here touches a bus.
#pragma once
#include <cstdint>

#include "k2/core/bootinfo.hpp"
#include "k2/core/bootmode.hpp"

namespace k2::bootinfo {

// First thing after the keys were read (src/core/main.cpp sample_boot_mode): the raw key mask, the raw levels of PA9 / PA13 / PB14 / PA14
// (kPa*, 1 = high = released) and the decided mode. Reads nothing; the values are kept as given.
void note_boot(bootmode::Mode mode, std::uint8_t keys, std::uint8_t pa);

// The key that put this boot in its mode (OK for OkRdCc1, PLUS for PlusRdCc2) is down right now. False for a plain boot.
bool boot_key_held();

// Everything the text needs, from this boot's records and the live readings.
void collect(Snapshot& s);

}  // namespace k2::bootinfo
