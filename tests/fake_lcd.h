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

// Optional paint timing, off (nullptr) for every test but the world replay (tests/test_world_replay.cpp). When set, the fake
// spends virtual time the way src/drivers/lcd.cpp does on the panel: runs of kDmaMinPixels or more go out by "DMA" (a fill
// returns with its transfer in flight, the next call waits for it), the idle hook runs on the driver's elapsed-time cadence
// (lcd_timing.hpp ServicePacer, kServiceUs since the last round ended: checked after every call's pixels and while a DMA
// wait spins), and every pixel costs its wire time. The CPU work that produces the pixels (glyph rendering, layout) is not
// on the host clock, so it is charged per call and per streamed pixel. Screenshot sink calls cost nothing (they never touch
// the panel). lcd::pump() runs the idle hook only while timing is on.
struct LcdTiming {
    void (*spend_us)(double us);   // advance the virtual clock (the world serves its own events inside)
    double (*now_us)();
    double px_us;                  // wire time of one pixel (lcd_timing.hpp wire_us at the shipped divider)
    double call_us;                // CPU per stream_* call (the driver's own bookkeeping)
    double window_us;              // per stream_begin: CASET / RASET / RAMWR in 8-bit frames
    double px_cpu_us;              // CPU per stream_pixels pixel: rendering the run the call pushes
    // True = serial DMA (stream_pixels waits for its own transfer, stream_begin sends CASET/RASET/RAMWR on entry,
    // stream_end waits for the last fill: pre-ping-pong driver); false = overlapped ping-pong + lazy windows (lcd.cpp).
    bool serial = false;
};
extern const LcdTiming *g_lcd_timing;
using LcdIdleHook = void (*)();
LcdIdleHook lcd_idle_hook();   // what lcd::set_idle_hook() stored (nullptr until the firmware sets one)
