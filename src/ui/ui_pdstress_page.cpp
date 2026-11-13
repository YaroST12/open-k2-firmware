// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/ui_pdstress_page.hpp"

#include <algorithm>
#include <bit>
#include <cstdlib>

#include "k2/core/scratch.hpp"
#include "k2/drivers/fusb302.hpp"
#include "k2/pdstress/pdstress_store_hw.hpp"
#include "k2/ui/home_model.hpp"
#include "k2/ui/trunc.hpp"
#include "k2/ui/ui.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2 {

namespace {

namespace ps = pdstress;
namespace dg = pdstress::digest;
using Stage = PdStressPage::Stage;

// ---- the copy (font 16 = 8 px, font 12 = 6 px a character) ---------------------------------------------------------------------------

// Setup rows and the Running name line: "Pn" + what the sequence does, by ps::Seq.
constexpr const char *kSeqNames[] = {
    "P1 Fixed ladder", "P2 Slam 5V <-> top", "P3 Same request", "P4 Caps re-announce", "P5 Fixed <-> PPS", "P6 PPS steps",
    "P7 AVS steps",    "P8 Current only",    "P9 EPR 28V",      "P10 Soft reset",      "P11 PPS below 5V", "P12 Mismatch bit",
};
static_assert(std::size(kSeqNames) == ps::kSeqCount, "one name per sequence");
static_assert(all_fit(kSeqNames, kSmallAdvPx, kit::kTailX - kit::kTextX), "a sequence name is wider than its row");

// The focused Setup row's note (font 12, the RowList's note box).
constexpr const char *kSeqNotes[] = {
    "Fixed levels up, then down", "5V to top level and back", "Same Request at 9V and top", "Get_Source_Cap every 500 ms",
    "Fixed to PPS at one voltage", "PPS steps 20 mV to 2 V",  "AVS steps 100 mV to 2 V",    "Half current and back",
    "EPR 28V and back. Opt-in",   "Soft_Reset again and again", "PPS requests under 5V",    "Mismatch bit, no real mismatch",
};
static_assert(std::size(kSeqNotes) == ps::kSeqCount, "one note per sequence");

constexpr const char *kRowNotes[] = {"Scales every test's repeats", "The wait after each change", "Review, then start"};
enum RowNote : uint8_t { kNoteCycles, kNotePace, kNoteDone };
static_assert(all_fit(kSeqNotes, kLabelAdv, kit::kNoteMaxPx) && all_fit(kRowNotes, kLabelAdv, kit::kNoteMaxPx),
              "a Setup note is wider than the note line");

// ps::Options::pace 0..2.
constexpr const char *kPaceNames[] = {"per sequence", "all Settled", "all Ready"};
static_assert(all_fit(kPaceNames, kSmallAdvPx, 112), "a pace name runs into the row text");

// The ResultGrid's label strip (font 12, right-aligned at x55, kResultLabelW - 8 = 52 px): at most 8 characters.
constexpr const char *kRowLabels[] = {"P1 Fixed", "P2 Slam", "P3 Same", "P4 Caps", "P5 F/PPS", "P6 PPS",
                                      "P7 AVS",   "P8 Amps", "P9 EPR",  "P10 Soft", "P11 <5V", "P12 Mism"};
static_assert(std::size(kRowLabels) == ps::kSeqCount, "one grid label per sequence");
static_assert(all_fit(kRowLabels, kLabelAdv, kit::kResultLabelW - 8), "a result row label is wider than the label strip");

// ps::Na, short, as the one cell of a row whose sequence the charger never offered what it needs.
constexpr const char *kNaShort[] = {"", "no 2 levels", "no PPS", "no AVS", "PPS from 5V+", "under 1A", "no EPR 28V", "no EPR cable",
                                    "K2 would drop", "not picked"};
static_assert(std::size(kNaShort) == static_cast<std::size_t>(ps::Na::NotSelected) + 1, "one short reason per Na");
// A lone grid cell spans x61..223 (162 px), its label in font 16 with a margin each side.
static_assert(all_fit(kNaShort, kSmallAdvPx, 150), "a reason is wider than its result cell");

// ps::Stop as the Results footer (font 16) and, short, in a saved result's note (font 12).
constexpr const char *kStopFoot[] = {
    "Not finished",          "Done",                  "Stopped",                 "Stopped: 15 min cap",       "Stopped: transition cap",
    "Stopped: 2 source resets", "Stopped: 2 K2 resets", "Stopped: VBUS too high", "Stopped: load on output",  "Stopped: needs a replug",
    "Stopped: step stuck",   "Stopped: PD contract lost", "Stopped: link inline", "Stopped: K2 reboots",
};
constexpr const char *kStopShort[] = {"not finished", "done",       "stopped",    "time cap",    "transition cap", "source resets", "K2 resets",
                                      "VBUS high",    "load",       "needs replug", "step stuck", "contract lost", "inline",        "K2 reboots"};
static_assert(std::size(kStopFoot) == static_cast<std::size_t>(ps::Stop::TooManyLosses) + 1, "one footer per Stop");
static_assert(std::size(kStopShort) == std::size(kStopFoot), "one short stop per Stop");
static_assert(all_fit(kStopFoot, kSmallAdvPx, kFooterTextPx), "a stop footer is wider than the footer line");
// "12 sequences, " (14) + the short stop: the note line.
static_assert(all_fit(kStopShort, kLabelAdv, kit::kNoteMaxPx - 14 * static_cast<int>(kLabelAdv)), "a saved result's note is too wide");

// The fixed footers and notes (font 16).
constexpr const char *kFooters[] = {
    "Keep the output empty",  "Running: keep output empty", "Resuming after K2 reboot", "Stopping: back to 5V",
    "Pick at least one test", "Result saved",               "Result discarded",         "Save failed: flash full",
    "Save failed",            "Result unreadable",          "Another run holds the port", "No PD source",
    "Load inline: unplug it", "No picked test applies",     "No buffer: charger test on", "Already running",
    "No power meter",
};
enum Foot : uint8_t {
    kReview, kRunning, kResuming, kStopping, kPickOne, kSaved, kDiscarded, kSaveFull, kSaveFailed, kUnreadable,
    kBusy, kNoCaps, kInline, kNoSequence, kNoStore, kAlready, kNoMeter,
};
static_assert(all_fit(kFooters, kSmallAdvPx, kFooterTextPx), "a PD Stress footer is wider than the footer line");

constexpr const char kHint[] = "Output must stay empty";   // the ActionCell's caution line (font 12)
static_assert(fits_px(kLabelAdv, kHint, 224 - 40 - 15), "the START hint is wider than the action cell");

// Review: a KvTable of the run that START makes.
constexpr kit::KvLayout kReviewKv{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, kit::kKvValueX, 18, Align::Left};
constexpr const char *kReviewKeys[] = {"Tests", "Cycles", "Pace", "Charger", "Output"};
constexpr int kReviewRows = static_cast<int>(std::size(kReviewKeys));
static_assert(all_fit(kReviewKeys, kSmallAdvPx, kit::kKvValueX - 6 - 4), "a Review key runs into the value column");
constexpr int16_t kReviewRuleY = kit::kBodyY + kReviewRows * kit::kRowH;   // 154
constexpr int16_t kActionY = kReviewRuleY + 2, kActionH = 64;                  // 156..219

// Running (level 1): seq n/N and the clock, the bar, the name, VBUS at 48 px, then the counts in two columns.
constexpr int16_t kSeqY = 26, kClockX = 170, kProgY = 44, kNameY = 50, kVbusLabelY = 74, kHeroX = 196, kHeroY = 70;
constexpr int16_t kCountRuleY = 120, kSplitX = 112;
constexpr kit::KvLayout kCountsL{kCountRuleY + 2, kit::kRowH, 6, &kLabelFont, nullptr, 76, 4, Align::Right};
constexpr kit::KvLayout kCountsR{kCountRuleY + 2, kit::kRowH, 116, &kLabelFont, nullptr, 186, 4, Align::Right};
constexpr const char *kCountKeysL[] = {"Transitions", "Hard resets", "Soft resets"};
constexpr const char *kCountKeysR[] = {"Rej / Wait", "Out of band", "Reboots"};
static_assert(all_fit(kCountKeysL, kLabelAdv, 76 - 6 - 2) && all_fit(kCountKeysR, kLabelAdv, 186 - 116 - 2),
              "a count key runs into its value");
constexpr int kCountRows = 3;
constexpr uint32_t kHeroMs = 250;   // digits at most 4 times a second
constexpr int kRunBudgetPx = PdStressPage::kRunBudgetPx;

// Results: the score band, the counters, the grid.
constexpr int16_t kScoreY = 26, kScoreX = 54, kScoreNoteX = 132, kBandRuleY = 52, kTallyY = 54, kTallyRuleY = 78, kGridY = 80;
// Three counters, each the number (font 16) then the word as a verdict cell (font 12): the same look as the grid's cells, the skip
// word struck. No lone glyph: a skip dash beside its number would read as a minus sign.
constexpr int16_t kTallyW = 74, kTallyNumX = 4, kTallyCellX = 24, kTallyCellW = 46, kTallyCellH = 20;
static_assert(3 * kTallyW <= 224, "the counters fit across");
constexpr const char *kTallyWords[] = {"clean", "issues", "skip"};
static_assert(all_fit(kTallyWords, kLabelAdv, kTallyCellW - 4), "a counter word is wider than its cell");

// Detail: two pages of seven counters.
constexpr kit::KvLayout kDetail{kit::kBodyY, kit::kRowH, 6, nullptr, nullptr, 160, 6, Align::Right};
constexpr const char *kDetailKeys[PdStressPage::kDetailPages][PdStressPage::kDetailRows] = {
    {"Transitions", "Accept late", "PS_RDY late", "PS_RDY out of band", "Wait", "Reject", "Caps resent"},
    {"VBUS out of band", "Soft resets", "Hard resets", "VBUS dropouts", "Contract lost", "No recovery", "Seq score"},
};
static_assert(all_fit(kDetailKeys[0], kSmallAdvPx, 160 - 6 - 4) && all_fit(kDetailKeys[1], kSmallAdvPx, 160 - 6 - 4),
              "a Detail key runs into the value");
// The event behind each Detail row (Count = not an event: transitions on page 1, the sequence's score on page 2).
constexpr ps::Event kDetailEvents[PdStressPage::kDetailPages][PdStressPage::kDetailRows] = {
    {ps::Event::Count, ps::Event::AcceptLate, ps::Event::PsRdyLate, ps::Event::PsRdyOutOfBand, ps::Event::WaitRx, ps::Event::RejectRx,
     ps::Event::CapsReannounce},
    {ps::Event::VbusBand, ps::Event::SoftResetRx, ps::Event::HardResetRx, ps::Event::VbusDropout, ps::Event::ContractLost,
     ps::Event::NoRecovery, ps::Event::Count},
};
constexpr const char *kVerdictWords[] = {"Clean", "Issues", "Not run", "Not offered", "Not picked"};   // dg::Verdict

constexpr uint32_t kNoteMs = 2500;

// A score in hundredths as the screen shows it: one decimal, "999+" past 999.9.
template <std::size_t N>
void put_score(StrBuf<N> &s, uint32_t cp) {
    if (cp >= 100000u) s.put("999+");
    else s.put_fixed(static_cast<int32_t>(cp * 10u), 1);
}

uint32_t capped(uint32_t v) { return v > 9999u ? 9999u : v; }

struct Budget {
    int cap;
    int spent = 0;
    bool admit(int px) {
        if (px <= cap - spent) { spent += px; return true; }
        return false;
    }
};

// A cached text cell under a paint budget: it repaints only when its text or colour changed and the budget admits its box.
void draw_cell(Field &f, uint32_t &sig, std::string_view text, uint16_t color, int box_px, Budget &b) {
    const uint32_t h = mix_u32(mix(mix_init(), text), color) | 1u;
    if (h == sig || !b.admit(box_px)) return;
    if ((sig >> 16) != (h >> 16) || sig == 0) f.set_colors(color, kBg);
    sig = h;
    f.draw(text);
}

std::size_t live_reader(void *ctx, std::size_t off, std::span<std::uint8_t> out) {
    return static_cast<const PdStressSession *>(ctx)->result_read(off, out);
}
std::size_t saved_reader(void *ctx, std::size_t off, std::span<std::uint8_t> out) {
    return ps::store::hw::read(*static_cast<const uint16_t *>(ctx), off, out);
}
void live_fill(void *ctx, std::size_t off, std::span<std::uint8_t> out) {
    const std::size_t n = static_cast<const PdStressSession *>(ctx)->result_read(off, out);
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n), out.end(), std::uint8_t{0});
}

// The source list in one line: the highest fixed level with its current, then the widest PPS range's top ("PD 20V5A PPS 21V").
template <std::size_t N>
void put_charger(StrBuf<N> &s, const pd::SourceCaps &caps) {
    const pd::Pdo *fixed = nullptr, *pps = nullptr;
    for (std::size_t i = 0; i < caps.num_pdos && i < caps.pdos.size(); i++) {
        const pd::Pdo &p = caps.pdos[i];
        if (p.type == pd::PdoType::Fixed && (!fixed || p.max_mv > fixed->max_mv)) fixed = &p;
        if (p.type == pd::PdoType::Pps && (!pps || p.max_mv > pps->max_mv)) pps = &p;
    }
    char t[12];
    StrBuf<24> line;
    if (fixed) {
        line.put("PD ").put({t, static_cast<std::size_t>(home::fmt_level(t, fixed->max_mv))}).put_char('V');
        line.put({t, static_cast<std::size_t>(home::fmt_level(t, fixed->max_ma))}).put_char('A');
    }
    if (pps) {
        if (fixed) line.put_char(' ');
        line.put("PPS ").put({t, static_cast<std::size_t>(home::fmt_level(t, pps->max_mv))}).put_char('V');
    }
    if (line.view().size() > kReviewKv.value_chars && fixed) {   // a 3.25 A level and a PPS range: the current goes
        line.clear();
        line.put("PD ").put({t, static_cast<std::size_t>(home::fmt_level(t, fixed->max_mv))}).put_char('V');
        if (pps) line.put(" PPS ").put({t, static_cast<std::size_t>(home::fmt_level(t, pps->max_mv))}).put_char('V');
    }
    s.put(line.view());
}

}  // namespace

PdStressPage::PdStressPage(UiContext &c) : PageBase(c, Page::Count, "PD Stress", Icon::Wave) {}

PdStressSession &PdStressPage::session() const { return ctx_.ui->pd_stress(); }

// ---- lifecycle ----------------------------------------------------------------------------------------------------------------------

// A run going on opens on Running, a result nobody saved or discarded yet on Results, anything else on Setup.
void PdStressPage::enter() {
    confirm_until_ = 0;
    note_.clear();
    note_until_ = 0;
    load_saved_list();
    const PdStressSession &ss = session();
    if (ss.active()) stage_ = Stage::Running;
    else if (ss.has_result() && ss.run_no() != ctx_.keep.pdstress.closed_run && load_live()) stage_ = Stage::Results;
    else stage_ = Stage::Setup;
    res_sel_ = 0;
    laid_ = bar_drawn_ = false;
}

void PdStressPage::draw_static() {
    bar();
    invalidate();
}

void PdStressPage::invalidate() {
    laid_ = false;
    for (Field &f : cell_) f.invalidate();
    action_.invalidate();
    prog_.invalidate();
    grid_.invalidate();
    ctx_.rows.invalidate();
    foot_.invalidate();
    bar_done_ = -1;
    clock_s_ = 0xFFFFFFFF;
    hero_ms_ = 0;
}

void PdStressPage::go(Stage s) {
    if (s != stage_) {
        stage_ = s;
        bar_drawn_ = false;
    }
    laid_ = false;
    confirm_until_ = 0;
    if (s == Stage::Results) res_sel_ = 0;
}

void PdStressPage::bar() {
    StrBuf<20> t;
    int step = 0;
    switch (stage_) {
    case Stage::Setup: t.put("Sequences"); step = 1; break;
    case Stage::Review: t.put("Review"); step = 2; break;
    case Stage::Running: t.put("PD Stress"); break;
    case Stage::Results:
        if (viewing_) t.put("Result #").put_u32(viewing_);
        else t.put("Result");
        break;
    case Stage::Detail: t.put(kRowLabels[static_cast<std::size_t>(detail_seq_)]); break;
    }
    kit::draw_top_bar(ctx_.content, t.view(), step, fusb302::self_powered() ? "SNK" : "PAS", true, 2);   // title_badge follows in update
    bar_valid_ = false;
    bar_drawn_ = true;
}

// ---- results ------------------------------------------------------------------------------------------------------------------------

// The saved results, newest first, with what a Setup row shows of each (one 32-byte header read per result).
void PdStressPage::load_saved_list() {
    std::array<uint16_t, ps::store::kMaxSaved> ids{};
    const std::size_t n = ps::store::hw::list(ids);
    nsaved_ = 0;
    for (std::size_t i = 0; i < n && i < ids.size(); i++) {
        std::array<uint8_t, 32> b{};
        if (ps::store::hw::read(ids[i], 0, b) != b.size() || b[0] != ps::result::kVersion) continue;
        Saved &s = saved_[nsaved_++];
        s.id = ids[i];
        s.stop = b[1];
        s.n = static_cast<uint8_t>(std::popcount(static_cast<unsigned>(dg::detail::u16(&b[8]) & ps::kAllSeqMask)));
        s.score_cp = dg::detail::u32(&b[24]);
    }
}

bool PdStressPage::load_live() {
    viewing_ = 0;
    res_sel_ = 0;
    return decode(false);
}

bool PdStressPage::load_saved(uint16_t id) {
    viewing_ = id;
    res_sel_ = 0;
    return decode(false);
}

// The full digest only in the scratch lease (neither reader takes a lease of its own: the live one copies from the session's
// record, the saved one reads littlefs); the page keeps a verdict, a reason and an event count per sequence.
bool PdStressPage::decode(bool detail) {
    scratch::Lease lease("pdstress");
    dg::Digest &d = scratch::make<dg::Digest>();
    if (viewing_ == 0) dg::load(d, live_reader, &session());
    else dg::load(d, saved_reader, &viewing_);
    res_valid_ = d.valid;
    res_stop_ = d.stop;
    res_score_cp_ = d.score_cp;
    const dg::Tally t = dg::tally(d);
    res_tally_ = {static_cast<uint8_t>(t.clean), static_cast<uint8_t>(t.issues), static_cast<uint8_t>(t.skip)};
    for (std::size_t i = 0; i < ps::kSeqCount; i++) {
        const auto s = static_cast<ps::Seq>(i);
        res_seq_[i] = {static_cast<uint8_t>(dg::verdict(d, s)), static_cast<uint8_t>(dg::na_of(d, s)),
                       static_cast<uint16_t>(dg::events(d.counts[i]))};
    }
    nrows_ = d.valid ? static_cast<uint8_t>(dg::rows(d, rows_)) : 0;
    if (detail) det_counts_ = d.counts[static_cast<std::size_t>(detail_seq_)];
    return d.valid;
}

int PdStressPage::result_rows() const { return nrows_ + (viewing_ == 0 ? 2 : 0); }

void PdStressPage::save_result() {
    uint16_t id = 0;
    const ps::store::Err e = ps::store::hw::put(live_fill, &session(), id);
    if (e != ps::store::Err::Ok) {
        say(kFooters[e == ps::store::Err::Full ? kSaveFull : kSaveFailed], kError);
        return;
    }
    close_result(true, id);
}

void PdStressPage::close_result(bool saved, uint16_t) {
    ctx_.keep.pdstress.closed_run = session().run_no();
    load_saved_list();
    go(Stage::Setup);
    say(kFooters[saved ? kSaved : kDiscarded], saved ? kOk : kDim);
}

// ---- start --------------------------------------------------------------------------------------------------------------------------

bool PdStressPage::start_run() {
    const auto &k = ctx_.keep.pdstress;
    ps::Options o;
    o.mask = k.mask;
    o.cycles_pct = kCyclesPct[std::min<std::size_t>(k.cycles, kCyclesPct.size() - 1)];
    o.pace = static_cast<uint8_t>(std::min<unsigned>(k.pace, 2));
    const ps::StartResult r = session().start(o, ctx_.now);
    switch (r) {
    case ps::StartResult::Ok: go(Stage::Running); return true;
    case ps::StartResult::Busy: say(kFooters[kBusy], kCaution); break;
    case ps::StartResult::NoCaps: say(kFooters[kNoCaps], kCaution); break;
    case ps::StartResult::Inline: say(kFooters[kInline], kCaution); break;
    case ps::StartResult::NoSequence: say(kFooters[kNoSequence], kCaution); break;
    case ps::StartResult::NoStore: say(kFooters[kNoStore], kCaution); break;
    case ps::StartResult::Running: say(kFooters[kAlready], kCaution); break;
    case ps::StartResult::NoMonitor: say(kFooters[kNoMeter], kCaution); break;
    }
    return false;
}

// ---- rows ---------------------------------------------------------------------------------------------------------------------------

void PdStressPage::setup_row(int i, bool, kit::RowSpec &out) {
    const auto &k = ctx_.keep.pdstress;
    if (i < kSeqRows) {
        out.marker = kit::Marker::Check;
        out.state = (k.mask >> i) & 1u;
        out.text = kSeqNames[i];
        out.note = kSeqNotes[i];
        return;
    }
    row_tail_.clear();
    if (i == kCyclesRow) {
        out.text = "Cycles";
        row_tail_.put_u32(kCyclesPct[std::min<std::size_t>(k.cycles, kCyclesPct.size() - 1)]).put(" %");
        out.tail = row_tail_.view();
        out.tail_color = kText;
        out.note = kRowNotes[kNoteCycles];
        return;
    }
    if (i == kPaceRow) {
        out.text = "Pace";
        out.tail = kPaceNames[std::min<unsigned>(k.pace, 2)];
        out.tail_color = kText;
        out.note = kRowNotes[kNotePace];
        return;
    }
    if (i == kDoneRow) {
        out.text = "Done";
        out.chevron = true;
        out.note = kRowNotes[kNoteDone];
        return;
    }
    const int s = i - kSavedRow0;
    if (s < 0 || s >= nsaved_) return;
    const Saved &sv = saved_[static_cast<std::size_t>(s)];
    out.marker = kit::Marker::Icon;
    out.icon = Icon::Report;
    row_text_.clear();
    row_text_.put("Result #").put_u32(sv.id);
    out.text = row_text_.view();
    row_tail_.put("score ");
    put_score(row_tail_, sv.score_cp);
    out.tail = row_tail_.view();
    row_note_.clear();
    row_note_.put_u32(sv.n).put(sv.n == 1 ? " sequence, " : " sequences, ").put(kStopShort[std::min<std::size_t>(sv.stop, std::size(kStopShort) - 1)]);
    out.note = row_note_.view();
}

void PdStressPage::result_row(int i, kit::ResultRowSpec &out) {
    out.n = 1;
    out.focus = i == res_sel_;
    if (i >= nrows_) {                       // the live result's actions
        out.label = "";
        out.section = i == nrows_;
        out.cells[0] = {kit::Verdict::None, i == nrows_ ? "Save result" : "Discard", {}};
        return;
    }
    const ps::Seq s = rows_[static_cast<std::size_t>(i)];
    out.label = kRowLabels[static_cast<std::size_t>(s)];
    cell_text_.clear();
    kit::ResultCell &c = out.cells[0];
    const SeqSummary &sm = res_seq_[static_cast<std::size_t>(s)];
    switch (static_cast<dg::Verdict>(sm.verdict)) {
    case dg::Verdict::Clean: c = {kit::Verdict::Pass, "clean", {}}; return;
    case dg::Verdict::Issues: {
        const uint32_t n = capped(sm.events);
        if (n == 0) {                        // given up after K2 reboots, no event counted
            c = {kit::Verdict::Partial, "K2 reboots", {}};
            return;
        }
        cell_text_.put_u32(n).put(n == 1 ? " issue" : " issues");
        c = {kit::Verdict::Partial, cell_text_.view(), {}};
        return;
    }
    case dg::Verdict::NotRun: c = {kit::Verdict::Skip, "not run", {}}; return;
    case dg::Verdict::Na:
    case dg::Verdict::NotSelected:
        c = {kit::Verdict::None, kNaShort[std::min<std::size_t>(sm.na, std::size(kNaShort) - 1)], {}};
        return;
    }
}

void PdStressPage::setup_row_fn(void *ctx, int index, bool focused, kit::RowSpec &out) {
    static_cast<PdStressPage *>(ctx)->setup_row(index, focused, out);
}
void PdStressPage::result_row_fn(void *ctx, int index, kit::ResultRowSpec &out) { static_cast<PdStressPage *>(ctx)->result_row(index, out); }

// ---- footer -------------------------------------------------------------------------------------------------------------------------

void PdStressPage::say(std::string_view text, uint16_t color) {
    note_.clear();
    note_.put(text);
    note_color_ = color;
    note_until_ = ctx_.now + kNoteMs;
    if (note_until_ == 0) note_until_ = 1;
}

void PdStressPage::compose_footer(uint32_t now_ms) {
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
    switch (stage_) {
    case Stage::Setup:
        foot_text_.put_u32(static_cast<uint32_t>(std::popcount(static_cast<unsigned>(ctx_.keep.pdstress.mask & ps::kAllSeqMask))))
            .put(" of ").put_u32(static_cast<uint32_t>(ps::kSeqCount)).put(" picked");
        return;
    case Stage::Review:
        foot_tone_ = FooterTone::Caution;
        foot_text_.put(kFooters[kReview]);
        return;
    case Stage::Running: {
        const PdStressSession &ss = session();
        const char *f = ss.footer();
        if (ss.restoring()) foot_text_.put(kFooters[kResuming]);
        else if (f && std::string_view(f) == kPdStressFooters[2]) foot_text_.put(kFooters[kStopping]);
        else foot_text_.put(kFooters[kRunning]);
        return;
    }
    case Stage::Results: {
        const auto st = static_cast<std::size_t>(res_stop_);
        const std::size_t i = std::min(st, std::size(kStopFoot) - 1);
        foot_tone_ = res_stop_ == ps::Stop::Done ? FooterTone::Ok
                     : res_stop_ == ps::Stop::Cancelled || res_stop_ == ps::Stop::None ? FooterTone::Info : FooterTone::Caution;
        foot_text_.put(res_valid_ ? kStopFoot[i] : kFooters[kUnreadable]);
        return;
    }
    case Stage::Detail: {
        const dg::Verdict v = seq_verdict(detail_seq_);
        foot_tone_ = v == dg::Verdict::Clean ? FooterTone::Ok : v == dg::Verdict::Issues ? FooterTone::Caution : FooterTone::Info;
        foot_text_.put(kVerdictWords[static_cast<std::size_t>(v)]).put(". Page ").put_u32(static_cast<uint32_t>(detail_page_ + 1))
            .put(" of ").put_u32(kDetailPages);
        return;
    }
    }
}

// ---- paint --------------------------------------------------------------------------------------------------------------------------

void PdStressPage::paint_setup() {
    const Surface &c = ctx_.content;
    if (!laid_) {
        if (!ctx_.blank) kit::RowList::clear_body_around(c);
        ctx_.rows.init(c, kit::kBodyY, setup_row_fn, this);
        ctx_.rows.invalidate();
        laid_ = true;
    }
    auto &k = ctx_.keep.pdstress;
    if (k.sel >= setup_count()) k.sel = static_cast<uint8_t>(setup_count() - 1);
    ctx_.rows.set_count(setup_count());
    ctx_.rows.select(k.sel);
    ctx_.rows.draw(ctx_.now, ctx_.budget);
}

void PdStressPage::paint_review(uint32_t now_ms) {
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
    const auto &k = ctx_.keep.pdstress;
    Budget b{1 << 30};
    StrBuf<24> t;
    const unsigned n = static_cast<unsigned>(std::popcount(static_cast<unsigned>(k.mask & ps::kAllSeqMask)));
    t.put_u32(n).put(n == 1 ? " sequence" : " sequences");
    draw_cell(v[0], sig_[0], t.view(), kText, 0, b);
    t.clear();
    t.put_u32(kCyclesPct[std::min<std::size_t>(k.cycles, kCyclesPct.size() - 1)]).put(" %");
    draw_cell(v[1], sig_[1], t.view(), kText, 0, b);
    draw_cell(v[2], sig_[2], kPaceNames[std::min<unsigned>(k.pace, 2)], kText, 0, b);
    t.clear();
    const pd::SourceCaps &caps = fusb302::caps();
    if (caps.valid) put_charger(t, caps);
    if (t.view().empty()) draw_cell(v[3], sig_[3], kFooters[kNoCaps], kDim, 0, b);
    else draw_cell(v[3], sig_[3], t.view(), kText, 0, b);
    t.clear();
    const int32_t ma = ctx_.pm_online ? std::abs(ctx_.pm.current_ma) : 0;
    if (ctx_.pm_online) t.put_fixed(ma, 2).put_char('A');
    else t.put(kFooters[kNoMeter]);
    draw_cell(v[4], sig_[4], t.view(), !ctx_.pm_online ? kDim : ma > ps::kLoadAbortMa ? kCaution : kAmp, 0, b);

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

void PdStressPage::paint_running(uint32_t now_ms) {
    const Surface &c = ctx_.content;
    Field *v = cell_.data();   // 0 seq, 1 clock, 2 name, 3..8 the counts
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        v[0].init(c, kSmall, 6, kSeqY, 12, Align::Left, kText, kBg);
        v[1].init(c, kSmall, kClockX, kSeqY, 6, Align::Right, kText, kBg);
        prog_.init(c, 6, kProgY, static_cast<int16_t>(c.w - 12));
        v[2].init(c, kSmall, 6, kNameY, 26, Align::Left, kText, kBg);
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
        bar_done_ = -1;
        laid_ = true;
    }
    const PdStressSession &ss = session();
    const ps::Engine *e = ss.engine();
    Budget b{kRunBudgetPx};
    const int cell16 = kSmall.adv * kSmall.height;

    // VBUS first, at 4 Hz: the walk-by value.
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
    // The clock, once a second.
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
    // The progress bar between steps.
    const int total = e && e->seq_total() ? e->seq_total() * 1000 : 1;
    int done = 0;
    if (e && e->seq_total()) {
        const int len = e->step_total();
        done = e->seq_index() * 1000 + (len > 0 ? std::min<int>(e->step_index(), len) * 1000 / len : 0);
        done = std::min(done, total);
    }
    const int pc = prog_.cost(done, total);
    if (pc == 0 || b.admit(pc)) prog_.set(done, total);
    // The sequence line and its name.
    StrBuf<16> t;
    if (e && e->seq_total()) {
        t.put("seq ").put_u32(std::min<uint32_t>(e->seq_index() + 1u, e->seq_total())).put_char('/').put_u32(e->seq_total());
    } else {
        t.put("seq");
    }
    draw_cell(v[0], sig_[0], t.view(), kText, 12 * cell16, b);
    draw_cell(v[2], sig_[2], e && e->seq_total() ? kSeqNames[static_cast<std::size_t>(e->seq())] : "", kText, 26 * cell16, b);
    // The counts.
    for (int i = 0; i < kCountCells; i++) {
        const uint32_t n = count_value(i);
        StrBuf<8> s;
        s.put_u32(capped(n));
        const uint16_t col = i == 0 || n == 0 ? kText : kCaution;
        draw_cell(v[3 + i], sig_[static_cast<std::size_t>(3 + i)], s.view(), col, 4 * cell16, b);
    }
}

uint32_t PdStressPage::count_value(int i) const {
    const ps::Engine *e = session().engine();
    if (!e) return 0;
    const ps::Counts tot = e->total();
    switch (i) {
    case 0: return e->transitions();
    case 1: return tot.get(ps::Event::HardResetRx);
    case 2: return tot.get(ps::Event::SoftResetRx);
    case 3: return static_cast<uint32_t>(tot.get(ps::Event::RejectRx)) + tot.get(ps::Event::WaitRx);
    case 4: return static_cast<uint32_t>(tot.get(ps::Event::PsRdyOutOfBand)) + tot.get(ps::Event::VbusBand);
    case 5: return e->reboots();
    default: return 0;
    }
}

uint32_t PdStressPage::detail_value(int row) const {
    if (row < 0 || row >= kDetailRows) return 0;
    const int pg = std::clamp(detail_page_, 0, kDetailPages - 1);
    const ps::Event ev = kDetailEvents[pg][row];
    if (ev != ps::Event::Count) return det_counts_.get(ev);
    return pg == 0 ? det_counts_.transitions : ps::score_cp(det_counts_);
}

void PdStressPage::paint_results() {
    const Surface &c = ctx_.content;
    Field &score = cell_[0];
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        gfx_text(c, kSmall, 6, 30, "Score", kDim, kBg);
        score.init(c, kMid, kScoreX, kScoreY, 6, Align::Right, kText, kBg);
        StrBuf<8> s;
        put_score(s, res_score_cp_);
        score.draw(res_valid_ ? s.view() : std::string_view{"--"});
        gfx_text(c, kLabelFont, kScoreNoteX, kScoreY, "provisional,", kDim, kBg);
        gfx_text(c, kLabelFont, kScoreNoteX, static_cast<int16_t>(kScoreY + 13), "0 = ideal", kDim, kBg);
        gfx_fill(c, 0, kBandRuleY, c.w, 2, kRule);
        const unsigned nums[3] = {res_tally_[0], res_tally_[1], res_tally_[2]};
        constexpr kit::Verdict kLook[3] = {kit::Verdict::Pass, kit::Verdict::Partial, kit::Verdict::Skip};
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
    grid_.set_count(result_rows());
    grid_.show(res_sel_);
    grid_.draw();
}

void PdStressPage::paint_detail() {
    const Surface &c = ctx_.content;
    Field *v = cell_.data();
    const int pg = std::clamp(detail_page_, 0, kDetailPages - 1);
    if (!laid_) {
        if (!ctx_.blank) gfx_fill(c, 0, kit::kBelowBar, c.w, static_cast<int16_t>(kFooterY - kit::kBelowBar), kBg);
        kit::kv_static(c, kDetail, kDetailKeys[pg], kDetailRows);
        kit::kv_cells(c, kDetail, v, kDetailRows);
        for (uint32_t &s : sig_) s = 0;
        laid_ = true;
    }
    Budget b{1 << 30};
    for (int i = 0; i < kDetailRows; i++) {
        StrBuf<8> s;
        const uint32_t n = detail_value(i);
        const bool event = kDetailEvents[pg][i] != ps::Event::Count;
        if (event || pg == 0) s.put_u32(capped(n));
        else put_score(s, n);                // the sequence's score
        draw_cell(v[i], sig_[static_cast<std::size_t>(i)], s.view(), event && n ? kCaution : kText, 0, b);
    }
}

void PdStressPage::update(uint32_t now_ms) {
    // The run decides Running: a run started elsewhere (the remote, the shell) shows here, and its end opens the result.
    const PdStressSession &ss = session();
    if (ss.active() && stage_ != Stage::Running) {
        go(Stage::Running);
    } else if (stage_ == Stage::Running && !ss.active()) {
        load_live();
        go(Stage::Results);
    }
    if (confirm_until_ != 0 && static_cast<int32_t>(now_ms - confirm_until_) >= 0) confirm_until_ = 0;   // the window ran out
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

bool PdStressPage::on_setup(Event e) {
    auto &k = ctx_.keep.pdstress;
    switch (e.button) {
    case Button::Back: return false;   // the Ui pops the page
    case Button::Minus:
        if (k.sel > 0) k.sel--;
        return true;
    case Button::Plus:
        if (k.sel + 1 < setup_count()) k.sel++;
        return true;
    case Button::Ok:
        if (k.sel < kSeqRows) {
            k.mask = static_cast<uint16_t>(k.mask ^ (1u << k.sel));
        } else if (k.sel == kCyclesRow) {
            k.cycles = static_cast<uint8_t>((k.cycles + 1u) % kCyclesPct.size());
        } else if (k.sel == kPaceRow) {
            k.pace = static_cast<uint8_t>((k.pace + 1u) % std::size(kPaceNames));
        } else if (k.sel == kDoneRow) {
            if ((k.mask & ps::kAllSeqMask) == 0) say(kFooters[kPickOne], kCaution);
            else go(Stage::Review);
        } else {
            const int s = k.sel - kSavedRow0;
            if (s >= 0 && s < nsaved_) {
                if (load_saved(saved_[static_cast<std::size_t>(s)].id)) go(Stage::Results);
                else say(kFooters[kUnreadable], kError);
            }
        }
        return true;
    }
    return false;
}

bool PdStressPage::on_review(Event e) {
    switch (e.button) {
    case Button::Back:
        go(Stage::Setup);
        return true;
    case Button::Ok:
        if (confirm_until_ != 0 && static_cast<int32_t>(ctx_.now - confirm_until_) < 0) {
            confirm_until_ = 0;
            start_run();
        } else if (const char *busy = ctx_.ui->busy_reason()) {
            say(busy, kCaution);                // refuse before the arm: nothing to confirm
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

bool PdStressPage::on_results(Event e) {
    switch (e.button) {
    case Button::Back:
        go(Stage::Setup);
        return true;
    case Button::Minus:
        if (res_sel_ > 0) res_sel_--;
        return true;
    case Button::Plus:
        if (res_sel_ + 1 < result_rows()) res_sel_++;
        return true;
    case Button::Ok:
        if (res_sel_ < nrows_) {
            detail_seq_ = rows_[static_cast<std::size_t>(res_sel_)];
            detail_page_ = 0;
            const int keep = res_sel_;
            if (!decode(true)) {             // the saved file went away under the list
                say(kFooters[kUnreadable], kError);
                return true;
            }
            go(Stage::Detail);
            res_sel_ = keep;
        } else if (viewing_ == 0 && res_sel_ == nrows_) {
            save_result();
        } else if (viewing_ == 0) {
            close_result(false, 0);
        }
        return true;
    }
    return false;
}

bool PdStressPage::on_detail(Event e) {
    switch (e.button) {
    case Button::Back: {
        const int keep = res_sel_;
        go(Stage::Results);
        res_sel_ = keep;
        return true;
    }
    case Button::Minus:
    case Button::Plus: {
        const int pg = std::clamp(detail_page_ + (e.button == Button::Plus ? 1 : -1), 0, kDetailPages - 1);
        if (pg != detail_page_) {
            detail_page_ = pg;
            laid_ = false;
        }
        return true;
    }
    case Button::Ok: return true;
    }
    return false;
}

bool PdStressPage::on_event(Event e) {
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
    case Stage::Detail: used = on_detail(e); break;
    }
    if (used) update(ctx_.now);
    return used;
}

PanelLabels PdStressPage::labels() const {
    PanelLabels l = base_labels();
    auto &ok = l.b[static_cast<int>(Button::Ok)];
    const auto none = SlotLabel{"", "", "", false};
    switch (stage_) {
    case Stage::Setup: {
        l = list_labels(l);
        const int s = ctx_.keep.pdstress.sel;
        l.b[static_cast<int>(Button::Ok)].shrt = s < kSeqRows ? "Toggle" : s == kCyclesRow || s == kPaceRow ? "Cycle" : s == kDoneRow ? "Next" : "Open";
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
        l.b[static_cast<int>(Button::Ok)].shrt = res_sel_ < nrows_ ? "Open" : "Select";
        break;
    case Stage::Detail:
        l = list_labels(l);
        l.b[static_cast<int>(Button::Ok)] = none;
        break;
    }
    return l;
}

}  // namespace k2
