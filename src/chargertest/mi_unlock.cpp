// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/mi_unlock.hpp"

#include <algorithm>

#include "k2/chargertest/step_label.hpp"
#include "k2/pd/pd.hpp"

namespace k2::mi_unlock {
namespace {
using pd::PdoType;

struct Out {
    step_label::Text& t;
    void put(char c) { if (t.len + 1 < step_label::kMax) t.s[t.len++] = c; t.s[t.len] = 0; }
    void put(const char* s) { while (*s) put(*s++); }
    void num(unsigned v) { char d[10]; int k = 0; do { d[k++] = char('0' + v % 10); v /= 10; } while (v && k < 10); while (k) put(d[--k]); }
    void milli(unsigned v) {   // 5000 -> "5", 3250 -> "3.25"
        num(v / 1000);
        if (const unsigned frac = v % 1000 / 10) { put('.'); put(char('0' + frac / 10)); if (frac % 10) put(char('0' + frac % 10)); }
    }
    void arrow() { put("\xE2\x86\x92"); }   // U+2192, one glyph cell
    bool top = false;   // the short form: a range by its top voltage alone
    void volts(const pd::Pdo& p) {
        if (p.type == PdoType::Fixed || top) { milli(p.max_mv); put('V'); return; }
        milli(p.min_mv); put('-'); milli(p.max_mv); put('V');
    }
    // The size of the offer: amps, or watts for an AVS APDO (its limit is a power).
    void size(const pd::Pdo& p) {
        if (p.type == PdoType::Avs) { num(p.max_mw / 1000); put('W'); return; }
        milli(p.max_ma); put('A');
    }
};
// Glyph cells of a text: a UTF-8 sequence (the arrow) is one cell.
unsigned cells(const step_label::Text& t) {
    unsigned n = 0;
    for (size_t i = 0; i < t.len; ++i) n += (static_cast<unsigned char>(t.s[i]) & 0xC0) != 0x80;
    return n;
}
bool known(const pd::Pdo& p) { return p.type == PdoType::Fixed || p.type == PdoType::Pps || p.type == PdoType::Avs; }
const char* proto(PdoType t) { return t == PdoType::Fixed ? "PD" : t == PdoType::Pps ? "PPS" : "AVS"; }
bool same_range(const pd::Pdo& a, const pd::Pdo& b) { return a.min_mv == b.min_mv && a.max_mv == b.max_mv; }
bool same_size(const pd::Pdo& a, const pd::Pdo& b) { return a.type == PdoType::Avs ? a.max_mw == b.max_mw : a.max_ma == b.max_ma; }
}  // namespace

bool raised(uint32_t before, uint32_t after) {
    const pd::Pdo b = pd::decode_pdo(before), a = pd::decode_pdo(after);
    if (!known(a) || a.type != b.type) return false;
    if (a.type == PdoType::Fixed) return a.max_mv == b.max_mv && (a.max_ma > b.max_ma || a.max_mw > b.max_mw);
    return a.max_ma > b.max_ma || a.max_mw > b.max_mw || a.max_mv > b.max_mv;
}

Diff diff(std::span<const uint32_t> before, std::span<const uint32_t> after) {
    Diff d;
    const size_t nb = std::min(before.size(), kSprSlots), na = std::min(after.size(), kSprSlots);
    // The before PDO each after PDO stands for (-1 = none): the same kind and voltage over the whole list first, then (PPS / AVS)
    // one that shares an end of the range (a range that grew), so an APDO inserted ahead never takes another's partner.
    bool used[kSprSlots] = {};
    int match[kSprSlots];
    std::fill(std::begin(match), std::end(match), -1);
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < na; ++i) {
            const pd::Pdo a = pd::decode_pdo(after[i]);
            if (!after[i] || !known(a) || match[i] >= 0) continue;
            for (size_t j = 0; j < nb; ++j) {
                if (used[j] || !before[j]) continue;
                const pd::Pdo b = pd::decode_pdo(before[j]);
                if (b.type != a.type) continue;
                if (pass == 0 ? same_range(a, b) : a.type != PdoType::Fixed && (a.min_mv == b.min_mv || a.max_mv == b.max_mv)) {
                    used[j] = true; match[i] = int(j); break;
                }
            }
        }
    for (size_t i = 0; i < na && d.n < kMaxModes; ++i) {
        if (!after[i] || !known(pd::decode_pdo(after[i]))) continue;
        if (match[i] < 0) { d.modes[d.n++] = {0, after[i], uint8_t(i + 1)}; continue; }
        const uint32_t b = before[size_t(match[i])];
        if (raised(b, after[i])) d.modes[d.n++] = {b, after[i], uint8_t(i + 1)};
    }
    return d;
}

Point test_point(uint32_t after) {
    const pd::Pdo p = pd::decode_pdo(after);
    if (!known(p)) return {};
    return {p.max_mv, pd::current_limit_ma(p, p.max_mv)};
}

unsigned point_w(uint32_t after) {
    const Point p = test_point(after);
    return unsigned(p.mv) * p.ma / 1000000u;
}

namespace {
step_label::Text mode_form(const Mode& m, bool fresh, bool top) {
    step_label::Text t;
    Out o{t, top};
    const pd::Pdo a = pd::decode_pdo(m.after);
    if (!known(a)) return t;
    o.put(proto(a.type)); o.put(' ');
    if (!m.before) {
        o.volts(a); o.put(' '); o.size(a);
        if (fresh) o.put(" new");
        return t;
    }
    const pd::Pdo b = pd::decode_pdo(m.before);
    if (same_range(a, b)) { o.volts(a); o.put(' '); o.size(b); o.arrow(); o.size(a); return t; }
    if (same_size(a, b)) {   // the range grew: "PPS 5-11→20V 3A" when the minimum stayed
        if (a.min_mv == b.min_mv && !top) { o.milli(a.min_mv); o.put('-'); o.milli(b.max_mv); }
        else o.milli(b.max_mv);
        o.arrow(); o.milli(a.max_mv); o.put("V "); o.size(a);
        return t;
    }
    o.arrow(); o.volts(a); o.put(' '); o.size(a);   // range and size both changed: the new offer ("PPS →5-20V 5A")
    return t;
}
}  // namespace

step_label::Text mode_text(const Mode& m, bool fresh) {
    const step_label::Text t = mode_form(m, fresh, false);
    return cells(t) <= step_label::kRowCells ? t : mode_form(m, fresh, true);   // too wide for a Steps row: the range by its top
}

uint8_t chain_code(bool svid, bool authed, uint8_t last_step, uint8_t finish) {
    if (!svid) return kCodeNoSvid;
    if (authed) return finish == 0x0A ? kCodeFinished : kCodeVerified;
    if (!last_step) return kCodeNoAnswer;
    return uint8_t(kCodeStalled + std::min<uint8_t>(last_step, 10));   // 4..13
}

Chain chain_of(uint8_t code) {
    if (code == kCodeNoSvid) return Chain::NoSvid;
    if (code == kCodeNoAnswer) return Chain::NoAnswer;
    if (code == kCodeVerified) return Chain::Verified;
    if (code == kCodeFinished) return Chain::Finished;
    if (code > kCodeStalled && code <= kCodeStalled + 10) return Chain::Stalled;
    return Chain::None;
}

uint8_t chain_step(uint8_t code) { return chain_of(code) == Chain::Stalled ? uint8_t(code - kCodeStalled) : uint8_t(0); }

step_label::Text chain_text(uint8_t code) {
    step_label::Text t;
    Out o{t};
    switch (chain_of(code)) {
    case Chain::NoSvid: o.put("no 0x2717 SVID"); break;
    case Chain::NoAnswer: o.put("no answer"); break;
    case Chain::Stalled: o.put("failed at step "); o.num(chain_step(code)); break;
    case Chain::Verified: o.put("done, verified"); break;
    case Chain::Finished: o.put("done via 020A"); break;
    default: break;
    }
    return t;
}

}  // namespace k2::mi_unlock
