// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fast-charge trigger engine, see k2/fastcharge/fc_engine.hpp. Pure logic over fc::Env; no hardware here.
#include "k2/fastcharge/fc_engine.hpp"

#include "k2/fastcharge/accp.hpp"
#include "k2/fastcharge/afc.hpp"
#include "k2/pd/pd_link.hpp"

namespace k2::fc {

namespace {
constexpr std::uint32_t kDischargeMs = 100, kFloatDpMs = 100, kBiasMs = 50;   // HVDCP handshake, method A
constexpr std::uint32_t kRedetectHoldMs = 500;   // a new D+/D- detection after a session: both lines 0 V for this long (inferred)
constexpr std::uint32_t kDropTimeoutMs = 2000, kDropPollMs = 10;
constexpr std::uint32_t kBandTimeoutMs = 3000, kLostMs = 1000, kPrevHoldMs = 1000;
constexpr std::uint32_t kQc3TransitionMs = 12000;
constexpr std::uint32_t kQc3GapMs = 20;     // least time between two polls of the pulse loop
constexpr std::uint32_t kQc3StillMs = 500;  // Baseus starts slewing ~200 ms after the pulses
constexpr std::uint32_t kQc3StableQuietMs = 100;   // no movement of kQc3MoveMv for this long before "stable" counts
constexpr int kQc3NoResponsePulses = 100;   // QC3+ pulses (about 2 V worth) that left VBUS where it was
// QC3 entry: 1000 ms after the handshake, then D+ 0.6 V / D- 3.3 V and 200 ms before pulsing.
constexpr std::uint32_t kQc3EnterWaitMs = 1000, kQc3BaselineMs = 200;
constexpr int kQc3BatchPulses = 4;       // 4 x 4.5 ms: a blocking list stays under 20 ms
constexpr int kQc3PlusBatchPulses = 50;
constexpr int kQc3CoarseMinPulses = 5;   // up pulses a batch needs before its per-pulse step can call the charger QC3-like
constexpr int kQc3MoveMv = 15, kQc3ProbePulses = 10;   // movement worth counting; size of the step-learning batch  // 50 x 90 us = 4.5 ms
constexpr int kMinQc3Mv = 3600;
// A Class A charger asked for more than its 12 V top (QC3 Class B ramp): it climbs to 12 V and then ignores every pulse. Two settled up
// batches that left VBUS in the 12 V band unmoved end the session as NoBand (what the 12 s band timeout ends it as, ~10 s sooner).
constexpr int kQc3ClassAMaxMv = 12000, kQc3ClassABandMv = 11400, kQc3DeadBatches = 2;
constexpr std::uint32_t kVoocRxTimeoutUs = 15000;  // one listen slice per poll: long, a frame that falls between slices is lost
constexpr std::uint32_t kVoocRampMs = 10000;       // accepted but still 5 V after this long: no voltage
constexpr int kVoocRaisedMv = 6000;                // VBUS above this after the accept: the adapter ramped
constexpr int kVoocGuardMv = 11000;                // abort above 110 % of this (12.1 V); the monitor ceiling still wins
constexpr std::uint32_t kVoocSilenceMs = 3000;     // no valid command for this long: give up
constexpr std::uint32_t kVoocWakeListenMs = 1500;  // no clock this long after a wake train: rest and try again
constexpr std::uint32_t kVoocRestMs = 3000;        // lines idle between wake trains
constexpr int kVoocTries = 3;                      // wake trains before the adapter is called silent ("needs load")
constexpr int kVoocActiveCmds = 3;                 // commands answered before the link counts as up
constexpr std::uint32_t kUfcsSilenceMs = 500;      // a running session that hears nothing at all for this long is lost
constexpr int kUfcsTries = 3;                      // asks per phase (Get_Output_Capabilities, Request) before "no response"
// The opener's retransmit interval, the listen window before "no response" and the opener code are the
// UFCS tuner's (k2/fastcharge/ufcs_tune.hpp; defaults 20 ms, 500 ms, Get_Output_Capabilities). The first
// ufcs::kFastTries openers go out at the retransmit pace (inferred), the rest at the keep-alive
// pace so a slow source is not flooded. A source's UFCS side may need hundreds of ms after the handshake
// to start (nothing in the reference bounds it), and the old ~60 ms window could not tell "silent" from "late".
constexpr std::uint32_t kVivoStepMs = 50, kVivoTickMs = 100, kVivoEntryMs = 2000, kVivoSilenceMs = 2000;
constexpr int kVivoRampMv = 100, kVivoReleaseSends = 3, kVivoMaxNotices = 4;
constexpr std::uint32_t kAfcSettleMs = 500;   // D+ held at 0.6 V before the first ping
constexpr std::uint32_t kAfcRetryLowMs = 100; // D- low before the one retry of an unanswered first exchange
}  // namespace

const char* name(Proto p) {
    switch (p) {
    case Proto::Qc2: return "QC2";
    case Proto::Qc3: return "QC3";
    case Proto::Qc3Plus: return "QC3+";
    case Proto::Afc: return "AFC";
    case Proto::Fcp: return "FCP";
    case Proto::Scp: return "SCP";
    case Proto::Vooc: return "VOOC";
    case Proto::SuperVooc: return "SVOOC";
    case Proto::SuperVooc65: return "SVOOC65";
    case Proto::Ufcs: return "UFCS";
    case Proto::Vivo: return "VIVO";
    default: return "?";
    }
}

const char* name(State s) {
    switch (s) {
    case State::Idle: return "idle";
    case State::Negotiating: return "negotiating";
    case State::Active: return "active";
    case State::Failed: return "failed";
    }
    return "?";
}

const char* name(Fail f) {
    switch (f) {
    case Fail::None: return "none";
    case Fail::NeedConfirm: return "need confirm";
    case Fail::OverLimit: return "over limit";
    case Fail::NoMonitor: return "no power monitor";
    case Fail::Busy: return "busy";
    case Fail::Unsupported: return "unsupported";
    case Fail::NoHandshake: return "no handshake";
    case Fail::NoResponse: return "no response";
    case Fail::NoBand: return "voltage not reached";
    case Fail::Overvoltage: return "overvoltage";
    case Fail::SourceLost: return "source lost";
    case Fail::Aborted: return "aborted";
    case Fail::Inline: return "PD or load active";
    case Fail::NeedsLoad: return "needs load";
    case Fail::NoVoltage: return "handshake ok";
    case Fail::Detected: return "detected";
    }
    return "?";
}

Fail Engine::validate(Proto p, int& target_mv, bool confirmed) const {
    const int ceiling = env_.ceiling_mv();
    if (ceiling <= 0) return Fail::NoMonitor;
    switch (p) {
    case Proto::Qc2:
        if (!qc::qc2_levels(target_mv)) return Fail::Unsupported;
        if (target_mv > ceiling) return Fail::OverLimit;
        break;
    case Proto::Qc3:
    case Proto::Qc3Plus:
        if (target_mv < kMinQc3Mv) return Fail::Unsupported;
        target_mv = qc::clamp_qc3_mv(target_mv, p == Proto::Qc3Plus ? std::min(ceiling, 12000) : ceiling);
        if (target_mv < kMinQc3Mv) return Fail::OverLimit;
        break;
    case Proto::Fcp:
        if (!accp::fcp_vout_code(target_mv)) return Fail::Unsupported;
        if (target_mv > ceiling) return Fail::OverLimit;
        break;
    case Proto::Scp:
        // 11 V unless the adapter's own limit registers (read by an earlier session) say it goes higher or lower.
        target_mv = accp::clamp_scp_mv(target_mv, ceiling, caps_.scp.valid ? caps_.scp.max_mv : accp::kScpMaxMv);
        if (target_mv < accp::kScpMinMv) return Fail::OverLimit;
        break;
    case Proto::Afc:
        if (!afc::afc_byte(target_mv)) return Fail::Unsupported;
        if (target_mv > ceiling) return Fail::OverLimit;
        break;
    case Proto::Ufcs:
        if (target_mv < ufcs::kMinMv || target_mv > ceiling) return Fail::OverLimit;
        target_mv = ufcs::units_to_mv(ufcs::mv_to_units(target_mv));
        break;
    case Proto::Vivo:   // 5 V, or 7..11 V (the unidentified-charger window)
        target_mv = (target_mv + 50) / 100 * 100;
        if (target_mv != vivo::kSafeMv && (target_mv < vivo::kStartMv || target_mv > vivo::kUnidentifiedMaxMv))
            return Fail::Unsupported;
        if (target_mv > ceiling) return Fail::OverLimit;
        break;
    case Proto::Vooc:
    case Proto::SuperVooc:
    case Proto::SuperVooc65:
        target_mv = kConfirmAboveMv;   // the adapter is only listened to: the output stays at 5 V
        break;
    default:
        return Fail::Unsupported;
    }
    if (target_mv > kConfirmAboveMv && !confirmed) return Fail::NeedConfirm;
    return Fail::None;
}

bool Engine::reg_read(std::uint8_t reg, std::uint8_t& out) {
    const auto tx = accp::read_frame(reg);
    std::uint8_t rx[3] = {};
    if (!env_.xfer(tx, rx)) return false;
    const auto v = accp::parse_read(rx);
    if (!v) return false;
    out = *v;
    acked_ = true;
    return true;
}

bool Engine::reg_write(std::uint8_t reg, std::uint8_t val) {
    const auto tx = accp::write_frame(reg, val);
    std::uint8_t rx[2] = {};
    if (!env_.xfer(tx, rx) || !accp::parse_write_ack(rx)) return false;
    acked_ = true;
    return true;
}

// The register scripts. FCP: read caps (must be non-zero), write voltage, current,
// enable. SCP: type checks, limits (logged only), control bytes, initial 5 V / 3.5 A, then the target.
void Engine::build_script() {
    using Op = Action::Op;
    using Chk = Action::Chk;
    script_n_ = 0;
    auto add = [&](Op op, Chk chk, std::uint8_t reg, std::uint8_t val, std::uint16_t wait) {
        if (script_n_ < script_.size()) script_[script_n_++] = {op, chk, reg, val, wait};
    };
    auto set_v = [&](int mv) {
        add(Op::Write, Chk::None, accp::kRegScpVset, accp::hi8(mv), 0);
        add(Op::Write, Chk::None, accp::kRegScpVset + 1, accp::lo8(mv), 0);
        // Anker Prime A2343: only the 8-bit register moves the output (16-bit writes are stored and ignored).
        if (mv <= accp::kScpMaxMv8) add(Op::Write, Chk::None, accp::kRegScpVset8, accp::scp_vset8(mv), 0);
    };
    if (proto_ == Proto::Fcp) {
        if (state_ == State::Negotiating && phase_ != Phase::Script && phase_ != Phase::Band)
            add(Op::Read, Chk::NonZero, accp::kRegFcpCaps, 0, 0);
        add(Op::Write, Chk::None, accp::kRegFcpVout, *accp::fcp_vout_code(target_mv_), 10);
        add(Op::Write, Chk::Soft, accp::kRegFcpIout, accp::kFcpIoutCode, 10);   // refused with a NACK by some adapters; the voltage write still counts
        add(Op::Write, Chk::None, accp::kRegFcpVoutEn, 1, 0);
    } else if (proto_ == Proto::Scp) {
        if (phase_ != Phase::Script && phase_ != Phase::Band && phase_ != Phase::ScpKeep) {
            add(Op::Read, Chk::Bit4Set, accp::kRegScpType0, 0, 0);
            add(Op::Read, Chk::Log, accp::kRegScpType1, 0, 0);
            for (std::uint8_t r = 0; r < 8; r++) add(Op::Read, Chk::Log, accp::kRegScpLimits + r, 0, 0);
            if (scp_ma_ <= 0) return;   // detect only: type bits and limits read, nothing written
            add(Op::Write, Chk::None, accp::kRegScpCtrl0, accp::kScpCtrl0Enable, accp::kScpEnableWaitMs);
            add(Op::Write, Chk::None, accp::kRegScpCtrl1, accp::kScpCtrl1Value, 0);
            set_v(accp::kScpInitMv);
            add(Op::Write, Chk::None, accp::kRegScpIset, accp::hi8(scp_ma_), 0);
            add(Op::Write, Chk::None, accp::kRegScpIset + 1, accp::lo8(scp_ma_), 0);
            add(Op::Write, Chk::None, accp::kRegScpIset8, static_cast<std::uint8_t>(std::min(255, scp_ma_ / 50)), 0);
        }
        if (target_mv_ != accp::kScpInitMv || script_n_ == 0) set_v(target_mv_);
    }
}

void Engine::begin_script() {
    script_i_ = 0;
    go(Phase::Script, 0);
}

// The current limit written before the target is held to the adapter's reported maximum; the write actions were built
// before the limit registers were read, so they are rewritten in place.
void Engine::patch_scp_current() {
    scp_ma_ = std::min(scp_ma_, caps_.scp.max_ma);
    for (std::size_t i = script_i_ + 1; i < script_n_; i++) {
        Action& w = script_[i];
        if (w.op != Action::Op::Write) continue;
        if (w.reg == accp::kRegScpIset) w.val = accp::hi8(scp_ma_);
        else if (w.reg == accp::kRegScpIset + 1) w.val = accp::lo8(scp_ma_);
        else if (w.reg == accp::kRegScpIset8) w.val = static_cast<std::uint8_t>(std::min(255, scp_ma_ / 50));
    }
}

void Engine::run_script() {
    using Chk = Action::Chk;
    const Action& a = script_[script_i_];
    std::uint8_t v = 0;
    if (a.op == Action::Op::Read) {
        if (!reg_read(a.reg, v)) return abort(Fail::NoResponse);
        env_.log_reg(a.reg, v);
        if (proto_ == Proto::Scp && a.reg >= accp::kRegScpLimits && a.reg < accp::kRegScpLimits + scp_lim_.size()) {
            scp_lim_[a.reg - accp::kRegScpLimits] = v;
            if (a.reg == accp::kRegScpLimits + scp_lim_.size() - 1) {   // the whole block is in: the adapter's capabilities
                caps_.scp = accp::scp_caps_decode(scp_lim_);
                if (caps_.scp.valid && scp_ma_ > 0) {
                    // Never ask above what it reported: a target past its maximum ends the session before anything is written,
                    // the current limit is lowered to its maximum.
                    if (target_mv_ > caps_.scp.max_mv) return abort(Fail::OverLimit);
                    patch_scp_current();
                }
            }
        }
        if ((a.chk == Chk::NonZero && v == 0) || (a.chk == Chk::Bit4Set && (!accp::scp_supported(v) || v == 0xFF))) {   // 0xFF: an idle/floating line, not a register
            return abort(Fail::Unsupported);
        }
    } else {
        const bool acked = reg_write(a.reg, a.val);
        env_.log_write(a.reg, a.val, acked);
        if (!acked && a.chk != Chk::Soft) return abort(Fail::NoResponse);
    }
    due_ms_ = env_.now_ms() + a.wait_ms;
    if (++script_i_ < script_n_) return;
    if (proto_ == Proto::Scp && scp_ma_ <= 0) return abort(Fail::Detected);   // every identifying read answered
    band_ms_ = env_.now_ms();
    keep_fail_ = 0;
    if (proto_ == Proto::Scp) {
        phase_ = Phase::ScpKeep;
        due_ms_ = band_ms_ + accp::kScpKeepAliveMs;
    } else {
        phase_ = Phase::Band;
    }
}

// SCP watchdog: ACCP traffic at least every 100 ms (the charger drops out after 500 ms of silence).
// Cycles through the 16-bit voltage (0xB8/0xB9) and current (0xBA/0xBB) readbacks, one byte a pass.
void Engine::keep_alive() {
    static constexpr std::uint8_t kRegs[4] = {accp::kRegScpVread, accp::kRegScpVread + 1, accp::kRegScpIread,
                                              accp::kRegScpIread + 1};
    std::uint8_t v = 0;
    if (reg_read(kRegs[keep_i_], v)) {
        keep_fail_ = 0;
        keep_raw_[keep_i_] = v;
        if (keep_i_ == 1) adapter_mv_ = accp::be16(keep_raw_[0], keep_raw_[1]);
        if (keep_i_ == 3) adapter_ma_ = accp::be16(keep_raw_[2], keep_raw_[3]);
    } else if (++keep_fail_ >= 3) {
        return abort(env_.vbus_mv() >= target_mv_ * 85 / 100 ? Fail::NoResponse : Fail::SourceLost);   // relative to the target
    }
    keep_i_ = (keep_i_ + 1) % 4;
    due_ms_ = env_.now_ms() + accp::kScpKeepAliveMs;
}

// Send the message built in utx_ and keep it until its ACK arrives (T/TAF 083-2024 8.2.3.2 and 8.3.1: every message
// that is not an ACK / NCK is acknowledged; no ACK within tACKReceive means the same frame, same number, goes out again).
void Engine::ufcs_track(std::size_t n, UKind kind, std::uint32_t now) {
    utx_n_ = static_cast<std::uint8_t>(n);
    utx_kind_ = kind;
    utx_id_ = uid_;
    utx_ms_ = now;
    uretx_ = 0;
    env_.ufcs_send({utx_.data(), n});
}

void Engine::ufcs_send_ctl(std::uint8_t cmd, UKind kind) {
    ufcs_track(ufcs::encode_control(utx_, cmd, uid_, env_.ufcs_tuner()), kind, env_.now_ms());
}

void Engine::ufcs_send_request(int pos, int mv, int ma) {
    ufcs_track(ufcs::encode_request(utx_, uid_, pos, mv, ma, env_.ufcs_tuner()), UKind::Main, env_.now_ms());
    prev_mv_ = std::max(prev_mv_, applied_mv_);
    applied_mv_ = mv;
    change_ms_ = env_.now_ms();
}

// The message sent last again, same number (no ACK, or an NCK).
void Engine::ufcs_resend(std::uint32_t now) {
    const std::span<const std::uint8_t> pending{ux_.data(), ux_n_};   // a half frame the source started, if any
    ++uretx_;
    env_.log_ufcs_retry(ufcs_what(), phase_ == Phase::UfcsPing && utx_kind_ == UKind::Main ? utries_ : uretx_, pending);
    utx_ms_ = now;
    env_.ufcs_send({utx_.data(), utx_n_});
}

const char* Engine::ufcs_what() const {
    switch (utx_kind_) {
    case UKind::Keep: return "ping";
    case UKind::Refuse: return "refuse";
    case UKind::Soft: return "soft";
    case UKind::Main: break;
    }
    return phase_ == Phase::UfcsPing ? "open" : phase_ == Phase::UfcsCaps ? "caps" : "req";
}

// A fresh negotiation: numbering from 0, nothing in flight, nothing owed.
void Engine::ufcs_clear() {
    uid_ = 0;
    utx_n_ = 0;
    uretx_ = 0;
    uwait_ = false;
    usoft_ = false;
    usrst_ = false;
    upend_ = UPend::None;
    uref_ = false;
    usrc_valid_ = false;
    uecho_ = uheard_ = false;
}

// A main message was answered before its ACK came (the source skipped it): the answer counts as the ACK.
void Engine::ufcs_implicit_ack() {
    if (utx_n_ > 0 && utx_kind_ == UKind::Main) {
        utx_n_ = 0;
        uid_ = static_cast<std::uint8_t>((uid_ + 1) & 0xF);
    }
}

// Next complete frame from the receive accumulator (valid until the next call), false when none yet.
bool Engine::ufcs_take(ufcs::Frame& f) {
    if (ux_drop_ > 0) {
        std::copy(ux_.begin() + static_cast<std::ptrdiff_t>(ux_drop_), ux_.begin() + static_cast<std::ptrdiff_t>(ux_n_), ux_.begin());
        ux_n_ -= ux_drop_;
        ux_drop_ = 0;
    }
    if (ux_n_ < ux_.size()) ux_n_ += env_.ufcs_recv(std::span(ux_).subspan(ux_n_));
    for (;;) {
        std::size_t used = 0;
        auto why = ufcs::Why::None;
        const auto r = ufcs::decode({ux_.data(), ux_n_}, f, used, &why, env_.ufcs_tuner());
        if (r == ufcs::Parse::Ok) {
            const std::size_t len = f.data ? f.payload.size() + 6 : 5;
            if (used > len) env_.log_ufcs_drop({ux_.data(), ux_n_}, used - len, ufcs::Why::NoSync);   // noise ahead of the sync
            ux_drop_ = used;
            return true;
        }
        if (used == 0 && ux_n_ == ux_.size()) { used = ux_n_; why = ufcs::Why::Overflow; }   // a full buffer without a frame is garbage
        if (used > 0) {
            env_.log_ufcs_drop({ux_.data(), ux_n_}, used, why);
            std::copy(ux_.begin() + static_cast<std::ptrdiff_t>(used), ux_.begin() + static_cast<std::ptrdiff_t>(ux_n_), ux_.begin());
            ux_n_ -= used;
        }
        if (r == ufcs::Parse::Need) return false;
    }
}

// Every message except Ack / Nack is acknowledged with the sender's message id.
// The ACK of our tracked message: the numbering moves on. Strictly by message number; any other ACK is stray.
void Engine::ufcs_on_ack(const ufcs::Frame& f, std::uint32_t now) {
    if (utx_n_ == 0 || f.id != utx_id_) return;
    const UKind kind = utx_kind_;
    utx_n_ = 0;
    if (kind == UKind::Soft) {   // both sides restarted their counters (8.2.3.5); the exchange begins again
        ufcs_source_reset(now);
        return;
    }
    uid_ = static_cast<std::uint8_t>((uid_ + 1) & 0xF);
    if (kind == UKind::Keep) usoft_ = false;
    if (kind != UKind::Main) return;
    if (phase_ == Phase::UfcsPing) {                       // the opener was heard
        utries_ = 1;
        if (env_.ufcs_tuner().opener == ufcs::kCtlGetOutputCap) {
            go(Phase::UfcsCaps, ufcs::kResponseWaitMs);   // it was the capabilities request itself
            uwait_ = true;
        } else {
            go(Phase::UfcsCaps, 0);
            upend_ = UPend::Caps;                         // the capabilities request follows after tMsgTransDelay
            upend_ms_ = now + ufcs::kMsgTransDelayMs;
        }
    } else if (phase_ == Phase::UfcsCaps || phase_ == Phase::UfcsRequest) {
        uwait_ = true;                                    // the answer is due within tReceiverResponse (plus slack)
        due_ms_ = now + ufcs::kResponseWaitMs;
    }
}

// Both sides have cleared their state: numbering from 0, nothing in flight, the contract void. In every phase of a
// live session (Request, Power wait, Keep) the opening starts over: the opener (Ping), Get_Output_Capabilities,
// then the Request for the current target. A source that resets again before its Power_Ready is not answered with
// a third try (see ufcs_on_msg).
void Engine::ufcs_source_reset(std::uint32_t now) {
    uid_ = 0;
    utx_n_ = 0;
    uretx_ = 0;
    uwait_ = false;
    uref_ = false;
    usrc_valid_ = false;
    utries_ = 0;
    go(Phase::UfcsPing, env_.ufcs_tuner().retry_ms);
    upend_ = UPend::Ping;
    upend_ms_ = now + ufcs::kMsgTransDelayMs;
}

// A message of the source that is not an ACK / NCK (already acknowledged by the caller).
void Engine::ufcs_on_msg(const ufcs::Frame& f, std::uint32_t now) {
    if (!f.data) {
        switch (f.cmd) {
        case ufcs::kCtlSoftReset:
            if (usrst_) return abort(phase_ == Phase::UfcsKeep || phase_ == Phase::UfcsPower ? Fail::SourceLost : Fail::NoResponse);
            usrst_ = true;
            return ufcs_source_reset(now);
        case ufcs::kCtlAccept:
            if (phase_ != Phase::UfcsRequest) return;
            ufcs_implicit_ack();
            uwait_ = false;
            upend_ = UPend::None;
            return go(Phase::UfcsPower, ufcs::kPowerWaitMs);   // tPowerSupply: Power_Ready comes before VBUS is judged
        case ufcs::kCtlPowerReady:
            if (phase_ != Phase::UfcsPower) return;
            usoft_ = false;
            usrst_ = false;
            band_ms_ = now;
            lost_ms_ = 0;
            ulast_rx_ = now;
            return go(Phase::UfcsKeep, ufcs::kKeepaliveMs);
        default: break;
        }
    } else if (f.cmd == ufcs::kDataOutputCap) {
        const std::size_t n = ufcs::decode_caps(f.payload, caps_.ufcs);
        if (phase_ == Phase::UfcsKeep) {   // a refreshed list: keep it for the next Request
            if (n > 0) caps_.ufcs_n = n;
            return;
        }
        if (phase_ != Phase::UfcsPing && phase_ != Phase::UfcsCaps) return;
        caps_.ufcs_n = n;
        ufcs_implicit_ack();
        uwait_ = false;
        ufcs::Pick pk;
        if (caps_.ufcs_n == 0) return abort(Fail::NoResponse);
        if (!ufcs::pick({caps_.ufcs.data(), caps_.ufcs_n}, target_mv_, env_.ceiling_mv(), pk, ufcs_ma_)) return abort(Fail::NoBand);
        upk_ = pk;
        upend_ = UPend::Request;
        upend_ms_ = now + ufcs::kMsgTransDelayMs;
        utries_ = 1;
        return go(Phase::UfcsRequest, 0);
    } else if (f.cmd == ufcs::kDataRefuse) {
        if (phase_ == Phase::UfcsCaps) return abort(Fail::NoResponse);
        if (phase_ == Phase::UfcsRequest || phase_ == Phase::UfcsPower) return abort(Fail::NoBand);   // the level is refused
        return;
    }
    if (const std::uint8_t why = ufcs::refuse_reason(f.data, f.cmd); why != 0 && !uref_) {
        uref_ = true;                       // answered with a Refuse once our ACK has had tMsgTransDelay
        uref_id_ = f.id;
        uref_type_ = f.data ? 1 : 0;
        uref_cmd_ = f.cmd;
        uref_reason_ = why;
        uref_ms_ = now + ufcs::kMsgTransDelayMs;
    }
}

// The message in flight went unanswered nMsgRetryCount times: a Soft_Reset (once), then the hardware reset.
void Engine::ufcs_escalate(std::uint32_t now) {
    const bool running = phase_ == Phase::UfcsKeep || phase_ == Phase::UfcsPower;
    if (utx_kind_ == UKind::Soft || usoft_) return abort(running ? Fail::SourceLost : Fail::NoResponse);
    usoft_ = true;
    uwait_ = false;
    upend_ = UPend::None;
    (void)now;
    ufcs_send_ctl(ufcs::kCtlSoftReset, UKind::Soft);
}

// Something the sink owes the source goes out as soon as nothing is in flight: a Refuse, then a queued main message.
void Engine::ufcs_send_queued(std::uint32_t now) {
    if (utx_n_ > 0) return;
    if (uref_ && now >= uref_ms_) {
        uref_ = false;
        ufcs_track(ufcs::encode_refuse(utx_, uid_, uref_id_, uref_type_, uref_cmd_, uref_reason_, env_.ufcs_tuner()), UKind::Refuse, now);
        return;
    }
    if (upend_ == UPend::None || now < upend_ms_) return;
    const UPend what = upend_;
    upend_ = UPend::None;
    if (what == UPend::Ping) ufcs_send_ctl(env_.ufcs_tuner().opener, UKind::Main);
    else if (what == UPend::Caps) ufcs_send_ctl(ufcs::kCtlGetOutputCap, UKind::Main);
    else ufcs_send_request(upk_.pos, upk_.mv, upk_.ma);
    uwait_ = false;
}

void Engine::ufcs_poll(std::uint32_t now) {
    const ufcs::Tuner& tn = env_.ufcs_tuner();
    if (phase_ == Phase::UfcsOpen) {
        if (!env_.ufcs_open()) return abort(Fail::NoHandshake);
        uart_up_ = true;
        ux_n_ = ux_drop_ = 0;
        ulast_rx_ = now;
        ufcs_clear();
        // The first frame after the handshake is the tuner's opener (default Ping, message number 0): the
        // source acknowledges it, then Get_Output_Capabilities follows (the order of the Baseus FH22 charger log).
        // Retransmits keep the number; the first few go out at the tuner's pace, the rest at the keep-alive pace.
        utries_ = 0;
        go(Phase::UfcsPing, tn.retry_ms);
        ufcs_send_ctl(tn.opener, UKind::Main);
        return;
    }

    ufcs::Frame f;
    int acked_id = -1;   // the message number acknowledged last in this pass
    while (ufcs_take(f)) {
        env_.log_ufcs_frame(f);
        if (f.addr != ufcs::kAddrSink) {   // not for us: our own transmission looping back (D+/D- shorted), or someone else's
            const std::size_t len = f.data ? f.payload.size() + 6 : 5;
            if (utx_n_ == len && ux_drop_ >= len && std::equal(utx_.begin(), utx_.begin() + len, ux_.begin() + (ux_drop_ - len)))
                uecho_ = true;             // byte for byte the frame we sent last
            continue;                      // no ACK, no state change, not a sign of life
        }
        uheard_ = true;
        ulast_rx_ = now;
        if (!f.data && f.cmd == ufcs::kCtlAck) {
            ufcs_on_ack(f, now);
            continue;
        }
        if (!f.data && f.cmd == ufcs::kCtlNack) {   // the source did not take the message: again, same number, at once
            if (utx_n_ == 0 || f.id != utx_id_) continue;
            if (uretx_ >= ufcs::kMsgRetryCount && !(phase_ == Phase::UfcsPing && utx_kind_ == UKind::Main)) {
                ufcs_escalate(now);
            } else {
                if (phase_ == Phase::UfcsPing && utx_kind_ == UKind::Main) {
                    ++utries_;
                    due_ms_ = now + ufcs::open_interval_ms(tn, utries_);
                }
                ufcs_resend(now);
            }
            if (state_ == State::Failed) return;
            continue;
        }
        // Every other message is acknowledged with its own number at once. A source whose ACK came late retransmits
        // (same number, same bytes) up to three times; copies that were all waiting in this one read share one ACK
        // (an ACK matches by number, a second one for the same number would only be noise on the wire).
        const bool again = usrc_valid_ && f.id == usrc_id_ && f.cmd == usrc_cmd_ && f.data == usrc_data_;   // its retransmission
        if (!(again && acked_id == f.id)) {
            std::array<std::uint8_t, 8> b{};
            env_.ufcs_send({b.data(), ufcs::encode_control(b, ufcs::kCtlAck, f.id, tn)});
        }
        acked_id = f.id;
        usrc_valid_ = true;
        usrc_id_ = f.id;
        usrc_cmd_ = f.cmd;
        usrc_data_ = f.data;
        if (again) continue;
        ufcs_on_msg(f, now);
        if (state_ == State::Failed) return;
    }

    // Time: the opener's own schedule, an unanswered message, a late answer, the supply wait.
    if (phase_ == Phase::UfcsPing && now - phase_ms_ >= tn.listen_ms) {
        if (!uecho_ || uheard_) return abort(Fail::NoResponse);
        env_.log_ufcs_echo();              // only our own frames came back and nothing else: the handshake was a false positive
        return abort(Fail::NoHandshake);
    }
    if (utx_n_ > 0) {
        if (phase_ == Phase::UfcsPing && utx_kind_ == UKind::Main) {
            if (now >= due_ms_) {
                ++utries_;
                due_ms_ = now + ufcs::open_interval_ms(tn, utries_);
                ufcs_resend(now);
            }
        } else if (now - utx_ms_ >= ufcs::kAckWaitMs) {
            if (uretx_ < ufcs::kMsgRetryCount) ufcs_resend(now);
            else ufcs_escalate(now);
            if (state_ == State::Failed) return;
        }
    }
    if (uwait_ && now >= due_ms_) {   // acknowledged but never answered: ask again with a new number
        uwait_ = false;
        if (utries_ >= kUfcsTries) return abort(Fail::NoResponse);
        ++utries_;
        if (phase_ == Phase::UfcsCaps) {
            upend_ = UPend::Caps;
        } else {
            ufcs::Pick pk;
            if (!ufcs::pick({caps_.ufcs.data(), caps_.ufcs_n}, target_mv_, env_.ceiling_mv(), pk, ufcs_ma_)) return abort(Fail::NoBand);
            upk_ = pk;
            upend_ = UPend::Request;
        }
        upend_ms_ = now;
    }
    ufcs_send_queued(now);

    if (phase_ == Phase::UfcsPower) {
        if (now >= due_ms_) abort(Fail::NoBand);   // no Power_Ready within tPowerSupply: hardware reset (8.3.3)
        return;
    }
    if (phase_ == Phase::UfcsKeep) {
        if (now - ulast_rx_ >= kUfcsSilenceMs) return abort(Fail::SourceLost);
        if (utx_n_ == 0 && !uref_ && upend_ == UPend::None && now >= due_ms_) {
            ufcs_send_ctl(tn.keepalive, UKind::Keep);
            due_ms_ = now + ufcs::kKeepaliveMs;
        }
        const int vbus = reached_mv_;
        if (in_band(target_mv_, vbus)) {
            state_ = State::Active;
            lost_ms_ = 0;
        } else if (state_ == State::Active) {
            if (lost_ms_ == 0) lost_ms_ = now;
            else if (now - lost_ms_ >= kLostMs) abort(Fail::SourceLost);
        } else if (now - band_ms_ >= kBandTimeoutMs) {
            abort(Fail::NoBand);
        }
    }
}

// Next complete VIVO frame from the receive accumulator, false when none yet.
bool Engine::vivo_take(vivo::Reply& r) {
    if (ux_drop_ > 0) {
        std::copy(ux_.begin() + static_cast<std::ptrdiff_t>(ux_drop_), ux_.begin() + static_cast<std::ptrdiff_t>(ux_n_), ux_.begin());
        ux_n_ -= ux_drop_;
        ux_drop_ = 0;
    }
    if (ux_n_ < ux_.size()) ux_n_ += env_.ufcs_recv(std::span(ux_).subspan(ux_n_));
    for (;;) {
        std::size_t used = 0;
        const auto p = vivo::decode({ux_.data(), ux_n_}, r, used);
        if (p == vivo::Parse::Ok) { ux_drop_ = used; return true; }
        if (used == 0 && ux_n_ == ux_.size()) used = ux_n_;
        if (used > 0) {
            std::copy(ux_.begin() + static_cast<std::ptrdiff_t>(used), ux_.begin() + static_cast<std::ptrdiff_t>(ux_n_), ux_.begin());
            ux_n_ -= used;
        }
        if (p == vivo::Parse::Need) return false;
    }
}

void Engine::vivo_send(const vivo::Canned& c) { env_.ufcs_send({c.bytes.data(), c.len}); }

// Command-3 frame: 2000 mA for the canned 5 V level, 6000 mA above it.
void Engine::vivo_send_volt(int mv) {
    std::array<std::uint8_t, 10> f{};
    const std::size_t n = vivo::encode_voltage(f, mv == vivo::kSafeMv ? vivo::kCannedMa : vivo::kDefaultMa,
                                               static_cast<std::uint16_t>(mv));
    env_.ufcs_send({f.data(), n});
    vreq_ = mv;
    prev_mv_ = std::max(prev_mv_, applied_mv_);
    applied_mv_ = mv;
    change_ms_ = env_.now_ms();
}

// Ask for 5 V a few times before the UART goes away, so the source does not rely on its watchdog.
void Engine::vivo_release() {
    if (!uart_up_ || proto_ != Proto::Vivo || phase_ == Phase::VivoOpen) return;
    for (int i = 0; i < kVivoReleaseSends; i++) vivo_send_volt(vivo::kSafeMv);
}

void Engine::vivo_advance(std::uint32_t now) {
    if (++vi_ < vivo::kSequence.size()) {
        vivo_send(vivo::kSequence[vi_]);
        due_ms_ = now + kVivoStepMs;
        return;
    }
    // Sequence done: 5 V stays on the canned level, anything higher starts at 7 V and ramps.
    vivo_send_volt(target_mv_ == vivo::kSafeMv ? vivo::kSafeMv : vivo::kStartMv);
    band_ms_ = now;
    lost_ms_ = 0;
    go(Phase::VivoRun, kVivoTickMs);
}

void Engine::vivo_poll(std::uint32_t now) {
    if (phase_ == Phase::VivoOpen) {
        if (!env_.vivo_open()) return abort(Fail::NoHandshake);
        uart_up_ = true;
        ux_n_ = ux_drop_ = 0;
        ulast_rx_ = now;
        vheard_ = false;
        utries_ = 0;
        vi_ = 0;
        vivo_send(vivo::kSequence[0]);
        return go(Phase::VivoSeq, kVivoStepMs);
    }
    vivo::Reply r;
    while (vivo_take(r)) {
        // Only the charger's own frames count: a reply or the entry notice. Any other checksum-valid frame is the K2's
        // own request coming back through a D+/D- short (a plain DCP), which proves nothing about the source.
        if (r.cmd != vivo::kReplyCmd && r.cmd != vivo::kNoticeCmd) continue;
        ulast_rx_ = now;
        vheard_ = true;
        if (r.cmd == vivo::kNoticeCmd) {   // entry notice: answer it, and start over while still in the sequence
            if (++utries_ > kVivoMaxNotices) continue;
            vivo_send(vivo::kNoticeAck);
            if (phase_ == Phase::VivoSeq && vi_ > 0) {
                vi_ = 0;
                vivo_send(vivo::kSequence[0]);
                due_ms_ = now + kVivoStepMs;
            }
        } else if (phase_ == Phase::VivoSeq && vivo::acks(r, vivo::kSequence[vi_].bytes[2])) {
            vivo_advance(now);
        }
    }
    if (phase_ == Phase::VivoSeq) {
        if (!vheard_ && now - phase_ms_ >= kVivoEntryMs) return abort(Fail::NoResponse);
        if (vheard_ && now - ulast_rx_ >= kVivoSilenceMs) return abort(Fail::SourceLost);
        if (now >= due_ms_) {   // a charger that has not spoken yet gets the first frame again, never the next
            if (vheard_) vivo_advance(now);
            else { vivo_send(vivo::kSequence[vi_]); due_ms_ = now + kVivoStepMs; }
        }
        return;
    }
    // VivoRun: ramp towards the target in 100 mV steps, keepalive in between, all on a 100 ms tick.
    if (now >= due_ms_) {
        due_ms_ = now + kVivoTickMs;
        if (vreq_ != target_mv_) {
            const int up = std::min(vreq_ + kVivoRampMv, target_mv_), down = std::max(vreq_ - kVivoRampMv, target_mv_);
            vivo_send_volt(vreq_ < target_mv_ ? up : down);
            if (vreq_ == target_mv_) band_ms_ = now;
        } else {
            vivo_send(vivo::kKeepalive);
        }
    }
    if (in_band(target_mv_, reached_mv_)) {
        state_ = State::Active;
        lost_ms_ = 0;
    } else if (state_ == State::Active) {
        if (lost_ms_ == 0) lost_ms_ = now;
        else if (now - lost_ms_ >= kLostMs) abort(Fail::SourceLost);
    } else if (vreq_ == target_mv_ && now - band_ms_ >= kBandTimeoutMs) {
        abort(Fail::NoBand);
    }
}

// VOOC wake train: with D- floating, 13 x (D+ 0 V 506 us, 3.3 V 510 us, 0 V 1010 us, 3.3 V 1510 us),
// then 0 V / 3.3 V / 0 V for 500 us each. The adapter answers it with its own D+ clock.
void Engine::vooc_wake() {
    ++vooc_tries_;
    env_.set_dm(Drive::HiZ);
    std::array<Step, 13 * 4 + 3> w{};
    std::size_t n = 0;
    for (int i = 0; i < 13; ++i) {
        w[n++] = {Drive::V0, Drive::HiZ, 506}; w[n++] = {Drive::V3p3, Drive::HiZ, 510};
        w[n++] = {Drive::V0, Drive::HiZ, 1010}; w[n++] = {Drive::V3p3, Drive::HiZ, 1510};
    }
    w[n++] = {Drive::V0, Drive::HiZ, 500}; w[n++] = {Drive::V3p3, Drive::HiZ, 500}; w[n++] = {Drive::V0, Drive::HiZ, 500};
    env_.play(w);
    env_.set_dp(Drive::Listen);
    env_.set_dm(Drive::Sniff);
    go(Phase::VoocListen, 0);
    band_ms_ = env_.now_ms();
}

// The first move of a protocol that has no HVDCP handshake; start() calls it at once or when RedetectHold ends.
void Engine::begin_protocol() {
    const Proto p = proto_;
    if (p == Proto::Afc) {          // no HVDCP handshake: D+ at 0.6 V, D- idle low, then the ping
        env_.set_dp(Drive::V0p6);
        env_.set_dm(Drive::V0);
        go(Phase::AfcSettle, kAfcSettleMs);
    } else if (p == Proto::Ufcs) {  // the handshake and the UART claim happen in ufcs_open()
        caps_.ufcs_n = ux_n_ = 0;
        ufcs_clear();
        utries_ = 0;
        go(Phase::UfcsOpen, 0);
    } else if (p == Proto::Vivo) {  // no D+/D- handshake: vivo_open() claims the UART with the lines high-Z
        ux_n_ = 0;
        go(Phase::VivoOpen, 0);
    } else {                        // VOOC / SVOOC
        vooc_ = vooc::Responder(p == Proto::Vooc ? vooc::Mode::Vooc
                                : p == Proto::SuperVooc ? vooc::Mode::SuperVooc5A : vooc::Mode::SuperVooc65A);
        vooc_cmds_ = vooc_frames_ = vooc_svooc_ = 0;
        vooc_ident_ = false;
        vooc_accepted_ = false;
        applied_mv_ = kVoocGuardMv;   // the adapter may ramp on its own: the guard is 12 V, not the 5 V request
        vooc_tries_ = 0;
        vooc_wake();
        go(Phase::VoocListen, 0);
        band_ms_ = env_.now_ms();
    }
}

Fail Engine::start(Proto p, int target_mv, bool confirmed, bool allow_inline, int scp_ma, int ufcs_ma) {
    if (state_ == State::Negotiating || state_ == State::Active) return Fail::Busy;
    const Fail f = validate(p, target_mv, confirmed);
    if (f != Fail::None) return f;
    if (allow_inline && !confirmed) return Fail::NeedConfirm;   // the override drives D+/D- with a sink possibly attached: both flags
    if (!allow_inline && env_.inline_reason() != link::Reason::None) return Fail::Inline;
    const std::uint32_t now = env_.now_ms();
    proto_ = p;
    target_mv_ = target_mv;
    scp_ma_ = scp_ma;
    ufcs_ma_ = ufcs_ma;
    if (p == Proto::Scp) caps_.scp = {};   // validated against the previous session's limits; this session reads them again
    adapter_mv_ = adapter_ma_ = 0;         // the readback of an earlier session says nothing about this one
    applied_mv_ = prev_mv_ = kConfirmAboveMv;   // until the data lines say otherwise, expect 5 V
    change_ms_ = now;
    reached_mv_ = 0;
    entry_mv_ = std::max(0, env_.vbus_mv());
    fail_ = Fail::None;
    state_ = State::Negotiating;
    band_ms_ = lost_ms_ = 0;
    sends_ = ok_sends_ = afc_n_ = 0;
    afc_retried_ = false;
    acked_ = vheard_ = false;
    // Re-detection right after a session: the lines stay at 0 V until the 500 ms hold since that release is over (pattern
    // qc-redetect-after-reset). Every protocol waits for it, not only the HVDCP ones: fctrig::start cancels the reset's own hold,
    // so a first move made inside it (the AFC settle, the VOOC wake, the UFCS / vivo UART claim) ran against a charger still
    // re-detecting. The openings that need no D+/D- handshake are deferred to RedetectHold's end.
    const std::uint32_t since = reset_seen_ ? now - reset_ms_ : kRedetectHoldMs;
    const std::uint32_t hold = since < kRedetectHoldMs ? kRedetectHoldMs - since : 0;
    if (p == Proto::Afc || p == Proto::Vooc || p == Proto::SuperVooc || p == Proto::SuperVooc65 || p == Proto::Ufcs ||
        p == Proto::Vivo) {
        if (hold > 0) {
            env_.set_dp(Drive::V0);
            env_.set_dm(Drive::V0);
            go(Phase::RedetectHold, hold);
            return Fail::None;
        }
        begin_protocol();
        return Fail::None;
    }
    env_.set_dp(Drive::V0);
    env_.set_dm(Drive::V0);
    if (p == Proto::Scp) {
        go(Phase::HsbReset, std::max<std::uint32_t>(qc::kHsbResetMs, hold));
        return Fail::None;
    }
    go(Phase::HsDischarge, std::max(kDischargeMs, hold));
    return Fail::None;
}

Fail Engine::set_target(int target_mv, bool confirmed, int ufcs_ma) {
    if (state_ != State::Negotiating && state_ != State::Active) return Fail::Unsupported;
    if (phase_ == Phase::VoocListen) return Fail::Unsupported;   // the VOOC output never leaves 5 V
    if (phase_ == Phase::RedetectHold || phase_ == Phase::HsDischarge || phase_ == Phase::HsFloatDp || phase_ == Phase::HsBias ||
        phase_ == Phase::HsSample || phase_ == Phase::HsWaitDrop || phase_ == Phase::UfcsOpen ||
        phase_ == Phase::UfcsPing || phase_ == Phase::UfcsCaps || phase_ == Phase::UfcsRequest ||
        phase_ == Phase::UfcsPower || phase_ == Phase::VivoOpen || phase_ == Phase::VivoSeq) {
        return Fail::Busy;
    }
    const Fail f = validate(proto_, target_mv, confirmed);
    if (f != Fail::None) return f;
    const std::uint32_t now = env_.now_ms();
    prev_mv_ = guard_mv();
    entry_mv_ = std::max(0, env_.vbus_mv());
    qc3_nomove_ = qc3_dead_ = 0;
    change_ms_ = now;
    target_mv_ = target_mv;
    state_ = State::Negotiating;
    band_ms_ = now;
    lost_ms_ = 0;
    if (proto_ == Proto::Qc2) apply_target_levels();
    else if (proto_ == Proto::Afc) { sends_ = ok_sends_ = 0; afc_retried_ = false; go(Phase::AfcSend, 0); }
    else if (proto_ == Proto::Fcp || proto_ == Proto::Scp) { applied_mv_ = target_mv; build_script(); begin_script(); }
    else if (proto_ == Proto::Ufcs) {
        ufcs::Pick pk;
        if (!ufcs::pick({caps_.ufcs.data(), caps_.ufcs_n}, target_mv, env_.ceiling_mv(), pk, ufcs_ma)) {
            target_mv_ = pk.mv ? pk.mv : applied_mv_;
            state_ = State::Active;
            return Fail::Unsupported;
        }
        upk_ = pk;                          // goes out as soon as nothing else is in flight (a keep-alive Ping)
        ufcs_ma_ = ufcs_ma;
        upend_ = UPend::Request;
        upend_ms_ = now;
        uwait_ = false;
        utries_ = 1;
        go(Phase::UfcsRequest, 0);
    }
    else if (proto_ == Proto::Vivo) {
        if (target_mv == vivo::kSafeMv) vivo_send_volt(vivo::kSafeMv);       // lowering: jump straight down
        else if (vreq_ < static_cast<int>(vivo::kStartMv)) vivo_send_volt(vivo::kStartMv);   // from 5 V
    }
    else applied_mv_ = target_mv;
    return Fail::None;
}

void Engine::stop() {
    if (proto_ == Proto::Ufcs && uart_up_ && state_ != State::Failed) {
        // Leave UFCS mode in the polite way (Exit_UFCS_Mode, 8.2.3.16) so the source returns to 5 V by itself, then
        // the bus reset below releases the lines. It is not waited for (no ACK is expected from a leaving source).
        std::array<std::uint8_t, 8> b{};
        env_.ufcs_send({b.data(), ufcs::encode_control(b, ufcs::kCtlExit, uid_, env_.ufcs_tuner())});
    }
    if (state_ != State::Failed) vivo_release();
    if (uart_up_) { uart_up_ = false; env_.ufcs_close(); }
    env_.reset_bus();
    reset_ms_ = env_.now_ms(); reset_seen_ = true;
    state_ = State::Idle;
    phase_ = Phase::Off;
    fail_ = Fail::None;
    target_mv_ = 0;
}

void Engine::abort(Fail why) {
    // AFC / FCP / SCP that never answered: no reply to the first exchange (or no HVDCP handshake to carry it) means the charger does
    // not speak the protocol. The rest (overvoltage above the ceiling, source lost, ...) keeps its own code.
    if (unacked_handshake() && (why == Fail::NoResponse || why == Fail::NoHandshake)) why = Fail::Unsupported;
    vivo_release();
    if (uart_up_) { uart_up_ = false; env_.ufcs_close(); }
    env_.reset_bus();
    reset_ms_ = env_.now_ms(); reset_seen_ = true;
    state_ = State::Failed;
    phase_ = Phase::Off;
    fail_ = why;
}

void Engine::go(Phase p, std::uint32_t wait_ms) {
    phase_ = p;
    phase_ms_ = env_.now_ms();
    due_ms_ = phase_ms_ + wait_ms;
}

void Engine::apply_target_levels() {
    const auto l = *qc::qc2_levels(target_mv_);
    env_.set_dp(l.dp);
    env_.set_dm(l.dm);
    prev_mv_ = std::max(prev_mv_, applied_mv_);
    applied_mv_ = target_mv_;
    band_ms_ = env_.now_ms();
}

int Engine::guard_mv() const {
    int g = applied_mv_;
    const bool pulsing = phase_ == Phase::Qc3Loop && state_ == State::Negotiating &&
                         env_.now_ms() - band_ms_ < kQc3TransitionMs;
    if (pulsing || env_.now_ms() - change_ms_ < kPrevHoldMs) g = std::max(g, prev_mv_);
    return g;
}

void Engine::service_ufcs() {
    if (state_ != State::Negotiating && state_ != State::Active) return;
    if (phase_ < Phase::UfcsPing || phase_ > Phase::UfcsKeep) return;   // UfcsOpen runs the handshake: main loop only
    ufcs_poll(env_.now_ms());
}

// The HVDCP handshake steps (method A) that poll() and service_hs() share: one deadline-driven step of HsDischarge .. HsWaitDrop.
// Fail::NoHandshake = the charger did not answer; poll() aborts on it, the hook leaves it to poll().
Fail Engine::hs_step(std::uint32_t now) {
    switch (phase_) {
    case Phase::HsDischarge:
        if (now < due_ms_) break;
        env_.set_dp(Drive::HiZ);
        go(Phase::HsFloatDp, kFloatDpMs);
        break;
    case Phase::HsFloatDp:
        if (now < due_ms_) break;
        env_.set_dp(Drive::V0p6);
        env_.set_dm(Drive::HiZ);
        go(Phase::HsBias, kBiasMs);
        break;
    case Phase::HsBias:
        if (now < due_ms_) break;
        dm0_mv_ = env_.dm_mv();
        if (!qc::handshake_initial_ok(dm0_mv_)) return Fail::NoHandshake;
        go(Phase::HsWaitDrop, kDropPollMs);
        break;
    case Phase::HsWaitDrop: {
        if (now < due_ms_) break;
        const int dm = env_.dm_mv();
        if (qc::handshake_dropped(dm0_mv_, dm)) {
            band_ms_ = now;
            env_.set_dm(Drive::V0);
            if (proto_ == Proto::Fcp) {
                applied_mv_ = target_mv_;
                build_script();
                begin_script();
            } else if (proto_ == Proto::Qc2) {
                apply_target_levels();
                go(Phase::Qc2Settle, 0);
            } else {
                applied_mv_ = target_mv_;
                go(Phase::Qc3Enter, kQc3EnterWaitMs);   // 1000 ms with D- low before the baseline
            }
            break;
        }
        if (now - phase_ms_ >= kDropTimeoutMs - kDropPollMs && now - change_ms_ >= kDropTimeoutMs + 400) return Fail::NoHandshake;
        due_ms_ = now + kDropPollMs;
        break;
    }
    default: break;
    }
    return Fail::None;
}

void Engine::service_hs() {
    if (busy_ || state_ != State::Negotiating || proto_ == Proto::Fcp) return;
    if (phase_ < Phase::HsDischarge || phase_ > Phase::HsWaitDrop) return;
    busy_ = true;
    (void)hs_step(env_.now_ms());
    busy_ = false;
}

void Engine::poll() {
    if (busy_) return;
    busy_ = true;
    poll_step();
    busy_ = false;
}

void Engine::poll_step() {
    if (state_ != State::Negotiating && state_ != State::Active) return;
    const std::uint32_t now = env_.now_ms();
    const int vbus = env_.vbus_mv();
    if (vbus < 0) return abort(Fail::NoMonitor);
    reached_mv_ = vbus;
    // A down-target below where VBUS started, with VBUS not risen above that start level, is a target that was never
    // reached (the row ends as "voltage not reached"), not an overvoltage.
    const bool unmoved = entry_mv_ > 0 && target_mv_ < entry_mv_ && vbus <= entry_mv_ * 103 / 100;
    if (vbus > env_.ceiling_mv()) return abort(Fail::Overvoltage);   // the monitor's own limit: an overvoltage whoever answered
    // Past the target's guard band. Before the charger has answered AFC / FCP / SCP itself this is a QC-only source following the
    // D+/D- levels: the bus is dropped all the same, but the finding is "not supported", not an overvoltage.
    // VIVO is the same case while no frame of the charger has been heard (vheard_); its NoResponse stays as it is, that is a result of its own.
    if (over_guard(guard_mv(), vbus) && !unmoved)
        return abort(unacked_handshake() || (proto_ == Proto::Vivo && !vheard_) ? Fail::Unsupported : Fail::Overvoltage);
    if (phase_ == Phase::Qc3Loop && state_ == State::Negotiating && now - band_ms_ >= kQc3TransitionMs)
        return abort(Fail::NoBand);
    // Open-loop bound (inferred): once the pulse size is known, the pulses sent explain a highest level; above it
    // the charger moved on its own, which the 110 % guard alone would let through.
    if (phase_ == Phase::Qc3Loop && qc3_learned_ && vbus > qc::open_loop_limit_mv(entry_mv_, target_mv_, qc3_step_mv_))
        return abort(Fail::Overvoltage);

    switch (phase_) {
    case Phase::RedetectHold:
        if (now < due_ms_) return;
        return begin_protocol();
    case Phase::HsDischarge: case Phase::HsFloatDp: case Phase::HsBias: case Phase::HsWaitDrop: {
        const Fail f = hs_step(now);
        if (f != Fail::None) return abort(f);
        return;
    }
    case Phase::Qc3Enter:
        if (now < due_ms_) return;
        env_.set_dp(Drive::V0p6);          // continuous mode baseline
        env_.set_dm(Drive::V3p3);
        qc3_pending_mv_ = 0;
        qc3_sent_ms_ = now;
        qc3_seen_mv_ = qc3_base_mv_ = env_.vbus_mv();
        qc3_cmd_n_ = 0;
        qc3_learned_ = false;
        qc3_nomove_ = qc3_dead_ = 0;
        qc3_settle_.reset();
        qc3_step_mv_ = qc::step_mv(proto_ == Proto::Qc3 ? qc::Mode::Qc3 : qc::Mode::Qc3Plus);
        return go(Phase::Qc3Loop, kQc3BaselineMs);
    case Phase::HsbReset:
        if (now < due_ms_) return;
        env_.set_dm(Drive::HiZ);
        env_.set_dp(Drive::V0p6);
        return go(Phase::HsbBias, qc::kHsbBiasMs);
    case Phase::HsbBias:
        if (now < due_ms_) return;
        if (!qc::handshake_b_bias_ok(env_.dm_mv())) return abort(Fail::NoHandshake);
        wake_i_ = 0;
        env_.set_dp(qc::kWake[0].dp);
        return go(Phase::HsbWake, qc::kWake[0].ms);
    case Phase::HsbWake:
        if (now < due_ms_) return;
        if (++wake_i_ < 4) {
            env_.set_dp(qc::kWake[wake_i_].dp);
            return go(Phase::HsbWake, qc::kWake[wake_i_].ms);
        }
        return go(Phase::HsbWaitLow, 0);
    case Phase::HsbWaitLow:
        if (qc::handshake_b_low(env_.dm_mv())) return go(Phase::HsbFinish, qc::kHsbFinishMs);
        if (now - phase_ms_ >= qc::kHsbPollTimeoutMs) return abort(Fail::NoHandshake);
        return;
    case Phase::HsbFinish:
        if (now < due_ms_) return;
        env_.set_dm(Drive::V0);
        applied_mv_ = target_mv_;
        build_script();
        return begin_script();
    case Phase::Script:
        if (now < due_ms_) return;
        return run_script();
    case Phase::ScpKeep:
        if (now < due_ms_) return;
        keep_alive();
        if (state_ == State::Failed) return;
        break;
    case Phase::VoocListen: {
        const int f = env_.vooc_rx(kVoocRxTimeoutUs);
        if (f >= 0) {
            ++vooc_frames_;
            const std::uint8_t cmd = vooc::frame_cmd(static_cast<std::uint16_t>(f));
            const auto r = vooc_.reply(cmd);
            if (r && env_.vooc_tx(*r)) {
                band_ms_ = now;
                ++vooc_cmds_;
                if (cmd == 0xA8) vooc_ident_ = true;
                else if (vooc_.mode() != vooc::Mode::Vooc && vooc::Responder::svooc_only(cmd)) ++vooc_svooc_;
                if (vooc_.mode() == vooc::Mode::Vooc) {
                    state_ = State::Active;                       // 4 A VOOC: the ident answer is the handshake
                } else if (!vooc_accepted_ && vooc::Responder::accepts(cmd, *r)) {
                    vooc_accepted_ = true;
                    vooc_accept_ms_ = now;
                }
            }
        }
        if (vooc_accepted_ && vooc_.mode() != vooc::Mode::Vooc) {
            if (state_ != State::Active && vbus >= kVoocRaisedMv) state_ = State::Active;
            else if (state_ != State::Active && now - vooc_accept_ms_ >= kVoocRampMs) return abort(Fail::NoVoltage);
        }
        if (vooc_cmds_ == 0 && now - band_ms_ >= kVoocWakeListenMs) {
            if (vooc_tries_ >= kVoocTries) return abort(vooc_frames_ > 0 ? Fail::NoResponse : Fail::NeedsLoad);
            env_.set_dp(Drive::HiZ);        // rest: both lines idle, then another wake train
            env_.set_dm(Drive::HiZ);
            return go(Phase::VoocRest, kVoocRestMs);
        }
        if (now - band_ms_ >= kVoocSilenceMs) {
            // Silent after it talked: the source is lost only when VBUS went with it; at 5 V it just stopped answering.
            if (state_ == State::Active) return abort(vbus < link::kVbusPresentMv ? Fail::SourceLost : Fail::NoResponse);
            if (vooc_accepted_) return abort(Fail::NoVoltage);
            if (vooc_ident_ && vooc_svooc_ > 0) return abort(Fail::Detected);   // decoded ident + an SVOOC-only command
            return abort(vooc_frames_ > 0 ? Fail::NoResponse : Fail::NeedsLoad);
        }
        return;
    }
    case Phase::VoocRest:
        if (now < due_ms_) return;
        return vooc_wake();
    case Phase::UfcsOpen:
    case Phase::UfcsPing:
    case Phase::UfcsCaps:
    case Phase::UfcsRequest:
    case Phase::UfcsPower:
    case Phase::UfcsKeep:
        return ufcs_poll(now);
    case Phase::VivoOpen:
    case Phase::VivoSeq:
    case Phase::VivoRun:
        return vivo_poll(now);
    case Phase::AfcSettle:
        if (now < due_ms_) return;
        go(Phase::AfcSend, 0);
        [[fallthrough]];
    case Phase::AfcSend: {
        if (now < due_ms_) return;
        const std::uint8_t byte = *afc::afc_byte(target_mv_);
        std::uint8_t rx[1] = {0};
        if (sends_ == 0 && !afc_retried_) {
            prev_mv_ = std::max(prev_mv_, applied_mv_);
            applied_mv_ = target_mv_;
            change_ms_ = now;
        }
        const bool ok = env_.xfer_once({&byte, 1}, rx);
        env_.log_afc(++afc_n_, byte, ok ? int(rx[0]) : -1, env_.vbus_mv());
        if (ok) {
            ok_sends_++;
            acked_ = true;
        } else if (ok_sends_ == 0) {
            // No slave ping to the first exchange: one retry once D- has sat low for a while, then the source is not answering
            // (a QC front end follows the ping level on its own, see docs/CHARGER_QUIRKS.md qc-follows-ping-level).
            if (afc_retried_) return abort(Fail::NoResponse);
            afc_retried_ = true;
            due_ms_ = env_.now_ms() + kAfcRetryLowMs;
            return;
        }
        if (++sends_ < afc::kSends) {
            due_ms_ = env_.now_ms() + afc::kSendGapMs;
            return;
        }
        if (ok_sends_ == 0) return abort(Fail::NoResponse);
        band_ms_ = env_.now_ms();
        return go(Phase::Band, 0);
    }
    case Phase::Qc2Settle:
    case Phase::Band:
    case Phase::Qc3Loop:
        break;
    default:
        return;
    }

    // Continuous QC3: Active means inside the suite's pass band (-300 / +150 mV of the target, settled: no pulse still
    // on its way), not the wide 85..110 % band; a settled miss (e.g. 9.35 V for 9 V) gets a correction, not a timeout.
    bool band = in_band(target_mv_, vbus);
    bool rests_at_5v = false;
    if (phase_ == Phase::Qc3Loop) {
        if (now < due_ms_) return;
        const qc::Mode m = proto_ == Proto::Qc3 ? qc::Mode::Qc3 : qc::Mode::Qc3Plus;
        qc3_settle_.feed(now, vbus);
        // The charger slews behind the pulses (and its real step may differ from the nominal one): count what was
        // commanded but has not shown on VBUS yet, in the step size measured on this charger.
        // Movement is accumulated against the last consumed level, so a slow ramp (a few mV per poll) is not
        // mistaken for an idle charger; pending is only forgotten after kQc3StillMs without any movement.
        const int moved = vbus - qc3_seen_mv_;
        if (moved <= -kQc3MoveMv || moved >= kQc3MoveMv) {
            qc3_seen_mv_ = vbus;
            qc3_sent_ms_ = now;                          // still slewing: the quiet time restarts
            if (qc3_pending_mv_ > 0) qc3_pending_mv_ = std::max(0, qc3_pending_mv_ - moved);
            else if (qc3_pending_mv_ < 0) qc3_pending_mv_ = std::min(0, qc3_pending_mv_ - moved);
        }
        // Settled = the pulses have shown on VBUS and it has stopped moving (a stable run of samples), or nothing
        // has moved for kQc3StillMs, or the wait ran out. Until then nothing near the target is sent on top of it.
        if (qc3_cmd_n_ != 0) {
            const int dv = vbus - qc3_base_mv_;
            const int adv = dv < 0 ? -dv : dv;
            const int n = qc3_cmd_n_ < 0 ? -qc3_cmd_n_ : qc3_cmd_n_;
            const bool shown = (dv > 0) == (qc3_cmd_n_ > 0) && adv >= n * qc3_step_mv_ / 2;
            const bool quiet = now - qc3_sent_ms_ >= kQc3StillMs;
            // A slow slew can pass the sample window (0.7 V/s moves 42 mV in two sample gaps), so "stable" also needs
            // a quiet stretch with no step of kQc3MoveMv or more since the level was last consumed.
            const bool stable = shown && qc3_settle_.stable() && now - qc3_sent_ms_ >= kQc3StableQuietMs;
            // The wait is bounded: kQc3StillMs without movement settles it whatever the samples say, and the
            // kQc3TransitionMs limit ends the row.
            if (quiet || stable) {
                qc3_pending_mv_ = 0;                     // the rest is not coming: forget it
                if ((dv > 0) == (qc3_cmd_n_ > 0) && adv >= 4 * kQc3MoveMv) {   // measure the step it really made
                    const int nom = qc::step_mv(m);
                    // QC3+ pulses that move VBUS by QC3-sized steps (hardware 2026-11-08: ~180 mV per 40 us pulse, a QC3 charger
                    // that reads the pulses as its own): that is not QC3+. Ending here, on the first measured up batch (the
                    // probe, at most kQc3ProbePulses pulses), keeps the ramp from ever commanding volts of pulses at the
                    // learned (clamped) step. Reported as "no response": the runner maps it to the neutral "no QC3+".
                    if (m == qc::Mode::Qc3Plus && qc3_cmd_n_ >= kQc3CoarseMinPulses && adv / n > qc::kPlusCoarseStepMv) {
                        env_.log_qc3_coarse(adv / n);
                        return abort(Fail::NoResponse);
                    }
                    qc3_step_mv_ = std::clamp(adv / n, nom / 4, nom * 4);
                    qc3_learned_ = true;
                } else if (n >= kQc3ProbePulses) {
                    qc3_learned_ = true;                 // it did not move: keep the nominal step, the band timeout decides
                }
                // QC3+ pulses that leave VBUS at its start level, again and again: the adapter does not do QC3+.
                qc3_nomove_ = adv < 2 * kQc3MoveMv ? qc3_nomove_ + n : 0;
                if (m == qc::Mode::Qc3Plus && qc3_nomove_ >= kQc3NoResponsePulses && vbus - entry_mv_ < 150)
                    return abort(Fail::NoResponse);
                qc3_dead_ = qc3_cmd_n_ > 0 && adv < 2 * kQc3MoveMv && vbus >= kQc3ClassABandMv ? qc3_dead_ + 1 : 0;
                if (target_mv_ > kQc3ClassAMaxMv && qc3_dead_ >= kQc3DeadBatches) return abort(Fail::NoBand);
                qc3_base_mv_ = vbus;
                qc3_seen_mv_ = vbus;
                qc3_cmd_n_ = 0;
            }
        }
        // The 5 V row of a bank that rests above the pass band (hardware 2026-11-08: 5.176 V unloaded, 5.15 V is the band's
        // top) and ignores the down pulse: pulses that moved nothing, with VBUS inside vSafe5V, mean the charger is at its
        // 5 V output. That is done, not a reason to resend the pulse every 500 ms until the 12 s timeout. A bank that does
        // answer the pulse moves VBUS, which clears qc3_nomove_ and keeps the normal correction; VBUS outside vSafe5V is
        // never accepted here.
        rests_at_5v = target_mv_ == 5000 && qc3_cmd_n_ == 0 && qc3_nomove_ > 0 && pd::is_vsafe5(vbus);
        const int cap = m == qc::Mode::Qc3 ? kQc3BatchPulses : kQc3PlusBatchPulses;
        int total = rests_at_5v ? 0 : qc::plan_pulses(m, vbus + qc3_pending_mv_, target_mv_, qc3_step_mv_);
        const int mag_total = total < 0 ? -total : total;
        if (total != 0 && !qc3_learned_) {
            // First batch: a small probe, then wait until it has settled to learn the real step before the bulk.
            if (qc3_cmd_n_ != 0) total = 0;
            else total = total < 0 ? -std::min(mag_total, kQc3ProbePulses) : std::min(mag_total, kQc3ProbePulses);
        }
        // Within the last batch worth of the target, or when the direction flips, the next batch waits for the settle.
        if (total != 0 && qc3_cmd_n_ != 0 && (mag_total <= cap || (total < 0) != (qc3_cmd_n_ < 0))) total = 0;
        if (total != 0) {
            StepList<2 * kQc3PlusBatchPulses> list;
            const int sent = std::min(qc::batch_pulses(m, total, cap), cap);
            const int signed_sent = total < 0 ? -sent : sent;
            env_.log_qc3(vbus, qc3_pending_mv_, qc3_step_mv_, signed_sent);
            if (qc3_cmd_n_ == 0 && qc3_pending_mv_ == 0) qc3_base_mv_ = vbus;
            qc::pulse_train(list, m, total < 0 ? qc::Dir::Down : qc::Dir::Up, sent);
            env_.play(list.span());
            qc3_sent_ms_ = env_.now_ms();
            qc3_settle_.reset();
            qc3_pending_mv_ += signed_sent * qc3_step_mv_;
            qc3_cmd_n_ += signed_sent;
            due_ms_ = env_.now_ms() + kQc3GapMs;
            state_ = State::Negotiating;
            return;
        }
        due_ms_ = now + kQc3GapMs;
    }
    if (phase_ == Phase::Qc3Loop)
        band = qc3_cmd_n_ == 0 && (qc::in_pass_band(vbus, target_mv_) || rests_at_5v);

    if (band) {
        state_ = State::Active;
        lost_ms_ = 0;
        band_ms_ = now;
    } else if (state_ == State::Active) {
        if (lost_ms_ == 0) lost_ms_ = now;
        else if (now - lost_ms_ >= kLostMs) abort(Fail::SourceLost);
    } else if (now - band_ms_ >= (phase_ == Phase::Qc2Settle ? kQc2TransitionMs : kBandTimeoutMs) &&
               (phase_ == Phase::Qc2Settle || phase_ == Phase::Band || phase_ == Phase::Script)) {
        abort(Fail::NoBand);
    } else if (now - band_ms_ >= 12000) {
        abort(Fail::NoBand);
    }
}

}  // namespace k2::fc
