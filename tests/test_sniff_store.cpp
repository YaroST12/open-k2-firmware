// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A sniffed edge buffer must survive the trip Black Box slot -> remote capture read -> decoder.
// Field bug (2026-11-02): 4096 edges were stored but the host read back none, because one edge
// record was larger than a remote response can carry and the reader stopped at it.
#include <cstdio>
#include <vector>
#include "k2/fastcharge/edges.hpp"
#include "k2/pd/pdtrace.hpp"
using namespace k2;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

constexpr std::size_t kRemoteOut = 58;   // remote::kMaxRspPayload

// What tools/k2_remote.py blackbox_capture_read does: pages of whole records until next >= len.
static std::vector<std::uint8_t> read_slot(unsigned slot) {
    std::vector<std::uint8_t> data;
    std::uint32_t off = 0;
    for (int guard = 0; guard < 5000; guard++) {
        std::uint8_t out[kRemoteOut];
        const std::size_t n = pdtrace::remote(pdtrace::kOpCapRead, (std::uint32_t(slot) << 16) | off, out);
        std::uint32_t nxt = out[1] | out[2] << 8 | out[3] << 16 | std::uint32_t(out[4]) << 24;
        std::uint32_t len = out[5] | out[6] << 8 | out[7] << 16 | std::uint32_t(out[8]) << 24;
        data.insert(data.end(), out + pdtrace::kReadHeader, out + n);
        if (nxt == off || nxt >= len) break;
        off = nxt;
    }
    return data;
}
static std::size_t decode_all(const std::vector<std::uint8_t>& d) {
    std::size_t i = 0, total = 0;
    while (i + 6 <= d.size() && d[i] >= 6 && i + d[i] <= d.size()) {
        if (d[i + 1] == std::uint8_t(pdtrace::Type::Edges)) {
            edges::Edge e[256]; std::uint8_t idx;
            total += edges::decode_chunk({d.data() + i + 6, std::size_t(d[i]) - 6}, e, idx);
        }
        i += d[i];
    }
    return total;
}
int main() {
    pdtrace::init();
    // 4096 edges 2..9 us apart, like SUPERVOOC traffic
    std::vector<std::uint32_t> w;
    std::uint32_t c = 1000;
    for (int i = 0; i < 4096; i++) { c += 240 * (2 + (i * 7) % 8); w.push_back(edges::pack(c, i & 1, (i >> 1) & 1)); }
    const std::size_t stored = pdtrace::edges_store(w, 0, 0);
    const auto data = read_slot(0);
    const std::size_t got = decode_all(data);
    std::printf("stored %zu edges, remote read decodes %zu (slot bytes %zu)\n", stored, got, data.size());
    CHECK(stored >= 500);          // the slot is edges-only: well over 500 of the first edges
    CHECK(got == stored);          // everything stored is readable remotely
    std::printf(fails ? "sniff_store: %d FAILED\n" : "sniff_store: ok\n", fails);
    return fails ? 1 : 0;
}
