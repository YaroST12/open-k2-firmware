// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Graphs page's scroll-diff draw (src/ui/ui_graphs.cpp): every incremental pass, sliced by the paint budget, must leave
// exactly the pixels of a full repaint (Ui::resync) in all four rotations, a flat line must cost nothing on a scroll, and the
// steady tick stays under the idle budget. Also the history it reads: the limit follows a renegotiation and a PPS ramp.
// K2_DUMP=<dir> writes the scenario renders (laptop with derate, PPS ramp, unplugged, idle at boot) per rotation.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "diag_fixture.h"
#include "k2/pd/pd_source.hpp"
#include <cmath>
#include <memory>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace k2;

namespace {

constexpr uint32_t kCol = 91;   // the 10 s tier's column period

// A new Ui each time: the trend and the energy meter live in it, so a reused one would carry the last scenario's history.
Ui &fresh_ui() {
    alignas(Ui) static unsigned char store[sizeof(Ui)];
    static Ui *live = nullptr;
    if (live) std::destroy_at(live);
    live = new (store) Ui;
    return *live;
}

void dump(const char *name) {
    if (const char *dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_mock_%s.panel", dir, name);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

void contract(uint8_t pos, uint16_t mv, uint16_t ma) {
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = pos; g_stub.contract.mv = mv; g_stub.contract.op_ma = g_stub.contract.max_ma = ma;
}

void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20010; g_stub.pm.current_ma = 3210; g_stub.pm.power_mw = 64232;
    g_stub.pm.flow = power::Flow::Forward;
    g_stub.caps.valid = true; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2; g_stub.caps.raw_header = 0x1181;
    g_stub.caps.num_pdos = 7;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},   {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 12000, 12000, 3000, 36000}, {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},
        {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},{pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000},
        {pd::PdoType::Pps, 0, 3300, 21000, 5000, 105000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
    g_stub.cc = 1;
    g_stub.pd_seen_attach = true;
    contract(5, 20000, 5000);
}

}  // namespace

static void set_pm(int32_t mv, int32_t ma) {
    g_stub.pm.vbus_mv = mv;
    g_stub.pm.current_ma = ma;
    g_stub.pm.power_mw = static_cast<int32_t>(static_cast<int64_t>(mv) * ma / 1000);
}

// One column of time per tick.
static void step(Ui &ui, uint32_t &t, int n = 1) {
    for (int i = 0; i < n; i++) { t += kCol; ui.tick(t); }
}
static void settle(Ui &ui, uint32_t t) { while (ui.painting()) ui.tick(t); }

// The incremental frame against a full repaint at the same instant: the number of differing panel pixels.
static unsigned diff_vs_full(Ui &ui, uint32_t t) {
    settle(ui, t);
    std::vector<uint16_t> before(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT);
    ui.resync(t);
    unsigned diff = 0;
    int x0 = 240, y0 = 240, x1 = -1, y1 = -1;
    for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        if (before[i] == g_panel[i]) continue;
        diff++;
        x0 = std::min(x0, i % LCD_WIDTH); x1 = std::max(x1, i % LCD_WIDTH);
        y0 = std::min(y0, i / LCD_WIDTH); y1 = std::max(y1, i / LCD_WIDTH);
    }
    if (diff) std::printf("  differing pixels in x %d..%d, y %d..%d\n", x0, x1, y0, y1);
    return diff;
}

// A laptop drawing ~64 W at 20 V that derates: 15 V 3 A after a renegotiation, then a slow wobble. Every pass on the way is
// a budget-sliced incremental one; at each checkpoint the screen must equal a full repaint, in every rotation.
static void test_diff_equals_full_repaint() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        fixture();
        Ui &ui = fresh_ui();
        ui.init(0);
        uint32_t t = 1000;
        ui.set_rotation(rot, t);
        ui.goto_page(static_cast<uint8_t>(Page::Graphs), t);
        settle(ui, t);
        for (int i = 0; i < 60; i++) { set_pm(20010 - (i % 7) * 30, 3200 + (i * 37) % 300); step(ui, t); }
        set_pm(20020, 3210); step(ui, t, 4);
        CHECK_EQ(diff_vs_full(ui, t), 0u);
        contract(4, 15000, 3000);                                 // the renegotiation: the limit steps down
        for (int i = 0; i < 50; i++) { set_pm(15010 + (i % 5) * 20, 2600 + static_cast<int>(300 * std::sin(i * 0.3))); step(ui, t); }
        set_pm(15020, 2700); step(ui, t, 4);
        CHECK_EQ(diff_vs_full(ui, t), 0u);
        ui.on_button(Button::Ok, ButtonEvent::Short, t);         // Zoom: 60 s, another tier and scale
        step(ui, t, 3);
        CHECK_EQ(diff_vs_full(ui, t), 0u);
        ui.on_button(Button::Ok, ButtonEvent::Short, t);         // 10 min
        ui.on_button(Button::Ok, ButtonEvent::Short, t);         // back to 10 s
        contract(7, 9000, 5000);                                  // a PPS ramp: the dotted limit follows the level
        for (int i = 0; i < 40; i++) {
            const uint16_t mv = static_cast<uint16_t>(5000 + i * 300);
            g_stub.contract.mv = mv;
            set_pm(mv + 20, 4000);
            step(ui, t);
        }
        step(ui, t, 3);
        CHECK_EQ(diff_vs_full(ui, t), 0u);
        set_pm(0, 0);                                             // unplugged: the stale screen greys the lines
        step(ui, t, 4);
        CHECK_EQ(diff_vs_full(ui, t), 0u);
    }
}

// Paint cost: a flat line scrolls for free (the screen-anchored dotted limit too); a moving signal pays only its changed
// column windows; the 4 Hz digits pay their changed characters. The steady tick stays under the idle budget.
static void test_paint_cost() {
    fixture();
    Ui &ui = fresh_ui();
    ui.init(0);
    uint32_t t = 1000;
    ui.set_rotation(0, t);
    ui.goto_page(static_cast<uint8_t>(Page::Graphs), t);
    settle(ui, t);
    set_pm(20010, 3210);
    step(ui, t, 130);   // the window full of one flat level
    settle(ui, t);
    g_lcd_pixels = 0;
    unsigned worst = 0, total = 0, wva = 0;
    for (int i = 0; i < 22; i++) {   // ~2 s at one flat level
        const unsigned before = g_lcd_pixels;
        std::vector<uint16_t> snap(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT);
        step(ui, t);
        settle(ui, t);
        worst = std::max(worst, g_lcd_pixels - before);
        total += g_lcd_pixels - before;
        for (int k = 0; k < 3; k++)   // the W, V and A graphs (and the W limit) stay untouched
            for (int y = graphs::graph_y0(k); y <= graphs::graph_y1(k); y++)
                for (int x = 0; x < 224; x++) wva += snap[y * LCD_WIDTH + x] != g_panel[y * LCD_WIDTH + x];
    }
    // What moves: the elapsed time and the Wh value (one character each), and every 10th column the Wh ramp's newest point.
    std::printf("graphs flat: worst %u px per tick, %u px over 22 scrolls (elapsed, Wh digits, the Wh ramp)\n", worst, total);
    CHECK_EQ(wva, 0u);
    CHECK_EQ(worst <= 3u * 8u * 16u, true);
    // One digit of VBUS moves: the changed characters only.
    g_lcd_pixels = 0;
    set_pm(20020, 3210);
    t += 300; ui.tick(t); settle(ui, t);
    std::printf("graphs digit tick (VBUS 20.01 -> 20.02, W 64.2 -> 64.3): %u px\n", g_lcd_pixels);
    CHECK_EQ(g_lcd_pixels > 0u, true);
    CHECK_EQ(g_lcd_pixels <= 4u * 8u * 16u + 2u * 8u * 16u, true);
    // A moving signal: a sawtooth on the current (and so the power) scrolls every column of two graphs.
    unsigned mv_total = 0, mv_worst = 0, scrolls = 0;
    for (int i = 0; i < 44; i++) {
        set_pm(20020, 2000 + (i % 11) * 120);
        const unsigned before = g_lcd_pixels;
        step(ui, t);
        settle(ui, t);
        const unsigned px = g_lcd_pixels - before;
        if (i >= 22) { mv_total += px; mv_worst = std::max(mv_worst, px); scrolls++; }
    }
    std::printf("graphs moving: %u px per scroll on average, worst %u (full graph area 4 x 220 x 37 = 32560)\n",
                mv_total / scrolls, mv_worst);
    CHECK_EQ(mv_worst < 6000u, true);
    CHECK_EQ(diff_vs_full(ui, t), 0u);
}

// The scenarios the brief asks to see, rendered per rotation (K2_DUMP): a laptop that derates, a PPS ramp, unplugged (the
// stale screen) and the idle K2 at boot.
static void render_scenarios() {
    static const char *const kRot[4] = {"r0", "r1", "r2", "r3"};
    const pd::diag::Report healthy = diag_fixture::report("tests/fixtures/bbdiag/legion_inline.raw");   // "Link healthy"
    for (uint8_t rot = 0; rot < 4; rot++) {
        char name[48];
        {   // idle at boot: no source, nothing recorded
            fixture();
            g_stub.pm = {};
            g_stub.contract = {};
            g_stub.caps = {};
            g_stub.pd_seen_attach = false;
            Ui &ui = fresh_ui();
            ui.init(0);
            uint32_t t = 1000;
            ui.set_rotation(rot, t);
            ui.goto_page(static_cast<uint8_t>(Page::Graphs), t);
            step(ui, t, 5);
            settle(ui, t);
            std::snprintf(name, sizeof name, "graphs_idle_%s", kRot[rot]);
            dump(name);
        }
        fixture();
        g_stub.diag_report = healthy;
        g_stub.diag_state = 1;
        Ui &ui = fresh_ui();
        ui.init(0);
        uint32_t t = 1000;
        ui.set_rotation(rot, t);
        ui.goto_page(static_cast<uint8_t>(Page::Graphs), t);
        // Laptop: 20 V 5 A contract, ~60 W with a ripple, then the derate to 15 V 3 A.
        for (int i = 0; i < 70; i++) { set_pm(20010 - (i % 4) * 20, 3000 + static_cast<int>(250 * std::sin(i * 0.4))); step(ui, t); }
        contract(4, 15000, 3000);
        for (int i = 0; i < 40; i++) { set_pm(15020 - (i % 3) * 15, 2600 + (i % 9) * 30); step(ui, t); }
        settle(ui, t);
        std::snprintf(name, sizeof name, "graphs_laptop_%s", kRot[rot]);
        dump(name);
        // PPS ramp: 5 V -> 20 V at 4 A on the 3.3-21 V APDO.
        contract(7, 5000, 5000);
        for (int i = 0; i < 110; i++) {
            const uint16_t mv = static_cast<uint16_t>(5000 + (i < 100 ? i : 100) * 150);
            g_stub.contract.mv = mv;
            set_pm(mv + 15, 4000 - (i % 5) * 10);
            step(ui, t);
        }
        settle(ui, t);
        std::snprintf(name, sizeof name, "graphs_pps_%s", kRot[rot]);
        dump(name);
        // Unplugged: VBUS gone, the last reading stays, grey.
        set_pm(0, 0);
        step(ui, t, 6);
        settle(ui, t);
        std::snprintf(name, sizeof name, "graphs_unplugged_%s", kRot[rot]);
        dump(name);
        g_stub.diag_report = {};
        g_stub.diag_state = 0;
    }
}

int main() {
    test_diff_equals_full_repaint();
    test_paint_cost();
    render_scenarios();
    return test_report("test_ui_graphs");
}
