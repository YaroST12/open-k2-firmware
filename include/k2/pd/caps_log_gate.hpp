// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Rate limit for the "[PD] Source_Capabilities received" K2LOG block, pure (host-tested in tests/test_caps_log_gate.cpp).
//
// Some sources repeat their caps every few hundred ms for as long as nobody answers; each repeat is a header line plus one
// line per PDO, which floods the 64-byte HID log ring and pushes everything else out. The first caps of a source (a new
// attach session), any change of content, and one block per kCapsLogIntervalMs are logged; the repeats held back in between
// are counted and reported with the next block. The Black Box (k2::pdtrace) records every message either way.
#pragma once
#include <cstdint>

namespace k2::pd {

inline constexpr std::uint32_t kCapsLogIntervalMs = 10000;

// FNV-1a over everything the block prints: the PDO words, their count, the spec revision and the active CC.
constexpr std::uint32_t caps_content_hash(const std::uint32_t* pdos, unsigned n, unsigned spec_rev, unsigned active_cc) {
    std::uint32_t h = 2166136261u;
    auto mix = [&h](std::uint32_t w) {
        for (unsigned i = 0; i < 4; i++) { h ^= (w >> (8 * i)) & 0xFF; h *= 16777619u; }
    };
    for (unsigned i = 0; i < n; i++) mix(pdos[i]);
    mix(n);
    mix(spec_rev);
    mix(active_cc);
    return h;
}

class CapsLogGate {
public:
    struct Decision {
        bool log = false;
        std::uint32_t repeats = 0;   // identical caps held back since the previous logged block
    };

    Decision on_caps(std::uint32_t session, std::uint32_t content_hash, std::uint32_t now_ms) {
        const bool first = !valid_ || session != session_ || content_hash != hash_ || now_ms - last_ms_ >= kCapsLogIntervalMs;
        if (!first) {
            held_++;
            return {};
        }
        Decision d{true, (valid_ && session == session_) ? held_ : 0};
        valid_ = true;
        session_ = session;
        hash_ = content_hash;
        last_ms_ = now_ms;
        held_ = 0;
        return d;
    }

private:
    bool valid_ = false;
    std::uint32_t session_ = 0, hash_ = 0, last_ms_ = 0, held_ = 0;
};

}  // namespace k2::pd
