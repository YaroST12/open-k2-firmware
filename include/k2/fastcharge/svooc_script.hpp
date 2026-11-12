// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Guarded SuperVOOC trigger, part 1 of 3: the responder, pure (tests/test_svooc_script.cpp). It stands in for the phone
// and picks the 9-bit reply word for each adapter command, from the words a OnePlus 15 sent its SUPERVOOC 120 W adapter
// in every captured session (reverse-engineered: tests/fixtures/op15_svooc_entry.edges and op15_svooc_replug.edges of
// 2026-11-11, op15_svooc_usbc_entry.edges of 2026-11-12 on another cable and battery level, oneplus15_svooc_init.edges of
// 2026-11-02; the init script is byte-identical in all of them). What the words mean is not known; the responder only
// replays them. Part 2 (svooc_wire.hpp) puts the words on D-, part 3 (svooc_run.hpp) holds the guards.
//
// Replies are keyed by (command, occurrence), never by the command alone: 0x98 is answered 0x073, 0x023, 0x010, 0x010
// in turn. The script:
//   ident  0xA8 > 0x00D, every time (up to kMaxIdents before the script; the phone sometimes left the first one silent
//          and answered the adapter's retry 166 ms later, on 2026-11-02 it answered the first one);
//   handshake (kScript) 98>073 98>023 9E>010 80>010 94>010 98>010 8A>028 98>010 8E>010 9A>030 82>074 82>00B 84>071
//          84>031 86>06B 86>03A 88>05D 88>042 8A>048 8A>052 8C>046 8C>066 88>04D; the last pair is "accepted" (a
//          rejected session answered that 0x88 with 0x00D or 0x000 and the adapter kept polling it);
//   ramp, after 88>04D (inferred from the four accepted sessions, no meaning known):
//          0x94 > 0x000 (the phone was silent to it in all four),
//          0x84 > 0x010 for kRampSteps polls, then 0x030 once (the phones sent 7, 8 and 10 x 0x010, sometimes a 0x020,
//                 and always ended the run with 0x030; the fewest 0x010 is taken),
//          0x86 > 0x023 and 0x00F alternately (cap1: 023, 00F, 023 over the first 2 s; the USB-C session sent 02D for 023),
//          0x82 > kSteadyWord every poll (the phone sent a slowly rising word; one fixed low word is the conservative
//                 stand-in, see kSteadyWord).
// Anything else is off-script: answered with kSilent (D- low for the frame, what a phone that does not answer leaves on
// the line) and counted; more than kOffScriptMax off-script commands in a row make abort() true. An on-script command
// ends the run.
#pragma once
#include <array>
#include <cstdint>

namespace k2::svooc {

inline constexpr std::uint8_t kIdent = 0xA8;
inline constexpr std::uint16_t kIdentWord = 0x00D;
inline constexpr std::uint16_t kSilent = 0x000;         // the phone's "no answer": D- low through the reply frame
inline constexpr int kMaxIdents = 3;                    // idents answered before the script starts
// The 0x82 poll's word after the handshake. The phone's word rose slowly (0x04C .. 0x067 in cap1, 0x051 .. 0x067 on
// USB-C; inferred, unproven: a battery reading the adapter regulates against). 0x051 is the operator's choice
// (2026-11-12): the lowest word of the USB-C session. Lower words were seen too (0x04C cap1, 0x033 on 2026-11-02).
inline constexpr std::uint16_t kSteadyWord = 0x051;
inline constexpr int kRampSteps = 7;                    // 0x84 polls answered kRampStep before kRampEnd
inline constexpr std::uint16_t kRampStep = 0x010, kRampEnd = 0x030;
inline constexpr std::array<std::uint16_t, 2> kBattery2{0x023, 0x00F};   // 0x86 after the handshake, alternating
inline constexpr std::uint8_t kOffScriptMax = 2;        // more off-script commands in a row than this: abort

struct Step {
    std::uint8_t cmd;
    std::uint16_t word;
};

// The handshake after the ident, in the adapter's order. The last step is the accepted cable check.
inline constexpr std::array<Step, 23> kScript{{
    {0x98, 0x073}, {0x98, 0x023}, {0x9E, 0x010}, {0x80, 0x010}, {0x94, 0x010}, {0x98, 0x010}, {0x8A, 0x028},
    {0x98, 0x010}, {0x8E, 0x010}, {0x9A, 0x030}, {0x82, 0x074}, {0x82, 0x00B}, {0x84, 0x071}, {0x84, 0x031},
    {0x86, 0x06B}, {0x86, 0x03A}, {0x88, 0x05D}, {0x88, 0x042}, {0x8A, 0x048}, {0x8A, 0x052}, {0x8C, 0x046},
    {0x8C, 0x066}, {0x88, 0x04D},
}};
static_assert(kScript.size() <= 32, "the used-step mask is 32 bits");

struct Answer {
    std::uint16_t word = kSilent;   // the 9-bit reply word
    bool on_script = false;
};

enum class Phase : std::uint8_t { Ident, Handshake, Ramp };

class Responder {
public:
    Answer on_command(std::uint8_t cmd) {
        const Answer a = lookup(cmd);
        if (a.on_script) off_ = 0;
        else if (off_ < 0xFF) ++off_;
        return a;
    }
    bool accepted() const { return phase_ == Phase::Ramp; }   // 88 > 04D was answered
    bool abort() const { return off_ > kOffScriptMax; }
    std::uint8_t off_run() const { return off_; }             // off-script commands in a row now
    Phase phase() const { return phase_; }

private:
    static constexpr Answer off() { return {kSilent, false}; }

    Answer lookup(std::uint8_t cmd) {
        if (phase_ == Phase::Ident) {
            if (cmd == kIdent) {
                if (idents_ >= kMaxIdents) return off();
                ++idents_;
                return {kIdentWord, true};
            }
            if (idents_ == 0) return off();   // the script opens with the ident
            phase_ = Phase::Handshake;
        }
        if (phase_ == Phase::Handshake) {
            // The n-th time a command comes it takes the n-th step that carries it: the first step not used yet.
            for (std::size_t i = 0; i < kScript.size(); ++i) {
                if (kScript[i].cmd != cmd || (used_ >> i & 1u)) continue;
                used_ |= 1u << i;
                if (i + 1 == kScript.size()) phase_ = Phase::Ramp;
                return {kScript[i].word, true};
            }
            return off();
        }
        switch (cmd) {
        case 0x94: return {kSilent, true};
        case 0x84:
            if (ramp_ > kRampSteps) return off();   // the ramp was ended already
            return {ramp_++ < kRampSteps ? kRampStep : kRampEnd, true};
        case 0x86: return {kBattery2[n86_++ & 1u], true};
        case 0x82: return {kSteadyWord, true};
        default: return off();
        }
    }

    Phase phase_ = Phase::Ident;
    std::uint8_t idents_ = 0;
    std::uint32_t used_ = 0;    // kScript steps answered
    int ramp_ = 0;              // 0x84 polls answered after the handshake
    std::uint8_t n86_ = 0;
    std::uint8_t off_ = 0;
};

}  // namespace k2::svooc
