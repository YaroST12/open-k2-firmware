// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Host tests for the display smoothing (k2/ui/smoothing.hpp): the One Euro filter (Casiez, Roussel, Vogel,
// CHI 2012) and its three presets. Signals are sampled every 20 ms like power::poll(). Expected figures come
// from the requirements, not from the filter: Off is identity, a 0.5 V drop shows within a few samples at every
// preset, a 4 V step passes at once, quiet-signal noise is reduced, a PPS-style ramp is followed closely.
#include <cmath>
#include <cstdint>
#include <initializer_list>

#include "k2/ui/smoothing.hpp"
#include "test_util.h"

using namespace k2::smoothing;

namespace {

constexpr std::uint32_t kPeriodMs = 20;
constexpr Preset kOn[] = {Preset::Low, Preset::Medium, Preset::High};

// Deterministic uniform noise in [-amp, +amp] (the ADC's +-20 mV class jitter).
struct Noise {
    std::uint32_t s = 12345;
    float next(float amp) {
        s = s * 1664525u + 1013904223u;
        return amp * (static_cast<float>(s >> 8) / static_cast<float>(1u << 23) - 1.0f);
    }
};

OneEuro make(Preset p) {
    OneEuro f;
    f.configure(params(p));
    return f;
}

// Settle on `from` for 2 s, then feed `to`; returns the filter output as a fraction of the step after n samples.
float step_fraction(Preset p, float from, float to, int n) {
    OneEuro f = make(p);
    std::uint32_t t = 0;
    for (int i = 0; i < 100; i++, t += kPeriodMs) f.update(from, t);
    float y = from;
    for (int i = 0; i < n; i++, t += kPeriodMs) y = f.update(to, t);
    return (y - from) / (to - from);
}

void test_off_is_identity() {
    OneEuro f = make(Preset::Off);
    CHECK_EQ(f.update(5.0f, 0) == 5.0f, 1);
    CHECK_EQ(f.update(9.1234f, 20) == 9.1234f, 1);
    CHECK_EQ(f.update(-0.25f, 20) == -0.25f, 1);       // same timestamp twice, still identity
    CHECK_EQ(f.update(20.0f, 100000) == 20.0f, 1);

    Readout r;
    r.set(Preset::Off);
    r.push(9123, -1876, 0);
    r.push(8501, 4, 20);
    CHECK_EQ(r.vbus_mv(), 8501);
    CHECK_EQ(r.current_ma(), 4);
}

void test_constant_input_is_a_fixed_point() {
    for (Preset p : kOn) {
        OneEuro f = make(p);
        float y = 0;
        for (std::uint32_t t = 0; t < 2000; t += kPeriodMs) y = f.update(9.0f, t);
        CHECK_EQ(std::fabs(y - 9.0f) < 1e-6f, 1);
    }
}

void test_first_sample_seeds_the_filter() {
    for (Preset p : kOn) {
        OneEuro f = make(p);
        CHECK_EQ(f.update(20.0f, 5000) == 20.0f, 1);   // no ramp up from zero at power-on
    }
}

// 0.5 V drop (and rise): visible at once, within 2 % after four samples (80 ms), at every preset.
void test_half_volt_step_passes_quickly() {
    const struct { Preset p; float first, second; } want[] = {
        {Preset::Low, 0.90f, 0.97f}, {Preset::Medium, 0.85f, 0.97f}, {Preset::High, 0.70f, 0.90f}};
    for (const auto& w : want) {
        for (float dir : {-1.0f, 1.0f}) {
            const float to = 9.0f + 0.5f * dir;
            CHECK_EQ(step_fraction(w.p, 9.0f, to, 1) >= w.first, 1);
            CHECK_EQ(step_fraction(w.p, 9.0f, to, 2) >= w.second, 1);
            CHECK_EQ(step_fraction(w.p, 9.0f, to, 4) >= 0.98f, 1);
        }
    }
}

// A source switching 5 V -> 9 V -> 20 V, or dropping 4 V: no visible lag.
void test_big_steps_pass_at_once() {
    for (Preset p : kOn) {
        CHECK_EQ(step_fraction(p, 5.0f, 9.0f, 1) >= 0.95f, 1);
        CHECK_EQ(step_fraction(p, 20.0f, 16.0f, 1) >= 0.95f, 1);
        CHECK_EQ(step_fraction(p, 9.0f, 0.0f, 2) >= 0.99f, 1);   // a collapse to 0 V
    }
}

// A 1 A load step (current in amps) and a 0.1 A drop.
void test_current_steps() {
    for (Preset p : kOn) {
        CHECK_EQ(step_fraction(p, 1.0f, 2.0f, 2) >= 0.95f, 1);
        CHECK_EQ(step_fraction(p, 1.0f, 0.0f, 4) >= 0.98f, 1);
    }
    CHECK_EQ(step_fraction(Preset::Low, 1.0f, 0.9f, 4) >= 0.90f, 1);
}

// +-20 mV uniform jitter on 5 V (rms 11.5 mV): rms output / rms input per preset.
float noise_ratio(Preset p, float amp, float level) {
    OneEuro f = make(p);
    Noise n;
    double in2 = 0, out2 = 0;
    std::uint32_t t = 0;
    for (int i = 0; i < 20000; i++, t += kPeriodMs) {
        const float x = level + n.next(amp);
        const float y = f.update(x, t);
        if (i < 500) continue;   // let the filter settle
        in2 += double(x - level) * (x - level);
        out2 += double(y - level) * (y - level);
    }
    return static_cast<float>(std::sqrt(out2 / in2));
}

void test_noise_is_reduced() {
    const float low = noise_ratio(Preset::Low, 0.020f, 5.0f);
    const float med = noise_ratio(Preset::Medium, 0.020f, 5.0f);
    const float high = noise_ratio(Preset::High, 0.020f, 5.0f);
    CHECK_EQ(low <= 0.72f, 1);
    CHECK_EQ(med <= 0.50f, 1);
    CHECK_EQ(high <= 0.42f, 1);
    CHECK_EQ(low > med && med > high, 1);              // the presets are ordered Low < Medium < High smoothing
    // the same on a current of 1 A with +-10 mA jitter
    CHECK_EQ(noise_ratio(Preset::Medium, 0.010f, 1.0f) <= 0.50f, 1);
}

// A PPS-style ramp of 1 V/s (20 mV per sample): the smoothed value trails by less than 0.1 V.
void test_ramp_is_followed() {
    for (Preset p : kOn) {
        OneEuro f = make(p);
        float x = 5.0f, y = 5.0f;
        std::uint32_t t = 0;
        for (int i = 0; i < 250; i++, t += kPeriodMs) { y = f.update(x, t); x += 0.020f; }
        CHECK_EQ(std::fabs((x - 0.020f) - y) < 0.1f, 1);
    }
}

// Timing robustness: the same millisecond twice, a long gap, and the 32-bit millisecond wrap.
void test_timestamps() {
    OneEuro f = make(Preset::Medium);
    f.update(9.0f, 100);
    const float a = f.update(8.5f, 100);               // dt = 0 must not divide by zero
    CHECK_EQ(std::isfinite(a) ? 1 : 0, 1);
    const float b = f.update(5.0f, 100 + 10000);       // a 10 s gap: the value is simply the new reading
    CHECK_EQ(std::fabs(b - 5.0f) < 0.05f, 1);

    OneEuro g = make(Preset::Medium);
    std::uint32_t t = 0xFFFFFFF0u;
    for (int i = 0; i < 100; i++, t += kPeriodMs) g.update(9.0f, t);      // crosses the wrap
    const float c = g.update(8.5f, t);
    CHECK_EQ(c < 9.0f - 0.85f * 0.5f, 1);
}

// Readout: whole units in and out, preset change re-seeds from the next reading.
void test_readout_rounds_and_reseeds() {
    Readout r;
    CHECK_EQ(r.preset() == Preset::Medium, 1);          // the shipped default
    std::uint32_t t = 0;
    for (int i = 0; i < 100; i++, t += kPeriodMs) r.push(9000, 1500, t);
    CHECK_EQ(r.vbus_mv(), 9000);
    CHECK_EQ(r.current_ma(), 1500);
    r.push(8500, 1500, t);                             // a 0.5 V drop shows
    CHECK_EQ(r.vbus_mv() <= 8575, 1);
    r.set(Preset::High);
    r.push(5000, -300, t + 20);                        // switching preset forgets the history
    CHECK_EQ(r.vbus_mv(), 5000);
    CHECK_EQ(r.current_ma(), -300);
    r.set(Preset::High);                               // same preset again: no reset
    r.push(5100, -300, t + 40);
    CHECK_EQ(r.vbus_mv() < 5100, 1);
}

void test_preset_table() {
    CHECK_STR(name(Preset::Off), "Off");
    CHECK_STR(name(Preset::Low), "Low");
    CHECK_STR(name(Preset::Medium), "Medium");
    CHECK_STR(name(Preset::High), "High");
    CHECK_EQ(static_cast<int>(kDefault), static_cast<int>(Preset::Medium));
    // more smoothing = lower resting cutoff
    CHECK_EQ(params(Preset::Low).min_cutoff_hz > params(Preset::Medium).min_cutoff_hz, 1);
    CHECK_EQ(params(Preset::Medium).min_cutoff_hz > params(Preset::High).min_cutoff_hz, 1);
    for (Preset p : kOn) CHECK_EQ(params(p).beta > 0.0f && params(p).d_cutoff_hz > 0.0f, 1);
    CHECK_EQ(preset_from_index(7) == Preset::Medium, 1);   // out of range falls back to the default
    CHECK_EQ(preset_from_index(0) == Preset::Off, 1);
    CHECK_EQ(preset_from_index(3) == Preset::High, 1);
}

}  // namespace

int main() {
    test_off_is_identity();
    test_constant_input_is_a_fixed_point();
    test_first_sample_seeds_the_filter();
    test_half_volt_step_passes_quickly();
    test_big_steps_pass_at_once();
    test_current_steps();
    test_noise_is_reduced();
    test_ramp_is_followed();
    test_timestamps();
    test_readout_rounds_and_reseeds();
    test_preset_table();
    return test_report("test_smoothing");
}
