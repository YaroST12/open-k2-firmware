// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The boot-time Ra of a self-powered K2 (pure rules, host-tested in tests/test_boot_ra.cpp).
//
// A source asks the cable (SOP' Discover Identity) when it attaches and sees an Ra. A key-mode boot attaches at plug-in with
// only the key's Rd on CC2, so an identity armed at START (seconds later) is too late for that attach. When a cable identity
// is wanted (the shared selection, or the journaled one of a run that resumes) and the boot is self-powered (a key mode or an
// inherited pin), the identity is armed and PC13 goes high (Ra on CC1, Rd on CC2 alone, k2/pd/emk_ra.hpp) as early in the boot
// as the journal can be read, before the 5 V contract and before anything else.
//
// The Ra guard (k2/pd/ra_guard.hpp) cannot classify a pinned K2: its own Rd on the lines is not "passive". What a self-powered
// boot can read is the line it does not pin: a receptacle charger shows its open Rp (>= kRpMinMv) on CC1 (the PLUS pin is on
// CC2); a lower level is a cable in between. No reading: a resumed run keeps the verdict its START already got, a mere
// selection is not armed (logged why).
#pragma once
#include <cstdint>

#include "k2/pd/emk_ra.hpp"
#include "k2/pd/ra_guard.hpp"

namespace k2::bootra {

enum class Verdict : std::uint8_t {
    NotRun,           // the decision was not made (this boot has no page yet)
    Arm,              // identity armed and the Ra on
    NotSelfPowered,   // a plain (HID-powered) boot: START arms as before
    NoIdentity,       // nothing selected and nothing journaled
    Refused,          // the Ra rules refuse (OK key pin on CC1, a sink inline, ...)
    Cable,            // the other line shows no Rp: a cable in between, the Ra would hide the sink
    NoReading,        // no analog reading and nothing journaled: the guard cannot be evaluated
    Failed,           // the decision was Arm but the arm or the Ra mode itself failed
};

struct In {
    bool self_powered = false;     // a key mode or an inherited pin
    bool identity = false;         // a valid identity other than Off is wanted
    bool journaled = false;        // it is the identity of a run that resumes (the guard already passed at its START)
    bool inline_sink = false;
    std::uint8_t pinned_rd = 0;    // the boot's Rd bits (bit0 CC1, bit1 CC2)
    std::int32_t cc1_mv = -1, cc2_mv = -1;   // -1 = no reading
};

constexpr Verdict decide(const In& in) {
    if (!in.self_powered) return Verdict::NotSelfPowered;
    if (!in.identity) return Verdict::NoIdentity;
    emkra::Ctx c;
    c.inline_sink = in.inline_sink;
    c.pinned_rd = in.pinned_rd;
    if (emkra::check(c) != emkra::Refuse::None) return Verdict::Refused;
    if (in.cc1_mv >= 0) return in.cc1_mv >= raguard::kRpMinMv ? Verdict::Arm : Verdict::Cable;
    return in.journaled ? Verdict::Arm : Verdict::NoReading;
}

constexpr const char* name(Verdict v) {
    switch (v) {
        case Verdict::NotRun: return "-";
        case Verdict::Arm: return "arm";
        case Verdict::NotSelfPowered: return "plain";
        case Verdict::NoIdentity: return "noid";
        case Verdict::Refused: return "refused";
        case Verdict::Cable: return "cable";
        case Verdict::NoReading: return "noread";
        case Verdict::Failed: return "fail";
    }
    return "?";
}

// What this boot decided, RAM only: shown in the boot-info text.
inline Verdict g_last = Verdict::NotRun;
inline void note(Verdict v) { g_last = v; }
inline Verdict last() { return g_last; }

}  // namespace k2::bootra
