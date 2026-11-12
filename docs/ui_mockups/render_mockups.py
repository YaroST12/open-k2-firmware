#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Render the docs/UI_DESIGN.md (v2) mockups: one 240x240 PNG per screen state (saved at x2) and an
ASCII sketch + element table per screen (docs/ui_mockups/screens.md). v1 is kept in v1/.

Design reference only; nothing here runs on the device. Uses the repo's JetBrains Mono TTF with
ligatures off, so glyph metrics match tools/gen_font.py (12/16/24 px full ASCII, 48 px digits only;
16/24 px also carry ° · Ω ← ↑ → ↓). Icons, badges, pills, gauges, sparklines and the port/flow
diagram are drawn primitives (rects with clipped corners, 1-bit 12x12 masks, 1-px columns), all
renderable row by row through the firmware's single 240-px line buffer (gfx.hpp).

Physical layout (screen upright): TOP edge = male USB-C (power path), BOTTOM edge = female USB-C
(power path), LEFT edge = female USB-C for the PC (HID/logging, not in the power path), RIGHT edge =
BACK, -, OK, + buttons. Power flows between TOP and BOTTOM in either direction, so every flow
indicator in these mockups is vertical: ↑ = bottom→top, ↓ = top→bottom.

    python3 docs/ui_mockups/render_mockups.py
"""
import math
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
TTF = ROOT / "third_party/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"
OUT = Path(__file__).resolve().parent

TOK = {  # RGB565
    "bg": 0x0000, "surface": 0x18E3, "surface2": 0x2124, "bar": 0x08A9, "panel": 0x2104,
    "line": 0x8410, "rule": 0x2945, "text": 0xFFFF, "dim": 0x8410, "faint": 0x4208,
    "volt": 0xFFE0, "amp": 0x07FF, "watt": 0xFB2C, "energy": 0x07E0, "charge": 0x5D1F, "ohm": 0xB5B6,
    "flow": 0x07E0, "ok": 0x07E0, "caution": 0xFC00, "error": 0xF800, "stale": 0xC618, "focus": 0x07FF,
    "dplus": 0xFB20, "dminus": 0x2D7F,
    # tints: ~15 % of the hue on black, for pill / tile / card fills
    "volt_t": 0x2120, "amp_t": 0x0124, "watt_t": 0x3083, "ok_t": 0x0120, "energy_t": 0x0120,
    "caution_t": 0x2900, "error_t": 0x3000, "stale_t": 0x2104, "focus_t": 0x0146, "dim_t": 0x2104,
    "charge_t": 0x0866, "dplus_t": 0x3080, "dminus_t": 0x0108, "faint_t": 0x1082,
}
ADV = {12: 7, 16: 10, 24: 14, 48: 29}
ASC = {12: 11, 16: 14, 24: 22, 48: 35}
LH = {12: 13, 16: 17, 24: 26, 48: 35}
FONTS = {s: ImageFont.truetype(str(TTF), s, layout_engine=ImageFont.Layout.BASIC) for s in ADV}
CW = 224
SLOTS = [(0, 66), (66, 54), (120, 54), (174, 66)]

ICONS = {  # 12x12 1-bit masks: 18 B each in flash
    "bolt": ["......##....", ".....##.....", "....##......", "...##.......", "..#######...", ".....###....",
             "....###.....", "...##.......", "..##........", ".##.........", "##..........", "............"],
    "cable": ["##..........", "###.........", "###.........", ".##.........", "..##........", "...##.......",
              "....##......", ".....##.....", "......##....", ".......###..", "........###.", ".........##."],
    "therm": ["....##......", "...#..#.....", "...#..#.....", "...#..#.....", "...#..#.....", "...####.....",
              "...####.....", "..######....", ".########...", ".########...", "..######....", "...####....."],
    "warn": [".....##.....", ".....##.....", "....####....", "....#..#....", "...##..##...", "...##..##...",
             "..###..###..", "..########..", ".####..####.", ".##########.", "############", "............"],
    "check": ["............", "...........#", "..........##", ".........##.", "#.......##..", "##.....##...",
              ".##...##....", "..##.##.....", "...###......", "....#.......", "............", "............"],
    "cross": ["............", "##......##..", ".##....##...", "..##..##....", "...####.....", "....##......",
              "...####.....", "..##..##....", ".##....##...", "##......##..", "............", "............"],
    "phone": ["..######....", ".#......#...", ".#.####.#...", ".#.####.#...", ".#.####.#...", ".#.####.#...",
              ".#.####.#...", ".#.####.#...", ".#......#...", ".#..##..#...", "..######....", "............"],
    "chip": ["..#.#.#.#...", ".########...", "##......##..", ".#.####.#...", "##.#..#.##..", ".#.#..#.#...",
             "##.####.##..", ".#......#...", "##########..", "..#.#.#.#...", "............", "............"],
    "ohm": ["...#####....", "..#.....#...", ".#.......#..", ".#.......#..", ".#.......#..", "..#.....#...",
            "...#...#....", "...#...#....", ".###...###..", "............", "............", "............"],
    "clock": ["...######...", "..#......#..", ".#...#....#.", ".#...#....#.", ".#...#....#.", ".#...####.#.",
              ".#........#.", ".#........#.", "..#......#..", "...######...", "............", "............"],
    "pc": ["##########..", "#........#..", "#........#..", "#........#..", "#........#..", "##########..",
           "....##......", "..######....", "............", "............", "............", "............"],
    "plug": ["..#....#....", "..#....#....", "..#....#....", "#########...", "#########...", "#########...",
             ".#######....", "..#####.....", "....#.......", "....#.......", "....#.......", "....#......."],
    "brick": ["....#..#....", "....#..#....", "....#..#....", "..########..", "..#......#..", "..#......#..",
              "..#......#..", "..#......#..", "..#......#..", "..#......#..", "..########..", "............"],
    "anon": ["............", "...######...", "...######...", "...######...", ".##########.", "............",
             ".###....###.", "....####....", ".###....###.", "............", "............", "............"],
    "pd": ["............", "............", ".###..####..", "#...#.#...#.", "#...#.#...#.", ".###..#...#.",
           "#.....#...#.", "#.....#...#.", "#.....####..", "............", "............", "............"],
    "res": ["............", "............", "...#...#....", "...#...#....", "..##..##....", "..#.#.#.#...",
            "###.#.#.#.##", "....#.#.#.#.", "....##..##..", ".....#...#..", ".....#...#..", "............"],
    # Tools / Settings rows (appended after the original 16 so their indices stay).
    "emark": ["............", "............", "..########..", ".#........#.", "#...####...#", "#...#..#...#",
              "#...#..#...#", "#...####...#", ".#........#.", "..########..", "............", "............"],
    "blackbox": ["............", ".##########.", "############", "############", "#####..#####", "####....####",
                 "####....####", "#####..#####", "############", "############", ".##########.", "............"],
    "report": ["..########..", "..#......#..", "..#.####.#..", "..#......#..", "..#.####.#..", "..#......#..",
               "..#.####.#..", "..#......#..", "..#.##...#..", "..#......#..", "..########..", "............"],
    "ghost": ["...######...", "..########..", ".##########.", ".##..##..##.", ".##..##..##.", ".##########.",
              ".##########.", ".##########.", ".##########.", ".##########.", ".##.####.##.", ".#...##...#."],
    "usb": [".....##.....", "....####....", ".....##.....", ".##..##..###", ".##..##..###", "..#..##..#..",
            "..##.##.##..", "...######...", ".....##.....", ".....##.....", "....####....", "....####...."],
    "rotate": ["...#####....", "..#.....#...", ".#.......#..", ".#.....#####", ".#......###.", ".#.......#..",
               ".#..........", ".#.......#..", "..#.....#...", "...#####....", "............", "............"],
    "keys": ["............", "###.###.###.", "#.#.#.#.#.#.", "###.###.###.", "............", "###.###.###.",
             "#.#.###.#.#.", "###.###.###.", "............", "............", "............", "............"],
    "camera": ["............", "...####.....", "############", "#..........#", "#...####...#", "#..#....#..#",
               "#..#....#..#", "#...####...#", "#..........#", "############", "............", "............"],
    "wave": ["............", "............", "..##....##..", ".#..#..#..#.", "#....##....#", "............",
             "............", "............", "#.#.#.#.#.#.", "............", "............", "............"],
    "info": ["...######...", "..#......#..", ".#...##...#.", ".#........#.", ".#...##...#.", ".#...##...#.",
             ".#...##...#.", ".#...##...#.", "..#......#..", "...######...", "............", "............"],
    "meter": ["....####....", "..##....##..", ".#........#.", ".#......#.#.", "#......#...#", "#.....#....#",
              "#....##....#", "#....##....#", ".#........#.", ".#........#.", "..##....##..", "....####...."],
    "battery": ["............", "............", "##########..", "#........#..", "#.######.###", "#.######.###",
                "#.######.###", "#.######.###", "#........#..", "##########..", "............", "............"],
}


class Screen:
    def __init__(self, sid, labels):
        self.sid, self.labels, self.ops = sid, labels, []

    def rect(self, x, y, w, h, tok, name="", r=0):
        self.ops.append(("rect", x, y, w, h, tok, name, r)); return self

    def frame(self, x, y, w, h, tok, name="", r=0, t=2):
        self.ops.append(("frame", x, y, w, h, tok, name, r, t)); return self

    def text(self, x, y, size, s, tok, name="", right=None):
        if right is not None: x = right - len(s) * ADV[size]
        self.ops.append(("text", x, y, size, s, tok, name)); return self

    def icon(self, x, y, key, tok, name=""):
        self.ops.append(("icon", x, y, key, tok, name or f"icon {key}")); return self

    def spark(self, x, y, w, h, data, tok, name="sparkline"):
        self.ops.append(("spark", x, y, w, h, data, tok, name)); return self

    def poly(self, pts, tok, name=""):
        self.ops.append(("poly", pts, tok, name)); return self


def tint(tok):
    return tok + "_t" if tok + "_t" in TOK else "surface2"


# ---- atoms / molecules ---------------------------------------------------------------------------
def pill(sc, x, y, s, tok, right=None, name="pill", solid=False):
    """S12 tag, 14 px high, r=3; tinted fill + coloured text, or solid fill + black text."""
    w = len(s) * 7 + 8
    if right is not None: x = right - w
    sc.rect(x, y, w, 14, tok if solid else tint(tok), name, r=3)
    sc.text(x + 4, y + 1, 12, s, "bg" if solid else tok, name + " text")
    return x + w


def badge(sc, x, y, s, tok, name="badge", size=26):
    """Outlined rounded badge: 2 px outline r=6, glyph centred, T24 or B16."""
    sc.frame(x, y, size, size, tok, name, r=6, t=2)
    f = 24 if size >= 26 else 16
    sc.text(x + (size - ADV[f] * len(s)) // 2, y + (size - LH[f]) // 2 + 1, f, s, tok, name + " glyph")


def chev_v(sc, x, y, n, tok, up, pitch=8, w=10, phase=-1):
    """Vertical chevrons (w x 6 each). The firmware animates by moving one bright chevron per 250 ms."""
    for k in range(n):
        cy = y + k * pitch
        t = tok if phase < 0 or k == phase else "faint"
        if up:
            pts = [(x, cy + 5), (x + w // 2, cy), (x + w, cy + 5), (x + w - 2, cy + 6), (x + w // 2, cy + 2), (x + 2, cy + 6)]
        else:
            pts = [(x, cy), (x + w // 2, cy + 5), (x + w, cy), (x + w - 2, cy - 1), (x + w // 2, cy + 3), (x + 2, cy - 1)]
        sc.poly(pts, t, "chevron")


def port_male(sc, x, y, tok, name="top port (male)"):
    """Male USB-C plug seen from the screen: solid rounded 18x8 tongue sticking up."""
    sc.rect(x, y, 18, 8, tok, name, r=3).rect(x + 4, y + 3, 10, 2, "bg", name + " slot")


def port_female(sc, x, y, tok, name="bottom port (female)"):
    """Female USB-C receptacle: rounded outline 18x8 + tongue."""
    sc.frame(x, y, 18, 8, tok, name, r=3, t=1)
    sc.rect(x + 4, y + 3, 10, 2, tok, name + " tongue")


def dots(sc, i, n):
    pitch = 8 if n <= 5 else 6   # six rotation pages: tighter dots keep the mini readout clear
    x0 = 220 - n * pitch
    for k in range(n):
        sc.rect(x0 + k * pitch, 7, 5, 5, "focus" if k == i else "faint", "page dot", r=1)


def status_bar(sc, title, page=None, live=True, stale=None, icon=None, mini=True):
    """y 0..17: icon + title, mini readout (V, A + ↑/↓), page dots or LAST pill."""
    sc.rect(0, 0, CW, 18, "bar", "status bar")
    x = 4
    if icon: sc.icon(4, 3, icon, "dim"); x = 20
    sc.text(x, 3, 12, title, "text", "title")
    mx = 102
    if mini and stale:
        sc.text(mx - 10, 3, 12, "20.01V 2.99A", "stale", "mini readout")
    elif mini and live:
        sc.text(mx - 10, 3, 12, "20.01V", "volt", "mini V").text(mx + 36, 3, 12, "2.99A", "amp", "mini A")
        sc.text(mx + 72, 1, 16, "↑", "flow", "mini flow")
    if stale:
        sc.rect(166, 2, 56, 14, "stale", "LAST pill", r=3).text(169, 3, 12, "LAST0:42", "bg", "LAST text")
    elif page:
        dots(sc, page[0], page[1])


def footer(sc, msg, tok="dim", icon=None, itok=None):
    sc.rect(0, 222, CW, 18, "bar", "footer")
    x = 4
    if icon: sc.icon(4, 225, icon, itok or tok); x = 20
    sc.text(x, 225, 12, msg, tok, "footer text")


def card(sc, x, y, w, h, accent=None, name="card"):
    sc.rect(x, y, w, h, "surface", name, r=5)
    if accent: sc.rect(x, y + 4, 3, h - 8, accent, name + " accent", r=1)


def gauge(sc, x, y, w, frac, tok, name="gauge", h=3):
    sc.rect(x, y, w, h, "rule", name + " track", r=1)
    if frac > 0: sc.rect(x, y, max(2, int(w * min(1, frac))), h, tok, name + " fill", r=1)


def row_sel(sc, y, h):
    sc.rect(2, y, CW - 6, h, "focus_t", "selected row", r=6).rect(4, y + 4, 3, h - 8, "focus", "focus bar", r=1)


def check_row(sc, x, y, ok, t, tok=None):
    sc.icon(x, y + 2, "check" if ok else "cross", "ok" if ok else (tok or "error"))
    sc.text(x + 16, y, 16, t, "dim" if ok else "text", "check text")


def flow_diagram(sc, y, top_role, bot_role, flow, ttok="dim", btok="dim", mid=None, mtok="text", h=72):
    """Vertical port diagram, x 0..63, y..y+h. The male TOP port and the female BOTTOM port sit where the
    connectors physically are; chevrons between point from source to sink. flow: 'up' | 'down' | None."""
    sc.rect(0, y, 64, h, "surface", "flow diagram", r=6)
    port_male(sc, 6, y + 5, ttok); port_female(sc, 6, y + h - 13, btok)
    if top_role: pill(sc, 28, y + 2, top_role[:4], ttok, name="top role")
    if bot_role: pill(sc, 28, y + h - 16, bot_role[:4], btok, name="bottom role")
    n = (h - 34) // 8
    if flow:
        chev_v(sc, 10, y + 18, n, "flow", flow == "up", phase=1 if flow == "down" else n - 2)
    else:
        sc.rect(14, y + 17, 2, h - 34, "faint", "no-flow line")
    if mid: sc.text(30, y + h // 2 - 6, 12, mid[:4], mtok, "diagram caption")


def wave(n, base, amp, step=False, seed=7.0):
    out = []
    for i in range(n):
        v = base + amp * math.sin(i / seed) + amp * 0.4 * math.sin(i / 2.3)
        if step and i > n * 0.55: v += amp * 2.2
        out.append(v)
    return out


def sub(ok, minus="", plus=""):
    return ["Back", minus, ok, plus]


def nav(ok):
    return ["Back", "-/Prev", ok, "+/Next"]


# ---- Home ----------------------------------------------------------------------------------------
def home(sid, state, flow="up", v="20.01", a="2.990", w="59.83"):
    sc = Screen(sid, nav("Rst*"))
    live, stale = state == "live", state == "stale"
    sc.rect(0, 0, CW, 18, "bar", "status bar")
    sc.icon(4, 3, "therm", "dim").text(18, 3, 12, "32.8C", "dim", "MCU temp")
    pill(sc, 62, 2, "CC1", "volt" if live else "stale", solid=live, name="CC pill")
    pill(sc, 94, 2, "PD3.0 65W" if state != "idle" else "no PD", "energy" if live else "stale", solid=live, name="PD pill")
    if stale:
        sc.rect(166, 2, 56, 14, "stale", "LAST pill", r=3).text(169, 3, 12, "LAST0:42", "bg", "LAST text")
    else:
        sc.icon(168, 3, "pc", "dim"); dots(sc, 0, 6)
    tk = (lambda t: t) if live else (lambda t: "stale") if stale else (lambda t: "faint")
    arrow = "↓" if flow == "down" else "↑"
    rows = [(22, v, "V", "V", "volt", 20.01 / 20.0), (66, a, "A", arrow if live else "A", "amp", 2.99 / 3.25),
            (110, w, "W", "W", "watt", 59.83 / 65)]
    if state == "error": rows = [(y, "--.--" if u != "A" else "-.---", u, b, t, 0) for y, _, u, b, t, _ in rows]
    if state == "idle": rows = [(y, " 0.00" if u != "A" else "0.000", u, b, t, 0) for y, _, u, b, t, _ in rows]
    for y, val, unit, bg, t, frac in rows:
        badge(sc, 2, y + 5, bg, tk(t), f"{unit} badge")
        sc.text(0, y, 48, val, tk(t), f"{unit} digits", right=178)
        sc.text(180, y + 13, 24, unit, tk(t), f"{unit} unit")
        gauge(sc, 34, y + 38, 160, frac if (live or stale) else 0, tk(t), f"{unit} gauge vs contract")
    # flow rail at the right of the units: top port (male) .. bottom port (female), x 198..216
    rail_tok = "flow" if live else "faint"
    port_male(sc, 198, 22, "text" if live else tk("dim"), "rail top port")
    port_female(sc, 198, 136, "text" if live else tk("dim"), "rail bottom port")
    if live:
        chev_v(sc, 202, 40, 11, rail_tok, flow == "up", pitch=8, w=10, phase=4)
    else:
        sc.rect(206, 36, 2, 96, "faint", "rail idle")
    # bottom cards (left lines card, right PD/totals card)
    card(sc, 0, 154, 92, 66, None, "lines card")
    pill(sc, 6, 158, "D+", "dplus", solid=not (stale or state == "error"), name="D+ tag")
    sc.text(36, 157, 16, "0.60", tk("text") if live else tk("text"), "D+ V")
    pill(sc, 6, 178, "D-", "dminus", solid=not (stale or state == "error"), name="D- tag")
    sc.text(36, 177, 16, "0.60", tk("text") if live else tk("text"), "D- V")
    sc.icon(6, 201, "ohm", "dim").text(22, 198, 16, "6.69Ω" if live else "--Ω", tk("ohm"), "load R")
    card(sc, 96, 154, 128, 66, None, "totals card")
    sc.text(102, 157, 16, "12.345" if state != "error" else "--.---", tk("energy"), "Wh").text(164, 160, 12, "Wh", "dim")
    sc.text(102, 176, 16, "0.617" if state != "error" else "-.---", tk("charge"), "Ah").text(154, 179, 12, "Ah", "dim")
    pill(sc, 0, 158, "QC3", "amp" if live else "faint", right=220, name="FC pill")
    sc.text(0, 179, 12, "1:23:45", "text" if live else tk("text"), "elapsed", right=220)
    sc.spark(102, 196, 116, 20, wave(58, 55, 6, step=True) if state in ("live", "stale") else [], tk("watt"),
             "W sparkline 60 s")
    msg = {"live": ("Power flows " + ("up" if flow == "up" else "down") + "  avg 1 s", "dim", "check", "ok"),
           "idle": ("No VBUS on top or bottom port", "text", "plug", "caution"),
           "stale": ("Unplugged 0:42 ago, frozen", "stale", "warn", "caution"),
           "error": ("Power meter not found", "error", "warn", "error")}[state]
    footer(sc, *msg)
    return sc


def reboot_hold(sid):
    sc = home(sid, "live")
    sc.rect(12, 70, 200, 84, "surface2", "hold card", r=8).frame(12, 70, 200, 84, "error", "hold outline", r=8, t=2)
    sc.icon(26, 86, "warn", "error").text(44, 82, 24, "Reboot in 3", "text", "countdown")
    gauge(sc, 26, 118, 172, 0.4, "error", "hold", h=6)
    sc.text(26, 132, 12, "Release Back to cancel", "dim")
    return sc


# ---- PD ------------------------------------------------------------------------------------------
PDOS = [("FIX", 5.00, 3.00, 15, "amp"), ("FIX", 9.00, 3.00, 27, "amp"), ("FIX", 15.00, 3.00, 45, "amp"),
        ("FIX", 20.00, 3.25, 65, "amp"), ("PPS", "3.3-11", 5.00, 55, "watt"), ("PPS", "3.3-21", 3.25, 65, "watt")]


def pdo_row(sc, y, i, p, active=False, cursor=False, stale=False):
    ty, v, a, w, tok = p
    t = "stale" if stale else tok
    if cursor: row_sel(sc, y - 1, 20)
    if active: sc.rect(4, y + 2, 3, 14, "stale" if stale else "energy", "active bar", r=1)
    sc.text(10, y, 16, str(i + 1), "dim")
    pill(sc, 22, y + 1, ty, t, name="type pill")
    vs = f"{v:5.2f}" if isinstance(v, float) else v
    sc.text(0, y, 16, vs, "stale" if stale else "text", "V", right=124)
    sc.text(0, y, 16, f"{a:.2f}", "stale" if stale else "text", "A", right=172)
    sc.rect(178, y + 2, 38, 14, tint("stale" if stale else ("energy" if active else "watt")), "W bar track", r=3)
    sc.rect(178, y + 2, int(38 * w / 65), 14, "stale" if stale else ("energy" if active else "watt"), "W bar", r=3)
    sc.text(182, y + 2, 12, f"{w}W", "text", "W")


def pd_page(sid, state):
    sc = Screen(sid, nav("Rescan" if state != "stale" else "Clear"))
    stale = state == "stale"
    status_bar(sc, "PD", (1, 6), live=state == "live", stale="0:42" if stale else None, icon="plug")
    if state == "wait":
        flow_diagram(sc, 22, "?", "?", None, "caution", "caution", mid="wait", mtok="caution", h=88)
        card(sc, 68, 22, 156, 88, "caution", "empty card")
        sc.text(76, 26, 16, "No PD source", "text")
        check_row(sc, 76, 50, True, "CC attached")
        check_row(sc, 76, 72, False, "Source_Caps", "caution")
        sc.text(76, 94, 12, "charger on either port", "dim")
        footer(sc, "Waiting for Source_Caps", "caution", "clock")
        return sc
    t = "stale" if stale else "energy"
    card(sc, 0, 20, CW, 34, t, "contract card")
    sc.text(10, 22, 12, "CONTRACT", "dim").text(10, 35, 16, "PDO4 20.00V 3.25A", t, "contract")
    pill(sc, 0, 24, "65W", t, right=218, solid=not stale)
    for i, p in enumerate(PDOS):
        pdo_row(sc, 60 + i * 21, i, p, active=i == 3, stale=stale)
    sc.rect(4, 188, 214, 1, "rule", "divider")
    x = pill(sc, 4, 194, "PD3.0", "stale" if stale else "energy")
    x = pill(sc, x + 4, 194, "RX 12", "stale" if stale else "amp")
    pill(sc, x + 4, 194, "EPR no", "stale" if stale else "dim")
    pill(sc, 0, 194, "+2 VDM", "dim", right=216)
    if stale: footer(sc, "Unplugged 0:42 ago. OK: clear", "stale", "warn", "caution")
    else: footer(sc, "Source max 65W", "dim", "bolt", "energy")
    return sc


# ---- Wants vs Offers -----------------------------------------------------------------------------
# What the sink asked for (sniffed Request RDO, sink caps when seen) against the charger's
# Source_Capabilities. Passive only: nothing here makes the K2 transmit.
MATCH_TAGS = {  # status -> (tag text, token, solid)
    "sel": ("REQ", "energy", True), "ok": ("OK", "ok", False), "low": ("LOW A", "caution", False),
    "no": ("NO", "error", False), "unk": ("?", "faint", False),
}


def match_row(sc, y, i, p, status):
    ty, v, a, tok = p
    sel = status in ("sel", "sel_low")
    if sel:
        sc.rect(2, y - 1, CW - 6, 20, "caution_t" if status == "sel_low" else "energy_t", "selected row", r=6)
        sc.rect(4, y + 3, 3, 12, "caution" if status == "sel_low" else "energy", "request bar", r=1)
    icon, itok = {"sel": ("check", "energy"), "sel_low": ("warn", "caution"), "ok": ("check", "dim"),
                  "low": ("warn", "caution"), "no": ("cross", "error"), "unk": (None, "faint")}[status]
    if icon: sc.icon(9, y + 3, icon, itok, "match icon")
    else: sc.rect(13, y + 8, 4, 4, "faint", "unknown dot", r=1)
    sc.text(22, y, 16, str(i + 1), "dim")
    pill(sc, 34, y + 3, ty, tok, name="type pill")
    off = "stale" if status == "unk" else "text"
    sc.text(0, y, 16, v, off, "V", right=126).text(0, y, 16, a, off, "A", right=170)
    if status == "sel": tag = MATCH_TAGS["sel"]
    elif status == "sel_low": tag = ("LOW A", "caution", True)
    else: tag = MATCH_TAGS[status]
    pill(sc, 0, y + 2, tag[0], tag[1], right=218, solid=tag[2], name="match tag")


def match_page(sid, state):
    sc = Screen(sid, nav(""))
    status_bar(sc, "Match", (2, 6), icon="plug")
    card(sc, 0, 20, CW, 52, "amp" if state != "missing" else None, "wants card")
    sc.text(10, 23, 12, "DEVICE WANTS", "dim", "overline")
    if state == "missing":
        sc.text(10, 37, 16, "No request seen", "faint", "wants")
        sc.icon(10, 56, "clock", "caution", "hint icon")
        sc.text(26, 55, 12, "Replug device to capture it", "dim", "hint")
        offers = [("FIX", " 5.00", "3.00", "amp"), ("FIX", " 9.00", "3.00", "amp"), ("FIX", "15.00", "3.00", "amp"),
                  ("FIX", "20.00", "3.25", "amp"), ("PPS", "3.3-11", "5.00", "watt"), ("PPS", "3.3-21", "3.25", "watt")]
        st = ["unk"] * 6
        pill(sc, 0, 82, "65W max", "dim", right=218, name="max pill")
        foot = ("Replug device on the charger", "caution", "clock", "caution")
    elif state == "mismatch":
        sc.text(10, 37, 16, "PDO4 15.00V 3.00A", "text", "wants")
        pill(sc, 0, 24, "60W", "amp", right=218, name="wants pill")
        sc.icon(10, 56, "warn", "caution", "flag icon")
        sc.text(26, 55, 12, "Mismatch bit set", "caution", "flag")
        offers = [("FIX", " 5.00", "3.00", "amp"), ("FIX", " 9.00", "3.00", "amp"), ("FIX", "12.00", "3.00", "amp"),
                  ("FIX", "15.00", "3.00", "amp")]
        st = ["ok", "ok", "no", "sel"]
        pill(sc, 0, 82, "45W max", "dim", right=218, name="max pill")
        foot = ("Wants 20V, charger max 15V", "caution", "warn", "caution")
    else:
        limited = state == "limited"
        sc.text(10, 37, 16, "PDO4 20.00V " + ("2.25A" if limited else "3.25A"), "text", "wants")
        pill(sc, 0, 24, "45W" if limited else "65W", "amp", right=218, name="wants pill")
        sc.text(10, 55, 12, "Sink caps 5 9 15 20V", "dim", "sink caps")
        offers = [("FIX", " 5.00", "3.00", "amp"), ("FIX", " 9.00", "2.00", "amp"), ("FIX", "15.00", "2.00", "amp"),
                  ("FIX", "20.00", "2.25" if limited else "3.25", "amp"), ("PPS", "3.3-11", "3.00", "watt"),
                  ("PPS", "3.3-21", "2.25" if limited else "3.25", "watt")]
        st = ["ok", "ok", "ok", "sel_low" if limited else "sel", "no", "no"]
        pill(sc, 0, 82, "45W max" if limited else "65W max", "dim", right=218, name="max pill")
        foot = (("Charger gives 2.25A, wants 3A", "caution", "warn", "caution") if limited
                else ("Request matches the offer", "dim", "check", "ok"))
    sc.text(10, 83, 12, "CHARGER OFFERS", "dim", "overline")
    for i, (p, s_) in enumerate(zip(offers, st)):
        match_row(sc, 100 + i * 20, i, p, s_)
    footer(sc, *foot)
    return sc


# ---- Diag / Lines --------------------------------------------------------------------------------
def diag(sid):
    sc = Screen(sid, nav(""))
    status_bar(sc, "Diag", (3, 6), icon="chip")
    groups = [("USB", "amp", [("Link", "configured", "ok"), ("RST/ISR", "1 / 48211", None), ("TX/drop", "1520 / 0", None)]),
              ("PD", "energy", [("FUSB302", "ID 0x91", "ok"), ("RX", "12 on CC1", None), ("Poll gap", "12 ms", None)]),
              ("SYS", "watt", [("Meter", "INA228", "ok"), ("Uptime", "1:23:45", None)])]
    y = 21
    for name, acc, rows in groups:
        h = 4 + len(rows) * 19
        card(sc, 0, y, CW, h, acc, name)
        pill(sc, 0, y + 3, name, acc, right=218)
        for i, (k, v, st) in enumerate(rows):
            ry = y + 2 + i * 19
            sc.text(10, ry, 16, k, "dim")
            if st: sc.rect(98, ry + 5, 7, 7, "ok", "ok dot", r=3)
            sc.text(110, ry, 16, v, "text")
        y += h + 3
    footer(sc, "All subsystems up", "dim", "check", "ok")
    return sc


def lines(sid):
    sc = Screen(sid, nav(""))
    status_bar(sc, "Lines", (4, 6), icon="cable")
    rows = [("D+", 0.60, "dplus"), ("D-", 0.60, "dminus"), ("CC1", 1.68, "volt"), ("CC2", 0.00, "volt")]
    card(sc, 0, 21, CW, 4 + len(rows) * 26 + 14, None, "lines card")
    for i, (k, v, tok) in enumerate(rows):
        y = 25 + i * 26
        pill(sc, 6, y + 1, k, tok, solid=True)
        sc.text(0, y, 16, f"{v:.2f}", "text", right=96)
        sc.rect(104, y + 3, 112, 12, "bg", "track", r=3)
        if v: sc.rect(104, y + 3, int(112 * v / 3.3), 12, tok, "fill", r=3)
        for th in (0.4, 2.0):
            sc.rect(104 + int(112 * th / 3.3), y + 1, 1, 16, "dim", "threshold")
    sc.text(104 + int(112 * 0.4 / 3.3) - 6, 128, 12, "0.4", "dim").text(104 + int(112 * 2 / 3.3) - 6, 128, 12, "2.0", "dim")
    card(sc, 0, 146, CW, 72, "amp", "verdict card")
    x = pill(sc, 10, 151, "DCP short", "ok", solid=True); pill(sc, x + 6, 151, "Apple 2.4A", "amp")
    sc.icon(10, 172, "plug", "dim").text(26, 170, 16, "Rp 3.0A on CC1", "text")
    sc.icon(10, 194, "therm", "dim").text(26, 192, 16, "MCU 32°C", "dim").text(0, 192, 16, "NTC --", "faint", right=216)
    footer(sc, "D+ = D-: dedicated charger", "dim", "check", "ok")
    return sc


# ---- Tools hub -----------------------------------------------------------------------------------
TOOLS = [("bolt", "FC trigger", "charger on top or bottom", "READY", "ok"),
         ("cable", "E-marker", "cable only, no charger", "SRC ON", "caution"),
         ("phone", "Identify", "device on top or bottom", "READY", "ok"),
         ("plug", "PD trigger", "PD charger, either port", "NO PD", "error"),
         ("ohm", "Cable R", "charger + cable + load", "READY", "ok")]


def tools(sid, sel):
    sc = Screen(sid, nav("Open"))
    status_bar(sc, "Tools", (5, 6), icon="chip")
    for i, (ic, name, hint, tag, tok) in enumerate(TOOLS):
        y = 21 + i * 40
        if i == sel: row_sel(sc, y, 38)
        sc.frame(12, y + 7, 24, 24, tok, "icon badge", r=6, t=2).icon(18, y + 13, ic, tok)
        sc.text(44, y + 2, 16, name, "text")
        sc.text(44, y + 21, 12, hint, "dim", "hint")
        if i != len(TOOLS) - 1 and i != sel and i + 1 != sel: sc.rect(44, y + 39, 172, 1, "rule", "divider")
        if i == sel: pill(sc, 0, y + 3, tag, tok, right=214, solid=True, name="tag")
        else: sc.rect(206, y + 7, 8, 8, tok, "state dot", r=4)
    why = {0: ("Ready", "dim", "check", "ok"), 1: ("Charger present: listen only", "caution", "warn", "caution"),
           3: ("No PD source on either port", "error", "warn", "error")}[sel]
    footer(sc, *why)
    return sc


def sbar(sc, name, icon, live=True):
    status_bar(sc, name, None, live=live, icon=icon)


# ---- E-marker ------------------------------------------------------------------------------------
def emarker(sid, state):
    ok = {"setup": "Scan", "sniff": "Listen", "scan": "", "result": "Rescan", "none": "Rescan"}[state]
    sc = Screen(sid, sub(ok, "Up" if state == "result" else "", "Down" if state == "result" else ""))
    sbar(sc, "E-marker", "cable", live=state == "sniff")
    if state in ("setup", "sniff", "scan"):
        if state == "sniff":
            flow_diagram(sc, 22, None, "CBL", "up", "volt", "ok")
        else:
            flow_diagram(sc, 22, "K2", "CBL", "down" if state == "scan" else None, "focus", "ok",
                         mid="VCON" if state == "setup" else None, mtok="focus")
        card(sc, 68, 22, 156, 72, "ok" if state != "sniff" else "caution", "checklist")
        check_row(sc, 76, 26, True, "Cable seen")
        check_row(sc, 76, 48, state != "sniff", "No charger", "caution")
        check_row(sc, 76, 70, state != "sniff", "VCONN ok", "caution")
        if state == "scan":
            card(sc, 0, 100, CW, 50, "volt", "progress card")
            sc.text(10, 104, 16, "Scanning SOP'", "volt")
            for k in range(4): sc.rect(10 + k * 52, 128, 48, 8, "volt" if k < 2 else "rule", "step seg", r=3)
            footer(sc, "Step 2 of 4. Back cancels", "volt", "clock", "volt")
        elif state == "sniff":
            sc.text(4, 102, 12, "K2 listens to the charger asking", "dim").text(4, 116, 12, "the cable. Replug the charger.", "dim")
            footer(sc, "Charger present: listen only", "caution", "warn", "caution")
        else:
            footer(sc, "Ready", "dim", "check", "ok")
    elif state == "none":
        card(sc, 0, 22, CW, 64, "caution", "verdict card")
        sc.icon(10, 30, "warn", "caution").text(28, 26, 24, "No e-marker", "caution")
        x = pill(sc, 10, 60, "3A max", "caution", solid=True); x = pill(sc, x + 6, 60, "passive", "dim"); pill(sc, x + 6, 60, "20V", "dim")
        footer(sc, "No reply to SOP' x3", "caution", "warn", "caution")
    else:
        card(sc, 0, 20, CW, 60, "energy", "verdict card")
        sc.text(10, 22, 24, "5A", "amp").text(48, 22, 24, "50V", "volt").text(102, 22, 24, "240W", "watt")
        x = pill(sc, 10, 54, "EPR", "energy", solid=True); x = pill(sc, x + 6, 54, "USB3.2 G2", "amp"); pill(sc, x + 6, 54, "passive", "dim")
        rows = [("Vendor", "Apple"), ("VID", "05AC"), ("PID", "1234"), ("HW/FW", "1 / 2"), ("Length", "<2 m"), ("Latency", "<10 ns")]
        for i, (k, v) in enumerate(rows):
            y = 86 + i * 21
            sc.text(6, y, 16, k, "dim").text(0, y, 16, v, "text", right=216)
            if i < 5: sc.rect(6, y + 19, 210, 1, "rule")
        sc.rect(221, 86, 3, 126, "rule", "scroll track", r=1).rect(221, 86, 3, 54, "dim", "scroll thumb", r=1)
        footer(sc, "Read 0:05 ago  6/14", "dim", "check", "ok")
    return sc


# ---- Identify ------------------------------------------------------------------------------------
def identify(sid, state):
    sc = Screen(sid, sub({"setup": "Run", "refused": "Force", "result": "Rerun"}[state],
                         "Up" if state == "result" else "", "Down" if state == "result" else ""))
    sbar(sc, "Identify", "phone", live=state == "refused")
    if state == "result":
        card(sc, 0, 22, CW, 50, "energy", "verdict card")
        sc.icon(10, 30, "phone", "energy").text(28, 24, 24, "iPad Air 5", "energy")
        x = pill(sc, 10, 52, "Apple", "dim"); x = pill(sc, x + 6, 52, "DRP", "amp"); pill(sc, x + 6, 52, "wants 20V 1.5A", "volt")
        for i, (k, v) in enumerate([("VID", "05AC"), ("PID", "1A2B"), ("SVID", "05AC Apple"), ("SVID", "8087 TBT"), ("Port", "top")]):
            y = 78 + i * 21
            sc.text(6, y, 16, k, "dim").text(0, y, 16, v, "text", right=216)
            sc.rect(6, y + 19, 210, 1, "rule")
        footer(sc, "Identified 0:03 ago", "dim", "check", "ok")
        return sc
    if state == "setup":
        flow_diagram(sc, 22, "K2", "DEV", None, "focus", "ok", mid="SRC", mtok="focus")
    else:
        flow_diagram(sc, 22, None, None, "up", "volt", "caution")
    card(sc, 68, 22, 156, 72, "ok" if state == "setup" else "caution", "checklist")
    check_row(sc, 76, 26, True, "Device seen")
    check_row(sc, 76, 48, state == "setup", "No charger", "caution")
    check_row(sc, 76, 70, state == "setup", "No live sink", "caution")
    footer(sc, "Ready" if state == "setup" else "Live sink. OK again forces",
           "dim" if state == "setup" else "caution", "check" if state == "setup" else "warn",
           "ok" if state == "setup" else "caution")
    return sc


# ---- FC trigger ----------------------------------------------------------------------------------
def fc(sid, state):
    labels = {"setup": sub("Start", "-V", "+V"), "run": sub("Stop", "-V", "+V"), "scan": sub("Rescan"), "refused": sub("")}[state]
    sc = Screen(sid, labels)
    sbar(sc, "FC trigger", "bolt", live=state != "refused")
    if state == "refused":
        flow_diagram(sc, 22, "?", "?", None, "error", "error", mid="none", mtok="error")
        card(sc, 68, 22, 156, 72, "error", "checklist")
        check_row(sc, 76, 26, False, "VBUS present")
        check_row(sc, 76, 48, True, "D+/D- free")
        sc.text(76, 72, 12, "top or bottom port", "dim")
        footer(sc, "No VBUS on either port", "error", "warn", "error")
        return sc
    flow_diagram(sc, 22, None, None, "up", "text", "text", mid="1.2A", mtok="amp", h=96)
    if state == "scan":
        grid = [("QC2 9V", "ok"), ("QC2 12V", "ok"), ("QC3", "ok"), ("AFC 9V", "error"),
                ("FCP 9V", "error"), ("SCP", "error"), ("VOOC", "faint"), ("UFCS", "faint")]
        for i, (t, tok) in enumerate(grid):
            x, y = 68 + (i % 2) * 78, 22 + (i // 2) * 24
            sc.rect(x, y, 76, 22, tint(tok), "result tile", r=5)
            sc.icon(x + 4, y + 5, "check" if tok == "ok" else "cross" if tok == "error" else "clock", tok)
            sc.text(x + 18, y + 4, 12, t, "text" if tok == "ok" else "dim")
        card(sc, 0, 122, CW, 40, "ok", "summary")
        sc.text(10, 124, 24, "3 of 8", "ok").text(100, 130, 16, "max 12V", "text")
        footer(sc, "Scan done", "dim", "check", "ok")
        return sc
    for x, cap, val, tok in ((68, "TARGET", " 9.00", "volt"), (148, "VBUS", " 9.02" if state == "run" else " 5.01", "volt")):
        card(sc, x, 22, 76, 44, None, cap)
        sc.text(x + 6, 24, 12, cap, "dim").text(x + 2, 38, 16, val + "V", tok)
        gauge(sc, x + 6, 58, 64, (9.0 if x == 68 else (9.02 if state == "run" else 5.01)) / 12, tok, cap)
    x = pill(sc, 68, 72, "QC3.0", "amp", solid=True)
    x = pill(sc, x + 4, 72, "Holding" if state == "run" else "Idle", "ok" if state == "run" else "dim")
    pill(sc, 68, 92, "1.20A", "amp")
    card(sc, 0, 124, CW, 94, None, "trace card")
    sc.text(6, 126, 12, "VBUS 10 s", "dim").text(0, 126, 12, "12V", "faint", right=216)
    sc.spark(6, 142, 210, 72, wave(105, 5, 0.05, step=state == "run"), "volt", "VBUS trace 10 s")
    footer(sc, "Long -/+: protocol", "dim", "bolt", "volt")
    return sc


# ---- PD trigger ----------------------------------------------------------------------------------
def pdt(sid, state):
    labels = {"list": sub("Send", "Prev", "Next"), "adjust": sub("Send", "-0.1", "+0.1"), "refused": sub("")}[state]
    sc = Screen(sid, labels)
    sbar(sc, "PD trigger", "plug", live=state != "refused")
    if state == "refused":
        flow_diagram(sc, 22, "?", "?", None, "error", "error", mid="caps", mtok="error")
        card(sc, 68, 22, 156, 72, "error", "checklist")
        check_row(sc, 76, 26, False, "PD charger")
        check_row(sc, 76, 48, True, "No live sink")
        sc.text(76, 72, 12, "either port", "dim")
        footer(sc, "No PD source on either port", "error", "warn", "error")
        return sc
    cur = 5 if state == "adjust" else 2
    for i, p in enumerate(PDOS):
        pdo_row(sc, 22 + i * 21, i, p, active=i == 3, cursor=i == cur)
    if state == "adjust":
        card(sc, 0, 152, CW, 66, "focus", "adjust card")
        sc.text(10, 154, 12, "REQUEST", "dim").text(0, 154, 12, "long OK: V/A", "dim", right=216)
        sc.text(10, 168, 24, "12.40V", "volt").text(124, 168, 24, "2.00A", "amp")
        sc.rect(10, 194, 84, 2, "focus", "edit underline", r=1)
        gauge(sc, 10, 204, 206, (12.4 - 3.3) / (21 - 3.3), "volt", "range 3.3-21V", h=5)
        footer(sc, "Editing V, 20 mV steps", "volt", "bolt", "volt")
    else:
        card(sc, 0, 152, CW, 66, "energy", "contract card")
        sc.text(10, 156, 12, "NOW", "dim").text(10, 170, 24, "PDO4 20V", "energy")
        sc.text(10, 198, 12, "OK requests PDO3 15V", "dim")
        sc.text(170, 162, 24, "↑", "flow")
        footer(sc, "Power flows up, 20V", "dim", "check", "ok")
    return sc


# ---- Cable R -------------------------------------------------------------------------------------
def cable_r(sid, state):
    sc = Screen(sid, sub("Reset", "", "Mode"))
    sbar(sc, "Cable R", "ohm", live=state != "error")
    if state == "error":
        card(sc, 0, 22, CW, 60, "error", "err card")
        sc.icon(10, 32, "warn", "error").text(28, 26, 24, "Meter missing", "error")
        sc.text(10, 58, 12, "Cable R needs INA2xx. See Diag.", "dim")
        footer(sc, "INA2xx not on I2C", "error", "warn", "error")
        return sc
    flow_diagram(sc, 22, None, None, "up", "text", "text", mid="3.0A" if state == "result" else "idle", mtok="amp", h=84)
    for i, (cap, val, done) in enumerate((("IDLE", "20.04V 0.05A", True),
                                          ("LOAD", "19.71V 3.01A" if state == "result" else ">= 1.0A", state == "result"))):
        y = 22 + i * 42
        card(sc, 68, y, 156, 38, "ok" if done else "caution", cap)
        sc.rect(76, y + 8, 22, 22, "ok" if done else "caution", "step tile", r=6)
        sc.text(82, y + 10, 16, str(i + 1), "bg")
        sc.text(104, y + 3, 12, cap, "dim").text(104, y + 17, 16, val, "volt" if done else "caution")
    if state == "result":
        card(sc, 0, 110, CW, 108, "ok", "result card")
        sc.text(8, 114, 48, "110", "energy").text(98, 127, 24, "mΩ", "energy")
        pill(sc, 0, 120, "GOOD", "ok", right=216, solid=True)
        for k, (tok, w) in enumerate((("ok", 70), ("caution", 70), ("error", 66))):
            sc.rect(10 + k * 70, 158, w - 2, 6, tok, "grade seg", r=2)
        mx = 10 + int(206 * 0.22)
        sc.poly([(mx - 4, 170), (mx + 4, 170), (mx, 165)], "text", "grade marker")
        sc.text(10, 172, 12, "<150", "dim").text(80, 172, 12, "<300", "dim").text(0, 172, 12, ">300", "dim", right=216)
        sc.text(10, 192, 16, "0.33V drop, 1.0W lost", "text")
        footer(sc, "Long +: Kelvin mode", "dim", "check", "ok")
    else:
        footer(sc, "Draw at least 1A", "caution", "bolt", "caution")
    return sc


SCREENS = [
    home("home_live", "live"), home("home_reverse", "live", flow="down", v=" 5.12", a="0.840", w=" 4.30"),
    home("home_idle", "idle"), home("home_stale", "stale"), home("home_error", "error"), reboot_hold("home_reboot_hold"),
    pd_page("pd_live", "live"), pd_page("pd_wait", "wait"), pd_page("pd_stale", "stale"),
    match_page("match_ok", "ok"), match_page("match_limited", "limited"), match_page("match_mismatch", "mismatch"),
    match_page("match_missing", "missing"),
    diag("diag"), lines("lines"),
    tools("tools_ready", 0), tools("tools_caution", 1), tools("tools_blocked", 3),
    emarker("emarker_setup", "setup"), emarker("emarker_sniff", "sniff"), emarker("emarker_scan", "scan"),
    emarker("emarker_result", "result"), emarker("emarker_none", "none"),
    identify("identify_setup", "setup"), identify("identify_refused", "refused"), identify("identify_result", "result"),
    fc("fc_setup", "setup"), fc("fc_run", "run"), fc("fc_scan", "scan"), fc("fc_refused", "refused"),
    pdt("pdt_list", "list"), pdt("pdt_adjust", "adjust"), pdt("pdt_refused", "refused"),
    cable_r("cabler_setup", "setup"), cable_r("cabler_result", "result"), cable_r("cabler_error", "error"),
]


# ---- rendering -----------------------------------------------------------------------------------
def draw_panel(img, d, labels):
    d.rectangle([224, 0, 239, 239], fill=rgb("panel"))
    for i, (y0, ln) in enumerate(SLOTS):
        if i: d.rectangle([224, y0, 239, y0 + 1], fill=rgb("line"))
        t = labels[i]
        if not t: continue
        tmp = Image.new("RGB", (len(t) * 7 + 2, 15), rgb("panel"))
        ImageDraw.Draw(tmp).text((1, 13), t, font=FONTS[12], fill=rgb("text"), anchor="ls")
        rot = tmp.rotate(90, expand=True)
        img.paste(rot, (225, y0 + (ln - rot.height) // 2))


def rgb(name):
    v = TOK[name]
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def render(sc):
    img = Image.new("RGB", (240, 240), rgb("bg"))
    d = ImageDraw.Draw(img)
    for op in sc.ops:
        k = op[0]
        if k == "rect":
            _, x, y, w, h, tok, _, r = op
            if w <= 0 or h <= 0: continue
            if r: d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, fill=rgb(tok))
            else: d.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb(tok))
        elif k == "frame":
            _, x, y, w, h, tok, _, r, t = op
            d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=r, outline=rgb(tok), width=t)
        elif k == "text":
            _, x, y, size, s, tok, _ = op
            d.text((x, y + ASC[size]), s, font=FONTS[size], fill=rgb(tok), anchor="ls")
        elif k == "icon":
            _, x, y, key, tok, _ = op
            for r_, row in enumerate(ICONS[key]):
                for c, ch in enumerate(row):
                    if ch == "#": d.point((x + c, y + r_), fill=rgb(tok))
        elif k == "poly":
            d.polygon(op[1], fill=rgb(op[2]))
        elif k == "spark":
            _, x, y, w, h, data, tok, _ = op
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb("bg"))
            for gy in (y, y + h // 2, y + h - 1):
                d.line([x, gy, x + w - 1, gy], fill=rgb("rule"))
            if data:
                lo, hi = min(data), max(data); hi = hi if hi > lo else lo + 1
                pts = [(x + i * (w - 1) / max(1, len(data) - 1), y + h - 2 - (v - lo) / (hi - lo) * (h - 4)) for i, v in enumerate(data)]
                for px, py in pts:
                    d.line([px, py, px, y + h - 2], fill=tuple(c // 4 for c in rgb(tok)))
                d.line(pts, fill=rgb(tok), width=1)
    draw_panel(img, d, sc.labels)
    img.resize((480, 480), Image.NEAREST).save(OUT / f"{sc.sid}.png")


def ascii_sketch(sc):
    W, H = 28, 20
    g = [[" "] * W + ["|"] for _ in range(H)]
    for op in sc.ops:
        k = op[0]
        if k in ("rect", "frame") and op[3] >= 60 and op[4] >= 20 and "card" in op[6] or k == "rect" and "diagram" in op[6]:
            x, y, w, h = op[1:5]
            for rr in (y // 12, min(H - 1, (y + h - 1) // 12)):
                for c in range(x // 8, min(W, (x + w) // 8)):
                    if g[rr][c] == " ": g[rr][c] = "."
        elif k == "text":
            _, x, y, size, s, tok, name = op
            step = max(1, round(ADV[size] / 8)); r = min(H - 1, (y + LH[size] // 2) // 12); c = x // 8
            for ch in s:
                if 0 <= c < W: g[r][c] = ch if ch.isascii() else {"→": ">", "←": "<", "↑": "^", "↓": "v", "Ω": "O", "°": "o"}.get(ch, "?")
                for kk in range(1, step):
                    if 0 <= c + kk < W: g[r][c + kk] = " "
                c += step
        elif k == "icon":
            g[min(H - 1, (op[2] + 6) // 12)][op[1] // 8] = "*"
        elif k == "poly" and op[3] == "chevron":
            px, py = op[1][0]; g[min(H - 1, py // 12)][min(W - 1, px // 8)] = "^" if op[1][1][1] < py else "v"
        elif k == "spark":
            _, x, y, w, h = op[:5]
            for c in range(x // 8, min(W, (x + w) // 8)): g[min(H - 1, (y + h // 2) // 12)][c] = "~"
    lines = ["    " + "".join(str(i % 10) for i in range(W)) + "|"]
    lines += [f"{r*12:3d} " + "".join(row) for r, row in enumerate(g)]
    return "\n".join(lines)


def table(sc):
    rows = ["| element | kind | x | y | w | h | font / radius | token |", "|---|---|---|---|---|---|---|---|"]
    for op in sc.ops:
        k = op[0]
        if k in ("rect", "frame"):
            _, x, y, w, h, tok, name, r = op[:8]
            if name: rows.append(f"| {name} | {k} | {x} | {y} | {w} | {h} | r{r} | `{tok}` |")
        elif k == "text":
            _, x, y, size, s, tok, name = op
            rows.append(f"| {name or repr(s)} | text | {x} | {y} | {len(s)*ADV[size]} | {LH[size]} | {size}px | `{tok}` |")
        elif k == "icon":
            rows.append(f"| {op[5]} | icon | {op[1]} | {op[2]} | 12 | 12 | - | `{op[4]}` |")
        elif k == "spark":
            _, x, y, w, h, data, tok, name = op
            rows.append(f"| {name} | spark | {x} | {y} | {w} | {h} | - | `{tok}` |")
    return "\n".join(rows)


def main():
    md = ["# K2 UI mockups v2: generated screens", "",
          "Generated by `render_mockups.py`; do not edit by hand. Sketch cell = 8 x 12 px; `*` icon, `^`/`v` flow",
          "chevrons, `~` sparkline, `.` card edge. Tables give pixels in the 224 x 240 content area (panel x 224..239).",
          "Chevrons and port symbols are listed in the code, not the table (polygons).", ""]
    for sc in SCREENS:
        render(sc)
        md += [f"## {sc.sid}", "", f"![{sc.sid}]({sc.sid}.png)", "",
               "Panel: " + " / ".join(l or "(empty)" for l in sc.labels), "", "```", ascii_sketch(sc), "```", "",
               "<details><summary>Element table</summary>", "", table(sc), "", "</details>", ""]
    (OUT / "screens.md").write_text("\n".join(md))
    print(f"{len(SCREENS)} screens -> {OUT}")


if __name__ == "__main__":
    main()
