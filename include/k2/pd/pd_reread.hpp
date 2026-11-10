// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Source-cap re-read sequencer, pure logic (host-tested in tests/test_pd_reread.cpp).
// Some chargers (the UGREEN Nexode X865, whose SPR AVS APDO has not shown up in our source caps) may only
// list everything once the sink has gone through a conversation after the contract: Get_Source_Cap_
// Extended, Discover Identity, Discover SVIDs, then Get_Source_Cap (up to twice). This class runs that, on SOP,
// one message at a time, never retrying: silence, NAK / BUSY / Reject / Not_Supported and a failed transmit all skip
// the step, and the whole run ends by kRereadTotalMs. A mask picks the steps so every experiment is one command.
// The Source_Capabilities the charger answers with is decoded by the engine as always (a longer list replaces the
// old one); this class keeps what the experiment needs to read off: how many PDOs came back and whether any is an
// SPR AVS APDO. The driver owns the wire.
// The survey (pd_survey.hpp) reuses this sequencer for the questions that need the PD wire: its own order (RereadOpts),
// a gap between messages, a longer budget, Discover Modes for the first SVIDs and Get_Manufacturer_Info. A refusal
// (Not_Supported, Reject, NAK) is told apart from silence in notsup_mask.
// With attach the run first waits for a contract (the driver makes one: Rd plus a Request for PDO 1 through the
// single-owner sink path, because a passive K2 never ACKs the caps and the charger never talks to it), then lets the
// line settle for kRereadSettleMs so whatever the charger sends after PS_RDY (Get_Sink_Cap) is heard before our first
// message; the step budget starts after that.
#pragma once
#include <cstdint>
#include <span>

#include "k2/pd/pd_probe.hpp"
#include "k2/pd/pd_sink.hpp"

namespace k2::pd {

// Modes (Discover Modes per SVID) and MfrInfo (Get_Manufacturer_Info) were added for the survey (pd_survey.hpp): the lab
// masks (kRereadDefault, kRereadMaskAll) do not include them, and in the lab order they come after the others.
enum class RereadStep : std::uint8_t { CapsExt, Identity, Svids, GetCap, GetCap2, Modes, MfrInfo, Done };
enum class RereadState : std::uint8_t { Idle, Running, Done, Aborted };

constexpr std::uint8_t reread_bit(RereadStep s) { return std::uint8_t(1u << unsigned(s)); }
inline constexpr std::uint8_t kRereadCapsExt = 1, kRereadIdentity = 2, kRereadSvids = 4, kRereadGetCap = 8, kRereadGetCap2 = 16,
                              kRereadModes = 32, kRereadMfrInfo = 64;
inline constexpr std::uint8_t kRereadDefault = kRereadCapsExt | kRereadIdentity | kRereadSvids | kRereadGetCap;   // one Get_Source_Cap
inline constexpr std::uint8_t kRereadMaskAll = kRereadDefault | kRereadGetCap2;
inline constexpr std::uint8_t kRereadMaskEvery = kRereadMaskAll | kRereadModes | kRereadMfrInfo;
inline constexpr std::uint32_t kRereadRespMs = 250, kRereadTotalMs = 2500;
inline constexpr unsigned kRereadModeSvids = 3;   // Discover Modes goes to at most this many SVIDs of the source
inline constexpr std::uint32_t kRereadAttachMs = 8000;   // longest wait for the contract
inline constexpr std::uint32_t kRereadSettleMs = 1000;   // quiet time after the contract, before our first message

// What the driver tells the sequencer every tick while it attaches.
struct RereadLink {
    bool contract = true;       // the sink holds a contract (Ready)
    std::uint8_t pdos = 0;      // PDOs the engine holds now
    std::uint8_t rev = 0;       // the source's wire spec revision once known (0 = keep the one from start)
    std::span<const std::uint16_t> svids{};   // the source's SVIDs as Discover SVIDs gave them (for the Modes step)
};

// Run options. The lab commands leave them at the default; the survey asks for its own order (the questions that most
// often get an answer first, the PD3-only ones last), a gap between two messages and a longer budget.
struct RereadOpts {
    bool survey_order = false;
    std::uint32_t pace_ms = 0;                   // quiet time between the end of one step (or SVID) and the next message
    std::uint32_t total_ms = kRereadTotalMs;     // the whole run, from the first step
};

struct RereadStatus {
    RereadState state = RereadState::Idle;
    RereadStep step = RereadStep::CapsExt;
    std::uint8_t mask = 0;          // the steps asked for
    std::uint8_t ok_mask = 0;       // bit per RereadStep that was answered
    std::uint8_t skipped_mask = 0;  // bit per asked RereadStep that was refused, ignored or never sent
    std::uint8_t notsup_mask = 0;   // the part of skipped_mask the source refused (Not_Supported, Reject, NAK), not silence or BUSY
    std::uint8_t pdos_before = 0;   // PDOs the engine held when the steps started (after the attach and the settle)
    std::uint8_t pdos_last = 0;     // PDOs in the latest Source_Capabilities received during the run (0 = none)
    std::uint8_t caps_rx = 0;       // Source_Capabilities received during the run
    bool avs_seen = false;          // one of them carried an SPR AVS APDO
    bool attaching = false;         // waiting for the contract (and the settle time): no step has started
    bool attach_failed = false;     // no contract within kRereadAttachMs: nothing was sent
};

// SPR AVS APDO: bits 31:30 = 11 (augmented), bits 29:28 = 10 (SPR AVS). Bits 29:28 = 01 is the EPR AVS APDO.
constexpr bool is_spr_avs_pdo(std::uint32_t w) { return (w >> 28) == 0xE; }

// A re-read's attach can end with its own PDO 1 5 V contract still on the wire (the driver's linger keeps it
// readable for k2_remote pdos). That hold is the re-read's own, not a foreign tool's: a charger-test START (and the
// spoof arming) take it over like the self-hold of a self-powered K2, instead of refusing "tool busy" / "Ra refused:
// contract" (hardware 2026-11-10, b14c1b9: after a plain pd-reread held=True and START was refused "tool busy
// (5100 mV)"; 72c2e28: after pd release the leftover Ready contract still refused the Ra arming, silently). Only the
// exact leftover qualifies - Ready at PDO 1 at vSafe5V, not in EPR mode, still owned by the re-read (the flag any
// other owner, a trigger or a run, clears) - so a user trigger's own hold keeps blocking.
constexpr bool reread_own_hold(const SinkStatus& s, bool reread_made_contract) {
    return reread_made_contract && s.state == SinkState::Ready && s.pos == 1 && s.mv <= kVsafe5vMv && !s.epr_mode;
}

class Reread {
public:
    // rev = wire spec revision of the source (1 = PD 2.0: no extended messages); mask = kReread* steps;
    // pdos_before = the engine's current PDO count, so the result can say "6 -> 7" (with attach it is re-read from
    // the link when the steps begin). attach = wait for the contract and the settle time first.
    void start(std::uint8_t rev, std::uint8_t mask, std::uint8_t pdos_before, std::uint32_t now, bool attach = false,
               RereadOpts opts = {});
    void abort() { if (st_.state == RereadState::Running) st_.state = RereadState::Aborted; st_.attaching = false; tx_pending_ = awaiting_ = false; }
    // A hard reset or detach of the source: ends a run that is past the attach; one still attaching goes on waiting.
    void on_link_reset() { if (!st_.attaching) abort(); }
    bool running() const { return st_.state == RereadState::Running; }

    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_tx_result(bool ok, std::uint32_t now);
    void tick(std::uint32_t now, bool cc_idle, const RereadLink& link = {});
    bool take_tx(TxMsg& out);
    const RereadStatus& status() const { return st_; }

private:
    enum class Reply : std::uint8_t { Got, Refused, Silent };   // how one question ended
    void step_done(bool ok, bool refused = false);
    void query_done(Reply r);   // one Discover Modes question; the Modes step ends with its last SVID
    void advance();
    void build(const RereadLink& link);
    void begin_steps(std::uint32_t now);
    void seek(unsigned order_pos);
    RereadStep step_at(unsigned order_pos) const;
    void no_answer();      // silence, or a transmit nobody acknowledged: the question is over, not a refusal
    RereadStatus st_{};
    RereadOpts opts_{};
    std::uint8_t rev_ = 2;
    std::uint8_t pos_ = 0;                                   // index of the current step in the run's order
    std::uint8_t modes_n_ = 0, modes_i_ = 0, modes_got_ = 0, modes_refused_ = 0;   // the Modes step: SVIDs, the one asked now, outcomes
    std::uint16_t modes_svid_[kRereadModeSvids] = {};
    std::uint32_t started_ = 0, deadline_ = 0, settle_until_ = 0, pace_until_ = 0, now_ = 0;
    bool awaiting_ = false, tx_pending_ = false, tx_sent_ = false, settling_ = false, modes_loaded_ = false;
    TxMsg tx_{};
};

}  // namespace k2::pd
