// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The guarded SuperVOOC trigger's bit layer (k2/fastcharge/svooc_wire.hpp). The real edges of the captured sessions go
// in as the ISR would see them; the test checks that the wire decodes the adapter's commands, drives nothing in a
// command frame, and in each reply slot sets D- 50 us after every D+ rise to the bit the phone itself put on the line
// (sampled where the adapter latches it: just before the next fall, the last bit 300 us after the last rise).
// Synthetic frames cover the cases the captures do not: a clock that stops mid-reply, a missed reply slot, a frame of
// ten pulses, an abort.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "k2/fastcharge/edges.hpp"
#include "k2/fastcharge/svooc_wire.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

constexpr std::uint32_t kUs = svooc::kCyclesPerUs;
using Kind = svooc::Action::Kind;

// Edge words (cycles & ~63 | dm << 1 | dp) of a raw capture file, in stream order (as tests/test_vooc_decode.cpp).
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

// One reply slot as the replay saw it: the bits the wire set (in order) and the bits the phone left on D- where the
// adapter latches them.
struct Slot {
    std::uint8_t cmd = 0;
    std::vector<int> ours, phone;
    std::vector<std::uint32_t> rise, at, next_fall;
};

struct Replay {
    svooc::Wire w;
    std::vector<Slot> slots;
    int drives_outside_slots = 0, releases = 0;
    std::vector<std::uint8_t> cmds_seen;   // the command of each reply slot the wire opened

    void run(const std::vector<std::uint32_t>& words, std::size_t max_slots) {
        bool in_slot = false, dp = true, dm = false, have = false;
        std::uint32_t last_rise = 0;
        bool want_last = false;   // the phone's last bit: D- 300 us after the last rise
        for (const std::uint32_t x : words) {
            const std::uint32_t t = x & ~63u;
            const bool ndp = x & 1u, ndm = x & 2u;
            if (want_last && t - last_rise >= 300 * kUs) { slots.back().phone.push_back(dm); want_last = false; in_slot = false; }
            if (have && dp && !ndp && in_slot && !slots.back().rise.empty()) {   // a fall inside the slot: the phone's bit
                slots.back().phone.push_back(dm);
                slots.back().next_fall.push_back(t);
            }
            const svooc::Action a = w.edge(x);
            if (have && !dp && ndp && in_slot) { last_rise = t; slots.back().rise.push_back(t); }
            switch (a.kind) {
            case Kind::None: break;
            case Kind::Take:
                if (slots.size() == max_slots) return;
                slots.push_back({});
                slots.back().cmd = w.log().last_cmd;
                cmds_seen.push_back(w.log().last_cmd);
                in_slot = true;
                break;
            case Kind::Bit:
                if (!in_slot) ++drives_outside_slots;
                slots.back().ours.push_back(a.level);
                slots.back().at.push_back(a.at);
                if (slots.back().ours.size() == 9) want_last = true;
                break;
            case Kind::Release: ++releases; break;
            }
            dp = ndp; dm = ndm; have = true;
        }
    }
};

std::uint16_t word_of(const std::vector<int>& bits) {
    std::uint16_t v = 0;
    for (int b : bits) v = std::uint16_t(v << 1 | (b ? 1 : 0));
    return v;
}

// Each capture's handshake: 25 reply slots from the first ident to 88 > 04D. The wire decodes the same commands as the
// tool, sets each bit 50 us after its rise, always before the adapter latches it, and the words equal the phone's but
// for the silent first ident (the responder answers every ident).
void check_capture(const char* path, std::size_t slots, std::uint32_t from_ms = 0) {
    Replay r;
    std::vector<std::uint32_t> words = load(path);
    const std::uint32_t t0 = words.empty() ? 0 : words[0] & ~63u;
    std::erase_if(words, [&](std::uint32_t x) { return (x & ~63u) - t0 < from_ms * 1000 * kUs; });
    r.run(words, slots);
    CHECK_EQ(r.slots.size(), slots);
    CHECK_EQ(r.drives_outside_slots, 0);
    if (r.slots.size() < 23) {
        std::printf("  %s: %zu slots, commands %u missed %u partials %u\n", path, r.slots.size(),
                    unsigned(r.w.log().commands), unsigned(r.w.log().missed), unsigned(r.w.log().partials));
        return;
    }
    static const std::uint8_t kCmds[] = {0x98, 0x98, 0x9E, 0x80, 0x94, 0x98, 0x8A, 0x98, 0x8E, 0x9A, 0x82, 0x82,
                                         0x84, 0x84, 0x86, 0x86, 0x88, 0x88, 0x8A, 0x8A, 0x8C, 0x8C, 0x88};
    std::size_t first = r.slots.size() - 23;   // the slots before the 23 script commands are idents
    for (std::size_t i = 0; i < first; ++i) CHECK_EQ(r.slots[i].cmd, 0xA8);
    for (std::size_t i = 0; i < 23 && first + i < r.slots.size(); ++i) CHECK_EQ(r.slots[first + i].cmd, kCmds[i]);
    for (std::size_t i = 0; i < r.slots.size(); ++i) {
        const Slot& s = r.slots[i];
        CHECK_EQ(s.ours.size(), 9u);
        CHECK_EQ(s.phone.size(), 9u);
        if (s.ours.size() != 9 || s.phone.size() != 9) continue;
        const std::uint16_t ours = word_of(s.ours), phone = word_of(s.phone);
        if (s.cmd == 0xA8 && phone == 0) CHECK_EQ(ours, 0x00D);
        else CHECK_EQ(ours, phone);
        for (std::size_t k = 0; k < 9; ++k) CHECK_EQ(s.at[k], s.rise[k] + svooc::kBitDelayUs * kUs);
        for (std::size_t k = 0; k + 1 < 9 && k < s.next_fall.size(); ++k) CHECK(s.next_fall[k] - s.at[k] >= 300 * kUs);
    }
    CHECK(r.w.responder().accepted());
    CHECK_EQ(r.w.log().replies, slots);
}

void test_captures() {
    check_capture("tests/fixtures/op15_svooc_entry.edges", 25);        // A8 silent, A8, then the script
    check_capture("tests/fixtures/op15_svooc_replug.edges", 25, 4000);   // from the second plug-in's wake on
    check_capture("tests/fixtures/op15_svooc_usbc_entry.edges", 25);
    check_capture("tests/fixtures/oneplus15_svooc_init.edges", 24);    // the phone answered the first ident
}

// ---- synthetic adapter: 9 pulses 30 us wide, 460 / 540 us apart; D- driven by the adapter in a command frame
struct Synth {
    std::vector<std::uint32_t> words;
    bool dm = true;
    std::uint32_t t = 1000 * kUs;
    void edge(std::uint32_t at, bool dp, bool ndm) { words.push_back(edges::pack(at, ndm, dp)); dm = ndm; }
    // A command frame starting at `start`: D- carries bit k 28 us after fall k, then the trailing 0.
    std::uint32_t command(std::uint32_t start, std::uint8_t cmd, int pulses = 9) {
        std::uint32_t f = start;
        for (int k = 0; k < pulses; ++k) {
            edge(f, false, dm);
            const bool bit = k < 8 ? (cmd >> (7 - k)) & 1 : false;
            edge(f + 28 * kUs, false, bit);
            edge(f + 30 * kUs, true, bit);
            f += (k % 2 == 0 ? 460 : 540) * kUs;
        }
        return f;
    }
    // A reply slot's clock only (D- is the wire's).
    std::uint32_t clock(std::uint32_t start, int pulses = 9) {
        std::uint32_t f = start;
        for (int k = 0; k < pulses; ++k) {
            edge(f, false, dm);
            edge(f + 30 * kUs, true, dm);
            f += (k % 2 == 0 ? 460 : 540) * kUs;
        }
        return f;
    }
};

struct Feed {
    svooc::Wire w;
    int takes = 0, bits = 0, releases = 0;
    void all(const std::vector<std::uint32_t>& v) { for (auto x : v) count(w.edge(x)); }
    void count(const svooc::Action& a) {
        if (a.kind == Kind::Take) ++takes;
        if (a.kind == Kind::Bit) ++bits;
        if (a.kind == Kind::Release) ++releases;
    }
};

// One ident and its slot 9 ms later: the wire takes D- at the slot's first fall and sets nine bits; D- stays driven
// with the last bit until kHoldUs after the last rise (the phone held it 2.3-2.6 ms), then tick() lets it go.
void test_reply_slot_and_hold() {
    Synth s;
    s.edge(s.t, true, false);
    s.command(s.t + 5000 * kUs, 0xA8);
    const std::uint32_t slot = s.t + 14000 * kUs;
    const std::uint32_t end = s.clock(slot);
    Feed f;
    f.all(s.words);
    CHECK_EQ(f.takes, 1);
    CHECK_EQ(f.bits, 9);
    CHECK(f.w.driving());
    const std::uint32_t last_rise = end - 460 * kUs + 30 * kUs;   // the 9th pulse rose 30 us after its fall
    CHECK_EQ(f.w.tick(last_rise + (svooc::kHoldUs - 10) * kUs).kind, Kind::None);
    CHECK_EQ(f.w.tick(last_rise + svooc::kHoldUs * kUs).kind, Kind::Release);
    CHECK(!f.w.driving());
    CHECK_EQ(f.w.log().n, 1u);
    CHECK_EQ(f.w.log().pairs[0].cmd, 0xA8);
    CHECK_EQ(f.w.log().pairs[0].word, 0x00D);
}

// The adapter's next command comes before the hold ran out: the wire lets go at that command's first fall.
void test_next_command_releases() {
    Synth s;
    s.edge(s.t, true, false);
    s.command(s.t + 5000 * kUs, 0xA8);
    s.clock(s.t + 14000 * kUs);
    s.command(s.t + 19500 * kUs, 0x98);   // 1 ms after the slot's last rise, inside the hold
    Feed f;
    f.all(s.words);
    CHECK_EQ(f.releases, 1);
    CHECK(!f.w.driving());
}

// The clock stops after three pulses of the slot: tick() releases D- one ms after the last rise.
void test_clock_stops_mid_reply() {
    Synth s;
    s.edge(s.t, true, false);
    s.command(s.t + 5000 * kUs, 0xA8);
    const std::uint32_t end = s.clock(s.t + 14000 * kUs, 3);
    Feed f;
    f.all(s.words);
    CHECK(f.w.driving());
    CHECK_EQ(f.bits, 3);
    const std::uint32_t last_rise = end - 460 * kUs + 30 * kUs;
    CHECK_EQ(f.w.tick(last_rise + 900 * kUs).kind, Kind::None);
    CHECK_EQ(f.w.tick(last_rise + 1100 * kUs).kind, Kind::Release);
    CHECK(!f.w.driving());
    f.w.tick(last_rise + 2500 * kUs);   // the frame closes once the clock stayed quiet for kGroupGapUs
    CHECK_EQ(f.w.log().cut, 1u);
    CHECK_EQ(f.w.log().replies, 0u);
}

// No slot within kReplyWaitUs: the next frame is a command, the missed slot is counted, nothing is driven.
void test_missed_slot() {
    Synth s;
    s.edge(s.t, true, false);
    s.command(s.t + 5000 * kUs, 0xA8);
    s.command(s.t + 25000 * kUs, 0xA8);   // 20 ms after the first: a command, not our slot
    Feed f;
    f.all(s.words);
    CHECK_EQ(f.takes, 0);
    CHECK_EQ(f.w.log().missed, 1u);
    CHECK_EQ(f.w.log().n, 1u);
    CHECK_EQ(f.w.log().pairs[0].word, svooc::kNoWord);
}

// Ten pulses are no command (the wake burst is longer still); nor are eight.
void test_ten_pulses_are_no_command() {
    Synth s;
    s.edge(s.t, true, false);
    const std::uint32_t end = s.command(s.t + 5000 * kUs, 0xA8, 10);
    s.command(end + 5000 * kUs, 0xA8, 8);
    Feed f;
    f.all(s.words);
    f.w.tick(s.words.back() + 5000 * kUs);
    CHECK_EQ(f.takes, 0);
    CHECK_EQ(f.w.log().commands, 0u);
    CHECK_EQ(f.w.log().partials, 2u);
}

// Off-script commands: the first two are answered silent, the third in a row stops the wire unanswered, and nothing is
// driven after it; stop() lets go of a held D-.
void test_abort_and_stop() {
    Synth s;
    s.edge(s.t, true, false);
    std::uint32_t at = s.t + 5000 * kUs;
    for (int i = 0; i < 4; ++i) {
        s.command(at, 0x55);
        s.clock(at + 9000 * kUs);
        at += 30000 * kUs;
    }
    Feed f;
    f.all(s.words);
    CHECK(f.w.stopped());
    CHECK_EQ(f.takes, 2);
    CHECK_EQ(f.w.log().replies, 2u);
    CHECK_EQ(f.w.log().n, 4u);
    CHECK_EQ(f.w.log().pairs[0].word, svooc::kSilent);
    CHECK_EQ(f.w.log().pairs[1].word, svooc::kSilent);
    CHECK_EQ(f.w.log().pairs[2].word, svooc::kNoWord);
    CHECK_EQ(f.w.log().pairs[3].word, svooc::kNoWord);
    Feed g;
    Synth u;
    u.edge(u.t, true, false);
    u.command(u.t + 5000 * kUs, 0xA8);
    u.clock(u.t + 14000 * kUs, 4);
    g.all(u.words);
    CHECK(g.w.driving());
    CHECK_EQ(g.w.stop().kind, Kind::Release);
    CHECK(!g.w.driving());
    CHECK(g.w.stopped());
}

}  // namespace

int main() {
    test_captures();
    test_reply_slot_and_hold();
    test_next_command_releases();
    test_clock_stops_mid_reply();
    test_missed_slot();
    test_ten_pulses_are_no_command();
    test_abort_and_stop();
    return test_report("test_svooc_wire");
}
