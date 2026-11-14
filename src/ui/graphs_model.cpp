// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/graphs_model.hpp"

#include "k2/core/math_opt.hpp"
#include "k2/ui/home_cards.hpp"

namespace k2::graphs {

namespace {
template <size_t N>
uint32_t pick(const uint32_t (&list)[N], uint32_t x) {
    for (uint32_t v : list) if (v >= x) return v;
    return list[N - 1];
}
int put(std::span<char> out, std::string_view s) {
    size_t n = 0;
    for (; n < s.size() && n < out.size(); n++) out[n] = s[n];
    return static_cast<int>(n);
}
}  // namespace

uint32_t scale_w10(uint32_t data_max10, uint32_t limit_max_w) {
    static constexpr uint32_t kW10[] = {1000, 1500, 2000, 3000, 4500, 6000, 6500, 10000, 14000, 24000};
    const uint32_t lim10 = limit_max_w * 100u;
    return pick(kW10, data_max10 > lim10 ? data_max10 : lim10);
}

uint32_t scale_v10(uint32_t data_max10) {
    static constexpr uint32_t kV10[] = {600, 1000, 1300, 1600, 2100, 2900, 3700, 4900};
    return pick(kV10, (data_max10 * 102u + 99u) / 100u);
}

uint32_t scale_a10(uint32_t data_max10) {
    static constexpr uint32_t kA10[] = {50, 100, 200, 300, 350, 550, 650, 1000};
    return pick(kA10, (data_max10 * 105u + 99u) / 100u);
}

uint32_t nice_step(uint32_t x) {
    uint32_t p = 1;
    while (p * 10u < x && p < 100000000u) p *= 10u;
    if (p >= x) return p;
    if (2u * p >= x) return 2u * p;
    if (5u * p >= x) return 5u * p;
    return 10u * p;
}

uint32_t wh_top(uint32_t max_units, uint32_t unit_mwh) {
    const uint32_t min_step = unit_mwh >= 5u ? 1u : 5u / unit_mwh;
    const uint32_t third = (max_units + 2u) / 3u;
    const uint32_t st = nice_step(third > min_step ? third : min_step);
    const uint32_t top = (max_units + st - 1u) / st * st;
    return top > st ? top : st;
}

int span_text(std::span<char> out, uint32_t seconds) {
    StrBuf<10> s;
    if (seconds < 120) s.put_u32(seconds).put_char('s');
    else if (seconds < 3600) s.put_u32(seconds / 60).put_char('m');
    else {
        s.put_u32(seconds / 3600).put_char('h');
        if (const uint32_t m = seconds % 3600 / 60) s.put_u32(m).put_char('m');
    }
    return put(out, s.view());
}

int value_text(Band b, int32_t milli, std::span<char> out) {
    return b == Band::V || b == Band::Wh ? home::fmt_hero(out, milli) : home::fmt_watts(out, milli);
}

void max_text(Band b, uint32_t max_milli, StrBuf<8> &out) {
    out.clear();
    if (b == Band::Wh) return;
    out.put_char('/');
    if (max_milli == 0) { out.put("--"); return; }
    if (b == Band::W) { out.put_u32((max_milli + 500u) / 1000u).put_char('W'); return; }
    char t[12];
    const int n = home::fmt_level(t, max_milli);
    out.put({t, static_cast<size_t>(n)}).put(unit_text(b));
}

uint32_t limit_mw(const home::ProtoInfo &p) {
    if (p.kind == home::ProtoKind::Pps && !p.measured) return math::mw_of(p.mv ? p.mv : p.hi_mv, p.ma);
    return home::contract_mw(p);
}

InfoLayout info_layout(size_t word_chars, size_t proto_chars, size_t elapsed_chars, int16_t right) {
    constexpr int kAdv = 8;
    InfoLayout l{-1, elapsed_chars > 0};
    if (proto_chars == 0) return l;
    const int wend = kWordX + static_cast<int>(word_chars) * kAdv;      // one past the word's last pixel
    const int x = wend <= kProtoX ? kProtoX : wend + kAdv;
    const int end = x + static_cast<int>(proto_chars) * kAdv;            // one past the protocol's last pixel
    const int el_x = right + 1 - static_cast<int>(elapsed_chars) * kAdv;
    if (end > right + 1) return l;                                    // past the content edge: the word keeps the line
    if (x != kProtoX && l.elapsed && end + kAdv > el_x) return l;        // behind a long word: the protocol yields
    l.proto_x = static_cast<int16_t>(x);
    if (l.elapsed && end + kAdv > el_x) l.elapsed = false;               // at x52 it keeps its place, the elapsed time goes
    return l;
}

}  // namespace k2::graphs
