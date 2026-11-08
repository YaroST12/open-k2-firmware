// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// k2::BinLog: the binary K2LOG ring the QR share record "LOG" carries. Entry layout (docs/QR_SHARE.md):
//   len u8 (body length), dt varint (ms since the previous entry), id varint, then one value per argument kind:
//   signed = zigzag varint, unsigned = varint, string = len u8 + bytes (cut to kStrMax).
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <vector>

#include "k2/core/binlog.hpp"
#include "test_util.h"

using namespace k2;
using logfmt::kSigned;
using logfmt::kString;
using logfmt::kUnsigned;

namespace {

constexpr std::uint32_t sig(std::initializer_list<std::uint32_t> kinds) {
    std::uint32_t s = 0;
    int n = 0;
    for (auto k : kinds) s |= k << (2 * n++);
    return s;
}

template <std::size_t N>
void push(BinLog<N> &log, std::uint32_t ms, std::uint16_t id, std::uint32_t s, ...) {
    std::va_list ap;
    va_start(ap, s);
    log.push(ms, id, s, ap);
    va_end(ap);
}

std::vector<std::uint8_t> bytes(std::initializer_list<int> v) {
    std::vector<std::uint8_t> out;
    for (int b : v) out.push_back(static_cast<std::uint8_t>(b));
    return out;
}

template <std::size_t N>
std::vector<std::uint8_t> snap(const BinLog<N> &log, std::size_t room, BinLogSnapshot *info = nullptr) {
    std::uint8_t buf[512];
    const BinLogSnapshot s = log.snapshot({buf, room});
    if (info) *info = s;
    return {buf, buf + s.len};
}

void test_entry_layout() {
    BinLog<256> log;
    push(log, 100, 7, 0);                                  // no args
    push(log, 130, 300, sig({kSigned, kUnsigned}), -3, 0x1F5);
    push(log, 131, 9, sig({kString}), "ab");
    BinLogSnapshot info;
    const auto got = snap(log, 256, &info);
    // dt 100, id 7  |  dt 30, id 300 = AC 02, zigzag(-3) = 5, 0x1F5 = F5 03  |  dt 1, id 9, "ab"
    const auto want = bytes({2, 100, 7, 6, 30, 0xAC, 0x02, 5, 0xF5, 0x03, 5, 1, 9, 2, 'a', 'b'});
    CHECK_EQ(got == want, true);
    CHECK_EQ(info.base_ms, 0u);
    CHECK_EQ(info.older, false);
}

void test_value_edges() {
    BinLog<256> log;
    push(log, 5, 1, sig({kSigned, kSigned, kUnsigned}), INT32_MIN, 2147483647, 0xFFFFFFFFu);
    const auto got = snap(log, 256);
    const auto want = bytes({17, 5, 1,
                             0xFF, 0xFF, 0xFF, 0xFF, 0x0F,    // zigzag(INT32_MIN) = 0xFFFFFFFF
                             0xFE, 0xFF, 0xFF, 0xFF, 0x0F,    // zigzag(INT32_MAX) = 0xFFFFFFFE
                             0xFF, 0xFF, 0xFF, 0xFF, 0x0F});
    CHECK_EQ(got == want, true);
}

void test_long_string_is_cut_and_null_is_empty() {
    BinLog<256> log;
    push(log, 1, 2, sig({kString, kString}), "0123456789ABCDEFGHIJ", nullptr);
    const auto got = snap(log, 256);
    CHECK_EQ(got[2], 2u);                                  // id
    CHECK_EQ(got[3], static_cast<unsigned>(BinLog<256>::kStrMax));
    CHECK_EQ(std::memcmp(&got[4], "0123456789AB", 12), 0);
    CHECK_EQ(got[4 + 12], 0u);                             // the null string is a zero-length one
}

void test_entry_is_capped() {
    BinLog<256> log;
    const char *s = "0123456789ABCDEF";
    push(log, 1, 2, sig({kString, kString, kString, kString, kString}), s, s, s, s, s);
    const auto got = snap(log, 256);
    CHECK_EQ(got.size() <= 1u + BinLog<256>::kEntryMax, true);
    CHECK_EQ(got[0], got.size() - 1);                       // len covers exactly the body that was kept
}

void test_wrap_drops_whole_entries_and_tracks_the_base() {
    BinLog<12> log;                                         // each entry below is 3 bytes: four fit
    for (std::uint32_t i = 0; i < 6; i++) push(log, 10 * (i + 1), static_cast<std::uint16_t>(i + 1), 0);
    BinLogSnapshot info;
    const auto got = snap(log, 64, &info);
    CHECK_EQ(got == bytes({2, 10, 3, 2, 10, 4, 2, 10, 5, 2, 10, 6}), true);   // ids 3..6, whole entries
    CHECK_EQ(info.base_ms, 20u);                            // the time of id 2, the entry before the first kept one
    CHECK_EQ(info.older, true);
}

void test_snapshot_takes_the_newest_whole_entries_that_fit() {
    BinLog<64> log;
    for (std::uint32_t i = 1; i <= 5; i++) push(log, 100 * i, static_cast<std::uint16_t>(i), 0);   // 5 entries of 3 B
    BinLogSnapshot info;
    const auto got = snap(log, 7, &info);                  // room for two entries, not three
    CHECK_EQ(got.size(), 6u);
    CHECK_EQ(got[2], 4u);
    CHECK_EQ(got[5], 5u);
    CHECK_EQ(info.base_ms, 300u);                           // up to and including the skipped entry id 3
    CHECK_EQ(info.older, true);
    CHECK_EQ(snap(log, 2).size(), 0u);                      // nothing fits
    CHECK_EQ(snap(log, 0).size(), 0u);
    CHECK_EQ(snap(log, 15, &info).size(), 15u);
    CHECK_EQ(info.base_ms, 0u);
    CHECK_EQ(info.older, false);
}

void test_delta_survives_a_32_bit_clock_wrap() {
    BinLog<64> log;
    push(log, 0xFFFFFFF0u, 1, 0);
    push(log, 0x10u, 2, 0);                                // 32 ms later
    const auto got = snap(log, 64);
    CHECK_EQ(got.size(), 3u + 1u + 6u);                    // first dt 0xFFFFFFF0 is a 5-byte varint
    CHECK_EQ(got[got.size() - 2], 32u);                     // dt of the second entry
    CHECK_EQ(got[got.size() - 1], 2u);                      // its id
}

void test_clear() {
    BinLog<64> log;
    push(log, 5, 1, 0);
    log.clear();
    CHECK_EQ(snap(log, 64).size(), 0u);
    CHECK_EQ(log.size(), 0u);
}

// The live reader (k2/core/dbg_stream.hpp) keeps an absolute byte position and an entry number: the ring is the queue.
void test_live_read_counts_and_positions() {
    BinLog<64> log;
    CHECK_EQ(log.head_abs(), 0u);
    push(log, 5, 1, 0);                                    // 3 bytes: len, dt, id
    push(log, 9, 2, 0);
    CHECK_EQ(log.entries_pushed(), 2u);
    CHECK_EQ(log.last_ms(), 9u);
    CHECK_EQ(log.entries_dropped(), 0u);
    CHECK_EQ(log.head_abs(), 6u);
    std::uint8_t buf[16];
    CHECK_EQ(log.read(0, buf), 6u);
    CHECK_EQ(buf[0], 2u);
    CHECK_EQ(buf[3], 2u);
    CHECK_EQ(log.read(3, buf), 3u);                        // from the second entry
    CHECK_EQ(buf[2], 2u);                                  // its id
    CHECK_EQ(log.read(6, buf), 0u);                        // at the head: nothing
    CHECK_EQ(log.read(0, std::span<std::uint8_t>(buf, 4)), 3u);   // only whole entries
    CHECK_EQ(BinLog<64>::dt_of(buf), 5u);
}

void test_live_read_survives_wrap_and_reports_drops() {
    BinLog<16> log;                                        // five 3-byte entries fill 15 bytes
    for (int i = 0; i < 5; i++) push(log, 10u * (i + 1), static_cast<std::uint16_t>(i + 1), 0);
    CHECK_EQ(log.tail_abs(), 0u);
    push(log, 60, 6, 0);                                   // drops entry 1
    push(log, 70, 7, 0);                                   // drops entry 2
    CHECK_EQ(log.entries_pushed(), 7u);
    CHECK_EQ(log.entries_dropped(), 2u);
    CHECK_EQ(log.tail_abs(), 6u);
    CHECK_EQ(log.head_abs(), 21u);
    CHECK_EQ(log.base_ms(), 20u);                          // the time of entry 2, the last one dropped
    std::uint8_t buf[32];
    const std::size_t n = log.read(log.tail_abs(), buf);
    CHECK_EQ(n, 15u);
    CHECK_EQ(buf[2], 3u);                                  // the oldest left is entry 3
    CHECK_EQ(buf[n - 1], 7u);
    CHECK_EQ(log.read(18, buf), 3u);                       // positions stay absolute across the wrap
    CHECK_EQ(buf[2], 7u);
}

void test_clear_keeps_the_counters_monotonic() {
    BinLog<64> log;
    push(log, 5, 1, 0);
    push(log, 6, 2, 0);
    log.clear();
    CHECK_EQ(log.tail_abs(), 6u);
    CHECK_EQ(log.head_abs(), 6u);
    CHECK_EQ(log.entries_dropped(), 2u);
    push(log, 9, 3, 0);
    CHECK_EQ(log.head_abs(), 9u);
    CHECK_EQ(log.entries_pushed(), 3u);
}

}  // namespace

int main() {
    test_live_read_counts_and_positions();
    test_live_read_survives_wrap_and_reports_drops();
    test_clear_keeps_the_counters_monotonic();
    test_entry_layout();
    test_value_edges();
    test_long_string_is_cut_and_null_is_empty();
    test_entry_is_capped();
    test_wrap_drops_whole_entries_and_tracks_the_base();
    test_snapshot_takes_the_newest_whole_entries_that_fit();
    test_delta_survives_a_32_bit_clock_wrap();
    test_clear();
    return test_report("test_binlog");
}
