// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The palette (k2/ui/theme.hpp) is Classic only (user decision 2026-11-13: Night and Paper are gone, there is no Theme
// row). The table holds the approved RGB565 values and every token of ui_style.hpp is a compile-time constant read
// from it; the Settings list has no Theme row.
#include <memory>
#include "fake_lcd.h"
#include "k2/ui/theme.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

// A token is a constant expression: no boot-time copy, no run-time read.
static_assert(kBg == 0x0000 && kText == 0xFFFF && kVolt == 0xFFE0 && kAmp == 0x07FF && kWatt == 0xFB2C);
static_assert(kVoltT == 0x2120 && kVoltL == 0x6B40 && kAmpT == 0x0124 && kAmpL == 0x0350 && kWattT == 0x3083 && kWattL == 0x6945);

namespace {

// Classic: the firmware tokens of 2026-11-12 (the Flow trends' tint and line pairs among them).
void test_classic_values() {
    const theme::Palette &p = theme::pal;
    CHECK_EQ(p.bg, 0x0000);
    CHECK_EQ(p.bg2, 0x2104);
    CHECK_EQ(p.rule, 0x2945);
    CHECK_EQ(p.ink, 0xFFFF);
    CHECK_EQ(p.ink2, 0x8410);
    CHECK_EQ(p.ink3, 0x4208);
    CHECK_EQ(p.focus, 0x0146);
    CHECK_EQ(p.focus_bar, 0x07FF);
    CHECK_EQ(p.accent, 0x07FF);
    CHECK_EQ(p.amp, p.accent);   // user decision: cyan for the A field and for focus
    CHECK_EQ(p.watt, 0xFB2C);
    CHECK_EQ(p.volt_d, 0xD680);
    CHECK_EQ(p.amp_d, 0x069A);
    CHECK_EQ(p.watt_d, 0xDA88);
    CHECK_EQ(p.err_d, 0xC000);
    CHECK_EQ(p.l_focus_t, 0x0146);
    CHECK_EQ(p.surface, 0x18E3);
    CHECK_EQ(p.bar, 0x08A9);
    CHECK_EQ(p.l_volt_t, 0x2120);
    CHECK_EQ(p.l_volt_l, 0x6B40);
    CHECK_EQ(p.l_stale, 0xC618);
    CHECK_EQ(theme::kAlarmInk, 0xFFFF);
}

// The tokens follow the table, legacy and Fieldbook names alike.
void test_tokens_follow_the_table() {
    CHECK_EQ(kBg, theme::pal.bg);
    CHECK_EQ(kText, theme::pal.ink);
    CHECK_EQ(kInk, theme::pal.ink);
    CHECK_EQ(kVolt, theme::pal.l_volt);
    CHECK_EQ(kSpotVolt, theme::pal.volt);
    CHECK_EQ(kFocusT, 0x0146);
    CHECK_EQ(kPanelBg, theme::pal.bg2);
    CHECK_EQ(kAlarmInk, 0xFFFF);
}

// The K2 place has no Theme row: the list is USB Link ... Wake sleeping port, System state, Boot info.
void test_settings_has_no_theme_row() {
    auto ui = std::make_unique<Ui>();
    g_stub = StubState();
    ui->init(1000);
    ui->set_rotation(0, 1000);
    ui->tick(1100);
    ui->goto_page(static_cast<uint8_t>(Page::Settings), 1200);
    CHECK_EQ(SettingsPage::kRows, 8);
    CHECK_EQ(ui->list_state().count, 8);
    for (int i = 0; i < 20; i++) ui->on_button(Button::Plus, ButtonEvent::Short, 1300 + i * 10);
    CHECK_EQ(ui->context().keep.settings_sel, SettingsPage::kRows - 1);   // clamps at Boot info
    CHECK_EQ(SettingsPage::kBootRow, SettingsPage::kRows - 1);
    CHECK_EQ(SettingsPage::kSystemRow, SettingsPage::kRows - 2);
    for (int i = 0; i < 2; i++) ui->on_button(Button::Minus, ButtonEvent::Short, 1600 + i * 10);   // up to the wake row
    CHECK_EQ(ui->context().keep.settings_sel, 5);
    const auto before = g_stub.prefs_sets;
    ui->on_button(Button::Ok, ButtonEvent::Short, 1800);   // the wake row toggles and stores its own flag
    CHECK_EQ(g_stub.prefs_sets, before + 1);
}

}  // namespace

int main() {
    test_classic_values();
    test_tokens_follow_the_table();
    test_settings_has_no_theme_row();
    return test_report("test_theme");
}
