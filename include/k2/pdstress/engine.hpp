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

enum class StartResult : std::uint8_t { Ok, Busy, NoCaps, Inline, NoSequence, NoStore, Running };
constexpr const char* start_name(StartResult r) {
    switch (r) {
        case StartResult::Ok: return "ok";
        case StartResult::Busy: return "ports busy";
        case StartResult::NoCaps: return "no PD source";
        case StartResult::Inline: return "link inline";
        case StartResult::NoSequence: return "no sequence applies";
        case StartResult::NoStore: return "no record buffer";
        case StartResult::Running: return "already running";
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
    virtual ~Env() = default;
    virtual Snapshot snapshot() = 0;
    virtual const pd::SourceCaps& caps() = 0;
    virtual std::uint32_t cap_mv() = 0;                 // the monitor ceiling a request may ask for
    virtual bool epr_proof() = 0;                       // the cable proof the EPR rows need
    virtual const char* busy() = 0;                     // why another tool owns the ports (nullptr = free)
    virtual pd::Err request(const pd::Choice& c) = 0;   // the sink's own guards apply; Ok = queued
    virtual bool get_caps() = 0;                        // solicited Get_Source_Cap; false = refused
    virtual bool soft_reset() = 0;                      // Soft_Reset on the wire; false = refused
    virtual void release() = 0;                         // back to 5 V
    // Reporting hooks (the firmware logs [STRESS] lines and journals from them; the defaults do nothing).
    virtual void on_seq_start(Seq, std::uint8_t /*idx*/, std::uint8_t /*total*/, std::uint16_t /*steps*/) {}
    virtual void on_seq_end(Seq, const Counts&, bool /*skipped*/) {}
    virtual void on_event(Seq, Event, std::uint32_t /*detail*/) {}
    virtual void on_note(const char*) {}
    virtual void on_finish(Stop) {}
};

// Timing constants of the engine itself (ms).
inline constexpr std::uint32_t kStepMaxMs = 10000;       // a Request not resolved by the sink after this: the engine gives up (Stuck)
inline constexpr std::uint32_t kRecoverMs = 10000;       // VBUS below the dropout line and not back by then: no recovery
inline constexpr std::uint32_t kBusyMs = 3000;           // a refused-for-now Request is retried this long
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

inline void Engine::event(Event e, std::uint32_t detail) {
    if (cur_ < n_order_) bump_event(cur_rec(), e);
    env_->on_event(cur_ < n_order_ ? order_[cur_] : Seq::Ladder, e, detail);
}

inline StartResult Engine::start(const Options& o, std::uint32_t now, const Resume* resume) {
    if (running()) return StartResult::Running;
    if (!rec_.ok()) return StartResult::NoStore;
    if (const char* why = env_->busy()) { env_->on_note(why); return StartResult::Busy; }
    const Snapshot s = env_->snapshot();
    if (s.inline_link) return StartResult::Inline;
    const pd::SourceCaps& caps = env_->caps();
    if (!s.attached || !caps.valid) return StartResult::NoCaps;
    opt_ = o;
    if (opt_.cycles_pct == 0) opt_.cycles_pct = 100;
    resume_ = resume ? *resume : Resume{};
    if (!resume) rec_.clear();
    const Levels L = scan(caps, env_->cap_mv());
    const bool proof = env_->epr_proof();
    n_order_ = 0;
    done_mask_ = skip_mask_ = 0;
    for (std::size_t i = 0; i < kSeqCount; ++i) {
        const Seq q = Seq(i);
        if (!(opt_.mask & seq_bit(q))) { na_[i] = Na::NotSelected; continue; }
        na_[i] = not_applicable(q, caps, L, proof, s.self_powered);
        if (na_[i] == Na::None) order_[n_order_++] = q;
        else skip_mask_ |= seq_bit(q);
    }
    if (n_order_ == 0) return StartResult::NoSequence;
    cur_ = resume ? std::min<std::uint8_t>(resume->next, n_order_) : 0;
    for (std::uint8_t i = 0; i < cur_; ++i) done_mask_ |= seq_bit(order_[i]);
    stop_ = Stop::None;
    cancel_ = false;
    t0_ = now;
    base_ms_ = resume ? resume->elapsed_ms : 0;
    transitions_ = rec_.total(kMaxSeqs).transitions;
    src_hr_ = k2_hr_ = 0;
    w0_ = s.waits_rx; r0_ = s.rejects_rx; s0_ = s.soft_resets_rx; c0_ = s.caps_unsolicited;
    h0_ = s.hard_resets; sh0_ = s.source_hard_resets;
    explained_until_ = drop_since_ = last_top_down_ = release_ms_ = 0;
    dropped_out_ = have_top_down_ = hr_in_step_ = false;
    stop_release_ = true;
    load_polls_ = 0;
    suspect_ = 0;
    release_unconfirmed_ = false;
    dropped_ = 0;
    fail_hold_ = false;
    meas_ = false;
    have_step_ = false;
    contract_mv_ = s.state == pd::SinkState::Ready ? s.mv : 0;
    start_wait_ = ready_ms_ = answer_ms_ = now;
    band_since_ = 0;
    target_ = 0;
    establishing_ = s.state != pd::SinkState::Ready;
    epr_ = s.epr_mode;
    phase_ = Phase::Pace;
    if (resume && cur_ >= n_order_) { finish(Stop::Done, now, true); return StartResult::Ok; }
    if (!begin_seq(now)) finish(Stop::Done, now, true);
    return StartResult::Ok;
}

inline bool Engine::begin_seq(std::uint32_t now) {
    (void)now;
    while (cur_ < n_order_) {
        const Seq q = order_[cur_];
        const std::size_t qi = static_cast<std::size_t>(q);
        if (resume_.given_up[qi]) {   // lost the K2 too often in this sequence: it stays as the journal left it
            env_->on_note("sequence given up after reboots");
            done_mask_ |= seq_bit(q);
            ++cur_;
            continue;
        }
        const pd::SourceCaps& caps = env_->caps();
        L_ = scan(caps, env_->cap_mv());
        cycles_ = scaled_cycles(q, opt_.cycles_pct);
        const Snapshot s = env_->snapshot();
        len_ = length(q, caps, L_, cycles_, env_->epr_proof(), s.self_powered);
        if (len_ == 0) {   // the list changed under us (re-announce): nothing to do here
            env_->on_seq_end(q, counts_of(rec_.rec(qi)), true);
            skip_mask_ |= seq_bit(q);
            ++cur_;
            continue;
        }
        step_ = 0;
        waits_ = fails_ = 0;
        dropped_ = 0;
        replan_ = false;
        caps_epoch_ = caps.rx_count;
        have_step_ = false;
        if (!resume_.seq_losses[qi]) reset(rec_.rec(qi));   // a sequence resumed after a reboot keeps what it had
        env_->on_seq_start(q, std::uint8_t(cur_ + 1), n_order_, len_);
        return true;
    }
    return false;
}

inline void Engine::end_seq(bool skipped) {
    if (cur_ >= n_order_) return;
    finalize_measure();
    const Seq q = order_[cur_];
    done_mask_ |= seq_bit(q);
    env_->on_seq_end(q, counts_of(rec_.rec(static_cast<std::size_t>(q))), skipped);
    ++cur_;
}

inline void Engine::finalize_measure() {
    if (!meas_ || cur_ >= n_order_) { meas_ = false; return; }
    Rec r = cur_rec();
    if (band_seen_) {
        add_sample(r, Metric::Over, excursion_);
        if (band_metric_) add_sample(r, Metric::Band, band_us_ / 1000);
    }
    meas_ = false;
}

inline void Engine::finish(Stop why, std::uint32_t now, bool release) {
    if (phase_ == Phase::Done || phase_ == Phase::Releasing) return;
    if (cur_ < n_order_) end_seq(false);
    stop_ = why;
    if (release) {
        phase_ = Phase::Releasing;
        release_ms_ = now;
        env_->release();
    } else {
        phase_ = Phase::Done;
        env_->on_finish(stop_);
    }
}

inline void Engine::poll(std::uint32_t now) {
    if (!running()) return;
    const Snapshot s = env_->snapshot();
    if (phase_ == Phase::Releasing) { releasing(now, s); return; }
    watch(now, s);
    if (!running() || phase_ == Phase::Releasing) return;
    sample(now, s);
    switch (phase_) {
        case Phase::Pace: pace(now, s); break;
        case Phase::InFlight: flight(now, s); break;
        case Phase::Recover: recover(now, s); break;
        default: break;
    }
}

// The checks that run every poll, whatever the engine is doing: counters the source moved, VBUS, the hard caps and the abort rules.
inline void Engine::watch(std::uint32_t now, const Snapshot& s) {
    if (cancel_) { finish(Stop::Cancelled, now, true); return; }
    if (s.inline_link) { finish(Stop::Inline, now, false); return; }
    if (std::int32_t(now - t0_) >= std::int32_t(opt_.max_ms - std::min(opt_.max_ms, base_ms_))) { finish(Stop::Timeout, now, true); return; }
    if (transitions_ >= opt_.max_transitions) { finish(Stop::MaxTransitions, now, true); return; }

    // What the source did to us.
    auto d16 = [](std::uint16_t now_v, std::uint16_t& base) { const std::uint16_t d = std::uint16_t(now_v - base); base = now_v; return d; };
    for (auto n = d16(s.waits_rx, w0_); n; --n) event(Event::WaitRx);
    for (auto n = d16(s.rejects_rx, r0_); n; --n) event(Event::RejectRx);
    const std::uint16_t soft = d16(s.soft_resets_rx, s0_);
    for (auto n = soft; n; --n) event(Event::SoftResetRx);
    if (const std::uint16_t c = d16(s.caps_unsolicited, c0_)) {
        for (auto n = c; n; --n) event(Event::CapsReannounce);
        replan_ = true;
    }
    const std::uint32_t sh = s.source_hard_resets - sh0_;
    sh0_ = s.source_hard_resets;
    const std::uint32_t kh = s.hard_resets - h0_;
    h0_ = s.hard_resets;
    if (sh) {
        for (std::uint32_t i = 0; i < sh; ++i) event(Event::HardResetRx);
        src_hr_ += sh;
    }
    if (kh) {
        for (std::uint32_t i = 0; i < kh; ++i) event(Event::ContractLost, 1);   // the K2 gave up on the source: a lost contract
        k2_hr_ += kh;
    }
    if (sh || kh || soft) {
        explained_until_ = now + 3000;   // the VBUS dip that follows is the reset's, not a separate dropout
        hr_in_step_ = true;
        if (phase_ == Phase::InFlight || phase_ == Phase::Pace) {
            abandon_ = phase_ == Phase::InFlight;
            phase_ = Phase::Recover;
            drop_since_ = now;
            finalize_measure();
        }
    }
    if (src_hr_ >= 2) { finish(Stop::SourceHardResets, now, true); return; }
    if (k2_hr_ >= 2) { finish(Stop::K2HardResets, now, true); return; }

    // VBUS.
    if (s.vbus_mv < kDropoutMv) {
        if (!dropped_out_) {
            dropped_out_ = true;
            drop_since_ = now;
            if (std::int32_t(now - explained_until_) >= 0) event(Event::VbusDropout);
            if (phase_ == Phase::InFlight || phase_ == Phase::Pace) {
                abandon_ = phase_ == Phase::InFlight;
                phase_ = Phase::Recover;
                finalize_measure();
            }
        }
        if (now - drop_since_ >= kRecoverMs) {
            event(Event::NoRecovery);
            finish(Stop::NoRecovery, now, false);
            return;
        }
    } else if (dropped_out_ && s.vbus_mv >= kBackMv) {
        dropped_out_ = false;
    }
    const std::uint32_t ceiling = pd::overcap_abort_mv(env_->cap_mv());
    if (s.vbus_mv > std::int32_t(ceiling)) { finish(Stop::Overvoltage, now, true); return; }
    if (phase_ == Phase::Pace && !fail_hold_ && contract_mv_ && s.state == pd::SinkState::Ready &&
        now - ready_ms_ >= settle_ms() + 300 && s.vbus_mv > std::int32_t(contract_mv_ + contract_mv_ / 10)) {
        finish(Stop::Overvoltage, now, true);   // settled and still more than 10 % above the target
        return;
    }
    // No load may be drawn (a self-powered K2 reads its own supply current, so the rule is off there).
    if (!s.self_powered && std::abs(s.ibus_ma) > kLoadAbortMa) {
        if (++load_polls_ >= kLoadPolls) { finish(Stop::Load, now, false); return; }
    } else {
        load_polls_ = 0;
    }
}

// VBUS bookkeeping of the transition in flight / just completed.
inline void Engine::sample(std::uint32_t now, const Snapshot& s) {
    (void)now;
    if (s.vbus_mv < kDropoutMv) { band_since_ = 0; return; }
    if (contract_mv_ && in_band(s.vbus_mv, contract_mv_)) { if (!band_since_) band_since_ = now; }
    else band_since_ = 0;
    if (!target_ || (phase_ != Phase::InFlight && !meas_)) return;
    const std::uint32_t t = target_;
    const std::uint32_t band = pd::spec::band_mv(t);
    if (!band_seen_ && in_band(s.vbus_mv, t)) {
        band_seen_ = true;
        const std::uint32_t t0 = req_us_ ? req_us_ : issue_ms_ * 1000u;
        band_us_ = (req_us_ ? s.now_us : now * 1000u) - t0;
        excursion_ = 0;
    }
    if (band_seen_) excursion_ = std::max<std::uint32_t>(excursion_, std::uint32_t(std::abs(s.vbus_mv - std::int32_t(t))));
    // Above the old and the new level together with the transition's own allowance: an overshoot.
    const std::uint32_t hi = std::max<std::uint32_t>(prev_mv_, t) + band + pd::spec::kVSrcValidExtraMv;
    if (s.vbus_mv > std::int32_t(hi) && !over_flag_) { over_flag_ = true; event(Event::VbusBand, std::uint32_t(s.vbus_mv)); }
    // Settled and still outside the band.
    if (meas_ && done_ok_ && !oob_flag_ && now - ready_ms_ >= settle_ms() && !in_band(s.vbus_mv, t)) {
        oob_flag_ = true;
        if (!over_flag_) event(Event::VbusBand, std::uint32_t(s.vbus_mv));
    }
}

inline void Engine::pace(std::uint32_t now, const Snapshot& s) {
    // The caps changed under us (an unsolicited or solicited re-announce): plan again from the list now in force.
    if (replan_ || env_->caps().rx_count != caps_epoch_) {
        replan_ = false;
        caps_epoch_ = env_->caps().rx_count;
        const Seq q = order_[cur_];
        L_ = scan(env_->caps(), env_->cap_mv());
        const std::uint16_t nl = length(q, env_->caps(), L_, cycles_, env_->epr_proof(), s.self_powered);
        if (nl == 0) { env_->on_note("replan: sequence no longer applies"); end_seq(true); if (!begin_seq(now)) finish(Stop::Done, now, true); return; }
        len_ = nl;
    }
    if (establishing_) {
        // No contract yet: ask for 5 V (PDO 1) once, unscored, then start.
        if (s.state == pd::SinkState::Ready) { establishing_ = false; contract_mv_ = s.mv; start_wait_ = now; return; }
        if (now - start_wait_ >= kStartMs) { finish(Stop::NoPd, now, false); return; }
        if (!have_step_ && (s.state == pd::SinkState::Idle || s.state == pd::SinkState::Failed)) {
            Step st;
            st.choice.pos = 1;
            st.target_mv = env_->caps().pdos[0].max_mv;
            st.pace = Pace::Settled;
            have_step_ = true;
            cur_step_ = st;
            issue(now, s, st);
        }
        return;
    }
    if (step_ >= len_) {
        end_seq(false);
        if (!begin_seq(now)) { finish(Stop::Done, now, true); return; }
    }
    auto st = step(order_[cur_], step_, env_->caps(), L_, cycles_, env_->epr_proof(), s.self_powered);
    if (!st) { ++step_; return; }
    if (st->kind == StepKind::Request && st->choice.pos && (dropped_ & (1u << st->choice.pos))) {   // the source said no to this level: it is left out
        ++step_;
        if (!(dropped_ & kDroppedNoted)) { dropped_ |= kDroppedNoted; env_->on_note("level dropped after a reject"); }
        return;
    }
    // The pace this step waits for.
    Pace p = st->pace;
    if (st->kind == StepKind::Request) {
        if (opt_.pace == 1) p = Pace::Settled;
        else if (opt_.pace == 2) p = Pace::Ready;
    }
    if (step_ == 0 || !done_ok_) p = (st->pace == Pace::Spaced) ? Pace::Spaced : Pace::Settled;   // the start of a sequence is calm
    const bool ready = s.state == pd::SinkState::Ready;
    bool go = false;
    if (fail_hold_) {   // the last step failed softly: the old contract may still stand; wait a settle time and a live VBUS
        go = now - fail_ms_ >= kSettledMs && s.vbus_mv >= kBackMv &&
             (ready || s.state == pd::SinkState::Failed);
    } else if (p == Pace::Settled) {
        go = ready && band_since_ && now - band_since_ >= kSettledMs && now - answer_ms_ >= kSettledMs;
        if (contract_mv_ == 0) go = ready && now - answer_ms_ >= kSettledMs;
    } else if (p == Pace::Ready) {
        go = ready && now - ready_ms_ >= kReadyMs;
    } else {
        go = ready && now - answer_ms_ >= kSpacedMs;
    }
    if (!go) return;
    // At most one down-step from the top level per second.
    const std::uint32_t top_mv = L_.top ? env_->caps().pdos[L_.top - 1].max_mv : 0;
    if (st->kind == StepKind::Request && contract_mv_ == top_mv && st->target_mv && st->target_mv < top_mv && have_top_down_ &&
        now - last_top_down_ < kTopDownMs) return;
    if (st->kind == StepKind::Request && contract_mv_ == top_mv && st->target_mv && st->target_mv < top_mv) { last_top_down_ = now; have_top_down_ = true; }
    cur_step_ = *st;
    issue(now, s, *st);
}

inline void Engine::issue(std::uint32_t now, const Snapshot& s, const Step& st) {
    finalize_measure();
    prev_mv_ = std::uint16_t(contract_mv_ ? contract_mv_ : s.mv);
    target_ = st.target_mv;
    ps_rdy_us0_ = s.ps_rdy_us;
    ps_rdy_at0_ = s.ps_rdy_at;
    req_us0_ = s.req_us;
    caps_rx0_ = s.caps_rx;
    rej0_ = s.rejects_rx;
    wait0_ = s.waits_rx;
    band_seen_ = over_flag_ = oob_flag_ = ps_oob_done_ = false;
    done_ok_ = false;
    hr_in_step_ = false;
    seen_not_ready_ = false;
    req_us_ = 0;
    bool queued = false;
    switch (st.kind) {
        case StepKind::Request: {
            const pd::Err e = env_->request(st.choice);
            if (e == pd::Err::Ok) queued = true;
            else if (e == pd::Err::Busy) {
                if (!busy_since_) busy_since_ = now;
                if (now - busy_since_ >= kBusyMs) { busy_since_ = 0; env_->on_note("request kept busy"); finish(Stop::Stuck, now, true); }
                return;
            } else if (e == pd::Err::Inline) { finish(Stop::Inline, now, false); return; }
            else {   // a step the list does not allow any more: skip it, say so
                env_->on_note(pd::err_name(e));
                have_step_ = false;
                if (establishing_) { finish(Stop::NoPd, now, false); return; }
                ++step_;
                return;
            }
            break;
        }
        case StepKind::EprEnter: {
            pd::Choice c;
            c.pos = 0;
            queued = env_->request(c) == pd::Err::Ok;
            if (!queued) { env_->on_note("EPR entry refused"); ++step_; return; }
            break;
        }
        case StepKind::GetCaps:
            if (s.epr_mode) { env_->on_note("no Get_Source_Cap in EPR"); ++step_; return; }
            queued = env_->get_caps();
            if (!queued) {
                if (!busy_since_) busy_since_ = now;
                if (now - busy_since_ >= kBusyMs) { busy_since_ = 0; ++step_; env_->on_note("Get_Source_Cap refused"); }
                return;
            }
            break;
        case StepKind::SoftReset:
            queued = env_->soft_reset();
            if (!queued) { env_->on_note("Soft_Reset refused"); ++step_; return; }
            break;
    }
    if (!queued) return;
    busy_since_ = 0;
    issue_ms_ = now;
    phase_ = Phase::InFlight;
    if (!establishing_) {
        bump_transition(cur_rec());
        ++transitions_;
    }
}

// The source said no (Reject) or kept saying wait: the old contract stands. Pace waits a settle time, then the sequence goes on.
inline void Engine::step_failed(std::uint32_t now, bool drop_level) {
    phase_ = Phase::Pace;
    fail_hold_ = true;
    fail_ms_ = answer_ms_ = now;
    ++fails_;
    finalize_measure();
    // A PPS sequence that is refused twice in a row: the source wants a load for PPS ("needs load"), the sequence ends. A PPS level is
    // never dropped on one Reject (the refusal may be for that one voltage); a fixed level is.
    const bool pps = kSeqs[static_cast<std::size_t>(order_[cur_])].pps;
    if (drop_level && !pps && cur_step_.kind == StepKind::Request && cur_step_.choice.pos) dropped_ |= std::uint16_t(1u << cur_step_.choice.pos);
    if (pps && fails_ >= 2) {
        env_->on_note("PPS refused: needs a load, sequence ends");
        end_seq(true);
        fails_ = 0;
        if (!begin_seq(now)) finish(Stop::Done, now, true);
        return;
    }
    // A Wait-exhausted step is tried again, at most kMaxStepWaits times; a Reject (or the last Wait) moves on.
    if (!drop_level && waits_ < kMaxStepWaits) { ++waits_; return; }
    waits_ = 0;
    ++step_;
}

inline void Engine::complete(std::uint32_t now, const Snapshot& s) {
    Rec r = cur_rec();
    done_ok_ = true;
    fail_hold_ = false;
    fails_ = 0;
    waits_ = 0;
    contract_mv_ = target_ ? target_ : s.mv;
    epr_ = s.epr_mode;
    band_since_ = 0;
    answer_ms_ = now;
    // Only a Request has a PS_RDY stamp of its own; after a Get_Source_Cap or a Soft_Reset the stamp is old, the settle clock starts now.
    ready_ms_ = cur_step_.kind != StepKind::Request || s.ps_rdy_inferred || !s.ps_rdy_at ? now : s.ps_rdy_at;
    if (cur_step_.kind == StepKind::Request) {
        // Accept and PS_RDY latency from the RX edge stamps; a stamp that is missing or not credible falls back to the ms stamps.
        std::int64_t acc_us = -1, rdy_us = -1;
        if (s.req_us && s.accept_us) acc_us = std::int32_t(s.accept_us - s.req_us);
        if (s.accept_us && s.ps_rdy_us) rdy_us = std::int32_t(s.ps_rdy_us - s.accept_us);
        if (acc_us >= 0 && acc_us < std::int64_t(kSuspectAcceptUs)) { ++suspect_; acc_us = -1; }
        if (acc_us < 0 && s.accept_at && s.accept_at >= issue_ms_) acc_us = std::int64_t(s.accept_at - issue_ms_) * 1000;
        if (rdy_us < 0 && s.accept_at && s.ps_rdy_at && s.ps_rdy_at >= s.accept_at) rdy_us = std::int64_t(s.ps_rdy_at - s.accept_at) * 1000;
        if (acc_us >= 0) {
            add_sample(r, Metric::Accept, std::uint32_t(acc_us / 100));
            if (acc_us > std::int64_t(pd::spec::kAcceptLimitMs) * 1000) event(Event::AcceptLate, std::uint32_t(acc_us / 100));
        }
        if (rdy_us >= 0 && !s.ps_rdy_inferred) {
            add_sample(r, Metric::PsRdy, std::uint32_t(rdy_us / 100));
            const std::uint32_t lim = s.epr_mode ? pd::spec::kPsTransitionEprMaxMs : pd::spec::kPsTransitionMaxMs;
            if (rdy_us > std::int64_t(lim) * 1000) event(Event::PsRdyLate, std::uint32_t(rdy_us / 100));
        }
        if (target_ && !in_band(s.vbus_mv, target_)) event(Event::PsRdyOutOfBand, std::uint32_t(s.vbus_mv));
        band_metric_ = target_ && std::abs(std::int32_t(prev_mv_) - std::int32_t(target_)) > std::int32_t(pd::spec::band_mv(target_));
        meas_ = true;
    } else {
        meas_ = false;   // EPR entry / Get_Source_Cap / Soft_Reset: no latency or voltage measurement (only Requests are timed)
    }
    ++step_;
    phase_ = Phase::Pace;
}

inline void Engine::flight(std::uint32_t now, const Snapshot& s) {
    if (now - issue_ms_ >= kStepMaxMs) { env_->on_note("step stuck"); finish(Stop::Stuck, now, true); return; }
    const bool ready = s.state == pd::SinkState::Ready;
    const bool moving = s.state == pd::SinkState::Requesting || s.state == pd::SinkState::Transition ||
                        s.state == pd::SinkState::EprEntering || s.state == pd::SinkState::EprWaitCaps || s.state == pd::SinkState::Aborting;
    switch (cur_step_.kind) {
        case StepKind::GetCaps:
            if (s.caps_rx != caps_rx0_) { complete(now, s); replan_ = true; }
            else if (now - issue_ms_ >= kGetCapsMs) {   // the source never answered a Get_Source_Cap
                event(Event::AcceptLate, kGetCapsMs * 10);
                ++step_;
                phase_ = Phase::Pace;
                answer_ms_ = now;
                done_ok_ = true;
            }
            return;
        case StepKind::SoftReset:
            // Done when the source has re-announced and the K2 is back in a 5 V contract (the driver re-requests PDO 1 itself).
            if (ready && s.pos == 1 && s.caps_rx != caps_rx0_) { complete(now, s); contract_mv_ = s.mv; }
            else if (now - issue_ms_ >= kSoftResetMs) {
                event(Event::ContractLost, 2);
                ++step_;
                fail_hold_ = true;
                fail_ms_ = answer_ms_ = now;
                phase_ = Phase::Pace;
            }
            return;
        case StepKind::EprEnter:
            if (moving) return;
            if (ready && s.epr_mode) { complete(now, s); return; }
            env_->on_note("EPR entry failed: sequence ends");   // the source stays in SPR: nothing else to try in this sequence
            end_seq(true);
            if (!begin_seq(now)) finish(Stop::Done, now, true);
            return;
        case StepKind::Request: break;
    }
    if (!req_us_ && s.req_us && s.req_us != req_us0_) req_us_ = s.req_us;   // the Request really left (TX_START stamp)
    if (moving) return;
    const bool new_rdy = (s.ps_rdy_us != 0 && s.ps_rdy_us != ps_rdy_us0_) || (s.ps_rdy_at != 0 && s.ps_rdy_at != ps_rdy_at0_) || s.ps_rdy_inferred;
    switch (s.result) {
        case pd::SinkResult::Ok:
            if (!ready || s.pos != cur_step_.choice.pos || !new_rdy) return;   // not this Request's PS_RDY (yet)
            if (establishing_) {   // the unscored start-up contract: calm start, nothing measured
                establishing_ = false;
                have_step_ = false;
                target_ = 0;
                meas_ = false;
                contract_mv_ = s.mv;
                answer_ms_ = ready_ms_ = now;
                done_ok_ = false;
                phase_ = Phase::Pace;
                return;
            }
            complete(now, s);
            return;
        case pd::SinkResult::Rejected:
            if (s.rejects_rx == rej0_) return;   // an old verdict, not an answer to this Request
            if (establishing_) { env_->on_note("5 V Request rejected"); finish(Stop::NoPd, now, false); return; }
            step_failed(now, true);
            return;
        case pd::SinkResult::WaitExhausted:
            if (s.waits_rx == wait0_) return;
            if (establishing_) { phase_ = Phase::Pace; have_step_ = false; return; }
            step_failed(now, false);
            return;
        case pd::SinkResult::None:
            return;
        case pd::SinkResult::OverCap:
        case pd::SinkResult::BadChoice:   // the sink refused the step after it was queued: an engine bug, skip it and say so
            env_->on_note("step refused by the sink");
            if (establishing_) { finish(Stop::NoPd, now, false); return; }
            ++step_;
            phase_ = Phase::Pace;
            return;
        case pd::SinkResult::HardReset:
        case pd::SinkResult::SoftReset:
            return;   // the watch counts the reset and moves us to Recover
        default:   // Timeout, TxFailed, Aborted, EprFailed: the contract did not come about without the source saying no
            if (establishing_) { phase_ = Phase::Pace; have_step_ = false; return; }
            event(Event::ContractLost, std::uint32_t(s.result));
            abandon_ = true;
            fail_hold_ = true;
            fail_ms_ = now;
            drop_since_ = now;
            finalize_measure();
            phase_ = Phase::Recover;
            return;
    }
}

// After a reset or a dropout: wait for the 5 V contract to be back, then go on with the next step.
inline void Engine::recover(std::uint32_t now, const Snapshot& s) {
    if (s.vbus_mv < kDropoutMv) return;   // the watch ends the run after kRecoverMs
    if (s.state == pd::SinkState::Ready && s.vbus_mv >= kBackMv && s.attached) {
        contract_mv_ = s.mv;
        done_ok_ = false;   // the next step starts calm
        fail_hold_ = false;
        answer_ms_ = now;
        ready_ms_ = now;
        band_since_ = 0;
        target_ = 0;
        if (abandon_) ++step_;   // the step that was in flight is not repeated
        abandon_ = false;
        waits_ = 0;
        phase_ = Phase::Pace;
        return;
    }
    // VBUS is back but the sink sits without a contract: the source needs to be asked (PDO 1), as at the start.
    if ((s.state == pd::SinkState::Idle || s.state == pd::SinkState::Failed) && s.vbus_mv >= kBackMv && s.attached &&
        now - drop_since_ >= kRecoverIdleMs) {
        if (abandon_) ++step_;
        abandon_ = false;
        establishing_ = true;
        have_step_ = false;
        start_wait_ = now;
        target_ = 0;
        phase_ = Phase::Pace;
        return;
    }
    if (now - drop_since_ >= kRecoverMs) { event(Event::NoRecovery); finish(Stop::NoRecovery, now, false); }
}

inline void Engine::releasing(std::uint32_t now, const Snapshot& s) {
    const bool five = !s.verifying && s.vbus_mv >= kBackMv && std::abs(s.vbus_mv - 5000) <= 500 &&
                      (s.state == pd::SinkState::Idle || s.state == pd::SinkState::Failed || (s.state == pd::SinkState::Ready && s.mv == 5000));
    if (five) { phase_ = Phase::Done; env_->on_finish(stop_); return; }
    if (now - release_ms_ >= kReleaseMs) {
        release_unconfirmed_ = true;
        env_->on_note("5 V release not confirmed");
        phase_ = Phase::Done;
        env_->on_finish(stop_);
    }
}

}  // namespace k2::pdstress
