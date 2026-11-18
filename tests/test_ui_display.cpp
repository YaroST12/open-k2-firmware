// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for Settings > Display: the brightness step, the value text, and the page driven through the real Ui.
#include <string>

#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/core/idle_hw.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_display.hpp"
#include "k2/ui/ui_pages.hpp"

using namespace k2;

std::string span_text(std::uint16_t s, bool brief) {
    char b[16];
    return std::string(b, uidisp::format_span(b, s, brief));
}

void test_step() {
    using uidisp::step_brightness;
    CHECK_EQ(int(step_brightness(100, -1)), 90);
    CHECK_EQ(int(step_brightness(100, 1)), 100);
    CHECK_EQ(int(step_brightness(10, -1)), 10);
    CHECK_EQ(int(step_brightness(10, 1)), 20);
    CHECK_EQ(int(step_brightness(55, 1)), 60);   // a remote-set level snaps to the grid
    CHECK_EQ(int(step_brightness(55, -1)), 50);
    CHECK_EQ(int(step_brightness(0, -1)), 10);
    CHECK_EQ(int(step_brightness(3, 1)), 10);
    CHECK_COND(!uidisp::can_step(100, 1) && uidisp::can_step(100, -1));
    CHECK_COND(!uidisp::can_step(10, -1) && uidisp::can_step(10, 1));
}

void test_text() {
    CHECK_STR(span_text(0, false).c_str(), "Never");
    CHECK_STR(span_text(30, false).c_str(), "30 s");
    CHECK_STR(span_text(60, false).c_str(), "60 s");
    CHECK_STR(span_text(120, false).c_str(), "2 min");
    CHECK_STR(span_text(300, false).c_str(), "5 min");
    CHECK_STR(span_text(1800, false).c_str(), "30 min");
    CHECK_STR(span_text(3600, false).c_str(), "1 h");
    CHECK_STR(span_text(150, false).c_str(), "2 min");   // minutes round down
    CHECK_STR(span_text(0, true).c_str(), "off");
    CHECK_STR(span_text(60, true).c_str(), "60s");
    CHECK_STR(span_text(300, true).c_str(), "5m");
    CHECK_STR(span_text(65535, true).c_str(), "18h");
    for (std::size_t i = 0; i < uidisp::kDimAfter.size(); i++) {
        CHECK_STR(span_text(uidisp::kDimAfter[i], false).c_str(), uidisp::kDimNames[i]);
        CHECK_STR(span_text(uidisp::kOffAfter[i], false).c_str(), uidisp::kOffNames[i]);
    }
    idle::Config c;
    char b[24];
    CHECK_STR((uidisp::format_summary(b, c), b), "100%, 60s/5m");
    c.brightness = 30; c.dim_after_s = 0; c.off_after_s = 0;
    CHECK_STR((uidisp::format_summary(b, c), b), "30%, off/off");
    c.brightness = 100; c.dim_after_s = 119; c.off_after_s = 65535;
    CHECK_COND(uidisp::format_summary(b, c) <= 16);   // the longest the row can show
    char tiny[4];
    CHECK_COND(uidisp::format_summary(tiny, c) <= 3);   // a short buffer is cut, never overrun
}

static Ui &fresh_ui() {
    static Ui ui;
    g_stub.ms = 10000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_stub.ms);
    return ui;
}

static uint32_t g_t;
static void press(Ui &ui, Button b) {
    g_t += 30;
    ui.on_button(b, ButtonEvent::Short, g_t);
    while (ui.painting()) ui.tick(g_t += 2);
}

static const char *label(Ui &ui, Button b) {
    static char buf[24];
    label_text(buf, ui.labels().b[static_cast<int>(b)], label_max_chars(b));
    return buf;
}
static bool dim(Ui &ui, Button b) { return ui.labels().b[static_cast<int>(b)].dim; }

static void open_display(Ui &ui) {
    ui.goto_page(5, g_t += 100);   // Settings
    while (ui.painting()) ui.tick(g_t += 2);
    for (int i = 0; i < SettingsPage::kDisplayRow; i++) press(ui, Button::Plus);
    CHECK_STR(label(ui, Button::Ok), "Open");
    press(ui, Button::Ok);
    CHECK_EQ(ui.menu() == Menu::Display, 1);
}

static void test_page() {
    idle_hw::set_config(idle::Config{});
    Ui &ui = fresh_ui();
    g_t = 20000;
    open_display(ui);
    CHECK_EQ(ui.list_state().count, 3);

    // Brightness: OK opens the adjust mode; [+] / [-] step by 10 and go dim at the limits; BACK leaves the mode, not the page.
    CHECK_STR(label(ui, Button::Ok), "Set");
    press(ui, Button::Ok);
    CHECK_STR(label(ui, Button::Plus), "+");
    CHECK_STR(label(ui, Button::Ok), "Done");
    CHECK_EQ(dim(ui, Button::Plus) && !dim(ui, Button::Minus), 1);   // at 100 %
    press(ui, Button::Plus);
    CHECK_EQ(idle_hw::config().brightness, 100);
    press(ui, Button::Minus);
    CHECK_EQ(idle_hw::config().brightness, 90);
    for (int i = 0; i < 12; i++) press(ui, Button::Minus);
    CHECK_EQ(idle_hw::config().brightness, 10);
    CHECK_EQ(dim(ui, Button::Minus) && !dim(ui, Button::Plus), 1);
    press(ui, Button::Plus);
    CHECK_EQ(idle_hw::config().brightness, 20);
    press(ui, Button::Back);
    CHECK_EQ(ui.menu() == Menu::Display, 1);
    CHECK_STR(label(ui, Button::Plus), "Dn");   // the list again

    // Dim after: OK opens the pick list on the stored value (60 s), OK on an option stores it and returns to the list.
    press(ui, Button::Plus);
    CHECK_STR(label(ui, Button::Ok), "Pick");
    press(ui, Button::Ok);
    CHECK_EQ(ui.list_state().count, 5);
    CHECK_STR(label(ui, Button::Ok), "Select");
    press(ui, Button::Minus);   // 30 s
    press(ui, Button::Ok);
    CHECK_EQ(idle_hw::config().dim_after_s, 30);
    CHECK_EQ(idle_hw::config().off_after_s, 300);   // the other timeout is untouched
    CHECK_EQ(ui.list_state().count, 3);
    press(ui, Button::Ok);
    press(ui, Button::Minus);   // Never
    CHECK_EQ(dim(ui, Button::Minus), 1);
    press(ui, Button::Ok);
    CHECK_EQ(idle_hw::config().dim_after_s, 0);

    // Screen off after: BACK from a pick list changes nothing and stays on the page; a value at the end dims [+].
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    press(ui, Button::Plus);
    press(ui, Button::Plus);   // 5 min -> 10 min -> 30 min
    CHECK_EQ(dim(ui, Button::Plus), 1);
    press(ui, Button::Back);
    CHECK_EQ(ui.menu() == Menu::Display, 1);
    CHECK_EQ(idle_hw::config().off_after_s, 300);
    press(ui, Button::Ok);
    press(ui, Button::Plus);
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    CHECK_EQ(idle_hw::config().off_after_s, 1800);
    press(ui, Button::Back);   // BACK from the list leaves the page
    CHECK_EQ(ui.menu() == Menu::Display, 0);
    CHECK_EQ(ui.page() == Page::Settings, 1);
    CHECK_EQ(idle_hw::config().brightness, 20);
    idle_hw::set_config(idle::Config{});
}

int main() {
    test_step();
    test_text();
    test_page();
    return test_report("test_ui_display");
}
