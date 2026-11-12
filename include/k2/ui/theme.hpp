// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Fieldbook themes: three const RGB565 tables in flash (Night, Paper, Classic), one of them copied into `pal` at boot
// from the stored setting (uiprefs). A theme change takes effect at the next boot: there is no run-time repaint path,
// so every colour a page reads is the one it was drawn with.
//
// Two kinds of token live in one table:
//  - the Fieldbook tokens (docs/UI_SYSTEM.md "Palette"): ground, inks, spot fills and the role tokens the kit names
//    (focus, accent, cap, ticket, chip, watt). New code names these (ui_style.hpp kInk, kAccent, ...).
//  - the legacy tokens of ui_style.hpp (kText, kVolt, kFocusT ...), resolved per theme so a page that still names them
//    reads well on every ground: on Paper and Night the unit hues become ink (colour is a block, never a glyph) and the
//    tints become bg2; Classic keeps the firmware's own values, so a legacy page looks as it did.
// The values are RGB565-exact. tests/test_theme.cpp pins them.
#pragma once
#include <cstdint>
#include <string_view>

namespace k2::theme {

enum class Id : std::uint8_t { Night = 0, Paper = 1, Classic = 2 };
inline constexpr int kCount = 3;
// The stored value (uiprefs) to a theme: anything unknown reads as the default (Night, the fresh-device theme).
constexpr Id id_from(unsigned v) { return v < static_cast<unsigned>(kCount) ? static_cast<Id>(v) : Id::Night; }
constexpr std::string_view name(Id t) {
    return t == Id::Paper ? std::string_view("Paper") : t == Id::Classic ? std::string_view("Classic") : std::string_view("Night");
}

using C = std::uint16_t;
struct Palette {
    // ---- Fieldbook tokens
    C bg, bg2, rule, ink, ink2, ink3, on_ink, on_ink_dim, ink_lift, stale, stale_ink;
    C volt, volt_d, amp, amp_d, ok, warn, err, err_d, ok_t, warn_t, err_t, spot_ink, err_ink;
    C focus, on_focus, on_focus_dim, focus_bar;   // focus_bar 0 = none (Paper, Night: the block alone)
    C accent, cap, on_cap, ticket, on_ticket, ticket_frame;   // ticket_frame 0 = none
    C chip, on_chip, watt, watt_ink, watt_d, watt_dim, qr_lt, qr_dk;
    C pass, on_pass, part, on_part;   // verdict cells: Paper/Night solid spots, Classic the old tint + hue
    // The Flow trends (home_cell.cpp): per live quantity (and the grey of a stale field) the 1 px line and the faint area
    // under it, both mixed from the field's block towards its digit ink. The line is at least 3:1 against the block above
    // it and against the area below it; the digits stay at least 4.5:1 over the area (tests/test_ui_flow_contrast.cpp).
    C trace_volt, trace_amp, trace_watt, trace_stale, area_volt, area_amp, area_watt, area_stale;
    // ---- legacy tokens (the ui_style.hpp names), resolved for this theme
    C surface, surface2, bar, l_volt, l_amp, l_watt, l_energy, l_charge, l_ohm, l_dplus, l_dminus, l_flow, l_stale, l_focus;
    C l_volt_t, l_amp_t, l_watt_t, l_energy_t, l_charge_t, l_focus_t, l_dim_t, l_dplus_t, l_dminus_t, l_faint_t;
    C l_volt_l, l_amp_l, l_watt_l, l_panel_hi, l_panel_line;
};

// Spot fills shared by Paper and Night.
inline constexpr C kSpotVolt = 0xFE85, kSpotVoltD = 0xE5A0, kSpotAmp = 0x5E3E, kSpotAmpD = 0x353B;
inline constexpr C kSpotOk = 0x3DCC, kSpotWarn = 0xFC44, kSpotErr = 0xC985, kSpotErrD = 0xA124;
inline constexpr C kPaperGround = 0xF79C, kInkGround = 0x10C3;
// The alarm ink: white on the error red in every theme (user decision 2026-11-12, the Flow over-contract state).
inline constexpr C kAlarmInk = 0xFFFF;

// `a` moved n/32 of the way towards `b`, channel by channel in RGB565 (truncating). The trend shades of a field are its
// block moved towards its digit ink, so they stay in the block's hue (Classic keeps its cyan, yellow and coral).
constexpr C mix565(C a, C b, int n) {
    const int r = (a >> 11) & 31, g = (a >> 5) & 63, bl = a & 31;
    const int rb = (b >> 11) & 31, gb = (b >> 5) & 63, bb = b & 31;
    return static_cast<C>(((r + (rb - r) * n / 32) << 11) | ((g + (gb - g) * n / 32) << 5) | (bl + (bb - bl) * n / 32));
}
// Area under a trend: 3/32 of the way to the ink (2/32 on a stale grey, whose block already leaves its digits little room).
inline constexpr int kAreaN = 3, kAreaStaleN = 2;

constexpr Palette paper_or_night(bool night) {
    const C bg = night ? kInkGround : kPaperGround, ink = night ? kPaperGround : kInkGround;
    const C bg2 = night ? 0x2125 : 0xE71A, rule = night ? 0x3A09 : 0xCE57;
    const C ink2 = night ? 0xAD33 : 0x5B0C, ink3 = night ? 0x634E : 0xA512;
    const C on_ink_dim = night ? 0x5B0C : 0xB574, ink_lift = night ? 0xD677 : 0x31C8;
    Palette p{};
    p.bg = bg; p.bg2 = bg2; p.rule = rule; p.ink = ink; p.ink2 = ink2; p.ink3 = ink3;
    p.on_ink = bg; p.on_ink_dim = on_ink_dim; p.ink_lift = ink_lift;
    p.stale = night ? 0x31A7 : 0xD698; p.stale_ink = ink2;
    p.volt = kSpotVolt; p.volt_d = kSpotVoltD; p.amp = kSpotAmp; p.amp_d = kSpotAmpD;
    p.ok = kSpotOk; p.warn = kSpotWarn; p.err = kSpotErr; p.err_d = kSpotErrD;
    p.ok_t = night ? 0x19A4 : 0xD77A; p.warn_t = night ? 0x4941 : 0xFF17; p.err_t = night ? 0x48A2 : 0xF699;
    p.spot_ink = kInkGround; p.err_ink = kPaperGround;
    p.focus = ink; p.on_focus = bg; p.on_focus_dim = on_ink_dim; p.focus_bar = 0;
    p.accent = ink; p.cap = ink; p.on_cap = bg; p.ticket = ink; p.on_ticket = bg; p.ticket_frame = 0;
    p.chip = ink; p.on_chip = bg; p.watt = ink; p.watt_ink = bg; p.watt_d = ink_lift; p.watt_dim = on_ink_dim;
    p.qr_lt = kPaperGround; p.qr_dk = kInkGround;
    p.pass = kSpotOk; p.on_pass = kInkGround; p.part = kSpotWarn; p.on_part = kInkGround;
    // line: block to ink in 32nds, the least that puts it 3:1 off both the block and its area (measured: the test)
    p.trace_volt = mix565(p.volt, p.spot_ink, 18); p.trace_amp = mix565(p.amp, p.spot_ink, 20);
    p.trace_watt = mix565(p.watt, p.watt_ink, night ? 18 : 14); p.trace_stale = mix565(p.stale, p.stale_ink, night ? 22 : 28);
    p.area_volt = mix565(p.volt, p.spot_ink, kAreaN); p.area_amp = mix565(p.amp, p.spot_ink, kAreaN);
    p.area_watt = mix565(p.watt, p.watt_ink, kAreaN); p.area_stale = mix565(p.stale, p.stale_ink, kAreaStaleN);
    // legacy: surfaces and tints are bg2, the masthead has no fill, the unit hues are ink, focus is the accent ink
    p.surface = bg2; p.surface2 = bg2; p.bar = bg;
    p.l_volt = ink; p.l_amp = ink; p.l_watt = ink; p.l_energy = ink; p.l_charge = ink; p.l_ohm = ink;
    p.l_dplus = ink; p.l_dminus = ink; p.l_flow = ink; p.l_stale = ink2; p.l_focus = ink;
    p.l_volt_t = bg2; p.l_amp_t = bg2; p.l_watt_t = bg2; p.l_energy_t = bg2; p.l_charge_t = bg2; p.l_focus_t = bg2;
    p.l_dim_t = bg2; p.l_dplus_t = bg2; p.l_dminus_t = bg2; p.l_faint_t = bg2;
    p.l_volt_l = ink3; p.l_amp_l = ink3; p.l_watt_l = ink3; p.l_panel_hi = ink; p.l_panel_line = rule;
    return p;
}

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
    p.trace_volt = mix565(p.volt, p.spot_ink, 16); p.trace_amp = mix565(p.amp, p.spot_ink, 16);
    p.trace_watt = mix565(p.watt, p.watt_ink, 19); p.trace_stale = mix565(p.stale, p.stale_ink, 17);
    p.area_volt = mix565(p.volt, p.spot_ink, kAreaN); p.area_amp = mix565(p.amp, p.spot_ink, kAreaN);
    p.area_watt = mix565(p.watt, p.watt_ink, kAreaN); p.area_stale = mix565(p.stale, p.stale_ink, kAreaStaleN);
    // legacy: the firmware tokens of 2026-11-12, unchanged
    p.surface = 0x18E3; p.surface2 = 0x2124; p.bar = 0x08A9;
    p.l_volt = 0xFFE0; p.l_amp = 0x07FF; p.l_watt = 0xFB2C; p.l_energy = 0x07E0; p.l_charge = 0x5D1F; p.l_ohm = 0xB5B6;
    p.l_dplus = 0xFB20; p.l_dminus = 0x2D7F; p.l_flow = 0x07E0; p.l_stale = 0xC618; p.l_focus = 0x07FF;
    p.l_volt_t = 0x2120; p.l_amp_t = 0x0124; p.l_watt_t = 0x3083; p.l_energy_t = 0x0120; p.l_charge_t = 0x0866;
    p.l_focus_t = 0x0146; p.l_dim_t = 0x2104; p.l_dplus_t = 0x3080; p.l_dminus_t = 0x0108; p.l_faint_t = 0x1082;
    p.l_volt_l = 0x6B40; p.l_amp_l = 0x0350; p.l_watt_l = 0x6945; p.l_panel_hi = 0x07FF; p.l_panel_line = 0x8410;
    return p;
}

inline constexpr Palette kNight = paper_or_night(true);
inline constexpr Palette kPaper = paper_or_night(false);
inline constexpr Palette kClassic = classic();

constexpr const Palette &table(Id t) { return t == Id::Paper ? kPaper : t == Id::Classic ? kClassic : kNight; }

// The palette in force. Constant-initialised to the default theme, so host tests and code that runs before boot's
// apply() read Night.
inline Palette pal = kNight;
inline Id active_id = Id::Night;
inline Id chosen_id = Id::Night;   // what the setting holds now: differs from active_id until the next boot

// Boot only, before the first paint: puts theme `t` in force.
inline void apply(Id t) {
    pal = table(t);
    active_id = chosen_id = t;
}
inline Id active() { return active_id; }
// K2 > Theme: the theme the next boot applies (the caller stores it in uiprefs). Nothing is repainted.
inline void choose(Id t) { chosen_id = t; }
inline Id chosen() { return chosen_id; }
inline bool reboot_pending() { return chosen_id != active_id; }

}  // namespace k2::theme
