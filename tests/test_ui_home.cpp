// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home 2c and Detail pages: the pure readout texts and the states rendered through the real page code.
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


// ---- Home 2c ---------------------------------------------------------------------------------------
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

// Home 2c regions in logical coordinates (W hero by default: V in ledger row 1, A in row 2).
static constexpr int kHeroH = 74;                                  // y0..73
static constexpr int kHeroDigits[4] = {38, 25, 160, 39};          // x, y, w, h of the 64 px field
static constexpr int kRowDigitY[2] = {99, 156};                    // 48 px digits, x8..127, 30 rows
static constexpr int kRowTop[2] = {76, 137};
static constexpr int kCardX = 141, kCardW = 83;

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

// The live Home 2c: coral W in the hero, yellow V and cyan A in the ledger, PEAK in the upper card, the contract bars, no
// title bar or footer.
static void test_live_home() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 40);
    dump("2c", "live_pd");
    // Classic: black fields, each quantity's digits, unit and status edge in its own colour (restored 2026-11-13).
    CHECK_TRUE(ink_logical(ui, kHeroDigits[0], kHeroDigits[1], kHeroDigits[2], kHeroDigits[3], kWatt) > 60);   // W hero digits
    CHECK_TRUE(ink_logical(ui, 8, kRowDigitY[0], 120, 30, kVolt) > 40);   // V in ledger row 1
    CHECK_TRUE(ink_logical(ui, 8, kRowDigitY[1], 120, 30, kAmp) > 40);    // A in ledger row 2
    CHECK_TRUE(ink_logical(ui, 0, 0, 4, kHeroH, kWatt) == 4 * kHeroH);    // the hero's status edge is live
    CHECK_TRUE(ink_logical(ui, 10, 68, 205, 3, kWatt) > 0);               // W bar fill against the 100 W contract
    CHECK_TRUE(ink_logical(ui, 10, 68, 205, 3, kRule) > 0);               // ... on the rule-grey track
    CHECK_TRUE(ink_logical(ui, 6, 190, 128, 3, kAmp) > 0);                // A bar fill in row 2
    CHECK_EQ(ink_logical(ui, 6, 132, 110, 3, kVolt), 0);                  // no bar for V
    CHECK_TRUE(ink_logical(ui, kCardX, kRowTop[0], kCardW, 58, kText) > 10);   // PEAK in the upper card, ink
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, kStale), 0);                 // live is never grey
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);                 // no alarm, no pill
    CHECK_TRUE(ink_logical(ui, 160, 3, 60, 12, kDim) > 8);                // dim "max 100W"
    CHECK_TRUE(ink_logical(ui, 90, kRowTop[0] + 3, 46, 12, kDim) > 8);    // dim "max 20V"
    CHECK_TRUE(ink_logical(ui, 90, kRowTop[1] + 3, 46, 12, kDim) > 8);    // dim "max 5A"
    CHECK_TRUE(ink_logical(ui, 46, 3, 30, 12, kFaint) > 3);               // the "10s" window label after POWER
    CHECK_TRUE(ink_logical(ui, 4, 201, 40, 16, kChip) > 8);               // session row 1: the protocol chip
    CHECK_TRUE(ink_logical(ui, 4, 223, 220, 16, kDim) > 8);               // session row 2: the diagnosis
}

// Long Back cycles the hero W -> V -> A -> W: each quantity moves to the hero cell, the setting is stored, and a new Ui
// (a reboot) starts with the stored hero. A record without the field reads W.
static void test_hero_cycle() {
    Ui &ui = home_ui();
    CHECK_EQ(int(ui.context().keep.home_hero), int(hm::Qty::W));
    live_pd(false, 20010, 1520);
    run(ui, 20);
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Back)].lng, "Hero");
    ui.on_button(Button::Back, ButtonEvent::Long, g_t);
    run(ui, 20);
    dump("2c", "hero_v");
    CHECK_EQ(int(ui.context().keep.home_hero), int(hm::Qty::V));
    CHECK_EQ(int(uiprefs::stored().home_hero), 1);
    CHECK_TRUE(ink_logical(ui, kHeroDigits[0], kHeroDigits[1], kHeroDigits[2], kHeroDigits[3], kVolt) > 60);
    CHECK_TRUE(ink_logical(ui, 0, 0, 4, kHeroH, kVolt) == 4 * kHeroH);
    CHECK_TRUE(ink_logical(ui, 8, kRowDigitY[0], 120, 30, kWatt) > 40);   // W moves to row 1
    CHECK_EQ(ink_logical(ui, 10, 68, 205, 3, kVolt), 0);                  // the V hero has no bar
    ui.on_button(Button::Back, ButtonEvent::Long, g_t);
    run(ui, 20);
    dump("2c", "hero_a");
    CHECK_EQ(int(uiprefs::stored().home_hero), 2);
    CHECK_TRUE(ink_logical(ui, kHeroDigits[0], kHeroDigits[1], kHeroDigits[2], kHeroDigits[3], kAmp) > 60);
    CHECK_TRUE(ink_logical(ui, 8, kRowDigitY[1], 120, 30, kVolt) > 40);   // V in row 2
    CHECK_TRUE(ink_logical(ui, 8, 33, 22, 22, kFlow) > 40);               // the hero's flow arrow, left of the digits
    // A reboot restores the stored hero (main.cpp: restore_home_hero before init). fresh_ui() resets the stub store too.
    const uint8_t kept = uiprefs::stored().home_hero;
    Ui &again = fresh_ui();
    CHECK_EQ(int(again.context().keep.home_hero), int(hm::Qty::W));   // nothing restored: the default
    again.restore_home_hero(kept);
    CHECK_EQ(int(again.context().keep.home_hero), int(hm::Qty::A));
    again.on_button(Button::Back, ButtonEvent::Long, g_t);   // fresh_ui starts on Home: A -> W
    CHECK_EQ(int(again.context().keep.home_hero), int(hm::Qty::W));
    CHECK_EQ(int(uiprefs::stored().home_hero), 0);
    CHECK_EQ(int(hm::qty_from(7)), int(hm::Qty::W));
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

// The remote session block reads the Home protocol row's source through Ui::proto_now(): one derive_proto call
// site, the same inputs, no page needed (the Detail page is on screen here, not Home).
static void test_proto_now_is_the_homes_protocol() {
    Ui &ui = fresh_ui();
    ui.goto_page(static_cast<uint8_t>(Page::Detail), g_t);
    live_pd(false, 20010, 1520);
    run(ui, 40);
    const home::ProtoInfo p = ui.proto_now();
    CHECK_EQ(int(p.kind), int(home::ProtoKind::PdFixed));
    CHECK_EQ(int(p.mv), 20000);
    CHECK_EQ(int(p.max_mv), 20000);
    CHECK_EQ(int(p.ma), 5000);   // the 20 V PDO's current
    g_stub.contract = {}; g_stub.caps = {}; g_stub.fc = fastcharge::Protocol::Qc3;
    g_stub.pm.vbus_mv = 9000;
    run(ui, 10);
    CHECK_EQ(int(ui.proto_now().kind), int(home::ProtoKind::Qc3));
    CHECK_EQ(int(ui.proto_now().max_mv), 12000);
    unplug();
    run(ui, 40);
    CHECK_EQ(int(ui.proto_now().kind), int(home::ProtoKind::None));
}

// A PPS contract and a Quick Charge source (no PD caps, the D+/D- verdict only) fill the protocol row.
static void test_protocol_states() {
    {
        Ui &ui = home_ui();
        live_pd(false, 9100, 2000);
        g_stub.caps.pdos[1] = {pd::PdoType::Pps, 0, 3300, 11000, 3000, 33000};
        g_stub.contract.pos = 2; g_stub.contract.mv = 9100; g_stub.contract.op_ma = 2000; g_stub.contract.max_ma = 3000;
        run(ui, 40);
        dump("2c", "pps");
        CHECK_TRUE(ink_logical(ui, 0, 0, 4, kHeroH, kWatt) == 4 * kHeroH);
        CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kText) > 8);        // protocol row
        CHECK_TRUE(ink_logical(ui, 10, 68, 205, 3, kWatt) > 0);         // 33 W bar (the APDO's 11 V x 3 A)
        CHECK_TRUE(ink_logical(ui, 6, 190, 128, 3, kAmp) > 0);          // 3 A limit bar
    }
    {
        Ui &ui = home_ui();
        live_pd(false, 9000, 1200);
        g_stub.caps = {}; g_stub.contract = {}; g_stub.em = {};
        g_stub.fc = fastcharge::Protocol::Qc2_9V;
        run(ui, 40);
        dump("2c", "qc");
        CHECK_TRUE(ink_logical(ui, 0, 0, 4, kHeroH, kWatt) == 4 * kHeroH);
        CHECK_TRUE(ink_logical(ui, kHeroDigits[0], kHeroDigits[1], kHeroDigits[2], kHeroDigits[3], kWatt) > 60);
        CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kText) > 8);        // "QC2 9V"-style row
        CHECK_EQ(ink_logical(ui, 10, 68, 205, 3, kWatt), 0);            // no current rating: no W bar
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);           // no limit known: no alarm
    }
    {   // inline VOOC monitor: a live session, no PD; the row is "SVOOC 9.0V3.6A" from the readings, and the measured
        // current is no rating, so no limit bar and no alarm even at 6.5 A
        Ui &ui = home_ui();
        live_pd(false, 8962, 6460);
        g_stub.caps = {}; g_stub.contract = {}; g_stub.em = {};
        g_stub.fc = fastcharge::Protocol::UsbData;
        g_stub.vooc.seen = true; g_stub.vooc.r82 = 0x6B;
        run(ui, 40);
        dump("2c", "svooc_inline");
        CHECK_EQ(int(ui.proto_now().kind), int(home::ProtoKind::Svooc));
        CHECK_EQ(int(ui.proto_now().ma), 0);
        CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kText) > 8);        // protocol row
        CHECK_EQ(ink_logical(ui, 6, 190, 128, 3, kAmp), 0);             // no limit bar
        CHECK_EQ(ink_logical(ui, 10, 68, 205, 3, kWatt), 0);
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);           // no alarm
        g_stub.vooc = {};
    }
    {   // the phone on the USB-C port: a 5 V PD contract AND a live SuperVOOC session; the session wins the row, the PD
        // row returns when it ends
        Ui &ui = home_ui();
        live_pd(false, 9130, 2810);
        g_stub.contract.pos = 1; g_stub.contract.mv = 5000; g_stub.contract.op_ma = 3000; g_stub.contract.max_ma = 3000;
        g_stub.fc = fastcharge::Protocol::UsbData;
        g_stub.vooc.seen = true; g_stub.vooc.r82 = 0x6F;
        run(ui, 40);
        dump("2c", "svooc_over_pd");
        CHECK_EQ(int(ui.proto_now().kind), int(home::ProtoKind::Svooc));
        CHECK_EQ(int(ui.proto_now().ma), 0);
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 240, kError), 0);           // no alarm
        g_stub.vooc = {};
        run(ui, 40);
        CHECK_EQ(int(ui.proto_now().kind), int(home::ProtoKind::PdFixed));
    }
}

// The sweep paints the faint trend line behind the digits once samples arrive and nothing while idle.
static void test_sweep() {
    Ui &ui = home_ui();
    run(ui, 4);
    CHECK_EQ(ink_logical(ui, 4, 2, 220, 70, kWattL) + ink_logical(ui, 4, 2, 220, 70, kWattT), 0);   // idle: no sweep
    live_pd(false, 20010, 1520);
    run(ui, 60);
    // The old trend (restored 2026-11-13): a dark tint column under a brighter 1 px level line, on the black field.
    CHECK_TRUE(ink_logical(ui, 4, 2, 220, 70, kWattL) > 10);            // the hero's line
    CHECK_TRUE(ink_logical(ui, 4, 2, 220, 70, kWattT) > 100);           // and its tint under it
    CHECK_TRUE(ink_logical(ui, 0, kRowTop[0], 140, 58, kVoltL) > 10);   // and both ledger rows', over the same span
    CHECK_TRUE(ink_logical(ui, 0, kRowTop[1], 140, 57, kAmpL) > 10);
    CHECK_TRUE(ink_logical(ui, 0, kRowTop[0], 140, 58, kVoltT) > 100);
    CHECK_TRUE(ink_logical(ui, 0, kRowTop[1], 140, 57, kAmpT) > 100);
    CHECK_EQ(ink_logical(ui, kCardX, kRowTop[0], kCardW, 118, kVoltL) + ink_logical(ui, kCardX, kRowTop[0], kCardW, 118, kAmpL)
                 + ink_logical(ui, kCardX, kRowTop[0], kCardW, 118, kVoltT) + ink_logical(ui, kCardX, kRowTop[0], kCardW, 118, kAmpT), 0);   // never under the cards
    CHECK_TRUE(ink_logical(ui, 4, 2, 220, 66, kRule) > 0);              // the cursor line
}

// Every repaint strip of the moving sweep must leave the panel exactly as a full-frame render of the same state would.
// The reference is Ui::resync() at the same instant (every cell repainted whole, no sweep step: the frame is a replay);
// the incremental panel is put back afterwards and carried from frame to frame, so a strip that erases a pixel nobody
// draws again, or draws text twice, shows up as a mismatch on that frame.
// Rotations 1 and 3 are the stretched landscape ones (glyphs go through the native second pass).
// With `dump_as` set (and K2_DUMP), both panels are written as k2_home2_<dump_as>_{inc,full}.panel.
static int sweep_mismatches(Ui &ui, const char *dump_as, int *first_x, int *first_y) {
    static uint16_t inc[240 * 240];
    for (int i = 0; i < 240 * 240; i++) inc[i] = g_panel[i];
    ui.resync(g_t);
    int bad = 0;
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 240; x++) {
            if (g_panel[y * 240 + x] == inc[y * 240 + x]) continue;
            if (bad++ == 0) { *first_x = x; *first_y = y; }
        }
    if (dump_as) {
        char state[48];
        std::snprintf(state, sizeof state, "%s_full", dump_as);
        dump("sweep", state);
        for (int i = 0; i < 240 * 240; i++) g_panel[i] = inc[i];
        std::snprintf(state, sizeof state, "%s_inc", dump_as);
        dump("sweep", state);
    }
    for (int i = 0; i < 240 * 240; i++) g_panel[i] = inc[i];   // carry the incremental panel on
    return bad;
}

static void test_sweep_strips_match_full_render() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        Ui &ui = home_ui(rot);
        live_pd(false, 20010, 1520);
        run(ui, 6);
        int bad_frames = 0, first_x = -1, first_y = -1, first_frame = -1;
        for (int f = 0; f < 130; f++) {    // a full wrap of the 110 columns and then some
            // The level changes every 25 frames (the trace gets steps); the digits and the band follow the display filter
            // with a lag the full render does not have, so the frames right after a change are not compared.
            const int block = f / 25;
            live_pd(false, 18000 + (block % 3) * 1500, 700 + (block % 4) * 700);
            run(ui, 1);
            if (f % 25 < 12) continue;
            char name[16];
            std::snprintf(name, sizeof name, "rot%d", int(rot));
            int x = 0, y = 0;
            if (sweep_mismatches(ui, f == 70 ? name : nullptr, &x, &y) > 0 && bad_frames++ == 0) { first_x = x; first_y = y; first_frame = f; }
        }
        if (bad_frames) std::printf("sweep strips differ from the full render: rotation %d, %d of 130 frames (first: frame %d at panel %d,%d)\n",
                                    int(rot), bad_frames, first_frame, first_x, first_y);
        CHECK_EQ(bad_frames, 0);
    }
}

// A long Dn steps the window of all three sweeps forward, a long Up back; its label sits once, after the hero label.
static void test_window_cycle() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 10);
    const int a = ink_logical(ui, 46, 3, 30, 12, kFaint);   // the window label, faint after POWER
    CHECK_TRUE(a > 3);
    ui.on_button(Button::Plus, ButtonEvent::Long, g_t);
    run(ui, 10);
    CHECK_TRUE(ui.context().keep.home_window == trend::Window::S60);
    dump("2c", "window_60s");
    ui.on_button(Button::Plus, ButtonEvent::Long, g_t);
    run(ui, 10);
    dump("2c", "window_10m");
    ui.on_button(Button::Minus, ButtonEvent::Long, g_t);
    run(ui, 10);
    CHECK_TRUE(ui.context().keep.home_window == trend::Window::S60);
    ui.on_button(Button::Minus, ButtonEvent::Long, g_t);
    run(ui, 10);
    CHECK_TRUE(ui.context().keep.home_window == trend::Window::S10);
    CHECK_TRUE(ui.page() == Page::Home);   // the span presses never leave Flow
    for (int r = 0; r < 2; r++) CHECK_EQ(ink_logical(ui, 30, kRowTop[r] + 3, 18, 12, kFaint), 0);   // never in a ledger row
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Ok)].shrt, "Bench");
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
    CHECK_EQ(ink_logical(ui, 100, 3, 55, 12, kDim), 0);                 // left of the NO VBUS pill: no "max"
    for (int r = 0; r < 2; r++) CHECK_EQ(ink_logical(ui, 80, kRowTop[r] + 3, 56, 12, kDim), 0);
}

// Idle: grey zeros, a NO VBUS pill, a faint edge, "No source".
static void test_idle_home() {
    Ui &ui = home_ui();
    unplug();
    run(ui, 20);
    dump("2c", "idle");
    CHECK_TRUE(ink_logical(ui, 190, 1, 30, 17, kSurface) > 100);        // the NO VBUS pill fill
    CHECK_EQ(ink_logical(ui, 0, 0, 4, kHeroH, kWatt), 0);
    CHECK_EQ(ink_logical(ui, 0, 0, 4, kHeroH, kFaint), 4 * kHeroH);
    for (uint16_t c : {kVolt, kAmp, kWatt, kEnergy, kCharge})            // no live colours above the session rule (y194 is cyan)
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, c), 0);
    CHECK_TRUE(ink_logical(ui, 52, 201, 120, 16, kDim) > 8);            // "No source" (session row 1)
    CHECK_EQ(ink_logical(ui, 50, 146, 13, 13, kFlow), 0);               // no arrow
    CHECK_EQ(ink_logical(ui, 10, 66, 205, 7, kRule), 0);                // no bars
    CHECK_EQ(ink_logical(ui, 6, 188, 128, 5, kRule), 0);
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
    dump("2c", "idle-crossed");
    CHECK_TRUE(ink_logical(ui, 52, 223, 170, 16, kError) > 8);          // "CROSSED: flip one"
    CHECK_EQ(ink_logical(ui, 52, 223, 120, 16, kDim), 0);               // not "No source"
    Ui &hint = home_ui();
    unplug();
    feed_cc(hint, 25, cc_zero, cc_loaded);
    dump("2c", "idle-flip-hint");
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
    dump("2c", "stale");
    CHECK_TRUE(ink_logical(ui, 150, 1, 70, 17, kStale) > 100);          // the LAST pill fill
    CHECK_EQ(ink_logical(ui, 0, 0, 4, kHeroH, kStale), 4 * kHeroH);
    for (uint16_t c : {kVolt, kAmp, kWatt, kEnergy, kCharge})            // grey everywhere above the session rule
        CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, c), 0);
    CHECK_TRUE(ink_logical(ui, kHeroDigits[0], kHeroDigits[1], kHeroDigits[2], kHeroDigits[3], kStale) > 60);   // the frozen digits
    CHECK_TRUE(ink_logical(ui, kCardX, kRowTop[0], kCardW, 58, kStale) > 10);   // PEAK stays, grey, above Wh / Ah
    CHECK_TRUE(ink_logical(ui, kCardX, kRowTop[1], kCardW, 58, kStale) > 10);
}

// Near (>= 90 %) and over (> 100 %) of the contract: the hero's W against 20 V x 5 A = 100 W in full ("94% OF 100W"),
// the ledger's A in its short form ("94%").
static void test_alarms() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 4700);                                        // 94 W of 100 W, 4.7 A of 5 A
    run(ui, 20);
    dump("2c", "near");
    CHECK_TRUE(ink_logical(ui, 0, 0, 4, kHeroH, kCaution) == 4 * kHeroH);   // near: the hero's edge turns orange ...
    CHECK_TRUE(ink_logical(ui, 10, 0, 200, 18, kCaution) > 20);         // ... with the orange "94% OF 100W" chip
    CHECK_TRUE(ink_logical(ui, 4, kRowTop[1], 130, 18, kCaution) > 10); // row 2: the "94%" chip
    CHECK_TRUE(ink_logical(ui, 8, kRowDigitY[1], 120, 30, kCaution) > 40);   // and its digits
    CHECK_TRUE(ink_logical(ui, 10, 68, 205, 3, kCaution) > 0);          // and the bars turn orange
    CHECK_TRUE(ink_logical(ui, 6, 190, 128, 3, kCaution) > 0);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, kError), 0);
    CHECK_EQ(ink_logical(ui, 0, kRowTop[0], 140, 58, kCaution), 0);     // V never alarms
    live_pd(false, 20010, 5400);                                        // 108 %
    run(ui, 20);
    dump("2c", "over");
    CHECK_TRUE(ink_logical(ui, 0, 0, 224, kHeroH, kError) > 224 * kHeroH / 2);   // the whole hero cell is filled
    CHECK_TRUE(ink_logical(ui, 0, kRowTop[1], 140, 58, kError) > 140 * 58 / 2);  // and the IBUS row
    CHECK_EQ(ink_logical(ui, kCardX, kRowTop[0], kCardW, 118, kError), 0);       // the cards stay out of it
    live_pd(false, 20010, 1520);
    run(ui, 20);
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, kError), 0);               // back to normal: nothing left behind
    CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, kCaution), 0);
}

// The meter is missing: dashes and a red message.
static void test_nometer_home() {
    Ui &ui = home_ui();
    g_stub.pm_online = false;
    run(ui, 20);
    dump("2c", "nometer");
    CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kError) > 20);          // Fieldbook: the message sits on row 1
    for (uint16_t c : {kVolt, kAmp, kWatt}) CHECK_EQ(ink_logical(ui, 0, 0, 224, 194, c), 0);
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
    CHECK_EQ(ink_logical(ui, 4, 201, 220, 16, kError), 0);              // no red yet
    CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kDim) > 20);            // "Power meter starting"
    g_t = hm::kMeterGraceMs + 100;
    run(ui, 4);
    CHECK_TRUE(ink_logical(ui, 4, 201, 220, 16, kError) > 20);          // the grace is over: the error
    g_t = saved;
}

// ---- direction arrow: a 13 px glyph in the IBUS ledger row (22 px when A is the hero) ----------------

static constexpr int kAx = 50, kAy = 146, kAs = 13;

// Per half of the box, the widest physical ink line across the arrow's axis: the head's base is far wider than the
// shaft. Physical pixels, so the landscape stretch (a logical column can cover two panel columns) counts nothing twice.
struct ArrowInk { int top, bottom, left, right, total; };
static ArrowInk arrow_ink(Ui &ui, uint16_t color) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters, kSlotLens, kPanelThickness).content;
    const int x0 = box.x + kAx * box.w / 224, y0 = box.y + kAy * box.h / 240;
    const int x1 = box.x + (kAx + kAs) * box.w / 224, y1 = box.y + (kAy + kAs) * box.h / 240;
    const int w = x1 - x0, h = y1 - y0;
    ArrowInk a{};
    for (int i = 0; i < h / 2; i++) {
        a.top = std::max(a.top, ink(x0, y0 + i, w, 1, color));
        a.bottom = std::max(a.bottom, ink(x0, y1 - 1 - i, w, 1, color));
    }
    for (int i = 0; i < w / 2; i++) {
        a.left = std::max(a.left, ink(x0 + i, y0, 1, h, color));
        a.right = std::max(a.right, ink(x1 - 1 - i, y0, 1, h, color));
    }
    a.total = ink(x0, y0, w, h, color);
    return a;
}

// The head is the wide end. Toward the physical TOP port: up at rotation 0, left at 1, down at 2,
// right at 3; toward the BOTTOM port the opposite.
static void expect_points(const ArrowInk &a, int dir /*0 up, 1 left, 2 down, 3 right*/) {
    const int toward = dir == 0 ? a.top : dir == 1 ? a.left : dir == 2 ? a.bottom : a.right;
    const int away = dir == 0 ? a.bottom : dir == 1 ? a.right : dir == 2 ? a.top : a.left;
    if (!(toward > away + 2) || a.total <= 20)
        std::printf("arrow dir %d: toward %d away %d total %d\n", dir, toward, away, a.total);
    CHECK_EQ(toward > away + 2, 1);
    CHECK_EQ(a.total > 20, 1);
}

static void test_flow_arrow() {
    for (uint8_t r = 0; r < 4; r++) {
        Ui &ui = home_ui(r);
        live_pd(false, 20010, 1520);
        run(ui, 20);
        expect_points(arrow_ink(ui, kFlow), r);
        const char rot_name[2] = {static_cast<char>('0' + r), 0};
        dump("2c_flow_rot", rot_name);

        live_pd(true, 20010, 1520);                       // reverse: toward the physical bottom
        run(ui, 20);
        expect_points(arrow_ink(ui, kFlow), (r + 2) & 3);

        live_pd(false, 20010, 0);                          // deadband: a short faint dash
        g_stub.pm.flow = power::Flow::Idle; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
        run(ui, 20);
        CHECK_EQ(ink_logical(ui, kAx, kAy, kAs, kAs, kFlow), 0);
        const int dash = ink_logical(ui, kAx, kAy, kAs, kAs, kFaint);
        CHECK_EQ(dash >= 3 && dash <= 40, 1);

        live_pd(false, 20010, 1520);
        run(ui, 20);
        g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;   // unplugged: the last direction, grey
        g_stub.pm.flow = power::Flow::Idle;
        run(ui, 20);
        CHECK_EQ(ink_logical(ui, kAx, kAy, kAs, kAs, kFlow), 0);
        expect_points(arrow_ink(ui, kStale), r);
        // A steady screen does not repaint the arrow: wipe it, run more frames, it stays blank. (Stale, so no sweep strip
        // passes over the arrow's columns: a live strip repaints them, arrow included, by design.)
        wipe_logical(ui, kAx, kAy, kAs, kAs);
        CHECK_EQ(arrow_ink(ui, kStale).total, 0);
        run(ui, 20);
        CHECK_EQ(arrow_ink(ui, kStale).total, 0);

        Ui &none = home_ui(r);                             // nothing seen: no arrow at all
        g_stub.pm = {};
        run(none, 20);
        CHECK_EQ(ink_logical(none, kAx, kAy, kAs, kAs, kFlow), 0);
        CHECK_EQ(ink_logical(none, kAx, kAy, kAs, kAs, kFaint), 0);
    }
}

// The ledger arrow box lies right of the "IBUS" label, inside row 2, clear of the vertical rule.
static void test_flow_arrow_geometry() {
    CHECK_EQ(kAx >= 6 + 4 * 6 + 4, 1);
    CHECK_EQ(kAy >= hm::kRowY0[1] && kAy + kAs <= hm::kRowY1[1], 1);
    CHECK_EQ(kAx + kAs <= hm::kVRuleX, 1);
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
        dump("2c", n);
    }
}

// The right-column cards on the page: PEAK above, Wh / Ah below until the first half dwell, then the deck; an attach
// starts the cycle and the peaks over, long OK resets them; a swap repaints only its own cell.
static void test_cards_on_the_page() {
    Ui &ui = home_ui();
    live_pd(false, 20010, 1520);
    run(ui, 10);                                                        // 1 s live
    CHECK_EQ(int(ui.context().keep.cards.top()), int(hm::Card::Peak));
    CHECK_EQ(int(ui.context().keep.cards.bottom()), int(hm::Card::Energy));
    CHECK_TRUE(ink_logical(ui, kCardX, kRowTop[1], kCardW, 58, kText) > 40);     // Wh over Ah (Fieldbook: values in the ink)
    CHECK_EQ(ui.context().extremes.peak_mw(), 30400u);
    // A swap of the lower cell leaves the upper one alone: wipe it, cross the half dwell, it stays blank.
    wipe_logical(ui, kCardX + 1, kRowTop[0] + 1, kCardW - 2, 50);
    run(ui, 16);                                                        // 2.6 s live: the lower cell's first step
    CHECK_TRUE(int(ui.context().keep.cards.bottom()) != int(hm::Card::Energy));
    dump("2c", "card_lower_step");
    CHECK_EQ(ink_logical(ui, kCardX + 1, kRowTop[0] + 1, kCardW - 2, 50, kText), 0);   // PEAK not repainted (same value)
    CHECK_TRUE(ink_logical(ui, kCardX, kRowTop[1], kCardW, 58, kText) > 20);           // the lower cell repainted
    // A bigger load raises the peak; long OK resets the peaks and the cycle.
    live_pd(false, 20010, 3000);
    run(ui, 10);
    CHECK_EQ(ui.context().extremes.peak_mw(), 60000u);
    ui.on_button(Button::Ok, ButtonEvent::Long, g_t);
    CHECK_EQ(ui.context().extremes.peak_mw(), 0u);
    CHECK_EQ(int(ui.context().keep.cards.bottom()), int(hm::Card::Energy));
    // Unplug: PEAK over Wh / Ah, grey; a new attach starts over.
    unplug();
    g_stub.pm.vbus_mv = 0;
    run(ui, 10);
    CHECK_EQ(int(ui.context().keep.cards.top()), int(hm::Card::Peak));
    CHECK_EQ(int(ui.context().keep.cards.bottom()), int(hm::Card::Energy));
    live_pd(false, 5000, 500);
    run(ui, 2);
    CHECK_EQ(ui.context().extremes.peak_mw(), 2500u);
}

// The DROP card while a load runs: "need 1A" until the step is big enough, then the volts; the SAG line under it.
static void test_drop_and_sag_cards() {
    Ui &ui = home_ui();
    live_pd(false, 20000, 100);                                         // light load: the cable meter's idle reference
    run(ui, 30);
    live_pd(false, 19850, 3000);                                        // 2.9 A more: 150 mV drop, 0.75 % below 20 V
    run(ui, 40);
    const hm::Extremes &x = ui.context().extremes;
    CHECK_EQ(x.has_sag() ? 1 : 0, 1);
    CHECK_EQ(x.sag_level_mv(), 20000u);
    CHECK_EQ(x.sag_bp(), 75u);
    // The load step's new peak has PEAK wanting the upper cell (home::Attention).
    CHECK_EQ(int(ui.context().keep.attention.want()), int(hm::Card::Peak));
    CHECK_EQ(int(ui.context().keep.cards.top()), int(hm::Card::Peak));
    CHECK_TRUE(ui.context().keep.cards.forced());
    // Show the DROP card above at once through the seam (the attention cleared first) and render it.
    ui.context().keep.attention.reset();
    ui.context().keep.cards.force_top(hm::Card::Drop, 0xFF);
    run(ui, 3);
    dump("2c", "card_drop");
    CHECK_EQ(int(ui.context().keep.cards.top()), int(hm::Card::Drop));
    CHECK_TRUE(ink_logical(ui, 218, kRowTop[0] + 1, 5, 5, kFocus) == 25);   // the attention dot
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
    test_proto_now_is_the_homes_protocol();
    test_sweep();
    test_sweep_strips_match_full_render();
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
    test_hero_cycle();
    test_cards_on_the_page();
    test_drop_and_sag_cards();
    return test_report("test_ui_home");
}
