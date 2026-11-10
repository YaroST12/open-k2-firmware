// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Passive-mode policy, pure (host-tested in tests/test_pd_passive.cpp): when the partner is gone,
// and when the chip may ACK (Auto-CRC).
#pragma once
#include <cstdint>

namespace k2::pd {

inline constexpr std::int32_t kDetachVbusMv = 3000;    // VBUS below this ...
inline constexpr std::uint32_t kDetachVbusMs = 1000;   // ... for this long is a detach
inline constexpr std::uint32_t kDetachCcMs = 500;      // no Rp on the active CC for this long is a detach

inline constexpr std::uint32_t kDetachSinkMs = 500;   // inline: no sink Rd on the active CC for this long
inline constexpr std::int32_t kSinkBandCenterMv = 1500, kSinkBandMv = 700;  // inline sink-present band
inline constexpr std::uint32_t kRdHoldMs = 1500;       // Rd off after a detach: the charger sees an open line

// A real sink's Rd on the active line, measured with none of our own Rd on it: the voltage sits near
// 1.5 V (Rp 80 uA into 5.1 k = 0.4 V .. 3 A Rp 330 uA = 1.7 V), open Rp rises to the rail. Unknown (-1)
// counts as present: a missing measurement is never a reason to detach.
constexpr bool sink_present(std::int32_t cc_mv) {
    if (cc_mv < 0) return true;
    const std::int32_t d = cc_mv - kSinkBandCenterMv;
    return (d < 0 ? -d : d) <= kSinkBandMv;
}

enum class DetachReason : std::uint8_t { None, Vbus, Cc, Sink };
inline const char* detach_reason_name(DetachReason r) {
    switch (r) {
        case DetachReason::Vbus: return "VBUS low";
        case DetachReason::Cc: return "no Rp on the active line";
        case DetachReason::Sink: return "the sink left (inline)";
        default: return "none";
    }
}

// Rd stays off for kRdHoldMs after a detach.
class RdHold {
public:
    void start(std::uint32_t now) { until_ = now + kRdHoldMs; on_ = true; }
    void cancel() { on_ = false; }
    // True once, when the hold has run out: the caller puts Rd back then, whatever the scan found.
    bool take_expiry(std::uint32_t now) {
        if (!on_ || held(now)) return false;
        on_ = false;
        return true;
    }
    bool held(std::uint32_t now) const {
        const std::int32_t left = std::int32_t(until_ - now);
        return on_ && left > 0 && left <= std::int32_t(kRdHoldMs);   // a clock that went backwards is not a hold
    }
private:
    bool on_ = false;
    std::uint32_t until_ = 0;
};

// Passive detach: VBUS below 3.0 V for 1 s, or the CC line gone for 500 ms.
class DetachTracker {
public:
    void reset() { *this = DetachTracker{}; }
    // `vbus_mv` < 0 means unknown (ignored). Returns true once when the partner is gone; call
    // reset() when attached again.
    // `sink_ok` is false only while inline and no sink Rd is on the line.
    bool update(std::uint32_t now, std::int32_t vbus_mv, bool cc_present, bool sink_ok = true) {
        if (vbus_mv >= kDetachVbusMv) vbus_seen_ = true;
        // Only a VBUS that was there and went away counts: a reading that never reached 3 V may belong
        // to the other port (or an unpowered probe) while the partner on this one is alive.
        if (vbus_seen_ && vbus_mv >= 0 && vbus_mv < kDetachVbusMv) {
            if (!vbus_low_) { vbus_low_ = true; vbus_since_ = now; }
        } else if (vbus_mv >= 0) {
            vbus_low_ = false;
        }
        if (!cc_present) {
            if (!cc_gone_) { cc_gone_ = true; cc_since_ = now; }
        } else {
            cc_gone_ = false;
        }
        if (!sink_ok && sink_seen_) {
            if (!sink_gone_) { sink_gone_ = true; sink_since_ = now; }
        } else {
            sink_gone_ = false;
        }
        if (vbus_low_ && now - vbus_since_ >= kDetachVbusMs) { reason_ = DetachReason::Vbus; return true; }
        if (cc_gone_ && now - cc_since_ >= kDetachCcMs) { reason_ = DetachReason::Cc; return true; }
        if (sink_gone_ && now - sink_since_ >= kDetachSinkMs) { reason_ = DetachReason::Sink; return true; }
        return false;
    }
    DetachReason reason() const { return reason_; }
    bool vbus_seen() const { return vbus_seen_; }
    // A real sink reading (known CC level inside the band) was taken while inline. The sink-gone timer
    // only runs after this: a line that never showed a sink (Rd not visible through the K2) is not a
    // sink that left.
    void sink_seen() { sink_seen_ = true; }
    bool sink_was_seen() const { return sink_seen_; }
    void inline_begin() { sink_seen_ = false; sink_gone_ = false; }
private:
    bool vbus_low_ = false, cc_gone_ = false, vbus_seen_ = false, sink_gone_ = false, sink_seen_ = false;
    std::uint32_t vbus_since_ = 0, cc_since_ = 0, sink_since_ = 0;
    DetachReason reason_ = DetachReason::None;
};

// Trigger gate, one for the wizard and the remote. A live sink on the far side already has a contract
// with the source: a Request from the K2 would renegotiate (or hard-reset) its supply, so no Request is
// transmitted while any of these holds.
enum class TriggerRefusal : std::uint8_t { None, Inline, SinkRd, ForeignRequest };
struct TriggerFacts {
    bool inline_mode;        // pd::Link decided a real sink is attached
    bool sink_rd_seen;       // a sink Rd was read on the far side of the K2
    bool foreign_request;    // a Request we did not send was sniffed in this session
};
constexpr TriggerRefusal can_trigger(const TriggerFacts& f) {
    return f.inline_mode ? TriggerRefusal::Inline : f.sink_rd_seen ? TriggerRefusal::SinkRd
         : f.foreign_request ? TriggerRefusal::ForeignRequest : TriggerRefusal::None;
}
constexpr const char* trigger_refusal_name(TriggerRefusal r) {
    switch (r) {
        case TriggerRefusal::Inline: return "link inline";
        case TriggerRefusal::SinkRd: return "sink Rd on the other port";
        case TriggerRefusal::ForeignRequest: return "request from another sink";
        case TriggerRefusal::None: break;
    }
    return "none";
}

// Lab-only over-load gate for the remote's `pd request ... --over-load` (never the UI, never the
// charger test): let the K2 send its Request while a sink sits behind it, but only while that sink
// stays a passive load - it showed its Rd and sent no PD message this attach. The facts come from
// the one existing foreign-evidence tracking (pd::Link's inline latch, the sink Rd read on CC,
// fusb302's far-sink records); the gate adds no second tracker.
struct OverLoadFacts {
    bool far_sink;        // a sink sits behind the K2 this attach (inline latch, sink Rd on CC, or a foreign Request)
    bool far_pd_message;  // that sink sent a PD message this attach (a Request we did not send, any of its control
                          // messages - GoodCRC, Get_Source_Cap -, its Sink_Capabilities / VDMs)
};
// Allowed only with a far sink attached and silent on PD this attach; refused with no far sink
// and refused after any far PD message.
constexpr bool over_load_allowed(const OverLoadFacts& f) { return f.far_sink && !f.far_pd_message; }

// The lab-only inline Hard Reset (`k2_remote.py pd reset --inline --confirm`, ROADMAP 2026-11-10, connection
// matrix P4): a Hard Reset while a live sink is attached cuts that sink's supply, so it is refused inline by
// default; the lab override sends exactly ONE in that state, to watch the far sink renegotiate (P4: the phone
// then negotiated PPS itself). Same lab gating shape as --over-load: refused while a charger test runs (a run
// owns the link), and it needs BOTH flags - `--inline` alone is a NeedConfirm, `--confirm` alone is no override.
// Never reachable from the UI, the shell or the charger test: only the remote `pd reset` subparser carries the
// flags (payload flag byte on Cmd::PdReset, kPdFlagResetInline).
enum class LabResetVerdict : std::uint8_t { Allowed, RefusedBusy, NeedConfirm, RefusedInline };
struct LabResetFacts {
    bool charger_test_busy;   // a charger-test run owns the link: lab ops are refused during it
    bool inline_mode;         // pd::Link latched a real sink behind the K2
    bool trigger_refusal;     // can_trigger() != None (a sink Rd or a foreign Request on the wire)
};
constexpr LabResetVerdict lab_reset_verdict(const LabResetFacts& f, bool flag_inline, bool flag_confirm) {
    if (f.charger_test_busy) return LabResetVerdict::RefusedBusy;                 // the run owns the link, flags or not
    if (flag_inline && !flag_confirm) return LabResetVerdict::NeedConfirm;        // the lab op needs both flags
    if ((f.inline_mode || f.trigger_refusal) && !(flag_inline && flag_confirm)) return LabResetVerdict::RefusedInline;  // as today
    return LabResetVerdict::Allowed;
}

// The phase-1 lab hand-back (`k2_remote.py pd handback --confirm`, ROADMAP 2026-11-10 "An inline latch leaves the
// far sink on Type-C current", phase 1 lab only): hand the contract back with ONE Hard Reset - allowed only when the
// K2 owns an explicit contract (self-hold, a request) while a sink is latched inline, the far sink sent no Request
// of its own this attach, and no hand-back was done this attach yet. Same lab gating shape as --over-load /
// `pd reset --inline`: refused while a charger test runs (the run owns the link) and it needs --confirm. Never
// reachable from the UI, the shell or the charger test: only the remote `pd handback` subparser carries the flag.
enum class HandbackVerdict : std::uint8_t {
    Allowed, RefusedBusy, NeedConfirm, RefusedNotInline, RefusedNoContract, RefusedFarRequest, RefusedAgain
};
struct HandbackFacts {
    bool charger_test_busy;   // a charger-test run owns the link: lab ops are refused during it
    bool inline_mode;         // pd::Link latched a real sink behind the K2
    bool contract_owned;      // the K2 holds an explicit contract (self-hold or a request: sink Ready / held)
    bool far_request_seen;    // a foreign Request this attach (fusb302::far_sink_seen())
    bool handback_done;       // a hand-back was already done this attach
};
constexpr HandbackVerdict handback_verdict(const HandbackFacts& f, bool flag_confirm) {
    if (f.charger_test_busy) return HandbackVerdict::RefusedBusy;
    if (!flag_confirm) return HandbackVerdict::NeedConfirm;
    if (!f.inline_mode) return HandbackVerdict::RefusedNotInline;
    if (!f.contract_owned) return HandbackVerdict::RefusedNoContract;   // nothing to hand back
    if (f.far_request_seen) return HandbackVerdict::RefusedFarRequest;   // the far end already has its own contract
    if (f.handback_done) return HandbackVerdict::RefusedAgain;           // one hand-back per attach
    return HandbackVerdict::Allowed;
}
constexpr const char* handback_verdict_name(HandbackVerdict v) {
    switch (v) {
    case HandbackVerdict::Allowed: return "allowed";
    case HandbackVerdict::RefusedBusy: return "charger test running";
    case HandbackVerdict::NeedConfirm: return "confirm needed";
    case HandbackVerdict::RefusedNotInline: return "not inline";
    case HandbackVerdict::RefusedNoContract: return "no contract owned";
    case HandbackVerdict::RefusedFarRequest: return "far sink Request seen this attach";
    case HandbackVerdict::RefusedAgain: return "already handed back this attach";
    }
    return "?";
}

// Type-C current advertised by the source's Rp, from the FUSB302 BC_LVL (STATUS0 bits 1:0):
// 1 = default USB (500 mA), 2 = 1.5 A, 3 = 3 A. With a Rd on the line the level reads from the
// Rp/Rd divider, so callers keep the highest level seen since attach.
constexpr std::uint32_t typec_ma(std::uint8_t bc_lvl) {
    return bc_lvl == 1 ? 500 : bc_lvl == 2 ? 1500 : bc_lvl == 3 ? 3000 : 0;
}
// A source alone that sent no PD traffic at all this long after attach is a plain Type-C source.
inline constexpr std::uint32_t kTypeCOnlyMs = 3000;
// Advertised current of a Type-C-only source, 0 while it may still be a PD source (or not a source).
// Never while inline (a sink holds a contract, the caps went by before the K2 listened) and never once any
// PD message was seen this session: that source speaks PD, its caps were just missed.
inline std::uint32_t typec_only_ma(std::uint32_t attach_age_ms, bool caps_fresh, std::uint8_t bc_max,
                                   bool inline_mode = false, bool pd_traffic = false) {
    if (caps_fresh || inline_mode || pd_traffic || attach_age_ms < kTypeCOnlyMs) return 0;
    return typec_ma(bc_max);
}

// Auto-CRC is on only while an active operation runs (trigger, Identify, Get_Source_Cap,
// e-marker scan) and for a short linger after it, so the reply to the last message is still ACKed.
inline constexpr std::uint32_t kCrcLingerMs = 300;

class AutoCrcGate {
public:
    // `busy`: an active operation needs the chip to ACK. Returns the wanted Auto-CRC state.
    bool update(std::uint32_t now, bool busy) {
        if (busy) { on_ = true; busy_at_ = now; }
        else if (on_ && now - busy_at_ >= kCrcLingerMs) on_ = false;
        return on_;
    }
    bool on() const { return on_; }
    void reset() { on_ = false; }
private:
    bool on_ = false;
    std::uint32_t busy_at_ = 0;
};

// What keeps the chip fully active (Rd, Auto-CRC on, answers every message): the whole trigger
// contract, not only the message in flight. The driver returns to passive only when none is set and
// the linger has run out, or on detach.
// Two questions, two answers (k2/rd-release, hardware 2026-11-06: the armed e-marker spoof kept Rd on CC2 for good):
//   crc_demand: the chip must ACK (GoodCRC) what the partner sends. Everything below, the armed spoof included.
//   rd_demand:  the K2 acts as a sink and must look like one (Rd on the active line). The same set WITHOUT the spoof: the
//               spoof answers SOP' cable traffic, which is not a sink and needs no Rd.
struct CrcDemand {
    bool sink_active = false;   // the sink engine is negotiating, or holds a contract that is still in use (see ContractLinger)
    bool sink_held = false;     // a trigger contract is held (keep-alives, Ready state)
    bool ident = false;         // Identify is running
    bool getcap = false;        // a Get_Source_Cap is pending (or a VCONN swap runs)
    bool prep = false;          // a trigger is preparing (re-announce)
    bool run_start = false;     // a run start presents Rd (charger test START): the source's caps need a GoodCRC
    bool spoof = false;         // the e-marker spoof is armed: SOP' needs our GoodCRC, nothing else
    // A self-powered K2 (a key-mode or inherited Rd pin, see pd::self_hold_wanted) lives on the source it is attached to: it is
    // a sink from the attach on, so the source's caps are GoodCRC'd (a source that is never answered gives the attach up after
    // 7-10 s, OnePlus 120W 2026-11-06) and its 5 V contract is never lingered out. The Rd is the pinned one anyway.
    bool self_hold = false;
};
constexpr bool rd_demand(const CrcDemand& d) {
    return d.sink_active || d.sink_held || d.ident || d.getcap || d.prep || d.run_start || d.self_hold;
}
constexpr bool crc_demand(const CrcDemand& d) { return rd_demand(d) || d.spoof; }

// A contract nobody holds (Ready at 5 V after `pd release`, after a run, after a re-read on a contract that was already
// there) keeps the sink "active" for this long after the last thing that used it, then the driver forgets it and Rd goes:
// the source sees the detach. Held contracts, negotiations in flight, Identify, Get_Source_Cap, a prepare and a run's own
// Rd are all "engaged" and never time out. Before this the Ready contract kept Rd and Auto-CRC until a reboot.
inline constexpr std::uint32_t kSinkIdleLingerMs = 30000;

class ContractLinger {
public:
    // `engaged`: anything but a bare contract in use. True while the contract still counts: engaged, or within the
    // linger after the last engaged pass. Never engaged = never counts.
    bool update(std::uint32_t now, bool engaged) {
        if (engaged) { at_ = now; seen_ = true; return true; }
        return seen_ && now - at_ < kSinkIdleLingerMs;
    }
    void reset() { seen_ = false; }
private:
    bool seen_ = false;
    std::uint32_t at_ = 0;
};

// A charger whose Source_Capabilities went unanswered while Auto-CRC was off stops announcing and
// ignores a Request. The prepare cycles the CC lines (the source restarts and re-sends its caps, now
// ACKed) and sends the Request 5 ms after those caps.
inline constexpr std::uint32_t kPrepOpenMs = 150;        // CC lines open: the source sees a detach
inline constexpr std::uint32_t kPrepWaitCapsMs = 2500;   // wait for the re-announce (Anker A2343: ~1.5-2 s after the detach), then send anyway
inline constexpr std::uint32_t kPrepRetryOpenMs = 400;  // second, longer open when the first brought no caps back
inline constexpr std::uint32_t kPrepRequestDelayMs = 5;  // the request goes out ~5 ms after the caps

class TriggerPrep {
public:
    enum class Act : std::uint8_t { None, OpenCc, CloseCc, Send };
    // `caps_acked`: the caps on hand were received with Auto-CRC on, so the source is in a live PD state.
    void start(std::uint32_t now, std::uint32_t caps_count, bool caps_acked) {
        t0_ = now;
        count0_ = caps_count;
        step_ = caps_acked ? Step::Send : Step::Open;
        opened_ = false;
        retried_ = false;
    }
    void abort() { step_ = Step::Idle; }
    bool active() const { return step_ != Step::Idle; }
    // Auto-CRC is wanted from the CC close on (and for the immediate send), not while the lines are open.
    bool crc_wanted() const { return step_ == Step::Wait || step_ == Step::Delay || step_ == Step::Send; }
    Act tick(std::uint32_t now, std::uint32_t caps_count) {
        switch (step_) {
        case Step::Idle: return Act::None;
        case Step::Send: step_ = Step::Idle; return Act::Send;
        case Step::Open:
            if (!opened_) { opened_ = true; return Act::OpenCc; }
            if (now - t0_ >= (retried_ ? kPrepRetryOpenMs : kPrepOpenMs)) { step_ = Step::Wait; t1_ = now; return Act::CloseCc; }
            return Act::None;
        case Step::Wait:
            if (caps_count != count0_) { step_ = Step::Delay; t1_ = now; }
            else if (now - t1_ >= kPrepWaitCapsMs) {
                if (!retried_) { retried_ = true; opened_ = false; t0_ = now; step_ = Step::Open; return Act::None; }
                step_ = Step::Idle; return Act::Send;
            }
            return Act::None;
        case Step::Delay:
            if (now - t1_ >= kPrepRequestDelayMs) { step_ = Step::Idle; return Act::Send; }
            return Act::None;
        }
        return Act::None;
    }
private:
    enum class Step : std::uint8_t { Idle, Open, Wait, Delay, Send };
    Step step_ = Step::Idle;
    bool opened_ = false;
    bool retried_ = false;
    std::uint32_t t0_ = 0, t1_ = 0, count0_ = 0;
};

// Attach sessions. The id changes on every attach and every detach; Source_Capabilities belong to
// the session they arrived in. A Request needs caps of the current session: after a detach and a
// re-scan the old list may still sit in memory, but nobody ever offered it to this attach.
class CapsSession {
public:
    void attach() { ++id_; caps_id_ = 0; }
    void detach() { ++id_; caps_id_ = 0; }
    void caps_rx() { caps_id_ = id_; }
    bool fresh() const { return caps_id_ != 0 && caps_id_ == id_; }
    std::uint32_t id() const { return id_; }
private:
    std::uint32_t id_ = 0, caps_id_ = 0;
};

}  // namespace k2::pd
