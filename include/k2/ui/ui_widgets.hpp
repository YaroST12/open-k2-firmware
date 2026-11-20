// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// UI design v2 primitives: rounded fills and frames, pills, gauges, chevrons, port symbols,
// icons and sparklines. Everything is drawn row by row through the strip renderer (no
// framebuffer). Geometry that does not touch the panel (corner insets, pill width, gauge fill,
// chevron rows, sparkline cells) is a constexpr/pure function so host tests can pin it down.
#pragma once
#include <cstdint>
#include <string_view>
#include "k2/core/math_opt.hpp"
#include "k2/ui/gfx.hpp"
#include "k2/ui/ui_elem.hpp"
#include "k2/ui/ui_icons.hpp"
#include "k2/ui/ui_labels.hpp"
#include "k2/ui/ui_style.hpp"

namespace k2 {

// ---- pure geometry ---------------------------------------------------------------------------
constexpr int kPillH = 14;
constexpr int pill_width(int chars) { return chars * static_cast<int>(kLabelAdv) + 8; }

// Vertical chevron, 10 x 6: row k covers [l, l+2) and [8-l, 10-l) with l = kChevL[k]. The "up"
// shape lists rows top to bottom, "down" mirrors them.
constexpr int kChevW = 10, kChevH = 6;
constexpr uint8_t kChevL[kChevH] = {4, 3, 2, 1, 0, 0};
constexpr int chevron_left(bool up, int row) { return kChevL[up ? row : kChevH - 1 - row]; }

// The same chevron turned with the screen (rotation 0..3): it points at the
// physical TOP port when toward_top, at the BOTTOM port otherwise. Quarter turns are 6 wide x 10 tall.
// physical_chevron_row() is row `row` as a bit mask, bit c = column c from the left; the status-bar
// band writes it row by row.
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

// Sparkline cell classification. `level` is the height level of the row (h-1 = top row),
// `v` the sample height (0..h-1), `prev` the previous sample.
enum class Cell : uint8_t { None, Fill, Line };
constexpr Cell spark_cell(int level, int v, int prev) {
    const int lo = v < prev ? v : prev, hi = v < prev ? prev : v;
    if (level >= lo && level <= hi) return Cell::Line;
    return level < v ? Cell::Fill : Cell::None;
}

// ---- drawing (coordinates relative to the surface, like gfx_fill) -----------------------------
void draw_frame(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, int t, uint16_t color);

// 12 x 12 icon; set bits take fg, clear bits bg (opaque).
void draw_icon(const Surface &s, int16_t x, int16_t y, Icon icon, uint16_t fg, uint16_t bg);

// Anti-aliased vector check / cross, a square of `size` px (the font's cap height) at (x, y), fg over bg.
void draw_mark(const Surface &s, int16_t x, int16_t y, int size, bool check, uint16_t fg, uint16_t bg);
// Text where kMarkCheck / kMarkCross draw a green check / red cross sitting on the baseline. Returns the end x.
constexpr int mark_size(const Font &f) { return f.ascent * 3 / 4; }
constexpr int mark_cell(const Font &f) { return mark_size(f) + 4; }   // 2 px lead, mark, 2 px trail
int draw_marked_text(const Surface &s, const Font &f, int16_t x, int16_t y, std::string_view text, uint16_t fg, uint16_t bg);
int marked_width(const Font &f, std::string_view text);

// Shared single-pass S12 label-in-box painter (chips, LAST badge).
void draw_label_box(const Surface &s, int16_t x, int16_t y, int16_t w, int16_t h, std::string_view text,
                    uint16_t fg, uint16_t bg);

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

// Option B (Footer-Less + Floating Toast): no permanent bottom footer reservation (kFooterY = 240).
// draw_footer() remains available for unit tests / legacy callers (hugs s.h - 18); transient notifications use draw_toast().
constexpr int16_t kFooterY = 240;
constexpr int16_t kFooterTextX = 8;
constexpr int16_t kFooterBarW = 4;   // the tone bar at the left edge of an Ok / Caution fact line

// The most cells a footer / toast line holds across 208 px (25 cells of 8 px S16, or 29 cells of 6 px S12).
constexpr int kFooterCells = 25;
constexpr int kFooterTextPx = kFooterCells * static_cast<int>(kSmallAdvPx);   // 200

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

struct PaintBudget;

void draw_footer(const Surface &s, FooterTone tone, std::string_view text K2_TRUNC_DECL);
// Whether the footer drawn last (draw_footer, whichever variant reached it) was an Error one. The idle power logic reads it
// so a screen showing an error is never switched off; Ui clears it with every page switch or full clear, the incoming
// page's own footer then sets it again.
bool footer_error_shown();
void footer_error_clear();
// Budget-gated version of draw_footer_cached: returns false and leaves `e` invalid/old if `bg` refuses.
bool draw_footer_budgeted(const Surface &s, FooterTone tone, std::string_view text, Elem &e, PaintBudget *bg K2_TRUNC_DECL);
// Same as draw_footer, but paints through the element's cache: skips the repaint when the
// (tone, text) signature is unchanged since the last call with this `e` slot.
// e.invalidate() forces a repaint.
inline void draw_footer_cached(const Surface &s, FooterTone tone, std::string_view text, Elem &e K2_TRUNC_DECL) {
    draw_footer_budgeted(s, tone, text, e, nullptr K2_TRUNC_ARG);
}

// ---- Floating transient Toast pill (Option B: 16 px pill, bottom-center overlay, ~2.5 s auto-expiry) ----
constexpr int16_t kToastH = 16;
constexpr int16_t kToastMarginBottom = 4;
constexpr uint32_t kToastDefaultMs = 2500;
constexpr int kToastCells = (kTwinContentW - 24) / static_cast<int>(kLabelAdv);   // 30 cells of font 12 in 208 px
constexpr int kToastTextPx = kToastCells * static_cast<int>(kLabelAdv);           // 180 px

struct Toast {
    char text[32] = {};
    uint8_t len = 0;
    FooterTone tone = FooterTone::Info;
    uint32_t until_ms = 0;
    bool drawn = false;
    bool erase_pending = false;

    void show(FooterTone t, std::string_view msg, uint32_t now_ms, uint32_t dwell_ms = kToastDefaultMs) {
        const bool changed = t != tone || msg != view();
        erase_pending = erase_pending || (drawn && changed);
        tone = t;
        len = static_cast<uint8_t>(msg.size() < sizeof(text) - 1 ? msg.size() : sizeof(text) - 1);
        for (uint8_t i = 0; i < len; i++) text[i] = msg[i];
        text[len] = '\0';
        until_ms = now_ms + dwell_ms;
        if (until_ms == 0) until_ms = 1;
        drawn = false;
    }
    void clear() { erase_pending = erase_pending || drawn; len = 0; text[0] = '\0'; until_ms = 0; drawn = false; }
    bool active(uint32_t now_ms) const {
        return until_ms != 0 && len > 0 && static_cast<int32_t>(now_ms - until_ms) < 0;
    }
    bool expired(uint32_t now_ms) const {
        return until_ms != 0 && static_cast<int32_t>(now_ms - until_ms) >= 0;
    }
    std::string_view view() const { return {text, len}; }
};

// Draws the 16 px floating toast pill centred near the bottom of `s` (s.h - kToastH - kToastMarginBottom).
// Single-pass fill + 1 px tone border + S12 text; sets footer_error_shown() when tone == FooterTone::Error.
void draw_toast(const Surface &s, FooterTone tone, std::string_view text K2_TRUNC_DECL);

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
                      std::string_view badge = {}, bool readout_follows = false, uint16_t badge_color = kInk2);

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

// What the live readout shows, kept by its caller so the next live tick repaints only the character cells that changed
// (a reading that drifts by one digit sends ~2 x 72 px instead of the 84 x 20 band). The slots are 5 cells of kLabelAdv,
// right-aligned to match the masthead's fixed-edge digits; the 3 px colour keys sit outside the value slots.
struct ReadoutShown {
    static constexpr int kCells = 5, kChevX = 140, kVoltX = kMastVoltX, kAmpX = kMastAmpX;
    bool valid = false;   // false: unknown (a band repaint, a capture), the next draw repaints the whole band
    StatusReadout mode = StatusReadout::None;
    Arrow flow = Arrow::Idle;
    uint8_t rotation = 0;
    char volts[kCells] = {}, amps[kCells] = {};
    // Sets the slot right-aligned from the drawn text; false when it does not fit 5 single-byte cells.
    static constexpr bool set(char (&slot)[kCells], std::string_view text) {
        bool ok = text.size() <= kCells;
        const int n = static_cast<int>(text.size());
        const int pad = kCells - (n < kCells ? n : kCells);
        for (int i = 0; i < kCells; i++) {
            const char c = i >= pad && (i - pad) < n ? text[static_cast<std::size_t>(i - pad)] : ' ';
            if (static_cast<unsigned char>(c) >= 0x80) ok = false;
            slot[i] = c;
        }
        return ok;
    }
};
// One logical x range [x0, x1) of the readout band to repaint.
struct ReadoutSpan { int16_t x0 = 0, x1 = 0; };
// The spans that differ between `was` and `now`, both live in the same rotation: the chevron slot when the flow changed,
// then the changed cells of each slot (first to last differing cell). n = 0: nothing to send.
struct ReadoutDelta { ReadoutSpan span[3]; int n = 0; };
constexpr ReadoutDelta readout_delta(const ReadoutShown &was, const ReadoutShown &now) {
    ReadoutDelta d;
    if (was.flow != now.flow) d.span[d.n++] = {ReadoutShown::kChevX, static_cast<int16_t>(ReadoutShown::kChevX + kChevW)};
    const auto cells = [&](const char (&a)[ReadoutShown::kCells], const char (&b)[ReadoutShown::kCells], int x) {
        int first = 0, last = ReadoutShown::kCells - 1;
        while (first <= last && a[first] == b[first]) first++;
        while (last >= first && a[last] == b[last]) last--;
        if (first <= last)
            d.span[d.n++] = {static_cast<int16_t>(x + first * static_cast<int>(kLabelAdv)),
                             static_cast<int16_t>(x + (last + 1) * static_cast<int>(kLabelAdv))};
    };
    cells(was.volts, now.volts, ReadoutShown::kVoltX);
    cells(was.amps, now.amps, ReadoutShown::kAmpX);
    return d;
}
void draw_top_readout(const Surface &s, StatusReadout mode, std::string_view volts, std::string_view amps, Arrow flow,
                      uint8_t rotation, ReadoutShown &shown);

// The Meter faces' pager in place of the readout: `count` 7 x 7 squares right-aligned to x 219, the current one solid ink,
// the others a 1 px ink outline. Repaints the band rows of the right half (x 140..223), like draw_top_readout.
constexpr int kPagerBox = 7, kPagerPitch = 11, kPagerRight = 220, kPagerY = 7;
void draw_top_pager(const Surface &s, int count, int current);

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

}  // namespace k2
