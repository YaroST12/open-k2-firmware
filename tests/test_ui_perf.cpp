// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Performance budgets for the UI redraw paths (host-measured streamed pixels, g_lcd_pixels).
// The panel has no read-back, so "partial redraw" means skipping draw calls: a page switch
// erases only the outgoing page's ink (lcd erase mode) and a list scroll repaints only the
// slots whose (row, highlight) changed. These budgets lock that in; the convergence checks
// prove the incremental paths produce exactly the same pixels as a forced full redraw.
#include "fake_lcd.h"
#include "k2/ui/pixel_sink.hpp"
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "ui_stubs.h"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/emlib.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <vector>

using namespace k2;

static void fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 7; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;   // caps + a sink answer: the verdict is Present
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},
        {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000}, {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000},
        {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
}

static void test_page_switch_budgets() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1100);
    // One full screen (content 224x240 + panel 240x16) is 57600 px. A switch into an opaque page (Detail, Diag, Lines, Tools,
    // Settings: Ui::page_opaque) paints once with no erase pass, so it costs about one screen; a page that is not opaque
    // (Charger, Connection) still pays the erase replay of the page it replaces, Home's included (its two measurement cells,
    // band and info block make that replay dear). Connection (entered here on the Source tab) wipes and repaints its whole
    // body: up to eight rows of text over a full-body fill. tests/test_ui_overdraw.cpp prices every pair. Connection also sets
    // its OK verb once its first update has built the rows (a key-rail slot repaint inside the switch).
    struct { uint8_t page; unsigned budget; const char *name; } cases[] = {
        {1, 59500, "Detail"}, {2, 111000, "Charger"}, {kPdPage, 142600, "Connection"}, {kDiagPage, 58800, "Diag"},
        {3, 60000, "Lines"}, {4, 58800, "Tools"}, {5, 58200, "Settings"},
    };
    for (auto &c : cases) {
        g_lcd_pixels = 0;
        CHECK_EQ(ui.goto_page(c.page, 2000), true);
        std::printf("page switch -> %-8s: %u px (budget %u)\n", c.name, g_lcd_pixels, c.budget);
        CHECK_EQ(g_lcd_pixels <= c.budget, true);
        g_lcd_pixels = 0;
        ui.tick(2050);
        CHECK_EQ(g_lcd_pixels, 0u);   // steady state right after the switch: nothing is rewritten
        CHECK_EQ(ui.goto_page(0, 2100), true);
    }
}

static void test_scroll_budgets() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1100);
    // Tools hub: moving the selection one row repaints the two highlight-transition rows and the
    // changed text cells only (was ~42k with a full-row fill per row).
    ui.goto_page(4, 2000);
    ui.tick(2100);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 2200 + i * 100);
        std::printf("tools move %-2d: %u px (budget 26000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 26000, true);
    }
    // ChargerTest setup list (ListView): same budget on the shared widget.
    ui.goto_page(2, 3000);
    ui.tick(3100);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 3200 + i * 100);
        std::printf("ct scroll  %-2d: %u px (budget 24000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 24000, true);
    }
    // Spoof menu: the saved-cable list on the shared ListView (was a full-body wipe per scroll).
    g_stub.lib.clear();
    for (unsigned n = 1; n <= 6; ++n) {
        emlib::Entry e;
        e.seq = n;
        const uint32_t v[] = {0xFF00A041, 0x1800203C, 123, 0x10000 + n, 0x00182052};
        e.rec = emrec::make(v, 2);
        std::snprintf(e.name.data(), e.name.size(), "Cable %u", n);
        g_stub.lib.push_back(e);
    }
    ui.goto_page(4, 4000);
    ui.tick(4100);
    CHECK_EQ(ui.push_menu(Menu::Spoof, 4200), true);
    ui.tick(4300);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 4400 + i * 100);
        std::printf("spoof scroll %-2d: %u px (budget 26000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 26000, true);
    }
    ui.pop_menu(4700);
    // Settings (kit RowList, 39 px two-line rows): moving the selection repaints exactly the two rows
    // whose highlight changed, opaque (row fill then text), plus the footer hint.
    ui.goto_page(5, 4800);
    ui.tick(4900);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 5000 + i * 100);
        std::printf("settings move %-2d: %u px (budget 32000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 32000, true);
    }
    // E-marker saved-cable menu: scrolling the list repaints only the shifted rows.
    g_stub.em = {};
    g_stub.em.valid = true; g_stub.em.num_vdos = 5; g_stub.em.spec_rev = 2; g_stub.em.is_active = true;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    g_stub.lib.clear();
    for (unsigned n = 1; n <= 6; ++n) {
        emlib::Entry e;
        e.seq = n;
        const uint32_t v[] = {0xFF00A041, 0x1800203C, 123, 0x10000 + n, 0x00182052};
        e.rec = emrec::make(v, 2);
        std::snprintf(e.name.data(), e.name.size(), "Cable %u", n);
        g_stub.lib.push_back(e);
    }
    ui.goto_page(6, 5400);
    CHECK_EQ(ui.push_menu(Menu::Emarker, 5500), true);
    ui.run_action(Action::EmarkerScan, 5600);
    ui.tick(5700);
    ui.on_button(Button::Ok, ButtonEvent::Long, 5800);    // open the cable-actions menu
    ui.tick(5900);
    ui.on_button(Button::Plus, ButtonEvent::Short, 5950);  // select "Saved cables"
    ui.on_button(Button::Ok, ButtonEvent::Short, 6000);   // open the saved-cable list
    ui.tick(6100);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 6200 + i * 100);
        ui.tick(6200 + i * 100 + 50);
        std::printf("em scroll %-2d: %u px (budget 46000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 46000, true);
    }
    // PD trigger OFFER list: once the cursor walks past the visible window, every scroll input shifts
    // the window; only the shifted rows' content (not their base fills) may be repainted.
    g_stub.caps.num_pdos = 10;
    const pd::Pdo more[3] = {
        {pd::PdoType::Fixed, 0, 12000, 12000, 3000, 36000},
        {pd::PdoType::Pps, 0, 3300, 20000, 3000, 60000},
        {pd::PdoType::Avs, 0, 15000, 21000, 4000, 84000}};
    for (int i = 0; i < 3; i++) g_stub.caps.pdos[7 + i] = more[i];
    ui.goto_page(4, 6400);
    CHECK_EQ(ui.push_menu(Menu::PdTrigger, 6500), true);
    ui.tick(6600);
    for (int i = 0; i < 6; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 6700 + i * 100);  // to the window edge
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 7300 + i * 100);
        std::printf("pdtrig scroll %-2d: %u px (budget 69200)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 69200, true);
    }
    ui.pop_menu(7700);
    // Report cards list: +/- must move through the ListView's per-slot cache -- no body wipe, no
    // list invalidation, no full header repaint.
    stub_cards_reset();
    for (int n = 0; n < 10; n++) {
        namespace rc = report_card;
        rc::Card cd;
        cd.id = {uint32_t(0x1000 + n), 0x0B05, 0x1234};
        cd.boots = 3 + n;
        cd.runs = 1 + n;
        cd.group[0] = {rc::GV::Pass, 20000, 0, 0};
        cd.group[1] = {rc::GV::Fail, 0, 0, 1};
        rc::set_name(cd, "Charger");
        rc::hw::put(cd);
    }
    ui.goto_page(4, 7800);
    CHECK_EQ(ui.push_menu(Menu::Cards, 7900), true);
    ui.tick(8000);
    for (int i = 0; i < 3; i++) {
        g_lcd_pixels = 0;
        ui.on_button(Button::Plus, ButtonEvent::Short, 8100 + i * 100);
        ui.tick(8100 + i * 100 + 50);
        // The kit RowList opens the focused row to 40 px (its note line), so a move shifts the rows under it: the budget is
        // the kit's (as Tools and Settings), still far below a body wipe.
        std::printf("cards scroll %-2d: %u px (budget 25000)\n", i, g_lcd_pixels);
        CHECK_EQ(g_lcd_pixels <= 25000, true);
    }
    ui.pop_menu(8500);
}

// Write-count sink: how many times each panel pixel is written during one input or tick. The
// ChargerTest list used to wipe the whole body and then repaint every ListView row on top of the
// wipe (every pixel twice, a black flash on the real panel); a settled screen must write nothing.
namespace heat {
uint8_t n[240 * 240];
unsigned total, cx, cy, x0, x1, y0, y1;
PixelSink panel;   // the sink replaces the fake panel's own stream: keep g_panel in step so a later screen diff is true
void begin(uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    static bool attached;
    if (!attached) { panel.attach(g_panel, 240, 0, 240); attached = true; }
    panel.window(a, b, c, d);
    x0 = cx = a; y0 = cy = b; x1 = c; y1 = d;
}
void put(uint32_t k) {
    for (; k; --k) {
        if (cx < 240 && cy < 240 && n[cy * 240 + cx] < 255) ++n[cy * 240 + cx];
        ++total;
        if (++cx > x1) { cx = x0; if (++cy > y1) cy = y0; }
    }
}
void pixels(const uint16_t *p, uint32_t c) { panel.push(p, c); put(c); }
void fill(uint16_t color, uint32_t c) { panel.fill(color, c); put(c); }
const lcd::Sink sink = {begin, pixels, fill};
// Pixels written in rows y0..y1-1 (all columns) at least `times` times.
unsigned rewrites(int ya, int yb, int times) {
    unsigned o = 0;
    for (int y = ya; y < yb; ++y) for (int x = 0; x < 240; ++x) o += n[y * 240 + x] >= times;
    return o;
}
// Pixels written at all in rows ya..yb-1.
unsigned touched(int ya, int yb) { return rewrites(ya, yb, 1); }
}  // namespace heat

struct ChargerRig {
    Ui ui;
    uint32_t now = 1200;
    unsigned last_px = 0;
    void start() {
        g_stub = {};
        g_stub.pm_online = true;
        g_stub.pm.vbus_mv = 5000;
        g_stub.caps.valid = true; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
        g_stub.caps.raw_header = 0x1181; g_stub.caps.num_pdos = 1;
        g_stub.caps.pdos[0] = pd::decode_pdo(0x0001912C);
        ui.init(1000); ui.set_rotation(0, 1000); ui.tick(1000);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
        tick(4);
    }
    void measured(void (*fn)(ChargerRig &)) {
        std::memset(heat::n, 0, sizeof heat::n); heat::total = 0;
        lcd::set_sink(&heat::sink);
        fn(*this);
        lcd::set_sink(nullptr);
        last_px = heat::total;
    }
    void tick(int n = 1) { for (int i = 0; i < n; ++i) { now += kPageTickMs; ui.tick(now); } }
    void press(Button b) { ui.on_button(b, ButtonEvent::Short, now); now += 10; }
    void move(unsigned n) { for (unsigned i = 0; i < n; ++i) press(Button::Plus); }
    void long_ok() { ui.on_button(Button::Ok, ButtonEvent::Long, now); now += 10; }
};

static unsigned force_full_redraw(Ui &ui, uint32_t now);

static void test_charger_page_flicker() {
    const auto old = g_stub;
    static ChargerRig r;
    r.start();
    // 1. A settled list repaints nothing, whatever the volatile inputs do. pd_traffic and the VBUS
    // level sit in the page's proposal (they matter to Review) but no Algorithms row shows them.
    unsigned worst_idle = 0;
    for (int i = 0; i < 10; ++i) {
        g_stub.pd_traffic = i & 1;
        g_stub.pm.vbus_mv = (i % 3 == 2) ? 3000 : 5000 + 20 * i;
        r.measured([](ChargerRig &x) { x.tick(); });
        // The title bar (y < 20) carries the PD-activity badge, which follows pd_traffic by design.
        worst_idle = std::max(worst_idle, heat::touched(20, 240));
    }
    std::printf("charger idle with volatile inputs: worst %u body px per tick\n", worst_idle);
    CHECK_EQ(worst_idle, 0u);
    g_stub.pd_traffic = false; g_stub.pm.vbus_mv = 5000;
    r.tick(2);

    // 2. A content change must not wipe the body ahead of the list repaint: the margin above the list (y 22..23) and
    // everything below the rows are ink-free, so nothing may write them. Group of the USB PD family (rows: PD fixed, PPS),
    // "PD fixed" toggled: exactly the focused row (40 px with its note) changes; the PPS row below stays untouched.
    r.move(4);                          // Tests row 4: "Custom..." (rows are 0-based: there is no Next row)
    r.press(Button::Ok);                // > the Custom list (the families, then Done)
    r.tick(2);
    r.move(4);                          // Custom row 4: the USB PD family
    r.long_ok();                        // long OK opens the family's entries: Group
    r.tick(2);
    r.measured([](ChargerRig &x) { x.press(Button::Ok); });   // toggles the first entry (PD fixed)
    // Geometry: rows from y 24 (the focused one is 40 px tall), the footer from y 222.
    const unsigned bands = heat::touched(22, 24) + heat::touched(110, 222);
    const unsigned quiet = heat::touched(64, 110);
    std::printf("charger group entry toggle: %u px, margin bands %u px, unaffected slots %u px\n", r.last_px, bands, quiet);
    CHECK_EQ(bands, 0u);
    CHECK_EQ(quiet, 0u);
    CHECK_EQ(r.last_px <= 20000, true);
    r.tick(2);
    unsigned diff = force_full_redraw(r.ui, r.now);
    std::printf("charger group after toggle: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    r.tick(2);
    // Nothing is left to repaint after the change: a later idle tick writes nothing.
    r.measured([](ChargerRig &x) { x.tick(); });
    CHECK_EQ(r.last_px, 0u);
    // 3. A stage change wipes the body once (the old stage owned all of it) and paints the new list: about two body areas.
    r.measured([](ChargerRig &x) { x.press(Button::Back); });
    std::printf("charger group -> custom list: %u px (budget 115000)\n", r.last_px);
    CHECK_EQ(r.last_px <= 115000, true);
    r.tick(2);
    diff = force_full_redraw(r.ui, r.now);
    std::printf("charger custom list after Back: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);

    // 4. Review: the summary lines are page ink outside the list. They repaint when what they print changes
    // (a toggled algorithm), never for PD traffic or a VBUS wobble, and the result matches a forced redraw.
    for (int i = 0; i < 6; ++i) r.press(Button::Minus);   // back to row 0 (Back left the cursor on the PD family row)
    r.move(6);                          // Done
    r.press(Button::Ok);                // Done: Cable (from the Custom list)
    r.tick(2);
    r.press(Button::Ok);                // OFF (row 0) is picked: it advances to Review
    r.tick(2);
    CHECK_EQ(r.ui.charger_test_stage(), 4);
    unsigned review_idle = 0;
    for (int i = 0; i < 10; ++i) {
        g_stub.pd_traffic = i & 1;
        g_stub.pm.vbus_mv = 5000 + 20 * i;
        r.measured([](ChargerRig &x) { x.tick(); });
        review_idle = std::max(review_idle, heat::touched(20, 240));
    }
    std::printf("charger review idle with volatile inputs: worst %u body px per tick\n", review_idle);
    CHECK_EQ(review_idle, 0u);
    // VBUS off the 5 V default refuses the start: that changes the hint line (y 208) and the amber footer, nothing above.
    g_stub.pm.vbus_mv = 3000;
    r.measured([](ChargerRig &x) { x.tick(); });
    std::printf("charger review refusal note: %u px, list and summary %u px\n", r.last_px, heat::touched(20, 208));
    CHECK_EQ(heat::touched(20, 208), 0u);
    g_stub.pd_traffic = false; g_stub.pm.vbus_mv = 5000;
    r.tick(2);
    diff = force_full_redraw(r.ui, r.now);
    std::printf("charger review: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    g_stub = old;
}

// The wizard's screens (Tests, Custom, Cable, Review) and Tools -> Spoof, which share one cable-identity list: the first paint of
// each step, one cursor move, a selection change and an arm are bounded, an idle tick writes nothing, and the incremental frame
// equals a forced repaint. Budgets are the measured values plus about 10 %.
static void test_wizard_budgets() {
    const auto old = g_stub;
    static ChargerRig r;
    r.start();
    g_stub.lib.clear();
    for (unsigned n = 1; n <= 6; ++n) {
        emlib::Entry e;
        e.seq = n;
        const uint32_t v[] = {0xFF00A041, 0x1800203C, 123, 0x10000 + n, 0x00182052};
        e.rec = emrec::make(v, 2);
        std::snprintf(e.name.data(), e.name.size(), "Cable %u", n);
        g_stub.lib.push_back(e);
    }
    r.tick(2);
    unsigned diff;
    // A stage change wipes the body once (22..222, about 45 k px) and paints the new list, whose rows are filled and then drawn,
    // so a stage entry writes about two to three body areas. The budgets below are the measured values plus about 10 %.
    // Tests step: one cursor move (rows are 0-based; a pick advances, there is no Next row).
    r.measured([](ChargerRig &x) { x.move(1); });
    std::printf("wizard tests move: %u px (budget %u)\n", r.last_px, 23000u);
    CHECK_EQ(r.last_px <= 23000u, true);
    CHECK_EQ(heat::touched(22, 24), 0u);
    r.measured([](ChargerRig &x) { x.tick(3); });
    CHECK_EQ(r.last_px, 0u);
    // Custom list: first paint of the stage, then a move.
    r.move(3);                          // Custom... (row 4)
    r.measured([](ChargerRig &x) { x.press(Button::Ok); });
    std::printf("wizard tests -> custom: %u px (budget %u)\n", r.last_px, 126000u);
    CHECK_EQ(r.ui.charger_test_stage(), 14);
    CHECK_EQ(r.last_px <= 126000u, true);
    r.tick(2);
    diff = force_full_redraw(r.ui, r.now);
    CHECK_EQ(diff, 0u);
    r.measured([](ChargerRig &x) { x.move(1); });
    std::printf("wizard custom move: %u px (budget %u)\n", r.last_px, 23000u);
    CHECK_EQ(r.last_px <= 23000u, true);
    r.press(Button::Back);
    r.tick(2);
    CHECK_EQ(r.ui.charger_test_stage(), 0);
    // Tests -> Cable: a preset pick advances. First paint of the stage, a move, a scroll, an idle tick.
    for (int i = 0; i < 6; ++i) r.press(Button::Minus);   // back on row 0
    r.measured([](ChargerRig &x) { x.press(Button::Ok); });
    std::printf("wizard tests -> cable: %u px (budget %u)\n", r.last_px, 119000u);
    CHECK_EQ(r.ui.charger_test_stage(), 2);
    CHECK_EQ(r.last_px <= 119000u, true);
    r.tick(2);
    diff = force_full_redraw(r.ui, r.now);
    std::printf("wizard cable: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    r.measured([](ChargerRig &x) { x.move(1); });
    std::printf("wizard cable move: %u px (budget %u)\n", r.last_px, 23000u);
    CHECK_EQ(r.last_px <= 23000u, true);
    r.measured([](ChargerRig &x) { x.tick(3); });
    CHECK_EQ(r.last_px, 0u);
    // Scrolling the cable list (11 rows, 5 visible): a window shift repaints the visible rows and nothing outside.
    unsigned worst = 0;
    for (int i = 0; i < 8; ++i) {
        r.measured([](ChargerRig &x) { x.move(1); });
        worst = std::max(worst, r.last_px);
        CHECK_EQ(heat::touched(22, 24), 0u);
    }
    std::printf("wizard cable scroll: worst %u px (budget %u)\n", worst, 30000u);
    CHECK_EQ(worst <= 30000u, true);
    // Review: a pick advances; first paint, then idle.
    r.measured([](ChargerRig &x) { x.press(Button::Ok); });
    std::printf("wizard cable -> review: %u px (budget %u)\n", r.last_px, 130000u);
    CHECK_EQ(r.ui.charger_test_stage(), 4);
    CHECK_EQ(r.last_px <= 130000u, true);   // measured 118041: the wipe, the summary lines, the cable rows and the footer
    r.tick(2);
    r.measured([](ChargerRig &x) { x.tick(3); });
    CHECK_EQ(r.last_px, 0u);
    diff = force_full_redraw(r.ui, r.now);
    std::printf("wizard review: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);

    // Tools -> Spoof on the same list: arming repaints the card, two pills and the footer only.
    r.ui.goto_page(static_cast<uint8_t>(Page::Tools), r.now);
    r.tick(2);
    CHECK_EQ(r.ui.push_menu(Menu::Spoof, r.now), true);
    r.tick(2);
    r.measured([](ChargerRig &x) { x.move(1); });
    std::printf("spoof move: %u px (budget %u)\n", r.last_px, 23000u);
    CHECK_EQ(r.last_px <= 23000u, true);
    r.measured([](ChargerRig &x) { x.press(Button::Ok); x.tick(2); });
    std::printf("spoof arm: %u px (budget %u)\n", r.last_px, 40000u);   // the state card, two pill rows, the footer, the key labels
    CHECK_EQ(r.last_px <= 40000u, true);
    CHECK_EQ(g_stub.spoof_arms, 1);
    r.measured([](ChargerRig &x) { x.tick(3); });
    CHECK_EQ(r.last_px, 0u);
    diff = force_full_redraw(r.ui, r.now);
    std::printf("spoof: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    g_stub = old;
}

// The Results grid (eight 18 px unit rows, counters, summary strip): the first paint of the page and one selection move are
// bounded, and a move touches only the two rows that change focus (or the scroll window), never the margin above the list.
static void test_results_grid_budgets() {
    constexpr unsigned kResRowPx = 40 * 221;   // one tallest (volt + current) row without the 3 px scrollbar lane
    const auto old = g_stub;
    static ChargerRig r;
    r.start();
    g_stub.fc_answers = true;   // the QC unit only gets a full row once the charger answered
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
    r.ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);   // re-enter with the richer source
    r.tick(4);
    for (unsigned i = 0; i < 12; ++i) r.press(Button::Minus);
    r.press(Button::Ok); r.tick(2);     // the first preset: advances to Cable
    r.press(Button::Ok); r.tick(2);     // the first cable (Off): advances to Review
    CHECK_EQ(r.ui.charger_test_stage(), 4);
    r.press(Button::Ok); r.tick(2);     // START: the confirm window opens on Review
    for (int i = 0; i < 4000 && r.ui.charger_test_stage() == 13; ++i) { fusb302::service(); r.tick(); }
    r.press(Button::Ok);                // the confirming press starts the run
    unsigned first_paint = 0, run_worst = 0, run_ticks = 0, run_entry = 0;
    for (int i = 0; i < 100000 && r.ui.charger_test_stage() != 6; ++i) {
        const bool running = r.ui.charger_test_stage() == 5;
        r.measured([](ChargerRig &x) { x.tick(); });
        first_paint = r.last_px;
        if (!running || r.ui.charger_test_stage() != 5) continue;   // the flip tick to Results is budgeted below
        if (run_ticks++ == 0) run_entry = r.last_px;   // the first tick on the stage
        else run_worst = std::max(run_worst, r.last_px);    }
    CHECK_EQ(r.ui.charger_test_stage(), 6);
    // A step in flight: painting is gated while the runner is quiet, so most ticks write nothing or the hero value (~6 000 px).
    // A unit boundary moves the running row and may shift the grid window, which repaints every visible row (about 50 000 px,
    // like a Results scroll; ~60 of the ~2 800 ticks of a full run are above 3 000 px). Budget: measured worst 40 492 +10 %.
    std::printf("charger running: entry %u px, %u ticks, worst later tick %u px (budget 44500)\n", run_entry, run_ticks, run_worst);
    CHECK_EQ(run_ticks > 0, true);
    CHECK_EQ(run_worst <= 44500, true);
    // The whole body once is ~45 000 px (wipe 22..222); the hero band, the counters, the section rules and the unit rows
    // (26 px, 40 px with a current line, each streamed once) bring the flip tick to about 98 000. Budget: measured 97 688 +10 %.
    std::printf("charger results first paint: %u px (budget 107500)\n", first_paint);
    CHECK_EQ(first_paint <= 107500, true);
    r.tick(4);
    // A move inside the window rewrites the rows that change focus (26 or 40 px each) and the footer line (it explains the
    // focused unit). A move that scrolls the window repaints every visible row: ResultGrid has no blit, so that is the
    // cost of a scroll. Budgets: measured (19 264 and 37 632) +10 %.
    unsigned worst_move = 0, worst_scroll = 0, scrolls = 0;
    for (int i = 0; i < 12; ++i) {
        r.measured([](ChargerRig &x) { x.press(Button::Plus); x.tick(4); });
        const bool scrolled = heat::touched(72, 222) > 3 * kResRowPx;
        (scrolled ? worst_scroll : worst_move) = std::max(scrolled ? worst_scroll : worst_move, r.last_px);
        scrolls += scrolled;
        CHECK_EQ(heat::touched(20, 72), 0u);   // the hero band, counters and summary strip stay as they are
    }
    std::printf("charger results selection move: worst %u px (budget 21200), scrolling move: worst %u px (budget 41400), %u scrolls\n",
                worst_move, worst_scroll, scrolls);
    CHECK_EQ(worst_move <= 21200, true);
    CHECK_EQ(scrolls > 0 && worst_scroll <= 41400, true);
    g_stub = old;
}

// Incremental updates must converge to exactly what a forced full redraw paints. The forced
// redraw is a rotation flip at the same instant (lcd::fill + redraw), which does not disturb
// page state (goto_page would re-enter the charger-test page and reset its stage).
static unsigned force_full_redraw(Ui &ui, uint32_t now) {
    std::vector<uint16_t> before(g_panel, g_panel + 240 * 240);
    ui.set_rotation(1, now);
    ui.set_rotation(0, now);
    unsigned diff = 0;
    int x0 = 240, y0 = 240, x1 = -1, y1 = -1;
    for (int i = 0; i < 240 * 240; i++) {
        if (before[i] == g_panel[i]) continue;
        diff++;
        x0 = std::min(x0, i % 240); x1 = std::max(x1, i % 240);
        y0 = std::min(y0, i / 240); y1 = std::max(y1, i / 240);
    }
    if (diff) std::printf("  differing pixels in x %d..%d, y %d..%d\n", x0, x1, y0, y1);
    return diff;
}

static void test_convergence() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1100);
    ui.tick(1400);   // let the display throttles lapse so a redraw is not held back

    ui.goto_page(4, 2000);   // Tools
    ui.tick(2100);
    for (int i = 0; i < 5; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 2200 + i * 100);
    ui.tick(2500);
    ui.tick(2800);
    unsigned diff = force_full_redraw(ui, 2800);
    std::printf("tools: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);

    ui.goto_page(5, 2900);   // Settings (two-line kit RowList rows), selection moved
    ui.tick(2950);
    for (int i = 0; i < 3; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 2960 + i * 10);
    ui.tick(2990);
    ui.tick(2995);
    diff = force_full_redraw(ui, 2995);
    std::printf("settings: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);

    ui.goto_page(2, 3000);   // ChargerTest setup list, then scroll
    ui.tick(3100);
    for (int i = 0; i < 4; i++) ui.on_button(Button::Plus, ButtonEvent::Short, 3200 + i * 100);
    ui.tick(3500);
    ui.tick(3800);
    diff = force_full_redraw(ui, 3800);
    std::printf("charger-test: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);

    // A full tour followed by a forced same-instant redraw of Home must be pixel-stable.
    const uint8_t tour[] = {1, 2, 3, 4, 5, kPdPage, kDiagPage};
    for (uint8_t i = 0; i < sizeof tour; i++) { ui.goto_page(tour[i], 4000 + i * 100); ui.tick(4100 + i * 100); }
    ui.goto_page(0, 5000);
    ui.tick(5100);
    ui.tick(5400);
    diff = force_full_redraw(ui, 5400);
    std::printf("home: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
}

// FC trigger protocol list (Pick stage): +/- repaints only the old and the new selected row plus the
// scrollbar thumb. It used to wipe the whole body (the body signature held the selection, and every
// consumed press invalidated the page) and repaint all seven rows, a visible flicker.
static void test_trigger_pick_budgets() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1100);
    ui.tick(1400);
    ui.goto_page(4, 2000);
    ui.tick(2100);
    for (int i = 0; i < 12; i++) ui.on_button(Button::Minus, ButtonEvent::Short, 2110 + i);   // selection to the top
    CHECK_EQ(ui.push_menu(Menu::Trigger, 2200), true);
    ui.tick(2300);
    ui.tick(2600);
    unsigned worst_move = 0, worst_scroll = 0, scrolls = 0;
    // Down through the list (past the seven visible rows, so the window scrolls too), then back up.
    for (int i = 0; i < 12; i++) {
        const int offset = ui.list_state().offset;
        g_lcd_pixels = 0;
        ui.on_button(i < 8 ? Button::Plus : Button::Minus, ButtonEvent::Short, 3000 + i * 300);
        ui.tick(3000 + i * 300 + 150);
        const bool scrolled = ui.list_state().offset != offset;
        std::printf("fc trigger %s %-2d: %u px\n", scrolled ? "scroll" : "move  ", i, g_lcd_pixels);
        if (scrolled) { ++scrolls; worst_scroll = std::max(worst_scroll, g_lcd_pixels); }
        else worst_move = std::max(worst_move, g_lcd_pixels);
    }
    // A move inside the window: two list rows (about 5k px each) and, at most, a footer line. A scroll shifts
    // every slot, like the charger results list. The old body wipe plus full repaint wrote 107k px per move.
    // The kit rows are edge to edge and 26 px tall (full-width focus fill plus bar), so a move costs a little more than
    // the old padded 21 px rows did: 17.1k measured against 15.0k before.
    std::printf("fc trigger selection move: worst %u px (budget 18000), scroll: worst %u px (budget 56000), %u scrolls\n",
                worst_move, worst_scroll, scrolls);
    CHECK_EQ(worst_move <= 18000, true);
    CHECK_EQ(scrolls > 0 && worst_scroll <= 56000, true);
    ui.tick(7000);
    ui.tick(7300);
    const unsigned diff = force_full_redraw(ui, 7300);
    std::printf("fc trigger: incremental vs forced redraw diff=%u\n", diff);
    CHECK_EQ(diff, 0u);
    // The press no longer invalidates the page, so every stage change must still converge on its own signatures:
    // Edit (draft stepped), Review, and back to the list.
    uint32_t t = 7400;
    for (int step = 0; step < 6; ++step) {
        const Button b = step == 0 ? Button::Ok : step == 1 || step == 2 ? Button::Plus : step == 3 ? Button::Ok : Button::Back;
        ui.on_button(b, ButtonEvent::Short, t += 200);
        ui.tick(t += 200);
        const unsigned d = force_full_redraw(ui, t);
        std::printf("fc trigger step %d: incremental vs forced redraw diff=%u\n", step, d);
        CHECK_EQ(d, 0u);
    }
}

// The PD page cheap pre-check (perf/match-precheck): with unchanged inputs the 10 Hz updates must
// not run match_live() again after the page settled, the frame must stay pixel-identical, and one
// real input change must run the expensive path exactly once more.
static void test_match_precheck() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1100);
    ui.goto_page(kPdPage, 2000);
    ui.tick(2100);
    ui.tick(2200);
    ui.tick(2300);   // let one-shot state (inline latch etc.) settle: the page is painted now
    const uint32_t base = g_match_live_calls;
    std::vector<uint16_t> before(g_panel, g_panel + 240 * 240);
    for (int i = 0; i < 50; i++) ui.tick(2400 + 100u * i);   // 50 ten-hz ticks, unchanged inputs
    const uint32_t skipped = g_match_live_calls - base;
    std::printf("pd precheck: %u match_live calls over 50 unchanged updates\n", skipped);
    CHECK_EQ(skipped, 0u);
    unsigned diff = 0;
    for (int i = 0; i < 240 * 240; i++) diff += before[i] != g_panel[i];
    std::printf("pd precheck: %u pixels changed over 50 unchanged updates\n", diff);
    CHECK_EQ(diff, 0u);
    // A real change (a sniffed Request) runs match_live exactly once, then the pre-check holds again.
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 4;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 5000;
    const uint32_t c0 = g_match_live_calls;
    ui.tick(7500);
    std::printf("pd precheck: %u match_live calls on a contract change\n", g_match_live_calls - c0);
    CHECK_EQ(g_match_live_calls - c0, 1u);
    const uint32_t c1 = g_match_live_calls;
    for (int i = 0; i < 10; i++) ui.tick(7600 + 100u * i);
    CHECK_EQ(g_match_live_calls - c1, 0u);
}

// Steady-state cost of one page update tick (kPageTickMs) with live numbers moving, per page.
// This is the number to compare against device timings (log "[UI] full=.. field=.."): the wire
// time of the pixels a tick streams, at each SPI divider, is what decides whether the tick period
// can shrink to 50 ms or 33 ms. "idle" is a tick with unchanged inputs (the signature gates must
// keep it at zero pixels).
static void test_tick_cost() {
    fixture();
    Ui ui;
    ui.init(1000);
    ui.set_rotation(0, 1000);
    ui.tick(1000 + kPageTickMs);
    // Budgets are measured values plus ~10 %. Home and Detail animate (flow arrow, gauges), so
    // their "idle" (3 ticks, inputs unchanged) is not zero; every other page must stay at 0.
    struct { uint8_t page; unsigned idle3_px, worst_px; const char *name; } cases[] = {
        {0, 29500, 29500, "Home"}, {1, 450, 3400, "Detail"}, {2, 0, 1850, "Charger"}, {kPdPage, 0, 1850, "Connect"},
        {kDiagPage, 0, 2000, "Diag"}, {3, 0, 1850, "Lines"},
        {4, 0, 3400, "Bench"},   // the Meter row's live readings and the watts a device gets, on top of the bar readout
        {5, 0, 1850, "Settings"},
    };
    std::printf("page tick = %u ms; px per tick with live values moving (wire ms at DIV8/DIV4/DIV2):\n",
                unsigned(kPageTickMs));
    uint32_t now = 2000;
    for (auto &c : cases) {
        ui.goto_page(c.page, now);
        for (int i = 0; i < 4; i++) { now += kPageTickMs; ui.tick(now); }   // settle
        g_lcd_pixels = 0;
        for (int i = 0; i < 3; i++) { now += kPageTickMs; ui.tick(now); }
        const unsigned idle = g_lcd_pixels;
        unsigned sum = 0, worst = 0;
        const int n = 20;
        for (int i = 0; i < n; i++) {
            g_stub.pm.vbus_mv = 20030 + 37 * (i + 1);
            g_stub.pm.current_ma = 1520 + 13 * (i + 1);
            g_stub.pm.power_mw = g_stub.pm.vbus_mv / 1000 * g_stub.pm.current_ma;
            g_lcd_pixels = 0;
            now += kPageTickMs;
            ui.tick(now);
            sum += g_lcd_pixels;
            worst = g_lcd_pixels > worst ? g_lcd_pixels : worst;
        }
        std::printf("  %-8s idle(3 ticks) %5u px | mean %6u px, worst %6u px = %5.1f / %5.1f / %5.1f ms\n", c.name,
                    idle, sum / n, worst, lcd::wire_us(worst, lcd::SpiDiv::Div8) / 1000.0,
                    lcd::wire_us(worst, lcd::SpiDiv::Div4) / 1000.0, lcd::wire_us(worst, lcd::SpiDiv::Div2) / 1000.0);
        CHECK_EQ(idle <= c.idle3_px, true);
        CHECK_EQ(worst <= c.worst_px, true);
        // Every page's worst tick fits in a 50 ms period at the shipped divider only if its wire
        // time is under 50 ms; Home is the one that does not (animation), the rest are far below.
        if (c.page != 0) CHECK_EQ(lcd::wire_us(worst, lcd::SpiDiv::Div8) < 50000, true);
    }
}

// Home with no source (the walk-by idle state) and Home on a flat trace: once painted, a steady tick
// must stay cheap (< 6000 px per 100 ms tick; the idle page has nothing to animate and no label to redraw).
static void test_home_idle_tick() {
    struct { const char *name; bool source; } cases[] = {{"idle", false}, {"flat", true}};
    for (auto &c : cases) {
        fixture();
        if (!c.source) {
            g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
            g_stub.caps.valid = false;
        }
        Ui ui;
        ui.init(1000);
        ui.set_rotation(0, 1000);
        uint32_t now = 1000;
        ui.goto_page(0, now);
        for (int i = 0; i < 140; i++) { now += kPageTickMs; ui.tick(now); }   // settle: paint, fill the 10 s trace
        unsigned worst = 0, sum = 0;
        const int n = 20;
        for (int i = 0; i < n; i++) {
            g_lcd_pixels = 0;
            now += kPageTickMs;
            ui.tick(now);
            sum += g_lcd_pixels;
            worst = g_lcd_pixels > worst ? g_lcd_pixels : worst;
        }
        std::printf("home %-4s steady tick: mean %u px, worst %u px (budget 6000 per %u ms)\n", c.name, sum / n, worst,
                    unsigned(kPageTickMs));
        CHECK_EQ(worst < 6000u, true);
        const unsigned diff = force_full_redraw(ui, now);   // the idle / flat frame equals a forced repaint
        std::printf("home %-4s: incremental vs forced redraw diff=%u\n", c.name, diff);
        CHECK_EQ(diff, 0u);
    }
}

int main() {
    test_tick_cost();
    test_home_idle_tick();
    test_page_switch_budgets();
    test_scroll_budgets();
    test_convergence();
    test_trigger_pick_budgets();
    test_charger_page_flicker();
    test_wizard_budgets();
    test_results_grid_budgets();
    test_match_precheck();
    return test_report("test_ui_perf");
}
