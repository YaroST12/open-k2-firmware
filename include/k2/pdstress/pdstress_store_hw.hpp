// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress saved results bound to the K2's volume (src/pdstress/pdstress_store_hw.cpp); see k2/pdstress/pdstress_store.hpp for what
// each call does. Main loop only. A put is one ~1.4 KB file commit and holds the main loop like a journal write (5 to 250 ms), so the
// page calls it only on the user's Save, never while a run is active. The host tests bind the same calls to a RAM disk
// (tests/ui_stubs.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pdstress/pdstress_store.hpp"

namespace k2::pdstress::store::hw {

Err put(fs::Fs::Fill fill, void* ctx, std::uint16_t& id);
std::size_t list(std::span<std::uint16_t> out);                                     // newest first
std::size_t read(std::uint16_t id, std::size_t off, std::span<std::uint8_t> out);
Err remove(std::uint16_t id);

}  // namespace k2::pdstress::store::hw
