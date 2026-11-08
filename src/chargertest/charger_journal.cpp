// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test journal on the littlefs volume, see k2/chargertest/charger_journal_hw.hpp.
#include "k2/core/log.hpp"
#include "k2/chargertest/charger_journal_hw.hpp"

#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/drivers/usb.hpp"

namespace k2::charger_journal::hw {
namespace {
Journal g_j(fs::instance());
bool g_broken_logged;   // the first refused write after a failure is named; the rest of the run's refusals are not (a run logged 98)

// The totals of one burst of writes (an arm, or the sections a plan change or a verdict made dirty), for the one line that closes it.
struct JobLog {
    bool active, arm;
    std::uint32_t t0, pumps, busy_ms, worst_ms;
} g_job;

// A write that held the main loop longer than this is named (the PD engine is starved for the whole call: a source resets the
// link after ~100 ms of silence, the sink must answer within ~25 ms). One section file is 35 to 250 ms; over that is news.
constexpr std::uint32_t kSlowMs = 150;

std::uint32_t used() { return static_cast<std::uint32_t>(fs::instance().used_blocks()); }
std::uint32_t total() { return static_cast<std::uint32_t>(fs::instance().block_count()); }

// A failed write names the filesystem error and how full the volume is (2026-11-08: "err=2" alone could not say why), and how
// long the call held the main loop. Err::Broken is the latch answering after a failure: said once per failure.
void report(const char* what, Err e, std::uint32_t ms = 0) {
    if (e == Err::Ok) return;
    if (e == Err::Broken) {
        if (!g_broken_logged) K2LOG("[CJRN] %s refused: journal broken since an earlier failure\n", what);
        g_broken_logged = true;
        return;
    }
    g_broken_logged = false;
    K2LOG("[CJRN] %s failed err=%d fs=%s used=%u/%u ms=%u\n", what, static_cast<int>(e), fs::err_name(g_j.last_fs()),
          static_cast<unsigned>(used()), static_cast<unsigned>(total()), static_cast<unsigned>(ms));
}

void slow(const char* what, std::uint32_t ms) {
    if (ms > kSlowMs) K2LOG("[CJRN] slow %s ms=%u used=%u/%u\n", what, static_cast<unsigned>(ms), static_cast<unsigned>(used()),
                            static_cast<unsigned>(total()));
}

// One journal write, timed: any call over kSlowMs is named, a failure is reported with its time.
template <class F>
void timed(const char* what, F&& f) {
    const std::uint32_t t0 = systick::millis();
    const Err r = f();
    const std::uint32_t ms = systick::millis() - t0;
    slow(what, ms);
    report(what, r, ms);
}
}  // namespace

void mount() { K2LOG("[CJRN] journal in /jr on littlefs\n"); }
bool load(Replay& into) {
    g_j.replay(into);
    if (!into.found) {
        // A header that is there but does not give a run back is named and dropped: it would be found, and refused, on every boot.
        if (into.problem == Problem::OldFormat) {
            K2LOG("[CJRN] old journal layout dropped (format changed)\n");
            g_j.discard_old();
        } else if (into.problem == Problem::BadHeader) {
            K2LOG("[CJRN] journal header unreadable, dropped\n");
            g_j.discard_old();
        } else if (into.problem == Problem::BadSection) {
            // Not resumable, on purpose: a section that does not match the header names rows the run no longer has, and a
            // resume that guessed at them could request a level nobody planned. The run is dropped, the others are not trusted.
            K2LOG("[CJRN] run not resumable: section %s does not match its header, journal dropped\n", section_name(into.bad));
            g_j.discard_old();
        }
        return false;
    }
    // A run that is on flash but closed (it ended, or every row is final) is never resumed: say so, so a "why did it not resume"
    // question has an answer in the log.
    if (!into.unfinished) { K2LOG("[CJRN] run %u is closed, not resumed\n", unsigned(into.run_id)); return false; }
    g_j.adopt(into);   // later changes are written as changes to the section files read here
    return true;
}

void arm_start() {
    g_job = {true, true, systick::millis(), 0, 0, 0};
    g_broken_logged = false;
    g_j.start_arm();
}
bool working() { return g_j.working(); }
bool pending() { return g_j.pending(); }
bool plan_pending() { return g_j.plan_pending(); }
void cancel() {
    if (g_j.working()) K2LOG("[CJRN] begin cancelled after %u pumps\n", static_cast<unsigned>(g_job.pumps));
    g_j.cancel();
    g_job.active = false;
}
void note(std::span<const charger_test::Step> steps, std::size_t index) { g_j.note(steps, index); }
void note_plan(std::span<const charger_test::Step> steps) { g_j.note_plan(steps); }

void pump(std::uint32_t run_id, const charger_test::Options& o, const charger_test::Evidence& e,
          std::span<const charger_test::Step> steps) {
    if (!g_j.pending()) return;
    const std::uint32_t t0 = systick::millis();
    if (!g_job.active) g_job = {true, false, t0, 0, 0, 0};
    const char* what = g_job.arm ? "begin" : "replan";
    const Err r = g_j.pump(run_id, o, e, steps);
    const std::uint32_t t1 = systick::millis();
    const std::uint32_t ms = t1 - t0;
    ++g_job.pumps; g_job.busy_ms += ms;
    if (ms > g_job.worst_ms) g_job.worst_ms = ms;
    slow(g_job.arm ? "begin pump" : "section pump", ms);
    if (r != Err::Ok) { g_job.active = false; report(what, r, ms); return; }
    if (!g_j.pending()) {
        K2LOG("[CJRN] %s ok rows=%u pumps=%u busy=%ums worst=%ums elapsed=%ums used=%u/%u\n", what, static_cast<unsigned>(steps.size()),
              static_cast<unsigned>(g_job.pumps), static_cast<unsigned>(g_job.busy_ms), static_cast<unsigned>(g_job.worst_ms),
              static_cast<unsigned>(t1 - g_job.t0), static_cast<unsigned>(used()), static_cast<unsigned>(total()));
        g_job.active = false;
    }
}

void phase(Phase p, std::span<const charger_test::Step> steps, std::size_t index) {
    timed("phase", [&] { return g_j.phase(p, steps, index); });
}
void resumed(std::uint8_t count) { timed("resumed", [&] { return g_j.resumed(count); }); }
void meta(const RunMeta& m) { timed("meta", [&] { return g_j.meta(m); }); }
void attempt(std::size_t index, std::uint8_t count) { timed("attempt", [&] { return g_j.attempt(index, count); }); }
void finish(std::uint8_t state, std::uint8_t reason) { timed("finish", [&] { return g_j.finish(state, reason); }); }

}  // namespace k2::charger_journal::hw
