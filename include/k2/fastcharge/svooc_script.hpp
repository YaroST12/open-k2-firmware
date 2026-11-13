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
//   phone profile (Params::profile Phone / Probe): the words of a OnePlus 15 session after the accept, from its own passive
//          capture (reverse-engineered; the phone is never cut off by the adapter, the fixed profile above was at 72 pairs):
//          0x84 > 0x020 x4 then 0x030, 0x86 > 0x06D at the ramp end, then 0x82 polls of a slowly moving word (phone_word:
//          0x025 x11, 0x026 x10, 0x029 x12, 0x028 x11, ... up to 0x057, the battery telemetry), the first escape on the
//          12th 82 poll (0.52 s after the ramp-end 0x86) answered 0x86 > 0x00F, the next one every 34 polls (1.5 s)
//          answered by the ladder (Phone: 0x06D, the phone's own second ask; Probe: 0x028, 0x03F, 0x06D, upward from a
//          0x00F first ask, so the highest level the adapter keeps polling under shows). Every 0x86 answered after the
//          handshake is recorded (asks(): time, word, and the 82 polls that followed it).
//          The 0x86 word is the CURRENT LIMIT the sink asks for, 100 mA per count (confirmed against measured current:
//          0x00F 1.5 A, 0x028 4.0 A, 0x03F 6.3 A, 0x06D 10.9 A). It is the K2's own word: what the responder can say about
//          an adapter is the highest ask under which the session went on (ceiling_word()), never a measured limit.
// Anything else is off-script: answered with kSilent (D- low for the frame, what a phone that does not answer leaves on
// the line) and counted; more than kOffScriptMax off-script commands in a row make abort() true. An on-script command
// ends the run.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

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

// The phone profile's words (the OnePlus 15 capture): the 0x84 ramp, the 0x86 asks and the escape schedule, in 82 polls.
inline constexpr int kPhoneRampSteps = 4;
inline constexpr std::uint16_t kPhoneRampStep = 0x020;
inline constexpr std::uint16_t kI86PhoneMax = 0x06D;    // the phone's top ask, 10.9 A (the protocol's word, no load drawn)
inline constexpr std::uint16_t kPhoneFirstAsk = 0x06D;  // 0x86 at the ramp end (Phone)
inline constexpr std::uint16_t kPhoneEscAsk = 0x00F;    // 0x86 after the first escape
inline constexpr int kEscFirstPoll = 11;                // the escape is the 12th 82 poll after the ramp end ...
inline constexpr int kEscGapPolls = 34;                 // ... and then every 34th (43.5 ms each: 0.52 s, then 1.5 s)
inline constexpr int kAcceptPolls = 4;                  // 82 polls the adapter kept on the script after an ask: accepted
inline constexpr std::size_t kAsksMax = 8;

// How the responder answers after the accept. Fixed = the operator's constants (the original, a OnePlus session survives
// about 72 pairs of it); Phone = the OnePlus 15's own words and escapes; Probe = Phone's ramp and 0x82 words with an
// ascending 0x86 ladder (0x00F, 0x028, 0x03F, 0x06D).
enum class Profile : std::uint8_t { Fixed, Phone, Probe };
constexpr const char* name(Profile p) {
    switch (p) {
    case Profile::Fixed: return "fixed";
    case Profile::Phone: return "phone";
    case Profile::Probe: return "probe";
    }
    return "?";
}

// The current limit an 0x86 word asks for, in mA (100 mA per count).
constexpr std::int32_t ask_ma(std::uint16_t word) { return std::int32_t(word) * 100; }

// The phone's 0x82 word after the n-th word poll (escape polls not counted), the capture's runs of the same word: the
// battery telemetry, a few counts up in the first seconds, then holding at its last word.
inline constexpr std::array<std::array<std::uint8_t, 2>, 23> kPhoneRuns{{
    {0x25, 11}, {0x26, 10}, {0x29, 12}, {0x28, 11}, {0x28, 10}, {0x27, 11}, {0x2C, 13}, {0x30, 10}, {0x34, 13},
    {0x39, 10}, {0x3C, 14}, {0x41, 9}, {0x43, 2}, {0x44, 12}, {0x47, 9}, {0x49, 3}, {0x4A, 11}, {0x4E, 9},
    {0x50, 3}, {0x51, 12}, {0x53, 8}, {0x55, 3}, {0x56, 12},
}};
inline constexpr std::uint16_t kPhoneHold = 0x057;
constexpr std::uint16_t phone_word(std::uint32_t n) {
    for (const auto& r : kPhoneRuns) {
        if (n < r[1]) return r[0];
        n -= r[1];
    }
    return kPhoneHold;
}

struct Params {
    std::uint16_t i86_first = kI86Default;   // 0x86 answers until an escape was answered (Fixed)
    std::uint16_t i86_esc = kI86Default;     // 0x86 answers from the escape's 0x86 on (Fixed); the last ask of Phone's ladder
    std::uint16_t r82 = kSteadyWord;         // constant 0x82 word (ignored while vtarget_mv != 0 and VBUS is known; Fixed)
    std::uint16_t vtarget_mv = 0;            // closed loop on 0x82: the VBUS at which the word says "battery high"; 0 = off
    std::uint8_t esc_at_s = 0;               // seconds after the wake: one 0x82 poll gets the escape; 0 = never (Fixed)
    Profile profile = Profile::Fixed;

    constexpr Params clamped() const {
        Params p = *this;
        const auto cl = [](std::uint16_t v, std::uint16_t lo, std::uint16_t hi) { return v < lo ? lo : v > hi ? hi : v; };
        const std::uint16_t top = profile == Profile::Fixed ? kI86Max : kI86PhoneMax;
        p.i86_first = cl(i86_first, kI86Min, top);
        p.i86_esc = cl(i86_esc, kI86Min, top);
        p.r82 = cl(r82, 0, kR82Max);
        p.vtarget_mv = vtarget_mv > kVtargetMaxMv ? kVtargetMaxMv : vtarget_mv;
        p.esc_at_s = esc_at_s > kEscapeAtMaxS ? kEscapeAtMaxS : esc_at_s;
        if (profile != Profile::Phone && profile != Profile::Probe) p.profile = Profile::Fixed;
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
        esc_ = (p_.esc_at_s || ladder()) ? Escape::Armed : Escape::Off;
        esc_t_ms_ = 0;
        esc_n_ = 0;
        n82_ = 0;
        nw_ = 0;
        next_esc_ = kEscFirstPoll;
        ask_ = ladder() ? ladder_word(0) : 0;
        n_asks_ = 0;
    }
    // Every 0x86 answered after the handshake: when (ms since the wake, 16 bits), the word, and how many 82 polls the
    // adapter kept on the script while it stood.
    struct Ask {
        std::uint16_t t_ms = 0;
        std::uint16_t word = 0;
        std::uint8_t after = 0;
    };
    std::span<const Ask> asks() const { return {asks_.data(), n_asks_}; }
    // The highest ask the adapter went on under (at least kAcceptPolls 82 polls on the script after it), 0 = none yet. This
    // is what the session shows of the adapter's current ceiling: the K2 asked it, nothing was drawn.
    std::uint16_t ceiling_word() const {
        std::uint16_t best = 0;
        for (const Ask& a : asks()) if (a.after >= kAcceptPolls && a.word > best) best = a.word;
        return best;
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
        const int steps = ladder() ? kPhoneRampSteps : kRampSteps;
        switch (cmd) {
        case 0x94: return {kSilent, true};
        case 0x84:
            if (ramp_ > steps) return off();   // the ramp was ended already
            return {ramp_++ < steps ? (ladder() ? kPhoneRampStep : kRampStep) : kRampEnd, true};
        case 0x86: {
            // Fixed: from the escape on (the 0x86 that followed it, or a late one) the new target stands. Phone / Probe:
            // the ladder's current rung, set when its escape went out.
            const std::uint16_t w = ladder() ? ask_
                                    : (esc_ == Escape::Got86 || esc_ == Escape::NoGet86 ? p_.i86_esc : p_.i86_first);
            record_ask(t_ms, w);
            return {w, true};
        }
        case 0x82: {
            const bool due = ladder() ? (esc_n_ + 1 < ladder_len() && int(n82_) >= next_esc_ && esc_ != Escape::Sent)
                                      : (esc_ == Escape::Armed && t_ms >= std::uint32_t(p_.esc_at_s) * 1000u);
            if (n_asks_ && asks_[n_asks_ - 1].after < 0xFF) ++asks_[n_asks_ - 1].after;
            if (n82_ < 0xFFFF) ++n82_;
            if (due) {
                esc_ = Escape::Sent;
                esc_t_ms_ = t_ms;
                if (ladder()) {
                    ask_ = ladder_word(++esc_n_);
                    next_esc_ = int(n82_) - 1 + kEscGapPolls;
                }
                return {kEscapeWord, true};
            }
            if (ladder()) return {phone_word(nw_++), true};
            if (p_.vtarget_mv && vbus_mv) return {closed_loop_reply(vbus_mv, p_.vtarget_mv), true};
            return {p_.r82, true};
        }
        default: return off();
        }
    }

    bool ladder() const { return p_.profile != Profile::Fixed; }
    // The ladder's asks: k = 0 at the ramp end, k = n after the n-th escape. Phone: 06D, 00F, then i86_esc (the phone's
    // own second ask); Probe: 00F, 028, 03F, 06D (every level the phone asked, upward).
    std::uint16_t ladder_word(int k) const {
        if (p_.profile == Profile::Probe) {
            constexpr std::uint16_t kProbe[] = {0x00F, 0x028, 0x03F, 0x06D};
            return kProbe[k < 4 ? k : 3];
        }
        return k == 0 ? kPhoneFirstAsk : k == 1 ? kPhoneEscAsk : p_.i86_esc;
    }
    int ladder_len() const { return p_.profile == Profile::Probe ? 4 : 3; }
    void record_ask(std::uint32_t t_ms, std::uint16_t w) {
        const std::uint16_t t = t_ms > 0xFFFF ? 0xFFFF : std::uint16_t(t_ms);
        const std::size_t i = n_asks_ < kAsksMax ? n_asks_++ : kAsksMax - 1;   // full: the last slot follows the latest
        asks_[i] = Ask{t, w, 0};
    }

    Phase phase_ = Phase::Ident;
    std::uint8_t idents_ = 0;
    std::uint32_t used_ = 0;    // kScript steps answered
    int ramp_ = 0;              // 0x84 polls answered after the handshake
    std::uint8_t off_ = 0;
    Params p_ = Params{}.clamped();
    Escape esc_ = Escape::Off;
    std::uint32_t esc_t_ms_ = 0;
    int esc_n_ = 0;               // escapes sent (the ladder's rung)
    std::uint16_t n82_ = 0;       // 82 polls answered after the handshake, escapes included
    std::uint32_t nw_ = 0;        // the phone's word polls answered (escapes not counted)
    int next_esc_ = kEscFirstPoll;   // the n82_ at which the next escape goes out
    std::uint16_t ask_ = 0;       // the ladder's current 0x86 word
    std::array<Ask, kAsksMax> asks_{};
    std::size_t n_asks_ = 0;
};

}  // namespace k2::svooc
