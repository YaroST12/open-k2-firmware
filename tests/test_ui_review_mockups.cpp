// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UI review scratch harness (docs/agents/UI_REVIEW_2026-11-06.md). Not a regression test: it renders
//   - "before": real Ui pages at the bottom-strip 240x220 canvas that tests/test_ui_capture.cpp and
//     tests/test_ui_mockup.cpp do not reach (a finished charger run: results, steps, detail, the
//     Ask dialog, the running list), and
//   - "after": the proposed screens, painted with the same gfx primitives, fonts and palette on top of
//     the real chrome (title bar, button strip, footer) so a before/after pair differs only where the
//     proposal differs. Nothing here is linked into the firmware.
// Run: K2_DUMP=<dir> make host-build/test_ui_review_mockups && K2_DUMP=<dir> ./host-build/test_ui_review_mockups
// Output: <dir>/k2_<name>.panel (240x240 RGB565 LE) for tools/panel2png.py.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/emk_spoof.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace k2;
namespace ct = charger_test;

static void dump(const char* name) {
    if (const char* dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_%s.panel", dir, name);
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

static void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20040; g_stub.pm.current_ma = 1834; g_stub.pm.power_mw = 36753;
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
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 5;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
}

static uint32_t g_t = 1000;
static void settle(Ui& ui) { ui.tick(g_t); g_t += 200; ui.tick(g_t); g_t += 200; }
static void shot(Ui& ui, const char* name) { settle(ui); dump(name); }
static void press(Ui& ui, Button b) { ui.on_button(b, ButtonEvent::Short, g_t); g_t += 100; }

// ---------------------------------------------------------------- part B: the proposal, painted
// The "after" screens start from the real "before" frame (real title bar, strip, footer), repaint what
// the proposal changes with the shipped primitives (gfx_*, fill_round, draw_pill, draw_mark, fonts,
// palette) inside a ContentViewport, dump the frame, and put the "before" frame back so the Ui's
// incremental-repaint state stays truthful. Coordinates are the pages' logical 224 x 240 content.
static Layout g_lay;
static uint16_t g_save[LCD_WIDTH * LCD_HEIGHT];

template <class F> static void overlay(const char* name, F&& body) {
    std::memcpy(g_save, g_panel, sizeof g_save);
    { ContentViewport vp(g_lay.content); const Surface s{0, 0, 224, 240}; body(s); }
    dump(name);
    std::memcpy(g_panel, g_save, sizeof g_save);
}

static int tw(const Font& f, std::string_view t) { return static_cast<int>(t.size()) * f.adv; }
static void txt(const Surface& s, const Font& f, int x, int y, std::string_view t, uint16_t fg, uint16_t bg) {
    gfx_text(s, f, static_cast<int16_t>(x), static_cast<int16_t>(y), t, fg, bg);
}
static void txt_r(const Surface& s, const Font& f, int xr, int y, std::string_view t, uint16_t fg, uint16_t bg) {
    txt(s, f, xr - tw(f, t), y, t, fg, bg);
}
static void fill(const Surface& s, int x, int y, int w, int h, uint16_t c) {
    gfx_fill(s, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(w), static_cast<int16_t>(h), c);
}
// Clear the page body (below the 21 px title bar, above the footer band) before repainting it.
static void clear_body(const Surface& s, int y0 = 22, int y1 = 222) { fill(s, 0, y0, 224, y1 - y0, kBg); }

// ---- A1: Charger results grid ----------------------------------------------------------------
struct VCell { const char* v; char st; };   // st: P pass, F fail, S skip
struct VRow { const char* lab; int n; VCell c[5]; };
static const VRow kGrid[] = {   // illustrative data, not a real run
    {"PD",      5, {{"5", 'P'}, {"9", 'P'}, {"12", 'P'}, {"15", 'P'}, {"20", 'P'}}},
    {"PPS",     4, {{"5", 'P'}, {"9", 'P'}, {"15", 'P'}, {"20", 'F'}, {}}},
    {"SPR AVS", 0, {}},
    {"EPR AVS", 2, {{"28", 'P'}, {"36", 'S'}, {}, {}, {}}},
    {"QC2",     3, {{"5", 'P'}, {"9", 'F'}, {"12", 'P'}, {}, {}}},
    {"QC3",     3, {{"5", 'P'}, {"9", 'P'}, {"12", 'S'}, {}, {}}},
    {"AFC",     3, {{"5", 'P'}, {"9", 'P'}, {"12", 'F'}, {}, {}}},
    {"FCP",     3, {{"5", 'P'}, {"9", 'F'}, {"12", 'S'}, {}, {}}},
};
constexpr int kGridRows = sizeof kGrid / sizeof kGrid[0];

static void grid_cell(const Surface& s, int x, int y, const VCell& c) {
    const bool p = c.st == 'P', f = c.st == 'F';
    const uint16_t fg = p ? kEnergy : f ? kError : kDim;
    const uint16_t tint = p ? kOkT : f ? kErrorT : kDimT;
    fill_round(s, static_cast<int16_t>(x), static_cast<int16_t>(y), 29, 20, 3, tint);
    if (p || f) draw_mark(s, static_cast<int16_t>(x + 3), static_cast<int16_t>(y + 6), 8, p, fg, tint);
    else fill(s, x + 4, y + 9, 7, 2, fg);                       // skip: a dash, neither tick nor cross
    txt_r(s, kLabelFont, x + 26, y + 4, c.v, fg, tint);
}

static void paint_results_after(const Surface& s, int sel) {
    clear_body(s);
    int pass = 0, fail = 0, skip = 0;
    for (const VRow& r : kGrid)
        for (int i = 0; i < r.n; ++i) (r.c[i].st == 'P' ? pass : r.c[i].st == 'F' ? fail : skip)++;
    // Verdict line: three counters, number big, word small; the words carry the meaning, colour repeats it.
    const int hy = 24;
    const struct { int n; const char* w; uint16_t c; } sum[3] = {{pass, "pass", kEnergy}, {fail, "fail", kError}, {skip, "skip", kDim}};
    for (int i = 0; i < 3; ++i) {
        char num[8]; std::snprintf(num, sizeof num, "%d", sum[i].n);
        const int x = 6 + i * 74;
        txt(s, kMid, x, hy, num, sum[i].c, kBg);
        txt(s, kLabelFont, x + tw(kMid, num) + 4, top_for_baseline(kLabelFont, static_cast<int16_t>(hy + kMid.ascent)),
            sum[i].w, kDim, kBg);
    }
    // Legend (words + the shapes the cells use) and the hairline.
    const int ly = hy + kMid.height + 2;
    draw_mark(s, 6, static_cast<int16_t>(ly + 2), 8, true, kEnergy, kBg);  txt(s, kLabelFont, 18, ly, "pass", kDim, kBg);
    draw_mark(s, 56, static_cast<int16_t>(ly + 2), 8, false, kError, kBg); txt(s, kLabelFont, 68, ly, "fail", kDim, kBg);
    fill(s, 106, ly + 6, 7, 2, kDim);                                       txt(s, kLabelFont, 118, ly, "skip", kDim, kBg);
    txt_r(s, kLabelFont, 218, ly, "volts", kFaint, kBg);
    fill(s, 0, ly + 14, 224, 1, kRule);
    const int y0 = ly + 16;
    const int rows = (222 - y0) / 24;
    const int first = sel >= rows ? sel - rows + 1 : 0;
    for (int k = 0; k < rows && first + k < kGridRows; ++k) {
        const VRow& r = kGrid[first + k];
        const int y = y0 + k * 24;
        const bool f = first + k == sel;
        if (f) { fill(s, 0, y, 220, 24, kFocusT); fill(s, 0, y, 3, 24, kFocus); }
        const uint16_t bg = f ? kFocusT : kBg;
        txt(s, kSmall, 8, y + 4, r.lab, kText, bg);
        if (r.n == 0) { txt(s, kSmall, 70, y + 4, "not offered", kDim, bg); continue; }
        for (int i = 0; i < r.n; ++i) grid_cell(s, 66 + i * 30, y + 2, r.c[i]);
    }
    // Scroll rail (3 px, right edge), like the kit RowList.
    const int track = rows * 24, thumb = track * rows / kGridRows;
    fill(s, 221, y0, 3, track, kRule);
    fill(s, 221, y0 + (track - thumb) * first / (kGridRows - rows), 3, thumb, kDim);
    // Footer explains the focused cell in words (the old Detail screen's first line).
    draw_footer(s, FooterTone::Error, "QC2 9V: no reply in 2.0s");
}

// ---- A2: one compact wizard header, Review ---------------------------------------------------
static void wizard_header(const Surface& s, int step, int of, const char* title) {
    char n[8]; std::snprintf(n, sizeof n, "%d/%d", step, of);
    txt(s, kLabelFont, 6, 28, n, kDim, kBg);
    txt(s, kSmall, 34, 26, title, kText, kBg);
    for (int i = 0; i < of; ++i)   // progress: past dim, current bright, ahead faint (never the cyan of focus)
        fill(s, 224 - 6 - (of - i) * 17 + 3, 31, 14, 4, i + 1 < step ? kDim : i + 1 == step ? kText : kRule);
    fill(s, 0, 44, 224, 1, kRule);
}

// Algorithms (step 1/6) in the same header: the heading card goes, the list gets RowList-style 24 px
// rows with the ON/OFF state as a word in a pill (green = state, cyan = focus, never swapped).
static void paint_algorithms_after(const Surface& s) {
    clear_body(s);
    wizard_header(s, 1, 6, "Algorithms");
    const struct { const char* n; const char* c; } al[6] = {
        {"USB PD", "2/2"}, {"Qualcomm QC", "3/3"}, {"Samsung AFC", "1/1"},
        {"Huawei ACCP", "2/2"}, {"OPPO / OnePlus", "3/3"}, {"UFCS / VIVO", "2/2"}};
    fill(s, 0, 50, 220, 24, kFocusT); fill(s, 0, 50, 3, 24, kFocus);
    txt(s, kSmall, 10, 54, "Next: Cable / Spoof", kAmp, kFocusT);
    for (int i = 0; i < 6; ++i) {
        const int y = 74 + i * 24;
        txt(s, kSmall, 10, y + 4, al[i].n, kText, kBg);
        const int w = pill_width(2);
        draw_pill(s, static_cast<int16_t>(218 - w), static_cast<int16_t>(y + 5), "ON", kEnergy, kOkT, false);
        txt_r(s, kLabelFont, 218 - w - 6, y + 6, al[i].c, kDim, kBg);
    }
    fill(s, 221, 50, 3, 168, kRule); fill(s, 221, 50, 3, 120, kDim);
    draw_footer(s, FooterTone::Info, "13/13 ON; the rest SKIP");
}

static void paint_review_after(const Surface& s) {
    clear_body(s);
    wizard_header(s, 5, 6, "Review");
    const struct { const char* k; const char* v; } kv[5] = {
        {"Tests", "33 run, 5 skip"}, {"Peak", "12.0 V, ~111 s"}, {"Cap", "36V wire 20V"},
        {"Header", "PD3.x / RDMO -"}, {"Spoof", "OFF"}};
    for (int i = 0; i < 5; ++i) {
        txt(s, kSmall, 6, 50 + i * 22, kv[i].k, kDim, kBg);
        txt(s, kSmall, 84, 50 + i * 22, kv[i].v, kText, kBg);
    }
    // The one action of the page: a full-width RowList-style focus row, with the confirm rule as a pill.
    fill(s, 0, 166, 220, 28, kFocusT); fill(s, 0, 166, 3, 28, kFocus);
    txt(s, kSmall, 10, 172, "START tests", kAmp, kFocusT);
    draw_pill(s, static_cast<int16_t>(218 - pill_width(5)), 173, "OK x2", kCaution, kCautionT, false);
    txt(s, kLabelFont, 6, 200, "Voltage only; no load test", kDim, kBg);
    draw_footer(s, FooterTone::Caution, "No load! OK twice in 5s");
}

// ---- A3: Tools, the readiness tag on every row it applies to -------------------------------------
// The shipped row_fn sets the tag on the selected row only; the icon colour carries readiness on the
// others (colour alone). The proposal drops the `if (selected)`: a tag is drawn on every row that is
// not Ready, tinted (not solid) when unselected. Ready rows stay quiet.
static void paint_tools_after(const Surface& s) {
    const struct { int row; const char* tag; uint16_t c, t; } tags[] = {
        {1, "INLINE", kCaution, kCautionT}, {2, "LOAD", kCaution, kCautionT},
        {3, "LOAD", kError, kErrorT},       {6, "LOAD", kError, kErrorT}};
    for (const auto& g : tags) {
        const int y = 24 + g.row * 24, w = pill_width(static_cast<int>(std::strlen(g.tag)));
        draw_pill(s, static_cast<int16_t>(218 - w), static_cast<int16_t>(y + 5), g.tag, g.c, g.t, false);
    }
}

// A4 (the Home trend strips and idle info row proposal) shipped with Home v7: no overlay remains.

// ---------------------------------------------------------------- part A: the real UI, before
static void before_charger(Ui& ui, bool pd_only) {
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;   // no load: START is allowed
    ui.goto_page(uint8_t(Page::ChargerTest), g_t);
    if (pd_only) {
        static const uint8_t kPdOnly[] = {uint8_t(ct::Unit::Pd), uint8_t(ct::Unit::PdLow)};
        ui.charger_test_set_units(kPdOnly);
    }
    shot(ui, "b_ct_algorithms");
    overlay("a_ct_algorithms", [](const Surface& s) { paint_algorithms_after(s); });
    for (size_t i = 0; i < ct::kMaxSteps; ++i) press(ui, Button::Minus);
    press(ui, Button::Ok); shot(ui, "b_ct_cable");
    press(ui, Button::Ok); shot(ui, "b_ct_review");
    overlay("a_ct_review", [](const Surface& s) { paint_review_after(s); });
    press(ui, Button::Ok); shot(ui, "b_ct_confirm");         // START: the confirm window (no identity dialog any more)
    press(ui, Button::Ok);
    // mid-run: tick until the first step is active, then grab the running list
    for (int i = 0; i < 40 && ui.charger_test_busy(); ++i) { g_t += 100; ui.tick(g_t); }
    dump("b_ct_running");
    for (; g_t <= 900000 && ui.charger_test_busy(); g_t += 100) ui.tick(g_t);
    // The incremental repaint of this screen leaves stale chip fragments in the host render (side issue);
    // a forced full repaint of the same state is the faithful "before".
    ui.set_rotation(2, g_t); g_t += 100; ui.set_rotation(3, g_t); g_t += 100;
    shot(ui, "b_ct_results");
    overlay("a_ct_results", [](const Surface& s) { paint_results_after(s, 4); });
    for (int i = 0; i < 3; ++i) { press(ui, Button::Plus); }
    shot(ui, "b_ct_results_sel");
    ui.set_rotation(2, g_t); g_t += 100; ui.set_rotation(3, g_t); g_t += 100;   // forced full repaint of the same state
    shot(ui, "b_ct_results_forced");
    press(ui, Button::Ok); shot(ui, "b_ct_steps");
    press(ui, Button::Ok); shot(ui, "b_ct_detail");
}

static void other_pages(Ui& ui) {
    // Home with no source plugged in: the idle walk-by state.
    g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
    g_stub.caps.valid = false; g_stub.contract.valid = false; g_stub.contract.ready = false; g_stub.em.valid = false;
    ui.goto_page(uint8_t(Page::Home), g_t);
    shot(ui, "b_home_idle");
    // Home with no power meter (error state): the heroes keep "--", the protocol row is red.
    fixture();
    g_stub.pm_online = false;
    ui.goto_page(uint8_t(Page::Tools), g_t); settle(ui);
    ui.goto_page(uint8_t(Page::Home), g_t);
    shot(ui, "b_home_nometer");
    fixture();
    for (int i = 0; i < 12; ++i) settle(ui);   // a flat 20 V / 1.83 A trace
    shot(ui, "b_home_flat");
    // The flat-range change (one label instead of "20.0 / 20.0") is described in the doc only: the
    // strip label boxes carry a tint here that a host overlay cannot repaint faithfully.
    // Tools with a load attached (inline); the first row is selected and keeps its solid pill.
    fixture();
    ui.goto_page(uint8_t(Page::Tools), g_t);
    shot(ui, "b_tools");
    overlay("a_tools", [](const Surface& s) { paint_tools_after(s); });
}

int main() {
    fixture();
    g_lay = compute_layout(PanelEdge::Bottom, kSlotCenters, kSlotLens, kPanelThickness);
    {
        Ui ui; ui.init(0); ui.configure_rotation(false, 3); g_t = 1000;
        other_pages(ui);
    }
    fixture();
    {
        Ui ui; ui.init(0); ui.configure_rotation(false, 3); g_t = 1000;
        before_charger(ui, false);
    }
    std::printf("review mockups dumped\n");
    return 0;
}
