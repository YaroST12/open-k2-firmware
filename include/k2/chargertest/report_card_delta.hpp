// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Report card delta: what changed between the last card of a charger and the run that just finished.
// Pure logic (tests/test_report_card_delta.cpp); the text is ASCII for the K2 screen.
//   group:  "PD: was FAIL, now PASS"          (both runs tested the group, the verdict differs)
//   range:  "PPS 13V: was FAIL, now PASS"      (the first failing target appeared or disappeared)
//           "PPS: fail 13V > 9V"               (it fails at another target)
//           "PPS max 21V > 20V"                (same failures, the highest passing voltage moved)
// A group that is None in either card was not part of that run and is no change; a range that is missing in
// one card is no change either.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "k2/chargertest/report_card.hpp"

namespace k2::report_card {

struct DeltaLine {
    enum Kind : uint8_t { Group, RangeFail, RangeMax } kind = Group;
    uint8_t index = 0;               // ct::Group or ct::Algorithm
    GV was = GV::None, now = GV::None;
    uint16_t was_dv = 0, now_dv = 0; // range values, 100 mV
};

namespace detail {
inline const char* group_label(unsigned g) {
    static constexpr const char* n[] = {"PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO"};
    return g < kGroups ? n[g] : "?";
}
inline const char* alg_label(unsigned a) {
    return a == unsigned(ct::Algorithm::Pps) ? "PPS" : a == unsigned(ct::Algorithm::SprAvs) ? "AVS" : a == unsigned(ct::Algorithm::EprAvs) ? "EAVS" : "RNG";
}
inline const char* gv_label(GV v) { return v == GV::Pass ? "PASS" : v == GV::Fail ? "FAIL" : v == GV::Skip ? "SKIP" : "-"; }
// Appends to a bounded buffer, always NUL terminated.
struct Out {
    char* p; size_t n, len = 0;
    void str(const char* s) { while (*s && len + 1 < n) p[len++] = *s++; p[len] = 0; }
    void num(unsigned v) {
        char t[10]; unsigned k = 0;
        do { t[k++] = char('0' + v % 10); v /= 10; } while (v && k < sizeof t);
        while (k) { if (len + 1 < n) p[len++] = t[--k]; else --k; }
        p[len] = 0;
    }
    void volts(unsigned dv) { num(dv / 10); if (dv % 10) { str("."); num(dv % 10); } str("V"); }
};
}  // namespace detail

// Writes up to out.size() lines; returns the count. Fixed groups first, then ranges.
inline size_t delta(const Card& was, const Card& now, std::span<DeltaLine> out) {
    size_t n = 0;
    for (unsigned g = 0; g < kGroups && n < out.size(); ++g) {
        const GV a = was.group[g].verdict, b = now.group[g].verdict;
        if (a == GV::None || b == GV::None || a == b) continue;
        out[n++] = {DeltaLine::Group, uint8_t(g), a, b, 0, 0};
    }
    for (unsigned i = 0; i < now.n_ranges && n < out.size(); ++i) {
        const Range& r = now.range[i];
        const Range* w = nullptr;
        for (unsigned j = 0; j < was.n_ranges; ++j) if (was.range[j].alg == r.alg && was.range[j].lo_dv == r.lo_dv && was.range[j].hi_dv == r.hi_dv) { w = &was.range[j]; break; }
        if (!w) continue;
        if (w->fail_dv != r.fail_dv) out[n++] = {DeltaLine::RangeFail, r.alg, GV::None, GV::None, w->fail_dv, r.fail_dv};
        else if (w->max_dv != r.max_dv) out[n++] = {DeltaLine::RangeMax, r.alg, GV::None, GV::None, w->max_dv, r.max_dv};
    }
    return n;
}

inline void format_delta(const DeltaLine& d, char* buf, size_t size) {
    detail::Out o{buf, size};
    if (size == 0) return;
    buf[0] = 0;
    if (d.kind == DeltaLine::Group) {
        o.str(detail::group_label(d.index)); o.str(": was "); o.str(detail::gv_label(d.was)); o.str(", now "); o.str(detail::gv_label(d.now));
        return;
    }
    o.str(detail::alg_label(d.index));
    if (d.kind == DeltaLine::RangeMax) { o.str(" max "); o.volts(d.was_dv); o.str(" > "); o.volts(d.now_dv); return; }
    if (d.was_dv && d.now_dv) { o.str(": fail "); o.volts(d.was_dv); o.str(" > "); o.volts(d.now_dv); return; }
    const unsigned dv = d.was_dv ? d.was_dv : d.now_dv;
    o.str(" "); o.volts(dv);
    o.str(d.was_dv ? ": was FAIL, now PASS" : ": was PASS, now FAIL");
}

}  // namespace k2::report_card
