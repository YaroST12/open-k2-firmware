// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The palette: one const table of RGB565 values, Classic (black ground, cyan focus, yellow volts, coral watts). The
// Fieldbook experiment shipped Night and Paper beside it; the user took them out (2026-11-13), so there is no theme to
// choose, no stored setting (uiprefs ignores the old theme bits) and no boot-time copy: `pal` is a constant, and every
// token of ui_style.hpp reads straight from flash.
//
// Two kinds of token live in the one table:
//  - the Fieldbook tokens (docs/UI_SYSTEM.md "Palette"): ground, inks, spot fills and the role tokens the kit names
//    (focus, accent, cap, ticket, chip, watt). New code names these (ui_style.hpp kInk, kAccent, ...).
//  - the legacy tokens of ui_style.hpp (kText, kVolt, kFocusT ...): the firmware's own colours since before Fieldbook.
// The values are RGB565-exact. tests/test_theme.cpp pins them.
#pragma once
#include <cstdint>

namespace k2::theme {

using C = std::uint16_t;
struct Palette {
    // ---- Fieldbook tokens
    C bg, bg2, rule, ink, ink2, ink3, on_ink, on_ink_dim, ink_lift, stale, stale_ink;
    C volt, volt_d, amp, amp_d, ok, warn, err, err_d, ok_t, warn_t, err_t, spot_ink, err_ink;
    C focus, on_focus, on_focus_dim, focus_bar;   // focus_bar 0 = none
    C accent, cap, on_cap, ticket, on_ticket, ticket_frame;   // ticket_frame 0 = none
    C chip, on_chip, watt, watt_ink, watt_d, watt_dim, qr_lt, qr_dk;
    C pass, on_pass, part, on_part;   // verdict cells: the dark tint plus the hue
    // ---- legacy tokens (the ui_style.hpp names)
    C surface, surface2, bar, l_volt, l_amp, l_watt, l_energy, l_charge, l_ohm, l_dplus, l_dminus, l_flow, l_stale, l_focus;
    C l_volt_t, l_amp_t, l_watt_t, l_energy_t, l_charge_t, l_focus_t, l_dim_t, l_dplus_t, l_dminus_t, l_faint_t;
    C l_volt_l, l_amp_l, l_watt_l, l_panel_hi, l_panel_line;
};

// The alarm ink: white on the error red (the Flow over-contract state).
inline constexpr C kAlarmInk = 0xFFFF;

constexpr Palette classic() {
    Palette p{};
    p.bg = 0x0000; p.bg2 = 0x2104; p.rule = 0x2945; p.ink = 0xFFFF; p.ink2 = 0x8410; p.ink3 = 0x4208;
    p.on_ink = 0x0000; p.on_ink_dim = 0x4208; p.ink_lift = 0x2945; p.stale = 0x2104; p.stale_ink = 0xC618;
    p.volt = 0xFFE0; p.volt_d = 0xD680; p.amp = 0x07FF; p.amp_d = 0x069A;
    p.ok = 0x07E0; p.warn = 0xFC00; p.err = 0xF800; p.err_d = 0xC000;
    p.ok_t = 0x0120; p.warn_t = 0x2900; p.err_t = 0x3000; p.spot_ink = 0x0000; p.err_ink = 0x0000;
    p.focus = 0x0146; p.on_focus = 0xFFFF; p.on_focus_dim = 0x8410; p.focus_bar = 0x07FF;
    p.accent = 0x07FF; p.cap = 0x07FF; p.on_cap = 0x0000; p.ticket = 0x0146; p.on_ticket = 0xFFFF; p.ticket_frame = 0x07FF;
    p.chip = 0x07FF; p.on_chip = 0x0000; p.watt = 0xFB2C; p.watt_ink = 0x0000; p.watt_d = 0xDA88; p.watt_dim = 0x0000;
    p.qr_lt = 0xFFFF; p.qr_dk = 0x0000;
    p.pass = 0x0120; p.on_pass = 0x07E0; p.part = 0x2900; p.on_part = 0xFC00;
    // legacy: the firmware tokens of 2026-11-12, unchanged
    p.surface = 0x18E3; p.surface2 = 0x2124; p.bar = 0x08A9;
    p.l_volt = 0xFFE0; p.l_amp = 0x07FF; p.l_watt = 0xFB2C; p.l_energy = 0x07E0; p.l_charge = 0x5D1F; p.l_ohm = 0xB5B6;
    p.l_dplus = 0xFB20; p.l_dminus = 0x2D7F; p.l_flow = 0x07E0; p.l_stale = 0xC618; p.l_focus = 0x07FF;
    p.l_volt_t = 0x2120; p.l_amp_t = 0x0124; p.l_watt_t = 0x3083; p.l_energy_t = 0x0120; p.l_charge_t = 0x0866;
    p.l_focus_t = 0x0146; p.l_dim_t = 0x2104; p.l_dplus_t = 0x3080; p.l_dminus_t = 0x0108; p.l_faint_t = 0x1082;
    p.l_volt_l = 0x6B40; p.l_amp_l = 0x0350; p.l_watt_l = 0x6945; p.l_panel_hi = 0x07FF; p.l_panel_line = 0x8410;
    return p;
}

// The palette in force: a constant, so every token is a compile-time value.
inline constexpr Palette pal = classic();

}  // namespace k2::theme
