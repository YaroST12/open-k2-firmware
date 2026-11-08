// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Timed D+/D- step lists (pure): builders in qc.hpp / afc.hpp / accp.hpp produce them, dpdm::play()
// runs them on the pins and tests replay them against a fake clock.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/drivers/dpdm.hpp"

namespace k2::fc {

using dpdm::Drive;
using dpdm::Step;

constexpr std::uint32_t total_us(std::span<const Step> steps) {
    std::uint32_t t = 0;
    for (const Step& s : steps) t += s.us;
    return t;
}

// Fixed-capacity step list; push() returns false (and drops the step) when full.
template <std::size_t N>
struct StepList {
    std::array<Step, N> steps{};
    std::size_t n = 0;

    bool push(Drive dp, Drive dm, std::uint32_t us) {
        if (n >= N) return false;
        steps[n++] = {dp, dm, us};
        return true;
    }
    std::span<const Step> span() const { return {steps.data(), n}; }
};

// Replay a list on any sink with set(dp, dm) and wait_us(us). dpdm::play() is the hardware sink;
// host tests use a recording sink with a fake clock.
template <class Io>
void replay(std::span<const Step> steps, Io& io) {
    for (const Step& s : steps) {
        io.set(s.dp, s.dm);
        io.wait_us(s.us);
    }
}

}  // namespace k2::fc
