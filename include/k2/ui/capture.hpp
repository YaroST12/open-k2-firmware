// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Screenshot by re-rendering: the current page is drawn once per 4-row strip into a RAM buffer
// (the lcd sink diverts every pixel write), each strip is RLE565-coded and sent as stream frames.
// There is no framebuffer. Runs in the main loop and blocks for the transfer (a few hundred ms),
// keeping the USB stack serviced; the LCD content is not changed.
#pragma once
#include <cstdint>
#include "k2/ui/ui.hpp"

namespace k2 {

// Streams the screen as stream frames carrying `seq` (see remote_proto.hpp, SCREENSHOT).
// Returns false when the host stopped reading and the capture was aborted.
bool capture_screen(Ui &ui, uint8_t seq, uint32_t now_ms);

// Strip-at-a-time access for the HTTP screenshot (BMP): begin, then capture_strip() for strips
// 0..kCaptureStrips-1 in order, then end. The pointer is valid until the next call. One session at
// a time: begin fails (and capture_screen() aborts) while another capture is running.
constexpr int kCaptureStrips = 60;
constexpr int kCaptureStripPixels = 240 * 4;
bool capture_begin(Ui &ui);
const uint16_t *capture_strip(Ui &ui, int strip, uint32_t now_ms);
void capture_end(Ui &ui, uint32_t now_ms);

}  // namespace k2
