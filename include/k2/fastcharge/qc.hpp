// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Qualcomm Quick Charge 2.0 / 3.0 / 3+ (docs/FASTCHARGE_PROTOCOLS.md section 1), pure logic:
// the QC2.0 level table, the HVDCP handshake window and the QC3 pulse trains as step lists.
#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>

#include "k2/fastcharge/fc_safety.hpp"
#include "k2/fastcharge/fc_seq.hpp"

namespace k2::qc {

using fc::Drive;

struct Levels {
    Drive dp, dm;
};

// QC2.0 table: 5 V, 9 V, 12 V, 20 V; anything else has no entry.
constexpr std::optional<Levels> qc2_levels(int mv) {
    switch (mv) {
    case 5000:  return Levels{Drive::V0p6, Drive::V0};
    case 9000:  return Levels{Drive::V3p3, Drive::V0p6};
    case 12000: return Levels{Drive::V0p6, Drive::V0p6};
    case 20000: return Levels{Drive::V3p3, Drive::V3p3};
    default:    return std::nullopt;
    }
}

// HVDCP entry (method A). With D+ at 0.6 V the not-yet-switched charger shorts D- to D+, so
// D- reads 0.35 .. 1.15 V; when the charger opens the short its pull-down takes D- down by 0.35 V+.
constexpr bool handshake_initial_ok(int dm0_mv) { return dm0_mv >= 350 && dm0_mv <= 1150; }
constexpr bool handshake_dropped(int dm0_mv, int dm_mv) { return dm0_mv - dm_mv >= 350; }

// Strict handshake (method B), used before SCP: 1000 ms at 0 V, D- floating with D+ 0.6 V
// for 190 ms (D- must read 0.20 .. 1.00 V), then D+ 0 V 322 ms / 0.6 V 190 ms / 0 V 87 ms / 0.6 V
// 50 ms, then wait for D- < 0.50 V (up to 2000 ms) and 38 ms.
constexpr bool handshake_b_bias_ok(int dm_mv) { return dm_mv > 200 && dm_mv < 1000; }
constexpr bool handshake_b_low(int dm_mv) { return dm_mv < 500; }
struct WakeStep {
    Drive dp;
    std::uint32_t ms;
};
inline constexpr WakeStep kWake[4] = {{Drive::V0, 322}, {Drive::V0p6, 190}, {Drive::V0, 87}, {Drive::V0p6, 50}};
inline constexpr std::uint32_t kHsbResetMs = 1000, kHsbBiasMs = 190, kHsbPollTimeoutMs = 2000, kHsbFinishMs = 38;

enum class Mode : std::uint8_t { Qc3, Qc3Plus };
enum class Dir : std::uint8_t { Up, Down };

// Pulse timing: QC3.0 holds the pulse 2250 us; QC3+ 4 units (~40 us) then 5 units
// (~50 us) of settling. The QC3.0 gap is not in the spec: it is set equal to the pulse width.
// QC3+ down (measured on hardware, a power bank that follows 40 us up pulses): a down pulse of 40, 100 or 150 us
// leaves VBUS where it is, 175 us and longer steps it down. A 250 us hold keeps a margin over that edge and a
// 50-pulse batch (15 ms) inside one blocking list; the gap stays 50 us (a 500 us gap with a 40 us pulse did not help).
inline constexpr std::uint32_t kQc3PulseUs = 2250, kQc3GapUs = 2250;
inline constexpr std::uint32_t kQc3PlusPulseUs = 40, kQc3PlusGapUs = 50, kQc3PlusDownPulseUs = 250;

// Millivolts one pulse moves the output, and the dead band of the closed loop (just under one pulse
// for QC3.0 so that the residual error after the last pulse is always inside it).
constexpr int step_mv(Mode m) { return m == Mode::Qc3 ? 200 : 20; }
constexpr int deadband_mv(Mode m) { return m == Mode::Qc3 ? 150 : 30; }

// A QC3+ pulse that moves VBUS by more than this is not a QC3+ pulse: the charger reads it as a QC3 step (nominal 20 mV
// vs 200 mV; measured 180 mV on a power bank, 2026-11-08). 100 mV is 5x the nominal step and half the QC3 one, well
// above what pulse-size tolerance (the engine accepts up to 4x = 80 mV) or measurement noise can produce.
inline constexpr int kPlusCoarseStepMv = 100;

// Baseline of the continuous mode: D+ 0.6 V, D- 3.3 V. Up: D+ to 3.3 V; down: D- to 0.6 V.
// Appends up to `count` pulses (fewer when the list is full); returns how many were added.
template <std::size_t N>
int pulse_train(fc::StepList<N>& out, Mode m, Dir d, int count) {
    const std::uint32_t hold = m == Mode::Qc3 ? kQc3PulseUs : d == Dir::Down ? kQc3PlusDownPulseUs : kQc3PlusPulseUs;
    const std::uint32_t gap = m == Mode::Qc3 ? kQc3GapUs : kQc3PlusGapUs;
    int added = 0;
    for (; added < count; added++) {
        if (out.n + 2 > N) break;
        if (d == Dir::Up) out.push(Drive::V3p3, Drive::V3p3, hold);
        else out.push(Drive::V0p6, Drive::V0p6, hold);
        out.push(Drive::V0p6, Drive::V3p3, gap);
    }
    return added;
}

// Signed pulse count that moves VBUS by `error_mv` (target - measured); 0 inside the dead band.
// `step` is the step measured on this charger (default: the nominal one); the dead band scales with it.
inline constexpr int kMaxDeadbandMv = 250;   // below the suite's +-300 mV pass band, however coarse the step
constexpr int pulses_for(Mode m, int error_mv, int step = 0) {
    const int mag = error_mv < 0 ? -error_mv : error_mv;
    const int st = step > 0 ? step : step_mv(m);
    if (mag < std::min(deadband_mv(m) * st / step_mv(m), kMaxDeadbandMv)) return 0;
    const int n = std::max(1, mag / st);
    return error_mv < 0 ? -n : n;
}

// Pass band of the continuous rows (inferred): a settled level may sit up to 300 mV
// under the target but only 150 mV over it, so the loop approaches from below.
inline constexpr int kBandBelowMv = 300, kBandAboveMv = 150, kAimBelowMv = 100;
constexpr bool in_pass_band(int mv, int target_mv) { return mv >= target_mv - kBandBelowMv && mv <= target_mv + kBandAboveMv; }

// Is VBUS stable? Three samples at least kSampleMs apart (the power monitor refreshes every ~20 ms, so a faster
// feed would count the same reading twice) that agree within kSpanMv. A slew faster than ~0.8 V/s keeps it false.
class Settle {
public:
    static constexpr std::uint32_t kSampleMs = 30;
    static constexpr int kSpanMv = 50;
    constexpr void reset() { n_ = 0; }
    constexpr bool feed(std::uint32_t now_ms, int mv) {
        if (n_ == 0 || now_ms - last_ms_ >= kSampleMs) {
            s_[head_] = mv;
            head_ = (head_ + 1) % 3;
            if (n_ < 3) ++n_;
            last_ms_ = now_ms;
        }
        return stable();
    }
    constexpr bool stable() const {
        if (n_ < 3) return false;
        return std::max({s_[0], s_[1], s_[2]}) - std::min({s_[0], s_[1], s_[2]}) <= kSpanMv;
    }

private:
    int s_[3] = {};
    int head_ = 0, n_ = 0;
    std::uint32_t last_ms_ = 0;
};

// Total signed pulses that bring `vbus` into the pass band. QC3.0 steps are coarse (200 mV or more): hold inside the
// band, otherwise pick the level (pulse count, or one more) closest to target - kAimBelowMv that lands in the band,
// the lower count on a tie; when no level fits take the closer one, or hold when no pulse gets closer. QC3+ steps
// are fine (20 mV): the dead-band loop aims at the same point, target - kAimBelowMv, and holds anywhere above it
// that is still inside the band.
constexpr int plan_pulses(Mode m, int vbus, int target_mv, int step = 0) {
    const int st = step > 0 ? step : step_mv(m);
    if (st < 100) {
        if (vbus > target_mv - kAimBelowMv && in_pass_band(vbus, target_mv)) return 0;
        return pulses_for(m, target_mv - kAimBelowMv - vbus, st);
    }
    if (in_pass_band(vbus, target_mv)) return 0;
    const int aim = target_mv - kAimBelowMv;
    const int e = aim - vbus;
    const int dir = e < 0 ? -1 : 1;
    const int lo = (e < 0 ? -e : e) / st;
    auto score = [&](int v) {
        const int off = v - aim < 0 ? aim - v : v - aim;
        const int miss = v > target_mv ? v - target_mv : target_mv - v;
        return in_pass_band(v, target_mv) ? off : 1000 + miss;
    };
    int best = 0, best_score = score(vbus);
    for (int c : {lo, lo + 1}) {
        const int s = score(vbus + dir * c * st);
        if (s < best_score) { best_score = s; best = c; }
    }
    return dir * best;
}

// Pulses for the next batch out of `total` still to send. QC3.0: a full batch while more than two remain beyond it,
// then single pulses so the last steps are each measured before the next one. QC3+: full batches far away, then
// half of what is left (at least kPlusTail), so the last ~200 mV come in one small batch after the others settled.
inline constexpr int kPlusTail = 10;
constexpr int batch_pulses(Mode m, int total, int cap) {
    const int mag = total < 0 ? -total : total;
    if (m == Mode::Qc3Plus) return mag <= kPlusTail ? mag : std::min(cap, std::max(kPlusTail, mag / 2));
    if (mag <= 2) return mag > 0 ? 1 : 0;
    return std::min(mag - 2, cap);
}

// Open-loop bound (inferred): the highest VBUS the expected pulse count can explain, plus one step (at least 100 mV,
// the measurement noise floor, so a 20 mV QC3+ step does not trip on ripple). A level above it means the charger
// moved more than the pulses ask for.
constexpr int open_loop_limit_mv(int entry_mv, int target_mv, int step_mv) {
    const int st = step_mv > 0 ? step_mv : 1;
    const int up = target_mv > entry_mv ? (target_mv - entry_mv + st - 1) / st : 0;
    return entry_mv + up * st + std::max(st, 100);
}

// QC3 Class B reaches 20 V; Class A callers must limit their chosen targets to 12 V.
// The chip ceiling can only lower this protocol ceiling. 0 without a power monitor.
constexpr int clamp_qc3_mv(int mv, int ceiling_mv) {
    return fc::clamp_to_ceiling(std::clamp(mv, 3600, 20000), ceiling_mv);
}

}  // namespace k2::qc
