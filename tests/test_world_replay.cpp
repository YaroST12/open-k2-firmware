// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD world replay: every recorded attach in tests/scenarios/*.scn (written by tools/trace2scenario.py) is played against the real
// FUSB302 driver, the real power-monitor driver, the real UI and the real main loop (src/core/main_loop.cpp) on a virtual clock
// (tests/world_sim.h), on Home and on Detail, at ten loop phases. A run passes when the K2 heard what the expectations of its
// episode say (first SOP message, capabilities count, the [LINK] verdict). Each run is a fork of this process, so every run boots
// a fresh firmware state. The report goes to runs/world-replay/report.txt; docs/TESTING.md "World replay" explains the model.
//
// Environment: K2_WORLD_ONLY=<scenario>[:<episode>[:<page>[:<phase>]]] picks runs, K2_WORLD_LOG=1 prints the firmware log of
// each picked run, K2_WORLD_PX_CPU_US overrides the calibrated rendering cost, K2_WORLD_SCENARIOS a scenario directory.
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
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
#include "k2/core/boot_pd.hpp"
#include "k2/core/bootmode.hpp"
#include "k2/core/loop_stats.hpp"
#include "k2/core/main_loop.hpp"
#include "k2/core/rd_comeback.hpp"
#include "k2/core/rd_intent.hpp"
#include "k2/drivers/fs_flash.hpp"
#include "k2/core/remote.hpp"
#include "k2/chargertest/session_store.hpp"
#include "k2/drivers/accelerometer.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/drivers/power_monitor.hpp"
#include "k2/drivers/systick.hpp"
#include "k2/fastcharge/fastcharge.hpp"
#include "k2/ui/bootscreen.hpp"
#include "k2/ui/ui.hpp"

// The remote is the USB host's: nobody presses anything in the world (src/core/remote.cpp is not in the host build).
namespace k2 {
Remote::Remote(Ui& ui) : ui_(ui) {}
RemoteEvent Remote::poll() { return RemoteEvent::None; }
bool Remote::button_down(Button, uint32_t) { return false; }
}  // namespace k2

namespace {

// Calibrated on the 421.4 s Home repaint of runs/2026-11-11-late-attach/cycles (docs/TESTING.md "World replay").
constexpr double kPxCpuUs = 1.65;
// Loop phase against the world: boot 0..477 ms later in 53 ms steps, which spreads the runs over the 500 ms scan period and
// lands each on a different millisecond of the 20 ms slow-task tick and the 10 ms poll (50 ms steps hit only two).
constexpr int kPhases = 10, kPhaseStepMs = 53;
constexpr double kSettleMs = 2500;               // boot this long before the episode window opens
const char* const kPageName[] = {"Home", "Detail"};

struct RebootCase;
struct Job {
    const world::Scenario* sc;
    const world::Episode* ep;
    const world::Expect* ex;
    int page, phase;
    std::string out;                    // the child's report file
    const RebootCase* rc = nullptr;     // a reboot-world case instead of a replay run
};

world::Timing timing() {
    world::Timing t;
    t.px_cpu_us = kPxCpuUs;
    if (const char* e = std::getenv("K2_WORLD_PX_CPU_US")) t.px_cpu_us = std::atof(e);
    return t;
}

void reboot_stub() {}
void dfu_stub() {}

// The marker decision this boot made (rdcomeback::boot_look) as a line in the world's log, for the cases to match on.
void note_boot_look() {
    const k2::rdcomeback::BootLook& b = k2::rdcomeback::boot_look();
    using K = k2::rdintent::IntentKind;
    const char* kind = b.kind == K::Session ? "session" : b.kind == K::Reattach ? "reattach" : b.kind == K::Rescan ? "rescan" : "none";
    char s[160];
    std::snprintf(s, sizeof s, "[TEST] boot_look looked=%d kind=%s pin=%u canary=%d host=%d sram_words=%u", b.looked ? 1 : 0, kind,
                  unsigned(b.pin), b.canary ? 1 : 0, b.host ? 1 : 0, unsigned(b.sram_words));
    world::note(s);
}

// main.cpp boot() as far as the PD link sees it, a cold boot: the boot keys (PLUS, in the reboot world), the PD half of the boot
// through the same calls main.cpp makes (src/core/boot_pd.cpp: the Rd intent marker, the Rd decision, the comeback's early PD
// service, the self-power bring-up), the power monitor, the volume, the UI, the hooks. The rest of the slow init (LCD, USB) takes
// no part. With nothing held and no marker (the replay) the chip-default Rd goes and the FUSB302 comes up passive.
void boot(int page) {
    const std::uint8_t held = world::plus_held() ? k2::bootmode::kPlus : 0;
    const k2::bootmode::Mode keys = k2::bootmode::decide(held);
    const k2::bootpd::Early pd_boot = k2::bootpd::early(keys, false);
    note_boot_look();       // right after the marker decision: a boot the rail does not outlive still tells what it decided
    k2::adc::init();        // boottrace::init: the ADC and the power monitor
    k2::power::init();
    {
        bool formatted = false;
        k2::fs::init(formatted);
    }
    k2::systick::delay_ms(50);
    k2::bootpd::late(pd_boot);
    k2::session::init();    // the backup registers' session record: what the previous life left, and this life's own
    k2::fastcharge::init();
    static k2::Ui ui;
    static k2::Remote remote{ui};
    static k2::bootscreen::Controller bootscreen;
    static k2::accelerometer::Driver accel;
    static k2::mainloop::Context ctx{ui, remote, bootscreen, accel, reboot_stub, dfu_stub};
    ui.init(k2::systick::millis(), false);
    if (page) ui.goto_page(std::uint8_t(page), k2::systick::millis());
    ctx.rescan_landing_due = k2::rdcomeback::found().kind == k2::rdintent::IntentKind::Rescan;
    if (k2::bootmode::self_powered(keys)) {   // as main.cpp: the key that chose the mode is not the user's
        bootscreen.start(0);
        ctx.boot_key_swallow = true;
        ctx.boot_key = k2::Button::Plus;
    }
    accel.init(k2::systick::millis());
    k2::mainloop::install_hooks();
    k2::mainloop::begin(ctx);
}

struct Result {
    bool pass = true;
    std::string why;
    std::string first;     // first SOP message the driver read in the window
    double first_ms = -1;  // its wire time from the rise
    double attach_ms = -1; // "[FUSB302] Active CC detected" from the rise
    int lost_unrouted = 0, lost_other = 0;
    double longest_pass_ms = 0, longest_at = 0;
    std::vector<std::pair<double, double>> long_passes;   // (world start, ms) of every pass over 10 ms in the window
    std::string verdict;
    int caps = 0;            // Source_Capabilities the driver decoded in the window
    int verdict_caps = -1;   // the [LINK] verdict's caps= (the link layer's count for its attach)
    k2::loopstat::Stats gaps;   // the firmware's own loop and PD-service gap counters over the episode window
};

const char* find_after(const std::string& s, const char* key) {
    const auto p = s.find(key);
    return p == std::string::npos ? nullptr : s.c_str() + p + std::strlen(key);
}

Result run(const Job& j) {
    const world::Episode& ep = *j.ep;
    const world::Timing t = timing();
    world::start(*j.sc, ep.t0 - kSettleMs - j.phase * kPhaseStepMs, t);
    boot(j.page);
    Result r;
    bool window_open = false;
    while (world::world_ms() < ep.t1) {
        const double a = world::fw_ms(), aw = world::world_ms();
        if (!window_open && aw >= ep.t0) {   // the gap counters cover the episode window only, not the boot
            k2::loopstat::stats.reset(k2::systick::millis());
            window_open = true;
        }
        k2::mainloop::pass();
        world::spend_us(t.pass_us);
        const double d = world::fw_ms() - a;
        if (aw >= ep.t0 && d > r.longest_pass_ms) { r.longest_pass_ms = d; r.longest_at = aw; }
        if (aw >= ep.t0 && d > 10) r.long_passes.push_back({aw, d});
        world::wfi();
    }
    r.gaps = k2::loopstat::stats;
    const auto& frames = j.sc->frames;
    for (const world::FrameFate& f : world::fates()) {
        const world::Frame& fr = frames[f.index];
        if (fr.t < ep.t0 || fr.t > ep.t1) continue;
        if (!f.lost.empty()) (f.lost.rfind("unrouted", 0) == 0 ? r.lost_unrouted : r.lost_other)++;
        if (r.first.empty() && f.read && !fr.raw.empty() && (fr.raw[0] & 0xE0) == 0xE0) {
            r.first = world::msg_name(fr.raw);
            r.first_ms = fr.t - ep.rise;
        }
    }
    for (const world::LogLine& l : world::logs()) {
        if (l.world_ms < ep.t0 || l.world_ms > ep.t1) continue;
        if (r.attach_ms < 0 && l.text.find("[FUSB302] Active CC detected") != std::string::npos) r.attach_ms = l.world_ms - ep.rise;
        if (l.text.find("[PD] Source_Capabilities received") != std::string::npos) r.caps++;
        // The first verdict edge that is not "pending" (the verdict of this attach).
        if (const char* v = find_after(l.text, "[LINK] pd verdict="); v && r.verdict.empty() && std::strncmp(v, "pending", 7) != 0) {
            r.verdict.assign(v, std::strcspn(v, " \n"));
            if (const char* c = find_after(l.text, "caps=")) r.verdict_caps = std::atoi(c);
        }
    }
    auto fail = [&](const std::string& w) { r.pass = false; r.why += (r.why.empty() ? "" : "; ") + w; };
    if (const world::Expect* x = j.ex) {
        if (!x->first_sop.empty() && r.first != x->first_sop) fail("first SOP " + (r.first.empty() ? "none" : r.first) + ", want " + x->first_sop);
        if (x->caps_min >= 0 && r.caps < x->caps_min) fail("caps=" + std::to_string(r.caps) + ", want >=" + std::to_string(x->caps_min));
        if (x->verdict_caps_min >= 0 && r.verdict_caps < x->verdict_caps_min)
            fail("verdict caps=" + std::to_string(r.verdict_caps) + ", want >=" + std::to_string(x->verdict_caps_min));
        if (!x->verdict.empty() && r.verdict != x->verdict) fail("verdict " + (r.verdict.empty() ? "none" : r.verdict) + ", want " + x->verdict);
    }
    if (world::tx_starts()) fail("transmitted " + std::to_string(world::tx_starts()) + "x while inline");
    return r;
}

std::string line_of(const Job& j, const Result& r) {
    char b[512];
    std::snprintf(b, sizeof b,
                  "%-18s %-12s %-6s +%3dms %s first=%s@%+.0f attach=%+.0f lost_unrouted=%d lost_other=%d longest_pass=%.1fms@%.1f "
                  "caps=%d verdict=%s verdict_caps=%d%s%s%s",
                  j.sc->name.c_str(), j.ep->name.c_str(), kPageName[j.page], j.phase * kPhaseStepMs, r.pass ? "PASS" : "FAIL",
                  r.first.empty() ? "none" : r.first.c_str(), r.first_ms, r.attach_ms, r.lost_unrouted, r.lost_other,
                  r.longest_pass_ms, r.longest_at / 1000.0, r.caps, r.verdict.empty() ? "none" : r.verdict.c_str(), r.verdict_caps,
                  r.verdict_caps >= 0 && r.verdict_caps < r.caps && r.verdict_caps == 0 ? "  (verdict undercounts the caps)" : "",
                  r.pass ? "" : "  -- ", r.why.c_str());
    return b;
}

// ---- loop gap survey (docs/ARCH_REWORK.md) ----
// Every run's episode window as the firmware's own counters saw it (include/k2/core/loop_stats.hpp): the gap between two
// main-loop passes and between two 1 ms PD services (the pass or the LCD idle hook). The child writes one "  gaps" line, the
// parent sums them per scenario and page. Ceilings: a pass gap of one slow-task period (main_loop.cpp kLoopMs: power, PD poll and
// button debounce all assume it) and a PD service gap of 10 ms (a third of tSenderResponse, the Request deadline after
// Source_Capabilities). Today's firmware is over both on long repaints, so the table only reports; K2_GAP_GATE=1 fails on it.
constexpr unsigned kLoopCeilingMs = 20, kSvcCeilingMs = 10;
constexpr const char* kGapsTag = "  gaps loop=";

std::string hist_text(const k2::loopstat::Gaps& g) {
    std::string s;
    for (std::size_t i = 0; i < g.hist.size(); i++) s += (i ? "," : "") + std::to_string(g.hist[i]);
    return s;
}

std::string gaps_line(const Result& r) {
    return kGapsTag + hist_text(r.gaps.loop) + " max=" + std::to_string(r.gaps.loop.max_ms) + " svc=" + hist_text(r.gaps.service) +
           " max=" + std::to_string(r.gaps.service.max_ms) + "\n";
}

struct GapAgg {
    std::string key;
    unsigned runs = 0;
    std::array<unsigned long, k2::loopstat::kBuckets> loop{}, svc{};
    unsigned loop_max = 0, svc_max = 0;
};

// Parses a child's "  gaps" line into the scenario/page aggregate; false when the text has none (a crash, a reboot case).
bool add_gaps(const std::string& text, GapAgg& a) {
    const auto p = text.find(kGapsTag);
    if (p == std::string::npos) return false;
    unsigned long l[8], s[8];
    unsigned lm = 0, sm = 0;
    static_assert(k2::loopstat::kBuckets == 8, "the gaps line carries 8 buckets");
    if (std::sscanf(text.c_str() + p, "  gaps loop=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu max=%u svc=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu max=%u", &l[0],
                    &l[1], &l[2], &l[3], &l[4], &l[5], &l[6], &l[7], &lm, &s[0], &s[1], &s[2], &s[3], &s[4], &s[5], &s[6], &s[7],
                    &sm) != 18)
        return false;
    for (std::size_t i = 0; i < 8; i++) { a.loop[i] += l[i]; a.svc[i] += s[i]; }
    a.loop_max = std::max(a.loop_max, lm);
    a.svc_max = std::max(a.svc_max, sm);
    a.runs++;
    return true;
}

std::string gap_table(const std::vector<GapAgg>& aggs, bool gate, unsigned& over) {
    std::string out;
    char b[400];
    std::snprintf(b, sizeof b,
                  "loop gaps over each episode window, summed over the phases (buckets <=1 <=2 <=5 <=10 <=20 <=50 <=100 >100 ms); "
                  "ceiling: pass gap %u ms, PD service gap %u ms%s\n%-36s %4s  %-44s %6s  %-44s %6s\n",
                  kLoopCeilingMs, kSvcCeilingMs, gate ? " (K2_GAP_GATE: enforced)" : " (report only, K2_GAP_GATE=1 enforces)",
                  "scenario/episode/page", "runs", "pass gap histogram", "max", "PD service gap histogram", "max");
    out += b;
    auto hist = [](const auto& h) {
        std::string s;
        for (std::size_t i = 0; i < h.size(); i++) s += (i ? " " : "") + std::to_string(h[i]);
        return s;
    };
    over = 0;
    for (const GapAgg& a : aggs) {
        const bool o = a.loop_max > kLoopCeilingMs || a.svc_max > kSvcCeilingMs;
        if (o) over++;
        std::snprintf(b, sizeof b, "%-36s %4u  %-44s %6u  %-44s %6u%s\n", a.key.c_str(), a.runs, hist(a.loop).c_str(), a.loop_max,
                      hist(a.svc).c_str(), a.svc_max, o ? "  OVER" : "");
        out += b;
    }
    return out;
}

// ---- reboot bridging ----
// The reboot world (tests/world_sim.h): a charger that turns VBUS on only for a valid Rd, the K2's rail, brown-out and the cold
// boot through the real boot path. Each case says what the firmware does across the power loss; docs/TESTING.md "Reboot world"
// lists the model's numbers and where they come from.
struct LogWant { int life; const char* text; };
struct RebootCase {
    std::string name, variant;
    world::RebootScenario sc;
    int lives = 0;                  // firmware lives (power-on resets) in the scenario
    bool alive = false;             // the K2 runs at the end
    int contracts = 0;              // contracts the charger completed (PS_RDY acknowledged), at least
    std::vector<LogWant> logs;      // lines that must appear in that life's log
    std::vector<LogWant> absent;    // lines that must not
    std::string note;
};

// Source_Capabilities data objects of the charger in tests/scenarios/h-side-inline.scn (5 V 3 A first, a PPS last).
std::vector<std::uint8_t> h_side_caps() {
    const std::string hex = "2c9101082cd102002cb10400f44106006432a4c1";
    std::vector<std::uint8_t> v;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) v.push_back(std::uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return v;
}

// hr-session: a PLUS boot on a charger alone (the key's Rd makes the charger start VBUS), a self-powered session (contract, the
// follower writes the Session marker), PLUS released at 1.5 s; at 6 s the charger sends a Hard Reset and cuts VBUS for ~0.8 s: the
// K2 dies and boots again, cold, when VBUS comes back. The marker's voucher is the backup registers' session record
// (session::backup_alive); SRAM (.noinit) never survives a dark and decides nothing. Variants: the backup registers kept over the
// dark (bpr_hold_ms longer than the ~0.7 s dark) or lost (hold 0: any dark clears them) x the K2's HID port looped to its own
// receptacle or not (the loop's Rd holds the charger, its VBUS feeds the aux rail).
std::vector<RebootCase> reboot_cases() {
    std::vector<RebootCase> v;
    for (int hid = 0; hid < 2; hid++)
        for (int kept = 1; kept >= 0; kept--) {
            RebootCase c;
            c.name = "hr-session";
            c.variant = std::string(kept ? "bpr-kept" : "bpr-lost") + (hid ? "-hid-loop" : "");
            c.sc.name = c.name;
            c.sc.ch.caps = h_side_caps();
            if (!kept) c.sc.pm.bpr_hold_ms = 0;
            c.sc.hid_loop = hid;
            c.sc.plus_until_ms = 1500;
            c.sc.end_ms = 10000;
            c.sc.events = {{6000, world::RebootEvent::HardReset}};
            c.lives = 2;
            // Life 1: a key-mode boot never reads the marker, and the backup domain is empty (power-on).
            c.logs = {{1, "[PD] self-power bring-up: contract ready"}, {1, "[RDI] marker written: session pin=2"},
                      {1, "[SESS] no previous session record"}, {1, "[TEST] boot_look looked=0 kind=none pin=0 canary=0 host=0"}};
            c.absent = {{1, "[RDI] boot look"}};
            if (!hid && kept) {
                c.alive = true;
                c.contracts = 2;
                c.logs.insert(c.logs.end(), {{2, "[RDI] comeback boot: session pin=2"}, {2, "[RDI] comeback vouched"},
                                             {2, "[PD] self-power bring-up: contract ready"}, {2, "[SESS] previous session #1"},
                                             {2, "[RDI] boot look: record session pin=2 canary=1 host=0"},
                                             {2, "[TEST] boot_look looked=1 kind=session pin=2 canary=1 host=0 sram_words=0"}});
                c.absent.push_back({2, "did not survive"});
                c.note = "the backup registers vouch: the comeback bridges the Hard Reset";
            } else if (!hid) {
                c.alive = false;
                c.contracts = 1;
                // The rail is gone before this boot reaches session::init: no [SESS] line, the decision is logged by begin() itself.
                c.logs.insert(c.logs.end(), {{2, "[RDI] record session pin=2 ignored: the backup registers did not survive this boot"},
                                             {2, "[RDI] boot look: record session pin=2 canary=0 host=0"},
                                             {2, "[TEST] boot_look looked=1 kind=session pin=2 canary=0 host=0 sram_words=0"},
                                             {2, "[WORLD] charger: no Rd"}});
                c.absent.push_back({2, "[RDI] comeback boot"});
                c.note = "no backup registers over the Hard Reset's dark = a stale record; the plain boot drops the Rd, the charger detaches";
            } else if (kept) {
                c.alive = true;
                c.contracts = 1;
                c.logs.insert(c.logs.end(), {{2, "[RDI] record session pin=2 ignored: a USB host powers this boot"},
                                             {2, "[RDI] boot look: record session pin=2 canary=1 host=1"},
                                             {2, "[TEST] boot_look looked=1 kind=session pin=2 canary=1 host=1 sram_words=0"}});
                c.absent.push_back({2, "[RDI] comeback boot"});
                c.absent.push_back({2, "did not survive"});
                c.note = "the loop's Rd holds the charger; its aux rail makes the record a host boot's: passive, alive, no new contract";
            } else {
                c.alive = true;
                c.contracts = 1;
                c.logs.insert(c.logs.end(), {{2, "[RDI] record session pin=2 ignored: the backup registers did not survive this boot"},
                                             {2, "[RDI] boot look: record session pin=2 canary=0 host=1"}, {2, "[SESS] no previous session record"},
                                             {2, "[TEST] boot_look looked=1 kind=session pin=2 canary=0 host=1 sram_words=0"}});
                c.absent.push_back({2, "[RDI] comeback boot"});
                c.note = "stale record and a host-powered boot: passive either way, alive on the loop's Rd, no new contract";
            }
            v.push_back(std::move(c));
        }
    return v;
}

// One firmware life: power-on reset, the boot, the main loop until the scenario ends (or the rail drops: world_sim exits).
[[noreturn]] void life(double end_ms) {
    world::life_begin();
    boot(0);
    const world::Timing t = timing();
    while (world::world_ms() < end_ms) {
        k2::mainloop::pass();
        world::spend_us(t.pass_us);
        world::wfi();
    }
    world::life_end();
    std::_Exit(0);
}

struct RebootOut {
    bool pass = true;
    std::string why;
    world::RebootResult r;
};

RebootOut run_reboot(const RebootCase& c) {
    RebootOut o;
    auto fail = [&](const std::string& w) { o.pass = false; o.why += (o.why.empty() ? "" : "; ") + w; };
    world::reboot_start(c.sc, timing());
    if (!world::noinit_found()) fail(".noinit not found in this executable (or larger than the world's copy): the SRAM probe cannot be modelled");
    while (world::reboot_wait_boot()) {
        std::fflush(nullptr);
        const pid_t p = fork();
        if (p == 0) life(c.sc.end_ms);
        int st = 0;
        waitpid(p, &st, 0);
        if (!WIFEXITED(st) || (WEXITSTATUS(st) != 0 && WEXITSTATUS(st) != world::kLifeDied)) {
            fail("a life crashed (status " + std::to_string(st) + ")");
            break;
        }
    }
    o.r = world::reboot_result();
    const world::RebootResult& r = o.r;
    if (r.lives != c.lives) fail("lives=" + std::to_string(r.lives) + ", want " + std::to_string(c.lives));
    if (r.alive_at_end != c.alive) fail(c.alive ? "dead at the end" : "alive at the end");
    if (r.ch.contracts < c.contracts) fail("contracts=" + std::to_string(r.ch.contracts) + ", want >=" + std::to_string(c.contracts));
    if (c.alive && r.ch.rd_gap_max_ms >= c.sc.ch.pd_debounce_ms) {
        char b[96];
        std::snprintf(b, sizeof b, "Rd gap %.1f ms >= tPDDebounce %.0f ms", r.ch.rd_gap_max_ms, c.sc.ch.pd_debounce_ms);
        fail(b);
    }
    const auto& logs = world::logs();
    auto seen = [&](const LogWant& w) {
        return std::any_of(logs.begin(), logs.end(), [&](const world::LogLine& l) { return l.life == w.life && l.text.find(w.text) != std::string::npos; });
    };
    for (const LogWant& w : c.logs) if (!seen(w)) fail("life " + std::to_string(w.life) + " never logged \"" + w.text + "\"");
    for (const LogWant& w : c.absent) if (seen(w)) fail("life " + std::to_string(w.life) + " logged \"" + w.text + "\"");
    return o;
}

[[noreturn]] void reboot_child(const Job& j) {
    const RebootCase& c = *j.rc;
    const RebootOut o = run_reboot(c);
    const world::RebootResult& r = o.r;
    char b[640];
    std::snprintf(b, sizeof b,
                  "%-18s %-22s %s lives=%d alive=%d contracts=%d hard_resets=%d attaches=%d detaches=%d caps_acked=%d/%d "
                  "rd_gap_max=%.1fms@%.1f last_boot=%.1f  (%s)%s%s",
                  c.name.c_str(), c.variant.c_str(), o.pass ? "PASS" : "FAIL", r.lives, r.alive_at_end ? 1 : 0, r.ch.contracts,
                  r.ch.hard_resets, r.ch.attaches, r.ch.detaches, r.ch.caps_acked, r.ch.caps_sent, r.ch.rd_gap_max_ms,
                  r.ch.rd_gap_max_at, r.last_boot_ms, c.note.c_str(), o.pass ? "" : "  -- ", o.why.c_str());
    std::ofstream out(j.out);
    out << b << "\n";
    if (std::getenv("K2_WORLD_LOG"))
        for (const world::LogLine& l : world::logs()) {
            char p[64];
            std::snprintf(p, sizeof p, "    log L%d %9.1f ", l.life, l.world_ms);
            out << p << l.text;
        }
    out.close();
    std::_Exit(o.pass ? 0 : 1);
}

// The child: one run, its report line (and on K2_WORLD_LOG the log and the lost frames) into j.out; exit 0 on PASS.
[[noreturn]] void child(const Job& j) {
    if (j.rc) reboot_child(j);
    const Result r = run(j);
    std::ofstream o(j.out);
    o << line_of(j, r) << "\n" << gaps_line(r);
    if (std::getenv("K2_WORLD_LOG")) {
        for (const auto& [at, ms] : r.long_passes) {
            char b[96];
            std::snprintf(b, sizeof b, "    pass %.1f %.1f ms (rise%+.1f)\n", at, ms, at - j.ep->rise);
            o << b;
        }
        for (const world::LogLine& l : world::logs())
            if (l.world_ms >= j.ep->t0 - 200 && l.world_ms <= j.ep->t1) o << "    log " << l.world_ms << " " << l.text;
        for (const world::FrameFate& f : world::fates()) {
            const world::Frame& fr = j.sc->frames[f.index];
            if (fr.t < j.ep->t0 || fr.t > j.ep->t1) continue;
            char b[200];
            std::snprintf(b, sizeof b, "    frame %.1f CC%d %s:%s %s%s\n", fr.t, fr.line, world::sop_name(fr.raw[0]),
                          world::msg_name(fr.raw), f.read ? "read" : f.delivered ? "in-fifo" : "lost: ", f.lost.c_str());
            o << b;
        }
    }
    o.close();
    std::_Exit(r.pass ? 0 : 1);
}

std::vector<std::string> scenario_files(const std::string& dir) {
    std::vector<std::string> v;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".scn") v.push_back(dir + "/" + n);
        }
        closedir(d);
    }
    std::sort(v.begin(), v.end());
    return v;
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> v;
    std::stringstream ss(s);
    for (std::string p; std::getline(ss, p, c);) v.push_back(p);
    return v;
}

}  // namespace

int main() {
    const char* sdir = std::getenv("K2_WORLD_SCENARIOS");
    const std::string dir = sdir ? sdir : "tests/scenarios";
    std::vector<world::Scenario> scs;
    for (const std::string& f : scenario_files(dir)) {
        world::Scenario s;
        std::string err;
        CHECK_COND(world::load_scenario(f, s, err));
        if (!err.empty()) std::printf("%s\n", err.c_str());
        else scs.push_back(std::move(s));
    }
    CHECK_COND(!scs.empty());
    const char* only_env = std::getenv("K2_WORLD_ONLY");
    const std::vector<std::string> only = only_env ? split(only_env, ':') : std::vector<std::string>{};
    auto picked = [&](std::size_t i, const std::string& v) { return only.size() <= i || only[i] == v; };

    mkdir("runs", 0755);
    mkdir("runs/world-replay", 0755);
    std::vector<Job> jobs;
    for (const world::Scenario& s : scs) {
        if (!picked(0, s.name)) continue;
        for (const world::Episode& e : s.episodes) {
            if (!picked(1, e.name)) continue;
            const world::Expect* x = nullptr;
            for (const world::Expect& c : s.expects) if (c.episode == e.name) x = &c;
            CHECK_COND(x != nullptr);   // every episode carries its expectations
            for (int page = 0; page < 2; page++) {
                if (!picked(2, kPageName[page])) continue;
                for (int ph = 0; ph < kPhases; ph++) {
                    if (!picked(3, std::to_string(ph * kPhaseStepMs))) continue;
                    jobs.push_back({&s, &e, x, page, ph, "runs/world-replay/job" + std::to_string(jobs.size()) + ".txt"});
                }
            }
        }
    }
    static const std::vector<RebootCase> rcs = reboot_cases();
    for (const RebootCase& c : rcs)
        if (picked(0, c.name) && picked(1, c.variant))
            jobs.push_back({nullptr, nullptr, nullptr, 0, 0, "runs/world-replay/job" + std::to_string(jobs.size()) + ".txt", &c});
    std::fflush(stdout);

    const long ncpu = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
    std::vector<int> status(jobs.size(), -1);
    std::vector<pid_t> pids(jobs.size(), 0);
    std::size_t next = 0, running = 0;
    while (next < jobs.size() || running) {
        while (next < jobs.size() && long(running) < ncpu) {
            const pid_t p = fork();
            if (p == 0) child(jobs[next]);
            pids[next++] = p;
            running++;
        }
        int st = 0;
        const pid_t p = wait(&st);
        if (p <= 0) break;
        running--;
        for (std::size_t i = 0; i < jobs.size(); i++)
            if (pids[i] == p) status[i] = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }

    std::ofstream rep("runs/world-replay/report.txt");
    const world::Timing t = timing();
    char hdr[300];
    std::snprintf(hdr, sizeof hdr,
                  "world replay: fusb_byte=%.0fus ina_byte=%.0fus delay_us x%.2f px wire=%.4fus px cpu=%.3fus call=%.1fus "
                  "window=%.1fus pass=%.0fus\n",
                  t.fusb_byte_us, t.ina_byte_us, t.delay_us_factor, t.px_us, t.px_cpu_us, t.lcd_call_us, t.lcd_window_us, t.pass_us);
    rep << hdr;
    int fails = 0;
    std::vector<GapAgg> aggs;
    for (std::size_t i = 0; i < jobs.size(); i++) {
        std::ifstream in(jobs[i].out);
        std::stringstream body;
        body << in.rdbuf();
        std::string text = body.str();
        if (!jobs[i].rc) {
            const std::string key = jobs[i].sc->name + "/" + jobs[i].ep->name + "/" + kPageName[jobs[i].page];
            if (aggs.empty() || aggs.back().key != key) {
                aggs.emplace_back();
                aggs.back().key = key;
            }
            add_gaps(text, aggs.back());
        }
        if (text.empty())
            text = (jobs[i].rc ? jobs[i].rc->name + " " + jobs[i].rc->variant : jobs[i].sc->name + " " + jobs[i].ep->name) +
                   " crashed, status " + std::to_string(status[i]) + "\n";
        rep << text;
        g_checks++;
        if (status[i] != 0) {
            g_failures++;
            fails++;
            std::printf("FAIL %s", text.c_str());
        } else if (only_env) std::printf("%s", text.c_str());
        std::remove(jobs[i].out.c_str());
    }
    const bool gate = std::getenv("K2_GAP_GATE") != nullptr;
    unsigned over = 0;
    const std::string table = gap_table(aggs, gate, over);
    rep << table;
    std::fputs(table.c_str(), stdout);
    g_checks++;
    if (gate && over) g_failures++;
    std::printf("world replay: %zu runs, %d failed (runs/world-replay/report.txt); %u of %zu scenario pages over the gap ceiling\n",
                jobs.size(), fails, over, aggs.size());
    return test_report("test_world_replay");
}
