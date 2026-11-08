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


if __name__ == "__main__":
    unittest.main()
