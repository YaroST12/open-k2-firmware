// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "test_util.h"
#include "k2/ui/ui.hpp"
#include "k2/ui/kit.hpp"
#include "k2/ui/ui_viewport.hpp"
#include "k2/ui/ui_style.hpp"
#include "fake_lcd.h"
#include "ui_stubs.h"
#include "k2/ui/capture.hpp"
#include "k2/ui/ui_orientation.hpp"
#include "k2/ui/blend.hpp"
#include "k2/ui/text.hpp"
#include <algorithm>
#include <array>

using namespace k2;

static void test_viewport() {
    for (uint8_t r = 0; r < 4; ++r) {
        std::fill(std::begin(g_panel), std::end(g_panel), 0xDEAD);
        const auto layout = compute_layout(orientation_for(r).edge, kSlotCenters, kSlotLens, kPanelThickness);
        {
            ContentViewport map(layout.content);
            lcd::fill_rect(0, 0, 224, 240, 0x1234);
            lcd::fill_rect(0, 222, 224, 18, 0xABCD);
        }
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) {
            const auto &c = layout.content;
            const bool inside = x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
            if (!inside) CHECK_EQ(g_panel[y * 240 + x], 0xDEAD);
            else CHECK_EQ(g_panel[y * 240 + x], y >= c.y + 222 * c.h / 240 ? 0xABCD : 0x1234);
        }
        CHECK_EQ(lcd::get_sink(), nullptr);
    }
}

static void test_native_menu_fonts() {
    const Surface logical{0, 0, 224, 240};
    std::array<uint16_t, 240 * 240> expected;
    for (uint8_t r = 0; r < 4; ++r) {
        const auto box = compute_layout(orientation_for(r).edge, kSlotCenters, kSlotLens, kPanelThickness).content;
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        const Surface native{box.x, box.y, box.w, box.h};
        gfx_text(native, kSmall, static_cast<int16_t>(20 * box.w / 224),
                 static_cast<int16_t>(40 * box.h / 240), "Identify", kText, kBg);
        std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        {
            ContentViewport viewport(box);
            gfx_text(logical, kSmall, 20, 40, "Identify", kText, kBg);
        }
        CHECK_EQ(std::equal(expected.begin(), expected.end(), std::begin(g_panel)), true);
        {
            ContentViewport viewport(box);
            draw_footer(logical, FooterTone::Caution, "Identify");
        }
        uint16_t lut[16], line[240];
        make_blend_lut(kInk, kCautionT, lut);   // a Caution fact line: ink on the amber tint
        const int footer_top = box.y + kFooterY * box.h / 240 + 1;
        for (int row = 0; row < kSmall.height; ++row) {
            const int width = render_row(kSmall, "Identify", row, lut, line, 240);
            CHECK_EQ(std::equal(line, line + width, g_panel + (footer_top + row) * 240 + box.x + kFooterTextX * box.w / 224), true);
        }
        Field field;
        {
            ContentViewport viewport(box);
            field.init(logical, kSmall, 20, 40, 8, Align::Left, kText, kBg);
            field.draw("Identify");
            field.draw("Idle");
        }
        std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
        {
            ContentViewport viewport(box);
            field.invalidate();
            field.draw("Idle");
        }
        CHECK_EQ(std::equal(expected.begin(), expected.end(), std::begin(g_panel)), true);
        // The changed suffix must use native advances, not independently mapped cell origins.
        {
            ContentViewport viewport(box);
            field.draw("Idle1234");
            field.draw("Idle5678");
        }
        std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
        {
            ContentViewport viewport(box);
            field.invalidate();
            field.draw("Idle5678");
        }
        CHECK_EQ(std::equal(expected.begin(), expected.end(), std::begin(g_panel)), true);
    }
}

static void test_static_flow_indicator() {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm = {};
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;
    g_stub.pm.power_mw = 5000;
    g_stub.pm.flow = power::Flow::Forward;
    Ui ui;
    ui.init(0);
    ui.tick(6000);
    // The rail (x 0..14) is driven by a time-based phase: no time passed, no step.
    std::array<uint16_t, 14 * 150> rail{};
    for (int y = 40; y < 190; ++y)
        std::copy(g_panel + y * 240, g_panel + y * 240 + 14, rail.begin() + (y - 40) * 14);
    ui.tick(6000);
    for (int y = 40; y < 190; ++y)
        CHECK_EQ(std::equal(rail.begin() + (y - 40) * 14, rail.begin() + (y - 39) * 14,
                            g_panel + y * 240), true);
}

static void test_captures_and_keys() {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;
    g_stub.pm.power_mw = 5000;
    g_stub.pm.flow = power::Flow::Forward;
    Ui ui;
    ui.init(0);
    ui.tick(6000); // expire the discoverability notice before comparing stable captures
    std::array<uint16_t, 240 * 240> expected;
    ui.set_rotation(2, 6000);   // init boots at rotation 0 (right edge, physical button side); force a real transition so set_rotation(0) below is not a no-op
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 6000);
        constexpr uint8_t pages = static_cast<uint8_t>(Page::Count);
        for (uint8_t p = 0; p < pages + static_cast<uint8_t>(Menu::Count); ++p) {
            CHECK_EQ(ui.goto_page(p < pages ? p : static_cast<uint8_t>(Page::Tools), 6000), true);
            if (p >= pages) CHECK_EQ(ui.push_menu(static_cast<Menu>(p - pages), 6000), true);
            // The QR scan-test and share screens cover the whole panel, button strip included (chrome hidden while
            // shown), so the slot pixels are the code's there, not the panel's: skip only that check for them.
            const bool qrtest = p >= pages && (static_cast<Menu>(p - pages) == Menu::QrTest ||
                                               static_cast<Menu>(p - pages) == Menu::Share);
            std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
            CHECK_EQ(capture_begin(ui), true);
            for (int strip = 0; strip < kCaptureStrips; ++strip) {
                const uint16_t* pixels = capture_strip(ui, strip, 6000);
                CHECK_EQ(std::equal(pixels, pixels + kCaptureStripPixels,
                                    expected.begin() + strip * kCaptureStripPixels), true);
            }
            capture_end(ui, 6000);
            CHECK_EQ(ui.rotation(), r);
            CHECK_EQ(ui.context().content.w, 224);
            CHECK_EQ(ui.context().content.h, 240);
            ui.set_pressed(Button::Ok, true, 6000);
            CHECK_EQ(ui.lit(Button::Ok), true);
            CHECK_EQ(ui.lit(Button::Plus), false);
            ui.set_pressed(Button::Ok, false, 6200);
            const auto orient = orientation_for(r);
            const auto layout = compute_layout(orient.edge, kSlotCenters, kSlotLens, kPanelThickness);
            for (unsigned key = 0; key < 4; ++key) {
                ui.set_pressed(static_cast<Button>(key), true, 6300);
                for (unsigned slot = 0; slot < 4; ++slot) {
                    const auto& box = layout.slot[slot];
                    const int px = box.x + (panel_is_column(orient.edge) ? 0 : 10);   // the rail edge: outside the OK cap
                    const int py = box.y + (panel_is_column(orient.edge) ? 10 : 0);
                    if (!qrtest)
                        CHECK_EQ(g_panel[py * 240 + px], slot_button(orient, slot) == static_cast<Button>(key) ? kCap : kBg2);   // a held key flashes in the cap colour
                    // Independently apply the MADCTL coordinate permutation back to physical glass.
                    const int lx = box.x + box.w / 2, ly = box.y + box.h / 2;
                    const int physical_y = r == 0 ? ly : r == 1 ? lx : r == 2 ? 239 - ly : 239 - lx;
                    const auto button = static_cast<unsigned>(slot_button(orient, slot));
                    CHECK_EQ(std::abs(physical_y - kSlotCenters[button]) <= 1, true);
                }
                ui.set_pressed(static_cast<Button>(key), false, 6500);
            }
        }
    }
}

static void test_physical_flow_pixels() {
    const Surface surface{0, 0, 224, 240};
    std::array<uint16_t, 16 * 16> upright{};
    std::fill(std::begin(g_panel), std::end(g_panel), 0);
    draw_physical_arrow(surface, 20, 20, true, 0, 0x1234);
    for (int y = 0; y < 16; ++y)
        std::copy(g_panel + (20 + y) * 240 + 20, g_panel + (20 + y) * 240 + 36, upright.begin() + y * 16);
    CHECK_EQ(upright[1 * 16 + 7], 0x1234);
    CHECK_EQ(upright[14 * 16 + 7], 0x1234);
    CHECK_EQ(upright[14 * 16 + 2], 0);
    for (uint8_t r = 0; r < 4; ++r) for (bool toward_top : {false, true}) {
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        draw_physical_arrow(surface, 20, 20, toward_top, r, 0x1234);
        const int turn = (r + (toward_top ? 0 : 2)) & 3;
        for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
            const int xx = turn == 0 ? x : turn == 1 ? y : turn == 2 ? 15 - x : 15 - y;
            const int yy = turn == 0 ? y : turn == 1 ? 15 - x : turn == 2 ? 15 - y : x;
            CHECK_EQ(g_panel[(20 + yy) * 240 + 20 + xx], upright[y * 16 + x]);
        }
    }
    for (uint8_t r = 0; r < 4; ++r) {
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        draw_physical_chevron(surface, 20, 20, true, r, 0x1234);
        int min_x = 240, max_x = 0, min_y = 240, max_y = 0;
        for (int y = 20; y < 30; ++y) for (int x = 20; x < 30; ++x) if (g_panel[y * 240 + x]) {
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
        }
        // An apex has 2 pixels; the tail has two separated arms (4 pixels).
        int apex = 0, tail = 0;
        for (int i = 20; i < 30; ++i) {
            if (!(r & 1)) {
                apex += g_panel[(r == 0 ? min_y : max_y) * 240 + i] != 0;
                tail += g_panel[(r == 0 ? max_y : min_y) * 240 + i] != 0;
            } else {
                apex += g_panel[i * 240 + (r == 1 ? min_x : max_x)] != 0;
                tail += g_panel[i * 240 + (r == 1 ? max_x : min_x)] != 0;
            }
        }
        CHECK_EQ(apex, 2); CHECK_EQ(tail, 4);
    }
}

// The idle dash lies along the port axis: vertical (2 x 8) at rotations 0 and 2, horizontal (8 x 2) at 1 and 3,
// centred in its 16 x 16 cell, and it takes the colour it is given (faint, grey when frozen).
static void test_physical_dash() {
    const Surface surface{0, 0, 224, 240};
    for (uint8_t r = 0; r < 4; ++r) {
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        draw_physical_dash(surface, 20, 20, r, 0x1234);
        int min_x = 240, max_x = -1, min_y = 240, max_y = -1, n = 0;
        for (int y = 0; y < 240; ++y) for (int x = 0; x < 240; ++x) if (g_panel[y * 240 + x]) {
            CHECK_EQ(g_panel[y * 240 + x], 0x1234);
            min_x = std::min(min_x, x); max_x = std::max(max_x, x);
            min_y = std::min(min_y, y); max_y = std::max(max_y, y);
            ++n;
        }
        const bool vertical = !(r & 1);
        CHECK_EQ(n, 16);
        CHECK_EQ(max_x - min_x + 1, vertical ? 2 : 8);
        CHECK_EQ(max_y - min_y + 1, vertical ? 8 : 2);
        CHECK_EQ(min_x >= 20 && max_x < 36 && min_y >= 20 && max_y < 36, 1);
        CHECK_EQ(min_x + max_x, 2 * 20 + 15);   // centred in the cell
        CHECK_EQ(min_y + max_y, 2 * 20 + 15);
    }
}

// The row-mask form of the physical chevron (what the status-bar band writes) equals the pixels of
// draw_physical_chevron in all four rotations and both directions.
static void test_physical_chevron_mask() {
    const Surface surface{0, 0, 224, 240};
    for (uint8_t r = 0; r < 4; ++r) for (bool toward_top : {false, true}) {
        std::fill(std::begin(g_panel), std::end(g_panel), 0);
        draw_physical_chevron(surface, 20, 20, toward_top, r, 0x1234);
        const int w = physical_chevron_w(r), h = physical_chevron_h(r);
        CHECK_EQ(w, (r & 1) ? 6 : 10);
        CHECK_EQ(h, (r & 1) ? 10 : 6);
        for (int y = 0; y < 12; ++y) for (int x = 0; x < 12; ++x) {
            const bool mask = y < h && x < w && (physical_chevron_row(toward_top, r, y) >> x & 1);
            CHECK_EQ(g_panel[(20 + y) * 240 + 20 + x] != 0, mask);
        }
    }
}

static void chord(Ui &ui, uint32_t now) {
    ui.set_pressed(Button::Minus, true, now);
    ui.set_pressed(Button::Plus, true, now);
    ui.on_button(Button::Minus, ButtonEvent::Long, now + 500);
    ui.on_button(Button::Plus, ButtonEvent::Long, now + 500);
    ui.set_pressed(Button::Minus, false, now + 600);
    ui.set_pressed(Button::Plus, false, now + 600);
    ui.tick(now + 600);
}

static void test_lock_and_feed() {
    g_stub = {};
    Ui ui;
    ui.init(0);
    const auto page = ui.page();
    chord(ui, 1000);
    CHECK_EQ(ui.rotation_locked(), true);
    CHECK_EQ(ui.rotation(), 0);
    for (uint32_t now = 2000; now <= 2400; now += 50)
        ui.feed_gravity(orientation::Gravity{0, -1000, 0}, now);
    CHECK_EQ(ui.rotation(), 0); // lock ignores feed, but tracker retains the current posture
    for (uint32_t n = 0; n < 3; ++n) {
        chord(ui, 3000 + n * 1000);
        CHECK_EQ(ui.rotation(), n + 1);
        CHECK_EQ(ui.rotation_locked(), true);
        CHECK_EQ(ui.page(), page);
    }
    chord(ui, 6000);
    CHECK_EQ(ui.rotation_locked(), false);
    CHECK_EQ(ui.rotation(), 2); // AUTO catches up to gravity seen while locked
    for (uint32_t now = 7000; now <= 7400; now += 50)
        ui.feed_gravity(orientation::Gravity{0, 1000, 0}, now);
    CHECK_EQ(ui.rotation(), 0);
}

static void test_settings_rotation_modes() {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    g_stub.pm.current_ma = 1000;
    g_stub.pm.power_mw = 5000;
    g_stub.pm.flow = power::Flow::Forward;
    Ui ui;
    ui.init(0);
    ui.tick(6000);
    ui.open_place(bench::Place::K2, 6100);   // Bench > K2: the settings are the place page
    CHECK_EQ(ui.page() == Page::Settings, true);
    CHECK_EQ(ui.list_state().count, SettingsPage::kRows);   // ... Wake sleeping port, System state, Boot info
    {   // Smoothing row: Medium by default, OK cycles High -> Off -> Low -> Medium, stores each and applies it at once
        g_stub.prefs = {true, false}; g_stub.prefs_sets = 0; g_stub.smoothing_applied = smoothing::Preset::Medium;
        for (int i = 0; i < 4; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 6205 + i);
        const smoothing::Preset want[] = {smoothing::Preset::High, smoothing::Preset::Off, smoothing::Preset::Low,
                                          smoothing::Preset::Medium};
        for (int i = 0; i < 4; ++i) {
            ui.on_button(Button::Ok, ButtonEvent::Short, 6220 + i);
            CHECK_EQ(g_stub.smoothing_applied == want[i], true);
            CHECK_EQ(g_stub.prefs.smoothing == want[i], true);
            CHECK_EQ(g_stub.prefs.qc_after_pd, true);      // the other stored flags ride along
            CHECK_EQ(g_stub.prefs.screenshot, false);
        }
        CHECK_EQ(g_stub.prefs_sets, 4);
        ui.on_button(Button::Plus, ButtonEvent::Short, 6230);   // the Wake sleeping port row follows (then System state, Boot info)
        for (int i = 0; i < 5; ++i) ui.on_button(Button::Minus, ButtonEvent::Short, 6240 + i);   // back to the top
        g_stub.prefs = {};
    }
    {   // Wake sleeping port row (row 5, before System state): Off by default, OK toggles it, stores it with the other flags, applies it now
        g_stub.prefs = {true, false}; g_stub.prefs_sets = 0; g_stub.idle_wake = false;
        for (int i = 0; i < 5; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 6250 + i);
        CHECK_EQ(g_stub.idle_wake, false);
        ui.on_button(Button::Ok, ButtonEvent::Short, 6260);
        CHECK_EQ(g_stub.idle_wake, true);
        CHECK_EQ(g_stub.prefs.idle_wake, true);
        CHECK_EQ(g_stub.prefs.qc_after_pd, true); CHECK_EQ(g_stub.prefs.screenshot, false);
        CHECK_EQ(g_stub.prefs_sets, 1);
        ui.on_button(Button::Ok, ButtonEvent::Short, 6261);
        CHECK_EQ(g_stub.idle_wake, false); CHECK_EQ(g_stub.prefs.idle_wake, false);
        ui.on_button(Button::Plus, ButtonEvent::Short, 6262); ui.on_button(Button::Minus, ButtonEvent::Short, 6262);   // System state follows; back
        ui.on_button(Button::Ok, ButtonEvent::Short, 6263);
        CHECK_EQ(g_stub.idle_wake, true);
        ui.on_button(Button::Ok, ButtonEvent::Short, 6264);
        CHECK_EQ(g_stub.idle_wake, false);
        CHECK_EQ(g_stub.prefs_sets, 4);
        for (int i = 0; i < 5; ++i) ui.on_button(Button::Minus, ButtonEvent::Short, 6270 + i);
        g_stub.prefs = {};
    }
    {   // Screenshot row: default on, OK toggles it and stores it, the stored prefs keep the other flag
        uiprefs::screenshot_enabled = true; g_stub.prefs = {true}; g_stub.prefs_sets = 0;
        for (int i = 0; i < 3; ++i) ui.on_button(Button::Plus, ButtonEvent::Short, 6210 + i * 10);
        ui.on_button(Button::Ok, ButtonEvent::Short, 6250);
        CHECK_EQ(uiprefs::screenshot_enabled, false);
        CHECK_EQ(g_stub.prefs.screenshot, false); CHECK_EQ(g_stub.prefs.qc_after_pd, true);
        CHECK_EQ(g_stub.prefs_sets, 1);
        ui.on_button(Button::Ok, ButtonEvent::Short, 6260);
        CHECK_EQ(uiprefs::screenshot_enabled, true); CHECK_EQ(g_stub.prefs.screenshot, true);
        for (int i = 0; i < 3; ++i) ui.on_button(Button::Minus, ButtonEvent::Short, 6270 + i * 10);
        g_stub.prefs = {};
    }
    ui.on_button(Button::Ok, ButtonEvent::Short, 6300);
    CHECK_EQ(ui.menu() == Menu::UsbLink, true);
    ui.on_button(Button::Back, ButtonEvent::Short, 6400);
    CHECK_EQ(ui.page() == Page::Settings, true);
    CHECK_EQ(ui.menu_depth(), 0);
    ui.on_button(Button::Plus, ButtonEvent::Short, 6500);
    ui.on_button(Button::Ok, ButtonEvent::Short, 6600);
    CHECK_EQ(ui.menu() == Menu::Autorotate, true);
    CHECK_EQ(ui.list_state().count, 5);
    CHECK_EQ(g_panel[30 * 240 + 1], kFocus);   // selection bar x0..2
    unsigned active_pixels = 0;   // the active row carries the radio dot in kFocus (12 px marker at x8, row 0 at y24)
    for (int y = 31; y < 43; ++y)
        for (int x = 8; x < 20; ++x) active_pixels += g_panel[y * 240 + x] == kFocus;
    CHECK_EQ(active_pixels > 0, true);
    uint32_t now = 7000;
    for (uint8_t rotation = 0; rotation < 4; ++rotation) {
        ui.on_button(Button::Plus, ButtonEvent::Short, now);
        if (rotation == 0) {
            CHECK_EQ(g_panel[30 * 240 + 1] == kFocus, false);
            CHECK_EQ(g_panel[(30 + 26) * 240 + 1], kFocus);   // rows are 26 px pitch now
            unsigned still_active = 0;
            for (int y = 31; y < 43; ++y)
                for (int x = 8; x < 20; ++x) still_active += g_panel[y * 240 + x] == kFocus;
            CHECK_EQ(still_active > 0, true);
        }
        ui.on_button(Button::Ok, ButtonEvent::Short, now + 100);
        CHECK_EQ(ui.rotation_locked(), true);
        CHECK_EQ(ui.rotation(), rotation);
        CHECK_EQ(ui.menu() == Menu::Autorotate, true);
        for (uint32_t t = now + 200; t <= now + 600; t += 50)
            ui.feed_gravity(orientation::Gravity{0, -1000, 0}, t);
        CHECK_EQ(ui.rotation(), rotation);
        std::array<uint16_t, 240 * 240> expected;
        std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
        CHECK_EQ(capture_begin(ui), true);
        for (int strip = 0; strip < kCaptureStrips; ++strip) {
            const uint16_t *pixels = capture_strip(ui, strip, now + 600);
            CHECK_EQ(std::equal(pixels, pixels + kCaptureStripPixels,
                                expected.begin() + strip * kCaptureStripPixels), true);
        }
        capture_end(ui, now + 600);
        now += 1000;
    }
    for (int i = 0; i < 4; ++i) ui.on_button(Button::Minus, ButtonEvent::Short, now);
    ui.on_button(Button::Ok, ButtonEvent::Short, now + 100);
    CHECK_EQ(ui.rotation_locked(), false);
    CHECK_EQ(ui.rotation(), 2);
    for (uint32_t t = now + 200; t <= now + 600; t += 50)
        ui.feed_gravity(orientation::Gravity{0, 1000, 0}, t);
    CHECK_EQ(ui.rotation(), 0);
    ui.on_button(Button::Back, ButtonEvent::Short, now + 700);
    CHECK_EQ(ui.page() == Page::Settings, true);
    CHECK_EQ(ui.menu_depth(), 0);
}

// Orientation settings are remembered: Settings > Rotate buttons, Settings > Autorotate and the +/- hold
// gesture write the uiprefs record (the other stored fields ride along), and a boot applies it before the first draw.
static void test_rotation_prefs_persist() {
    {   // Rotate buttons row: OK toggles it and stores it
        g_stub = {};
        g_stub.prefs = {true, false};   // Try QC on, Screenshot off: must survive the write
        Ui ui;
        ui.init(0);
        ui.goto_page(static_cast<uint8_t>(Page::Settings), 1000);
        ui.on_button(Button::Plus, ButtonEvent::Short, 1100);
        ui.on_button(Button::Plus, ButtonEvent::Short, 1200);
        ui.on_button(Button::Ok, ButtonEvent::Short, 1300);
        CHECK_EQ(ui.rotate_buttons(), true);
        CHECK_EQ(g_stub.prefs_sets, 1);
        CHECK_EQ(g_stub.prefs.rotate_buttons, true);
        CHECK_EQ(g_stub.prefs.qc_after_pd, true); CHECK_EQ(g_stub.prefs.screenshot, false);
        ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
        CHECK_EQ(ui.rotate_buttons(), false);
        CHECK_EQ(g_stub.prefs_sets, 2);
        CHECK_EQ(g_stub.prefs.rotate_buttons, false);
    }
    {   // Autorotate page: Manual 90 deg is stored as manual + rotation 1, Auto clears manual
        g_stub = {};
        g_stub.prefs = {true, false};
        Ui ui;
        ui.init(0);
        ui.goto_page(static_cast<uint8_t>(Page::Settings), 1000);
        CHECK_EQ(ui.push_menu(Menu::Autorotate, 1100), true);
        ui.on_button(Button::Plus, ButtonEvent::Short, 1200);   // Manual 0 deg
        ui.on_button(Button::Plus, ButtonEvent::Short, 1300);   // Manual 90 deg
        ui.on_button(Button::Ok, ButtonEvent::Short, 1400);
        CHECK_EQ(ui.rotation(), 1); CHECK_EQ(ui.rotation_locked(), true);
        CHECK_EQ(g_stub.prefs_sets, 1);
        CHECK_EQ(g_stub.prefs.rotation_manual, true); CHECK_EQ(g_stub.prefs.rotation, 1);
        CHECK_EQ(g_stub.prefs.qc_after_pd, true); CHECK_EQ(g_stub.prefs.screenshot, false);
        ui.on_button(Button::Minus, ButtonEvent::Short, 1500);
        ui.on_button(Button::Minus, ButtonEvent::Short, 1600);
        ui.on_button(Button::Ok, ButtonEvent::Short, 1700);     // Auto
        CHECK_EQ(ui.rotation_locked(), false);
        CHECK_EQ(g_stub.prefs_sets, 2);
        CHECK_EQ(g_stub.prefs.rotation_manual, false);
    }
    {   // the +/- hold gesture stores the lock and each quarter-turn, and the return to Auto
        g_stub = {};
        Ui ui;
        ui.init(0);
        chord(ui, 1000);
        CHECK_EQ(g_stub.prefs.rotation_manual, true); CHECK_EQ(g_stub.prefs.rotation, 0);
        chord(ui, 3000);
        CHECK_EQ(g_stub.prefs.rotation_manual, true); CHECK_EQ(g_stub.prefs.rotation, 1);
        chord(ui, 4000); chord(ui, 5000);
        CHECK_EQ(g_stub.prefs.rotation, 3);
        chord(ui, 6000);
        CHECK_EQ(ui.rotation_locked(), false);
        CHECK_EQ(g_stub.prefs.rotation_manual, false);
        CHECK_EQ(g_stub.prefs_sets, 5);
    }
    {   // boot: a stored manual 180 deg + rotated buttons is in force before the first draw, and nothing is rewritten
        g_stub = {};
        Ui ui;
        ui.restore_orientation(true, 2, true);
        ui.init(0);
        CHECK_EQ(ui.rotation(), 2); CHECK_EQ(ui.rotation_locked(), true); CHECK_EQ(ui.rotate_buttons(), true);
        CHECK_EQ(g_stub.prefs_sets, 0);
        for (uint32_t t = 2000; t <= 2400; t += 50) ui.feed_gravity(orientation::Gravity{0, 1000, 0}, t);
        CHECK_EQ(ui.rotation(), 2);   // locked: gravity does not move it
        ui.tick(5000);
        std::array<uint16_t, 240 * 240> booted;   // what the restored boot drew ...
        std::copy(std::begin(g_panel), std::end(g_panel), booted.begin());
        Ui reference;                             // ... is what a plain boot rotated to 180 deg draws
        reference.init(0);
        reference.set_rotate_buttons(true);       // the strip labels follow the setting too
        reference.set_rotation(2, 100);
        reference.tick(5000);
        CHECK_EQ(std::equal(booted.begin(), booted.end(), std::begin(g_panel)), true);
    }
    {   // boot with the defaults (an old record): automatic, buttons as printed, rotation 0
        g_stub = {};
        Ui ui;
        ui.restore_orientation(false, 0, false);
        ui.init(0);
        CHECK_EQ(ui.rotation(), 0); CHECK_EQ(ui.rotation_locked(), false); CHECK_EQ(ui.rotate_buttons(), false);
    }
}

static void check_physical_slot_label(Ui &ui, uint8_t rotation, unsigned slot, Button action) {
    const auto orient = orientation_for(rotation);
    const auto layout = compute_layout(orient.edge, kSlotCenters, kSlotLens, kPanelThickness);
    const Rect box = layout.slot[slot];
    std::array<uint16_t, 240 * 240> actual;
    std::copy(std::begin(g_panel), std::end(g_panel), actual.begin());
    const SlotLabel lab = ui.labels().b[static_cast<int>(action)];
    Surface s{box.x, box.y, box.w, box.h};
    // The reference: the kit's slot painter fed the label of `action` (the mapping under test is which action's label
    // lands in which physical slot; the painter itself is pinned in test_kit).
    kit::draw_key_slot(s, panel_is_column(orient.edge), panel_text_dir(orient.edge), slot, lab, action == Button::Ok, false);
    for (int y = box.y; y < box.y + box.h; ++y)
        CHECK_EQ(std::equal(g_panel + y * 240 + box.x, g_panel + y * 240 + box.x + box.w,
                            actual.begin() + y * 240 + box.x), true);
    std::copy(actual.begin(), actual.end(), std::begin(g_panel));
}

static void test_optional_rotated_buttons() {
    g_stub = {};
    Ui ui;
    ui.init(0);
    CHECK_EQ(ui.rotate_buttons(), false);
    ui.goto_page(static_cast<uint8_t>(Page::Settings), 1000);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1100);
    ui.on_button(Button::Plus, ButtonEvent::Short, 1200);
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Ok)].shrt, "Toggle");
    ui.on_button(Button::Ok, ButtonEvent::Short, 1300);
    CHECK_EQ(ui.rotate_buttons(), true);

    constexpr Button actions[4][4] = {
        {Button::Back, Button::Minus, Button::Ok, Button::Plus},
        {Button::Back, Button::Minus, Button::Ok, Button::Plus},
        {Button::Minus, Button::Ok, Button::Plus, Button::Back},
        {Button::Minus, Button::Ok, Button::Plus, Button::Back},
    };
    for (uint8_t r = 0; r < 4; ++r) {
        ui.set_rotation(r, 2000 + r * 1000);
        ui.goto_page(static_cast<uint8_t>(Page::Tools), 2100 + r * 1000);
        const auto orient = orientation_for(r);
        for (unsigned slot = 0; slot < 4; ++slot) {
            const Button physical = slot_button(orient, slot);
            CHECK_EQ(oriented_button(physical, r, true), actions[r][slot]);
            check_physical_slot_label(ui, r, slot, actions[r][slot]);
            ui.set_pressed(physical, true, 2200 + r * 1000);
            CHECK_EQ(ui.lit(physical), true);
            ui.set_pressed(physical, false, 2400 + r * 1000);
        }
        std::array<uint16_t, 240 * 240> expected;
        std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
        CHECK_EQ(capture_begin(ui), true);
        for (int strip = 0; strip < kCaptureStrips; ++strip) {
            const uint16_t *pixels = capture_strip(ui, strip, 2500 + r * 1000);
            CHECK_EQ(std::equal(pixels, pixels + kCaptureStripPixels,
                                expected.begin() + strip * kCaptureStripPixels), true);
        }
        capture_end(ui, 2500 + r * 1000);
    }

    ui.set_rotation(2, 7000);
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 7100);
    ui.on_button(Button::Plus, ButtonEvent::Short, 7200);
    CHECK_EQ(ui.page(), Page::Detail);   // rotation 2 swaps the direction: Plus flips to the previous face
    ui.on_button(Button::Minus, ButtonEvent::Short, 7300);
    CHECK_EQ(ui.page(), Page::Lines);

    ui.context().keep.bench_sel = static_cast<int>(bench::Place::Charger);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 7400);
    ui.on_button(Button::Minus, ButtonEvent::Short, 7500);
    ui.on_button(Button::Ok, ButtonEvent::Short, 7600);
    CHECK_EQ(ui.menu() == Menu::Connection, true);                       // a place page ...
    CHECK_EQ(ui.context().keep.bench_sel, int(bench::Place::Cable));    // rotation 2 swaps the direction: [-] moves down a row
    ui.on_button(Button::Back, ButtonEvent::Short, 7700);
    CHECK_EQ(int(ui.menu_depth()), 0);   // the Bench list itself
    CHECK_EQ(ui.menu_depth(), 0u);

    const auto before_chord = ui.page();
    chord(ui, 7800);
    CHECK_EQ(ui.page(), before_chord);
    CHECK_EQ(ui.rotation_locked(), true);

    ui.goto_page(static_cast<uint8_t>(Page::Lines), 8000);
    ui.set_pressed(Button::Plus, true, 8100);
    ui.set_rotation(0, 8200);
    ui.on_button(Button::Plus, ButtonEvent::Short, 8300);   // the press that crossed the rotation does nothing
    CHECK_EQ(ui.page(), Page::Lines);
    ui.set_pressed(Button::Plus, false, 8400);
    ui.set_pressed(Button::Plus, true, 8500);
    ui.on_button(Button::Plus, ButtonEvent::Short, 9100);   // a fresh one at rotation 0: Dn flips to the next face
    CHECK_EQ(ui.page(), Page::Home);
    ui.set_pressed(Button::Plus, false, 9200);

    ui.set_rotation(2, 9300);
    ui.goto_page(static_cast<uint8_t>(Page::Settings), 9350);
    CHECK_STR(ui.labels().b[static_cast<int>(Button::Ok)].shrt, "Toggle");
    ui.on_button(Button::Ok, ButtonEvent::Short, 9375);
    CHECK_EQ(ui.rotate_buttons(), false);
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 9400);
    ui.on_button(Button::Plus, ButtonEvent::Short, 9500);
    CHECK_EQ(ui.page(), Page::Home);
    Ui fresh;
    fresh.init(10000);
    CHECK_EQ(fresh.rotate_buttons(), false);
    fresh.set_rotate_buttons(true);
    for (uint32_t t = 10100; t <= 10500; t += 50)
        fresh.feed_gravity(orientation::Gravity{0, -1000, 0}, t);
    CHECK_EQ(fresh.rotation(), 2);
    fresh.goto_page(static_cast<uint8_t>(Page::Lines), 10600);
    fresh.on_button(Button::Plus, ButtonEvent::Short, 10700);
    CHECK_EQ(fresh.page(), Page::Detail);
}

static void test_rotated_fc_draft() {
    g_stub = {};
    g_stub.pm_online = true;
    g_stub.pm.vbus_mv = 5000;
    Ui ui;
    ui.init(0);
    ui.tick(1000);
    ui.set_rotate_buttons(true);
    ui.set_rotation(2, 1100);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 1200);
    CHECK_EQ(ui.push_menu(Menu::Trigger, 1300), true);
    ui.on_button(Button::Minus, ButtonEvent::Short, 1400);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1500);
    ui.on_button(Button::Minus, ButtonEvent::Short, 1600);
    ui.on_button(Button::Minus, ButtonEvent::Long, 1700);
    CHECK_EQ(g_stub.fc_starts, 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1800);
    ui.on_button(Button::Ok, ButtonEvent::Short, 1900);
    CHECK_EQ(g_stub.fc_starts, 0);
    ui.set_rotation(0, 1950);
    ui.on_button(Button::Ok, ButtonEvent::Short, 2000);
    CHECK_EQ(g_stub.fc_starts, 0);
    ui.on_button(Button::Ok, ButtonEvent::Short, 2100);
    CHECK_EQ(g_stub.fc_starts, 1);
    CHECK_EQ(g_stub.fc_start_mv, 10200);
    CHECK_EQ(g_stub.fc_confirmed, true);
}

static void test_fc_stages_in_four_rotations() {
    for (uint8_t rotation = 0; rotation < 4; ++rotation) {
        g_stub = {};
        g_stub.pm_online = true; g_stub.pm.vbus_mv = 5000;
        Ui ui; ui.init(0);
        uint32_t now = 6000;
        ui.tick(now);
        ui.set_rotation(rotation, now);
        ui.goto_page(static_cast<uint8_t>(Page::Tools), now);
        CHECK_EQ(ui.push_menu(Menu::Trigger, now), true);
        auto key = [&](Button b, ButtonEvent p = ButtonEvent::Short) { ui.on_button(b, p, now += 100); };
        auto snapshot = [&]() {
            std::array<uint16_t, 240 * 240> expected;
            std::copy(std::begin(g_panel), std::end(g_panel), expected.begin());
            CHECK_EQ(capture_begin(ui), true);
            for (int strip = 0; strip < kCaptureStrips; ++strip) {
                const auto *pixels = capture_strip(ui, strip, now);
                CHECK_EQ(std::equal(pixels, pixels + kCaptureStripPixels,
                                    expected.begin() + strip * kCaptureStripPixels), true);
            }
            capture_end(ui, now);
            CHECK_EQ(ui.rotation(), rotation);
        };
        snapshot();
        key(Button::Plus); key(Button::Ok); // QC3 editor
        char label[24];
        label_text(label, ui.labels().b[int(Button::Minus)], label_max_chars(Button::Minus));
        CHECK_STR(label, "-/Big*");
        key(Button::Plus); key(Button::Plus, ButtonEvent::Long); key(Button::Plus, ButtonEvent::Hold);
        CHECK_EQ(g_stub.fc_starts, 0);
        snapshot();
        key(Button::Ok); key(Button::Ok);   // review, arm
        chord(ui, now);
        now += 700;
        key(Button::Ok);
        CHECK_EQ(g_stub.fc_starts, 0);       // chord invalidated the arm
        key(Button::Ok);
        CHECK_EQ(g_stub.fc_starts, 1);
        CHECK_EQ(g_stub.fc_start_mv, 10200); // same physical +/- meaning in all orientations
        CHECK_EQ(g_stub.fc_confirmed, true);
        ui.tick(now += 100);
        snapshot();                         // negotiating: no live adjustment
        g_stub.fc_state = int(fc::State::Active);
        g_stub.pm.vbus_mv = 10200;
        ui.tick(now += 100);
        key(Button::Minus);                 // draft 10V, held10.2V
        CHECK_EQ(g_stub.fc_retargets, 0);
        snapshot();
        key(Button::Ok); key(Button::Ok);
        CHECK_EQ(g_stub.fc_retargets, 0);
        snapshot();
        key(Button::Ok);
        CHECK_EQ(g_stub.fc_retargets, 1);
        CHECK_EQ(g_stub.fc_retarget_mv, 10000);
        key(Button::Back, ButtonEvent::Long);
        CHECK_EQ(g_stub.fc_stops, 1);
        CHECK_EQ(ui.ports_busy(), true);
        key(Button::Back);
        snapshot();                         // cleanup warning also captures after exit
        g_stub.pm.vbus_mv = 5000;
        for (int n = 0; n < 6; ++n) ui.tick(now += 100);
        CHECK_EQ(ui.ports_busy(), false);
    }
}

// A remote press is a logical key: plus = next/down, minus = previous/up, whatever the rotation and the
// Rotate buttons setting. A physical key keeps the rotated mapping, and the remote can never make the
// rotation gesture.
static void test_remote_keys_are_logical() {
    g_stub = {};
    Ui ui;
    ui.init(0);
    ui.tick(1000);
    ui.set_rotate_buttons(true);
    ui.set_rotation(2, 1100);
    CHECK_EQ(ui.rotation(), 2);

    // The Bench cursor persists, so each key goes to a fresh UI at rotation 2 with Rotate buttons on and the place it
    // opens is compared. Remote plus moves down (Charger -> Cable), as in the default orientation.
    auto pick = [](bool remote, Button key) {
        g_stub = {};
        Ui u;
        u.init(0);
        u.tick(1000);
        u.set_rotate_buttons(true);
        u.set_rotation(2, 1100);
        u.goto_page(static_cast<uint8_t>(Page::Tools), 1200);   // the Bench opens on Charger
        if (remote) { u.on_remote_button(key, ButtonEvent::Short, 1300); u.on_remote_button(Button::Ok, ButtonEvent::Short, 1400); }
        else        { u.on_button(key, ButtonEvent::Short, 1300);        u.on_button(Button::Ok, ButtonEvent::Short, 1400); }
        return u.menu() == Menu::Connection ? u.context().keep.bench_sel : -1;   // a place page opened over the Bench
    };
    const int cable = static_cast<int>(bench::Place::Cable);
    CHECK_EQ(pick(true, Button::Plus) == cable, true);    // remote plus = down
    CHECK_EQ(pick(false, Button::Minus) == cable, true);  // physical minus at rotation 2 = down
    CHECK_EQ(pick(false, Button::Plus) == cable, false);  // physical plus at rotation 2 = up
    CHECK_EQ(pick(true, Button::Minus) == pick(false, Button::Plus), true);   // remote minus = up, like that key

    // Remote plus flips to the next face (Lines -> Flow) at rotation 2, where a physical plus flips back.
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 3000);
    ui.on_remote_button(Button::Plus, ButtonEvent::Short, 3100);
    CHECK_EQ(ui.page(), Page::Home);
    ui.on_remote_button(Button::Minus, ButtonEvent::Short, 3200);
    CHECK_EQ(ui.page(), Page::Lines);
    ui.on_button(Button::Plus, ButtonEvent::Short, 3300);
    CHECK_EQ(ui.page(), Page::Detail);

    // Remote plus and minus together (overlapping long presses) are two logical keys, never the rotation gesture.
    ui.goto_page(static_cast<uint8_t>(Page::Lines), 4000);
    ui.set_remote_pressed(Button::Plus, true, 4000);
    ui.set_remote_pressed(Button::Minus, true, 4000);
    ui.on_remote_button(Button::Minus, ButtonEvent::Long, 4500);
    ui.on_remote_button(Button::Plus, ButtonEvent::Long, 4500);
    ui.set_remote_pressed(Button::Plus, false, 4600);
    ui.set_remote_pressed(Button::Minus, false, 4600);
    ui.tick(4700);
    CHECK_EQ(ui.rotation(), 2);
    CHECK_EQ(ui.rotation_locked(), false);
    ui.tick(5000);
    CHECK_EQ(ui.lit(Button::Plus), false);
    CHECK_EQ(ui.lit(Button::Minus), false);

    // A remote key held while a physical one is pressed does not join it into a chord.
    ui.set_pressed(Button::Minus, true, 5100);
    ui.set_remote_pressed(Button::Plus, true, 5100);
    ui.on_button(Button::Minus, ButtonEvent::Long, 5600);
    CHECK_EQ(ui.rotation_locked(), false);
    ui.set_remote_pressed(Button::Plus, false, 5700);
    ui.set_pressed(Button::Minus, false, 5700);

    // The key that lights on the strip is the one that carries the action: remote plus at rotation 2 with
    // Rotate buttons lights the physical minus slot.
    ui.set_remote_pressed(Button::Plus, true, 6000);
    CHECK_EQ(ui.lit(Button::Minus), true);
    CHECK_EQ(ui.lit(Button::Plus), false);
    ui.set_rotation(0, 6050);                 // the rotation changes under a held remote key
    ui.set_remote_pressed(Button::Plus, false, 6100);
    ui.tick(6300);
    CHECK_EQ(ui.lit(Button::Minus), false);
    CHECK_EQ(ui.lit(Button::Plus), false);

    // Back and OK are the same key either way, and Back still counts as held for the reboot card.
    ui.set_remote_pressed(Button::Back, true, 7000);
    CHECK_EQ(ui.lit(Button::Back), true);
    ui.set_remote_pressed(Button::Back, false, 7010);
    ui.tick(7300);
    CHECK_EQ(ui.lit(Button::Back), false);

    // Rotation 0 with the setting on or off: logical equals physical.
    ui.set_rotation(0, 8000);
    ui.context().keep.bench_sel = static_cast<int>(bench::Place::Charger);
    ui.goto_page(static_cast<uint8_t>(Page::Tools), 8100);
    ui.on_remote_button(Button::Plus, ButtonEvent::Short, 8200);
    ui.on_remote_button(Button::Ok, ButtonEvent::Short, 8300);
    CHECK_EQ(ui.menu() == Menu::Connection, true);   // the Cable place page
    CHECK_EQ(ui.context().keep.bench_sel, int(bench::Place::Cable));
}

int main() {
    test_remote_keys_are_logical();
    test_native_menu_fonts();
    test_static_flow_indicator();
    test_viewport();
    test_captures_and_keys();
    test_lock_and_feed();
    test_physical_flow_pixels();
    test_physical_dash();
    test_physical_chevron_mask();
    test_settings_rotation_modes();
    test_rotation_prefs_persist();
    test_optional_rotated_buttons();
    test_rotated_fc_draft();
    test_fc_stages_in_four_rotations();
    return test_report("test_ui_rotation");
}
