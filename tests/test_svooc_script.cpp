// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The guarded SuperVOOC trigger's responder (k2/fastcharge/svooc_script.hpp): the adapter commands of three captured
// OnePlus 15 + SUPERVOOC 120 W sessions go in, the reply words the responder chose come out and are compared with what
// the phone answered. The tables are tools/vooc_edges.py's decode of the fixtures (tests/test_svooc_fixtures.py checks
// that they are the fixtures' content).
#include <cstdio>
#include <span>

#include "k2/fastcharge/svooc_script.hpp"
#include "test_util.h"

using namespace k2;

#define CHECK(c) CHECK_EQ(!!(c), 1)

namespace {

struct Pair { std::uint8_t cmd; std::uint16_t phone; };

// tests/fixtures/op15_svooc_entry.edges (cap1 session 1, 2026-11-11, USB-A, A-to-C cable): the handshake and the ramp.
const Pair kCap1[] = {
    {0xA8, 0x000}, {0xA8, 0x00D}, {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010},
    {0x98, 0x010}, {0x8A, 0x028}, {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B},
    {0x84, 0x071}, {0x84, 0x031}, {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048},
    {0x8A, 0x052}, {0x8C, 0x046}, {0x8C, 0x066}, {0x88, 0x04D},
    {0x94, 0x000}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010},
    {0x84, 0x010}, {0x84, 0x020}, {0x84, 0x030}, {0x86, 0x023}, {0x82, 0x04C},
};
// tests/fixtures/op15_svooc_replug.edges (cap1 session 2): the same handshake, ten 0x010 ramp steps.
const Pair kCap1Replug[] = {
    {0xA8, 0x000}, {0xA8, 0x00D}, {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010},
    {0x98, 0x010}, {0x8A, 0x028}, {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B},
    {0x84, 0x071}, {0x84, 0x031}, {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048},
    {0x8A, 0x052}, {0x8C, 0x046}, {0x8C, 0x066}, {0x88, 0x04D},
};
// tests/fixtures/op15_svooc_usbc_entry.edges (2026-11-12, another day, cable and battery level).
const Pair kUsbc[] = {
    {0xA8, 0x000}, {0xA8, 0x00D}, {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010},
    {0x98, 0x010}, {0x8A, 0x028}, {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B},
    {0x84, 0x071}, {0x84, 0x031}, {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048},
    {0x8A, 0x052}, {0x8C, 0x046}, {0x8C, 0x066}, {0x88, 0x04D},
    {0x94, 0x000}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010},
    {0x84, 0x010}, {0x84, 0x030}, {0x86, 0x02D}, {0x82, 0x051},
};
// tests/fixtures/oneplus15_svooc_init.edges (2026-11-02): the phone answered the FIRST ident.
const Pair kInit1102[] = {
    {0xA8, 0x00D}, {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010}, {0x98, 0x010},
    {0x8A, 0x028}, {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B}, {0x84, 0x071},
    {0x84, 0x031}, {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048}, {0x8A, 0x052},
    {0x8C, 0x046}, {0x8C, 0x066}, {0x88, 0x04D}, {0x94, 0x000},
};

// The handshake of a capture, up to and including 88 > 04D: the responder answers every command with the phone's
// word. The one exception is a silent first ident (0xA8 > 0x000, the phone not yet listening): the responder answers
// every ident with 0x00D, as the phone did on 2026-11-02 (kInit1102), so it never needs the adapter's retry.
void check_handshake(std::span<const Pair> cap) {
    svooc::Responder r;
    bool accepted_seen = false;
    for (const Pair& p : cap) {
        const svooc::Answer a = r.on_command(p.cmd);
        CHECK(a.on_script);
        const std::uint16_t want = (p.cmd == 0xA8 && p.phone == 0x000) ? 0x00D : p.phone;
        CHECK_EQ(a.word, want);
        if (a.word != want) std::printf("  cmd 0x%02X: 0x%03X, phone 0x%03X\n", p.cmd, a.word, p.phone);
        if (p.cmd == 0x88 && p.phone == 0x04D) { accepted_seen = true; break; }
        CHECK(!r.accepted());
    }
    CHECK(accepted_seen);
    CHECK(r.accepted());
    CHECK(!r.abort());
}

void test_handshake_matches_every_capture() {
    check_handshake(kCap1);
    check_handshake(kCap1Replug);
    check_handshake(kUsbc);
    check_handshake(kInit1102);
}

// After 88 > 04D. The ramp words follow the captures: 94 silent (all four accepted sessions), 0x84 answered 0x010 for
// the fewest steps any session took (7) and then the 0x030 every session ended the ramp with, 0x86 alternating 0x023 /
// 0x00F as in cap1, and every 0x82 poll the one fixed word kSteadyWord.
void test_ramp_and_steady_words() {
    svooc::Responder r;
    for (const Pair& p : kUsbc) {
        r.on_command(p.cmd);
        if (r.accepted()) break;
    }
    const struct { std::uint8_t cmd; std::uint16_t want; } after[] = {
        {0x94, 0x000}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010},
        {0x84, 0x010}, {0x84, 0x030}, {0x86, 0x023}, {0x82, 0x051}, {0x86, 0x00F}, {0x82, 0x051}, {0x86, 0x023},
        {0x82, 0x051}, {0x82, 0x051},
    };
    for (const auto& x : after) {
        const svooc::Answer a = r.on_command(x.cmd);
        CHECK(a.on_script);
        CHECK_EQ(a.word, x.want);
    }
    CHECK_EQ(svooc::kSteadyWord, 0x051);
    CHECK(!r.abort());
    // The USB-C capture after the handshake: every word but the data-bearing 0x86 one is the phone's.
    svooc::Responder u;
    int same = 0, total = 0;
    bool after_hs = false;
    for (const Pair& p : kUsbc) {
        const svooc::Answer a = u.on_command(p.cmd);
        if (after_hs) { ++total; if (a.word == p.phone) ++same; }
        after_hs = after_hs || u.accepted();
    }
    CHECK_EQ(total, 11);
    CHECK_EQ(same, 10);   // 94, 84 x8, the first 82 poll (0x051); not 86 > 02D (cap1 sent 023 there)
}

// An 0x84 after the ramp-ending 0x030 never happened in a capture: off-script.
void test_ramp_overrun_is_off_script() {
    svooc::Responder r;
    for (const Pair& p : kUsbc) { r.on_command(p.cmd); if (r.accepted()) break; }
    for (int i = 0; i < 8; ++i) CHECK(r.on_command(0x84).on_script);
    const svooc::Answer a = r.on_command(0x84);
    CHECK(!a.on_script);
    CHECK_EQ(a.word, svooc::kSilent);
}

// Off-script = a command the script has no word for at this point: answered silent; more than two in a row abort.
void test_off_script_run_aborts_on_the_third() {
    svooc::Responder r;
    CHECK(r.on_command(0xA8).on_script);
    svooc::Answer a = r.on_command(0x55);            // never seen
    CHECK(!a.on_script);
    CHECK_EQ(a.word, svooc::kSilent);
    CHECK(!r.abort());
    a = r.on_command(0x92);                          // a status command of the older adapters, never in this script
    CHECK(!a.on_script);
    CHECK(!r.abort());
    CHECK(r.on_command(0x98).on_script);             // back on the script: the run starts over
    CHECK_EQ(r.off_run(), 0);
    r.on_command(0x55);
    r.on_command(0x55);
    CHECK(!r.abort());                               // two in a row: still answering
    a = r.on_command(0x55);                          // the third: abort
    CHECK(!a.on_script);
    CHECK(r.abort());
}

// A command beyond its count (a fourth 0x88: the adapter polls the cable check it did not accept) is off-script, and
// so is an ident once the script has moved on.
void test_counts_are_part_of_the_key() {
    svooc::Responder r;
    for (const Pair& p : kInit1102) { r.on_command(p.cmd); if (r.accepted()) break; }
    CHECK(r.accepted());
    CHECK(!r.on_command(0x88).on_script);
    CHECK(!r.on_command(0x98).on_script);            // a handshake command after the handshake
    svooc::Responder s;
    CHECK(s.on_command(0xA8).on_script);
    CHECK(s.on_command(0x98).on_script);
    CHECK(!s.on_command(0xA8).on_script);            // an ident after the script started
    svooc::Responder t;
    for (int i = 0; i < 3; ++i) CHECK(t.on_command(0xA8).on_script);   // the adapter's retries before the script
    CHECK(!t.on_command(0xA8).on_script);
}

}  // namespace

int main() {
    test_handshake_matches_every_capture();
    test_ramp_and_steady_words();
    test_ramp_overrun_is_off_script();
    test_off_script_run_aborts_on_the_third();
    test_counts_are_part_of_the_key();
    return test_report("test_svooc_script");
}
