// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Live boot-info frame: the Snapshot comes from the drivers' records (src/core/bootinfo_hw.cpp).
#include "k2/ui/bootscreen_hw.hpp"

#include "k2/core/bootinfo_hw.hpp"
#include "k2/ui/bootscreen.hpp"

namespace k2::bootscreen {

// Its own frame, so the Snapshot (about 200 bytes) is popped before the main loop goes on.
[[gnu::noinline]] void draw_live(std::uint32_t frame) {
    bootinfo::Snapshot s;
    bootinfo::collect(s);
    draw(s, frame);
}

}  // namespace k2::bootscreen
