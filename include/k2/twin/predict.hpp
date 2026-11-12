// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Device twin prediction, pure logic (host-tested in tests/test_twin_predict.cpp): given a twin (a device's sink PDOs, optionally
// its sniffed Request) and a charger's Source_Capabilities, which contract would that device end up in, and what holds it there.
//
// SELECTION POLICY. Each rule has a test of the same number in tests/test_twin_predict.cpp.
//  1. A fixed source PDO is usable when the twin has a fixed sink PDO at the same voltage. (A sink's Sink_Capabilities list the
//     voltages it can run on; the K2 does not guess that a device with a 5 V and a PPS entry would also take 9 V.)
//  2. Its current is min(the source PDO's maximum, the twin's operational current at that voltage): the 10 mA field of the
//     sink PDO, the current the device says it needs there (USB PD 3.1 6.4.1.3.1). Never more than the charger advertises.
//  3. A programmable (PPS) source PDO is usable against each PPS APDO of the twin whose voltage range overlaps it; the voltage is
//     the top of the overlap, the current min(source, twin) as in rule 2, the power voltage x current.
//  4. The highest power wins. On a tie a PPS level above 5 V beats fixed (the device then holds the level with fine voltage
//     control; a PPS "level" at 5 V is just the always-present 5 V PDO, which stays the answer), and otherwise the higher
//     voltage wins (less current in the cable); after that the lower position.
//  5. Nothing above 5 V usable: the contract is the 5 V PDO (Kind::FiveV). The RDO then carries Capability Mismatch when the
//     twin's sink PDO 1 has Higher Capability set: the device said it needs more than vSafe5V.
//  6. No usable result (no valid source list, no sink PDOs and no sniffed Request, a first sink PDO that is not 5 V): Kind::None.
//     A twin made from a sniffed Request alone (no sink PDOs) answers by rule 7 only.
//  7. A sniffed Request wins over rules 4-5 when the new list has a PDO of the same shape as the one the device asked from
//     (fixed: the same voltage; PPS: a range that contains the voltage it asked for), flagged `observed`. Its current is what the
//     device asked for when it asked for LESS than that PDO then offered (that is its real need); when it asked for everything
//     (it took all it was given) the current is the larger of the Request and the sink PDO, so a weak first charger does not
//     cap the prediction on a stronger one. Positions are never compared: a charger lists its levels in its own order.
//  8. Never more than the charger advertises, and never more than a known cable carries: Facts::cable_ma / cable_mv clip every
//     level (0 = cable unknown). The K2's own monitor ceiling is the live proof's business (it refuses a level above it), not
//     the prediction's: an SPR twin never asks above 21 V.
//  9. Capability Mismatch (Outcome::mismatch) is the offer being below the twin's declared need: the source's current (after the
//     cable) is below the need at that voltage, or rule 5.
// 10. Limiter, in this order. The contract meets what the device wants (the best power of its own sink PDOs; for an observed
//     Request, what it asked for): `Device` when the charger advertises more than the contract (the device takes no more), `None`
//     ("full") when it advertises no more. Otherwise `Cable` when a known cable clipped the contract or the offer looks held down by
//     the cable the charger read (rdintent::offer_held_by_cable: its own power claim, else the 20 V / 3.00 A signature), else
//     `Charger`.
//
// WHERE THIS DEVIATES FROM THE SPEC. USB PD lets a sink Request any source PDO that meets its needs, and real sinks do not
// stay inside their own Sink_Capabilities (a phone with a PPS APDO may take a 9 V fixed level it did not list; a laptop may
// request below its listed current). The policy above is the conservative reading of what the device declared; rule 7 is the
// correction from what it actually did. Not modelled: Variable and Battery PDOs (the twin's or the charger's), SPR AVS and EPR
// levels (a twin holds SPR words only), and anything a vendor protocol adds (Xiaomi 6 A, SuperVOOC, Apple's own steps).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "k2/core/rd_intent.hpp"
#include "k2/pd/pd.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/twin/twin.hpp"

namespace k2::twin {

enum class Kind : std::uint8_t { None, Fixed, Pps, FiveV };
enum class Limiter : std::uint8_t { None, Device, Charger, Cable };

// What the caller knows besides the lists. 0 = not known.
struct Facts {
    std::uint32_t claim_w = 0;   // the source's own power claim (Source_Capabilities_Extended PDP), whole watts
    std::uint16_t cable_ma = 0;  // the cable in the path, from its e-marker: current / voltage it carries
    std::uint16_t cable_mv = 0;
};

struct Outcome {
    Kind kind = Kind::None;
    std::uint8_t pos = 0;      // 1-based source PDO position
    std::uint16_t mv = 0, ma = 0;
    std::uint32_t mw = 0;      // mv x ma
    std::uint32_t want_mw = 0; // what the device wants: the best power of its sink PDOs ("of 96W")
    bool mismatch = false;     // the RDO would carry Capability Mismatch
    bool observed = false;     // from the device's own sniffed Request (rule 7), not the policy
    Limiter limiter = Limiter::None;
};

namespace detail {

constexpr std::uint32_t mw_of(std::uint32_t mv, std::uint32_t ma) { return std::uint32_t(std::uint64_t(mv) * ma / 1000u); }

struct Cand {
    Kind kind = Kind::None;
    std::uint8_t pos = 0;
    std::uint16_t mv = 0, ma = 0;
    std::uint32_t mw = 0;
    std::uint16_t need_ma = 0;   // what the twin needs there
    std::uint16_t lim_ma = 0;    // what the source gives there, after the cable
    bool observed = false;
    bool cable_clip = false;     // the cable, not the source or the twin, set the current or the voltage
};

constexpr Kind kind_at(const pd::Pdo& p, std::uint16_t mv) {
    return p.type == pd::PdoType::Pps ? Kind::Pps : (mv == pd::kVsafe5vMv ? Kind::FiveV : Kind::Fixed);
}

// Rule 4 order: more power, then PPS, then the higher voltage; an equal candidate keeps the earlier position.
constexpr bool better(const Cand& a, const Cand& b) {
    if (b.kind == Kind::None) return true;
    if (a.mw != b.mw) return a.mw > b.mw;
    const bool pa = a.kind == Kind::Pps && a.mv > pd::kVsafe5vMv, pb = b.kind == Kind::Pps && b.mv > pd::kVsafe5vMv;
    if (pa != pb) return pa;
    return a.mv > b.mv;
}

inline std::uint16_t clip_ma(std::uint16_t src_ma, const Facts& f) { return f.cable_ma != 0 ? std::min(src_ma, f.cable_ma) : src_ma; }

// Fills `c` for source PDO `p` at position `pos` against sink PDO `s` (a decoded fixed or PPS word); false when unusable (rules 1-3, 8).
inline bool candidate(const pd::Pdo& p, std::uint8_t pos, const pd::Pdo& s, const Facts& f, Cand& c) {
    c = Cand{};
    c.pos = pos;
    if (p.type == pd::PdoType::Fixed && s.type == pd::PdoType::Fixed) {
        if (p.max_mv != s.max_mv) return false;
        if (f.cable_mv != 0 && p.max_mv > f.cable_mv) return false;
        c.mv = p.max_mv;
        c.need_ma = s.max_ma;
        c.lim_ma = clip_ma(p.max_ma, f);
        c.cable_clip = c.lim_ma < std::min(p.max_ma, s.max_ma);
    } else if (p.type == pd::PdoType::Pps && s.type == pd::PdoType::Pps) {
        const std::uint16_t lo = std::max(p.min_mv, s.min_mv);
        std::uint16_t hi = std::min(p.max_mv, s.max_mv);
        c.cable_clip = f.cable_mv != 0 && f.cable_mv < hi;
        if (c.cable_clip) hi = f.cable_mv;
        if (hi < lo) return false;
        c.mv = hi;
        c.need_ma = s.max_ma;
        c.lim_ma = clip_ma(p.max_ma, f);
        c.cable_clip = c.cable_clip || c.lim_ma < std::min(p.max_ma, s.max_ma);
    } else {
        return false;
    }
    c.ma = std::min(c.lim_ma, c.need_ma);
    if (c.ma == 0) return false;
    c.kind = kind_at(p, c.mv);
    c.mw = mw_of(c.mv, c.ma);
    return true;
}

// Rule 7: the candidate the device's sniffed Request names in the new list, or false when the list has no PDO of that shape.
inline bool observed_candidate(const Twin& t, const pd::SourceCaps& caps, const pd::Pdo* sink, std::size_t n_sink, const Facts& f, Cand& c) {
    if (!t.has_request()) return false;
    const pd::Pdo sp = pd::decode_pdo(t.rdo_pdo);
    const pd::Rdo r = pd::decode_rdo(t.rdo, &sp);
    if (r.op_ma == 0 || (sp.type != pd::PdoType::Fixed && sp.type != pd::PdoType::Pps)) return false;
    std::uint16_t sink_ma = 0;   // the twin's own figure for that voltage, when it listed one
    for (std::size_t i = 0; i < n_sink; ++i) {
        const pd::Pdo& s = sink[i];
        if (s.type != sp.type) continue;
        const bool same = sp.type == pd::PdoType::Fixed ? s.max_mv == sp.max_mv : (r.mv >= s.min_mv && r.mv <= s.max_mv);
        if (same) sink_ma = std::max(sink_ma, s.max_ma);
    }
    const std::uint16_t need = r.op_ma < sp.max_ma ? r.op_ma : std::max(r.op_ma, sink_ma);
    const std::size_t n = std::min<std::size_t>(caps.num_pdos, caps.pdos.size());
    for (std::size_t i = 0; i < n; ++i) {
        const pd::Pdo& p = caps.pdos[i];
        if (p.type != sp.type) continue;
        std::uint16_t mv;
        if (sp.type == pd::PdoType::Fixed) {
            if (p.max_mv != sp.max_mv) continue;
            mv = p.max_mv;
        } else {
            if (r.mv < p.min_mv || r.mv > p.max_mv) continue;
            mv = r.mv;
        }
        if (f.cable_mv != 0 && mv > f.cable_mv) continue;
        c = Cand{};
        c.pos = std::uint8_t(i + 1);
        c.mv = mv;
        c.need_ma = need;
        c.lim_ma = clip_ma(p.max_ma, f);
        c.cable_clip = c.lim_ma < std::min(p.max_ma, need);
        c.ma = std::min(c.lim_ma, need);
        if (c.ma == 0) return false;
        c.kind = kind_at(p, c.mv);
        c.mw = mw_of(c.mv, c.ma);
        c.observed = true;
        return true;
    }
    return false;
}

}  // namespace detail

// The best power any of the twin's sink PDOs says it can take: fixed V x I, PPS top voltage x current (0 for an empty twin).
inline std::uint32_t want_mw(const Twin& t) {
    std::uint32_t best = 0;
    for (std::size_t i = 0; i < t.num_pdos && i < t.pdos.size(); ++i) {
        const pd::Pdo s = pd::decode_pdo(t.pdos[i]);
        if (s.type == pd::PdoType::Fixed || s.type == pd::PdoType::Pps) best = std::max(best, detail::mw_of(s.max_mv, s.max_ma));
    }
    return best;
}

// A perfect match: the outcome is exactly one of the device's best sink levels (its full voltage at its full current, the most
// power it takes), and the Request carries no Capability Mismatch.
inline bool perfect_match(const Twin& t, const Outcome& o) {
    if (o.kind == Kind::None || o.mismatch) return false;
    const std::uint32_t want = want_mw(t);
    for (std::size_t i = 0; i < t.num_pdos && i < t.pdos.size(); ++i) {
        const pd::Pdo s = pd::decode_pdo(t.pdos[i]);
        if ((s.type == pd::PdoType::Fixed || s.type == pd::PdoType::Pps) && detail::mw_of(s.max_mv, s.max_ma) == want &&
            o.mv == s.max_mv && o.ma == s.max_ma)
            return true;
    }
    return false;
}

// Rules 1-10. `caps` is the charger's list as in hand (SourceCaps::pdos by position).
inline Outcome predict(const Twin& t, const pd::SourceCaps& caps, const Facts& f) {
    Outcome out;
    if (!caps.valid || caps.num_pdos == 0 || (t.num_pdos == 0 && !t.has_request())) return out;
    pd::Pdo sink[kMaxSinkPdos];
    const std::size_t ns = std::min<std::size_t>(t.num_pdos, kMaxSinkPdos);
    for (std::size_t i = 0; i < ns; ++i) sink[i] = pd::decode_pdo(t.pdos[i]);
    // Rule 6. A twin with no sink PDOs (made from a sniffed Request alone) has only rule 7 to go by.
    if (ns != 0 && (sink[0].type != pd::PdoType::Fixed || sink[0].max_mv != pd::kVsafe5vMv)) return out;
    const bool higher_cap = ns != 0 && (t.pdos[0] & kPdoHigherCapability) != 0;

    detail::Cand best;
    const bool seen = detail::observed_candidate(t, caps, sink, ns, f, best);   // rule 7
    if (!seen && ns != 0) {
        const std::size_t n = std::min<std::size_t>(caps.num_pdos, caps.pdos.size());
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t j = 0; j < ns; ++j) {
                detail::Cand c;
                if (detail::candidate(caps.pdos[i], std::uint8_t(i + 1), sink[j], f, c) && detail::better(c, best)) best = c;
            }
        }
    }
    if (best.kind == Kind::None) return out;

    out.kind = best.kind;
    out.pos = best.pos;
    out.mv = best.mv;
    out.ma = best.ma;
    out.mw = best.mw;
    out.observed = best.observed;
    out.want_mw = want_mw(t);
    out.mismatch = best.lim_ma < best.need_ma || (best.mv == pd::kVsafe5vMv && higher_cap);   // rules 5 and 9

    // Rule 10.
    const bool met = best.observed ? best.lim_ma >= best.need_ma : out.mw >= out.want_mw;
    if (best.observed && (met || out.want_mw == 0)) out.want_mw = detail::mw_of(best.mv, best.need_ma);   // what it asked for
    if (met) {
        out.limiter = rdintent::best_power_w(caps) * 1000u > out.mw ? Limiter::Device : Limiter::None;
    } else {
        out.limiter = best.cable_clip || rdintent::offer_held_by_cable(caps, f.claim_w) ? Limiter::Cable : Limiter::Charger;
    }
    return out;
}

// The Request the live proof sends for this outcome: the position, and for a PPS level the voltage; the current is the one chosen.
inline pd::Choice to_choice(const Outcome& o) {
    pd::Choice c;
    if (o.kind == Kind::None) return c;
    c.pos = o.pos;
    c.mv = o.kind == Kind::Pps ? o.mv : 0;
    c.ma = o.ma;
    return c;
}

}  // namespace k2::twin
