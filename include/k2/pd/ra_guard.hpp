// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Ra-mode guard: is the charger plugged into the K2 directly, or through a cable? (pure rules, host-tested in
// tests/test_ra_guard.cpp.)
//
// The spoof (emk_ra.hpp, fusb302::ra_set) holds a fixed Ra on CC1 and an Rd on CC2. It only makes sense when a receptacle
// charger is plugged DIRECTLY into the K2 top plug: the charger then sees our Ra + Rd as "sink plus powered cable" and
// asks the spoofed identity. A cable carries a single CC wire: if that wire lands on CC1 the charger sees only an Ra
// ("cable, no sink") and never turns VBUS on; on CC2 the charger cannot see our Ra through a passive cable at all, and an
// e-marked cable answers SOP' itself (user report 2026-11-06: hub PD output, USB 3.2 Gen2 C-to-C cable on the bottom port,
// saved clone picked: nothing came up and the START dropped back).
//
// The topology is read BEFORE the Ra goes on, with the K2 passive and the source waiting unattached. A receptacle charger
// shows its Rp on BOTH CC lines. A cable shows Rp on ONE line; the other is idle, at an Ra, or at the PLUS-coupled idle.
//
// Levels measured on this K2 (hardware 2026-11-06, `debug cc-probe` sweeps and the [ADC] heartbeat line):
//   - Rp from a waiting receptacle charger (Baseus 140W, CUKTECH 10/120W, OnePlus 120W): 3.2 - 3.3 V on both lines
//     (the ADC saturates at the 3.3 V rail),
//   - an idle CC1 net: ~0 V, an idle CC2 net: ~1.7 V (1.707 V; it sags while PLUS is pressed),
//   - an Ra (a real e-marked cable's, or our PC13 Ra): ~0.32 V.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace k2::raguard {

// A line at or above this level carries an Rp pull-up. Under it is idle (0 V, 1.7 V) or an Ra (0.3 V); the nearest
// measured non-Rp level is the 1.707 V CC2 idle, the lowest Rp 3.2 V, so 2.4 V sits between them with ~0.7 V margin.
inline constexpr std::int32_t kRpMinMv = 2400;
// VBUS at or above this: a source is already attached to something (not "waiting unattached"), so the lines no longer
// show the idle-source signature and the guard cannot judge.
inline constexpr std::int32_t kVbusAttachedMv = 1000;
// The dialog samples the lines this often; with PeakWindow::kSamples that makes the window ~0.8 s (a PLUS press is shorter).
inline constexpr std::uint32_t kSampleMs = 100;

enum class Topology : std::uint8_t {
    Unknown,           // no reading, or the K2 is not passive, or VBUS is already up: never blocks
    NoSource,          // no Rp on either line
    CableSingleCc,     // Rp on exactly one line: a cable (or an adapter) in between
    DirectReceptacle,  // Rp on both lines: a receptacle charger directly on the plug
};

struct Reading {
    std::int32_t cc1_mv = -1, cc2_mv = -1;   // -1 = no reading
    std::int32_t vbus_mv = -1;               // -1 = no power monitor: a missing VBUS is not proof of a source
    bool passive = false;                    // no Rd / Ra of ours on the lines, no sink inline
};

constexpr Topology classify(const Reading& r) {
    if (!r.passive || r.cc1_mv < 0 || r.cc2_mv < 0 || r.vbus_mv >= kVbusAttachedMv) return Topology::Unknown;
    const int rp = int(r.cc1_mv >= kRpMinMv) + int(r.cc2_mv >= kRpMinMv);
    return rp == 2 ? Topology::DirectReceptacle : rp == 1 ? Topology::CableSingleCc : Topology::NoSource;
}

// Only a receptacle charger (or a topology we cannot judge) may run the spoof.
constexpr bool spoof_allowed(Topology t) { return t == Topology::DirectReceptacle || t == Topology::Unknown; }

constexpr const char* name(Topology t) {
    switch (t) {
        case Topology::Unknown: return "unknown";
        case Topology::NoSource: return "none";
        case Topology::CableSingleCc: return "cable";
        case Topology::DirectReceptacle: return "direct";
    }
    return "?";
}

// Peak of the last kSamples readings per line. A PLUS press (or any other coupling) sags a line for a few samples; the
// verdict follows the peak, so a one-off sag never turns a direct charger into a "cable". A line that really went
// idle (the charger was unplugged) leaves the window after kSamples samples, so a stale Direct does not linger.
class PeakWindow {
public:
    static constexpr std::size_t kSamples = 8;
    void clear() { n_ = 0; head_ = 0; }
    void add(std::int32_t cc1_mv, std::int32_t cc2_mv) {
        cc1_[head_] = cc1_mv; cc2_[head_] = cc2_mv;
        head_ = (head_ + 1) % kSamples;
        if (n_ < kSamples) ++n_;
    }
    std::size_t count() const { return n_; }
    std::int32_t cc1() const { return peak(cc1_); }
    std::int32_t cc2() const { return peak(cc2_); }
private:
    std::int32_t peak(const std::array<std::int32_t, kSamples>& a) const {
        std::int32_t best = -1;
        for (std::size_t i = 0; i < n_; ++i) best = std::max(best, a[i]);
        return best;
    }
    std::array<std::int32_t, kSamples> cc1_{}, cc2_{};
    std::size_t n_ = 0, head_ = 0;
};

// The refusal the dialog shows, one row per line (no wrapping inside a row).
inline constexpr std::array<const char*, 4> kRefusalLines = {
    "Spoof needs the charger",
    "plugged in the K2 directly",
    "(top plug). Through a cable",
    "the cable itself answers.",
};
// The same message for the remote START (ct_start.py prints it as an error).
inline constexpr const char* kRefusalText =
    "spoof needs the charger plugged into the K2 directly (top plug); through a cable the cable itself answers";

}  // namespace k2::raguard
