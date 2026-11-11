# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for the pure parts and the safety allowlist of tools/loop_survey.py (no device)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import loop_survey as s


def loop_read(page=1, loop_max=47, svc_max=47, over=(3, 0, 1)):
    keys = ["<=1", "<=2", "<=5", "<=10", "<=20", "<=50", "<=100", ">100"]
    hist = dict(zip(keys, [9000, 40, 10, 5, 2, *over]))
    return {"elapsed_ms": 10000, "loop_max_ms": loop_max, "loop_hist": hist, "svc_max_ms": svc_max, "svc_hist": hist,
            "page": page, "page_ms": 47.2, "page_px": 87024, "worst_page": page, "worst_ms": 47.2, "worst_px": 87024,
            "over_20ms": sum(over)}


class Allowlist(unittest.TestCase):
    def test_reads_and_navigation_only(self):
        for ok in (["ping"], ["state"], ["page", "home"], ["debug", "loop", "--reset"], ["debug", "lcd"]):
            self.assertTrue(s.allowed(ok), ok)
        for bad in (["press", "ok"], ["fc", "qc", "9"], ["debug", "pdos", "--fake", "3"], ["fault", "hard"], ["reboot"],
                    ["debug", "pd-msg", "0000"], ["pd-reread"], ["identify"], ["dfu", "--confirm"]):
            self.assertFalse(s.allowed(bad), bad)

    def test_remote_refuses_before_any_usb(self):
        with self.assertRaises(s.SurveyError):
            s.remote("press", "ok")


class Pure(unittest.TestCase):
    def test_page_index_matches_the_firmware(self):
        self.assertEqual([s.page_index(p) for p in ("home", "detail", "settings", "connection", "diag")], [0, 1, 5, 16, 17])

    def test_reboot_detection(self):
        self.assertFalse(s.rebooted(100000, 190000, 90.0))
        self.assertTrue(s.rebooted(100000, 5000, 90.0))     # the uptime went backwards
        self.assertTrue(s.rebooted(100000, 120000, 90.0))   # 20 s of uptime over 90 s of run

    def test_row_and_report(self):
        r = s.row("detail", loop_read(), {"paint_px": 1000, "paint_ms": 10}, {"paint_px": 51000, "paint_ms": 150},
                  {"poll_gap_max_ms": 48})
        self.assertEqual((r["paint_px"], r["paint_ms"], r["usb_poll_gap_max_ms"]), (50000, 140, 48))
        self.assertEqual(r["passes_over_20ms"], 4)
        self.assertTrue(r["over"])
        calm = s.row("home", loop_read(page=0, loop_max=2, svc_max=2, over=(0, 0, 0)), {}, {}, None)
        self.assertFalse(calm["over"])
        self.assertIsNone(calm["usb_poll_gap_max_ms"])
        md = s.report_md("1.0.0-1-abc", 10, [r, calm], ["note one"])
        self.assertIn("| detail **OVER** | 47.2 | 87024 | 47 | 47 | 4 |", md)
        self.assertIn("- note one", md)


if __name__ == "__main__":
    unittest.main()
