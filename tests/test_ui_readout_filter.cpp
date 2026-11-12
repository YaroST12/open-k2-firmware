// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The status bar's V x A readout is a display value: it follows power::read_display() (the One Euro
// filter), never power::read() (the 128-sample average, ~2.5 s). The host stub keeps the two apart
// (g_stub.pm = average, g_stub.pm_display = filtered), so a screen that read the average shows up here.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_pages.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/smoothing.hpp"
#include <vector>

using namespace k2;

namespace {

constexpr int kBandX = 140, kBandW = 84, kBandH = kBarH - 1;   // draw_top_readout's band, hairline row excluded

power::Metrics metrics(int32_t mv, int32_t ma) {
    power::Metrics m{};
    m.vbus_mv = mv;
    m.current_ma = ma;
    m.power_mw = mv / 1000 * ma;
    m.flow = ma > 0 ? power::Flow::Forward : power::Flow::Idle;
    return m;
}

std::vector<uint16_t> band() {
    std::vector<uint16_t> b;
    for (int y = 0; y < kBandH; y++)
        for (int x = kBandX; x < kBandX + kBandW; x++) b.push_back(g_panel[y * 240 + x]);
    return b;
}

struct Rig {
    Ui ui;
    uint32_t now = 20000;
    void start() {
        g_stub = {};
        g_stub.pm_online = true;
        ui.init(now);
        ui.set_rotation(0, now);
        ui.goto_page(static_cast<uint8_t>(Page::Tools), now);   // a page with the V x A readout in the bar (Home and the Meter faces have none)
        settle();
    }
    void settle() { for (int i = 0; i < 4; i++) tick(); }
    void tick() { now += 100; ui.tick(now); }   // kPageTickMs: one page update per call
    // The bar as drawn for a screen whose average and display filter both read (mv, ma).
    std::vector<uint16_t> reference(int32_t mv, int32_t ma) {
        g_stub.pm_display_set = false;
        g_stub.pm = metrics(mv, ma);
        settle();
        return band();
    }
};

// A display value that differs from the average reaches the bar; the average does not.
void test_bar_follows_display_not_average() {
    static Rig r;
    r.start();
    const auto ref_avg = r.reference(5000, 100);
    const auto ref_disp = r.reference(12000, 1000);
    CHECK_EQ(ref_avg == ref_disp, false);   // the two readouts do differ on screen

    g_stub.pm = metrics(5000, 100);               // the 2.5 s average has not moved yet
    g_stub.pm_display = metrics(12000, 1000);     // the filter has
    g_stub.pm_display_set = true;
    r.settle();
    CHECK_EQ(band() == ref_disp, true);
    CHECK_EQ(band() == ref_avg, false);
}

// A step on VBUS shows in the bar within the filter's settle time (two 20 ms samples), not after the
// averaging window: the display value comes from the real One Euro filter, the average stays put.
void test_bar_shows_step_within_filter_settle_time() {
    static Rig r;
    r.start();
    smoothing::Readout filter;   // the driver's display filter, Medium (the default)
    filter.set(smoothing::kDefault);
    uint32_t t = 0;
    for (int i = 0; i < 50; i++, t += 20) filter.push(5000, 100, t);   // a settled 5 V / 0.1 A
    for (int i = 0; i < 2; i++, t += 20) filter.push(12000, 1000, t);  // the step, two samples = 40 ms
    CHECK_EQ(filter.vbus_mv() >= 11900, true);
    CHECK_EQ(filter.current_ma() >= 950, true);

    const auto ref_old = r.reference(5000, 100);
    const auto ref_new = r.reference(filter.vbus_mv(), filter.current_ma());
    CHECK_EQ(ref_old == ref_new, false);

    g_stub.pm = metrics(5000, 100);   // the average still shows the old level 40 ms after the step
    g_stub.pm_display = metrics(filter.vbus_mv(), filter.current_ma());
    g_stub.pm_display_set = true;
    r.settle();   // a few page ticks, far less than 2 s
    CHECK_EQ(band() == ref_new, true);
    CHECK_EQ(band() == ref_old, false);
}

// Pixels written inside the bar's band while a sink stands in for the panel.
namespace bandsink {
unsigned inside;
unsigned x0, x1, y0, y1, cx, cy;
void begin(uint16_t a, uint16_t b, uint16_t c, uint16_t d) { x0 = cx = a; y0 = cy = b; x1 = c; y1 = d; }
void put(uint32_t k) {
    for (; k; --k) {
        if (cx >= static_cast<unsigned>(kBandX) && cy < static_cast<unsigned>(kBandH)) ++inside;
        if (++cx > x1) { cx = x0; if (++cy > y1) cy = y0; }
    }
}
void pixels(const uint16_t *, uint32_t c) { put(c); }
void fill(uint16_t, uint32_t c) { put(c); }
const lcd::Sink sink = {begin, pixels, fill};
}  // namespace bandsink

// A faster filter must not repaint the bar while the shown text stays the same: jitter below the text
// resolution (12.0 V, 1.00 A) writes nothing into the bar. (The rest of the page may change; the
// Detail page's elapsed-time field ticks every second.)
void test_bar_quiet_while_text_unchanged() {
    static Rig r;
    r.start();
    g_stub.pm = metrics(12000, 1000);
    g_stub.pm_display = metrics(12000, 1000);
    g_stub.pm_display_set = true;
    r.settle();
    const int32_t jitter[] = {12003, 12001, 12004, 12002, 12003, 12001};
    for (int32_t mv : jitter) {
        g_stub.pm_display = metrics(mv, 1000);
        bandsink::inside = 0;
        lcd::set_sink(&bandsink::sink);
        r.tick();
        lcd::set_sink(nullptr);
        CHECK_EQ(bandsink::inside, 0u);
    }
    // The same harness sees a real change: 12.0 V -> 9.0 V repaints the bar.
    g_stub.pm_display = metrics(9000, 1000);
    bandsink::inside = 0;
    lcd::set_sink(&bandsink::sink);
    r.tick();
    lcd::set_sink(nullptr);
    CHECK_EQ(bandsink::inside > 0, true);
}

}  // namespace

int main() {
    test_bar_follows_display_not_average();
    test_bar_shows_step_within_filter_settle_time();
    test_bar_quiet_while_text_unchanged();
    return test_report("test_ui_readout_filter");
}
