// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Charger-test Results screen, pure layout: the run's steps become one row per protocol (a label plus
// chips), then the rows wrap into display lines. No drawing here; host-tested in tests/test_result_layout.cpp.
//   Fixed PD:  one chip per offered PDO, "5V 3A" (the advertised current, from the PDO in the run evidence).
//   PPS / AVS: one row per advertised range (APDO), one chip in it, "3.3-11V 3A", coloured by how many of its
//              steps passed; a range the charger does not offer reads "None". One row per APDO keeps a Mi 6A
//              probe the plan runs between two APDOs between their rows, where it executes.
//   Legacy:    one chip per distinct target voltage ("9V"), so QC3 up and down steps collapse; SCP adds its limit ("10V 2.25A").
//   Mi 6 A:    one row "Mi 6A" per probed level, a chip per probe step, "20V 6A", where the plan runs it.
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
// A caution step (a Pass that carries ct::Reason::Folded: the output folded back inside a sweep) counts as `capped` too, never as
// `pass` and never as `fail`: the source delivered every level, and one of them is worth a second look (amber, "CAUTION").
// A Skip step whose reason is Advertised or LightLevel (a Light run's listing) counts as `noted`, not as `skip`; an advertised PD
// offer (advertised()) is also counted in `adv`, which roll_up reads as a pass.
struct Tally { unsigned pass = 0, fail = 0, capped = 0, skip = 0, noted = 0, pending = 0, adv = 0; };
Tone roll_up(const Tally&);
// A Light run's listing: a Skip whose reason is Advertised or LightLevel (counted as `noted`, drawn as kit::Verdict::Noted).
bool noted(const charger_test::Step& s);
// A PD offer a Light run lists without asking for it (a fixed PDO, a PPS / AVS APDO, an EPR level the source listed): the source
// advertises it, so it is drawn with the pass look (kit::Verdict::Pass) while its band word stays LISTED. Not UFCS, SCP or a legacy level.
bool advertised(const charger_test::Step& s);
// A voltage fold the deep sweep found: a Pass that carries Reason::Folded (the sweep row, and the fixed rows of the same APDO
// that sit on the folded side). A caution, drawn kit::Verdict::Partial.
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
    uint16_t mv = 0;     // the level a fixed or legacy chip stands for (0: a range, a word, or a probe): the Running grid finds the
                         // chip of the step in flight by it (a legacy chip merges the up and down steps of one level)
};
struct Row {
    char label[9] = {};
    Tone label_tone = Tone::Dim;
    charger_test::Algorithm algorithm = charger_test::Algorithm::Count;   // Count: a collapsed row or a header
    bool header = false;            // section header row: no tokens, not openable
    bool probe = false;             // a Mi 6A probe row (algorithm Count): the 6 A hunt steps at `mv`
    bool sweep = false;             // a deep-sweep row (one per APDO): one range chip, the swept range ("5-21V 3.2A")
    uint8_t pos = 0;                // the PDO slot of a range or sweep row; 0: not one of those
    uint16_t mv = 0;                // the probed level of a probe row
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
struct Metrics { int adv = 6, pad = 2, gap = 2, avail = 158, mark = 11, max_groups = 0; bool cells = false; };
// The Results / Running verdict grid wraps the layout into lines of at most 5 equal cells (8 px glyphs, 163 px beside the label
// strip, 1 px gaps). Shared with the gate's sweep of every passive label (tests/test_ui_no_truncation.cpp).
inline constexpr Metrics kGridMetrics{.adv = 8, .pad = 2, .gap = 1, .avail = 163, .max_groups = 5, .cells = true};

// Results-tab sections, in the default run order (docs/CHARGER_TEST_FLOW.md): the OEM vendor modes by company
// (Huawei / Honor, Samsung, OPPO / OnePlus / vivo), the identity facts of a PD source (QC4+/QC5, Lenovo, HP), USB PD (CC) with
// Xiaomi / Redmi (the Mi 6A probes, PD-based) where the planner runs them, then QC / UFCS. Row labels hold 8 characters, so the
// headers are the short forms of the wizard's family names.
enum class Section : uint8_t { Huawei = 0, Samsung, Oppo, Xiaomi, OemPd, UsbPd, Qc, Count };
const char* section_name(Section);
Section section_of(charger_test::Algorithm);
// Optional facts about the source (labels from the source identity, never behaviour):
// qc_label/qc_why = the passive QC4+/QC5 label, vendor = VID-driven OEM name (Lenovo/HP/...), mi_auth =
// the SVID 0x2717 auth completed this session (shown in the Xiaomi section with the 6 A probes).
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
    unsigned monitor_mv = 36000;   // the K2's monitor ceiling, for planning a pending sweep row's strip range
    bool oppo_svid = false;   // the source listed OPPO's SVID: a passive "OPPO SVID 0x22D9 (discovery only)" row, nothing sent
    const char* scp_cap = nullptr;   // what the adapter's SCP readout reported, the words after "SCP cap " of report_card::scp_note ("20V 6A")
    // The SuperVOOC ceiling stage's result, the words of svooc_ceiling::format_cap ("<=11.1V, 10.9A claimed", "refused"): the highest
    // current limit the K2 asked for under which the charger kept the session going. Never a drawn current. nullptr = no stage ran.
    const char* svooc_cap = nullptr;
    // The VOOC family's ident command the detection row heard (svooc_ceiling::format_ident: "0xAC"): a passive "VOOC id" row at
    // every level. One ident proves the family, not the variant. nullptr = none heard.
    const char* vooc_ident = nullptr;
    unsigned rebalanced = 0;   // how often the source re-announced its list mid-run (budget-reannounce-mid-run): a passive "SOURCE rebalanced Nx" row
};
// Vendor-VID label for the OEM section; nullptr when the VID has no PD-derived OEM protocol to name.
const char* vendor_oem_name(uint16_t vid);

// The rows come from the steps alone: they are the plan, in the order the runner executes them, so a row stands where the plan first
// runs one of its steps. A row is a protocol's steps (a fixed PD row: one per algorithm; a PPS / AVS row: one per APDO, Row::pos),
// or (a probe row, Row::probe) the 6 A hunt steps at one level, wherever the planner put them, or (a sweep row, Row::sweep) the
// deep sweep of one APDO: one range chip like the range row's, coloured by the sweep's result and the worst recorded error
// (the points come from `sweep`, the runner's record). The Running grid, the Results page and the Steps page read the
// same step list, so none keeps an order of its own.
void build(Layout&, std::span<const charger_test::Step>, const OemNotes& = {}, const ctsweep::View& sweep = {});
// Does step `s` belong to layout row `r`? A probe row owns the hunt steps at its level; a sweep row the sweep step of its
// APDO; a range row (one per APDO) owns its slot's other steps; any other protocol row the protocol's other steps.
bool row_has(const Row& r, const charger_test::Step& s);
// Appends a fact row of plain words after the protocol rows (a collapsed row: its OK opens the whole step list).
// Dropped when the layout is full.
void add_note(Layout&, const char* label, Tone, std::initializer_list<const char*> words);
// The Results summary card: the best passing mode of each protocol family, as words with volts and amps.
//   PD (fixed and PPS: amps = the advertised PDO current, or the requested one on the full-current PPS row; AVS: volts only,
//   ranked by the offer's limit), Mi (the 6 A hunt,
//   amps = the probed current), QC / AFC / Huawei / OPPO / UFCS (D+/D- modes: volts only, SCP adds its limit; a UFCS grid
//   chip, not this line, carries its mode's current: step_label::ufcs_level).
// Ranked by watts where both are known, then the volts-only families by voltage. A handshake without a voltage proof
// reads "VOOC ok" / "SVOOC seen". `mw` is 0 when no current is known. Returns the lines written (at most out.size()).
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
// The Detail page's "Reached" value (at most 16): "5-19.4V of 21V", "5-21V"; empty when nothing passed.
step_label::Text reach_span(const Reach&);
int token_width(const Token&, const Metrics&);
int cell_width(const Token&, const Metrics&);   // the grid cell a token needs (Metrics::cells)
size_t wrap(const Layout&, const Metrics&, std::span<Line> out);
const char* display_name(charger_test::Algorithm);

// Compact persistent representation for ChargerTestPage: step-backed rows store only their key (6 B) and regenerate their
// tokens on demand from Runner::steps(), avoiding a 2.2 KB persistent Layout inside the page arena.
inline constexpr size_t kMaxRowTokens = 20;
inline constexpr size_t kCompactPoolBytes = 200;
inline constexpr size_t kMaxFactTokens = 24;

enum class CompactKind : uint8_t {
    Header, Probe, Sweep, Range, RangeNone, NoQc3Plus, Fixed, Legacy, Fact, Collapsed
};
struct CompactRow {
    CompactKind kind = CompactKind::Header;
    uint8_t algo = 0;      // charger_test::Algorithm, Section (Header), or Tone (Fact / Collapsed label tone)
    uint16_t mv = 0;       // Probe mv, Range/Sweep pos, Fact label pool offset, or Collapsed (0=absent, 1=skipped)
    uint8_t first = 0;     // Fact: first fact_tok index
    uint8_t count = 0;     // Fact: fact_tok count
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
    uint16_t monitor_mv = 36000;
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

}  // namespace k2::result_layout
