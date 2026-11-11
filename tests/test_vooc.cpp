// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OPPO VOOC / SuperVOOC phone-side slave, pure: the frame-start detector, the ident test and the bit-level slave driven
// by a fake adapter clock that follows the framing measured on the OnePlus 15 + SUPERVOOC 120 W captures.
#include <vector>

#include "k2/fastcharge/vooc.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

// Command -> reply pairs of the accepted init attempt (61.57 s) of build/sn/handshake2.edges, as decoded by
// tools/vooc_edges.py; the same list is the content of tests/fixtures/oneplus15_svooc_init.edges.
struct Pair { std::uint8_t cmd; std::uint16_t reply; };
static const Pair kGolden[] = {
    {0xA8, 0x00D}, {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010}, {0x98, 0x010},
    {0x8A, 0x028}, {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B}, {0x84, 0x071},
    {0x84, 0x031}, {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048}, {0x8A, 0x052},
    {0x8C, 0x046}, {0x8C, 0x066}, {0x88, 0x04D},
};

// The adapter side in microseconds. Frames are 9 pulses, 30 us wide, 460 / 540 us apart; D- is the adapter's
// in a command frame (changes 28 us after each fall, idle 0) and the phone's in the reply slot.
struct Pulse { std::int64_t fall, rise; int k; int frame; };
struct FakeAdapter {
    std::vector<Pulse> pulses;
    std::vector<std::vector<int>> cmd_bits;      // per frame: the 9 bits the adapter drives, empty = reply slot
    std::vector<std::pair<std::int64_t, int>> drive;   // phone drive events: (time, -1 release / 0 / 1)
    std::int64_t now = 0;
    int cur = -1;                                // index of the pulse the slave last waited on
    int last_before_fall = 0;
    bool stop_clock_after = false;               // test knob: drop the clock after the 3rd pulse of a reply slot

    void add_frame(std::int64_t start, int cmd) {   // cmd < 0: reply slot
        const int f = static_cast<int>(cmd_bits.size());
        std::vector<int> bits;
        if (cmd >= 0) for (int i = 7; i >= 0; --i) bits.push_back((cmd >> i) & 1);
        if (cmd >= 0) bits.push_back(0);
        cmd_bits.push_back(bits);
        std::int64_t t = start;
        for (int k = 0; k < vooc::kFrameBits; ++k) {
            pulses.push_back({t, t + 30, k, f});
            t += (k % 2 == 0) ? 460 : 540;
        }
    }
    int adapter_dm(std::int64_t t) const {
        for (int i = static_cast<int>(pulses.size()) - 1; i >= 0; --i) {
            if (pulses[i].fall + 28 <= t) {
                const auto& b = cmd_bits[pulses[i].frame];
                // inside a command frame the bit of pulse k appears 28 us after its fall and stays through the gap
                if (b.empty()) return 0;
                if (t - pulses[i].rise > 3000) return 0;     // well after the frame: back to the idle level
                return b[pulses[i].k];
            }
        }
        return 0;
    }
    int dm_at(std::int64_t t) const {
        int d = -1;
        for (const auto& e : drive) if (e.first <= t) d = e.second;
        return d >= 0 ? d : adapter_dm(t);
    }
    // --- vooc::Hw
    bool wait_frame(std::uint32_t timeout) {
        for (std::size_t i = 0; i < pulses.size(); ++i) {
            if (pulses[i].k != 0) continue;
            if (pulses[i].fall >= now) return goto_fall(i, timeout);
            // D+ held low since long before the frame (the first ident after the wake burst): it starts at the rise
            if (pulses[i].rise >= now && now - pulses[i].fall >= vooc::kFrameGapUs) {
                if (pulses[i].rise - now > timeout) { now += timeout; return false; }
                now = pulses[i].rise;
                cur = static_cast<int>(i);
                return true;
            }
        }
        now += timeout;
        return false;
    }
    bool goto_fall(std::size_t i, std::uint32_t timeout) {
        if (pulses[i].fall - now > timeout) { now += timeout; return false; }
        last_before_fall = dm_at(pulses[i].fall - 1);
        now = pulses[i].fall;
        cur = static_cast<int>(i);
        return true;
    }
    bool wait_fall(std::uint32_t timeout) {
        const std::size_t n = static_cast<std::size_t>(cur + 1);
        if (n >= pulses.size() || pulses[n].k == 0 || (stop_clock_after && pulses[n].k >= 3 && pulses[n].frame % 2 == 1)) {
            now += timeout;
            return false;
        }
        return goto_fall(n, timeout);
    }
    bool wait_rise(std::uint32_t timeout) {
        if (cur < 0) return false;
        if (pulses[cur].rise - now > timeout) { now += timeout; return false; }
        now = std::max(now, pulses[cur].rise);
        return true;
    }
    bool dm_before_fall() const { return last_before_fall; }
    bool dm() const { return dm_at(now) == 1; }
    void delay_us(std::uint32_t n) { now += n; }
    void drive_dm(bool level) { drive.push_back({now, level ? 1 : 0}); }
    void release_dm() { drive.push_back({now, -1}); }
    // What the adapter latches in a reply slot: D- just before each next fall, the last bit 300 us after the last pulse.
    int latch_reply(int frame) const {
        int v = 0;
        std::vector<const Pulse*> p;
        for (const auto& x : pulses) if (x.frame == frame) p.push_back(&x);
        for (std::size_t k = 0; k + 1 < p.size(); ++k) v = v << 1 | dm_at(p[k + 1]->fall - 1);
        return v << 1 | dm_at(p.back()->rise + 300);
    }
};

// The bit-level slave against the captured session: every command decodes, and a reply word driven by tx_reply (the lab
// `vooc answer` op; the engine itself answers nothing) is the word the adapter latches.
static void test_golden_replay_through_the_slave() {
    FakeAdapter a;
    std::int64_t t = 1000;
    int n = 0;
    for (const auto& g : kGolden) {
        a.add_frame(t, g.cmd);          // command frame
        a.add_frame(t + 9000, -1);      // the reply slot, 9 ms later
        a.now = t - 500;                // the slave starts listening during the idle gap
        const int f = vooc::rx_command(a, 5000);
        CHECK_EQ(f, g.cmd << 1);
        CHECK(vooc::tx_reply(a, g.reply));
        CHECK_EQ(a.latch_reply(2 * n + 1), g.reply);
        a.drive.push_back({a.now, -1});
        t += 30000; ++n;
    }
}

// cap1 (2026-11-11): after the phone's wake burst D+ stays low for 539 ms, and the adapter's first ident starts by raising
// D- then D+: pulse 1 is only a rise. The old frame start (D+ high 0.7 ms, then a fall) missed it and caught the reply slot.
static void test_rx_frame_starting_with_d_plus_held_low() {
    FakeAdapter a;
    a.add_frame(600000, 0xA8);
    a.pulses[0].fall = 61000;           // D+ low since the end of the burst
    a.now = 100000;                     // listening during the low
    CHECK_EQ(vooc::rx_command(a, 600000), 0xA8 << 1);
}

// The frame-start detector the firmware runs over D+ samples: either edge after D+ held one level for kFrameGapUs.
static void test_frame_start_detector() {
    using vooc::FrameStart;
    {   // idle high, then pulse 1 falls
        FrameStart s;
        CHECK(!s.feed(0, true)); CHECK(!s.feed(699, true)); CHECK(s.feed(800, false));
    }
    {   // held low for 539 ms, then pulse 1 rises
        FrameStart s;
        CHECK(!s.feed(0, false)); CHECK(!s.feed(539000, false)); CHECK(s.feed(539460, true));
    }
    {   // inside a frame: 30 us lows, 460 / 540 us highs: never a start
        FrameStart s;
        bool any = s.feed(0, false);
        std::uint32_t t = 0;
        for (int k = 0; k < 9; ++k) {
            any |= s.feed(t += 30, true);
            any |= s.feed(t += (k % 2 ? 540 : 460), false);
        }
        CHECK(!any);
    }
    {   // a level seen for less than the gap since listening began does not count, whatever came before
        FrameStart s;
        CHECK(!s.feed(0, false)); CHECK(!s.feed(500, true));
    }
}

// What the engine counts: an all-ones frame is D- floating high under an adapter clock nobody answers, i.e. nothing heard;
// the idents are 0xA8 (shared by VOOC / SVOOC / SVOOC65) and 0xAC / 0xA6 (VOOC).
static void test_heard_and_ident() {
    CHECK(!vooc::heard(-1));
    CHECK(!vooc::heard(0x1FF));
    CHECK(vooc::heard(0xA8 << 1));
    CHECK(vooc::heard(0x000));
    CHECK(vooc::is_ident(0xA8) && vooc::is_ident(0xAC) && vooc::is_ident(0xA6));
    CHECK(!vooc::is_ident(0x98) && !vooc::is_ident(0xFF) && !vooc::is_ident(0x82));
}

static void test_rx_samples_before_the_next_fall() {
    FakeAdapter a;
    a.add_frame(1000, 0xA8);   // 1010 1000 + trailing 0: a sample 1 us after a fall would read the previous bit
    a.now = 0;
    CHECK_EQ(vooc::rx_command(a, 5000), 0xA8 << 1);
}

static void test_silent_adapter_gives_no_frame_and_no_drive() {
    FakeAdapter a;
    CHECK_EQ(vooc::rx_command(a, 5000), -1);
    CHECK(a.drive.empty());
}

static void test_reply_bits_follow_the_rise_by_50us() {
    FakeAdapter a;
    a.add_frame(100, -1);
    a.now = 0;
    CHECK(vooc::tx_reply(a, 0x1FF));
    // first event: D- high at once; then one drive per bit at rise + 50 us; the last event releases
    CHECK_EQ(a.drive.front().second, 1);
    CHECK_EQ(a.drive[1].first, a.pulses[0].rise + vooc::kReplyDelayUs);
    CHECK_EQ(a.drive.back().second, -1);
}

static void test_clock_stopping_mid_reply_releases_d_minus() {
    FakeAdapter a;
    a.stop_clock_after = true;
    a.add_frame(100, 0x00);
    a.add_frame(9100, -1);
    a.now = 0;
    CHECK(vooc::rx_command(a, 5000) >= 0);
    CHECK(!vooc::tx_reply(a, 0x04D));
    CHECK_EQ(a.drive.back().second, -1);
}

static void test_no_reply_slot_releases_d_minus() {
    FakeAdapter a;
    CHECK(!vooc::tx_reply(a, 0x00D));
    CHECK_EQ(a.drive.back().second, -1);
}

int main() {
    test_golden_replay_through_the_slave();
    test_rx_samples_before_the_next_fall();
    test_silent_adapter_gives_no_frame_and_no_drive();
    test_reply_bits_follow_the_rise_by_50us();
    test_clock_stopping_mid_reply_releases_d_minus();
    test_no_reply_slot_releases_d_minus();
    test_rx_frame_starting_with_d_plus_held_low();
    test_frame_start_detector();
    test_heard_and_ident();
    return test_report("test_vooc");
}
