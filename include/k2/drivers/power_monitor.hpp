// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// TI power monitor on the PB8/PB9 bus at 0x40: INA228, INA226 or INA237, auto-detected.
// Conversion maths lives in power_calc.hpp; this is the bus-facing driver.
#pragma once
#include <cstdint>
#include <optional>

#include "k2/drivers/power_calc.hpp"
#include "k2/ui/smoothing.hpp"

namespace k2::power {

inline constexpr std::uint8_t kI2cAddr = 0x40;

struct Metrics {
    std::uint16_t die_id = 0;  // DIE_ID / MFG_ID register read during detection
    std::int32_t vbus_mv = 0;
    std::int32_t current_ma = 0;  // signed; 0 inside the +-kDeadbandMa idle band
    std::int32_t power_mw = 0;    // signed; 0 when idle
    Flow flow = Flow::Idle;       // Forward = positive current (see power_calc.hpp)
};

// Probe for INA228, INA226, INA237 (in that order) and set up the one found.
// Logs "[PWR] detected ..." or "[PWR] no INA228/INA226/INA237 at 0x40". True if one answered.
bool init();

Chip chip();  // Chip::None until detection succeeds

// Highest VBUS the detected chip can measure, in mV; 0 while no chip is present. The fast-charge
// triggers never request or tolerate more than this.
std::int32_t max_vbus_mv();

// Take one sample into the averages; call every ~20 ms. With no chip present, retries
// detection at most once per second.
void poll();

// Averaged readings (128-sample window, ~2.5 s), or nullopt while offline or before the first sample. For
// everything that is not a screen: the remote and web state, logs, link detection, the charger test's current.
std::optional<Metrics> read();

// What the screen shows: VBUS and current smoothed by the One Euro filter of the chosen preset
// (k2/ui/smoothing.hpp; Settings > Smoothing), power = smoothed VBUS x smoothed current. Quiet jitter is calmed
// while a step or drop passes within a sample or two, so it never hides a transient the way read()'s window
// does. Off = the latest sample as read. Used by the readouts, the graphs and the trends. Nullopt while
// offline or before the first sample.
std::optional<Metrics> read_display();

// The latest sample with no filtering at all (current dead band applied): the energy meter and anything
// that must see every dip. Nullopt while offline or before the first sample.
std::optional<Metrics> read_raw();

// Choose the preset read_display() uses. Changing it re-seeds the filter from the next sample.
void set_smoothing(smoothing::Preset preset);
smoothing::Preset smoothing_preset();

// VBUS of the latest poll() sample (at most one main-loop pass old): no software average, so no lag. Every decision
// about a VBUS transition (landed, band, dark, source lost, timeout) reads this; read() is for display. Nullopt while
// offline or before the first sample.
std::optional<std::int32_t> vbus_fast_mv();

// Forget the averaging window and start it from one direct reading. The window is 128 samples (~2.5 s):
// after a step on VBUS (a source switching on) the average ramps for that long. No-op while offline.
void restart_average();

// One direct register read: no software average, no deadband (the INA226's own 16-sample
// average still applies, so a fresh value arrives about every 35 ms). For short-pulse probes.
std::optional<Metrics> sample_now();

// Fast VBUS sampling for the transition capture (k2/pd/transcap.hpp).
// fast_begin() switches an INA226 from 16 averages (a fresh value every ~4.5 ms) to no averaging with 140 us
// conversions (a fresh VBUS value every ~280 us); fast_end() restores the normal setup. The INA228 / INA237 are
// left alone, their normal conversion (~1 ms) is already fast enough. While fast the current readings are
// noisier (the shunt average has 16 times fewer samples per value). Returns false without a chip.
bool fast_begin();
void fast_end();
bool fast_active();

// One direct VBUS register read (one bus transaction, ~215 us), in mV. False without a chip or on a bus error.
bool fast_vbus_mv(std::int32_t& mv);

}  // namespace k2::power
