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

// ---- the waiting classifier ------------------------------------------------------------------------
// One classifier for "a source waits on the line for a sink", from the Rp presence over time (the ADC's CC
// levels, sampled every UI tick; a line at/above kRpMinMv carries an Rp). A charger's Rp is steady; a dual-role
// port (a phone alone on the port) toggles its Rp open about every 50-100 ms while it looks for a partner
// (STATUS0 BC_LVL 3 <-> 0 in the trace, hardware 2026-11-10, connection matrix H13). The Source tab, the bottom
// bar and the remote state field all read this one verdict (fed by pd::Link::wait_kind).
enum class WaitKind : std::uint8_t {
    Unknown = 0,       // no history yet, the K2 not passive, or VBUS already up: no verdict
    NoSource,          // no Rp on either line over the recent window
    ChargerWaiting,    // Rp steady for >= kSteadyMs: a charger waiting for a sink
    DualRolePort,      // Rp present only part of the time, toggling: a dual-role port looking for a partner
};

class RpTrace {
public:
    static constexpr std::size_t kSamples = 48;      // ring of (time, rp) samples; at a 20 ms tick ~1 s of history
    static constexpr std::uint32_t kWindowMs = 800;  // how far back classify() looks
    static constexpr std::uint32_t kSteadyMs = 300;  // Rp steady this long = a charger waiting
    static constexpr std::uint32_t kToggleMs = 400;  // edges inside this span count as toggling
    static constexpr unsigned kToggleEdges = 3;      // this many edges in kToggleMs = a toggling port (an unplug is one edge)

    void clear() { n_ = 0; head_ = 0; }
    void add(std::uint32_t now_ms, bool rp_present) {
        s_[head_] = {now_ms, rp_present};
        head_ = (head_ + 1) % kSamples;
        if (n_ < kSamples) ++n_;
    }
    std::size_t count() const { return n_; }

    // now_ms = the reference time (the newest sample on the device); vbus_up = VBUS at/above kVbusAttachedMv
    // (a source already feeds something: the lines are no longer an idle source's advertisement).
    WaitKind classify(std::uint32_t now_ms, bool vbus_up) const {
        if (vbus_up || n_ == 0) return WaitKind::Unknown;
        // The window, oldest first (the ring is in time order: head_ is the oldest slot once full).
        std::array<S, kSamples> w{};
        std::size_t m = 0;
        const std::size_t oldest = (head_ + kSamples - n_) % kSamples;
        for (std::size_t i = 0; i < n_; ++i) {
            const S& s = s_[(oldest + i) % kSamples];
            if (s.t <= now_ms && s.t + kWindowMs > now_ms) w[m++] = s;
        }
        if (m == 0 || w[m - 1].t - w[0].t < kSteadyMs) return WaitKind::Unknown;   // not enough history yet
        // Steady suffix: the oldest sample from which every sample still carries Rp.
        std::size_t i0 = 0;
        for (std::size_t i = m; i-- > 0;) if (!w[i].rp) { i0 = i + 1; break; }
        if (i0 < m && w[m - 1].t - w[i0].t >= kSteadyMs) return WaitKind::ChargerWaiting;
        // Toggling: part of the window carries Rp and the line flips often enough (an unplug is a single edge).
        bool any_rp = false, all_rp = true;
        unsigned edges = 0;
        for (std::size_t i = 0; i < m; ++i) {
            any_rp = any_rp || w[i].rp;
            all_rp = all_rp && w[i].rp;
            if (i > 0 && w[i].t + kToggleMs > now_ms && w[i].rp != w[i - 1].rp) ++edges;
        }
        if (any_rp && !all_rp && edges >= kToggleEdges) return WaitKind::DualRolePort;
        // No Rp in the recent steady span: nothing waits.
        bool rp_recent = false;
        for (std::size_t i = 0; i < m; ++i) if (w[i].t + kSteadyMs > now_ms && w[i].rp) rp_recent = true;
        if (!rp_recent) return WaitKind::NoSource;
        return WaitKind::Unknown;   // Rp seen recently but neither steady nor toggling: no verdict yet
    }

private:
    struct S { std::uint32_t t; bool rp; };
    std::array<S, kSamples> s_{};
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
