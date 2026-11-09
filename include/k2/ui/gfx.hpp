// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Drawing on the ST7789 without a framebuffer: anti-aliased text and fixed-width fields,
// streamed row by row through one 240-pixel line buffer.
#pragma once
#include <cstdint>
#include <string_view>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

// A screen-space rectangle that acts as origin and clip for everything drawn through it.
// Coordinates passed to the functions below are relative to its top-left corner.
struct Surface { int16_t x, y, w, h; };

constexpr Surface surface_of(const Rect &r) { return {r.x, r.y, r.w, r.h}; }

void gfx_fill(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Map a logical rect to its NATIVE panel rect: the origin goes through the ContentViewport
// mapping, the size never scales, and the box is clipped to the viewport's destination rectangle
// (exactly like a glyph box in gfx_text). Identity when no viewport is active (a 1:1 rotation).
// An empty rect (w or h <= 0) comes back when the box lies outside the surface or the viewport.
Rect gfx_native_rect(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h);
// Fill an already-mapped native rect straight to the panel, at its native size in every rotation.
// Frames and marker squares must keep all four border lines: a fill through the viewport mapping
// can have a 1 px edge squeezed away to zero physical rows in a landscape rotation, and which
// edges vanish moves as the rows scroll.
void gfx_fill_panel(const Rect &r, uint16_t color);
// Convenience: gfx_fill_panel(gfx_native_rect(...)).
void gfx_fill_native(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);

// Draw UTF-8 text with its line box's top-left at (x, y); the box is painted in `bg`
// (glyphs are blended between fg and bg), so no separate clear is needed.
// Text that runs past the surface's right edge is clipped there; host builds report that to the no-truncation
// gate (trunc.hpp), because clipped copy is truncated copy.
void gfx_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
              uint16_t fg, uint16_t bg, int16_t cell_offset = 0 K2_TRUNC_DECL);

// Same, rotated by 90 degrees (see TextDir). (x, y) is the top-left of the rotated box, which is
// f.height wide and text_width tall. The glyphs are streamed transposed through one window; the
// panel's MADCTL is not touched. Nothing is drawn when the box does not fit the surface (reported to the gate).
void gfx_text_rot(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text,
                  uint16_t fg, uint16_t bg, TextDir dir K2_TRUNC_DECL);

// y of the line-box top so that the text baseline lands on `baseline`.
constexpr int16_t top_for_baseline(const Font &f, int16_t baseline) {
    return static_cast<int16_t>(baseline - f.ascent);
}

// A fixed-width ASCII text field. draw() pads/aligns the text to `chars` cells and repaints
// only the cells that changed since the last call, inside its own box.
enum class Align : uint8_t { Left, Right };

class Field {
public:
    static constexpr int kMaxChars = 32;   // the charger-test list rows are 29 chars wide

    void init(const Surface &s, const Font &f, int16_t x, int16_t y, uint8_t chars, Align a,
              uint16_t fg, uint16_t bg);
    void set_colors(uint16_t fg, uint16_t bg);   // takes effect with the next invalidate()/draw
    void invalidate() { valid_ = false; }        // next draw repaints the whole box
    void draw(std::string_view text K2_TRUNC_DECL);
    int16_t width_px() const { return static_cast<int16_t>(chars_ * font_->adv); }

private:
    Surface surf_{};
    const Font *font_ = nullptr;
    int16_t x_ = 0, y_ = 0;
    uint8_t chars_ = 0;
    Align align_ = Align::Left;
    uint16_t fg_ = 0, bg_ = 0;
    bool valid_ = false;
    char cur_[kMaxChars + 1] = {};
};

}  // namespace k2
