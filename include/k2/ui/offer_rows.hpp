// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The Charger tab's offer rows: SPR, EPR, PPS and AVS, one group each, and the headline watts. Pure logic over PDO values
// (host-tested in tests/test_offer_rows.cpp); the page turns each Line into a row.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "k2/pd/pd.hpp"
#include "k2/ui/ui_match.hpp"

namespace k2::offers {

inline constexpr std::size_t kSprPositions = 7;   // positions 1..7 are SPR, EPR ones start at position 8

enum class Kind : uint8_t { None, Spr, Epr, Pps, Avs };

// Where a PDO of the source belongs. pos0 is the 0-based position. Battery and variable supplies have no row (as before).
constexpr Kind kind_of(const pd::Pdo &p, std::size_t pos0) {
    switch (p.type) {
    case pd::PdoType::Fixed: return pos0 >= kSprPositions ? Kind::Epr : Kind::Spr;
    case pd::PdoType::Pps: return Kind::Pps;
    case pd::PdoType::Avs: return Kind::Avs;
    default: return Kind::None;
    }
}

// PDO1 of a PD 3.x source carries "EPR Mode Capable" in bit 23 (reserved in PD 2.0); EPR chunks prove it too.
inline bool epr_capable(const pd::SourceCaps &c) {
    return c.epr || (c.valid && c.spec_rev == 2 && c.num_pdos && c.pdos[0].type == pd::PdoType::Fixed && (c.pdos[0].raw & (1u << 23)));
}

// The PDO at a 0-based position: the caps' own, else, from position 8, the survey's EPR list (raw words, same positions).
inline pd::Pdo pdo_at(const pd::SourceCaps &c, std::size_t rows, std::span<const uint32_t> epr, std::size_t i) {
    if (i < rows && i < c.pdos.size() && c.pdos[i].raw != 0) return c.pdos[i];
    if (i >= kSprPositions && i < epr.size()) return pd::decode_pdo(epr[i]);
    return {};
}

// The headline watts: the charger's rating (Source_Capabilities_Extended PDP, the EPR one when it has it), else its best offer.
constexpr uint32_t hero_w(bool ext_valid, uint32_t pdp_w, uint32_t epr_pdp_w, uint32_t best_mw) {
    const uint32_t rated = ext_valid ? (epr_pdp_w > pdp_w ? epr_pdp_w : pdp_w) : 0;
    return rated ? rated : best_mw / 1000;
}

struct Summary { bool pps = false, avs = false, epr = false; };   // what the identity chips say

inline Summary summarize(const pd::SourceCaps &c, std::size_t rows, std::span<const uint32_t> epr) {
    Summary s;
    s.epr = c.epr;
    for (std::size_t i = 0; i < pd::kMaxPdos; i++) {
        const Kind k = kind_of(pdo_at(c, rows, epr, i), i);
        s.pps = s.pps || k == Kind::Pps;
        s.avs = s.avs || k == Kind::Avs;
        s.epr = s.epr || k == Kind::Epr;
    }
    return s;
}

// One row. cut > 0 splits the text there: the head in volts, the tail (a current, or the watts of an AVS) in its own colour.
struct Line {
    const char *key;
    std::string_view text;
    uint8_t cut;
    bool tail_watts;
    bool dim;
};

// "5/9/15V 3A": the fixed PDOs of one kind, a row per current and per line of `cells`; every row ends with its own current.
template <class Emit>
bool fixed_rows(Kind want, const char *key, const pd::SourceCaps &c, std::size_t rows, std::span<const uint32_t> epr,
                std::size_t cells, Emit &emit) {
    StrBuf<28> head;   // the voltages of the open row
    uint16_t cur_ma = 0;
    bool any = false;
    auto flush = [&] {
        if (head.view().empty()) return;
        StrBuf<28> v;
        v.put(head.view()).put_char('V');
        const uint8_t cut = static_cast<uint8_t>(v.view().size());
        v.put_char(' ');
        put_a(v, cur_ma);
        emit(Line{key, v.view(), cut, false, false});
        head.clear();
        any = true;
    };
    for (std::size_t i = 0; i < pd::kMaxPdos; i++) {
        const pd::Pdo p = pdo_at(c, rows, epr, i);
        if (kind_of(p, i) != want) continue;
        StrBuf<12> vs, as;
        put_v(vs, p.max_mv);
        put_a(as, p.max_ma);
        const std::size_t joined = head.view().size() + (head.view().empty() ? 0 : 1) + vs.view().size();
        if (!head.view().empty() && (p.max_ma != cur_ma || joined + 2 + as.view().size() > cells)) flush();
        if (!head.view().empty()) head.put_char('/');
        head.put(vs.view());
        cur_ma = p.max_ma;
    }
    flush();
    return any;
}

// "3.3-21V 5A" (PPS) or "15-28V 140W" (AVS): one row per APDO.
template <class Emit>
bool range_rows(Kind want, const char *key, const pd::SourceCaps &c, std::size_t rows, std::span<const uint32_t> epr, Emit &emit) {
    bool any = false;
    for (std::size_t i = 0; i < pd::kMaxPdos; i++) {
        const pd::Pdo p = pdo_at(c, rows, epr, i);
        if (kind_of(p, i) != want) continue;
        StrBuf<28> v;
        if (p.min_mv != p.max_mv) { put_v(v, p.min_mv); v.put_char('-'); }
        put_v(v, p.max_mv);
        v.put_char('V');
        const uint8_t cut = static_cast<uint8_t>(v.view().size());
        v.put_char(' ');
        if (want == Kind::Avs) v.put_u32(p.max_mw / 1000).put_char('W');
        else put_a(v, p.max_ma);
        emit(Line{key, v.view(), cut, want == Kind::Avs, false});
        any = true;
    }
    return any;
}

// The four groups, in order: SPR, EPR, PPS, AVS. A group with no offer is a dim "none"; an EPR-capable source whose EPR
// list was not read says why in `epr_word` (the survey's word, "" when it has none).
template <class Emit>
void rows(const pd::SourceCaps &c, std::size_t nrows, std::span<const uint32_t> epr, bool epr_is_capable, const char *epr_word,
          std::size_t cells, Emit &&emit) {
    auto none = [&](const char *key, const char *text) { emit(Line{key, text, 0, false, true}); };
    if (!fixed_rows(Kind::Spr, "SPR", c, nrows, epr, cells, emit)) none("SPR", "none");
    if (!fixed_rows(Kind::Epr, "EPR", c, nrows, epr, cells, emit)) none("EPR", epr_is_capable && epr_word[0] ? epr_word : "none");
    if (!range_rows(Kind::Pps, "PPS", c, nrows, epr, emit)) none("PPS", "none");
    if (!range_rows(Kind::Avs, "AVS", c, nrows, epr, emit)) none("AVS", "none");
}

}  // namespace k2::offers
