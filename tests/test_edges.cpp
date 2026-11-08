// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include <cstdio>
#include <vector>
#include "k2/fastcharge/edges.hpp"
using namespace k2::edges;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void test_pack() {
    const std::uint32_t w = pack(0x12345678, true, false);
    CHECK(state(w) == 2); CHECK(dm(w) && !dp(w));
    CHECK(units(w) == (0x12345678u >> 6));
    CHECK(state(pack(1000, false, true)) == 1);
}
static void test_recorder() {
    std::uint32_t buf[3];
    Recorder r; r.init(buf, 3);
    CHECK(r.push(1)); CHECK(r.push(2)); CHECK(!r.overflowed()); CHECK(r.push(3));
    CHECK(r.full()); CHECK(!r.push(4)); CHECK(r.overflowed()); CHECK(r.size() == 3);
    r.reset(); CHECK(r.size() == 0 && !r.overflowed());
}
static void test_session() {
    Session s; s.arm(100);
    CHECK(s.poll(200, 0, false, 5000) == End::None);       // no edge yet: waits forever
    CHECK(s.poll(5000, 0, false, 5000) == End::None);
    CHECK(s.poll(5001, 1, false, 5000) == End::None && s.started());
    CHECK(s.poll(5100, 5, false, 5000) == End::None);
    CHECK(s.poll(5599, 5, false, 5000) == End::None);
    CHECK(s.poll(5600, 5, false, 5000) == End::Idle && !s.armed());
    CHECK(s.poll(5700, 5, false, 5000) == End::None);       // reported once
    Session f; f.arm(0); CHECK(f.poll(10, 9, true, 5000) == End::Full);
    Session d; d.arm(0);
    CHECK(d.poll(5, 1, false, 5000) == End::None); CHECK(d.poll(20, 1, false, 0) == End::Detach);
    Session q; q.arm(0);   // VBUS drops before any edge (phone unplugged after arming): keeps waiting
    CHECK(q.poll(5, 0, false, 8400) == End::None); CHECK(q.poll(20, 0, false, 0) == End::None);
    Session z; z.arm(0);   // armed with VBUS 0: waits for the phone, no detach
    CHECK(z.poll(10, 0, false, 0) == End::None); CHECK(z.poll(9000, 0, false, 0) == End::None);
    CHECK(z.poll(9100, 1, false, 0) == End::None);   // first edge while VBUS is still 0: VBUS never seen high
    CHECK(z.poll(9200, 2, false, 0) == End::None);
    CHECK(z.poll(9300, 3, false, 5000) == End::None); CHECK(z.poll(9400, 4, false, 0) == End::Detach);
    Session c; c.arm(0); CHECK(c.cancel() == End::Cancel); CHECK(c.cancel() == End::None);
    Session t; t.arm(0);
    std::size_t n = 1; std::uint32_t now = 0; End e = End::None;
    while (e == End::None && now < 10000) { now += 100; e = t.poll(now, n++, false, 5000); }
    CHECK(e == End::Timeout);
}
static void test_overflow_keeps_first() {
    std::uint32_t buf[4]; Recorder r; r.init(buf, 4);
    for (std::uint32_t i = 1; i <= 10; i++) r.push(i);
    CHECK(r.size() == 4 && r.at(0) == 1 && r.at(3) == 4);   // the first edges (the handshake) are kept
    CHECK(r.dropped() == 6 && r.total() == 10 && r.overflowed());
}
static void test_read_record() {
    std::vector<std::uint32_t> w;
    for (int i = 0; i < 300; i++) w.push_back(pack(240u * (100 + i * 3), i & 1, 0));
    std::vector<Edge> got; std::size_t from = 0;
    while (from < w.size()) {
        std::uint8_t out[49]; std::size_t used;
        const std::size_t n = read_record(w, from, 15, 77, out, used);
        CHECK(n > 0 && n <= 49 && out[0] == n && out[1] == 15);
        Edge e[64]; std::uint8_t idx;
        const std::size_t k = decode_chunk({out + 6, n - 6}, e, idx);
        CHECK(k == used);
        for (std::size_t i = 0; i < k; i++) got.push_back(e[i]);
        from += used;
    }
    CHECK(got.size() == w.size());
}
static void test_stream() {
    std::uint32_t buf[4]; StreamRing r; r.init(buf, 4);
    for (std::uint32_t i = 1; i <= 6; i++) r.push(i);
    CHECK(r.pending() == 4 && r.overruns() == 2 && r.pushed() == 4);   // full: the new edges are dropped and counted
    std::uint32_t w[8];
    CHECK(r.peek(w) == 4 && w[0] == 1 && w[3] == 4 && r.pending() == 4);   // peek does not consume
    r.drop(3); CHECK(r.pending() == 1);
    r.push(7); r.push(8); r.push(9); r.push(10);   // wraps; one fits, then full again
    CHECK(r.peek(w) == 4 && w[0] == 4 && w[1] == 7 && w[3] == 9 && r.overruns() == 3);
    // frame layout: sync, len, payload, sum
    const std::uint8_t pl[3] = {1, 2, 3}; std::uint8_t f[16];
    CHECK(frame(pl, f) == 6 && f[0] == 0xA5 && f[1] == 3 && f[5] == 9);
    std::uint8_t big[kChunkBytes + 1] = {};
    CHECK(frame(big, f) == 0);
}
static void test_word_us() {
    CHECK(word_us(pack(240 * 1000, false, false), 0, 50) == 1050);
    CHECK(word_us(pack(100, false, false), 0xFFFFFF00u, 7) == 7 + (0x100u + 64u) / 240);  // wrap of the cycle counter
}
static void test_roundtrip() {
    std::vector<std::uint32_t> w;
    std::uint32_t c = 0xFFFFF000u;   // crosses the 32-bit cycle wrap
    for (int i = 0; i < 700; i++) {
        c += 240 * (1 + (i * 37) % 500);
        w.push_back(pack(c, i & 1, (i >> 1) & 1));
    }
    std::vector<Edge> got;
    std::size_t from = 0; std::uint8_t idx = 0;
    while (from < w.size()) {
        std::uint8_t buf[255]; std::size_t len = 0;
        const std::size_t used = encode_chunk(w, from, idx, buf, len);
        CHECK(used > 0); CHECK(len <= kChunkBytes);
        Edge e[256]; std::uint8_t gi = 0;
        const std::size_t n = decode_chunk({buf, len}, e, gi);
        CHECK(n == used); CHECK(gi == idx);
        for (std::size_t i = 0; i < n; i++) got.push_back(e[i]);
        from += used; idx++;
    }
    CHECK(got.size() == w.size());
    for (std::size_t i = 0; i < got.size() && i < w.size(); i++) {
        CHECK(got[i].units == units(w[i])); CHECK(got[i].state == state(w[i]));
    }
}
static void test_golden() {
    // two edges 3 units apart: idx 7, first_units 0x000102, deltas 0 then 3
    const std::uint32_t w[2] = { (0x000102u << 6) | 1u, (0x000105u << 6) | 3u };
    std::uint8_t buf[32]; std::size_t len = 0;
    CHECK(encode_chunk(w, 0, 7, buf, len) == 2);
    const std::uint8_t want[] = {7, 0x02, 0x01, 0x00, 0x00, (0 << 2) | 1, (3 << 2) | 3};
    CHECK(len == sizeof want);
    for (std::size_t i = 0; i < sizeof want && i < len; i++) CHECK(buf[i] == want[i]);
    const std::uint8_t bad[] = {1, 0, 0, 0, 0, 0x80};
    Edge e[4]; std::uint8_t gi;
    CHECK(decode_chunk(bad, e, gi) == 0);
}
int main() {
    test_pack(); test_recorder(); test_session(); test_overflow_keeps_first(); test_read_record(); test_stream(); test_word_us(); test_roundtrip(); test_golden();
    std::printf(fails ? "edges: %d FAILED\n" : "edges: ok\n", fails);
    return fails ? 1 : 0;
}
