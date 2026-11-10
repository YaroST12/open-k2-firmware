// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Text layout and per-row rendering for the anti-aliased monospace fonts. Pure: no hardware.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {
// In-text markers drawn as an anti-aliased check / cross by draw_marked_text() (ui_widgets.hpp).
constexpr char kMarkCheck = '\x01', kMarkCross = '\x02';


// Decode one UTF-8 code point at s[i] and advance i (always by at least one byte).
// Invalid or truncated sequences give '?'.
uint32_t next_codepoint(std::string_view s, size_t &i);

// Debug counter: code points a font could not draw exactly (it had to fall back to '?'). Host tests
// assert it stays 0 over every page and submenu string.
extern uint32_t g_missing_glyphs;

// Glyph for a code point, falling back to '?'; nullptr when the font has neither.
const Glyph *find_glyph(const Font &f, uint32_t cp);

// Width in pixels of the string (monospace: code points x advance).
int text_width(const Font &f, std::string_view s);

// Render pixel row `row` (0 = top of the line box, f.height rows) of the string into out,
// blending between lut[0] (background) and lut[15] (foreground). Writes min(width, max_px)
// pixels and returns that count.
int render_row(const Font &f, std::string_view s, int row, const uint16_t (&lut)[16],
               uint16_t *out, int max_px);

// The window [x0, x0 + count) of that row, written to out[0..]: a caller with a short buffer streams a wide
// row as consecutive windows. Clipped at the text width; returns the pixels written. render_row is x0 = 0.
int render_row_span(const Font &f, std::string_view s, int row, const uint16_t (&lut)[16],
                    uint16_t *out, int x0, int count);

// ---- Text rotated by 90 degrees, streamed transposed -----------------------------------------
// A rotated line occupies a window line_h wide and text_w tall (screen pixels). The window is
// streamed row by row (sy = 0..text_w-1), each row holding line_h pixels (sx = 0..line_h-1).

struct SrcPos { int x, y; };   // pixel position inside the unrotated line box

// Which pixel of the unrotated line box (text_w x line_h) lands at window pixel (sx, sy).
constexpr SrcPos rot_source(TextDir d, int text_w, int line_h, int sx, int sy) {
    return d == TextDir::Ccw ? SrcPos{text_w - 1 - sy, sx} : SrcPos{sy, line_h - 1 - sx};
}

// Pixel column `col` (0 = leftmost) of the unrotated string, top to bottom: f.height pixels.
// Same pixels as render_row, read along the other axis.
int render_column(const Font &f, std::string_view s, int col, const uint16_t (&lut)[16], uint16_t *out);

// Pixel row `sy` of the rotated window (line_h = f.height pixels).
void render_rotated_row(const Font &f, std::string_view s, TextDir d, int sy, const uint16_t (&lut)[16],
                        uint16_t *out);

}  // namespace k2
