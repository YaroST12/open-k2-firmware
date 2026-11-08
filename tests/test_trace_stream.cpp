// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD black box stream over the CDC shell (k2/core/trace_stream.hpp): framing round trip, the explicit
// lost-records frame, a burst larger than the CDC FIFO, and the per-pass cost bound. The reader here is
// written independently of the encoder (own CRC, own parser) so a shared mistake cannot hide.
#include <chrono>
#include <cstdio>
#include <vector>

#include "k2/pd/pdtrace.hpp"
#include "k2/core/trace_stream.hpp"
#include "test_util.h"

using namespace k2::pdtrace;
namespace ts = k2::trace_stream;

namespace {

using Bytes = std::vector<std::uint8_t>;

// CRC-16/CCITT-FALSE, bitwise (poly 0x1021, init 0xFFFF): the reference, not the firmware's nibble table.
std::uint16_t crc_ref(const std::uint8_t* p, std::size_t n) {
    std::uint16_t c = 0xFFFF;
    for (std::size_t i = 0; i < n; i++) {
        c ^= std::uint16_t(p[i] << 8);
        for (int b = 0; b < 8; b++) c = std::uint16_t(c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1);
    }
    return c;
}

struct Frame { std::uint8_t kind; Bytes payload; };

// Frames in a byte stream (text between them is skipped); `bad` counts CRC failures.
std::vector<Frame> parse(const Bytes& s, int* bad = nullptr) {
    std::vector<Frame> out;
    std::size_t i = 0;
    while (i < s.size()) {
        if (s[i] != 0xA6) { i++; continue; }
        if (i + 3 > s.size()) break;
        const std::size_t n = s[i + 2];
        if (i + 3 + n + 2 > s.size()) break;
        const std::uint16_t want = std::uint16_t(s[i + 3 + n] | s[i + 4 + n] << 8);
        if (crc_ref(&s[i + 1], 2 + n) == want) {
            out.push_back({s[i + 1], Bytes(s.begin() + i + 3, s.begin() + i + 3 + n)});
            i += 5 + n;
        } else {
            if (bad) (*bad)++;
            i++;
        }
    }
    return out;
}

std::uint32_t u32(const Bytes& b, std::size_t at) {
    return b[at] | b[at + 1] << 8 | b[at + 2] << 16 | std::uint32_t(b[at + 3]) << 24;
}

using R = Ring<600>;

// A record of `size` bytes whose first payload byte is `tag` (the record's number).
void push_rec(R& r, std::uint8_t tag, std::size_t size = 10) {
    Bytes p(size - kHeader, tag);
    r.push(Type::Mark, tag, p);
}

// One main-loop pass with `room` free FIFO bytes: the bytes a host would see.
template <class Ring>
Bytes pass(ts::Streamer& s, const Ring& r, std::size_t room) {
    Bytes out;
    s.service(r, room, [&](std::span<const std::uint8_t> f) { out.insert(out.end(), f.begin(), f.end()); });
    return out;
}

// Passes with an empty FIFO until the streamer has nothing left to say.
template <class Ring>
Bytes drain(ts::Streamer& s, const Ring& r) {
    Bytes all;
    for (Bytes b; !(b = pass(s, r, 100000)).empty();) all.insert(all.end(), b.begin(), b.end());
    return all;
}

// Record tags (first payload byte) in the Data frames of `frames`.
std::vector<int> data_tags(const std::vector<Frame>& frames) {
    std::vector<int> t;
    for (const auto& f : frames) {
        if (f.kind != 2) continue;
        for (std::size_t i = 4; i < f.payload.size(); i += f.payload[i]) t.push_back(f.payload[i + kHeader]);
    }
    return t;
}

}  // namespace

int main() {
    {   // the CRC is CRC-16/CCITT-FALSE: check value of "123456789"
        const std::uint8_t d[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        CHECK_EQ(ts::crc16(d), 0x29B1);
    }
    {   // round trip from the oldest record: Start, then the records of the ring byte for byte
        R r;
        for (std::uint8_t i = 1; i <= 5; i++) push_rec(r, i);
        ts::Streamer s;
        CHECK_EQ(s.running(), false);
        s.start(r, true);
        CHECK_EQ(s.running(), true);
        int bad = 0;
        const auto frames = parse(drain(s, r), &bad);
        CHECK_EQ(bad, 0);
        CHECK_EQ(frames.size(), 2u);
        CHECK_EQ(frames[0].kind, 1);                        // Start: flags, cursor, head, tail
        CHECK_EQ(frames[0].payload.size(), 13u);
        CHECK_EQ(frames[0].payload[0], 1);                  // from the oldest
        CHECK_EQ(u32(frames[0].payload, 1), 0u);
        CHECK_EQ(u32(frames[0].payload, 5), 50u);
        CHECK_EQ(frames[1].kind, 2);                        // Data: ring offset, then whole records
        CHECK_EQ(u32(frames[1].payload, 0), 0u);
        CHECK_EQ(frames[1].payload.size(), 4u + 50u);
        std::uint8_t ring_bytes[64];
        r.read(0, ring_bytes);
        CHECK_EQ(memcmp(&frames[1].payload[4], ring_bytes, 50), 0);
        CHECK_EQ(drain(s, r).size(), 0u);                   // nothing new, nothing sent
        push_rec(r, 6);                                     // a new record follows at offset 50
        const auto more = parse(drain(s, r));
        CHECK_EQ(more.size(), 1u);
        CHECK_EQ(u32(more[0].payload, 0), 50u);
        CHECK_EQ(data_tags(more)[0], 6);
        CHECK_EQ(s.stats().records, 6u);
        CHECK_EQ(s.stats().lost, 0u);
    }
    {   // default start is the current head: the old records are not sent
        R r;
        for (std::uint8_t i = 1; i <= 5; i++) push_rec(r, i);
        ts::Streamer s;
        s.start(r, false);
        auto first = parse(drain(s, r));
        CHECK_EQ(first.size(), 1u);
        CHECK_EQ(first[0].payload[0], 0);
        push_rec(r, 6);
        push_rec(r, 7);
        const auto tags = data_tags(parse(drain(s, r)));
        CHECK_EQ(tags.size(), 2u);
        CHECK_EQ(tags[0], 6);
        CHECK_EQ(tags[1], 7);
    }
    {   // the ring wraps over unsent records: an explicit Lost frame with the exact count, then the tail
        R r;                                                // 600 B ring, 10 B records: 60 fit
        ts::Streamer s;
        s.start(r, false);
        parse(drain(s, r));                                 // consume the Start frame
        for (int i = 1; i <= 100; i++) push_rec(r, std::uint8_t(i));
        int bad = 0;
        const auto frames = parse(drain(s, r), &bad);
        CHECK_EQ(bad, 0);
        CHECK_EQ(frames[0].kind, 3);                        // Lost comes before any data
        CHECK_EQ(u32(frames[0].payload, 0), 40u);           // records 1..40 were overwritten
        CHECK_EQ(u32(frames[0].payload, 4), 400u);          // bytes
        CHECK_EQ(frames[0].payload[8], 0);                  // flags: not a ring restart
        const auto tags = data_tags(frames);
        CHECK_EQ(tags.size(), 60u);
        CHECK_EQ(tags.front(), 41);
        CHECK_EQ(tags.back(), 100);
        CHECK_EQ(s.stats().lost, 40u);
        CHECK_EQ(s.stats().records, 60u);
        CHECK_EQ(drain(s, r).size(), 0u);                   // and no second Lost
    }
    {   // a partly read ring that then wraps: lost counts only the unsent ones
        R r;
        for (int i = 1; i <= 20; i++) push_rec(r, std::uint8_t(i));
        ts::Streamer s;
        s.start(r, true);
        parse(drain(s, r));                                 // all 20 sent
        for (int i = 21; i <= 150; i++) push_rec(r, std::uint8_t(i));   // 60 stay: 91..150
        const auto frames = parse(drain(s, r));
        CHECK_EQ(frames[0].kind, 3);
        CHECK_EQ(u32(frames[0].payload, 0), 70u);           // 21..90
        CHECK_EQ(data_tags(frames).front(), 91);
    }
    {   // a cleared ring (remote `clear`) drops the unsent records: reported as lost too
        R r;
        for (int i = 1; i <= 5; i++) push_rec(r, std::uint8_t(i));
        ts::Streamer s;
        s.start(r, true);
        parse(pass(s, r, ts::kFrameMax));                   // room for one frame: the Start frame only
        r.clear();
        push_rec(r, 9);
        const auto frames = parse(drain(s, r));
        CHECK_EQ(frames[0].kind, 3);
        CHECK_EQ(u32(frames[0].payload, 0), 5u);
        CHECK_EQ(data_tags(frames).size(), 1u);
        CHECK_EQ(data_tags(frames)[0], 9);
    }
    {   // a cursor beyond the head (a new ring after a restart): flagged, restarts at the tail
        R a, b;
        for (int i = 1; i <= 30; i++) push_rec(a, std::uint8_t(i));
        ts::Streamer s;
        s.start(a, false);
        parse(drain(s, a));
        push_rec(b, 77);
        const auto frames = parse(drain(s, b));
        CHECK_EQ(frames[0].kind, 3);
        CHECK_EQ(frames[0].payload[8], 1);
        CHECK_EQ(data_tags(frames)[0], 77);
    }
    {   // a burst bigger than the CDC FIFO: frames go out only while whole frames fit; the rest waits a pass
        Ring<8192> r;
        ts::Streamer s;
        s.start(r, false);
        parse(drain(s, r));
        for (int i = 0; i < 150; i++) {
            Bytes p(35, std::uint8_t(i));                   // 41-byte records, 6150 B in total
            r.push(Type::Rx, i, p);
        }
        Bytes wire;
        const std::size_t fifo = 256;
        std::size_t used = 0, passes = 0, max_frames = 0;
        std::vector<int> seen;
        while (true) {
            std::size_t frames_now = 0;
            const Bytes before = wire;
            s.service(r, fifo - used, [&](std::span<const std::uint8_t> f) {
                CHECK_EQ(f.size() <= ts::kFrameMax, true);
                wire.insert(wire.end(), f.begin(), f.end());
                used += f.size();
                frames_now++;
            });
            if (frames_now > max_frames) max_frames = frames_now;
            CHECK_EQ(used <= fifo, true);                   // never more than the FIFO holds
            used = used > 16 ? used - 16 : 0;               // the host drains one 16-byte packet per pass
            if (frames_now == 0 && used == 0) break;
            if (++passes > 100000) break;
        }
        int bad = 0;
        const auto frames = parse(wire, &bad);
        CHECK_EQ(bad, 0);
        const auto tags = data_tags(frames);
        CHECK_EQ(tags.size(), 150u);
        for (int i = 0; i < 150 && i < int(tags.size()); i++) CHECK_EQ(tags[i], i);
        for (const auto& f : frames) CHECK_EQ(f.kind, 2);   // no Lost frame: the ring never wrapped
        CHECK_EQ(max_frames <= ts::kMaxFramesPerPass, true);
        CHECK_EQ(max_frames >= 1, true);
        CHECK_EQ(s.stats().records, 150u);
        // offsets are contiguous: each frame starts where the previous ended
        std::uint32_t expect = 0;
        for (const auto& f : frames) {
            CHECK_EQ(u32(f.payload, 0), expect);
            expect += std::uint32_t(f.payload.size() - 4);
        }
        CHECK_EQ(expect, 6150u);
    }
    {   // stop ends it; a restart begins from the new position
        R r;
        ts::Streamer s;
        s.start(r, false);
        s.stop();
        CHECK_EQ(s.running(), false);
        push_rec(r, 1);
        CHECK_EQ(drain(s, r).size(), 0u);
        s.start(r, false);
        parse(drain(s, r));
        push_rec(r, 2);
        CHECK_EQ(data_tags(parse(drain(s, r)))[0], 2);
    }
    {   // cost bound: one pass sends at most kMaxFramesPerPass frames of at most kFrameMax bytes
        // and copies at most kChunk ring bytes per frame, whatever the backlog is.
        Ring<8192> r;
        for (int i = 0; i < 190; i++) {
            Bytes p(35, std::uint8_t(i));
            r.push(Type::Rx, i, p);
        }
        ts::Streamer s;
        s.start(r, true);
        std::size_t frames = 0, bytes = 0;
        s.service(r, 100000, [&](std::span<const std::uint8_t> f) { frames++; bytes += f.size(); });
        CHECK_EQ(frames, std::size_t(ts::kMaxFramesPerPass));
        CHECK_EQ(bytes <= ts::kMaxFramesPerPass * ts::kFrameMax, true);
        // time the busiest case (full 82-byte chunks, ring never empty) on the host: an upper bound
        // for the work, the Cortex-M4 does the same instructions
        Ring<8192> big;
        const int kPasses = 20000;
        ts::Streamer t;
        t.start(big, false);
        double worst_ns = 0, total_ns = 0;
        for (int k = 0; k < kPasses; k++) {
            // 8 records of 41 B = 328 B = four full 82 B chunks: every pass after the first sends the
            // maximum (kMaxFramesPerPass full Data frames) and the backlog stays level
            for (int j = 0; j < 8; j++) { Bytes p(35, std::uint8_t(k)); big.push(Type::Rx, k, p); }
            const auto t0 = std::chrono::steady_clock::now();
            t.service(big, 100000, [](std::span<const std::uint8_t>) {});
            const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
            total_ns += ns;
            if (ns > worst_ns) worst_ns = ns;
        }
        std::printf("trace stream pass cost (host -O0): mean %.0f ns, worst %.0f ns, %u frames max per pass\n",
                    total_ns / kPasses, worst_ns, unsigned(ts::kMaxFramesPerPass));
        CHECK_EQ(total_ns / kPasses < 100000.0, true);      // mean under 100 us even at -O0 on a busy host
        // two records per frame, four frames per pass: each pass sent what the loop pushed, minus the
        // Start frame's slot on pass one
        CHECK_EQ(t.stats().records >= std::uint32_t(kPasses) * 8u - 8u, true);
        CHECK_EQ(t.stats().lost, std::uint32_t(0));
    }
    return test_report("test_trace_stream");
}
