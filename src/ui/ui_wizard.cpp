// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_wizard.hpp"

#include <algorithm>

#include "k2/ui/ui_pdtrig.hpp"

namespace k2 {

namespace {
bool is_adj(pd::PdoType t) { return t == pd::PdoType::Pps || t == pd::PdoType::Avs; }
uint32_t floor_to(uint32_t v, uint32_t step) { return v / step * step; }
uint32_t ceil_to(uint32_t v, uint32_t step) { return (v + step - 1) / step * step; }
}  // namespace

bool Wizard::adjustable(const pd::SourceCaps &caps) const {
    return cursor < caps.num_pdos && cursor < pd::kMaxPdos && is_adj(caps.pdos[cursor].type);
}

bool Wizard::offer_available(const pd::SourceCaps &caps, unsigned idx, uint32_t cap_mv) {
    pd::Choice c;
    c.pos = uint8_t(idx + 1);
    return pd::plan_request(caps, c, cap_mv, c.pos >= 8).err == pd::Err::Ok;
}

uint16_t Wizard::volt_step_mv(const pd::SourceCaps &caps) const {
    return cursor < pd::kMaxPdos && caps.pdos[cursor].type == pd::PdoType::Avs ? kAvsStepMv : kPpsStepMv;
}

void Wizard::volt_range(const pd::SourceCaps &caps, uint32_t cap_mv, uint32_t &lo, uint32_t &hi) const {
    const pd::Pdo &d = caps.pdos[cursor];
    const uint32_t st = volt_step_mv(caps);
    cap_mv = std::max<uint32_t>(cap_mv, pd::kVsafe5vMv);
    lo = ceil_to(d.min_mv, st);
    hi = floor_to(std::min<uint32_t>(d.max_mv, cap_mv), st);
    if (hi < lo) hi = lo;
}

uint16_t Wizard::current_limit(const pd::SourceCaps &caps, uint32_t cap_mv) const {
    pd::Choice c;
    c.pos = uint8_t(cursor + 1);
    c.mv = mv;
    const pd::Plan p = pd::plan_request(caps, c, cap_mv, c.pos >= 8);
    return p.err == pd::Err::Ok ? p.ma : caps.pdos[cursor].max_ma;
}

void Wizard::enter(const pd::SourceCaps &caps, uint8_t held_pos) {
    *this = Wizard{};
    cursor = held_pos >= 1 && held_pos <= caps.num_pdos ? uint8_t(held_pos - 1) : 0;
}

bool Wizard::nudge(const pd::SourceCaps &caps, int dir, bool coarse, uint32_t cap_mv) {
    const Wizard before = *this;
    switch (step) {
        case WizStep::Offer: {
            const int last = caps.num_pdos ? int(std::min<unsigned>(caps.num_pdos, pd::kMaxPdos)) - 1 : 0;
            cursor = uint8_t(std::clamp(int(cursor) + (dir > 0 ? 1 : -1), 0, last));
            break;
        }
        case WizStep::Voltage: {
            uint32_t lo, hi;
            volt_range(caps, cap_mv, lo, hi);
            const int32_t st = coarse ? kCoarseMv : volt_step_mv(caps);
            const int32_t v = std::clamp<int32_t>(int32_t(mv) + (dir > 0 ? st : -st), int32_t(lo), int32_t(hi));
            mv = uint16_t(std::clamp<uint32_t>(floor_to(uint32_t(v), volt_step_mv(caps)), lo, hi));
            break;
        }
        case WizStep::Current: {
            const uint32_t hi = current_limit(caps, cap_mv);
            const uint32_t lo = std::min<uint32_t>(kStepMa, hi);
            const int32_t st = coarse ? kCoarseMa : kStepMa;
            int32_t v = std::clamp<int32_t>(int32_t(ma) + (dir > 0 ? st : -st), int32_t(lo), int32_t(hi));
            if (uint32_t(v) != hi) v = int32_t(floor_to(uint32_t(v), kStepMa));
            ma = uint16_t(std::clamp<uint32_t>(uint32_t(v), lo, hi));
            break;
        }
        default: break;
    }
    return before.cursor != cursor || before.mv != mv || before.ma != ma;
}

Wizard::Ok Wizard::ok(const pd::SourceCaps &caps, uint32_t cap_mv) {
    switch (step) {
        case WizStep::Offer:
            if (!offer_available(caps, cursor, cap_mv)) return Ok::Stay;
            adj = adjustable(caps);
            if (adj) {
                uint32_t lo, hi;
                volt_range(caps, cap_mv, lo, hi);
                mv = uint16_t(hi);
                step = WizStep::Voltage;
            } else {
                mv = 0;
                ma = current_limit(caps, cap_mv);
                step = WizStep::Current;
            }
            return Ok::Next;
        case WizStep::Voltage:
            ma = current_limit(caps, cap_mv);
            step = WizStep::Current;
            return Ok::Next;
        case WizStep::Current:
            step = WizStep::Confirm;
            return Ok::Next;
        case WizStep::Confirm: return Ok::Send;
        case WizStep::Live: break;
    }
    return Ok::Stay;
}

bool Wizard::back() {
    switch (step) {
        case WizStep::Offer: return true;
        case WizStep::Voltage: step = WizStep::Offer; break;
        case WizStep::Current: step = adj ? WizStep::Voltage : WizStep::Offer; break;
        case WizStep::Confirm: step = WizStep::Current; break;
        case WizStep::Live: step = WizStep::Confirm; break;
    }
    return false;
}

int Wizard::total(const pd::SourceCaps &caps) const { return adjustable(caps) ? 4 : 3; }

int Wizard::number(const pd::SourceCaps &caps) const {
    const bool a = adjustable(caps);
    switch (step) {
        case WizStep::Offer: return 1;
        case WizStep::Voltage: return 2;
        case WizStep::Current: return a ? 3 : 2;
        default: return a ? 4 : 3;
    }
}

pd::Choice Wizard::choice(const pd::SourceCaps &caps) const {
    pd::Choice c;
    c.pos = uint8_t(cursor + 1);
    c.mv = adjustable(caps) ? mv : uint16_t(0);
    c.ma = ma;
    return c;
}

void Wizard::title(const pd::SourceCaps &caps, StrBuf<24> &out) const {
    if (step == WizStep::Live) {
        out.put("LIVE");
        return;
    }
    static constexpr const char *kNames[] = {"OFFER", "VOLTAGE", "CURRENT", "CONFIRM"};
    out.put("Step ").put_u32(uint32_t(number(caps))).put_char('/').put_u32(uint32_t(total(caps)));
    out.put(": ").put(kNames[uint8_t(step)]);
}

void Wizard::footer(const pd::SourceCaps &caps, uint32_t cap_mv, bool armed, StrBuf<32> &out) const {
    switch (step) {
        // Facts about the step, never the keys: the panel strip already names what each button does.
        case WizStep::Offer: out.put("Pick an offer"); break;
        case WizStep::Voltage: out.put_u32(volt_step_mv(caps)).put("mV steps"); break;
        case WizStep::Current: out.put("50mA steps"); break;
        case WizStep::Confirm:
            if (!trigger_needs_confirm(caps, choice(caps), cap_mv)) out.put("Ready to send");
            else out.put(armed ? "Above 5V! Send again" : "Above 5V: send twice");
            break;
        case WizStep::Live: out.put("Exit releases to 5V"); break;
    }
}

void put_short(StrBuf<8> &out, uint32_t milli, char unit) {
    const uint32_t tenths = (milli + 50) / 100;
    if (tenths % 10 == 0) out.put_u32(tenths / 10);
    else out.put_fixed(int32_t(tenths * 100), 1);
    out.put_char(unit);
}

bool wiz_summary(const pd::SourceCaps &caps, pd::Choice c, uint32_t cap_mv, WizSummary &out) {
    out = WizSummary{};
    if (c.pos < 1 || c.pos > caps.num_pdos || c.pos > pd::kMaxPdos) return false;
    const pd::Pdo &d = caps.pdos[c.pos - 1];
    const pd::Plan p = pd::plan_request(caps, c, cap_mv, c.pos >= 8);
    out.err = p.err;
    out.kind.put(d.type == pd::PdoType::Fixed ? "FIX" : d.type == pd::PdoType::Pps ? "PPS"
                 : d.type == pd::PdoType::Avs ? "AVS" : d.type == pd::PdoType::Battery ? "BAT" : "VAR");
    out.offer_mw = d.max_mw ? d.max_mw : uint32_t(d.max_mv) * d.max_ma / 1000;
    out.offer_w.put_u32((out.offer_mw + 500) / 1000).put_char('W');
    if (p.err != pd::Err::Ok) return true;
    out.needs_confirm = p.mv > pd::kVsafe5vMv;
    out.req_mw = uint32_t(p.mv) * p.ma / 1000;
    out.volts.put_fixed(int32_t(p.mv), 2).put_char('V');
    out.amps.put_fixed(int32_t(p.ma), 2).put_char('A');
    out.watts.put_fixed(int32_t(out.req_mw), 1).put_char('W');
    out.line.put("Request ").put(out.kind.view()).put_char(' ').put(out.volts.view()).put_char(' ')
        .put(out.amps.view()).put(" = ").put(out.watts.view());
    return true;
}

LiveState live_state(const pd::SinkStatus &s) {
    using R = pd::SinkResult;
    using S = pd::SinkState;
    if (s.result == R::Rejected) return LiveState::Rejected;
    if (s.result == R::HardReset) return LiveState::HardReset;
    const bool failing = s.result == R::WaitExhausted || s.result == R::Timeout || s.result == R::TxFailed ||
                         s.result == R::OverCap || s.result == R::EprFailed || s.result == R::BadChoice ||
                         s.result == R::SoftReset;
    if (failing && !s.held) return LiveState::Failed;
    switch (s.state) {
        case S::EprEntering:
        case S::EprWaitCaps:
        case S::Requesting: return LiveState::Requesting;
        case S::Transition: return LiveState::Accepted;
        case S::Ready: return s.held ? LiveState::PsRdy : LiveState::Released;
        case S::Aborting: return LiveState::Dropping;
        case S::Failed: return LiveState::Failed;
        case S::Idle: break;
    }
    return LiveState::Released;
}

const char *live_name(LiveState s) {
    switch (s) {
        case LiveState::Requesting: return "Requesting";
        case LiveState::Accepted: return "Accepted";
        case LiveState::PsRdy: return "PS_RDY";
        case LiveState::Rejected: return "Rejected";
        case LiveState::HardReset: return "Hard reset";
        case LiveState::Failed: return "Failed";
        case LiveState::Dropping: return "Back to 5V";
        case LiveState::Released: return "Released";
    }
    return "";
}

bool live_good(LiveState s) { return s == LiveState::PsRdy; }
bool live_bad(LiveState s) { return s == LiveState::Rejected || s == LiveState::HardReset || s == LiveState::Failed; }

}  // namespace k2
