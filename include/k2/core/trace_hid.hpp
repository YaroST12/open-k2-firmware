// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Push mode of the PD black box ring over the HID port (remote command 0x73, tools/pd_trace.py hid-stream).
// The CDC stream (trace_stream.hpp) needs the shell port; the HID port is the one every K2 tool already has, and
// while a trace is being taken nothing else needs it. In this mode the K2 pushes the new ring records on its own,
// as many whole records as fit a 64 byte HID IN report, and the K2LOG text reports are held back (usb::set_hid_quiet)
// so the 1 kHz interrupt pipe carries trace data only. Command responses (0xA6) still pass, so the host can keep the
// session alive and stop it with the ordinary request/response protocol.
//
// Pure code over a pdtrace::Ring (no hardware, host-tested in tests/test_trace_hid.cpp). The record queue is the ring
// itself, exactly as in the CDC stream: the Streamer owns a cursor and reports a ring wrap as an explicit Lost body.
//
// Report (every field one byte; text log reports never start with a byte >= 0x80 and responses start with 0xA6):
//   0xA7, kind, seq, len, payload(len), zero padding to 64 bytes
//   seq counts every report of the session from 0 and wraps at 256, so a host sees a report dropped between the
//   device and the host (the device pacing makes that a bug, but the number makes it visible, not silent).
// Kinds 1..3 are trace_stream.hpp's Start / Data / Lost with the same payloads (Data carries the ring offset and up
// to kChunk bytes of whole records, so a Data payload is at most 60 bytes); kind 0x10:
//   End(0x10) reason(1): 1 = the keepalive timeout ended the session (the stop command ends it silently)
// Session rule: start() arms a deadline of `timeout` ms; every keepalive() moves it; when it passes, next() sends the
// End report once and the session is off. A host that dies, or a cable that is pulled, therefore returns the HID port
// to normal on its own.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/core/trace_stream.hpp"
#include "k2/pd/pdtrace.hpp"

namespace k2::trace_hid {

inline constexpr std::uint8_t kSync = 0xA7;
inline constexpr std::size_t kReportBytes = 64;
inline constexpr std::size_t kHeader = 4;                         // sync, kind, seq, len
inline constexpr std::size_t kPayloadMax = kReportBytes - kHeader; // 60
inline constexpr std::size_t kChunk = kPayloadMax - 4;             // ring bytes per Data report (after the 4 byte offset)
static_assert(kChunk >= pdtrace::kMaxRecord, "one largest record must fit a report");

inline constexpr std::uint8_t kKindEnd = 0x10;
inline constexpr std::uint8_t kEndTimeout = 1;

inline constexpr std::uint32_t kTimeoutDefaultMs = 3000, kTimeoutMinMs = 250, kTimeoutMaxMs = 60000;

// The keepalive timeout a host asks for: 0 = the default, otherwise clamped to [kTimeoutMinMs, kTimeoutMaxMs].
constexpr std::uint32_t clamp_timeout_ms(std::uint32_t ms) {
    if (ms == 0) return kTimeoutDefaultMs;
    if (ms < kTimeoutMinMs) return kTimeoutMinMs;
    return ms > kTimeoutMaxMs ? kTimeoutMaxMs : ms;
}

class Session {
public:
    struct Stats { std::uint32_t reports = 0, records = 0, lost = 0; };   // since start()

    // Begin at the current head (only records written from now on) or, with `all`, at the oldest record.
    template <std::size_t N>
    void start(const pdtrace::Ring<N>& r, bool all, std::uint32_t now_ms, std::uint32_t timeout_ms) {
        streamer_.start(r, all);
        timeout_ = clamp_timeout_ms(timeout_ms);
        last_ = now_ms;
        seq_ = 0;
        reports_ = 0;
        active_ = true;
    }
    void keepalive(std::uint32_t now_ms) { last_ = now_ms; }
    void stop() {
        active_ = false;
        streamer_.stop();
    }
    bool active() const { return active_; }
    // The keepalive deadline has passed. next() sends the End report for it; a caller whose USB queue is full (the host
    // stopped reading) calls stop() on this instead, so the port is not held quiet for ever.
    bool expired(std::uint32_t now_ms) const { return active_ && static_cast<std::uint32_t>(now_ms - last_) > timeout_; }
    Stats stats() const { return {reports_, streamer_.stats().records, streamer_.stats().lost}; }

    // Something to say (the Start report, a Lost report or unsent records), for a caller that wants to skip the pass.
    template <std::size_t N>
    bool pending(const pdtrace::Ring<N>& r) const { return active_ && streamer_.pending(r); }

    // The next report into `out`; false when there is nothing to send (idle, or off). After the deadline the End
    // report is returned once and the session turns off.
    template <std::size_t N>
    bool next(const pdtrace::Ring<N>& r, std::uint32_t now_ms, std::span<std::uint8_t, kReportBytes> out) {
        if (!active_) return false;
        std::uint8_t* pl = out.data() + kHeader;
        trace_stream::Streamer::Body b;
        if (expired(now_ms)) {
            pl[0] = kEndTimeout;
            b = {static_cast<trace_stream::Kind>(kKindEnd), 1};
            stop();
        } else {
            b = streamer_.next_body(r, pl, kChunk);
            if (b.len == 0) return false;
        }
        out[0] = kSync;
        out[1] = static_cast<std::uint8_t>(b.kind);
        out[2] = seq_++;
        out[3] = static_cast<std::uint8_t>(b.len);
        for (std::size_t i = kHeader + b.len; i < kReportBytes; i++) out[i] = 0;
        reports_++;
        return true;
    }

private:
    trace_stream::Streamer streamer_;
    std::uint32_t timeout_ = kTimeoutDefaultMs;
    std::uint32_t last_ = 0;
    std::uint32_t reports_ = 0;
    std::uint8_t seq_ = 0;
    bool active_ = false;
};

}  // namespace k2::trace_hid
