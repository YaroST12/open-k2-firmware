// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ACCP / AFC single-wire physical layer on D-, pure: step-list builders for the master
// ping and one transmitted frame, the slave-ping width window and the receive state machine.
// D+ stays at 0.6 V throughout; on D- a high bit is 2.0 V and a low bit is 0 V.
#pragma once
#include <bit>
#include <cstdint>
#include <span>

#include "k2/fastcharge/fc_seq.hpp"

namespace k2::wire {

using fc::Drive;

inline constexpr std::uint32_t kUiUs = 162;          // one bit time
inline constexpr std::uint32_t kPingHighUs = 2570;   // master ping: D- 2.0 V ...
inline constexpr std::uint32_t kPingLowUs = 10;      // ... then 0 V
inline constexpr std::uint32_t kSyncUs = 41;         // sync header pulse width
inline constexpr std::uint32_t kStopUs = 40;         // stop pulse width
inline constexpr std::uint32_t kSlavePingWaitUs = 2000;
inline constexpr std::uint32_t kSlavePingMinUs = 300, kSlavePingMaxUs = 4000;
inline constexpr std::uint32_t kRxPeriodUs = 10;     // RX sampling period, 16 samples per bit
inline constexpr std::size_t kRxSamples = 9 * kUiUs / kRxPeriodUs;   // 8 data bits + parity

constexpr bool slave_ping_ok(std::uint32_t width_us) {
    return width_us >= kSlavePingMinUs && width_us <= kSlavePingMaxUs;
}

// Odd parity: with an odd number of ones the parity bit is 0, with an even number it is 1.
constexpr bool parity_bit(std::uint8_t byte) { return std::popcount(byte) % 2 == 0; }

template <std::size_t N>
void master_ping(fc::StepList<N>& out) {
    out.push(Drive::V0p6, Drive::V2p0, kPingHighUs);
    out.push(Drive::V0p6, Drive::V0, kPingLowUs);
}

// One transmitted frame. The 4-pulse header (low, high, low, high at
// 41 us) is sent ONCE per frame. Every byte is 8 data bits MSB first (162 us), the parity bit, then a stop:
// odd ones -> parity low, high 40; even ones -> parity high, low 40, high 40. What follows depends on the
// previous byte's parity and bit 7 of the next byte (the line is high after the stop):
//   odd,  bit7=1: low 40            odd,  bit7=0: low 41, high 41 (re-sync pulse)
//   even, bit7=1: low 40            even, bit7=0: nothing
// The first byte follows the header (high) like an even one. After the last byte the line goes low 40 us; the
// caller then sends the end-of-frame master_ping. Needs 4 + 15 steps per byte at most.
template <std::size_t N>
void frame_steps(fc::StepList<N>& out, std::span<const std::uint8_t> bytes) {
    for (int i = 0; i < 4; i++) out.push(Drive::V0p6, i % 2 ? Drive::V2p0 : Drive::V0, kSyncUs);
    bool prev_odd = false;
    for (const std::uint8_t byte : bytes) {
        const bool msb = (byte & 0x80) != 0;
        if (msb) out.push(Drive::V0p6, Drive::V0, kStopUs);
        else if (prev_odd) {
            out.push(Drive::V0p6, Drive::V0, kSyncUs);
            out.push(Drive::V0p6, Drive::V2p0, kSyncUs);
        }
        for (int b = 7; b >= 0; b--) out.push(Drive::V0p6, (byte >> b) & 1 ? Drive::V2p0 : Drive::V0, kUiUs);
        const bool p = parity_bit(byte);
        out.push(Drive::V0p6, p ? Drive::V2p0 : Drive::V0, kUiUs);
        if (p) out.push(Drive::V0p6, Drive::V0, kStopUs);
        out.push(Drive::V0p6, Drive::V2p0, kStopUs);
        prev_odd = !p;
    }
    out.push(Drive::V0p6, Drive::V0, kStopUs);
}

// The receive loop over D- samples taken every kRxPeriodUs (10 us). Highs > 160 in a row
// end the frame (the charger's closing ping); a falling edge after a short high (< 10 samples) is a header or
// stop pulse: one that follows another within 30 samples restarts the byte, a lone one closes a byte. Data
// bit k is the level 7 + 16k samples after the last such falling edge (70 + 160k us), MSB first; the parity
// bit is read and ignored. Returns the number of bytes completed (at most out.size()).
inline std::size_t decode_frame(std::span<const std::uint8_t> s, std::span<std::uint8_t> out) {
    const std::size_t len = out.size();
    unsigned hi = 0, lo = 0, since = 0, bit = 0;
    std::size_t idx = 0;
    bool started = false, prev = false;
    for (const std::uint8_t v : s) {
        const bool cur = v != 0;
        since++;
        if (cur) {
            if (++hi > 160) break;
            lo = 0;
        } else {
            bool pulse = false, fresh = false;
            if (prev && hi < 10) {
                pulse = true;
                fresh = since < 30;
                since = 0;
                bit = 0;
            }
            if (++lo > 1000) break;
            hi = 0;
            if (fresh) {
                if (started) out[idx] = 0;
                else { idx = 0; started = true; out[0] = 0; }
            } else if (pulse && started) {
                if (++idx >= len) break;
                out[idx] = 0;
            }
        }
        if (started && since == 7 + 16 * bit) {
            if (bit < 8 && cur) out[idx] = static_cast<std::uint8_t>(out[idx] | (0x80 >> bit));
            bit++;
        }
        prev = cur;
    }
    return started ? idx : 0;
}

}  // namespace k2::wire
