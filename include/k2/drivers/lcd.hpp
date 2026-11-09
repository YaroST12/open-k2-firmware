// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ST7789V 240x240 IPS panel on SPI3 (8-bit, 1-line TX only, no read-back).
// Colours are RGB565. Drawing is done in "streams": open a window, push pixels, close.
#pragma once
#include <cstdint>
#include <span>

namespace k2::lcd {

inline constexpr std::uint16_t kWidth = 240, kHeight = 240;

// Clocks, pins, SPI3, panel reset and the ST7789 power-up sequence; backlight on.
// Takes ~0.3 s of busy waiting (reset 15 + 120 ms, sleep-out 120 ms, display-on 20 ms).
void init();

// Display off, sleep in, backlight off: the panel as a power cycle leaves it, before a reset.
// The driver is unusable until init() runs again.
void park();

// ST7789 rotation 0..3 (MADCTL 0x00/0x60/0xC0/0xA0). The 240x240 panel sits in a 240x320
// RAM, so the flipped modes shift the window origin by 80; only rotation 0 is verified.
void set_rotation(std::uint8_t rotation);

void fill(std::uint16_t color);
void fill_rect(std::uint16_t x, std::uint16_t y, std::uint16_t w, std::uint16_t h, std::uint16_t color);

// Streamed write into the window (x0,y0)-(x1,y1) inclusive. usb::service() and the idle hook run
// every ~2048 pixels (polled path) or about every millisecond of wire time (DMA path) so that a
// long fill cannot starve the USB stack or PD. Runs of 32+ pixels go out by DMA (SPI3 TX):
// stream_fill returns with its last piece still on the wire, and the next lcd call (the next
// stream_*, stream_end, a command) waits for it; stream_pixels returns with its buffer consumed.
// -DK2_LCD_DMA=0 builds the polled-only driver.
void stream_begin(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1);
void stream_pixels(std::span<const std::uint16_t> px);
void stream_fill(std::uint16_t color, std::uint32_t count);
void stream_end();

// Divert the stream_* calls (every fill, rect and text goes through them) to a sink instead of
// the panel, e.g. a RAM strip for screenshots. nullptr restores the panel. The sink is a plain
// function table so the capture code stays free of driver headers.
struct Sink {
    void (*begin)(std::uint16_t x0, std::uint16_t y0, std::uint16_t x1, std::uint16_t y1);
    void (*pixels)(const std::uint16_t* px, std::uint32_t count);
    void (*fill)(std::uint16_t color, std::uint32_t count);
};
void set_sink(const Sink* sink);
const Sink* get_sink();

// Erase mode: every pixel streamed to the panel becomes kBg (black), whatever the caller drew.
// Replaying a page's draw_static()+update() in erase mode wipes exactly that page's ink, so a
// page switch repaints only the outgoing and incoming ink instead of the whole screen. Capture
// sinks are not affected. Only meaningful with no sink attached.
void set_erase(bool erase);
bool get_erase();

// State of the SPI3 TX DMA path, for the remote `debug lcd` query (the boot-time "[LCD] DMA ok" log line is
// gone before a host attaches). `enabled` is the build option, `ok` goes false for good after a stalled
// transfer; `transfers` counts completed DMA transfers (pieces of up to 65535 units) since boot and
// `stalls` the abandoned ones.
struct DmaInfo {
    bool enabled = false, ok = false;
    std::uint32_t transfers = 0, stalls = 0;
};
DmaInfo dma_info();

// Called every few hundred streamed pixels (with usb::service) so time-critical work, such as
// answering PD messages, keeps running while a page draws. nullptr disables it.
void set_idle_hook(void (*hook)());

// One round of the same service work outside a stream (usb::service() and the idle hook), for a long computation that
// must not starve USB and the PD engine (the QR encoder of a large symbol polls it between its mask candidates).
void pump();

}  // namespace k2::lcd
