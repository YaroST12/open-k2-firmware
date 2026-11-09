// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// qrview (k2/ui/qrview.hpp): the layout rule with the quiet/px parameters the QR scan-test path adds, and the drawing
// of a symbol held in a caller buffer above qr::kMaxVersion (draw_code_raw_in). The defaults (quiet 4, px = the largest
// that fits) are what the share view and the boot screen use, and are pinned unchanged here.
#include <array>
#include <cstdio>
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
    // The share view: 224 x 240 content area, min_px 2, quiet 4 (the default), text line under the box.
    const qrview::Area portrait{0, 0, 224, 240}, landscape{0, 0, 240, 224};
    constexpr int th = qrview::kLineH + 2;
    qrview::Plan p = qrview::plan_in(portrait, 61, th, 2);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 207); CHECK_EQ(p.x0, 8); CHECK_EQ(p.y0, 0);
    p = qrview::plan_in(landscape, 65, th, 2);
    CHECK_EQ(p.px, 2); CHECK_EQ(p.box, 146); CHECK_EQ(p.x0, (240 - 146) / 2);
    // The boot screen: whole panel, min_px 3, quiet 4, text line.
    p = qrview::plan_in(qrview::Area{}, 65, th, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 219); CHECK_EQ(p.x0, (240 - 219) / 2);
    CHECK_EQ(qrview::plan(57, 14).px, 3);
    // plan() is plan_in() over the whole panel with the defaults.
    CHECK_EQ(qrview::plan(57, 14).box, qrview::plan_in(qrview::Area{}, 57, 14, qrview::kMinModulePx).box);
}

static void test_plan_quiet_px() {
    const qrview::Area full{};   // the whole 240x240 panel, no text line (the qrtest screen)
    // Explicit px, quiet 4: v13 (69 modules) at 3 px -> box 231, centred.
    qrview::Plan p = qrview::plan_in(full, 69, 0, 3, 4, 3);
    CHECK_EQ(p.px, 3); CHECK_EQ(p.box, 231); CHECK_EQ(p.x0, (240 - 231) / 2); CHECK_EQ(p.y0, 0);
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
    CHECK_EQ(y, p2.box + 2);
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

int main() {
    test_plan_defaults_unchanged();
    test_plan_quiet_px();
    test_draw_code_raw_in();
    return test_report("test_qrview");
}
