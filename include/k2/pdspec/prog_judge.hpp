// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check, the per-move programmable judge: one measured PPS / AVS move (analyze.hpp StepOut) turned into the measurements its
// checks are judged on, each with its spec limit (pd_spec_timing.hpp) and its place. Pure and stateless: the monotonic and track
// comparisons need the previous level and stay with the caller. One judge, three callers: the engine's own passes, the charger
// test's moves the engine takes in a Deep run (Engine::judge_test_move) and the range-row caution flags (chargertest/prog_check.hpp).
// Host-tested in tests/test_pdspec_judge.cpp.
#pragma once

#include <cstdint>
#include <cstdlib>

#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pdspec/analyze.hpp"
#include "k2/pdspec/plan.hpp"
#include "k2/pdspec/result.hpp"

namespace k2::pdspec {

// One programmable move: the APDO position, its kind, the step class it is judged in (plan.hpp: 0..2 a step size, kSlam a jump) and
// the set points it went between.
struct ProgMove {
    std::uint8_t pos = 0;
    bool avs = false;
    std::uint8_t cls = kSlam;
    std::int32_t from_mv = 0, to_mv = 0;
};

// Over vPpsSmallStep / vAvsSmallStep: the large-step limits apply.
constexpr bool large_step(bool avs, std::int32_t step_mv) {
    return step_mv > std::int32_t(avs ? pd::spec::kAvsSmallStepMv : pd::spec::kPpsSmallStepMv);
}
// tPpsSrcTransSmall / Large, tAvsSrcTransSmall / Large (SPR), ms.
constexpr std::int32_t step_limit_ms(bool avs, bool large) {
    return std::int32_t(avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                            : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs));
}

struct ProgMeas {
    bool up = false, large = false;
    std::uint8_t a = 0, b = 0;    // the place: position | class << 4 | 0x80 down; the target in 100 mV
    Check band_c{};               // PpsSettle on the PPS 20 mV class (the meter's floor), else PpsBand / AvsBand
    Meas band{};                  // kNever when the move never entered the band (or never settled, PpsSettle)
    Check over_c{};
    Meas over{};                  // the peak (up) / dip (down) against vPpsNew + vPpsValid (vAvsNew + vAvsValid)
    Check trans_c{};
    Meas trans{};
    bool trans_known = false;     // VBUS entered the band: the longest stretch outside it after that
    Check acc_c{};
    Meas acc{};                   // the final level against vPpsNew / vAvsNew
    bool level_known = false;     // in the band or settled: the level is the source's, not a ramp still moving
};

inline ProgMeas measure_prog(const ProgMove& mv, const StepOut& o) {
    ProgMeas m;
    const std::int32_t target = mv.to_mv;
    m.up = target > mv.from_mv;
    m.large = large_step(mv.avs, std::abs(target - mv.from_mv));
    m.a = static_cast<std::uint8_t>(mv.pos | mv.cls << 4 | (m.up ? 0 : 0x80));
    m.b = static_cast<std::uint8_t>(target / 100);
    const std::int32_t lim_us = step_limit_ms(mv.avs, m.large) * 1000;
    if (!mv.avs && mv.cls == 0) {
        m.band_c = Check::PpsSettle;
        m.band = {o.settled ? std::int32_t(o.settle.us) : kNever, std::int32_t(o.settle.gap_us), 0, lim_us, m.a, m.b};
    } else {
        m.band_c = mv.avs ? Check::AvsBand : Check::PpsBand;
        m.band = {o.band.seen ? std::int32_t(o.band.us) : kNever, std::int32_t(o.band.gap_us), 0, lim_us, m.a, m.b};
    }
    const std::int32_t extra = mv.avs ? pd::spec::kAvsValidExtraMv : pd::spec::kPpsValidExtraMv;
    m.over_c = mv.avs ? Check::AvsOvershoot : Check::PpsOvershoot;
    m.over = m.up ? Meas{o.peak_mv, 0, -kOpenLimit, band_hi(target) + extra, m.a, m.b}
                  : Meas{o.peak_mv, 0, band_lo(target) - extra, kOpenLimit, m.a, m.b};
    m.trans_c = mv.avs ? Check::AvsTransient : Check::PpsTransient;
    m.trans_known = o.band.seen;
    const std::int32_t tr_us = std::int32_t(mv.avs ? pd::spec::kAvsTransientMs : pd::spec::kPpsTransientMs) * 1000;
    m.trans = {std::int32_t(o.out_us), o.out_us ? std::int32_t(o.band.gap_us) : 0, 0, tr_us, m.a, m.b};
    m.acc_c = mv.avs ? Check::AvsAccuracy : Check::PpsAccuracy;
    m.level_known = o.band.seen || o.settled;
    const std::int32_t tol = target * std::int32_t(mv.avs ? pd::spec::kAvsNewPermille : pd::spec::kPpsNewPermille) / 1000;
    m.acc = {o.final_mv, 0, target - tol, target + tol, m.a, m.b};
    return m;
}

}  // namespace k2::pdspec
