// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS (T/TAF 083-2024, T/CCSA 393-2024) request arithmetic and wire frames, pure. Frame: sync 0xAA,
// 16-bit header (8.2.2), message code, [length, payload], CRC-8 poly 0x29 over everything after the
// sync. Section numbers in the comments are the standard's; the byte vectors in tests/test_ufcs_frame.cpp
// come from a real source (Baseus FH22 charger). docs/FASTCHARGE_PROTOCOLS.md section 4 has the summary.
#pragma once
#include <array>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <span>

#include "k2/fastcharge/ufcs_tune.hpp"

namespace k2::ufcs {

inline constexpr int kUnitMv = 10;             // output mode voltage unit (table 16, 17)
inline constexpr int kUnitMa = 10;             // output mode current unit
inline constexpr int kMinMv = 3300;            // lowest request ever made
// Current asked when the cable is unknown: we read no cable tag and measure no path impedance, so the
// worst row of table 36 (Rcable > 500 mOhm, at most 2 A) applies (9.6.3, 8.2.4.2).
inline constexpr int kNoCableMa = 2000;
inline constexpr std::size_t kMaxCaps = 7;     // an Output_Capabilities message holds 1..7 output modes
inline constexpr std::uint8_t kSync = 0xAA;

// Timers and counters (table 30, 8.4.3), milliseconds. The standard's limits first, then what the
// engine waits (the limit plus the polling and UART slack: a timer that fires early would reset a
// healthy source).
inline constexpr std::uint32_t kAckReceiveMs = 10;       // tACKReceive: the ACK must arrive within this
inline constexpr std::uint32_t kSenderResponseMs = 50;   // tSenderResponse: the answer after the ACK
inline constexpr std::uint32_t kReceiverResponseMs = 40; // tReceiverResponse: we answer a request this fast
inline constexpr std::uint32_t kPowerSupplyMs = 550;     // tPowerSupply: Accept to Power_Ready
inline constexpr std::uint32_t kMsgTransDelayMs = 2;     // tMsgTransDelay: between our ACK and our answer
inline constexpr std::uint32_t kResetLowMs = 2;          // hardware reset: the data line low for at least this long
inline constexpr int kMsgRetryCount = 3;                 // nMsgRetryCount: retransmits before Soft_Reset
inline constexpr std::uint32_t kAckWaitMs = 20;          // engine: retransmit when no ACK after this (2 x tACKReceive)
inline constexpr std::uint32_t kResponseWaitMs = 60;     // engine: resend / give up when no answer after the ACK
inline constexpr std::uint32_t kPowerWaitMs = 600;       // engine: hardware reset when no Power_Ready after the Accept
inline constexpr std::uint32_t kWatchdogMs = 1000;       // the source's default watchdog; any message from us clears it
inline constexpr std::uint32_t kKeepaliveMs = 100;       // our Ping pace (every 100 ms, a tenth of the watchdog)

// Receiver addresses (8.2.2, table 13): who the frame is for.
inline constexpr std::uint8_t kAddrSource = 1;
inline constexpr std::uint8_t kAddrSink = 2;
inline constexpr std::uint8_t kAddrCable = 3;

// Control commands (8.2.3, table 14).
inline constexpr std::uint8_t kCtlPing = 0;
inline constexpr std::uint8_t kCtlAck = 1;               // answers every message that is not an ACK / NCK, echoes its message number
inline constexpr std::uint8_t kCtlNack = 2;              // CRC failed: the sender retransmits at once
inline constexpr std::uint8_t kCtlAccept = 3;            // the source accepted the Request
inline constexpr std::uint8_t kCtlSoftReset = 4;
inline constexpr std::uint8_t kCtlPowerReady = 5;        // the output reached the requested voltage and current
inline constexpr std::uint8_t kCtlGetOutputCap = 6;
inline constexpr std::uint8_t kCtlGetSourceInfo = 7;
inline constexpr std::uint8_t kCtlGetSinkInfo = 8;
inline constexpr std::uint8_t kCtlGetCableInfo = 9;
inline constexpr std::uint8_t kCtlGetDeviceInfo = 0xA;
inline constexpr std::uint8_t kCtlGetErrorInfo = 0xB;
inline constexpr std::uint8_t kCtlDetectCableInfo = 0xC;
inline constexpr std::uint8_t kCtlStartCableDetect = 0xD;
inline constexpr std::uint8_t kCtlEndCableDetect = 0xE;
inline constexpr std::uint8_t kCtlExit = 0xF;            // Exit_UFCS_Mode: the source falls back to its default voltage
inline constexpr std::uint8_t kCtlGetSinkInfoExt = 0x10;
// Data commands (8.2.4, table 15).
inline constexpr std::uint8_t kDataOutputCap = 1;
inline constexpr std::uint8_t kDataRequest = 2;
inline constexpr std::uint8_t kDataSourceInfo = 3;
inline constexpr std::uint8_t kDataSinkInfo = 4;
inline constexpr std::uint8_t kDataCableInfo = 5;
inline constexpr std::uint8_t kDataDeviceInfo = 6;
inline constexpr std::uint8_t kDataErrorInfo = 7;
inline constexpr std::uint8_t kDataConfigWatchdog = 8;
inline constexpr std::uint8_t kDataRefuse = 9;
inline constexpr std::uint8_t kDataVerifyRequest = 0xA;
inline constexpr std::uint8_t kDataVerifyResponse = 0xB;
inline constexpr std::uint8_t kDataPowerChange = 0xC;
inline constexpr std::uint8_t kDataSinkInfoExt = 0xD;
// Refuse reasons (8.2.4.9, table 24).
inline constexpr std::uint8_t kRefuseUnknown = 1;        // command or data not recognised
inline constexpr std::uint8_t kRefuseUnsupported = 2;
inline constexpr std::uint8_t kRefuseBusy = 3;
inline constexpr std::uint8_t kRefuseRange = 4;          // the requested voltage, current or power is out of range
inline constexpr std::uint8_t kRefuseOther = 5;

// Bit-reversed byte (the reflected form of a CRC polynomial).
constexpr std::uint8_t reflect8(std::uint8_t v) {
    std::uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = static_cast<std::uint8_t>((r << 1) | ((v >> i) & 1));
    return r;
}

// CRC-8 with the tuner's polynomial, init and bit order (default: poly 0x29, init 0, MSB first).
constexpr std::uint8_t crc8(std::span<const std::uint8_t> b, const Tuner& t = kDefaultTuner) {
    std::uint8_t c = t.crc_init;
    const std::uint8_t rpoly = reflect8(t.crc_poly);
    for (std::uint8_t x : b) {
        c ^= x;
        for (int i = 0; i < 8; i++) {
            if (t.crc_lsb) c = static_cast<std::uint8_t>((c & 1) ? (c >> 1) ^ rpoly : (c >> 1));
            else c = static_cast<std::uint8_t>((c & 0x80) ? (c << 1) ^ t.crc_poly : (c << 1));
        }
    }
    return c;
}

// The CRC of a frame whose bytes before the CRC are `f` (sync first): over everything after the sync,
// or over the sync too when the tuner says so.
constexpr std::uint8_t frame_crc(std::span<const std::uint8_t> f, const Tuner& t) {
    return crc8(f.subspan(t.crc_sync ? 0 : 1), t);
}

// Header (8.2.2, table 13), sent big-endian: receiver address [15:13], message number [12:9], protocol
// version [8:3], message type [2:0] (0 control, 1 data). The tuner (k2/fastcharge/ufcs_tune.hpp) sets address,
// version and the type bits; its default is the standard.
constexpr std::uint16_t header(bool data, std::uint8_t id, const Tuner& t = kDefaultTuner) {
    return static_cast<std::uint16_t>(((t.addr & 7u) << 13) | ((id & 0xFu) << 9) | ((t.version & 0x3Fu) << 3) |
                                      ((data ? t.data_type : t.ctl_type) & 7u));
}

// Control frame (5 bytes) into `out`; the byte count, 0 when it does not fit.
constexpr std::size_t encode_control(std::span<std::uint8_t> out, std::uint8_t cmd, std::uint8_t id,
                                     const Tuner& t = kDefaultTuner) {
    if (out.size() < 5) return 0;
    const std::uint16_t h = header(false, id, t);
    out[0] = kSync;
    out[1] = static_cast<std::uint8_t>(h >> 8);
    out[2] = static_cast<std::uint8_t>(h);
    out[3] = cmd;
    out[4] = frame_crc(out.first(4), t);
    return 5;
}

constexpr std::size_t encode_data(std::span<std::uint8_t> out, std::uint8_t cmd, std::span<const std::uint8_t> payload,
                                  std::uint8_t id, const Tuner& t = kDefaultTuner) {
    const std::size_t n = payload.size() + 6;
    if (payload.size() > 255 || out.size() < n) return 0;
    const std::uint16_t h = header(true, id, t);
    out[0] = kSync;
    out[1] = static_cast<std::uint8_t>(h >> 8);
    out[2] = static_cast<std::uint8_t>(h);
    out[3] = cmd;
    out[4] = static_cast<std::uint8_t>(payload.size());
    for (std::size_t i = 0; i < payload.size(); i++) out[5 + i] = payload[i];
    out[n - 1] = frame_crc(out.first(n - 1), t);
    return n;
}

// Request data (8.2.4.2, table 17): output mode number [63:60], reserved [59:32], requested voltage
// [31:16] in 10 mV, requested current [15:0] in 10 mA; sent big-endian as an 8-byte payload.
constexpr std::uint64_t request_word(int pos, int mv, int ma) {
    return (static_cast<std::uint64_t>(pos & 0xF) << 60) | (static_cast<std::uint64_t>(mv / kUnitMv & 0xFFFF) << 16) |
           static_cast<std::uint64_t>(ma / kUnitMa & 0xFFFF);
}

constexpr std::size_t encode_request(std::span<std::uint8_t> out, std::uint8_t id, int pos, int mv, int ma,
                                     const Tuner& t = kDefaultTuner) {
    const std::uint64_t w = request_word(pos, mv, ma);
    std::array<std::uint8_t, 8> p{};
    // big-endian: most significant byte first
    for (int i = 0; i < 8; i++) p[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(w >> (8 * (7 - i)));
    return encode_data(out, kDataRequest, p, id, t);
}

// Refuse (8.2.4.9, table 24): the message number [27:24], type [18:16] and command [15:8] of the message
// refused, and the reason [7:0].
constexpr std::size_t encode_refuse(std::span<std::uint8_t> out, std::uint8_t id, std::uint8_t refused_id,
                                    std::uint8_t refused_type, std::uint8_t refused_cmd, std::uint8_t reason,
                                    const Tuner& t = kDefaultTuner) {
    const std::array<std::uint8_t, 4> p{static_cast<std::uint8_t>(refused_id & 0xF), static_cast<std::uint8_t>(refused_type & 7),
                                        refused_cmd, reason};
    return encode_data(out, kDataRefuse, p, id, t);
}

struct Frame {
    bool data = false;
    std::uint8_t cmd = 0, id = 0;
    std::uint8_t addr = 0, ver = 0;   // header [15:13] receiver address, [8:3] protocol version
    std::span<const std::uint8_t> payload;
};
enum class Parse : std::uint8_t { Need, Bad, Ok };
// Largest frame the receiver accepts (the fc engine's accumulator). A data frame is 6 + length
// bytes; a length byte claiming more cannot be a frame we can hold, so it is Bad at once. 64 holds the
// largest message a source sends us, Output_Capabilities with seven modes (6 + 7 * 8 = 62 bytes).
inline constexpr std::size_t kFrameMax = 64;

// Why bytes were thrown away (diagnostics only: the wire log names it). None = nothing dropped.
// Overflow is the engine's own case (accumulator full without a frame), never set by decode().
enum class Why : std::uint8_t { None, NoSync, Type, Length, Crc, Overflow };

constexpr const char* why_name(Why w) {
    switch (w) {
    case Why::NoSync: return "nosync";
    case Why::Type: return "type";
    case Why::Length: return "length";
    case Why::Crc: return "crc";
    case Why::Overflow: return "overflow";
    case Why::None: break;
    }
    return "none";
}

// Decode the first frame in `in`. `used` is the bytes to drop: the whole frame on Ok, the leading
// garbage plus the sync byte on Bad (resync), the leading garbage on Need. `why`, when given, says
// what made the bytes unusable (None on Ok and on a clean partial frame).
constexpr Parse decode(std::span<const std::uint8_t> in, Frame& f, std::size_t& used, Why* why = nullptr,
                       const Tuner& t = kDefaultTuner) {
    Why w = Why::None;
    std::size_t s = 0;
    while (s < in.size() && in[s] != kSync) s++;
    used = s;
    if (s > 0) w = Why::NoSync;
    if (why) *why = w;
    const auto b = in.subspan(s);
    if (b.size() < 5) return Parse::Need;
    const std::uint16_t h = static_cast<std::uint16_t>(b[1] << 8 | b[2]);
    const bool data = (h & 7u) == (t.data_type & 7u) && t.data_type != t.ctl_type;
    if (!data && (h & 7u) != (t.ctl_type & 7u)) { used = s + 1; if (why) *why = Why::Type; return Parse::Bad; }
    const std::size_t n = data ? static_cast<std::size_t>(b[4]) + 6 : 5;
    if (n > kFrameMax) { used = s + 1; if (why) *why = Why::Length; return Parse::Bad; }
    if (b.size() < n) return Parse::Need;
    if (frame_crc(b.first(n - 1), t) != b[n - 1]) { used = s + 1; if (why) *why = Why::Crc; return Parse::Bad; }
    if (why) *why = Why::None;
    f.data = data;
    f.cmd = b[3];
    f.id = static_cast<std::uint8_t>((h >> 9) & 0xF);
    f.addr = static_cast<std::uint8_t>(h >> 13);
    f.ver = static_cast<std::uint8_t>((h >> 3) & 0x3F);
    f.payload = data ? b.subspan(5, n - 6) : std::span<const std::uint8_t>{};
    used = s + n;
    return Parse::Ok;
}

// One Output_Capabilities output mode (8.2.4.1, table 16; 8 big-endian bytes): mode number [63:60],
// current step [59:57] (000b..100b = 10..50 mA, others reserved), voltage step [56] (0 = 10 mV,
// 1 = 20 mV), max voltage [55:40] and min voltage [39:24] in 10 mV, max current [23:8] and min current
// [7:0] in 10 mA. step_ma is 0 for a reserved step code: such a mode cannot be requested.
struct Cap {
    int min_mv = 0, max_mv = 0, min_ma = 0, max_ma = 0;
    std::uint8_t pos = 0, step_mv = 0, step_ma = 0;   // one byte each: the engine keeps seven of these in RAM
};

// Entries decoded into `out`; 0 for an empty payload or a length that is not a multiple of 8.
constexpr std::size_t decode_caps(std::span<const std::uint8_t> payload, std::span<Cap> out) {
    if (payload.empty() || payload.size() % 8 != 0) return 0;
    const std::size_t n = std::min(payload.size() / 8, out.size());
    for (std::size_t i = 0; i < n; i++) {
        std::uint64_t w = 0;
        for (std::size_t k = 0; k < 8; k++) w = w << 8 | payload[i * 8 + k];
        const int code = static_cast<int>((w >> 57) & 7);
        out[i] = {static_cast<int>((w >> 24) & 0xFFFF) * kUnitMv,
                  static_cast<int>((w >> 40) & 0xFFFF) * kUnitMv,
                  static_cast<int>(w & 0xFF) * kUnitMa,
                  static_cast<int>((w >> 8) & 0xFFFF) * kUnitMa,
                  static_cast<std::uint8_t>(w >> 60),
                  static_cast<std::uint8_t>((w >> 56) & 1 ? 20 : 10),
                  static_cast<std::uint8_t>(code <= 4 ? (code + 1) * 10 : 0)};
    }
    return n;
}

struct Pick {
    int pos = 0, mv = 0, ma = 0;
};

constexpr int mv_to_units(int mv) { return mv / kUnitMv; }
constexpr int units_to_mv(int u) { return u * kUnitMv; }
constexpr int ma_to_units(int ma) { return ma / kUnitMa; }
constexpr int units_to_ma(int u) { return u * kUnitMa; }

// A request, rounded down to a whole unit and held to [kMinMv, ceiling]; 0 when there is no ceiling.
constexpr int clamp_request_mv(int mv, int ceiling_mv) {
    if (ceiling_mv <= 0) return 0;
    if (mv > ceiling_mv) mv = ceiling_mv;
    if (mv < kMinMv) mv = kMinMv;
    return units_to_mv(mv_to_units(mv));
}

// v on the grid of multiples of `step`, rounded down, then up one step when that fell below `lo`.
constexpr int on_grid(int v, int step, int lo) {
    const int r = v / step * step;
    return r < lo ? r + step : r;
}

// The first output mode whose range holds the clamped target (a boundary belongs to the lower mode);
// false when none does. The request follows the mode's steps (8.2.4.1: multiples of the step) and asks
// `want_ma` (0 = kNoCableMa), held to the mode's own current range.
constexpr bool pick(std::span<const Cap> caps, int target_mv, int ceiling_mv, Pick& out, int want_ma = 0) {
    const int want = clamp_request_mv(target_mv, ceiling_mv);
    if (want == 0) return false;
    for (const Cap& c : caps) {
        if (c.step_ma <= 0 || c.step_mv <= 0 || c.max_ma <= 0) continue;
        if (want < c.min_mv || want > c.max_mv) continue;
        const int mv = on_grid(want, c.step_mv, c.min_mv);
        const int ma = on_grid(std::clamp(want_ma > 0 ? want_ma : kNoCableMa, c.min_ma, c.max_ma), c.step_ma, c.min_ma);
        if (mv > c.max_mv || ma > c.max_ma) continue;
        out = {c.pos, mv, ma};
        return true;
    }
    return false;
}

// What a sink answers to a request of the source it cannot serve (Get_Sink_Info, Get_Device_Info,
// Detect_Cable_Info, Verify_Request ...): a Refuse with this reason (8.2.4.9, "unsupported"), within
// tReceiverResponse. 0 = no answer owed.
constexpr std::uint8_t refuse_reason(bool data, std::uint8_t cmd) {
    if (data) return cmd == kDataVerifyRequest ? kRefuseUnsupported : 0;
    switch (cmd) {
    case kCtlGetSourceInfo:
    case kCtlGetSinkInfo:
    case kCtlGetCableInfo:
    case kCtlGetDeviceInfo:
    case kCtlGetErrorInfo:
    case kCtlDetectCableInfo:
    case kCtlStartCableDetect:
    case kCtlGetSinkInfoExt: return kRefuseUnsupported;
    default: return 0;
    }
}

// The source drops the contract (and does a hardware reset) when it hears nothing for kWatchdogMs.
class Watchdog {
public:
    void fed(std::uint32_t now_ms) { last_ = now_ms; armed_ = true; }
    bool due(std::uint32_t now_ms) const { return armed_ && now_ms - last_ >= kWatchdogMs; }

private:
    std::uint32_t last_ = 0;
    bool armed_ = false;
};

}  // namespace k2::ufcs
