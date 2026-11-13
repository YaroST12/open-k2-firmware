// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> PD diagnosis (k2/ui/ui_diagnosis.hpp) through the real Ui: the verdict cell of each severity, the footer
// tone of a report (red for a Fail, amber for a Warn, green only for a healthy link with the power it needs), the rows
// of case 1 (Legion rear port on a 60 W hub) in the screen form's order, PLUS / MINUS moving the focus, the one waiting
// row before any session, and BACK popping the page.
#include <cstdint>

#include "diag_fixture.h"
#include "fake_lcd.h"
#include "k2/pd/diag_screen.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_diagnosis.hpp"
#include "k2/ui/ui_style.hpp"
#include "test_util.h"
#include "ui_stubs.h"

#define CHECK(cond) CHECK_EQ((cond) ? 1 : 0, 1)

using namespace k2;
namespace diag = k2::pd::diag;

static int count_px(std::uint16_t c) {
    int n = 0;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) n += g_panel[i] == c;
    return n;
}

static void test_cell_of() {
    CHECK_EQ(int(DiagnosisPage::cell_of(diag::Sev::Fail)), int(kit::Verdict::Fail));
    CHECK_EQ(int(DiagnosisPage::cell_of(diag::Sev::Warn)), int(kit::Verdict::Partial));
    CHECK_EQ(int(DiagnosisPage::cell_of(diag::Sev::Info)), int(kit::Verdict::None));
    CHECK_EQ(int(DiagnosisPage::cell_of(diag::Sev::Ok)), int(kit::Verdict::Pass));
}

static void test_tone_of() {
    const diag::Report case1 = diag_fixture::case1();
    CHECK_EQ(int(DiagnosisPage::tone_of(case1)), int(FooterTone::Error));
    diag::Report warn = case1;
    for (unsigned i = 0; i < warn.n; i++)
        if (warn.f[i].sev == diag::Sev::Fail) warn.f[i].sev = diag::Sev::Warn;
    CHECK_EQ(int(DiagnosisPage::tone_of(warn)), int(FooterTone::Caution));
    CHECK_EQ(int(DiagnosisPage::tone_of(diag::Report{})), int(FooterTone::Info));   // no session yet
    diag::Report ok;
    ok.empty = false;
    ok.link = 0;
    ok.power = diag::kPowerOk;
    CHECK_EQ(int(DiagnosisPage::tone_of(ok)), int(FooterTone::Ok));
    ok.link = 1;                                                                    // a degraded link is not green
    CHECK_EQ(int(DiagnosisPage::tone_of(ok)), int(FooterTone::Info));
    ok.link = 0;
    ok.power = diag::kPowerUnseen;                                                  // nor a contract it never saw
    CHECK_EQ(int(DiagnosisPage::tone_of(ok)), int(FooterTone::Info));
}

static Ui &fresh_ui() {
    static Ui ui;
    g_stub.ms = 1000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_stub.ms);
    return ui;
}

static void test_page_case1() {
    const diag::Report case1 = diag_fixture::case1();
    g_stub.diag_report = case1;
    g_stub.diag_state = 1;
    Ui &ui = fresh_ui();
    ui.tick(1000);
    std::uint32_t t = 1100;
    CHECK(ui.push_menu(Menu::Diagnosis, t));
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
    CHECK_EQ(int(ui.menu()), int(Menu::Diagnosis));
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<int>(Menu::Diagnosis));
    const unsigned rows = diag::screen::row_count(case1);
    CHECK(rows >= 6);
    CHECK_EQ(ui.diagnosis_page().list_state().count, int(rows));
    CHECK_EQ(ui.diagnosis_page().selected(), 0);
    // The first row is the worst finding: a Fail cell (solid red) and the solid red fact line.
    diag::screen::Row r0;
    CHECK(diag::screen::row(case1, 0, r0));
    CHECK_EQ(int(r0.sev), int(diag::Sev::Fail));
    CHECK(count_px(kError) > 3000);                  // the fact line, 224 x 18, solid red
    CHECK(count_px(kError) > 100);                   // the fail cell
    // PLUS walks down to the last row and stops there; MINUS walks back.
    for (unsigned i = 0; i < rows + 3; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 1300 + i);
    CHECK_EQ(ui.diagnosis_page().selected(), int(rows) - 1);
    ui.on_button(Button::Minus, ButtonEvent::Short, 1400);
    CHECK_EQ(ui.diagnosis_page().selected(), int(rows) - 2);
    ui.on_button(Button::Back, ButtonEvent::Short, 1500);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
}

static void test_page_empty() {
    g_stub.diag_report = diag::Report{};
    g_stub.diag_state = 0;
    Ui &ui = fresh_ui();
    ui.tick(1000);
    std::uint32_t t = 1100;
    CHECK(ui.push_menu(Menu::Diagnosis, t));
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
    CHECK_EQ(ui.diagnosis_page().list_state().count, 1);   // the waiting row
    CHECK_EQ(count_px(kErrorT), 0);
    CHECK_EQ(count_px(kCautionT), 0);
    CHECK_EQ(count_px(kOkT), 0);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1300);
    CHECK_EQ(ui.diagnosis_page().selected(), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);
    CHECK_EQ(int(ui.menu()), int(Menu::Count));
}

// The Log place: the stamp names the top finding, so the list does not repeat it (it starts at the second finding) and the
// finding's evidence note moves under the stamp's word; with no finding to name, the list is the whole screen form.
static void test_place_stamp_replaces_top() {
    const diag::Report case1 = diag_fixture::case1();
    CHECK(diag::screen::names_top_finding(case1));
    g_stub.diag_report = case1;
    g_stub.diag_state = 1;
    Ui &ui = fresh_ui();
    ui.tick(1000);
    std::uint32_t t = 1100;
    ui.open_place(bench::Place::Log, t);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
    const unsigned rows = diag::screen::row_count(case1);
    CHECK_EQ(ui.diagnosis_page().list_state().count, int(rows));   // the screen rows less the stamped one, plus the Black Box row
    // the stamp's sub-line (y = stamp + 31 .. 43) carries ink: the top finding's note
    const int y0 = kit::kBelowBar + DiagnosisPage::kStampSubY;
    int ink = 0;
    const std::uint16_t ground = g_panel[(y0 + 1) * LCD_WIDTH + 226];
    for (int y = y0; y < y0 + 12; y++)
        for (int x = DiagnosisPage::kStampWordX; x < 222; x++) ink += g_panel[y * LCD_WIDTH + x] != ground;
    CHECK(ink > 40);

    diag::Report healthy{};
    healthy.empty = false; healthy.link = 0; healthy.power = diag::kPowerOk;
    healthy.quiet_contract = healthy.quiet_link = true;
    CHECK(!diag::screen::names_top_finding(healthy));
    g_stub.diag_report = healthy;
    ui.tick(t += 100);
    ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 20);
    CHECK_EQ(ui.diagnosis_page().list_state().count, int(diag::screen::row_count(healthy)) + 1);
    ui.on_button(Button::Back, ButtonEvent::Short, t + 100);
}

int main() {
    test_cell_of();
    test_tone_of();
    test_page_case1();
    test_page_empty();
    test_place_stamp_replaces_top();
    return test_report("test_ui_diagnosis");
}
