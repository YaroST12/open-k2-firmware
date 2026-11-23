// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/chargertest/result_layout.hpp"
#include "k2/core/math_opt.hpp"
#include "k2/pd/pd.hpp"
#include "k2/fastcharge/fc_engine.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/oem_watts.hpp"
#include "k2/chargertest/mi_unlock.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace k2::result_layout {
namespace ct = charger_test;

bool noted(const ct::Step& s) {
    return s.verdict == ct::Verdict::Skip && (s.reason == ct::Reason::Advertised || s.reason == ct::Reason::LightLevel);
}
// A row skipped because its unit never answered (VOOC no opener after either wake, a protocol that sent no frame): "No reply".
bool is_no_answer(const ct::Step& x) {
    return x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::Request && ct::no_reply(x.group, fc::Fail(x.detail));
}
bool advertised(const ct::Step& s) { return s.verdict == ct::Verdict::Skip && s.reason == ct::Reason::Advertised && s.group == ct::Group::Pd; }
bool caution(const ct::Step& s) {
    return s.verdict == ct::Verdict::Pass &&
           (s.reason == ct::Reason::Folded || s.reason == ct::Reason::StepResolution || s.reason == ct::Reason::ProgCheck);
}
const char* verdict_word(const ct::Step& s) {
    switch (s.verdict) {
    case ct::Verdict::Pass: return caution(s) ? "CAUTION" : "PASS";
    case ct::Verdict::Fail: return s.reason == ct::Reason::Capped ? "CAPPED" : "FAIL";
    case ct::Verdict::Skip: return s.reason == ct::Reason::Advertised ? "LISTED" : s.reason == ct::Reason::LightLevel ? "NOT RUN" : "SKIP";
    case ct::Verdict::Running: return "RUNNING";
    default: return "PENDING";
    }
}
unsigned sweep_worst_mv(const ctsweep::View& v, uint8_t alg, uint8_t pos) {
    unsigned w = 0;
    for (const bool low : {false, true})
        if (const auto* r = v.find(alg, pos, low)) w = std::max(w, unsigned(std::abs(int(r->sum.worst_err))));
    return w;
}
bool deviates(const ct::Step& s, const ctsweep::View& v) {
    return ct::is_sweep(s) && s.verdict == ct::Verdict::Pass && s.reason != ct::Reason::Folded &&
           sweep_worst_mv(v, uint8_t(s.algorithm), s.pos) > kSweepPassMv;
}
bool caution(const ct::Step& s, const ctsweep::View& v) { return caution(s) || deviates(s, v); }
const ct::Step* step_caution(std::span<const ct::Step> steps, ct::Algorithm a, uint8_t pos) {
    const ct::Step* prog = nullptr;
    for (const auto& y : steps) {
        if (y.algorithm != a || y.pos != pos || y.hunt || y.verdict != ct::Verdict::Pass) continue;
        if (y.reason == ct::Reason::StepResolution) return &y;   // the walk's words name the whole range; a transition flag only one level
        if (y.reason == ct::Reason::ProgCheck && !prog) prog = &y;
    }
    return prog;
}
const char* verdict_word(const ct::Step& s, const ctsweep::View& v) { return deviates(s, v) ? "CAUTION" : verdict_word(s); }
Progress run_progress(std::span<const ct::Step> steps) {
    Progress p;
    for (const auto& s : steps) {
        if (s.verdict == ct::Verdict::Skip) continue;
        ++p.total;
        p.done += s.verdict == ct::Verdict::Pass || s.verdict == ct::Verdict::Fail;
    }
    return p;
}
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
    case ct::Verdict::Pass: caution(y) ? ++t.capped : ++t.pass; break;   // a fold, a step-walk miss or a transition flag is a caution: neither a pass nor a fail
    case ct::Verdict::Fail: y.reason == ct::Reason::Capped ? ++t.capped : ++t.fail; break;
    case ct::Verdict::Skip: noted(y) ? ++t.noted : ++t.skip; t.adv += advertised(y); break;
    default: ++t.pending; break;
    }
}
Tone tone(const ct::Step& x) {   // a chip of one step (a fixed level, a Xiaomi mode)
    Tally t;
    count(t, x);
    return roll_up(t);
}
// Shared token emitter for Layout (into Layout::tokens), CompactGrid (into a single-row RowTokens before line wrapping),
// and on-demand row regeneration (row_tokens).
struct TokenBuf {
    Token* data = nullptr;
    uint8_t cap = 0;
    uint8_t* count = nullptr;
    uint8_t* cuts = nullptr;

    void put_str(char* dst, size_t dcap, std::string_view s) {
        const size_t n = std::min(s.size(), dcap - 1);
        std::memcpy(dst, s.data(), n);
        dst[n] = 0;
        if (n < s.size() && cuts && *cuts < 255) ++(*cuts);
    }
    [[gnu::noinline]] Token* token(bool chip, Tone t, bool glue = false) {
        if (!data || !count || *count >= cap) return nullptr;
        Token& k = data[(*count)++];
        k = {}; k.chip = chip; k.tone = t; k.glue = glue;
        return &k;
    }
    void word(std::string_view s, Tone t) { if (Token* k = token(false, t)) put_str(k->text, sizeof k->text, s); }
    // `s` as plain tokens: whole words packed into tokens of at most 15 characters, so a long label wraps onto further grid
    // lines instead of being cut. `split_at_number`: the first word that starts with a digit opens a new token ("via PPS" |
    // "5-20V 5A"), keeping a range and its current together.
    [[gnu::noinline]] void phrase(std::string_view s, Tone t, bool split_at_number) {
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
                put_str(cur->text, sizeof cur->text, w);
                len = std::min(w.size(), kMax);
            }
        }
    }
};

[[gnu::noinline]] void wrap_row_tokens(std::span<const Token> toks, const Metrics& m,
                                      void (*emit)(void*, uint8_t first, uint8_t count, bool head), void* ctx) {
    if (toks.empty()) { emit(ctx, 0, 0, true); return; }
    uint8_t first = 0, count = 0;
    bool head = true, after_range = false;
    int x = 0, groups = 0, widest = 0;
    for (size_t i = 0; i < toks.size();) {
        size_t end = i + 1;
        int gw = token_width(toks[i], m);
        while (end < toks.size() && toks[end].glue) { gw += m.gap + token_width(toks[end], m); ++end; }
        if (m.cells) gw = cell_width(toks[i], m);   // the verdict grid: equal cells, only the head's volt text counts
        // A line breaks when the chips no longer fit: side by side (x), or in `groups` equal cells of the widest one's width.
        const bool full = m.cells ? (groups + 1) * (std::max(widest, gw) + m.gap) - m.gap > m.avail
                                  : x + m.gap + gw > m.avail;
        // A PD range chip has a line to itself: its wide cell must not widen the equal cells of the volt chips, and a line is one APDO.
        const bool range = toks[i].kind != 0;
        if (count && (full || range || after_range || (m.max_groups && groups >= m.max_groups))) {
            emit(ctx, first, count, head);
            first = static_cast<uint8_t>(i);
            count = 0;
            head = false;
            x = 0; groups = 0; widest = 0;
        }
        ++groups;
        after_range = range;
        widest = std::max(widest, gw);
        x += (count ? m.gap : 0) + gw;
        count = static_cast<uint8_t>(count + (end - i));
        i = end;
    }
    emit(ctx, first, count, head);
}

struct Sink {
    Layout* l = nullptr;
    CompactGrid* g = nullptr;
    const Metrics* m = nullptr;
    std::span<CompactLine> lines{};
    size_t* line_n = nullptr;
    RowTokens* rt = nullptr;
    Row* lrow = nullptr;
    CompactRow crow{};
    bool active = false;
    uint8_t ltok_cap = 0;

    TokenBuf buf() {
        if (l) {
            const uint8_t rem = lrow && l->token_count < l->tokens.size()
                                    ? static_cast<uint8_t>(l->tokens.size() - l->token_count) : 0;
            return {lrow ? l->tokens.data() + l->token_count : nullptr, rem, lrow ? &lrow->count : nullptr, &l->cuts};
        }
        return {active ? rt->toks.data() : nullptr, kMaxRowTokens, active ? &rt->count : nullptr, &g->cuts};
    }

    size_t row_count() const { return l ? l->row_count : g->row_count; }
    size_t line_count() const { return line_n ? *line_n : 0; }
    uint8_t open_tok_count() const { return l ? (lrow ? lrow->count : 0) : (active ? rt->count : 0); }

    [[gnu::noinline]] void flush() {
        if (!active) return;
        active = false;
        if (l) {
            if (lrow) l->token_count += lrow->count;
            lrow = nullptr;
            return;
        }
        if (g->row_count >= g->rows.size()) return;
        const uint8_t r_idx = g->row_count++;
        if (crow.kind == CompactKind::Fact) {
            const size_t llen = std::strlen(rt->label) + 1;
            if (g->pool_used + llen <= g->pool.size()) {
                crow.mv = g->pool_used;
                std::memcpy(g->pool.data() + g->pool_used, rt->label, llen);
                g->pool_used = static_cast<uint8_t>(g->pool_used + llen);
            } else {
                crow.mv = 0;
                if (g->cuts < 255) ++g->cuts;
            }
            crow.first = g->fact_count;
            crow.count = 0;
            for (uint8_t i = 0; i < rt->count; ++i) {
                const Token& k = rt->toks[i];
                const size_t len = std::min<size_t>(std::strlen(k.text), 31);
                if (g->fact_count >= g->fact_toks.size() || g->pool_used + len > g->pool.size()) {
                    if (g->cuts < 255) ++g->cuts;
                    break;
                }
                g->fact_toks[g->fact_count++] = {g->pool_used, static_cast<uint8_t>(len | (unsigned(k.tone) << 5))};
                std::memcpy(g->pool.data() + g->pool_used, k.text, len);
                g->pool_used = static_cast<uint8_t>(g->pool_used + len);
                ++crow.count;
            }
        }
        g->rows[r_idx] = crow;
        struct Ctx { Sink* s; uint8_t r; };
        Ctx c{this, r_idx};
        wrap_row_tokens(std::span<const Token>(rt->toks.data(), rt->count), *m,
                        [](void* p, uint8_t first, uint8_t count, bool head) {
                            auto* cx = static_cast<Ctx*>(p);
                            if (*cx->s->line_n < cx->s->lines.size())
                                cx->s->lines[(*cx->s->line_n)++] = CompactLine::make(cx->r, first, count, head);
                        },
                        &c);
    }

    [[gnu::noinline]] void begin(CompactKind kind, std::string_view label, Tone label_tone, A a, uint16_t mv = 0) {
        flush();
        if (l) {
            if (l->row_count >= l->rows.size()) { lrow = nullptr; return; }
            lrow = &l->rows[l->row_count++];
            *lrow = {};
            TokenBuf tb{nullptr, 0, nullptr, &l->cuts};
            tb.put_str(lrow->label, sizeof lrow->label, label);
            lrow->label_tone = label_tone;
            lrow->algorithm = a;
            lrow->first = static_cast<uint8_t>(l->token_count);
            lrow->probe = kind == CompactKind::Probe;
            lrow->header = kind == CompactKind::Header;
            lrow->levels = kind == CompactKind::Levels;
            lrow->pd = kind == CompactKind::Pd;
            if (kind == CompactKind::Probe) lrow->mv = mv;
            active = true;
            return;
        }
        if (g->row_count >= g->rows.size()) return;
        rt->count = 0;
        rt->label_tone = label_tone;
        TokenBuf tb{nullptr, 0, nullptr, &g->cuts};
        tb.put_str(rt->label, sizeof rt->label, label);
        crow = {};
        crow.kind = kind;
        crow.algo = kind == CompactKind::Header ? static_cast<uint8_t>(mv) :
                    (kind == CompactKind::Fact || kind == CompactKind::Collapsed) ? static_cast<uint8_t>(label_tone) :
                    static_cast<uint8_t>(a);
        crow.mv = mv;
        active = true;
    }

    void levels(uint8_t first, uint8_t count) {
        if (l) { if (lrow) lrow->pos = first; }
        else { crow.first = first; crow.count = count; }
    }

    void drop_open() {
        if (!active) return;
        active = false;
        if (l) {
            if (lrow) --l->row_count;
            lrow = nullptr;
        } else {
            rt->count = 0;
        }
    }

    void rewind(size_t rc, size_t lc) {
        flush();
        if (l) {
            l->row_count = rc;
            l->token_count = rc ? l->rows[rc - 1].first + l->rows[rc - 1].count : 0;
        } else {
            g->row_count = static_cast<uint8_t>(rc);
            if (line_n) *line_n = lc;
        }
    }

    [[gnu::noinline]] void word(std::string_view s, Tone t) { TokenBuf tb = buf(); tb.word(s, t); }
    [[gnu::noinline]] void phrase(std::string_view s, Tone t, bool split_at_number) { TokenBuf tb = buf(); tb.phrase(s, t, split_at_number); }
};
bool is_revision(A a) { return a >= A::Pd30Revision && a <= A::Pd32Revision; }
// Nothing to show for this protocol: every step is "not advertised", or a legacy protocol never answered at 5 V.
bool is_absent(std::span<const ct::Step> s, A a) {
    bool pass = false, all_none = true;
    for (const auto& x : s) {
        if (x.algorithm != a) continue;
        pass |= x.verdict == ct::Verdict::Pass;
        all_none &= (x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::NoOffer) || is_no_answer(x);
    }
    if (pass) return false;
    if (all_none) return true;
    for (const auto& x : s) if (x.algorithm == a) return x.verdict == ct::Verdict::Fail;   // the first (5 V) step failed
    return false;
}
// The source offered no PD at all: every PD step is "not advertised" (a block with no chip that is not this was skipped).
bool pd_absent(std::span<const ct::Step> s) {
    for (const auto& x : s)
        if (!x.hunt && !is_revision(x.algorithm) && ct::group(x.algorithm) == ct::Group::Pd &&
            !(x.verdict == ct::Verdict::Skip && x.reason == ct::Reason::NoOffer))
            return false;
    return true;
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
    case ct::Reason::NeedsLoad: return "needs load";   // pattern pps-off-at-no-load: a skip, never a fail (10 chars: fits a token)
    default: return nullptr;
    }
}
void tag(TokenBuf& b, const ct::Step& x) {
    if (const char* t = skip_tag(x))
        if (Token* k = b.token(false, Tone::Caution, true)) std::strncpy(k->text, t, sizeof k->text - 1);
}
// The plan runs the fixed levels from the top voltage down, the grid reads them left to right ascending: chips go out in
// (voltage, plan index) order, one selection pass per chip (at most kMaxSteps steps, no scratch array). `algs` = the algorithms
// whose levels share the run (ct::kPdFixedAlgorithms). `fold`: an EPR level that was skipped (cable, refused, over the monitor
// cap) has no chip; a listed one (a Light run) keeps its pass look. The reasons stay on the block's Steps page.
[[gnu::noinline]] void fixed_row(TokenBuf& b, std::span<const ct::Step> s, uint32_t algs, bool fold) {
    uint32_t done = 0;   // the sort key of the chip emitted last; keys are unique and start above 0
    for (;;) {
        const ct::Step* next = nullptr;
        uint32_t best = UINT32_MAX;
        for (size_t i = 0; i < s.size(); ++i) {
            const auto& x = s[i];
            if (!(algs >> unsigned(x.algorithm) & 1) || !x.mv || x.hunt) continue;   // a Xiaomi row step is a row of its own (probe_row)
            if (fold && x.algorithm == A::Epr && x.verdict == ct::Verdict::Skip && !noted(x)) continue;
            const uint32_t key = (uint32_t(x.mv) << 10) | uint32_t(i);
            if (key > done && key < best) { best = key; next = &x; }
        }
        if (!next) return;
        done = best;
        Token* k = b.token(true, tone(*next));
        if (!k) return;
        k->mv = next->mv;
        put_text(*k, step_label::params(*next).c_str());   // "9V 3A", "20V 3.35A", "28V 5A"
        tag(b, *next);
    }
}
// The worse of two chip tones, for the merge of an APDO's range steps with its deep sweep: pending while anything is pending,
// then fail, then partial (a caution is one), then pass; a listing (Noted) and a skip rank last, so a chip is Skip only when
// every part skipped.
unsigned tone_rank(Tone t) {
    switch (t) {
    case Tone::Pending: return 5;
    case Tone::Fail: return 4;
    case Tone::Partial: return 3;
    case Tone::Pass: return 2;
    case Tone::Noted: return 1;
    default: return 0;
    }
}
// One chip for one APDO (`pos`), Token::kind and mv = its slot. The range steps roll up by the shared tone table (roll_up); the APDO's deep sweep steps (PPS / AVS sweeps
// have no row of their own) roll up the same way, a pass that deviates or folds counting as a caution (caution(step, view)),
// a skipped part not counting at all; the chip takes the worse of the two and wears the "S" mark (Token::sweep) when a sweep
// part was planned, ran or runs. PPS / AVS ranges are continuous; the steps only sample them. The chip text stays the advertised
// range: reach() says how far the source got, the Results footer how the sweep ended. `fold`: an EPR AVS range that was only skipped
// (no EPR cable, entry refused, over the cap) has no chip, like the skipped EPR levels (fixed_row).
[[gnu::noinline]] void range_row(TokenBuf& b, std::span<const ct::Step> s, const ctsweep::View& v, A a, uint8_t pos, bool fold) {
    const ct::Step* x = nullptr;   // the APDO's first range step (else its first sweep part): names the range, tags a skip
    Tally t, w;
    bool has_range = false;
    for (const auto& y : s) {
        if (y.algorithm != a || !y.pdo || y.hunt || y.pos != pos) continue;
        const bool sw = ct::is_sweep(y);
        if (!x || (!sw && ct::is_sweep(*x))) x = &y;
        if (!sw) { count(t, y); has_range = true; }   // probes are rows of their own
        else if (y.verdict == ct::Verdict::Pass && caution(y, v)) ++w.capped;
        else if (y.verdict != ct::Verdict::Skip) count(w, y);
    }
    if (!x) return;
    const bool swept = w.pass + w.fail + w.capped + w.pending != 0;
    Tone tn = has_range ? roll_up(t) : Tone::Skip;
    if (swept) {
        const Tone sw = w.fail ? Tone::Fail : roll_up(w);   // one failed part (a lost source) fails the chip, whatever the other part did
        if (tone_rank(sw) > tone_rank(tn)) tn = sw;
    }
    if (fold && a == A::EprAvs && tn == Tone::Skip) return;
    Token* k = b.token(true, tn);   // one chip per APDO
    if (!k) return;
    k->kind = kind_of(a);
    k->mv = pos;
    k->sweep = swept;
    put_text(*k, step_label::range(x->pdo).c_str());   // "3.3-21V 5A", "15-28V 5A": the PPS / AVS word is drawn from `kind`
    if (tn == Tone::Skip) tag(b, *x);   // every step was skipped: say why
    else {   // some levels ran: a "needs load" level (pattern pps-off-at-no-load) still says so next to the range
        for (const auto& y : s)
            if (y.algorithm == a && y.pos == pos && !y.hunt && y.verdict == ct::Verdict::Skip && y.reason == ct::Reason::NeedsLoad) {
                tag(b, y);
                break;
            }
    }
}
// The PD block: the fixed levels of every PD algorithm ascending, then the PPS ranges, the SPR AVS ranges and the EPR AVS ranges, one chip
// per APDO in plan order. A kind the source never offered has no chip (its steps carry no PDO).
[[gnu::noinline]] void pd_row(TokenBuf& b, std::span<const ct::Step> s, const ctsweep::View& v, bool fold) {
    fixed_row(b, s, ct::kPdFixedAlgorithms, fold);
    for (const A a : {A::Pps, A::SprAvs, A::EprAvs}) {
        uint32_t seen = 0;   // PDO slots already looked at
        for (const auto& x : s) {
            if (x.algorithm != a || !x.pdo || x.hunt || x.pos >= 32 || (seen >> x.pos & 1)) continue;
            seen |= 1u << x.pos;
            range_row(b, s, v, a, x.pos, fold);
        }
    }
}
[[gnu::noinline]] void legacy_row(TokenBuf& b, std::span<const ct::Step> s, A a) {
    // A unit whose detection step got no reply (every row a no-answer skip) is one dim cell, as soon as that step settles: the
    // Running grid reads like the Results fold. Chip mv = its first row's, so a token still finds its step.
    const ct::Step* first = nullptr;
    bool silent = true;
    for (const auto& y : s) {
        if (y.algorithm != a || !y.mv) continue;
        if (!first) first = &y;
        silent &= is_no_answer(y);
    }
    if (first && silent) {
        if (Token* k = b.token(false, Tone::Dim)) { k->mv = first->mv; put_text(*k, "No reply"); }   // a word, not a chip: a chip splits at its space
        return;
    }
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
        else if (const auto w = oem_watts::chip(x); w.len) put_text(*k, w.c_str());   // SCP / UFCS: "10V 22.5W", the level's claimed watts
        else if (a == A::Ufcs) put_text(*k, step_label::ufcs_level(x).c_str());   // "9V": planned before the mode list was read
        else put_text(*k, step_label::params(x).c_str());   // "9V"
        if (class_a && !t.pass && !fail) {   // a Class A charger: the neutral chip says why the 20 V level shows no verdict
            if (Token* t = b.token(false, Tone::Dim, true)) std::strncpy(t->text, "Class A", sizeof t->text - 1);
        }
    }
}
// The VOOC family's units (one shared detection) as OEM level rows (OemNotes::vooc_watts, oem_watts.hpp).
bool is_vooc(A a) { return a == A::Vooc || a == A::SuperVooc || a == A::SuperVooc65; }
// A unit's word in a "No reply" / "Skipped" row: the VOOC family's units read once, as "VOOC" (`vooc`: already said); nullptr = said.
const char* fold_word(A a, bool& vooc) {
    if (ct::group(a) == ct::Group::Pd) return "PD";   // the PD block is one word, never its algorithms
    if (!is_vooc(a)) return display_name(a);
    if (vooc) return nullptr;
    vooc = true;
    return "VOOC";
}
// A level the K2 never draws is never a pass: claimed reads framed (Noted), not reached plain dim, not tested struck (Skip).
Tone level_tone(oem_watts::Claim c) {
    return c == oem_watts::Claim::Claimed ? Tone::Noted : c == oem_watts::Claim::NotReached ? Tone::Dim : Tone::Skip;
}
// One level row, levels first .. first+count of oem_watts::kLevels: the family's first row leads with the detection chip of
// `lead` (the unit the plan runs first), then one chip per level, its watts ("120W") in the tone of its claim.
[[gnu::noinline]] void levels_row(TokenBuf& b, std::span<const ct::Step> s, A lead, uint16_t claims, uint8_t first, uint8_t count) {
    const oem_watts::Span sp = oem_watts::span(oem_watts::kLevels[first].family);
    if (first == sp.first) legacy_row(b, s, lead);
    for (unsigned i = first; i < unsigned(first) + count; ++i) {
        Token* k = b.token(true, level_tone(oem_watts::unpack(claims, i - sp.first)));
        if (!k) return;
        put_text(*k, oem_watts::watts(uint32_t(oem_watts::kLevels[i].w) * 1000u).c_str());
    }
}
// The Xiaomi row: one chip per mode the chain unlocked, by the watts of its new level ("55W", "66W"). With no mode, one "auth" chip:
// passed (the chain finished, nothing raised), skipped (it did not run or failed; the reason stays on the steps page), or as it
// stands while it waits. The chain row's own chip shows only then.
[[gnu::noinline]] void probe_row(TokenBuf& b, std::span<const ct::Step> steps) {
    const ct::Step* chain = nullptr;
    bool modes = false;
    for (const auto& x : steps) {
        if (!x.hunt) continue;
        if (ct::is_mi_chain(x)) { chain = &x; continue; }
        modes = true;
        Token* k = b.token(true, tone(x));
        if (!k) break;
        put_text(*k, step_label::params(x).c_str());   // the mode's new level by its watts: "55W", "66W"
        tag(b, x);
    }
    if (modes || !chain) return;
    Token* k = b.token(true, chain->verdict == ct::Verdict::Skip ? Tone::Skip : tone(*chain));
    if (k) put_text(*k, "auth");
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
            if (y.reason == ct::Reason::Folded) {   // the other cautions (walk, transition) do not fold the range
                r.folded = true;
                if (!r.fold_mv || y.measured_mv < r.fold_mv) r.fold_mv = y.measured_mv;
            }
        } else if (y.verdict == ct::Verdict::Fail && y.reason == ct::Reason::Capped) {
            r.capped = true;
            r.hi_mv = std::max(r.hi_mv, y.measured_mv);   // the ceiling the source held
        } else if (y.verdict == ct::Verdict::Fail && y.reason == ct::Reason::Floored) {
            r.floored = true;
            r.floor_mv = std::max(r.floor_mv, y.measured_mv);   // the floor the source held
        }
    }
    r.tone = roll_up(t);
    r.listed = t.adv && !t.pass && !t.fail && !t.capped;
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
    const unsigned ok = t.pass + t.adv;   // an advertised PD offer reads as a pass (nothing measured: only the tone)
    if (!ok && !t.fail && !t.capped) return t.skip ? Tone::Skip : t.noted ? Tone::Noted : Tone::Pending;
    if (!t.fail && !t.capped && !t.skip) return Tone::Pass;
    return ok || t.capped ? Tone::Partial : Tone::Fail;
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
    } else if (r.folded && r.fold_mv) {
        put_word(t, "folds to "); put_level(t, r.fold_mv); put_word(t, "V");   // a level the sweep found folded back reads this low
    } else if (r.tone == Tone::Pending) {
        put_word(t, "not finished");
    } else if (r.listed) {
        put_word(t, "advertised");   // a Light run's listing: never asked, so never a shortfall
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

step_label::Text family_footer(const char* label, const FamilyTally& c) {
    static constexpr const char* kWords[5] = {" pass", " fail", " caution", " listed", " skip"};
    const unsigned n[5] = {c.pass, c.fail, c.caution, c.listed, c.skip};
    step_label::Text t;
    put_word(t, label); put_word(t, ": ");
    size_t head = t.len;
    for (int pass = 0; pass < 2; ++pass) {   // the second pass, when a count did not fit beside the label: the counts alone
        if (pass) t.len = head = 0;
        bool cut = false;
        for (unsigned i = 0; i < 5; ++i) {   // every non-zero count, in order, while it fits the band
            if (!n[i]) continue;
            char d[12]; unsigned k = 0;
            for (unsigned v = n[i]; v; v /= 10) d[k++] = char('0' + v % 10);
            const bool first = t.len == head;
            if (t.len + (first ? 0 : 2) + k + std::strlen(kWords[i]) > kFooterChars) { cut = i < 3; break; }   // the first that does not fit ends the line (never a lesser count in its place); only a pass, fail or caution count that would be lost sends the label away (a lost skip or listed count is no loss)
            if (!first) put_word(t, ", ");
            while (k) t.s[t.len++] = d[--k];
            t.s[t.len] = 0;
            put_word(t, kWords[i]);
        }
        if (!cut) break;
    }
    if (t.len == head) put_word(t, "no steps");
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
// A separate list from the vendor table (pd::vendor_find, which names the card): this one maps a VID to the OEM protocol family,
// the Xiaomi-ecosystem VID 0x2B01 (Zimi/CUKTECH) included.
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

namespace {

[[gnu::noinline]] void emit_facts(Sink& b, const OemNotes& oem, bool any_probe, bool& facts_done,
                void (*header)(Sink&, const OemNotes&, Section), void (*mi_auth_row)(Sink&, const OemNotes&, bool&),
                bool& mi_shown) {
    if (facts_done) return;
    facts_done = true;
    if (oem.mi_auth && !any_probe) { header(b, oem, Section::Xiaomi); mi_auth_row(b, oem, mi_shown); }
    if (!(oem.qc_label || oem.vendor || oem.oppo_svid || oem.scp_cap || oem.vooc_ident || oem.svooc_cap || oem.rebalanced)) return;
    header(b, oem, Section::OemPd);
    if (oem.rebalanced) {
        b.begin(CompactKind::Fact, "SOURCE", Tone::Caution, A::Count);
        b.word("rebalanced", Tone::Caution);
        char nx[5] = {};
        char rev[3] = {};
        unsigned v = oem.rebalanced, n = 0;
        do { rev[n++] = char('0' + v % 10); v /= 10; } while (v && n < sizeof rev);
        for (unsigned i = 0; i < n; ++i) nx[i] = rev[n - 1 - i];
        nx[n] = 'x';
        b.word(nx, Tone::Caution);
    }
    if (oem.qc_label) {
        const std::string_view name = oem.qc_label;
        const size_t sp = std::min(name.find(' '), name.size());
        b.begin(CompactKind::Fact, name.substr(0, sp), Tone::Pass, A::Count);
        b.phrase(name.substr(sp), Tone::Dim, false);
        if (oem.qc_why) b.phrase(oem.qc_why, Tone::Dim, true);
    }
    if (oem.vendor) { b.begin(CompactKind::Fact, "OEM", Tone::Dim, A::Count); b.phrase(oem.vendor, Tone::Dim, false); }
    if (oem.oppo_svid) { b.begin(CompactKind::Fact, "OPPO", Tone::Dim, A::Count); b.phrase("SVID 0x22D9 (discovery only)", Tone::Dim, false); }
    if (oem.scp_cap) { b.begin(CompactKind::Fact, "SCP cap", Tone::Dim, A::Count); b.phrase(oem.scp_cap, Tone::Dim, false); }
    if (oem.vooc_ident) { b.begin(CompactKind::Fact, "VOOC id", Tone::Dim, A::Count); b.phrase(oem.vooc_ident, Tone::Dim, false); }
    if (oem.svooc_cap) { b.begin(CompactKind::Fact, "SV cap", Tone::Dim, A::Count); b.phrase(oem.svooc_cap, Tone::Dim, false); }
}

// The VOOC family as level rows (OemNotes::vooc_watts): "VOOC" then "SVOOC", one row per run of equal words in the level table.
// A family that never answered (no step passed) folds as its lead unit's one word into "No reply" / "Skipped", never three words.
[[gnu::noinline]] void vooc_rows(Sink& b, std::span<const ct::Step> steps, const OemNotes& oem, A lead,
                                 bool (&absent)[unsigned(A::Count)], bool (&skipped)[unsigned(A::Count)]) {
    bool pass = false, gone = false, skipped_only = true;
    for (const A a : {A::Vooc, A::SuperVooc, A::SuperVooc65}) {
        bool planned = false;
        for (const auto& x : steps)
            if (x.algorithm == a) { planned = true; pass |= x.verdict == ct::Verdict::Pass; }
        if (!planned) continue;
        gone |= is_absent(steps, a);
        skipped_only &= is_skipped_only(steps, a);
    }
    if (!pass && oem.fold && (gone || skipped_only)) { (gone ? absent : skipped)[unsigned(lead)] = true; return; }
    const oem_watts::Span sp = oem_watts::span(oem_watts::Family::Vooc);
    for (unsigned i = sp.first; i < unsigned(sp.first) + sp.count;) {
        unsigned j = i + 1;
        while (j < unsigned(sp.first) + sp.count && std::strcmp(oem_watts::kLevels[j].word, oem_watts::kLevels[i].word) == 0) ++j;
        b.begin(CompactKind::Levels, oem_watts::kLevels[i].word, Tone::Dim, lead, oem.vooc_claims);
        b.levels(uint8_t(i), uint8_t(j - i));
        TokenBuf tb = b.buf();
        levels_row(tb, steps, lead, oem.vooc_claims, uint8_t(i), uint8_t(j - i));
        i = j;
    }
}

void build_impl(Sink& b, std::span<const ct::Step> steps, const OemNotes& oem, const ctsweep::View& sweep) {
    struct Key { A a; bool probe, pd; };   // a: the protocol, or the PD block's lead (the first PD algorithm the plan runs)
    Key keys[unsigned(A::Count) + 2];
    size_t key_n = 0;
    bool absent[unsigned(A::Count)] = {}, skipped[unsigned(A::Count)] = {};
    bool any_probe = false;
    for (const auto& s : steps) {
        if (is_revision(s.algorithm)) continue;
        const bool pd = !s.hunt && ct::group(s.algorithm) == ct::Group::Pd;
        const Key k{s.hunt ? A::Count : s.algorithm, s.hunt, pd};   // every PD step shares one key (the block); the Xiaomi unit is one row
        bool seen = false;
        for (size_t i = 0; i < key_n && !seen; ++i)
            seen = pd ? keys[i].pd : (!keys[i].pd && keys[i].a == k.a && keys[i].probe == k.probe);
        if (seen || key_n == std::size(keys)) continue;
        keys[key_n++] = k;
        any_probe |= k.probe;
    }
    auto section_of_key = [](const Key& k) { return k.probe ? Section::Xiaomi : section_of(k.a); };
    auto header = [](Sink& s, const OemNotes& o, Section sec) {
        if (!o.headers) return;
        s.begin(CompactKind::Header, section_name(sec), Tone::Caution, A::Count, static_cast<uint16_t>(sec));
        s.flush();
    };
    bool mi_shown = false;
    auto mi_auth_row = [](Sink& s, const OemNotes& o, bool& shown) {
        if (!o.mi_auth || shown) return;
        shown = true;
        s.begin(CompactKind::Fact, "Mi Turbo", Tone::Pass, A::Count);
        s.word("auth ok", Tone::Dim);
    };
    bool facts_done = false, vooc_done = false;
    for (size_t at = 0; at < key_n;) {
        size_t end = at + 1;
        while (end < key_n && section_of_key(keys[end]) == section_of_key(keys[at])) ++end;
        const Section sec = section_of_key(keys[at]);
        if (sec == Section::UsbPd || sec == Section::Xiaomi)
            emit_facts(b, oem, any_probe, facts_done, header, mi_auth_row, mi_shown);
        const size_t before_row = b.row_count();
        const size_t before_line = b.line_count();
        header(b, oem, sec);
        const size_t after_header = b.row_count();
        for (size_t i = at; i < end; ++i) {
            const Key& key = keys[i];
            if (key.probe) {
                b.begin(CompactKind::Probe, "Xiaomi", Tone::Dim, A::Count, 0);
                TokenBuf tb = b.buf();
                probe_row(tb, steps);
                continue;
            }
            const A a = key.a;
            if (oem.vooc_watts && is_vooc(a)) {   // the family's rows stand where the plan first runs one of its units
                if (!vooc_done) vooc_rows(b, steps, oem, a, absent, skipped);
                vooc_done = true;
                continue;
            }
            if (key.pd) {   // one block for every PD algorithm; a source that offered none of it folds as the one word "PD"
                b.begin(CompactKind::Pd, "PD", Tone::Dim, a, oem.fold ? 1 : 0);
                TokenBuf tb = b.buf();
                pd_row(tb, steps, sweep, oem.fold);
                if (b.open_tok_count() == 0) {
                    b.drop_open();
                    (pd_absent(steps) ? absent : skipped)[unsigned(a)] = true;
                }
                continue;
            }
            if (oem.fold && is_absent(steps, a)) { absent[unsigned(a)] = true; continue; }
            if (is_no_qc3plus(steps, a)) {
                b.begin(CompactKind::NoQc3Plus, display_name(a), Tone::Dim, a);
                b.word("no QC3+", Tone::Dim);
                continue;
            }
            if (oem.fold && is_skipped_only(steps, a) && !is_class_a_only(steps, a)) {
                skipped[unsigned(a)] = true;
                continue;
            }
            b.begin(CompactKind::Legacy, display_name(a), Tone::Dim, a);
            TokenBuf tb = b.buf();
            legacy_row(tb, steps, a);
            if (b.open_tok_count() == 0 && is_skipped_only(steps, a)) {
                b.drop_open();
                skipped[unsigned(a)] = true;
            }
        }
        b.flush();
        if (after_header > before_row && b.row_count() == after_header) b.rewind(before_row, before_line);
        at = end;
    }
    emit_facts(b, oem, any_probe, facts_done, header, mi_auth_row, mi_shown);
    auto collapsed = [&](const bool (&set)[unsigned(A::Count)], const char* label, Tone t, uint16_t slot) {
        uint32_t mask = 0;
        for (size_t i = 0; i < key_n; ++i)
            if (!keys[i].probe && set[unsigned(keys[i].a)]) mask |= 1u << unsigned(keys[i].a);
        if (!mask) return;
        if (b.g) b.g->collapsed_masks[slot] = mask;
        b.begin(CompactKind::Collapsed, label, t, A::Count, slot);
        bool said[unsigned(A::Count)] = {};
        bool vooc = false;   // the VOOC family folds as one word, its family's ("VOOC"), never three versions
        for (size_t i = 0; i < key_n; ++i)
            if (!keys[i].probe && set[unsigned(keys[i].a)] && !said[unsigned(keys[i].a)]) {
                said[unsigned(keys[i].a)] = true;
                if (const char* w = fold_word(keys[i].a, vooc)) b.word(w, t);
            }
    };
    collapsed(absent, "No reply", Tone::Dim, 0);
    collapsed(skipped, "Skipped", Tone::Caution, 1);
    b.flush();
}
}  // namespace

void build(Layout& l, std::span<const ct::Step> steps, const OemNotes& oem, const ctsweep::View& sweep) {
    // Reset in place: `l = {}` builds a whole Layout on the stack first (about 2 KB, over the frame limit).
    for (Token& k : l.tokens) k = {};
    for (Row& r : l.rows) r = {};
    l.token_count = l.row_count = 0;
    l.cuts = 0;
    Sink s;
    s.l = &l;
    build_impl(s, steps, oem, sweep);
}

size_t build_compact(CompactGrid& g, const Metrics& m, std::span<CompactLine> out_lines,
                     std::span<const ct::Step> steps, const OemNotes& oem, const ctsweep::View& sweep) {
    g.row_count = g.fact_count = g.cuts = 0;
    g.pool[0] = 0;
    g.pool_used = 1;
    g.collapsed_masks = {};
    size_t line_n = 0;
    RowTokens rt;
    Sink s;
    s.g = &g;
    s.m = &m;
    s.lines = out_lines;
    s.line_n = &line_n;
    s.rt = &rt;
    build_impl(s, steps, oem, sweep);
    return line_n;
}

bool row_has(const CompactRow& r, const ct::Step& s) {
    if (r.kind == CompactKind::Header || r.kind == CompactKind::Fact || r.kind == CompactKind::Collapsed) return false;
    if (r.kind == CompactKind::Probe) return s.hunt;
    if (r.kind == CompactKind::Levels)   // every level row of a family lists the family's shared detection
        return !s.hunt && oem_watts::in_family(oem_watts::kLevels[r.first].family, s.algorithm);
    if (s.hunt) return false;
    if (r.kind == CompactKind::Pd) return ct::group(s.algorithm) == ct::Group::Pd;   // every PD step: fixed, PPS, AVS, sweeps, revisions
    return !ct::is_sweep(s) && static_cast<A>(r.algo) == s.algorithm;
}

int row_section(const CompactGrid& g, size_t row) {
    for (size_t r = row + 1; r-- > 0;) {
        const CompactRow& cr = g.rows[r];
        if (cr.kind == CompactKind::Probe) return static_cast<int>(Section::Xiaomi);
        if (cr.kind != CompactKind::Header && cr.kind != CompactKind::Fact && cr.kind != CompactKind::Collapsed)
            return static_cast<int>(section_of(static_cast<A>(cr.algo)));
    }
    return -1;
}

const char* row_label(const CompactGrid& g, size_t row) {
    const CompactRow& cr = g.rows[row];
    switch (cr.kind) {
    case CompactKind::Header: return section_name(static_cast<Section>(cr.algo));
    case CompactKind::Probe: return "Xiaomi";
    case CompactKind::Fact: return g.pool.data() + cr.mv;
    case CompactKind::Collapsed: return cr.mv ? "Skipped" : "No reply";
    case CompactKind::Levels: return oem_watts::kLevels[cr.first].word;
    case CompactKind::Pd: return "PD";
    default: return display_name(static_cast<A>(cr.algo));
    }
}

void row_tokens(const CompactGrid& g, size_t row, std::span<const ct::Step> steps, const ctsweep::View& sweep, RowTokens& out) {
    out.count = 0;
    const CompactRow& cr = g.rows[row];
    out.label_tone = (cr.kind == CompactKind::Fact || cr.kind == CompactKind::Collapsed) ? static_cast<Tone>(cr.algo)
                     : cr.kind == CompactKind::Header ? Tone::Caution : Tone::Dim;
    const char* lbl = row_label(g, row);
    uint8_t dummy_cuts = 0;
    TokenBuf tb{out.toks.data(), kMaxRowTokens, &out.count, &dummy_cuts};
    tb.put_str(out.label, sizeof out.label, lbl);
    switch (cr.kind) {
    case CompactKind::Header: break;
    case CompactKind::Probe: probe_row(tb, steps); break;
    case CompactKind::Pd: pd_row(tb, steps, sweep, cr.mv & 1u); break;
    case CompactKind::NoQc3Plus: tb.word("no QC3+", Tone::Dim); break;
    case CompactKind::Legacy: legacy_row(tb, steps, static_cast<A>(cr.algo)); break;
    case CompactKind::Levels: levels_row(tb, steps, static_cast<A>(cr.algo), cr.mv, cr.first, cr.count); break;
    case CompactKind::Fact:
        for (uint8_t i = 0; i < cr.count; ++i) {
            const FactToken& ft = g.fact_toks[cr.first + i];
            tb.word(std::string_view(g.pool.data() + ft.off, ft.len()), ft.tone());
        }
        break;
    case CompactKind::Collapsed: {
        const uint32_t mask = g.collapsed_masks[cr.mv];
        const Tone t = cr.mv ? Tone::Caution : Tone::Dim;
        bool said[unsigned(A::Count)] = {};
        bool vooc = false;
        for (const auto& s : steps) {
            if (s.hunt || is_revision(s.algorithm)) continue;
            const unsigned a = unsigned(s.algorithm);
            if ((mask & (1u << a)) && !said[a]) {
                said[a] = true;
                if (const char* w = fold_word(s.algorithm, vooc)) tb.word(w, t);
            }
        }
        break;
    }
    }
}

bool row_has(const Row& r, const ct::Step& s) {
    if (r.header) return false;
    if (r.probe) return s.hunt;
    if (r.levels) return !s.hunt && oem_watts::in_family(oem_watts::kLevels[r.pos].family, s.algorithm);   // pos: its first level
    if (s.hunt) return false;
    if (r.pd) return ct::group(s.algorithm) == ct::Group::Pd;
    return !ct::is_sweep(s) && r.algorithm == s.algorithm;
}

step_label::Text levels_footer(const CompactRow& r, const char* label) {
    step_label::Text t;
    if (r.kind != CompactKind::Levels || !r.count) return t;
    const oem_watts::Span sp = oem_watts::span(oem_watts::kLevels[r.first].family);
    unsigned top = 0;   // the highest claimed level + 1; 0: none claimed
    bool ran = false;   // some level not reached: the ladder ran its course below it
    for (unsigned i = r.first; i < unsigned(r.first) + r.count; ++i) {
        const oem_watts::Claim c = oem_watts::unpack(r.mv, i - sp.first);
        if (c == oem_watts::Claim::Claimed) top = i + 1;
        ran |= c == oem_watts::Claim::NotReached;
    }
    char w[24] = {};
    Out o{w, sizeof w};
    if (top) {   // "up to 120W claimed"; a one-level row names its level: "20W claimed"
        if (r.count > 1) o.put("up to ");
        o.num(oem_watts::kLevels[top - 1].w);
        o.put("W claimed");
    } else {   // "50-120W not reached" / "20W not tested"
        o.num(oem_watts::kLevels[r.first].w);
        if (r.count > 1) { o.put('-'); o.num(oem_watts::kLevels[r.first + r.count - 1].w); }
        o.put(ran ? "W not reached" : "W not tested");
    }
    if (std::strlen(label) + 2 + o.len <= kFooterChars) { put_word(t, label); put_word(t, ": "); }
    put_word(t, w);
    return t;
}

void add_note(Layout& l, const char* label, Tone t, std::initializer_list<const char*> words) {
    Sink b;
    b.l = &l;
    b.begin(CompactKind::Fact, label, t, A::Count);
    if (!b.lrow) return;
    for (const char* w : words) b.word(w, Tone::Dim);
    b.flush();
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
    if (x.hunt) return x.ma;   // a Xiaomi mode: the current it asks (its label words the watts)
    if (const unsigned ma = step_label::amps_ma(x)) return ma;
    return x.avs && x.pdo ? pd::current_limit_ma(pd::decode_pdo(x.pdo), x.mv) : 0u;
}
struct Best { bool have = false; const ct::Step* step = nullptr; unsigned mw = 0, mv = 0, ma = 0; bool hs = false; unsigned cw = 0; };
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
        const unsigned cw = oem_watts::step_mw(x);   // SCP / UFCS: the level's claimed watts, its line's words; never ranked across families
        Best& b = best[unsigned(f)];
        // Within the family the claimed watts lead the volts (the line names them); on a tie the later (more capable) protocol wins.
        if (!b.have || mw > b.mw || (mw == b.mw && cw > b.cw) || (mw == b.mw && cw == b.cw && mv > b.mv) ||
            (mw == b.mw && cw == b.cw && mv == b.mv && x.algorithm >= b.step->algorithm))
            b = {true, &x, mw, mv, ma, unproven && x.detail == uint8_t(fc::Fail::NoVoltage), cw};
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
        o.put(order[i] == Fam::Oppo ? "VOOC" : step_label::protocol(*b.step).c_str());   // the family's word, never a version
        o.put(' ');
        if (b.cw) o.put(oem_watts::watts(b.cw).c_str());   // "SCP 22.5W", "UFCS 60W": claimed, unranked
        else if (b.mv) o.put(step_label::params(*b.step).c_str());   // "20V 3.35A", "12V"
        else o.put(b.hs ? "ok" : "seen");
        line.mw = b.mw; line.mv = b.mv;
    }
    return written;
}

uint8_t kind_of(A a) { return a == A::Pps ? kKindPps : a == A::SprAvs ? kKindSprAvs : a == A::EprAvs ? kKindEprAvs : uint8_t(0); }
A kind_algorithm(uint8_t kind) { return kind == kKindPps ? A::Pps : kind == kKindSprAvs ? A::SprAvs : kind == kKindEprAvs ? A::EprAvs : A::Count; }
const char* kind_word(uint8_t kind) { return kind == kKindPps ? "PPS" : kind ? "AVS" : ""; }
// range_row's chip rule: any step names the range; a folded EPR AVS range needs one that was not skipped.
uint16_t range_positions(std::span<const ct::Step> s) {
    uint16_t m = 0;
    for (const auto& x : s) {
        if (!kind_of(x.algorithm) || !x.pdo || x.hunt || !x.pos || x.pos > 15) continue;
        if (x.algorithm == A::EprAvs && x.verdict == ct::Verdict::Skip) continue;
        m = uint16_t(m | 1u << x.pos);
    }
    return m;
}
int token_width(const Token& t, const Metrics& m) {
    const int word = t.kind ? int(std::strlen(kind_word(t.kind)) + 1) : 0;   // "PPS " is drawn before a range chip's volts
    return (int(std::strlen(t.text)) + word) * m.adv + (t.chip ? 2 * m.pad + m.mark : 0) + (t.sweep ? m.sweep : 0);   // the S mark: its fixed gap + one glyph
}
int cell_width(const Token& t, const Metrics& m) {
    int n = t.kind ? int(std::strlen(kind_word(t.kind)) + 1) : 0;
    for (int i = 0; t.text[i] && (!t.chip || t.text[i] != ' '); ++i) ++n;   // a chip's volt text; a word is whole
    return n * m.adv + 2 * m.pad + (t.sweep ? m.sweep : 0);
}
char mark(Tone t) {
    switch (t) {
    case Tone::Pass: return '\x01';
    case Tone::Fail: return '\x02';
    case Tone::Partial: return '~';
    case Tone::Skip: return '-';
    case Tone::Pending: return '.';
    case Tone::Noted: return 'i';   // listed (advertised / not asked): information, not a verdict
    default: return ' ';
    }
}

size_t wrap(const Layout& l, const Metrics& m, std::span<Line> out) {
    struct Ctx { std::span<Line> out; size_t n; uint8_t r; uint8_t rfirst; };
    Ctx c{out, 0, 0, 0};
    for (size_t r = 0; r < l.row_count; ++r) {
        const Row& row = l.rows[r];
        if (row.header) {
            if (c.n < out.size()) out[c.n++] = {static_cast<uint8_t>(r), row.first, 0, true};
            continue;
        }
        c.r = static_cast<uint8_t>(r);
        c.rfirst = row.first;
        wrap_row_tokens(std::span<const Token>(l.tokens.data() + row.first, row.count), m,
                        [](void* p, uint8_t first, uint8_t count, bool head) {
                            auto* cx = static_cast<Ctx*>(p);
                            if (cx->n < cx->out.size())
                                cx->out[cx->n++] = {cx->r, static_cast<uint8_t>(cx->rfirst + first), count, head};
                        },
                        &c);
    }
    return c.n;
}
}  // namespace k2::result_layout
