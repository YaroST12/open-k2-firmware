// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home v7 and Detail pages: the pure readout texts and the states rendered through the real page code.
// K2_DUMP=dir also writes the screens as k2_home2_<page>_<state>.panel for tools/panel2png.py.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/pd/pd_source.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

using namespace k2;
#define CHECK_TRUE(c) CHECK_EQ(int(!!(c)), 1)

static std::string contract_of(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus) {
    StrBuf<13> s;
    home_contract_text(c, p, vbus, s);
    return std::string(s.view());
}

static std::string short_of(const pd::Contract &c, fastcharge::Protocol p, int32_t vbus) {
    StrBuf<13> s;
    home_contract_short(c, p, vbus, s);
    return std::string(s.view());
}

static void test_contract_text() {
    pd::Contract c;
    CHECK_STR(contract_of(c, fastcharge::Protocol::NoQuick, 5000).c_str(), "");           // nothing negotiated
    c.valid = true; c.ready = true; c.pos = 2; c.mv = 9000; c.op_ma = 3000; c.max_ma = 3000;
    CHECK_STR(contract_of(c, fastcharge::Protocol::NoQuick, 9000).c_str(), "PD 9V 3A");
    CHECK_STR(short_of(c, fastcharge::Protocol::NoQuick, 9000).c_str(), "9V 3A");          // the charger icon carries "PD"
    c.mv = 20000; c.op_ma = 5000;
    CHECK_STR(contract_of(c, fastcharge::Protocol::NoQuick, 20000).c_str(), "PD 20V 5A");
    CHECK_STR(short_of(c, fastcharge::Protocol::NoQuick, 20000).c_str(), "20V 5A");
    c.mv = 5400; c.op_ma = 2200;                                                          // PPS point
    CHECK_STR(contract_of(c, fastcharge::Protocol::NoQuick, 5400).c_str(), "PD 5.4V 2.2A");
    c.ready = false;
    CHECK_STR(contract_of(c, fastcharge::Protocol::NoQuick, 5000).c_str(), "PD 5.4V ..");  // request not accepted yet
    pd::Contract none;
    CHECK_STR(contract_of(none, fastcharge::Protocol::Qc3, 9000).c_str(), "QC3 9V");       // legacy protocol: name + VBUS
    CHECK_STR(short_of(none, fastcharge::Protocol::Qc3, 9000).c_str(), "QC3 9V");
    CHECK_STR(contract_of(none, fastcharge::Protocol::Svooc, 10000).c_str(), "VOOC 10V");
    CHECK_STR(contract_of(none, fastcharge::Protocol::Pending, 5000).c_str(), "");
    CHECK_STR(contract_of(none, fastcharge::Protocol::Qc3, 400).c_str(), "");              // no VBUS: no contract
}

static void test_cable_and_cc_text() {
    pd::Emarker em;
    StrBuf<12> s;
    home_cable_text(em, s);
    CHECK_STR(s.view().data(), "no e-marker");
    em.valid = true; em.max_current_a = 5; em.max_power_w = 100;
    StrBuf<12> t;
    home_cable_text(em, t);
    CHECK_STR(t.view().data(), "5A 100W");
    em.max_current_a = 3; em.max_power_w = 60;
    StrBuf<12> u;
    home_cable_text(em, u);
    CHECK_STR(u.view().data(), "3A 60W");
    StrBuf<28> d1;
    em.max_voltage_v = 20;
    detail_cable_text(em, d1);
    CHECK_STR(d1.view().data(), "3A 60W 20V");
    StrBuf<28> d2;
    em.max_current_a = 5; em.max_power_w = 240; em.max_voltage_v = 48; em.is_active = true;
    detail_cable_text(em, d2);
    CHECK_STR(d2.view().data(), "5A 240W 48V EPR active");
    StrBuf<28> d3;
    detail_cable_text(pd::Emarker{}, d3);
    CHECK_STR(d3.view().data(), "no e-marker");
    StrBuf<10> c;
    home_cc_text(1, 1620, c);
    CHECK_STR(c.view().data(), "CC1 1.62V");
    StrBuf<10> e;
    home_cc_text(1, -1, e);
    CHECK_STR(e.view().data(), "CC1 -.--V");   // no ADC reading
}

static void test_offer_text() {
    pd::SourceCaps caps;
    StrBuf<28> s;
    detail_offer_text(caps, s);
    CHECK_STR(s.view().data(), "");
    caps.valid = true; caps.num_pdos = 2;
    caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    StrBuf<28> t;
    detail_offer_text(caps, t);
    CHECK_STR(t.view().data(), "100W max, 2 PDO");
}

// Idle Home: the idle note stays; the VBUS / IBUS maxima are never derived from the live reading.
static void test_idle_note() {
    CHECK_STR(home_idle_note(1), "No source");
    CHECK_STR(home_idle_note(0), "");
    CHECK_TRUE(home_idle_note(2) == nullptr);
    CHECK_TRUE(home_idle_note(3) == nullptr);
}


static Ui &fresh_ui() {
    static Ui ui;
    ui.~Ui();
    new (&ui) Ui;   // a never-used Ui: no live reading seen yet, rotation 0
    g_stub = StubState();
    g_stub.ms = 10000;
    ui.init(g_stub.ms);
    return ui;
}

static void dump(const char *page, const char *state) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char p[256];
    std::snprintf(p, sizeof p, "%s/k2_home2_%s_%s.panel", dir, page, state);
    if (FILE *f = std::fopen(p, "wb")) {
        std::fwrite(g_panel, 2, 240 * 240, f);
        std::fclose(f);
    }
}

static uint32_t g_t = 100000;

static void settle(Ui &ui) {
    for (int i = 0; i < 4; i++) { g_t += 300; ui.tick(g_t); }
}

static void live_pd(bool reverse, int mv, int ma) {
    g_stub.caps = {};
    static uint32_t rx = 0;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 3; g_stub.caps.spec_rev = 2;
    g_stub.caps.rx_count = ++rx;   // a new Source_Capabilities: the link leaves "waiting"
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.contract = {};
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 3;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = 3000; g_stub.contract.max_ma = 3000;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = mv;
    g_stub.pm.current_ma = reverse ? -ma : ma;
    g_stub.pm.power_mw = mv / 1000 * (reverse ? -ma : ma);
    g_stub.pm.flow = reverse ? power::Flow::Reverse : power::Flow::Forward;
    g_stub.em.valid = true; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 100; g_stub.em.max_voltage_v = 20;
    adc::Reading r;
    r.dp_mv = 600; r.dm_mv = 580; r.cc1_mv = 1620; r.cc2_mv = 70; r.vref_raw = 1489; r.vdd_mv = 3300;
    r.mcu = {312, true, false};
    g_stub.adc = r;
    g_stub.cc = 1;
    g_stub.fc = fastcharge::Protocol::Pd;
}

static int ink(int x0, int y0, int w, int h, uint16_t color) {
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) n += g_panel[y * 240 + x] == color;
    return n;
}

// ink() over a logical 224x240 rect, mapped through the current rotation's content viewport
// (the button strip sits on the physical button edge, which varies with rotation).
static int ink_logical(Ui &ui, int lx, int ly, int lw, int lh, uint16_t color) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters, kSlotLens,
                                    kPanelThickness).content;
    const int x0 = box.x + lx * box.w / 224, y0 = box.y + ly * box.h / 240;
    const int x1 = box.x + (lx + lw) * box.w / 224, y1 = box.y + (ly + lh) * box.h / 240;
    return ink(x0, y0, x1 - x0, y1 - y0, color);
}

// Blank a logical rect on the panel (same mapping as ink_logical) to prove a later frame does not repaint it.
static void wipe_logical(Ui &ui, int lx, int ly, int lw, int lh) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters, kSlotLens,
                                    kPanelThickness).content;
    const int x0 = box.x + lx * box.w / 224, y0 = box.y + ly * box.h / 240;
    const int x1 = box.x + (lx + lw) * box.w / 224, y1 = box.y + (ly + lh) * box.h / 240;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) g_panel[y * 240 + x] = 0;
}


// ---- Home v7 ---------------------------------------------------------------------------------------
namespace hm = k2::home;

static Ui &home_ui(uint8_t rot = 0) {
    Ui &ui = fresh_ui();
    ui.set_rotation(rot, g_t);
    ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    return ui;
}

// Run `n` frames 100 ms apart (the display filter lets digits change 4x a second).
static void run(Ui &ui, int n) {
    for (int i = 0; i < n; i++) { g_t += 100; ui.tick(g_t); }
}

static void unplug() {
    g_stub.pm = {};
    g_stub.contract = {}; g_stub.em = {}; g_stub.caps = {};
    g_stub.fc = fastcharge::Protocol::NoInput;
}

// Cell regions in logical coordinates.
static constexpr int kVcellY = 0, kVcellH = 70, kAcellY = 72, kAcellH = 70;

// Both pages render every state (the Detail page keeps its screens in the sheet).
static void test_render_states() {
    for (int pg = 0; pg < 2; pg++) {
        const char *name = pg == 0 ? "big" : "detail";
        Ui &ui = fresh_ui();
        ui.goto_page(static_cast<uint8_t>(pg == 0 ? Page::Home : Page::Detail), g_t);
        settle(ui);
        dump(name, "idle");
        live_pd(false, 20010, 1520);
        settle(ui);
        dump(name, "forward");
        live_pd(true, 9000, 2000);
        g_stub.contract.mv = 9000; g_stub.contract.op_ma = 2000;
        settle(ui);
        dump(name, "reverse");
        g_stub.contract = {}; g_stub.caps = {}; g_stub.fc = fastcharge::Protocol::Qc3;   // legacy QC
        live_pd(false, 9000, 1500);
        g_stub.contract = {}; g_stub.caps = {};
        settle(ui);
        dump(name, "legacy");
        live_pd(false, 20010, 1520);
        g_stub.em = {};
        settle(ui);
        dump(name, "noemarker");
        unplug();
        settle(ui);
        dump(name, "novbus");
        g_stub.pm_online = false;      // meter not found
        settle(ui);
        dump(name, "error");
    }
}

// The live Home: yellow V and cyan A digits, the W, the energy block, the limit bar, no title bar or footer.
static void test_live_home() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 40);
    dump("v7", "live_pd");
    CHECK_TRUE(ink_logical(ui, 39, 24, 162, 39, kVolt) > 60);           // V digits in the hero font
    CHECK_TRUE(ink_logical(ui, 39, 94, 162, 39, kAmp) > 60);            // A digits
    CHECK_TRUE(ink_logical(ui, 4, 153, 96, 48, kWatt) > 40);   // W
    CHECK_TRUE(ink_logical(ui, 124, 144, 99, 24, kEnergy) > 10);        // Wh
    CHECK_TRUE(ink_logical(ui, 124, 170, 99, 24, kCharge) > 10);        // Ah
    CHECK_TRUE(ink_logical(ui, 0, 0, 4, 70, kVolt) == 4 * 70);          // the status edge is live
    CHECK_TRUE(ink_logical(ui, 0, 72, 4, 70, kAmp) == 4 * 70);
    CHECK_TRUE(ink_logical(ui, 10, 136, 205, 3, kAmp) > 0);             // limit bar fill
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 70, kStale), 0);                // live is never grey
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);               // no alarm, no pill
    CHECK_TRUE(ink_logical(ui, 150, 3, 70, 12, kDim) > 8);              // dim "max 20V"
    CHECK_TRUE(ink_logical(ui, 170, 75, 50, 12, kDim) > 8);             // dim "max 3A"
    CHECK_TRUE(ink_logical(ui, 40, 3, 30, 12, kFaint) > 3);             // the "10s" window label
    CHECK_TRUE(ink_logical(ui, 4, 201, 40, 16, kDim) > 8);              // USB tag
    CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kDim) > 8);             // protocol row
}

// A held Rd pin shows the caution badge "Rd pin CC2" in the VBUS cell's upper right (the Home status badge);
// it goes away when the pin clears (e.g. on the far end's detach).
static void test_home_shows_the_rd_pin_badge() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 40);
    CHECK_EQ(ink_logical(ui, 131, 1, 91, 17, kCaution), 0);          // no pin: no badge
    g_stub.pin = 2;                                                   // a leftover pin on CC2
    run(ui, 4);
    CHECK_TRUE(ink_logical(ui, 131, 1, 91, 17, kCaution) > 40);      // the "Rd pin CC2" pill
    g_stub.pin = 0;
    run(ui, 4);
    CHECK_EQ(ink_logical(ui, 131, 1, 91, 17, kCaution), 0);           // cleared: the badge is gone
}

// A PPS contract and a Quick Charge source (no PD caps, the D+/D- verdict only) fill the protocol row.
static void test_protocol_states() {
    {
        Ui &ui = home_ui();
        live_pd(false, 9100, 2000);
        g_stub.caps.pdos[1] = {pd::PdoType::Pps, 0, 3300, 11000, 3000, 33000};
        g_stub.contract.pos = 2; g_stub.contract.mv = 9100; g_stub.contract.op_ma = 2000; g_stub.contract.max_ma = 3000;
        run(ui, 40);
        dump("v7", "pps");
        CHECK_TRUE(ink_logical(ui, 0, 0, 4, 70, kVolt) == 4 * 70);
        CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kDim) > 8);         // protocol row
        CHECK_TRUE(ink_logical(ui, 10, 136, 205, 3, kAmp) > 0);         // 3 A limit bar
    }
    {
        Ui &ui = home_ui();
        live_pd(false, 9000, 1200);
        g_stub.caps = {}; g_stub.contract = {}; g_stub.em = {};
        g_stub.fc = fastcharge::Protocol::Qc2_9V;
        run(ui, 40);
        dump("v7", "qc");
        CHECK_TRUE(ink_logical(ui, 0, 0, 4, 70, kVolt) == 4 * 70);
        CHECK_TRUE(ink_logical(ui, 39, 24, 162, 39, kVolt) > 60);
        CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kDim) > 8);         // "QC2 9V"-style row
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);           // no limit known: no alarm
    }
}

// The sweep paints the faint trend line behind the digits once samples arrive and nothing while idle.
static void test_sweep() {
    Ui &ui = home_ui();
    run(ui, 4);
    CHECK_EQ(ink_logical(ui, 4, 2, 220, 66, kVoltL), 0);                // idle: no sweep
    live_pd(false, 20010, 1520);
    run(ui, 60);
    CHECK_TRUE(ink_logical(ui, 4, 2, 220, 66, kVoltL) > 10);
    CHECK_TRUE(ink_logical(ui, 4, 74, 220, 66, kAmpL) > 10);
    CHECK_TRUE(ink_logical(ui, 4, 2, 220, 66, kRule) > 0);              // the cursor line
}

// OK short cycles the window label; the label sits only in the VBUS cell.
static void test_window_cycle() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 10);
    const int a = ink_logical(ui, 40, 3, 30, 12, kFaint);
    CHECK_TRUE(a > 3);
    ui.on_button(Button::Ok, ButtonEvent::Short, g_t);
    run(ui, 10);
    dump("v7", "window_60s");
    ui.on_button(Button::Ok, ButtonEvent::Short, g_t);
    run(ui, 10);
    dump("v7", "window_10m");
    ui.on_button(Button::Ok, ButtonEvent::Short, g_t);
    run(ui, 10);
    CHECK_EQ(ink_logical(ui, 40, 75, 30, 12, kFaint), 0);               // never in the IBUS cell
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Ok)].shrt, "Zoom");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Ok)].lng, "Reset");
}

// The old non-PD row printed "max 1.0V 3A" while unplugged. The maxima come from the contract or
// protocol and only while live.
static void test_max_text_never_comes_from_live_vbus() {
    hm::ProtoInfo p;
    p.kind = hm::ProtoKind::TypeC; p.mv = 5000; p.ma = 3000; p.max_mv = 5000;
    StrBuf<12> s;
    for (hm::Mode m : {hm::Mode::NoMeter, hm::Mode::Idle, hm::Mode::Stale}) {
        CHECK_EQ(hm::max_volts_text(m, p, s) ? 1 : 0, 0);
        CHECK_EQ(hm::max_amps_text(m, p, s) ? 1 : 0, 0);
    }
    CHECK_EQ(hm::max_volts_text(hm::Mode::Live, p, s) ? 1 : 0, 1);
    CHECK_STR(s.view().data(), "max 5V");
    // On the page: a 1.0 V leak with a current reading: no "max" text anywhere in the cells.
    Ui &ui = home_ui();
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 1000 - 1; g_stub.pm.current_ma = 3; g_stub.pm.power_mw = 3;
    g_stub.fc = fastcharge::Protocol::NoInput;
    run(ui, 20);
    CHECK_EQ(ink_logical(ui, 100, 3, 55, 12, kDim), 0);                 // left of the NO VBUS pill: no "max 1.0V"
    CHECK_EQ(ink_logical(ui, 170, 75, 50, 12, kDim), 0);
}

// Idle: grey zeros, a NO VBUS pill, a faint edge, "No source".
static void test_idle_home() {
    Ui &ui = home_ui();
    unplug();
    run(ui, 20);
    dump("v7", "idle");
    CHECK_TRUE(ink_logical(ui, 190, 1, 30, 17, kSurface) > 100);        // the NO VBUS pill fill
    CHECK_EQ(ink_logical(ui, 0, 0, 4, 70, kVolt), 0);
    CHECK_EQ(ink_logical(ui, 0, 0, 4, 70, kFaint), 4 * 70);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kVolt), 0);                // no live colours at all
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kAmp), 0);
    CHECK_TRUE(ink_logical(ui, 52, 223, 120, 16, kDim) > 8);            // "No source"
    CHECK_EQ(ink_logical(ui, 8, 101, 22, 22, kFlow), 0);                // no arrow
    CHECK_EQ(ink_logical(ui, 10, 134, 205, 7, kRule), 0);               // no limit bar
}

// Idle with the rig's CC levels (a charger on one cable, a phone toggling on the other, crossed): row 2 says so in the error
// colour instead of "No source"; the case-3 hint is amber; a charger alone or a phone alone keeps "No source".
static void feed_cc(Ui &ui, int frames, int (*cc1)(int), int (*cc2)(int)) {
    for (int k = 0; k < frames; k++) {
        adc::Reading r;
        r.cc1_mv = uint16_t(cc1(k)); r.cc2_mv = uint16_t(cc2(k));
        g_stub.adc = r;
        run(ui, 1);
    }
}
static int cc_pixel(int k) { static const int c[6] = {3300, 0, 1800, 3300, 1200, 0}; return c[k % 6]; }
static int cc_loaded(int k) { return 480 + (k % 4) * 20; }
static int cc_zero(int) { return 0; }
static int cc_rp(int) { return 3290; }
static void test_idle_home_says_crossed() {
    Ui &ui = home_ui();
    unplug();
    feed_cc(ui, 25, cc_pixel, cc_loaded);
    dump("v7", "idle-crossed");
    CHECK_TRUE(ink_logical(ui, 52, 223, 170, 16, kError) > 8);          // "CROSSED: flip one"
    CHECK_EQ(ink_logical(ui, 52, 223, 120, 16, kDim), 0);               // not "No source"
    Ui &hint = home_ui();
    unplug();
    feed_cc(hint, 25, cc_zero, cc_loaded);
    dump("v7", "idle-flip-hint");
    CHECK_TRUE(ink_logical(hint, 52, 223, 170, 16, kCaution) > 8);      // "Flip a cable?"
    Ui &charger = home_ui();
    unplug();
    feed_cc(charger, 25, cc_zero, cc_rp);
    CHECK_EQ(ink_logical(charger, 52, 223, 170, 16, kError), 0);
    CHECK_EQ(ink_logical(charger, 52, 223, 170, 16, kCaution), 0);
    Ui &phone = home_ui();
    unplug();
    feed_cc(phone, 25, cc_pixel, cc_zero);
    CHECK_EQ(ink_logical(phone, 52, 223, 170, 16, kError), 0);
    CHECK_EQ(ink_logical(phone, 52, 223, 170, 16, kCaution), 0);
    g_stub.adc.reset();
}

// Unplugged after a live reading: everything grey, the LAST pill counts the age.
static void test_stale_home() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 40);
    unplug();
    g_stub.pm.vbus_mv = 0;
    run(ui, 20);
    dump("v7", "stale");
    CHECK_TRUE(ink_logical(ui, 150, 1, 70, 17, kStale) > 100);          // the LAST pill fill
    CHECK_EQ(ink_logical(ui, 0, 0, 4, 70, kStale), 4 * 70);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kVolt), 0);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kAmp), 0);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kEnergy), 0);
    CHECK_TRUE(ink_logical(ui, 39, 24, 162, 39, kStale) > 60);          // the frozen digits
}

// Near (>= 90 %) and over (> 100 %) of the contract current.
static void test_alarms() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 4700);                                        // 5 A PDO: 94 %
    run(ui, 20);
    dump("v7", "near");
    CHECK_TRUE(ink_logical(ui, 0, 72, 4, 70, kCaution) == 4 * 70);
    CHECK_TRUE(ink_logical(ui, 10, 72, 200, 20, kCaution) > 20);        // the warn triangle and "94% OF 5A"
    CHECK_EQ(ink_logical(ui, 0, 72, 224, 70, kError), 0);
    live_pd(false, 20010, 5400);                                        // 108 %
    run(ui, 20);
    dump("v7", "over");
    CHECK_TRUE(ink_logical(ui, 0, 72, 224, 70, kError) > 224 * 70 / 2); // the whole IBUS cell is filled
    CHECK_TRUE(ink_logical(ui, 100, 1, 120, 17, kError) + ink_logical(ui, 100, 1, 120, 17, kCaution) > 100);   // the OVER pill
    live_pd(false, 20010, 1520);
    run(ui, 20);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 142, kError), 0);               // back to normal: nothing left behind
}

// The meter is missing: dashes and a red message.
static void test_nometer_home() {
    Ui &ui = home_ui();
    g_stub.pm_online = false;
    run(ui, 20);
    dump("v7", "nometer");
    CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kError) > 20);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 142, kVolt), 0);
}

// The first seconds of a boot: the meter is still being found, so the row says "starting" in the neutral colour and only a meter
// still missing after hm::kMeterGraceMs is the red error (hardware 2026-11-09: a key-mode boot showed the error for seconds, then the
// chip answered by itself).
static void test_nometer_grace() {
    CHECK_EQ(hm::meter_starting(false, 0), true);
    CHECK_EQ(hm::meter_starting(false, hm::kMeterGraceMs - 1), true);
    CHECK_EQ(hm::meter_starting(false, hm::kMeterGraceMs), false);
    CHECK_EQ(hm::meter_starting(true, 0), false);
    const uint32_t saved = g_t;
    g_t = 1000;
    Ui &ui = home_ui();
    g_stub.pm_online = false;
    run(ui, 10);                                                        // 2 s of uptime
    CHECK_EQ(ink_logical(ui, 4, 223, 220, 16, kError), 0);              // no red yet
    CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kDim) > 20);            // "Power meter starting"
    g_t = hm::kMeterGraceMs + 100;
    run(ui, 4);
    CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kError) > 20);          // the grace is over: the error
    g_t = saved;
}

// ---- direction arrow: a 22 px glyph beside the current ----------------------------------------------

static constexpr int kAx = 8, kAy = 101, kAs = 22;

struct ArrowInk { int top, bottom, left, right, total; };
static ArrowInk arrow_ink(Ui &ui, uint16_t color) {
    ArrowInk a;
    a.top = ink_logical(ui, kAx, kAy, kAs, kAs / 2, color);
    a.bottom = ink_logical(ui, kAx, kAy + kAs / 2, kAs, kAs / 2, color);
    a.left = ink_logical(ui, kAx, kAy, kAs / 2, kAs, color);
    a.right = ink_logical(ui, kAx + kAs / 2, kAy, kAs / 2, kAs, color);
    a.total = ink_logical(ui, kAx, kAy, kAs, kAs, color);
    return a;
}

// The head is the heavy end. Toward the physical TOP port: up at rotation 0, left at 1, down at 2,
// right at 3; toward the BOTTOM port the opposite.
static void expect_points(const ArrowInk &a, int dir /*0 up, 1 left, 2 down, 3 right*/) {
    const int toward = dir == 0 ? a.top : dir == 1 ? a.left : dir == 2 ? a.bottom : a.right;
    const int away = dir == 0 ? a.bottom : dir == 1 ? a.right : dir == 2 ? a.top : a.left;
    CHECK_EQ(toward > away + 4, 1);
    CHECK_EQ(a.total > 40, 1);
}

static void test_flow_arrow() {
    for (uint8_t r = 0; r < 4; r++) {
        Ui &ui = home_ui(r);
        live_pd(false, 20010, 1520);
        run(ui, 20);
        expect_points(arrow_ink(ui, kFlow), r);
        const char rot_name[2] = {static_cast<char>('0' + r), 0};
        dump("v7_flow_rot", rot_name);
        // A steady screen does not repaint the arrow: wipe it, run more frames, it stays blank.
        wipe_logical(ui, kAx, kAy, kAs, kAs);
        CHECK_EQ(arrow_ink(ui, kFlow).total, 0);
        run(ui, 20);
        CHECK_EQ(arrow_ink(ui, kFlow).total, 0);

        live_pd(true, 20010, 1520);                        // reverse: toward the physical bottom
        run(ui, 20);
        expect_points(arrow_ink(ui, kFlow), (r + 2) & 3);

        live_pd(false, 20010, 0);                          // deadband: a short faint dash
        g_stub.pm.flow = power::Flow::Idle; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
        run(ui, 20);
        CHECK_EQ(ink_logical(ui, kAx, kAy, kAs, kAs, kFlow), 0);
        const int dash = ink_logical(ui, kAx, kAy, kAs, kAs, kFaint);
        CHECK_EQ(dash >= 8 && dash <= 80, 1);

        live_pd(false, 20010, 1520);
        run(ui, 20);
        g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;   // unplugged: the last direction, grey
        g_stub.pm.flow = power::Flow::Idle;
        run(ui, 20);
        CHECK_EQ(ink_logical(ui, kAx, kAy, kAs, kAs, kFlow), 0);
        expect_points(arrow_ink(ui, kStale), r);

        Ui &none = home_ui(r);                             // nothing seen: no arrow at all
        g_stub.pm = {};
        run(none, 20);
        CHECK_EQ(ink_logical(none, kAx, kAy, kAs, kAs, kFlow), 0);
        CHECK_EQ(ink_logical(none, kAx, kAy, kAs, kAs, kFaint), 0);
    }
}

// The arrow box lies between the label column and the digits, on the current's row.
static void test_flow_arrow_geometry() {
    CHECK_EQ(kAx + kAs <= 39, 1);
    CHECK_EQ(kAy >= hm::kAmpCell.top + 24 && kAy + kAs <= hm::kAmpCell.bottom, 1);
}

// Three consecutive frames of the live screen, for the animation strip.
static void test_animation_frames() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 40);
    for (int i = 0; i < 3; i++) {
        g_t += 250;
        ui.tick(g_t);
        char n[8];
        std::snprintf(n, sizeof n, "anim%d", i);
        dump("v7", n);
    }
}

int main() {
    test_contract_text();
    test_cable_and_cc_text();
    test_offer_text();
    test_idle_note();
    test_render_states();
    test_live_home();
    test_home_shows_the_rd_pin_badge();
    test_protocol_states();
    test_sweep();
    test_window_cycle();
    test_max_text_never_comes_from_live_vbus();
    test_idle_home();
    test_idle_home_says_crossed();
    test_stale_home();
    test_alarms();
    test_nometer_home();
    test_nometer_grace();
    test_flow_arrow_geometry();
    test_flow_arrow();
    test_animation_frames();
    return test_report("test_ui_home");
}
