// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_reread.hpp"

#include "k2/pd/pd_identify.hpp"   // vdm_header

namespace k2::pd {

namespace {
constexpr std::uint16_t kPdSvid = 0xFF00;
constexpr std::uint8_t kVdmDiscoverIdentity = 1, kVdmDiscoverSvids = 2, kVdmDiscoverModes = 3;
// The order a run asks in. Lab: the original conversation, the survey's additions last. Survey: the questions that most
// often get an answer first, the PD3-only ones last, the plain re-read of the caps (not part of the survey) at the end.
constexpr unsigned kSteps = unsigned(RereadStep::Done);
constexpr RereadStep kLabOrder[kSteps] = {RereadStep::CapsExt, RereadStep::Identity, RereadStep::Svids, RereadStep::GetCap,
                                          RereadStep::GetCap2, RereadStep::Modes, RereadStep::MfrInfo};
constexpr RereadStep kSurveyOrder[kSteps] = {RereadStep::Identity, RereadStep::Svids, RereadStep::Modes, RereadStep::CapsExt,
                                             RereadStep::MfrInfo, RereadStep::GetCap, RereadStep::GetCap2};
constexpr std::uint8_t kPd3Only = kRereadCapsExt | kRereadMfrInfo;   // extended messages: PD 2.0 has none
}  // namespace

RereadStep Reread::step_at(unsigned p) const { return opts_.survey_order ? kSurveyOrder[p] : kLabOrder[p]; }

void Reread::start(std::uint8_t rev, std::uint8_t mask, std::uint8_t pdos_before, std::uint32_t now, bool attach,
                   RereadOpts opts) {
    st_ = {};
    st_.state = RereadState::Running;
    st_.mask = mask & kRereadMaskEvery;
    st_.pdos_before = pdos_before;
    opts_ = opts;
    rev_ = rev >= 2 ? 2 : 1;
    started_ = now_ = now;   // with attach: the start of the wait for the contract
    pace_until_ = now;
    awaiting_ = tx_pending_ = tx_sent_ = settling_ = modes_loaded_ = false;
    pos_ = 0;
    st_.step = RereadStep(0);
    if (attach) { st_.attaching = true; return; }
    begin_steps(now);
}

// The first asked step; the run's step budget starts here.
void Reread::begin_steps(std::uint32_t now) {
    st_.attaching = false;
    settling_ = false;
    started_ = now;
    pace_until_ = now;
    st_.skipped_mask = 0;
    if (rev_ < 2) st_.skipped_mask = st_.mask & kPd3Only;   // PD 2.0 has no Get_Source_Cap_Extended or Get_Manufacturer_Info
    pos_ = 0;
    seek(0);
}

// Make the next asked step that is not already skipped, from order position p, the current one; Done when there is none.
void Reread::seek(unsigned p) {
    while (p < kSteps && (!(st_.mask & reread_bit(step_at(p))) || (st_.skipped_mask & reread_bit(step_at(p))))) p++;
    pos_ = std::uint8_t(p);
    if (p >= kSteps) { st_.step = RereadStep::Done; st_.state = RereadState::Done; }
    else st_.step = step_at(p);
}

void Reread::advance() {
    awaiting_ = tx_pending_ = tx_sent_ = modes_loaded_ = false;
    seek(unsigned(pos_) + 1);
}

// The step ended. refused = the source said Not_Supported / Reject / NAK (silence and BUSY are not a refusal).
void Reread::step_done(bool ok, bool refused) {
    const std::uint8_t bit = reread_bit(st_.step);
    if (ok) st_.ok_mask |= bit;
    else {
        st_.skipped_mask |= bit;
        if (refused) st_.notsup_mask |= bit;
    }
    pace_until_ = now_ + opts_.pace_ms;
    advance();
}

// One Discover Modes question ended; the Modes step ends with the last SVID: one ACK among them is a Got, all refused is
// a refusal, anything else (silence, BUSY, a failed transmit) is a skip.
void Reread::query_done(Reply r) {
    if (r == Reply::Got) modes_got_++;
    else if (r == Reply::Refused) modes_refused_++;
    pace_until_ = now_ + opts_.pace_ms;
    awaiting_ = tx_pending_ = tx_sent_ = false;
    if (++modes_i_ < modes_n_) return;
    if (modes_got_) step_done(true);
    else step_done(false, modes_refused_ == modes_n_);
}

void Reread::build(const RereadLink& link) {
    TxMsg m;
    switch (st_.step) {
        case RereadStep::CapsExt: m.header = make_header(kCtlGetSourceCapExt, 0, rev_); break;
        case RereadStep::Identity:
        case RereadStep::Svids: {
            const std::uint32_t h = vdm_header(kPdSvid, st_.step == RereadStep::Identity ? kVdmDiscoverIdentity : kVdmDiscoverSvids, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case RereadStep::Modes: {
            if (!modes_loaded_) {   // the first kRereadModeSvids SVIDs of the source, the PD SID itself excluded
                modes_loaded_ = true;
                modes_n_ = modes_i_ = modes_got_ = modes_refused_ = 0;
                for (std::uint16_t s : link.svids)
                    if (s && s != kPdSvid && modes_n_ < kRereadModeSvids) modes_svid_[modes_n_++] = s;
            }
            if (!modes_n_) {   // nothing to ask about: no claim either way, and no pause for a message that is not sent
                st_.skipped_mask |= reread_bit(st_.step);
                advance();
                return;
            }
            const std::uint32_t h = vdm_header(modes_svid_[modes_i_], kVdmDiscoverModes, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case RereadStep::MfrInfo:   // unchunked extended message: ext header (2 data bytes), then target 0 (port), ref 0
            m.header = make_header(kExtGetManufacturerInfo, 1, rev_, true);
            m.len = 4;
            m.data[0] = 2;
            break;
        case RereadStep::GetCap:
        case RereadStep::GetCap2: m.header = make_header(kCtlGetSourceCap, 0, rev_); break;
        case RereadStep::Done: return;
    }
    tx_ = m;
    tx_pending_ = true;
}

void Reread::tick(std::uint32_t now, bool cc_idle, const RereadLink& link) {
    if (st_.state != RereadState::Running) return;
    now_ = now;
    if (st_.attaching) {
        if (link.contract) {
            if (!settling_) { settling_ = true; settle_until_ = now + kRereadSettleMs; }
            if (std::int32_t(now - settle_until_) < 0) return;
            st_.pdos_before = link.pdos;
            if (link.rev) rev_ = link.rev >= 2 ? 2 : 1;
            begin_steps(now);
        } else {
            settling_ = false;   // the contract went away: the settle starts over
            if (std::int32_t(now - started_) >= std::int32_t(kRereadAttachMs)) {
                st_.attaching = false;
                st_.attach_failed = true;
                st_.skipped_mask = st_.mask;
                st_.state = RereadState::Done;
                st_.step = RereadStep::Done;
            }
            return;
        }
    }
    if (std::int32_t(now - started_) >= std::int32_t(opts_.total_ms)) {   // out of time: what was not answered is skipped
        st_.skipped_mask = std::uint8_t(st_.mask & ~st_.ok_mask);
        st_.state = RereadState::Done;
        st_.step = RereadStep::Done;
        tx_pending_ = awaiting_ = false;
        return;
    }
    if (tx_pending_) return;
    if (awaiting_) {
        if (std::int32_t(now - deadline_) >= 0) no_answer();
        return;
    }
    if (!cc_idle) return;
    if (opts_.pace_ms && std::int32_t(now - pace_until_) < 0) return;   // the survey leaves a gap between two messages
    build(link);
}

void Reread::no_answer() {
    if (st_.step == RereadStep::Modes) query_done(Reply::Silent);
    else step_done(false);
}

bool Reread::take_tx(TxMsg& out) {
    if (!tx_pending_) return false;
    out = tx_;
    tx_pending_ = false;
    tx_sent_ = true;
    return true;
}

void Reread::on_tx_result(bool ok, std::uint32_t now) {
    if (st_.state != RereadState::Running || !tx_sent_) return;
    now_ = now;
    tx_sent_ = false;
    if (!ok) { no_answer(); return; }   // no GoodCRC: not retried
    awaiting_ = true;
    deadline_ = now + kRereadRespMs;
}

void Reread::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (st_.state != RereadState::Running || sop != Sop::Sop) return;
    now_ = now;
    const std::uint8_t type = header & 0x1F;
    const unsigned num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;
    // Every Source_Capabilities of the run counts, whichever step it answers: the charger may re-announce on its own.
    if (!ext && type == kDataSourceCap && num_do > 0) {
        st_.caps_rx++;
        st_.pdos_last = std::uint8_t(num_do);
        for (unsigned i = 0; i < num_do && 4 * i + 3 < payload.size(); i++) {
            const std::uint32_t w = payload[4 * i] | (payload[4 * i + 1] << 8) | (payload[4 * i + 2] << 16) | (std::uint32_t(payload[4 * i + 3]) << 24);
            if (is_spr_avs_pdo(w)) st_.avs_seen = true;
        }
    }
    if (!awaiting_) return;
    if (num_do == 0 && !ext) {
        const bool refusal = type == kCtlReject || type == kCtlNotSupported;
        if (!refusal && type != kCtlWait) return;
        if (st_.step == RereadStep::Modes) query_done(refusal ? Reply::Refused : Reply::Silent);
        else step_done(false, refusal);
        return;
    }
    switch (st_.step) {
        case RereadStep::CapsExt:
            if (ext && type == kExtSourceCapsExt) step_done(true);
            break;
        case RereadStep::MfrInfo:
            if (ext && type == kExtManufacturerInfo) step_done(true);
            break;
        case RereadStep::Identity:
        case RereadStep::Svids:
        case RereadStep::Modes: {
            if (ext || type != kMsgVendorDefined || payload.size() < 4 || !(payload[1] & 0x80)) return;
            const bool modes = st_.step == RereadStep::Modes;
            const std::uint8_t want = modes ? kVdmDiscoverModes : st_.step == RereadStep::Identity ? kVdmDiscoverIdentity : kVdmDiscoverSvids;
            if ((payload[0] & 0x1F) != want) return;
            // The Modes answer names the SVID it is for: only the one asked now counts.
            if (modes && std::uint16_t(payload[2] | (payload[3] << 8)) != modes_svid_[modes_i_]) return;
            const std::uint8_t ctype = (payload[0] >> 6) & 3;   // 0 is our own request echoed
            if (ctype == 1) { if (modes) query_done(Reply::Got); else step_done(true); }
            else if (ctype == 2) { if (modes) query_done(Reply::Refused); else step_done(false, true); }   // NAK
            else if (ctype == 3) { if (modes) query_done(Reply::Silent); else step_done(false); }          // BUSY
            break;
        }
        case RereadStep::GetCap:
        case RereadStep::GetCap2:
            if (!ext && type == kDataSourceCap) step_done(true);
            break;
        case RereadStep::Done: break;
    }
}

}  // namespace k2::pd
