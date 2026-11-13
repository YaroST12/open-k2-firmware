// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Generic UI element backend: one signature-cache primitive every stateful widget is built on.
// The panel has no read-back, so "partial redraw" means skipping draw calls: an element paints
// only when the signature of its content changed since the last paint. Field has its own
// per-cell cache (gfx.hpp); everything else (icons, pills, rows, footers, chrome) goes through
// Elem here, so a perf optimization added in one place applies to every element.
#pragma once
#include <cstdint>
#include <string_view>
#include "k2/ui/gfx.hpp"
#include "k2/ui/ui_icons.hpp"

namespace k2 {

// FNV-1a helpers, the one shared definition (pages used to each carry their own copy).
constexpr uint32_t mix_init() { return 2166136261u; }
constexpr uint32_t mix_u32(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
inline uint32_t mix(uint32_t h, std::string_view s) {
    for (char ch : s) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    return (h ^ 0xFFu) * 16777619u;
}

// A cached element: paint(sig, fn) runs fn only when sig differs from the last painted one.
// invalidate() forces the next paint (e.g. after the surrounding area was erased or refilled).
struct Elem {
    uint32_t sig = 0;
    bool valid = false;
    bool needs(uint32_t s) const { return !valid || s != sig; }
    template <typename Fn>
    void paint(uint32_t s, Fn &&fn) {
        if (valid && s == sig) return;
        sig = s;
        valid = true;
        fn();
    }
    template <typename Bg, typename Fn>
    bool paint_budgeted(uint32_t s, Bg *bg, int cost, Fn &&fn) {
        if (valid && s == sig) return true;
        if (bg && !bg->admit(cost, true)) return false;
        sig = s;
        valid = true;
        fn();
        return true;
    }
    void invalidate() { valid = false; }
};

// A fixed-position 12 x 12 icon cell: repaints only when (icon, fg, bg) changes.
struct IconCell {
    Surface surf{};
    int16_t x = 0, y = 0;
    Elem e;
    void init(const Surface &s, int16_t x_, int16_t y_) {
        surf = s;
        x = x_;
        y = y_;
        e.invalidate();
    }
    void draw(Icon icon, uint16_t fg, uint16_t bg);   // ui_widgets.cpp
    void invalidate() { e.invalidate(); }
};

}  // namespace k2
