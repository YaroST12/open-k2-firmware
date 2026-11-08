// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A whole hardware run as a report card, shared by the host tests (the tone table, the link golden for the results page).
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "k2/chargertest/report_card.hpp"

namespace cardfx {
namespace ct = k2::charger_test;
namespace rc = k2::report_card;
using A = ct::Algorithm;
using V = ct::Verdict;

inline ct::Step step(A a, ct::Group g, std::uint8_t pos, unsigned mv, V v, unsigned measured = 0, ct::Reason why = ct::Reason::None,
                     std::uint32_t pdo = 0, bool cont = false, bool avs = false, std::uint8_t detail = 0) {
    ct::Step s;
    s.algorithm = a; s.group = g; s.pos = pos; s.mv = std::uint16_t(mv); s.verdict = v; s.measured_mv = std::uint16_t(measured);
    s.reason = why; s.pdo = pdo; s.continuous = cont; s.avs = avs; s.detail = detail;
    return s;
}

// Baseus FH22 100 W as the hardware run of 2026-11-08 (card #18) read: the five fixed levels pass (20 V measured 19.4 V, inside
// the PD tolerance), the SPR AVS 9-20 V range passes, the PPS 5-21 V range stops at 19.4 V (Capped), and the D+/D- protocols the
// charger does not carry never answer: AFC / FCP / SCP "unsupported", VOOC / SVOOC / SVOOC 65W "needs load", VIVO "no response".
inline rc::Card fh22_full() {
    using F = k2::fc::Fail;
    constexpr std::uint32_t kPps = (3u << 30) | (210u << 17) | (50u << 8) | 100;   // PPS 5-21 V 5 A
    constexpr std::uint32_t kAvs = (3u << 30) | (2u << 28) | 100;                  // SPR AVS, 20 V current set: 9-20 V
    auto dead = [](A a, ct::Group g, F why) { return step(a, g, 0, 5000, V::Fail, 0, ct::Reason::Timeout, 0, false, false, std::uint8_t(why)); };
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5030),
        step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010),
        step(A::Pd3Fixed, ct::Group::Pd, 3, 12000, V::Pass, 12010),
        step(A::Pd3Fixed, ct::Group::Pd, 4, 15000, V::Pass, 15020),
        step(A::Pd3Fixed, ct::Group::Pd, 5, 20000, V::Pass, 19413),
        step(A::SprAvs, ct::Group::Pd, 7, 9000, V::Pass, 9020, ct::Reason::None, kAvs, true, true),
        step(A::SprAvs, ct::Group::Pd, 7, 15000, V::Pass, 15010, ct::Reason::None, kAvs, true, true),
        step(A::SprAvs, ct::Group::Pd, 7, 20000, V::Pass, 19406, ct::Reason::None, kAvs, true, true),
        step(A::Pps, ct::Group::Pd, 6, 5000, V::Pass, 5040, ct::Reason::None, kPps, true),
        step(A::Pps, ct::Group::Pd, 6, 13000, V::Pass, 12965, ct::Reason::None, kPps, true),
        step(A::Pps, ct::Group::Pd, 6, 21000, V::Fail, 19419, ct::Reason::Capped, kPps, true),
        step(A::Qc2, ct::Group::Qc2, 0, 9000, V::Skip, 0, ct::Reason::NoOffer),
        dead(A::Afc, ct::Group::Afc, F::Unsupported),
        dead(A::Fcp, ct::Group::Fcp, F::Unsupported),
        dead(A::Scp, ct::Group::Scp, F::Unsupported),
        dead(A::Vooc, ct::Group::Vooc, F::NeedsLoad),
        dead(A::SuperVooc, ct::Group::SuperVooc, F::NeedsLoad),
        dead(A::SuperVooc65, ct::Group::SuperVooc65, F::NeedsLoad),
        dead(A::Vivo, ct::Group::Vivo, F::NoResponse),
    };
    ct::Evidence ev;
    ev.wire_rev = 3; ev.num_pdos = 7; ev.flags = ct::PpsOffer;
    rc::Identity id{.fp = 0x6B8A1F3C, .vid = 0, .pid = 0, .pdp_w = 100};
    rc::Card c = rc::build(steps, ev, {}, id, {.fw = 0x0FCB3128, .boots = 41, .uptime_s = 905}, ct::State::Done, ct::Reason::None);
    c.card_no = 18; c.runs = 2;
    std::strcpy(c.name, "FH22 100W");
    return c;
}

// A UFCS charger as the planner tags its rows: three modes of Output_Capabilities (5-11 V 3 A, 5-20 V 3 A, 3.3-5.5 V 5 A), each UFCS row
// carrying the mode it samples, lowest minimum first as the planner orders them. A 60 W UFCS brick that also speaks PD.
inline rc::Card ufcs_modes() {
    auto row = [](unsigned mv, unsigned measured, int lo, int hi, int ma, V v = V::Pass) {
        return step(A::Ufcs, ct::Group::Ufcs, 0, mv, v, measured, ct::Reason::None, ct::pack_ufcs_mode(lo, hi, 0, ma));
    };
    const std::vector<ct::Step> steps = {
        step(A::Pd3Fixed, ct::Group::Pd, 1, 5000, V::Pass, 5030),
        step(A::Pd3Fixed, ct::Group::Pd, 2, 9000, V::Pass, 9010),
        step(A::Pd3Fixed, ct::Group::Pd, 3, 20000, V::Pass, 19950),
        row(5000, 5020, 3300, 5500, 5000), row(3300, 3310, 3300, 5500, 5000), row(5500, 5480, 3300, 5500, 5000),
        row(5000, 5010, 5000, 11000, 3000), row(8000, 7990, 5000, 11000, 3000), row(11000, 10980, 5000, 11000, 3000),
        row(5000, 5015, 5000, 20000, 3000), row(12500, 12480, 5000, 20000, 3000), row(20000, 19960, 5000, 20000, 3000),
    };
    ct::Evidence ev;
    ev.wire_rev = 3; ev.num_pdos = 3;
    rc::Identity id{.fp = 0x7C2D4E90, .vid = 0x2717, .pid = 0x0101, .pdp_w = 60};
    rc::Card c = rc::build(steps, ev, {}, id, {.fw = 0x0FCB3128, .boots = 44, .uptime_s = 1210}, ct::State::Done, ct::Reason::None);
    c.card_no = 21; c.runs = 1;
    std::strcpy(c.name, "UFCS 60W");
    return c;
}
}  // namespace cardfx
