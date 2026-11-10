// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PixelSink (include/k2/ui/pixel_sink.hpp) against the plain per-pixel write-pointer model it replaced: random windows
// (inside, across and outside the strip, wider than the buffer), random pushes and fills (shorter than a row, longer
// than the window so the pointer wraps), several calls per window. And lcd::clip_to_band.
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/pixel_sink.hpp"
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace k2;

namespace {

// The old algorithm, one pixel at a time.
struct Model {
    std::vector<uint16_t> buf;
    int width = 0, y0 = 0, rows = 0;
    uint32_t wx0 = 0, wy0 = 0, wx1 = 0, wy1 = 0, cx = 0, cy = 0;
    void attach(int w, int top, int n) {
        width = w; y0 = top; rows = n;
        buf.assign(static_cast<size_t>(w) * n, 0);
        window(0, 0, static_cast<uint16_t>(w - 1), 0);
    }
    void window(uint16_t x0_, uint16_t y0_, uint16_t x1_, uint16_t y1_) { wx0 = x0_; wy0 = y0_; wx1 = x1_; wy1 = y1_; cx = x0_; cy = y0_; }
    void put(uint16_t v) {
        if (cy >= static_cast<uint32_t>(y0) && cy < static_cast<uint32_t>(y0 + rows) && cx < static_cast<uint32_t>(width))
            buf[(cy - y0) * width + cx] = v;
        if (++cx > wx1) { cx = wx0; if (++cy > wy1) cy = wy0; }
    }
};

uint32_t g_seed = 12345;
uint32_t rnd(uint32_t n) { g_seed = g_seed * 1664525u + 1013904223u; return (g_seed >> 8) % n; }

void test_matches_the_per_pixel_model() {
    constexpr int kW = 24, kRows = 4, kTop = 8;   // a small screen so windows hit every edge case often
    int mismatches = 0;
    for (int round = 0; round < 400; round++) {
        std::vector<uint16_t> storage(kW * kRows, 0);
        PixelSink sink;
        Model model;
        sink.attach(storage.data(), kW, kTop, kRows);
        model.attach(kW, kTop, kRows);
        for (int w = 0; w < 6; w++) {
            const uint16_t x0 = static_cast<uint16_t>(rnd(kW + 6)), y0 = static_cast<uint16_t>(rnd(24));
            const uint16_t x1 = static_cast<uint16_t>(x0 + rnd(12)), y1 = static_cast<uint16_t>(y0 + rnd(10));
            sink.window(x0, y0, x1, y1);
            model.window(x0, y0, x1, y1);
            const uint32_t area = (x1 - x0 + 1u) * (y1 - y0 + 1u);
            for (int c = 0; c < 4; c++) {
                const uint32_t n = rnd(area * 2 + 3);   // up to two windows: wraps
                if (rnd(2)) {
                    const uint16_t color = static_cast<uint16_t>(1 + rnd(0xFFFE));
                    sink.fill(color, n);
                    for (uint32_t i = 0; i < n; i++) model.put(color);
                } else {
                    std::vector<uint16_t> px(n);
                    for (auto &v : px) v = static_cast<uint16_t>(1 + rnd(0xFFFE));
                    sink.push(px.data(), n);
                    for (uint32_t i = 0; i < n; i++) model.put(px[i]);
                }
            }
        }
        if (storage != model.buf) mismatches++;
    }
    CHECK_EQ(mismatches, 0);
}

void test_window_straddling_the_strip() {   // a window that starts above the strip, runs through it and ends below
    uint16_t buf[8 * 2] = {};
    PixelSink sink;
    sink.attach(buf, 8, 10, 2);
    sink.window(0, 9, 7, 12);                                // rows 9 (above), 10, 11 (inside), 12 (below)
    sink.fill(0x1111, 8);                                    // row 9: discarded
    sink.fill(0x2222, 8 + 3);                                // row 10 and 3 pixels of row 11
    sink.fill(0x3333, 5 + 8);                                // the rest of row 11, then row 12 (discarded)
    for (int i = 0; i < 8; i++) CHECK_EQ(buf[i], 0x2222);
    for (int i = 8; i < 11; i++) CHECK_EQ(buf[i], 0x2222);
    for (int i = 11; i < 16; i++) CHECK_EQ(buf[i], 0x3333);
    sink.window(0, 0, 7, 3);                                 // wholly above: a fill leaves the strip alone
    sink.fill(0x4444, 1000);
    CHECK_EQ(buf[0], 0x2222);
    CHECK_EQ(buf[15], 0x3333);
}

uint16_t g_band_first = 20, g_band_last = 23;
void band_of(uint16_t *first, uint16_t *last) { *first = g_band_first; *last = g_band_last; }
void nop_begin(uint16_t, uint16_t, uint16_t, uint16_t) {}
void nop_pixels(const uint16_t *, uint32_t) {}
void nop_fill(uint16_t, uint32_t) {}

void test_clip_to_band() {
    uint16_t y = 100, h = 30;
    CHECK_EQ(lcd::clip_to_band(y, h), true);                 // no sink: untouched
    CHECK_EQ(y, 100);
    CHECK_EQ(h, 30);

    const lcd::Sink plain = {nop_begin, nop_pixels, nop_fill};   // a sink without a band keeps everything
    lcd::set_sink(&plain);
    CHECK_EQ(lcd::clip_to_band(y, h), true);
    CHECK_EQ(h, 30);

    const lcd::Sink banded = {nop_begin, nop_pixels, nop_fill, band_of};
    lcd::set_sink(&banded);
    y = 0; h = 20;   CHECK_EQ(lcd::clip_to_band(y, h), false);    // ends at row 19, band starts at 20
    y = 24; h = 5;   CHECK_EQ(lcd::clip_to_band(y, h), false);    // starts after the band
    y = 18; h = 4;   CHECK_EQ(lcd::clip_to_band(y, h), true);     // rows 18..21 -> 20..21
    CHECK_EQ(y, 20);
    CHECK_EQ(h, 2);
    y = 22; h = 10;  CHECK_EQ(lcd::clip_to_band(y, h), true);     // rows 22..31 -> 22..23
    CHECK_EQ(y, 22);
    CHECK_EQ(h, 2);
    y = 0; h = 240;  CHECK_EQ(lcd::clip_to_band(y, h), true);     // the whole screen -> the band
    CHECK_EQ(y, 20);
    CHECK_EQ(h, 4);
    y = 21; h = 1;   CHECK_EQ(lcd::clip_to_band(y, h), true);     // inside: untouched
    CHECK_EQ(y, 21);
    CHECK_EQ(h, 1);
    lcd::set_sink(nullptr);
}

}  // namespace

int main() {
    test_matches_the_per_pixel_model();
    test_window_straddling_the_strip();
    test_clip_to_band();
    return test_report("test_pixel_sink");
}
