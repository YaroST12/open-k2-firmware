// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UI mockup harness: renders every page (and the charger-test results view) 1:1 into $K2_DUMP as
// k2_mock_<name>.panel (240x240 RGB565 LE). Convert with tools/panel2png.py or view as text with
// tools/panel2ascii.py. This is the fast design-iteration loop: edit src/ui*.cpp, rebuild this host
// test, re-render, no flashing needed. Run: K2_DUMP=host-build make host-build/test_ui_mockup && ./host-build/test_ui_mockup
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "diag_fixture.h"
#include "k2/pd/diag_screen.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/emk_spoof.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace k2;
namespace ct = charger_test;

constexpr int kStageReview = 4, kStageResults = 6;   // ChargerTestPage::Stage values

static void dump(const char* name) {
    if (const char* dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_mock_%s.panel", dir, name);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

static void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20040; g_stub.pm.current_ma = 1834; g_stub.pm.power_mw = 36753;
    g_stub.pm.flow = power::Flow::Forward;   // what power::flow_of derives from the positive current
    g_stub.caps.valid = true; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2; g_stub.caps.raw_header = 0x1181;
    g_stub.caps.num_pdos = 7;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},   {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 12000, 12000, 3000, 36000}, {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},
        {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},{pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000},
        {pd::PdoType::Pps, 0, 3300, 20000, 5000, 100000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
    g_stub.cc = 1;
    g_stub.em.valid = true; g_stub.em.max_voltage_v = 20; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 100;
    g_stub.em.vendor_str = "CUKTECH"; g_stub.em.vid = 0x2B01;
    g_stub.cable_ans = k2::emk_variant::answered_from(1, 0);   // orientation A, PD3 + SVDM 2.0: the first attempt
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 5;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
}

static void render(Ui& ui, uint32_t& t, const char* name) {
    ui.tick(t); t += 200; ui.tick(t); t += 200;
    dump(name);
}

int main() {
    fixture();
    Ui ui;
    ui.init(0);
    // The design canvas is the bottom-strip 240x220 layout (rotation 3 = buttons at the bottom);
    // the device boot default is rotation 0 (buttons on the physical right edge).
    ui.configure_rotation(false, 3);
    uint32_t t = 1000;

    // Home / Detail
    ui.goto_page(uint8_t(Page::Home), t); render(ui, t, "home");
    ui.goto_page(uint8_t(Page::Detail), t + 100); render(ui, t, "detail");

    // Connection (the old PD page), three tabs
    ui.goto_page(kPdPage, t + 100);
    static const char* const tabs[] = {"pd_source", "pd_cable", "pd_sink"};
    for (int i = 0; i < 3; i++) { render(ui, t, tabs[i]); ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100; }

    // The Cable tab of a cable that gave no identity, alone: a chip's pull-down on a CC line ("chip silent") versus none
    // ("no chip"), and the older / unprobed reading. Charger and contract are gone, as they are with the cable alone.
    {
        static StubState saved;
        saved = g_stub;
        g_stub = {};   // a bare K2: no charger, no contract, no meter (a fresh Ui: the main one latched "inline" from the contract above)
        g_stub.cable_read = {emk_attached::Mode::Alone, 0};
        static Ui bare;
        bare.init(0);
        bare.configure_rotation(false, 3);
        uint32_t tb = 1000;
        static const struct { raclass::PullDown p; const char* name; } kScenes[] = {
            {raclass::PullDown::Cc2, "pd_cable_chip_silent"}, {raclass::PullDown::None, "pd_cable_no_chip"},
            {raclass::PullDown::NotProbed, "pd_cable_not_probed"}};
        for (const auto& s : kScenes) {
            g_stub.pull_down = s.p;
            bare.goto_page(static_cast<uint8_t>(kConnTabAlias + 1), tb + 100); render(bare, tb, s.name);
        }
        g_stub = saved;
    }

    // Diag / Lines / Tools / Settings
    ui.goto_page(kDiagPage, t + 100); render(ui, t, "diag");
    ui.goto_page(uint8_t(Page::Lines), t + 100); render(ui, t, "lines");
    ui.goto_page(uint8_t(Page::Tools), t + 100); render(ui, t, "tools");
    for (int i = 0; i < 7; i++) { ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100; }
    render(ui, t, "tools_end");   // last of the eight 1-line rows selected
    ui.goto_page(uint8_t(Page::Settings), t + 100); render(ui, t, "settings");
    for (int i = 0; i < 4; i++) { ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100; }
    render(ui, t, "settings_end");   // last of the five 2-line rows selected
    ui.goto_page(kMenuPageBase + static_cast<uint8_t>(Menu::UsbLink), t + 100); render(ui, t, "usblink");
    ui.goto_page(kMenuPageBase + static_cast<uint8_t>(Menu::Autorotate), t + 100); render(ui, t, "autorotate");
    ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100;
    render(ui, t, "autorotate_2");   // the second choice selected

    // ChargerTest: setup page, then a full PD-only run to the results view. START refuses a live load, so the
    // meter reads an idle 5 V line (the same thing the run's own VBUS-safe check needs).
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
    for (uint32_t i = 0; i < k2::pd::kInlineQuietMs / 100 + 20; ++i) { t += 100; ui.tick(t); }   // the inline latch (quiet window) and the readout filters settle
    ui.goto_page(uint8_t(Page::ChargerTest), t + 100); render(ui, t, "ct_setup");
    static const uint8_t kPdOnly[] = {uint8_t(ct::Unit::Pd), uint8_t(ct::Unit::PdLow)};
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    for (int i = 0; i < ct::kMaxSteps; ++i) ui.on_button(Button::Minus, ButtonEvent::Short, t);   // to the top
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;   // Next -> Cable
    render(ui, t, "ct_cable");
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;   // Next -> Review
    render(ui, t, "ct_review");
    // Each scene is checked against the stage it is named after, so a change of the START flow cannot silently
    // retitle a scene. The first START press opens the confirm window on Review (no dialog: the cable identity was
    // chosen on the Cable step).
    CHECK_EQ(ui.charger_test_stage(), kStageReview);
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;   // START: the confirm window
    CHECK_EQ(ui.charger_test_stage(), kStageReview);
    render(ui, t, "ct_confirm");
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;   // confirm: the run starts
    CHECK_EQ(ui.charger_test_busy(), true);
    for (; t <= 400000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(ui.charger_test_stage(), kStageResults);
    render(ui, t, "ct_results");

    // Device twins: the list against a CUKTECH C1 (fixed 5-20 V 3 A, PPS 3.3-11 V 3 A), then the confirm, a run and its result.
    fixture();
    stub_twin_reset();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;   // idle line: the run refuses a device inline
    for (uint32_t i = 0; i < k2::pd::kInlineQuietMs / 100 + 20; ++i) { t += 100; ui.tick(t); }
    g_stub.caps = {};
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2;
    for (uint32_t w : {0x0A01912Cu, 0x0002D12Cu, 0x0003C12Cu, 0x0004B12Cu, 0x0006412Cu, 0xC8DC323Cu, 0xC990323Cu})
        g_stub.caps.pdos[g_stub.caps.num_pdos++] = pd::decode_pdo(w);
    stub_twin_add("Pixel 9", {0x3601912C, 0x0002D12C, 0xC0DC323C});
    stub_twin_add("Laptop", {0x3601912C});
    stub_twin_add("OP15", {0x2601912C, 0x0002D12C, 0x0003C12C, 0xC0DC3264});
    stub_twin_add("Galaxy S24 Ultra case", {0x2601912C, 0x0002D12C, 0xC0DC3264});
    ui.goto_page(uint8_t(Page::Tools), t + 100);
    ui.push_menu(Menu::Twin, t + 100); render(ui, t, "twin_list");
    ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100;
    ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100;
    render(ui, t, "twin_cable");                                         // OP15 focused: limited by the cable
    ui.on_button(Button::Minus, ButtonEvent::Short, t); t += 100;
    ui.on_button(Button::Minus, ButtonEvent::Short, t); t += 100;
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    render(ui, t, "twin_confirm");                                       // first OK: the row is armed
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    ui.tick(t); t += 150; ui.tick(t);
    dump("twin_running");
    for (int i = 0; i < 600 && twin_hw::live_running(); i++) { t += 25; ui.tick(t); }
    render(ui, t, "twin_done");                                          // pass cell, the live text in the note, the tally
    for (int i = 0; i < 5; i++) { ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100; }
    render(ui, t, "twin_learn");                                         // the Learn row
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    render(ui, t, "twin_learn_refused");                                 // nothing identified: the footer says so
    g_stub.caps = {};
    stub_twin_reset();
    ui.tick(t); t += 400; ui.tick(t);
    render(ui, t, "twin_empty");

    // Loop probe: idle (no stored result), the armed START, a step in flight, the CC step, the QR view it ends in, the
    // table after BACK from it, and a refusal (VBUS above 5.5 V).
    fixture();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
    ui.goto_page(uint8_t(Page::Tools), t + 100);
    CHECK_EQ(ui.push_menu(Menu::LoopProbe, t + 100), true);
    render(ui, t, "loop_idle");
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    render(ui, t, "loop_confirm");
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    CHECK_EQ(ui.loop_probe_busy(), true);
    for (int i = 0; i < 6; i++) { t += 150; ui.tick(t); }
    dump("loop_running");
    for (int i = 0; i < 400 && ui.loop_probe_busy() && ui.loop_probe_page()->footer() != "CC step: VBUS may drop"; i++) { t += 25; ui.tick(t); }
    ui.tick(t); t += 100; ui.tick(t);
    dump("loop_cc");
    for (int i = 0; i < 2000 && ui.loop_probe_busy(); i++) { t += 25; ui.tick(t); }
    CHECK_EQ(ui.menu() == Menu::Share, true);
    render(ui, t, "loop_share");
    ui.on_button(Button::Back, ButtonEvent::Short, t); t += 100;
    render(ui, t, "loop_done");
    g_stub.lp_vbus_mv = 9000;
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    ui.on_button(Button::Ok, ButtonEvent::Short, t); t += 100;
    render(ui, t, "loop_refused");
    g_stub.lp_vbus_mv = 5050;

    // PD diagnosis: the Tools view of case 1 (Legion rear port on a 60 W hub), its rows focused one by one, the 70 W ramp,
    // a finished session (LAST) and the empty state; then Home row 1 with each of them.
    fixture();
    const pd::diag::Report kCase1 = diag_fixture::case1();
    const pd::diag::Report kRamp = diag_fixture::report("tests/fixtures/bbdiag/ramp_70w.raw");
    const pd::diag::Report kStress = diag_fixture::report("tests/fixtures/bbdiag/stress_soft_resets.raw");
    const pd::diag::Report kInline = diag_fixture::report("tests/fixtures/bbdiag/legion_inline.raw");
    g_stub.diag_report = kCase1;
    g_stub.diag_state = 1;
    ui.goto_page(uint8_t(Page::Tools), t + 100);
    ui.push_menu(Menu::Diagnosis, t + 100); render(ui, t, "diagnosis_case1");
    const unsigned n1 = pd::diag::screen::row_count(kCase1);
    for (unsigned i = 1; i < n1; i++) {
        ui.on_button(Button::Plus, ButtonEvent::Short, t); t += 100;
        char name[40];
        std::snprintf(name, sizeof name, "diagnosis_case1_r%u", i);
        render(ui, t, name);
    }
    g_stub.diag_report = kRamp;
    for (unsigned i = 0; i < n1; i++) { ui.on_button(Button::Minus, ButtonEvent::Short, t); t += 100; }
    render(ui, t, "diagnosis_ramp");
    g_stub.diag_report = kStress;
    render(ui, t, "diagnosis_stress");
    g_stub.diag_report = kCase1;
    g_stub.diag_state = 2;
    render(ui, t, "diagnosis_last");
    g_stub.diag_report = {};
    g_stub.diag_state = 0;
    render(ui, t, "diagnosis_empty");
    static const struct { const pd::diag::Report* r; uint8_t state; const char* name; } kHome[] = {
        {&kCase1, 1, "home_diag_case1"}, {&kRamp, 1, "home_diag_ramp"}, {&kStress, 1, "home_diag_stress"},
        {&kInline, 1, "home_diag_inline"}, {nullptr, 0, "home_diag_none"}};
    ui.goto_page(uint8_t(Page::Home), t + 100);
    for (const auto& h : kHome) {
        g_stub.diag_report = h.r ? *h.r : pd::diag::Report{};
        g_stub.diag_state = h.state;
        render(ui, t, h.name);
        char w[24];
        pd::diag::Sev sev;
        pd::diag::screen::word(g_stub.diag_report, w, sev);
        printf("%-18s Diag %s (sev %d)\n", h.name, w, int(sev));
    }
    // The Connection page's Source tab with an open case-1 session: the verdict is its bar line.
    g_stub.diag_report = kCase1;
    g_stub.diag_state = 1;
    ui.goto_page(kPdPage, t + 100);
    render(ui, t, "pd_source_diag_case1");
    g_stub.diag_report = {};
    g_stub.diag_state = 0;

    printf("mockups dumped\n");
    return 0;
}
