// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD limiter, pure logic (host-tested in tests/test_pd_limiter.cpp). The K2 sits inline between a charger and a sink. Armed with a
// power limit P, it sniffs the negotiation and sends the sink a forged Source_Capabilities, speaking AS THE SOURCE: the charger's
// own list with the same count and positions, every object clamped to P. The charger judges the sink's Request by position
// against its REAL list, so a Request for a clamped object is always acceptable to it. SPR only: any sign of EPR stops it.
//
//   Armed        waiting for a real Source_Capabilities (arming mid-contract needs a replug: nothing to forge from)
//   Negotiating  a real list is out: the sink's Request, the charger's Accept (kLimNegMs at most)
//   Pending      PS_RDY seen: send the forged list on a quiet line (kLimQuietMs), at least kLimGapMs after the last one
//   Injected     sent: the sink's next Request decides (kLimVerifyMs)
//   Limited      that Request is within P. A real list again, or a Request above P, starts over
//
// The forged list takes the MessageID two past the charger's newest: the charger's next message (its Accept of the Request our
// list provokes) is the one past it and must differ from ours, or the sink's duplicate filter drops it. Our frame and the
// charger's GoodCRC collide with the sink's: the transmit result means nothing, the Request is the proof.
// Give-up: a Hard or Soft Reset within kLimBlameMs of our list counts against us (kLimMaxRejects, attributed to the side that
// reset: LimWhy::ChargerResets / SinkResets), as does a list the sink answers with another Request above P or not at all
// (SinkIgnores). kLimMaxInjections per arm caps the rate. Nothing loops; the limit lives in RAM only.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_interrogate.hpp"

namespace k2::pd {

inline constexpr std::uint32_t kLimMinMw = 5000, kLimMaxMw = 100000;   // SPR: 5 V x 1 A up to 20 V x 5 A
inline constexpr std::uint32_t kLimQuietMs = 10;       // a quiet line before the forged list
inline constexpr std::uint32_t kLimGapMs = 500;        // between two forged lists
inline constexpr std::uint32_t kLimBlameMs = 150;      // a reset this soon after our list is a rejection
inline constexpr std::uint32_t kLimVerifyMs = 400;     // the sink's Request after our list
inline constexpr std::uint32_t kLimNegMs = 1000;       // a negotiation that never reaches PS_RDY is dropped
inline constexpr std::uint32_t kLimPendMs = 2000;      // a line that never goes quiet
inline constexpr std::uint8_t kLimMaxRejects = 2;
inline constexpr std::uint8_t kLimMaxInjections = 20;

// Arming gate: what the caller knows (another port owner busy) plus what the driver adds (CC, the inline latch, its own engines).
enum class LimRefusal : std::uint8_t { None, BadLimit, NoCc, NotInline, Busy };
struct LimFacts {
    bool cc = false, inline_link = false, busy = false;
};
LimRefusal lim_gate(const LimFacts& f, std::uint32_t limit_mw);
const char* lim_refusal_name(LimRefusal r);

enum class LimState : std::uint8_t { Off, Armed, Negotiating, Pending, Injected, Limited };
enum class LimWhy : std::uint8_t { None, User, Detached, Epr, Rate, ChargerResets, SinkResets, SinkIgnores };
const char* lim_state_name(LimState s);
const char* lim_why_name(LimWhy w);

// The power a Request asks for, mW: fixed = the object's voltage x the operating current, PPS/AVS = the requested voltage x the
// current, battery = the operating power. 0 when the position is not in `pdos` (unknown is never "over").
std::uint32_t request_mw(std::uint32_t rdo, std::span<const std::uint32_t> pdos);

// `real` clamped to `limit_mw`, object for object (same count, same positions); returns the count (at most out.size()).
// Fixed: current = min(I, P/V), 10 mA down. PPS: min(I, P/Vmax), 50 mA down. Battery: power. Variable: P/Vmax. SPR AVS: both
// current fields. The EPR-capable bit of object 1 is cleared (we never take part in EPR); the other flag bits stay as they are.
std::size_t forge_caps(std::span<const std::uint32_t> real, std::uint32_t limit_mw, std::span<std::uint32_t> out);

struct LimStatus {
    LimState state = LimState::Off;
    LimWhy why = LimWhy::None;          // why it is Off (None while armed)
    std::uint32_t limit_mw = 0;
    std::uint8_t injections = 0;
    std::uint8_t rejects = 0;           // resets right after our list, since the last stable Limited
    std::uint8_t ignores = 0;           // lists the sink answered with another too-large Request or nothing
    std::uint8_t forged_n = 0;
    std::array<std::uint32_t, 7> forged{};   // the last list we sent
    std::uint8_t req_pos = 0;           // the sink's latest Request
    std::uint32_t req_rdo = 0;
    std::uint32_t req_mw = 0;
};

class Limiter {
public:
    bool arm(std::uint32_t limit_mw, std::uint32_t now);   // false: outside kLimMinMw..kLimMaxMw
    void disarm() { stop(LimWhy::User); }
    void on_detach() { stop(LimWhy::Detached); }
    bool armed() const { return st_.state != LimState::Off; }
    bool wants_tx() const { return st_.state == LimState::Pending; }   // the driver opens its TX choke point for it
    const LimStatus& status() const { return st_; }

    // Every frame the sniffer sees (GoodCRCs too, they keep the line "busy"), and each Hard Reset.
    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_hard_reset(std::uint32_t now);
    void tick(std::uint32_t now);   // the timeouts
    // The forged list, when it is time: Pending, a quiet line, the gap kept. Moves to Injected; the caller transmits `m`.
    bool take_tx(TxMsg& m, std::uint32_t now);

private:
    void stop(LimWhy why);
    void restart();                                     // a reset: nothing of the old link counts
    void reject(LimWhy cause);
    void blame_check(LimWhy cause, std::uint32_t now);  // a reset or a new list right after our list
    void saw_request(std::uint32_t rdo, std::uint32_t now);

    LimStatus st_{};
    IqWatch watch_{};
    std::uint32_t last_at_ = 0, neg_at_ = 0, ready_at_ = 0, inj_at_ = 0;
};

}  // namespace k2::pd
