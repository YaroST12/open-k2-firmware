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

kit::RowSpec plain_of(Ui &ui, int index) {
    kit::RowSpec s;
    ui.twin_page().peek(index, false, s);
    return s;
}

// OK on list row `index`: the twin's detail opens with Prove live focused.
void open_twin(Ui &ui, int index) {
    for (int i = 0; i < index; i++) press(ui, Button::Plus);
    press(ui, Button::Ok);
    step(ui, 50);
}

void test_rows_and_footer() {
    reset();
    add_three();
    Ui &ui = open_page();
    CHECK_EQ(ui.menu() == Menu::Twin, 1);
    CHECK_EQ(ui.twin_page().list_state().count, 4);   // three twins and "Learn from device"
    CHECK_STR(text_of(ui, 0).c_str(), "Pixel 9");
    CHECK_STR(tail_of(ui, 0).c_str(), "");           // no tail: the name runs to the scroll lane
    kit::RowSpec s = plain_of(ui, 0);                 // full power: a solid square in plain ink
    CHECK_EQ(int(s.marker), int(kit::Marker::Level));
    CHECK_EQ(int(s.state), 2);
    CHECK_EQ(int(s.marker_color), int(kText));
    s = plain_of(ui, 1);                              // the laptop sits on 5 V: hollow, amber
    CHECK_EQ(int(s.marker), int(kit::Marker::Level));
    CHECK_EQ(int(s.state), 0);
    CHECK_EQ(int(s.marker_color), int(kCaution));
    s = plain_of(ui, 2);                              // OP15, held back by the cable: half, amber
    CHECK_EQ(int(s.state), 1);
    CHECK_EQ(int(s.marker_color), int(kCaution));
    CHECK_STR(note_of(ui, 0).c_str(), "gets 33W, device max");   // the charger offers more than the Pixel wants
    CHECK_STR(note_of(ui, 1).c_str(), "5V only, device");
    CHECK_STR(note_of(ui, 2).c_str(), "gets 36 of 55W, cable");
    CHECK_STR(text_of(ui, 3).c_str(), "Learn from device");
    CHECK_STR(footer_of(ui).c_str(), "PD 20V3A: 1 of 3 full");
    CHECK_STR(label(ui, Button::Ok), "Open");
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
    CHECK_EQ(int(plain_of(ui, 0).marker), int(kit::Marker::None));   // nothing to predict: no marker
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
    CHECK_STR(note_of(ui, 0).c_str(), "gets 33W, device max");
    g_stub.caps = {};                            // another charger: 5 V and 9 V, 3 A
    g_stub.caps.valid = true;
    g_stub.caps.spec_rev = 2;
    for (uint32_t w : {0x0A01912Cu, 0x0002D12Cu}) g_stub.caps.pdos[g_stub.caps.num_pdos++] = pd::decode_pdo(w);
    step(ui, 100);
    CHECK_STR(note_of(ui, 0).c_str(), "gets 27 of 33W, charger");
    CHECK_STR(footer_of(ui).c_str(), "PD 9V3A: 0 of 3 full");
}

void test_detail_opens_and_closes() {
    reset();
    add_three();
    Ui &ui = open_page();
    open_twin(ui, 2);                            // OP15
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 1);
    CHECK_EQ(ui.twin_page().action(), 0);
    CHECK_STR(text_of(ui, 0).c_str(), "Prove live");
    CHECK_STR(text_of(ui, 1).c_str(), "Re-learn from device");
    CHECK_STR(text_of(ui, 2).c_str(), "Delete twin");
    CHECK_STR(note_of(ui, 0).c_str(), "asks the charger for 12V3A");
    CHECK_STR(label(ui, Button::Ok), "Run");
    CHECK_STR(footer_of(ui).c_str(), "Basis: its sink caps");
    press(ui, Button::Plus);
    CHECK_STR(label(ui, Button::Ok), "Learn");
    press(ui, Button::Plus);
    CHECK_STR(label(ui, Button::Ok), "Delete");
    CHECK_EQ(ui.twin_page().action(), 2);
    CHECK_EQ(ui.twin_page().first_line() > 0 ? 1 : 0, 1);   // the page scrolled so Delete is on screen
    press(ui, Button::Plus);                     // clamped
    CHECK_EQ(ui.twin_page().action(), 2);
    press(ui, Button::Back);                     // back to the list, on the same twin
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 0);
    CHECK_EQ(ui.menu() == Menu::Twin, 1);
    CHECK_EQ(ui.twin_page().cursor(), 2);
}

void test_arm_confirm_run() {
    reset();
    add_three();
    Ui &ui = open_page();
    open_twin(ui, 0);
    press(ui, Button::Ok);                       // first press arms, starts nothing
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    CHECK_STR(footer_of(ui).c_str(), "Asks for PPS 11V");
    press(ui, Button::Ok);                       // second press starts, confirmed
    CHECK_EQ(g_stub.twin_starts, 1);
    CHECK_EQ(g_stub.twin_start_confirmed ? 1 : 0, 1);
    CHECK_EQ(int(g_stub.twin_start_seqs.size()), 1);
    CHECK_EQ(int(g_stub.twin_start_seqs[0]), 1);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);
    CHECK_STR(label(ui, Button::Back), "Stop");
    CHECK_STR(label(ui, Button::Ok), "");
    CHECK_STR(footer_of(ui).c_str(), "Live proof running");
    run_out(ui);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    kit::RowSpec s = spec_of(ui, 0);
    CHECK_EQ(int(s.marker), int(kit::Marker::Verdict));
    CHECK_EQ(int(s.state), int(kit::Verdict::Pass));
    CHECK_STR(note_of(ui, 0).c_str(), "proved 33W");
    CHECK_STR(label(ui, Button::Back), "Back");
    CHECK_STR(label(ui, Button::Ok), "Run");
    press(ui, Button::Back);                     // the list wears the verdict too
    CHECK_EQ(int(spec_of(ui, 0).state), int(kit::Verdict::Pass));
    CHECK_STR(note_of(ui, 0).c_str(), "proved 33W");
    CHECK_STR(footer_of(ui).c_str(), "1 of 1 proved");
}

void test_reject_is_a_fail() {
    reset();
    add_three();
    g_stub.twin_reject = true;                   // the charger refuses the Request the twin chose
    Ui &ui = open_page();
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run_out(ui);
    CHECK_EQ(int(spec_of(ui, 0).state), int(kit::Verdict::Fail));
    CHECK_STR(note_of(ui, 0).c_str(), "charger refused 11V");
    press(ui, Button::Back);
    CHECK_STR(footer_of(ui).c_str(), "0 of 1 proved");
}

void test_arm_expires_and_moves() {
    reset();
    add_three();
    Ui &ui = open_page();
    open_twin(ui, 0);
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
    press(ui, Button::Back);                     // BACK disarms and stays on the detail
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 0);
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 1);
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
        open_twin(ui, 0);
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
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    press(ui, Button::Back);                     // BACK ends the run and stays on the page
    CHECK_EQ(g_stub.twin_cancels, 1);
    CHECK_EQ(ui.menu() == Menu::Twin, 1);
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 1);
    run_out(ui);                                 // the runner puts 5 V back, then it is over
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    press(ui, Button::Back);                     // idle: BACK goes to the list,
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 0);
    press(ui, Button::Back);                     // then leaves the page
    CHECK_EQ(int(ui.menu_depth()), 0);
}

void test_leaving_cancels_own_run() {
    reset();
    add_three();
    Ui &ui = open_page();
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 1);
    ui.goto_page(uint8_t(Page::Home), g_t + 10);  // paged away while it runs
    CHECK_EQ(g_stub.twin_cancels, 1);
}

void test_delete_confirm() {
    reset();
    add_three();
    Ui &ui = open_page();
    open_twin(ui, 1);                            // Laptop
    press(ui, Button::Plus);
    press(ui, Button::Plus);
    press(ui, Button::Ok);                       // arms, deletes nothing
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
    CHECK_EQ(int(g_stub.twins.size()), 3);
    CHECK_STR(footer_of(ui).c_str(), "Deletes this twin for good");
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    press(ui, Button::Ok);
    CHECK_EQ(int(g_stub.twins.size()), 2);
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 0);
    CHECK_STR(footer_of(ui).c_str(), "Twin deleted");
    CHECK_EQ(ui.twin_page().list_state().count, 3);
    CHECK_STR(text_of(ui, 1).c_str(), "OP15");
}

void test_learn_row() {
    reset();
    Ui &ui = open_page();
    g_stub.twin_learned.how = twin_hw::Learn::NotReady;
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.twin_learns, 1);
    CHECK_STR(footer_of(ui).c_str(), "Plug a device in alone");

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

// A phone alone on the K2 (VBUS dark, no source caps, its Rp toggling): OK on the Learn row reads it first (the same Identify as
// Who's attached > Sink), shows that it is reading, and learns once the Identify is over. Red first: the row only learned from an
// Identify the user had run by hand ("Run Identify first").
void make_alone(Ui &ui) {
    g_stub.cc = 1;
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 0;
    g_stub.caps.valid = false;
    g_stub.caps_this_attach = false;
    for (int i = 0; i < 15; i++) {   // the dual-role toggle: Rp on CC2 every other 100 ms
        adc::Reading r;
        r.cc2_mv = static_cast<uint16_t>(i % 2 == 0 ? 3300 : 0);
        g_stub.adc = r;
        step(ui, 100);
    }
    CHECK_EQ(int(ui.link().wait_kind()), int(raguard::WaitKind::DualRolePort));
}

void test_learn_reads_a_device_alone() {
    reset();
    Ui &ui = open_page();
    make_alone(ui);
    g_stub.ident.state = pd::IdState::Running;   // the driver is busy from identify_start() on
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.identifies, 1);
    CHECK_EQ(g_stub.twin_learns, 0);
    CHECK_EQ(ui.twin_page().reading() ? 1 : 0, 1);
    step(ui, 100);
    CHECK_STR(footer_of(ui).c_str(), "Reading the device");
    CHECK_STR(label(ui, Button::Back), "Stop");
    press(ui, Button::Ok);                          // a second OK while it reads starts nothing
    CHECK_EQ(g_stub.identifies, 1);

    twin::Twin t;
    const char name[] = "Pixel";
    for (std::size_t i = 0; i + 1 < sizeof name; i++) t.name[i] = name[i];
    t.pdos[t.num_pdos++] = 0x3601912C;
    g_stub.twin_learned.how = twin_hw::Learn::Ok;
    g_stub.twin_learned.twin = t;
    g_stub.ident.state = pd::IdState::Done;
    g_stub.ident_current = true;
    step(ui, 100);
    CHECK_EQ(g_stub.twin_learns, 1);
    CHECK_EQ(ui.twin_page().reading() ? 1 : 0, 0);
    CHECK_STR(footer_of(ui).c_str(), "Twin learned");
    CHECK_STR(text_of(ui, 0).c_str(), "Pixel");
}

void test_learn_read_without_answer() {
    reset();
    Ui &ui = open_page();
    make_alone(ui);
    g_stub.ident.state = pd::IdState::Running;
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.identifies, 1);
    g_stub.ident.state = pd::IdState::Aborted;     // no contract, or the VDMs went unanswered
    step(ui, 100);
    CHECK_EQ(g_stub.twin_learns, 0);
    CHECK_STR(footer_of(ui).c_str(), "Device did not answer");

    step(ui, 3000);
    make_alone(ui);                                 // the phone keeps toggling
    g_stub.ident.state = pd::IdState::Running;     // Back stops a read in flight; nothing is learned
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.identifies, 2);
    CHECK_EQ(ui.twin_page().reading() ? 1 : 0, 1);
    press(ui, Button::Back);
    CHECK_EQ(int(ui.menu_depth()) > 0 ? 1 : 0, 1);  // Back stopped the read, it did not pop the page
    if (ui.menu_depth() == 0) return;
    CHECK_EQ(ui.twin_page().reading() ? 1 : 0, 0);
    CHECK_EQ(g_stub.twin_learns, 0);
}

void test_footer_note_times_out() {
    reset();
    Ui &ui = open_page();
    g_stub.twin_learned.how = twin_hw::Learn::NotReady;
    press(ui, Button::Ok);
    CHECK_STR(footer_of(ui).c_str(), "Plug a device in alone");
    step(ui, 3000);
    CHECK_STR(footer_of(ui).c_str(), "No twin yet. Learn one");
}

// ---- wake on prove (docs/UI_SYSTEM.md 9.9) ----
// A charger that has not offered: its Rp sits steady on CC1, VBUS is dark, nothing was listed. g_stub.twin_offer is what it lists
// once the K2 presents Rd (the stub's Env::wake delivers it after g_stub.twin_wake_ms).
void make_waiting(Ui &ui) {
    g_stub.twin_offer = g_stub.caps;
    g_stub.caps = {};
    g_stub.caps_this_attach = false;
    g_stub.cc = 0;
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 0;
    adc::Reading r;
    r.cc1_mv = 3300;
    g_stub.adc = r;
    for (int i = 0; i < 15; i++) step(ui, 100);
    CHECK_EQ(int(ui.link().wait_kind()), int(raguard::WaitKind::ChargerWaiting));
}

void test_wake_not_before_confirm() {
    reset();
    add_three();
    Ui &ui = open_page();
    make_waiting(ui);
    CHECK_STR(footer_of(ui).c_str(), "Charger has not offered");   // the list: a charger waits, it is not "No charger"
    CHECK_STR(note_of(ui, 0).c_str(), "charger has not offered");
    CHECK_EQ(int(plain_of(ui, 0).marker), int(kit::Marker::None));   // nothing to predict yet
    open_twin(ui, 0);
    CHECK_STR(footer_of(ui).c_str(), "Charger has not offered");
    CHECK_STR(note_of(ui, 0).c_str(), "wakes the charger first");
    CHECK_STR(label(ui, Button::Ok), "Run");
    press(ui, Button::Ok);                       // arms, presents nothing
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(ui.twin_page().armed() ? 1 : 0, 1);
    CHECK_STR(footer_of(ui).c_str(), "Wakes the charger. Confirm");
    CHECK_STR(label(ui, Button::Ok), "Confirm");
    step(ui, 200);
    CHECK_EQ(g_stub.rd_run_changes, 0);          // opening the page and arming never touched Rd
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 0);
}

void test_wake_then_prove() {
    reset();
    add_three();
    Ui &ui = open_page();
    make_waiting(ui);
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);                       // confirmed: Rd goes on, the charger is woken
    CHECK_EQ(g_stub.twin_starts, 1);
    CHECK_EQ(g_stub.twin_start_confirmed ? 1 : 0, 1);
    CHECK_EQ(g_stub.twin_start_keep_wake ? 1 : 0, 1);   // the page keeps the woken Rd until it is left
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 1);
    CHECK_EQ(twin_hw::live().waking() ? 1 : 0, 1);
    CHECK_STR(footer_of(ui).c_str(), "Waking charger");
    CHECK_STR(label(ui, Button::Back), "Stop");
    step(ui, 100);
    CHECK_STR(note_of(ui, 0).c_str(), "waking the charger");
    run_out(ui);                                 // caps arrive, the prediction is made, the proof runs and ends
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    CHECK_EQ(int(g_stub.caps.valid), 1);
    CHECK_EQ(int(spec_of(ui, 0).state), int(kit::Verdict::Pass));
    CHECK_STR(note_of(ui, 0).c_str(), "proved 33W");
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 1);          // held: without Rd the caps would vanish under the verdict
    CHECK_EQ(g_stub.twin_wake_releases, 0);
    press(ui, Button::Back);                     // to the list
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 1);
    press(ui, Button::Back);                     // leaves the page: Rd goes back
    CHECK_EQ(g_stub.twin_wake_releases, 1);
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 0);
}

void test_wake_refused_without_a_charger() {
    reset();
    add_three();
    g_stub.caps.valid = false;                   // nothing on the plug: Unknown
    Ui &ui = open_page();
    open_twin(ui, 0);
    CHECK_STR(footer_of(ui).c_str(), "No charger: no predictions");
    CHECK_STR(note_of(ui, 0).c_str(), "no charger to predict from");
    press(ui, Button::Ok);
    CHECK_STR(footer_of(ui).c_str(), "No charger to run against");
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(g_stub.rd_run_changes, 0);

    reset();                                     // a phone toggling its Rp (dual-role): never woken
    add_three();
    Ui &ui2 = open_page();
    make_alone(ui2);
    open_twin(ui2, 0);
    CHECK_STR(footer_of(ui2).c_str(), "No charger: no predictions");
    press(ui2, Button::Ok);
    CHECK_STR(footer_of(ui2).c_str(), "No charger to run against");
    CHECK_EQ(g_stub.twin_starts, 0);
    CHECK_EQ(g_stub.rd_run_changes, 0);
}

void test_wake_no_offer() {
    reset();
    add_three();
    g_stub.twin_wake_answers = false;            // the charger never lists
    Ui &ui = open_page();
    make_waiting(ui);
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 500);
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 1);
    CHECK_STR(footer_of(ui).c_str(), "Waking charger");
    run_out(ui);
    CHECK_EQ(int(twin_hw::live().reason()), int(twin::live::Reason::NoOffer));
    CHECK_STR(footer_of(ui).c_str(), "Charger did not offer");
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 0);          // given back at once, the page has nothing to hold
    CHECK_EQ(g_stub.twin_unwakes, 1);
    CHECK_EQ(g_stub.twin_starts, 1);
}

void test_wake_back_stops() {
    reset();
    add_three();
    g_stub.twin_wake_answers = false;
    Ui &ui = open_page();
    make_waiting(ui);
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(twin_hw::live().waking() ? 1 : 0, 1);
    press(ui, Button::Back);
    CHECK_EQ(g_stub.twin_cancels, 1);
    CHECK_STR(footer_of(ui).c_str(), "Stopped. Charger released");
    run_out(ui);
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 0);
    CHECK_EQ(g_stub.twin_unwakes, 1);
    CHECK_EQ(twin_hw::live_running() ? 1 : 0, 0);
    CHECK_EQ(ui.twin_page().in_detail() ? 1 : 0, 1);   // BACK stopped the wake, it did not leave the page
}

void test_wake_leaving_releases() {
    reset();
    add_three();
    g_stub.twin_wake_answers = false;
    Ui &ui = open_page();
    make_waiting(ui);
    open_twin(ui, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 150);
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 1);
    ui.goto_page(uint8_t(Page::Home), g_t + 10);   // paged away mid-wake
    step(ui, 300);
    CHECK_EQ(g_stub.twin_cancels, 1);
    CHECK_EQ(g_stub.rd_run ? 1 : 0, 0);
}

}  // namespace

int main() {
    test_rows_and_footer();
    test_no_charger_and_empty();
    test_repredict_on_new_charger();
    test_detail_opens_and_closes();
    test_delete_confirm();
    test_arm_confirm_run();
    test_reject_is_a_fail();
    test_arm_expires_and_moves();
    test_refusals();
    test_back_stops_the_run();
    test_leaving_cancels_own_run();
    test_learn_row();
    test_learn_reads_a_device_alone();
    test_learn_read_without_answer();
    test_footer_note_times_out();
    test_wake_not_before_confirm();
    test_wake_then_prove();
    test_wake_refused_without_a_charger();
    test_wake_no_offer();
    test_wake_back_stops();
    test_wake_leaving_releases();
    return test_report("test_ui_twin");
}
