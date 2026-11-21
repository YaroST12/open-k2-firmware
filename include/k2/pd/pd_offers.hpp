// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The automatic offers read of the Charger tab, pure logic (host-tested in tests/test_pd_offers.cpp). A charger that is attached or
// waiting (steady Rp) is read once when the tab is shown, "ultralight": Rd if the K2 has no contract, the source's capabilities, the
// source's EPR list when its PDO 1 has the EPR bit (EPR_Get_Source_Cap in SPR mode, never an EPR entry), then the port goes back as it
// was. Nothing is requested above 5 V and no VDM goes out. core/offers_read.cpp maps each action to its sender.
//   Attach   no contract: present Rd and take the 5 V contract (the pd::Reread attach, no steps)
//   Epr      the source's EPR list (only when PDO 1 has the EPR bit)
//   Release  give back what the attach took (Rd and the contract); never when the K2 already held a contract
//   Abort    the bound hit inside the attach: the attach's own release runs
// One read per presence of the charger: the latch re-arms when the charger was gone kOffersGoneMs (the K2's own Rd cycle does not
// count: it keeps the presence), and two reads are never closer than kOffersGapMs; a Refresh re-arms both.
#pragma once
#include <cstdint>

#include "k2/pd/pd_survey.hpp"

namespace k2::pd {

enum class OrWhy : std::uint8_t { None, NoCharger, Inline, Busy };
enum class OrStep : std::uint8_t { Idle, Attach, Epr, Release, Done, Refused };
enum class OrAct : std::uint8_t { None, Attach, Epr, Release, Abort };

inline constexpr std::uint32_t kOffersTotalMs = 12000;   // the whole read, from its start
inline constexpr std::uint32_t kOffersGoneMs = 3000;     // no charger this long = a new presence
inline constexpr std::uint32_t kOffersGapMs = 60000;     // two automatic reads are never closer than this

struct OrCtx {
    bool present = false;       // a charger is attached or waiting
    bool inline_sink = false;   // passive inline measurement: nothing active, ever
    bool busy = false;          // a charger test, PD trigger or another tool owns the port
    bool contract = false;      // the sink already holds a contract (also a K2 that lives on this port)
    bool epr_capable = false;   // PDO 1 has the EPR bit (known now only with a contract; else learned after the attach)
};

class OffersRead {
public:
    // Every main-loop tick: is the charger there. Absent for kOffersGoneMs re-arms the latch.
    void presence(bool present, std::uint32_t now);
    OrWhy gate(const OrCtx& c) const;                 // why a read may not start now (None = it may)
    bool due(std::uint32_t now) const;                // not running, not yet tried in this presence, gap kept
    void rearm();                                     // a Refresh: read again
    bool start(const OrCtx& c, std::uint32_t now);    // false = refused (why() says); the attempt is latched when it starts
    OrAct poll(std::uint32_t now);                    // the next action to perform, once per step
    void attached(bool caps, bool epr_capable, bool owns_port);   // the Attach ended: caps in hand, EPR bit, Rd or contract taken
    void epr_done(SvOutcome o);
    void released();

    bool running() const { return step_ == OrStep::Attach || step_ == OrStep::Epr || step_ == OrStep::Release; }
    OrStep step() const { return step_; }
    OrWhy why() const { return why_; }
    bool epr_asked() const { return epr_asked_; }
    SvOutcome epr() const { return epr_; }

private:
    void leave_epr();
    OrStep step_ = OrStep::Idle;
    OrWhy why_ = OrWhy::None;
    bool issued_ = false, owns_ = false, epr_asked_ = false, tried_ = false, have_start_ = false, absent_ = false;
    SvOutcome epr_ = SvOutcome::Silent;
    std::uint32_t started_ = 0, absent_since_ = 0;
};

}  // namespace k2::pd
