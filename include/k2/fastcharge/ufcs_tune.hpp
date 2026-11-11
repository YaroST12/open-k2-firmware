// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS runtime tuner, pure. The defaults are the standard behaviour (reverse-engineered) as confirmed on a
// real source (Baseus FH22 charger): 8N1 at 115200, receiver address 1 (source), protocol version 1,
// Ping first, CRC-8 poly 0x29 over everything after the sync byte. Every one of those stays a field so a
// source that deviates can still be tried: header address / version / type bits, the opener and keep-alive
// codes, the retransmit timing, the CRC definition, and the UART and line settings. The tuner lives in
// RAM only (reset at boot); the shell (`ufcs set`), the remote (UFCS command) and tools/ufcs_sweep.py
// change it, `ufcs probe` runs the handshake and the opener exchange with it. Nothing here touches a pin.
//
// Not a field: swapping TX onto D+ and RX onto D-. USART2 on this part has no TX/RX swap and the two
// pins are fixed (PA2 -> D-, PA3 <- D+), so that experiment needs a hardware change.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::ufcs {

struct Tuner {
    // Frame header (16 bits, sent big-endian, 8.2.2): receiver address [15:13], message number [12:9],
    // protocol version [8:3], message type [2:0].
    std::uint8_t addr = 1;         // [15:13]: who receives the frame; every frame we send goes to the source (1)
    std::uint8_t version = 1;      // [8:3]: the version the Ping announces (V1.0 = 000001b)
    std::uint8_t ctl_type = 0;     // [2:0] of a control frame
    std::uint8_t data_type = 1;    // [2:0] of a data frame
    // Messages and timing.
    std::uint8_t opener = 0;       // first command sent after the handshake (0 = Ping; 6 = Get_Output_Capabilities)
    std::uint8_t keepalive = 0;    // keep-alive command (0 = Ping)
    std::uint16_t retry_ms = 20;   // opener retransmit interval, for the first three tries
    std::uint16_t listen_ms = 500; // how long the opener is repeated before "no response"
    // CRC-8 over the frame.
    bool crc_sync = false;         // the CRC also covers the sync byte
    std::uint8_t crc_init = 0;
    std::uint8_t crc_poly = 0x29;
    bool crc_lsb = false;          // reflected (least significant bit first) instead of MSB first
    // UART and lines (used when the UART phase opens).
    std::uint8_t stop_bits = 1;    // 1 or 2 (the standard is 8N1)
    std::uint32_t baud = 115200;
    std::uint16_t delay_ms = 10;   // between the handshake and the first frame
    std::uint8_t dm = 0;           // D- in the UART phase: 0 released, 1 held at 0 V, 2 held at 3.3 V (through the divider)
    std::uint8_t dp = 0;           // D+ in the UART phase: 0 released, 1 biased at 0.6 V
    constexpr bool operator==(const Tuner&) const = default;
};

inline constexpr Tuner kDefaultTuner{};
constexpr bool is_default(const Tuner& t) { return t == kDefaultTuner; }

enum class Field : std::uint8_t {
    Addr, Version, CtlType, DataType, Opener, KeepAlive, RetryMs, ListenMs,
    CrcSync, CrcInit, CrcPoly, CrcLsb, StopBits, Baud, DelayMs, Dm, Dp,
    Count
};
inline constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);

struct FieldDef {
    const char* name;
    std::uint32_t lo, hi, def;
};

// One row per Field, in enum order. Ranges are what the hardware or the wire can carry; the delay is
// capped because it blocks the main loop.
inline constexpr FieldDef kFields[kFieldCount] = {
    {"addr", 0, 7, 1},
    {"version", 0, 63, 1},
    {"ctl", 0, 7, 0},
    {"data", 0, 7, 1},
    {"opener", 0, 255, 0},
    {"keepalive", 0, 255, 0},
    {"retry", 1, 1000, 20},
    {"listen", 50, 5000, 500},
    {"crcsync", 0, 1, 0},
    {"crcinit", 0, 255, 0},
    {"crcpoly", 1, 255, 0x29},
    {"crclsb", 0, 1, 0},
    {"stop", 1, 2, 1},
    {"baud", 1200, 230400, 115200},
    {"delay", 0, 200, 10},
    {"dm", 0, 2, 0},
    {"dp", 0, 1, 0},
};

constexpr std::uint32_t get(const Tuner& t, Field f) {
    switch (f) {
    case Field::Addr: return t.addr;
    case Field::Version: return t.version;
    case Field::CtlType: return t.ctl_type;
    case Field::DataType: return t.data_type;
    case Field::Opener: return t.opener;
    case Field::KeepAlive: return t.keepalive;
    case Field::RetryMs: return t.retry_ms;
    case Field::ListenMs: return t.listen_ms;
    case Field::CrcSync: return t.crc_sync ? 1 : 0;
    case Field::CrcInit: return t.crc_init;
    case Field::CrcPoly: return t.crc_poly;
    case Field::CrcLsb: return t.crc_lsb ? 1 : 0;
    case Field::StopBits: return t.stop_bits;
    case Field::Baud: return t.baud;
    case Field::DelayMs: return t.delay_ms;
    case Field::Dm: return t.dm;
    case Field::Dp: return t.dp;
    case Field::Count: break;
    }
    return 0;
}

// False (and nothing changed) when the value is outside the field's range.
constexpr bool set(Tuner& t, Field f, std::uint32_t v) {
    if (f >= Field::Count) return false;
    const FieldDef& d = kFields[static_cast<std::size_t>(f)];
    if (v < d.lo || v > d.hi) return false;
    switch (f) {
    case Field::Addr: t.addr = static_cast<std::uint8_t>(v); break;
    case Field::Version: t.version = static_cast<std::uint8_t>(v); break;
    case Field::CtlType: t.ctl_type = static_cast<std::uint8_t>(v); break;
    case Field::DataType: t.data_type = static_cast<std::uint8_t>(v); break;
    case Field::Opener: t.opener = static_cast<std::uint8_t>(v); break;
    case Field::KeepAlive: t.keepalive = static_cast<std::uint8_t>(v); break;
    case Field::RetryMs: t.retry_ms = static_cast<std::uint16_t>(v); break;
    case Field::ListenMs: t.listen_ms = static_cast<std::uint16_t>(v); break;
    case Field::CrcSync: t.crc_sync = v != 0; break;
    case Field::CrcInit: t.crc_init = static_cast<std::uint8_t>(v); break;
    case Field::CrcPoly: t.crc_poly = static_cast<std::uint8_t>(v); break;
    case Field::CrcLsb: t.crc_lsb = v != 0; break;
    case Field::StopBits: t.stop_bits = static_cast<std::uint8_t>(v); break;
    case Field::Baud: t.baud = v; break;
    case Field::DelayMs: t.delay_ms = static_cast<std::uint16_t>(v); break;
    case Field::Dm: t.dm = static_cast<std::uint8_t>(v); break;
    case Field::Dp: t.dp = static_cast<std::uint8_t>(v); break;
    case Field::Count: break;
    }
    return true;
}

constexpr bool find_field(std::string_view name, Field& out) {
    for (std::size_t i = 0; i < kFieldCount; ++i) {
        if (name == kFields[i].name) {
            out = static_cast<Field>(i);
            return true;
        }
    }
    return false;
}

// Tries at the retransmit interval before the keep-alive pace takes over (inferred).
inline constexpr int kFastTries = 3;
inline constexpr std::uint32_t kSlowRetryMs = 100;

// Wait before opener retransmission number `try_no` (1 = the first retransmit): the retransmit interval
// for the first kFastTries - 1, then the keep-alive pace so a slow source is not flooded.
constexpr std::uint32_t open_interval_ms(const Tuner& t, int try_no) {
    return try_no < kFastTries ? t.retry_ms : kSlowRetryMs;
}

}  // namespace k2::ufcs
