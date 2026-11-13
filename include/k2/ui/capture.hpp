// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Screenshot by re-rendering: the current page is drawn once per 4-row strip into a RAM buffer
// (the lcd sink diverts every pixel write), each strip is RLE565-coded and sent as stream frames.
// There is no framebuffer. Runs in the main loop and blocks for the transfer (a few hundred ms),
// keeping the USB stack serviced; the LCD content is not changed.
#pragma once
#include <cstdint>
#include "k2/core/build_config.h"
#include "k2/ui/ui.hpp"

namespace k2 {

constexpr int kCaptureStrips = 60;
constexpr int kCaptureStripPixels = 240 * 4;

#if K2_SCREENSHOT
// Streams the screen as stream frames carrying `seq` (see remote_proto.hpp, SCREENSHOT), over the HID IN endpoint or,
// with `cdc`, the CDC bulk pipe (the host asked and has the port open).
// Returns false when the host stopped reading and the capture was aborted.
bool capture_screen(Ui &ui, uint8_t seq, uint32_t now_ms, bool cdc = false);

// Strip-at-a-time access for the HTTP screenshot (BMP): begin, then capture_strip() for strips
// 0..kCaptureStrips-1 in order, then end. The pointer is valid until the next call. One session at
// a time: begin fails (and capture_screen() aborts) while another capture is running.
bool capture_begin(Ui &ui);
const uint16_t *capture_strip(Ui &ui, int strip, uint32_t now_ms);
void capture_end(Ui &ui, uint32_t now_ms);
#else
inline bool capture_screen(Ui &, uint8_t, uint32_t, bool = false) { return false; }
inline bool capture_begin(Ui &) { return false; }
inline const uint16_t *capture_strip(Ui &, int, uint32_t) { return nullptr; }
inline void capture_end(Ui &, uint32_t) {}
#endif

}  // namespace k2
