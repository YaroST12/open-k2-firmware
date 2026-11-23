# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""src/ui/ui_icons.cpp must be the output of tools/gen_icons.py for the current mockup ICONS table,
and the Icon enum must list the icons in the same order."""
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Icons(unittest.TestCase):
    def test_generated_file_current(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools/gen_icons.py"), "--check"])
        self.assertEqual(r.returncode, 0, "run tools/gen_icons.py")

    def test_enum_order(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import gen_icons
        names = list(gen_icons.load_icons())
        hdr = (ROOT / "include/k2/ui/ui_icons.hpp").read_text()
        enum = re.search(r"enum class Icon[^{]*\{([^}]*)\}", hdr).group(1)
        self.assertEqual([n.strip().lower() for n in enum.split(",")], names)

    def icons(self):
        sys.path.insert(0, str(ROOT / "tools"))
        import gen_icons
        return gen_icons.load_icons()

    def test_icons_centred(self):
        # The ink box sits in the middle of the 12x12 cell (within a pixel), so a row of icons lines up.
        for name, rows in self.icons().items():
            if name == "sand":   # a fill that lives inside the glass
                continue
            xs = [x for r in rows for x, c in enumerate(r) if c == "#"]
            ys = [y for y, r in enumerate(rows) if "#" in r]
            self.assertLessEqual(abs(min(xs) - (11 - max(xs))), 1, name)
            self.assertLessEqual(abs(min(ys) - (11 - max(ys))), 1, name)

    def test_symmetric_icons(self):
        icons = self.icons()
        for name in ("therm", "warn", "cross", "phone", "chip", "ohm", "plug", "anon", "brick", "emark", "blackbox",
                     "ghost", "info", "socket", "monitor", "glass", "sand", "keys"):
            self.assertTrue(all(r == r[::-1] for r in icons[name]), name + " is not left-right symmetric")
        for name in ("socket", "glass", "cross", "chip", "keys"):
            self.assertEqual(icons[name], icons[name][::-1], name + " is not top-bottom symmetric")

    def test_sand_inside_glass(self):
        icons = self.icons()
        glass, sand = icons["glass"], icons["sand"]
        for y in range(12):
            for x in range(12):
                if sand[y][x] != "#":
                    continue
                self.assertEqual(glass[y][x], ".", f"sand over the glass at {x},{y}")
                left = glass[y][:x].count("#")
                right = glass[y][x + 1:].count("#")
                self.assertTrue(left and right, f"sand outside the walls at {x},{y}")

    def test_socket_is_round_with_two_pins(self):
        rows = self.icons()["socket"]
        self.assertEqual(rows[5], "#..##..##..#")   # two 2 px pins inside the round rim
        self.assertEqual(rows[0], rows[11])
        self.assertEqual(rows[0].count("#"), 6)


if __name__ == "__main__":
    unittest.main()
