// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// On-chip ADC1 + DMA1 channel 1: continuous 7-slot scan of the analog front end (D+, D-, CC1, CC2,
// aux rail, NTC, VREFINT) averaged over 100 sweeps, plus two on-demand modes that borrow the same
// hardware: a raw one-shot sweep of every channel and the 512-sample ripple capture.
// The driver owns ADC1, DMA1 channel 1, the PA0..PA6 pin setup and the ADC clock; callers use only
// this API. Conversions are in adc_calc.hpp.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>

#include "k2/drivers/adc_calc.hpp"

namespace k2::adc {

inline constexpr std::uint32_t kAdcClockHz = 15'000'000;   // PCLK2 / 8
inline constexpr std::size_t kScanSweeps = 100;            // sweeps in the DMA ring / the average

// Clock, pins, calibration, DMA ring, start of the scan. Logs "[ADC] init ..." and returns true
// when the ADC calibrated and the scan runs.
bool init();

// Refresh the averaged reading from the DMA ring; call every ~20 ms.
void poll();

// Latest averaged sweep, or nullopt until the first full ring of sweeps (~15 ms after init()).
std::optional<Reading> read();

// The newest complete sweep's CC1 / CC2 raw readings (12 bit), straight from the DMA ring with no averaging and
// no work for the CPU beyond a few loads: the transition capture reads it every millisecond. False while the
// scan is not running (not ready, pins released, ring not yet filled once).
bool latest_cc(std::uint16_t& cc1_raw, std::uint16_t& cc2_raw);

// Hand PA2/PA3 (CC1/CC2) to another driver (k2::uart2): the scan stops, read() returns nullopt, the
// one-shot modes refuse, and both pins are left as floating inputs. reclaim_dpdm_pins() makes them
// analog again and restarts the scan. The ADC never converts them while another driver owns them.
void release_dpdm_pins();
void reclaim_dpdm_pins();

// One-shot raw conversions (239.5-cycle sampling) of ADC channels 0..7, 16 and 17, in that order.
// Interrupts the scan for ~3 ms and restores it. For finding out which pin carries which signal.
inline constexpr std::size_t kRawChannels = 10;
using RawAll = std::array<std::uint16_t, kRawChannels>;
inline constexpr std::array<std::uint8_t, kRawChannels> kRawChannelIds = {0, 1, 2, 3, 4, 5, 6, 7, 16, 17};
bool sweep_all(RawAll& out);

// Log both halves of sweep_all() as "[ADC] raw ..." lines.
void log_raw();

// High-speed capture of PA1 (the AC-coupled VBUS ripple): fills `out` with back-to-back single-channel
// conversions at sample-time code 0..7 (1.5 ... 239.5 cycles). Returns the time between samples in
// nanoseconds, or 0 when the capture timed out. Interrupts the scan for the capture and restores it.
inline constexpr std::size_t kRippleSamples = 512;
std::uint32_t capture_ripple(std::span<std::uint16_t, kRippleSamples> out, std::uint8_t sample_code);

}  // namespace k2::adc
