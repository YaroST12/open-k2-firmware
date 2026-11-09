// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_survey.hpp"

namespace k2::pd {

namespace {

constexpr SvAct action_of(SvField f) {
    switch (f) {
        case SvField::Caps: return SvAct::Caps;
        case SvField::EprList: return SvAct::Epr;
        case SvField::Cable: return SvAct::Cable;
        default: return SvAct::Wire;
    }
}

// The kReread* step behind a Wire field (0 for the others).
constexpr std::uint8_t wire_bit(SvField f) {
    switch (f) {
        case SvField::Identity: return kRereadIdentity;
        case SvField::Svids: return kRereadSvids;
        case SvField::Modes: return kRereadModes;
        case SvField::CapsExt: return kRereadCapsExt;
        case SvField::MfrInfo: return kRereadMfrInfo;
        default: return 0;
    }
}

constexpr SvAct kPlan[] = {SvAct::Caps, SvAct::Wire, SvAct::Epr, SvAct::Cable};

}  // namespace

void Survey::clear() {
    for (SvEntry& e : e_) e = {};
    started_ = running_ = false;
    act_ = SvAct::None;
}

void Survey::start(const SvCtx& ctx, std::uint32_t now) {
    clear();
    if (!ctx.contract) return;   // nobody to ask: the hint stays
    started_ = true;
    started_at_ = not_before_ = now;
    // One reason for the whole run when it may not send at all; the strongest first.
    SvWhy all = SvWhy::None;
    if (ctx.inline_sink) all = SvWhy::Inline;
    else if (ctx.charger_test) all = SvWhy::ChargerTest;
    else if (ctx.xiaomi_chain) all = SvWhy::XiaomiChain;
    for (unsigned i = 0; i < kSvFields; i++) {
        const SvField f = SvField(i);
        SvWhy why = all;
        if (why == SvWhy::None && ctx.pd20 && (f == SvField::CapsExt || f == SvField::MfrInfo || f == SvField::EprList)) why = SvWhy::Pd20;
        if (why == SvWhy::None && f == SvField::EprList && !ctx.epr_capable) why = SvWhy::NoEpr;
        e_[i] = why == SvWhy::None ? SvEntry{SvState::Pending, SvWhy::None} : SvEntry{SvState::Blocked, why};
    }
    settle_running();
}

unsigned Survey::count(SvState s) const {
    unsigned n = 0;
    for (const SvEntry& e : e_) n += e.state == s;
    return n;
}

unsigned Survey::planned() const {
    unsigned n = 0;
    for (const SvEntry& e : e_) n += e.state != SvState::NotAsked && e.state != SvState::Blocked;
    return n;
}

unsigned Survey::resolved() const { return planned() - count(SvState::Pending); }

std::uint8_t Survey::wire_mask() const {
    std::uint8_t m = 0;
    for (unsigned i = 0; i < kSvFields; i++)
        if (e_[i].state != SvState::Blocked && e_[i].state != SvState::NotAsked) m |= wire_bit(SvField(i));
    return m;
}

bool Survey::open(SvAct a) const {
    for (unsigned i = 0; i < kSvFields; i++)
        if (e_[i].state == SvState::Pending && action_of(SvField(i)) == a) return true;
    return false;
}

SvAct Survey::next_action() const {
    for (SvAct a : kPlan)
        if (open(a)) return a;
    return SvAct::None;
}

void Survey::settle_running() { running_ = started_ && count(SvState::Pending) > 0; }

void Survey::open_to_no_answer(SvAct a) {
    for (unsigned i = 0; i < kSvFields; i++)
        if (e_[i].state == SvState::Pending && (a == SvAct::None || action_of(SvField(i)) == a)) e_[i].state = SvState::NoAnswer;
}

void Survey::set(SvField f, SvOutcome o) {
    SvEntry& e = e_[unsigned(f)];
    if (e.state != SvState::Pending) return;
    e.state = o == SvOutcome::Answered ? SvState::Got : o == SvOutcome::Refused ? SvState::NotSupported : SvState::NoAnswer;
    settle_running();
}

void Survey::finish(std::uint32_t now) {
    if (act_ == SvAct::None) return;
    open_to_no_answer(act_);
    act_ = SvAct::None;
    not_before_ = now + kSurveyPaceMs;
    settle_running();
}

SvAct Survey::poll(std::uint32_t now, bool sink_quiet, bool line_idle) {
    if (!running_) return SvAct::None;
    if (std::int32_t(now - started_at_) >= std::int32_t(kSurveyTotalMs)) {   // the whole run is bounded
        const bool was_in_flight = act_ != SvAct::None;
        open_to_no_answer(SvAct::None);
        act_ = SvAct::None;
        running_ = false;
        return was_in_flight ? SvAct::Abort : SvAct::None;
    }
    if (act_ != SvAct::None) return SvAct::None;                              // one action at a time
    if (!sink_quiet || !line_idle) return SvAct::None;                        // never over a negotiation or a busy line
    if (std::int32_t(now - not_before_) < 0) return SvAct::None;              // paced
    act_ = next_action();
    return act_;
}

}  // namespace k2::pd
