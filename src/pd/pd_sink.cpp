// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_sink.hpp"
#include "k2/core/strpack.hpp"

#include <algorithm>

#include "k2/core/hot.h"
#include "k2/core/log.hpp"
#include "k2/pd/pd_ext.hpp"
#include "k2/pd/pd_link.hpp"
#include "k2/pd/pd_reply.hpp"
#include "k2/pd/xiaomi_auth.hpp"

namespace k2::pd {

namespace {

// Bits 24 (No USB Suspend) and 25 (USB Communications Capable) are both set.
constexpr std::uint32_t kRdoNoSuspend = 3u << 24, kRdoEpr = 1u << 22;
constexpr std::uint32_t kAbortHardResetMs = 500;  // still above the cap this long after the 5 V request
constexpr std::uint32_t kTxRetryMs = 50;
constexpr std::uint16_t kReleaseMaxMa = 3000;

std::uint32_t pos_bits(std::uint8_t pos) { return std::uint32_t(pos & 0xF) << 28; }
void bump(std::uint16_t& n) { if (n != 0xFFFF) n++; }

// Build a VDM header word: SVID, structured bit, command type (0 REQ .. 3 BUSY) and command.
constexpr std::uint32_t build_vdm_header(std::uint16_t svid, bool structured, std::uint8_t cmd_type, std::uint8_t cmd) {
    return (std::uint32_t(svid) << 16) | (structured ? (1u << 15) : 0) | (std::uint32_t(cmd_type) << 6) | cmd;
}

// A Vendor_Defined SOP message out of the readout engine's words (one tx slot, like everything else).
static TxMsg vdm_txmsg(const AppleVdm& v, std::uint8_t rev) {
    TxMsg m;
    m.header = make_header(kMsgVendorDefined, v.n, rev);
    m.len = std::uint8_t(v.n * 4);
    for (unsigned i = 0; i < v.n; i++)
        for (int b = 0; b < 4; b++) m.data[4 * i + b] = std::uint8_t(v.w[i] >> (8 * b));
    return m;
}

// PDO1 (the 5 V fixed supply) of a PD 3.x source carries "EPR Mode Capable" in bit 23 (reserved in PD 2.0).
K2_HOT bool source_offers_epr(const SourceCaps& c) {
    return c.valid && c.spec_rev == 2 && c.num_pdos && c.pdos[0].type == PdoType::Fixed && (c.pdos[0].raw & (1u << 23));
}

// Xiaomi Mi Turbo Charge (SVID 0x2717) bite points:
// the sink OPENS the auth chain itself: it sends a 1-DO Vendor_Defined
// 0x27170101 right after the Discover-SVIDs exchange that showed SVID 0x2717, paced ~200 ms.
constexpr std::uint32_t kXiaomiOpenerVdo = 0x27170101;
constexpr std::uint32_t kXiaomiOpenerMs = 200;  // pacing between sends
// The proactive Discover-SVIDs REQ paces after PS_RDY (~200-250 ms), so the
// charger's own discovery runs first and the REQ cannot desync it (H4, trace5).
constexpr std::uint32_t kProactiveDiscoverMs = 220;

#if K2_OPPO_PROBE
// Header bit 5 of a SOP message is the Port Data Role (1 = DFP); a cable message (SOP') uses it as Cable Plug, a Hard Reset has none.
K2_HOT void with_role(TxMsg& m, bool dfp) {
    if (m.hard_reset || m.sop != Sop::Sop) return;
    m.header = std::uint16_t((m.header & ~0x20u) | (dfp ? 0x20u : 0u));
}
#endif

}  // namespace

const char* epr_fail_name(std::uint8_t code) {
    switch (code) {
        case 0: return "unknown cause";
        case 1: return "cable not EPR capable";
        case 2: return "source not VCONN source";
        case 3: return "EPR bit not set in RDO";
        case 4: return "source unable now";
        case 5: return "EPR bit not set in 5 V PDO";
    }
    return "invalid reason";
}

K2_HOT std::uint32_t rdo_fixed(std::uint8_t pos, std::uint16_t op_ma, std::uint16_t max_ma, bool epr) {
    return pos_bits(pos) | kRdoNoSuspend | (epr ? kRdoEpr : 0u) |
           (std::uint32_t(std::min<unsigned>(op_ma / 10u, 1023u)) << 10) | std::min<unsigned>(max_ma / 10u, 1023u);
}

K2_HOT std::uint32_t rdo_pps(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr) {
    return pos_bits(pos) | kRdoNoSuspend | (epr ? kRdoEpr : 0u) | (std::uint32_t((mv / 20u) & 0xFFF) << 9) |
           std::min<unsigned>(ma / 50u, 127u);
}

K2_HOT std::uint32_t rdo_avs(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr) {
    const unsigned units = (mv / 100u) * 4u;  // 25 mV units, 100 mV steps
    return pos_bits(pos) | kRdoNoSuspend | (epr ? kRdoEpr : 0u) | (std::uint32_t(units & 0xFFF) << 9) |
           std::min<unsigned>(ma / 50u, 127u);
}

const char* err_name(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::NoCaps: return "no source capabilities";
        case Err::BadPos: return "bad PDO position";
        case Err::OverCap: return "above the voltage cap";
        case Err::BadVoltage: return "bad voltage";
        case Err::Inline: return "live sink attached";
        case Err::Busy: return "busy";
        case Err::BadChoice: return "PDO advertises no current";
    }
    return "?";
}

const char* busy_why_name(BusyWhy w) {
    switch (w) {
        case BusyWhy::None: return "";
        case BusyWhy::Test: return "a charger test or tool owns the port";
        case BusyWhy::Legacy: return "legacy unit running (PD-silent)";
        case BusyWhy::Identify: return "identify running";
        case BusyWhy::Probe: return "probe running";
        case BusyWhy::Prepare: return "re-announcing the source caps first";
        case BusyWhy::Negotiating: return "negotiation in flight (previous request not settled)";
    }
    return "?";
}

const char* state_name(SinkState s) {
    switch (s) {
        case SinkState::Idle: return "idle";
        case SinkState::EprEntering: return K2_P("entering EPR");
        case SinkState::EprWaitCaps: return K2_P("reading EPR caps");
        case SinkState::Requesting: return K2_P("requesting");
        case SinkState::Transition: return K2_P("transition");
        case SinkState::Ready: return "ready";
        case SinkState::Aborting: return K2_P("back to 5V");
        case SinkState::Failed: return K2_P("failed");
    }
    return "?";
}

const char* result_name(SinkResult r) {
    switch (r) {
        case SinkResult::None: return "none";
        case SinkResult::Ok: return "ok";
        case SinkResult::Rejected: return "rejected";
        case SinkResult::WaitExhausted: return "source busy";
        case SinkResult::Timeout: return "timeout";
        case SinkResult::TxFailed: return "no GoodCRC";
        case SinkResult::HardReset: return "hard reset";
        case SinkResult::SoftReset: return "soft reset";
        case SinkResult::Aborted: return "dropped to 5V";
        case SinkResult::OverCap: return "over the cap";
        case SinkResult::EprFailed: return "EPR refused";
        case SinkResult::BadChoice: return "bad choice";
    }
    return "?";
}

K2_HOT std::uint32_t request_cap_mv(std::int32_t max_request_mv, std::int32_t max_vbus_mv) {
    if (max_request_mv <= 0 || max_vbus_mv <= 0) return kVsafe5vMv;
    return std::uint32_t(std::max<std::int32_t>(std::min(max_request_mv, max_vbus_mv), std::int32_t(kVsafe5vMv)));
}

K2_HOT Plan plan_request(const SourceCaps& caps, Choice c, std::uint32_t cap_mv, bool epr_mode) {
    Plan p;
    if (cap_mv < kVsafe5vMv) cap_mv = kVsafe5vMv;
    if (!caps.valid || caps.num_pdos == 0) return p;  // err = NoCaps
    // The K2 can enter EPR, so every RDO to a source that offers it says so: the source checks "EPR Mode Capable" in the most
    // recent RDO when the sink later sends EPR_Mode Enter, and answers Enter_Failed (data 3) when it is clear (Baseus 140 W,
    // hardware 2026-11-04: the START Request had the bit clear and EPR entry failed 6 ms after the Enter).
    epr_mode = epr_mode || source_offers_epr(caps);
    if (c.pos < 1 || c.pos > caps.num_pdos || c.pos > kMaxPdos) { p.err = Err::BadPos; return p; }
    const Pdo& d = caps.pdos[c.pos - 1];
    p.pos = c.pos;
    p.pdo = d.raw;
    p.type = d.type;
    p.needs_epr = c.pos >= 8;
    switch (d.type) {
        case PdoType::Fixed: {
            if (!fits_monitor(d.max_mv, cap_mv)) { p.err = Err::OverCap; return p; }
            p.mv = d.max_mv;
            p.ma = c.hunt ? std::min<unsigned>(c.ma ? c.ma : d.max_ma, 10230) : (c.ma && c.ma < d.max_ma ? c.ma : d.max_ma);
            p.rdo = rdo_fixed(c.pos, p.ma, p.ma, epr_mode);
            break;
        }
        case PdoType::Pps:
        case PdoType::Avs: {
            const std::uint32_t lo = d.min_mv, hi = std::min<std::uint32_t>(d.max_mv, cap_mv);
            if (hi < lo) { p.err = Err::OverCap; return p; }
            std::uint32_t mv = c.mv ? std::clamp<std::uint32_t>(c.mv, lo, hi) : hi;
            if (d.type == PdoType::Avs) {
                mv = mv / 100 * 100;  // 100 mV steps
                if (mv < lo) mv = (lo + 99) / 100 * 100;
            }
            std::uint32_t limit = current_limit_ma(d, static_cast<std::uint16_t>(mv));
            if (d.type == PdoType::Avs && c.pos >= 8 && d.max_mw && mv) {  // EPR AVS: power limited
                limit = std::min<std::uint32_t>(5000, d.max_mw * 1000u / mv);
            }
            p.mv = std::uint16_t(mv);
            p.ma = std::uint16_t(c.hunt ? std::min<unsigned>(c.ma ? c.ma : limit, 6350) : (c.ma && c.ma < limit ? c.ma : limit));
            p.rdo = d.type == PdoType::Pps ? rdo_pps(c.pos, p.mv, p.ma, epr_mode)
                                           : rdo_avs(c.pos, p.mv, p.ma, epr_mode || c.pos >= 8);
            break;
        }
        default:
            p.err = Err::BadPos;  // battery and variable supplies are not requested
            return p;
    }
    if (p.ma == 0 || p.mv == 0) { p.err = Err::BadChoice; return p; }  // never request 0 mA / 0 mV
    if (c.mismatch) p.rdo |= 1u << 26;   // Capability Mismatch (kRdoFMismatch): a probe of how the source treats the bit
    p.err = Err::Ok;
    return p;
}

K2_HOT Choice lookup_held(const SourceCaps& caps, std::uint16_t mv, std::uint16_t ma, PdoType type) {
    Choice c;
    if (!caps.valid || !mv) return c;
    const std::size_t n = std::min<std::size_t>(caps.num_pdos, caps.pdos.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Pdo& d = caps.pdos[i];
        if (type == PdoType::Fixed) {
            if (d.type == PdoType::Fixed && d.max_mv == mv) { c.pos = std::uint8_t(i + 1); c.mv = mv; c.ma = ma; return c; }
        } else if (type == PdoType::Pps || type == PdoType::Avs) {
            if (d.type == type && mv >= d.min_mv && mv <= d.max_mv) { c.pos = std::uint8_t(i + 1); c.mv = mv; c.ma = ma; return c; }
        }
    }
    return c;   // pos 0: the list no longer offers the held level
}

K2_HOT std::array<std::uint8_t, 24> make_skedb() {
    std::array<std::uint8_t, 24> s{};
    s[0] = 0x16; s[1] = 0x07;   // VID 0x0716
    s[2] = 0x60; s[3] = 0x50;   // PID 0x5060
    s[8] = 1; s[9] = 1;         // firmware / hardware version
    s[10] = 1;                  // SKEDB version
    s[17] = 0x23;               // Sink Modes: AVS (0x20), VBUS powered (0x02), PPS charging (0x01)
    s[18] = 1; s[19] = 15; s[20] = 33;   // SPR Sink min / operational / max PDP in watts; EPR PDPs (21..23) stay 0
    return s;
}

// ---- Sink ----

void Sink::update_rev() {
    if (caps_ && caps_->valid) rev_ = caps_->spec_rev == 2 ? 2 : 1;
}

// One out-of-line copy: the TxMsg copy inlined at its ~20 callers cost about 0.7 KB.
[[gnu::noinline]] K2_HOT void Sink::queue(const TxMsg& m) {
    tx_ = m;
    tx_pending_ = true;
#if K2_OPPO_PROBE
    tx_owner_ = TxOwner::Sink;   // the probe / DR_Swap paths name themselves after queueing
#endif
}

K2_HOT bool Sink::take_tx(TxMsg& out) {
    if (sink_cap_pending_ && !(tx_pending_ && tx_.hard_reset)) {   // tSenderResponse: the answer goes first
        sink_cap_pending_ = false;
        update_rev();
        out = TxMsg{};
        const std::uint32_t* words = override_pdos_ != nullptr ? override_pdos_ : kSinkCapPdos;
        const std::uint8_t nw = override_pdos_ != nullptr ? override_n_ : 2;
        out.header = make_header(kDataSinkCap, nw, rev_);
        out.len = std::uint8_t(4 * nw);
        for (int w = 0; w < nw; w++)
            for (int i = 0; i < 4; i++) out.data[4 * w + i] = std::uint8_t(words[w] >> (8 * i));
#if K2_OPPO_PROBE
        with_role(out, dfp_);
        last_tx_ = out;
        last_owner_ = TxOwner::Sink;
#else
        last_tx_ = out;
#endif
        return true;
    }
    if (sink_cap_ext_pending_ && !(tx_pending_ && tx_.hard_reset)) {
        sink_cap_ext_pending_ = false;
        update_rev();
        out = TxMsg{};
        if (sink_cap_ext_mode_ == SinkCapExtMode::Skedb) {
            out.header = make_header(kExtSinkCapsExt, 7, rev_, true);
            out.len = 28;
            out.data[0] = 0x18;   // extended header 0x8018: chunked, chunk 0, data size 24
            out.data[1] = 0x80;
            const auto s = make_skedb();
            for (std::size_t i = 0; i < s.size(); i++) out.data[2 + i] = s[i];   // bytes 26, 27 stay 0 (pad to the object)
        } else {
            out.header = make_header(kCtlNotSupported, 0, rev_);
        }
#if K2_OPPO_PROBE
        with_role(out, dfp_);
        last_tx_ = out;
        last_owner_ = TxOwner::Sink;
#else
        last_tx_ = out;
#endif
        return true;
    }
    if (ans_pending_ && !(tx_pending_ && tx_.hard_reset)) {   // the answer to a request: also tSenderResponse
        out = ans_;
        ans_pending_ = false;
#if K2_OPPO_PROBE
        with_role(out, dfp_);
        last_tx_ = out;
        last_owner_ = TxOwner::Sink;
#else
        last_tx_ = out;
#endif
        return true;
    }
    if (!tx_pending_) return false;
    out = tx_;
#if K2_OPPO_PROBE
    with_role(out, dfp_);
    last_tx_ = out;
    last_owner_ = tx_owner_;
#else
    last_tx_ = tx_;
#endif
    tx_pending_ = false;
    return true;
}

[[gnu::noinline]] K2_HOT void Sink::fail(SinkResult r, SinkState to) {
    st_.result = r;
    st_.held = false;
    st_.auto_hold = false;
    st_.state = to;
    retry_pending_ = false;
    retx_pending_ = false;
    deferred_ = Deferred::None;
    release_pending_ = false;   // whatever ended the attempt also ends the wish and the VBUS check that hung on it
    verify_ = false;
    st_.verifying = false;
}

// The attempt a release waited behind failed (Reject, Wait exhausted): the release is still wanted, so it survives fail().
// The refused level never becomes the contract's: plan_ is the last accepted contract again, so a re-announce is answered for
// it (before, the next Source_Capabilities / EPR_Source_Capabilities asked for the level the source had just refused).
K2_HOT void Sink::fail_keep_release(SinkResult r, SinkState to, std::uint32_t now) {
    const bool rp = release_pending_;
    if (contract_) plan_ = contract_plan_;
    fail(r, to);
    release_pending_ = rp;
    settle_release(now);
}

K2_HOT void Sink::do_hard_reset(SinkResult r, SinkState to, bool forced) {
    // A self-powered K2 lives on the source's VBUS: its own timeouts, aborts and over-cap guards end the attempt but never put
    // a Hard Reset on the wire (the source cuts VBUS for ~650 ms and the K2 dies). Only the explicit tool (`forced`) does.
    const bool wire = forced || !never_hard_reset_;
    TxMsg m;
    m.hard_reset = true;
    tx_pending_ = false;  // nothing else matters after a hard reset
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    ans_pending_ = false;
    get_cap_sent_ = false;
    refresh_pending_ = false;
    sr_wait_ = false;
    if (wire) {
        queue(m);
        st_.hard_resets++;
    }
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    ka_silence_ = KaSilence::Off;
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    source_asked_identity_ = false;
    xiaomi_adapter_id_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();   // the lab-only Apple readout belongs to the attach that just ended
#if K2_OPPO_PROBE
    link_reset(oppo::End::HardReset, wire);   // nothing on the wire (self-powered): the partner still has the roles we swapped
#endif
    fail(r, to);
}

void Sink::hard_reset(std::uint32_t) { do_hard_reset(SinkResult::HardReset, SinkState::Idle, true); }

K2_HOT void Sink::on_hard_reset_rx(std::uint32_t) {
    // Only a K2 that owned a contract or was negotiating one asks for it back. A passive K2 that never requested anything (the
    // idle Home page on a charger, a Hard Reset because a tester downstream started its own test, hardware 2026-11-07) has
    // nothing to resume: its next Request would present Rd, turn Auto-CRC on and take the line from the real sink.
    const bool owned = st_.held || contract_ ||
                       st_.state == SinkState::Requesting || st_.state == SinkState::Transition || st_.state == SinkState::Aborting ||
                       st_.state == SinkState::EprEntering || st_.state == SinkState::EprWaitCaps;
    st_.source_hard_resets++;   // apart from hard_resets (the K2's own): the charger test reads the sum as "the link was reset"
    tx_pending_ = false;
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    ans_pending_ = false;
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    ka_silence_ = KaSilence::Off;
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    source_asked_identity_ = false;
    xiaomi_adapter_id_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();
#if K2_OPPO_PROBE
    link_reset(oppo::End::HardReset, true);   // both ends are back to their default roles
#endif
    resume_ = resume_ || owned;   // a second Hard Reset right behind the first finds the sink already idle: keep the flag
    get_cap_sent_ = false;
    refresh_pending_ = false;
    sr_wait_ = false;
    rev_ = 2;   // headers stay PD3.x until the re-announced Source_Capabilities say otherwise (EPR needs PD3)
    fail(SinkResult::HardReset, SinkState::Idle);
}

void Sink::detach() {
    resume_ = false;
    get_cap_sent_ = false;
    refresh_pending_ = false;
    sr_wait_ = false;
    st_.epr_info = EprInfo::None;   // asked once per attach
    tx_pending_ = false;
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    ans_pending_ = false;
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    ka_silence_ = KaSilence::Off;
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    source_asked_identity_ = false;
    xiaomi_adapter_id_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();
    txok_.reset();   // a new source: it has not shown SinkTxOK yet
#if K2_OPPO_PROBE
    link_reset(oppo::End::Detach, true);
#endif
    fail(SinkResult::None, SinkState::Idle);
}

K2_HOT void Sink::soft_reset(std::uint32_t now) {
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    ans_pending_ = false;
    get_cap_sent_ = false;
    refresh_pending_ = false;
    TxMsg m;
    m.header = make_header(kCtlSoftReset, 0, rev_);
    m.restart_id = true;   // the MessageID counters restart on both sides
    queue(m);
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    ka_silence_ = KaSilence::Off;
    // The contract is gone: the Xiaomi opener and the SVID discovery belong to the next one
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    sr_wait_ = true;
    sr_at_ = now + 400;
    ap_.reset();
#if K2_OPPO_PROBE
    link_reset(oppo::End::SoftReset, false);   // a Soft Reset keeps the data role
#endif
    fail(SinkResult::SoftReset, SinkState::Idle);
}

bool Sink::xiaomi_chain_active(std::uint32_t now) const {
    if (!source_has_xiaomi_svid_ || !contract_) return false;
    // After 0x27170208 our Get_Source_Cap is out and the source re-announces: a Request before that answer would be for the old list.
    if (xiaomi_authed_) return get_cap_sent_ && now - chain_at_ < kXiaomiCapsWaitMs;
    if (now - chain_start_ >= kXiaomiChainBoundMs) return false;
    return xiaomi_opener_pending_ || now - chain_at_ < kXiaomiChainQuietMs;
}

void Sink::note_xiaomi_svid(std::uint32_t now) {
    if (!source_has_xiaomi_svid_) chain_start_ = now;
    source_has_xiaomi_svid_ = true;
    st_.xiaomi_svid = true;
    chain_at_ = now;
}

K2_HOT void Sink::send_request(std::uint32_t now, bool may_defer) {
    // A sink-initiated Request waits for the Xiaomi chain to end and for the source's SinkTxOK (tick() sends it when both clear; the
    // Accept timer starts then). Answers to the source's own messages and the release / abort paths call this without may_defer.
    const bool chain = may_defer && xiaomi_chain_active(now);
    if (chain || (may_defer && txok_.hold(rev_, now))) {
        deferred_ = Deferred::Request;
        if (chain) xm_.req_held = true;
        retry_pending_ = false;
        retx_pending_ = false;
        return;
    }
    deferred_ = Deferred::None;
    txok_.sent();
    // Diagnostics only (the charger test's hunt evidence): what this Request is and whether the chain was still running.
    xm_.req_held = false; ++xm_.req_n; xm_.req_at = now; xm_.answer = ReqAnswer::None; xm_.answer_at = 0;
    xm_.req_pdo = plan_.pdo; xm_.req_rdo = plan_.rdo; xm_.req_mv = plan_.mv; xm_.req_ma = plan_.ma; xm_.req_pos = plan_.pos;
    xm_.req_type = plan_.type; xm_.req_caps = caps_ ? caps_->num_pdos : std::uint8_t(0); xm_.req_in_chain = xiaomi_chain_active(now);
    TxMsg m;
    if (st_.epr_mode) {
        m.header = make_header(kDataEprRequest, 2, rev_);
        m.len = 8;
    } else {
        m.header = make_header(kDataRequest, 1, rev_);
        m.len = 4;
    }
    for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(plan_.rdo >> (8 * i));
    if (st_.epr_mode) for (int i = 0; i < 4; i++) m.data[4 + i] = std::uint8_t(plan_.pdo >> (8 * i));
    deadline_ = now + kAcceptMs;
    tx_retries_ = 0;
    retx_pending_ = false;
    queue(m);
}

void Sink::send_epr_enter(std::uint32_t now, bool may_defer) {
    if (may_defer && (xiaomi_chain_active(now) || txok_.hold(rev_, now))) {
        deferred_ = Deferred::EprEnter;
        st_.epr_fail = 0;
        st_.state = SinkState::EprEntering;
        return;
    }
    deferred_ = Deferred::None;
    txok_.sent();
    TxMsg m;
    m.header = make_header(kDataEprMode, 1, rev_);
    m.len = 4;
    m.data[2] = kEprSinkPdpW;
    m.data[3] = kEprEnter;
    st_.epr_fail = 0;
    deadline_ = now + kEprEnterMs;
    tx_retries_ = 0;
    queue(m);
    st_.state = SinkState::EprEntering;
}

void Sink::queue_epr_control(std::uint8_t kind) {  // Extended_Control: ext header (size 2) + type + data
    TxMsg m;
    m.header = make_header(kExtControl, 1, rev_, true);
    m.len = 4;
    m.data[0] = 2;
    m.data[1] = 0x80;  // Chunked: a source without unchunked extended messages rejects the plain form
    m.data[2] = kind;
    queue(m);
}

Err Sink::request_epr_info(std::uint32_t now, bool force) {
    if (!caps_ || !info_) return Err::NoCaps;
    if (!force && !source_offers_epr(*caps_)) return Err::BadChoice;
    if (st_.epr_info != EprInfo::None || st_.epr_mode || st_.state != SinkState::Ready || tx_pending_ || xiaomi_chain_active(now) ||
        ams_open())
        return Err::Busy;
    queue_epr_control(kEprGetSourceCap);
    st_.epr_info = EprInfo::Pending;
    info_deadline_ = now + kEprInfoMs;
    return Err::Ok;
}

K2_HOT Err Sink::request_source_caps(std::uint32_t now) {
    if (!caps_) return Err::NoCaps;
    // One negotiation in flight: Ready only (no Request, Accept wait or PS_RDY outstanding), nothing of ours queued or being
    // retransmitted, no release / abort / VBUS check hanging on a Request, no Xiaomi chain (it holds our own Request back too), and
    // not still inside the last ask's answer window. EPR mode: an unsolicited Source_Capabilities there is a Hard Reset.
    if (st_.state != SinkState::Ready || st_.epr_mode || !contract_ || tx_pending_ || retx_pending_ || release_pending_ || verify_ ||
        deferred_ != Deferred::None || sr_wait_ || get_cap_sent_ || st_.epr_info == EprInfo::Pending || xiaomi_chain_active(now) ||
        ams_open())
        return Err::Busy;
    if (txok_.hold(rev_, now)) return Err::Busy;   // a sink-initiated message: SinkTxNG (1.5 A) means the source asks us to wait
    txok_.sent();
#if K2_OPPO_PROBE
    queue_get_caps(now);
    return Err::Ok;
}

void Sink::queue_get_caps(std::uint32_t now) {
#endif
    TxMsg m;
    m.header = make_header(kCtlGetSourceCap, 0, rev_);
    queue(m);
    get_cap_sent_ = true;
    refresh_pending_ = true;
    refresh_deadline_ = now + kGetCapAnswerMs;
#if !K2_OPPO_PROBE
    return Err::Ok;
#endif
}

Err Sink::request_pps_status(std::uint32_t now) {
    if (!caps_) return Err::NoCaps;
    if (st_.state != SinkState::Ready || !contract_ || tx_pending_ || retx_pending_ || release_pending_ || verify_ ||
        deferred_ != Deferred::None || sr_wait_ || get_cap_sent_ || st_.epr_info == EprInfo::Pending || pps_.ask == PpsAsk::Pending ||
        xiaomi_chain_active(now) || ams_open())
        return Err::Busy;
    if (txok_.hold(rev_, now)) return Err::Busy;
    txok_.sent();
    const std::uint8_t seq = pps_.seq;
    pps_ = PpsAnswer{};   // before the queue: its TX_START stamp (on_tx_result) must survive
    pps_.seq = seq;
    pps_.ask = PpsAsk::Pending;
    TxMsg m;
    m.header = make_header(kCtlGetPpsStatus, 0, rev_);
    queue(m);
    pps_deadline_ = now + kPpsStatusAnswerMs;
    return Err::Ok;
}

Err Sink::arm_apple_read(std::uint32_t now) {
    // The remote gates this with apple_read_refusal() first; the sink double-checks the hard ones:
    // never transmit inline / to a source that is not our partner, and never without a contract.
    if (!answers_sop_) return Err::Inline;
    if (!contract_) return Err::NoCaps;
    if (st_.state != SinkState::Ready || tx_pending_ || retx_pending_ || xiaomi_chain_active(now)) return Err::Busy;
    ap_.arm(now);
    // Arming triggers a re-request of the CURRENT contract (Get_Source_Cap): the source re-announces
    // its caps and runs its 0x05AC discovery ~1.4 s later. Never a different voltage.
    const Err e = request_source_caps(now);
    if (e != Err::Ok) {
        ap_.reset();
        return e;
    }
    return Err::Ok;
}

Err Sink::trigger(Choice c, std::uint32_t now) {
    if (!caps_) return Err::NoCaps;
#if K2_OPPO_PROBE
    if (op_.running() || swap_ == DrSwap::Pending) return Err::Busy;   // the lab probe owns the contract until it ends
#endif
    switch (st_.state) {
        case SinkState::EprEntering: case SinkState::EprWaitCaps: case SinkState::Requesting:
        case SinkState::Transition: case SinkState::Aborting:
            return Err::Busy;
        default: break;
    }
    update_rev();
    const bool defer = c.pos == 0 || (c.pos >= 8 && !st_.epr_mode);  // EPR list needed first
    if (defer && !st_.epr_mode && hv_lock_) return Err::OverCap;    // no EPR entry under the 5 V lock
    if (!defer) {
        const Plan p = plan_for(c, st_.epr_mode);
        if (p.err != Err::Ok) return p.err;
        plan_ = p;
    } else if (!caps_->valid) {
        return Err::NoCaps;
    }
    want_ = c;
    st_.held = true;
    st_.auto_hold = false;   // somebody asked: no longer the driver's own answer (answer_5v sets it after this)
    ka_silence_ = KaSilence::Off;   // a new wish: nobody is timing the old contract's keep-alive any more
    st_.result = SinkResult::None;
    wait_retries_ = 0;
    verify_ = false;            // a new wish ends the check of the last release
    st_.verifying = false;
    if (defer) {
        if (st_.epr_mode) { proceed_after_epr_caps(now, false); return Err::Ok; }
        epr_caps_seen_ = false;
        send_epr_enter(now, true);
        return Err::Ok;
    }
    fresh_ = true;
    st_.state = SinkState::Requesting;
    send_request(now, true);
    return Err::Ok;
}

Err Sink::answer_5v(Choice c, std::uint32_t now) {
    const Err e = trigger(c, now);
    if (e == Err::Ok) st_.auto_hold = true;
    return e;
}

Plan Sink::plan_for(Choice c, bool epr_mode) const {
    Plan p = plan_request(*caps_, c, cap_mv_, epr_mode);
    if (hv_lock_ && p.err == Err::Ok && above_hv_lock(p)) p.err = Err::OverCap;   // the 5 V lock is a 5 V cap
    return p;
}

void Sink::set_hv_lock(bool on, std::uint32_t now) {
    const bool edge = on && !hv_lock_;
    hv_lock_ = on;
    if (!edge) return;
    // The lock edge: an EPR entry under way is dropped, a contract or a Request in flight above 5 V goes back to PDO 1 through
    // release() (queued behind a Request already on the wire, as any release is).
    const bool entering = st_.state == SinkState::EprEntering || st_.state == SinkState::EprWaitCaps;
    const bool high = above_hv_lock(plan_) || (contract_ && st_.mv > 5500);
    if (entering || (high && (contract_ || st_.state != SinkState::Idle))) release(now);
}

// `answer`: the last chunk of the source's EPR_Source_Capabilities is in (on_message). That message started the source's
// SenderResponseTimer (~30 ms) and only an EPR_Request stops it, so the Request goes out from this same pass: never held back
// (SinkTxOK, Xiaomi chain) and never left to the caller's next poll (Baseus FH22 140W, 2026-11-17: a list-only entry left
// the list unanswered while the charger test planned its next row, Hard Reset 31 ms after the last chunk). Without
// `answer` (trigger() while already in EPR mode) nothing is waiting for an answer: the Request is sink-initiated.
K2_HOT void Sink::proceed_after_epr_caps(std::uint32_t now, bool answer) {
    if (want_.pos == 0) {  // only the list was wanted
        st_.result = SinkResult::Ok;
        st_.held = false;
        st_.state = SinkState::Ready;
        next_epr_ka_ = now + kEprKeepAliveMs;
        st_.epr_ka_at = now;
        if (!answer) return;
        // The answer keeps what the contract holds: its level looked up in the EPR list, else PDO 1; Ready again on its PS_RDY.
        Choice hc{};
        if (contract_) hc = lookup_held(*caps_, contract_plan_.mv, contract_plan_.ma, contract_plan_.type);
        if (!hc.pos) hc = {1, 0, kReleaseMaxMa};
        Plan p = plan_for(hc, true);
        if (p.err != Err::Ok) p = plan_for({1, 0, kReleaseMaxMa}, true);   // the 5 V lock: PDO 1
        if (p.err != Err::Ok) return;
        plan_ = p;
        st_.state = SinkState::Requesting;
        send_request(now);
        return;
    }
    const Plan p = plan_for(want_, true);
    if (p.err != Err::Ok) {
        const SinkResult why = p.err == Err::OverCap ? SinkResult::OverCap : SinkResult::BadChoice;
        // The wanted level cannot be asked: the list is still answered, with PDO 1 (5 V), and the reason is reported on its PS_RDY.
        if (answer) { begin_abort(why, now); return; }
        fail(why, SinkState::Ready);
        next_epr_ka_ = now + kEprKeepAliveMs;
        st_.epr_ka_at = now;
        return;
    }
    plan_ = p;
    fresh_ = true;
    st_.state = SinkState::Requesting;
    send_request(now, !answer);
}

void Sink::begin_abort(SinkResult why, std::uint32_t now) {
    if (st_.state == SinkState::Aborting) return;
    abort_result_ = why;
    st_.held = false;
    retry_pending_ = false;
    release_pending_ = false;
    abort_at_ = now;
    st_.state = SinkState::Aborting;
    restart_abort(now);
}

// The Request for 5 V (PDO 1 of the list in force) of the abort in progress; also the answer to caps re-announced meanwhile.
void Sink::restart_abort(std::uint32_t now) {
    accepted_ = false;
    Plan p;
    if (caps_) p = plan_request(*caps_, {1, 0, kReleaseMaxMa}, cap_mv_, st_.epr_mode);
    if (p.err != Err::Ok) {  // no way to ask for 5 V
        do_hard_reset(abort_result_, SinkState::Failed);
        return;
    }
    plan_ = p;
    send_request(now);
}

// The one Request in flight is over (PS_RDY, Reject, Wait given up): a release that waited behind it goes out now.
void Sink::settle_release(std::uint32_t now) {
    if (!release_pending_) return;
    release_pending_ = false;
    if (st_.state != SinkState::Ready) return;
    if (contract_ && st_.pos == 1 && st_.mv <= kVsafe5vMv) { st_.held = false; return; }   // already at 5 V
    begin_abort(SinkResult::Ok, now);
}

// Verification of a release (see kVerifyRetries). Armed by the PS_RDY of the 5 V Request, only with a live VBUS feed.
void Sink::arm_verify(std::uint32_t now) {
    if (!vbus_seen_ || now - vbus_at_ > kVerifyFreshMs) { verify_ = false; st_.verifying = false; return; }
    if (!verify_) verify_tries_ = 0;   // a retry's PS_RDY keeps counting
    verify_ = true;
    st_.verifying = true;
    verify_at_ = now;
    vref_set_ = false;
}

K2_HOT void Sink::verify_vbus(std::uint32_t now) {
    if (now - vbus_at_ > kVerifyFreshMs) { verify_ = false; st_.verifying = false; return; }   // the feed stopped: cannot judge
    if (vbus_mv_ <= kVSafe5MaxMv) { verify_ = false; st_.verifying = false; return; }
    const std::uint32_t elapsed = now - verify_at_;
    const bool falling = vref_set_ && now - vref_at_ <= kStallMs && elapsed < kSettleMs + kDescentMs;
    if (elapsed < kSettleMs || falling) return;
    if (verify_tries_ < kVerifyRetries) {
        verify_tries_++;
        vref_set_ = false;
        begin_abort(abort_result_, now);   // verify_ stays set: the retry's PS_RDY starts the next window
        return;
    }
    do_hard_reset(SinkResult::Aborted, SinkState::Failed);   // bounded: a Hard Reset on the wire unless never_hard_reset
}

K2_HOT void Sink::release(std::uint32_t now) {
    resume_ = false;   // the user let go: a Hard Reset's pending "ask for 5 V again" goes with the hold
    if (st_.state == SinkState::Aborting) return;
    if (st_.state == SinkState::EprEntering || st_.state == SinkState::EprWaitCaps) {
        st_.held = false;  // no voltage change was made: just stop waiting
        st_.state = st_.epr_mode ? SinkState::Ready : SinkState::Idle;
        return;
    }
    if (!st_.held && !contract_ && st_.state == SinkState::Idle) return;  // already at 5 V
    // The Request in flight already asks for PDO 1 (5 V): that is where a release ends up. A second Request now would race
    // the source's Accept / PS_RDY for the first (UGREEN X865, hardware 2026-11-04: TX timeout, two Accept + PS_RDY pairs).
    // Just stop holding; the exchange finishes on its own and lands in Ready at 5 V.
    if ((st_.state == SinkState::Requesting || st_.state == SinkState::Transition) && plan_.pos == 1 && plan_.mv <= kVsafe5vMv) {
        st_.held = false;
        return;
    }
    if (!st_.held && contract_ && st_.pos == 1 && st_.state == SinkState::Ready) return;
    // Another Request is in flight (for a voltage above 5 V): the sink is the single gate, so the release is queued behind it
    // and goes out at its PS_RDY / Reject (settle_release). A second Request now would be sent before the first one's PS_RDY, the
    // source's Accept and PS_RDY for the first would be taken as the answer to the second, and the sink would believe in 5 V while
    // VBUS stays up (Anker splitter after its mid-run re-announce, hardware 2026-11-09, runs/anker-split/bb9.txt).
    // (A Request that is only waiting - the Wait retry timer, the Xiaomi chain guard - has not gone out: nothing to queue behind.)
    const bool in_flight = st_.state == SinkState::Transition ||
                           (st_.state == SinkState::Requesting && !retry_pending_ && deferred_ == Deferred::None);
    if (in_flight) {
        release_pending_ = true;
        return;
    }
    begin_abort(SinkResult::Ok, now);
}

K2_HOT void Sink::after_ps_rdy(std::uint32_t now) {
    const std::int32_t drop_mv = contract_ ? std::int32_t(st_.mv) - std::int32_t(plan_.mv) : 0;   // the step down, for the descent window
    contract_ = true;
    contract_plan_ = plan_;
    st_.pos = plan_.pos;
    st_.mv = plan_.mv;
    st_.ma = plan_.ma;
    st_.result = SinkResult::Ok;
    st_.state = SinkState::Ready;
    // Proactive Discover SVIDs once a contract exists: some chargers (CUKTECH 10, hardware
    // 2026-11-04) never send Discover SVIDs unless the sink starts VDM discovery. Harmless to
    // chargers that drive the flow themselves (they get an ACK, then send their auth VDMs).
    // Paced ~220 ms after PS_RDY (H4): the charger's own discovery runs first, ours
    // comes after the contract settled. Sent on SOP (the driver sends sink TX on SOP only).
    if (!discover_svids_sent_ && proactive_discover_ && gives_sink_cap_ && !source_has_xiaomi_svid_) {
        discover_svids_sent_ = true;   // armed and thus once per session
        discover_svids_pending_ = true;
        discover_svids_at_ = now + kProactiveDiscoverMs;
    } else if (discover_svids_pending_) {
        discover_svids_at_ = now + kProactiveDiscoverMs;   // a Request went in between: paced from this PS_RDY again
    }
    if (fresh_) {
        ready_at_ = now; fresh_ = false; tracking_ = false;
        descent_ms_ = descent_window_ms(drop_mv, plan_.type == PdoType::Pps || plan_.type == PdoType::Avs);
    }
    next_keepalive_ = now + kPpsKeepAliveMs;
    if (st_.epr_mode) { next_epr_ka_ = now + kEprKeepAliveMs; st_.epr_ka_at = now; }
    settle_release(now);   // a release that waited behind this Request goes out now (not for the abort's own PS_RDY)
#if K2_OPPO_PROBE
    if (op_.running()) op_.on_ready(now);   // the probe's closing Get_Source_Cap: its re-request is done
#endif
}

// The Chunk Request for the next chunk of the chunked extended message `header` / `payload` (its own type); false when that
// was the last chunk, a chunk request itself, or not chunked.
static bool next_chunk_request(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint8_t rev, TxMsg& m) {
    if (payload.size() < 2) return false;
    const ExtHeader eh = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (eh.request || !eh.chunked) return false;
    if ((unsigned(eh.chunk_number) + 1) * kChunkBytes >= eh.data_size) return false;  // that was the last chunk
    m = TxMsg{};
    m.header = make_header(std::uint8_t(header & 0x1F), 1, rev, true);
    // 4 B = 2 B extended header padded to the next 4-byte boundary, as required for a
    // chunked message with Data Size < 26 (USB PD R3.2 §6.5.1.1). The unpadded 2 B form was
    // tried and the Baseus 140 W stopped answering chunk requests, so keep the padded form.
    m.len = 4;
    const std::uint16_t req = std::uint16_t((1u << 15) | (1u << 10) | ((eh.chunk_number + 1u) << 11));
    m.data[0] = std::uint8_t(req);
    m.data[1] = std::uint8_t(req >> 8);
    return true;
}

// A re-announced list inside a negotiation or a contract (Source_Capabilities in SPR mode, the last chunk of
// EPR_Source_Capabilities in EPR mode) starts the source's SenderResponseTimer: answer it with a Request now.
K2_HOT void Sink::answer_reannounce(std::uint32_t now) {
    // Also while Requesting - the in-flight Request was for the old caps; without a fresh one the source's tSenderResponse
    // expires and it Hard Resets (CUKTECH C1 right after the Xiaomi auth unlock re-announces, hardware 2026-11-04, SPR mode).
    // In EPR mode the same holds for EPR_Source_Capabilities (USB PD R3.2: the sink answers them with an EPR_Request).
    if (release_pending_ && caps_ && (st_.state == SinkState::Requesting || st_.state == SinkState::Transition)) {
        // The runner already wants 5 V and its release waits behind a Request for the old list: answer the new list
        // with the release itself (PDO 1), one Request, instead of PDO N first and the release after it.
        begin_abort(SinkResult::Ok, now);
    } else if (st_.state == SinkState::Aborting && caps_) {
        restart_abort(now);   // the 5 V Request in flight was for the old list: the source wants the answer to the new one
    } else if (st_.held && (st_.state == SinkState::Ready || st_.state == SinkState::Requesting ||
                            st_.state == SinkState::Transition) && caps_) {
        // A re-announce while held: answer with a Request for the HELD LEVEL looked up in the new list by
        // voltage (fixed) or range (programmable) - never by position (review 2026-11-09: a list that moved
        // the held PDO must not change what we hold). Found -> its new slot and current word, the hold stands.
        // Not found -> the hold is lost: ask PDO 1 (5 V) and report it (begin_abort: held off, Aborted).
        // In Transition the source restarts negotiation: the fresh list is answered the same way, with a
        // Request inside tSenderResponse (with none the source Hard Resets).
        // Also the answer to our own Refresh (request_source_caps): a Source_Capabilities, asked for or not,
        // starts the source's tSenderResponse. An unchanged list plans the same RDO: Accept + PS_RDY, no change.
        Choice hc = lookup_held(*caps_, plan_.mv, plan_.ma, plan_.type);
        if (hc.pos) {
            const Plan p = plan_for(hc, st_.epr_mode);   // refused under the 5 V lock too: the hold is lost, PDO 1
            if (p.err != Err::Ok) { begin_abort(SinkResult::Aborted, now); return; }
            plan_ = p;
            st_.state = SinkState::Requesting;
            send_request(now);
        } else {
            begin_abort(SinkResult::Aborted, now);   // the Request for PDO 1, and the hold reported lost
        }
    } else if (!st_.held && st_.state == SinkState::Ready && contract_ && caps_) {
        // Re-announce with a contract but no trigger (the 5 V contract after a release, caps repeated after
        // our CC rescan): stay silent and a source such as the C135 sends a Hard Reset 28 ms later. Ask for
        // the contract's own level looked up by voltage, else PDO 1.
        Choice hc = lookup_held(*caps_, plan_.mv, plan_.ma, plan_.type);
        if (!hc.pos) hc = {1, 0, kReleaseMaxMa};
        Plan p = plan_for(hc, st_.epr_mode);
        if (p.err != Err::Ok && hv_lock_) p = plan_for({1, 0, kReleaseMaxMa}, st_.epr_mode);   // the 5 V lock: PDO 1
        if (p.err != Err::Ok) return;
        plan_ = p;
        st_.state = SinkState::Requesting;
        send_request(now);
    }
}

K2_HOT bool Sink::request_chunk(std::uint16_t header, std::span<const std::uint8_t> payload) {
    TxMsg m;
    if (!next_chunk_request(header, payload, rev_, m)) return false;
    queue(m);
    return true;
}

// An extended message with no handler above: EPR_Get_Sink_Cap gets the sink's EPR_Sink_Capabilities, a request the K2 does
// not support gets Not_Supported once its last chunk is in (the rest is asked for first: USB PD R3.2 6.12.2 chunking), a chunk
// request aimed at us is ignored (every extended message of ours is one chunk), replies get nothing (pd_reply.hpp).
void Sink::on_unhandled_ext(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (!answers_requests() || payload.size() < 2) return;
    update_rev();
    const ExtHeader eh = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (eh.request) return;
    const std::uint8_t type = header & 0x1F;
    const std::uint8_t ecdb = payload.size() > 2 ? payload[2] : 0;
    if (type == kExtControl && ecdb == kEprGetSinkCap && eh.chunk_number == 0) { answer_epr_sink_caps(); return; }
    const std::uint8_t refusal = refusal_for(header, ecdb, rev_);
    if (refusal == 0) return;
    TxMsg m;
    if (next_chunk_request(header, payload, rev_, m)) { answer(m); return; }   // refused after its last chunk
    answer_ctl(refusal);
}

void Sink::answer_ctl(std::uint8_t type) {
    if (type == 0) return;
    TxMsg m;
    m.header = make_header(type, 0, rev_);
    answer(m);
}

void Sink::answer_vdm(std::uint32_t word) {
    TxMsg m;
    m.header = make_header(kMsgVendorDefined, 1, rev_);
    m.len = 4;
    for (int b = 0; b < 4; b++) m.data[b] = std::uint8_t(word >> (8 * b));
    answer(m);
}

// Get_Revision (PD 3.1 and later): the Revision data message, one RMDO. The K2 speaks USB PD Revision 3.2, Version 1.0
// (bits 31..28 revision major, 27..24 minor, 23..20 version major, 19..16 minor).
void Sink::answer_revision() {
    TxMsg m;
    m.header = make_header(kDataRevision, 1, rev_);
    m.len = 4;
    const std::uint32_t rmdo = kSinkRmdo;
    for (int b = 0; b < 4; b++) m.data[b] = std::uint8_t(rmdo >> (8 * b));
    answer(m);
}

// EPR_Get_Sink_Cap: EPR_Sink_Capabilities, one chunk (extended header + the sink's PDOs, the same words Get_Sink_Cap gets,
// at most 6 so the message stays one chunk).
void Sink::answer_epr_sink_caps() {
    const std::uint32_t* words = override_pdos_ != nullptr ? override_pdos_ : kSinkCapPdos;
    std::uint8_t nw = override_pdos_ != nullptr ? override_n_ : 2;
    if (nw > 6) nw = 6;
    TxMsg m;
    const std::uint16_t eh = std::uint16_t((1u << 15) | (4u * nw));   // chunked, chunk 0, data size
    m.data[0] = std::uint8_t(eh);
    m.data[1] = std::uint8_t(eh >> 8);
    for (int w = 0; w < nw; w++)
        for (int i = 0; i < 4; i++) m.data[2 + 4 * w + i] = std::uint8_t(words[w] >> (8 * i));
    const std::uint8_t objs = std::uint8_t((2 + 4 * nw + 3) / 4);
    m.header = make_header(kExtEprSinkCaps, objs, rev_, true);
    m.len = std::uint8_t(4 * objs);
    answer(m);
}

K2_HOT void Sink::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    const std::uint32_t edge_us = stamp_us_;   // this frame's RX edge, if the driver had one
    stamp_us_ = 0;
    if (sop != Sop::Sop) return;
#if K2_OPPO_PROBE
    last_io_ = now;   // the line was busy: the lab probe keeps its quiet gap from here
#endif
    const std::uint8_t type = header & 0x1F;
    const unsigned num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;

    // Handle Vendor Defined Messages (VDM) - Xiaomi Mi Turbo Charge auth (SVID 0x2717)
    if (!ext && type == kMsgVendorDefined && num_do > 0) {
        if (answers_sop_) on_vdm(header, payload, now);   // else: another sink's conversation, never answered
        return;
    }

    if (ext) {
        if (type == kExtPpsStatusMsg) {   // the answer to our Get_PPS_Status: extended header (2) + PPSSDB (4), one chunk
            if (pps_.ask == PpsAsk::Pending && payload.size() >= 6) {
                pps_.mv_raw = std::uint16_t(payload[2] | payload[3] << 8);
                pps_.ma_raw = payload[4];
                pps_.flags = payload[5];
                pps_.rx_us = edge_us;
                pps_.ask = PpsAsk::Got;
                ++pps_.seq;
            }
            return;
        }
        if (type != kExtEprSourceCaps) { on_unhandled_ext(header, payload); return; }
        if (st_.epr_info == EprInfo::Pending && !st_.epr_mode) {
            // The answer to our SPR-mode EPR_Get_Source_Cap: informational (USB PD R3.2 6.5.17.1), never answered with a
            // Request. Fetch the remaining chunks; the engine decoded them into the bound info list.
            request_chunk(header, payload);
            if (info_ && info_->epr && !info_->epr_partial) st_.epr_info = EprInfo::Got;
            return;
        }
        epr_caps_seen_ = true;
        if (request_chunk(header, payload)) return;   // more chunks to come
        if (st_.state == SinkState::EprWaitCaps && !tx_pending_ && caps_ && caps_->epr && !caps_->epr_partial) {
            proceed_after_epr_caps(now, true);
        } else if (st_.epr_mode && st_.state != SinkState::EprWaitCaps && caps_ && caps_->epr && !caps_->epr_partial) {
            // The EPR re-announce (the source re-sends its list in EPR mode, e.g. after a keep-alive): the same answer as the SPR
            // re-announce, an EPR_Request now. Nothing answered it before: the source's timer ran on until something else of
            // ours went out (streamA6 2026-11-09: the runner's release, 14.8 ms after the last chunk).
            answer_reannounce(now);
        }
        return;
    }

    if (num_do > 0) {
        if (type == kDataSourceCap) {
            update_rev();
            const bool asked_sr = sr_wait_;
            sr_wait_ = false;   // the caps after our Soft_Reset arrived
            if (xiaomi_authed_ && !xm_.reannounced) { xm_.reannounced = true; xm_.reannounce_at = now; }
            const bool solicited = get_cap_sent_;
#if K2_OPPO_PROBE
            // A source that restarts with its caps has ended our DR_Swap AMS: the Accept to come is the Request's.
            if (swap_ == DrSwap::Pending) end_swap(DrSwap::Timeout, now);
            if (op_.wants_caps()) oppo_caps_rx(payload, num_do);   // the lab probe's closing Get_Source_Cap: keep its list
#endif
            if (contract_ && !solicited && !asked_sr) bump(st_.caps_unsolicited);   // a re-announce inside a contract nobody asked for
            get_cap_sent_ = false;
            refresh_pending_ = false;   // an answer to our Refresh (request_source_caps) ends its wait window
            if (st_.epr_mode && !solicited) {
                // USB PD R3.2 7.30.5: in EPR mode the source only sends EPR_Source_Capabilities, and a Source_Capabilities
                // that is not the answer to our Get_Source_Cap makes the SINK initiate a Hard Reset. The re-announce
                // answers below are SPR-mode behaviour (the CUKTECH C1 has no EPR bit). Re-request once the source is back.
                do_hard_reset(SinkResult::HardReset, SinkState::Idle);
                resume_ = true;
                return;
            }
            answer_reannounce(now);
        } else if (type == kDataEprMode && payload.size() >= 4) {
            const std::uint8_t action = payload[3];
            if (action == kEprExit) { st_.epr_mode = false; return; }
            if (st_.state != SinkState::EprEntering) return;
            if (action == kEprAck) {
                deadline_ = now + kEprEnterMs;
            } else if (action == kEprSucceeded) {
                st_.epr_mode = true;
                st_.state = SinkState::EprWaitCaps;
                deadline_ = now + kEprCapsMs;
                get_cap_at_ = now + 400;
                epr_get_sent_ = false;
                epr_caps_seen_ = false;
            } else if (action == kEprFailed) {
                st_.epr_fail = payload[2];
                fail(SinkResult::EprFailed, SinkState::Idle);
            }
        } else if (answers_requests()) {   // Get_Country_Info, Enter_USB, a reserved type: refused; the rest needs no answer
            update_rev();
            answer_ctl(refusal_for(header, 0, rev_));
        }
        return;
    }

    switch (type) {
        case kCtlAccept:
#if K2_OPPO_PROBE
            if (swap_ == DrSwap::Pending) { end_swap(DrSwap::Accepted, now); break; }   // our DR_Swap's answer (one AMS at a time)
#endif
            if (st_.state == SinkState::Requesting) {
                xm_.answer = ReqAnswer::Accept; xm_.answer_at = now;
                st_.accept_at = now; st_.ps_rdy_at = 0; st_.ps_rdy_inferred = false;
                st_.accept_us = edge_us; st_.ps_rdy_us = 0;
                st_.state = SinkState::Transition;
                down_set_ = false;   // a new step: the band dwell starts empty
                deadline_ = now + (plan_.mv && plan_.mv < st_.mv ? kPsRdyDownMs : kPsRdyMs);
            } else if (st_.state == SinkState::Aborting && !accepted_) {
                accepted_ = true;
                deadline_ = now + (plan_.mv && plan_.mv < st_.mv ? kPsRdyDownMs : kPsRdyMs);
            }
            break;
        case kCtlPsRdy:
            if (st_.state == SinkState::Transition) {
                st_.ps_rdy_at = now;
                st_.ps_rdy_us = edge_us;
                after_ps_rdy(now);
            } else if (st_.state == SinkState::Aborting && accepted_) {
                after_ps_rdy(now);  // plan_ holds PDO 1
                st_.held = false;
                st_.result = abort_result_;
                arm_verify(now);   // PS_RDY is a promise, VBUS is the proof
                if (st_.epr_mode) {
                    TxMsg exit;
                    exit.header = make_header(kDataEprMode, 1, rev_);
                    exit.len = 4;
                    exit.data[3] = kEprExit;
                    queue(exit);
                    st_.epr_mode = false;
                }
            }
            break;
        case kCtlNotSupported:
#if K2_OPPO_PROBE
            if (swap_ == DrSwap::Pending) { end_swap(DrSwap::NotSupported, now); break; }
            if (op_.on_not_supported(now)) break;   // the answer to the lab probe's VDM
#endif
            if (pps_.ask == PpsAsk::Pending) {
                pps_.rx_us = edge_us;
                pps_.ask = PpsAsk::NotSupported;
                ++pps_.seq;
                break;
            }
            if (st_.epr_info == EprInfo::Pending && st_.state == SinkState::Ready) st_.epr_info = EprInfo::Refused;
            break;
        case kCtlReject:
#if K2_OPPO_PROBE
            if (swap_ == DrSwap::Pending) { end_swap(DrSwap::Rejected, now); break; }   // no Request was refused: not counted
#endif
            bump(st_.rejects_rx);
            if (st_.epr_info == EprInfo::Pending && st_.state == SinkState::Ready) {
                st_.epr_info = EprInfo::Refused;
            } else if (st_.state == SinkState::Requesting) {
                xm_.answer = ReqAnswer::Reject; xm_.answer_at = now;
                fail_keep_release(SinkResult::Rejected, SinkState::Ready, now);
            } else if (st_.state == SinkState::Aborting) {
                do_hard_reset(abort_result_, SinkState::Failed);
            }
            break;
        case kCtlWait:
#if K2_OPPO_PROBE
            if (swap_ == DrSwap::Pending) { end_swap(DrSwap::Wait, now); break; }
#endif
            bump(st_.waits_rx);
            if (st_.state == SinkState::Requesting) {
                xm_.answer = ReqAnswer::Wait; xm_.answer_at = now;
                if (wait_retries_ >= kMaxWaitRetries) {
                    fail_keep_release(SinkResult::WaitExhausted, SinkState::Ready, now);
                } else {
                    wait_retries_++;
                    retry_pending_ = true;
                    retry_at_ = now + kWaitRetryMs;
                }
            } else if (st_.state == SinkState::Aborting) {
                do_hard_reset(abort_result_, SinkState::Failed);
            }
            break;
        case kCtlGetSinkCap:
            sink_cap_pending_ = gives_sink_cap_;   // answered by take_tx, ahead of anything queued
            break;
        case kCtlGetSinkCapExt:
            sink_cap_ext_pending_ = gives_sink_cap_ && sink_cap_ext_mode_ != SinkCapExtMode::Silent;
            break;
        case kCtlDrSwap:
        case kCtlPrSwap:
        case kCtlFrSwap: {
            if (!gives_sink_cap_) break;   // inline / PD-silent: not addressed to us, nothing goes on the wire
            const SwapKind k = type == kCtlDrSwap ? SwapKind::Dr : type == kCtlPrSwap ? SwapKind::Pr : SwapKind::Fr;
            std::uint8_t& n = swaps_rx_[std::size_t(k)];
            if (n < 255) n++;
            update_rev();
            answer_ctl(swap_reply(k, rev_));
            break;
        }
        case kCtlSoftReset: {
            bump(st_.soft_resets_rx);
            sink_cap_pending_ = false;
            sink_cap_ext_pending_ = false;
            ans_pending_ = false;
            get_cap_sent_ = false;
            refresh_pending_ = false;
            sr_wait_ = false;
            if (answers_sop_) {   // a far sink's Soft_Reset is not ours to Accept
                TxMsg m;
                m.header = make_header(kCtlAccept, 0, rev_);
                m.restart_id = true;
                queue(m);
            }
            st_.epr_mode = false;
            contract_ = false;
            st_.pos = 0;
            // The contract is gone: the opener belongs to the next one
            xiaomi_opener_pending_ = false;
            xiaomi_opener_sent_ = false;
            discover_svids_pending_ = false;
#if K2_OPPO_PROBE
            link_reset(oppo::End::SoftReset, false);   // the data role stays (USB PD: a Soft Reset changes no role)
#endif
            fail(SinkResult::SoftReset, SinkState::Idle);
            break;
        }
        case kCtlGetRevision:   // PD 3.1 and later; a PD 2.0 port does not define the type and ignores it
            update_rev();
            if (answers_requests() && rev_ >= 2) answer_revision();
            break;
        default:   // every other control message: refused when it is a request the K2 does not support (pd_reply.hpp)
            if (answers_requests()) { update_rev(); answer_ctl(refusal_for(header, 0, rev_)); }
            break;
    }
}

K2_HOT void Sink::on_tx_result(bool ok, std::uint32_t now) {
    const std::uint32_t start_us = stamp_us_;   // TX_START of the message this result is for
    stamp_us_ = 0;
    if (last_tx_.hard_reset) return;
#if K2_OPPO_PROBE
    last_io_ = now;
    const TxOwner owner = last_owner_;
    last_owner_ = TxOwner::Sink;
    if (owner == TxOwner::Swap) {   // our DR_Swap: no GoodCRC ends it at once, a GoodCRC leaves it waiting for the answer
        if (!ok && swap_ == DrSwap::Pending) end_swap(DrSwap::TxFail, now);
        return;
    }
    if (owner == TxOwner::Probe) { op_.on_tx(ok, now); return; }
#endif
    const std::uint8_t type = last_tx_.header & 0x1F;
    const bool ext = (last_tx_.header >> 15) & 1;
    if (ok) {
        tx_retries_ = 0;
        if (!ext && (type == kDataRequest || type == kDataEprRequest)) { st_.req_us = start_us; st_.accept_us = 0; st_.ps_rdy_us = 0; }
        if (!ext && type == kCtlGetPpsStatus && last_tx_.len == 0) pps_.ask_us = start_us;
        return;
    }
    const bool critical = !ext && (type == kDataRequest || type == kDataEprRequest || type == kDataEprMode);
    if (!critical) return;  // keep-alives and chunk requests are simply sent again by their timers
    if (tx_retries_ < kMaxTxRetries) {
        tx_retries_++;
        retx_pending_ = true;
        retx_at_ = now + kTxRetryMs;
        return;
    }
    if (st_.state == SinkState::Aborting) do_hard_reset(abort_result_, SinkState::Failed);
    else fail(SinkResult::TxFailed, st_.epr_mode || contract_ ? SinkState::Ready : SinkState::Idle);
}

void Sink::set_vbus_external(bool on, std::uint32_t now) {
    if (on == vbus_external_) return;
    vbus_external_ = on;
    if (on) return;
    // The owner lets go while VBUS may still be above the contract and falling: nothing is judged against samples from before
    // (descent / release-check / down-step tracking restart), and the settle window runs from now with the longest descent window
    // (a charger leaving a high-voltage mode drains an unloaded output slowly), so the return to the contract is not an abort.
    tracking_ = false;
    vref_set_ = false;
    down_set_ = false;
    verify_ = false;
    st_.verifying = false;
    ready_at_ = now;
    descent_ms_ = kDescentMaxMs;
    next_keepalive_ = now + kPpsKeepAliveMs;
    if (st_.epr_mode) next_epr_ka_ = now + kEprKeepAliveMs;
}

K2_HOT void Sink::on_vbus(std::int32_t mv, std::uint32_t now) {
    vbus_mv_ = mv; vbus_at_ = now; vbus_seen_ = true;
    // An external VBUS owner (set_vbus_external): the sample is kept, nothing is judged. This also skips the owner-covered
    // over-cap abort: the owner (the SuperVOOC trigger) has its own, tighter 12 V ceiling and drops the session itself.
    if (vbus_external_) return;
    if (verify_ && st_.state == SinkState::Ready) {   // the descent after a release: progress is a sample >= kFallStepMv below the last best
        if (!vref_set_ || mv + kFallStepMv <= vref_mv_) { vref_mv_ = mv; vref_at_ = now; vref_set_ = true; }
        if (mv <= kVSafe5MaxMv) { verify_ = false; st_.verifying = false; }
    }
    const bool active = st_.held || st_.state == SinkState::Requesting || st_.state == SinkState::Transition ||
                        st_.state == SinkState::Aborting;
    if (!active) return;
    if (mv > std::int32_t(overcap_abort_mv(cap_mv_))) {   // the monitor ceiling + 5 %: a source's tolerance above its top level is no fault
        if (st_.state == SinkState::Aborting) {
            if (now - abort_at_ >= kAbortHardResetMs) do_hard_reset(SinkResult::OverCap, SinkState::Failed);
        } else {
            begin_abort(SinkResult::OverCap, now);
        }
        return;
    }
    // Pattern no-psrdy-on-downstep: an unloaded programmable source Accepts a lower voltage, drains toward it and never
    // sends PS_RDY (Lenovo C135, hardware 2026-11-03). Once VBUS has settled at the target (steady inside the band for
    // kDownSettleMs, never one sample) the step happened: take the contract as ready instead of letting the timeout Hard Reset a
    // source that does not come back from it.
    if (st_.state == SinkState::Transition && plan_.mv && plan_.mv < st_.mv &&
        (plan_.type == PdoType::Pps || plan_.type == PdoType::Avs)) {
        if (mv < programmable_floor_mv(plan_.mv) || mv > programmable_ceiling_mv(plan_.mv)) down_set_ = false;
        else if (!down_set_ || mv < down_ref_mv_ - kRiseMv || mv > down_ref_mv_ + kRiseMv) { down_ref_mv_ = mv; down_at_ = now; down_set_ = true; }
        else if (now - down_at_ >= kDownSettleMs) { down_set_ = false; st_.ps_rdy_inferred = true; after_ps_rdy(now); return; }
    }
    if (st_.state != SinkState::Ready || !st_.held || !contract_) return;
    // Track the descent from the first sample after PS_RDY, so a fall that began inside the settle time counts.
    if (!tracking_ || mv + kFallStepMv <= ref_mv_) { ref_mv_ = mv; ref_at_ = now; tracking_ = true; }
    const bool rising = mv > ref_mv_ + kRiseMv;
    const std::int32_t limit = plan_.type == PdoType::Pps || plan_.type == PdoType::Avs ?
        programmable_ceiling_mv(plan_.mv) : std::int32_t(plan_.mv) + std::int32_t(plan_.mv) / 10;
    if (now - ready_at_ < kSettleMs || mv <= limit) return;
    const bool falling = !rising && now - ref_at_ <= kStallMs && now - ready_at_ < kSettleMs + descent_ms_;
    if (!falling) begin_abort(SinkResult::Aborted, now);
}

K2_HOT void Sink::tick(std::uint32_t now) {
    if (retx_pending_ && now >= retx_at_ && !tx_pending_) {
        retx_pending_ = false;
        queue(last_tx_);
        return;
    }
#if K2_OPPO_PROBE
    if (swap_ == DrSwap::Pending && std::int32_t(now - swap_deadline_) >= 0) end_swap(DrSwap::Timeout, now);   // a silent partner
    if (op_.running() && oppo_tick(now)) return;
#endif
    // Sink-initiated Xiaomi chain opener: fire after the pacing delay once the SVIDs exchange
    // showed 0x2717 and a contract is live (the VDM conversation dies with the contract).
#if K2_OPPO_PROBE
    if (xiaomi_opener_pending_ && !tx_pending_ && contract_ && !ams_open() && std::int32_t(now - xiaomi_opener_at_) >= 0) {
#else
    if (xiaomi_opener_pending_ && !tx_pending_ && contract_ && std::int32_t(now - xiaomi_opener_at_) >= 0) {
#endif
        xiaomi_opener_pending_ = false;
        xiaomi_opener_sent_ = true;
        xm_.opener_sent = true;
        const std::array<std::uint32_t, 1> vdos = {kXiaomiOpenerVdo};
        // 1-DO Vendor_Defined on SOP (TxMsg.sop defaults there; the driver sends sink messages on SOP only)
        send_xiaomi_vdm_reply(0, std::span<const std::uint32_t>(vdos), now);
        return;
    }
    // The paced proactive Discover-SVIDs REQ (on SOP, like every sink TX). Only in Ready: between a Request's Accept and its
    // PS_RDY the source is in its power transition and answers any message with a Hard Reset (Anker IQ4, hardware 2026-11-14).
#if K2_OPPO_PROBE
    if (discover_svids_pending_ && !tx_pending_ && st_.state == SinkState::Ready && contract_ && !ams_open() &&
        std::int32_t(now - discover_svids_at_) >= 0) {
#else
    if (discover_svids_pending_ && !tx_pending_ && st_.state == SinkState::Ready && contract_ &&
        std::int32_t(now - discover_svids_at_) >= 0) {
#endif
        discover_svids_pending_ = false;
        TxMsg m;
        m.header = make_header(kMsgVendorDefined, 1, rev_);
        m.len = 4;
        const std::uint32_t h = build_vdm_header(0xFF00, true, 0, 2);  // Discover SVIDs REQ
        for (int b = 0; b < 4; b++) m.data[b] = std::uint8_t(h >> (8 * b));
        queue(m);
        return;
    }
    // Lab-only Apple readout: its paced reads / retries go through the same single tx gate as everything else.
#if K2_OPPO_PROBE
    if (ap_.armed() && !ams_open()) {
#else
    if (ap_.armed()) {
#endif
        const AppleVdm tx = ap_.tick(now);
        if (tx.n && !tx_pending_) {
            queue(vdm_txmsg(tx, rev_));
            return;
        }
    }
    auto due = [now](std::uint32_t t) { return std::int32_t(now - t) >= 0; };
    if (sr_wait_ && !tx_pending_ && due(sr_at_)) {   // the source is silent after our Soft_Reset: ask for its caps once
        sr_wait_ = false;
        TxMsg m;
        m.header = make_header(kCtlGetSourceCap, 0, rev_);
        queue(m);
        get_cap_sent_ = true;
        refresh_pending_ = true;   // the same answer window as a Refresh: soft_reset_pending() ends with it
        refresh_deadline_ = now + kGetCapAnswerMs;
        return;
    }
    if (refresh_pending_ && due(refresh_deadline_)) {   // a silent source: the answer window of our Refresh is over
        refresh_pending_ = false;
        get_cap_sent_ = false;
    }
    if (st_.epr_info == EprInfo::Pending && due(info_deadline_)) st_.epr_info = EprInfo::Timeout;
    if (pps_.ask == PpsAsk::Pending && due(pps_deadline_)) { pps_.ask = PpsAsk::Timeout; ++pps_.seq; }
    if (deferred_ != Deferred::None) {   // a Request / EPR entry held back by the Xiaomi chain guard
        const bool stale = deferred_ == Deferred::Request ? st_.state != SinkState::Requesting : st_.state != SinkState::EprEntering;
        if (stale) {
            deferred_ = Deferred::None;
            xm_.req_held = false;
        } else {
            // The state timers (Accept, EPR enter) start when the message goes out: nothing below runs meanwhile.
            if (!tx_pending_ && !xiaomi_chain_active(now) && !txok_.hold(rev_, now)) {
                const Deferred d = deferred_;
                deferred_ = Deferred::None;
                if (d == Deferred::Request) send_request(now);
                else send_epr_enter(now);
            }
            return;
        }
    }
    switch (st_.state) {
        case SinkState::EprEntering:
            if (due(deadline_)) fail(SinkResult::Timeout, SinkState::Idle);
            break;
        case SinkState::EprWaitCaps:
            if (!epr_get_sent_ && !epr_caps_seen_ && due(get_cap_at_) && !tx_pending_) {
                epr_get_sent_ = true;
                queue_epr_control(kEprGetSourceCap);
            }
            if (due(deadline_)) fail(SinkResult::Timeout, SinkState::Ready);
            break;
        case SinkState::Requesting:
            if (retry_pending_) {
                // A new sink-initiated Request: it waits for SinkTxOK and for the Xiaomi chain like a first one.
                if (due(retry_at_) && !tx_pending_) { retry_pending_ = false; send_request(now, true); }
            } else if (!retx_pending_ && !tx_pending_ && due(deadline_)) {
                do_hard_reset(SinkResult::Timeout, SinkState::Failed);
            }
            break;
        case SinkState::Transition:
            if (due(deadline_)) do_hard_reset(SinkResult::Timeout, SinkState::Failed);
            break;
        case SinkState::Aborting:
            if (!retx_pending_ && !tx_pending_ && due(deadline_)) do_hard_reset(abort_result_, SinkState::Failed);
            break;
        case SinkState::Ready:
#if K2_OPPO_PROBE
            if (tx_pending_ || vbus_external_ || ams_open()) break;   // an external VBUS owner / a lab AMS: nothing of our own
#else
            if (tx_pending_ || vbus_external_) break;   // an external VBUS owner: no verify, no keep-alive, nothing of our own
#endif
            if (verify_) { verify_vbus(now); break; }
            if (ka_silence_ != KaSilence::Off && due(ka_silence_until_)) ka_silence_ = KaSilence::Off;   // the bound
            if (st_.held && contract_ && (plan_.type == PdoType::Pps || plan_.type == PdoType::Avs) &&
                due(next_keepalive_) && ka_silence_ != KaSilence::Pps) {
                st_.state = SinkState::Requesting;  // the same Request again, answered with Accept + PS_RDY
                send_request(now, true);
            } else if (st_.epr_mode && due(next_epr_ka_) && ka_silence_ != KaSilence::Epr) {
                next_epr_ka_ = now + kEprKeepAliveMs;
                st_.epr_ka_at = now;
                queue_epr_control(kEprKeepAlive);
            }
            break;
        default: break;
    }
}

// ---- Xiaomi Mi Turbo Charge authentication (SVID 0x2717) ----

K2_HOT void Sink::on_vdm(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    (void)now;  // may be unused
    const unsigned num_do = (header >> 12) & 7;
    if (payload.size() < 4 || num_do == 0) return;
    note_xiaomi_exchange(now);   // any VDM once the source listed 0x2717 is the chain talking (the chain guard counts from it)

    // Parse first VDO (header)
    const std::uint32_t vdm_hdr = std::uint32_t(payload[0]) | (std::uint32_t(payload[1]) << 8) |
                                  (std::uint32_t(payload[2]) << 16) | (std::uint32_t(payload[3]) << 24);
    const std::uint16_t svid = vdm_hdr >> 16;
    const bool structured = (vdm_hdr >> 15) & 1;
    const std::uint8_t cmd_type = (vdm_hdr >> 6) & 3;
    const std::uint8_t cmd = vdm_hdr & 0x1F;

#if K2_OPPO_PROBE
    // The lab OPPO probe's answers (SVID 0x22D9, structured ACK / NAK / BUSY or unstructured): recorded, never refused - also a late
    // one after its step timed out (an unstructured VDM nobody owns would get Not_Supported below).
    if (svid == oppo::kSvid && op_.engaged()) {
        oppo_vdm_rx(payload, num_do, now);
        return;
    }
#endif

    // Only handle SOP VDMs (SOP' and SOP'' are for cables)

    // Discover Identity REQ from the source (0xFF008001): answer with our identity ACK
    // (0xFF008041: structured, ACK, cmd 1). The charger gates its whole VDM chain on this -
    // without an ACK it never sends Discover SVIDs and the Xiaomi auth never starts
    // (hardware 2026-11-04, CUKTECH 10 on C1: identity REQ seen, no ACK, chain dead).
    // Identity says "Xiaomi sink" (VID 0x2717, UFP AMA): hardware 2026-11-04 shows the
    // CUKTECH answers our Discover SVIDs with [0x2717] and keeps the auth chain dormant while
    // the sink identity is a foreign VID (0x0716); Mi Turbo Charge targets Xiaomi devices.
    if (svid == 0xFF00 && structured && cmd_type == 0 && cmd == 1) {
        source_asked_identity_ = true;   // our ACK's product VDO becomes the auth digest's adapter_id
        TxMsg m;
        m.header = make_header(kMsgVendorDefined, 4, rev_);
        m.len = 16;
        // Identity ACK: the VDM header with bit 13 set, the Xiaomi ID header VDO, Cert Stat 0 and the
        // product VDO (kXiaomiProductVdo). The constants live in pd_sink.hpp.
        const std::uint32_t vdos[4] = {kXiaomiIdentityAckHeader,
                                       kXiaomiIdHeaderVdo,
                                       0x00000000u,
                                       kXiaomiProductVdo};
        for (int w = 0; w < 4; w++)
            for (int b = 0; b < 4; b++) m.data[4 * w + b] = std::uint8_t(vdos[w] >> (8 * b));
        queue(m);
        return;
    }

    // Discover SVIDs REQ from the source (0xFF008002): reply with 0x2717 - this is what opens
    // the Xiaomi auth gate on chargers that drive the flow themselves. The charger asks, we
    // answer - and the opener below is armed like after an ACK, because the charger-driven
    // traces (6/7) went silent exactly here, both sides waiting.
    // The REQ is a question about US, not evidence about the source: a plain laptop brick asks it
    // too (Lenovo 65 W, hardware 2026-11-08: asks, never lists 0x2717, never runs a chain). So it
    // does NOT call note_xiaomi_svid(): source_has_xiaomi_svid_ / st_.xiaomi_svid (card hunt rows,
    // "chain unfinished", the chain guard that holds Requests) mean "the SOURCE is Xiaomi-family"
    // and are set only by the source listing 0x2717 in its own SVIDs ACK (below) or by its first
    // 0x2717 VDM (further down). The opener still goes out: a source-driven Xiaomi charger needs it
    // before it says anything with 0x2717, and for a foreign source it is one ignored message.
    if (svid == 0xFF00 && structured && cmd_type == 0 && cmd == 2) {
        if (contract_ && !xiaomi_opener_sent_) { xiaomi_opener_pending_ = true; xiaomi_opener_at_ = now + kXiaomiOpenerMs; }
        // Unarmed the SVIDs ACK stays exactly [0x2717] (the Xiaomi chain is untouched); armed by the
        // lab-only apple-read op it lists [0x2717, 0x05AC] packed in one word.
        const std::array<std::uint32_t, 2> vdos = {build_vdm_header(0xFF00, true, 1, 2), ap_.svid_ack_word()};
        send_xiaomi_vdm_reply(0, std::span<const std::uint32_t>(vdos), now);
        return;
    }

    // Discover SVIDs ACK from the source (answering our own Discover SVIDs): note 0x2717
    if (svid == 0xFF00 && structured && cmd_type == 1 && cmd == 2) {
        bool xiaomi_listed = false;
        for (std::size_t i = 1; i < num_do && i * 4 + 3 < payload.size(); i++) {
            const std::uint32_t svid_vdo = std::uint32_t(payload[i * 4 + 0]) | (std::uint32_t(payload[i * 4 + 1]) << 8) |
                                           (std::uint32_t(payload[i * 4 + 2]) << 16) | (std::uint32_t(payload[i * 4 + 3]) << 24);
            // OPPO's SVID is only recorded (a passive note: nothing is sent to it). A word carries two SVIDs.
            if (svid_vdo >> 16 == 0x22D9 || (svid_vdo & 0xFFFFu) == 0x22D9) st_.oppo_svid = true;
            if (svid_vdo >> 16 == 0x2717 && !xiaomi_listed) {
                xiaomi_listed = true;   // the first listing acts once; the scan goes on for the OPPO note
                note_xiaomi_svid(now);
                // The sink opens the auth chain (the CUKTECH C1 deadlock in
                // trace5 was both sides waiting for exactly this): arm the paced opener once.
                if (contract_ && !xiaomi_opener_sent_) { xiaomi_opener_pending_ = true; xiaomi_opener_at_ = now + kXiaomiOpenerMs; }
            }
        }
        return;
    }

    // Lab-only Apple readout (SVID 0x05AC), armed by the remote `apple-read` op only: while armed the
    // source's 0x05AC discovery and read answers are routed to the engine (apple_readout.cpp owns the
    // state machine), its replies go out through the same single tx gate. Unarmed 0x05AC VDMs fall
    // through and are ignored, exactly like any other foreign SVID.
    if (svid == kAppleSvid && ap_.armed()) {
        AppleVdm rx;
        const unsigned n = num_do < rx.w.size() ? num_do : unsigned(rx.w.size());
        unsigned got = 0;
        for (unsigned i = 0; i < n && i * 4 + 3 < payload.size(); i++) {
            rx.w[i] = std::uint32_t(payload[i * 4]) | (std::uint32_t(payload[i * 4 + 1]) << 8) |
                      (std::uint32_t(payload[i * 4 + 2]) << 16) | (std::uint32_t(payload[i * 4 + 3]) << 24);
            got++;
        }
        rx.n = std::uint8_t(got);
        const AppleVdm tx = ap_.on_vdm(rx, now);
        if (tx.n && !tx_pending_) queue(vdm_txmsg(tx, rev_));
        return;
    }

    // Every structured request the cases above did not take is NAKed (USB PD R3.2 6.4.4.2), never left to the source's
    // timer: Discover Modes for an SVID we do not list or have no modes for, Enter / Exit Mode, SVID-specific commands,
    // reserved commands (hardware 2026-11-15: 23 of 25 Discover Modes for SVID 0x05AC went unanswered). Attention and the
    // source's ACK / NAK / BUSY get nothing (pd_reply.hpp vdm_nak_for).
    if (structured) {
        if (const std::uint32_t nak = vdm_nak_for(vdm_hdr); nak != 0 && answers_requests()) { update_rev(); answer_vdm(nak); }
        return;
    }

    // Only Xiaomi unstructured VDMs from here on; an unstructured 0x2717 REQ from the source is itself the evidence that it is
    // Xiaomi-family (the source-driven flow never lists 0x2717 in an ACK). An unstructured VDM nobody here owns: PD 3
    // Not_Supported (USB PD R3.2 6.4.4.1), PD 2.0 ignored.
    if (svid != 0x2717) {
        if (answers_requests()) { update_rev(); if (rev_ >= 2) answer_ctl(kCtlNotSupported); }
        return;
    }

    // Xiaomi commands: 0x27170201 through 0x2717020A
    // cmd field is 1..8, 0x0A
    if (cmd_type != 0) return;  // only REQ from source
    if (!source_has_xiaomi_svid_) note_xiaomi_svid(now);
    xm_.last_step = std::uint8_t(cmd);

    switch (cmd) {
        case 1: {
            // H3, step 1: RX 0x27170201 -> TX kXiaomiStep1ReplyVdo (0x27170102 runs the full chain, 0x27170106
            // does not unlock).
            const std::array<std::uint32_t, 1> vdos = {kXiaomiStep1ReplyVdo};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 2: {
            // RX 0x27170202 -> TX 0x27170103
            TxMsg m;
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            m.data[0] = 0x03; m.data[1] = 0x01; m.data[2] = 0x17; m.data[3] = 0x27;
            queue(m);
            break;
        }
        case 3: {
            // RX 0x27170203 -> TX 5-DO: 0x27170104 + the active session seed (the key table lives in
            // the gitignored priv/, referenced here by variable name only)
            const std::uint32_t* seed = mi::kSessionSeed[mi::kActiveKeyIndex];
            const std::array<std::uint32_t, 5> vdos = {0x27170104u, seed[0], seed[1], seed[2], seed[3]};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 4: {
            // RX 0x27170204 -> TX 5-DO: 0x27170105 + the fixed challenge (the digest's nonce)
            const std::array<std::uint32_t, 5> vdos = {0x27170105u, mi::kChallenge[0], mi::kChallenge[1],
                                                       mi::kChallenge[2], mi::kChallenge[3]};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 5: {
            // RX 0x27170205 (the charger's HMAC[0..15]) -> TX 0x27170106 VERIFIED.
            // The digest's adapter_id is our identity ACK's product VDO when the source asked for our
            // identity, 0 when it did not (a source that never sends Discover Identity REQ computes the
            // HMAC with adapter_id 0). Pick the candidate whose digest matches the charger's answer.
            const std::uint32_t ack_id = kXiaomiProductVdo & 0xFFFFu;
            const std::uint32_t primary = source_asked_identity_ ? ack_id : 0u;
            const std::uint32_t candidates[3] = {primary, 0u, ack_id};
            std::uint32_t rx[4] = {0, 0, 0, 0};
            const bool have_rx = num_do >= 5 && payload.size() >= 20;
            if (have_rx)
                for (unsigned w = 0; w < 4; w++)
                    rx[w] = std::uint32_t(payload[4 + 4 * w]) | (std::uint32_t(payload[4 + 4 * w + 1]) << 8) |
                            (std::uint32_t(payload[4 + 4 * w + 2]) << 16) | (std::uint32_t(payload[4 + 4 * w + 3]) << 24);
            std::uint8_t digest[32];
            std::uint32_t chosen = primary;
            bool matched = false;
            for (unsigned c = 0; c < 3 && !matched; c++) {
                bool dup = false;
                for (unsigned p = 0; p < c; p++) dup = dup || candidates[c] == candidates[p];
                if (dup) continue;
                mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, candidates[c], digest);
                matched = have_rx && mi::digest_word(digest, 0) == rx[0] && mi::digest_word(digest, 1) == rx[1] &&
                          mi::digest_word(digest, 2) == rx[2] && mi::digest_word(digest, 3) == rx[3];
                if (matched) chosen = candidates[c];
            }
            if (!matched) mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, primary, digest);
            xiaomi_adapter_id_ = chosen;
            K2LOG("[MI] 0205 %s adapter=0x%04X rx=%08X %08X %08X %08X\n", matched ? "match" : "no-match",
                  unsigned(chosen), unsigned(rx[0]), unsigned(rx[1]), unsigned(rx[2]), unsigned(rx[3]));
            // VERIFIED is 1: the observed chargers gate the unlock on the 0x27170108 half, not on
            // this word, and the known-good flow always sent 1.
            const std::array<std::uint32_t, 2> vdos = {0x27170106u, 0x00000001u};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 6: {
            // RX 0x27170206 -> TX 2-DO: 0x27170107, 0x00000001
            constexpr std::array<std::uint32_t, 2> vdos = {0x27170107u, 0x00000001u};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 7: {
            // RX 0x27170207 -> TX 5-DO: 0x27170108 + HMAC[16..31] for the adapter_id chosen at
            // 0x27170205 (the reverse-auth half of the same digest the charger answered with)
            std::uint8_t digest[32];
            mi::auth_digest(mi::kActiveKeyIndex, mi::kChallenge, xiaomi_adapter_id_, digest);
            const std::array<std::uint32_t, 5> vdos = {0x27170108u, mi::digest_word(digest + 16, 0),
                                                       mi::digest_word(digest + 16, 1), mi::digest_word(digest + 16, 2),
                                                       mi::digest_word(digest + 16, 3)};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 8: {
            // RX 0x27170208, the chain's last step. Two shapes observed on the wire:
            //  - payload 1 (Baseus FH22 family): the chain is done here - mark authed and fetch the
            //    re-announced caps, nothing else. No 0x2717010A: the FH22 2C1A answers it Not_Supported
            //    even after a verified 0208, and sent behind this Get_Source_Cap (before its
            //    Source_Capabilities) it broke that AMS and drew a Hard Reset (hardware 2026-11-17).
            //  - payload 0 (CUKTECH 120 W, the hardware-verified 6 A unlock): the charger wants our
            //    0x2717010A and answers 0x2717020A, which then finishes the auth (case 0x0A below).
            std::uint32_t w8 = 0;
            if (num_do >= 2 && payload.size() >= 8)
                w8 = std::uint32_t(payload[4]) | (std::uint32_t(payload[5]) << 8) |
                     (std::uint32_t(payload[6]) << 16) | (std::uint32_t(payload[7]) << 24);
            if (w8 == 1) {
                xiaomi_authed_ = true;
                st_.xiaomi_authed = true;
                xiaomi_cmd_ = cmd;
                xm_.authed = true; xm_.authed_at = now; xm_.reannounced = false; xm_.reannounce_at = 0;
                note_xiaomi_finish(0x08);
                TxMsg m;
                m.header = make_header(kCtlGetSourceCap, 0, rev_);
                queue(m);
                get_cap_sent_ = true;
            } else {
                const std::array<std::uint32_t, 2> vdos = {0x2717010Au, 0x00000001u};
                send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            }
            break;
        }
        case 0x0A: {
            // RX 0x2717020A (the CUKTECH-style finish after our 0x2717010A): mark authed and fetch
            // the re-announced caps. Idempotent with a 0x27170208-payload-1 finish: the authed chain
            // already asked for the caps, a second Get_Source_Cap could land inside that AMS.
            if (!xiaomi_authed_) {
                xiaomi_authed_ = true;
                st_.xiaomi_authed = true;
                xiaomi_cmd_ = cmd;
                xm_.authed = true; xm_.authed_at = now; xm_.reannounced = false; xm_.reannounce_at = 0;
                note_xiaomi_finish(0x0A);
                TxMsg m;
                m.header = make_header(kCtlGetSourceCap, 0, rev_);
                queue(m);
                get_cap_sent_ = true;
            }
            break;
        }
        default:
            break;
    }
}

// The chain finished at `step`: keep the SPR list in force now, the offer before the source's re-announce (diagnostics and the
// charger test's unlock diff only; nothing in the sink reads it back).
void Sink::note_xiaomi_finish(std::uint8_t step) {
    xm_.finish = step;
    xm_.pre = {};
    xm_.n_pre = 0;
    if (!caps_ || !caps_->valid) return;
    const std::size_t n = std::min<std::size_t>(std::min<std::size_t>(caps_->num_pdos, caps_->pdos.size()), xm_.pre.size());
    for (std::size_t i = 0; i < n; ++i) xm_.pre[i] = caps_->pdos[i].raw;
    xm_.n_pre = std::uint8_t(n);
}

void Sink::send_xiaomi_vdm_reply(std::uint8_t cmd, std::span<const std::uint32_t> vdos, std::uint32_t now) {
    (void)cmd;
    note_xiaomi_exchange(now);
    TxMsg m;
    m.header = make_header(kMsgVendorDefined, std::uint8_t(vdos.size()), rev_);
    m.len = std::uint8_t(vdos.size() * 4);
    for (std::size_t i = 0; i < vdos.size(); i++) {
        const std::uint32_t v = vdos[i];
        m.data[i * 4 + 0] = std::uint8_t(v);
        m.data[i * 4 + 1] = std::uint8_t(v >> 8);
        m.data[i * 4 + 2] = std::uint8_t(v >> 16);
        m.data[i * 4 + 3] = std::uint8_t(v >> 24);
    }
    queue(m);
}

#if K2_OPPO_PROBE
// ---- Sink-initiated DR_Swap and the OPPO (SVID 0x22D9) lab probe ----

namespace {
// A received message's data objects as words, at most 7.
std::size_t objs_to_words(std::span<const std::uint8_t> payload, unsigned num_do, std::array<std::uint32_t, 7>& w) {
    const std::size_t n = std::min({std::size_t(num_do), w.size(), payload.size() / 4});
    for (std::size_t i = 0; i < n; i++)
        w[i] = std::uint32_t(payload[4 * i]) | std::uint32_t(payload[4 * i + 1]) << 8 | std::uint32_t(payload[4 * i + 2]) << 16 |
               std::uint32_t(payload[4 * i + 3]) << 24;
    return n;
}
}  // namespace

[[gnu::noinline]] void Sink::oppo_caps_rx(std::span<const std::uint8_t> payload, unsigned num_do) {
    std::array<std::uint32_t, 7> w{};
    op_.on_caps({w.data(), objs_to_words(payload, num_do, w)});
}

// The probe's answers (SVID 0x22D9, structured ACK / NAK / BUSY or unstructured): recorded, never refused - also a late one after
// its step timed out (an unstructured VDM nobody owns would get Not_Supported).
[[gnu::noinline]] void Sink::oppo_vdm_rx(std::span<const std::uint8_t> payload, unsigned num_do, std::uint32_t now) {
    std::array<std::uint32_t, 7> w{};
    op_.on_vdm({w.data(), objs_to_words(payload, num_do, w)}, now);
}

Err Sink::request_dr_swap(bool to_dfp, std::uint32_t now) {
    if (!answers_requests()) return Err::Inline;   // never transmit for another sink's link
    if (!caps_ || !contract_) return Err::NoCaps;
    if (to_dfp == dfp_) return Err::BadChoice;
    if (st_.state != SinkState::Ready || tx_pending_ || retx_pending_ || ams_open() || deferred_ != Deferred::None || sr_wait_ ||
        get_cap_sent_ || release_pending_ || verify_ || st_.epr_info == EprInfo::Pending || pps_.ask == PpsAsk::Pending ||
        xiaomi_chain_active(now))
        return Err::Busy;
    if (txok_.hold(rev_, now)) return Err::Busy;
    txok_.sent();
    queue_dr_swap(to_dfp, now);
    return Err::Ok;
}

void Sink::queue_dr_swap(bool to_dfp, std::uint32_t now) {
    TxMsg m;
    m.header = make_header(kCtlDrSwap, 0, rev_);
    queue(m);
    tx_owner_ = TxOwner::Swap;
    swap_ = DrSwap::Pending;
    swap_to_ = to_dfp;
    swap_deadline_ = now + kDrSwapMs;
}

// The DR_Swap AMS is over. An Accept takes the asked role at once (the initiator changes its role on the Accept; the chip already
// sent that Accept's GoodCRC in the old role, the driver writes the new one before the next frame). Cold, never inlined into the
// hot receive path (as the other probe helpers below).
[[gnu::noinline]] void Sink::end_swap(DrSwap r, std::uint32_t now) {
    swap_ = r;
    if (r == DrSwap::Accepted) dfp_ = swap_to_;
    K2LOG("[PD] DR_Swap answer=%u dfp=%u\n", unsigned(r), unsigned(dfp_));
    if (!op_.running()) return;
    oppo::Outcome o = oppo::Outcome::Timeout;
    switch (r) {
        case DrSwap::Accepted: o = oppo::Outcome::Accept; break;
        case DrSwap::Rejected: o = oppo::Outcome::Reject; break;
        case DrSwap::Wait: o = oppo::Outcome::Wait; break;
        case DrSwap::NotSupported: o = oppo::Outcome::NotSupported; break;
        case DrSwap::TxFail: o = oppo::Outcome::TxFail; break;
        default: break;
    }
    op_.on_swap(o, dfp_, now);
}

// A Hard Reset (either way) or a detach puts both ends back in their default roles (UFP for a sink); a Soft_Reset keeps them.
// to_ufp is false for our own Hard Reset that never reached the wire (self-powered K2): the partner still holds the roles.
[[gnu::noinline]] void Sink::link_reset(oppo::End why, bool to_ufp) {
    swap_ = DrSwap::None;
    if (to_ufp) dfp_ = false;
    op_.stop(why, dfp_);
}

Err Sink::oppo_start(const oppo::Config& c, std::uint32_t now) {
    if (!answers_requests()) return Err::Inline;
    if (op_.running() || ap_.armed() || swap_ == DrSwap::Pending) return Err::Busy;
    if (!caps_ || !contract_ || st_.state != SinkState::Ready) return Err::NoCaps;
    if (st_.epr_mode || st_.mv > kVsafe5vMv) return Err::BadVoltage;
    std::array<std::uint32_t, 7> w{};
    const std::size_t n = std::min<std::size_t>(caps_->num_pdos, w.size());
    for (std::size_t i = 0; i < n; i++) w[i] = caps_->pdos[i].raw;
    op_.start(c, {w.data(), n}, dfp_, now);
    K2LOG("[OPPO] start steps=0x%X skip_swap=%u dfp=%u\n", unsigned(c.steps), unsigned(c.skip_swap), unsigned(dfp_));
    return Err::Ok;
}

void Sink::oppo_stop(std::uint32_t) {
    if (op_.running()) op_.stop(oppo::End::Stopped, dfp_);
}

// No AMS of anybody open and nothing of ours queued: the conditions request_source_caps() waits for, plus the answer slots, the paced
// sink messages, a DR_Swap, and the quiet gap since the last frame either way.
bool Sink::oppo_line_idle(std::uint32_t now) const {
    return st_.state == SinkState::Ready && contract_ && !st_.epr_mode && !tx_pending_ && !retx_pending_ && !ans_pending_ &&
           !sink_cap_pending_ && !sink_cap_ext_pending_ && deferred_ == Deferred::None && !sr_wait_ && !get_cap_sent_ &&
           !refresh_pending_ && !release_pending_ && !verify_ && !discover_svids_pending_ && !xiaomi_opener_pending_ &&
           st_.epr_info != EprInfo::Pending && pps_.ask != PpsAsk::Pending && swap_ != DrSwap::Pending &&
           !xiaomi_chain_active(now) && now - last_io_ >= oppo::kGapMs;
}

[[gnu::noinline]] bool Sink::oppo_tick(std::uint32_t now) {
    // SinkTxNG is asked only when the probe really has a message due (txok_.hold counts an episode per ask).
    const bool idle = op_.ready_to_send(now) && oppo_line_idle(now) && !txok_.hold(rev_, now);
    const oppo::Action a = op_.next(now, idle);
    switch (a.want) {
        case oppo::Want::None: return false;
        case oppo::Want::SwapToDfp: case oppo::Want::SwapToUfp:
            queue_dr_swap(a.want == oppo::Want::SwapToDfp, now);
            break;
        case oppo::Want::Vdm: {
            TxMsg m;
            m.header = make_header(kMsgVendorDefined, a.vdm.n, rev_);
            m.len = std::uint8_t(4 * a.vdm.n);
            for (unsigned i = 0; i < a.vdm.n; i++)
                for (int b = 0; b < 4; b++) m.data[4 * i + b] = std::uint8_t(a.vdm.w[i] >> (8 * b));
            queue(m);
            tx_owner_ = TxOwner::Probe;
            break;
        }
        case oppo::Want::GetCaps:
            queue_get_caps(now);
            tx_owner_ = TxOwner::Probe;
            break;
    }
    txok_.sent();
    return true;
}
#endif   // K2_OPPO_PROBE

}  // namespace k2::pd
