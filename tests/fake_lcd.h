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
// The panel as it was at the FIRST lcd::pump() after g_pump_snap_armed was set (what the screen shows while a slow encode runs).
extern uint16_t g_pump_panel[LCD_WIDTH * LCD_HEIGHT];
extern bool g_pump_snap_armed;
// What the drawing code asked of a screenshot sink (set_sink), counted at the lcd stream calls: the work a capture does per
// strip. A text row is one pixel call, so pixel_calls is also the number of rendered glyph rows.
struct SinkWork {
    unsigned begins = 0, pixel_calls = 0, fill_calls = 0;
    unsigned long long pixels = 0, fill_pixels = 0;
};
extern SinkWork g_sink_work;
