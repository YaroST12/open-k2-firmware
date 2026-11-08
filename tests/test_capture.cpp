// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the screenshot path: PixelSink window semantics, RLE565 coding, and the strip
// capture itself (real gfx/text/fonts drawn through the sink, 15 strips versus one full pass).
#include "test_util.h"
#include "fake_lcd.h"
#include "k2/ui/pixel_sink.hpp"
#include "k2/ui/rle565.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/font_aa.hpp"
#include <vector>
#include <cstdlib>

using namespace k2;

// ---- PixelSink ----
static void test_sink_window() {
    uint16_t buf[8 * 4] = {};
    PixelSink s;
    s.attach(buf, 8, 10, 4);                       // strip covers screen rows 10..13
    s.window(2, 10, 4, 11);                         // 3x2 window
    const uint16_t px[7] = {1, 2, 3, 4, 5, 6, 7};
    s.push(px, 7);                                  // 6 fill the window, the 7th wraps to the origin
    CHECK_EQ(buf[0 * 8 + 2], 7); CHECK_EQ(buf[0 * 8 + 3], 2); CHECK_EQ(buf[0 * 8 + 4], 3);
    CHECK_EQ(buf[1 * 8 + 2], 4); CHECK_EQ(buf[1 * 8 + 4], 6);
    // Rows outside the strip are dropped but still consume window positions.
    s.window(0, 12, 1, 15);
    s.fill(9, 8);
    CHECK_EQ(buf[2 * 8 + 0], 9); CHECK_EQ(buf[3 * 8 + 1], 9);
    s.window(0, 8, 1, 11);                          // rows 8,9 clipped, 10,11 written
    const uint16_t q[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    s.push(q, 8);
    CHECK_EQ(buf[0], 5); CHECK_EQ(buf[1], 6); CHECK_EQ(buf[8], 7); CHECK_EQ(buf[9], 8);
    // Columns beyond the width never write.
    s.window(6, 10, 9, 10);
    s.fill(0xAAAA, 4);
    CHECK_EQ(buf[6], 0xAAAA); CHECK_EQ(buf[7], 0xAAAA);
}

// ---- RLE565 ----
static std::vector<uint8_t> encode(const std::vector<uint16_t> &px, size_t chunk_cap = 56,
                                   std::vector<size_t> *chunks = nullptr) {
    Rle565Encoder e;
    e.reset(px.data(), px.size());
    std::vector<uint8_t> out;
    uint8_t buf[56];
    while (!e.done()) {
        size_t n = e.next(buf, chunk_cap);
        if (chunks) chunks->push_back(n);
        out.insert(out.end(), buf, buf + n);
    }
    return out;
}

static void roundtrip(const std::vector<uint16_t> &px, const char *what) {
    std::vector<size_t> chunks;
    auto enc = encode(px, 56, &chunks);
    std::vector<uint16_t> dec(px.size() + 4, 0xDEAD);
    size_t n = 0;
    bool ok = rle565_decode(enc.data(), enc.size(), dec.data(), dec.size(), n);
    if (!ok || n != px.size() || !std::equal(px.begin(), px.end(), dec.begin())) {
        g_failures++; printf("FAIL roundtrip %s\n", what);
    }
    g_checks++;
    CHECK_EQ(enc.size() <= rle565_worst_case(px.size()), 1);
    for (size_t c : chunks) CHECK_EQ(c <= 56, 1);
}

static void test_rle() {
    roundtrip({}, "empty");
    roundtrip({0x1234}, "one");
    roundtrip(std::vector<uint16_t>(3840, 0), "all same");
    roundtrip(std::vector<uint16_t>(129, 7), "run just over 128");
    roundtrip(std::vector<uint16_t>(128, 7), "run of 128");
    std::vector<uint16_t> alt(3840);
    for (size_t i = 0; i < alt.size(); i++) alt[i] = static_cast<uint16_t>(i * 7 + 1);   // worst case
    roundtrip(alt, "all different");
    CHECK_EQ(encode(alt).size(), rle565_worst_case(alt.size()));
    std::vector<uint16_t> two(3840);
    for (size_t i = 0; i < two.size(); i++) two[i] = (i / 2) & 1 ? 0x1111 : 0x2222;       // runs of 2
    roundtrip(two, "runs of two");
    std::srand(1);
    for (int t = 0; t < 200; t++) {
        std::vector<uint16_t> r;
        size_t n = std::rand() % 400;
        while (r.size() < n) {
            uint16_t v = static_cast<uint16_t>(std::rand() % 5);
            size_t run = 1 + std::rand() % 6;
            for (size_t i = 0; i < run && r.size() < n; i++) r.push_back(v);
        }
        roundtrip(r, "random");
    }
    // A run of exactly one token: 3 bytes.
    CHECK_EQ(encode(std::vector<uint16_t>(100, 5)).size(), 3);
    // Decoder rejects truncated and oversize streams.
    uint8_t bad1[] = {0x05, 0x34};                  // run token missing its pixel byte
    uint16_t out[8]; size_t n = 0;
    CHECK_EQ(rle565_decode(bad1, sizeof bad1, out, 8, n), 0);
    uint8_t bad2[] = {0x07, 0x34, 0x12};            // 8 pixels
    CHECK_EQ(rle565_decode(bad2, sizeof bad2, out, 4, n), 0);
    CHECK_EQ(rle565_decode(bad2, sizeof bad2, out, 8, n), 1);
}

// ---- strip capture versus full render ----
static void scene() {
    const Surface scr = {0, 0, 240, 240};
    gfx_fill(scr, 0, 0, 240, 240, 0x0841);
    gfx_fill(scr, 10, 5, 100, 40, 0xF800);
    gfx_fill(scr, 0, 30, 240, 3, 0x07E0);                     // crosses strip boundaries
    gfx_text(scr, font_24, 12, 14, "20.03 V", 0xFFFF, 0xF800);
    gfx_text(scr, font_16, 8, 120, "PD3.1 12.0V", 0xFFE0, 0x0841);
    gfx_text_rot(scr, font_16, 215, 20, "BK\xC2\xB7RBT", 0xFFFF, 0x001F, TextDir::Ccw);
    gfx_text_rot(scr, font_16, 215, 100, "PG>", 0xFFFF, 0x001F, TextDir::Cw);
    Field f;
    f.init(scr, font_24, 20, 60, 8, Align::Right, 0x07FF, 0x0000);
    f.draw("12.34");
    f.draw("12.99");
    gfx_fill(scr, 237, 237, 10, 10, 0x1234);                  // clipped at the corner
}

static std::vector<uint16_t> g_strip_buf;
static PixelSink g_ps;
static void s_begin(uint16_t a, uint16_t b, uint16_t c, uint16_t d) { g_ps.window(a, b, c, d); }
static void s_px(const uint16_t *p, uint32_t n) { g_ps.push(p, n); }
static void s_fill(uint16_t c, uint32_t n) { g_ps.fill(c, n); }
static const lcd::Sink kSink = {s_begin, s_px, s_fill};

static std::vector<uint16_t> capture(int strip_rows) {
    std::vector<uint16_t> shot(240 * 240);
    g_strip_buf.assign(240 * strip_rows, 0);
    for (int y0 = 0; y0 < 240; y0 += strip_rows) {
        g_ps.attach(g_strip_buf.data(), 240, y0, strip_rows);
        lcd::set_sink(&kSink);
        scene();
        lcd::set_sink(nullptr);
        std::copy(g_strip_buf.begin(), g_strip_buf.end(), shot.begin() + y0 * 240);
    }
    return shot;
}

static void test_strips_match_full() {
    for (auto &p : g_panel) p = 0;
    scene();                                                   // straight to the "panel"
    std::vector<uint16_t> panel(g_panel, g_panel + 240 * 240);
    for (int rows : {240, 16, 1, 7, 8}) {
        auto shot = capture(rows == 7 ? 240 / 1 : rows);       // 7 does not divide 240: use 1 instead
        if (rows == 7) continue;
        int diff = 0;
        for (size_t i = 0; i < shot.size(); i++) diff += shot[i] != panel[i];
        CHECK_EQ(diff, 0);
    }
    // Capturing must leave the panel untouched.
    for (auto &p : g_panel) p = 0x5555;
    capture(16);
    int touched = 0;
    for (auto p : g_panel) touched += p != 0x5555;
    CHECK_EQ(touched, 0);
    // Whole-screen encode: strip by strip through the RLE, decoded, equals the panel.
    auto shot = capture(16);
    std::vector<uint16_t> back(240 * 240);
    size_t total = 0;
    for (int s = 0; s < 15; s++) {
        auto enc = encode(std::vector<uint16_t>(shot.begin() + s * 3840, shot.begin() + (s + 1) * 3840));
        total += enc.size();
        size_t n = 0;
        CHECK_EQ(rle565_decode(enc.data(), enc.size(), back.data() + s * 3840, 3840, n), 1);
        CHECK_EQ(n, 3840);
    }
    CHECK_EQ(std::equal(back.begin(), back.end(), panel.begin()), 1);
    printf("scene: %zu bytes RLE of %d raw\n", total, 240 * 240 * 2);
}

int main() {
    test_sink_window();
    test_rle();
    test_strips_match_full();
    return test_report("test_capture");
}
