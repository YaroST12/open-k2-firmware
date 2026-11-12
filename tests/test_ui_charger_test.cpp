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
#include "k2/ui/ui_widgets.hpp"
#include "k2/ui/blend.hpp"
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
static void move(Ui& ui, unsigned n, uint32_t now = 1200) { for (unsigned i = 0; i < n; ++i) press(ui, Button::Plus, now); }
static void top(Ui& ui, uint32_t now = 1200) { for (unsigned i = 0; i < ct::kMaxSteps; ++i) press(ui, Button::Minus, now); }
static void open(Ui& ui) { ui.init(0); ui.set_rotation(0, 0); ui.tick(1000); ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); }
// A preset pick advances (there is no Next row): the first preset ("All tests") goes on to the Cable step.
static void cable(Ui& ui, uint32_t now = 1200) {
    top(ui, now); press(ui, Button::Ok, now);
    CHECK_EQ(ui.charger_test_stage(), 2);
}
// A cable pick selects and advances: the cursor starts on the cable in force, so one OK keeps it and goes on to Review.
static void review(Ui& ui, uint32_t now = 1200) {
    cable(ui, now); press(ui, Button::Ok, now);
    CHECK_EQ(ui.charger_test_stage(), 4); CHECK_EQ(ui.charger_test_cursor(), 0);
}
constexpr int kCustomStage = 14;   // Stage::Custom, the per-family / per-algorithm list under Tests step "Custom..." (not a numbered step)
// Tests step rows (0-based): 0 All tests, 1 PD only, 2 Legacy only, 3 Vendor modes, 4 Custom..., 5 Last results (after a run).
constexpr int kAllRow = 0, kPdRow = 1, kLegacyRow = 2, kVendorRow = 3, kCustomRow = 4;
// Custom list rows (the families in run order): 0 Huawei / Honor, 1 Samsung, 2 OPPO / vivo, 3 Xiaomi / Redmi, 4 USB PD,
// 5 QC / UFCS, 6 Done (to the Cable step). Short OK toggles a family, long OK opens its entries.
constexpr int kFamHuawei = 0, kFamSamsung = 1, kFamOppo = 2, kFamXiaomi = 3, kFamPd = 4, kFamQc = 5, kFamDone = 6;
static void custom(Ui& ui, uint32_t now = 1200) {   // from the Tests step: open the Custom list
    top(ui, now); move(ui, kCustomRow, now); press(ui, Button::Ok, now);
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
}
static void long_ok(Ui& ui, uint32_t now = 1200) { ui.on_button(Button::Ok, ButtonEvent::Long, now); }
// Open a family's entries from the Custom list (long OK on its row).
static void open_group(Ui& ui, int family, uint32_t now = 1200) {
    top(ui, now); move(ui, family, now); long_ok(ui, now);
    CHECK_EQ(ui.charger_test_stage(), 1);
}
// From the Custom list: Done goes on to the Cable step, one OK keeps the cable and goes on to Review.
static void custom_done_review(Ui& ui, uint32_t now = 1200) {
    top(ui, now); move(ui, kFamDone, now); press(ui, Button::Ok, now);
    CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Ok, now);
    CHECK_EQ(ui.charger_test_stage(), 4);
}
// Only VIVO armed (a short legacy run): from the Tests step, clear the five toggleable families, open OPPO's entries, pick the
// fifth one, then Back, Done, the cable and Review.
static void only_vivo_review(Ui& ui, uint32_t now = 1200) {
    custom(ui, now);
    for (int fam : {kFamHuawei, kFamSamsung, kFamOppo, kFamPd, kFamQc}) { top(ui, now); move(ui, fam, now); press(ui, Button::Ok, now); }
    CHECK_EQ(ui.charger_test_selection(), 0u);
    open_group(ui, kFamOppo, now); move(ui, 4, now); press(ui, Button::Ok, now);   // OPPO group: Next, VOOC, SVOOC, SVOOC65, VIVO
    CHECK_EQ(ui.charger_test_selection(), ct::bit(A::Vivo));
    press(ui, Button::Back, now);
    custom_done_review(ui, now);
}
constexpr uint32_t kPdMask = ct::kDefaultAlgorithms & (ct::kPdFixedAlgorithms | ct::kPpsAlgorithms);
constexpr uint32_t kLegacyMask = ct::kDefaultAlgorithms & ~(ct::kPdFixedAlgorithms | ct::kPpsAlgorithms);
constexpr int kRefuseStage = 12;   // Stage::Refuse, the Ra guard's refusal of a spoof START
constexpr int kWaitStage = 13;     // Stage::Waiting, START's wait for the charger (never blocks the UI)
constexpr int kRunningStage = 5;   // Stage::Running
constexpr int kResumeStage = 9;    // Stage::Resume
constexpr int kRefuseY = 110;      // the Refuse stage's answer rows start here (under the alert band and its two lines)
// Cable rows (the one identity list): 0 Off, 1..3 the three built-in chips (ids 1, 2, 4), 4.. the saved clones.
constexpr int kOffRow = 0, kBuiltinRow = 1, kSavedRow = 4;
// Pick a cable-identity row from the top of the Cable step: it selects the choice and advances to Review (nothing is armed).
static void pick_row(Ui& ui, int row, uint32_t now = 1200) {
    top(ui, now); move(ui, row, now); press(ui, Button::Ok, now);
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
    CHECK_EQ(ui.list_state().count, 5); CHECK_EQ(ui.list_state().visible, 7);   // four presets, Custom... (no run to view yet)
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
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
    press(ui, Button::Ok); CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    move(ui, 1); CHECK_EQ(ui.list_state().offset, 0);
    press(ui, Button::Ok);           // PPS off
    CHECK_EQ(ui.charger_test_selection() & ct::kPpsAlgorithms, 0);
    press(ui, Button::Ok); CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    long_ok(ui);                     // long OK on an entry page toggles the whole group
    CHECK_EQ(ui.charger_test_selection() & (ct::kPdFixedAlgorithms | ct::kPpsAlgorithms), 0);
    long_ok(ui); CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), kCustomStage); CHECK_EQ(ui.charger_test_cursor(), kFamPd);
    // Short OK on a family toggles all of it: clear every family that can be toggled (Xiaomi rides the PD unit).
    top(ui);
    for (const int f : {kFamHuawei, kFamSamsung, kFamOppo, kFamPd, kFamQc}) { top(ui); move(ui, f); press(ui, Button::Ok); }
    CHECK_EQ(ui.charger_test_selection(), 0);
    top(ui); move(ui, kFamDone); press(ui, Button::Ok);   // Done: the Cable step
    CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Ok); CHECK_EQ(ui.charger_test_stage(), 4);
    start_press(ui, 1300); start_press(ui, 1400);
    CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(ui.charger_test_results().run_id(), 0);
    no_operations();
    press(ui, Button::Back); press(ui, Button::Back);   // Review -> Cable -> Tests
    CHECK_EQ(ui.charger_test_stage(), 0);
    top(ui); move(ui, kAllRow); press(ui, Button::Ok);   // the "All tests" preset brings the defaults back
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
}
// Long OK on a Custom family opens its entries; on an entries page it toggles the whole group: all selected -> none,
// otherwise -> all of it.
static void test_long_ok_toggles_a_group() {
    fixture(); Ui ui; open(ui);
    const auto long_ok = [&] { ui.on_button(Button::Ok, ButtonEvent::Long, 1200); };
    const uint32_t qc = ct::bit(A::Qc2) | ct::bit(A::Qc3) | ct::bit(A::Qc3Plus);
    top(ui); move(ui, kLegacyRow); long_ok();                 // a Tests-step preset row: long OK does nothing at all
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms); CHECK_EQ(ui.charger_test_stage(), 0);
    custom(ui);
    top(ui); move(ui, kFamQc); long_ok();                     // on a family row long OK opens its entries
    CHECK_EQ(ui.charger_test_stage(), 1); CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
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
// Kit RowList look: rows of kit::kRowH from `y0` (the body top), the 16 px text at kit::kTextX, the marker at kit::kMarkX, the
// focus rail x 0..2. A focused row with a note opens kit::kFocusExtra more (`open` = that row; the rows below it shift down).
static int row_top(int row, int open = -1, int y0 = kit::kBodyY) {
    return y0 + row * kit::kRowH + (open >= 0 && row > open ? kit::kFocusExtra : 0);
}
static int wizard_text_y(int row, int open = -1, int y0 = kit::kBodyY) {
    return row_top(row, open, y0) + kit::row_text_y(kit::kRowH, kSmall.height);
}
constexpr int kSavedRoom = kit::kTailX - 40 - 8 - kit::kTextX;   // a saved clone's name: the room before the dim "saved" tail
constexpr int kNoTailRoom = 224 - ListView::kBarW - kit::kTextX;   // a row with no rating column and no tail: up to the scroll lane
constexpr int kNextPillRoom = kit::kTailX - pill_width(text_cells("NEXT")) - kit::kTailGap - kit::kTextX;   // room beside a NEXT pill
static void check_text_at(const Font& font, int y, std::string_view text, uint16_t color, uint16_t bg, int x0 = kit::kTextX) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    gfx_text({0, 0, 224, 240}, font, x0, y, text, color, bg);
    unsigned diff = 0;
    for (int yy = y; yy < y + font.height; ++yy)
        for (int x = x0; x < x0 + text_width(font, text); ++x)
            diff += actual[yy * LCD_WIDTH + x] != g_panel[yy * LCD_WIDTH + x];
    if (diff) std::fprintf(stderr, "  text '%.*s' at x=%d y=%d differs\n", int(text.size()), text.data(), x0, y);
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
    check_text_at(kLabelFont, row_top(row, -1, y0) + kit::kNoteDy, fit_text(kLabelFont, note, kit::kNoteMaxPx, buf), kDim, kFocusT);
}
// A marker (radio, box, tick) is "on" when its 8 x 8 interior holds ink over the row ground; off, the interior is all ground.
static void check_radio(int row, bool on, int open = -1, uint16_t ground = kBg, int y0 = kit::kBodyY) {
    unsigned ink = 0;
    for (int y = 2; y < 10; ++y)
        for (int x = 2; x < 10; ++x)
            ink += g_panel[(row_top(row, open, y0) + 7 + y) * LCD_WIDTH + kit::kMarkX + x] != ground;
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
    fill_round({0, 0, 224, 240}, px0, py, pw, kPillH, 3, tint);
    gfx_text({0, 0, 224, 240}, kLabelFont, px0 + 4, py + 1, tag, color, tint);
    unsigned diff = 0;
    for (int yy = y; yy < y + kit::kRowH; ++yy)
        for (int x = px0; x < kit::kTailX; ++x)
            diff += actual[yy * LCD_WIDTH + x] != g_panel[yy * LCD_WIDTH + x];
    if (diff) std::fprintf(stderr, "  pill '%.*s' row %d differs\n", int(tag.size()), tag.data(), row);
    CHECK_EQ(diff, 0);
    std::copy(actual.begin(), actual.end(), g_panel);
}
static void test_selection_and_focus_rendering() {
    fixture(); g_stub.lib = {saved(1), saved(2)};
    Ui ui; open(ui); ui.tick(1200);
    // The Tests step: the focused first row is open (its note), the rail is lit; the preset in force has its radio on.
    check_row_text(kAllRow, "All tests", kText, kFocusT, kAllRow, 150);
    check_radio(kAllRow, true, kAllRow, kFocusT);
    CHECK_EQ(g_panel[(row_top(kAllRow) + 4) * LCD_WIDTH + 1], kFocus);
    check_row_note(kAllRow, "Everything, QC Class B 20V too");
    check_row_text(kPdRow, "PD only", kText, kBg, kAllRow, 150); check_radio(kPdRow, false, kAllRow);
    check_row_text(kLegacyRow, "Legacy only", kText, kBg, kAllRow, 150);
    check_row_text(kCustomRow, "Custom...", kText, kBg, kAllRow, 150); check_radio(kCustomRow, false, kAllRow);
    move(ui, kPdRow, 1220); ui.tick(1220);
    check_row_text(kAllRow, "All tests", kText, kBg, kPdRow, 150); check_radio(kAllRow, true, kPdRow);
    check_row_text(kPdRow, "PD only", kText, kFocusT, kPdRow, 150); check_radio(kPdRow, false, kPdRow, kFocusT);
    press(ui, Button::Ok, 1230); ui.tick(1230);   // a preset pick selects and advances: there is no Next row
    CHECK_EQ(ui.charger_test_selection(), kPdMask); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back, 1235); ui.tick(1240);   // back on the preset row, now with its radio on
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kPdRow);
    check_radio(kPdRow, true, kPdRow, kFocusT); check_radio(kAllRow, false, kPdRow);
    press(ui, Button::Minus, 1245); press(ui, Button::Ok, 1245);   // back to the defaults (and on to the cable)
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    press(ui, Button::Back, 1246); ui.tick(1250);
    check_radio(kAllRow, true, kAllRow, kFocusT);
    custom(ui, 1255); ui.tick(1255);   // the families: Box3 markers, the focused one open with its note
    check_row_text(kFamHuawei, "Huawei / Honor", kText, kFocusT, kFamHuawei, 120); check_radio(kFamHuawei, true, kFamHuawei, kFocusT);
    move(ui, kFamPd, 1260); ui.tick(1260);
    check_row_text(kFamPd, "USB PD", kText, kFocusT, kFamPd, 100); check_radio(kFamPd, true, kFamPd, kFocusT);
    check_row_text(kFamXiaomi, "Xiaomi / Redmi", kDim, kBg, kFamPd, 120);   // mirrors the PD selection, no count of its own
    check_radio(kFamXiaomi, true, kFamPd);
    press(ui, Button::Ok, 1300); ui.tick(1300);   // a short OK toggles the family
    check_radio(kFamPd, false, kFamPd, kFocusT); check_radio(kFamXiaomi, false, kFamPd);
    long_ok(ui, 1310); ui.tick(1310);             // a long OK opens its entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    check_row_text(0, "PD fixed", kText, kFocusT, 0, 150); check_radio(0, false, 0, kFocusT);
    move(ui, 1, 1350); ui.tick(1350);
    check_row_text(1, "PPS", kText, kFocusT, 1, 150); check_radio(1, false, 1, kFocusT);
    press(ui, Button::Ok, 1400); ui.tick(1400);
    check_radio(1, true, 1, kFocusT); check_radio(0, false, 1);
    press(ui, Button::Back, 1450); top(ui, 1450); move(ui, kFamDone, 1450); press(ui, Button::Ok, 1500); ui.tick(1500);
    CHECK_EQ(ui.charger_test_stage(), 2);
    check_row_text(kOffRow, "Off (real cable)", kText, kFocusT, kOffRow, kNoTailRoom);   // the default choice: its radio is on
    check_radio(kOffRow, true, kOffRow, kFocusT);
    move(ui, kSavedRow, 1800); press(ui, Button::Ok, 1800);   // pick the first saved clone: selects and advances to Review
    CHECK_EQ(ui.charger_test_stage(), 4);
    press(ui, Button::Back, 1850); ui.tick(1900);             // the cursor comes back on the cable in force: clone 1
    CHECK_EQ(ui.charger_test_cursor(), kSavedRow);
    check_row_text(kSavedRow, "Host identity 1", kText, kFocusT, kSavedRow, kSavedRoom); check_radio(kSavedRow, true, kSavedRow, kFocusT);
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
    press(ui, Button::Back, 1250); ui.tick(1300);
    CHECK_EQ(ui.charger_test_stage(), 2);
    // The longest saved name is held whole: the dim "saved" word yields its room (a 24 cell name is 192 px of the 193 px
    // between the text column and the scroll rail).
    constexpr int kRoom = 224 - ListView::kBarW - kit::kTextX;
    static_assert(emlib::kNameMax * kSmallAdvPx <= kRoom, "the longest saved name fits the row");
    check_row_text(kSavedRow, "ABCDEFGHIJKLMNOPQRSTUVWX", kText, kFocusT, kSavedRow, kRoom);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        const Rect box = compute_layout(orientation_for(r).edge, kSlotCenters, kSlotLens, kPanelThickness).content;
        const int x = box.x + kit::kTextX * box.w / 224;
        const int y = box.y + wizard_text_y(kSavedRow) * box.h / 240;
        uint16_t lut[16], line[240];
        make_blend_lut(kText, kFocusT, lut);
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
    CHECK_EQ(ui.charger_test_selection(), ct::bit(A::Vivo));
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
        if (mode == 2) ui.on_button(Button::Plus, ButtonEvent::Long, now);
        if (mode == 3) CHECK_EQ(ui.on_button(Button::Back, ButtonEvent::Hold, now), UiAction::None);   // stops the run, no reboot
        CHECK_EQ(g_stub.releases, 1); CHECK_EQ(ui.charger_test_busy(), true);
        CHECK_EQ(ui.spoof_busy(), true);
        for (const uint32_t end = now + 20000; now <= end && ui.charger_test_busy(); now += 100) ui.tick(now);
        CHECK_EQ(ui.charger_test_busy(), false); CHECK_EQ(g_stub.requests, 1);
    }
}
static bool footer_says(std::string_view text, uint16_t color);
static void test_full_help_text() {
    // Each stage's copy: the focused row opens a 12 px dim note under its text; the footer says what is picked.
    fixture(); Ui ui; open(ui); ui.tick(1200);
    static constexpr const char* kPresetNote[] = {"Everything, QC Class B 20V too", "PD fixed, PPS, EPR, Mi 6A",
                                                  "D+/D-: vendor modes, QC, UFCS", "Huawei, Samsung, OPPO, vivo"};
    for (int r = 0; r < 4; ++r) {
        top(ui, 1210); move(ui, r, 1210); ui.tick(1210 + r);
        check_row_note(r, kPresetNote[r]);
    }
    top(ui, 1220); move(ui, kCustomRow, 1220); ui.tick(1220);
    check_row_note(kCustomRow, "Choose families one by one");
    CHECK_EQ(footer_says("Picked: All tests", kText), true);
    press(ui, Button::Ok, 1230); ui.tick(1230);   // Custom...
    CHECK_EQ(ui.charger_test_stage(), kCustomStage);
    check_row_note(kFamHuawei, "Entries inside this family");
    CHECK_EQ(footer_says("6 of 6 families on", kText), true);
    top(ui, 1240); move(ui, kFamXiaomi, 1240); ui.tick(1240);
    check_row_note(kFamXiaomi, "Mi 6A probes run in PD");   // Xiaomi / Redmi rides the PD unit
    move(ui, 1, 1250); long_ok(ui, 1250); ui.tick(1250);    // USB PD's entries
    CHECK_EQ(ui.charger_test_stage(), 1);
    check_row_note(0, "Fixed offers: SPR + EPR");
    CHECK_EQ(footer_says("2 of 2 entries on", kText), true);
    move(ui, 1, 1260); ui.tick(1260);
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
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.list_state().count, 5);
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
        CHECK_EQ(v.charger_test_selection(), kVendorMask); CHECK_EQ(v.charger_test_stage(), 2);   // the pick goes on to the cable
        press(v, Button::Back); v.tick(1300);
        CHECK_EQ(v.charger_test_stage(), 0); CHECK_EQ(v.charger_test_cursor(), kVendorRow);   // BACK: the cursor sits on the pick
        check_row_text(kVendorRow, "Vendor modes", kText, kFocusT, kVendorRow, 150);
        check_radio(kVendorRow, true, kVendorRow, kFocusT); check_radio(kLegacyRow, false, kVendorRow);
    }
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    move(ui, kPdRow); press(ui, Button::Ok);
    CHECK_EQ(ui.charger_test_selection(), kPdMask); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kPdRow);
    move(ui, 1); press(ui, Button::Ok);
    CHECK_EQ(ui.charger_test_selection(), kLegacyMask);
    press(ui, Button::Back); CHECK_EQ(ui.charger_test_cursor(), kLegacyRow);
    ui.tick(1300);
    check_radio(kLegacyRow, true, kLegacyRow, kFocusT); check_radio(kPdRow, false, kLegacyRow);
    top(ui); move(ui, kAllRow); press(ui, Button::Ok);
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms);
    // The same preset again changes nothing (no toggle: a preset is a choice, not a switch).
    press(ui, Button::Back); press(ui, Button::Ok);
    CHECK_EQ(ui.charger_test_selection(), ct::kDefaultAlgorithms); CHECK_EQ(ui.charger_test_stage(), 2);
    press(ui, Button::Back);
    // A hand-made mix is no preset: the radio moves to "Custom...".
    custom(ui, 1400); open_group(ui, kFamPd, 1400); press(ui, Button::Ok, 1400);   // PD fixed off
    press(ui, Button::Back); press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 0); CHECK_EQ(ui.charger_test_cursor(), kCustomRow);
    CHECK_EQ(ui.charger_test_selection() != ct::kDefaultAlgorithms, true);
    ui.tick(1500);
    check_radio(kCustomRow, true, kCustomRow, kFocusT); check_radio(kAllRow, false, kCustomRow);
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
static void check_header(std::string_view title, int step) {
    // The masthead title: font 16 at x8, or font 12 when it would run into the SNK badge (the stub is self-powered).
    constexpr int kRoomBeforeSnk = 132 - 3 * kLabelAdv - 5 - 4 - kMastTitleX;
    if (text_width(kSmall, title) <= kRoomBeforeSnk) check_text_at(kSmall, kMastTitleY, title, kInk, kBar, kMastTitleX);
    else check_text_at(kLabelFont, kBarTextY, title, kInk, kBar, kMastTitleX);
    for (int i = 0; i < 3; ++i)
        CHECK_EQ(g_panel[kit::kProgY * LCD_WIDTH + i * kit::kProgPitch + 5], i < step ? kFocus : kRule);
}
static void test_three_step_header() {
    fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); ui.tick(1200);
    check_header("1/3 Tests", 1);
    custom(ui, 1300); ui.tick(1300);
    check_header("1/3 Custom", 1);
    open_group(ui, kFamSamsung, 1310); ui.tick(1320);
    check_header("1/3 Entries", 1);   // a family's page is still step 1
    press(ui, Button::Back, 1330); top(ui, 1330); move(ui, kFamDone, 1330); press(ui, Button::Ok, 1330); ui.tick(1340);
    CHECK_EQ(ui.charger_test_stage(), 2);
    check_header("2/3 Spoof cable", 2);
    press(ui, Button::Ok, 1350); ui.tick(1360);
    CHECK_EQ(ui.charger_test_stage(), 4);
    check_header("3/3 Review", 3);
}
// Review's Cable row (row 2 of the table, the value column at x72) is the identity the START presents. The armed state is not
// part of the line: it is on the Cable step (the ARMED pill) and in the Refuse / Waiting stages.
static void check_review_spoof(std::string_view text, uint16_t color) {
    char buf[kFitBuf];
    const int y = kit::kBodyY + 2 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height);
    check_text_at(kSmall, y, "Spoof", kDim, kBg, 6);   // the key of the identity row
    check_text_at(kSmall, y, fit_data(kSmall, text, 18 * kSmall.adv, buf), color, kBg, 72);
}
// The face name of a built-in chip ("CUKTECH 6A" -> "CUKTECH"): the rating is the dim second column of its row.
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
    ui.tick(1310);
    constexpr int kSpoofY = SpoofPage::kListY;
    check_row_text(0, "Off (real cable)", kText, kFocusT, -1, kNoTailRoom, kSpoofY);   // the default selection, the cursor row
    press(ui, Button::Plus, 1320); press(ui, Button::Plus, 1330);               // the second built-in chip
    press(ui, Button::Ok, 1340);                                                // Spoof arms at once
    CHECK_EQ(g_stub.spoof_on, true);
    ui.tick(1350);
    check_row_text(2, name, kCaution, kFocusT, -1, kit::kCol2X - kit::kTextX, kSpoofY);
    check_row_pill(2, "ARMED", kCaution, kCautionT, -1, kSpoofY);                // the Spoof view: ARMED only, never CURR/NEXT
    ui.on_button(Button::Back, ButtonEvent::Short, 1400);                       // back to the charger test
    ui.tick(1410);
    cable(ui, 1500); ui.tick(1510);                                             // the cursor starts on the identity in force
    check_row_text(kBuiltinRow + 1, name, kCaution, kFocusT, kBuiltinRow + 1, kit::kCol2X - kit::kTextX);   // the same row, the same colour
    check_row_pill(kBuiltinRow + 1, "ARMED", kCaution, kCautionT, kBuiltinRow + 1);   // pick == armed: ARMED on the Cable step too
    // Picking another identity on the wizard only selects: the armed row wears CURR, the pick NEXT.
    pick_row(ui, kBuiltinRow + 2, 1600);
    CHECK_EQ(ui.charger_test_stage(), 4);
    press(ui, Button::Back, 1650); ui.tick(1660);
    CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow + 2);
    CHECK_EQ(g_stub.spoof_arms, 1);
    check_row_text(kBuiltinRow + 1, name, kCaution, kBg, kBuiltinRow + 2, kit::kCol2X - kit::kTextX);
    check_row_pill(kBuiltinRow + 1, "CURR", kCaution, kDimT, kBuiltinRow + 2);
    check_row_text(kBuiltinRow + 2, face_name(emrec::builtin_info(4).label), kOk, kFocusT, kBuiltinRow + 2, kit::kCol2X - kit::kTextX);
    check_row_pill(kBuiltinRow + 2, "NEXT", kOk, kOkT, kBuiltinRow + 2);
}
// The footer message line (what a refused START says) as the panel shows it.
static bool footer_says(std::string_view text, uint16_t color) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> actual;
    std::copy_n(g_panel, actual.size(), actual.begin());
    const FooterStyle st = footer_style(footer_tone_of(color));   // the band: a tone's fill, 16 px text from x4, one row under the band top
    gfx_text({0, 0, 224, 240}, kSmall, kFooterTextX, kFooterY + 1, text, st.text, st.fill);
    unsigned diff = 0;
    for (int y = kFooterY + 1; y < kFooterY + 1 + kSmall.height; ++y)
        for (int x = kFooterTextX; x < kFooterTextX + text_width(kSmall, text); ++x)
            diff += actual[y * LCD_WIDTH + x] != g_panel[y * LCD_WIDTH + x];
    return diff == 0;
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
        else { cable(ui, 1500); pick_row(ui, kOffRow, 1500); top(ui, 1500); press(ui, Button::Ok, 1500); ui.tick(1510); check_review_spoof("Off", kText); }
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
        const auto passes = [&] { unsigned n = 0; for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass; return n; };
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
    ui.tick(1000000);
    CHECK_EQ(ui.charger_test_cursor(), 0); CHECK_EQ(ui.list_state().offset, 0);
    CHECK_EQ(lines > ui.list_state().visible, true);   // the case under test: more lines than the grid shows (it scrolls by pixel height)
    uint32_t t = 1000100;
    for (int i = 1; i < lines; ++i) {
        const int was = ui.list_state().offset, shown = ui.list_state().visible;
        press(ui, Button::Plus); ui.tick(t += 100);
        CHECK_EQ(ui.charger_test_cursor(), i);                                       // one line per press
        const auto s = ui.list_state();
        CHECK_EQ(i >= s.offset && i < s.offset + s.visible, true);                   // the selected line is always on screen
        if (i < was + shown) CHECK_EQ(s.offset, was);                                // scrolls only when it would leave the view
        CHECK_EQ(s.offset >= was, true);
    }
    for (int i = lines - 2; i >= 0; --i) {
        const int was = ui.list_state().offset;
        press(ui, Button::Minus); ui.tick(t += 100);
        CHECK_EQ(ui.charger_test_cursor(), i);
        const auto s = ui.list_state();
        CHECK_EQ(i >= s.offset && i < s.offset + s.visible, true);
        CHECK_EQ(s.offset <= was, true);                                             // and never scrolls down going up
    }
    CHECK_EQ(ui.list_state().offset, 0);
}
// The Results list repaints incrementally (one slot per changed line). After every scroll the panel must equal a forced
// full repaint of the same state, so no chip fragment of an earlier line may survive at its slot.
static void settle_ms(Ui& ui, uint32_t& t) { for (int i = 0; i < 4; ++i) { t += 100; ui.tick(t); } }   // the scrollbar thumb glides 160 ms
static unsigned panel_diff_vs_forced_repaint(Ui& ui, uint32_t now) {
    std::vector<uint16_t> incremental(g_panel, g_panel + LCD_WIDTH * LCD_HEIGHT);
    ui.render_for_capture(now);
    unsigned diff = 0;
    for (unsigned i = 0; i < incremental.size(); ++i) diff += incremental[i] != g_panel[i];
    return diff;
}
static void results_incremental_repaint_matches_forced(uint8_t rot) {
    fixture();
    const unsigned fixed_mv[] = {5000, 9000, 12000, 15000, 20000};
    g_stub.caps.num_pdos = 6;
    for (unsigned i = 0; i < 5; ++i) g_stub.caps.pdos[i] = pd::decode_pdo(((fixed_mv[i] / 50) << 10) | 300);
    g_stub.caps.pdos[5] = pd::decode_pdo(0xc0000000 | (110u << 17) | (33u << 8) | 100);
    Ui ui; open(ui); ui.set_rotation(rot, 1050); review(ui);
    start_press(ui, 1300); press(ui, Button::Ok, 1400);
    uint32_t t = 1500;
    for (; t <= 900000 && ui.charger_test_busy(); t += 100) ui.tick(t);
    CHECK_EQ(ui.charger_test_stage(), 6);
    ui.tick(t); t += 200; ui.tick(t); t += 200;
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
    press(ui, Button::Plus); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Minus); CHECK_EQ(ui.charger_test_cursor(), 0);
    press(ui, Button::Back);
    CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), 0);
}
// A self-powered K2 says on Review, in one line, that the run keeps VBUS up; a HID-powered one keeps its old note.
static void test_self_powered_review_note() {
    for (unsigned self = 0; self < 2; ++self) {
        fixture(); g_stub.self_powered = self != 0;
        Ui ui; open(ui); review(ui);
        CHECK_EQ(footer_says(self ? "Self-powered: VBUS stays up" : "Runs without a load", kText), true);
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
    CHECK_EQ(footer_says("VBUS 8.5 V on a 5V contract", kCaution), true);   // red today: "VBUS not safe 5V"
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
    check_row_text(2, "AVS sweep", kText, kFocusT, 2, 150);        // red today: row 2 reads "PPS"
    press(ui, Button::Ok, 1320); ui.tick(1320);                    // toggle the sweep row
    CHECK_EQ(ui.charger_test_selection() & (1u << 30), 1u << 30);   // red today: row 2 toggles the PPS bit, not the AVS flag
}
static void test_results_counters_are_words() {
    for (unsigned kind = 0; kind < 3; ++kind) {
        fixture();
        g_stub.caps.spec_rev = kind == 0 ? 1 : 2;
        if (kind == 2) g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 1};
        Ui ui; open(ui); only_vivo_review(ui);   // only VIVO
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        g_stub.source_revision = {.valid = true, .rev_major = 3, .rev_minor = 2};
        // The charger keeps talking PD after the Legacy attach: one try, one longer retry, then every row of the unit skips.
        g_stub.pd_chatter = true;
        for (uint32_t t = 1500; t <= 120000; t += 100) ui.tick(t);
        CHECK_EQ(ui.charger_test_stage(), 6);
        // The counters strip (y 22..46, three 75 px columns) says the totals as words in the verdict colours: "0 pass  0 fail
        // 4 skip", 16 px text at x = 16 + 75 * column after the verdict shape (the fail count stays kError even at 0).
        const struct { int x; std::string_view text; uint16_t color; } words[] = {
            {16, "0 pass", kOk}, {16 + 75, "0 fail", kError}, {16 + 150, "4 skip", kDim}};
        for (const auto& w : words) {
            check_text_at(kSmall, 26, w.text, w.color, kBg, w.x);
        }
        no_operations();
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
}
// The Results grid (kit ResultGrid from y 72): the focused unit lights the 60 px label strip (3 px rail + tint) and its first
// verdict cell is a flat pass fill; a footer explains the focused unit (it follows the selection).
static void test_results_grid_rows_rail_and_footer() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    CHECK_EQ(ui.list_state().count > 8, true);
    CHECK_EQ(g_panel[70 * LCD_WIDTH + 100] == kRule && g_panel[71 * LCD_WIDTH + 100] == kRule, true);   // the 2 px rule over the grid
    CHECK_EQ(g_panel[72 * LCD_WIDTH + 1] == kFocus, true);                                           // rail on the focused row
    CHECK_EQ(g_panel[72 * LCD_WIDTH + 30] == kFocusT, true);                                         // and its tint
    CHECK_EQ(g_panel[72 * LCD_WIDTH + kit::kResultLabelW] == kBg, true);                            // no label rule: white space
    CHECK_EQ(g_panel[73 * LCD_WIDTH + kit::kResultLabelW + 2] == kOkT, true);                        // a passing cell: the flat pass fill
    const auto footer_ink = [&] {
        std::vector<uint16_t> f;
        for (int y = kFooterY; y < 240; ++y) f.insert(f.end(), g_panel + y * LCD_WIDTH, g_panel + y * LCD_WIDTH + 224);
        return f;
    };
    const auto first = footer_ink();
    CHECK_EQ(std::count(first.begin(), first.end(), first.front()) < static_cast<long>(first.size()), true);   // it says something
    move(ui, 2); ui.tick(t + 100);
    CHECK_EQ(footer_ink() != first, true);   // and says it about the new focused unit
    CHECK_EQ(ui.charger_test_stage(), 6);
}
// A verdict cell is one flat fill with its text centred on it: across the first row's cells the fill never breaks except for the
// 1 px gap between neighbours, and the cells stop short of the right edge.
static void test_results_cells_are_flat_fills() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    unsigned fill = 0, gaps = 0;
    int last_fill = 0;
    for (int x = kit::kResultLabelW + 1; x < 224; ++x) {
        const uint16_t p = g_panel[73 * LCD_WIDTH + x];   // the cell's top line: no glyph reaches it
        if (p == kOkT || p == kError) { ++fill; last_fill = x; }   // a pass fill or a fail fill
    }
    // The first unit's 9 V / 12 V rows fail on this stub (VBUS stays 5 V) and the row behind a failed retarget is skipped, which
    // paints no fill: count the gap columns up to the last filled cell only.
    for (int x = kit::kResultLabelW + 1; x < last_fill; ++x) gaps += g_panel[73 * LCD_WIDTH + x] == kBg;
    CHECK_EQ(fill > 100, true);
    CHECK_EQ(gaps >= 1 && gaps < 12, true);
    CHECK_EQ(g_panel[73 * LCD_WIDTH + 223] == kBg || g_panel[73 * LCD_WIDTH + 223] == kOkT, true);
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
    for (size_t i = 0; i < l.rows.size(); ++i) if (!l.rows[i].header && l.rows[i].algorithm == ct::Algorithm::Pps) { pps = static_cast<int>(i); break; }
    CHECK_EQ(pps >= 0, true);
    // Focus the PPS row: its footer says how far the converter got, in the caution tone (the chip's own words stay "5-21V 3.2A").
    StrBuf<32> want;
    want.put(l.rows[static_cast<size_t>(pps)].label).put(": caps 19.4V of 21V");
    bool found = false;
    top(ui, t);
    for (int n = 0; n < 24 && !found; ++n) {
        found = footer_says(want.view(), kCaution);
        if (!found) { press(ui, Button::Plus); ui.tick(t += 100); }
    }
    CHECK_EQ(found, true);
    // The chip: an amber tint with the bar under its text.
    unsigned tint = 0, bar = 0;
    for (int y = 72; y < kFooterY; ++y)
        for (int x = kit::kResultLabelW + 1; x < 224; ++x) {
            const uint16_t p = g_panel[y * LCD_WIDTH + x];
            tint += p == kCautionT; bar += p == kCaution;
        }
    CHECK_EQ(tint > 50, true);
    CHECK_EQ(bar > 20, true);
    // Steps of the row, then the capped step's Detail: an amber band reading CAPPED, and "Reached" where the window was.
    press(ui, Button::Ok); ui.tick(t += 100);
    CHECK_EQ(ui.charger_test_stage(), 7);   // Steps
    bool detail = false;
    for (int n = 0; n < 12 && !detail; ++n) {
        press(ui, Button::Ok); ui.tick(t += 100);
        if (ui.charger_test_stage() == 8 && g_panel[24 * LCD_WIDTH + 100] == kCautionT) detail = true;
        else { press(ui, Button::Back); ui.tick(t += 100); press(ui, Button::Plus); ui.tick(t += 100); }
    }
    CHECK_EQ(detail, true);
    if (!detail) return;
    check_text_at(kMid, 28, "CAPPED", kCaution, kCautionT, 32);
    const int ty = 60 + 2 * kit::kRowH + kit::row_text_y(kit::kRowH, kSmall.height);   // the third row of the Detail table (x6 label, x92 value)
    check_text_at(kSmall, ty, "Reached", kDim, kBg, 6);
    check_text_at(kSmall, ty, "3.3-19.4V of 21V", kCaution, kBg, 92);   // the PPS range starts at 3.3 V: 16 cells, the full value column
}
// Every Steps row reads as a whole step label (step_label::label), never cut with dots: the ink of each visible row's text
// cell is compared with the ink of every label of the run, and some label must match. The cut would end in "...". The same scan
// found two repaint bugs in a list that scrolls under a fixed highlight or over a longer verdict (stale cells of the row before).
static void test_steps_rows_show_whole_labels() {
    six_pdo_fixture(); Ui ui; uint32_t t;
    finished_results(ui, t);
    const auto steps = ui.charger_test_results().steps();
    unsigned rows_checked = 0, ramps = 0, failed_rows = 0;
    // The Steps rows are kit rows: a Verdict marker, the label at x28 (16 px, up to the voltage tail), the measured voltage right
    // aligned at x214. A row is a whole label when some step of the run (label + its own voltage tail) reproduces its ink.
    const auto check_window = [&] {
        const int offset = ui.list_state().offset, shown = ui.list_state().visible;
        const int open = ui.charger_test_cursor() - offset;
        const int w = kit::kTailX - 40 - 8 - kit::kTextX;   // the room of a 5-character voltage tail ("20.1V")
        for (int slot = 0; slot < shown && offset + slot < ui.list_state().count; ++slot) {
            const int y = wizard_text_y(slot, open);
            const uint16_t bg = g_panel[y * LCD_WIDTH + kit::kMarkX + kit::kMarkSize + 2];   // clear of the marker and the text
            std::vector<uint8_t> seen;
            for (int yy = y; yy < y + kSmall.height; ++yy)
                for (int x = kit::kTextX; x < kit::kTextX + w; ++x) seen.push_back(g_panel[yy * LCD_WIDTH + x] != bg);
            if (std::count(seen.begin(), seen.end(), 1) == 0) continue;   // a blank row below the last step
            std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> keep;
            std::copy_n(g_panel, keep.size(), keep.begin());
            bool matched = false;
            for (size_t i = 0; i < steps.size() && !matched; ++i) {
                const auto text = step_label::label(steps[i], i ? &steps[i - 1] : nullptr);
                gfx_fill({0, 0, 224, 240}, kit::kTextX, y, w, kSmall.height, bg);
                gfx_text({0, 0, 224, 240}, kSmall, kit::kTextX, y, text.view(), kText, bg);
                std::vector<uint8_t> want;
                for (int yy = y; yy < y + kSmall.height; ++yy)
                    for (int x = kit::kTextX; x < kit::kTextX + w; ++x) want.push_back(g_panel[yy * LCD_WIDTH + x] != bg);
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
        ui.tick(t); t += 200;
        for (int window = 0; window < 4; ++window) {   // the list scrolls a window at a time
            check_window();
            move(ui, 6, t);
            for (int i = 0; i < 4; ++i) { ui.tick(t); t += 200; }
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
        move(ui, kPdRow); press(ui, Button::Ok);   // "PD only": the PD fixed / PPS / Mi 6A rows; a preset pick goes on to the cable
        CHECK_EQ(ui.charger_test_stage(), 2);
        press(ui, Button::Ok);                     // the cable pick keeps "Off" and goes on to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        char name[48];
        const auto shot = [&](const char* scene, uint32_t at) {
            ui.tick(at);
            std::snprintf(name, sizeof name, "labels_rot%u_%s", unsigned(rot), scene);
            dump_panel(name);
        };
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
        shot("running", t);
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
}
static ufcs::Cap ufcs_mode_cap(uint8_t pos, int lo_mv, int hi_mv, int lo_ma, int hi_ma) {
    ufcs::Cap c; c.pos = pos; c.min_mv = lo_mv; c.max_mv = hi_mv; c.min_ma = lo_ma; c.max_ma = hi_ma; return c;
}
// The Detail page of the first sampled UFCS step: the "Mode" row (row 4 of the table, value column x92) reads the whole mode.
static void check_ufcs_detail(Ui& ui, uint32_t& t, std::string_view mode_text, const char* shot) {
    top(ui, t); press(ui, Button::Ok, t); ui.tick(t); t += 200;
    CHECK_EQ(ui.charger_test_stage(), 7);
    press(ui, Button::Ok, t); ui.tick(t); t += 200;
    CHECK_EQ(ui.charger_test_stage(), 8);
    constexpr kit::KvLayout kDetail{60, kit::kRowH, 6, nullptr, nullptr, 92, 16, Align::Left};   // as ChargerTestPage::paint_detail
    const int y = kit::kv_row_y(kDetail, 4) + kit::row_text_y(kit::kRowH, kSmall.height);
    check_text_at(kSmall, y, mode_text, kText, kBg, 92);
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
        unsigned with_amps = 0;
        for (const auto& tk : l.tokens) if (std::string_view(tk.text).find("3A") != std::string_view::npos) ++with_amps;
        CHECK_EQ(with_amps >= 3, true);
        dump_panel("ufcs_cuktech_results");
        check_ufcs_detail(ui, t, "5-20V 1-3A", "cuktech");   // the 5 V baseline is judged in the last mode that reaches 5 V
    }
    { // FH22-like: one 5.5-11 V mode at 4 A (44 W)
        ufcs_caps_fixture({ufcs_mode_cap(1, 5500, 11000, 1000, 4000)});
        Ui ui; uint32_t t; ufcs_run(ui, t, "fh22");
        result_layout::Layout l; result_layout::build(l, ui.charger_test_results().steps());
        bool found = false;
        for (const auto& tk : l.tokens) found |= std::string_view(tk.text).find("11V 4A") != std::string_view::npos;
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
    check_text_at(kSmall, 51, lines[0].text, kText, kBg, 6);
    move(ui, 2); ui.tick(t + 100);
    check_text_at(kSmall, 51, lines[0].text, kText, kBg, 6);
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
        ui.tick(9200);
        const Rect box = compute_layout(orientation_for(0).edge, kSlotCenters, kSlotLens, kPanelThickness).content;
        auto px = [&](int x, int y) { return g_panel[(box.y + y) * LCD_WIDTH + box.x + x]; };
        CHECK_EQ(box.w, 224); CHECK_EQ(box.h, 240);
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
    ui.tick(1600);
    CHECK_EQ(ui.list_state().count, kSavedRow + 2);      // Off, three built-ins, two saved clones
    CHECK_EQ(ui.charger_test_cursor(), 0);               // the cursor sits on the pick in force
    check_row_text(0, "Off (real cable)", kText, kFocusT, 0, kNoTailRoom); check_radio(0, true, 0, kFocusT);
    static constexpr const char* kShort[] = {"EPR", "CUKTECH", "OnePlus"};
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
            for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass;
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
        static constexpr const char* kShort[] = {"EPR", "CUKTECH", "OnePlus"};   // the Cable step's short names
        const int row = kBuiltinRow + int(r);
        pick_row(ui, row, 1300);                         // a pick selects and goes on to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        press(ui, Button::Back, 1340); ui.tick(1350);    // back to the list: the cursor is on the pick, the radio is on
        CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), row);
        check_radio(row, true, row, kFocusT);
        check_row_text(row, kShort[r], kText, kFocusT, row, kit::kCol2X - 4 - kit::kTextX);
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
    CHECK_EQ(ui.list_state().count, 14); CHECK_EQ(ui.list_state().visible, 7); CHECK_EQ(ui.list_state().offset, 0);
    ui.tick(1350);
    check_row_text(0, "Off (real cable)", kText, kFocusT, 0, kNoTailRoom);   // the cursor starts on the pick in force (Off), its note open
    check_row_text(3, "OnePlus", kText, kBg, 0, kit::kCol2X - 4 - kit::kTextX);
    move(ui, 7, 1400);                                   // the cursor leaves the first page: it scrolls by one row at a time
    CHECK_EQ(ui.charger_test_cursor(), 7); CHECK_EQ(ui.list_state().offset, 1);
    move(ui, 7, 1400);
    CHECK_EQ(ui.charger_test_cursor(), 13); CHECK_EQ(ui.list_state().offset, 7);   // the last row is the bottom row
    ui.tick(1500);
    check_row_text(0, "Host identity 4", kText, kBg, 6, kSavedRoom);    // names come from the cache filled when the window moved
    check_row_text(5, "Host identity 9", kText, kBg, 6, kSavedRoom);
    check_row_text(6, "Host identity 10", kText, kFocusT, 6, kSavedRoom);
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
            for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass;
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
        r.options.selected = ui.charger_test_selection(); r.options.units = g_stub.journal_meta.units;
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
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); ui.tick(1100);
        cable(ui, 1200); ui.tick(1210);
        CHECK_EQ(ui.charger_test_stage(), 2); CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow);
        check_row_pill(kBuiltinRow, "ARMED", kCaution, kCautionT, kBuiltinRow);   // pick == armed
        CHECK_EQ(ui.charger_test_set_cable(kOff), true);   // pick Off while the boot identity answers
        ui.tick(1300);
        CHECK_EQ(ui.charger_test_cursor(), kOffRow);
        check_row_text(kOffRow, "Off (real cable)", kOk, kFocusT, kOffRow, kNextPillRoom);
        check_row_pill(kOffRow, "NEXT", kOk, kOkT, kOffRow);
        check_row_pill(kBuiltinRow, "CURR", kCaution, kDimT, kOffRow);
        press(ui, Button::Ok, 1400);                    // OK on the Off row: selects and advances to Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        ui.tick(1410); check_review_spoof("Off", kText);
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
        ui.goto_page(static_cast<uint8_t>(Page::ChargerTest), 1100); ui.tick(1100);
        cable(ui, 1200); ui.tick(1210);
        const Choice cuck{Kind::Builtin, 2};            // CUKTECH, row kBuiltinRow + 1
        CHECK_EQ(ui.charger_test_set_cable(cuck), true);
        ui.tick(1300);
        CHECK_EQ(ui.charger_test_cursor(), kBuiltinRow + 1);
        check_row_pill(kBuiltinRow + 1, "NEXT", kOk, kOkT, kBuiltinRow + 1);
        check_row_pill(kBuiltinRow, "CURR", kCaution, kDimT, kBuiltinRow + 1);
        press(ui, Button::Ok, 1400);                    // OK on the pick: Review
        CHECK_EQ(ui.charger_test_stage(), 4);
        ui.tick(1410); check_review_spoof(std::string(emrec::builtin_info(2).label), kText);
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
// A saved clone is remembered by its library id; one that is gone by the next boot reads as Off (and the stale record is
// replaced, so a later clone that takes the number is not picked up by accident).
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
    CHECK_EQ(ui.charger_test_cable_info().choice == kOff, true);
    CHECK_EQ(g_stub.spoof_arms + g_stub.ra_ons, 0);
    CHECK_EQ(int(bootra::last()), int(bootra::Verdict::NoIdentity));
    CHECK_EQ(g_stub.cable_pref == kOff, true);
    CHECK_EQ(g_stub.cable_pref_sets, sets + 1);
    reboot_with(true);
    Ui again; boot_ui(again);
    CHECK_EQ(g_stub.cable_pref_sets, sets + 1);                         // Off is stable: no write at the next boot
}
// A clone deleted while the K2 runs drops the selection to Off in the list; the record follows.
static void test_a_clone_deleted_while_running_clears_the_record() {
    fixture(); g_stub.lib = {saved(1)};
    Ui ui; open(ui); CHECK_EQ(ui.charger_test_set_cable(kSaved1), true);
    CHECK_EQ(g_stub.cable_pref == kSaved1, true);
    g_stub.lib.clear(); g_stub.lib_removes++;
    cable(ui);                                                          // the Cable step re-reads the library
    CHECK_EQ(ui.charger_test_cable_info().choice == kOff, true);
    CHECK_EQ(g_stub.cable_pref == kOff, true);
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
        for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass;
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
        for (const auto& s : ui.charger_test_results().steps()) n += s.verdict == ct::Verdict::Pass;
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
constexpr int kGridTop = 114, kGridBottom = 222, kGridCellsX = 60;   // the verdict grid's rows, and where its cells start
// The Running cell's dot: a solid kRunDotSize square of kFocus. (kFocus is also the amps cyan, so a pixel is not enough: text never
// makes a solid 5 x 5 block.) The top-left corners of every such block in the grid window, in reading order.
static std::vector<std::pair<int, int>> running_dots(int x0 = kGridCellsX, int y0 = kGridTop, int y1 = kGridBottom) {
    std::vector<std::pair<int, int>> out;
    constexpr int d = k2::kit::kRunDotSize;
    for (int y = y0; y + d <= y1; ++y)
        for (int x = x0; x + d <= 224; ++x) {
            bool solid = true;
            for (int j = 0; j < d && solid; ++j)
                for (int i = 0; i < d; ++i)
                    if (g_panel[(y + j) * LCD_WIDTH + x + i] != kFocus) { solid = false; break; }
            if (solid) out.emplace_back(x, y);
        }
    return out;
}
// Is a Running cell (the one with the dot) on screen in the grid window?
static bool running_cell_visible() { return !running_dots().empty(); }
// The running cell's rows in the grid: the dot sits on the cell's top edge, the cell is a verdict cell high (25 px at most).
// False = not on screen.
static bool running_cell_rows(int& top, int& bottom) {
    const auto dots = running_dots();
    if (dots.empty()) return false;
    top = dots.front().second; bottom = top + 25;
    return true;
}
// How many pixels above `y` in the grid wear the Pending ink (a chip that has not run yet).
static unsigned pending_ink_above(int y) {
    unsigned n = 0;
    for (int yy = kGridTop; yy < y && yy < kGridBottom; ++yy)
        for (int x = kGridCellsX; x < 224; ++x) n += g_panel[yy * LCD_WIDTH + x] == kFaint;
    return n;
}
static std::string_view handshake_words(int proto);   // below: the footer line a handshake row announces
// In flight a tick paints at most the budget: kFlightPx for the items together, kFlightRowPx for one grid row on an otherwise
// idle tick. Between steps (no step in flight) there is no cap. The one exception is the tick that starts a handshake row: it
// also paints the footer line naming the window (kAnnouncePx), which is not counted here.
static void test_a_row_in_flight_paints_inside_the_budget() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; unsigned flight_ticks = 0, max_px = 0, settled_ticks = 0, max_settled_grid = 0, max_settled_px = 0;
    unsigned prev_rev = ~0u, prev_idx = ~0u, same = 0;
    std::vector<uint16_t> before(LCD_WIDTH * LCD_HEIGHT);
    for (; now <= 60000 && ui.charger_test_busy(); now += 100) {
        std::copy_n(g_panel, before.size(), before.begin());
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
        if (same < 5) continue;
        // Five ticks into a step (the layout, the scroll and the deferred rows have all settled): only the hero, its needle, the
        // clock and the footer text change. The grid area sees nothing: the running cell's dot is steady.
        ++settled_ticks; max_settled_px = std::max(max_settled_px, px);
        unsigned grid = 0;
        for (int y = kGridTop; y < kGridBottom; ++y)
            for (int x = 0; x < 224; ++x) grid += before[y * LCD_WIDTH + x] != g_panel[y * LCD_WIDTH + x];
        max_settled_grid = std::max(max_settled_grid, grid);
    }
    CHECK_EQ(flight_ticks > 20, true); CHECK_EQ(settled_ticks > 10, true);
    CHECK_EQ(max_px <= static_cast<unsigned>(ChargerTestPage::kFlightRowPx), true);   // a tick never exceeds the tallest row
    CHECK_EQ(max_settled_px <= static_cast<unsigned>(ChargerTestPage::kFlightPx), true);
    CHECK_EQ(max_settled_grid, 0u);   // a steady dot: five ticks into a step the grid area does not change at all
    std::printf("[flight] ticks=%u max_px=%u settled=%u max_settled_px=%u max_settled_grid_changed=%u\n", flight_ticks, max_px, settled_ticks, max_settled_px, max_settled_grid);
}
// The idle hook does not serve an engine window (HS pulses, VOOC slices, UFCS / VIVO frames): there the page paints nothing.
static void test_a_row_in_an_engine_window_paints_nothing() {
    Ui ui; start_all_run(ui);
    uint32_t now = 1500; unsigned blocked = 0, free_px_ticks = 0;
    for (; now <= 40000 && ui.charger_test_busy(); now += 100) {
        g_stub.fc_critical = (now / 1000) % 2 == 0;   // every other second an engine window is open
        const unsigned px0 = g_lcd_pixels, starts0 = g_stub.fc_starts;
        ui.tick(now);
        if (ui.charger_test_stage() != kRunningStage || !ui.charger_test_results().quiet()) continue;
        // The tick that starts a handshake row paints its footer line (kAnnouncePx) before the engine starts: the forced window
        // here opens ahead of the start, which a real engine window never does.
        const unsigned announce = g_stub.fc_starts != starts0 && !handshake_words(g_stub.fc_proto).empty() ? ChargerTestPage::kAnnouncePx : 0;
        if (g_stub.fc_critical) { ++blocked; CHECK_EQ(g_lcd_pixels - px0 - announce, 0u); }
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
static bool footer_shows(std::string_view text) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> keep;
    std::copy_n(g_panel, keep.size(), keep.begin());
    const bool shown = footer_says(text, kText);
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
        const unsigned starts0 = g_stub.fc_starts;
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
        const unsigned starts0 = g_stub.fc_starts, px0 = g_lcd_pixels;
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
            const unsigned pend = pending_ink_above(top);            if (pend) ++pending_above;
            const unsigned total = static_cast<unsigned>(r.steps().size());
            if (!early_done) { dump_screen("follow_early"); early_done = true; }
            if (!mid_done && r.index() >= total / 2) { dump_screen("follow_mid"); mid_done = true; }
            if (r.index() + 6 >= total) dump_screen("follow_end");   // the last one followed wins
            if (top > kGridTop + 60) ++rows_above;   // the window holds finished rows above the running row
        }
        else if (++lag > 8) { ++missed; waiting = false; }   // a budgeted row at a time: never more than 8 ticks
    }
    // A step whose chip the layout merged into one already decided (the same volts at another current) has no cell of its own to
    // light: its row is on screen all the same. Those are the rare ones.
    for (unsigned k = 0; k < 5; ++k) ui.tick(now += 100);
    dump_screen("results_end");   // K2_DUMP: the finished run's Results grid, the skipped units struck out
    if (ui.charger_test_stage() == 6) {
        for (int k = 0; k < 5; ++k) {
            move(ui, 3, now += 100); ui.tick(now += 100); ui.tick(now += 100);
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
    for (int x = 0; x < 224; ++x)
        for (int y = 98; y < 98 + k2::kit::kPassBandH; ++y)
            if (g_panel[y * LCD_WIDTH + x] == kOk) { if (lo < 0) lo = x; hi = x; break; }
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
    unsigned moves = 0, band_stale = 0, dot_stale = 0, double_dots = 0, off_screen = 0, ticks = 0;
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
            if (band == prev_band) ++band_stale;
            if (dots == prev_dots) { if (dots.empty()) ++off_screen; else ++dot_stale; }
        }
        prev_idx = at; prev_mv = mv; prev_band = band; prev_dots = dots;
    }
    g_stub.fc_start_fail = 0;
    CHECK_EQ(ticks > 20, true); CHECK_EQ(moves > 10, true);
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
        ui.tick(1500);
        check_row_text(0, "Start anyway", kCaution, kFocusT, -1, 150, kRefuseY);
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
    ui.tick(1500);
    check_text_at(kMid, 22 + (kit::kAlertH - kMid.height) / 2, "Cable connected", kBg, kCaution, 36);   // the alert band's word
    check_text_at(kSmall, 64, "the spoof won't reach", kText, kBg, 6);   // the two explanation lines
    check_text_at(kSmall, 84, "the charger.", kText, kBg, 6);
    CHECK_EQ(ui.list_state().count, 2);
    check_row_text(0, "Start anyway", kCaution, kFocusT, -1, 150, kRefuseY);
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
        ui.tick(1500);
        check_row_text(0, "Start anyway", kCaution, kFocusT, -1, 150, kRefuseY);
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
        ui.tick(1700);
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
    // A hold of + on another stage keeps its old meaning: long + pages on.
    ui.on_button(Button::Plus, ButtonEvent::Long, t + 100);
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
    ui.tick(1600);
    CHECK_EQ(footer_says("No VBUS. Hold + few secs", kText), true);   // no schedule yet: the older hint
    g_stub.cc = 1; g_stub.trial_line = 1; g_stub.trial_moves = 0; g_stub.trial_live = true;
    ui.tick(1700);
    CHECK_EQ(footer_says("CC1 picked. Waking charger", kText), true);
    check_text_at(kSmall, 146, "Rd on CC1", kText, kBg, 28);
    dump_screen("wait_line_picked");   // K2_DUMP: the screen for the design review (runs/ui-wait-line)
    g_stub.cc = 2; g_stub.trial_line = 2; g_stub.trial_moves = 1;   // one move made
    ui.tick(1800);
    CHECK_EQ(footer_says("Now on CC2. Waking charger", kText), true);
    check_text_at(kSmall, 146, "Rd on CC2", kText, kBg, 28);
    dump_screen("wait_line_moved");
    g_stub.trial_line = 1; g_stub.trial_moves = 3; g_stub.cc = 1;   // the fourth dwell
    ui.tick(1900);
    CHECK_EQ(footer_says("Now on CC1. Waking charger", kText), true);
    g_stub.trial_live = false; g_stub.trial_line = 0;   // the schedule ran out: the key hint takes the footer back
    ui.tick(2000);
    CHECK_EQ(footer_says("No VBUS. Hold + few secs", kText), true);
    dump_screen("wait_line_exhausted");
    // No Rp on either line: nothing to wake, whatever the schedule says.
    set_cc(0, 0); g_stub.trial_line = 1; g_stub.trial_live = true;
    ui.tick(2100);
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
    check_text_at(kSmall, 92, "Rp 3A", kText, kBg, 154);   // the Rp word while the key is up
    ui.set_pressed(Button::Plus, true, 1700);
    ui.tick(1800);
    check_text_at(kSmall, 92, "1650mV", kText, kBg, 154);   // the raw level while it is down
    ui.set_pressed(Button::Plus, false, 1900);
    ui.tick(2000);
    check_text_at(kSmall, 92, "Rp 3A", kText, kBg, 154);
}

// Shell parity batch 1 (red): the charger-test view is what the screen shows, read without a screenshot. The footer words
// are the page's own (one implementation: footer_line), the preview is the Review summary, the timers are the ones the
// Waiting and Running screens draw.
static std::string view_text(const ChargerTestSession::View& v) { return std::string(v.message, v.message_len); }
static void test_view_says_what_the_wizard_shows() {
    { // the Tests step: stage 0, the rows of the list, the footer's words
        fixture(); Ui ui; open(ui); ui.tick(1200);
        const auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, 0); CHECK_EQ(v.cursor, 0); CHECK_EQ(v.on_screen, true); CHECK_EQ(v.rows >= 5, true);
        CHECK_STR(view_text(v).c_str(), "Picked: All tests");
        CHECK_EQ(footer_says("Picked: All tests", kText), true);   // the same words are on the panel
        CHECK_EQ(v.confirming, false); CHECK_EQ(v.card_no, 0); CHECK_EQ(v.delta_n, 0);
    }
    { // Review: the preview counts, the footer, then the confirmation window with its timer
        fixture(); Ui ui; open(ui); review(ui); ui.tick(1300);
        auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, 4); CHECK_EQ(v.rows, 1);
        CHECK_EQ(v.preview.runnable > 0, true); CHECK_EQ(v.preview.refusal, ct::Reason::None);
        CHECK_EQ(v.confirming, false); CHECK_EQ(v.confirm_left_ms, 0u);
        CHECK_STR(view_text(v).c_str(), "Runs without a load");
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
    }
    { // Running: the m:ss clock in whole seconds, the footer's unit line; Results: the saved card's number
        fixture(); g_stub.fc_answers = false; Ui ui; open(ui); only_vivo_review(ui);
        start_press(ui, 1300); press(ui, Button::Ok, 1400);
        uint32_t t = 1500;
        for (; t <= 3500 && ui.charger_test_busy(); t += 100) ui.tick(t);
        auto v = ui.charger_test_view();
        CHECK_EQ(v.stage, kRunningStage); CHECK_EQ(v.run_elapsed_s >= 1 && v.run_elapsed_s <= 3, true);
        CHECK_EQ(v.rows > 0, true); CHECK_EQ(v.message_len > 0, true);
        for (; t <= 600000 && ui.charger_test_busy(); t += 100) ui.tick(t);
        ui.tick(t); ui.tick(t + 200);
        v = ui.charger_test_view();
        CHECK_EQ(v.stage, 6); CHECK_EQ(v.run_elapsed_s, 0); CHECK_EQ(v.rows > 0, true);
        CHECK_EQ(v.card_no != 0, true);
    }
}

int main() {
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
    test_a_row_in_an_engine_window_paints_nothing(); test_the_running_grid_follows_the_step();
    test_the_gap_line_names_the_blocked_window(); test_a_started_row_that_ends_as_a_skip_still_logs_its_gap();
    test_a_handshake_window_says_why_the_screen_is_still(); test_the_running_screen_keeps_updating_through_a_qc_handshake();
    test_failed_units_stay_above_the_running_row(); test_the_gauge_band_and_the_dot_move_in_the_same_tick();
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
    test_steps_rows_show_whole_labels();
    test_step_label_gallery();
    test_ufcs_results_and_detail_show_the_mode_current();
    test_results_best_line_names_the_best_modes();
    test_review_start_is_only_row();
    test_vsafe5_review();
    test_preflight_refusal_is_logged();
    test_preflight_wrong_voltage_under_pdo1_contract_is_logged();
    test_sweep_log_line_has_no_literal_format_bug();
    test_state_change_log_keeps_the_full_state_name_in_the_format();
    test_avs_sweep_is_a_selection_bit();
    test_avs_sweep_row_follows_the_only_sweep_kind();
    test_self_powered_review_note(); test_self_powered_run_is_on_the_card();
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
    return test_report("test_ui_charger_test");
}
