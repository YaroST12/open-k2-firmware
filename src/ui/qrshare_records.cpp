// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// QR share record builders (see k2/ui/qrshare_records.hpp).
#include "k2/ui/qrshare_records.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "k2/core/fmt.hpp"

namespace k2::qrshare::records {

namespace rc = report_card;

// ---- schema v2: the LSB-first bit writer and the two packers (see the header for the layouts) ----

namespace v2 {

// LSB-first bit stream into `out`: put() masks the value to `width` bits and appends, varint() is 7 value bits + 1 continuation
// bit per group. finish() flushes the last partial byte (zero padded) and returns the length, 0 when `out` was too small.
class BitW {
public:
    explicit BitW(std::span<std::uint8_t> out) : out_(out) {}
    void put(std::uint32_t v, unsigned width) {
        const std::uint32_t mask = width >= 32 ? ~0u : (1u << width) - 1u;
        acc_ |= std::uint64_t(v & mask) << nbits_;
        nbits_ += width;
        while (nbits_ >= 8) flush();
    }
    void varint(std::uint32_t v) {
        while (v >= 128) {
            put(v & 0x7F, 7);
            put(1, 1);
            v >>= 7;
        }
        put(v, 7);
        put(0, 1);
    }
    std::size_t finish() {
        if (!overflow_ && nbits_ != 0) flush();
        return overflow_ ? 0 : bytes_;
    }

private:
    void flush() {
        if (bytes_ >= out_.size()) overflow_ = true;
        else out_[bytes_++] = std::uint8_t(acc_ & 0xFF);
        acc_ >>= 8;
        nbits_ -= 8;
    }
    std::span<std::uint8_t> out_;
    std::uint64_t acc_ = 0;
    unsigned nbits_ = 0;
    std::size_t bytes_ = 0;
    bool overflow_ = false;
};

// One group result (verdict/reason/detail byte + the u24 of max_dv | fail_dv<<9 | detail<<18), as both layouts store it.
inline void put_group(BitW& w, std::uint8_t vr, std::uint32_t v) {
    w.put(vr >> 6, 2);
    w.put(vr & 0x3F, 6);
    w.put(v >> 18, 6);                       // the packed detail, 6 bits, unchanged
    if (v & 0x1FF) { w.put(1, 1); w.put(v & 0x1FF, 9); } else w.put(0, 1);
    if (v >> 9 & 0x1FF) { w.put(1, 1); w.put(v >> 9 & 0x1FF, 9); } else w.put(0, 1);
}

// The cable kind's 3-bit code (0 = 0, 1 = 1, 2 = 2, 3 = 4, 4 = 0xFF; anything else reads as 0).
constexpr unsigned cable_kind_code(std::uint8_t kind) {
    switch (kind) {
        case 1: return 1;
        case 2: return 2;
        case 4: return 3;
        case 0xFF: return 4;
        default: return 0;
    }
}

}  // namespace v2

bool valid_v1(Type type, std::span<const std::uint8_t> rec) {
    if (!has_v2(type) || rec.empty()) return false;
    if (type == Type::Card)
        return rec.size() == rc::kMaxRecord && rec[0] == rc::kMagic && rec[1] == rc::kTypeCard && rec[2] == rc::kPayload;
    if (rec.size() < kEmarkerHead) return false;
    if (rec[0] & kEmarkerQuirks) {
        const std::size_t q = rec.size() - kEmarkerHead;
        if (q < emk_quirk::kBlockHeader || q != emk_quirk::block_size(rec[kEmarkerHead + 2])) return false;
        if (rec[kEmarkerHead + 2] > emk_quirk::kMaxRows) return false;
        for (unsigned i = 0; i < rec[kEmarkerHead + 2]; i++) {
            const std::uint8_t* r = rec.data() + kEmarkerHead + emk_quirk::kBlockHeader + emk_quirk::kRowBytes * i;
            if (r[2] > emk_quirk::kMaxReqVdos || r[3] > emk_quirk::kMaxWords) return false;
        }
        return true;
    }
    return rec.size() == kEmarkerHead;
}

// The format-7 card record (202 B) -> the v2 record. The payload is copied to a local first, so `out` may alias `in` (the
// varints can run the write pointer a few bytes ahead of the read pointer).
static std::size_t pack_card_v2(std::span<const std::uint8_t> rec, std::span<std::uint8_t> out) {
    std::array<std::uint8_t, rc::kPayload> pl{};
    std::memcpy(pl.data(), rec.data() + 4, rc::kPayload);
    const std::uint16_t crc16 = rc::detail::get16(rec.data() + rc::kMaxRecord - 2);
    const std::uint8_t* p = pl.data();
    if (p[31] > rc::kMaxRanges) return 0;
    const unsigned n_ranges = p[31];
    v2::BitW w(out);
    w.put(kSchemaV2, 8);
    w.put(rc::detail::get32(p + 0), 32);
    w.put(rc::detail::get16(p + 4), 16);
    w.put(rc::detail::get16(p + 6), 16);
    w.varint(rc::detail::get16(p + 8));                 // card_no
    w.varint(rc::detail::get16(p + 10));                // runs
    w.varint(rc::detail::get32(p + 12));                // boots
    w.varint(rc::detail::get32(p + 16));                // uptime_s
    w.put(rc::detail::get32(p + 20), 32);               // fw
    w.varint(rc::detail::get16(p + 24));                // handover_ds
    w.put(p[26], 2);                            // spec_rev
    w.put(p[27], 4);                            // num_pdos
    w.put(p[28], 3);                            // handover
    w.put(p[29], 4);                            // state
    w.put(p[30], 6);                            // reason
    w.put(n_ranges, 3);
    w.put(p[32], 8);                            // ev_flags
    unsigned nl = 0;
    while (nl < rc::kNameMax && p[rc::off::kName + nl] != 0) nl++;
    w.put(nl, 5);
    for (unsigned i = 0; i < nl; i++) w.put(p[rc::off::kName + i], 7);
    w.put(p[rc::off::kPdp], 8);                 // pdp_w
    w.put(p[rc::off::kFlags] & 1, 1);           // name_auto
    w.put(p[rc::off::kFlags] >> 1, 7);          // pd_ma50
    for (unsigned g = 0; g < rc::kGroups; g++) {
        const std::uint8_t* q = p + rc::off::kGroups + 4 * g;
        v2::put_group(w, q[0], std::uint32_t(q[1]) | std::uint32_t(q[2]) << 8 | std::uint32_t(q[3]) << 16);
    }
    for (unsigned i = 0; i < n_ranges; i++) {
        const std::uint8_t* q = p + rc::off::kRanges + 10 * i;
        const std::uint16_t lo = rc::detail::get16(q + 2);
        w.put(q[0], 5);                         // alg
        w.put(q[1], 8);                         // pos
        w.put(lo & 0x1FF, 9);                   // lo_dv
        w.put(rc::detail::get16(q + 4), 9);             // hi_dv
        w.put(rc::detail::get16(q + 6), 9);             // max_dv
        w.put(rc::detail::get16(q + 8), 9);             // fail_dv
        w.put(lo >> 9, 7);                      // ma50
    }
    for (unsigned i = 0; i < rc::kEprLevels; i++) {
        const std::uint8_t* q = p + rc::off::kEpr + 6 * i;
        const std::uint16_t dv = rc::detail::get16(q);
        w.put(dv == 280 ? 1 : dv == 360 ? 2 : dv == 480 ? 3 : 0, 2);
        v2::put_group(w, q[2], std::uint32_t(q[3]) | std::uint32_t(q[4]) << 8 | std::uint32_t(q[5]) << 16);
    }
    const std::uint8_t* cb = p + rc::off::kCable;
    bool has_cable = false;
    for (unsigned i = 0; i < 16; i++) has_cable = has_cable || cb[i] != 0;
    if (has_cable) {
        w.put(1, 1);
        w.varint(rc::detail::get32(cb));                // seq
        w.put(rc::detail::get16(cb + 4), 16);           // vid
        w.put(rc::detail::get16(cb + 6), 16);           // pid
        w.put(rc::detail::get16(cb + 8), 16);           // max_mv
        w.put(rc::detail::get16(cb + 10), 16);          // max_ma
        w.put(v2::cable_kind_code(cb[12]), 3);  // kind
        w.put(cb[13], 8);                       // flags
        w.put(cb[14], 8);                       // resumed
        w.put(cb[15], 8);                       // src_flags
    } else {
        w.put(0, 1);
    }
    unsigned n_ufcs = 0;
    std::array<std::uint32_t, rc::kMaxUfcs> uf{};
    for (unsigned i = 0; i < rc::kMaxUfcs; i++) {
        const std::uint8_t* q = p + rc::off::kUfcs + 3 * i;
        const std::uint32_t v = q[0] | std::uint32_t(q[1]) << 8 | std::uint32_t(q[2]) << 16;
        if (v != 0) uf[n_ufcs++] = v;
    }
    w.put(n_ufcs, 3);
    for (unsigned i = 0; i < n_ufcs; i++) {
        w.put(uf[i] & 0xFF, 8);                 // lo_dv
        w.put(uf[i] >> 8 & 0x1FF, 9);           // hi_dv
        w.put(uf[i] >> 17, 7);                  // ma50
    }
    const unsigned times = p[rc::off::kRepeat];
    w.varint(times);
    if (times > 1) {
        w.put(rc::detail::get16(p + rc::off::kRepeat + 1), 16);   // last_boots
        w.put(std::uint32_t(p[rc::off::kRepeat + 3]) | std::uint32_t(p[rc::off::kRepeat + 4]) << 8 |
                  std::uint32_t(p[rc::off::kRepeat + 5]) << 16,
              24);                                     // last_uptime_s
    }
    w.put(crc16, 16);                           // the v1 record's CRC-16: the integrity check is unchanged
    return w.finish();
}

// The e-marker record (flags, name[24], the 66-byte emrec, the quirk block when flag bit0 is set) -> the v2 record, which drops
// the emrec's 24 reserved zero bytes. The record is copied to a local first, so `out` may alias `in`.
static std::size_t pack_emk_v2(std::span<const std::uint8_t> rec, std::span<std::uint8_t> out) {
    std::array<std::uint8_t, kMaxEmarker> cp{};
    std::memcpy(cp.data(), rec.data(), rec.size());
    const std::uint8_t* e = cp.data();
    const bool has_q = (e[0] & kEmarkerQuirks) != 0;
    v2::BitW w(out);
    w.put(kSchemaV2, 8);
    unsigned nl = 0;
    while (nl < kEmarkerName && e[1 + nl] != 0) nl++;
    w.put(nl, 5);
    for (unsigned i = 0; i < nl; i++) w.put(e[1 + i], 7);
    w.put(e[0], 8);                             // flags
    const std::uint8_t* r = e + 1 + kEmarkerName;   // the 66-byte emrec
    unsigned rl = 0;
    while (rl < emrec::kNameLen && r[rl] != 0) rl++;
    w.put(rl, 5);
    for (unsigned i = 0; i < rl; i++) w.put(r[i], 7);
    w.put(r[emrec::kNameLen], 2);               // spec_rev
    for (unsigned i = 0; i < 5; i++) w.put(rc::detail::get32(r + 21 + 4 * i), 32);   // the five VDOs
    w.put(r[emrec::kRecordSize - 1], 8);        // checksum
    if (has_q) {
        const std::uint8_t* q = e + kEmarkerHead;
        const unsigned count = q[2];
        w.put(1, 1);
        w.put(q[0], 2);                         // flags
        w.put(q[1], 2);                         // unmatched
        w.put(count, 4);
        for (unsigned i = 0; i < count; i++) {
            const std::uint8_t* row = q + emk_quirk::kBlockHeader + emk_quirk::kRowBytes * i;
            const std::uint32_t hdr_val = rc::detail::get32(row + 4), hdr_mask = rc::detail::get32(row + 8);
            w.put(row[0], 2);                   // sop
            w.put(row[1], 2);                   // kind
            w.put(row[2], 2);                   // nreq
            w.put(row[3], 3);                   // nrep
            const bool full = hdr_mask == emk_quirk::kSvdmMask;
            w.put(full ? 1 : 0, 1);
            w.put(hdr_val, 32);
            if (!full) w.put(hdr_mask, 32);
            for (unsigned k = 0; k < row[2]; k++) {
                w.put(rc::detail::get32(row + 12 + 4 * k), 32);   // req_val[k]
                w.put(rc::detail::get32(row + 20 + 4 * k), 32);   // req_mask[k]
            }
            for (unsigned k = 0; k < row[3]; k++) w.put(rc::detail::get32(row + 28 + 4 * k), 32);   // rep
        }
    } else {
        w.put(0, 1);
    }
    return w.finish();
}

std::size_t pack_v2(Type type, std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    if (!valid_v1(type, in)) return 0;
    if (type == Type::Card) return pack_card_v2(in, out);
    return pack_emk_v2(in, out);
}

std::size_t pack_v2_in_place(Type type, std::span<std::uint8_t> rec) { return pack_v2(type, rec, rec); }

std::size_t emarker(const emrec::Record& rec, std::string_view name, const emk_quirk::Quirks& quirks, std::span<std::uint8_t> out) {
    const std::size_t qlen = quirks.empty() ? 0 : emk_quirk::block_size(quirks.count);
    if (out.size() < kEmarkerHead + qlen) return 0;
    out[0] = qlen ? kEmarkerQuirks : 0;
    const std::size_t n = std::min(name.size(), kEmarkerName);
    std::memset(out.data() + 1, 0, kEmarkerName);
    std::memcpy(out.data() + 1, name.data(), n);
    std::memcpy(out.data() + 1 + kEmarkerName, rec.data(), rec.size());
    if (qlen && emk_quirk::encode(quirks, out.subspan(kEmarkerHead, qlen)) != qlen) return 0;
    return kEmarkerHead + qlen;
}

std::size_t pdcaps(const pd::SourceCaps& caps, std::span<std::uint8_t> out) {
    if (!caps.valid || caps.num_pdos == 0) return 0;
    const std::size_t n = std::min<std::size_t>(caps.num_pdos, pd::kMaxPdos);
    if (out.size() < kPdCapsHead + 4 * n) return 0;
    out[0] = static_cast<std::uint8_t>(n);
    out[1] = static_cast<std::uint8_t>((caps.epr ? kPdCapsEpr : 0) | (caps.epr_partial ? kPdCapsEprPartial : 0));
    out[2] = caps.spec_rev;
    out[3] = caps.active_cc;
    out[4] = static_cast<std::uint8_t>(caps.raw_header);
    out[5] = static_cast<std::uint8_t>(caps.raw_header >> 8);
    for (std::size_t i = 0; i < n; i++) {
        const std::uint32_t w = caps.pdos[i].raw;
        for (unsigned b = 0; b < 4; b++) out[kPdCapsHead + 4 * i + b] = static_cast<std::uint8_t>(w >> (8 * b));
    }
    return kPdCapsHead + 4 * n;
}

std::size_t card(const report_card::Card& c, std::span<std::uint8_t> out) {
    if (out.size() < kMaxCard) return 0;
    return report_card::encode_card(out, c);
}

std::size_t identity(std::string_view fw, std::string_view git, unsigned proto, std::span<std::uint8_t> out) {
    char num[12];
    const std::size_t nl = fmt::format(num, "%u", proto);
    fw = fw.substr(0, 15);
    git = git.substr(0, 39);
    const std::size_t n = 3 + fw.size() + 3 + git.size() + 3 + nl;   // "fw=" fw " g=" git " p=" proto
    if (out.size() < n) return 0;
    std::uint8_t* p = out.data();
    const auto put = [&p](std::string_view s) { std::memcpy(p, s.data(), s.size()); p += s.size(); };
    put("fw=");
    put(fw);
    put(" g=");
    put(git);
    put(" p=");
    put(std::string_view(num, nl));
    return n;
}

std::size_t boot(const bootinfo::Snapshot& s, std::span<std::uint8_t> out) {
    char tmp[bootinfo::kQrMax + 1];
    const std::size_t n = bootinfo::format(s, tmp);
    if (n == 0 || out.size() < n) return 0;
    std::memcpy(out.data(), tmp, n);
    return n;
}

std::size_t log(std::string_view git, std::uint32_t now_ms, std::uint32_t base_ms, bool older, std::size_t entries, std::span<std::uint8_t> out) {
    if (entries == 0 || out.size() < kLogHeadMax + entries) return 0;
    git = git.substr(0, kLogGit);
    const std::size_t head = 1 + git.size() + 1 + 8;
    std::uint8_t* p = out.data();
    std::memmove(p + head, p + kLogHeadMax, entries);
    *p++ = static_cast<std::uint8_t>(git.size());
    std::memcpy(p, git.data(), git.size());
    p += git.size();
    *p++ = older ? kLogOlder : 0;
    for (const std::uint32_t v : {base_ms, now_ms})
        for (unsigned b = 0; b < 4; b++) *p++ = static_cast<std::uint8_t>(v >> (8 * b));
    return head + entries;
}

}  // namespace k2::qrshare::records
