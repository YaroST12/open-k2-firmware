// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools > Loop probe: the page through the real Ui and the REAL loopprobe::Runner over the stub Env and the RAM result file
// (ui_stubs.cpp, k2::loopprobe::hw). Start needs the confirm; the run ends in the share view; BACK and leaving stop it.
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "fake_lcd.h"
#include "k2/loopprobe/loop_probe.hpp"
#include "k2/ui/ui.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;
namespace lp = k2::loopprobe;

namespace {

uint32_t g_t = 200000;

void reset() { g_stub = StubState(); }

Ui &ui_instance() {
    static Ui ui;
    return ui;
}

Ui &open_page() {
    Ui &ui = ui_instance();
    g_t += 1000;
    ui.init(g_t);
    ui.tick(g_t);
    ui.goto_page(uint8_t(Page::Tools), g_t + 50);
    ui.push_menu(Menu::LoopProbe, g_t + 100);
    g_t += 150;
    ui.tick(g_t);
    return ui;
}

void step(Ui &ui, uint32_t ms) {
    g_t += ms;
    ui.tick(g_t);
}

void press(Ui &ui, Button b, ButtonEvent e = ButtonEvent::Short) {
    g_t += 30;
    ui.on_button(b, e, g_t);
}

std::string ok_short(Ui &ui) { return ui.labels().b[int(Button::Ok)].shrt; }
std::string ok_long(Ui &ui) { return ui.labels().b[int(Button::Ok)].lng; }
std::string back(Ui &ui) { return ui.labels().b[int(Button::Back)].shrt; }
std::string footer(Ui &ui) { return std::string(ui.loop_probe_page()->footer()); }
std::string cell(Ui &ui, int i) { return std::string(ui.loop_probe_page()->step_text(i)); }

// Ticks until the run is over (the page opens the share view then), at most `limit_ms`.
void run_out(Ui &ui, uint32_t limit_ms = 30000) {
    for (uint32_t t = 0; t < limit_ms && ui.loop_probe_busy(); t += 20) step(ui, 20);
}

// The share view carries `r` as the probe encodes it: the record itself, or its LZ4 block when that saved a quarter (the
// share packs any record that way; tools/k2_qr.py unpacks it before the loop decoder).
bool share_holds(Ui &ui, const lp::Record &r) {
    std::vector<uint8_t> rec(lp::kRecordBytes);
    if (lp::encode(r, rec) != lp::kRecordBytes) return false;
    const auto shown = ui.share().record();
    if (!ui.share().packed()) return std::vector<uint8_t>(shown.begin(), shown.end()) == rec;
    std::vector<uint8_t> z(lp::kRecordBytes);
    const size_t n = qrshare::pack(rec, z);
    return n == shown.size() && std::equal(shown.begin(), shown.end(), z.begin());
}

std::vector<uint8_t> in_progress_file() {
    lp::Record r;
    r.flags = lp::kFlagInProgress;
    for (auto &s : r.step) s.status = uint8_t(lp::Status::Done);
    r.at(lp::Step::Cc).status = uint8_t(lp::Status::NotRun);
    std::vector<uint8_t> f(lp::kFileBytes);
    CHECK_EQ(lp::encode_file(r, f), lp::kFileBytes);
    return f;
}

void test_idle_without_a_result() {
    reset();
    Ui &ui = open_page();
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));
    CHECK_STR(footer(ui).c_str(), "Cable into own HID port");
    CHECK_STR(ok_short(ui).c_str(), "Start");
    CHECK_STR(ok_long(ui).c_str(), "");                       // nothing to share yet
    CHECK_STR(back(ui).c_str(), "Back");
    for (int i = 0; i < int(lp::kSteps); i++) CHECK_STR(cell(ui, i).c_str(), "--");
    press(ui, Button::Ok, ButtonEvent::Long);
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));            // no share view without a record
    CHECK_STR(footer(ui).c_str(), "No result to share");
}

void test_confirm_window() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    CHECK_EQ(ui.loop_probe_page()->armed(), true);
    CHECK_EQ(ui.loop_probe_busy(), false);                     // the first OK only arms
    CHECK_STR(ok_short(ui).c_str(), "Confirm");
    step(ui, Ui::kConfirmWindowMs + 100);
    CHECK_EQ(ui.loop_probe_page()->armed(), false);            // the window ran out
    CHECK_STR(ok_short(ui).c_str(), "Start");
    press(ui, Button::Ok);                                     // a fresh arm, not a start
    CHECK_EQ(ui.loop_probe_busy(), false);
}

void test_full_run_ends_in_the_share_view() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    CHECK_EQ(ui.loop_probe_busy(), true);
    CHECK_EQ(ui.ports_busy(), true);                           // other tools and menus are kept out
    CHECK_EQ(int(ui.busy_why()), int(BusyWhy::LoopProbe));
    CHECK_STR(ui.busy_reason(), "Loop probe: stop it");
    CHECK_STR(back(ui).c_str(), "Stop");
    CHECK_STR(ok_short(ui).c_str(), "");
    CHECK_STR(footer(ui).c_str(), "Running. 5V only");
    CHECK_STR(cell(ui, 0).c_str(), "running");
    // The CC step is on screen (caution footer, its row running) before its hold, with nothing held yet.
    for (uint32_t t = 0; t < 30000 && ui.loop_probe_busy() && footer(ui) != "CC step: VBUS may drop"; t += 20) step(ui, 20);
    CHECK_STR(footer(ui).c_str(), "CC step: VBUS may drop");
    CHECK_STR(cell(ui, 4).c_str(), "running");
    CHECK_EQ(g_stub.lp_cc_holds, 1);                            // only the baseline's as-found look so far
    run_out(ui);
    CHECK_EQ(ui.loop_probe_busy(), false);
    CHECK_EQ(int(ui.menu()), int(Menu::Share));
    CHECK_EQ(int(ui.share().type()), int(qrshare::Type::LoopProbe));
    CHECK_STR(std::string(ui.share().title()).c_str(), "Loop probe");
    CHECK_EQ(int(ui.share().schema()), int(qrshare::kSchemaLoop));
    CHECK_EQ(g_stub.lp_cc_holds, 2);                           // the as-found look in Baseline, then the CC step's plan
    CHECK_EQ(g_stub.lp_writes >= 2, true);                     // in progress before CC, then the result
    // The share view shows what the flash holds.
    const auto stored = lp::decode_file(g_stub.lp_file);
    CHECK_EQ(stored.has_value(), true);
    if (stored) {
        CHECK_EQ(share_holds(ui, *stored), true);
        CHECK_EQ(int(stored->flags), 0);                       // the in-progress mark is cleared at the end
        for (size_t i = 0; i < lp::kSteps; i++) CHECK_EQ(stored->step[i].status != uint8_t(lp::Status::NotRun), true);
        CHECK_STR(stored->fw.data(), "1.0.0-0-host");
    }
    // Back from the code: the page says the run is done and keeps the table.
    press(ui, Button::Back);
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));
    step(ui, 50);
    CHECK_STR(footer(ui).c_str(), "Done. Result saved");
    CHECK_STR(cell(ui, 0).substr(0, 5).c_str(), "done ");
    CHECK_STR(ok_long(ui).c_str(), "Share");
    press(ui, Button::Ok, ButtonEvent::Long);                  // shares the same record again
    CHECK_EQ(int(ui.menu()), int(Menu::Share));
}

void test_back_stops_a_run() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 100);
    CHECK_EQ(ui.loop_probe_busy(), true);
    press(ui, Button::Back);
    CHECK_EQ(ui.loop_probe_busy(), false);
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));            // stopped, not popped, and no share view
    CHECK_STR(footer(ui).c_str(), "Stopped. Result saved");
    CHECK_STR(cell(ui, 0).c_str(), "stopped");
    CHECK_STR(back(ui).c_str(), "Back");
    const auto stored = lp::decode_file(g_stub.lp_file);
    CHECK_EQ(stored.has_value(), true);
    if (stored) {
        CHECK_EQ(int(stored->at(lp::Step::Baseline).status), int(lp::Status::Aborted));
        CHECK_EQ(int(stored->at(lp::Step::Baseline).why), int(lp::Why::Cancelled));
    }
    step(ui, 3000);                                            // the note goes, the state stays
    CHECK_STR(footer(ui).c_str(), "Stopped. Result saved");
    CHECK_EQ(g_stub.lp_cc_holds, 0);
}

void test_leaving_the_page_stops_a_run() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 100);
    CHECK_EQ(ui.loop_probe_busy(), true);
    ui.goto_page(uint8_t(Page::Home), g_t + 10);               // the remote's page change, or paging away
    CHECK_EQ(ui.loop_probe_page() == nullptr, true);
    CHECK_EQ(ui.loop_probe_busy(), false);
    const auto stored = lp::decode_file(g_stub.lp_file);
    CHECK_EQ(stored.has_value() && stored->at(lp::Step::Baseline).why == uint8_t(lp::Why::Cancelled), true);
}

void test_cancel_runs_stops_a_run() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    step(ui, 100);
    ui.cancel_runs();                                          // the remote's stop / the lease expiry
    CHECK_EQ(ui.loop_probe_busy(), false);
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));
}

void test_refusals() {
    reset();
    g_stub.lp_vbus_mv = 9000;                                  // a charger left at 9 V: never runs
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    CHECK_EQ(ui.loop_probe_busy(), false);
    CHECK_STR(footer(ui).c_str(), "VBUS above 5.5V: refused");
    CHECK_EQ(g_stub.lp_writes, 0);                             // a refused start writes nothing

    reset();
    g_stub.lp_aux_mv = 6000;
    Ui &u2 = open_page();
    press(u2, Button::Ok);
    press(u2, Button::Ok);
    CHECK_EQ(u2.loop_probe_busy(), false);
    CHECK_STR(footer(u2).c_str(), "Aux above 5.8V: refused");

    reset();
    g_stub.pm_online = false;                                  // no power monitor: OK does not even arm
    Ui &u3 = open_page();
    CHECK_STR(footer(u3).c_str(), "No power meter");
    press(u3, Button::Ok);
    CHECK_EQ(u3.loop_probe_page()->armed(), false);
    CHECK_EQ(u3.loop_probe_busy(), false);
}

void test_stored_result_on_enter() {
    reset();
    Ui &ui = open_page();
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run_out(ui);
    const std::vector<uint8_t> file = g_stub.lp_file;
    // A later visit: the table and the long OK come from the flash.
    Ui &u2 = open_page();
    CHECK_EQ(g_stub.lp_file == file, true);
    CHECK_STR(footer(u2).c_str(), "Last result on flash");
    CHECK_STR(ok_long(u2).c_str(), "Share");
    CHECK_STR(cell(u2, 0).substr(0, 5).c_str(), "done ");
    press(u2, Button::Ok, ButtonEvent::Long);
    CHECK_EQ(int(u2.menu()), int(Menu::Share));
    CHECK_EQ(int(u2.share().type()), int(qrshare::Type::LoopProbe));
    const auto stored = lp::decode_file(file);
    CHECK_EQ(stored.has_value(), true);
    if (stored) CHECK_EQ(share_holds(u2, *stored), true);
}

void test_reboot_in_cc_is_reported() {
    reset();
    g_stub.lp_file = in_progress_file();
    g_stub.lp_file_present = true;
    Ui &ui = open_page();                                      // Ui::init runs the boot check
    const auto stored = lp::decode_file(g_stub.lp_file);
    CHECK_EQ(stored.has_value(), true);
    if (stored) {
        CHECK_EQ(int(stored->flags & lp::kFlagInProgress), 0);
        CHECK_EQ(int(stored->at(lp::Step::Cc).status), int(lp::Status::Interrupted));
    }
    CHECK_STR(footer(ui).c_str(), "Last run rebooted in CC");
    CHECK_STR(cell(ui, int(lp::Step::Cc)).c_str(), "rebooted");
}

void test_tools_row_opens_the_page() {
    reset();
    Ui &ui = ui_instance();
    g_t += 1000;
    ui.init(g_t);
    ui.goto_page(uint8_t(Page::Tools), g_t + 50);
    for (int i = 0; i < ToolsPage::kTools; i++) ui.on_button(Button::Plus, ButtonEvent::Short, g_t + 60);   // to the last row
    ui.on_button(Button::Ok, ButtonEvent::Short, g_t + 100);
    CHECK_EQ(int(ui.menu()), int(Menu::LoopProbe));
    CHECK_STR(ToolsPage::tool_name(kToolLoopProbe), "Loop probe");
    CHECK_EQ(int(ToolsPage::tool_menu(kToolLoopProbe)), int(Menu::LoopProbe));
}

}  // namespace

int main() {
    test_idle_without_a_result();
    test_confirm_window();
    test_full_run_ends_in_the_share_view();
    test_back_stops_a_run();
    test_leaving_the_page_stops_a_run();
    test_cancel_runs_stops_a_run();
    test_refusals();
    test_stored_result_on_enter();
    test_reboot_in_cc_is_reported();
    test_tools_row_opens_the_page();
    return test_report("test_ui_loop_probe");
}
