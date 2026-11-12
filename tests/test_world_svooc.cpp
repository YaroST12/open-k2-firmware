// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// SuperVOOC world replay: the guarded trigger (src/fastcharge/svooc_trig.cpp, the real wire, responder and guards) answering a
// SUPERVOOC adapter model (tests/world_svooc.h) through the real main loop, the real UI, the real FUSB302 and power-monitor
// drivers and the littlefs volume with the flash cost model (erases and half-word programs with interrupts masked), on the world's
// virtual clock with an interrupt model (the D+/D- EXTI and the one-shot timer). Each case is a fork, so each boots fresh.
//
// The question (hardware 2026-11-12, runs 8 and 9): every run stopped after 72 pairs (~3.36 s after the wake) with a 169 ms
// main-loop gap. The cases put a 169 ms stall just before the 72nd command and show what the adapter does:
//   - the firmware before the one-shot timer (only the main loop lets D- go after a reply): D- is still driven when the adapter's
//     next command comes, and the adapter ends the session after 72 pairs: the hardware's symptom;
//   - with the timer: D- goes 2.4 ms after each reply's last rise whatever the main loop does, and the session runs on;
//   - a stall with interrupts masked (a flash erase): no interrupt runs, so neither the edge ISR nor the timer can help.
// A natural run (no injected stall) reports the main loop's own worst stage after the wake (the stage meter, k2/core/stage_meter.hpp)
// and every pass over 20 ms. The report goes to runs/world-svooc/report.txt.
//
// Environment: K2_WORLD_LOG=1 prints the firmware's [SVOOC] lines of each case.
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "fake_lcd.h"
#include "test_util.h"
#include "ui_stubs.h"
#include "world_sim.h"
#include "world_svooc.h"
#include "k2/core/boot_pd.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/core/main_loop.hpp"
#include "k2/core/remote.hpp"
#include "k2/core/stage_meter.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/fastcharge/svooc_trig.hpp"
#include "k2/ui/bootscreen.hpp"
#include "k2/ui/ui.hpp"

namespace k2 {
Remote::Remote(Ui& ui) : ui_(ui) {}
RemoteEvent Remote::poll() { return RemoteEvent::None; }
bool Remote::button_down(Button, uint32_t) { return false; }
}  // namespace k2

namespace {

constexpr double kPxCpuUs = 1.65;          // tests/test_world_replay.cpp kPxCpuUs
constexpr double kStartMs = 3000;          // the trigger starts this long after boot (the UI has settled)
constexpr double kRunAfterWakeMs = 6000;   // and the case watches this long after the wake
constexpr double kStall72Ms = 3361;        // just before the 72nd command (3365 ms; the hardware's last pair came at ~3357 ms)
constexpr double kStallMs = 169;           // the hardware's longest main-loop gap, runs 8 and 9
constexpr std::size_t kHwPairs = 72;       // where every hardware run stopped
constexpr double kHoldUs = 2400;           // svooc_wire.hpp kHoldUs

world::Timing timing() {
    world::Timing t;
    t.px_cpu_us = kPxCpuUs;
    t.flash_erase_ms = 30;      // inferred ("a 2 KB sector erases in tens of ms", src/drivers/flash.cpp)
    t.flash_halfword_us = 45;   // inferred ("a half-word program (~50 us)")
    return t;
}

void reboot_stub() {}
void dfu_stub() {}

void boot() {
    const k2::bootmode::Mode keys = k2::bootmode::decide(0);
    const k2::bootpd::Early pd_boot = k2::bootpd::early(keys, false);
    k2::adc::init();
    k2::power::init();
    {
        bool formatted = false;
        k2::fs::init(formatted);
    }
    k2::systick::delay_ms(50);
    k2::bootpd::late(pd_boot);
    k2::session::init();
    k2::fastcharge::init();
    static k2::Ui ui;
    static k2::Remote remote{ui};
    static k2::bootscreen::Controller bootscreen;
    static k2::accelerometer::Driver accel;
    static k2::mainloop::Context ctx{ui, remote, bootscreen, accel, reboot_stub, dfu_stub};
    ui.init(k2::systick::millis(), false);
    accel.init(k2::systick::millis());
    k2::mainloop::install_hooks();
    k2::mainloop::begin(ctx);
}

struct Case {
    const char* name;
    wsvooc::Config cfg;
};

struct Outcome {
    bool ended = false;
    double ended_ms = -1;
    std::size_t pairs_latched = 0;    // commands whose slot the adapter latched before the end
    std::size_t pairs_ok = 0;         // ... and latched the word the K2's responder answered
    unsigned commands = 0, replies = 0;
    double hold_max_us = 0, hold_max_at = 0;
    unsigned contention = 0;
    std::string abort, why, stall_line;
    k2::stage::Worst worst{};
    std::vector<std::pair<double, double>> long_passes;   // (ms after the wake, ms)
    unsigned erases = 0;
};

Outcome run(const Case& c) {
    static world::Scenario sc;   // no PD, no frames: a legacy USB-A port; VBUS is the adapter model's
    sc.name = "svooc";
    sc.vbus = {{0, 5270}};
    const world::Timing t = timing();
    world::start(sc, 0, t);
    wsvooc::install(c.cfg);
    boot();
    Outcome o;
    bool started = false;
    int erases_at_wake = -1;
    while (true) {
        const double now = world::fw_ms();
        if (!started && now >= kStartMs) {
            started = true;
            k2::svooc::Params p;
            p.i86_first = 0x00A;   // run 8: --i86-first 0x00A
            const auto r = k2::svooc_trig::start(true, false, 8, true, p);
            if (r != k2::svooc::Refusal::None) { o.abort = std::string("refused: ") + k2::svooc::name(r); return o; }
        }
        const double w = wsvooc::wake_ms();
        if (w >= 0 && erases_at_wake < 0) erases_at_wake = int(world::flash_stats().erases);
        if (w >= 0 && now - w > kRunAfterWakeMs) break;
        if (now > kStartMs + 15000) break;   // never woke
        k2::mainloop::pass();
        world::spend_us(t.pass_us);
        const double d = world::fw_ms() - now;
        if (w >= 0 && d > 20) o.long_passes.push_back({now - w, d});
        world::wfi();
    }
    const wsvooc::Result& r = wsvooc::result();
    o.ended = r.ended;
    o.ended_ms = r.ended_ms;
    o.why = r.why;
    o.hold_max_us = r.hold_max_us;
    o.hold_max_at = r.hold_max_at_ms;
    o.contention = r.contention;
    const k2::svooc_trig::Snapshot s = k2::svooc_trig::snapshot();
    o.commands = s.log.commands;
    o.replies = s.log.replies;
    o.abort = k2::svooc::name(s.report.abort);
    o.worst = k2::stage::meter.worst();
    o.erases = world::flash_stats().erases - unsigned(std::max(erases_at_wake, 0));
    // The adapter latched a slot once its ninth sample came: the pairs up to the end (or the watch's end).
    const double end_ms = o.ended ? o.ended_ms : kRunAfterWakeMs;
    const char* show = std::getenv("K2_WORLD_PAIRS");   // print the first N pairs: the adapter's view and the wire's
    const std::size_t n_show = show ? std::size_t(std::atoi(show)) : 0;
    for (std::size_t i = 0; i < r.pairs.size(); ++i) {
        k2::svooc::PairRow row[1];
        const bool have = k2::svooc_trig::read_pairs(std::uint16_t(i), row) == 1;
        if (i < n_show)
            std::printf("    pair %3zu adapter %02X @%7.1f ms latched %03X | wire %s %02X word %03X @%u ms\n", i, r.pairs[i].cmd,
                        r.pairs[i].t_ms, r.pairs[i].word, have ? "" : "(none)", have ? row[0].cmd : 0,
                        have ? row[0].word : 0, have ? unsigned(row[0].t_ms) : 0);
        if (r.pairs[i].t_ms + 14 > end_ms) break;
        o.pairs_latched++;
        if (have && row[0].word == r.pairs[i].word && row[0].cmd == r.pairs[i].cmd) o.pairs_ok++;
    }
    for (const world::LogLine& l : world::logs())
        if (l.text.find("[SVOOC]") != std::string::npos) {
            if (l.text.find("[SVOOC] stall:") != std::string::npos) o.stall_line = l.text;
            if (std::getenv("K2_WORLD_LOG")) std::printf("    %s", l.text.c_str());
        }
    return o;
}

std::string serialize(const Outcome& o) {
    std::ostringstream s;
    s << o.ended << ' ' << o.ended_ms << ' ' << o.pairs_latched << ' ' << o.pairs_ok << ' ' << o.commands << ' ' << o.replies
      << ' ' << o.hold_max_us << ' ' << o.hold_max_at << ' ' << o.contention << ' ' << int(o.worst.id) << ' ' << o.worst.us << ' '
      << o.worst.at_ms << ' ' << o.worst.ticked_ms << ' ' << o.erases << ' ' << o.long_passes.size();
    for (const auto& p : o.long_passes) s << ' ' << p.first << ' ' << p.second;
    s << '\n' << o.abort << '\n' << o.why << '\n' << o.stall_line;
    return s.str();
}

Outcome deserialize(const std::string& text) {
    Outcome o;
    std::istringstream s(text);
    int id = 0;
    std::size_t n = 0;
    s >> o.ended >> o.ended_ms >> o.pairs_latched >> o.pairs_ok >> o.commands >> o.replies >> o.hold_max_us >> o.hold_max_at >>
        o.contention >> id >> o.worst.us >> o.worst.at_ms >> o.worst.ticked_ms >> o.erases >> n;
    o.worst.id = k2::stage::Id(id);
    for (std::size_t i = 0; i < n; ++i) {
        double a = 0, b = 0;
        s >> a >> b;
        o.long_passes.push_back({a, b});
    }
    std::string line;
    std::getline(s, line);
    std::getline(s, o.abort);
    std::getline(s, o.why);
    std::getline(s, o.stall_line);
    return o;
}

Outcome run_forked(const Case& c) {
    const std::string path = "/tmp/k2_world_svooc_" + std::to_string(getpid()) + "_" + c.name;
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        const Outcome o = run(c);
        std::ofstream(path) << serialize(o);
        std::fflush(stdout);
        std::_Exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    std::ifstream f(path);
    std::stringstream b;
    b << f.rdbuf();
    std::remove(path.c_str());
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        Outcome o;
        o.abort = "CRASHED";
        return o;
    }
    return deserialize(b.str());
}

std::string describe(const char* name, const Outcome& o) {
    char buf[1024];
    std::snprintf(buf, sizeof buf,
                  "%-26s adapter %s; %zu pairs latched, %zu as answered; wire commands %u replies %u; trigger abort: %s; "
                  "D- held after a reply up to %.0f us (at %.0f ms); contention %u; flash erases after the wake %u\n"
                  "%-26s worst main-loop stage since the wake: %s %.1f ms at %u ms (ms clock +%u)\n",
                  name, o.ended ? ("ENDED at " + std::to_string(int(o.ended_ms)) + " ms: " + o.why).c_str() : "kept clocking",
                  o.pairs_latched, o.pairs_ok, o.commands, o.replies, o.abort.c_str(), o.hold_max_us, o.hold_max_at,
                  o.contention, o.erases, "", k2::stage::name(o.worst.id), o.worst.us / 1000.0, unsigned(o.worst.at_ms),
                  unsigned(o.worst.ticked_ms));
    std::string s = buf;
    if (!o.long_passes.empty()) {
        s += std::string(28, ' ') + "passes over 20 ms (ms after the wake: length):";
        for (const auto& p : o.long_passes) {
            std::snprintf(buf, sizeof buf, " %.0f:%.0f", p.first, p.second);
            s += buf;
        }
        s += "\n";
    }
    if (!o.stall_line.empty()) s += std::string(28, ' ') + o.stall_line + (o.stall_line.back() == '\n' ? "" : "\n");
    return s;
}

}  // namespace

int main() {
    std::string report = "SuperVOOC world (tests/test_world_svooc.cpp): the trigger against the adapter model, 6 s after the wake\n";
    wsvooc::Config natural;                       // no injected stall, the timer on
    wsvooc::Config old_open;                      // the firmware before the timer, a 169 ms paint-like stall after pair 72
    old_open.timer = false;
    old_open.stall_at_ms = kStall72Ms;
    old_open.stall_ms = kStallMs;
    wsvooc::Config new_open = old_open;           // the same stall with the timer
    new_open.timer = true;
    wsvooc::Config new_masked = new_open;         // the same stall with interrupts masked (a flash erase)
    new_masked.stall_masked = true;
    wsvooc::Config old_natural = natural;
    old_natural.timer = false;
    const Case cases[] = {{"natural (timer)", natural}, {"natural (no timer)", old_natural},
                          {"169ms open, no timer", old_open}, {"169ms open, timer", new_open},
                          {"169ms masked, timer", new_masked}};
    Outcome out[5];
    for (std::size_t i = 0; i < 5; ++i) {
        out[i] = run_forked(cases[i]);
        report += describe(cases[i].name, out[i]);
    }
    std::printf("%s", report.c_str());
    mkdir("runs", 0755);
    mkdir("runs/world-svooc", 0755);
    std::ofstream("runs/world-svooc/report.txt") << report;

    // No stall: the adapter clocks the whole watch, every latched word is the responder's, D- goes kHoldUs after each reply.
    for (int i : {0, 1}) {
        CHECK_COND(!out[i].ended);
        CHECK_COND(out[i].pairs_latched > 120);
        CHECK_EQ(out[i].pairs_ok, out[i].pairs_latched);
        CHECK_EQ(out[i].contention, 0u);
    }
    CHECK_COND(out[0].hold_max_us < kHoldUs + 100);
    // The firmware before the timer, a 169 ms stall after the 72nd reply: D- still held at the next command, the adapter
    // ends the session after 72 pairs (the hardware's symptom).
    CHECK_COND(out[2].ended);
    CHECK_EQ(out[2].pairs_latched, kHwPairs);
    CHECK_EQ(out[2].pairs_ok, kHwPairs);
    // The timer: the same stall changes nothing on the wire.
    CHECK_COND(!out[3].ended);
    CHECK_EQ(out[3].pairs_ok, out[3].pairs_latched);
    CHECK_COND(out[3].pairs_latched > 120);
    CHECK_COND(out[3].hold_max_us < kHoldUs + 100);
    CHECK_EQ(int(out[3].worst.id), int(k2::stage::Id::Shell));   // the stage meter names the stalled poll
    CHECK_COND(out[3].worst.us >= kStallMs * 1000 && out[3].worst.at_ms >= kStall72Ms);
    // Masked (a flash erase): no interrupt runs, so the commands inside the stall go unseen and unanswered (the model's adapter
    // only ends a session on a held D-; a real one may end it on a missing reply), and the stage meter shows the ms clock
    // stood still. So a stall the hardware measured with the ms clock (poll_gap_max 169 ms) ran with interrupts open.
    CHECK_COND(out[4].pairs_ok < out[4].pairs_latched);
    CHECK_EQ(int(out[4].worst.id), int(k2::stage::Id::Shell));
    CHECK_COND(out[4].worst.ticked_ms + 100 < out[4].worst.us / 1000);
    return test_report("test_world_svooc");
}
