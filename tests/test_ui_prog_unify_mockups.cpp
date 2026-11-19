// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Design renders for docs/agents/prog-unify.md: two ways the Deep run's merged PPS / AVS checks could sit on the Results grid, painted
// with the kit (draw_result_row, draw_footer) over the real Results page in all four rotations. Not a regression test, and nothing
// here is linked into the firmware; a label the kit cuts still fails the run (test_report).
//   A "spec row per range": the PD spec section keeps Transitions and the observer, the PPS / AVS checks become one row per APDO
//     with three grouped cells (Level: accuracy, monotonic, track, PPS status; Timing: band, settle, PS_RDY; Shape: overshoot,
//     transient).
//   B "spec on the chip": each APDO's range row carries a spec cell beside its range chip (the worst group's word when it fails);
//     the spec section keeps only Transitions and the observer.
// Run: K2_DUMP=<dir> ./host-build/test_ui_prog_unify_mockups, then tools/panel2png.py <dir> <out>.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/pd/pd_source.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace k2;
using namespace k2::kit;
namespace ct = charger_test;

namespace {

void dump(const char* name) {
    if (const char* dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_%s.panel", dir, name);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

// A 5-20 V charger with two PPS ranges (illustrative, the shape of the 140 W bench bricks); test_ui_charger_test's fixture.
void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2; g_stub.caps.raw_header = 0x1181;
    const unsigned fixed[][2] = {{5000, 300}, {9000, 300}, {12000, 300}, {15000, 300}, {20000, 500}};
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed[i][0] / 50) << 10) | fixed[i][1]);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000u | (110u << 17) | (33u << 8) | 100);
    g_stub.caps.pdos[6] = pd::decode_pdo(0xc0000000u | (210u << 17) | (33u << 8) | 100);
    g_stub.caps.num_pdos = 7;
    g_stub.fc_answers = true;
    g_stub.vbus_ceiling_mv = 36000;
}

uint32_t g_t = 1000;
void settle(Ui& ui) {
    for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(g_t);
    CHECK_EQ(ui.painting(), false);
}
void press(Ui& ui, Button b) { ui.on_button(b, ButtonEvent::Short, g_t); g_t += 100; }

constexpr int16_t kGridY = 72, kGridEnd = 222;   // below the Results page's counters and header line, above the footer

// Rows top-down from kGridY; a row that would cross kGridEnd is left out, as the grid does.
void rows(const Surface& s, const ResultRowSpec* r, int n) {
    gfx_fill(s, 0, kGridY, 224, kGridEnd - kGridY, kBg);
    int16_t y = kGridY;
    for (int i = 0; i < n; i++) {
        const int16_t h = result_row_h(r[i]);
        if (y + h > kGridEnd) break;
        draw_result_row(s, y, h, r[i].label, r[i].cells, r[i].n, r[i].focus, r[i].section);
        y = static_cast<int16_t>(y + h);
    }
}

using V = k2::kit::Verdict;
ResultCell c(V v, std::string_view label, std::string_view current = {}, bool sweep = false) { return {v, label, current, sweep}; }
ResultRowSpec row(std::string_view label, std::initializer_list<ResultCell> cells, bool section = false, bool focus = false) {
    ResultRowSpec r;
    r.label = label;
    for (const ResultCell& x : cells) r.cells[r.n++] = x;
    r.section = section;
    r.focus = focus;
    return r;
}

// A: the spec section scrolled to its end: the last fixed checks, one row per APDO, the observer.
void paint_a(const Surface& s) {
    const ResultRowSpec r[] = {
        row("Fall", {c(V::Pass, "240ms")}),
        row("Oversh.", {c(V::Pass, "+0.12V")}),
        row("PPS 11V", {c(V::Pass, "Level"), c(V::Pass, "Timing"), c(V::Pass, "Shape")}, true),
        row("PPS 21V", {c(V::Pass, "Level"), c(V::Pass, "Timing"), c(V::Fail, "Shape")}, false, true),
        row("Src rst", {c(V::Pass, "none")}, true),
    };
    rows(s, r, int(sizeof r / sizeof r[0]));
    draw_footer(s, FooterTone::Error, "PPS 21V shape: +0.41V over");
}

// B: the units with each range row's spec cell, then the slimmer spec section.
void paint_b(const Surface& s) {
    const ResultRowSpec r[] = {
        row("PD", {c(V::Pass, "5V", "3A"), c(V::Pass, "9V", "3A"), c(V::Pass, "12V", "3A"), c(V::Pass, "15V", "3A"), c(V::Pass, "20V", "5A")}),
        row("PPS", {c(V::Pass, "3.3-11V", "5A", true), c(V::Pass, "spec")}, true),
        row("PPS", {c(V::Pass, "3.3-21V", "5A", true), c(V::Fail, "shape")}, false, true),
    };
    rows(s, r, int(sizeof r / sizeof r[0]));
    draw_footer(s, FooterTone::Error, "PPS 21V shape: +0.41V over");
}

// B, scrolled to the spec section: Transitions and the observer only.
void paint_b_spec(const Surface& s) {
    const ResultRowSpec r[] = {
        row("PS_RDY", {c(V::Pass, "182ms")}, true),
        row("Rise", {c(V::Pass, "169ms")}),
        row("Fall", {c(V::Pass, "240ms")}),
        row("Oversh.", {c(V::Pass, "+0.12V")}),
        row("Src rst", {c(V::Pass, "none")}, true, true),
    };
    rows(s, r, int(sizeof r / sizeof r[0]));
    draw_footer(s, FooterTone::Ok, "Source reset: pass");
}

template <class F> void overlay(uint8_t rot, const char* name, F&& body) {
    static uint16_t save[LCD_WIDTH * LCD_HEIGHT];
    std::memcpy(save, g_panel, sizeof save);
    const Layout lay = compute_layout(orientation_for(rot).edge, kSlotCenters, kSlotLens, kPanelThickness);
    { ContentViewport vp(lay.content); const Surface s{0, 0, 224, 240}; body(s); }
    char full[64];
    std::snprintf(full, sizeof full, "%s_rot%u", name, unsigned(rot));
    dump(full);
    std::memcpy(g_panel, save, sizeof save);
}

}  // namespace

int main() {
    fixture();
    Ui ui;
    ui.init(0);
    ui.set_rotation(0, 0);
    ui.tick(g_t);
    ui.goto_page(uint8_t(Page::ChargerTest), g_t);
    settle(ui);
    static const uint8_t kPdOnly[] = {uint8_t(ct::Unit::Pd), uint8_t(ct::Unit::PdLow)};
    ui.charger_test_set_units(kPdOnly);
    ui.charger_test_set_sweep(true);
    settle(ui);
    for (int i = 0; i < 3; ++i) { press(ui, Button::Ok); settle(ui); }   // Tests, Cable, Review: START
    press(ui, Button::Ok);                                               // confirm
    for (; g_t <= 900000 && ui.charger_test_busy(); g_t += 100) ui.tick(g_t);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(ui.charger_test_stage(), 6);   // Results
    for (uint8_t rot = 0; rot < 4; ++rot) {
        ui.set_rotation(rot, g_t); g_t += 100;
        ui.tick(g_t); g_t += 200;
        settle(ui);
        ui.render_for_capture(g_t);
        dump((std::string("results_rot") + char('0' + rot)).c_str());
        overlay(rot, "alt_a", paint_a);
        overlay(rot, "alt_b", paint_b);
        overlay(rot, "alt_b_spec", paint_b_spec);
    }
    return test_report("test_ui_prog_unify_mockups");
}
