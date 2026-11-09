// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include <stdint.h>
#include "k2/drivers/lcd.hpp"
inline constexpr int LCD_WIDTH = k2::lcd::kWidth, LCD_HEIGHT = k2::lcd::kHeight;
extern uint16_t g_panel[LCD_WIDTH * LCD_HEIGHT];
extern unsigned g_lcd_windows;   // stream_begin calls: a steady screen must not add to it
extern unsigned g_lcd_pumps;     // lcd::pump() calls (the QR encoder's poll hook)
extern unsigned g_lcd_pixels;    // pixels actually sent to the panel (not screenshot sinks)
