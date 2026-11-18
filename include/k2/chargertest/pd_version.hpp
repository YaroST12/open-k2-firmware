// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The PD version a charger-test run shows ("PD3.0") and the Results headline that leads with it. Pure values, no hardware:
// tools/charger_summary.py and tools/web/ctgrid.js mirror derive() and the golden cases are tests/fixtures/pd_versions.txt.
//
// What the offer proves: an SPR AVS APDO is PD 3.2, any EPR (the EPR table, or EPR AVS) is PD 3.1, a PPS APDO is PD 3.0, a
// header of spec revision 3.x with none of those is PD 3.0, a 2.0 header is PD 2.0, and no PD at all shows nothing. A
// Get_Revision answer (RMDO) wins when it is as high as that, and Revision is itself a 3.1 message, so an answer below 3.1 is
// also under what the charger proved by sending it. An answer under the proof shows the proof, marked "*" (a charger
// that under-reports: docs/CHARGER_QUIRKS.md `revision-below-offer`); the detail text then says both.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace k2::pd_version {

enum class Basis : uint8_t { None, Header2, Header3, Pps, Epr, SprAvs, RevisionMsg };

struct Inputs {
    uint8_t wire_rev = 0;          // Source_Capabilities header spec revision: 0 unknown, 1 PD2.0, 2 PD3.x
    bool rmdo = false;             // a Get_Revision answer was read
    uint8_t rmdo_major = 0, rmdo_minor = 0;
    bool pps = false, epr = false, spr_avs = false;   // the offer: a PPS APDO, EPR (table or EPR AVS), an SPR AVS APDO
};

struct Version {
    uint8_t major = 0, minor = 0;  // the version shown; major 0 = none (no PD)
    Basis basis = Basis::None;     // what proved it (the offer, or the Revision message itself)
    bool mark = false;             // an answer is lower than the proof: the proof is shown
    uint8_t ans_major = 0, ans_minor = 0;   // the answer, when mark is set
    constexpr bool valid() const { return major != 0; }
};

namespace detail {
constexpr unsigned code(unsigned major, unsigned minor) { return major * 16 + (minor & 0xF); }
}

constexpr Version derive(const Inputs& in) {
    unsigned proof = 0;
    Basis basis = Basis::None;
    if (in.spr_avs) { proof = detail::code(3, 2); basis = Basis::SprAvs; }
    else if (in.epr) { proof = detail::code(3, 1); basis = Basis::Epr; }
    else if (in.pps) { proof = detail::code(3, 0); basis = Basis::Pps; }
    else if (in.wire_rev == 2) { proof = detail::code(3, 0); basis = Basis::Header3; }
    else if (in.wire_rev == 1) { proof = detail::code(2, 0); basis = Basis::Header2; }
    if (in.rmdo && proof < detail::code(3, 1)) { proof = detail::code(3, 1); basis = Basis::RevisionMsg; }
    Version v;
    if (in.rmdo && detail::code(in.rmdo_major, in.rmdo_minor) >= proof) {
        v.major = in.rmdo_major; v.minor = in.rmdo_minor & 0xF; v.basis = basis;
        return v;
    }
    if (proof == 0) return v;
    v.major = uint8_t(proof >> 4); v.minor = uint8_t(proof & 0xF); v.basis = basis;
    if (in.rmdo) { v.mark = true; v.ans_major = in.rmdo_major; v.ans_minor = in.rmdo_minor & 0xF; }
    return v;
}

inline constexpr size_t kMaxLabel = 8;   // "PD15.15*"
inline constexpr size_t kMaxText = 12;

struct Text {
    char s[kMaxText] = {};
    size_t len = 0;
    std::string_view view() const { return {s, len}; }
};

namespace detail {
constexpr void put_num(Text& t, unsigned n) {
    if (n >= 10) t.s[t.len++] = char('0' + n / 10);
    t.s[t.len++] = char('0' + n % 10);
}
}

// "PD3.2", "PD3.2*" for a marked one; empty for none.
constexpr Text label(const Version& v) {
    Text t;
    if (!v.valid()) return t;
    t.s[t.len++] = 'P'; t.s[t.len++] = 'D';
    detail::put_num(t, v.major); t.s[t.len++] = '.'; detail::put_num(t, v.minor);
    if (v.mark) t.s[t.len++] = '*';
    return t;
}

inline constexpr size_t kMaxDetail = 20;   // the value column holds 16
struct Detail {
    char s[kMaxDetail] = {};
    size_t len = 0;
    std::string_view view() const { return {s, len}; }
};

// The Detail page's "Revision" row when an answer is under the proof: "3.1 < AVS 3.2" (the answer, then what proved more). Empty
// when nothing conflicts (the page keeps its Header row). The table's value column is 16 characters; an answer under the proof is
// at most "2.15 < AVS 3.2".
constexpr Detail conflict(const Version& v) {
    Detail d;
    if (!v.mark) return d;
    auto put = [&d](std::string_view w) { for (const char c : w) d.s[d.len++] = c; };
    auto num = [&d](unsigned n) { if (n >= 10) d.s[d.len++] = char('0' + n / 10); d.s[d.len++] = char('0' + n % 10); };
    num(v.ans_major); d.s[d.len++] = '.'; num(v.ans_minor); put(" < ");
    switch (v.basis) {
    case Basis::SprAvs: put("AVS "); break;
    case Basis::Epr: put("EPR "); break;
    default: put("msg "); break;   // the Revision message itself (a 3.1 message)
    }
    num(v.major); d.s[d.len++] = '.'; num(v.minor);
    return d;
}

// The headline: the version first, then the items, two spaces apart, at most max_chars. Items are dropped from the end; the
// version never is. Returns the characters written (no terminator beyond cap-1). With no items the text says so.
inline size_t headline(char* out, size_t cap, std::string_view version, const std::string_view* items, size_t n,
                       size_t max_chars) {
    size_t len = 0;
    auto put = [&](std::string_view s) { for (const char c : s) if (len + 1 < cap) out[len++] = c; };
    put(version);
    for (size_t i = 0; i < n; ++i) {
        const size_t sep = len ? 2 : 0;
        if (len + sep + items[i].size() > max_chars) break;
        if (sep) put("  ");
        put(items[i]);
    }
    if (cap) out[len] = '\0';
    return len;
}

}   // namespace k2::pd_version
