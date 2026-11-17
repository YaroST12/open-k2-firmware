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

std::size_t build(const Card& c, const ct::Evidence& ev, ctsweep::View sweep, std::span<std::uint8_t> out) {
    if (c.level != ct::Level::Deep || out.size() < kMaxBlob) return 0;
    const bool has_spec = ev.spec.end != specsec::End::NotRun, has_sv = ev.svooc_end != 0;
    const ctsweep::RowView* lo[kMaxRanges] = {};
    const ctsweep::RowView* hi[kMaxRanges] = {};
    unsigned mask = 0;
    for (unsigned i = 0; i < c.n_ranges && i < kMaxRanges; ++i) {
        lo[i] = sweep.find(c.range[i].alg, c.range[i].pos, true);
        hi[i] = sweep.find(c.range[i].alg, c.range[i].pos, false);
        if (lo[i] || hi[i]) mask |= 1u << i;
    }
    if (!has_spec && !has_sv && !mask) return 0;

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
    out[0] = static_cast<std::uint8_t>(w.pos & 0xFFu);
    out[1] = static_cast<std::uint8_t>(w.pos >> 8);
    return 2u + (w.pos + 7u) / 8u;
}

}  // namespace k2::report_card::deep
