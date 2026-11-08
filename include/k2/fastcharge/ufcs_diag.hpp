// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS bring-up diagnostics, pure: the hex text of wire bytes for the log lines and the classifier
// of the D+/D- handshake timings. Nothing here drives a pin or changes what the engine decides.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::ufcs {

// Bytes per log line: the binary log keeps at most binlog kStrMax = 12 characters of a string argument, so a
// line carries 6 bytes (12 hex characters). 16 lost bytes 7..16 of every chunk (seen as 6-byte [UART2] lines).
inline constexpr std::size_t kHexChunk = 6;

constexpr std::size_t chunk_count(std::size_t bytes) { return (bytes + kHexChunk - 1) / kHexChunk; }

// Upper-case hex without separators (the log encoder escapes spaces), NUL-terminated. Only whole
// bytes that fit with the terminator are written; returns the characters written (no NUL).
constexpr std::size_t hex(std::span<const std::uint8_t> bytes, std::span<char> out) {
    if (out.empty()) return 0;
    constexpr char kDigits[] = "0123456789ABCDEF";
    std::size_t n = 0;
    for (const std::uint8_t b : bytes) {
        if (n + 3 > out.size()) break;
        out[n++] = kDigits[b >> 4];
        out[n++] = kDigits[b & 0xF];
    }
    out[n] = '\0';
    return n;
}

// Microseconds still to wait to fill a `total_us` window after a timed wait that returned `elapsed_us`
// (-1 = the wait ran into its timeout, so the whole window is gone).
constexpr std::uint32_t remaining_us(std::uint32_t total_us, int elapsed_us) {
    if (elapsed_us < 0) return 0;
    const auto e = static_cast<std::uint32_t>(elapsed_us);
    return e >= total_us ? 0 : total_us - e;
}

// Handshake timings in microseconds from each D- edge to the D+ edge that follows it: the two
// rises (D- to 3.3 V), the two falls (D- to 0 V) and the final D+ rise, timed from the 4th signal's D- fall (D+ detection starts there).
// -1 = D+ never moved inside the wait.
struct Handshake {
    int rise_us[2];
    int fall_us[2];
    int final_us;
};

// A BC1.2 DCP shorts D+ to D-, so D+ follows D- within the pin's slew time (microseconds). The
// reference gives no answer latency for a UFCS source, so this only labels the timings: it never
// changes the pass/fail of the handshake. kMirrorMaxUs is a guess (a short plus pin slew), not a spec value.
inline constexpr int kMirrorMaxUs = 100;
enum class HsClass : std::uint8_t { NoAnswer, Mirror, Delayed };

constexpr HsClass classify(const Handshake& h) {
    const int all[5] = {h.rise_us[0], h.fall_us[0], h.rise_us[1], h.fall_us[1], h.final_us};
    bool instant = true;
    for (const int us : all) {
        if (us < 0) return HsClass::NoAnswer;
        if (us > kMirrorMaxUs) instant = false;
    }
    return instant ? HsClass::Mirror : HsClass::Delayed;
}

constexpr const char* name(HsClass c) {
    switch (c) {
    case HsClass::Mirror: return "mirror";
    case HsClass::Delayed: return "delayed";
    case HsClass::NoAnswer: break;
    }
    return "none";
}

}  // namespace k2::ufcs
