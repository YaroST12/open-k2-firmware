// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UFCS diagnostics, pure parts: the hex formatter behind the wire log lines, the decode drop reasons
// and the handshake timing classifier (an instant D+ mirror, as a BC1.2 DCP short gives, versus a
// delayed answer versus none).
#include <array>
#include <cstdint>
#include <cstring>

#include "k2/fastcharge/ufcs.hpp"
#include "k2/drivers/uart2.hpp"
#include "k2/fastcharge/ufcs_diag.hpp"
#include "test_util.h"

using namespace k2::ufcs;

#define CHECK(c) CHECK_EQ(static_cast<bool>(c), true)

static void test_hex_basic() {
    const std::array<std::uint8_t, 5> f{0xAA, 0x20, 0x08, 0x00, 0x63};
    std::array<char, 40> out{};
    const std::size_t n = hex(f, out);
    CHECK_EQ(n, 10);
    CHECK_EQ(std::strcmp(out.data(), "AA20080063"), 0);
    CHECK_EQ(hex({}, out), 0);
    CHECK_EQ(out[0], 0);   // always terminated
}

static void test_hex_truncates_to_whole_bytes() {
    const std::array<std::uint8_t, 4> f{0x01, 0xAB, 0xCD, 0xEF};
    std::array<char, 6> out{};   // room for 2 bytes + NUL (a third byte would need 7)
    CHECK_EQ(hex(f, out), 4);
    CHECK_EQ(std::strcmp(out.data(), "01AB"), 0);
    std::array<char, 1> tiny{'x'};
    CHECK_EQ(hex(f, tiny), 0);
    CHECK_EQ(tiny[0], 0);
    std::array<char, 0> none{};
    CHECK_EQ(hex(f, none), 0);
}

static void test_hex_chunks() {
    CHECK_EQ(kHexChunk, 6);
    CHECK_EQ(chunk_count(0), 0);
    CHECK_EQ(chunk_count(1), 1);
    CHECK_EQ(chunk_count(6), 1);
    CHECK_EQ(chunk_count(7), 2);
    CHECK_EQ(chunk_count(62), 11);
}

static void test_decode_why() {
    Frame f;
    std::size_t used = 0;
    Why w = Why::None;
    // clean frame: no reason
    std::array<std::uint8_t, 8> b{};
    const std::size_t n = encode_control(b, kCtlPing, 0);
    CHECK(decode({b.data(), n}, f, used, &w) == Parse::Ok);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::None));
    // no sync at all: the bytes are dropped as garbage
    const std::array<std::uint8_t, 4> junk{0x00, 0xFF, 0x12, 0x34};
    CHECK(decode(junk, f, used, &w) == Parse::Need);
    CHECK_EQ(used, 4);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::NoSync));
    // garbage then a sync with too few bytes: garbage dropped, sync kept, need more
    const std::array<std::uint8_t, 4> part{0x55, 0xAA, 0x20, 0x08};
    CHECK(decode(part, f, used, &w) == Parse::Need);
    CHECK_EQ(used, 1);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::NoSync));
    // partial frame starting at the sync: nothing dropped, no reason
    const std::array<std::uint8_t, 4> only{0xAA, 0x20, 0x08, 0x00};
    CHECK(decode(only, f, used, &w) == Parse::Need);
    CHECK_EQ(used, 0);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::None));
    // bad CRC
    std::array<std::uint8_t, 5> bad{0xAA, 0x20, 0x08, 0x00, 0x00};
    CHECK(decode(bad, f, used, &w) == Parse::Bad);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::Crc));
    // header type above 1
    const std::array<std::uint8_t, 5> type{0xAA, 0x20, 0x0A, 0x00, 0x00};
    CHECK(decode(type, f, used, &w) == Parse::Bad);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::Type));
    // a data length that cannot fit the accumulator
    const std::array<std::uint8_t, 5> big{0xAA, 0x20, 0x09, 0x01, 0xFF};
    CHECK(decode(big, f, used, &w) == Parse::Bad);
    CHECK_EQ(static_cast<int>(w), static_cast<int>(Why::Length));
    // the reason pointer is optional
    CHECK(decode(bad, f, used) == Parse::Bad);
    CHECK_EQ(std::strcmp(why_name(Why::Crc), "crc"), 0);
    CHECK_EQ(std::strcmp(why_name(Why::NoSync), "nosync"), 0);
    CHECK_EQ(std::strcmp(why_name(Why::Type), "type"), 0);
    CHECK_EQ(std::strcmp(why_name(Why::Length), "length"), 0);
    CHECK_EQ(std::strcmp(why_name(Why::Overflow), "overflow"), 0);
}

static void test_handshake_class() {
    // a DCP short: D+ follows D- within microseconds on every edge
    CHECK(classify({{3, 4}, {2, 3}, 5}) == HsClass::Mirror);
    CHECK(classify({{0, 0}, {0, 0}, 0}) == HsClass::Mirror);
    CHECK(classify({{kMirrorMaxUs, kMirrorMaxUs}, {kMirrorMaxUs, 1}, 1}) == HsClass::Mirror);
    // a source that thinks first: some edge later than the mirror window
    CHECK(classify({{450, 460}, {30, 40}, 800}) == HsClass::Delayed);
    CHECK(classify({{3, 4}, {2, kMirrorMaxUs + 1}, 5}) == HsClass::Delayed);
    // any edge that never came
    CHECK(classify({{-1, 4}, {2, 3}, 5}) == HsClass::NoAnswer);
    CHECK(classify({{3, 4}, {2, -1}, 5}) == HsClass::NoAnswer);
    CHECK(classify({{3, 4}, {2, 3}, -1}) == HsClass::NoAnswer);
    CHECK(std::strcmp(name(HsClass::Mirror), "mirror") == 0);
    CHECK(std::strcmp(name(HsClass::Delayed), "delayed") == 0);
    CHECK(std::strcmp(name(HsClass::NoAnswer), "none") == 0);
}

static void test_remaining_window() {
    CHECK_EQ(remaining_us(2000, 0), 2000);      // D+ was already there: the whole window is still to wait
    CHECK_EQ(remaining_us(2000, 5), 1995);
    CHECK_EQ(remaining_us(2000, 2000), 0);
    CHECK_EQ(remaining_us(2000, 2500), 0);
    CHECK_EQ(remaining_us(8000, -1), 0);        // the wait timed out: the window has passed
}

static void test_uart_error_tally() {
    using namespace k2::uart2;
    Errors e;
    std::uint32_t prev = 0;
    CHECK_EQ(e.total(), 0);
    tally_errors(0, prev, e);
    CHECK_EQ(e.total(), 0);
    tally_errors(kStsFerr | kStsNerr, prev, e);   // a framing and a noise error appear
    CHECK_EQ(e.framing, 1);
    CHECK_EQ(e.noise, 1);
    CHECK_EQ(e.overrun, 0);
    tally_errors(kStsFerr | kStsNerr, prev, e);   // the flags stay set until the next byte: not new errors
    CHECK_EQ(e.total(), 2);
    tally_errors(kStsFerr, prev, e);              // noise cleared, framing still set
    CHECK_EQ(e.total(), 2);
    tally_errors(kStsFerr | kStsRoerr | kStsPerr, prev, e);   // overrun and parity are new, framing is not
    CHECK_EQ(e.overrun, 1);
    CHECK_EQ(e.parity, 1);
    CHECK_EQ(e.framing, 1);
    tally_errors(kStsRdbf | kStsIdlef, prev, e);  // non-error status bits are ignored
    CHECK_EQ(e.total(), 4);
    tally_errors(kStsFerr, prev, e);              // cleared in between, so this one is new
    CHECK_EQ(e.framing, 2);
}

int main() {
    test_remaining_window();
    test_uart_error_tally();
    test_hex_basic();
    test_hex_truncates_to_whole_bytes();
    test_hex_chunks();
    test_decode_why();
    test_handshake_class();
    return test_report("test_ufcs_diag");
}
