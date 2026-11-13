// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Display smoothing for the on-screen readouts and graphs: the One Euro filter (Casiez, Roussel, Vogel,
// "1 EUR Filter: A Simple Speed-based Low-pass Filter for Noisy Input in Interactive Systems", CHI 2012)
// and the four presets of Settings > Smoothing. Pure code over values, no hardware, host-tested
// (tests/test_smoothing.cpp). The driver side is power::read_display() (k2/drivers/power_monitor.hpp).
//
// One Euro filter: an exponential low-pass whose cutoff rises with the speed of the signal.
//     rate   = (x - x_prev_filtered) / dt
//     dx_hat = lowpass(rate, d_cutoff)                      derivative, smoothed to keep noise out of it
//     fc     = min_cutoff + beta * |dx_hat|                 quiet signal: heavy smoothing; fast edge: light
//     x_hat  = lowpass(x, fc)
//     alpha  = r / (1 + r),  r = 2 pi fc dt                 the low-pass weight for a sample dt seconds after the last
// So jitter of a steady rail is calmed by min_cutoff, while a sudden step makes |dx_hat| large, the cutoff
// jumps far above the sample rate and the step passes (almost) unfiltered: a voltage drop is never hidden.
// Signals are fed in volts and amps (the same numbers for both quantities). dt comes from the sample
// timestamps (milliseconds, 32-bit wrap safe), so an irregular loop period needs no retuning.
#pragma once
#include <cstdint>

#include "k2/core/hot.h"

namespace k2::smoothing {

// Settings > Smoothing. The numeric values are the stored index (k2/ui/uiprefs.hpp), do not reorder.
enum class Preset : std::uint8_t { Off, Low, Medium, High };

inline constexpr Preset kDefault = Preset::Medium;
inline constexpr unsigned kPresetCount = 4;

struct Params {
    float min_cutoff_hz;   // resting cutoff, <= 0 means "no filter" (Off)
    float beta;            // cutoff added per V/s (or A/s) of smoothed rate: responsiveness
    float d_cutoff_hz;     // cutoff of the derivative's own low-pass
};

// Preset table. Measured with 50 Hz samples, +-20 mV uniform jitter on a 9 V rail (docs/ARCHITECTURE.md,
// "Display smoothing"). noise = output rms / input rms of the jitter, step = fraction of a 0.5 V step shown
// after the 1st / 2nd sample (20 / 40 ms):
//     Off     identity
//     Low     min 2.0 Hz, beta 100, d 1.0 Hz   noise 65 %   step 97 / 100 %
//     Medium  min 0.5 Hz, beta  20, d 1.0 Hz   noise 43 %   step 88 /  99 %
//     High    min 0.3 Hz, beta  10, d 1.0 Hz   noise 35 %   step 78 /  96 %
K2_HOT constexpr Params params(Preset p) {
    switch (p) {
    case Preset::Low:    return {2.0f, 100.0f, 1.0f};
    case Preset::Medium: return {0.5f, 20.0f, 1.0f};
    case Preset::High:   return {0.3f, 10.0f, 1.0f};
    default:             return {0.0f, 0.0f, 1.0f};
    }
}

constexpr const char* name(Preset p) {
    switch (p) {
    case Preset::Off:    return "Off";
    case Preset::Low:    return "Low";
    case Preset::Medium: return "Medium";
    default:             return "High";
    }
}

// Stored index -> preset; anything out of range is the default.
constexpr Preset preset_from_index(unsigned i) { return i < kPresetCount ? static_cast<Preset>(i) : kDefault; }

class OneEuro {
public:
    void configure(const Params& p) { p_ = p; }   // keeps the state: the next update continues from it
    void reset() { primed_ = false; }

    // Filter one sample taken at now_ms. The first sample after reset() seeds the filter and passes through.
    K2_HOT float update(float x, std::uint32_t now_ms) {
        if (p_.min_cutoff_hz <= 0.0f) { primed_ = false; return x; }   // Off: identity, no state
        if (!primed_) {
            primed_ = true;
            x_ = x;
            dx_ = 0.0f;
            t_ = now_ms;
            return x;
        }
        const std::uint32_t dt_ms = now_ms - t_;
        const float dt = (dt_ms == 0 ? 1u : dt_ms) * 0.001f;   // two samples in the same millisecond: treat as 1 ms
        t_ = now_ms;
        dx_ += alpha(p_.d_cutoff_hz, dt) * ((x - x_) / dt - dx_);
        x_ += alpha(p_.min_cutoff_hz + p_.beta * abs_f(dx_), dt) * (x - x_);
        return x_;
    }

private:
    static constexpr float kTwoPi = 6.2831853f;
    static constexpr float abs_f(float v) { return v < 0.0f ? -v : v; }
    static constexpr float alpha(float cutoff_hz, float dt) {
        const float r = kTwoPi * cutoff_hz * dt;
        return r / (1.0f + r);
    }

    Params p_{params(kDefault)};
    float x_ = 0.0f, dx_ = 0.0f;
    std::uint32_t t_ = 0;
    bool primed_ = false;
};

// VBUS and current as the display wants them: whole mV / mA in, whole mV / mA out, one filter each.
// Changing the preset forgets the history, so the next sample is shown as read.
class Readout {
public:
    Preset preset() const { return preset_; }

    K2_HOT void set(Preset p) {
        if (p == preset_) return;
        preset_ = p;
        v_.configure(params(p));
        i_.configure(params(p));
        reset();
    }

    void reset() { v_.reset(); i_.reset(); }

    K2_HOT void push(std::int32_t vbus_mv, std::int32_t current_ma, std::uint32_t now_ms) {
        vbus_mv_ = round_to_int(v_.update(static_cast<float>(vbus_mv) * 0.001f, now_ms) * 1000.0f);
        current_ma_ = round_to_int(i_.update(static_cast<float>(current_ma) * 0.001f, now_ms) * 1000.0f);
    }

    std::int32_t vbus_mv() const { return vbus_mv_; }
    std::int32_t current_ma() const { return current_ma_; }

private:
    static std::int32_t round_to_int(float v) { return static_cast<std::int32_t>(v >= 0.0f ? v + 0.5f : v - 0.5f); }

    Preset preset_ = kDefault;
    OneEuro v_, i_;
    std::int32_t vbus_mv_ = 0, current_ma_ = 0;
};

}  // namespace k2::smoothing
