// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure conversions for the on-chip ADC front end: DMA sweep averaging, ratiometric millivolts
// against VREFINT, the NTC temperature and the ripple-capture helpers. No register access, so
// host tests cover it (tests/test_adc_calc.cpp). Channel roles: the slot table below.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::adc {

// Position of each signal in one scan sweep (seven board signals, plus the internal temperature
// sensor appended as slot 7). Which physical ADC
// channel feeds a slot is the driver's table (adc.cpp), not this file's business.
enum Slot : std::size_t { Aux, Cc1, Cc2, Dp, Dm, Ntc, Vref, Mcu, kSlots };

using Raw = std::array<std::uint16_t, kSlots>;

// Slot -> ADC1 channel. PA2/PA3 are the USB D-/D+ sense pins (they also carry USART2 TX/RX for UFCS/VIVO),
// PA4/PA5 are CC1/CC2 (hardware-verified 2026-11-02: driving the D+/D- switch network moves ch3/ch2 only).
inline constexpr std::array<std::uint8_t, kSlots> kScanChannel = {0, 4, 5, 3, 2, 6, 17, 16};

// DMA buffer of back-to-back sweeps (kSlots halfwords each) -> per-slot mean, rounded to nearest.
// A trailing partial sweep is ignored; an empty buffer gives zeros.
void average_sweeps(std::span<const std::uint16_t> dma, Raw& out);

inline constexpr std::uint32_t kVrefMv = 1200;        // VREFINT nominal
inline constexpr std::uint32_t kFullScale = 4095;     // 12-bit
inline constexpr std::uint32_t kAuxDivider = 11;      // PA0 sits behind an 11:1 divider

// raw * 1.2 V / VREFINT counts, in mV (rounded). 0 without a reference reading.
std::int32_t to_mv(std::uint16_t raw, std::uint16_t vref_raw);
// The supply the ADC runs from, 4095 * 1.2 V / VREFINT counts, in mV.
std::int32_t vdd_mv(std::uint16_t vref_raw);

// Board temperature from the PA6 NTC. The sensor curve is not documented, so this assumes a
// 10k / B3950 NTC to ground with a 10k pull-up to VDD (the ratio raw/4095 needs no reference)
// and says so: `calibrated` is always false until a measured curve replaces the table.
struct Temp {
    std::int16_t deci_c = 0;   // tenths of a degree C
    bool valid = false;        // false for a shorted / open sensor
    bool calibrated = false;
};
Temp ntc_temp(std::uint16_t raw);

// MCU die temperature from the internal sensor (ADC channel 16), AT32F403A form
// T = (V25 - Vsense) / Avg_Slope + 25 with Avg_Slope negative: Vsense RISES with temperature.
// Vsense is converted against VREFINT, so supply drift cancels. Constants: V25 = 1.28 V and
// Avg_Slope = -4.13 mV/C, the AT32F403A datasheet typicals as given by the project owner (not
// re-read from the PDF here), kept as the slope magnitude. Never measured on this board, so
// `calibrated` stays false. Invalid without a reference reading or outside -40 .. 150 C.
inline constexpr std::int32_t kMcuV25Mv = 1280;
inline constexpr std::int32_t kMcuSlopeUvPerC = 4130;   // magnitude of Avg_Slope
Temp mcu_temp(std::uint16_t raw, std::uint16_t vref_raw);

// Everything the pages and the remote show, from one averaged sweep.
struct Reading {
    std::int32_t dp_mv = 0, dm_mv = 0, cc1_mv = 0, cc2_mv = 0, aux_mv = 0;
    Temp temp{};   // board NTC candidate (PA6): live loads showed ch1/ch6/ch7 do not track board temperature, so the UI shows n/a
    Temp mcu{};    // internal die sensor
    std::uint16_t vref_raw = 0;
    std::int32_t vdd_mv = 0;
    Raw raw{};
};
Reading make_reading(const Raw& avg);

// Time per conversion for sample-time code 0..7 (1.5 ... 239.5 cycles, plus 12.5 conversion
// cycles) at the given ADC clock; codes above 7 count as 7. Nanoseconds.
std::uint32_t sample_period_ns(std::uint8_t code, std::uint32_t adc_hz);

struct Stats {
    std::uint16_t min = 0, max = 0, mean = 0;
};
Stats summarize(std::span<const std::uint16_t> samples);   // zeros when empty

}  // namespace k2::adc
