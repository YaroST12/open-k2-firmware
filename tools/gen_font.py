#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Convert the UI font into glyph bitmaps for the K2 display.

Host-only tool; the generated src/ui/fonts/*.cpp and include/k2/ui/font_aa.hpp are committed because the
firmware build does not run it.

    python3 tools/gen_font.py                    # Atkinson Hyperlegible Mono, 4 bpp (the shipped font)
    python3 tools/gen_font.py --source spleen    # fallback: Spleen bitmap fonts, 1 bpp
    python3 tools/gen_font.py --check            # fail if the committed files differ from a fresh run

The default source rasterises the outline font with FreeType (freetype-py) after pinning its weight
axis with fontTools; both are third-party modules, so run it from a venv:
`python -I` with freetype-py + fonttools installed. Without them `--check` only verifies the input
stamp written into every generated file (font file hashes + generator parameters); the Spleen source
needs neither module.

Glyph format (see include/k2/ui/font_aa.hpp): each glyph is cropped to its ink box and stored as rows of
pixels, either 1 bit (Spleen: 8 per byte, leftmost pixel in bit 7, stride (w + 7) / 8 bytes) or 4 bits
(Atkinson: coverage 0..15, two per byte with the left pixel in the high nibble, stride (w + 1) / 2
bytes). `left` is the pixel offset of the box from the pen position, `top` its height above the common
baseline. Every glyph of a font has the same advance (monospace) and its ink lies inside its own cell,
so neighbouring glyphs never overlap. Glyphs a Spleen BDF lacks are hand-drawn in
tools/font_overrides.py; glyphs Atkinson lacks (the four arrows) are rasterised from JetBrains Mono.
"""
import argparse
import hashlib
import io
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONT_DIR = ROOT / "third_party/fonts"
OUT_SRC = ROOT / "src/ui/fonts"
OUT_HDR = ROOT / "include/k2/ui/font_aa.hpp"
ATKINSON_TTF = FONT_DIR / "atkinson-hyperlegible-mono/AtkinsonHyperlegibleMono[wght].ttf"
BORROW_TTF = FONT_DIR / "jetbrains-mono/JetBrainsMono-Regular.ttf"

ASCII = [chr(c) for c in range(32, 127)]
# degree, middle dot, Greek capital omega (ohm), arrows left/up/right/down
EXTRA = ["°", "·", "×", "Ω", "←", "↑", "→", "↓"]

# Glyph sets: 12 px (panel labels, pills, footers), 16 px and 24 px (body, headings).
SET_12 = ASCII + ["·", "°", "×", "Ω", "↑", "→", "↓"]   # → for the ramp labels ("PPS 11→8V")
SET_16 = ASCII + EXTRA
# Big meter digits only: units are drawn in the 24 px font, so the set stays at digits, '.', '-', ' '.
SET_48 = list("0123456789.- ")
# Home hero digits (VBUS / IBUS, 5 characters = 160 px): the same set at 64 px.
SET_64 = SET_48

# name, nominal size, characters, source (a key of BDF_FILES).
FONTS = [
    ("font_12", 12, SET_12, "spleen-12"),
    ("font_16", 16, SET_16, "spleen-16"),
    ("font_24", 24, SET_16, "spleen-24"),
    ("font_48", 48, SET_48, "spleen-48"),
]

# source name -> (BDF under third_party/fonts, banner label, scale (num, den) or None, ink metrics).
# Ink metrics = ascent/descent from the ink of the generated glyphs (the 48 px digits) instead of the
# BDF's cell metrics. The 48 px digits are Spleen 12x24 scaled x2 (integer, nearest neighbour): its
# 2 px stems become an even 4 px, 24 px advance, ink height set by the scaled digits.
BDF_FILES = {
    "spleen-12": ("spleen/spleen-6x12.bdf", "Spleen 2.2.0 6x12 (BSD-2-Clause)", None, False),
    "spleen-16": ("spleen/spleen-8x16.bdf", "Spleen 2.2.0 8x16 (BSD-2-Clause)", None, False),
    "spleen-24": ("spleen/spleen-12x24.bdf", "Spleen 2.2.0 12x24 (BSD-2-Clause)", None, False),
    "spleen-48": ("spleen/spleen-12x24.bdf", "Spleen 2.2.0 12x24 scaled x2 (BSD-2-Clause)", (2, 1), True),
}


def parse_bdf(text):
    """Parse a BDF (data only, nothing is executed). Returns (ascent, descent, {cp: glyph}) where a
    glyph is (w, h, left, top, adv, rows) in the render_glyph convention: rows of 0/1, `top` the
    ink box height above the baseline."""
    ascent = descent = None
    glyphs = {}
    cur = None
    bitmap = None
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        key = parts[0]
        if bitmap is not None:
            if key == "ENDCHAR":
                w, h = cur["bbx"][:2]
                assert len(bitmap) == h, f"bad BITMAP height for U+{cur['enc']:04X}"
                rows = []
                for hexrow in bitmap:
                    bits = bin(int(hexrow, 16))[2:].zfill(len(hexrow) * 4)
                    rows.append([int(b) for b in bits[:w]])
                bw, bh, bx, by = cur["bbx"]
                if cur["enc"] >= 0:
                    glyphs[cur["enc"]] = (bw, bh, bx, by + bh, cur["adv"], rows)
                bitmap = cur = None
            else:
                bitmap.append(key)
            continue
        if key == "FONT_ASCENT":
            ascent = int(parts[1])
        elif key == "FONT_DESCENT":
            descent = int(parts[1])
        elif key == "STARTCHAR":
            cur = dict(enc=-1, adv=0, bbx=(0, 0, 0, 0))
        elif cur is not None and key == "ENCODING":
            cur["enc"] = int(parts[1])
        elif cur is not None and key == "DWIDTH":
            cur["adv"] = int(parts[1])
        elif cur is not None and key == "BBX":
            cur["bbx"] = tuple(int(v) for v in parts[1:5])
        elif cur is not None and key == "BITMAP":
            bitmap = []
    assert ascent is not None and descent is not None, "BDF without FONT_ASCENT/FONT_DESCENT"
    return ascent, descent, glyphs


def art_glyph(art, ascent, adv):
    """A hand-drawn fill-in: `art` = one string per cell row from the top of the line box ('#' = ink)."""
    rows = [[1 if c == "#" else 0 for c in line.ljust(adv)[:adv]] for line in art]
    return (adv, len(rows), 0, ascent, adv, rows)


def clip_rows(w, h, left, top, rows, ascent, descent):
    """Crop a glyph box to the line box (ascent above, descent below the baseline) and its ink."""
    cut_top = max(0, top - ascent)
    cut_bot = max(0, (h - top) - descent)
    rows = rows[cut_top:h - cut_bot]
    top -= cut_top
    while rows and not any(rows[0]):
        rows, top = rows[1:], top - 1
    while rows and not any(rows[-1]):
        rows = rows[:-1]
    if not rows:
        return 0, 0, 0, 0, []
    lo = min(r.index(1) for r in rows if 1 in r)
    hi = max(len(r) - r[::-1].index(1) for r in rows if 1 in r)
    rows = [r[lo:hi] for r in rows]
    return hi - lo, len(rows), left + lo, top, rows


def pack(rows, w):
    out = bytearray()
    for row in rows:
        for x in range(0, w, 8):
            b = 0
            for i in range(8):
                if x + i < w and row[x + i]:
                    b |= 0x80 >> i
            out.append(b)
    return bytes(out)


def scale_font(ascent, descent, glyphs, num, den):
    """Scale a font by num/den, nearest neighbour, in line-box coordinates (so every glyph is sampled
    on the same grid). Returns (ascent, descent, glyphs) in the same glyph convention."""
    adv = next(iter(glyphs.values()))[4]
    nadv, nasc, ndesc = adv * num // den, ascent * num // den, descent * num // den
    out = {}
    for ch, (w, h, left, top, _, rows) in glyphs.items():
        cell = [[0] * adv for _ in range(ascent + descent)]
        for y, row in enumerate(rows):
            for x, v in enumerate(row):
                if v and 0 <= left + x < adv and 0 <= ascent - top + y < ascent + descent:
                    cell[ascent - top + y][left + x] = 1
        big = [[cell[y * den // num][x * den // num] for x in range(nadv)] for y in range(nasc + ndesc)]
        out[ch] = (nadv, nasc + ndesc, 0, nasc, nadv, big)
    return nasc, ndesc, out


def load_bdf_source(source, chars):
    """BDF glyphs for `chars` plus the cell metrics; glyphs the BDF lacks come from font_overrides.
    Returns (ascent, descent, {char: glyph}, [chars filled in by hand])."""
    import font_overrides
    path, _, scale, _ = BDF_FILES[source]
    ascent, descent, bdf = parse_bdf((FONT_DIR / path).read_text(encoding="utf-8"))
    out, drawn = {}, []
    adv = next(iter(bdf.values()))[4]
    for ch in chars:
        if ord(ch) in bdf:
            out[ch] = bdf[ord(ch)]
        else:
            art = font_overrides.OVERRIDES.get(source, {}).get(ord(ch))
            assert art, f"{source}: U+{ord(ch):04X} is missing and has no override"
            out[ch] = art_glyph(art, ascent, adv)
            drawn.append(ch)
    if scale:
        ascent, descent, out = scale_font(ascent, descent, out, *scale)
    return ascent, descent, out, drawn


def build_font(name, size, chars, source):
    chars = sorted(set(chars), key=ord)
    ascent, descent, rendered, _ = load_bdf_source(source, chars)
    if BDF_FILES[source][3]:
        ink = [clip_rows(*rendered[ch][:4], rendered[ch][5], ascent, descent) for ch in chars]   # (w, h, left, top, rows)
        ascent = max(g[3] for g in ink)
        descent = max(0, max(g[1] - g[3] for g in ink))
    bitmap = bytearray()
    glyphs = []
    for ch in chars:
        w, h, left, top, adv, rows = rendered[ch]
        w, h, left, top, rows = clip_rows(w, h, left, top, rows, ascent, descent)
        data = pack(rows, w)
        glyphs.append(dict(cp=ord(ch), off=len(bitmap), w=w, h=h, left=left, top=top, adv=adv))
        bitmap += data
    advs = {g["adv"] for g in glyphs}
    assert len(advs) == 1, f"{name}: not monospace {advs}"
    assert len(bitmap) < 65536, f"{name}: bitmap too large for 16-bit offsets"
    for g in glyphs:
        assert -128 <= g["left"] < 128 and -128 <= g["top"] < 128
    return dict(name=name, size=size, bitmap=bytes(bitmap), glyphs=glyphs, bpp=1,
                adv=advs.pop(), ascent=ascent, descent=descent)


# ---- Atkinson Hyperlegible Mono (outline font, 4 bpp) ------------------------------------------------

# Per role: (digit advance in px that the UI layout assumes, maximum line height). The line height is
# ascent + descent over the glyph set; the 48 px role is digits only, so its cap is the digit height.
ROLES = {12: (6, 12), 16: (8, 16), 24: (12, 24), 48: (24, 32), 64: (32, 40)}
ROLE_CHARS = {12: SET_12, 16: SET_16, 24: SET_16, 48: SET_48, 64: SET_64}
DIGIT_ROLES = (48, 64)   # digits-only roles: the height cap is the digit height above the baseline
# Weight axis per role: small text is a touch heavier so thin stems keep their contrast on the panel.
ROLE_WGHT = {12: 500, 16: 450, 24: 400, 48: 400, 64: 450}
# Line box per role: (rows above, rows below the baseline) the UI layout was drawn around (the Spleen
# metrics). The font reports at least this, so text keeps its baseline and the layout does not move;
# a glyph set whose ink is taller still grows the box.
LINE_BOX = {12: (9, 3), 16: (12, 4), 24: (19, 5), 48: (30, 0), 64: (38, 0)}
MAX_OVERSHOOT = 0.2   # px by which the exact digit advance may exceed the target before rounding
GAMMA = 0.8            # coverage curve applied to the rasteriser's 0..255 (<1 thickens light-on-dark text)
WHITE_POINT = 0.9      # rasteriser coverage that already counts as full ink: a hinted 1 px stem lands at
                       # 0.85..0.95 and must still reach the pure foreground colour, not a shade of it
HINT = "light"        # FreeType light auto-hinter: snaps stems and the baseline to whole pixels vertically
BORROWED = "←↑→↓"      # glyphs Atkinson Mono lacks, rasterised from BORROW_TTF at the same cell width
PARAMS = repr((ROLES, {r: "".join(c) for r, c in ROLE_CHARS.items()}, ROLE_WGHT, MAX_OVERSHOOT, GAMMA, HINT,
               BORROWED, LINE_BOX, WHITE_POINT))


class Outline:
    """One outline font (optionally pinned at one weight) as a FreeType face. In memory only."""

    def __init__(self, path, wght=None):
        import freetype
        data = Path(path).read_bytes()
        if wght is not None:
            from fontTools.ttLib import TTFont
            from fontTools.varLib import instancer
            inst = instancer.instantiateVariableFont(TTFont(io.BytesIO(data)), {"wght": wght}, inplace=False)
            buf = io.BytesIO()
            inst.save(buf)
            data = buf.getvalue()
        self.ft = freetype
        self.face = freetype.Face(io.BytesIO(data))
        self.upem = self.face.units_per_EM

    def has(self, ch):
        return self.face.get_char_index(ord(ch)) != 0

    def set_size(self, ppem):
        self.face.set_char_size(int(round(ppem * 64)))

    def load(self, ch, hint=HINT):
        """(rows of 0..255 coverage, bitmap_left, bitmap_top, exact advance in px) at the current size."""
        ft = self.ft
        flags = {"light": ft.FT_LOAD_TARGET_LIGHT, "none": ft.FT_LOAD_NO_HINTING}[hint]
        self.face.load_glyph(self.face.get_char_index(ord(ch)), flags | ft.FT_LOAD_RENDER)
        g = self.face.glyph
        bm = g.bitmap
        rows = [list(bm.buffer[r * bm.pitch:r * bm.pitch + bm.width]) for r in range(bm.rows)]
        return rows, g.bitmap_left, g.bitmap_top, g.linearHoriAdvance / 65536.0

    def digit_adv(self, ppem):
        self.set_size(ppem)
        return self.load("0", "none")[3]


def autofit(font, role):
    """Largest ppem (0.1 px steps) whose digit advance fits the role's cell and whose ink fits its height."""
    adv_t, h_t = ROLES[role]
    for tenth in range(800, 40, -1):
        ppem = tenth / 10
        if font.digit_adv(ppem) > adv_t + MAX_OVERSHOOT:
            continue
        top = bot = 0
        font.set_size(ppem)
        for ch in ROLE_CHARS[role]:
            if font.has(ch):
                rows, _, t, _ = font.load(ch)
                top, bot = max(top, t), max(bot, len(rows) - t)
        if (top if role in DIGIT_ROLES else top + bot) > h_t:
            continue
        return ppem
    raise AssertionError(f"no size fits role {role}")


def to_levels(rows):
    return [[min(15, int(round((min(1.0, v / 255.0 / WHITE_POINT) ** GAMMA) * 15))) for v in r] for r in rows]


def crop_levels(levels, left, top):
    """Trim blank border rows/columns of a level matrix; returns (w, h, left, top, rows)."""
    rows = list(levels)
    while rows and not any(rows[0]):
        rows, top = rows[1:], top - 1
    while rows and not any(rows[-1]):
        rows = rows[:-1]
    if not rows:
        return 0, 0, 0, 0, []
    lo = min(next(i for i, v in enumerate(r) if v) for r in rows if any(r))
    hi = max(len(r) - next(i for i, v in enumerate(reversed(r)) if v) for r in rows if any(r))
    return hi - lo, len(rows), left + lo, top, [r[lo:hi] for r in rows]


def fit_cell(w, left, rows, adv):
    """Move a glyph's ink box inside its cell [0, adv) (the renderer lets no ink cross a cell edge):
    shift it whole pixels when it fits, else clip the overhanging columns. Returns
    (w, left, rows, clipped) with `clipped` = the highest coverage level that was cut off (0 = none)."""
    if w == 0:
        return w, left, rows, 0
    if w <= adv:
        return w, max(0, min(left, adv - w)), rows, 0
    cut_l, cut_r = max(0, -left), max(0, left + w - adv)
    lost = [r[:cut_l] + r[w - cut_r:] for r in rows]
    keep = [r[cut_l:w - cut_r] for r in rows]
    return w - cut_l - cut_r, left + cut_l, keep, max((max(r) for r in lost if r), default=0)


def pack4(rows, w):
    """4 bpp rows: two pixels per byte, left pixel in the high nibble, odd widths padded with level 0."""
    out = bytearray()
    for r in rows:
        for x in range(0, w, 2):
            out.append((r[x] << 4) | (r[x + 1] if x + 1 < w else 0))
    return bytes(out)


def build_outline_font(role, main, borrow):
    """One role of the Atkinson set -> font dict like build_font. `main` is the face pinned at the role's
    weight, `borrow` the face the BORROWED glyphs come from."""
    chars = sorted(set(ROLE_CHARS[role]), key=ord)
    ppem = autofit(main, role)
    main.set_size(ppem)
    exact = main.digit_adv(ppem)
    adv = max(1, int(round(exact)))
    assert adv == ROLES[role][0], f"role {role}: digit advance {adv}, the layout expects {ROLES[role][0]}"
    borrow_ppem = ppem * exact / (borrow.digit_adv(ppem) or 1)
    ink, borrowed, clipped = [], [], []
    for ch in chars:
        if ch == " ":
            ink.append((ch, 0, 0, 0, 0, []))
            continue
        src, size = (main, ppem) if main.has(ch) else (borrow, borrow_ppem)
        if src is borrow:
            assert ch in BORROWED, f"U+{ord(ch):04X} is missing from Atkinson Mono and not a listed borrow"
            borrowed.append(ch)
        src.set_size(size)
        rows, left, top, glyph_adv = src.load(ch)
        left += int(round((adv - glyph_adv) / 2.0))        # centre the glyph in the digit cell
        w, h, left, top, crows = crop_levels(to_levels(rows), left, top)
        w, left, crows, lost = fit_cell(w, left, crows, adv)
        if lost:
            clipped.append((ch, lost))
        ink.append((ch, w, h, left, top, crows))
    ascent = max(LINE_BOX[role][0], max(g[4] for g in ink))
    descent = max(LINE_BOX[role][1], max(g[2] - g[4] for g in ink))
    bitmap, glyphs = bytearray(), []
    for ch, w, h, left, top, rows in ink:
        glyphs.append(dict(cp=ord(ch), off=len(bitmap), w=w, h=h, left=left, top=top, adv=adv))
        bitmap += pack4(rows, w)
    assert len(bitmap) < 65536, f"role {role}: bitmap too large for 16-bit offsets"
    assert ascent + descent <= ROLES[role][1] or role in DIGIT_ROLES, f"role {role}: line box {ascent + descent} too tall"
    for g in glyphs:
        assert -128 <= g["left"] < 128 and -128 <= g["top"] < 128
    return dict(name=f"font_{role}", size=role, bitmap=bytes(bitmap), glyphs=glyphs, adv=adv, ascent=ascent,
                descent=descent, bpp=4, ppem=ppem, wght=ROLE_WGHT[role], borrowed=borrowed, clipped=clipped)


def input_stamp():
    """Fingerprint of everything the Atkinson tables are derived from, without rasterising anything."""
    h = hashlib.sha256()
    for p in (ATKINSON_TTF, BORROW_TTF):
        h.update(p.read_bytes())
    h.update(PARAMS.encode())
    return h.hexdigest()[:16]


def c_source(f, banner):
    n = len(f["glyphs"])
    lines = [banner, '#include "k2/ui/font_aa.hpp"', "", "namespace k2 {", "", "namespace {", ""]
    lines.append(f"const uint8_t bitmap[{len(f['bitmap'])}] = {{")
    for i in range(0, len(f["bitmap"]), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in f["bitmap"][i:i + 16]) + ",")
    lines.append("};\n")
    lines.append(f"const uint16_t codepoints[{n}] = {{")
    cps = [f"0x{g['cp']:04X}" for g in f["glyphs"]]
    for i in range(0, n, 12):
        lines.append("    " + ", ".join(cps[i:i + 12]) + ",")
    lines.append("};\n")
    lines.append(f"const Glyph glyphs[{n}] = {{")
    lines.append("    /* offset, w, h, left, top, adv */")
    for g in f["glyphs"]:
        ch = chr(g["cp"])
        label = ch if 32 < g["cp"] < 127 and ch not in "*/\\" else f"U+{g['cp']:04X}"
        lines.append(f"    {{ {g['off']}, {g['w']}, {g['h']}, {g['left']}, {g['top']}, {g['adv']} }}, /* {label} */")
    lines.append("};\n")
    lines.append("}  // namespace\n")
    lines.append(f"const Font {f['name']} = {{")
    lines.append("    .bitmap = bitmap, .codepoints = codepoints, .glyphs = glyphs,")
    lines.append(f"    .count = {n}, .adv = {f['adv']}, .ascent = {f['ascent']}, .descent = {f['descent']},")
    lines.append(f"    .height = {f['ascent'] + f['descent']}, .px = {f['size']}, .bpp = {f['bpp']},")
    lines.append("};\n")
    lines.append("}  // namespace k2")
    return "\n".join(lines) + "\n"


HEADER = """\
// GENERATED by tools/gen_font.py - do not edit.
//
@SOURCE@
#pragma once
#include <cstdint>

namespace k2 {

// One glyph, cropped to its ink box and lying inside its own advance cell. Pixel rows start at
// font.bitmap + offset and are packed by Font::bpp:
//   bpp 1: set = ink, eight pixels per byte (leftmost in bit 7), row stride (w + 7) / 8 bytes;
//   bpp 4: coverage 0..15 (the blend LUT index, 0 = background .. 15 = foreground), two pixels per
//          byte (left pixel in the high nibble), row stride (w + 1) / 2 bytes.
// `left` = box x offset from the pen, `top` = box height above the baseline (the box covers rows
// top-1 .. top-h of the baseline).
struct Glyph {
    uint16_t offset;
    uint8_t  w, h;
    int8_t   left, top;
    uint8_t  adv;
};

struct Font {
    const uint8_t  *bitmap;
    const uint16_t *codepoints;  // sorted ascending, `count` entries
    const Glyph    *glyphs;      // parallel to codepoints
    uint16_t count;
    uint8_t  adv;      // advance of every glyph (monospace)
    uint8_t  ascent;   // tallest glyph above the baseline
    uint8_t  descent;  // deepest glyph below the baseline
    uint8_t  height;   // ascent + descent: pixel height of one text line
    uint8_t  px;       // nominal em size in pixels
    uint8_t  bpp = 1;  // bits per pixel of the glyph rows: 1 or 4
};

@DECLS@
}  // namespace k2
"""

SOURCE_NOTE = {
    "atkinson": "// Monospace 4 bits per pixel fonts rasterised from Atkinson Hyperlegible Mono 2.001 (SIL OFL 1.1)\n"
                "// at 12, 16, 24, 48 and 64 px cells; the four arrows come from JetBrains Mono 2.304 (SIL OFL 1.1).\n"
                "// Sources, hashes and licences: third_party/fonts/.",
    "spleen": "// Monospace 1 bit per pixel bitmap fonts converted from Spleen 2.2.0 (BSD-2-Clause) BDF sources\n"
              "// (6x12, 8x16, 12x24, and 12x24 scaled x2 for the 48 px digits). Sources and licence:\n"
              "// third_party/fonts/spleen/.",
}


def decl(f):
    return (f"/* {f['size']} px, {len(f['glyphs'])} glyphs, advance {f['adv']}, ascent {f['ascent']}, "
            f"descent {f['descent']} */\nextern const Font {f['name']};\n")


def flash_note(f):
    table = len(f["glyphs"]) * (8 + 2)
    return (f"{f['name']}: {len(f['glyphs'])} glyphs, {f['bpp']} bpp, bitmap {len(f['bitmap'])} B, "
            f"tables {table} B, total ~{len(f['bitmap']) + table + 20} B flash")


def spleen_outputs():
    outs, decls, report = {}, [], []
    for name, size, chars, source in FONTS:
        f = build_font(name, size, chars, source)
        drawn = load_bdf_source(source, sorted(set(chars), key=ord))[3]
        note = f" Hand-drawn fill-ins (tools/font_overrides.py): {' '.join(drawn)}." if drawn else ""
        outs[OUT_SRC / f"{name}.cpp"] = c_source(
            f, f"// GENERATED by tools/gen_font.py - do not edit.\n// {BDF_FILES[source][1]}, converted from the BDF.{note}")
        decls.append(decl(f))
        report.append(flash_note(f))
    outs[OUT_HDR] = HEADER.replace("@SOURCE@", SOURCE_NOTE["spleen"]).replace("@DECLS@", "\n".join(decls))
    return outs, report


def atkinson_outputs():
    outs, decls, report = {}, [], []
    borrow = Outline(BORROW_TTF)
    for role in (12, 16, 24, 48, 64):
        f = build_outline_font(role, Outline(ATKINSON_TTF, ROLE_WGHT[role]), borrow)
        lent = f" Borrowed from JetBrains Mono: {' '.join(f['borrowed'])}." if f["borrowed"] else ""
        outs[OUT_SRC / f"{f['name']}.cpp"] = c_source(
            f, "// GENERATED by tools/gen_font.py - do not edit.\n"
               f"// Atkinson Hyperlegible Mono 2.001 (SIL OFL 1.1), wght {f['wght']}, {f['ppem']} ppem, 4 bpp.{lent}\n"
               f"// Input stamp {input_stamp()}")
        decls.append(decl(f))
        report.append(flash_note(f) + f", {f['ppem']} ppem wght {f['wght']}"
                      + (f", borrowed '{''.join(f['borrowed'])}'" if f["borrowed"] else "")
                      + (f", clipped {f['clipped']}" if f["clipped"] else ""))
    outs[OUT_HDR] = HEADER.replace("@SOURCE@", SOURCE_NOTE["atkinson"]).replace("@DECLS@", "\n".join(decls))
    return outs, report


def make_outputs(source="atkinson"):
    return atkinson_outputs() if source == "atkinson" else spleen_outputs()


def have_rasteriser():
    try:
        import freetype  # noqa: F401
        import fontTools  # noqa: F401
        return True
    except ImportError:
        return False


def stamp_current():
    """Without a rasteriser: the committed Atkinson files carry the stamp of today's inputs."""
    want = f"// Input stamp {input_stamp()}"
    return [str(p) for p in sorted(OUT_SRC.glob("font_*.cpp")) if want not in p.read_text().splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--source", choices=["atkinson", "spleen"], default="atkinson")
    args = ap.parse_args()
    if args.check and args.source == "atkinson" and not have_rasteriser():
        bad = stamp_current()
        if bad:
            print("out of date (input stamp; freetype-py/fonttools not installed, nothing rasterised):",
                  *bad, sep="\n  ")
            return 1
        print("fonts up to date (input stamp only: freetype-py/fonttools not installed, nothing rasterised)")
        return 0
    outs, report = make_outputs(args.source)
    if args.check:
        bad = [str(p) for p, t in outs.items() if not p.exists() or p.read_text() != t]
        if bad:
            print("out of date:", *bad, sep="\n  ")
            return 1
        print("fonts up to date")
        return 0
    OUT_SRC.mkdir(parents=True, exist_ok=True)
    for p, t in outs.items():
        p.write_text(t)
        print("wrote", p.relative_to(ROOT))
    print(*report, sep="\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
