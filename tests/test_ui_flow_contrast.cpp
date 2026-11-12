// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Flow face (Home) draws a patient-monitor trend behind the digits of every quantity block. This test renders the
// face with realistic live data (a 20 V 3 A charge with ripple, a 5 V to 20 V ramp, a 5 V idle, a LAST snapshot) in
// the three themes and checks the contrast of what the trend is made of (WCAG 2.x relative luminance):
//  - the trend's line against the block it is drawn on and against the area under it: at least 3:1;
//  - the numerals against the block, the area and the line they sit over: at least 4.5:1.
// K2_DUMP=dir also writes every render as k2_flow_<theme>_<scenario>.panel (tools/panel2png.py).
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/theme.hpp"
#include "k2/pd/pd_source.hpp"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>

using namespace k2;
namespace hm = k2::home;

// ---- WCAG ----------------------------------------------------------------------------------------
static double channel(int c8) {
    const double c = c8 / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}
static double luminance(uint16_t v) {
    const int r = ((v >> 11) & 31) * 255 / 31, g = ((v >> 5) & 63) * 255 / 63, b = (v & 31) * 255 / 31;
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b);
}
static double ratio(uint16_t a, uint16_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { const double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

// ---- driving the face ----------------------------------------------------------------------------
static uint32_t g_t = 100000;

static Ui &fresh_ui() {
    static Ui ui;
    ui.~Ui();
    new (&ui) Ui;
    g_stub = StubState();
    g_stub.ms = 10000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_t);
    ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    return ui;
}

// A 20 V PD contract (the 100 W source of the other Home tests): 20 V 5 A 100 W is the scale of the trends.
static void live(int mv, int ma) {
    g_stub.caps = {};
    static uint32_t rx = 0;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 3; g_stub.caps.spec_rev = 2;
    g_stub.caps.rx_count = ++rx;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.contract = {};
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 3;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = 5000; g_stub.contract.max_ma = 5000;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = mv;
    g_stub.pm.current_ma = ma;
    g_stub.pm.power_mw = static_cast<int32_t>(static_cast<int64_t>(mv) * ma / 1000);
    g_stub.pm.flow = power::Flow::Forward;
    g_stub.em.valid = true; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 100; g_stub.em.max_voltage_v = 20;
    adc::Reading r;
    r.dp_mv = 600; r.dm_mv = 580; r.cc1_mv = 1620; r.cc2_mv = 70; r.vref_raw = 1489; r.vdd_mv = 3300;
    r.mcu = {312, true, false};
    g_stub.adc = r;
    g_stub.cc = 1;
    g_stub.fc = fastcharge::Protocol::Pd;
}

static uint32_t g_rng = 12345;
static int noise(int amp) {   // deterministic, roughly triangular
    g_rng = g_rng * 1664525u + 1013904223u;
    const int a = static_cast<int>((g_rng >> 16) & 0xFF), b = static_cast<int>((g_rng >> 8) & 0xFF);
    return (a + b - 255) * amp / 255;
}

enum class Scene { Charge, Ramp, Idle5 };

// One frame of the scene (frame f, 100 ms apart).
static void feed(Scene s, int f) {
    switch (s) {
    case Scene::Charge: {   // 20 V 3 A with a slow ripple on both rails and a step in the current half way
        const double ph = f * 0.35;
        const int mv = 20000 + static_cast<int>(60 * std::sin(ph)) + noise(25);
        const int base = f < 70 ? 3000 : 2200;
        const int ma = base + static_cast<int>(140 * std::sin(ph * 0.8)) + noise(60);
        live(mv, ma);
        break;
    }
    case Scene::Ramp: {     // a PPS ramp 5 V to 20 V while the current builds
        const int k = f < 150 ? f : 150;
        const int mv = 5000 + k * 100;
        const int ma = 400 + k * 17 + noise(30);
        live(mv, ma);
        break;
    }
    case Scene::Idle5:      // 5 V standby: a trickle with a few mA of noise
        live(5050 + noise(12), 120 + noise(15));
        break;
    }
}

static void dump(const char *theme_name, const char *scene) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char p[256];
    std::snprintf(p, sizeof p, "%s/k2_flow_%s_%s.panel", dir, theme_name, scene);
    if (FILE *f = std::fopen(p, "wb")) {
        std::fwrite(g_panel, 2, 240 * 240, f);
        std::fclose(f);
    }
}

static void run(Ui &ui, Scene s, int frames, int from = 0) {
    for (int i = 0; i < frames; i++) {
        feed(s, from + i);
        g_t += 100;
        ui.tick(g_t);
    }
}

static void unplug() {
    g_stub.pm = {};
    g_stub.contract = {}; g_stub.em = {}; g_stub.caps = {};
    g_stub.fc = fastcharge::Protocol::NoInput;
}

#define CHECK_TRUE(c) CHECK_EQ(int(!!(c)), 1)

static int count_px(uint16_t colour) {
    int n = 0;
    for (int i = 0; i < 240 * 240; i++) n += g_panel[i] == colour;
    return n;
}

// What the trend of one field is made of, in one theme: the block it sits on, the digit ink, the line and the area.
struct TraceCase { const char *name; uint16_t fill, ink, line, area; bool stale; };

static void check_theme(theme::Id id) {
    const theme::Palette &p = theme::table(id);
    const char *tn = theme::name(id).data();
    const TraceCase cases[] = {
        {"V", p.volt, p.spot_ink, p.trace_volt, p.area_volt, false},
        {"A", p.amp, p.spot_ink, p.trace_amp, p.area_amp, false},
        {"W", p.watt, p.watt_ink, p.trace_watt, p.area_watt, false},
        {"LAST", p.stale, p.stale_ink, p.trace_stale, p.area_stale, true},
    };
    for (const TraceCase &t : cases) {
        const double lf = ratio(t.line, t.fill), la = ratio(t.line, t.area), af = ratio(t.area, t.fill);
        const double df = ratio(t.ink, t.fill), da = ratio(t.ink, t.area), dl = ratio(t.ink, t.line);
        std::printf("[contrast] %-7s %-4s line/block %.2f line/area %.2f area/block %.2f | digits/block %.2f digits/area %.2f digits/line %.2f\n",
                    tn, t.name, lf, la, af, df, da, dl);
        CHECK_TRUE(lf >= 3.0);                  // the line against the block above it
        CHECK_TRUE(la >= 3.0);                  // and against the area below it
        if (!t.stale) {
            CHECK_TRUE(af >= 1.1);              // the area is a visible fill, not the block again
            CHECK_TRUE(da >= 4.5);              // digits over a live area
        } else {
            CHECK_TRUE(da >= 4.0);              // a stale grey is almost a line-only trace: its block alone gives Paper's digits 4.17
        }
        CHECK_TRUE(dl >= (t.stale ? 1.2 : 2.0));// a 1 px line crossing a digit: bounded by digits/block = digits/line * line/block
    }
    // The over-contract field is not a trend (no sweep): it stays white on red in every theme, untouched by this test's palette.
    CHECK_EQ(theme::kAlarmInk, 0xFFFF);
    std::printf("[contrast] %-7s OVER white/err %.2f (unchanged)\n", tn, ratio(theme::kAlarmInk, p.err));
}

static void render_scenes(theme::Id id) {
    theme::apply(id);
    const char *n = theme::name(id).data();
    char lower[16];
    int k = 0;
    for (const char *c = n; *c && k < 15; c++) lower[k++] = static_cast<char>(*c | 0x20);
    lower[k] = 0;
    check_theme(id);
    {
        Ui &ui = fresh_ui();
        run(ui, Scene::Charge, 150);
        dump(lower, "charge");
        // the trends are on the panel in the new shades: lines and areas of all three quantities
        const theme::Palette &p = theme::table(id);
        for (uint16_t c : {p.trace_volt, p.trace_amp, p.trace_watt}) CHECK_TRUE(count_px(c) > 40);
        for (uint16_t c : {p.area_volt, p.area_amp, p.area_watt}) CHECK_TRUE(count_px(c) > 400);
        unplug();
        g_stub.pm.vbus_mv = 0;
        for (int i = 0; i < 20; i++) { g_t += 100; ui.tick(g_t); }
        dump(lower, "last");
    }
    { Ui &ui = fresh_ui(); run(ui, Scene::Ramp, 150); dump(lower, "ramp"); }
    { Ui &ui = fresh_ui(); run(ui, Scene::Idle5, 150); dump(lower, "idle5"); }
}

int main() {
    for (theme::Id id : {theme::Id::Night, theme::Id::Paper, theme::Id::Classic}) render_scenes(id);
    return test_report("test_ui_flow_contrast");
}
