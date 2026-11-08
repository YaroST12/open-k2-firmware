// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// OPPO VOOC / SuperVOOC phone-side slave, pure: the reply table and the bit-level slave driven by a
// fake adapter clock that follows the framing measured on the OnePlus 15 + SUPERVOOC 120 W capture.
#include <vector>

#include "k2/fastcharge/vooc.hpp"
#include "test_util.h"

using namespace k2;
using vooc::Mode;

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
                if (t - pulses[i].fall > 3000) return 0;     // well after the frame: back to the idle level
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
        for (std::size_t i = 0; i < pulses.size(); ++i)
            if (pulses[i].k == 0 && pulses[i].fall >= now) return goto_fall(i, timeout);
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

static void test_golden_replay_through_the_slave() {
    vooc::Responder r(Mode::SuperVooc5A);
    FakeAdapter a;
    std::int64_t t = 1000;
    int n = 0;
    for (const auto& g : kGolden) {
        a.add_frame(t, g.cmd);          // command frame
        a.add_frame(t + 9000, -1);      // the reply slot, 9 ms later
        a.now = t - 500;                // the slave starts listening during the idle gap
        const int f = vooc::rx_command(a, 5000);
        CHECK_EQ(f, g.cmd << 1);
        const auto w = r.reply(vooc::frame_cmd(static_cast<std::uint16_t>(f)));
        CHECK(w.has_value());
        CHECK(vooc::tx_reply(a, *w));
        CHECK_EQ(a.latch_reply(2 * n + 1), g.reply);
        CHECK_EQ(*w, g.reply);
        a.drive.push_back({a.now, -1});
        t += 30000; ++n;
    }
    CHECK_EQ(vooc::Responder::accepts(0x88, 0x04D), true);
    CHECK_EQ(vooc::Responder::accepts(0x88, 0x00D), false);
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

static void test_table_per_command_and_accept_rule() {
    vooc::Responder r(Mode::SuperVooc65A);
    CHECK_EQ(r.reply(0x88).value(), 0x05D);
    CHECK_EQ(r.reply(0x88).value(), 0x042);
    CHECK_EQ(r.reply(0x88).value(), 0x04D);   // the third cable check is the accepted one
    CHECK_EQ(r.reply(0x88).value(), 0x04D);
    CHECK_EQ(r.reply(0x82).value(), 0x074);
    CHECK_EQ(r.reply(0x82).value(), 0x00B);
    CHECK_EQ(r.reply(0x82).value(), 0x035);
    CHECK_EQ(r.reply(0x82).value(), 0x035);   // steady-state poll
    CHECK(!r.reply(0xAC).has_value());
    CHECK(!r.reply(0x00).has_value());
    r.reset();
    CHECK_EQ(r.reply(0x88).value(), 0x05D);
}

static void test_vooc_4a_ident_only() {
    vooc::Responder r(Mode::Vooc);
    CHECK_EQ(r.reply(0xA8).value(), 0x00D);
    CHECK_EQ(r.reply(0xAC).value(), 0x00D);
    CHECK(!r.reply(0x98).has_value());
}

int main() {
    test_golden_replay_through_the_slave();
    test_rx_samples_before_the_next_fall();
    test_silent_adapter_gives_no_frame_and_no_drive();
    test_reply_bits_follow_the_rise_by_50us();
    test_clock_stopping_mid_reply_releases_d_minus();
    test_no_reply_slot_releases_d_minus();
    test_table_per_command_and_accept_rule();
    test_vooc_4a_ident_only();
    return test_report("test_vooc");
}
