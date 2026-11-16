// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The charger-test Running view's two stackings below the progress bar (y44..46), as constants the page and its test share.
//
//   Plain  : family y52, name y68 (x6), the 4-cell VBUS hero beside them (y54..83), the target line y88, the gauge y98, a rule,
//            the grid from y114.
//   Sweep  : the signed ERROR is the hero (user decision 2026-11-16): a sign in the 24 px font and up to three digits of the 48 px
//            font with their unit on the left, the act / set pair (24 px values, 12 px labels and unit) stacked on the right,
//            the name above them (the family label to its right), then the gauge, the rule and the grid.
//
// Why the boxes are listed with a `glyphs` flag: in the landscape rotations the 224 x 240 content is drawn into 240 x 224. A fill
// (gfx_fill, a bar, the gauge) is stretched, 14/15 in y; a glyph cell (gfx_text, Field) keeps its native height and only its origin
// moves. A text cell therefore ends mapped(y) + h, up to one row below the stretched end of the same box, and a neighbour that
// starts too close has its fill overwrite the cell's last row (the 3-decimal VBUS hero lost its bottom ink row under the
// set / err line this way, rot 1 and 3). A box below a text cell needs a gap of at least h / 14 rows; the test walks every pair
// of boxes in both mappings (tests/test_ui_sweep_live.cpp).
#pragma once
#include <cstdint>
#include "k2/ui/sweep_hero.hpp"

namespace k2::runlay {

struct RunLayout {
    int16_t name_y, hero_y, gauge_y, rule_y, grid_y;
    uint8_t hero_chars;
};
inline constexpr RunLayout kPlainRun{68, 54, 98, 112, 114, 4};
inline constexpr RunLayout kSweepRun{48, 72, 116, 130, 132, 3};

inline constexpr int16_t kBelowBar = 47;       // first row under the progress bar; a body wipe starts here
inline constexpr int16_t kPlainHeroRight = 196;

// Font cell metrics the geometry is written in (checked against the real fonts in the test).
inline constexpr int kH12 = 12, kA12 = 9, kAdv12 = 6;
inline constexpr int kH16 = 16, kA16 = 12, kAdv16 = 8;
inline constexpr int kH24 = 24, kA24 = 19, kAdv24 = 12;
inline constexpr int kH48 = 30, kA48 = 30, kAdv48 = 24;

// Sweep: the error hero (left), the act / set block (right).
inline constexpr int16_t kSweepFamX = 122;
inline constexpr int16_t kHeroDigitsRight = 90;                // right edge of the three digit cells (x18..89)
inline constexpr int16_t kHeroUnitX = kHeroDigitsRight + 3;    // "mV" / "V", 24 px font, on the digits' baseline
inline constexpr int16_t kSignGap = 3;                         // the sign's right edge stands this far left of the first digit's cell (landscape drift)
inline constexpr int16_t kSignDy = 4;                          // the sign cell's top below the hero's, so a '+' sits at the digits' middle
inline constexpr int16_t kActY = 65, kSetY = 90;               // tops of the 24 px value cells
inline constexpr int16_t kBlockLabelX = 120, kBlockValueX = 144, kBlockValueChars = 6;
inline constexpr int16_t kBlockUnitX = kBlockValueX + kBlockValueChars * kAdv24;   // "V", 12 px, on the value's baseline

// One painted box in logical coordinates. `glyphs`: text cells (native height in every rotation); else fills (stretched).
struct Box {
    const char* name;
    int16_t x, y, w, h;
    bool glyphs;
    int16_t dx = 0;   // glyph cells inside a Field: native pixels from the mapped origin of the Field's own x (a Field lays its cells out natively)
};

// A sweep row's boxes in plan order, but for the error hero's digit cells and its sign, which depend on the digit count
// (digits_box / sign_box). Row labels sit on the value's baseline.
inline constexpr Box kSweepBoxes[] = {
    {"progress", 6, 44, 212, 3, false},
    {"name", 6, kSweepRun.name_y, 11 * kAdv16, kH16, true},
    {"family", kSweepFamX, kSweepRun.name_y + kA16 - kA12, 16 * kAdv12, kH12, true},
    {"hero unit", kHeroUnitX, kSweepRun.hero_y + kH48 - kA24, 2 * kAdv24, kH24, true},
    {"act label", kBlockLabelX, kActY + kA24 - kA12, 3 * kAdv12, kH12, true},
    {"act value", kBlockValueX, kActY, kBlockValueChars * kAdv24, kH24, true},
    {"act unit", kBlockUnitX, kActY + kA24 - kA12, kAdv12, kH12, true},
    {"set label", kBlockLabelX, kSetY + kA24 - kA12, 3 * kAdv12, kH12, true},
    {"set value", kBlockValueX, kSetY, kBlockValueChars * kAdv24, kH24, true},
    {"set unit", kBlockUnitX, kSetY + kA24 - kA12, kAdv12, kH12, true},
    {"gauge", 6, kSweepRun.gauge_y, 210, 12, false},
    {"rule", 0, kSweepRun.rule_y, 224, 2, false},
    {"grid", 0, kSweepRun.grid_y, 224, 222 - kSweepRun.grid_y, false},
};
inline constexpr int kSweepBoxCount = sizeof(kSweepBoxes) / sizeof(kSweepBoxes[0]);

// The sign cell of an error of `n` digits (1..3).
constexpr Box sign_box(int n) {
    return {"sign", int16_t(sweep_hero::sign_x(kHeroDigitsRight, kAdv48, kAdv24, n, kSignGap)), int16_t(kSweepRun.hero_y + kSignDy), kAdv24, kH24, true};
}
// The digit cells an error of n digits fills (the rest of the three-cell box stays ground); n = 3 is the whole Field box, which a
// full repaint (a colour change) blanks end to end.
constexpr Box digits_box(int n) {
    return {"digits", int16_t(kHeroDigitsRight - 3 * kAdv48), kSweepRun.hero_y, int16_t(n * kAdv48), kH48, true, int16_t((3 - n) * kAdv48)};
}

// What a box really covers on the panel when the 224 x 240 content is drawn into ext_w x ext_h (the same arithmetic as
// ContentViewport: mapped(c) = c * extent / logical). A fill is stretched, a text cell keeps its native size and moves only its origin.
struct Painted { int x0, y0, x1, y1; };
constexpr Painted painted(const Box& b, int ext_w, int ext_h) {
    const int x0 = b.x * ext_w / 224, y0 = b.y * ext_h / 240;
    if (b.glyphs) return {x0 + b.dx, y0, x0 + b.dx + b.w, y0 + b.h};
    return {x0, y0, (b.x + b.w) * ext_w / 224, (b.y + b.h) * ext_h / 240};
}
constexpr bool overlap(const Painted& a, const Painted& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
constexpr bool overlap(const Box& a, const Box& b, int ext_w, int ext_h) { return overlap(painted(a, ext_w, ext_h), painted(b, ext_w, ext_h)); }

// The sweep row with an error of n digits on screen (n = 1..3: the digit cells and the sign; n = 0: the whole digit box, no sign)
// paints no two boxes over one another, in a content mapped to ext_w x ext_h.
constexpr bool sweep_clear(int n, int ext_w, int ext_h) {
    Box v[kSweepBoxCount + 2] = {};
    int m = 0;
    for (const Box& b : kSweepBoxes) v[m++] = b;
    v[m++] = digits_box(n == 0 ? 3 : n);
    if (n > 0) v[m++] = sign_box(n);
    for (int i = 0; i < m; ++i)
        for (int j = i + 1; j < m; ++j)
            if (overlap(v[i], v[j], ext_w, ext_h)) return false;
    return true;
}
// Portrait (rot 0, 2) is the identity; landscape (rot 1, 3) maps to 240 x 224 and is where a text cell outgrows the stretched box under it.
static_assert(sweep_clear(0, 224, 240) && sweep_clear(1, 224, 240) && sweep_clear(2, 224, 240) && sweep_clear(3, 224, 240),
              "sweep Running: two boxes overlap in portrait");
static_assert(sweep_clear(0, 240, 224) && sweep_clear(1, 240, 224) && sweep_clear(2, 240, 224) && sweep_clear(3, 240, 224),
              "sweep Running: two boxes overlap in landscape");

}  // namespace k2::runlay
