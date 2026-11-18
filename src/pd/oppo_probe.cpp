// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Lab-only OPPO (SVID 0x22D9) sink-side probe: the sequencer and its pure helpers (see the header).
#include "k2/pd/oppo_probe.hpp"

#include "k2/core/log.hpp"

namespace k2::pd::oppo {

const char* outcome_name(Outcome o) {
    static constexpr const char* kNames[] = {"not_run", "ack", "nak", "busy", "reply", "accept", "reject", "wait",
                                             "not_supported", "timeout", "tx_fail", "skipped", "already", "lost"};
    return std::size_t(o) < sizeof kNames / sizeof kNames[0] ? kNames[std::size_t(o)] : "?";
}

const char* end_name(End e) {
    static constexpr const char* kNames[] = {"none", "done", "stopped", "hard_reset", "detach", "soft_reset", "stalled"};
    return std::size_t(e) < sizeof kNames / sizeof kNames[0] ? kNames[std::size_t(e)] : "?";
}

const char* refusal_name(Refusal r) {
    static constexpr const char* kNames[] = {"ok", "busy", "inline", "not partner", "no contract", "not at 5 V", "running",
                                             "bad argument"};
    return std::size_t(r) < sizeof kNames / sizeof kNames[0] ? kNames[std::size_t(r)] : "?";
}

Words words_of(std::span<const std::uint32_t> src) {
    Words out;
    for (const std::uint32_t v : src) {
        if (out.n >= out.w.size()) break;
        out.w[out.n++] = v;
    }
    return out;
}

std::uint8_t caps_diff(std::span<const std::uint32_t> before, std::span<const std::uint32_t> after) {
    std::uint8_t mask = 0;
    for (std::size_t i = 0; i < 7; i++) {
        const std::uint32_t a = i < before.size() ? before[i] : 0, b = i < after.size() ? after[i] : 0;
        if (a != b) mask = std::uint8_t(mask | (1u << i));
    }
    return mask;
}

Pick pick_power(const SourceCaps& caps, std::uint16_t mv, std::uint16_t ma) {
    if (!caps.valid || mv == 0) return {};
    const std::size_t n = caps.num_pdos < caps.pdos.size() ? caps.num_pdos : caps.pdos.size();
    for (std::size_t i = 0; i < n; i++) {
        const Pdo& p = caps.pdos[i];
        bool fits = false;
        switch (p.type) {
            case PdoType::Fixed: fits = mv == p.max_mv; break;
            case PdoType::Pps: case PdoType::Avs: case PdoType::Variable: fits = mv >= p.min_mv && mv <= p.max_mv; break;
            default: break;   // a battery supply is not asked by voltage
        }
        if (!fits) continue;
        // The current the slot offers at that voltage: an AVS one is its PDP over the voltage.
        std::uint32_t offer = p.max_ma;
        if (p.type == PdoType::Avs && p.max_mw != 0) offer = p.max_mw * 1000u / mv;
        if (offer == 0 || ma > offer) continue;
        return {std::uint8_t(i + 1), mv, std::uint16_t(ma != 0 ? ma : offer)};
    }
    return {};
}

namespace {
constexpr bool due(std::uint32_t now, std::uint32_t t) { return std::int32_t(now - t) >= 0; }
// The DR_Swap answer window is the sink's (kDrSwapMs reports a Timeout through on_swap); this only bounds a lost report.
constexpr std::uint32_t kSwapBackstopMs = 1000;
}  // namespace

void Probe::start(const Config& c, std::span<const std::uint32_t> caps_before, bool dfp, std::uint32_t now) {
    *this = Probe{};
    cfg_ = c;
    state_ = State::Running;
    started_ = now;
    next_at_ = now;
    r_.before = words_of(caps_before);
    r_.steps = c.steps;
    r_.skip_swap = c.skip_swap;
    r_.dfp = dfp;
}

void Probe::finish(End why) {
    state_ = State::Done;
    waiting_ = false;
    r_.end = why;
    K2LOG("[OPPO] end=%u dfp=%u diff=0x%X\n", unsigned(why), unsigned(r_.dfp), unsigned(r_.diff));
}

void Probe::stop(End why, bool dfp) {
    if (!running()) return;
    if (waiting_) r_.out[std::size_t(step_)] = Outcome::Lost;
    r_.dfp = dfp;
    finish(why);
}

void Probe::advance() {
    step_ = Step(std::uint8_t(step_) + 1);
    swap_tries_ = 0;
    if (step_ == Step::Count) finish(End::Done);
}

// One line per step with the raw words (a VDM answer has up to 7: the rest on a second line; the log slot holds about 5 numbers).
void Probe::record(Outcome o, std::uint32_t now) {
    const std::size_t s = std::size_t(step_);
    r_.out[s] = o;
    waiting_ = false;
    next_at_ = now;
    const Words* w = step_ == Step::Auth ? &r_.vdm[0] : step_ == Step::Power ? &r_.vdm[1] : step_ == Step::Exapdo ? &r_.vdm[2]
                   : step_ == Step::Caps ? &r_.after : nullptr;
    const Words none{};
    const Words& v = w ? *w : none;
    K2LOG("[OPPO] step=%u res=%u n=%u %X %X %X %X\n", unsigned(s), unsigned(o), unsigned(v.n), unsigned(v.w[0]), unsigned(v.w[1]),
          unsigned(v.w[2]), unsigned(v.w[3]));
    if (v.n > 4) K2LOG("[OPPO] step=%u more %X %X %X\n", unsigned(s), unsigned(v.w[4]), unsigned(v.w[5]), unsigned(v.w[6]));
    advance();
}

Action Probe::next(std::uint32_t now, bool idle) {
    if (!running()) return {};
    if (waiting_) {
        if (due(now, deadline_)) record(step_ == Step::Caps && caps_in_ ? Outcome::Reply : Outcome::Timeout, now);
        return {};
    }
    if (now - started_ >= kTotalMs) { finish(End::Stalled); return {}; }
    // Steps that need no message end here, without a gap.
    while (running()) {
        Outcome skip = Outcome::NotRun;
        switch (step_) {
            case Step::Swap: skip = cfg_.skip_swap ? Outcome::Skipped : r_.dfp ? Outcome::Already : Outcome::NotRun; break;
            case Step::Auth: if (!(cfg_.steps & kStepAuth)) skip = Outcome::Skipped; break;
            case Step::Power: if (!(cfg_.steps & kStepPower)) skip = Outcome::Skipped; break;
            case Step::Exapdo: if (!(cfg_.steps & kStepExapdo)) skip = Outcome::Skipped; break;
            case Step::SwapBack: if (!swapped_) skip = Outcome::Skipped; break;
            case Step::Caps: if (!(cfg_.steps & kStepCaps)) skip = Outcome::Skipped; break;
            case Step::Count: break;
        }
        if (skip == Outcome::NotRun) break;
        r_.out[std::size_t(step_)] = skip;
        advance();
    }
    if (!running() || !idle || !due(now, next_at_)) return {};
    Action a;
    deadline_ = now + kVdmReplyMs;
    switch (step_) {
        case Step::Swap: a.want = Want::SwapToDfp; deadline_ = now + kSwapBackstopMs; break;
        case Step::Auth: {
            const std::uint32_t w[5] = {kAuthHeader, cfg_.challenge[0], cfg_.challenge[1], cfg_.challenge[2], cfg_.challenge[3]};
            a.want = Want::Vdm;
            a.vdm = words_of(w);
            break;
        }
        case Step::Power: { const std::uint32_t w[2] = {kPowerHeader, 0}; a.want = Want::Vdm; a.vdm = words_of(w); break; }
        case Step::Exapdo: { const std::uint32_t w[2] = {kExapdoHeader, 0}; a.want = Want::Vdm; a.vdm = words_of(w); break; }
        case Step::SwapBack: a.want = Want::SwapToUfp; deadline_ = now + kSwapBackstopMs; break;
        case Step::Caps: a.want = Want::GetCaps; deadline_ = now + kCapsMs; caps_in_ = false; break;
        case Step::Count: return {};
    }
    waiting_ = true;
    return a;
}

void Probe::on_tx(bool ok, std::uint32_t now) {
    if (awaiting() && !ok) record(Outcome::TxFail, now);
}

void Probe::on_swap(Outcome o, bool dfp, std::uint32_t now) {
    if (!awaiting() || (step_ != Step::Swap && step_ != Step::SwapBack)) return;
    r_.dfp = dfp;
    if (o == Outcome::Wait) {
        if (r_.swap_waits < 255) r_.swap_waits++;
        if (++swap_tries_ < kSwapTries) {   // asked again after tDRSwapWait
            waiting_ = false;
            next_at_ = now + kSwapWaitMs;
            return;
        }
    }
    if (o == Outcome::Accept) swapped_ = step_ == Step::Swap;
    record(o, now);
}

bool Probe::on_vdm(std::span<const std::uint32_t> words, std::uint32_t now) {
    if (!awaiting() || words.empty()) return false;
    std::size_t i = 0;
    switch (step_) {
        case Step::Auth: i = 0; break;
        case Step::Power: i = 1; break;
        case Step::Exapdo: i = 2; break;
        default: return false;
    }
    r_.vdm[i] = words_of(words);
    record(classify(words[0]), now);
    return true;
}

bool Probe::on_not_supported(std::uint32_t now) {
    if (!awaiting() || step_ == Step::Swap || step_ == Step::SwapBack) return false;
    record(Outcome::NotSupported, now);
    return true;
}

void Probe::on_caps(std::span<const std::uint32_t> words) {
    if (!wants_caps()) return;
    r_.after = words_of(words);
    r_.diff = caps_diff(r_.before.view(), r_.after.view());
    caps_in_ = true;
}

void Probe::on_ready(std::uint32_t now) {
    if (wants_caps() && caps_in_) record(Outcome::Reply, now);
}

}  // namespace k2::pd::oppo
