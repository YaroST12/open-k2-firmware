// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Pure conversions for the TI power monitor (INA228 / INA226 / INA237): register contents
// to common raw counts to millivolts / milliamps. No bus access, so host tests cover it
// (tests/test_power_calc.cpp).
#pragma once
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::power {

enum class Chip : std::uint8_t { None, Ina228, Ina226, Ina237 };

const char* name(Chip chip);  // "INA228" ... "NONE"

// Highest VBUS the chip survives (datasheet absolute maximum): INA226 36 V, INA228/INA237 85 V.
// 0 for Chip::None. Millivolts.
std::int32_t max_vbus_mv(Chip chip);

// Highest voltage a PD request may reach: 36 V with the INA226, 48 V (EPR) with the
// INA228 / INA237. 0 for Chip::None.
std::int32_t max_request_mv(Chip chip);

// Current-sense shunt on VBUS: 3 mOhm (R003) on every K2 batch.
inline constexpr std::int32_t kShuntUohm = 3000;

// The shunt implied by the chip's 2.0 A reference counts and the datasheet shunt-voltage LSB,
// in micro-ohms. Equals kShuntUohm for every chip (checked in the host tests).
std::int32_t implied_shunt_uohm(Chip chip);

// Model badge: second character of the "N6.55" version string, '8' for INA228,
// '7' for INA237, '6' for INA226, '?' without a chip.
char model_badge(Chip chip);

// Writes the model string "N<badge>.<vv>" ("N6.55"); vv is the version number,
// two digits (values above 99 print 99). Cut to fit and NUL-terminated; returns out.data().
const char* format_model(std::span<char> out, Chip chip, unsigned version);

// Register width on the wire: 3 bytes for INA228 VSHUNT/VBUS, 2 for the others.
int reg_bytes(Chip chip);

// Register contents (big-endian value of the 2 or 3 bytes read) -> common raw counts.
std::int32_t align_shunt(Chip chip, std::uint32_t reg);
std::int32_t align_vbus(Chip chip, std::uint32_t reg);

// Raw counts (already averaged) -> physical units.
std::int32_t raw_to_mv(Chip chip, std::int32_t raw_v);
std::int32_t raw_to_ma(Chip chip, std::int32_t raw_i);  // clamped to the chip's current limit
std::int32_t mw(std::int32_t vbus_mv, std::int32_t current_ma);

// Idle band: an INA226 with nothing attached reads a couple of mA of offset. Currents with
// |I| < kDeadbandMa are treated as exactly 0 (and so 0 W, no direction arrow).
inline constexpr std::int32_t kDeadbandMa = 10;

// Power-flow direction. "Input" is the side the charger plugs into, "output" the side the load
// (laptop) plugs into. Forward = power flows input -> output = positive current everywhere above the
// driver (Metrics, GET_STATE, JSON, the Home arrow, the energy meter). The INA shunt is wired so that
// this normal case reads NEGATIVE (measured: 100 W PD charger, 19.7 V, INA = -1892 mA), so the one
// place that knows is kInaForwardSign; forward_ma() applies it.
enum class Flow : std::uint8_t { Idle, Forward, Reverse };

inline constexpr std::int32_t kInaForwardSign = -1;

// INA shunt current (mA, as the chip reports it) -> current with positive = forward.
constexpr std::int32_t forward_ma(std::int32_t ina_ma) { return ina_ma * kInaForwardSign; }

std::int32_t apply_deadband(std::int32_t current_ma);
Flow flow_of(std::int32_t current_ma);
std::uint32_t magnitude(std::int32_t v);  // |v|, safe for INT32_MIN
// |v| as int32, INT32_MIN saturated to INT32_MAX (no UB on a garbage reading).
constexpr std::int32_t abs_sat(std::int32_t v) { return v >= 0 ? v : v == INT32_MIN ? INT32_MAX : -v; }

// Sliding average of raw counts over the last kAvgLen samples (one ring per quantity).
inline constexpr std::size_t kAvgLen = 128;

class Average {
public:
    void reset() { *this = Average{}; }
    void restart(std::int32_t v) { reset(); push(v); }   // forget the window, start from one fresh sample
    void push(std::int32_t v);
    std::int32_t mean() const;  // 0 when empty
    std::size_t count() const { return count_; }

private:
    std::array<std::int32_t, kAvgLen> buf_{};
    std::size_t head_ = 0, count_ = 0;
};

}  // namespace k2::power
