// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The page arena in the real Ui (ui.hpp): only the pages on screen exist, a page is built when it is entered and gone when it
// is left, and what must outlive a page (the remembered cursors, the trigger draft, the share record, the charger test) lives
// outside it. The arena class itself is tested in test_page_arena.cpp; the charger test's own cases are in
// test_ui_charger_test.cpp.
#include <memory>
#include <string_view>

#include "fake_lcd.h"
#include "k2/ui/ui.hpp"
#include "test_util.h"
#include "ui_nav.h"
#include "ui_stubs.h"

using namespace k2;
namespace qs = k2::qrshare;

#define CHECK(c) CHECK_EQ(int(!!(c)), 1)

// A new Ui every time, parked on Home.
static Ui &fresh() {
    static std::unique_ptr<Ui> ui;
    ui = std::make_unique<Ui>();
    g_stub = StubState();
    ui->init(1000);
    ui->set_rotation(0, 1000);
    ui->tick(1100);
    return *ui;
}

static void press(Ui &ui, Button b, uint32_t now, ButtonEvent ev = ButtonEvent::Short) { ui.on_button(b, ev, now); }

static void test_only_the_visible_pages_exist() {
    Ui &ui = fresh();
    CHECK_EQ(int(ui.live_pages()), 1);                 // Home, nothing else
    CHECK_EQ(int(ui.page_bytes()), int(sizeof(HomePage)));
    ui.goto_page(static_cast<uint8_t>(Page::Detail), 1200);
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK_EQ(int(ui.page_bytes()), int(sizeof(DetailPage)));   // Home is gone, Detail took its place
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1300);
    CHECK_EQ(int(ui.page_bytes()), int(sizeof(ToolsPage)));
    CHECK(ui.push_menu(Menu::BlackBox, 1400));
    CHECK_EQ(int(ui.live_pages()), 2);                 // the base stays under its menu
    const std::size_t two = ui.page_bytes();
    CHECK(two > sizeof(ToolsPage) + sizeof(BlackBoxPage) - 1);
    CHECK(ui.pop_menu(1500));
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK_EQ(int(ui.page_bytes()), int(sizeof(ToolsPage)));   // the menu is destroyed, the arena is given back
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1600);
    CHECK_EQ(int(ui.page_bytes()), int(sizeof(HomePage)));
}

// The deepest chain of the UI fits: Tools > Connection > Saved cables > Share.
static void test_the_deepest_chain_fits() {
    Ui &ui = fresh();
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1200);
    CHECK(ui.push_menu(Menu::Connection, 1300));
    CHECK(ui.push_menu(Menu::Emarker, 1400));
    CHECK(ui.push_menu(Menu::Share, 1500));
    CHECK_EQ(int(ui.live_pages()), 4);
    CHECK(!ui.push_menu(Menu::Cards, 1600));           // the stack is full
    CHECK_EQ(int(ui.live_pages()), 4);
    // A page change drops the whole chain.
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1700);
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK_EQ(int(ui.menu_depth()), 0);
    // The deepest base with the largest menu on it.
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1800);
    CHECK(ui.push_menu(Menu::BlackBox, 1900));
    CHECK_EQ(int(ui.live_pages()), 2);
}

static void fill_record(Ui &ui, std::size_t n) {
    for (std::size_t i = 0; i < n; i++) ui.share_buffer()[i] = static_cast<std::uint8_t>(i * 7 + 3);
}

// Leave and enter again builds the page afresh (its counters restart), and the record it shows is not in the page.
static void test_a_page_is_built_fresh_and_the_share_record_survives() {
    Ui &ui = fresh();
    fill_record(ui, 184);
    CHECK(ui.open_share(qs::Type::Card, 1, "Kept", 184, 2000));
    CHECK(ui.menu() == Menu::Share);
    ui.tick(2100);
    const unsigned first_paints = ui.share().paints();
    CHECK(first_paints > 0);
    press(ui, Button::Back, 2200);
    CHECK(ui.menu() == Menu::Count);
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK(ui.push_menu(Menu::Share, 2300));            // no opener, no new record: the page shows the one kept
    CHECK(ui.share().title() == "Kept");
    CHECK_EQ(int(ui.share().parts()), 1);
    ui.tick(2400);
    CHECK_EQ(int(ui.share().paints()), int(first_paints));   // a new page: counted from zero, not on top of the old
    CHECK_EQ(int(ui.share().index()), 0);
}

// Home's trend window, the Bench and Settings cursors and the FC trigger draft are not in the pages.
static void test_remembered_state_survives_leaving_the_page() {
    Ui &ui = fresh();
    CHECK(ui.context().keep.home_window == trend::Window::S10);
    press(ui, Button::Plus, 1200, ButtonEvent::Long);  // Flow: a long Dn steps the trend span
    const trend::Window w = ui.context().keep.home_window;
    CHECK(w != trend::Window::S10);
    ui.goto_page(static_cast<uint8_t>(Page::Detail), 1300);
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1400);
    CHECK(ui.context().keep.home_window == w);

    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1500);
    CHECK_EQ(ui.context().keep.bench_sel, 1);          // the Bench opens on Charger
    press(ui, Button::Plus, 1600);
    press(ui, Button::Plus, 1610);
    CHECK_EQ(ui.context().keep.bench_sel, 3);
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1700);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1800);
    CHECK_EQ(ui.context().keep.bench_sel, 3);
    press(ui, Button::Minus, 1810);                    // the rebuilt page steps on from where the cursor was
    CHECK_EQ(ui.context().keep.bench_sel, 2);
    press(ui, Button::Ok, 1820);                       // a place remembers its own action
    press(ui, Button::Plus, 1830);
    CHECK_EQ(int(ui.context().keep.place_sel[static_cast<int>(bench::Place::Cable)]), 1);
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1840);
    press(ui, Button::Ok, 1850);                       // OK on Flow: the Bench's list, the focus still on the last place
    CHECK(ui.page() == Page::Tools);
    CHECK_EQ(int(ui.context().keep.place), static_cast<int>(bench::Place::Meter));
    CHECK_EQ(ui.context().keep.bench_sel, 2);

    ui.goto_page(static_cast<uint8_t>(Page::Settings), 1900);
    press(ui, Button::Plus, 2000);
    CHECK_EQ(ui.context().keep.settings_sel, 1);
    ui.goto_page(static_cast<uint8_t>(Page::Home), 2100);
    ui.goto_page(static_cast<uint8_t>(Page::Settings), 2200);
    CHECK_EQ(ui.context().keep.settings_sel, 1);

    CHECK(ui.push_menu(Menu::Trigger, 2300));
    const int sel = ui.context().keep.trigger.sel();
    press(ui, Button::Plus, 2400);                     // Pick: the protocol cursor moves
    CHECK(ui.context().keep.trigger.sel() != sel);
    const int moved = ui.context().keep.trigger.sel();
    CHECK(ui.pop_menu(2500));
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK(ui.push_menu(Menu::Trigger, 2600));
    CHECK_EQ(ui.context().keep.trigger.sel(), moved);
}

// Connection is built on its default tab each time, whatever tab the last visit left.
static void test_a_menu_reenters_on_its_default() {
    Ui &ui = fresh();
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1200);
    CHECK(ui.push_menu(Menu::Connection, 1300));
    const conn::Tab dflt = ui.connection_page().tab();
    CHECK(ui.pop_menu(1350));
    const conn::Tab other = dflt == conn::Tab::Cable ? conn::Tab::Source : conn::Tab::Cable;
    CHECK(ui.open_connection(other, false, 1400));
    CHECK(ui.connection_page().tab() == other);
    CHECK(ui.pop_menu(1500));
    CHECK(ui.push_menu(Menu::Connection, 1600));
    CHECK(ui.connection_page().tab() == dflt);
}

int main() {
    test_only_the_visible_pages_exist();
    test_the_deepest_chain_fits();
    test_a_page_is_built_fresh_and_the_share_record_survives();
    test_remembered_state_survives_leaving_the_page();
    test_a_menu_reenters_on_its_default();
    return test_report("test_ui_arena");
}
