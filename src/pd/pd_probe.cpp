// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_probe.hpp"

#include <algorithm>

namespace k2::pd {

namespace {
constexpr std::uint16_t kPdSvid = 0xFF00;
constexpr std::uint8_t kVdmDiscoverIdentity = 1;
}  // namespace

void Probe::start(std::uint8_t rev, std::uint32_t now) {
    st_ = {};
    st_.state = ProbeState::Running;
    rev_ = rev;
    started_ = now;
    awaiting_ = tx_pending_ = tx_sent_ = false;
    if (rev < 2) st_.skipped_mask = probe_bit(ProbeStep::MfrInfo) | probe_bit(ProbeStep::CapsExt);   // PD 2.0 has neither
}

void Probe::step_done(bool ok) {
    (ok ? st_.ok_mask : st_.skipped_mask) |= probe_bit(st_.step);
    awaiting_ = tx_pending_ = tx_sent_ = false;
    do st_.step = ProbeStep(unsigned(st_.step) + 1);
    while (st_.step != ProbeStep::Done && (st_.skipped_mask & probe_bit(st_.step)));
    if (st_.step == ProbeStep::Done) st_.state = ProbeState::Done;
}

void Probe::build() {
    TxMsg m;
    switch (st_.step) {
        case ProbeStep::Identity: {
            const std::uint32_t h = vdm_header(kPdSvid, kVdmDiscoverIdentity, rev_);
            m.header = make_header(kMsgVendorDefined, 1, rev_);
            m.len = 4;
            for (int i = 0; i < 4; i++) m.data[i] = std::uint8_t(h >> (8 * i));
            break;
        }
        case ProbeStep::MfrInfo:   // unchunked extended message: ext header (2 data bytes), then target 0 (port), ref 0
            m.header = make_header(kExtGetManufacturerInfo, 1, rev_, true);
            m.len = 4;
            m.data[0] = 2;
            break;
        case ProbeStep::CapsExt: m.header = make_header(kCtlGetSourceCapExt, 0, rev_); break;
        case ProbeStep::Done: return;
    }
    tx_ = m;
    tx_pending_ = true;
}

void Probe::tick(std::uint32_t now, bool cc_idle) {
    if (st_.state != ProbeState::Running) return;
    if (std::int32_t(now - started_) >= std::int32_t(kProbeTotalMs)) {   // out of time: what was not answered is skipped
        for (auto s : {ProbeStep::Identity, ProbeStep::MfrInfo, ProbeStep::CapsExt})
            if (!(st_.ok_mask & probe_bit(s))) st_.skipped_mask |= probe_bit(s);
        st_.state = ProbeState::Done; st_.step = ProbeStep::Done; tx_pending_ = awaiting_ = false;
        return;
    }
    if (tx_pending_) return;
    if (awaiting_) {
        if (std::int32_t(now - deadline_) >= 0) step_done(false);   // silence: the source ignores it
        return;
    }
    if (cc_idle) build();
}

bool Probe::take_tx(TxMsg& out) {
    if (!tx_pending_) return false;
    out = tx_;
    tx_pending_ = false;
    tx_sent_ = true;
    return true;
}

void Probe::on_tx_result(bool ok, std::uint32_t now) {
    if (st_.state != ProbeState::Running || !tx_sent_) return;
    tx_sent_ = false;
    if (!ok) { step_done(false); return; }   // no GoodCRC: not retried
    awaiting_ = true;
    deadline_ = now + kProbeRespMs;
}

void Probe::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload, std::uint32_t) {
    if (st_.state != ProbeState::Running || !awaiting_ || sop != Sop::Sop) return;
    const std::uint8_t type = header & 0x1F;
    const unsigned num_do = (header >> 12) & 7;
    const bool ext = (header >> 15) & 1;
    if (num_do == 0 && !ext) {
        if (type == kCtlReject || type == kCtlNotSupported || type == kCtlWait) step_done(false);
        return;
    }
    switch (st_.step) {
        case ProbeStep::Identity: {
            if (ext || type != kMsgVendorDefined || payload.size() < 4 || !(payload[1] & 0x80)) return;
            if ((payload[0] & 0x1F) != kVdmDiscoverIdentity) return;
            const std::uint8_t ctype = (payload[0] >> 6) & 3;   // 0 is our own request echoed
            if (ctype == 1) step_done(true);
            else if (ctype != 0) step_done(false);              // NAK or BUSY
            break;
        }
        case ProbeStep::MfrInfo:
            if (ext && type == kExtManufacturerInfo) step_done(true);
            break;
        case ProbeStep::CapsExt:
            if (ext && type == kExtSourceCapsExt) step_done(true);
            break;
        case ProbeStep::Done: break;
    }
}

// All-ones / zero are "no vendor" placeholders (an Anker Prime A2343 answers FFFF:FFFF): never an identity.
static bool real_vid(std::uint16_t v) { return v != 0 && v != 0xFFFF; }

ProbeResult probe_result(const ProbeStatus& st, const VdmInfo& vdm, const ExtInfo& ext, const SourceCaps& caps) {
    ProbeResult r;
    const bool mfr = (st.ok_mask & probe_bit(ProbeStep::MfrInfo)) && ext.mfr.valid;
    const bool cex = (st.ok_mask & probe_bit(ProbeStep::CapsExt)) && ext.caps_ext.valid;
    if (vdm.identity.valid && real_vid(vdm.identity.vid)) { r.vid = vdm.identity.vid; r.pid = vdm.identity.pid; }
    else if (mfr && real_vid(ext.mfr.vid)) { r.vid = ext.mfr.vid; r.pid = ext.mfr.pid; }
    else if (cex && real_vid(ext.caps_ext.vid)) { r.vid = ext.caps_ext.vid; r.pid = ext.caps_ext.pid; }
    if (mfr) {
        unsigned n = 0;
        for (unsigned i = 0; i < sizeof ext.mfr.name && n < sizeof r.mfr - 1 && ext.mfr.name[i]; ++i) {
            const char c = ext.mfr.name[i];
            if (c >= 0x20 && c < 0x7F) r.mfr[n++] = c;
        }
        while (n && r.mfr[n - 1] == ' ') r.mfr[--n] = 0;
    }
    if (cex && ext.caps_ext.pdp_w) r.pdp_w = ext.caps_ext.pdp_w;
    else if (caps.valid) {
        std::uint32_t mw = 0;
        for (unsigned i = 0; i < caps.num_pdos && i < caps.pdos.size(); ++i) mw = std::max(mw, caps.pdos[i].max_mw);
        r.pdp_w = std::uint8_t(std::min<std::uint32_t>((mw + 500) / 1000, 255));
    }
    return r;
}

}  // namespace k2::pd
