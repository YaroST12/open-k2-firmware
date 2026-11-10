// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/step_label.hpp"
#include "k2/pd/pd.hpp"

namespace k2::step_label {
namespace ct = charger_test;
using A = ct::Algorithm;
namespace {
struct Out {
    Text& t;
    void put(char c) { if (t.len + 1 < kMax) t.s[t.len++] = c; t.s[t.len] = 0; }
    void put(const char* s) { while (*s) put(*s++); }
    void num(unsigned v) { char d[10]; int k = 0; do { d[k++] = char('0' + v % 10); v /= 10; } while (v && k < 10); while (k) put(d[--k]); }
    // 5000 -> "5", 3300 -> "3.3", 12140 -> "12.14" (volts or amps from milli-units, up to two decimals)
    void milli(unsigned v) {
        num(v / 1000);
        if (const unsigned frac = v % 1000 / 10) {
            put('.'); put(char('0' + frac / 10));
            if (frac % 10) put(char('0' + frac % 10));
        }
    }
};
bool is_avs(const ct::Step& s) { return s.algorithm == A::SprAvs || s.algorithm == A::EprAvs || (s.algorithm == A::Epr && s.avs); }
// The offers whose rows ramp (one sequence of targets per PDO position).
bool is_programmable(const ct::Step& s) { return s.algorithm == A::Pps || is_avs(s); }
}  // namespace

Text protocol(const ct::Step& s) {
    Text t;
    Out o{t};
    if (s.hunt) { o.put("Mi"); return t; }
    switch (s.algorithm) {
    case A::Pd2Fixed: o.put(s.mv ? "PD" : "PD2.0"); break;
    case A::Pd3Fixed: o.put(s.mv ? "PD" : "PD3.x"); break;
    case A::Epr: o.put(s.avs ? "AVS" : "EPR"); break;
    case A::SprAvs: case A::EprAvs: o.put("AVS"); break;
    case A::Pps: o.put("PPS"); break;
    case A::Pd30Revision: o.put("PD3.0 rev"); break;
    case A::Pd31Revision: o.put("PD3.1 rev"); break;
    case A::Pd32Revision: o.put("PD3.2 rev"); break;
    case A::Qc2ClassB: o.put("QC2B"); break;
    case A::Qc3ClassB: o.put("QC3B"); break;
    default: o.put(ct::name(s.group)); break;
    }
    return t;
}

unsigned amps_ma(const ct::Step& s) {
    if (s.hunt) return s.ma;
    // The full-current PPS row asks the APDO's whole current (cable-clamped, so it can be below the advertised one): the label
    // says what was requested. The other PPS rows request at most 1 A and show the advertised current.
    if (s.algorithm == A::Pps && s.ma > ct::kPlanRowMa) return s.ma;
    switch (s.algorithm) {
    case A::Pd2Fixed: case A::Pd3Fixed: case A::Pps: case A::Epr: {
        if (s.avs || !s.pdo) return 0;
        const pd::Pdo p = pd::decode_pdo(s.pdo);
        return p.type == pd::PdoType::Fixed || p.type == pd::PdoType::Pps ? p.max_ma : 0u;
    }
    case A::Scp: return s.ma;
    default: return 0;
    }
}

unsigned ufcs_amps_ma(const ct::Step& s) {
    ct::UfcsMode m;
    return ct::ufcs_mode_of(s, m) ? m.max_ma : 0u;
}

Text ufcs_level(const ct::Step& s) {
    Text t = params(s);
    if (const unsigned ma = ufcs_amps_ma(s); ma && t.len) { Out o{t}; o.put(' '); o.milli(ma); o.put('A'); }
    return t;
}

Text ufcs_range(const ct::Step& s) {
    Text t;
    ct::UfcsMode m;
    if (!ct::ufcs_mode_of(s, m)) return t;
    // "5-20V 0.5-3A"; a mode of one current, or one whose range would not fit the Detail value column (16 cells), reads the top only.
    for (const bool low : {true, false}) {
        t = {};
        Out o{t};
        o.milli(m.min_mv); o.put('-'); o.milli(m.max_mv); o.put('V');
        if (m.max_ma) {
            o.put(' ');
            if (low && m.min_ma && m.min_ma != m.max_ma) { o.milli(m.min_ma); o.put('-'); }
            o.milli(m.max_ma); o.put('A');
        }
        if (!low || t.len <= kUfcsRangeCells) break;
    }
    return t;
}

Text params(const ct::Step& s, const ct::Step* prev) {
    Text t;
    if (!s.mv) return t;
    Out o{t};
    const bool ramp = s.continuous && prev && is_programmable(s) && prev->algorithm == s.algorithm && prev->pos == s.pos &&
                      !prev->hunt && !s.hunt && prev->mv && prev->mv != s.mv;
    if (ramp) {
        o.milli(prev->mv); o.put("\xE2\x86\x92"); o.milli(s.mv); o.put('V');   // U+2192
        return t;
    }
    o.milli(s.mv); o.put('V');
    if (const unsigned ma = amps_ma(s)) { o.put(' '); o.milli(ma); o.put('A'); }
    return t;
}

Text label(const ct::Step& s, const ct::Step* prev) {
    if (ct::is_sweep(s)) {   // the deep-sweep row names the APDO's whole range, not its first point ("PPS 3.3-21V", "AVS 9-20V")
        Text t = protocol(s);
        Out o{t};
        o.put(' ');
        const pd::Pdo p = pd::decode_pdo(s.pdo);
        const Text r = range(p.min_mv, p.max_mv, 0);
        o.put(r.c_str());
        return t;
    }
    Text t = protocol(s);
    const Text p = params(s, prev);
    Out o{t};
    if (p.len) { o.put(' '); o.put(p.c_str()); }
    return t;
}

Text range(unsigned min_mv, unsigned max_mv, unsigned ma, unsigned mw) {
    Text t;
    Out o{t};
    o.milli(min_mv); o.put('-'); o.milli(max_mv); o.put('V');
    if (ma) { o.put(' '); o.milli(ma); o.put('A'); }
    else if (mw) { o.put(' '); o.num(mw / 1000); o.put('W'); }
    return t;
}

Text range(uint32_t pdo) {
    if (!pdo) return {};
    const pd::Pdo p = pd::decode_pdo(pdo);
    return range(p.min_mv, p.max_mv, p.max_ma, p.max_mw);
}

}  // namespace k2::step_label
