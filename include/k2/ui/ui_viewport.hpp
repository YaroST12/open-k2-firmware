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
    // The inverse of mapped_rect() for one panel column / row: the logical 224x240 column / row whose stretched box
    // contains it, or -1 when no logical row owns it. Identity without a viewport. Independent of Pause, so a painter can
    // compose a stretched rectangle pixel by pixel in panel space (home_cell paint()).
    static int source_x(int panel_x);
    static int source_y(int panel_y);

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
    void push(uint16_t color);
    void flush();

    Rect destination_;
    const lcd::Sink* previous_ = nullptr;
    bool owns_ = false;
    uint16_t x0_ = 0, y0_ = 0, width_ = 0, column_ = 0, row_ = 0;
    // The output row lives in one static buffer (ui_viewport.cpp): only the owning instance (`active`) ever fills it,
    // and the Ui methods that nest one viewport inside another used to carry 480 B of stack each.
};

}  // namespace k2
