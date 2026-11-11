// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_sink.hpp"

#include <algorithm>

#include "k2/pd/pd_link.hpp"

namespace k2::pd {

namespace {

// Bits 24 (No USB Suspend) and 25 (USB Communications Capable) are both set.
constexpr std::uint32_t kRdoNoSuspend = 3u << 24, kRdoEpr = 1u << 22;
constexpr std::uint32_t kAbortHardResetMs = 500;  // still above the cap this long after the 5 V request
constexpr std::uint32_t kTxRetryMs = 50;
constexpr std::uint16_t kReleaseMaxMa = 3000;

std::uint32_t pos_bits(std::uint8_t pos) { return std::uint32_t(pos & 0xF) << 28; }

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
bool source_offers_epr(const SourceCaps& c) {
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

std::uint32_t rdo_fixed(std::uint8_t pos, std::uint16_t op_ma, std::uint16_t max_ma, bool epr) {
    return pos_bits(pos) | kRdoNoSuspend | (epr ? kRdoEpr : 0u) |
           (std::uint32_t(std::min<unsigned>(op_ma / 10u, 1023u)) << 10) | std::min<unsigned>(max_ma / 10u, 1023u);
}

std::uint32_t rdo_pps(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr) {
    return pos_bits(pos) | kRdoNoSuspend | (epr ? kRdoEpr : 0u) | (std::uint32_t((mv / 20u) & 0xFFF) << 9) |
           std::min<unsigned>(ma / 50u, 127u);
}

std::uint32_t rdo_avs(std::uint8_t pos, std::uint16_t mv, std::uint16_t ma, bool epr) {
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

const char* state_name(SinkState s) {
    switch (s) {
        case SinkState::Idle: return "idle";
        case SinkState::EprEntering: return "entering EPR";
        case SinkState::EprWaitCaps: return "reading EPR caps";
        case SinkState::Requesting: return "requesting";
        case SinkState::Transition: return "transition";
        case SinkState::Ready: return "ready";
        case SinkState::Aborting: return "back to 5V";
        case SinkState::Failed: return "failed";
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

std::uint32_t request_cap_mv(std::int32_t max_request_mv, std::int32_t max_vbus_mv) {
    if (max_request_mv <= 0 || max_vbus_mv <= 0) return kVsafe5vMv;
    return std::uint32_t(std::max<std::int32_t>(std::min(max_request_mv, max_vbus_mv), std::int32_t(kVsafe5vMv)));
}

Plan plan_request(const SourceCaps& caps, Choice c, std::uint32_t cap_mv, bool epr_mode) {
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
    p.err = Err::Ok;
    return p;
}

Choice lookup_held(const SourceCaps& caps, std::uint16_t mv, std::uint16_t ma, PdoType type) {
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

std::array<std::uint8_t, 24> make_skedb() {
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

void Sink::queue(const TxMsg& m) {
    tx_ = m;
    tx_pending_ = true;
}

bool Sink::take_tx(TxMsg& out) {
    if (sink_cap_pending_ && !(tx_pending_ && tx_.hard_reset)) {   // tSenderResponse: the answer goes first
        sink_cap_pending_ = false;
        update_rev();
        out = TxMsg{};
        out.header = make_header(kDataSinkCap, 2, rev_);
        out.len = 8;
        for (int w = 0; w < 2; w++)
            for (int i = 0; i < 4; i++) out.data[4 * w + i] = std::uint8_t(kSinkCapPdos[w] >> (8 * i));
        last_tx_ = out;
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
        last_tx_ = out;
        return true;
    }
    if (swap_reply_ && !(tx_pending_ && tx_.hard_reset)) {   // a role-swap answer: also tSenderResponse
        out = TxMsg{};
        out.header = make_header(swap_reply_, 0, rev_);
        swap_reply_ = 0;
        last_tx_ = out;
        return true;
    }
    if (!tx_pending_) return false;
    out = tx_;
    last_tx_ = tx_;
    tx_pending_ = false;
    return true;
}

void Sink::fail(SinkResult r, SinkState to) {
    st_.result = r;
    st_.held = false;
    st_.state = to;
    retry_pending_ = false;
    retx_pending_ = false;
    deferred_ = Deferred::None;
    release_pending_ = false;   // whatever ended the attempt also ends the wish and the VBUS check that hung on it
    verify_ = false;
    st_.verifying = false;
}

// The attempt a release waited behind failed (Reject, Wait exhausted): the release is still wanted, so it survives fail().
void Sink::fail_keep_release(SinkResult r, SinkState to, std::uint32_t now) {
    const bool rp = release_pending_;
    fail(r, to);
    release_pending_ = rp;
    settle_release(now);
}

void Sink::do_hard_reset(SinkResult r, SinkState to, bool forced) {
    // A self-powered K2 lives on the source's VBUS: its own timeouts, aborts and over-cap guards end the attempt but never put
    // a Hard Reset on the wire (the source cuts VBUS for ~650 ms and the K2 dies). Only the explicit tool (`forced`) does.
    const bool wire = forced || !never_hard_reset_;
    TxMsg m;
    m.hard_reset = true;
    tx_pending_ = false;  // nothing else matters after a hard reset
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    swap_reply_ = 0;
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
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();   // the lab-only Apple readout belongs to the attach that just ended
    fail(r, to);
}

void Sink::hard_reset(std::uint32_t) { do_hard_reset(SinkResult::HardReset, SinkState::Idle, true); }

void Sink::on_hard_reset_rx(std::uint32_t) {
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
    swap_reply_ = 0;
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();
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
    swap_reply_ = 0;
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    // Reset Xiaomi auth state
    xiaomi_authed_ = false;
    st_.xiaomi_authed = false;
    source_has_xiaomi_svid_ = false;
    st_.xiaomi_svid = false;
    st_.oppo_svid = false;
    xiaomi_cmd_ = 0;
    discover_svids_sent_ = false;
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    xm_ = {};
    ap_.reset();
    fail(SinkResult::None, SinkState::Idle);
}

void Sink::soft_reset(std::uint32_t now) {
    sink_cap_pending_ = false;
    sink_cap_ext_pending_ = false;
    swap_reply_ = 0;
    get_cap_sent_ = false;
    refresh_pending_ = false;
    TxMsg m;
    m.header = make_header(kCtlSoftReset, 0, rev_);
    m.restart_id = true;   // the MessageID counters restart on both sides
    queue(m);
    st_.epr_mode = false;
    contract_ = false;
    st_.pos = 0;
    // The contract is gone: the Xiaomi opener and the SVID discovery belong to the next one
    xiaomi_opener_pending_ = false;
    xiaomi_opener_sent_ = false;
    discover_svids_pending_ = false;
    sr_wait_ = true;
    sr_at_ = now + 400;
    ap_.reset();
    fail(SinkResult::SoftReset, SinkState::Idle);
}

bool Sink::xiaomi_chain_active(std::uint32_t now) const {
    if (!source_has_xiaomi_svid_ || !contract_) return false;
    // After 0x2717020A our Get_Source_Cap is out and the source re-announces: a Request before that answer would be for the old list.
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

void Sink::send_request(std::uint32_t now, bool may_defer) {
    if (may_defer && xiaomi_chain_active(now)) {   // tick() sends it when the chain ends; the Accept timer starts then
        deferred_ = Deferred::Request;
        xm_.req_held = true;
        retry_pending_ = false;
        retx_pending_ = false;
        return;
    }
    deferred_ = Deferred::None;
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
    if (may_defer && xiaomi_chain_active(now)) {
        deferred_ = Deferred::EprEnter;
        st_.epr_fail = 0;
        st_.state = SinkState::EprEntering;
        return;
    }
    deferred_ = Deferred::None;
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
    if (st_.epr_info != EprInfo::None || st_.epr_mode || st_.state != SinkState::Ready || tx_pending_ || xiaomi_chain_active(now)) return Err::Busy;
    queue_epr_control(kEprGetSourceCap);
    st_.epr_info = EprInfo::Pending;
    info_deadline_ = now + kEprInfoMs;
    return Err::Ok;
}

Err Sink::request_source_caps(std::uint32_t now) {
    if (!caps_) return Err::NoCaps;
    // One negotiation in flight: Ready only (no Request, Accept wait or PS_RDY outstanding), nothing of ours queued or being
    // retransmitted, no release / abort / VBUS check hanging on a Request, no Xiaomi chain (it holds our own Request back too), and
    // not still inside the last ask's answer window. EPR mode: an unsolicited Source_Capabilities there is a Hard Reset.
    if (st_.state != SinkState::Ready || st_.epr_mode || !contract_ || tx_pending_ || retx_pending_ || release_pending_ || verify_ ||
        deferred_ != Deferred::None || sr_wait_ || get_cap_sent_ || st_.epr_info == EprInfo::Pending || xiaomi_chain_active(now))
        return Err::Busy;
    TxMsg m;
    m.header = make_header(kCtlGetSourceCap, 0, rev_);
    queue(m);
    get_cap_sent_ = true;
    refresh_pending_ = true;
    refresh_deadline_ = now + kGetCapAnswerMs;
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
    switch (st_.state) {
        case SinkState::EprEntering: case SinkState::EprWaitCaps: case SinkState::Requesting:
        case SinkState::Transition: case SinkState::Aborting:
            return Err::Busy;
        default: break;
    }
    update_rev();
    const bool defer = c.pos == 0 || (c.pos >= 8 && !st_.epr_mode);  // EPR list needed first
    if (!defer) {
        const Plan p = plan_request(*caps_, c, cap_mv_, st_.epr_mode);
        if (p.err != Err::Ok) return p.err;
        plan_ = p;
    } else if (!caps_->valid) {
        return Err::NoCaps;
    }
    want_ = c;
    st_.held = true;
    st_.result = SinkResult::None;
    wait_retries_ = 0;
    verify_ = false;            // a new wish ends the check of the last release
    st_.verifying = false;
    if (defer) {
        if (st_.epr_mode) { proceed_after_epr_caps(now); return Err::Ok; }
        epr_caps_seen_ = false;
        send_epr_enter(now, true);
        return Err::Ok;
    }
    fresh_ = true;
    st_.state = SinkState::Requesting;
    send_request(now, true);
    return Err::Ok;
}

void Sink::proceed_after_epr_caps(std::uint32_t now) {
    if (want_.pos == 0) {  // only the list was wanted
        st_.result = SinkResult::Ok;
        st_.held = false;
        st_.state = SinkState::Ready;
        next_epr_ka_ = now + kEprKeepAliveMs;
        return;
    }
    const Plan p = plan_request(*caps_, want_, cap_mv_, true);
    if (p.err != Err::Ok) {
        fail(p.err == Err::OverCap ? SinkResult::OverCap : SinkResult::BadChoice, SinkState::Ready);
        next_epr_ka_ = now + kEprKeepAliveMs;
        return;
    }
    plan_ = p;
    fresh_ = true;
    st_.state = SinkState::Requesting;
    send_request(now, true);
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

void Sink::verify_vbus(std::uint32_t now) {
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

void Sink::release(std::uint32_t now) {
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

void Sink::after_ps_rdy(std::uint32_t now) {
    const std::int32_t drop_mv = contract_ ? std::int32_t(st_.mv) - std::int32_t(plan_.mv) : 0;   // the step down, for the descent window
    contract_ = true;
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
    }
    if (fresh_) {
        ready_at_ = now; fresh_ = false; tracking_ = false;
        descent_ms_ = descent_window_ms(drop_mv, plan_.type == PdoType::Pps || plan_.type == PdoType::Avs);
    }
    next_keepalive_ = now + kPpsKeepAliveMs;
    if (st_.epr_mode) next_epr_ka_ = now + kEprKeepAliveMs;
    settle_release(now);   // a release that waited behind this Request goes out now (not for the abort's own PS_RDY)
}

void Sink::request_chunk(std::uint16_t header, std::span<const std::uint8_t> payload) {
    (void)header;
    if (payload.size() < 2) return;
    const ExtHeader eh = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (eh.request || !eh.chunked) return;
    if ((unsigned(eh.chunk_number) + 1) * kChunkBytes >= eh.data_size) return;  // that was the last chunk
    TxMsg m;
    m.header = make_header(kExtEprSourceCaps, 1, rev_, true);
    // 4 B = 2 B extended header padded to the next 4-byte boundary, as required for a
    // chunked message with Data Size < 26 (USB PD R3.2 §6.5.1.1). The unpadded 2 B form was
    // tried and the Baseus 140 W stopped answering chunk requests, so keep the padded form.
    m.len = 4;
    const std::uint16_t req = std::uint16_t((1u << 15) | (1u << 10) | ((eh.chunk_number + 1u) << 11));
    m.data[0] = std::uint8_t(req);
    m.data[1] = std::uint8_t(req >> 8);
    queue(m);
}

void Sink::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (sop != Sop::Sop) return;
    const std::uint8_t type = header & 0x1F;
    const unsigned num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;

    // Handle Vendor Defined Messages (VDM) - Xiaomi Mi Turbo Charge auth (SVID 0x2717)
    if (!ext && type == kMsgVendorDefined && num_do > 0) {
        if (answers_sop_) on_vdm(header, payload, now);   // else: another sink's conversation, never answered
        return;
    }

    if (ext) {
        if (type != kExtEprSourceCaps) return;
        if (st_.epr_info == EprInfo::Pending && !st_.epr_mode) {
            // The answer to our SPR-mode EPR_Get_Source_Cap: informational (USB PD R3.2 6.5.17.1), never answered with a
            // Request. Fetch the remaining chunks; the engine decoded them into the bound info list.
            request_chunk(header, payload);
            if (info_ && info_->epr && !info_->epr_partial) st_.epr_info = EprInfo::Got;
            return;
        }
        epr_caps_seen_ = true;
        request_chunk(header, payload);
        if (st_.state == SinkState::EprWaitCaps && !tx_pending_ && caps_ && caps_->epr && !caps_->epr_partial) {
            proceed_after_epr_caps(now);
        }
        return;
    }

    if (num_do > 0) {
        if (type == kDataSourceCap) {
            update_rev();
            sr_wait_ = false;   // the caps after our Soft_Reset arrived
            if (xiaomi_authed_ && !xm_.reannounced) { xm_.reannounced = true; xm_.reannounce_at = now; }
            const bool solicited = get_cap_sent_;
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
            // SPR-mode re-announce: answer it. Also while Requesting - the in-flight Request was for the old
            // caps; without a fresh one the source's tSenderResponse expires and it Hard Resets
            // (CUKTECH C1 right after the Xiaomi auth unlock re-announces, hardware 2026-11-04; that source
            // has no EPR bit and was never in EPR mode, so it does not argue for answering in EPR mode).
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
                    const Plan p = plan_request(*caps_, hc, cap_mv_, st_.epr_mode);
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
                const Plan p = plan_request(*caps_, hc, cap_mv_, st_.epr_mode);
                if (p.err != Err::Ok) return;
                plan_ = p;
                st_.state = SinkState::Requesting;
                send_request(now);
            }
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
        }
        return;
    }

    switch (type) {
        case kCtlAccept:
            if (st_.state == SinkState::Requesting) {
                xm_.answer = ReqAnswer::Accept; xm_.answer_at = now;
                st_.accept_at = now; st_.ps_rdy_at = 0; st_.ps_rdy_inferred = false;
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
            if (st_.epr_info == EprInfo::Pending && st_.state == SinkState::Ready) st_.epr_info = EprInfo::Refused;
            break;
        case kCtlReject:
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
            swap_reply_ = swap_reply(k, rev_);
            break;
        }
        case kCtlSoftReset: {
            sink_cap_pending_ = false;
            sink_cap_ext_pending_ = false;
            swap_reply_ = 0;
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
            fail(SinkResult::SoftReset, SinkState::Idle);
            break;
        }
        default: break;
    }
}

void Sink::on_tx_result(bool ok, std::uint32_t now) {
    if (last_tx_.hard_reset) return;
    if (ok) { tx_retries_ = 0; return; }
    const std::uint8_t type = last_tx_.header & 0x1F;
    const bool ext = (last_tx_.header >> 15) & 1;
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

void Sink::on_vbus(std::int32_t mv, std::uint32_t now) {
    vbus_mv_ = mv; vbus_at_ = now; vbus_seen_ = true;
    if (verify_ && st_.state == SinkState::Ready) {   // the descent after a release: progress is a sample >= kFallStepMv below the last best
        if (!vref_set_ || mv + kFallStepMv <= vref_mv_) { vref_mv_ = mv; vref_at_ = now; vref_set_ = true; }
        if (mv <= kVSafe5MaxMv) { verify_ = false; st_.verifying = false; }
    }
    const bool active = st_.held || st_.state == SinkState::Requesting || st_.state == SinkState::Transition ||
                        st_.state == SinkState::Aborting;
    if (!active) return;
    if (mv > std::int32_t(cap_mv_)) {
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

void Sink::tick(std::uint32_t now) {
    if (retx_pending_ && now >= retx_at_ && !tx_pending_) {
        retx_pending_ = false;
        queue(last_tx_);
        return;
    }
    // Sink-initiated Xiaomi chain opener: fire after the pacing delay once the SVIDs exchange
    // showed 0x2717 and a contract is live (the VDM conversation dies with the contract).
    if (xiaomi_opener_pending_ && !tx_pending_ && contract_ && std::int32_t(now - xiaomi_opener_at_) >= 0) {
        xiaomi_opener_pending_ = false;
        xiaomi_opener_sent_ = true;
        xm_.opener_sent = true;
        const std::array<std::uint32_t, 1> vdos = {kXiaomiOpenerVdo};
        // 1-DO Vendor_Defined on SOP (TxMsg.sop defaults there; the driver sends sink messages on SOP only)
        send_xiaomi_vdm_reply(0, std::span<const std::uint32_t>(vdos), now);
        return;
    }
    // The paced proactive Discover-SVIDs REQ (on SOP, like every sink TX)
    if (discover_svids_pending_ && !tx_pending_ && std::int32_t(now - discover_svids_at_) >= 0) {
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
    if (ap_.armed()) {
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
        return;
    }
    if (refresh_pending_ && due(refresh_deadline_)) {   // a silent source: the answer window of our Refresh is over
        refresh_pending_ = false;
        get_cap_sent_ = false;
    }
    if (st_.epr_info == EprInfo::Pending && due(info_deadline_)) st_.epr_info = EprInfo::Timeout;
    if (deferred_ != Deferred::None) {   // a Request / EPR entry held back by the Xiaomi chain guard
        const bool stale = deferred_ == Deferred::Request ? st_.state != SinkState::Requesting : st_.state != SinkState::EprEntering;
        if (stale) {
            deferred_ = Deferred::None;
            xm_.req_held = false;
        } else {
            // The state timers (Accept, EPR enter) start when the message goes out: nothing below runs meanwhile.
            if (!tx_pending_ && !xiaomi_chain_active(now)) {
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
                if (due(retry_at_) && !tx_pending_) { retry_pending_ = false; send_request(now); }
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
            if (tx_pending_) break;
            if (verify_) { verify_vbus(now); break; }
            if (st_.held && contract_ && (plan_.type == PdoType::Pps || plan_.type == PdoType::Avs) &&
                due(next_keepalive_)) {
                st_.state = SinkState::Requesting;  // the same Request again, answered with Accept + PS_RDY
                send_request(now, true);
            } else if (st_.epr_mode && due(next_epr_ka_)) {
                next_epr_ka_ = now + kEprKeepAliveMs;
                queue_epr_control(kEprKeepAlive);
            }
            break;
        default: break;
    }
}

// ---- Xiaomi Mi Turbo Charge authentication (SVID 0x2717) ----

void Sink::on_vdm(std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
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

    // Only handle SOP VDMs (SOP' and SOP'' are for cables)

    // Discover Identity REQ from the source (0xFF008001): answer with our identity ACK
    // (0xFF008041: structured, ACK, cmd 1). The charger gates its whole VDM chain on this -
    // without an ACK it never sends Discover SVIDs and the Xiaomi auth never starts
    // (hardware 2026-11-04, CUKTECH 10 on C1: identity REQ seen, no ACK, chain dead).
    // Identity says "Xiaomi sink" (VID 0x2717, UFP AMA): hardware 2026-11-04 shows the
    // CUKTECH answers our Discover SVIDs with [0x2717] and keeps the auth chain dormant while
    // the sink identity is a foreign VID (0x0716); Mi Turbo Charge targets Xiaomi devices.
    if (svid == 0xFF00 && structured && cmd_type == 0 && cmd == 1) {
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

    // Only Xiaomi VDMs from here on; an unstructured 0x2717 REQ from the source is itself the
    // evidence that it is Xiaomi-family (the source-driven flow never lists 0x2717 in an ACK)
    if (svid != 0x2717) return;

    // Handle Xiaomi unstructured VDM commands (cmd_type = 0 = REQ from source)
    if (structured) return;  // we only handle unstructured Xiaomi VDMs

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
            // RX 0x27170203 -> TX 5-DO: 0x27170104, 0xA601934C, 0x1403BFD9, 0x55EBD77F, 0x509BA17F
            constexpr std::array<std::uint32_t, 5> vdos = {0x27170104u, 0xA601934Cu, 0x1403BFD9u, 0x55EBD77Fu, 0x509BA17Fu};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 4: {
            // RX 0x27170204 -> TX 5-DO: 0x27170105, 0x0BED65CD, 0xB2579250, 0x3AA3D437, 0x51EB9F51
            constexpr std::array<std::uint32_t, 5> vdos = {0x27170105u, 0x0BED65CDu, 0xB2579250u, 0x3AA3D437u, 0x51EB9F51u};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 5: {
            // RX 0x27170205 -> TX 2-DO: 0x27170106, 0x00000001
            constexpr std::array<std::uint32_t, 2> vdos = {0x27170106u, 0x00000001u};
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
            // RX 0x27170207 -> TX 5-DO: 0x27170108, 0x5D808AA6, 0x1D79FAA7, 0x11D10BDD, 0x8F158DE2
            constexpr std::array<std::uint32_t, 5> vdos = {0x27170108u, 0x5D808AA6u, 0x1D79FAA7u, 0x11D10BDDu, 0x8F158DE2u};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 8: {
            // RX 0x27170208 -> TX 2-DO: 0x2717010A, 0x00000001
            constexpr std::array<std::uint32_t, 2> vdos = {0x2717010Au, 0x00000001u};
            send_xiaomi_vdm_reply(cmd, std::span<const std::uint32_t>(vdos), now);
            break;
        }
        case 0x0A: {
            // RX 0x2717020A -> set authed flag and send Get_Source_Cap
            xiaomi_authed_ = true;
            st_.xiaomi_authed = true;
            xiaomi_cmd_ = cmd;
            xm_.authed = true; xm_.authed_at = now; xm_.reannounced = false; xm_.reannounce_at = 0;
            // Send Get_Source_Cap to request updated capabilities
            TxMsg m;
            m.header = make_header(kCtlGetSourceCap, 0, rev_);
            queue(m);
            get_cap_sent_ = true;
            break;
        }
        default:
            break;
    }
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

}  // namespace k2::pd
