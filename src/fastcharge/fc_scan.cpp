// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Auto-detect scanner, see k2/fastcharge/fc_scan.hpp.
#include "k2/fastcharge/fc_scan.hpp"

namespace k2::fc {

namespace {
constexpr std::uint32_t kHoldMs = 800;          // stay in band this long before the step counts
constexpr std::uint32_t kStepTimeoutMs = 20000; // hard limit per step
constexpr std::uint32_t kReturnTimeoutMs = 4000;
constexpr int kBackAt5vMv = 5600;
}  // namespace

const char* name(Outcome o) {
    switch (o) {
    case Outcome::None: return "-";
    case Outcome::Accepted: return "ACCEPTED";
    case Outcome::Rejected: return "rejected";
    case Outcome::Skipped: return "skipped";
    }
    return "?";
}

int scan_target_mv(Proto p) {
    switch (p) {
    case Proto::Qc3:
    case Proto::Qc3Plus: return kScanQc3Mv;
    case Proto::Scp:
    case Proto::Vooc: return kConfirmAboveMv;   // capability / listen only, stays at 5 V
    default: return kScanMaxMv;
    }
}

Fail Scanner::start(bool confirmed, bool allow_inline) {
    if (running()) return Fail::Busy;
    if (env_.ceiling_mv() <= 0) return Fail::NoMonitor;
    if (!confirmed) return Fail::NeedConfirm;
    if (!allow_inline && env_.inline_reason() != link::Reason::None) return Fail::Inline;
    if (eng_.state() == State::Negotiating || eng_.state() == State::Active) return Fail::Busy;
    if (env_.vbus_mv() < kScanMinSourceMv) return Fail::SourceLost;
    done_ = false;
    allow_inline_ = allow_inline;
    fail_ = Fail::None;
    step_ = done_n_ = 0;
    begin_step();
    return Fail::None;
}

void Scanner::begin_step() {
    while (step_ < kScanSteps) {
        const Proto p = kScanOrder[step_];
        const Fail f = eng_.start(p, scan_target_mv(p), true, allow_inline_);
        if (f == Fail::None) {
            phase_ = Phase::Running;
            step_ms_ = env_.now_ms();
            return;
        }
        res_[done_n_++] = {p, Outcome::Skipped, 0, f};
        step_++;
    }
    phase_ = Phase::Off;
    done_ = true;
}

void Scanner::finish_step(Outcome o, int mv, Fail reason) {
    if (o == Outcome::Rejected && reason == Fail::None) reason = eng_.fail();
    res_[done_n_++] = {kScanOrder[step_], o, mv, reason};
    eng_.stop();                  // reset helper: both lines to 0 V, back to 5 V
    phase_ = Phase::Return;
    t_ms_ = env_.now_ms();
}

void Scanner::abort() {
    if (!running()) return;
    eng_.stop();
    phase_ = Phase::Off;
    fail_ = Fail::Aborted;
}

void Scanner::poll() {
    if (phase_ == Phase::Off) return;
    Fail guard = Fail::None;
    if (!allow_inline_ && env_.inline_reason() != link::Reason::None) guard = Fail::Inline;
    else if (env_.ceiling_mv() <= 0) guard = Fail::NoMonitor;
    else if (env_.vbus_mv() < kScanMinSourceMv) guard = Fail::SourceLost;
    else if (env_.vbus_mv() > env_.ceiling_mv()) guard = Fail::Overvoltage;
    else if (eng_.state() == State::Failed && (eng_.fail() == Fail::Overvoltage || eng_.fail() == Fail::NoMonitor)) guard = eng_.fail();
    if (guard != Fail::None) {
        eng_.stop();
        phase_ = Phase::Off;
        fail_ = guard;
        return;
    }
    const std::uint32_t now = env_.now_ms();
    switch (phase_) {
    case Phase::Running:
        if (eng_.state() == State::Failed) {
            finish_step(Outcome::Rejected, 0);
        } else if (eng_.state() == State::Active) {
            phase_ = Phase::Hold;
            t_ms_ = now;
        } else if (now - step_ms_ >= kStepTimeoutMs) {
            finish_step(Outcome::Rejected, 0, Fail::NoResponse);
        }
        break;
    case Phase::Hold:
        if (eng_.state() == State::Failed) {
            finish_step(Outcome::Rejected, 0);
        } else if (eng_.state() != State::Active) {
            phase_ = Phase::Running;
        } else if (now - t_ms_ >= kHoldMs) {
            finish_step(Outcome::Accepted, eng_.reached_mv());
        }
        break;
    case Phase::Return: {
        const bool back = !env_.reset_busy() && env_.vbus_mv() >= 0 && env_.vbus_mv() <= kBackAt5vMv;
        if (back) {
            step_++;
            begin_step();
        } else if (now - t_ms_ >= kReturnTimeoutMs) {
            phase_ = Phase::Off;      // VBUS did not fall back to 5 V: stop asking for more
            fail_ = Fail::SourceLost;
        }
        break;
    }
    case Phase::Off:
        break;
    }
}

}  // namespace k2::fc
