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

Glyph format (see include/k2/ui/font_aa.hpp): each glyph is cropped to its ink box; its pixels are
coverage levels 0..15 (Spleen: 0 and 15 only), run-length coded as nibbles in groups of 8 rows so a
renderer decodes any row of any glyph in one pass with no buffer. The coding is chosen here (per-font
zero-run limit `zmax`) and decoded again for a round-trip check on every run. `left` is the pixel offset
of the box from the pen position, `top` its height above the common baseline. Every glyph of a font has
the same advance (monospace) and its ink lies inside its own cell, so neighbouring glyphs never overlap.
Glyphs a Spleen BDF lacks are hand-drawn in tools/font_overrides.py; glyphs Atkinson lacks (the four
arrows) are rasterised from JetBrains Mono.

tools/font_levels.txt holds the uncompressed levels of the Atkinson fonts (with the input stamp of the
rasteriser run that made them). It lets the tables be regenerated, and `--check` verified bit for bit,
without freetype-py/fonttools, and it is the independent reference tests/test_font_decode.cpp decodes
the shipped tables against.
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
    glyphs = []
    for ch in chars:
        w, h, left, top, adv, rows = rendered[ch]
        w, h, left, top, rows = clip_rows(w, h, left, top, rows, ascent, descent)
        # 1 bit ink becomes coverage 15 (full foreground), so both sources share one coding.
        glyphs.append(dict(cp=ord(ch), w=w, h=h, left=left, top=top, adv=adv,
                           rows=[[15 if v else 0 for v in r] for r in rows]))
    advs = {g["adv"] for g in glyphs}
    assert len(advs) == 1, f"{name}: not monospace {advs}"
    for g in glyphs:
        assert -128 <= g["left"] < 128 and -128 <= g["top"] < 128
    return encode_font(dict(name=name, size=size, glyphs=glyphs, adv=advs.pop(), ascent=ascent, descent=descent))


# ---- Glyph coding (the format is documented in HEADER / include/k2/ui/font_aa.hpp) ---------------------

GROUP_ROWS = 8   # rows per independently addressable group of one glyph
FULL_MIN = 3     # shortest run of level 15 that gets a run token (shorter ones stay literals)


def encode_stream(px, zmax):
    """Nibbles for a flat list of levels 0..15: a literal 1..15 is one pixel; `0 n` with n < zmax is n + 1
    zero pixels, with n >= zmax it is n - zmax + FULL_MIN pixels of level 15."""
    out, i = [], 0
    full_max = 15 - zmax + FULL_MIN
    while i < len(px):
        v, j = px[i], i
        while j < len(px) and px[j] == v:
            j += 1
        run, i = j - i, j
        if v == 0:
            while run:
                k = min(run, zmax)
                out += [0, k - 1]
                run -= k
        elif v == 15:
            while run:
                if run < FULL_MIN:
                    out += [15] * run
                    break
                k = min(run, full_max)
                out += [0, zmax + k - FULL_MIN]
                run -= k
        else:
            out += [v] * run
    return out


def decode_stream(nibs, zmax, count):
    """Inverse of encode_stream: `count` pixels from the nibbles (extra padding nibbles are ignored)."""
    out, i = [], 0
    while len(out) < count:
        t = nibs[i]
        i += 1
        if t:
            out.append(t)
            continue
        n = nibs[i]
        i += 1
        out += [0] * (n + 1) if n < zmax else [15] * (n - zmax + FULL_MIN)
    assert len(out) == count, "a run crosses its group"
    return out


def encode_glyph(rows, w, zmax):
    """(groups - 1) length bytes, then the groups: each GROUP_ROWS rows as one byte-aligned nibble stream."""
    if w == 0 or not rows:
        return b""
    groups = []
    for r0 in range(0, len(rows), GROUP_ROWS):
        nibs = encode_stream([v for r in rows[r0:r0 + GROUP_ROWS] for v in r], zmax)
        nibs += [0] * (len(nibs) & 1)
        groups.append(bytes((nibs[i] << 4) | nibs[i + 1] for i in range(0, len(nibs), 2)))
    assert all(len(g) < 256 for g in groups[:-1]), "a group is longer than its length byte"
    return bytes(len(g) for g in groups[:-1]) + b"".join(groups)


def decode_glyph(data, w, h, zmax):
    """Inverse of encode_glyph: the h rows of w levels."""
    if w == 0 or h == 0:
        return []
    ngroups = -(-h // GROUP_ROWS)
    pos, rows = ngroups - 1, []
    for k in range(ngroups):
        gh = min(GROUP_ROWS, h - k * GROUP_ROWS)
        end = pos + data[k] if k < ngroups - 1 else len(data)
        nibs = [n for b in data[pos:end] for n in (b >> 4, b & 15)]
        px = decode_stream(nibs, zmax, gh * w)
        rows += [px[r * w:(r + 1) * w] for r in range(gh)]
        pos = end
    return rows


def packed_size(g):
    """Bytes the glyph took as plain 4 bpp rows (two pixels per byte): the figure the coding is compared with."""
    return (g["w"] + 1) // 2 * g["h"]


def encode_font(f):
    """Add the coded `bitmap`, every glyph's `off` and the font's `zmax` to a font dict of level rows. The zero
    run limit is the one that makes the whole table smallest; the result is decoded again and compared."""
    best = None
    for zmax in range(1, 16):
        blobs = [encode_glyph(g["rows"], g["w"], zmax) for g in f["glyphs"]]
        size = sum(map(len, blobs))
        if best is None or size < best[0]:
            best = (size, zmax, blobs)
    size, zmax, blobs = best
    assert size < 65536, f"{f['name']}: bitmap too large for 16-bit offsets"
    off = 0
    for g, blob in zip(f["glyphs"], blobs):
        g["off"] = off
        off += len(blob)
        assert decode_glyph(blob, g["w"], g["h"], zmax) == g["rows"], f"{f['name']}: U+{g['cp']:04X} does not round-trip"
    f.update(bitmap=b"".join(blobs), zmax=zmax, packed=sum(map(packed_size, f["glyphs"])))
    return f


# ---- Atkinson Hyperlegible Mono (outline font, 4 bpp) ------------------------------------------------

# Per role: (digit advance in px that the UI layout assumes, maximum line height). The line height is
# ascent + descent over the glyph set; the 48 px role is digits only, so its cap is the digit height.
ROLES = {12: (6, 12), 16: (8, 16), 24: (12, 24), 48: (24, 32), 64: (32, 40)}
ROLE_CHARS = {12: SET_12, 16: SET_16, 24: SET_16, 48: SET_48, 64: SET_64}
ROLE_ORDER = (12, 16, 24, 48, 64)
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


def build_outline_font(role, main, borrow):
    """One role of the Atkinson set -> font dict of uncompressed levels (see levels_font; encode_font codes
    it). `main` is the face pinned at the role's weight, `borrow` the face the BORROWED glyphs come from."""
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
    glyphs = [dict(cp=ord(ch), w=w, h=h, left=left, top=top, adv=adv, rows=rows) for ch, w, h, left, top, rows in ink]
    assert ascent + descent <= ROLES[role][1] or role in DIGIT_ROLES, f"role {role}: line box {ascent + descent} too tall"
    for g in glyphs:
        assert -128 <= g["left"] < 128 and -128 <= g["top"] < 128
    return encode_font(dict(name=f"font_{role}", size=role, glyphs=glyphs, adv=adv, ascent=ascent, descent=descent,
                            ppem=ppem, wght=ROLE_WGHT[role], borrowed=borrowed, clipped=clipped))


def input_stamp():
    """Fingerprint of everything the Atkinson tables are derived from, without rasterising anything."""
    h = hashlib.sha256()
    for p in (ATKINSON_TTF, BORROW_TTF):
        h.update(p.read_bytes())
    h.update(PARAMS.encode())
    return h.hexdigest()[:16]


def dense_run(f):
    """How many leading glyphs are 0x20, 0x21, ... in order (found by index, so they need no code point)."""
    k = 0
    while k < len(f["glyphs"]) and f["glyphs"][k]["cp"] == 0x20 + k:
        k += 1
    return k


def c_source(f, banner):
    n = len(f["glyphs"])
    dense = dense_run(f)
    sparse = n - dense
    assert dense <= 255 and all(a["cp"] < b["cp"] for a, b in zip(f["glyphs"], f["glyphs"][1:])), f["name"]
    lines = [banner, '#include "k2/ui/font_aa.hpp"', "", "namespace k2 {", "", "namespace {", ""]
    lines.append(f"const uint8_t bitmap[{len(f['bitmap'])}] = {{")
    for i in range(0, len(f["bitmap"]), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in f["bitmap"][i:i + 16]) + ",")
    lines.append("};\n")
    if sparse:
        lines.append(f"const uint16_t codepoints[{sparse}] = {{")
        cps = [f"0x{g['cp']:04X}" for g in f["glyphs"][dense:]]
        for i in range(0, sparse, 12):
            lines.append("    " + ", ".join(cps[i:i + 12]) + ",")
        lines.append("};\n")
    lines.append(f"const Glyph glyphs[{n}] = {{")
    lines.append("    /* offset, w, h, left, top */")
    for g in f["glyphs"]:
        ch = chr(g["cp"])
        label = ch if 32 < g["cp"] < 127 and ch not in "*/\\" else f"U+{g['cp']:04X}"
        lines.append(f"    {{ {g['off']}, {g['w']}, {g['h']}, {g['left']}, {g['top']} }}, /* {label} */")
    lines.append("};\n")
    lines.append("}  // namespace\n")
    lines.append(f"const Font {f['name']} = {{")
    lines.append(f"    .bitmap = bitmap, .codepoints = {'codepoints' if sparse else 'nullptr'}, .glyphs = glyphs,")
    lines.append(f"    .count = {n}, .dense = {dense}, .adv = {f['adv']}, .ascent = {f['ascent']}, "
                 f".descent = {f['descent']},")
    lines.append(f"    .height = {f['ascent'] + f['descent']}, .px = {f['size']}, .zmax = {f['zmax']},")
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

// One glyph, cropped to its ink box and lying inside its own advance cell. Its pixels start at
// font.bitmap + offset and are coverage levels 0..15 (the blend LUT index, 0 = background .. 15 =
// foreground), run-length coded as nibbles, high nibble first. The h rows are cut into groups of 8
// (the last one may be shorter); each group is one flat row-major stream of its pixels, starts on a
// byte boundary, and no run crosses a group. A glyph's data is
//   (groups - 1) bytes: the byte length of every group but the last, then
//   the groups one after another; the last byte of a group is padded with a 0 nibble.
// Tokens (z = Font::zmax, chosen per font by the generator to make the table smallest):
//   t, 1..15          one pixel of level t
//   0 n, n <  z       n + 1 pixels of level 0   (1 .. z)
//   0 n, n >= z       n - z + 3 pixels of level 15   (3 .. 18 - z)
// Row r of a glyph is therefore reached by adding the lengths of the r / 8 groups before it and
// scanning at most 7 rows of tokens: any glyph and row is addressable with no buffer. A glyph
// without ink (w = 0, h = 0) has no data.
// `left` = box x offset from the pen, `top` = box height above the baseline (the box covers rows
// top-1 .. top-h of the baseline).
struct Glyph {
    uint16_t offset;
    uint8_t  w, h;
    int8_t   left, top;
};

// Glyph lookup: glyphs[0 .. dense-1] are the code points 0x20 .. 0x20+dense-1 in order, found by
// index with no table; glyphs[dense .. count-1] carry the sorted code points in `codepoints`
// (count - dense entries, entry i belongs to glyphs[dense + i]).
struct Font {
    const uint8_t  *bitmap;
    const uint16_t *codepoints;  // sorted ascending, the glyphs after the dense run
    const Glyph    *glyphs;      // `count` entries
    uint16_t count;
    uint8_t  dense;    // leading glyphs that are 0x20, 0x21, ... with no codepoints entry
    uint8_t  adv;      // advance of every glyph (monospace)
    uint8_t  ascent;   // tallest glyph above the baseline
    uint8_t  descent;  // deepest glyph below the baseline
    uint8_t  height;   // ascent + descent: pixel height of one text line
    uint8_t  px;       // nominal em size in pixels
    uint8_t  zmax;     // zero-run limit of this font's pixel coding (1..15), see Glyph
};

@DECLS@
}  // namespace k2
"""

SOURCE_NOTE = {
    "atkinson": "// Monospace 16-level fonts rasterised from Atkinson Hyperlegible Mono 2.001 (SIL OFL 1.1)\n"
                "// at 12, 16, 24, 48 and 64 px cells; the four arrows come from JetBrains Mono 2.304 (SIL OFL 1.1).\n"
                "// Sources, hashes and licences: third_party/fonts/.",
    "spleen": "// Monospace bitmap fonts (ink = level 15) converted from Spleen 2.2.0 (BSD-2-Clause) BDF sources\n"
              "// (6x12, 8x16, 12x24, and 12x24 scaled x2 for the 48 px digits). Sources and licence:\n"
              "// third_party/fonts/spleen/.",
}


def decl(f):
    return (f"/* {f['size']} px, {len(f['glyphs'])} glyphs, advance {f['adv']}, ascent {f['ascent']}, "
            f"descent {f['descent']} */\nextern const Font {f['name']};\n")


def flash_note(f):
    table = len(f["glyphs"]) * 6 + (len(f["glyphs"]) - dense_run(f)) * 2
    return (f"{f['name']}: {len(f['glyphs'])} glyphs, zmax {f['zmax']}, bitmap {len(f['bitmap'])} B "
            f"(plain 4 bpp {f['packed']} B), tables {table} B, total ~{len(f['bitmap']) + table + 20} B flash")


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


LEVELS_TXT = ROOT / "tools/font_levels.txt"


class StaleLevels(Exception):
    """tools/font_levels.txt is missing or made from other inputs, and there is no rasteriser to remake it."""


def levels_text(fonts):
    """The uncompressed level cache: one `font` line, then per glyph a `glyph` line and h rows of hex digits."""
    lines = ["# GENERATED by tools/gen_font.py - do not edit.",
             "# Uncompressed coverage levels (one hex digit per pixel, 0 = background .. F = foreground) of the",
             "# Atkinson fonts as rasterised: the input of the coding in src/ui/fonts/*.cpp, and the reference",
             "# tests/test_font_decode.cpp decodes the shipped tables against.",
             "# font <px> <count> <adv> <ascent> <descent> <height> ppem <p> wght <w> borrowed <cp,..|->",
             "# glyph <codepoint> <w> <h> <left> <top>, then h rows",
             f"# Input stamp {input_stamp()}"]
    for f in fonts:
        borrowed = ",".join(str(ord(c)) for c in f["borrowed"]) or "-"
        lines.append(f"font {f['size']} {len(f['glyphs'])} {f['adv']} {f['ascent']} {f['descent']} "
                     f"{f['ascent'] + f['descent']} ppem {f['ppem']} wght {f['wght']} borrowed {borrowed}")
        for g in f["glyphs"]:
            lines.append(f"glyph {g['cp']} {g['w']} {g['h']} {g['left']} {g['top']}")
            lines += ["".join(f"{v:X}" for v in row) for row in g["rows"]]
    return "\n".join(lines) + "\n"


def parse_levels(text):
    """(input stamp, [font dict of level rows]) from levels_text output."""
    stamp, fonts, rows_left = None, [], 0
    for line in text.splitlines():
        if line.startswith("# Input stamp "):
            stamp = line.split()[-1]
        if line.startswith("#") or not line:
            continue
        t = line.split()
        if rows_left:
            g["rows"].append([int(c, 16) for c in line])
            rows_left -= 1
        elif t[0] == "font":
            role, _count, adv, asc, desc, _h = map(int, t[1:7])
            borrowed = "".join(chr(int(c)) for c in t[12].split(",")) if t[12] != "-" else ""
            fonts.append(dict(name=f"font_{role}", size=role, glyphs=[], adv=adv, ascent=asc, descent=desc,
                              ppem=float(t[8]), wght=int(t[10]), borrowed=borrowed, clipped=[]))   # clipped: report only
        else:
            cp, w, h, left, top = map(int, t[1:6])
            g = dict(cp=cp, w=w, h=h, left=left, top=top, adv=fonts[-1]["adv"], rows=[])
            fonts[-1]["glyphs"].append(g)
            rows_left = h
    return stamp, fonts


def atkinson_levels():
    """The Atkinson fonts as uncompressed level rows, coded: rasterised when freetype-py and fonttools are
    installed, else read from tools/font_levels.txt (which must carry today's input stamp)."""
    if have_rasteriser():
        borrow = Outline(BORROW_TTF)
        return [build_outline_font(role, Outline(ATKINSON_TTF, ROLE_WGHT[role]), borrow) for role in ROLE_ORDER]
    if not LEVELS_TXT.exists():
        raise StaleLevels(f"{LEVELS_TXT.relative_to(ROOT)} is missing and freetype-py/fonttools are not installed")
    stamp, fonts = parse_levels(LEVELS_TXT.read_text())
    if stamp != input_stamp():
        raise StaleLevels(f"{LEVELS_TXT.relative_to(ROOT)} is from other inputs ({stamp}, now {input_stamp()}) and "
                          "freetype-py/fonttools are not installed: run tools/gen_font.py once from a venv that has them")
    return [encode_font(f) for f in fonts]


def atkinson_outputs():
    outs, decls, report = {}, [], []
    fonts = atkinson_levels()
    for f in fonts:
        lent = f" Borrowed from JetBrains Mono: {' '.join(f['borrowed'])}." if f["borrowed"] else ""
        outs[OUT_SRC / f"{f['name']}.cpp"] = c_source(
            f, "// GENERATED by tools/gen_font.py - do not edit.\n"
               f"// Atkinson Hyperlegible Mono 2.001 (SIL OFL 1.1), wght {f['wght']}, {f['ppem']} ppem, 16 levels, run-length coded.{lent}\n"
               f"// Input stamp {input_stamp()}")
        decls.append(decl(f))
        report.append(flash_note(f) + f", {f['ppem']} ppem wght {f['wght']}"
                      + (f", borrowed '{''.join(f['borrowed'])}'" if f["borrowed"] else "")
                      + (f", clipped {f['clipped']}" if f["clipped"] else ""))
    outs[OUT_HDR] = HEADER.replace("@SOURCE@", SOURCE_NOTE["atkinson"]).replace("@DECLS@", "\n".join(decls))
    outs[LEVELS_TXT] = levels_text(fonts)
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--source", choices=["atkinson", "spleen"], default="atkinson")
    args = ap.parse_args()
    try:
        outs, report = make_outputs(args.source)
    except StaleLevels as e:
        print("out of date:", e)
        return 1
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
