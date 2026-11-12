// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <cstdio>
#include <cstdlib>
#include "test_util.h"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/ui.hpp"
#include "ui_nav.h"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/capture.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include "k2/drivers/usb.hpp"
#include "k2/pd/pd_source.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

using namespace k2;

static void check_text(Ui &ui, int x, int y, int right, int bottom, const Font &font,
                       std::string_view text, uint16_t fg, uint16_t bg, int rows = 0) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters,
                                    kSlotLens, kPanelThickness).content;
    const int px = box.x + x * box.w / 224, py = box.y + y * box.h / 240;
    const int edge = box.x + right * box.w / 224, floor = box.y + bottom * box.h / 240;
    CHECK_EQ(px + text_width(font, text) <= edge, true);
    CHECK_EQ(py + font.height <= floor, true);
    uint16_t lut[16], line[240];
    make_blend_lut(fg, bg, lut);
    // `rows` limits the compare to the first glyph rows: at the compressed rotations the next row's hairline
    // lands on the blank last glyph row.
    for (int row = 0; row < (rows ? rows : font.height); ++row) {
        const int n = render_row(font, text, row, lut, line, 240);
        const bool same = std::equal(line, line + n, g_panel + (py + row) * LCD_WIDTH + px);
        if (!same && row == 5) {
            for (int i = 0; i < n; ++i) if (line[i] != g_panel[(py + row) * LCD_WIDTH + px + i]) {
                printf("text '%.*s' rot %u x %d got %04x expected %04x\n", int(text.size()), text.data(),
                       ui.rotation(), i, g_panel[(py + row) * LCD_WIDTH + px + i], line[i]);
                break;
            }
        }
        CHECK_EQ(same, true);
    }
}

static void parity(Ui &ui, uint32_t now) {
    std::array<uint16_t, LCD_WIDTH * LCD_HEIGHT> panel;
    std::copy_n(g_panel, panel.size(), panel.begin());
    CHECK_EQ(capture_begin(ui), true);
    for (int strip = 0; strip < kCaptureStrips; ++strip) {
        const uint16_t *pixels = capture_strip(ui, strip, now);
        const bool same = std::equal(pixels, pixels + kCaptureStripPixels,
                                     panel.begin() + strip * kCaptureStripPixels);
        if (!same) printf("capture page %u rot %u strip %d\n", unsigned(ui.page()), ui.rotation(), strip);
        CHECK_EQ(same, true);
    }
    capture_end(ui, now);
}

static void check_menu_mini(Ui &ui, std::string_view text, bool volts = false) {
    const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters,
                                    kSlotLens, kPanelThickness).content;
    // The chrome bar readout has fixed slots: the band maps its origin (x 140 = 140*240/224 at
    // the wide rotations) but paints text at native pixel advances: each value right-aligned in its 30 px slot, volts from
    // +15 (x155), amps from +52 (x192), in ink (the colour key in front of it carries the quantity).
    const int slot = 5 * static_cast<int>(kLabelAdv);
    const int x = box.x + 140 * box.w / 224 + (volts ? kMastVoltX : kMastAmpX) - 140 + slot - text_width(kLabelFont, text);
    const int y = box.y + kBarTextY;
    CHECK_EQ(text_width(kLabelFont, text) <= slot, true);
    uint16_t lut[16], line[240];
    make_blend_lut(kInk, kBar, lut);
    for (int row = 0; row < kLabelFont.height; ++row) {
        const int n = render_row(kLabelFont, text, row, lut, line, 240);
        const bool same = std::equal(line, line + n, g_panel + (y + row) * LCD_WIDTH + x);
        if (!same && row == 6 && text == "OVR")
            for (int i = 0; i < n; ++i) printf("%04x/%04x ", g_panel[(y + row) * LCD_WIDTH + x + i], line[i]);
        CHECK_EQ(same, true);
    }
}

static void fixture(Ui &ui) {
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    g_stub.caps.valid = true;
    g_stub.caps.active_cc = 1;
    g_stub.caps.spec_rev = 2;
    g_stub.cc = 1;
    ui.init(0);
    ui.debug_link(true, 1000);
}

// Home v7 has no chrome bar (the layout fills the content area): capture parity in every rotation
// with a self-powered source, and nothing is transmitted.
static void test_home_self_power_status_bar() {
    Ui ui; fixture(ui);
    g_stub.self_powered = true;
    g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300u);
    g_stub.caps.pdos[0].max_mw = 240000;
    ui.tick(1400);
    ui.set_rotation(2, 1400);   // force a real transition so the r=0 pass below starts from a full redraw
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1400);
        parity(ui, 1400);
    }
    CHECK_EQ(g_stub.requests + g_stub.scans + g_stub.rescans, 0);
}

static void test_diag_counters() {
    Ui ui; fixture(ui);
    usb::stats.resets = UINT32_MAX;
    usb::stats.isrs = UINT32_MAX;
    usb::stats.log_sent = UINT32_MAX;
    usb::stats.log_dropped = UINT32_MAX;
    usb::stats.poll_gap_max = UINT32_MAX;
    g_stub.caps.rx_count = UINT32_MAX;
    ui.goto_page(kDiagPage, 1100);
    ui.tick(1300);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        // Diag kit KvTable rows: value right-aligned to x218 in one 17 char box (x82), kSmall at row_y+4, rows at 24 + i*22.
        // Rows 1/2 print compact counts (10K, 999M, ">999M": at most 5 chars each), so the worst case
        // ">999M / >999M" is 13 chars, clear of the labels (S12 at x28).
        // A saturated count is neutral, never the caution colour (the amber is for ordinary log drops).
        check_text(ui, 114, 50, 218, 69, kSmall, ">999M / >999M", kText, kBg);
        check_text(ui, 114, 72, 218, 91, kSmall, ">999M / >999M", kText, kBg);
        check_text(ui, 82, 116, 218, 135, kSmall, "4294967295 on CC1", kText, kBg);
        check_text(ui, 114, 138, 218, 157, kSmall, "4294967295 ms", kText, kBg);
        parity(ui, 1300);
    }
    // Ordinary log drops keep the caution colour; only the saturated count above went neutral.
    usb::stats = {};
    usb::stats.log_sent = 5000; usb::stats.log_dropped = 12;
    ui.set_rotation(0, 1400);
    ui.tick(1400);
    check_text(ui, 146, 72, 218, 91, kSmall, "5000 / 12", kCaution, kBg);
    usb::stats = {};
    g_stub.caps.rx_count = 0;
    ui.set_rotation(0, 1500);   // right edge: logical == physical, exact pixel match
    ui.tick(1500);
    check_text(ui, 178, 50, 218, 69, kSmall, "0 / 0", kText, kBg);
    parity(ui, 1500);
}

static void test_emarker_unpowered_setup() {
    Ui ui;
    g_stub = {};
    pd::set_fake(0);
    g_stub.pm_online = true;
    ui.init(0);
    ui.debug_link(false, 1000);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    ui.push_menu(Menu::Emarker, 1200);
    ui.tick(1300);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        const Rect box = compute_layout(orientation_for(ui.rotation()).edge, kSlotCenters,
                                        kSlotLens, kPanelThickness).content;
        const int top = box.y + 22 * box.h / 240, bottom = box.y + 94 * box.h / 240;
        unsigned ink = 0;
        for (int y = top; y < bottom; ++y)
            for (int x = box.x; x < box.x + box.w; ++x)
                ink += g_panel[y * LCD_WIDTH + x] != kBg;
        CHECK_EQ(ink > 100, true);
        parity(ui, 1300);
    }
    CHECK_EQ(g_stub.requests + g_stub.scans + g_stub.rescans, 0);
}

static void test_pd_tabs_text() {
    Ui ui; fixture(ui);
    constexpr uint32_t fixed = (400u << 10) | 300u;   // 20 V 3 A
    g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = pd::decode_pdo(fixed);
    ui.goto_page(kPdPage, 1100);
    ui.tick(1300);
    // Source tab: the ID row is first (y 42), the PD summary row second (value column x 64, row y 62).
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        check_text(ui, 6, 67, 50, 82, kLabelFont, "PD", kDim, kBg);   // key at x 6, row_y+5
        parity(ui, 1300);
    }
    // Short + moves to the Cable tab: no cable read, so the verdict band names it.
    ui.on_button(Button::Plus, ButtonEvent::Short, 1400);
    ui.tick(1700);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1700);
        check_text(ui, 32, 48, 220, 78, kMid, "NO CABLE READ", kText, kSurface);   // the verdict band (UI_SYSTEM 5.8)
        parity(ui, 1700);
    }
    CHECK_EQ(g_stub.requests + g_stub.scans + g_stub.rescans, 0);
}

// A charger with more fixed voltages than fit one line (22 cells): the line breaks at the 19-cell limit and
// each line keeps its own current, so no PDO is cut and no line is left without its A.
static void test_source_fixed_rows_wrap() {
    Ui ui; fixture(ui);
    constexpr uint16_t mv[] = {5, 9, 12, 15, 18, 20, 24};
    g_stub.caps.num_pdos = 7;
    for (int i = 0; i < 7; ++i) g_stub.caps.pdos[i] = pd::decode_pdo((uint32_t(mv[i]) * 1000 / 50 << 10) | 300u);
    ui.goto_page(kPdPage, 1100);
    ui.tick(1300);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        // rows: ID (y 42), PD (62), Fixed (82), Fixed (102); value column x 64, text 3 px below the row top
        check_text(ui, 64, 85, 220, 104, kSmall, "5/9/12/15/18/20V", kVolt, kBg);
        check_text(ui, 64 + 16 * kSmall.adv, 85, 220, 104, kSmall, " 3A", kAmp, kBg);
        check_text(ui, 64, 105, 220, 124, kSmall, "24V", kVolt, kBg, 15);
        parity(ui, 1300);
    }
}

// K2_DUMP=dir writes the three tabs of a 100 W charger through a 5 A cable and a 45 W request.
static void test_pd_tabs_dump() {
    const char *dir = std::getenv("K2_DUMP");
    Ui ui; fixture(ui);
    g_stub.caps.num_pdos = 3;
    g_stub.caps.pdos[0] = pd::decode_pdo((100u << 10) | 300u);
    g_stub.caps.pdos[1] = pd::decode_pdo((300u << 10) | 300u);
    g_stub.caps.pdos[2] = pd::decode_pdo((400u << 10) | 500u);
    g_stub.em.valid = true; g_stub.em.max_voltage_v = 50; g_stub.em.max_current_a = 5; g_stub.em.max_power_w = 250;
    g_stub.em.speed_str = "USB 3.2 Gen2 (10G)"; g_stub.em.vendor_str = "Apple"; g_stub.em.vid = 0x05AC;
    g_stub.em.latency_str = "10-20 ns (~2 m)";
    g_stub.contract.valid = true; g_stub.contract.ready = true; g_stub.contract.pos = 3;
    g_stub.contract.mv = 20000; g_stub.contract.op_ma = g_stub.contract.max_ma = 2250;
    ui.goto_page(kPdPage, 1100);
    uint32_t t = 1300;
    static const char *const names[] = {"source", "cable", "sink"};
    for (int i = 0; i < 3; i++) {
        ui.tick(t); t += 300; ui.tick(t); t += 300;
        if (dir) {
            char path[256];
            std::snprintf(path, sizeof path, "%s/k2_pdtab_%s.panel", dir, names[i]);
            if (FILE *f = std::fopen(path, "wb")) { std::fwrite(g_panel, 2, LCD_WIDTH * LCD_HEIGHT, f); std::fclose(f); }
        }
        ui.on_button(Button::Plus, ButtonEvent::Short, t);
    }
    CHECK_EQ(on_pd(ui), true);
}

static void test_trigger_ranges() {
    Ui ui; fixture(ui);
    const uint32_t avs = 0xD0000000u | (511u << 17) | (255u << 8) | 255u;
    const uint32_t spr_avs = 0xE0000000u | 1023u;
    g_stub.caps.num_pdos = 2;
    g_stub.caps.pdos[0] = pd::decode_pdo(avs);
    g_stub.caps.pdos[1] = pd::decode_pdo(spr_avs);
    ui.tick(1000);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    CHECK_EQ(ui.push_menu(Menu::PdTrigger, 1150), true);
    ui.tick(1300);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1300);
        // The widest AVS row: text from x28, the power at the right edge (x182..214), on the focused row (y24..50).
        check_text(ui, 28, 30, 214, 49, kSmall, "AVS 25.5-51.1V 5A", kText, kFocusT);
        check_text(ui, 182, 30, 214, 49, kSmall, "255W", kWatt, kFocusT);
        parity(ui, 1300);
    }
    ui.on_button(Button::Plus, ButtonEvent::Short, 1300);
    CHECK_EQ(ui.list_state().offset, 0);
    CHECK_EQ(g_stub.requests + g_stub.scans + g_stub.rescans, 0);
}

static void test_trigger_edit_caption() {
    Ui ui; fixture(ui);
    const uint32_t pps = 0xC0000000u | (255u << 17) | (33u << 8) | 127u;
    g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = pd::decode_pdo(pps);
    ui.tick(1000);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    CHECK_EQ(ui.push_menu(Menu::PdTrigger, 1150), true);
    ui.tick(1300);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 1400);
        // VOLTAGE: the offer (the amps round to one decimal so the row fits) and its exact current limit in the table.
        check_text(ui, 72, 27, 224, 45, kSmall, "PPS 3.3-25.5V 6.4A", kText, kBg);
        check_text(ui, 72, 47, 224, 65, kSmall, "6.35A", kAmp, kBg);
        parity(ui, 1400);
    }
    ui.on_button(Button::Back, ButtonEvent::Short, 1500);
    CHECK_EQ(g_stub.requests + g_stub.scans + g_stub.rescans, 0);
}

static void test_live_measurements() {
    Ui ui; fixture(ui);
    constexpr uint32_t fixed = (100u << 10) | 300u;
    g_stub.caps.num_pdos = 1;
    g_stub.caps.pdos[0] = pd::decode_pdo(fixed);
    ui.tick(1000);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1100);
    CHECK_EQ(ui.push_menu(Menu::PdTrigger, 1150), true);
    ui.tick(1300);
    // The test uses only the fake host sink; no device or real voltage request is involved.
    for (uint32_t t : {1400u, 1500u, 1600u}) ui.on_button(Button::Ok, ButtonEvent::Short, t);
    CHECK_EQ(g_stub.requests, 1);
    g_stub.sink.state = pd::SinkState::Ready;
    g_stub.sink.held = true;
    struct Measurement { int32_t ma; const char *text; const Font *font; int y; };
    const Measurement cases[] = {
        {9990, "9.99A", &kSmall, 131},
        {10000, "10.00A", &kSmall, 131},
        {123450, "123.45A", &kSmall, 131},
        {INT32_MAX, "2147483.65A", &kSmall, 131},
        {INT32_MIN, "2147483.65A", &kSmall, 131},
    };
    uint32_t now = 2000;
    // The chrome bar's readout is fmt3-based (three total digits, floating point, magnitudes only):
    // a value that formats to more than 5 characters incl. the unit shows "OVR" in its slot.
    for (const auto &value : cases) for (auto flow : {power::Flow::Forward, power::Flow::Reverse, power::Flow::Idle}) {
        g_stub.pm.current_ma = value.ma;
        g_stub.pm.flow = flow;
        now += 200;
        ui.tick(now);
        const char *mini = value.ma == 9990 ? "9.99A" : value.ma == 10000 ? "10.0A"
                         : value.ma == 123450 ? "123A" : "OVR";
        for (uint8_t r = 0; r < 4; ++r) {
            ui.set_rotation(r, now);
            check_text(ui, 84, value.y, 224, 151, *value.font, value.text, kAmp, kBg);   // the measured-A cell of the LIVE table
            check_menu_mini(ui, mini);
            parity(ui, now);
        }
    }
    g_stub.pm.vbus_mv = INT32_MAX;
    g_stub.pm.current_ma = 10000;
    g_stub.pm.flow = power::Flow::Forward;
    ui.tick(now += 200);
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, now);
        check_text(ui, 84, 91, 224, 111, kSmall, "2147483.65V", kVolt, kBg);
        check_menu_mini(ui, "10.0A");   // amps slot: 10000 mA fits, no OVR
        check_menu_mini(ui, "OVR", true);   // volts slot: INT32_MAX overflows
        parity(ui, now);
    }
    CHECK_EQ(g_stub.requests, 1);
}

int main() {
    if (std::getenv("ONLY_LIVE")) { test_live_measurements(); return test_report("test_ui_extremes"); }
    test_home_self_power_status_bar();
    test_diag_counters();
    test_emarker_unpowered_setup();
    test_pd_tabs_text();
    test_source_fixed_rows_wrap();
    test_pd_tabs_dump();
    test_trigger_ranges();
    test_trigger_edit_caption();
    test_live_measurements();
    return test_report("test_ui_extremes");
}
