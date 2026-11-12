// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger journal (k2/chargertest/charger_journal.hpp) on littlefs: the plan in five per-section files, a section rewrite on its
// own, power cut at every operation, reboot, run restart, resume gate.
#include "test_util.h"
#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "k2/chargertest/charger_journal.hpp"
#include "k2/core/bbstore.hpp"
#include "ramdisk.hpp"

using namespace k2;
namespace cj = k2::charger_journal;
namespace ct = k2::charger_test;
using A = ct::Algorithm;
using G = ct::Group;
using S = cj::Section;

static constexpr uint32_t kRun = 5;

static std::vector<ct::Step> plan(size_t n) {
    std::vector<ct::Step> v(n);
    for (size_t i = 0; i < n; ++i) {
        v[i].group = i % 2 ? ct::Group::Qc3 : ct::Group::Pd;
        v[i].algorithm = i % 2 ? A::Qc3 : A::Pps;
        v[i].pos = uint8_t(i + 1); v[i].mv = uint16_t(5000 + 100 * i); v[i].ma = 1000; v[i].pdo = 0x12345678u + i;
        v[i].continuous = i % 3 == 1; v[i].avs = i % 4 == 2;
    }
    return v;
}
static ct::Step row(G g, A a, unsigned pos, unsigned mv, unsigned ma = 1000, bool cont = false, bool hunt = false, bool avs = false) {
    ct::Step s; s.group = g; s.algorithm = a; s.pos = uint8_t(pos); s.mv = uint16_t(mv); s.ma = uint16_t(ma);
    s.continuous = cont; s.hunt = hunt; s.avs = avs; s.pdo = 0x1000u * pos + mv;
    return s;
}
// A plan shaped like the Baseus FH22 run, in the order the planner lays it out with the default unit order: Fcp, Afc, Vooc,
// SuperVooc, then the PD rows (a fixed 20 V hunt, the SPR AVS APDO when the late list brought it, one PPS APDO with its levels,
// the 11 V / 20 V hunts and the full-current row), Qc2, Qc3, UFCS, and the low PPS level of PdLow last.
static std::vector<ct::Step> fh22(bool avs) {
    std::vector<ct::Step> v;
    for (unsigned i = 0; i < 3; ++i) v.push_back(row(G::Fcp, A::Fcp, 0, 5000 + 4000 * i, 0));
    for (unsigned i = 0; i < 2; ++i) v.push_back(row(G::Afc, A::Afc, 0, 9000 + 3000 * i, 0, i != 0));
    v.push_back(row(G::Vooc, A::Vooc, 0, 5000));
    v.push_back(row(G::SuperVooc, A::SuperVooc, 0, 5000)); v.push_back(row(G::SuperVooc, A::SuperVooc, 0, 10000));
    const unsigned fixed[] = {5000, 9000, 12000, 15000, 20000};
    for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Pd, A::Pd3Fixed, i + 1, fixed[i]));
    v.push_back(row(G::Pd, A::Pd3Fixed, 5, 20000, 6000, false, true));            // the fixed 20 V 6 A hunt
    if (avs)
        for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Pd, A::SprAvs, 7, 9000 + 3000 * i, 1000, i != 0, false, true));
    const unsigned lv[] = {11000, 21000, 11000, 8000};
    for (unsigned i = 0; i < 4; ++i) v.push_back(row(G::Pd, A::Pps, 6, lv[i], 1000, i != 0));
    v.push_back(row(G::Pd, A::Pps, 6, 11000, 6000, false, true));                 // PPS 11 V 6 A hunt (Turbo 6A)
    v.push_back(row(G::Pd, A::Pps, 6, 20000, 6000, false, true));                 // PPS 20 V 6 A hunt
    v.push_back(row(G::Pd, A::Pps, 6, 21000, 5000));                              // the full-current row
    for (unsigned i = 0; i < 3; ++i) v.push_back(row(G::Qc2, A::Qc2, 0, 5000 + 3500 * i, 0, i != 0));
    for (unsigned i = 0; i < 5; ++i) v.push_back(row(G::Qc3, A::Qc3, 0, 5000 + 700 * i, 0, i != 0));
    const unsigned uf[] = {5000, 9000, 11000, 5000, 12000, 20000};
    for (unsigned i = 0; i < 6; ++i) v.push_back(row(G::Ufcs, A::Ufcs, 0, uf[i], 0, i != 0));
    v.push_back(row(G::Pd, A::Pps, 6, 3300, 1000, true));                         // PdLow: the low edge of the APDO, last of all
    return v;
}
// The index of the first non-hunt row of an algorithm.
static size_t first_of(const std::vector<ct::Step>& v, A a) {
    for (size_t n = 0; n < v.size(); ++n) if (v[n].algorithm == a && !v[n].hunt) return n;
    return v.size();
}
static std::vector<ct::Step> without_hunts(std::vector<ct::Step> v) {
    v.erase(std::remove_if(v.begin(), v.end(), [](const ct::Step& s) { return s.hunt; }), v.end());
    return v;
}
static bool same_row(const ct::Step& a, const ct::Step& b) {
    return a.group == b.group && a.algorithm == b.algorithm && a.pos == b.pos && a.verdict == b.verdict && a.mv == b.mv && a.ma == b.ma &&
           a.measured_mv == b.measured_mv && a.reason == b.reason && a.detail == b.detail && a.continuous == b.continuous &&
           a.avs == b.avs && a.hunt == b.hunt && a.tries == b.tries && a.pdo == b.pdo;
}
// The rows come back in the order the journal replayed them: the plan's own order, exactly as it was armed (the header's
// order is written from the planner's step list, so a resumed run executes in the order it was planned in).
static bool rows_are(const cj::Replay& r, const std::vector<ct::Step>& plan_rows) {
    if (r.count != plan_rows.size()) return false;
    for (size_t n = 0; n < plan_rows.size(); ++n) if (!same_row(r.steps[n], plan_rows[n])) return false;
    return true;
}
static ct::Evidence evidence() {
    ct::Evidence e; e.selected = 0x1234; e.session = 77; e.monitor_mv = 36000; e.cable_mv = 50000; e.cable_ma = 5000;
    e.raw_header = 0x7181; e.wire_rev = 2; e.flags = 0x23; e.num_pdos = 7; e.rebalanced = 3;   // the re-plan count survives a comeback
    e.reported = {}; e.reported.valid = true; e.reported.rev_major = 3; e.reported.rev_minor = 1;
    e.reported.ver_major = 1; e.reported.ver_minor = 2;
    return e;
}
static const ct::Options kOpt{.selected = ct::kAllAlgorithms};   // every unit takes part: the replay's unit order is the default one

// One volume on a RAM disk; reboot() remounts it with a fresh journal (no RAM state survives).
struct Vol {
    k2test::RamDisk disk{96};
    std::unique_ptr<fs::Fs> fs;
    std::unique_ptr<cj::Journal> j;
    Vol() { reboot(); }
    void reboot() {
        j.reset();
        disk.revive();
        fs = std::make_unique<fs::Fs>(disk, 96);
        bool fm = false;
        fs->mount_or_format(fm);
        j = std::make_unique<cj::Journal>(*fs);
    }
    // The journal writes what is dirty, one file per pump, until nothing is left.
    cj::Err settle(std::span<const ct::Step> steps, std::uint32_t run = kRun) { return j->settle(run, kOpt, evidence(), steps); }
    // A row of the plan changed (a verdict): note it and write it.
    cj::Err commit(std::vector<ct::Step>& steps, size_t idx) { j->note(steps, idx); return settle(steps); }
};
static void arm(Vol& v, const std::vector<ct::Step>& steps, std::uint32_t run = kRun) {
    CHECK_EQ(int(v.j->begin(run, kOpt, evidence(), steps)), int(cj::Err::Ok));
}

// A fresh plan made by the real planner (the same step list the grid reads since 98e2f39): a Xiaomi-style source list with two
// PPS APDOs, so the plan carries the two Mi 6 A hunt rows (11 V 6 A on the first APDO, 20 V 6 A on the 5-21 V one).
struct PlanEnv final : ct::Env {
    pd::Err request_pd(pd::Choice) override { return pd::Err::NoCaps; }
    fc::Fail request_fc(fc::Proto, int, bool, int) override { return fc::Fail::None; }
    void release(bool) override {}
    void record(const ct::Step&) override {}
};
static std::vector<ct::Step> planner_plan() {
    pd::SourceCaps c;
    c.valid = true; c.spec_rev = 2; c.active_cc = 1; c.raw_header = 0x7181;
    const unsigned fv[] = {5000, 9000, 12000, 15000, 20000};
    size_t n = 0;
    for (unsigned f : fv) c.pdos[n++] = pd::decode_pdo(((f / 50) << 10) | 325);
    c.pdos[n++] = pd::decode_pdo(0xc0000000u | (110u << 17) | (50u << 8) | 64u);   // APDO pos 6: 2.5-11 V, 3.2 A
    c.pdos[n++] = pd::decode_pdo(0xc0000000u | (210u << 17) | (50u << 8) | 64u);   // APDO pos 7: 2.5-21 V, 3.2 A
    c.num_pdos = uint8_t(n);
    PlanEnv env;
    ct::Input in;
    in.vbus_mv = 5000; in.cap_mv = 36000; in.cable_mv = 50000; in.cable_ma = 3000;
    ct::Runner r(env);
    CHECK_EQ(int(r.start(c, kOpt, in, true)), int(ct::Reason::None));
    return std::vector<ct::Step>(r.steps().begin(), r.steps().end());
}
static size_t hunt_at(const std::vector<ct::Step>& v, unsigned mv) {
    for (size_t n = 0; n < v.size(); ++n) if (v[n].hunt && v[n].mv == mv) return n;
    return v.size();
}

// The files of the plan as they lie on flash: a test names what a change rewrote by comparing two snapshots.
using Snap = std::map<std::string, std::vector<uint8_t>>;
static Snap snap(Vol& v) {
    Snap m;
    std::array<uint8_t, 2048> b{};
    for (const char* n : {cj::kHeaderPath, cj::section_path(S::Pd), cj::section_path(S::Pps), cj::section_path(S::Qc),
                          cj::section_path(S::Oem), cj::section_path(S::Ext)}) {
        std::size_t k = 0;
        if (v.fs->read_file(n, b, k) == fs::Err::Ok) m[n].assign(b.begin(), b.begin() + long(k));
    }
    return m;
}
static std::string changed(const Snap& a, const Snap& b) {
    std::string out;
    for (const auto& [name, data] : b) {
        const auto it = a.find(name);
        if (it == a.end() || it->second != data) out += (out.empty() ? "" : ",") + name.substr(4);
    }
    for (const auto& [name, data] : a) if (!b.count(name)) out += (out.empty() ? "" : ",") + name.substr(4) + "(gone)";
    return out;
}

static void test_section_mapping() {
    // The unit-to-section map of docs/CHARGER_TEST_FLOW.md.
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pd2Fixed, 1, 5000))), int(S::Pd));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pd3Fixed, 1, 5000))), int(S::Pd));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Epr, 8, 28000))), int(S::Pd));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pd31Revision, 0, 0))), int(S::Pd));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pps, 6, 9000))), int(S::Pps));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::SprAvs, 7, 9000))), int(S::Pps));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::EprAvs, 9, 9000))), int(S::Pps));
    for (const auto g : {A::Qc2, A::Qc3, A::Qc3Plus, A::Afc, A::Fcp, A::Scp, A::Qc2ClassB, A::Qc3ClassB})
        CHECK_EQ(int(cj::section_of(row(ct::group(g), g, 0, 5000))), int(S::Qc));
    for (const auto g : {A::Vooc, A::SuperVooc, A::SuperVooc65, A::Vivo})
        CHECK_EQ(int(cj::section_of(row(ct::group(g), g, 0, 5000))), int(S::Oem));
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pd3Fixed, 5, 20000, 6000, false, true))), int(S::Oem));   // a 6 A hunt row is a vendor mode
    CHECK_EQ(int(cj::section_of(row(G::Pd, A::Pps, 6, 11000, 6000, false, true))), int(S::Oem));
    CHECK_EQ(int(cj::section_of(row(G::Ufcs, A::Ufcs, 0, 5000))), int(S::Ext));
    // The layout hash is the plan's shape, not how a row went: a verdict leaves it, a moved level changes it, the seed is the section.
    auto v = fh22(true);
    const auto h = cj::layout_sig(S::Pps, v);
    for (auto& s : v) { s.verdict = ct::Verdict::Pass; s.measured_mv = 4242; s.reason = ct::Reason::Capped; s.tries = 2; s.detail = 9; }
    CHECK_EQ(cj::layout_sig(S::Pps, v), h);
    const size_t pps = first_of(v, A::Pps);
    v[pps].mv = uint16_t(v[pps].mv + 20);
    CHECK_EQ(cj::layout_sig(S::Pps, v) != h, true);
    CHECK_EQ(cj::section_rows(fh22(true), S::Pps), 11u); CHECK_EQ(cj::section_rows(fh22(false), S::Pps), 6u);
    CHECK_EQ(cj::section_rows(fh22(true), S::Oem), 3u + 3u);                     // vooc, two super vooc, three hunts
}

static void test_roundtrip() {
    Vol v; cj::Journal& j = *v.j;
    cj::Replay r; j.replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::None));
    auto steps = plan(6);                        // rows 0,2,4 are PPS, rows 1,3,5 are QC3
    arm(v, steps);
    steps[0].verdict = ct::Verdict::Pass; steps[0].measured_mv = 5050;
    CHECK_EQ(int(v.commit(steps, 0)), 0);
    CHECK_EQ(int(j.phase(cj::Phase::Request, steps, 1)), 0);
    v.reboot();                                  // everything is on flash
    v.j->replay(r);
    v.j->adopt(r);
    CHECK_EQ(r.found, true); CHECK_EQ(r.unfinished, true); CHECK_EQ(r.run_id, 5u); CHECK_EQ(r.count, 6u);
    CHECK_EQ(r.options.selected, ct::kAllAlgorithms);
    CHECK_EQ(r.evidence.session, 77u); CHECK_EQ(r.evidence.cable_mv, 50000); CHECK_EQ(r.evidence.flags, 0x23);
    CHECK_EQ(r.evidence.rebalanced, 3u);   // journaled in the header: the count survives the reboot
    CHECK_EQ(r.evidence.reported.valid, true); CHECK_EQ(r.evidence.reported.rev_major, 3); CHECK_EQ(r.evidence.reported.ver_minor, 2);
    CHECK_EQ(rows_are(r, steps), true);
    CHECK_EQ(int(r.steps[0].verdict), int(ct::Verdict::Pass)); CHECK_EQ(r.steps[0].measured_mv, 5050);
    CHECK_EQ(r.steps[4].pdo, 0x12345678u + 4);   // the rows come back in the armed order: plan row 4 at index 4
    CHECK_EQ(int(r.phase), int(cj::Phase::Request)); CHECK_EQ(r.phase_index, 1u);   // the marker was written for plan row 1
    // A final result for the step clears the marker; Finish closes the run.
    cj::Journal& k = *v.j;
    steps[1].verdict = ct::Verdict::Fail;
    v.commit(steps, 1); k.replay(r);
    CHECK_EQ(int(r.phase), int(cj::Phase::None)); CHECK_EQ(r.unfinished, true);
    k.resumed(1); k.finish(5, 0); k.replay(r);
    CHECK_EQ(r.unfinished, false); CHECK_EQ(r.resumed, 1);
}
// A Capped step (the last enum value: the reason byte is stored whole) comes back from the journal as it was recorded.
static void test_capped_step_roundtrip() {
    Vol v;
    auto steps = plan(3);
    arm(v, steps);
    steps[1].verdict = ct::Verdict::Fail; steps[1].reason = ct::Reason::Capped; steps[1].measured_mv = 19419;
    CHECK_EQ(int(v.commit(steps, 1)), 0);
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(rows_are(r, steps), true);
    CHECK_EQ(int(r.steps[1].reason), int(ct::Reason::Capped)); CHECK_EQ(r.steps[1].measured_mv, 19419);   // the armed order: row 1 keeps its result
}

static void test_a_late_avs_list_rewrites_only_the_pps_section() {
    Vol v;
    auto steps = fh22(false);
    arm(v, steps);
    const Snap a = snap(v);
    CHECK_EQ(a.size(), 6u);                                       // header + pd, pps, qc, oem, ext (all five have rows)
    steps = fh22(true);                                           // the AVS APDO appears in the middle of the array
    v.j->note_plan(steps);
    CHECK_EQ(v.j->pending(), true);
    CHECK_EQ(int(v.settle(steps)), 0);
    CHECK_EQ(v.j->pending(), false);
    CHECK_EQ(changed(a, snap(v)) == "h.rec,pps.sec", true);       // the other four sections are the same bytes
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, steps), true);
    // The same plan again costs nothing.
    v.j->note_plan(steps);
    CHECK_EQ(v.j->pending(), false);
}
static void test_dropping_the_turbo_rows_rewrites_only_the_oem_section() {
    Vol v;
    auto steps = fh22(true);
    arm(v, steps);
    const Snap a = snap(v);
    steps = without_hunts(steps);                                 // Turbo 6A not offered: the hunt rows drop out of the plan
    v.j->note_plan(steps);
    CHECK_EQ(int(v.settle(steps)), 0);
    CHECK_EQ(changed(a, snap(v)) == "h.rec,oem.sec", true);
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, steps), true);
    v.j->adopt(r);                                                // the resumed run goes on writing what it changes
    // Rows addressed by index inside a section: the rows after the dropped ones in the other sections did not move.
    CHECK_EQ(cj::section_rows(steps, S::Oem), 3u); CHECK_EQ(cj::section_rows(steps, S::Pps), 11u);
    // Every hunt row gone and a plan with none at all: the section file goes, the header says zero rows.
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](const ct::Step& s) { return cj::section_of(s) == S::Oem; }), steps.end());
    v.j->note_plan(steps);
    CHECK_EQ(int(v.settle(steps)), 0);
    CHECK_EQ(v.fs->exists(cj::section_path(S::Oem)), false);
    v.reboot(); v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, steps), true);
}
static void test_a_verdict_rewrites_only_its_section_and_not_the_header() {
    Vol v;
    auto steps = fh22(true);
    arm(v, steps);
    const Snap a = snap(v);
    size_t qc = 0;
    for (size_t n = 0; n < steps.size(); ++n) if (steps[n].group == G::Qc3) { qc = n; break; }
    steps[qc].verdict = ct::Verdict::Pass; steps[qc].measured_mv = 5010;
    v.j->note(steps, qc);
    int pumps = 0;
    while (v.j->pending()) { CHECK_EQ(int(v.j->pump(kRun, kOpt, evidence(), steps)), 0); ++pumps; }
    CHECK_EQ(pumps, 1);                                            // one file, no header
    CHECK_EQ(changed(a, snap(v)) == "qc.sec", true);
    // Two sections noted: two pumps, the first dirty section first.
    steps[0].verdict = ct::Verdict::Pass; steps[steps.size() - 1].verdict = ct::Verdict::Fail;
    v.j->note(steps, 0); v.j->note(steps, steps.size() - 1);
    pumps = 0;
    while (v.j->pending()) { CHECK_EQ(int(v.j->pump(kRun, kOpt, evidence(), steps)), 0); ++pumps; }
    CHECK_EQ(pumps, 2);
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(rows_are(r, steps), true);
}
// A pump is one file; the cost model of the RAM disk (0.2 ms a half-word, 35 ms an erase) bounds what it holds the main loop for.
static void test_a_pump_writes_one_file_and_its_cost_is_bounded() {
    Vol v;
    auto steps = fh22(false);
    arm(v, steps);
    steps = fh22(true);
    v.j->note_plan(steps);
    for (size_t n = 0; n < steps.size(); ++n) { steps[n].verdict = ct::Verdict::Pass; v.j->note(steps, n); }   // every section dirty
    int pumps = 0; double worst = 0, total = 0;
    while (v.j->pending()) {
        v.disk.reset_counters();
        CHECK_EQ(int(v.j->pump(kRun, kOpt, evidence(), steps)), 0);
        const double c = v.disk.cost_ms();
        ++pumps; total += c; worst = std::max(worst, c);
        if (pumps > 20) { CHECK_EQ(0, 1); break; }
    }
    CHECK_EQ(pumps, 6);                                            // five sections and the header (the pps layout changed)
    CHECK_EQ(worst <= 250.0, true);
    std::printf("[journal] fh22 all sections dirty: pumps=%d worst=%.0f ms total=%.0f ms\n", pumps, worst, total);
    // A full section (the most rows a plan can hold in one) is still one file and one block.
    std::vector<ct::Step> fat;
    for (size_t n = 0; n < ct::kMaxSteps; ++n) fat.push_back(row(G::Pd, A::Pps, uint8_t(1 + n % 8), 3300 + 20 * n));
    Vol w;
    arm(w, fat);
    for (auto& s : fat) s.verdict = ct::Verdict::Pass;
    w.j->note(fat, 0);
    w.disk.reset_counters();
    CHECK_EQ(int(w.j->pump(kRun, kOpt, evidence(), fat)), 0);
    std::printf("[journal] one full pps section (%zu rows): %.0f ms\n", fat.size(), w.disk.cost_ms());
    CHECK_EQ(w.disk.cost_ms() <= 300.0, true);                     // the synthetic worst: a whole plan of one section
    std::size_t sz = 0;
    CHECK_EQ(int(w.fs->size_of(cj::section_path(S::Pps), sz)), 0);
    CHECK_EQ(sz, 8 + 18 * fat.size()); CHECK_EQ(sz <= fs::kBlockSize - 8, true);
}

// A cut at every flash operation of a section rewrite leaves the old plan or the whole new one; the one window (the section
// replaced, the header not yet) is a run that is not resumable, never a plan that mixes the two.
static void test_power_cut_mid_section_rewrite() {
    const auto before = fh22(false), after = fh22(true);
    Vol probe;
    arm(probe, before);
    probe.j->note_plan(after);
    probe.disk.reset_counters();
    probe.settle(after);
    const int ops = probe.disk.ops_done();
    CHECK_EQ(ops > 0, true);
    int old_n = 0, new_n = 0, bad_n = 0;
    for (int cut = 0; cut <= ops; ++cut) {
        Vol v;
        arm(v, before);
        v.j->note_plan(after);
        v.disk.cut_after(cut);
        v.settle(after);
        v.reboot();
        cj::Replay r; v.j->replay(r);
        if (!r.found) {
            CHECK_EQ(int(r.problem), int(cj::Problem::BadSection));
            CHECK_EQ(int(r.bad), int(S::Pps));                      // only the section that was being rewritten is in doubt
            ++bad_n; continue;
        }
        if (r.count == before.size()) { CHECK_EQ(rows_are(r, before), true); ++old_n; }
        else { CHECK_EQ(rows_are(r, after), true); ++new_n; }
        CHECK_EQ(int(r.problem), int(cj::Problem::None));
        // and the next boot can go on writing
        CHECK_EQ(int(v.j->phase(cj::Phase::Handover, after, 0)), 0);
    }
    CHECK_EQ(old_n > 0 && new_n > 0, true);
    std::printf("[journal] cut sweep over %d ops: old plan %d, new plan %d, not resumable %d\n", ops + 1, old_n, new_n, bad_n);
}
// The same for a verdict write: the old or the new result is read back, nothing else moves.
static void test_power_cut_at_every_operation() {
    const auto steps0 = plan(3);
    Vol probe;
    arm(probe, steps0);
    probe.disk.reset_counters();
    auto s = steps0; s[0].verdict = ct::Verdict::Pass;
    probe.commit(s, 0);
    const int ops = probe.disk.ops_done();
    CHECK_EQ(ops > 0, true);
    for (int cut = 0; cut <= ops; ++cut) {
        Vol v;
        arm(v, steps0);
        auto t = steps0; t[0].verdict = ct::Verdict::Pass;
        v.j->note(t, 0);
        v.disk.cut_after(cut);
        v.settle(t);
        v.reboot();
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, true); CHECK_EQ(r.count, 3u);
        const auto vd = int(r.steps[0].verdict);
        CHECK_EQ(vd == int(ct::Verdict::Pass) || vd == int(ct::Verdict::Pending), true);
        CHECK_EQ(int(v.j->phase(cj::Phase::Handover, t, 1)), 0);
        v.j->replay(r);
        CHECK_EQ(int(r.phase), int(cj::Phase::Handover));
    }
}
static void test_power_cut_during_begin() {
    // A cut while a new run is written leaves the previous run or the whole new one, never a mix; an
    // earlier run that is gone leaves no run at all.
    const auto steps = plan(8);
    Vol probe;
    arm(probe, steps, 1);
    probe.disk.reset_counters();
    arm(probe, steps, 2);
    const int ops = probe.disk.ops_done();
    for (int cut = 0; cut <= ops; cut += 3) {
        Vol v;
        arm(v, steps, 1);
        v.disk.cut_after(cut);
        v.j->begin(2, kOpt, evidence(), steps);
        v.reboot();
        cj::Replay r; v.j->replay(r);
        if (r.found) { CHECK_EQ(r.run_id == 1u || r.run_id == 2u, true); CHECK_EQ(r.count, 8u); CHECK_EQ(rows_are(r, steps), true); }
    }
}
static void test_restart_replaces_the_run() {
    Vol v; cj::Journal& j = *v.j;
    arm(v, plan(30), 1);
    auto p30 = plan(30);
    j.phase(cj::Phase::Request, p30, 4); j.resumed(2); j.finish(5, 0);
    arm(v, plan(3), 2);
    cj::Replay r; j.replay(r);
    CHECK_EQ(r.run_id, 2u); CHECK_EQ(r.count, 3u); CHECK_EQ(r.unfinished, true);
    CHECK_EQ(int(r.phase), int(cj::Phase::None)); CHECK_EQ(r.resumed, 0);
    // Repeated runs do not fill the volume.
    auto big = plan(ct::kMaxSteps);
    for (int run = 0; run < 40; ++run) {
        arm(v, big, run + 10);
        for (size_t n = 0; n < big.size(); ++n) { j.phase(cj::Phase::Request, big, n); big[n].verdict = ct::Verdict::Pass; j.note(big, n); }
        CHECK_EQ(int(v.settle(big, run + 10)), 0);
        j.finish(5, 0);
        j.replay(r);
        CHECK_EQ(r.run_id, uint32_t(run + 10)); CHECK_EQ(r.unfinished, false); CHECK_EQ(j.broken(), false);
        for (auto& s : big) s.verdict = ct::Verdict::Pending;
    }
}
static void test_flash_failure_marks_broken() {
    Vol v;
    v.disk.cut_after(0);
    CHECK_EQ(int(v.j->begin(1, kOpt, evidence(), plan(2))), int(cj::Err::Flash));
    CHECK_EQ(v.j->broken(), true);
    CHECK_EQ(v.j->pending(), false);                                 // nothing left to wait for
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, plan(2), 0)), int(cj::Err::Broken));
    v.reboot();
    CHECK_EQ(int(v.j->begin(1, kOpt, evidence(), plan(2))), int(cj::Err::Ok));
}
// Run meta (the unit order, whether anyone is at the K2, the cable identity the run presented) and the loss counter of the
// step in flight: both survive a reboot, both belong to one run, a record from before them replays as "none".
static cj::RunMeta meta3() {
    cj::RunMeta m;
    m.units.add(ct::Unit::Qc3); m.units.add(ct::Unit::Pd); m.units.add(ct::Unit::Afc);
    m.unattended = true; m.cable_kind = 1; m.cable_seq = 0x1234;
    m.guard = 0x80 | 3;   // the Ra guard read "cable" (Topology 2, +1) and the user started anyway
    return m;
}
static void test_meta_and_attempt_roundtrip() {
    Vol v; cj::Journal& j = *v.j;
    cj::Replay r;
    const auto p6 = plan(6);
    arm(v, p6);
    v.reboot(); v.j->replay(r);
    CHECK_EQ(r.has_meta, false); CHECK_EQ(r.attempts, 0); CHECK_EQ(r.options.units.n, 0);   // an older run: no meta, default order
    CHECK_EQ(int(v.j->meta(meta3())), 0); CHECK_EQ(int(v.j->attempt(4, 1)), 0);
    v.reboot(); v.j->replay(r);
    CHECK_EQ(r.has_meta, true); CHECK_EQ(r.meta.units.n, 3);
    CHECK_EQ(int(r.meta.units.u[0]), int(ct::Unit::Qc3)); CHECK_EQ(int(r.meta.units.u[1]), int(ct::Unit::Pd));
    CHECK_EQ(int(r.meta.units.u[2]), int(ct::Unit::Afc));
    CHECK_EQ(r.meta.unattended, true); CHECK_EQ(r.meta.cable_kind, 1); CHECK_EQ(r.meta.cable_seq, 0x1234);
    CHECK_EQ(r.meta.guard, 0x83);                       // the guard byte rides in the meta record's spare byte
    CHECK_EQ(r.options.units.n, 3); CHECK_EQ(int(r.options.units.u[2]), int(ct::Unit::Afc));   // restore() gets the order back
    CHECK_EQ(r.attempt_index, 4u); CHECK_EQ(r.attempts, 1);
    // A phase marker can be voided (a retry that has not reached its request yet).
    v.j->phase(cj::Phase::Request, p6, 4);
    v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::Request));
    v.j->phase(cj::Phase::None, p6, 4);
    v.reboot(); v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::None));
    // The next run starts clean.
    arm(v, plan(3), 2); v.j->replay(r);
    CHECK_EQ(r.has_meta, false); CHECK_EQ(r.attempts, 0); CHECK_EQ(r.options.units.n, 0);
    (void)j;
}
// A meta written without a guard reading (no identity, or a firmware before the byte existed) replays as "not judged".
static void test_meta_without_a_guard_reads_zero() {
    Vol v; cj::Replay r;
    cj::RunMeta m = meta3(); m.guard = 0;
    arm(v, plan(6));
    CHECK_EQ(int(v.j->meta(m)), 0);
    v.reboot(); v.j->replay(r);
    CHECK_EQ(r.has_meta, true); CHECK_EQ(r.meta.guard, 0);
}
static void test_meta_and_attempt_power_cut() {
    for (int cut = 0; cut < 40; ++cut) {
        Vol v; cj::Journal& j = *v.j;
        arm(v, plan(4));
        v.disk.reset_counters(); v.disk.cut_after(cut);
        j.meta(meta3()); j.attempt(2, 1); j.attempt(2, 2);
        v.reboot();
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, true); CHECK_EQ(r.run_id, 5u);
        CHECK_EQ(r.has_meta == false || r.meta.units.n == 3, true);                    // all of the record or none of it
        CHECK_EQ(r.attempts <= 2, true);
        CHECK_EQ(r.attempts == 0 || r.attempt_index == 2u, true);
    }
}
// A run that ENDED does not come back: a Finish closes it whatever its rows say (an earlier firmware could leave Pending rows
// behind a "stopped" run), however many reboots follow, and a Finish that was torn leaves the run open (it was interrupted).
static void test_an_ended_run_is_not_resumable() {
    for (const auto state : {ct::State::Stopped, ct::State::Done, ct::State::Unsafe}) {
        Vol v; cj::Journal& j = *v.j;
        auto p6 = plan(6);
        arm(v, p6);
        p6[0].verdict = ct::Verdict::Pass; v.commit(p6, 0);
        j.phase(cj::Phase::Request, p6, 1); j.resumed(1);              // rows 1..5 Pending, a phase marker open
        cj::Replay r; j.replay(r);
        CHECK_EQ(r.found, true); CHECK_EQ(r.unfinished, true);        // interrupted: resumes
        j.finish(uint8_t(state), uint8_t(ct::Reason::Lost));
        for (int boot = 0; boot < 3; ++boot) {
            v.reboot(); v.j->replay(r);
            CHECK_EQ(r.found, true); CHECK_EQ(r.unfinished, false);   // ended: the next boot does not offer it again
            CHECK_EQ(int(r.phase), int(cj::Phase::None));
        }
    }
    // The next run starts open again.
    Vol v; arm(v, plan(3), 8); v.j->finish(uint8_t(ct::State::Stopped), 0);
    arm(v, plan(3), 9);
    cj::Replay r; v.j->replay(r); CHECK_EQ(r.unfinished, true);
}
static void test_finish_cut_at_every_operation() {
    const auto steps = plan(4);
    Vol probe;
    arm(probe, steps, 1);
    probe.disk.reset_counters();
    probe.j->finish(uint8_t(ct::State::Stopped), uint8_t(ct::Reason::Cancelled));
    const int ops = probe.disk.ops_done();
    CHECK_EQ(ops > 0, true);
    int open = 0, closed = 0;
    for (int cut = 0; cut <= ops; ++cut) {
        Vol v;
        arm(v, steps, 1);
        v.disk.cut_after(cut);
        v.j->finish(uint8_t(ct::State::Stopped), uint8_t(ct::Reason::Cancelled));
        v.reboot();
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, true);                                       // never garbage: the run is either open or closed
        (r.unfinished ? open : closed)++;
        CHECK_EQ(int(v.j->finish(uint8_t(ct::State::Stopped), 0)), 0); // and the next boot can close it
        v.j->replay(r); CHECK_EQ(r.unfinished, false);
    }
    CHECK_EQ(open > 0 && closed > 0, true);
}
// A flash write that failed earlier marks the journal broken for the rest of the run, but the CLOSE is always tried: a run that
// ended must not stay open just because one step record was lost (it resumed on every boot until the card was wiped).
static void test_finish_is_tried_after_a_failed_write() {
    Vol v; cj::Journal& j = *v.j;
    auto steps = plan(5);
    arm(v, steps, 1);
    v.disk.cut_after(0);
    steps[0].verdict = ct::Verdict::Pass;
    j.note(steps, 0);
    CHECK_EQ(int(v.settle(steps, 1)), int(cj::Err::Flash));
    CHECK_EQ(j.broken(), true);
    CHECK_EQ(int(j.phase(cj::Phase::Request, steps, 1)), int(cj::Err::Broken));   // later records are refused
    v.disk.revive();                                                  // the flash works again
    CHECK_EQ(int(j.finish(uint8_t(ct::State::Stopped), uint8_t(ct::Reason::Lost))), 0);
    cj::Replay r; j.replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(r.unfinished, false);
    v.reboot(); v.j->replay(r); CHECK_EQ(r.unfinished, false);
}
static void test_resume_gate() {
    cj::ResumeGate g;
    CHECK_EQ(g.ready(0), false);
    g.update(1000, 5000); CHECK_EQ(g.ready(1000), false);
    for (uint32_t t = 1100; t <= 1400; t += 100) g.update(t, 5000);
    CHECK_EQ(g.ready(1400), false);
    g.update(1500, 5100); CHECK_EQ(g.ready(1500), true);
    g.update(1600, 9000); CHECK_EQ(g.ready(1600), false);        // above vSafe5V
    g.update(1700, 5000); g.update(1800, 4700); CHECK_EQ(g.ready(2500), false);   // below 4.75 V
    g.update(2000, 5000); g.update(2100, 5000); g.update(2700, 5000);   // 600 ms gap restarts the window
    CHECK_EQ(g.ready(2700), false);
    for (uint32_t t = 3000; t <= 3500; t += 100) g.update(t, 5000);
    CHECK_EQ(g.ready(3500), true);
}
// The filesystem error behind Err::Flash is kept (last_fs) so the device log can name it: a journal that failed on every write
// (2026-11-08 flight) logged only "err=2", which says nothing about the volume.
static void test_failure_keeps_the_filesystem_error() {
    Vol v; cj::Journal& j = *v.j;
    CHECK_EQ(int(j.last_fs()), int(fs::Err::Ok));
    v.disk.cut_after(0);
    CHECK_EQ(int(j.begin(1, kOpt, evidence(), plan(2))), int(cj::Err::Flash));
    CHECK_EQ(int(j.last_fs()) != int(fs::Err::Ok), true);
    v.reboot();
    CHECK_EQ(int(v.j->begin(1, kOpt, evidence(), plan(2))), int(cj::Err::Ok));
    CHECK_EQ(int(v.j->last_fs()), int(fs::Err::Ok));                 // a clean call clears it
    // A volume without room: the error names it.
    Vol w;
    std::vector<uint8_t> blob(2000, 0x5A);
    char name[16];
    for (int n = 0; n < 200; ++n) {
        if (w.fs->block_count() - w.fs->used_blocks() <= 2) break;
        std::snprintf(name, sizeof name, "/x%d", n);
        if (w.fs->write_file(name, blob) != fs::Err::Ok) break;
    }
    CHECK_EQ(int(w.j->begin(1, kOpt, evidence(), plan(57))), int(cj::Err::Flash));
    CHECK_EQ(int(w.j->last_fs()), int(fs::Err::NoSpace));
}
// A polled arm writes at most one file per pump(): the main loop returns to the PD engine between two of them. What one pump
// holds the loop for is the model in RamDisk::cost_ms(): a header or a sweep step is a few ms, a section file up to about 215 ms.
static void test_polled_arm_is_bounded_per_pump_and_writes_the_header_last() {
    Vol v;
    auto old = plan(60);                                 // a longer old run, every row final: the clean-up has work
    for (auto& s : old) { s.verdict = ct::Verdict::Pass; s.measured_mv = s.mv; }
    arm(v, old, 1);
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, old, 3)), int(cj::Err::Ok));
    v.j->resumed(2);
    const auto steps = plan(57);
    v.disk.reset_counters();
    v.j->start_arm();
    CHECK_EQ(v.j->working(), true); CHECK_EQ(v.j->pending(), true);
    CHECK_EQ(int(v.disk.progs + v.disk.erases), 0);      // starting touches no flash
    int pumps = 0; double worst = 0, total = 0;
    cj::Replay r;
    while (v.j->working()) {
        v.disk.reset_counters();
        CHECK_EQ(int(v.j->pump(2, kOpt, evidence(), steps)), int(cj::Err::Ok));
        const double c = v.disk.cost_ms();
        ++pumps; total += c; worst = std::max(worst, c);
        v.j->replay(r);
        if (v.j->working()) CHECK_EQ(r.found && r.run_id == 2, false);   // no run 2 until the header is in
        if (pumps > 40) { CHECK_EQ(0, 1); break; }
    }
    CHECK_EQ(v.j->pending(), false);
    CHECK_EQ(worst <= 250.0, true);                      // a pump is one file
    v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(r.run_id, 2u); CHECK_EQ(r.count, 57u);
    CHECK_EQ(rows_are(r, steps), true);
    CHECK_EQ(r.resumed, 0);                              // the old run's records are gone
    std::printf("[arm] pumps=%d worst=%.0f ms total=%.0f ms\n", pumps, worst, total);
    // The old layout's leftovers are swept, nothing but the new run's files stays in /jr.
    int files = 0;
    v.fs->list("/jr", [&](const fs::Info&) { ++files; return true; });
    CHECK_EQ(files, 3);                                  // pps, qc sections and the header (a plan with no hunts has no oem file)
}
// A plan that did not change costs no write; one that did costs the section and the header, and note_plan finds it without
// anyone telling which rows moved.
static void test_note_plan_finds_changed_sections() {
    Vol v;
    auto steps = fh22(true);
    arm(v, steps);
    v.j->note_plan(steps);
    CHECK_EQ(v.j->pending(), false);
    const size_t pps = first_of(steps, A::Pps);
    steps[pps].mv = uint16_t(steps[pps].mv + 20);        // a PPS level moved
    v.j->note_plan(steps);
    CHECK_EQ(v.j->pending(), true);
    const Snap a = snap(v);
    v.settle(steps);
    CHECK_EQ(changed(a, snap(v)) == "h.rec,pps.sec", true);
    // A job in the way of a close: cancel() drops it, Finish still lands.
    steps[0].mv = uint16_t(steps[0].mv + 20);
    v.j->note_plan(steps);
    v.j->cancel();
    CHECK_EQ(v.j->pending(), false);
    CHECK_EQ(int(v.j->finish(5, 0)), int(cj::Err::Ok));
}
static void test_polled_arm_stops_at_a_failure_and_a_too_big_plan() {
    Vol v;
    v.j->start_arm();
    const auto big = plan(ct::kMaxSteps + 1);
    CHECK_EQ(int(v.j->pump(1, kOpt, evidence(), big)), int(cj::Err::TooBig));
    CHECK_EQ(v.j->working(), false);
    v.j->start_arm();
    v.disk.cut_after(3);                                 // the disk dies inside the arm
    int guard = 0;
    cj::Err e = cj::Err::Ok;
    while (v.j->working() && guard++ < 600) e = v.j->pump(1, kOpt, evidence(), plan(20));
    CHECK_EQ(int(e), int(cj::Err::Flash)); CHECK_EQ(v.j->working(), false); CHECK_EQ(v.j->broken(), true);
}

// The format is versioned: a journal of the one-file-per-row layout is dropped at boot and never read as a plan.
static void write_old_layout(Vol& v) {
    v.fs->mkdir("/jr");
    std::array<uint8_t, 64> b{};
    const std::size_t n = cj::finish_record(b, cj::Type::Header, 30);   // the old Header record: type 1, 30 bytes
    v.fs->write_file("/jr/h.rec", {b.data(), n});
    const std::size_t m = cj::finish_record(b, cj::Type::Step, 20);
    for (const char* p : {"/jr/s000.rec", "/jr/s001.rec", "/jr/s002.rec"}) v.fs->write_file(p, {b.data(), m});
}
static void test_an_old_layout_is_discarded_not_read() {
    Vol v;
    write_old_layout(v);
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::OldFormat));
    v.j->discard_old();
    CHECK_EQ(v.fs->exists(cj::kHeaderPath), false);
    v.j->replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::None));    // said once
    // The next arm sweeps the leftover row files.
    arm(v, plan(4));
    CHECK_EQ(v.fs->exists("/jr/s000.rec"), false); CHECK_EQ(v.fs->exists("/jr/s002.rec"), false);
    v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(r.count, 4u);
    // A format byte that is not ours (a newer layout) is treated the same.
    Vol w;
    arm(w, plan(4));
    std::array<uint8_t, cj::kMaxRecord> b{};
    std::size_t n = 0;
    CHECK_EQ(int(w.fs->read_file(cj::kHeaderPath, b, n)), 0);
    b[4 + 8] = 9;                                         // the format field
    std::uint16_t crc = bbstore::crc16(0xFFFF, {b.data(), n - 2});
    b[n - 2] = uint8_t(crc); b[n - 1] = uint8_t(crc >> 8);
    w.fs->write_file(cj::kHeaderPath, {b.data(), n});
    w.j->replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::OldFormat));
    // Format 2 (sections, no order in the header: a 60-byte payload) is old too: dropped, never read.
    Vol x;
    arm(x, plan(4));
    std::array<uint8_t, cj::kMaxRecord> c{};
    CHECK_EQ(int(x.fs->read_file(cj::kHeaderPath, c, n)), 0);
    CHECK_EQ(n, 6 + cj::kHeaderPayload);
    const std::size_t old_n = 6 + 60;
    c[2] = 60; c[4 + 8] = 2;                              // payload length 60, format byte 2
    crc = bbstore::crc16(0xFFFF, {c.data(), old_n - 2});
    c[old_n - 2] = uint8_t(crc); c[old_n - 1] = uint8_t(crc >> 8);
    x.fs->write_file(cj::kHeaderPath, {c.data(), old_n});
    x.j->replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::OldFormat));
}
// A section that does not match the header (torn, flipped, from another plan, missing) makes the run NOT resumable: re-running a
// plan that is only partly known would put every pending row at the wrong address. The report names the section.
static void test_a_bad_section_makes_the_run_not_resumable() {
    const auto steps = fh22(true);
    std::array<uint8_t, 2048> b{};
    for (const auto sec : {S::Pd, S::Pps, S::Qc, S::Oem, S::Ext}) {
        const char* path = cj::section_path(sec);
        std::size_t n = 0;
        {   // one flipped bit in the rows
            Vol v; arm(v, steps);
            v.fs->read_file(path, b, n); b[n - 3] ^= 0x10; v.fs->write_file(path, {b.data(), n});
            cj::Replay r; v.j->replay(r);
            CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::BadSection)); CHECK_EQ(int(r.bad), int(sec));
        }
        {   // cut short
            Vol v; arm(v, steps);
            v.fs->read_file(path, b, n); v.fs->write_file(path, {b.data(), n - 5});
            cj::Replay r; v.j->replay(r);
            CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::BadSection)); CHECK_EQ(int(r.bad), int(sec));
        }
        {   // missing
            Vol v; arm(v, steps);
            v.fs->remove(path);
            cj::Replay r; v.j->replay(r);
            CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::BadSection)); CHECK_EQ(int(r.bad), int(sec));
        }
    }
    {   // a whole valid section of ANOTHER plan (its own CRC is right, its hash is not the header's)
        Vol other; arm(other, fh22(false));
        std::size_t n = 0;
        other.fs->read_file(cj::section_path(S::Pps), b, n);
        Vol v; arm(v, steps);
        v.fs->write_file(cj::section_path(S::Pps), {b.data(), n});
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::BadSection)); CHECK_EQ(int(r.bad), int(S::Pps));
    }
    {   // a stray section file the header counts as empty is not read
        auto no_oem = steps;
        no_oem.erase(std::remove_if(no_oem.begin(), no_oem.end(), [](const ct::Step& s) { return cj::section_of(s) == S::Oem; }), no_oem.end());
        Vol v; arm(v, no_oem);
        std::size_t n = 0;
        Vol o; arm(o, steps);
        o.fs->read_file(cj::section_path(S::Oem), b, n);
        v.fs->write_file(cj::section_path(S::Oem), {b.data(), n});
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, no_oem), true);
    }
}
// A run resumed from the journal goes on writing: the sections it read are the ones it rewrites, the header carries on.
static void test_a_resumed_run_keeps_writing_only_what_changed() {
    Vol v;
    auto steps = fh22(true);
    arm(v, steps);
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, true);
    v.j->adopt(r);
    std::vector<ct::Step> live(r.steps.begin(), r.steps.begin() + long(r.count));    // the Runner's rows after restore()
    CHECK_EQ(rows_are(r, steps), true);                    // and they are the plan's rows in the plan's order
    const Snap a = snap(v);
    v.j->note_plan(live);
    CHECK_EQ(v.j->pending(), false);                       // nothing differs from what was read
    const size_t fixed = first_of(live, A::Pd3Fixed);
    live[fixed].verdict = ct::Verdict::Pass;
    v.commit(live, fixed);
    CHECK_EQ(changed(a, snap(v)) == "pd.sec", true);
    live.erase(live.begin() + long(first_of(live, A::Pps)));   // a plan change after the resume (a row of the pps block drops)
    v.j->note_plan(live);
    v.settle(live);
    v.reboot(); v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(r.count, live.size());
}
// The phase marker names its row by section and place in it, checked against what the row is: a plan that moved under it voids it.
static void test_phase_marker_maps_through_sections() {
    Vol v;
    auto steps = fh22(false);
    arm(v, steps);
    size_t hunt = 0;
    for (size_t n = 0; n < steps.size(); ++n) if (steps[n].hunt && steps[n].mv == 20000 && steps[n].algorithm == A::Pps) hunt = n;
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, steps, hunt)), 0);
    v.reboot();
    cj::Replay r; v.j->replay(r);
    v.j->adopt(r);
    CHECK_EQ(int(r.phase), int(cj::Phase::Request));
    CHECK_EQ(r.steps[r.phase_index].hunt, true); CHECK_EQ(r.steps[r.phase_index].mv, 20000); CHECK_EQ(int(r.steps[r.phase_index].algorithm), int(A::Pps));
    // A final result for the row voids it.
    steps[hunt].verdict = ct::Verdict::Skip; steps[hunt].reason = ct::Reason::NotUnlocked;
    v.commit(steps, hunt);
    v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::None));
    // A marker for a row still pending stands even when a later row of its section has a result: the units run in the user's
    // order, which is not the order of the rows inside a section.
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, steps, hunt - 1)), 0);
    v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::Request)); CHECK_EQ(r.steps[r.phase_index].mv, 11000);
    // A Handover marker past the last row (the unit has no row left) comes back as such.
    CHECK_EQ(int(v.j->phase(cj::Phase::Handover, steps, steps.size())), 0);
    v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::Handover)); CHECK_EQ(r.phase_index, r.count);
    // The plan moved under a marker (the row it named dropped, the next one took its place): the marker no longer names what it
    // did and is void.
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, steps, 9)), 0);
    steps.erase(steps.begin() + 9);
    v.j->note_plan(steps); v.settle(steps);
    v.j->replay(r); CHECK_EQ(int(r.phase), int(cj::Phase::None));
}

// The header carries the plan's order (format 3), written from the planner's step list when the plan is journaled;
// replay applies it verbatim. encode_order is the mechanical section-by-section indexing of that list - no rule of its own.
static bool same_rows(const std::vector<ct::Step>& a, const std::vector<ct::Step>& b) {
    if (a.size() != b.size()) return false;
    for (size_t n = 0; n < a.size(); ++n) if (!same_row(a[n], b[n])) return false;
    return true;
}
static void test_the_header_carries_the_plan_order() {
    // The header's order bytes are exactly encode_order of the armed plan: order[k] = where the plan's k-th row sits
    // in the section-by-section array.
    Vol v;
    const auto steps = fh22(true);
    arm(v, steps);
    std::array<uint8_t, cj::kMaxRecord> b{};
    std::size_t n = 0;
    CHECK_EQ(int(v.fs->read_file(cj::kHeaderPath, b, n)), 0);
    CHECK_EQ(n, cj::kRecordOverhead + cj::kHeaderPayload);
    CHECK_EQ(b[4 + 8], cj::kFormat);
    std::array<uint8_t, cj::kOrderBytes> order{};
    cj::detail::encode_order(steps, order.data());
    size_t total = 0;
    for (size_t s = 0; s < cj::kSections; ++s) total += cj::section_rows(steps, cj::Section(s));
    CHECK_EQ(total, steps.size());
    for (size_t k = 0; k < total; ++k) CHECK_EQ(b[4 + cj::kHeaderBase + k], order[k]);
    for (size_t k = total; k < cj::kOrderBytes; ++k) CHECK_EQ(b[4 + cj::kHeaderBase + k], 0xFF);
    // Replay puts the rows back in exactly that order (the FH22 plan: the 6 A hunts land before their APDO's full-current row).
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, steps), true);
    // An order that is not a permutation of the rows (a flipped byte, crc fixed) is a bad header: not resumable, dropped.
    Vol w;
    arm(w, steps);
    CHECK_EQ(int(w.fs->read_file(cj::kHeaderPath, b, n)), 0);
    b[4 + cj::kHeaderBase + 2] = uint8_t(b[4 + cj::kHeaderBase + 2] ^ 0x05);   // not a permutation any more
    const std::uint16_t crc = bbstore::crc16(0xFFFF, {b.data(), n - 2});
    b[n - 2] = uint8_t(crc); b[n - 1] = uint8_t(crc >> 8);
    w.fs->write_file(cj::kHeaderPath, {b.data(), n});
    w.j->replay(r);
    CHECK_EQ(r.found, false); CHECK_EQ(int(r.problem), int(cj::Problem::BadHeader));
    // A replan that moves rows rewrites the header too: the order is part of the plan's shape, and the moved order is
    // what a resume comes back in.
    Vol x;
    auto live = fh22(true);
    arm(x, live);
    x.reboot(); cj::Replay rr; x.j->replay(rr); x.j->adopt(rr);
    std::vector<ct::Step> moved = live;
    const size_t pps0 = first_of(moved, A::Pps);
    std::rotate(moved.begin() + long(pps0), moved.begin() + long(pps0) + 1, moved.begin() + long(pps0) + 2);   // two PPS rows swap places
    x.j->note_plan(moved);
    CHECK_EQ(x.j->plan_pending(), true);          // the plan moved: sections and header are stale
    x.settle(moved);
    CHECK_EQ(x.j->plan_pending(), false);
    x.reboot(); x.j->replay(rr);
    CHECK_EQ(rr.found, true); CHECK_EQ(rows_are(rr, moved), true);   // the moved order is what comes back
}
// ROADMAP 2026-11-09 "Charger-test journal keeps its own row order": a resume mid-PD (Mi 6 A rows in the plan) must rebuild the
// steps in the planner's step-list order - the order the run executes and the grid shows - not an order re-derived from the
// rows. Mid-PD the runner can mark a hunt row "gone" in place (adopt_list: the re-announced list dropped the 6 A offer, so the
// row keeps its place with pos=0/pdo=0, Skip NoOffer); the journal must put it back exactly there.
static void test_a_resume_mid_pd_keeps_the_planners_step_order() {
    // Guard: an untouched fresh plan round-trips in its own order.
    {
        Vol v;
        const auto fresh = planner_plan();
        CHECK_EQ(hunt_at(fresh, 11000) < fresh.size(), true);
        CHECK_EQ(hunt_at(fresh, 20000) < fresh.size(), true);
        arm(v, fresh);
        v.reboot();
        cj::Replay r; v.j->replay(r);
        CHECK_EQ(r.found, true); CHECK_EQ(rows_are(r, fresh), true);
    }
    // Mid-PD: the fixed rows passed, the 20 V 6 A hunt was answered by a list without its offer and marked gone in place,
    // and a phase marker sits at the 11 V 6 A hunt before its above-5 V request.
    Vol v;
    auto live = planner_plan();
    arm(v, live);
    for (size_t n = 0; n < 5; ++n) { live[n].verdict = ct::Verdict::Pass; live[n].measured_mv = live[n].mv; }
    CHECK_EQ(int(v.commit(live, 0)), 0);
    const size_t gone = hunt_at(live, 20000);
    CHECK_EQ(gone < live.size(), true);
    live[gone].pos = 0; live[gone].pdo = 0; live[gone].verdict = ct::Verdict::Skip; live[gone].reason = ct::Reason::NoOffer;
    CHECK_EQ(int(v.commit(live, gone)), 0);
    const size_t hunt11 = hunt_at(live, 11000);
    CHECK_EQ(hunt11 < live.size(), true);
    CHECK_EQ(int(v.j->phase(cj::Phase::Request, live, hunt11)), 0);
    v.reboot();
    cj::Replay r; v.j->replay(r);
    CHECK_EQ(r.found, true);
    CHECK_EQ(rows_are(r, live), true);   // the resumed step order equals the fresh plan's order
    CHECK_EQ(int(r.phase), int(cj::Phase::Request));
    CHECK_EQ(r.steps[r.phase_index].hunt, true); CHECK_EQ(r.steps[r.phase_index].mv, 11000u);
}
// plan_pending() is what the Runner waits for before an above-5 V request: only a change of the plan itself counts.
static void test_plan_pending_ignores_verdicts() {
    Vol v;
    auto steps = fh22(true);
    arm(v, steps);
    CHECK_EQ(v.j->plan_pending(), false);
    steps[0].verdict = ct::Verdict::Pass;
    v.j->note(steps, 0);
    CHECK_EQ(v.j->pending(), true); CHECK_EQ(v.j->plan_pending(), false);   // a verdict is saved, but nothing waits for it
    steps.erase(steps.begin() + first_of(steps, A::Vooc));
    v.j->note_plan(steps);
    CHECK_EQ(v.j->plan_pending(), true);
    v.settle(steps);
    CHECK_EQ(v.j->plan_pending(), false); CHECK_EQ(v.j->pending(), false);
}

int main() {
    test_a_resume_mid_pd_keeps_the_planners_step_order();
    test_the_header_carries_the_plan_order(); test_plan_pending_ignores_verdicts();
    test_section_mapping();
    test_a_late_avs_list_rewrites_only_the_pps_section(); test_dropping_the_turbo_rows_rewrites_only_the_oem_section();
    test_a_verdict_rewrites_only_its_section_and_not_the_header(); test_a_pump_writes_one_file_and_its_cost_is_bounded();
    test_power_cut_mid_section_rewrite(); test_an_old_layout_is_discarded_not_read(); test_a_bad_section_makes_the_run_not_resumable();
    test_a_resumed_run_keeps_writing_only_what_changed(); test_phase_marker_maps_through_sections(); test_note_plan_finds_changed_sections();
    test_polled_arm_is_bounded_per_pump_and_writes_the_header_last();
    test_polled_arm_stops_at_a_failure_and_a_too_big_plan();
    test_failure_keeps_the_filesystem_error();
    test_roundtrip(); test_capped_step_roundtrip(); test_power_cut_at_every_operation(); test_power_cut_during_begin();
    test_restart_replaces_the_run(); test_flash_failure_marks_broken(); test_meta_and_attempt_roundtrip(); test_meta_without_a_guard_reads_zero();
    test_meta_and_attempt_power_cut(); test_an_ended_run_is_not_resumable(); test_finish_cut_at_every_operation();
    test_finish_is_tried_after_a_failed_write(); test_resume_gate();
    return test_report("test_charger_journal");
}
