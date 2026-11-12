// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Live stream of the PD black box ring over the CDC shell (`trace stream`, tools/pd_trace.py stream).
// The HID port may be busy with another tool (a charger-test run holds it for minutes and the 8 KB
// ring wraps in seconds on a chatty source); the CDC port is free, so the new ring records are pushed
// down it as they are written.
//
// Pure code over a pdtrace::Ring (no hardware, host-tested in tests/test_trace_stream.cpp). It owns
// only a read cursor and a few counters, no buffer: a frame is built in the caller's stack buffer
// straight from the ring bytes, and the ring itself is the queue. A host that falls behind therefore
// costs nothing on the device; when the ring overwrote records that were never sent, the next frame
// is an explicit Lost frame with the exact number of records and bytes, never a silent gap.
//
// Frame (text lines of the shell never contain 0xA6, a reader resyncs on it and drops a bad CRC):
//   0xA6, kind(1), len(1), payload(len), crc16 little endian of kind, len and the payload
//   (CRC-16/CCITT-FALSE: polynomial 0x1021, initial value 0xFFFF, no reflection, no final xor)
// Kinds:
//   Start(1)  flags(1: bit0 from the oldest record) cursor(4) head(4) tail(4), sent once at the start
//   Data(2)   offset(4) then whole ring records exactly as pdtrace.hpp lays them out (len, type, ts,
//             payload); offset is the ring byte offset of the first record, so frames chain: the next
//             Data offset is this offset plus this payload minus 4 unless a Lost frame came between
//   Lost(3)   records(4) bytes(4) flags(1: bit0 the cursor was beyond the ring head, a restarted ring)
// The shell's `stream on` sends these three unchanged inside the multiplexed debug stream, next to the kinds
// 4..10 (k2/core/dbg_stream.hpp); `trace stream` is the same stream with only the trace kind selected.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/pd/pdtrace.hpp"

namespace k2::trace_stream {

inline constexpr std::uint8_t kSync = 0xA6;
// Start/Data/Lost belong to this streamer; Hello..Boot are the other kinds of the multiplexed debug stream, which sends
// this streamer's frames unchanged next to them (k2/core/dbg_stream.hpp lays the payloads out).
enum class Kind : std::uint8_t { Start = 1, Data = 2, Lost = 3, Hello = 4, Text = 5, Log = 6, Power = 7, Run = 8, Gap = 9, Beat = 10, Boot = 11 };

inline constexpr std::size_t kChunk = 2 * pdtrace::kMaxRecord;   // ring bytes per Data frame: two largest records
inline constexpr std::size_t kFrameMax = 3 + 4 + kChunk + 2;     // 91
inline constexpr unsigned kMaxFramesPerPass = 4;                 // bounds the work of one main-loop pass

namespace detail {
constexpr std::array<std::uint16_t, 16> make_nibble_table() {
    std::array<std::uint16_t, 16> t{};
    for (unsigned i = 0; i < 16; i++) {
        auto c = static_cast<std::uint16_t>(i << 12);
        for (int b = 0; b < 4; b++) c = static_cast<std::uint16_t>((c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1);
        t[i] = c;
    }
    return t;
}
inline constexpr auto kNibble = make_nibble_table();   // 32 bytes of flash, two lookups per byte
}  // namespace detail

inline std::uint16_t crc16(std::span<const std::uint8_t> d) {
    std::uint16_t c = 0xFFFF;
    for (std::uint8_t b : d) {
        c = static_cast<std::uint16_t>((c << 4) ^ detail::kNibble[(c >> 12) ^ (b >> 4)]);
        c = static_cast<std::uint16_t>((c << 4) ^ detail::kNibble[(c >> 12) ^ (b & 15)]);
    }
    return c;
}

inline void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
inline void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// Frames `len` payload bytes already placed at out[3..]: sync, kind, len in front, the CRC behind. Returns the frame length.
inline std::size_t seal_frame(std::span<std::uint8_t> out, Kind k, std::size_t len) {
    out[0] = kSync;
    out[1] = static_cast<std::uint8_t>(k);
    out[2] = static_cast<std::uint8_t>(len);
    const std::uint16_t c = crc16(out.subspan(1, 2 + len));
    out[3 + len] = static_cast<std::uint8_t>(c);
    out[4 + len] = static_cast<std::uint8_t>(c >> 8);
    return 5 + len;
}

class Streamer {
public:
    struct Stats { std::uint32_t records = 0, lost = 0; };   // sent / reported lost since start()

    // Begin at the current head (only records written from now on) or, with `all`, at the oldest record.
    template <std::size_t N>
    void start(const pdtrace::Ring<N>& r, bool all) {
        running_ = true;
        announced_ = false;
        all_ = all;
        cursor_ = all ? r.tail() : r.head();
        seq_ = all ? r.records_dropped() : r.records_pushed();
        stats_ = {};
    }
    void stop() { running_ = false; }
    bool running() const { return running_; }
    const Stats& stats() const { return stats_; }
    // Something to send: the Start frame, a Lost report or unsent records (the multiplexer's stall count).
    template <std::size_t N>
    bool pending(const pdtrace::Ring<N>& r) const {
        return running_ && (!announced_ || cursor_ != r.head());
    }

    struct Body { Kind kind = Kind::Start; std::size_t len = 0; };   // len 0 = nothing to send

    // The next frame body (kind and payload) into `pl`, which holds at least max(13, 4 + chunk) bytes; `chunk` caps
    // the ring bytes of a Data body and must be at least pdtrace::kMaxRecord. The CDC framing (next) and the HID
    // report packing (k2/core/trace_hid.hpp) both wrap these bodies, so the loss semantics are one piece of code.
    template <std::size_t N>
    Body next_body(const pdtrace::Ring<N>& r, std::uint8_t* pl, std::size_t chunk) {
        if (!running_) return {};
        if (!announced_) {
            announced_ = true;
            pl[0] = all_ ? 1 : 0;
            put32(pl + 1, cursor_);
            put32(pl + 5, r.head());
            put32(pl + 9, r.tail());
            return {Kind::Start, 13};
        }
        const bool beyond = cursor_ > r.head();   // a ring that restarted under the cursor
        if (beyond || cursor_ < r.tail()) {
            const std::uint32_t rec = beyond ? 0 : r.records_dropped() - seq_;
            const std::uint32_t bytes = beyond ? 0 : r.tail() - cursor_;
            cursor_ = r.tail();
            seq_ = r.records_dropped();
            stats_.lost += rec;
            put32(pl, rec);
            put32(pl + 4, bytes);
            pl[8] = beyond ? 1 : 0;
            return {Kind::Lost, 9};
        }
        if (cursor_ == r.head()) return {};
        const auto rd = r.read(cursor_, std::span<std::uint8_t>(pl + 4, chunk));
        if (rd.bytes == 0) return {};
        put32(pl, cursor_);
        for (std::size_t i = 0; i < rd.bytes; i += pl[4 + i]) { seq_++; stats_.records++; }
        cursor_ = rd.next;
        return {Kind::Data, 4 + rd.bytes};
    }

    // The next CDC frame into `out` (at least kFrameMax bytes); its length, 0 when there is nothing to send.
    template <std::size_t N>
    std::size_t next(const pdtrace::Ring<N>& r, std::span<std::uint8_t> out) {
        if (!running_ || out.size() < kFrameMax) return 0;
        const Body b = next_body(r, out.data() + 3, kChunk);
        return b.len == 0 ? 0 : seal(out, b.kind, b.len);
    }

    // One main-loop pass: hands `write` whole frames while `room` (free bytes in the CDC TX FIFO) holds
    // a worst-case frame, at most kMaxFramesPerPass of them. Returns the frames sent.
    template <std::size_t N, class Write>
    unsigned service(const pdtrace::Ring<N>& r, std::size_t room, Write&& write) {
        std::array<std::uint8_t, kFrameMax> f;
        unsigned n = 0;
        while (n < kMaxFramesPerPass && room >= kFrameMax) {
            const std::size_t len = next(r, f);
            if (len == 0) break;
            write(std::span<const std::uint8_t>(f.data(), len));
            room -= len;
            n++;
        }
        return n;
    }

private:
    static std::size_t seal(std::span<std::uint8_t> out, Kind k, std::size_t len) { return seal_frame(out, k, len); }

    std::uint32_t cursor_ = 0;   // ring byte offset of the next record to send
    std::uint32_t seq_ = 0;      // number of that record (Ring::records_dropped() counts the tail's)
    Stats stats_;
    bool running_ = false, announced_ = false, all_ = false;
};

}  // namespace k2::trace_stream
