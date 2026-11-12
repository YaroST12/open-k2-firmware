// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UI design v2 primitives: rounded fills and frames, pills, gauges, chevrons, port symbols,
// icons and sparklines. Everything is drawn row by row through the strip renderer (no
// framebuffer). Geometry that does not touch the panel (corner insets, pill width, gauge fill,
// chevron rows, sparkline cells) is a constexpr/pure function so host tests can pin it down.
#pragma once
#include <cstdint>
#include <string_view>
#include "k2/ui/gfx.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_icons.hpp"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

// ---- pure geometry ---------------------------------------------------------------------------
// Pixels cut off the left (and right) end of row `i` (0 = top row) of a rounded corner of radius r.
constexpr int round_inset(int r, int i) {
    if (r <= 0 || i < 0 || i >= r) return 0;
    const int dy2 = 2 * (r - i) - 1;          // twice the distance from the row centre to the arc centre
    const int q = 4 * r * r - dy2 * dy2;
    int s = 0;
    while ((s + 1) * (s + 1) <= q) s++;       // integer sqrt (tiny radii only)
    return r - (s + 1) / 2;
}

// Inset of row i of a w x h rounded rectangle (the bottom rows mirror the top ones).
constexpr int round_row_inset(int r, int h, int i) {
    if (r * 2 > h) r = h / 2;
    return round_inset(r, i < h - 1 - i ? i : h - 1 - i);
}

constexpr int kPillH = 14;
constexpr int pill_width(int chars) { return chars * static_cast<int>(kLabelAdv) + 8; }

// Width in pixels of a gauge fill: 0 when the value is not positive, at least 2 otherwise,
// never more than w. `full` <= 0 draws an empty gauge.
constexpr int gauge_fill_px(int w, int32_t value, int32_t full) {
    if (value <= 0 || full <= 0) return 0;
    if (value >= full) return w;
    const int px = static_cast<int>(static_cast<int64_t>(w) * value / full);
    return px < 2 ? 2 : px;
}

// Vertical chevron, 10 x 6: row k covers [l, l+2) and [8-l, 10-l) with l = kChevL[k]. The "up"
// shape lists rows top to bottom, "down" mirrors them.
constexpr int kChevW = 10, kChevH = 6, kChevPitch = 8;
constexpr uint8_t kChevL[kChevH] = {4, 3, 2, 1, 0, 0};
constexpr int chevron_left(bool up, int row) { return kChevL[up ? row : kChevH - 1 - row]; }

// The same chevron turned with the screen (rotation 0..3, see draw_physical_chevron): it points at the
// physical TOP port when toward_top, at the BOTTOM port otherwise. Quarter turns are 6 wide x 10 tall.
// physical_chevron_row() is row `row` as a bit mask, bit c = column c from the left; the status-bar
// band writes it row by row, draw_physical_chevron draws the identical pixels (host-tested).
constexpr int physical_chevron_w(unsigned rotation) { return (rotation & 1) ? kChevH : kChevW; }
constexpr int physical_chevron_h(unsigned rotation) { return (rotation & 1) ? kChevW : kChevH; }
constexpr uint16_t physical_chevron_row(bool toward_top, unsigned rotation, int row) {
    rotation &= 3;
    if (!(rotation & 1)) {
        const int l = chevron_left(toward_top != (rotation == 2), row);
        return static_cast<uint16_t>((3u << l) | (3u << (8 - l)));
    }
    uint16_t bits = 0;
    for (int src = 0; src < kChevH; ++src) {
        const int left = chevron_left(toward_top, src);
        const int col = rotation == 1 ? src : kChevH - 1 - src;
        for (int arm : {left, 8 - left}) {
            const int yy = rotation == 1 ? kChevW - 2 - arm : arm;
            if (row == yy || row == yy + 1) bits = static_cast<uint16_t>(bits | (1u << col));
        }
    }
    return bits;
}

// Solid arrow head of odd height h, tri_width(h) wide: row r is tri_row_w(h, r) pixels wide, growing
// to the middle row. Drawn with its box top-left at (x, y); right = points right (base on the left).
constexpr int tri_row_w(int h, int row) { return (row < h - 1 - row ? row : h - 1 - row) + 1; }
constexpr int tri_width(int h) { return (h + 1) / 2; }

// Sparkline cell classification. `level` is the height level of the row (h-1 = top row),
// `v` the sample height (0..h-1), `prev` the previous sample.
enum class Cell : uint8_t { None, Fill, Line };
constexpr Cell spark_cell(int level, int v, int prev) {
    const int lo = v < prev ? v : prev, hi = v < prev ? prev : v;
    if (level >= lo && level <= hi) return Cell::Line;
    return level < v ? Cell::Fill : Cell::None;
}

// ---- drawing (coordinates relative to the surface, like gfx_fill) -----------------------------
void fill_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, uint16_t color);
// fill_round without the rectangle (hx, hy, hw, hh): the caller paints it next with an opaque text cell, so no pixel is written twice.
void fill_round_hole(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, uint16_t color, int16_t hx, int16_t hy,
                     int16_t hw, int16_t hh);
void frame_round(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int r, int t, uint16_t color);

// 12 x 12 icon; set bits take fg, clear bits bg (opaque).
void draw_icon(const Surface &s, int16_t x, int16_t y, Icon icon, uint16_t fg, uint16_t bg);

// 14 x 14 one-bit icons of the Big page: a wall charger (power pins on a block) and a USB-C plug.
enum class Icon14 : uint8_t { Charger, PlugC };
void draw_icon14(const Surface &s, int16_t x, int16_t y, Icon14 icon, uint16_t fg, uint16_t bg);

// Anti-aliased vector check / cross, a square of `size` px (the font's cap height) at (x, y), fg over bg.
void draw_mark(const Surface &s, int16_t x, int16_t y, int size, bool check, uint16_t fg, uint16_t bg);
// Text where kMarkCheck / kMarkCross draw a green check / red cross sitting on the baseline. Returns the end x.
constexpr int mark_size(const Font &f) { return f.ascent * 3 / 4; }
constexpr int mark_cell(const Font &f) { return mark_size(f) + 4; }   // 2 px lead, mark, 2 px trail
int draw_marked_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t fg, uint16_t bg);
int marked_width(const Font &f, std::string_view text);

// Tag: r 3, S12 text. solid = fill in `color` with black text, otherwise `tint` fill with `color` text.
// Returns the pill width.
int draw_pill(const Surface &s, int16_t x, int16_t y, std::string_view text, uint16_t color, uint16_t tint,
              bool solid);

// Track (kRule) plus fill; only the fill/track boundary is repainted when `prev_px` is given.
void draw_gauge(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int fill_px, uint16_t color,
                int prev_px = -1);

// Bar with a label drawn over it (the PD trigger watt bar): rounded `track`, rounded `fill` of
// `fill_px`, then `text` (S12, 4 px in from the left, `fg`) blended over whichever of the two is
// behind each pixel. 0 <= fill_px <= w.
void draw_bar_label(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int fill_px, uint16_t track,
                    uint16_t fill, std::string_view text, uint16_t fg);

void draw_triangle(const Surface &s, int16_t x, int16_t y, int h, bool right, uint16_t color);

// One chevron with its top-left at (x, y); the pixels of a recolour are identical.
void draw_chevron(const Surface &s, int16_t x, int16_t y, bool up, uint16_t color);

// USB-C port symbols, 18 x 8: male = solid plug with a slot, female = outlined receptacle with a tongue.
void draw_port_male(const Surface &s, int16_t x, int16_t y, uint16_t color, uint16_t bg);
void draw_port_female(const Surface &s, int16_t x, int16_t y, uint16_t color);

// w x h sparkline of `w` samples (oldest first, each 0..h-1): bg box, three rule lines, a fill
// under the trace in `tint`, and a one-pixel trace in `color`.
constexpr uint8_t kSparkNone = 255;   // a column value that draws nothing (no sample yet)
void draw_sparkline(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, const uint8_t *samples,
                    uint16_t color, uint16_t tint, uint16_t bg);

// Range trace: `cols` columns, each drawn `xs` px wide, as a min..max band (lo/hi 0..255 of the full scale,
// bottom = 0) on the bg box with the three rule lines. `mark_a` / `mark_b` are column numbers that get a full
// height line in `ca` / `cb` (kSparkNone = no mark). Used for the transition-capture VBUS thumbnail.
constexpr int range_row(uint8_t v, int h) { return h - 1 - (v * (h - 1) + 127) / 255; }   // 0 = top row
void draw_range_trace(const Surface &s, int16_t x, int16_t y, int cols, int xs, int16_t h, const uint8_t *lo,
                      const uint8_t *hi, uint8_t mark_a, uint16_t ca, uint8_t mark_b, uint16_t cb, uint16_t color,
                      uint16_t bg);

// Footer: the band y 222..239, no icon. The tone is the band FILL (docs/UI_DESIGN.md section 6, item 9) and the
// message is S16 text from x 4. The whole band is repainted, so a message change needs no clearing.
constexpr int16_t kFooterY = 222;
constexpr int16_t kFooterTextX = 8;
constexpr int16_t kFooterBarW = 4;   // the tone bar at the left edge of an Ok / Caution fact line

// The most cells a footer line holds: the text runs x 4..219 (27 cells of the 8 px font). Constant footer strings
// are static_asserted against it; a longer runtime one is cut with "..." as the last resort and host builds report it
// to the no-truncation gate (trunc.hpp).
constexpr int kFooterCells = 27;
constexpr int kFooterTextPx = kFooterCells * static_cast<int>(kSmallAdvPx);   // 216

// Footer tone: Info is the neutral band (also for a plain hint or a count), Ok a good or active state, Caution an
// amber warning, Error a red one. A tone is a fill plus its text colour, so the mapping lives in one table.
// The Fieldbook fact line: Info is the ground with a 1 px rule on its top row and ink2 text; Ok and Caution a tint with a
// 4 px bar of the tone at the left edge and ink text; Error solid red with the on-red ink. `bar` 0 = none.
enum class FooterTone : uint8_t { Ok, Info, Caution, Error };
struct FooterStyle {
    uint16_t fill;
    uint16_t text;
    uint16_t bar = 0;    // the left-edge tone bar
    uint16_t rule = 0;   // the top row's rule (Info)
};
inline FooterStyle footer_style(FooterTone t) {
    switch (t) {
    case FooterTone::Ok:      return {kOkT, kInk, kOk, 0};
    case FooterTone::Caution: return {kCautionT, kInk, kCaution, 0};
    case FooterTone::Error:   return {kError, kErrInk, 0, 0};
    case FooterTone::Info:    break;
    }
    return {kBg, kInk2, 0, kRule};
}
// The tone for a status colour a caller already holds (kOk, kCaution, kError; anything else is neutral).
inline FooterTone footer_tone_of(uint16_t color) {
    return color == kOk ? FooterTone::Ok : color == kCaution ? FooterTone::Caution
         : color == kError ? FooterTone::Error : FooterTone::Info;
}

void draw_footer(const Surface &s, FooterTone tone, std::string_view text K2_TRUNC_DECL);
// Same as draw_footer, but paints through the element's cache: skips the repaint when the
// (tone, text) signature is unchanged since the last call with this `e` slot.
// e.invalidate() forces a repaint.
void draw_footer_cached(const Surface &s, FooterTone tone, std::string_view text, Elem &e K2_TRUNC_DECL);

// ---- top chrome bar (micro-GUI phase 1): 20 px band + 1 px hairline at y kBarH-1 -----------------
// Page name left; on every page except Home the right side carries the compact fmt3(V) x fmt3(A)
// readout (see draw_top_readout). The band is 21 rows: kBarH-1 = 20 band rows, the last row the
// 1 px hairline (kRule) between the bar and the content.
constexpr int kBarH = 21;
constexpr int16_t kBarTextY = 4;
// Fieldbook masthead geometry: icon and title, and the live readout's slots (right-aligned, each led by a 3 x 9 key).
constexpr int16_t kMastIconX = 8, kMastTitleX = 8, kMastTitleIconX = 26, kMastTitleY = 2;
constexpr int16_t kMastKeyW = 3, kMastKeyH = 9, kMastKeyY = 6;
constexpr int16_t kMastVoltKeyX = 151, kMastVoltX = 155, kMastAmpKeyX = 187, kMastAmpX = 192;   // value slots are 30 px

enum class Arrow : uint8_t { Idle, Up, Down };
enum class StatusReadout : uint8_t { None, Live, Stale };

// Fills the whole band, then the optional 12 px icon (x 4) and the S12 title. A title that would
// reach past x `right` is cut short with "..." instead of being overpainted by the right readout.
// A non-empty `badge` is drawn right-aligned to `right`, separated from the title by a 1 px rule.
// `readout_follows`: the caller repaints the right-hand readout area (x 140 and up, draw_top_readout) right after, so the band
// covers only x 0..139 and that area is written once, not twice.
void draw_status_left(const Surface &s, bool has_icon, Icon icon, std::string_view title, int16_t right = 132,
                      std::string_view badge = {}, bool readout_follows = false);

// The top chrome bar's role badge (draw_status_left's `badge`): a held Rd pin names its line
// ("Rd pin CC2"), so a leftover pin (pd-reread --pin) is visible on the status line; no pin is the
// passive inline role ("PAS"). pin = fusb302::pin_rd(): 0, 1 = CC1, 2 = CC2 (never both).
inline std::string_view role_badge(std::uint8_t pin, char (&buf)[11]) {
    if (pin == 1 || pin == 2) {
        constexpr std::string_view kTxt = "Rd pin CC";   // string_view: no NUL in the range
        std::size_t i = 0;
        for (char c : kTxt) buf[i++] = c;
        buf[i++] = static_cast<char>('0' + pin);
        buf[i] = '\0';
        return {buf, i};
    }
    constexpr std::string_view kPas = "PAS";
    for (std::size_t i = 0; i < kPas.size(); i++) buf[i] = kPas[i];
    buf[kPas.size()] = '\0';
    return kPas;
}

// Solid stale-grey pill right-aligned to x 222, 2 px side padding ("LAST", no counter).
void draw_last_pill(const Surface &s, std::string_view text);

// Right end of the top chrome bar: the compact readout, fmt3(V) x fmt3(A) in fixed slots right-aligned
// to x 218 (volts x152..182 in the volt colour, the multiplication sign at x 184, amps x 190..218 in
// the amp colour; the units live in the text, the colours are redundant). Stale: the frozen readout
// makes room for the stale-grey LAST pill (right-aligned to x 222) and hides. None: nothing.
// Live and not Idle also draws the direction: one static physical chevron (physical_chevron_row) in
// the slot x 140..149, centred on the text rows, in kFlow, one colour for both directions. Up points
// at the physical TOP port; `rotation` turns it with the screen, never reverses the measured flow.
// The call repaints the band rows of the right half (x 140..223); the hairline row is the title's.
void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps,
                      Arrow flow = Arrow::Idle, uint8_t rotation = 0, std::string_view last_text = "LAST");

// The Meter faces' pager in place of the readout: `count` 7 x 7 squares right-aligned to x 219, the current one solid ink,
// the others a 1 px ink outline. Repaints the band rows of the right half (x 140..223), like draw_top_readout.
constexpr int kPagerBox = 7, kPagerPitch = 11, kPagerRight = 220, kPagerY = 7;
void draw_top_pager(const Surface &s, int count, int current);

// 8 x 8 party markers of the PD match strip (the fonts have no such glyphs): 0 source (bolt),
// 1 cable (diameter sign), 2 sink / device (rectangle).
inline constexpr int kPartyGlyphs = 3, kPartySize = 8;
void draw_party_glyph(const Surface &s, int16_t x, int16_t y, int party, uint16_t color);

// Text that fits in `max_px` with font `f`: the text itself, or its start plus "..." written into `buf`.
// Characters are whole UTF-8 sequences; with no room for even the dots the result is as many dots as fit.
// This is the last-resort safety net for OUR copy: no on-screen text may be cut (docs/UI_DESIGN.md section 6), so
// host builds report every cut to the no-truncation gate (trunc.hpp) and the test that drove it fails. Constant
// copy is checked at compile time with fits_px / all_fit. External data (names, vendor strings) uses fit_data.
constexpr size_t kFitBuf = 48;
std::string_view fit_text(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DECL);
// The same cut for text that is not ours: a saved cable or card name, a vendor string, a user-typed value. It may be
// longer than any box, so the cut is by design; the gate only counts it.
std::string_view fit_data(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DECL);
// Same, but cut in the middle: the start, "..", then the last four characters (names such as card titles,
// whose differing part is usually the end). With too little room for start + dots + tail it is fit_text.
// Always data (the gate counts it, never fails on it).
std::string_view fit_text_mid(const Font &f, std::string_view text, int max_px, char (&buf)[kFitBuf] K2_TRUNC_DECL);

// ---- tool template: flow diagram column, checklist card, step tile ------------------------------
// FlowDiagram: a 64 x h card with a male port at the top, a female port at the bottom, a role pill
// beside each (K2, CBL, DEV, CHG: the role of the thing, never the port position), the chevron
// column between them and a caption mid-height. Offsets are relative to the card's top-left.
struct FlowGeom { int16_t top_port_y, bottom_port_y, role_top_y, role_bottom_y, line_y, line_h, caption_y; };
constexpr FlowGeom flow_geom(int16_t h) {
    const int16_t bottom = static_cast<int16_t>(h - 13);
    return {5, bottom, 2, static_cast<int16_t>(h - 16), 17, static_cast<int16_t>(bottom - 4 - 17),
            static_cast<int16_t>(h / 2 - 6)};
}
constexpr int flow_chevrons(int16_t h) { return flow_geom(h).line_h / kChevPitch; }
constexpr int16_t kFlowW = 64;

struct FlowSpec {
    std::string_view top_role, bottom_role;
    uint16_t top_color = 0, top_tint = 0, bottom_color = 0, bottom_tint = 0;
    Arrow flow = Arrow::Idle;            // Idle = a faint line, Up / Down = the chevron column
    std::string_view caption;            // S12, at x 30 beside the column
    uint16_t caption_color = 0;
};
void draw_flow_diagram(const Surface &s, int16_t x, int16_t y, int16_t h, const FlowSpec &f);

// Checklist: a card with a 3 px accent bar and one row per item: a check (met, dim text) or a cross
// in the accent colour (open, full text), B16 text. Rows are 22 px apart.
struct CheckItem { std::string_view text; bool met; };
constexpr int16_t checklist_icon_y(int i) { return static_cast<int16_t>(6 + 22 * i); }
constexpr int16_t checklist_text_y(int i) { return static_cast<int16_t>(4 + 22 * i); }
void draw_checklist(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, uint16_t accent,
                    const CheckItem *items, int n);

// StepCard tile: 22 x 22, r 5, a number on it; done = ok, to do = caution.
void draw_step_tile(const Surface &s, int16_t x, int16_t y, int number, bool done);

}  // namespace k2
