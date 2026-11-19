// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for pure VBUS ripple analysis (k2::ripple) and the Ripple Meter face (RipplePage).
#include <array>
#include <cmath>
#include <cstdint>

#include "k2/core/ripple.hpp"
#include "k2/drivers/adc.hpp"
#include "k2/ui/ui.hpp"
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
    for (size_t i = 0; i < buf.size(); i++) buf[i] = static_cast<uint16_t>(2056 + ((i & 1) ? 20 : -20));
    const uint32_t period_ns = adc::sample_period_ns(5, adc::kAdcClockHz);
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, 2, ripple::Trigger::Auto);
    CHECK_COND(st.valid);
    CHECK_COND(st.below_floor);
    CHECK_EQ(st.pp_raw, 40);
    CHECK_EQ(st.freq_hz, 0u);
    StrBuf<16> s;
    ripple::fmt_vpp(st, s);
    CHECK_STR(s.c_str(), "< floor");
    ripple::fmt_freq(st, s);
    CHECK_STR(s.c_str(), "--");
}

void test_sine_vpp_rms_freq_and_trigger() {
    // 5 kHz sine, amplitude 100 counts (200 counts p-p ~ 161 mVpp at vref=1489 -> VDD=3300mV).
    std::array<uint16_t, adc::kRippleSamples> buf{};
    const uint32_t period_ns = adc::sample_period_ns(5, adc::kAdcClockHz);   // ~4533 ns
    fill_sine(buf, 5000.0, period_ns, 100.0, 1.2);   // start mid-cycle so trigger must advance
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, 2, ripple::Trigger::Auto);
    CHECK_COND(st.valid);
    CHECK_COND(!st.below_floor);
    CHECK_COND(st.vpp_mv >= 158 && st.vpp_mv <= 164);
    CHECK_COND(st.vrms_mv >= 54 && st.vrms_mv <= 60);
    CHECK_COND(st.freq_hz >= 4750u && st.freq_hz <= 5250u);
    CHECK_COND(st.trigger_idx > 0 && st.trigger_idx < 100);

    const ripple::Stats st_free = ripple::analyze(buf, period_ns, 1489, 2, ripple::Trigger::Free);
    CHECK_EQ(st_free.trigger_idx, 0u);

    CHECK_EQ(ripple::resolve_mv_per_div(0, 80), 50u);
    CHECK_EQ(ripple::resolve_mv_per_div(0, 90), 100u);
    CHECK_EQ(ripple::resolve_mv_per_div(1, 500), 20u);
}

void test_project_columns_span_and_continuity() {
    std::array<uint16_t, adc::kRippleSamples> buf{};
    const uint32_t period_ns = adc::sample_period_ns(5, adc::kAdcClockHz);
    fill_sine(buf, 4000.0, period_ns, 120.0);
    const ripple::Stats st = ripple::analyze(buf, period_ns, 1489, 2, ripple::Trigger::Auto);
    std::array<ripple::ColSpan, RipplePage::kPlotW> cols{};
    ripple::project(buf, period_ns, 1489, st, 2, 50, RipplePage::kPlotH, cols);
    uint8_t top = 255, bot = 0;
    for (size_t x = 0; x < cols.size(); x++) {
        CHECK_COND(cols[x].lo <= cols[x].hi);
        CHECK_COND(cols[x].hi < RipplePage::kPlotH);
        if (cols[x].lo < top) top = cols[x].lo;
        if (cols[x].hi > bot) bot = cols[x].hi;
    }
    CHECK_COND(top < RipplePage::kPlotH / 4);
    CHECK_COND(bot > (RipplePage::kPlotH * 3) / 4);
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
    ui.init(t);
    ui.goto_page(static_cast<uint8_t>(Page::Ripple), t);
    CHECK_COND(ui.page() == Page::Ripple);
    for (int i = 0; i < 6; i++) { t += 100; ui.tick(t); }
    CHECK_COND(g_capture_calls >= 1);
    CHECK_EQ(g_last_sample_code, ripple::kTimebases[ripple::kDefaultTimebase].sample_code);

    CHECK_COND(label_violation(ui.labels()) == nullptr);

    // Short OK opens Settings overlay.
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    t += 100; ui.tick(t);
    CHECK_COND(label_violation(ui.labels()) == nullptr);

    // Move to row 1 (Time / div) and cycle it: 500 us/div (idx 2) -> 1 ms/div (idx 3, code 7).
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    ui.on_button(Button::Ok, ButtonEvent::Short, t);
    // BACK closes settings and triggers fresh capture with code 7.
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

    // Plus flips to next Meter face (wraps back to Home).
    ui.on_button(Button::Plus, ButtonEvent::Short, t);
    CHECK_COND(ui.page() == Page::Home);
}

}  // namespace

int main() {
    test_below_floor_noise();
    test_sine_vpp_rms_freq_and_trigger();
    test_project_columns_span_and_continuity();
    test_formatters();
    test_ripple_page_navigation_and_settings();
    return test_report("test_ripple");
}
