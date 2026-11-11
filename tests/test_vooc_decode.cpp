// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The inline VOOC frame decoder (k2/fastcharge/vooc_decode.hpp) against the two OnePlus 15 + SUPERVOOC 120 W slices of
// runs/2026-11-11-op15-svooc/cap1.edges that tests/test_vooc_edges.py decodes with tools/vooc_edges.py: the numbers below
// are the tool's summary of the same files, so the firmware and the tool share one set of framing rules.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "k2/fastcharge/edges.hpp"
#include "k2/fastcharge/vooc_decode.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

constexpr std::uint32_t kCycPerMs = vooc::kCyclesPerUs * 1000;

// Edge words (cycles & ~63 | dm << 1 | dp) of a raw capture file, in stream order.
std::vector<std::uint32_t> load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::uint32_t> w;
    for (std::size_t i = 0; i + 6 <= b.size();) {
        const std::size_t n = b[i];
        if (n < 6 || i + n > b.size()) break;
        if (b[i + 1] == 15) {
            edges::Edge e[64];
            std::uint8_t idx = 0;
            const std::size_t k = edges::decode_chunk({b.data() + i + 6, n - 6}, e, idx);
            for (std::size_t j = 0; j < k; ++j) w.push_back((e[j].units << 6) | e[j].state);
        }
        i += n;
    }
    return w;
}

struct Replay {
    vooc::Decoder d;
    int starts = 0, ends = 0;
    std::vector<vooc::Summary> ended;   // the summary as each session ended
    void count(unsigned ev) {
        if (ev & vooc::kSessionEnd) { ++ends; ended.push_back(d.summary()); }
        if (ev & vooc::kSessionStart) ++starts;
    }
    // Like the main loop: a tick every millisecond between the edges, then 1.5 s of silence at the end.
    void run(const std::vector<std::uint32_t>& w) {
        std::uint32_t t = w.empty() ? 0 : w[0] & ~63u;
        for (const std::uint32_t x : w) {
            const std::uint32_t at = x & ~63u;
            while (at - t > kCycPerMs) { t += kCycPerMs; count(d.tick(t)); }
            count(d.push(x));
            t = at;
        }
        for (int i = 0; i < 1500; ++i) { t += kCycPerMs; count(d.tick(t)); }
    }
};

const vooc::CmdStat* find(const vooc::Summary& s, std::uint8_t cmd) {
    for (std::size_t i = 0; i < s.ncmds; ++i) if (s.cmds[i].cmd == cmd) return &s.cmds[i];
    return nullptr;
}

struct Want { std::uint8_t cmd; std::uint16_t count, last; };

void check_cmds(const vooc::Summary& s, std::initializer_list<Want> want) {
    CHECK_EQ(int(s.ncmds), int(want.size()));
    for (const auto& x : want) {
        const auto* c = find(s, x.cmd);
        CHECK(c != nullptr);
        if (!c) { std::printf("  missing cmd 0x%02X\n", x.cmd); continue; }
        CHECK_EQ(c->count, x.count);
        CHECK_EQ(c->last, x.last);
    }
}

}  // namespace

// cap1 0-17.5 s: the phone's wake burst (40 clocks), the 539 ms low, the held-low ident, the 26-pair handshake, the
// steady 0x82 poll. tools/vooc_edges.py --summary: 1 wake, 1 session of 111 frames, 55 pairs, the histogram below.
static void test_entry_capture_matches_the_tool() {
    Replay r;
    r.run(load("tests/fixtures/op15_svooc_entry.edges"));
    CHECK_EQ(r.starts, 1);
    CHECK_EQ(r.ends, 1);
    const auto& s = r.d.summary();
    CHECK_EQ(int(s.sessions), 1);
    CHECK_EQ(int(s.wakes), 1);
    CHECK_EQ(int(s.partials), 1);
    CHECK_EQ(s.frames, 111u);
    CHECK(!s.active);
    CHECK(s.handshake);
    CHECK_EQ(int(s.ident), 0xA8);
    CHECK_EQ(int(s.ident_reply), 0x00D);
    CHECK_EQ(int(s.hs_pairs), 26);
    CHECK_EQ(s.pairs, 55u);
    check_cmds(s, {{0x80, 1, 0x010}, {0x82, 20, 0x052}, {0x84, 11, 0x030}, {0x86, 4, 0x00F}, {0x88, 3, 0x04D},
                   {0x8A, 3, 0x052}, {0x8C, 2, 0x066}, {0x8E, 1, 0x010}, {0x94, 2, 0x000}, {0x98, 4, 0x010},
                   {0x9A, 1, 0x030}, {0x9E, 1, 0x010}, {0xA8, 2, 0x00D}});
    CHECK_EQ(int(s.r82_n), 18);
    CHECK_EQ(int(s.r82_last), 0x052);
    CHECK_EQ(int(s.r82_min), 0x04C);
    CHECK_EQ(int(s.r82_max), 0x07F);
    // 16 steady 0x82 -> 0x82 gaps, mean 46.23 ms, the last 43.98 ms
    CHECK_EQ(int(s.p82_n), 16);
    CHECK(s.p82_mean_us() > 46100 && s.p82_mean_us() < 46400);
    CHECK(s.p82_last_us > 43900 && s.p82_last_us < 44100);
}

// cap1 57.5-63.6 s: the tail of a steady session (no ident in the slice), the unplug, the replug burst and a fresh
// handshake. The first session's last 0x82 reply is 0x1FF: D- floating high as the phone leaves.
static void test_replug_capture_has_two_sessions() {
    Replay r;
    r.run(load("tests/fixtures/op15_svooc_replug.edges"));
    CHECK_EQ(r.starts, 2);
    CHECK_EQ(r.ends, 2);
    CHECK_EQ(int(r.ended.size()), 2);
    if (r.ended.size() != 2) return;
    const auto& a = r.ended[0];
    CHECK(!a.handshake);
    CHECK_EQ(int(a.hs_pairs), 0);
    CHECK_EQ(a.pairs, 18u);
    check_cmds(a, {{0x82, 18, 0x1FF}});
    CHECK_EQ(int(a.r82_n), 18);
    CHECK_EQ(int(a.r82_min), 0x06A);
    CHECK_EQ(int(a.r82_max), 0x1FF);
    CHECK_EQ(int(a.p82_n), 17);
    CHECK(a.p82_mean_us() > 44100 && a.p82_mean_us() < 44300);
    const auto& b = r.ended[1];
    CHECK(b.handshake);
    CHECK_EQ(int(b.hs_pairs), 26);
    CHECK_EQ(b.pairs, 38u);
    CHECK_EQ(int(b.r82_n), 0);
    CHECK_EQ(int(b.r82_last), int(vooc::kNoReply));
    CHECK_EQ(int(b.p82_n), 0);
    const auto& s = r.d.summary();
    CHECK_EQ(int(s.sessions), 2);
    CHECK_EQ(int(s.wakes), 1);
    CHECK_EQ(s.frames, 113u);
}

// The same replay without the millisecond ticks (only the edges' own gaps close frames, pairs and sessions) except the
// final silence: the ticks change nothing but the moment a session is closed.
static void test_ticks_do_not_change_the_result() {
    const auto w = load("tests/fixtures/op15_svooc_entry.edges");
    vooc::Decoder d;
    for (const std::uint32_t x : w) d.push(x);
    std::uint32_t t = w.back() & ~63u;
    for (int i = 0; i < 3; ++i) d.tick(t += 600 * kCycPerMs);
    const auto& s = d.summary();
    CHECK_EQ(s.frames, 111u);
    CHECK_EQ(s.pairs, 55u);
    CHECK_EQ(int(s.hs_pairs), 26);
    CHECK_EQ(int(s.r82_n), 18);
    CHECK(!s.active);
}

// A session stays open while frames come less than kSessionGapMs apart and ends once the line stays quiet that long.
static void test_session_ends_after_one_second_of_silence() {
    const auto w = load("tests/fixtures/op15_svooc_entry.edges");
    vooc::Decoder d;
    unsigned ev = 0;
    for (const std::uint32_t x : w) ev |= d.push(x);
    CHECK(ev & vooc::kSessionStart);
    CHECK(d.summary().active);
    const std::uint32_t last = w.back() & ~63u;
    CHECK_EQ(d.tick(last + 900 * kCycPerMs) & vooc::kSessionEnd, 0u);
    CHECK(d.summary().active);
    CHECK(d.tick(last + 1100 * kCycPerMs) & vooc::kSessionEnd);
    CHECK(!d.summary().active);
    CHECK_EQ(d.tick(last + 1200 * kCycPerMs), 0u);   // once
    // The monitor stopping mid-session (inline gone, an engine took the lines) closes it at once, the last pair kept.
    vooc::Decoder e;
    for (const std::uint32_t x : w) e.push(x);
    CHECK(e.end(last) & vooc::kSessionEnd);
    CHECK(!e.summary().active);
    CHECK_EQ(e.summary().pairs, 55u);
}

// Silence longer than the 26-bit edge clock wraps (17.9 s) must not alias: the main loop's tick closes everything first.
static void test_long_silence_does_not_alias() {
    const auto w = load("tests/fixtures/op15_svooc_entry.edges");
    vooc::Decoder d;
    std::uint32_t t = 0;
    for (const std::uint32_t x : w) { d.push(x); t = x & ~63u; }
    for (int i = 0; i < 40; ++i) d.tick(t += 500 * kCycPerMs);   // 20 s of main-loop ticks
    for (const std::uint32_t x : w) d.push(x + t - (w[0] & ~63u) + 1000 * kCycPerMs);   // the same exchange again
    d.tick(t += 30000 * kCycPerMs);
    const auto& s = d.summary();
    CHECK_EQ(int(s.sessions), 2);
    CHECK_EQ(int(s.wakes), 2);
    CHECK_EQ(int(s.hs_pairs), 26);
    CHECK_EQ(s.pairs, 55u);
}

// Nothing but the idle levels (D+ high, D- low), or a lone D- change: no frame, no session.
static void test_idle_lines_are_no_session() {
    vooc::Decoder d;
    CHECK_EQ(d.push(edges::pack(1000, false, true)), 0u);
    CHECK_EQ(d.push(edges::pack(500000, true, true)), 0u);
    CHECK_EQ(d.push(edges::pack(900000, false, true)), 0u);
    CHECK_EQ(d.tick(900000 + 5000 * kCycPerMs), 0u);
    CHECK_EQ(int(d.summary().sessions), 0);
    CHECK_EQ(d.summary().frames, 0u);
}

// The command table keeps kMaxCmds distinct commands; the rest only count in `other`.
static void test_command_table_overflow_counts_other() {
    vooc::Decoder d;
    // Synthetic frames: a command every 20 ms, 9 pulses of 30 us low, 460 / 540 us apart; D- carries the bits.
    std::uint32_t t = 1000 * kCycPerMs;
    auto frame = [&](unsigned v9) {
        bool dm = false;
        std::uint32_t at = t;
        for (int k = 0; k < 9; ++k) {
            const bool bit = (v9 >> (8 - k)) & 1;
            // bit k is D- before pulse k+1 falls: set it 100 us after pulse k rises
            d.push(edges::pack(at, dm, false));
            d.push(edges::pack(at + 30 * vooc::kCyclesPerUs, dm, true));
            if (bit != dm) { dm = bit; d.push(edges::pack(at + 100 * vooc::kCyclesPerUs, dm, true)); }
            at += ((k % 2) ? 540 : 460) * vooc::kCyclesPerUs;
        }
        if (dm) d.push(edges::pack(at + 3000 * vooc::kCyclesPerUs, false, true));   // back to the idle 0
        t += 20 * kCycPerMs;
    };
    d.push(edges::pack(t - 5 * kCycPerMs, false, true));   // the idle levels first: the next fall is a pulse
    for (unsigned c = 0; c < vooc::kMaxCmds + 3; ++c) frame((0x80u + c) << 1);
    d.tick(t + 5000 * kCycPerMs);
    const auto& s = d.summary();
    CHECK_EQ(int(s.ncmds), int(vooc::kMaxCmds));
    CHECK_EQ(int(s.other), 3);
    CHECK_EQ(s.pairs, std::uint32_t(vooc::kMaxCmds + 3));
    CHECK(find(s, 0x80) && find(s, 0x80)->last == vooc::kNoReply);
}

// When the monitor may hold the lines: inline (a far sink draws through the K2), nothing else owns them, and the
// operator has not switched it off. A PD contract is no reason to stay off: the phone on the USB-C port holds a 5 V
// contract and runs SuperVOOC on D+/D- at the same time (hardware 2026-11-11), so the policy has no contract input.
static void test_monitor_policy() {
    using vooc::monitor_wanted;
    CHECK(monitor_wanted(true, true, true));
    CHECK(!monitor_wanted(false, true, true));    // switched off
    CHECK(!monitor_wanted(true, false, true));    // not inline: nothing draws through the K2
    CHECK(!monitor_wanted(true, true, false));    // an engine, the reset hold or another owner has the lines
}

// What the screen may say (Home `SVOOC`, the raw 0x82 note): only while the monitor runs and the session is live and
// proven VOOC (an ident the phone answered, or steady 0x82 replies), and never once it ended.
static void test_live_view() {
    vooc::Summary s;
    CHECK(!vooc::live_of(true, s).seen);
    CHECK_EQ(vooc::live_of(true, s).r82, vooc::kNoReply);
    s.active = true;
    CHECK(!vooc::live_of(true, s).seen);                // a session of wake bursts only proves nothing
    s.handshake = true;
    CHECK(vooc::live_of(true, s).seen);
    CHECK_EQ(vooc::live_of(true, s).r82, vooc::kNoReply);
    CHECK(!vooc::live_of(false, s).seen);               // the monitor stopped: the summary is history
    s.handshake = false; s.r82_n = 3; s.r82_last = 0x6B;
    CHECK(vooc::live_of(true, s).seen);                 // joined mid-charge: steady 0x82 replies, no handshake seen
    CHECK_EQ(vooc::live_of(true, s).r82, 0x6B);
    s.active = false;
    CHECK(!vooc::live_of(true, s).seen);                // ended
    CHECK_EQ(vooc::live_of(true, s).r82, vooc::kNoReply);
    // The entry capture: live from the handshake on, gone one second after the last frame.
    Replay r;
    bool seen_any = false;
    const auto w = load("tests/fixtures/op15_svooc_entry.edges");
    for (const std::uint32_t x : w) { r.count(r.d.push(x)); seen_any |= vooc::live_of(true, r.d.summary()).seen; }
    CHECK(seen_any);
    r.count(r.d.end(w.back() & ~63u));
    CHECK(!vooc::live_of(true, r.d.summary()).seen);
}

// The RAM the inline monitor costs: the decoder holds the summary; no edge buffer (it decodes in the EXTI ISR).
static void test_ram_budget() {
    std::printf("vooc::Decoder %zu bytes (Summary %zu)\n", sizeof(vooc::Decoder), sizeof(vooc::Summary));
    CHECK(sizeof(vooc::Decoder) <= 256);
}

int main() {
    test_ram_budget();
    test_entry_capture_matches_the_tool();
    test_replug_capture_has_two_sessions();
    test_ticks_do_not_change_the_result();
    test_session_ends_after_one_second_of_silence();
    test_long_silence_does_not_alias();
    test_idle_lines_are_no_session();
    test_command_table_overflow_counts_other();
    test_monitor_policy();
    test_live_view();
    return test_report("test_vooc_decode");
}
