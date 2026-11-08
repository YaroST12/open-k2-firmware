// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test wizard pages rendered through the real page code. K2_DUMP=dir writes every page as
// k2_wizard_<name>.panel (tools/panel2png.py turns them into PNGs; build/size/wizard/ holds the before/after set).
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/emk_spoof.hpp"
#include <cstdio>
#include <cstdlib>

using namespace k2;
namespace ct = charger_test;
static uint32_t g_t = 1200;
static void press(Ui& ui, Button b) { ui.on_button(b, ButtonEvent::Short, g_t); }
static void move(Ui& ui, unsigned n) { for (unsigned i = 0; i < n; ++i) press(ui, Button::Plus); }
static void top(Ui& ui) { for (unsigned i = 0; i < 40; ++i) press(ui, Button::Minus); }
static void settle(Ui& ui) { for (int i = 0; i < 3; ++i) ui.tick(g_t += 50); }
static void dump(const char* name) {
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char p[256];
    std::snprintf(p, sizeof p, "%s/k2_wizard_%s.panel", dir, name);
    if (FILE* f = std::fopen(p, "wb")) { std::fwrite(g_panel, 2, 240 * 240, f); std::fclose(f); }
}
static void fixture() {
    g_stub = {}; pd::set_fake(0);
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.raw_header = 0x1181;
    const uint32_t v[5] = {0xFF00A041, 0x1800203C, 123, 1u << 16, 0x00182052};
    emlib::Entry e; e.seq = 1; e.version = 1; e.rec = emrec::make(v, 2);
    std::snprintf(e.name.data(), e.name.size(), "Host identity 1");
    g_stub.lib = {e};
}
static void test_pages() {
    fixture(); Ui ui; ui.init(0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    settle(ui); dump("1_tests");
    top(ui); move(ui, 4); press(ui, Button::Ok); settle(ui);   // "Custom..." opens the per-family list (not a numbered step)
    CHECK_EQ(ui.charger_test_stage(), 14); dump("1b_custom");
    move(ui, 1); ui.on_button(Button::Ok, ButtonEvent::Long, g_t); settle(ui);   // a long OK on a family opens its entries
    CHECK_EQ(ui.charger_test_stage(), 1); dump("2_group");
    press(ui, Button::Back); settle(ui);
    CHECK_EQ(ui.charger_test_stage(), 14);
    top(ui); move(ui, 6); press(ui, Button::Ok); settle(ui);   // "Done" closes the list and goes on to the cable
    CHECK_EQ(ui.charger_test_stage(), 2); dump("3_cable");
    top(ui); move(ui, 2); dump("4_identity");
    press(ui, Button::Ok); settle(ui);                // a pick selects the cable and advances: there is no Next row
    CHECK_EQ(ui.charger_test_stage(), 4); dump("5_review");
    press(ui, Button::Ok); settle(ui);                // START: the confirm window opens on Review (no dialog)
    CHECK_EQ(ui.charger_test_stage(), 4); dump("5b_confirm");
    press(ui, Button::Ok);                            // the confirming press
    for (int i = 0; i < 4000 && ui.charger_test_busy(); ++i) { ui.tick(g_t += 100); if (i == 20) dump("6_running"); }
    settle(ui);
    CHECK_EQ(ui.charger_test_stage(), 6);
    dump("7_results");
}
int main() { test_pages(); return test_report("test_ui_wizard"); }
