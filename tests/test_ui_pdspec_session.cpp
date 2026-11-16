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

}  // namespace

int main() {
    test_start_in_test();
    test_start_alone();
    return test_report("test_ui_pdspec_session");
}
