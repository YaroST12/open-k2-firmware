// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Flow face (Home) draws a patient-monitor trend behind the digits of every quantity field. The user took the pre-Fieldbook
// look back (2026-11-13: "older graphs looked better"): a black field, the quantity's own colour for its digits, a dark tint
// column under a brighter 1 px level line (kVoltT/kVoltL, kAmpT/kAmpL, kWattT/kWattL), and for a stale (LAST) field a grey
// tint with a faint line. This test renders the face with realistic live data (a 20 V 3 A charge with ripple, a 5 V to 20 V
// ramp, a 5 V idle, a LAST snapshot) and pins that look on the panel:
//  - the line and tint colours of all three quantities are drawn on a black ground, and none of them when idle;
//  - the LAST field draws neither a live line nor a live tint;
//  - the digits stay readable over the tint (WCAG 2.x relative luminance, at least 4.5:1).
// K2_DUMP=dir also writes every render as k2_flow_<scenario>.panel (tools/panel2png.py).
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
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

static void dump(const char *scene) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char p[256];
    std::snprintf(p, sizeof p, "%s/k2_flow_%s.panel", dir, scene);
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

// The palette's own pairs: the digits of a quantity over its tint column.
static void check_palette() {
    struct Pair { const char *name; uint16_t digits, tint, line; };
    const Pair pairs[] = {{"V", kVolt, kVoltT, kVoltL}, {"A", kAmp, kAmpT, kAmpL}, {"W", kWatt, kWattT, kWattL}};
    for (const Pair &p : pairs) {
        const double dt = ratio(p.digits, p.tint), dg = ratio(p.digits, kBg), lg = ratio(p.line, kBg);
        std::printf("[trend] %s digits/tint %.2f digits/ground %.2f line/ground %.2f\n", p.name, dt, dg, lg);
        CHECK_TRUE(dt >= 4.5);   // the numerals stay readable over their own tint
        CHECK_TRUE(dg >= 4.5);
        CHECK_TRUE(lg > 1.5);    // the line is a visible step above the ground (the old look: a dim shade, not a second colour)
    }
    CHECK_EQ(kBg, 0x0000);       // black ground
}

int main() {
    check_palette();
    {
        Ui &ui = fresh_ui();
        run(ui, Scene::Charge, 150);
        dump("charge");
        // the three trends are on the panel: the line of each quantity and its tint column under it
        for (uint16_t c : {kVoltL, kAmpL, kWattL}) CHECK_TRUE(count_px(c) > 40);
        for (uint16_t c : {kVoltT, kAmpT, kWattT}) CHECK_TRUE(count_px(c) > 400);
        unplug();
        g_stub.pm.vbus_mv = 0;
        for (int i = 0; i < 20; i++) { g_t += 100; ui.tick(g_t); }
        dump("last");
        // LAST: the trend keeps its shape in grey, never in the quantities' colours
        for (uint16_t c : {kVoltL, kAmpL, kWattL}) CHECK_EQ(count_px(c), 0);
        for (uint16_t c : {kVoltT, kAmpT, kWattT}) CHECK_EQ(count_px(c), 0);
        CHECK_TRUE(count_px(kPanelBg) > 400);   // the stale tint
    }
    { Ui &ui = fresh_ui(); run(ui, Scene::Ramp, 150); dump("ramp"); for (uint16_t c : {kVoltL, kAmpL, kWattL}) CHECK_TRUE(count_px(c) > 40); }
    { Ui &ui = fresh_ui(); run(ui, Scene::Idle5, 150); dump("idle5"); for (uint16_t c : {kVoltL, kAmpL, kWattL}) CHECK_TRUE(count_px(c) > 40); }
    {   // no source: no trend, nothing drawn in the live shades
        Ui &ui = fresh_ui();
        unplug();
        g_stub.pm.vbus_mv = 0;
        for (int i = 0; i < 40; i++) { g_t += 100; ui.tick(g_t); }
        dump("idle");
        for (uint16_t c : {kVoltL, kAmpL, kWattL, kVoltT, kAmpT, kWattT}) CHECK_EQ(count_px(c), 0);
    }
    return test_report("test_ui_flow_trend");
}
