// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Tools -> PD Spec Check page (see k2/ui/ui_pdspec_page.hpp). K2_PDSPEC=1 only.
#include "k2/ui/ui_pdspec_page.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>

#include "k2/drivers/fusb302.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/pd_spec_session.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

namespace sc = pdspec;
using Stage = PdSpecPage::Stage;

static_assert(PdSpecPage::kFamilyRows == static_cast<int>(sc::kFamilies), "one Setup row per family");

// ---- the copy (font 16 = 8 px, font 12 = 6 px a character) ---------------------------------------------------------------------------

// The focused Setup row's note (font 12), by sc::Family; the row text is sc::family_name.
constexpr const char *kFamilyNotes[] = {
    "fixed level pairs, up and down", "20 mV PPS steps, up and back", "PPS without refresh: its reset",
    "EPR keep-alive held back",           "K2 Hard Reset at the top level", "Rd off at the top level, back",
};
static_assert(std::size(kFamilyNotes) == sc::kFamilies, "one note per family");
constexpr const char *kRowNotes[] = {"Review, then start", "the result of the last run"};
static_assert(all_fit(kFamilyNotes, kLabelAdv, kit::kNoteMaxPx) && all_fit(kRowNotes, kLabelAdv, kit::kNoteMaxPx),
              "a Setup note is wider than the note line");

// The ResultGrid's label strip (font 12, right-aligned at x55, kResultLabelW - 8 = 52 px): at most 8 characters, by sc::Check.
constexpr const char *kCheckShort[] = {
    "Accept",  "PS_RDY",  "VBUS go", "Rdy high", "Rdy low", "Rise",   "Fall",   "Oversh.",  "Level",   "PPS mono", "PPS lvl",
    "PPS set", "PPS t/o", "EPR t/o", "HR 0V",    "HR off",  "HR on",  "HR caps", "Detach",  "Re-att.", "Src rst",
};
static_assert(std::size(kCheckShort) == sc::kChecks, "one grid label per check");
static_assert(all_fit(kCheckShort, kLabelAdv, kit::kResultLabelW - 8), "a check label is wider than the label strip");
// The first check of each grid section (a 2 px rule above): transitions, PPS, the timers, Hard Reset, detach, source reset.
constexpr bool section_start(int k) {
    return k == static_cast<int>(sc::Check::PpsMonotonic) || k == static_cast<int>(sc::Check::PpsKeepAlive) ||
           k == static_cast<int>(sc::Check::HrSafe0V) || k == static_cast<int>(sc::Check::DetachSafe0V) ||
           k == static_cast<int>(sc::Check::SourceReset);
}

// A skip reason as a grid cell (font 16 in the lone cell, 150 px) and as a Detail value (18 characters).
static_assert(fits_px(kSmallAdvPx, sc::skip_name(sc::Skip::NoEprProof), 150), "a skip reason is wider than its cell");

// sc::Stop as the Results footer (font 16).
constexpr const char *kStopFoot[] = {
    "Not finished", "Done", "Stopped", "Stopped: VBUS over limit", "Stopped: load on output", "Stopped: source gone",
    "Stopped: step stuck", "Stopped: no PD contract", "Stopped: link inline", "Stopped: 20 min cap",
};
static_assert(std::size(kStopFoot) == static_cast<std::size_t>(sc::Stop::Timeout) + 1, "one footer per Stop");
static_assert(all_fit(kStopFoot, kSmallAdvPx, kFooterTextPx), "a stop footer is wider than the footer line");

constexpr const char *kFooters[] = {
    "Keep the output empty",   "Running: keep output empty", "Stopping: back to 5V",  "Pick at least one family",
    "Another tool holds port", "No PD source",               "Load inline: unplug it", "K2 runs on this charger",
    "No power meter",          "Already running",            "Not back at 5V: unplug", "No result yet",
};
enum Foot : uint8_t { kReview, kRunning, kStopping, kPickOne, kBusy, kNoCaps, kInline, kSelfPowered, kNoMeter, kAlready, kNot5V, kNoResult };
static_assert(all_fit(kFooters, kSmallAdvPx, kFooterTextPx), "a PD Spec footer is wider than the footer line");

constexpr const char kHint[] = "Output must stay empty";   // the ActionCell's caution line (font 12)
static_assert(fits_px(kLabelAdv, kHint, 224 - 40 - 15), "the START hint is wider than the action cell");

// Review.
constexpr kit::KvLayout kReviewKv{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, kit::kKvValueX, 18, Align::Left};
constexpr const char *kReviewKeys[] = {"Checks", "Charger", "Ceiling", "EPR", "Output"};
constexpr int kReviewRows = static_cast<int>(std::size(kReviewKeys));
static_assert(all_fit(kReviewKeys, kSmallAdvPx, kit::kKvValueX - 6 - 4), "a Review key runs into the value column");
constexpr int16_t kReviewRuleY = kit::kBodyY + kReviewRows * kit::kRowH;   // 154
constexpr int16_t kActionY = kReviewRuleY + 2, kActionH = 64;

// Running (level 1).
constexpr int16_t kFamY = 26, kClockX = 170, kProgY = 44, kItemY = 50, kVbusLabelY = 74, kHeroX = 196, kHeroY = 70;
constexpr int16_t kCountRuleY = 120, kSplitX = 112;
constexpr kit::KvLayout kCountsL{kCountRuleY + 2, kit::kRowH, 6, &kLabelFont, nullptr, 76, 4, Align::Right};
constexpr kit::KvLayout kCountsR{kCountRuleY + 2, kit::kRowH, 116, &kLabelFont, nullptr, 186, 4, Align::Right};
constexpr const char *kCountKeysL[] = {"Steps", "PPS points"};
constexpr const char *kCountKeysR[] = {"Fails", "Score"};
static_assert(all_fit(kCountKeysL, kLabelAdv, 76 - 6 - 2) && all_fit(kCountKeysR, kLabelAdv, 186 - 116 - 2), "a count key runs into its value");
constexpr int kCountRows = 2;
constexpr uint32_t kHeroMs = 250;

// Results.
constexpr int16_t kScoreY = 26, kScoreX = 54, kScoreNoteX = 132, kBandRuleY = 52, kTallyY = 54, kTallyRuleY = 78, kGridY = 80;
constexpr int16_t kTallyW = 74, kTallyNumX = 4, kTallyCellX = 24, kTallyCellW = 46, kTallyCellH = 20;
constexpr const char *kTallyWords[] = {"pass", "fail", "skip"};
static_assert(all_fit(kTallyWords, kLabelAdv, kTallyCellW - 4), "a counter word is wider than its cell");

// Detail: a KvTable at x72 (18 characters).
constexpr kit::KvLayout kDetail{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, kit::kKvValueX, 18, Align::Left};
constexpr const char *kDetailKeys[PdSpecPage::kDetailRows] = {"Verdict", "Worst", "Limit", "Count", "Fails", "At", "Penalty"};
static_assert(all_fit(kDetailKeys, kSmallAdvPx, kit::kKvValueX - 6 - 4), "a Detail key runs into the value column");

constexpr uint32_t kNoteMs = 2500;

struct Budget {
    int cap;
    int spent = 0;
    bool admit(int px) {
        if (px <= cap - spent) { spent += px; return true; }
        return false;
    }
};

void draw_cell(Field &f, uint32_t &sig, std::string_view text, uint16_t color, int box_px, Budget &b) {
    const uint32_t h = mix_u32(mix(mix_init(), text), color) | 1u;
    if (h == sig || !b.admit(box_px)) return;
    if ((sig >> 16) != (h >> 16) || sig == 0) f.set_colors(color, kBg);
    sig = h;
    f.draw(text);
}

template <std::size_t N>
void put_int(StrBuf<N> &s, int32_t v) {
    if (v < 0) s.put_char('-');
    s.put_u32(v < 0 ? 0u - static_cast<uint32_t>(v) : static_cast<uint32_t>(v));   // INT32_MIN included
}

// One measured value in its unit: us as ms with one decimal, ms whole, mV as volts with two decimals, a count bare.
template <std::size_t N>
void put_value(StrBuf<N> &s, sc::Unit u, int32_t v) {
    if (v >= static_cast<int32_t>(sc::kNever)) { s.put("not seen"); return; }
    switch (u) {
    case sc::Unit::Us: s.put_fixed(v, 1).put("ms"); return;
    case sc::Unit::Ms: put_int(s, v); s.put("ms"); return;
    case sc::Unit::Mv: s.put_fixed(v, 2).put_char('V'); return;
    case sc::Unit::Count: put_int(s, v); return;
    }
}

template <std::size_t N>
void put_limit(StrBuf<N> &s, const sc::CheckDef &d, const sc::Agg &a) {
    switch (d.kind) {
    case sc::Kind::Max: s.put("max "); put_value(s, d.unit, a.hi); return;
    case sc::Kind::Min: s.put("min "); put_value(s, d.unit, a.lo); return;
    case sc::Kind::Range:
        // One open side (an overshoot): only the bound that applies.
        if (a.lo <= -sc::kOpenLimit) { s.put("max "); put_value(s, d.unit, a.hi); return; }
        if (a.hi >= sc::kOpenLimit) { s.put("min "); put_value(s, d.unit, a.lo); return; }
        put_value(s, d.unit, a.lo);
        s.put_char('-');
        put_value(s, d.unit, a.hi);
        return;
    }
}

// The source list in one line: the highest fixed level with its current, then the top of the widest PPS range.
template <std::size_t N>
void put_charger(StrBuf<N> &s, const pd::SourceCaps &caps) {
    const pd::Pdo *fixed = nullptr, *pps = nullptr;
    for (std::size_t i = 0; i < caps.num_pdos && i < caps.pdos.size(); i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type == pd::PdoType::Fixed && (!fixed || p.max_mv > fixed->max_mv)) fixed = &p;
        if (p.type == pd::PdoType::Pps && (!pps || p.max_mv > pps->max_mv)) pps = &p;
    }
    char t[12];
    if (fixed) {
        s.put("PD ").put({t, static_cast<std::size_t>(home::fmt_level(t, fixed->max_mv))}).put_char('V');
        if (!pps) s.put({t, static_cast<std::size_t>(home::fmt_level(t, fixed->max_ma))}).put_char('A');
    }
    if (pps) {
        if (fixed) s.put_char(' ');
        s.put("PPS ").put({t, static_cast<std::size_t>(home::fmt_level(t, pps->max_mv))}).put_char('V');
    }
}

}  // namespace

PdSpecPage::PdSpecPage(UiContext &c) : PageBase(c, Page::Count, "PD Spec Check", Icon::Check) {}

PdSpecSession &PdSpecPage::session() const { return ctx_.ui->pd_spec(); }

const sc::Agg *PdSpecPage::agg(int check) const {
    const sc::Engine *e = session().engine();
    if (!e || check < 0 || check >= static_cast<int>(sc::kChecks)) return nullptr;
    return &e->agg()[static_cast<std::size_t>(check)];
}

int PdSpecPage::setup_count() const { return session().has_result() ? kLastRow + 1 : kDoneRow + 1; }

// ---- lifecycle ----------------------------------------------------------------------------------------------------------------------

// A run going on opens on Running, a result not shown yet on Results, anything else on Setup.
void PdSpecPage::enter() {
    confirm_until_ = 0;
    note_.clear();
    note_until_ = 0;
    const PdSpecSession &ss = session();
    if (ss.active()) stage_ = Stage::Running;
    else if (ss.has_result() && ss.run_no() != ctx_.keep.pdspec.seen_run) stage_ = Stage::Results;
    else stage_ = Stage::Setup;
    if (stage_ == Stage::Results) ctx_.keep.pdspec.seen_run = ss.run_no();
    res_sel_ = 0;
    laid_ = bar_drawn_ = false;
}

void PdSpecPage::draw_static() {
    bar();
    invalidate();
}

void PdSpecPage::invalidate() {
    laid_ = false;
    for (Field &f : cell_) f.invalidate();
    action_.invalidate();
    prog_.invalidate();
    grid_.invalidate();
    ctx_.rows.invalidate();
    foot_.invalidate();
    clock_s_ = 0xFFFFFFFF;
    hero_ms_ = 0;
}

void PdSpecPage::go(Stage s) {
    if (s != stage_) {
        stage_ = s;
        bar_drawn_ = false;
    }
    laid_ = false;
    confirm_until_ = 0;
    if (s == Stage::Results) ctx_.keep.pdspec.seen_run = session().run_no();
}

void PdSpecPage::bar() {
    std::string_view t;
    int step = 0;
    switch (stage_) {
    case Stage::Setup: t = "Checks"; step = 1; break;
    case Stage::Review: t = "Review"; step = 2; break;
    case Stage::Running: t = "PD Spec Check"; break;
    case Stage::Results: t = "Result"; break;
    case Stage::Detail: t = sc::kDefs[static_cast<std::size_t>(std::clamp(res_sel_, 0, static_cast<int>(sc::kChecks) - 1))].label; break;
    }
    kit::draw_top_bar(ctx_.content, t, step, fusb302::self_powered() ? "SNK" : "PAS", true, 2);
    bar_valid_ = false;
    bar_drawn_ = true;
}

// ---- start --------------------------------------------------------------------------------------------------------------------------

bool PdSpecPage::start_run() {
    const sc::StartResult r = session().start(ctx_.keep.pdspec.mask, ctx_.now);
    switch (r) {
    case sc::StartResult::Ok: go(Stage::Running); return true;
    case sc::StartResult::Busy: say(kFooters[kBusy], kCaution); break;
    case sc::StartResult::NoCaps: say(kFooters[kNoCaps], kCaution); break;
    case sc::StartResult::Inline: say(kFooters[kInline], kCaution); break;
    case sc::StartResult::SelfPowered: say(kFooters[kSelfPowered], kCaution); break;
    case sc::StartResult::NoMonitor: say(kFooters[kNoMeter], kCaution); break;
    case sc::StartResult::Running: say(kFooters[kAlready], kCaution); break;
    }
    return false;
}

// ---- rows ---------------------------------------------------------------------------------------------------------------------------

void PdSpecPage::setup_row(int i, bool, kit::RowSpec &out) {
    if (i < kFamilyRows) {
        out.marker = kit::Marker::Check;
        out.state = (ctx_.keep.pdspec.mask >> i) & 1u;
        out.text = sc::family_name(static_cast<sc::Family>(i));
        out.note = kFamilyNotes[i];
        return;
    }
    if (i == kDoneRow) {
        out.text = "Done";
        out.chevron = true;
        out.note = kRowNotes[0];
        return;
    }
    out.marker = kit::Marker::Icon;
    out.icon = Icon::Report;
    out.text = "Last result";
    row_tail_.clear();
    if (const sc::Engine *e = session().engine()) {
        row_tail_.put("score ").put_u32(e->score());
        out.tail = row_tail_.view();
    }
    out.note = kRowNotes[1];
}

void PdSpecPage::result_row(int i, kit::ResultRowSpec &out) {
    out.n = 1;
    out.focus = i == res_sel_;
    out.section = section_start(i);
    out.label = kCheckShort[static_cast<std::size_t>(std::clamp(i, 0, static_cast<int>(sc::kChecks) - 1))];
    kit::ResultCell &c = out.cells[0];
    const sc::Agg *a = agg(i);
    if (!a) { c = {kit::Verdict::None, "", {}}; return; }
    cell_text_.clear();
    switch (sc::verdict(static_cast<sc::Check>(i), *a)) {
    case sc::Verdict::Skip: c = {kit::Verdict::Skip, sc::skip_name(a->skip), {}}; return;
    case sc::Verdict::Pending: c = {kit::Verdict::Pending, "", {}}; return;
    case sc::Verdict::Unverified:   // an inferred limit: the number, text only (no pass/fail look)
        put_value(cell_text_, sc::kDefs[static_cast<std::size_t>(i)].unit, a->worst);
        c = {kit::Verdict::None, cell_text_.view(), {}};
        return;
    case sc::Verdict::Pass:
    case sc::Verdict::Fail: {
        const sc::CheckDef &d = sc::kDefs[static_cast<std::size_t>(i)];
        put_value(cell_text_, d.unit, a->worst);
        if (a->fails) cell_text_.put(", ").put_u32(a->fails).put(" fail");
        c = {a->fails ? kit::Verdict::Fail : kit::Verdict::Pass, cell_text_.view(), {}};
        return;
    }
    }
}

void PdSpecPage::detail_value(int row, StrBuf<20> &s, uint16_t &color) const {
    color = kText;
    const int k = std::clamp(res_sel_, 0, static_cast<int>(sc::kChecks) - 1);
    const sc::Agg *a = agg(k);
    if (!a) return;
    const sc::CheckDef &d = sc::kDefs[static_cast<std::size_t>(k)];
    const bool measured = a->n > 0;
    const bool judged = !d.inferred;   // an inferred limit: reported, never judged
    switch (row) {
    case 0:
        if (measured && !judged) { s.put("Unverified"); return; }
        s.put(measured ? (a->fails ? "Fail" : "Pass") : "Skip");
        color = measured ? (a->fails ? kError : kOk) : kDim;
        return;
    case 1:
        if (measured) put_value(s, d.unit, a->worst);
        else { s.put(sc::skip_name(a->skip)); color = kDim; }
        return;
    case 2:
        if (measured) put_limit(s, d, *a);
        else { s.put("not judged"); color = kDim; }
        return;
    case 3: s.put_u32(a->n); return;
    case 4: s.put_u32(a->fails); color = a->fails && judged ? kCaution : kText; return;
    case 5:
        if (!measured) { s.put("none"); color = kDim; return; }
        if (k < static_cast<int>(sc::Check::PpsMonotonic)) s.put("PDO ").put_u32(a->a).put(" to ").put_u32(a->b);
        else if (k < static_cast<int>(sc::Check::PpsKeepAlive)) s.put("APDO ").put_u32(a->a).put(", point ").put_u32(a->b);
        else if (a->a) s.put("PDO ").put_u32(a->a);
        else { s.put("the source"); color = kDim; }
        return;
    case 6: {
        const uint32_t pts = judged ? static_cast<uint32_t>(d.weight) * a->fails : 0;
        s.put_u32(pts); color = pts ? kCaution : kText; return;
    }
    default: return;
    }
}

void PdSpecPage::setup_row_fn(void *ctx, int index, bool focused, kit::RowSpec &out) {
    static_cast<PdSpecPage *>(ctx)->setup_row(index, focused, out);
}
void PdSpecPage::result_row_fn(void *ctx, int index, kit::ResultRowSpec &out) { static_cast<PdSpecPage *>(ctx)->result_row(index, out); }

// ---- footer -------------------------------------------------------------------------------------------------------------------------

void PdSpecPage::say(std::string_view text, uint16_t color) {
    note_.clear();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + kNoteMs;
    if (note_until_ == 0) note_until_ = 1;
}

void PdSpecPage::compose_footer(uint32_t now_ms) {
    foot_text_.clear();
    foot_tone_ = FooterTone::Info;
    if (note_until_ && static_cast<int32_t>(now_ms - note_until_) >= 0) {
        note_until_ = 0;
        note_.clear();
    }
    if (!note_.view().empty()) {
        foot_tone_ = note_color_ == kDim ? FooterTone::Info : footer_tone_of(note_color_);
        foot_text_.put(note_.view());
        return;
    }
    const sc::Engine *e = session().engine();
    switch (stage_) {
    case Stage::Setup:
        foot_text_.put_u32(static_cast<uint32_t>(std::popcount(static_cast<unsigned>(ctx_.keep.pdspec.mask & sc::kAllFamilies))))
            .put(" of ").put_u32(static_cast<uint32_t>(sc::kFamilies)).put(" picked");
        return;
    case Stage::Review:
        foot_tone_ = FooterTone::Caution;
        foot_text_.put(kFooters[kReview]);
        return;
    case Stage::Running: foot_text_.put(kFooters[session().footer() && std::string_view(session().footer()) == kPdSpecFooters[1] ? kStopping : kRunning]); return;
    case Stage::Results:
        if (!e) { foot_text_.put(kFooters[kNoResult]); return; }
        if (e->release_unconfirmed()) {
            foot_tone_ = FooterTone::Error;
            foot_text_.put(kFooters[kNot5V]);
            return;
        }
        foot_tone_ = e->stop() == sc::Stop::Done ? FooterTone::Ok
                     : e->stop() == sc::Stop::Cancelled || e->stop() == sc::Stop::None ? FooterTone::Info : FooterTone::Caution;
        foot_text_.put(kStopFoot[std::min<std::size_t>(static_cast<std::size_t>(e->stop()), std::size(kStopFoot) - 1)]);
        return;
    case Stage::Detail: {
        const sc::Agg *a = agg(res_sel_);
        if (!a || a->n == 0) { foot_text_.put("Skipped"); return; }
        foot_tone_ = a->fails ? FooterTone::Caution : FooterTone::Ok;
        if (a->fails) foot_text_.put("Fail: ").put_u32(a->fails).put(" of ").put_u32(a->n).put(" measured");
        else foot_text_.put("Pass: ").put_u32(a->n).put(" measured");
        return;
    }
    }
}

// ---- paint --------------------------------------------------------------------------------------------------------------------------

void PdSpecPage::paint_setup() {
    const Surface &c = ctx_.content;
    if (!laid_) {
        if (!ctx_.blank) kit::RowList::clear_body_around(c);
        ctx_.rows.init(c, kit::kBodyY, setup_row_fn, this);
        ctx_.rows.invalidate();
        laid_ = true;
    }
    auto &k = ctx_.keep.pdspec;
    if (k.sel >= setup_count()) k.sel = static_cast<uint8_t>(setup_count() - 1);
    ctx_.rows.set_count(setup_count());
    ctx_.rows.select(k.sel);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

void PdSpecPage::paint_review(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    Field *v = cell_.data();
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        kit::kv_static(c, kReviewKv, kReviewKeys, kReviewRows);
        kit::kv_cells(c, kReviewKv, v, kReviewRows);
        gfx_fill(c, 0, kReviewRuleY, c.w, 2, kRule);
        action_.init(c, kActionY, kActionH);
        for (uint32_t &s : sig_) s = 0;
        laid_ = true;
    }
    Budget b{1 << 30};
    StrBuf<24> t;
    const unsigned n = static_cast<unsigned>(std::popcount(static_cast<unsigned>(ctx_.keep.pdspec.mask & sc::kAllFamilies)));
    if (n == sc::kFamilies) t.put("all 6 families");
    else t.put_u32(n).put(" of 6 families");
    draw_cell(v[0], sig_[0], t.view(), kText, 0, b);
    t.clear();
    const pd::SourceCaps &caps = fusb302::caps();
    if (caps.valid) put_charger(t, caps);
    if (t.view().empty()) draw_cell(v[1], sig_[1], kFooters[kNoCaps], kDim, 0, b);
    else draw_cell(v[1], sig_[1], t.view(), kText, 0, b);
    t.clear();
    char lv[12];
    t.put({lv, static_cast<std::size_t>(home::fmt_level(lv, static_cast<int32_t>(ctx_.cap_mv)))}).put("V max");
    draw_cell(v[2], sig_[2], t.view(), kText, 0, b);
    bool epr = false;
    for (std::size_t i = 0; i < caps.num_pdos && i < caps.pdos.size(); i++) epr = epr || (i >= 7 && caps.pdos[i].max_mv > 20000);
    const auto &cab = fusb302::physical_cable();
    const bool proof = cab.observed && cab.max_mv > 20000 && cab.max_ma >= 5000;
    draw_cell(v[3], sig_[3], !caps.valid || !epr ? "not offered" : proof ? "cable proven" : "no cable proof", epr && proof ? kText : kDim, 0, b);
    t.clear();
    const int32_t ma = ctx_.pm_online ? std::abs(ctx_.pm.current_ma) : 0;
    if (ctx_.pm_online) t.put_fixed(ma, 2).put_char('A');
    else t.put(kFooters[kNoMeter]);
    draw_cell(v[4], sig_[4], t.view(), !ctx_.pm_online ? kDim : ma > sc::kLoadAbortMa ? kCaution : kAmp, 0, b);

    if (confirm_until_ != 0) {
        const int32_t left = std::max<int32_t>(0, static_cast<int32_t>(confirm_until_ - now_ms));
        const int permille = std::clamp<int>(left * 1000 / static_cast<int32_t>(Ui::kConfirmWindowMs), 0, 1000);
        StrBuf<24> sub;
        sub.put("closes in ").put_u32(static_cast<uint32_t>((left + 999) / 1000)).put_char('s');
        action_.draw_armed("CONFIRM", sub.view(), permille);
    } else {
        action_.draw_idle("START", kHint);
    }
}

void PdSpecPage::paint_running(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    Field *v = cell_.data();   // 0 family, 1 clock, 2 item, 3..6 the counts
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        v[0].init(c, kSmall, 6, kFamY, 18, Align::Left, kText, kBg);
        v[1].init(c, kSmall, kClockX, kFamY, 6, Align::Right, kText, kBg);
        prog_.init(c, 6, kProgY, static_cast<int16_t>(c.w - 12));
        v[2].init(c, kSmall, 6, kItemY, 26, Align::Left, kText, kBg);
        gfx_text(c, kLabelFont, 6, kVbusLabelY, "VBUS", kDim, kBg);
        gfx_fill(c, 0, kCountRuleY, c.w, 2, kRule);
        kit::kv_static(c, kCountsL, kCountKeysL, kCountRows);
        for (int i = 0; i < kCountRows; i++)
            gfx_text(c, kLabelFont, kCountsR.label_x, static_cast<int16_t>(kit::kv_row_y(kCountsR, i) + kit::row_text_y(kit::kRowH, kLabelFont.height)),
                     kCountKeysR[i], kDim, kBg);
        gfx_fill(c, kSplitX, static_cast<int16_t>(kCountsL.y0 + 1), 1, static_cast<int16_t>(kCountRows * kit::kRowH - 1), kRule);
        kit::kv_cells(c, kCountsL, v + 3, kCountRows);
        kit::kv_cells(c, kCountsR, v + 3 + kCountRows, kCountRows);
        for (uint32_t &s : sig_) s = 0;
        v[kHeroCell].invalidate();
        hero_ms_ = 0;
        clock_s_ = 0xFFFFFFFF;
        laid_ = true;
    }
    const PdSpecSession &ss = session();
    const sc::Engine *e = ss.engine();
    Budget b{kRunBudgetPx};
    const int cell16 = kSmall.adv * kSmall.height;

    if (hero_ms_ == 0 || now_ms - hero_ms_ >= kHeroMs) {
        const int32_t mv = std::max<int32_t>(0, ctx_.pm.vbus_mv);
        StrBuf<8> t;
        if (!ctx_.pm_online) t.put("--");
        else if (mv < 10000) t.put_fixed(mv, 2);
        else t.put_fixed(mv, 1);
        const bool full = hero_ms_ == 0;
        if (b.admit(full ? 4 * kBig.adv * kBig.height + kMid.adv * kMid.height : 4 * kBig.adv * kBig.height)) {
            kit::draw_hero(c, v[kHeroCell], kit::HeroSpec{&kBig, kHeroX, kHeroY, 4, ctx_.pm_online ? kVolt : kFaint, "V"}, t.view(), full);
            hero_ms_ = now_ms ? now_ms : 1;
        }
    }
    const uint32_t secs = ss.elapsed_ms(now_ms) / 1000u;
    if (secs != clock_s_) {
        StrBuf<8> t;
        t.put_u32(std::min<uint32_t>(secs / 60u, 99u)).put_char(':');
        if (secs % 60u < 10u) t.put_char('0');
        t.put_u32(secs % 60u);
        if (b.admit(6 * cell16)) {
            v[1].draw(t.view());
            clock_s_ = secs;
        }
    }
    // The bar: the picked families done, plus the share of the one in progress.
    int total = 1000, done = 0;
    if (e) {
        const unsigned mask = e->families() & sc::kAllFamilies;
        const int n = std::popcount(mask);
        const unsigned before = mask & ((1u << static_cast<unsigned>(e->family())) - 1u);
        total = std::max(n, 1) * 1000;
        done = std::popcount(before) * 1000 + (e->items() ? std::min<int>(e->item(), e->items()) * 1000 / e->items() : 0);
        done = std::min(done, total);
    }
    const int pc = prog_.cost(done, total);
    if (pc == 0 || b.admit(pc)) prog_.set(done, total);
    const bool live = e && e->running() && e->family() != sc::Family::Count;
    draw_cell(v[0], sig_[0], live ? sc::family_name(e->family()) : "", kText, 18 * cell16, b);
    StrBuf<24> t;
    if (live && e->items() > 1) t.put("item ").put_u32(std::min<uint32_t>(e->item() + 1u, e->items())).put(" of ").put_u32(e->items());
    draw_cell(v[2], sig_[2], t.view(), kDim, 26 * cell16, b);
    uint32_t fails = 0;
    if (e) for (const sc::Agg &a : e->agg()) fails += a.fails;
    const uint32_t vals[kCountCells] = {e ? static_cast<uint32_t>(e->trans().size()) : 0u, e ? static_cast<uint32_t>(e->pps().size()) : 0u,
                                        fails, e ? e->score() : 0u};
    for (int i = 0; i < kCountCells; i++) {
        StrBuf<8> s;
        s.put_u32(std::min<uint32_t>(vals[i], 9999u));
        const uint16_t col = i >= 2 && vals[i] ? kCaution : kText;
        draw_cell(v[3 + i], sig_[static_cast<std::size_t>(3 + i)], s.view(), col, 4 * cell16, b);
    }
}

void PdSpecPage::paint_results() {
    const Surface &c = ctx_.content;
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        const sc::Engine *e = session().engine();
        gfx_text(c, kSmall, 6, 30, "Score", kDim, kBg);
        Field &score = cell_[0];
        score.init(c, kMid, kScoreX, kScoreY, 6, Align::Right, kText, kBg);
        StrBuf<8> s;
        if (e) s.put_u32(e->score());
        score.draw(s.view());
        gfx_text(c, kLabelFont, kScoreNoteX, kScoreY, "penalty,", kDim, kBg);
        gfx_text(c, kLabelFont, kScoreNoteX, static_cast<int16_t>(kScoreY + 13), "0 = ideal", kDim, kBg);
        gfx_fill(c, 0, kBandRuleY, c.w, 2, kRule);
        unsigned nums[3] = {0, 0, 0};
        if (e)
            for (std::size_t k = 0; k < sc::kChecks; ++k) {   // an unverified check is in none of the three
                const sc::Verdict v = sc::verdict(static_cast<sc::Check>(k), e->agg()[k]);
                if (v != sc::Verdict::Unverified) nums[v == sc::Verdict::Pass ? 0 : v == sc::Verdict::Fail ? 1 : 2]++;
            }
        constexpr kit::Verdict kLook[3] = {kit::Verdict::Pass, kit::Verdict::Fail, kit::Verdict::Skip};
        for (int i = 0; i < 3; i++) {
            const int16_t x = static_cast<int16_t>(i * kTallyW);
            StrBuf<4> n;
            n.put_u32(nums[i]);
            gfx_text(c, kSmall, static_cast<int16_t>(x + kTallyNumX), static_cast<int16_t>(kTallyY + 4), n.view(), kText, kBg);
            kit::draw_verdict(c, static_cast<int16_t>(x + kTallyCellX), static_cast<int16_t>(kTallyY + 2), kTallyCellW, kTallyCellH, kLook[i],
                              kTallyWords[i], kBg);
        }
        gfx_fill(c, 0, kTallyRuleY, c.w, 2, kRule);
        grid_.init(c, kGridY, kFooterY, result_row_fn, this);
        grid_.invalidate();
        laid_ = true;
    }
    grid_.set_count(static_cast<int>(sc::kChecks));
    grid_.show(res_sel_);
    grid_.draw();
}

void PdSpecPage::paint_detail() {
    const Surface &c = ctx_.content;
    Field *v = cell_.data();
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        kit::kv_static(c, kDetail, kDetailKeys, kDetailRows);
        kit::kv_cells(c, kDetail, v, kDetailRows);
        for (uint32_t &s : sig_) s = 0;
        laid_ = true;
    }
    Budget b{1 << 30};
    for (int i = 0; i < kDetailRows; i++) {
        StrBuf<20> s;
        uint16_t col = kText;
        detail_value(i, s, col);
        draw_cell(v[i], sig_[static_cast<std::size_t>(i)], s.view(), col, 0, b);
    }
}

void PdSpecPage::update(uint32_t now_ms) {
    // The run decides Running: a run the remote started shows here, and its end opens the result.
    const PdSpecSession &ss = session();
    if (ss.active() && stage_ != Stage::Running) {
        go(Stage::Running);
    } else if (stage_ == Stage::Running && !ss.active()) {
        res_sel_ = 0;
        go(Stage::Results);
    }
    if (confirm_until_ != 0 && static_cast<int32_t>(now_ms - confirm_until_) >= 0) confirm_until_ = 0;
    if (!bar_drawn_) bar();
    title_badge(false);
    switch (stage_) {
    case Stage::Setup: paint_setup(); break;
    case Stage::Review: paint_review(now_ms); break;
    case Stage::Running: paint_running(now_ms); break;
    case Stage::Results: paint_results(); break;
    case Stage::Detail: paint_detail(); break;
    }
    compose_footer(now_ms);
    draw_footer_cached(ctx_.content, foot_tone_, foot_text_.view(), foot_);
}

// ---- input --------------------------------------------------------------------------------------------------------------------------

bool PdSpecPage::on_setup(Event e) {
    auto &k = ctx_.keep.pdspec;
    switch (e.button) {
    case Button::Back: return false;   // the Ui pops the page
    case Button::Minus:
        if (k.sel > 0) k.sel--;
        return true;
    case Button::Plus:
        if (k.sel + 1 < setup_count()) k.sel++;
        return true;
    case Button::Ok:
        if (k.sel < kFamilyRows) {
            k.mask = static_cast<uint8_t>(k.mask ^ (1u << k.sel));
        } else if (k.sel == kDoneRow) {
            if ((k.mask & sc::kAllFamilies) == 0) say(kFooters[kPickOne], kCaution);
            else go(Stage::Review);
        } else if (session().has_result()) {
            res_sel_ = 0;
            go(Stage::Results);
        }
        return true;
    }
    return false;
}

bool PdSpecPage::on_review(Event e) {
    switch (e.button) {
    case Button::Back:
        go(Stage::Setup);
        return true;
    case Button::Ok:
        if (confirm_until_ != 0 && static_cast<int32_t>(ctx_.now - confirm_until_) < 0) {
            confirm_until_ = 0;
            start_run();
        } else if (const char *busy = session().foreign_busy()) {
            say(busy, kCaution);   // refuse before the arm: nothing to confirm (the run's own hold and contract do not count)
        } else {
            confirm_until_ = ctx_.now + Ui::kConfirmWindowMs;
            if (confirm_until_ == 0) confirm_until_ = 1;
            note_.clear();
            note_until_ = 0;
        }
        return true;
    case Button::Minus:
    case Button::Plus: return false;
    }
    return false;
}

bool PdSpecPage::on_results(Event e) {
    switch (e.button) {
    case Button::Back:
        go(Stage::Setup);
        return true;
    case Button::Minus:
        if (res_sel_ > 0) res_sel_--;
        return true;
    case Button::Plus:
        if (res_sel_ + 1 < static_cast<int>(sc::kChecks)) res_sel_++;
        return true;
    case Button::Ok:
        go(Stage::Detail);
        return true;
    }
    return false;
}

bool PdSpecPage::on_event(Event e) {
    if (e.press != Press::Short) return false;
    bool used = false;
    switch (stage_) {
    case Stage::Setup: used = on_setup(e); break;
    case Stage::Review: used = on_review(e); break;
    case Stage::Running:
        if (e.button == Button::Back) {
            session().cancel();   // the run releases to 5 V first; its end opens Results
            used = true;
        } else {
            used = e.button == Button::Ok;
        }
        break;
    case Stage::Results: used = on_results(e); break;
    case Stage::Detail:
        if (e.button == Button::Back) {
            const int keep = res_sel_;
            go(Stage::Results);
            res_sel_ = keep;
        }
        used = true;
        break;
    }
    if (used) update(ctx_.now);
    return used;
}

PanelLabels PdSpecPage::labels() const {
    PanelLabels l = base_labels();
    auto &ok = l.b[static_cast<int>(Button::Ok)];
    const auto none = SlotLabel{"", "", "", false};
    switch (stage_) {
    case Stage::Setup: {
        l = list_labels(l);
        const int s = ctx_.keep.pdspec.sel;
        l.b[static_cast<int>(Button::Ok)].shrt = s < kFamilyRows ? "Toggle" : s == kDoneRow ? "Next" : "Open";
        break;
    }
    case Stage::Review:
        l.b[static_cast<int>(Button::Minus)] = none;
        l.b[static_cast<int>(Button::Plus)] = none;
        ok.shrt = confirm_until_ != 0 ? "Confirm" : "Start";
        break;
    case Stage::Running:
        l.b[static_cast<int>(Button::Back)].shrt = "Stop";
        l.b[static_cast<int>(Button::Minus)] = none;
        l.b[static_cast<int>(Button::Plus)] = none;
        ok = none;
        break;
    case Stage::Results:
        l = list_labels(l);
        l.b[static_cast<int>(Button::Ok)].shrt = "Open";
        break;
    case Stage::Detail:
        l.b[static_cast<int>(Button::Minus)] = none;
        l.b[static_cast<int>(Button::Plus)] = none;
        ok = none;
        break;
    }
    return l;
}

}  // namespace k2
