// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the real Ui: event routing into pages, ListView scrolling on the PD page with
// 7 PDOs, and the panel label set per page (wording, dimming, fit in the slot).
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "ui_nav.h"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/diag_live.hpp"
#include <algorithm>
#include <string.h>
#include <string>
#include <vector>

using namespace k2;

static void fill_pdos(int n) {
    g_stub.caps = {};
    g_stub.caps.valid = true; g_stub.caps.num_pdos = static_cast<uint8_t>(n); g_stub.caps.spec_rev = 2;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},
        {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000}, {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000},
        {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000}};
    for (int i = 0; i < n && i < 7; i++) g_stub.caps.pdos[i] = mix[i];
}

static Ui &fresh_ui() {
    static Ui ui;
    // The inline latch survives a VBUS gap (pd::kInlineHoldMs), so a case no longer ends with the Link back at "standalone" by itself:
    // start from a new Link (the rest of the Ui carries over between cases as before).
    const_cast<pd::Link &>(ui.link()) = pd::Link{};
    g_stub.ms = 10000;
    ui.init(g_stub.ms);
    ui.set_rotation(0, g_stub.ms);   // boot default is bottom-edge@0; the pixel checks below assume the right edge (logical == physical, no scaling)
    return ui;
}

static const char *label(Ui &ui, Button b) {
    static char buf[24];
    label_text(buf, ui.labels().b[static_cast<int>(b)], label_max_chars(b));
    return buf;
}

static void test_label_sets() {
    Ui &ui = fresh_ui();
    const char *expect[8][4] = {   // Back, Minus, Ok, Plus
        {"Hero*", "Up", "Bench", "Dn"},           // Flow: no level above it (long Back cycles the hero), Up / Dn flip the face
        {"Back", "Up", "Bench",  "Dn"},           // Numbers
        {"Back", "Up", "Pick",   "Dn"},           // Charger test: a preset picks and advances
        {"Back", "Up", "Bench",  "Dn"},           // Lines
        {"Back", "Up", "Open",   "Dn"},           // the Bench
        {"Back", "Up", "Open",   "Dn"},           // Settings
        {"Back", "Tab-", "Actions", "Tab+"},      // Connection (a submenu of Tools): long OK shares only when there is something to share
        {"Back", "Up", "Share*", "Dn"},           // Diag (a submenu of Tools): long OK shares the log
    };
    static constexpr uint8_t kPages[8] = {0, 1, 2, 3, 4, 5, kPdPage, kDiagPage};
    for (uint8_t i = 0; i < 8; i++) {
        const uint8_t p = kPages[i];
        ui.goto_page(p, 20000);
        for (int b = 0; b < 4; b++) CHECK_STR(label(ui, static_cast<Button>(b)), expect[i][b]);
        // Nothing pages any more, so nothing is dimmed for being the first or the last page.
        CHECK_EQ(ui.labels().b[static_cast<int>(Button::Minus)].dim, false);
        CHECK_EQ(ui.labels().b[static_cast<int>(Button::Plus)].dim, false);
        // Every label fits the slot: characters x advance of the panel font <= slot length.
        for (int b = 0; b < 4; b++) CHECK_EQ(static_cast<int>(strlen(label(ui, static_cast<Button>(b))) * kLabelFont.adv) + kDividerPx + 2 * kSlotPadPx <= static_cast<int>(b == 0 || b == 3 ? kSlotLenPx[0] : kSlotLenPx[1]), 1);
    }
}

// Fieldbook navigation (CONCEPT 7): Up / Dn flip the Meter's faces with a wrap, OK opens the Bench, the Bench opens a place,
// BACK climbs one level, a long BACK lands on Flow from anywhere. No press pages through the old carousel.
static void test_global_paging() {
    Ui &ui = fresh_ui();
    uint32_t t = 20000;
    CHECK_EQ(ui.page() == Page::Home, 1);
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Plus, ButtonEvent::Short, t)), static_cast<int>(UiAction::None));
    CHECK_EQ(ui.page() == Page::Detail, 1);                           // Flow -> Numbers -> Lines -> Flow
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::Lines, 1);
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::Home, 1);
    ui.on_button(Button::Minus, ButtonEvent::Short, t);               // and back the other way, wrapping too
    CHECK_EQ(ui.page() == Page::Lines, 1);
    ui.on_button(Button::Plus, ButtonEvent::Long, t);                 // long presses page nothing
    ui.on_button(Button::Minus, ButtonEvent::Long, t);
    CHECK_EQ(ui.page() == Page::Lines, 1);
    ui.on_button(Button::Ok, ButtonEvent::Short, t);                  // OK on a face: the Bench
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    ui.on_button(Button::Back, ButtonEvent::Short, t);                // BACK on the Bench: the face it came from
    CHECK_EQ(ui.page() == Page::Lines, 1);
    ui.on_button(Button::Back, ButtonEvent::Short, t);                // BACK on Numbers or Lines: Flow
    CHECK_EQ(ui.page() == Page::Home, 1);
    ui.on_button(Button::Back, ButtonEvent::Short, t);                // BACK on Flow: nothing above it
    CHECK_EQ(ui.page() == Page::Home, 1);
    // Bench -> Charger -> Test charger, then BACK climbs back: the charger test to its place, the place to the Bench.
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    ui.context().keep.bench_sel = static_cast<int>(bench::Place::Charger);
    ui.context().keep.place_sel[static_cast<int>(bench::Place::Charger)] = 0;
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);                       // the Charger place page over the Bench
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::ChargerTest, 1);
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    CHECK_EQ(int(ui.connection_page().tab()), int(conn::Tab::Source));
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    CHECK_EQ(ui.context().keep.bench_sel, static_cast<int>(bench::Place::Charger));   // the Bench, Charger focused
    ui.context().keep.bench_sel = static_cast<int>(bench::Place::Meter);   // the Meter row goes back to the faces
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    CHECK_EQ(ui.page() == Page::Home, 1);
    // A long BACK nobody takes lands on Flow, from a page or a submenu over it.
    ui.goto_page(static_cast<uint8_t>(Page::Settings), t);
    ui.on_button(Button::Back, ButtonEvent::Long, t);
    CHECK_EQ(ui.page() == Page::Home, 1);
    ui.goto_page(kDiagPage, t);
    CHECK_EQ(int(ui.menu_depth()), 1);
    ui.on_button(Button::Back, ButtonEvent::Long, t);
    CHECK_EQ(ui.page() == Page::Home, 1);
    CHECK_EQ(int(ui.menu_depth()), 0);
    // Back held 5 s is no reboot any more (OK + Back held 3 s is: test_reboot_hold_card); long Back on Home cycles the hero.
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Hold, t)), static_cast<int>(UiAction::None));
    CHECK_EQ(static_cast<int>(ui.context().keep.home_hero), static_cast<int>(home::Qty::W));
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Long, t)), static_cast<int>(UiAction::None));
    CHECK_EQ(static_cast<int>(ui.context().keep.home_hero), static_cast<int>(home::Qty::V));
    CHECK_EQ(ui.page() == Page::Home, 1);
    ui.set_home_hero(home::Qty::W);   // the Ui carries over between cases
}

static void test_short_presses_do_not_page() {
    Ui &ui = fresh_ui();
    fill_pdos(3);
    ui.goto_page(kPdPage, 20000);                                           // 3 PDOs: the list fits
    ui.on_button(Button::Plus, ButtonEvent::Short, 20000);
    ui.on_button(Button::Minus, ButtonEvent::Short, 20000);
    CHECK_EQ(on_pd(ui), 1);                                           // no fallback paging
    CHECK_EQ(ui.list_state().offset, 0);
    ui.goto_page(static_cast<uint8_t>(Page::Settings), 20000);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20000);            // a list page moves its focus, it never pages
    ui.on_button(Button::Minus, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Settings, 1);
}

static void test_back_goes_home() {
    Ui &ui = fresh_ui();
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 20000);
    ui.on_button(Button::Back, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Home, 1);   // one press home, from any rotation page
    ui.goto_page(kDiagPage, 20000);          // a Tools submenu opened from Home: BACK pops to the page under it
    CHECK_EQ(ui.menu() == Menu::Diag, 1);
    ui.on_button(Button::Back, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Home, 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 20000);   // opened from the Tools list: BACK returns there
    ui.push_menu(Menu::Diag, 20000);
    ui.on_button(Button::Back, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    ui.goto_page(0, 20000);
    ui.on_button(Button::Back, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Home, 1);
}

// A press lights its panel slot for at least 120 ms; a held button stays lit until release.
static void test_press_flash() {
    Ui &ui = fresh_ui();
    ui.set_pressed(Button::Ok, true, 1000);
    CHECK_EQ(ui.lit(Button::Ok) ? 1 : 0, 1);
    ui.set_pressed(Button::Ok, false, 1030);          // quick tap: released after 30 ms
    CHECK_EQ(ui.lit(Button::Ok) ? 1 : 0, 1);
    ui.tick(1100);
    CHECK_EQ(ui.lit(Button::Ok) ? 1 : 0, 1);
    ui.tick(1121);
    CHECK_EQ(ui.lit(Button::Ok) ? 1 : 0, 0);
    ui.set_pressed(Button::Back, true, 2000);         // long hold
    ui.tick(3500);
    CHECK_EQ(ui.lit(Button::Back) ? 1 : 0, 1);
    ui.set_pressed(Button::Back, false, 3600);
    CHECK_EQ(ui.lit(Button::Back) ? 1 : 0, 0);
}

// OK + Back held together (user decision 2026-11-12): the HoldCard appears at 1 s and counts down, tick() asks for the
// reboot once at 3 s, a release cancels it and repaints the page. Back alone never shows it, and the chord's keys do
// nothing on their own until each is pressed afresh.
static void test_reboot_hold_card() {
    constexpr int kNone = static_cast<int>(UiAction::None), kReboot = static_cast<int>(UiAction::Reboot);
    auto card = [](Ui &u) { return u.hold_card_visible() ? 1 : 0; };
    Ui &ui = fresh_ui();
    ui.goto_page(0, 20000);
    ui.set_home_hero(home::Qty::W);
    ui.set_pressed(Button::Back, true, 30000);       // Back alone, held past the old 5 s: no card, no reboot
    CHECK_EQ(static_cast<int>(ui.tick(31000)), kNone);
    CHECK_EQ(card(ui), 0);
    CHECK_EQ(static_cast<int>(ui.tick(35500)), kNone);
    CHECK_EQ(card(ui), 0);
    ui.set_pressed(Button::Back, false, 35600);
    ui.tick(35700);

    ui.set_pressed(Button::Ok, true, 40000);
    ui.set_pressed(Button::Back, true, 40100);       // the chord starts with the second key
    CHECK_EQ(static_cast<int>(ui.tick(41000)), kNone);
    CHECK_EQ(card(ui), 0);
    CHECK_EQ(static_cast<int>(ui.tick(41100)), kNone);
    CHECK_EQ(card(ui), 1);                            // 1 s: the card
    CHECK_EQ(static_cast<int>(ui.tick(42000)), kNone);
    CHECK_EQ(card(ui), 1);
    ui.set_pressed(Button::Ok, false, 42500);        // OK let go at 2.4 s: cancelled
    CHECK_EQ(static_cast<int>(ui.tick(42520)), kNone);
    CHECK_EQ(card(ui), 0);
    CHECK_EQ(static_cast<int>(ui.tick(43200)), kNone);   // still held Back alone: nothing comes back
    CHECK_EQ(card(ui), 0);
    // The keys' own events (Back's Long = the hero cycle, OK's Short = zoom) are dropped until a fresh press.
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Long, 43300)), kNone);
    CHECK_EQ(static_cast<int>(ui.context().keep.home_hero), static_cast<int>(home::Qty::W));
    ui.set_pressed(Button::Back, false, 43400);
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Short, 43400)), kNone);
    CHECK_EQ(ui.page() == Page::Home, 1);
    ui.set_pressed(Button::Back, true, 44000);       // pressed afresh: Back works again
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Long, 44500)), kNone);
    CHECK_EQ(static_cast<int>(ui.context().keep.home_hero), static_cast<int>(home::Qty::V));
    ui.set_pressed(Button::Back, false, 44600);

    // Held 3 s: the reboot, asked once, from a submenu as well as from a page.
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 50000);
    ui.push_menu(Menu::Diag, 50000);
    ui.set_pressed(Button::Back, true, 50100);
    ui.set_pressed(Button::Ok, true, 50150);
    CHECK_EQ(static_cast<int>(ui.tick(51150)), kNone);
    CHECK_EQ(card(ui), 1);
    CHECK_EQ(static_cast<int>(ui.tick(53149)), kNone);
    CHECK_EQ(static_cast<int>(ui.tick(53150)), kReboot);
    CHECK_EQ(static_cast<int>(ui.tick(53300)), kNone);    // once
    CHECK_EQ(ui.menu() == Menu::Diag, 1);                  // neither key acted on the menu
    ui.set_pressed(Button::Ok, false, 53400);
    ui.set_pressed(Button::Back, false, 53400);
    ui.tick(53500);
    CHECK_EQ(card(ui), 0);
    // The remote's keys make the same chord.
    ui.set_remote_pressed(Button::Ok, true, 60000);
    ui.set_remote_pressed(Button::Back, true, 60000);
    ui.tick(61000);
    CHECK_EQ(card(ui), 1);
    ui.set_remote_pressed(Button::Ok, false, 61500);
    ui.set_remote_pressed(Button::Back, false, 61500);
    ui.tick(61600);
    CHECK_EQ(card(ui), 0);
    // A short tap never shows it (and pressing each key afresh ends the chord's suppression for the next cases).
    for (Button b : {Button::Back, Button::Ok}) {
        ui.set_pressed(b, true, 70000);
        ui.set_pressed(b, false, 70100);
    }
    ui.tick(70200);
    CHECK_EQ(card(ui), 0);
    ui.pop_menu(70300);
    ui.goto_page(0, 70400);
    ui.set_home_hero(home::Qty::W);
}

static void test_pd_list_with_seven_pdos() {
    Ui &ui = fresh_ui();
    fill_pdos(7);
    ui.goto_page(kPdPage, 20000);
    // Short +/- switches the Source / Cable / Sink tab and stays on the page.
    ui.on_button(Button::Plus, ButtonEvent::Short, 20000);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20010);
    ui.on_button(Button::Minus, ButtonEvent::Short, 20020);
    CHECK_EQ(on_pd(ui), 1);
}

// One attach = one list: offers only ever get added, and a detach empties it (no "last charger").
static void test_pd_list_clears_on_detach() {
    Ui &ui = fresh_ui();
    ui.debug_link(true, 21000);
    fill_pdos(5);
    CHECK_EQ(match_live(ui.link()).rows, 5);
    fill_pdos(3);                                   // the source now offers fewer: the list keeps all five
    CHECK_EQ(match_live(ui.link()).rows, 5);
    g_stub.clear_sources = 0;
    g_stub.cc_detaches = 0;
    ui.debug_link(false, 22000);
    ui.tick(22100);
    CHECK_EQ(match_live(ui.link()).rows, 0);
    CHECK_EQ(g_stub.clear_sources >= 1, 1);         // the driver's copy goes too, so the remote agrees
    CHECK_EQ(g_stub.cc_detaches, 1);                // and the chip really drops Rd, not only the display
    ui.goto_page(kPdPage, 22200);
    ui.tick(22300);
}

static void test_debug_fake_zero_clears() {
    // `debug pdos --fake 0` drops the injected list, the forced link and the driver's list.
    Ui &ui = fresh_ui();
    ui.debug_fake_pdos(7, 23000);
    CHECK_EQ(pd::active_caps().num_pdos, 7);
    CHECK_EQ(match_live(ui.link()).rows, 7); // observe the fake book before checking its removal
    CHECK_EQ(int(pd::is_fake()), 1);
    g_stub.clear_sources = 0;
    g_stub.caps.num_pdos = 0;
    ui.debug_fake_pdos(0, 23100);
    CHECK_EQ(int(pd::is_fake()), 0);
    CHECK_EQ(pd::active_caps().num_pdos, 0);
    CHECK_EQ(g_stub.clear_sources >= 1, 1);
    ui.tick(23200);
    CHECK_EQ(match_live(ui.link()).rows, 0);
}

static void test_fake_zero_leaves_no_gone_rows() {
    // QA: after `--fake 0` the fake rows stayed on the PD list as GONE when the real charger's list was
    // a prefix of the fake one. The book starts over when the fake list goes, and on a new session.
    Ui &ui = fresh_ui();
    ui.debug_fake_pdos(7, 24000);
    ui.tick(24010);
    CHECK_EQ(match_live(ui.link()).rows, 7);
    pd::SourceCaps real = pd::active_caps();    // the fake list, cut to two offers
    real.num_pdos = 2;
    ui.debug_fake_pdos(0, 24100);
    g_stub.caps = real;                          // the real charger's caps arrive afterwards
    ui.tick(24200);
    MatchResult r = match_live(ui.link());
    CHECK_EQ(r.rows, 2);
    CHECK_EQ(r.gone, 0);
    // A new attach session (the detach went unseen) with a shorter list of the same charger kind.
    g_stub.caps.num_pdos = 1;
    g_stub.session += 1;
    r = match_live(ui.link());
    CHECK_EQ(r.rows, 1);
    CHECK_EQ(r.gone, 0);
}

static void test_diag_is_fixed() {
    // Diag v2 is three cards (a Tools submenu): nothing scrolls and short +/- do nothing.
    Ui &ui = fresh_ui();
    ui.goto_page(kDiagPage, 20000);
    CHECK_EQ(ui.list_state().overflows(), 0);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20000);
    ui.on_button(Button::Minus, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.list_state().offset, 0);
    CHECK_EQ(on_diag(ui), 1);
}

// Connection's Cable tab: with no e-marker data the pop-up greys "Save cable" and "Share" and saving does nothing;
// a real e-marker enables them.
static void test_cable_tab_not_found_greys_save() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    g_stub.em = {};                                     // valid = false, defaults 3A / 20V / USB 2.0
    ui.goto_page(kPdPage + 1, 20000);                   // Cable tab
    ui.tick(20050);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20100);   // pop-up: Read cable, Save cable (greyed), ...
    ui.on_button(Button::Plus, ButtonEvent::Short, 20150);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20200);   // greyed: refused, nothing saved
    CHECK_EQ(g_stub.lib_saves, 0);
    g_stub.em.valid = true;
    g_stub.em.num_vdos = 5;
    g_stub.em.spec_rev = 2;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    ui.tick(20300);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20400);   // the pop-up is still open on Save cable
    CHECK_EQ(g_stub.lib_saves, 1);
    g_stub = StubState();
}

static void test_cable_tab_save_and_list() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    g_stub.em = {};
    g_stub.em.valid = true;
    g_stub.em.num_vdos = 5;
    g_stub.em.spec_rev = 2;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    ui.goto_page(kPdPage + 1, 20000);                       // Cable tab
    ui.tick(20050);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20100);   // pop-up
    CHECK_STR(label(ui, Button::Ok), "Select");
    ui.on_button(Button::Plus, ButtonEvent::Short, 20150); // Save cable
    ui.on_button(Button::Ok, ButtonEvent::Short, 20300);   // direct, no confirmation, no address
    CHECK_EQ(g_stub.lib_saves, 1);
    CHECK_EQ(int(g_stub.lib.size()), 1);
    ui.tick(20350);                                         // toast painted
    ui.on_button(Button::Ok, ButtonEvent::Short, 20400);   // the pop-up again
    ui.on_button(Button::Plus, ButtonEvent::Short, 20410);
    ui.on_button(Button::Ok, ButtonEvent::Short, 20420);   // the same cable again: nothing new
    CHECK_EQ(int(g_stub.lib.size()), 1);
    ui.tick(24000);                                         // toast expired
    ui.on_button(Button::Ok, ButtonEvent::Short, 24100);   // pop-up: Saved cables is the fourth row
    for (int i = 0; i < 3; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 24110 + i);
    ui.on_button(Button::Ok, ButtonEvent::Short, 24200);
    ui.tick(24250);
    CHECK_EQ(ui.menu() == Menu::Emarker, 1);
    CHECK_EQ(ui.menu_depth(), 2u);                          // Connection > Saved cables (opened from Home, no Tools under it)
    CHECK_STR(label(ui, Button::Ok), "View");
    ui.on_button(Button::Ok, ButtonEvent::Short, 24300);   // detail card
    CHECK_STR(label(ui, Button::Ok), "Delete");
    ui.on_button(Button::Ok, ButtonEvent::Short, 24400);   // ask
    CHECK_EQ(g_stub.lib_removes, 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 24500); // keep
    CHECK_EQ(g_stub.lib_removes, 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, 24600);
    ui.on_button(Button::Ok, ButtonEvent::Short, 24700);   // confirm
    CHECK_EQ(g_stub.lib_removes, 1);
    CHECK_EQ(int(g_stub.lib.size()), 0);
    ui.tick(24750);
    ui.on_button(Button::Back, ButtonEvent::Short, 24800); // list -> back to Connection
    ui.tick(24850);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    // Library full and flash failures reach the user as a toast, not a state change.
    ui.on_button(Button::Ok, ButtonEvent::Short, 25000);
    g_stub.lib_err = emlib::Err::Full;
    ui.on_button(Button::Plus, ButtonEvent::Short, 25050);
    ui.on_button(Button::Ok, ButtonEvent::Short, 25100);
    CHECK_EQ(int(g_stub.lib.size()), 0);
    g_stub = StubState();
}

static void test_spoof_tool() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    using Mark = CableIdentityList::Mark;
    const auto builtin = [](uint16_t id) { return ctcable::Choice{ctcable::Kind::Builtin, id}; };
    ui.push_menu(Menu::Spoof, 30000);
    ui.tick(30050);
    const CableIdentityList &cl = ui.context().cables;           // the one list the wizard's Cable step shares
    CHECK_EQ(cl.count(), CableIdentityList::kFirstSaved);        // Off + the built-in chips, nothing saved
    CHECK_STR(label(ui, Button::Ok), "");                        // the cursor starts on Off: nothing armed, nothing to do
    ui.on_button(Button::Ok, ButtonEvent::Short, 30100);
    CHECK_EQ(g_stub.spoof_arms, 0);
    CHECK_EQ(g_stub.spoof_disarms, 0);
    // A saved clone is a row of the same list.
    g_stub.em = {}; g_stub.em.valid = true; g_stub.em.num_vdos = 5;
    g_stub.em.spec_rev = 2;
    g_stub.em.raw_vdos = {0xFF00A041, (3u << 27) | 0x05ACu, 0, 0x12340005, (2u << 21) | (3u << 9) | (2u << 5)};
    CHECK_EQ(int(storage::lib_save_current().err), int(emlib::Err::Ok));
    ui.on_button(Button::Back, ButtonEvent::Short, 30200);
    ui.push_menu(Menu::Spoof, 30300);                            // re-enter: the list is re-read
    ui.tick(30350);
    CHECK_EQ(cl.count(), CableIdentityList::kFirstSaved + 1);
    ui.on_button(Button::Plus, ButtonEvent::Short, 30360);       // row 1: the first built-in chip
    ui.tick(30370);
    CHECK_STR(label(ui, Button::Ok), "Arm");
    g_stub.spoof_err = emk_spoof::Err::Inline;                   // a live sink refuses the arm
    ui.on_button(Button::Ok, ButtonEvent::Short, 30400);
    CHECK_EQ(g_stub.spoof_arms, 1);
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 0);
    CHECK_EQ(cl.armed(), false);
    g_stub.spoof_err = emk_spoof::Err::Ok;
    ui.on_button(Button::Ok, ButtonEvent::Short, 30500);         // picking a row on this page selects and arms it
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 1);
    CHECK_EQ(cl.armed_choice() == builtin(1), true);
    CHECK_EQ(cl.selected() == builtin(1), true);
    CHECK_EQ(cl.row(1).mark == Mark::Armed, true);
    ui.tick(30550);
    CHECK_STR(label(ui, Button::Ok), "Disarm");
    g_stub.spoof_requests = 3;
    ui.tick(30600);                                              // counters repaint without a crash
    ui.on_button(Button::Plus, ButtonEvent::Short, 30650);       // another identity: OK re-arms with that one
    ui.tick(30660);
    CHECK_STR(label(ui, Button::Ok), "Arm");
    ui.on_button(Button::Ok, ButtonEvent::Short, 30670);
    CHECK_EQ(g_stub.spoof_arms, 3);
    CHECK_EQ(cl.armed_choice() == builtin(2), true);
    CHECK_EQ(cl.row(1).mark == Mark::None, true);
    CHECK_EQ(cl.row(2).mark == Mark::Armed, true);
    ui.on_button(Button::Ok, ButtonEvent::Short, 30700);         // OK on the armed row disarms; the selection stays
    CHECK_EQ(g_stub.spoof_disarms, 1);
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 0);
    CHECK_EQ(cl.selected() == builtin(2), true);
    CHECK_EQ(cl.row(2).mark == Mark::Use, true);
    for (int i = 0; i < 3; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 30710 + i);   // the saved clone, row 5
    ui.on_button(Button::Ok, ButtonEvent::Short, 30720);
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 1);
    CHECK_EQ(cl.armed_choice().kind == ctcable::Kind::Saved, true);
    g_stub.spoof_generation++;                                   // an arm made elsewhere replaces this one: no marker
    ui.tick(30750);
    CHECK_EQ(cl.armed(), false);
    g_stub.spoof_on = false; g_stub.spoof_arms = 0; g_stub.spoof_disarms = 0;
    ui.on_button(Button::Back, ButtonEvent::Short, 30800);
    g_stub = StubState();
}

// An identity armed without this list (a remote command, an older arm) is "external": its row is not marked, and the
// Off row's OK turns it off.
static void test_spoof_external_identity() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    g_stub.spoof_on = true; ++g_stub.spoof_generation;
    ui.push_menu(Menu::Spoof, 31000);
    ui.tick(31050);
    CHECK_EQ(ui.context().cables.armed(), false);
    CHECK_STR(label(ui, Button::Ok), "Disarm");
    ui.on_button(Button::Ok, ButtonEvent::Short, 31100);         // Off row
    CHECK_EQ(g_stub.spoof_disarms, 1);
    CHECK_EQ(g_stub.spoof_on ? 1 : 0, 0);
    ui.on_button(Button::Back, ButtonEvent::Short, 31200);
    g_stub = StubState();
}

static void test_spoof_text() {
    StrBuf<24> c;
    spoof_counts_text(4, 3, c);
    CHECK_STR(std::string(c.view()).c_str(), "4 / 3");
}

static void test_em_row_text() {
    const std::uint32_t v[5] = {0xFF00A041, 0x1C00203C, 0x00000123, 0x00010000, 0x00082052};
    emlib::Entry r;
    r.rec = emrec::make(v, 2);
    const char nm[] = "Apple-USB4-240W";
    for (std::size_t i = 0; i < sizeof nm; i++) r.name[i] = nm[i];
    static const char *const kWant[kEmDetailRows] = {"Apple-USB4-240W", "203C 0001", "00000123", "3.x", "1C00203C",
                                                      "00082052", "5A 20V"};
    for (int i = 0; i < kEmDetailRows; i++) {
        StrBuf<24> s;
        em_detail_value(i, r, s);
        CHECK_STR(std::string(s.view()).c_str(), kWant[i]);
    }
}

static void test_page_actions() {
    Ui &ui = fresh_ui();
    // A short OK on Flow opens the Bench; a long OK resets the meter (the page consumes it).
    ui.on_button(Button::Ok, ButtonEvent::Short, 20000);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    ui.goto_page(0, 20010);
}

static void test_fake_pdos_on_the_page() {
    Ui &ui = fresh_ui();
    g_stub.caps = {};                                   // no charger
    ui.goto_page(kPdPage, 20000);
    pd::set_fake(7);
    ui.tick(20500);
    ui.run_action(Action::PdRescan, 21000);             // Rescan drops the fake caps
    CHECK_EQ(pd::is_fake(), 0);
    pd::set_fake(0);
}

static std::string text_of(int i, const std::optional<adc::Reading> &r, fastcharge::Protocol p) {
    StrBuf<24> s;
    lines_row_text(i, r, p, s);
    return std::string(s.view());
}

static void test_lines_rows() {
    adc::Reading r;
    r.dp_mv = 600; r.dm_mv = 598; r.cc1_mv = 1650; r.cc2_mv = 12;
    r.aux_mv = 8865; r.temp = {245, true, false}; r.vref_raw = 1489; r.vdd_mv = 3300;
    const std::optional<adc::Reading> some = r;
    CHECK_STR(text_of(0, some, fastcharge::Protocol::Dcp5V).c_str(), "D+   600 mV");
    CHECK_STR(text_of(1, some, fastcharge::Protocol::Dcp5V).c_str(), "D-   598 mV");
    CHECK_STR(text_of(2, some, fastcharge::Protocol::Dcp5V).c_str(), "CC1  1650 mV");
    CHECK_STR(text_of(3, some, fastcharge::Protocol::Dcp5V).c_str(), "CC2  12 mV");
    CHECK_STR(text_of(4, some, fastcharge::Protocol::Dcp5V).c_str(), "AUX  8.865 V");
    // MCU die sensor row, then the board NTC row (no exposed channel tracks it: always n/a).
    r.mcu = {371, true, false};
    const std::optional<adc::Reading> hot = r;
    CHECK_STR(text_of(5, hot, fastcharge::Protocol::Dcp5V).c_str(), "MCU  37.1 C uncal");
    CHECK_STR(text_of(6, hot, fastcharge::Protocol::Dcp5V).c_str(), "BRD  n/a (no sensor)");
    r.temp = {0, false, false};
    CHECK_STR(text_of(6, r, fastcharge::Protocol::Dcp5V).c_str(), "BRD  n/a (no sensor)");
    r.mcu = {-35, true, true};
    CHECK_STR(text_of(5, r, fastcharge::Protocol::Pd).c_str(), "MCU  -3.5 C");
    r.mcu = {0, false, false};
    CHECK_STR(text_of(5, r, fastcharge::Protocol::Pd).c_str(), "MCU  --");
    CHECK_STR(text_of(7, some, fastcharge::Protocol::Dcp5V).c_str(), "VREF 1489 3.30 V");
    CHECK_STR(text_of(8, some, fastcharge::Protocol::Dcp5V).c_str(), "FC   DCP 5V");
    // No reading yet: dashes, the verdict still shows.
    CHECK_STR(text_of(0, std::nullopt, fastcharge::Protocol::Pending).c_str(), "D+   --");
    CHECK_STR(text_of(7, std::nullopt, fastcharge::Protocol::Pending).c_str(), "VREF --");
    CHECK_STR(text_of(8, std::nullopt, fastcharge::Protocol::NoInput).c_str(), "FC   NO INPUT");
    // Every row fits the 22 character content width.
    for (int i = 0; i < kLinesRows; i++) CHECK_EQ(text_of(i, some, fastcharge::Protocol::Usb11Low).size() <= 22, 1);
}

static std::string bar_temp(std::optional<adc::Reading> r) {
    StrBuf<8> s;
    temp_short_text(r, s);
    return std::string(s.view());
}

static void test_temp_short() {
    // The Home status bar shows the MCU die temperature (the board NTC channel is unconfirmed).
    CHECK_STR(bar_temp(adc::Reading{.mcu = {245, true, false}}).c_str(), "MCU25C");   // 24.5 rounds away from zero
    CHECK_STR(bar_temp(adc::Reading{.mcu = {-35, true, false}}).c_str(), "MCU-4C");
    CHECK_STR(bar_temp(adc::Reading{.temp = {0, false, false}, .mcu = {371, true, false}}).c_str(), "MCU37C");
    CHECK_STR(bar_temp(adc::Reading{.mcu = {0, false, false}}).c_str(), "MCU --");
    CHECK_STR(bar_temp(std::nullopt).c_str(), "MCU --");
    // Never wider than the field: out-of-range values clamp to a marker.
    CHECK_STR(bar_temp(adc::Reading{.mcu = {1004, true, false}}).c_str(), "MCU>99");
    CHECK_STR(bar_temp(adc::Reading{.mcu = {-100, true, false}}).c_str(), "MCU<-9");
    CHECK_STR(bar_temp(adc::Reading{.mcu = {994, true, false}}).c_str(), "MCU99C");
}

// Home v2: gauges fill by V / A / W against the active PDO, the footer follows the state.
static void test_home_gauges_and_footer() {
    Ui &ui = fresh_ui();
    g_stub.caps = {};
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0].type = pd::PdoType::Fixed; g_stub.caps.pdos[0].max_mv = 20000;
    g_stub.caps.pdos[0].max_ma = 3000; g_stub.caps.pdos[0].max_mw = 60000;
    g_stub.contract = {};
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 1;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = 3000; g_stub.contract.max_ma = 3000;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 10000; g_stub.pm.current_ma = 1500; g_stub.pm.power_mw = 15000;
    g_stub.pm.flow = power::Flow::Forward;
    ui.tick(70000);
    ui.tick(70200);
    auto px = [](int x, int y) { return g_panel[y * 240 + x]; };
    // Home v3: the readout digits and the three strips carry the measurement colours.
    auto inkc = [&](int x0, int y0, int w, int h, uint16_t c) {
        int n = 0;
        for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) n += px(x, y) == c;
        return n;
    };
    CHECK_EQ(inkc(38, 25, 160, 39, kWatt) > 60, 1);    // Home 2c: the 64 px W hero
    CHECK_EQ(inkc(8, 99, 120, 30, kVolt) > 40, 1);     // the 48 px VBUS ledger row
    CHECK_EQ(inkc(8, 156, 120, 30, kAmp) > 40, 1);     // the 48 px IBUS ledger row
    // A live reading carries no red; no meter shows the red message in the protocol row.
    auto count = [&](int x0, int x1, uint16_t col) {
        int n = 0;
        for (int x = x0; x < x1; x++) for (int y = 0; y < 240; y++) n += px(x, y) == col;
        return n;
    };
    CHECK_EQ(count(0, 224, kError), 0);
    g_stub.pm_online = false;
    ui.tick(70400);
    CHECK_EQ(count(4, 224, kError) > 10, 1);
    CHECK_EQ(count(4, 224, kVolt), 0);   // x0..3: the idle field's colour key
    g_stub.pm_online = true;
    g_stub.pm = {};
    g_stub.caps = {};
    g_stub.contract = {};
}

static std::string pill_of(fastcharge::Protocol p) {
    StrBuf<6> s;
    fc_pill_text(p, s);
    return std::string(s.view());
}

static void test_fc_pill_text() {
    using P = fastcharge::Protocol;
    // At most 4 characters (the totals card keeps the Wh unit clear of the pill); no verdict = no pill.
    CHECK_STR(pill_of(P::Qc3).c_str(), "QC3");
    CHECK_STR(pill_of(P::Qc2_9V).c_str(), "QC2");
    CHECK_STR(pill_of(P::AfcFcp).c_str(), "AFC");
    CHECK_STR(pill_of(P::Apple21).c_str(), "APL");
    CHECK_STR(pill_of(P::Svooc).c_str(), "VOOC");
    CHECK_STR(pill_of(P::VoocWarp).c_str(), "WARP");
    CHECK_STR(pill_of(P::Usb2Hi).c_str(), "USB2");
    CHECK_STR(pill_of(P::UsbData).c_str(), "USB");
    CHECK_STR(pill_of(P::PdMtk).c_str(), "PD");
    CHECK_STR(pill_of(P::Pd).c_str(), "PD");
    CHECK_STR(pill_of(P::PdLikely).c_str(), "PD?");
    CHECK_STR(pill_of(P::Pending).c_str(), "AI..");
    CHECK_STR(pill_of(P::NoInput).c_str(), "");
    CHECK_STR(pill_of(P::NoQuick).c_str(), "");
    for (int i = 0; i <= static_cast<int>(P::PdLikely); i++) CHECK_EQ(pill_of(static_cast<P>(i)).size() <= 4, 1);
}

static void test_lines_page() {
    Ui &ui = fresh_ui();
    adc::Reading r;
    r.dp_mv = 600; r.vref_raw = 1489; r.vdd_mv = 3300; r.mcu = {245, true, false};
    g_stub.adc = r;
    g_stub.fc = fastcharge::Protocol::Dcp5V;
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 20000);
    ui.tick(20500);
    CHECK_EQ(ui.page() == Page::Lines, 1);
    CHECK_EQ(ui.list_state().count, 0);                  // v2: a fixed layout, nothing scrolls
    CHECK_EQ(ui.list_state().overflows(), 0);
    g_stub.adc.reset();                                  // sensor gone: dashes, no crash
    ui.tick(21000);
    CHECK_EQ(ui.list_state().count, 0);
    ui.goto_page(kDiagPage, 22000);                              // Diag: fixed cards
    ui.tick(22500);
    CHECK_EQ(ui.list_state().count, 0);
    CHECK_EQ(ui.goto_page(250, 23000), false);             // an unknown index changes nothing
    CHECK_EQ(on_diag(ui), 1);
}

static std::string cable_text(int i, bool online, int v, int a, const cable_r::Meter &m) {
    StrBuf<24> s;
    cable_row_text(i, online, v, a, m, s);
    return std::string(s.view());
}

static void test_cable_rows() {
    cable_r::Meter m;
    CHECK_STR(cable_text(0, false, 0, 0, m).c_str(), "-- no power monitor");
    CHECK_STR(cable_text(0, true, 5120, -1980, m).c_str(), "5.12V 1.980A");
    CHECK_STR(cable_text(1, true, 5120, 0, m).c_str(), "IDLE waiting <0.2A");
    CHECK_STR(cable_text(2, true, 5120, 0, m).c_str(), "LOAD --");
    CHECK_STR(cable_text(3, true, 5120, 0, m).c_str(), "R --");
    CHECK_STR(cable_text(4, true, 5120, 0, m).c_str(), "WAITING FOR LOAD");
    m.sample(5100, 50);
    CHECK_STR(cable_text(1, true, 5100, 50, m).c_str(), "IDLE 5.10V 0.05A");
    CHECK_STR(cable_text(2, true, 5100, 50, m).c_str(), "LOAD need +1.0A step");
    m.sample(4900, 2050);          // dv 200 mV over di 2000 mA = 100 mohm
    CHECK_STR(cable_text(2, true, 4900, 2050, m).c_str(), "LOAD 4.90V 2.05A");
    CHECK_STR(cable_text(3, true, 4900, 2050, m).c_str(), "R 100 m\xCE\xA9");
    CHECK_STR(cable_text(4, true, 4900, 2050, m).c_str(), "EXCELLENT");
    for (int i = 0; i < kCableRows; i++) CHECK_EQ(cable_text(i, true, 4900, 2050, m).size() <= 22, 1);
}

static std::string kelvin_text(int i, bool online, int v, int a, const std::optional<cable_r::Kelvin> &k) {
    StrBuf<24> s;
    cable_kelvin_row_text(i, online, v, a, k, s);
    return std::string(s.view());
}

// Glyph count of a UTF-8 string (the Omega is two bytes, one 16 px cell).
static size_t glyphs(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s) n += (c & 0xC0) != 0x80;
    return n;
}

static void test_cable_kelvin_rows() {
    const auto none = std::optional<cable_r::Kelvin>();
    CHECK_STR(kelvin_text(0, false, 0, 0, none).c_str(), "-- no power monitor");
    CHECK_STR(kelvin_text(0, true, 5120, -1000, none).c_str(), "5.12V 1.000A");
    CHECK_STR(kelvin_text(1, true, 5120, 1000, none).c_str(), "POS -- GND --");
    CHECK_STR(kelvin_text(2, true, 5120, 300, none).c_str(), "LOAD need 0.45A+");
    CHECK_STR(kelvin_text(2, true, 5120, -1000, none).c_str(), "SENSE check fixture");
    CHECK_STR(kelvin_text(3, true, 5120, 1000, none).c_str(), "R --");
    CHECK_STR(kelvin_text(4, true, 5120, 1000, none).c_str(), "WAITING FOR LOAD");
    const auto k = cable_r::kelvin(5000, 5120, 5070, 1000);   // 120 = 50 + 70 mohm at 1 A
    CHECK_STR(kelvin_text(1, true, 5120, 1000, k).c_str(), "POS 50 m\xCE\xA9 GND 70 m\xCE\xA9");
    CHECK_STR(kelvin_text(2, true, 5120, 1000, k).c_str(), "SENSE ok");
    CHECK_STR(kelvin_text(3, true, 5120, 1000, k).c_str(), "R 120 m\xCE\xA9");
    CHECK_STR(kelvin_text(4, true, 5120, 1000, k).c_str(), "EXCELLENT");
    const auto big = cable_r::kelvin(0, 2400, 1200, 1000);    // 1200 + 1200 mohm: widest legs "1.20 ohm"
    for (int i = 0; i < kCableRows; i++) {
        CHECK_EQ(glyphs(kelvin_text(i, true, 20000, -1000, k)) <= 21, 1);
        CHECK_EQ(glyphs(kelvin_text(i, true, 20000, -1000, big)) <= 21, 1);
    }
}

static void test_cable_mode_toggle() {
    Ui &ui = fresh_ui();
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 20000);
    ui.push_menu(Menu::CableR, 20100);
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(cable_r::Mode::Passive));
    CHECK_STR(label(ui, Button::Plus), "Mode");
    CHECK_EQ(ui.labels().b[static_cast<int>(Button::Plus)].dim, false);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20200);
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(cable_r::Mode::Kelvin));
    ui.on_button(Button::Plus, ButtonEvent::Short, 20300);
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(cable_r::Mode::Passive));
    ui.set_cable_mode(cable_r::Mode::Kelvin);   // the remote's path
    CHECK_EQ(static_cast<int>(ui.cable_mode()), static_cast<int>(cable_r::Mode::Kelvin));
    ui.set_cable_mode(cable_r::Mode::Passive);
    ui.pop_menu(20400);
}

// Known gap: a label wider than its slot (and with no shorter spelling) is blanked by label_text(). Every
// page and submenu, in every state the label sets depend on, must show something for every slot that
// has a label, and what it shows must fit the slot's pixels. A label with a deliberate lng_alt (the
// "Prv" of "-/Prev" on a list menu) may drop to it; a plain short word must never be dropped.
static void test_every_label_fits_its_slot() {
    Ui &ui = fresh_ui();
    int checked = 0;
    auto check_all = [&](const char *what) {
        // The strip vocabulary (Up/Dn on lists, -/+ on values, OK a verb, BACK always Back).
        const char *bad = label_violation(ui.labels());
        if (bad) printf("strip vocabulary: %s: %s\n", what, bad);
        CHECK_EQ(bad == nullptr, 1);
        for (int b = 0; b < 4; b++) {
            const SlotLabel &l = ui.labels().b[b];
            const unsigned room = label_max_chars(static_cast<Button>(b));
            char fit[24], full[24];
            const unsigned got = label_text(fit, l, room);
            const unsigned want = label_text(full, l, 23);
            if (want > 0 && got == 0)
                printf("label blanked: %s button %d \"%s\" (%u chars, room for %u)\n", what, b, full, want, room);
            CHECK_EQ(want > 0 ? got > 0 : got == 0, 1);              // never blanked
            CHECK_EQ(got <= room, 1);
            const int slot_len = static_cast<int>(b == 0 || b == 3 ? kSlotLenPx[0] : kSlotLenPx[1]);
            CHECK_EQ(static_cast<int>(got * kLabelFont.adv) + kDividerPx + 2 * kSlotPadPx <= slot_len, 1);
            CHECK_EQ(strlen(fit), got);
            // The short word alone always survives (the long press / alternative is what may be cut).
            if (l.shrt[0]) CHECK_EQ(strncmp(fit, l.shrt, strlen(l.shrt)), 0);
            checked++;
        }
    };
    for (uint8_t p = 0; p < static_cast<uint8_t>(Page::Count); p++) {
        ui.goto_page(p, 20000);
        check_all("page");
    }
    for (int m = 0; m < static_cast<int>(Menu::Count); m++) {
        ui.goto_page(static_cast<uint8_t>(Page::Tools), 21000);
        ui.push_menu(static_cast<Menu>(m), 21100);
        check_all("menu");
        fill_pdos(7);
        for (int st = 0; st < 4; st++) {
            g_stub.fc_state = st;
            ui.tick(21200 + st * 300);
            check_all("menu state");
        }
        g_stub.fc_state = 0;
        ui.pop_menu(22000);
    }
    CHECK_EQ(checked >= 4 * (static_cast<int>(Page::Count) + static_cast<int>(Menu::Count)), 1);
}

static void test_ai_rows_on_lines() {
    // The PD power summary (ai::recognise) rides in the verdict card's second pill; drawing it with live
    // caps, then without a reading, must not crash and the page stays a fixed layout.
    Ui &ui = fresh_ui();
    g_stub.fc = fastcharge::Protocol::Pd;
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 20000);
    ui.tick(20500);
    CHECK_EQ(ui.list_state().count, 0);
    ui.on_button(Button::Plus, ButtonEvent::Short, 20600);   // [+] pages on, never scrolls a list
    ui.tick(20700);
}

static constexpr uint32_t kT0 = 40000;

// The row of tool slot `tool` in place `p` (-1 when the place has no such action).
static int act_row(bench::Place p, int tool) {
    const bench::Acts a = bench::actions(p);
    if (p == bench::Place::Charger || p == bench::Place::Cable || p == bench::Place::Device) {   // a place page's rows
        const conn::Tab tab = p == bench::Place::Charger ? conn::Tab::Source : p == bench::Place::Cable ? conn::Tab::Cable : conn::Tab::Sink;
        std::array<conn::Item, conn::kMaxItems> it{};
        const int n = conn::place_actions(tab, conn::ActFacts{}, it);
        for (int i = 1; i < n; i++) {
            const conn::Item &x = it[static_cast<size_t>(i)];
            if (x.act == conn::Act::Bench && a.a[x.arg].kind == bench::ActKind::Tool && a.a[x.arg].arg == tool) return i - 1;
        }
        return -1;
    }
    if (p == bench::Place::Log) {   // the Log place is the PD diagnosis: its findings (less the one the stamp names), then the Black Box row
        const pd::diag::Report &r = pd::diag::live::report();
        return tool == 7 ? std::max<int>(1, static_cast<int>(pd::diag::screen::row_count(r)) - (pd::diag::screen::names_top_finding(r) ? 1 : 0)) : -1;
    }
    for (int i = 0; i < a.n; i++)
        if (a.a[i].kind == bench::ActKind::Tool && a.a[i].arg == tool) return i;
    return -1;
}

// Bench -> place -> the action's row -> OK, by key presses from the place's top row.
static void open_act(Ui &ui, bench::Place p, int tool, uint32_t t) {
    ui.context().keep.place_sel[static_cast<int>(p)] = 0;
    ui.open_place(p, t);
    for (int i = 0; i < act_row(p, tool); i++) ui.on_button(Button::Plus, ButtonEvent::Short, t + 5);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 10);
}

static void open_trigger(Ui &ui, uint32_t t) { open_act(ui, bench::Place::Charger, 0, t); }   // Charger > Fast charge

static void press(Ui &ui, Button b, ButtonEvent e, uint32_t t) { ui.on_button(b, e, t); }

static void test_tools_hub() {
    Ui &ui = fresh_ui();
    g_stub.fc_stops = 0;
    ui.goto_page(static_cast<uint8_t>(Page::Tools), kT0);
    CHECK_EQ(ui.list_state().count, 0);   // fixed rows, no scroll
    CHECK_EQ(ui.menu_depth(), 0u);
    // The Bench: short +/- move between the six places (no wrap, no paging), OK opens one on the same page.
    ui.context().keep.bench_sel = 0;
    press(ui, Button::Minus, ButtonEvent::Short, kT0 + 10);
    CHECK_EQ(ui.context().keep.bench_sel, 0);
    for (int i = 0; i < 8; i++) press(ui, Button::Plus, ButtonEvent::Short, kT0 + 20);
    CHECK_EQ(ui.context().keep.bench_sel, bench::kPlaces - 1);
    for (int i = 0; i < 3; i++) press(ui, Button::Minus, ButtonEvent::Short, kT0 + 30);   // Cable
    ui.context().keep.place_sel[static_cast<int>(bench::Place::Cable)] = 0;
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 35);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);   // the Cable place page over the Bench
    CHECK_EQ(int(ui.connection_page().tab()), int(conn::Tab::Cable));
    // In a place, short +/- select an action and OK opens it: Cable > Resistance is Cable R.
    for (int i = 0; i < act_row(bench::Place::Cable, 3); i++) press(ui, Button::Plus, ButtonEvent::Short, kT0 + 38);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 40);
    CHECK_EQ(ui.menu_depth(), 2u);
    CHECK_EQ(ui.menu() == Menu::CableR, 1);
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<uint8_t>(Menu::CableR));
    // BACK returns to the place, with the selection kept.
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 50);
    CHECK_EQ(ui.menu_depth(), 1u);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    CHECK_EQ(ui.connection_page().action_row(), act_row(bench::Place::Cable, 3));
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 60);
    CHECK_EQ(ui.menu() == Menu::CableR, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 70);
    open_trigger(ui, kT0 + 90);
    CHECK_EQ(ui.menu() == Menu::Trigger, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 100);
    open_act(ui, bench::Place::Charger, 1, kT0 + 110);   // Request voltage: the PD trigger
    CHECK_EQ(ui.menu() == Menu::PdTrigger, 1);
    // Long +/- page nothing in a tool.
    press(ui, Button::Plus, ButtonEvent::Long, kT0 + 130);
    CHECK_EQ(ui.menu() == Menu::PdTrigger, 1);
    CHECK_STR(label(ui, Button::Back), "Back");
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 140);
    // BACK from the tool goes back to the Charger place page, a second BACK to the Bench.
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 180);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    CHECK_EQ(ui.context().keep.bench_sel, int(bench::Place::Charger));
    CHECK_EQ(g_stub.fc_stops, 0);                  // nothing was triggered: leaving the trigger tool left the pins alone
}

static void test_submenu_is_modal() {
    Ui ui; ui.init(10000);
    g_stub.fc_stops = g_stub.fc_state = 0;
    open_trigger(ui, kT0);
    CHECK_EQ(ui.menu() == Menu::Trigger, 1);
    const int stops = g_stub.fc_stops;
    // Long +/- on protocol selection do nothing; the modal still suppresses paging.
    press(ui, Button::Plus, ButtonEvent::Long, kT0 + 100);
    press(ui, Button::Minus, ButtonEvent::Long, kT0 + 110);
    CHECK_EQ(ui.page() == Page::Tools, 1);
    CHECK_EQ(ui.menu() == Menu::Trigger, 1);
    // A short BACK pops; nothing was triggered, so the bus is left alone (no dpdm reset pulse).
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 120);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);   // back on the Charger place page
    CHECK_EQ(g_stub.fc_stops, stops);
    // Back in the place: a long BACK goes straight to the Meter.
    press(ui, Button::Back, ButtonEvent::Long, kT0 + 130);
    CHECK_EQ(ui.page() == Page::Home, 1);
    // Picking/editing/arming is passive; BACK retreats without resetting D+/D-.
    open_trigger(ui, kT0 + 150);
    const int s1 = g_stub.fc_stops;
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 160);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 162);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 164);
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 170);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 172);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 174);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    CHECK_EQ(g_stub.fc_stops, s1);
    // Failed state still pending: BACK resets as well.
    open_trigger(ui, kT0 + 180);
    g_stub.fc_state = static_cast<int>(fc::State::Failed);
    ui.tick(kT0 + 190);
    const int s3 = g_stub.fc_stops;
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 195);
    CHECK_EQ(g_stub.fc_stops, s3 + 1);
    g_stub.fc_state = 0;
    g_stub.pm.vbus_mv = 5000;
    for (int n = 0; n < 6; ++n) ui.tick(kT0 + 200 + n * 100);
    // The BACK hold no longer reboots, here either (the OK + Back chord does: test_reboot_hold_card).
    open_trigger(ui, kT0 + 800);
    CHECK_EQ(static_cast<int>(ui.on_button(Button::Back, ButtonEvent::Hold, kT0 + 900)), static_cast<int>(UiAction::None));
    // The remote's set-page closes the submenu and resets when something was triggered.
    open_trigger(ui, kT0 + 950);
    g_stub.fc_state = static_cast<int>(fc::State::Active);
    ui.tick(kT0 + 960);
    const int s2 = g_stub.fc_stops;
    CHECK_EQ(ui.goto_page(static_cast<uint8_t>(Page::Home), kT0 + 1000), 1);
    CHECK_EQ(ui.menu_depth(), 0u);
    CHECK_EQ(g_stub.fc_stops, s2 + 1);
    g_stub.fc_state = 0;
    for (int n = 0; n < 6; ++n) ui.tick(kT0 + 1100 + n * 100);
    // The stack is three deep at most (Tools > Connection > Saved cables / Spoof / Share).
    CHECK_EQ(ui.push_menu(Menu::Trigger, kT0 + 1700), 1);
    CHECK_EQ(ui.push_menu(Menu::Emarker, kT0 + 1710), 1);
    CHECK_EQ(ui.push_menu(Menu::Connection, kT0 + 1715), 1);
    CHECK_EQ(ui.push_menu(Menu::CableR, kT0 + 1720), 0);
    CHECK_EQ(ui.pop_menu(kT0 + 1725), 1);
    CHECK_EQ(ui.menu() == Menu::Emarker, 1);
    CHECK_EQ(ui.pop_menu(kT0 + 1730), 1);
    CHECK_EQ(ui.menu() == Menu::Trigger, 1);
    ui.goto_page(0, kT0 + 1800);
}

static void test_trigger_submenu() {
    Ui ui; ui.init(10000);
    g_stub.fc_stops = g_stub.fc_scans = g_stub.fc_state = g_stub.fc_starts = g_stub.fc_inline_ok = 0;
    g_stub.fc_retargets = g_stub.fc_start_fail = g_stub.fc_retarget_fail = 0;
    uint32_t t = kT0 + 1000;
    g_stub.caps.active_cc = 0; g_stub.link_inline = false;
    open_trigger(ui, t);
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0;
    ui.tick(t += 100);
    CHECK_STR(label(ui, Button::Plus), "Dn");
    CHECK_STR(label(ui, Button::Minus), "Up");
    CHECK_STR(label(ui, Button::Ok), "Next");
    CHECK_STR(label(ui, Button::Back), "Back");
    press(ui, Button::Plus, ButtonEvent::Short, t += 100); // QC3
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);   // draft editor
    CHECK_STR(label(ui, Button::Minus), "-/Big*");
    CHECK_EQ(g_stub.fc_starts, 0);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);   // review
    CHECK_STR(label(ui, Button::Ok), "Send");
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_starts, 0);
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_starts, 1);
    g_stub.fc_state = static_cast<int>(fc::State::Active);
    g_stub.fc_proto = int(fc::Proto::Qc3); g_stub.fc_target_mv = 9000;
    ui.tick(t += 100);
    CHECK_STR(label(ui, Button::Ok), "");      // BACK reads Stop while a level is live (UI_SYSTEM.md 7.13.1)
    CHECK_STR(label(ui, Button::Back), "Stop");
    press(ui, Button::Plus, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_retargets, 0);
    CHECK_STR(label(ui, Button::Ok), "Next");
    press(ui, Button::Ok, ButtonEvent::Short, t += 100); // review draft 9.2V, held 9V
    press(ui, Button::Ok, ButtonEvent::Short, t += 100); // arm
    CHECK_EQ(g_stub.fc_retargets, 0);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_retargets, 1);
    CHECK_EQ(g_stub.fc_retarget_mv, 9200);
    const int stops = g_stub.fc_stops;
    press(ui, Button::Minus, ButtonEvent::Short, t += 100); // downward draft also needs confirmation
    press(ui, Button::Back, ButtonEvent::Long, t += 100);  // stop-first, even in editor
    CHECK_EQ(g_stub.fc_stops, stops + 1);
    CHECK_EQ(ui.menu() == Menu::Trigger, 1);
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_EQ(ui.run_action(Action::FcScan, t += 100), false);
    CHECK_EQ(int(ui.trigger({1, 0, 0}, t += 100, false).err), int(pd::Err::Busy));
    // Leaving during cleanup must not drop the guard.
    press(ui, Button::Back, ButtonEvent::Short, t += 100);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_EQ(ui.push_menu(Menu::PdTrigger, t += 100), false);
    CHECK_EQ(ui.goto_page(uint8_t(Page::ChargerTest), t += 100), true);
    ui.tick(t += 100);
    const int requests_before = g_stub.requests, starts_before = g_stub.fc_starts;
    for (int n = 0; n < int(charger_test::Group::Count) + 3; ++n)
        press(ui, Button::Plus, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.requests, requests_before);
    CHECK_EQ(g_stub.fc_starts, starts_before);
    CHECK_EQ(ui.charger_test_busy(), false);
    ui.goto_page(kPdPage, t += 100);
    const int scans_before = g_stub.fc_scans;
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_scans, scans_before);
    g_stub.fc_state = 0;
    g_stub.pm.vbus_mv = 9000;
    ui.tick(t += 8100);
    CHECK_EQ(int(ui.fc_release().state()), int(trig::Release::State::Unsafe));
    CHECK_EQ(ui.ports_busy(), true);
    g_stub.pm.vbus_mv = 5000;
    for (int n = 0; n < 6; ++n) ui.tick(t += 100);
    CHECK_EQ(ui.ports_busy(), false);
    CHECK_EQ(int(ui.fc_release().state()), int(trig::Release::State::Safe));
    ui.goto_page(uint8_t(Page::Tools), t += 100);
    // Initial arm timeout and source-session changes cannot send stale requests.
    open_trigger(ui, t += 1000);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    const int starts = g_stub.fc_starts;
    press(ui, Button::Ok, ButtonEvent::Short, t += 3100);
    CHECK_EQ(g_stub.fc_starts, starts);
    ++g_stub.session;
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_starts, starts);
    press(ui, Button::Back, ButtonEvent::Short, t += 100); // edit
    press(ui, Button::Back, ButtonEvent::Short, t += 100); // pick
    press(ui, Button::Back, ButtonEvent::Short, t += 100);
    // The ceiling clamps the shown target: 9 V ceiling keeps QC2 off 12 V and 20 V.
    g_stub.max_vbus_mv = 9000;
    open_trigger(ui, t += 1000);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Plus, ButtonEvent::Long, t += 100); // capped draft
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_starts, starts + 1);
    press(ui, Button::Back, ButtonEvent::Short, t += 100);
    g_stub.max_vbus_mv = 36000;
}

static void test_fc_session_change_stops_running() {
    Ui ui; g_stub = {};
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    ui.init(0); ui.tick(6000);
    ui.goto_page(uint8_t(Page::Tools), 6000);
    ui.push_menu(Menu::Trigger, 6000);
    for (uint32_t t = 6100; t <= 6400; t += 100) ui.on_button(Button::Ok, ButtonEvent::Short, t);
    CHECK_EQ(g_stub.fc_starts, 1);
    CHECK_EQ(g_stub.fc_state, int(fc::State::Negotiating));
    ui.tick(6500);
    ++g_stub.session;
    ui.on_button(Button::Plus, ButtonEvent::Short, 6600); // poll at event delivery too
    CHECK_EQ(g_stub.fc_stops, 1);
    CHECK_EQ(g_stub.fc_retargets, 0);
    CHECK_EQ(ui.ports_busy(), true);
    ui.on_button(Button::Ok, ButtonEvent::Short, 6700);
    CHECK_EQ(g_stub.fc_starts, 1);
    for (uint32_t t = 6800; t <= 7300; t += 100) ui.tick(t);
    CHECK_EQ(ui.ports_busy(), false);
}

static void test_fc_retarget_refuses_new_load() {
    Ui ui; g_stub = {};
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    ui.init(0); ui.tick(6000);
    ui.goto_page(uint8_t(Page::Tools), 6000); ui.push_menu(Menu::Trigger, 6000);
    uint32_t t = 6000;
    auto key = [&](Button b) { ui.on_button(b, ButtonEvent::Short, t += 100); };
    key(Button::Plus); // QC3
    key(Button::Ok); key(Button::Ok); key(Button::Ok); key(Button::Ok);
    g_stub.fc_state = int(fc::State::Active);
    ui.tick(t += 100);
    key(Button::Plus); key(Button::Ok); // draft, review
    g_stub.pm.current_ma = 1000; // a load appears after the initial apply
    ui.tick(t += 100);
    key(Button::Ok); key(Button::Ok);
    CHECK_EQ(g_stub.fc_retargets, 0);
    CHECK_EQ(g_stub.fc_inline_ok, 0);
    CHECK_EQ(g_stub.fc_stops, 1);
    CHECK_EQ(ui.ports_busy(), true);
}

// FC staged requests never bypass the load/PD guard, even after a refusal.
static void test_trigger_inline_override() {
    Ui ui; ui.init(10000);
    g_stub.fc_starts = g_stub.fc_inline_ok = g_stub.fc_state = 0;
    g_stub.fc_start_fail = static_cast<int>(fc::Fail::Inline);
    uint32_t t = kT0 + 30000;
    open_trigger(ui, t);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Minus, ButtonEvent::Short, t += 100);    // QC2 9 V -> 5 V
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_starts, 1);
    CHECK_EQ(g_stub.fc_inline_ok, 0);
    CHECK_STR(label(ui, Button::Ok), "Send");
    press(ui, Button::Ok, ButtonEvent::Short, t += 600);
    CHECK_EQ(g_stub.fc_starts, 2);
    CHECK_EQ(g_stub.fc_inline_ok, 0);
    g_stub.fc_start_fail = 0;
    ui.goto_page(0, t + 1000);
}

// SCAN is a visible selection with its own review, followed by two short OKs.
static void test_trigger_scan() {
    Ui ui; ui.init(10000);
    g_stub.fc_scans = g_stub.fc_state = 0;
    uint32_t t = kT0 + 60000;
    open_trigger(ui, t);
    press(ui, Button::Minus, ButtonEvent::Short, t += 100);    // QC2 -> SCAN
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);       // review
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_scans, 0);
    press(ui, Button::Ok, ButtonEvent::Short, t += 100);
    CHECK_EQ(g_stub.fc_scans, 1);
    ui.goto_page(0, t + 1000);
}

static void test_diag_model_row() {
    StrBuf<24> s;
    model_row_text(power::Chip::Ina226, s);
    CHECK_STR(s.c_str(), "N6.55");
    StrBuf<24> t;
    model_row_text(power::Chip::None, t);
    CHECK_STR(t.c_str(), "--");
}

static void test_inline_gates_emarker_scan() {
    Ui &ui = fresh_ui();
    g_stub.em = {};
    g_stub.pm.vbus_mv = 19700;
    g_stub.pm.current_ma = 2000;              // charging a laptop through the K2
    ui.tick(30000);
    CHECK_EQ(ui.link().inline_mode(), 1);
    CHECK_EQ(g_stub.inl, 1);                  // the driver was told
    CHECK_EQ(g_stub.link_inline, 1);          // k2::link was told (one owner: pd::Link)
    ui.goto_page(kDiagPage, 30000);
    g_stub.scans = 0;
    // Inline means a source is present: the scan only listens (Sniff), so no confirm is needed and
    // the live charge is never touched (the confirm gate guards the active VCONN scan only).
    CHECK_EQ(ui.run_action(Action::EmarkerScan, 30100) ? 1 : 0, 1);
    CHECK_EQ(g_stub.scans, 1);
    CHECK_EQ(int(g_stub.last_scan_mode), int(pd::EmarkerMode::Sniff));
    CHECK_EQ(ui.run_action(Action::EmarkerScan, 40000) ? 1 : 0, 1);
    CHECK_EQ(g_stub.scans, 2);
    // Rescan inline is passive and never needs a confirm.
    CHECK_EQ(ui.run_action(Action::PdRescan, 40200) ? 1 : 0, 1);
    g_stub.pm = {};
    g_stub.inl = false;
    ui.debug_link(false, 40300);   // the latch is held through a VBUS gap now: an explicit detach ends it for the next case
}

// Which e-marker method runs depends on the link: no source = VCONN scan (also with nothing attached,
// the inline guard never refuses it); a source present = listen only, never touch CC.
static void test_emarker_mode_by_link() {
    Ui &ui = fresh_ui();
    g_stub.em = {};
    g_stub.scan_result = true;
    // Nothing attached: VBUS 0, no CC. The scan must run, in Vconn mode, unconfirmed.
    g_stub.pm = {};
    g_stub.cc = 0;
    ui.debug_link(false, 49900);
    ui.tick(50000);
    CHECK_EQ(ui.link().attached(), 0);
    CHECK_EQ(ui.link().inline_mode(), 0);
    g_stub.scans = 0;
    CHECK_EQ(ui.run_action(Action::EmarkerScan, 50100) ? 1 : 0, 1);
    CHECK_EQ(g_stub.scans, 1);
    CHECK_EQ(g_stub.scan_confirmed, 0);
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Vconn));
    // Charger attached, nothing drawing: listen only.
    ui.debug_link(true, 59900);
    g_stub.pm.vbus_mv = 5117;
    g_stub.pm.current_ma = 0;
    g_stub.cc = 1;
    ui.tick(60000);
    CHECK_EQ(ui.link().attached(), 1);
    CHECK_EQ(ui.link().inline_mode(), 0);
    CHECK_EQ(ui.run_action(Action::EmarkerScan, 60100) ? 1 : 0, 1);
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Sniff));
    // A sniff that heard nothing is not a completed scan: the page must not claim one.
    // (the sniff at 60100 stamped the link; a sniff that hears nothing must not restamp it)
    g_stub.scan_result = false;
    CHECK_EQ(ui.run_action(Action::EmarkerScan, 61100) ? 1 : 0, 1);
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Sniff));
    CHECK_EQ(ui.link().cable_age_s(61100), 1u);      // still dated by the 60100 scan
    // The source's own SOP' exchange was captured while idle: the cable data becomes fresh.
    g_stub.cable_seq++;
    ui.tick(62000);
    CHECK_EQ(ui.link().cable_age_s(62000), 0u);
    g_stub.scan_result = true;
    g_stub.pm = {};
    g_stub.cc = 0;
    ui.debug_link(false, 62100);
}

static void test_contract_text() {
    pd::Contract c;
    StrBuf<24> s;
    contract_text(c, s);
    CHECK_EQ(s.view().size(), 0u);
    c.valid = true; c.ready = true; c.pos = 4; c.mv = 20000; c.op_ma = 3000;
    contract_text(c, s);
    CHECK_STR(std::string(s.view()).c_str(), "Contract PDO4 20V 3A");
    StrBuf<24> t;
    c.pos = 5; c.mv = 9100; c.op_ma = 2500; c.ready = false;
    contract_text(c, t);
    CHECK_EQ(t.view().size() <= 24, 1);
    CHECK_STR(std::string(t.view()).c_str(), "Contract PDO5 9.1V 2.5A");
}

static void open_pd_trigger(Ui &ui, uint32_t t) {
    ui.goto_page(static_cast<uint8_t>(Page::Tools), t);
    ui.push_menu(Menu::PdTrigger, t);
}

static void test_pd_trigger_menu() {
    Ui &ui = fresh_ui();
    fill_pdos(7);
    g_stub.cc = 1;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 5000;
    g_stub.requests = g_stub.releases = 0;
    g_stub.sink = {};
    uint32_t t = 50000;
    ui.tick(t);
    open_pd_trigger(ui, t);
    CHECK_EQ(ui.link().attached(), 1);
    CHECK_EQ(g_stub.sink_cap, 36000u);                    // INA226: the monitor's limit reaches the engine
    CHECK_EQ(static_cast<int>(ui.menu()), static_cast<int>(Menu::PdTrigger));
    CHECK_EQ(g_stub.requests, 0);                         // opening the menu sends nothing
    CHECK_STR(label(ui, Button::Ok), "Next");
    ui.on_button(Button::Plus, ButtonEvent::Short, t);    // OFFER: PDO 2 (9 V fixed)
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 1);  // CURRENT (fixed: no VOLTAGE)
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 2);  // CONFIRM
    CHECK_STR(label(ui, Button::Ok), "Send");
    CHECK_EQ(g_stub.requests, 0);
    // First OK arms, nothing is sent; the second OK inside 5 s sends it confirmed.
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 10);
    CHECK_EQ(g_stub.requests, 0);
    ui.tick(t + 20);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 100);
    CHECK_EQ(g_stub.requests, 1);
    CHECK_EQ(int(g_stub.last_choice.pos), 2);
    CHECK_EQ(g_stub.last_confirmed, true);
    // LIVE: BACK returns to CONFIRM (nothing held yet, so no release); the confirm window expires.
    ui.on_button(Button::Back, ButtonEvent::Short, t + 150);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 200);
    CHECK_EQ(g_stub.requests, 1);
    ui.tick(t + 200 + Ui::kConfirmWindowMs + 100);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 200 + Ui::kConfirmWindowMs + 200);
    CHECK_EQ(g_stub.requests, 1);                         // armed again, not sent
    // PDO 1 (5 V) needs no confirm: back to OFFER, pick it, three OKs.
    ui.on_button(Button::Back, ButtonEvent::Short, t + 20000);   // CURRENT
    ui.on_button(Button::Back, ButtonEvent::Short, t + 20001);   // OFFER
    ui.on_button(Button::Minus, ButtonEvent::Short, t + 20002);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 20003);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 20004);
    ui.on_button(Button::Ok, ButtonEvent::Short, t + 20010);
    CHECK_EQ(g_stub.requests, 2);
    CHECK_EQ(int(g_stub.last_choice.pos), 1);
    CHECK_EQ(g_stub.releases, 0);
    // BACK from LIVE releases to 5 V when the contract is held; leaving the menu then does not release twice.
    g_stub.sink.held = true;
    ui.on_button(Button::Back, ButtonEvent::Short, t + 20040);
    CHECK_EQ(g_stub.releases, 1);
    CHECK_STR(label(ui, Button::Ok), "Send");             // back on CONFIRM
    g_stub.sink.held = false;
    for (int i = 0; i < 3; i++) ui.on_button(Button::Back, ButtonEvent::Short, t + 20050 + i);   // CURRENT, OFFER, exit
    CHECK_EQ(ui.menu_depth(), 0);
    CHECK_EQ(g_stub.releases, 1);
    g_stub.releases = 0;
    {   // PPS path (PDO 5): OFFER -> VOLTAGE -> CURRENT -> CONFIRM, long +/- is the coarse step
        open_pd_trigger(ui, t + 30000);
        for (int i = 0; i < 4; i++) ui.on_button(Button::Plus, ButtonEvent::Short, t + 30001 + i);
        ui.on_button(Button::Ok, ButtonEvent::Short, t + 30010);    // VOLTAGE
        CHECK_STR(label(ui, Button::Ok), "Next");
        ui.on_button(Button::Ok, ButtonEvent::Short, t + 30011);    // CURRENT
        ui.on_button(Button::Ok, ButtonEvent::Short, t + 30012);    // CONFIRM
        CHECK_STR(label(ui, Button::Ok), "Send");
        CHECK_EQ(g_stub.requests, 2);                               // nothing sent by stepping
        ui.on_button(Button::Back, ButtonEvent::Short, t + 30020);  // CURRENT
        ui.on_button(Button::Back, ButtonEvent::Short, t + 30021);  // VOLTAGE
        ui.on_button(Button::Back, ButtonEvent::Short, t + 30022);  // OFFER
        ui.on_button(Button::Back, ButtonEvent::Short, t + 30023);  // exit
        CHECK_EQ(ui.menu_depth(), 0);
        CHECK_EQ(g_stub.releases, 0);
    }
    // Paging away from the Tools page while it is open is closed first: the release is not doubled.
    open_pd_trigger(ui, t + 20050);
    g_stub.sink.held = true;
    ui.goto_page(0, t + 20060);
    CHECK_EQ(g_stub.releases, 1);
    g_stub.sink = {};
}

static void test_pd_trigger_refusals() {
    Ui &ui = fresh_ui();
    fill_pdos(7);
    g_stub.cc = 1;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 19700;
    g_stub.pm.current_ma = 2000;
    g_stub.requests = 0;
    ui.tick(60000);
    CHECK_EQ(ui.link().inline_mode(), 1);
    open_pd_trigger(ui, 61000);
    ui.on_button(Button::Ok, ButtonEvent::Short, 61100);   // inline: refused outright
    CHECK_EQ(g_stub.requests, 0);
    CHECK_STR(label(ui, Button::Ok), "");
    ui.on_button(Button::Back, ButtonEvent::Short, 61200);
    CHECK_EQ(ui.menu_depth(), 0);
    // Identify inline: refused even with a confirm - an inline K2 never transmits; the sniffed
    // identity fills the Sink tab instead. The refusal is its own result now (the remote maps it
    // to pd::Err::Inline, not "busy").
    g_stub.identifies = 0;
    CHECK_EQ(int(ui.identify(61300)), int(Ui::IdResult::Inline));
    CHECK_EQ(g_stub.identifies, 0);
    CHECK_EQ(int(ui.identify(61400)), int(Ui::IdResult::Inline));
    CHECK_EQ(g_stub.identifies, 0);
    g_stub.pm = {};
    ui.debug_link(false, 69000);                          // detach clears the inline flag
    // No source caps: nothing to list, nothing to send.
    pd::set_fake(0);
    g_stub.caps.num_pdos = 0;
    ui.debug_link(true, 69500);
    ui.tick(70000);
    open_pd_trigger(ui, 70100);
    ui.on_button(Button::Ok, ButtonEvent::Short, 70200);
    CHECK_EQ(g_stub.requests, 0);
    ui.goto_page(0, 70300);
    fill_pdos(7);
}

static void test_emarker_listen_window() {
    Ui &ui = fresh_ui();
    g_stub.em = {};
    g_stub.scan_result = false;                           // the sniff hears nothing
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 5117;                             // a source is on VBUS: Sniff mode
    g_stub.cc = 1;
    ui.debug_link(true, 79900);
    ui.tick(80000);
    ui.goto_page(kPdPage + 1, 80000);                     // Connection, Cable tab
    CHECK_EQ(on_pd(ui), 1);
    ui.on_button(Button::Ok, ButtonEvent::Short, 80050);  // pop-up
    ui.on_button(Button::Ok, ButtonEvent::Short, 80100);  // Read cable: with a source on VBUS it only listens
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Sniff));
    // The passive window runs 5 s; a new cable_seq inside it ends it (data arrived).
    ui.tick(82000);
    CHECK_EQ(ui.context().em_listen_until != 0, true);
    g_stub.cable_seq++;
    ui.tick(82200);
    CHECK_EQ(ui.context().em_listen_until, 0u);
    CHECK_EQ(ui.context().em_quiet, false);
    // Listen again; nothing arrives: the window expires into the quiet state.
    ui.on_button(Button::Ok, ButtonEvent::Short, 82900);   // pop-up again
    ui.on_button(Button::Ok, ButtonEvent::Short, 83000);   // Read cable
    ui.tick(87900);
    CHECK_EQ(ui.context().em_listen_until != 0, true);
    CHECK_EQ(ui.context().em_quiet, false);
    ui.tick(88100);
    CHECK_EQ(ui.context().em_listen_until, 0u);
    CHECK_EQ(ui.context().em_quiet, true);
    // Leaving the submenu clears the state.
    ui.on_button(Button::Back, ButtonEvent::Short, 88200);
    CHECK_EQ(ui.context().em_quiet, false);
    g_stub.scan_result = true;
    g_stub.pm = {};
    g_stub.cc = 0;
    ui.debug_link(false, 88300);
}

// Bug report: scan a cable with no source (works), move it onto a charger, swap the cable, scan again:
// the sniff finds the OLD cable's data still valid in the engine and must not call it a fresh scan.
static void test_emarker_sniff_stale_data() {
    Ui &ui = fresh_ui();
    g_stub.em = {};
    g_stub.scan_result = true;
    g_stub.pm = {};
    g_stub.cc = 0;
    ui.debug_link(false, 89900);
    ui.run_action(Action::PdRescan, 89950);                               // drops the debug hold
    ui.tick(90000);
    CHECK_EQ(int(ui.run_action(Action::EmarkerScan, 90100)), 1);          // Vconn scan, cable found
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Vconn));
    CHECK_EQ(ui.link().cable_scanned(), true);
    g_stub.pm.vbus_mv = 5117;                                             // moved onto a charger
    g_stub.cc = 1;
    g_stub.caps.rx_count++;
    ui.tick(91000);
    CHECK_EQ(ui.link().attached(), 1);
    CHECK_EQ(ui.link().cable_stale(), false);                             // same cable: its data is still good
    g_stub.cc = 0;                                                        // cable swapped
    ui.tick(91100); ui.tick(91500);
    g_stub.cc = 1;
    ui.tick(91600);
    CHECK_EQ(ui.link().cable_stale(), true);
    CHECK_EQ(int(ui.run_action(Action::EmarkerScan, 92100)), 1);
    CHECK_EQ(int(g_stub.last_scan_mode), int(k2::pd::EmarkerMode::Sniff));
    CHECK_EQ(ui.link().cable_stale(), true);                              // old data is not a new scan
    CHECK_EQ(ui.context().em_listen_until != 0, true);                    // it listens for the source's query
    g_stub.cable_seq++;                                                   // the charger asked the new cable
    ui.tick(93000);
    CHECK_EQ(ui.link().cable_stale(), false);
    g_stub.pm = {};
    g_stub.cc = 0;
    ui.debug_link(false, 93100);
}

static void test_trigger_remote_and_refusals() {
    Ui &ui = fresh_ui();
    fill_pdos(7);
    g_stub.cc = 1;
    g_stub.pm = {};
    g_stub.requests = 0;
    ui.tick(60000);
    Ui::TriggerOutcome o = ui.trigger({2, 0, 0}, 60100, false);
    CHECK_EQ(o.need_confirm, true);
    CHECK_EQ(g_stub.requests, 0);
    o = ui.trigger({2, 0, 0}, 60200, true);
    CHECK_EQ(o.need_confirm, false);
    CHECK_EQ(g_stub.requests, 1);
    o = ui.trigger({1, 0, 0}, 60300, false);              // 5 V: no confirm
    CHECK_EQ(g_stub.requests, 2);
    g_stub.pm = {};
    g_stub.inl = false;
}

static void test_identify_page() {
    Ui &ui = fresh_ui();
    g_stub.pm = {};
    g_stub.ident = {};
    g_stub.vdm = {};
    ui.debug_link(false, 69000);                          // earlier tests left the link inline: detach clears it
    ui.debug_link(true, 69500);
    ui.tick(70000);
    ui.goto_page(kPdPage + 2, 70000);                     // Connection, Sink tab
    g_stub.identifies = 0;
    ui.on_button(Button::Ok, ButtonEvent::Short, 70080);   // pop-up: Identify is its only row
    ui.on_button(Button::Ok, ButtonEvent::Short, 70100);
    CHECK_EQ(g_stub.identifies, 1);
    g_stub.ident.state = pd::IdState::Done;
    g_stub.ident.ok_mask = 1;
    g_stub.sink_vdm.identity.valid = true;
    g_stub.sink_vdm.identity.vid = 0x05AC;
    ui.tick(70300);
    CHECK_EQ(on_pd(ui), 1);
    g_stub.ident = {};
    g_stub.sink_vdm = {};
    g_stub.vdm = {};
}

static void test_cable_submenu() {
    Ui ui; ui.init(10000);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5100; g_stub.pm.current_ma = 40;
    open_act(ui, bench::Place::Cable, 3, kT0);   // Cable > Resistance
    CHECK_EQ(ui.menu() == Menu::CableR, 1);
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<uint8_t>(Menu::CableR));
    CHECK_STR(label(ui, Button::Ok), "Reset");
    CHECK_STR(label(ui, Button::Back), "Back");
    ui.tick(kT0 + 100);                               // idle reference
    g_stub.pm.vbus_mv = 4900; g_stub.pm.current_ma = -2050;
    ui.tick(kT0 + 200);
    press(ui, Button::Plus, ButtonEvent::Long, kT0 + 210);   // modal: no paging
    CHECK_EQ(ui.menu() == Menu::CableR, 1);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 220);    // reset clears the estimate
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 230);
    CHECK_EQ(ui.menu() == Menu::Connection, 1);               // back on the Cable place page
    g_stub.pm_online = false;
}

// Tools -> Black Box: an empty list says so; a hard-reset capture shows up, OK opens its timeline,
// BACK returns to the list and a second BACK leaves the submenu.
static void test_blackbox_submenu() {
    Ui &ui = fresh_ui();
    pdtrace::init();
    open_act(ui, bench::Place::Log, 7, kT0);   // Log > Black Box
    CHECK_EQ(ui.menu() == Menu::BlackBox, 1);
    CHECK_EQ(ui.page_index(), kMenuPageBase + static_cast<uint8_t>(Menu::BlackBox));
    CHECK_EQ(ui.list_state().count, 0);                         // the empty card is painted, not a list row
    pdtrace::rx(0xE0, 0x1181, std::span<const uint8_t>{});
    pdtrace::reset(pdtrace::ResetKind::HardRx);
    ui.tick(kT0 + 100);
    CHECK_EQ(pdtrace::capture_info(0).trigger == pdtrace::Trigger::HardReset, 1);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 120);       // open the timeline
    CHECK_STR(label(ui, Button::Ok), "Link");
    CHECK_EQ(ui.list_state().count >= 1, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 130);     // back to the list, still in the menu
    CHECK_EQ(ui.menu() == Menu::BlackBox, 1);
    CHECK_STR(label(ui, Button::Ok), "Open");
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 140);     // the list leaves to the Log place it was opened from
    CHECK_EQ(ui.menu() == Menu::Diagnosis, 1);
    CHECK_EQ(ui.diagnosis_page().place(), true);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 150);     // and the place to the Bench
    CHECK_EQ(ui.menu_depth(), 0u);
    pdtrace::init();
}

// Bench > Log is the PD diagnosis as a place page: the place's title, the verdict stamp under the bar (the waiting state has
// no verdict: the surface fill), the findings, then the Black Box row last, the only row OK opens.
static void test_log_place() {
    Ui &ui = fresh_ui();
    pdtrace::init();
    ui.open_place(bench::Place::Log, kT0);
    CHECK_EQ(ui.menu() == Menu::Diagnosis, 1);
    CHECK_EQ(ui.diagnosis_page().place(), true);
    CHECK_EQ(ui.context().keep.bench_sel, static_cast<int>(bench::Place::Log));
    CHECK_EQ(g_panel[(kit::kBelowBar + 2) * LCD_WIDTH + 100], kSurface);   // the stamp's ground (no verdict yet)
    CHECK_STR(label(ui, Button::Ok), "");                                   // a finding opens nothing
    for (int i = 0; i < 12; i++) press(ui, Button::Plus, ButtonEvent::Short, kT0 + 10 + i);
    CHECK_EQ(ui.diagnosis_page().selected(), act_row(bench::Place::Log, 7));
    CHECK_STR(label(ui, Button::Ok), "Open");
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 40);
    CHECK_EQ(ui.menu() == Menu::BlackBox, 1);
    // the stamp's verdict: nothing seen, none; a healthy link that had its power, pass
    pd::diag::Report r{};
    r.empty = true;
    CHECK_EQ(DiagnosisPage::stamp_of(r) == kit::Verdict::None, true);
    r.empty = false; r.link = 0; r.power = pd::diag::kPowerOk;
    CHECK_EQ(DiagnosisPage::stamp_of(r) == kit::Verdict::Pass, true);
}

// Bench > K2 is the settings page as a place: the settings, then System state (the Diag page) and Boot info (this boot's
// record as QR codes); BACK lands on the Bench with K2 focused.
static void test_k2_place() {
    Ui &ui = fresh_ui();
    ui.context().keep.settings_sel = 0;
    ui.open_place(bench::Place::K2, kT0);
    CHECK_EQ(ui.page() == Page::Settings, true);
    CHECK_EQ(ui.list_state().count, SettingsPage::kRows);
    for (int i = 0; i < SettingsPage::kSystemRow; i++) press(ui, Button::Plus, ButtonEvent::Short, kT0 + 10 + i);
    CHECK_STR(label(ui, Button::Ok), "Open");
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 30);
    CHECK_EQ(ui.menu() == Menu::Diag, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 40);
    CHECK_EQ(ui.menu_depth(), 0u);
    press(ui, Button::Plus, ButtonEvent::Short, kT0 + 50);   // Boot info
    CHECK_EQ(ui.context().keep.settings_sel, SettingsPage::kBootRow);
    press(ui, Button::Ok, ButtonEvent::Short, kT0 + 60);
    CHECK_EQ(ui.menu() == Menu::Share, 1);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 70);
    CHECK_EQ(ui.menu_depth(), 0u);
    press(ui, Button::Back, ButtonEvent::Short, kT0 + 80);   // the place leaves to the Bench, K2 focused
    CHECK_EQ(ui.page() == Page::Tools, true);
    CHECK_EQ(ui.context().keep.bench_sel, static_cast<int>(bench::Place::K2));
    ui.context().keep.settings_sel = 0;
}

// The chrome bar has no flow rail any more (micro-GUI phase 1): direction shows only as the
// readout's sign-free digits and the hero knows nothing of it; the digit filter is the test.
static void test_home_flow_arrow() {
    Ui &ui = fresh_ui();
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 20000; g_stub.pm.current_ma = 2000; g_stub.pm.power_mw = 40000;
    g_stub.pm.flow = power::Flow::Forward;
    ui.tick(68000);
    ui.tick(68250);
    // The hero readout is painted once (mode change) and holds with no repaint requests.
    auto ink = [&](uint16_t col, int x0, int y0, int w, int h) {
        int n = 0;
        for (int yy = y0; yy < y0 + h; yy++)
            for (int x = x0; x < x0 + w; x++) n += g_panel[yy * 240 + x] == col;
        return n;
    };
    auto hero_ink = [&](uint16_t col) { return ink(col, 38, 25, 160, 39); };   // Home 2c: the 64 px hero
    auto amps_ink = [&](uint16_t col) { return ink(col, 8, 156, 120, 30); };   // the 48 px IBUS ledger row
    CHECK_EQ(hero_ink(kWatt) > 60, 1);
    CHECK_EQ(amps_ink(kAmp) > 60, 1);
    CHECK_EQ(hero_ink(kVolt) + hero_ink(kAmp), 0);   // V and A live in the ledger below
    g_stub.pm.current_ma = -2000; g_stub.pm.power_mw = -40000; g_stub.pm.flow = power::Flow::Reverse;
    ui.tick(68500);
    CHECK_EQ(amps_ink(kAmp) > 60, 1);   // reversed: still cyan (magnitudes, never a sign)
    CHECK_EQ(hero_ink(kWatt) > 60, 1);
    g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = power::Flow::Idle;
    ui.tick(69000);
    g_stub.pm = {};
}

// Item 18 (kept from v3): digits change at most 4 times a second (the 250 ms display filter).
static void test_home_animation() {
    Ui &ui = fresh_ui();
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 20000; g_stub.pm.current_ma = 2000; g_stub.pm.power_mw = 40000;
    g_stub.pm.flow = power::Flow::Forward;
    ui.tick(70000);   // later than every other test: the link engine keeps its clock across init()

    // Digit filter: a value change inside 250 ms of the last one waits (the W hero, Home 2c).
    g_stub.pm.vbus_mv = 20000; g_stub.pm.current_ma = 2000; g_stub.pm.power_mw = 40000; g_stub.pm.flow = power::Flow::Forward;
    g_stub.pm.power_mw = 39600;
    ui.tick(71000);
    auto region = [&]() {
        std::vector<uint16_t> r;
        for (int y = 25; y < 25 + 39; y++)
            for (int x = 38; x < 198; x++) r.push_back(g_panel[y * 240 + x]);
        return r;
    };
    const auto before = region();
    g_stub.pm.power_mw = 39800;
    ui.tick(71100);
    CHECK_EQ(region() == before ? 1 : 0, 1);           // 100 ms after the last change: held back
    ui.tick(71260);
    CHECK_EQ(region() == before ? 1 : 0, 0);           // allowed now
    g_stub.pm = {};
}

// Item 12: unplugging after a live reading freezes Home in grey with a LAST pill; long OK clears it.
static void test_home_stale() {
    Ui &ui = fresh_ui();
    g_stub.pm = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20010; g_stub.pm.current_ma = 2990; g_stub.pm.power_mw = 59830;
    g_stub.pm.flow = power::Flow::Forward;
    ui.tick(80000);
    ui.tick(80200);
    auto count = [&](int x0, int y0, int x1, int y1, uint16_t col) {
        int n = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) n += g_panel[y * 240 + x] == col;
        return n;
    };
    CHECK_EQ(ui.context().stale() ? 1 : 0, 0);
    CHECK_EQ(count(38, 25, 198, 64, kWatt) > 50, 1);    // Home 2c: the W hero
    CHECK_EQ(count(38, 25, 198, 64, kStale), 0);
    g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = power::Flow::Idle;
    ui.tick(82000);
    ui.tick(82200);
    CHECK_EQ(ui.context().stale() ? 1 : 0, 1);
    CHECK_EQ(count(38, 25, 198, 64, kStale) > 50, 1);   // the last power, frozen in grey
    CHECK_EQ(count(38, 25, 198, 64, kWatt), 0);
    CHECK_EQ(count(8, 99, 128, 129, kStale) > 50, 1);   // the volts row too
    CHECK_EQ(count(8, 156, 128, 186, kStale) > 50, 1);  // and the amps row
    CHECK_EQ(count(150, 1, 222, 18, kStale) > 100, 1);   // the LAST pill: a kStale plate in the hero cell's corner
    ui.tick(84200);
    ui.tick(84300);
    CHECK_EQ(count(150, 1, 222, 18, kStale) > 100, 1);   // it counts the age but stays a plate
    // Long OK on Home drops the frozen state: back to the NO VBUS plate.
    ui.on_button(Button::Ok, ButtonEvent::Long, 84400);
    ui.tick(84500);
    CHECK_EQ(ui.context().stale() ? 1 : 0, 0);
    CHECK_EQ(count(0, 0, 224, 240, kStale), 0);
    CHECK_EQ(count(150, 1, 222, 18, kSurface) > 100, 1);   // the NO VBUS pill
    g_stub.pm = {};
}

// Detail is a live instrument: with Home frozen on its LAST state (unplugged after a live reading), Detail still shows the
// live metrics (no grey, no LAST pill) and follows the analog lines as they move. Only Home keeps the frozen snapshot.
static void test_detail_is_live_while_home_holds_last() {
    Ui &ui = fresh_ui();
    g_stub.pm = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20010; g_stub.pm.current_ma = 2990; g_stub.pm.power_mw = 59830;
    g_stub.pm.flow = power::Flow::Forward;
    adc::Reading a;
    a.cc1_mv = 1620; a.cc2_mv = 20; a.dp_mv = 600; a.dm_mv = 580;
    g_stub.adc = a;
    ui.tick(80000);
    ui.tick(80200);
    g_stub.pm.vbus_mv = 0; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0; g_stub.pm.flow = power::Flow::Idle;
    a.cc1_mv = 2000; a.cc2_mv = 1100; a.dp_mv = 120; a.dm_mv = 90;      // the lines as they read once unplugged
    g_stub.adc = a;
    ui.tick(82000);
    ui.tick(82200);
    CHECK_EQ(ui.context().stale() ? 1 : 0, 1);                          // Home would be on LAST here
    ui.goto_page(uint8_t(Page::Detail), 82300);
    ui.tick(82400);
    ui.tick(82600);
    auto count = [&](int x0, int y0, int x1, int y1, uint16_t col) {
        int n = 0;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) n += g_panel[y * 240 + x] == col;
        return n;
    };
    auto hash = [&](int x0, int y0, int x1, int y1) {
        uint32_t h = 2166136261u;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) h = (h ^ g_panel[y * 240 + x]) * 16777619u;
        return h;
    };
    CHECK_EQ(count(0, 0, 224, 240, kStale), 0);                         // no frozen grey, no LAST pill
    // Numbers: CC1 and CC2 are the left cells of rows 5 and 6 (y 170..213), D+ and D- the right cells of rows 3 and 4 (y 126..169).
    const uint32_t cc_before = hash(4, 170, 101, 214), dm_before = hash(106, 126, 223, 170);
    a.cc1_mv = 3300; a.cc2_mv = 0; a.dp_mv = 600; a.dm_mv = 600;        // a charger's Rp appears: Numbers follows at once
    g_stub.adc = a;
    ui.tick(83000);
    ui.tick(83200);
    CHECK_EQ(hash(4, 170, 101, 214) != cc_before ? 1 : 0, 1);           // CC1/CC2 cells moved
    CHECK_EQ(hash(106, 126, 223, 170) != dm_before ? 1 : 0, 1);         // D+/D- cells moved
    CHECK_EQ(count(0, 0, 224, 240, kStale), 0);
    // Home keeps its snapshot: back on Home the LAST pill is still there.
    ui.goto_page(uint8_t(Page::Home), 83300);
    ui.tick(83400);
    ui.tick(83600);
    CHECK_EQ(count(150, 1, 222, 18, kStale) > 100 ? 1 : 0, 1);
    g_stub.pm = {};
    g_stub.adc.reset();
}

static void test_usb_link_toggles() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    g_stub.usb_stored = {false, false};
    k2::usbcfg::current = {false, false};
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 21000);
    ui.push_menu(Menu::UsbLink, 21100);
    CHECK_EQ(ui.menu() == Menu::UsbLink, 1);
    CHECK_STR(label(ui, Button::Ok), "Toggle");
    ui.on_button(Button::Ok, ButtonEvent::Short, 21200);          // HID on
    CHECK_EQ(g_stub.usb_stored.hid, 1); CHECK_EQ(g_stub.usb_stored.drive, 0);
    ui.on_button(Button::Plus, ButtonEvent::Short, 21300);        // select Drive
    ui.on_button(Button::Ok, ButtonEvent::Short, 21400);
    CHECK_EQ(g_stub.usb_stored.hid, 1); CHECK_EQ(g_stub.usb_stored.drive, 1);
    ui.on_button(Button::Ok, ButtonEvent::Short, 21500);          // Drive off again
    CHECK_EQ(g_stub.usb_stored.drive, 0);
    CHECK_EQ(g_stub.usb_sets, 3);
    // The longest hint must clear the "OFF" pill (right edge 214).
    CHECK_EQ(44 + static_cast<int>(strlen("web 192.168.7.1")) * kLabelFont.adv <= 214 - (3 * 7 + 8) - 4, 1);
    ui.on_button(Button::Ok, ButtonEvent::Short, 21520);          // Drive on again, then Network
    ui.on_button(Button::Plus, ButtonEvent::Short, 21530);        // select Network
    ui.on_button(Button::Ok, ButtonEvent::Short, 21540);          // Net on: HID stays, Drive goes off
    CHECK_EQ(g_stub.usb_stored.net, 1); CHECK_EQ(g_stub.usb_stored.hid, 1); CHECK_EQ(g_stub.usb_stored.drive, 0);
    ui.on_button(Button::Minus, ButtonEvent::Short, 21550);       // back to Drive: turning it on drops Net
    ui.on_button(Button::Ok, ButtonEvent::Short, 21560);
    CHECK_EQ(g_stub.usb_stored.net, 0); CHECK_EQ(g_stub.usb_stored.drive, 1);
    ui.on_button(Button::Plus, ButtonEvent::Short, 21570);        // Network again, then wrap to HID
    ui.on_button(Button::Plus, ButtonEvent::Short, 21580);
    ui.on_button(Button::Ok, ButtonEvent::Short, 21590);          // HID off: nothing else stays on but Drive
    CHECK_EQ(g_stub.usb_stored.hid, 0); CHECK_EQ(g_stub.usb_stored.net, 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, 21595);          // HID on again, Drive off for the next checks
    ui.on_button(Button::Plus, ButtonEvent::Short, 21596);
    ui.on_button(Button::Ok, ButtonEvent::Short, 21597);
    CHECK_EQ(g_stub.usb_stored.drive, 0);
    g_stub.usb_set_ok = false;                                    // flash refuses: nothing changes
    ui.on_button(Button::Ok, ButtonEvent::Short, 21600);
    CHECK_EQ(g_stub.usb_stored.drive, 0);
    ui.pop_menu(21700);
    k2::usbcfg::current = {true, true};
    g_stub = StubState();
}

// The USB Link footer band is the ok-green fill only while USB is really active; "USB is off" is informational
// and keeps the neutral bar fill (the words say it too).
static void test_usb_link_footer_icon_tone() {
    Ui &ui = fresh_ui();
    auto fill_at = [](uint16_t col) { return g_panel[230 * 240 + 6] == col ? 100 : 0; };   // the band fill, right of the tone bar
    auto icon_count = fill_at;
    g_stub = StubState();
    g_stub.usb_stored = {false, false};
    k2::usbcfg::current = {false, false};
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 22000);
    ui.push_menu(Menu::UsbLink, 22100);
    ui.tick(22200); ui.tick(22300);
    CHECK_EQ(icon_count(kOkT), 0);                      // off: no green
    CHECK_EQ(icon_count(kBg) > 10, 1);                  // the neutral (ground) band is there
    ui.pop_menu(22400);
    g_stub = StubState();
    g_stub.usb_stored = {true, false};
    k2::usbcfg::current = {true, false};
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 22500);
    ui.push_menu(Menu::UsbLink, 22600);
    ui.tick(22700); ui.tick(22800);
    CHECK_EQ(icon_count(kOkT) > 10, 1);                 // active this boot: the green band
    ui.pop_menu(22900);
    k2::usbcfg::current = {true, true};
    g_stub = StubState();
}

// `pd request ... --over-load` above 5 V needs --confirm too. The refused-attach branch used to send an
// unconfirmed over-load straight to the far-sink gate (RED before the fix: requests == 1, no need_confirm).
static void test_over_load_above_5v_needs_confirm() {
    Ui &ui = fresh_ui();
    g_stub = StubState();
    fill_pdos(4);                                        // PDO 2 = 9 V, PDO 1 = 5 V
    g_stub.trig_refusal = pd::TriggerRefusal::Inline;    // a sink sits behind the K2
    g_stub.ol_facts = {true, false};                     // ... and it is a passive load: the far-sink gate would allow it
    uint32_t t = 30000;
    ui.tick(t += 100); ui.tick(t += 100);                // the live cap (36 V monitor ceiling) is read in the tick
    // 9 V, over-load without confirm: need-confirm, nothing sent.
    Ui::TriggerOutcome o = ui.trigger({2, 0, 0}, t += 100, false, true);
    CHECK_EQ(o.need_confirm, true);
    CHECK_EQ(g_stub.requests, 0);
    // Both flags: handed on to the far-sink gate (over_load reaches the driver).
    o = ui.trigger({2, 0, 0}, t += 100, true, true);
    CHECK_EQ(o.need_confirm, false);
    CHECK_EQ(g_stub.requests, 1);
    CHECK_EQ(g_stub.last_over_load, true);
    CHECK_EQ(g_stub.last_confirmed, true);
    // 5 V over-load: no confirm rule (nothing above 5 V is asked).
    o = ui.trigger({1, 0, 0}, t += 100, false, true);
    CHECK_EQ(o.need_confirm, false);
    CHECK_EQ(g_stub.requests, 2);
    // No over-load and a refusal: unchanged, no prompt, the driver refuses (the UI never prompts in a refused state).
    o = ui.trigger({2, 0, 0}, t += 100, false, false);
    CHECK_EQ(o.need_confirm, false);
    CHECK_EQ(g_stub.requests, 3);
    CHECK_EQ(g_stub.last_over_load, false);
    g_stub = StubState();
}

int main() {
    test_over_load_above_5v_needs_confirm();
    test_usb_link_footer_icon_tone();
    test_usb_link_toggles();
    test_home_flow_arrow();
    test_home_animation();
    test_home_stale();
    test_detail_is_live_while_home_holds_last();
    test_fc_pill_text();
    test_home_gauges_and_footer();
    test_cable_tab_save_and_list();
    test_spoof_tool();
    test_spoof_external_identity();
    test_spoof_text();
    test_em_row_text();
    test_inline_gates_emarker_scan();
    test_emarker_mode_by_link();
    test_contract_text();
    test_diag_is_fixed();
    test_cable_tab_not_found_greys_save();
    test_lines_rows();
    test_temp_short();
    test_lines_page();
    test_cable_rows();
    test_cable_kelvin_rows();
    test_cable_mode_toggle();
    test_every_label_fits_its_slot();
    test_ai_rows_on_lines();
    test_tools_hub();
    test_submenu_is_modal();
    test_trigger_submenu();
    test_trigger_inline_override();
    test_trigger_scan();
    test_fc_session_change_stops_running();
    test_fc_retarget_refuses_new_load();
    test_diag_model_row();
    test_fake_pdos_on_the_page();
    test_label_sets();
    test_global_paging();
    test_short_presses_do_not_page();
    test_back_goes_home();
    test_press_flash();
    test_reboot_hold_card();
    test_pd_list_with_seven_pdos();
    test_page_actions();
    test_pd_trigger_menu();
    test_pd_trigger_refusals();
    test_emarker_listen_window();
    test_emarker_sniff_stale_data();
    test_trigger_remote_and_refusals();
    test_identify_page();
    test_cable_submenu();
    test_blackbox_submenu();
    test_log_place();
    test_k2_place();
    test_debug_fake_zero_clears();
    test_fake_zero_leaves_no_gone_rows();
    test_pd_list_clears_on_detach();   // last: it leaves the forced link detached
    return test_report("test_ui_pages");
}
