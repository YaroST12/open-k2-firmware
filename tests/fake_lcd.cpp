// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host stand-in for src/drivers/lcd.cpp: the same sink diversion, with a 240x240 "panel" RAM behind it.
#include "fake_lcd.h"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/ui/pixel_sink.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <dlfcn.h>
#include <execinfo.h>

uint16_t g_panel[LCD_WIDTH * LCD_HEIGHT];
unsigned g_lcd_windows;
unsigned g_lcd_pixels;
unsigned g_lcd_pumps;
uint16_t g_pump_panel[LCD_WIDTH * LCD_HEIGHT];
bool g_pump_snap_armed;
SinkWork g_sink_work;
unsigned g_meter_sent;
unsigned g_meter_unique;

namespace {
const k2::lcd::Sink* g_sink;
k2::PixelSink g_panel_sink;
bool g_panel_attached;
bool g_erase;
bool g_clearing;   // inside lcd::fill(): not metered
std::uint8_t g_writes[LCD_WIDTH * LCD_HEIGHT];
unsigned g_wx0, g_wy0, g_wx1, g_wy1, g_cx, g_cy;   // the window and write pointer, as the controller keeps them

// Call-site attribution (K2_OVERDRAW_TRACE=1): the stack at every window start, with the pixels sent and rewritten under it.
constexpr int kFrames = 16;
using Stack = std::array<void *, kFrames>;
struct TraceCount { unsigned sent = 0, rewritten = 0; };
std::map<Stack, TraceCount> g_trace;
Stack g_stack{};
bool g_trace_on = std::getenv("K2_OVERDRAW_TRACE") != nullptr;

// Counts `count` pixels written from the write pointer, which wraps inside the window.
void meter_write(std::uint32_t count) {
    if (g_clearing) return;
    g_meter_sent += count;
    TraceCount *tc = g_trace_on ? &g_trace[g_stack] : nullptr;
    if (tc) tc->sent += count;
    for (; count; --count) {
        if (g_cx < unsigned(LCD_WIDTH) && g_cy < unsigned(LCD_HEIGHT)) {
            std::uint8_t &w = g_writes[g_cy * LCD_WIDTH + g_cx];
            if (w == 0) ++g_meter_unique;
            else if (tc) ++tc->rewritten;
            if (w < 255) ++w;
        }
        if (++g_cx > g_wx1) { g_cx = g_wx0; if (++g_cy > g_wy1) g_cy = g_wy0; }
    }
}

void ensure_panel() {
    if (!g_panel_attached) { g_panel_sink.attach(g_panel, LCD_WIDTH, 0, LCD_HEIGHT); g_panel_attached = true; }
}
}  // namespace

namespace k2::lcd {

void set_sink(const Sink* sink) { g_sink = sink; }
const Sink* get_sink() { return g_sink; }
bool sink_band(std::uint16_t& first, std::uint16_t& last) {
    if (!g_sink || !g_sink->band) return false;
    g_sink->band(&first, &last);
    return true;
}

void stream_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    g_lcd_windows++;
    if (g_sink) { g_sink_work.begins++; g_sink->begin(x0, y0, x1, y1); return; }
    ensure_panel();
    g_panel_sink.window(x0, y0, x1, y1);
    g_wx0 = g_cx = x0; g_wy0 = g_cy = y0; g_wx1 = x1; g_wy1 = y1;
    if (g_trace_on) { g_stack.fill(nullptr); backtrace(g_stack.data(), kFrames); }
}

void stream_pixels(std::span<const std::uint16_t> px) {
    if (g_sink) {
        g_sink_work.pixel_calls++;
        g_sink_work.pixels += px.size();
        g_sink->pixels(px.data(), static_cast<std::uint32_t>(px.size()));
        return;
    }
    g_lcd_pixels += static_cast<unsigned>(px.size());
    meter_write(static_cast<std::uint32_t>(px.size()));
    if (g_erase) {
        static const std::uint16_t black[64] = {};
        for (std::size_t i = 0; i < px.size(); i += 64)
            g_panel_sink.push(black, static_cast<std::uint32_t>(std::min<std::size_t>(64, px.size() - i)));
        return;
    }
    g_panel_sink.push(px.data(), static_cast<std::uint32_t>(px.size()));
}

void stream_fill(std::uint16_t color, std::uint32_t count) {
    if (g_sink) {
        g_sink_work.fill_calls++;
        g_sink_work.fill_pixels += count;
        g_sink->fill(color, count);
        return;
    }
    g_lcd_pixels += count;
    meter_write(count);
    if (g_erase) color = 0;
    g_panel_sink.fill(color, count);
}

void stream_end() {}

void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, std::uint16_t color) {
    if (!w || !h) return;
    if (!clip_to_band(y, h)) return;
    stream_begin(x, y, static_cast<std::uint16_t>(x + w - 1), static_cast<std::uint16_t>(y + h - 1));
    stream_fill(color, static_cast<std::uint32_t>(w) * h);
    stream_end();
}

void fill(std::uint16_t color) {
    g_clearing = true;
    fill_rect(0, 0, kWidth, kHeight, color);
    g_clearing = false;
}
void set_rotation(std::uint8_t) {}
void pump() {
    g_lcd_pumps++;
    if (g_pump_snap_armed) {
        g_pump_snap_armed = false;
        std::copy(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT, g_pump_panel);
    }
}
void set_erase(bool erase) { g_erase = erase; }
bool get_erase() { return g_erase; }
DmaInfo dma_info() { return {}; }
static unsigned g_spi_div = 8;
unsigned spi_div() { return g_spi_div; }
bool set_spi_div(unsigned divider) {
    SpiDiv d;
    if (!lab_spi_div(divider, d)) return false;
    g_spi_div = divider;
    return true;
}

}  // namespace k2::lcd

void lcd_meter_reset() {
    g_meter_sent = g_meter_unique = 0;
    std::fill(g_writes, g_writes + sizeof g_writes, std::uint8_t{0});
    g_trace.clear();
}
const uint8_t *lcd_meter_writes() { return g_writes; }
unsigned lcd_meter_rewritten(int x0, int y0, int x1, int y1) {
    unsigned n = 0;
    for (int y = std::max(y0, 0); y < std::min(y1, int(LCD_HEIGHT)); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, int(LCD_WIDTH)); ++x)
            if (g_writes[y * LCD_WIDTH + x] > 1) n += g_writes[y * LCD_WIDTH + x] - 1;
    return n;
}

// One line per call stack that rewrote pixels: "TRACE <label> sent=N rewritten=N off off ...", the offsets in the test binary
// (tools/overdraw_where.py resolves them). Clears the table.
void lcd_meter_trace_dump(const char *label) {
    if (!g_trace_on) return;
    for (const auto &[stack, c] : g_trace) {
        if (!c.rewritten) continue;
        std::printf("TRACE %s sent=%u rewritten=%u", label, c.sent, c.rewritten);
        for (void *a : stack) {
            Dl_info info;
            if (!a || !dladdr(a, &info)) break;
            std::printf(" %lx", static_cast<unsigned long>(static_cast<char *>(a) - static_cast<char *>(info.dli_fbase)));
        }
        std::printf("\n");
    }
    g_trace.clear();
}
