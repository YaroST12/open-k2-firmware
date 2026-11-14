// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Memory page (Settings > Memory) through the real Ui: the stack low-water row keeps the normal ink at and above 7000 B, goes
// caution (amber ink, amber footer) under 7000 B and error (red ink, red footer) under 6000 B, an unmeasured snapshot colours
// nothing, the tone shows in all four rotations, and a steady snapshot costs no pixels. With K2_DUMP set the cases are also
// written as k2_mock_memory_<case>_r<rotation>.panel (tools/panel2png.py), the host render of the page.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "fake_lcd.h"
#include "k2/core/memstat.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "test_util.h"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;

static int count_px(std::uint16_t c, int x0 = 0, int y0 = 0, int x1 = LCD_WIDTH, int y1 = LCD_HEIGHT) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) n += g_panel[y * LCD_WIDTH + x] == c;
    return n;
}

static memstat::Snapshot snap(std::uint32_t psp_low, bool ok = true) {
    memstat::Snapshot m;
    m.ram_total = 98304; m.ram_static = 64000; m.psp_size = 10944; m.msp_size = 1344;
    m.psp_now = psp_low + 1600 > m.psp_size ? m.psp_size : psp_low + 1600;
    m.psp_low = psp_low; m.msp_low = 800;
    m.fs_blocks = 146; m.fs_block_bytes = 2048; m.fs_used = 40; m.fs_valid = ok;
    m.image_bytes = 700000; m.flash_bytes = 944128; m.ok = ok;
    return m;
}

static void dump(const char *what, int rot) {
    if (const char *dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_mock_memory_%s_r%d.panel", dir, what, rot);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

static void settle(Ui &ui, std::uint32_t &t) {
    ui.tick(t += 100);
    for (int i = 0; i < 400 && ui.painting(); i++) ui.tick(t += 20);
    ui.tick(t += 100);
}

// One rotation: every case settles to the tone it should have, the other tones are absent.
static void test_rotation(int rot) {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    Ui ui;
    ui.init(0);
    ui.set_rotation(2, 6000);
    ui.set_rotation(static_cast<std::uint8_t>(rot), 6000);
    std::uint32_t t = 7000;
    // Bench > K2 with the Memory row focused: the row that opens the page (render only)
    ui.context().keep.settings_sel = SettingsPage::kMemoryRow;
    ui.open_place(bench::Place::K2, t);
    settle(ui, t);
    CHECK_EQ(int(ui.page()), int(Page::Settings));
    dump("settings_row", rot);
    ui.context().keep.settings_sel = 0;
    ui.goto_page(static_cast<std::uint8_t>(Page::Tools), t);
    CHECK(ui.push_menu(Menu::Memory, t));
    const struct { const char *what; std::uint32_t low; bool ok; int tone; } cases[] = {
        {"ok", 7400, true, 0}, {"edge7000", 7000, true, 0}, {"caution", 6999, true, 1}, {"edge6000", 6000, true, 1},
        {"error", 5999, true, 2}, {"worst", 5312, true, 2}, {"unmeasured", 0, false, 0},
    };
    for (const auto &c : cases) {
        stub_memstat_set(snap(c.low, c.ok));
        settle(ui, t);
        // Classic footer tones: caution is the amber tint (kCautionT) with an amber bar, error is solid kError with black ink.
        const int amber_fill = count_px(kCautionT), red_fill = count_px(kError);
        if (c.tone == 0) {
            CHECK_EQ(amber_fill, 0);
            CHECK_EQ(red_fill, 0);
            CHECK_EQ(count_px(kCaution), 0);   // a normal value is never amber ...
            CHECK_EQ(count_px(kError), 0);     // ... or red
        } else if (c.tone == 1) {
            CHECK(amber_fill > 2000);          // the footer band is the tone fill
            CHECK_EQ(red_fill, 0);
            CHECK(count_px(kCaution) > 40);    // the value's ink, and the footer bar
        } else {
            CHECK(red_fill > 2000);            // the solid footer band and the value's ink
            CHECK_EQ(amber_fill, 0);
            CHECK_EQ(count_px(kCaution), 0);
        }
        if (rot == 0) {
            // the Stack low row (the second): its value box is x 90..218, one 22 px row under the first
            const int y0 = 24 + 22, y1 = y0 + 22;
            const int amber = count_px(kCaution, 90, y0, 218, y1), red = count_px(kError, 90, y0, 218, y1);
            CHECK_EQ(amber > 20, c.tone == 1);
            CHECK_EQ(red > 20, c.tone == 2);
        }
        if (c.tone != 0 || c.what[0] == 'o') dump(c.what, rot);
        // a steady snapshot repaints nothing
        const unsigned before = g_lcd_pixels;
        for (int i = 0; i < 20; i++) ui.tick(t += 100);
        CHECK_EQ(g_lcd_pixels, before);
    }
    stub_memstat_set(memstat::Snapshot{});
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    settle(ui, t);
    CHECK_EQ(int(ui.page()), int(Page::Tools));
}

int main() {
    for (int rot = 0; rot < 4; rot++) test_rotation(rot);
    return test_report("test_ui_memory");
}
