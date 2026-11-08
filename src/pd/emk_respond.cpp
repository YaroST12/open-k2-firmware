// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emk_spoof.hpp"

namespace k2::emk_spoof {
namespace {

using emk_quirk::Kind;
using emk_quirk::Row;
using emk_quirk::Unmatched;

constexpr std::uint8_t kNotSupported = 16;     // control message (PD 3.x)
constexpr std::uint8_t kCmdAttention = 6;

// SOP' sync is SOP1 SOP1 SOP3 SOP3, SOP'' sync is SOP1 SOP3 SOP1 SOP3.
Frame make_frame(std::uint8_t sop, std::uint16_t header, std::span<const std::uint32_t> words) {
    Frame f;
    std::size_t n = 0;
    const std::uint8_t sync_p[4] = {0x12, 0x12, 0x1B, 0x1B}, sync_pp[4] = {0x12, 0x1B, 0x12, 0x1B};
    for (std::uint8_t b : sop == emk_quirk::kSopPP ? sync_pp : sync_p) f.t[n++] = b;
    f.t[n++] = static_cast<std::uint8_t>(0x80 | (2 + 4 * words.size()));
    f.t[n++] = static_cast<std::uint8_t>(header);
    f.t[n++] = static_cast<std::uint8_t>(header >> 8);
    for (std::uint32_t w : words)
        for (int i = 0; i < 4; i++) f.t[n++] = static_cast<std::uint8_t>(w >> (8 * i));
    f.t[n++] = 0xFF;   // JAM_CRC
    f.t[n++] = 0x14;   // EOP
    f.t[n++] = 0xFE;   // TXOFF
    f.n = static_cast<std::uint8_t>(n);
    return f;
}

// PD header from the cable: the identity's spec revision, cable-plug bit, MessageID, object count.
std::uint16_t pd_header(const emrec::Record& rec, std::uint8_t type, std::uint8_t id, std::size_t nobj) {
    return static_cast<std::uint16_t>(type | ((rec[0x14] & 3u) << 6) | (1u << 8) | ((id & 7u) << 9) | (nobj << 12));
}

// The reply words of a row for the request header `req`; returns the count.
std::size_t row_words(const Row& r, std::uint32_t req, std::array<std::uint32_t, emk_quirk::kMaxWords + 1>& w) {
    if (r.kind == Kind::Raw) {
        for (std::size_t i = 0; i < r.nrep; i++) w[i] = r.rep[i];
        return r.nrep;
    }
    const std::uint32_t type = r.kind == Kind::Ack ? 1 : r.kind == Kind::Nak ? 2 : 3;
    w[0] = (req & 0xFFFFFF1Fu) | (type << 6);   // SVID, version and object position of the request, our type
    if (r.kind != Kind::Ack) return 1;
    for (std::size_t i = 0; i < r.nrep; i++) w[1 + i] = r.rep[i];
    return 1u + r.nrep;
}

bool row_matches(const Row& r, std::uint8_t sop, std::span<const std::uint32_t> words) {
    if ((r.sop & sop) == 0 || (words[0] & r.hdr_mask) != r.hdr_val) return false;
    if (words.size() < 1u + r.nreq) return false;
    for (unsigned k = 0; k < r.nreq; k++)
        if ((words[1 + k] & r.req_mask[k]) != r.req_val[k]) return false;
    return true;
}

}  // namespace

Outcome respond(const emrec::Record& rec, const emk_quirk::Quirks& quirks, std::uint8_t sop, std::uint16_t header,
                std::span<const std::uint8_t> payload, std::uint8_t msg_id) {
    Outcome o;
    const bool echo = (quirks.flags & emk_quirk::kQMsgIdEcho) != 0;
    const std::uint8_t id = echo ? static_cast<std::uint8_t>((header >> 9) & 7) : static_cast<std::uint8_t>(msg_id & 7);
    o.counter = !echo;

    std::array<std::uint32_t, 7> words{};
    const std::size_t n = payload.size() / 4 > words.size() ? words.size() : payload.size() / 4;
    for (std::size_t i = 0; i < n; i++)
        words[i] = payload[4 * i] | (payload[4 * i + 1] << 8) | (payload[4 * i + 2] << 16) | (std::uint32_t(payload[4 * i + 3]) << 24);

    const bool ext = (header & 0x8000) != 0;
    const std::uint8_t type = header & 0x1F;
    if (ext || type != kMsgVendorDefined || n < 1) {   // not a VDM: nothing answers it (a GoodCRC is no request)
        o.unmatched = !(!ext && type == 1 && n == 0);
        return o;
    }
    const std::span<const std::uint32_t> w(words.data(), n);

    const unsigned rows = quirks.count > emk_quirk::kMaxRows ? emk_quirk::kMaxRows : quirks.count;
    for (unsigned i = 0; i < rows; i++) {
        const Row& r = quirks.rows[i];
        if (!row_matches(r, sop, w)) continue;
        std::array<std::uint32_t, emk_quirk::kMaxWords + 1> out{};
        const std::size_t cnt = row_words(r, w[0], out);
        o.how = Match::Row;
        o.row = static_cast<std::uint8_t>(i);
        o.frame = make_frame(sop, pd_header(rec, kMsgVendorDefined, id, cnt), {out.data(), cnt});
        return o;
    }

    if (is_discover_identity_req(header, payload) && (sop == emk_quirk::kSopP || (quirks.flags & emk_quirk::kQSopPP) != 0)) {
        std::array<std::uint32_t, 5> v;
        for (unsigned i = 0; i < 5; i++) v[i] = emrec::vdo(rec, i);
        o.how = Match::Identity;
        o.frame = make_frame(sop, reply_header(rec, id), v);
        return o;
    }

    // A structured request (command type REQ) nobody knows: the identity's default. Attention never gets an answer.
    const bool structured_req = (w[0] & 0x8000) != 0 && ((w[0] >> 6) & 3) == 0 && (w[0] & 0x1F) != kCmdAttention;
    o.unmatched = true;
    if (!structured_req) return o;
    if (quirks.unmatched == Unmatched::Nak) {
        const std::uint32_t nak = (w[0] & 0xFFFFFF1Fu) | (2u << 6);
        o.how = Match::Policy;
        o.frame = make_frame(sop, pd_header(rec, kMsgVendorDefined, id, 1), {&nak, 1});
    } else if (quirks.unmatched == Unmatched::NotSupported && (rec[0x14] & 3) == 2) {
        o.how = Match::Policy;
        o.frame = make_frame(sop, pd_header(rec, kNotSupported, id, 0), {});
    }
    return o;
}

}  // namespace k2::emk_spoof
