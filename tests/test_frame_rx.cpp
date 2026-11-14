// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// FrameAssembler (k2/core/frame_rx.hpp): turns the byte stream of the WebUSB bulk OUT pipe into whole 64-byte remote
// frames, whatever the chunking of the reads, and resyncs on the request magic after garbage or a lost tail.
#include <array>
#include <cstring>
#include <vector>

#include "k2/core/frame_rx.hpp"
#include "k2/core/remote_proto.hpp"
#include "test_util.h"

using k2::remote::FrameAssembler;
using k2::remote::kFrameSize;

namespace {

using Frame = std::array<std::uint8_t, kFrameSize>;

Frame make_frame(std::uint8_t cmd, std::uint8_t seq, std::uint8_t fill = 0) {
    Frame f{};
    f[0] = k2::remote::kReqMagic0; f[1] = k2::remote::kReqMagic1; f[2] = cmd; f[3] = seq;
    for (std::size_t i = 4; i < f.size(); i++) f[i] = static_cast<std::uint8_t>(fill + i);
    return f;
}

struct Sink {
    std::vector<Frame> got;
    void operator()(std::span<const std::uint8_t, kFrameSize> f) {
        Frame a;
        std::memcpy(a.data(), f.data(), kFrameSize);
        got.push_back(a);
    }
};

void test_whole_frame() {
    FrameAssembler a;
    Sink s;
    const Frame f = make_frame(1, 7);
    a.feed(f, 0, s);
    CHECK_EQ(s.got.size(), 1u);
    CHECK_EQ(s.got[0] == f, 1);
    CHECK_EQ(a.pending(), 0u);
}

void test_split_feeds() {
    for (std::size_t chunk : {1u, 2u, 3u, 7u, 31u, 63u}) {
        FrameAssembler a;
        Sink s;
        const Frame f = make_frame(2, 9, 3);
        for (std::size_t off = 0; off < f.size(); off += chunk) {
            const std::size_t n = std::min(chunk, f.size() - off);
            a.feed(std::span<const std::uint8_t>(f.data() + off, n), 0, s);
        }
        CHECK_EQ(s.got.size(), 1u);
        CHECK_EQ(s.got[0] == f, 1);
    }
}

void test_two_in_one_feed() {
    FrameAssembler a;
    Sink s;
    const Frame f1 = make_frame(1, 1), f2 = make_frame(2, 2, 9);
    std::array<std::uint8_t, 128> both;
    std::memcpy(both.data(), f1.data(), 64);
    std::memcpy(both.data() + 64, f2.data(), 64);
    a.feed(both, 0, s);
    CHECK_EQ(s.got.size(), 2u);
    CHECK_EQ(s.got[0] == f1, 1); CHECK_EQ(s.got[1] == f2, 1);
    // A frame and the start of the next one.
    FrameAssembler b;
    Sink t;
    std::array<std::uint8_t, 100> mix;
    std::memcpy(mix.data(), f1.data(), 64);
    std::memcpy(mix.data() + 64, f2.data(), 36);
    b.feed(mix, 0, t);
    CHECK_EQ(t.got.size(), 1u); CHECK_EQ(b.pending(), 36u);
    b.feed(std::span<const std::uint8_t>(f2.data() + 36, 28), 0, t);
    CHECK_EQ(t.got.size(), 2u); CHECK_EQ(t.got[1] == f2, 1);
}

void test_garbage_prefix() {
    FrameAssembler a;
    Sink s;
    const Frame f = make_frame(5, 5);
    std::array<std::uint8_t, 5 + 64> in;
    const std::uint8_t junk[5] = {0x00, 0xA5, 0x11, 0xFF, 0x4B};   // a lone A5, a lone 4B: neither starts a frame
    std::memcpy(in.data(), junk, 5);
    std::memcpy(in.data() + 5, f.data(), 64);
    a.feed(in, 0, s);
    CHECK_EQ(s.got.size(), 1u); CHECK_EQ(s.got[0] == f, 1);
    // A5 A5 4B ...: the first A5 is junk, the second starts the frame.
    FrameAssembler b;
    Sink t;
    std::array<std::uint8_t, 65> dbl;
    dbl[0] = 0xA5;
    std::memcpy(dbl.data() + 1, f.data(), 64);
    b.feed(dbl, 0, t);
    CHECK_EQ(t.got.size(), 1u); CHECK_EQ(t.got[0] == f, 1);
    // Responses (A6 4B) are not requests.
    FrameAssembler c;
    Sink u;
    Frame r = f;
    r[0] = k2::remote::kRspMagic0;
    c.feed(r, 0, u);
    CHECK_EQ(u.got.size(), 0u); CHECK_EQ(c.pending(), 0u);
}

void test_reset_and_timeout() {
    FrameAssembler a;
    Sink s;
    const Frame f = make_frame(3, 3);
    a.feed(std::span<const std::uint8_t>(f.data(), 20), 100, s);
    CHECK_EQ(a.pending(), 20u);
    a.reset();
    CHECK_EQ(a.pending(), 0u);
    a.feed(f, 101, s);
    CHECK_EQ(s.got.size(), 1u);

    // A partial frame older than the timeout is dropped: the next feed starts clean instead of swallowing a whole frame.
    FrameAssembler b;
    Sink t;
    b.feed(std::span<const std::uint8_t>(f.data(), 20), 1000, t);
    b.feed(f, 1000 + FrameAssembler::kPartialTimeoutMs + 1, t);
    CHECK_EQ(t.got.size(), 1u); CHECK_EQ(t.got[0] == f, 1); CHECK_EQ(b.pending(), 0u);
    // Inside the timeout the tail completes the first frame.
    FrameAssembler c;
    Sink u;
    c.feed(std::span<const std::uint8_t>(f.data(), 20), 1000, u);
    c.feed(std::span<const std::uint8_t>(f.data() + 20, 44), 1000 + FrameAssembler::kPartialTimeoutMs, u);
    CHECK_EQ(u.got.size(), 1u);
    CHECK_EQ(u.got[0] == f, 1);
}

void test_hold() {
    // hold(): the reader could not take bytes (backpressure), so the partial frame keeps its clock instead of timing out.
    const Frame f = make_frame(4, 4);
    FrameAssembler a;
    Sink s;
    a.feed(std::span<const std::uint8_t>(f.data(), 20), 1000, s);
    a.hold(1000 + FrameAssembler::kPartialTimeoutMs);
    a.hold(1000 + 2 * FrameAssembler::kPartialTimeoutMs);
    a.feed(std::span<const std::uint8_t>(f.data() + 20, 44), 1000 + 2 * FrameAssembler::kPartialTimeoutMs + 10, s);
    CHECK_EQ(s.got.size(), 1u);
    CHECK_EQ(s.got[0] == f, 1);
}

}  // namespace

int main() {
    test_whole_frame();
    test_split_feeds();
    test_two_in_one_feed();
    test_garbage_prefix();
    test_reset_and_timeout();
    test_hold();
    return test_report("test_frame_rx");
}
