// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Idle power, the UI side (k2/core/idle.hpp drives it): with painting off Ui::tick() keeps the model running and draws nothing,
// switching it back on repaints the whole screen, and the alarm level the idle logic holds the screen for follows the live
// meter against the contract (Home's OVER rule) and the footer tone drawn last.
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_nav.h"
#include "ui_stubs.h"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_widgets.hpp"

using namespace k2;

namespace {

void live_fixture() {   // a 20 V 5 A contract on CC1, 20 V 1.5 A flowing
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 4; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;
    const pd::Pdo mix[4] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000}};
    for (int i = 0; i < 4; i++) g_stub.caps.pdos[i] = mix[i];
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 4;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = 5000; g_stub.contract.max_ma = 5000;
}

void settle(Ui& ui, uint32_t& now) {
    for (int i = 0; i < 20; i++) { now += 20; ui.tick(now); }
    while (ui.painting()) { now += 20; ui.tick(now); }
}

void test_off_paints_nothing_and_wake_repaints() {
    live_fixture();
    Ui ui;
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    settle(ui, now);
    CHECK_COND(ui.paint_enabled());

    ui.set_paint_enabled(false, now);
    CHECK_COND(!ui.paint_enabled());
    const unsigned px0 = g_lcd_pixels, win0 = g_lcd_windows;
    g_stub.pm.current_ma = 1700;   // the model keeps moving
    g_stub.pm.power_mw = 34000;
    for (int i = 0; i < 50; i++) { now += 20; ui.tick(now); CHECK_COND(!ui.paint_more(now + 1)); }
    CHECK_EQ(g_lcd_pixels, px0);
    CHECK_EQ(g_lcd_windows, win0);
    CHECK_COND(!ui.painting());

    // a page switch while off (a remote press) paints nothing either; the wake paints whatever page is up
    ui.goto_page(1, now);
    CHECK_EQ(g_lcd_pixels, px0);

    ui.set_paint_enabled(true, now);
    CHECK_COND(ui.paint_enabled());
    CHECK_COND(ui.painting());   // the repaint is queued from scratch
    settle(ui, now);
    CHECK_COND(g_lcd_pixels > px0 + 20000);   // a whole screen, not a field
    ui.set_paint_enabled(true, now);   // already on: no second repaint
    CHECK_COND(!ui.painting());
}

void test_alert_follows_the_meter() {
    live_fixture();
    Ui ui;
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    settle(ui, now);
    CHECK_COND(!ui.alert_active());

    g_stub.pm.current_ma = 9000;      // far over the 5 A contract
    g_stub.pm.power_mw = 180000;
    for (int i = 0; i < 100; i++) { now += 20; ui.tick(now); }
    CHECK_COND(ui.alert_active());
    ui.set_paint_enabled(false, now);   // still known while the screen is off
    for (int i = 0; i < 20; i++) { now += 20; ui.tick(now); }
    CHECK_COND(ui.alert_active());

    g_stub.pm.current_ma = 1500;
    g_stub.pm.power_mw = 30000;
    for (int i = 0; i < 200; i++) { now += 20; ui.tick(now); }
    CHECK_COND(!ui.alert_active());
}

void test_error_footer_latch() {
    footer_error_clear();
    CHECK_COND(!footer_error_shown());
    const Surface s{0, 0, 240, 224};
    draw_footer(s, FooterTone::Error, "Unsafe VBUS: unplug source");
    CHECK_COND(footer_error_shown());
    draw_footer(s, FooterTone::Info, "All subsystems up");   // the footer showing now is not an error
    CHECK_COND(!footer_error_shown());
    draw_footer(s, FooterTone::Error, "x");
    footer_error_clear();   // a page switch
    CHECK_COND(!footer_error_shown());
}

}  // namespace

int main() {
    test_off_paints_nothing_and_wake_repaints();
    test_alert_follows_the_meter();
    test_error_footer_latch();
    return test_report("test_ui_paint_gate");
}
