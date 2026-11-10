// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test Running view of a deep-sweep row (ROADMAP "Sweep live view"), driven end to end through the page on the host
// stubs: the badge names the range, the gauge shows the range as two dotted lines with a setpoint marker that moves point by
// point, and one text line reads the last point's request, reading and error. A non-sweep row keeps today's look.
// K2_DUMP=dir host-build/test_ui_sweep_live writes dir/k2_sweep_*.panel (tools/panel2png.py makes the PNGs).
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/kit.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/sweep.hpp"

using namespace k2;
namespace ct = charger_test;
using A = ct::Algorithm;

namespace {
constexpr int kRunningStage = 5, kCustomStage = 14;
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamPd = 4, kFamQc = 5, kFamDone = 6;
// The Running view's gauge strip, the target line (rows 88..99, its last two rows share the strip's first two), the per-point line
// of a sweep row (two rows higher, so it never touches the strip) and the name line.
constexpr int kStripY = 98, kTargetY = 88, kPointY = 86, kNameY = 68;

void press(Ui& ui, Button b, uint32_t now) { ui.on_button(b, ButtonEvent::Short, now); }
void move(Ui& ui, unsigned n, uint32_t now) { for (unsigned i = 0; i < n; ++i) press(ui, Button::Plus, now); }
void top(Ui& ui, uint32_t now) { for (unsigned i = 0; i < ct::kMaxSteps; ++i) press(ui, Button::Minus, now); }

void dump_panel(const char* name) {
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[160];
    std::snprintf(path, sizeof path, "%s/k2_%s.panel", dir, name);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, size_t(LCD_WIDTH) * LCD_HEIGHT, f); std::fclose(f); }
}

// A source with a fixed 5 V and one programmable range; the converter follows every request up to `ceiling_mv`.
void fixture(bool avs, int ceiling_mv) {
    g_stub = {}; pd::set_fake(0);
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.fc_answers = true;
    g_stub.vbus_ceiling_mv = ceiling_mv;
    g_stub.caps.valid = true; g_stub.caps.num_pdos = avs ? 7 : 2; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    if (avs) {   // a SPR AVS APDO is only offered behind the fixed levels, at position 7; a PPS beside it keeps the AVS sweep's line at row 3
        for (unsigned n = 0; n < 5; ++n) g_stub.caps.pdos[n] = pd::decode_pdo(((5000 + 4000 * n) / 50 << 10) | 325);
        g_stub.caps.pdos[5] = pd::decode_pdo(0xC0000000u | (210u << 17) | (33u << 8) | 100u);
        g_stub.caps.pdos[6] = pd::decode_pdo(0xE004B145u);
    } else {
        g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
        g_stub.caps.pdos[1] = pd::decode_pdo(0xC0000000u | (210u << 17) | (33u << 8) | 100u);
    }
    g_stub.caps.raw_header = 0x1181;
}

// Starts a PD-only run with the deep sweep armed (Tests -> Custom -> PD family -> the sweep's own line -> Review -> START).
void start_sweep(Ui& ui, uint32_t& t, bool avs, uint8_t rot, int ceiling_mv) {
    fixture(avs, ceiling_mv);
    ui.init(0); ui.set_rotation(rot, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    top(ui, 1200); move(ui, 4, 1200); press(ui, Button::Ok, 1200);   // Tests step: Custom...
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
    for (int fam : {kFamHuawei, kFamSamsung, kFamOppo, kFamQc}) { top(ui, 1200); move(ui, unsigned(fam), 1200); press(ui, Button::Ok, 1200); }
    top(ui, 1200); move(ui, kFamPd, 1200); ui.on_button(Button::Ok, ButtonEvent::Long, 1200);   // the PD family's entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    top(ui, 1200); move(ui, avs ? 3 : 2, 1200); press(ui, Button::Ok, 1200);   // the deep sweep's own line
    press(ui, Button::Back, 1200);
    top(ui, 1200); move(ui, kFamDone, 1200); press(ui, Button::Ok, 1200);   // Done: the cable step
    press(ui, Button::Ok, 1200);                                                // keep the cable: Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    press(ui, Button::Ok, 1300); press(ui, Button::Ok, 1400);   // START, confirm
    t = 1500;
}

// Ticks until the sweep row under test has `points` points behind it, or the run ends.
bool run_to_point(Ui& ui, uint32_t& t, unsigned points) {
    for (; t <= 2000000 && ui.charger_test_busy(); t += 100) {
        ui.tick(t);
        if (ui.charger_test_stage() == kRunningStage && ui.charger_test_results().sweep_live().n >= points) return true;
    }
    std::fprintf(stderr, "run_to_point(%u) gave up: t=%u stage=%d busy=%d n=%u idx=%zu/%zu\n", points, t, ui.charger_test_stage(), int(ui.charger_test_busy()),
                 ui.charger_test_results().sweep_live().n, ui.charger_test_results().index(), ui.charger_test_results().steps().size());
    return false;
}

// Ticks until the running step is the first (non-sweep) row of the plan, i.e. before the sweep row starts.
bool run_to_plan_row(Ui& ui, uint32_t& t) {
    for (; t <= 200000 && ui.charger_test_busy(); t += 100) {
        ui.tick(t);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() == kRunningStage && r.index() < r.steps().size() && r.steps()[r.index()].verdict == ct::Verdict::Running &&
            !ct::is_sweep(r.steps()[r.index()]))
            return true;
    }
    return false;
}

// The panel holds `text` drawn at (x, y) in `font` (the page paints a cached cell with exactly these pixels).
unsigned text_diff(const Font& font, int x, int y, std::string_view text, uint16_t color, int rows = 0) {
    std::array<uint16_t, size_t(LCD_WIDTH) * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_text({0, 0, 224, 240}, font, x, y, text, color, kBg);
    unsigned diff = 0;
    for (int yy = y; yy < y + (rows ? rows : font.height); ++yy)
        for (int xx = x; xx < x + text_width(font, text); ++xx) diff += actual[size_t(yy) * LCD_WIDTH + xx] != g_panel[size_t(yy) * LCD_WIDTH + xx];
    std::copy(actual.begin(), actual.end(), g_panel);
    return diff;
}
uint16_t at(int x, int y) { return g_panel[size_t(y) * LCD_WIDTH + x]; }
// The columns of a strip row (default the top one) holding `color`, inside the content (x < 224).
std::vector<int> columns_of(uint16_t color, int y = kStripY) {
    std::vector<int> v;
    for (int x = 0; x < 224; ++x) if (at(x, y) == color) v.push_back(x);
    return v;
}

// The gauge scale the page uses: one per run, 10 % above the highest target (the sweep row in flight adds the top of its swept
// range), rounded up to a whole volt, at least 6 V.
int32_t full_scale(const ct::Runner& r) {
    int32_t topmv = r.sweep_live().hi_mv;
    for (const auto& s : r.steps()) topmv = std::max<int32_t>(topmv, s.mv);
    return std::max<int32_t>((topmv * 110 / 100 + 999) / 1000 * 1000, 6000);
}

// One scene at rotation 0, pixel-checked: the badge, the gauge, the per-point line.
void check_scene(bool avs, int ceiling_mv, unsigned points, bool expect_out_of_band, const char* name) {
    Ui ui; uint32_t t;
    start_sweep(ui, t, avs, 0, ceiling_mv);
    const bool reached = run_to_point(ui, t, points);
    CHECK_EQ(reached, true);
    if (!reached) return;
    t += 600; ui.tick(t);
    const auto& r = ui.charger_test_results();
    const auto live = r.sweep_live();
    CHECK_EQ(live.active, true);
    if (!live.active || r.index() >= r.steps().size()) return;
    const auto& st = r.steps()[r.index()];
    // 1. the badge names the range being swept
    const std::string badge = step_label::label(st, r.index() ? &r.steps()[r.index() - 1] : nullptr).c_str();
    CHECK_STR(badge.c_str(), avs ? "AVS 9-20V" : "PPS 3.3-21V");
    CHECK_EQ(text_diff(kSmall, 6, kNameY, badge, kText), 0u);
    // 2. the range: two dotted 2 px lines at the swept range's ends, in the strip's top row; the setpoint marker (6 px, kFocus) and
    // the 2 px needle sit where the geometry says
    const int32_t vbus = ui.charger_test_results().sweep_live().last_measured_mv();
    (void)vbus;
    const auto ok = columns_of(kOk);
    CHECK_EQ(ok.size(), 4u);
    const auto geom = kit::range_geom(6, 210, 0, live.set_mv, live.lo_mv, live.hi_mv, full_scale(r));
    if (ok.size() == 4) {
        CHECK_EQ(ok[0], int(geom.lo)); CHECK_EQ(ok[1], int(geom.lo) + 1);
        CHECK_EQ(ok[2], int(geom.hi) - 1); CHECK_EQ(ok[3], int(geom.hi));
    }
    // the marker is 6 columns wide (mark-2 .. mark+3); the 2 px needle is drawn over it where the two meet (the reading follows the setpoint)
    const auto needle = columns_of(kText);
    CHECK_EQ(needle.size(), 2u);
    std::vector<int> want;
    for (int x = int(geom.mark) - 2; x <= int(geom.mark) + 3; ++x)
        if (std::find(needle.begin(), needle.end(), x) == needle.end()) want.push_back(x);
    CHECK_EQ(columns_of(kFocus) == want, true);
    // 3. the per-point line: the last recorded point's request, reading and error
    char b[ctsweep::kPointTextMax];
    const std::string line(b, ctsweep::format_point(live.last_set_mv(), live.last_measured_mv(), b));
    CHECK_EQ(line.empty(), false);
    CHECK_EQ(text_diff(kLabelFont, 6, kPointY, line, expect_out_of_band ? kCaution : kDim), 0u);
    CHECK_EQ(ctsweep::out_of_band(live.last_set_mv(), live.last_measured_mv()), expect_out_of_band);
    dump_panel(name);
}
}  // namespace

int main() {
    // PPS 3.3-21 V and SPR AVS 9-20 V on a converter that follows every request: the line reads err+0mV in the plain colour.
    check_scene(false, 36000, 12, false, "sweep_pps_rot0_early");
    check_scene(true, 36000, 12, false, "sweep_avs_rot0_early");
    // The marker moves along the range with the points: later in the sweep it sits further right.
    {
        Ui ui; uint32_t t;
        start_sweep(ui, t, false, 0, 36000);
        CHECK_EQ(run_to_point(ui, t, 8), true);
        t += 600; ui.tick(t);
        const auto early = columns_of(kFocus);
        CHECK_EQ(run_to_point(ui, t, 90), true);
        t += 600; ui.tick(t);
        const auto late = columns_of(kFocus);
        CHECK_EQ(early.size() >= 4u && early.size() <= 6u, true); CHECK_EQ(late.size() >= 4u && late.size() <= 6u, true);   // 6 less the needle's overlap
        if (!early.empty() && !late.empty()) CHECK_EQ(late.front() > early.front() + 20, true);
        dump_panel("sweep_pps_rot0_late");
    }
    // A source that holds 12 V while the request climbs: the points above fall out of the band; the line turns to the caution colour.
    check_scene(false, 12000, 100, true, "sweep_pps_rot0_out_of_band");
    // A non-sweep row keeps today's look: the "target" line and the pass band (a solid window, no dotted lines, no marker).
    {
        Ui ui; uint32_t t;
        start_sweep(ui, t, false, 0, 36000);
        const bool found = run_to_plan_row(ui, t);
        CHECK_EQ(found, true);
        if (!found) return test_report("test_ui_sweep_live");
        t += 600; ui.tick(t);
        const auto& r = ui.charger_test_results();
        const auto& st = r.steps()[r.index()];
        CHECK_EQ(r.sweep_live().active, false);
        dump_panel("plan_row_rot0");
        std::string target = "target ";
        char v[16]; std::snprintf(v, sizeof v, "%g", st.mv / 1000.0);
        target += v; target += "V";
        CHECK_EQ(text_diff(kLabelFont, 6, kTargetY, target, kDim, 10), 0u);   // today's line, at today's place
        for (int y = kPointY; y < kTargetY; ++y)   // nothing at the sweep line's place above it
            for (int x = 0; x < 224; ++x) CHECK_EQ(at(x, y), kBg);
        CHECK_EQ(columns_of(kFocus).size(), 0u);   // no marker
        const auto ok = columns_of(kOk, kStripY + 4);   // the pass band's window (rows 4..7 of the strip): one solid run of columns
        CHECK_EQ(ok.empty(), false);
        if (!ok.empty()) {   // one solid run, the 2 px needle (kText, over the window) aside
            const auto needle = columns_of(kText, kStripY + 4);
            int inside = 0;
            for (int x : needle) inside += x > ok.front() && x < ok.back();
            CHECK_EQ(int(ok.back() - ok.front() + 1), int(ok.size()) + inside);
        }
    }
    // Both ranges, both orientations, rendered for the contact sheet (the truncation gate runs on every scene through test_report).
    for (const bool avs : {false, true})
        for (const uint8_t rot : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)}) {
            Ui ui; uint32_t t;
            start_sweep(ui, t, avs, rot, 36000);
            CHECK_EQ(run_to_point(ui, t, 40), true);
            t += 600; ui.tick(t);
            char name[48];
            std::snprintf(name, sizeof name, "sweep_%s_rot%u", avs ? "avs" : "pps", unsigned(rot));
            dump_panel(name);
        }
    return test_report("test_ui_sweep_live");
}
