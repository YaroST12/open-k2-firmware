// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_offers.hpp"

namespace k2::pd {

bool OffersRead::presence(bool present, std::uint32_t now) {
    if (present) { absent_ = gone_ = false; return false; }
    if (!absent_) { absent_ = true; absent_since_ = now; }
    if (gone_ || std::int32_t(now - absent_since_) < std::int32_t(kOffersGoneMs)) return false;
    gone_ = true;
    tried_ = false;
    return true;
}

OrWhy OffersRead::gate(const OrCtx& c) const {
    if (c.inline_sink) return OrWhy::Inline;
    if (c.busy) return OrWhy::Busy;
    if (!c.present) return OrWhy::NoCharger;
    return OrWhy::None;
}

bool OffersRead::due(std::uint32_t now) const {
    if (running() || tried_) return false;
    return !have_start_ || std::int32_t(now - started_) >= std::int32_t(kOffersGapMs);
}

void OffersRead::mark_read(std::uint32_t now) {
    if (running()) return;
    tried_ = have_start_ = true;
    started_ = now;
}

bool OffersRead::start(const OrCtx& c, std::uint32_t now) {
    if (running()) return false;
    why_ = gate(c);
    if (why_ != OrWhy::None) { step_ = OrStep::Refused; return false; }
    tried_ = have_start_ = true;
    started_ = now;
    issued_ = owns_ = epr_asked_ = false;
    epr_ = SvOutcome::Silent;
    if (!c.contract) { step_ = OrStep::Attach; return true; }
    step_ = c.epr_capable ? OrStep::Epr : OrStep::Done;   // a contract in hand: caps are known, nothing to take, nothing to give back
    return true;
}

void OffersRead::leave_epr() {
    issued_ = false;
    step_ = owns_ ? OrStep::Release : OrStep::Done;
}

OrAct OffersRead::poll(std::uint32_t now) {
    if (!running()) return OrAct::None;
    if (std::int32_t(now - started_) >= std::int32_t(kOffersTotalMs)) {
        if (step_ == OrStep::Attach) { step_ = OrStep::Done; return OrAct::Abort; }
        if (step_ == OrStep::Epr) { epr_ = SvOutcome::Silent; leave_epr(); }
        else { step_ = OrStep::Done; return OrAct::None; }
    }
    if (issued_) return OrAct::None;
    issued_ = true;
    switch (step_) {
    case OrStep::Attach: return OrAct::Attach;
    case OrStep::Epr: epr_asked_ = true; return OrAct::Epr;
    case OrStep::Release: return OrAct::Release;
    default: return OrAct::None;
    }
}

void OffersRead::attached(bool caps, bool epr_capable, bool owns_port) {
    if (step_ != OrStep::Attach) return;
    owns_ = owns_port;
    if (caps && epr_capable) { issued_ = false; step_ = OrStep::Epr; }
    else leave_epr();
}

void OffersRead::epr_done(SvOutcome o) {
    if (step_ != OrStep::Epr) return;
    epr_ = o;
    leave_epr();
}

void OffersRead::released() {
    if (step_ == OrStep::Release) step_ = OrStep::Done;
}

}  // namespace k2::pd
