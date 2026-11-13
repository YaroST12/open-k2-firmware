// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The guarded SuperVOOC trigger's responder (k2/fastcharge/svooc_script.hpp): the adapter commands of three captured
// OnePlus 15 + SUPERVOOC 120 W sessions go in, the reply words the responder chose come out and are compared with what
// the phone answered. The tables are tools/vooc_edges.py's decode of the fixtures (tests/test_svooc_fixtures.py checks
// that they are the fixtures' content).
#include <cstdio>
#include <span>

#include "k2/chargertest/svooc_ceiling.hpp"
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
// the fewest steps any session took (7) and then the 0x030 every session ended the ramp with, 0x86 answered the session
// current target (default kI86Default, about 0.5 A; the captures had 0x023 and 0x02D), and every 0x82 poll the one fixed
// word kSteadyWord.
void test_ramp_and_steady_words() {
    svooc::Responder r;
    for (const Pair& p : kUsbc) {
        r.on_command(p.cmd);
        if (r.accepted()) break;
    }
    const struct { std::uint8_t cmd; std::uint16_t want; } after[] = {
        {0x94, 0x000}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010}, {0x84, 0x010},
        {0x84, 0x010}, {0x84, 0x030}, {0x86, 0x005}, {0x82, 0x051}, {0x86, 0x005}, {0x82, 0x051}, {0x86, 0x005},
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

// A responder past the handshake and the ramp (94, eight 84 polls), with the given parameters.
svooc::Responder ramped(const svooc::Params& p = {}) {
    svooc::Responder r;
    r.configure(p);
    for (const Pair& q : kUsbc) { r.on_command(q.cmd); if (r.accepted()) break; }
    r.on_command(0x94);
    for (int i = 0; i < 8; ++i) r.on_command(0x84);
    return r;
}

// The post-accept 0x86 word is a parameter: default 0x005, clamped to 0x001..0x023. Every 0x86 is answered with it.
void test_i86_parameter_and_clamp() {
    CHECK_EQ(svooc::kI86Default, 0x005);
    svooc::Params d;
    CHECK_EQ(d.i86_first, 0x005);
    CHECK_EQ(d.r82, svooc::kSteadyWord);
    CHECK_EQ(d.vtarget_mv, 0);
    CHECK_EQ(d.esc_at_s, 0);
    svooc::Responder r = ramped();
    CHECK_EQ(r.on_command(0x86).word, 0x005);
    CHECK_EQ(r.on_command(0x86).word, 0x005);
    const struct { std::uint16_t asked, got; } cases[] = {{0x000, 0x001}, {0x001, 0x001}, {0x00F, 0x00F}, {0x023, 0x023},
                                                          {0x024, 0x023}, {0x1FF, 0x023}};
    for (const auto& c : cases) {
        svooc::Params p;
        p.i86_first = c.asked;
        svooc::Responder x = ramped(p);
        CHECK_EQ(x.on_command(0x86).word, c.got);
        CHECK_EQ(x.params().i86_first, c.got);   // the effective value is what params() reports
    }
}

// --r82: the constant 0x82 word, clamped to 0x000..0x07E (0x07F is the escape, reserved).
void test_r82_parameter_and_clamp() {
    const struct { std::uint16_t asked, got; } cases[] = {{0x000, 0x000}, {0x051, 0x051}, {0x07E, 0x07E}, {0x07F, 0x07E},
                                                          {0x1FF, 0x07E}};
    for (const auto& c : cases) {
        svooc::Params p;
        p.r82 = c.asked;
        svooc::Responder x = ramped(p);
        for (int i = 0; i < 3; ++i) CHECK_EQ(x.on_command(0x82).word, c.got);
    }
}

// The closed-loop rule (svooc_script.hpp): count(v) = round((v - 6918) / 20.35), the real phone's 0x82 word at 3.6 A;
// reply = clamp(count(v) + k, 0, 0x7E) with k = max(0, 0x7E - count(vtarget)): never below the phone's word, and the top
// of the scale (0x7E, "battery high") exactly when VBUS reaches the target.
void test_closed_loop_rule() {
    using svooc::vbus_count;
    CHECK_EQ(vbus_count(6918), 0);
    CHECK_EQ(vbus_count(8570), 81);      // 0x051 is 8567 mV on the fit
    CHECK_EQ(vbus_count(9087), 107);     // the capture's 0x06B at 9087 mV
    CHECK_EQ(vbus_count(5270), -81);     // below the fit's zero: negative, the caller clamps
    CHECK_EQ(svooc::closed_loop_reply(9000, 9000), 0x07E);
    CHECK_EQ(svooc::closed_loop_reply(9500, 9000), 0x07E);   // above the target stays at the top
    CHECK(svooc::closed_loop_reply(8500, 9000) < 0x07E);
    CHECK(svooc::closed_loop_reply(8500, 9000) > svooc::closed_loop_reply(8000, 9000));
    CHECK_EQ(svooc::closed_loop_reply(5270, 9000), 0);       // far below: clamps at 0
    // The word moves 1 per 20.35 mV: 20 steps of 20 mV are about 20 counts apart.
    const int span = svooc::closed_loop_reply(8800, 9000) - svooc::closed_loop_reply(8400, 9000);
    CHECK(span >= 19 && span <= 21);
    // A target above what the 7-bit word can say (count 0x7E = 9482 mV): k = 0, the phone's own word, saturating.
    CHECK_EQ(svooc::closed_loop_reply(9087, 11000), 107);
    CHECK_EQ(svooc::closed_loop_reply(11000, 11000), 0x07E);
    // The responder uses the measured VBUS of each poll; without a reading (0) it falls back to r82.
    svooc::Params p;
    p.vtarget_mv = 9000;
    svooc::Responder r = ramped(p);
    CHECK_EQ(r.on_command(0x82, 0, 0).word, svooc::kSteadyWord);
    const std::uint16_t a = r.on_command(0x82, 0, 8000).word;
    const std::uint16_t b = r.on_command(0x82, 0, 8400).word;
    const std::uint16_t c = r.on_command(0x82, 0, 9000).word;
    CHECK(a < b);
    CHECK(b < c);
    CHECK_EQ(c, 0x07E);
    CHECK_EQ(a, svooc::closed_loop_reply(8000, 9000));
}

// The vtarget clamp: nonzero targets are held to 11000 mV at most (below the 12 V guard); 0 = closed loop off.
void test_vtarget_clamp() {
    svooc::Params p;
    p.vtarget_mv = 15000;
    CHECK_EQ(p.clamped().vtarget_mv, 11000);
    p.vtarget_mv = 11000;
    CHECK_EQ(p.clamped().vtarget_mv, 11000);
    p.vtarget_mv = 9000;
    CHECK_EQ(p.clamped().vtarget_mv, 9000);
    p.vtarget_mv = 0;
    CHECK_EQ(p.clamped().vtarget_mv, 0);
    svooc::Params q;
    q.vtarget_mv = 30000;
    svooc::Responder r = ramped(q);
    CHECK_EQ(r.params().vtarget_mv, 11000);
}

// The escape: after esc_at_s seconds one 0x82 poll is answered 0x07F; the charger then sends 0x86 on its next command
// and that one is answered i86_esc (clamped like i86_first); the first 0x86 answers stay i86_first before it.
void test_escape_provokes_a_fresh_0x86() {
    svooc::Params p;
    p.esc_at_s = 3;
    p.i86_first = 0x005;
    p.i86_esc = 0x00F;
    svooc::Responder r = ramped(p);
    CHECK(r.escape() == svooc::Escape::Armed);
    CHECK_EQ(r.on_command(0x86, 1000).word, 0x005);
    CHECK_EQ(r.on_command(0x82, 2999).word, svooc::kSteadyWord);   // not yet
    CHECK(r.escape() == svooc::Escape::Armed);
    CHECK_EQ(r.on_command(0x82, 3000).word, svooc::kEscapeWord);
    CHECK(r.escape() == svooc::Escape::Sent);
    const svooc::Answer a = r.on_command(0x86, 3044);
    CHECK(a.on_script);
    CHECK_EQ(a.word, 0x00F);
    CHECK(r.escape() == svooc::Escape::Got86);
    CHECK_EQ(r.on_command(0x82, 3088).word, svooc::kSteadyWord);   // once only
    CHECK_EQ(r.on_command(0x82, 9000).word, svooc::kSteadyWord);
    CHECK_EQ(r.on_command(0x86, 9044).word, 0x00F);                // the target stays the new one
    CHECK(r.escape() == svooc::Escape::Got86);
    CHECK_EQ(r.escape_t_ms(), 3000u);
}

// No 0x86 after the escape: recorded as such, the next 0x86 (when it comes) still takes the new target; nothing aborts.
void test_escape_without_0x86() {
    svooc::Params p;
    p.esc_at_s = 1;
    p.i86_esc = 0x040;   // clamped to 0x023
    svooc::Responder r = ramped(p);
    CHECK_EQ(r.on_command(0x82, 1000).word, svooc::kEscapeWord);
    CHECK_EQ(r.on_command(0x82, 1044).word, svooc::kSteadyWord);   // the charger polled 0x82 again: no 0x86
    CHECK(r.escape() == svooc::Escape::NoGet86);
    CHECK(!r.abort());
    CHECK_EQ(r.on_command(0x86, 1100).word, 0x023);
}

// Off by default and in the handshake: esc_at_s = 0 never escapes; an 0x82 poll of the handshake (answered 074 / 00B) is
// no escape point even past the time.
void test_escape_off_and_handshake() {
    svooc::Responder r = ramped();
    CHECK(r.escape() == svooc::Escape::Off);
    CHECK_EQ(r.on_command(0x82, 60000).word, svooc::kSteadyWord);
    svooc::Params p;
    p.esc_at_s = 1;
    svooc::Responder h;
    h.configure(p);
    for (const Pair& q : kUsbc) {
        const svooc::Answer a = h.on_command(q.cmd, 5000);
        CHECK(a.word != svooc::kEscapeWord);
        if (h.accepted()) break;
    }
}

// The escape word wins over the closed loop for the one poll.
void test_escape_over_closed_loop() {
    svooc::Params p;
    p.esc_at_s = 1;
    p.vtarget_mv = 9000;
    svooc::Responder r = ramped(p);
    CHECK_EQ(r.on_command(0x82, 1500, 8000).word, svooc::kEscapeWord);
    CHECK_EQ(r.on_command(0x86, 1544, 8100).word, 0x005);
    CHECK_EQ(r.on_command(0x82, 1588, 8200).word, svooc::closed_loop_reply(8200, 9000));
}

// --- the phone profile (Params::profile = Phone): the post-accept words of a OnePlus 15 session, from its own passive
// capture (reverse-engineered, 2026-11-12, 10 % battery): 94 silent, 84 > 020 x4 then 030, 86 > 06D at the ramp end, 82
// polls of a slowly moving word (0x025 x11, 0x026 x10, 0x029 x12, 0x028 x11, ...), the first escape on the 12th 82 poll
// (about 0.52 s after the ramp-end 86) and answered 86 > 00F, the second 34 polls later (about 1.5 s) answered 86 > 06D.

// A responder past the handshake with the profile set; the ramp itself is run by the caller.
svooc::Responder handshaken(svooc::Profile pr, std::uint16_t esc = 0x06D) {
    svooc::Params p;
    p.profile = pr;
    p.i86_esc = esc;
    svooc::Responder r;
    r.configure(p);
    for (const Pair& q : kUsbc) { r.on_command(q.cmd); if (r.accepted()) break; }
    return r;
}

void test_phone_ramp_words() {
    svooc::Responder r = handshaken(svooc::Profile::Phone);
    CHECK(r.accepted());
    CHECK_EQ(r.on_command(0x94, 1517).word, 0x000);
    for (int i = 0; i < 4; ++i) CHECK_EQ(r.on_command(0x84, 1600 + 34 * i).word, 0x020);
    CHECK_EQ(r.on_command(0x84, 1728).word, 0x030);
    CHECK(!r.on_command(0x84, 1800).on_script);       // the ramp ended
    const svooc::Answer a = r.on_command(0x86, 1771);
    CHECK(a.on_script);
    CHECK_EQ(a.word, 0x06D);                          // the phone asks its top, 10.9 A, at the ramp end
    CHECK_EQ(r.asks().size(), 1u);
    CHECK_EQ(r.asks()[0].word, 0x06D);
    CHECK_EQ(r.asks()[0].t_ms, 1771);
}

// The 0x82 words of the phone's capture (10 % battery, runs of the same word, the escape polls not counted), the first
// five runs: 0x025 x11, 0x026 x10, 0x029 x12, 0x028 x11 around the escapes.
void test_phone_82_sequence_and_escapes() {
    svooc::Responder r = handshaken(svooc::Profile::Phone);
    r.on_command(0x94);
    for (int i = 0; i < 4; ++i) r.on_command(0x84);
    r.on_command(0x84);
    r.on_command(0x86, 1771);
    std::uint32_t t = 1814;
    const auto poll = [&] { const svooc::Answer a = r.on_command(0x82, t); t += 43; return a.word; };
    for (int i = 0; i < 11; ++i) CHECK_EQ(poll(), 0x025);
    CHECK_EQ(poll(), svooc::kEscapeWord);             // the 12th poll, 0.52 s after the ramp-end 86
    CHECK(r.escape() == svooc::Escape::Sent);
    CHECK_EQ(r.on_command(0x86, t).word, 0x00F);      // the phone's first re-ask
    CHECK(r.escape() == svooc::Escape::Got86);
    t += 43;
    for (int i = 0; i < 10; ++i) CHECK_EQ(poll(), 0x026);
    for (int i = 0; i < 12; ++i) CHECK_EQ(poll(), 0x029);
    // 11 + 1 + 10 + 12 = 34 polls so far: 11 more normal polls (0x028) and the second escape is the 34th after the first.
    for (int i = 0; i < 11; ++i) CHECK_EQ(poll(), 0x028);
    CHECK_EQ(poll(), svooc::kEscapeWord);
    CHECK_EQ(r.on_command(0x86, t).word, 0x06D);      // the second ask goes back up to the top
    CHECK_EQ(r.asks().size(), 3u);
    CHECK_EQ(r.asks()[1].word, 0x00F);
    CHECK_EQ(r.asks()[2].word, 0x06D);
    CHECK(!r.abort());
}

// Past the table the word holds; the session never leaves the script however long it runs (the fixed profile was cut
// at 72 pairs).
void test_phone_session_runs_long() {
    svooc::Responder r = handshaken(svooc::Profile::Phone);
    r.on_command(0x94);
    for (int i = 0; i < 5; ++i) r.on_command(0x84);
    r.on_command(0x86, 1771);
    std::uint32_t t = 1814;
    std::uint16_t prev = 0;
    int escapes = 0;
    for (int i = 0; i < 700; ++i, t += 43) {
        const svooc::Answer a = r.on_command(0x82, t);
        CHECK(a.on_script);
        if (a.word == svooc::kEscapeWord) {
            ++escapes;
            CHECK(r.on_command(0x86, t + 9).on_script);
            continue;
        }
        CHECK(a.word <= svooc::kR82Max);
        prev = a.word;
    }
    CHECK(!r.abort());
    CHECK_EQ(escapes, 2);                             // the ladder is two asks long; nothing escapes after that
    CHECK(prev >= 0x056);
}

// The Probe profile asks upward after the phone's own steps so the highest accepted level shows: 00F at the ramp end,
// then 028, 03F, 06D, each on its own escape.
void test_probe_ladder_and_ceiling() {
    svooc::Responder r = handshaken(svooc::Profile::Probe);
    r.on_command(0x94);
    for (int i = 0; i < 5; ++i) r.on_command(0x84);
    CHECK_EQ(r.on_command(0x86, 1771).word, 0x00F);
    std::uint32_t t = 1814;
    int escapes = 0;
    for (int i = 0; i < 400 && escapes < 3; ++i, t += 43) {
        if (r.on_command(0x82, t).word == svooc::kEscapeWord) {
            ++escapes;
            r.on_command(0x86, t + 9);
        }
    }
    CHECK_EQ(escapes, 3);
    CHECK_EQ(r.asks().size(), 4u);
    const std::uint16_t want[] = {0x00F, 0x028, 0x03F, 0x06D};
    for (std::size_t i = 0; i < 4; ++i) CHECK_EQ(r.asks()[i].word, want[i]);
    // Four 82 polls after an ask on the script = the session went on with that ceiling: accepted. The last ask has none yet.
    CHECK_EQ(r.ceiling_word(), 0x03F);
    CHECK(!k2::chargertest::svooc_ceiling::ladder_judged(r.asks()));   // the charger test's stage waits for the last rung
    for (int i = 0; i < 4; ++i) { r.on_command(0x82, t); t += 43; }
    CHECK_EQ(r.ceiling_word(), 0x06D);
    CHECK(k2::chargertest::svooc_ceiling::ladder_judged(r.asks()));    // all four rungs asked, the top one stood: judged
    CHECK_EQ(r.asks().size(), k2::chargertest::svooc_ceiling::kLadder);
}

void test_ceiling_decode() {
    CHECK_EQ(svooc::ask_ma(0x00F), 1500);             // 100 mA per count, confirmed on measured current
    CHECK_EQ(svooc::ask_ma(0x028), 4000);
    CHECK_EQ(svooc::ask_ma(0x03F), 6300);
    CHECK_EQ(svooc::ask_ma(0x06D), 10900);
    CHECK_EQ(svooc::ask_ma(0), 0);
    // The fixed profile keeps its old words and clamp, and records its asks too.
    svooc::Params p;
    p.i86_first = 0x06D;
    CHECK_EQ(p.clamped().i86_first, 0x023);
    svooc::Responder r = ramped();
    r.on_command(0x86, 1771);
    CHECK_EQ(r.asks().size(), 1u);
    CHECK_EQ(r.asks()[0].word, 0x005);
    CHECK_EQ(r.ceiling_word(), 0);                    // nothing followed it yet
}

// The phone profiles lift the 0x86 clamp to the phone's top (0x06D); anything above is held there.
void test_profile_clamp() {
    svooc::Params p;
    p.profile = svooc::Profile::Phone;
    p.i86_first = 0x1FF;
    p.i86_esc = 0x050;
    CHECK_EQ(p.clamped().i86_first, 0x06D);
    CHECK_EQ(p.clamped().i86_esc, 0x050);
    svooc::Responder r = handshaken(svooc::Profile::Phone, 0x03F);
    r.on_command(0x94);
    for (int i = 0; i < 5; ++i) r.on_command(0x84);
    r.on_command(0x86, 1771);
    std::uint32_t t = 1814;
    int n = 0;
    while (n < 2 && t < 20000) {
        if (r.on_command(0x82, t).word == svooc::kEscapeWord) { r.on_command(0x86, t + 9); ++n; }
        t += 43;
    }
    CHECK_EQ(r.asks().size(), 3u);
    CHECK_EQ(r.asks()[2].word, 0x03F);                // the configurable ceiling ask
}

}  // namespace

int main() {
    test_phone_ramp_words();
    test_phone_82_sequence_and_escapes();
    test_phone_session_runs_long();
    test_probe_ladder_and_ceiling();
    test_ceiling_decode();
    test_profile_clamp();
    test_i86_parameter_and_clamp();
    test_r82_parameter_and_clamp();
    test_closed_loop_rule();
    test_vtarget_clamp();
    test_escape_provokes_a_fresh_0x86();
    test_escape_without_0x86();
    test_escape_off_and_handshake();
    test_escape_over_closed_loop();
    test_handshake_matches_every_capture();
    test_ramp_and_steady_words();
    test_ramp_overrun_is_off_script();
    test_off_script_run_aborts_on_the_third();
    test_counts_are_part_of_the_key();
    return test_report("test_svooc_script");
}
