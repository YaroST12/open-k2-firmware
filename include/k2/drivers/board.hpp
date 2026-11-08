// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Board-level GPIOs whose function is not known yet. PB13 is driven low once at start-up and never
// touched again; PB15 is left floating (analog). Move each pin into its owning driver once its
// function is known.
#pragma once

namespace k2::board {

// Enable the GPIOB clock, drive PB13 low as a push-pull output, leave PB15 as an analog (floating) input.
void init();

}  // namespace k2::board
