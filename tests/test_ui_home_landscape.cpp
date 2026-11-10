// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home in every rotation: the frame (rules, band hairlines) survives every state and every partial repaint, the
// incremental frames equal a full render, and the info block never leaves an empty bar. The landscape rotations
// squeeze the 224x240 logical page onto 240x220, so a native glyph box is taller than the logical row that holds it:
// a field that clears its whole box would cut the rule under it.
// K2_DUMP=dir writes k2_homefill_<state>_r<rot>.panel for tools/panel2png.py.
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/pd/pd_source.hpp"
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>

using namespace k2;
#define CHECK_TRUE(c) CHECK_EQ(int(!!(c)), 1)
namespace hm = k2::home;

static uint32_t g_t = 100000;

static Ui &home_ui(uint8_t rot) {
    static Ui ui;
    ui.~Ui();
    new (&ui) Ui;
    g_stub = StubState();
    g_stub.ms = 10000;
    g_stub.pm_online = true;
    ui.init(g_stub.ms);
    ui.set_rotation(rot, g_t);
    ui.goto_page(static_cast<uint8_t>(Page::Home), g_t);
    return ui;
}
static void run(Ui &ui, int n) { for (int i = 0; i < n; i++) { g_t += 100; ui.tick(g_t); } }

static void dump(const char *state, int rot) {
    const char *dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char p[256];
    std::snprintf(p, sizeof p, "%s/k2_homefill_%s_r%d.panel", dir, state, rot);
    if (FILE *f = std::fopen(p, "wb")) { std::fwrite(g_panel, 2, 240 * 240, f); std::fclose(f); }
}

static void set_adc(int cc1, int cc2, int dp, int dm) {
    adc::Reading r;
    r.dp_mv = dp; r.dm_mv = dm; r.cc1_mv = cc1; r.cc2_mv = cc2; r.vref_raw = 1489; r.vdd_mv = 3300;
    r.mcu = {312, true, false};
    g_stub.adc = r;
}

static void no_source() {
    g_stub.pm = {};
    g_stub.contract = {};
    g_stub.caps = {};
    g_stub.fc = fastcharge::Protocol::Pending;
    set_adc(3300, 0, 0, 0);
}

static void v5_idle() {
    no_source();
    g_stub.pm.vbus_mv = 5000;
    set_adc(1650, 0, 3300, 3300);
}

static void pd_source(int mv, int ma, bool pps, int64_t mw = -1) {
    static uint32_t rx = 0;
    g_stub.caps = {};
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 4; g_stub.caps.spec_rev = 2;
    g_stub.caps.rx_count = ++rx;
    g_stub.caps.pdos[0] = {pd::PdoType::Fixed, 0, 5000, 5000, 3000, 15000};
    g_stub.caps.pdos[1] = {pd::PdoType::Fixed, 0, 9000, 9000, 3000, 27000};
    g_stub.caps.pdos[2] = {pd::PdoType::Fixed, 0, 20000, 20000, 5000, 100000};
    g_stub.caps.pdos[3] = {pd::PdoType::Pps, 0, 3300, 21000, 5000, 105000};
    g_stub.contract = {};
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = pps ? 4 : 3;
    g_stub.contract.mv = mv; g_stub.contract.op_ma = 3000; g_stub.contract.max_ma = 3000;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = mv; g_stub.pm.current_ma = ma;
    g_stub.pm.power_mw = mw >= 0 ? static_cast<int32_t>(mw) : mv / 1000 * ma;
    g_stub.pm.flow = power::Flow::Forward;
    g_stub.em.valid = true; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 100; g_stub.em.max_voltage_v = 20;
    g_stub.cc = 1;
    g_stub.fc = fastcharge::Protocol::Pd;
    set_adc(1620, 70, 600, 580);
}

struct State {
    const char *name;
    void (*apply)();
};
static const State kStates[] = {
    {"nosource", [] { no_source(); }},
    {"v5idle", [] { v5_idle(); }},
    {"pd20v3a", [] { pd_source(20000, 3000, false); }},
    {"pps", [] { pd_source(12340, 2500, true); }},
    {"wide", [] { pd_source(20010, 5000, false, 100000); }},   // 100.0 W: the widest W
};

// ---- panel geometry ----------------------------------------------------------------------------------

struct PRect { int x0, y0, x1, y1; };   // panel pixels, x1 / y1 exclusive

// A logical 224x240 rect on the panel, mapped like the content viewport does it (c * extent / logical).
static PRect panel_of(Ui &ui, int lx, int ly, int lw, int lh) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters, kSlotLens,
                                    kPanelThickness).content;
    return {box.x + lx * box.w / 224, box.y + ly * box.h / 240,
            box.x + (lx + lw) * box.w / 224, box.y + (ly + lh) * box.h / 240};
}
static int count_not(const PRect &r, uint16_t color) {
    int n = 0;
    for (int y = r.y0; y < r.y1; y++)
        for (int x = r.x0; x < r.x1; x++) n += g_panel[y * 240 + x] != color;
    return n;
}
static int count_ink(const PRect &r) { return count_not(r, kBg); }

// The frame: three 2 px rules, the band's divider and split line. Every pixel of each mapped rect must be kRule, and
// each must keep at least one physical row / column (a 1 px line must not be squeezed away by the mapping).
static int frame_damage(Ui &ui, const char *what) {
    struct Line { const char *name; int x, y, w, h; };
    const Line lines[] = {
        {"rule1", 0, hm::kRule1Y, hm::kW, 2},
        {"rule2", 0, hm::kRule2Y, hm::kW, 2},
        {"rule3", 0, hm::kRule3Y, hm::kW, 2},
        {"divider", hm::kBandMidX, hm::kBandY0, 1, hm::kBandY1 - hm::kBandY0 + 1},
        {"split", hm::kBandMidX + 1, hm::kBandSplitY, hm::kW - hm::kBandMidX - 1, 1},
    };
    int bad = 0;
    for (const Line &l : lines) {
        const PRect r = panel_of(ui, l.x, l.y, l.w, l.h);
        const int area = (r.x1 - r.x0) * (r.y1 - r.y0);
        const int wrong = area > 0 ? count_not(r, kRule) : 1;
        if (wrong) {
            std::printf("frame damage: %s rot %d %s: %d of %d px not kRule\n", l.name, int(ui.rotation()), what, wrong, area);
            bad++;
        }
    }
    return bad;
}

static int mismatches(Ui &ui) {
    static uint16_t inc[240 * 240];
    for (int i = 0; i < 240 * 240; i++) inc[i] = g_panel[i];
    ui.resync(g_t);
    int bad = 0;
    for (int i = 0; i < 240 * 240; i++) bad += g_panel[i] != inc[i];
    for (int i = 0; i < 240 * 240; i++) g_panel[i] = inc[i];   // carry the incremental panel on
    return bad;
}

// ---- tests -------------------------------------------------------------------------------------------

// The rules and band hairlines are whole in every rotation and every state.
static void test_frame_whole_in_every_state() {
    for (uint8_t rot = 0; rot < 4; rot++)
        for (const State &s : kStates) {
            Ui &ui = home_ui(rot);
            s.apply();
            run(ui, 30);
            dump(s.name, rot);
            CHECK_EQ(frame_damage(ui, s.name), 0);
        }
}

// A value that changes width repaints in place: the frame stays whole and the panel equals a full render.
static void test_width_changes_keep_the_frame() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        Ui &ui = home_ui(rot);
        pd_source(9999, 3000, false, 0);
        run(ui, 30);
        pd_source(10000, 3000, false, 99900);   // 9.999 -> 10.00 V, 99.9 W
        run(ui, 12);
        CHECK_EQ(frame_damage(ui, "10.00 V"), 0);
        CHECK_EQ(mismatches(ui), 0);
        pd_source(10000, 3000, false, 100000);  // 99.9 -> 100.0 W
        run(ui, 12);
        CHECK_EQ(frame_damage(ui, "100.0 W"), 0);
        CHECK_EQ(mismatches(ui), 0);
        pd_source(5000, 100, false, 500);       // and back down to 0.50 W
        run(ui, 12);
        CHECK_EQ(frame_damage(ui, "0.50 W"), 0);
        CHECK_EQ(mismatches(ui), 0);
    }
}

// Badge and protocol text change width: live -> unplugged (LAST pill, stale row) -> idle -> PPS -> live again.
static void test_text_changes_keep_the_frame() {
    for (uint8_t rot = 0; rot < 4; rot++) {
        Ui &ui = home_ui(rot);
        pd_source(20000, 3000, false);
        run(ui, 30);
        for (const State &s : {kStates[0], kStates[3], kStates[1], kStates[2], kStates[0]}) {
            s.apply();
            run(ui, 14);
            CHECK_EQ(frame_damage(ui, s.name), 0);
            CHECK_EQ(mismatches(ui), 0);
        }
    }
}

// No bar is left empty: the first info row always carries the measured lines (CC1/CC2 outside Live, D+/D- while
// the USB link is unknown), the second row the contract. The label alone ("USB") does not fill the bar.
static void test_info_bars_carry_data() {
    for (uint8_t rot = 0; rot < 4; rot++)
        for (const State &s : kStates) {
            Ui &ui = home_ui(rot);
            s.apply();
            run(ui, 30);
            const PRect row1 = panel_of(ui, 52, 201, 120, 16);   // the value column of the first bar
            std::printf("  %s r%d row1 ink %d\n", s.name, int(rot), count_ink(row1));
            CHECK_TRUE(count_ink(row1) > 120);
        }
}

static void test_format_lines() {
    hm::LinesText t;
    hm::format_lines(false, 1620, 70, t);
    CHECK_STR(t.tag.c_str(), "CC1/2");
    CHECK_STR(t.value.c_str(), "1.62/0.07V");
    hm::format_lines(true, 600, 580, t);
    CHECK_STR(t.tag.c_str(), "D+/D-");
    CHECK_STR(t.value.c_str(), "0.60/0.58V");
    hm::format_lines(false, -1, 3300, t);   // no ADC reading
    CHECK_STR(t.value.c_str(), "-.--/3.30V");
    hm::format_lines(false, 3300, 3300, t);
    CHECK_STR(t.value.c_str(), "3.30/3.30V");
}

int main() {
    test_format_lines();
    test_frame_whole_in_every_state();
    test_width_changes_keep_the_frame();
    test_text_changes_keep_the_frame();
    test_info_bars_carry_data();
    return test_report("test_ui_home_landscape");
}
