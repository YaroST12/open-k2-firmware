// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test Running view of a deep-sweep row (ROADMAP "Sweep live view"), driven end to end through the page on the host
// stubs: the badge names the range, the gauge shows the range as two dotted lines with a setpoint marker that moves point by
// point, and the signed ERROR (reading - setpoint) is the hero, with the act / set pair beside it (include/k2/ui/run_layout.hpp).
// A non-sweep row keeps today's look. The geometry is checked in both content mappings (portrait 224 x 240, landscape 240 x 224:
// a fill stretches, a text cell keeps its native height), and the error hero and the act / set block pixel by pixel in all four
// rotations. K2_DUMP=dir host-build/test_ui_sweep_live writes dir/k2_sweep_*.panel (tools/panel2png.py makes the PNGs).
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
#include "k2/ui/run_layout.hpp"
#include "k2/ui/sweep_hero.hpp"
#include "k2/ui/ui_layout.hpp"
#include "k2/ui/blend.hpp"

using namespace k2;
namespace ct = charger_test;
using A = ct::Algorithm;

namespace {
constexpr int kRunningStage = 5, kCustomStage = 14;
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamPd = 4, kFamQc = 5, kFamDone = 6;
// The Running view's gauge strip, the target line (rows 68..79, its last two rows share the strip's first two) and the name line.
constexpr int kStripY = runlay::kPlainRun.gauge_y, kTargetY = 68, kPointY = 66, kNameY = runlay::kPlainRun.name_y;
// A sweep row restacks the view (run_layout.hpp): the name moves up, the signed error is the hero (three 48 px cells at x18..89, a
// 24 px sign left of the first digit, "mV" / "V" on the baseline), the act / set pair stacks on the right, and the gauge and the
// grid move down to make room.
using runlay::kSweepRun;
constexpr int kSweepNameY = kSweepRun.name_y, kSweepStripY = kSweepRun.gauge_y, kHeroY = kSweepRun.hero_y;
constexpr int kHeroLeft = runlay::kHeroDigitsRight - 3 * runlay::kAdv48;

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
    top(ui, 1200); move(ui, 5, 1200); press(ui, Button::Ok, 1200);   // Tests step: Custom...
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
    for (int fam : {kFamHuawei, kFamSamsung, kFamOppo, kFamQc}) { top(ui, 1200); move(ui, unsigned(fam), 1200); press(ui, Button::Ok, 1200); }
    top(ui, 1200); move(ui, kFamPd, 1200); ui.on_button(Button::Ok, ButtonEvent::Long, 1200);   // the PD family's entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    // Both deep sweeps are off at the default level (Medium): a PPS scene turns the PPS sweep on on its line (row 2), an AVS
    // scene the AVS sweep on its line (row 3), so each scene's row is the only sweep of the run.
    top(ui, 1200); move(ui, avs ? 3 : 2, 1200); press(ui, Button::Ok, 1200);
    CHECK_EQ(ui.charger_test_selection() >> 30, avs ? 1u : 2u);   // bit 31 PPS sweep, bit 30 AVS sweep
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

// The panel holds `text` drawn at logical (x, y) in `font` (rotation 0: content offset +kOppRailThickness).
unsigned text_diff(const Font& font, int x, int y, std::string_view text, uint16_t color, int rows = 0) {
    std::array<uint16_t, size_t(LCD_WIDTH) * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_text({kOppRailThickness, 0, 208, 240}, font, x, y, text, color, kBg);
    unsigned diff = 0;
    const int px = kOppRailThickness + x;
    for (int yy = y; yy < y + (rows ? rows : font.height); ++yy)
        for (int xx = px; xx < px + text_width(font, text); ++xx) diff += actual[size_t(yy) * LCD_WIDTH + xx] != g_panel[size_t(yy) * LCD_WIDTH + xx];
    std::copy(actual.begin(), actual.end(), g_panel);
    return diff;
}
uint16_t at(int x, int y) { return g_panel[size_t(y) * LCD_WIDTH + kOppRailThickness + x]; }
// The logical columns of a strip row (default the top one) holding `color`, inside the content (x < 208).
std::vector<int> columns_of(uint16_t color, int y = kStripY) {
    std::vector<int> v;
    for (int x = 0; x < 208; ++x) if (at(x, y) == color) v.push_back(x);
    return v;
}

// The gauge scale the page uses: one per run, 10 % above the highest target (the sweep row in flight adds the top of its swept
// range), rounded up to a whole volt, at least 6 V.
int32_t full_scale(const ct::Runner& r) {
    int32_t topmv = r.sweep_live().hi_mv;
    for (const auto& s : r.steps()) topmv = std::max<int32_t>(topmv, s.mv);
    return std::max<int32_t>((topmv * 110 / 100 + 999) / 1000 * 1000, 6000);
}

void settle(Ui& ui, uint32_t& t) {
    for (int i = 0; i < 40 && ui.painting(); ++i) { t += 10; ui.tick(t); }
}

// ---- the error hero and the act / set block, in any rotation ---------------------------------------------------------------------

// Where the logical content sits on the panel in a rotation, and where a logical point lands: mapped(c) = c * extent / logical, the
// origin of a glyph cell only (a cell keeps its native size).
Rect content_box(uint8_t rot) { return compute_layout(orientation_for(rot).edge, kSlotCenters, kSlotLens, kPanelThickness).twin_content; }
int map_x(const Rect& b, int x) { return b.x + x * b.w / 208; }
int map_y(const Rect& b, int y) { return b.y + y * b.h / 240; }

// The number of pixels of the glyph cell 'text' (font, logical x / y, colour on ground) that differ on the panel; the cell sits at the
// mapped origin at native size, every row of it (a neighbour's stretched fill overwriting the last row shows here).
unsigned glyph_diff(const Rect& b, const Font& f, int x, int y, std::string_view text, uint16_t fg, int dx = 0) {
    const int px = map_x(b, x) + dx, py = map_y(b, y);
    uint16_t lut[16], line[240];
    make_blend_lut(fg, kBg, lut);
    unsigned d = 0;
    for (int row = 0; row < f.height; ++row) {
        const int n = render_row(f, text, row, lut, line, 240);
        for (int i = 0; i < n; ++i) d += line[i] != g_panel[size_t(py + row) * LCD_WIDTH + px + i];
    }
    if (d && std::getenv("K2_DBG")) std::printf("   glyph_diff font h%d x%d y%d '%.*s' fg %04x: %u\n", f.height, x, y, int(text.size()), text.data(), fg, d);
    return d;
}
struct Cell { int x0, y0, x1, y1; };
Cell cell_of(const Rect& b, const Font& f, int x, int y, std::string_view text, int dx = 0) {
    return {map_x(b, x) + dx, map_y(b, y), map_x(b, x) + dx + text_width(f, text), map_y(b, y) + f.height};
}
// Pixels of the panel rectangle [x0, x1) x [y0, y1) (logical x range, mapped rows: y0 / y1 are mapped panel rows) that are not ground
// and not inside one of the allowed cells.
unsigned stray_ink(int px0, int py0, int px1, int py1, const std::vector<Cell>& allowed) {
    unsigned n = 0;
    for (int y = py0; y < py1; ++y)
        for (int x = px0; x < px1; ++x) {
            if (g_panel[size_t(y) * LCD_WIDTH + x] == kBg) continue;
            bool in = false;
            for (const Cell& c : allowed) in = in || (x >= c.x0 && x < c.x1 && y >= c.y0 && y < c.y1);
            n += !in;
        }
    return n;
}

uint16_t grade_color(sweep_hero::Grade g) { return g == sweep_hero::Grade::Ok ? kOk : g == sweep_hero::Grade::Caution ? kCaution : kError; }
std::string cells6(int32_t mv) { char s[16]; std::snprintf(s, sizeof s, "%6.3f", mv / 1000.0); return s; }

// Everything the sweep row's hero and block paint, checked against a reading and a setpoint in the rotation of the content box 'b': each
// element's pixels equal its glyph cell, and nothing else is inked in the hero's band (x < the family label) or the block's.
// vbus < 0 or set == 0: the dim placeholders in their places.
void check_error_hero(const Rect& b, int32_t vbus, unsigned set, const char* what) {
    const bool have = vbus >= 0 && set != 0;
    const sweep_hero::Err e = sweep_hero::format(have ? vbus - int32_t(set) : 0);
    const uint16_t col = have ? grade_color(e.grade) : kDim;
    const bool volts = have && e.volts;
    // the digits right-aligned in the three cells; the blank cells are not compared (the sign stands in the first of them)
    const std::string digits = have ? std::string(e.digits, e.len) : std::string("--");
    const int digits_dx = (3 - int(digits.size())) * runlay::kAdv48;
    const std::string unit = volts ? "V " : "mV";
    unsigned bad = 0;
    std::vector<Cell> allowed;
    // dx: native pixels from the mapped origin (a Field lays its cells out from its own mapped origin)
    auto elem = [&](const Font& f, int x, int y, const std::string& s, uint16_t fg, int dx = 0) {
        bad += glyph_diff(b, f, x, y, s, fg, dx);
        allowed.push_back(cell_of(b, f, x, y, s, dx));
    };
    // the hero
    elem(kBig, kHeroLeft, kHeroY, digits, col, digits_dx);
    elem(kMid, runlay::kHeroUnitX, kHeroY + runlay::kH48 - runlay::kA24, unit, col);
    if (have && e.sign) {
        const char s1[2] = {e.sign, 0};
        elem(kMid, sweep_hero::sign_x(runlay::kHeroDigitsRight, runlay::kAdv48, runlay::kAdv24, e.len, runlay::kSignGap), kHeroY + runlay::kSignDy, s1, col);
    }
    const int band_y0 = map_y(b, kHeroY);
    const unsigned hero_stray = stray_ink(map_x(b, 0), band_y0, map_x(b, runlay::kBlockLabelX), band_y0 + runlay::kH48, allowed);
    // the block: act over set, the labels and the units on the values' baselines
    allowed.clear();
    const int dy = runlay::kA24 - runlay::kA12;
    elem(kLabelFont, runlay::kBlockLabelX, runlay::kActY + dy, "ac", kDim);
    elem(kMid, runlay::kBlockValueX, runlay::kActY, vbus >= 0 ? cells6(vbus) : std::string("    --"), vbus >= 0 ? kVolt : kDim);
    elem(kLabelFont, runlay::kBlockUnitX, runlay::kActY + dy, "V", vbus >= 0 ? kVolt : kDim);
    elem(kLabelFont, runlay::kBlockLabelX, runlay::kSetY + dy, "st", kDim);
    elem(kMid, runlay::kBlockValueX, runlay::kSetY, set ? cells6(int32_t(set)) : std::string("    --"), set ? kText : kDim);
    elem(kLabelFont, runlay::kBlockUnitX, runlay::kSetY + dy, "V", kDim);
    const unsigned block_stray = stray_ink(map_x(b, runlay::kBlockLabelX), map_y(b, runlay::kActY),
                                           b.x + b.w, map_y(b, runlay::kSetY) + runlay::kH24, allowed);
    if (bad || hero_stray || block_stray)
        std::printf("  %s: vbus %d set %u: %u pixels differ, %u stray in the hero band, %u in the block\n", what, int(vbus), set, bad, hero_stray, block_stray);
    CHECK_EQ(bad, 0u);
    CHECK_EQ(hero_stray, 0u);
    CHECK_EQ(block_stray, 0u);
}

// Sets the reading to (the live setpoint + delta), lets the page paint it, and returns the setpoint it was painted against (0 when the
// sweep moved on to the next point during the paint: the caller asks again).
unsigned inject(Ui& ui, uint32_t& t, int32_t delta) {
    for (int tries = 0; tries < 6; ++tries) {
        const unsigned set = ui.charger_test_results().sweep_live().set_mv;
        g_stub.pm.vbus_mv = int32_t(set) + delta;
        t += 600; ui.tick(t); settle(ui, t);
        if (ui.charger_test_results().sweep_live().set_mv == set && g_stub.pm.vbus_mv == int32_t(set) + delta) return set;
    }
    return 0;
}

// One scene at rotation 0, pixel-checked: the badge, the gauge, the per-point line.
void check_scene(bool avs, int ceiling_mv, unsigned points, bool expect_out_of_band, const char* name) {
    Ui ui; uint32_t t;
    start_sweep(ui, t, avs, 0, ceiling_mv);
    const bool reached = run_to_point(ui, t, points);
    CHECK_EQ(reached, true);
    if (!reached) return;
    t += 600; ui.tick(t); settle(ui, t);
    const auto& r = ui.charger_test_results();
    const auto live = r.sweep_live();
    CHECK_EQ(live.active, true);
    if (!live.active || r.index() >= r.steps().size()) return;
    const auto& st = r.steps()[r.index()];
    // 1. the badge names the range being swept
    const std::string badge = step_label::label(st, r.index() ? &r.steps()[r.index() - 1] : nullptr).c_str();
    CHECK_STR(badge.c_str(), avs ? "AVS 9-20V" : "PPS 5-21V");   // the sweep's part from 5 V up runs first (split 2026-11-14)
    CHECK_EQ(text_diff(kSmall, 6, kSweepNameY, badge, kText), 0u);
    // 2. the range: two dotted 2 px lines at the swept range's ends, in the strip's top row; the setpoint marker (6 px, kFocus) and
    // the 2 px needle sit where the geometry says
    const int32_t vbus = ui.charger_test_results().sweep_live().last_measured_mv();
    (void)vbus;
    const auto ok = columns_of(kOk, kSweepStripY);
    CHECK_EQ(ok.size(), 4u);
    const auto geom = kit::range_geom(6, 196, 0, live.set_mv, live.lo_mv, live.hi_mv, full_scale(r));
    if (ok.size() == 4) {
        CHECK_EQ(ok[0], int(geom.lo)); CHECK_EQ(ok[1], int(geom.lo) + 1);
        CHECK_EQ(ok[2], int(geom.hi) - 1); CHECK_EQ(ok[3], int(geom.hi));
    }
    // the marker is 6 columns wide (mark-2 .. mark+3); the 2 px needle is drawn over it where the two meet (the reading follows the setpoint)
    const auto needle = columns_of(kText, kSweepStripY);
    CHECK_EQ(needle.size(), 2u);
    std::vector<int> want;
    for (int x = int(geom.mark) - 2; x <= int(geom.mark) + 3; ++x)
        if (std::find(needle.begin(), needle.end(), x) == needle.end()) want.push_back(x);
    CHECK_EQ(columns_of(kFocus, kSweepStripY) == want, true);
    // 3. the hero is the signed error of the reading against the setpoint the last Request asked for (to the mV, no deadband), graded by
    // size; the act / set pair stacks beside it
    const int32_t shown = g_stub.pm.vbus_mv;
    check_error_hero(content_box(0), shown, live.set_mv, name);
    // an out-of-band point (the output holds 12 V while the request climbs) is red; a following converter's error is +0 in green
    const auto err = sweep_hero::format(shown - int32_t(live.set_mv));
    CHECK_EQ(err.grade == sweep_hero::Grade::Error, expect_out_of_band);
    CHECK_EQ(ctsweep::out_of_band(live.last_set_mv(), live.last_measured_mv()), expect_out_of_band);
    dump_panel(name);
}
}  // namespace

int main() {
    // The geometry: the real fonts are what run_layout.hpp's constants say, and no two boxes of the sweep row overlap in either mapping.
    CHECK_EQ(kLabelFont.height, runlay::kH12); CHECK_EQ(kLabelFont.ascent, runlay::kA12); CHECK_EQ(kLabelFont.adv, runlay::kAdv12);
    CHECK_EQ(kSmall.height, runlay::kH16); CHECK_EQ(kSmall.ascent, runlay::kA16); CHECK_EQ(kSmall.adv, runlay::kAdv16);
    CHECK_EQ(kMid.height, runlay::kH24); CHECK_EQ(kMid.ascent, runlay::kA24); CHECK_EQ(kMid.adv, runlay::kAdv24);
    CHECK_EQ(kBig.height, runlay::kH48); CHECK_EQ(kBig.ascent, runlay::kA48); CHECK_EQ(kBig.adv, runlay::kAdv48);
    for (int n = 0; n <= 3; ++n) {
        CHECK_EQ(runlay::sweep_clear(n, 208, 240), true);   // portrait (rot 0, 2)
        CHECK_EQ(runlay::sweep_clear(n, 240, 208), true);   // landscape (rot 1, 3)
    }
    // the walk itself bites: the layout of dc7aec88 (name y48, a 30 px hero at y67, a font-16 line at y98) overlaps in landscape only, which
    // is why its rot 0 test passed while the hero's bottom ink row was gone on a device in the default (gravity) rotation
    {
        using runlay::Box;
        const Box old_name{"name", 6, 48, 88, 16, true}, old_hero{"hero", 52, 67, 144, 30, true}, old_line{"line", 6, 98, 208, 16, true};
        CHECK_EQ(runlay::overlap(old_hero, old_line, 224, 240), false);
        CHECK_EQ(runlay::overlap(old_hero, old_line, 240, 224), true);
        CHECK_EQ(runlay::overlap(old_name, old_hero, 240, 224), false);
        // and the sign's gap is needed: without it a one-digit error's sign stands into its digit cell in landscape
        const Box bare{"sign", int16_t(sweep_hero::sign_x(runlay::kHeroDigitsRight, runlay::kAdv48, runlay::kAdv24, 1)), kHeroY + runlay::kSignDy, runlay::kAdv24, runlay::kH24, true};
        CHECK_EQ(runlay::overlap(bare, runlay::digits_box(1), 240, 208), true);
        CHECK_EQ(runlay::overlap(bare, runlay::digits_box(1), 208, 240), false);
        CHECK_EQ(runlay::overlap(runlay::sign_box(1), runlay::digits_box(1), 240, 208), false);
    }
    // PPS 3.3-21 V and SPR AVS 9-20 V on a converter that follows every request: the error reads 0, green, in the hero.
    check_scene(false, 36000, 12, false, "sweep_pps_rot0_early");
    check_scene(true, 36000, 12, false, "sweep_avs_rot0_early");
    // The marker moves along the range with the points: later in the sweep it sits further right.
    {
        Ui ui; uint32_t t;
        start_sweep(ui, t, false, 0, 36000);
        CHECK_EQ(run_to_point(ui, t, 8), true);
        t += 600; ui.tick(t);
        const auto early = columns_of(kFocus, kSweepStripY);
        CHECK_EQ(run_to_point(ui, t, 90), true);
        t += 600; ui.tick(t);
        const auto late = columns_of(kFocus, kSweepStripY);
        CHECK_EQ(early.size() >= 4u && early.size() <= 6u, true); CHECK_EQ(late.size() >= 4u && late.size() <= 6u, true);   // 6 less the needle's overlap
        if (!early.empty() && !late.empty()) CHECK_EQ(late.front() > early.front() + 20, true);
        dump_panel("sweep_pps_rot0_late");
    }
    // A source that holds 12 V while the request climbs: the points above fall out of the band; the error hero turns red.
    check_scene(false, 12000, 100, true, "sweep_pps_rot0_out_of_band");
    // A non-sweep row keeps today's look: the "target" line and the pass band (a solid window, no dotted lines, no marker).
    {
        Ui ui; uint32_t t;
        start_sweep(ui, t, false, 0, 36000);
        const bool found = run_to_plan_row(ui, t);
        CHECK_EQ(found, true);
        if (!found) return test_report("test_ui_sweep_live");
        t += 600; ui.tick(t); settle(ui, t);
        const auto& r = ui.charger_test_results();
        const auto& st = r.steps()[r.index()];
        CHECK_EQ(r.sweep_live().active, false);
        dump_panel("plan_row_rot0");
        std::string target = "target ";
        char v[16]; std::snprintf(v, sizeof v, "%g", st.mv / 1000.0);
        target += v; target += "V";
        CHECK_EQ(text_diff(kLabelFont, 6, kTargetY, target, kDim, 10), 0u);   // today's line, at today's place
        for (int y = kPointY; y < kTargetY; ++y)   // nothing at the sweep line's place above it
            for (int x = 0; x < 208; ++x) CHECK_EQ(at(x, y), kBg);
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
    // Back on a plain row after the sweep: today's layout returns whole (target line, pass band, the 4-cell hero), and nothing of
    // the sweep's stack is left behind (the hero's lower rows, the line, the gauge's old place).
    {
        Ui ui; uint32_t t;
        start_sweep(ui, t, false, 0, 36000);
        CHECK_EQ(run_to_point(ui, t, 8), true);
        bool back = false;
        for (; t <= 2000000 && ui.charger_test_busy() && !back; t += 100) {
            ui.tick(t);
            const auto& r = ui.charger_test_results();
            back = ui.charger_test_stage() == kRunningStage && r.index() < r.steps().size() && r.steps()[r.index()].verdict == ct::Verdict::Running &&
                   !r.sweep_live().active;
        }
        CHECK_EQ(back, true);
        if (back) {
            t += 600; ui.tick(t); settle(ui, t);
            const auto& r = ui.charger_test_results();
            const auto& st = r.steps()[r.index()];
            std::string target = "target ";
            char v[16]; std::snprintf(v, sizeof v, "%g", st.mv / 1000.0);
            target += v; target += "V";
            CHECK_EQ(text_diff(kLabelFont, 6, kTargetY, target, kDim, 10), 0u);
            CHECK_EQ(columns_of(kOk, kStripY + 4).empty(), false);
            for (int y = kPointY; y < kTargetY; ++y) for (int x = 0; x < 208; ++x) CHECK_EQ(at(x, y), kBg);
            for (int y = 65; y < 78; ++y) for (int x = 114; x < 208; ++x) CHECK_EQ(at(x, y), kBg);   // the sweep hero's lower rows
            for (int x = 0; x < 208; ++x) CHECK_EQ(at(x, 28), kBg);
            dump_panel("plan_row_after_sweep_rot0");
        }
    }
    // Both ranges, all four rotations: the error hero walks through every look (the sign moving with the digit count, a colour change
    // repainting the field whole, the mV -> V unit switch and back, zero without a sign) and after each the hero, the act / set block and
    // the ground around them equal the glyph cells exactly, in every row of every cell (the landscape rotations map the gauge's fill onto
    // the row under the hero's text cell). Each look is dumped for the contact sheet (the truncation gate runs through test_report).
    struct Look { int32_t delta; const char* tag; };
    const Look looks[] = {{50, "ok_p050"}, {123, "amber_p123"}, {5, "ok_p005"}, {0, "ok_zero"}, {-7, "ok_m007"}, {-400, "red_m400"},
                          {50, "ok_p050b"}, {-1200, "red_m1v2"}, {2, "ok_p002"}, {-100, "ok_m100"}, {-101, "amber_m101"}, {300, "amber_p300"},
                          {301, "red_p301"}, {12000, "red_p12v"}};
    for (const bool avs : {false, true})
        for (const uint8_t rot : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)}) {
            Ui ui; uint32_t t;
            start_sweep(ui, t, avs, rot, 36000);
            CHECK_EQ(run_to_point(ui, t, 40), true);
            const Rect b = content_box(rot);
            for (const Look& l : looks) {
                const unsigned set = inject(ui, t, l.delta);
                CHECK_EQ(set != 0, true);
                if (!set) continue;
                char name[64];
                std::snprintf(name, sizeof name, "%s rot %u %s", avs ? "avs" : "pps", unsigned(rot), l.tag);
                check_error_hero(b, int32_t(set) + l.delta, set, name);
                std::snprintf(name, sizeof name, "sweep_%s_rot%u_%s", avs ? "avs" : "pps", unsigned(rot), l.tag);
                dump_panel(name);
            }
        }
    return test_report("test_ui_sweep_live");
}
