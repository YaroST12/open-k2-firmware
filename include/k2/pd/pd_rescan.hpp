// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Rescan plan: the CC operations a rescan performs, as data. The fusb302 driver executes the
// ops; host tests assert that the inline plan (a real sink is attached and charging) is empty,
// so a rescan can never add or remove the K2's Rd, open the CC switches or transmit there.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::pd {

enum class OpKind : std::uint8_t {
    CcSwitches,   // value: 1 = both CC analog switches closed, 0 = open (PC14/PC15)
    Delay,        // value = milliseconds
};

struct Op {
    OpKind kind;
    std::uint8_t value;
};

// Passive rescan: the CC analog switches open and close, the source sees a
// detach and re-broadcasts Source_Capabilities. Rd, the chip registers and the FIFOs are not
// touched and nothing is transmitted.
inline constexpr std::array<Op, 4> kActiveRescan = {{
    {OpKind::CcSwitches, 0}, {OpKind::Delay, 150},
    {OpKind::CcSwitches, 1}, {OpKind::Delay, 50},
}};

// Inline: no ops at all. The driver only arms "next Source_Capabilities starts a new source".
inline std::span<const Op> rescan_plan(bool inline_mode) {
    return inline_mode ? std::span<const Op>{} : std::span<const Op>{kActiveRescan};
}

// True if any op acts on the CC switches (the only thing a rescan may touch).
inline bool plan_touches_cc(std::span<const Op> plan) {
    for (const Op& op : plan)
        if (op.kind != OpKind::Delay) return true;
    return false;
}

}  // namespace k2::pd
