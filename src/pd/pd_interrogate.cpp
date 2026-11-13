// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_interrogate.hpp"

#include <algorithm>

#include "k2/pd/pd_ext.hpp"

namespace k2::pd {

namespace {

constexpr std::uint8_t kVdmIdentity = 1, kVdmSvids = 2, kVdmModes = 3;
constexpr std::uint16_t kRoleBits = 0x0120;   // header bit 8 power role, bit 5 data role

constexpr bool from_source(std::uint16_t h) { return (h & 0x0100) != 0; }
constexpr std::uint8_t type_of(std::uint16_t h) { return std::uint8_t(h & 0x1F); }
constexpr unsigned ndo_of(std::uint16_t h) { return (h >> 12) & 7; }
constexpr bool ext_of(std::uint16_t h) { return (h & 0x8000) != 0; }
constexpr std::uint8_t id_of(std::uint16_t h) { return std::uint8_t((h >> 9) & 7); }
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
bool due(std::uint32_t now, std::uint32_t t) { return std::int32_t(now - t) >= 0; }

}  // namespace

const char* iq_phase_name(IqPhase p) {
    switch (p) {
        case IqPhase::Idle: return "idle";
        case IqPhase::Reset1: return "reset";
        case IqPhase::Sniff1: return "sniff";
        case IqPhase::Settle: return "settle";
        case IqPhase::Ask: return "ask";
        case IqPhase::Reset2: return "reset back";
        case IqPhase::Sniff2: return "sniff back";
        case IqPhase::Done: return "done";
        case IqPhase::Aborted: return "aborted";
    }
    return "?";
}

const char* iq_step_name(IqStep s) {
    switch (s) {
        case IqStep::Identity: return "Discover Identity";
        case IqStep::SinkCap: return "Get_Sink_Cap";
        case IqStep::SinkCapExt: return "Get_Sink_Cap_Extended";
        case IqStep::Revision: return "Get_Revision";
        case IqStep::Svids: return "Discover SVIDs";
        case IqStep::Modes: return "Discover Modes";
        case IqStep::Done: return "done";
        case IqStep::EprSinkCap: return "EPR_Get_Sink_Cap";
    }
    return "?";
}

const char* iq_why_name(IqWhy w) {
    switch (w) {
        case IqWhy::None: return "none";
        case IqWhy::Stopped: return "stopped";
        case IqWhy::Detached: return "detached";
        case IqWhy::NotInline: return "inline link lost";
        case IqWhy::NoContract: return "no renegotiation";
        case IqWhy::Resets: return "too many resets";
        case IqWhy::Timeout: return "time limit";
        case IqWhy::SourceResets: return "charger resets when asked";
        case IqWhy::SourceRestarts: return "charger restarts when asked";
    }
    return "?";
}

const char* iq_result_name(IqResult r) {
    switch (r) {
        case IqResult::Pending: return "pending";
        case IqResult::Answered: return "device answered";
        case IqResult::Refused: return "device refused";
        case IqResult::ChargerOnly: return "only the charger spoke";
        case IqResult::GoodCrcOnly: return "device GoodCRC only";
        case IqResult::Nothing: return "nothing";
        case IqResult::SourceReset: return "charger Hard Reset";
        case IqResult::SourceRestart: return "charger restarted";
    }
    return "?";
}

const char* iq_refusal_name(IqRefusal r) {
    switch (r) {
        case IqRefusal::None: return "started";
        case IqRefusal::Running: return "another PD tool is running";
        case IqRefusal::NoCc: return "no CC line attached";
        case IqRefusal::NotInline: return "no device inline";
        case IqRefusal::Busy: return "a test or trigger owns the ports";
        case IqRefusal::OwnContract: return "the K2 holds its own contract";
        case IqRefusal::SelfPowered: return "the K2 runs on this VBUS";
        case IqRefusal::NeedConfirm: return "needs the confirm";
        case IqRefusal::BadSpoof: return "the spoof target is unknown";
    }
    return "?";
}

const char* iq_match_name(IqMatch m) {
    switch (m) {
        case IqMatch::Unknown: return "unknown";
        case IqMatch::Same: return "same contract";
        case IqMatch::Differs: return "contract changed";
        case IqMatch::NoSecond: return "no contract after the reset";
    }
    return "?";
}

// ---- the watch ----

void IqWatch::reset() { *this = IqWatch{}; }

void IqWatch::on_message(Sop sop, std::uint16_t h, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (sop != Sop::Sop) return;
    heard_ = true;
    last_at_ = now;
    const std::uint8_t t = type_of(h);
    const unsigned n = ndo_of(h);
    const bool ext = ext_of(h), ctl = n == 0 && !ext;
    if (ctl && t == kCtlGoodCrc) return;   // the PHY's acknowledgement: no MessageID of its own to follow
    if (ctl && t == kCtlSoftReset) {        // either side: the counters restart, the source lists again
        caps_ = requested_ = accepted_ = false;
        c_ = {};
    }
    if (from_source(h)) {
        src_known_ = true;
        src_id_ = id_of(h);
        src_roles_ = std::uint16_t(h & kRoleBits);
        src_rev_ = std::uint8_t((h >> 6) & 3);
        if (!ext && n > 0 && t == kDataSourceCap) {
            n_pdos_ = std::uint8_t(std::min<std::size_t>(std::min<unsigned>(n, 7), payload.size() / 4));
            for (unsigned i = 0; i < n_pdos_; i++) pdos_[i] = le32(&payload[4 * i]);
            caps_ = true;
            requested_ = accepted_ = false;
            c_ = {};
        } else if (ctl && t == kCtlAccept && requested_) {
            accepted_ = true;
        } else if (ctl && (t == kCtlReject || t == kCtlWait)) {
            requested_ = accepted_ = false;
        } else if (ctl && t == kCtlPsRdy && accepted_) {
            c_.ready = true;
            c_.pos = pend_pos_;
            c_.rdo = pend_rdo_;
            c_.dev_rev = pend_rev_;
            c_.src_pdo = pdos_[pend_pos_ - 1];
            psrdy_at_ = now;
            requested_ = accepted_ = false;
        }
        return;
    }
    if (!ext && n > 0 && t == kDataRequest && caps_ && payload.size() >= 4) {
        const std::uint32_t rdo = le32(payload.data());
        const unsigned pos = (rdo >> 28) & 0xF;
        requested_ = pos >= 1 && pos <= n_pdos_;
        accepted_ = false;
        if (requested_) {
            pend_pos_ = std::uint8_t(pos);
            pend_rdo_ = rdo;
            pend_rev_ = std::uint8_t((h >> 6) & 3);
        }
    }
}

// ---- the sequencer ----

IqRefusal Interrogate::start(IqFacts f, std::uint32_t now) {
    f.other_running = f.other_running || running();
    const IqRefusal r = iq_refusal(f);
    if (r != IqRefusal::None) return r;
    st_ = IqStatus{};
    st_.started_at = now;
    now_ = now;
    attempts_ = mode_idx_ = order_idx_ = 0;
    foreign_mask_ = 0;
    soft_since_tx_ = false;
    peer_spoke_since_tx_ = false;
    st_.step = kIqAskOrder[0];
    q_open_ = false;
    awaiting_ = acked_ = tx_pending_ = dirty_ = false;
    why_pending_ = IqWhy::None;
    st_.spoof = f.spoof;
    tx_chunk_ = chunk_pending_ = false;
    epr_raw_ = {};
    epr_size_ = epr_have_ = 0;
    sopp_until_ = sopp_first_ = 0;
    sopp_started_ = false;
    // With the spoof the first Hard Reset always goes: the charger has to ask the cable again (after a reset it does) while the
    // spoof answers, and a fresh contract sniffed earlier says nothing about that.
    if (!f.spoof && watch_.contract().ready && watch_.src_known()) {
        st_.skipped_first_reset = true;
        st_.first = watch_.contract();
        to_settle(now);
    } else {
        enter_reset(IqPhase::Reset1);
    }
    return IqRefusal::None;
}

void Interrogate::enter_reset(IqPhase ph) {
    st_.phase = ph;
    phase_at_ = now_;
    awaiting_ = false;
    tx_chunk_ = chunk_pending_ = false;
    tx_ = TxMsg{};
    tx_.hard_reset = true;
    tx_pending_ = true;
}

void Interrogate::to_settle(std::uint32_t now) {
    const IqContract& c = watch_.contract();
    std::uint8_t rev = c.dev_rev != 0 ? c.dev_rev : 2;
    if (watch_.src_rev() != 0 && watch_.src_rev() < rev) rev = watch_.src_rev();
    st_.rev = rev >= 2 ? 2 : 1;
    st_.phase = IqPhase::Settle;
    phase_at_ = now;
}

void Interrogate::end(IqPhase ph, IqWhy why) {
    st_.phase = ph;
    st_.why = why;
    st_.ended_at = now_;
    tx_pending_ = awaiting_ = false;
}

void Interrogate::stop(IqWhy why) {
    if (dirty_) {
        if (why_pending_ == IqWhy::None) why_pending_ = why;
        enter_reset(IqPhase::Reset2);
        return;
    }
    const IqWhy w = why != IqWhy::None ? why : why_pending_;
    end(w == IqWhy::None ? IqPhase::Done : IqPhase::Aborted, w);
}

void Interrogate::abort(IqWhy why, std::uint32_t now) {
    now_ = now;
    if (!running()) return;
    if (why == IqWhy::Detached || why == IqWhy::NotInline) {   // nobody left to resync
        end(IqPhase::Aborted, why);
        return;
    }
    switch (st_.phase) {
        case IqPhase::Reset2:
        case IqPhase::Sniff2:
            if (why_pending_ == IqWhy::None) why_pending_ = why;   // the final reset is on its way
            return;
        default:
            stop(why);
            return;
    }
}

std::uint8_t Interrogate::modes_wanted() const {
    if (!(st_.ok_mask & iq_bit(IqStep::Svids))) return 0;
    return std::uint8_t(std::min<unsigned>(st_.num_svids, kIdModeSvids));
}

bool Interrogate::wanted(IqStep s) const {
    const std::uint8_t b = iq_bit(s);
    if ((st_.ok_mask | st_.refused_mask | st_.silent_mask) & b) return false;
    switch (s) {
        case IqStep::Identity:
        case IqStep::SinkCap: return true;
        case IqStep::SinkCapExt:
        case IqStep::Revision:
        case IqStep::EprSinkCap: return st_.rev >= 2;   // PD 2.0 has none of these messages (EPR is PD 3.1)
        case IqStep::Svids: return st_.id.valid && ((st_.id.id_header >> 26) & 1) != 0;   // ID Header: modal operation supported
        case IqStep::Modes: return mode_idx_ < modes_wanted();
        case IqStep::Done: return false;
    }
    return false;
}

// An answer is in: a step already given up as silent becomes a late answer (it shows as answered, not as silent).
void Interrogate::mark_ok(IqStep s) {
    const std::uint8_t b = iq_bit(s);
    st_.ok_mask |= b;
    if (st_.silent_mask & b) {
        st_.silent_mask = std::uint8_t(st_.silent_mask & ~b);
        st_.late_mask |= b;
    }
}

// ---- the spoof window ----

bool Interrogate::sopp_window(std::uint32_t now) const {
    if (!st_.spoof || !running()) return false;
    switch (st_.phase) {   // our questions go out with Auto-CRC off, and a reset is no time for acknowledgements
        case IqPhase::Sniff1:
        case IqPhase::Settle:
        case IqPhase::Sniff2: break;
        default: return false;
    }
    return std::int32_t(sopp_until_ - now) > 0;
}

// The charger's SOP' frame (the driver sees it with Auto-CRC off the first time: only a Discover Identity request counts).
void Interrogate::on_sopp(std::uint16_t h, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (!st_.spoof || ext_of(h) || ndo_of(h) == 0 || type_of(h) != kMsgVendorDefined || payload.size() < 4) return;
    const std::uint32_t vh = le32(payload.data());
    if (!((vh >> 15) & 1) || (vh & 0x1F) != kVdmIdentity) return;
    if (((vh >> 6) & 3) != 0) {   // an answer on SOP' is never ours (our reply goes out, not in): a real chip in the cable
        st_.sopp_real = true;
        return;
    }
    if (st_.sopp_queries < 255) st_.sopp_queries++;
    if (!sopp_started_) {
        sopp_started_ = true;
        sopp_first_ = now;
    }
    if (std::uint32_t(now - sopp_first_) <= kIqSoppMaxMs) sopp_until_ = now + kIqSoppWindowMs;   // a retry re-opens it, within the budget
}

void Interrogate::on_sopp_reply(std::uint32_t now) {
    if (st_.sopp_replies < 255) st_.sopp_replies++;
    if (std::int32_t(sopp_until_ - (now + kIqSoppReplyMs)) > 0) sopp_until_ = now + kIqSoppReplyMs;   // the next frame is the ACK
}

// The line is quiet long enough after PS_RDY and no spoof window is open.
bool Interrogate::settled(std::uint32_t now) const {
    return due(now, watch_.psrdy_at() + kIqSettleMs) && due(now, watch_.last_frame_at() + kIqQuietMs) && !sopp_window(now);
}

void Interrogate::close(IqResult r) {
    if (!q_open_) return;
    q_open_ = false;
    cur_.result = r;
    st_.q = cur_;
    st_.closed++;
}

void Interrogate::next_step() {
    close(IqResult::Answered);   // a no-op after refused() or a timeout
    awaiting_ = false;
    attempts_ = 0;
    if (order_idx_ < kIqAskOrder.size()) order_idx_++;
    st_.step = order_idx_ < kIqAskOrder.size() ? kIqAskOrder[order_idx_] : IqStep::Done;
}

// One Discover Modes SVID is over (answered, refused or silent): the next one, or the step ends.
void Interrogate::modes_next(bool got) {
    if (got) close(IqResult::Answered);
    awaiting_ = false;
    attempts_ = 0;
    mode_idx_++;
    if (mode_idx_ < modes_wanted()) return;
    if (st_.num_modes > 0) mark_ok(IqStep::Modes);
    else st_.refused_mask |= iq_bit(IqStep::Modes);
    next_step();
}

void Interrogate::refused() {
    close(IqResult::Refused);
    if (st_.step == IqStep::Modes) { modes_next(false); return; }
    st_.refused_mask |= iq_bit(st_.step);
    next_step();
}

void Interrogate::attempt_failed() {
    // A Wait or a BUSY comes here too: the device did speak, it only said not now.
    close(cur_.dev_msgs > 0       ? IqResult::Refused
          : cur_.src_msgs > 0     ? IqResult::ChargerOnly
          : cur_.dev_goodcrc      ? IqResult::GoodCrcOnly
                                  : IqResult::Nothing);
    awaiting_ = false;
    if (++attempts_ < kIqAttempts) return;   // asked again with a fresh MessageID once the line is quiet
    if (st_.step == IqStep::Modes) {
        if (mode_idx_ + 1 >= modes_wanted() && st_.num_modes == 0) {
            st_.silent_mask |= iq_bit(IqStep::Modes);
            next_step();
            return;
        }
        modes_next(false);
        return;
    }
    if (!(st_.ok_mask & iq_bit(st_.step))) st_.silent_mask |= iq_bit(st_.step);   // answered meanwhile (to the charger): not silent
    next_step();
}

// A message of the device that matches the question in flight: the only kind that counts as its answer.
void Interrogate::got_answer() {
    if (awaiting_ && q_open_ && cur_.dev_msgs < 255) cur_.dev_msgs++;
}

// A device's structured VDM response to a request the CHARGER made a moment ago answers the charger, not us.
bool Interrogate::take_foreign(std::uint8_t cmd) {
    if (cmd < kVdmIdentity || cmd > kVdmModes) return false;
    const unsigned i = cmd - kVdmIdentity;
    const bool hit = (foreign_mask_ & (1u << i)) != 0 && std::uint32_t(now_ - foreign_at_[i]) <= kIqRespMs;
    foreign_mask_ = std::uint8_t(foreign_mask_ & ~(1u << i));
    return hit;
}

bool Interrogate::asking(IqStep s, std::uint16_t svid) const {
    return st_.phase == IqPhase::Ask && awaiting_ && st_.step == s && (s != IqStep::Modes || svid == cur_svid_);
}

void Interrogate::build() {
    TxMsg m;
    m.inline_tx = true;
    const std::uint8_t rev = st_.rev;
    auto vdm = [&](std::uint16_t svid, std::uint8_t cmd) {
        const std::uint32_t w = vdm_header(svid, cmd, rev);
        m.header = make_header(kMsgVendorDefined, 1, rev);
        m.len = 4;
        for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(w >> (8 * i));
    };
    switch (st_.step) {
        case IqStep::Identity: vdm(kSvidPd, kVdmIdentity); break;
        case IqStep::Svids: vdm(kSvidPd, kVdmSvids); break;
        case IqStep::Modes:
            cur_svid_ = id_modes_svid(std::span<const std::uint16_t>(st_.svids.data(), st_.num_svids), mode_idx_);
            vdm(cur_svid_, kVdmModes);
            break;
        case IqStep::SinkCap: m.header = make_header(kCtlGetSinkCap, 0, rev); break;
        case IqStep::SinkCapExt: m.header = make_header(kCtlGetSinkCapExt, 0, rev); break;
        case IqStep::Revision: m.header = make_header(kCtlGetRevision, 0, rev); break;
        case IqStep::EprSinkCap:   // Extended_Control, chunked form (as the sink's own EPR control messages): ext header 0x8002, ECDB type
            m.header = make_header(kExtControl, 1, rev, true);
            m.len = 4;
            m.data[0] = 0x02;
            m.data[1] = 0x80;
            m.data[2] = kEprGetSinkCap;
            epr_raw_ = {};
            epr_size_ = epr_have_ = 0;
            chunk_pending_ = false;
            break;
        case IqStep::Done: return;
    }
    m.header = with_msg_id(std::uint16_t(m.header | watch_.src_roles()), watch_.next_id());
    tx_ = m;
    tx_chunk_ = false;
    tx_pending_ = true;
}

// The next chunk of EPR_Sink_Capabilities (only chunk 1 is ever asked: kIqEprMaxBytes holds two). The 4-byte padded form of the
// request, as the sink's own chunk requests send it.
void Interrogate::build_chunk_request() {
    TxMsg m;
    m.inline_tx = true;
    m.header = make_header(kExtEprSinkCaps, 1, st_.rev, true);
    m.len = 4;
    const std::uint16_t req = std::uint16_t((1u << 15) | (1u << 10) | (1u << 11));
    m.data[0] = std::uint8_t(req);
    m.data[1] = std::uint8_t(req >> 8);
    m.header = with_msg_id(std::uint16_t(m.header | watch_.src_roles()), watch_.next_id());
    tx_ = m;
    tx_chunk_ = true;
    tx_pending_ = true;
}

void Interrogate::tick(std::uint32_t now) {
    now_ = now;
    switch (st_.phase) {
        case IqPhase::Idle:
        case IqPhase::Done:
        case IqPhase::Aborted:
        case IqPhase::Reset1:
        case IqPhase::Reset2:   // the Hard Reset waits in tx_ for the driver
            return;
        case IqPhase::Sniff1:
        case IqPhase::Sniff2: {
            const IqContract& c = watch_.contract();
            if (c.ready) {
                if (st_.phase == IqPhase::Sniff1) {
                    if (!st_.first.ready) st_.first = c;
                    to_settle(now);
                } else {
                    // With the spoof the charger may still ask the cable and list again (a 5 A cable known): the contract
                    // that counts is the one after the settle, kIqSettleMaxMs at most.
                    if (st_.spoof && !settled(now) && !due(now, watch_.psrdy_at() + kIqSettleMaxMs)) return;
                    st_.second = c;
                    st_.match = same_contract(st_.first, c) ? IqMatch::Same : IqMatch::Differs;
                    end(why_pending_ == IqWhy::None ? IqPhase::Done : IqPhase::Aborted, why_pending_);
                }
                return;
            }
            if (!due(now, phase_at_ + kIqRenegMs)) return;
            if (st_.phase == IqPhase::Sniff1) {
                stop(IqWhy::NoContract);
            } else {
                st_.match = IqMatch::NoSecond;
                end(why_pending_ == IqWhy::None ? IqPhase::Done : IqPhase::Aborted, why_pending_);
            }
            return;
        }
        case IqPhase::Settle: {
            if (due(now, st_.started_at + kIqTotalMs)) { stop(IqWhy::Timeout); return; }
            if (sopp_window(now)) return;   // the spoof answers the charger's cable query first (bounded by kIqSoppMaxMs)
            if (!settled(now) && !due(now, phase_at_ + kIqSettleMaxMs)) return;
            st_.phase = IqPhase::Ask;
            phase_at_ = now;
            attempts_ = 0;
            awaiting_ = false;
            break;
        }
        case IqPhase::Ask: break;
    }
    // Ask.
    if (tx_pending_) return;
    if (awaiting_ && chunk_pending_) {   // the first EPR chunk said more follows: ask for it once the line is quiet
        if (!due(now, watch_.last_frame_at() + kIqTxQuietMs)) return;
        chunk_pending_ = false;
        build_chunk_request();
        return;
    }
    if (awaiting_) {
        if (!due(now, deadline_)) return;
        // The first EPR chunk arrived but not the second: what is kept is the answer (marked partial), no second question.
        if (st_.step == IqStep::EprSinkCap && (st_.ok_mask & iq_bit(IqStep::EprSinkCap))) next_step();
        else attempt_failed();   // silence: a collision, a deaf device, or our question dropped
    }
    if (!watch_.contract().ready) {   // a Soft_Reset between the charger and the device: wait for their new contract first
        st_.phase = IqPhase::Sniff1;
        phase_at_ = now;
        return;
    }
    if (due(now, st_.started_at + kIqTotalMs)) {
        if (why_pending_ == IqWhy::None) why_pending_ = IqWhy::Timeout;
        stop(why_pending_);
        return;
    }
    while (st_.step != IqStep::Done && !wanted(st_.step)) next_step();
    if (st_.step == IqStep::Done) {
        stop(IqWhy::None);
        return;
    }
    if (watch_.heard() && !due(now, watch_.last_frame_at() + kIqTxQuietMs)) return;
    build();
}

bool Interrogate::take_tx(TxMsg& out) {
    if (!tx_pending_) return false;
    out = tx_;
    tx_pending_ = false;
    if (out.hard_reset) {
        st_.resets_sent++;
        watch_.reset();
        sopp_started_ = false;   // each negotiation gets its own SOP' window budget
        sopp_until_ = 0;
        dirty_ = false;
        st_.phase = st_.phase == IqPhase::Reset1 ? IqPhase::Sniff1 : IqPhase::Sniff2;
        phase_at_ = now_;
        return true;
    }
    if (tx_chunk_) {   // a chunk request of the question in flight: not a new question, the same window goes on
        tx_chunk_ = false;
        watch_.on_own_tx(out.header);
        sent_id_ = id_of(out.header);
        tx_at_ = now_;
        acked_ = false;
        deadline_ = now_ + kIqRespMs;
        return true;
    }
    st_.sent++;
    st_.asked_mask |= iq_bit(st_.step);
    watch_.on_own_tx(out.header);
    sent_id_ = id_of(out.header);
    tx_at_ = now_;
    soft_since_tx_ = false;
    peer_spoke_since_tx_ = false;
    cur_ = IqQuestion{};
    cur_.step = st_.step;
    cur_.id = sent_id_;
    cur_.attempt = std::uint8_t(attempts_ + 1);
    q_open_ = true;
    dirty_ = true;
    awaiting_ = true;
    acked_ = false;
    deadline_ = now_ + kIqRespMs;
    return true;
}

void Interrogate::on_tx_result(bool acked, std::uint32_t now) {
    now_ = now;
    if (!awaiting_) return;
    (void)acked;   // the GoodCRCs of the device and the charger collide: an answer may come either way
    deadline_ = now + kIqRespMs;   // the window counts from the end of the transmit
}

void Interrogate::on_hard_reset(std::uint32_t now) {
    now_ = now;
    watch_.reset();
    foreign_mask_ = 0;
    if (!running()) return;
    switch (st_.phase) {
        case IqPhase::Reset1:
        case IqPhase::Reset2:   // somebody else reset first: that one resyncs the link, ours is not needed
            tx_pending_ = false;
            dirty_ = false;
            st_.phase = st_.phase == IqPhase::Reset1 ? IqPhase::Sniff1 : IqPhase::Sniff2;
            phase_at_ = now;
            return;
        case IqPhase::Sniff1:
        case IqPhase::Sniff2:
            phase_at_ = now;   // the wait for the contract starts again
            return;
        default:
            break;
    }
    // A Hard Reset right after our question is the question's doing (a source that takes a message from a "source" for a role
    // clash): stop at once. No final reset (theirs resynced the link), no resync wait, no further question: asking again
    // only puts the source into a reset loop. Not when the device spoke to the charger on its own after our question (a
    // VCONN_Swap, a Soft_Reset): then the reset is theirs, and the ordinary budgeted path below applies.
    if (st_.phase == IqPhase::Ask && dirty_ && !peer_spoke_since_tx_ && std::uint32_t(now - tx_at_) <= kIqBlameMs) {
        close(IqResult::SourceReset);
        st_.resets_seen++;
        dirty_ = false;
        end(IqPhase::Aborted, IqWhy::SourceResets);
        return;
    }
    dirty_ = false;   // the reset left nothing of ours behind
    awaiting_ = tx_pending_ = false;
    tx_chunk_ = chunk_pending_ = false;
    attempts_ = 0;
    close(IqResult::SourceReset);
    if (++st_.resets_seen > kIqMaxResets) {
        end(IqPhase::Aborted, IqWhy::Resets);
        return;
    }
    st_.phase = IqPhase::Sniff1;
    phase_at_ = now;
}

void Interrogate::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    now_ = now;
    watch_.on_message(sop, header, payload, now);
    if (!running()) return;
    if (sop == Sop::SopP) {
        on_sopp(header, payload, now);
        return;
    }
    if (sop != Sop::Sop) return;
    const bool goodcrc = ndo_of(header) == 0 && !ext_of(header) && type_of(header) == kCtlGoodCrc;
    if (from_source(header)) {
        if (goodcrc) return;
        if (awaiting_ && q_open_ && cur_.src_msgs < 255) cur_.src_msgs++;   // the charger spoke inside the answer window
        on_source(header, payload);
        return;
    }
    on_device(header, payload);   // dev_msgs counts only what matches the question (got_answer), not whatever the device said
}

// A message from the charger (the source power role). The watch has it already; this is what it means for the run.
void Interrogate::on_source(std::uint16_t h, std::span<const std::uint8_t> payload) {
    const std::uint8_t t = type_of(h);
    const unsigned n = ndo_of(h);
    if (ext_of(h)) return;
    if (n == 0) {
        if (t == kCtlSoftReset && dirty_) soft_since_tx_ = true;   // the charger's way of recovering: its new contract is not a restart
        return;
    }
    if (t == kDataSourceCap) {
        sopp_until_ = 0;   // a fresh negotiation: whatever the cable query left open is over
        // A fresh negotiation right after our question (without a Soft_Reset first) is the question's doing: the laptop on the
        // bench re-offered its capabilities ~130 ms after every question and its Request/Accept/PS_RDY followed. Anything the
        // device says from here on belongs to that negotiation or to the charger's own questions, not to ours. Stop at once:
        // the renegotiation is the resync, asking again only makes the source restart again.
        // Not when the device began a conversation of its own meanwhile (its VCONN_Swap, Soft_Reset, Discover Identity): the
        // renegotiation is then the two of them, and the ordinary path (wait for the new contract, ask again) applies.
        if (st_.phase == IqPhase::Ask && dirty_ && !soft_since_tx_ && !peer_spoke_since_tx_ && std::uint32_t(now_ - tx_at_) <= kIqRenegBlameMs) {
            close(IqResult::SourceRestart);
            dirty_ = false;
            awaiting_ = tx_pending_ = false;
            end(IqPhase::Aborted, IqWhy::SourceRestarts);
        }
        return;
    }
    if (t == kMsgVendorDefined && payload.size() >= 4) {   // the charger's own structured request: the device's reply to it is
        const std::uint32_t vh = le32(payload.data());
        const std::uint8_t cmd = std::uint8_t(vh & 0x1F);
        if (((vh >> 15) & 1) && ((vh >> 6) & 3) == 0 && cmd >= kVdmIdentity && cmd <= kVdmModes) {
            foreign_mask_ = std::uint8_t(foreign_mask_ | (1u << (cmd - kVdmIdentity)));
            foreign_at_[cmd - kVdmIdentity] = now_;
        }
    }
}

// A chunk of EPR_Sink_Capabilities (extended, sink role). Chunk 0 is already an answer (kept, marked partial while the rest is
// outstanding); chunk 1 completes it. A third chunk is never asked: kIqEprMaxBytes holds the two.
void Interrogate::on_epr_chunk(std::span<const std::uint8_t> payload) {
    const ExtHeader x = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (x.request || x.chunk_number > 1) return;
    const std::size_t size = x.data_size;
    const std::size_t base = std::size_t(x.chunk_number) * kChunkBytes;
    if (x.chunk_number == 1 && (!(st_.ok_mask & iq_bit(IqStep::EprSinkCap)) || epr_have_ != kChunkBytes || size != epr_size_)) return;
    if (size <= base) return;
    const std::size_t take = std::min<std::size_t>({size - base, kChunkBytes, payload.size() - 2, kIqEprMaxBytes - base});
    if (take == 0) return;
    if (x.chunk_number == 0) {
        epr_raw_ = {};
        epr_size_ = std::uint16_t(size);
    }
    std::copy_n(payload.begin() + 2, take, epr_raw_.begin() + base);
    epr_have_ = std::uint16_t(base + take);
    st_.num_epr_pdos = std::uint8_t(epr_have_ / 4);
    for (unsigned i = 0; i < st_.num_epr_pdos; i++) st_.epr_pdos[i] = le32(&epr_raw_[4 * i]);
    st_.epr_partial = epr_have_ < epr_size_;
    mark_ok(IqStep::EprSinkCap);
    if (!asking(IqStep::EprSinkCap)) return;
    // More announced and room for it: the second chunk is asked for. Otherwise this is all there will be.
    if (x.chunk_number == 0 && epr_size_ > kChunkBytes && kIqEprMaxBytes > kChunkBytes) {
        chunk_pending_ = true;
        return;
    }
    got_answer();
    next_step();
}

// A message from the device (the sink power role): an answer to us, or to the charger's own question. Either way it is kept.
void Interrogate::on_device(std::uint16_t h, std::span<const std::uint8_t> payload) {
    const std::uint8_t t = type_of(h);
    const unsigned n = ndo_of(h);
    const bool ext = ext_of(h);
    if (n == 0 && !ext) {
        if (t == kCtlGoodCrc) {
            if (awaiting_ && !acked_ && id_of(h) == sent_id_) {
                acked_ = true;
                cur_.dev_goodcrc = true;
                st_.dev_acks++;
            }
            return;
        }
        // A control message that is not a refusal of ours (VCONN_Swap, Soft_Reset...) is the device talking to the charger
        // on its own: a reset that follows is the two of them, not our question.
        if (st_.phase == IqPhase::Ask && dirty_ && t != kCtlNotSupported && t != kCtlReject && t != kCtlWait)
            peer_spoke_since_tx_ = true;
        if (st_.phase != IqPhase::Ask || !awaiting_) return;
        if (t == kCtlNotSupported || t == kCtlReject) { got_answer(); refused(); }
        else if (t == kCtlWait) { got_answer(); attempt_failed(); }
        return;
    }
    if (ext) {
        if (payload.size() < 2) return;
        if (t == kExtEprSinkCaps) { on_epr_chunk(payload); return; }
        if (t != kExtSinkCapsExt) return;
        const ExtHeader x = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
        if (x.request || x.chunk_number != 0) return;
        const std::size_t len = std::min<std::size_t>({std::size_t(x.data_size), payload.size() - 2, kIqSinkExtBytes});
        if (len == 0) return;
        st_.sink_ext = {};
        std::copy_n(payload.begin() + 2, len, st_.sink_ext.begin());
        st_.sink_ext_len = std::uint8_t(len);
        mark_ok(IqStep::SinkCapExt);
        if (asking(IqStep::SinkCapExt)) { got_answer(); next_step(); }
        return;
    }
    if (t == kDataSinkCap) {
        st_.num_sink_pdos = std::uint8_t(std::min<std::size_t>(std::min<unsigned>(n, 7), payload.size() / 4));
        for (unsigned i = 0; i < st_.num_sink_pdos; i++) st_.sink_pdos[i] = le32(&payload[4 * i]);
        mark_ok(IqStep::SinkCap);
        if (asking(IqStep::SinkCap)) { got_answer(); next_step(); }
        return;
    }
    if (t == kDataRevision && payload.size() >= 4) {
        st_.revision = le32(payload.data());
        mark_ok(IqStep::Revision);
        if (asking(IqStep::Revision)) { got_answer(); next_step(); }
        return;
    }
    if (t != kMsgVendorDefined || payload.size() < 4) return;
    const std::uint32_t vh = le32(payload.data());
    if (!((vh >> 15) & 1)) return;   // unstructured: not an answer
    const std::uint16_t svid = std::uint16_t(vh >> 16);
    const std::uint8_t cmd = std::uint8_t(vh & 0x1F), ctype = std::uint8_t((vh >> 6) & 3);
    if (ctype == 0) {   // a question of the device's own: it is talking to the charger, whatever follows is theirs
        if (st_.phase == IqPhase::Ask && dirty_) peer_spoke_since_tx_ = true;
        return;
    }
    IqStep s;
    if (svid == kSvidPd && cmd == kVdmIdentity) s = IqStep::Identity;
    else if (svid == kSvidPd && cmd == kVdmSvids) s = IqStep::Svids;
    else if (cmd == kVdmModes) s = IqStep::Modes;
    else return;
    // Command and SVID match ours, and the charger did not ask the same a moment ago: only then it answers us.
    const bool foreign = take_foreign(cmd);
    const bool ours = !foreign && asking(s, svid);
    const std::size_t nw = std::min<std::size_t>(payload.size() / 4, 7);
    if (ctype == 1) {   // ACK: keep it whoever asked
        if (s == IqStep::Identity && nw >= 2) {
            Identity id;
            id.valid = true;
            id.id_header = le32(&payload[4]);
            id.vid = std::uint16_t(id.id_header & 0xFFFF);
            id.vendor = svid_name(id.vid);
            if (nw >= 3) id.cert_stat = le32(&payload[8]);
            if (nw >= 4) {
                const std::uint32_t p = le32(&payload[12]);
                id.pid = std::uint16_t(p >> 16);
                id.bcd = std::uint16_t(p & 0xFFFF);
            }
            id.num_vdos = std::uint8_t(std::min<std::size_t>(nw, id.vdos.size()));
            for (std::size_t i = 0; i < id.num_vdos; i++) id.vdos[i] = le32(&payload[4 * i]);
            st_.id = id;
            mark_ok(IqStep::Identity);
            if (ours) { got_answer(); next_step(); }
        } else if (s == IqStep::Svids) {
            st_.num_svids = 0;
            for (std::size_t i = 1; i < nw; i++) {
                const std::uint32_t w = le32(&payload[4 * i]);
                bool end_of_list = false;
                for (const std::uint16_t v : {std::uint16_t(w >> 16), std::uint16_t(w & 0xFFFF)}) {
                    if (v == 0) { end_of_list = true; break; }
                    if (st_.num_svids < kMaxSvids) st_.svids[st_.num_svids++] = v;
                }
                if (end_of_list) break;
            }
            mark_ok(IqStep::Svids);
            if (ours) { got_answer(); next_step(); }
        } else if (s == IqStep::Modes) {
            bool have = false;
            for (std::size_t i = 0; i < st_.num_modes; i++) have = have || st_.mode_svid[i] == svid;
            if (!have && st_.num_modes < kIdModeSvids) {
                st_.mode_svid[st_.num_modes] = svid;
                st_.mode_vdo[st_.num_modes++] = nw >= 2 ? le32(&payload[4]) : 0;
            }
            if (st_.silent_mask & iq_bit(IqStep::Modes)) mark_ok(IqStep::Modes);   // a late one
            if (ours) { got_answer(); modes_next(true); }
        }
        return;
    }
    if (!ours) return;
    got_answer();
    if (ctype == 2) refused();   // NAK
    else attempt_failed();       // BUSY
}

}  // namespace k2::pd
