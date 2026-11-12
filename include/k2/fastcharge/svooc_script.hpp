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
//          0x86 > the session current target, Params::i86_first (default 0x005; the phone sent 0x00F early in the ramp and
//                 0x023 / 0x02D at the steady state, about 100 mA per count, inferred from measured current),
//          0x82 > Params::r82 (default kSteadyWord) every poll, or with Params::vtarget_mv the closed-loop word of the
//                 measured VBUS (closed_loop_reply). The phone sent a slowly rising word that is the battery telemetry:
//                 VBUS = 20.35 mV x word + 6918 mV at 3.6 A (inferred fit, RMS 6 mV). One fixed low word is the
//                 conservative stand-in, see kSteadyWord.
//   escape (Params::esc_at_s): the phone asks for a fresh 0x86 by answering a 0x82 poll with kEscapeWord (0x07F, seen
//          6 of 6 times); the adapter then sends 0x86 on its next command. After esc_at_s seconds (since the wake) one
//          0x82 poll of the ramp is answered with it, and the 0x86 that follows is answered Params::i86_esc.
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
inline constexpr std::uint8_t kOffScriptMax = 2;        // more off-script commands in a row than this: abort

// The operator's knobs (the remote's start request), clamped by Params::clamped():
// 0x86 after the handshake: the session current target, 0x001..0x023 (about 0.1 A per count: 0.1 .. 3.5 A); default 0x005.
inline constexpr std::uint16_t kI86Default = 0x005, kI86Min = 0x001, kI86Max = 0x023;
// 0x82 constant: 0x000..0x07E; 0x07F is the escape, never a steady word.
inline constexpr std::uint16_t kR82Max = 0x07E, kEscapeWord = 0x07F;
// The closed loop's target VBUS: at most 11000 mV (the guard's own ceiling is 12 V). 0 = no closed loop.
inline constexpr std::uint16_t kVtargetMaxMv = 11000;
inline constexpr std::uint8_t kEscapeAtMaxS = 20;       // the run's cap

struct Params {
    std::uint16_t i86_first = kI86Default;   // 0x86 answers until an escape was answered
    std::uint16_t i86_esc = kI86Default;     // 0x86 answers from the escape's 0x86 on
    std::uint16_t r82 = kSteadyWord;         // constant 0x82 word (ignored while vtarget_mv != 0 and VBUS is known)
    std::uint16_t vtarget_mv = 0;            // closed loop on 0x82: the VBUS at which the word says "battery high"; 0 = off
    std::uint8_t esc_at_s = 0;               // seconds after the wake: one 0x82 poll gets the escape; 0 = never

    constexpr Params clamped() const {
        Params p = *this;
        const auto cl = [](std::uint16_t v, std::uint16_t lo, std::uint16_t hi) { return v < lo ? lo : v > hi ? hi : v; };
        p.i86_first = cl(i86_first, kI86Min, kI86Max);
        p.i86_esc = cl(i86_esc, kI86Min, kI86Max);
        p.r82 = cl(r82, 0, kR82Max);
        p.vtarget_mv = vtarget_mv > kVtargetMaxMv ? kVtargetMaxMv : vtarget_mv;
        p.esc_at_s = esc_at_s > kEscapeAtMaxS ? kEscapeAtMaxS : esc_at_s;
        return p;
    }
};

// The telemetry fit, in counts: round((vbus_mv - 6918) / 20.35), negative below 6918 mV. A real phone's 0x82 word at 3.6 A.
constexpr int vbus_count(std::int32_t vbus_mv) {
    const std::int32_t x = (vbus_mv - 6918) * 1000;
    return int((x + (x >= 0 ? 10175 : -10175)) / 20350);
}

// The closed-loop 0x82 word for a measured VBUS: vbus_count(v) + k, clamped to 0..kR82Max, where k = max(0, kR82Max -
// vbus_count(vtarget)). It never reads below the real phone's word (k >= 0) and reaches the top of the 7-bit scale
// (kR82Max, "battery high") exactly when VBUS reaches the target; a target above what the scale can say (9482 mV) gives
// the phone's own word, saturating at kR82Max. Inferred rule, not a known adapter behaviour.
constexpr std::uint16_t closed_loop_reply(std::int32_t vbus_mv, std::int32_t vtarget_mv) {
    const int k = int(kR82Max) - vbus_count(vtarget_mv);
    const int w = vbus_count(vbus_mv) + (k > 0 ? k : 0);
    return std::uint16_t(w < 0 ? 0 : w > int(kR82Max) ? int(kR82Max) : w);
}

// What came of the escape: Off (esc_at_s = 0), Armed (waiting for its time), Sent (0x07F answered, the next command not
// seen yet), Got86 (the adapter sent 0x86 next, as expected), NoGet86 (it sent something else).
enum class Escape : std::uint8_t { Off, Armed, Sent, Got86, NoGet86 };
constexpr const char* name(Escape e) {
    switch (e) {
    case Escape::Off: return "off";
    case Escape::Armed: return "armed";
    case Escape::Sent: return "sent";
    case Escape::Got86: return "got 86";
    case Escape::NoGet86: return "no 86";
    }
    return "?";
}

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
    // Sets the operator's parameters (clamped). Before the run: the script state is not touched, only the escape is re-armed.
    void configure(const Params& p) {
        p_ = p.clamped();
        esc_ = p_.esc_at_s ? Escape::Armed : Escape::Off;
        esc_t_ms_ = 0;
    }
    // t_ms: milliseconds since the wake (the escape's clock); vbus_mv: the latest VBUS reading, 0 = unknown (the closed
    // loop then falls back to the constant word).
    Answer on_command(std::uint8_t cmd, std::uint32_t t_ms = 0, std::uint16_t vbus_mv = 0) {
        const Answer a = lookup(cmd, t_ms, vbus_mv);
        if (a.on_script) off_ = 0;
        else if (off_ < 0xFF) ++off_;
        return a;
    }
    const Params& params() const { return p_; }               // the effective (clamped) parameters
    Escape escape() const { return esc_; }
    std::uint32_t escape_t_ms() const { return esc_t_ms_; }   // when the 0x07F was sent (the poll's time stamp)
    bool accepted() const { return phase_ == Phase::Ramp; }   // 88 > 04D was answered
    bool abort() const { return off_ > kOffScriptMax; }
    std::uint8_t off_run() const { return off_; }             // off-script commands in a row now
    Phase phase() const { return phase_; }

private:
    static constexpr Answer off() { return {kSilent, false}; }

    Answer lookup(std::uint8_t cmd, std::uint32_t t_ms, std::uint16_t vbus_mv) {
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
        // The command after the escape tells whether the adapter asked for 0x86 as expected.
        const bool after_escape = esc_ == Escape::Sent;
        if (after_escape) esc_ = cmd == 0x86 ? Escape::Got86 : Escape::NoGet86;
        switch (cmd) {
        case 0x94: return {kSilent, true};
        case 0x84:
            if (ramp_ > kRampSteps) return off();   // the ramp was ended already
            return {ramp_++ < kRampSteps ? kRampStep : kRampEnd, true};
        case 0x86:
            // From the escape on (the 0x86 that followed it, or a late one) the new target stands.
            return {esc_ == Escape::Got86 || esc_ == Escape::NoGet86 ? p_.i86_esc : p_.i86_first, true};
        case 0x82:
            if (esc_ == Escape::Armed && t_ms >= std::uint32_t(p_.esc_at_s) * 1000u) {
                esc_ = Escape::Sent;
                esc_t_ms_ = t_ms;
                return {kEscapeWord, true};
            }
            if (p_.vtarget_mv && vbus_mv) return {closed_loop_reply(vbus_mv, p_.vtarget_mv), true};
            return {p_.r82, true};
        default: return off();
        }
    }

    Phase phase_ = Phase::Ident;
    std::uint8_t idents_ = 0;
    std::uint32_t used_ = 0;    // kScript steps answered
    int ramp_ = 0;              // 0x84 polls answered after the handshake
    std::uint8_t off_ = 0;
    Params p_ = Params{}.clamped();
    Escape esc_ = Escape::Off;
    std::uint32_t esc_t_ms_ = 0;
};

}  // namespace k2::svooc
