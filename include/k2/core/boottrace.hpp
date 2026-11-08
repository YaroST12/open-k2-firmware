// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Power trace of the first seconds after reset (pure part: sample packing, ring, register image).
//
// Hardware result on the self-powered boot: with only a charger attached the K2 stays up while PLUS is
// held and dies when it is released. To see what the supply, the CC lines and the button did in the
// last moments, a sample is taken every 50 ms from the first moment after the clocks (src/boottrace.cpp)
// and the newest kRing samples are kept in backup registers. A dip that resets the MCU without
// removing the backup supply leaves the trace for the next boot to print; a total power loss clears it
// (no battery on VBAT), and the live [BOOT] lines are the only record then.
//
// A sample packs into 24 bits: vbus 7 bits (160 mV steps, 20.3 V), aux rail 7 bits (300 mV steps,
// 38 V), PLUS 1 bit (down), CC1 and CC2 4 bits each (256 mV steps, 3.8 V). Register image, 14
// halfwords: [0] magic, [1] CRC-16/CCITT over the bytes of [2..13], [2] sample count (wraps), [3..13]
// 22 bytes = 7 samples of 3 bytes (slot = index % 7) + the boot mode in byte 21.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace k2::boottrace {

inline constexpr std::uint16_t kMagic = 0x7B54;
inline constexpr std::size_t kWords = 14;
inline constexpr std::size_t kRing = 7;
inline constexpr std::uint32_t kPeriodMs = 50;
inline constexpr std::uint32_t kSpanMs = 3000;
inline constexpr std::uint32_t kVbusStepMv = 160, kAuxStepMv = 300, kCcStepMv = 256;

struct Sample {
    std::uint16_t vbus_mv = 0, aux_mv = 0, cc1_mv = 0, cc2_mv = 0;
    bool plus = false;   // PLUS held (pin low)
};

constexpr std::uint32_t clampq(std::uint32_t mv, std::uint32_t step, std::uint32_t max) {
    const std::uint32_t q = mv / step;
    return q > max ? max : q;
}

constexpr std::uint32_t pack(const Sample& s) {
    return clampq(s.vbus_mv, kVbusStepMv, 127) | clampq(s.aux_mv, kAuxStepMv, 127) << 7 | (s.plus ? 1u : 0u) << 14 |
           clampq(s.cc1_mv, kCcStepMv, 15) << 15 | clampq(s.cc2_mv, kCcStepMv, 15) << 19;
}

constexpr Sample unpack(std::uint32_t v) {
    Sample s;
    s.vbus_mv = std::uint16_t((v & 127) * kVbusStepMv);
    s.aux_mv = std::uint16_t((v >> 7 & 127) * kAuxStepMv);
    s.plus = (v >> 14 & 1) != 0;
    s.cc1_mv = std::uint16_t((v >> 15 & 15) * kCcStepMv);
    s.cc2_mv = std::uint16_t((v >> 19 & 15) * kCcStepMv);
    return s;
}

struct Trace {
    std::uint16_t count = 0;        // samples taken (wraps)
    std::uint8_t mode = 0;          // bootmode::Mode of that boot
    std::array<std::uint32_t, kRing> ring{};

    void push(const Sample& s) { ring[count % kRing] = pack(s); count++; }
    std::size_t stored() const { return count < kRing ? count : kRing; }
    // i = 0 is the newest sample.
    std::uint32_t at(std::size_t i) const { return ring[(count + kRing - 1 - i) % kRing]; }
};

constexpr std::uint16_t crc16(const std::array<std::uint16_t, kWords>& w) {
    std::uint16_t crc = 0xFFFF;
    for (std::size_t i = 2; i < kWords; i++) {
        for (int byte = 0; byte < 2; byte++) {
            crc = std::uint16_t(crc ^ (std::uint16_t(w[i] >> (8 * byte) & 0xFF) << 8));
            for (int b = 0; b < 8; b++) crc = std::uint16_t((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

constexpr std::array<std::uint16_t, kWords> encode(const Trace& t) {
    std::array<std::uint8_t, 22> b{};
    for (std::size_t i = 0; i < kRing; i++)
        for (std::size_t k = 0; k < 3; k++) b[3 * i + k] = std::uint8_t(t.ring[i] >> (8 * k));
    b[21] = t.mode;
    std::array<std::uint16_t, kWords> w{};
    w[0] = kMagic;
    w[2] = t.count;
    for (std::size_t i = 0; i < 11; i++) w[3 + i] = std::uint16_t(b[2 * i] | b[2 * i + 1] << 8);
    w[1] = crc16(w);
    return w;
}

constexpr std::optional<Trace> decode(const std::array<std::uint16_t, kWords>& w) {
    if (w[0] != kMagic || w[1] != crc16(w)) return std::nullopt;
    std::array<std::uint8_t, 22> b{};
    for (std::size_t i = 0; i < 11; i++) {
        b[2 * i] = std::uint8_t(w[3 + i] & 0xFF);
        b[2 * i + 1] = std::uint8_t(w[3 + i] >> 8);
    }
    Trace t;
    t.count = w[2];
    t.mode = b[21];
    for (std::size_t i = 0; i < kRing; i++) t.ring[i] = b[3 * i] | b[3 * i + 1] << 8 | std::uint32_t(b[3 * i + 2]) << 16;
    return t;
}

// "t=150ms vbus=4960 aux=4800 plus=1 cc1=1536 cc2=0", NUL terminated, truncated to `n`; returns the length.
inline std::size_t format_sample(char* out, std::size_t n, const Sample& s, std::uint32_t t_ms) {
    if (n == 0) return 0;
    std::size_t len = 0;
    auto put = [&](char c) { if (len + 1 < n) out[len++] = c; };
    auto str = [&](const char* p) { while (*p) put(*p++); };
    auto num = [&](unsigned v) {
        char t[10];
        int k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v);
        while (k) put(t[--k]);
    };
    str("t="); num(t_ms); str("ms vbus="); num(s.vbus_mv); str(" aux="); num(s.aux_mv);
    str(" plus="); num(s.plus ? 1 : 0); str(" cc1="); num(s.cc1_mv); str(" cc2="); num(s.cc2_mv);
    out[len] = '\0';
    return len;
}

}  // namespace k2::boottrace
