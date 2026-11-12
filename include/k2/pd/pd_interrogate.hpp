// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Inline interrogation, pure logic (host-tested in tests/test_pd_interrogate.cpp). The K2 sits inline between a charger and a
// device. To learn the device it asks the device itself, speaking AS THE SOURCE, then hands the link back:
//
//   Reset1   a Hard Reset (skipped when a fresh contract was sniffed: caps -> Request -> Accept -> PS_RDY with no reset since)
//   Sniff1   the renegotiation, sniffed: Source_Capabilities, the device's Request, Accept, PS_RDY (kIqRenegMs at most)
//   Settle   kIqSettleMs after PS_RDY and kIqQuietMs of a quiet line: the source's own post-contract messages (its Discover
//            Identity, a Get_Sink_Cap) go first, and whatever the device answers THEM is kept like an answer to us
//   Ask      Discover Identity, Get_Sink_Cap, Get_Sink_Cap_Extended and Get_Revision (PD 3.x), Discover SVIDs and Discover
//            Modes (only when the Identity ACK says modal operation); a step the device already answered is not asked
//   Reset2   a second Hard Reset: the charger's MessageID bookkeeping and the answers it overheard are left behind
//   Sniff2   the renegotiated contract, compared with the first one (IqMatch)
//
// Messages carry the source's power and data role bits and the contract's revision (the lower of the device's Request and the
// source's last header). MessageID rule: the next ID after the newest non-GoodCRC SOP message with the source power role on the
// wire (the source's own or ours), so the device's duplicate filter never drops a question as a retransmission. Every attempt
// takes a fresh ID. A question is answered only by a message with the sink power role (the charger may answer too: ignored).
// A step is retried once (kIqAttempts) when nothing came back in kIqRespMs; NAK, Not_Supported and Reject end it. The
// transmit result does not count: the device and the charger both GoodCRC our message, the two GoodCRCs collide, and the
// device answers anyway. A Hard Reset from the charger or the device mid-run re-sniffs the contract and goes on (kIqMaxResets).
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

enum class IqPhase : std::uint8_t { Idle, Reset1, Sniff1, Settle, Ask, Reset2, Sniff2, Done, Aborted };
enum class IqStep : std::uint8_t { Identity, SinkCap, SinkCapExt, Revision, Svids, Modes, Done };
// Why a run ended early (with Aborted, or with Done after the final reset of a stopped run).
enum class IqWhy : std::uint8_t { None, Stopped, Detached, NotInline, NoContract, Resets, Timeout };
enum class IqMatch : std::uint8_t { Unknown, Same, Differs, NoSecond };
enum class IqRefusal : std::uint8_t { None, Running, NoCc, NotInline, Busy, OwnContract, SelfPowered, NeedConfirm };

const char* iq_phase_name(IqPhase p);
const char* iq_step_name(IqStep s);
const char* iq_why_name(IqWhy w);
const char* iq_refusal_name(IqRefusal r);
const char* iq_match_name(IqMatch m);
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

struct IqStatus {
    IqPhase phase = IqPhase::Idle;
    IqStep step = IqStep::Identity;
    IqWhy why = IqWhy::None;
    IqMatch match = IqMatch::Unknown;
    std::uint8_t rev = 0;           // the revision the questions were asked in (1 PD 2.0, 2 PD 3.x)
    std::uint8_t asked_mask = 0;    // iq_bit per step that went on the wire at least once
    std::uint8_t ok_mask = 0;       // answered (by us asking, or to the source's own question)
    std::uint8_t refused_mask = 0;  // NAK, Not_Supported or Reject
    std::uint8_t silent_mask = 0;   // asked kIqAttempts times, nothing came back
    std::uint8_t sent = 0;          // questions transmitted (every attempt)
    std::uint8_t dev_acks = 0;      // GoodCRCs the device sent for them (the collision may hide some)
    std::uint8_t resets_sent = 0;   // our Hard Resets
    std::uint8_t resets_seen = 0;   // Hard Resets from the charger or the device mid-run
    bool skipped_first_reset = false;
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

private:
    void enter_reset(IqPhase ph);
    void to_settle(std::uint32_t now);
    void stop(IqWhy why);                    // the final reset when anything was asked, else the end
    void end(IqPhase ph, IqWhy why);
    bool wanted(IqStep s) const;
    void next_step();
    void modes_next(bool got);
    void refused();
    void attempt_failed();
    void build();
    void on_device(std::uint16_t header, std::span<const std::uint8_t> payload);
    bool asking(IqStep s, std::uint16_t svid = 0) const;   // the question in flight is this one
    std::uint8_t modes_wanted() const;

    IqStatus st_{};
    IqWatch watch_{};
    std::uint32_t now_ = 0;                  // the latest time any entry point was given
    std::uint32_t phase_at_ = 0;
    std::uint32_t deadline_ = 0;
    std::uint8_t attempts_ = 0, mode_idx_ = 0, sent_id_ = 0;
    std::uint16_t cur_svid_ = 0;
    bool awaiting_ = false, acked_ = false, tx_pending_ = false;
    bool dirty_ = false;                     // a question went out since the last reset: the final reset is owed
    IqWhy why_pending_ = IqWhy::None;        // a stop that waits for the final reset
    TxMsg tx_{};
};

}  // namespace k2::pd
