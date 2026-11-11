// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// A reusable vertical list: fixed row height, a row-draw callback, a total count and a scroll
// offset. Scrolling clamps at both ends (no wrap) and redraws only the list's own rows: the row
// background is repainted only for slots whose highlight changed (plus empty tail slots), the
// rows themselves repaint only what their fixed-width fields changed. A thin scrollbar shows
// only while the list overflows; its thumb eases towards the target position.
//
// ListState is the pure part (offset arithmetic, thumb geometry); ListView adds the drawing.
#pragma once
#include <cstdint>
#include "k2/ui/gfx.hpp"

namespace k2 {

struct ListState {
    int count = 0;
    int visible = 1;     // rows that fit in the list rect
    int offset = 0;      // index of the first shown row

    bool overflows() const { return count > visible; }
    int max_offset() const { return overflows() ? count - visible : 0; }
    int first() const { return offset; }
    int end() const { return offset + visible < count ? offset + visible : count; }   // one past last shown

    // Returns true when the offset changed (a shrinking list can pull it back).
    bool set_count(int n) {
        count = n < 0 ? 0 : n;
        return clamp_offset();
    }
    // Moves by `delta` rows, clamped at the ends. True when the offset changed.
    bool scroll(int delta) {
        const int before = offset;
        offset += delta;
        clamp_offset();
        return offset != before;
    }
    // Scrollbar thumb inside a track `track_h` px tall: start and length.
    void thumb(int track_h, int &pos, int &len) const {
        constexpr int kMinLen = 6;
        if (!overflows()) { pos = 0; len = track_h; return; }
        len = track_h * visible / count;
        if (len < kMinLen) len = kMinLen;
        if (len > track_h) len = track_h;
        pos = (track_h - len) * offset / max_offset();
    }

private:
    bool clamp_offset() {
        const int before = offset;
        if (offset > max_offset()) offset = max_offset();
        if (offset < 0) offset = 0;
        return offset != before;
    }
};

class ListView {
public:
    static constexpr int kMaxRows = 10;
    static constexpr int kBarW = 3;   // scrollbar width: the right-most px of the list surface
    // Draws row `index` into `f`, which sits at `r` (surface coordinates) and is already
    // initialised for the row. `highlighted` marks the highlight row (-1 = none by default).
    // The row background is NOT painted here: ListView fills a slot's whole row with kBg only
    // when its highlight state changed (or the slot is empty), so the callback must fully
    // repaint any element whose size, colour or presence can change (fixed-width Fields do).
    using DrawRow = void (*)(void *ctx, int index, const Rect &r, bool highlighted, Field &f);

    // The list occupies (x, y) .. rows * row_h tall inside `s`; text fields are `chars` wide in
    // `font`. The scrollbar takes the right-most 3 px of the surface.
    void init(const Surface &s, int16_t x, int16_t y, uint8_t chars, int16_t row_h, int rows,
              const Font &font, DrawRow fn, void *ctx);

    // Optional content signature of row `index`: when set, draw() asks it for every shown row and
    // repaints a row whose signature changed while its index and highlight stayed (its base row
    // is refilled with kBg, then the callback runs once). Rows whose signature is unchanged are
    // not repainted, so a page can keep a live list (step verdicts, toggles) flicker-free without
    // wiping the body. Without it a row repaints only when its index or highlight changes.
    using RowSig = uint32_t (*)(void *ctx, int index);
    void set_row_sig(RowSig fn, void *ctx) { sig_fn_ = fn; sig_ctx_ = ctx; }

    // Opaque rows (optional): every repaint of a slot, whatever caused it (index, highlight or
    // signature), first fills the slot's row with `bg` (`hi_bg` when highlighted), so the callback
    // paints on a clean base and needs no clearing of its own. Without it the base is refilled
    // with kBg only when the highlight changed (the default every existing list relies on).
    // `rule` (optional, >= 0) is a one-pixel line the list draws at the top of every opaque row, and the base fill then
    // starts one pixel lower: the rows no longer write that line twice (once as base, once as the page's hairline).
    void set_row_bg(uint16_t bg, uint16_t hi_bg, int32_t rule = -1) { opaque_ = true; bg_ = bg; hi_bg_ = hi_bg; rule_ = rule; }
    void clear_row_bg() { opaque_ = false; rule_ = -1; }   // rows paint their own base again (a page that switches between two looks)

    // Expanding focus (the kit's RowList): the highlighted row is `px` taller than the others and the rows below it
    // shift down by `px`. The list rect is rows * row_h + px tall, whichever row has the focus, so a focus move only
    // repaints the rows whose position or highlight changed; the part of the rect no row covers (the highlight is outside
    // the window) is wiped once. Reset to 0 by init().
    void set_focus_extra(int16_t px) { extra_ = px; }
    // Full-width lane: while the list does not overflow the row base runs under the scrollbar lane too (no notch beside
    // a focused row, and the page needs no lane painter); an overflowing list keeps the lane for its thumb. Reset by init().
    void set_full_lane(bool on) { full_lane_ = on; }
    // Self-grounded rows (opaque rows only): the list draws the rule line but not the base under the row; the callback paints
    // every pixel of the row below the rule, ground included, each once (kit::fill_except around its text and marker boxes).
    // Reset by init().
    void set_self_ground(bool on) { self_ground_ = on; }
    // The width a row spans right now: the surface, less the scrollbar lane while the list overflows (or has no full lane).
    int16_t row_width() const { return static_cast<int16_t>(surf_.w - (full_lane_ && !st_.overflows() ? 0 : ListView::kBarW)); }
    // Moves the window to `offset` (clamped) without drawing: the caller's next draw() paints it once.
    void set_offset(int offset) { st_.scroll(offset - st_.offset); }

    // Sets the total row count; redraws when the visible rows changed.
    // `redraw` = false leaves the drawing to the caller's next draw() / scroll() (a page that is about to re-scroll the
    // view would otherwise paint every row twice, once at the stale offset).
    void set_count(int n, bool redraw = true);
    // Scrolls by `delta` rows and redraws the rows. False (and no drawing) when clamped.
    // `now_ms` (0 = none) drives the scrollbar thumb easing.
    bool scroll(int delta, uint32_t now_ms = 0);
    void set_highlight(int index) { highlight_ = index; }
    // Draws every visible row (unchanged characters are skipped) and the scrollbar.
    void draw(uint32_t now_ms = 0);
    // Forget what is on screen (after a capture pass or a repaint of the surrounding page).
    void invalidate();

    const ListState &state() const { return st_; }

private:
    void draw_bar(uint32_t now_ms);

    Surface surf_{};
    int16_t x_ = 0, y_ = 0, row_h_ = 0;
    DrawRow fn_ = nullptr;
    void *ctx_ = nullptr;
    int highlight_ = -1;
    ListState st_;
    Field row_[kMaxRows];
    // Per-slot paint cache: state_ = highlight the slot's background was last painted for
    // (-1 = never), last_index_ = row index the slot last showed. A slot whose (index, hi)
    // pair is unchanged is not repainted at all; the base row fill (kBg) happens only when
    // the highlight state changes.
    int8_t state_[kMaxRows] = {};
    int last_index_[kMaxRows] = {};
    uint32_t sig_[kMaxRows] = {};     // row signature each slot was last painted with
    RowSig sig_fn_ = nullptr;
    void *sig_ctx_ = nullptr;
    bool opaque_ = false;
    uint16_t bg_ = 0, hi_bg_ = 0;
    int32_t rule_ = -1;
    int16_t extra_ = 0;               // focus expansion (set_focus_extra)
    bool full_lane_ = false;
    bool self_ground_ = false;
    int16_t slot_y_[kMaxRows] = {};   // y each slot was last painted at (-1 = never): a shift repaints
    int16_t tail_used_ = -1;          // list height the rows covered at the last draw (-1 = unknown)
    // What the scrollbar currently shows, to redraw it only when it changes.
    bool bar_drawn_ = false;
    int bar_count_ = -1, bar_offset_ = -1;
    // Thumb easing: displayed position animates towards the target over kThumbAnimMs.
    int bar_pos_ = -1, bar_target_ = -1, bar_drawn_pos_ = -1, bar_anim_from_ = 0;
    uint32_t bar_anim_start_ = 0;
    static constexpr uint32_t kThumbAnimMs = 160;
    uint8_t chars_ = 0;
    int16_t text_dy_ = 0;
};

}  // namespace k2
