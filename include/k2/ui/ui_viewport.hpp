// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#pragma once
#include "k2/drivers/lcd.hpp"
#include "k2/ui/ui_layout.hpp"

namespace k2 {

// Map the pages' established 224x240 coordinates into the rotated content rectangle.
// Each source row is consumed once; only one output row is retained, never a framebuffer. Main loop only.
class ContentViewport {
public:
    explicit ContentViewport(Rect destination);
    ~ContentViewport();
    ContentViewport(const ContentViewport&) = delete;
    ContentViewport& operator=(const ContentViewport&) = delete;

    // Map a glyph origin, preserving its native pixel dimensions. Cell offsets stay native
    // so partial Field redraws land at exactly the same advance as a whole-field draw.
    static Rect native_rect(Rect source, int cell_offset = 0);
    static Rect mapped_rect(Rect source);
    // True while fills and glyphs go through a stretch: a glyph keeps its native size, so it no longer fills the box that
    // the page measured for it, and a caller that leaves such a box open to save bus time must paint it instead.
    static bool stretched();
    // True while the map changes the size of what a page draws (the landscape rotations: 224 x 240 into 240 x 224). A move
    // alone (rotation 2 shifts the content right past the strip) keeps every glyph as large as its box, so ground that leaves
    // a hole for a glyph stays correct there; only `scaled()` has to fill the holes.
    static bool scaled();
    // The inverse of mapped_rect() for one panel column / row: the logical 224x240 column / row whose stretched box
    // contains it, or -1 when no logical row owns it. Identity without a viewport. Independent of Pause, so a painter can
    // compose a stretched rectangle pixel by pixel in panel space (home_cell paint()).
    static int source_x(int panel_x);
    static int source_y(int panel_y);
    // Panel-space band of the underlying sink (unwraps an active ContentViewport if needed).
    static bool panel_band(uint16_t& first, uint16_t& last);

    class Pause {
    public:
        Pause();
        ~Pause();
    private:
        const lcd::Sink* saved_;
    };

private:
    static void begin(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
    static void pixels(const uint16_t* pixels, uint32_t count);
    static void fill(uint16_t color, uint32_t count);
    bool row_in_band() const;
    void push(uint16_t color);
    void flush();

    Rect destination_;
    const lcd::Sink* previous_ = nullptr;
    bool owns_ = false;
    bool has_band_ = false;
    uint16_t x0_ = 0, y0_ = 0, width_ = 0, column_ = 0, row_ = 0;
    uint16_t band_first_ = 0, band_last_ = 0;
    // The output row lives in one static buffer (ui_viewport.cpp): only the owning instance (`active`) ever fills it,
    // and the Ui methods that nest one viewport inside another used to carry 480 B of stack each.
};

}  // namespace k2
