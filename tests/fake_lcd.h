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
// Overdraw meter (panel mode only, not screenshot sinks): per-pixel write counts since lcd_meter_reset(). The full-screen
// lcd::fill() (the clear before a repaint) is not metered, like the clear subtracted from the hardware readings.
extern unsigned g_meter_sent;      // pixels sent since the reset
extern unsigned g_meter_unique;    // distinct panel pixels written at least once since the reset
void lcd_meter_reset();
const uint8_t *lcd_meter_writes();   // per pixel (240*240, y * 240 + x): writes since the reset, saturating at 255
// What was written to one pixel row span more than once: rows y0..y1-1, x0..x1-1.
unsigned lcd_meter_rewritten(int x0, int y0, int x1, int y1);   // sum over the area of (writes - 1) for pixels written twice or more
void lcd_meter_trace_dump(const char *label);   // K2_OVERDRAW_TRACE=1: print and clear the rewrite call stacks
