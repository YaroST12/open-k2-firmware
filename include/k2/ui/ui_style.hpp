// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Palette, fonts and panel geometry shared by the Ui and its pages.
#pragma once
#include <cstdint>
#include "k2/ui/font_aa.hpp"
#include "k2/ui/theme.hpp"
#include "k2/ui/ui_labels.hpp"

namespace k2 {

// RGB565 palette tokens. The values are the theme's (k2/ui/theme.hpp): three const tables in flash, one copied into
// theme::pal at boot. Each name is a reference into that copy, so a token is a run-time read, never a constant
// expression. The legacy names (kText, kVolt, kFocusT ...) keep their meaning per theme; new code uses the Fieldbook
// names below them (docs/UI_SYSTEM.md "Palette").
#define K2_TOKEN(name, field) inline const uint16_t &name = theme::pal.field;
K2_TOKEN(kBg, bg)
K2_TOKEN(kSurface, surface)     // cards, flow-diagram panel
K2_TOKEN(kSurface2, surface2)   // overlays
K2_TOKEN(kBar, bar)             // title / status bar ground
K2_TOKEN(kRule, rule)           // dividers, gauge tracks
K2_TOKEN(kText, ink)
K2_TOKEN(kDim, ink2)
K2_TOKEN(kFaint, ink3)          // "no data"
K2_TOKEN(kVolt, l_volt)         // V text (Classic yellow; ink on Paper and Night)
K2_TOKEN(kAmp, l_amp)
K2_TOKEN(kWatt, l_watt)
K2_TOKEN(kEnergy, l_energy)
K2_TOKEN(kCharge, l_charge)
K2_TOKEN(kOhm, l_ohm)
K2_TOKEN(kDPlus, l_dplus)
K2_TOKEN(kDMinus, l_dminus)
K2_TOKEN(kFlow, l_flow)
K2_TOKEN(kOk, ok)
K2_TOKEN(kCaution, warn)        // amber
K2_TOKEN(kError, err)
K2_TOKEN(kWarn, err)            // legacy name of kError
K2_TOKEN(kStale, l_stale)       // frozen values, LAST pill
K2_TOKEN(kFocus, l_focus)       // selected row bar, picked markers, progress
K2_TOKEN(kVoltT, l_volt_t) K2_TOKEN(kAmpT, l_amp_t) K2_TOKEN(kWattT, l_watt_t) K2_TOKEN(kEnergyT, l_energy_t)
K2_TOKEN(kOkT, ok_t) K2_TOKEN(kChargeT, l_charge_t) K2_TOKEN(kCautionT, warn_t) K2_TOKEN(kErrorT, err_t)
K2_TOKEN(kStaleT, stale) K2_TOKEN(kFocusT, l_focus_t) K2_TOKEN(kDimT, l_dim_t) K2_TOKEN(kDPlusT, l_dplus_t)
K2_TOKEN(kDMinusT, l_dminus_t) K2_TOKEN(kFaintT, l_faint_t)
// Trend sweep: the 1 px level line above a tint column.
K2_TOKEN(kVoltL, l_volt_l) K2_TOKEN(kAmpL, l_amp_l) K2_TOKEN(kWattL, l_watt_l)
K2_TOKEN(kPanelBg, bg2)
K2_TOKEN(kPanelHi, l_panel_hi)     // pressed slot
K2_TOKEN(kPanelLine, l_panel_line) // slot divider

// ---- Fieldbook tokens (theme.hpp; CONCEPT section 3) ----
K2_TOKEN(kBg2, bg2)             // key rail, bar tracks, neutral chips
K2_TOKEN(kInk, ink)             // text
K2_TOKEN(kInk2, ink2)           // keys, notes, tails
K2_TOKEN(kInk3, ink3)           // placeholders, dot leaders, blocked rows (never a value)
K2_TOKEN(kOnInk, on_ink)        // text on an inverted block
K2_TOKEN(kOnInkDim, on_ink_dim)
K2_TOKEN(kInkLift, ink_lift)    // trend and bar track on the W field
K2_TOKEN(kStaleFill, stale)     // a field holding a last reading
K2_TOKEN(kStaleInk, stale_ink)  // digits of a stale field
K2_TOKEN(kSpotVolt, volt) K2_TOKEN(kSpotVoltD, volt_d) K2_TOKEN(kSpotAmp, amp) K2_TOKEN(kSpotAmpD, amp_d)
K2_TOKEN(kErrD, err_d)
K2_TOKEN(kTraceVolt, trace_volt) K2_TOKEN(kTraceAmp, trace_amp) K2_TOKEN(kTraceWatt, trace_watt) K2_TOKEN(kTraceStale, trace_stale)
K2_TOKEN(kAreaVolt, area_volt) K2_TOKEN(kAreaAmp, area_amp) K2_TOKEN(kAreaWatt, area_watt) K2_TOKEN(kAreaStale, area_stale)
K2_TOKEN(kSpotInk, spot_ink)    // text on a spot fill (volt, amp, ok, warn)
K2_TOKEN(kErrInk, err_ink)      // text on the error red
K2_TOKEN(kFocusFill, focus) K2_TOKEN(kOnFocus, on_focus) K2_TOKEN(kOnFocusDim, on_focus_dim)
K2_TOKEN(kFocusBar, focus_bar)  // 0 = none
K2_TOKEN(kAccent, accent)       // masthead rule, progress, picked markers, running outline, Bench line
K2_TOKEN(kCap, cap) K2_TOKEN(kOnCap, on_cap)          // the OK cap of the key rail
K2_TOKEN(kTicket, ticket) K2_TOKEN(kOnTicket, on_ticket)
K2_TOKEN(kTicketFrame, ticket_frame)                  // 0 = none
K2_TOKEN(kChip, chip) K2_TOKEN(kOnChip, on_chip)      // solid state chips, line tags
K2_TOKEN(kWattFill, watt) K2_TOKEN(kWattInk, watt_ink) K2_TOKEN(kWattD, watt_d) K2_TOKEN(kWattDim, watt_dim)
K2_TOKEN(kQrLt, qr_lt) K2_TOKEN(kQrDk, qr_dk)
K2_TOKEN(kPassFill, pass) K2_TOKEN(kOnPass, on_pass) K2_TOKEN(kPartFill, part) K2_TOKEN(kOnPart, on_part)
#undef K2_TOKEN
inline constexpr uint16_t kAlarmInk = theme::kAlarmInk;   // white on red, every theme

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
