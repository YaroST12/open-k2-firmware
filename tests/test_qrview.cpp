// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// qrview (k2/ui/qrview.hpp): the layout rule with the quiet/px parameters the QR scan-test path adds, and the drawing
// of a symbol held in a caller buffer above qr::kMaxVersion (draw_code_raw_in). The defaults (quiet 4, px = the largest
// that fits) are what the share view and the boot screen use, and are pinned unchanged here.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fake_lcd.h"
#include "k2/ui/qr.hpp"
#include "k2/ui/qrview.hpp"
#include "k2/ui/ui_qrtest.hpp"
#include "qr_decode.hpp"
#include "test_util.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;

// Reads the symbol drawn at plan p back through the independent reader; "" when it does not read.
static std::string read_panel(const qrview::Plan& p, int n, int quiet, const std::uint8_t* code) {
    if (p.px == 0 || n == 0) return "";
    const qrdec::Result r = qrdec::decode(n, [&](int x, int y) {
        const int px = p.x0 + (x + quiet) * p.px + p.px / 2;
        const int py = p.y0 + (y + quiet) * p.px + p.px / 2;
        return g_panel[py * 240 + px] == 0x0000;
    });
    (void)code;
    return r.ok ? r.text : "";
}

static void test_plan_defaults_unchanged() {
    // A 224 x 240 content area, min_px 2, quiet 4 (the default), text line under the box. The box and its text line are
    // centred as one block: (240 - 14 - 207) / 2 = 9 px above the box, the same below the text line.
    const qrview::Area portrait{0, 0, 224, 240}, landscape{0, 0, 240, 224};
    constexpr int th = qrview::kLineH + 2;
    qrview::Plan p = qrview::plan_in(portrait, 61, th, 2);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 207); CHECK_EQ(p.x0, 8); CHECK_EQ(p.y0, 9);
    p = qrview::plan_in(landscape, 65, th, 2);
    CHECK_EQ(p.px, 2); CHECK_EQ(p.box, 146); CHECK_EQ(p.x0, (240 - 146) / 2); CHECK_EQ(p.y0, (224 - th - 146) / 2);
    // The boot screen: whole panel, min_px 3, quiet 4, text line.
    p = qrview::plan_in(qrview::Area{}, 65, th, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 219); CHECK_EQ(p.x0, (240 - 219) / 2); CHECK_EQ(p.y0, (240 - th - 219) / 2);
    CHECK_EQ(qrview::plan(57, 14).px, 3);
    // plan() is plan_in() over the whole panel with the defaults.
    CHECK_EQ(qrview::plan(57, 14).box, qrview::plan_in(qrview::Area{}, 57, 14, qrview::kMinModulePx).box);
}

static void test_plan_quiet_px() {
    const qrview::Area full{};   // the whole 240x240 panel, no text line (the qrtest screen)
    // Explicit px, quiet 4: v13 (69 modules) at 3 px -> box 231, centred.
    qrview::Plan p = qrview::plan_in(full, 69, 0, 3, 4, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 231); CHECK_EQ(p.x0, (240 - 231) / 2); CHECK_EQ(p.y0, (240 - 231) / 2);
    // v25 (117) at 2 px needs quiet <= 1: quiet 2 is refused (box 242), quiet 1 fits (box 238).
    CHECK_EQ(qrview::plan_in(full, 117, 0, 2, 2, 2).px, 0);
    p = qrview::plan_in(full, 117, 0, 2, 1, 2);
    CHECK_EQ(p.px, 2); CHECK_EQ(p.box, 238); CHECK_EQ(p.x0, 1);
    // v24 (113) at 2 px quiet 2: box 234 fits.
    p = qrview::plan_in(full, 113, 0, 2, 2, 2);
    CHECK_EQ(p.px, 2); CHECK_EQ(p.box, 234);
    // An explicit px below min_px, or whose box does not fit the area, is refused.
    CHECK_EQ(qrview::plan_in(full, 69, 0, 3, 4, 2).px, 0);          // 2 < min_px 3
    CHECK_EQ(qrview::plan_in(full, 117, 0, 2, 2, 3).px, 0);          // box 363 > 240
    CHECK_EQ(qrview::plan_in(full, 61, 0, 2, 4, 4).px, 0);           // box 276 > 240
    // px 0 keeps the old behaviour (largest that fits) also with a non-default quiet.
    p = qrview::plan_in(full, 61, 0, 2, 2, 0);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, (61 + 4) * 3);
    // A text line still reduces the room with the new parameters.
    CHECK_EQ(qrview::plan_in(full, 65, 14, 3, 4, 0).box, 219);
    CHECK_EQ(qrview::plan_in(full, 69, 14, 3, 4, 0).px, 0);         // 231 > 240 - 14
}

static void test_draw_code_raw_in() {
    // A version 11 symbol (61 modules) in a caller buffer above the qr::Workspace size path: draw it full-screen
    // at an explicit (px, quiet) and read the panel back.
    static qrtest::Workspace w;
    std::uint16_t line[qrview::kPanelPx];
    std::string text;
    for (int i = 0; i < 300; i++) text.push_back(static_cast<char>('a' + i * 7 % 26));
    const int n = qr::encode_version(text, w.code, w.temp, qrtest::kWsBufLen, 11);
    CHECK_EQ(n, 61);

    std::memset(g_panel, 0x55, sizeof g_panel);
    unsigned windows = g_lcd_windows;
    // Full panel, quiet 4, px 3 (the boot-screen geometry, but through the raw-code path).
    int y = qrview::draw_code_raw_in(qrview::Area{}, w.code, n, 0, 3, line, 4, 3);
    const qrview::Plan p = qrview::plan_in(qrview::Area{}, n, 0, 3, 4, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 207); CHECK_EQ(p.x0, 16);
    CHECK_EQ(y, p.y0 + p.box + 2);
    CHECK(g_lcd_windows > windows);
    CHECK_EQ(g_panel[0], 0x5555);                     // outside the box the caller's clear stays
    CHECK_EQ(g_panel[p.y0 * 240 + p.x0], 0xFFFF);     // the quiet zone's first pixel inside the box
    CHECK(read_panel(p, n, 4, w.code) == text);

    // quiet 2, px 3: box 195, centred at x0 = 22.
    std::memset(g_panel, 0x55, sizeof g_panel);
    y = qrview::draw_code_raw_in(qrview::Area{}, w.code, n, 0, 3, line, 2, 3);
    const qrview::Plan p2 = qrview::plan_in(qrview::Area{}, n, 0, 3, 2, 3);
    CHECK_EQ(p2.box, 195); CHECK_EQ(p2.x0, 22);
    CHECK_EQ(y, p2.y0 + p2.box + 2);
    CHECK(read_panel(p2, n, 2, w.code) == text);

    // A box that does not fit draws nothing and reports 0.
    windows = g_lcd_windows;
    CHECK_EQ(qrview::draw_code_raw_in(qrview::Area{}, w.code, n, 0, 2, line, 4, 4), 0);
    CHECK_EQ(g_lcd_windows, windows);

    // The Workspace& overload keeps its defaults (quiet 4, largest px): a version 11 text in the 224 px share area.
    qr::Workspace small;
    std::string big;
    for (int i = 0; i < 300; i++) big.push_back(static_cast<char>('a' + i * 7 % 26));
    const int n2 = qr::encode(big, small);
    CHECK_EQ(n2, 61);                   // version 11
    std::memset(g_panel, 0x55, sizeof g_panel);
    qrview::draw_code_in(qrview::Area{0, 0, 224, 240}, small, n2, 14, 2, line);
    const qrview::Plan p3 = qrview::plan_in(qrview::Area{0, 0, 224, 240}, n2, 14, 2);
    CHECK_EQ(p3.px, 3); CHECK_EQ(p3.box, 207); CHECK_EQ(p3.x0, 8);
    CHECK(read_panel(p3, n2, qrview::kQuietModules, small.code) == big);
}

// The bounding box of the black pixels on the panel: {x0, y0, x1, y1}, x1 / y1 exclusive.
struct Ink { int x0 = 240, y0 = 240, x1 = 0, y1 = 0; };
static Ink ink_box() {
    Ink b;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++)
            if (g_panel[y * 240 + x] == 0x0000) {
                b.x0 = std::min(b.x0, x); b.y0 = std::min(b.y0, y);
                b.x1 = std::max(b.x1, x + 1); b.y1 = std::max(b.y1, y + 1);
            }
    return b;
}

static void test_look_policy_table() {
    // v1..v13: 3 px, quiet 4 (v13 = 231 px); v14..v21: 2 px, quiet 4, the main-camera range; v22 / v23: 2 px quiet 4 and the
    // wide-camera hint; v24: 2 px, quiet 2 (quiet 4 would be 242 px) and the hint; v25 and up: outside the policy.
    for (int v = 1; v <= 13; v++) {
        const qrview::Look l = qrview::look_for(v);
        CHECK_EQ(l.px, 3); CHECK_EQ(l.quiet, 4); CHECK_EQ(l.wide ? 1 : 0, 0);
    }
    CHECK_EQ(qrview::box_of(13, qrview::look_for(13)), 231);
    for (int v = 14; v <= 21; v++) {
        const qrview::Look l = qrview::look_for(v);
        CHECK_EQ(l.px, 2); CHECK_EQ(l.quiet, 4); CHECK_EQ(l.wide ? 1 : 0, 0);
    }
    CHECK_EQ(qrview::box_of(14, qrview::look_for(14)), 162);
    CHECK_EQ(qrview::box_of(21, qrview::look_for(21)), 218);
    for (int v = 22; v <= 24; v++) CHECK_EQ(qrview::look_for(v).wide ? 1 : 0, 1);
    CHECK_EQ(qrview::look_for(22).quiet, 4); CHECK_EQ(qrview::box_of(22, qrview::look_for(22)), 226);
    CHECK_EQ(qrview::look_for(23).quiet, 4); CHECK_EQ(qrview::box_of(23, qrview::look_for(23)), 234);
    CHECK_EQ(qrview::look_for(24).quiet, 2); CHECK_EQ(qrview::box_of(24, qrview::look_for(24)), 234);
    CHECK_EQ(qrview::look_for(24).px, 2);
    CHECK_EQ(qrview::look_for(25).px, 0);
    CHECK_EQ(qrview::look_for(0).px, 0);
    CHECK_EQ(qrview::version_of(65), 12);
    CHECK_EQ(qrview::version_of(113), 24);
    // A caption line fits below the centred box only when the spare margin is a text line high (box <= 212 for 14 px).
    CHECK_EQ(qrview::caption_fits(qrview::box_of(11, qrview::look_for(11)), qrview::kLineH + 2) ? 1 : 0, 1);   // 207
    CHECK_EQ(qrview::caption_fits(qrview::box_of(12, qrview::look_for(12)), qrview::kLineH + 2) ? 1 : 0, 0);   // 219
    CHECK_EQ(qrview::caption_fits(qrview::box_of(20, qrview::look_for(20)), qrview::kLineH + 2) ? 1 : 0, 1);   // 210
    CHECK_EQ(qrview::caption_fits(qrview::box_of(21, qrview::look_for(21)), qrview::kLineH + 2) ? 1 : 0, 0);   // 218
}

// Every (version, px, quiet) the policy can pick is centred on the whole panel within 1 px on both axes (the centring bug:
// y was pinned to the top, so v18 at 2 px left 46 px below the code and 0 above), and the drawn ink agrees with the plan.
static void test_policy_is_centred() {
    static std::uint8_t code[qr::buf_len_for(24)];
    static std::uint8_t tmp[qr::buf_len_for(24)];
    std::uint16_t line[qrview::kPanelPx];
    for (int v = 1; v <= qrview::kLookMaxVersion; v++) {
        const qrview::Look l = qrview::look_for(v);
        const int n = qrview::modules_of(v);
        const qrview::Plan p = qrview::plan_in(qrview::Area{}, n, 0, l.px, l.quiet, l.px);
        CHECK_EQ(p.px, l.px); CHECK_EQ(p.box, qrview::box_of(v, l));
        const int right = qrview::kPanelPx - p.x0 - p.box, below = qrview::kPanelPx - p.y0 - p.box;
        CHECK(std::abs(p.x0 - right) <= 1);
        CHECK(std::abs(p.y0 - below) <= 1);
        // The symbol a payload that fills version v exactly produces draws its finder squares in the three corners: the ink
        // bounding box is the symbol (quiet zone excluded), centred on the same axes.
        std::string text(qrtest::byte_l(v), 'a');
        const int got = qr::encode_version(text, code, tmp, sizeof tmp, v);
        CHECK_EQ(got, n);
        std::memset(g_panel, 0xFF, sizeof g_panel);
        const int y = qrview::draw_code_raw_in(qrview::Area{}, code, got, 0, l.px, line, l.quiet, l.px);
        CHECK_EQ(y, p.y0 + p.box + 2);
        const Ink ink = ink_box();
        CHECK_EQ(ink.x1 - ink.x0, n * l.px);
        CHECK_EQ(ink.y1 - ink.y0, n * l.px);
        CHECK(std::abs(ink.x0 - (240 - ink.x1)) <= 1);
        CHECK(std::abs(ink.y0 - (240 - ink.y1)) <= 1);
    }
}

// v18 at 2 px / quiet 4 (the owner's case): box 194, 23 px on every side.
static void test_v18_is_not_top_left() {
    const qrview::Plan p = qrview::plan_in(qrview::Area{}, qrview::modules_of(18), 0, 2, 4, 2);
    CHECK_EQ(p.box, 194); CHECK_EQ(p.x0, 23); CHECK_EQ(p.y0, 23);
}

int main() {
    test_plan_defaults_unchanged();
    test_plan_quiet_px();
    test_look_policy_table();
    test_policy_is_centred();
    test_v18_is_not_top_left();
    test_draw_code_raw_in();
    return test_report("test_qrview");
}
