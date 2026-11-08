// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// VIVO FlashCharge serial frames, pure code. USART2 115200 8N2 on PA2/PA3. PA7/PB0 (the D+/D-
// dividers) are left as analog inputs and PB1/PB2 are released, so there is no D+/D- handshake:
// the UART is simply claimed with the data lines high-Z. Frame format: docs/FASTCHARGE_PROTOCOLS.md.
//
//   frame  = 0xAA, n, data[n], sum        sum = (0x56 + every byte before it) & 0xFF
//   reply  = AA n 0x08 <echo of the command> ...      (byte 3 is compared with the command
//                                                       byte of the frame that was sent)
//   notice = AA 01 55 <sum>                           (charger entry notice; the K2 answers kNoticeAck
//                                                       and restarts the sequence at index 0)
//
// The K2 sends a table of 14 canned frames, each once, advancing when the reply echoes the command
// or after about 50 ms. Command bytes: 04 55 02 16 01 1E 03 19 1E 1F 16 A6 20 0D. The 0x55 frame
// (aa 03 55 00 00 08) has a non-standard checksum (0x08 instead of 0x58) and is sent verbatim; its
// purpose is unknown. Command 03 carries two little-endian halfwords: the first is a current limit
// in mA (2000 in the canned 5 V frame, 6000 in the dynamic frame for an unidentified charger), the
// second the requested voltage in mV (5000 canned, 7000 start, 100 mV steps up to 20000).
// `aa 01 17 18` is a separate keepalive, not part of the table; the active phase sends
// `aa 01 a6 a7` every 100 ms.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::vivo {

inline constexpr std::uint8_t kSync = 0xAA;
inline constexpr std::uint8_t kReplyCmd = 0x08;
inline constexpr std::uint8_t kSumInit = 0x56;
inline constexpr std::uint16_t kStartMv = 7000;
inline constexpr std::uint16_t kStepMv = 100;
inline constexpr std::uint16_t kMaxMv = 20000;
// Highest level the engine asks for from an unidentified charger.
inline constexpr std::uint16_t kUnidentifiedMaxMv = 11000;

constexpr std::uint8_t checksum(std::span<const std::uint8_t> b) {
    std::uint8_t s = kSumInit;
    for (auto v : b) s = static_cast<std::uint8_t>(s + v);
    return s;
}

struct Canned {
    std::uint8_t len;                       // total bytes on the wire
    std::array<std::uint8_t, 10> bytes;
};
inline constexpr std::uint8_t kKeepaliveCmd = 0xA6;
inline constexpr std::uint8_t kNoticeCmd = 0x55;
inline constexpr std::uint16_t kDefaultMa = 6000;    // start value of the dynamic frame
inline constexpr std::uint16_t kCannedMa = 2000;
inline constexpr std::uint16_t kSafeMv = 5000;

// The canned frames in the order they are sent.
inline constexpr std::array<Canned, 14> kSequence{{
    {4, {0xAA, 0x01, 0x04, 0x05}},
    {6, {0xAA, 0x03, 0x55, 0x00, 0x00, 0x08}},
    {4, {0xAA, 0x01, 0x02, 0x03}},
    {5, {0xAA, 0x02, 0x16, 0x00, 0x18}},
    {4, {0xAA, 0x01, 0x01, 0x02}},
    {8, {0xAA, 0x05, 0x1E, 0xAE, 0x05, 0x00, 0x00, 0xD6}},
    {10, {0xAA, 0x07, 0x03, 0xD0, 0x07, 0x88, 0x13, 0x00, 0x00, 0x7C}},
    {5, {0xAA, 0x02, 0x19, 0x00, 0x1B}},
    {8, {0xAA, 0x05, 0x1E, 0xAE, 0x05, 0x00, 0x00, 0xD6}},
    {5, {0xAA, 0x02, 0x1F, 0x80, 0xA1}},
    {5, {0xAA, 0x02, 0x16, 0x00, 0x18}},
    {4, {0xAA, 0x01, 0xA6, 0xA7}},
    {4, {0xAA, 0x01, 0x20, 0x21}},
    {4, {0xAA, 0x01, 0x0D, 0x0E}},
}};
inline constexpr Canned kKeepalive{4, {0xAA, 0x01, 0xA6, 0xA7}};
inline constexpr Canned kNoticeAck{6, {0xAA, 0x03, 0x08, 0x55, 0x00, 0x60}};

// Command 3 frame: first halfword (current limit mA), requested voltage in mV.
constexpr std::size_t encode_voltage(std::span<std::uint8_t> out, std::uint16_t first, std::uint16_t mv) {
    if (out.size() < 10) return 0;
    out[0] = kSync;
    out[1] = 7;
    out[2] = 0x03;
    out[3] = static_cast<std::uint8_t>(first);
    out[4] = static_cast<std::uint8_t>(first >> 8);
    out[5] = static_cast<std::uint8_t>(mv);
    out[6] = static_cast<std::uint8_t>(mv >> 8);
    out[7] = 0;
    out[8] = 0;
    out[9] = checksum(out.first(9));
    return 10;
}

struct Reply {
    std::uint8_t cmd = 0;    // first data byte (0x08 for an ack, 0x55 for the sink notice)
    std::uint8_t echo = 0;   // second data byte, the command being acknowledged
};

enum class Parse : std::uint8_t { Need, Bad, Ok };

// Frames start at the first 0xAA; `used` is how many bytes to drop (a bad frame drops the sync only).
constexpr Parse decode(std::span<const std::uint8_t> in, Reply& r, std::size_t& used) {
    std::size_t s = 0;
    while (s < in.size() && in[s] != kSync) s++;
    used = s;
    if (in.size() - s < 2) return Parse::Need;
    const std::size_t n = in[s + 1];
    const std::size_t total = n + 3;
    if (n == 0 || n > 16) { used = s + 1; return Parse::Bad; }
    if (in.size() - s < total) return Parse::Need;
    if (checksum(in.subspan(s, total - 1)) != in[s + total - 1]) { used = s + 1; return Parse::Bad; }
    r.cmd = in[s + 2];
    r.echo = n >= 2 ? in[s + 3] : 0;
    used = s + total;
    return Parse::Ok;
}

constexpr bool acks(const Reply& r, std::uint8_t sent_cmd) { return r.cmd == kReplyCmd && r.echo == sent_cmd; }

}  // namespace k2::vivo
