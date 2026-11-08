#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Render the docs/UI_DESIGN.md mockups: one 240x240 PNG per screen state (saved at x2) and an
ASCII sketch + element table per screen (docs/ui_mockups/screens.md).

Design reference only; nothing here runs on the device. Uses the repo's JetBrains Mono TTF, so
glyph metrics match tools/gen_font.py (12/16/24 px full ASCII, 48 px digits only; 16/24 px also
carry ° · Ω ← ↑ → ↓). Colours are RGB565 tokens converted to RGB888.

    python3 docs/ui_mockups/render_mockups.py
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[2]
TTF = ROOT / "third_party/fonts/jetbrains-mono/JetBrainsMono-Regular.ttf"
OUT = Path(__file__).resolve().parent

# ---- tokens ------------------------------------------------------------------------------------
TOK = {  # name: RGB565
    "bg": 0x0000, "surface": 0x18E3, "bar": 0x08A9, "panel": 0x2104, "line": 0x8410, "rule": 0x2945,
    "text": 0xFFFF, "dim": 0x8410, "faint": 0x4208,
    "volt": 0xFFE0, "amp": 0x07FF, "watt": 0xFD20, "energy": 0x07E0, "charge": 0x5D1F,
    "flow_fwd": 0x07E0, "flow_rev": 0xF81F,
    "ok": 0x07E0, "caution": 0xFC00, "error": 0xF800, "info": 0x5D1F, "stale": 0xC618,
    "focus": 0x07FF,
}
ADV = {12: 7, 16: 10, 24: 14, 48: 29}
ASC = {12: 11, 16: 14, 24: 22, 48: 35}
LH = {12: 13, 16: 17, 24: 26, 48: 35}
FONTS = {s: ImageFont.truetype(str(TTF), s, layout_engine=ImageFont.Layout.BASIC) for s in ADV}  # no ligatures, like the device
CW, PANEL_X = 224, 224
SLOTS = [(0, 66), (66, 54), (120, 54), (174, 66)]


def rgb(name):
    v = TOK[name] if isinstance(name, str) else name
    r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


class Screen:
    def __init__(self, sid, title, labels):
        self.sid, self.title, self.labels = sid, title, labels
        self.ops = []      # ("rect", x, y, w, h, tok) | ("text", x, y, size, s, tok, bgtok)

    def rect(self, x, y, w, h, tok, name=""):
        self.ops.append(("rect", x, y, w, h, tok, name)); return self

    def text(self, x, y, size, s, tok, name="", right=None):
        if right is not None: x = right - len(s) * ADV[size]
        self.ops.append(("text", x, y, size, s, tok, name)); return self


# ---- shared organisms ----------------------------------------------------------------------------
def status_bar(sc, title, page=None, live=True, stale=None, mini=True, mx=60):
    """y 0..17. Title (12 px) left; mini V/A readout; page n/N or LAST badge right."""
    sc.rect(0, 0, CW, 18, "bar", "status bar")
    sc.text(4, 3, 12, title, "text", "title")
    if mini:
        if stale:
            sc.text(mx, 3, 12, "20.01V 2.99A", "stale", "mini readout (stale)")
        elif live:
            sc.text(mx, 3, 12, "20.01V", "volt", "mini V").text(mx + 46, 3, 12, "2.99A", "amp", "mini A")
            sc.text(mx + 84, 2, 16, "→", "flow_fwd", "mini flow")
        else:
            sc.text(mx, 3, 12, "--.--V  -.--A", "faint", "mini readout (none)")
    if stale:
        sc.rect(158, 2, 64, 14, "stale", "LAST badge").text(161, 3, 12, "LAST" + stale, "bg", "badge text")
    elif page:
        sc.text(0, 3, 12, page, "dim", "page n/N", right=220)


def footer(sc, msg, tok="dim", dot=None):
    """y 222..239: one-line status / reason. Optional 6x6 state dot."""
    sc.rect(0, 222, CW, 18, "bar", "footer")
    x = 4
    if dot:
        sc.rect(4, 228, 6, 6, dot, "state dot"); x = 14
    sc.text(x, 225, 12, msg, tok, "footer text")


def scrollbar(sc, y0, h, pos, frac):
    sc.rect(221, y0, 3, h, "rule", "scroll track")
    th = max(8, int(h * frac)); sc.rect(221, y0 + int((h - th) * pos), 3, th, "dim", "scroll thumb")


def checklist(sc, y, items):
    """Setup checklist: 6x6 dot + 16 px line, 20 px pitch. items: (state, text) state ok|bad|wait."""
    tokmap = {"ok": "ok", "bad": "error", "wait": "caution"}
    for i, (st, t) in enumerate(items):
        sc.rect(6, y + i * 20 + 6, 6, 6, tokmap[st], f"check {st}")
        sc.text(18, y + i * 20, 16, t, "text" if st != "ok" else "dim", "check text")


def port_strip(sc, y, left, right, lt="dim", rt="dim"):
    """Wiring diagram: [left] IN ▸ K2 ▸ OUT [right], 16 px, one line."""
    sc.rect(0, y - 2, CW, 21, "surface", "wiring strip")
    sc.text(4, y, 16, f"{left:>5}", lt, "IN side")
    sc.text(56, y, 16, "→IN K2 OUT→", "text", "ports")
    sc.text(170, y, 16, f"{right:<5}", rt, "OUT side")


NAV = ["Back", "-/Prev", "", "+/Next"]


def nav(ok):
    return ["Back", "-/Prev", ok, "+/Next"]


def sub(ok, minus="Prev", plus="Next"):
    return ["Back", minus, ok, plus]


# ---- Home ----------------------------------------------------------------------------------------
def home(sid, v, a, w, flow, state):
    labels = nav("Rst*")
    sc = Screen(sid, "Home", labels)
    stale = state == "stale"
    sc.rect(0, 0, CW, 18, "bar", "status bar")
    sc.text(4, 3, 12, "PD3.0" if state == "live" else "noPD" if state == "idle" else "PD3.0",
            "text" if state == "live" else "dim", "PD rev")
    sc.text(46, 3, 12, "CC1" if state != "idle" else "CC-", "amp" if state == "live" else "dim", "CC pin")
    sc.text(74, 3, 12, "INA228", "dim", "meter chip")
    sc.text(124, 3, 12, "32C", "dim", "MCU temp")
    if stale:
        sc.rect(158, 2, 64, 14, "stale", "LAST badge").text(161, 3, 12, "LAST0:42", "bg", "badge text")
    else:
        sc.text(0, 3, 12, "1/5", "dim", "page n/N", right=220)
    vt, at, wt = ("volt", "amp", "watt") if state == "live" else ("stale",) * 3 if stale else ("faint",) * 3
    if state == "error": vt = at = wt = "faint"
    for (y, val, unit, tok, nm) in ((22, v, "V", vt, "V"), (66, a, "A", at, "A"), (110, w, "W", wt, "W")):
        sc.text(0, y, 48, val, tok, f"{nm} digits", right=178)
        sc.text(182, y + 35 - 22, 24, unit, tok, f"{nm} unit")
    if flow:
        sc.text(204, 66 + 13, 24, flow, "flow_fwd" if flow == "→" else "flow_rev", "flow arrow")
        sc.text(200, 66 + 38 - 4, 12, "fwd" if flow == "→" else "rev", "dim", "flow caption")
    sc.rect(0, 152, CW, 1, "rule", "divider")
    et = "energy" if state == "live" else "stale" if stale else "faint"
    sc.text(4, 158, 16, "Wh", "dim", "Wh label").text(0, 158, 16, "12.345" if state != "error" else "--.---", et, "Wh", right=106)
    sc.text(116, 158, 16, "Ah", "dim", "Ah label").text(0, 158, 16, "0.617" if state != "error" else "-.---",
                                                           "charge" if state == "live" else et, "Ah", right=216)
    sc.text(4, 180, 16, "t", "dim", "time label").text(0, 180, 16, "01:23:45", "text" if state == "live" else et, "elapsed", right=106)
    sc.text(116, 180, 16, "R", "dim", "load R label").text(0, 180, 16, "6.69Ω" if state == "live" else "--Ω", et, "load R", right=216)
    sc.text(4, 202, 12, "D+0.60 D-0.60", "dim", "D lines")
    sc.text(0, 202, 12, "QC3 9V" if state == "live" else "--", "amp" if state == "live" else "faint", "FC protocol", right=216)
    msgs = {
        "live": ("Live  avg 1s  Long OK: reset", "dim", "ok"),
        "idle": ("No VBUS. Source to IN.", "text", "caution"),
        "stale": ("Unplugged. Values frozen 0:42", "stale", "caution"),
        "error": ("Meter not found. See Diag.", "error", "error"),
    }[state]
    footer(sc, msgs[0], msgs[1], msgs[2])
    return sc


# ---- PD ------------------------------------------------------------------------------------------
PDOS = [("FIX", " 5.00", "3.00", " 15", "amp"), ("FIX", " 9.00", "3.00", " 27", "amp"),
        ("FIX", "15.00", "3.00", " 45", "amp"), ("FIX", "20.00", "3.25", " 65", "amp"),
        ("PPS", " 3.3-11", "5.00", " 55", "watt"), ("PPS", " 3.3-21", "3.25", " 65", "watt")]


def pd_page(sid, state):
    sc = Screen(sid, "PD", nav("Rescan"))
    stale = "0:42" if state == "stale" else None
    status_bar(sc, "PD", "2/5", live=state == "live", stale=stale)
    if state == "wait":
        sc.text(4, 60, 24, "No PD source", "text", "empty title")
        sc.text(4, 96, 16, "Plug a PD charger", "dim", "empty hint")
        sc.text(4, 116, 16, "into IN.", "dim", "empty hint")
        sc.text(4, 146, 16, "OK: Rescan", "amp", "empty action")
        footer(sc, "Waiting for Source_Caps...", "caution", "caution")
        return sc
    t = "stale" if stale else None
    sc.rect(0, 20, CW, 21, "surface", "contract strip")
    sc.text(4, 22, 16, "4", t or "energy", "contract index").text(20, 22, 16, "FIX 20.00V 3.25A 65W", t or "energy", "contract")
    sc.text(4, 45, 12, "#  TYP     V      A    W", "dim", "column head")
    for i, (ty, v, a, w, tok) in enumerate(PDOS):
        y = 60 + i * 20
        mark = i == 3
        if mark:
            sc.rect(0, y - 2, 3, 20, t or "energy", "active marker")
        sc.text(6, y, 16, f"{i+1} {ty}{v:>7} {a} {w}", t or ("text" if mark else tok), f"PDO {i+1}")
    scrollbar(sc, 60, 158, 0.0, 0.7)
    if stale:
        footer(sc, "Unplugged 0:42 ago. OK: clear", "stale", "caution")
    else:
        footer(sc, "RX 12  PD3.0  Src 65W  EPR no", "dim", "ok")
    return sc


# ---- Diag / Lines ------------------------------------------------------------------------------
def kv_page(sid, title, page, rows, foot, ok=""):
    sc = Screen(sid, title, nav(ok))
    status_bar(sc, title, page)
    for i, (k, v, tok) in enumerate(rows):
        y = 22 + i * 20
        sc.text(4, y, 16, k, "dim", f"key {k}")
        sc.text(0, y, 16, v, tok, f"value {k}", right=216)
    footer(sc, foot[0], foot[1], foot[2])
    return sc


def lines_page(sid):
    sc = Screen(sid, "Lines", nav(""))
    status_bar(sc, "Lines", "4/5")
    rows = [("D+", 0.60, "volt"), ("D-", 0.60, "volt"), ("CC1", 1.68, "amp"), ("CC2", 0.00, "amp"),
            ("AUX", 0.00, "dim")]
    for i, (k, v, tok) in enumerate(rows):
        y = 22 + i * 24
        sc.text(4, y, 16, k, "dim", f"{k} label")
        sc.text(0, y, 16, f"{v:.2f}V", tok, f"{k} value", right=108)
        sc.rect(116, y + 4, 100, 10, "rule", f"{k} bar track")
        sc.rect(116, y + 4, int(100 * v / 3.3), 10, tok, f"{k} bar fill")
    sc.rect(116 + int(100 * 0.4 / 3.3), 22, 1, 118, "faint", "0.4V tick")
    sc.rect(0, 146, CW, 1, "rule", "divider")
    sc.text(4, 152, 16, "MCU", "dim").text(0, 152, 16, "32°C", "text", right=106)
    sc.text(116, 152, 16, "VREF", "dim").text(0, 152, 16, "1.20V", "text", right=216)
    sc.text(4, 174, 16, "Rp", "dim").text(0, 174, 16, "3.0A (CC1)", "amp", "Rp advert", right=216)
    sc.text(4, 196, 16, "FC", "dim").text(0, 196, 16, "Apple 2.4A", "amp", "FC guess", right=216)
    footer(sc, "D lines: DCP short detected", "dim", "ok")
    return sc


# ---- Tools hub ---------------------------------------------------------------------------------
TOOLS = [("Fast-charge trigger", "Source on IN", "READY", "ok"),
         ("E-marker", "Cable on OUT", "SRC ON", "caution"),
         ("Identify device", "Device on OUT", "READY", "ok"),
         ("PD trigger", "PD charger on IN", "NO PD", "error"),
         ("Cable resistance", "Cable IN-OUT + load", "READY", "ok")]


def tools_page(sid, sel):
    sc = Screen(sid, "Tools", nav("Open"))
    status_bar(sc, "Tools", "5/5")
    for i, (name, hint, tag, tok) in enumerate(TOOLS):
        y = 22 + i * 40
        if i == sel:
            sc.rect(0, y, CW, 38, "surface", "selected row").rect(0, y, 3, 38, "focus", "focus bar")
        sc.text(8, y + 2, 16, name, "text", f"tool {i}")
        sc.text(8, y + 21, 12, hint, "dim", f"setup hint {i}")
        sc.text(0, y + 22, 12, tag, tok, f"readiness {i}", right=216)
    why = {0: ("OK opens. Source 20V on IN.", "dim", "ok"),
           1: ("Source on IN: scan only listens", "caution", "caution"),
           3: ("No PD source on IN. Plug one.", "error", "error")}[sel]
    footer(sc, *why)
    return sc


def sub_bar(sc, name, live=True):
    status_bar(sc, "<" + name, None, live=live, mx=118)


# ---- E-marker ----------------------------------------------------------------------------------
def emarker(sid, state):
    ok = {"setup": "Scan", "sniff": "Listen", "scan": "", "result": "Rescan", "none": "Rescan"}[state]
    sc = Screen(sid, "E-marker", sub(ok, "Up", "Down") if state == "result" else sub(ok, "", ""))
    sub_bar(sc, "E-marker", live=state == "sniff")
    if state in ("setup", "sniff"):
        port_strip(sc, 24, "none" if state == "setup" else "SRC", "CABLE",
                   "dim" if state == "setup" else "caution", "ok")
        checklist(sc, 54, [("ok", "Cable on OUT"),
                           ("ok" if state == "setup" else "bad", "No source on IN" if state == "setup" else "Source on IN"),
                           ("ok", "VCONN available")])
        if state == "setup":
            sc.text(4, 124, 16, "K2 powers VCONN and", "dim").text(4, 144, 16, "reads the cable ID.", "dim")
            footer(sc, "Ready. OK: Scan", "ok", "ok")
        else:
            sc.text(4, 124, 16, "Can't drive VCONN with", "text").text(4, 144, 16, "a source on IN. OK", "text")
            sc.text(4, 164, 16, "listens; then replug", "text").text(4, 184, 16, "the charger.", "text")
            footer(sc, "Source on IN: listen only", "caution", "caution")
    elif state == "scan":
        sc.text(4, 60, 24, "Scanning...", "volt", "progress title")
        sc.rect(4, 96, 212, 8, "rule", "progress track").rect(4, 96, 120, 8, "volt", "progress fill")
        sc.text(4, 112, 16, "SOP' Discover Identity", "dim", "step")
        footer(sc, "Scanning 2/4. Back cancels", "volt", "caution")
    elif state == "none":
        sc.text(4, 50, 24, "No e-marker", "watt", "result title")
        sc.text(4, 86, 16, "Cable gave no answer.", "text")
        sc.text(4, 106, 16, "Passive cable: 3A max", "dim")
        sc.text(4, 126, 16, "(USB-C spec default).", "dim")
        footer(sc, "No reply to SOP' x3", "watt", "caution")
    else:
        sc.rect(0, 20, CW, 46, "surface", "verdict card")
        sc.text(4, 22, 24, "5A  50V  EPR", "energy", "verdict")
        sc.text(4, 48, 12, "Passive  USB 3.2 Gen2  <2m", "text", "verdict line 2")
        rows = [("Vendor", "Apple"), ("VID", "05AC"), ("PID", "1234 HW1 FW2"), ("SOP'", "PD3.x"),
                ("Lat", "<10ns"), ("Term", "VCONN not req")]
        for i, (k, v) in enumerate(rows):
            y = 72 + i * 20
            sc.text(4, y, 16, k, "dim").text(0, y, 16, v, "text", right=216)
        scrollbar(sc, 72, 146, 0.0, 0.5)
        footer(sc, "Read 0:05 ago  6 of 14 rows", "dim", "ok")
    return sc


# ---- Identify ------------------------------------------------------------------------------------
def identify(sid, state):
    sc = Screen(sid, "Identify", sub({"setup": "Run", "refused": "Run", "result": "Rerun"}[state],
                                     "Up" if state == "result" else "", "Down" if state == "result" else ""))
    sub_bar(sc, "Identify", live=False)
    if state == "result":
        sc.rect(0, 20, CW, 30, "surface", "verdict card")
        sc.text(4, 22, 24, "iPad Air 5", "energy", "device")
        rows = [("VID", "05AC Apple"), ("PID", "1A2B"), ("Role", "Sink  DRP"), ("Asks", "20.00V 1.50A"),
                ("SVID", "05AC Apple"), ("SVID", "8087 TBT")]
        for i, (k, v) in enumerate(rows):
            y = 56 + i * 20
            sc.text(4, y, 16, k, "dim").text(0, y, 16, v, "text", right=216)
        footer(sc, "Identified 0:03 ago", "dim", "ok")
        return sc
    port_strip(sc, 24, "none", "DEV", "dim", "ok" if state == "setup" else "error")
    checklist(sc, 54, [("ok", "No source on IN"), ("ok" if state == "setup" else "bad",
                                                    "Device on OUT" if state == "setup" else "Sink drawing power")])
    if state == "setup":
        sc.text(4, 104, 16, "K2 acts as source and", "dim").text(4, 124, 16, "asks the device who", "dim")
        sc.text(4, 144, 16, "it is and what it needs", "dim")
        footer(sc, "Ready. OK: Run", "ok", "ok")
    else:
        sc.text(4, 104, 16, "A device is already", "text").text(4, 124, 16, "powered through K2.", "text")
        sc.text(4, 144, 16, "Unplug the source, or", "text").text(4, 164, 16, "OK again forces it.", "text")
        footer(sc, "Live sink on port. OK again", "caution", "caution")
    return sc


# ---- Fast-charge trigger ---------------------------------------------------------------------
def fc_trigger(sid, state):
    labels = {"setup": sub("Start", "-V", "+V"), "run": sub("Stop", "-V", "+V"),
              "scan": sub("Rescan", "", ""), "refused": sub("", "-V", "+V")}[state]
    sc = Screen(sid, "FC trigger", labels)
    sub_bar(sc, "FC trigger", live=state != "refused")
    sc.text(4, 22, 24, "QC3.0" if state != "scan" else "Auto-detect", "text", "protocol")
    if state == "scan":
        grid = [("QC2 9V", "ok"), ("QC2 12V", "ok"), ("QC3 var", "ok"), ("AFC 9V", "error"),
                ("FCP 9V", "error"), ("SCP", "error"), ("VOOC", "faint"), ("UFCS", "faint")]
        for i, (t, tok) in enumerate(grid):
            x, y = 4 + (i % 2) * 110, 56 + (i // 2) * 22
            sc.rect(x, y + 5, 6, 6, tok, "result dot").text(x + 12, y, 16, t, "text" if tok == "ok" else "dim")
        sc.text(4, 150, 16, "3 of 8 accepted", "energy", "summary")
        footer(sc, "Scan done. Max QC2 12V", "dim", "ok")
        return sc
    sc.text(4, 52, 12, "TARGET", "dim").text(4, 64, 24, " 9.00V", "volt", "target")
    sc.text(116, 52, 12, "VBUS NOW", "dim").text(116, 64, 24, " 9.02V" if state == "run" else
                                                   " 5.01V" if state == "setup" else " 0.00V",
                                                   "volt" if state != "refused" else "faint", "measured")
    if state == "refused":
        port_strip(sc, 104, "none", "", "error", "dim")
        checklist(sc, 132, [("bad", "Source on IN"), ("ok", "D+/D- free")])
        footer(sc, "No VBUS on IN. Plug charger", "error", "error")
    else:
        sc.text(4, 104, 16, "State", "dim").text(0, 104, 16, "Holding" if state == "run" else "Idle",
                                                 "energy" if state == "run" else "text", right=216)
        sc.text(4, 124, 16, "Current", "dim").text(0, 124, 16, "1.20A →", "amp", right=216)
        sc.text(4, 144, 16, "Line", "dim").text(0, 144, 16, "D+0.60 D-3.30", "dim", right=216)
        sc.text(4, 172, 12, "Long +/-: protocol. Load on OUT ok", "dim", "hint")
        footer(sc, "Holding 9V 0:12" if state == "run" else "OK: request 9V", "energy" if state == "run" else "dim", "ok")
    return sc


# ---- PD trigger ----------------------------------------------------------------------------------
def pd_trigger(sid, state):
    labels = {"list": sub("Send", "Prev", "Next"), "adjust": sub("Send", "-0.1", "+0.1"), "refused": sub("", "", "")}[state]
    sc = Screen(sid, "PD trigger", labels)
    sub_bar(sc, "PD trigger", live=state != "refused")
    if state == "refused":
        port_strip(sc, 24, "none", "", "error", "dim")
        checklist(sc, 54, [("bad", "PD charger on IN"), ("ok", "No live sink on OUT")])
        sc.text(4, 104, 24, "No PD source", "text")
        sc.text(4, 136, 16, "Plug a PD charger into", "dim").text(4, 156, 16, "IN, then reopen.", "dim")
        footer(sc, "No Source_Caps received", "error", "error")
        return sc
    sc.text(4, 22, 12, "#  TYP     V      A    W", "dim", "column head")
    for i, (ty, v, a, w, tok) in enumerate(PDOS):
        y = 36 + i * 20
        cur = i == (5 if state == "adjust" else 2)
        if cur:
            sc.rect(0, y - 2, CW - 4, 20, "surface", "cursor row").rect(0, y - 2, 3, 20, "focus", "focus bar")
        sc.text(6, y, 16, f"{i+1} {ty}{v:>7} {a} {w}", "text" if cur else tok, f"PDO {i+1}")
    if state == "adjust":
        sc.rect(0, 160, CW, 40, "surface", "adjust card")
        sc.text(4, 162, 12, "REQUEST  (long OK: V/A)", "dim")
        sc.text(4, 174, 24, "12.40V", "volt", "req V").text(110, 174, 24, "2.00A", "amp", "req A")
        sc.rect(4, 198, 84, 2, "focus", "edit underline")
        footer(sc, "Editing V. 20mV steps", "volt", "caution")
    else:
        sc.text(4, 164, 16, "Now", "dim").text(0, 164, 16, "PDO4 20.00V", "energy", right=216)
        sc.text(4, 184, 16, "VBUS", "dim").text(0, 184, 16, "20.01V 2.99A →", "volt", right=216)
        footer(sc, "OK: request PDO3 15V", "dim", "ok")
    return sc


# ---- Cable R -------------------------------------------------------------------------------------
def cable_r(sid, state):
    sc = Screen(sid, "Cable R", sub("Reset", "", "Mode"))
    sub_bar(sc, "Cable R", live=state != "error")
    port_strip(sc, 24, "SRC", "LOAD", "ok" if state != "error" else "dim", "ok" if state == "result" else "caution")
    if state == "error":
        sc.text(4, 60, 24, "Meter not found", "error")
        sc.text(4, 96, 16, "Cable R needs the", "text").text(4, 116, 16, "power meter. See Diag.", "text")
        footer(sc, "INA2xx not on I2C", "error", "error")
        return sc
    sc.text(4, 52, 12, "STEP 1  IDLE  <0.20A", "dim")
    sc.text(4, 66, 16, "20.04V 0.05A", "volt" if state == "result" else "text", "idle sample")
    sc.text(4, 90, 12, "STEP 2  LOAD  >=1.0A", "dim")
    sc.text(4, 104, 16, "19.71V 3.01A" if state == "result" else "Add load...", "volt" if state == "result" else "caution", "load sample")
    sc.rect(0, 128, CW, 1, "rule")
    if state == "result":
        sc.text(4, 134, 12, "LOOP RESISTANCE", "dim")
        sc.text(4, 148, 48, "110", "energy", "R digits").text(96, 148 + 13, 24, "mΩ", "energy", "unit")
        sc.text(150, 162, 16, "GOOD", "ok", "grade")
        sc.text(4, 194, 12, "Drop 0.33V at 3A = 1.0W lost", "dim")
        footer(sc, "Long +: Kelvin mode", "dim", "ok")
    else:
        sc.text(4, 138, 16, "Draw at least 1A on", "text").text(4, 158, 16, "OUT (e-load / phone).", "text")
        footer(sc, "Idle captured. Need load", "caution", "caution")
    return sc


# ---- Reboot hold overlay -----------------------------------------------------------------------
def reboot_hold(sid):
    sc = home(sid, "20.01", "2.990", "59.83", "→", "live")
    sc.rect(12, 80, 200, 80, "surface", "hold card").rect(12, 80, 200, 2, "error", "card accent")
    sc.text(24, 90, 24, "Reboot in 3", "text", "countdown")
    sc.rect(24, 124, 176, 8, "rule", "hold track").rect(24, 124, 70, 8, "error", "hold fill")
    sc.text(24, 140, 12, "Release Back to cancel", "dim")
    return sc


SCREENS = [
    home("home_live", "20.01", "2.990", "59.83", "→", "live"),
    home("home_reverse", " 5.12", "0.840", " 4.30", "←", "live"),
    home("home_idle", " 0.00", "0.000", " 0.00", None, "idle"),
    home("home_stale", "20.01", "2.990", "59.83", None, "stale"),
    home("home_error", "--.--", "-.---", "--.--", None, "error"),
    reboot_hold("home_reboot_hold"),
    pd_page("pd_live", "live"), pd_page("pd_wait", "wait"), pd_page("pd_stale", "stale"),
    kv_page("diag", "Diag", "3/5", [("USB", "configured", "energy"), ("RST/ISR", "1 / 48211", "text"),
                                   ("TX/DROP", "1520 / 0", "text"), ("Poll gap", "12 ms", "amp"),
                                   ("Meter", "INA228 0x40", "text"), ("FUSB", "ID 0x91", "dim"),
                                   ("PD RX", "12 CC1", "text"), ("Model", "K2 fw N6.55", "dim"),
                                   ("Uptime", "01:23:45", "dim")], ("All subsystems up", "dim", "ok")),
    lines_page("lines"),
    tools_page("tools_ready", 0), tools_page("tools_blocked", 3), tools_page("tools_caution", 1),
    emarker("emarker_setup", "setup"), emarker("emarker_sniff", "sniff"), emarker("emarker_scan", "scan"),
    emarker("emarker_result", "result"), emarker("emarker_none", "none"),
    identify("identify_setup", "setup"), identify("identify_refused", "refused"), identify("identify_result", "result"),
    fc_trigger("fc_setup", "setup"), fc_trigger("fc_run", "run"), fc_trigger("fc_scan", "scan"), fc_trigger("fc_refused", "refused"),
    pd_trigger("pdt_list", "list"), pd_trigger("pdt_adjust", "adjust"), pd_trigger("pdt_refused", "refused"),
    cable_r("cabler_setup", "setup"), cable_r("cabler_result", "result"), cable_r("cabler_error", "error"),
]


# ---- rendering -----------------------------------------------------------------------------------
def draw_panel(img, d, labels):
    d.rectangle([PANEL_X, 0, 239, 239], fill=rgb("panel"))
    for i, (y0, ln) in enumerate(SLOTS):
        if i: d.rectangle([PANEL_X, y0, 239, y0 + 1], fill=rgb("line"))
        t = labels[i]
        if not t: continue
        tw = len(t) * ADV[12]
        tmp = Image.new("RGB", (tw + 2, 15), rgb("panel"))
        ImageDraw.Draw(tmp).text((1, 13), t, font=FONTS[12], fill=rgb("text"), anchor="ls")
        rot = tmp.rotate(90, expand=True)   # CCW: reads bottom-to-top
        img.paste(rot, (PANEL_X + 1, y0 + (ln - rot.height) // 2))


def render(sc):
    img = Image.new("RGB", (240, 240), rgb("bg"))
    d = ImageDraw.Draw(img)
    for op in sc.ops:
        if op[0] == "rect":
            _, x, y, w, h, tok, _ = op
            if w <= 0 or h <= 0: continue
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=rgb(tok))
        else:
            _, x, y, size, s, tok, _ = op
            d.text((x, y + ASC[size]), s, font=FONTS[size], fill=rgb(tok), anchor="ls")
    draw_panel(img, d, sc.labels)
    img.resize((480, 480), Image.NEAREST).save(OUT / f"{sc.sid}.png")


def ascii_sketch(sc):
    """28 x 20 grid, 1 cell = 8 x 12 px; content x 0..223, panel shown as '|'."""
    W, H = 28, 20
    g = [[" "] * W + ["|"] for _ in range(H)]
    for op in sc.ops:
        if op[0] == "rect":
            _, x, y, w, h, tok, name = op
            if h <= 2 and w > 40:   # rules
                r = min(H - 1, y // 12)
                for c in range(x // 8, min(W, (x + w) // 8)): g[r][c] = "-"
        else:
            _, x, y, size, s, tok, name = op
            step = max(1, round(ADV[size] / 8)); r = min(H - 1, (y + LH[size] // 2) // 12)
            c = x // 8
            for ch in s:
                if 0 <= c < W: g[r][c] = ch if ch.isascii() else {"→": ">", "←": "<", "Ω": "O", "°": "o"}.get(ch, "?")
                for k in range(1, step):
                    if 0 <= c + k < W: g[r][c + k] = " "
                c += step
    lines = ["    " + "".join(str(i % 10) for i in range(W)) + "|"]
    lines += [f"{r*12:3d} " + "".join(row) for r, row in enumerate(g)]
    return "\n".join(lines)


def table(sc):
    rows = ["| element | x | y | w | h | font | token |", "|---|---|---|---|---|---|---|"]
    for op in sc.ops:
        if op[0] == "rect":
            _, x, y, w, h, tok, name = op
            if name: rows.append(f"| {name} | {x} | {y} | {w} | {h} | - | `{tok}` |")
        else:
            _, x, y, size, s, tok, name = op
            label = name or f"\"{s}\""
            rows.append(f"| {label} | {x} | {y} | {len(s)*ADV[size]} | {LH[size]} | {size} | `{tok}` |")
    return "\n".join(rows)


def main():
    md = ["# K2 UI mockups: generated screens", "",
          "Generated by `render_mockups.py`; do not edit by hand. Grid cell = 8 x 12 px (column header =",
          "x/8, row label = y). x/y/w/h are pixels in the 224 x 240 content area; the panel is x 224..239.", ""]
    for sc in SCREENS:
        render(sc)
        md += [f"## {sc.sid}", "", f"![{sc.sid}]({sc.sid}.png)", "", "Panel: " +
               " / ".join(l or "(empty)" for l in sc.labels), "", "```", ascii_sketch(sc), "```", "",
               "<details><summary>Element table</summary>", "", table(sc), "", "</details>", ""]
    (OUT / "screens.md").write_text("\n".join(md))
    print(f"{len(SCREENS)} screens -> {OUT}")


if __name__ == "__main__":
    main()
