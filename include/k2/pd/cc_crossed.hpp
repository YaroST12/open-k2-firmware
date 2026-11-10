// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Crossed CC through the K2 (pure rules, host-tested in tests/test_cc_crossed.cpp).
//
// A passive Type-C cable carries ONE CC wire. With a charger on one K2 port and a phone on the other, the two wires land on
// the K2's CC lines, and when they land on different lines (CC1 for one cable, CC2 for the other) neither far end sees a
// partner: the charger's Rp finds no Rd and keeps VBUS dark, the phone's dual-role toggle finds no Rp. The K2 sees it from
// the levels alone, with VBUS dark and nothing of its own on the lines (bench 2026-11-10, Anker IQ4 -> cable -> K2 -> cable ->
// Pixel): the charger's Rp loaded by the phone cable's Ra reads a steady ~0.5 V on one line while the other cycles
// 0 / 1.2 / 1.8 / 3.3 V.
//
//   Crossed = one line steady (Rp loaded by an Ra: 0.2 - 0.8 V, or a plain cable's steady charger Rp: >= kRpMinMv) AND the
//             other line toggling (>= kToggleEdges level-band changes in the window),
//   Hint    = Rp-vs-Ra steady on one line and NOTHING on the other (a charger behind an e-marked cable, nothing at the far
//             end, or the phone's wire on the line we cannot see): "flip a cable?" only,
//   None    = everything else: a charger alone (steady Rp, the other line idle), a dual-role phone alone (one or both lines
//             toggle, no steady partner), nothing plugged in, an unplug (one edge, not a toggle).
// The owner feeds it only while the K2 is passive and VBUS is dark and calls clear() the moment a gate drops; no verdict
// before the lines have been watched for kObserveMs.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/pd/ra_guard.hpp"

namespace k2::raguard {

enum class CrossKind : std::uint8_t { None = 0, Hint, Crossed };

class CrossTrace {
public:
    static constexpr std::size_t kSamples = 96;           // at a 20 ms tick ~1.9 s of history
    static constexpr std::uint32_t kWindowMs = 1600;      // how far back classify() looks
    static constexpr std::uint32_t kObserveMs = 1000;     // no verdict before the lines were watched this long
    static constexpr std::int32_t kLoadedMinMv = 200;     // Rp loaded by an Ra: the band measured on the rig, ~0.5 V
    static constexpr std::int32_t kLoadedMaxMv = 800;
    static constexpr std::int32_t kQuietRangeMv = 300;    // "nothing": a line that stays within this span, below an Rp
    static constexpr unsigned kToggleEdges = 3;           // level-band changes in the window = a toggling line

    void clear() { n_ = 0; head_ = 0; }
    void add(std::uint32_t now_ms, std::int32_t cc1_mv, std::int32_t cc2_mv) {
        if (cc1_mv < 0 || cc2_mv < 0) return;   // no ADC reading
        if (n_ == 0) first_ms_ = now_ms;
        s_[head_] = {now_ms, cc1_mv, cc2_mv};
        head_ = (head_ + 1) % kSamples;
        if (n_ < kSamples) ++n_;
    }
    std::size_t count() const { return n_; }

    CrossKind classify(std::uint32_t now_ms) const {
        if (n_ == 0 || now_ms < first_ms_ || now_ms - first_ms_ < kObserveMs) return CrossKind::None;
        Line a, b;
        const std::size_t oldest = (head_ + kSamples - n_) % kSamples;
        std::size_t m = 0;
        for (std::size_t i = 0; i < n_; ++i) {
            const S& s = s_[(oldest + i) % kSamples];
            if (s.t > now_ms || s.t + kWindowMs <= now_ms) continue;
            a.add(s.cc1);
            b.add(s.cc2);
            ++m;
        }
        if (m == 0) return CrossKind::None;
        const bool a_steady = a.steady_loaded() || a.steady_rp(), b_steady = b.steady_loaded() || b.steady_rp();
        if ((a_steady && b.toggling()) || (b_steady && a.toggling())) return CrossKind::Crossed;
        if ((a.steady_loaded() && b.quiet()) || (b.steady_loaded() && a.quiet())) return CrossKind::Hint;
        return CrossKind::None;
    }

private:
    struct S { std::uint32_t t; std::int32_t cc1, cc2; };
    // One line's statistics over the window.
    struct Line {
        std::int32_t lo = INT32_MAX, hi = INT32_MIN;
        std::int32_t prev_band = -1;
        unsigned edges = 0;
        static std::int32_t band(std::int32_t mv) {
            return mv < kLoadedMinMv ? 0 : mv < 1000 ? 1 : mv < kRpMinMv ? 2 : 3;
        }
        void add(std::int32_t mv) {
            if (mv < lo) lo = mv;
            if (mv > hi) hi = mv;
            const std::int32_t b = band(mv);
            if (prev_band >= 0 && b != prev_band) ++edges;
            prev_band = b;
        }
        bool steady_loaded() const { return lo >= kLoadedMinMv && hi <= kLoadedMaxMv; }
        bool steady_rp() const { return lo >= kRpMinMv; }
        bool toggling() const { return edges >= kToggleEdges; }
        bool quiet() const { return hi - lo <= kQuietRangeMv && hi < kRpMinMv && !steady_loaded(); }
    };
    std::array<S, kSamples> s_{};
    std::size_t n_ = 0, head_ = 0;
    std::uint32_t first_ms_ = 0;
};

}  // namespace k2::raguard
