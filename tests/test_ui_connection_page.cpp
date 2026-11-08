// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Connection (page level, real Ui): which tab it opens on, the passive port topology it senses from the CC
// lines, the tab switching and the action pop-up (greyed Spoof while a device is inline), the Tools shortcut
// to Spoof, and that merely showing an empty state never drives the lines.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "ui_nav.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(int(!!(c)), 1)

// A new Ui every time: init() keeps the submenu stack and the link state of the previous case.
static Ui &fresh_ui() {
    static std::unique_ptr<Ui> ui;
    ui = std::make_unique<Ui>();
    g_stub = StubState();
    g_stub.ms = 10000;
    g_stub.cc = 0;                    // the stub's default (CC1 active) would make a charger "attached"
    g_stub.pm.vbus_mv = 0;
    ui->init(g_stub.ms);
    ui->set_rotation(0, g_stub.ms);
    for (uint32_t t = 10100; t <= 11500; t += 100) ui->tick(t);   // the link starts attached; VBUS down + no CC detaches it
    g_stub.ms = 12000;
    return *ui;
}

// A load behind the K2: the link goes inline once it sees VBUS and current flowing through.
static void make_inline(Ui &ui, uint32_t t) {
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 19700;
    g_stub.pm.current_ma = 2000;
    for (int i = 0; i < 3; i++) ui.tick(t + 100u * i);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
}

static const ConnectionPage &cp(const Ui &ui) { return ui.connection_page(); }

// CC levels the ADC averages: both lines at Rp = a receptacle charger straight on the top plug, one = a cable
// in between, none = nothing.
static void set_cc(int cc1, int cc2) {
    adc::Reading r;
    r.cc1_mv = static_cast<uint16_t>(cc1);
    r.cc2_mv = static_cast<uint16_t>(cc2);
    g_stub.adc = r;
}

// Open Connection on `tab_alias` and let the topology window fill (ten-hertz ticks, ~1 s).
static uint32_t open_and_settle(Ui &ui, uint8_t page, uint32_t t) {
    ui.goto_page(page, t);
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    return t;
}

// Every active CC / PD operation the stub counts (the link's own inline state is not one: it only reads).
static int acts() {
    return g_stub.ra_ons + g_stub.ra_offs + g_stub.rd_pauses + g_stub.scans + g_stub.cc_detaches + g_stub.rescans +
           g_stub.force_line + g_stub.identifies + g_stub.getcaps + g_stub.requests + g_stub.probes + g_stub.hard_resets +
           g_stub.new_sources + g_stub.clear_sources + g_stub.unit_detaches + g_stub.unit_attaches + g_stub.releases +
           g_stub.rd_releases + g_stub.rd_run_changes;
}

static void test_default_tab_on_open() {
    {   // nothing anywhere: Source, empty
        Ui &ui = fresh_ui();
        ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), 20000);
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    }
    {   // only a cable: Cable
        Ui &ui = fresh_ui();
        g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2;
        ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), 20000);
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    }
    {   // a charger is attached: Source (even with a cable held)
        Ui &ui = fresh_ui();
        g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2;
        ui.debug_link(true, 20000);
        ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), 20100);
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    }
    {   // a device is inline: Sink wins over everything
        Ui &ui = fresh_ui();
        make_inline(ui, 20000);
        g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2;
        ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), 20100);
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    }
    {   // the remote's tab aliases pick the tab, whatever is attached
        Ui &ui = fresh_ui();
        CHECK(ui.goto_page(kConnTabAlias + 2, 20000));
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
        CHECK(ui.goto_page(kConnTabAlias + 1, 20100));
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
        CHECK(ui.goto_page(kConnTabAlias, 20200));
        CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    }
}

static void test_topology_from_the_cc_lines() {
    struct Case { int cc1, cc2; raguard::Topology want; const char *name; };
    const Case cases[] = {
        {3300, 3300, raguard::Topology::DirectReceptacle, "Rp on both lines"},
        {3300, 0, raguard::Topology::CableSingleCc, "Rp on one line"},
        {0, 3300, raguard::Topology::CableSingleCc, "Rp on the other line"},
        {0, 0, raguard::Topology::NoSource, "nothing"},
    };
    for (const Case &c : cases) {
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 0;
        set_cc(c.cc1, c.cc2);
        open_and_settle(ui, kConnTabAlias, 20000);
        if (cp(ui).facts().topo != c.want) std::printf("topology case: %s\n", c.name);
        CHECK_EQ(int(cp(ui).facts().topo), int(c.want));
    }
}

static void test_topology_unknown_unless_passive_and_dead() {
    {   // VBUS up: the lines are not a waiting charger any more
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 5000;
        set_cc(3300, 3300);
        open_and_settle(ui, kConnTabAlias, 20000);
        CHECK_EQ(int(cp(ui).facts().topo), int(raguard::Topology::Unknown));
    }
    {   // no ADC reading at all
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 0;
        g_stub.adc.reset();
        open_and_settle(ui, kConnTabAlias, 20000);
        CHECK_EQ(int(cp(ui).facts().topo), int(raguard::Topology::Unknown));
    }
    {   // a device inline (VBUS and current flowing through)
        Ui &ui = fresh_ui();
        set_cc(3300, 3300);
        make_inline(ui, 20000);
        open_and_settle(ui, kConnTabAlias, 20400);
        CHECK_EQ(int(cp(ui).facts().topo), int(raguard::Topology::Unknown));
    }
    {   // the K2 holds Rd itself (self-powered run): not passive, whatever the lines show
        Ui &ui = fresh_ui();
        g_stub.self_powered = true;
        set_cc(3300, 3300);
        open_and_settle(ui, kConnTabAlias, 20000);
        CHECK_EQ(int(cp(ui).facts().topo), int(raguard::Topology::Unknown));
    }
}

// Showing every empty state on every topology only reads: no Rd, Ra, scan, rescan, request or line force.
static void test_empty_states_are_passive() {
    const int ccs[][2] = {{3300, 3300}, {3300, 0}, {0, 0}};
    for (const auto &cc : ccs) {
        for (int tab = 0; tab < 3; tab++) {
            Ui &ui = fresh_ui();
            g_stub.pm.vbus_mv = 0;
            set_cc(cc[0], cc[1]);
            const int base = acts();
            const uint32_t t = open_and_settle(ui, kConnTabAlias + uint8_t(tab), 20000);
            CHECK_EQ(int(cp(ui).tab()), tab);
            ui.tick(t + 100);
            if (acts() != base) std::printf("active op: cc=%d/%d tab=%d ra=%d/%d scans=%d rescans=%d detach=%d\n", cc[0], cc[1], tab,
                                            g_stub.ra_ons, g_stub.ra_offs, g_stub.scans, g_stub.rescans, g_stub.cc_detaches);
            CHECK_EQ(acts(), base);
        }
    }
}

static void test_tab_switching() {
    Ui &ui = fresh_ui();
    ui.goto_page(kConnTabAlias, 20000);
    ui.tick(20050);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    ui.on_button(Button::Plus, ButtonEvent::Short, 20100);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    ui.on_button(Button::Plus, ButtonEvent::Short, 20150);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    ui.on_button(Button::Plus, ButtonEvent::Short, 20200);          // wraps
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    ui.on_button(Button::Minus, ButtonEvent::Short, 20250);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK_EQ(on_pd(ui), 1);
}

static void test_popup_per_tab() {
    Ui &ui = fresh_ui();
    ui.goto_page(kConnTabAlias, 20000);
    ui.tick(20050);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20100);
    CHECK_EQ(cp(ui).popup_open(), true);
    CHECK_EQ(cp(ui).popup_count(), 2);                              // Source: Refresh, Share (QR)
    CHECK_EQ(cp(ui).popup_item(0).enabled, true);                   // Refresh is always available
    CHECK_EQ(cp(ui).popup_item(1).enabled, false);                  // nothing to share yet
    ui.on_button(Button::Back, ButtonEvent::Short, 20200);          // BACK closes the pop-up, does not leave
    CHECK_EQ(cp(ui).popup_open(), false);
    CHECK_EQ(on_pd(ui), 1);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20300);          // Cable
    ui.on_button(Button::Ok, ButtonEvent::Short, 20350);
    CHECK_EQ(cp(ui).popup_count(), 5);                              // Read, Save, Spoof as..., Saved cables, Share
    ui.on_button(Button::Back, ButtonEvent::Short, 20400);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20450);          // Sink
    ui.on_button(Button::Ok, ButtonEvent::Short, 20500);
    CHECK_EQ(cp(ui).popup_count(), 1);                              // Identify
    CHECK_EQ(cp(ui).popup_item(0).enabled, false);                  // no device attached
    ui.on_button(Button::Back, ButtonEvent::Short, 20550);
    ui.on_button(Button::Back, ButtonEvent::Short, 20600);          // second BACK leaves Connection
    CHECK_EQ(on_pd(ui), 0);
}

static void test_spoof_greyed_while_a_device_is_inline() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    ui.goto_page(kConnTabAlias + 1, 20100);                         // Cable tab
    ui.tick(20150);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20200);
    int spoof = -1;
    for (int i = 0; i < cp(ui).popup_count(); i++)
        if (cp(ui).popup_item(i).act == conn::Act::SpoofAs) spoof = i;
    CHECK(spoof >= 0);
    CHECK_EQ(cp(ui).popup_item(spoof).enabled, false);
    CHECK_STR(cp(ui).popup_item(spoof).why, "Live sink: unplug load");
    const int depth = int(ui.menu_depth());
    for (int i = 0; i < spoof; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 20250 + i);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20400);            // refused: the Spoof list does not open
    CHECK_EQ(int(ui.menu_depth()), depth);
    CHECK(ui.menu() != Menu::Spoof);
}

static void test_spoof_enabled_when_nothing_is_inline() {
    Ui &ui = fresh_ui();
    ui.goto_page(kConnTabAlias + 1, 20100);
    ui.tick(20150);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20200);
    for (int i = 0; i < cp(ui).popup_count(); i++)
        if (cp(ui).popup_item(i).act == conn::Act::SpoofAs) CHECK_EQ(cp(ui).popup_item(i).enabled, true);
}

static void test_tools_spoof_shortcut_opens_the_cable_tab_list() {
    Ui &ui = fresh_ui();
    CHECK(ui.open_connection(conn::Tab::Cable, true, 20000));
    ui.tick(20050);
    CHECK(ui.menu() == Menu::Spoof);                                // the same list as Cable > Spoof as...
    CHECK_EQ(int(ui.menu_depth()), 2);
    ui.on_button(Button::Back, ButtonEvent::Short, 20100);
    ui.tick(20150);
    CHECK(ui.menu() == Menu::Connection);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
}

// ---- which side a VDM answer belongs to (the engine files them by the sender's power role) ----

static std::vector<uint16_t> frame() { return std::vector<uint16_t>(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT); }

// The tab as the user sees it after the page had a second to settle.
static std::vector<uint16_t> show(Ui &ui, uint8_t tab, uint32_t &t) {
    ui.goto_page(static_cast<uint8_t>(kConnTabAlias + tab), t += 100);
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    if (const char *dir = std::getenv("K2_DUMP")) {
        static int n = 0;
        char path[256];
        std::snprintf(path, sizeof path, "%s/k2_side%02d_tab%d.panel", dir, n++, tab);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
    return frame();
}

static void set_id(k2::pd::VdmInfo &v, uint16_t vid, uint16_t pid, const char *vendor) {
    v.identity.valid = true; v.identity.vid = vid; v.identity.pid = pid; v.identity.vendor = vendor;
    v.identity.id_header = 0x80000000u | vid;
}

// A charger and a phone behind the K2, as on the bench: the charger answered Discover Identity as the source, the phone
// answered the charger's own Discover Identity as the sink. The Source tab shows the charger, the Sink tab the phone, and
// neither tab moves when the other side's answer changes.
static void test_each_tab_shows_its_own_side() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 5000, 11000, 5000, 55000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;   // a Source_Capabilities arrived: the page is no longer waiting
    uint32_t t = 21000;
    ui.tick(t);
    set_id(g_stub.vdm, 0x2B01, 0x0000, "Unknown");
    set_id(g_stub.sink_vdm, 0x18D1, 0x4EE1, "Google");
    const auto src_a = show(ui, 0, t);
    const auto snk_a = show(ui, 2, t);
    CHECK(src_a != snk_a);

    set_id(g_stub.sink_vdm, 0x05AC, 0x1A2B, "Apple");   // another phone answers: only the Sink tab moves
    const auto src_b = show(ui, 0, t);
    const auto snk_b = show(ui, 2, t);
    CHECK(src_b == src_a);
    CHECK(snk_b != snk_a);

    set_id(g_stub.vdm, 0x05AC, 0x1A2B, "Apple");        // another charger answers: only the Source tab moves
    const auto src_c = show(ui, 0, t);
    const auto snk_c = show(ui, 2, t);
    CHECK(src_c != src_b);
    CHECK(snk_c == snk_b);

    // A vendor SVID is a protocol of the side that listed it, never a shared Alt entry.
    g_stub.vdm.num_svids = 1; g_stub.vdm.svids[0] = 0x2717;
    const auto src_d = show(ui, 0, t);
    const auto snk_d = show(ui, 2, t);
    CHECK(src_d != src_c);                              // the Source tab gains its Proto row
    CHECK(snk_d == snk_c);                              // the Sink tab does not
    g_stub.vdm.num_svids = 0;
    g_stub.sink_vdm.num_svids = 1; g_stub.sink_vdm.svids[0] = 0x2717;
    CHECK(show(ui, 0, t) == src_c);                     // the Source tab is back to what it was
    CHECK(show(ui, 2, t) != snk_c);
    g_stub.vdm = {};
    g_stub.sink_vdm = {};
}

// A sink that negotiated behind the K2 (a sniffed Request: the engine's contract) stays "seen" after the
// inline latch expires (2 s quiet, no current, no new Request): the Sink tab then shows the sink's rows,
// not the NO DEVICE empty state. The empty frame is captured on the same page first, nothing attached.
static void test_sink_tab_not_empty_when_a_sink_was_seen() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
    // A Request was sniffed on CC; set before the attach so the attach edge itself does not count it as
    // a "new" Request for the inline detector.
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 1; g_stub.contract.mv = 9000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.contract.requests = 1;
    uint32_t t = 20000;
    const auto empty = show(ui, 2, t);              // Sink tab, nothing attached: the empty state
    g_stub.cc = 1; g_stub.pm.current_ma = 2000;     // attach with a load behind the K2: inline latches
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    g_stub.pm.current_ma = 0;                       // the sink goes quiet: the latch expires after 2 s
    for (int i = 0; i < 30; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    const auto seen = show(ui, 2, t);               // same tab, a sink was seen on CC
    CHECK(seen != empty);                           // rows, not the empty state
    g_stub.contract = {};
}

// A non-PD source (attached, but Source_Capabilities never arrive): "waiting for caps" ends after
// pd::kWaitingCapsMs and the Source tab falls through to what is known instead of the single waiting row.
static void test_source_tab_leaves_waiting_after_timeout() {
    Ui &ui = fresh_ui();                    // detached: cc 0, VBUS 0
    g_stub.pm.vbus_mv = 5000;              // a plain 5 V port, no PD traffic at all
    g_stub.cc = 1;                         // attach: CC back with VBUS up; the caps counter never moves
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    CHECK_EQ(int(ui.link().waiting()), 1);
    const auto waiting_frame = show(ui, 0, t);        // Source tab: "Waiting for caps..."
    for (int i = 0; i < 55; i++) ui.tick(t += 100);    // 5.5 s with no caps
    CHECK_EQ(int(ui.link().waiting()), 0);             // the timeout ended the wait
    const auto after = show(ui, 0, t);
    CHECK(after != waiting_frame);                     // the tab now shows what is known
}

// Count the full-width hairlines of the Connection list area: one per row top plus the closing rule when
// the list is short (kRowY0 = kit::kBodyY + 18, pitch 20, nine rows max -- ui_connection_page.cpp).
static int body_hairlines() {
    const int y0 = kit::kBodyY + 18, pitch = 20;
    int n = 0;
    for (int y = y0; y < 222; y += pitch) {
        int rule = 0;
        for (int x = 0; x < LCD_WIDTH; x++) rule += g_panel[y * LCD_WIDTH + x] == kRule;
        if (rule >= LCD_WIDTH - 20) n++;   // a hairline spans the width (the panel edge pixels differ)
    }
    return n;
}

// The Proto row must survive the 9-row cap: when the Alt row would only show "-", it is dropped and the
// protocol name takes its slot. Ten rows are built here (ID, PD, Fixed, Fixed, PPS, PPS, Used, USB, Alt,
// Proto), so without the fix the Proto row spills and adding an SVID changes nothing on the panel.
static void test_proto_row_survives_the_row_cap() {
    Ui &ui = fresh_ui();
    ui.debug_link(true, 20000);            // attached, not waiting
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 5;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.pdos[3] = {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000};
    g_stub.caps.pdos[4] = {pd::PdoType::Pps, 0, 3300, 21000, 3250, 68250};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 5; g_stub.contract.mv = 21000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3250;
    set_id(g_stub.vdm, 0x2B01, 0x0000, "Unknown");
    uint32_t t = 21000;
    ui.tick(t);
    g_stub.vdm.num_svids = 0;
    const auto no_proto = show(ui, 0, t);
    g_stub.vdm.num_svids = 1; g_stub.vdm.svids[0] = 0x2717;   // a vendor protocol SVID: the Proto row appears
    const auto with_proto = show(ui, 0, t);
    CHECK(with_proto != no_proto);
    g_stub.vdm = {}; g_stub.contract = {}; g_stub.caps = {};
}

// The Data row only appears when it says something the USB row does not: a plain rate that would just
// repeat the USB row ("5G" with no cable read) is omitted; "5G of 10G" (the cable carries more) stays.
static void test_data_row_only_when_it_adds_something() {
    Ui &ui = fresh_ui();
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 1;
    g_stub.contract.mv = 5000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000; g_stub.contract.requests = 1;
    make_inline(ui, 30000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000; g_stub.ul.conf = usblink::Confidence::Negotiated;
    uint32_t t = 31000;
    ui.tick(t);
    show(ui, 2, t);                                   // Sink tab, no cable read: Data would be plain "5G"
    const int rows_plain = body_hairlines();
    g_stub.em.valid = true; g_stub.em.raw_vdos[4] = 2;  // a 10G cable: Data "5G of 10G" says something
    g_stub.em.max_power_w = 100;
    show(ui, 2, t);
    const int rows_cable = body_hairlines();
    CHECK_EQ(rows_cable, rows_plain + 1);             // the Data row appeared exactly there
    g_stub.em = {}; g_stub.contract = {}; g_stub.caps = {}; g_stub.ul = {};
}

// ---- every row fits the 19-cell value column, worst case on every tab (test_report fails on any cut) ----

static void test_worst_case_rows_fit() {
    Ui &ui = fresh_ui();
    make_inline(ui, 30000);
    g_stub.pm_online = true;
    // A charger with the full 11 PDOs, a PPS last, and a sink that asked for it at its most digits.
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 11;
    for (int i = 0; i < 10; i++) g_stub.caps.pdos[i] = {pd::PdoType::Fixed, 0, uint16_t(5000 + 1000 * i), uint16_t(5000 + 1000 * i), 3000, uint32_t(15000 + 3000 * i)};
    g_stub.caps.pdos[10] = {pd::PdoType::Pps, 0, 3300, 21120, 3250, 68640};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 11; g_stub.contract.mv = 21120; g_stub.contract.op_ma = 3250; g_stub.contract.max_ma = 3250;
    g_stub.contract.flags = pd::kRdoFGiveBack | pd::kRdoFMismatch | pd::kRdoFUsbComm | pd::kRdoFNoSuspend | pd::kRdoFEpr;
    // The ten-character vendor "ChargerLAB" on every side, an active TB cable that needs VCONN, a 5 G device with DP.
    set_id(g_stub.vdm, 0x2B01, 0x0000, "ChargerLAB");
    set_id(g_stub.sink_vdm, 0x2B01, 0x0000, "ChargerLAB");
    g_stub.vdm.num_svids = 3; g_stub.vdm.svids[0] = 0x2717; g_stub.vdm.svids[1] = 0x22D9; g_stub.vdm.svids[2] = 0x1D45;
    g_stub.sink_vdm = g_stub.vdm;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.is_active = true; em.vid = 0x2B01; em.pid = 0x0000; em.vendor_str = "ChargerLAB";
    em.speed_str = "USB 3.2 Gen2 (10G)"; em.latency_str = "50-60 ns (~6 m)";
    em.max_current_a = 5; em.max_voltage_v = 50; em.max_power_w = 250; em.epr_capable = true;
    em.vdo2_valid = true; em.usb_lanes = 2; em.usb4_supported = true; em.vconn_required = true; em.type_c_plug = true;
    g_stub.cable_tb = true;
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000; g_stub.ul.dp = true;
    g_stub.ul.conf = usblink::Confidence::Negotiated;
    uint32_t t = 31000;
    ui.tick(t);
    show(ui, 0, t); show(ui, 1, t); show(ui, 2, t);
    // The thin-cable case: a 3 A cable under a 100 W offer, sink unknown, draws nothing yet.
    em.max_current_a = 3; em.max_power_w = 60; em.vconn_required = false;
    g_stub.contract = {};
    g_stub.ul.mbps = 10000;
    show(ui, 1, t); show(ui, 2, t);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.vdm = {}; g_stub.sink_vdm = {}; g_stub.ul = {}; g_stub.cable_tb = false;
}

// ---- a live link repaints the rows that changed, not the whole tab ----

// A PPS phone behind the K2 re-requests every second or two (volts and amps move by a step): the Request and Pwr rows
// change, the rest of the tab must not be touched.
static void test_live_request_repaints_only_its_rows() {
    Ui &ui = fresh_ui();
    make_inline(ui, 40000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 5000, 11000, 5000, 55000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 3; g_stub.contract.mv = 8880; g_stub.contract.op_ma = 2050; g_stub.contract.max_ma = 2050;
    uint32_t t = 41000;
    ui.tick(t);
    for (uint8_t tab = 0; tab < 3; tab++) {
        show(ui, tab, t);
        // A steady link repaints nothing.
        const unsigned still0 = g_lcd_pixels;
        for (int i = 0; i < 10; i++) ui.tick(t += 100);
        CHECK_EQ(int(g_lcd_pixels - still0), 0);
        // The sink steps its request: only the changed rows repaint, well under the 45 k px of the body.
        g_stub.contract.mv += 60; g_stub.contract.op_ma += 50; g_stub.contract.max_ma += 50;
        const unsigned px0 = g_lcd_pixels;
        for (int i = 0; i < 10; i++) ui.tick(t += 100);
        const unsigned moved = g_lcd_pixels - px0;
        std::printf("[live-request] tab %d: %u px for one request step\n", tab, moved);
        CHECK(moved < 12000);   // at most a few 224 x 20 rows (4.5 k px each)
        // What is on the panel equals a whole-body repaint of the same data.
        const auto partial = frame();
        const auto full = show(ui, tab, t);
        CHECK(partial == full);
    }
    // A tab change repaints the whole body.
    const unsigned p0 = g_lcd_pixels;
    show(ui, 0, t);
    CHECK(g_lcd_pixels - p0 > 30000);
    g_stub.contract = {};
}

int main() {
    test_live_request_repaints_only_its_rows();
    test_worst_case_rows_fit();
    test_each_tab_shows_its_own_side();
    test_sink_tab_not_empty_when_a_sink_was_seen();
    test_source_tab_leaves_waiting_after_timeout();
    test_proto_row_survives_the_row_cap();
    test_data_row_only_when_it_adds_something();
    test_default_tab_on_open();
    test_topology_from_the_cc_lines();
    test_topology_unknown_unless_passive_and_dead();
    test_empty_states_are_passive();
    test_tab_switching();
    test_popup_per_tab();
    test_spoof_greyed_while_a_device_is_inline();
    test_spoof_enabled_when_nothing_is_inline();
    test_tools_spoof_shortcut_opens_the_cable_tab_list();
    return test_report("test_ui_connection_page");
}
