// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// D+/D- edge capture, pure logic (host-tested). The driver (dpsniff) timestamps each edge with
// the DWT cycle counter and pushes one 32-bit word per edge; this header packs the words, holds
// the fixed buffer, decides when a capture ends and turns the words into Black Box records.
//
// Event word   (cycles & ~63) | dm << 1 | dp      the lines' state AFTER the edge; 64 cycles
//                                                  = 0.267 us at 240 MHz, so 26 bits = 17.9 s.
// Record type  pdtrace::Type::Edges, payload:
//                  idx(1) first_units(4 LE) then per event varint((delta_units << 2) | state)
//              delta_units is measured from the previous event of the same record (0 for the first),
//              modulo 2^26. Every record restarts from an absolute time, so a lost record costs only
//              its own edges. A record is at most kChunkBytes of payload.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace k2::edges {

inline constexpr std::uint32_t kCyclesPerUnit = 64;
inline constexpr std::uint32_t kUnitMask = (1u << 26) - 1;
// A record (6 header + payload) must fit one remote capture read (58-byte response - 9 header = 49), or the
// reader cannot step past it: with 200 the host saw 0 edges of a stored 4096-edge capture.
inline constexpr std::size_t kChunkBytes = 43;
inline constexpr std::uint32_t kIdleMs = 500;      // no edge for this long after the first one ends the capture
inline constexpr std::uint32_t kTimeoutMs = 6000;  // hard cap from the first edge
inline constexpr std::int32_t kVbusSeenMv = 4500;  // the detach rule applies only after VBUS was this high since arming
inline constexpr std::int32_t kVbusGoneMv = 3000;

constexpr std::uint32_t pack(std::uint32_t cycles, bool dm, bool dp) {
    return (cycles & ~(kCyclesPerUnit - 1)) | (dm ? 2u : 0u) | (dp ? 1u : 0u);
}
constexpr std::uint32_t units(std::uint32_t word) { return word >> 6; }
constexpr std::uint8_t state(std::uint32_t word) { return std::uint8_t(word & 3u); }
constexpr bool dp(std::uint32_t word) { return word & 1u; }
constexpr bool dm(std::uint32_t word) { return word & 2u; }

// Ordering: buf_ is plain memory, the indices are volatile. Volatile orders only volatile accesses, so
// under LTO the compiler may sink the buf_ store past the index publish (or hoist the consumer's buf_
// load above its index read). Single core, ISR vs main loop: a compiler barrier is enough (no DMB), so
// the producer fences (release) between the data store and the index store, the consumer (acquire)
// between the index load and the data loads.
inline void publish_fence() { std::atomic_signal_fence(std::memory_order_release); }
inline void consume_fence() { std::atomic_signal_fence(std::memory_order_acquire); }

// Fixed span, one producer (the EXTI ISR), one consumer (the main loop). Push never blocks; when
// the span is full the flag is set and the edge is dropped.
class Recorder {
public:
    void init(std::uint32_t* buf, std::size_t cap) { buf_ = buf; cap_ = cap; n_ = 0; over_ = false; dropped_ = 0; }
    void reset() { n_ = 0; over_ = false; dropped_ = 0; }
    bool push(std::uint32_t word) {
        if (n_ >= cap_) { over_ = true; dropped_ = dropped_ + 1; return false; }
        buf_[n_] = word;
        publish_fence();
        n_ = n_ + 1;
        return true;
    }
    std::size_t size() const { return n_; }
    std::size_t capacity() const { return cap_; }
    bool full() const { return n_ >= cap_; }
    bool overflowed() const { return over_; }
    std::uint32_t dropped() const { return dropped_; }   // edges that arrived after the buffer was full (the first cap are kept)
    std::uint32_t total() const { return std::uint32_t(n_) + dropped_; }
    std::uint32_t at(std::size_t i) const { consume_fence(); return buf_[i]; }   // i < a size() read before
    std::span<const std::uint32_t> words() const { const std::size_t n = n_; consume_fence(); return {buf_, n}; }
private:
    std::uint32_t* buf_ = nullptr;
    volatile std::size_t n_ = 0;
    std::size_t cap_ = 0;
    volatile bool over_ = false;
    volatile std::uint32_t dropped_ = 0;
};

// Live stream: the ISR pushes words into a ring, the main loop pops them, encodes and sends. A push into
// a full ring drops that edge and counts an overrun (never blocks, never overwrites unsent edges).
class StreamRing {
public:
    void init(std::uint32_t* buf, std::size_t cap) { buf_ = buf; cap_ = cap; head_ = tail_ = 0; over_ = 0; pushed_ = 0; }
    void push(std::uint32_t w) {   // producer: the EXTI ISR (or the synthetic source)
        if (head_ - tail_ >= cap_) { over_ = over_ + 1; return; }
        buf_[head_ % cap_] = w;
        publish_fence();
        head_ = head_ + 1;
        pushed_ = pushed_ + 1;
    }
    // consumer: copy up to out.size() words WITHOUT consuming them, then drop() what was used
    std::size_t peek(std::span<std::uint32_t> out) const {
        std::size_t n = 0;
        const std::uint32_t head = head_;
        consume_fence();
        for (std::uint32_t t = tail_; n < out.size() && t != head; t = t + 1) out[n++] = buf_[t % cap_];
        return n;
    }
    void drop(std::size_t n) { publish_fence(); tail_ = tail_ + std::uint32_t(n); }   // reads done before the slot is freed
    std::size_t pending() const { return head_ - tail_; }
    std::uint32_t overruns() const { return over_; }
    std::uint32_t pushed() const { return pushed_; }
private:
    std::uint32_t* buf_ = nullptr;
    std::size_t cap_ = 0;
    volatile std::uint32_t head_ = 0, tail_ = 0, over_ = 0, pushed_ = 0;
};

// Stream frame on the CDC port: kSync, len (payload bytes), payload = one encode_chunk() record
// (idx, first_units, varint edges), sum (8-bit sum of len and the payload). A reader resyncs on kSync
// and drops a frame whose sum fails; text lines of the shell between frames never contain 0xA5.
inline constexpr std::uint8_t kSync = 0xA5;
inline constexpr std::size_t kFrameMax = 3 + kChunkBytes;
inline std::size_t frame(std::span<const std::uint8_t> payload, std::span<std::uint8_t> out) {
    if (payload.size() > kChunkBytes || out.size() < payload.size() + 3) return 0;
    out[0] = kSync;
    out[1] = std::uint8_t(payload.size());
    std::uint8_t sum = out[1];
    for (std::size_t i = 0; i < payload.size(); i++) { out[2 + i] = payload[i]; sum = std::uint8_t(sum + payload[i]); }
    out[2 + payload.size()] = sum;
    return payload.size() + 3;
}

// Storm = the EXTI edge-storm guard tripped (k2/fastcharge/edge_guard.hpp: data traffic on D+/D-).
enum class End : std::uint8_t { None = 0, Full, Idle, Detach, Timeout, Cancel, Storm };

// Arm -> wait for the first edge -> record -> end. Polled from the main loop with the edge count.
class Session {
public:
    void arm(std::uint32_t now_ms) { armed_ = true; started_ = false; vbus_seen_ = false; end_ = End::None; arm_ms_ = now_ms; }
    bool armed() const { return armed_; }
    bool started() const { return started_; }
    End reason() const { return end_; }
    // `count` edges recorded so far, `full` = the buffer is full, `vbus_mv` = the bus voltage now.
    // A detach (VBUS below 3 V) only counts once edges are recording and VBUS was seen at 4.5 V or more
    // since arming, so arming with nothing plugged in (or before unplugging the phone) waits for the
    // handshake instead of ending at once.
    // Returns the end reason once, End::None while the session continues.
    End poll(std::uint32_t now_ms, std::size_t count, bool full, std::int32_t vbus_mv) {
        if (!armed_) return End::None;
        if (vbus_mv >= kVbusSeenMv) vbus_seen_ = true;
        const bool detach = started_ && vbus_seen_ && vbus_mv < kVbusGoneMv;
        if (count != 0 && !started_) { started_ = true; first_ms_ = now_ms; last_n_ = count; last_ms_ = now_ms; }
        if (started_ && count != last_n_) { last_n_ = count; last_ms_ = now_ms; }
        if (full) return finish(End::Full);
        if (detach) return finish(End::Detach);
        if (started_ && now_ms - last_ms_ >= kIdleMs) return finish(End::Idle);
        if (started_ && now_ms - first_ms_ >= kTimeoutMs) return finish(End::Timeout);
        return End::None;
    }
    End cancel() { return armed_ ? finish(End::Cancel) : End::None; }
private:
    End finish(End e) { armed_ = false; end_ = e; return e; }
    bool armed_ = false, started_ = false, vbus_seen_ = false;
    End end_ = End::None;
    std::uint32_t arm_ms_ = 0, first_ms_ = 0, last_ms_ = 0;
    std::size_t last_n_ = 0;
};

// Systick microseconds of a word, from a (cycles, micros) pair taken together at arm time.
constexpr std::uint32_t word_us(std::uint32_t word, std::uint32_t arm_cycles, std::uint32_t arm_us,
                                std::uint32_t cycles_per_us = 240) {
    const std::uint32_t dc = (word & ~(kCyclesPerUnit - 1)) - arm_cycles;   // modulo 2^32
    return arm_us + dc / cycles_per_us;
}

// Encode words[from..] into one record payload. Returns the number of words consumed; `out_len`
// gets the payload length. At least one word is consumed when any remain.
inline std::size_t encode_chunk(std::span<const std::uint32_t> words, std::size_t from, std::uint8_t idx,
                                std::span<std::uint8_t> out, std::size_t& out_len) {
    out_len = 0;
    if (from >= words.size() || out.size() < 5 + 5) return 0;
    const std::size_t limit = out.size() < kChunkBytes ? out.size() : kChunkBytes;
    const std::uint32_t first = units(words[from]);
    out[0] = idx;
    for (int i = 0; i < 4; i++) out[1 + i] = std::uint8_t(first >> (8 * i));
    std::size_t n = 5, used = 0;
    std::uint32_t prev = first;
    for (std::size_t i = from; i < words.size(); i++) {
        const std::uint32_t d = (units(words[i]) - prev) & kUnitMask;
        std::uint64_t v = (std::uint64_t(d) << 2) | state(words[i]);
        std::size_t need = 1;
        for (std::uint64_t t = v >> 7; t; t >>= 7) need++;
        if (n + need > limit) break;
        while (v >= 0x80) { out[n++] = std::uint8_t(v | 0x80); v >>= 7; }
        out[n++] = std::uint8_t(v);
        prev = units(words[i]);
        used++;
    }
    out_len = n;
    return used;
}

// One Edges record (header + payload, same layout as in a Black Box slot) for a remote read of the
// whole buffer: words[from..] encoded to fit `out`. Returns the record length (0 = nothing left);
// `used` gets the words consumed.
inline std::size_t read_record(std::span<const std::uint32_t> words, std::size_t from, std::uint8_t type,
                               std::uint32_t ts_us, std::span<std::uint8_t> out, std::size_t& used) {
    used = 0;
    if (from >= words.size() || out.size() < 6 + 10) return 0;
    std::size_t len = 0;
    used = encode_chunk(words, from, 0, out.subspan(6), len);
    if (used == 0) return 0;
    out[0] = std::uint8_t(6 + len);
    out[1] = type;
    for (int i = 0; i < 4; i++) out[2 + i] = std::uint8_t(ts_us >> (8 * i));
    return 6 + len;
}

struct Edge { std::uint32_t units; std::uint8_t state; };

// Decode one payload into `out`; returns the count (0 on a malformed payload). first_units is
// returned through the reference so a reader can unwrap across records.
inline std::size_t decode_chunk(std::span<const std::uint8_t> p, std::span<Edge> out, std::uint8_t& idx) {
    if (p.size() < 5) return 0;
    idx = p[0];
    std::uint32_t t = p[1] | (std::uint32_t(p[2]) << 8) | (std::uint32_t(p[3]) << 16) | (std::uint32_t(p[4]) << 24);
    std::size_t n = 0, i = 5;
    bool first = true;
    while (i < p.size() && n < out.size()) {
        std::uint64_t v = 0;
        int sh = 0;
        for (;;) {
            if (i >= p.size() || sh > 35) return 0;
            const std::uint8_t b = p[i++];
            v |= std::uint64_t(b & 0x7f) << sh;
            sh += 7;
            if (!(b & 0x80)) break;
        }
        if (!first) t = (t + std::uint32_t(v >> 2)) & kUnitMask;
        first = false;
        out[n++] = {t, std::uint8_t(v & 3)};
    }
    return n;
}

}  // namespace k2::edges
