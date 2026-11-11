// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The K2 UI component kit (docs/UI_DESIGN.md "Component kit", skill k2-micro-gui section 6): one implementation of every
// shared piece, so a page states WHAT it shows and never how to draw it. Painters are stateless; the few caches
// (RowList, ActionCell, ProgressBar) are small value types that skip a repaint when nothing changed.
//
// Geometry is the approved 224 x 240 content canvas: top bar y 0..20 (hairline at 20), body from y 24, footer y 222.
// Rows are 26 px with a 1 px kRule line at the TOP of the row, text 16 px at x28, markers at x8, values right-aligned to
// x214. The focused row is kFocusT with a 3 px cyan bar at x0..2 and opens to 40 px to show its 12 px note.
//
// Pure style and geometry tables (verdict_style, constants) are constexpr so host tests pin them down.
#pragma once
#include <cstdint>
#include <string_view>
#include "k2/ui/gfx.hpp"
#include "k2/ui/list_view.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_icons.hpp"
#include "k2/ui/ui_style.hpp"
#include "k2/ui/ui_widgets.hpp"

namespace k2::kit {

// ---- geometry ---------------------------------------------------------------------------------------------------------
constexpr int16_t kBodyY = 24;            // first body row, under the top bar and its rule
constexpr int16_t kRowH = 26;             // list and key/value row pitch
constexpr int16_t kFocusExtra = 14;       // the focused row opens to kRowH + kFocusExtra = 40 px when it has a note
constexpr int16_t kNoteDy = 24;           // note line top, from the row top
constexpr int16_t kMarkX = 8, kMarkSize = 12;
constexpr int16_t kTextX = 28;            // row text left edge
constexpr int16_t kTailX = 214;           // right edge of a row's value, count or chevron
constexpr int kTailGap = 8;               // gap between a title and the value / chevron beside it
constexpr int16_t kNoteMaxPx = 190;       // note box: x28 .. 218
constexpr int kWindowRows = (kFooterY - kBodyY - kFocusExtra) / kRowH;   // rows in the default body, with the open row: 7
static_assert(kWindowRows == 7, "the focused row's 14 px must fit above the footer");
static_assert(kBodyY + kWindowRows * kRowH + kFocusExtra <= kFooterY, "a RowList never reaches the footer band");
constexpr int16_t kListBottom = kBodyY + kWindowRows * kRowH + kFocusExtra;   // where a default RowList (at kBodyY) ends: 220
constexpr int16_t kProgPitch = 75, kProgSegW = 74, kProgY = kBarH - 1, kProgH = 2;   // wizard progress rule
constexpr int kProgSegs = 3;
constexpr int16_t kBelowBar = kProgY + kProgH;   // first row no top bar touches (22): a body fill starts here, never at kBarH
constexpr int16_t kKvValueX = 72;         // key/value tables: value column (x92 for long keys)
constexpr int16_t kInfoTagX = 4, kInfoValueX = 52, kInfoPitch = 20;   // InfoBlock columns

// y of a 16 px (or `font_h` px) text line inside a row of `pitch` whose rule is on the top line.
constexpr int16_t row_text_y(int16_t pitch, int16_t font_h) { return static_cast<int16_t>(1 + (pitch - font_h) / 2); }

// ---- single-pass ground ---------------------------------------------------------------------------------------------------
// A box of the surface: what a ground fill leaves to a glyph, a marker or a rule that is drawn over it.
struct Box {
    int16_t x, y, w, h;
};
// Paints `r` in `color` except where `holes` go (clipped to `r`, may touch; at most 6 are used). Whatever is drawn afterwards owns
// its hole, so no pixel is streamed twice: the panel bus is the cost of a redraw. On a scaled page (ContentViewport::scaled())
// a glyph no longer covers its hole (it keeps its native size), so there the whole of `r` is filled and the glyphs paint over it.
void fill_except(const Surface &s, Box r, const Box *holes, int n, uint16_t color);

// A text line of a ground: its origin, font, text and ink (the bg is the ground's).
struct TextRun {
    int16_t x, y;
    const Font *font;
    std::string_view text;
    uint16_t ink;
};
// `ground` in `bg` with every run's text on it, each pixel streamed once: the ground is filled around the runs' boxes
// (the box a gfx_text writes is text_width x font height), then the runs are drawn. At most 6 runs leave a hole; more still draw.
void paint_texts(const Surface &s, Box ground, const TextRun *runs, int n, uint16_t bg);

// ---- VerdictCell --------------------------------------------------------------------------------------------------------
// pass: green tint + green ink; fail: solid red + black ink; none / stopped: no fill, dim ink (a mode the charger never
// offered is never red); skip: the same dim ink with a 1 px strike-through across the label (a labelled cell; a label-less
// one keeps the dash) so a skipped test never reads as pending; pending: no fill, faint ink, no strike;
// running: the pending look plus a solid kFocus dot (kRunDotSize square) in the cell's top-right corner, or centred in a
// label-less cell; steady, no blink. The dot is a few pixels, so it can move from cell to cell in the tick the step changes
// (ResultGrid::sync_dots) without repainting either cell;
// partial: amber tint + amber ink, a 2 px amber bar along the bottom edge (a labelled cell) or a tilde (no label): some of the
// steps passed or the source capped below the target. Amber because it is abnormal; the bar / tilde because colour alone never
// carries a state (redundant coding). Appended last: the values travel in RowSpec::state.
enum class Verdict : uint8_t { Pending, Running, Pass, Fail, None, Skip, Stopped, Partial };
constexpr int16_t kPartialBarH = 2;   // the bar of a labelled Partial cell
constexpr int16_t kRunDotSize = 5;    // the Running cell's dot: a kRunDotSize x kRunDotSize square
constexpr int16_t kRunDotInsetX = 1;  // ... that many px in from the cell's right edge
constexpr int16_t kRunDotInsetY = 0;  // ... and from its top edge (the label's ink starts 7 px down in a grid cell, so the dot clears it)
constexpr int kRunDotPx = kRunDotSize * kRunDotSize;
struct VerdictStyle {
    uint16_t fill, ink, dot;
    bool filled, dotted;
};
constexpr VerdictStyle verdict_style(Verdict v) {
    switch (v) {
    case Verdict::Pass:    return {kOkT, kOk, 0, true, false};
    case Verdict::Fail:    return {kError, kBg, 0, true, false};
    case Verdict::Partial: return {kCautionT, kCaution, 0, true, false};
    case Verdict::Running: return {0, kFaint, kFocus, false, true};   // the pending look + the bright dot (the shape cue)
    case Verdict::Pending: return {0, kFaint, 0, false, false};
    case Verdict::None:
    case Verdict::Skip:
    case Verdict::Stopped: break;
    }
    return {0, kDim, 0, false, false};
}
// Paints the cell (w x h at x, y): its base (the style's fill, else `under`), the outline, then `label` centred (S12, or
// S16 from 22 px tall) in the ink colour, or the state's glyph (check, cross, tilde, dash, dot) when the label is empty.
void draw_verdict(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, Verdict v, std::string_view label,
                  uint16_t under);
// Pixels a cell streams beyond its area: a Skip label's strike is painted over the text, the label's width + 2 px (clipped to
// the cell). 0 for every other verdict and for an empty label. The budgeted grid adds it to the cell's cost.
int strike_px(Verdict v, std::string_view label, int16_t w, int16_t h);
// A labelled Running cell's dot is painted over the cell too: kRunDotPx more. (A label-less one leaves a hole for its dot.)
constexpr int dot_px(Verdict v, bool labelled) { return v == Verdict::Running && labelled ? kRunDotPx : 0; }
// A name in font `f` at (x, y) with the same strike as a Skip cell's label (text, then the 1 px line in `ink` over it): the
// card's rows of a skipped unit, where the verdict cell is a label-less 12 px glyph. Costs the text width + 2 px more.
void draw_struck_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t ink, uint16_t bg);

// ---- TopBar -------------------------------------------------------------------------------------------------------------
// Wizard progress rule: the bar's bottom edge (rows 20..21) as three 74 px segments, the first `step` in kFocus and the
// rest in kRule. step 0 leaves the plain hairline.
void draw_progress_rule(const Surface &s, int step);
// Band, title (with "n/3 " in front for a wizard step 1..3), badge behind its rule, and the progress rule. The live V x A
// readout is the page's (PageBase::title_badge), it repaints on its own cache. `readout_follows`: the caller repaints the
// readout area (x 140..223) right after, so the band leaves it alone (one write per pixel).
void draw_top_bar(const Surface &s, std::string_view title, int step = 0, std::string_view badge = {},
                  bool readout_follows = false);

// ---- RowList -------------------------------------------------------------------------------------------------------------
enum class Marker : uint8_t { None, Radio, Box3, Check, Verdict, Icon };
struct RowSpec {
    Marker marker = Marker::None;
    uint8_t state = 0;           // Radio / Check: 0 off, 1 on. Box3: 0 off, 1 part, 2 on. Verdict: a Verdict value
    Icon icon = Icon::Bolt;      // Marker::Icon
    uint16_t marker_color = 0;   // 0 = derived (kFocus when on, kDim when off; Icon: kDim)
    std::string_view text;
    uint16_t text_color = kText;
    std::string_view tail;       // value or count, right-aligned to kTailX
    uint16_t tail_color = kDim;
    bool chevron = false;        // a drill-in chevron at kTailX instead of tail text
    bool data = false;           // `text` is external data (a saved name): fitted with fit_data, counted instead of a gate failure
    std::string_view note;       // 12 px dim line under the focused row; a focused row with a note opens to 40 px
    std::string_view col2;       // dim second column, left-aligned at kCol2X (the text is cut to fit before it)
    uint16_t tail_tint = 0;      // non-zero: the tail is a 12 px pill on this fill (text in tail_color) instead of plain text
    bool tail_yields = false;    // the tail is a hint: it is left out when `text` would not fit beside it (the text then runs up to the scroll lane)
};
constexpr int16_t kCol2X = 104;
// The 12 x 12 marker at (x, y) on `bg`.
void draw_marker(const Surface &s, int16_t x, int16_t y, const RowSpec &r, uint16_t bg);
// A drill-in chevron (5 x 9) whose right edge is `right`, centred on `cy`.
void draw_drill(const Surface &s, int16_t right, int16_t cy, uint16_t color);

// A ListView whose rows share one look. One instance lives in the UiContext (only one page is visible at a time); a
// page binds it with init(), keeps its own selection and re-applies count and selection on every update().
class RowList {
public:
    // Builds row `index` (focused = the selected row). Strings must stay valid until the next call into the page.
    using RowFn = void (*)(void *ctx, int index, bool focused, RowSpec &out);
    // The list sits at (0, y) and shows as many rows as fit above the footer with the open row counted. A different `ctx` than
    // the last bind (another page) restarts the window at the top; the same one keeps count and offset (repaint passes).
    void init(const Surface &s, int16_t y, RowFn fn, void *ctx);
    void set_count(int n) { count_ = n < 0 ? 0 : n; }
    void select(int index) { sel_ = index; }
    int selected() const { return sel_; }
    int count() const { return count_; }
    int visible() const { return list_.state().visible; }
    const ListState &state() const { return list_.state(); }
    // Applies count and selection (the window follows the selection), repaints what changed. An idle call writes nothing.
    void draw(uint32_t now_ms);
    void invalidate();   // after an erase, a capture pass or a page repaint
    void restart() { list_.set_offset(0); }   // a page with several lists on one binding (the wizard's stages): a new list starts on top
    // Clears the body around a default list (one at kBodyY): the strips above and below it. The list repaints every row it
    // owns (opaque rows, the tail strip), so a body fill under it would write those pixels twice.
    static void clear_body_around(const Surface &s);

private:
    static void draw_row_cb(void *ctx, int index, const Rect &r, bool highlighted, Field &f);
    static uint32_t row_sig_cb(void *ctx, int index);

    ListView list_;
    Surface surf_{};
    int16_t y_ = kBodyY;
    RowFn fn_ = nullptr;
    void *ctx_ = nullptr;
    int count_ = 0, sel_ = 0;
    int16_t extra_ = -1;          // the focus expansion last applied (-1 = unknown)
};

// ---- KvTable -------------------------------------------------------------------------------------------------------------
// Rows of `pitch` px under the body top, a hairline at the top of every row and one closing the table, the key dim at
// label_x and the value in a fixed Field box. The caller keeps the Field array (a page already needs its live cells).
struct KvLayout {
    int16_t y0 = kBodyY, pitch = kRowH;
    int16_t label_x = 6;
    const Font *label_font = nullptr;   // nullptr = kSmall
    const Font *value_font = nullptr;   // nullptr = kSmall
    int16_t value_x = kKvValueX;        // left edge of the value box
    uint8_t value_chars = 18;
    Align align = Align::Left;
};
constexpr int16_t kv_row_y(const KvLayout &l, int i) { return static_cast<int16_t>(l.y0 + i * l.pitch); }
// The static part: hairlines and keys. `labels` holds n strings.
void kv_static(const Surface &s, const KvLayout &l, const char *const *labels, int n);
// Initialises n value cells (white on kBg) at the layout's value box of each row.
// `chars` (optional, n entries) narrows each cell to its own worst-case width; a right-aligned cell keeps the layout's right
// edge (value_x + value_chars * advance), so a repaint never reaches a long key to its left.
void kv_cells(const Surface &s, const KvLayout &l, Field *cells, int n, const uint8_t *chars = nullptr);
// The ground of the n rows (under each row's hairline) around the key, the value cell (same `chars` as kv_cells) and, for a row
// whose bit is set in `marks`, the verdict marker at kMarkX: every pixel the static part and the cells leave, laid once. A page
// calls it before kv_static when it opens over a live screen, so it covers its rows with no erase pass.
void kv_ground(const Surface &s, const KvLayout &l, const char *const *labels, int n, const uint8_t *chars = nullptr, uint32_t marks = 0);
// One static key/value row at y (no cache): the key at label_x, the value clipped to the box, then the hairline.
// `ours`: the value is firmware text (fit_text: a cut fails the host gate); default false = it may be a name or a vendor string
// (fit_data: cut by design, only counted).
void kv_row(const Surface &s, const KvLayout &l, int16_t y, std::string_view key, std::string_view value, uint16_t value_color,
            bool ours = false K2_TRUNC_DECL);

// ---- AlertBand -----------------------------------------------------------------------------------------------------------
// Full-width band, kCaution or kError fill: a 20 px black triangle and the word in S24 (black ink). Short lines below it
// are the page's (S16 on kBg), choices below those are a RowList.
constexpr int16_t kAlertH = 36;
void draw_alert_band(const Surface &s, int16_t y, bool error, std::string_view word);
// A warning triangle of `sz` px (the lab's glyph): solid `color`, exclamation mark in `ink`.
void draw_warn_triangle(const Surface &s, int16_t x, int16_t y, int16_t sz, uint16_t color, uint16_t ink);

// ---- ActionCell ----------------------------------------------------------------------------------------------------------
// The one thing OK can do on a page (START). Idle: 2 px kFocus outline, kFocusT fill, the verb in S24 centred and an
// optional caution hint (triangle + S12) under it. Armed: kCaution fill, black word (CONFIRM), a S12 line and a 4 px
// black bar along the bottom that drains over the confirm window (only the vacated columns repaint).
class ActionCell {
public:
    static constexpr int16_t kBarH = 4;
    void init(const Surface &s, int16_t y, int16_t h) { surf_ = s; y_ = y; h_ = h; invalidate(); }
    void draw_idle(std::string_view verb, std::string_view hint);
    // `left_permille` 1000..0: the share of the window still left; `sub` is the line under the word.
    void draw_armed(std::string_view word, std::string_view sub, int left_permille);
    void invalidate() { e_.invalidate(); bar_px_ = -1; }

private:
    Surface surf_{};
    int16_t y_ = 0, h_ = 0;
    Elem e_;
    int16_t bar_px_ = -1;   // bar width drawn so far
};

// ---- ProgressBar ---------------------------------------------------------------------------------------------------------
// 3 px kRule track with a kFocus fill of done / total. Repaints only when the fill width changes (between steps).
class ProgressBar {
public:
    static constexpr int16_t kH = 3;
    void init(const Surface &s, int16_t x, int16_t y, int16_t w) { surf_ = s; x_ = x; y_ = y; w_ = w; px_ = -1; }
    void set(int done, int total);
    // The pixels set(done, total) would write now: 0 when the bar is already there (a budget asks before it paints).
    int cost(int done, int total) const;
    void invalidate() { px_ = -1; }

private:
    int px_for(int done, int total) const;
    Surface surf_{};
    int16_t x_ = 0, y_ = 0, w_ = 0, px_ = -1;
};

// ---- InfoBlock -----------------------------------------------------------------------------------------------------------
// A tag / value row: the tag (S16, dim) at x4, the value (S16) at x52. The row (kInfoPitch tall, full width) is wiped
// first, so a shorter value leaves nothing behind.
void draw_info_row(const Surface &s, int16_t y, std::string_view tag, std::string_view value, uint16_t value_color = kText);

// ---- ResultGrid ----------------------------------------------------------------------------------------------------------
// A 60 px label strip (S12, dim, right-aligned at x55, 1 px rule at x60) and up to 5 verdict cells per row that flex-fill the
// rest (1 px gaps). A row whose cells carry a current is 40 px tall: the cell part is 24 px and under it one bracket per run
// of equal current (S12, dim, neutral: the verdict belongs to each voltage). A volt-only row is 26 px. A family's first row
// opens with a 2 px kRule section rule, counted in its height. A row that is the only cell of its unit and never ran spells
// the reason as the cell label. The focused unit lights the label strip (kFocusT, 3 px kFocus bar, kFocus label).
constexpr int kResultMaxCells = 5;
constexpr int16_t kResultLabelW = 60;                       // label strip, x0..59, rule at x60
// Worst case of strike_px in one 224 px grid row: a strike never exceeds its own cell and the cells tile what is right of the
// rule, so all five struck at once is the cell span (the headroom the page's one-row budget adds, ui_pages.hpp kFlightRowPx).
constexpr int kStrikeRowMaxPx = 224 - kResultLabelW - 1;
// Worst case of the dots in one row: the running step is one cell, so one dot (a row never wears two).
constexpr int kDotRowMaxPx = kRunDotPx;
constexpr int16_t kResultRowH = 26, kResultRowHCur = 40;    // volt-only and with-current row heights
constexpr int16_t kResultSectionH = 2;                      // the rule above a family's first row
struct ResultCell {
    Verdict verdict = Verdict::Pending;
    std::string_view label;     // the volt text ("5V"), or the reason on a lone never-ran cell
    std::string_view current;   // "3A": brackets are drawn under the run of cells that share it; empty = volt-only
};
struct ResultRowSpec {
    std::string_view label;     // unit label, up to 9 chars at S12
    ResultCell cells[kResultMaxCells];
    uint8_t n = 0;
    bool section = false;       // 2 px rule above
    bool focus = false;         // the unit under the cursor
};
constexpr int16_t result_row_h(const ResultRowSpec &r) {
    bool cur = false;
    for (int i = 0; i < r.n; i++) cur = cur || !r.cells[i].current.empty();
    return static_cast<int16_t>((cur ? kResultRowHCur : kResultRowH) + (r.section ? kResultSectionH : 0));
}
// A verdict cell's box on the surface.
struct CellRect {
    int16_t x = 0, y = 0, w = 0, h = 0;
};
// The Running cell's dot of a labelled cell (top-right corner), on its own: `on` paints it in kFocus, else the box is wiped
// to `under` (the cell has no fill, so its base is the caller's background). kRunDotPx pixels either way; the cell and its
// label are not touched, as long as the dot's box misses the label (the grid's cells are wide enough: tests/test_kit.cpp).
CellRect run_dot_box(const CellRect &cell);
void draw_run_dot(const Surface &s, const CellRect &cell, bool on, uint16_t under = kBg);
// One row at y (h from result_row_h). Every pixel of the row is streamed exactly once, so repainting it needs no erase.
void draw_result_row(const Surface &s, int16_t y, int16_t h, std::string_view row_label, const ResultCell *cells, int n,
                     bool focus = false, bool section = false);

// The scrolling body: rows of different heights from `first`, only whole rows draw. Each visible slot has a signature, so a
// repaint writes the rows that changed and nothing else (the grid repaints between steps, never inside one).
class ResultGrid {
public:
    using RowFn = void (*)(void *ctx, int index, ResultRowSpec &out);
    static constexpr int kMaxSlots = 8;
    // The grid fills y .. bottom of the surface. A different ctx than the last bind restarts at the top.
    void init(const Surface &s, int16_t y, int16_t bottom, RowFn fn, void *ctx);
    void set_count(int n) { count_ = n < 0 ? 0 : n; }
    int count() const { return count_; }
    // Scrolls (in whole rows, by pixel height) until `index` is fully visible; `lead` rows above it stay visible when they fit.
    void show(int index, int lead = 0);
    // Scrolls so the window ends on `index`: the least scroll (counted in pixels, the rows differ in height) that shows it, so
    // the rows above it stay on screen as long as they fit. While everything up to `index` fits, first() is 0 (no blank
    // above). Unlike show() it also scrolls back up, so the result depends only on `index`.
    void anchor_bottom(int index);
    int first() const { return first_; }
    void set_first(int i) { first_ = i < 0 ? 0 : i; }
    void draw();
    // draw() in pieces of at most `budget_px` (a changed row costs width * height, a row whose verdicts alone changed only
    // its changed cells, the blank under the last row its area). `prefer` (a row index, -1 = none) is painted first. The
    // first piece of a call always paints, so a call progresses with any budget (`strict`: unless it exceeds the budget).
    // Returns the pixels painted; pending() says that something was left for the next call.
    int draw_some(int budget_px, int prefer = -1, bool strict = false);
    bool pending() const { return pending_; }
    void invalidate();
    // Rows fully visible from first(); y of the row `index` (-1 when it is not visible).
    int visible() const { return vis_; }
    int row_y(int index) const;
    // The Running dot's cheap path: the grid remembers where its one dot is on screen, and moves it to the labelled cell the
    // rows now call Running, kRunDotPx an arrival and kRunDotPx a departure, no budget. The caller runs it first in a tick (before
    // first() moves) so the dot moves with the step. The old dot leaves wherever it is (the cell then reads Pending until the
    // budgeted draw_some repaints its verdict); the new one arrives when its row is on screen where the rows put it, whether or
    // not its currents changed (they do not move the cell's top edge). A cell whose row is not on screen yet gets its dot from
    // draw_some, with the row. One Running cell at a time. Returns the pixels painted.
    int sync_dots();

private:
    Surface surf_{};
    int16_t y_ = 0, bottom_ = 0;
    RowFn fn_ = nullptr;
    void *ctx_ = nullptr;
    int count_ = 0, first_ = 0, vis_ = 0;
    int16_t end_y_ = -1;        // where the last drawn row ended
    uint32_t sig_[kMaxSlots] = {};      // the shape of the row painted in each slot (everything but the verdicts)
    uint16_t vd_[kMaxSlots] = {};       // ... and its cells' verdicts, 3 bits each
    uint32_t core_[kMaxSlots] = {};     // the place and cells of the row (sig_ without its height and currents): where a dot goes
    bool valid_[kMaxSlots] = {};
    bool pending_ = false;
    bool dot_on_ = false;               // the Running dot is on screen,
    bool dot_hit_ = false;              // ... its box touches the label's ink (a cell too short for the cheap erase),
    CellRect dot_cell_{};               // ... on this cell,
    int dot_slot_ = 0, dot_k_ = 0;      // ... of this slot's row, cell k
    void note_dot(const CellRect &cell, bool running, std::string_view label, int slot, int k);
};

// ---- PassBandGauge -------------------------------------------------------------------------------------------------------
// A 12 px tall strip at (x, y), w wide: 4 px kRule track in the middle, the kOk window of +-5 % around `target` (kOkT 8 px
// high with a kOk 4 px core) and a 2 px white needle at `value` on a 0..`full` scale (all in the same unit). The window is at
// least kPassBandMinW px wide, centred on the target, so a low target on a wide scale stays visible. Pass the same `full`
// for a whole run so the window moves with the target. Returns the needle's x (relative to the surface). With `prev_px`
// (the value last returned) only the old and new 2 px needle columns repaint; -1 paints the whole strip.
constexpr int16_t kPassBandH = 12;
constexpr int16_t kPassBandMinW = 5;
int draw_pass_band(const Surface &s, int16_t x, int16_t y, int16_t w, int32_t value, int32_t target, int32_t full,
                   int prev_px = -1);

// ---- RangeGauge ----------------------------------------------------------------------------------------------------------
// The gauge of a programmable sweep row: the same 12 px strip and 0..`full` scale as the PassBandGauge, but the window is the
// whole swept range [lo, hi] (two dotted 2 px lines, 2 on / 2 off, with the kOkT tint between them) and a small kFocus marker
// (a 6 px cap on the strip's top and bottom edges) shows the setpoint `set`, moving along the range point by point. The 2 px
// white needle shows `value`. range_geom is the pure geometry; draw_range_gauge paints. With `prev` (the last return) only the
// old and new marker and needle columns repaint; a negative needle paints the whole strip.
struct RangeGeom { int16_t lo, hi, mark, needle; };   // x of the left line, the right line, the marker centre, the needle (surface px)
struct RangePos { int16_t mark = -1, needle = -1; };
RangeGeom range_geom(int16_t x, int16_t w, int32_t value, int32_t set, int32_t lo, int32_t hi, int32_t full);
RangePos draw_range_gauge(const Surface &s, int16_t x, int16_t y, int16_t w, int32_t value, int32_t set, int32_t lo, int32_t hi,
                          int32_t full, RangePos prev = {});

// ---- HeroReadout ---------------------------------------------------------------------------------------------------------
// 48 / 64 px digits right-aligned in a `chars`-wide Field box whose RIGHT edge is `x`, with the unit in S24 on the digits'
// baseline 3 px behind it. `full` (first paint, a colour change, a repaint after an erase) re-initialises the field and draws
// the unit; otherwise only the characters that changed repaint.
struct HeroSpec {
    const Font *digits = nullptr;
    int16_t x = 0, y = 0;       // right edge of the digit box, top of the digit line
    uint8_t chars = 0;
    uint16_t color = kText;
    std::string_view unit;
};
void draw_hero(const Surface &s, Field &digits, const HeroSpec &h, std::string_view value, bool full = false);

}  // namespace k2::kit
