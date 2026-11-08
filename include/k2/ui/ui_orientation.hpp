// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

void draw_physical_arrow(const Surface& s, int16_t x, int16_t y, bool toward_top,
                         uint8_t rotation, uint16_t color);

// Idle marker for the same 16 x 16 cell: a short dash along the port axis (vertical at rotations 0/2).
void draw_physical_dash(const Surface& s, int16_t x, int16_t y, uint8_t rotation, uint16_t color);

void draw_physical_chevron(const Surface& s, int16_t x, int16_t y, bool toward_top,
                           uint8_t rotation, uint16_t color);
void draw_physical_flow_diagram(const Surface& s, int16_t x, int16_t y, int16_t h,
                               const FlowSpec& spec, uint8_t rotation);

}  // namespace k2
