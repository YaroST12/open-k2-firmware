// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Deep extension blob of the report card: the sweep bucket reducer and the writer (layout in report_card_deep.hpp).
#include "k2/chargertest/report_card_deep.hpp"

#include <algorithm>
#include <cstring>

namespace k2::report_card::deep {
namespace {

// LSB-first bit writer over a zeroed buffer (widths up to 16).
struct BitW {
    std::uint8_t* p;
    unsigned pos = 0;
    void put(std::uint32_t v, unsigned n) {
        while (n) {
            const unsigned bi = pos & 7u, take = std::min(n, 8u - bi);
            p[pos >> 3] = static_cast<std::uint8_t>(p[pos >> 3] | ((v & ((1u << take) - 1u)) << bi));
            v >>= take; n -= take; pos += take;
        }
    }
    void put_signed(int v, unsigned n) { put(static_cast<std::uint32_t>(v) & ((1u << n) - 1u), n); }
    void put32(std::uint32_t v) { put(v & 0xFFFFu, 16); put(v >> 16, 16); }
};
// The matching reader over `nbits` bits; reading past the end sets `bad` and gives 0.
struct BitR {
    const std::uint8_t* p;
    unsigned nbits, pos = 0;
    bool bad = false;
    std::uint32_t get(unsigned n) {
        if (pos + n > nbits) { bad = true; pos = nbits; return 0; }
        std::uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i, ++pos) v |= std::uint32_t(p[pos >> 3] >> (pos & 7u) & 1u) << i;
        return v;
    }
    std::uint32_t get32() { const std::uint32_t lo = get(16); return lo | get(16) << 16; }
    // Steps over a field of any width (get() holds at most 32 bits; the SVOOC part is 37).
    void skip(unsigned n) {
        if (pos + n > nbits) { bad = true; pos = nbits; return; }
        pos += n;
    }
};

// The recorded error at setpoint index `s` of the span (false = no point there: an early stop or a gap).
bool at(const Seg& a, const Seg& b, unsigned lo_mv, unsigned s, int& e) {
    const unsigned mv = lo_mv + s * ctsweep::kStepMv;
    for (const Seg* g : {&a, &b}) {
        if (mv < g->lo_mv) continue;
        const unsigned i = (mv - g->lo_mv) / ctsweep::kStepMv;
        if (i < g->err.size()) { e = g->err[i]; return true; }
    }
    return false;
}
int rdiv(int num, int den) { return num >= 0 ? (2 * num + den) / (2 * den) : -((-2 * num + den) / (2 * den)); }   // half away from zero
unsigned cdiv(int x, int den) { return x <= 0 ? 0u : static_cast<unsigned>((x + den - 1) / den); }                  // ceiling, never below 0

}  // namespace

Bucket bucket(const Seg& a, const Seg& b, unsigned lo_mv, unsigned n, unsigned i, unsigned scale) {
    Bucket k;
    if (n == 0) return k;
    const unsigned nb = bucket_count(n);
    const unsigned s0 = i * n / nb, s1 = (i + 1) * n / nb;
    int sum = 0, mn = 32767, mx = -32768, e = 0;
    unsigned cnt = 0;
    for (unsigned s = s0; s < s1; ++s) {
        if (!at(a, b, lo_mv, s, e)) continue;
        sum += e; mn = std::min(mn, e); mx = std::max(mx, e); ++cnt;
    }
    if (!cnt) return k;
    const int unit = 1 << scale;
    int q = rdiv(sum, static_cast<int>(cnt) * unit);
    if (q > 127 || q < -127) { q = q > 0 ? 127 : -127; k.saturated = true; }
    const unsigned dn = cdiv(q * unit - mn, unit), up = cdiv(mx - q * unit, unit);
    if (dn > 15 || up > 15) k.saturated = true;
    k.mean = static_cast<std::uint8_t>(static_cast<std::int8_t>(q));
    k.down = static_cast<std::uint8_t>(std::min(dn, 15u));
    k.up = static_cast<std::uint8_t>(std::min(up, 15u));
    return k;
}

unsigned choose_scale(const Seg& a, const Seg& b, unsigned lo_mv, unsigned n) {
    const unsigned nb = bucket_count(n);
    for (unsigned sc = 0; sc < kMaxScale; ++sc) {
        bool ok = true;
        for (unsigned i = 0; i < nb && ok; ++i) ok = !bucket(a, b, lo_mv, n, i, sc).saturated;
        if (ok) return sc;
    }
    return kMaxScale;
}

namespace {
// One part of a range's sweep as the reducer's segment (the record's points, clipped to the array).
Seg seg_of(const ctsweep::RowView* r, std::span<const std::int16_t> pts) {
    if (!r || r->first >= pts.size()) return {};
    return {pts.subspan(r->first, std::min<std::size_t>(r->n, pts.size() - r->first)), r->lo_mv};
}
}  // namespace

unsigned mi_watts(const MiMode& m) { return (unsigned(m.mv) * 100u * unsigned(m.ma) * 50u + 500000u) / 1000000u; }

MiSection mi_of(std::span<const ct::Step> steps, const ct::Evidence& ev) {
    MiSection m;
    for (const ct::Step& s : steps) {
        if (ct::is_mi_chain(s)) {
            if (s.verdict != ct::Verdict::Pending && s.verdict != ct::Verdict::Running && s.detail) m.chain = std::min<std::uint8_t>(s.detail, 31);
            continue;
        }
        if (!ct::is_mi_mode(s) || m.n >= m.mode.size()) continue;
        MiMode& x = m.mode[m.n++];
        x.before = kBeforeLost;
        for (unsigned k = 0; k < ev.mi_n && k < ev.mi.size(); ++k)
            if (ev.mi[k].pos == s.pos && ev.mi[k].after == s.pdo) { x.before = ev.mi[k].before; break; }
        x.after = s.pdo;
        x.mv = std::uint16_t(std::min<unsigned>((s.mv + 50u) / 100u, 511u));
        x.ma = std::uint16_t(std::min<unsigned>(s.ma / 50u, 127u));
        x.verdict = std::uint8_t(std::min<unsigned>(unsigned(s.verdict), 7u));
        x.reason = std::uint8_t(std::min<unsigned>(unsigned(s.reason), 63u));
    }
    // No section unless the source listed 0x2717: "no SVID" is every other PD charger, and a Medium card would carry a block for it.
    if (m.chain == mi_unlock::kCodeNoSvid || !m.chain) m = {};
    return m;
}

MiSection read_mi(std::span<const std::uint8_t> blob) {
    MiSection m;
    if (!blob_valid(blob)) return m;
    BitR r{blob.data() + 2, blob_bits(blob)};
    const bool has_spec = r.get(1), has_sv = r.get(1);
    const unsigned mask = r.get(kMaxRanges);
    if (has_sv) r.skip(kSvoocBits);
    if (has_spec) {
        r.skip(3 + 6 + 1);
        const unsigned cells = r.get(5);
        for (unsigned i = 0; i < cells && !r.bad; ++i) {
            const unsigned v = r.get(3);
            if (v == static_cast<unsigned>(pdspec::Verdict::Skip)) { r.skip(4); continue; }
            if (v == static_cast<unsigned>(pdspec::Verdict::Pending)) continue;
            r.skip(13);
            if (v == static_cast<unsigned>(pdspec::Verdict::Fail)) r.skip(24);
        }
    }
    for (unsigned i = 0; i < kMaxRanges && !r.bad; ++i) {
        if (!(mask >> i & 1u)) continue;
        r.skip(9);
        const unsigned n = r.get(kSpanBits);
        r.skip(3);
        r.skip(bucket_count(n) * kBucketBits);
    }
    if (r.bad || r.nbits - r.pos < kMiHeadBits) return {};
    m.chain = std::uint8_t(r.get(5));
    m.n = std::uint8_t(r.get(3));
    // the section is the block's tail, to the bit, and names a chain outcome: anything else is not one
    if (m.n > m.mode.size() || r.nbits - r.pos != m.n * kMiModeBits || mi_unlock::chain_of(m.chain) == mi_unlock::Chain::None) return {};
    for (unsigned k = 0; k < m.n; ++k) {
        MiMode& x = m.mode[k];
        x.before = r.get32(); x.after = r.get32();
        x.mv = std::uint16_t(r.get(9)); x.ma = std::uint16_t(r.get(7));
        x.verdict = std::uint8_t(r.get(3)); x.reason = std::uint8_t(r.get(6));
    }
    return r.bad ? MiSection{} : m;
}

std::size_t build(const Card& c, std::span<const ct::Step> steps, const ct::Evidence& ev, ctsweep::View sweep, std::span<std::uint8_t> out) {
    if (out.size() < kMaxBlob) return 0;
    const bool deep = c.level == ct::Level::Deep;
    const bool has_spec = deep && ev.spec.end != specsec::End::NotRun, has_sv = deep && ev.svooc_end != 0;
    const ctsweep::RowView* lo[kMaxRanges] = {};
    const ctsweep::RowView* hi[kMaxRanges] = {};
    unsigned mask = 0;
    for (unsigned i = 0; deep && i < c.n_ranges && i < kMaxRanges; ++i) {
        lo[i] = sweep.find(c.range[i].alg, c.range[i].pos, true);
        hi[i] = sweep.find(c.range[i].alg, c.range[i].pos, false);
        if (lo[i] || hi[i]) mask |= 1u << i;
    }
    const MiSection mi = mi_of(steps, ev);
    if (!has_spec && !has_sv && !mask && !mi.chain) return 0;

    std::memset(out.data(), 0, kMaxBlob);
    BitW w{out.data() + 2};
    w.put(has_spec, 1); w.put(has_sv, 1); w.put(mask, kMaxRanges);
    if (has_sv) {
        w.put(std::min<unsigned>(ev.svooc_end, 7), 3);
        w.put(std::min<unsigned>(ev.svooc_why, 15), 4);
        w.put(std::min<unsigned>(ev.svooc_asks, 7), 3);
        w.put(std::min<unsigned>(ev.svooc_word, 255), 8);
        w.put(std::min<unsigned>(ev.svooc_vmax_mv / 100u, 511), 9);
        w.put(std::min<unsigned>(ev.svooc_stop_ms / 100u, 1023), 10);
    }
    if (has_spec) {
        const specsec::Result& r = ev.spec;
        w.put(static_cast<unsigned>(r.end), 3);
        w.put(std::min<unsigned>(r.why, 63), 6);
        w.put(r.unconfirmed, 1);
        w.put(specsec::kCells, 5);
        for (const specsec::Cell& cell : r.cells) {
            const unsigned v = cell.code & 7u;
            w.put(v, 3);
            if (v == static_cast<unsigned>(pdspec::Verdict::Skip)) { w.put((cell.code >> 3) & 15u, 4); continue; }
            if (v == static_cast<unsigned>(pdspec::Verdict::Pending)) continue;
            w.put_signed(cell.value == specsec::kUnseen ? kValueNever : std::clamp<int>(cell.value, -kValueMax, kValueMax), 13);
            if (v == static_cast<unsigned>(pdspec::Verdict::Fail)) { w.put(cell.fails, 8); w.put(cell.a, 8); w.put(cell.b, 8); }
        }
    }
    for (unsigned i = 0; i < kMaxRanges; ++i) {
        if (!(mask >> i & 1u)) continue;
        const unsigned lo_mv = lo[i] ? lo[i]->lo_mv : hi[i]->lo_mv, hi_mv = hi[i] ? hi[i]->hi_mv : lo[i]->hi_mv;
        const unsigned n = std::min<unsigned>(hi_mv >= lo_mv ? (hi_mv - lo_mv) / ctsweep::kStepMv + 1u : 1u, (1u << kSpanBits) - 1u);
        const Seg a = seg_of(lo[i], sweep.points), b = seg_of(hi[i], sweep.points);
        const unsigned sc = choose_scale(a, b, lo_mv, n);
        w.put(std::min<unsigned>(lo_mv / 100u, 511), 9); w.put(n, kSpanBits); w.put(sc, 3);
        for (unsigned k = 0, nb = bucket_count(n); k < nb; ++k) {
            const Bucket bk = bucket(a, b, lo_mv, n, k, sc);
            w.put(bk.mean, 8); w.put(bk.down, 4); w.put(bk.up, 4);
        }
    }
    if (mi.chain) {
        w.put(mi.chain, 5); w.put(mi.n, 3);
        for (unsigned k = 0; k < mi.n; ++k) {
            const MiMode& x = mi.mode[k];
            w.put32(x.before); w.put32(x.after);
            w.put(x.mv, 9); w.put(x.ma, 7); w.put(x.verdict, 3); w.put(x.reason, 6);
        }
    }
    out[0] = static_cast<std::uint8_t>(w.pos & 0xFFu);
    out[1] = static_cast<std::uint8_t>(w.pos >> 8);
    return 2u + (w.pos + 7u) / 8u;
}

}  // namespace k2::report_card::deep
