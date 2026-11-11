// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
#include "k2/ui/list_view.hpp"

namespace k2 {

namespace {
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kTrack = 0x2104;
constexpr uint16_t kThumb = 0x8410;
}

void ListView::init(const Surface &s, int16_t x, int16_t y, uint8_t chars, int16_t row_h, int rows,
                    const Font &font, DrawRow fn, void *ctx) {
    surf_ = s; x_ = x; y_ = y; row_h_ = row_h; fn_ = fn; ctx_ = ctx; chars_ = chars;
    if (rows > kMaxRows) rows = kMaxRows;
    // The count and the scroll offset survive a re-init, so a repaint of the page (a screenshot
    // pass, a return to the page) keeps the list where it was.
    st_.visible = rows;
    st_.set_count(st_.count);
    highlight_ = -1;
    extra_ = 0; full_lane_ = false; self_ground_ = false; tail_used_ = -1;
    text_dy_ = static_cast<int16_t>((row_h - font.height) / 2);
    for (int i = 0; i < rows; i++) {
        row_[i].init(s, font, x, static_cast<int16_t>(y + i * row_h + text_dy_), chars, Align::Left, 0xFFFF, kBg);
        state_[i] = -1; last_index_[i] = -1; sig_[i] = 0; slot_y_[i] = -1;   // first draw paints every row
    }
    // The thumb's animation state survives a re-init (like the count and offset): a capture
    // pass at the same instant must render the same thumb position as the live screen.
    bar_drawn_ = false; bar_count_ = bar_offset_ = -1;
}

void ListView::invalidate() {
    for (int i = 0; i < st_.visible; i++) { row_[i].invalidate(); state_[i] = -1; last_index_[i] = -1; sig_[i] = 0; slot_y_[i] = -1; }
    tail_used_ = -1;
    bar_drawn_ = false; bar_count_ = bar_offset_ = -1;   // force the bar repaint; keep the thumb position
}

void ListView::set_count(int n, bool redraw) {
    if (n == st_.count) return;
    st_.set_count(n);
    if (redraw) draw();
}

bool ListView::scroll(int delta, uint32_t now_ms) {
    if (!st_.scroll(delta)) return false;
    draw(now_ms);
    return true;
}

void ListView::draw(uint32_t now_ms) {
    // Full-lane rows ran under the lane while the list did not overflow; a list that just started to overflow
    // paints its thumb over them (draw_bar), one that stopped must give the lane back to every row.
    if (full_lane_ && bar_drawn_ && !st_.overflows())
        for (int slot = 0; slot < st_.visible; slot++) state_[slot] = -1;
    // Rows span the surface (minus the lane while a thumb needs it); the focus expansion shifts the rows below it.
    const int16_t rw = static_cast<int16_t>(surf_.w - (full_lane_ && !st_.overflows() ? 0 : ListView::kBarW));
    const int hi_slot = highlight_ >= st_.first() && highlight_ < st_.count && highlight_ < st_.first() + st_.visible
                            ? highlight_ - st_.first() : -1;
    for (int slot = 0; slot < st_.visible; slot++) {
        const int index = st_.first() + slot;
        const int shown = index < st_.count ? index : -1;   // -1 = empty tail slot
        const bool hi = shown >= 0 && shown == highlight_;
        const int16_t sy = static_cast<int16_t>(y_ + slot * row_h_ + (extra_ && hi_slot >= 0 && slot > hi_slot ? extra_ : 0));
        const int16_t sh = static_cast<int16_t>(row_h_ + (hi ? extra_ : 0));
        const uint32_t sig = sig_fn_ && shown >= 0 ? sig_fn_(sig_ctx_, shown) : 0;
        const bool same_row = state_[slot] == (hi ? 1 : 0) && last_index_[slot] == shown && slot_y_[slot] == sy;
        if (same_row && sig_[slot] == sig)
            continue;   // this slot already shows exactly this state: nothing to repaint
        slot_y_[slot] = sy;
        if (shown < 0) {
            // Became empty: wipe the whole row once, the previous content must not stay. While
            // it stays empty, skip (the page may paint other content over the tail area).
            gfx_fill(surf_, 0, sy, rw, row_h_, kBg);
            state_[slot] = 0;
            last_index_[slot] = -1;
            sig_[slot] = 0;
            // The field is only forgotten, not drawn blank: its cell sits at the slot's unshifted position and can lie
            // under an open (focus extra) row above, which a blank cell write would cut short.
            row_[slot].set_colors(0xFFFF, kBg);
            row_[slot].invalidate();
            continue;
        }
        if (opaque_ || state_[slot] != (hi ? 1 : 0) || same_row) {
            // The slot's background state changed (or its content did while the row stayed, which
            // the callback cannot be trusted to erase): repaint the base row (kBg); a highlighted
            // slot paints its own tint over this in the callback. The field cache must forget
            // the old text too, or its unchanged cells would not be repainted over the new base.
            // Opaque rows (set_row_bg) refill on every repaint, in the row's own colours.
            if (opaque_ && rule_ >= 0) {   // the rule line first, the base below it: no pixel is written twice
                gfx_fill(surf_, 0, sy, rw, 1, static_cast<uint16_t>(rule_));
                if (!self_ground_) gfx_fill(surf_, 0, static_cast<int16_t>(sy + 1), rw, static_cast<int16_t>(sh - 1), hi ? hi_bg_ : bg_);
            } else {
                gfx_fill(surf_, 0, sy, rw, sh, opaque_ ? (hi ? hi_bg_ : bg_) : kBg);
            }
            row_[slot].invalidate();
        }
        state_[slot] = hi ? 1 : 0;
        last_index_[slot] = shown;
        sig_[slot] = sig;
        const Rect r{x_, sy, row_[slot].width_px(), sh};
        fn_(ctx_, index, r, hi, row_[slot]);
    }
    if (extra_) {
        // The rect is rows * row_h + extra tall; with no highlighted row in the window the last `extra_` px belong to
        // nobody and are wiped once (the rows themselves tile the rest).
        const int16_t used = static_cast<int16_t>(st_.visible * row_h_ + (hi_slot >= 0 ? extra_ : 0));
        if (used != tail_used_) {
            if (hi_slot < 0) gfx_fill(surf_, 0, static_cast<int16_t>(y_ + used), rw, extra_, kBg);
            tail_used_ = used;
        }
    }
    draw_bar(now_ms);
}

void ListView::draw_bar(uint32_t now_ms) {
    const int16_t bx = static_cast<int16_t>(surf_.w - ListView::kBarW);
    const int16_t track_h = static_cast<int16_t>(st_.visible * row_h_ + extra_);
    if (!st_.overflows()) {
        if (!full_lane_ && (bar_drawn_ || bar_count_ < 0)) gfx_fill(surf_, bx, y_, ListView::kBarW, track_h, kBg);
        bar_drawn_ = false; bar_count_ = st_.count; bar_offset_ = st_.offset;
        bar_pos_ = bar_target_ = bar_drawn_pos_ = -1; bar_anim_start_ = 0;
        return;
    }
    int pos, len;
    st_.thumb(track_h, pos, len);
    // Ease the thumb towards its target so a scroll reads as a glide, not a jump. now_ms == 0
    // (no clock at this call site) jumps straight to the target.
    // The thumb state survives a re-init, so after a page re-lays the list on a shorter track (the charger wizard's
    // Results look) the old position can lie below the new track; it would paint over whatever is under the list.
    if (bar_pos_ > track_h - len) bar_pos_ = track_h - len;
    if (bar_anim_from_ > track_h - len) bar_anim_from_ = track_h - len;
    if (bar_pos_ < 0 || now_ms == 0) {
        bar_pos_ = bar_target_ = pos;
        bar_anim_start_ = 0;
    } else if (pos != bar_target_) {
        bar_anim_from_ = bar_pos_;
        bar_target_ = pos;
        bar_anim_start_ = now_ms;
    }
    if (bar_anim_start_) {
        const uint32_t el = now_ms - bar_anim_start_;
        if (el >= kThumbAnimMs) { bar_pos_ = bar_target_; bar_anim_start_ = 0; }
        else bar_pos_ = bar_anim_from_ + (bar_target_ - bar_anim_from_) * static_cast<int>(el) /
                                     static_cast<int>(kThumbAnimMs);
    }
    if (bar_drawn_ && bar_count_ == st_.count && bar_offset_ == st_.offset &&
        bar_pos_ == bar_drawn_pos_ && !bar_anim_start_)
        return;   // settled and unchanged: the thumb is already on screen
    gfx_fill(surf_, bx, y_, ListView::kBarW, track_h, kTrack);
    gfx_fill(surf_, bx, static_cast<int16_t>(y_ + bar_pos_), ListView::kBarW, static_cast<int16_t>(len), kThumb);
    bar_drawn_ = true; bar_count_ = st_.count; bar_offset_ = st_.offset; bar_drawn_pos_ = bar_pos_;
}

}  // namespace k2
