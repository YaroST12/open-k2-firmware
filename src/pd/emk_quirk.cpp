// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/pd/emk_quirk.hpp"

#include "k2/pd/emrec.hpp"

namespace k2::emk_quirk {
namespace {

void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

bool row_valid(const Row& r) {
    if ((r.sop & ~(kSopP | kSopPP)) != 0 || r.sop == 0) return false;
    if (static_cast<std::uint8_t>(r.kind) > static_cast<std::uint8_t>(Kind::Raw)) return false;
    if (r.nreq > kMaxReqVdos || r.nrep > kMaxWords) return false;
    return r.kind != Kind::Raw || r.nrep >= 1;
}

// Built-in identities with extra behaviour (none yet); every other id answers Discover Identity only.
constexpr Quirks kNone{};

}  // namespace

std::size_t encode(const Quirks& q, std::span<std::uint8_t> out) {
    const unsigned count = q.count > kMaxRows ? kMaxRows : q.count;
    const std::size_t need = block_size(count);
    if (out.size() < need) return 0;
    out[0] = q.flags;
    out[1] = static_cast<std::uint8_t>(q.unmatched);
    out[2] = static_cast<std::uint8_t>(count);
    for (unsigned i = 0; i < count; i++) {
        const Row& r = q.rows[i];
        std::uint8_t* p = &out[kBlockHeader + i * kRowBytes];
        p[0] = r.sop;
        p[1] = static_cast<std::uint8_t>(r.kind);
        p[2] = r.nreq;
        p[3] = r.nrep;
        put32(p + 4, r.hdr_val);
        put32(p + 8, r.hdr_mask);
        for (unsigned k = 0; k < kMaxReqVdos; k++) {
            put32(p + 12 + 4 * k, r.req_val[k]);
            put32(p + 20 + 4 * k, r.req_mask[k]);
        }
        for (unsigned k = 0; k < kMaxWords; k++) put32(p + 28 + 4 * k, r.rep[k]);
    }
    return need;
}

bool decode(std::span<const std::uint8_t> raw, Quirks& out) {
    if (raw.size() < kBlockHeader || raw[2] > kMaxRows || raw.size() != block_size(raw[2])) return false;
    if ((raw[0] & ~kQFlagsKnown) != 0 || raw[1] > static_cast<std::uint8_t>(Unmatched::NotSupported)) return false;
    Quirks q;
    q.flags = raw[0];
    q.unmatched = static_cast<Unmatched>(raw[1]);
    q.count = raw[2];
    for (unsigned i = 0; i < q.count; i++) {
        const std::uint8_t* p = &raw[kBlockHeader + i * kRowBytes];
        Row& r = q.rows[i];
        r.sop = p[0];
        r.kind = static_cast<Kind>(p[1]);
        r.nreq = p[2];
        r.nrep = p[3];
        r.hdr_val = get32(p + 4);
        r.hdr_mask = get32(p + 8);
        for (unsigned k = 0; k < kMaxReqVdos; k++) {
            r.req_val[k] = get32(p + 12 + 4 * k);
            r.req_mask[k] = get32(p + 20 + 4 * k);
        }
        for (unsigned k = 0; k < kMaxWords; k++) r.rep[k] = get32(p + 28 + 4 * k);
        if (!row_valid(r)) return false;
    }
    out = q;
    return true;
}

const Quirks& builtin_quirks(unsigned id) {
    (void)id;
    return kNone;
}

std::size_t encode_seen(const SeenLog& log, unsigned index, std::span<std::uint8_t> out) {
    if (out.size() < 4) return 0;
    const std::uint32_t total = log.total() > 0xFFFFu ? 0xFFFFu : log.total();
    out[0] = 0;
    out[1] = static_cast<std::uint8_t>(total);
    out[2] = static_cast<std::uint8_t>(total >> 8);
    out[3] = static_cast<std::uint8_t>(log.size());
    if (index == kSeenTotals) return 4;
    Seen s;
    if (!log.get(index, s)) {
        out[0] = 3;
        return 1;
    }
    const std::size_t len = 8 + 4u * s.n;
    if (out.size() < len) return 0;
    out[4] = s.sop;
    out[5] = s.n;
    out[6] = static_cast<std::uint8_t>(s.header);
    out[7] = static_cast<std::uint8_t>(s.header >> 8);
    for (unsigned i = 0; i < s.n; i++) put32(&out[8 + 4 * i], s.w[i]);
    return len;
}

}  // namespace k2::emk_quirk
