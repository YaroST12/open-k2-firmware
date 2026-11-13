// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Inline interrogation, pure logic (host-tested in tests/test_pd_interrogate.cpp). The K2 sits inline between a charger and a
// device. To learn the device it asks the device itself, speaking AS THE SOURCE, then hands the link back:
//
//   Reset1   a Hard Reset (skipped when a fresh contract was sniffed: caps -> Request -> Accept -> PS_RDY with no reset since)
//   Sniff1   the renegotiation, sniffed: Source_Capabilities, the device's Request, Accept, PS_RDY (kIqRenegMs at most)
//   Settle   kIqSettleMs after PS_RDY and kIqQuietMs of a quiet line: the source's own post-contract messages (its Discover
//            Identity, a Get_Sink_Cap) go first, and whatever the device answers THEM is kept like an answer to us
//   Ask      Get_Sink_Cap, Get_Sink_Cap_Extended and Get_Revision (PD 3.x), then the VDMs: Discover Identity, Discover SVIDs and
//            Discover Modes (only when the Identity ACK says modal operation); a step the device already answered is not
//            asked. The VDMs go last because a DFP-capable source (a laptop port) may take our DFP-role VDM for a role clash
//            and Hard Reset; whatever the plain questions got is kept by then. A Hard Reset within kIqBlameMs of a question
//            ends the run at once (IqWhy::SourceResets): no final reset, no resync, no second try, so a source that resets
//            when asked is never put into a reset loop. So does a fresh negotiation within kIqRenegBlameMs of a question
//            (the source re-sends Source_Capabilities without a Soft_Reset: IqWhy::SourceRestarts), the laptop-port case.
//   Reset2   a second Hard Reset: the charger's MessageID bookkeeping and the answers it overheard are left behind
//   Sniff2   the renegotiated contract, compared with the first one (IqMatch)
//
// Messages carry the source's power and data role bits and the contract's revision (the lower of the device's Request and the
// source's last header). MessageID rule: the next ID after the newest non-GoodCRC SOP message with the source power role on the
// wire (the source's own or ours), so the device's duplicate filter never drops a question as a retransmission. Every attempt
// takes a fresh ID. A question is answered only by a message with the sink power role (the charger may answer too: ignored)
// that matches it: Sink_Capabilities / Sink_Capabilities_Extended / Revision for the control questions, a structured VDM
// response with the same command and SVID for the VDMs (unless the charger asked the same command just before: that reply is
// the charger's), Not_Supported / Reject / Wait for any. Everything else the device says is kept but is no answer.
// A step is retried once (kIqAttempts) when nothing came back in kIqRespMs; NAK, Not_Supported and Reject end it. The
// transmit result does not count: the device and the charger both GoodCRC our message, the two GoodCRCs collide, and the
// device answers anyway. A Hard Reset from the charger or the device mid-run, not right after a question, re-sniffs the
// contract and goes on (kIqMaxResets). IqStatus::q records, per closed question, what the wire showed: the device's answer,
// the charger's, a bare GoodCRC or nothing.
// Asking stops at kIqTotalMs; the final reset always goes out once anything was sent.
// The driver owns the wire: it feeds every frame (on_message) and every Hard Reset it receives in, transmits take_tx() with
// Auto-CRC OFF (the K2 never GoodCRCs: the charger already does for the device) and the MessageID as given (TxMsg::inline_tx).
#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "k2/pd/pd_engine.hpp"
#include "k2/pd/pd_identify.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_vdm.hpp"

namespace k2::pd {

inline constexpr std::uint32_t kIqRenegMs = 5000;     // Hard Reset -> PS_RDY (bench: OnePlus 120 W + Canon, ~1.13 s)
inline constexpr std::uint32_t kIqSettleMs = 250;     // after PS_RDY, at least
inline constexpr std::uint32_t kIqQuietMs = 150;      // ... and the line quiet this long (an AMS of the source's own runs first)
inline constexpr std::uint32_t kIqSettleMaxMs = 3000; // a line that never goes quiet (a chatty pair): ask anyway, retries cover it
inline constexpr std::uint32_t kIqTxQuietMs = 30;     // no question on top of a frame that just ended
inline constexpr std::uint32_t kIqRespMs = 200;       // an answer window (the device answers in a few ms)
inline constexpr std::uint8_t kIqAttempts = 2;
inline constexpr std::uint32_t kIqTotalMs = 20000;    // from start: no further question after this; the final reset still goes
inline constexpr std::uint8_t kIqMaxResets = 2;       // Hard Resets from the charger or the device tolerated mid-run
inline constexpr std::uint32_t kIqBlameMs = 150;      // a Hard Reset this soon after our last question is the question's doing
inline constexpr std::uint32_t kIqRenegBlameMs = 400; // ... and so is a renegotiation (laptop port: Source_Capabilities ~130 ms after it)
// The e-marker spoof window (IqFacts::spoof). The K2 normally never GoodCRCs inline (the charger's own Auto-CRC is the device's);
// a charger's SOP' query (the cable's identity) is seen passively first, and only then does the driver turn Auto-CRC on for a
// bounded window so the charger's retry gets a GoodCRC and the spoofed reply. Closed in Reset and Ask, and by a fresh
// Source_Capabilities: the questions are asked with Auto-CRC off.
inline constexpr std::uint32_t kIqSoppWindowMs = 300; // a SOP' query opens (and each retry re-opens) the window this long
inline constexpr std::uint32_t kIqSoppReplyMs = 60;   // after our reply: the charger's next frame is its acknowledgement
inline constexpr std::uint32_t kIqSoppMaxMs = 1500;   // from the first query: a charger that keeps asking cannot hold the run
inline constexpr std::size_t kIqEprMaxBytes = 52;     // EPR_Sink_Capabilities kept: two chunks, 13 objects
inline constexpr std::size_t kIqEprMaxPdos = kIqEprMaxBytes / 4;

enum class IqPhase : std::uint8_t { Idle, Reset1, Sniff1, Settle, Ask, Reset2, Sniff2, Done, Aborted };
// The values are bit positions on the wire (iq_bit, the twin protocol): never reorder, append only. The ask order is
// separate (kIqAskOrder): the plain control messages first, the VDMs last.
// Done (6) is a sentinel that is also written raw on the wire, so EprSinkCap (PD 3.x EPR_Get_Sink_Cap) is appended after it.
enum class IqStep : std::uint8_t { Identity, SinkCap, SinkCapExt, Revision, Svids, Modes, Done, EprSinkCap };
inline constexpr std::array<IqStep, 7> kIqAskOrder = {IqStep::SinkCap, IqStep::SinkCapExt, IqStep::Revision, IqStep::EprSinkCap,
                                                      IqStep::Identity, IqStep::Svids,       IqStep::Modes};
// Why a run ended early (with Aborted, or with Done after the final reset of a stopped run). Append only (wire values).
enum class IqWhy : std::uint8_t { None, Stopped, Detached, NotInline, NoContract, Resets, Timeout, SourceResets, SourceRestarts };
// What came back after one question (one attempt).
enum class IqResult : std::uint8_t {
    Pending,      // in flight
    Answered,     // the device (sink power role) answered
    Refused,      // NAK, Not_Supported or Reject
    ChargerOnly,  // nothing from the device, but the charger (source power role) spoke inside the window
    GoodCrcOnly,  // only the device's GoodCRC
    Nothing,      // not even a GoodCRC
    SourceReset,  // the source Hard Reset right after it
    SourceRestart // the source started a fresh negotiation (Source_Capabilities) right after it
};
enum class IqMatch : std::uint8_t { Unknown, Same, Differs, NoSecond };
// Append only (wire values). BadSpoof: the spoof target did not resolve (the caller's check, before the run).
enum class IqRefusal : std::uint8_t { None, Running, NoCc, NotInline, Busy, OwnContract, SelfPowered, NeedConfirm, BadSpoof };

const char* iq_phase_name(IqPhase p);
const char* iq_step_name(IqStep s);
const char* iq_why_name(IqWhy w);
const char* iq_refusal_name(IqRefusal r);
const char* iq_match_name(IqMatch m);
const char* iq_result_name(IqResult r);
constexpr std::uint8_t iq_bit(IqStep s) { return std::uint8_t(1u << unsigned(s)); }

// What the driver and the caller know when a run is asked for.
struct IqFacts {
    bool other_running = false;  // an Identify, a probe, a re-read, a trigger prepare, or this sequencer already runs
    bool cc = false;             // a CC line is attached
    bool inline_link = false;    // a real sink sits behind the K2 (the inline latch or a far Request)
    bool busy = false;           // a charger test, the SuperVOOC trigger, PD Stress, an FC release or a loop probe owns the ports
    bool own_contract = false;   // the K2's own sink holds or makes a contract, or presents Rd for a run
    bool self_powered = false;   // the K2 lives on this VBUS: the reset would cut its own supply
    bool confirmed = false;
    bool spoof = false;          // the e-marker spoof is armed for this run: the first Hard Reset is never skipped (see sopp_window)
};
// The first refusal in this order; None = it may start.
constexpr IqRefusal iq_refusal(const IqFacts& f) {
    if (f.other_running) return IqRefusal::Running;
    if (!f.cc) return IqRefusal::NoCc;
    if (!f.inline_link) return IqRefusal::NotInline;
    if (f.busy) return IqRefusal::Busy;
    if (f.own_contract) return IqRefusal::OwnContract;
    if (f.self_powered) return IqRefusal::SelfPowered;
    if (!f.confirmed) return IqRefusal::NeedConfirm;
    return IqRefusal::None;
}

// A contract sniffed between the charger and the device.
struct IqContract {
    bool ready = false;           // Source_Capabilities -> Request -> Accept -> PS_RDY, in this order
    std::uint8_t pos = 0;         // the Request's object position (1-based)
    std::uint8_t dev_rev = 0;     // the Request header's revision (1 PD 2.0, 2 PD 3.x)
    std::uint32_t rdo = 0;
    std::uint32_t src_pdo = 0;    // the source PDO at `pos`
};
constexpr bool same_contract(const IqContract& a, const IqContract& b) {
    return a.ready && b.ready && a.pos == b.pos && a.rdo == b.rdo && a.src_pdo == b.src_pdo;
}

// The passive half: the contract and the source's MessageID as the wire shows them. Always fed, also while no run is going
// (that is how a fresh contract lets a run skip its first reset).
class IqWatch {
public:
    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_own_tx(std::uint16_t header) { src_id_ = std::uint8_t((header >> 9) & 7); src_known_ = true; }
    void reset();   // a Hard Reset or a detach: nothing of the old link counts
    const IqContract& contract() const { return c_; }
    bool src_known() const { return src_known_; }
    std::uint8_t next_id() const { return src_known_ ? std::uint8_t((src_id_ + 1) & 7) : std::uint8_t(0); }
    std::uint16_t src_roles() const { return src_roles_; }   // header bits 8 (power role) and 5 (data role) of the source
    std::uint8_t src_rev() const { return src_rev_; }
    std::uint32_t psrdy_at() const { return psrdy_at_; }
    std::uint32_t last_frame_at() const { return last_at_; }
    bool heard() const { return heard_; }   // any frame since the reset

private:
    IqContract c_{};
    std::array<std::uint32_t, 7> pdos_{};
    std::uint8_t n_pdos_ = 0;
    bool caps_ = false, requested_ = false, accepted_ = false;
    bool src_known_ = false, heard_ = false;
    std::uint8_t src_id_ = 0, src_rev_ = 0;
    std::uint16_t src_roles_ = 0x0120;   // source, DFP until the wire says otherwise
    std::uint32_t psrdy_at_ = 0, last_at_ = 0;
    std::uint8_t pend_pos_ = 0, pend_rev_ = 0;
    std::uint32_t pend_rdo_ = 0;
};

inline constexpr std::size_t kIqSinkExtBytes = 24;   // Sink_Capabilities_Extended data block

// The latest question that closed (answered, refused, timed out or cut by a reset): what the wire showed in its window.
struct IqQuestion {
    IqStep step = IqStep::Identity;
    IqResult result = IqResult::Pending;
    std::uint8_t id = 0;         // its MessageID
    std::uint8_t attempt = 0;    // 1-based
    std::uint8_t dev_msgs = 0;   // messages from the device (sink power role, no GoodCRC) in the window
    std::uint8_t src_msgs = 0;   // messages from the charger (source power role, no GoodCRC) in the window
    bool dev_goodcrc = false;    // the device GoodCRC'd it
};

struct IqStatus {
    IqPhase phase = IqPhase::Idle;
    IqStep step = IqStep::SinkCap;   // the question in flight; the first of kIqAskOrder
    IqWhy why = IqWhy::None;
    IqMatch match = IqMatch::Unknown;
    std::uint8_t rev = 0;           // the revision the questions were asked in (1 PD 2.0, 2 PD 3.x)
    std::uint8_t asked_mask = 0;    // iq_bit per step that went on the wire at least once
    std::uint8_t ok_mask = 0;       // answered (by us asking, or to the source's own question)
    std::uint8_t refused_mask = 0;  // NAK, Not_Supported or Reject
    std::uint8_t silent_mask = 0;   // asked kIqAttempts times, nothing came back (never together with ok_mask: see late_mask)
    std::uint8_t late_mask = 0;     // answered, but only after the step was given up as silent (a late answer: in ok_mask too)
    std::uint8_t sent = 0;         // questions transmitted (every attempt)
    std::uint8_t dev_acks = 0;      // GoodCRCs the device sent for them (the collision may hide some)
    std::uint8_t resets_sent = 0;   // our Hard Resets
    std::uint8_t resets_seen = 0;   // Hard Resets from the charger or the device mid-run
    bool skipped_first_reset = false;
    IqQuestion q{};                 // the latest closed question
    std::uint8_t closed = 0;        // questions closed so far (wraps): the driver logs `q` when this moves
    IqContract first{}, second{};
    // The answers.
    Identity id{};
    std::array<std::uint16_t, kMaxSvids> svids{};
    std::uint8_t num_svids = 0;
    std::uint8_t num_modes = 0;
    std::array<std::uint16_t, kIdModeSvids> mode_svid{};
    std::array<std::uint32_t, kIdModeSvids> mode_vdo{};
    std::uint8_t num_sink_pdos = 0;
    std::array<std::uint32_t, 7> sink_pdos{};
    std::uint8_t sink_ext_len = 0;  // 0 = none, else the bytes kept (the data size, at most kIqSinkExtBytes)
    std::array<std::uint8_t, kIqSinkExtBytes> sink_ext{};
    std::uint32_t revision = 0;     // Revision data object (0 = none)
    std::uint8_t num_epr_pdos = 0;  // EPR_Sink_Capabilities objects kept (SPR 1..7 first, the EPR ones after)
    std::array<std::uint32_t, kIqEprMaxPdos> epr_pdos{};
    bool epr_partial = false;       // only the first chunk arrived (or the data size beyond kIqEprMaxBytes was cut)
    // The e-marker spoof of this run (IqFacts::spoof): what the charger asked of the cable and what we answered.
    bool spoof = false;
    std::uint8_t sopp_queries = 0;  // Discover Identity requests the charger sent on SOP' (retries included), saturating
    std::uint8_t sopp_replies = 0;  // spoofed SOP' replies that went out
    std::uint32_t started_at = 0, ended_at = 0;
};
constexpr bool iq_finished(IqPhase p) { return p == IqPhase::Done || p == IqPhase::Aborted; }

class Interrogate {
public:
    // Starts a run when iq_refusal(f) is None (the facts' other_running is OR-ed with this run). Returns the refusal.
    IqRefusal start(IqFacts f, std::uint32_t now);
    // A stop from outside. Detached / NotInline end at once (nobody to resync); anything else sends the final reset first
    // when a question already went out.
    void abort(IqWhy why, std::uint32_t now);
    bool running() const { return st_.phase != IqPhase::Idle && !iq_finished(st_.phase); }
    // A reset of this run is in flight or its renegotiation is awaited: VBUS is expected to drop (bounded by kIqRenegMs per
    // sniff), so the driver's VBUS detach rule holds off; the CC rule still ends the run on a real unplug.
    bool resetting() const {
        return st_.phase == IqPhase::Reset1 || st_.phase == IqPhase::Sniff1 || st_.phase == IqPhase::Reset2 || st_.phase == IqPhase::Sniff2;
    }

    void on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void on_hard_reset(std::uint32_t now);   // received on the wire (the charger's or the device's), never our own
    void tick(std::uint32_t now);
    bool take_tx(TxMsg& out);                // a question (TxMsg::inline_tx set) or a Hard Reset
    void on_tx_result(bool acked, std::uint32_t now);
    const IqStatus& status() const { return st_; }
    const IqWatch& watch() const { return watch_; }
    // The spoof window: true while Auto-CRC may be on so the charger's SOP' query gets its GoodCRC and our reply (only in
    // Sniff1, Settle and Sniff2 of a run with IqFacts::spoof). The driver reports each spoofed reply it sent.
    bool sopp_window(std::uint32_t now) const;
    void on_sopp_reply(std::uint32_t now);

private:
    void on_sopp(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now);
    void mark_ok(IqStep s);                  // answered: a step already given up as silent becomes a late answer instead
    void on_epr_chunk(std::span<const std::uint8_t> payload);
    void build_chunk_request();
    bool settled(std::uint32_t now) const;
    void enter_reset(IqPhase ph);
    void to_settle(std::uint32_t now);
    void stop(IqWhy why);                    // the final reset when anything was asked, else the end
    void end(IqPhase ph, IqWhy why);
    bool wanted(IqStep s) const;
    void next_step();
    void modes_next(bool got);
    void refused();
    void attempt_failed();
    void close(IqResult r);                  // the question in flight is over: it becomes IqStatus::q
    void build();
    void on_device(std::uint16_t header, std::span<const std::uint8_t> payload);
    void on_source(std::uint16_t header, std::span<const std::uint8_t> payload);
    void got_answer();                       // the device's message matches the question in flight
    bool take_foreign(std::uint8_t cmd);     // the charger asked this VDM command a moment ago: the reply is its own
    bool asking(IqStep s, std::uint16_t svid = 0) const;   // the question in flight is this one
    std::uint8_t modes_wanted() const;

    IqStatus st_{};
    IqWatch watch_{};
    std::uint32_t now_ = 0;                  // the latest time any entry point was given
    std::uint32_t phase_at_ = 0;
    std::uint32_t deadline_ = 0;
    std::uint32_t tx_at_ = 0;                // when the last question left
    IqQuestion cur_{};                       // the question in flight and what the wire showed since
    bool q_open_ = false;
    std::uint8_t order_idx_ = 0;             // position in kIqAskOrder
    std::uint8_t attempts_ = 0, mode_idx_ = 0, sent_id_ = 0;
    std::uint16_t cur_svid_ = 0;
    bool awaiting_ = false, acked_ = false, tx_pending_ = false;
    bool dirty_ = false;                     // a question went out since the last reset: the final reset is owed
    bool soft_since_tx_ = false;             // the charger sent a Soft_Reset since our last question
    std::uint8_t foreign_mask_ = 0;          // bit per VDM command (Identity, SVIDs, Modes) the charger asked and the device owes
    std::array<std::uint32_t, 3> foreign_at_{};
    IqWhy why_pending_ = IqWhy::None;        // a stop that waits for the final reset
    TxMsg tx_{};
    bool tx_chunk_ = false;                  // tx_ is a chunk request of the question in flight, not a new question
    bool chunk_pending_ = false;             // the first EPR chunk said more follows: the request for it is due
    std::array<std::uint8_t, kIqEprMaxBytes> epr_raw_{};
    std::uint16_t epr_size_ = 0, epr_have_ = 0;   // the data size the device announced, the bytes kept so far
    std::uint32_t sopp_until_ = 0, sopp_first_ = 0;   // the spoof window end; the first query since the last Hard Reset (its budget)
    bool sopp_started_ = false;                       // a query came since the last Hard Reset (sopp_first_ is valid)
};

}  // namespace k2::pd
