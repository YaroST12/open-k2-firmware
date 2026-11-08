# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/gen_font.py: the committed font sources are current, and the BDF reader and the hand-drawn
fill-ins produce the glyph geometry the renderer expects."""
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import gen_font

BDF = """STARTFONT 2.1
FONT_ASCENT 3
FONT_DESCENT 1
CHARS 1
STARTCHAR A
ENCODING 65
DWIDTH 4 0
BBX 3 2 0 -1
BITMAP
A0
40
ENDCHAR
ENDFONT
"""


class GenFont(unittest.TestCase):
    def test_generated_files_current(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools/gen_font.py"), "--check"])
        self.assertEqual(r.returncode, 0, "run tools/gen_font.py")

    def test_parse_bdf(self):
        asc, desc, g = gen_font.parse_bdf(BDF)
        self.assertEqual((asc, desc), (3, 1))
        # BBX 3x2 with y offset -1: the box spans baseline+1 .. baseline-1, so top = 1
        self.assertEqual(g[65], (3, 2, 0, 1, 4, [[1, 0, 1], [0, 1, 0]]))

    def test_override_glyph_sits_in_the_cell(self):
        w, h, left, top, adv, rows = gen_font.art_glyph(["#.", ".#"], 9, 2)
        self.assertEqual((w, h, left, top, adv), (2, 2, 0, 9, 2))
        self.assertEqual(rows, [[1, 0], [0, 1]])

    def test_levels_reach_full_colour_at_the_white_point(self):
        v = int(round(255 * gen_font.WHITE_POINT))
        self.assertEqual(gen_font.to_levels([[0, v, 255]]), [[0, 15, 15]])
        # gamma < 1 lifts the middle of the curve; a quarter coverage is above 4 of 15
        self.assertGreater(gen_font.to_levels([[64]])[0][0], 4)

    def test_pack4_pads_odd_widths_and_puts_the_left_pixel_high(self):
        self.assertEqual(gen_font.pack4([[1, 2, 3]], 3), bytes([0x12, 0x30]))
        self.assertEqual(gen_font.pack4([[15, 0], [0, 15]], 2), bytes([0xF0, 0x0F]))

    def test_fit_cell_shifts_then_clips(self):
        rows = [[1, 2, 3, 4]]
        self.assertEqual(gen_font.fit_cell(4, -1, rows, 6), (4, 0, rows, 0))      # shifted in from the left
        self.assertEqual(gen_font.fit_cell(4, 4, rows, 6), (4, 2, rows, 0))       # shifted in from the right
        w, left, kept, lost = gen_font.fit_cell(4, 0, rows, 3)                    # wider than the cell
        self.assertEqual((w, left, kept, lost), (3, 0, [[1, 2, 3]], 4))

    def test_spleen_fallback_writes_one_bit_tables(self):
        outs, _ = gen_font.make_outputs("spleen")
        header = outs[gen_font.OUT_HDR]
        self.assertIn("Spleen", header)
        for path, text in outs.items():
            if path != gen_font.OUT_HDR:
                self.assertIn(".bpp = 1,", text)

    def test_committed_tables_are_four_bit_and_inside_their_cells(self):
        import re
        for role in (12, 16, 24, 48, 64):
            text = (ROOT / f"src/ui/fonts/font_{role}.cpp").read_text()
            self.assertIn(".bpp = 4,", text)
            adv = int(re.search(r"\.adv = (\d+)", text).group(1))
            glyphs = re.findall(r"\{ (\d+), (\d+), (\d+), (-?\d+), (-?\d+), (\d+) \}", text)
            self.assertGreater(len(glyphs), 10)
            for _off, w, _h, left, _top, a in glyphs:
                self.assertEqual(int(a), adv)
                self.assertTrue(int(w) == 0 or 0 <= int(left) and int(left) + int(w) <= adv, (role, w, left))


if __name__ == "__main__":
    unittest.main()
