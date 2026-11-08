// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "fake_lcd.h"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "ui_stubs.h"
#include <cstdio>

using namespace k2;

int main() {
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.pm.flow = power::Flow::Forward;
    Ui ui;
    ui.init(1000);
    ui.tick(1300);
    g_lcd_pixels = 0;
    g_stub.pm.vbus_mv += 10;
    g_stub.pm.power_mw += 15;
    ui.tick(1700);   // past the 250 ms display filter
    CHECK_EQ(g_lcd_pixels > 0, true);
    // Home v3 repaints three 168x32 strips (16k pixels) next to the digits at most 4 times a second.
    CHECK_EQ(g_lcd_pixels <= 40000, true);
    std::printf("Home numeric update: %u pixels (budget 40000)\n", g_lcd_pixels);

    CHECK_EQ(ui.goto_page(kPdPage, 1800), true);
    ui.tick(2100);
    g_lcd_pixels = 0;
    g_stub.pm.vbus_mv += 10;
    ui.tick(2200);
    CHECK_EQ(g_lcd_pixels > 0, true);
    CHECK_EQ(g_lcd_pixels <= 6000, true);
    std::printf("Reader numeric update: %u pixels (budget 6000)\n", g_lcd_pixels);
    return test_report("test_ui_update_budget");
}
