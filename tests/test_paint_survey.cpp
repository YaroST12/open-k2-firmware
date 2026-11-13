// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Paint stall survey (docs/ARCH_REWORK.md): every page in the states that matter, each UI call (tick, button, page switch,
// forced repaint) timed with the world replay's LCD cost model (tests/fake_lcd.h LcdTiming: wire 1.0667 us/px at /8 plus the
// calibrated 1.65 us/px CPU share). A UI call runs inside one main-loop pass, so its length is a lower bound on that pass's
// gap. Per state the survey prints the calls, the worst call (ms and px), how many calls passed the ceiling, and the longest
// stretch inside one call with no LCD idle-hook round (the PD service cadence during a paint).
//
// Ceilings:
// - kCeilingMs per call (one slow-task period, kLoopMs in main_loop.cpp). A call over it is marked OVER. That is the state
//   of the firmware today, so the survey only reports it; K2_GAP_GATE=1 turns every OVER into a failure (the later gate).
// - kSvcCeilingMs for the PD service gap (a third of tSenderResponse). Enforced always (ARCH_REWORK E1: the idle hook runs
//   on elapsed time), so a state over it fails `make test` and is marked SVC.
// The report goes to runs/paint-survey/report.txt too.
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_nav.h"
#include "ui_stubs.h"
#include "k2/drivers/lcd_timing.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/ui.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace k2;

namespace {

constexpr double kCeilingMs = 20.0;
constexpr double kSvcCeilingMs = 10.0;   // tests/test_world_replay.cpp kSvcCeilingMs
constexpr double kPxCpuUs = 1.65;   // tests/test_world_replay.cpp kPxCpuUs (calibrated on the 421.4 s Home repaint)

double g_us = 0;            // the survey's virtual clock: only LCD work moves it
double g_hook_last = 0;     // the last idle-hook round inside the call being measured
double g_hook_gap = 0;      // the longest stretch without one
void spend(double us) { g_us += us; }
double now_us() { return g_us; }
uint32_t paint_clock() { return static_cast<uint32_t>(static_cast<uint64_t>(g_us)); }   // what main_loop.cpp arms the budget with
void hook() {
    if (g_us - g_hook_last > g_hook_gap) g_hook_gap = g_us - g_hook_last;
    g_hook_last = g_us;
}

const LcdTiming kTiming = {spend, now_us, 1.0667, 2.0, 8.0, kPxCpuUs};

struct Call {
    unsigned px = 0;
    double ms = 0, svc_ms = 0;
};

struct Row {
    std::string page, state;
    unsigned calls = 0, over = 0;
    Call worst;                  // the longest call
    double svc_worst_ms = 0;     // the longest stretch with no idle-hook round, over all calls
    std::string worst_what;      // which call it was
};
std::vector<Row> g_rows;
Row* g_row = nullptr;

void begin_row(const char* page, const char* state) {
    Row r;
    r.page = page;
    r.state = state;
    g_rows.push_back(r);
    g_row = &g_rows.back();
}

template <class F>
void measure(const char* what, F f) {
    const unsigned px0 = g_lcd_pixels;
    const double t0 = g_us;
    g_hook_last = t0;
    g_hook_gap = 0;
    f();
    hook();   // the stretch from the last round to the end of the call
    Call c{g_lcd_pixels - px0, (g_us - t0) / 1000.0, g_hook_gap / 1000.0};
    Row& r = *g_row;
    r.calls++;
    if (c.ms > kCeilingMs) r.over++;
    if (c.ms > r.worst.ms) { r.worst = c; r.worst_what = what; }
    if (c.svc_ms > r.svc_worst_ms) r.svc_worst_ms = c.svc_ms;
}

// A 20 W PD source with PPS and AVS on CC1, 20 V 1.5 A flowing (tests/test_ui_perf.cpp fixture).
void live_fixture() {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 7; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;
    const pd::Pdo mix[7] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000},
        {pd::PdoType::Pps, 0, 3300, 11000, 5000, 55000}, {pd::PdoType::Pps, 0, 3300, 21000, 5000, 100000},
        {pd::PdoType::Avs, 0, 15000, 28000, 5000, 140000}};
    for (int i = 0; i < 7; i++) g_stub.caps.pdos[i] = mix[i];
}

void idle_fixture() {   // nothing attached: VBUS 0, no caps
    live_fixture();
    g_stub.pm = {};
    g_stub.caps.valid = false;
    g_stub.pd_seen_attach = false;
}

void wiggle(int i) {   // live values moving, as the power monitor reports them
    g_stub.pm.vbus_mv = 20030 + 37 * (i % 17);
    g_stub.pm.current_ma = 1520 + 13 * (i % 23);
    g_stub.pm.power_mw = g_stub.pm.vbus_mv / 1000 * g_stub.pm.current_ma;
}

struct PageCase { uint8_t page; const char* name; };
const PageCase kPages[] = {
    {0, "Home"}, {1, "Detail"}, {static_cast<uint8_t>(Page::ChargerTest), "Charger"}, {3, "Lines"}, {4, "Tools"},
    {5, "Settings"}, {kPdPage, "Connection"}, {kDiagPage, "Diag"},
};

// One page in one fixture: the switch into it (from Home, or from Detail for Home), 5 s of 20 ms ticks with the values
// moving, then a forced full repaint.
void survey_page(const PageCase& pc, const char* state, bool live) {
    live ? live_fixture() : idle_fixture();
    Ui ui;
    ui.set_paint_clock(paint_clock);
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    const uint8_t from = pc.page == 0 ? 1 : 0;
    ui.goto_page(from, now);
    for (int i = 0; i < 20; i++) { now += 20; ui.tick(now); }
    begin_row(pc.name, state);
    measure("switch in", [&] { ui.goto_page(pc.page, now); });
    for (int i = 0; i < 250; i++) {
        now += 20;
        if (live) wiggle(i);
        measure("tick", [&] { ui.tick(now); });
    }
    measure("forced repaint", [&] { ui.repaint(now); });
}

// The charger test: START on "All tests" with a 5 V..20 V + PPS charger (the stub answers every request), then the run's ticks,
// then the results and a few presses on them.
void survey_charger_run() {
    live_fixture();
    g_stub.pm.vbus_mv = 5000; g_stub.pm.current_ma = 0; g_stub.pm.power_mw = 0;
    g_stub.fc_answers = true;
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
    g_stub.caps.raw_header = 0x1181;
    Ui ui;
    ui.set_paint_clock(paint_clock);
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    begin_row("Charger", "setup (wizard)");
    measure("switch in", [&] { ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now); });
    auto press = [&](Button b, const char* what) { measure(what, [&] { ui.on_button(b, ButtonEvent::Short, now); }); now += 20; };
    for (unsigned i = 0; i < 12; ++i) press(Button::Minus, "cursor");   // top row: All tests
    press(Button::Ok, "pick preset");                                     // -> Cable
    press(Button::Ok, "pick cable");                                      // -> Review
    press(Button::Ok, "arm START");
    begin_row("Charger", "running");
    press(Button::Ok, "confirm START");
    unsigned ticks = 0;
    for (; ticks < 30000 && ui.charger_test_busy(); ++ticks) {
        now += 20;
        measure("tick", [&] { ui.tick(now); });
    }
    std::printf("charger run: %u ticks (%u s), stage %u\n", ticks, ticks / 50, unsigned(ui.charger_test_stage()));
    begin_row("Charger", "results");
    for (int i = 0; i < 100; ++i) { now += 20; measure("tick", [&] { ui.tick(now); }); }
    for (int i = 0; i < 6; ++i) { press(Button::Plus, "results cursor"); now += 20; measure("tick", [&] { ui.tick(now); }); }
    press(Button::Ok, "open unit steps");
    for (int i = 0; i < 10; ++i) { now += 20; measure("tick", [&] { ui.tick(now); }); }
    press(Button::Back, "back to results");
    for (int i = 0; i < 10; ++i) { now += 20; measure("tick", [&] { ui.tick(now); }); }
    measure("forced repaint", [&] { ui.repaint(now); });
}

}  // namespace

int main() {
    g_lcd_timing = &kTiming;
    lcd::set_idle_hook(hook);
    for (const PageCase& pc : kPages) {
        survey_page(pc, "live 20V", true);
        if (pc.page == 0 || pc.page == 1) survey_page(pc, "idle (no source)", false);
    }
    survey_charger_run();
    g_lcd_timing = nullptr;

    const bool gate = std::getenv("K2_GAP_GATE") != nullptr;
    mkdir("runs", 0755);
    mkdir("runs/paint-survey", 0755);
    FILE* rep = std::fopen("runs/paint-survey/report.txt", "w");
    char line[400];
    std::snprintf(line, sizeof line,
                  "paint survey: LCD model wire %.4f us/px + cpu %.2f us/px, idle hook every %u us of elapsed time; "
                  "ceiling %.0f ms per call%s, PD service gap %.0f ms (enforced)\n%-11s %-17s %6s %5s %9s %7s %-15s %8s\n",
                  kTiming.px_us, kTiming.px_cpu_us, unsigned(lcd::kServiceUs), kCeilingMs,
                  gate ? " (K2_GAP_GATE: enforced)" : " (report only)", kSvcCeilingMs, "page", "state", "calls", "over",
                  "worst_ms", "px", "worst call", "svc_gap");
    std::fputs(line, stdout);
    if (rep) std::fputs(line, rep);
    unsigned over_rows = 0, svc_rows = 0;
    for (const Row& r : g_rows) {
        const bool svc_over = r.svc_worst_ms > kSvcCeilingMs;
        std::snprintf(line, sizeof line, "%-11s %-17s %6u %5u %9.1f %7u %-15s %8.1f%s%s\n", r.page.c_str(), r.state.c_str(), r.calls,
                      r.over, r.worst.ms, r.worst.px, r.worst_what.c_str(), r.svc_worst_ms, r.over ? "  OVER" : "",
                      svc_over ? "  SVC" : "");
        std::fputs(line, stdout);
        if (rep) std::fputs(line, rep);
        if (r.over) over_rows++;
        if (svc_over) svc_rows++;
        g_checks++;
        if (gate && r.over) g_failures++;
        CHECK_COND(r.calls > 0);
        CHECK_COND(!svc_over);   // the PD service gap during a paint (ARCH_REWORK E1)
    }
    std::snprintf(line, sizeof line, "paint survey: %zu states, %u over the %.0f ms ceiling%s, %u over the %.0f ms PD service ceiling\n",
                  g_rows.size(), over_rows, kCeilingMs, gate || !over_rows ? "" : " (K2_GAP_GATE=1 makes them fail)", svc_rows,
                  kSvcCeilingMs);
    std::fputs(line, stdout);
    if (rep) { std::fputs(line, rep); std::fclose(rep); }
    return test_report("test_paint_survey");
}
