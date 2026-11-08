// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_reread.hpp"

#include "k2/pd/pd_identify.hpp"   // vdm_header

namespace k2::pd {

namespace {
constexpr std::uint16_t kPdSvid = 0xFF00;
constexpr std::uint8_t kVdmDiscoverIdentity = 1, kVdmDiscoverSvids = 2;
constexpr unsigned kSteps = unsigned(RereadStep::Done);
}  // namespace

void Reread::start(std::uint8_t rev, std::uint8_t mask, std::uint8_t pdos_before, std::uint32_t now, bool attach) {
    st_ = {};
    st_.state = RereadState::Running;
    st_.mask = mask & kRereadMaskAll;
    st_.pdos_before = pdos_before;
    rev_ = rev >= 2 ? 2 : 1;
    started_ = now;   // with attach: the start of the wait for the contract
    awaiting_ = tx_pending_ = tx_sent_ = settling_ = false;
    st_.step = RereadStep(0);
    if (attach) { st_.attaching = true; return; }
    begin_steps(now);
}

// The first asked step; the run's step budget starts here.
void Reread::begin_steps(std::uint32_t now) {
    st_.attaching = false;
    settling_ = false;
    started_ = now;
    st_.step = RereadStep(0);
    st_.skipped_mask = 0;
    if (rev_ < 2 && (st_.mask & kRereadCapsExt)) st_.skipped_mask = kRereadCapsExt;   // PD 2.0 has no Get_Source_Cap_Extended
    if (!(st_.mask & reread_bit(st_.step)) || (st_.skipped_mask & reread_bit(st_.step))) advance();
}

// Next asked step that is not already skipped; Done when there is none.
void Reread::advance() {
    awaiting_ = tx_pending_ = tx_sent_ = false;
    unsigned s = unsigned(st_.step);
    do s++;
    while (s < kSteps && (!(st_.mask & (1u << s)) || (st_.skipped_mask & (1u << s))));
    st_.step = RereadStep(s);
    if (s >= kSteps) st_.state = RereadState::Done;
}

void Reread::step_done(bool ok) {
    (ok ? st_.ok_mask : st_.skipped_mask) |= reread_bit(st_.step);
    advance();
}

void Reread::build() {
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
        case RereadStep::GetCap:
        case RereadStep::GetCap2: m.header = make_header(kCtlGetSourceCap, 0, rev_); break;
        case RereadStep::Done: return;
    }
    tx_ = m;
    tx_pending_ = true;
}

void Reread::tick(std::uint32_t now, bool cc_idle, const RereadLink& link) {
    if (st_.state != RereadState::Running) return;
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
    if (std::int32_t(now - started_) >= std::int32_t(kRereadTotalMs)) {   // out of time: what was not answered is skipped
        st_.skipped_mask = std::uint8_t(st_.mask & ~st_.ok_mask);
        st_.state = RereadState::Done;
        st_.step = RereadStep::Done;
        tx_pending_ = awaiting_ = false;
        return;
    }
    if (tx_pending_) return;
    if (awaiting_) {
        if (std::int32_t(now - deadline_) >= 0) step_done(false);   // silence
        return;
    }
    if (cc_idle) build();
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
    tx_sent_ = false;
    if (!ok) { step_done(false); return; }   // no GoodCRC: not retried
    awaiting_ = true;
    deadline_ = now + kRereadRespMs;
}

void Reread::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t) {
    if (st_.state != RereadState::Running || sop != Sop::Sop) return;
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
        if (type == kCtlReject || type == kCtlNotSupported || type == kCtlWait) step_done(false);
        return;
    }
    switch (st_.step) {
        case RereadStep::CapsExt:
            if (ext && type == kExtSourceCapsExt) step_done(true);
            break;
        case RereadStep::Identity:
        case RereadStep::Svids: {
            if (ext || type != kMsgVendorDefined || payload.size() < 4 || !(payload[1] & 0x80)) return;
            const std::uint8_t want = st_.step == RereadStep::Identity ? kVdmDiscoverIdentity : kVdmDiscoverSvids;
            if ((payload[0] & 0x1F) != want) return;
            const std::uint8_t ctype = (payload[0] >> 6) & 3;   // 0 is our own request echoed
            if (ctype == 1) step_done(true);
            else if (ctype != 0) step_done(false);              // NAK or BUSY
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
