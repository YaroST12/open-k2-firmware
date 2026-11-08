// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// LZ4-block codec (k2/core/lz.hpp): round trips on synthetic and on black-box shaped traces, the size
// limits, and decoder robustness against corrupt streams.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "k2/core/lz.hpp"
#include "k2/pd/pdtrace.hpp"
#include "test_util.h"

using namespace k2;
using Buf = std::vector<std::uint8_t>;

namespace {
bool round_trip(const Buf& in, std::size_t* packed = nullptr) {
    Buf z(lz::bound(in.size()));
    const std::size_t n = lz::compress(in, z);
    if (n == 0 && !in.empty()) return false;
    if (packed) *packed = n;
    Buf out(in.size() + 8, 0xAA);
    const std::size_t m = lz::decompress({z.data(), n}, out);
    if (m != in.size()) return false;
    for (std::size_t i = 0; i < in.size(); i++) if (out[i] != in[i]) return false;
    return out[in.size()] == 0xAA;   // nothing written past the end
}

// A typical black box window: Source_Capabilities, Request, Accept, PS_RDY, VBUS ramps, Cc and
// Status0 events, GoodCRC TxResults, the way pdtrace::Ring stores them.
Buf trace(unsigned seed, std::size_t want) {
    pdtrace::Ring<8192> ring;
    std::uint32_t ts = 1000 * seed;
    auto u32 = [](Buf& p, std::uint32_t v) { for (int i = 0; i < 4; i++) p.push_back(std::uint8_t(v >> (8 * i))); };
    auto rx = [&](unsigned hdr, std::initializer_list<std::uint32_t> objs) {
        Buf p = {0xE0, std::uint8_t(hdr), std::uint8_t(hdr >> 8)};
        for (auto o : objs) u32(p, o);
        p.insert(p.end(), {0x11, 0x22, 0x33, std::uint8_t(0x40 + seed)});
        ring.push(pdtrace::Type::Rx, ts += 900, p);
    };
    unsigned mv = 5000;
    while (ring.head() - ring.tail() < want) {
        rx(0x7161, {0x0001912C, 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0006414F, 0xC0DC213C, 0xC1A8213C});
        Buf tx = {0x82, 0x12};
        u32(tx, 0x2004B12C);
        ring.push(pdtrace::Type::Tx, ts += 400, tx);
        ring.push(pdtrace::Type::TxResult, ts += 300, Buf{0, 2, 0x10});
        rx(0x0363, {});
        rx(0x0566, {});
        for (int k = 0; k < 4; k++, mv += 2500) {
            Buf v = {std::uint8_t(mv), std::uint8_t(mv >> 8), 0x9C, 0xFF};
            ring.push(pdtrace::Type::Vbus, ts += 8000, v);
        }
        ring.push(pdtrace::Type::Status0, ts += 100, Buf{0x40});
        ring.push(pdtrace::Type::Cc, ts += 100, Buf{1, 1, 0x73});
        mv = 5000;
    }
    Buf out(want);
    const auto r = ring.read(ring.cursor_for_last(want), out);
    out.resize(r.bytes);
    return out;
}
}  // namespace

int main() {
    {   // edges
        CHECK_EQ(round_trip(Buf{}), 1);
        CHECK_EQ(round_trip(Buf{7}), 1);
        CHECK_EQ(round_trip(Buf(4, 0)), 1);
        CHECK_EQ(round_trip(Buf(5, 9)), 1);
        CHECK_EQ(round_trip(Buf(12, 1)), 1);
        CHECK_EQ(round_trip(Buf(13, 1)), 1);
        CHECK_EQ(round_trip(Buf(17, 1)), 1);
    }
    {   // long runs: match lengths past 15 + 255 need the extension bytes
        std::size_t z = 0;
        Buf zeros(2048, 0);
        CHECK_EQ(round_trip(zeros, &z), 1);
        CHECK_EQ(z < 32, 1);
        Buf ramp(3000);
        for (std::size_t i = 0; i < ramp.size(); i++) ramp[i] = std::uint8_t(i % 7);
        CHECK_EQ(round_trip(ramp), 1);
    }
    {   // incompressible data still round trips and stays within bound()
        std::srand(1);
        Buf r(2048);
        for (auto& b : r) b = std::uint8_t(std::rand());
        std::size_t z = 0;
        CHECK_EQ(round_trip(r, &z), 1);
        CHECK_EQ(z <= lz::bound(r.size()), 1);
    }
    {   // black-box shaped traces: round trip and a real saving
        for (unsigned seed = 1; seed <= 8; seed++) {
            const Buf t = trace(seed, 1024 + 150 * seed);
            std::size_t z = 0;
            CHECK_EQ(round_trip(t, &z), 1);
            CHECK_EQ(z * 3 < t.size() * 2, 1);   // at least 1.5x
        }
        const Buf full = trace(3, 2040);
        std::size_t z = 0;
        CHECK_EQ(round_trip(full, &z), 1);
        printf("lz: %zu -> %zu bytes (%.2fx)\n", full.size(), z, double(full.size()) / double(z));
    }
    {   // output too small: compress reports failure, decompress reports an error
        const Buf t = trace(2, 1500);
        Buf small(100);
        CHECK_EQ(lz::compress(t, small), 0);
        Buf z(lz::bound(t.size()));
        const std::size_t n = lz::compress(t, z);
        Buf tiny(t.size() - 1);
        CHECK_EQ(lz::decompress({z.data(), n}, tiny), lz::kError);
    }
    {   // corrupt streams never run off the buffers
        const Buf t = trace(4, 1600);
        Buf z(lz::bound(t.size()));
        const std::size_t n = lz::compress(t, z);
        Buf out(t.size());
        for (std::size_t cut = 0; cut < n; cut += 7) lz::decompress({z.data(), cut}, out);   // truncated
        std::srand(5);
        for (int k = 0; k < 400; k++) {
            Buf bad(z.begin(), z.begin() + std::ptrdiff_t(n));
            for (int j = 0; j < 3; j++) bad[std::size_t(std::rand()) % n] ^= std::uint8_t(1 + std::rand() % 255);
            const std::size_t m = lz::decompress(bad, out);
            CHECK_EQ(m == lz::kError || m <= out.size(), 1);
        }
        const std::uint8_t offset_zero[] = {0x14, 'a', 0, 0, 0};
        CHECK_EQ(lz::decompress(offset_zero, out), lz::kError);
        const std::uint8_t offset_far[] = {0x14, 'a', 9, 0, 0};
        CHECK_EQ(lz::decompress(offset_far, out), lz::kError);
    }
    return test_report("test_lz");
}
