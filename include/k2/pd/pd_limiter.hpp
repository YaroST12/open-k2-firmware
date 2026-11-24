// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD limiter, pure logic (host-tested in tests/test_pd_limiter.cpp). The K2 sits inline between a charger and a sink. Armed with a
// power / per-technology limit, it sniffs the negotiation and sends the sink a forged Source_Capabilities, speaking AS THE SOURCE:
// keeping original PDO positions, removing or neutering objects outside the limit and clamping kept currents/ranges. The charger
// judges the sink's Request by position against its REAL list, so a Request for a kept/clamped object is always acceptable to it.
// SPR only: any sign of EPR stops it.
//
//   Armed        waiting for a real Source_Capabilities (nothing attached, a detach and a replug all land here: the limit stays)
//   Negotiating  a real list is out: the sink's Request, the charger's Accept (kLimNegMs at most)
//   Pending      PS_RDY seen: send the forged list on a quiet line (kLimQuietMs), at least kLimGapMs after the last one
//   Injected     sent: the sink's next Request decides (kLimVerifyMs)
//   Limited      that Request complies with the forged list. A real list again, or a non-compliant Request, starts over
//
// The forged list takes a fresh MessageID past the charger's newest (skipping at least +1 ID so the charger's Accept uses a
// distinct ID, advancing further on retries so the sink's duplicate filter never drops it). Our frame and the charger's GoodCRC
// collide with the sink's: the transmit result means nothing, the Request is the proof.
// Give-up: a Hard or Soft Reset within kLimBlameMs of our list counts against us (kLimMaxRejects, attributed to the side that
// reset: LimWhy::ChargerResets / SinkResets), as does a list the sink answers with a non-compliant Request or not at all
// (SinkIgnores). kLimMaxInjections per arm caps the rate. Nothing loops; the runtime state lives in RAM only (except the short-lived
// Hard Reset comeback record when bus-powered).
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_interrogate.hpp"

namespace k2::pd {

inline constexpr std::uint32_t kLimMinMw = 5000, kLimMaxMw = 100000;   // SPR: 5 V x 1 A up to 20 V x 5 A
inline constexpr std::uint16_t kLimStepMa = 100;       // 100 mA current ceiling granularity (divides both 10 mA and 50 mA PDO units)
inline constexpr std::uint16_t kLimPpsStepMv = 1000;   // 1 V step in the UI PPS ceiling picker
inline constexpr std::uint16_t kLimMinMv = 3300, kLimMaxMv = 21000;
inline constexpr std::uint16_t kLimMaxMa = 5000;
inline constexpr std::uint16_t kLimMinFixedMa = 100;   // minimum mandatory 5 V Fixed current when Fixed mode is off
inline constexpr std::uint32_t kLimQuietMs = 50;       // a quiet line before the forged list
inline constexpr std::uint32_t kLimGapMs = 500;        // between two forged lists
inline constexpr std::uint32_t kLimBlameMs = 150;      // a reset this soon after our list is a rejection
inline constexpr std::uint32_t kLimVerifyMs = 400;     // the sink's Request after our list
inline constexpr std::uint32_t kLimNegMs = 1000;       // a negotiation that never reaches PS_RDY is dropped
inline constexpr std::uint32_t kLimPendMs = 2000;      // a line that never goes quiet
inline constexpr std::uint8_t kLimMaxRejects = 2;
inline constexpr std::uint8_t kLimMaxInjections = 20;

struct LimTech {
    bool enabled = true;
    std::uint16_t max_mv = 0;   // 0 = uncapped for this tech
    std::uint16_t max_ma = 0;   // 0 = uncapped for this tech (kLimStepMa steps)
    constexpr bool operator==(const LimTech&) const = default;
};

struct LimConfig {
    LimTech fixed{}, pps{}, avs{};
    std::uint32_t limit_mw = 0;   // 0 = no overall watt ceiling
    constexpr bool operator==(const LimConfig&) const = default;
};

constexpr LimConfig lim_config_watts(std::uint32_t mw) { return {LimTech{}, LimTech{}, LimTech{}, mw}; }
constexpr bool lim_config_active(const LimConfig& c) {
    return c.limit_mw != 0 || !c.fixed.enabled || c.fixed.max_mv != 0 || c.fixed.max_ma != 0 ||
           !c.pps.enabled || c.pps.max_mv != 0 || c.pps.max_ma != 0 ||
           !c.avs.enabled || c.avs.max_mv != 0 || c.avs.max_ma != 0;
}
bool lim_config_valid(const LimConfig& c);

// Arming gate: valid config and "another feature owns the wire". Nothing attached, or only a charger, is no refusal: it waits.
enum class LimRefusal : std::uint8_t { None, BadLimit, Busy };
struct LimFacts {
    bool busy = false;
};
LimRefusal lim_gate(const LimFacts& f, const LimConfig& cfg);
LimRefusal lim_gate(const LimFacts& f, std::uint32_t limit_mw);
const char* lim_refusal_name(LimRefusal r);

// Compact human label (at most kLimLabelMax chars, e.g. "27W", "9V3A", "PPS11", "AVS15"). Returns length written.
inline constexpr std::size_t kLimLabelMax = 5;
std::size_t lim_label(const LimConfig& cfg, std::span<char> out);

// The picker's levels: whole watts of the charger's own fixed offers below its best, lowest first. mv/ma is the offer that
// sets the level (the first of equal powers). Offers outside kLimMinMw..kLimMaxMw are dropped, never clamped.
struct LimPick {
    std::uint32_t mw = 0;
    std::uint16_t mv = 0, ma = 0;
};
inline constexpr std::size_t kLimMaxPicks = 7;
std::size_t lim_picks(std::span<const Pdo> pdos, std::span<LimPick> out);   // count written
constexpr std::uint32_t lim_watts(std::uint32_t mw) { return mw / 1000; }   // every label floors

enum class LimState : std::uint8_t { Off, Armed, Negotiating, Pending, Injected, Limited };
enum class LimWhy : std::uint8_t { None, User, Detached, Epr, Rate, ChargerResets, SinkResets, SinkIgnores };
const char* lim_state_name(LimState s);
const char* lim_why_name(LimWhy w);

// The power a Request asks for, mW: fixed = the object's voltage x the operating current, PPS/AVS = the requested voltage x the
// current, battery = the operating power. 0 when the position is not in `pdos` (unknown is never "over").
std::uint32_t request_mw(std::uint32_t rdo, std::span<const std::uint32_t> pdos);

// True when `rdo` stays inside the forged PDO at its position (and `cfg.limit_mw` if set). Never accepts neutered 0 mA slots.
bool request_complies(std::uint32_t rdo, std::span<const std::uint32_t> real_pdos,
                      std::span<const std::uint32_t> forged_pdos, const LimConfig& cfg);

// `real` transformed to `cfg`, keeping exact PDO positions up to the highest enabled/kept object (trailing disabled objects are
// truncated; non-trailing disabled objects are neutered in-place to 0 mA/0 mW). The EPR-capable bit of object 1 is cleared.
std::size_t forge_caps(std::span<const std::uint32_t> real, const LimConfig& cfg, std::span<std::uint32_t> out);
std::size_t forge_caps(std::span<const std::uint32_t> real, std::uint32_t limit_mw, std::span<std::uint32_t> out);
bool forge_differs(std::span<const std::uint32_t> real, std::span<const std::uint32_t> forged);

struct LimStatus {
    LimState state = LimState::Off;
    LimWhy why = LimWhy::None;          // why it is Off (None while armed)
    std::uint32_t limit_mw = 0;         // mirrored cfg.limit_mw (for existing wire layout / logs)
    LimConfig cfg{};
    std::uint8_t injections = 0;
    std::uint8_t rejects = 0;           // resets right after our list, since the last stable Limited
    std::uint8_t ignores = 0;           // lists the sink answered with another too-large Request or nothing
    std::uint8_t forged_n = 0;
    std::array<std::uint32_t, 7> forged{};   // the last list we sent
    std::uint8_t req_pos = 0;           // the sink's latest Request
    std::uint32_t req_rdo = 0;
    std::uint32_t req_mw = 0;
};

constexpr LimConfig lim_effective_config(const LimStatus& s) {
    return (s.limit_mw != 0 && !lim_config_active(s.cfg)) ? lim_config_watts(s.limit_mw) : s.cfg;
}

class Limiter {
public:
    // false: invalid config. `seed` is the always-fed sniffer of this attach: a contract it holds is limited at once.
    bool arm(const LimConfig& cfg, std::uint32_t now, const IqWatch* seed = nullptr);
    bool arm(std::uint32_t limit_mw, std::uint32_t now, const IqWatch* seed = nullptr);
    void disarm() { stop(LimWhy::User); }
    void on_detach();   // the pair is gone: the limit waits for the next one, the sink's Request and our list go with the attach
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
    bool forged_differs_from_watch(std::array<std::uint32_t, 7>& f, std::size_t& cnt) const;

    LimStatus st_{};
    IqWatch watch_{};
    std::uint32_t last_at_ = 0, neg_at_ = 0, ready_at_ = 0, inj_at_ = 0;
    std::uint8_t inj_streak_ = 0;
    bool sink_asked_caps_ = false;
};

}  // namespace k2::pd
