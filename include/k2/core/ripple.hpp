// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure VBUS ripple analysis and oscilloscope projection over PA1 DMA captures (512 samples).
// Uses VREFINT-referenced PA1 pin millivolts with a single named front-end gain ratio
// (kGainNum / kGainDen) so hardware calibration is a one-line update. 32-bit integer math only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/ui/ui_calc.hpp"

namespace k2::ripple {

// Front-end analog gain between VBUS AC ripple and PA1 pin voltage (VBUS_ac = PA1_ac * kGainNum / kGainDen).
// Default 1:1 (direct VREFINT-calibrated PA1 pin mV) until verified against a known reference.
inline constexpr std::int32_t kGainNum = 1;
inline constexpr std::int32_t kGainDen = 1;

// First samples after switching ADC1 from the scan sequence to PA1 carry mux-settling transients.
inline constexpr std::size_t kSkipSamples = 4;

// Noise floor in raw 12-bit ADC counts peak-to-peak (after skipping mux settling; ~8 mVpp at 3.3 V).
inline constexpr std::uint16_t kNoiseFloorCounts = 10;

// Nominal VDD when VREFINT is unavailable (0).
inline constexpr std::int32_t kNominalVddMv = 3300;

// Oscilloscope grid geometry: 4 horizontal divisions, 4 vertical divisions (+-2 divs around 0 mV).
inline constexpr std::uint8_t kHorizDivs = 4;
inline constexpr std::uint8_t kVertDivs = 4;

// Fast PA1 timebases. Each maps to an ADC1 sample-time code (0..7 at 15 MHz)
// whose 508 usable samples are longer than 4 * us_per_div, leaving headroom for edge triggering.
struct Timebase {
    std::uint16_t us_per_div;   // microseconds per horizontal division
    std::uint8_t sample_code;   // ADC1 SPT code (0..7)
    const char *label;          // compact chip text ("50us", "100us", ...)
    const char *menu_label;     // settings row text ("50 us/div", ...)
};

inline constexpr std::uint8_t kTimebaseCount = 7;
inline constexpr Timebase kTimebases[kTimebaseCount] = {
    {25,   0, "25us",  "25 us/div"},
    {50,   0, "50us",  "50 us/div"},
    {100,  0, "100us", "100 us/div"},
    {200,  2, "200us", "200 us/div"},
    {500,  5, "500us", "500 us/div"},
    {1000, 7, "1ms",   "1 ms/div"},
    {2000, 7, "2ms",   "2 ms/div"},
};
inline constexpr std::uint8_t kDefaultTimebase = 1;   // 50 us/div (200 us span, 1.07 MS/s)
inline constexpr std::uint8_t kTimebaseSettingCount = kTimebaseCount + 1;   // 0 = Auto, 1..7 = fixed kTimebases[setting - 1]

// Vertical scales: 0 = Auto, 1..6 = fixed mV/div (+-2 divs = +-2*mv_per_div full scale).
inline constexpr std::uint8_t kScaleCount = 7;
inline constexpr std::uint16_t kScaleMv[kScaleCount] = {0, 10, 20, 50, 100, 200, 500};
inline constexpr const char *kScaleLabel[kScaleCount] = {"AUTO", "10mV", "20mV", "50mV", "100mV", "200mV", "500mV"};
inline constexpr const char *kScaleMenu[kScaleCount] = {"Auto", "10 mV/div", "20 mV/div", "50 mV/div", "100 mV/div", "200 mV/div", "500 mV/div"};

enum class Trigger : std::uint8_t { Auto = 0, Free = 1 };

struct Stats {
    std::uint16_t mean_raw = 0;
    std::uint16_t pp_raw = 0;
    std::int32_t vpp_mv = 0;
    std::int32_t vrms_mv = 0;
    std::int32_t min_ac_mv = 0;
    std::int32_t max_ac_mv = 0;
    std::uint32_t freq_hz = 0;     // 0 when below floor or fewer than 2 rising crossings
    std::uint16_t trigger_idx = 0; // start sample for display alignment
    bool below_floor = true;
    bool valid = false;
};

// Per-column vertical pixel span [lo, hi] (inclusive, 0 = top of plot, h-1 = bottom).
struct ColSpan {
    std::uint8_t lo = 0;
    std::uint8_t hi = 0;
};

// Convert a signed count delta (sample - mean) to millivolts using VREFINT (or nominal 3.3 V if vref_raw == 0).
std::int32_t delta_to_mv(std::int32_t delta_counts, std::uint16_t vref_raw);

// Choose mV/div for a given peak AC excursion: returns the fixed scale when scale_idx > 0,
// or the smallest nice step (20, 50, 100, 200, 500, 1000 mV/div) that holds peak_mv inside +-2 divs.
std::uint16_t resolve_mv_per_div(std::uint8_t scale_idx, std::int32_t peak_mv);

// Settings menu label for a timebase setting (0 = "Auto", 1..7 = kTimebases[setting - 1].menu_label).
const char *timebase_menu_label(std::uint8_t setting);

// Choose physical timebase index (0..kTimebaseCount-1) for Auto timescale from the current stats.
// Pure 32-bit integer math with hysteresis (1.5..8.0 cycles across 4 divs).
std::uint8_t resolve_auto_timebase(std::uint8_t cur_tb, const Stats &st);

// Analyze a PA1 capture buffer for Vpp, Vrms, dominant frequency, and rising-edge trigger index.
// `timebase_idx` selects the displayed window length (for clamping trigger_idx so the window fits).
Stats analyze(std::span<const std::uint16_t> samples, std::uint32_t period_ns, std::uint16_t vref_raw,
              std::uint8_t timebase_idx, Trigger trig);

// Project the triggered window of `samples` into `cols` vertical spans over [0 .. plot_h - 1],
// centered vertically on `stats.mean_raw` with full scale +-2 * mv_per_div.
void project(std::span<const std::uint16_t> samples, std::uint32_t period_ns, std::uint16_t vref_raw,
             const Stats &stats, std::uint8_t timebase_idx, std::uint16_t mv_per_div,
             std::uint8_t plot_h, std::span<ColSpan> cols);

// Formatters for the Ripple page readouts.
void fmt_vpp(const Stats &s, StrBuf<16> &out);    // "< floor", "42 mVpp", "1.24 Vpp", "--"
void fmt_vrms(const Stats &s, StrBuf<16> &out);   // "14 mVrms", "--"
void fmt_freq(const Stats &s, StrBuf<16> &out);   // "62.5 kHz", "420 Hz", "--"

}  // namespace k2::ripple
