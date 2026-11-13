// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Sliced paint on the main loop (docs/ARCH_REWORK.md): a page switch paints in slices, and a slice must not wait for the next
// 20 ms slow-task tick to follow the one before it. Ui::paint_more() continues a pending paint from every other 1 ms main-loop
// pass; Ui::tick() (the 20 ms one) still does the rest of the UI's work. The panel is the survey's model at the shipped /4
// divider (wire 0.5333 us/px, calibrated 1.65 us/px of CPU per rendered pixel), with a virtual clock only the LCD moves.
// Checked: the wall time a switch takes to finish, the longest single pass (the stall the slices exist to bound), and that a
// call with nothing pending does nothing.
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_nav.h"
#include "ui_stubs.h"
#include "k2/pd/pd_source.hpp"
#include "k2/ui/ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace k2;

namespace {

double g_us = 0;
void spend(double us) { g_us += us; }
double now_us() { return g_us; }
uint32_t paint_clock() { return static_cast<uint32_t>(static_cast<uint64_t>(g_us)); }
const LcdTiming kTiming = {spend, now_us, 0.5333, 2.0, 8.0, 1.65};

void live_fixture() {   // the survey's 20 W PD source on CC1, 20 V 1.5 A flowing
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 20030;
    g_stub.pm.current_ma = 1520;
    g_stub.pm.power_mw = 30456;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 4; g_stub.caps.spec_rev = 2; g_stub.caps.active_cc = 1;
    g_stub.pd_seen_attach = true; g_stub.ol_facts.far_pd_message = true;
    const pd::Pdo mix[4] = {
        {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000},     {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000},
        {pd::PdoType::Fixed, 0, 15000, 15000, 3000, 45000},   {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000}};
    for (int i = 0; i < 4; i++) g_stub.caps.pdos[i] = mix[i];
}

struct Run {
    double wall_ms = 0;    // from the switch to the last slice
    double worst_ms = 0;   // the longest single pass
    int passes = 0;
};

// The main loop's shape: a pass every 1 ms (the SysTick wake), the slow tasks (Ui::tick) on every 20th. `continued` adds what
// pass() does on the other 19: Ui::paint_more() while a paint is pending.
Run switch_page(uint8_t from, uint8_t to, bool continued, bool armed) {
    live_fixture();
    g_us = 0;
    Ui ui;
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    ui.goto_page(from, now);
    for (int i = 0; i < 20; i++) { now += 20; ui.tick(now); }
    while (ui.painting()) { now += 20; ui.tick(now); }
    if (armed) ui.set_paint_clock(paint_clock);
    Run r;
    const double t0 = g_us;
    ui.goto_page(to, now);   // the switch paints its first slice here, as the button's pass does
    r.worst_ms = (g_us - t0) / 1000.0;
    int since_tick = 0;
    while (ui.painting() && r.passes < 5000) {
        const double next_systick = t0 + (std::floor((g_us - t0) / 1000.0) + 1.0) * 1000.0;   // wfi up to the next 1 ms edge
        spend(next_systick - g_us);
        now += 1;
        const double p0 = g_us;
        if (++since_tick >= 20) { since_tick = 0; ui.tick(now); }
        else if (continued) ui.paint_more(now);
        r.worst_ms = std::max(r.worst_ms, (g_us - p0) / 1000.0);
        r.passes++;
    }
    r.wall_ms = (g_us - t0) / 1000.0;
    return r;
}

void test_paint_more_does_nothing_when_idle() {
    live_fixture();
    g_us = 0;
    g_lcd_timing = &kTiming;
    Ui ui;
    uint32_t now = 1000;
    ui.init(now);
    ui.set_rotation(0, now);
    ui.tick(now);
    while (ui.painting()) { now += 20; ui.tick(now); }
    const unsigned px0 = g_lcd_pixels;
    CHECK_COND(!ui.paint_more(now + 1));
    CHECK_EQ(g_lcd_pixels, px0);
    g_lcd_timing = nullptr;
}

void test_continuation_finishes_a_switch_faster() {
    g_lcd_timing = &kTiming;
    const Run slow = switch_page(0, 1, false, false);
    const Run fast = switch_page(0, 1, true, false);
    std::printf("switch Home->Detail, tick only: %.1f ms in %d passes (worst %.1f ms); with paint_more: %.1f ms in %d passes (worst %.1f ms)\n",
                slow.wall_ms, slow.passes, slow.worst_ms, fast.wall_ms, fast.passes, fast.worst_ms);
    CHECK_COND(slow.wall_ms > 0);
    CHECK_COND(fast.wall_ms * 1.5 < slow.wall_ms);   // the 20 ms gaps between slices are gone
    CHECK_COND(fast.worst_ms < 12.0);                // and no pass grew: the slice is still one budget's worth
    g_lcd_timing = nullptr;
}

void test_armed_budget_bounds_the_pass_in_time() {
    g_lcd_timing = &kTiming;
    const uint8_t targets[] = {1, 2, 3, 4, 5};   // Detail, Charger, Lines, Tools, Settings
    for (uint8_t to : targets) {
        const Run px = switch_page(0, to, true, false);
        const Run run = switch_page(0, to, true, true);
        std::printf("switch Home->page %u: pixel cap %.1f ms (worst %.1f ms); armed %.1f ms in %d passes (worst %.1f ms)\n",
                    unsigned(to), px.wall_ms, px.worst_ms, run.wall_ms, run.passes, run.worst_ms);
        CHECK_COND(run.worst_ms < 10.0);
        CHECK_COND(run.wall_ms > 0);
    }
    g_lcd_timing = nullptr;
}

}  // namespace

int main() {
    test_paint_more_does_nothing_when_idle();
    test_continuation_finishes_a_switch_faster();
    test_armed_budget_bounds_the_pass_in_time();
    return test_report("test_paint_more");
}
