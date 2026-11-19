// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec session (src/ui/ui_pdspec.cpp) through the real Ui and the real engine over the stub converter (tests/ui_stubs.cpp),
// in its two uses: started INSIDE the charger test's Deep run (start_in_test: the test owns the ports, the Rd and the run flag, no
// footer) and started alone by the remote (start: refusals, its own Rd, the status block, the result blob). The engine's own judging
// is tested in tests/test_pdspec_engine.cpp, the stage around it in tests/test_charger_test.cpp.
#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

#include "k2/chargertest/spec_section.hpp"
#include "k2/ui/ui.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;
namespace sc = pdspec;

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
Ui &boot(std::unique_ptr<Ui> &h) {
    h = std::make_unique<Ui>();
    h->init(g_now);
    h->set_rotation(0, g_now);
    run(*h, 20);
    return *h;
}
// Runs until the engine has finished (bounded) and the stub converter is back at 5 V.
void finish(Ui &ui) {
    for (int i = 0; i < 200 && ui.pd_spec().running(); i++) run(ui, 200);
    run(ui, 200);
}

// Inside the charger test: no foreign-busy check, nothing taken, nothing shown; the engine is readable for the stage afterwards.
void test_start_in_test() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecSession &s = ui.pd_spec();
    CHECK_EQ(s.engine_for_test() == nullptr, true);   // nothing yet
    g_stub.self_powered = true;                       // a K2 fed by the charger under test would cut its own supply
    CHECK_EQ(int(s.start_in_test(g_now)), int(sc::StartResult::SelfPowered));
    g_stub.self_powered = false;
    CHECK_EQ(s.running(), false);
    CHECK_EQ(int(s.start_in_test(g_now)), int(sc::StartResult::Ok));
    run(ui, 200);
    CHECK_EQ(s.running(), true);
    CHECK_EQ(s.engine_for_test() != nullptr, true);
    CHECK_EQ(s.footer() == nullptr, true);            // the charger test's own footer carries the stage
    CHECK_EQ(g_stub.rd_run, false);                   // the test owns the Rd: the session takes none
    CHECK_EQ(g_stub.reread_take_overs, 0);
    CHECK_EQ(g_stub.last_confirmed, true);            // every Request goes through the sink's guards, confirmed
    CHECK_EQ(int(s.start_in_test(g_now)), int(sc::StartResult::Running));   // one run at a time
    // The stage stops it: the source goes back to 5 V and the run ends with its result in place.
    run(ui, 3000);
    s.cancel();
    finish(ui);
    CHECK_EQ(s.running(), false);
    CHECK_EQ(g_stub.releases > 0, true);
    CHECK_EQ(g_stub.sink.pos, 1);
    CHECK_EQ(g_stub.pm.vbus_mv, 5000);
    CHECK_EQ(g_stub.ka_silence == pd::KaSilence::Off, true);   // nothing is left silenced
    CHECK_EQ(g_stub.rd_run, false);
    const sc::Engine *e = s.engine_for_test();
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(int(e->stop()), int(sc::Stop::Cancelled));
    // The section the stage builds from it: every check has a cell, none is left out.
    const specsec::Result r = specsec::from_aggs(e->agg(), specsec::End::Cancelled,
                                                 static_cast<std::uint8_t>(e->stop()), e->release_unconfirmed());
    CHECK_EQ(unsigned(r.end), unsigned(specsec::End::Cancelled));
    CHECK_EQ(r.cells.size(), specsec::kCells);
}

// Started alone, as the remote does: its refusals, its own Rd, the status block and the result blob.
void test_start_alone() {
    fixture();
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecSession &s = ui.pd_spec();
    g_stub.caps.valid = false;
    CHECK_EQ(int(s.start(1u << int(sc::Family::Transitions), g_now)), int(sc::StartResult::NoCaps));
    CHECK_EQ(s.running(), false);
    CHECK_EQ(g_stub.requests, 0);
    g_stub.caps.valid = true;
    g_stub.self_powered = true;
    CHECK_EQ(int(s.start(1u << int(sc::Family::Transitions), g_now)), int(sc::StartResult::SelfPowered));
    CHECK_EQ(int(s.status(g_now).start), int(sc::StartResult::SelfPowered));
    g_stub.self_powered = false;
    CHECK_EQ(int(s.start(1u << int(sc::Family::Transitions), g_now)), int(sc::StartResult::Ok));
    run(ui, 200);
    CHECK_EQ(s.running(), true);
    CHECK_EQ(s.engine_for_test() == nullptr, true);   // a standalone run is not the charger test's
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_EQ(int(ui.busy_why()), int(BusyWhy::PdSpec));
    CHECK_EQ(g_stub.reread_take_overs > 0, true);
    // The run presents its own Rd for its whole length (a Hard Reset's lost contract took the Rd along and the source never came
    // back, hardware 2026-11-14) and owns the line.
    CHECK_EQ(g_stub.rd_run, true);
    CHECK_EQ(g_stub.run_owner, true);
    CHECK_EQ(s.footer() != nullptr, true);
    run(ui, 3000);
    s.cancel();
    bool saw_stopping = false;
    for (int i = 0; i < 40 && s.running(); i++) {
        run(ui, 1);
        if (s.running() && s.footer() && std::string_view(s.footer()) == "PD Spec: stopping") saw_stopping = true;
    }
    CHECK_EQ(saw_stopping, true);   // the footer says "stopping", not the PPS/AVS progress text
    finish(ui);
    CHECK_EQ(s.running(), false);
    CHECK_EQ(g_stub.releases > 0, true);
    CHECK_EQ(g_stub.sink.pos, 1);
    CHECK_EQ(g_stub.pm.vbus_mv, 5000);
    CHECK_EQ(g_stub.ka_silence == pd::KaSilence::Off, true);
    CHECK_EQ(g_stub.run_owner, false);
    CHECK_EQ(g_stub.rd_run, false);   // and it gives them back
    const sc::Engine *e = s.engine();
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    CHECK_EQ(int(e->stop()), int(sc::Stop::Cancelled));
    // The status block the remote serves agrees with the engine.
    const PdSpecSession::Status st = s.status(g_now);
    CHECK_EQ(int(st.state), int(PdSpecSession::State::Finished));
    CHECK_EQ(int(st.stop), int(sc::Stop::Cancelled));
    CHECK_EQ(int(st.families), 1);
    CHECK_EQ(st.passes + st.fails + st.skips, int(sc::kChecks));
    CHECK_EQ(st.blob_len, int(sc::blob_len(e->trans().size(), e->passes().size())));
    std::array<std::uint8_t, 4> magic{};
    CHECK_EQ(s.result_read(0, magic), size_t(4));
    CHECK_EQ(magic[0], 'P'); CHECK_EQ(magic[3], '2');
}

// The recorder is not free when a step wants it. A frozen capture nobody waits for (the charger test never kept it) is taken over; a kept
// one waiting for its write, or a recording another tool runs (a manual arm / a capture in its post window), is left alone and the
// engine times from the stamps. Inside the charger test an Armed leftover of an earlier row is the run's own and is dropped.
struct Held { transcap::State st; bool keep; };
int arms_with_recorder_held(Held h, bool in_test) {
    fixture();
    g_stub.tc_arm_result = transcap::ArmResult::Ok;
    g_stub.tc_state = h.st; g_stub.tc_keep_pending = h.keep;
    std::unique_ptr<Ui> hd; Ui &ui = boot(hd);
    PdSpecSession &s = ui.pd_spec();
    const auto r = in_test ? s.start_in_test(g_now) : s.start(1u << int(sc::Family::Transitions), g_now);
    CHECK_EQ(int(r), int(sc::StartResult::Ok));
    run(ui, 3000);
    s.cancel();
    finish(ui);
    return g_stub.tc_arms;
}
void test_capture_takes_over_only_what_nobody_waits_for() {
    using transcap::State;
    CHECK_EQ(arms_with_recorder_held({State::Off, false}, false) >= 1, true);      // free: the usual path
    CHECK_EQ(arms_with_recorder_held({State::Frozen, false}, false) >= 1, true);   // finished, never kept (hardware 2026-11-17)
    CHECK_EQ(arms_with_recorder_held({State::Frozen, true}, false), 0);            // waiting for its flash write
    CHECK_EQ(arms_with_recorder_held({State::Armed, false}, false), 0);            // someone else is recording
    CHECK_EQ(arms_with_recorder_held({State::Post, false}, false), 0);
    CHECK_EQ(arms_with_recorder_held({State::Armed, false}, true) >= 1, true);     // the test's own earlier row left it armed
    CHECK_EQ(arms_with_recorder_held({State::Frozen, true}, true), 0);             // a kept failure still waits, in the test too
    CHECK_EQ(arms_with_recorder_held({State::Post, false}, true), 0);
}


// A capture of one programmable step, from_mv -> to_mv at 1 kHz: the Request at 20 ms, the Accept at 25 ms, a 5 ms ramp, PS_RDY at
// 40 ms, then the new level; frozen on a 1 us post window as the session's own freeze does.
void build_step(transcap::Rec &r, uint16_t from_mv, uint16_t to_mv) {
    r.clear();
    r.arm();
    uint32_t t = 1000;
    for (int i = 0; i < 300; i++, t += 1000) {
        const int k = int(t / 1000) - 26;   // the ramp's samples after the Accept
        const int v = k < 0 ? from_mv : k >= 5 ? to_mv : from_mv + (to_mv - from_mv) * (k + 1) / 5;
        r.push({t, uint16_t(v), 0, 0});
        if (t == 20000) r.mark(t + 100, transcap::Mark::TxRequest);
        if (t == 25000) r.mark(t + 100, transcap::Mark::Accept);
        if (t == 40000) r.mark(t + 100, transcap::Mark::PsRdy);
    }
    r.trigger(t, transcap::Reason::Manual, 1);
    r.push({t, to_mv, 0, 0});
}

// prog_unify: a Deep run's PPS moves, handed over before its stage, are judged into the stage's engine on the session's next poll and
// the recorder is released for the next Request; an APDO whose sweep was handed over whole runs only its slams in the stage.
void test_moves_fed_before_the_stage() {
    fixture();
    g_stub.caps.num_pdos = 5;
    g_stub.caps.pdos[4] = pd::decode_pdo(0xC0000000u | (110u << 17) | (33u << 8) | 60);   // PPS 3.3-11 V 3 A
    std::unique_ptr<Ui> h; Ui &ui = boot(h);
    PdSpecSession &s = ui.pd_spec();
    static transcap::Rec rec;
    build_step(rec, 5000, 5100);
    g_stub.tc_rec = &rec; g_stub.tc_freeze_on_trigger = true;
    g_stub.tc_state = transcap::State::Armed;
    CHECK_EQ(s.test_move(7, {5, false, 1, 5000, 5100}, sc::kMoveSweep, g_now), true);
    CHECK_EQ(s.feeding(), true);
    run(ui, 10);
    CHECK_EQ(s.feeding(), false);
    CHECK_EQ(int(g_stub.tc_state), int(transcap::State::Off));   // released: the next Request arms a fresh recording
    CHECK_EQ(s.engine() != nullptr, true);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsBand).n), 1);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsAccuracy).fails), 0);
    CHECK_EQ(int(s.status(g_now).state), int(PdSpecSession::State::Idle));   // no stage yet: nothing for the remote
    CHECK_EQ(int(s.blob_len()), 0);
    // The last point of the sweep, chained; then a range row's own capture (frozen already: left for the row).
    g_stub.tc_state = transcap::State::Armed;
    CHECK_EQ(s.test_move(7, {5, false, 1, 5000, 5100}, sc::kMoveSweep | sc::kMoveChain | sc::kMoveLast, g_now), true);
    run(ui, 10);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsMonotonic).n), 1);
    g_stub.tc_state = transcap::State::Frozen;
    CHECK_EQ(s.test_move(7, {5, false, sc::kSlam, 5000, 5100}, 0, g_now), true);
    run(ui, 10);
    CHECK_EQ(s.feeding(), false);
    CHECK_EQ(int(g_stub.tc_state), int(transcap::State::Frozen));
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsBand).n), 3);
    // Not recording: nothing to take, no wait.
    g_stub.tc_state = transcap::State::Off;
    CHECK_EQ(s.test_move(7, {5, false, 1, 5100, 5200}, sc::kMoveSweep, g_now), false);
    CHECK_EQ(s.feeding(), false);
    // The stage of the same run keeps the moves and the APDO the sweep covered (its slams only); another run's starts from nothing.
    g_stub.tc_rec = nullptr; g_stub.tc_freeze_on_trigger = false;
    CHECK_EQ(int(s.start_in_test(g_now, 7)), int(sc::StartResult::Ok));
    CHECK_EQ(int(s.engine()->covered()), 1 << 5);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsBand).n), 3);
    s.cancel();
    finish(ui);
    g_stub.tc_state = transcap::State::Armed; g_stub.tc_rec = &rec; g_stub.tc_freeze_on_trigger = true;
    CHECK_EQ(s.test_move(8, {5, false, 1, 5000, 5100}, sc::kMoveSweep | sc::kMoveLast, g_now), true);
    run(ui, 10);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsBand).n), 1);   // a fresh engine for the new run
    g_stub.tc_rec = nullptr; g_stub.tc_freeze_on_trigger = false; g_stub.tc_state = transcap::State::Off;
    CHECK_EQ(int(s.start_in_test(g_now, 9)), int(sc::StartResult::Ok));
    CHECK_EQ(int(s.engine()->covered()), 0);
    CHECK_EQ(int(s.engine()->agg(sc::Check::PpsBand).n), 0);
    s.cancel();
    finish(ui);
}

}  // namespace

int main() {
    test_start_in_test();
    test_start_alone();
    test_capture_takes_over_only_what_nobody_waits_for();
    test_moves_fed_before_the_stage();
    return test_report("test_ui_pdspec_session");
}
