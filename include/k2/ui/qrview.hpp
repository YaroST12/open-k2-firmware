// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Putting a QR symbol on the panel: layout and drawing shared by the boot-info screen and the share view.
//
// Black modules on white, at least kMinModulePx pixels per module, a kQuietModules-module quiet zone on every side (part of
// the box), the box centred at the top, readable text lines under it. draw_code() streams the box row by row through the
// lcd stream API (each module row built once into one pixel line, pushed px times), so no frame buffer is needed.
#pragma once
#include <cstdint>

#include "k2/ui/qr.hpp"

namespace k2::qrview {

inline constexpr int kPanelPx = 240;
inline constexpr int kMinModulePx = 3;          // below this a phone camera loses the code on this panel
inline constexpr int kQuietModules = 4;         // the QR specification's quiet zone
inline constexpr int kLineH = 12;               // one text line (font_12)
inline constexpr int kCells = kPanelPx / 6;     // characters per text line in the 6 px font
inline constexpr std::uint16_t kBlack = 0x0000, kWhite = 0xFFFF;

// Where the code goes for a symbol of `n` modules per side with `text_h` pixels reserved under the box: the largest whole
// pixels per module that keeps the box and the text on the panel. px == 0 when the symbol does not fit at kMinModulePx.
struct Plan {
    int px = 0;
    int box = 0;      // side of the square (symbol + quiet zone), pixels
    int x0 = 0, y0 = 0;
};
Plan plan(int n, int text_h);

// The same inside a rectangle of the panel (the share view: the content area beside the button strip, 224 x 240 or 240 x 224
// by rotation). Plan::x0 / y0 are panel coordinates; the box is centred across the area at its top. `min_px` is the least
// pixels per module accepted (the share view takes 2 where 3 does not fit; 3 is the camera-safe size the boot screen holds).
// `quiet` is the quiet zone in modules (default kQuietModules, the QR specification's 4) and `px` the pixels per module:
// 0 = the largest whole px that keeps the box and the text on the panel (the default, what the share view and the boot
// screen use), a positive px = exactly that size, refused (px == 0) when the box does not fit the area at it.
struct Area {
    int x = 0, y = 0, w = kPanelPx, h = kPanelPx;
};
Plan plan_in(const Area& a, int n, int text_h, int min_px, int quiet = kQuietModules, int px = 0);

// Streams the symbol of `w` (n modules per side) into the area as plan_in() lays it out, over what is there: the caller clears
// the area first when the old box may be bigger. `line` is a kPanelPx-pixel work row. Returns the panel y where the text
// starts (box + 2 below the box's top), or 0 when the symbol does not fit.
int draw_code_in(const Area& a, const qr::Workspace& w, int n, int text_h, int min_px, std::uint16_t* line,
                 int quiet = kQuietModules, int px = 0);

// The same for a symbol in a caller buffer (a workspace above qr::kMaxVersion, the QR scan-test path): `code` is the
// packed bitmap, `quiet` and `px` are explicit (no defaults: the caller chose them).
int draw_code_raw_in(const Area& a, const std::uint8_t* code, int n, int text_h, int min_px, std::uint16_t* line,
                     int quiet, int px);

// Draws the symbol held in `w` (n modules per side) on the whole panel. The panel is cleared to white first when `fresh` is set
// or the symbol size differs from the last one drawn (a different size leaves the old box's edge behind). `line` is a
// kPanelPx-pixel work row. Returns the y where the text starts (box + 2), or 0 when the symbol does not fit.
int draw_code(const qr::Workspace& w, int n, int text_h, std::uint16_t* line, bool fresh);

// One text line at row `y`, black on white, padded with spaces to the full width so a shorter text repaints the cells the
// longer one left. Cut at kCells characters.
void draw_line(int y, const char* text);
// The same across an area (area.w / 6 characters, from the area's left edge).
void draw_line_in(const Area& a, int y, const char* text);

}  // namespace k2::qrview
