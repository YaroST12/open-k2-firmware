// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The boot-info screen with live readings: collects the Snapshot from the drivers and draws one frame (k2/ui/bootscreen.hpp).
#pragma once
#include <cstdint>

namespace k2::bootscreen {

void draw_live(std::uint32_t frame);

}  // namespace k2::bootscreen
