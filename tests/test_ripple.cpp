// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for pure VBUS ripple analysis (k2::ripple) and the Ripple Meter face (RipplePage).
#include <array>
#include <cmath>
#include <cstdint>

#include "k2/core/ripple.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/drivers/lcd.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_style.hpp"
#include "fake_lcd.h"
#include "test_util.h"
#include "ui_stubs.h"

namespace {

using namespace k2;

void fill_sine(std::array<uint16_t, adc::kRippleSamples> &buf, double freq_hz, uint32_t period_ns,
               double amp_counts, double phase = 0.0) {
    const double dt = static_cast<double>(period_ns) * 1e-9;
    for (size_t i = 0; i < buf.size(); i++) {
        const double v = 2056.0 + amp_counts * std::sin(2.0 * 3.14159265358979323846 * freq_hz * (i * dt) + phase);
        buf[i] = static_cast<uint16_t>(v < 0 ? 0 : (v > 4095 ? 4095 : v + 0.5));
    }
}

void test_below_floor_noise() {
    std::array<uint16_t, adc::kRippleSamples> buf{};
    for (size_t i = 0; i < buf.size(); i++) buf[i] = static_cast<uint16_t>(2056 + ((i & 1) ? 4 : -4));
    // Huge mux-settling transient in the first samples must be ignored.
    buf[0] = 1600;
    buf[1] = 2600;
    const uint32_t period_ns = adc::sample_period_ns(5, adc::kAdcClockHz);
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, 4, ripple::Trigger::Auto);
    CHECK_COND(st.valid);
    CHECK_COND(st.below_floor);
    CHECK_EQ(st.pp_raw, 8);
    CHECK_EQ(st.freq_hz, 0u);
    StrBuf<16> s;
    ripple::fmt_vpp(st, s);
    CHECK_STR(s.c_str(), "< floor");
    ripple::fmt_freq(st, s);
    CHECK_STR(s.c_str(), "--");
}

void test_sine_vpp_rms_freq_and_trigger() {
    // 100 kHz GaN switching ripple, amplitude 12 counts (24 counts p-p ~ 19 mVpp at vref=1489 -> VDD=3300mV).
    std::array<uint16_t, adc::kRippleSamples> buf{};
    const uint32_t period_ns = adc::sample_period_ns(0, adc::kAdcClockHz);   // 933 ns (1.07 MS/s)
    fill_sine(buf, 100000.0, period_ns, 12.0, 1.2);
    buf[0] = 1900;   // mux-settling glitch must not affect Vpp or frequency
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, ripple::kDefaultTimebase, ripple::Trigger::Auto);
    CHECK_COND(st.valid);
    CHECK_COND(!st.below_floor);
    CHECK_COND(st.vpp_mv >= 17 && st.vpp_mv <= 22);
    CHECK_COND(st.vrms_mv >= 5 && st.vrms_mv <= 9);
    CHECK_COND(st.freq_hz >= 95000u && st.freq_hz <= 105000u);
    CHECK_COND(st.trigger_idx > 0 && st.trigger_idx < 50);

    const ripple::Stats st_free = ripple::analyze(buf, period_ns, 1489, ripple::kDefaultTimebase, ripple::Trigger::Free);
    CHECK_EQ(st_free.trigger_idx, 0u);

    CHECK_EQ(ripple::resolve_mv_per_div(0, 10), 10u);
    CHECK_EQ(ripple::resolve_mv_per_div(0, 80), 50u);
    CHECK_EQ(ripple::resolve_mv_per_div(0, 90), 100u);
    CHECK_EQ(ripple::resolve_mv_per_div(1, 500), 10u);
}

void test_project_columns_span_and_continuity() {
    std::array<uint16_t, adc::kRippleSamples> buf{};
    const uint32_t period_ns = adc::sample_period_ns(5, adc::kAdcClockHz);
    fill_sine(buf, 4000.0, period_ns, 120.0);
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, 2, ripple::Trigger::Auto);
    constexpr int16_t kTestPlotW = 204;
    constexpr uint8_t kTestPlotH = 192;
    std::array<ripple::ColSpan, kTestPlotW> cols{};
    ripple::project(buf, period_ns, 1489, st, 2, 50, kTestPlotH, cols);
    uint8_t top = 255, bot = 0;
    for (size_t x = 0; x < cols.size(); x++) {
        CHECK_COND(cols[x].lo <= cols[x].hi);
        CHECK_COND(cols[x].hi < kTestPlotH);
        if (cols[x].lo < top) top = cols[x].lo;
        if (cols[x].hi > bot) bot = cols[x].hi;
    }
    CHECK_COND(top < kTestPlotH / 4);
    CHECK_COND(bot > (kTestPlotH * 3) / 4);
}

void test_auto_timebase_resolution() {
    CHECK_STR(ripple::timebase_menu_label(0), "Auto");
    for (uint8_t i = 1; i < ripple::kTimebaseSettingCount; i++) {
        CHECK_STR(ripple::timebase_menu_label(i), ripple::kTimebases[i - 1].menu_label);
    }

    ripple::Stats st{};
    st.valid = true;
    st.below_floor = false;

    // Frequencies outside hysteresis should pick the smallest timebase with >= 2.5 cycles in 4 divs.
    st.freq_hz = 60000;
    CHECK_EQ(ripple::resolve_auto_timebase(4, st), 0u);   // 25us (6.0 cycles)
    st.freq_hz = 24000;
    CHECK_EQ(ripple::resolve_auto_timebase(4, st), 1u);   // 25us has 2.4 cycles (< 2.5) -> 50us (4.8 cycles)
    st.freq_hz = 5400;
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 3u);   // 200us (4.3 cycles)
    st.freq_hz = 1200;
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 5u);   // 1ms (4.8 cycles)
    st.freq_hz = 100;
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 6u);   // 2ms (clamped at slowest)

    // Hysteresis: 1.5..8.0 cycles across 4 divs stays put on cur_tb.
    st.freq_hz = 10000;   // at 50us (200us span): 2.0 cycles
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 1u);
    st.freq_hz = 35000;   // at 50us (200us span): 7.0 cycles
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 1u);

    // Below-floor signal stays put (don't hunt on flat noise).
    st.below_floor = true;
    st.freq_hz = 60000;
    CHECK_EQ(ripple::resolve_auto_timebase(3, st), 3u);

    // Above-floor with 0 crossings steps one timebase slower, clamped at kTimebaseCount - 1.
    st.below_floor = false;
    st.freq_hz = 0;
    CHECK_EQ(ripple::resolve_auto_timebase(1, st), 2u);
    CHECK_EQ(ripple::resolve_auto_timebase(ripple::kTimebaseCount - 1, st), ripple::kTimebaseCount - 1u);
}

void test_formatters() {
    ripple::Stats s;
    s.valid = true;
    s.below_floor = false;
    s.vpp_mv = 1240;
    s.vrms_mv = 438;
    s.freq_hz = 62500;
    StrBuf<16> out;
    ripple::fmt_vpp(s, out);
    CHECK_STR(out.c_str(), "1.24 Vpp");
    ripple::fmt_vrms(s, out);
    CHECK_STR(out.c_str(), "438 mVrms");
    ripple::fmt_freq(s, out);
    CHECK_STR(out.c_str(), "62.5 kHz");

    s.freq_hz = 125400;
    ripple::fmt_freq(s, out);
    CHECK_STR(out.c_str(), "125 kHz");

    s.freq_hz = 420;
    ripple::fmt_freq(s, out);
    CHECK_STR(out.c_str(), "420 Hz");
}

static uint8_t g_last_sample_code = 0xFF;
static int g_capture_calls = 0;
static uint32_t fake_sine_capture(std::span<uint16_t, adc::kRippleSamples> out, uint8_t sample_code) {
    g_last_sample_code = sample_code;
    g_capture_calls++;
    const uint32_t period_ns = adc::sample_period_ns(sample_code, adc::kAdcClockHz);
    std::array<uint16_t, adc::kRippleSamples> tmp{};
    fill_sine(tmp, 5000.0, period_ns, 100.0);
    for (size_t i = 0; i < out.size(); i++) out[i] = tmp[i];
    return period_ns;
}

void test_ripple_page_navigation_and_settings() {
    g_stub = {};
    g_stub.ripple_fn = &fake_sine_capture;
    g_capture_calls = 0;
    g_last_sample_code = 0xFF;
    adc::Reading ar{};
    ar.vref_raw = 1489;
    g_stub.adc = ar;

    Ui ui;
    uint32_t t = 1000;
    lcd::set_spi_div(4);
    ui.init(t);
    ui.goto_page(static_cast<uint8_t>(Page::Ripple), t);
    CHECK_COND(ui.page() == Page::Ripple);
    // Default is Auto (0), which starts at kDefaultTimebase (code 0) and auto-switches 5 kHz -> 200us (tb 3, code 2).
    for (int i = 0; i < 6; i++) { t += 100; ui.tick(t); }
    CHECK_COND(g_capture_calls >= 2);
    CHECK_EQ(g_last_sample_code, ripple::kTimebases[3].sample_code);
    // SPI3 boosted to /2 (60 MHz) while Ripple is active.
    CHECK_EQ(lcd::spi_div(), 2u);

    // Check portrait (rot 0, content 208x240 at x0=16, y0=0) frame borders: x=1..206, y=45..238 -> panel x=17..222, y=45..238.
    CHECK_EQ(g_panel[45 * 240 + 17], kRule);
    CHECK_EQ(g_panel[45 * 240 + 222], kRule);
    CHECK_EQ(g_panel[238 * 240 + 17], kRule);
    CHECK_EQ(g_panel[238 * 240 + 222], kRule);

    // Check landscape (rot 1, content 240x208 at x0=0, y0=16) frame borders: x=1..238, y=45..206 -> panel x=1..238, y=61..222.
    ui.set_rotation(1, t);
    for (int i = 0; i < 10; i++) { t += 100; ui.tick(t); }
    CHECK_EQ(g_panel[61 * 240 + 1], kRule);
    CHECK_EQ(g_panel[61 * 240 + 238], kRule);
    CHECK_EQ(g_panel[222 * 240 + 1], kRule);
    CHECK_EQ(g_panel[222 * 240 + 238], kRule);
    ui.set_rotation(0, t);
    for (int i = 0; i < 10; i++) { t += 100; ui.tick(t); }

    CHECK_COND(label_violation(ui.labels()) == nullptr);

    // Short OK opens Settings overlay.
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    t += 100; ui.tick(t);
    CHECK_COND(label_violation(ui.labels()) == nullptr);

    // The settings rows stop at their ends: Up dim on the first row, Dn on the last.
    const auto dim = [&](Button b) { return ui.labels().b[static_cast<int>(b)].dim; };
    CHECK_EQ(dim(Button::Minus), true);  CHECK_EQ(dim(Button::Plus), false);
    ui.on_button(Button::Minus, ButtonEvent::Short, t);   // no wrap to the last row
    CHECK_EQ(dim(Button::Minus), true);  CHECK_EQ(dim(Button::Plus), false);
    for (int i = 0; i < 4; i++) ui.on_button(Button::Plus, ButtonEvent::Short, t);   // past the last row: stays
    CHECK_EQ(dim(Button::Minus), false); CHECK_EQ(dim(Button::Plus), true);
    for (int i = 0; i < 3; i++) ui.on_button(Button::Minus, ButtonEvent::Short, t);  // back on row 0
    CHECK_EQ(dim(Button::Minus), true);  CHECK_EQ(dim(Button::Plus), false);

    // Move to row 1 (Time / div) and cycle through all 8 settings: Auto (0) -> 25us (1) -> ... -> 2ms (7) -> Auto (0).
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    for (uint8_t s = 0; s < ripple::kTimebaseSettingCount; s++) {
        ui.on_button(Button::Ok, ButtonEvent::Short, t);
    }

    // Cycle 4 times from Auto (0) to select 200us fixed (setting 4 -> kTimebases[3], code 2).
    for (int i = 0; i < 4; i++) ui.on_button(Button::Ok, ButtonEvent::Short, t);
    // BACK closes settings and triggers fresh capture with code 2.
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    const int before = g_capture_calls;
    t += 100; ui.tick(t);
    CHECK_COND(g_capture_calls > before);
    CHECK_EQ(g_last_sample_code, ripple::kTimebases[3].sample_code);

    // Long OK toggles Hold: no new captures while held.
    ui.on_button(Button::Ok, ButtonEvent::Long, t);
    const int held_calls = g_capture_calls;
    for (int i = 0; i < 5; i++) { t += 300; ui.tick(t); }
    CHECK_EQ(g_capture_calls, held_calls);

    // Long OK again resumes live capture.
    ui.on_button(Button::Ok, ButtonEvent::Long, t);
    t += 100; ui.tick(t);
    CHECK_COND(g_capture_calls > held_calls);

    // Back leaves the entered page for the ring (still Ripple, still boosted); Plus then steps on to the Charger place,
    // which leaves Ripple and restores the SPI3 divider to /4.
    ui.on_button(Button::Back, ButtonEvent::Short, t);
    CHECK_COND(ui.ring_view());
    CHECK_COND(ui.page() == Page::Ripple);
    CHECK_EQ(lcd::spi_div(), 2u);
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    t += 100; ui.tick(t);
    CHECK_COND(ui.page() != Page::Ripple);
    CHECK_EQ(lcd::spi_div(), 4u);
}

}  // namespace

int main() {
    test_below_floor_noise();
    test_sine_vpp_rms_freq_and_trigger();
    test_project_columns_span_and_continuity();
    test_auto_timebase_resolution();
    test_formatters();
    test_ripple_page_navigation_and_settings();
    return test_report("test_ripple");
}
