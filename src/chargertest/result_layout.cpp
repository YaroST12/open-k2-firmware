// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/result_layout.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/pd/pd.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/chargertest/step_label.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace k2::result_layout {
namespace ct = charger_test;
using A = ct::Algorithm;
namespace {
struct Out {
    char* p; size_t n; size_t len = 0;
    void put(char c) { if (len + 1 < n) p[len++] = c; p[len] = 0; }
    void put(const char* s) { while (*s) put(*s++); }
    void num(unsigned v) { char t[8]; int k = 0; do { t[k++] = char('0' + v % 10); v /= 10; } while (v && k < 8); while (k) put(t[--k]); }
};
// The chip words come from k2::step_label (one wording for the rows, the chips and the report card).
void put_text(Token& k, const char* s) { Out o{k.text, sizeof k.text}; o.put(s); }
// One step into a chip's tally. A Capped step is its own column: it is a fail by verdict, but the source delivered up to its
// ceiling, so the tone table reads it as "reached something" (amber), never as "none passed" (red).
void count(Tally& t, const ct::Step& y) {
    switch (y.verdict) {
    case ct::Verdict::Pass: ++t.pass; break;
    case ct::Verdict::Fail: y.reason == ct::Reason::Capped ? ++t.capped : ++t.fail; break;
    case ct::Verdict::Skip: ++t.skip; break;
    default: ++t.pending; break;
    }
}
Tone tone(const ct::Step& x) {   // a chip of one step (a fixed level, a 6 A probe)
    Tally t;
    count(t, x);
    return roll_up(t);
}
struct Builder {
    Layout& l;
    Row* row = nullptr;
    // Copies `s` into a fixed field; a text that does not fit is cut AND counted (Layout::cuts, a gate failure), never silent.
    void put(char* dst, size_t cap, std::string_view s) {
        const size_t n = std::min(s.size(), cap - 1);
        std::memcpy(dst, s.data(), n);
        dst[n] = 0;
        if (n < s.size() && l.cuts < 255) ++l.cuts;
    }
    void begin(std::string_view label, Tone label_tone, A a) {
        if (l.row_count >= l.rows.size()) { row = nullptr; return; }
        row = &l.rows[l.row_count++];
        *row = {};
        put(row->label, sizeof row->label, label);
        row->label_tone = label_tone; row->algorithm = a; row->first = static_cast<uint8_t>(l.token_count);
    }
    Token* token(bool chip, Tone t, bool glue = false) {
        if (!row || l.token_count >= l.tokens.size()) return nullptr;
        Token& k = l.tokens[l.token_count++];
        k = {}; k.chip = chip; k.tone = t; k.glue = glue; ++row->count;
        return &k;
    }
    void word(std::string_view s, Tone t) { if (Token* k = token(false, t)) put(k->text, sizeof k->text, s); }
    // `s` as plain tokens: whole words packed into tokens of at most 15 characters, so a long label wraps onto further grid
    // lines instead of being cut. `split_at_number`: the first word that starts with a digit opens a new token ("via PPS" |
    // "5-20V 5A"), keeping a range and its current together.
    void phrase(std::string_view s, Tone t, bool split_at_number) {
        constexpr size_t kMax = sizeof Token::text - 1;
        Token* cur = nullptr;
        size_t len = 0;
        bool numbered = false;
        while (!s.empty()) {
            while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
            const size_t sp = std::min(s.find(' '), s.size());
            const std::string_view w = s.substr(0, sp);
            s.remove_prefix(sp);
            if (w.empty()) break;
            if (split_at_number && !numbered && w.front() >= '0' && w.front() <= '9') { numbered = true; cur = nullptr; }
            if (cur && len + 1 + w.size() <= kMax) {
                cur->text[len++] = ' ';
                std::memcpy(cur->text + len, w.data(), w.size());
                len += w.size();
                cur->text[len] = 0;
            } else {
                cur = token(false, t);
                if (!cur) return;
                put(cur->text, sizeof cur->text, w);
                len = std::min(w.size(), kMax);
            }
        }
    }
};
bool is_revision(A a) { return a >= A::Pd30Revision && a <= A::Pd32Revision; }
bool is_range(A a) { return a == A::Pps || a == A::SprAvs || a == A::EprAvs; }
// Nothing to show for this protocol: every step is "not advertised", or a legacy protocol never answered at 5 V.
bool is_absent(std::span<const ct::Step> s, A a) {
    bool pass = false, all_none = true;
    for (const auto& x : s) {
        if (x.algorithm != a) continue;
        pass |= x.verdict == ct::Verdict::Pass;
        all_none &= x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::NoOffer;
    }
    if (pass) return false;
    if (all_none) return true;
    if (ct::group(a) == ct::Group::Pd) return false;
    for (const auto& x : s) if (x.algorithm == a) return x.verdict == ct::Verdict::Fail;   // the first (5 V) step failed
    return false;
}
bool is_skipped_only(std::span<const ct::Step> s, A a) {
    for (const auto& x : s) if (x.algorithm == a && x.verdict != ct::Verdict::Skip) return false;
    return true;
}
// A QC Class B (20 V) row the charger did not follow: a Class A charger. It keeps its own row with a neutral "Class A" chip.
bool is_class_a_only(std::span<const ct::Step> s, A a) {
    for (const auto& x : s) if (x.algorithm == a && x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::ClassAOnly) return true;
    return false;
}
// A QC3+ run the charger does not do (no movement, or QC3-sized steps): its raised rows are skipped "no QC3+". The row reads as
// one neutral word instead of chips, so the 5 V baseline that passed does not look like partial QC3+ support.
bool is_no_qc3plus(std::span<const ct::Step> s, A a) {
    for (const auto& x : s) if (x.algorithm == a && x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::NoQc3Plus) return true;
    return false;
}
// Why a skipped PD level did not run, in a word that sits next to its chip (the chip's colour alone does not say). Mirrors
// ct::explanation(): the monitor ceiling for an over-cap level (kDetailMonitor*), the source's refusal, the cable limit.
const char* skip_tag(const ct::Step& x) {
    if (x.verdict != ct::Verdict::Skip) return nullptr;
    switch (x.reason) {
    case ct::Reason::OverCap: return x.detail == ct::kDetailMonitor36 ? ">36V" : x.detail == ct::kDetailMonitor48 ? ">48V" : ">cap";
    case ct::Reason::CableLimit: return "cable";
    case ct::Reason::EprRefused: return "refused";
    case ct::Reason::NoContract: return "no PD";
    case ct::Reason::ClassAOnly: return "Class A";   // neutral: the charger tops out at 12 V, the 20 V level was never expected
    case ct::Reason::NotUnlocked: return x.detail == ct::kDetailCableCap ? "cable" : "locked";   // a 6 A hunt the source did not offer
    case ct::Reason::NeedsLoad: return "needs load";   // pattern pps-off-at-no-load: a skip, never a fail (10 chars: fits a token)
    default: return nullptr;
    }
}
void tag(Builder& b, const ct::Step& x) {
    if (const char* t = skip_tag(x))
        if (Token* k = b.token(false, Tone::Caution, true)) std::strncpy(k->text, t, sizeof k->text - 1);
}
void fixed_row(Builder& b, std::span<const ct::Step> s, A a) {
    for (const auto& x : s) {
        if (x.algorithm != a || !x.mv || x.hunt) continue;   // a 6 A probe is a row of its own (probe_row)
        Token* k = b.token(true, tone(x));
        if (!k) return;
        k->mv = x.mv;
        put_text(*k, step_label::params(x).c_str());   // "9V 3A", "20V 3.35A", "28V 5A"
        tag(b, x);
    }
}
// One chip for one APDO (`pos`): the row is one advertised range, so a probe the plan runs between two APDOs lands
// between their rows. Rolled up by the shared tone table (roll_up). PPS / AVS ranges are continuous; the steps only
// sample them. The chip text stays the advertised range: reach() says how far the source got.
void range_row(Builder& b, std::span<const ct::Step> s, A a, uint8_t pos) {
    for (const auto& x : s) {
        if (x.algorithm != a || !x.pdo || x.hunt || ct::is_sweep(x) || x.pos != pos) continue;
        Tally t;
        for (const auto& y : s)
            if (y.algorithm == a && y.pos == pos && !y.hunt && !ct::is_sweep(y)) count(t, y);   // probes and sweep rows are rows of their own
        const Tone tn = roll_up(t);
        Token* k = b.token(true, tn);
        if (!k) return;
        put_text(*k, step_label::range(x.pdo).c_str());   // "3.3-21V 5A", "15-28V 5A"
        if (tn == Tone::Skip) tag(b, x);   // every step was skipped: say why
        else {   // some levels ran: a "needs load" level (pattern pps-off-at-no-load) still says so next to the range
            for (const auto& y : s)
                if (y.algorithm == a && y.pos == pos && !y.hunt && y.verdict == ct::Verdict::Skip && y.reason == ct::Reason::NeedsLoad) {
                    tag(b, y);
                    break;
                }
        }
        return;   // one row per APDO: this row is that one chip
    }
}
void legacy_row(Builder& b, std::span<const ct::Step> s, A a) {
    for (size_t i = 0; i < s.size(); ++i) {
        const auto& x = s[i];
        if (x.algorithm != a || !x.mv) continue;
        bool seen = false;
        for (size_t j = 0; j < i; ++j) seen |= s[j].algorithm == a && s[j].mv == x.mv;
        if (seen) continue;
        bool detected = false, hs_ok = false, class_a = false;
        Tally t;   // the up and down steps of one level roll up through the shared tone table
        for (const auto& y : s) {
            if (y.algorithm != a || y.mv != x.mv) continue;
            class_a |= y.verdict == ct::Verdict::Skip && y.reason == ct::Reason::ClassAOnly;
            hs_ok |= y.verdict == ct::Verdict::Pass && y.detail == uint8_t(fc::Fail::NoVoltage);
            detected |= y.verdict == ct::Verdict::Pass && y.detail == uint8_t(fc::Fail::Detected);
            count(t, y);
        }
        const bool fail = t.fail != 0;
        Token* k = b.token(true, roll_up(t));
        if (!k) return;
        k->mv = x.mv;
        Out o{k->text, sizeof k->text};
        if (hs_ok && !fail) { for (const char* w = "handshake ok"; *w; ++w) o.put(*w); }
        else if (detected && !fail) { for (const char* w = "detected"; *w; ++w) o.put(*w); }   // answered SVOOC frames, no voltage proof
        else if (a == A::Ufcs) put_text(*k, step_label::ufcs_level(x).c_str());   // "9V 3A": the mode's top current; "9V" before the list was read
        else put_text(*k, step_label::params(x).c_str());   // "9V"; SCP also its limit: "10V 2.25A"
        if (class_a && !t.pass && !fail) {   // a Class A charger: the neutral chip says why the 20 V level shows no verdict
            if (Token* t = b.token(false, Tone::Dim, true)) std::strncpy(t->text, "Class A", sizeof t->text - 1);
        }
    }
}
// Reach of one PDO slot's range steps.
Reach reach_of(std::span<const ct::Step> s, A a, uint8_t pos) {
    Reach r;
    Tally t;
    bool needs_load = false;
    for (const auto& y : s) {
        if (y.algorithm != a || y.pos != pos || y.hunt || ct::is_sweep(y) || !y.pdo || !y.mv) continue;
        count(t, y);
        if (!r.adv_mv) r.adv_mv = uint16_t(pd::decode_pdo(y.pdo).max_mv);
        if (y.verdict == ct::Verdict::Skip && y.reason == ct::Reason::NeedsLoad) needs_load = true;
        if (y.verdict == ct::Verdict::Pass) {
            if (!r.lo_mv || y.mv < r.lo_mv) r.lo_mv = y.mv;
            r.hi_mv = std::max(r.hi_mv, y.mv);
        } else if (y.verdict == ct::Verdict::Fail && y.reason == ct::Reason::Capped) {
            r.capped = true;
            r.hi_mv = std::max(r.hi_mv, y.measured_mv);   // the ceiling the source held
        } else if (y.verdict == ct::Verdict::Fail && y.reason == ct::Reason::Floored) {
            r.floored = true;
            r.floor_mv = std::max(r.floor_mv, y.measured_mv);   // the floor the source held
        }
    }
    r.tone = roll_up(t);
    r.needs_load = needs_load && !t.fail;   // a plain failure in the range is the headline, never hidden behind "needs load"
    return r;
}
// Levels at 0.1 V: 19419 -> "19.4", 21000 -> "21".
void put_level(step_label::Text& t, unsigned mv) {
    const unsigned d = mv / 100;
    char b[12]; unsigned k = 0;
    auto num = [&](unsigned v) { char x[6]; unsigned n = 0; do { x[n++] = char('0' + v % 10); v /= 10; } while (v && n < 6); while (n) b[k++] = x[--n]; };
    num(d / 10);
    if (d % 10) { b[k++] = '.'; num(d % 10); }
    for (unsigned i = 0; i < k && t.len + 1 < step_label::kMax; ++i) t.s[t.len++] = b[i];
    t.s[t.len] = 0;
}
void put_word(step_label::Text& t, const char* w) {
    while (*w && t.len + 1 < step_label::kMax) t.s[t.len++] = *w++;
    t.s[t.len] = 0;
}
}  // namespace

Tone roll_up(const Tally& t) {
    if (t.pending) return Tone::Pending;
    if (!t.pass && !t.fail && !t.capped) return t.skip ? Tone::Skip : Tone::Pending;
    if (!t.fail && !t.capped && !t.skip) return Tone::Pass;
    return t.pass || t.capped ? Tone::Partial : Tone::Fail;
}

Reach reach(std::span<const ct::Step> s, A a, int pos) {
    if (pos >= 0) return reach_of(s, a, uint8_t(pos));
    Reach last;
    uint32_t seen = 0;   // PDO slots already looked at (slots are small: at most 15)
    for (const auto& x : s) {
        if (x.algorithm != a || x.hunt || ct::is_sweep(x) || !x.pdo || x.pos >= 32 || (seen >> x.pos & 1)) continue;
        seen |= 1u << x.pos;
        last = reach_of(s, a, x.pos);
        if (last.tone != Tone::Pass) return last;   // the first range that is not clean tells the story
    }
    return last;
}

step_label::Text reach_footer(const Reach& r) {
    step_label::Text t;
    if (r.floored) {
        put_word(t, "floor at "); put_level(t, r.floor_mv); put_word(t, "V");
    } else if (r.capped) {
        put_word(t, "caps "); put_level(t, r.hi_mv); put_word(t, "V of "); put_level(t, r.adv_mv); put_word(t, "V");
    } else if (r.tone == Tone::Pending) {
        put_word(t, "not finished");
    } else if (r.needs_load && r.adv_mv && (r.tone == Tone::Skip || r.tone == Tone::Partial)) {
        // pattern pps-off-at-no-load: the levels were skipped, not failed and not never-run; say why
        put_word(t, "needs load");
        if (r.lo_mv) { put_word(t, " >"); put_level(t, r.hi_mv); put_word(t, "V"); }   // the highest level that passed
    } else if (r.tone == Tone::Skip || !r.adv_mv) {
        put_word(t, "not run");
    } else if (!r.lo_mv) {
        put_word(t, "none reached");
    } else if (r.tone == Tone::Pass) {
        put_word(t, "reached "); put_level(t, r.lo_mv); put_word(t, "-"); put_level(t, r.hi_mv); put_word(t, "V");
    } else {
        put_word(t, "max "); put_level(t, r.hi_mv); put_word(t, "V of "); put_level(t, r.adv_mv); put_word(t, "V");
    }
    return t;
}

step_label::Text reach_span(const Reach& r) {
    step_label::Text t;
    if (!r.hi_mv) return t;
    if (r.lo_mv && r.lo_mv != r.hi_mv) { put_level(t, r.lo_mv); put_word(t, "-"); }
    put_level(t, r.hi_mv); put_word(t, "V");
    if (r.hi_mv < r.adv_mv) { put_word(t, " of "); put_level(t, r.adv_mv); put_word(t, "V"); }
    return t;
}

const char* section_name(Section s) {
    switch (s) {
    case Section::Huawei: return "Huawei";
    case Section::Samsung: return "Samsung";
    case Section::Oppo: return "OPPO";
    case Section::Xiaomi: return "Xiaomi";
    case Section::OemPd: return "OEM / PD";
    case Section::UsbPd: return "USB PD";
    case Section::Qc: return "QC/UFCS";
    default: return "?";
    }
}
Section section_of(A a) {
    switch (a) {
    case A::Fcp: case A::Scp: return Section::Huawei;
    case A::Afc: return Section::Samsung;
    case A::Vooc: case A::SuperVooc: case A::SuperVooc65: case A::Vivo: return Section::Oppo;
    default: return ct::group(a) == ct::Group::Pd ? Section::UsbPd : Section::Qc;   // QC2 / QC3 / QC3+ / Class B / UFCS
    }
}
// PD-derived OEM protocols are named from the source VID only (a VID may drive a label, never
// behaviour). 0x05C6 Qualcomm is covered by the QC4+/QC5 label itself; 0x17EF Lenovo (the C140/C135
// slim-tip 7 A mode is not PD-visible over C-to-C), 0x03F0 HP, 0x2717/0x2B01 Xiaomi-ecosystem.
// A separate list from report_card::brand(): that one names only usb.ids vendors for the card name, this one also maps the
// Xiaomi-ecosystem VID 0x2B01 (Zimi/CUKTECH), which usb.ids does not list.
const char* vendor_oem_name(uint16_t vid) {
    switch (vid) {
    case 0x17EF: return "Lenovo";
    case 0x03F0: return "HP";
    case 0x2717: case 0x2B01: return "Xiaomi";
    default: return nullptr;
    }
}

const char* display_name(A a) {
    switch (a) {
    case A::Pd2Fixed: return "PD2.0";
    case A::Pd3Fixed: return "PD3.x";
    case A::Epr: return "EPR";
    case A::SprAvs: return "SPR AVS";
    case A::Pps: return "PPS";
    case A::EprAvs: return "EPR AVS";
    case A::Qc2: return "QC2";
    case A::Qc3: return "QC3";
    case A::Qc3Plus: return "QC3+";
    case A::Qc2ClassB: return "QC2B";
    case A::Qc3ClassB: return "QC3B";
    case A::Afc: return "AFC";
    case A::Fcp: return "FCP";
    case A::Scp: return "SCP";
    case A::Vooc: return "VOOC";
    case A::SuperVooc: return "SVOOC";
    case A::SuperVooc65: return "SVOOC65";
    case A::Ufcs: return "UFCS";
    case A::Vivo: return "VIVO";
    default: return "?";
    }
}

// The deep sweep's row: ONE range chip, in the style of the range row above it ("5-21V 3.2A": the swept range, the APDO's
// current under it in the grid). The range is the sweep's own: the record's row view when the row ran (or runs), else the
// APDO range capped at the monitor ceiling. The chip has no level (Token::mv 0), so the Running dot sits on it while the
// sweep is in flight and the live band above shows where the sweep is; the per-point errors are the footer's and the log's.
// Tone: Pending until the sweep closes (Running included: the dot needs it), Skip when skipped, Fail when the source was
// lost, else by the worst recorded |error| (<= 100 mV Pass, <= 300 mV Partial, above Fail).
void sweep_row(Builder& b, std::span<const ct::Step> s, const ctsweep::View& v, A a, uint8_t pos, unsigned cap_mv) {
    const ct::Step* step = nullptr;
    for (const auto& x : s) if (ct::is_sweep(x) && x.algorithm == a && x.pos == pos) { step = &x; break; }
    if (!step) return;
    const auto p = pd::decode_pdo(step->pdo);
    const auto pr = ctsweep::point_range(p.min_mv, p.max_mv, cap_mv);
    unsigned lo = pr.lo, hi = pr.hi;
    const ctsweep::RowView* rw = v.find(uint8_t(a), pos);
    if (rw) { lo = rw->lo_mv; hi = rw->hi_mv; }
    if (hi < lo) { lo = p.min_mv; hi = p.max_mv; }   // nothing fits under the ceiling: the row still names the advertised range
    Tone tn = Tone::Pending;
    switch (step->verdict) {
    case ct::Verdict::Skip: tn = Tone::Skip; break;
    case ct::Verdict::Fail: tn = Tone::Fail; break;
    case ct::Verdict::Pass: {
        int worst = 0;
        if (rw)
            for (unsigned i = 0; i < rw->n; ++i) worst = std::max(worst, std::abs(int(v.points[rw->first + i])));
        tn = worst <= 100 ? Tone::Pass : worst <= 300 ? Tone::Partial : Tone::Fail;
        break;
    }
    default: break;
    }
    Token* k = b.token(true, tn);
    if (!k) return;
    put_text(*k, step_label::range(lo, hi, p.max_ma, p.max_mw).c_str());
    if (tn == Tone::Skip) tag(b, *step);   // why nothing was swept ("cable", ">36V"), as the range row says it
}

void build(Layout& l, std::span<const ct::Step> steps, const OemNotes& oem, const ctsweep::View& sweep) {
    // Reset in place: `l = {}` builds a whole Layout on the stack first (about 2 KB, over the frame limit).
    for (Token& k : l.tokens) k = {};
    for (Row& r : l.rows) r = {};
    l.token_count = l.row_count = 0;
    l.cuts = 0;
    Builder b{l};
    // The rows come from the plan itself: the steps ARE the execution order (the runner walks them by index), so each row is a
    // group of steps, placed where the plan first runs one of them. A group is a protocol's steps (Revision rows are no grid
    // rows; a range algorithm groups per APDO slot, so a probe the planner runs between two APDOs lands between their
    // rows), or the 6 A probes of one level (every hunt step at that voltage): a probe is a row of its own, wherever the
    // planner put it (between the PPS rows, or after the fixed ones), and it moves with the plan when a re-plan regrows the rows.
    // Key::mv carries the probed level of a probe key, the PDO slot of a range or sweep key, else 0; sweep marks a
    // deep-sweep key (one strip row per APDO, next to the APDO's range row).
    struct Key { A a; bool probe; bool sweep; uint16_t mv; };
    Key keys[unsigned(A::Count) + 2 * pd::kMaxPdos + 4];   // worst case: every algorithm, both lists' PDO slots, the hunt levels
    size_t key_n = 0;
    bool absent[unsigned(A::Count)] = {}, skipped[unsigned(A::Count)] = {}, range_none[unsigned(A::Count)] = {};
    bool any_probe = false;
    for (const auto& s : steps) {
        if (is_revision(s.algorithm)) continue;
        const Key k{s.hunt ? A::Count : s.algorithm, s.hunt, ct::is_sweep(s),
                    s.hunt ? s.mv : uint16_t(is_range(s.algorithm) ? s.pos : 0)};
        bool seen = false;
        for (size_t i = 0; i < key_n && !seen; ++i)
            seen = keys[i].a == k.a && keys[i].probe == k.probe && keys[i].sweep == k.sweep && keys[i].mv == k.mv;
        if (seen || key_n == std::size(keys)) continue;
        keys[key_n++] = k;
        any_probe |= k.probe;
    }
    auto section_of_key = [](const Key& k) { return k.probe ? Section::Xiaomi : section_of(k.a); };
    auto header = [&](Section s) {
        if (!oem.headers || l.row_count >= l.rows.size()) { b.row = nullptr; return; }
        Row* r = &l.rows[l.row_count++];
        *r = {};
        std::strncpy(r->label, section_name(s), sizeof r->label - 1);
        r->label_tone = Tone::Caution;
        r->algorithm = A::Count;
        r->header = true;
        b.row = nullptr;
    };
    // A run = consecutive rows of the plan that share a section: the default order has one run per section.
    using Run = std::span<const Key>;
    // The 6 A probes of one level (Mi Turbo 20 V 6 A / 11 V 6 A and any other OEM unlocked mode): one chip per probe step, always
    // labelled with the probed current (6 A), green only when the source accepted it.
    auto probe_row = [&](uint16_t mv) {
        b.begin("Mi 6A", Tone::Dim, A::Count);   // Row.label holds 8 chars
        if (b.row) { b.row->probe = true; b.row->mv = mv; }
        for (const auto& x : steps) {
            if (!x.hunt || x.mv != mv) continue;
            Token* k = b.token(true, tone(x));
            if (!k) break;
            put_text(*k, step_label::params(x).c_str());   // the probed current: "20V 6A", not the advertised max
            tag(b, x);
        }
    };
    bool mi_shown = false;
    auto mi_auth_row = [&] {   // the SVID 0x2717 auth fact, once, in front of the first probe (or alone when no probe is in the plan)
        if (!oem.mi_auth || mi_shown) return;
        mi_shown = true;
        b.begin("Mi Turbo", Tone::Pass, A::Count);
        b.word("auth ok", Tone::Dim);
    };
    auto rows_for = [&](Run run) {
        for (const Key& key : run) {
            if (key.probe) { probe_row(key.mv); continue; }
            if (key.sweep) {   // the deep sweep's strip row, right after its APDO's range row (the plan emits it there)
                b.begin("Sweep", Tone::Dim, key.a);
                if (b.row) { b.row->sweep = true; b.row->pos = uint8_t(key.mv); }
                sweep_row(b, steps, sweep, key.a, uint8_t(key.mv), oem.monitor_mv);
                continue;
            }
            const A a = key.a;
            const bool legacy = ct::group(a) != ct::Group::Pd;
            if (is_absent(steps, a)) {
                // A range the charger does not offer keeps its own row: "PPS None" (once per algorithm, not per APDO slot).
                // A legacy unit keeps its chips when not folding.
                if (is_range(a)) {
                    if (range_none[unsigned(a)]) continue;
                    range_none[unsigned(a)] = true;
                    b.begin(display_name(a), Tone::Dim, a); b.word("None", Tone::Dim); continue;
                }
                if (oem.fold || !legacy) { absent[unsigned(a)] = true; continue; }
            }
            if (is_no_qc3plus(steps, a)) { b.begin(display_name(a), Tone::Dim, a); b.word("no QC3+", Tone::Dim); continue; }
            if (oem.fold && legacy && is_skipped_only(steps, a) && !is_class_a_only(steps, a)) { skipped[unsigned(a)] = true; continue; }
            b.begin(display_name(a), Tone::Dim, a);
            if (is_range(a)) {
                if (b.row) b.row->pos = uint8_t(key.mv);   // one row per APDO: the row carries its PDO slot
                range_row(b, steps, a, uint8_t(key.mv));
            }
            else if (ct::group(a) == ct::Group::Pd) fixed_row(b, steps, a);
            else legacy_row(b, steps, a);
            // Skipped before any voltage or PDO was known (an EPR step held back by the cable), or a range key whose slot
            // has no step of its own: no chip to show, so no row of its own; a bare label under the neighbouring row reads
            // as a blank line.
            if (b.row && b.row->count == 0 && (is_skipped_only(steps, a) || is_range(a))) {
                --l.row_count;
                if (is_skipped_only(steps, a)) skipped[unsigned(a)] = true;
            }
        }
    };
    // A section whose protocols all fold into "No reply" / "Skipped" shows no rows of its own, so its header is dropped
    // again (a bare header above the next header reads as an empty family).
    auto section = [&](Run run) {
        const Section sec = section_of_key(run[0]);
        const size_t before = l.row_count;
        header(sec);
        const size_t after_header = l.row_count;
        if (sec == Section::Xiaomi) mi_auth_row();
        rows_for(run);
        if (after_header > before && l.row_count == after_header) l.row_count = before;
    };
    // The sections follow the plan (the default: OEM vendor modes by company, USB PD, QC / UFCS; the Xiaomi probes where the
    // planner runs them, inside or after the PD rows). A custom unit order moves the rows and their sections with it; a family
    // the plan splits shows as two runs, so the rows stay in the order they are tested in.
    Run runs[unsigned(A::Count) + 4];
    size_t run_n = 0;
    for (size_t at = 0; at < key_n;) {
        size_t end = at + 1;
        while (end < key_n && section_of_key(keys[end]) == section_of_key(keys[at])) ++end;
        runs[run_n++] = Run{keys + at, end - at};
        at = end;
    }
    // Facts of a PD source's identity (the passive QC4+/QC5 label, the VID-driven OEM name, the Mi Turbo auth state when the plan
    // holds no probe to carry it): in front of the first PD or Xiaomi run, or after the last run. Omitted when nothing was detected.
    bool facts_done = false;
    auto facts = [&] {
        if (facts_done) return;
        facts_done = true;
        if (oem.mi_auth && !any_probe) { header(Section::Xiaomi); mi_auth_row(); }
        if (!(oem.qc_label || oem.vendor || oem.oppo_svid || oem.scp_cap || oem.svooc_cap || oem.rebalanced)) return;
        header(Section::OemPd);
        if (oem.rebalanced) {   // the source re-split its budget mid-run and the PD rows were re-planned per row
            b.begin("SOURCE", Tone::Caution, A::Count);
            b.word("rebalanced", Tone::Caution);
            char nx[5] = {};   // "<n>x"; no stdio on the device (snprintf would drag newlib's stdio into the link)
            char rev[3] = {};
            unsigned v = oem.rebalanced, n = 0;
            do { rev[n++] = char('0' + v % 10); v /= 10; } while (v && n < sizeof rev);
            for (unsigned i = 0; i < n; ++i) nx[i] = rev[n - 1 - i];
            nx[n] = 'x';
            b.word(nx, Tone::Caution);
        }
        if (oem.qc_label) {
            // The label is the full name ("Samsung SFC 2.0", "Moto TurboPower", "QC5"): its first word fits the 8-character
            // row label, the rest and the justification ("via PPS 5-20V 5A") are tokens, which the grid wraps onto more lines.
            const std::string_view name = oem.qc_label;
            const size_t sp = std::min(name.find(' '), name.size());
            b.begin(name.substr(0, sp), Tone::Pass, A::Count);
            b.phrase(name.substr(sp), Tone::Dim, false);
            if (oem.qc_why) b.phrase(oem.qc_why, Tone::Dim, true);
        }
        if (oem.vendor) { b.begin("OEM", Tone::Dim, A::Count); b.phrase(oem.vendor, Tone::Dim, false); }
        if (oem.oppo_svid) {   // the same words as report_card::oppo_note(); discovery only, no VDM is ever sent to it
            b.begin("OPPO", Tone::Dim, A::Count);
            b.phrase("SVID 0x22D9 (discovery only)", Tone::Dim, false);
        }
        if (oem.scp_cap) {   // what the adapter reported to the SCP session: "20V 6A"
            b.begin("SCP cap", Tone::Dim, A::Count);
            b.phrase(oem.scp_cap, Tone::Dim, false);
        }
        if (oem.svooc_cap) {   // the ceiling stage's result: what the charger accepted as an ask, no current drawn
            b.begin("SV cap", Tone::Dim, A::Count);
            b.phrase(oem.svooc_cap, Tone::Dim, false);
        }
    };
    // USB PD (compliant USB-C, PD over CC) and QC / UFCS (standard D+/D- protocols; QC runs on USB-C as well, so not "USB-A"),
    // each where the plan puts it.
    for (size_t r = 0; r < run_n; ++r) {
        const Section sec = section_of_key(runs[r][0]);
        if (sec == Section::UsbPd || sec == Section::Xiaomi) facts();
        section(runs[r]);
    }
    facts();
    auto collapsed = [&](const bool (&set)[unsigned(A::Count)], const char* label, Tone t) {
        bool any = false;
        for (size_t i = 0; i < key_n; ++i) any |= !keys[i].probe && set[unsigned(keys[i].a)];
        if (!any) return;
        b.begin(label, t, A::Count);
        bool said[unsigned(A::Count)] = {};   // one word per protocol: a range has a key per APDO slot, the fold names it once
        for (size_t i = 0; i < key_n; ++i)
            if (!keys[i].probe && set[unsigned(keys[i].a)] && !said[unsigned(keys[i].a)]) {
                said[unsigned(keys[i].a)] = true;
                b.word(display_name(keys[i].a), t);
            }
    };
    collapsed(absent, "No reply", Tone::Dim);
    collapsed(skipped, "Skipped", Tone::Caution);
}

bool row_has(const Row& r, const ct::Step& s) {
    if (r.header) return false;
    if (r.probe) return s.hunt && s.mv == r.mv;
    if (r.sweep) return ct::is_sweep(s) && s.pos == r.pos;   // the strip row owns its APDO's sweep step
    if (s.hunt || ct::is_sweep(s) || r.algorithm != s.algorithm) return false;
    return !is_range(r.algorithm) || s.pos == r.pos;   // one row per APDO: a range row owns only its slot's steps
}

void add_note(Layout& l, const char* label, Tone t, std::initializer_list<const char*> words) {
    Builder b{l};
    b.begin(label, t, A::Count);
    if (!b.row) return;
    for (const char* w : words) b.word(w, Tone::Dim);
}

namespace {
enum class Fam : uint8_t { Pd, Mi, Qc, Afc, Huawei, Oppo, Ufcs, Count };
Fam family_of(const ct::Step& x) {
    if (x.hunt) return Fam::Mi;
    switch (x.algorithm) {
    case A::Qc2: case A::Qc3: case A::Qc3Plus: case A::Qc2ClassB: case A::Qc3ClassB: return Fam::Qc;
    case A::Afc: return Fam::Afc;
    case A::Fcp: case A::Scp: return Fam::Huawei;
    case A::Vooc: case A::SuperVooc: case A::SuperVooc65: return Fam::Oppo;
    case A::Ufcs: case A::Vivo: return Fam::Ufcs;
    default: return ct::group(x.algorithm) == ct::Group::Pd ? Fam::Pd : Fam::Count;
    }
}
// The current a step is ranked by: what its label shows, or for an AVS offer (no amps in the words) the offer's limit at the
// step's voltage, so a 28 V AVS offer is not outranked by a 20 V fixed one.
unsigned rank_ma(const ct::Step& x) {
    if (const unsigned ma = step_label::amps_ma(x)) return ma;
    return x.avs && x.pdo ? pd::current_limit_ma(pd::decode_pdo(x.pdo), x.mv) : 0u;
}
struct Best { bool have = false; const ct::Step* step = nullptr; unsigned mw = 0, mv = 0, ma = 0; bool hs = false; };
}  // namespace

size_t summarize(std::span<const ct::Step> steps, std::span<SummaryLine> out) {
    Best best[unsigned(Fam::Count)];
    for (const auto& x : steps) {
        const Fam f = family_of(x);
        if (f == Fam::Count || x.verdict != ct::Verdict::Pass || is_revision(x.algorithm) || ct::is_sweep(x)) continue;
        if (x.algorithm == A::Qc3Plus && is_no_qc3plus(steps, A::Qc3Plus)) continue;   // the 5 V baseline of a charger without QC3+
        const bool pd = f == Fam::Pd;
        const unsigned ma = pd || x.hunt ? rank_ma(x) : 0;
        // A D+/D- handshake that proved no voltage (VOOC 5 V handshake, SVOOC frames seen) has no voltage to rank by.
        const bool unproven = !pd && !x.hunt && (x.detail == uint8_t(fc::Fail::NoVoltage) || x.detail == uint8_t(fc::Fail::Detected));
        const unsigned mv = unproven ? 0 : x.mv;
        if (!unproven && !mv) continue;
        const unsigned mw = math::mw_of(mv, ma);
        Best& b = best[unsigned(f)];
        // On a tie the later (more capable) protocol of the family wins.
        if (!b.have || mw > b.mw || (mw == b.mw && mv > b.mv) || (mw == b.mw && mv == b.mv && x.algorithm >= b.step->algorithm))
            b = {true, &x, mw, mv, ma, unproven && x.detail == uint8_t(fc::Fail::NoVoltage)};
    }
    Fam order[unsigned(Fam::Count)];
    size_t n = 0;
    for (unsigned f = 0; f < unsigned(Fam::Count); ++f) if (best[f].have) order[n++] = Fam(f);
    // Insertion sort, stable and allocation-free (std::stable_sort would pull in the heap): watts, then volts, descending.
    const auto before = [&](Fam a, Fam b) {
        const Best& x = best[unsigned(a)];
        const Best& y = best[unsigned(b)];
        return x.mw != y.mw ? x.mw > y.mw : x.mv > y.mv;
    };
    for (size_t i = 1; i < n; ++i)
        for (size_t j = i; j > 0 && before(order[j], order[j - 1]); --j) std::swap(order[j], order[j - 1]);
    size_t written = 0;
    for (size_t i = 0; i < n && written < out.size(); ++i) {
        const Best& b = best[unsigned(order[i])];
        SummaryLine& line = out[written++];
        line = {};
        Out o{line.text, sizeof line.text};
        o.put(step_label::protocol(*b.step).c_str());
        o.put(' ');
        if (b.mv) o.put(step_label::params(*b.step).c_str());   // "20V 3.35A", "12V", "SCP 10V 2.25A"
        else o.put(b.hs ? "ok" : "seen");
        line.mw = b.mw; line.mv = b.mv;
    }
    return written;
}

int token_width(const Token& t, const Metrics& m) {
    return int(std::strlen(t.text)) * m.adv + (t.chip ? 2 * m.pad + m.mark : 0);
}
int cell_width(const Token& t, const Metrics& m) {
    int n = 0;
    while (t.text[n] && (!t.chip || t.text[n] != ' ')) ++n;   // a chip's volt text; a word is whole
    return n * m.adv + 2 * m.pad;
}
char mark(Tone t) {
    switch (t) {
    case Tone::Pass: return '\x01';
    case Tone::Fail: return '\x02';
    case Tone::Partial: return '~';
    case Tone::Skip: return '-';
    case Tone::Pending: return '.';
    default: return ' ';
    }
}

size_t wrap(const Layout& l, const Metrics& m, std::span<Line> out) {
    size_t n = 0;
    auto emit = [&](const Line& line) { if (n < out.size()) out[n++] = line; };
    for (size_t r = 0; r < l.row_count; ++r) {
        const Row& row = l.rows[r];
        if (row.header) { emit({static_cast<uint8_t>(r), row.first, 0, true}); continue; }
        const unsigned stop = unsigned(row.first) + row.count;
        Line cur{static_cast<uint8_t>(r), row.first, 0, true};
        int x = 0, groups = 0, widest = 0;
        for (unsigned i = row.first; i < stop;) {
            unsigned end = i + 1;
            int gw = token_width(l.tokens[i], m);
            while (end < stop && l.tokens[end].glue) { gw += m.gap + token_width(l.tokens[end], m); ++end; }
            if (m.cells) gw = cell_width(l.tokens[i], m);   // the verdict grid: equal cells, only the head's volt text counts
            // A line breaks when the chips no longer fit: side by side (x), or in `groups` equal cells of the widest one's width.
            const bool full = m.cells ? (groups + 1) * (std::max(widest, gw) + m.gap) - m.gap > m.avail
                                      : x + m.gap + gw > m.avail;
            if (cur.count && (full || (m.max_groups && groups >= m.max_groups))) {
                emit(cur);
                cur = {static_cast<uint8_t>(r), static_cast<uint8_t>(i), 0, false};
                x = 0; groups = 0; widest = 0;
            }
            ++groups;
            widest = std::max(widest, gw);
            x += (cur.count ? m.gap : 0) + gw;
            cur.count = static_cast<uint8_t>(cur.count + (end - i));
            i = end;
        }
        emit(cur);
    }
    return n;
}
}  // namespace k2::result_layout
