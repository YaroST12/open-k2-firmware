// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Home measurement cell compositor: a cell is a base colour, an optional patient-monitor sweep
// behind it, and an ordered list of items (fills, text, arrows, the warning triangle) drawn over it.
// Text blends its anti-aliased edge against the pixel underneath, so digits can sit on the sweep and
// survive a column passing under them. No framebuffer: one row of at most 224 pixels is composed on the
// stack and streamed. compose_row() is pure and host-tested (tests/test_home_cell.cpp); paint() streams
// through lcd:: (so the erase pass, the rotated-layout viewport and the host capture all see it).
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/home_model.hpp"

namespace k2::home {

// ---- items -----------------------------------------------------------------------------------
enum class ItemKind : uint8_t { Fill, Text, Arrow, Warn };

constexpr int kArrowBox = 22;     // flow arrow box, px
constexpr int kWarnBox = 11;      // warning triangle box, px
constexpr uint8_t kDashV = 4, kDashH = 5;   // Arrow `aux` values beyond the four directions

struct Item {
    ItemKind kind = ItemKind::Fill;
    uint8_t aux = 0;              // Arrow: 0 up, 1 left, 2 down, 3 right (screen), kDashV / kDashH for the idle dash
    int16_t x = 0, y = 0;         // Fill: top-left; Text: pen x and line-box top; Arrow/Warn: top-left of the box
    int16_t w = 0, h = 0;         // Fill size; Arrow: box size in w (0 = kArrowBox), the 22 px mask scaled to it
    uint16_t color = 0;           // Fill colour, Text foreground, Arrow colour, Warn triangle colour
    uint16_t ink = 0;             // Warn: the exclamation mark
    const Font *font = nullptr;
    std::string_view text;        // Text: points at storage the page keeps alive while the item is painted
};

constexpr Item fill(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
    Item i;
    i.kind = ItemKind::Fill; i.x = x; i.y = y; i.w = w; i.h = h; i.color = color;
    return i;
}
constexpr Item text(const Font &f, int16_t x, int16_t top, std::string_view s, uint16_t color) {
    Item i;
    i.kind = ItemKind::Text; i.x = x; i.y = top; i.font = &f; i.text = s; i.color = color;
    return i;
}
constexpr Item arrow(int16_t x, int16_t y, uint8_t aux, uint16_t color, int16_t size = 0) {
    Item i;
    i.kind = ItemKind::Arrow; i.x = x; i.y = y; i.aux = aux; i.color = color; i.w = size;
    return i;
}
constexpr int arrow_box(const Item &i) { return i.w > 0 ? i.w : kArrowBox; }
constexpr Item warn(int16_t x, int16_t y, uint16_t color, uint16_t ink) {
    Item i;
    i.kind = ItemKind::Warn; i.x = x; i.y = y; i.color = color; i.ink = ink;
    return i;
}

// ---- masks -----------------------------------------------------------------------------------
// Arrow direction on the screen from the physical flow: forward (toward the top port) points up at
// rotation 0; each quarter turn of the device turns the arrow with the ports.
constexpr uint8_t arrow_dir(bool toward_top, uint8_t rotation) { return static_cast<uint8_t>((rotation + (toward_top ? 0 : 2)) & 3); }
// The idle dash lies along the port axis.
constexpr uint8_t dash_dir(uint8_t rotation) { return (rotation & 1) ? kDashH : kDashV; }
// Is pixel (x, y) of the 22 x 22 arrow / dash box inked? `aux` as in Item.
bool arrow_hit(uint8_t aux, int x, int y);
// 0 = outside, 1 = triangle body, 2 = the exclamation mark, for pixel (x, y) of the 11 x 11 box.
int warn_hit(int x, int y);

// ---- sweep -----------------------------------------------------------------------------------
// Column heights (rows above the sweep bottom, 0 = no sample) of one cell and the cursor that wraps
// over them. The cursor is a thin line (1 px) in the column about to be written; the old trace stays
// beside it, nothing is blanked ahead of it. `n` columns are used (kCols for the hero, kLedgerCols for a ledger row).
struct Sweep {
    uint8_t rows[kCols] = {};
    uint8_t cursor = 0;
    uint8_t n = kCols;

    void clear() {
        for (auto &r : rows) r = 0;
        cursor = 0;
    }
    // Write `level` at the cursor column and advance. Returns the first column that changed: the old
    // cursor column; the strip to repaint runs from it to the new cursor column (wrapping).
    int step(uint8_t level) {
        const int first = cursor;
        rows[cursor] = level;
        cursor = static_cast<uint8_t>((cursor + 1) % n);
        return first;
    }
    // Place up to n levels, oldest first, from column 0; the cursor lands after them (wrapping to 0 when full).
    void load(std::span<const uint8_t> levels) {
        clear();
        const size_t k = levels.size() < static_cast<size_t>(n) ? levels.size() : static_cast<size_t>(n);
        for (size_t i = 0; i < k; i++) rows[i] = levels[i];
        cursor = static_cast<uint8_t>(k % n);
    }
};

// Pixel x ranges (inclusive) of the strip a step() touched, as one or two pieces when it wraps (`cols` = the sweep's n).
struct XRange { int16_t x0, x1; };
struct Strip { XRange r[2]; int n; };
constexpr Strip strip_of(int first_col, int steps = 1, int cols = kCols) {
    Strip s{{{0, 0}, {0, 0}}, 0};
    const int last = first_col + steps;   // the new cursor column (after `steps` steps in a row)
    if (last < cols) {
        s.r[0] = {x_of_col(first_col), static_cast<int16_t>(x_of_col(last) + 1)};
        s.n = 1;
    } else {
        s.r[0] = {x_of_col(first_col), static_cast<int16_t>(x_of_col(cols - 1) + 1)};
        s.r[1] = {x_of_col(0), static_cast<int16_t>(x_of_col(last - cols) + 1)};
        s.n = 2;
    }
    return s;
}

// ---- cell ------------------------------------------------------------------------------------
// A cell owns the rows geom.top..geom.bottom and the columns x0..x1 (the full width by default; a ledger row or a card
// cell is narrower). paint() never writes outside it.
struct Cell {
    CellGeom geom{};
    uint16_t bg = 0;                   // base colour of the cell
    bool sweep = false;                // draw the sweep (false: inverted over-limit cell, no trend)
    uint16_t tint = 0, line = 0, cursor_line = 0;
    const Sweep *sw = nullptr;
    std::span<const Item> items;
    int16_t x0 = 0, x1 = kW - 1;
};

// Pixel under the items at (x, y): base colour, or the sweep column state.
uint16_t bg_at(const Cell &c, int x, int y);
// Compose pixels x0..x1 of row y into out[0 .. x1-x0] (bg, then every item in order).
void compose_row(const Cell &c, int y, int x0, int x1, uint16_t *out);
// Stream the rectangle (inclusive, logical content coordinates) through lcd:: in ONE write: every
// intersecting item is composed back to front, so nothing is drawn under a glyph that the glyph does not
// cover again. Through a stretched (landscape) ContentViewport the rectangle is composed per panel pixel:
// the non-text items keep the stretch, the Text items are laid over at their native pixel size (native_rect
// origin, like gfx_text), so glyphs render at the same pixel size in every rotation.
void paint(const Cell &c, int x0, int y0, int x1, int y1);

// ---- digit field -----------------------------------------------------------------------------
// A right-aligned text of fixed width whose repaint is limited to the characters that changed.
struct DigitField {
    const Font *font = nullptr;
    int16_t x = 0, top = 0;            // left edge of the first cell, line-box top
    uint8_t chars = 0;                 // <= 8
    bool valid = false;
    char cur[9] = {};

    void init(const Font &f, int16_t x0, int16_t top_y, uint8_t n) {
        font = &f; x = x0; top = top_y; chars = n > 8 ? 8 : n; valid = false;
        for (auto &ch : cur) ch = ' ';
    }
    void invalidate() { valid = false; }
    int16_t right() const { return static_cast<int16_t>(x + chars * font->adv - 1); }
    std::string_view view() const { return {cur, chars}; }
    // Takes the new text (right-aligned, space-padded). Returns false when nothing needs a repaint;
    // otherwise [px0, px1] is the pixel x range of the changed characters (the whole field when invalid).
    bool set(std::string_view s, int16_t &px0, int16_t &px1) {
        char next[9];
        const size_t len = s.size() < chars ? s.size() : chars;
        for (size_t i = 0; i < chars; i++) next[i] = ' ';
        for (size_t i = 0; i < len; i++) next[chars - len + i] = s[i];
        int first = 0, last = chars - 1;
        if (valid) {
            while (first <= last && next[first] == cur[first]) first++;
            while (last >= first && next[last] == cur[last]) last--;
            if (first > last) return false;
        }
        for (int i = 0; i < chars; i++) cur[i] = next[i];
        valid = true;
        px0 = static_cast<int16_t>(x + first * font->adv);
        px1 = static_cast<int16_t>(x + (last + 1) * font->adv - 1);
        return true;
    }
};

}  // namespace k2::home
