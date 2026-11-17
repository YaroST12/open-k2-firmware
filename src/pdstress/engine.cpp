// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Stress engine state machine (out-of-line in src/pdstress/ so COLD_SIZE_FLAGS applies).
#include "k2/pdstress/engine.hpp"

namespace k2::pdstress {

void Engine::event(Event e, std::uint32_t detail) {
    if (cur_ < n_order_) bump_event(cur_rec(), e);
    env_->on_event(cur_ < n_order_ ? order_[cur_] : Seq::Ladder, e, detail);
}

StartResult Engine::start(const Options& o, std::uint32_t now, const Resume* resume) {
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
    if (resume && resume->losses >= kMaxRunLosses) { finish(Stop::TooManyLosses, now, true); return StartResult::Ok; }
    if (resume && cur_ >= n_order_) { finish(Stop::Done, now, true); return StartResult::Ok; }
    if (!begin_seq(now)) finish(Stop::Done, now, true);
    return StartResult::Ok;
}

bool Engine::begin_seq(std::uint32_t now) {
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
        boundary_ = true;
        boundary_ms_ = now;
        return true;
    }
    return false;
}

void Engine::end_seq(bool skipped) {
    if (cur_ >= n_order_) return;
    finalize_measure();
    const Seq q = order_[cur_];
    done_mask_ |= seq_bit(q);
    env_->on_seq_end(q, counts_of(rec_.rec(static_cast<std::size_t>(q))), skipped);
    ++cur_;
}

void Engine::finalize_measure() {
    if (!meas_ || cur_ >= n_order_) { meas_ = false; return; }
    Rec r = cur_rec();
    if (band_seen_) {
        add_sample(r, Metric::Over, excursion_);
        if (band_metric_) add_sample(r, Metric::Band, band_us_ / 1000);
    }
    meas_ = false;
}

void Engine::finish(Stop why, std::uint32_t now, bool release) {
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

void Engine::poll(std::uint32_t now) {
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
void Engine::watch(std::uint32_t now, const Snapshot& s) {
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
void Engine::sample(std::uint32_t now, const Snapshot& s) {
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

void Engine::pace(std::uint32_t now, const Snapshot& s) {
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
    // A sequence boundary: the Env may hold the first step for its journal write (it asks for a quiet link); bounded, so a chatty
    // source cannot stall the run.
    if (boundary_) {
        if (!env_->boundary_ok(now) && now - boundary_ms_ < kBoundaryMs) return;
        boundary_ = false;
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

void Engine::issue(std::uint32_t now, const Snapshot& s, const Step& st) {
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
void Engine::step_failed(std::uint32_t now, bool drop_level) {
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

void Engine::complete(std::uint32_t now, const Snapshot& s) {
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
            const std::uint32_t acc_t = math::div_u64_to_u32(static_cast<std::uint64_t>(acc_us), 100u);
            add_sample(r, Metric::Accept, acc_t);
            if (acc_us > std::int64_t(pd::spec::kAcceptLimitMs) * 1000) event(Event::AcceptLate, acc_t);
        }
        if (rdy_us >= 0 && !s.ps_rdy_inferred) {
            const std::uint32_t rdy_t = math::div_u64_to_u32(static_cast<std::uint64_t>(rdy_us), 100u);
            add_sample(r, Metric::PsRdy, rdy_t);
            const std::uint32_t lim = s.epr_mode ? pd::spec::kPsTransitionEprMaxMs : pd::spec::kPsTransitionMaxMs;
            if (rdy_us > std::int64_t(lim) * 1000) event(Event::PsRdyLate, rdy_t);
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

void Engine::flight(std::uint32_t now, const Snapshot& s) {
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
void Engine::recover(std::uint32_t now, const Snapshot& s) {
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

void Engine::releasing(std::uint32_t now, const Snapshot& s) {
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
