// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Connection (page level, real Ui): which tab it opens on, the passive port topology it senses from the CC
// lines, the tab switching and the action pop-up (greyed Spoof while a device is inline), the Tools shortcut
// to Spoof, and that merely showing an empty state never drives the lines.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/core/held.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
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
    while (ui.painting()) ui.tick(t += 2);
    return t;
}

// Every active CC / PD operation the stub counts (the link's own inline state is not one: it only reads).
static int acts() {
    return g_stub.ra_ons + g_stub.ra_offs + g_stub.rd_pauses + g_stub.scans + g_stub.cc_detaches + g_stub.rescans +
           g_stub.force_line + g_stub.identifies + g_stub.getcaps + g_stub.requests + g_stub.probes + g_stub.hard_resets +
           g_stub.new_sources + g_stub.clear_sources + g_stub.unit_detaches + g_stub.unit_attaches + g_stub.releases +
           g_stub.rd_releases + g_stub.rd_run_changes;
}

static void test_place_aliases_pick_tab() {
    Ui &ui = fresh_ui();
    CHECK(ui.goto_page(kConnTabAlias + 2, 20000));
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK(ui.goto_page(kConnTabAlias + 1, 20100));
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    CHECK(ui.goto_page(kConnTabAlias, 20200));
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
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

// The remote session block reads the same classifier without the page: Ui::passive_facts() is the one
// implementation the page paints from (conn::sense_passive), so with the page open and settled both agree on the
// topology, the waiting classifier and the standby evidence, and the call works with no page on screen.
static void test_passive_facts_are_the_pages_facts() {
    struct Case { int cc1, cc2, vbus; };
    const Case cases[] = {{3300, 3300, 0}, {3300, 0, 0}, {0, 3300, 0}, {0, 0, 0}, {3300, 3300, 5000}};
    for (const Case &c : cases) {
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = c.vbus;
        set_cc(c.cc1, c.cc2);
        for (int i = 0; i < 12; i++) ui.tick(g_stub.ms += 100);   // the analog snapshot lands with no page open
        const conn::Facts off = ui.passive_facts();               // no Connection page on screen
        open_and_settle(ui, kConnTabAlias, 20000);
        const conn::Facts on = cp(ui).facts();
        CHECK_EQ(int(off.topo), int(on.topo));
        CHECK_EQ(int(off.wait), int(on.wait));
        CHECK_EQ(int(off.cross), int(on.cross));
        CHECK_EQ(int(off.open_cc_mv), int(on.open_cc_mv));
        CHECK_EQ(int(off.charger), int(on.charger));
        CHECK_EQ(int(off.vbus_mv), int(on.vbus_mv));
    }
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 0;
    set_cc(3300, 3300);
    for (int i = 0; i < 12; i++) ui.tick(g_stub.ms += 100);
    CHECK_EQ(int(ui.passive_facts().topo), int(raguard::Topology::DirectReceptacle));
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

static void test_place_items_per_tab() {
    Ui &ui = fresh_ui();
    ui.goto_page(kConnTabAlias, 20000);                              // Charger Spec
    ui.tick(20050);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    CHECK_EQ(cp(ui).item_count(), 2 + bench::actions(bench::Place::Charger).n);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));    // row 0: pinned Reset & re-read (includes Read again)
    CHECK_EQ(cp(ui).item(0).enabled, true);
    CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::Details));
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Bench));        // Test charger
    for (int i = 0; i < cp(ui).item_count(); i++) {
        CHECK(cp(ui).item(i).act != conn::Act::Refresh);
        CHECK(std::strcmp(cp(ui).item(i).label, "Request voltage") != 0);
        CHECK(std::strcmp(cp(ui).item(i).label, "Fast charge") != 0);
    }
    ui.goto_page(kConnTabAlias + 1, 20300);                          // Cable Spec
    ui.tick(20350);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    const int cb_n = bench::actions(bench::Place::Cable).n;
    CHECK_EQ(cp(ui).item_count(), 2 + cb_n + 3);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));
    CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::ReadCable));
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Bench));        // no Chip details row: Read e-marker opens the sheet
    CHECK_EQ(int(cp(ui).item(2 + cb_n + 2).act), int(conn::Act::SpoofAs));
    ui.goto_page(kConnTabAlias + 2, 20450);                          // Device Spec
    ui.tick(20500);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK(cp(ui).item_count() >= 3);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));
    CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::Details));
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Identify));     // Ask device
    CHECK_EQ(cp(ui).item(2).enabled, false);                         // no device attached
    ui.on_button(Button::Back, ButtonEvent::Short, 20600);           // BACK leaves Connection
    CHECK_EQ(on_pd(ui), 0);
}

static void test_cable_place_has_spoof_row() {
    const int cb_n = bench::actions(bench::Place::Cable).n;
    const int save_i = 2 + cb_n;
    for (bool inl : {false, true}) {
        Ui &ui = fresh_ui();
        if (inl) make_inline(ui, 20000);
        ui.goto_page(kConnTabAlias + 1, 20100);                      // Cable Spec
        ui.tick(20150);
        CHECK_EQ(cp(ui).item_count(), save_i + 3);
        CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));
        CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::ReadCable));
        CHECK_EQ(int(cp(ui).item(save_i).act), int(conn::Act::SaveCable));
        CHECK_EQ(int(cp(ui).item(save_i + 1).act), int(conn::Act::SavedCables));
        CHECK_EQ(int(cp(ui).item(save_i + 2).act), int(conn::Act::SpoofAs));
        CHECK_EQ(cp(ui).item(save_i + 2).enabled, !inl);
        CHECK_STR(cp(ui).item(save_i + 2).tag, inl ? "load" : "");
    }
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
    CHECK_EQ(cp(ui).actions_mode(), true);
}

// ---- which side a VDM answer belongs to (the engine files them by the sender's power role) ----

static std::vector<uint16_t> frame() { return std::vector<uint16_t>(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT); }

// The tab as the user sees it after the page had a second to settle.
static std::vector<uint16_t> show(Ui &ui, uint8_t tab, uint32_t &t) {
    ui.goto_page(static_cast<uint8_t>(kConnTabAlias + tab), t += 100);
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
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
    g_stub.pd_seen_attach = true;                  // caps are PD messages (the driver sets this on any decode)
    g_stub.ol_facts.far_pd_message = true;         // the phone answered the charger's own Discover Identity
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
    uint32_t t = 20000;
    const auto empty = show(ui, 2, t);              // Sink tab, nothing attached, no sink seen: the empty state
    // A Request was sniffed on CC; set before the attach so the attach edge itself does not count it as
    // a "new" Request for the inline detector.
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 1; g_stub.contract.mv = 9000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.contract.requests = 1;
    g_stub.pd_seen_attach = true;                  // the sniffed Request is a decoded PD message
    g_stub.ol_facts.far_pd_message = true;         // ... sent by the sink: it answered
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    g_stub.cc = 1; g_stub.pm.current_ma = 2000;     // attach with a load behind the K2: inline latches
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    g_stub.pm.current_ma = 0;                       // the sink goes quiet: the latch expires after the quiet window
    for (uint32_t i = 0; i < k2::pd::kInlineQuietMs / 100 + 10; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    const auto seen = show(ui, 2, t);               // same tab, a sink was seen on CC
    CHECK(seen != empty);                           // rows, not the empty state
    g_stub.contract = {};
}

// A non-PD source (attached, but Source_Capabilities never arrive): the "PD present" verdict stays Pending
// for the settle window (the Source tab still shows "Waiting for caps..."), settles to None at attach+1.5 s
// and the tab then says there is no PD (ROADMAP 2026-11-10, Row C); the caps wait itself still ends after
// pd::kWaitingCapsMs.
static void test_source_tab_leaves_waiting_after_timeout() {
    Ui &ui = fresh_ui();                    // detached: cc 0, VBUS 0
    g_stub.pm.vbus_mv = 5000;              // a plain 5 V port, no PD traffic at all
    g_stub.cc = 1;                         // attach: CC back with VBUS up; the caps counter never moves
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    CHECK_EQ(int(ui.link().waiting()), 1);
    ui.goto_page(kConnTabAlias, t += 100);
    for (int i = 0; i < 8; i++) ui.tick(t += 100);     // t = 21200: still inside the 1.5 s settle window
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::Pending));
    const auto waiting_frame = frame();                // Source tab: "Waiting for caps..."
    for (int i = 0; i < 6; i++) ui.tick(t += 100);     // past attach+1500: the verdict settles
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::None));
    CHECK_EQ(int(ui.link().pd_none_why()), int(pd::PdNoneWhy::SourceNeverOffered));
    const auto after = frame();
    CHECK(after != waiting_frame);                     // the tab now says there is no PD
    CHECK_EQ(int(ui.link().waiting()), 1);             // the caps wait itself is unchanged
    for (int i = 0; i < 45; i++) ui.tick(t += 100);    // 5.5 s with no caps
    CHECK_EQ(int(ui.link().waiting()), 0);             // the timeout ended the wait
}

// Place-page list geometry (Option B 208x240, x offset +16 for left OppRail in rot 0): rows from kit::kPlaceListY (46), pitch 21, value at logical x 52 (panel x 68).
static bool place_row_value_is(int idx, const char *text, uint16_t colour = kText) {
    constexpr int kValX = kOppRailThickness + 52, kPitch = 21, kRowY0 = kit::kPlaceListY;
    constexpr int W = 204 - 52;
    const int y = kRowY0 + idx * kPitch;
    const int vy = y + kit::row_text_y(kPitch, 16);
    uint16_t saved[W * (kPitch - 1)];
    for (int dy = 1; dy < kPitch; dy++)
        for (int x = 0; x < W; x++) saved[(dy - 1) * W + x] = g_panel[(y + dy) * LCD_WIDTH + kValX + x];
    const Surface s{0, 0, LCD_WIDTH, LCD_HEIGHT};
    gfx_fill(s, kValX, y + 1, W, kPitch - 1, kBg);
    gfx_text(s, kSmall, kValX, vy, text, colour, kBg);
    bool same = true;
    for (int dy = 1; dy < kPitch && same; dy++)
        for (int x = 0; x < W; x++)
            if (g_panel[(y + dy) * LCD_WIDTH + kValX + x] != saved[(dy - 1) * W + x]) { same = false; break; }
    for (int dy = 1; dy < kPitch; dy++)
        for (int x = 0; x < W; x++) g_panel[(y + dy) * LCD_WIDTH + kValX + x] = saved[(dy - 1) * W + x];
    return same;
}

// A row whose value is two runs: the volts, then the tail (a current, or the watts of an AVS) in its own colour.
static bool place_row_split_is(int idx, const char *head, const char *tail, uint16_t hc, uint16_t tc) {
    constexpr int kValX = kOppRailThickness + 52, kPitch = 21, kRowY0 = kit::kPlaceListY;
    constexpr int W = 204 - 52;
    const int y = kRowY0 + idx * kPitch;
    const int vy = y + kit::row_text_y(kPitch, 16);
    uint16_t saved[W * (kPitch - 1)];
    for (int dy = 1; dy < kPitch; dy++)
        for (int x = 0; x < W; x++) saved[(dy - 1) * W + x] = g_panel[(y + dy) * LCD_WIDTH + kValX + x];
    const Surface s{0, 0, LCD_WIDTH, LCD_HEIGHT};
    gfx_fill(s, kValX, y + 1, W, kPitch - 1, kBg);
    gfx_text(s, kSmall, kValX, vy, head, hc, kBg);
    gfx_text(s, kSmall, kValX + int(std::strlen(head)) * kSmall.adv, vy, tail, tc, kBg);
    bool same = true;
    for (int dy = 1; dy < kPitch && same; dy++)
        for (int x = 0; x < W; x++)
            if (g_panel[(y + dy) * LCD_WIDTH + kValX + x] != saved[(dy - 1) * W + x]) { same = false; break; }
    for (int dy = 1; dy < kPitch; dy++)
        for (int x = 0; x < W; x++) g_panel[(y + dy) * LCD_WIDTH + kValX + x] = saved[(dy - 1) * W + x];
    return same;
}

static bool nopd_row_value_is(int idx, const char *text, uint16_t colour = kText) {
    return place_row_value_is(idx, text, colour);
}

// A source without PD still advertises its Rp on the active CC line: once the no-PD verdict has settled
// (ROADMAP 2026-11-10, Row C), the Source tab shows the no-PD message plus a Type-C row whenever an Rp is
// there -- a default-USB Rp (CC 0.41 V) included -- with the measured VBUS in the row (never a hardcoded 5 V).
static void test_source_tab_typec_advertisement() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 5000;               // a plain 5 V port, no PD traffic at all
    g_stub.cc = 1;                          // attach: CC back with VBUS up; the caps counter never moves
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    for (int i = 0; i < 16; i++) ui.tick(t += 100);    // past the 1.5 s settle: the verdict is None
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::None));
    set_cc(408, 1);                         // a default-USB Rp on the active line
    ui.tick(t += 100);
    show(ui, 0, t);
    CHECK_EQ(cp(ui).result_rows(), 1);                  // the Type-C row
    CHECK(nopd_row_value_is(0, "5V USB default"));
    // A 3 A Rp while VBUS sits at 20.2 V: the same row, now showing the measured voltage (and the
    // longest wording still fits the 19-cell column -- the trunc check at the end of main).
    set_cc(1678, 3300);
    g_stub.pm.vbus_mv = 20250;
    ui.tick(t += 100);
    show(ui, 0, t);
    CHECK_EQ(cp(ui).result_rows(), 1);
    CHECK(nopd_row_value_is(0, "20.2V 3A"));
    // Default-USB Rp at 20.2 V: the longest possible Type-C row ("20.2V USB default", 17 cells).
    set_cc(408, 1);
    ui.tick(t += 100);
    show(ui, 0, t);
    CHECK_EQ(cp(ui).result_rows(), 1);
    CHECK(nopd_row_value_is(0, "20.2V USB default"));
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);   // every Type-C row above fit the 19-cell column
#endif
    g_stub.pm.vbus_mv = 5000;
}

// Dock -> K2 -> laptop with no PD traffic of its own (hardware 2026-11-09). A sink's Rd on the far side no longer says anything
// (user decision 2026-11-15: only a load or a sink's Request latches); once the laptop draws, the Link calls it inline, the
// Source tab's "PD none" row says the line is in use, and the Sink tab is not the NO DEVICE empty state.
static void test_source_and_sink_tabs_say_in_use_when_possibly_inline() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 5000;                          // 5 V alone is a standalone K2; a load is what says "inline"
    g_stub.cc = 1;
    g_stub.pd_seen_attach = true;                      // a decoded PD message: the verdict is Present, the rows body stays
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    for (int i = 0; i < 55; i++) ui.tick(t += 100);   // the waiting-for-caps window is over
    set_cc(1676, 3303);
    ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    const auto src_alone = show(ui, 0, t);
    const auto snk_alone = show(ui, 2, t);
    ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);         // the sink-band level alone latches nothing
    CHECK(show(ui, 0, t) == src_alone);
    g_stub.pm.current_ma = 400;                        // the laptop draws: certain evidence
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    CHECK(show(ui, 0, t) != src_alone);                // "PD none, line in use"
    CHECK(show(ui, 2, t) != snk_alone);                // the Sink tab leaves the empty state
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.pm.current_ma = 0;
    g_stub.pm.vbus_mv = 5000;
}

// The Legacy row of the no-PD Source tab lists D+/D- fast-charge protocols only: a PD-family verdict
// (Pd, PdPe, PdMtk, PdLikely) never gets a row there (ROADMAP 2026-11-10, Row C: the no-PD body keeps the
// Type-C row and adds Legacy when D+/D- answered).
static void test_source_tab_legacy_row_skips_pd_verdicts() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 5000;
    g_stub.cc = 1;
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    for (int i = 0; i < 16; i++) ui.tick(t += 100);    // past the settle: the verdict is None
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::None));
    set_cc(1678, 3300);
    g_stub.fc = fastcharge::Protocol::Qc2_5V;           // a D+/D- protocol: the Legacy row appears
    ui.tick(t += 100);
    show(ui, 0, t);
    const int rows_qc = cp(ui).result_rows();           // Type-C, Legacy
    CHECK_EQ(rows_qc, 2);
    CHECK(nopd_row_value_is(1, "QC2", kEnergy));                 // fc_pill_text for Qc2_5V
    for (fastcharge::Protocol p : {fastcharge::Protocol::Pd, fastcharge::Protocol::PdPe,
                                   fastcharge::Protocol::PdMtk, fastcharge::Protocol::PdLikely}) {
        g_stub.fc = p;                                  // a PD verdict: no Legacy row
        ui.tick(t += 100);
        show(ui, 0, t);
        CHECK_EQ(cp(ui).result_rows(), rows_qc - 1);
    }
    g_stub.fc = fastcharge::Protocol::Pending;
}

// The Sink tab omits the Data row when the data line says nothing (the bug-9 rule): no "Data -"
// placeholder. Rows here: ID, Request, State, Pwr, USB, Caps -- six fact rows.
static void test_sink_tab_omits_empty_data_row() {
    Ui &ui = fresh_ui();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 1;
    g_stub.contract.mv = 5000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // the Request is PD traffic, answered by the sink
    uint32_t t = 20000;
    show(ui, 2, t);                                     // Sink tab, a sink was seen on CC
    CHECK_EQ(cp(ui).result_rows(), 6);                  // no Data row: nothing is known about the data line
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000;
    g_stub.ul.conf = usblink::Confidence::Negotiated;
    ui.tick(t += 100);
    show(ui, 2, t);
    CHECK_EQ(cp(ui).result_rows(), 6);                  // a plain "5G" would only repeat the USB row
    g_stub.ul = {}; g_stub.contract = {}; g_stub.pd_seen_attach = false; g_stub.ol_facts.far_pd_message = false;
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
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // the Request is PD traffic, answered by the sink
    make_inline(ui, 30000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000; g_stub.ul.conf = usblink::Confidence::Negotiated;
    uint32_t t = 31000;
    ui.tick(t);
    show(ui, 2, t);                                   // Sink tab, no cable read: Data would be plain "5G"
    const int rows_plain = cp(ui).result_rows();
    g_stub.em.valid = true; g_stub.em.raw_vdos[4] = 2;  // a 10G cable: Data "5G of 10G" says something
    g_stub.em.max_power_w = 100;
    show(ui, 2, t);
    const int rows_cable = cp(ui).result_rows();
    CHECK_EQ(rows_cable, rows_plain + 1);             // the Data row appeared exactly there
    g_stub.em = {}; g_stub.contract = {}; g_stub.caps = {}; g_stub.ul = {};
}

// The signatures cover displayed inputs only: a change that never reaches the screen (the Request
// counter, the VDM request counter) must not even run match_live, and a displayed change must run it
// exactly once. No pixel change either way.
static void test_non_displayed_inputs_do_not_run_match_live() {
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 5000, 11000, 5000, 55000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 3; g_stub.contract.mv = 8880; g_stub.contract.op_ma = g_stub.contract.max_ma = 2050;
    uint32_t t = 51000;
    ui.tick(t);
    show(ui, 0, t);
    for (int i = 0; i < 10; i++) ui.tick(t += 100);          // settled
    const uint32_t base = g_match_live_calls;
    const unsigned px0 = g_lcd_pixels;
    g_stub.contract.requests += 5;                            // counters, never rendered
    g_stub.vdm.requests += 3;
    for (int i = 0; i < 10; i++) ui.tick(t += 100);
    CHECK_EQ(g_match_live_calls - base, 0u);                  // the cheap signature did not even fire
    CHECK_EQ(int(g_lcd_pixels - px0), 0);
    g_stub.contract.mv += 60;                                 // a displayed input: exactly one run
    const uint32_t c0 = g_match_live_calls;
    ui.tick(t += 100);
    CHECK_EQ(g_match_live_calls - c0, 1u);
    g_stub.contract = {};
}

// True when the value column of list row `idx` shows exactly `text`: the band the page painted is saved,
// the candidate is drawn the same way the row draws it (kSmall at kValX, kText on kBg) and compared pixel
// for pixel, then the page's band is restored. The row geometry is ui_connection_page.cpp's (kRowY0 =
// kit::kBodyY + 18, pitch 20, value at x 64).
static bool row_value_is(int idx, const char *text, uint16_t ink = kText) {
    return place_row_value_is(idx, text, ink);
}

// A plain 5 V source without PD (only its Rp advertises the current) plus a sink seen on CC: the Sink
// tab's Pwr row is the Type-C default budget, labelled as a budget ("up to 2.5W" for a 500 mA Rp,
// "up to 15W" for 3 A) -- never a bare "2W"/"15W" that reads as a measurement. With a decoded PD
// contract the row is unchanged ("18W of 100W").
static void test_sink_tab_pwr_row_is_a_budget_without_pd() {
    {   // default-USB Rp (CC 0.41 V -> 500 mA): "up to 2.5W"
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 5000;
        g_stub.cc = 1;                        // attach: CC back with VBUS up, no PD traffic at all
        g_stub.contract.valid = true;         // a sink was seen on CC: the Sink tab shows rows, not the empty state
        g_stub.pd_seen_attach = true;         // the sniffed Request is a decoded PD message...
        g_stub.ol_facts.far_pd_message = true;  // ... sent by the sink: the verdict is Present, the rows body stays
        uint32_t t = 20000;
        for (int i = 0; i < 3; i++) ui.tick(t += 100);
        CHECK_EQ(int(ui.link().attached()), 1);
        set_cc(408, 1);                       // the Rp advertisement on the active line: 500 mA
        ui.tick(t += 100);
        show(ui, 2, t);                       // Sink tab
        CHECK_EQ(cp(ui).result_rows(), 5);    // ID, Request, Pwr, USB, Caps
        CHECK(row_value_is(2, "up to 2.5W"));
        CHECK(!row_value_is(2, "2W"));        // the old integer-math reading is gone
        g_stub.contract = {};
    }
    {   // 3 A Rp (CC 1.68 V): "up to 15W"
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 5000;
        g_stub.cc = 1;
        g_stub.contract.valid = true;
        g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;
        uint32_t t = 20000;
        for (int i = 0; i < 3; i++) ui.tick(t += 100);
        CHECK_EQ(int(ui.link().attached()), 1);
        set_cc(1678, 1);
        ui.tick(t += 100);
        show(ui, 2, t);
        CHECK_EQ(cp(ui).result_rows(), 5);
        CHECK(row_value_is(2, "up to 15W"));
        CHECK(!row_value_is(2, "15W"));
        g_stub.contract = {};
    }
    {   // a decoded PD contract: the Pwr row is unchanged
        Ui &ui = fresh_ui();
        g_stub.pm.vbus_mv = 5000; g_stub.cc = 1;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1;
        g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
        g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;   // a Source_Capabilities arrived
        g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
        g_stub.contract.pos = 1; g_stub.contract.mv = 9000; g_stub.contract.op_ma = g_stub.contract.max_ma = 2000;
        uint32_t t = 20000;
        ui.tick(t);
        show(ui, 2, t);                       // Sink tab
        CHECK_EQ(cp(ui).result_rows(), 6);    // ID, Request, State, Pwr, USB, Caps
        CHECK(row_value_is(3, "18W of 100W"));
        CHECK(!row_value_is(3, "up to 15W"));
        g_stub.caps = {}; g_stub.contract = {};
    }
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);   // every Pwr row above fit the 19-cell value column whole
#endif
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
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // without these the Source and Sink tabs read "NO PD"
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 11; g_stub.contract.mv = 21120; g_stub.contract.op_ma = 3250; g_stub.contract.max_ma = 3250;
    g_stub.contract.flags = pd::kRdoFGiveBack | pd::kRdoFMismatch | pd::kRdoFUsbComm | pd::kRdoFNoSuspend | pd::kRdoFEpr;
    // The widest real vendor name (14 characters, in capitals) on every side, an active TB cable that needs VCONN, a 5 G device with DP.
    set_id(g_stub.vdm, 0x0456, 0x0000, "ANALOG DEVICES");
    set_id(g_stub.sink_vdm, 0x0456, 0x0000, "ANALOG DEVICES");
    g_stub.vdm.num_svids = 3; g_stub.vdm.svids[0] = 0x2717; g_stub.vdm.svids[1] = 0x22D9; g_stub.vdm.svids[2] = 0x1D45;
    g_stub.sink_vdm = g_stub.vdm;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.is_active = true; em.vid = 0x2B01; em.pid = 0x0000; em.vendor_str = "ANALOG DEVICES";
    em.speed_str = "USB 3.2 Gen2 (10G)"; em.latency_str = "50-60 ns (~6 m)";
    em.max_current_a = 5; em.max_voltage_v = 50; em.max_power_w = 250; em.epr_capable = true;
    em.vdo2_valid = true; em.usb_lanes = 2; em.usb4_supported = true; em.vconn_required = true; em.type_c_plug = true;
    g_stub.cable_tb = true;
    g_stub.cable_ans = emk_variant::answered_from(2, 1);   // "B, PD3, SVDM 1.0": the ninth row (VCONN required) of the Cable tab
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000; g_stub.ul.dp = true;
    g_stub.ul.conf = usblink::Confidence::Negotiated;
    uint32_t t = 31000;
    ui.tick(t);
    show(ui, 0, t); show(ui, 1, t); show(ui, 2, t);
    // The longest name a table row can take (16 characters) must still draw inside every band.
    set_id(g_stub.vdm, 0x2B01, 0x0000, "SIXTEEN CHARS OK");
    set_id(g_stub.sink_vdm, 0x2B01, 0x0000, "SIXTEEN CHARS OK");
    em.vendor_str = "SIXTEEN CHARS OK";
    show(ui, 0, t); show(ui, 1, t); show(ui, 2, t);
    // The thin-cable case: a 3 A cable under a 100 W offer, sink unknown, draws nothing yet.
    em.max_current_a = 3; em.max_power_w = 60; em.vconn_required = false;
    g_stub.contract = {};
    g_stub.ul.mbps = 10000;
    show(ui, 1, t); show(ui, 2, t);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.vdm = {}; g_stub.sink_vdm = {}; g_stub.ul = {}; g_stub.cable_tb = false; g_stub.cable_ans = {};
}

// The Cable tab's last row names how the cable answered the read, in the short form with commas (the font has no
// middle dot): "B, PD3, SVDM 1.0" for the Baseus cable, "A, PD3, SVDM 2.0" for the CUKTECH one. No answer (a clone,
// an older reading) = no row.
static void test_cable_tab_answered_row() {
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 2.0"; em.latency_str = "<10 ns (~1 m)";
    em.max_current_a = 5; em.max_voltage_v = 20; em.max_power_w = 100;
    uint32_t t = 51000;
    ui.tick(t);
    show(ui, 1, t);
    const int rows_without = cp(ui).result_rows();   // ID, Speed, Current, Voltage, EPR, Type, Length
    CHECK_EQ(rows_without, 7);
    g_stub.cable_ans = emk_variant::answered_from(2, 1); g_stub.cable_seq++;
    show(ui, 1, t);
    CHECK_EQ(cp(ui).result_rows(), rows_without + 1);
    auto tick_paint = [&]() { for (int i = 0; i < 12; i++) ui.tick(t += 100); while (ui.painting()) ui.tick(t += 2); };
    // Scroll down to see the 8th fact row (Answered) on the 7-row viewport.
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // unfocus action row -> first_ = 0
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // scroll down 6 rows -> row 7 at slot 1
    tick_paint();
    CHECK(row_value_is(7 - cp(ui).window_first(), "B, PD3, SVDM 1.0", kDim));
    g_stub.cable_ans = emk_variant::answered_from(1, 0); g_stub.cable_seq++;
    tick_paint();
    CHECK(row_value_is(7 - cp(ui).window_first(), "A, PD3, SVDM 2.0", kDim));
    g_stub.cable_ans = emk_variant::answered_from(1, 2); g_stub.cable_seq++;
    tick_paint();
    CHECK(row_value_is(7 - cp(ui).window_first(), "A, PD2, SVDM 1.0", kDim));
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.cable_ans = {};
}

// The Cable tab after a full read: an active, optical Thunderbolt cable with every SVID answered, and a SOP'' plug. The
// list is longer than the window, so the walk scrolls to the end; every row painted must fit (test_report fails on a cut).
static void test_cable_full_rows() {
    Ui &ui = fresh_ui();
    make_inline(ui, 52000);
    g_stub.pm_online = true;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.is_active = true; em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 3.2 Gen2 (10G)";
    em.latency_str = "<10 ns (~1 m)"; em.max_current_a = 5; em.max_voltage_v = 50; em.max_power_w = 250; em.epr_capable = true;
    em.vdo_version = 3; em.vdo2_valid = true; em.usb_lanes = 2; em.usb_gen2 = true; em.usb4_supported = true;
    em.optical_isolated = true; em.type_c_plug = true;
    em.max_temp_c = 70; em.shutdown_temp_c = 85; em.u3_power = 3;
    em.raw_vdos[5] = 70u << 24 | 85u << 16 | 3u << 12 | 1u << 10 | 1u << 9 | 2u << 6 | 1u << 1;   // optical retimer, 2 hubs
    cable_full::Full &f = g_stub.cable_full;
    f.valid = true;
    f.p1.svids_ack = true; f.p1.nsvid = 4; f.p1.mode_ack = 0xF;
    f.p1.svid = {0x8087, 0xFF01, 0xFF02, 0x1D45};
    f.p1.mode[0] = 3u << 16 | 1u << 19 | 1u << 21 | 1u << 22 | 1u << 23 | 1u << 25 | 1u;   // 10/20G, rounded, optical, retimer, uni
    f.p1.mode[1] = 3u << 2 | 2u << 28 | 0x3Fu << 8;                                        // UHBR20, re-driver, pins A..F
    f.p1.mode[2] = 0x0000AB12u; f.p1.mode[3] = 0xFFFFFFFFu;
    f.id2_ack = true; f.id2_n = 5; f.id2[0] = 0x2E99; f.id2[2] = 0x1234u << 16;
    f.p2.svids_ack = true; f.p2.nsvid = 2; f.p2.mode_ack = 0x3; f.p2.svid = {0xFF02, 0x1D45, 0, 0};
    f.p2.mode[0] = 0x0000AB12u; f.p2.mode[1] = 0xFFFFFFFFu;
    g_stub.cable_seq++;
    uint32_t t = 53000;
    ui.tick(t);
    show(ui, 1, t);
    // ID, Speed, Current, Voltage, EPR, Type, Length (7), Temp, U3 pwr, Build, USB, Lanes (12), SVIDs, TBT3, TBT ext, DP,
    // DP pins, Mode, Mode (19), Plug 2, SVIDs 2, Mode 2, Mode 2 (23)
    CHECK_EQ(cp(ui).result_rows(), 23);
    static const char *const want[] = {"max 70C off 85C", "0.5-1mW", "Optical retimer iso", "USB2 USB3 USB4", "2 lanes Gen2+ hub 2",
                                       "8087 FF01 FF02 1D45", "10/20G active rnd", "optical retimer uni", "UHBR20 redriver",
                                       "A B C D E F", "FF02 0000AB12", "1D45 FFFFFFFF", "2E99:1234", "FF02 1D45", "FF02 0000AB12"};
    bool seen[sizeof want / sizeof *want] = {};
    auto look = [&]() {
        for (std::size_t k = 0; k < sizeof want / sizeof *want; k++)
            for (int i = 0; i < 9; i++)
                if (row_value_is(i, want[k])) seen[k] = true;
    };
    auto tick_paint = [&]() { for (int i = 0; i < 12; i++) ui.tick(t += 100); while (ui.painting()) ui.tick(t += 2); };
    auto dump = [&](int n) {   // K2_DUMP: the windows as panel files (tools/panel2png.py)
        if (const char *dir = std::getenv("K2_DUMP")) {
            char path[256];
            std::snprintf(path, sizeof path, "%s/k2_cable_full_%02d.panel", dir, n);
            if (FILE *fp = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, fp); std::fclose(fp); }
        }
    };
    look();
    dump(0);
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // unfocus the action row, then page down to the end
    for (int p = 0, last = -1; p < 40 && cp(ui).window_first() != last; p++) {   // one row per press
        last = cp(ui).window_first();
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
        tick_paint();
        look();
        if (p % 8 == 7) dump(1 + p / 8);
    }
    dump(9);
    for (std::size_t k = 0; k < sizeof want / sizeof *want; k++) {
        if (!seen[k]) std::printf("not seen: %s\n", want[k]);
        CHECK(seen[k]);
    }
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.cable_full = {};
}

// The Charger tab keeps the high-water-mark offers after the charger goes quiet (VBUS off, Rd dropped) or throttles
// to a lower power offer (marking offer_throttled), and replaces them only for a higher/equal-power list after 800 ms.
static void test_charger_tab_keeps_the_offers() {
    Ui &ui = fresh_ui();
    make_inline(ui, 56000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Pps, 0, 5000, 11000, 5000, 55000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    uint32_t t = 57000;
    ui.tick(t);
    show(ui, 0, t);
    const int rows = cp(ui).result_rows();
    CHECK(rows >= 3);
    CHECK_EQ(held::shown_caps().num_pdos, 3);
    CHECK_EQ(held::offer_throttled(), false);
    const pd::SourceCaps three = g_stub.caps;
    g_stub.caps = {};                                  // the driver's list is gone: the charger sleeps
    g_stub.pm.vbus_mv = 0;
    show(ui, 0, t);
    CHECK_EQ(cp(ui).result_rows(), rows);              // the same rows, not the empty band
    g_stub.caps = three;                               // the same offers again, a new message
    g_stub.caps.raw_header = 0x1381; g_stub.caps.rx_count += 2;
    g_stub.pm.vbus_mv = 5000;
    show(ui, 0, t);
    CHECK_EQ(cp(ui).result_rows(), rows);
    g_stub.caps.num_pdos = 2;                          // throttle to lower power (27 W < 55 W): high-water-mark keeps 55 W
    g_stub.caps.raw_header = 0x1581; g_stub.caps.rx_count++;
    show(ui, 0, t);
    CHECK_EQ(held::shown_caps().num_pdos, 3);
    CHECK_EQ(held::offer_throttled(), true);
    CHECK_EQ(held::live_offer_mw(), 27000u);
    g_stub.caps = three;                               // upgrade to higher power (100 W > 55 W): replaces held after 800 ms
    g_stub.caps.num_pdos = 4;
    g_stub.caps.pdos[3] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.raw_header = 0x1781; g_stub.caps.rx_count++;
    show(ui, 0, t);
    CHECK_EQ(held::shown_caps().num_pdos, 4);
    CHECK_EQ(held::offer_throttled(), false);
    g_stub.caps = {};
    g_stub.pm.vbus_mv = 0;
    show(ui, 0, t);
    CHECK(held::showing_held());
    CHECK_EQ(held::shown_caps().num_pdos, 4);          // the new high-water-mark list is held now
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.caps = {};
}

// The Charger and Cable pages with nothing held, as a charger waits on the line and as nothing is plugged: all four rotations
// (K2_DUMP writes the panels).
static void test_empty_pages_say_what_fills_them() {
    const struct { const char *name; int cc1, cc2; } scenes[] = {{"idle", 0, 0}, {"waiting", 3300, 3300}};
    for (const auto &sc : scenes)
        for (uint8_t rot = 0; rot < 4; rot++) {
            Ui &ui = fresh_ui();
            ui.set_rotation(rot, g_stub.ms);
            set_cc(sc.cc1, sc.cc2);
            uint32_t t = 13000;
            for (int i = 0; i < 12; i++) ui.tick(t += 100);
            for (uint8_t tab = 0; tab < 2; tab++) {
                show(ui, tab, t);
                CHECK_EQ(cp(ui).result_rows(), 0);                          // the identity block alone, no fact rows
                if (const char *dir = std::getenv("K2_DUMP")) {
                    char path[256];
                    std::snprintf(path, sizeof path, "%s/k2_empty_%s_%s_rot%u.panel", dir, sc.name, tab ? "cable" : "charger", unsigned(rot));
                    if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
                }
            }
#ifdef K2_TRUNC_LOG
            CHECK_EQ(k2::trunc::failures(), 0);
#endif
        }
}

// A read cable stays on the Cable tab after the driver forgets it (detach, a read starting), in full colour, until a read
// returns a different cable; the same cable read again without SVIDs keeps the SVIDs it had.
static void test_cable_tab_keeps_a_read_cable() {
    Ui &ui = fresh_ui();
    make_inline(ui, 54000);
    g_stub.pm_online = true;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 2.0"; em.latency_str = "<10 ns (~1 m)";
    em.max_current_a = 5; em.max_voltage_v = 20; em.max_power_w = 100; em.raw_vdos[3] = 0x11;
    cable_full::Full &f = g_stub.cable_full;
    f.valid = true; f.p1.svids_ack = true; f.p1.nsvid = 1; f.p1.svid[0] = 0xFF01;
    uint32_t t = 55000;
    ui.tick(t);
    show(ui, 1, t);
    const int with_svids = cp(ui).result_rows();
    CHECK(with_svids > 7);
    CHECK(row_value_is(1, "2.0"));
    g_stub.em = {}; g_stub.cable_full = {}; g_stub.cable_ans = {}; g_stub.cable_seq++;   // unplugged: the driver's copy is gone
    show(ui, 1, t);
    CHECK_EQ(cp(ui).result_rows(), with_svids);
    CHECK(row_value_is(1, "2.0"));                    // not dimmed
    em.valid = true; em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 2.0"; em.latency_str = "<10 ns (~1 m)";
    em.max_current_a = 5; em.max_voltage_v = 20; em.max_power_w = 100; em.raw_vdos[3] = 0x11;
    g_stub.cable_seq++;                               // the same cable read again: identity only
    show(ui, 1, t);
    CHECK_EQ(cp(ui).result_rows(), with_svids);
    em.raw_vdos[3] = 0x12;                            // another cable: the old SVIDs are not its own
    g_stub.cable_seq++;
    show(ui, 1, t);
    CHECK_EQ(cp(ui).result_rows(), 7);
    g_stub.em = {}; g_stub.cable_seq++;
    show(ui, 1, t);
    CHECK_EQ(cp(ui).result_rows(), 7);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.cable_full = {};
}

// ---- a live link repaints the rows that changed, not the whole tab ----

// A PPS phone behind the K2 re-requests every second or two (volts and amps move by a step): the Request and Pwr rows
// change, the rest of the tab must not be touched.
static void test_live_request_repaints_only_its_rows() {
    Ui &ui = fresh_ui();
    make_inline(ui, 40000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // caps + a sink Request: the verdict is Present
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
        CHECK(moved < 20000);   // identity block + changed row, well under a full repaint
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

// Refresh goes through the rescan module (a live contract asks for the caps instead of cycling CC; the cycle and its marker are the
// module's business, tests/test_rescan_bridge.cpp), and a refused one (a negotiation in flight) is said on the page.
static void test_refresh_goes_through_the_rescan_module() {
    Ui &ui = fresh_ui();
    ui.goto_page(kMenuPageBase + uint8_t(Menu::Connection), 20000);
    g_stub.rescan_result = rescan::Result::Asked;
    CHECK_EQ(ui.run_action(Action::PdRescan, 20100), true);
    CHECK_EQ(g_stub.rescan_requests, 1);
    CHECK_EQ(g_stub.rescans, 0);                 // no CC cycle behind a Get_Source_Cap
    g_stub.rescan_result = rescan::Result::Cycled;
    CHECK_EQ(ui.run_action(Action::PdRescan, 20200), true);
    CHECK_EQ(g_stub.rescan_requests, 2);
    g_stub.rescan_result = rescan::Result::Busy;
    CHECK_EQ(ui.run_action(Action::PdRescan, 20300), false);   // a negotiation in flight: nothing happened, ask again
    CHECK_EQ(g_stub.rescan_requests, 3);
}

// A source that holds VBUS off (standby signature on the open line, no Rd of ours): the Charger overview wakes it once.
static void asleep_port(Ui &ui) {
    g_stub.pm.vbus_mv = 0;
    set_cc(3300, 0);
    g_stub.open_cc_mv[0] = 2950;
    g_stub.rescan_result = rescan::Result::Cycled;
    (void)ui;
}

// The Who's attached overview on its Charger tab, the way the ring opens it (the page aliases open the Spec sheet).
static uint32_t open_overview(Ui &ui, uint32_t t) {
    ui.open_place(bench::Place::Charger, t);
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    return t;
}

static uint32_t run_ticks(Ui &ui, uint32_t t, int n) {
    for (int i = 0; i < n; i++) ui.tick(t += 100);
    return t;
}

static void test_auto_wake_once_on_the_charger_overview() {
    Ui &ui = fresh_ui();
    asleep_port(ui);
    uint32_t t = open_overview(ui, 20000);
    CHECK_EQ(int(cp(ui).view()), int(ConnectionPage::View::Overview));
    CHECK_EQ(g_stub.rescan_requests, 1);                        // the Refresh path, by itself
    CHECK_EQ(g_stub.survey_refreshes, 1);
    g_stub.open_cc_mv[0] = -1;                                  // our own cycle moved the line ...
    t = run_ticks(ui, t, 10);
    g_stub.open_cc_mv[0] = 2950;                                // ... and it fell asleep again: still the same episode
    t = run_ticks(ui, t, 100);                                  // 10 s of a charger that stays silent: no second try
    CHECK_EQ(g_stub.rescan_requests, 1);
    t = run_ticks(ui, t, 60);
    g_stub.open_cc_mv[0] = -1;                                  // a replug after the retry gap is a new episode
    t = run_ticks(ui, t, 10);
    g_stub.open_cc_mv[0] = 2950;
    t = run_ticks(ui, t, 300);
    CHECK_EQ(g_stub.rescan_requests, 2);
}

// A page opened on a sleeping port waits out the 600 ms sense hold, then says what it does.
static void test_auto_wake_settles_then_toasts() {
    Ui &ui = fresh_ui();
    asleep_port(ui);
    uint32_t t = 20000;
    ui.open_place(bench::Place::Charger, t);
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(g_stub.rescan_requests, 0);
    for (int i = 0; i < 8 && g_stub.rescan_requests == 0; i++) ui.tick(t += 100);
    CHECK_EQ(g_stub.rescan_requests, 1);
    CHECK(ui.context().toast.active(t));
    CHECK_STR(ui.context().toast.view().data(), "Waking charger");
}

// The direct-plug line fallback moves our Rd on its own: no second wake over it.
static void test_auto_wake_waits_for_the_line_trial() {
    Ui &ui = fresh_ui();
    asleep_port(ui);
    g_stub.trial_live = true;
    g_stub.trial_line = 1;
    uint32_t t = open_overview(ui, 20000);
    t = run_ticks(ui, t, 30);
    CHECK_EQ(g_stub.rescan_requests, 0);
    g_stub.trial_live = false;
    t = run_ticks(ui, t, 5);
    CHECK_EQ(g_stub.rescan_requests, 1);
}

static void test_auto_wake_not_on_other_tabs_or_views() {
    {
        Ui &ui = fresh_ui();
        asleep_port(ui);
        open_and_settle(ui, kConnTabAlias + 1, 20000);          // the Cable tab's fact sheet
        CHECK_EQ(g_stub.rescan_requests, 0);
        open_and_settle(ui, kConnTabAlias, 22000);              // the Charger's fact sheet (Spec view)
        CHECK_EQ(g_stub.rescan_requests, 0);
    }
    {
        Ui &ui = fresh_ui();
        asleep_port(ui);
        g_stub.ra_on = true;                                    // blocked while the page opens, so nothing fires yet
        uint32_t t = open_overview(ui, 20000);
        CHECK_EQ(g_stub.rescan_requests, 0);
        for (int i = 0; i < 4 && cp(ui).view() == ConnectionPage::View::Overview; i++) ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
        CHECK(cp(ui).view() != ConnectionPage::View::Overview);
        g_stub.ra_on = false;
        run_ticks(ui, t, 20);
        CHECK_EQ(g_stub.rescan_requests, 0);                    // Spec / Actions: no wake behind the user's back
    }
}

static void test_auto_wake_honours_the_auto_read_block() {
    Ui &ui = fresh_ui();
    asleep_port(ui);
    g_stub.ra_on = true;
    uint32_t t = open_overview(ui, 20000);
    t = run_ticks(ui, t, 50);
    CHECK_EQ(g_stub.rescan_requests, 0);
    g_stub.ra_on = false;                                       // the block clears: not tried yet, so it goes once
    t = run_ticks(ui, t, 5);
    CHECK_EQ(g_stub.rescan_requests, 1);
}

static void test_auto_wake_needs_the_live_facts_too() {
    Ui &ui = fresh_ui();
    asleep_port(ui);
    g_stub.ra_on = true;
    uint32_t t = open_overview(ui, 20000);
    CHECK_EQ(g_stub.rescan_requests, 0);
    g_stub.ra_on = false;                                       // the block clears on the tick the line is no longer asleep:
    g_stub.pm.vbus_mv = 5000;                                   // the debounced sense still says asleep for 600 ms
    ui.tick(t += 100);
    CHECK_EQ(g_stub.rescan_requests, 0);
}

// The comeback of a bridged Refresh lands on Connection -> Source, whatever page the boot opened.
static void test_the_rescan_comeback_lands_on_the_source_tab() {
    Ui &ui = fresh_ui();
    ui.goto_page(uint8_t(Page::Home), 20000);
    CHECK_EQ(ui.menu() == Menu::Connection, false);
    CHECK_EQ(ui.land_rescanned(20100), true);
    CHECK_EQ(ui.menu() == Menu::Connection, true);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    ui.open_connection(conn::Tab::Cable, false, 20150);   // from another tab: back on Source
    CHECK_EQ(ui.land_rescanned(20200), true);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
}

// ---- the survey on the page: nothing asked until Refresh, a hint, progress, a summary, a Details page ----

static const pd::SvCtx kSurveyCtx{.contract = true, .inline_sink = false, .charger_test = false, .xiaomi_chain = false, .pd20 = false, .epr_capable = true};

// A PD charger behind a plain attach (no load, so not inline): the Source tab has rows.
static void attach_charger(Ui &ui, uint32_t &t) {
    g_stub.cc = 1; g_stub.pm.vbus_mv = 9000; g_stub.pm.current_ma = 0; g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;   // the caps themselves are PD messages
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
}

static void settle(Ui &ui, uint32_t &t) {
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
}

// Opening the place pages never asks the charger anything: only Refresh does.
static void test_survey_nothing_active_before_refresh() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    const int base = acts();
    show(ui, 0, t); show(ui, 1, t); show(ui, 2, t); show(ui, 0, t);
    settle(ui, t);
    CHECK_EQ(acts(), base);
    CHECK_EQ(g_stub.survey_refreshes, 0);
}

// Standalone Reset & re-read (row 0 on Charger [Actions], with Read again merged in) starts the survey.
static void test_survey_refresh_item_starts_it() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    g_stub.pm.vbus_mv = 5000;                                 // standalone 5 V charger (no external >5.5 V contract)
    ui.tick(t += 100);
    show(ui, 0, t);
    g_stub.rescan_result = rescan::Result::Asked;
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);   // Spec -> Actions (row 0 = Reset & re-read)
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);   // activate Reset & re-read
    CHECK_EQ(g_stub.survey_refreshes, 1);
    CHECK_EQ(int(g_stub.survey_last), int(rescan::Result::Asked));
}

// The status line: live survey state at rest and "Reading charger r/p" during an active survey run.
static void test_survey_status_line() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    show(ui, 0, t);
    settle(ui, t);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Not asked: Reset & re-read");
    show(ui, 1, t);
    show(ui, 0, t);
    g_stub.survey.start(kSurveyCtx, t);
    settle(ui, t);
    char want[40];
    std::snprintf(want, sizeof want, "Reading charger %u/%u", g_stub.survey.resolved(), g_stub.survey.planned());
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), want);
    g_stub.survey.set(pd::SvField::Caps, pd::SvOutcome::Answered);
    settle(ui, t);
    std::snprintf(want, sizeof want, "Reading charger %u/%u", g_stub.survey.resolved(), g_stub.survey.planned());
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), want);
    g_stub.survey.abandon();
    settle(ui, t);
    CHECK(g_stub.survey.ran());
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Read: 1 got, 7 no answer");
}

// The attach's auto read asks only while nothing else holds the link: a tool, the Ra mode or a legacy handshake each block it.
static void test_auto_read_gate() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    CHECK_EQ(int(ui.auto_read_block()), int(pd::SvWhy::Inline));   // a foreign contract on the wire: not the K2's to ask
    g_stub.sink.state = pd::SinkState::Ready;                      // the K2 sent the Request
    CHECK_EQ(int(ui.auto_read_block()), int(pd::SvWhy::None));
    g_stub.ra_on = true;
    CHECK_EQ(int(ui.auto_read_block()), int(pd::SvWhy::ChargerTest));
    g_stub.ra_on = false;
    g_stub.legacy_active = true;
    CHECK_EQ(int(ui.auto_read_block()), int(pd::SvWhy::ChargerTest));
    g_stub.legacy_active = false;
    CHECK_EQ(int(ui.auto_read_block()), int(pd::SvWhy::None));
}

// The Charger place's survey rows say what each field is and show the answers once they are in.
static void test_survey_details_rows_follow_the_survey() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    show(ui, 0, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);       // scroll down to survey rows (Caps, Mfr, Ext...)
    settle(ui, t);
    const auto unasked = frame();
    g_stub.survey.start(kSurveyCtx, t);
    g_stub.survey.abandon();                                        // everything planned is now "no answer"
    settle(ui, t);
    const auto silent = frame();
    CHECK(silent != unasked);
    g_stub.ext.mfr.valid = true; g_stub.ext.mfr.vid = 0x1234; g_stub.ext.mfr.pid = 0x5678;
    std::strcpy(g_stub.ext.mfr.name, "Fake Charger");
    g_stub.survey.start(kSurveyCtx, t);
    g_stub.survey.set(pd::SvField::MfrInfo, pd::SvOutcome::Answered);
    g_stub.survey.abandon();
    settle(ui, t);
    CHECK(frame() != silent);                                       // the manufacturer row shows its name
}

// The Sink tab's ID row marks an identity that was only heard on the wire: an inline K2 never asks
// (its Identify is refused), so the phone's answer to the charger's own Discover Identity is shown
// as sniffed. Only the ID row may differ between the two renders: no sniffed sink caps and no
// Sink_Cap step, so the wants list is empty either way.
static void test_sink_id_row_marks_a_sniffed_identity() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // caps + the phone's VDM answer: Present
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    uint32_t t = 21000;
    ui.tick(t);
    set_id(g_stub.sink_vdm, 0x05C6, 0x209E, "OnePlus");
    g_stub.ident_current = false;                       // nothing was asked this attach: sniffed
    show(ui, 2, t);
    while (cp(ui).action_row() >= 0 || cp(ui).window_first() > 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // scroll to top so ID row is visible
    settle(ui, t);
    const auto sniffed = frame();
    g_stub.ident_current = true;                        // a same-session Identify collected it
    g_stub.ident.ok_mask = 0;                           // no Sink_Cap step: the wants list stays empty
    settle(ui, t);
    const auto asked = frame();
    CHECK(sniffed != asked);                            // the ID row differs once the mark is in
    g_stub.sink_vdm = {};
}

// A device ALONE on the K2 (a dual-role phone: Rp toggling on CC2, VBUS dark, no source caps) is offered the
// active Identify on the Sink tab (ROADMAP "Sink Identify inline drops the phone", fix item (2)). Red first:
// the item was enabled only when a charger or a sink was attached.
static void test_sink_tab_offers_identify_when_a_device_is_alone() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;                        // the scan attached on the phone's Rp
    g_stub.pm.vbus_mv = 0;                 // dark: the phone sources nothing while the K2 is passive
    g_stub.caps.valid = false; g_stub.caps_this_attach = false;
    for (uint32_t t = 12000; t <= 13400; t += 100) {   // the dual-role toggle: Rp on CC2 every other 100 ms
        set_cc(0, (t / 100) % 2 == 0 ? 3300 : 0);
        ui.tick(t);
    }
    CHECK_EQ(int(ui.link().wait_kind()), int(raguard::WaitKind::DualRolePort));
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    ui.goto_page(kConnTabAlias + 2, 13500);                  // Device Spec
    ui.tick(13550);
    CHECK(cp(ui).item_count() >= 3);
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Identify));
    CHECK_EQ(cp(ui).item(2).enabled, true);                  // Ask device offered: a device is alone on the K2
}

// Row B item 6 (red): the [UI] identify line keeps its fixed phrases in the format string. Today they ride a %s,
// which the device binlog cuts at 12 bytes ("[UI] identify refused: inl", "(device alo").
static void test_identify_log_keeps_fixed_text_in_the_format() {
    {   // inline: refused with the whole reason in the format, no %s
        Ui &ui = fresh_ui();
        make_inline(ui, 20000);
        g_stub.log_fmts.clear();
        CHECK_EQ(ui.run_action(Action::Identify, 20100), false);
        CHECK_EQ(g_stub.log_fmts.find("[UI] identify refused: inline (no TX; the Sink tab shows the sniffed identity)\n") != std::string::npos, true);
    }
    {   // a device alone on the K2: started, the alone note in the format
        Ui &ui = fresh_ui();
        g_stub.cc = 1; g_stub.pm_online = true; g_stub.pm.vbus_mv = 0;
        g_stub.caps.valid = false; g_stub.caps_this_attach = false;
        for (uint32_t t = 12000; t <= 13400; t += 100) {
            set_cc(0, (t / 100) % 2 == 0 ? 3300 : 0);
            ui.tick(t);
        }
        CHECK_EQ(int(ui.link().wait_kind()), int(raguard::WaitKind::DualRolePort));
        g_stub.log_fmts.clear();
        CHECK_EQ(ui.run_action(Action::Identify, 13500), true);
        CHECK_EQ(g_stub.log_fmts.find("[UI] identify started (device alone on the K2)\n") != std::string::npos, true);
    }
    {   // a plain attach: started, no suffix - and a refusal with no inline note when the driver refuses
        Ui &ui = fresh_ui();
        g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000; g_stub.cc = 1;
        g_stub.caps.valid = true; g_stub.caps_this_attach = true;
        ui.tick(12000);
        g_stub.log_fmts.clear();
        CHECK_EQ(ui.run_action(Action::Identify, 12100), true);
        CHECK_EQ(g_stub.log_fmts.find("[UI] identify started\n") != std::string::npos, true);
        g_stub.identify_ok = false;
        g_stub.log_fmts.clear();
        CHECK_EQ(ui.run_action(Action::Identify, 12200), false);
        CHECK_EQ(g_stub.log_fmts.find("[UI] identify refused\n") != std::string::npos, true);
    }
}

// ---- ROADMAP 2026-11-10, Row C: the no-PD gate on the Connection page --------------------------------------

static void dump_panel(const char *name) {
    if (const char *dir = std::getenv("K2_DUMP")) {
        char path[256];
        std::snprintf(path, sizeof path, "%s/%s.panel", dir, name);
        if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
    }
}

// Ink pixels in a band (rotation 0: logical == physical).
static int ink_in(int y0, int y1) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < 224; x++) {
            const uint16_t p = g_panel[y * LCD_WIDTH + x];
            if (p != kBg && p != kRule) n++;
        }
    return n;
}

// Pixel -> K2 -> SSD with the source's PD offers unanswered: attach with a small load behind the K2, the
// caps counter climbs past the attach edge (repeated Source_Capabilities), the sink never answers
// (far_pd_message false). After the settle window the verdict is None / SinkSilentSourceOffered and the
// Sink tab shows the no-PD body: one message band, then measured rows from y 108 (not the placeholder list
// from y 42), and the pop-up's Identify is greyed with the tab's own words.
static void test_sink_tab_no_pd_offered_unanswered() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 5030;
    g_stub.pm.current_ma = 70;
    g_stub.caps.valid = true;
    g_stub.caps.rx_count = 100;
    g_stub.pd_seen_attach = true;          // the caps themselves are PD messages
    uint32_t t = 20000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);         // attach + the inline latch (the load)
    CHECK_EQ(int(ui.link().attached()), 1);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    for (int i = 0; i < 4; i++) { g_stub.caps.rx_count++; ui.tick(t += 100); }   // 4 caps this attach, no answer
    CHECK(ui.link().caps_rx_this_attach() >= 3);
    for (int i = 0; i < 14; i++) ui.tick(t += 100);        // past attach + 1500 ms: the settle
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::None));
    CHECK_EQ(int(ui.link().pd_none_why()), int(pd::PdNoneWhy::SinkSilentSourceOffered));
    ui.goto_page(kConnTabAlias + 2, t += 100);             // Sink tab
    settle(ui, t);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK_EQ(int(cp(ui).pd_verdict()), int(pd::PdVerdict::None));
    CHECK_EQ(int(cp(ui).pd_none_why()), int(pd::PdNoneWhy::SinkSilentSourceOffered));
    dump_panel("k2_nopd_sink_offered");
    // The no-PD body (rotation 0): identity block + row list from kit::kPlaceListY (68).
    CHECK(ink_in(22, 67) > 100);
    CHECK_EQ(cp(ui).result_rows(), 2);
    CHECK(ink_in(kit::kPlaceListY + 1, kit::kPlaceListY + 40) > 50);
    // The place's Ask device item (row 2) is greyed with the same words the page shows.
    CHECK(cp(ui).item_count() >= 3);
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Identify));
    CHECK_EQ(cp(ui).item(2).enabled, false);
    CHECK_STR(cp(ui).item(2).why, "No PD from this device");
}

// A plain Type-C source (Rp only, never offered PD) with a load behind the K2: after the settle the verdict
// is None / SourceNeverOffered and the Source tab shows the no-PD body in every rotation (the copy wraps at
// 26 cells; test_ui_no_truncation / the trunc log fail on any cut).
static void test_source_tab_no_pd_never_offered_all_rotations() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 5030;
    g_stub.pm.current_ma = 70;
    g_stub.pd_seen_attach = false;         // nothing decoded this attach
    g_stub.caps.valid = false;
    g_stub.typec_ma_val = 1500;            // the source's advertised Rp: default USB
    uint32_t t = 30000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    for (int i = 0; i < 16; i++) ui.tick(t += 100);        // past the settle
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::None));
    CHECK_EQ(int(ui.link().pd_none_why()), int(pd::PdNoneWhy::SourceNeverOffered));
    for (int r = 0; r < 4; r++) {
        ui.set_rotation(r, t += 100);
        ui.goto_page(kConnTabAlias, t += 100);             // Source tab
        settle(ui, t);
        CHECK_EQ(int(cp(ui).pd_verdict()), int(pd::PdVerdict::None));
        CHECK_EQ(int(cp(ui).pd_none_why()), int(pd::PdNoneWhy::SourceNeverOffered));
        char name[64];
        std::snprintf(name, sizeof name, "k2_nopd_source_never_rot%d", r);
        dump_panel(name);
        if (r == 0) {
            CHECK(ink_in(22, 67) > 100);
            CHECK_EQ(cp(ui).result_rows(), 1);
            CHECK(ink_in(kit::kPlaceListY + 1, kit::kPlaceListY + 40) > 50);
        }
    }
    ui.set_rotation(0, t += 100);
}

// The attach's auto read as the Source tab shows it, in four rotations: the hint before it, the progress while it runs, the
// summary after it, and the rows an abort blocked. Panels go to K2_DUMP (k2_autoread_<state>_rot<N>.panel); the trunc log fails on a cut.
static void test_auto_read_states_all_rotations() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    show(ui, 0, t);
    settle(ui, t);
    for (int r = 0; r < 4; r++) {
        ui.set_rotation(r, t += 100);
        char name[64];
        g_stub.survey = {};
        settle(ui, t);
        std::snprintf(name, sizeof name, "k2_autoread_hint_rot%d", r);
        dump_panel(name);
        g_stub.survey.start_auto(kSurveyCtx, t);
        settle(ui, t);
        std::snprintf(name, sizeof name, "k2_autoread_running_rot%d", r);
        dump_panel(name);
        CHECK(std::string(cp(ui).status_line()).rfind("Reading charger ", 0) == 0);
        g_stub.survey.abandon();
        settle(ui, t);
        std::snprintf(name, sizeof name, "k2_autoread_done_rot%d", r);
        dump_panel(name);
        g_stub.survey = {};
        g_stub.survey.start_auto(kSurveyCtx, t);
        g_stub.survey.block_open(pd::SvWhy::Inline);
        settle(ui, t);
        std::snprintf(name, sizeof name, "k2_autoread_blocked_rot%d", r);
        dump_panel(name);
    }
    ui.set_rotation(0, t += 100);
    g_stub.survey = {};
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// The widest table name (ANALOG DEVICES, 14) on the charger and the sink, and a 16-capital cable maker string, every tab in all
// four rotations: panels go to K2_DUMP (k2_vendor16_t<tab>_rot<N>.panel) and the trunc log fails on any cut.
static void test_widest_vendor_all_rotations() {
    Ui &ui = fresh_ui();
    make_inline(ui, 30000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 11;
    for (int i = 0; i < 10; i++) g_stub.caps.pdos[i] = {pd::PdoType::Fixed, 0, uint16_t(5000 + 1000 * i), uint16_t(5000 + 1000 * i), 3000, uint32_t(15000 + 3000 * i)};
    g_stub.caps.pdos[10] = {pd::PdoType::Pps, 0, 3300, 21120, 3250, 68640};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.accepted = true;
    g_stub.contract.pos = 11; g_stub.contract.mv = 21120; g_stub.contract.op_ma = 3250; g_stub.contract.max_ma = 3250;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // a PD charger and a PD sink, so all three tabs have rows
    set_id(g_stub.vdm, 0x0456, 0x0000, "ANALOG DEVICES");   // the widest name in the table (14); the 16 limit goes through the cable head
    g_stub.vdm.num_svids = 3; g_stub.vdm.svids[0] = 0x2717; g_stub.vdm.svids[1] = 0x22D9; g_stub.vdm.svids[2] = 0x1D45;
    g_stub.sink_vdm = g_stub.vdm;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.is_active = true; em.vid = 0x2B01; em.pid = 0x0000; em.vendor_str = "SIXTEEN CHARS OK";
    em.speed_str = "USB 3.2 Gen2 (10G)"; em.latency_str = "50-60 ns (~6 m)";
    em.max_current_a = 5; em.max_voltage_v = 50; em.max_power_w = 250; em.epr_capable = true;
    em.vdo2_valid = true; em.usb_lanes = 2; em.usb4_supported = true; em.vconn_required = true; em.type_c_plug = true;
    g_stub.cable_tb = true;
    uint32_t t = 40000;
    ui.tick(t);
    for (int r = 0; r < 4; r++) {
        ui.set_rotation(r, t += 100);
        for (uint8_t tab = 0; tab < 3; tab++) {
            show(ui, tab, t);
            char name[64];
            std::snprintf(name, sizeof name, "k2_vendor16_t%u_rot%d", unsigned(tab), r);
            dump_panel(name);
        }
    }
    ui.set_rotation(0, t += 100);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.vdm = {}; g_stub.sink_vdm = {}; g_stub.caps = {}; g_stub.contract = {}; g_stub.cable_tb = false;
    g_stub.pd_seen_attach = false; g_stub.ol_facts.far_pd_message = false;
}

// A standalone device read (the Sink tab's headline is the vendor when it fits the head) with the widest table name and a 16
// character name from the device itself: all four rotations, nothing cut. Panels: k2_vendorid_<14|16>_rot<N>.panel.
static void test_widest_vendor_identify_all_rotations() {
    for (const char *name : {"ANALOG DEVICES", "SIXTEEN CHARS OK"}) {
        Ui &ui = fresh_ui();
        g_stub.cc = 1; g_stub.pm_online = true; g_stub.pm.vbus_mv = 0;
        g_stub.caps.valid = false; g_stub.caps_this_attach = false;
        uint32_t t = 12000;
        for (; t <= 13400; t += 100) { set_cc(0, (t / 100) % 2 == 0 ? 3300 : 0); ui.tick(t); }
        g_stub.ident_current = true;
        g_stub.ident.state = pd::IdState::Done;
        g_stub.ident.ok_mask = uint16_t((1u << int(pd::IdStep::Identity)) | (1u << int(pd::IdStep::SinkCap)) | (1u << int(pd::IdStep::Revision)));
        g_stub.ident_identity.valid = true; g_stub.ident_identity.vid = 0x0456; g_stub.ident_identity.pid = 0x0001;
        g_stub.ident_identity.vendor = name;
        g_stub.ident.revision = 0x31200000u;
        for (int r = 0; r < 4; r++) {
            ui.set_rotation(r, t += 100);
            ui.goto_page(kConnTabAlias + 2, t += 100);
            settle(ui, t);
            char dump[64];
            std::snprintf(dump, sizeof dump, "k2_vendorid_%zu_rot%d", std::strlen(name), r);
            dump_panel(dump);
        }
        ui.set_rotation(0, t += 100);
#ifdef K2_TRUNC_LOG
        CHECK_EQ(k2::trunc::failures(), 0);
#endif
        g_stub.ident_current = false; g_stub.ident_identity = {}; g_stub.ident = {};
    }
}

// The same inline setup but the sink answered (a GoodCRC / Request this attach): the verdict is Present, the
// Sink tab keeps its rows and the pop-up's Identify stays offered - the no-PD gate does not touch a PD link.
static void test_pd_present_keeps_identify_offered() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 5030;
    g_stub.pm.current_ma = 70;
    g_stub.caps.valid = true;
    g_stub.caps.rx_count = 50;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;                 // the sink answered this attach
    uint32_t t = 40000;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().pd_verdict()), int(pd::PdVerdict::Present));
    ui.goto_page(kConnTabAlias + 2, t += 100);
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    CHECK_EQ(int(cp(ui).pd_verdict()), int(pd::PdVerdict::Present));
    dump_panel("k2_pd_present_sink");
    CHECK_EQ(cp(ui).item(0).enabled, true);                // Ask device stays offered
}

// The inline VOOC monitor sees a live session behind the K2 (a phone that never talks PD): the Sink tab's no-PD body
// gets a third measured row, the raw `VOOC 0x82 0x06B raw` note (y 200..219), and loses it when the session ends.
static void test_sink_tab_no_pd_shows_the_raw_vooc_note() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 5030;
    g_stub.pm.current_ma = 70;
    g_stub.caps.valid = true;
    g_stub.caps.rx_count = 100;
    g_stub.pd_seen_attach = true;
    uint32_t t = 50000;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    for (int i = 0; i < 4; i++) { g_stub.caps.rx_count++; ui.tick(t += 100); }
    for (int i = 0; i < 14; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);
    settle(ui, t);
    CHECK_EQ(int(cp(ui).pd_verdict()), int(pd::PdVerdict::None));
    CHECK_EQ(cp(ui).result_rows(), 2);                       // two rows only: Type-C, Draw
    g_stub.vooc.seen = true;
    g_stub.vooc.r82 = 0x6B;
    for (int i = 0; i < 6; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    dump_panel("k2_nopd_sink_vooc");
    CHECK_EQ(cp(ui).result_rows(), 3);                       // the VOOC note
    CHECK(nopd_row_value_is(2, "0x82 0x06B raw", kDim));
    g_stub.vooc = {};
    for (int i = 0; i < 6; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    CHECK_EQ(cp(ui).result_rows(), 2);
    g_stub.vooc = {};
}

static void test_sink_tab_pd_body_shows_the_raw_vooc_note() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1;
    g_stub.pm.vbus_mv = 5030;
    g_stub.pm.current_ma = 70;
    g_stub.caps.valid = true;
    g_stub.caps.rx_count = 50;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;                 // the sink answered: a PD link, not the no-PD body
    g_stub.vooc = {};
    uint32_t t = 60000;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);
    settle(ui, t);
    CHECK_EQ(int(cp(ui).pd_verdict()), int(pd::PdVerdict::Present));
    const int before = cp(ui).result_rows();
    CHECK(before >= 3 && before <= 8);                     // room for one more row
    g_stub.vooc.seen = true;
    g_stub.vooc.r82 = 0x6F;
    for (int i = 0; i < 6; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    dump_panel("k2_pd_sink_vooc");
    CHECK_EQ(cp(ui).result_rows(), before + 1);            // the VOOC row
    g_stub.vooc = {};
    for (int i = 0; i < 6; i++) ui.tick(t += 100);
    while (ui.painting()) ui.tick(t += 2);
    CHECK_EQ(cp(ui).result_rows(), before);                // the session ended: the row is gone at once
    g_stub.vooc = {};
}

// The place pages (Fieldbook): Charger, Cable and Device are this page locked to one tab, opened over the Bench. No tab strip,
// Plus/Minus never switch the tab; a place comes back on its last row, Up past the first row lets go of the focus (OK then
// runs the place's own read), BACK pops back to the Bench with the place focused.
static const char *ok_label(Ui &ui) { return ui.labels().b[static_cast<int>(Button::Ok)].shrt; }

static void test_single_tools_lock_one_tab() {
    Ui &ui = fresh_ui();
    ui.open_place(bench::Place::Cable, 20000);
    ui.tick(20050);
    CHECK_EQ(int(ui.menu()), int(Menu::Connection));
    CHECK_EQ(int(ui.page()), int(Page::Lines));                       // the Link ring page, entered
    CHECK_EQ(ui.ring_view(), false);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    CHECK_EQ(cp(ui).overview_mode(), true);                           // opens on 3-slice Overview focused on Cable
    CHECK_EQ(cp(ui).actions_mode(), false);
    CHECK_EQ(cp(ui).action_row(), -1);
    CHECK_STR(ok_label(ui), "Open");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Back)].shrt, "Back");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Back)].lng, "");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Minus)].shrt, "Up");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Minus)].lng, "");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Plus)].shrt, "Dn");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Plus)].lng, "");
    CHECK_EQ(label_violation(ui.labels()) == nullptr, true);
    ui.on_button(Button::Plus, ButtonEvent::Long, 20075);              // Long Plus does nothing (hardware Rd safety)
    CHECK_EQ(cp(ui).overview_mode(), true);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20080);               // Short OK -> Cable Actions sub-page
    ui.tick(20090);
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Back)].shrt, "Back");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Back)].lng, "");
    CHECK_EQ(cp(ui).action_row(), 0);                                  // row 0: Reset & re-read
    CHECK_STR(ok_label(ui), "Reset");
    ui.on_button(Button::Plus, ButtonEvent::Short, 20100);             // Dn: Read e-marker
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));                // never a tab switch
    CHECK_EQ(cp(ui).action_row(), 1);
    CHECK_STR(ok_label(ui), "Read");
    for (int i = 0; i < 10; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 20150 + i * 10);
    CHECK_EQ(cp(ui).action_row(), cp(ui).act_count() - 1);             // stops on the last Cable action row (Spoof as...)
    CHECK_STR(ok_label(ui), "Open");
    for (int i = 0; i < 10; i++) ui.on_button(Button::Minus, ButtonEvent::Short, 20300 + i * 10);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // Actions page: clamped on row 0 (Reset & re-read)
    CHECK_STR(ok_label(ui), "Reset");
    ui.on_button(Button::Back, ButtonEvent::Short, 20450);             // Short Back -> back to 3-slice Overview
    ui.tick(20460);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(cp(ui).action_row(), -1);
    ui.on_button(Button::Back, ButtonEvent::Short, 20500);             // Short Back on Overview -> out to the ring, Pos::Link
    CHECK_EQ(ui.ring_view(), true);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
    CHECK_EQ(int(ui.menu()), int(Menu::Connection));

    ui.open_place(bench::Place::Charger, 21000);
    ui.tick(21050);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_STR(ok_label(ui), "Open");
    ui.on_button(Button::Ok, ButtonEvent::Short, 21055);               // Short OK -> Charger Actions
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // row 0 = Reset & re-read
    CHECK_STR(ok_label(ui), "Reset");
    ui.on_button(Button::Minus, ButtonEvent::Short, 21060);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // Actions page: stays clamped on row 0
    CHECK_STR(ok_label(ui), "Reset");
    ui.open_place(bench::Place::Device, 21100);                        // Device slice focused on Overview
    ui.tick(21150);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));
    CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::Details));
    CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::Identify));
    ui.on_button(Button::Ok, ButtonEvent::Short, 21160);               // Overview -> Device Actions
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // row 0: Reset & re-read
    ui.on_button(Button::Plus, ButtonEvent::Short, 21165);             // row 1: Sink details
    CHECK_STR(ok_label(ui), "View");
    ui.on_button(Button::Plus, ButtonEvent::Short, 21170);             // row 2: Ask device
    CHECK_STR(ok_label(ui), "Ask");
    ui.on_button(Button::Back, ButtonEvent::Short, 21180);             // Short Back on Actions -> returns to Overview
    CHECK_EQ(cp(ui).overview_mode(), true);
    ui.on_button(Button::Back, ButtonEvent::Short, 21200);
    CHECK_EQ(ui.ring_view(), true);                                    // Short Back on Overview leaves to the ring
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
}

// The ring opens the unified Link page (Home + 5 steps, then OK). Slice Actions open tools over the Connection page
// (BACK comes back to Actions); Test charger replaces both and its BACK lands on Charger Actions again.
// The Spoof shortcut opens Cable Actions underneath Menu::Spoof.
static void test_tools_rows_open_single_tools() {
    Ui &ui = fresh_ui();
    ui.open_place(bench::Place::Meter, 20000);
    ui.on_button(Button::Back, ButtonEvent::Short, 20010);             // Home entered -> the ring on Home
    CHECK_EQ(ui.ring_view(), true);
    for (int i = 0; i < 5; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 20100 + i * 10);   // Pos::Link (5)
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
    CHECK_EQ(cp(ui).overview_mode(), true);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20500);               // enter Link Overview
    CHECK_EQ(on_pd(ui), 1);
    CHECK_EQ(ui.ring_view(), false);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    ui.on_button(Button::Back, ButtonEvent::Short, 20600);
    CHECK_EQ(ui.ring_view(), true);                                    // Overview -> out to the ring

    // Charger Actions: 0=Reset & re-read, 1=Spec & offers, 2=Test charger, 3=Report cards (triggers moved to 7 TOOLS)
    ui.context().keep.place_sel[static_cast<int>(bench::Place::Charger)] = 3;   // Report cards
    ui.open_place(bench::Place::Charger, 24000);
    CHECK_EQ(cp(ui).overview_mode(), true);
    ui.on_button(Button::Ok, ButtonEvent::Short, 24050);
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 3);
    CHECK_STR(cp(ui).item(3).label, "Report cards");
    ui.on_button(Button::Ok, ButtonEvent::Short, 24100);
    CHECK_EQ(int(ui.menu()), int(Menu::Cards));
    CHECK_EQ(int(ui.menu_depth()), 2);                                 // over the place page
    ui.on_button(Button::Back, ButtonEvent::Short, 24200);
    CHECK_EQ(on_pd(ui), 1);
    CHECK_EQ(cp(ui).actions_mode(), true);
    ui.on_button(Button::Minus, ButtonEvent::Short, 24300);            // Test charger (row 2)
    CHECK_EQ(cp(ui).action_row(), 2);
    ui.on_button(Button::Ok, ButtonEvent::Short, 24400);
    CHECK_EQ(int(ui.page()), int(Page::ChargerTest));
    CHECK_EQ(int(ui.menu_depth()), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 24500);
    CHECK_EQ(on_pd(ui), 1);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 2);
    CHECK(ui.open_connection(conn::Tab::Cable, true, 25000));
    CHECK_EQ(int(ui.menu()), int(Menu::Spoof));
    ui.on_button(Button::Back, ButtonEvent::Short, 25100);
    CHECK_EQ(on_pd(ui), 1);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    CHECK_EQ(cp(ui).actions_mode(), true);
}

// Hardware 2026-11-15 (OnePlus 120 W, charger test SVOOC ceiling stage, PD-silent legacy session, no contract): the adapter
// answered the K2's SuperVOOC trigger by raising VBUS (4.93 V -> 6.90 V in 20 ms) and the Link took that rise for a sink
// behind the K2 (cause Rise): Rd off, the stage aborted, the brick dropped VBUS. The trigger's own ramp is the K2's doing.
static void test_svooc_trigger_rise_is_not_a_sink() {
    Ui &ui = fresh_ui();
    g_stub.cc = 1; g_stub.pm.vbus_mv = 5270; g_stub.pm.current_ma = 0;
    uint32_t t = 20000;
    for (int i = 0; i < 5; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    g_stub.svooc_run = true;                           // the guarded trigger holds D+/D-
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    g_stub.pm.vbus_mv = 4931; ui.tick(t += 20);        // the dip before the ramp
    for (int mv : {6903, 7879, 7965, 7983}) { g_stub.pm.vbus_mv = mv; ui.tick(t += 20); }
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    g_stub.svooc_run = false;                          // the exit: VBUS back at 5 V inside the hold
    g_stub.pm.vbus_mv = 5100;
    for (int i = 0; i < 5; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    // A rise nobody on this side asked for latches nothing either (user decision 2026-11-15); a load does.
    for (int i = 0; i < 300; i++) ui.tick(t += 100);
    g_stub.pm.vbus_mv = 9000;
    for (int i = 0; i < 30; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    g_stub.pm.current_ma = 400;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    g_stub.pm.current_ma = 0;
    g_stub.pm.vbus_mv = 5000; g_stub.cc = 0;
}

static void test_device_place_inline_interrogate_and_readout() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    uint32_t t = 21000;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);   // Sink Spec
    settle(ui, t);
    const int before_rows = cp(ui).result_rows();
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);   // Spec -> Actions (row 0 = Reset & re-read)
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100); // row 1 = Sink details
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100); // row 2 = Ask device
    // First press arms confirmation ("OK again: resets link")
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.iq_starts, 0);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "OK again: resets link");
    // Second press starts inline interrogation
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.iq_starts, 1);
    CHECK_EQ(g_stub.iq_running, true);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Asking device...");
    // Populate full answers and finish interrogation
    g_stub.iq_running = false;
    g_stub.iq_status.phase = pd::IqPhase::Done;
    g_stub.iq_status.ok_mask = pd::iq_bit(pd::IqStep::Identity) | pd::iq_bit(pd::IqStep::SinkCap) |
                               pd::iq_bit(pd::IqStep::SinkCapExt) | pd::iq_bit(pd::IqStep::Revision) |
                               pd::iq_bit(pd::IqStep::BatteryCap) | pd::iq_bit(pd::IqStep::BatteryStatus) |
                               pd::iq_bit(pd::IqStep::MfrInfo) | pd::iq_bit(pd::IqStep::BattMfrInfo) |
                               pd::iq_bit(pd::IqStep::Status) | pd::iq_bit(pd::IqStep::Svids) |
                               pd::iq_bit(pd::IqStep::EprSinkCap);
    g_stub.iq_status.id.valid = true; g_stub.iq_status.id.vid = 0x18D1; g_stub.iq_status.id.pid = 0x4EE7; g_stub.iq_status.id.vendor = "Google";
    g_stub.iq_status.mfr.valid = true; std::strcpy(g_stub.iq_status.mfr.name, "Google LLC");
    g_stub.iq_status.revision = 0x31200000u;
    g_stub.iq_status.sink_ext_len = 24;
    g_stub.iq_status.sink_ext[8] = 1; g_stub.iq_status.sink_ext[9] = 2;
    g_stub.iq_status.sink_ext[16] = 0x01; g_stub.iq_status.sink_ext[17] = 0x23;
    g_stub.iq_status.sink_ext[18] = 15; g_stub.iq_status.sink_ext[19] = 27; g_stub.iq_status.sink_ext[20] = 37;
    g_stub.iq_status.battery.valid = true; g_stub.iq_status.battery.design_dwh = 195; g_stub.iq_status.battery.last_full_dwh = 186;
    g_stub.iq_status.battery_status.valid = true; g_stub.iq_status.battery_status.present = true;
    g_stub.iq_status.battery_status.charging = 0; g_stub.iq_status.battery_status.present_dwh = 149;
    g_stub.iq_status.batt_mfr.valid = true; g_stub.iq_status.batt_mfr.vid = 0x18D1; g_stub.iq_status.batt_mfr.pid = 0x0001;
    std::strcpy(g_stub.iq_status.batt_mfr.name, "ATL-4800mAh");
    g_stub.iq_status.status.valid = true; g_stub.iq_status.status.internal_temp_c = 34;
    g_stub.iq_status.status.temp_status = 1; g_stub.iq_status.status.present_input = 0x02;
    g_stub.iq_status.num_svids = 2; g_stub.iq_status.svids[0] = 0xFF01; g_stub.iq_status.svids[1] = 0x18D1;
    g_stub.iq_status.num_epr_pdos = 9;
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // row 1 = Sink details
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // -> View::Spec to inspect rows
    settle(ui, t);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Device read");
    // Mfr, Rev, PDP, Sink, Batt, Health, Batt mfr, Status, SVIDs, EPR all added (+10 rows) with zero truncation
    CHECK_EQ(cp(ui).result_rows(), before_rows + 10);
    auto row_visible_is = [&](int row_idx, const char *val, uint16_t ink = kText) {
        while (cp(ui).window_first() > row_idx) {
            ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
            settle(ui, t);
        }
        while (cp(ui).window_first() + ConnectionPage::kPlaceRows <= row_idx) {
            ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
            settle(ui, t);
        }
        return row_value_is(row_idx - cp(ui).window_first(), val, ink);
    };
    CHECK(row_visible_is(before_rows + 2, "SPR 27/37W PPS AVS"));
    CHECK(row_visible_is(before_rows + 3, "FW1 HW2 VBUS 1bat"));
    CHECK(row_visible_is(before_rows + 6, "ATL-4800mAh"));
    CHECK(row_visible_is(before_rows + 7, "34C Ext"));
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

static void test_device_place_inline_interrogate_with_sink_pdos() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    uint32_t t = 21000;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);   // Sink Spec
    settle(ui, t);
    const int before_rows = cp(ui).result_rows();   // includes Caps: Not seen (1 row)
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // Spec -> Actions
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 1 = Sink details
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 2 = Ask device
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // arm
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // confirm
    g_stub.iq_running = false;
    g_stub.iq_status.phase = pd::IqPhase::Done;
    g_stub.iq_status.ok_mask = pd::iq_bit(pd::IqStep::Identity) | pd::iq_bit(pd::IqStep::SinkCap) |
                               pd::iq_bit(pd::IqStep::SinkCapExt) | pd::iq_bit(pd::IqStep::Revision) |
                               pd::iq_bit(pd::IqStep::BatteryCap) | pd::iq_bit(pd::IqStep::BatteryStatus) |
                               pd::iq_bit(pd::IqStep::MfrInfo) | pd::iq_bit(pd::IqStep::BattMfrInfo) |
                               pd::iq_bit(pd::IqStep::Status) | pd::iq_bit(pd::IqStep::Svids) |
                               pd::iq_bit(pd::IqStep::EprSinkCap);
    g_stub.iq_status.id.valid = true; g_stub.iq_status.id.vid = 0x18D1; g_stub.iq_status.id.pid = 0x4EE7; g_stub.iq_status.id.vendor = "Google";
    g_stub.iq_status.num_sink_pdos = 3;
    g_stub.iq_status.sink_pdos = {0x3601912Cu, 0x0002D12Cu, 0xC0DC323Cu, 0, 0, 0, 0};   // 5V 3A, 9V 3A, PPS 5-11V 3A
    g_stub.iq_status.mfr.valid = true; std::strcpy(g_stub.iq_status.mfr.name, "Google LLC");
    g_stub.iq_status.revision = 0x31200000u;
    g_stub.iq_status.sink_ext_len = 24;
    g_stub.iq_status.sink_ext[8] = 2; g_stub.iq_status.sink_ext[9] = 1;
    g_stub.iq_status.sink_ext[17] = 0x2B;   // PPS | AVS | VBUS | Batt
    g_stub.iq_status.sink_ext[19] = 27; g_stub.iq_status.sink_ext[20] = 37; g_stub.iq_status.sink_ext[23] = 100;
    g_stub.iq_status.battery.valid = true; g_stub.iq_status.battery.design_dwh = 195; g_stub.iq_status.battery.last_full_dwh = 186;
    g_stub.iq_status.battery_status.valid = true; g_stub.iq_status.battery_status.present = true;
    g_stub.iq_status.battery_status.charging = 0; g_stub.iq_status.battery_status.present_dwh = 149;
    g_stub.iq_status.batt_mfr.valid = true; g_stub.iq_status.batt_mfr.vid = 0x04A9; g_stub.iq_status.batt_mfr.pid = 0x5678;
    g_stub.iq_status.status.valid = true; g_stub.iq_status.status.internal_temp_c = 42;
    g_stub.iq_status.status.temp_status = 2; g_stub.iq_status.status.present_input = 0x06; g_stub.iq_status.status.event_flags = 0x08;
    g_stub.iq_status.num_svids = 2; g_stub.iq_status.svids[0] = 0xFF01; g_stub.iq_status.svids[1] = 0x18D1;
    g_stub.iq_status.num_epr_pdos = 9;
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // row 1 = Sink details
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // -> View::Spec
    settle(ui, t);
    // Caps: Not seen (1 row) -> grouped Fixed (1 row) + PPS (1 row) (+1 net), plus Mfr, Rev, PDP, Sink, Batt, Health, Batt mfr, Status, SVIDs, EPR (+10) = +11 rows
    CHECK_EQ(cp(ui).result_rows(), before_rows + 11);
    auto row_visible_is = [&](int row_idx, const char *val, uint16_t ink = kText) {
        while (cp(ui).window_first() > row_idx) {
            ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
            settle(ui, t);
        }
        while (cp(ui).window_first() + ConnectionPage::kPlaceRows <= row_idx) {
            ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
            settle(ui, t);
        }
        return row_value_is(row_idx - cp(ui).window_first(), val, ink);
    };
    CHECK(row_visible_is(before_rows + 3, "SPR 37W EPR 100W"));
    CHECK(row_visible_is(before_rows + 4, "FW2 HW1 VBUS Batt"));
    CHECK(row_visible_is(before_rows + 7, "04A9:5678"));
    CHECK(row_visible_is(before_rows + 8, "42C Warn OVP AC", kError));
    // Scroll through the remaining rows so every row is painted and checked for truncation
    for (int i = 0; i < 4; i++) { ui.on_button(Button::Plus, ButtonEvent::Short, t += 100); settle(ui, t); }
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

static void test_device_place_standalone_and_self_powered_identify() {
    {   // Standalone DRP Identify: Rp toggling, !request_seen, identify_current() == true
        Ui &ui = fresh_ui();
        g_stub.cc = 1; g_stub.pm_online = true; g_stub.pm.vbus_mv = 0;
        g_stub.caps.valid = false; g_stub.caps_this_attach = false;
        uint32_t t = 12000;
        for (; t <= 13400; t += 100) {
            set_cc(0, (t / 100) % 2 == 0 ? 3300 : 0);
            ui.tick(t);
        }
        CHECK_EQ(int(ui.link().wait_kind()), int(raguard::WaitKind::DualRolePort));
        g_stub.ident_current = true;
        g_stub.ident.state = pd::IdState::Done;
        g_stub.ident.ok_mask = uint16_t((1u << int(pd::IdStep::Identity)) | (1u << int(pd::IdStep::SinkCap)) |
                                        (1u << int(pd::IdStep::Revision)) | (1u << int(pd::IdStep::BatCap)) |
                                        (1u << int(pd::IdStep::BatStatus)));
        g_stub.ident_identity.valid = true; g_stub.ident_identity.vid = 0x18D1; g_stub.ident_identity.pid = 0x4EE7;
        g_stub.ident_identity.vendor = "Google";
        g_stub.ident.num_sink_pdos = 3;
        g_stub.ident.sink_pdos = {0x3601912Cu, 0x0002D12Cu, 0xC0DC323Cu, 0, 0, 0, 0};
        g_stub.ident.revision = 0x31200000u;
        g_stub.ident_ext.battery.valid = true; g_stub.ident_ext.battery.design_dwh = 195; g_stub.ident_ext.battery.last_full_dwh = 150;
        g_stub.ident_ext.battery_status.valid = true; g_stub.ident_ext.battery_status.present = true;
        g_stub.ident_ext.battery_status.charging = 0; g_stub.ident_ext.battery_status.present_dwh = 120;
        ui.goto_page(kConnTabAlias + 2, t += 100);
        settle(ui, t);
        while (cp(ui).action_row() >= 0 || cp(ui).window_first() > 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        // Report card rows only: ID, Fixed, PPS, Rev, Batt, Health (6 rows; no Request/Pwr/USB noise)
        CHECK_EQ(cp(ui).result_rows(), 6);
        CHECK(!row_value_is(0, "Unknown"));
        CHECK(!row_value_is(1, "None seen", kFaint));
#ifdef K2_TRUNC_LOG
        CHECK_EQ(k2::trunc::failures(), 0);
#endif
    }
    {   // Self-powered Identify: self_powered == true, identify_current() == true
        Ui &ui = fresh_ui();
        g_stub.self_powered = true;
        g_stub.cc = 1; g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 1;
        g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
        g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
        uint32_t t = 20000;
        for (int i = 0; i < 5; i++) ui.tick(t += 100);
        ui.goto_page(kConnTabAlias + 2, t += 100);
        settle(ui, t);
        while (cp(ui).action_row() >= 0 || cp(ui).window_first() > 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).result_rows(), 3);   // pre-Identify: ID: K2, Sink: K2 (sink mode OK), USB
        g_stub.ident_current = true;
        g_stub.ident.state = pd::IdState::Done;
        g_stub.ident.ok_mask = uint16_t((1u << int(pd::IdStep::Identity)) | (1u << int(pd::IdStep::SinkCap)) |
                                        (1u << int(pd::IdStep::Revision)) | (1u << int(pd::IdStep::BatCap)) |
                                        (1u << int(pd::IdStep::BatStatus)));
        g_stub.ident_identity.valid = true; g_stub.ident_identity.vid = 0x18D1; g_stub.ident_identity.pid = 0x4EE7;
        g_stub.ident_identity.vendor = "Google";
        g_stub.ident.num_sink_pdos = 2;
        g_stub.ident.sink_pdos = {0x3601912Cu, 0xC0DC323Cu, 0, 0, 0, 0, 0};
        g_stub.ident.revision = 0x31200000u;
        g_stub.ident_ext.battery.valid = true; g_stub.ident_ext.battery.design_dwh = 195; g_stub.ident_ext.battery.last_full_dwh = 186;
        g_stub.ident_ext.battery_status.valid = true; g_stub.ident_ext.battery_status.present = true;
        g_stub.ident_ext.battery_status.charging = 0; g_stub.ident_ext.battery_status.present_dwh = 149;
        settle(ui, t);
        // Report card rows only: ID, Fixed, PPS, Rev, Batt, Health (6 rows; no Request/Pwr/USB/K2 noise)
        CHECK_EQ(cp(ui).result_rows(), 6);
        CHECK(!row_value_is(0, "K2", kFaint));
#ifdef K2_TRUNC_LOG
        CHECK_EQ(k2::trunc::failures(), 0);
#endif
    }
}

static void test_place_two_page_info_and_actions() {
    // 1. Empty Cable Spec page: opens on Spec (result_rows == 0, action_row == -1); Short OK enters Actions.
    {
        Ui &ui = fresh_ui();
        uint32_t t = 20000;
        ui.goto_page(kConnTabAlias + 1, t);
        settle(ui, t);
        CHECK_EQ(cp(ui).spec_mode(), true);
        CHECK_EQ(cp(ui).actions_mode(), false);
        CHECK_EQ(cp(ui).result_rows(), 0);
        CHECK_EQ(cp(ui).window_first(), 0);
        CHECK_EQ(cp(ui).action_row(), -1);
        CHECK_EQ(ui.list_state().count, 0);
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).window_first(), 0);
        CHECK_EQ(cp(ui).action_row(), -1);

        // Short OK enters Actions page using kit::RowList
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(cp(ui).action_row(), 0);
        CHECK_EQ(ui.list_state().count, cp(ui).act_count());
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).action_row(), 0);
    }

    // 2. Populated Cable Spec with >= 8 fact rows:
    //    - Spec opens at window_first() == 0, action_row() == -1; Short OK switches to Actions;
    //      Short +/- step through Actions; Short Back switches to Overview.
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 2.0"; em.latency_str = "<10 ns (~1 m)";
    em.max_current_a = 5; em.max_voltage_v = 20; em.max_power_w = 100;
    g_stub.cable_ans = emk_variant::answered_from(2, 1);
    g_stub.cable_seq++;
    uint32_t t = 51000;
    ui.tick(t);
    ui.goto_page(kConnTabAlias + 1, t += 100);
    settle(ui, t);

    const int res = cp(ui).result_rows();
    CHECK(res >= 8);
    CHECK_EQ(cp(ui).spec_mode(), true);
    CHECK_EQ(cp(ui).window_first(), 0);
    CHECK_EQ(cp(ui).action_row(), -1);

    // Scroll down 1 fact row at a time until all fact rows are visible (first == max(0, res - kPlaceRows)).
    const int max_fact_first = std::max(0, res - ConnectionPage::kPlaceRows);
    for (int f = 1; f <= max_fact_first; f++) {
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).window_first(), f);
        CHECK_EQ(cp(ui).action_row(), -1);
    }
    // Further Short Plus on Spec clamps at max_fact_first (does not enter Actions).
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).window_first(), max_fact_first);
    CHECK_EQ(cp(ui).action_row(), -1);
    CHECK_EQ(cp(ui).actions_mode(), false);

    // Minus scrolls back 1 row at a time to 0.
    for (int f = max_fact_first - 1; f >= 0; f--) {
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).window_first(), f);
        CHECK_EQ(cp(ui).action_row(), -1);
    }

    // Short OK switches to Actions sub-page.
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 0);
    CHECK_EQ(ui.list_state().count, cp(ui).act_count());
    const int spoof_row = cp(ui).act_count() - 1;
    for (int k = 0; k < spoof_row; k++) { ui.on_button(Button::Plus, ButtonEvent::Short, t += 100); settle(ui, t); }
    CHECK_EQ(cp(ui).action_row(), spoof_row);
    CHECK_EQ(int(cp(ui).item(spoof_row).act), int(conn::Act::SpoofAs));
    CHECK_EQ(cp(ui).item(spoof_row).enabled, false);
    CHECK_STR(cp(ui).item(spoof_row).tag, "load");
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Live sink: unplug load");
    // Short Back on Actions switches back to Overview.
    ui.on_button(Button::Back, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(cp(ui).action_row(), -1);
    CHECK_EQ(ui.list_state().count, 0);
    g_stub.em = {}; g_stub.cable_ans = {};
}

static void test_cable_identity_positive_chips_and_no_trunc() {
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    pd::Emarker &em = g_stub.em;
    em.valid = true; em.epr_capable = false;
    em.vid = 0x2E99; em.vendor_str = "Baseus"; em.speed_str = "USB 2.0"; em.latency_str = "<10 ns (~1 m)";
    em.max_current_a = 5; em.max_voltage_v = 20; em.max_power_w = 100;
    uint32_t t = 51000;
    ui.tick(t);
    ui.goto_page(kConnTabAlias + 1, t += 100);
    settle(ui, t);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    // Verify the painted identity header matches positive chips only ([100W] [2.0]) and NOT "no EPR".
    const Surface s = ui.context().screen;
    const int bw = s.w, bh = kit::kPlaceListY - kit::kIdY, by = kit::kIdY;
    std::vector<uint16_t> orig(size_t(bw * bh));
    for (int y = 0; y < bh; y++)
        std::memcpy(&orig[size_t(y * bw)], &g_panel[size_t((by + y) * LCD_WIDTH + s.x)], size_t(bw) * sizeof(uint16_t));

    auto matches_ident = [&](const kit::Ident &id) -> bool {
        uint32_t sig = 0;
        kit::paint_identity(s, id, true, sig);
        bool eq = true;
        for (int y = 0; y < bh && eq; y++)
            if (std::memcmp(&orig[size_t(y * bw)], &g_panel[size_t((by + y) * LCD_WIDTH + s.x)], size_t(bw) * sizeof(uint16_t)) != 0)
                eq = false;
        for (int y = 0; y < bh; y++)
            std::memcpy(&g_panel[size_t((by + y) * LCD_WIDTH + s.x)], &orig[size_t(y * bw)], size_t(bw) * sizeof(uint16_t));
        return eq;
    };

    kit::Ident expected;
    expected.dim = ui.link().cable_stale();
    expected.head.put("5A 20V");
    expected.add("100W", kit::Ident::Solid);
    expected.add("2.0", kit::Ident::Outline);
    expected.line.put("2E99:0000 Baseus, passive");
    CHECK(matches_ident(expected));

    kit::Ident with_no_epr;
    with_no_epr.dim = ui.link().cable_stale();
    with_no_epr.head.put("5A 20V");
    with_no_epr.add("100W", kit::Ident::Solid);
    with_no_epr.add("no EPR", kit::Ident::Outline);
    with_no_epr.line.put("2E99:0000 Baseus, passive");
    CHECK(!matches_ident(with_no_epr));

    g_stub.em = {};
}

static void test_device_place_telemetry_rows_and_live_repaints() {
    // 1. Live updates to ext.status, ext.batt_mfr, and iq.sink_ext trigger immediate repaints via dev_sig.
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    uint32_t t = 21000;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);
    settle(ui, t);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // Spec -> Actions
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 1 = Sink details
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 2 = Ask device
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    g_stub.iq_running = false;
    g_stub.iq_status.phase = pd::IqPhase::Done;
    g_stub.iq_status.ok_mask = pd::iq_bit(pd::IqStep::SinkCapExt);
    g_stub.iq_status.sink_ext_len = 24;
    g_stub.iq_status.sink_ext[18] = 15;
    g_stub.iq_status.sink_ext[20] = 45;   // SPR 45W min 15 (spr_op == 0, epr_max == 0)
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // row 1 = Sink details
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // -> View::Spec
    settle(ui, t);
    const int base_rows = cp(ui).result_rows();   // includes PDP ("SPR 45W min 15"), no Sink/Batt mfr/Status yet
    auto row_visible_is = [&](int row_idx, const char *val, uint16_t ink = kText) {
        while (cp(ui).window_first() > row_idx) { ui.on_button(Button::Minus, ButtonEvent::Short, t += 100); settle(ui, t); }
        while (cp(ui).window_first() + ConnectionPage::kPlaceRows <= row_idx) {
            ui.on_button(Button::Plus, ButtonEvent::Short, t += 100); settle(ui, t);
        }
        return row_value_is(row_idx - cp(ui).window_first(), val, ink);
    };
    CHECK(row_visible_is(base_rows - 1, "SPR 45W min 15"));

    // Updating iq.sink_ext bytes (load_step + AC + 2bat) repaints immediately without any other state change.
    g_stub.iq_status.sink_ext[11] = 1;
    g_stub.iq_status.sink_ext[16] = 0x05;
    g_stub.iq_status.sink_ext[17] = 0x04;
    settle(ui, t);
    CHECK_EQ(cp(ui).result_rows(), base_rows + 1);
    CHECK(row_visible_is(base_rows, "AC 2bat 500mA/us"));

    // Updating ext.batt_mfr and ext.status triggers immediate repaints and handles <2C/Int, Warn, OTP deduplication, Norm.
    g_stub.ext.batt_mfr.valid = true;
    std::strcpy(g_stub.ext.batt_mfr.name, "VeryLongBatteryVendorName26");
    g_stub.ext.status.valid = true;
    g_stub.ext.status.internal_temp_c = 1;
    g_stub.ext.status.present_input = 0x10;
    settle(ui, t);
    CHECK_EQ(cp(ui).result_rows(), base_rows + 3);
    CHECK(row_visible_is(base_rows + 1, "VeryLongBatteryVend"));
    CHECK(row_visible_is(base_rows + 2, "<2C Int"));

    g_stub.ext.status.internal_temp_c = 0;
    g_stub.ext.status.temp_status = 1;
    g_stub.ext.status.present_input = 0x06;
    settle(ui, t);
    CHECK(row_visible_is(base_rows + 2, "Norm AC"));

    g_stub.ext.status.internal_temp_c = 40;
    g_stub.ext.status.temp_status = 2;
    g_stub.ext.status.present_input = 0x08;
    settle(ui, t);
    CHECK(row_visible_is(base_rows + 2, "40C Warn Batt", kCaution));

    // temp_status == 3 + event_flags OTP (0x04) must not duplicate "OTP".
    g_stub.ext.status.internal_temp_c = 125;
    g_stub.ext.status.temp_status = 3;
    g_stub.ext.status.event_flags = 0x0E;
    g_stub.ext.status.present_input = 0x1A;
    settle(ui, t);
    CHECK(row_visible_is(base_rows + 2, "125C OTP OCP OVP", kError));

    // 2. Standalone Identify also surfaces id_ext.batt_mfr and id_ext.status and updates immediately.
    Ui &ui2 = fresh_ui();
    g_stub.cc = 1; g_stub.pm_online = true; g_stub.pm.vbus_mv = 0;
    g_stub.caps.valid = false; g_stub.caps_this_attach = false;
    uint32_t t2 = 12000;
    for (; t2 <= 13400; t2 += 100) {
        set_cc(0, (t2 / 100) % 2 == 0 ? 3300 : 0);
        ui2.tick(t2);
    }
    g_stub.ident_current = true;
    g_stub.ident.state = pd::IdState::Done;
    g_stub.ident_identity.valid = true; g_stub.ident_identity.vid = 0x18D1; g_stub.ident_identity.pid = 0x4EE7;
    g_stub.ident_ext.batt_mfr.valid = true; g_stub.ident_ext.batt_mfr.vid = 0x04A9; g_stub.ident_ext.batt_mfr.pid = 0x1234;
    g_stub.ident_ext.status.valid = true; g_stub.ident_ext.status.internal_temp_c = 29; g_stub.ident_ext.status.present_input = 0x02;
    ui2.goto_page(kConnTabAlias + 2, t2 += 100);
    settle(ui2, t2);
    while (cp(ui2).action_row() >= 0 || cp(ui2).window_first() > 0) ui2.on_button(Button::Minus, ButtonEvent::Short, t2 += 100);
    settle(ui2, t2);
    CHECK_EQ(cp(ui2).result_rows(), 3);   // ID, Batt mfr, Status
    CHECK(row_value_is(1, "04A9:1234"));
    CHECK(row_value_is(2, "29C Ext"));

    g_stub.ident_ext.status.temp_status = 3;
    settle(ui2, t2);
    CHECK(row_value_is(2, "29C OTP Ext", kError));
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// A cable record the page holds (what the Cable overview shows): the K2 read it earlier.
static void hold_cable(Ui &ui, uint32_t t) {
    g_stub.em = {};
    g_stub.em.valid = true; g_stub.em.vid = 0x0451; g_stub.em.pid = 0x1234; g_stub.em.vendor_str = "TI";
    g_stub.em.speed_str = "USB4 Gen3"; g_stub.em.latency_str = "<10 ns (~1 m)";
    g_stub.em.max_current_a = 3; g_stub.em.max_voltage_v = 20; g_stub.em.max_power_w = 60;
    g_stub.cable_seq++;
    ui.tick(t += 100);
    held::poll(t += 10, false);
}

static void test_cable_read_emarker_ux_paths() {
    // 1. VCONN ReadCable success -> opens the chip details (Spec) (spec_mode() == true, actions_mode() == false), no error toast.
    {
        Ui &ui = fresh_ui();
        g_stub.em = {};
        g_stub.pm = {};
        g_stub.cc = 0;
        g_stub.scan_result = true;
        ui.goto_page(kConnTabAlias + 1, 20000);   // Cable Spec
        ui.tick(20050);
        CHECK_EQ(cp(ui).actions_mode(), false);
        ui.on_button(Button::Ok, ButtonEvent::Short, 20100);   // Spec -> Actions (row 0 = Reset & re-read)
        CHECK_EQ(cp(ui).actions_mode(), true);
        ui.on_button(Button::Plus, ButtonEvent::Short, 20125); // row 1 = Read e-marker
        ui.on_button(Button::Ok, ButtonEvent::Short, 20150);   // Read e-marker (VCONN)
        ui.tick(20200);
        CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Vconn));
        CHECK_EQ(cp(ui).actions_mode(), false);
        CHECK_EQ(cp(ui).spec_mode(), true);
        CHECK_EQ(ui.context().toast.active(20200), false);
    }

    // 2. VCONN ReadCable failure, no held record -> stays on Actions, "Failed to read" toast with FooterTone::Error.
    {
        Ui &ui = fresh_ui();
        g_stub.em = {};
        g_stub.pm = {};
        g_stub.cc = 0;
        g_stub.scan_result = false;
        ui.goto_page(kConnTabAlias + 1, 30000);
        ui.tick(30050);
        ui.on_button(Button::Ok, ButtonEvent::Short, 30100);   // Spec -> Actions
        CHECK_EQ(cp(ui).actions_mode(), true);
        ui.on_button(Button::Plus, ButtonEvent::Short, 30125); // row 1 = Read e-marker
        ui.on_button(Button::Ok, ButtonEvent::Short, 30150);   // Read e-marker (VCONN fails)
        ui.tick(30200);
        CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Vconn));
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(ui.context().toast.active(30200), true);
        CHECK_STR(ui.context().toast.view().data(), "Failed to read");
        CHECK_EQ(int(ui.context().toast.tone), int(k2::FooterTone::Error));
        CHECK_EQ(ui.context().toast.active(30150 + k2::kToastDefaultMs - 1), true);
        CHECK_EQ(ui.context().toast.active(30150 + k2::kToastDefaultMs), false);
    }

    // 3. Sniff async success (cable_seq++) -> switches to Spec (actions_mode() == false).
    {
        Ui &ui = fresh_ui();
        g_stub.em = {};
        g_stub.scan_result = false;
        g_stub.pm = {};
        g_stub.pm.vbus_mv = 5117;
        g_stub.cc = 1;
        uint32_t t = 40000;
        ui.debug_link(true, t);
        ui.tick(t += 50);
        ui.goto_page(kConnTabAlias + 1, t += 50);
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Spec -> Actions
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 50); // row 1 = Read e-marker
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Read e-marker (Sniff listen)
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Sniff));
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(ui.context().em_listen_until != 0, true);
        g_stub.cable_seq++;
        ui.tick(t += 500);
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(ui.context().em_listen_until, 0u);
        CHECK_EQ(cp(ui).actions_mode(), false);
        CHECK_EQ(cp(ui).spec_mode(), true);
        CHECK_EQ(ui.context().toast.active(t), false);
        ui.debug_link(false, t += 100);
    }

    // 4. Sniff async timeout (em_quiet), no held record -> stays on Actions, "Failed to read" toast with FooterTone::Error.
    {
        Ui &ui = fresh_ui();
        g_stub.em = {};
        g_stub.scan_result = false;
        g_stub.pm = {};
        g_stub.pm.vbus_mv = 5117;
        g_stub.cc = 1;
        uint32_t t = 50000;
        ui.debug_link(true, t);
        ui.tick(t += 50);
        ui.goto_page(kConnTabAlias + 1, t += 50);
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Spec -> Actions
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 50); // row 1 = Read e-marker
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Read e-marker (Sniff listen)
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(cp(ui).actions_mode(), true);
        ui.tick(t += 5200);                                      // 5 s listen window expires
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(ui.context().em_listen_until, 0u);
        CHECK_EQ(ui.context().em_quiet, true);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(ui.context().toast.active(t), true);
        CHECK_STR(ui.context().toast.view().data(), "Failed to read");
        CHECK_EQ(int(ui.context().toast.tone), int(k2::FooterTone::Error));
        ui.debug_link(false, t += 100);
    }

    // 5. VCONN failure with a held cable record -> the held chip details open, "Failed to read" in the footer.
    {
        Ui &ui = fresh_ui();
        g_stub.pm = {};
        g_stub.cc = 0;
        g_stub.scan_result = false;
        hold_cable(ui, 60000);
        CHECK_EQ(held::cable_em().valid, true);
        ui.goto_page(kConnTabAlias + 1, 60500);
        ui.tick(60550);
        ui.on_button(Button::Ok, ButtonEvent::Short, 60600);   // Spec -> Actions
        ui.on_button(Button::Plus, ButtonEvent::Short, 60625); // row 1 = Read e-marker
        ui.on_button(Button::Ok, ButtonEvent::Short, 60650);   // the read fails
        ui.tick(60700);
        CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Vconn));
        CHECK_EQ(cp(ui).actions_mode(), false);
        CHECK_EQ(cp(ui).spec_mode(), true);
        CHECK_EQ(ui.context().toast.active(60700), true);
        CHECK_STR(ui.context().toast.view().data(), "Failed to read");
        CHECK_EQ(int(ui.context().toast.tone), int(k2::FooterTone::Error));
        CHECK_EQ(held::cable_em().valid, true);
        ui.on_button(Button::Ok, ButtonEvent::Short, 60800);   // the sheet is one press from Actions again
        CHECK_EQ(cp(ui).actions_mode(), true);
    }

    // 6. Sniff timeout with a held cable record -> the held chip details open, "Failed to read" in the footer.
    {
        Ui &ui = fresh_ui();
        g_stub.scan_result = false;
        g_stub.pm = {};
        g_stub.pm.vbus_mv = 5117;
        g_stub.cc = 1;
        uint32_t t = 70000;
        ui.debug_link(true, t);
        hold_cable(ui, t += 50);
        CHECK_EQ(held::cable_em().valid, true);
        t += 600;
        ui.goto_page(kConnTabAlias + 1, t += 50);
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Spec -> Actions
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 50); // row 1 = Read e-marker
        while (ui.painting()) ui.tick(t += 2);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Read e-marker (Sniff listen)
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(cp(ui).actions_mode(), true);                   // listening: still on the menu
        ui.tick(t += 5200);                                      // 5 s listen window expires
        while (ui.painting()) ui.tick(t += 2);
        CHECK_EQ(ui.context().em_quiet, true);
        CHECK_EQ(cp(ui).actions_mode(), false);
        CHECK_EQ(cp(ui).spec_mode(), true);
        CHECK_EQ(ui.context().toast.active(t), true);
        CHECK_STR(ui.context().toast.view().data(), "Failed to read");
        CHECK_EQ(int(ui.context().toast.tone), int(k2::FooterTone::Error));
        ui.debug_link(false, t += 100);
    }

    g_stub.em = {};
    g_stub.scan_result = true;
    g_stub.pm = {};
    g_stub.cc = 0;
}

static void setup_scrollable_device(Ui &ui, uint32_t &t) {
    make_inline(ui, t);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.ol_facts.far_pd_message = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    for (int i = 0; i < 20; i++) ui.tick(t += 100);
    ui.goto_page(kConnTabAlias + 2, t += 100);
    settle(ui, t);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // Spec -> Actions
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 1 = Sink details
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);    // row 2 = Ask device
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    g_stub.iq_running = false;
    g_stub.iq_status.phase = pd::IqPhase::Done;
    g_stub.iq_status.ok_mask = pd::iq_bit(pd::IqStep::Identity) | pd::iq_bit(pd::IqStep::SinkCap) |
                               pd::iq_bit(pd::IqStep::SinkCapExt) | pd::iq_bit(pd::IqStep::Revision) |
                               pd::iq_bit(pd::IqStep::BatteryCap) | pd::iq_bit(pd::IqStep::BatteryStatus) |
                               pd::iq_bit(pd::IqStep::MfrInfo) | pd::iq_bit(pd::IqStep::BattMfrInfo) |
                               pd::iq_bit(pd::IqStep::Status) | pd::iq_bit(pd::IqStep::Svids) |
                               pd::iq_bit(pd::IqStep::EprSinkCap);
    g_stub.iq_status.id.valid = true; g_stub.iq_status.id.vid = 0x18D1; g_stub.iq_status.id.pid = 0x4EE7; g_stub.iq_status.id.vendor = "Google";
    g_stub.iq_status.num_sink_pdos = 3;
    g_stub.iq_status.sink_pdos = {0x3601912Cu, 0x0002D12Cu, 0xC0DC323Cu, 0, 0, 0, 0};
    g_stub.iq_status.mfr.valid = true; std::strcpy(g_stub.iq_status.mfr.name, "Google LLC");
    g_stub.iq_status.revision = 0x31200000u;
    g_stub.iq_status.sink_ext_len = 24;
    g_stub.iq_status.sink_ext[8] = 2; g_stub.iq_status.sink_ext[9] = 1;
    g_stub.iq_status.sink_ext[17] = 0x2B;
    g_stub.iq_status.sink_ext[19] = 27; g_stub.iq_status.sink_ext[20] = 37; g_stub.iq_status.sink_ext[23] = 100;
    g_stub.iq_status.battery.valid = true; g_stub.iq_status.battery.design_dwh = 195; g_stub.iq_status.battery.last_full_dwh = 186;
    g_stub.iq_status.battery_status.valid = true; g_stub.iq_status.battery_status.present = true;
    g_stub.iq_status.battery_status.present_dwh = 149;
    g_stub.iq_status.batt_mfr.valid = true; g_stub.iq_status.batt_mfr.vid = 0x04A9; g_stub.iq_status.batt_mfr.pid = 0x5678;
    g_stub.iq_status.status.valid = true; g_stub.iq_status.status.internal_temp_c = 42;
    g_stub.iq_status.status.temp_status = 2; g_stub.iq_status.status.present_input = 0x06; g_stub.iq_status.status.event_flags = 0x08;
    g_stub.iq_status.num_svids = 2; g_stub.iq_status.svids[0] = 0xFF01; g_stub.iq_status.svids[1] = 0x18D1;
    g_stub.iq_status.num_epr_pdos = 9;
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // row 1 = Sink details
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);      // -> View::Spec
    settle(ui, t);
}

static void test_device_scroll_thumb_reaches_bottom() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    setup_scrollable_device(ui, t);
    const int total = cp(ui).result_rows();
    CHECK(total > ConnectionPage::kPlaceRows);
    const int max_offset = total - ConnectionPage::kPlaceRows;
    for (int i = 0; i < max_offset; i++) {
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
        settle(ui, t);
    }
    CHECK_EQ(cp(ui).window_first(), max_offset);
    // Portrait content starts at x = kOppRailThickness (16), width = kTwinContentW (208).
    // Scroll thumb (3 px wide, kInk3) sits at logical x = 205..207 -> panel x = 221..223.
    const int thumb_x = kOppRailThickness + kTwinContentW - 2;
    CHECK_EQ(g_panel[(kFooterY - 2) * LCD_WIDTH + thumb_x], kInk3);
    CHECK_EQ(g_panel[(kFooterY - 1) * LCD_WIDTH + thumb_x], kInk3);
    CHECK_EQ(g_panel[kit::kPlaceListY * LCD_WIDTH + thumb_x], kBg);
}

static void test_device_scroll_toast_stays_on_top() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    setup_scrollable_device(ui, t);
    const int total = cp(ui).result_rows();
    CHECK(total > ConnectionPage::kPlaceRows);

    constexpr std::string_view kText = "OK again: resets link";
    ui.context().toast.show(FooterTone::Info, kText, t, kToastDefaultMs);
    ui.tick(t += 100);
    while (ui.painting()) ui.paint_more(t += 2);
    CHECK(ui.context().toast.active(t));
    CHECK(ui.context().toast.drawn);

    // Snapshot the toast pill itself (the centered frame at y = 220..235, clear of the x=220..223 scroll lane).
    constexpr int py = kFooterY - kToastH - kToastMarginBottom;
    constexpr int x0 = kOppRailThickness + 4;
    constexpr int w = kTwinContentW - 8;
    std::vector<uint16_t> expected(static_cast<size_t>(kToastH) * w);
    // The pill's own columns from its geometry: the page rows beside it are live and scroll under it.
    const int pill_w = static_cast<int>(kText.size() * kLabelAdv) + 16;
    const int pill_l = (kTwinContentW - pill_w) / 2 - (x0 - kOppRailThickness);
    const int pill_r = pill_l + pill_w - 1;
    for (int dy = 0; dy < kToastH; dy++)
        for (int dx = 0; dx < w; dx++) expected[static_cast<size_t>(dy) * w + dx] = g_panel[(py + dy) * LCD_WIDTH + x0 + dx];
    CHECK(pill_r - pill_l + 1 > 32);

    auto check_toast_band = [&]() {
        for (int dy = 0; dy < kToastH; dy++) {
            for (int dx = pill_l; dx <= pill_r; dx++) {
                CHECK_EQ(g_panel[(py + dy) * LCD_WIDTH + x0 + dx], expected[static_cast<size_t>(dy) * w + dx]);
            }
        }
    };

    const int max_offset = total - ConnectionPage::kPlaceRows;
    for (int i = 0; i < max_offset; i++) {
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 20);
        check_toast_band();
        while (ui.painting()) {
            ui.paint_more(t += 2);
            check_toast_band();
        }
    }
    for (int i = 0; i < max_offset; i++) {
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 20);
        check_toast_band();
        while (ui.painting()) {
            ui.paint_more(t += 2);
            check_toast_band();
        }
    }
}

// Track 2B: 3-slice Overview, Direct Slice Focus, Spec drill-in & full Back chain (Spec -> Actions -> Overview -> Ring).
static void test_overview_3slice_focus_and_navigation() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    // Walk the ring to Pos::Link (ring_view == true -> calm 3-slice Overview with NO focus bar).
    ui.open_place(bench::Place::Meter, t);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 20);
    CHECK_EQ(ui.ring_view(), true);
    for (int i = 0; i < 5; i++) ui.on_button(Button::Plus, ButtonEvent::Short, t += 20);
    settle(ui, t);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(cp(ui).ring_rest(), true);
    const int x_bar = kOppRailThickness + 1;
    CHECK(g_panel[10 * LCD_WIDTH + x_bar] != kFocusBar);   // no focus highlight in ring view

    // Enter Pos::Link -> Direct Slice Focus lights Slice 0 (Charger).
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(ui.ring_view(), false);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    if (kFocusBar != 0) CHECK_EQ(g_panel[10 * LCD_WIDTH + x_bar], kFocusBar);

    // Plus steps Charger -> Cable -> Sink (clamped at Sink); Minus steps back to Charger (clamped).
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    if (kFocusBar != 0) {
        CHECK(g_panel[10 * LCD_WIDTH + x_bar] != kFocusBar);
        CHECK_EQ(g_panel[90 * LCD_WIDTH + x_bar], kFocusBar);
    }
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);   // clamped at Sink
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 50);
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 50);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 50);  // clamped at Source
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));

    // OK -> View::Actions for Charger; Row 0 = Reset & re-read, Row 1 = Spec & offers (2 px kRule top divider).
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(cp(ui).actions_mode(), true);
    CHECK_EQ(cp(ui).action_row(), 0);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::ResetLink));
    CHECK_STR(ok_label(ui), "Reset");
    // Row 0 is focused (expanded by kFocusExtra = 14 px -> height 40 px), so Row 1 starts at kPlaceListY + 40 = 86.
    const int div_y = kit::kPlaceListY + kit::kRowH + kit::kFocusExtra;
    CHECK_EQ(g_panel[div_y * LCD_WIDTH + kOppRailThickness + 20], kRule);
    CHECK_EQ(g_panel[(div_y + 1) * LCD_WIDTH + kOppRailThickness + 20], kRule);

    // Step to Row 1 (Spec & offers) and press OK -> View::Spec.
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    CHECK_EQ(cp(ui).action_row(), 1);
    CHECK_STR(ok_label(ui), "View");
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(cp(ui).spec_mode(), true);

    // Full Short Back chain: Spec -> Actions -> Overview -> Ring view.
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(cp(ui).actions_mode(), true);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(ui.ring_view(), false);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(ui.ring_view(), true);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));

    // Long Back from Actions leaves straight to Ring view.
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Ring -> Overview
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);   // Overview -> Actions
    CHECK_EQ(cp(ui).actions_mode(), true);
    ui.on_button(Button::Back, ButtonEvent::Long, t += 50);
    CHECK_EQ(ui.ring_view(), true);
    CHECK_EQ(label_violation(ui.labels()) == nullptr, true);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// Track 2B: Top "Reset & re-read" section: standalone CC cycle, inline 2-press confirm + disarm on move, self-powered refusal.
static void test_reset_link_actions_standalone_inline_selfpowered() {
    // 1. Standalone charger: single OK cycles link, clears held offers, shows toast, returns to Overview.
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000; g_stub.cc = 1; g_stub.self_powered = false;
        g_stub.caps.valid = true; g_stub.caps.num_pdos = 1;
        g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
        uint32_t t = 20000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        ui.open_place_actions(bench::Place::Charger, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(cp(ui).action_row(), 0);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
        settle(ui, t);
        CHECK_EQ(cp(ui).overview_mode(), true);
        CHECK_STR(ui.context().toast.view().data(), "Rescanning...");
    }

    // 2. Inline link: first OK arms (Confirm verb, toast); moving to Row 1 disarms; 2nd OK triggers HardReset -> Overview.
    {
        Ui &ui = fresh_ui();
        make_inline(ui, 30000);
        g_stub.self_powered = false;
        uint32_t t = 31000;
        ui.open_place_actions(bench::Place::Charger, t);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_EQ(cp(ui).action_row(), 0);
        CHECK_STR(ok_label(ui), "Reset");
        // First OK arms inline confirmation.
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_STR(ok_label(ui), "Confirm");
        CHECK_EQ(label_violation(ui.labels()) == nullptr, true);
        CHECK_STR(ui.context().toast.view().data(), "OK again: resets link");
        // Moving focus off Row 0 disarms it.
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
        CHECK_EQ(cp(ui).action_row(), 1);
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 50);
        CHECK_EQ(cp(ui).action_row(), 0);
        CHECK_STR(ok_label(ui), "Reset");
        // Arm again and press OK within confirm window -> HardReset -> returns to Overview.
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
        CHECK_STR(ok_label(ui), "Confirm");
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).overview_mode(), true);
        CHECK_STR(ui.context().toast.view().data(), "Resetting link...");
    }

    // 3. Self-powered K2: refuses with "Power K2 via USB port" toast and stays on Actions.
    {
        Ui &ui = fresh_ui();
        make_inline(ui, 40000);
        g_stub.self_powered = true;
        uint32_t t = 41000;
        ui.open_place_actions(bench::Place::Charger, t);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
        settle(ui, t);
        CHECK_EQ(cp(ui).actions_mode(), true);
        CHECK_STR(ui.context().toast.view().data(), "Power K2 via USB port");
        CHECK_EQ(int(ui.context().toast.tone), int(FooterTone::Caution));
    }
}

static int count_px(int y0, int y1, int x0, int x1, uint16_t col) {
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (g_panel[y * LCD_WIDTH + x] == col) n++;
    return n;
}

// The dim ink of one Overview slice's chip row (only a provenance letter ever had it) as its left x and bottom y
// relative to the slice top, and the gap to the hero's last kInk column. False when the row has none.
struct TagInk { int left = 0, bottom = 0, gap = 0; };
static bool tag_ink(int slice_y, TagInk &t) {
    const int x0 = kOppRailThickness, x1 = x0 + 208;
    int left = 1 << 20, bottom = -1, hero_right = -1;
    for (int y = slice_y + 22; y < slice_y + 43; y++)
        for (int x = x0; x < x1; x++)
            if (g_panel[y * LCD_WIDTH + x] == kDim) { left = std::min(left, x); bottom = std::max(bottom, y); }
    for (int y = slice_y + 19; y < slice_y + 43; y++)
        for (int x = x0; x < left && x < x1; x++)
            if (g_panel[y * LCD_WIDTH + x] == kInk) hero_right = std::max(hero_right, x);
    if (bottom < 0) return false;
    t = {left - x0, bottom - slice_y, left - hero_right};
    return true;
}

// Track 2B: Overview shows no provenance letter (data and remote keep it), PEAK chip, bottleneck Cable highlight,
// and Cable Res. row.
static void test_overview_provenance_throttled_and_bottleneck() {
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.epr = true; g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 28000, 28000, 5000, 140000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 2;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.em.valid = true; g_stub.em.vid = 0x0451; g_stub.em.pid = 0x1234; g_stub.em.vendor_str = "TI";
    g_stub.em.speed_str = "USB4 Gen3"; g_stub.em.latency_str = "<10 ns (~1 m)";
    g_stub.em.max_current_a = 3; g_stub.em.max_voltage_v = 20; g_stub.em.max_power_w = 60;
    // Feed a passive cable resistance measurement (~112 mOhm).
    ui.context().cable.sample(20000, 100);
    ui.context().cable.sample(19776, 2100);
    CHECK(ui.context().cable.estimate().has_value());

    uint32_t t = 51000;
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    ui.open_place(bench::Place::Charger, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK_EQ(int(held::offer_source()), int(pd::OfferSource::Sniffed));
    // Slice 1 (Cable, unfocused) is a bottleneck (60W cable < 140W charger): 3 px kCaution bar + kCautionT ground.
    const int x_bar = kOppRailThickness + 1;
    CHECK_EQ(g_panel[90 * LCD_WIDTH + x_bar], kCaution);
    CHECK_EQ(g_panel[155 * LCD_WIDTH + kOppRailThickness + 10], kCautionT);
    // No slice draws the provenance letter (dim ink in the chip row), and a clean link has no amber chip.
    TagInk ti;
    const int sy[3] = {2, 81, 160};
    for (int i = 0; i < 3; i++) CHECK(!tag_ink(sy[i], ti));
    CHECK_EQ(count_px(sy[0] + 24, sy[0] + 38, kOppRailThickness, kOppRailThickness + 208, kCaution), 0);
    // The sink slice keeps its OK chip.
    CHECK(count_px(sy[2] + 24, sy[2] + 38, kOppRailThickness, kOppRailThickness + 208, kOkT) > 0);

    // Throttle charger from 140W down to 60W and settle > 800 ms -> PEAK / offer_throttled().
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 3000, 60000};
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(held::offer_throttled(), true);
    CHECK_EQ(held::live_offer_mw(), 60000u);
    // PEAK is an exception chip and stays.
    CHECK(count_px(2 + 24, 2 + 38, kOppRailThickness, kOppRailThickness + 208, kOkT) > 0);
    CHECK(!tag_ink(2, ti));

    // Cable Spec view includes measured Res. row.
    ui.goto_page(kConnTabAlias + 1, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).spec_mode(), true);
    CHECK(cp(ui).result_rows() >= 8);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {};
}

// The PD limit shows as an amber LIM chip on the Charger slice while armed, and goes with it.
static void test_charger_slice_lim_chip_while_armed() {
    Ui &ui = fresh_ui();
    make_inline(ui, 50000);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.caps = {};
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 2;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.lim = {};
    uint32_t t = 51000;
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    ui.open_place(bench::Place::Charger, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    const int x0 = kOppRailThickness, x1 = x0 + 208;
    CHECK_EQ(count_px(2 + 24, 2 + 38, x0, x1, kCautionT), 0);
    g_stub.lim.state = pd::LimState::Armed; g_stub.lim.limit_mw = 100000;   // the widest text: LIM 100W
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK(count_px(2 + 24, 2 + 38, x0, x1, kCautionT) > 0);
    g_stub.lim.state = pd::LimState::Off;
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(count_px(2 + 24, 2 + 38, x0, x1, kCautionT), 0);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// Bring up the sniffed Request scene of the provenance test (20V 3A PD contract, caps, cable e-marker) on 6 LINK.
static void inline_pd_scene(Ui &ui, uint32_t &t) {
    make_inline(ui, t);
    g_stub.pm_online = true;
    g_stub.pd_seen_attach = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 3000, 60000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 2;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 3000;
    g_stub.em.valid = true; g_stub.em.vid = 0x0451; g_stub.em.pid = 0x1234; g_stub.em.vendor_str = "TI";
    g_stub.em.speed_str = "USB4 Gen3"; g_stub.em.latency_str = "<10 ns (~1 m)";
    g_stub.em.max_current_a = 3; g_stub.em.max_voltage_v = 20; g_stub.em.max_power_w = 60;
    t += 1000;
    for (int i = 0; i < 12; i++) ui.tick(t += 100);
    ui.open_place(bench::Place::Charger, t += 100);
    settle(ui, t);
}

// The K2 counts as the sink only when nothing is inline: fusb302::self_powered() is the boot pin latch, which stays set
// while a phone sits behind the K2 (the driver opens the switches, the latch stays).
static void test_inline_sink_is_not_the_k2_with_boot_pin_latched() {
    for (int latched = 0; latched < 2; latched++) {
        Ui &ui = fresh_ui();
        uint32_t t = 90000;
        inline_pd_scene(ui, t);
        g_stub.self_powered = latched != 0;
        g_stub.pin = latched ? 2 : 0;
        for (int i = 0; i < 6; i++) ui.tick(t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).overview_mode(), true);
        CHECK_EQ(ui.link().inline_mode(), true);
        const int x0 = kOppRailThickness, x1 = x0 + 208;
        // The sniffed Request slice has the green OK chip and no solid SELF chip (kChip fill) in its chip row.
        CHECK(count_px(160 + 24, 160 + 38, x0, x1, kOkT) > 0);
        CHECK_EQ(count_px(160 + 24, 160 + 38, x0, x1, kChip), 0);
    }
    g_stub.self_powered = false;
    g_stub.pin = 0;
    g_stub.em = {};
}

// Inline power fields populate on 6 LINK Overview even when K2 attached mid-session (no PD caps / Request sniffed yet),
// and all 3 slices show W hero + W/V/A across full PD, plain 3A, bottlenecked cable, and live-inline scenes with 0 truncation.
static void test_inline_overview_power_fields_and_w_hero_slices() {
    auto count_ink = [](int y0, int y1) {
        int ink = 0;
        for (int y = y0; y < y1; y++)
            for (int x = kOppRailThickness + 4; x < LCD_WIDTH - 4; x++)
                if (g_panel[y * LCD_WIDTH + x] != kBg) ink++;
        return ink;
    };
    // 1. Mid-session inline link (20V dock + laptop at 2.4A = 48W, NO caps and NO Request sniffed yet).
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true;
        g_stub.cc = 1;
        g_stub.pm.vbus_mv = 19838;
        g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 19838;
        g_stub.pm.current_ma = 2400;
        g_stub.pm.power_mw = 47611;
        uint32_t t = 60000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link().inline_mode(), true);
        // Settle > 1.5 s so pd_verdict becomes None (missed handshake) — Overview must STILL show the live power.
        for (int i = 0; i < 20; i++) ui.tick(t += 100);
        ui.open_place(bench::Place::Charger, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).overview_mode(), true);
        // Slice 0 (Charger, y 2..80) and Slice 2 (Sink, y 160..239) both have a bright hero.
        CHECK(count_ink(20, 44) > 50);
        CHECK(count_ink(178, 202) > 50);
        // An inline link is a normal state: no amber LIVE / INLINE chip on either slice.
        CHECK_EQ(count_px(2 + 24, 2 + 38, kOppRailThickness + 4, LCD_WIDTH, kCaution), 0);
        CHECK_EQ(count_px(160 + 24, 160 + 38, kOppRailThickness + 4, LCD_WIDTH, kCaution), 0);
    }
    // 2. Full PD link (96W dock, 100W cable, 96W laptop Request) + plain 3A cable (60W) + Request without caps across 4 rotations.
    {
        Ui &ui = fresh_ui();
        make_inline(ui, 70000);
        g_stub.pm_online = true; g_stub.pd_seen_attach = true;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.num_pdos = 2;
        g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
        g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 4800, 96000};
        g_stub.caps.rx_count++;
        g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 2;
        g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 4800;
        g_stub.em.valid = true; g_stub.em.vid = 0x291A; g_stub.em.pid = 0x0001; g_stub.em.vendor_str = "Anker";
        g_stub.em.max_current_a = 5; g_stub.em.max_voltage_v = 20; g_stub.em.max_power_w = 0;   // derived 100W hero
        g_stub.em.speed_str = "USB4 Gen3"; g_stub.em.latency_str = "<10 ns (~1 m)";
        uint32_t t = 71000;
        for (int i = 0; i < 12; i++) ui.tick(t += 100);
        for (uint8_t rot = 0; rot < 4; rot++) {
            ui.set_rotation(rot, t += 50);
            ui.open_place(bench::Place::Charger, t += 50);
            settle(ui, t);
            CHECK_EQ(cp(ui).overview_mode(), true);
        }
        // Request seen without Source_Capabilities: match_compute must preserve want_mv & want_mw.
        pd::SourceCaps no_caps{};
        pd::Contract c{};
        c.valid = true; c.pos = 5; c.mv = 20000; c.op_ma = 4800; c.max_ma = 4800;
        const MatchResult mr = match_compute({no_caps, c, Tri::Unknown, {}, 0});
        CHECK_EQ(mr.request_seen, true);
        CHECK_EQ(mr.want_mv, 20000u);
        CHECK_EQ(mr.want_ma, 4800u);
        CHECK_EQ(mr.want_mw, 96000u);
    }
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// Ui::link_reset handles mid-session inline links (low draw, no Request sniffed), K2-owned contracts, and standalone rescans.
static void test_link_reset_mid_session_inline_and_k2_contract() {
    // 1. Mid-session 20V inline link with low draw (< 50 mA, no Request sniffed): link.inline_mode() is false,
    //    yet link_reset_inline() detects the live external link and sends inline Hard Reset after 2-press confirm.
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true;
        g_stub.cc = 1;
        g_stub.pm.vbus_mv = 19838;
        g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 19838;
        g_stub.pm.current_ma = 10;
        g_stub.self_powered = false;
        uint32_t t = 80000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link().attached(), true);
        CHECK_EQ(ui.link().inline_mode(), false);
        CHECK_EQ(ui.link_reset_inline(), true);
        CHECK_EQ(int(ui.link_reset(false, t += 50)), int(Ui::LinkResetResult::NeedConfirm));
        CHECK_EQ(g_stub.hard_resets, 0);
        CHECK_EQ(int(ui.link_reset(true, t += 50)), int(Ui::LinkResetResult::HardReset));
        CHECK_EQ(g_stub.hard_resets, 1);
        CHECK_EQ(g_stub.lab_hard_resets, 1);
    }
    // 2. Mid-session 5V inline link with Rp/Rd divider in sink band on active CC while K2 is not pulling Rd.
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true;
        g_stub.cc = 1;
        g_stub.pm.vbus_mv = 5000;
        g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 5000;
        g_stub.pm.current_ma = 10;
        g_stub.self_powered = false;
        adc::Reading r; r.cc1_mv = 1500; r.cc2_mv = 0;
        g_stub.adc = r;
        uint32_t t = 82000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link().inline_mode(), false);
        CHECK_EQ(ui.link_reset_inline(), true);
        CHECK_EQ(int(ui.link_reset(false, t += 50)), int(Ui::LinkResetResult::NeedConfirm));
        CHECK_EQ(int(ui.link_reset(true, t += 50)), int(Ui::LinkResetResult::HardReset));
        CHECK_EQ(g_stub.lab_hard_resets, 1);
    }
    // 3. Standalone K2-owned PD contract (SinkState::Ready, non-bus-powered): single press sends sink Hard Reset (lab_inline=false).
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true; g_stub.cc = 1; g_stub.pm.vbus_mv = 9000;
        g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 9000;
        g_stub.self_powered = false;
        g_stub.sink.state = pd::SinkState::Ready;
        g_stub.sink.mv = 9000;
        uint32_t t = 84000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link_reset_inline(), false);
        CHECK_EQ(int(ui.link_reset(false, t += 50)), int(Ui::LinkResetResult::HardReset));
        CHECK_EQ(g_stub.hard_resets, 1);
        CHECK_EQ(g_stub.lab_hard_resets, 0);
    }
    // 4. Standalone K2-owned PD contract while bus-powered: falls back to safe PdRescan (Cycled), never unbridged Hard Reset.
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true; g_stub.cc = 1; g_stub.pm.vbus_mv = 9000;
        g_stub.self_powered = true;
        g_stub.sink.state = pd::SinkState::Ready;
        uint32_t t = 86000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link_reset_inline(), false);
        CHECK_EQ(int(ui.link_reset(false, t += 50)), int(Ui::LinkResetResult::Cycled));
        CHECK_EQ(g_stub.hard_resets, 0);
    }
    // 5. Standalone charger with open Rp (~3300 mV, no downstream sink): stays standalone PdRescan (Cycled).
    {
        Ui &ui = fresh_ui();
        g_stub.pm_online = true; g_stub.cc = 1; g_stub.pm.vbus_mv = 5000;
        g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 5000;
        g_stub.self_powered = false;
        adc::Reading r; r.cc1_mv = 3300; r.cc2_mv = 0;
        g_stub.adc = r;
        uint32_t t = 88000;
        for (int i = 0; i < 4; i++) ui.tick(t += 100);
        CHECK_EQ(ui.link_reset_inline(), false);
        CHECK_EQ(int(ui.link_reset(false, t += 50)), int(Ui::LinkResetResult::Cycled));
        CHECK_EQ(g_stub.hard_resets, 0);
    }
}

// 7 TOOLS sits right after 6 LINK with Request voltage & Fast charge at the top, preserving existing Menu wire IDs.
static void test_tools_ring_position_and_triggers() {
    static_assert(static_cast<uint8_t>(Menu::Display) == 19, "existing Menu wire IDs must not shift");
    static_assert(static_cast<uint8_t>(Menu::Tools) == 20, "Menu::Tools appended right before Menu::Count");
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    ui.goto_page(uint8_t(Page::Home), t);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);   // ring view on 1 HOME
    for (int i = 0; i < 5; i++) ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Link));
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Tools));
    CHECK_EQ(ui.menu() == Menu::Tools, true);
    CHECK_EQ(ui.ring_view(), true);
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<uint8_t>(Menu::Tools));
    // Enter 7 TOOLS -> Row 0 = Request voltage (PD trigger), Row 1 = Fast charge (FC trigger).
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    settle(ui, t);
    CHECK_EQ(ui.ring_view(), false);
    CHECK_EQ(ui.context().keep.tools_sel, 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.menu() == Menu::PdTrigger, true);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.menu() == Menu::Tools, true);
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.context().keep.tools_sel, 1);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.menu() == Menu::Trigger, true);
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.menu() == Menu::Tools, true);
    // Short Back from entered 7 TOOLS returns to ring view on Pos::Tools; Plus moves to 8 BATTERY.
    ui.on_button(Button::Back, ButtonEvent::Short, t += 50);
    CHECK_EQ(ui.ring_view(), true);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Tools));
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 50);
    CHECK_EQ(int(ui.ring_position()), int(ring::Pos::Battery));
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
}

// ---- a session ends with its charger: stale slices on a detach, a clean start on the next attach ----

// One Overview slice's ink: the LAST pill (kStale fill) in the chip row, the amber and green chips, and the hero's full ink.
struct SliceInk { int last, amber, green, hero; };
static SliceInk slice_ink(int slice) {
    const int sy = slice == 0 ? 2 : slice == 1 ? 81 : 160, x0 = kOppRailThickness, x1 = x0 + 208;
    return {count_px(sy + 24, sy + 38, x0, x1, kStale),
            count_px(sy + 1, sy + 77, x0, x1, kCaution) + count_px(sy + 1, sy + 77, x0, x1, kCautionT),
            count_px(sy + 24, sy + 38, x0, x1, kOkT), count_px(sy + 19, sy + 43, x0, x1, kInk)};
}

// The charger and its cable go away (VBUS and CC down, the driver forgets the source, the far Request and the cable).
static void unplug_charger(Ui &ui, uint32_t &t) {
    g_stub.cc = 0; g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0;
    g_stub.caps = {}; g_stub.contract = {}; g_stub.em = {}; g_stub.cable_full = {}; g_stub.cable_ans = {};
    for (int i = 0; i < 80; i++) ui.tick(t += 100);   // past the inline hold
    CHECK_EQ(int(ui.link().attached()), 0);
    settle(ui, t);
}

// The limit stays armed across a detach, but its LIM chip is per-attach: the stale Charger slice shows LAST, no amber.
static void test_stale_charger_slice_drops_lim_chip() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    inline_pd_scene(ui, t);
    g_stub.lim = {};
    g_stub.lim.state = pd::LimState::Limited; g_stub.lim.limit_mw = 100000;
    for (int i = 0; i < 6; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    CHECK(slice_ink(0).amber > 0);                       // live: the LIM chip
    unplug_charger(ui, t);
    CHECK_EQ(g_stub.lim.state, pd::LimState::Limited);   // the limit itself is untouched
    const SliceInk k = slice_ink(0);
    CHECK(k.last > 0);
    CHECK_EQ(k.amber, 0);
}

// A detach ends the session: the held offers and cable stay readable but grey, a LAST pill, no amber and no green.
static void test_overview_slices_go_stale_on_detach() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    inline_pd_scene(ui, t);
    g_stub.cable_seq++;                                  // the cable's SOP' exchange was decoded: a scanned cable
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    for (int i = 0; i < 3; i++) {
        const SliceInk k = slice_ink(i);
        CHECK_EQ(k.last, 0);
        CHECK(k.hero > 0);
    }
    unplug_charger(ui, t);
    CHECK_EQ(held::shown_caps().valid, true);            // the offers are still held
    CHECK_EQ(held::cable_em().valid, true);
    for (int i = 0; i < 2; i++) {                        // Charger and Cable
        const SliceInk k = slice_ink(i);
        CHECK(k.last > 0);
        CHECK_EQ(k.amber, 0);
        CHECK_EQ(k.green, 0);
        CHECK_EQ(k.hero, 0);                             // the hero is grey, not full ink
    }
    CHECK_EQ(slice_ink(2).green, 0);
    {   // one LAST pill, not a leftover chip's text with LAST appended
        const Surface s{0, 0, LCD_WIDTH, LCD_HEIGHT};
        gfx_fill(s, 0, 100, LCD_WIDTH, 20, kBg);
        kit::draw_chip(s, 60, 102, "LAST", kStale, kBg);
        const int ref = count_px(100, 120, 0, LCD_WIDTH, kStale);
        CHECK_EQ(slice_ink(0).last, ref);
    }
    // The same charger comes back: the slices are live again once caps arrive (nothing grey is left behind).
    g_stub.cc = 1; g_stub.pm.vbus_mv = 9000; g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    g_stub.session++;
    for (int i = 0; i < 4; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(slice_ink(0).last, 0);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    g_stub.em = {}; g_stub.caps = {}; g_stub.contract = {};
}

// A new attach starts every slice from nothing at once: no offers, no cable, no Request of the last charger, whether or
// not caps ever arrive (a plain source) and without waiting for a session change.
static void test_new_attach_drops_the_last_session() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    inline_pd_scene(ui, t);
    g_stub.cable_seq++;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    unplug_charger(ui, t);
    CHECK_EQ(held::shown_caps().valid, true);
    g_stub.cc = 1; g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;   // a plain 5 V source: no caps follow
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(int(ui.link().attached()), 1);
    CHECK_EQ(held::shown_caps().valid, false);          // dropped at the edge, before any new caps
    CHECK_EQ(held::cable_em().valid, false);            // the CC detach made the cable stale
    CHECK_EQ(int(held::offer_source()), int(pd::OfferSource::None));
    for (int i = 0; i < 80; i++) ui.tick(t += 100);     // past the caps wait: the old list never comes back
    settle(ui, t);
    CHECK_EQ(held::shown_caps().valid, false);
    for (int i = 0; i < 3; i++) {
        const SliceInk k = slice_ink(i);
        CHECK_EQ(k.last, 0);
        CHECK_EQ(k.green, 0);
    }
    g_stub.em = {}; g_stub.caps = {}; g_stub.contract = {};
}

// A cable read on its own, after the charger left, is a fresh read: it is not grey, and the next attach keeps it.
static void test_standalone_cable_read_is_not_stale() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    inline_pd_scene(ui, t);
    g_stub.cable_seq++;
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    unplug_charger(ui, t);
    g_stub.em.valid = true; g_stub.em.vid = 0x0451; g_stub.em.pid = 0x1234; g_stub.em.vendor_str = "TI";
    g_stub.em.speed_str = "USB4 Gen3"; g_stub.em.latency_str = "<10 ns (~1 m)";
    g_stub.em.max_current_a = 3; g_stub.em.max_voltage_v = 20; g_stub.em.max_power_w = 60;
    g_stub.cable_seq++;                                 // the K2 read the cable itself (VCONN, no charger)
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(int(ui.link().cable_stale()), 0);
    CHECK_EQ(slice_ink(1).last, 0);
    CHECK(slice_ink(1).hero > 0);
    g_stub.cc = 1; g_stub.pm.vbus_mv = 5000;           // a charger arrives on the same cable
    for (int i = 0; i < 3; i++) ui.tick(t += 100);
    CHECK_EQ(held::cable_em().valid, true);
    g_stub.em = {}; g_stub.caps = {}; g_stub.contract = {};
}

// The inline sink leaves while the charger stays: the last Request is a past one (LAST, no OK chip) until a sink asks again.
static void test_sink_slice_goes_stale_when_the_sink_leaves() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    inline_pd_scene(ui, t);
    CHECK_EQ(slice_ink(2).last, 0);
    CHECK(slice_ink(2).green > 0);
    g_stub.pm.current_ma = 0;                           // the phone is unplugged; the charger's VBUS stays
    for (int i = 0; i < 70; i++) ui.tick(t += 100);     // past the inline quiet window
    CHECK_EQ(int(ui.link().attached()), 1);
    CHECK_EQ(int(ui.link().inline_mode()), 0);
    settle(ui, t);
    CHECK(slice_ink(2).last > 0);
    CHECK_EQ(slice_ink(2).green, 0);
    CHECK_EQ(slice_ink(2).amber, 0);
    CHECK_EQ(slice_ink(2).hero, 0);
    g_stub.pm.current_ma = 2000;                        // another device draws behind the K2: a live session again
    for (int i = 0; i < 4; i++) ui.tick(t += 100);
    settle(ui, t);
    CHECK_EQ(int(ui.link().inline_mode()), 1);
    CHECK_EQ(slice_ink(2).last, 0);
    g_stub.em = {}; g_stub.caps = {}; g_stub.contract = {};
}

// The Charger slice's revision chip is the wire revision the other views name (PD3.x for a rev 2 source), not a guessed PD3.0.
static void test_charger_slice_revision_chip_names_the_wire_revision() {
    Ui &ui = fresh_ui();
    uint32_t t = 90000;
    attach_charger(ui, t);                               // spec_rev 2 caps, no EPR
    ui.open_place(bench::Place::Charger, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).overview_mode(), true);
    const int x0 = kOppRailThickness, x1 = x0 + 208;
    const int ink = count_px(2 + 24, 2 + 38, x0 + 4, x1, kChip);   // fill minus glyph ink tells "x" from "0"; x0+4 clears the focus bar
    CHECK(ink > 0);
    const Surface s{0, 0, LCD_WIDTH, LCD_HEIGHT};
    auto ref_ink = [&](const char *txt) {
        gfx_fill(s, 0, 100, LCD_WIDTH, 20, kBg);
        kit::draw_chip(s, 60, 102, txt, kChip, kOnChip);
        return count_px(100, 120, 0, LCD_WIDTH, kChip);
    };
    CHECK_EQ(ink, ref_ink(pd::wire_revision_name(2)));
    CHECK(ink != ref_ink("PD3.0"));
    g_stub.caps = {};
}

// The Charger tab leads with the four offer groups (SPR, EPR, PPS, AVS), a dim "none" for a group the source does not offer,
// then who made them (ID). A 100 W bank: no EPR, no AVS.
static void test_source_tab_offer_groups_bank() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 6;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 12000, 12000, 3000, 36000};
    g_stub.caps.pdos[3] = {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000};
    g_stub.caps.pdos[4] = {pd::PdoType::Fixed, 0, 20000, 20000, 3000, 60000};
    g_stub.caps.pdos[5] = {pd::PdoType::Pps, 0, 5000, 21000, 3000, 63000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    uint32_t t = 21000;
    ui.tick(t);
    set_id(g_stub.vdm, 0x2B01, 0x0000, "Unknown");
    show(ui, 0, t);
    CHECK(place_row_split_is(0, "5/9/12/15/20V", " 3A", kVolt, kAmp));
    CHECK(place_row_value_is(1, "none", kFaint));
    CHECK(place_row_split_is(2, "5-21V", " 3A", kVolt, kAmp));
    CHECK(place_row_value_is(3, "none", kFaint));
    g_stub.vdm = {}; g_stub.caps = {};
}

// A 140 W EPR source: the extra offers split by kind, the AVS range is rated in watts.
static void test_source_tab_offer_groups_epr() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 10; g_stub.caps.epr = true;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000};
    g_stub.caps.pdos[3] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.pdos[4] = {pd::PdoType::Pps, 0, 3300, 21000, 5000, 105000};
    g_stub.caps.pdos[7] = {pd::PdoType::Fixed, 0, 28000, 28000, 5000, 140000};
    g_stub.caps.pdos[8] = {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    uint32_t t = 21000;
    ui.tick(t);
    set_id(g_stub.vdm, 0x2B01, 0x0000, "Unknown");
    show(ui, 0, t);
    CHECK(place_row_split_is(0, "5/9/15V", " 3A", kVolt, kAmp));
    CHECK(place_row_split_is(1, "20V", " 5A", kVolt, kAmp));
    CHECK(place_row_split_is(2, "28V", " 5A", kVolt, kAmp));
    CHECK(place_row_split_is(3, "3.3-21V", " 5A", kVolt, kAmp));
    CHECK(place_row_split_is(4, "15-28V", " 140W", kVolt, kWatt));
    g_stub.vdm = {}; g_stub.caps = {};
}

// The headline watts follow the extended caps (PDP): the cheap page signature must see them arrive and change, or the hero
// would stay on the best offer until something else repaints the page.
static uint64_t hero_hash() {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < 46; y++)
        for (int x = 16; x < 224; x++) h = (h ^ g_panel[y * LCD_WIDTH + x]) * 1099511628211ull;
    return h;
}

static void test_source_tab_hero_follows_the_pdp() {
    Ui &ui = fresh_ui();
    make_inline(ui, 20000);
    g_stub.pm_online = true;
    g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 20000, 20000, 3000, 60000};
    g_stub.caps.raw_header = 0x1181; g_stub.caps.rx_count++;
    g_stub.pd_seen_attach = true;
    uint32_t t = 21000;
    ui.tick(t);
    show(ui, 0, t);
    const uint64_t best = hero_hash();
    auto settle = [&] {
        for (int i = 0; i < 12; i++) ui.tick(t += 100);
        while (ui.painting()) ui.tick(t += 2);
    };
    g_stub.ext.caps_ext.valid = true; g_stub.ext.caps_ext.pdp_w = 65;
    settle();
    const uint64_t pdp65 = hero_hash();
    CHECK(pdp65 != best);
    g_stub.ext.caps_ext.pdp_w = 100;
    settle();
    CHECK(hero_hash() != pdp65);
    g_stub.ext.caps_ext = {};
    settle();
    CHECK(hero_hash() == best);
    g_stub.vdm = {}; g_stub.caps = {};
}

int main() {
    test_source_tab_hero_follows_the_pdp();
    test_source_tab_offer_groups_bank();
    test_source_tab_offer_groups_epr();
    test_auto_wake_once_on_the_charger_overview();
    test_auto_wake_settles_then_toasts();
    test_auto_wake_waits_for_the_line_trial();
    test_auto_wake_not_on_other_tabs_or_views();
    test_auto_wake_honours_the_auto_read_block();
    test_auto_wake_needs_the_live_facts_too();
    test_sink_tab_pd_body_shows_the_raw_vooc_note();
    test_sink_tab_no_pd_shows_the_raw_vooc_note();
    test_sink_tab_offers_identify_when_a_device_is_alone();
    test_identify_log_keeps_fixed_text_in_the_format();
    test_survey_nothing_active_before_refresh();
    test_survey_refresh_item_starts_it();
    test_survey_status_line();
    test_auto_read_gate();
    test_auto_read_states_all_rotations();
    test_survey_details_rows_follow_the_survey();
    test_refresh_goes_through_the_rescan_module();
    test_the_rescan_comeback_lands_on_the_source_tab();
    test_live_request_repaints_only_its_rows();
    test_worst_case_rows_fit();
    test_cable_tab_answered_row();
    test_cable_full_rows();
    test_charger_tab_keeps_the_offers();
    test_cable_tab_keeps_a_read_cable();
    test_empty_pages_say_what_fills_them();
    test_each_tab_shows_its_own_side();
    test_sink_tab_not_empty_when_a_sink_was_seen();
    test_source_tab_leaves_waiting_after_timeout();
    test_source_tab_typec_advertisement();
    test_source_and_sink_tabs_say_in_use_when_possibly_inline();
    test_source_tab_legacy_row_skips_pd_verdicts();
    test_sink_tab_omits_empty_data_row();
    test_sink_tab_pwr_row_is_a_budget_without_pd();
    test_proto_row_survives_the_row_cap();
    test_data_row_only_when_it_adds_something();
    test_non_displayed_inputs_do_not_run_match_live();
    test_place_aliases_pick_tab();
    test_topology_from_the_cc_lines();
    test_passive_facts_are_the_pages_facts();
    test_topology_unknown_unless_passive_and_dead();
    test_empty_states_are_passive();
    test_place_items_per_tab();
    test_cable_place_has_spoof_row();
    test_tools_spoof_shortcut_opens_the_cable_tab_list();
    test_sink_id_row_marks_a_sniffed_identity();
    test_sink_tab_no_pd_offered_unanswered();
    test_source_tab_no_pd_never_offered_all_rotations();
    test_widest_vendor_all_rotations();
    test_widest_vendor_identify_all_rotations();
    test_pd_present_keeps_identify_offered();
    test_single_tools_lock_one_tab();
    test_tools_rows_open_single_tools();
    test_svooc_trigger_rise_is_not_a_sink();
    test_device_place_inline_interrogate_and_readout();
    test_device_place_inline_interrogate_with_sink_pdos();
    test_device_place_standalone_and_self_powered_identify();
    test_place_two_page_info_and_actions();
    test_cable_identity_positive_chips_and_no_trunc();
    test_device_place_telemetry_rows_and_live_repaints();
    test_cable_read_emarker_ux_paths();
    test_device_scroll_thumb_reaches_bottom();
    test_device_scroll_toast_stays_on_top();
    test_overview_3slice_focus_and_navigation();
    test_reset_link_actions_standalone_inline_selfpowered();
    test_overview_provenance_throttled_and_bottleneck();
    test_inline_sink_is_not_the_k2_with_boot_pin_latched();
    test_inline_overview_power_fields_and_w_hero_slices();
    test_link_reset_mid_session_inline_and_k2_contract();
    test_tools_ring_position_and_triggers();
    test_overview_slices_go_stale_on_detach();
    test_new_attach_drops_the_last_session();
    test_standalone_cable_read_is_not_stale();
    test_sink_slice_goes_stale_when_the_sink_leaves();
    test_charger_slice_revision_chip_names_the_wire_revision();
    test_charger_slice_lim_chip_while_armed();
    test_stale_charger_slice_drops_lim_chip();
    return test_report("test_ui_connection_page");
}
