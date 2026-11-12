// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Flash writes during a charger test. A littlefs volume on a counting RAM disk carries the run journal, the Black Box ring
// log sits on its own sectors (a RAM flash), as on the device. A Baseus-FH22-shaped run is played row by row (verdicts into
// the journal, the phase marker before each PD row, the captures a PD row makes) and every erase and program is attributed
// to the store that made it and to the phase it fell in: during the run (from the arm to the finish) or after it. Every
// sector erase freezes the main loop for tens of ms on the device (USB, the 1 ms PD service and SysTick), so the number that
// matters is how many erases fall inside the run: the Black Box makes none there (a save never erases, its maintenance
// waits for the end of the run), and its writes must not grow with the run.
#include <cstdio>
#include <memory>
#include <vector>

#include "k2/chargertest/charger_journal.hpp"
#include "k2/core/bbpersist.hpp"
#include "k2/drivers/flash.hpp"
#include "k2/pd/pdtrace.hpp"
#include "ramdisk.hpp"
#include "ramflash.hpp"
#include "test_util.h"
#include "ui_stubs.h"

using namespace k2;
namespace cj = k2::charger_journal;
namespace ct = k2::charger_test;
using A = ct::Algorithm;
using G = ct::Group;

namespace {

ct::Step row(G g, A a, unsigned pos, unsigned mv, unsigned ma = 1000, bool hunt = false) {
    ct::Step s; s.group = g; s.algorithm = a; s.pos = std::uint8_t(pos); s.mv = std::uint16_t(mv); s.ma = std::uint16_t(ma);
    s.hunt = hunt; s.pdo = 0x1000u * pos + mv;
    return s;
}
// The row mix of a full PD charger run (the shape of the FH22 plan: legacy units first, then PD fixed / PPS / AVS, QC, UFCS).
std::vector<ct::Step> plan() {
    std::vector<ct::Step> v;
    for (unsigned i = 0; i < 3; ++i) v.push_back(row(G::Fcp, A::Fcp, 0, 5000 + 4000 * i, 0));
    for (unsigned i = 0; i < 2; ++i) v.push_back(row(G::Afc, A::Afc, 0, 9000 + 3000 * i, 0));
    v.push_back(row(G::Vooc, A::Vooc, 0, 5000));
    for (unsigned i = 0; i < 2; ++i) v.push_back(row(G::SuperVooc, A::SuperVooc, 0, 5000 + 5000 * i));
    const unsigned fixed[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Pd, A::Pd3Fixed, i + 1, fixed[i]));
    v.push_back(row(G::Pd, A::Pd3Fixed, 5, 20000, 6000, true));
    for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Pd, A::SprAvs, 7, 9000 + 3000 * i));
    const unsigned lv[] = {11000, 21000, 11000, 8000};
    for (unsigned i = 0; i < 4; ++i) v.push_back(row(G::Pd, A::Pps, 6, lv[i]));
    v.push_back(row(G::Pd, A::Pps, 6, 11000, 6000, true));
    v.push_back(row(G::Pd, A::Pps, 6, 20000, 6000, true));
    for (unsigned i = 0; i < 3; ++i) v.push_back(row(G::Qc2, A::Qc2, 0, 5000 + 3500 * i, 0));
    for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Qc3, A::Qc3, 0, 5000 + 700 * i, 0));
    for (unsigned i = 0; i < 6; ++i) v.push_back(row(G::Ufcs, A::Ufcs, 0, 5000 + 1000 * i, 0));
    return v;
}
ct::Evidence evidence() {
    ct::Evidence e; e.selected = 0x1234; e.session = 77; e.monitor_mv = 36000; e.cable_mv = 50000; e.cable_ma = 5000;
    e.raw_header = 0x7181; e.wire_rev = 2; e.flags = 0x23; e.num_pdos = 7;
    return e;
}
const ct::Options kOpt{.selected = ct::kAllAlgorithms};
constexpr std::uint32_t kRun = 9;

struct Counts {
    std::uint64_t erases = 0, progs = 0, halfwords = 0;
    double ms = 0;
    void add(const Counts& o) { erases += o.erases; progs += o.progs; halfwords += o.halfwords; ms += o.ms; }
};

struct Sim {
    k2test::RamDisk disk{111};
    fs::Fs fs{disk, 111};
    k2test::RamFlash bb_flash;
    ringlog::Ring bb_ring{bb_flash, flash::kBbRingBase, flash::kBbRingSectors, bbstore::kRingKind};
    std::unique_ptr<cj::Journal> j;
    std::unique_ptr<bbpersist::Persister> bb;
    std::vector<ct::Step> steps = plan();
    Counts journal_run, bb_run, bb_after;
    Counts j_arm, j_phase, j_rows, j_finish;   // journal_run split by what wrote it
    bool running = false;
    std::uint32_t max_per_poll = 0;   // the most captures one poll() wrote

    Sim() {
        bool fm = false;
        fs.mount_or_format(fm);
        bb_ring.scan();
        j = std::make_unique<cj::Journal>(fs);
        bb = std::make_unique<bbpersist::Persister>(bb_ring);
        pdtrace::init();
        pdtrace::clear_captures();
        bb->init();
        g_stub.ms = 1000000;
    }
    // What f() made on the journal's volume.
    template <class F> Counts spend(F&& f) {
        disk.reset_counters();
        f();
        return {disk.erases, disk.progs, disk.halfwords, disk.cost_ms()};
    }
    // What f() made on the Black Box ring (the same cost model as RamDisk::cost_ms()).
    template <class F> Counts spend_bb(F&& f) {
        const Counts c0{bb_flash.erases, bb_flash.progs, bb_flash.halfwords, 0};
        f();
        Counts c{bb_flash.erases - c0.erases, bb_flash.progs - c0.progs, bb_flash.halfwords - c0.halfwords, 0};
        c.ms = 0.2 * double(c.halfwords) + 35.0 * double(c.erases);
        return c;
    }
    void tick(unsigned ms = 50) { g_stub.ms += ms; pdtrace::vbus_sample(5000, 0); }
    void traffic() { pdtrace::rx(0xE0, 0x0001, std::vector<std::uint8_t>(4, 0)); }
    // The UI's tick: renew the hold, one Black Box poll, and one journal write when the link has been quiet. The main
    // loop's maintenance runs too, allowed only outside the run (the firmware's gate: no charger test, a quiet link).
    void service(bool journal_too) {
        if (running) bb->hold_run();
        const std::uint32_t before = bb->status().saves;
        const Counts b = spend_bb([&] { bb->poll(); bb->maintain(!running); });
        (running ? bb_run : bb_after).add(b);
        const std::uint32_t made = bb->status().saves - before;
        if (made > max_per_poll) max_per_poll = made;
        if (journal_too && j->pending() && pdtrace::since_activity_ms() >= 80) {   // the UI's kJrQuietMs
            const Counts c = spend([&] { j->pump(kRun, kOpt, evidence(), steps); });
            journal_run.add(c);
            j_rows.add(c);
        }
    }
    // What one row makes on the wire: a PD row a contract (a capture of its own, held in a run), a failing row a hard reset
    // (a rare kind: it is saved at once unless the run defers it), a hunt row a refusal, a detach before each legacy unit.
    void row_events(std::size_t i) {
        const ct::Step& s = steps[i];
        if (s.group == G::Pd) {
            pdtrace::sink(0, 0, true, false, std::uint8_t(1 + i % 7));
            traffic();
        }
        if (i % 4 == 3) { pdtrace::reset(pdtrace::ResetKind::HardRx); traffic(); }
        if (s.hunt) { pdtrace::refusal(1, 2); traffic(); }
        if (i == 0 || steps[i - 1].group != s.group) { pdtrace::cc(pdtrace::CcEvent::Detach, 1, 0); traffic(); }
    }
    void play() {
        bb->make_room();                                   // START, before Rd goes on: the run's rare captures find room
        const Counts a = spend([&] { CHECK_EQ(int(j->begin(kRun, kOpt, evidence(), steps)), int(cj::Err::Ok)); });
        journal_run.add(a);
        j_arm = a;
        running = true;
        for (std::size_t i = 0; i < steps.size(); ++i) {
            if (steps[i].group == G::Pd) {
                const Counts c = spend([&] { CHECK_EQ(int(j->phase(ct::Phase::Request, steps, i)), int(cj::Err::Ok)); });
                journal_run.add(c);
                j_phase.add(c);
            }
            row_events(i);
            for (int t = 0; t < 16; ++t) { tick(); traffic(); service(false); }   // the row: link busy for 800 ms
            steps[i].verdict = i % 4 == 3 ? ct::Verdict::Fail : ct::Verdict::Pass;
            steps[i].measured_mv = std::uint16_t(steps[i].mv - 40);
            j->note(steps, i);
            for (int t = 0; t < 30; ++t) { tick(); service(true); }               // the gap behind it: quiet link
        }
        const Counts f = spend([&] { j->finish(2, 0); });
        journal_run.add(f);
        j_finish = f;
        running = false;
    }
};

}  // namespace

int main() {
    Sim s;
    s.play();
    const std::size_t rows = s.steps.size();
    // The run is over; the flush is the idle loop's: one save per poll, the held captures and everything deferred.
    unsigned polls = 0;
    for (int t = 0; t < 400; ++t) { s.tick(); s.service(false); ++polls; }
    const auto st = s.bb->status();
    std::printf("rows=%zu  journal: erases=%llu progs=%llu hw=%llu cost=%.0f ms\n", rows, (unsigned long long)s.journal_run.erases,
                (unsigned long long)s.journal_run.progs, (unsigned long long)s.journal_run.halfwords, s.journal_run.ms);
    const auto line = [](const char* n, const Counts& c) {
        std::printf("  journal %-8s erases=%llu progs=%llu hw=%llu cost=%.0f ms\n", n, (unsigned long long)c.erases,
                    (unsigned long long)c.progs, (unsigned long long)c.halfwords, c.ms);
    };
    line("arm", s.j_arm); line("phase", s.j_phase); line("rows", s.j_rows); line("finish", s.j_finish);
    std::printf("black box during the run: erases=%llu progs=%llu hw=%llu cost=%.0f ms\n", (unsigned long long)s.bb_run.erases,
                (unsigned long long)s.bb_run.progs, (unsigned long long)s.bb_run.halfwords, s.bb_run.ms);
    std::printf("black box after the run:  erases=%llu progs=%llu hw=%llu cost=%.0f ms (saves=%u skipped=%u entries=%d polls=%u dropped=%u deferred=%u)\n",
                (unsigned long long)s.bb_after.erases, (unsigned long long)s.bb_after.progs, (unsigned long long)s.bb_after.halfwords,
                s.bb_after.ms, unsigned(st.saves), unsigned(st.skipped), st.entries, polls, unsigned(st.dropped), unsigned(st.deferred));
    // The Black Box writes nothing that a run has to wait for beyond a small fixed number of captures (the rare kinds that
    // the run lets through), however many rows fail, and erases nothing during the run: the rest is saved after the run.
    CHECK_EQ(s.bb_run.erases, 0u);
    CHECK_EQ(s.bb_run.halfwords <= 2 * 700, true);
    CHECK_EQ(st.saves >= 1, true);   // and what is left after the run does reach the flash
    CHECK_EQ(s.bb_after.progs > 0, true);
    CHECK_EQ(s.max_per_poll, 1u);    // one write per idle pass
    // The captures that did not fit the three RAM slots are counted, not silent: the run made far more than three.
    CHECK_EQ(st.deferred > 0, true);
    CHECK_EQ(st.dropped > 0, true);
    CHECK_EQ(st.waiting, 0u);        // nothing is left waiting once the run is over
    return test_report("test_run_writes");
}
