// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// PD Spec Check engine state machine (out-of-line in src/pdspec/ so COLD_SIZE_FLAGS applies).
#include "k2/pdspec/engine.hpp"

namespace k2::pdspec {

StartResult Engine::start(std::uint32_t now_ms, std::uint8_t families, std::uint8_t opts) {
    if (running_) return StartResult::Running;
    const Snapshot s = env_->snapshot();
    if (s.inline_link) return StartResult::Inline;
    if (s.self_powered) return StartResult::SelfPowered;
    if (!s.attached || !env_->caps().valid) return StartResult::NoCaps;
    running_ = true;
    t0_ = now_ms;
    n_tr_ = 0;
    n_pass_ = 0;
    pool_ = env_->record_pool();
    L_ = scan(env_->caps(), env_->cap_mv(), env_->epr_proof());
    walk_ = make_walk(L_.n_fixed);
    full20_ = (opts & kOptFull20) != 0;
    note_ = pdspec::caps_note(env_->caps());
    hr0_ = s.source_hard_resets;
    caps0_ = s.caps_rx;
    for (std::uint8_t k = 0; k < L_.n_fixed; ++k) if (pdo(L_.fixed[k]).max_mv > top_mv_) top_mv_ = pdo(L_.fixed[k]).max_mv;
    for (std::uint8_t k = 0; k < L_.n_prog; ++k) if (L_.prog[k].hi > top_mv_) top_mv_ = L_.prog[k].hi;
    fam_ = Family::Transitions;
    ph_ = Ph::FamStart;
    mask_ = families ? families : kAllFamilies;
    return StartResult::Ok;
}

void Engine::skip_family(Family f, Skip why) {
    for (std::size_t k = 0; k < kChecks; ++k) if (owns(f, Check(k))) skip(Check(k), why);
}

void Engine::next_family() {
    fam_ = Family(static_cast<std::uint8_t>(fam_) + 1);
    item_ = items_ = 0;
    ph_ = Ph::FamStart;
}

void Engine::go(std::uint8_t pos, std::uint16_t mv, After a, std::uint32_t now) {
    tgt_pos_ = pos;
    tgt_mv_ = mv;
    after_ = a;
    phase_t_ = now;
    band_since_ = 0;
    const Snapshot s = env_->snapshot();
    ema_mv_ = s.vbus_mv;
    {
        const std::int32_t cur = s.pos ? level_mv(s.pos, s.mv) : 5000, want = level_mv(pos, mv);
        allowed_mv_ = cur > want ? cur : want;
    }
    if (s.state == pd::SinkState::Ready && s.pos == pos && (mv == 0 || s.mv == mv)) { ph_ = Ph::Settle; return; }
    sent_ = false;
    ph_ = Ph::Goto;
}

// true = queued. A Busy sink is retried for kBusyMs; any other refusal (over the ceiling, bad choice) is a refused level.
bool Engine::send(std::uint32_t now) {
    const Snapshot s = env_->snapshot();
    pd::Choice c;
    c.pos = tgt_pos_;
    c.mv = tgt_mv_;
    const pd::Err e = env_->request(c);
    if (e == pd::Err::Ok) {
        sent_ = true;
        left_ready_ = false;
        send_t0_ = now;
        accept0_ = s.accept_at;
        hard0_ = s.hard_resets;
        rej0_ = s.rejects_rx;
        wait0_ = s.waits_rx;
        return true;
    }
    if (e != pd::Err::Busy || now - phase_t_ > kBusyMs) refused(now);
    return false;
}

int Engine::outcome(const Snapshot& s) const {
    if (s.hard_resets != hard0_) return -2;   // the K2 Hard Reset the source (no Accept / PS_RDY in time)
    if (!s.attached) return -2;
    if (s.state == pd::SinkState::Ready) {
        if (s.result == pd::SinkResult::Ok && s.accept_at != accept0_ && s.pos == tgt_pos_) return 1;
        // The counters, not a state change: a Reject can come and go between two polls.
        if (s.rejects_rx != rej0_ || (s.result == pd::SinkResult::WaitExhausted && s.waits_rx != wait0_)) return -1;
        return 0;
    }
    if (s.state == pd::SinkState::Failed || (left_ready_ && s.state == pd::SinkState::Idle)) return -2;
    return 0;
}

bool Engine::settled(const Snapshot& s) const {
    const std::int32_t t = level_mv(tgt_pos_, tgt_mv_);
    if (programmable(tgt_pos_)) return s.vbus_mv >= pd::programmable_floor_mv(t) && s.vbus_mv <= pd::programmable_ceiling_mv(t);
    return in_band(s.vbus_mv, t);
}

// A setup level the source would not give (Reject / Wait / refused by the sink): the family ends here, back to 5 V.
void Engine::refused(std::uint32_t now) {
    env_->on_note("level refused");
    env_->capture_stop();
    if (after_ == After::Family && ph_ == Ph::Goto) { finish(Stop::Stuck, now); return; }   // even 5 V refused: nothing to restore to
    if (fam_ == Family::Transitions) {   // that level is left out, the walk goes on with the pairs that do not touch it
        for (std::uint8_t k = 0; k < L_.n_fixed; ++k) if (L_.fixed[k] == tgt_pos_) bad_ = static_cast<std::uint8_t>(bad_ | 1u << k);
        if (ph_ == Ph::Measure) {
            cur_.flags |= kTrRefused;
            push_tr(cur_);
            next_item(now);
        } else {
            measure_begin(now);
        }
        return;
    }
    skip_family(fam_, Skip::Refused);
    restore(now);
}

// The contract went (the K2's own Hard Reset after a missing answer, a detach): wait for a 5 V contract, then the next family.
void Engine::lost(const Snapshot& s, std::uint32_t now) {
    env_->on_note("contract lost");
    // The K2 Hard Reset the source after an Accept with no PS_RDY in time (tPSTransition): that is the measurement.
    if ((ph_ == Ph::Measure || ph_ == Ph::Frozen) && s.hard_resets != hard0_ && s.accept_at != accept0_) {
        const bool epr = (cur_.flags & kTrEpr) != 0;
        if (fam_ == Family::Pps) {   // a programmable step: its own PS_RDY check and window
            const bool avs = prog().kind == ProgKind::Avs, large = large_step(std::abs(tgt_mv_ - from_mv_));
            const std::uint32_t ms = avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                                         : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs);
            meas(avs ? Check::AvsPsRdy : Check::PpsPsRdy, {kNever, 0, 0, std::int32_t(ms * 1000), tgt_pos_, static_cast<std::uint8_t>(tgt_mv_ / 100)});
        } else {
            meas(Check::PsRdyTime, {kNever, 0, 0, std::int32_t((epr ? pd::spec::kSrcTransReqEprMs : pd::spec::kSrcTransReqSprMs) * 1000), tgt_pos_, 0});
        }
    }
    if (silenced_) { env_->keepalive_silence(Silence::Off); silenced_ = false; }
    env_->capture_stop();
    skip_family(fam_, Skip::Stopped);
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

void Engine::restore(std::uint32_t now) { go(1, 0, After::Family, now); }

void Engine::begin_family(std::uint32_t now) {
    if (fam_ != Family::Count && !(mask_ >> static_cast<std::uint8_t>(fam_) & 1)) {
        skip_family(fam_, Skip::NotSelected);
        next_family();
        return;
    }
    switch (fam_) {
        case Family::Transitions:
            if (L_.n_fixed < 2) { skip_family(fam_, Skip::OneLevel); next_family(); return; }
            items_ = static_cast<std::uint8_t>(walk_.len - 1);
            env_->on_item(fam_, 0, items_);
            measure_begin(now);
            return;
        case Family::Pps: {
            bool pps = false, avs = false;
            for (std::uint8_t k = 0; k < L_.n_prog; ++k) (L_.prog[k].kind == ProgKind::Avs ? avs : pps) = true;
            for (std::size_t k = 0; k < kChecks; ++k)
                if (owns(fam_, Check(k))) {
                    if (avs_check(Check(k)) && !avs) skip(Check(k), Skip::NoAvs);
                    if (!avs_check(Check(k)) && !pps) skip(Check(k), Skip::NoPps);
                }
            if (L_.n_prog == 0) { next_family(); return; }
            items_ = static_cast<std::uint8_t>(L_.n_prog * kClasses);
            w_ = cls_ = 0;
            env_->on_item(fam_, 0, items_);
            go(prog().pos, static_cast<std::uint16_t>(prog().lo), After::PpsSteps, now);
            return;
        }
        case Family::PpsKeepAlive: {
            // The first PPS window, at 5 V when it reaches it (else its nearest end): the level the source keeps longest.
            const Prog* p = nullptr;
            for (std::uint8_t k = 0; k < L_.n_prog && !p; ++k) if (L_.prog[k].kind == ProgKind::Pps) p = &L_.prog[k];
            if (!p) { skip_family(fam_, Skip::NoPps); next_family(); return; }
            const std::int32_t mv = kPpsKaMv < p->lo ? p->lo : kPpsKaMv > p->hi ? p->hi : kPpsKaMv;
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(p->pos, static_cast<std::uint16_t>(mv), After::PpsKa, now);
            return;
        }
        case Family::EprKeepAlive:
            if (L_.epr == 0) { skip_family(fam_, L_.epr_offered ? Skip::NoEprProof : Skip::NoEpr); next_family(); return; }
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.epr, 0, After::EprKa, now);
            return;
        case Family::HardReset:
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.top_spr ? L_.top_spr : 1, 0, After::HardReset, now);
            return;
        case Family::Detach:
            items_ = 1;
            env_->on_item(fam_, 0, 1);
            go(L_.top_spr ? L_.top_spr : 1, 0, After::Detach, now);
            return;
        case Family::Count:
            finish(Stop::Done, now);
            return;
    }
}

void Engine::after(const Snapshot& s, std::uint32_t now) {
    switch (after_) {
        case After::Measure: measure_begin(now); return;
        // A point reached by a move that is not of the pass's own step class (the window entry, a jump between windows, the landing of a
        // slam) is NOT a reference for the monotonic / track comparison: a source may land such a jump up to vPpsValid off and hold the
        // landing until the next request (UGREEN Nexode Air: 95 mV high), and the first in-window step then looks like a fall of it. The
        // first in-window result (judge_prog) is the first reference; the entry point is only a place to start from.
        case After::PpsSteps:   // at the window's minimum: the first pass starts here
            cur_mv_ = tgt_mv_;
            prev_valid_ = false;
            pass_begin(now);
            return;
        case After::PpsNext:    // a setup move, or a measured one that ended outside its band, has settled
            cur_mv_ = tgt_mv_;
            prev_valid_ = false;
            prog_next(now);
            return;
        case After::PpsKa:
            env_->keepalive_silence(Silence::Pps);
            silenced_ = true;
            ka_t0_ = request_origin(s);   // tPPSTimeout runs from the GoodCRC of the last Request
            ka_extra_ = 0;
            hr0_ = s.source_hard_resets;
            ph_ = Ph::KaWait;
            phase_t_ = now;
            return;
        case After::EprKa:
            if (!s.epr_mode) { skip_family(fam_, Skip::Refused); restore(now); return; }
            env_->keepalive_silence(Silence::Epr);
            silenced_ = true;
            // The source's timer runs from the last EPR_KeepAlive: the sink's stamp, or (none known) a bracket of one keep-alive period.
            ka_t0_ = s.epr_ka_at ? s.epr_ka_at : now;
            ka_extra_ = s.epr_ka_at ? 0 : pd::kEprKeepAliveMs;
            hr0_ = s.source_hard_resets;
            ph_ = Ph::KaWait;
            phase_t_ = now;
            return;
        case After::HardReset:
            tm_.hr_from_mv = static_cast<std::uint16_t>(s.vbus_mv < 0 ? 0 : s.vbus_mv);
            hr_epr_ = s.epr_mode;   // a reset that leaves EPR mode has the longer tSrcRecover
            caps0_ = s.caps_rx;
            track_.start(now);
            if (!env_->hard_reset()) { skip_family(fam_, Skip::Refused); restore(now); return; }
            ph_ = Ph::ResetWatch;
            phase_t_ = now;
            return;
        case After::Detach:
            tm_.det_from_mv = static_cast<std::uint16_t>(s.vbus_mv < 0 ? 0 : s.vbus_mv);
            track_.start(now);
            env_->rd_off(true);
            rd_off_ = true;
            off_at_ = 0;
            ph_ = Ph::DetachGap;
            phase_t_ = now;
            return;
        case After::Family: next_family(); return;
    }
}

// ---- fixed transitions ----

void Engine::measure_begin(std::uint32_t now) {
    // Pairs that touch a refused level are left out (each counted once, as refused).
    while (item_ + 1 < walk_.len && ((bad_ >> walk_.node[item_] & 1) || (bad_ >> walk_.node[item_ + 1] & 1))) {
        ++item_;
        if (item_ < items_) env_->on_item(fam_, item_, items_);   // past the last item there is nothing to announce
    }
    if (item_ + 1 >= walk_.len) { restore(now); return; }
    const std::uint8_t from = node(item_), to = node(item_ + 1);
    const Snapshot s = env_->snapshot();
    if (s.state != pd::SinkState::Ready || s.pos != from) { go(from, 0, After::Measure, now); return; }   // a setup step (after a refusal)
    cur_ = {};
    cur_.from = from;
    cur_.to = to;
    if (from >= 8 || to >= 8) cur_.flags |= kTrEpr;
    from_mv_ = pdo(from).max_mv;
    captured_ = env_->capture_start(kPostFixedUs);
    if (!captured_) {   // the capture is busy (another tool holds it): the timings come from the sink's stamps and live polling
        cur_.flags |= kTrNoCapture;
        for (const Check c : {Check::SrcStart, Check::PsRdyEarly, Check::PsRdyLow, Check::Overshoot}) skip(c, Skip::NoCapture);
    }
    from_us_ = s.now_us;
    tgt_pos_ = to;
    tgt_mv_ = 0;
    allowed_mv_ = from_mv_ > pdo(to).max_mv ? from_mv_ : pdo(to).max_mv;
    phase_t_ = now;
    sent_ = false;
    ph_ = Ph::Measure;
}

// Request -> Accept and Accept -> PS_RDY: the sink's edge stamps when they are real and credible, else the capture's markers. The PS_RDY
// goes to rdy (PsRdyTime for a fixed step, PpsPsRdy / AvsPsRdy for a programmable one) against rdy_lim_us; returned for the records.
Engine::Lat Engine::latencies(const Edges& e, const Snapshot& s, Check rdy_c, std::int32_t rdy_lim_us, std::uint8_t a, std::uint8_t b) {
    Lat l;
    std::uint32_t req = e.req_us, acc = e.accept_us, rdy = e.psrdy_us;
    bool have_acc = e.accept, have_rdy = e.psrdy;
    if ((s.req_us & 1) && (s.accept_us & 1) && s.accept_us - s.req_us >= kSuspectAcceptUs && s.accept_us - s.req_us < 1000000) {
        req = s.req_us; acc = s.accept_us; have_acc = true;
        if (s.ps_rdy_us & 1) { rdy = s.ps_rdy_us; have_rdy = static_cast<std::int32_t>(rdy - acc) > 0; }
    } else if (e.accept && (s.req_us & 1) == 0) {
        cur_.flags |= kTrSuspectStamp;
    }
    std::int32_t acc_gap = 0, rdy_gap = 0;
    if (!have_acc && s.accept_at != accept0_ && s.accept_at - send_t0_ < 1000) {
        // No edge stamp and no capture marker for the Accept (inferred from the trace: it is read behind the GoodCRC of the same service
        // pass, which takes the only stamp). The sink's ms stamps still time it, to a service pass: Request = send(), PS_RDY = its own read.
        // Everything of the capture's own (microsecond) PS_RDY is dropped: it must not be mixed with a millisecond Accept.
        cur_.flags |= kTrSuspectStamp;
        req = send_t0_; acc = s.accept_at;
        have_acc = true; have_rdy = false;
        acc_gap = kMsFallbackAcceptGapUs;
        l.acc_us = (acc - req) * 1000;
        if (s.ps_rdy_at && static_cast<std::int32_t>(s.ps_rdy_at - s.accept_at) >= 0) {   // 0 while inferred or not come yet
            rdy = s.ps_rdy_at; have_rdy = true; rdy_gap = kMsFallbackRdyGapUs;
            l.rdy_us = (rdy - acc) * 1000;
        }
    }
    if (have_acc) {
        l.acc = true;
        if (!acc_gap) l.acc_us = acc - req;
        meas(Check::AcceptTime, {std::int32_t(l.acc_us), acc_gap, 0, std::int32_t(pd::spec::kAcceptSpecLimitMs * 1000), a, b});
    }
    if (have_acc && have_rdy) {
        l.rdy = true;
        if (!rdy_gap) l.rdy_us = rdy - acc;
        meas(rdy_c, {std::int32_t(l.rdy_us), rdy_gap, 0, rdy_lim_us, a, b});
    } else if (have_acc && !acc_gap) {   // (the ms fallback cannot tell "never" from an inferred PS_RDY: no value, not a failure)
        meas(rdy_c, {kNever, 0, 0, rdy_lim_us, a, b});   // the contract was taken without a PS_RDY
    }
    return l;
}

void Engine::measure_done(const Snapshot& s, std::uint32_t now) {
    Edges e = captured_ ? env_->capture_edges(from_us_) : Edges{};
    if (fam_ == Family::Pps) {
        const StepOut o = captured_ && e.accept ? env_->capture_step({e.accept_us, from_mv_, tgt_mv_}) : StepOut{};
        env_->capture_stop();
        judge_prog(o, e, s);
        // Not inside the new band by the end of the window (an unloaded fall can be slow): the next move starts once VBUS is there, and
        // its monotonic check has no level to compare with.
        if (!o.band.seen && !o.settled) { prev_valid_ = false; go(tgt_pos_, tgt_mv_, After::PpsNext, now); return; }
        prog_next(now);
        return;
    }
    const bool epr = (cur_.flags & kTrEpr) != 0;
    const Lat l = latencies(e, s, Check::PsRdyTime, std::int32_t((epr ? pd::spec::kSrcTransReqEprMs : pd::spec::kSrcTransReqSprMs) * 1000),
                            cur_.from, cur_.to);
    if (l.acc) cur_.accept_100us = static_cast<std::uint16_t>(l.acc_us / 100 > 0xFFFE ? 0xFFFE : l.acc_us / 100);
    if (l.rdy) cur_.psrdy_ms = static_cast<std::uint16_t>(l.rdy_us / 1000 > 0xFFFE ? 0xFFFE : l.rdy_us / 1000);
    TransitionOut o{};
    if (captured_ && e.accept) o = env_->capture_transition({e, from_mv_, pdo(cur_.to).max_mv});
    env_->capture_stop();
    judge_transition(o, e, now);
}

void Engine::judge_transition(const TransitionOut& o, const Edges& e, std::uint32_t now) {
    const std::int32_t to_mv = pdo(cur_.to).max_mv;
    const bool up = to_mv > from_mv_, epr = (cur_.flags & kTrEpr) != 0;
    const std::uint8_t a = cur_.from, b = cur_.to;
    cur_.gap_ms = static_cast<std::uint8_t>(o.max_gap_us / 1000 > 255 ? 255 : o.max_gap_us / 1000);
    if (o.start.seen) {
        meas(Check::SrcStart, {std::int32_t(o.start.us), std::int32_t(o.start.gap_us), std::int32_t(pd::spec::kSrcTransitionMinMs * 1000), 0, a, b});
        cur_.start_ms = static_cast<std::uint16_t>(o.start.us / 1000);
    }
    if (o.psrdy_known) {
        cur_.psrdy_mv = static_cast<std::uint16_t>(o.vbus_at_psrdy_mv);
        if (up) meas(Check::PsRdyLow, {o.vbus_at_psrdy_mv, 0, band_lo(to_mv), 0, a, b});
        else meas(Check::PsRdyEarly, {o.vbus_at_psrdy_mv, 0, 0, band_hi(to_mv), a, b});
    }
    if (e.accept && captured_) {
        cur_.peak_mv = static_cast<std::uint16_t>(o.peak_mv < 0 ? 0 : o.peak_mv);
        if (up) meas(Check::Overshoot, {o.peak_mv, 0, -kOpenLimit, band_hi(to_mv) + std::int32_t(pd::spec::kVSrcValidExtraMv), a, b});
        else meas(Check::Overshoot, {o.peak_mv, 0, band_lo(to_mv) - std::int32_t(pd::spec::kVSrcValidExtraMv), kOpenLimit, a, b});
    }
    const Check tc = up ? Check::RiseTime : Check::FallTime;
    const std::int32_t lim = std::int32_t((epr ? pd::spec::kInBandEprMs : pd::spec::kInBandSprMs) * 1000);
    if (o.band.seen) {
        meas(tc, {std::int32_t(o.band.us), std::int32_t(o.band.gap_us), 0, lim, a, b});
        cur_.band_ms = static_cast<std::uint16_t>(o.band.us / 1000);
        if (o.final_valid) {
            meas(Check::Accuracy, {o.final_mv, 0, band_lo(to_mv), band_hi(to_mv), a, b});
            cur_.final_mv = static_cast<std::uint16_t>(o.final_mv);
        }
        push_tr(cur_);
        next_item(now);
        return;
    }
    // Not in the band by the end of the capture (an unloaded fall can take seconds): follow it live, at poll resolution.
    live_t_ = now;
    live_n_ = 0;
    live_first_ = true;
    ph_ = Ph::LiveBand;
    phase_t_ = now;
}

void Engine::live_band(const Snapshot& s, std::uint32_t now) {
    const std::int32_t to_mv = pdo(cur_.to).max_mv;
    const bool up = to_mv > from_mv_, epr = (cur_.flags & kTrEpr) != 0;
    const Check tc = up ? Check::RiseTime : Check::FallTime;
    const std::int32_t lim = std::int32_t((epr ? pd::spec::kInBandEprMs : pd::spec::kInBandSprMs) * 1000);
    const std::uint32_t acc = s.accept_at;
    const bool first = live_first_;
    live_first_ = false;
    if (in_band(s.vbus_mv, to_mv)) {
        // Already in the band at the first look (no capture to show the entry): the entry was somewhere after the Accept, so the
        // value is only an upper end and the whole of it is the bracket (a pass is sure, a fail cannot happen).
        if (live_n_++ == 0) { live_t_ = now; live_unseen_ = first; }
        if (live_n_ >= kLiveBandPolls) {
            const std::uint32_t ms = live_t_ - acc;
            meas(tc, {std::int32_t(ms * 1000), std::int32_t((live_unseen_ ? ms : poll_gap_ + kVbusPeriodMs) * 1000), 0, lim, cur_.from, cur_.to});
            meas(Check::Accuracy, {s.vbus_mv, 0, band_lo(to_mv), band_hi(to_mv), cur_.from, cur_.to});
            cur_.band_ms = static_cast<std::uint16_t>(ms > 0xFFFE ? 0xFFFE : ms);
            cur_.final_mv = static_cast<std::uint16_t>(s.vbus_mv);
            cur_.flags |= kTrLiveBand;
            push_tr(cur_);
            next_item(now);
        }
        return;
    }
    live_n_ = 0;
    if (now - phase_t_ > kLiveBandMaxMs) {
        meas(tc, {kNever, 0, 0, lim, cur_.from, cur_.to});
        cur_.flags |= kTrLiveBand;
        push_tr(cur_);
        env_->on_note("level not reached");
        next_item(now);
    }
}

// ---- programmable sweep (PPS and SPR AVS) ----
// Per window (plan.hpp Prog) the classes 0..3 in order, each a Sweep from the window's minimum up and back to it. A measured move is
// captured and judged (judge_prog); a setup move (between the 20 mV windows) is a plain go() + settle. A PPS pass asks Get_PPS_Status at
// its top (before the first move down) and at its end.

const Prog& Engine::prog() const { return L_.prog[w_]; }

void Engine::pass_begin(std::uint32_t now) {
    const Prog& p = prog();
    sw_ = make_sweep(p, cls_, full20_);
    have_pend_ = false;
    dir_ = 0;
    asked_top_ = false;
    PassRec r;
    r.pos = p.pos;
    r.cls = static_cast<std::uint8_t>(cls_ | (p.kind == ProgKind::Avs ? kPassAvs : 0));
    r.lo_mv = static_cast<std::uint16_t>(p.lo);
    r.hi_mv = static_cast<std::uint16_t>(p.hi);
    r.min_delta_mv = 0x7FFF;
    r.over_mv = -0x7FFF;
    if (std::uint8_t* m = pass_raw()) ::new (m) PassRec(r);
    prog_next(now);
}

void Engine::prog_next(std::uint32_t now) {
    const bool pps = prog().kind == ProgKind::Pps;
    Move m;
    if (have_pend_) {
        m = pend_;
        have_pend_ = false;
    } else if (!sw_.next(m)) {
        if (pps && !asked_end_) { asked_end_ = true; status_begin(now); return; }
        asked_end_ = false;
        pass_end(now);
        return;
    }
    if (!m.measured) { go(prog().pos, static_cast<std::uint16_t>(m.mv), After::PpsNext, now); return; }
    const std::int8_t d = m.mv > cur_mv_ ? 1 : -1;
    if (pps && dir_ == 1 && d == -1 && !asked_top_) {   // the top of the pass: ask before the first move down
        asked_top_ = true;
        pend_ = m;
        have_pend_ = true;
        status_begin(now);
        return;
    }
    dir_ = d;
    const Snapshot s = env_->snapshot();
    from_us_ = s.now_us;
    from_mv_ = cur_mv_;
    tgt_pos_ = prog().pos;
    tgt_mv_ = static_cast<std::uint16_t>(m.mv);
    cur_mv_ = m.mv;
    const std::int32_t step = std::abs(tgt_mv_ - from_mv_);
    captured_ = env_->capture_start(large_step(step) ? kPostLargeUs : kPostStepUs);
    if (!captured_) for (const Check c : prog_checks_meter()) skip(c, Skip::NoCapture);
    allowed_mv_ = from_mv_ > tgt_mv_ ? from_mv_ : tgt_mv_;
    phase_t_ = now;
    sent_ = false;
    ph_ = Ph::Measure;
}

bool Engine::large_step(std::int32_t step_mv) const {
    return prog().kind == ProgKind::Avs ? step_mv > std::int32_t(pd::spec::kAvsSmallStepMv) : step_mv > std::int32_t(pd::spec::kPpsSmallStepMv);
}

void Engine::pass_end(std::uint32_t now) {
    if (pass_raw()) ++n_pass_;
    ++item_;
    if (item_ < items_) env_->on_item(fam_, item_, items_);   // the next pass's number; none after the last
    if (++cls_ < kClasses) { pass_begin(now); return; }   // the pass ended at the window's minimum: the next class starts there
    cls_ = 0;
    if (++w_ < L_.n_prog) { go(prog().pos, static_cast<std::uint16_t>(prog().lo), After::PpsSteps, now); return; }
    restore(now);
}

// The checks the meter feeds (a busy capture skips them).
std::array<Check, 6> Engine::prog_checks_meter() const {
    if (prog().kind == ProgKind::Avs) return {Check::AvsMonotonic, Check::AvsAccuracy, Check::AvsBand, Check::AvsOvershoot, Check::AvsTransient, Check::AvsTrack};
    return {Check::PpsMonotonic, Check::PpsAccuracy, cls_ == 0 ? Check::PpsSettle : Check::PpsBand, Check::PpsOvershoot, Check::PpsTransient, Check::PpsTrack};
}

void Engine::judge_prog(const StepOut& o, const Edges& e, const Snapshot& s) {
    const bool avs = prog().kind == ProgKind::Avs;
    const std::int32_t target = tgt_mv_, from = from_mv_;
    const bool up = target > from;
    const std::int32_t step = std::abs(target - from);
    const bool large = large_step(step);
    const std::int32_t lim_ms = std::int32_t(avs ? (large ? pd::spec::kAvsLargeStepMs : pd::spec::kAvsSmallStepMs)
                                                 : (large ? pd::spec::kPpsLargeStepMs : pd::spec::kPpsSmallStepMs));
    const auto a = static_cast<std::uint8_t>(tgt_pos_ | cls_ << 4 | (up ? 0 : 0x80));
    const auto b = static_cast<std::uint8_t>(target / 100);
    cur_ = {};
    PassRec scratch;
    PassRec* slot = pass_slot();
    PassRec& r = slot ? *slot : scratch;
    // PS_RDY is a received message: its limit carries the wire allowance (pd_spec_timing.hpp kPsRdyWireOverheadMs); the band does not.
    const std::int32_t rdy_ms = lim_ms + std::int32_t(pd::spec::kPsRdyWireOverheadMs);
    const Lat l = latencies(e, s, avs ? Check::AvsPsRdy : Check::PpsPsRdy, rdy_ms * 1000, a, b);
    if (l.acc) {
        const std::uint32_t t = l.rdy ? l.rdy_us / 100 : 0xFFFF;
        if (t > r.psrdy_100us) r.psrdy_100us = static_cast<std::uint16_t>(t > 0xFFFF ? 0xFFFF : t);
        if (!l.rdy || l.rdy_us > std::uint32_t(rdy_ms) * 1000) bump_sat(r.psrdy_fails);
    }
    if (!o.valid) return;
    ++r.steps;
    const auto sat = [](std::uint32_t us) { return static_cast<std::uint16_t>(us / 100 > 0xFFFE ? 0xFFFE : us / 100); };
    // In the band (classes 1..3) / on the final level (the 20 mV class: unverified, the meter's floor).
    if (!avs && cls_ == 0) {
        const std::int32_t settle = o.settled ? std::int32_t(o.settle.us) : kNever;
        meas(Check::PpsSettle, {settle, std::int32_t(o.settle.gap_us), 0, std::int32_t(pd::spec::kPpsSmallStepMs * 1000), a, b});
        const std::uint16_t t = o.settled ? sat(o.settle.us) : 0xFFFF;
        if (t > r.band_100us) r.band_100us = t;
        if (!pdspec::passes(Kind::Max, {settle, std::int32_t(o.settle.gap_us), 0, std::int32_t(pd::spec::kPpsSmallStepMs * 1000)})) bump_sat(r.band_fails);
    } else {
        const Meas m{o.band.seen ? std::int32_t(o.band.us) : kNever, std::int32_t(o.band.gap_us), 0, lim_ms * 1000, a, b};
        meas(avs ? Check::AvsBand : Check::PpsBand, m);
        const std::uint16_t t = o.band.seen ? sat(o.band.us) : 0xFFFF;
        if (t > r.band_100us) r.band_100us = t;
        if (!pdspec::passes(Kind::Max, m)) bump_sat(r.band_fails);
    }
    // Overshoot / undershoot beyond vPpsNew + vPpsValid (vAvsNew + vAvsValid), on the side the step moves to.
    const std::int32_t extra = avs ? pd::spec::kAvsValidExtraMv : pd::spec::kPpsValidExtraMv;
    const std::int32_t over = up ? o.peak_mv - band_hi(target) : band_lo(target) - o.peak_mv;
    if (up) meas(avs ? Check::AvsOvershoot : Check::PpsOvershoot, {o.peak_mv, 0, -kOpenLimit, band_hi(target) + extra, a, b});
    else meas(avs ? Check::AvsOvershoot : Check::PpsOvershoot, {o.peak_mv, 0, band_lo(target) - extra, kOpenLimit, a, b});
    if (over > r.over_mv) r.over_mv = static_cast<std::int16_t>(over > 0x7FFF ? 0x7FFF : over);
    if (over > extra) bump_sat(r.over_fails);
    // How long VBUS left the band again after entering it (tPpsTransient / tAvsTransient); one sample of bracket.
    if (o.band.seen) {
        const std::int32_t tr_ms = std::int32_t(avs ? pd::spec::kAvsTransientMs : pd::spec::kPpsTransientMs);
        const Meas mt{std::int32_t(o.out_us), o.out_us ? std::int32_t(o.band.gap_us) : 0, 0, tr_ms * 1000, a, b};
        meas(avs ? Check::AvsTransient : Check::PpsTransient, mt);
        if (sat(o.out_us) > r.out_100us) r.out_100us = sat(o.out_us);
        if (!pdspec::passes(Kind::Max, mt)) bump_sat(r.out_fails);
    }
    // The level is known when VBUS ended inside the band or sat still on its final level (an offset source); a level still moving at
    // the window's end (an unloaded fall) has none to judge.
    if (!o.band.seen && !o.settled) return;
    // The settled level inside vPpsNew / vAvsNew, and the move the right way (or not at all).
    const std::int32_t tol = target * std::int32_t(avs ? pd::spec::kAvsNewPermille : pd::spec::kPpsNewPermille) / 1000;
    const std::int32_t err = o.final_mv - target;
    meas(avs ? Check::AvsAccuracy : Check::PpsAccuracy, {o.final_mv, 0, target - tol, target + tol, a, b});
    if (std::abs(err) > tol) bump_sat(r.acc_fails);
    if (std::abs(err) > std::abs(r.worst_err_mv)) r.worst_err_mv = static_cast<std::int16_t>(err);
    if (prev_valid_) {
        const std::int32_t delta = up ? o.final_mv - prev_mv_ : prev_mv_ - o.final_mv;
        meas(avs ? Check::AvsMonotonic : Check::PpsMonotonic, {delta, 0, -kMonoAllowMv, 0, a, b});
        if (delta < -kMonoAllowMv) bump_sat(r.mono_fails);
        if (delta < r.min_delta_mv) r.min_delta_mv = static_cast<std::int16_t>(delta);
        // The finding of pd/prog_track.hpp on an upward step (the slams judge the two ends of a window, not a sweep): the first one of
        // a pass only - after a fold every later step sits on the plateau. The value is how far the output fell behind the request.
        if (up && cls_ < 3 && !r.track) {
            const std::int32_t behind = pd::track_behind(from, prev_mv_, target, o.final_mv), limit = pd::track_limit(step);
            meas(avs ? Check::AvsTrack : Check::PpsTrack, {behind, 0, 0, limit, a, b});
            if (behind > limit) r.track = 1;
        }
    }
    prev_mv_ = o.final_mv;
    prev_valid_ = true;
}

// Get_PPS_Status (PPS only, never in the keep-alive family): asked through the sink, the answer judged against the no-load
// expectations (PD CTS SRC3.7#2) and timed against tReceiverResponse.
void Engine::status_begin(std::uint32_t now) {
    st_sent_ = false;
    ph_ = Ph::Status;
    phase_t_ = now;
}

void Engine::status_poll(const Snapshot& s, std::uint32_t now) {
    if (!st_sent_) {
        const pd::Err e = env_->ask_pps_status();
        if (e == pd::Err::Ok) { st_sent_ = true; st_seq0_ = s.pps_seq; return; }
        if (now - phase_t_ > kBusyMs) { env_->on_note("PPS status not asked"); prog_next(now); }   // our own guards held it: no verdict
        return;
    }
    const bool done = s.pps_seq != st_seq0_ && s.pps_ask != pd::PpsAsk::Pending;
    if (!done) {
        if (now - phase_t_ > kStepMaxMs) finish(Stop::Stuck, now);
        return;
    }
    const std::int32_t target = cur_mv_;
    const auto a = static_cast<std::uint8_t>(tgt_pos_ | cls_ << 4 | (asked_end_ ? 0x80 : 0));
    const auto b = static_cast<std::uint8_t>(target / 100);
    const std::int32_t bits = pps_status_faults(s, target);
    meas(Check::PpsStatus, {bits, 0, 0, 0, a, b});
    if (PassRec* p = pass_slot()) p->status = static_cast<std::uint8_t>(p->status | bits);
    if (s.pps_ask != pd::PpsAsk::Timeout && (s.pps_ask_us & 1) && (s.pps_rx_us & 1) && s.pps_rx_us - s.pps_ask_us < 1000000)
        meas(Check::PpsStatusTime, {std::int32_t(s.pps_rx_us - s.pps_ask_us), 0, 0, std::int32_t(pd::spec::kPpsStatusLimitMs * 1000), a, b});
    prog_next(now);
}


std::int32_t Engine::pps_status_faults(const Snapshot& s, std::int32_t target_mv) {
    return pdspec::pps_status_faults(s.pps_ask, s.pps_mv_raw, s.pps_ma_raw, s.pps_flags, target_mv);
}

void Engine::next_item(std::uint32_t now) {
    if (fam_ != Family::Transitions) return;
    ++item_;
    if (item_ >= items_) { restore(now); return; }
    env_->on_item(fam_, item_, items_);
    measure_begin(now);
}

// ---- timers ----

void Engine::track_gap(std::uint32_t now) {
    if (last_poll_ && now - last_poll_ > poll_gap_) poll_gap_ = now - last_poll_;
    last_poll_ = now;
    if (poll_gap_ > tm_.poll_gap_ms) tm_.poll_gap_ms = static_cast<std::uint16_t>(poll_gap_ > 0xFFFF ? 0xFFFF : poll_gap_);
}

// When the source's PPS timer started: the GoodCRC of the last Request it accepted. The sink stamps the Accept in ms; with real edge
// stamps the Request went out (accept_us - req_us) before it (its GoodCRC follows the TX start by under 1 ms), else the Accept stamp.
std::uint32_t Engine::request_origin(const Snapshot& s) const {
    if (!s.accept_at) return 0;
    if ((s.req_us & 1) && (s.accept_us & 1) && s.accept_us - s.req_us < 1000000) return s.accept_at - (s.accept_us - s.req_us) / 1000;
    return s.accept_at;
}

void Engine::ka_wait(const Snapshot& s, std::uint32_t now) {
    const bool pps = fam_ == Family::PpsKeepAlive;
    const Check c = pps ? Check::PpsKeepAlive : Check::EprKeepAlive;
    const std::int32_t lo = std::int32_t(pps ? pd::spec::kPpsTimeoutMinMs : pd::spec::kSourceEprKeepAliveMinMs);
    const std::int32_t hi = std::int32_t(pps ? pd::spec::kPpsTimeoutMaxMs : pd::spec::kSourceEprKeepAliveMaxMs);
    // The origin follows the sink's own stamp should one more keep-alive have gone out as the silence began.
    const std::uint32_t origin = pps ? request_origin(s) : s.epr_ka_at;
    if (s.attached && origin && static_cast<std::int32_t>(origin - ka_t0_) > 0) { ka_t0_ = origin; ka_extra_ = 0; }
    // Without a stamp the last keep-alive went out up to one period before the silence: the value is the upper end, gap the bracket.
    const std::int32_t extra = std::int32_t(ka_extra_);
    const std::int32_t gap = std::int32_t(poll_gap_ + kVbusPeriodMs) + extra;
    if (s.source_hard_resets != hr0_ || (!s.attached && s.vbus_mv < pd::spec::kVSafe0VMaxMv)) {
        hr0_ = s.source_hard_resets;
        const std::int32_t ms = std::int32_t(now - ka_t0_) + extra;
        meas(c, {ms, gap, lo, hi, tgt_pos_, 0});
        if (pps) tm_.pps_ka_ms = std::uint32_t(ms); else tm_.epr_ka_ms = std::uint32_t(ms);
        env_->keepalive_silence(Silence::Off);
        silenced_ = false;
        after_ = After::Family;
        ph_ = Ph::Recover;
        phase_t_ = now;
        asked_ = false;
        return;
    }
    if (now - ka_t0_ > (pps ? kPpsKaWaitMs : kEprKaWaitMs)) {
        meas(c, {kNever, 0, lo, hi, tgt_pos_, 0});   // no reset at all: the source never timed out
        env_->keepalive_silence(Silence::Off);
        silenced_ = false;
        env_->on_note("no keep-alive timeout");
        restore(now);
    }
}

void Engine::reset_watch(const Snapshot& s, std::uint32_t now) {
    track_.feed(now, s.vbus_mv, s.caps_rx != caps0_);
    const ResetTimes& t = track_.times();
    const bool done = t.caps_seen || now - phase_t_ > kResetWatchMs;
    if (!done) return;
    const std::int32_t g = std::int32_t(t.gap_ms + kVbusPeriodMs);
    const std::int32_t el = std::int32_t(now - phase_t_);
    meas(Check::HrSafe0V, {t.off_seen ? std::int32_t(t.off_ms) : el, g, 0, std::int32_t(pd::spec::kPsHardResetMaxMs + pd::spec::kSafe0VMs), 0, 0});
    if (t.off_seen) {
        tm_.hr_off_ms = static_cast<std::uint16_t>(t.off_ms);
        meas(Check::HrRecover, {t.up_seen ? std::int32_t(t.dark_ms) : el, g,
                                std::int32_t(hr_epr_ ? pd::spec::kSrcRecoverEprMinMs : pd::spec::kSrcRecoverMinMs),
                                std::int32_t(hr_epr_ ? pd::spec::kSrcRecoverEprMaxMs : pd::spec::kSrcRecoverMaxMs), 0, 0});
    }
    if (t.up_seen) {
        tm_.hr_dark_ms = static_cast<std::uint16_t>(t.dark_ms);
        meas(Check::HrTurnOn, {t.on_seen ? std::int32_t(t.turn_on_ms) : el, g, 0, std::int32_t(pd::spec::kSrcTurnOnMs), 0, 0});
    }
    if (t.on_seen) {
        tm_.hr_on_ms = static_cast<std::uint16_t>(t.turn_on_ms);
        meas(Check::HrCaps, {t.caps_seen ? std::int32_t(t.caps_ms) : el, g, 0, std::int32_t(pd::spec::kFirstSourceCapMs), 0, 0});
    }
    if (t.caps_seen) tm_.hr_caps_ms = static_cast<std::uint16_t>(t.caps_ms);
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

void Engine::detach_gap(const Snapshot& s, std::uint32_t now) {
    track_.feed(now, s.vbus_mv, false);
    const ResetTimes& t = track_.times();
    if (t.off_seen && off_at_ == 0) off_at_ = now ? now : 1;
    const bool done = (t.off_seen && now - off_at_ >= kDetachHoldMs) || now - phase_t_ >= kDetachMaxMs;
    if (!done) return;
    meas(Check::DetachSafe0V, {t.off_seen ? std::int32_t(t.off_ms) : std::int32_t(now - phase_t_), std::int32_t(t.gap_ms + kVbusPeriodMs), 0,
                               std::int32_t(pd::spec::kVbusOffMs + pd::spec::kSrcDisconnectMaxMs), 0, 0});
    if (t.off_seen) tm_.det_off_ms = static_cast<std::uint16_t>(t.off_ms);
    env_->rd_off(false);
    rd_off_ = false;
    rd_back_ = now;
    track_.start(now);   // the way back: VBUS up to vSafe5V, then the first capabilities
    caps0_ = s.caps_rx;
    after_ = After::Family;
    ph_ = Ph::Recover;
    phase_t_ = now;
    asked_ = false;
}

// Rd back -> VBUS at vSafe5V within tCCDebounce + tBulkDischarge + tVBUSON, and the first Source_Capabilities within tFirstSourceCap of
// it (Type-C R2.5 Tables 4-32 / 4-34, PD R3.3 Table 7.10). At poll resolution; what never came is kNever.
void Engine::reattach_judge(std::uint32_t now) {
    const ResetTimes& t = track_.times();
    // VBUS never left after the detach (the source kept it up): there is no way back to time (detach_safe0v has the verdict).
    if (!t.off_seen) { skip(Check::Reattach, Skip::NoData); skip(Check::ReattachCaps, Skip::NoData); return; }
    const std::int32_t g = std::int32_t(t.gap_ms + kVbusPeriodMs);
    const std::uint32_t bulk = tm_.det_from_mv > pd::spec::kBulkDischargeHvAboveMv ? pd::spec::kBulkDischargeHvMs : pd::spec::kBulkDischargeMs;
    const std::int32_t lim = std::int32_t(pd::spec::kCcDebounceMaxMs + bulk + pd::spec::kVbusOnMs);
    const std::int32_t up = t.on_seen ? std::int32_t(t.off_ms + t.dark_ms + t.turn_on_ms) : kNever;
    meas(Check::Reattach, {up, t.on_seen ? g : 0, 0, lim, 0, 0});
    if (t.on_seen) meas(Check::ReattachCaps, {t.caps_seen ? std::int32_t(t.caps_ms) : kNever, t.caps_seen ? g : 0, 0, std::int32_t(pd::spec::kFirstSourceCapMs), 0, 0});
    (void)now;
}

void Engine::recover(const Snapshot& s, std::uint32_t now) {
    const bool back = s.attached && s.state == pd::SinkState::Ready && s.pos == 1 && s.vbus_mv >= pd::spec::kVSafe5VMinMv;
    if (fam_ == Family::Detach && rd_back_) track_.feed(now, s.vbus_mv, s.caps_rx != caps0_);
    if (back) {
        if (fam_ == Family::Detach && rd_back_) {
            reattach_judge(now);
            tm_.det_back_ms = static_cast<std::uint16_t>(now - rd_back_ > 0xFFFE ? 0xFFFE : now - rd_back_);
            rd_back_ = 0;
        }
        hr0_ = s.source_hard_resets;
        next_family();
        return;
    }
    // Back with a contract that is not 5 V (the sink re-asked an older level), or with capabilities and no contract after a while: ask
    // for 5 V (once it is queued).
    const bool other = s.attached && s.state == pd::SinkState::Ready && s.pos != 1;
    const bool idle = s.attached && s.state == pd::SinkState::Idle && now - phase_t_ > kRecoverAskMs;
    if (!asked_ && (other || idle)) {
        pd::Choice c;
        c.pos = 1;
        asked_ = env_->request(c) == pd::Err::Ok;
    }
    if (now - phase_t_ > kRecoverMs) {
        if (fam_ == Family::Detach && rd_back_) reattach_judge(now);
        finish(Stop::NoRecovery, now);
    }
}

// ---- the end ----

void Engine::finish(Stop why, std::uint32_t now) {
    if (finishing_) return;
    finishing_ = true;
    stop_ = why;
    if (silenced_) { env_->keepalive_silence(Silence::Off); silenced_ = false; }
    if (rd_off_) { env_->rd_off(false); rd_off_ = false; }
    env_->capture_stop();
    // Checks with no measurement and no reason yet: a family left out of the run was not selected, the family in flight was stopped,
    // the later ones were not reached, an earlier one that ran without a single value for it (no down-step got its band, say) had no
    // data.
    for (std::size_t f = 0; f < kFamilies; ++f) {
        const auto cur = static_cast<std::size_t>(fam_);
        const bool picked = (mask_ >> f & 1) != 0;
        skip_family(Family(f), !picked ? Skip::NotSelected : f < cur ? Skip::NoData : f == cur ? Skip::Stopped : Skip::NotReached);
    }
    meas(Check::SourceReset, {std::int32_t(src_resets_), 0, 0, 0, 0, 0});
    env_->release();
    ph_ = Ph::Finishing;
    phase_t_ = now;
}

bool Engine::guards(const Snapshot& s, std::uint32_t now) {
    if (cancel_) { finish(Stop::Cancelled, now); return false; }
    if (now - t0_ > kMaxRunMs) { finish(Stop::Timeout, now); return false; }
    if (s.inline_link) { finish(Stop::Inline, now); return false; }
    // Over the monitor ceiling + 5 % at once; over the step's own highest level + 10 % + 1 V for kOvPolls polls in a row (an overshoot
    // spike is a measurement, not a reason to stop).
    if (s.vbus_mv > std::int32_t(pd::overcap_abort_mv(env_->cap_mv()))) { finish(Stop::Overvoltage, now); return false; }
    const std::int32_t allowed = (ph_ == Ph::Settle || ph_ == Ph::Goto || ph_ == Ph::Measure || ph_ == Ph::Frozen || ph_ == Ph::LiveBand)
                                     ? allowed_mv_ : top_mv_;
    if (s.vbus_mv > allowed + allowed / 10 + 1000) {
        if (++ov_polls_ >= kOvPolls) { finish(Stop::Overvoltage, now); return false; }
    } else {
        ov_polls_ = 0;
    }
    if (std::abs(s.ibus_ma) > kLoadAbortMa) {
        if (++load_polls_ >= kLoadPolls) { finish(Stop::Load, now); return false; }
    } else {
        load_polls_ = 0;
    }
    // A Hard Reset from the source outside a keep-alive wait: none was due. Counted, the family ends, wait for 5 V.
    if (s.source_hard_resets != hr0_ && ph_ != Ph::KaWait) {
        hr0_ = s.source_hard_resets;
        ++src_resets_;
        if (ph_ != Ph::Recover) { lost(s, now); return false; }
    }
    return true;
}

void Engine::poll(std::uint32_t now) {
    if (!running_) return;
    const Snapshot s = env_->snapshot();
    if (ph_ == Ph::Finishing) {
        // Done at 5 V: a PDO 1 contract, or a source with no contract of ours (it holds vSafe5V). A dark line (a stop inside a reset
        // or the detach gap) must stay dark kDarkFinalMs: a source coming back starts at vSafe5V and is then confirmed as above.
        const bool low = s.vbus_mv <= kRestoreMaxMv;
        const bool at5 = s.attached && low && s.vbus_mv >= std::int32_t(pd::spec::kVSafe0VMaxMv) &&
                         ((s.state == pd::SinkState::Ready && s.pos == 1) || s.state == pd::SinkState::Idle);
        if (!s.attached && s.vbus_mv < pd::spec::kVSafe0VMaxMv) {
            if (!dark_since_) dark_since_ = now ? now : 1;
        } else {
            dark_since_ = 0;
        }
        const bool gone = dark_since_ && now - dark_since_ >= kDarkFinalMs;
        if (at5 || gone || now - phase_t_ > kReleaseMs) {
            if (!at5 && !gone) release_unconfirmed_ = true;
            running_ = false;
            end_ms_ = now;
            ph_ = Ph::Done;
            env_->on_finish(stop_);
        }
        return;
    }
    if (!guards(s, now)) return;
    track_gap(now);   // the run's longest poll gap: every live-timed value carries it as its uncertainty
    if (s.state != pd::SinkState::Ready) left_ready_ = true;
    const bool expect_dark = ph_ == Ph::KaWait || ph_ == Ph::ResetWatch || ph_ == Ph::DetachGap || ph_ == Ph::Recover;
    if (!s.attached && !expect_dark) { lost(s, now); return; }
    switch (ph_) {
        case Ph::FamStart: begin_family(now); return;
        case Ph::Goto: {
            if (!sent_) { (void)send(now); return; }
            const int o = outcome(s);
            if (o == 0) { if (now - send_t0_ > kStepMaxMs) finish(Stop::Stuck, now); return; }
            if (o == -1) { refused(now); return; }
            if (o == -2) { lost(s, now); return; }
            ph_ = Ph::Settle;
            phase_t_ = now;
            band_since_ = 0;
            ema_mv_ = s.vbus_mv;
            return;
        }
        case Ph::Settle:
            ema_mv_ += (s.vbus_mv - ema_mv_) / 4;
            if (settled(s)) {
                if (!band_since_) band_since_ = now ? now : 1;
                if (now - band_since_ >= kSettleQuietMs) after(s, now);
                return;
            }
            band_since_ = 0;
            if (now - phase_t_ > kSettleMaxMs) { env_->on_note("level not settled"); after(s, now); }
            return;
        case Ph::Measure: {
            if (!sent_) { (void)send(now); return; }
            const int o = outcome(s);
            if (o == 0) { if (now - send_t0_ > kStepMaxMs) finish(Stop::Stuck, now); return; }
            if (o == -1) { refused(now); return; }
            if (o == -2) { lost(s, now); return; }
            ph_ = Ph::Frozen;
            phase_t_ = now;
            if (!captured_) measure_done(s, now);
            return;
        }
        case Ph::Frozen:
            if (env_->capture_frozen() || now - phase_t_ > kFrozenMaxMs) measure_done(s, now);
            return;
        case Ph::LiveBand: live_band(s, now); return;
        case Ph::Status: status_poll(s, now); return;
        case Ph::KaWait: ka_wait(s, now); return;
        case Ph::ResetWatch: reset_watch(s, now); return;
        case Ph::DetachGap: detach_gap(s, now); return;
        case Ph::Recover: recover(s, now); return;
        case Ph::Restore:
        case Ph::Finishing:
        case Ph::Done: return;
    }
}

}  // namespace k2::pdspec
