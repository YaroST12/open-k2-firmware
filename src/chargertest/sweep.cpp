// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/sweep.hpp"
#include <string_view>
#include "k2/core/hot.h"
#include "k2/core/strpack.hpp"

namespace k2::ctsweep {

unsigned phone_wins(const pd::SourceCaps& caps, unsigned cap_mv, unsigned slot) {
    if (!caps.valid || slot < 1 || slot > 7 || slot > caps.num_pdos) return 0;
    const size_t n = std::min<size_t>(caps.num_pdos, 7);
    unsigned mask = 0;
    for (unsigned w = 0; w < kWinCount; ++w) {
        unsigned best_ma = 0, best_ov = 0;
        size_t best = 0;   // slot index + 1 of the winner
        for (size_t i = 0; i < n; ++i) {
            const auto& p = caps.pdos[i];
            if (!p.raw || !p.min_mv || p.type != pd::PdoType::Pps) continue;
            const unsigned lo = std::max<unsigned>(p.min_mv, kWinMv[w][0]), hi = std::min({unsigned(p.max_mv), cap_mv, kWinMv[w][1]});
            if (hi <= lo) continue;
            const unsigned ma = pd::current_limit_ma(p, uint16_t(hi)), ov = hi - lo;
            if (!best || ma > best_ma || (ma == best_ma && ov > best_ov)) { best = i + 1; best_ma = ma; best_ov = ov; }
        }
        if (best == slot) mask |= 1u << w;
    }
    return mask;
}

unsigned plan_total(const pd::SourceCaps& caps, unsigned cap_mv, Kind kind, Depth depth) {
    if (!caps.valid) return 0;
    unsigned total = 0;
    const size_t n = std::min<size_t>(caps.num_pdos, caps.pdos.size());
    for (size_t i = 0; i < n; ++i) {
        const auto& p = caps.pdos[i];
        if (!p.raw || !p.min_mv) continue;
        const bool epr_slot = i >= 7;   // slots 8+ are the EPR list's; they count only when it is in hand
        const bool want = kind == Kind::Pps ? (!epr_slot && p.type == pd::PdoType::Pps)
                                             : (p.type == pd::PdoType::Avs && (!epr_slot || caps.epr));
        if (!want) continue;
        const auto r = point_range(p.min_mv, p.max_mv, cap_mv);
        const unsigned c = point_count(r);   // the record's cells: the budget is memory, whatever the depth
        if (total >= kMaxPoints) break;
        // The points measured: both parts of the split row, each stepped by the depth (the top point included).
        unsigned n = 0;
        const unsigned limit = std::min<unsigned>(p.max_mv, cap_mv), wins = kind == Kind::Pps ? phone_wins(caps, cap_mv, unsigned(i + 1)) : 0u;
        for (const bool low : {false, true}) {
            const auto part = part_range(r, low);
            if (!part.empty()) n += plan_points(make_plan(depth, kind, part, limit, wins));
        }
        const unsigned room = kMaxPoints - total;
        total += c <= room ? n : n * room / c;
    }
    return total;
}

Summary summarize(std::span<const int16_t> err, unsigned lo_mv) { return summarize(err, lo_mv, {}, 0); }

// The points in setpoint order: segment a from a_lo_mv, then segment b from b_lo_mv (the parts of a split sweep; b empty for one row).
// A kHole cell (not measured at this depth) is skipped; the fold rule then judges neighbouring MEASURED points.
// K2_HOT keeps this file off the cold -Oz flags and flatten inlines TrackScan::feed; the per-point state lives in locals, stored
// once at the end (selftest sweep_sum).
K2_HOT [[gnu::flatten]] Summary summarize(std::span<const int16_t> a, unsigned a_lo_mv, std::span<const int16_t> b, unsigned b_lo_mv) {
    Summary s;
    const std::span<const int16_t> seg[2] = {a, b};
    const unsigned seg_lo[2] = {a_lo_mv, b_lo_mv};
    unsigned points = 0;
    int min_err = INT32_MAX, max_err = INT32_MIN, worst_abs = -1, worst_err = 0, peak = INT32_MIN;
    unsigned worst_mv = 0, peak_at = 0;
    bool oob = false;
    pd::TrackScan track;   // the shared fold / track-break rule (k2/pd/prog_track.hpp), fed in setpoint order
    // The first finding of either kind is the onset. A fold is a Fold, or a Break (what a 200 mV step shows of a fold: the output
    // moves +14 mV for a 200 mV request) that a later point then falls away from; a Break that stays level is no fold.
    unsigned onset_seg = 0, onset_j = 0, onset_set = 0;
    bool onset = false;
    unsigned onset_ok = 0, onset_err = 0;   // the last tracking setpoint and the worst |err| before the onset
    int onset_level = 0;
    unsigned prev_set = 0;   // the last measured setpoint (the fold's "last tracking" one)
    for (unsigned k = 0; k < 2; ++k) {
        const int16_t* p = seg[k].data();
        const unsigned n = unsigned(seg[k].size());
        unsigned set = seg_lo[k];
        for (unsigned j = 0; j < n; ++j, set += kStepMv) {
            const int e = p[j];
            if (e == kHole) continue;
            const int measured = e + int(set);
            ++points;
            if (e < min_err) min_err = e;
            if (e > max_err) max_err = e;
            // The fold rule runs before this point's own worst-error update, so the worst |err| held at the onset is the one over
            // the points BEFORE it (track_err_mv).
            track.feed(int(set), measured);
            if (!onset && track.found()) {
                onset = true; onset_seg = k; onset_j = j; onset_set = set; onset_ok = prev_set; onset_level = measured;
                onset_err = unsigned(std::max(worst_abs, 0));   // the points before the onset: how it tracked
            }
            const int ae = e < 0 ? -e : e;
            if (ae > worst_abs) { worst_abs = ae; worst_err = e; worst_mv = set; }
            if (measured > peak) { peak = int(uint16_t(measured)); peak_at = set; }   // compared as the stored 16-bit value
            // A point outside the row's band (the PPS band is one-sided: within 5 % / 100 mV below, the ceiling above) was
            // steady and recorded; the summary says so, the strip colours it.
            if (out_of_band(set, measured)) oob = true;
            prev_set = set;
        }
    }
    if (!points) return s;
    s.points = uint16_t(points);
    s.min_err = int16_t(min_err); s.max_err = int16_t(max_err);
    s.worst_err = int16_t(worst_err); s.worst_mv = uint16_t(worst_mv);
    s.peak_mv = uint16_t(peak); s.peak_at_mv = uint16_t(peak_at);
    s.out_of_band = oob;
    int low = 0;   // the lowest level from the onset on
    bool have = false;
    if (onset)
        for (unsigned k = onset_seg; k < 2; ++k) {
            const int16_t* p = seg[k].data();
            const unsigned n = unsigned(seg[k].size());
            for (unsigned j = k == onset_seg ? onset_j : 0u; j < n; ++j) {
                if (p[j] == kHole) continue;
                const int measured = p[j] + int(seg_lo[k] + j * kStepMv);
                if (!have || measured < low) { low = measured; have = true; }
            }
        }
    const bool fold = onset && (track.kind() == pd::Track::Fold || low < onset_level - pd::kTrackFoldMv);
    if (!fold) { s.track_err_mv = uint16_t(worst_abs); return s; }   // every point is tracking
    s.fold_from_mv = uint16_t(onset_set);
    s.fold_ok_mv = uint16_t(onset_ok);
    s.track_err_mv = uint16_t(onset_err);
    s.fold_mv = uint16_t(std::max(low, 0));
    return s;
}

std::optional<RowView> View::apdo(uint8_t alg, uint8_t pos) const {
    const RowView* lo = find(alg, pos, true);
    const RowView* hi = find(alg, pos, false);
    if (!lo || !hi) {
        if (const RowView* one = lo ? lo : hi) return *one;
        return std::nullopt;
    }
    auto pts = [&](const RowView& r) {
        const size_t first = std::min<size_t>(r.first, points.size());
        return points.subspan(first, std::min<size_t>(r.n, points.size() - first));
    };
    RowView m = *hi;
    m.lo_mv = lo->lo_mv;
    m.n = uint16_t(lo->n + hi->n);
    m.sum = summarize(pts(*lo), lo->lo_mv, pts(*hi), hi->lo_mv);
    // A located fold (Refiner) is finer than the cells the merge re-reads: it survives from whichever part found it.
    for (const RowView* r : {lo, hi})
        if (r->sum.fold_located && m.sum.fold_mv) {
            m.sum.fold_located = true; m.sum.fold_ok_mv = r->sum.fold_ok_mv; m.sum.fold_from_mv = r->sum.fold_from_mv;
            m.sum.fold_mv = std::min(m.sum.fold_mv, r->sum.fold_mv);
        }
    return m;
}

bool out_of_band(unsigned set_mv, int measured_mv) {
    return measured_mv < int(set_mv) - int(std::max(100u, set_mv / 20));
}

size_t format_point(unsigned set_mv, int measured_mv, std::span<char> out) {
    if (out.empty()) return 0;
    const unsigned got = measured_mv > 0 ? unsigned(measured_mv) : 0u;
    const int err = std::clamp(int(got) - int(set_mv), -9999, 9999);
    size_t n = 0;
    auto put = [&](std::string_view t) { strpack::for_chars(t, [&](char c) { if (n + 1 < out.size()) out[n++] = c; }); };
    auto num = [&](unsigned v, unsigned min_digits) {
        char t[10]; unsigned k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while ((v || k < min_digits) && k < sizeof t);
        while (k) { const char c = t[--k]; put({&c, 1}); }
    };
    auto volts = [&](unsigned mv) {   // 10 mV resolution, always two decimals: 12.30
        const unsigned r = (mv + 5) / 10;
        num(r / 100, 1); put("."); num(r % 100, 2); put("V");
    };
    put("req "); volts(set_mv);
    put(K2_P("  got ")); volts(got);
    put(K2_P("  err")); put(err < 0 ? "-" : "+"); num(unsigned(err < 0 ? -err : err), 1); put("mV");
    if (n < out.size()) out[n] = 0;
    return n;
}

size_t format_mv(unsigned mv, std::span<char> out) {
    if (out.empty()) return 0;
    const unsigned r = (mv + 5) / 10 * 10;   // 10 mV resolution
    unsigned whole = r / 1000, frac = (r / 10) % 100;
    size_t n = 0;
    auto put = [&](char c) { if (n + 1 < out.size()) out[n++] = c; };
    auto num = [&](unsigned v) {
        char t[8]; unsigned k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v && k < 8);
        while (k) put(t[--k]);
    };
    num(whole);
    put('.');
    const char d2 = char('0' + frac % 10), d1 = char('0' + frac / 10);
    if (frac % 10) { put(d1); put(d2); }   // two decimals: 19.75
    else { put(d1); }                        // one decimal kept: 19.9, 20.0
    if (n < out.size()) out[n] = 0;
    return n;
}

size_t format_summary(const Summary& s, std::span<char> out) {
    if (out.empty()) return 0;
    size_t n = 0;
    auto put = [&](std::string_view t) { strpack::for_chars(t, [&](char c) { if (n + 1 < out.size()) out[n++] = c; }); };
    auto mv = [&](unsigned v) { char b[12]; const size_t k = format_mv(v, {b, sizeof b}); put({b, k}); };
    if (!s.points) { put(K2_P("no points")); }
    else {
        put(K2_P("peak ")); mv(s.peak_mv); put(K2_P(" V at ")); mv(s.peak_at_mv); put(" V");
        if (s.fold_mv) { put(K2_P(", folds back to ")); mv(s.fold_mv); put(K2_P(" V from ")); mv(s.fold_from_mv); put(" V"); }
    }
    if (n < out.size()) out[n] = 0;
    return n;
}

// A level in mV as tenths of a volt, nearest: 19419 -> 194 ("19.4"). The card and the web round the same way (100 mV units).
size_t format_fold(const Summary& s, std::span<char> out) {
    if (out.empty()) return 0;
    if (!s.fold_mv) { out[0] = 0; return 0; }
    size_t n = 0;
    auto put = [&](std::string_view t) { strpack::for_chars(t, [&](char c) { if (n + 1 < out.size()) out[n++] = c; }); };
    auto tenths = [&](unsigned mv) {
        const unsigned t = (mv + 50) / 100;
        char d[8]; unsigned k = 0, w = t / 10;
        do { d[k++] = char('0' + w % 10); w /= 10; } while (w && k < sizeof d);
        while (k) { const char c = d[--k]; put({&c, 1}); }
        if (t % 10) { const char f[2] = {'.', char('0' + t % 10)}; put({f, 2}); }   // "19.4V", "20V" (the card's put_volts reads the same)
        put("V");
    };
    put(K2_P("folds to ")); tenths(s.fold_mv);
    put(K2_P(" above ")); tenths(s.fold_ok_mv ? s.fold_ok_mv : s.fold_from_mv > kStepMv ? s.fold_from_mv - kStepMv : 0u);
    if (n < out.size()) out[n] = 0;
    return n;
}

}  // namespace k2::ctsweep
