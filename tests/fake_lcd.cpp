// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host stand-in for src/drivers/lcd.cpp: the same sink diversion, with a 240x240 "panel" RAM behind it.
#include "fake_lcd.h"
#include "k2/ui/pixel_sink.hpp"
#include <algorithm>
#include <cstddef>

uint16_t g_panel[LCD_WIDTH * LCD_HEIGHT];
unsigned g_lcd_windows;
unsigned g_lcd_pixels;

namespace {
const k2::lcd::Sink* g_sink;
k2::PixelSink g_panel_sink;
bool g_panel_attached;
bool g_erase;

void ensure_panel() {
    if (!g_panel_attached) { g_panel_sink.attach(g_panel, LCD_WIDTH, 0, LCD_HEIGHT); g_panel_attached = true; }
}
}  // namespace

namespace k2::lcd {

void set_sink(const Sink* sink) { g_sink = sink; }
const Sink* get_sink() { return g_sink; }

void stream_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1) {
    g_lcd_windows++;
    if (g_sink) { g_sink->begin(x0, y0, x1, y1); return; }
    ensure_panel();
    g_panel_sink.window(x0, y0, x1, y1);
}

void stream_pixels(std::span<const std::uint16_t> px) {
    if (g_sink) { g_sink->pixels(px.data(), static_cast<std::uint32_t>(px.size())); return; }
    g_lcd_pixels += static_cast<unsigned>(px.size());
    if (g_erase) {
        static const std::uint16_t black[64] = {};
        for (std::size_t i = 0; i < px.size(); i += 64)
            g_panel_sink.push(black, static_cast<std::uint32_t>(std::min<std::size_t>(64, px.size() - i)));
        return;
    }
    g_panel_sink.push(px.data(), static_cast<std::uint32_t>(px.size()));
}

void stream_fill(std::uint16_t color, std::uint32_t count) {
    if (g_sink) { g_sink->fill(color, count); return; }
    g_lcd_pixels += count;
    if (g_erase) color = 0;
    g_panel_sink.fill(color, count);
}

void stream_end() {}

void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, std::uint16_t color) {
    if (!w || !h) return;
    stream_begin(x, y, static_cast<std::uint16_t>(x + w - 1), static_cast<std::uint16_t>(y + h - 1));
    stream_fill(color, static_cast<std::uint32_t>(w) * h);
    stream_end();
}

void fill(std::uint16_t color) { fill_rect(0, 0, kWidth, kHeight, color); }
void set_rotation(std::uint8_t) {}
void set_erase(bool erase) { g_erase = erase; }
bool get_erase() { return g_erase; }
DmaInfo dma_info() { return {}; }

}  // namespace k2::lcd
