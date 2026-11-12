// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Palette, fonts and panel geometry shared by the Ui and its pages.
#pragma once
#include <cstdint>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/font_aa.hpp"
#include "k2/ui/ui_labels.hpp"

namespace k2 {

// RGB565 palette (UI design v2 section 3.3; tests/test_ui_tokens.py checks it against the mockup
// renderer's table). Dark background, one distinct colour per measured quantity. Each hue has a
// tint (about 15 % of the hue on black) for pill, tile and selected-row fills.
constexpr uint16_t kBg = 0x0000;
constexpr uint16_t kSurface = 0x18E3;   // cards, flow-diagram panel
constexpr uint16_t kSurface2 = 0x2124;  // overlays (HoldCard)
constexpr uint16_t kBar = 0x08A9;       // title / status bar
constexpr uint16_t kRule = 0x2945;      // dividers, gauge tracks, sparkline grid
constexpr uint16_t kText = 0xFFFF;
constexpr uint16_t kDim = 0x8410;
constexpr uint16_t kFaint = 0x4208;     // "no data"
constexpr uint16_t kVolt = 0xFFE0;      // yellow: V, CC pill, VBUS trace
constexpr uint16_t kAmp = 0x07FF;       // cyan: A, FIX PDO, protocol pills
constexpr uint16_t kWatt = 0xFB2C;      // coral: W, PPS PDO, W bars only (never a warning colour)
constexpr uint16_t kEnergy = 0x07E0;    // green: Wh, active contract, PD pill
constexpr uint16_t kCharge = 0x5D1F;    // blue: Ah
constexpr uint16_t kOhm = 0xB5B6;       // silver: load R
constexpr uint16_t kDPlus = 0xFB20;     // orange D+ tag
constexpr uint16_t kDMinus = 0x2D7F;    // blue D- tag
constexpr uint16_t kFlow = 0x07E0;      // flow arrows and chevrons: one colour for both directions
constexpr uint16_t kOk = 0x07E0;
constexpr uint16_t kCaution = 0xFC00;   // amber
constexpr uint16_t kError = 0xF800;
constexpr uint16_t kWarn = kError;      // legacy name
constexpr uint16_t kStale = 0xC618;     // frozen values, LAST pill
constexpr uint16_t kFocus = 0x07FF;     // selected row bar, edit underline, current page dot
constexpr uint16_t kVoltT = 0x2120, kAmpT = 0x0124, kWattT = 0x3083, kEnergyT = 0x0120, kOkT = 0x0120;
constexpr uint16_t kChargeT = 0x0866, kCautionT = 0x2900, kErrorT = 0x3000, kStaleT = 0x2104;
constexpr uint16_t kFocusT = 0x0146, kDimT = 0x2104, kDPlusT = 0x3080, kDMinusT = 0x0108, kFaintT = 0x1082;
// Home trend sweep: the 1 px level line above a tint column, one dim step brighter than the tint.
constexpr uint16_t kVoltL = 0x6B40, kAmpL = 0x0350, kWattL = 0x6945;
constexpr uint16_t kPanelBg = 0x2104;
constexpr uint16_t kPanelHi = 0x07FF;   // pressed slot
constexpr uint16_t kPanelLine = 0x8410;   // slot divider: light grey, clearly visible on kPanelBg

// Slot lengths along the panel (pixels) come from kSlotLenPx (ui_labels.hpp). Each centre is the
// slot start + len / 2, so the slots tile 0..240. To follow measured button positions, change the
// lengths there (the sum must stay 240); the centres follow.
constexpr int16_t slot_start(unsigned i) {
    int16_t s = 0;
    for (unsigned k = 0; k < i; k++) s = static_cast<int16_t>(s + kSlotLenPx[k]);
    return s;
}
constexpr int16_t kSlotLens[4] = {kSlotLenPx[0], kSlotLenPx[1], kSlotLenPx[2], kSlotLenPx[3]};
constexpr int16_t kSlotCenters[4] = {
    static_cast<int16_t>(slot_start(0) + kSlotLens[0] / 2), static_cast<int16_t>(slot_start(1) + kSlotLens[1] / 2),
    static_cast<int16_t>(slot_start(2) + kSlotLens[2] / 2), static_cast<int16_t>(slot_start(3) + kSlotLens[3] / 2)};
constexpr int16_t kPanelThickness = 16;   // one rotated 13 px label line plus margin

constexpr unsigned kSmallAdvPx = 8;   // advance of the 16 px font (Spleen 8x16; tests check it)
constexpr int kContentChars = 220 / kSmallAdvPx;   // 220 px / advance of the 16 px font (27)

inline const Font &kSmall = font_16;
inline const Font &kMid = font_24;
inline const Font &kBig = font_48;
inline const Font &kHero = font_64;   // Home VBUS / IBUS digits (digits, '.', '-', ' ' only)
inline const Font &kLabelFont = font_12;

}  // namespace k2
