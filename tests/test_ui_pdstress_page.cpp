// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools > PD Stress, the page (src/ui/ui_pdstress_page.cpp) through the real Ui, the real PdStressSession and engine over the stub
// converter, and the saved-result store on the stub RAM disk (tests/ui_stubs.cpp): Setup picks, Review arms and starts with the
// right Options, Running follows the run, Results shows the verdicts, Save / Discard close the result, a saved one opens again.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pdstress_page.hpp"
#include "k2/ui/ui_tools.hpp"
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;
namespace ps = pdstress;
namespace dg = pdstress::digest;

namespace {

// A 5 / 9 / 15 / 20 V, 3 A charger (no PPS) on a monitor-equipped K2 whose stub converter follows every Request.
void fixture() {
    g_stub = {};
    stub_ps_saved_reset();
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.vbus_ceiling_mv = 20000;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 4; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300);
    g_stub.caps.pdos[2] = pd::decode_pdo((300u << 10) | 300);
    g_stub.caps.pdos[3] = pd::decode_pdo((400u << 10) | 300);
    g_stub.caps.raw_header = 0x1181;
    g_stub.sink.state = pd::SinkState::Ready; g_stub.sink.result = pd::SinkResult::Ok;
    g_stub.sink.pos = 1; g_stub.sink.mv = 5000; g_stub.sink.ps_rdy_inferred = true;
}

uint32_t g_now = 1000;
void run(Ui &ui, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) { g_now += 5; ui.tick(g_now); }
}
void press(Ui &ui, Button b, ButtonEvent e = ButtonEvent::Short) {
    g_now += 30;
    ui.on_button(b, e, g_now);
}
void presses(Ui &ui, Button b, int n) { for (int i = 0; i < n; i++) press(ui, b); }

Ui &boot(std::unique_ptr<Ui> &h) {
    h = std::make_unique<Ui>();
    h->init(g_now);
    h->set_rotation(0, g_now);
    run(*h, 20);
    return *h;
}
PdStressPage &open(Ui &ui) {
    ui.goto_page(uint8_t(Page::Tools), g_now);
    ui.push_menu(Menu::PdStress, g_now + 10);
    run(ui, 120);
    return *ui.pd_stress_page();
}
void reopen(Ui &ui) {
    ui.goto_page(uint8_t(Page::Home), g_now);
    run(ui, 50);
    open(ui);
}
std::string foot(Ui &ui) { return std::string(ui.pd_stress_page()->footer()); }
std::string ok_label(Ui &ui) { return ui.labels().b[int(Button::Ok)].shrt; }
std::string back_label(Ui &ui) { return ui.labels().b[int(Button::Back)].shrt; }
std::string tail_of(PdStressPage &p, int row) {
    kit::RowSpec r;
    p.setup_row(row, false, r);
    return std::string(r.tail);
}

// The live result decoded on its own, to check the page against.
dg::Digest live_digest(Ui &ui) {
    dg::Digest d;
    dg::load(d, [](void *c, std::size_t off, std::span<std::uint8_t> o) { return static_cast<const PdStressSession *>(c)->result_read(off, o); },
             &ui.pd_stress());
    return d;
}

// Setup straight to Review with these options (the cursor parked on Done).
void to_review(Ui &ui, uint16_t mask, uint8_t cycles, uint8_t pace) {
    auto &k = ui.context().keep.pdstress;
    k.mask = mask; k.cycles = cycles; k.pace = pace; k.sel = PdStressPage::kDoneRow;
    run(ui, 120);
    press(ui, Button::Ok);
}

// Review -> START -> the run to its end.
void run_to_results(Ui &ui, uint16_t mask) {
    to_review(ui, mask, 0, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    for (int i = 0; i < 400 && ui.pd_stress().active(); i++) run(ui, 500);
    run(ui, 200);
}

// ---- Setup ------------------------------------------------------------------------------------------------------------------------------

void test_setup_opens_with_the_default_pick() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    CHECK_EQ(int(ui.menu()), int(Menu::PdStress));
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "11 of 12 picked");
    kit::RowSpec r;
    p.setup_row(0, true, r);
    CHECK_STR(std::string(r.text).c_str(), "P1 Fixed ladder");
    CHECK_EQ(r.state, 1);
    CHECK_EQ(r.note.empty(), false);
    p.setup_row(int(ps::Seq::Epr), false, r);
    CHECK_STR(std::string(r.text).c_str(), "P9 EPR 28V");
    CHECK_EQ(r.state, 0);                                   // opt-in
    CHECK_STR(tail_of(p, PdStressPage::kCyclesRow).c_str(), "100 %");
    CHECK_STR(tail_of(p, PdStressPage::kPaceRow).c_str(), "per sequence");
    CHECK_STR(ok_label(ui).c_str(), "Toggle");
    CHECK_EQ(int(ui.context().keep.pdstress.mask), int(ps::kDefaultSeqMask));
}

void test_setup_toggles_the_mask() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    open(ui);
    auto &k = ui.context().keep.pdstress;
    k.sel = 0;
    press(ui, Button::Ok);                                  // P1 off
    CHECK_EQ(k.mask & ps::seq_bit(ps::Seq::Ladder), 0);
    CHECK_STR(foot(ui).c_str(), "10 of 12 picked");
    presses(ui, Button::Plus, int(ps::Seq::Epr));           // down to P9
    CHECK_EQ(int(k.sel), int(ps::Seq::Epr));
    press(ui, Button::Ok);                                  // P9 on
    CHECK_EQ(k.mask & ps::seq_bit(ps::Seq::Epr), ps::seq_bit(ps::Seq::Epr));
    CHECK_STR(foot(ui).c_str(), "11 of 12 picked");
    press(ui, Button::Minus);
    CHECK_EQ(int(k.sel), int(ps::Seq::Epr) - 1);
}

void test_cycles_and_pace_cycle_on_ok() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    auto &k = ui.context().keep.pdstress;
    k.sel = 0;
    presses(ui, Button::Plus, PdStressPage::kCyclesRow);
    CHECK_EQ(int(k.sel), PdStressPage::kCyclesRow);
    CHECK_STR(ok_label(ui).c_str(), "Cycle");
    press(ui, Button::Ok);
    CHECK_STR(tail_of(p, PdStressPage::kCyclesRow).c_str(), "200 %");
    press(ui, Button::Ok);
    CHECK_STR(tail_of(p, PdStressPage::kCyclesRow).c_str(), "25 %");
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    CHECK_STR(tail_of(p, PdStressPage::kPaceRow).c_str(), "all Settled");
    press(ui, Button::Ok);
    CHECK_STR(tail_of(p, PdStressPage::kPaceRow).c_str(), "all Ready");
    press(ui, Button::Ok);
    CHECK_STR(tail_of(p, PdStressPage::kPaceRow).c_str(), "per sequence");
    presses(ui, Button::Plus, 5);                           // the list ends at Done (no saved results yet)
    CHECK_EQ(int(k.sel), PdStressPage::kDoneRow);
    CHECK_STR(ok_label(ui).c_str(), "Next");
}

void test_done_with_nothing_picked_stays() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    to_review(ui, 0, 2, 0);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "Pick at least one test");
}

// ---- Review and START ---------------------------------------------------------------------------------------------------------------

void test_start_needs_two_presses_and_passes_the_options() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    const uint16_t mask = ps::seq_bit(ps::Seq::Ladder) | ps::seq_bit(ps::Seq::Slam);
    to_review(ui, mask, 0, 1);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Review));
    CHECK_STR(foot(ui).c_str(), "Keep the output empty");
    CHECK_STR(ok_label(ui).c_str(), "Start");
    press(ui, Button::Ok);
    CHECK_EQ(p.armed(), true);
    CHECK_EQ(ui.pd_stress().active(), false);               // the first OK only arms
    CHECK_STR(ok_label(ui).c_str(), "Confirm");
    run(ui, Ui::kConfirmWindowMs + 100);
    CHECK_EQ(p.armed(), false);                             // the window ran out
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    CHECK_EQ(ui.pd_stress().running(), true);
    run(ui, 120);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Running));
    const ps::Options &o = ui.pd_stress().engine()->options();
    CHECK_EQ(o.mask, mask);
    CHECK_EQ(o.cycles_pct, 25);
    CHECK_EQ(o.pace, 1);
    CHECK_STR(back_label(ui).c_str(), "Stop");
    CHECK_STR(ok_label(ui).c_str(), "");
}

void test_back_on_review_returns_to_setup() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    to_review(ui, ps::kDefaultSeqMask, 2, 0);
    press(ui, Button::Ok);                                  // armed
    press(ui, Button::Back);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Setup));
    CHECK_EQ(p.armed(), false);
    CHECK_EQ(int(ui.menu()), int(Menu::PdStress));
}

void test_a_refused_start_says_why() {
    fixture();
    g_stub.caps.valid = false;
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    to_review(ui, ps::kDefaultSeqMask, 2, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Review));
    CHECK_STR(foot(ui).c_str(), "No PD source");
    CHECK_EQ(ui.pd_stress().active(), false);
}

// ---- Running ------------------------------------------------------------------------------------------------------------------------

void test_running_shows_the_counts_and_back_stops() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    to_review(ui, ps::seq_bit(ps::Seq::Ladder), 2, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 3000);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Running));
    const uint32_t tr = ui.pd_stress().engine()->transitions();
    CHECK_EQ(tr > 0, true);
    CHECK_EQ(p.count_value(0), tr);
    CHECK_EQ(p.count_value(5), 0u);                         // no reboots
    CHECK_STR(foot(ui).c_str(), "Running: keep output empty");
    press(ui, Button::Back);
    CHECK_EQ(int(ui.menu()), int(Menu::PdStress));          // BACK stops, it does not leave
    for (int i = 0; i < 40 && ui.pd_stress().active(); i++) run(ui, 500);
    run(ui, 200);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Results));
    CHECK_EQ(int(p.result_stop()), int(ps::Stop::Cancelled));
    CHECK_STR(foot(ui).c_str(), "Stopped");
}

// Level 1 while a run goes on: after the layout tick a tick paints at most the budget (the hero at 4 Hz, the clock, the bar, the
// texts and counts that changed), plus the TopBar's live readout, which every page with a TopBar pays.
void test_running_paints_inside_the_budget() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    to_review(ui, ps::seq_bit(ps::Seq::Ladder) | ps::seq_bit(ps::Seq::SameReq), 2, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 100);                                            // the layout tick
    unsigned ticks = 0, max_px = 0;
    for (int i = 0; i < 4000 && ui.pd_stress().active(); i++) {
        g_now += 5;
        g_stub.pm.vbus_mv = g_stub.sink.mv + (i % 7) * 10;  // the hero digits change on every repaint
        const unsigned px0 = g_lcd_pixels;
        ui.tick(g_now);
        if (p.stage() != PdStressPage::Stage::Running) break;
        ticks++;
        max_px = std::max(max_px, g_lcd_pixels - px0);
    }
    std::printf("[pdstress] running ticks=%u max_px=%u\n", ticks, max_px);
    CHECK_EQ(ticks > 100, true);
    CHECK_EQ(max_px <= unsigned(PdStressPage::kRunBudgetPx + PdStressPage::kChromePx), true);
    ui.pd_stress().cancel();
    run(ui, 6000);
}

// ---- Results --------------------------------------------------------------------------------------------------------------------------

void test_results_show_a_row_per_picked_sequence() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder) | ps::seq_bit(ps::Seq::PpsSteps));
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Results));
    CHECK_EQ(p.viewing(), 0);
    CHECK_EQ(p.result_valid(), true);
    CHECK_EQ(int(p.result_stop()), int(ps::Stop::Done));
    CHECK_STR(foot(ui).c_str(), "Done");
    CHECK_EQ(p.result_rows(), 2 + 2);                       // P1, P6, then Save result / Discard
    kit::ResultRowSpec r;
    p.result_row(0, r);
    CHECK_STR(std::string(r.label).c_str(), "P1 Fixed");
    CHECK_EQ(r.n, 1);
    const dg::Digest d = live_digest(ui);
    CHECK_EQ(int(p.seq_verdict(ps::Seq::Ladder)), int(dg::verdict(d, ps::Seq::Ladder)));
    CHECK_EQ(int(p.seq_verdict(ps::Seq::PpsSteps)), int(dg::Verdict::Na));
    if (dg::verdict(d, ps::Seq::Ladder) == dg::Verdict::Clean) {
        CHECK_EQ(int(r.cells[0].verdict), int(kit::Verdict::Pass));
        CHECK_STR(std::string(r.cells[0].label).c_str(), "clean");
    } else {
        CHECK_EQ(int(r.cells[0].verdict), int(kit::Verdict::Partial));
    }
    p.result_row(1, r);                                     // no PPS on this charger: the reason, not a failure
    CHECK_STR(std::string(r.label).c_str(), "P6 PPS");
    CHECK_EQ(int(r.cells[0].verdict), int(kit::Verdict::None));
    CHECK_STR(std::string(r.cells[0].label).c_str(), "no PPS");
    p.result_row(2, r);
    CHECK_STR(std::string(r.cells[0].label).c_str(), "Save result");
    p.result_row(3, r);
    CHECK_STR(std::string(r.cells[0].label).c_str(), "Discard");
    CHECK_STR(ok_label(ui).c_str(), "Open");
}

void test_detail_pages_through_the_counters() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder));
    press(ui, Button::Ok);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Detail));
    CHECK_EQ(p.detail_page(), 0);
    const dg::Digest d = live_digest(ui);
    CHECK_EQ(d.counts[0].transitions > 0, true);
    CHECK_EQ(p.detail_value(0), uint32_t(d.counts[0].transitions));
    CHECK_EQ(p.detail_value(1), uint32_t(d.counts[0].get(ps::Event::AcceptLate)));
    press(ui, Button::Plus);
    CHECK_EQ(p.detail_page(), 1);
    CHECK_EQ(p.detail_value(2), uint32_t(d.counts[0].get(ps::Event::HardResetRx)));
    CHECK_EQ(p.detail_value(6), ps::score_cp(d.counts[0]));
    press(ui, Button::Plus);
    CHECK_EQ(p.detail_page(), 1);                           // two pages
    press(ui, Button::Minus);
    CHECK_EQ(p.detail_page(), 0);
    press(ui, Button::Back);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Results));
}

// ---- Save / Discard ----------------------------------------------------------------------------------------------------------------

void test_save_writes_once_and_lists_the_result() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder));
    CHECK_EQ(g_stub.ps_saves, 0);                           // nothing is written before Save
    press(ui, Button::Plus);                                // P1 -> Save result
    CHECK_STR(ok_label(ui).c_str(), "Select");
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.ps_saves, 1);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "Result saved");
    CHECK_EQ(p.saved_count(), 1);
    CHECK_EQ(ui.context().keep.pdstress.closed_run, ui.pd_stress().run_no());
    kit::RowSpec r;
    p.setup_row(PdStressPage::kSavedRow0, false, r);
    CHECK_STR(std::string(r.text).c_str(), "Result #1");
    CHECK_EQ(std::string(r.tail).rfind("score ", 0) == 0, true);
    reopen(ui);                                              // the result was dealt with: the page opens on Setup
    PdStressPage &q = *ui.pd_stress_page();
    CHECK_EQ(int(q.stage()), int(PdStressPage::Stage::Setup));
    ui.context().keep.pdstress.sel = PdStressPage::kSavedRow0;
    run(ui, 120);
    CHECK_STR(ok_label(ui).c_str(), "Open");
    press(ui, Button::Ok);
    CHECK_EQ(int(q.stage()), int(PdStressPage::Stage::Results));
    CHECK_EQ(q.viewing(), 1);
    CHECK_EQ(q.result_rows(), 1);                            // a saved result has no Save / Discard rows
    press(ui, Button::Back);
    CHECK_EQ(int(q.stage()), int(PdStressPage::Stage::Setup));
}

void test_discard_writes_nothing() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder));
    presses(ui, Button::Plus, 2);
    press(ui, Button::Ok);
    CHECK_EQ(g_stub.ps_saves, 0);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "Result discarded");
    CHECK_EQ(p.saved_count(), 0);
    reopen(ui);
    CHECK_EQ(int(ui.pd_stress_page()->stage()), int(PdStressPage::Stage::Setup));
}

void test_a_failed_save_keeps_the_result() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdStressPage &p = open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder));
    g_stub.ps_save_fails = true;
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Results));
    CHECK_STR(foot(ui).c_str(), "Save failed: flash full");
    CHECK_EQ(ui.context().keep.pdstress.closed_run == ui.pd_stress().run_no(), false);
}

void test_an_unsaved_result_opens_the_page_again() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    open(ui);
    run_to_results(ui, ps::seq_bit(ps::Seq::Ladder));
    press(ui, Button::Back);                                 // Results -> Setup
    CHECK_EQ(int(ui.pd_stress_page()->stage()), int(PdStressPage::Stage::Setup));
    reopen(ui);
    CHECK_EQ(int(ui.pd_stress_page()->stage()), int(PdStressPage::Stage::Results));
}

// A run started with the page closed (the remote, the shell) shows as Running when the page opens.
void test_a_run_started_elsewhere_opens_running() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    ps::Options o;
    o.mask = ps::seq_bit(ps::Seq::Ladder);
    CHECK_EQ(int(ui.pd_stress().start(o, g_now)), int(ps::StartResult::Ok));
    run(ui, 100);
    PdStressPage &p = open(ui);
    CHECK_EQ(int(p.stage()), int(PdStressPage::Stage::Running));
    ui.pd_stress().cancel();
    run(ui, 6000);
}

// ---- Tools ----------------------------------------------------------------------------------------------------------------------------

void test_the_tools_row_opens_the_page() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    ui.context().keep.place_sel[static_cast<int>(bench::Place::Charger)] = 0;
    ui.open_place(bench::Place::Charger, g_now);   // Stress test is the Charger place's last action
    run(ui, 120);
    presses(ui, Button::Plus, bench::actions(bench::Place::Charger).n - 1);
    press(ui, Button::Ok);
    run(ui, 120);
    CHECK_EQ(int(ui.menu()), int(Menu::PdStress));
    CHECK_EQ(int(ToolsPage::tool_menu(kToolPdStress)), int(Menu::PdStress));
    CHECK_STR(ToolsPage::tool_name(kToolPdStress), "PD Stress");
}

// ---- renders ----------------------------------------------------------------------------------------------------------------------------

// K2_DUMP=dir: every stage 1:1 at rotation 3 (buttons at the bottom) into dir/k2_pdstress_<name>.panel (tools/panel2png.py).
void dump(const char *name) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/k2_pdstress_%s.panel", dir, name);
    if (FILE *f = std::fopen(path, "wb")) {
        std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f);
        std::fclose(f);
    }
}

void test_renders() {
    if (!std::getenv("K2_DUMP")) return;
    fixture();
    g_stub.caps.num_pdos = 5;
    g_stub.caps.pdos[3] = pd::decode_pdo((400u << 10) | 500);                      // 20 V 5 A
    g_stub.caps.pdos[4] = pd::Pdo{pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000};
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    ui.set_rotation(3, g_now);
    run(ui, 50);
    PdStressPage &p = open(ui);
    ui.context().keep.pdstress.sel = 1;
    run(ui, 200);
    dump("1_setup");
    ui.context().keep.pdstress.sel = PdStressPage::kCyclesRow;
    run(ui, 200);
    dump("2_setup_cycles");
    to_review(ui, ps::seq_bit(ps::Seq::Ladder) | ps::seq_bit(ps::Seq::SameReq) | ps::seq_bit(ps::Seq::AvsSteps), 2, 0);
    run(ui, 200);
    dump("3_review");
    press(ui, Button::Ok);
    run(ui, 1200);
    dump("4_review_armed");
    press(ui, Button::Ok);
    run(ui, 900);
    dump("5_running");
    for (int i = 0; i < 400 && ui.pd_stress().active(); i++) run(ui, 500);
    run(ui, 300);
    dump("6_results");
    presses(ui, Button::Plus, 3);
    run(ui, 200);
    dump("7_results_save");
    presses(ui, Button::Minus, 3);
    press(ui, Button::Ok);
    run(ui, 200);
    dump("8_detail");
    press(ui, Button::Plus);
    run(ui, 200);
    dump("9_detail_2");
    press(ui, Button::Back);
    presses(ui, Button::Plus, 3);
    press(ui, Button::Ok);                                   // Save result
    run(ui, 200);
    dump("10_saved");
    ui.context().keep.pdstress.sel = PdStressPage::kSavedRow0;
    run(ui, 2600);
    dump("11_setup_last_results");
    g_stub.caps.valid = false;
    to_review(ui, ps::seq_bit(ps::Seq::Ladder), 2, 0);
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 200);
    dump("12_refused");
    (void)p;
}

}  // namespace

int main() {
    test_setup_opens_with_the_default_pick();
    test_setup_toggles_the_mask();
    test_cycles_and_pace_cycle_on_ok();
    test_done_with_nothing_picked_stays();
    test_start_needs_two_presses_and_passes_the_options();
    test_back_on_review_returns_to_setup();
    test_a_refused_start_says_why();
    test_running_shows_the_counts_and_back_stops();
    test_running_paints_inside_the_budget();
    test_results_show_a_row_per_picked_sequence();
    test_detail_pages_through_the_counters();
    test_save_writes_once_and_lists_the_result();
    test_discard_writes_nothing();
    test_a_failed_save_keeps_the_result();
    test_an_unsaved_result_opens_the_page_again();
    test_a_run_started_elsewhere_opens_running();
    test_the_tools_row_opens_the_page();
    test_renders();
    return test_report("ui_pdstress_page");
}
