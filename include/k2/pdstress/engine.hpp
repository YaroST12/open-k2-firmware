// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress engine: a pure state machine that walks the sequences of plan.hpp against a live source, measures every transition and counts
// what the source does (score.hpp, dist.hpp). It never touches hardware: the Env seam below is the only way out (the firmware binds it
// to fusb302::sink_request and friends, so the sink's own guards - TX-OK gate, caps, monitor ceiling, one Request at a time - always
// apply). poll(now_ms) runs from the main loop. Host-tested in tests/test_pdstress_engine.cpp with a fake Env.
//
// Pacing: Settled waits for the contract, VBUS in the band and kSettledMs of quiet; Ready sends the next Request kReadyMs after the
// source's PS_RDY whatever VBUS is doing. Nothing here sends a message before the one in flight is answered, ignores a Wait's
// pacing, asks for more than advertised or starves a keep-alive: the tool stays inside the PD protocol.
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>

#include "k2/core/math_opt.hpp"
#include "k2/pd/pd_sink.hpp"
#include "k2/pd/pd_spec_timing.hpp"
#include "k2/pdstress/dist.hpp"
#include "k2/pdstress/plan.hpp"
#include "k2/pdstress/score.hpp"

namespace k2::pdstress {

// Everything the engine reads from the K2 in one poll.
struct Snapshot {
    bool attached = false;        // a PD source with valid Source_Capabilities is on the line
    bool inline_link = false;     // a sink sits behind the K2: nothing may be sent
    bool self_powered = false;    // VBUS is the K2's own supply
    pd::SinkState state = pd::SinkState::Idle;
    pd::SinkResult result = pd::SinkResult::None;
    std::uint8_t pos = 0;
    std::uint16_t mv = 0, ma = 0;
    bool epr_mode = false, verifying = false;
    std::uint32_t req_us = 0, accept_us = 0, ps_rdy_us = 0;   // edge stamps (pd_rxstamp.hpp), 0 = unknown, bit 0 set when real
    std::uint32_t accept_at = 0, ps_rdy_at = 0;               // the sink's ms stamps (the fallback when a stamp is unusable)
    bool ps_rdy_inferred = false;
    std::uint16_t waits_rx = 0, rejects_rx = 0, soft_resets_rx = 0, caps_unsolicited = 0;
    std::uint32_t hard_resets = 0, source_hard_resets = 0;    // K2-sent / source-sent
    std::uint32_t caps_rx = 0;
    std::int32_t vbus_mv = 0, ibus_ma = 0;                    // power monitor, the latest sample
    std::uint32_t now_us = 0;
};

// How a run ended.
enum class Stop : std::uint8_t {
    None, Done, Cancelled, Timeout, MaxTransitions, SourceHardResets, K2HardResets, Overvoltage, Load, NoRecovery, Stuck, NoPd, Inline,
    TooManyLosses
};
constexpr const char* stop_name(Stop s) {
    switch (s) {
        case Stop::None: return "running";
        case Stop::Done: return "done";
        case Stop::Cancelled: return "cancelled";
        case Stop::Timeout: return "run time cap";
        case Stop::MaxTransitions: return "transition cap";
        case Stop::SourceHardResets: return "second source hard reset";
        case Stop::K2HardResets: return "second K2 hard reset";
        case Stop::Overvoltage: return "VBUS over the limit";
        case Stop::Load: return "load on the output";
        case Stop::NoRecovery: return "no recovery without a replug";
        case Stop::Stuck: return "step stuck";
        case Stop::NoPd: return "no PD contract";
        case Stop::Inline: return "link inline";
        case Stop::TooManyLosses: return "too many K2 reboots";
    }
    return "?";
}

// NoMonitor is the session's refusal (no power monitor reading, so no VBUS judgement and no overvoltage guard); the engine never returns it.
enum class StartResult : std::uint8_t { Ok, Busy, NoCaps, Inline, NoSequence, NoStore, Running, NoMonitor };
constexpr const char* start_name(StartResult r) {
    switch (r) {
        case StartResult::Ok: return "ok";
        case StartResult::Busy: return "ports busy";
        case StartResult::NoCaps: return "no PD source";
        case StartResult::Inline: return "link inline";
        case StartResult::NoSequence: return "no sequence applies";
        case StartResult::NoStore: return "no record buffer";
        case StartResult::Running: return "already running";
        case StartResult::NoMonitor: return "no power monitor";
    }
    return "?";
}

struct Options {
    std::uint16_t mask = kDefaultSeqMask;
    std::uint16_t cycles_pct = 100;    // scales every sequence's cycle count (1 minimum)
    std::uint8_t pace = 0;             // 0 = per sequence, 1 = all Settled, 2 = all Ready (Reannounce stays Spaced)
    std::uint32_t max_ms = 900000;     // hard cap on the run time
    std::uint16_t max_transitions = 1200;
};

// Options from the numbers the remote start op and the shell carry: 0 in a field = the default; a mask is cut to the known sequences
// (an empty cut = the default mask); a time cap is in seconds; the pace is clamped to 0..2.
constexpr Options make_options(std::uint32_t mask, std::uint32_t cycles_pct, std::uint32_t pace, std::uint32_t max_s, std::uint32_t max_trans) {
    Options o;
    const std::uint16_t m = static_cast<std::uint16_t>(mask & kAllSeqMask);
    if (m != 0) o.mask = m;
    if (cycles_pct != 0) o.cycles_pct = static_cast<std::uint16_t>(cycles_pct > 0xFFFF ? 0xFFFF : cycles_pct);
    o.pace = static_cast<std::uint8_t>(pace > 2 ? 2 : pace);
    if (max_s != 0) o.max_ms = max_s * 1000u;
    if (max_trans != 0) o.max_transitions = static_cast<std::uint16_t>(max_trans > 0xFFFF ? 0xFFFF : max_trans);
    return o;
}

// What a sequence's journal line carries across a reboot (journal.hpp).
struct Resume {
    std::uint8_t next = 0;             // order index to continue at
    std::uint8_t losses = 0;           // reboots of this run so far
    std::uint32_t elapsed_ms = 0;
    std::array<std::uint8_t, kMaxSeqs> seq_losses{};
    std::array<std::uint8_t, kMaxSeqs> given_up{};
};

class Env {
public:
    virtual Snapshot snapshot() = 0;
    virtual const pd::SourceCaps& caps() = 0;
    virtual std::uint32_t cap_mv() = 0;                 // the monitor ceiling a request may ask for
    virtual bool epr_proof() = 0;                       // the cable proof the EPR rows need
    virtual const char* busy() = 0;                     // why another tool owns the ports (nullptr = free)
    virtual pd::Err request(const pd::Choice& c) = 0;   // the sink's own guards apply; Ok = queued
    virtual bool get_caps() = 0;                        // solicited Get_Source_Cap; false = refused
    virtual bool soft_reset() = 0;                      // Soft_Reset on the wire; false = refused
    virtual void release() = 0;                         // back to 5 V
    // Asked at every poll while a sequence has just begun, before its first step: true = go on. The firmware writes its journal here
    // (on a quiet link) and says false until that is done. The engine waits at most kBoundaryMs.
    virtual bool boundary_ok(std::uint32_t /*now_ms*/) { return true; }
    // Reporting hooks (the firmware logs [STRESS] lines and journals from them; the defaults do nothing).
    virtual void on_seq_start(Seq, std::uint8_t /*idx*/, std::uint8_t /*total*/, std::uint16_t /*steps*/) {}
    virtual void on_seq_end(Seq, const Counts&, bool /*skipped*/) {}
    virtual void on_event(Seq, Event, std::uint32_t /*detail*/) {}
    virtual void on_note(const char*) {}
    virtual void on_finish(Stop) {}

protected:
    ~Env() = default;   // never deleted through the interface: a virtual destructor would pull operator delete (and malloc) into the link
};

// Timing constants of the engine itself (ms).
inline constexpr std::uint32_t kStepMaxMs = 10000;       // a Request not resolved by the sink after this: the engine gives up (Stuck)
inline constexpr std::uint32_t kRecoverMs = 10000;       // VBUS below the dropout line and not back by then: no recovery
inline constexpr std::uint32_t kBoundaryMs = 2000;       // the longest the first step of a sequence waits for the Env's journal write
inline constexpr std::uint32_t kBusyMs = 3000;          // a refused-for-now Request is retried this long
inline constexpr std::uint32_t kTopDownMs = 1000;        // at most one down-step from the top level per second
inline constexpr std::uint32_t kReleaseMs = 6000;        // the final 5 V release must be confirmed by then
inline constexpr std::uint32_t kStartMs = 10000;         // a contract must exist by then
inline constexpr std::uint32_t kGetCapsMs = 3000;        // a Get_Source_Cap the source has not answered by then counts as late
inline constexpr std::uint32_t kSoftResetMs = 6000;      // a K2 Soft_Reset must be back in a 5 V contract by then
inline constexpr std::uint16_t kDroppedNoted = 0x8000;   // bit 15 of dropped_: the "level dropped" note went out once in this sequence
inline constexpr std::uint32_t kRecoverIdleMs = 1500;   // after a reset the sink gets this long to Request PDO 1 itself before the engine does
inline constexpr std::int32_t kDropoutMv = 1000;
inline constexpr std::int32_t kBackMv = 4000;            // VBUS counts as back above this
inline constexpr std::int32_t kLoadAbortMa = 100;
inline constexpr std::uint8_t kLoadPolls = 5;
inline constexpr std::uint8_t kMaxStepWaits = 2;         // a step Wait-exhausted this often is dropped
inline constexpr std::uint8_t kMaxSeqLosses = 2;         // reboots in one sequence before it is given up
inline constexpr std::uint8_t kMaxRunLosses = 4;         // reboots in one run
inline constexpr std::uint32_t kSuspectAcceptUs = 300;   // an Accept faster than this after the Request is not credible: the ms stamps are used

class Engine {
public:
    Engine(Env& env, Record rec) : env_(&env), rec_(rec) {}

    StartResult start(const Options& o, std::uint32_t now_ms, const Resume* resume = nullptr);
    void cancel() { if (running()) cancel_ = true; }
    void poll(std::uint32_t now_ms);

    bool running() const { return phase_ != Phase::Off && phase_ != Phase::Done; }
    bool finished() const { return phase_ == Phase::Done; }
    Stop stop() const { return stop_; }
    std::uint8_t seq_index() const { return cur_; }            // position in the order list (0-based)
    std::uint8_t seq_total() const { return n_order_; }
    Seq seq() const { return order_[cur_ < n_order_ ? cur_ : 0]; }
    std::uint16_t step_index() const { return step_; }
    std::uint16_t step_total() const { return len_; }
    std::uint32_t elapsed_ms(std::uint32_t now_ms) const { return base_ms_ + (now_ms - t0_); }
    std::uint32_t transitions() const { return transitions_; }
    bool release_unconfirmed() const { return release_unconfirmed_; }
    std::uint16_t stamp_suspect() const { return suspect_; }
    Na na(Seq s) const { return na_[static_cast<std::size_t>(s)]; }
    const Options& options() const { return opt_; }
    const Record& record() const { return rec_; }
    Counts total() const { return rec_.total(kMaxSeqs); }
    std::uint8_t reboots() const { return resume_.losses; }
    const Resume& resume() const { return resume_; }          // the reboot accounting the journal carries (next / elapsed are the caller's)
    std::uint8_t given_up(Seq s) const { return resume_.given_up[static_cast<std::size_t>(s)]; }
    // The order list (what will run), for status and the journal.
    Seq order(std::uint8_t i) const { return order_[i]; }
    bool seq_done(Seq s) const { return done_mask_ & seq_bit(s); }
    bool seq_skipped(Seq s) const { return skip_mask_ & seq_bit(s); }
    Counts seq_counts(Seq s) const { return counts_of(rec_.rec(static_cast<std::size_t>(s))); }

private:
    enum class Phase : std::uint8_t { Off, Pace, InFlight, Recover, Releasing, Done };

    void watch(std::uint32_t now, const Snapshot& s);
    void pace(std::uint32_t now, const Snapshot& s);
    void flight(std::uint32_t now, const Snapshot& s);
    void recover(std::uint32_t now, const Snapshot& s);
    void releasing(std::uint32_t now, const Snapshot& s);
    bool begin_seq(std::uint32_t now);                 // false = nothing left
    void end_seq(bool skipped);
    void issue(std::uint32_t now, const Snapshot& s, const Step& st);
    void complete(std::uint32_t now, const Snapshot& s);
    void step_failed(std::uint32_t now, bool drop_level);
    void sample(std::uint32_t now, const Snapshot& s);
    void finalize_measure();
    void finish(Stop why, std::uint32_t now, bool release);
    void event(Event e, std::uint32_t detail = 0);
    Rec cur_rec() { return rec_.rec(static_cast<std::size_t>(order_[cur_])); }
    bool in_band(std::int32_t vbus, std::uint32_t target) const {
        return std::abs(vbus - std::int32_t(target)) <= std::int32_t(pd::spec::band_mv(target));
    }
    std::uint32_t settle_ms() const { return epr_ ? pd::spec::kSrcSettleEprMs : pd::spec::kSrcSettleSprMs; }

    Env* env_;
    Record rec_;
    Options opt_{};
    Resume resume_{};
    Phase phase_ = Phase::Off;
    Stop stop_ = Stop::None;
    bool cancel_ = false;
    std::uint32_t t0_ = 0, base_ms_ = 0;
    std::array<Seq, kMaxSeqs> order_{};
    std::array<Na, kSeqCount> na_{};
    std::uint8_t n_order_ = 0, cur_ = 0;
    std::uint16_t done_mask_ = 0, skip_mask_ = 0;
    // sequence in progress
    Levels L_{};
    std::uint16_t cycles_ = 1, step_ = 0, len_ = 0;
    std::uint32_t caps_epoch_ = 0;
    bool replan_ = false;
    std::uint16_t dropped_ = 0;          // bit per position the source Rejected (or kept Waiting on)
    std::uint8_t waits_ = 0, fails_ = 0;
    bool epr_ = false;
    // the transition in flight / just completed
    Step cur_step_{};
    bool establishing_ = false, have_step_ = false;
    bool boundary_ = false;                // a sequence has just begun: its first step waits for Env::boundary_ok()
    std::uint32_t boundary_ms_ = 0;
    std::uint32_t issue_ms_ = 0, busy_since_ = 0;
    std::uint32_t req_us0_ = 0, ps_rdy_us0_ = 0, ps_rdy_at0_ = 0, caps_rx0_ = 0, req_us_ = 0;
    std::uint16_t prev_mv_ = 0, target_ = 0;
    std::uint16_t rej0_ = 0, wait0_ = 0;   // the sink's Reject / Wait counters when the step went out (a verdict needs them to have moved)
    bool abandon_ = false;                 // the Recover phase was entered with a step in flight: that step is not repeated
    std::uint32_t contract_mv_ = 0;
    bool done_ok_ = false, seen_not_ready_ = false;
    std::uint32_t ready_ms_ = 0, answer_ms_ = 0, fail_ms_ = 0, band_since_ = 0;
    bool fail_hold_ = false;
    // measurement of the completed transition (open until the next one is issued)
    bool meas_ = false, band_seen_ = false, band_metric_ = false, over_flag_ = false, oob_flag_ = false, ps_oob_done_ = false;
    std::uint32_t band_us_ = 0;
    std::uint32_t excursion_ = 0;
    // global watch
    std::uint16_t w0_ = 0, r0_ = 0, s0_ = 0, c0_ = 0;
    std::uint32_t h0_ = 0, sh0_ = 0;
    std::uint32_t transitions_ = 0, src_hr_ = 0, k2_hr_ = 0;
    std::uint32_t explained_until_ = 0, drop_since_ = 0, last_top_down_ = 0, start_wait_ = 0, release_ms_ = 0;
    bool dropped_out_ = false, have_top_down_ = false, hr_in_step_ = false, stop_release_ = true;
    std::uint8_t load_polls_ = 0;
    std::uint16_t suspect_ = 0;
    bool release_unconfirmed_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------------------------

}  // namespace k2::pdstress
