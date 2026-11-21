# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/ct_soak.py: the parts that need no device."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"))
import ct_soak  # noqa: E402


class SoakTest(unittest.TestCase):
    def test_summary_line_picks_the_counts(self):
        text = "level: medium\nRun 3 · PD3.1 · 9 PDOs\n26 pass, 0 fail, 2 skip · done\n- PD: 20V 5A"
        self.assertEqual(ct_soak.summary_line(text), "26 pass, 0 fail, 2 skip · done")
        self.assertEqual(ct_soak.summary_line("could not reach Review START"), "no summary")

    def test_knob_tables_cover_every_knob(self):
        import k2_remote as kr
        self.assertEqual(sorted(ct_soak.KNOB_CHOICES), sorted(kr.LEGACY_KNOBS))
        self.assertEqual(sorted(ct_soak.KNOB_DEFAULTS), sorted(kr.LEGACY_KNOBS))

    def test_read_log_skips_broken_lines(self):
        import tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write('{"text": "[TEST] Complete"}\nnot json\n{"text": "x"}\n')
        try:
            self.assertEqual([x["text"] for x in ct_soak.read_log(f.name)], ["[TEST] Complete", "x"])
        finally:
            os.unlink(f.name)
        self.assertEqual(ct_soak.read_log("/nonexistent/log.jsonl"), [])


if __name__ == "__main__":
    unittest.main()
