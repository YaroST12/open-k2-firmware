// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/sweep.hpp"
#include <string_view>

namespace k2::ctsweep {

unsigned plan_total(const pd::SourceCaps& caps, unsigned cap_mv, Kind kind) {
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
        const unsigned c = point_count(point_range(p.min_mv, p.max_mv, cap_mv));
        if (total >= kMaxPoints) break;
        total += std::min(c, kMaxPoints - total);
    }
    return total;
}

Summary summarize(std::span<const int16_t> err, unsigned lo_mv) {
    Summary s;
    s.points = uint16_t(err.size());
    if (err.empty()) return s;
    int worst_abs = -1;
    int prev_measured = -1;
    unsigned fold_at = 0;   // index of the first falling point
    for (size_t i = 0; i < err.size(); ++i) {
        const unsigned set = lo_mv + unsigned(i) * kStepMv;
        const int measured = int(err[i]) + int(set);
        const int e = int(err[i]);
        if (i == 0 || e < s.min_err) s.min_err = int16_t(e);
        if (i == 0 || e > s.max_err) s.max_err = int16_t(e);
        if (std::abs(e) > worst_abs) { worst_abs = std::abs(e); s.worst_err = int16_t(e); s.worst_mv = uint16_t(set); }
        if (i == 0 || measured > s.peak_mv) { s.peak_mv = uint16_t(measured); s.peak_at_mv = uint16_t(set); }
        if (i > 0 && prev_measured >= 0 && measured < prev_measured && !fold_at) fold_at = unsigned(i);
        prev_measured = measured;
    }
    if (fold_at) {
        s.fold_from_mv = uint16_t(lo_mv + fold_at * kStepMv);
        int low = 0;
        for (size_t i = fold_at; i < err.size(); ++i) {
            const int measured = int(err[i]) + int(lo_mv + i * kStepMv);
            if (i == fold_at || measured < low) low = measured;
        }
        s.fold_mv = uint16_t(std::max(low, 0));
    }
    // A point outside the row's band (the PPS band is one-sided: within 5 % / 100 mV below, the ceiling above) was
    // steady and recorded; the summary says so, the strip colours it.
    for (size_t i = 0; i < err.size(); ++i) {
        const unsigned set = lo_mv + unsigned(i) * kStepMv;
        const int measured = int(err[i]) + int(set);
        if (measured < int(set) - int(std::max(100u, set / 20))) { s.out_of_band = true; break; }
    }
    return s;
}

bool out_of_band(unsigned, int) { return false; }   // red: filled in with the live view
size_t format_point(unsigned, int, std::span<char>) { return 0; }

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
    auto put = [&](std::string_view t) { for (char c : t) if (n + 1 < out.size()) out[n++] = c; };
    auto mv = [&](unsigned v) { char b[12]; const size_t k = format_mv(v, {b, sizeof b}); put({b, k}); };
    if (!s.points) { put("no points"); }
    else {
        put("peak "); mv(s.peak_mv); put(" V at "); mv(s.peak_at_mv); put(" V");
        if (s.fold_mv) { put(", folds back to "); mv(s.fold_mv); put(" V from "); mv(s.fold_from_mv); put(" V"); }
    }
    if (n < out.size()) out[n] = 0;
    return n;
}

}  // namespace k2::ctsweep
