// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/pd_engine.hpp"

#include <algorithm>

namespace k2::pd {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

// A lone PDO with one of these halfwords is receiver noise, not a charger.
bool is_noise_frame(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (((header >> 12) & 7) != 1 || payload.size() < 4) return false;
    const std::uint32_t pdo = le32(payload.data());
    for (const std::uint16_t n : {0x639C, 0xC7CE, 0x27F6})
        if ((pdo & 0xFFFF) == n || (pdo >> 16) == n) return true;
    return false;
}

}  // namespace

ExtHeader decode_ext_header(std::uint16_t raw) {
    ExtHeader h;
    h.data_size = raw & 0x1FF;
    h.request = (raw >> 10) & 1;
    h.chunk_number = (raw >> 11) & 0xF;
    h.chunked = (raw >> 15) & 1;
    return h;
}

void Engine::on_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload,
                        std::uint8_t cc) {
    usb.on_message(static_cast<usblink::Sop>(sop), header, payload);
    if (sop != Sop::Sop) {
        on_cable_message(sop, header, payload);
        return;
    }
    const bool ext = (header >> 15) & 1;
    const std::uint8_t type = header & 0x1F;
    if (!ext && type == 0x01 && ((header >> 12) & 7) > 0) {
        if (is_noise_frame(header, payload)) return;
        decode_source_caps(caps, header, payload, cc);
        epr_total_ = 0;  // a plain Source_Capabilities supersedes any EPR message
        epr_mask_ = 0;
    } else if (ext && type == kExtEprSourceCaps) {
        on_epr_caps_chunk(header, payload, cc);
    } else if (ext) {
        on_ext_message(type, payload);
    } else if (!ext && (type == 0x02 || type == 0x09) && ((header >> 12) & 7) > 0) {
        on_request(header, payload);
    } else if (!ext && type == 0x04 && ((header >> 12) & 7) > 0 && !((header >> 8) & 1)) {  // a sink's Sink_Capabilities
        const unsigned n = std::min<unsigned>((header >> 12) & 7, 7);
        sink.valid = payload.size() >= n * 4;
        sink.n = sink.valid ? std::uint8_t(n) : 0;
        for (unsigned i = 0; sink.valid && i < n; i++) sink.raw[i] = le32(payload.data() + 4 * i);
    } else if (!ext && type == kMsgVendorDefined) {
        parse_vdm((header >> 8) & 1 ? vdm : sink_vdm, payload);  // bit 8 on SOP: the sender's power role
    } else if (!ext && ((header >> 12) & 7) == 0) {  // a control message on SOP
        on_sop_control(type);
    } else if (type == kDataRevision && ((header >> 12) & 7) == 1 &&
               (header & (1u << 8)) && payload.size() >= 4) {
        parse_revision(payload.first(4), this->ext.revision);
    }
}

// Control messages on SOP. Besides the Request answer tracking, a PR_Swap (type 0x0A, sent by either
// partner) is followed to its completion: Accept + PS_RDY exchange the power roles, so the per-side
// VDM records swap with them. Reject / Wait cancel it, a PS_RDY without Accept does not complete it.
void Engine::on_sop_control(std::uint8_t type) {
    if (type == 0x0A) {  // PR_Swap
        pr_swap_pending_ = true;
        pr_swap_accepted_ = false;
        return;
    }
    if (type == 0x03) {  // Accept
        if (pr_swap_pending_) pr_swap_accepted_ = true;
        if (contract.valid) { contract.accepted = true; contract.rejected = false; }
        return;
    }
    if (type == 0x04 || type == 0x0C) {  // Reject / Wait
        pr_swap_pending_ = pr_swap_accepted_ = false;
        if (contract.valid) { contract.rejected = true; contract.accepted = false; }
        return;
    }
    if (type == 0x06) {  // PS_RDY
        if (pr_swap_pending_ && pr_swap_accepted_) {
            std::swap(vdm, sink_vdm);  // the power roles just exchanged; the data roles did not
            pr_swap_pending_ = pr_swap_accepted_ = false;
        }
        if (contract.valid) contract.ready = true;
    }
}

void Engine::reset_source() {
    const std::uint32_t rx = caps.rx_count, req = contract.requests;
    caps = {};
    epr_info = {};
    vdm = {};
    sink_vdm = {};
    ext = {};
    usb.on_reset();
    contract = {};
    caps.rx_count = rx;
    contract.requests = req;
    epr_total_ = 0;
    epr_mask_ = 0;
    pr_swap_pending_ = pr_swap_accepted_ = false;
}

// A Request (type 2, or EPR_Request type 9 with the RDO first) from a sink: power role bit 8 clear.
void Engine::on_request(std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (((header >> 8) & 1) || payload.size() < 4) return;
    const std::uint32_t rdo = le32(payload.data());
    const unsigned pos = rdo >> 28;
    if (pos == 0) return;
    contract.valid = true;
    contract.ready = false;
    contract.accepted = false;
    contract.rejected = false;
    contract.pos = std::uint8_t(pos);
    contract.rdo = rdo;
    contract.requests++;
    const Pdo* p = pos <= caps.pdos.size() && caps.valid ? &caps.pdos[pos - 1] : nullptr;
    const Rdo d = decode_rdo(rdo, p);
    contract.mv = d.mv;
    contract.op_ma = d.op_ma;
    contract.max_ma = d.max_ma;
    contract.flags = d.flags;
}

bool Engine::cable_thunderbolt() const {
    for (std::size_t i = 0; i < cable_vdm.num_svids; i++)
        if (cable_vdm.svids[i] == 0x8087) return true;
    return false;
}

// SOP' / SOP'' traffic: only VDM ACKs are kept (a cable never sends anything else we decode,
// and the source's REQs to the cable are not ours to answer).
void Engine::on_cable_message(Sop sop, std::uint16_t header, std::span<const std::uint8_t> payload) {
    if (((header >> 15) & 1) || (header & 0x1F) != kMsgVendorDefined) return;
    const unsigned ndo = std::min<std::size_t>((header >> 12) & 7, payload.size() / 4);
    if (ndo == 0) return;
    const std::uint32_t vh = std::uint32_t(payload[0]) | (std::uint32_t(payload[1]) << 8) |
                             (std::uint32_t(payload[2]) << 16) | (std::uint32_t(payload[3]) << 24);
    if (((vh >> 6) & 3) != 1) return;  // ACK only
    if ((vh >> 16) == 0x05AC) {  // Apple cable metadata chunk
        if (apple.chunks < AppleCable::kChunks && ndo >= 2) {
            std::uint8_t* dst = &apple.bytes[apple.chunks * 24];
            const std::size_t n = std::min<std::size_t>((ndo - 1) * 4, 24);
            for (std::size_t i = 0; i < n; i++) dst[i] = payload[4 + i];
            apple.chunks++;
        }
        return;
    }
    if (!((vh >> 15) & 1)) return;  // the rest is structured
    switch (vh & 0x1F) {
        case 1:
            cable = parse_emarker(payload, ndo);
            cable_acks++;
            cable.sop = sop == Sop::SopP ? 1 : 2;
            cable.spec_rev = (header >> 6) & 3;
            break;
        case 2:
        case 3: parse_vdm(cable_vdm, payload); break;
        default: break;
    }
}

// Small extended messages: only the first chunk is decoded (a block longer than one chunk,
// such as a full Manufacturer_Info, is decoded from what chunk 0 carries).
void Engine::on_ext_message(std::uint8_t type, std::span<const std::uint8_t> payload) {
    if (payload.size() < 2) return;
    const ExtHeader eh = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (eh.request || (eh.chunked && eh.chunk_number != 0)) return;
    const auto data = payload.subspan(2, std::min<std::size_t>(payload.size() - 2, std::min<std::size_t>(eh.data_size, kChunkBytes)));
    switch (type) {
        case kExtSourceCapsExt: parse_source_caps_ext(data, ext.caps_ext); break;
        case kExtStatus: parse_status(data, ext.status); break;
        case kExtBatteryCaps: parse_battery_caps(data, ext.battery); break;
        case kExtManufacturerInfo: parse_manufacturer_info(data, ext.mfr); break;
        case kExtPpsStatus: parse_pps_status(data, ext.pps); break;
        default: break;
    }
}

// EPR_Source_Capabilities is a stream of up to 11 PDOs split into 26-byte chunks; chunk 0
// holds PDOs 1..6 and half of the 7th. Chunk 0 starts a message, later chunks are placed
// by number and only counted when a message is in progress.
void Engine::on_epr_caps_chunk(std::uint16_t header, std::span<const std::uint8_t> payload,
                               std::uint8_t cc) {
    if (payload.size() < 2) return;
    const ExtHeader eh = decode_ext_header(std::uint16_t(payload[0] | (payload[1] << 8)));
    if (eh.request) return;  // a sink asking for a chunk: no data in it
    const std::size_t chunk = eh.chunked ? eh.chunk_number : 0;
    if (chunk * kChunkBytes >= epr_buf_.size()) return;
    SourceCaps& dst = epr_info_only ? epr_info : caps;   // an asked-for SPR-mode list never replaces the live one
    if (chunk == 0) {
        epr_total_ = std::min<std::size_t>(eh.data_size, epr_buf_.size());
        epr_mask_ = 0;
        epr_buf_.fill(0);
        dst.rx_count++;
    }
    if (epr_total_ == 0) return;  // no chunk 0 yet
    const std::size_t off = chunk * kChunkBytes;
    const std::size_t n = std::min({payload.size() - 2, kChunkBytes, std::size_t(epr_total_) - std::min<std::size_t>(off, epr_total_)});
    std::copy_n(payload.begin() + 2, n, epr_buf_.begin() + off);
    epr_mask_ |= std::uint8_t(1u << chunk);

    const std::size_t nchunks = (epr_total_ + kChunkBytes - 1) / kChunkBytes;
    const bool complete = epr_mask_ == ((1u << nchunks) - 1);
    // Bytes usable from the start: whole chunks in sequence, the last one possibly short.
    std::size_t have = 0;
    for (std::size_t c = 0; c < nchunks && (epr_mask_ >> c & 1); c++)
        have = std::min<std::size_t>(epr_total_, (c + 1) * kChunkBytes);
    const std::size_t whole = have / 4;

    dst.pdos = {};
    for (std::size_t i = 0; i < whole; i++) dst.pdos[i] = decode_pdo(le32(&epr_buf_[4 * i]));
    dst.valid = true;
    dst.epr = true;
    dst.epr_partial = !complete;
    dst.num_pdos = std::uint8_t(whole);
    dst.active_cc = cc;
    dst.spec_rev = (header >> 6) & 3;
    dst.raw_header = header;
}

}  // namespace k2::pd
