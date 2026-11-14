// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pixel sink for the LCD streaming interface (window + pixel pushes), writing into a RAM strip
// of full-width rows instead of the panel. Pure: used by the screenshot capture (a 4-row strip,
// re-rendered once per strip) and by the host tests (the whole screen as one 240-row strip).
//
// It mimics the ST7789 write pointer: pixels fill the window row by row, left to right, and wrap
// to the window origin after the last pixel. Pixels outside the strip or outside the buffer width
// are discarded, so a drawing routine can run unchanged while only one strip is kept.
//
// Cost: a window entirely outside the strip moves the write pointer arithmetically (O(1) per push or fill, whatever the
// pixel count); inside the strip a window row is one span copy or fill, not a per-pixel loop.
#pragma once
#include <cstdint>
#include <cstring>
#include "k2/core/hot.h"

namespace k2 {

class PixelSink {
public:
    // buf holds `rows` rows of `width` pixels covering screen rows y0 .. y0+rows-1.
    K2_HOT void attach(uint16_t *buf, int width, int y0, int rows) {
        buf_ = buf; width_ = width; y0_ = y0; rows_ = rows;
        window(0, 0, static_cast<uint16_t>(width - 1), 0);
    }

    // The screen rows the strip keeps (for lcd::Sink::band).
    K2_HOT int first_row() const { return y0_; }
    K2_HOT int last_row() const { return y0_ + rows_ - 1; }

    K2_HOT void window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
        wx0_ = x0; wy0_ = y0; wx1_ = x1; wy1_ = y1;
        cx_ = x0; cy_ = y0;
        outside_ = x1 >= x0 && y1 >= y0 &&
                   (static_cast<int>(y1) < y0_ || static_cast<int>(y0) >= y0_ + rows_);
    }

    K2_HOT void push(const uint16_t *px, uint32_t count) {
        if (outside_) { skip(count); return; }
        while (count) {
            const uint32_t run = chunk(count);
            if (writable()) {
                const uint32_t n = visible(run);
                if (n) std::memcpy(row_ptr() + cx_, px, n * sizeof(uint16_t));
            }
            px += run;
            advance(run);
            count -= run;
        }
    }

    K2_HOT void fill(uint16_t color, uint32_t count) {
        if (outside_) { skip(count); return; }
        while (count) {
            const uint32_t run = chunk(count);
            if (writable()) {
                const uint32_t n = visible(run);
                uint16_t *d = row_ptr() + cx_;
                for (uint32_t i = 0; i < n; i++) d[i] = color;
            }
            advance(run);
            count -= run;
        }
    }

private:
    K2_HOT uint32_t chunk(uint32_t count) const {   // pixels left in the current window row
        const uint32_t left = static_cast<uint32_t>(wx1_) - cx_ + 1;
        return count < left ? count : left;
    }
    K2_HOT bool writable() const { return buf_ && cy_ >= static_cast<uint32_t>(y0_) && cy_ < static_cast<uint32_t>(y0_ + rows_); }
    K2_HOT uint32_t visible(uint32_t run) const {   // pixels of this window-row chunk inside the buffer width
        const uint32_t w = static_cast<uint32_t>(width_);
        return cx_ >= w ? 0 : (run < w - cx_ ? run : w - cx_);
    }
    K2_HOT uint16_t *row_ptr() const { return buf_ + (cy_ - static_cast<uint32_t>(y0_)) * static_cast<uint32_t>(width_); }
    K2_HOT void advance(uint32_t run) {
        cx_ += run;
        if (cx_ > wx1_) {
            cx_ = wx0_;
            if (++cy_ > wy1_) cy_ = wy0_;
        }
    }
    // The write pointer after `count` pixels that land nowhere: position in the window modulo its area.
    K2_HOT void skip(uint32_t count) {
        const uint32_t w = wx1_ - wx0_ + 1, h = wy1_ - wy0_ + 1;   // a panel window: w * h fits 32 bits
        const uint32_t to = ((cy_ - wy0_) * w + (cx_ - wx0_) + count) % (w * h);
        cx_ = wx0_ + to % w;
        cy_ = wy0_ + to / w;
    }

    uint16_t *buf_ = nullptr;
    int width_ = 0, y0_ = 0, rows_ = 0;
    uint32_t wx0_ = 0, wy0_ = 0, wx1_ = 0, wy1_ = 0;
    uint32_t cx_ = 0, cy_ = 0;
    bool outside_ = false;   // the whole window lies above or below the strip
};

}  // namespace k2
