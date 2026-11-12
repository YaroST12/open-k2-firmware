# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The pair tables of tests/test_svooc_script.cpp are the fixtures' content: tools/vooc_edges.py (the framing ground
truth) decodes each fixture into the same command -> phone reply list, so the responder test compares against the
captures and not against a hand-made list."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import vooc_edges as ve

FIXTURES = ROOT / "tests" / "fixtures"
TABLES = {"kCap1": "op15_svooc_entry.edges", "kCap1Replug": "op15_svooc_replug.edges",
          "kUsbc": "op15_svooc_usbc_entry.edges", "kInit1102": "oneplus15_svooc_init.edges"}


def cpp_tables():
    """name -> [(cmd, reply)] parsed from the C++ test."""
    src = (ROOT / "tests" / "test_svooc_script.cpp").read_text()
    out = {}
    for name in TABLES:
        body = re.search(r"const Pair " + name + r"\[\] = \{(.*?)\n\};", src, re.S).group(1)
        out[name] = [(int(c, 16), int(r, 16)) for c, r in re.findall(r"\{0x([0-9A-F]{2}), 0x([0-9A-F]{3})\}", body)]
    return out


def fixture_pairs(name):
    """The first session's command -> reply pairs, from its first ident, as the tool decodes them."""
    edges = ve.edges_from_records((FIXTURES / name).read_bytes())
    frames = [f for f in ve.decode_frames(edges) if f["complete"]]
    pairs = ve._pairs(frames)
    start = next(i for i, p in enumerate(pairs) if p[1] == 0xA8)
    return [(cmd, rep) for _, cmd, rep in pairs[start:]]


class FixtureTables(unittest.TestCase):
    def test_every_table_is_a_prefix_of_its_fixture(self):
        tables = cpp_tables()
        for name, fixture in TABLES.items():
            with self.subTest(name):
                want = tables[name]
                self.assertGreaterEqual(len(want), 25)
                self.assertEqual(fixture_pairs(fixture)[:len(want)], want)


if __name__ == "__main__":
    unittest.main()
