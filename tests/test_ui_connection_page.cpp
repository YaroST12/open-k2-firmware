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
    ui.goto_page(kConnTabAlias, 20000);                              // Charger place
    ui.tick(20050);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Source));
    CHECK(cp(ui).item_count() >= 2);
    CHECK_EQ(cp(ui).item(0).enabled, true);                          // Refresh is always available
    ui.goto_page(kConnTabAlias + 1, 20300);                          // Cable place
    ui.tick(20350);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    CHECK(cp(ui).item_count() >= 4);                                 // Read, Save, Saved cables (no Spoof as...)
    for (int i = 0; i < cp(ui).item_count(); i++)
        CHECK(cp(ui).item(i).act != conn::Act::SpoofAs);
    ui.goto_page(kConnTabAlias + 2, 20450);                          // Device place
    ui.tick(20500);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK(cp(ui).item_count() >= 1);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::Identify));     // OK explicitly interrogates/asks the device
    CHECK_EQ(cp(ui).item(0).enabled, false);                         // no device attached
    for (int i = 1; i < cp(ui).item_count(); i++) {
        CHECK(cp(ui).item(i).act != conn::Act::Identify);            // no "Ask device" button row
        CHECK(cp(ui).item(i).act != conn::Act::Refresh);             // no "Read again" button row
    }
    ui.on_button(Button::Back, ButtonEvent::Short, 20600);           // BACK leaves Connection
    CHECK_EQ(on_pd(ui), 0);
}

static void test_cable_place_has_no_spoof_row() {
    for (bool inl : {false, true}) {
        Ui &ui = fresh_ui();
        if (inl) make_inline(ui, 20000);
        ui.goto_page(kConnTabAlias + 1, 20100);                      // Cable place
        ui.tick(20150);
        CHECK(cp(ui).item_count() >= 4);
        CHECK_EQ(int(cp(ui).item(1).act), int(conn::Act::ReadCable));
        CHECK_EQ(int(cp(ui).item(2).act), int(conn::Act::SaveCable));
        CHECK_EQ(int(cp(ui).item(3).act), int(conn::Act::SavedCables));
        for (int i = 0; i < cp(ui).item_count(); i++)
            CHECK(cp(ui).item(i).act != conn::Act::SpoofAs);
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

// Place-page list geometry (ui_connection_page.cpp): rows from kit::kPlaceListY (68), pitch 20, value at x 64, 4 px scroll lane at right.
static bool place_row_value_is(int idx, const char *text, uint16_t colour = kText) {
    constexpr int kValX = 64, kPitch = 20, kRowY0 = kit::kPlaceListY;
    constexpr int W = 220 - kValX;
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
    uint32_t t = 20000;
    show(ui, 2, t);                                     // Sink tab, a sink was seen on CC, no charger attached
    CHECK_EQ(cp(ui).result_rows(), 6);                  // no Data row: nothing is known about the data line
    g_stub.ul.family = usblink::Family::Usb32; g_stub.ul.mbps = 5000;
    g_stub.ul.conf = usblink::Confidence::Negotiated;
    ui.tick(t += 100);
    show(ui, 2, t);
    CHECK_EQ(cp(ui).result_rows(), 6);                  // a plain "5G" would only repeat the USB row
    g_stub.ul = {}; g_stub.contract = {};
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
    g_stub.cable_ans = emk_variant::answered_from(2, 1);   // "B, PD3, SVDM 1.0": the ninth row (VCONN required) of the Cable tab
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

// Refresh (OK with no action row focused on the Charger place) starts the survey.
static void test_survey_refresh_item_starts_it() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    show(ui, 0, t);
    g_stub.rescan_result = rescan::Result::Asked;
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.survey_refreshes, 1);
    CHECK_EQ(int(g_stub.survey_last), int(rescan::Result::Asked));
}

// The status bar: the hint while the open tab has unasked fields, "Reading r/p" during the run, the summary after it.
static void test_survey_status_line() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    attach_charger(ui, t);
    show(ui, 0, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // unfocus action row so footer shows survey status
    settle(ui, t);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), "Source caps not read yet");
    show(ui, 1, t);                                                 // Cable tab: its own field is unasked too
    show(ui, 0, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    g_stub.survey.start(kSurveyCtx, t);
    settle(ui, t);
    char want[40];
    std::snprintf(want, sizeof want, "Reading %u/%u", g_stub.survey.resolved(), g_stub.survey.planned());
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), want);
    g_stub.survey.set(pd::SvField::Caps, pd::SvOutcome::Answered);
    settle(ui, t);
    std::snprintf(want, sizeof want, "Reading %u/%u", g_stub.survey.resolved(), g_stub.survey.planned());
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), want);
    g_stub.survey.abandon();                                        // the run is over: one got, the rest silent
    settle(ui, t);
    CHECK(g_stub.survey.ran());
    StrBuf<40> sum;
    conn::read_summary(g_stub.survey, sum);
    CHECK_STR(std::string(cp(ui).status_line()).c_str(), std::string(sum.view()).c_str());
    CHECK(std::strncmp(cp(ui).status_line().data(), "Read: 1 got", 11) == 0);
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
    ui.goto_page(kConnTabAlias + 2, 13500);                  // Device place
    ui.tick(13550);
    CHECK(cp(ui).item_count() >= 1);
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::Identify));
    CHECK_EQ(cp(ui).item(0).enabled, true);                  // Ask device offered: a device is alone on the K2
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
    // The place's Ask device item is greyed with the same words the page shows.
    CHECK(cp(ui).item_count() >= 1);
    CHECK_EQ(cp(ui).item(0).enabled, false);
    CHECK_STR(cp(ui).item(0).why, "No PD from this device");
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
    CHECK_EQ(int(ui.page()), int(Page::Tools));                       // over the Bench
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));
    CHECK_EQ(cp(ui).action_row(), 0);                                  // the first row: Read e-marker
    CHECK_STR(ok_label(ui), "Read");
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Plus)].shrt, "Dn");
    CHECK_EQ(label_violation(ui.labels()) == nullptr, true);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20100);             // Dn: Save cable
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Cable));                // never a tab switch
    CHECK_EQ(cp(ui).action_row(), 1);
    CHECK_STR(ok_label(ui), "Save");
    for (int i = 0; i < 10; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 20150 + i * 10);
    CHECK_EQ(cp(ui).action_row(), cp(ui).item_count() - 2);            // stops on the last Cable action row
    CHECK_STR(ok_label(ui), "Open");
    for (int i = 0; i < 10; i++) ui.on_button(Button::Minus, ButtonEvent::Short, 20300 + i * 10);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // empty page: clamped on first action row (Read e-marker)
    CHECK_STR(ok_label(ui), "Read");
    ui.on_button(Button::Back, ButtonEvent::Short, 20500);
    CHECK_EQ(int(ui.menu_depth()), 0);
    CHECK_EQ(int(ui.page()), int(Page::Tools));
    CHECK_EQ(ui.context().keep.bench_sel, int(bench::Place::Cable));    // the Bench list, focused on the place just left

    ui.open_place(bench::Place::Charger, 21000);
    ui.tick(21050);
    CHECK_STR(ok_label(ui), "Refresh");                                // Read again (Charger place is info-only)
    ui.on_button(Button::Minus, ButtonEvent::Short, 21060);
    CHECK_EQ(cp(ui).action_row(), 0);                                  // empty page: stays clamped on row 0
    CHECK_STR(ok_label(ui), "Refresh");                                // still Read again
    ui.open_place(bench::Place::Device, 21100);                        // a new place, not a tab switch
    ui.tick(21150);
    CHECK_EQ(int(cp(ui).tab()), int(conn::Tab::Sink));
    CHECK_EQ(int(cp(ui).item(0).act), int(conn::Act::Identify));       // out[0] is Ask device
    CHECK_EQ(cp(ui).item_count(), 1);                                  // no action rows: OK always asks
    CHECK_EQ(cp(ui).action_row(), -1);
    CHECK_STR(ok_label(ui), "Ask");                                    // Ask device
    ui.on_button(Button::Back, ButtonEvent::Short, 21200);
    CHECK_EQ(int(ui.menu_depth()), 0);
}

// The Bench opens the info-only place pages (Charger, Cable, Device) each on its own tab. Active charger tools live
// in Tests -> Charger (opening a tool pushes it over Tests; Test charger replaces both and BACK returns to Tests -> Charger).
// The Spoof shortcut (single tools) lands on Menu::Spoof.
static void test_tools_rows_open_single_tools() {
    Ui &ui = fresh_ui();
    const conn::Tab kTabs[] = {conn::Tab::Source, conn::Tab::Cable, conn::Tab::Sink};
    for (int k = 0; k < 3; k++) {
        ui.open_place(bench::Place::Meter, 20000 + k * 1000);
        for (int i = 0; i < 6; i++) ui.on_button(Button::Minus, ButtonEvent::Short, 20010 + k * 1000 + i * 10);   // Meter row
        for (int i = 0; i <= k; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 20100 + k * 1000 + i * 10);
        ui.on_button(Button::Ok, ButtonEvent::Short, 20500 + k * 1000);
        CHECK_EQ(on_pd(ui), 1);
        if (!on_pd(ui)) return;
        CHECK_EQ(int(cp(ui).tab()), int(kTabs[k]));
        ui.on_button(Button::Back, ButtonEvent::Short, 20600 + k * 1000);
        CHECK_EQ(int(ui.menu_depth()), 0);
    }
    ui.open_tests(TestTab::Charger, 24000);
    CHECK_EQ(int(ui.menu()), int(Menu::Tests));
    ui.on_button(Button::Plus, ButtonEvent::Short, 24050);             // Request voltage
    ui.on_button(Button::Ok, ButtonEvent::Short, 24100);
    CHECK_EQ(int(ui.menu()), int(Menu::PdTrigger));
    CHECK_EQ(int(ui.menu_depth()), 2);                                 // over Tests
    ui.on_button(Button::Back, ButtonEvent::Short, 24200);
    CHECK_EQ(int(ui.menu()), int(Menu::Tests));
    ui.on_button(Button::Minus, ButtonEvent::Short, 24300);            // Test charger
    ui.on_button(Button::Ok, ButtonEvent::Short, 24400);
    CHECK_EQ(int(ui.page()), int(Page::ChargerTest));
    CHECK_EQ(int(ui.menu_depth()), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 24500);
    CHECK_EQ(int(ui.menu()), int(Menu::Tests));
    CHECK_EQ(int(ui.menu_depth()), 1);
    CHECK(ui.open_connection(conn::Tab::Cable, true, 25000));
    CHECK_EQ(int(ui.menu()), int(Menu::Spoof));
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
    ui.open_place(bench::Place::Device, t += 100);
    settle(ui, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // OK = Ask device
    const int before_rows = cp(ui).result_rows();
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
    ui.open_place(bench::Place::Device, t += 100);
    settle(ui, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);   // OK = Ask device
    const int before_rows = cp(ui).result_rows();   // includes Caps: Not seen (1 row)
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
        ui.open_place(bench::Place::Device, t += 100);
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
        ui.open_place(bench::Place::Device, t += 100);
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

static void test_place_unified_one_row_scrolling() {
    // 1. Empty place page: opens at action_row() == 0 and stays clamped at 0 on Minus.
    {
        Ui &ui = fresh_ui();
        uint32_t t = 20000;
        ui.open_place(bench::Place::Cable, t);
        settle(ui, t);
        CHECK_EQ(cp(ui).result_rows(), 0);
        CHECK_EQ(cp(ui).window_first(), 0);
        CHECK_EQ(cp(ui).action_row(), 0);
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).window_first(), 0);
        CHECK_EQ(cp(ui).action_row(), 0);
    }

    // 2. Populated Cable place with >= 7 fact rows (8 rows: ID, Speed, Current, Voltage, EPR, Type, Length, Answered):
    //    - Opening starts at window_first() == 0, action_row() == -1.
    //    - Plus scrolls window_first() 1 row at a time until all fact rows are visible, then focuses action_row() == 0, 1, ...
    //    - Minus steps back 1 action row at a time, then from action_row() == 0 with window_first() > 0 immediately
    //      unfocuses AND decrements window_first() by 1 (no dead press), scrolling back to 0.
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
    ui.open_place(bench::Place::Cable, t += 100);
    settle(ui, t);

    const int res = cp(ui).result_rows();
    CHECK(res >= 8);
    CHECK_EQ(cp(ui).window_first(), 0);
    CHECK_EQ(cp(ui).action_row(), -1);

    // Scroll down 1 fact row at a time until all fact rows are visible (first == res - kPlaceRows).
    const int max_fact_first = res - ConnectionPage::kPlaceRows;
    for (int f = 1; f <= max_fact_first; f++) {
        ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).window_first(), f);
        CHECK_EQ(cp(ui).action_row(), -1);
    }

    // Next Plus focuses action_row() == 0, then 1.
    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).action_row(), 0);
    CHECK_EQ(cp(ui).window_first(), max_fact_first + 1);

    ui.on_button(Button::Plus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).action_row(), 1);
    CHECK_EQ(cp(ui).window_first(), max_fact_first + 2);

    // Minus steps back 1 action row at a time.
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).action_row(), 0);

    // Minus from action_row() == 0 with window_first() > 0 immediately unfocuses AND decrements window_first() by 1.
    const int wf_before = cp(ui).window_first();
    CHECK(wf_before > 0);
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).action_row(), -1);
    CHECK_EQ(cp(ui).window_first(), wf_before - 1);

    // Minus scrolls back 1 row at a time to 0.
    for (int f = wf_before - 2; f >= 0; f--) {
        ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
        settle(ui, t);
        CHECK_EQ(cp(ui).action_row(), -1);
        CHECK_EQ(cp(ui).window_first(), f);
    }
    // At top: Minus stays at 0, -1.
    ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    settle(ui, t);
    CHECK_EQ(cp(ui).window_first(), 0);
    CHECK_EQ(cp(ui).action_row(), -1);
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
    ui.open_place(bench::Place::Cable, t += 100);
    settle(ui, t);
#ifdef K2_TRUNC_LOG
    CHECK_EQ(k2::trunc::failures(), 0);
#endif
    // Verify the painted identity header matches positive chips only ([100W] [2.0]) and NOT "no EPR".
    const Surface s = ui.context().content;
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
    ui.open_place(bench::Place::Device, t += 100);
    settle(ui, t);
    while (cp(ui).action_row() >= 0) ui.on_button(Button::Minus, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t += 100);
    g_stub.iq_running = false;
    g_stub.iq_status.phase = pd::IqPhase::Done;
    g_stub.iq_status.ok_mask = pd::iq_bit(pd::IqStep::SinkCapExt);
    g_stub.iq_status.sink_ext_len = 24;
    g_stub.iq_status.sink_ext[18] = 15;
    g_stub.iq_status.sink_ext[20] = 45;   // SPR 45W min 15 (spr_op == 0, epr_max == 0)
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
    ui2.open_place(bench::Place::Device, t2 += 100);
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

int main() {
    test_sink_tab_pd_body_shows_the_raw_vooc_note();
    test_sink_tab_no_pd_shows_the_raw_vooc_note();
    test_sink_tab_offers_identify_when_a_device_is_alone();
    test_identify_log_keeps_fixed_text_in_the_format();
    test_survey_nothing_active_before_refresh();
    test_survey_refresh_item_starts_it();
    test_survey_status_line();
    test_survey_details_rows_follow_the_survey();
    test_refresh_goes_through_the_rescan_module();
    test_the_rescan_comeback_lands_on_the_source_tab();
    test_live_request_repaints_only_its_rows();
    test_worst_case_rows_fit();
    test_cable_tab_answered_row();
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
    test_cable_place_has_no_spoof_row();
    test_tools_spoof_shortcut_opens_the_cable_tab_list();
    test_sink_id_row_marks_a_sniffed_identity();
    test_sink_tab_no_pd_offered_unanswered();
    test_source_tab_no_pd_never_offered_all_rotations();
    test_pd_present_keeps_identify_offered();
    test_single_tools_lock_one_tab();
    test_tools_rows_open_single_tools();
    test_svooc_trigger_rise_is_not_a_sink();
    test_device_place_inline_interrogate_and_readout();
    test_device_place_inline_interrogate_with_sink_pdos();
    test_device_place_standalone_and_self_powered_identify();
    test_place_unified_one_row_scrolling();
    test_cable_identity_positive_chips_and_no_trunc();
    test_device_place_telemetry_rows_and_live_repaints();
    return test_report("test_ui_connection_page");
}
