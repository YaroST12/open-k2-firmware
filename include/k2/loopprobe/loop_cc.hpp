// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The loop probe's CC step as the FUSB302 driver runs it (fusb302::loop_cc_hold): the plan codes and the result. A header of
// its own so the driver does not include the whole probe (k2/loopprobe/loop_probe.hpp).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_phy.hpp"

namespace k2::loopprobe {

// One plan entry = the K2's own Rd while it is held: bit0 Rd on CC1, bit1 Rd on CC2, kCcAsFound = SWITCHES0 as found.
// Rd on both lines is refused unless it was found that way.
inline constexpr std::uint8_t kCcAsFound = 0x80;
inline constexpr std::size_t kCcRows = 5;
inline constexpr std::array<std::uint8_t, kCcRows> kCcPlan = {kCcAsFound, 0x01, 0x02, 0x00, kCcAsFound};

inline constexpr std::int32_t kCcVbusLimitMv = 5500;   // VBUS above this refuses the step or ends it (never above 5 V)
inline constexpr std::uint16_t kCcMeasureUs = 100;     // BC_LVL settle after the measure switch moves to a line

enum class CcRefusal : std::uint8_t { None, Bus, Contract, Engine, Vconn, VbusHigh, BadPlan };

struct CcRow {
    std::uint8_t cfg = 0;                     // the plan entry
    std::uint16_t cc1_raw = 0, cc2_raw = 0;   // 12-bit ADC counts, newest sweep (no divider: 4095 ~ the 3.3 V supply)
    std::uint8_t bc = 0;                      // bits 1:0 BC_LVL measuring CC1, bits 3:2 measuring CC2, bit 4 valid
    std::uint16_t vbus_mv = 0;
};

inline constexpr std::uint8_t kCcRestored = 1 << 0;   // SWITCHES0 read back equal to the value found
inline constexpr std::uint8_t kCcAborted = 1 << 1;    // VBUS rose above the limit: the plan ended early
inline constexpr std::uint8_t kCcDisturbed = 1 << 2;  // SWITCHES0 read back different at a sample (someone else wrote it)

struct CcResult {
    std::uint8_t refusal = 0;                 // CcRefusal
    std::uint8_t sw0 = 0;                     // SWITCHES0 as found
    std::uint8_t pc = 0;                      // CC switch pins as found: bit0 PC13 (VCONN), bit1 PC14 (CC1), bit2 PC15 (CC2)
    std::uint8_t flags = 0;
    std::uint8_t n = 0;
    std::array<CcRow, kCcRows> rows{};
};

// True for a plan entry the driver accepts.
constexpr bool cc_cfg_ok(std::uint8_t c) { return c == kCcAsFound || c <= 0x02; }

// A plan the driver runs: 1..kCcRows entries, each cc_cfg_ok (so never Rd on both lines unless found that way).
constexpr bool cc_plan_ok(std::span<const std::uint8_t> plan) {
    if (plan.empty() || plan.size() > kCcRows) return false;
    for (const std::uint8_t c : plan)
        if (!cc_cfg_ok(c)) return false;
    return true;
}

// SWITCHES0 while a plan entry is held: the value found with only the pull-down bits replaced (the measure switch and
// everything else stay as found). kCcAsFound gives the found value back.
constexpr std::uint8_t cc_sw0(std::uint8_t found, std::uint8_t cfg) {
    if (cfg == kCcAsFound) return found;
    namespace phy = k2::pd::phy;
    return std::uint8_t((found & ~phy::kSw0PullDowns) | ((cfg & 1) ? phy::kPdwn1 : 0) | ((cfg & 2) ? phy::kPdwn2 : 0));
}

}  // namespace k2::loopprobe
