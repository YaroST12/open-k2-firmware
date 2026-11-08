// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Huawei ACCP register protocol, pure: CRC-8, read/write frames and their responses, the
// FCP / SCP register map constants and the value conversions.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace k2::accp {

// CRC-8, polynomial 0x39, init 0, MSB first, no final xor.
constexpr std::uint8_t crc8(std::span<const std::uint8_t> buf) {
    std::uint8_t crc = 0;
    for (std::uint8_t b : buf) {
        crc ^= b;
        for (int i = 0; i < 8; i++) crc = static_cast<std::uint8_t>(crc & 0x80 ? (crc << 1) ^ 0x39 : crc << 1);
    }
    return crc;
}

inline constexpr std::uint8_t kCmdRead = 0x0C, kCmdWrite = 0x0B, kAck = 0x08;
// Write ack CRC quoted by the spec (5.3); its algorithm gives 0xF1 for [0x08]. Both are accepted.
inline constexpr std::uint8_t kSpecWriteAckCrc = 0xD2;

constexpr std::array<std::uint8_t, 3> read_frame(std::uint8_t reg) {
    const std::uint8_t b[2] = {kCmdRead, reg};
    return {kCmdRead, reg, crc8(b)};
}

constexpr std::array<std::uint8_t, 4> write_frame(std::uint8_t reg, std::uint8_t val) {
    const std::uint8_t b[3] = {kCmdWrite, reg, val};
    return {kCmdWrite, reg, val, crc8(b)};
}

// Response [0x08, data, crc] to a read.
constexpr std::optional<std::uint8_t> parse_read(std::span<const std::uint8_t> rx) {
    if (rx.size() < 3 || rx[0] != kAck || rx[2] != crc8(rx.first(2))) return std::nullopt;
    return rx[1];
}

// Response [0x08, crc] to a write.
constexpr bool parse_write_ack(std::span<const std::uint8_t> rx) {
    return rx.size() >= 2 && rx[0] == kAck && (rx[1] == crc8(rx.first(1)) || rx[1] == kSpecWriteAckCrc);
}

// FCP registers: capabilities, enable, voltage (100 mV units), current (100 mA units).
inline constexpr std::uint8_t kRegFcpCaps = 0x21, kRegFcpVoutEn = 0x2B, kRegFcpVout = 0x2C, kRegFcpIout = 0x2D;
inline constexpr std::uint8_t kFcpIoutCode = 20;   // 2.0 A

constexpr std::optional<std::uint8_t> fcp_vout_code(int mv) {
    switch (mv) {
    case 5000:  return std::uint8_t{50};
    case 9000:  return std::uint8_t{90};
    case 12000: return std::uint8_t{120};
    default:    return std::nullopt;
    }
}

// SCP registers.
inline constexpr std::uint8_t kRegScpType1 = 0x7E, kRegScpType0 = 0x80, kRegScpMaxPower = 0x90,
                              kRegScpLimits = 0x90,   // 0x90 .. 0x97
                              kRegScpCtrl0 = 0xA0, kRegScpCtrl1 = 0xA1, kRegScpVset = 0xB0, kRegScpIset = 0xB2,
                              kRegScpVread = 0xB8, kRegScpIread = 0xBA;
inline constexpr std::uint8_t kScpCtrl0Enable = 0xC0, kScpCtrl1Value = 0x06;
inline constexpr int kScpInitMv = 5000, kScpInitMa = 3500, kScpEnableWaitMs = 22, kScpKeepAliveMs = 100;
inline constexpr std::uint8_t kRegScpVset8 = 0xCA, kRegScpIset8 = 0xCB;   // the set registers this adapter honours
inline constexpr int kScpMaxMv8 = 5550;   // 3000 + 255 * 10
inline constexpr int kScpMinMv = 3000, kScpMaxMv = 11000;

constexpr bool scp_supported(std::uint8_t type0) { return (type0 & 0x10) != 0; }
constexpr bool scp_type1_valid(std::uint8_t type1) { return (type1 & 0x10) == 0; }

constexpr int be16(std::uint8_t hi, std::uint8_t lo) { return hi << 8 | lo; }
constexpr std::uint8_t hi8(int v) { return static_cast<std::uint8_t>(v >> 8); }
constexpr std::uint8_t lo8(int v) { return static_cast<std::uint8_t>(v); }

constexpr int scp_vread8_mv(std::uint8_t reg) { return reg * 10 + 3000; }
constexpr int scp_iread8_ma(std::uint8_t reg) { return reg * 50; }
constexpr std::uint8_t scp_vset8(int mv) { return static_cast<std::uint8_t>(std::max(0, mv - 3000) / 10); }

// Step encoding of the limit registers: exponent bits [7:6] -> 1, 10, 100, 1000; mantissa [5:0].
// The unit of the decoded value is not documented; it is only logged.
constexpr int scp_step_decode(std::uint8_t reg) {
    constexpr int mult[4] = {1, 10, 100, 1000};
    return (reg & 0x3F) * mult[reg >> 6];
}

// What the adapter says about itself in registers 0x90..0x97 (inferred: 0x92/0x93 min/max voltage, 0x94/0x95 min/max
// current, step-encoded, decoded to mV / mA). `valid` only for a list that reads like one (an idle line, an all-zero
// block, a max below the min, or values in other units are not capabilities).
struct ScpCaps {
    int min_mv = 0, max_mv = 0, min_ma = 0, max_ma = 0;
    bool valid = false;
};

constexpr ScpCaps scp_caps_decode(const std::array<std::uint8_t, 8>& r) {
    ScpCaps c;
    c.min_mv = scp_step_decode(r[2]);
    c.max_mv = scp_step_decode(r[3]);
    c.min_ma = scp_step_decode(r[4]);
    c.max_ma = scp_step_decode(r[5]);
    c.valid = c.max_mv >= kScpMinMv && c.max_mv <= 40000 && c.min_mv <= c.max_mv && c.max_ma >= 500 &&
              c.min_ma <= c.max_ma;
    return c;
}

// Programmable target: 3 V .. `max_mv` (11 V unless the adapter reported a maximum), lowered to the ceiling; 0 without a ceiling.
constexpr int clamp_scp_mv(int mv, int ceiling_mv, int max_mv = kScpMaxMv) {
    return ceiling_mv <= 0 ? 0 : std::min(std::clamp(mv, kScpMinMv, std::max(max_mv, kScpMinMv)), ceiling_mv);
}

}  // namespace k2::accp
