// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <array>
#include <algorithm>
#include <cstring>
#include "k2/pd/pdtrace.hpp"
#include <string>
#include "test_util.h"
#include "k2/core/scratch.hpp"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/capture.hpp"
#include "fake_remote_backend.hpp"
#include "k2/pd/pd_source.hpp"
#include "k2/pd/emk_spoof.hpp"
#include "k2/chargertest/ct_cable.hpp"
#include "k2/pd/ra_guard.hpp"
#include "k2/pd/boot_ra.hpp"
#include "k2/chargertest/result_layout.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/pd/emrec.hpp"
#include "k2/chargertest/run_events.hpp"
#include "k2/chargertest/report_card_hw.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_setup_page.hpp"
#include "k2/ui/text.hpp"
#include "k2/ui/trunc.hpp"
#include "overlap_log.h"
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/spec_text.hpp"
#include "k2/ui/run_layout.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

using namespace k2;
namespace ct = charger_test;
using A = ct::Algorithm;
static constexpr uint8_t kPdOnly[] = {uint8_t(ct::Unit::Pd), uint8_t(ct::Unit::PdLow)};   // Cmd::ChargerUnits payload
static void fixture() {
    g_stub = {}; pd::set_fake(0);
    g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
    g_stub.fc_answers = true;   // a legacy unit only gets a row once the charger answered its handshake
    g_stub.caps.valid = true; g_stub.caps.num_pdos = 1; g_stub.caps.active_cc = 1; g_stub.caps.spec_rev = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.raw_header = 0x1181;
}
static void press(Ui& ui, Button b, uint32_t now = 1200) { ui.on_button(b, ButtonEvent::Short, now); }
// The algorithm part of the draft selection: charger_test_selection() also carries the deep-sweep flags (bit 31 PPS, bit 30
// AVS) and the level (bits 28..29), which are Options, not algorithms.
static uint32_t algos(const Ui& ui) { return ui.charger_test_selection() & 0x0FFFFFFFu; }
static void move(Ui& ui, unsigned n, uint32_t now = 1200) { for (unsigned i = 0; i < n; ++i) press(ui, Button::Plus, now); }
static void top(Ui& ui, uint32_t now = 1200) { for (unsigned i = 0; i < ct::kMaxSteps; ++i) press(ui, Button::Minus, now); }
static void settle(Ui& ui, uint32_t now = 1200) {
    for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(now);
    CHECK_EQ(ui.painting(), false);
}
static void open(Ui& ui) {
    ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); settle(ui, 1100);
    if (ui.context().screen.x != 16) std::fprintf(stderr, "OPEN screen x=%d expected=16 content=%d\n", ui.context().screen.x, ui.context().content.w);
}
// A preset pick advances (there is no Next row): the first preset ("All tests", row 1) goes on to the Cable step.
static void cable(Ui& ui, uint32_t now = 1200) {
    top(ui, now); move(ui, 1, now); press(ui, Button::Ok, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 2);
}
// A cable pick selects and advances: the cursor starts on the cable in force, so one OK keeps it and goes on to Review.
static void review(Ui& ui, uint32_t now = 1200) {
    cable(ui, now); press(ui, Button::Ok, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_cursor(), 0);   // START (row 0)
}
constexpr int kCustomStage = 14;   // Stage::Custom, the per-family / per-algorithm list under Tests step "Custom..." (not a numbered step)
// Tests step rows (0-based): 0 Depth, 1 All tests, 2 PD only, 3 Legacy only, 4 Vendor modes, 5 Custom..., 6 Last results (after a run).
constexpr int kDepthRow = 0, kAllRow = 1, kPdRow = 2, kLegacyRow = 3, kVendorRow = 4, kCustomRow = 5;
// Custom list rows (the families in run order): 0 Huawei/Honor, 1 Samsung, 2 OPPO/vivo, 3 Xiaomi/Redmi, 4 USB PD,
// 5 QC/UFCS, 6 Done (to the Cable step). Short OK toggles a family, long OK opens its entries.
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamXiaomi = 3, kFamPd = 4, kFamQc = 5, kFamDone = 6;
static void custom(Ui& ui, uint32_t now = 1200) {   // from the Tests step: open the Custom list
    top(ui, now); move(ui, kCustomRow, now); press(ui, Button::Ok, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
}
static void long_ok(Ui& ui, uint32_t now = 1200) { ui.on_button(Button::Ok, ButtonEvent::Long, now); }
// Open a family's entries from the Custom list (long OK on its row).
static void open_group(Ui& ui, int family, uint32_t now = 1200) {
    top(ui, now); move(ui, family, now); long_ok(ui, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 1);
}
// From the Custom list: Done goes on to the Cable step, one OK keeps the cable and goes on to Review.
static void custom_done_review(Ui& ui, uint32_t now = 1200) {
    top(ui, now); move(ui, kFamDone, now); press(ui, Button::Ok, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Ok, now); settle(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 4);
}
// Only VIVO armed (a short legacy run): from the Tests step, clear the five toggleable families, open OPPO's entries, pick the
// fifth one, then Back, Done, the cable and Review.
static void only_vivo_review(Ui& ui, uint32_t now = 1200) {
    custom(ui, now);
    for (int fam : {kFamHuawei, kFamSamsung, kFamOppo, kFamPd, kFamQc}) { top(ui, now); move(ui, fam, now); press(ui, Button::Ok, now); }
    CHECK_EQ(algos(ui), 0u);
    open_group(ui, kFamOppo, now); move(ui, 4, now); press(ui, Button::Ok, now);   // OPPO group: Next, VOOC, SVOOC, SVOOC65, VIVO
    CHECK_EQ(algos(ui), ct::bit(A::Vivo));
    press(ui, Button::Back, now);
    custom_done_review(ui, now);
}
constexpr uint32_t kPdMask = ct::kDefaultAlgorithms & (ct::kPdFixedAlgorithms | ct::kPpsAlgorithms);
constexpr uint32_t kLegacyMask = ct::kDefaultAlgorithms & ~(ct::kPdFixedAlgorithms | ct::kPpsAlgorithms);
constexpr int kRefuseStage = 12;   // Stage::Refuse, the Ra guard's refusal of a spoof START
constexpr int kWaitStage = 13;     // Stage::Waiting, START's wait for the charger (never blocks the UI)
constexpr int kRunningStage = 5;   // Stage::Running
constexpr int kResumeStage = 9;    // Stage::Resume
constexpr int kRefuseY = k2::runlay::kRefuseRowsY;      // the Refuse stage's answer rows start here (under the alert band and its two lines)
// Cable rows (the one identity list): 0 Off, 1..3 the three built-in chips (ids 1, 2, 4), 4.. the saved clones.
constexpr int kOffRow = 0, kBuiltinRow = 1, kSavedRow = 4;
// Pick a cable-identity row from the top of the Cable step: it selects the choice and advances to Review (nothing is armed).
static void pick_row(Ui& ui, int row, uint32_t now = 1200) {
    top(ui, now); move(ui, row, now); press(ui, Button::Ok, now); settle(ui, now);
}
// START is two OK presses on Review. The first opens the confirmation ("OK again within 5 s"): nothing is touched, no Rd,
// no identity, no guard. The second confirms it: the guard, the identity, then the Rd and the wait for the charger as the
// LAST stage before the tests; a START that needs the Rd sits in Waiting and goes on by itself, one that does not
// (VBUS already up) starts the run at once.
static void start_press(Ui& ui, uint32_t now = 1200) { press(ui, Button::Ok, now); }
static void confirm_press_nowait(Ui& ui, uint32_t now = 1200) { press(ui, Button::Ok, now); }
// The run's journal is written in pieces, one file per tick (the Runner waits for it in Baseline): ticks until the header and the
// meta record are on flash. Returns the ticks it took.
static unsigned settle_arm(Ui& ui, uint32_t now) {
    unsigned n = 0;
    while (g_stub.journal_arm_open && n < 400) { ui.tick(now); ++n; }
    return n;
}
// What the main loop does while START waits: the PD engine's service() every pass and the UI tick every 20 ms.
// `hook(t)` runs before each tick (a charger answering at a given time). g_wait_end = the time the wait left Waiting.
static uint32_t g_wait_end = 0;
static void finish_wait(Ui& ui, uint32_t now, void (*hook)(uint32_t) = nullptr, uint32_t limit = 40000) {
    uint32_t t = now;
    while (ui.charger_test_stage() == kWaitStage && t < now + limit) {
        t += 20;
        fusb302::service();
        if (hook) hook(t);
        ui.tick(t);
    }
    g_wait_end = t;
}
// The confirming press, then the wait out: the run (or a refusal on Review) is what the caller sees next.
static void confirm_press(Ui& ui, uint32_t now = 1200) {
    confirm_press_nowait(ui, now);
    finish_wait(ui, now);
}
// Both presses and the wait: the whole START.
static void start_run(Ui& ui, uint32_t now = 1200) {
    start_press(ui, now);
    confirm_press(ui, now + 100);
}
// Nothing of START has reached the hardware: no Rd, no Ra, no identity, no e-marker scan.
static void nothing_armed_stub() {
    CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.rd_run_changes, 0);
    CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.scans, 0);
}
static void no_operations() {
    CHECK_EQ(g_stub.requests + g_stub.releases + g_stub.fc_starts + g_stub.fc_retargets, 0);
    CHECK_EQ(g_stub.rescans + g_stub.scans + g_stub.lib_saves + g_stub.spoof_arms, 0);
}
static void read_results(Ui& ui) {
    using namespace remote;
    const auto& runner = ui.charger_test_results();
    const auto state = runner.state();
    const auto revision = runner.revision();
    const auto index = runner.index();
    const auto* storage = runner.steps().data();
    const auto requests = g_stub.requests, releases = g_stub.releases, fc_starts = g_stub.fc_starts;
    Fake backend;
    backend.test_results = charger_test_snapshot(runner, ui.charger_test_selection(), ui.charger_test_stage());
    std::array<uint8_t, 64> req{0xA5, 0x4B, 0x0B, 42, 1, 2}, rsp{};
    CHECK_EQ(handle_request(req, backend, rsp), true);
    CHECK_EQ(rsp[4], 0); CHECK_EQ(rsp[5], 42);
    CHECK_EQ(rsp[15], static_cast<uint8_t>(state));
    req[4] = 10;
    for (unsigned n = 0; n < 4; ++n) {
        req[7 + n] = runner.run_id() >> (n * 8);
        req[11 + n] = revision >> (n * 8);
    }
    for (unsigned n = 0; n < runner.steps().size(); ++n) {
        req[6] = n;
        CHECK_EQ(handle_request(req, backend, rsp), true);
        CHECK_EQ(rsp[4], 0);
        CHECK_EQ(rsp[19], n);
    }
    CHECK_EQ(runner.state(), state); CHECK_EQ(runner.revision(), revision); CHECK_EQ(runner.index(), index);
    CHECK_EQ(runner.steps().data() == storage, true);
    CHECK_EQ(g_stub.requests, requests); CHECK_EQ(g_stub.releases, releases); CHECK_EQ(g_stub.fc_starts, fc_starts);
    CHECK_EQ(backend.fc_calls + backend.releases + backend.resets + backend.store_calls, 0);
}
static void test_confirmation_and_capture() {
    fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.list_state().count, 6);     CHECK_EQ(ui.list_state().visible, 8);   // Depth, four presets, Custom... (no run to view yet)
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);   // the OEM units come first in the default order: keep the timeline short
    review(ui);
    start_press(ui, 1300);
    CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.requests, 0);
    ui.tick(6500); // confirmation expires
    start_press(ui, 6600);
    CHECK_EQ(ui.charger_test_busy(), false);
    ui.on_button(Button::Ok, ButtonEvent::Short, 6700);
    CHECK_EQ(ui.charger_test_busy(), true);
    CHECK_EQ(g_stub.requests, 0);
    read_results(ui); // active baseline: remote retrieval cannot request the first target
    CHECK_EQ(capture_begin(ui), true);
    for (int n = 0; n < kCaptureStrips; ++n) capture_strip(ui, n, 7000 + n * 100);
    capture_end(ui, 9500);
    CHECK_EQ(g_stub.requests, 0); // screenshot rendering cannot advance or start the runner
    // The Pd unit detaches and attaches before its first row: tick until the first request.
    uint32_t now = 6800;
    for (; now <= 20000 && g_stub.requests < 1; now += 100) ui.tick(now);
    CHECK_EQ(g_stub.requests, 1); CHECK_EQ(g_stub.last_confirmed, true);
    read_results(ui); // requesting
    CHECK_EQ(ui.trigger({1, 5000, 1000}, now + 100, true).err, pd::Err::Busy);
    CHECK_EQ(ui.run_action(Action::PdRescan, now + 100), false);
    CHECK_EQ(g_stub.rescans, 0);
    ui.goto_page(static_cast<uint8_t>(Page::Home), now + 200);
    CHECK_EQ(g_stub.releases > 0, true);
    read_results(ui); // releasing
    for (now += 300; now <= 400000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    read_results(ui); // finished stopped results
}
static void test_fake_offers_refused() {
    fixture(); Ui ui; open(ui);
    ui.debug_fake_pdos(7, 1100);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1200);
    review(ui, 1300);
    start_press(ui, 1400);
    start_press(ui, 1500);
    CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.requests, 0);
    CHECK_EQ(g_stub.fc_starts, 0);
    ui.debug_fake_pdos(0, 1600);
}
static void test_group_selection_and_empty_refusal() {
    fixture(); Ui ui; open(ui);
    custom(ui); CHECK_EQ(ui.charger_test_cursor(), 0);
    move(ui, kFamPd); long_ok(ui);   // long OK opens the family's entries (the USB PD family has two: PD fixed, PPS)
    CHECK_EQ(ui.charger_test_stage(), 1); CHECK_EQ(ui.list_state().count, 2);
    press(ui, Button::Ok);           // the first entry, PD fixed, off
    CHECK_EQ(ui.charger_test_selection() & ct::bit(A::Pd2Fixed), 0);
    press(ui, Button::Ok); CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    move(ui, 1); CHECK_EQ(ui.list_state().offset, 0);
    press(ui, Button::Ok);           // PPS off
    CHECK_EQ(ui.charger_test_selection() & ct::kPpsAlgorithms, 0);
    press(ui, Button::Ok); CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    long_ok(ui);                     // long OK on an entry page toggles the whole group
    CHECK_EQ(ui.charger_test_selection() & (ct::kPdFixedAlgorithms | ct::kPpsAlgorithms), 0);
    long_ok(ui); CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), kCustomStage); CHECK_EQ(ui.charger_test_cursor(), kFamPd);
    // Short OK on a family toggles all of it: clear every family that can be toggled (Xiaomi rides the PD unit).
    top(ui);
    for (const int f : {kFamHuawei, kFamSamsung, kFamOppo, kFamPd, kFamQc}) { top(ui); move(ui, f); press(ui, Button::Ok); }
    CHECK_EQ(algos(ui), 0);
    top(ui); move(ui, kFamDone); press(ui, Button::Ok);   // Done: the Cable step
    CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 4);
    start_press(ui, 1300); start_press(ui, 1400);
    CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(ui.charger_test_results().run_id(), 0);
    no_operations();
    press(ui, Button::Back); press(ui, Button::Back);   // Review -> Cable -> Tests
    CHECK_EQ(ui.charger_test_stage(), 0);
    top(ui); move(ui, kAllRow); press(ui, Button::Ok);   // the "All tests" preset brings the defaults back
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
}
// Long OK on a Custom family opens its entries; on an entries page it toggles the whole group: all selected -> none,
// otherwise -> all of it.
static void test_long_ok_toggles_a_group() {
    fixture(); Ui ui; open(ui);
    const auto long_ok = [&] { ui.on_button(Button::Ok, ButtonEvent::Long, 1200); };
    const uint32_t qc = ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Qc3Plus);
    top(ui); move(ui, kLegacyRow); long_ok();                 // a Tests-step preset row: long OK does nothing at all
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms); CHECK_EQ(ui.charger_test_stage(), 0);
    custom(ui);
    top(ui); move(ui, kFamQc); long_ok();                     // on a family row long OK opens its entries
    CHECK_EQ(ui.charger_test_stage(), 1); CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    long_ok();                                                // inside the entries: all on -> all off
    CHECK_EQ(ui.charger_test_selection() & qc, 0); CHECK_EQ(ui.charger_test_stage(), 1);
    press(ui, Button::Ok);                                    // the first entry, QC2, back on
    CHECK_EQ(ui.charger_test_selection() & qc, ct::bit(A::Qc2));
    long_ok();                                                // some on -> all on
    CHECK_EQ(ui.charger_test_selection() & qc, qc);
    long_ok(); CHECK_EQ(ui.charger_test_selection() & qc, 0);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_cursor(), kFamQc);
    top(ui); move(ui, kFamDone); long_ok();                   // the Done row ignores a long OK
    CHECK_EQ(ui.charger_test_stage(), kCustomStage); CHECK_EQ(ui.charger_test_selection() & qc, 0);
    top(ui); move(ui, kFamXiaomi); long_ok();                 // Xiaomi has no entries of its own: long OK ignored
    CHECK_EQ(ui.charger_test_stage(), kCustomStage); CHECK_EQ(ui.charger_test_selection() & qc, 0);
    // Class B (20 V) belongs to the QC group: it was on by default and went off with the group.
    CHECK_EQ(ui.charger_test_selection() & (ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB)), 0);
    top(ui); move(ui, kFamQc); long_ok(); long_ok();          // open the QC entries, then the whole group, Class B included, on
    CHECK_EQ(ui.charger_test_selection() & (ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB)),
             ct::bit(A::Qc2ClassB) | ct::bit(A::Qc3ClassB));
    no_operations();
}
static emlib::Entry saved(unsigned seq) {
    const uint32_t v[5] = {0xFF00A041, 0x1800203C, 123, seq << 16, 0x00182052};
    emlib::Entry e;
    e.seq = seq; e.version = 1; e.rec = emrec::make(v, 2);
    std::snprintf(e.name.data(), e.name.size(), "Host identity %u", seq);
    return e;
}
// Option B: at rotation 0 the content surface sits kOx px in from the panel edge (the button strip is on the left); every
// content-relative x below is shifted by it. PANEL[] reads the panel in content coordinates.
static const int kOx = compute_layout(orientation_for(0).edge, kSlotCenters, kSlotLens, kPanelThickness).twin_content.x;
#define PANEL (g_panel + kOx)
static const Surface kContent0 = {int16_t(kOx), 0, kTwinContentW, kTwinContentH};
// Kit RowList look: rows of kit::kRowH from `y0` (the body top), the 16 px text at kit::kTextX, the marker at kit::kMarkX, the
// focus rail x 0..2. A focused row with a note opens kit::kFocusExtra more (`open` = that row; the rows below it shift down).
static int row_top(int row, int open = -1, int y0 = kit::kBodyY) {
    return y0 + row * kit::kRowH + (open >= 0 && row > open ? kit::kFocusExtra : 0);
}
static int wizard_text_y(int row, int open = -1, int y0 = kit::kBodyY) {
    return row_top(row, open, y0) + kit::row_text_y(kit::kRowH, kSmall.height);
}
constexpr int kSavedRoom = kit::kTailX - 40 - 8 - kit::kTextX;   // a saved clone's name: the room before the dim "saved" tail
constexpr int kNoTailRoom = kTwinContentW - ListView::kBarW - kit::kTextX;   // a row with no rating column and no tail: up to the scroll lane
constexpr int kNextPillRoom = kit::kTailX - pill_width(text_cells("NEXT")) - kit::kTailGap - kit::kTextX;   // room beside a NEXT pill
static void check_text_at(const Font& font, int y, std::string_view text, uint16_t color, uint16_t bg, int x0 = kit::kTextX) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_text(kContent0, font, x0, y, text, color, bg);
    unsigned diff = 0;
    int first = -1;
    for (int yy = y; yy < y + font.height; ++yy)
        for (int x = x0; x < x0 + text_width(font, text); ++x)
            if (PANEL[yy * LCD_WIDTH + x] != bg &&
                actual[yy * LCD_WIDTH + kOx + x] == bg) {
                if (first < 0) first = yy * LCD_WIDTH + kOx + x;
                ++diff;
            }
    if (diff) std::fprintf(stderr, "  text '%.*s' at x=%d y=%d differs first x=%d y=%d got=%04x want=%04x\n",
                           int(text.size()), text.data(), x0, y, first % LCD_WIDTH, first / LCD_WIDTH,
                           actual[first], PANEL[(first / LCD_WIDTH) * LCD_WIDTH + first % LCD_WIDTH - kOx]);
    if (diff && text.size() <= 40) {
        int best = 0x7fffffff, best_y = y, best_x = x0;
        for (int cy = std::max(0, y - 30); cy <= std::min(239, y + 30); ++cy) {
          for (int cx = std::max(0, x0 - 32); cx <= std::min<int>(kTwinContentW - text_width(font, text), x0 + 32); ++cx) {
            std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> expected;
            std::copy_n(actual.data(), expected.size(), expected.begin());
            std::copy(expected.begin(), expected.end(), g_panel);
            gfx_text(kContent0, font, cx, cy, text, color, bg);
            int n = 0;
            for (int yy = cy; yy < cy + font.height; ++yy)
                for (int x = cx; x < cx + text_width(font, text); ++x)
                    if (PANEL[yy * LCD_WIDTH + x] != bg &&
                        actual[yy * LCD_WIDTH + kOx + x] == bg) ++n;
            std::copy(expected.begin(), expected.end(), g_panel);
            if (n < best) { best = n; best_x = cx; best_y = cy; }
          }
        }
        std::fprintf(stderr, "    best nearby x=%d y=%d differing ink=%d\n", best_x, best_y, best);
    }
    CHECK_EQ(diff, 0);
    std::copy(actual.begin(), actual.end(), g_panel);
}
// A list row's text (16 px, cut to `width` px as the kit does: the room before the tail / column; `open` = the focused row
// when it shows a note).
static void check_row_text(int row, std::string_view text, uint16_t color, uint16_t bg, int open = -1,
                           int width = kit::kTailX - kit::kTextX, int y0 = kit::kBodyY) {
    char buf[kFitBuf];
    check_text_at(kSmall, wizard_text_y(row, open, y0), fit_data(kSmall, text, width, buf), color, bg);   // the expected cut, not a copy check
}
// The focused row's note line (12 px dim, under the text).
static void check_row_note(int row, std::string_view note, int y0 = kit::kBodyY) {
    char buf[kFitBuf];
    check_text_at(kLabelFont, row_top(row, -1, y0) + kit::kNoteDy, fit_text(kLabelFont, note, kit::kNoteMaxPx, buf), kOnFocusDim, kFocusFill);
}
// A marker (radio, box, tick) is "on" when its 8 x 8 interior holds ink over the row ground; off, the interior is all ground.
static void check_radio(int row, bool on, int open = -1, uint16_t ground = kBg, int y0 = kit::kBodyY) {
    unsigned ink = 0;
    for (int y = 2; y < 10; ++y)
        for (int x = 2; x < 10; ++x)
            ink += PANEL[(row_top(row, open, y0) + 7 + y) * LCD_WIDTH + kit::kMarkX + x] != ground;
    CHECK_EQ(ink != 0, on);
}
// A list row's tail pill as the kit draws it: a kPillH-high rounded fill whose right edge is kit::kTailX, the 12 px tag
// inside (`open` = the focused row when it shows a note, which shifts the rows below it).
static void check_row_pill(int row, std::string_view tag, uint16_t color, uint16_t tint, int open = -1, int y0 = kit::kBodyY) {
    const int pw = pill_width(text_cells(tag));
    const int16_t px0 = static_cast<int16_t>(kit::kTailX - pw);
    const int y = row_top(row, open, y0);
    const int16_t py = static_cast<int16_t>(y + 1 + (kit::kRowH - 1 - kPillH) / 2);
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_fill(kContent0, px0, py, pw, kPillH, tint);
    gfx_text(kContent0, kLabelFont, px0 + 4, py + 1, tag, color, tint);
    unsigned diff = 0;
    for (int yy = y; yy < y + kit::kRowH; ++yy)
        for (int x = px0; x < kit::kTailX; ++x)
            if (actual[yy * LCD_WIDTH + kOx + x] != PANEL[yy * LCD_WIDTH + x]) {
                if (diff == 0)
                    std::fprintf(stderr, "  pill '%.*s' row %d first x=%d y=%d got=%04x want=%04x\n",
                                 int(tag.size()), tag.data(), row, kOx + x, yy,
                                 actual[yy * LCD_WIDTH + kOx + x], PANEL[yy * LCD_WIDTH + x]);
                ++diff;
            }
    CHECK_EQ(diff, 0);
    std::copy(actual.begin(), actual.end(), g_panel);
}
static void test_selection_and_focus_rendering() {
    fixture(); g_stub.lib = {saved(1), saved(2)};
    Ui ui; open(ui); ui.tick(1200); settle(ui, 1200);
    // The Tests step: the focused first row is open (its note), the rail is lit; the preset in force has its radio on.
    check_row_text(kAllRow, "All tests", kText, kFocusFill, kAllRow, 150);
    check_radio(kAllRow, true, kAllRow, kFocusFill);
    CHECK_EQ(PANEL[(row_top(kAllRow) + 4) * LCD_WIDTH + 1], kFocus);
    check_row_note(kAllRow, "Everything, QC Class B 20V");
    check_row_text(kPdRow, "PD only", kText, kBg, kAllRow, 150); check_radio(kPdRow, false, kAllRow);
    check_row_text(kLegacyRow, "Legacy only", kText, kBg, kAllRow, 150);
    check_row_text(kCustomRow, "Custom...", kText, kBg, kAllRow, 150); check_radio(kCustomRow, false, kAllRow);
    top(ui, 1220); move(ui, kPdRow, 1220); ui.tick(1220); settle(ui, 1220);
    check_row_text(kAllRow, "All tests", kText, kBg, kPdRow, 150); check_radio(kAllRow, true, kPdRow);
    check_row_text(kPdRow, "PD only", kText, kFocusFill, kPdRow, 150); check_radio(kPdRow, false, kPdRow, kFocusFill);
    press(ui, Button::Ok, 1230); ui.tick(1230); settle(ui, 1230);   // a preset pick selects and advances: there is no Next row
    CHECK_EQ(algos(ui), kPdMask); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back, 1235); ui.tick(1240); settle(ui, 1240);   // back on the preset row, now with its radio on
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kPdRow);
    check_radio(kPdRow, true, kPdRow, kFocusFill); check_radio(kAllRow, false, kPdRow);
    press(ui, Button::Minus, 1245); press(ui, Button::Ok, 1245);   // back to the defaults (and on to the cable)
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    press(ui, Button::Back, 1246); ui.tick(1250); settle(ui, 1250);
    check_radio(kAllRow, true, kAllRow, kFocusFill);
    custom(ui, 1255); ui.tick(1255); settle(ui, 1255);   // the families: Box3 markers, the focused one open with its note
    check_row_text(kFamHuawei, "Huawei/Honor", kText, kFocusFill, kFamHuawei, 120); check_radio(kFamHuawei, true, kFamHuawei, kFocusFill);
    move(ui, kFamPd, 1260); ui.tick(1260); settle(ui, 1260);
    check_row_text(kFamPd, "USB PD", kText, kFocusFill, kFamPd, 100); check_radio(kFamPd, true, kFamPd, kFocusFill);
    check_row_text(kFamXiaomi, "Xiaomi/Redmi", kDim, kBg, kFamPd, 120);   // mirrors the PD selection, no count of its own
    check_radio(kFamXiaomi, true, kFamPd);
    press(ui, Button::Ok, 1300); ui.tick(1300); settle(ui, 1300);   // a short OK toggles the family
    check_radio(kFamPd, false, kFamPd, kFocusFill); check_radio(kFamXiaomi, false, kFamPd);
    long_ok(ui, 1310); ui.tick(1310); settle(ui, 1310);             // a long OK opens its entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    check_row_text(0, "PD fixed", kText, kFocusFill, 0, 150); check_radio(0, false, 0, kFocusFill);
    move(ui, 1, 1350); ui.tick(1350); settle(ui, 1350);
    check_row_text(1, "PPS", kText, kFocusFill, 1, 150); check_radio(1, false, 1, kFocusFill);
    press(ui, Button::Ok, 1400); ui.tick(1400); settle(ui, 1400);
    check_radio(1, true, 1, kFocusFill); check_radio(0, false, 1);
    press(ui, Button::Back, 1450); top(ui, 1450); move(ui, kFamDone, 1450); press(ui, Button::Ok, 1500); ui.tick(1500); settle(ui, 1500);
    CHECK_EQ(ui.charger_test_stage(), 2);
    check_row_text(kOffRow, "Off (real cable)", kText, kFocusFill, kOffRow, kNoTailRoom);
    check_radio(kOffRow, true, kOffRow, kFocusFill);
    move(ui, kSavedRow, 1800); press(ui, Button::Ok, 1800);   // pick the first saved clone: selects and advances to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    press(ui, Button::Back, 1850); ui.tick(1900); settle(ui, 1900);             // the cursor comes back on the cable in force: clone 1
    CHECK_EQ(ui.charger_test_cursor(), kSavedRow);
    check_row_text(kSavedRow, "Host identity 1", kText, kFocusFill, kSavedRow, kSavedRoom); check_radio(kSavedRow, true, kSavedRow, kFocusFill);
    check_row_text(kOffRow, "Off (real cable)", kText, kBg, kSavedRow, kNoTailRoom); check_radio(kOffRow, false, kSavedRow);
    no_operations();   // picking arms nothing: no spoof, no scan, no save
}

static void test_long_identity_rendering() {
    fixture();
    auto identity = saved(1);
    std::snprintf(identity.name.data(), identity.name.size(), "ABCDEFGHIJKLMNOPQRSTUVWX");
    g_stub.lib = {identity};
    Ui ui; open(ui); cable(ui);
    move(ui, kSavedRow); press(ui, Button::Ok);   // the pick advances to Review; BACK returns with the cursor on the clone
    press(ui, Button::Back, 1250); ui.tick(1300); settle(ui, 1300);
    CHECK_EQ(ui.charger_test_stage(), 2);
    // External names yield the tail and use the full no-tail lane; fit_data keeps clipping counted.
    constexpr int kRoom = kTwinContentW - ListView::kBarW - kit::kTextX;
    check_row_text(kSavedRow, "ABCDEFGHIJKLMNOPQRSTUVWX", kText, kFocusFill, kSavedRow, kRoom);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300); settle(ui, 1300);
        const Rect box = compute_layout(orientation_for(r).edge, kSlotCenters, kSlotLens, kPanelThickness).twin_content;
        const int x = box.x + kit::kTextX * box.w / kTwinContentW;
        const int y = box.y + wizard_text_y(kSavedRow) * box.h / 240;
        uint16_t lut[16], line[240];
        make_blend_lut(kText, kFocusFill, lut);
        for (int row = 0; row < kSmall.height; ++row) {
            char fit[kFitBuf];
            const int n = render_row(kSmall, fit_data(kSmall, "ABCDEFGHIJKLMNOPQRSTUVWX", kRoom, fit), row, lut, line, 240);
            CHECK_EQ(std::equal(line, line + n, g_panel + (y + row) * LCD_WIDTH + x), true);
        }
        std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> expected;
        std::copy_n(g_panel, expected.size(), expected.begin());
        CHECK_EQ(capture_begin(ui), true);
        for (int strip = 0; strip < kCaptureStrips; ++strip) {
            const uint16_t *pixels = capture_strip(ui, strip, 1300);
            CHECK_EQ(std::equal(pixels, pixels + kCaptureStripPixels,
                                expected.begin() + strip * kCaptureStripPixels), true);
        }
        capture_end(ui, 1300);
    }
    press(ui, Button::Back, 1400);
    CHECK_EQ(ui.charger_test_stage(), 0);   // BACK from the Cable step goes to the Tests step: there is no sub-list to leave
    CHECK_EQ(g_stub.spoof_arms, 0);
    no_operations();
}
// One identity list: picking only selects. Nothing is armed until START, the Review page shows the choice, and the
// library is never written.
static void test_cable_pick_only_selects_and_start_arms() {
    fixture(); g_stub.lib = {saved(1), saved(2)}; const auto before = g_stub.lib;
    Ui ui; open(ui); cable(ui);
    CHECK_EQ(ui.list_state().count, kSavedRow + 2);
    CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::Choice{}, true);   // the default is Off
    pick_row(ui, kSavedRow + 1);
    CHECK_EQ(ui.charger_test_stage(), 4);   // the pick advances to Review
    CHECK_EQ((ui.charger_test_cable_info().choice == ctcable::Choice{ctcable::Kind::Saved, 2}), true);
    CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.spoof_on, false);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), kSavedRow + 1);   // BACK: the cursor sits on the pick
    pick_row(ui, kOffRow); CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::Choice{}, true);
    press(ui, Button::Back);
    pick_row(ui, kBuiltinRow + 1);
    CHECK_EQ((ui.charger_test_cable_info().choice == ctcable::Choice{ctcable::Kind::Builtin, 2}), true);
    press(ui, Button::Back);
    pick_row(ui, kSavedRow + 1);
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1300);   // leaving the page arms nothing either
    CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_arms, 0);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1400);
    cable(ui, 1500); pick_row(ui, kSavedRow + 1, 1500);   // the pick goes on to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    start_press(ui, 1600);                        // the first OK only opens the confirmation: the clone is not armed yet
    CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_arms, 0);
    confirm_press(ui, 1700);                      // the second arms the chosen clone: the guard cannot judge in the stub, it goes on
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == before[1].rec, true);
    CHECK_EQ(g_stub.spoof_arms, 1);
    CHECK_EQ(g_stub.cable_limits.max_mv, 20000); CHECK_EQ(g_stub.cable_limits.max_ma, 3000);
    CHECK_EQ(g_stub.lib.size(), before.size());
    for (unsigned i = 0; i < before.size(); ++i) {
        CHECK_EQ(g_stub.lib[i].rec == before[i].rec, true); CHECK_EQ(g_stub.lib[i].name == before[i].name, true);
    }
    CHECK_EQ(g_stub.scans + g_stub.lib_saves + g_stub.requests + g_stub.fc_starts, 0);
}
// START with Off disarms whatever is armed: a spoof armed from Tools (or the remote) goes with the START, the pick
// (Off) takes over and arms nothing.
static void test_start_with_off_disarms_a_tools_spoof() {
    fixture(); g_stub.lib = {saved(1)};
    g_stub.spoof_on = true; g_stub.spoof_record = g_stub.lib[0].rec; ++g_stub.spoof_generation;
    Ui ui; open(ui); review(ui); start_run(ui, 1300);
    CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_disarms, 1); CHECK_EQ(g_stub.spoof_arms, 0);
}
static void test_cable_page_never_writes_flash() {
    // The "Try QC after PD" row is gone: every unit detaches on its own, so the cable page has no
    // preference to store. Moving around and pressing OK on its rows never writes flash.
    fixture(); g_stub.prefs = {}; g_stub.prefs_sets = 0;
    Ui ui; open(ui); cable(ui); move(ui, 3); press(ui, Button::Ok);
    CHECK_EQ(g_stub.prefs_sets, 0);
    press(ui, Button::Back); move(ui, 1); press(ui, Button::Ok);
    CHECK_EQ(g_stub.prefs_sets, 0);
    CHECK_EQ(g_stub.prefs.qc_after_pd, false);
}
// The refusals now come on START, where the identity is armed; each leaves nothing armed.
static void test_spoof_refusals_and_shared_busy() {
    for (unsigned mode = 0; mode < 9; ++mode) {
        fixture(); g_stub.lib = {saved(1)}; Ui ui; open(ui); cable(ui); pick_row(ui, kSavedRow);
        if (mode == 0) g_stub.lib.clear();
        if (mode == 1) g_stub.lib[0].rec[0] ^= 1;
        if (mode == 3) g_stub.fc_state = static_cast<int>(fc::State::Active);
        if (mode == 4) g_stub.fc_scanning = true;
        if (mode == 5) g_stub.sink.held = true;
        if (mode == 6) g_stub.sink.epr_mode = true;
        if (mode == 7) g_stub.ident.state = pd::IdState::Running;
        if (mode == 8) g_stub.sink.state = pd::SinkState::Aborting;
        CHECK_EQ(ui.charger_test_stage(), 4);   // the pick went on to Review
        if (mode == 2) { g_stub.inl = true; g_stub.link_inline = true; }   // after the page's own ticks, which recompute the link state
        start_run(ui, 1300);
        CHECK_EQ(g_stub.spoof_on, false);
        CHECK_EQ(g_stub.spoof_arms, mode == 1 || mode == 2 ? 1 : 0);   // two attempts reach the driver, which refuses them; the rest stop before it
        if (mode >= 3) CHECK_EQ(ui.spoof_busy(), true);
        CHECK_EQ(g_stub.scans + g_stub.lib_saves + g_stub.requests + g_stub.fc_starts, 0);
    }
}
static void test_confirmation_bound_to_exact_proposal() {
    for (unsigned mode = 0; mode < 12; ++mode) {
        fixture(); Ui ui; open(ui); review(ui); start_press(ui, 1300);
        if (mode == 0) ++g_stub.session;
        if (mode == 1) ++g_stub.caps.pdos[0].raw;
        if (mode == 2) g_stub.caps.spec_rev = 1;
        if (mode == 3) g_stub.caps.active_cc = 2;
        if (mode == 4) g_stub.caps.epr_partial = true;
        if (mode == 5) g_stub.cable_limits = {.max_mv = 50000, .max_ma = 5000, .observed = true};
        if (mode == 6) g_stub.power_chip = power::Chip::Ina228;
        if (mode == 7) g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2};
        if (mode == 8) { g_stub.spoof_on = true; ++g_stub.spoof_generation; }
        if (mode == 9) g_stub.pd_traffic = true;
        if (mode == 10) ui.set_rotation(1, 1400);
        if (mode == 11) {
            press(ui, Button::Minus, 1400); press(ui, Button::Plus, 1450);
        }
        ui.tick(1500); start_press(ui, 1600);
        CHECK_EQ(ui.charger_test_busy(), false);
        press(ui, Button::Ok, 1700); CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.requests, 0); CHECK_EQ(g_stub.spoof_arms, 0);
    }
    fixture(); Ui repeated; open(repeated); review(repeated); start_press(repeated, 1300);
    ++g_stub.caps.rx_count; g_stub.caps.raw_header ^= 1u << 9;
    repeated.tick(1400); press(repeated, Button::Ok, 1500);
    CHECK_EQ(repeated.charger_test_busy(), true); // identical offers don't invalidate on MessageID alone

    fixture(); Ui wrap; open(wrap); review(wrap, 0xffffec78u); start_press(wrap, 0xffffec78u);
    press(wrap, Button::Ok, 0xffffff00u);
    CHECK_EQ(wrap.charger_test_busy(), true); // a deadline of zero is still an active confirmation
}
static void test_skip_results_and_deselection() {
    fixture(); g_stub.fc_answers = false; Ui ui; open(ui); only_vivo_review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (uint32_t t = 1500; t <= 600000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6); CHECK_EQ(ui.charger_test_results().steps().size(), 4);
    for (const auto& s : ui.charger_test_results().steps()) {   // the stub's fast-charge source never answers: every VIVO level times out
        CHECK_EQ(s.verdict, ct::Verdict::Fail); CHECK_EQ(s.reason, ct::Reason::Timeout);
    }
    CHECK_EQ(g_stub.requests + g_stub.releases, 0);   // legacy rows never talk PD
    CHECK_EQ(g_stub.rescans + g_stub.scans + g_stub.lib_saves + g_stub.spoof_arms, 0);
    read_results(ui);
    top(ui); press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 7);   // the grid has no separator rows: line 0 is a unit, OK opens its steps
    move(ui, 1); press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 8);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 7);
    press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 8);
    press(ui, Button::Ok); press(ui, Button::Back); press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 0);
    CHECK_EQ(algos(ui), ct::bit(A::Vivo));
    CHECK_EQ(g_stub.requests + g_stub.releases, 0);
    CHECK_EQ(g_stub.rescans + g_stub.scans + g_stub.lib_saves + g_stub.spoof_arms, 0);
}
static void test_navigation_and_stop_controls() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        fixture(); Ui ui; open(ui); review(ui); start_press(ui, 1300);
        if (mode == 0) press(ui, Button::Back, 1400);
        if (mode == 1) press(ui, Button::Plus, 1400);
        if (mode == 2) ui.goto_page(static_cast<uint8_t>(Page::Home), 1400);
        if (mode == 3) ui.on_button(Button::Plus, ButtonEvent::Long, 1400);
        no_operations();
        if (mode < 2) {
            start_press(ui, 1500); CHECK_EQ(ui.charger_test_busy(), false);
        }
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        fixture(); Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
        review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t now = 1500;   // the Pd unit detaches and attaches first: tick until its first request
        for (; now <= 20000 && g_stub.requests < 1; now += 100) ui.tick(now);
        CHECK_EQ(g_stub.requests, 1);
        if (mode == 0) press(ui, Button::Back, now);
        if (mode == 1) {   // OK is free while running: nothing stops until BACK
            press(ui, Button::Ok, now);
            CHECK_EQ(g_stub.releases, 0);
            press(ui, Button::Back, now);
        }
        if (mode == 2) ui.on_button(Button::Back, ButtonEvent::Long, now);   // straight to the Meter: leaving stops the run
        if (mode == 3) CHECK_EQ(ui.on_button(Button::Back, ButtonEvent::Hold, now), UiAction::None);   // stops the run, no reboot
        CHECK_EQ(g_stub.releases, 1); CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(ui.spoof_busy(), true);
        for (const uint32_t end = now + 20000; now <= end && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.requests, 1);
    }
}
static bool footer_says(std::string_view text, uint16_t color);
static bool footer_shows(std::string_view text, uint16_t color = kText);
static bool view_says(Ui& ui, std::string_view text);
static void test_full_help_text() {
    // Each stage's copy: the focused row opens a 12 px dim note under its text; the footer says what is picked.
    fixture(); Ui ui; open(ui); ui.tick(1200);
    static constexpr const char* kPresetNote[] = {"Everything, QC Class B 20V", "PD fixed, PPS, EPR, Xiaomi",
                                                  "D+/D-: vendor modes, QC, UFCS", "Huawei, Samsung, OPPO, vivo"};
    for (int r = 0; r < 4; ++r) {
        top(ui, 1210); move(ui, kAllRow + r, 1210); ui.tick(1210 + r); settle(ui, 1210 + r);
        check_row_note(kAllRow + r, kPresetNote[r]);
    }
    top(ui, 1220); move(ui, kCustomRow, 1220); ui.tick(1220); settle(ui, 1220);
    check_row_note(kCustomRow, "Choose families one by one");
    CHECK_EQ(view_says(ui, "Picked: All tests"), true);
    press(ui, Button::Ok, 1230); ui.tick(1230); settle(ui, 1230);   // Custom...
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
    check_row_note(kFamHuawei, "Entries inside this family");
    CHECK_EQ(footer_says("6 of 6 families on", kText), true);
    top(ui, 1240); move(ui, kFamXiaomi, 1240); ui.tick(1240); settle(ui, 1240);
    check_row_note(kFamXiaomi, "SVID 0x2717 auth, runs in PD");   // Xiaomi / Redmi rides the PD unit
    move(ui, 1, 1250); long_ok(ui, 1250); ui.tick(1250); settle(ui, 1250);    // USB PD's entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    check_row_note(0, "Fixed offers: SPR + EPR");
    CHECK_EQ(footer_says("2 of 2 entries on", kText), true);
    move(ui, 1, 1260); ui.tick(1260); settle(ui, 1260);
    check_row_note(1, "PPS + SPR/EPR AVS offers");
    no_operations();
}
static void test_first_menu_and_pd_bundles() {
    fixture(); Ui ui; open(ui); custom(ui); open_group(ui, kFamPd);
    press(ui, Button::Ok);   // the first entry: PD fixed off
    CHECK_EQ(ui.charger_test_selection() & ct::kPdFixedAlgorithms, 0);
    CHECK_EQ(ui.charger_test_selection() & ct::kPpsAlgorithms, ct::kPpsAlgorithms);
    const auto selected = ui.charger_test_selection();
    ui.goto_page(static_cast<uint8_t>(Page::Home), 1400);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1500);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.list_state().offset, 0);
    CHECK_EQ(ui.charger_test_selection(), selected);
    custom(ui, 1600); open_group(ui, kFamPd, 1600); move(ui, 1, 1600); press(ui, Button::Ok, 1600);   // PPS off too
    CHECK_EQ(ui.charger_test_selection() & (ct::kPdFixedAlgorithms | ct::kPpsAlgorithms), 0);
    no_operations();
}
// The four presets of the Tests step: OK only selects (the cursor stays), the pill follows what is in force, and a mix that
// is no preset puts the pill on "Custom...".
static void test_tests_step_presets() {
    fixture(); Ui ui; open(ui); ui.tick(1200);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.list_state().count, 6);
    CHECK_EQ(kPdMask & kLegacyMask, 0); CHECK_EQ(kPdMask | kLegacyMask, ct::kDefaultAlgorithms);   // two halves of "All tests"
    CHECK_EQ(kPdMask != 0 && kLegacyMask != 0, true);
    CHECK_EQ(kLegacyMask & ct::bit(A::Qc2ClassB), ct::bit(A::Qc2ClassB));   // Class B rides with the legacy half
    // "Vendor modes": the OEM D+/D- families only (no PD, no QC / UFCS), a subset of "Legacy only".
    constexpr uint32_t kVendorMask = ct::bit(A::Fcp) | ct::bit(A::Scp) | ct::bit(A::Afc) | ct::bit(A::Vooc) |
                                     ct::bit(A::SuperVooc) | ct::bit(A::SuperVooc65) | ct::bit(A::Vivo);
    CHECK_EQ(kVendorMask & ~kLegacyMask, 0);
    CHECK_EQ(kVendorMask & (ct::bit(A::Qc2) | ct::bit(A::Ufcs)), 0);
    {
        Ui v; open(v); v.tick(1200);
        top(v); move(v, kVendorRow); press(v, Button::Ok);
        CHECK_EQ(algos(v), kVendorMask); CHECK_EQ(v.charger_test_stage(), 2);   // the pick goes on to the cable
        press(v, Button::Back); v.tick(1300); settle(v, 1300);
        CHECK_EQ(v.charger_test_stage(), 0); CHECK_EQ(v.charger_test_cursor(), kVendorRow);   // BACK: the cursor sits on the pick
        check_row_text(kVendorRow, "Vendor modes", kText, kFocusFill, kVendorRow, 150);
        check_radio(kVendorRow, true, kVendorRow, kFocusFill); check_radio(kLegacyRow, false, kVendorRow);
    }
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    top(ui); move(ui, kPdRow); press(ui, Button::Ok);
    CHECK_EQ(algos(ui), kPdMask); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kPdRow);
    move(ui, 1); press(ui, Button::Ok);
    CHECK_EQ(algos(ui), kLegacyMask);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_cursor(), kLegacyRow);
    ui.tick(1300); settle(ui, 1300);
    check_radio(kLegacyRow, true, kLegacyRow, kFocusFill); check_radio(kPdRow, false, kLegacyRow);
    top(ui); move(ui, kAllRow); press(ui, Button::Ok);
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);
    // The same preset again changes nothing (no toggle: a preset is a choice, not a switch).
    press(ui, Button::Back); press(ui, Button::Ok);
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back);
    // A hand-made mix is no preset: the radio moves to "Custom...".
    custom(ui, 1400); open_group(ui, kFamPd, 1400); press(ui, Button::Ok, 1400);   // PD fixed off
    press(ui, Button::Back); press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kCustomRow);
    CHECK_EQ(algos(ui) != ct::kDefaultAlgorithms, true);
    ui.tick(1500); settle(ui, 1500);
    check_radio(kCustomRow, true, kCustomRow, kFocusFill); check_radio(kAllRow, false, kCustomRow);
    no_operations();
}
// Back walks the steps in reverse: Review -> Cable -> Tests, a Group page -> Custom -> Tests; Custom's Done goes to the Cable
// step, and Back from there returns to Tests (Custom is a sub-list of step 1, not a step).
static void test_back_chain() {
    fixture(); Ui ui; open(ui);
    review(ui);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 0);
    custom(ui); press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kCustomRow);
    custom(ui); open_group(ui, kFamPd);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), kCustomStage); CHECK_EQ(ui.charger_test_cursor(), kFamPd);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 0);
    custom(ui); top(ui); move(ui, kFamDone); press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 2);   // Custom's Done: the Cable step
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 0);
    custom(ui); open_group(ui, kFamSamsung); press(ui, Button::Back);   // a Group page returns to Custom, whose Done goes on
    top(ui); move(ui, kFamDone); press(ui, Button::Ok);
    CHECK_EQ(ui.charger_test_stage(), 2);
    no_operations();
}
// The top bar counts three steps: "1/3 Tests" (also over Custom and its Group pages), "2/3 Spoof cable", "3/3 Review". The progress
// rule under it (3 segments, pitch 75) lights the segments up to the step.
static void check_header(Ui &ui, int step) {
    const kit::OppRailSpec spec = ui.current_opp_rail();
    CHECK_EQ(spec.dot_count, 3);
    CHECK_EQ(spec.dot_cur, step - 1);
    CHECK_EQ(g_panel[0] == kBar || g_panel[0] == kFocus, true);
}
static void test_three_step_header() {
    fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); ui.tick(1200);
    check_header(ui, 1);
    custom(ui, 1300); ui.tick(1300);
    check_header(ui, 1);
    open_group(ui, kFamSamsung, 1310); ui.tick(1320);
    check_header(ui, 1);   // a family's page is still step 1
    press(ui, Button::Back, 1330); top(ui, 1330); move(ui, kFamDone, 1330); press(ui, Button::Ok, 1330); ui.tick(1340);
    CHECK_EQ(ui.charger_test_stage(), 2);
    check_header(ui, 2);
    press(ui, Button::Ok, 1350); ui.tick(1360);
    CHECK_EQ(ui.charger_test_stage(), 4);
    check_header(ui, 3);
}
// Review's Spoof row (row 2 of the table, the value column at x72) is the identity the START presents. The armed state is not
// part of the line: it is on the Cable step (the ARMED pill) and in the Refuse / Waiting stages.
static void check_review_spoof(std::string_view text, uint16_t color) {
    char buf[kFitBuf];
    const int y = 90 + 2 * 24 + kit::row_text_y(24, kSmall.height);
    check_text_at(kSmall, y, "Spoof", kDim, kBg, 6);   // the key of the identity row
    check_text_at(kSmall, y, fit_data(kSmall, text, 17 * kSmall.adv, buf), color, kBg, 68);
}
// The face name of a built-in chip ("Xiaomi 6A" -> "Xiaomi"): the rating is the dim second column of its row.
static std::string face_name(std::string_view label) { return std::string(label.substr(0, label.find(' '))); }
static void test_review_shows_the_armed_identity() {
    fixture(); Ui ui; open(ui);
    cable(ui); pick_row(ui, kBuiltinRow + 1);                // the pick advances to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    ui.tick(1300);
    const std::string label(emrec::builtin_info(2).label);
    check_review_spoof(label, kText);                        // selected, not armed
    start_press(ui, 1400);                                   // the first OK: the confirmation, the identity stays unarmed
    CHECK_EQ(g_stub.spoof_on, false);
    ui.tick(1500);
    check_review_spoof(label, kText);
    confirm_press(ui, 1600);                                 // the second OK arms the identity and the run starts
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_arms, 1);
}
// ONE armed marker, ONE list: an identity armed on Tools -> Spoof shows ARMED on the Spoof page and on the wizard's Cable
// step while it is the pick, with the same names and the same colours. Both lists are kit rows: the name at x28 (kCaution
// for the armed row), the rating in the second column, the pill at the tail. On the Cable step a different pick splits
// the marks: the pick wears NEXT, the identity answering now wears CURR (the Spoof view keeps ARMED only).
static void test_armed_marker_is_shared_with_tools_spoof() {
    fixture(); Ui ui; open(ui);
    const std::string name = face_name(emrec::builtin_info(2).label);
    ui.push_menu(Menu::Spoof, 1300);
    ui.tick(1310); settle(ui, 1310);
    constexpr int kSpoofY = SpoofPage::kListY;
    check_row_text(0, "Off (real cable)", kText, kFocusFill, -1, kNoTailRoom, kSpoofY);   // the default selection, the cursor row
    press(ui, Button::Plus, 1320); press(ui, Button::Plus, 1330);               // the second built-in chip
    press(ui, Button::Ok, 1340);                                                // Spoof arms at once
    CHECK_EQ(g_stub.spoof_on, true);
    ui.tick(1350); settle(ui, 1350);
    check_row_text(2, name, kCaution, kFocusFill, -1, kit::kCol2X - kit::kTextX, kSpoofY);
    check_row_pill(2, "ARMED", kCaution, kCautionT, -1, kSpoofY);                // the Spoof view: ARMED only, never CURR/NEXT
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);                       // back to the charger test
    ui.tick(1410); settle(ui, 1410);
    cable(ui, 1500); ui.tick(1510); settle(ui, 1510);                                             // the cursor starts on the identity in force
    check_row_text(kBuiltinRow + 1, name, kCaution, kFocusFill, kBuiltinRow + 1, kit::kCol2X - kit::kTextX);   // the same row, the same colour
    check_row_pill(kBuiltinRow + 1, "ARMED", kCaution, kCautionT, kBuiltinRow + 1);   // pick == armed: ARMED on the Cable step too
    // Picking another identity on the wizard only selects: the armed row wears CURR, the pick NEXT.
    pick_row(ui, kBuiltinRow + 2, 1600);
    CHECK_EQ(ui.charger_test_stage(), 4);
    press(ui, Button::Back, 1650); ui.tick(1660); settle(ui, 1660);
    CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow + 2);
    CHECK_EQ(g_stub.spoof_arms, 1);
    check_row_text(kBuiltinRow + 1, name, kCaution, kBg, kBuiltinRow + 2, kit::kCol2X - kit::kTextX);
    check_row_pill(kBuiltinRow + 1, "CURR", kCaution, kDimT, kBuiltinRow + 2);
    check_row_text(kBuiltinRow + 2, face_name(emrec::builtin_info(4).label), kOk, kFocusFill, kBuiltinRow + 2, kit::kCol2X - kit::kTextX);
    check_row_pill(kBuiltinRow + 2, "NEXT", kOk, kOkT, kBuiltinRow + 2);
}
// The footer message line (what a refused START says) as the panel shows it.
static bool footer_says(std::string_view text, uint16_t color) {
    // Option B: the message is the floating toast pill over the bottom of the content surface. Draw the same pill on a copy
    // of the panel and compare the whole panel: identical means the pill (frame, fill, words) is already there.
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    draw_toast(kContent0, footer_tone_of(color), text);
    unsigned diff = 0;
    for (size_t i = 0; i < actual.size(); ++i) diff += actual[i] != g_panel[i];
    std::copy(actual.begin(), actual.end(), g_panel);   // look without touching
    return diff == 0;
}
static bool view_says(Ui& ui, std::string_view text) {
    const auto view = ui.charger_test_view();
    return std::string_view(view.message, view.message_len) == text;
}
// A START refused for a busy tool says which one and what to do, never a bare "tool busy" (hardware 2026-11-07: the scripted
// START was refused that way and nothing on the screen said why). The refusal opens no confirmation and touches nothing.
static void test_busy_refusal_names_the_cause_and_the_way_out() {
    struct Cause { const char* text; void (*set)(); };
    static const Cause causes[] = {
        {"QC trigger: stop it", [] { g_stub.fc_state = static_cast<int>(fc::State::Active); }},
        {"QC trigger: stop it", [] { g_stub.fc_scanning = true; }},
        {"PD hold: release it", [] { g_stub.sink.held = true; }},
        {"EPR contract: exit it", [] { g_stub.sink.epr_mode = true; }},
        {"Identify: wait", [] { g_stub.ident.state = pd::IdState::Running; }},
        {"PD request: wait", [] { g_stub.sink.state = pd::SinkState::Aborting; }},
    };
    for (const auto& cause : causes) {
        fixture(); Ui ui; open(ui); review(ui);
        cause.set();
        start_press(ui, 1300);
        ui.tick(1400);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(footer_says(cause.text, kCaution), true);
        CHECK_EQ(std::string_view(ui.busy_reason()) == std::string_view(cause.text), true);
        nothing_armed_stub();
    }
}
// A spoof armed on Tools -> Spoof is the user's while the run presents it: however the run ends, the identity stays armed.
// A run that starts with "Off" disarms it at START (the pick takes over; Off arms nothing), however the run ends, and
// Review says plain "Off". A run's own identity is gone at its end, Review shows it as chosen but no longer armed.
static void arm_tools_spoof(Ui& ui) {
    ui.push_menu(Menu::Spoof, 1300); ui.tick(1310);
    press(ui, Button::Plus, 1320); press(ui, Button::Plus, 1330);
    press(ui, Button::Ok, 1340);                         // the second built-in chip, armed at once
    CHECK_EQ(g_stub.spoof_on, true);
    ui.on_button(Button::Back, ButtonEvent::Short, 1400); ui.tick(1410);
}
static void test_tools_spoof_survives_a_runs_end() {
    const std::string label(emrec::builtin_info(2).label);
    for (unsigned variant = 0; variant < 6; ++variant) {
        const unsigned how = variant % 3;          // done, BACK mid-run, the page left mid-run
        const bool presents = variant >= 3;        // the run presents the Tools identity itself / the choice is Off
        fixture(); Ui ui; open(ui);
        arm_tools_spoof(ui);                       // Tools -> Spoof arms AND selects the identity
        if (presents) { review(ui, 1500); ui.tick(1510); check_review_spoof(label, kText); }
        else {
            cable(ui, 1500); pick_row(ui, kOffRow, 1500);
            if (ui.charger_test_stage() != 4) { top(ui, 1500); press(ui, Button::Ok, 1500); }
            ui.tick(1510); check_review_spoof("Off", kText);
        }
        start_press(ui, 1600); confirm_press(ui, 1700);
        CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.ra_on, presents);          // the Ra mode belongs to the run; Off never turns it on
        if (!presents) { CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_disarms, 1); }   // START with Off disarmed the Tools spoof
        uint32_t now = 1800;
        if (how == 1 || how == 2) { for (int i = 0; i < 20; ++i, now += 100) ui.tick(now); }
        if (how == 1) press(ui, Button::Back, now);
        if (how == 2) ui.goto_page(static_cast<uint8_t>(Page::Home), now);
        for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.spoof_on, presents); CHECK_EQ(g_stub.spoof_disarms, presents ? 0 : 1);   // presented: still answering / Off: disarmed at START
        CHECK_EQ(g_stub.ra_on, false);
        ui.goto_page(static_cast<uint8_t>(Page::Home), now + 100);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now + 200);
        review(ui, now + 300); ui.tick(now + 400);
        if (presents) check_review_spoof(label, kText);
        else check_review_spoof("Off", kText);
    }
    { // a suspended run abandoned on the Resume page leaves the presented spoof alone too
        fixture(); g_stub.self_powered = true;
        g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
        Ui ui; open(ui); arm_tools_spoof(ui);
        CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui, 1500);
        start_press(ui, 1600); confirm_press(ui, 1700);
        CHECK_EQ(ui.charger_test_busy(), true);
        uint32_t now = 1800;
        const auto passes = [&] { unsigned n = 0; for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail; return n; };
        for (; now <= 60000 && passes() < 1; now += 100) ui.tick(now);
        g_stub.pm.vbus_mv = 0;
        for (const uint32_t end = now + 1000; now < end; now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        press(ui, Button::Back, now);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_disarms, 0);
    }
}
// Results longer than the 8 visible lines: [+]/[-] move the selection one line at a time and the view scrolls only when
// the selection would leave it (the same rule as the other list pages), so the highlight is never stuck on the top line.
static void test_results_selection_moves_and_view_follows_it() {
    fixture();
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
    Ui ui; open(ui); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (uint32_t t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const int lines = ui.list_state().count;
    ui.tick(1000000); settle(ui, 1000000);
    CHECK_EQ(ui.charger_test_cursor(), 0); CHECK_EQ(ui.list_state().offset, 0);
    CHECK_EQ(lines > ui.list_state().visible, true);   // the case under test: more lines than the grid shows (it scrolls by pixel height)
    uint32_t t = 1000100;
    for (int i = 1; i < lines; ++i) {
        const int was = ui.list_state().offset, shown = ui.list_state().visible;
        press(ui, Button::Plus); ui.tick(t += 100); settle(ui, t);
        CHECK_EQ(ui.charger_test_cursor(), i);                                       // one line per press
        const auto s = ui.list_state();
        CHECK_EQ(i >= s.offset && i < s.offset + s.visible, true);                   // the selected line is always on screen
        if (i < was + shown) CHECK_EQ(s.offset, was);                                // scrolls only when it would leave the view
        CHECK_EQ(s.offset >= was, true);
    }
    for (int i = lines - 2; i >= 0; --i) {
        const int was = ui.list_state().offset;
        press(ui, Button::Minus); ui.tick(t += 100); settle(ui, t);
        CHECK_EQ(ui.charger_test_cursor(), i);
        const auto s = ui.list_state();
        CHECK_EQ(i >= s.offset && i < s.offset + s.visible, true);
        CHECK_EQ(s.offset <= was, true);                                             // and never scrolls down going up
    }
    CHECK_EQ(ui.list_state().offset, 0);
}
// The Results list repaints incrementally (one slot per changed line). After every scroll the panel must equal a forced
// full repaint of the same state, so no chip fragment of an earlier line may survive at its slot.
static void settle_ms(Ui& ui, uint32_t& t) { for (int i = 0; i < 4; ++i) { t += 100; ui.tick(t); } settle(ui, t); }   // the scrollbar thumb glides 160 ms
static unsigned panel_diff_vs_forced_repaint(Ui& ui, uint32_t now) {
    std::vector<uint16_t> incremental(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT);
    ui.render_for_capture(now);
    unsigned diff = 0;
    int first = -1;
    for (unsigned i = 0; i < incremental.size(); ++i)
        if (incremental[i] != g_panel[i]) {
            if (first < 0) first = static_cast<int>(i);
            ++diff;
        }
    if (diff) std::fprintf(stderr, "  forced repaint first x=%d y=%d incremental=%04x forced=%04x\n",
                           first % LCD_WIDTH, first / LCD_WIDTH, incremental[first], g_panel[first]);
    return diff;
}
static void results_incremental_repaint_matches_forced(uint8_t rot) {
    fixture();
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
    Ui ui; open(ui); ui.set_rotation(rot, 1050); settle(ui, 1050); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    ui.tick(t); t += 200; ui.tick(t); t += 200;
    settle(ui, t);
    const int lines = ui.list_state().count;
    CHECK_EQ(lines > 8, true);
    CHECK_EQ(panel_diff_vs_forced_repaint(ui, t), 0u);
    for (int i = 1; i < lines; ++i) {
        press(ui, Button::Plus, t); settle_ms(ui, t);
        const unsigned diff = panel_diff_vs_forced_repaint(ui, t);
        if (diff) printf("  results scroll down line %d: %u pixels differ\n", i, diff);
        CHECK_EQ(diff, 0u);
    }
    for (int i = lines - 2; i >= 0; --i) {
        press(ui, Button::Minus, t); settle_ms(ui, t);
        const unsigned diff = panel_diff_vs_forced_repaint(ui, t);
        if (diff) printf("  results scroll up line %d: %u pixels differ\n", i, diff);
        CHECK_EQ(diff, 0u);
    }
}
static void test_results_incremental_repaint_matches_forced() {
    for (uint8_t rot = 0; rot < 4; ++rot) results_incremental_repaint_matches_forced(rot);
}
// Abort a run with BACK at several points and watch the Results page while the runner winds down: every tick that
// changes a result line must leave the panel equal to a forced repaint.
static void test_results_update_matches_forced() {
    for (uint32_t abort_at = 1500; abort_at <= 60000; abort_at += 7300) {
        fixture();
        const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
        g_stub.caps.num_pdos = 6;
        for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
        g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t < abort_at && ui.charger_test_busy(); t += 100) ui.tick(t);
        press(ui, Button::Back, t);
        for (int i = 0; i < 400 && ui.charger_test_stage() == 6; ++i) {
            t += 50; ui.tick(t);
            if (ui.charger_test_stage() != 6) break;
            const unsigned diff = panel_diff_vs_forced_repaint(ui, t);
            if (diff) printf("  abort_at %u tick %d: %u pixels differ\n", abort_at, i, diff);
            CHECK_EQ(diff, 0u);
            if (i % 9 == 0) { press(ui, Button::Plus, t); }
        }
    }
}
static void test_pps_wire_headroom_note() {
    fixture();
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[1] = pd::decode_pdo(0xc0000000 | (210u << 17) | (50u << 8) | 60);
    Ui ui; open(ui); review(ui);
    CHECK_EQ(footer_says("PPS: 21V on 20V wire", kText), true);   // the Review footer (Info tone)
    no_operations();
}
static void test_review_start_is_only_row() {
    fixture(); Ui ui; open(ui); review(ui);
    press(ui, Button::Plus); CHECK_EQ(ui.charger_test_cursor(), 0);    // START is the only row
    press(ui, Button::Minus); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), 0);
}
// A self-powered K2 says on Review, in one line, that the run keeps VBUS up; a HID-powered one keeps its old note.
static void test_self_powered_review_note() {
    for (unsigned self = 0; self < 2; ++self) {
        fixture(); g_stub.self_powered = self != 0;
        Ui ui; open(ui); review(ui);
        if (self) CHECK_EQ(footer_says("Self-powered: VBUS stays up", kText), true);
        else CHECK_EQ(footer_says("Check the other port", kCaution), true);   // the default Review footer: the warning's one-line echo
    }
}
// Review ALWAYS carries the stern other-port warning (user decision 2026-11-15): the amber band, the two lines, the START ticket's
// hint and the Caution footer, on a bare idle link, a self-powered K2 and under the Level row's cursor alike; START's own confirm is
// the acknowledgement (one confirm per run, nothing more). The band word must also fit its box (no cut).
static void test_review_carries_the_other_port_warning() {
    CHECK_EQ(text_width(kMid, "OTHER PORT FREE") <= 240 - 36 - 4, true);   // the band's word box
    for (unsigned mode = 0; mode < 3; ++mode) {
        fixture(); g_stub.self_powered = mode == 1;
        Ui ui; open(ui); review(ui);
        if (mode == 2) { press(ui, Button::Minus); settle(ui); CHECK_EQ(ui.charger_test_cursor(), 0); }   // the Level row
        CHECK_EQ(PANEL[30 * LCD_WIDTH + 2], kCaution);   // the band's amber, left of the triangle
        check_text_at(kMid, 8, "OTHER PORT FREE", kSpotInk, kCaution, 26);
        check_text_at(kSmall, 48, "Device on the other port", kText, kBg, 6);
        check_text_at(kSmall, 66, "can be damaged. Unplug it", kText, kBg, 6);
        if (mode == 0) CHECK_EQ(footer_says("Check the other port", kCaution), true);
        if (mode == 1) CHECK_EQ(footer_says("Self-powered: VBUS stays up", kText), true);
        no_operations();
    }
    // One confirm per run: START arms (CONFIRM), the second OK starts; the warning asks for nothing more.
    fixture(); Ui ui; open(ui); review(ui);
    start_press(ui, 1300);
    CHECK_EQ(ui.charger_test_stage(), 4);      // armed, still on Review: nothing started
    CHECK_EQ(ui.charger_test_busy(), false);
    confirm_press(ui, 1400);
    CHECK_EQ(ui.charger_test_busy(), true);    // the single confirm starts it
}
// The latch policy (user decision 2026-11-15): a CC level, a foreign VBUS level or a rise nothing caused is no evidence of a device
// on the other port, so none of them refuses a run (hardware: a USB-A charger with nothing plugged in reads ~1.75 V on CC2); a load
// beyond the K2's own supply or a Request the K2 did not send is, and still does. The real pd::Link runs inside the Ui here.
static void set_lines(int cc1, int cc2);
static void ticks_for(Ui& ui, uint32_t& t, uint32_t ms) { for (uint32_t end = t + ms; t < end; t += 100) { fusb302::service(); ui.tick(t); } }
static void test_phantom_evidence_does_not_refuse_a_run_but_a_load_does() {
    for (unsigned mode = 0; mode < 4; ++mode) {
        fixture();
        if (mode == 0) set_lines(3300, 1750);                       // a floating CC2 on a bare USB-A charger
        if (mode == 1) { set_lines(3300, 3300); g_stub.pm.vbus_mv = 5100; }   // a foreign VBUS level, no current
        if (mode == 2) { set_lines(1600, 1600); }   // both lines pulled to a sink-like level, nothing flowing
        if (mode == 3) g_stub.pm.current_ma = 1500;                 // a real load beyond the K2's own supply
        Ui ui; open(ui);
        uint32_t t = 1200;
        ticks_for(ui, t, k2::pd::kInlineQuietMs + 3000);
        review(ui, t); start_run(ui, t + 100);
        if (mode == 3) CHECK_EQ(ui.charger_test_busy(), false);
        else CHECK_EQ(ui.charger_test_busy(), true);
    }
}
// The card of a self-powered run records it (cable flags bit 2), a HID-powered one does not.
static void test_self_powered_run_is_on_the_card() {
    for (unsigned self = 0; self < 2; ++self) {
        fixture(); stub_cards_reset(); g_stub.self_powered = self != 0;
        Ui ui; open(ui); only_vivo_review(ui);   // only VIVO: a short legacy run
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        for (uint32_t t = 1500; t <= 600000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        std::array<uint16_t, 8> nums{};
        const size_t n = report_card::hw::numbers(nums);
        CHECK_EQ(n, 1);
        report_card::Card c;
        CHECK_EQ(n && report_card::hw::get(nums[0], c), true);
        CHECK_EQ((c.cable.flags & report_card::kCableSelfPowered) != 0, self != 0);
    }
}
// The driver's cable reading is per session (every unit attach and source Hard Reset drops it); the run keeps what it read, so the
// card names the cable the charger's own SOP' query was answered by even when the run ends in a session that never read one.
static void test_a_cable_read_once_stays_with_the_run() {
    fixture(); stub_cards_reset();
    g_stub.cable_limits.max_mv = 50000; g_stub.cable_limits.max_ma = 5000; g_stub.cable_limits.observed = true;
    Ui ui; open(ui); only_vivo_review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    g_stub.cable_limits = {};   // the next session read nothing
    for (uint32_t t = 1500; t <= 600000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    std::array<uint16_t, 8> nums{};
    const size_t n = report_card::hw::numbers(nums);
    report_card::Card c;
    CHECK_EQ(n &&  report_card::hw::get(nums[0], c), true);
    CHECK_EQ((c.cable.flags & report_card::kCableObserved) != 0, true);
    CHECK_EQ(c.cable.max_mv, 50000);
    CHECK_EQ(c.cable.max_ma, 5000);
}
static void test_vsafe5_review() {
    {
        fixture(); g_stub.pm.vbus_mv = 5269;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); CHECK_EQ(ui.charger_test_busy(), false);
        press(ui, Button::Ok, 1400); CHECK_EQ(ui.charger_test_busy(), true);
        no_operations();
    }
    {
        fixture(); g_stub.pm.vbus_mv = 4600;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(footer_says("VBUS not safe 5V", kCaution), true);
        no_operations();
    }
}
// The START preflight's refusal is a footer message on screen only; the log must carry it too (one [TEST] line with the
// reason in the on-screen words and the VBUS reading), so a remote START (tools/ct_start.py) can print why the run id
// stayed 0 - a source already elevated under the PDO 1 contract (7.7 V on the splitter's other port) refuses the START.
static void test_preflight_refusal_is_logged() {
    {
        fixture(); g_stub.pm.vbus_mv = 7700;   // above vSafe5V, and up (>= kStartVbusMv): no Rd wait, refused on the first OK
        Ui ui; open(ui); review(ui);
        g_stub.log_text.clear();
        start_press(ui, 1300);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(footer_says("VBUS not safe 5V", kCaution), true);
        CHECK_EQ(g_stub.log_text.find("[TEST] preflight refused: VBUS not safe 5V (7700 mV)") != std::string::npos, true);
        no_operations();
    }
    {
        fixture(); g_stub.pm.vbus_mv = 5000;   // a safe 5 V source: the START goes on, no refusal is logged
        Ui ui; open(ui); review(ui);
        g_stub.log_text.clear();
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.log_text.find("preflight refused") == std::string::npos, true);
    }
}
// (B5, pattern shared-bus-split; the B5 run runs/2026-11-10-splitter-wrongv read VBUS 8.47 V under a PDO 1 contract):
// with the sink holding a contract at PDO 1 and VBUS outside vSafe5V, the START's preflight refuses WrongVoltage -
// the footer names the measured level and the log line says "VBUS 8.5 V on a 5 V contract (shared bus)", which
// ct_start.py prints when the run id stays 0.
static void test_preflight_wrong_voltage_under_pdo1_contract_is_logged() {
    fixture(); g_stub.pm.vbus_mv = 8470;
    g_stub.sink.state = pd::SinkState::Ready; g_stub.sink.pos = 1; g_stub.sink.mv = 5000;   // PS_RDY at PDO 1
    Ui ui; open(ui); review(ui);
    g_stub.log_text.clear();
    start_press(ui, 1300);   // VBUS is up (>= kStartVbusMv): no Rd wait, refused on the first OK
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(view_says(ui, "VBUS 8.5 V on a 5V contract"), true);
    CHECK_EQ(ui.context().toast.view() == "VBUS 8.5 V on a 5V contract", true);
    CHECK_EQ(g_stub.log_text.find("[TEST] preflight refused: VBUS 8.5 V on a 5 V contract (shared bus)") != std::string::npos, true);   // red today
    no_operations();
}
// QA 2026-11-10 row B item 1 (red): the [SWEEP] summary line keeps its fixed words in the format string - today the
// line ends with a literal ": .*s", because neither binlog's signature nor the host's renderer knows %.*s.
static void test_sweep_log_line_has_no_literal_format_bug() {
    fixture();
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[1] = pd::decode_pdo(0xc0000000 | (60u << 17) | (50u << 8) | 20);   // PPS 5-6 V 1 A: an 11-point sweep
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    g_stub.log_text.clear();
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (uint32_t t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(g_stub.log_text.find("[SWEEP]") != std::string::npos, true);
    CHECK_EQ(g_stub.log_text.find(".*s") == std::string::npos, true);       // no literal ": .*s" tail (red today)
    CHECK_EQ(g_stub.log_text.find("11 pts") != std::string::npos, true);
    CHECK_EQ(g_stub.log_text.find("peak=") != std::string::npos, true);     // the summary's fields ride as numbers
}
// Row B item 6 (red): the [TEST] state-change line keeps the state's full name in the format string - today the
// state rides a %s, which the device binlog cuts at 12 bytes ("[TEST] Detach charg", "[TEST] Initializin").
static void test_state_change_log_keeps_the_full_state_name_in_the_format() {
    fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui);
    g_stub.log_fmts.clear();
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (uint32_t t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(g_stub.log_fmts.find("[TEST] %s %s\n") == std::string::npos, true);        // red today
    CHECK_EQ(g_stub.log_fmts.find("[TEST] Detach charger %s\n") != std::string::npos, true);   // red today
    CHECK_EQ(g_stub.log_fmts.find("[TEST] Complete %s\n") != std::string::npos, true);         // red today
}
// Row B item 8 (red): the AVS sweep is a Status field too - bit 30 of the draft selection on the wire (bit 31 is
// the PPS sweep; the CLI toggle is the tools branch).
static void test_avs_sweep_is_a_selection_bit() {
    fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_selection() & (1u << 30), 0u);
    ui.charger_test_set_avs_sweep(true);
    CHECK_EQ(ui.charger_test_selection() & (1u << 30), 1u << 30);   // red today: bit 30 is never set
    ui.charger_test_set_avs_sweep(false);
    CHECK_EQ(ui.charger_test_selection() & (1u << 30), 0u);
}
// Row B item 10 (red): with an AVS-only source the AVS sweep is row 2 of the PD entries - today it is pinned at
// row 3, beyond the list (row_count = 2 + 0 PPS + 1 AVS), so an AVS-only source cannot reach its own sweep.
static void test_avs_sweep_row_follows_the_only_sweep_kind() {
    fixture();
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[1] = pd::decode_pdo(0xE0000000u | 300u);   // SPR AVS 9-20 V 3 A, no PPS APDO
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui); custom(ui); open_group(ui, kFamPd, 1300); ui.tick(1300);
    CHECK_EQ(ui.charger_test_stage(), 1);
    move(ui, 2, 1310); ui.tick(1310);
    check_row_text(2, "AVS sweep", kText, kFocusFill, 2, 150);        // red today: row 2 reads "PPS"
    press(ui, Button::Ok, 1320); ui.tick(1320);                    // toggle the sweep row
    CHECK_EQ(ui.charger_test_selection() & (1u << 30), 1u << 30);   // red today: row 2 toggles the PPS bit, not the AVS flag
}
// User decision 2026-11-15: the default run is the Medium level, without the deep sweeps; Deep turns both on (bits 31 PPS,
// 30 AVS), and the draft carries the level in bits 28..29. The remote knob still turns one sweep off after that.
// "Offers" is the Depth row's fourth value (Light -> Medium -> Deep -> Offers -> Light): the Review line says so, the preset picked
// stays as it was (Offers -> Light finds it again) but the run is the PD plan whatever it is, and the run presents the EPR cable
// itself while the Settings spoof is Off: the Review Spoof line shows it, START arms it, the stored pick stays Off.
static void dump_screen(const char* name);
static void test_depth_offers_value_and_the_forced_epr_spoof() {
    fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_cable(ctcable::Choice{}), true);               // Settings spoof: Off
    top(ui); press(ui, Button::Ok); settle(ui);                                  // Medium -> Deep
    CHECK_EQ(ui.charger_test_selection() >> 28, 0xEu);
    press(ui, Button::Ok); settle(ui);                                           // Deep -> Offers
    CHECK_EQ(ui.charger_test_cursor(), kDepthRow);
    CHECK_EQ((ui.charger_test_selection() >> 28), unsigned(ct::Level::Offers));  // no sweep rides on it
    CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);                                 // the picked set is kept for the way back
    check_row_text(kDepthRow, "Depth", kText, kFocusFill, kDepthRow, 150);
    check_row_note(kDepthRow, "Lists offers, one AVS request");
    for (uint8_t rot = 0; rot < 4; ++rot) {                                      // K2_DUMP: the Tests list in every rotation
        char path[40]; std::snprintf(path, sizeof path, "offers_tests_rot%u", unsigned(rot));
        ui.set_rotation(rot, 1500 + rot); settle(ui, 1500 + rot); dump_screen(path);
    }
    ui.set_rotation(0, 1600); settle(ui, 1600);
    const unsigned offers_all = ui.charger_test_view().preview.runnable;
    pick_row(ui, kPdRow); press(ui, Button::Back); settle(ui);                  // PD only, back on Tests
    CHECK_EQ(ui.charger_test_view().preview.runnable, offers_all);              // legacy rows are not in an Offers plan, PD pick or not
    pick_row(ui, kLegacyRow); press(ui, Button::Back); settle(ui);
    CHECK_EQ(ui.charger_test_view().preview.runnable, offers_all);              // a legacy-only pick plans the PD list
    top(ui); move(ui, kAllRow); press(ui, Button::Ok); settle(ui);              // All tests: on to the Cable step
    CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Ok); settle(ui);                                           // the Off row stays picked: on to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    ui.tick(1300);
    const int depth_y = 90 + 1 * 24 + kit::row_text_y(24, kSmall.height);
    check_text_at(kSmall, depth_y, "Depth", kDim, kBg, 6);
    check_text_at(kSmall, depth_y, "Offers", kText, kBg, 68);
    check_review_spoof(std::string(emrec::builtin_info(1).label), kText);       // what the run will present, not "Off"
    for (uint8_t rot = 0; rot < 4; ++rot) {
        char path[40]; std::snprintf(path, sizeof path, "offers_review_rot%u", unsigned(rot));
        ui.set_rotation(rot, 1700 + rot); settle(ui, 1700 + rot); dump_screen(path);
    }
    ui.set_rotation(0, 1800); settle(ui, 1800);
    CHECK_EQ(g_stub.spoof_on, false);                                            // nothing armed before START
    start_run(ui, 1900);
    CHECK_EQ(ui.charger_test_busy(), true);
    CHECK_EQ(ui.charger_test_results().level(), ct::Level::Offers);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_arms, 1);            // the EPR identity answers the charger's cable check
    CHECK_EQ(ui.charger_test_cable_info().run == ctcable::kDefaultChoice, true);
    CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::Choice{}, true);   // the stored pick stays Off
}
// Any other level keeps the pick as it is: Off arms nothing, and Review says Off. The way back round the cycle finds the preset.
static void test_other_levels_keep_the_spoof_pick() {
    fixture();
    {
        Ui ui; open(ui);
        CHECK_EQ(ui.charger_test_set_cable(ctcable::Choice{}), true);
        review(ui); ui.tick(1300);
        check_review_spoof("Off", kText);
        start_run(ui, 1400);
        CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_arms, 0);
    }
    fixture(); Ui again; open(again);
    top(again); for (int i = 0; i < 4; ++i) { press(again, Button::Ok); settle(again); }   // Medium -> Deep -> Offers -> Light -> Medium
    CHECK_EQ(again.charger_test_selection() >> 28, 0u);
    CHECK_EQ(algos(again), ct::kDefaultAlgorithms);
}
static void test_the_default_run_is_medium() {
    fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0u);                   // Medium, no sweep
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Deep)), true);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0xEu);                 // PPS + AVS sweep, level 2
    ui.charger_test_set_sweep(false);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0x6u);                 // the AVS sweep stays, the level stays Deep
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Light)), true);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0x1u);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Offers)), true);   // the Depth row's fourth value
    CHECK_EQ(ui.charger_test_selection() >> 28, 3u);                   // no sweep rides on it
    CHECK_EQ(ui.charger_test_set_level(4), false);                     // no such level
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Light)), true);
    // The SVOOC ceiling stage follows the level (Deep only); a usb-a waiver survives Deep and is dropped below it.
    CHECK_EQ(ui.charger_test_svooc_cap(), 0u);                         // Light: off
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Deep)), true);
    CHECK_EQ(ui.charger_test_svooc_cap(), 1u);                         // Deep: on
    CHECK_EQ(ui.charger_test_set_svooc_cap(2), true);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Deep)), true);
    CHECK_EQ(ui.charger_test_svooc_cap(), 2u);                         // Deep again: the usb-a waiver stays
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Medium)), true);
    CHECK_EQ(ui.charger_test_svooc_cap(), 0u);                         // Medium: off
}
// Review (step 3/3): the Level row sits above START. START keeps the opening focus; MINUS reaches Level, whose OK cycles
// Medium -> Deep -> Light -> Medium (next_level), the Tests row's step count follows the level, and the footer says what the
// level does. PLUS goes back to START, whose OK opens the confirmation as before.
static void dump_screen(const char* name);
// The panel's blank columns between the amber chip's label ink and its dim "S" mark (-1: no such chip): the label is drawn with
// kCaution on the kCautionT fill, the mark with kDim; the rows scanned are the chip body (the Partial bar below it is kCaution too).
static int amber_chip_sweep_gap() {
    int y0 = -1, y1 = -1;
    for (int y = 0; y < LCD_HEIGHT; ++y) {   // the first run of chip rows; the footer's own tone (a caution count) is a later run
        int n = 0;
        for (int x = 0; x < LCD_WIDTH; ++x) n += PANEL[y * LCD_WIDTH + x] == kCautionT;
        if (n >= 100) { if (y0 < 0) y0 = y; y1 = y; }
        else if (y0 >= 0) break;
    }
    if (y0 < 0) return -1;
    int fill_x0 = LCD_WIDTH, first_dim = LCD_WIDTH;
    for (int x = 0; x < LCD_WIDTH; ++x)
        if (PANEL[y0 * LCD_WIDTH + x] == kCautionT && x < fill_x0) fill_x0 = x;
    for (int y = y0; y <= y1; ++y)
        for (int x = fill_x0; x < LCD_WIDTH; ++x)   // inside the chip: the row label ("PPS") is dim too, left of it
            if (PANEL[y * LCD_WIDTH + x] == kDim && x < first_dim) first_dim = x;
    int last_ink = -1;
    for (int y = y0; y <= y1; ++y)
        for (int x = 0; x < first_dim; ++x)
            if (PANEL[y * LCD_WIDTH + x] == kCaution && x > last_ink) last_ink = x;
    return first_dim < LCD_WIDTH && last_ink >= 0 ? first_dim - last_ink - 1 : -1;
}
static void test_depth_row_cycles_and_shows_on_review() {
    fixture();
    g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300);                                       // 9 V 3 A
    g_stub.caps.pdos[2] = pd::decode_pdo(0xC0000000u | (210u << 17) | (33u << 8) | 100u);           // PPS 3.3-21 V 5 A
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kAllRow);   // opens on All tests, with Depth right above
    press(ui, Button::Minus); settle(ui);
    CHECK_EQ(ui.charger_test_cursor(), kDepthRow);
    check_row_text(kDepthRow, "Depth", kText, kFocusFill, kDepthRow, 150);
    check_row_note(kDepthRow, "Tests each advertised voltage");
    const unsigned medium = ui.charger_test_view().preview.runnable;
    CHECK_EQ(ui.charger_test_selection() >> 28, 0u);
    // OK on Depth cycles Medium -> Deep -> Offers -> Light -> Medium (stays on Tests).
    press(ui, Button::Ok); settle(ui);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kDepthRow);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0xEu);                             // Deep: both sweeps, level 2
    check_row_note(kDepthRow, "Sweeps, accuracy & PD spec");
    const unsigned deep = ui.charger_test_view().preview.runnable;
    CHECK_EQ(unsigned(ui.charger_test_view().preview.spec_checks), unsigned(specsec::kCells));
    CHECK_EQ(unsigned(ui.charger_test_view().preview.total) >= deep, true);
    press(ui, Button::Ok); settle(ui);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0x3u);                             // Offers: no sweep
    check_row_note(kDepthRow, "Lists offers, one AVS request");
    press(ui, Button::Ok); settle(ui);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0x1u);                             // Light
    check_row_note(kDepthRow, "Checks what charger offers");
    const unsigned light = ui.charger_test_view().preview.runnable;
    CHECK_EQ(unsigned(ui.charger_test_view().preview.spec_checks), 0u);
    std::printf("[tests-depth] runnable: Light %u, Medium %u, Deep %u\n", light, medium, deep);
    CHECK_EQ(light < medium && medium < deep, true);
    // Advance to Review: Depth shows as a KV row ("Light"), Plus/Minus are no-ops, OK starts confirmation.
    review(ui); ui.tick(1300);
    const int depth_y = 90 + 1 * 24 + kit::row_text_y(24, kSmall.height);
    check_text_at(kSmall, depth_y, "Depth", kDim, kBg, 6);
    check_text_at(kSmall, depth_y, "Light", kText, kBg, 68);
    press(ui, Button::Minus); settle(ui); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Plus); settle(ui); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Ok, 1300); settle(ui, 1300);
    CHECK_EQ(ui.charger_test_view().confirming, true);
    // Back to Tests and cycle Light -> Medium.
    press(ui, Button::Back); press(ui, Button::Back); press(ui, Button::Back);
    top(ui); press(ui, Button::Ok); settle(ui);
    CHECK_EQ(ui.charger_test_selection() >> 28, 0u);                               // back to Medium
    CHECK_EQ(ui.charger_test_view().preview.runnable, medium);
}
static void test_results_counters_are_words() {
    for (unsigned kind = 0; kind < 3; ++kind) {
        fixture();
        g_stub.caps.spec_rev = kind == 0 ? 1 : 2;
        if (kind == 2) g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 1};
        Ui ui; open(ui); only_vivo_review(ui);   // only VIVO
        ct::legacy_tune_set(uint8_t(ct::LegacyKnob::QuietMs), ct::kLegacyQuietMs);   // chatter is seen by the PD-quiet wait (default 0 skips it)
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2};
        // The charger keeps talking PD after the Legacy attach: one try, one longer retry, then every row of the unit skips.
        g_stub.pd_chatter = true;
        for (uint32_t t = 1500; t <= 120000; t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        // The counters strip (y 0..23, three 69 px columns) says the totals as words in the verdict colours: "0 pass  0 fail
        // 4 skip", 16 px text at x = 16 + 69 * column after the verdict shape (the fail count stays kError even at 0).
        const struct { int x; std::string_view text; uint16_t color; } words[] = {
            {16, "0 pass", kOk}, {16 + 69, "0 fail", kError}, {16 + 138, "4 skip", kDim}};
        for (const auto& w : words) {
            check_text_at(kSmall, 4, w.text, w.color, kBg, w.x);
        }
        no_operations();
        ct::legacy_tune_reset();
    }
}
// A finished run of a six-PDO source (five fixed levels and a PPS range), the Results page on screen, rotation 0.
static void six_pdo_fixture() {
    fixture();
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
}
static void finished_results(Ui& ui, uint32_t& t) {
    open(ui); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    ui.tick(t); t += 200; ui.tick(t); t += 200;
    settle(ui, t);
}
// The Results grid is the run's: what the live link says after the run ended (the Xiaomi SVID gone, the Mi auth lost, a source
// unplugged) must not change its lines, its cursor bounds or the Steps a press opens.
static void test_results_grid_is_frozen_at_run_end() {
    six_pdo_fixture(); Ui plain; uint32_t t;
    finished_results(plain, t);
    const int plain_lines = plain.list_state().count;
    six_pdo_fixture(); g_stub.vdm.xiaomi = true; g_stub.sink.xiaomi_authed = true;
    Ui ui;
    finished_results(ui, t);
    const int xiaomi_lines = ui.list_state().count;
    CHECK_EQ(xiaomi_lines != plain_lines, true);   // the facts do change the grid, or the checks below prove nothing
    g_stub.vdm.xiaomi = false; g_stub.sink.xiaomi_authed = false;   // the charger goes away
    ui.tick(t + 100); t += 200;
    CHECK_EQ(ui.list_state().count, xiaomi_lines);
    press(ui, Button::Ok, t); t += 200; ui.tick(t);     // opens the unit's Steps (a rebuild of the grid on the way back)
    CHECK_EQ(ui.charger_test_stage(), 7);
    press(ui, Button::Back, t); t += 200; ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    settle(ui, t);
    CHECK_EQ(ui.list_state().count, xiaomi_lines);
}
// The Results grid (kit ResultGrid from y 32): the focused unit lights the 54 px label strip (3 px rail + tint) and its first
// verdict cell is a flat pass fill.
static void test_results_grid_rows_rail_and_footer() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    CHECK_EQ(ui.list_state().count > 8, true);
    constexpr int kTop = k2::runlay::kResGridY;
    CHECK_EQ(PANEL[(kTop - 2) * LCD_WIDTH + 100] == kRule && PANEL[(kTop - 1) * LCD_WIDTH + 100] == kRule, true);   // the 2 px rule over the grid
    CHECK_EQ(PANEL[kTop * LCD_WIDTH + 1] == kFocus, true);                                           // rail on the focused row
    CHECK_EQ(PANEL[kTop * LCD_WIDTH + 30] == kFocusFill, true);                                         // and its tint
    CHECK_EQ(PANEL[kTop * LCD_WIDTH + kit::kResultLabelW] == kBg, true);                            // no label rule: white space
    CHECK_EQ(PANEL[(kTop + 1) * LCD_WIDTH + kit::kResultLabelW + 2] == kOkT, true);                        // a passing cell: the flat pass fill
    move(ui, 2); ui.tick(t + 100);
    CHECK_EQ(ui.charger_test_stage(), 6);
}
// A verdict cell is one flat fill with its text centred on it: across the first row's cells the fill never breaks except for the
// 1 px gap between neighbours, and the cells stop short of the right edge.
static void test_results_cells_are_flat_fills() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    unsigned fill = 0, gaps = 0;
    int last_fill = 0, fill_y = k2::runlay::kResGridY;
    for (int y = k2::runlay::kResGridY; y < k2::runlay::kResGridY + 40; ++y) {
        unsigned row_fill = 0;
        int row_last = 0;
        for (int x = kit::kResultLabelW + 1; x < kTwinContentW; ++x) {
            const uint16_t p = PANEL[y * LCD_WIDTH + x];
            if (p == kOkT || p == kError) { ++row_fill; row_last = x; }
        }
        if (row_fill > fill) { fill = row_fill; last_fill = row_last; fill_y = y; }
    }
    // The first unit's 9 V / 12 V rows fail on this stub (VBUS stays 5 V) and the row behind a failed retarget is skipped, which
    // paints no fill: count the gap columns up to the last filled cell only.
    for (int x = kit::kResultLabelW + 1; x < last_fill; ++x) gaps += PANEL[fill_y * LCD_WIDTH + x] == kBg;
    CHECK_EQ(fill > 100, true);
    CHECK_EQ(gaps >= 1 && gaps < 12, true);
    CHECK_EQ(PANEL[fill_y * LCD_WIDTH + kTwinContentW - 1] == kBg || PANEL[fill_y * LCD_WIDTH + kTwinContentW - 1] == kOkT, true);
}
// A source whose converter stops at 19.4 V (the Baseus FH22): VBUS stays at 19.4 V whatever is asked. The PPS 5-21V range then
// plateaus below its top, and the Results screen must say how far it got instead of a red chip:
//   chip amber (tint + a bar under the text), the footer "PPS...: caps 19.4V of 21V" in the caution tone,
//   Steps lead cell amber, Detail band "CAPPED" in amber with "Reached" in place of "Window".
static void test_a_capped_range_reads_amber_and_says_how_far_it_got() {
    fixture();
    g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300);
    g_stub.caps.pdos[1] = pd::decode_pdo((400u << 10) | 300);
    g_stub.caps.pdos[2] = pd::decode_pdo(0xc0000000 | (210u << 17) | (33u << 8) | 60);
    g_stub.vbus_ceiling_mv = 19400;   // the converter climbs to what is asked and stops at 19.4 V
    Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto& r = ui.charger_test_results();
    int capped = -1;
    for (size_t i = 0; i < r.steps().size(); ++i)
        if (r.steps()[i].verdict == ct::Verdict::Fail && r.steps()[i].reason == ct::Reason::Capped) capped = static_cast<int>(i);
    CHECK_EQ(capped >= 0, true);
    if (capped < 0) return;
    CHECK_EQ(int(r.steps()[static_cast<size_t>(capped)].measured_mv / 100), 194);
    result_layout::Layout l; result_layout::build(l, r.steps());
    int pps = -1;
    for (size_t i = 0; i < l.rows.size(); ++i) if (!l.rows[i].header && l.rows[i].pd) { pps = static_cast<int>(i); break; }
    CHECK_EQ(pps >= 0, true);   // the PD block: a range line's footer names its kind
    // Focus the PPS row: its footer says how far the converter got, in the caution tone (the chip's own words stay "5-21V 3.2A").
    StrBuf<32> want;
    want.put("PPS: caps 19.4V of 21V");
    bool found = false;
    top(ui, t); settle(ui, t);
    for (int n = 0; n < 24 && !found; ++n) {
        found = view_says(ui, want.view());
        if (!found) { press(ui, Button::Plus); ui.tick(t += 100); settle(ui, t); }
    }
    CHECK_EQ(found, true);
    settle(ui, t);
    // The chip: an amber tint with the bar under its text.
    unsigned tint = 0, bar = 0;
    for (int y = k2::runlay::kResGridY; y < kFooterY; ++y)
        for (int x = kit::kResultLabelW + 1; x < 224; ++x) {
            const uint16_t p = PANEL[y * LCD_WIDTH + x];
            tint += p == kPartFill; bar += p == kOnPart;
        }
    CHECK_EQ(tint > 50, true);
    CHECK_EQ(bar > 20, true);
    // Steps of the row, then the capped step's Detail: an amber band reading CAPPED, and "Reached" where the window was.
    press(ui, Button::Ok); ui.tick(t += 100); settle(ui, t);
    CHECK_EQ(ui.charger_test_stage(), 7);   // Steps
    bool detail = false;
    for (int n = 0; n < 40 && !detail; ++n) {   // the PD block's Steps page lists every PD step
        press(ui, Button::Ok); ui.tick(t += 100); settle(ui, t);
        if (ui.charger_test_stage() == 8 && PANEL[2 * LCD_WIDTH + 100] == kPartFill) detail = true;
        else { press(ui, Button::Back); ui.tick(t += 100); settle(ui, t); press(ui, Button::Plus); ui.tick(t += 100); settle(ui, t); }
    }
    CHECK_EQ(detail, true);
    if (!detail) return;
    check_text_at(kSmall, 12, "CAPPED", kOnPart, kPartFill, 32);   // beside the whole name the word drops to 16 px
    const int ty = 44 + 2 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height);   // the third row of the Detail table (x6 label, x76 value)
    check_text_at(kSmall, ty, "Reached", kDim, kBg, 6);
    check_text_at(kSmall, ty, "3.3-19.4V of 21V", kCaution, kBg, 76);   // the PPS range starts at 3.3 V: 16 cells, the full value column
}
static void dump_screen(const char* name);
static int ty_reason() { return 44 + 3 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height); }   // the fourth row of the Detail table
// A pass has no reason and a skipped step never measured anything: the Detail table says "--" (the Window row's placeholder) and the Steps
// row's tail does too, neither a blank cell nor the source's idle voltage in the measured colour.
static void test_a_pass_or_a_skip_reads_as_placeholders() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    const auto& r = ui.charger_test_results();
    const auto mark = [&](ct::Verdict v) {
        for (const auto& s : r.steps()) {
            auto& w = const_cast<ct::Step&>(s);
            w.verdict = v; w.reason = ct::Reason::None; w.detail = 0; w.measured_mv = 12000;
        }
    };
    const auto volt_ink = [&] {
        int n = 0;
        for (int y = kit::kBodyY; y < kFooterY; ++y)
            for (int x = 0; x < kTwinContentW; ++x) n += PANEL[y * LCD_WIDTH + x] == kVolt;   // the rail's own V reading stays out
        return n;
    };
    const auto open_steps = [&] {
        top(ui, t); calm();
        press(ui, Button::Ok); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 7);
        CHECK_EQ(ui.current_opp_rail().title == "STEPS", true);   // the list is called Steps, as its strip and footer say
        top(ui, t); calm();
    };
    const int ty_measured = 44 + 1 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height);
    mark(ct::Verdict::Pass);
    open_steps();
    CHECK_EQ(volt_ink() > 50, true);   // a measured step keeps its reading
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 8);
    CHECK_EQ(ui.current_opp_rail().title == "DETAIL", true);
    check_text_at(kSmall, ty_measured, "12.00V", kVolt, kBg, 76);
    check_text_at(kSmall, ty_reason(), "--", kDim, kBg, 76);
    press(ui, Button::Back); ui.tick(t += 100); calm();
    press(ui, Button::Back); ui.tick(t += 100); calm();
    mark(ct::Verdict::Skip);
    open_steps();
    CHECK_EQ(volt_ink(), 0);           // a skipped step shows the placeholder, not 12.0V
    dump_screen("skip_steps");         // K2_DUMP
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 8);
    check_text_at(kSmall, ty_measured, "--", kFaint, kBg, 76);
    dump_screen("skip_detail");
}
// The PD spec section of a Deep run (include/k2/chargertest/spec_section.hpp): one row per check after the unit rows, each a one-cell
// VerdictCell with the worst value; OK opens the check's detail page, BACK returns to the row. The cells are set on the finished
// run (the host stub has no pdspec engine run), including the extremes the quantised value can hold.
static specsec::Result spec_fixture() {
    namespace sc = pdspec;
    specsec::Result r;
    r.end = specsec::End::Done;
    for (size_t i = 0; i < specsec::kCells; ++i) {
        const sc::Unit u = sc::kDefs[static_cast<size_t>(specsec::kOrder[i])].unit;
        switch (i % 6) {
            case 0: r.cells[i] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0, specsec::quantize(u, 1234)); break;
            case 1: r.cells[i] = specsec::make_cell(sc::Verdict::Fail, sc::Skip::None, 255, specsec::quantize(u, 987654)); break;   // widest text
            case 2: r.cells[i] = specsec::make_cell(sc::Verdict::Skip, sc::Skip::NoEprProof, 0); break;                           // longest reason
            case 3: r.cells[i] = specsec::make_cell(sc::Verdict::Unverified, sc::Skip::None, 0, specsec::quantize(u, -5432)); break;
            case 4: r.cells[i] = specsec::make_cell(sc::Verdict::Pass, sc::Skip::None, 0, specsec::kUnseen); break;                // "not seen"
            default: r.cells[i] = specsec::make_cell(sc::Verdict::Pending, sc::Skip::None, 0); break;
        }
    }
    return r;
}
static void test_the_results_grid_carries_the_pdspec_section() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const int units = ui.list_state().count;
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    // Without a spec result (every Light / Medium run, the default image) the grid is the unit rows alone.
    CHECK_EQ(specsec::kBuilt, true);   // the host build has the engine glue (K2_PDSPEC=1)
    const_cast<ct::Evidence&>(ui.charger_test_results().evidence()).spec = spec_fixture();
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t += 100); calm();   // into a unit's Steps and back: the grid is rebuilt
    press(ui, Button::Back, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 6);
    CHECK_EQ(ui.list_state().count, units + int(specsec::kCells));
    // The cursor runs on past the unit rows, one check per press, and stops on the last.
    for (int i = 0; i < units + 40; ++i) press(ui, Button::Plus, t);
    ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_cursor(), units + int(specsec::kCells) - 1);
    dump_screen("spec_results_end");
    press(ui, Button::Ok, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 15);   // SpecDetail
    dump_screen("spec_detail_last");
    press(ui, Button::Back, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 6);
    CHECK_EQ(ui.charger_test_cursor(), units + int(specsec::kCells) - 1);   // the cursor is where it was
    // Every row and detail page of every kind draws whole (the truncation walk fails the run on any cut).
    for (int row = units; row < units + int(specsec::kCells); ++row) {
        top(ui, t);
        for (int i = 0; i < row; ++i) press(ui, Button::Plus, t);
        ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_cursor(), row);
        if (row == units) dump_screen("spec_results_first");   // K2_DUMP: the first spec row, the unit rows scrolled off
        if (row == units + 12) dump_screen("spec_results_mid");
        press(ui, Button::Ok, t); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 15);
        if (row == units) dump_screen("spec_detail_pass");        // K2_DUMP: value, limit and verdict of a passing and a failing check
        if (row == units + 1) dump_screen("spec_detail_fail");
        press(ui, Button::Back, t); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 6);
    }
}
static void family_offer();
// Layout B (user decision 2026-11-19): each range line reads "PPS | 3.3-21V 5A | spec" (the failing family's word when a check of the
// range failed), the merged PPS / AVS rows leave the spec section once every measured range has a line, and a range's Steps page lists
// its own checks after its steps; OK opens one, BACK returns to the Steps row. The offer has two PPS ranges and an SPR AVS one; the
// second PPS range's steps are rewritten into an EPR AVS range (pos 9), which the host stub does not run. K2_DUMP: renders, 4 rotations.
static void test_layout_b_range_lines_carry_their_spec_word() {
    namespace sc = pdspec;
    using C = sc::Check;
    family_offer();
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    const auto go = [&](Button b, int n = 1) { for (int i = 0; i < n; ++i) press(ui, b, t); ui.tick(t += 100); calm(); };
    const auto renders = [&](const char* name) {   // K2_DUMP: the screen in all four rotations, then back to rotation 0
        for (uint8_t rot = 0; rot < 4; ++rot) {
            ui.set_rotation(rot, t); ui.tick(t += 100); calm();
            char path[40];
            std::snprintf(path, sizeof path, "%s_rot%u", name, unsigned(rot));
            dump_screen(path);
        }
        ui.set_rotation(0, t); ui.tick(t += 100); calm();
    };
    const auto& run = ui.charger_test_results();
    constexpr uint32_t kEprAvs = 0xD0000000u | (280u << 17) | (150u << 8) | 140;   // EPR AVS 15-28V 140W
    for (const auto& s : run.steps()) {
        if (s.algorithm != ct::Algorithm::Pps || s.pos != 6) continue;
        auto& w = const_cast<ct::Step&>(s);
        w.algorithm = ct::Algorithm::EprAvs; w.pos = 9; w.pdo = kEprAvs; w.avs = true;
    }
    const auto rebuild = [&] { top(ui, t); go(Button::Ok); go(Button::Back); };   // into a unit's Steps and back: the grid is rebuilt
    const uint16_t lines = result_layout::range_positions(run.steps());
    CHECK_EQ(unsigned(lines), unsigned(1u << 5 | 1u << 7 | 1u << 9));
    const specsec::Result r = spec_fixture();
    const_cast<ct::Evidence&>(run.evidence()).spec = r;
    const auto m = [](int32_t v, int32_t lo, int32_t hi, uint8_t pos) { return sc::Meas{v, 0, lo, hi, static_cast<uint8_t>(pos | 1u << 4), 50}; };
    const auto word = [&](uint8_t pos) { const specsec::Apdo* p = run.spec_apdo().find(pos); return p ? specsec::range_word(specsec::range_spec(*p)) : "-"; };
    rebuild();
    const int units_plain = ui.list_state().count - int(specsec::kCells);   // no range measured: nothing splits
    // Scene 1: a caution (an inferred limit), a timing and a level word; every measured range has a line, so both kinds split.
    ui.charger_test_spec_apdo_clear();
    ui.charger_test_spec_apdo(C::PpsBand, m(120000, 0, 275000, 5));
    ui.charger_test_spec_apdo(C::PpsSettle, m(90000, 0, 25000, 5));
    ui.charger_test_spec_apdo(C::AvsBand, m(120000, 0, 275000, 7));
    ui.charger_test_spec_apdo(C::AvsPsRdy, m(400000, 0, 275000, 7));
    ui.charger_test_spec_apdo(C::AvsAccuracy, m(16500, 14250, 15750, 9));
    CHECK_STR(word(5), "timing");   // the inferred settle limit: amber, not red
    CHECK_COND(specsec::range_spec(*run.spec_apdo().find(5)).tone == specsec::RangeTone::Caution);
    CHECK_STR(word(7), "timing");
    CHECK_STR(word(9), "level");
    rebuild();
    specsec::Split sp = specsec::split(r, run.spec_apdo(), lines);
    CHECK_COND(sp.pps && sp.avs);
    CHECK_EQ(ui.list_state().count, units_plain + int(specsec::shown_n(sp)));
    // Find each range line by its footer (a range that passed its own steps names the spec check behind its word).
    StrBuf<32> want;
    spectext::range_footer(want, *run.spec_apdo().find(9), specsec::range_spec(*run.spec_apdo().find(9)));
    top(ui, t); go(Button::Minus);
    int epr_row = -1, epr_press = 0;
    for (int n = 0; n < units_plain && epr_row < 0; ++n) {
        if (footer_shows(want.view(), kCaution)) { epr_row = ui.charger_test_cursor(); epr_press = n; }
        else go(Button::Plus);
    }
    CHECK_EQ(epr_row >= 0, true);
    if (epr_row < 0) return;
    renders("b_results");
    // The EPR AVS range's Steps page: the PD steps, then its 8 AVS checks; the accuracy is the first of them.
    go(Button::Ok);
    CHECK_EQ(ui.charger_test_stage(), 7);
    const int rows = ui.list_state().count;
    go(Button::Plus, rows);
    CHECK_EQ(ui.charger_test_cursor(), rows - 1);
    go(Button::Minus, 7);
    CHECK_EQ(footer_shows("AVS lvl: fail 1", kCaution), true);
    renders("b_steps_spec");
    go(Button::Ok);
    CHECK_EQ(ui.charger_test_stage(), 15);   // SpecDetail of the range's slot
    CHECK_EQ(footer_shows("fail 1 of 1 measured", kCaution), true);
    renders("b_spec_detail");
    go(Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 7);
    CHECK_EQ(ui.charger_test_cursor(), rows - 8);
    for (int row = rows - 8; row < rows; ++row) {   // every slot opens and draws whole; OK returns as BACK does
        top(ui, t); go(Button::Plus, row);
        go(Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 15);
        go(Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 7);
        CHECK_EQ(ui.charger_test_cursor(), row);
    }
    go(Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 6);
    CHECK_EQ(ui.charger_test_cursor(), epr_row);
    // The section: Transitions, then the observer as the last row.
    go(Button::Plus, units_plain + 40);
    CHECK_EQ(ui.charger_test_cursor(), units_plain + int(specsec::shown_n(sp)) - 1);
    renders("b_spec_end");
    go(Button::Ok);
    CHECK_EQ(ui.charger_test_stage(), 15);
    go(Button::Back);
    // Scene 2: "spec", a line with no range measured ("-") and "shape".
    ui.charger_test_spec_apdo_clear();
    ui.charger_test_spec_apdo(C::PpsBand, m(120000, 0, 275000, 5));
    ui.charger_test_spec_apdo(C::AvsOvershoot, m(29900, 20000, 29400, 9));
    CHECK_STR(word(5), "spec");
    CHECK_STR(word(7), "-");
    CHECK_STR(word(9), "shape");
    rebuild();
    top(ui, t); go(Button::Minus); go(Button::Plus, epr_press);
    renders("b_results_words");
    // Scene 3: a measured AVS range with no line (pos 10): the AVS kind keeps its merged rows, PPS still splits.
    ui.charger_test_spec_apdo(C::AvsBand, m(120000, 0, 275000, 10));
    rebuild();
    sp = specsec::split(r, run.spec_apdo(), lines);
    CHECK_COND(sp.pps && !sp.avs);
    CHECK_EQ(ui.list_state().count, units_plain + 9 + 7 + 1);
    // Scene 4: a range whose own steps failed keeps its own footer; its spec word stays in the cell.
    for (const auto& s : run.steps())
        if (s.algorithm == ct::Algorithm::EprAvs && !ct::is_sweep(s)) const_cast<ct::Step&>(s).verdict = ct::Verdict::Fail;
    rebuild();
    top(ui, t); go(Button::Minus); go(Button::Plus, epr_press);
    const auto rc = result_layout::reach(run.steps(), ct::Algorithm::EprAvs, 9);
    StrBuf<32> reach;
    reach.put("AVS: ").put(result_layout::reach_footer(rc).c_str());
    CHECK_COND(rc.tone != result_layout::Tone::Pass);
    CHECK_EQ(footer_shows(reach.view(), kCaution), true);
}
// The detail page of a check whose run the session's engine still holds (the live run, right after the Deep test): the limit, the count
// and the place come from the engine's aggregate, not the dim "-" a restored result shows. The stage's own result is built from the
// engine exactly as the Runner does (spec_collect), then the grid is rebuilt and a measured check's detail opened.
static void test_the_spec_detail_shows_the_limit_while_the_engine_holds_the_run() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    PdSpecSession& s = ui.pd_spec();
    CHECK_EQ(int(s.start_in_test(t)), int(pdspec::StartResult::Ok));
    for (int i = 0; i < 400 && s.running(); ++i) { t += 20; fusb302::service(); ui.tick(t); if (i == 150) s.cancel(); }
    CHECK_EQ(s.running(), false);
    const pdspec::Engine* e = s.engine_for_test();
    CHECK_EQ(e != nullptr, true);
    if (!e) return;
    specsec::Result r = specsec::from_aggs(e->agg(), specsec::End::Cancelled, static_cast<uint8_t>(e->stop()), e->release_unconfirmed());
    int k = -1;   // the first check the run measured and judged
    for (size_t i = 0; i < specsec::kCells; ++i)
        if (r.cells[i].measured() && e->agg()[static_cast<size_t>(specsec::kOrder[i])].n) { k = int(i); break; }
    CHECK_EQ(k >= 0, true);
    if (k < 0) return;
    const_cast<ct::Evidence&>(ui.charger_test_results().evidence()).spec = r;
    const int units = ui.list_state().count;
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t += 100); calm();   // into a unit's Steps and back: the grid is rebuilt
    press(ui, Button::Back, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 6);
    top(ui, t);
    for (int i = 0; i < units + k; ++i) press(ui, Button::Plus, t);
    ui.tick(t += 100); calm();
    press(ui, Button::Ok, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 15);   // SpecDetail
    dump_screen("spec_detail_live");
    const pdspec::Agg& a = e->agg()[static_cast<size_t>(specsec::kOrder[static_cast<size_t>(k)])];
    StrBuf<20> lim, cnt;
    spectext::put_limit(lim, pdspec::kDefs[static_cast<size_t>(specsec::kOrder[static_cast<size_t>(k)])], a.lo, a.hi);   // the engine's own bounds
    cnt.put_u32(a.n);
    const int row_y = [&](int row) { return kit::kBodyY + row * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height); }(2);
    check_text_at(kSmall, row_y, lim.view(), kText, kBg, kit::kKvValueX);                                  // Limit: the engine's bound
    check_text_at(kSmall, row_y + kit::kRowH, cnt.view(), kText, kBg, kit::kKvValueX);                    // Count
}
#if K2_SPECROWS
// The rows file the journal keeps for the run (chargertest/spec_rows.hpp): cell k lists per_cell[k] (fixed transitions, 10 bytes each).
static std::vector<uint8_t> spec_rows_image(uint32_t run_id, const std::vector<std::vector<specsec::Row>>& per_cell) {
    std::array<uint16_t, specsec::kCells> counts{};
    std::array<uint8_t, specsec::kCells> worst{};
    size_t total = 0;
    for (size_t k = 0; k < per_cell.size() && k < specsec::kCells; ++k) {
        counts[k] = static_cast<uint16_t>(per_cell[k].size());
        total += per_cell[k].size();
        for (size_t i = 0; i < per_cell[k].size(); ++i)
            if (specsec::rows_detail::rank(per_cell[k][i]) > specsec::rows_detail::rank(per_cell[k][worst[k]])) worst[k] = static_cast<uint8_t>(i);
    }
    std::vector<uint8_t> out(specsec::kRowsHdr + total * specsec::kRowBytes, 0);
    specsec::rows_put_header(out.data(), run_id, counts, worst);
    size_t at = specsec::kRowsHdr;
    for (const auto& cell : per_cell)
        for (const specsec::Row& r : cell) { specsec::row_encode(r, out.data() + at); at += specsec::kRowBytes; }
    return out;
}
static specsec::Row fixed_trans_row(size_t cell, unsigned from_mv, unsigned to_mv, int value, int margin, bool fail) {
    const pdspec::Unit u = pdspec::kDefs[static_cast<size_t>(specsec::kOrder[cell])].unit;
    specsec::Row r;
    r.check = static_cast<uint8_t>(specsec::kOrder[cell]);
    r.flags = fail ? specsec::kRowFail : 0;
    r.a = static_cast<uint16_t>(from_mv);
    r.b = static_cast<uint16_t>(to_mv);
    r.value = specsec::quantize(u, value);
    r.margin = specsec::quantize(u, margin);
    return r;
}
// OK on a check opens its detail: a list of its transitions when the journal's rows file has two or more of them and agree with the verdict
// (the list's rows are the cell's count, PLUS / MINUS move the cursor, the footer names the worst), else the single-measurement summary.
static void test_the_spec_detail_lists_the_transitions_of_the_journal() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    const int units = ui.list_state().count;   // the unit rows: read before the spec result is set
    const_cast<ct::Evidence&>(ui.charger_test_results().evidence()).spec = spec_fixture();
    const uint32_t run = ui.charger_test_results().run_id();
    std::vector<std::vector<specsec::Row>> cells(specsec::kCells);
    cells[0] = {fixed_trans_row(0, 5000, 9000, 1200, -300, false), fixed_trans_row(0, 9000, 12000, 900, -600, false),
                fixed_trans_row(0, 12000, 15000, 1500, -100, false), fixed_trans_row(0, 15000, 20000, 1100, -400, false)};   // a pass
    cells[1] = {fixed_trans_row(1, 5000, 9000, 90000, -10000, false), fixed_trans_row(1, 9000, 12000, 540000, 40000, true),
                fixed_trans_row(1, 12000, 15000, 120000, -9000, false)};   // a fail: one of three
    for (unsigned i = 0; i < 20; ++i) cells[6].push_back(fixed_trans_row(6, 5000 + 500 * i, 5500 + 500 * i, 400 + 3 * i, -90 + int(i), false));   // a long list
    {   // a programmable check: the margin is a count of failed steps, unsigned ("3 bad"), and keeps one space in the footer
        const uint8_t chk = static_cast<uint8_t>(specsec::kOrder[19]);
        specsec::Row ok, bad;
        ok.check = bad.check = chk;
        ok.flags = specsec::kRowProg;
        bad.flags = specsec::kRowProg | specsec::kRowFail;
        ok.a = bad.a = 5000; ok.b = bad.b = 9000;
        ok.value = bad.value = 0;
        ok.margin = 0; bad.margin = 3;
        cells[19] = {ok, bad};
    }
    cells[7] = {fixed_trans_row(7, 5000, 9000, 9210, 210, false), fixed_trans_row(7, 9000, 12000, 12040, 40, false)};   // a FAIL verdict with no failing row: the summary
    cells[4] = {fixed_trans_row(4, 5000, 9000, 4000, -100, false)};   // one row: the summary
    g_stub.journal_rows = spec_rows_image(run, cells);
    g_stub.journal_rows_noted = false;   // the run's own note_spec() (if any) must not make the stub pump rewrite the file from the engine
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t += 100); calm();   // into a unit's Steps and back: the grid is rebuilt
    press(ui, Button::Back, t); ui.tick(t += 100); calm();
    const auto open_check = [&](int cell) {
        top(ui, t);
        for (int i = 0; i < units + cell; ++i) press(ui, Button::Plus, t);
        ui.tick(t += 100); calm();
        press(ui, Button::Ok, t); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 15);   // SpecDetail
    };
    const auto close_check = [&] { press(ui, Button::Back, t); ui.tick(t += 100); calm(); CHECK_EQ(ui.charger_test_stage(), 6); };
    open_check(0);
    CHECK_EQ(ui.list_state().count, 4);
    CHECK_EQ(ui.charger_test_cursor(), 0);
    dump_screen("spec_transitions_pass");
    CHECK_EQ(footer_shows("worst 12→15V: 1.5ms", kOk), true);   // the pass: the largest value, no margin
    press(ui, Button::Plus, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_cursor(), 1);
    press(ui, Button::Minus, t); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_cursor(), 0);
    close_check();
    open_check(1);
    CHECK_EQ(ui.list_state().count, 3);
    dump_screen("spec_transitions_fail");
    CHECK_EQ(footer_shows("worst 9→12V: 540ms +40ms", kError), true);   // the failing transition, the margin in 27 cells
    close_check();
    open_check(19);
    CHECK_EQ(ui.list_state().count, 2);
    CHECK_EQ(footer_shows("worst 5-9V 100mV: 0mV 3 bad", kError), true);   // an unsigned margin: one space, no "+"
    close_check();
    open_check(6);
    CHECK_EQ(ui.list_state().count, 20);
    for (int i = 0; i < 16; ++i) press(ui, Button::Plus, t);
    ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_cursor(), 16);
    dump_screen("spec_transitions_long");
    for (int i = 0; i < 20; ++i) press(ui, Button::Plus, t);
    ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_cursor(), 19);   // stops on the last
    close_check();
    // A verdict that disagrees with the rows, or one row alone, or no row: the summary table, no list.
    for (const int cell : {7, 4, 2, 3}) {
        open_check(cell);
        CHECK_EQ(ui.list_state().count, 0);
        close_check();
    }
    // Another run's file is not this run's: no list anywhere.
    g_stub.journal_rows = spec_rows_image(run + 1, cells);
    open_check(0); CHECK_EQ(ui.list_state().count, 0); close_check();
    g_stub.journal_rows.clear();
    open_check(0); CHECK_EQ(ui.list_state().count, 0); close_check();
}
#else
// A build without K2_SPECROWS never lists transitions, whatever the stub journal holds: OK on a check opens the seven-row summary table
// (no list rows, the Limit row drawn from the cell), for a passing and a failing check alike.
static void test_the_spec_detail_is_the_summary_table_without_transition_rows() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); CHECK_EQ(ui.painting(), false); };
    const int units = ui.list_state().count;   // the unit rows: read before the spec result is set
    const specsec::Result spec = spec_fixture();
    const_cast<ct::Evidence&>(ui.charger_test_results().evidence()).spec = spec;
    g_stub.journal_rows.assign(256, 0x5A);   // a file a K2_SPECROWS build would have written: it is never read
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t += 100); calm();   // into a unit's Steps and back: the grid is rebuilt
    press(ui, Button::Back, t); ui.tick(t += 100); calm();
    for (const int k : {0, 1}) {   // fixture: cell 0 passes, cell 1 fails
        top(ui, t);
        for (int i = 0; i < units + k; ++i) press(ui, Button::Plus, t);
        ui.tick(t += 100); calm();
        press(ui, Button::Ok, t); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 15);   // SpecDetail
        CHECK_EQ(ui.list_state().count, 0);
        StrBuf<20> v; uint16_t color = kText;
        spectext::detail_value(2, static_cast<int>(specsec::kOrder[static_cast<size_t>(k)]), spec.cells[static_cast<size_t>(k)], v, color);
        const int row_y = kit::kBodyY + 2 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height);
        check_text_at(kSmall, row_y, v.view(), color, kBg, kit::kKvValueX);   // the Limit row
        press(ui, Button::Back, t); ui.tick(t += 100); calm();
        CHECK_EQ(ui.charger_test_stage(), 6);
    }
}
#endif
// A Deep run in the host stub: the PD spec section runs the engine against the stub's source and, when the run ends, the detail pages list
// the transitions from the engine (no journal file read); the stub's source gives the engine none, so no rows file is written.
static void test_a_deep_run_lists_its_transitions_from_the_engine() {
    six_pdo_fixture(); Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_level(2), true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 3600000 && ui.charger_test_busy(); t += 20) { fusb302::service(); ui.tick(t); }
    const pdspec::Engine* e = ui.pd_spec().engine_for_test();
    // The host stub's source gives the engine no transitions, so the run is over, the section is shown and no rows file was asked for.
    CHECK_EQ(ui.charger_test_stage(), 6);
    CHECK_EQ(g_stub.journal_rows.empty(), true);
    CHECK_EQ(e != nullptr, true);
#if K2_SPECROWS
    if (e) CHECK_EQ(specsec::rows_total(*e), size_t(0));
#endif
}
// A voltage fold is a caution (user decision 2026-11-16): the sweep row and the APDO's fixed rows on the folded side read amber,
// neither pass nor fail. The sweep itself is a clean run here; the fold is set on the finished run (the host stub cannot make a
// converter fold back), then the Results grid is rebuilt. Checks the header's fourth cell, the Steps footer and the Detail page.
// The Running header's counter beside the bar: executed rows done of executed rows (result_layout::run_progress), the numbers the
// bar draws. A plan-time skip (a level the K2's own ceiling cannot reach) is no work: it is in neither number.
static bool counter_says(std::string_view text) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_text(kContent0, kSmall, 6, 4, text, kText, kBg);
    unsigned diff = 0;
    for (int y = 4; y < 4 + kSmall.height; ++y)
        for (int x = 6; x < 6 + text_width(kSmall, text); ++x) diff += actual[y * LCD_WIDTH + kOx + x] != PANEL[y * LCD_WIDTH + x];
    std::copy(actual.begin(), actual.end(), g_panel);
    return diff == 0;
}
static void test_the_running_counter_counts_executed_rows_like_the_bar() {
    six_pdo_fixture();
    g_stub.vbus_ceiling_mv = 12000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    unsigned checked = 0, skips_seen = 0;
    for (uint32_t t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) {
        ui.tick(t);
        if (ui.charger_test_stage() != 5 || ui.painting()) continue;
        const auto steps = ui.charger_test_results().steps();
        const auto work = result_layout::run_progress(steps);
        skips_seen += std::count_if(steps.begin(), steps.end(), [](const ct::Step& s) { return s.verdict == ct::Verdict::Skip; }) != 0;
        StrBuf<16> want;
        want.put_u32(work.done).put_char('/').put_u32(work.total).put(" steps");
        CHECK_EQ(counter_says(want.view()), true);
        ++checked;
    }
    CHECK_EQ(checked > 3, true);
    CHECK_EQ(skips_seen > 0, true);   // the run did plan-time skips, or the check above proves nothing
}
// The family Steps pages of a source that offers fixed 5/9/15/20 V, two PPS ranges, SPR AVS and EPR fixed 28 V, behind a converter
// that follows every request (a clean PD-only run with the sweep on): every Results line opened, at the four rotations, plus the
// list scrolled. K2_DUMP=dir writes dir/k2_fam_rot<r>_line<k>[_scrolled].panel (tools/panel2png.py makes the PNGs); without K2_DUMP
// it walks the scenes. The whole truncation walk (every UI test fails on a cut) runs over these screens.
static void family_offer() {
    constexpr uint32_t kOffer[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xC1A42164, 0xE004B000, 0x0008C1F4};
    fixture();
    g_stub.vbus_ceiling_mv = 36000;
    g_stub.caps.num_pdos = 8; g_stub.caps.epr = true;
    for (unsigned i = 0; i < 8; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(kOffer[i]);
}
// The Results line of the first row called `label`, and what its Steps page must say: how many steps it lists, their tally and the
// place of the first caution among them. -1 line when no such row. Built from the finished run the way the page builds its grid.
struct FamilyPick {
    int line = -1;
    unsigned count = 0;
    int first_caution = -1;
    uint8_t algo = 0, pos = 0;   // the row's own APDO (a range row)
    result_layout::FamilyTally tally;
};
static FamilyPick pick_family(Ui& ui, const char* label) {
    FamilyPick pick;
    const auto steps = ui.charger_test_results().steps();
    const auto view = ui.charger_test_results().sweep_view();
    result_layout::Layout layout;
    result_layout::OemNotes oem; oem.headers = false;
    result_layout::build(layout, steps, oem, view);
    std::array<result_layout::Line, result_layout::kMaxLines> ln;
    const size_t line_n = result_layout::wrap(layout, result_layout::kGridMetrics, ln);
    // A kind word ("PPS", "AVS") picks the first line of the PD block that holds a chip of that kind: its APDO is algo / pos.
    for (size_t k = 0; k < line_n && pick.line < 0; ++k) {
        if (std::string_view(layout.rows[ln[k].row].label) == label) pick.line = int(k);
        for (size_t i = ln[k].first; pick.line < 0 && i < size_t(ln[k].first) + ln[k].count; ++i) {
            const auto& t = layout.tokens[i];
            if (t.kind && std::string_view(result_layout::kind_word(t.kind)) == label) {
                pick.line = int(k);
                pick.algo = uint8_t(result_layout::kind_algorithm(t.kind)); pick.pos = uint8_t(t.mv);
            }
        }
    }
    if (pick.line < 0) return pick;
    const auto& row = layout.rows[ln[size_t(pick.line)].row];
    if (!pick.algo) { pick.algo = uint8_t(row.algorithm); pick.pos = row.pos; }
    for (const auto& s : steps) {
        if (!result_layout::row_has(row, s)) continue;
        const bool caution = result_layout::caution(s, view);
        if (caution && pick.first_caution < 0) pick.first_caution = int(pick.count);
        ++pick.count;
        pick.tally.caution += caution;
        pick.tally.pass += s.verdict == ct::Verdict::Pass && !caution;
        pick.tally.fail += s.verdict == ct::Verdict::Fail;
        pick.tally.listed += result_layout::advertised(s);
        pick.tally.skip += s.verdict == ct::Verdict::Skip && !result_layout::advertised(s);
    }
    return pick;
}
// The y of the first list row's top on a family Steps page: the grid header sits above it. The cursor is on the first row, so the focus
// bar (x 0..2) starts 1 px under that row's top hairline.
static int family_list_top() {
    int y = kit::kBodyY;
    while (y < LCD_HEIGHT && PANEL[y * LCD_WIDTH + 1] != kFocus) ++y;
    return y - 1;
}
static void test_family_steps_scenes() {
    family_offer();
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] {
        for (int i = 0; i < 80 && ui.painting(); ++i) ui.tick(t += 20);
        CHECK_EQ(ui.painting(), false);
    };
    ui.tick(t += 200); ui.tick(t += 200); calm();
    const int lines = ui.list_state().count;
    CHECK_EQ(lines >= 5, true);
    // The same layout the page built: the rows and lines the Results cursor walks (no section headers: the grid is units only).
    const auto steps = ui.charger_test_results().steps();
    const auto view = ui.charger_test_results().sweep_view();
    result_layout::Layout layout;
    result_layout::OemNotes oem; oem.headers = false;
    result_layout::build(layout, steps, oem, view);
    std::array<result_layout::Line, result_layout::kMaxLines> ln;
    const size_t line_n = result_layout::wrap(layout, result_layout::kGridMetrics, ln);
    CHECK_EQ(int(line_n), lines);
    // A line of the grid is tall (a current under the volts) when one of its chips carries a current after the space.
    const auto line_h = [&](const result_layout::Line& l) {
        for (size_t i = l.first; i < size_t(l.first) + l.count; ++i)
            if (layout.tokens[i].chip && std::strchr(layout.tokens[i].text, ' ')) return int(kit::kResultRowHCur);
        return int(kit::kResultRowH);
    };
    for (uint8_t rot = 0; rot < 4; ++rot) {
        ui.set_rotation(rot, t); calm();
        top(ui, t); calm();
        {
            char name[64];
            std::snprintf(name, sizeof name, "fam_rot%u_results", unsigned(rot));
            dump_screen(name);
        }
        for (int k = 0; k < lines; ++k) {
            const auto& row = layout.rows[ln[size_t(k)].row];
            top(ui, t); move(ui, unsigned(k), t); calm();
            press(ui, Button::Ok, t); calm();
            CHECK_EQ(ui.charger_test_stage(), 7);
            if (ui.charger_test_stage() != 7) { press(ui, Button::Back, t); calm(); continue; }
            // Only this row's steps: its own range, level family or sweep, not the whole protocol family.
            unsigned want = 0, swept = 0, ranged = 0;
            result_layout::FamilyTally tally;
            for (const auto& s : steps) {
                if (!result_layout::row_has(row, s)) continue;
                ++want;
                (ct::is_sweep(s) ? swept : ranged) += 1;
                const bool caution = result_layout::caution(s, view);
                tally.caution += caution;
                tally.pass += s.verdict == ct::Verdict::Pass && !caution;
                tally.fail += s.verdict == ct::Verdict::Fail;
                tally.listed += result_layout::advertised(s);
                tally.skip += s.verdict == ct::Verdict::Skip && !result_layout::advertised(s);
            }
            CHECK_EQ(want > 0, true);
            bool marked = false;
            for (size_t i = row.first; i < size_t(row.first) + row.count; ++i) marked = marked || layout.tokens[i].sweep;
            if (marked) {
                CHECK_EQ(swept >= 1, true);   // a chip marked S: its Steps page lists the sweep beside the range steps
                CHECK_EQ(ranged >= 1, true);
            }
            CHECK_EQ(unsigned(ui.list_state().count), want);
            if (want < steps.size()) CHECK_EQ(ui.list_state().count < int(steps.size()), true);
            const auto foot = result_layout::family_footer(row.label, tally);
            const uint16_t tone = tally.fail || tally.caution ? kCaution : tally.pass || tally.listed ? kOk : kText;
            if (rot == 0) CHECK_EQ(footer_shows(foot.view(), tone), true);   // the footer helper reads the panel as it is at rotation 0
            char name[64];
            std::snprintf(name, sizeof name, "fam_rot%u_line%d", unsigned(rot), k);
            dump_screen(name);
            if (rot == 0) {   // the grid header is the picked row's own lines (at most two); the list starts under it (its hairline, then the focus bar at x 0..2)
                int first = k;
                while (first > 0 && !ln[size_t(first)].head) --first;
                int h = 0, n = 0;
                if (row.pd) { first = k; h = line_h(ln[size_t(k)]); n = 2; }   // the PD block's header is the opened line alone
                while (n < 2 && first + n < int(line_n) && ln[size_t(first + n)].row == ln[size_t(first)].row && (n == 0 || !ln[size_t(first + n)].head)) h += line_h(ln[size_t(first + n++)]);
                int y = 2;
                while (y < 240 && PANEL[y * LCD_WIDTH + 1] != kFocus) ++y;
                CHECK_EQ(y, 2 + h + 1);
                press(ui, Button::Ok, t); calm();   // a step's detail and back: the header is still there
                CHECK_EQ(ui.charger_test_stage(), 8);
                press(ui, Button::Back, t); calm();
                CHECK_EQ(ui.charger_test_stage(), 7);
                y = 2;
                while (y < 240 && PANEL[y * LCD_WIDTH + 1] != kFocus) ++y;
                CHECK_EQ(y, 2 + h + 1);
            }
            const int cnt = ui.list_state().count;
            if (cnt > ui.list_state().visible) {
                move(ui, 8, t); calm();
                std::snprintf(name, sizeof name, "fam_rot%u_line%d_scrolled", unsigned(rot), k);
                dump_screen(name);
            }
            press(ui, Button::Back, t); calm();
            CHECK_EQ(ui.charger_test_stage(), 6);
            CHECK_EQ(int(ui.charger_test_cursor()), k);   // Back lands on the row it left
        }
    }
}
// A family with more steps than the room under its grid header (the PPS ranges of the run: seven steps): the page scrolls its list
// under the fixed header, at the four rotations; the header stays and the list's last step is reachable.
static void test_a_long_family_list_scrolls_under_its_header() {
    family_offer();
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] {
        for (int i = 0; i < 80 && ui.painting(); ++i) ui.tick(t += 20);
        CHECK_EQ(ui.painting(), false);
    };
    ui.tick(t += 200); ui.tick(t += 200); calm();
    const int lines = ui.list_state().count;
    // The longest family page of the run.
    int best = -1, best_count = 0;
    for (int k = 0; k < lines; ++k) {
        top(ui, t); move(ui, unsigned(k), t); calm();
        press(ui, Button::Ok, t); calm();
        if (ui.charger_test_stage() == 7 && ui.list_state().count > best_count) { best_count = ui.list_state().count; best = k; }
        press(ui, Button::Back, t); calm();   // (a spec line opens its own detail, not a step list)
    }
    CHECK_EQ(best_count > 6, true);   // a header of one grid line leaves the list room for six rows
    if (best < 0) return;
    for (uint8_t rot = 0; rot < 4; ++rot) {
        ui.set_rotation(rot, t); calm();
        top(ui, t); move(ui, unsigned(best), t); calm();
        press(ui, Button::Ok, t); calm();
        CHECK_EQ(ui.charger_test_stage(), 7);
        char name[64];
        std::snprintf(name, sizeof name, "fam_long_rot%u", unsigned(rot));
        dump_screen(name);
        if (rot == 0) {   // a turn of the device on the page and back: the header is laid out again, the list under it where it was
            const int y0 = family_list_top();
            ui.set_rotation(1, t); calm();
            ui.set_rotation(0, t); calm();
            CHECK_EQ(family_list_top(), y0);
            CHECK_EQ(ui.charger_test_stage(), 7);
        }
        move(ui, unsigned(best_count - 1), t); calm();   // the last step: the window scrolled, the header did not
        CHECK_EQ(ui.list_state().count > ui.list_state().visible, true);
        CHECK_EQ(ui.list_state().offset > 0, true);
        std::snprintf(name, sizeof name, "fam_long_rot%u_scrolled", unsigned(rot));
        dump_screen(name);
        press(ui, Button::Back, t); calm();
        CHECK_EQ(ui.charger_test_stage(), 6);
    }
}
static void test_a_voltage_fold_reads_as_a_caution() {
    six_pdo_fixture();
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    int where = 0;
    const auto calm = [&] {
        ++where;
        for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100);
        CHECK_EQ(ui.painting(), false);
    };
    ui.tick(t += 200); ui.tick(t += 200);
    calm();
    const auto& r = ui.charger_test_results();
    // The fold, set on the finished run: the sweep row and the PPS fixed rows at or above 10.4 V (the folded side) turn caution.
    unsigned marked = 0, sweeps = 0;
    for (size_t i = 0; i < r.steps().size(); ++i) {
        auto& s = const_cast<ct::Step&>(r.steps()[i]);
        if (s.group != ct::Group::Pd || !(ct::kPpsAlgorithms & ct::bit(s.algorithm))) continue;
        if (ct::is_sweep(s)) {
            ++sweeps;
            CHECK_EQ(int(s.verdict), int(ct::Verdict::Pass));
            if (ct::sweep_low(s)) continue;   // the low part (below the split) folds nothing
            // The fold: the last five points of the high part fall away (the converter folds back above 10.5 V).
            for (const auto& rw : r.sweep_view().rows)
                if (rw.pos == s.pos && rw.alg == uint8_t(s.algorithm) && rw.lo_mv >= ctsweep::kSplitMv && rw.n >= 5)
                    for (unsigned k = rw.n - 5; k < rw.n; ++k) {
                        const unsigned set = rw.lo_mv + k * ctsweep::kStepMv;
                        const_cast<int16_t&>(r.sweep_view().points[rw.first + k]) = int16_t(10000 - int(set) - int(k - (rw.n - 5)) * 200);
                    }
        } else if (s.verdict != ct::Verdict::Pass || s.mv < 10400) continue;
        s.reason = ct::Reason::Folded;
        ++marked;
    }
    CHECK_EQ(sweeps >= 1, true);
    CHECK_EQ(marked >= 2, true);   // the sweep row and at least one fixed PPS row
    unsigned caution = 0, pass = 0;
    for (const auto& s : r.steps()) { const bool c = result_layout::caution(s); caution += c; pass += s.verdict == ct::Verdict::Pass && !c; }
    CHECK_EQ(caution, marked);
    // Rebuild the Results grid: into the Steps of a row and back.
    top(ui, t); calm();
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 7);
    press(ui, Button::Back); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 6);
    top(ui, t); calm();
    dump_screen("fold_results");
    CHECK_EQ(amber_chip_sweep_gap() >= 6, true);   // the S mark never hugs the label: at least one font-12 advance in every rotation
    const auto readme_rot3 = [&](const char* name, bool results = true) {   // the README's picture of the same screen, rotation 3 (the strip at the bottom)
        ui.set_rotation(3, t); calm();
        dump_screen(name);
        if (results) CHECK_EQ(amber_chip_sweep_gap() >= 6, true);   // the Results grid has the chip; the Detail page does not
        ui.set_rotation(0, t); calm();
    };
    readme_rot3("readme_ct_caution");
    for (const uint8_t rot : {1, 2}) {   // the merged chip with its S mark in the landscape mappings too (every cut would fail the run)
        ui.set_rotation(rot, t); calm();
        dump_screen(rot == 1 ? "fold_results_rot1" : "fold_results_rot2");
        CHECK_EQ(amber_chip_sweep_gap() >= 6, true);
        ui.set_rotation(0, t); calm();
    }
    // The header: pass / caution / fail / skip; the caution cell is amber (four columns: the number only, behind its amber mark).
    StrBuf<8> n; n.put_u32(caution);
    check_text_at(kSmall, 4, n.view(), kCaution, kBg, 68);
    n.clear(); n.put_u32(pass);
    check_text_at(kSmall, 4, n.view(), kOk, kBg, 16);
    // Steps of the PPS row: the footer counts that family's cautions, in the caution tone ("PPS: n pass, m caution").
    const auto pps = pick_family(ui, "PPS");
    CHECK_EQ(pps.line >= 0, true);
    if (pps.line < 0) return;
    CHECK_EQ(pps.tally.caution >= 1, true);
    CHECK_EQ(pps.first_caution >= 0, true);
    top(ui, t); calm();
    for (int k = 0; k < pps.line; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
    {   // the merged row on the Results screen: its footer carries the sweep's fold words, in the caution tone (the Sweep row's old job)
        char fold[ctsweep::kFoldTextMax] = {};
        const auto rw = r.sweep_view().apdo(pps.algo, pps.pos);
        const size_t fl = rw ? ctsweep::format_fold(rw->sum, fold) : 0;
        CHECK_EQ(fl > 0, true);
        CHECK_EQ(view_says(ui, std::string_view(fold, fl)), true);
    }
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 7);
    CHECK_EQ(unsigned(ui.list_state().count), pps.count);
    top(ui, t); calm();
    CHECK_EQ(view_says(ui, result_layout::family_footer("PD", pps.tally).view()), true);
    {   // scroll to the first caution row of the list and keep that screen for the design review
        for (int k = 0; k < pps.first_caution; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
        dump_screen("fold_steps");
        top(ui, t); calm();
    }
    // Walk the steps to one that reads amber and open its Detail: an amber CAUTION band, the reason and the fold's own words.
    bool detail = false;
    for (int k = 0; k < ui.list_state().count && !detail; ++k) {
        press(ui, Button::Ok); ui.tick(t += 100); calm();
        if (ui.charger_test_stage() == 8 && PANEL[24 * LCD_WIDTH + 100] == kPartFill) detail = true;
        else { press(ui, Button::Back); ui.tick(t += 100); calm(); press(ui, Button::Plus); ui.tick(t += 100); calm(); }
    }
    CHECK_EQ(detail, true);
    if (!detail) return;
    dump_screen("fold_detail");
    readme_rot3("readme_ct_caution_detail", false);
    // The long word and the long name ("PPS 7.14->11V") share the band: the word drops to the small font and nothing overlaps.
    check_text_at(kSmall, 12, "CAUTION", kOnPart, kPartFill, 32);
    char want[ctsweep::kFoldTextMax] = {};
    const ct::Step* st = nullptr;
    for (const auto& s : r.steps()) if (result_layout::caution(s)) { st = &s; break; }
    CHECK_EQ(st != nullptr, true);
    if (!st) return;
    const auto rw = r.sweep_view().apdo(uint8_t(st->algorithm), st->pos);
    CHECK_EQ(rw.has_value(), true);
    const size_t wl = rw ? ctsweep::format_fold(rw->sum, want) : 0;
    CHECK_EQ(wl > 0, true);
    CHECK_STR(want, "folds to 9.2V above 10.5V");   // the last five points fall 200 mV each from 10 V: the lowest is 9.2 V
    CHECK_EQ(view_says(ui, std::string_view(want, wl)), true);   // the fold's own words, from the APDO's sweep
    check_text_at(kSmall, ty_reason(), "folds back", kCaution, kBg, 76);
}
// A finished PPS or AVS level row given a caution reason is a caution on its range chip, never a chip of its own and never red: the family
// count, the focused row's note and the Detail footer carry the words. `label` picks the range family, `algo` the row the reason is set on; the
// Steps page and the Detail page are dumped in all four rotations (every cut fails the run), K2_DUMP names <tag>_steps[_rot<r>] / <tag>_detail[_rot<r>].
static void caution_scene(const char* label, ct::Algorithm algo, ct::Reason reason, uint16_t detail, const char* tag, const char* footer_words) {
    if (algo == ct::Algorithm::SprAvs) family_offer(); else six_pdo_fixture();   // the six-PDO fixture has a PPS range only
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] {
        for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100);
        CHECK_EQ(ui.painting(), false);
    };
    const auto all_rotations = [&](const char* scene) {
        char name[48];
        dump_screen(scene);
        for (const uint8_t rot : {1, 2, 3}) {
            ui.set_rotation(rot, t); calm();
            std::snprintf(name, sizeof name, "%s_rot%u", scene, unsigned(rot));
            dump_screen(name);
        }
        ui.set_rotation(0, t); calm();
    };
    ui.tick(t += 200); ui.tick(t += 200);
    calm();
    const auto& r = ui.charger_test_results();
    unsigned before = 0;
    for (const auto& s : r.steps()) before += result_layout::caution(s, r.sweep_view());
    ct::Step* row = nullptr;
    for (size_t i = 0; i < r.steps().size() && !row; ++i) {
        auto& s = const_cast<ct::Step&>(r.steps()[i]);
        if (s.group == ct::Group::Pd && s.algorithm == algo && !ct::is_sweep(s) && s.verdict == ct::Verdict::Pass &&
            !result_layout::caution(s, r.sweep_view()))
            row = &s;
    }
    CHECK_EQ(row != nullptr, true);
    if (!row) return;
    row->reason = reason; row->detail = detail;
    unsigned after = 0;
    for (const auto& s : r.steps()) after += result_layout::caution(s, r.sweep_view());
    CHECK_EQ(after, before + 1);
    CHECK_EQ(int(row->verdict), int(ct::Verdict::Pass));   // a caution, the levels passed
    top(ui, t); calm();
    press(ui, Button::Ok); ui.tick(t += 100); calm();   // into a row and back: the Results grid is built again from the marked run
    press(ui, Button::Back); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 6);
    top(ui, t); calm();
    char scene[40];
    const auto fam = pick_family(ui, label);
    CHECK_EQ(fam.line >= 0, true);
    if (fam.line < 0) return;
    CHECK_EQ(fam.tally.caution >= 1, true);
    CHECK_EQ(fam.first_caution >= 0, true);
    for (int k = 0; k < fam.line; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
    std::snprintf(scene, sizeof scene, "%s_results", tag);
    all_rotations(scene);   // the amber range chip on the Results grid, its line focused
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 7);
    top(ui, t); calm();
    CHECK_EQ(view_says(ui, result_layout::family_footer("PD", fam.tally).view()), true);
    for (int k = 0; k < fam.first_caution; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }   // the focused row opens its note
    std::snprintf(scene, sizeof scene, "%s_steps", tag);
    all_rotations(scene);
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 8);
    CHECK_EQ(view_says(ui, footer_words), true);
    std::snprintf(scene, sizeof scene, "%s_detail", tag);
    all_rotations(scene);
}
// A step walk the source answered at a coarse quantum (a bank landing 100 mV for a +20 mV ask) is a caution on its range chip, never a
// chip of its own and never red: the footer and the Detail footer carry "steps 100mV, not 20mV". An AVS row steps 100 mV, so 200 mV misses.
static void test_a_step_walk_miss_reads_as_a_caution() {
    caution_scene("PPS", ct::Algorithm::Pps, ct::Reason::StepResolution, 100, "walk", "steps 100mV, not 20mV");
    caution_scene("AVS", ct::Algorithm::SprAvs, ct::Reason::StepResolution, 200, "avs_walk", "steps 200mV, not 100mV");
}
// A Deep transition check that found a spec limit broken (Reason::ProgCheck, flag bits in detail) is a caution on a finished PPS or AVS
// level row: the family count, the focused row's note ("CAUTION: overshoot +1 PDO #n") and the Detail footer say it; never red, never a chip.
static void test_a_transition_flag_reads_as_a_caution() {
    caution_scene("PPS", ct::Algorithm::Pps, ct::Reason::ProgCheck, 3, "prog", "overshoot +1");   // overshoot + slow rise
    caution_scene("AVS", ct::Algorithm::SprAvs, ct::Reason::ProgCheck, 3, "avs_prog", "overshoot +1");
}
// A passing sweep that tracked worse than 100 mV is a caution as well (result_layout::deviates, user decision 2026-11-16): the header's
// fourth cell, the Steps footer and the focused row's note count it, and the chip is amber, never red, so "0 fail" never sits over a
// red cell. The host stub tracks cleanly, so the run's summaries are set to a 250 mV error on the finished run, then the grid is rebuilt.
static void test_a_sweep_off_by_more_than_100mv_reads_as_a_caution() {
    six_pdo_fixture();
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] {
        for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100);
        CHECK_EQ(ui.painting(), false);
    };
    ui.tick(t += 200); ui.tick(t += 200);
    calm();
    const auto& r = ui.charger_test_results();
    unsigned before = 0;
    for (const auto& s : r.steps()) before += result_layout::caution(s, r.sweep_view());
    for (const auto& rw : r.sweep_view().rows) {   // every recorded part: a steady 250 mV shortfall (within the 300 mV the old chip called amber)
        auto& m = const_cast<ctsweep::RowView&>(rw);
        m.sum.worst_err = -250; m.sum.min_err = -250; m.sum.worst_mv = m.lo_mv;
    }
    unsigned caution = 0, pass = 0, fail = 0, sweeps = 0;
    for (const auto& s : r.steps()) {
        const bool c = result_layout::caution(s, r.sweep_view());
        caution += c; pass += s.verdict == ct::Verdict::Pass && !c; fail += s.verdict == ct::Verdict::Fail;
        if (ct::is_sweep(s) && s.verdict == ct::Verdict::Pass) { ++sweeps; CHECK_EQ(c, true); CHECK_EQ(result_layout::deviates(s, r.sweep_view()), true); }
    }
    CHECK_EQ(sweeps >= 1, true);
    CHECK_EQ(caution >= before + sweeps, true);
    top(ui, t); calm();
    press(ui, Button::Ok); ui.tick(t += 100); calm();   // into the Steps and back: the Results grid is rebuilt
    press(ui, Button::Back); ui.tick(t += 100); calm();
    top(ui, t); calm();
    StrBuf<8> n; n.put_u32(caution);
    check_text_at(kSmall, 4, n.view(), kCaution, kBg, 68);        // the header's caution cell holds the deviating sweeps
    n.clear(); n.put_u32(pass);
    check_text_at(kSmall, 4, n.view(), kOk, kBg, 16);
    // The PPS row's steps page lists the range steps and the APDO's sweep, and counts its deviating sweep as a caution.
    const auto sw = pick_family(ui, "PPS");
    CHECK_EQ(sw.line >= 0, true);
    if (sw.line < 0) return;
    CHECK_EQ(sw.tally.caution >= 1, true);
    for (int k = 0; k < sw.line; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    CHECK_EQ(ui.charger_test_stage(), 7);
    CHECK_EQ(unsigned(ui.list_state().count), sw.count);
    top(ui, t); calm();
    CHECK_EQ(view_says(ui, result_layout::family_footer("PD", sw.tally).view()), true);
}
// A sweep part that failed (the source was lost mid-sweep) paints the merged chip red; the Results footer of that row must then speak
// of the sweep in the caution tone, never of the range's reach alone ("PPS: 3.3-11V reached" in green under a red chip): the
// peak it did record, or "sweep failed" when no point was recorded at all.
static void test_a_failed_sweep_footer_never_reads_as_reached() {
    six_pdo_fixture();
    g_stub.vbus_ceiling_mv = 36000;
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    ui.charger_test_set_sweep(true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    const auto calm = [&] {
        for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100);
        CHECK_EQ(ui.painting(), false);
    };
    ui.tick(t += 200); ui.tick(t += 200);
    calm();
    const auto& r = ui.charger_test_results();
    for (size_t i = 0; i < r.steps().size(); ++i) {   // every sweep part of the run lost its source
        auto& s = const_cast<ct::Step&>(r.steps()[i]);
        if (ct::is_sweep(s)) s.verdict = ct::Verdict::Fail;
    }
    top(ui, t); calm();
    press(ui, Button::Ok); ui.tick(t += 100); calm();   // into the Steps and back: the Results grid is rebuilt
    press(ui, Button::Back); ui.tick(t += 100); calm();
    top(ui, t); calm();
    const auto pps = pick_family(ui, "PPS");
    CHECK_EQ(pps.line >= 0, true);
    if (pps.line < 0) return;
    const auto at_pps = [&] {
        top(ui, t); calm();
        for (int k = 0; k < pps.line; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
    };
    at_pps();
    const auto rw = r.sweep_view().apdo(pps.algo, pps.pos);
    CHECK_EQ(rw.has_value() && rw->sum.points > 0, true);
    if (!rw) return;
    char a[12] = {}, b[12] = {};
    StrBuf<40> want;
    want.put("PPS: peak ").put(std::string_view(a, ctsweep::format_mv(rw->sum.peak_mv, a))).put("V@");
    want.put(std::string_view(b, ctsweep::format_mv(rw->sum.peak_at_mv, b))).put_char('V');
    CHECK_EQ(view_says(ui, want.view()), true);   // the peak it recorded
    for (const auto& row : r.sweep_view().rows)             // no point recorded at all
        const_cast<ctsweep::RowView&>(row).n = 0, const_cast<ctsweep::RowView&>(row).sum.points = 0;
    ui.tick(t += 100); calm();
    top(ui, t); calm();
    press(ui, Button::Ok); ui.tick(t += 100); calm();
    press(ui, Button::Back); ui.tick(t += 100); calm();
    at_pps();
    CHECK_EQ(view_says(ui, "PPS: sweep failed"), true);
}
// Every Steps row reads as a whole step label (step_label::label), never cut with dots: the ink of each visible row's text
// cell is compared with the ink of every label of the run, and some label must match. The cut would end in "...". The same scan
// found two repaint bugs in a list that scrolls under a fixed highlight or over a longer verdict (stale cells of the row before).
static void test_steps_rows_show_whole_labels() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto steps = ui.charger_test_results().steps();
    unsigned rows_checked = 0, ramps = 0, failed_rows = 0;
    int list_y = kit::kBodyY;   // the first list row's top: under the picked row's grid header
    // The Steps rows are kit rows: a Verdict marker, the label at x28 (16 px, up to the voltage tail), the measured voltage right
    // aligned at x214. A row is a whole label when some step of the run (label + its own voltage tail) reproduces its ink.
    const auto check_window = [&] {
        const int offset = ui.list_state().offset, shown = ui.list_state().visible;
        const int open = ui.charger_test_cursor() - offset;
        const int w = kit::kTailX - 40 - 8 - kit::kTextX;   // the room of a 5-character voltage tail ("20.1V")
        for (int slot = 0; slot < shown && offset + slot < ui.list_state().count; ++slot) {
            const int y = wizard_text_y(slot, open, list_y);
            if (ui.context().toast.active(t) &&
                y + kSmall.height > kFooterY - kToastH - kToastMarginBottom) continue;
            const uint16_t bg = PANEL[y * LCD_WIDTH + kit::kMarkX + kit::kMarkSize + 2];   // clear of the marker and the text
            std::vector<uint8_t> seen;
            for (int yy = y; yy < y + kSmall.height; ++yy)
                for (int x = kit::kTextX; x < kit::kTextX + w; ++x) seen.push_back(PANEL[yy * LCD_WIDTH + x] != bg);
            if (std::count(seen.begin(), seen.end(), 1) == 0) continue;   // a blank row below the last step
            std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> keep;
            std::copy_n(g_panel, keep.size(), keep.begin());
            bool matched = false;
            for (size_t i = 0; i < steps.size() && !matched; ++i) {
                const auto text = step_label::label(steps[i], i ? &steps[i - 1] : nullptr);
                char fitted[kFitBuf];
                const auto shown = fit_text(kSmall, text.view(), w, fitted);
                gfx_fill(kContent0, kit::kTextX, y, w, kSmall.height, bg);
                gfx_text(kContent0, kSmall, kit::kTextX, y, shown, kText, bg);
                std::vector<uint8_t> want;
                for (int yy = y; yy < y + kSmall.height; ++yy)
                    for (int x = kit::kTextX; x < kit::kTextX + w; ++x) want.push_back(PANEL[yy * LCD_WIDTH + x] != bg);
                matched = want == seen;
                if (matched && text.view().find("\xE2\x86\x92") != std::string_view::npos) ++ramps;
                if (matched && steps[i].verdict == ct::Verdict::Fail) ++failed_rows;
            }
            std::copy(keep.begin(), keep.end(), g_panel);
            CHECK_EQ(matched, true);
            ++rows_checked;
        }
    };
    for (unsigned k = 0; k < 10; ++k) {
        ui.tick(t); t += 200;
        top(ui, t); move(ui, k, t); press(ui, Button::Ok, t);
        if (ui.charger_test_stage() != 7) { CHECK_EQ(ui.charger_test_stage(), 6); continue; }
        ui.tick(t); t += 200; settle(ui, t);
        list_y = family_list_top();   // the cursor is on the first row
        for (int window = 0; window < 4; ++window) {   // the list scrolls a window at a time
            check_window();
            move(ui, 6, t);
            for (int i = 0; i < 4; ++i) { ui.tick(t); t += 200; }
            settle(ui, t);
        }
        press(ui, Button::Back, t);
        CHECK_EQ(ui.charger_test_stage(), 6);
    }
    CHECK_EQ(rows_checked > 20, true);   // the scan really met step rows
    CHECK_EQ(ramps > 0, true);           // and the long "from→to" kind
    CHECK_EQ(failed_rows > 0, true);     // and failed rows (one-cell verdict) among them
    // The content surface is at least the 224 x 224 the rows are laid out on at every rotation: the 19 label cells never get less.
    for (uint8_t r = 0; r < 4; ++r) {
        const Rect box = compute_layout(orientation_for(r).edge, kSlotCenters, kSlotLens, kPanelThickness).content;
        CHECK_EQ(box.w >= 224, true);
        CHECK_EQ(box.h >= 224, true);
    }
}
// The step-label gallery at the four rotations: a Running screen and the Steps list of a source that offers fixed 5/9/15/20 V,
// two PPS ranges, SPR AVS and EPR fixed 28 V. K2_DUMP=dir host-build/test_ui_charger_test writes dir/k2_labels_rot<r>_<scene>.panel,
// then `tools/panel2png.py dir out <names>` makes the PNGs (docs/ui_review_2026-11-06/labels/). Without K2_DUMP it only checks that
// the scenes are reachable.
static void dump_panel(const char* name) {
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[128];
    std::snprintf(path, sizeof path, "%s/k2_%s.panel", dir, name);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, size_t(LCD_WIDTH) * LCD_HEIGHT, f); std::fclose(f); }
}
static void test_step_label_gallery() {
    constexpr uint32_t kOffer[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xC1A42164, 0xE004B000, 0x0008C1F4};
    for (uint8_t rot = 0; rot < 4; ++rot) {
        fixture();
        g_stub.caps.num_pdos = 8; g_stub.caps.epr = true;
        for (unsigned i = 0; i < 8; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(kOffer[i]);
        Ui ui;
        open(ui); ui.set_rotation(rot, 1100);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok);   // "PD only": the PD fixed / PPS / Xiaomi rows; a preset pick goes on to the cable
        CHECK_EQ(ui.charger_test_stage(), 2);
        press(ui, Button::Ok);                     // the cable pick keeps "Off" and goes on to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        char name[48];
        const auto shot = [&](const char* scene, uint32_t& at, bool whole = true) {
            ui.tick(at);
            for (int i = 0; whole && i < 60 && ui.painting(); ++i) ui.tick(at += 20);   // a settled frame, as the screen shows it
            std::snprintf(name, sizeof name, "labels_rot%u_%s", unsigned(rot), scene);
            dump_panel(name);
        };
        {   // Review, whole (a stage change paints over several ticks): the README's ct-review.png at rotation 3
            uint32_t at = 1100;
            for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(at += 20);
            shot("review", at);
            press(ui, Button::Minus, at += 20);   // the cursor on Level: the focus look (README / review renders)
            for (int i = 0; i < 20 && ui.painting(); ++i) ui.tick(at += 20);
            shot("review_level", at);
            press(ui, Button::Plus, at += 20);    // back on START
            for (int i = 0; i < 20 && ui.painting(); ++i) ui.tick(at += 20);
        }
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        // Running: stop with some rows done above the one being negotiated.
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) {
            ui.tick(t);
            const auto& steps = ui.charger_test_results().steps();
            if (ui.charger_test_stage() == 5 && steps.size() > 12 &&
                std::count_if(steps.begin(), steps.end(), [](const ct::Step& s) { return s.verdict > ct::Verdict::Running; }) >= 8) break;
        }
        CHECK_EQ(ui.charger_test_stage(), 5);
        shot("running", t, false);   // mid-run: the painter is never waited for
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        ui.tick(t); t += 200; ui.tick(t); t += 200;
        shot("results", t);
        for (unsigned k = 0; k < 10; ++k) {   // the unit lines: show the first PD line's Steps, then a scrolled window
            top(ui, t); move(ui, k, t); press(ui, Button::Ok, t);
            if (ui.charger_test_stage() != 7) { CHECK_EQ(ui.charger_test_stage(), 6); continue; }
            ui.tick(t); t += 200;
            const auto& steps = ui.charger_test_results().steps();
            if (steps.size() < 12 || steps[0].group != ct::Group::Pd) { press(ui, Button::Back, t); continue; }
            shot("steps", t);
            move(ui, 6, t); ui.tick(t); t += 200; shot("steps_scrolled", t);
            move(ui, 6, t); ui.tick(t); t += 200; shot("steps_scrolled2", t);
            press(ui, Button::Ok, t); ui.tick(t); t += 200; shot("detail", t);
            break;
        }
    }
    {   // The same offer at Light: the Results grid with the Advertised / Detected cells (Verdict::Noted), rotation 3
        fixture();
        g_stub.caps.num_pdos = 8; g_stub.caps.epr = true;
        for (unsigned i = 0; i < 8; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(kOffer[i]);
        Ui ui;
        open(ui); ui.set_rotation(3, 1100);
        CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Light)), true);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok); press(ui, Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 4);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(t += 20);
        dump_panel("labels_rot3_results_light");
        // A grid row's footer counts the listing apart from the results; a listed range reads "advertised" in the Ok
        // tone (the source has it, nothing was asked), never a shortfall in amber.
        ui.set_rotation(0, t); top(ui, t); settle(ui, t);   // the text checks read rotation 0's panel
        CHECK_EQ(footer_shows("PD: 1 pass, 6 listed", kOk), true);   // one PD block: the fixed chips and both PPS ranges count together
        bool avs = false;
        for (int n = 0; n < 12 && !avs; ++n) {
            avs = view_says(ui, "AVS: advertised");
            if (!avs) { press(ui, Button::Plus); ui.tick(t += 100); settle(ui, t); }
        }
        CHECK_EQ(avs, true);
        ui.set_rotation(3, t); top(ui, t); settle(ui, t); press(ui, Button::Ok, t);   // the PD line's Steps, then its third row (top-down order: 20 V, 15 V, 9 V: Advertised)
        if (ui.charger_test_stage() == 7) {
            for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(t += 20);
            dump_panel("labels_rot3_steps_light");
            ui.set_rotation(0, t); top(ui, t); settle(ui, t);
            const int list_y = family_list_top();   // under the PD row's own grid header
            move(ui, 2, t); settle(ui, t);
            check_row_note(2, "LISTED PDO #2", list_y);   // the focused note: the band word, no skip cause
            ui.set_rotation(3, t); settle(ui, t); press(ui, Button::Ok, t);
            for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(t += 20);
            dump_panel("labels_rot3_detail_light");
        }
        for (const auto& s : ui.charger_test_results().steps())   // a Light run asks for nothing above 5 V
            CHECK_EQ(s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail ? s.mv <= 5000u : true, true);
    }
}
// The Results grid's PD block for three sources (a bank-like one, an EPR one with AVS, a PD 2.0 fixed-only one) at rotations 0 and 3:
// the first window, the last window and the block's Steps page. K2_DUMP=dir writes dir/k2_pdb_<source>_rot<r>_<scene>.panel
// (tools/panel2png.py makes the PNGs); without K2_DUMP it only walks the scenes.
static void test_pd_block_scenes() {
    // ver: the PD version the Results headline leads with (a PPS offer is 3.0, the EPR table 3.1, an SPR AVS APDO 3.2, a 2.0 header 2.0)
    struct Source { const char* name; uint8_t rev; bool epr; unsigned ceil_mv; std::initializer_list<uint32_t> pdos; const char* ver; };
    const Source sources[] = {
        {"bank", 2, false, 0, {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC0DC3264, 0xC1903228}, "PD3.0"},
        {"epr", 2, true, 0, {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xC1A42164, 0xE004B000, 0x0008C1F4, 0xD230968C}, "PD3.2"},
        {"eprok", 2, true, 36000, {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xC1A42164, 0xE004B000, 0x0008C1F4, 0xD230968C}, "PD3.2"},   // a 5 A EPR cable and a monitor that reaches 28 V: the EPR level and the EPR AVS range run
        {"epr31", 2, true, 0, {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0x0008C1F4, 0xD230968C}, "PD3.1"},   // EPR and PPS, no SPR AVS: 3.1
        {"pd2", 1, false, 0, {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4}, "PD2.0"},
    };
    for (const Source& s : sources) for (uint8_t rot : {uint8_t(0), uint8_t(3)}) {
        fixture();
        g_stub.caps.spec_rev = s.rev; g_stub.caps.epr = s.epr;
        if (s.ceil_mv) { g_stub.vbus_ceiling_mv = s.ceil_mv; g_stub.cable_limits = {.max_mv = 50000, .max_ma = 5000, .observed = true}; }
        g_stub.caps.num_pdos = uint8_t(s.pdos.size());
        unsigned i = 0;
        for (uint32_t w : s.pdos) g_stub.caps.pdos[i++] = pd::decode_pdo(w);
        Ui ui;
        open(ui); ui.set_rotation(rot, 1100);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok); press(ui, Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 4);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        for (int k = 0; k < 60 && ui.painting(); ++k) ui.tick(t += 20);
        char name[64];
        const auto shot = [&](const char* scene) {
            for (int k = 0; k < 60 && ui.painting(); ++k) ui.tick(t += 20);
            std::snprintf(name, sizeof name, "pdb_%s_rot%u_%s", s.name, unsigned(rot), scene);
            dump_panel(name);
        };
        top(ui, t); shot("top");
        if (rot == 0) {   // the headline leads with the version, then as many modes as fit (the trailing ones go first)
            std::array<result_layout::SummaryLine, 3> lines;
            const size_t n = result_layout::summarize(ui.charger_test_results().steps(), lines);
            std::string want = s.ver;
            for (size_t i = 0; i < n && want.size() + 2 + std::strlen(lines[i].text) <= 26; ++i) want += std::string("  ") + lines[i].text;
            CHECK_EQ(want.size() > std::strlen(s.ver), true);
            check_text_at(kSmall, k2::runlay::kResBestY, want, kText, kBg, 6);
        }
        move(ui, 40, t); shot("end");
        top(ui, t); settle(ui, t);
        for (unsigned k = 0; k < 10; ++k) {   // the first PD row's Steps page
            top(ui, t); move(ui, k, t); press(ui, Button::Ok, t);
            if (ui.charger_test_stage() != 7) { CHECK_EQ(ui.charger_test_stage(), 6); continue; }
            const auto& steps = ui.charger_test_results().steps();
            if (steps.empty() || steps[0].group != ct::Group::Pd) { press(ui, Button::Back, t); continue; }
            shot("steps");
            unsigned at = 0;   // the EPR entry: where the reason for a skipped or refused level reads
            for (size_t j = 0; j < steps.size() && steps[j].group == ct::Group::Pd; ++j)
                if (steps[j].algorithm == ct::Algorithm::Epr) { at = unsigned(j); break; }
            if (at) {
                top(ui, t); move(ui, at, t); shot("eprstep");
                press(ui, Button::Ok, t);
                CHECK_EQ(ui.charger_test_stage(), 8);
                shot("eprdetail");
            }
            break;
        }
        for (int k = 0; k < 3 && ui.charger_test_stage() > 6; ++k) press(ui, Button::Back, t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        top(ui, t); settle(ui, t);   // the cursor on a label-less line of the block
        for (unsigned k = 1; k < 4; ++k) { top(ui, t); move(ui, k, t); shot(k == 1 ? "cont1" : k == 2 ? "cont2" : "cont3"); }
    }
}
// An answer under what the offer proves (Revision 3.1 beside an SPR AVS APDO, which is 3.2): the headline leads with the offer's version
// marked "PD3.2*" and the first PD step's Detail says both in its Revision row; an equal answer shows the answer, unmarked, and
// the Detail keeps its Header row. K2_DUMP writes dir/k2_pdv_<case>_rot<r>_{results,detail}.panel.
static void test_pd_version_answer_against_the_offer() {
    struct Case { const char* name; uint8_t major, minor; const char* head; const char* row_key; const char* row; };
    const Case cases[] = {
        {"under", 3, 1, "PD3.2*", "Revision", "3.1 < AVS 3.2"},
        {"equal", 3, 2, "PD3.2", "Header", "PD3.x  RMDO 3.2"},
        {"below31", 3, 0, "PD3.2*", "Revision", "3.0 < AVS 3.2"},
    };
    for (const Case& c : cases) for (uint8_t rot : {uint8_t(0), uint8_t(3)}) {
        fixture();
        g_stub.caps.spec_rev = 2;
        constexpr uint32_t kOffer[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xE004B000};
        g_stub.caps.num_pdos = 6;
        for (unsigned i = 0; i < 6; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(kOffer[i]);
        g_stub.source_revision = {true, c.major, c.minor, 1, 0};
        Ui ui;
        open(ui); ui.set_rotation(rot, 1100);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok); press(ui, Button::Ok);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        for (int k = 0; k < 60 && ui.painting(); ++k) ui.tick(t += 20);
        top(ui, t); settle(ui, t);
        char name[64];
        std::snprintf(name, sizeof name, "pdv_%s_rot%u_results", c.name, unsigned(rot)); dump_panel(name);
        if (rot == 0) {
            std::array<result_layout::SummaryLine, 3> lines;
            const size_t n = result_layout::summarize(ui.charger_test_results().steps(), lines);
            std::string want = c.head;
            for (size_t i = 0; i < n && want.size() + 2 + std::strlen(lines[i].text) <= 26; ++i) want += std::string("  ") + lines[i].text;
            check_text_at(kSmall, k2::runlay::kResBestY, want, kText, kBg, 6);
        }
        top(ui, t); press(ui, Button::Ok, t); ui.tick(t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 7);
        press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 8);
        std::snprintf(name, sizeof name, "pdv_%s_rot%u_detail", c.name, unsigned(rot)); dump_panel(name);
        if (rot == 0) {
            constexpr kit::KvLayout kDetail{44, kit::kRowH, 6, nullptr, nullptr, 76, 16, Align::Left};
            const int y = kit::kv_row_y(kDetail, 4) + kit::row_text_y(kit::kRowH, kSmall.height);
            check_text_at(kSmall, y, c.row, c.row[0] == '3' && c.row_key[0] == 'R' && c.row_key[1] == 'e' && c.row_key[2] == 'v' ? kCaution : kText, kBg, 76);
        }
    }
}
// The README's charger-test pictures (docs/img/ui/ct-*.png), rotation 3 (the button strip at the bottom): an EPR-capable charger
// (fixed 5/9/15/20 V, two PPS ranges, SPR AVS, EPR 28 V) behind a converter that follows every request up to the monitor's 36 V
// ceiling, so a PD-only run is a clean pass. Writes $K2_DUMP/k2_readme_ct_<scene>.panel; without K2_DUMP it only walks the scenes.
// (The caution frames come from test_a_voltage_fold_reads_as_a_caution.)
static void test_readme_frames() {
    constexpr uint32_t kOffer[] = {0x0001912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC0DC213C, 0xC1A42164, 0xE004B000, 0x0008C1F4};
    const auto offer = [&] {
        fixture();
        g_stub.vbus_ceiling_mv = 36000;
        g_stub.caps.num_pdos = 8; g_stub.caps.epr = true;
        for (unsigned i = 0; i < 8; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(kOffer[i]);
    };
    const auto shot = [](Ui& ui, const char* scene, uint32_t& t) {
        for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(t += 20);
        CHECK_EQ(ui.painting(), false);
        char name[48]; std::snprintf(name, sizeof name, "readme_ct_%s", scene);
        dump_screen(name);
    };
    {   // Medium (the default level): Review, armed, Running, Results, the Steps of the first PD line, one step's Detail
        offer();
        Ui ui; open(ui); ui.set_rotation(3, 1100);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok);
        press(ui, Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 4);
        uint32_t t = 1100;
        shot(ui, "review", t);
        start_press(ui, 1300);                      // the first OK arms: CONFIRM, the 5 s bar
        shot(ui, "confirm", t);
        press(ui, Button::Ok, 1400);
        t = 1500;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) {
            ui.tick(t);
            const auto& steps = ui.charger_test_results().steps();
            if (ui.charger_test_stage() == 5 && steps.size() > 12 &&
                std::count_if(steps.begin(), steps.end(), [](const ct::Step& s) { return s.verdict > ct::Verdict::Running; }) >= 8) break;
        }
        CHECK_EQ(ui.charger_test_stage(), 5);
        shot(ui, "running", t);
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        ui.tick(t); t += 200; ui.tick(t); t += 200;
        shot(ui, "results", t);
        for (const auto& s : ui.charger_test_results().steps())   // a clean run: no failure anywhere
            CHECK_EQ(s.verdict == ct::Verdict::Fail, false);
        for (unsigned k = 0; k < 10; ++k) {
            top(ui, t); move(ui, k, t); press(ui, Button::Ok, t);
            if (ui.charger_test_stage() != 7) { CHECK_EQ(ui.charger_test_stage(), 6); continue; }
            ui.tick(t); t += 200;
            const auto& steps = ui.charger_test_results().steps();
            if (steps.size() < 12 || steps[0].group != ct::Group::Pd) { press(ui, Button::Back, t); continue; }
            shot(ui, "steps", t);
            press(ui, Button::Plus, t); press(ui, Button::Ok, t); ui.tick(t); t += 200;
            CHECK_EQ(ui.charger_test_stage(), 8);
            shot(ui, "detail", t);
            break;
        }
    }
    {   // Light: the Results grid with the Advertised cells (nothing above 5 V was asked for)
        offer();
        Ui ui; open(ui); ui.set_rotation(3, 1100);
        CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Light)), true);
        top(ui); move(ui, kPdRow); press(ui, Button::Ok); press(ui, Button::Ok);
        CHECK_EQ(ui.charger_test_stage(), 4);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        shot(ui, "light", t);
    }
}
// A UFCS unit planned from the charger's Output_Capabilities: the Results grid and the Detail page say the current each mode
// offers. fc_caps is what the UFCS session read (the stub's fctrig::caps()); the run samples the levels of each mode.
static void ufcs_caps_fixture(std::initializer_list<ufcs::Cap> modes) {
    fixture();
    g_stub.fc_caps = {};
    for (const auto& m : modes) g_stub.fc_caps.ufcs[g_stub.fc_caps.ufcs_n++] = m;
}
static void ufcs_run(Ui& ui, uint32_t& t, const char* shot = nullptr) {
    constexpr uint8_t kUfcsOnly[] = {uint8_t(ct::Unit::Ufcs)};
    open(ui); CHECK_EQ(ui.charger_test_set_units(kUfcsOnly), true); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    bool running_shot = !shot;
    for (t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) {
        ui.tick(t);
        if (!running_shot && ui.charger_test_stage() == 5) {
            const auto& steps = ui.charger_test_results().steps();
            if (std::count_if(steps.begin(), steps.end(), [](const ct::Step& s) { return s.verdict > ct::Verdict::Running; }) >= 3) {
                char name[64]; std::snprintf(name, sizeof name, "ufcs_%s_running", shot); dump_panel(name); running_shot = true;
            }
        }
    }
    CHECK_EQ(ui.charger_test_stage(), 6);
    ui.tick(t); t += 200; ui.tick(t); t += 200;
    settle(ui, t);
}
static ufcs::Cap ufcs_mode_cap(uint8_t pos, int lo_mv, int hi_mv, int lo_ma, int hi_ma) {
    ufcs::Cap c; c.pos = pos; c.min_mv = lo_mv; c.max_mv = hi_mv; c.min_ma = lo_ma; c.max_ma = hi_ma; return c;
}
// The Detail page of the first sampled UFCS step: the "Mode" row (row 4 of the table, value column x92) reads the whole mode.
static void check_ufcs_detail(Ui& ui, uint32_t& t, std::string_view mode_text, const char* shot) {
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t); t += 200;
    CHECK_EQ(ui.charger_test_stage(), 7);
    press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
    CHECK_EQ(ui.charger_test_stage(), 8);
    constexpr kit::KvLayout kDetail{44, kit::kRowH, 6, nullptr, nullptr, 76, 16, Align::Left};
    const int y = kit::kv_row_y(kDetail, 4) + kit::row_text_y(kit::kRowH, kSmall.height);
    check_text_at(kSmall, y, mode_text, kText, kBg, 76);
    char name[64]; std::snprintf(name, sizeof name, "ufcs_%s_detail", shot); dump_panel(name);
}
static void test_ufcs_results_and_detail_show_the_mode_current() {
    { // CUKTECH-like: 5-11 V 3 A and 5-20 V 3 A (one bracket would cover both modes' equal current)
        ufcs_caps_fixture({ufcs_mode_cap(1, 5000, 11000, 1000, 3000), ufcs_mode_cap(2, 5000, 20000, 1000, 3000)});
        Ui ui; uint32_t t; ufcs_run(ui, t, "cuktech");
        const auto& steps = ui.charger_test_results().steps();
        unsigned tagged = 0;
        for (const auto& s : steps) if (s.group == ct::Group::Ufcs && s.mv) tagged += step_label::ufcs_amps_ma(s) == 3000;
        CHECK_EQ(tagged > 3, true);
        result_layout::Layout l; result_layout::build(l, steps);
        unsigned with_watts = 0;   // OEM watts (oem_watts::chip): each level's volts x its mode's 3 A, "9V 27W"
        for (const auto& tk : l.tokens) {
            const std::string_view x(tk.text);
            with_watts += x.size() > 1 && x.back() == 'W' && x.find("V ") != std::string_view::npos;
        }
        CHECK_EQ(with_watts >= 3, true);
        dump_panel("ufcs_cuktech_results");
        check_ufcs_detail(ui, t, "5-20V 1-3A", "cuktech");   // the 5 V baseline is judged in the last mode that reaches 5 V
    }
    { // FH22-like: one 5.5-11 V mode at 4 A (44 W)
        ufcs_caps_fixture({ufcs_mode_cap(1, 5500, 11000, 1000, 4000)});
        Ui ui; uint32_t t; ufcs_run(ui, t, "fh22");
        result_layout::Layout l; result_layout::build(l, ui.charger_test_results().steps());
        bool found = false;
        for (const auto& tk : l.tokens) found |= std::string_view(tk.text).find("11V 44W") != std::string_view::npos;
        CHECK_EQ(found, true);
        dump_panel("ufcs_fh22_results");
        check_ufcs_detail(ui, t, "5.5-11V 1-4A", "fh22");
    }
}
// The best line between the counters and the grid (y 51, 16 px, 26 cells): the best passing mode of each protocol family as words
// with volts and amps, from result_layout::summarize(); moving the cursor never changes it.
static void test_results_best_line_names_the_best_modes() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    std::array<result_layout::SummaryLine, 3> lines;
    const size_t n = result_layout::summarize(ui.charger_test_results().steps(), lines);
    CHECK_EQ(n >= 1, true);
    CHECK_EQ(std::string_view(lines[0].text).substr(0, 2) == "PD", true);
    CHECK_EQ(std::strlen(lines[0].text) <= 26, true);
    // The charger's PD version leads it (this source lists a PPS range under a 3.x header: PD3.0), two spaces before the modes.
    std::string want = "PD3.0";
    for (size_t i = 0; i < n && want.size() + 2 + std::strlen(lines[i].text) <= 26; ++i) want += std::string("  ") + lines[i].text;
    CHECK_EQ(want.size() > 7, true);
    check_text_at(kSmall, k2::runlay::kResBestY, want, kText, kBg, 6);
    move(ui, 2); ui.tick(t + 100);
    check_text_at(kSmall, k2::runlay::kResBestY, want, kText, kBg, 6);
}
static void test_start_presents_rd() {
    { // charger alone, no VBUS: the first OK touches nothing; the confirming OK presents Rd, VBUS comes up, the run starts
      // by itself (no third press), Rd drops when it leaves
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        Ui ui; open(ui); review(ui);
        CHECK_EQ(g_stub.rd_run, false);
        start_press(ui, 1300);
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0);   // the window only
        confirm_press_nowait(ui, 1400);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);   // the Rd and the wait are the last stage before the tests
        CHECK_EQ(g_stub.rd_run, true); CHECK_EQ(ui.charger_test_busy(), false);
        finish_wait(ui, 1400);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.rd_run, true);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 1500);
        CHECK_EQ(g_stub.rd_run, false);
    }
    { // START reads the cable's e-marker (VCONN) before Rd goes on, when nothing supplies VBUS: on the confirming OK
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        static bool rd_at_scan; rd_at_scan = true; g_stub.on_scan = [] { rd_at_scan = g_stub.rd_run; };
        Ui ui; open(ui); review(ui);
        int scans0 = g_stub.scans;
        start_press(ui, 1300);
        CHECK_EQ(g_stub.scans - scans0, 0);              // the first OK reads nothing
        confirm_press(ui, 1400);
        g_stub.on_scan = nullptr;
        CHECK_EQ(g_stub.scans - scans0, 1);
        CHECK_EQ(int(g_stub.last_scan_mode), int(pd::EmarkerMode::Vconn));
        CHECK_EQ(g_stub.scan_confirmed, false);
        CHECK_EQ(rd_at_scan, false);
        CHECK_EQ(g_stub.rd_run, true);
    }
    { // a live charger (VBUS up): no scan, nobody to power the cable from; no Waiting at all, the run starts on the second OK
        fixture(); g_stub.pm.vbus_mv = 5000;
        Ui ui; open(ui); review(ui);
        int scans0 = g_stub.scans;
        start_press(ui, 1300);
        confirm_press_nowait(ui, 1400);
        CHECK_EQ(g_stub.scans - scans0, 0);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage); CHECK_EQ(ui.charger_test_busy(), true);
    }
    { // the sink holds PDO 1 from START's own request: that is not a foreign tool, the run still starts
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1400);
        g_stub.sink.held = true;   // the Request goes out while the wait runs, after the window was spent
        finish_wait(ui, 1400);
        CHECK_EQ(g_stub.rd_run, true);   // not refused as "tool busy"
        CHECK_EQ(ui.charger_test_busy(), true);
    }
    { // one OK, no second one: the confirm window expires and nothing was ever touched (no Rd to drop)
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300);
        CHECK_EQ(g_stub.rd_run, false);
        ui.tick(3000);
        ui.tick(6500);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0);
        CHECK_EQ(ui.charger_test_busy(), false);
        press(ui, Button::Ok, 6600);                     // the window is gone: this OK opens a new one, it does not start
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.rd_run, false);
    }
    { // Rd brings nothing: up to 10 s of waiting (charger attached), the preflight refuses, Rd is off again
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_run(ui, 1300);
        CHECK_EQ(g_stub.rd_run, false);
        CHECK_EQ(g_stub.rd_run_changes, 4);   // on, off, on again (one retry), off after the refusal
        CHECK_EQ(g_stub.rd_pauses, 1);        // the retry's Rd-off keeps the scanned cable (rd_for_run(false) would drop it)
        CHECK_EQ(g_wait_end - 1400 >= 20990 && g_wait_end - 1400 <= 21200, true);   // two 10 s waits and a 1 s Rd-off pause
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(ui.charger_test_stage(), 4);   // Review again, the refusal on its footer
        CHECK_EQ(g_stub.delays_ms, 0u);         // none of it blocked
    }
    { // VBUS already up: plain boot stays untouched
        fixture();
        Ui ui; open(ui); review(ui);
        start_run(ui, 1300);
        CHECK_EQ(g_stub.rd_run_changes, 0);
        CHECK_EQ(g_stub.delays_ms, 0u);
        CHECK_EQ(ui.charger_test_busy(), true);
    }
}
static void two_pdos() {   // 5 V and 9 V fixed: the 9 V step is a request that leaves the default rail
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300);
}
static void run_to_end(Ui& ui) {
    review(ui);
    start_press(ui, 1300); ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
    // Every unit detaches and attaches (the stub's source keeps its output up, so each detach waits out its dark bound).
    for (uint32_t t = 1500; t <= 600000; t += 100) ui.tick(t);
}
static void test_transition_capture_hooks() {
    { // only 5 V in the offer: no step leaves the default rail, nothing is armed
        fixture(); Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_arms, 0); CHECK_EQ(g_stub.tc_keeps, 0);
    }
    { // a refused 9 V request: armed before the request, frozen and kept for that run and step
        // (BadPos, not Busy: Busy is retried since cdcc620 and the step then ends Timeout)
        fixture(); two_pdos(); g_stub.request_err = pd::Err::BadPos;
        Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_arms >= 1, true);
        CHECK_EQ(g_stub.tc_arm_pos, 2); CHECK_EQ(g_stub.tc_arm_mv, 9000);
        CHECK_EQ(g_stub.tc_triggers, 1); CHECK_EQ(g_stub.tc_keeps, 1);
        CHECK_EQ(int(g_stub.tc_reason), int(transcap::Reason::NoPsRdy));
        const auto& r = ui.charger_test_results();
        CHECK_EQ(g_stub.tc_keep_run, r.run_id());
        CHECK_EQ(g_stub.tc_keep_step >= 0 && size_t(g_stub.tc_keep_step) < r.steps().size(), true);
        CHECK_EQ(int(r.steps()[g_stub.tc_keep_step].verdict), int(ct::Verdict::Fail));
        CHECK_EQ(int(g_stub.tc_state), int(transcap::State::Post));   // the finished run leaves it to the main loop
    }
    { // the capture froze before the step failed (the cut's detach): the failed step still keeps it
        fixture(); two_pdos(); g_stub.request_err = pd::Err::Busy; g_stub.tc_freeze_on_arm = true;
        Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_triggers, 0); CHECK_EQ(g_stub.tc_keeps, 1);
        const auto& r = ui.charger_test_results();
        CHECK_EQ(g_stub.tc_keep_run, r.run_id());
        CHECK_EQ(int(r.steps()[g_stub.tc_keep_step].verdict), int(ct::Verdict::Fail));
    }
    { // a passing step whose capture froze on its own (a detach of the cut, never kept) gives the recorder back: it would hold the
      // overlay, and the PD spec section's own capture, for the rest of the run (hardware 2026-11-17)
        fixture(); two_pdos(); g_stub.tc_freeze_on_arm = true; g_stub.vbus_ceiling_mv = 20000;   // the converter follows the Request
        Ui ui; open(ui);
        CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
        review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        for (uint32_t t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        CHECK_EQ(g_stub.tc_arms >= 1, true); CHECK_EQ(g_stub.tc_keeps, 0);
        CHECK_EQ(int(g_stub.tc_state), int(transcap::State::Off));
    }
    { // a retried request does NOT re-arm over a capture that froze on its own: it is the row's evidence (the cut's trace) until the row
      // ends, and the failed step then keeps it (the existing case above); a re-arm here would record an empty window
        fixture(); two_pdos(); g_stub.request_err = pd::Err::Busy; g_stub.tc_freeze_on_arm = true;
        Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_arms, 1);
        CHECK_EQ(g_stub.tc_keeps, 1);
    }
    { // a kept capture waits for its write: nothing drops it (the stub marks it kept until arm / disarm)
        fixture(); two_pdos(); g_stub.request_err = pd::Err::BadPos;
        Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_keeps, 1);
        CHECK_EQ(g_stub.tc_keep_pending, true);
        CHECK_EQ(int(g_stub.tc_state), int(transcap::State::Post));   // a run end leaves a kept capture to the main loop
    }
    { // another tool owns the overlay: the run goes on without a capture
        fixture(); two_pdos(); g_stub.request_err = pd::Err::Busy; g_stub.tc_arm_result = transcap::ArmResult::Busy;
        Ui ui; open(ui); run_to_end(ui);
        CHECK_EQ(g_stub.tc_arms, 0); CHECK_EQ(g_stub.tc_keeps, 0);
    }
}
// The live debug stream's run events (k2/chargertest/run_events.hpp): a run says when it starts, which row begins and ends with what verdict,
// each unit attach and detach, and how it ended; every row that ended has exactly one RowEnd.
static void test_run_events_follow_the_run() {
    using runev::Code;
    fixture(); two_pdos(); g_stub.request_err = pd::Err::BadPos;
    Ui ui; open(ui);
    review(ui);
    std::vector<runev::Event> ev;
    uint32_t cursor = runev::live().pushed(), overflow = 0;
    auto drain = [&] {
        const auto& q = runev::live();
        if (q.oldest() > cursor) { overflow += q.oldest() - cursor; cursor = q.oldest(); }
        for (; cursor < q.pushed(); cursor++) ev.push_back(q.at(cursor));
    };
    start_press(ui, 1300); ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
    drain();
    for (uint32_t t = 1500; t <= 600000; t += 100) { ui.tick(t); drain(); }
    CHECK_EQ(overflow, 0u);   // draining each pass (as the stream does) never loses an event, even across the skip burst
    const auto& r = ui.charger_test_results();
    CHECK_EQ(int(r.state()) == int(ct::State::Done) || int(r.state()) == int(ct::State::Stopped), true);
    auto count = [&](Code c) { unsigned n = 0; for (const auto& e : ev) n += e.code == c; return n; };
    CHECK_EQ(count(Code::Start), 1u);
    CHECK_EQ(int(ev.front().code), int(Code::Start));
    CHECK_EQ(unsigned(ev.front().b), unsigned(r.steps().size()));
    CHECK_EQ(count(Code::End), 1u);
    CHECK_EQ(int(ev.back().code) == int(Code::End) || int(ev.back().code) == int(Code::Stage), true);
    unsigned ended = 0, failed = 0;
    for (const auto& s : r.steps()) { ended += s.verdict != ct::Verdict::Pending && s.verdict != ct::Verdict::Running; failed += s.verdict == ct::Verdict::Fail; }
    // every row that ended is told once: a live row by its RowEnd, the rows a failure skipped by a RowSkips range
    unsigned skipped = 0, skip_rows = 0;
    for (const auto& s : r.steps()) skipped += s.verdict == ct::Verdict::Skip;
    for (const auto& e : ev) {
        if (e.code != Code::RowSkips) continue;
        skip_rows += e.d;
        CHECK_EQ(unsigned(e.b - e.a + 1), unsigned(e.d));   // contiguous rows
        for (unsigned row = e.a; row <= e.b; row++) {
            CHECK_EQ(row < r.steps().size(), true);
            CHECK_EQ(int(r.steps()[row].verdict), int(ct::Verdict::Skip));
            CHECK_EQ(unsigned(r.steps()[row].reason), unsigned(e.c));
        }
    }
    CHECK_EQ(count(Code::RowEnd) + skip_rows, ended);
    CHECK_EQ(skip_rows, skipped);
    CHECK_EQ(skipped >= 1, true);
    CHECK_EQ(failed >= 1, true);
    // the skip rows are a few events, not one each: the burst fits the ring (the loss the live run of 2026-11-07 showed)
    CHECK_EQ(count(Code::RowSkips) < skipped, true);
    // a row's RowEnd carries the verdict the Runner kept for it
    for (const auto& e : ev) {
        if (e.code != Code::RowEnd) continue;
        CHECK_EQ(e.a < r.steps().size(), true);
        CHECK_EQ(unsigned(e.b), unsigned(r.steps()[e.a].verdict));
        CHECK_EQ(int(r.steps()[e.a].verdict) != int(ct::Verdict::Skip), true);
    }
    CHECK_EQ(count(Code::RowStart) >= 1, true);
    CHECK_EQ(count(Code::Attach) >= 1, true);
    CHECK_EQ(count(Code::Detach) >= 1, true);
    // time stamps never go backwards
    for (size_t i = 1; i < ev.size(); i++) CHECK_EQ(int32_t(ev[i].ms - ev[i - 1].ms) >= 0, true);
}
// Detail screen of a step: the stored transition capture shows as a VBUS band with the Accept and trigger marks.
static void detail_capture(Ui& ui, bool with_capture) {
    fixture(); two_pdos(); g_stub.request_err = pd::Err::Busy;
    Ui probe; open(probe); run_to_end(probe);
    const uint32_t run = probe.charger_test_results().run_id();
    (void)ui;
    fixture(); two_pdos(); g_stub.request_err = pd::Err::Busy;
    g_stub.tc_has_meta = with_capture;
    g_stub.tc_meta = {};
    g_stub.tc_meta.run_id = run; g_stub.tc_meta.step = 0xFFFF;
    g_stub.tc_meta.thumb.lo.fill(128); g_stub.tc_meta.thumb.hi.fill(128);
    g_stub.tc_meta.thumb.vmax_mv = 9000; g_stub.tc_meta.thumb.accept_col = 40; g_stub.tc_meta.thumb.trig_col = 60;
}
static void test_detail_capture() {
    for (int with = 0; with < 2; ++with) {
        Ui ui; detail_capture(ui, with);
        open(ui); run_to_end(ui);
        CHECK_EQ(ui.charger_test_stage(), 6);                 // Results
        top(ui);                                              // line 0 is a unit (the grid has no section rows)
        press(ui, Button::Ok, 9100); press(ui, Button::Ok, 9100);   // first group -> its first step
        CHECK_EQ(ui.charger_test_stage(), 8);                 // Detail
        ui.tick(9200); settle(ui, 9200);
        const Rect box = compute_layout(orientation_for(0).edge, kSlotCenters, kSlotLens, kPanelThickness).twin_content;
        auto px = [&](int x, int y) { return g_panel[(box.y + y) * LCD_WIDTH + box.x + x]; };
        CHECK_EQ(box.w, kTwinContentW); CHECK_EQ(box.h, 240);
        if (with) {
            CHECK_EQ(px(6 + 20, 167 + 12), kVolt);            // the band at 128/255 of 9 V
            CHECK_EQ(px(6 + 40 * 2, 167 + 0), kCaution);      // the Accept mark, above the band
            CHECK_EQ(px(6 + 60 * 2, 167 + 0), kError);        // the trigger mark
        } else {
            CHECK_EQ(px(6 + 20, 167 + 12), kBg);
            CHECK_EQ(px(6 + 40 * 2, 167 + 0), kBg);
        }
    }
}
// START's wait never blocks: the confirming OK returns into Waiting, the main loop's ticks carry the wait, and BACK / OK
// (a key, or the remote's press) cancel it with the Rd released. The wait is the LAST stage: when the charger is up the run
// starts by itself, so a remote caller must not press OK while the stage is Waiting (tools/ct_start.py polls for it).
static void dump_screen(const char* name);
static void test_start_wait_is_not_blocking() {
    { // what the screen says while the K2 waits (K2_DUMP: the screen for the design review), 20 columns at most
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1350);
        for (uint32_t t = 1370; t <= 4400; t += 20) { fusb302::service(); ui.tick(t); }
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        dump_screen("start_waiting");
    }
    { // the confirming OK returns at once, Rd presented, nothing started, the clock untouched
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300);
        const auto ms0 = g_stub.ms;
        confirm_press_nowait(ui, 1300);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(g_stub.rd_run, true);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.delays_ms, 0u);
        CHECK_EQ(g_stub.ms - ms0 < 100, true);           // a handful of millis() reads, not a 5 s loop
        for (uint32_t t = 1320; t < 3000; t += 20) { fusb302::service(); ui.tick(t); }
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);   // 1.7 s on, no charger: still waiting, Rd still presented
        CHECK_EQ(g_stub.rd_run, true);
        CHECK_EQ(g_stub.delays_ms, 0u);
    }
    { // BACK cancels: Review again, Rd released, and the dead wait does nothing on the next ticks
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1300);
        ui.tick(1400);
        press(ui, Button::Back, 1500);
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_cursor(), 0);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.rd_run_changes, 2);
        CHECK_EQ(ui.charger_test_busy(), false);
        for (uint32_t t = 1520; t < 30000; t += 20) ui.tick(t);
        CHECK_EQ(g_stub.rd_run_changes, 2); CHECK_EQ(g_stub.rd_pauses, 0);
        CHECK_EQ(ui.charger_test_stage(), 4);
        CHECK_EQ(g_stub.requests + g_stub.fc_starts, 0);   // nothing runs; START's own e-marker scan (VBUS 0) is the one read
        CHECK_EQ(g_stub.scans, 1); CHECK_EQ(g_stub.rescans + g_stub.lib_saves + g_stub.spoof_arms, 0);
    }
    { // OK is free mid-wait (BACK reads Stop): it leaves the wait alone; BACK then cancels with nothing left on
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1300);
        ui.tick(1400);
        press(ui, Button::Ok, 1800);
        CHECK_EQ(ui.charger_test_stage(), 13);
        press(ui, Button::Back, 1900);
        CHECK_EQ(ui.charger_test_stage(), 4);
        CHECK_EQ(g_stub.rd_run, false);
        CHECK_EQ(ui.charger_test_busy(), false);
        press(ui, Button::Ok, 2000);                     // the cancel took the confirmation with it: a fresh first OK, nothing started
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(ui.charger_test_busy(), false);
    }
    { // leaving the page mid-wait releases the Rd and drops the dialog's identity
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.lib = {saved(1)};
        Ui ui; open(ui); review(ui);
        CHECK_EQ(ui.charger_test_set_cable({ctcable::Kind::Saved, 1}), true);
        start_press(ui, 1300); confirm_press_nowait(ui, 1300);   // no CC readings in the stub: the guard cannot judge, the identity goes on
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(g_stub.rd_run, true);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 1500);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false);
    }
    { // a charger that answers during the wait: the run starts by itself, no further press
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1300);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        finish_wait(ui, 1300);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(g_wait_end - 1300 <= 60, true);
        CHECK_EQ(ui.charger_test_busy(), true);
    }
    { // the wait ends without a charger: back on Review with the refusal on the footer, Rd released, the confirmation spent
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_run(ui, 1300);
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_cursor(), 0);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(ui.charger_test_busy(), false);
        press(ui, Button::Ok, g_wait_end + 100);          // a new first OK, not a start
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.rd_run, false);
    }
}
static uint32_t g_hook_t0 = 0;
static void caps_after_700ms(uint32_t t) {   // the C135's Source_Capabilities, ~700 ms after the Rd
    if (t - g_hook_t0 >= 700) { g_stub.caps.valid = true; g_stub.caps.active_cc = 1; g_stub.caps.num_pdos = 1; }
}
static void test_start_waits_for_the_caps() {
    { // VBUS is up at once, the caps follow: the wait ends with them, so the run's plan already has the offers
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        g_stub.caps.valid = false; g_stub.caps.num_pdos = 0; g_stub.caps.active_cc = 0;
        Ui ui; open(ui); review(ui);
        g_hook_t0 = 1300;
        start_press(ui, 1300); confirm_press_nowait(ui, 1300);
        finish_wait(ui, 1300, caps_after_700ms);
        CHECK_EQ(g_wait_end - 1300 >= 700 && g_wait_end - 1300 <= 800, true);
        CHECK_EQ(g_stub.caps.valid, true);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // a source that never speaks PD: 2 s of silence after VBUS up, then the run goes on without caps
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        g_stub.caps.valid = false; g_stub.caps.num_pdos = 0; g_stub.caps.active_cc = 0;
        Ui ui; open(ui); review(ui);
        start_run(ui, 1300);
        CHECK_EQ(g_wait_end - 1400 >= 1900 && g_wait_end - 1400 <= 2200, true);
        CHECK_EQ(g_stub.rd_run, true);
    }
}
static void test_decisions_use_the_unaveraged_vbus() {
    // The 128-sample average (~2.5 s) lags a step on VBUS: display reads it, every judgement reads the latest sample.
    { // the PD engine's guards see the live VBUS
        fixture(); g_stub.pm.vbus_mv = 500; g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 5000;
        Ui ui; open(ui);
        ui.tick(1200);
        CHECK_EQ(g_stub.sink_vbus, 5000);
    }
    { // a charger that switched on just now: no Rd, no wait, and the run's preflight sees the live 5 V
        fixture(); g_stub.pm.vbus_mv = 500; g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 5000;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300);
        CHECK_EQ(g_stub.rd_run_changes, 0);
        CHECK_EQ(g_stub.delays_ms, 0u);
        ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
        CHECK_EQ(ui.charger_test_busy(), true);   // the Runner's input carried 5000 mV, not the averaged 500
    }
    { // the other way: the source just went away, the average still reads 5 V
        fixture(); g_stub.pm.vbus_mv = 5000; g_stub.pm_fast_set = true; g_stub.pm_fast_mv = 0; g_stub.vbus_on_rd = false;
        Ui ui; open(ui); review(ui);
        start_run(ui, 1300);
        CHECK_EQ(g_stub.rd_run, false);   // START found nothing live: it waited for Rd (10 s) and then refused
        CHECK_EQ(g_wait_end - 1400 >= 19400, true);
    }
}
// ---- the one cable-identity list of the Cable step (Off, the built-in chips, the saved clones) ----
using ctcable::Choice; using ctcable::Kind;
static constexpr Choice kOff{}, kBuiltin{Kind::Builtin, 1}, kSaved1{Kind::Saved, 1}, kSaved2{Kind::Saved, 2}, kSaved9{Kind::Saved, 9}, kMalformed{Kind::Off, 3};
static bool named(const ctcable::Info& i, std::string_view name) { return std::string_view(i.run_name.data()) == name; }
// A charger that is not up yet and comes on with the Rd: START takes the Rd path (scan, Rd, wait), not "VBUS already up".
static void rd_fixture() { fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true; }
// The Cable step with one row picked (the pick advances): the Review page with that identity chosen.
static void review_with(Ui& ui, int row, uint32_t now = 1200) {
    cable(ui, now); pick_row(ui, row, now);
    CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_cursor(), 0);
}
static void test_cable_list_rows_and_default() {
    fixture(); g_stub.lib = {saved(1), saved(2)};
    Ui ui; open(ui); review(ui);
    start_press(ui, 1300);                               // no dialog: START goes straight to the confirm window
    CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_busy(), false);
    press(ui, Button::Back, 1400);                       // out of the confirm window: Review again, then back to the list
    press(ui, Button::Back, 1500);
    CHECK_EQ(ui.charger_test_stage(), 2);
    ui.tick(1600); settle(ui, 1600);
    CHECK_EQ(ui.list_state().count, kSavedRow + 2);      // Off, three built-ins, two saved clones
    CHECK_EQ(ui.charger_test_cursor(), 0);               // the cursor sits on the pick in force
    check_row_text(0, "Off (real cable)", kText, kFocusFill, 0, kNoTailRoom); check_radio(0, true, 0, kFocusFill);
    static constexpr const char* kShort[] = {"EPR", "Xiaomi", "OnePlus"};
    for (unsigned r = 0; r < ctcable::kDialogBuiltins; ++r)
        check_row_text(kBuiltinRow + int(r), kShort[r], kText, kBg, 0, kit::kCol2X - 4 - kit::kTextX);
    check_row_text(kSavedRow, "Host identity 1", kText, kBg, 0, kSavedRoom);
    no_operations(); CHECK_EQ(g_stub.ra_ons + g_stub.rd_run_changes, 0);
    CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
}
static void test_cable_dialog_off_is_todays_start() {
    rd_fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
    start_press(ui, 1300);                               // the first OK: the confirmation only
    CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(ui.charger_test_busy(), false);
    confirm_press(ui, 1400);                             // Off is the default: nothing is armed, the Rd is the last thing
    CHECK_EQ(g_stub.rd_run, true); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.spoof_arms, 0);
    CHECK_EQ(ui.charger_test_busy(), true);
    for (uint32_t t = 1500; t <= 400000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.ra_ons + g_stub.ra_offs + g_stub.spoof_arms + g_stub.spoof_disarms, 0);
    CHECK_EQ(g_stub.rd_run, false);
}
static void test_cable_dialog_builtin_run_owns_ra_and_spoof() {
    rd_fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review_with(ui, kBuiltinRow);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);      // picked, not armed
    start_press(ui, 1400);                               // the first OK: the confirmation, nothing armed
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons + g_stub.rd_run_changes + g_stub.scans, 0);
    CHECK_EQ(ui.charger_test_stage(), 4);
    confirm_press_nowait(ui, 1400);                      // the second: guard, arm, Ra, then the Rd and the wait for the charger
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(ctcable::kBuiltinKind), true);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.ra_ons, 1); CHECK_EQ(g_stub.force_line, 2);
    CHECK_EQ(g_stub.rd_run, true);
    CHECK_EQ(ui.charger_test_busy(), false);
    finish_wait(ui, 1400);                               // the run starts by itself when the charger is up
    CHECK_EQ(ui.charger_test_stage(), kRunningStage); CHECK_EQ(ui.charger_test_busy(), true);
    const auto info = ui.charger_test_cable_info();
    CHECK_EQ(info.run == kBuiltin, true); CHECK_EQ(info.choice == kBuiltin, true); CHECK_EQ(named(info, "EPR 50V 5A"), true);
    uint32_t now = g_wait_end + 100;                                 // the Pd unit detaches and attaches before its first request
    for (; now <= 20000 && g_stub.requests < 1; now += 100) ui.tick(now);
    CHECK_EQ(g_stub.requests, 1);
    CHECK_EQ(g_stub.unit_detaches >= 1, true);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.ra_offs, 0); CHECK_EQ(g_stub.spoof_on, true);   // both stay across the re-attach
    for (; now <= 400000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.force_line, 0);   // the run is over: Ra off, the scan line free again
    CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_disarms, 1); CHECK_EQ(g_stub.rd_run, false);
    CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);    // the card header still names the identity used
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);   // the choice is remembered for the next run
}
// Baseus 70W (2026-11-07): the Ra mode makes the charger deaf to the D+/D- handshakes, so the identity (Ra + spoof) is on for the
// Pd-mode units only. START arms it, the Legacy unit's attach drops it (plain Rd, nothing spoofed), the Pd unit's attach puts it
// back, the end of the run drops it for good; the card header keeps naming the identity of the run throughout.
static void test_cable_identity_is_dropped_for_a_legacy_unit_and_back_for_the_pd_unit() {
    static constexpr uint8_t kQc2ThenPd[] = {uint8_t(ct::Unit::Qc2), uint8_t(ct::Unit::Pd)};
    rd_fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kQc2ThenPd), true); review_with(ui, kBuiltinRow);
    start_press(ui, 1400);
    confirm_press_nowait(ui, 1400);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.ra_ons, 1); CHECK_EQ(g_stub.spoof_arms, 1);
    finish_wait(ui, 1400);
    CHECK_EQ(ui.charger_test_busy(), true);
    uint32_t now = g_wait_end + 100;
    bool legacy_seen = false;
    for (; now <= 400000 && ui.charger_test_busy() && g_stub.requests < 1; now += 100) {   // up to the Pd unit's first request
        ui.tick(now);
        if (g_stub.legacy_attach && g_stub.unit_attaches > 0) {   // inside the Legacy unit's attach: plain Rd, nothing spoofed
            legacy_seen = true;
            CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.force_line, 0);
            CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
        }
    }
    CHECK_EQ(legacy_seen, true);
    CHECK_EQ(g_stub.requests, 1);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.force_line, 2);   // the Pd unit runs under the identity
    CHECK_EQ(g_stub.ra_offs, 1); CHECK_EQ(g_stub.ra_ons, 2); CHECK_EQ(g_stub.spoof_arms, 2); CHECK_EQ(g_stub.spoof_disarms, 1);
    CHECK_EQ(g_stub.ra_owned >= 1, true);   // the re-armed Ra mode belongs to the run again (it survives the unit's detach)
    for (; now <= 400000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.rd_run, false);
    CHECK_EQ(g_stub.spoof_disarms, 2);
    CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
}
// Hardware 2026-11-07: after scripted runs with an identity the spoof stayed armed (Tools showed ARMED, the next START said "tool
// busy"). An identity armed BY A RUN is the run's own: however the run ends, the K2 is passive again (Ra off, scan line free,
// spoof disarmed, no Rd) and the next START is not refused.
static bool passive_again() {
    return !g_stub.ra_on && !g_stub.spoof_on && g_stub.force_line == 0 && !g_stub.rd_run;
}
static void expect_passive(const char* how, unsigned units, unsigned offset) {
    if (!passive_again())
        std::printf("  not passive after %s (units %u, offset %u): ra=%d spoof=%d line=%d rd=%d\n", how, units, offset,
                    int(g_stub.ra_on), int(g_stub.spoof_on), g_stub.force_line, int(g_stub.rd_run));
    CHECK_EQ(passive_again(), true);
}
// START with the built-in identity on `units` (empty = the default order); the wait is run out, `now` ends at the first tick after.
static void begin_identity_run(Ui& ui, std::span<const uint8_t> units, uint32_t& now) {
    open(ui);
    if (!units.empty()) CHECK_EQ(ui.charger_test_set_units(units), true);
    review_with(ui, kBuiltinRow);
    start_press(ui, 1400); confirm_press_nowait(ui, 1400);
    finish_wait(ui, 1400);
    now = g_wait_end + 100;
}
static const std::span<const uint8_t> kIdentityUnitSets[] = {
    {}, kPdOnly,
    []{ static constexpr uint8_t u[] = {uint8_t(ct::Unit::Qc2)}; return std::span<const uint8_t>(u); }(),
    []{ static constexpr uint8_t u[] = {uint8_t(ct::Unit::Qc2), uint8_t(ct::Unit::Pd)}; return std::span<const uint8_t>(u); }(),
    []{ static constexpr uint8_t u[] = {uint8_t(ct::Unit::Pd), uint8_t(ct::Unit::Qc2)}; return std::span<const uint8_t>(u); }(),
};
// Both START paths: the charger not up yet (the Rd brings it, START waits) and VBUS already up (the run begins at once).
static bool g_vbus_up = false;
static void identity_fixture() { if (g_vbus_up) fixture(); else rd_fixture(); }
static void run_identity_is_released_when_the_run_ends() {
    for (unsigned set = 0; set < std::size(kIdentityUnitSets); ++set) {
        { // Done
            identity_fixture(); Ui ui; uint32_t now;
            begin_identity_run(ui, kIdentityUnitSets[set], now);
            CHECK_EQ(ui.charger_test_busy(), true);
            for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
            CHECK_EQ(ui.charger_test_busy(), false);
            expect_passive("done", set, 0);
            CHECK_EQ(ui.spoof_busy(), false);
        }
        for (unsigned how = 0; how < 3; ++how) {   // cancelled at any point: BACK, OK, leaving the page
            for (const unsigned offset : {0u, 3u, 21u, 320u}) {
                identity_fixture(); Ui ui; uint32_t now;
                begin_identity_run(ui, kIdentityUnitSets[set], now);
                for (unsigned i = 0; i < offset && ui.charger_test_busy(); ++i, now += 100) ui.tick(now);
                if (!ui.charger_test_busy()) continue;
                if (how == 0) press(ui, Button::Back, now);
                if (how == 1) press(ui, Button::Ok, now);
                if (how == 2) ui.goto_page(static_cast<uint8_t>(Page::Home), now);
                for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
                CHECK_EQ(ui.charger_test_busy(), false);
                expect_passive(how == 0 ? "back" : how == 1 ? "ok" : "page leave", set, offset);
                CHECK_EQ(ui.spoof_busy(), false);
            }
        }
    }
    if (g_vbus_up) return;   // the two cases below need START to wait for the charger
    { // START cancelled while Waiting
        rd_fixture(); Ui ui; open(ui); review_with(ui, kBuiltinRow);
        start_press(ui, 1400); confirm_press_nowait(ui, 1400);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        press(ui, Button::Back, 1500);
        expect_passive("waiting cancel", 0, 0);
        CHECK_EQ(ui.spoof_busy(), false);
    }
    { // START refused when the wait ends (the charger never comes up): back on Review, passive
        rd_fixture(); g_stub.vbus_on_rd = false;
        Ui ui; open(ui); review_with(ui, kBuiltinRow);
        start_press(ui, 1400); confirm_press_nowait(ui, 1400);
        finish_wait(ui, 1400);
        CHECK_EQ(ui.charger_test_busy(), false);
        expect_passive("refused launch", 0, 0);
        CHECK_EQ(ui.spoof_busy(), false);
    }
}
static void test_run_identity_is_released_when_the_run_ends() {
    for (const bool up : {false, true}) { g_vbus_up = up; run_identity_is_released_when_the_run_ends(); }
    g_vbus_up = false;
}
// Scripted sessions (ct_start): run after run on the SAME page, the identity choice changing in between, each one ending its own way.
static void run_identity_survives_a_session() {
    const unsigned kRuns = 2;
    for (const unsigned set : {0u, 4u}) {   // the default order, Pd after a legacy unit
        for (unsigned plan = 0; plan < 9; ++plan) {   // every pair of choices; run n: choice = (plan / 3^n) % 3 (0 off, 1 builtin, 2 saved), ending = alternate
            identity_fixture(); g_stub.lib = {saved(1)};
            Ui ui; open(ui);
            uint32_t now = 2000;
            for (unsigned n = 0; n < kRuns; ++n) {
                unsigned div = 1; for (unsigned i = 0; i < n; ++i) div *= 3;
                const unsigned choice = (plan / div) % 3;
                const bool cancel = (n + plan) % 2 == 1;
                while (ui.charger_test_stage() != 0 && now < 3000000) { press(ui, Button::Back, now); now += 10; }
                CHECK_EQ(ui.charger_test_stage(), 0);
                if (!kIdentityUnitSets[set].empty()) CHECK_EQ(ui.charger_test_set_units(kIdentityUnitSets[set]), true);
                review_with(ui, choice == 0 ? kOffRow : choice == 1 ? kBuiltinRow : kSavedRow, now);
                start_press(ui, now + 100); confirm_press_nowait(ui, now + 100);
                finish_wait(ui, now + 100);
                now = g_wait_end + 100;
                if (!ui.charger_test_busy()) { std::printf("  run %u of plan %u did not start (stage %d)\n", n, plan, ui.charger_test_stage()); CHECK_EQ(1, 0); break; }
                if (cancel) { for (unsigned i = 0; i < 25 && ui.charger_test_busy(); ++i, now += 100) ui.tick(now); press(ui, Button::Back, now); }
                for (; now <= 90000000 && ui.charger_test_busy(); now += 100) ui.tick(now);
                CHECK_EQ(ui.charger_test_busy(), false);
                expect_passive(cancel ? "cancelled run" : "done run", set, plan * 10 + n);
                CHECK_EQ(ui.spoof_busy(), false);
            }
        }
    }
}
static void test_run_identity_survives_no_run_in_a_session() {
    for (const bool up : {false, true}) { g_vbus_up = up; run_identity_survives_a_session(); }
    g_vbus_up = false;
}
// The run's own identity: gone at the end, Review names it as chosen but no longer armed, the next START is not refused.
static void test_review_after_a_run_with_an_identity() {
    rd_fixture(); Ui ui; uint32_t now;
    begin_identity_run(ui, {}, now);
    CHECK_EQ(g_stub.spoof_on, true);
    for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    expect_passive("done", 0, 0);
    ui.goto_page(static_cast<uint8_t>(Page::Home), now + 100);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now + 200);
    review(ui, now + 400); ui.tick(now + 500);
    check_review_spoof(std::string(emrec::builtin_info(1).label), kText);   // chosen, no longer armed: plain text
    CHECK_EQ(ui.spoof_busy(), false);
}
// A self-powered K2 loses VBUS mid-run: the run is suspended (not over), the page waits on Resume. BACK there abandons the run, and
// the identity START armed for it must go with it (found while chasing the 2026-11-07 leak: abandon_run only released what a
// host-powered resume had put on the line).
static void test_abandoned_resume_releases_the_runs_identity() {
    for (const bool self : {true, false}) {
        fixture();
        if (self) g_stub.self_powered = true; else g_stub.bus_powered = true;
        g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
        Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review_with(ui, kBuiltinRow);
        start_press(ui, 1400); confirm_press(ui, 1400);
        uint32_t now = g_wait_end + 100;
        CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(g_stub.spoof_on, true);
        const auto count_pass = [&] {
            unsigned n = 0;
            for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;   // decided (top-down: the stub holds 5 V, the high rows fail first)
            return n;
        };
        for (; now <= 60000 && count_pass() < 1; now += 100) ui.tick(now);
        g_stub.pm.vbus_mv = 0;
        for (const uint32_t end = now + 1000; now < end; now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        press(ui, Button::Back, now);                     // abandon
        CHECK_EQ(ui.charger_test_stage() == kResumeStage, false);
        expect_passive(self ? "abandoned resume (self-powered)" : "abandoned resume (bus-powered)", self, 0);
        CHECK_EQ(ui.spoof_busy(), false);
    }
}
static void test_cable_dialog_saved_clone() {
    rd_fixture(); g_stub.lib = {saved(1), saved(2)};
    Ui ui; open(ui); review_with(ui, kSavedRow + 1);
    CHECK_EQ(g_stub.spoof_arms, 0);
    start_run(ui, 1400);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == g_stub.lib[1].rec, true);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.rd_run, true);
    const auto info = ui.charger_test_cable_info();
    CHECK_EQ(info.run == kSaved2, true); CHECK_EQ(named(info, "Host identity 2"), true);
}
// Each built-in row presents its own captured identity (spoof record, name, Ra) and the dialog remembers it.
static void test_cable_dialog_every_builtin_row() {
    for (unsigned r = 0; r < ctcable::kDialogBuiltins; ++r) {
        const unsigned id = ctcable::dialog_id(r);       // ids 1, 2, 4: id 3 is retired and has no row
        rd_fixture(); g_stub.lib = {saved(1)};
        Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); cable(ui);
        static constexpr const char* kShort[] = {"EPR", "Xiaomi", "OnePlus"};   // the Cable step's short names
        const int row = kBuiltinRow + int(r);
        pick_row(ui, row, 1300);                         // a pick selects and goes on to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        press(ui, Button::Back, 1340); ui.tick(1350); settle(ui, 1350);    // back to the list: the cursor is on the pick, the radio is on
        CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), row);
        check_radio(row, true, row, kFocusFill);
        check_row_text(row, kShort[r], kText, kFocusFill, row, kit::kCol2X - 4 - kit::kTextX);
        press(ui, Button::Ok, 1360);                     // OK on the pick keeps it: Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        start_run(ui, 1400);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(id), true);
        CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.rd_run, true);
        const auto info = ui.charger_test_cable_info();
        const Choice want{Kind::Builtin, uint16_t(id)};
        CHECK_EQ(info.run == want, true); CHECK_EQ(info.choice == want, true);
        CHECK_EQ(named(info, emrec::builtin_info(id).label.data()), true);
    }
}
// Three built-ins plus ten saved clones overflow the 7-row list: the view follows the cursor and keeps its rows.
static void test_cable_dialog_scrolls_with_many_clones() {
    rd_fixture();
    for (unsigned n = 1; n <= 10; ++n) g_stub.lib.push_back(saved(n));
    Ui ui; open(ui); cable(ui);
    CHECK_EQ(ui.list_state().count, 14); CHECK_EQ(ui.list_state().visible, 8); CHECK_EQ(ui.list_state().offset, 0);
    ui.tick(1350); settle(ui, 1350);
    check_row_text(0, "Off (real cable)", kText, kFocusFill, 0, kNoTailRoom);   // the cursor starts on the pick in force (Off), its note open
    check_row_text(3, "OnePlus", kText, kBg, 0, kit::kCol2X - 4 - kit::kTextX);
    move(ui, 7, 1400);                                   // row 7 is still in the 8-row window
    CHECK_EQ(ui.charger_test_cursor(), 7); CHECK_EQ(ui.list_state().offset, 0);
    move(ui, 7, 1400);
    CHECK_EQ(ui.charger_test_cursor(), 13); CHECK_EQ(ui.list_state().offset, 6);   // the last row is the bottom row
    ui.tick(1500); settle(ui, 1500);
    check_row_text(0, "Host identity 3", kText, kBg, 7, kSavedRoom);    // names come from the cache filled when the window moved
    check_row_text(6, "Host identity 9", kText, kBg, 7, kSavedRoom);
    check_row_text(7, "Host identity 10", kText, kFocusFill, 7, kSavedRoom);
    press(ui, Button::Ok, 1550); ui.tick(1550);          // OK picks the scrolled-to clone and goes on to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    start_run(ui, 1600);                                 // START arms that clone
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == g_stub.lib[9].rec, true);
    const Choice want{Kind::Saved, 10};
    CHECK_EQ(ui.charger_test_cable_info().run == want, true);
}
static void test_cable_dialog_refusals() {
    { // Ra refused (a contract, a busy tool ...): nothing stays armed, nothing starts, Review stays
        rd_fixture(); g_stub.ra_refuse = true;
        Ui ui; open(ui); review_with(ui, kBuiltinRow);
        start_press(ui, 1400);
        CHECK_EQ(g_stub.spoof_arms, 0);                  // the first OK arms nothing: the refusal comes with the second
        confirm_press(ui, 1500);
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(g_stub.ra_on, false);
        CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.spoof_disarms, 1);
        CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
        press(ui, Button::Ok, 1600); CHECK_EQ(ui.charger_test_stage(), 4);   // the confirmation is spent: a first OK again, no dialog
        CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.rd_run_changes, 0);
    }
    { // a live sink inline: the spoof refuses, no Ra is attempted
        rd_fixture();
        Ui ui; open(ui); review_with(ui, kBuiltinRow);
        g_stub.inl = true; g_stub.link_inline = true;   // after the page's own ticks, which recompute the link state
        start_press(ui, 1400); confirm_press(ui, 1500);
        CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run, false);
        CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
    }
    { // a saved clone deleted after it was picked
        rd_fixture(); g_stub.lib = {saved(1)};
        Ui ui; open(ui); review_with(ui, kSavedRow);
        g_stub.lib.clear();
        start_press(ui, 1400); confirm_press(ui, 1500);
        CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run, false);
    }
    { // the identity tool or the 5 V handover owns the PD engine
        rd_fixture(); g_stub.ident.state = pd::IdState::Running;
        Ui ui; open(ui); review_with(ui, kBuiltinRow);
        start_press(ui, 1400); confirm_press(ui, 1500);
        CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run, false);
    }
}
static void test_cable_dialog_back_during_confirm_goes_passive() {
    rd_fixture();
    Ui ui; open(ui); review_with(ui, kBuiltinRow);
    start_press(ui, 1400); confirm_press_nowait(ui, 1400);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.rd_run, true);
    press(ui, Button::Back, 1500);                      // out of the wait: Ra, spoof and Rd go
    CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.rd_run, false);
    CHECK_EQ(g_stub.force_line, 0);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(ui.charger_test_stage(), 4);
}
// Back out of the confirm window itself (before the second OK): nothing was armed, so nothing has to go.
static void test_confirm_window_back_and_expiry_touch_nothing() {
    rd_fixture();
    Ui ui; open(ui); review_with(ui, kBuiltinRow);
    start_press(ui, 1400);
    press(ui, Button::Back, 1500);
    CHECK_EQ(ui.charger_test_stage(), 4);
    nothing_armed_stub();
    start_press(ui, 1600);
    ui.tick(1600 + Ui::kConfirmWindowMs + 100);          // the window runs out
    CHECK_EQ(ui.charger_test_stage(), 4);
    nothing_armed_stub();
    confirm_press(ui, 7000);                             // a late second OK is a first one again
    CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed_stub();
}
static void test_cable_choice_remote_default() {
    fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_cable(kSaved9), false);   // not in the library
    CHECK_EQ(ui.charger_test_set_cable(kMalformed), false);     // malformed
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    cable(ui);
    CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow);      // the Cable step opens on the pick in force
    CHECK_EQ(ui.charger_test_set_cable(kSaved1), true);   // a remote choice while the list is open moves the cursor to it
    CHECK_EQ(ui.charger_test_cursor(), kSavedRow);
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow);
    CHECK_EQ(ui.charger_test_set_cable(kSaved1), true);
    CHECK_EQ(ui.charger_test_cable_info().choice == kSaved1, true);
    CHECK_EQ(ui.charger_test_set_cable(kOff), true);
    CHECK_EQ(ui.charger_test_cable_info().choice == kOff, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);                       // a default is a default: nothing happens until START
}
// ---- reboot-resume on a charger-powered K2 -----------------------------------------------------------------------------
// A run of the Pd unit on a charger-powered K2 (Rd pinned, nobody at the buttons), cut after its first final step: the journal
// as the next boot finds it. `unattended` is what the first boot journaled.
static void interrupted_run(bool unattended) {
    fixture(); g_stub.self_powered = true;
    {
        Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        CHECK_EQ(ui.charger_test_busy(), true);
        settle_arm(ui, 1400);
        CHECK_EQ(g_stub.journal_metas, 1);
        CHECK_EQ(g_stub.journal_meta.unattended, true);   // self_powered() at the start decides it
        CHECK_EQ(g_stub.journal_meta.units.n, 2);
        const auto count_pass = [&] {
            unsigned n = 0;
            for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;   // decided (top-down: the stub holds 5 V, the high rows fail first)
            return n;
        };
        uint32_t now = 1500;
        for (; now <= 60000 && count_pass() < 1; now += 100) ui.tick(now);
        CHECK_EQ(count_pass() >= 1, true);
        CHECK_EQ(g_stub.bb_holds > 5, true);                 // the live run renews the Black Box's Contract hold on its ticks
        auto& r = g_stub.journal;
        r = {};
        r.found = r.unfinished = true;
        r.run_id = ui.charger_test_results().run_id();
        r.options.selected = algos(ui); r.options.units = g_stub.journal_meta.units;
        r.evidence = ui.charger_test_results().evidence();
        const auto steps = ui.charger_test_results().steps();
        for (std::size_t i = 0; i < steps.size(); ++i) {
            r.steps[i] = steps[i];
            if (r.steps[i].verdict == ct::Verdict::Running) r.steps[i].verdict = ct::Verdict::Pending;
        }
        r.count = steps.size();
        r.steps[r.count - 1].verdict = ct::Verdict::Pending;   // the power went before the last step had a result
        r.steps[r.count - 1].reason = ct::Reason::None;
        r.has_meta = true; r.meta = g_stub.journal_meta; r.meta.unattended = unattended;
    }
    g_stub.journal_load = true;       // the next boot replays it
    g_stub.journal_metas = g_stub.journal_finishes = g_stub.journal_resumes = 0;
    g_stub.requests = g_stub.releases = 0;
    g_stub.pm.vbus_mv = 5000;
}
// A run journaled without the PPS sweep bit (older firmware, a Medium run, or charger-sweep off) resumes without it: the
// replayed options replace the session's default (here Deep, as Settings > Test level sets it at boot), so the resumed plan
// matches the journaled rows.
static void test_an_old_journal_resumes_without_the_default_sweep() {
    interrupted_run(true);
    CHECK_EQ(g_stub.journal.meta.pps_sweep, false);   // this run itself started at the default level, Medium: no sweep
    g_stub.journal.meta.pps_sweep = false; g_stub.journal.options.pps_sweep = false;   // what an older journal replays to
    Ui ui; ui.charger_test_set_level(uint8_t(ct::Level::Deep)); ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(ui.charger_test_selection() & (1u << 31), 0u);
}
// The identity of a charger-powered run (spoof + Ra on CC1, Rd pinned on CC2 by PLUS) is re-armed by the resume.
static void test_resume_rearms_the_cable_identity() {
    interrupted_run(true);
    g_stub.journal.meta.cable_kind = static_cast<uint8_t>(kBuiltin.kind); g_stub.journal.meta.cable_seq = kBuiltin.seq;
    g_stub.spoof_arms = g_stub.ra_ons = 0; g_stub.ra_on = g_stub.spoof_on = false;
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    // The charger asks the cable at its attach, long before the resume gate: the identity and the Ra are on from the boot.
    CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Arm));
    uint32_t now = 1100;
    for (; now <= 2400 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(g_stub.spoof_arms, 1);                   // the resume does not arm it a second time
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
    CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
    CHECK_EQ(g_stub.journal_resumes, 1);
}
// A K2 whose journaled identity cannot be put on at boot (the Ra mode refuses) says so and still resumes: the resume's own
// re-arm tries again once VBUS is steady.
static void test_resume_boot_arm_refused_is_retried_by_the_resume() {
    interrupted_run(true);
    g_stub.journal.meta.cable_kind = static_cast<uint8_t>(kBuiltin.kind); g_stub.journal.meta.cable_seq = kBuiltin.seq;
    g_stub.spoof_arms = g_stub.ra_ons = 0; g_stub.ra_on = g_stub.spoof_on = false;
    g_stub.ra_refuse = true;
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false);   // nothing is left half armed
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Failed));
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    g_stub.ra_refuse = false;
    uint32_t now = 1100;
    for (; now <= 2400 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
}
// A key-mode / inherited-pin boot with an identity selected arms it and puts the Ra on before anything asks the charger.
static void boot_with_selection(bool self_powered, int cc1_mv, bool have_reading) {
    fixture(); g_stub.self_powered = self_powered;
    bootra::note(bootra::Verdict::NotRun);
    if (have_reading) { g_stub.adc = k2::adc::Reading{}; g_stub.adc->cc1_mv = cc1_mv; g_stub.adc->cc2_mv = 900; }
}
static void test_boot_arms_the_selected_identity_when_self_powered() {
    boot_with_selection(true, 3290, true);
    Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
    CHECK_EQ(g_stub.ra_ons, 1);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Arm));
    CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
}
static void test_boot_does_not_arm_without_an_identity_or_when_not_self_powered_or_unsafe() {
    {   // no identity: today's boot
        boot_with_selection(true, 3290, true);
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
        CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoIdentity));
    }
    {   // a HID-powered boot keeps START's detach / re-attach arming
        boot_with_selection(false, 3290, true);
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
        CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NotSelfPowered));
    }
    {   // the guard cannot be evaluated: no reading, only a selection -> logged, not armed
        boot_with_selection(true, 0, false);
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
        CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoReading));
    }
    {   // the other line shows no Rp: a cable in between
        boot_with_selection(true, 40, true);
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
        CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Cable));
    }
    {   // the Ra mode refuses (a sink inline, a contract): nothing is left armed
        boot_with_selection(true, 3290, true);
        g_stub.ra_refuse = true;
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.ra_on, false); CHECK_EQ(g_stub.spoof_on, false);
        CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Failed));
    }
}
// The Cable step with a boot-armed identity and a different pick: the pick wears NEXT, the identity answering now wears
// CURR (the old bug: the ARMED pill stayed on the boot/Tools identity while OFF had the radio). START disarms the old
// identity and arms the pick; with pick Off nothing stays armed. Review shows the pick plain.
static void test_cable_step_next_curr_and_start_takes_over() {
    {   // pick Off: OFF wears NEXT, the boot-armed row CURR; START with Off disarms it
        boot_with_selection(true, 3290, true);
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);   // the saved pick: the PLUS boot arms it
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_arms, 1);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); settle(ui, 1100);
        cable(ui, 1200); ui.tick(1210); settle(ui, 1210);
        CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow);
        check_row_pill(kBuiltinRow, "ARMED", kCaution, kCautionT, kBuiltinRow);   // pick == armed
        CHECK_EQ(ui.charger_test_set_cable(kOff), true);   // pick Off while the boot identity answers
        ui.tick(1300); settle(ui, 1300);
        CHECK_EQ(ui.charger_test_cursor(), kOffRow);
        check_row_text(kOffRow, "Off (real cable)", kOk, kFocusFill, kOffRow, kNextPillRoom);
        check_row_pill(kOffRow, "NEXT", kOk, kOkT, kOffRow);
        check_row_pill(kBuiltinRow, "CURR", kCaution, kDimT, kOffRow);
        press(ui, Button::Ok, 1400);                    // OK on the Off row: selects and advances to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        ui.tick(1410); settle(ui, 1410); check_review_spoof("Off", kText);
        start_press(ui, 1500); confirm_press(ui, 1600); // START with Off disarms the boot-armed identity
        CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_disarms, 1); CHECK_EQ(g_stub.spoof_arms, 1);
        CHECK_EQ(ui.charger_test_busy(), true);
        for (uint32_t now = 1700; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.spoof_disarms, 1);
    }
    {   // a different pick: START disarms the old identity and arms the pick
        boot_with_selection(true, 3290, true);
        Ui ui; CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_arms, 1);
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); settle(ui, 1100);
        cable(ui, 1200); ui.tick(1210); settle(ui, 1210);
        const Choice cuck{Kind::Builtin, 2};            // Xiaomi 6A, row kBuiltinRow + 1
        CHECK_EQ(ui.charger_test_set_cable(cuck), true);
        ui.tick(1300); settle(ui, 1300);
        CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow + 1);
        check_row_pill(kBuiltinRow + 1, "NEXT", kOk, kOkT, kBuiltinRow + 1);
        check_row_pill(kBuiltinRow, "CURR", kCaution, kDimT, kBuiltinRow + 1);
        press(ui, Button::Ok, 1400);                    // OK on the pick: Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        ui.tick(1410); settle(ui, 1410); check_review_spoof(std::string(emrec::builtin_info(2).label), kText);
        start_press(ui, 1500); confirm_press(ui, 1600);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(2), true);
        CHECK_EQ(g_stub.spoof_disarms, 1); CHECK_EQ(g_stub.spoof_arms, 2);   // the old identity went, the pick armed
        CHECK_EQ(ui.charger_test_busy(), true);
        for (uint32_t now = 1700; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.spoof_on, false);               // the run's own arming went with its end
    }
}
// ---- the selection outlives a power cycle ---------------------------------------------------------------------------------
// "Reboot" = a new Ui over the same stub volume (g_stub holds the remembered record, the library and the spoof state).
static void reboot_with(bool self_powered, int cc1_mv = 3290) {
    bootra::note(bootra::Verdict::NotRun);
    g_stub.self_powered = self_powered;
    g_stub.spoof_arms = g_stub.ra_ons = 0; g_stub.ra_on = g_stub.spoof_on = false;   // the K2 lost power: nothing is armed
    g_stub.adc = k2::adc::Reading{}; g_stub.adc->cc1_mv = cc1_mv; g_stub.adc->cc2_mv = 900;
}
static void boot_ui(Ui& ui) { ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); }
// Pick a cable on the K2 (a HID-powered session), unplug, PLUS-boot into the charger: the identity is armed at that boot.
static void test_wizard_pick_survives_a_reboot_and_arms_on_a_self_powered_boot() {
    fixture();
    { Ui ui; open(ui); cable(ui); pick_row(ui, kBuiltinRow); CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true); }
    CHECK_EQ(g_stub.cable_pref == kBuiltin, true);
    CHECK_EQ(g_stub.cable_pref_sets, 1);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);                    // picking only selects
    reboot_with(true);
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);   // the selection is back
    CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Arm));
    CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
    CHECK_EQ(g_stub.cable_pref_sets, 1);                                // reading it back wrote nothing
}
// Tools -> Spoof selects too: its pick is remembered the same way.
static void test_spoof_tool_pick_survives_a_reboot() {
    fixture();
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(ui.push_menu(Menu::Spoof, 2000), true); ui.tick(2050);
        ui.on_button(Button::Plus, ButtonEvent::Short, 2100);          // row 1: the first built-in chip
        ui.on_button(Button::Plus, ButtonEvent::Short, 2110);          // row 2
        ui.on_button(Button::Ok, ButtonEvent::Short, 2200);            // selects and arms
        CHECK_EQ(g_stub.spoof_on, true);
    }
    const Choice second{Kind::Builtin, 2};
    CHECK_EQ(g_stub.cable_pref == second, true);
    CHECK_EQ(g_stub.cable_pref_sets, 1);
    reboot_with(true);
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == second, true);
    CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Arm));
    CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(2), true);
}
// A HID-powered boot gets the selection back but arms nothing: START's own arming stays the only one there.
static void test_hid_powered_boot_restores_the_selection_without_arming() {
    fixture();
    { Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true); }
    reboot_with(false);
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NotSelfPowered));
}
// A saved clone is remembered by its library id; one that is gone by the next boot reads as the default (the EPR spoof, not
// chosen: nothing arms at that boot) and the stale record is removed, so a later clone that takes the number is not picked up
// by accident.
static void test_saved_clone_selection_and_a_clone_that_is_gone() {
    fixture(); g_stub.lib = {saved(1), saved(2)};
    { Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_cable(kSaved2), true); }
    CHECK_EQ(g_stub.cable_pref == kSaved2, true);
    reboot_with(true);
    {
        Ui ui; boot_ui(ui);
        CHECK_EQ(ui.charger_test_cable_info().choice == kSaved2, true);
        CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(int(bootra::last()), int(bootra::Verdict::Arm));
    }
    g_stub.lib = {saved(1)}; g_stub.lib_removes++;                      // the clone was deleted over the drive meanwhile
    reboot_with(true);
    const int sets = g_stub.cable_pref_sets;
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::kDefaultChoice, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);                     // the default is START's: a boot arms nothing for it
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoIdentity));
    CHECK_EQ(g_stub.cable_pref_set, false);                             // the stale record is gone
    CHECK_EQ(g_stub.cable_pref_clears, 1);
    CHECK_EQ(g_stub.cable_pref_sets, sets);                             // and the default is never written
    reboot_with(true);
    Ui again; boot_ui(again);
    CHECK_EQ(g_stub.cable_pref_clears, 1);                              // a blank volume is stable: no write at the next boot
    CHECK_EQ(g_stub.cable_pref_sets, sets);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
}
// A clone deleted while the K2 runs drops the selection to the default in the list; the record is removed.
static void test_a_clone_deleted_while_running_clears_the_record() {
    fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_cable(kSaved1), true);
    CHECK_EQ(g_stub.cable_pref == kSaved1, true);
    g_stub.lib.clear(); g_stub.lib_removes++;
    cable(ui);                                                          // the Cable step re-reads the library
    CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::kDefaultChoice, true);
    CHECK_EQ(g_stub.cable_pref_set, false);
    CHECK_EQ(g_stub.cable_pref_clears, 1);
}
// A blank volume (nothing ever picked): the dialog, the remote read and START use the EPR spoof, the flash is not written,
// and a self-powered boot arms nothing (the default is START's, only a pick arms at boot).
static void test_blank_volume_defaults_to_the_epr_spoof() {
    fixture(); g_stub.cable_pref_set = false;
    {
        Ui ui; open(ui);
        CHECK_EQ(ui.charger_test_cable_info().choice == ctcable::kDefaultChoice, true);
        CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
        cable(ui);
        CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow);                // the dialog opens on the EPR row
        CHECK_EQ(g_stub.cable_pref_sets + g_stub.cable_pref_clears, 0);  // looking never writes
        CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    }
    reboot_with(true);
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoIdentity));
    CHECK_EQ(g_stub.cable_pref_sets + g_stub.cable_pref_clears, 0);
}
// An explicit Off pick is a record like any other: it survives a reboot as Off and is not replaced by the default.
static void test_an_explicit_off_pick_stays_off() {
    fixture(); g_stub.cable_pref_set = false;
    { Ui ui; open(ui); cable(ui); pick_row(ui, kOffRow); CHECK_EQ(ui.charger_test_cable_info().choice == kOff, true); }
    CHECK_EQ(g_stub.cable_pref_set, true); CHECK_EQ(g_stub.cable_pref == kOff, true);
    CHECK_EQ(g_stub.cable_pref_sets, 1);
    reboot_with(true);
    Ui ui; boot_ui(ui);
    CHECK_EQ(ui.charger_test_cable_info().choice == kOff, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(g_stub.cable_pref_sets, 1); CHECK_EQ(g_stub.cable_pref_clears, 0);
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);                // and it can be changed by hand
    CHECK_EQ(g_stub.cable_pref == kBuiltin, true);
}
// START on a blank volume arms the EPR spoof (the Ra guard, the arming and the Ra mode are the ordinary ones for any identity).
static void test_start_on_a_blank_volume_arms_the_epr_spoof() {
    rd_fixture(); g_stub.cable_pref_set = false;
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(g_stub.cable_pref_sets, 1);   // OK on the Cable step is a pick, also on the default row: it is remembered (reported as a decision)
    start_press(ui, 1400);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    confirm_press_nowait(ui, 1400);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(ctcable::kBuiltinKind), true);
    CHECK_EQ(g_stub.ra_on, true);
}
// Id 3 ("OnePlus 100W (mag)") is retired but stays reserved: a remote or resumed 3 is OnePlus 100W (id 4, the last built-in row),
// never Off and never another cable, and what the run records (card kind, remote reply) is the canonical 4.
static void test_the_retired_builtin_id_reads_as_oneplus_100w() {
    rd_fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    CHECK_EQ(ui.charger_test_set_cable(Choice{Kind::Builtin, 3}), true);
    const Choice four{Kind::Builtin, 4};
    CHECK_EQ(ui.charger_test_cable_info().choice == four, true);
    cable(ui);
    CHECK_EQ(ui.charger_test_set_cable(kSaved1), true);
    CHECK_EQ(ui.charger_test_set_cable(Choice{Kind::Builtin, 3}), true);   // the cursor follows a remote choice
    CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow + 2);
    press(ui, Button::Ok, 1350);                                           // the pick (on the cursor row) goes on to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    start_run(ui, 1400);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_record == emrec::builtin_cable(4), true);
    const auto info = ui.charger_test_cable_info();
    CHECK_EQ(info.run == four, true); CHECK_EQ(info.choice == four, true);
    CHECK_EQ(named(info, "OnePlus 100W"), true);
}
// Flash wear: only a change is written. Repeats, ticks, START's re-selection and a boot that reads the same record cost nothing.
static void test_the_record_is_written_only_when_the_selection_changes() {
    fixture();
    Ui ui; open(ui);
    CHECK_EQ(g_stub.cable_pref_sets, 0);                                // nothing chosen, nothing written
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    cable(ui); pick_row(ui, kBuiltinRow); press(ui, Button::Back); pick_row(ui, kBuiltinRow);
    for (uint32_t t = 1300; t < 4000; t += 100) ui.tick(t);
    CHECK_EQ(g_stub.cable_pref_sets, 1);
    CHECK_EQ(ui.charger_test_set_cable(Choice{Kind::Builtin, 0}), true);   // the old spelling of the same chip
    CHECK_EQ(g_stub.cable_pref_sets, 1);
    CHECK_EQ(ui.charger_test_set_cable(kOff), true);
    CHECK_EQ(g_stub.cable_pref_sets, 2);
    CHECK_EQ(g_stub.cable_pref == kOff, true);
}
// A refused flash write does not lose the choice for this boot and is tried again with the next selection.
static void test_a_failed_write_is_retried() {
    fixture();
    Ui ui; open(ui);
    g_stub.cable_pref_ok = false;
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    CHECK_EQ(g_stub.cable_pref == kOff, true);
    g_stub.cable_pref_ok = true;
    CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);                // the same choice again: the record is still stale
    CHECK_EQ(g_stub.cable_pref == kBuiltin, true);
}
// Off is remembered too: after "no identity" the next boot arms nothing.
static void test_off_is_remembered() {
    fixture();
    { Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true); CHECK_EQ(ui.charger_test_set_cable(kOff), true); }
    reboot_with(true);
    Ui ui; boot_ui(ui);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoIdentity));
}
static void test_resume_unattended_goes_on_by_itself() {
    interrupted_run(true);
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(ui.charger_test_busy(), false);
    ui.tick(1100);                                    // VBUS is steady for less than the gate: not yet
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    uint32_t now = 1200;
    for (; now <= 2400 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage); // no button was pressed
    CHECK_EQ(ui.charger_test_busy(), true);
    CHECK_EQ(g_stub.journal_resumes, 1);
    for (; now <= 600000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(int(ui.charger_test_results().resumed()), 1);
    CHECK_EQ(g_stub.journal_finishes, 1);
}
// A self-powered K2 that still runs when VBUS goes (a reset normally ends it first) does not finalize the run: the journal stays
// open, the page waits on the resume stage and goes on by itself once VBUS is steady again (hardware 2026-11-06: the finalized
// journal was "stopped (source lost)" with every row skipped).
static void test_self_powered_vbus_loss_keeps_the_journal_open() {
    fixture(); g_stub.self_powered = true;
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    CHECK_EQ(ui.charger_test_busy(), true);
    const auto count_pass = [&] {
        unsigned n = 0;
        for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;   // decided (top-down: the stub holds 5 V, the high rows fail first)
        return n;
    };
    uint32_t now = 1500;
    for (; now <= 60000 && count_pass() < 1; now += 100) ui.tick(now);
    CHECK_EQ(count_pass() >= 1, true);
    g_stub.journal_finishes = 0;
    g_stub.pm.vbus_mv = 0;
    for (const uint32_t end = now + 1000; now < end; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Stopped));
    CHECK_EQ(ui.charger_test_results().suspended(), true);
    CHECK_EQ(g_stub.journal_finishes, 0);
    bool pending = false;
    for (const auto& s : ui.charger_test_results().steps()) {
        CHECK_EQ(int(s.reason) != int(ct::Reason::Lost), true);   // nothing is skipped or failed as "source lost"
        pending |= s.verdict == ct::Verdict::Pending;
    }
    CHECK_EQ(pending, true);
    g_stub.pm.vbus_mv = 5000;
    const int resumes = g_stub.journal_resumes;
    for (const uint32_t end = now + 3000; now < end && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(g_stub.journal_resumes, resumes + 1);
    for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(g_stub.journal_finishes, 1);
}
// User decision 2026-11-06: a reboot always resumes by itself, in every power mode (HID-powered too); no OK press anywhere.
// A K2 with no host that booted on a charger's A port (power_source::bus_powered(), no pinned Rd) runs like a self-powered one: the
// run journals itself "self-powered", never detaches (an always-on source would only stall the Detach wait), and a VBUS loss
// suspends it until VBUS is steady again.
static void test_bus_powered_run_behaves_like_self_powered() {
    fixture(); g_stub.bus_powered = true;                 // g_stub.self_powered stays false: no boot key, no pinned Rd
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    CHECK_EQ(ui.charger_test_busy(), true);
    settle_arm(ui, 1400);
    CHECK_EQ(g_stub.journal_meta.unattended, true);
    const auto count_pass = [&] {
        unsigned n = 0;
        for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;   // decided (top-down: the stub holds 5 V, the high rows fail first)
        return n;
    };
    uint32_t now = 1500;
    for (; now <= 60000 && count_pass() < 1; now += 100) ui.tick(now);
    CHECK_EQ(count_pass() >= 1, true);
    CHECK_EQ(g_stub.unit_detaches, 0);                    // no detach, so no Detach wait to stall on
    CHECK_EQ(g_stub.unit_holds >= 1, true);
    g_stub.pm.vbus_mv = 0;
    for (const uint32_t end = now + 1000; now < end; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(ui.charger_test_results().suspended(), true);
    g_stub.pm.vbus_mv = 5000;
    g_stub.supply_decided = false;                        // (a reboot into a still-undecided supply waits for the decision)
    for (const uint32_t end = now + 2000; now < end; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    g_stub.supply_decided = true;
    for (const uint32_t end = now + 3000; now < end && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(g_stub.unit_detaches, 0);
}
static void test_resume_after_reboot_never_waits_for_ok() {
    interrupted_run(false);
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(g_stub.journal_resumes, 0);              // not before VBUS has been steady for the gate
    uint32_t now = 1100;
    for (; now <= 2600 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage); // no button was pressed
    CHECK_EQ(g_stub.journal_resumes, 1);
    for (; now <= 600000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
}
// VBUS that never comes back: the Resume stage keeps waiting (no timeout, no OK).
static void test_resume_waits_for_vbus_without_giving_up() {
    interrupted_run(false);
    g_stub.pm.vbus_mv = 0;
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    for (uint32_t now = 1100; now <= 30000; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(g_stub.journal_resumes, 0);
    g_stub.pm.vbus_mv = 5000;
    uint32_t now = 30100;
    for (; now <= 32000 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(g_stub.journal_resumes, 1);
}
static void test_resume_cancelled_by_back_at_boot() {
    interrupted_run(true);
    g_stub.rd_inherited = true;
    Ui ui; ui.init(0, true); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage() == kResumeStage, false);
    for (uint32_t now = 1100; now <= 5000; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.journal_resumes, 0);
    CHECK_EQ(g_stub.journal_finishes, 1);             // the run is closed: the next boot does not find it again
    CHECK_EQ(g_stub.rd_releases, 1);                  // an inherited Rd has no run to serve
    CHECK_EQ(g_stub.requests, 0);
}
// A run that ends is closed in the journal BEFORE its rows are skipped: a reset (or a flash that gives up) in the middle of the
// skip rows must not leave a stopped run that the next boot resumes (hardware 2026-11-07: an old run on the Resume stage).
// The skip rows are not journaled at all any more (nothing drains, a closed run is never resumed): nothing is written after Finish.
static void test_a_final_stop_closes_the_journal_before_it_skips() {
    fixture();
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t now = 1500;
    for (; now <= 20000 && g_stub.requests < 1; now += 100) ui.tick(now);
    CHECK_EQ(g_stub.requests, 1);
    CHECK_EQ(g_stub.journal_finishes, 0);             // a run in progress stays open
    // Rows already skipped when the stop comes (the plan's own) may be written ahead of Finish: they were decided before it.
    std::vector<bool> decided;
    for (const auto& s : ui.charger_test_results().steps()) decided.push_back(s.verdict == ct::Verdict::Skip);
    g_stub.journal_skips = g_stub.journal_skips_before_finish = 0;
    g_stub.journal_skip_rows_before_finish.clear();
    press(ui, Button::Back, now);
    for (const uint32_t end = now + 20000; now <= end && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    for (const size_t row : g_stub.journal_skip_rows_before_finish)   // a skip row journaled ahead of Finish was decided before it
        CHECK_EQ(row < decided.size() && decided[row], true);
    CHECK_EQ(g_stub.journal_finishes >= 1, true);
    CHECK_EQ(g_stub.journal_log.back(), 'N');         // Finish is the last journal write: no skip row, pump or drain after it
    CHECK_EQ(g_stub.journal_skips, g_stub.journal_skips_before_finish);   // no Skip row went to the journal after Finish
}
// Quiet rows (hardware 2026-11-08: a UFCS Accept was read 65 ms late behind a 100-200 ms redraw, and a littlefs write masks
// interrupts for a whole erase): while a row is in flight the page writes no journal record and paints no list or footer; the
// records wait for the gap behind the row and none is lost.
static Ui* g_quiet_ui = nullptr;
static bool quiet_probe() { return g_quiet_ui->charger_test_results().quiet(); }
static void test_a_row_in_flight_writes_no_journal_record() {
    fixture();
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    g_quiet_ui = &ui; g_stub.journal_quiet = quiet_probe;
    uint32_t now = 1500; unsigned quiet_ticks = 0;
    for (; now <= 200000 && ui.charger_test_busy(); now += 100) { ui.tick(now); quiet_ticks += ui.charger_test_results().quiet(); }
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(quiet_ticks > 0, true);                  // the probe saw rows in flight
    CHECK_EQ(g_stub.journal_writes > 0, true);        // the verdicts were journaled...
    CHECK_EQ(g_stub.journal_writes_quiet, 0);         // ...every one of them outside a row
    // Nothing was lost to the deferral: the journal holds the verdict of every row the run decided.
    const auto steps = ui.charger_test_results().steps();
    for (size_t i = 0; i < steps.size(); ++i)
        if (steps[i].verdict != ct::Verdict::Pending)
            CHECK_EQ(i < g_stub.journal_verdicts.size() ? int(g_stub.journal_verdicts[i]) : -1, int(steps[i].verdict));
    g_stub.journal_quiet = nullptr; g_quiet_ui = nullptr;
}
// ---- the run's plan on flash (hardware 2026-11-08: a synchronous 57-row begin held the main loop 1.5-2.2 s with the charger's
// contract up; no GoodCRC, then a hard reset from the source). The plan goes in one file per tick, the Runner waits for it in
// Baseline, and not one flash program lies between Rd going on and the first contract. -----------------------------------
static void two_pdo_charger() {
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
}
static void test_the_plan_is_written_in_pieces_before_the_first_request() {
    fixture(); two_pdo_charger();
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300);
    CHECK_EQ(g_stub.bb_make_rooms, 0);                // (the confirmation is not a START)
    press(ui, Button::Ok, 1400);
    CHECK_EQ(g_stub.bb_make_rooms, 1);                // the journal's blocks are made free before Rd goes on
    CHECK_EQ(g_stub.journal_arms, 1);
    CHECK_EQ(g_stub.journal_pumps, 0);                // START itself writes nothing: no flash program in the press
    CHECK_EQ(g_stub.journal_writes, 0);
    uint32_t now = 1500; unsigned ticks = 0, worst = 0;
    for (; now <= 60000 && g_stub.journal_phase_writes < 1; now += 100, ++ticks) {
        const int before = g_stub.journal_writes;
        ui.tick(now);
        worst = std::max<unsigned>(worst, unsigned(g_stub.journal_writes - before));
        if (g_stub.journal_arm_open) CHECK_EQ(g_stub.requests, 0);   // the Runner waits for the header: no request yet
    }
    CHECK_EQ(g_stub.journal_phase_writes >= 1, true);
    CHECK_EQ(worst <= 1u, true);                      // one file per tick, never a batch
    CHECK_EQ(g_stub.journal_pumps > 2, true);         // the plan really went in pieces
    CHECK_EQ(g_stub.journal_writes_in_arm, 0);        // no row, phase or meta record between arm_start and the header
    const auto h = g_stub.journal_log.find('H'), m = g_stub.journal_log.find('M'), f = g_stub.journal_log.find('F');
    CHECK_EQ(h != std::string::npos && m != std::string::npos && f != std::string::npos, true);
    CHECK_EQ(h < m, true);                            // the header is the last file of the plan, the meta follows it
    CHECK_EQ(m < f, true);                            // and the first phase marker (ahead of the first request) follows both
    CHECK_EQ(g_stub.journal_log.compare(0, 1, "A"), 0);
    CHECK_EQ(g_stub.bb_holds > 0, true);              // the Black Box was held for the whole arm
}
// A charger that never leaves the link quiet does not keep the plan off the flash for ever: after kArmWaitMs the pieces go in
// anyway (still one per tick), and until then not one flash program happens on a busy link.
static void test_a_busy_link_defers_the_plan_but_not_for_ever() {
    fixture(); two_pdo_charger();
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    const std::array<uint8_t, 4> body{};
    const auto busy = [&] { pdtrace::rx(0xE0, 0x0001, body); };
    uint32_t now = 1500;
    for (; now < 1400 + 4800; now += 20) { busy(); ui.tick(now); }
    CHECK_EQ(g_stub.journal_pumps, 0);                // PD messages every 20 ms: no flash write
    CHECK_EQ(g_stub.requests, 0);                     // and the Runner still waits in Baseline
    for (; now < 1400 + 9000 && g_stub.journal_arm_open; now += 20) { busy(); ui.tick(now); }
    CHECK_EQ(g_stub.journal_arm_open, false);         // the fall-through wrote it all
    CHECK_EQ(g_stub.journal_pumps > 0, true);
    CHECK_EQ(g_stub.journal_writes_in_arm, 0);
}
// The source lists one more PDO right after the plan went to flash (inside the Baseline: the list re-sent after our
// Sink_Capabilities_Extended): the rows are planned again. The new plan goes in as a polled job too (one file per tick, only the
// rows that changed), and it is complete before the phase marker of the first request that relies on it.
static void test_a_replan_is_polled_and_done_before_the_first_phase_marker() {
    fixture(); two_pdo_charger();
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    g_stub.replan_pumps = 6;
    uint32_t now = 1500;
    for (; now <= 60000 && g_stub.journal_arm_open; now += 100) ui.tick(now);
    CHECK_EQ(g_stub.journal_arm_open, false);
    g_stub.caps.num_pdos = 4; g_stub.caps.pdos[3] = pd::decode_pdo((300u << 10) | 300);   // a new offer: the plan follows the list
    unsigned worst = 0;
    for (; now <= 60000 && g_stub.journal_phase_writes < 1; now += 100) {
        const int before = g_stub.journal_writes;
        ui.tick(now);
        worst = std::max<unsigned>(worst, unsigned(g_stub.journal_writes - before));
    }
    CHECK_EQ(g_stub.journal_replan_starts >= 1, true);
    CHECK_EQ(worst <= 1u, true);
    CHECK_EQ(g_stub.journal_job || g_stub.journal_plan_dirty, false);   // finished in the gaps (nothing drains), never half-written
    CHECK_EQ(g_stub.journal_replans, g_stub.journal_replan_starts - g_stub.journal_cancels);
    const auto r = g_stub.journal_log.rfind('R'), f = g_stub.journal_log.find('F');
    CHECK_EQ(r != std::string::npos && f != std::string::npos && r < f, true);   // the new plan precedes the first phase marker
}
// START and BACK before the plan is on flash: the header was never written, so there is no run to resume and nothing to close.
static void test_a_cancel_during_the_plan_leaves_no_run() {
    fixture(); two_pdo_charger();
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    ui.tick(1500); ui.tick(1600);
    CHECK_EQ(g_stub.journal_arm_open, true);
    CHECK_EQ(g_stub.journal_pumps > 0, true);
    press(ui, Button::Back, 1700);
    uint32_t now = 1800;
    for (; now <= 20000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.journal_cancels, 1);
    CHECK_EQ(g_stub.journal_job, false);
    CHECK_EQ(g_stub.journal_arm_open, false);
    CHECK_EQ(g_stub.journal_metas, 0);
    CHECK_EQ(g_stub.journal_finishes, 0);             // no header on flash: nothing for the next boot to resume or close
    const int pumps = g_stub.journal_pumps;
    for (int i = 0; i < 20; ++i) ui.tick(now += 100);
    CHECK_EQ(g_stub.journal_pumps, pumps);            // the job is gone, nothing keeps writing
}
// ---- the Running page in flight: a bounded paint budget, the grid follows the step, a manual scroll holds ----
// An all-tests run (many rows: the verdict grid does not fit its window), started the way the user does and ticked at 100 ms.
static void start_all_run(Ui& ui) {
    fixture();
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
}
constexpr int kGridTop = 94, kGridBottom = kFooterY, kGridCellsX = 55;   // the verdict grid's rows, and where its cells start
// The Running cell's outline: a kRunFrame px kFocus rim along the cell's edge. (kFocus is also the amps cyan, so a pixel is not
// enough: text never makes a solid corner whose top and left edges both run 10 px, 2 px deep.) The top-left corners of every
// such rim in the grid window, in reading order.
static std::vector<std::pair<int, int>> running_dots(int x0 = kGridCellsX, int y0 = kGridTop, int y1 = kGridBottom) {
    std::vector<std::pair<int, int>> out;
    constexpr int t = k2::kit::kRunFrame, run = 10;
    auto on = [](int x, int y) { return x >= 0 && y >= 0 && PANEL[y * LCD_WIDTH + x] == kFocus; };
    for (int y = y0; y + run <= y1; ++y)
        for (int x = x0; x + run <= 224; ++x) {
            if (on(x - 1, y) || on(x, y - 1)) continue;   // a corner, not the middle of an edge
            bool solid = true;
            for (int j = 0; j < t && solid; ++j)
                for (int i = 0; i < run && solid; ++i) solid = on(x + i, y + j) && on(x + j, y + i);
            if (solid) out.emplace_back(x, y);
        }
    return out;
}
// Is a Running cell (the one with the outline) on screen in the grid window?
static bool running_cell_visible() { return !running_dots().empty(); }
// The running cell's rows in the grid: the rim starts on the cell's top edge, the cell is a verdict cell high (25 px at most).
// False = not on screen.
static bool running_cell_rows(int& top, int& bottom) {
    const auto dots = running_dots();
    if (dots.empty()) return false;
    top = dots.front().second; bottom = top + 25;
    return true;
}
// The Running bar's filled pixels (kFocus on its middle line, x6..217): how far the run has come.
static unsigned run_bar_px() {
    unsigned n = 0;
    for (int x = 6; x < 202; ++x) n += PANEL[25 * LCD_WIDTH + x] == kFocus;
    return n;
}
// How many pixels above `y` in the grid wear the Pending ink (a chip that has not run yet). The current brackets are drawn in
// the same ink: their 1 px lines (12 px or longer) and the 3 px ticks above the line ends are not a chip's, so they do not count.
static unsigned pending_ink_above(int y) {
    unsigned n = 0;
    auto faint = [](int x, int yy) { return x >= 0 && x < LCD_WIDTH && yy < LCD_HEIGHT && PANEL[yy * LCD_WIDTH + x] == kFaint; };
    auto in_line = [&](int x, int yy) {
        int a = x, b = x;
        while (faint(a - 1, yy)) --a;
        while (faint(b + 1, yy)) ++b;
        return faint(x, yy) && b - a >= 11;
    };
    for (int yy = kGridTop; yy < y && yy < kGridBottom; ++yy)
        for (int x = kGridCellsX; x < 224; ++x) {
            if (!faint(x, yy) || in_line(x, yy)) continue;
            bool tick = false;
            for (int k = 1; k <= 3 && faint(x, yy + k - 1) && !tick; ++k) tick = in_line(x, yy + k);
            n += !tick;
        }
    return n;
}
static std::string_view handshake_words(int proto);   // below: the footer line a handshake row announces
// In flight a tick paints at most the budget: kFlightPx for the items together, kFlightRowPx for one grid row on an otherwise
// idle tick. Between steps (no step in flight) there is no cap. The one exception is the tick that starts a handshake row: it
// also paints the footer line naming the window (kAnnouncePx), which is not counted here.
static void test_a_row_in_flight_paints_inside_the_budget() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; unsigned flight_ticks = 0, max_px = 0, settled_ticks = 0, max_settled_px = 0;
    unsigned prev_rev = ~0u, prev_idx = ~0u, same = 0;
    for (; now <= 60000 && ui.charger_test_busy(); now += 100) {
        const unsigned px0 = g_lcd_pixels, starts0 = static_cast<unsigned>(g_stub.fc_starts);
        ui.tick(now);
        // the tick that starts a handshake row also paints its footer line (ChargerTestPage::kAnnouncePx), once
        const unsigned total = g_lcd_pixels - px0;
        const unsigned announce = static_cast<unsigned>(g_stub.fc_starts) != starts0 && !handshake_words(g_stub.fc_proto).empty() ? ChargerTestPage::kAnnouncePx : 0;
        const unsigned px = total > announce ? total - announce : 0;
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage || !r.quiet()) { prev_rev = ~0u; same = 0; continue; }
        ++flight_ticks;
        max_px = std::max(max_px, px);
        same = (r.revision() == prev_rev && r.index() == prev_idx) ? same + 1 : 0;
        prev_rev = r.revision(); prev_idx = static_cast<unsigned>(r.index());
        if (same < 20) continue;
        // Twenty ticks into a step the layout and deferred rows have settled; only the hero and clock remain volatile.
        ++settled_ticks; max_settled_px = std::max(max_settled_px, px);
    }
    CHECK_EQ(flight_ticks > 20, true); CHECK_EQ(settled_ticks > 10, true);
    CHECK_EQ(max_px <= static_cast<unsigned>(ChargerTestPage::kFlightRowPx), true);   // a tick never exceeds the tallest row
    CHECK_EQ(max_settled_px <= static_cast<unsigned>(ChargerTestPage::kFlightPx), true);
    std::printf("[flight] ticks=%u max_px=%u settled=%u max_settled_px=%u\n",
                flight_ticks, max_px, settled_ticks, max_settled_px);
}
// The idle hook does not serve an engine window (HS pulses, VOOC slices, UFCS / VIVO frames): there the page paints nothing.
static void test_a_row_in_an_engine_window_paints_nothing() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; unsigned blocked = 0, free_px_ticks = 0;
    for (; now <= 40000 && ui.charger_test_busy(); now += 100) {
        g_stub.fc_critical = (now / 1000) % 2 == 0;   // every other second an engine window is open
        const unsigned px0 = g_lcd_pixels;
        const auto starts0 = g_stub.fc_starts;
        ui.tick(now);
        if (ui.charger_test_stage() != kRunningStage || !ui.charger_test_results().quiet()) continue;
        // The tick that starts a handshake row paints its footer line (kAnnouncePx) before the engine starts: the forced window
        // here opens ahead of the start, which a real engine window never does.
        const unsigned announce = g_stub.fc_starts != starts0 && !handshake_words(g_stub.fc_proto).empty() ? ChargerTestPage::kAnnouncePx : 0;
        if (g_stub.fc_critical) {
            ++blocked;
            const unsigned toast_clear = announce ? 0u : 208u * static_cast<unsigned>(kToastH);
            CHECK_EQ(g_lcd_pixels - px0 <= announce + toast_clear, true);
        }
        else free_px_ticks += g_lcd_pixels != px0;
    }
    g_stub.fc_critical = false;
    CHECK_EQ(blocked > 5, true); CHECK_EQ(free_px_ticks > 0, true);   // the window blocked paints, the rest of the step did not
}
// Every "[CT] gap row=N ... blocked=Mms" line the log holds, by row (the log is emptied, so each tick's lines are read once).
struct GapLine { bool seen = false, has_blocked = false; unsigned blocked = 0; };
static void harvest_gap_lines(std::array<GapLine, 128>& rows) {
    const std::string& log = g_stub.log_text;
    for (size_t at = log.find("[CT] gap row="); at != std::string::npos; at = log.find("[CT] gap row=", at + 1)) {
        const size_t eol = log.find('\n', at);
        const std::string line = log.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        unsigned row = 0;
        if (std::sscanf(line.c_str(), "[CT] gap row=%u", &row) != 1 || row >= rows.size()) continue;
        rows[row].seen = true;
        const size_t b = line.find("blocked=");
        if (b != std::string::npos && std::sscanf(line.c_str() + b, "blocked=%ums", &rows[row].blocked) == 1) rows[row].has_blocked = true;
    }
    g_stub.log_text.clear();
}
// The page wanted to paint and could not: that continuous time is named per row on the row's `[CT] gap` line, so a freeze the
// operator sees can be read back from the log (hardware 2026-11-11: QC2 1.4 s, QC3 3.1 s, nothing measured it).
static void test_the_gap_line_names_the_blocked_window() {
    Ui ui; start_all_run(ui);
    std::array<GapLine, 128> rows{};
    g_stub.log_text.clear();
    size_t target = ~size_t(0), prev_idx = ~size_t(0); uint32_t first = 0, last_blocked = 0; bool open = false; unsigned expect = 0;
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        const auto& r = ui.charger_test_results();
        const bool quiet = ui.charger_test_stage() == kRunningStage && r.quiet();
        if (quiet && r.index() != prev_idx) { prev_idx = r.index(); if (target == ~size_t(0) && prev_idx >= 2) { target = prev_idx; first = now + 200; } }
        // Row `target`: an engine window from 200 ms into the row for 600 ms (it may be cut short by the row's own end).
        g_stub.fc_critical = quiet && r.index() == target && now >= first && now < first + 600;
        const bool was_open = open;
        open = g_stub.fc_critical;
        if (open && !was_open) expect = 0;
        if (open) { expect = now + 100 - first; last_blocked = now; }
        ui.tick(now);
        harvest_gap_lines(rows);
    }
    g_stub.fc_critical = false;
    CHECK_EQ(target != ~size_t(0), true);
    unsigned with_field = 0, seen = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (!rows[i].seen) continue;
        ++seen; with_field += rows[i].has_blocked;
        if (i != target) CHECK_EQ(rows[i].blocked, 0u);   // a row that never wanted to paint in vain reads 0
    }
    CHECK_EQ(seen > 5, true);
    CHECK_EQ(with_field, seen);   // every gap line carries the field
    CHECK_EQ(rows[target].has_blocked, true);
    // The window the page saw: from its first blocked tick to the first free one (tick-sized slack either way).
    CHECK_EQ(rows[target].blocked + 100 >= expect, true);
    CHECK_EQ(rows[target].blocked <= expect + 100, true);
    CHECK_EQ(rows[target].blocked >= 300, true);
    (void)last_blocked;
}
// A row that began and then ended as a skip (the QC2 Class B 20 V wait: "Class A") used to leave no `[CT] gap` line at all, so its
// blocked time was lost. It is logged like any other row that ran.
static void test_a_started_row_that_ends_as_a_skip_still_logs_its_gap() {
    Ui ui; start_all_run(ui);
    std::array<GapLine, 128> rows{};
    g_stub.log_text.clear();
    size_t skipped = ~size_t(0);
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        const auto& r = ui.charger_test_results();
        const bool quiet = ui.charger_test_stage() == kRunningStage && r.quiet();
        const bool b20 = quiet && r.index() < r.steps().size() && r.steps()[r.index()].algorithm == ct::Algorithm::Qc2ClassB;
        if (b20) { g_stub.fc_state = static_cast<int>(k2::fc::State::Failed); skipped = r.index(); }   // the charger stops following
        g_stub.fc_critical = b20;                                                                          // and the page sat blocked
        ui.tick(now);
        harvest_gap_lines(rows);
    }
    g_stub.fc_critical = false;
    CHECK_EQ(skipped != ~size_t(0), true);
    CHECK_EQ(ui.charger_test_results().steps()[skipped].verdict, ct::Verdict::Skip);
    CHECK_EQ(rows[skipped].seen, true);                 // the skipped row that ran has its gap line
    CHECK_EQ(rows[skipped].has_blocked, true);
}
// A screen that stops for the length of a handshake says why first: one Info footer line, painted before the engine starts (never
// inside the window it names), on screen for the whole window, and gone once the window ends.
static std::string_view handshake_words(int proto) {
    switch (proto) {
    case int(k2::fc::Proto::Fcp): return "FCP: handshake";
    case int(k2::fc::Proto::Scp): return "SCP: handshake";
    case int(k2::fc::Proto::Vooc): case int(k2::fc::Proto::SuperVooc): case int(k2::fc::Proto::SuperVooc65): return "VOOC: listening";
    case int(k2::fc::Proto::Ufcs): return "UFCS: handshake";
    case int(k2::fc::Proto::Vivo): return "VIVO: handshake";
    default: return {};
    }
}
// footer_says draws the words onto the panel as it compares, so a second look would find its own ink: look without touching.
static bool footer_shows(std::string_view text, uint16_t color) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> keep;
    std::copy_n(g_panel, keep.size(), keep.begin());
    const bool shown = footer_says(text, color);
    std::copy_n(keep.begin(), keep.size(), g_panel);
    return shown;
}
static unsigned g_start_calls, g_start_with_words;   // engine starts of a handshake protocol, and how many found the words on screen
static void count_words_at_start(int proto) {
    const auto w = handshake_words(proto);
    if (w.empty()) return;
    ++g_start_calls;
    g_start_with_words += footer_shows(w);
}
static void test_a_handshake_window_says_why_the_screen_is_still() {
    Ui ui; start_all_run(ui);
    g_start_calls = g_start_with_words = 0;
    g_stub.fc_open_on_start = true; g_stub.fc_start_hook = count_words_at_start;
    bool seen[11] = {};
    std::string_view words; size_t row = ~size_t(0); uint32_t opened = 0, closed = 0;
    unsigned in_window = 0, with_words = 0, after = 0, after_gone = 0, windows = 0;
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        const auto starts0 = g_stub.fc_starts;
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        const bool quiet = ui.charger_test_stage() == kRunningStage && r.quiet();
        if (g_stub.fc_starts != starts0 && g_stub.fc_critical) {
            opened = now; closed = 0; row = r.index(); words = handshake_words(g_stub.fc_proto); seen[g_stub.fc_proto] = true; ++windows;
        }
        if (g_stub.fc_critical && quiet && !words.empty()) {
            if (now - opened >= 800) { g_stub.fc_critical = false; closed = now; }   // the window ends
            else { ++in_window; with_words += footer_shows(words); }
        } else if (closed && quiet && r.index() == row && !words.empty() && now - closed <= 500 && now > closed) {
            ++after; after_gone += !footer_shows(words);   // the window is over: the footer is the step's own again
        }
    }
    g_stub.fc_critical = false; g_stub.fc_open_on_start = false; g_stub.fc_start_hook = nullptr;
    CHECK_EQ(seen[int(k2::fc::Proto::Fcp)], true); CHECK_EQ(seen[int(k2::fc::Proto::Scp)], true);
    CHECK_EQ(seen[int(k2::fc::Proto::Qc2)], false); CHECK_EQ(seen[int(k2::fc::Proto::Qc3)], false);   // the hook serves those: no window, no words
    CHECK_EQ(windows > 2, true); CHECK_EQ(in_window > 5, true);
    CHECK_EQ(with_words, in_window);                  // every blocked tick shows the words
    CHECK_EQ(g_start_calls > 2, true);
    CHECK_EQ(g_start_with_words, g_start_calls);      // painted before the engine started, not from inside the window
    CHECK_EQ(after > 0, true); CHECK_EQ(after_gone > 0, true);
}
// The QC2 / QC3 / QC3+ handshake is served from the LCD idle hook (fctrig::service_hs), so its window never stops the page: the
// clock (and the hero, whenever the voltage moves) keeps painting through the first seconds of every QC step. Hardware 2026-11-11:
// the screen froze for 1.4 s (QC2) to 3.1 s (QC3) because the page paid nothing while the engine held the pulse timing.
static void test_the_running_screen_keeps_updating_through_a_qc_handshake() {
    Ui ui; start_all_run(ui);
    g_stub.fc_open_on_start = true;
    bool in_qc = false; uint32_t began = 0; unsigned windows = 0, buckets = 0, dead_buckets = 0, blocked_ticks = 0, bucket_px = 0;
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        const auto starts0 = g_stub.fc_starts;
        const unsigned px0 = g_lcd_pixels;
        g_stub.fc_critical = false;   // a window a blocking protocol (FCP, SCP, VOOC...) opened is not this test's business
        ui.tick(now);
        const auto proto = static_cast<k2::fc::Proto>(g_stub.fc_proto);
        const bool qc = proto == k2::fc::Proto::Qc2 || proto == k2::fc::Proto::Qc3 || proto == k2::fc::Proto::Qc3Plus;
        if (g_stub.fc_starts != starts0) { in_qc = qc; began = now; bucket_px = 0; windows += qc; }
        if (!in_qc) continue;
        if (ui.charger_test_stage() != kRunningStage || !ui.charger_test_results().quiet()) { in_qc = false; continue; }
        blocked_ticks += g_stub.fc_critical;
        bucket_px += g_lcd_pixels - px0;
        const uint32_t into = now - began;
        if (into >= 1000 && into % 1000 == 0 && into <= 3000) { ++buckets; dead_buckets += bucket_px == 0; bucket_px = 0; }
    }
    g_stub.fc_critical = false; g_stub.fc_open_on_start = false;
    CHECK_EQ(windows > 2, true); CHECK_EQ(buckets > 2, true);
    CHECK_EQ(blocked_ticks, 0u);      // the engine reports no window the page must wait out
    CHECK_EQ(dead_buckets, 0u);       // and every second of the handshake the screen changed
}
// The grid follows the step under test: the Running cell is on screen a few ticks after every step starts, with no scrolling.
// A Light run plans most of its rows away (advertised PD items are skips from the start): its bar must start at 0 and then only fill
// with the rows that ran, never jump in on the plan-time skips.
static void test_a_light_run_bar_starts_empty_and_only_fills() {
    fixture();
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Light)), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    unsigned prev = 0, ticks_running = 0; bool first = true, grew = false;
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        ui.tick(now);
        if (ui.charger_test_stage() != kRunningStage) continue;
        if (!ui.charger_test_results().quiet()) continue;
        const unsigned px = run_bar_px();
        if (first) { CHECK_EQ(px, 0u); dump_screen("light_start"); first = false; }   // K2_DUMP: the Light run's first screen
        CHECK_EQ(px >= prev, true); grew |= px > prev; prev = px; ++ticks_running;
    }
    CHECK_EQ(ticks_running > 0, true);
    CHECK_EQ(grew, true);
}
static void test_the_running_grid_follows_the_step() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; size_t seen_idx = ~size_t(0); unsigned since = 0, steps_followed = 0, worst_lag = 0, lag = 0, missed = 0;
    bool waiting = false, early_done = false, mid_done = false;
    unsigned pending_above = 0, rows_above = 0;
    for (; now <= 400000 && ui.charger_test_busy(); now += 100) {
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage) continue;
        if (r.index() != seen_idx) { seen_idx = r.index(); since = 0; lag = 0; waiting = true; }
        ++since;
        if (!waiting || !r.quiet()) continue;   // follow what a step in flight shows
        int top = 0, bottom = 0;
        if (running_cell_rows(top, bottom)) {
            waiting = false; ++steps_followed; worst_lag = std::max(worst_lag, lag);
            char name[24]; std::snprintf(name, sizeof name, "flight_step%02zu", r.index());
            dump_screen(name);   // K2_DUMP: the grid on the step under test, for the design review
            // Anchored to the bottom: the row under test ends at or above the window's bottom edge, and the rows above it are
            // finished ones (no Pending chip). Early on the window sits at the top, so the rows below may still be pending.
            CHECK_EQ(bottom <= kGridBottom, true);
            if (pending_ink_above(top)) ++pending_above;
            const unsigned total = static_cast<unsigned>(r.steps().size());
            if (!early_done) { CHECK_EQ(run_bar_px(), 0u); dump_screen("follow_early"); early_done = true; }   // the first executed row: the bar starts empty
            if (!mid_done && r.index() >= total / 2) { dump_screen("follow_mid"); mid_done = true; }
            if (r.index() + 6 >= total) dump_screen("follow_end");   // the last one followed wins
            if (top > kGridTop + 60) ++rows_above;   // the window holds finished rows above the running row
        }
        else if (++lag > 8) { ++missed; waiting = false; }   // a budgeted row at a time: never more than 8 ticks
    }
    // A step whose chip the layout merged into one already decided (the same volts at another current) has no cell of its own to
    // light: its row is on screen all the same. Those are the rare ones.
    for (unsigned k = 0; k < 5; ++k) ui.tick(now += 100);
    for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(now += 20);
    dump_screen("results_end");   // K2_DUMP: the finished run's Results grid, the skipped units struck out
    if (ui.charger_test_stage() == 6) {
        for (int k = 0; k < 5; ++k) {
            move(ui, 3, now += 100); ui.tick(now += 100); ui.tick(now += 100);
            for (int i = 0; i < 60 && ui.painting(); ++i) ui.tick(now += 20);
            char name[32]; std::snprintf(name, sizeof name, "results_scrolled%d", k);
            dump_screen(name);
        }
    }
    CHECK_EQ(steps_followed > 5, true); CHECK_EQ(missed * 4 <= steps_followed, true);
    CHECK_EQ(rows_above > 5, true);                    // after the start the window is full of finished rows above the running one
    CHECK_EQ(pending_above * 10 <= steps_followed, true);   // ... and what is above it is finished (the odd out-of-order row aside)
    std::printf("[flight] grid followed %u steps (%u without a cell of their own), worst lag %u ticks, %u with finished rows above, %u with a pending chip above\n",
                steps_followed, missed, worst_lag, rows_above, pending_above);
}
// The gauge's green window as painted: the first and last x of a kOk pixel in its strip (the needle and the track are other colours).
static std::pair<int, int> gauge_band() {
    int lo = -1, hi = -1;
    for (int x = 0; x < 208; ++x)
        for (int y = 78; y < 78 + k2::kit::kPassBandH; ++y)
            if (PANEL[y * LCD_WIDTH + x] == kOk) { if (lo < 0) lo = x; hi = x; break; }
    return {lo, hi};
}
// The pair the user watches: the gauge's target window and the Running cell's dot. On every step that moves the target, both move
// in the tick the step changes (the dot may only stay where it was when neither the old nor the new cell is on screen), and the
// grid never shows two dots, in a run whose rows all pass and in one whose legacy units all fail.
static void gauge_and_dot_move_together(bool fail_legacy) {
    Ui ui; start_all_run(ui);
    if (fail_legacy) g_stub.fc_start_fail = static_cast<int>(k2::fc::Fail::NoResponse);
    uint32_t now = 1500; size_t prev_idx = ~size_t(0); int prev_mv = -1;
    std::pair<int, int> prev_band{-2, -2}; std::vector<std::pair<int, int>> prev_dots;
    unsigned moves = 0, band_stale = 0, band_unlaid = 0, dot_stale = 0, double_dots = 0, off_screen = 0, ticks = 0;
    for (; now <= 400000 && ui.charger_test_busy(); now += 100) {
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage || r.steps().empty()) { prev_idx = ~size_t(0); continue; }
        const size_t at = std::min(r.index(), r.steps().size() - 1);
        const int mv = static_cast<int>(r.steps()[at].mv);
        const auto band = gauge_band();
        const auto dots = running_dots();
        ++ticks;
        if (dots.size() > 1) ++double_dots;   // the dot is on the running cell only
        if (prev_idx != ~size_t(0) && at != prev_idx && mv != prev_mv) {
            ++moves;
            if (band == prev_band) { if (band.first < 0) ++band_unlaid; else ++band_stale; }
            if (dots == prev_dots) { if (dots.empty()) ++off_screen; else ++dot_stale; }
        }
        prev_idx = at; prev_mv = mv; prev_band = band; prev_dots = dots;
    }
    g_stub.fc_start_fail = 0;
    CHECK_EQ(ticks > 20, true); CHECK_EQ(moves - band_unlaid > 10, true);
    CHECK_EQ(band_stale, 0u);
    CHECK_EQ(dot_stale, 0u);
    CHECK_EQ(double_dots, 0u);
    std::printf("[flight] gauge+dot (%s): %u ticks, %u target changes, %u stale bands, %u stale dots, %u with the cell off screen, %u double dots\n",
                fail_legacy ? "legacy fails" : "all pass", ticks, moves, band_stale, dot_stale, off_screen, double_dots);
}
static void test_the_gauge_band_and_the_dot_move_in_the_same_tick() {
    gauge_and_dot_move_together(false);
    gauge_and_dot_move_together(true);
}
// Hardware 2026-11-21: in a Deep run the QC2 unit's dot sat on its first (5 V) cell while the 9 V and 12 V steps ran. The dot is on
// the cell of the level under test, at every level of the plan (Medium and Deep alike).
static void running_dot_follows_the_qc2_level(ct::Level level) {
    fixture();
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(level)), true);
    review(ui); start_press(ui, 1300); press(ui, Button::Ok, 1400);
    unsigned checked = 0, wrong = 0, none = 0, seen_mv = 0;
    for (uint32_t now = 1500; now <= 400000 && ui.charger_test_busy(); now += 100) {
        if (g_stub.fc_proto == int(k2::fc::Proto::Qc2) && g_stub.fc_state == int(k2::fc::State::Active) && g_stub.fc_target_mv > 0) g_stub.pm.vbus_mv = g_stub.fc_target_mv;   // the charger follows the Request
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage || r.steps().empty() || r.index() >= r.steps().size() || !r.quiet()) continue;
        const ct::Step& st = r.steps()[r.index()];
        if (st.algorithm != A::Qc2 || st.verdict != ct::Verdict::Running) continue;
        for (int i = 0; i < 8 && ui.painting(); ++i) ui.tick(now += 20);   // the budgeted rows settle
        // The QC2 row's chips, ascending: one per distinct level of the Class A algorithm (Class B 20 V is its own row).
        unsigned lv[8], n = 0;
        for (const auto& x : r.steps())
            if (x.algorithm == A::Qc2 && x.mv) {
                bool have = false;
                for (unsigned k = 0; k < n; ++k) have |= lv[k] == x.mv;
                if (!have && n < 8) lv[n++] = x.mv;
            }
        std::sort(lv, lv + n);
        unsigned cell = 0;
        while (cell < n && lv[cell] != st.mv) ++cell;
        if (cell == n) continue;
        const auto dots = running_dots();
        if (dots.empty()) { ++none; continue; }
        const int span = 208 - kGridCellsX;   // the cells run from the label column to the scroll gutter
        const int want_x = kGridCellsX + (span * int(cell) * 2 + int(n)) / (2 * int(n));
        ++checked; seen_mv |= 1u << (st.mv / 1000 - 1);
        if (dots.size() != 1 || dots[0].first != want_x) ++wrong;
    }
    CHECK_EQ(checked > 3, true);
    CHECK_EQ(seen_mv & (1u << 8), 1u << 8);    // 9 V
    CHECK_EQ(seen_mv & (1u << 11), 1u << 11);  // 12 V
    CHECK_EQ(wrong, 0u);
    (void)none;
}
static void test_the_running_dot_follows_the_qc2_level() {
    running_dot_follows_the_qc2_level(ct::Level::Medium);
    running_dot_follows_the_qc2_level(ct::Level::Deep);
}
// A charger that answers no handshake fails every legacy unit at its first step. Those rows stay where the plan put them, so the
// running row still has the finished ones above it (hardware 2026-11-08, FH22: the failed rows fell to a trailing "No reply"
// row, the running SVOOC row sat at the top of the window with only pending rows below it).
static void test_failed_units_stay_above_the_running_row() {
    Ui ui; start_all_run(ui);
    g_stub.fc_start_fail = static_cast<int>(k2::fc::Fail::NoResponse);
    uint32_t now = 1500; unsigned followed = 0, deep = 0, at_top = 0; size_t last_dumped = ~size_t(0);
    for (; now <= 400000 && ui.charger_test_busy(); now += 100) {
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage || !r.quiet() || r.steps().empty()) continue;
        const size_t at = std::min(r.index(), r.steps().size() - 1);
        if (r.steps()[at].algorithm == r.steps()[0].algorithm) continue;   // the plan's first row has nothing above it
        int top = 0, bottom = 0;
        if (!running_cell_rows(top, bottom)) continue;
        ++followed;
        if (r.index() >= 8) ++deep;
        if (top <= kGridTop + 4) ++at_top;   // the running row heads the window: nothing finished above it
        if (r.index() != last_dumped && followed <= 400) { last_dumped = r.index(); char nm[40]; std::snprintf(nm, sizeof nm, "failed_units_step%02zu", r.index()); dump_screen(nm); }
    }
    CHECK_EQ(followed > 10, true); CHECK_EQ(deep > 5, true);
    CHECK_EQ(at_top, 0u);
    std::printf("[flight] failed units: %u steps followed, %u with the running row at the top of the window\n", followed, at_top);
}
// PLUS / MINUS scroll the grid by hand; the follow resumes 5 s later or at the next step, whichever comes first.
static void test_a_manual_scroll_of_the_running_grid_holds_then_resumes() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; bool pressed = false; uint32_t t_press = 0; size_t step_press = 0, last_step = 0;
    unsigned held_ticks = 0, wait = 0, by_step = 0, by_timeout = 0, presses = 0;
    for (; now <= 200000 && ui.charger_test_busy() && !(by_step && by_timeout); now += 100) {
        ui.tick(now);
        const auto& r = ui.charger_test_results();
        if (ui.charger_test_stage() != kRunningStage || !r.quiet()) continue;
        if (!pressed) {
            if (r.index() < 12 || r.index() <= last_step || !running_cell_visible()) continue;   // a step well down the grid, followed on screen
            if (!presses) dump_screen("flight_deep_followed");   // K2_DUMP: far down the list the window still shows the running row
            for (int i = 0; i < 40; ++i) press(ui, Button::Minus, now);    // scroll to the top
            pressed = true; t_press = now; step_press = r.index(); wait = 0; ++presses;
            ui.tick(now += 100);
            if (presses == 1) dump_screen("flight_deep_scrolled");   // K2_DUMP: the user's scroll, held
            CHECK_EQ(running_cell_visible(), false);               // the user's scroll is respected at once
            continue;
        }
        if (r.index() == step_press && now - t_press < 5000) {   // inside the hold: the window stays where the user put it
            CHECK_EQ(running_cell_visible(), false); ++held_ticks;
        } else if (running_cell_visible()) {
            (r.index() == step_press ? by_timeout : by_step)++;   // the 5 s timeout or the next step brought the follow back
            pressed = false; last_step = r.index();
        } else if (++wait > 8) { CHECK_EQ(running_cell_visible(), true); pressed = false; last_step = r.index(); }
    }
    CHECK_EQ(presses > 0, true); CHECK_EQ(held_ticks > 0, true);
    CHECK_EQ(by_step + by_timeout > 0, true);
    std::printf("[flight] manual scroll: %u presses, held %u ticks, resumed %u by the next step, %u by the 5 s timeout\n", presses, held_ticks, by_step, by_timeout);
}
// ---- the resume on a K2 that nothing but the host powers: a USB-C source never turns VBUS on without an Rd ----
// A plain HID-powered boot is passive (no Rd), so the Resume stage presents it the way START does, then resumes once VBUS has
// been steady. (Hardware 2026-11-07, Baseus 70W on the top plug: "Waiting for stable 5V" forever.)
static void hid_interrupted_run(bool unattended) {
    interrupted_run(unattended);
    g_stub.self_powered = false; g_stub.bus_powered = false; g_stub.supply_decided = true;
    g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
}
static void set_lines(int cc1, int cc2) { k2::adc::Reading r{}; r.cc1_mv = cc1; r.cc2_mv = cc2; g_stub.adc = r; }
// One step per 20 ms with the PD engine's service(), until the stage leaves Resume (or `end`).
static uint32_t resume_run_out(Ui& ui, uint32_t now, uint32_t end) {
    for (; now <= end && ui.charger_test_stage() == kResumeStage; now += 20) { fusb302::service(); ui.tick(now); }
    return now;
}
static void test_resume_host_powered_presents_rd() {
    hid_interrupted_run(true);
    static bool rd_at_scan; rd_at_scan = true; g_stub.on_scan = [] { rd_at_scan = g_stub.rd_run; };
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(g_stub.rd_run, true);                    // no Rd on the line, so a charger never turns VBUS on: the resume presents it
    CHECK_EQ(g_stub.scans, 1); CHECK_EQ(rd_at_scan, false);   // the cable's e-marker is read first, as START does on a dead line
    CHECK_EQ(g_stub.journal_resumes, 0);              // not before VBUS has been steady
    resume_run_out(ui, 1020, 8000);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(g_stub.journal_resumes, 1);
    CHECK_EQ(g_stub.rd_run, true);
    CHECK_EQ(g_stub.pm.vbus_mv, 5000);
}
// BACK ends the wait for the charger: results kept, the journal closed, the Rd dropped and never presented again.
static void test_resume_host_powered_back_drops_the_rd() {
    hid_interrupted_run(true);
    g_stub.vbus_on_rd = false;                        // the charger never answers
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    resume_run_out(ui, 1020, 3000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    CHECK_EQ(g_stub.rd_run, true);
    press(ui, Button::Back, 3100);
    CHECK_EQ(ui.charger_test_stage() == kResumeStage, false);
    CHECK_EQ(g_stub.rd_run, false);
    CHECK_EQ(g_stub.journal_finishes, 1);
    for (uint32_t now = 3120; now <= 6000; now += 20) { fusb302::service(); ui.tick(now); }
    CHECK_EQ(g_stub.rd_run, false);
    CHECK_EQ(g_stub.journal_resumes, 0);
}
// Self-powered and bus-powered boots keep today's resume: nothing of ours goes on the line.
static void test_resume_self_and_bus_powered_present_no_rd() {
    interrupted_run(true);                            // self-powered (the Rd is pinned by the boot keys), VBUS up
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0);
        resume_run_out(ui, 1020, 3000);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    interrupted_run(false);
    g_stub.self_powered = false; g_stub.bus_powered = true;   // an A-port charger through an A-to-C cable
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0);
        resume_run_out(ui, 1020, 3000);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    hid_interrupted_run(true);                        // a host-powered K2 whose VBUS is already up (a charger that needs no Rd)
    g_stub.pm.vbus_mv = 5000; g_stub.vbus_on_rd = false;
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0);
    }
}
// The supply is not known yet (the host window is open): no Rd until it is decided; a sink inline never gets one.
static void test_resume_host_powered_refusals() {
    hid_interrupted_run(true);
    g_stub.supply_decided = false;
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        resume_run_out(ui, 1020, 3000);
        CHECK_EQ(g_stub.rd_run_changes, 0);
        g_stub.supply_decided = true;
        resume_run_out(ui, 3020, 3100);
        CHECK_EQ(g_stub.rd_run, true);
    }
    hid_interrupted_run(true);
    g_stub.inl = true; g_stub.link_inline = true;     // a live sink inline: 500 mA flowing on a part-way VBUS, the link latches it
    g_stub.pm.vbus_mv = 3000; g_stub.pm.current_ma = 500;
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        resume_run_out(ui, 1020, 6000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.scans, 0); CHECK_EQ(g_stub.ra_ons, 0);
    }
}
// A journaled identity is armed (Ra on CC1) before the Rd, and only for a charger directly on the plug (the Ra guard reads the
// lines first, K2 passive). Through a cable nothing is armed and no Rd goes on; the resume keeps waiting.
static void test_resume_host_powered_identity_follows_the_guard() {
    const auto setup = [] {
        hid_interrupted_run(true);
        g_stub.journal.meta.cable_kind = static_cast<uint8_t>(kBuiltin.kind); g_stub.journal.meta.cable_seq = kBuiltin.seq;
        g_stub.spoof_arms = g_stub.ra_ons = 0; g_stub.ra_on = g_stub.spoof_on = false;
    };
    setup();
    set_lines(3300, 3300);                            // a receptacle charger: Rp on both lines
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        resume_run_out(ui, 1020, 8000);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.spoof_on, true);
        CHECK_EQ(g_stub.rd_run, true); CHECK_EQ(g_stub.scans, 0);   // the identity answers: no e-marker scan with our VCONN
        CHECK_EQ(ui.charger_test_cable_info().run == kBuiltin, true);
        CHECK_EQ(g_stub.journal_resumes, 1);
    }
    setup();
    set_lines(3300, 0);                               // one Rp: a cable in between
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        resume_run_out(ui, 1020, 8000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run_changes, 0);
        CHECK_EQ(g_stub.journal_resumes, 0);
        set_lines(3300, 3300);                        // replugged straight into the top plug: it goes on by itself
        resume_run_out(ui, 8020, 14000);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.rd_run, true);
    }
}
// The replayed journal (~2.6 KB) is transient: it lives in the scratch arena for the duration of boot_restore() only, and a
// pending resume keeps just what it copied out (the Runner's steps, options, cable). Nothing holds the arena afterwards, so
// emlib / msc / transcap / the Black Box can lease it while the Resume stage waits.
static void test_resume_replay_is_transient_scratch() {
    using namespace k2;
    interrupted_run(false);
    {
        Ui ui; ui.init(0); ui.set_rotation(0, 0);
        CHECK_STR(g_stub.journal_load_owner ? g_stub.journal_load_owner : "(none)", "cjrn.boot");   // load() ran under the boot lease ...
        CHECK_EQ(g_stub.journal_load_in_arena, true);        // ... into the arena itself, not a static
        CHECK_EQ(scratch::held(), false);                    // released again before the page waits
        ui.tick(1000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        {   // the arena is free for its other users while the resume is pending, and they may scribble all over it
            scratch::Lease l("test.other");
            std::memset(scratch::arena(), 0xEE, scratch::kSize);
        }
        for (uint32_t now = 1100; now <= 2600; now += 100) ui.tick(now);   // the auto-resume still has every step it needs
        CHECK_EQ(ui.charger_test_stage() != kResumeStage, true);
        CHECK_EQ(int(ui.charger_test_results().steps().size()) == int(g_stub.journal.count), true);
        CHECK_EQ(g_stub.journal_resumes, 1);
    }
    // BACK held at boot (nothing to resume): the lease is gone too, and the closing finish() ran outside it.
    interrupted_run(true);
    {
        Ui ui; ui.init(0, true);
        CHECK_EQ(scratch::held(), false);
        CHECK_EQ(g_stub.journal_finishes, 1);
    }
}
static void test_no_journal_releases_an_inherited_rd() {
    fixture(); g_stub.rd_inherited = true; g_stub.self_powered = true;
    Ui ui; ui.init(0);
    CHECK_EQ(g_stub.rd_releases, 1);
}
// ---- the Ra guard: a spoof needs the charger directly on the plug (include/k2/pd/ra_guard.hpp) ----
static void dump_screen(const char* name) {   // $K2_DUMP/k2_<name>.panel (240x240 RGB565 LE; tools/panel2png.py)
    k2::trunc::context(name);
    k2::trunc::settled();   // every dumped screen is a settled frame: overlapping text fails the run
    const char* dir = std::getenv("K2_DUMP");
    if (!dir) return;
    char path[256];
    std::snprintf(path, sizeof path, "%s/k2_%s.panel", dir, name);
    if (FILE* f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
}
static void set_cc(int cc1, int cc2) { k2::adc::Reading r{}; r.cc1_mv = cc1; r.cc2_mv = cc2; g_stub.adc = r; }
// From Review: back to the Cable list, pick `row` (which advances to Review), START's two OKs (the lines read cc1 / cc2 all
// along). The guard speaks on the second one: a refusal opens the Refuse stage, otherwise the identity is armed and START sits
// in Waiting (a charger that is not up yet) or the run has begun (VBUS already up). The wait is not run out here.
static void pick_and_start(Ui& ui, int row, uint32_t now) {
    press(ui, Button::Back, now); pick_row(ui, row, now);
    start_press(ui, now + 100); confirm_press_nowait(ui, now + 100);
}
static void pick_builtin(Ui& ui, uint32_t now = 1300) { pick_and_start(ui, kBuiltinRow, now); }
static void nothing_armed() {
    CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.ra_on, false);
    CHECK_EQ(g_stub.rd_run_changes, 0); CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.scans, 0);
}
static void test_ra_guard_refuses_a_cable() {
    for (const auto& lines : {std::pair{3300, 0}, std::pair{0, 3300}, std::pair{3300, 1707}, std::pair{3300, 320}}) {
        // Rp on ONE line: a cable in between. The spoof is refused before anything is armed; the dialog says why.
        rd_fixture(); set_cc(lines.first, lines.second);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); CHECK_EQ(ui.charger_test_cursor(), 0);
        CHECK_EQ(ui.list_state().count, 2);              // Start anyway, Start, skip spoof
        nothing_armed();
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
        ui.tick(1500); settle(ui, 1500);
        check_row_text(0, "Start anyway", kCaution, kFocusFill, -1, 150, kRefuseY);
        check_row_text(1, "Start, skip spoof", kText, kBg, -1, 150, kRefuseY);
        if (lines.first == 3300 && lines.second == 0) dump_screen("ra_guard_refuse");   // K2_DUMP: the screen for the design review
        move(ui, 5, 1600);                               // the cursor stops on the last answer
        CHECK_EQ(ui.charger_test_cursor(), 1);
        const unsigned pref_sets = g_stub.cable_pref_sets;
        press(ui, Button::Ok, 1800);                     // OK on "Start, skip spoof": the plain START, no spoof, no Ra
        CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0);
        CHECK_EQ(g_stub.rd_run, true);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
        CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);   // the stored pick is untouched, not OFF
        CHECK_EQ(g_stub.cable_pref_sets, pref_sets);                     // /cable.cfg was not rewritten
    }
    { // BACK leaves the refusal (there is no Cancel row any more); "Start, skip spoof" starts without the identity
        rd_fixture(); set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage);
        press(ui, Button::Back, 1600);
        CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed();
        CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
        pick_builtin(ui, 1700);
        move(ui, 2, 1800); CHECK_EQ(ui.charger_test_cursor(), 1);   // the cursor stops on the last answer
        press(ui, Button::Ok, 1900);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run, true);
        CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
        CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    }
    { // nothing on either line: no charger waiting, same refusal
        rd_fixture(); set_cc(0, 0);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); nothing_armed();
    }
    { // a saved clone is refused the same way
        rd_fixture(); g_stub.lib = {saved(1)}; set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        pick_and_start(ui, kSavedRow, 1300);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); nothing_armed();
    }
    { // "Start anyway": the identity runs armed against the refused reading, the reading goes on the card
        rd_fixture(); set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); CHECK_EQ(ui.charger_test_cursor(), 0);
        press(ui, Button::Ok, 1800);
        CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_ons, 1);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    }
}
// The Refuse stage's wording: the alert band says "Cable connected", the two lines below say the spoof won't reach
// the charger, and the two options are "Start anyway" and "Start, skip spoof" (BACK returns to Review, there is no
// Cancel row). "Start, skip spoof" runs without the identity but leaves the stored pick (/cable.cfg) alone.
static void test_refuse_stage_wording_and_skip_spoof_keeps_the_pick() {
    rd_fixture(); set_cc(3300, 0);
    Ui ui; open(ui); review(ui);
    pick_builtin(ui);
    CHECK_EQ(ui.charger_test_stage(), kRefuseStage); CHECK_EQ(ui.charger_test_cursor(), 0);
    ui.tick(1500); settle(ui, 1500);
    check_text_at(kMid, kit::kBodyY + (kit::kAlertH - kMid.height) / 2, "Cable connected", kBg, kCaution, 26);   // the alert band's word
    check_text_at(kSmall, kRefuseY - 46, "the spoof won't reach", kText, kBg, 6);   // the two explanation lines
    check_text_at(kSmall, kRefuseY - 26, "the charger.", kText, kBg, 6);
    CHECK_EQ(ui.list_state().count, 2);
    check_row_text(0, "Start anyway", kCaution, kFocusFill, -1, 150, kRefuseY);
    check_row_text(1, "Start, skip spoof", kText, kBg, -1, 150, kRefuseY);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);
    const unsigned pref_sets = g_stub.cable_pref_sets;
    move(ui, 1, 1700);                               // the cursor on "Start, skip spoof"
    press(ui, Button::Ok, 1800);                     // "Start, skip spoof": this run without the identity
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0); CHECK_EQ(g_stub.rd_run, true);
    CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);      // the run carries no identity
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);   // the stored pick is unchanged
    CHECK_EQ(g_stub.cable_pref_sets, pref_sets);                   // /cable.cfg was not rewritten
    CHECK_EQ(g_stub.cable_pref == kBuiltin, true);
    // BACK on a fresh refusal returns to Review, nothing armed, the pick intact
    rd_fixture(); set_cc(3300, 0);
    Ui ui2; open(ui2); review(ui2);
    pick_builtin(ui2, 2000);
    CHECK_EQ(ui2.charger_test_stage(), kRefuseStage);
    press(ui2, Button::Back, 2100);
    CHECK_EQ(ui2.charger_test_stage(), 4); nothing_armed();
    CHECK_EQ(ui2.charger_test_cable_info().choice == kBuiltin, true);
}
// The guard belongs to the second OK: the first one opens the confirmation and refuses nothing a cable could cause; the refusal
// (still before anything is armed) comes with the second, and "Start, skip spoof" then goes on to the Rd and the wait.
static void test_ra_guard_speaks_on_the_second_ok() {
    rd_fixture(); set_cc(3300, 0);
    Ui ui; open(ui); review_with(ui, kBuiltinRow);
    start_press(ui, 1400);
    CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed();
    confirm_press_nowait(ui, 1600);
    CHECK_EQ(ui.charger_test_stage(), kRefuseStage); nothing_armed();
    move(ui, 1, 1650); press(ui, Button::Ok, 1700);      // Start, skip spoof: no confirmation again, Rd and the wait
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
    CHECK_EQ(ui.charger_test_cable_info().choice == kBuiltin, true);   // the stored pick stays
    CHECK_EQ(g_stub.rd_run, true); CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0);
    finish_wait(ui, 1700);                               // the charger comes up on the Rd: the run starts by itself
    CHECK_EQ(ui.charger_test_stage(), kRunningStage); CHECK_EQ(ui.charger_test_busy(), true);
}
static void test_ra_guard_lets_a_direct_charger_through() {
    { // Rp on BOTH lines: the receptacle charger on the top plug, the spoof runs as before
        rd_fixture(); set_cc(3300, 3300);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.ra_on, true); CHECK_EQ(g_stub.rd_run, true);
    }
    { // OFF is never refused, whatever the lines show
        rd_fixture(); set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1400);   // Off is the default choice
        CHECK_EQ(ui.charger_test_stage(), kWaitStage); CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.rd_run, true);
    }
    { // no ADC reading yet: not judged, not blocked
        rd_fixture();
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage); CHECK_EQ(g_stub.ra_on, true);
    }
    { // VBUS already up: a source is attached to something already, the guard does not judge; no Waiting, the run begins
        fixture(); g_stub.pm.vbus_mv = 5000; set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // a PLUS press sags CC2 for a moment: the peak over the window keeps the receptacle verdict
        rd_fixture(); set_cc(3300, 3300);
        Ui ui; open(ui); review(ui);
        for (uint32_t t = 1400; t <= 1800; t += 100) ui.tick(t);   // the window runs from the Cable step on
        set_cc(3300, 1700);                              // the sag
        for (uint32_t t = 1900; t <= 2100; t += 100) ui.tick(t);
        pick_and_start(ui, kBuiltinRow, 2200);           // START with CC2 still down
        CHECK_EQ(ui.charger_test_stage(), kWaitStage); CHECK_EQ(g_stub.ra_on, true);
    }
    { // the window follows a real change: a charger unplugged and a cable plugged in while the dialog stays open
        rd_fixture(); set_cc(3300, 3300);
        Ui ui; open(ui); review(ui);
        for (uint32_t t = 1400; t <= 1800; t += 100) ui.tick(t);
        set_cc(3300, 0);
        for (uint32_t t = 1900; t <= 3000; t += 100) ui.tick(t);   // longer than the window
        pick_and_start(ui, kBuiltinRow, 3100);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); nothing_armed();
    }
    { // a K2 already presenting Rd (a run's own Rd, a pinned boot Rd) is not passive: not judged
        rd_fixture(); set_cc(3300, 0); g_stub.rd_run = true;
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage() == kRefuseStage, false);
    }
    // A PLUS boot (Rd pinned on CC2): PLUS pulls CC2 down, so the lines can never both read as Rp. The guard cannot judge
    // and assumes the direct receptacle: the charger powers the K2 through the top plug, so no "cable on plug" refusal speaks
    // and no warning of any kind: the START goes straight on (test_self_supplied_start, below).
    for (const int vbus : {0, 5000}) {
        for (const auto& lines : {std::pair{3300, 600}, std::pair{3300, 0}, std::pair{0, 3300}}) {
            fixture(); g_stub.pm.vbus_mv = vbus; g_stub.vbus_on_rd = true; g_stub.self_powered = true;
            set_cc(lines.first, lines.second);
            Ui ui; open(ui); review(ui);
            pick_builtin(ui);
            CHECK_EQ(ui.charger_test_stage() == kRefuseStage, false);
            CHECK_EQ(g_stub.spoof_arms, 1);
        }
    }
}
// A K2 that runs on the measured port (PLUS boot: pinned Rd; or bus-powered) is attached to the charger before it can show a
// cable, and a charger that reads the e-marker does so at its attach (docs/CHARGER_QUIRKS.md `cable-look-at-attach`). There is no
// warning for that any more: START with an identity goes straight on, with the identity armed and no Refuse stage. Only the
// topology-based "Cable connected" refusal keeps its own screen. The planned re-attach (below) is a footer note, not a stage.
static void self_supplied_start(Ui& ui, bool plus) {
    fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
    if (plus) g_stub.self_powered = true; else g_stub.bus_powered = true;
    set_cc(3300, plus ? 600 : 3300);   // PLUS sags CC2; a bus-powered K2 on a receptacle charger reads Rp on both
    ui.init(0);
}
static void test_self_supplied_start() {
    for (const bool plus : {true, false}) {
        { // the identity is armed beside the pinned Rd and the START waits for the charger: no stage in between
            Ui ui; self_supplied_start(ui, plus); open(ui); review(ui);
            pick_builtin(ui);
            CHECK_EQ(ui.charger_test_stage(), kWaitStage);
            CHECK_EQ(g_stub.spoof_arms, 1); CHECK_EQ(g_stub.ra_ons, 1);
            CHECK_EQ(ui.charger_test_run_guard() != 0 && (ui.charger_test_run_guard() & 0x80) == 0, true);   // a reading, no "started anyway" bit
        }
        { // the spoof skipped (Off pick): no identity, no Ra, the Rd and the wait as for any plain START
            Ui ui; self_supplied_start(ui, plus); open(ui); review(ui);
            start_press(ui, 1300); confirm_press_nowait(ui, 1400);
            CHECK_EQ(g_stub.spoof_arms, 0); CHECK_EQ(g_stub.ra_ons, 0);
            CHECK_EQ(ui.charger_test_cable_info().run == kOff, true);
            CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        }
        { // the confirmation window on a self-supplied K2 is the plain one unless the planned re-attach applies
            Ui ui; self_supplied_start(ui, plus); g_stub.pm.vbus_mv = 5000; open(ui); review(ui);   // the charger is the supply: VBUS is up
            press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
            start_press(ui, 1400); ui.tick(1500);
            CHECK_EQ(ui.charger_test_confirm_reattach(), false);
            CHECK_EQ(footer_says("Press again to start", kCaution), true);
            nothing_armed();
        }
    }
    { // a K2 powered on its own (HID) goes straight on too: its START detaches and the Ra goes on first
        rd_fixture(); set_cc(3300, 3300);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.ra_on, true);
    }
    { // the cable-on-plug refusal keeps its own screen and precedence on a K2 that is not self-supplied
        rd_fixture(); set_cc(3300, 0);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage);
        ui.tick(1500); settle(ui, 1500);
        check_row_text(0, "Start anyway", kCaution, kFocusFill, -1, 150, kRefuseY);
        check_row_text(1, "Start, skip spoof", kText, kBg, -1, 150, kRefuseY);
        CHECK_EQ(footer_says("Direct plug is the sure way", kCaution), true);
        CHECK_EQ(ui.charger_test_run_guard(), 0);
        press(ui, Button::Ok, 1600);   // Start anyway: the identity runs and the run records that the reading was overruled
        CHECK_EQ(g_stub.spoof_arms, 1);
        CHECK_EQ(ui.charger_test_run_guard() & 0x80, 0x80);
    }
    { // an Ra refusal says why on the footer, never the bare "tool busy"
        rd_fixture(); set_cc(3300, 3300); g_stub.ra_refuse = true;
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(ui.charger_test_stage(), 4);
        ui.tick(1700); settle(ui, 1700);
        CHECK_EQ(footer_says("Ra refused: contract live", kCaution), true);
    }
}
// ---- the planned re-attach (rdintent::plan_reattach, src/ui/ui_charger_test.cpp reattach_*) -------------------------------------
// A self-powered K2 on the CC2-pinned Rd, a source whose offer the cable holds down (3 A levels, a claim far above them) and a cable
// that claims 5 A: the run is journaled, the intent marker written, then the Rd drops for a moment with the Ra kept, so the
// charger reads the cable at a real attach. The K2 normally dies in the dark and the comeback boot (rd_comeback.cpp) finishes the
// story; here it survives and the run goes on.
// The source's offer is a list of raw PDO words (the lab captures, decoded), its claim the Source_Caps_Ext PDP (0 = never answered).
static void set_offer(std::initializer_list<uint32_t> raws, unsigned claim_w) {
    g_stub.caps.num_pdos = 0;
    for (const uint32_t r : raws) g_stub.caps.pdos[g_stub.caps.num_pdos++] = pd::decode_pdo(r);
    g_stub.ext.caps_ext = {};
    if (claim_w) { g_stub.ext.caps_ext.valid = true; g_stub.ext.caps_ext.pdp_w = uint8_t(claim_w); }
}
// A 120 W charger held at 3 A by the cable it read: 5/9/12/15/20 V at 3 A and two 3 A PPS ranges (60 W best), claim 120 W.
static void cable_limited_offer(unsigned claim_w = 120) {
    set_offer({167874860u, 184620u, 246060u, 307500u, 409900u, 3369873980u, 3381670460u}, claim_w);
}
// A 30 W brick: 12 V 2.5 A, 15 V 2 A, 20 V 1.5 A and a 33 W PPS, claim 30 W.
static void small_charger_offer(unsigned claim_w = 30) {
    set_offer({184652076u, 184620u, 246010u, 307400u, 409750u, 3369869628u}, claim_w);
}
static void planned_fixture() {
    fixture(); g_stub.self_powered = true; g_stub.sink.state = pd::SinkState::Ready;
    cable_limited_offer();
    set_cc(3300, 600);
}
// Ticks in 20 ms steps from `t` (the PD engine's service() each pass) until `stop()` holds, at most `max` ticks.
static uint32_t tick_until(Ui& ui, uint32_t t, bool (*stop)(), unsigned max = 600) {
    for (unsigned n = 0; n < max && !stop(); ++n) { t += 20; fusb302::service(); ui.tick(t); }
    return t;
}
static void test_planned_reattach() {
    { // applies: the confirmation announces the blink, the run takes its journal, the marker and the gap come in that order
        planned_fixture();
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); ui.tick(1450);
        CHECK_EQ(ui.charger_test_confirm_reattach(), true);
        CHECK_EQ(footer_says("Press again, K2 blinks once", kCaution), true);
        CHECK_EQ(g_stub.rdc_begins, 0); CHECK_EQ(g_stub.glitch_begins, 0);   // nothing yet: the second press starts it
        dump_screen("planned_reattach_confirm");
        confirm_press_nowait(ui, 1500);
        CHECK_EQ(ui.charger_test_reattach_planned(), true);
        CHECK_EQ(g_stub.rdc_run_cable.kind == ctcable::Kind::Builtin, true);   // the follower records the run's identity
        CHECK_EQ(g_stub.glitch_begins, 0);                                    // the journal comes first
        uint32_t t = tick_until(ui, 1500, [] { return g_stub.glitch_begins > 0; });
        CHECK_EQ(g_stub.glitch_begins, 1); CHECK_EQ(g_stub.rdc_begins, 1);
        CHECK_EQ(g_stub.rdc_begin_cable.kind == ctcable::Kind::Builtin, true);   // the marker names the cable the run presents
        CHECK_EQ(g_stub.journal_arm_open, false);                                // after the journal's meta record
        CHECK_EQ(g_stub.ra_on_at_glitch, true);                                  // the Ra is on before the Rd edge
        CHECK_EQ(ui.charger_test_reattach_state(), 1);
        CHECK_EQ(g_stub.glitch_ends, 0); CHECK_EQ(g_stub.rdc_ends, 0);
        g_stub.sink.state = pd::SinkState::Idle;    // the charger's contract is gone while the Rd is back
        for (unsigned n = 0; n < 4; ++n) { t += 20; fusb302::service(); ui.tick(t); }
        CHECK_EQ(g_stub.glitch_ends, 1);            // the gap is over (30 ms)
        CHECK_EQ(ui.charger_test_reattach_state(), 2);
        CHECK_EQ(g_stub.rdc_ends, 0);               // not alive until the new contract
        g_stub.sink.state = pd::SinkState::Ready;
        t += 20; ui.tick(t);
        CHECK_EQ(ui.charger_test_reattach_state(), 0); CHECK_EQ(g_stub.rdc_ends, 1);
        CHECK_EQ(g_stub.glitch_ends, 1);            // closed once
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // a K2 that survives with no new contract goes on after 5 s
        planned_fixture();
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        uint32_t t = tick_until(ui, 1500, [] { return g_stub.glitch_begins > 0; });
        g_stub.sink.state = pd::SinkState::Idle;
        t += 100; ui.tick(t);
        CHECK_EQ(ui.charger_test_reattach_state(), 2);
        t += 4900; ui.tick(t);
        CHECK_EQ(ui.charger_test_reattach_state(), 2);
        t += 200; ui.tick(t);
        CHECK_EQ(ui.charger_test_reattach_state(), 0); CHECK_EQ(g_stub.rdc_ends, 1);
    }
    { // the marker did not make it to the volume: no blink (the K2 would not be known to come back), the run goes on
        planned_fixture(); g_stub.rdc_begin_ok = false;
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        tick_until(ui, 1500, [] { return g_stub.rdc_begins > 0; });
        CHECK_EQ(g_stub.rdc_begins, 1);
        CHECK_EQ(g_stub.glitch_begins, 0); CHECK_EQ(g_stub.glitch_ends, 0); CHECK_EQ(g_stub.rdc_ends, 0);
        CHECK_EQ(ui.charger_test_reattach_state(), 0); CHECK_EQ(ui.charger_test_reattach_planned(), false);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // BACK / a stop during the gap: the Rd comes back, the marker is the follower's again
        planned_fixture();
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        tick_until(ui, 1500, [] { return g_stub.glitch_begins > 0; });
        CHECK_EQ(ui.charger_test_reattach_state(), 1);
        ui.cancel_charger_test();
        CHECK_EQ(g_stub.glitch_ends, 1); CHECK_EQ(g_stub.rdc_ends, 1); CHECK_EQ(ui.charger_test_reattach_state(), 0);
        CHECK_EQ(g_stub.rdc_run_cable.kind == ctcable::Kind::Off, true);   // the run's identity is no longer recorded
    }
    { // a stop after the gap (waiting for the contract) ends the marker's hand-over once and does not touch the Rd again
        planned_fixture();
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        uint32_t t = tick_until(ui, 1500, [] { return g_stub.glitch_begins > 0; });
        g_stub.sink.state = pd::SinkState::Idle;
        for (unsigned n = 0; n < 4; ++n) { t += 20; ui.tick(t); }
        CHECK_EQ(ui.charger_test_reattach_state(), 2);
        ui.cancel_charger_test();
        CHECK_EQ(g_stub.glitch_ends, 1); CHECK_EQ(g_stub.rdc_ends, 1);
    }
    // Not applicable: no blink, no marker, the plain footer, and the run starts as it always did.
    struct Case { const char* what; void (*set)(); bool cable_on; };
    static const Case cases[] = {
        {"a 30 W charger offering its whole claim", [] { small_charger_offer(); }, true},
        {"the offer is the source's own limit (20 V 5 A, claim 120 W)", [] { set_offer({167874860u, 184620u, 410100u}, 120); }, true},
        {"the claim is 65 W over a 60 W offer", [] { cable_limited_offer(65); }, true},
        {"not self-powered (HID or bus power)", [] { g_stub.self_powered = false; g_stub.bus_powered = true; }, true},
        {"spoof off", [] {}, false},
    };
    for (const auto& c : cases) {
        planned_fixture(); c.set();
        Ui ui; open(ui); review(ui);
        if (c.cable_on) { press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300); }
        start_press(ui, 1400); ui.tick(1450);
        CHECK_EQ(ui.charger_test_confirm_reattach(), false);
        CHECK_EQ(footer_says("Press again to start", kCaution), true);
        confirm_press_nowait(ui, 1500);
        tick_until(ui, 1500, [] { return false; }, 300);
        CHECK_EQ(ui.charger_test_reattach_planned(), false);
        CHECK_EQ(g_stub.rdc_begins, 0); CHECK_EQ(g_stub.glitch_begins, 0); CHECK_EQ(g_stub.glitch_ends, 0); CHECK_EQ(g_stub.rdc_ends, 0);
        if (c.what[0] == 'c') CHECK_EQ(g_stub.rdc_run_cable.kind == ctcable::Kind::Off, true);
        (void)c.what;
    }
    { // no claim in hand at START: the footer promises nothing, the arm asks the source, and a source that never answers is judged by
      // the shape of its offer (top level 20 V at exactly 3 A, nothing above)
        planned_fixture(); cable_limited_offer(0);
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); ui.tick(1450);
        CHECK_EQ(ui.charger_test_confirm_reattach(), false);
        CHECK_EQ(footer_says("Press again to start", kCaution), true);
        confirm_press_nowait(ui, 1500);
        tick_until(ui, 1500, [] { return g_stub.glitch_begins > 0; });
        CHECK_EQ(g_stub.probes >= 1, true);                 // the claim was asked for before the decision
        CHECK_EQ(ui.charger_test_reattach_planned(), true); CHECK_EQ(g_stub.glitch_begins, 1);
    }
    { // the same without a claim, and the offer is a 30 W brick's: no blink
        planned_fixture(); small_charger_offer(0);
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        tick_until(ui, 1500, [] { return false; }, 300);
        CHECK_EQ(g_stub.probes >= 1, true);
        CHECK_EQ(ui.charger_test_reattach_planned(), false); CHECK_EQ(g_stub.glitch_begins, 0); CHECK_EQ(g_stub.rdc_begins, 0);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // the claim arrives from the probe the arm started: it decides alone (a 30 W claim over a 3 A shape: no blink)
        planned_fixture(); cable_limited_offer(0);
        g_stub.probe.state = pd::ProbeState::Running;
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        uint32_t t = tick_until(ui, 1500, [] { return g_stub.probes > 0; });   // the arm asked
        CHECK_EQ(g_stub.probes >= 1, true);
        t = tick_until(ui, t, [] { return false; }, 25);   // 500 ms: the probe is still running, the decision waits
        CHECK_EQ(ui.charger_test_reattach_planned(), false); CHECK_EQ(g_stub.glitch_begins, 0);
        g_stub.ext.caps_ext.valid = true; g_stub.ext.caps_ext.pdp_w = 60;   // the source claims 60 W: the 60 W offer is all of it
        g_stub.probe.state = pd::ProbeState::Done;
        tick_until(ui, t, [] { return false; }, 100);
        CHECK_EQ(ui.charger_test_reattach_planned(), false); CHECK_EQ(g_stub.glitch_begins, 0); CHECK_EQ(g_stub.rdc_begins, 0);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    }
    { // a probe that never ends: the decision comes after the claim wait (1 s) from the offer's shape, the run is not held for more
        planned_fixture(); cable_limited_offer(0);
        g_stub.probe.state = pd::ProbeState::Running;
        Ui ui; open(ui); review(ui);
        press(ui, Button::Back, 1300); pick_row(ui, kBuiltinRow, 1300);
        start_press(ui, 1400); confirm_press_nowait(ui, 1500);
        uint32_t t = tick_until(ui, 1500, [] { return g_stub.probes > 0; });
        t = tick_until(ui, t, [] { return false; }, 25);
        CHECK_EQ(g_stub.glitch_begins, 0);
        tick_until(ui, t, [] { return g_stub.glitch_begins > 0; }, 200);
        CHECK_EQ(g_stub.glitch_begins, 1);
    }
    { // an HID-powered START presents the Ra before the Rd (apply_cable arms the identity, then the Rd goes on), and never blinks
        rd_fixture(); set_cc(3300, 3300);
        Ui ui; open(ui); review(ui);
        pick_builtin(ui);
        CHECK_EQ(g_stub.ra_at_first_rd, 1);
        CHECK_EQ(g_stub.rd_run, true);
        tick_until(ui, 1500, [] { return false; }, 100);
        CHECK_EQ(g_stub.glitch_begins, 0); CHECK_EQ(g_stub.rdc_begins, 0);
    }
}
// ---- the session outlives the page (the page arena builds the page only while it is shown) -----------------------------------
// A finished run's results are in the session, not in the page: the page can be gone and the next one shows them again.
static void test_results_and_cursor_outlive_the_page() {
    fixture(); g_stub.bus_powered = true;
    g_stub.caps.num_pdos = 3; g_stub.caps.pdos[1] = pd::decode_pdo((180u << 10) | 300); g_stub.caps.pdos[2] = pd::decode_pdo((240u << 10) | 300);
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t now = 1500;
    for (; now <= 900000 && ui.charger_test_busy(); now += 100) ui.tick(now);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    const auto run_id = ui.charger_test_results().run_id();
    const auto n = ui.charger_test_results().steps().size();
    CHECK_EQ(n > 0, true);
    CHECK_EQ(ui.charger_test_stage(), 6);                   // Stage::Results
    ui.goto_page(static_cast<uint8_t>(Page::Home), now + 100);
    CHECK_EQ(int(ui.live_pages()), 1);
    for (uint32_t end = now + 2000; now < end; now += 100) ui.tick(now);
    CHECK_EQ(int(ui.charger_test_results().state()), int(ct::State::Done));
    CHECK_EQ(ui.charger_test_results().run_id() == run_id, true);
    CHECK_EQ(ui.charger_test_results().steps().size(), n);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now + 100);   // a page built afresh over the same session
    CHECK_EQ(ui.charger_test_results().run_id() == run_id, true);
    CHECK_EQ(ui.charger_test_results().steps().size(), n);
}
// A resume holding the Rd goes on while another page is shown: the arena has no ChargerTest page then, the session polls on.
static void test_resume_goes_on_while_another_page_shows() {
    interrupted_run(true);
    Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);
    ui.goto_page(static_cast<uint8_t>(Page::Detail), 1050);
    CHECK_EQ(int(ui.live_pages()), 1);
    CHECK_EQ(ui.charger_test_stage(), kResumeStage);        // leaving the page did not end the resume
    uint32_t now = 1100;
    for (; now <= 2400 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);       // no page is showing it, and it still resumed
    CHECK_EQ(ui.charger_test_busy(), true);
    CHECK_EQ(g_stub.journal_resumes, 1);
    ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), now + 100);   // the page that opens finds the run in progress
    CHECK_EQ(ui.charger_test_busy(), true);
}
// wake-vs-test (bench 2026-11-10): the charger test owns the idle wake's line for the whole run - the Rd-off gaps of the unit
// cycle included - and gives it back the moment the run ends, is left or is cancelled. The session sets the driver's run owner.
static void test_the_run_owns_the_idle_wake_line_for_its_whole_run() {
    { // a live charger: nothing owned in the review or the confirm window, owned on every tick of the run, released at its end
        fixture(); g_stub.pm.vbus_mv = 5000;
        Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
        CHECK_EQ(g_stub.run_owner, false);
        start_press(ui, 1300);
        CHECK_EQ(g_stub.run_owner, false);                   // the confirm window touches nothing
        confirm_press_nowait(ui, 1400);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(g_stub.run_owner, true);                    // from the START itself, before the first tick
        unsigned busy_ticks = 0, not_owned = 0;
        uint32_t now = 1500;
        for (; now <= 60000 && ui.charger_test_busy(); now += 100) {
            ui.tick(now);
            if (ui.charger_test_busy()) { busy_ticks++; if (!g_stub.run_owner) not_owned++; }
        }
        CHECK_EQ(busy_ticks > 20, true);
        CHECK_EQ(not_owned, 0u);                             // every busy phase, the unit detach and attach gaps included
        ui.tick(now + 100);
        CHECK_EQ(ui.charger_test_busy(), false);
        CHECK_EQ(g_stub.run_owner, false);                   // the run is over: the sleeping-port wake is back
    }
    { // leaving the page mid-run cancels it: released
        fixture(); g_stub.pm.vbus_mv = 5000;
        Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_units(kPdOnly), true); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1400);
        ui.tick(1500);
        CHECK_EQ(g_stub.run_owner, true);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 1600);
        CHECK_EQ(g_stub.run_owner, false);
    }
    { // the START wait (Rd presented, VBUS not up yet, its Rd-off retries) is the run's too
        fixture(); g_stub.pm.vbus_mv = 0; g_stub.vbus_on_rd = true;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1400);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage);
        ui.tick(1500);
        CHECK_EQ(g_stub.run_owner, true);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 1600);
        CHECK_EQ(g_stub.run_owner, false);
    }
    { // an interrupted run waiting to resume owns the line too (the boot's resume gate), and so does the resumed run
        interrupted_run(true);
        Ui ui; ui.init(0); ui.set_rotation(0, 0); ui.tick(1000);
        CHECK_EQ(ui.charger_test_stage(), kResumeStage);
        CHECK_EQ(g_stub.run_owner, true);
        uint32_t now = 1100;
        for (; now <= 2400 && ui.charger_test_stage() == kResumeStage; now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        CHECK_EQ(g_stub.run_owner, true);
    }
}

// ---- Waiting: the + key wakes a new charger (hardware: it is wired to the CC pins), the firmware only watches ----
// START with a charger that is not up: Rd presented, the Waiting stage, ticks to `t_end`.
static void enter_waiting(Ui& ui, uint32_t t_end) {
    review(ui);
    start_press(ui, 1300); confirm_press_nowait(ui, 1350);
    for (uint32_t t = 1370; t <= t_end; t += 20) { fusb302::service(); ui.tick(t); }
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    settle(ui, t_end);
}
static size_t count_of(const std::string& hay, const char* needle) {
    size_t n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) ++n;
    return n;
}
static void test_waiting_plus_minus_do_nothing() {
    fixture(); g_stub.pm.vbus_mv = 0; set_cc(0, 0);
    Ui ui; open(ui);
    enter_waiting(ui, 1500);
    const uint8_t cursor = ui.charger_test_cursor();
    const uint8_t page = ui.page_index();
    uint32_t t = 1520;
    // Every press length of both keys: no paging (a long + was the global "next page", which cancelled the wait), no cursor.
    for (Button b : {Button::Plus, Button::Minus})
        for (ButtonEvent ev : {ButtonEvent::Short, ButtonEvent::Long, ButtonEvent::Hold}) {
            ui.on_button(b, ev, t); t += 20;
            fusb302::service(); ui.tick(t); t += 20;
            CHECK_EQ(ui.charger_test_stage(), kWaitStage);
            CHECK_EQ(ui.page_index(), page);
            CHECK_EQ(ui.charger_test_cursor(), cursor);
            CHECK_EQ(g_stub.rd_run, true);   // the Rd is still presented: nothing cancelled the wait
        }
    // BACK still cancels: Review again, Rd released.
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    CHECK_EQ(ui.charger_test_stage(), 4);
    CHECK_EQ(g_stub.rd_run, false);
    // A long BACK on another stage keeps its meaning: straight to the Meter.
    ui.on_button(Button::Back, ButtonEvent::Long, t + 100);
    CHECK_EQ(ui.page_index() != page, true);
}
static void test_waiting_footer_says_how_to_wake() {
    struct Case { int cc1, cc2, vbus; const char* text; uint16_t color; };
    static const Case cases[] = {
        {0, 0, 0, "No charger. Hold + few secs", kCaution},
        {1700, 0, 0, "No VBUS. Hold + few secs", kText},
        {1700, 0, 800, "Standby. Wake the charger", kText},   // the existing higher-priority lines stay
    };
    for (const auto& c : cases) {
        fixture(); g_stub.pm.vbus_mv = c.vbus; set_cc(c.cc1, c.cc2);
        Ui ui; open(ui);
        enter_waiting(ui, 1500);
        ui.tick(1600);
        CHECK_EQ(footer_says(c.text, c.color), true);
    }
}
// The direct-plug line fallback shows on Waiting: the footer names the line the Rd is on (and, after a move, that it moved),
// the checklist's first row names it too, and "Hold + few secs" returns once the schedule is over. Nothing repaints on an
// idle tick (the footer is cached by its text, the checklist row by its signature).
static void test_waiting_names_the_line_being_tried() {
    fixture(); g_stub.pm.vbus_mv = 0; set_cc(1700, 1700);
    Ui ui; open(ui);
    enter_waiting(ui, 1500);
    ui.tick(1600); settle(ui, 1600);
    CHECK_EQ(footer_says("No VBUS. Hold + few secs", kText), true);   // no schedule yet: the older hint
    g_stub.cc = 1; g_stub.trial_line = 1; g_stub.trial_moves = 0; g_stub.trial_live = true;
    ui.tick(1700); settle(ui, 1700);
    CHECK_EQ(footer_says("CC1 picked. Waking charger", kText), true);
    check_text_at(kSmall, 146, "Rd on CC1", kText, kBg, 28);
    dump_screen("wait_line_picked");   // K2_DUMP: the screen for the design review (runs/ui-wait-line)
    g_stub.cc = 2; g_stub.trial_line = 2; g_stub.trial_moves = 1;   // one move made
    ui.tick(1800); settle(ui, 1800);
    CHECK_EQ(footer_says("Now on CC2. Waking charger", kText), true);
    check_text_at(kSmall, 146, "Rd on CC2", kText, kBg, 28);
    dump_screen("wait_line_moved");
    g_stub.trial_line = 1; g_stub.trial_moves = 3; g_stub.cc = 1;   // the fourth dwell
    ui.tick(1900); settle(ui, 1900);
    CHECK_EQ(footer_says("Now on CC1. Waking charger", kText), true);
    g_stub.trial_live = false; g_stub.trial_line = 0;   // the schedule ran out: the key hint takes the footer back
    ui.tick(2000); settle(ui, 2000);
    CHECK_EQ(footer_says("No VBUS. Hold + few secs", kText), true);
    dump_screen("wait_line_exhausted");
    // No Rp on either line: nothing to wake, whatever the schedule says.
    set_cc(0, 0); g_stub.trial_line = 1; g_stub.trial_live = true;
    ui.tick(2100); settle(ui, 2100);
    CHECK_EQ(footer_says("No charger. Hold + few secs", kCaution), true);
    dump_screen("wait_no_charger");
}
static void test_waiting_logs_the_plus_hold() {
    fixture(); g_stub.pm.vbus_mv = 0; set_cc(0, 0);
    Ui ui; open(ui);
    enter_waiting(ui, 2000);
    // Nothing pressed: not one line, however long the wait.
    CHECK_EQ(g_stub.log_text.find("[WAKE]") == std::string::npos, true);
    g_stub.log_text.clear(); g_stub.log_fmts.clear();
    uint32_t t = 2020;
    set_cc(0, 1650);   // the key pulls CC2 up while held: the log must carry that number
    ui.set_pressed(Button::Plus, true, t);
    ui.on_button(Button::Plus, ButtonEvent::Long, t + 500);
    for (; t <= 4000; t += 20) { fusb302::service(); ui.tick(t); }
    ui.set_pressed(Button::Plus, false, t);   // the detector fires nothing on the release of a long press
    const uint32_t released_at = t;
    set_cc(0, 0);
    for (; t <= released_at + 3000; t += 20) { fusb302::service(); ui.tick(t); }
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);   // the 5 s wait is not over (attached: 10 s)
    const std::string& log = g_stub.log_text;
    CHECK_EQ(count_of(log, "[WAKE] + pressed: CC1 0 mV CC2 1650 mV VBUS 0 mV\n"), 1u);
    CHECK_EQ(count_of(log, "[WAKE] + released after "), 1u);
    // ~10 Hz: 2 s down + 2 s tail = ~40 sample lines, never the 20 ms tick rate (100 lines).
    const size_t samples = count_of(log, "[WAKE] t=");
    CHECK_EQ(samples >= 34 && samples <= 42, true);
    CHECK_EQ(count_of(log, "key=1 CC1 0 mV CC2 1650 mV VBUS 0 mV") >= 17, true);   // while down
    CHECK_EQ(count_of(log, "key=0 CC1 0 mV CC2 0 mV VBUS 0 mV") >= 17, true);      // the tail after the release
    // The whole window ends 2 s after the release: the last second of the run is silent.
    const size_t last = log.rfind("[WAKE]");
    CHECK_EQ(last != std::string::npos, true);
    CHECK_EQ(log.find("\n", last) != std::string::npos, true);
    // The fixed phrases live in the format strings, the numbers are %d (the binlog cuts %s at 12 bytes).
    CHECK_EQ(g_stub.log_fmts.find("[WAKE] + pressed: CC1 %d mV CC2 %d mV VBUS %d mV\n") != std::string::npos, true);
    CHECK_EQ(g_stub.log_fmts.find("[WAKE] t=%d ms key=%d CC1 %d mV CC2 %d mV VBUS %d mV\n") != std::string::npos, true);
    CHECK_EQ(g_stub.log_fmts.find("[WAKE] + released after %d ms: CC1 %d mV CC2 %d mV VBUS %d mV\n") != std::string::npos, true);
}
static void test_plus_hold_off_waiting_is_silent() {
    fixture(); set_cc(0, 0);
    Ui ui; open(ui); review(ui);
    g_stub.log_text.clear();
    ui.set_pressed(Button::Plus, true, 1400);
    for (uint32_t t = 1420; t <= 3000; t += 20) ui.tick(t);
    ui.set_pressed(Button::Plus, false, 3000);
    CHECK_EQ(g_stub.log_text.find("[WAKE]") == std::string::npos, true);
}
static void test_waiting_cc_cells_show_raw_mv_while_plus_is_down() {
    fixture(); g_stub.pm.vbus_mv = 0; set_cc(0, 1650);
    Ui ui; open(ui);
    enter_waiting(ui, 1500);
    ui.tick(1600);
    check_text_at(kSmall, 92, "Rp 3A", kText, kBg, 148);   // the Rp word while the key is up
    ui.set_pressed(Button::Plus, true, 1700);
    ui.tick(1800);
    check_text_at(kSmall, 92, "1650mV", kText, kBg, 148);   // the raw level while it is down
    ui.set_pressed(Button::Plus, false, 1900);
    ui.tick(2000);
    check_text_at(kSmall, 92, "Rp 3A", kText, kBg, 148);
}

// Shell parity batch 1 (red): the charger-test view is what the screen shows, read without a screenshot. The footer words
// are the page's own (one implementation: footer_line), the preview is the Review summary, the timers are the ones the
// Waiting and Running screens draw.
static std::string view_text(const ChargerTestSession::View& v) { return std::string(v.message, v.message_len); }
static void test_view_says_what_the_wizard_shows() {
    { // the Tests step: stage 0, the rows of the list, the footer's words
        fixture(); Ui ui; open(ui); ui.tick(1200);
        const auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, 0); CHECK_EQ(v.cursor, 1); CHECK_EQ(v.on_screen, true); CHECK_EQ(v.rows >= 6, true);
        CHECK_STR(view_text(v).c_str(), "Picked: All tests");
        CHECK_EQ(v.confirming, false); CHECK_EQ(v.card_no, 0); CHECK_EQ(v.delta_n, 0);
    }
    { // Review: the preview counts, the footer, then the confirmation window with its timer
        fixture(); Ui ui; open(ui); review(ui); ui.tick(1300);
        auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, 4); CHECK_EQ(v.rows, 1);   // START
        CHECK_EQ(v.preview.runnable > 0, true); CHECK_EQ(v.preview.refusal, ct::Reason::None);
        CHECK_EQ(v.confirming, false); CHECK_EQ(v.confirm_left_ms, 0u);
        CHECK_STR(view_text(v).c_str(), "Check the other port");
        start_press(ui, 1300); ui.tick(1400);
        v = ui.charger_test_view();
        CHECK_EQ(v.confirming, true); CHECK_EQ(v.confirm_left_ms > 0 && v.confirm_left_ms <= Ui::kConfirmWindowMs, true);
        CHECK_STR(view_text(v).c_str(), "Press again to start");
        CHECK_EQ(footer_says("Press again to start", kCaution), true);
    }
    { // the preview is live with no page on screen: the refusal follows VBUS, the page-only fields read 0 / empty
        fixture(); g_stub.sink.state = pd::SinkState::Ready; g_stub.sink.pos = 1; g_stub.sink.mv = 5000;
        Ui ui; open(ui); review(ui); ui.tick(1300);
        ui.goto_page(static_cast<uint8_t>(Page::Home), 1400);
        g_stub.pm.vbus_mv = 8470; ui.tick(1500);
        const auto v = ui.charger_test_view();
        CHECK_EQ(v.on_screen, false); CHECK_EQ(v.rows, 0); CHECK_EQ(v.message_len, 0);
        CHECK_EQ(v.stage, 4);   // the session keeps its stage while the page is away
        CHECK_EQ(v.preview.refusal, ct::Reason::WrongVoltage); CHECK_EQ(v.preview.refusal_mv, 8470);
    }
}
static void test_view_carries_the_wait_and_the_run() {
    { // Waiting: the give-up clock's total (10 s with a charger attached) and the time spent, as paint_waiting counts them
        fixture(); g_stub.pm.vbus_mv = 0;
        Ui ui; open(ui); review(ui);
        start_press(ui, 1300); confirm_press_nowait(ui, 1350);
        for (uint32_t t = 1370; t <= 2370; t += 20) { fusb302::service(); ui.tick(t); }
        const auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, kWaitStage);
        CHECK_EQ(v.wait_total_ms, pd::kStartVbusAttachedWaitMs);
        CHECK_EQ(v.wait_elapsed_ms >= 900 && v.wait_elapsed_ms <= 1100, true);
        CHECK_EQ(v.wait_retry, false); CHECK_EQ(v.wait_paused, false); CHECK_EQ(v.confirming, false);
        CHECK_EQ(v.message_len > 0, true);
        // START was taken: the Review preview's "no source" (VBUS was 0 at Review) is no refusal any more
        CHECK_EQ(v.preview.refusal, ct::Reason::None); CHECK_EQ(v.preview.refusal_mv, 0);
    }
    { // Running: the m:ss clock in whole seconds, the footer's unit line; Results: the saved card's number
        fixture(); g_stub.fc_answers = false; Ui ui; open(ui); only_vivo_review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 3500 && ui.charger_test_busy(); t += 100) ui.tick(t);
        auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, kRunningStage); CHECK_EQ(v.run_elapsed_s >= 1 && v.run_elapsed_s <= 3, true);
        CHECK_EQ(v.rows > 0, true); CHECK_EQ(v.message_len > 0, true);
        CHECK_EQ(v.preview.refusal, ct::Reason::None);   // no START refusal while a run is going
        for (; t <= 600000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        ui.tick(t); ui.tick(t + 200);
        v = ui.charger_test_view();
        CHECK_EQ(v.stage, 6); CHECK_EQ(v.run_elapsed_s, 0); CHECK_EQ(v.rows > 0, true);
        CHECK_EQ(v.card_no != 0, true);
    }
}

// ---- remote START / CANCEL (Cmd::ChargerTest [4, flags] / [5]): the device's own START after its confirmation, and its own stop ----
using RemoteStart = ChargerTestSession::RemoteStart;
using Stopped = ChargerTestSession::Stopped;
static void boot_home(Ui& ui) { ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); settle(ui, 1000); }
static void ticks(Ui& ui, uint32_t from, uint32_t to) { for (uint32_t t = from; t <= to; t += 20) ui.tick(t); }   // the main loop's UI tick
static void test_remote_start_is_the_devices_start() {
    { // from another page: the charger-test page opens on Review with the confirm window; nothing is touched until the Ra
      // guard's window is full, then the second-press path confirms it and the run starts by itself, shown on the screen
        fixture(); Ui ui; boot_home(ui);
        CHECK_EQ(ui.page() == Page::ChargerTest, false);
        CHECK_EQ(ui.charger_test_set_units(kPdOnly), true);
        const auto r = ui.charger_test_start(true, 1200);
        CHECK_EQ(r.result == RemoteStart::Result::Accepted, true);
        CHECK_EQ(ui.page() == Page::ChargerTest, true);
        CHECK_EQ(ui.charger_test_stage(), 4);
        CHECK_EQ(algos(ui), ct::kDefaultAlgorithms);   // the "All tests" preset
        CHECK_EQ(ui.charger_test_view().confirming, true);
        nothing_armed_stub(); no_operations();
        uint32_t t = 1200;
        for (; t < 1800; t += 20) ui.tick(t);   // inside the guard's window (a sample every 100 ms, 8 of them): only the confirmation
        CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed_stub();
        for (; t <= 2100 && !ui.charger_test_busy(); t += 20) ui.tick(t);
        CHECK_EQ(ui.charger_test_busy(), true); CHECK_EQ(ui.charger_test_stage(), kRunningStage);
        // a second START while the run holds the session: Busy, the run goes on
        CHECK_EQ(ui.charger_test_start(true, 2200).result == RemoteStart::Result::Busy, true);
        CHECK_EQ(ui.charger_test_busy(), true);
        uint32_t now = 2300;
        for (; now <= 20000 && g_stub.requests < 1; now += 100) ui.tick(now);
        CHECK_EQ(g_stub.requests, 1);
        // CANCEL is BACK: the run stops (released to 5 V) and ends on Results
        CHECK_EQ(ui.charger_test_stop(now) == Stopped::Run, true);
        CHECK_EQ(g_stub.releases, 1);
        for (const uint32_t end = now + 20000; now <= end && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.requests, 1);
        CHECK_EQ(ui.charger_test_stop(now) == Stopped::Nothing, true);
    }
    { // without the preset flag the draft selection in force is the run's
        fixture(); Ui ui; open(ui); top(ui); move(ui, kPdRow); press(ui, Button::Ok); settle(ui);
        const uint32_t draft = ui.charger_test_selection();
        CHECK_EQ(draft != ct::kDefaultAlgorithms, true);
        CHECK_EQ(ui.charger_test_start(false, 1300).result == RemoteStart::Result::Accepted, true);
        CHECK_EQ(ui.charger_test_selection(), draft);
    }
    { // refused at once by the preflight: the reason comes back, the footer says it, no window opens, nothing is armed
        fixture(); Ui ui; open(ui); g_stub.sink.held = true;
        const auto r = ui.charger_test_start(true, 1200);
        CHECK_EQ(r.result == RemoteStart::Result::Refused, true);
        CHECK_EQ(r.reason == ct::Reason::Busy, true);
        CHECK_EQ(ui.charger_test_view().confirming, false);
        ui.tick(1300); settle(ui, 1300);
        CHECK_EQ(footer_says("PD hold: release it", kCaution), true);
        ui.tick(2500);
        CHECK_EQ(ui.charger_test_busy(), false); nothing_armed_stub();
    }
    { // the Ra guard reads a cable on the plug: the Refuse stage, as on the device; CANCEL returns to Review, nothing armed
        rd_fixture(); set_cc(3300, 0); Ui ui; open(ui);
        ticks(ui, 1100, 3000);   // a dark charger: the link detaches and its list goes (a changed offer drops a confirmation)
        CHECK_EQ(ui.charger_test_set_cable(kBuiltin), true);
        CHECK_EQ(ui.charger_test_start(false, 3000).result == RemoteStart::Result::Accepted, true);
        ticks(ui, 3000, 3900);
        CHECK_EQ(ui.charger_test_stage(), kRefuseStage); nothing_armed();
        CHECK_EQ(ui.charger_test_stop(4000) == Stopped::Refusal, true);
        CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed();
    }
    { // a charger alone (no VBUS): the Rd and the wait as on the device; CANCEL in the wait gives the Rd back, nothing started
        fixture(); g_stub.pm.vbus_mv = 0; Ui ui; open(ui);
        ticks(ui, 1100, 3000);
        CHECK_EQ(ui.charger_test_start(true, 3000).result == RemoteStart::Result::Accepted, true);
        CHECK_EQ(g_stub.rd_run, false);
        ticks(ui, 3000, 3900);
        CHECK_EQ(ui.charger_test_stage(), kWaitStage); CHECK_EQ(g_stub.rd_run, true);
        CHECK_EQ(ui.charger_test_start(true, 3950).result == RemoteStart::Result::Busy, true);
        CHECK_EQ(ui.charger_test_stop(4000) == Stopped::Wait, true);
        CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(ui.charger_test_busy(), false);
    }
    { // CANCEL inside the guard's window: the confirmation closes and the START never goes on
        fixture(); Ui ui; open(ui);
        CHECK_EQ(ui.charger_test_start(true, 1200).result == RemoteStart::Result::Accepted, true);
        CHECK_EQ(ui.charger_test_stop(1300) == Stopped::Confirmation, true);
        ticks(ui, 1300, 3000);
        CHECK_EQ(ui.charger_test_busy(), false); nothing_armed_stub(); no_operations();
    }
    { // a key pressed on the device inside the window drops the confirmation, as it drops the device's own
        fixture(); Ui ui; open(ui);
        CHECK_EQ(ui.charger_test_start(true, 1200).result == RemoteStart::Result::Accepted, true);
        press(ui, Button::Plus, 1300);
        ticks(ui, 1300, 3000);
        CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(ui.charger_test_stage(), 4); nothing_armed_stub();
    }
}
// The remote START is the device's own: the same forced identity, with the Settings spoof Off.
static void test_remote_offers_start_presents_the_epr_identity() {
    fixture(); Ui ui; boot_home(ui);
    CHECK_EQ(ui.charger_test_set_cable(ctcable::Choice{}), true);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Offers)), true);
    CHECK_EQ(ui.charger_test_start(false, 1200).result == RemoteStart::Result::Accepted, true);
    CHECK_EQ(g_stub.spoof_on, false);                                            // nothing armed until the confirmation is through
    for (uint32_t t = 1200; t <= 2100 && !ui.charger_test_busy(); t += 20) ui.tick(t);
    CHECK_EQ(ui.charger_test_busy(), true);
    CHECK_EQ(ui.charger_test_results().level(), ct::Level::Offers);
    CHECK_EQ(g_stub.spoof_on, true); CHECK_EQ(g_stub.spoof_arms, 1);
    CHECK_EQ(ui.charger_test_cable_info().run == ctcable::kDefaultChoice, true);
}
// "Do not disturb the connection": a load behind the K2 (the inline latch) means nothing new transmits, whatever the level.
static void inline_now() { g_stub.pm_online = true; g_stub.pm.vbus_mv = 3000; g_stub.pm.current_ma = 500; }   // a live sink on a part-way VBUS: the ticks latch the link
static void test_inline_at_entry_touches_nothing() {
    rd_fixture();
    Ui ui; open(ui);
    inline_now();
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Offers)), true);
    for (uint32_t t = 1200; t < 3000; t += 20) ui.tick(t);
    CHECK_EQ(ui.link().inline_mode(), true);                                     // the latch is real, not just the stub flag
    nothing_armed_stub(); no_operations();
    review(ui, 3100);                                                            // the Tests, Cable and Review steps read nothing either
    for (uint32_t t = 3200; t < 5000; t += 20) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 4);
    nothing_armed_stub(); no_operations();   // scans counts the VCONN e-marker read
}
// Offers goes through the same START guards as every level: refused with the live-sink reason, nothing presented, asked or armed.
static void test_offers_start_is_refused_inline() {
    rd_fixture();
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_cable(ctcable::Choice{}), true);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Offers)), true);
    review(ui);
    inline_now();
    for (uint32_t t = 1300; t < 1700; t += 20) ui.tick(t);
    CHECK_EQ(ui.link().inline_mode(), true);
    start_run(ui, 1800);
    CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(ui.charger_test_stage(), 4);
    CHECK_EQ(footer_says("load attached", kCaution), true);
    nothing_armed_stub(); no_operations(); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.ra_on, false);
}
// A load that latches while START waits for the charger ends the wait: the Rd and the identity go back, nothing is read.
static void test_inline_latch_during_the_wait_restores_the_port() {
    rd_fixture(); g_stub.vbus_on_rd = false;                                     // VBUS never comes: the wait stays open
    Ui ui; open(ui);
    CHECK_EQ(ui.charger_test_set_cable(ctcable::Choice{}), true);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Offers)), true);
    review(ui);
    start_press(ui, 1400); confirm_press_nowait(ui, 1500);
    CHECK_EQ(ui.charger_test_stage(), kWaitStage);
    CHECK_EQ(g_stub.rd_run, true); CHECK_EQ(g_stub.spoof_on, true);              // the forced identity is on the line
    const int scans = g_stub.scans;
    inline_now();
    for (uint32_t t = 1520; t < 2200; t += 20) { fusb302::service(); ui.tick(t); }
    CHECK_EQ(ui.link().inline_mode(), true);
    CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_busy(), false);
    CHECK_EQ(g_stub.rd_run, false); CHECK_EQ(g_stub.spoof_on, false); CHECK_EQ(g_stub.ra_on, false);
    CHECK_EQ(g_stub.scans, scans);                                               // nothing was read on the way out
    CHECK_EQ(int(ui.charger_test_results().reason()), int(ct::Reason::Inline));
}

// OEM levels by their watts (oem_watts.hpp): a VOOC-family charger detected at 5 V (opener 0xA8). Medium: the ident alone, every
// level not tested. Deep: the ceiling stage stood at 0x06D under 11.02 V (120 W claimed): every SuperVOOC level claimed. The Results
// grid reads "VOOC" [detected, 20W] and "SVOOC" [50W 65W 100W 120W]; the renders (K2_DUMP) cover all four rotations.
static void vooc_detects(int proto) {   // the adapter clocked the ident (0xA8): a VOOC-family session ends Detected, no voltage proof
    const auto p = static_cast<k2::fc::Proto>(proto);
    const bool vooc = p == k2::fc::Proto::Vooc || p == k2::fc::Proto::SuperVooc || p == k2::fc::Proto::SuperVooc65;
    g_stub.fc_status_fail = vooc ? int(k2::fc::Fail::Detected) : 0;
    g_stub.fc_status_ident = vooc ? 0xA8 : 0;
}
static void vooc_silent(int proto) {   // no opener after either wake: the family ends NoAnswer
    const auto p = static_cast<k2::fc::Proto>(proto);
    const bool vooc = p == k2::fc::Proto::Vooc || p == k2::fc::Proto::SuperVooc || p == k2::fc::Proto::SuperVooc65;
    g_stub.fc_status_fail = vooc ? int(k2::fc::Fail::NoAnswer) : 0;
    g_stub.fc_status_ident = 0;
}
static void vooc_watts_run(Ui& ui, uint32_t& t, ct::Level level, uint8_t rot, uint16_t ceiling = 0x06D, int32_t vmax_mv = 11020, bool silent = false) {
    six_pdo_fixture();
    if (level == ct::Level::Deep) {   // the ladder went on under every ask up to `ceiling`
        g_stub.svooc_n_asks = 4;
        const uint16_t asks[4] = {0x032, 0x041, 0x064, 0x06D};
        for (unsigned i = 0; i < 4; ++i) g_stub.svooc_asks[i] = std::min(asks[i], ceiling);
        g_stub.svooc_ceiling = ceiling;
        g_stub.svooc_after = uint8_t(svooc::kAcceptPolls);
        g_stub.svooc_vmax_mv = vmax_mv;
    }
    constexpr uint8_t kUnits[] = {uint8_t(ct::Unit::Vooc), uint8_t(ct::Unit::SuperVooc), uint8_t(ct::Unit::SuperVooc65), uint8_t(ct::Unit::Pd)};
    open(ui); ui.set_rotation(rot, 1050); settle(ui, 1050);
    CHECK_EQ(ui.charger_test_set_units(kUnits), true);
    CHECK_EQ(ui.charger_test_set_level(uint8_t(level)), true);
    review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    g_stub.fc_start_hook = silent ? vooc_silent : vooc_detects;
    bool mid = false;
    for (t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) {
        ui.tick(t);
        const auto& st = ui.charger_test_results().steps();
        if (silent && !mid && !st.empty() && st[0].algorithm == A::Vooc && st[0].verdict == ct::Verdict::Skip) {   // the running frame of a silent unit
            mid = true; settle(ui, t);
            char nm[64]; std::snprintf(nm, sizeof nm, "vooc_silent_running_rot%u", unsigned(rot)); dump_panel(nm);
        }
    }
    g_stub.fc_start_hook = nullptr;
    g_stub.fc_status_fail = g_stub.fc_status_ident = 0;
    CHECK_EQ(ui.charger_test_stage(), 6);
    ui.tick(t); t += 200; ui.tick(t); t += 200;
    settle(ui, t);
}
// A charger that never clocked after either wake: the family rows are skips ("no answer"), not fails; the grid folds them into
// "No reply" and the Detail footer reads the phrase. The renders cover all four rotations.
static void test_vooc_family_silent_reads_no_answer() {
    for (uint8_t rot = 0; rot < 4; ++rot) {
        Ui ui; uint32_t t;
        vooc_watts_run(ui, t, ct::Level::Medium, rot, 0, 0, true);
        const auto& r = ui.charger_test_results();
        unsigned fam = 0;
        for (const auto& s : r.steps()) {
            const bool f = s.algorithm == A::Vooc || s.algorithm == A::SuperVooc || s.algorithm == A::SuperVooc65;
            if (!f) continue;
            ++fam;
            CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
            CHECK_EQ(s.detail, uint8_t(k2::fc::Fail::NoAnswer));
            CHECK_EQ(result_layout::is_no_answer(s), true);
        }
        CHECK_EQ(fam >= 3, true);
        char name[64];
        std::snprintf(name, sizeof name, "vooc_silent_rot%u", unsigned(rot)); dump_panel(name);
        for (int k = 0; k < 3; ++k) { press(ui, Button::Plus, t); settle(ui, t); t += 200; }   // PPS, Xiaomi, then the no-reply group
        std::snprintf(name, sizeof name, "vooc_silent_rot%u_row2", unsigned(rot)); dump_panel(name);
        press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 7);
        std::snprintf(name, sizeof name, "vooc_silent_rot%u_steps", unsigned(rot)); dump_panel(name);
        press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 8);
        std::snprintf(name, sizeof name, "vooc_silent_rot%u_detail", unsigned(rot)); dump_panel(name);
    }
}
// A charger with no vivo protocol: the 5 V request gets no frame (NoResponse). Every VIVO row is a skip ("no reply"), not a fail; the
// grid folds it into "No reply" and the Detail footer reads the engine's words. The renders cover all four rotations.
static void vivo_silent(int proto) { g_stub.fc_status_fail = proto == int(k2::fc::Proto::Vivo) ? int(k2::fc::Fail::NoResponse) : 0; g_stub.fc_status_ident = 0; }
static void test_silent_vivo_reads_no_reply_not_a_fail() {
    for (uint8_t rot = 0; rot < 4; ++rot) {
        Ui ui;
        six_pdo_fixture();
        constexpr uint8_t kUnits[] = {uint8_t(ct::Unit::Vivo), uint8_t(ct::Unit::Pd)};
        open(ui); ui.set_rotation(rot, 1050); settle(ui, 1050);
        CHECK_EQ(ui.charger_test_set_units(kUnits), true);
        CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Medium)), true);
        review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        g_stub.fc_start_hook = vivo_silent;
        uint32_t t = 1500;
        bool mid = false;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) {
            ui.tick(t);
            // The running grid right after the silent unit's first step ended: dim and struck, never a red cell (the frame to set beside vooc_silent_running).
            const auto& st = ui.charger_test_results().steps();
            if (!mid && !st.empty() && st[0].algorithm == A::Vivo && st[0].verdict == ct::Verdict::Skip) {
                mid = true; settle(ui, t);
                char nm[64]; std::snprintf(nm, sizeof nm, "vivo_none_running_rot%u", unsigned(rot)); dump_panel(nm);
            }
        }
        CHECK_EQ(mid, true);
        g_stub.fc_start_hook = nullptr;
        g_stub.fc_status_fail = g_stub.fc_status_ident = 0;
        CHECK_EQ(ui.charger_test_stage(), 6);
        ui.tick(t); t += 200; ui.tick(t); t += 200;
        settle(ui, t);
        unsigned vivo = 0;
        for (const auto& s : ui.charger_test_results().steps()) {
            if (s.algorithm != A::Vivo) continue;
            ++vivo;
            CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
            CHECK_EQ(s.detail, uint8_t(k2::fc::Fail::NoResponse));
            CHECK_EQ(result_layout::is_no_answer(s), true);
        }
        CHECK_EQ(vivo >= 2, true);
        char name[64];
        std::snprintf(name, sizeof name, "vivo_none_rot%u", unsigned(rot)); dump_panel(name);
        for (int k = 0; k < 3; ++k) { press(ui, Button::Plus, t); settle(ui, t); t += 200; }
        std::snprintf(name, sizeof name, "vivo_none_rot%u_row2", unsigned(rot)); dump_panel(name);
        press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 7);
        std::snprintf(name, sizeof name, "vivo_none_rot%u_steps", unsigned(rot)); dump_panel(name);
        press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
        CHECK_EQ(ui.charger_test_stage(), 8);
        std::snprintf(name, sizeof name, "vivo_none_rot%u_detail", unsigned(rot)); dump_panel(name);
    }
}
// A PD-only charger: QC2 (with its Class B rows), QC3 and UFCS get no first handshake. Each unit is a skip ("no reply") at once: the
// Running grid shows its one dim cell, never a red one, and Results folds them into "No reply".
static void qc_silent(int proto) {
    const auto p = static_cast<k2::fc::Proto>(proto);
    const bool qc = p == k2::fc::Proto::Qc2 || p == k2::fc::Proto::Qc3 || p == k2::fc::Proto::Ufcs;
    g_stub.fc_status_fail = qc ? int(k2::fc::Fail::NoHandshake) : 0;
    g_stub.fc_status_ident = 0;
}
static void test_silent_qc_reads_no_reply_not_a_fail() {
    for (uint8_t rot = 0; rot < 4; ++rot) {
        Ui ui;
        six_pdo_fixture();
        constexpr uint8_t kUnits[] = {uint8_t(ct::Unit::Qc2), uint8_t(ct::Unit::Qc3), uint8_t(ct::Unit::Ufcs), uint8_t(ct::Unit::Pd)};
        open(ui); ui.set_rotation(rot, 1050); settle(ui, 1050);
        CHECK_EQ(ui.charger_test_set_units(kUnits), true);
        CHECK_EQ(ui.charger_test_set_level(uint8_t(ct::Level::Medium)), true);
        review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        g_stub.fc_start_hook = qc_silent;
        uint32_t t = 1500;
        bool mid = false, late = false;
        for (; t <= 900000 && ui.charger_test_busy(); t += 100) {
            ui.tick(t);
            const auto& st = ui.charger_test_results().steps();
            if (!late) for (const auto& y : st) if (y.algorithm == A::Ufcs && y.verdict == ct::Verdict::Skip) {
                late = true; settle(ui, t);   // QC2, QC3 and UFCS all settled: three dim "No reply" cells
                char nm[64]; std::snprintf(nm, sizeof nm, "qc_none_running_late_rot%u", unsigned(rot)); dump_panel(nm);
                break;
            }
            if (!mid && !st.empty() && st[0].algorithm == A::Qc2 && st[0].verdict == ct::Verdict::Skip) {
                mid = true; settle(ui, t);
                char nm[64]; std::snprintf(nm, sizeof nm, "qc_none_running_rot%u", unsigned(rot)); dump_panel(nm);
            }
        }
        CHECK_EQ(mid, true);
        CHECK_EQ(late, true);
        g_stub.fc_start_hook = nullptr;
        g_stub.fc_status_fail = g_stub.fc_status_ident = 0;
        CHECK_EQ(ui.charger_test_stage(), 6);
        ui.tick(t); t += 200; ui.tick(t); t += 200;
        settle(ui, t);
        unsigned silent = 0;
        for (const auto& s : ui.charger_test_results().steps()) {
            if (s.algorithm != A::Qc2 && s.algorithm != A::Qc3 && s.algorithm != A::Ufcs && s.algorithm != A::Qc2ClassB && s.algorithm != A::Qc3ClassB) continue;
            ++silent;
            CHECK_EQ(s.verdict == ct::Verdict::Skip, true);
            CHECK_EQ(s.detail, uint8_t(k2::fc::Fail::NoHandshake));
            CHECK_EQ(result_layout::is_no_answer(s), true);
        }
        CHECK_EQ(silent >= 5, true);
        char name[64];
        std::snprintf(name, sizeof name, "qc_none_rot%u", unsigned(rot)); dump_panel(name);
    }
}
static void test_vooc_family_results_read_watts_levels() {
    struct Scene { const char* name; ct::Level level; uint16_t ceiling; int32_t vmax_mv; };
    // deep69: the ladder stood at 6.3 A under 11.0 V (69 W): 50 and 65 W claimed, 100 and 120 W not reached
    constexpr Scene kScenes[] = {{"medium", ct::Level::Medium, 0, 0}, {"deep", ct::Level::Deep, 0x06D, 11020},
                                 {"deep69", ct::Level::Deep, 0x03F, 11000}};
    for (const Scene& sc : kScenes)
        for (uint8_t rot = 0; rot < 4; ++rot) {
            const ct::Level level = sc.level;
            Ui ui; uint32_t t;
            vooc_watts_run(ui, t, level, rot, sc.ceiling, sc.vmax_mv);
            const auto& r = ui.charger_test_results();
            unsigned detected = 0;
            for (const auto& s : r.steps())
                detected += (s.algorithm == A::Vooc || s.algorithm == A::SuperVooc || s.algorithm == A::SuperVooc65) &&
                            s.verdict == ct::Verdict::Pass && s.detail == uint8_t(k2::fc::Fail::Detected);
            CHECK_EQ(detected >= 1, true);
            if (!detected && rot == 0)
                for (const auto& s : r.steps())
                    std::printf("[oem-watts] step alg %u mv %u verdict %u reason %u detail %u\n", unsigned(s.algorithm), unsigned(s.mv),
                                unsigned(s.verdict), unsigned(s.reason), unsigned(s.detail));
            if (level == ct::Level::Deep && rot == 0)
                std::printf("[oem-watts] deep: svooc_end %u word 0x%03X vmax %u\n", unsigned(r.evidence().svooc_end),
                            unsigned(r.evidence().svooc_word), unsigned(r.evidence().svooc_vmax_mv));
            const char* lv = sc.name;
            char name[64];
            std::snprintf(name, sizeof name, "oem_watts_%s_rot%u", lv, unsigned(rot)); dump_panel(name);
            press(ui, Button::Plus, t); settle(ui, t); t += 200;   // the second row: SVOOC, its footer
            std::snprintf(name, sizeof name, "oem_watts_%s_rot%u_row2", lv, unsigned(rot)); dump_panel(name);
            if (rot != 0 && rot != 3) continue;
            press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;   // the level row opens the family's steps
            CHECK_EQ(ui.charger_test_stage(), 7);
            CHECK_EQ(ui.list_state().count >= 3, true);   // the three detection steps (VOOC, SVOOC, SVOOC65)
            std::snprintf(name, sizeof name, "oem_watts_%s_rot%u_steps", lv, unsigned(rot)); dump_panel(name);
            press(ui, Button::Ok, t); ui.tick(t); settle(ui, t); t += 200;
            CHECK_EQ(ui.charger_test_stage(), 8);
            std::snprintf(name, sizeof name, "oem_watts_%s_rot%u_detail", lv, unsigned(rot)); dump_panel(name);
        }
}

// The Xiaomi (SVID 0x2717) unit on the three hardware cases and a source without the SVID: a PD-only run on the list the source
// re-announced after the chain (the sink's trace keeps the list from before it). K2_DUMP=dir writes dir/k2_mi_<case>_rot<r>.panel
// (the Results grid, scrolled to the Xiaomi row) and k2_mi_<case>_steps / _detail (the row's Steps and its first mode's Detail).
struct MiCase { const char* name; std::array<uint32_t, 7> before, after; uint8_t n; bool svid, authed; uint8_t finish, modes; uint16_t cable_ma = 5000; };
static const MiCase kMiCases[] = {
    {"bank", {0x0201912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000640C8, 0xC0DC323C, 0xC1903228},
             {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000640C8, 0xC0DC3264, 0xC1903228}, 7, true, true, 8, 1},
    {"fh22", {0x0A81912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9A43264, 0},
             {0x0A81912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9A43264, 0}, 6, true, true, 8, 0},
    {"cuk", {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9903264},
            {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3278, 0xC9903278}, 7, true, true, 0x0A, 2},
    // c) on a 6 A cable (the Xiaomi 6A cable): the two 6 A modes are tested ("Mi 66W" at 11 V 6 A), not skipped for the cable
    {"cuk6a", {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0xC9903264},
              {0x0A01912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x000641F4, 0xC8DC3278, 0xC9903278}, 7, true, true, 0x0A, 2, 6000},
    {"nosvid", {0x0A01912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0, 0},
               {0x0A01912C, 0x0002D12C, 0x0004B12C, 0x000641F4, 0xC8DC3264, 0, 0}, 5, false, false, 0, 0},
};
static void mi_case_fixture(const MiCase& c) {
    fixture();
    g_stub.caps.num_pdos = c.n;
    for (uint8_t i = 0; i < c.n; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(c.after[i]);
    g_stub.cable_limits.max_mv = 20000; g_stub.cable_limits.max_ma = c.cable_ma; g_stub.cable_limits.observed = true;   // a read cable
    g_stub.vbus_ceiling_mv = 21000;   // every PD level the source offers comes (a clean run)
    g_stub.sink.xiaomi_svid = g_stub.mi_svid = c.svid; g_stub.sink.xiaomi_authed = g_stub.mi_authed = c.authed;
    if (c.authed) {
        auto& x = g_stub.mi_xm;
        x.authed = true; x.reannounced = true; x.last_step = c.finish; x.finish = c.finish;
        x.n_pre = c.n;
        for (uint8_t i = 0; i < c.n; ++i) x.pre[i] = c.before[i];
    }
    g_stub.xm = g_stub.mi_xm;
}
static void run_mi_case(Ui& ui, const MiCase& c, uint32_t& t) {
    mi_case_fixture(c);
    ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100);
    settle(ui, 1100);
    top(ui); move(ui, kPdRow); press(ui, Button::Ok); settle(ui);    // PD only
    press(ui, Button::Ok); settle(ui);                                // the cable in force
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    for (t = 1500; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100);
}
static void test_xiaomi_unit_scenes() {
    for (const MiCase& c : kMiCases) {
        Ui ui; uint32_t t = 0;
        run_mi_case(ui, c, t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        // one Xiaomi row on every case: the chain row plus each mode the diff found (bank: PPS 11V 3A->5A; fh22: none; cuk: both
        // PPS APDOs 5A->6A; nosvid: the chain row alone, skipped)
        const FamilyPick mi = pick_family(ui, "Xiaomi");
        CHECK_EQ(mi.line >= 0, true);
        CHECK_EQ(mi.count, unsigned(c.modes) + 1);
        {   // the grid row: the unlocked modes alone; "auth" passed when nothing unlocked, skipped when the chain did not run
            result_layout::Layout l; result_layout::build(l, ui.charger_test_results().steps());
            std::string g;
            result_layout::Tone first = result_layout::Tone::Dim;
            for (size_t i = 0; i < l.rows.size(); ++i) {
                if (l.rows[i].header || std::string_view(l.rows[i].label) != "Xiaomi") continue;
                first = l.tokens[l.rows[i].first].tone;
                for (unsigned k = l.rows[i].first; k < unsigned(l.rows[i].first) + l.rows[i].count; ++k) { if (!g.empty()) g += ' '; g += l.tokens[k].text; }
            }
            const std::string_view n = c.name;
            CHECK_STR(g.c_str(), n == "bank" ? "55W" : n == "fh22" ? "auth" : n == "nosvid" ? "auth" : n == "cuk" ? "66W cable 120W cable" : "66W 120W");
            if (n == "fh22") CHECK_EQ(int(first), int(result_layout::Tone::Pass));
            if (n == "nosvid") CHECK_EQ(int(first), int(result_layout::Tone::Skip));
        }
        unsigned modes = 0, cable_skips = 0, tested = 0;
        for (const auto& s : ui.charger_test_results().steps()) {
            if (!ct::is_mi_mode(s)) continue;
            modes++;
            cable_skips += s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::CableLimit;
            tested += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;
        }
        CHECK_EQ(modes, unsigned(c.modes));
        // a mode over the cable is skipped "cable limit" (cuk on a 5 A cable); on a 6 A cable it is tested (never above the offer)
        if (c.modes && c.cable_ma > 5000) { CHECK_EQ(cable_skips, 0u); CHECK_EQ(tested, unsigned(c.modes)); }
        if (c.modes == 2 && c.cable_ma == 5000) CHECK_EQ(cable_skips, 2u);
        const auto calm = [&] { for (int i = 0; i < 64 && ui.painting(); ++i) ui.tick(t += 100); };
        for (const uint8_t rot : {0, 1, 2, 3}) {
            ui.set_rotation(rot, t); calm();
            top(ui, t); calm();
            const int line = pick_family(ui, "Xiaomi").line;
            for (int k = 0; k < line; ++k) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
            char name[48]; std::snprintf(name, sizeof name, "mi_%s_rot%u", c.name, unsigned(rot));
            dump_screen(name);
            if (rot == 3 && line >= 0) {
                press(ui, Button::Ok); ui.tick(t += 100); calm();
                std::snprintf(name, sizeof name, "mi_%s_steps", c.name); dump_screen(name);
                if (ui.charger_test_stage() == 7) {
                    if (ui.list_state().count > 1) { press(ui, Button::Plus); ui.tick(t += 100); calm(); }
                    press(ui, Button::Ok); ui.tick(t += 100); calm();
                    std::snprintf(name, sizeof name, "mi_%s_detail", c.name); dump_screen(name);
                    press(ui, Button::Back); ui.tick(t += 100); calm();
                }
                press(ui, Button::Back); ui.tick(t += 100); calm();
            }
        }
        ui.set_rotation(0, t);
    }
}


// Stage footers are floating toasts, never persistent: each times out after kToastDefaultMs, and the repaint that clears it does
// not raise it again (Results, Steps, Detail, and Review, whose pill covers the START hint).
static void toast_times_out(const char* what, Ui& ui, uint32_t& t) {
    ticks_for(ui, t, 3000);
    settle(ui, t);
    if (ui.context().toast.active(t) || ui.context().toast.drawn) std::fprintf(stderr, "toast persists on %s\n", what);
    CHECK_EQ(ui.context().toast.active(t), false);
    CHECK_EQ(ui.context().toast.drawn, false);
    ticks_for(ui, t, 3000);
    CHECK_EQ(ui.context().toast.active(t), false);
}
static void test_toasts_time_out() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    toast_times_out("results", ui, t);
    press(ui, Button::Ok, t); t += 200; ui.tick(t); settle(ui, t);
    toast_times_out("steps", ui, t);
    press(ui, Button::Ok, t); t += 200; ui.tick(t); settle(ui, t);
    toast_times_out("detail", ui, t);
    {   Ui r; open(r); review(r); uint32_t u = 1300; r.tick(u);
        toast_times_out("review", r, u);
        CHECK_EQ(r.charger_test_stage(), 4);   // the START hint is back, nothing left over it
    }
}

static void test_transient_current_spike_does_not_abort_run() {
    // Regression (150819-deep-b4d5): a dark charger wakes on START even with a 0->5V inrush spike, and a single raw
    // current spike mid-run (e.g. transcap 1-avg 140 us sample during PPS steps) does not latch inline / fail the run.
    fixture();
    g_stub.pm.vbus_mv = 0;
    g_stub.vbus_on_rd = true;
    g_stub.caps.valid = false;
    g_stub.caps.num_pdos = 0;
    g_stub.caps.active_cc = 0;
    Ui ui;
    open(ui);
    review(ui);
    g_hook_t0 = 1300;
    start_press(ui, 1300);
    confirm_press_nowait(ui, 1300);
    // Wake the charger with a brief 2-tick inrush spike as VBUS rises.
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 250;
    ui.tick(1350);
    ui.tick(1370);
    g_stub.pm.current_ma = 0;
    finish_wait(ui, 1370, caps_after_700ms);
    CHECK_EQ(ui.charger_test_stage(), kRunningStage);
    CHECK_EQ(ui.link().inline_mode(), false);
    // Mid-run single-tick and 2-tick current spikes (even across a 470 ms gap) must not latch inline.
    uint32_t t = g_wait_end + 100;
    for (int i = 0; i < 12 && ui.charger_test_busy(); ++i) {
        g_stub.pm.current_ma = (i == 2 || i == 8 || i == 9) ? 140 : 0;
        t += (i == 9) ? 470 : 20;
        ui.tick(t);
    }
    CHECK_EQ(ui.link().inline_mode(), false);
}
int main() {
    overlap_enable();
    test_view_says_what_the_wizard_shows();
    test_view_carries_the_wait_and_the_run();
    test_waiting_plus_minus_do_nothing();
    test_waiting_footer_says_how_to_wake();
    test_waiting_names_the_line_being_tried();
    test_waiting_logs_the_plus_hold();
    test_plus_hold_off_waiting_is_silent();
    test_waiting_cc_cells_show_raw_mv_while_plus_is_down();
    test_the_run_owns_the_idle_wake_line_for_its_whole_run();
    test_results_and_cursor_outlive_the_page();
    test_resume_goes_on_while_another_page_shows();
    test_ra_guard_refuses_a_cable();
    test_refuse_stage_wording_and_skip_spoof_keeps_the_pick();
    test_ra_guard_speaks_on_the_second_ok();
    test_ra_guard_lets_a_direct_charger_through();
    test_self_supplied_start();
    test_planned_reattach();
    test_resume_unattended_goes_on_by_itself();
    test_self_powered_vbus_loss_keeps_the_journal_open();
    test_resume_rearms_the_cable_identity();
    test_resume_boot_arm_refused_is_retried_by_the_resume();
    test_boot_arms_the_selected_identity_when_self_powered();
    test_boot_does_not_arm_without_an_identity_or_when_not_self_powered_or_unsafe();
    test_wizard_pick_survives_a_reboot_and_arms_on_a_self_powered_boot();
    test_spoof_tool_pick_survives_a_reboot();
    test_hid_powered_boot_restores_the_selection_without_arming();
    test_saved_clone_selection_and_a_clone_that_is_gone();
    test_a_clone_deleted_while_running_clears_the_record();
    test_blank_volume_defaults_to_the_epr_spoof();
    test_an_explicit_off_pick_stays_off();
    test_start_on_a_blank_volume_arms_the_epr_spoof();
    test_the_retired_builtin_id_reads_as_oneplus_100w();
    test_the_record_is_written_only_when_the_selection_changes();
    test_a_failed_write_is_retried();
    test_off_is_remembered();
    test_bus_powered_run_behaves_like_self_powered();
    test_resume_after_reboot_never_waits_for_ok();
    test_resume_waits_for_vbus_without_giving_up();
    test_resume_cancelled_by_back_at_boot();
    test_resume_host_powered_presents_rd();
    test_resume_host_powered_back_drops_the_rd();
    test_a_final_stop_closes_the_journal_before_it_skips();
    test_a_row_in_flight_writes_no_journal_record(); test_a_row_in_flight_paints_inside_the_budget();
    test_the_plan_is_written_in_pieces_before_the_first_request(); test_a_busy_link_defers_the_plan_but_not_for_ever(); test_a_cancel_during_the_plan_leaves_no_run(); test_a_replan_is_polled_and_done_before_the_first_phase_marker();
    test_a_row_in_an_engine_window_paints_nothing(); test_the_running_grid_follows_the_step(); test_a_light_run_bar_starts_empty_and_only_fills();
    test_the_gap_line_names_the_blocked_window(); test_a_started_row_that_ends_as_a_skip_still_logs_its_gap();
    test_a_handshake_window_says_why_the_screen_is_still(); test_the_running_screen_keeps_updating_through_a_qc_handshake();
    test_failed_units_stay_above_the_running_row(); test_the_gauge_band_and_the_dot_move_in_the_same_tick(); test_the_running_dot_follows_the_qc2_level();
    test_a_manual_scroll_of_the_running_grid_holds_then_resumes();
    test_resume_self_and_bus_powered_present_no_rd();
    test_resume_host_powered_refusals();
    test_resume_host_powered_identity_follows_the_guard();
    test_resume_replay_is_transient_scratch();
    test_no_journal_releases_an_inherited_rd();
    test_cable_list_rows_and_default();
    test_cable_dialog_off_is_todays_start();
    test_cable_dialog_builtin_run_owns_ra_and_spoof(); test_cable_identity_is_dropped_for_a_legacy_unit_and_back_for_the_pd_unit();
    test_run_identity_is_released_when_the_run_ends(); test_run_identity_survives_no_run_in_a_session();
    test_abandoned_resume_releases_the_runs_identity();
    test_busy_refusal_names_the_cause_and_the_way_out(); test_tools_spoof_survives_a_runs_end(); test_review_after_a_run_with_an_identity();
    test_cable_step_next_curr_and_start_takes_over();
    test_cable_dialog_saved_clone();
    test_cable_dialog_every_builtin_row();
    test_cable_dialog_scrolls_with_many_clones();
    test_cable_dialog_refusals();
    test_cable_dialog_back_during_confirm_goes_passive();
    test_confirm_window_back_and_expiry_touch_nothing();
    test_cable_choice_remote_default();
    test_results_selection_moves_and_view_follows_it();
    test_results_incremental_repaint_matches_forced();
    test_results_update_matches_forced();
    test_transition_capture_hooks();
    test_run_events_follow_the_run();
    test_detail_capture();
    test_start_wait_is_not_blocking();
    test_start_presents_rd();
    test_selection_and_focus_rendering();
    test_long_ok_toggles_a_group();
    test_long_identity_rendering();
    test_results_counters_are_words();
    test_results_grid_rows_rail_and_footer();
    test_results_cells_are_flat_fills();
    test_a_capped_range_reads_amber_and_says_how_far_it_got();
    test_a_pass_or_a_skip_reads_as_placeholders();
    test_toasts_time_out();
    test_steps_rows_show_whole_labels();
    test_step_label_gallery();
    test_pd_block_scenes();
    test_readme_frames();
    test_ufcs_results_and_detail_show_the_mode_current();
    test_results_best_line_names_the_best_modes();
    test_pd_version_answer_against_the_offer();
    test_results_grid_is_frozen_at_run_end();
    test_review_start_is_only_row();
    test_vsafe5_review();
    test_preflight_refusal_is_logged();
    test_preflight_wrong_voltage_under_pdo1_contract_is_logged();
    test_the_results_grid_carries_the_pdspec_section();
    test_layout_b_range_lines_carry_their_spec_word();
    test_the_spec_detail_shows_the_limit_while_the_engine_holds_the_run();
#if K2_SPECROWS
    test_the_spec_detail_lists_the_transitions_of_the_journal();
#else
    test_the_spec_detail_is_the_summary_table_without_transition_rows();
#endif
    test_a_deep_run_lists_its_transitions_from_the_engine();
    test_the_running_counter_counts_executed_rows_like_the_bar();
    test_family_steps_scenes();
    test_a_long_family_list_scrolls_under_its_header();
    test_a_voltage_fold_reads_as_a_caution();
    test_a_step_walk_miss_reads_as_a_caution();
    test_a_transition_flag_reads_as_a_caution();
    test_a_failed_sweep_footer_never_reads_as_reached();
    test_a_sweep_off_by_more_than_100mv_reads_as_a_caution();
    test_sweep_log_line_has_no_literal_format_bug();
    test_state_change_log_keeps_the_full_state_name_in_the_format();
    test_avs_sweep_is_a_selection_bit();
    test_avs_sweep_row_follows_the_only_sweep_kind();
    test_the_default_run_is_medium();
    test_depth_offers_value_and_the_forced_epr_spoof(); test_other_levels_keep_the_spoof_pick(); test_remote_offers_start_presents_the_epr_identity();
    test_depth_row_cycles_and_shows_on_review();
    test_an_old_journal_resumes_without_the_default_sweep();
    test_self_powered_review_note(); test_review_carries_the_other_port_warning(); test_phantom_evidence_does_not_refuse_a_run_but_a_load_does(); test_self_powered_run_is_on_the_card(); test_a_cable_read_once_stays_with_the_run();
    test_pps_wire_headroom_note();
    test_first_menu_and_pd_bundles();
    test_tests_step_presets();
    test_back_chain();
    test_three_step_header();
    test_review_shows_the_armed_identity();
    test_armed_marker_is_shared_with_tools_spoof();
    test_full_help_text();
    test_confirmation_and_capture(); test_fake_offers_refused();
    test_group_selection_and_empty_refusal(); test_cable_pick_only_selects_and_start_arms();
    test_start_with_off_disarms_a_tools_spoof();
    test_cable_page_never_writes_flash(); test_spoof_refusals_and_shared_busy(); test_confirmation_bound_to_exact_proposal();
    test_skip_results_and_deselection(); test_navigation_and_stop_controls();
    test_start_waits_for_the_caps();
    test_decisions_use_the_unaveraged_vbus();
    test_remote_start_is_the_devices_start(); test_inline_at_entry_touches_nothing(); test_offers_start_is_refused_inline(); test_inline_latch_during_the_wait_restores_the_port();
    test_vooc_family_results_read_watts_levels();
    test_vooc_family_silent_reads_no_answer();
    test_silent_vivo_reads_no_reply_not_a_fail();
    test_silent_qc_reads_no_reply_not_a_fail();
    test_xiaomi_unit_scenes();
    test_transient_current_spike_does_not_abort_run();
    return test_report("test_ui_charger_test");
}
