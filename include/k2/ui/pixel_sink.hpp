// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pixel sink for the LCD streaming interface (window + pixel pushes), writing into a RAM strip
// of full-width rows instead of the panel. Pure: used by the screenshot capture (a 16-row strip,
// re-rendered once per strip) and by the host tests (the whole screen as one 240-row strip).
//
// It mimics the ST7789 write pointer: pixels fill the window row by row, left to right, and wrap
// to the window origin after the last pixel. Pixels outside the strip or outside the buffer width
// are discarded, so a drawing routine can run unchanged while only one strip is kept.
#pragma once
#include <cstdint>

namespace k2 {

class PixelSink {
public:
    // buf holds `rows` rows of `width` pixels covering screen rows y0 .. y0+rows-1.
    void attach(uint16_t *buf, int width, int y0, int rows) {
        buf_ = buf; width_ = width; y0_ = y0; rows_ = rows;
        window(0, 0, static_cast<uint16_t>(width - 1), 0);
    }

    void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        wx0_ = x0; wy0_ = y0; wx1_ = x1; wy1_ = y1;
        cx_ = x0; cy_ = y0;
    }

    void push(const uint16_t *px, uint32_t count) {
        while (count) {
            const uint32_t run = chunk(count);
            if (writable()) for (uint32_t i = 0; i < run; i++) put(cx_ + i, px[i]);
            px += run;
            advance(run);
            count -= run;
        }
    }

    void fill(uint16_t color, uint32_t count) {
        while (count) {
            const uint32_t run = chunk(count);
            if (writable()) for (uint32_t i = 0; i < run; i++) put(cx_ + i, color);
            advance(run);
            count -= run;
        }
    }

private:
    uint32_t chunk(uint32_t count) const {   // pixels left in the current window row
        const uint32_t left = static_cast<uint32_t>(wx1_) - cx_ + 1;
        return count < left ? count : left;
    }
    bool writable() const { return buf_ && cy_ >= static_cast<uint32_t>(y0_) && cy_ < static_cast<uint32_t>(y0_ + rows_); }
    void put(uint32_t x, uint16_t v) {
        if (x < static_cast<uint32_t>(width_)) buf_[(cy_ - static_cast<uint32_t>(y0_)) * static_cast<uint32_t>(width_) + x] = v;
    }
    void advance(uint32_t run) {
        cx_ += run;
        if (cx_ > wx1_) {
            cx_ = wx0_;
            if (++cy_ > wy1_) cy_ = wy0_;
        }
    }

    uint16_t *buf_ = nullptr;
    int width_ = 0, y0_ = 0, rows_ = 0;
    uint32_t wx0_ = 0, wy0_ = 0, wx1_ = 0, wy1_ = 0;
    uint32_t cx_ = 0, cy_ = 0;
};

}  // namespace k2
