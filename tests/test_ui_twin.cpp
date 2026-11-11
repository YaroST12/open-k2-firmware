// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools > Device twins: the list page through the real Ui over the stub library and the simulated live proof (ui_stubs.cpp:
// the REAL twin::live::Runner behind a charger that answers with g_stub.caps).
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "fake_lcd.h"
#include "k2/pd/pd.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;

namespace {

uint32_t g_t = 100000;

// The CUKTECH C1 list, plain cable: fixed 5/9/12/15/20 V 3 A, PPS 3.3-11 V 3 A at position 6, 3.3-11 V 3 A at 7 (as in test_twin_live).
void charger_c1() {
    g_stub.caps = {};
    g_stub.caps.valid = true;
    g_stub.caps.spec_rev = 2;
    for (uint32_t w : {0x0A01912Cu, 0x0002D12Cu, 0x0003C12Cu, 0x0004B12Cu, 0x0006412Cu, 0xC8DC323Cu, 0xC990323Cu})
        g_stub.caps.pdos[g_stub.caps.num_pdos++] = pd::decode_pdo(w);
}

void reset() {
    g_stub = StubState();
    stub_twin_reset();
    charger_c1();
}

void add_three() {
    stub_twin_add("Pixel 9", {0x3601912C, 0x0002D12C, 0xC0DC323C});
    stub_twin_add("Laptop", {0x3601912C});
    stub_twin_add("OP15", {0x2601912C, 0x0002D12C, 0x0003C12C, 0xC0DC3264});
}

Ui &open_page() {
    static Ui ui;
    g_t += 1000;
    ui.init(g_t);
    ui.tick(g_t);
    ui.goto_page(uint8_t(Page::Tools), g_t + 50);
    ui.push_menu(Menu::Twin, g_t + 100);
    g_t += 150;
    ui.tick(g_t);
    return ui;
}

void step(Ui &ui, uint32_t ms) {
    g_t += ms;
    ui.tick(g_t);
}

void press(Ui &ui, Button b) {
    g_t += 30;
    ui.on_button(b, ButtonEvent::Short, g_t);
}

const char *label(Ui &ui, Button b) {
    static char buf[24];
    label_text(buf, ui.labels().b[int(b)], label_max_chars(b));
    return buf;
}

std::string text_of(Ui &ui, int index) {
    kit::RowSpec s;
    ui.twin_page().peek(index, false, s);
    return std::string(s.text);
}
std::string tail_of(Ui &ui, int index) {
    kit::RowSpec s;
    ui.twin_page().peek(index, false, s);
    return std::string(s.tail);
}
std::string note_of(Ui &ui, int index) {
    kit::RowSpec s;
    ui.twin_page().peek(index, true, s);
    return std::string(s.note);
}
kit::RowSpec spec_of(Ui &ui, int index) {
    kit::RowSpec s;
    ui.twin_page().peek(index, true, s);
    return s;
}
std::string footer_of(Ui &ui) { return std::string(ui.twin_page().footer()); }

// Runs the simulated charger until the run is over (the real runner: dark, attach, request, hold, release).
void run_out(Ui &ui) {
    for (int i = 0; i < 600 && twin_hw::live_running(); i++) step(ui, 25);
    step(ui, 100);                               // the page repaints on its own cadence: one more tick shows the end state
}

void test_rows_and_footer() {
    reset();
    add_three();
    Ui &ui = open_page();
    CHECK_EQ(ui.menu() == Menu::Twin, 1);
    CHECK_EQ(ui.twin_page().list_state().count, 4);   // three twins and "Learn from last Identify"
    CHECK_STR(text_of(ui, 0).c_str(), "Pixel 9");
    CHECK_STR(tail_of(ui, 0).c_str(), "33W PPS");
    CHECK_STR(text_of(ui, 1).c_str(), "Laptop");
    CHECK_STR(tail_of(ui, 1).c_str(), "5V only");
    CHECK_STR(text_of(ui, 2).c_str(), "OP15");
    CHECK_STR(tail_of(ui, 2).c_str(), "36W of 55W");
    CHECK_STR(note_of(ui, 0).c_str(), "the device takes no more");   // the charger offers more than the Pixel wants
    CHECK_STR(note_of(ui, 1).c_str(), "the device takes no more");
    CHECK_STR(note_of(ui, 2).c_str(), "limited by the cable");
    CHECK_STR(text_of(ui, 3).c_str(), "Learn from last Identify");
    CHECK_STR(footer_of(ui).c_str(), "1 of 3 full on charger");
    CHECK_STR(label(ui, Button::Ok), "Run");
    CHECK_STR(label(ui, Button::Back), "Back");
    press(ui, Button::Plus);
    press(ui, Button::Plus);
    press(ui, Button::Plus);
    CHECK_EQ(ui.twin_page().cursor(), 3);
    CHECK_STR(label(ui, Button::Ok), "Learn");
    press(ui, Button::Plus);                     // clamped on the last row
    CHECK_EQ(ui.twin_page().cursor(), 3);
    press(ui, Button::Minus);
    CHECK_EQ(ui.twin_page().cursor(), 2);
}

void test_no_charger_and_empty() {
    reset();
    add_three();
    g_stub.caps.valid = false;
    Ui &ui = open_page();
    CHECK_STR(footer_of(ui).c_str(), "No charger: no predictions");
    CHECK_STR(tail_of(ui, 0).c_str(), "--");     // an absent value is a dim placeholder
    CHECK_STR(note_of(ui, 0).c_str(), "no charger to predict from");

    reset();                                     // no twin at all: only the Learn row
    Ui &ui2 = open_page();
    CHECK_EQ(ui2.twin_page().list_state().count, 1);
    CHECK_STR(footer_of(ui2).c_str(), "No twin yet. Learn one");
    CHECK_STR(label(ui2, Button::Ok), "Learn");
}

void test_repredict_on_new_charger() {
    reset();
    add_three();
    Ui &ui = open_page();
    CHECK_STR(tail_of(ui, 0).c_str(), "33W PPS");
    g_stub.caps = {};                            // another charger: 5 V and 9 V, 3 A
    g_stub.caps.valid = true;
    g_stub.caps.spec_rev = 2;
    for (uint32_t w : {0x0A01912Cu, 0x0002D12Cu}) g_stub.caps.pdos[g_stub.caps.num_pdos++] = pd::decode_pdo(w);
    step(ui, 100);
    CHECK_STR(tail_of(ui, 0).c_str(), "27W of 33W");
    CHECK_STR(note_of(ui, 0).c_str(), "limited by this charger");
}

void test_arm_confirm_run() {
    reset();
    add_three();
    Ui &ui = open_page();
    press(ui, Button::Ok);                       // first press arms, starts nothing
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    CHECK_STR(footer_of(ui).c_str(), "Asks the charger. Confirm");
    press(ui, Button::Ok);                       // second press starts, confirmed
    CHECK_EQ(g_stub.twin_starts, 1);
    CHECK_EQ(g_stub.twin_start_confirmed ? 1 : 0, 1);
    CHECK_EQ(int(g_stub.twin_start_seqs.size()), 1);
    CHECK_EQ(int(g_stub.twin_start_seqs[0]), 1);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);
    CHECK_STR(label(ui, Button::Back), "Stop");
    CHECK_STR(label(ui, Button::Ok), "");
    CHECK_STR(footer_of(ui).c_str(), "Live proof 1 of 1");
    CHECK_EQ(int(spec_of(ui, 0).marker), int(kit::Marker::Verdict));
    CHECK_EQ(int(spec_of(ui, 0).state), int(kit::Verdict::Running));
    run_out(ui);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    kit::RowSpec s = spec_of(ui, 0);
    CHECK_EQ(int(s.marker), int(kit::Marker::Verdict));
    CHECK_EQ(int(s.state), int(kit::Verdict::Pass));
    CHECK_EQ(note_of(ui, 0).rfind("33W PPS ok", 0), 0u);   // the live proof text replaces the limiter note
    CHECK_STR(footer_of(ui).c_str(), "1 of 1 granted");
    CHECK_STR(label(ui, Button::Back), "Back");
    CHECK_STR(label(ui, Button::Ok), "Run");
}

void test_reject_is_a_fail() {
    reset();
    add_three();
    g_stub.twin_reject = true;                   // the charger refuses the Request the twin chose
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run_out(ui);
    CHECK_EQ(int(spec_of(ui, 0).state), int(kit::Verdict::Fail));
    CHECK_STR(note_of(ui, 0).c_str(), "refused");
    CHECK_STR(footer_of(ui).c_str(), "0 of 1 granted");
}

void test_arm_expires_and_moves() {
    reset();
    add_three();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
    press(ui, Button::Plus);                     // a cursor move disarms
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);
    press(ui, Button::Minus);
    press(ui, Button::Ok);                       // armed again
    step(ui, Ui::kConfirmWindowMs + 100);        // the window runs out
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);
    press(ui, Button::Ok);                       // so this press only arms
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
}

void test_refusals() {
    struct Case { twin::live::Refusal r; const char *footer; } cases[] = {
        {twin::live::Refusal::Busy, "Tool or test running"},
        {twin::live::Refusal::Inline, "Device inline: unplug it"},
        {twin::live::Refusal::NoMonitor, "Power monitor not ready"},
        {twin::live::Refusal::NoSource, "No charger to run against"},
    };
    for (const Case &c : cases) {
        reset();
        add_three();
        g_stub.twin_refusal = c.r;
        Ui &ui = open_page();
        press(ui, Button::Ok);
        CHECK_STR(footer_of(ui).c_str(), c.footer);
        CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);   // nothing to confirm
        CHECK_EQ(g_stub.twin_starts, 0);
    }
}

void test_back_stops_the_run() {
    reset();
    add_three();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    press(ui, Button::Back);                     // BACK ends the run and stays on the page
    CHECK_EQ(g_stub.twin_cancels, 1);
    CHECK_EQ(ui.menu() == Menu::Twin, 1);
    run_out(ui);                                 // the runner puts 5 V back, then it is over
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    press(ui, Button::Back);                     // idle: BACK leaves the page
    CHECK_EQ(int(ui.menu_depth()), 0);
}

void test_leaving_cancels_own_run() {
    reset();
    add_three();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    ui.goto_page(uint8_t(Page::Home), g_t + 10);  // paged away while it runs
    CHECK_EQ(g_stub.twin_cancels, 1);
}

void test_learn_row() {
    reset();
    Ui &ui = open_page();
    g_stub.twin_learned.how = twin_hw::Learn::NotReady;
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.twin_learns, 1);
    CHECK_STR(footer_of(ui).c_str(), "Run Identify first");

    twin::Twin t;
    const char name[] = "Phone";
    for (std::size_t i = 0; i + 1 < sizeof name; i++) t.name[i] = name[i];
    t.pdos[t.num_pdos++] = 0x3601912C;
    t.pdos[t.num_pdos++] = 0x0002D12C;
    g_stub.twin_learned.how = twin_hw::Learn::Ok;
    g_stub.twin_learned.twin = t;
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.twin_learns, 2);
    CHECK_EQ(int(g_stub.twins.size()), 1);
    CHECK_STR(footer_of(ui).c_str(), "Twin learned");
    CHECK_EQ(ui.twin_page().list_state().count, 2);
    CHECK_EQ(ui.twin_page().cursor(), 0);        // the cursor goes to the new twin
    CHECK_STR(text_of(ui, 0).c_str(), "Phone");

    g_stub.twin_learned.how = twin_hw::Learn::Store;
    g_stub.twin_learned.store.err = twin::Err::Full;
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    CHECK_STR(footer_of(ui).c_str(), "Library full");
}

void test_footer_note_times_out() {
    reset();
    Ui &ui = open_page();
    g_stub.twin_learned.how = twin_hw::Learn::NotReady;
    press(ui, Button::Ok);
    CHECK_STR(footer_of(ui).c_str(), "Run Identify first");
    step(ui, 3000);
    CHECK_STR(footer_of(ui).c_str(), "No twin yet. Learn one");
}

}  // namespace

int main() {
    test_rows_and_footer();
    test_no_charger_and_empty();
    test_repredict_on_new_charger();
    test_arm_confirm_run();
    test_reject_is_a_fail();
    test_arm_expires_and_moves();
    test_refusals();
    test_back_stops_the_run();
    test_leaving_cancels_own_run();
    test_learn_row();
    test_footer_note_times_out();
    return test_report("test_ui_twin");
}
