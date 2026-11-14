// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools > PD Spec Check, the page (src/ui/ui_pdspec_page.cpp) through the real Ui, the real PdSpecSession and engine over the stub
// converter (tests/ui_stubs.cpp): Setup picks families, Review arms and starts, the refusals land in the footer, Running follows the
// run, BACK stops it with the source back at 5 V, Results and Detail read the engine. The engine's own judging is tested in
// tests/test_pdspec_engine.cpp; this file checks the binding and the screens.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pdspec_page.hpp"
#include "k2/ui/ui_tools.hpp"
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;
namespace sc = pdspec;

// Lays a finished result with chosen values into the session's engine (Engine befriends it for exactly this): the Results and Detail
// screens are drawn with every skip reason and the extreme values a real run can leave.
namespace k2::pdspec {
struct EngineProbe {
    // variant 0: every check skipped, the reasons cycling through all of them; 1: all pass at their limit; 2: all fail with nothing
    // seen and the counters saturated; 3: one failure each with negative, huge and one-sided values, the 5 V restore unconfirmed.
    static void lay(const Engine &ce, int variant) {
        Engine &e = const_cast<Engine &>(ce);
        for (std::size_t k = 0; k < kChecks; ++k) {
            const CheckDef &d = kDefs[k];
            Agg a{};
            const bool us = d.unit == Unit::Us, ms = d.unit == Unit::Ms, mv = d.unit == Unit::Mv;
            a.lo = us ? 25000 : ms ? 12000 : mv ? 34200 : 0;
            a.hi = us ? 760000 : ms ? 15000 : mv ? 37800 : 0;
            switch (variant) {
            case 0: a.skip = static_cast<Skip>(1 + k % 12); break;
            case 1:
                a.n = 56; a.a = 7; a.b = 8;
                a.worst = d.kind == Kind::Min ? a.lo : a.hi;
                break;
            case 2:
                a.n = 0xFFFF; a.fails = 0xFFFF; a.a = 13; a.b = 13;   // the highest PDO position a list can have
                a.worst = kNever;
                break;
            default:
                a.n = 1; a.fails = 1; a.a = 12; a.b = 1;
                a.worst = mv ? -1200 : us ? 2000000 : ms ? 99999 : 65535;
                if (d.kind == Kind::Range) a.lo = -kOpenLimit;
                break;
            }
            e.agg_[k] = a;
        }
        e.release_unconfirmed_ = variant == 3;
    }
};
}  // namespace k2::pdspec

namespace {

// A 5 / 9 / 15 / 20 V, 3 A charger (no PPS) on a monitor-equipped K2 whose stub converter follows every Request. The transition
// recorder refuses (the stub has no ring): every timing comes from the sink's stamps and live polling.
void fixture() {
    g_stub = {};
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
    g_stub.tc_arm_result = transcap::ArmResult::Busy;
}

uint32_t g_now = 1000;
void run(Ui &ui, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) { g_now += 5; ui.tick(g_now); }
}
void press(Ui &ui, Button b, ButtonEvent e = ButtonEvent::Short) {
    g_now += 30;
    ui.on_button(b, e, g_now);
}

Ui &boot(std::unique_ptr<Ui> &h) {
    h = std::make_unique<Ui>();
    h->init(g_now);
    h->set_rotation(0, g_now);
    run(*h, 20);
    return *h;
}
PdSpecPage &open(Ui &ui) {
    ui.goto_page(uint8_t(Page::Tools), g_now);
    ui.push_menu(Menu::PdSpec, g_now + 10);
    run(ui, 120);
    return *ui.pd_spec_page();
}
std::string foot(Ui &ui) { return std::string(ui.pd_spec_page()->footer()); }
std::string ok_label(Ui &ui) { return ui.labels().b[int(Button::Ok)].shrt; }
std::string back_label(Ui &ui) { return ui.labels().b[int(Button::Back)].shrt; }

// Setup straight to Review with this family mask (the cursor parked on Done).
void to_review(Ui &ui, uint8_t mask) {
    auto &k = ui.context().keep.pdspec;
    k.mask = mask; k.sel = PdSpecPage::kDoneRow;
    run(ui, 120);
    press(ui, Button::Ok);
}

void test_setup() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecPage &p = open(ui);
    CHECK_EQ(int(ui.menu()), int(Menu::PdSpec));
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "6 of 6 picked");
    kit::RowSpec r;
    p.setup_row(0, true, r);
    CHECK_STR(std::string(r.text).c_str(), "Transitions");
    CHECK_EQ(r.state, 1);
    CHECK_EQ(r.note.empty(), false);
    CHECK_STR(ok_label(ui).c_str(), "Toggle");
    press(ui, Button::Ok);   // Transitions off
    run(ui, 120);
    CHECK_STR(foot(ui).c_str(), "5 of 6 picked");
    CHECK_EQ(int(ui.context().keep.pdspec.mask), 0x3E);
    // Done with nothing picked stays on Setup with a caution.
    ui.context().keep.pdspec.mask = 0;
    ui.context().keep.pdspec.sel = PdSpecPage::kDoneRow;
    run(ui, 120);
    CHECK_STR(ok_label(ui).c_str(), "Next");
    press(ui, Button::Ok);
    run(ui, 60);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Setup));
    CHECK_STR(foot(ui).c_str(), "Pick at least one family");
    // No run yet: no "Last result" row.
    p.setup_row(PdSpecPage::kLastRow, false, r);
    ui.context().keep.pdspec.sel = PdSpecPage::kLastRow;
    run(ui, 120);
    CHECK_EQ(int(ui.context().keep.pdspec.sel), int(PdSpecPage::kDoneRow));   // clamped to the last row there is
}

void test_review_refusals() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecPage &p = open(ui);
    to_review(ui, 0x3F);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Review));
    CHECK_STR(foot(ui).c_str(), "Keep the output empty");
    CHECK_STR(ok_label(ui).c_str(), "Start");
    press(ui, Button::Ok);
    CHECK_EQ(p.armed(), true);
    CHECK_STR(ok_label(ui).c_str(), "Confirm");
    // The source list goes before the second press: the start is refused and nothing runs.
    g_stub.caps.valid = false;
    press(ui, Button::Ok);
    run(ui, 60);
    CHECK_EQ(ui.pd_spec().running(), false);
    CHECK_STR(foot(ui).c_str(), "No PD source");
    CHECK_EQ(g_stub.requests, 0);
    // A K2 that runs on the charger under test is refused: the run would cut its own supply.
    g_stub.caps.valid = true;
    g_stub.self_powered = true;
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 60);
    CHECK_EQ(ui.pd_spec().running(), false);
    CHECK_STR(foot(ui).c_str(), "K2 runs on this charger");
    CHECK_EQ(int(ui.pd_spec().status(g_now).start), int(sc::StartResult::SelfPowered));
    g_stub.self_powered = false;
    // BACK returns to Setup.
    press(ui, Button::Back);
    run(ui, 60);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Setup));
}

void test_run_stop_results() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecPage &p = open(ui);
    to_review(ui, 1u << int(sc::Family::Transitions));
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 200);
    CHECK_EQ(ui.pd_spec().running(), true);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Running));
    CHECK_STR(back_label(ui).c_str(), "Stop");
    CHECK_STR(foot(ui).c_str(), "Running: keep output empty");
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_EQ(int(ui.busy_why()), int(BusyWhy::PdSpec));
    CHECK_EQ(g_stub.reread_take_overs > 0, true);
    CHECK_EQ(g_stub.last_confirmed, true);            // every Request goes through the sink's guards, confirmed
    // A step or two later BACK stops: the source goes back to 5 V and the run ends on Results.
    run(ui, 3000);
    press(ui, Button::Back);
    for (int i = 0; i < 100 && ui.pd_spec().running(); i++) run(ui, 200);
    run(ui, 200);
    CHECK_EQ(ui.pd_spec().running(), false);
    CHECK_EQ(g_stub.releases > 0, true);
    CHECK_EQ(g_stub.sink.pos, 1);
    CHECK_EQ(g_stub.pm.vbus_mv, 5000);
    CHECK_EQ(g_stub.ka_silence == pd::KaSilence::Off, true);   // nothing is left silenced
    CHECK_EQ(g_stub.run_owner, false);
    const sc::Engine *e = ui.pd_spec().engine();
    CHECK_EQ(e != nullptr, true);
    CHECK_EQ(int(e->stop()), int(sc::Stop::Cancelled));
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Results));
    CHECK_STR(foot(ui).c_str(), "Stopped");
    // The families left out read "not selected" in their cells.
    kit::ResultRowSpec r;
    p.result_row(int(sc::Check::HrSafe0V), r);
    CHECK_EQ(int(r.cells[0].verdict), int(kit::Verdict::Skip));
    CHECK_STR(std::string(r.cells[0].label).c_str(), "not selected");
    CHECK_EQ(r.section, true);
    // OK opens the focused check's Detail; BACK comes back to the same row.
    press(ui, Button::Plus);
    press(ui, Button::Ok);
    run(ui, 60);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Detail));
    StrBuf<20> v;
    uint16_t col = 0;
    p.detail_value(3, v, col);   // Count: a number either way
    CHECK_EQ(v.view().empty(), false);
    press(ui, Button::Back);
    run(ui, 60);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Results));
    CHECK_EQ(p.result_selected(), 1);
    // The status block the remote serves agrees with the engine.
    const PdSpecSession::Status s = ui.pd_spec().status(g_now);
    CHECK_EQ(int(s.state), int(PdSpecSession::State::Finished));
    CHECK_EQ(int(s.stop), int(sc::Stop::Cancelled));
    CHECK_EQ(int(s.families), 1);
    CHECK_EQ(s.passes + s.fails + s.skips, int(sc::kChecks));
    CHECK_EQ(s.blob_len, int(sc::blob_len(e->trans().size(), e->pps().size())));
    std::array<std::uint8_t, 4> magic{};
    CHECK_EQ(ui.pd_spec().result_read(0, magic), size_t(4));
    CHECK_EQ(magic[0], 'P'); CHECK_EQ(magic[3], '1');
    // Setup now offers the result again.
    press(ui, Button::Back);
    run(ui, 60);
    kit::RowSpec row;
    p.setup_row(PdSpecPage::kLastRow, false, row);
    CHECK_STR(std::string(row.text).c_str(), "Last result");
}

// Results and Detail in all four rotations with every skip reason and the extreme values: no text may be cut (the truncation
// recorder fails the test through test_report), and a skipped cell spells its reason.
void test_results_extremes() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecPage &p = open(ui);
    to_review(ui, 1u << int(sc::Family::Transitions));
    press(ui, Button::Ok);
    press(ui, Button::Ok);
    run(ui, 500);
    press(ui, Button::Back);
    for (int i = 0; i < 100 && ui.pd_spec().running(); i++) run(ui, 200);
    run(ui, 200);
    CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Results));
    const sc::Engine *e = ui.pd_spec().engine();
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    for (int variant = 0; variant < 4; ++variant) {
        sc::EngineProbe::lay(*e, variant);
        if (variant == 0) {
            for (int k = 0; k < int(sc::kChecks); ++k) {
                kit::ResultRowSpec r;
                p.result_row(k, r);
                CHECK_EQ(int(r.cells[0].verdict), int(kit::Verdict::Skip));
                CHECK_STR(std::string(r.cells[0].label).c_str(), sc::skip_name(static_cast<sc::Skip>(1 + k % 12)));
            }
        }
        for (uint8_t rot = 0; rot < 4; ++rot) {
            ui.set_rotation(rot, g_now);
            run(ui, 200);
            for (int k = 0; k < int(sc::kChecks); ++k) {
                press(ui, Button::Ok);
                run(ui, 60);
                CHECK_EQ(int(p.stage()), int(PdSpecPage::Stage::Detail));
                press(ui, Button::Back);
                run(ui, 60);
                press(ui, Button::Plus);
                run(ui, 60);
            }
            for (int k = 0; k < int(sc::kChecks); ++k) press(ui, Button::Minus);
            run(ui, 100);
        }
        if (variant == 3) CHECK_STR(foot(ui).c_str(), "Not back at 5V: unplug");
    }
    ui.set_rotation(0, g_now);
}

}  // namespace

int main() {
    test_setup();
    test_review_refusals();
    test_run_stop_results();
    test_results_extremes();
    return test_report("test_ui_pdspec_page");
}
