// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress saved results on the littlefs volume, see k2/pdstress/pdstress_store_hw.hpp.
#include "k2/pdstress/pdstress_store_hw.hpp"

#include "k2/core/log.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/systick.hpp"

namespace k2::pdstress::store::hw {

Err put(fs::Fs::Fill fill, void* ctx, std::uint16_t& id) {
    const std::uint32_t t0 = systick::millis();
    id = 0;
    const Err e = Store{fs::instance()}.put(fill, ctx, id);
    K2LOG("[PS] result save id=%u err=%u ms=%u\n", static_cast<unsigned>(id), static_cast<unsigned>(e),
          static_cast<unsigned>(systick::millis() - t0));
    return e;
}

std::size_t list(std::span<std::uint16_t> out) { return Store{fs::instance()}.list(out); }

std::size_t read(std::uint16_t id, std::size_t off, std::span<std::uint8_t> out) { return Store{fs::instance()}.read(id, off, out); }

Err remove(std::uint16_t id) { return Store{fs::instance()}.remove(id); }

}  // namespace k2::pdstress::store::hw
