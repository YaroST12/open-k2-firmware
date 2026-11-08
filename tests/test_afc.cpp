// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// ACCP/AFC single-wire physical layer and Samsung AFC command bytes, pure.
#include <vector>

#include "k2/fastcharge/afc.hpp"
#include "k2/fastcharge/fc_seq.hpp"
#include "k2/fastcharge/wire.hpp"
#include "test_util.h"

#define CHECK(c) CHECK_EQ(!!(c), 1)

using namespace k2;
using dpdm::Drive;

struct FakeIo {
    std::uint32_t now = 0;
    struct Ev { std::uint32_t t; Drive dp, dm; };
    std::vector<Ev> ev;
    void set(Drive dp, Drive dm) { ev.push_back({now, dp, dm}); }
    void wait_us(std::uint32_t us) { now += us; }
};

// What D- reads every `period` us during a step list (1 = high), sampled mid-period.
static std::vector<std::uint8_t> sample_dm(std::span<const dpdm::Step> steps, std::uint32_t from_us, std::uint32_t period,
                                           std::size_t count) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < count; i++) {
        const std::uint32_t t = from_us + static_cast<std::uint32_t>(i) * period + period / 2;
        std::uint32_t acc = 0;
        std::uint8_t v = 0;
        for (const auto& s : steps) {
            if (t < acc + s.us) { v = s.dm == Drive::V2p0 ? 1 : 0; break; }
            acc += s.us;
        }
        out.push_back(v);
    }
    return out;
}

static void test_afc_bytes() {
    // (V_code << 4) | I_code: 9 V / 1.65 A = 0x46, 12 V / 2.10 A = 0x79.
    CHECK_EQ(afc::afc_byte(9000).value(), 0x46);
    CHECK_EQ(afc::afc_byte(12000).value(), 0x79);
    CHECK_EQ(afc::afc_byte(5000).has_value(), false);
    CHECK_EQ(afc::afc_byte(20000).has_value(), false);
    CHECK_EQ(afc::kSends, 3);
    CHECK_EQ(afc::kSendGapMs, 10u);
}

static void test_master_ping() {
    fc::StepList<4> l;
    wire::master_ping(l);
    CHECK_EQ(l.n, 2u);
    CHECK(l.steps[0].dm == Drive::V2p0 && l.steps[0].us == 2570);
    CHECK(l.steps[1].dm == Drive::V0 && l.steps[1].us == 10);
    CHECK(l.steps[0].dp == Drive::V0p6 && l.steps[1].dp == Drive::V0p6);   // D+ held at 0.6 V
}

static void test_parity_and_stop() {
    CHECK_EQ(wire::parity_bit(0x46), false);   // 0100 0110: 3 ones (odd) -> parity bit 0
    CHECK_EQ(wire::parity_bit(0x79), false);   // 0111 1001: 5 ones (odd) -> parity bit 0
    CHECK_EQ(wire::parity_bit(0x03), true);    // 2 ones (even) -> parity bit 1
}

// The transmit waveform as a (level, us) list. L = D- 0 V, H = D- 2.0 V.
struct E { bool high; std::uint32_t us; };
static std::vector<E> steps_of(std::span<const std::uint8_t> bytes) {
    fc::StepList<64> l;
    wire::frame_steps(l, bytes);
    std::vector<E> out;
    for (std::size_t i = 0; i < l.n; i++) {
        CHECK(l.steps[i].dp == Drive::V0p6);   // D+ held at 0.6 V
        out.push_back({l.steps[i].dm == Drive::V2p0, l.steps[i].us});
    }
    return out;
}
static void data(std::vector<E>& v, std::uint8_t b) {
    for (int i = 7; i >= 0; i--) v.push_back({((b >> i) & 1) != 0, 162});
}
static bool same(const std::vector<E>& a, const std::vector<E>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (a[i].high != b[i].high || a[i].us != b[i].us) return false;
    return true;
}
static const std::vector<E> kHeader = {{false, 41}, {true, 41}, {false, 41}, {true, 41}};

// One byte 0x01: header once, bit 7 is 0 so no gap, 8 data bits, odd ones -> parity 0 (L 162), stop H 40,
// then the 40 us low that precedes the end-of-frame master ping.
static void test_single_byte_frame() {
    const std::uint8_t f[] = {0x01};
    std::vector<E> want = kHeader;
    data(want, 0x01);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
    CHECK_EQ(want.size(), 15u);
}

// Even ones (0x81): parity H 162, stop L 40 H 40, then L. Next byte 0x01 has bit 7 = 0 and the previous parity was
// even: no gap and no extra pulse; the data bits follow directly.
static void test_even_parity_then_clear_msb() {
    const std::uint8_t f[] = {0x81, 0x01};
    std::vector<E> want = kHeader;
    want.push_back({false, 40});   // first byte with bit 7 = 1
    data(want, 0x81);
    want.push_back({true, 162});
    want.push_back({false, 40});
    want.push_back({true, 40});
    data(want, 0x01);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
}

// Odd parity then a byte with bit 7 = 1: the line is held low 40 us, then the data starts.
static void test_odd_parity_then_set_msb() {
    const std::uint8_t f[] = {0x01, 0x80};
    std::vector<E> want = kHeader;
    data(want, 0x01);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    data(want, 0x80);
    want.push_back({false, 162});   // 0x80: one one, odd
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
}

// Odd parity then a byte with bit 7 = 0: low 41, a re-sync pulse high 41, then the data.
static void test_odd_parity_then_clear_msb() {
    const std::uint8_t f[] = {0x01, 0x01};
    std::vector<E> want = kHeader;
    data(want, 0x01);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 41});
    want.push_back({true, 41});
    data(want, 0x01);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
}

// Even parity then bit 7 = 1: low 40 then data.
static void test_even_parity_then_set_msb() {
    const std::uint8_t f[] = {0x03, 0x80};
    std::vector<E> want = kHeader;
    data(want, 0x03);
    want.push_back({true, 162});
    want.push_back({false, 40});
    want.push_back({true, 40});
    want.push_back({false, 40});
    data(want, 0x80);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
}

// A first byte with bit 7 = 1 waits 40 us low after the header's fourth pulse.
static void test_first_byte_msb_set() {
    const std::uint8_t f[] = {0x80};
    std::vector<E> want = kHeader;
    want.push_back({false, 40});
    data(want, 0x80);
    want.push_back({false, 162});
    want.push_back({true, 40});
    want.push_back({false, 40});
    CHECK(same(steps_of(f), want));
}

// What the charger's D- shows at a 10 us oversampling.
static std::vector<std::uint8_t> capture(const std::vector<std::uint8_t>& frame, std::uint32_t preroll_us, std::uint32_t tail_us) {
    fc::StepList<64> l;
    l.push(Drive::V0p6, Drive::V0, preroll_us);
    wire::frame_steps(l, frame);
    wire::master_ping(l);
    l.push(Drive::V0p6, Drive::V0, tail_us);
    return sample_dm(l.span(), 0, wire::kRxPeriodUs, fc::total_us(l.span()) / wire::kRxPeriodUs);
}

static void test_decode_every_byte() {
    for (std::uint32_t pre : {50u, 400u}) {
        for (int v = 0; v < 256; v++) {
            const auto s = capture({static_cast<std::uint8_t>(v)}, pre, 100);
            std::uint8_t out[1] = {0};
            CHECK_EQ(wire::decode_frame(s, out), 1u);
            CHECK_EQ(out[0], v);
        }
    }
}

static void test_decode_three_byte_frames() {
    const std::uint8_t cases[][3] = {{0x08, 0x55, 0x4C}, {0x0C, 0x21, 0x00}, {0x08, 0x00, 0x00}, {0x01, 0x01, 0x01},
                                     {0x81, 0x80, 0x03}, {0xFF, 0x00, 0x80}, {0x08, 0xD2, 0x7F}};
    for (const auto& c : cases) {
        const auto s = capture({c[0], c[1], c[2]}, 60, 100);
        std::uint8_t out[3] = {};
        CHECK_EQ(wire::decode_frame(s, out), 3u);
        CHECK(out[0] == c[0] && out[1] == c[1] && out[2] == c[2]);
    }
}

static void test_decode_pairs_with_every_gap_rule() {
    for (int a = 0; a < 256; a++) {
        for (int b : {0x00, 0x01, 0x80, 0xFF, 0x55}) {
            const auto s = capture({static_cast<std::uint8_t>(a), static_cast<std::uint8_t>(b)}, 60, 100);
            std::uint8_t out[2] = {};
            CHECK_EQ(wire::decode_frame(s, out), 2u);
            CHECK(out[0] == a && out[1] == b);
        }
    }
}

// The decoder ignores the parity bit, so a corrupt parity does not matter.
static void test_decode_ignores_parity() {
    auto s = capture({0x46}, 60, 100);
    // the parity bit window: the data bits start at the last header fall (60 + 164 us), the parity bit is the 9th
    const std::size_t first = (60 + 164 + 8 * 162) / 10;
    for (std::size_t i = first + 2; i < first + 14; i++) s[i] ^= 1;
    std::uint8_t out[1] = {};
    CHECK_EQ(wire::decode_frame(s, out), 1u);
    CHECK_EQ(out[0], 0x46);
}

// A frame shorter than the buffer stops at the trailing ping (160 samples high) and reports what it got.
static void test_decode_short_reply_and_silence() {
    const auto s = capture({0x08, 0x2A}, 60, 100);
    std::uint8_t out[3] = {};
    CHECK_EQ(wire::decode_frame(s, out), 2u);
    CHECK(out[0] == 0x08 && out[1] == 0x2A);
    const std::vector<std::uint8_t> idle(600, 0);
    CHECK_EQ(wire::decode_frame(idle, out), 0u);
}

static void test_slave_ping_window() {
    CHECK_EQ(wire::slave_ping_ok(2560), true);
    CHECK_EQ(wire::slave_ping_ok(300), true);
    CHECK_EQ(wire::slave_ping_ok(4000), true);
    CHECK_EQ(wire::slave_ping_ok(299), false);
    CHECK_EQ(wire::slave_ping_ok(4001), false);
}

int main() {
    test_afc_bytes();
    test_master_ping();
    test_parity_and_stop();
    test_single_byte_frame();
    test_even_parity_then_clear_msb();
    test_odd_parity_then_set_msb();
    test_odd_parity_then_clear_msb();
    test_even_parity_then_set_msb();
    test_first_byte_msb_set();
    test_decode_every_byte();
    test_decode_three_byte_frames();
    test_decode_pairs_with_every_gap_rule();
    test_decode_ignores_parity();
    test_decode_short_reply_and_silence();
    test_slave_ping_window();
    return test_report("test_afc");
}
