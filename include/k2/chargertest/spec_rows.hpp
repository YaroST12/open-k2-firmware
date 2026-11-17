// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD spec check's per-transition rows: what a spec check measured on each fixed transition (5 V to 9 V, 12 V to 15 V, ...) or in
// each programmable pass (a window and a step class), as plain 10-byte rows. Pure: the rows are a projection of the engine's records
// (pdspec::TransRec / PassRec), written to the run journal as one file (/jr/spt.rec) and read back for the detail page, the remote and
// the web card. A check with one measurement or none keeps the single summary of its Cell (has_list says which).
//
// Row (10 bytes, little endian, fixed positions: the tools read it by offset):
//   0 check   pdspec::Check
//   1 flags   bit 0 failing (the aggregate's own judgement, the sample bracket counted), bit 1 programmable pass, bits 2..3 step class
//   2 a       from level in mV (fixed) / window minimum in mV (pass)
//   4 b       to level in mV (fixed) / window maximum in mV (pass)
//   6 value   the measurement in the check's unit at the cell's precision (0.1 ms, 10 mV, whole ms), kUnseen = never
//   8 margin  fixed: signed distance past the nearest limit (positive = outside), kUnseen = no value; pass: the failing steps (u8)
//
// File: header (kRowsHdr: "SPT1", run id u32, 27 row counts u16, 27 worst-row indices u8, one pad), then the rows of cell 0, of
// cell 1, ... each in record order. rows_parse refuses a file of another run, of another size or without the magic, and the page then
// shows the summary.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "k2/chargertest/spec_section.hpp"
#include "k2/pdspec/analyze.hpp"
#include "k2/pdspec/engine.hpp"
#include "k2/pdspec/result.hpp"

namespace k2::specsec {

inline constexpr std::uint8_t kRowFail = 1, kRowProg = 2;
inline constexpr std::size_t kRowBytes = 10;

struct Row {
    std::uint8_t check = 0;
    std::uint8_t flags = 0;
    std::uint16_t a = 0, b = 0;
    std::int16_t value = 0, margin = 0;
    bool operator==(const Row&) const = default;
};

constexpr std::uint8_t row_class(const Row& r) { return static_cast<std::uint8_t>((r.flags >> 2) & 3u); }

inline void row_encode(const Row& r, std::uint8_t* d) {
    const auto w = [&d](std::size_t at, std::uint16_t v) { d[at] = static_cast<std::uint8_t>(v); d[at + 1] = static_cast<std::uint8_t>(v >> 8); };
    d[0] = r.check;
    d[1] = r.flags;
    w(2, r.a);
    w(4, r.b);
    w(6, static_cast<std::uint16_t>(r.value));
    w(8, static_cast<std::uint16_t>(r.margin));
}
inline void row_decode(const std::uint8_t* d, Row& r) {
    const auto u = [&d](std::size_t at) { return static_cast<std::uint16_t>(d[at] | d[at + 1] << 8); };
    r.check = d[0];
    r.flags = d[1];
    r.a = u(2);
    r.b = u(4);
    r.value = static_cast<std::int16_t>(u(6));
    r.margin = static_cast<std::int16_t>(u(8));
}

// The checks that list a row per transition or pass. The rest (PPS_Status and its time, the tracking finding, the Source reset
// observer) answer once per pass or once per run: their cell is the whole story.
constexpr bool has_list(std::size_t cell) { return cell < 16 || (cell >= 19 && cell <= 24); }
static_assert(kOrder[15] == sc::Check::PpsTransient && kOrder[16] == sc::Check::PpsStatus && kOrder[19] == sc::Check::AvsMonotonic &&
                  kOrder[24] == sc::Check::AvsTransient && kOrder[25] == sc::Check::AvsTrack,
              "has_list names the cells by index");

// ---- the file --------------------------------------------------------------------------------------------------------------------
inline constexpr std::size_t kRowsHdr = 8 + 2 * kCells + kCells + 1;   // magic, run id, counts, worst indices, pad (even)
static_assert(kRowsHdr % 2 == 0);
// The most rows one run can list: every record of a full pool is a transition with 7 checks (the pool holds fewer passes, 6 rows each).
inline constexpr std::size_t kRowsMax = (pdspec::kPoolBytes / sizeof(pdspec::TransRec)) * 7;
static_assert((pdspec::kPoolBytes / sizeof(pdspec::PassRec)) * 6 <= kRowsMax, "a pool of passes lists fewer rows than a pool of transitions");
inline constexpr std::size_t kRowsMaxFile = kRowsHdr + kRowsMax * kRowBytes;

inline constexpr std::array<std::uint8_t, 4> kRowsMagic{'S', 'P', 'T', '1'};

inline void rows_put_header(std::uint8_t* d, std::uint32_t run_id, const std::array<std::uint16_t, kCells>& counts,
                            const std::array<std::uint8_t, kCells>& worst) {
    for (std::size_t i = 0; i < 4; ++i) d[i] = kRowsMagic[i];
    for (std::size_t i = 0; i < 4; ++i) d[4 + i] = static_cast<std::uint8_t>(run_id >> (8 * i));
    for (std::size_t i = 0; i < kCells; ++i) {
        d[8 + 2 * i] = static_cast<std::uint8_t>(counts[i]);
        d[9 + 2 * i] = static_cast<std::uint8_t>(counts[i] >> 8);
        d[8 + 2 * kCells + i] = worst[i];
    }
    d[kRowsHdr - 1] = 0;
}

// The index a file's header makes: where the rows of each cell start. Small (27 counts and worst indices), so the page keeps one.
struct RowsIndex {
    std::array<std::uint16_t, kCells> n{};
    std::array<std::uint16_t, kCells> start{};   // the first row of the cell in the file's row order
    std::array<std::uint8_t, kCells> w{};
    std::size_t count(std::size_t cell) const { return cell < kCells ? n[cell] : 0; }
    std::size_t first(std::size_t cell) const { return cell < kCells ? start[cell] : 0; }
    std::size_t worst(std::size_t cell) const { return cell < kCells ? w[cell] : 0; }
    std::size_t total() const { return kCells ? std::size_t(start[kCells - 1]) + n[kCells - 1] : 0; }
    // The byte offset in the file of the cell's k-th row.
    std::size_t offset(std::size_t cell, std::size_t k) const { return kRowsHdr + (first(cell) + k) * kRowBytes; }
};

// True when the header is this run's and the file is exactly the rows it counts.
inline bool rows_parse(const std::uint8_t* hdr, std::size_t file_size, std::uint32_t run_id, RowsIndex& ix) {
    if (file_size < kRowsHdr) return false;
    for (std::size_t i = 0; i < 4; ++i) if (hdr[i] != kRowsMagic[i]) return false;
    std::uint32_t id = 0;
    for (std::size_t i = 0; i < 4; ++i) id |= std::uint32_t(hdr[4 + i]) << (8 * i);
    if (id != run_id) return false;
    std::size_t sum = 0;
    for (std::size_t i = 0; i < kCells; ++i) {
        ix.n[i] = static_cast<std::uint16_t>(hdr[8 + 2 * i] | hdr[9 + 2 * i] << 8);
        ix.start[i] = static_cast<std::uint16_t>(sum);
        ix.w[i] = hdr[8 + 2 * kCells + i];
        sum += ix.n[i];
        if (ix.n[i] && ix.w[i] >= ix.n[i]) return false;
    }
    return sum <= kRowsMax && file_size == kRowsHdr + sum * kRowBytes;
}

// ---- the projection of the engine's records --------------------------------------------------------------------------------------

namespace rows_detail {

// A fixed transition's measurement of check c, as the check judged it: the value, the limits and how the check's kind reads them.
struct Fixed {
    std::int32_t v = 0, lo = 0, hi = 0;
    sc::Kind kind = sc::Kind::Max;
};

constexpr std::int32_t ms_or_never(std::uint16_t ms) { return ms == 0xFFFF ? sc::kNever : std::int32_t(ms) * 1000; }

// False when the record has nothing to list for the check on this transition (not measured, or the other direction's check).
inline bool fixed_of(sc::Check c, const sc::TransRec& t, std::int32_t from_mv, std::int32_t to_mv, Fixed& f) {
    if ((t.flags & sc::kTrRefused) || !(t.seen & sc::tr_bit(c)) || !from_mv || !to_mv) return false;
    const bool up = to_mv > from_mv, epr = (t.flags & sc::kTrEpr) != 0;
    using C = sc::Check;
    switch (c) {
        case C::AcceptTime:
            f = {t.accept_100us == 0xFFFF ? sc::kNever : std::int32_t(t.accept_100us) * 100, 0, std::int32_t(pd::spec::kAcceptSpecLimitMs * 1000), sc::Kind::Max};
            return true;
        case C::PsRdyTime:
            f = {ms_or_never(t.psrdy_ms), 0, std::int32_t((epr ? pd::spec::kSrcTransReqEprMs : pd::spec::kSrcTransReqSprMs) * 1000), sc::Kind::Max};
            return true;
        case C::SrcStart:
            f = {ms_or_never(t.start_ms), std::int32_t(pd::spec::kSrcTransitionMinMs * 1000), 0, sc::Kind::Min};
            return true;
        case C::PsRdyEarly:
            if (up) return false;
            f = {t.psrdy_mv, 0, sc::band_hi(to_mv), sc::Kind::Max};
            return true;
        case C::PsRdyLow:
            if (!up) return false;
            f = {t.psrdy_mv, sc::band_lo(to_mv), 0, sc::Kind::Min};
            return true;
        case C::RiseTime:
        case C::FallTime:
            if (up != (c == C::RiseTime)) return false;
            f = {ms_or_never(t.band_ms), 0, std::int32_t((epr ? pd::spec::kInBandEprMs : pd::spec::kInBandSprMs) * 1000), sc::Kind::Max};
            return true;
        case C::Overshoot: {
            const std::int32_t extra = std::int32_t(pd::spec::kVSrcValidExtraMv);
            f = up ? Fixed{t.peak_mv, -sc::kOpenLimit, sc::band_hi(to_mv) + extra, sc::Kind::Range}
                   : Fixed{t.peak_mv, sc::band_lo(to_mv) - extra, sc::kOpenLimit, sc::Kind::Range};
            return true;
        }
        case C::Accuracy:
            f = {t.final_mv, sc::band_lo(to_mv), sc::band_hi(to_mv), sc::Kind::Range};
            return true;
        default: return false;
    }
}

// How far past the nearest limit (positive = outside), in the check's unit; the finite sides only.
constexpr std::int32_t margin_of(const Fixed& f) {
    std::int32_t m = INT32_MIN;
    if (f.kind != sc::Kind::Min && f.hi < sc::kOpenLimit) m = f.v - f.hi;
    if (f.kind != sc::Kind::Max && f.lo > -sc::kOpenLimit) { const std::int32_t u = f.lo - f.v; if (u > m) m = u; }
    return m;
}

inline Row make_fixed(sc::Check c, const sc::TransRec& t, std::uint16_t from_mv, std::uint16_t to_mv, const Fixed& f) {
    const sc::Unit u = sc::def(c).unit;
    Row r;
    r.check = static_cast<std::uint8_t>(c);
    r.flags = (t.fails & sc::tr_bit(c)) ? kRowFail : std::uint8_t{0};
    r.a = from_mv;
    r.b = to_mv;
    r.value = quantize(u, f.v);
    r.margin = f.v >= sc::kNever ? kUnseen : quantize(u, margin_of(f));
    return r;
}

// One programmable pass's row for check c, or false when the pass has none (no steps measured, or another kind's check).
inline bool pass_row(sc::Check c, const sc::PassRec& p, Row& r) {
    using C = sc::Check;
    const bool avs = (p.cls & sc::kPassAvs) != 0;
    const std::uint8_t cls = p.cls & 3u;
    if (sc::avs_check(c) != avs) return false;
    const auto us = [](std::uint16_t t100) { return t100 == 0xFFFF ? kUnseen : static_cast<std::int16_t>(t100 > 32766 ? 32766 : t100); };
    std::int16_t value = 0;
    std::uint8_t fails = 0;
    bool listed = p.steps > 0;
    switch (c) {
        case C::PpsMonotonic: case C::AvsMonotonic:
            listed = listed && p.min_delta_mv != 0x7FFF;
            value = quantize(sc::Unit::Mv, p.min_delta_mv);
            fails = p.mono_fails;
            break;
        case C::PpsAccuracy: case C::AvsAccuracy:
            value = quantize(sc::Unit::Mv, p.worst_err_mv);
            fails = p.acc_fails;
            break;
        case C::PpsSettle:
            listed = listed && cls == 0;
            value = us(p.band_100us);
            fails = p.band_fails;
            break;
        case C::PpsBand:
            listed = listed && cls != 0;
            value = us(p.band_100us);
            fails = p.band_fails;
            break;
        case C::AvsBand:
            value = us(p.band_100us);
            fails = p.band_fails;
            break;
        case C::PpsPsRdy: case C::AvsPsRdy:
            listed = listed || p.psrdy_100us != 0;   // the Accept came even when the step could not be judged
            value = us(p.psrdy_100us);
            fails = p.psrdy_fails;
            break;
        case C::PpsOvershoot: case C::AvsOvershoot:
            listed = listed && p.over_mv != -0x7FFF;
            value = quantize(sc::Unit::Mv, p.over_mv);
            fails = p.over_fails;
            break;
        case C::PpsTransient: case C::AvsTransient:
            value = us(p.out_100us);
            fails = p.out_fails;
            break;
        default: return false;
    }
    if (!listed) return false;
    r.check = static_cast<std::uint8_t>(c);
    r.flags = static_cast<std::uint8_t>(kRowProg | cls << 2 | (fails ? kRowFail : 0));
    r.a = p.lo_mv;
    r.b = p.hi_mv;
    r.value = value;
    r.margin = fails;
    return true;
}

// Calls f(row) for the rows of one cell in record order.
template <class F>
void for_rows(const pdspec::Engine& e, std::size_t cell, F&& f) {
    if (!has_list(cell)) return;
    const sc::Check c = kOrder[cell];
    if (c < sc::Check::PpsMonotonic) {
        for (const sc::TransRec& t : e.trans()) {
            const std::uint16_t from = e.fixed_mv(t.from), to = e.fixed_mv(t.to);
            Fixed fx;
            if (fixed_of(c, t, from, to, fx)) f(make_fixed(c, t, from, to, fx));
        }
    } else {
        for (const sc::PassRec& p : e.passes()) {
            Row r;
            if (pass_row(c, p, r)) f(r);
        }
    }
}

// A row's rank for "the worst": failing first, then the larger margin (a never event ranks highest of its kind).
constexpr std::int32_t rank(const Row& r) { return ((r.flags & kRowFail) ? (1 << 20) : 0) + r.margin; }

}  // namespace rows_detail

inline std::size_t row_count(const pdspec::Engine& e, std::size_t cell) {
    std::size_t n = 0;
    rows_detail::for_rows(e, cell, [&n](const Row&) { ++n; });
    return n;
}

inline bool row_at(const pdspec::Engine& e, std::size_t cell, std::size_t k, Row& out) {
    std::size_t i = 0;
    bool got = false;
    rows_detail::for_rows(e, cell, [&](const Row& r) { if (i++ == k) { out = r; got = true; } });
    return got;
}

// The index (within the cell's rows) of the worst one; 0 when the cell lists nothing.
inline std::size_t worst_row(const pdspec::Engine& e, std::size_t cell) {
    std::size_t i = 0, best = 0;
    std::int32_t best_rank = INT32_MIN;
    rows_detail::for_rows(e, cell, [&](const Row& r) {
        if (rows_detail::rank(r) > best_rank) { best_rank = rows_detail::rank(r); best = i; }
        ++i;
    });
    return best;
}

inline std::size_t rows_total(const pdspec::Engine& e) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kCells; ++i) n += row_count(e, i);
    return n < kRowsMax ? n : kRowsMax;
}
inline std::size_t rows_file_size(const pdspec::Engine& e) { return kRowsHdr + rows_total(e) * kRowBytes; }

// Bytes [off, off + n) of the file the engine's records make (the journal writes it in pieces). Rows past the cap are not listed.
inline void rows_fill(const pdspec::Engine& e, std::uint32_t run_id, std::size_t off, std::uint8_t* dst, std::size_t n) {
    std::array<std::uint16_t, kCells> counts{};
    std::array<std::uint8_t, kCells> worst{};
    std::size_t sum = 0;
    for (std::size_t i = 0; i < kCells; ++i) {
        std::size_t c = row_count(e, i);
        if (sum + c > kRowsMax) c = kRowsMax - sum;
        counts[i] = static_cast<std::uint16_t>(c);
        worst[i] = static_cast<std::uint8_t>(c ? worst_row(e, i) : 0);
        sum += c;
    }
    for (std::size_t at = 0; at < n;) {
        const std::size_t pos = off + at;
        if (pos < kRowsHdr) {
            std::array<std::uint8_t, kRowsHdr> h{};
            rows_put_header(h.data(), run_id, counts, worst);
            const std::size_t take = std::min(n - at, kRowsHdr - pos);
            for (std::size_t i = 0; i < take; ++i) dst[at + i] = h[pos + i];
            at += take;
            continue;
        }
        // Which row, and which byte of it.
        std::size_t g = (pos - kRowsHdr) / kRowBytes, in = (pos - kRowsHdr) % kRowBytes, cell = 0;
        while (cell < kCells && g >= counts[cell]) { g -= counts[cell]; ++cell; }
        std::array<std::uint8_t, kRowBytes> b{};
        Row r;
        if (cell < kCells && row_at(e, cell, g, r)) row_encode(r, b.data());
        const std::size_t take = std::min(n - at, kRowBytes - in);
        for (std::size_t i = 0; i < take; ++i) dst[at + i] = b[in + i];
        at += take;
    }
}

}  // namespace k2::specsec
