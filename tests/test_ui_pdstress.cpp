// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the PD Stress session (src/ui/ui_pdstress.cpp) and its hooks in the Ui: the refusals, the footer line, the
// journal at sequence boundaries, the boot restore verdicts, the cancel path and the port ownership it takes.
#include <cstring>
#include <string>

#include "test_util.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/pdstress/journal_hw.hpp"

using namespace k2;
namespace ps = pdstress;

// A 5 / 9 / 15 / 20 V, 3 A charger on a monitor-equipped K2 whose stub converter follows every Request (and answers PS_RDY at once).
static void fixture() {
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
}

static ps::Options ladder_once() {
    ps::Options o;
    o.mask = ps::seq_bit(ps::Seq::Ladder);
    o.cycles_pct = 10;   // one cycle
    return o;
}

static uint32_t g_now = 1000;
static void run(Ui& ui, uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 5) { g_now += 5; ui.tick(g_now); }
}
static Ui* boot(std::unique_ptr<Ui>& holder, bool cancel_resume = false) {
    holder = std::make_unique<Ui>();
    holder->init(g_now, cancel_resume);
    holder->set_rotation(0, g_now);
    run(*holder, 20);
    return holder.get();
}
static bool logged(const char* needle) { return g_stub.log_text.find(needle) != std::string::npos; }

// ---- refusals ----------------------------------------------------------------------------------------------------------------------

static void test_refuses_without_a_monitor() {
    fixture(); g_stub.pm_online = false;
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::NoMonitor));
    CHECK_EQ(ui.pd_stress().active(), false);
    CHECK_EQ(ui.pd_stress().footer() == nullptr, true);
    CHECK_EQ(ui.pd_stress().has_result(), false);
}

static void test_refuses_without_a_source_list() {
    fixture(); g_stub.caps.valid = false;
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::NoCaps));
}

static void test_refuses_inline() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    ui.context().inline_mode = true;   // the K2 sits between a source and a far sink: it must not talk
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Inline));
    CHECK_EQ(g_stub.requests, 0);
}

static void test_refuses_while_another_tool_owns_the_ports() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    ui.context().em_scanning = true;
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Busy));
    CHECK_STR(ui.foreign_busy(), "E-marker scan: wait");
    ui.context().em_scanning = false;
    CHECK_EQ(ui.foreign_busy() == nullptr, true);
    // A contract the K2 itself holds (what a run leaves behind) is not a foreign owner.
    g_stub.sink.held = true;
    CHECK_EQ(ui.foreign_busy() == nullptr, true);
}

static void test_refuses_a_second_start() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Running));
}

// ---- a run -------------------------------------------------------------------------------------------------------------------------

static void test_a_run_owns_the_ports_and_says_so_in_the_footer() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(ui.ports_busy(), false);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_STR(ui.busy_reason(), "PD Stress: stop it");
    CHECK_STR(ui.pd_stress().footer(), "PD Stress: seq 1 of 1");
    CHECK_EQ(g_stub.run_owner, false);   // set by the first poll
    run(ui, 50);
    CHECK_EQ(g_stub.run_owner, true);    // the idle wake must not act on the run's own gaps
    CHECK_EQ(logged("[STRESS] start"), true);
}

static void test_a_run_ends_by_itself_and_leaves_a_result() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    run(ui, 120000);
    CHECK_EQ(ui.pd_stress().running(), false);
    CHECK_EQ(ui.pd_stress().has_result(), true);
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Done));
    CHECK_EQ(ui.ports_busy(), false);
    CHECK_EQ(ui.pd_stress().footer() == nullptr, true);
    CHECK_EQ(logged("[STRESS] finish done"), true);
    // The blob reads from any offset and carries the finished flag.
    uint8_t head[8] = {};
    CHECK_EQ(ui.pd_stress().result_read(0, head) == 8, true);
    CHECK_EQ(head[0], ps::result::kVersion);
    CHECK_EQ(head[5] & 2, 2);   // finished
    CHECK_EQ(head[5] & 1, 0);   // not running
    // elapsed time is frozen once the run ended
    const uint32_t e1 = ui.pd_stress().elapsed_ms(g_now);
    const uint32_t e2 = ui.pd_stress().elapsed_ms(g_now + 5000);
    CHECK_EQ(e1, e2);
    CHECK_EQ(e1 > 0, true);
}

static void test_cancel_releases_and_ends_the_run() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    run(ui, 3000);
    CHECK_EQ(ui.pd_stress().running(), true);
    ui.pd_stress().cancel();
    CHECK_STR(ui.pd_stress().footer(), "PD Stress: stopping");
    run(ui, 6000);
    CHECK_EQ(ui.pd_stress().running(), false);
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Cancelled));
    CHECK_EQ(g_stub.releases >= 1, true);
    CHECK_EQ(ui.pd_stress().footer() == nullptr, true);
}

// ---- stop and the dead-man lease ---------------------------------------------------------------------------------------------------

// The remote's `stop` (and the lease expiry, and the shell `stop`) end a run through Ui::cancel_runs, exactly as they end a charger
// test; the lease is told a run owns the ports through Ui::run_busy, again like a charger test. The session block names the owner
// with the next free BusyWhy value, appended after twin-run (9): the numbers are on the wire.
static void test_stop_ends_a_run_like_a_charger_test() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(ui.run_busy(), false);
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    run(ui, 3000);
    CHECK_EQ(ui.run_busy(), true);
    CHECK_EQ(int(ui.busy_why()), 10);
    CHECK_EQ(int(ui.busy_why()), int(BusyWhy::PdStress));
    CHECK_EQ(int(BusyWhy::TwinRun), 9);   // appended, never renumbered
    CHECK_EQ(int(ui.session().busy), 10);
    CHECK_STR(ui.busy_reason(), "PD Stress: stop it");
    ui.cancel_runs();
    run(ui, 6000);
    CHECK_EQ(ui.pd_stress().running(), false);
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Cancelled));
    CHECK_EQ(g_stub.releases >= 1, true);
    CHECK_EQ(ui.run_busy(), false);
    CHECK_EQ(int(ui.busy_why()), int(BusyWhy::None));
}

static void test_stop_with_no_run_is_a_no_op() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    ui.cancel_runs();
    CHECK_EQ(ui.run_busy(), false);
    CHECK_EQ(ui.pd_stress().has_result(), false);
}

// ---- the journal --------------------------------------------------------------------------------------------------------------------

static ps::Options two_sequences() {
    ps::Options o;
    o.mask = ps::seq_bit(ps::Seq::Ladder) | ps::seq_bit(ps::Seq::Slam);
    o.cycles_pct = 10;
    return o;
}

static void test_a_host_powered_k2_writes_no_journal() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(two_sequences(), g_now)), int(ps::StartResult::Ok));
    run(ui, 120000);
    CHECK_EQ(g_stub.ps_writes, 0);
    CHECK_EQ(g_stub.ps_file_present, false);
}

static void test_a_self_supplied_k2_writes_the_journal_at_a_boundary_and_clears_it_at_the_end() {
    fixture(); g_stub.self_powered = true;
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(two_sequences(), g_now)), int(ps::StartResult::Ok));
    run(ui, 6000);
    CHECK_EQ(g_stub.ps_writes >= 1, true);
    CHECK_EQ(g_stub.ps_file_present, true);
    run(ui, 120000);
    CHECK_EQ(ui.pd_stress().running(), false);
    CHECK_EQ(g_stub.ps_file_present, false);   // a finished run never resumes
}

static void test_a_failing_store_turns_the_journal_off_and_the_run_goes_on() {
    fixture(); g_stub.self_powered = true; g_stub.ps_write_fails = true;
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(two_sequences(), g_now)), int(ps::StartResult::Ok));
    run(ui, 120000);
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Done));
    CHECK_EQ(logged("[STRESS] journal off"), true);
    CHECK_EQ(g_stub.ps_writes, 0);
}

static void test_the_journal_waits_for_a_quiet_link() {
    fixture(); g_stub.self_powered = true;
    g_stub.pd_chatter = true; g_stub.legacy_attach = true;   // pd_quiet_ms() = 0: the link is busy all the time
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(two_sequences(), g_now)), int(ps::StartResult::Ok));
    run(ui, 120000);
    CHECK_EQ(g_stub.ps_writes, 0);   // never quiet: the engine's bounded hold gave up each time, nothing was written
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Done));
}

// ---- the boot restore ---------------------------------------------------------------------------------------------------------------

// A run is cut (the K2 loses its supply): the journal stays. The next boot finds it.
static void interrupted_run(std::unique_ptr<Ui>& h) {
    fixture(); g_stub.self_powered = true;
    Ui& ui = *boot(h);
    CHECK_EQ(int(ui.pd_stress().start(two_sequences(), g_now)), int(ps::StartResult::Ok));
    run(ui, 6000);
    CHECK_EQ(g_stub.ps_file_present, true);
    h.reset();   // power gone
}

static void set_vbus(int32_t mv) { g_stub.pm.vbus_mv = g_stub.pm_fast_mv = mv; g_stub.pm_fast_set = true; }

static void test_a_reboot_resumes_the_run_once_vbus_is_steady() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    const auto file = g_stub.ps_file;
    g_stub.sink = {}; set_vbus(0);   // the dark gap of the charger coming back
    boot(h);
    Ui& ui = *h;
    CHECK_EQ(ui.pd_stress().restoring(), true);
    CHECK_EQ(ui.ports_busy(), true);
    CHECK_STR(ui.pd_stress().footer(), "PD Stress: resuming");
    run(ui, 2000);
    CHECK_EQ(ui.pd_stress().running(), false);   // no VBUS yet: still waiting
    set_vbus(5000);
    g_stub.caps.num_pdos = 4;   // the charger announced its list again (the Ui cleared it while VBUS was dark)
    g_stub.sink.state = pd::SinkState::Ready; g_stub.sink.result = pd::SinkResult::Ok; g_stub.sink.pos = 1; g_stub.sink.mv = 5000;
    g_stub.sink.ps_rdy_inferred = true;
    run(ui, 1500);
    CHECK_EQ(ui.pd_stress().restoring(), false);
    CHECK_EQ(ui.pd_stress().running(), true);
    CHECK_EQ(ui.pd_stress().engine()->reboots(), 1);
    CHECK_EQ(logged("[STRESS] resume at sequence"), true);
    run(ui, 120000);
    CHECK_EQ(int(ui.pd_stress().engine()->stop()), int(ps::Stop::Done));
    (void)file;
}

static void test_a_reboot_on_another_charger_drops_the_journal() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    g_stub.caps.pdos[3] = pd::decode_pdo((300u << 10) | 300);   // another list
    g_stub.sink = {}; set_vbus(5000);
    boot(h);
    Ui& ui = *h;
    CHECK_EQ(ui.pd_stress().restoring(), true);
    run(ui, 1500);
    CHECK_EQ(ui.pd_stress().active(), false);
    CHECK_EQ(g_stub.ps_file_present, false);
    CHECK_EQ(logged("[STRESS] resume dropped"), true);
}

static void test_the_charger_that_never_comes_back_ends_the_wait() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    g_stub.sink = {}; set_vbus(0);
    boot(h);
    Ui& ui = *h;
    run(ui, PdStressSession::kRestoreWaitMs - 2000);
    CHECK_EQ(ui.pd_stress().restoring(), true);
    run(ui, 4000);
    CHECK_EQ(ui.pd_stress().restoring(), false);
    CHECK_EQ(g_stub.ps_file_present, false);
}

static void test_back_held_at_power_up_drops_the_journal() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    boot(h, true);
    CHECK_EQ(h->pd_stress().active(), false);
    CHECK_EQ(g_stub.ps_file_present, false);
}

static void test_a_damaged_journal_is_ignored() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    g_stub.ps_file[10] ^= 0x55;
    boot(h);
    CHECK_EQ(h->pd_stress().active(), false);
}

static void test_cancel_during_the_wait_drops_the_resume() {
    std::unique_ptr<Ui> h; interrupted_run(h);
    g_stub.sink = {}; set_vbus(0);
    boot(h);
    CHECK_EQ(h->pd_stress().restoring(), true);
    h->pd_stress().cancel();
    CHECK_EQ(h->pd_stress().active(), false);
    CHECK_EQ(g_stub.ps_file_present, false);
}

// The status block the remote op and the shell serve: idle, a refused start's code, running, finished with the score.
static void test_the_status_block_follows_the_run() {
    fixture();
    std::unique_ptr<Ui> h; Ui& ui = *boot(h);
    auto st = ui.pd_stress().status(g_now);
    CHECK_EQ(int(st.state), int(PdStressSession::State::Idle));
    CHECK_EQ(int(st.blob_len), 0);
    g_stub.caps.num_pdos = 0; g_stub.caps.valid = false;
    CHECK_EQ(int(ui.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::NoCaps));
    CHECK_EQ(int(ui.pd_stress().status(g_now).start), int(ps::StartResult::NoCaps));
    fixture();
    boot(h);
    Ui& u2 = *h;
    CHECK_EQ(int(u2.pd_stress().start(ladder_once(), g_now)), int(ps::StartResult::Ok));
    CHECK_EQ(int(u2.pd_stress().status(g_now).start), int(ps::StartResult::Ok));
    run(u2, 3000);
    st = u2.pd_stress().status(g_now);
    CHECK_EQ(int(st.state), int(PdStressSession::State::Running));
    CHECK_EQ(int(st.seq_total) >= 1, 1);
    CHECK_EQ(int(st.seq_cur), int(ps::Seq::Ladder));
    CHECK_EQ(int(st.blob_len), int(ps::result::kTotalBytes));
    run(u2, 120000);
    st = u2.pd_stress().status(g_now);
    CHECK_EQ(int(st.state), int(PdStressSession::State::Finished));
    CHECK_EQ(int(st.stop), int(ps::Stop::Done));
    CHECK_EQ(int(st.transitions) > 0, 1);
    CHECK_EQ(st.score_cp, ps::score_cp(u2.pd_stress().engine()->total()));
    CHECK_EQ(st.elapsed_ms, u2.pd_stress().elapsed_ms(g_now));
}

int main() {
    test_the_status_block_follows_the_run();
    test_refuses_without_a_monitor();
    test_refuses_without_a_source_list();
    test_refuses_inline();
    test_refuses_while_another_tool_owns_the_ports();
    test_refuses_a_second_start();
    test_a_run_owns_the_ports_and_says_so_in_the_footer();
    test_a_run_ends_by_itself_and_leaves_a_result();
    test_cancel_releases_and_ends_the_run();
    test_stop_ends_a_run_like_a_charger_test();
    test_stop_with_no_run_is_a_no_op();
    test_a_host_powered_k2_writes_no_journal();
    test_a_self_supplied_k2_writes_the_journal_at_a_boundary_and_clears_it_at_the_end();
    test_a_failing_store_turns_the_journal_off_and_the_run_goes_on();
    test_the_journal_waits_for_a_quiet_link();
    test_a_reboot_resumes_the_run_once_vbus_is_steady();
    test_a_reboot_on_another_charger_drops_the_journal();
    test_the_charger_that_never_comes_back_ends_the_wait();
    test_back_held_at_power_up_drops_the_journal();
    test_a_damaged_journal_is_ignored();
    test_cancel_during_the_wait_drops_the_resume();
    return test_report("ui_pdstress");
}
