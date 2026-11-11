// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_identify.hpp"

#include <algorithm>

namespace k2::pd {

namespace {
constexpr std::uint8_t bit(IdStep s) { return std::uint8_t(1u << unsigned(s)); }
constexpr std::uint8_t kVdmDiscoverIdentity = 1, kVdmDiscoverSvids = 2, kVdmDiscoverModes = 3;
constexpr std::uint16_t kPdSvid = 0xFF00, kSvidDp = 0xFF01, kSvidTbt = 0x8087;
}  // namespace

std::uint16_t id_modes_svid(std::span<const std::uint16_t> svids, unsigned idx) {
    unsigned at = 0;
    for (const std::uint16_t want : {kSvidDp, kSvidTbt})
        if (std::find(svids.begin(), svids.end(), want) != svids.end() && at++ == idx) return want;
    for (const std::uint16_t s : svids) {
        if (s == kSvidDp || s == kSvidTbt) continue;
        if (at++ == idx) return s;
    }
    return 0;
}

const char* id_step_short(IdStep s) {
    switch (s) {
        case IdStep::Identity: return "Identity";
        case IdStep::Svids: return "SVIDs";
        case IdStep::Modes: return "Modes";
        case IdStep::SinkCap: return "Sink caps";
        case IdStep::Status: return "Status";
        case IdStep::Revision: return "Revision";
        case IdStep::Done: return "done";
    }
    return "?";
}

const char* id_step_name(IdStep s) {
    switch (s) {
        case IdStep::Identity: return "Discover Identity";
        case IdStep::Svids: return "Discover SVIDs";
        case IdStep::Modes: return "Discover Modes";
        case IdStep::SinkCap: return "Get_Sink_Cap";
        case IdStep::Status: return "Get_Status";
        case IdStep::Revision: return "Get_Revision";
        case IdStep::Done: return "done";
    }
    return "?";
}

void Identify::start(std::uint8_t rev, std::uint32_t now) {
    st_ = IdStatus{};
    st_.state = IdState::Running;
    rev_ = rev >= 2 ? 2 : 1;
    mode_idx_ = 0;
    attempts_ = 0;
    awaiting_ = tx_pending_ = false;
    next_at_ = now;
    // The first step is Identity; Status and Revision do not exist in PD 2.0.
}

void Identify::advance() {
    attempts_ = 0;
    awaiting_ = false;
    tx_pending_ = false;
    if (st_.step == IdStep::Modes) {
        mode_idx_++;  // stays on Modes until every wanted SVID has been asked (checked in build)
        return;
    }
    st_.step = IdStep(unsigned(st_.step) + 1);
    if (rev_ < 2) {
        if (st_.step == IdStep::Status) st_.step = IdStep::Done;
    }
}

void Identify::step_ok() {
    if (st_.step != IdStep::Modes) st_.ok_mask |= bit(st_.step);
    advance();
}

void Identify::step_skipped() {
    st_.skipped_mask |= bit(st_.step);
    if (st_.step == IdStep::Modes) mode_idx_ = kIdModeSvids;  // give up on the rest of the modes
    advance();
}

void Identify::attempt_failed(std::uint32_t now) {
    awaiting_ = false;
    tx_pending_ = false;
    if (++attempts_ >= kIdMaxAttempts) {
        step_skipped();
        next_at_ = now;
        return;
    }
    st_.retries++;
    next_at_ = now + kIdBackoffMs * attempts_;
}

bool Identify::build(std::span<const std::uint16_t> svids) {
    TxMsg m;
    switch (st_.step) {
        case IdStep::Identity: {
            const std::uint32_t h = vdm_header(kPdSvid, kVdmDiscoverIdentity, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case IdStep::Svids: {
            const std::uint32_t h = vdm_header(kPdSvid, kVdmDiscoverSvids, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case IdStep::Modes: {
            const unsigned want = std::min<unsigned>(svids.size(), kIdModeSvids);
            // SVID 0xFF01 (DisplayPort) and friends are asked like any other.
            if (mode_idx_ >= want) {  // every wanted SVID asked: leave the Modes step
                if (want == 0) st_.skipped_mask |= bit(IdStep::Modes);
                else if (!(st_.skipped_mask & bit(IdStep::Modes))) st_.ok_mask |= bit(IdStep::Modes);
                st_.step = IdStep::SinkCap;
                attempts_ = 0;
                return false;
            }
            cur_svid_ = id_modes_svid(svids, mode_idx_);
            const std::uint32_t h = vdm_header(cur_svid_, kVdmDiscoverModes, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case IdStep::SinkCap: m.header = make_header(kCtlGetSinkCap, 0, rev_); break;
        case IdStep::Status: m.header = make_header(kCtlGetStatus, 0, rev_); break;
        case IdStep::Revision: m.header = make_header(kCtlGetRevision, 0, rev_); break;
        case IdStep::Done: return false;
    }
    tx_ = m;
    tx_pending_ = true;
    return true;
}

void Identify::tick(std::uint32_t now, bool cc_idle, std::span<const std::uint16_t> svids) {
    if (st_.state != IdState::Running) return;
    if (tx_pending_) return;
    auto due = [now](std::uint32_t t) { return std::int32_t(now - t) >= 0; };
    if (awaiting_) {
        if (due(deadline_)) attempt_failed(now);  // silence: a collision or a deaf partner
        return;
    }
    for (int guard = 0; guard < 8 && st_.state == IdState::Running; guard++) {
        if (st_.step == IdStep::Done) { st_.state = IdState::Done; return; }
        if (!due(next_at_) || !cc_idle) return;
        if (build(svids)) return;  // a message is ready to send
    }
}

bool Identify::take_tx(TxMsg& out) {
    if (!tx_pending_) return false;
    out = tx_;
    tx_pending_ = false;
    tx_sent_ = true;
    return true;
}

void Identify::on_tx_result(bool ok, std::uint32_t now) {
    if (st_.state != IdState::Running || !tx_sent_) return;
    tx_sent_ = false;
    if (!ok) { attempt_failed(now); return; }
    awaiting_ = true;
    deadline_ = now + kIdRespMs;
}

void Identify::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t now) {
    if (st_.state != IdState::Running || !awaiting_ || sop != Sop::Sop) return;
    const std::uint8_t type = header & 0x1F;
    const unsigned num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;

    if (num_do == 0 && !ext) {
        if (type == kCtlReject || type == kCtlNotSupported) step_skipped();
        else if (type == kCtlWait) attempt_failed(now);
        return;
    }
    switch (st_.step) {
        case IdStep::Identity: case IdStep::Svids: case IdStep::Modes: {
            if (ext || type != kMsgVendorDefined || payload.size() < 4) return;
            if (!(payload[1] & 0x80)) return;  // unstructured: not an answer
            const std::uint8_t cmd = payload[0] & 0x1F, ctype = (payload[0] >> 6) & 3;
            const std::uint8_t want = st_.step == IdStep::Identity ? kVdmDiscoverIdentity
                                    : st_.step == IdStep::Svids ? kVdmDiscoverSvids : kVdmDiscoverModes;
            if (cmd != want || ctype == 0) return;  // our own request echoed or someone else's command
            if (st_.step == IdStep::Modes && std::uint16_t(payload[2] | (payload[3] << 8)) != cur_svid_) return;
            if (ctype == 1) {
                if (st_.step == IdStep::Modes && payload.size() >= 8 && st_.num_modes < kIdModeSvids) {
                    st_.mode_svid[st_.num_modes] = cur_svid_;
                    st_.mode_vdo[st_.num_modes++] = payload[4] | (payload[5] << 8) | (payload[6] << 16) | (std::uint32_t(payload[7]) << 24);
                }
                step_ok();
            } else if (ctype == 2) step_skipped();
            else attempt_failed(now);  // BUSY
            break;
        }
        case IdStep::SinkCap:
            if (!ext && type == kDataSinkCap) {
                st_.num_sink_pdos = std::uint8_t(std::min<unsigned>(std::min<unsigned>(num_do, 7), payload.size() / 4));
                for (unsigned i = 0; i < st_.num_sink_pdos; i++)
                    st_.sink_pdos[i] = payload[4 * i] | (payload[4 * i + 1] << 8) | (payload[4 * i + 2] << 16) |
                                       (std::uint32_t(payload[4 * i + 3]) << 24);
                step_ok();
            }
            break;
        case IdStep::Status:
            if (ext && type == kExtStatusMsg) step_ok();
            break;
        case IdStep::Revision:
            if (!ext && type == kDataRevision && payload.size() >= 4) {
                st_.revision = payload[0] | (payload[1] << 8) | (payload[2] << 16) | (std::uint32_t(payload[3]) << 24);
                step_ok();
            }
            break;
        case IdStep::Done: break;
    }
}

}  // namespace k2::pd
