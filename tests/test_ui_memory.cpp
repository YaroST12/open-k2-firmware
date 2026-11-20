// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Memory page (Settings > Memory) through the real Ui: the FS free row keeps the normal ink at and above 20 % of the volume
// free, goes caution (amber ink, amber footer) under 20 % and error (red ink, red footer) under 10 %; the stack rows are plain
// numbers however low (user decision 2026-11-14); an unmeasured snapshot colours nothing, the tone shows in all four
// rotations, and a steady snapshot costs no pixels. With K2_DUMP set the cases are also
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

// Ground between a row's dot leader and its value, rotation 0: from the last column of the rightmost 2x2 kInk3 dot left of
// x 160 to the first non-ground pixel after it (the value's first ink). 999 = no leader on the row.
static int leader_gap(int y0, int y1) {
    const auto px = [](int x, int y) { return g_panel[y * LCD_WIDTH + x]; };
    int dot_end = -1;
    for (int y = y0; y + 1 < y1; y++)
        for (int x = kOppRailThickness; x + 1 < kOppRailThickness + 160; x++)
            if (px(x, y) == kInk3 && px(x + 1, y) == kInk3 && px(x, y + 1) == kInk3 && px(x + 1, y + 1) == kInk3 && x + 1 > dot_end)
                dot_end = x + 1;
    if (dot_end < 0) return 999;
    int ink = 999;
    for (int y = y0; y < y1; y++)
        for (int x = dot_end + 1; x < 224; x++)
            if (px(x, y) != kBg) { if (x < ink) ink = x; break; }
    return ink - dot_end - 1;
}

static memstat::Snapshot snap(std::uint32_t fs_free, std::uint32_t psp_low = 7400, bool ok = true) {
    memstat::Snapshot m;
    m.ram_total = 98304; m.ram_static = 64000; m.psp_size = 10944; m.msp_size = 1344;
    m.psp_now = psp_low + 1600 > m.psp_size ? m.psp_size : psp_low + 1600;
    m.psp_low = psp_low; m.msp_low = 800;
    m.fs_blocks = 146; m.fs_block_bytes = 2048; m.fs_used = 146 - fs_free; m.fs_valid = ok;
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
    ui.goto_page(static_cast<std::uint8_t>(Page::Lines), t);
    CHECK(ui.push_menu(Menu::Memory, t));
    // 146 blocks: 30 free is 20.5 % (fine), 29 is 19.9 % (caution), 15 is 10.3 % (caution), 14 is 9.6 % (error).
    const struct { const char *what; std::uint32_t free; std::uint32_t low; bool ok; int tone; } cases[] = {
        {"ok", 106, 7400, true, 0},    {"edge30", 30, 7400, true, 0}, {"caution", 29, 7400, true, 1}, {"edge15", 15, 7400, true, 1},
        {"error", 14, 7400, true, 2},  {"worst", 0, 7400, true, 2},   {"stack5312", 106, 5312, true, 0},
        {"unmeasured", 106, 0, false, 0},
    };
    for (const auto &c : cases) {
        stub_memstat_set(snap(c.free, c.low, c.ok));
        settle(ui, t);
        // Toast pill tones: caution is the amber tint (kCautionT) with an amber border, error is kErrorT with a red border.
        const int amber_fill = count_px(kCautionT), red_fill = count_px(kErrorT);
        if (c.tone == 0) {
            CHECK_EQ(amber_fill, 0);
            CHECK_EQ(red_fill, 0);
            CHECK_EQ(count_px(kCaution), 0);   // a normal value is never amber ...
            CHECK_EQ(count_px(kError), 0);     // ... or red
        } else if (c.tone == 1) {
            CHECK(amber_fill > 400);           // the toast pill is the tone fill
            CHECK_EQ(red_fill, 0);
            CHECK(count_px(kCaution) > 40);    // the value's ink, and the toast border
        } else {
            CHECK(red_fill > 400);             // the toast pill fill
            CHECK(count_px(kError) > 40);      // the value's ink and toast border
            CHECK_EQ(amber_fill, 0);
            CHECK_EQ(count_px(kCaution), 0);
        }
        if (rot == 0) {
            // the FS free row (the sixth): its value box is x (16+76)..(16+204); the Stack low row (the second) never takes a tone
            const int vx0 = kOppRailThickness + 76, vx1 = kOppRailThickness + 204;
            const int y0 = kit::kBodyY + 22 * 5, y1 = y0 + 22;
            CHECK_EQ(count_px(kCaution, vx0, kit::kBodyY + 22, vx1, kit::kBodyY + 44) + count_px(kError, vx0, kit::kBodyY + 22, vx1, kit::kBodyY + 44), 0);
            const int amber = count_px(kCaution, vx0, y0, vx1, y1), red = count_px(kError, vx0, y0, vx1, y1);
            CHECK_EQ(amber > 20, c.tone == 1);
            CHECK_EQ(red > 20, c.tone == 2);
        }
        if (rot == 0 && c.what[0] == 'o') {
            // The leader stops at least one font-16 character (8 px) short of its value on every row, the widest value
            // included: FS free "212 KB (106 blk)" fills the whole 16-character box (it read as part of the leader at 4 px).
            int worst = 999;
            for (int i = 0; i < memstat::kRows; i++) {
                const int g = leader_gap(kit::kBodyY + 22 * i, kit::kBodyY + 22 * (i + 1));
                if (g < worst) worst = g;
            }
            CHECK_EQ(worst >= 8 && worst < 999, true);
            if (worst < 8) std::printf("narrowest leader gap %d px\n", worst);
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
    CHECK_EQ(int(ui.page()), int(Page::Settings));   // Memory over Lines belongs to the K2 place: BACK leaves to its ring page
    CHECK_EQ(ui.ring_view(), true);
}

int main() {
    for (int rot = 0; rot < 4; rot++) test_rotation(rot);
    return test_report("test_ui_memory");
}
