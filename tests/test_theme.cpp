// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fieldbook themes (k2/ui/theme.hpp): the three const tables hold the approved RGB565 values, the boot pick puts one in
// force for every token, and K2 > Theme stores the next boot's theme without repainting anything.
#include <memory>
#include "fake_lcd.h"
#include "k2/ui/theme.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

namespace {

// The approved values (CONCEPT section 3, RGB565-exact).
void test_tables() {
    CHECK_EQ(theme::kNight.bg, 0x10C3);
    CHECK_EQ(theme::kNight.ink, 0xF79C);
    CHECK_EQ(theme::kNight.ink2, 0xAD33);
    CHECK_EQ(theme::kNight.ink3, 0x634E);
    CHECK_EQ(theme::kNight.bg2, 0x2125);
    CHECK_EQ(theme::kNight.rule, 0x3A09);
    CHECK_EQ(theme::kNight.ink_lift, 0xD677);
    CHECK_EQ(theme::kNight.stale, 0x31A7);
    CHECK_EQ(theme::kPaper.bg, 0xF79C);
    CHECK_EQ(theme::kPaper.ink, 0x10C3);
    CHECK_EQ(theme::kPaper.ink2, 0x5B0C);
    CHECK_EQ(theme::kPaper.ink3, 0xA512);
    CHECK_EQ(theme::kPaper.bg2, 0xE71A);
    CHECK_EQ(theme::kPaper.rule, 0xCE57);
    CHECK_EQ(theme::kPaper.ink_lift, 0x31C8);
    CHECK_EQ(theme::kPaper.stale, 0xD698);
    for (const theme::Palette *p : {&theme::kNight, &theme::kPaper}) {   // spot fills are shared
        CHECK_EQ(p->volt, 0xFE85);
        CHECK_EQ(p->volt_d, 0xE5A0);
        CHECK_EQ(p->amp, 0x5E3E);
        CHECK_EQ(p->amp_d, 0x353B);
        CHECK_EQ(p->ok, 0x3DCC);
        CHECK_EQ(p->warn, 0xFC44);
        CHECK_EQ(p->err, 0xC985);
        CHECK_EQ(p->err_d, 0xA124);
        CHECK_EQ(p->focus, p->ink);   // roles resolve to the ink pair: the inverted block
        CHECK_EQ(p->on_focus, p->bg);
        CHECK_EQ(p->focus_bar, 0);
        CHECK_EQ(p->watt, p->ink);
        CHECK_EQ(p->watt_ink, p->bg);
    }
    // Classic: the firmware tokens of 2026-11-12, plus four new trend shades
    CHECK_EQ(theme::kClassic.bg, 0x0000);
    CHECK_EQ(theme::kClassic.focus, 0x0146);
    CHECK_EQ(theme::kClassic.focus_bar, 0x07FF);
    CHECK_EQ(theme::kClassic.accent, 0x07FF);
    CHECK_EQ(theme::kClassic.amp, theme::kClassic.accent);   // user decision: cyan for the A field and for focus
    CHECK_EQ(theme::kClassic.watt, 0xFB2C);
    CHECK_EQ(theme::kClassic.volt_d, 0xD680);
    CHECK_EQ(theme::kClassic.amp_d, 0x069A);
    CHECK_EQ(theme::kClassic.watt_d, 0xDA88);
    CHECK_EQ(theme::kClassic.err_d, 0xC000);
    CHECK_EQ(theme::kClassic.l_focus_t, 0x0146);
    CHECK_EQ(theme::kClassic.surface, 0x18E3);
    CHECK_EQ(theme::kClassic.bar, 0x08A9);
    CHECK_EQ(theme::kAlarmInk, 0xFFFF);
}

void test_stored_value() {
    CHECK_EQ(theme::id_from(0) == theme::Id::Night, 1);
    CHECK_EQ(theme::id_from(1) == theme::Id::Paper, 1);
    CHECK_EQ(theme::id_from(2) == theme::Id::Classic, 1);
    CHECK_EQ(theme::id_from(3) == theme::Id::Night, 1);   // unknown: the default
    CHECK_EQ(theme::name(theme::Id::Paper) == "Paper", 1);
}

// Every token reads the palette in force: after apply() the legacy and the Fieldbook names follow.
void test_apply_moves_every_token() {
    theme::apply(theme::Id::Paper);
    CHECK_EQ(kBg, 0xF79C);
    CHECK_EQ(kText, 0x10C3);
    CHECK_EQ(kInk, 0x10C3);
    CHECK_EQ(kVolt, kText);   // on Paper a unit hue is ink: colour is a block, never a glyph
    CHECK_EQ(kSpotVolt, 0xFE85);
    theme::apply(theme::Id::Classic);
    CHECK_EQ(kBg, 0x0000);
    CHECK_EQ(kVolt, 0xFFE0);
    CHECK_EQ(kFocusT, 0x0146);
    theme::apply(theme::Id::Night);
    CHECK_EQ(kBg, 0x10C3);
    CHECK_EQ(theme::reboot_pending(), 0);
    theme::apply(theme::Id::Classic);
}

// K2 > Theme cycles the stored theme; the one in force stays until the next boot, and the footer says so.
void test_settings_row_stores_for_next_boot() {
    auto ui = std::make_unique<Ui>();
    g_stub = StubState();
    theme::apply(theme::Id::Classic);
    ui->init(1000);
    ui->set_rotation(0, 1000);
    ui->tick(1100);
    ui->goto_page(static_cast<uint8_t>(Page::Settings), 1200);
    for (int i = 0; i < 6; i++) ui->on_button(Button::Plus, ButtonEvent::Short, 1300 + i * 10);
    CHECK_EQ(ui->context().keep.settings_sel, 6);
    ui->on_button(Button::Ok, ButtonEvent::Short, 1400);
    CHECK_EQ(g_stub.prefs.theme, 0);   // Classic -> Night
    CHECK_EQ(theme::chosen() == theme::Id::Night, 1);
    CHECK_EQ(theme::active() == theme::Id::Classic, 1);
    CHECK_EQ(theme::reboot_pending(), 1);
    CHECK_EQ(kBg, 0x0000);   // nothing changes before the reboot
    ui->on_button(Button::Ok, ButtonEvent::Short, 1500);
    CHECK_EQ(g_stub.prefs.theme, 1);   // -> Paper
    ui->on_button(Button::Ok, ButtonEvent::Short, 1600);
    CHECK_EQ(g_stub.prefs.theme, 2);   // -> Classic, the one in force: nothing pending
    CHECK_EQ(theme::reboot_pending(), 0);
    theme::apply(theme::Id::Classic);
}

}  // namespace

int main() {
    test_tables();
    test_stored_value();
    test_apply_moves_every_token();
    test_settings_row_stores_for_next_boot();
    return test_report("test_theme");
}
