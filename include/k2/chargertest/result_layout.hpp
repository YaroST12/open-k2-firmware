// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test Results screen, pure layout: the run's steps become one row per protocol (a label plus
// chips), then the rows wrap into display lines. No drawing here; host-tested in tests/test_result_layout.cpp.
//   PD:        ONE block, labelled "PD" on its first line only (later lines have no label). In order: a chip per offered fixed
//              level ("5V 3A": the advertised current, from the PDO in the run evidence; PD2.0, PD3.x and EPR levels together,
//              ascending), then a chip per advertised PPS range ("PPS 3.3-11V" over its "3A"), then one per AVS range (SPR and
//              EPR, "AVS 15-28V"), coloured by the worst of the range's steps and its deep sweep; a small "S" beside the volts
//              says a sweep ran on that APDO (Token::sweep). Each range chip has a line to itself (Token::kind says PPS / AVS;
//              the word is drawn, never stored in the text). A kind the source never offered has no chip: no "None" rows.
//              Skipped EPR levels (cable, refused, over the monitor cap) have no chip in the Results grid (OemNotes::fold): the
//              reasons live on the block's Steps page. The sweep has no chip of its own.
//   Legacy:    one chip per distinct target voltage ("9V"), so QC3 up and down steps collapse; SCP adds its limit ("10V 2.25A").
//   Xiaomi:    one row "Xiaomi" for the unit (the hunt steps): a chip per mode the chain unlocked, by the watts of
//              its new level ("55W"), no auth chip; with none, one "auth" chip: passed (finished, nothing went up) or skipped
//              (no SVID, no answer, stalled: the reason stays on the Steps page).
//   The words are k2/chargertest/step_label.hpp's, the same as the Running / Steps rows and the report card.
//   Protocols with nothing to show (not advertised, or no answer at 5 V) fold into one "No reply" row of words;
//   protocols that were only skipped (PD active, over the cap, ...) fold into one "Skipped" row.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include "k2/chargertest/charger_test.hpp"
#include "k2/chargertest/step_label.hpp"
#include "k2/chargertest/sweep.hpp"

namespace k2::result_layout {

// Partial = some steps of the range passed, some did not; Skip = nothing ran (every step skipped); Noted = only listed: a Light run
// did not ask for it (a legacy level, a UFCS mode, an SCP level), so nothing was measured there and nothing failed. A PD offer the
// source advertises (advertised()) wears the pass look instead: the source has it, and a Light run only says so.
enum class Tone : uint8_t { Pass, Fail, Partial, Pending, Dim, Caution, Skip, Noted };
// The shape that says the verdict without the colour: check (pass), cross (fail), tilde (mixed), dash (skip), dot (pending).
// '\x01' / '\x02' are the in-text check / cross markers of k2::draw_marked_text (include/k2/ui/text.hpp); a plain token has none (' ').
char mark(Tone);
// The tone rules, one table for every chip (fixed, range and legacy; docs/UI_DESIGN.md "Charger test"). A chip covers one or
// more steps: a fixed level is one, a PPS / AVS range is all the steps sampling it, a legacy level is its up and down steps.
//   a step still to run                  -> Pending   dim
//   nothing sampled (every step skipped) -> Skip      grey, the glued tag says why
//   only listed (Light: not asked)       -> Noted     framed, neither green nor struck; beside a measured or
//                                           skipped step it adds nothing (5 V passed + 9 V listed = Pass)
//   a PD offer the source advertises     -> Pass      green, counted as `adv` (a pass for the tone only, never in a count's words)
//   every step passed                    -> Pass      green
//   some passed, or the source capped    -> Partial   amber ("~")
//   none passed                          -> Fail      red
// A Capped step (ct::Reason::Capped: the source plateaued below the target) counts as `capped`, not as `fail`.
// A caution step (a Pass that carries ct::Reason::Folded: the output folded back inside a sweep, ct::Reason::StepResolution: a step
// walk the source did not follow at its quantum, or ct::Reason::ProgCheck: a transition that broke a spec limit) counts as `capped` too, never as
// `pass` and never as `fail`: the source delivered every level, and one of them is worth a second look (amber, "CAUTION").
// A Skip step whose reason is Advertised or LightLevel (a Light run's listing) counts as `noted`, not as `skip`; an advertised PD
// offer (advertised()) is also counted in `adv`, which roll_up reads as a pass.
struct Tally { unsigned pass = 0, fail = 0, capped = 0, skip = 0, noted = 0, pending = 0, adv = 0; };
Tone roll_up(const Tally&);
// A Light run's listing: a Skip whose reason is Advertised or LightLevel (counted as `noted`, drawn as kit::Verdict::Noted).
bool noted(const charger_test::Step& s);
// A VOOC family row skipped because no wake clocked an opener (Skip, Request, fc::Fail::NoAnswer): folds as "No reply", not "Skipped".
bool is_no_answer(const charger_test::Step& s);
// A PD offer a Light run lists without asking for it (a fixed PDO, a PPS / AVS APDO, an EPR level the source listed): the source
// advertises it, so it is drawn with the pass look (kit::Verdict::Pass) while its band word stays LISTED. Not UFCS, SCP or a legacy level.
bool advertised(const charger_test::Step& s);
// A voltage fold the deep sweep found: a Pass that carries Reason::Folded (the sweep row, and the fixed rows of the same APDO
// that sit on the folded side), a step-walk miss (a Pass that carries Reason::StepResolution; an older journal holds it on a Fail,
// which stays a fail) or a transition flag (Reason::ProgCheck on a level row). A caution, drawn kit::Verdict::Partial.
bool caution(const charger_test::Step& s);
// The deep sweep's pass window: a passing sweep whose worst recorded |error| stays within it is a plain pass (green), beyond it a
// caution (amber). A passing sweep is never red: red is a failed step, so the chip, the header, the Steps footer and the report
// card agree (user decision 2026-11-16, the rule of the fold caution).
inline constexpr unsigned kSweepPassMv = 100;
// The worst recorded |error| over both parts of the APDO's sweep, mV (0 when no part closed): what the chip is graded by.
unsigned sweep_worst_mv(const ctsweep::View&, uint8_t alg, uint8_t pos);
// A passing sweep step (no fold caution of its own) whose APDO's worst error passes kSweepPassMv: a caution by deviation. The two
// parts of a split PPS sweep share one APDO summary, so both read the same.
bool deviates(const charger_test::Step& s, const ctsweep::View&);
// The APDO's non-fold caution row: the step-walk miss (Pass + StepResolution) first, else the first level row that carries a transition
// flag (Pass + ProgCheck); null when there is none. Its note is charger_test::caution_text.
const charger_test::Step* step_caution(std::span<const charger_test::Step>, charger_test::Algorithm, uint8_t pos);
// A caution of either kind: the fold (Reason::Folded on the step) or a tracking deviation (the sweep view). Counts, notes and tones
// read this; the one-argument form above sees only the fold.
bool caution(const charger_test::Step& s, const ctsweep::View&);
// verdict_word with the view: a deviating sweep reads CAUTION as well.
const char* verdict_word(const charger_test::Step& s, const ctsweep::View&);
// The word of a step's verdict band (Step detail): PASS, CAUTION (a fold), FAIL, CAPPED (a Capped fail), SKIP, LISTED (Advertised), NOT RUN
// (LightLevel), RUNNING, PENDING. At most 7 characters: the band's font-24 word ends before the step's name.
const char* verdict_word(const charger_test::Step& s);

// The Running view's progress bar: rows the run executes. done = Pass and Fail rows (a caution is a pass that ran); total = every row
// that is not a Skip, so a plan-time skip (a Light run's advertised PD rows, a protocol the charger never offered) is never work, a fresh
// run reads 0 and a runtime skip of a pending row leaves the total instead of counting as done. total 0 draws an empty bar.
struct Progress { unsigned done = 0, total = 0; };
Progress run_progress(std::span<const charger_test::Step> steps);

struct Token {
    char text[16] = {};   // the longest chip, "3.3-21V 5.25A", is 13
    Tone tone = Tone::Dim;
    bool chip = false;   // boxed (verdict chip) or plain text (range / collapsed protocol name)
    bool glue = false;   // stays on the line of the previous token
    uint8_t kind = 0;    // a PD range chip: kKindPps / kKindSprAvs / kKindEprAvs (0: any other token). The grid draws kind_word(kind) +
                         // ' ' before the volts, so it is in the cell width but not in `text`
    uint16_t mv = 0;     // the level a fixed or legacy chip stands for (0: a word or a probe); a range chip (kind != 0): its PDO slot.
                         // The Running grid finds the chip of the step in flight by it (a legacy chip merges the up and down steps)
    bool sweep = false;  // a range chip whose APDO ran (or runs) a deep sweep: the grid draws a small dim "S" after the volts
};
static_assert(sizeof(Token) == 24, "RowTokens and the Layout are sized by it");
inline constexpr uint8_t kKindPps = 1, kKindSprAvs = 2, kKindEprAvs = 3;
uint8_t kind_of(charger_test::Algorithm);   // the chip kind of a range algorithm; 0 for any other
charger_test::Algorithm kind_algorithm(uint8_t kind);   // back: Count for 0
const char* kind_word(uint8_t kind);   // "PPS" for kKindPps, "AVS" for both AVS kinds, "" for 0
// A bit per APDO position (1..15) that has a range chip on the Results grid (build() with the default fold): the spec section's split.
uint16_t range_positions(std::span<const charger_test::Step>);
struct Row {
    char label[9] = {};
    Tone label_tone = Tone::Dim;
    charger_test::Algorithm algorithm = charger_test::Algorithm::Count;   // Count: a collapsed row or a header
    bool header = false;            // section header row: no tokens, not openable
    bool probe = false;             // the Xiaomi row (algorithm Count): every hunt step (the chain row and its modes)
    bool pd = false;                // the PD block (algorithm = its lead PD algorithm): owns every PD step
    uint8_t pos = 0;                // Levels: the family's first level index, else 0
    uint16_t mv = 0;                // Levels: the claim bits, else 0
    bool levels = false;            // an OEM level row (CompactKind::Levels): owns every step of the level family's units
    uint8_t first = 0, count = 0;   // tokens
};
inline constexpr size_t kMaxTokens = 80, kMaxRows = 24, kMaxLines = 48;
struct Layout {
    std::array<Token, kMaxTokens> tokens{};
    std::array<Row, kMaxRows> rows{};
    size_t token_count = 0, row_count = 0;
    uint8_t cuts = 0;   // texts build() had to shorten to fit a Row label (8) or a Token (15): always 0, a gate failure otherwise
};
struct Line {
    uint8_t row = 0, first = 0, count = 0;   // tokens first .. first+count of the layout
    bool head = false;                       // first line of its row: the label is drawn here
};
// glyph advance (the 12 px panel font, 6 px), chip padding, gap, width after the label, the mark cell every chip starts with;
// max_groups = most chips (a chip and the tags glued to it count once) on one line, 0 = no limit (the verdict grid: 5 cells).
// cells = the verdict grid: a line is `groups` EQUAL cells as wide as the widest volt text (a chip's text before its space, a
// word whole; the glued tags are not shown there) plus 2 * pad, one `gap` apart, in `avail` px in all.
// sweep = the room a chip with a sweep keeps right of its text: the gap + the "S" glyph (kit::kSweepMarkW, checked in ui_charger_test).
struct Metrics { int adv = 6, pad = 2, gap = 2, avail = 158, mark = 11, max_groups = 0; bool cells = false; int sweep = 12; };
// The Results / Running verdict grid wraps the layout into lines of at most 5 equal cells (8 px glyphs, 163 px beside the label
// strip, 1 px gaps). Shared with the gate's sweep of every passive label (tests/test_ui_no_truncation.cpp).
inline constexpr Metrics kGridMetrics{.adv = 8, .pad = 2, .gap = 1, .avail = 153, .max_groups = 5, .cells = true, .sweep = 12};

// Results-tab sections, in the default run order (docs/CHARGER_TEST_FLOW.md): the OEM vendor modes by company
// (Huawei / Honor, Samsung, OPPO / OnePlus / vivo), the identity facts of a PD source (QC4+/QC5, Lenovo, HP), USB PD (CC) with
// Xiaomi (the SVID 0x2717 unit, PD-based) where the planner runs it, then QC / UFCS. Row labels hold 8 characters, so the
// headers are the short forms of the wizard's family names.
enum class Section : uint8_t { Huawei = 0, Samsung, Oppo, Xiaomi, OemPd, UsbPd, Qc, Count };
const char* section_name(Section);
Section section_of(charger_test::Algorithm);
// Optional facts about the source (labels from the source identity, never behaviour):
// qc_label/qc_why = the passive QC4+/QC5 label, vendor = VID-driven OEM name (Lenovo/HP/...), mi_auth =
// the SVID 0x2717 auth completed this session (a "Mi Turbo auth ok" fact when the run has no Xiaomi row).
struct OemNotes {
    const char* qc_label = nullptr;
    const char* qc_why = nullptr;
    const char* vendor = nullptr;
    bool mi_auth = false;
    bool headers = true;   // section separator rows; the dense Results grid leaves them out (a line is a unit there)
    // A legacy unit that never answered at 5 V, or was only skipped, folds into the trailing "No reply" / "Skipped" words row.
    // The Running grid keeps such a unit's row where the plan put it (fold = false): a row that jumps to the end when its unit
    // fails would take the finished rows from above the running one and shuffle the window mid-run.
    bool fold = true;
    bool oppo_svid = false;   // the source listed OPPO's SVID: a passive "OPPO SVID 0x22D9 (discovery only)" row, nothing sent
    const char* scp_cap = nullptr;   // what the adapter's SCP readout reported, the words after "SCP cap " of report_card::scp_note ("20V 6A")
    // The SuperVOOC ceiling stage's result, the words of svooc_ceiling::format_cap ("<=11.1V, 10.9A claimed", "refused"): the highest
    // current limit the K2 asked for under which the charger kept the session going. Never a drawn current. nullptr = no stage ran.
    const char* svooc_cap = nullptr;
    // The VOOC family's ident command the detection row heard (svooc_ceiling::format_ident: "0xAC"): a passive "VOOC id" row at
    // every level. One ident proves the family, not the variant. nullptr = none heard.
    const char* vooc_ident = nullptr;
    unsigned rebalanced = 0;   // how often the source re-announced its list mid-run (budget-reannounce-mid-run): a passive "SOURCE rebalanced Nx" row
    // OEM levels by their watts (oem_watts.hpp): the VOOC family's units (VOOC, SVOOC, SVOOC65; one shared detection) read as
    // level rows, "VOOC" [detection, 20W] and "SVOOC" [50W 65W 100W 120W], each level chip claimed (Noted), not reached (Dim) or
    // not tested (Skip) by vooc_claims (oem_watts::claims of the ceiling stage; 0 = every level not tested). A family that never
    // answered folds as one word. Off (the Running grid): the plan's unit rows.
    bool vooc_watts = false;
    uint16_t vooc_claims = 0;
};
// Vendor-VID label for the OEM section; nullptr when the VID has no PD-derived OEM protocol to name.
const char* vendor_oem_name(uint16_t vid);

// The rows come from the steps alone: they are the plan, in the order the runner executes them, so a row stands where the plan first
// runs one of its steps. A row is a protocol's steps (the PD block: every PD algorithm, Row::pd), or (the Xiaomi row, Row::probe)
// the hunt steps, wherever the planner put them. A PPS / AVS APDO's deep sweep steps belong to its range chip: the chip is the
// worst of the range steps and the sweep (the sweep's tracking error and fold come from `sweep`, the runner's record) and wears the
// "S" mark. The Running grid, the Results page and the Steps page read the same step list, so none keeps an order of its own.
void build(Layout&, std::span<const charger_test::Step>, const OemNotes& = {}, const ctsweep::View& sweep = {});
// Does step `s` belong to layout row `r`? The Xiaomi row owns the hunt steps; the PD block every PD step (fixed, PPS, AVS, sweeps,
// the revision rows of a Custom run); any other protocol row the protocol's steps.
bool row_has(const Row& r, const charger_test::Step& s);
// Appends a fact row of plain words after the protocol rows (a collapsed row: its OK opens the whole step list).
// Dropped when the layout is full.
void add_note(Layout&, const char* label, Tone, std::initializer_list<const char*> words);
// The Results summary card: the best passing mode of each protocol family, as words with volts and amps.
//   PD (fixed and PPS: amps = the advertised PDO current, or the requested one on the full-current PPS row; AVS: volts only,
//   ranked by the offer's limit), Mi (a mode the Xiaomi chain unlocked,
//   amps = its new current), QC / AFC / Huawei / OPPO / UFCS (D+/D- modes: volts only; SCP and UFCS read the claimed watts of
//   the level, oem_watts::step_mw: "SCP 22.5W", "UFCS 60W", never ranked as watts: the K2 drew none).
// Ranked by watts where both are known, then the volts-only families by voltage. A handshake without a voltage proof
// reads "VOOC ok" / "VOOC seen" (the VOOC family's word, never a unit's version). `mw` is 0 when no current is known. Returns the lines written (at most out.size()).
struct SummaryLine { char text[20] = {}; unsigned mw = 0; unsigned mv = 0; };
size_t summarize(std::span<const charger_test::Step>, std::span<SummaryLine> out);
// What a PPS / AVS range delivered, from its steps. The chip keeps the advertised range; this says how far the source got:
// lo = the lowest level that passed, hi = the highest reached (a passed target, or the measured ceiling of a Capped step),
// adv = the advertised maximum. `pos` = the PDO slot, or -1 for the row's most telling range: the first one that is not
// all-pass, else the last. An algorithm with no range steps reads adv_mv 0 and Tone::Skip. needs_load: some level was
// skipped for Reason::NeedsLoad and none failed plainly (pattern pps-off-at-no-load). floored / floor_mv: a low level
// failed because the output held a steady floor above it (Reason::Floored).
struct Reach {
    uint16_t lo_mv = 0, hi_mv = 0, adv_mv = 0;
    bool capped = false;
    bool folded = false;     // some level passed on the folded side of a fold the sweep found (Reason::Folded)
    uint16_t fold_mv = 0;    // the level the lowest such row measured
    bool floored = false;
    uint16_t floor_mv = 0;   // the level a Floored low row measured (the source's floor)
    bool needs_load = false;
    bool listed = false;     // the range was only advertised (a Light run): nothing measured, the source has it
    Tone tone = Tone::Skip;
};
Reach reach(std::span<const charger_test::Step>, charger_test::Algorithm, int pos = -1);
// The footer words after "<label>: " (at most 18 characters, so a 9-character label still fits 27): "caps 19.4V of 21V",
// "reached 5-21V", "max 13V of 21V", "none reached", "not run", "advertised" (a Light run's listing), "needs load" (or "needs load >13V" after some levels passed:
// the highest that did), "floor at 5V" (a low edge the source cannot reach), "folds to 19.4V" (a level passed on the folded side of a sweep's fold). Levels at 0.1 V.
step_label::Text reach_footer(const Reach&);
// A Results row's footer and its Steps page's footer, "<row label>: n pass, m caution": the counts of the steps one row owns, the
// non-zero ones in the order pass, fail, caution, listed (a Light run's listing), skip; "no steps" when all are 0. The label goes
// when a count would not fit kFooterChars (the UI's footer band, kFooterCells) beside it; the first count that still does not fit ends the line.
inline constexpr unsigned kFooterChars = 27;
struct FamilyTally { unsigned pass = 0, caution = 0, fail = 0, listed = 0, skip = 0; };
step_label::Text family_footer(const char* label, const FamilyTally&);
// The Detail page's "Reached" value (at most 16): "5-19.4V of 21V", "5-21V"; empty when nothing passed.
step_label::Text reach_span(const Reach&);
int token_width(const Token&, const Metrics&);
int cell_width(const Token&, const Metrics&);   // the grid cell a token needs (Metrics::cells)
size_t wrap(const Layout&, const Metrics&, std::span<Line> out);
const char* display_name(charger_test::Algorithm);

// Compact persistent representation for ChargerTestPage: step-backed rows store only their key (6 B) and regenerate their
// tokens on demand from Runner::steps(), avoiding a 2.2 KB persistent Layout inside the page arena.
inline constexpr size_t kMaxRowTokens = 24;   // the PD block: up to 11 PDO chips, each with a glued tag
inline constexpr size_t kCompactPoolBytes = 200;
inline constexpr size_t kMaxFactTokens = 24;

enum class CompactKind : uint8_t {
    Header, Probe, Pd, NoQc3Plus, Legacy, Fact, Collapsed,
    Levels   // OEM level row (OemNotes::vooc_watts): algo = the family's lead unit, mv = packed claims, first/count = oem_watts::kLevels slice
};
struct CompactRow {
    CompactKind kind = CompactKind::Header;
    uint8_t algo = 0;      // charger_test::Algorithm, Section (Header), or Tone (Fact / Collapsed label tone)
    uint16_t mv = 0;       // Pd: bit 0 = fold (skipped EPR levels get no chip), Fact label pool offset, or Collapsed (0=absent, 1=skipped)
    uint8_t first = 0;     // Fact: first fact_tok index; Levels: first oem_watts::kLevels index
    uint8_t count = 0;     // Fact: fact_tok count; Levels: level count
};
struct FactToken {
    uint8_t off = 0;       // byte offset in CompactGrid::pool (not null-terminated)
    uint8_t len_tone = 0;  // bits 0..4 length (0..17), bits 5..7 Tone
    uint8_t len() const { return len_tone & 31u; }
    Tone tone() const { return static_cast<Tone>(len_tone >> 5); }
};
struct CompactLine {
    uint16_t raw = 0;      // bits 0..4 row, bits 5..9 row-relative first token, bits 10..14 token count, bit 15 head
    static constexpr CompactLine make(uint8_t row, uint8_t first, uint8_t count, bool head) {
        return {uint16_t((row & 31u) | ((first & 31u) << 5) | ((count & 31u) << 10) | (head ? 0x8000u : 0u))};
    }
    uint8_t row() const { return raw & 31u; }
    uint8_t first() const { return (raw >> 5) & 31u; }
    uint8_t count() const { return (raw >> 10) & 31u; }
    bool head() const { return (raw & 0x8000u) != 0; }
};
struct CompactGrid {
    std::array<CompactRow, kMaxRows> rows{};
    std::array<FactToken, kMaxFactTokens> fact_toks{};
    std::array<char, kCompactPoolBytes> pool{};
    std::array<uint32_t, 2> collapsed_masks{};   // [0]=absent, [1]=skipped
    uint8_t row_count = 0, fact_count = 0, pool_used = 0, cuts = 0;
};
struct RowTokens {
    char label[10] = {};
    Tone label_tone = Tone::Dim;
    uint8_t count = 0;
    std::array<Token, kMaxRowTokens> toks{};
};

size_t build_compact(CompactGrid&, const Metrics&, std::span<CompactLine> out_lines,
                     std::span<const charger_test::Step>, const OemNotes& = {}, const ctsweep::View& sweep = {});
bool row_has(const CompactRow&, const charger_test::Step&);
int row_section(const CompactGrid&, size_t row);
const char* row_label(const CompactGrid&, size_t row);
void row_tokens(const CompactGrid&, size_t row, std::span<const charger_test::Step>, const ctsweep::View&, RowTokens& out);
// A level row's Results footer (at most kFooterChars): "<label>: up to 120W claimed" (the top claimed level; "20W claimed" for a
// one-level row), "<label>: 50-120W not reached" (the ladder ran, no level claimed), "<label>: 50-120W not tested". The label
// goes when the words would not fit beside it. Empty for any other row kind.
step_label::Text levels_footer(const CompactRow&, const char* label);

}  // namespace k2::result_layout
