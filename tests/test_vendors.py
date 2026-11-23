# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/vendors.csv and tools/gen_vendors.py: the table rules, the committed header matching the csv (the gate check), and
the USB-IF cross-check (runs only when the public list has been fetched into runs/vendor-ids/)."""
import os
import sys
import unittest

ROOT = os.path.join(os.path.dirname(__file__), "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_vendors  # noqa: E402

LIST = os.path.join(ROOT, "runs", "vendor-ids", "vendor_ids.txt")


class Table(unittest.TestCase):
    rows = gen_vendors.load(gen_vendors.CSV)

    def test_size(self):
        self.assertGreaterEqual(len(self.rows), 150)
        self.assertLessEqual(len(self.rows), 300)

    def test_rules(self):
        seen = set()
        for r in self.rows:
            self.assertNotIn(r.vid, seen, "duplicate VID %04X" % r.vid)
            seen.add(r.vid)
            self.assertTrue(0 < r.vid < 0xFFFF)
            self.assertLessEqual(len(r.name), 10, r.name)
            self.assertRegex(r.name, r"^[A-Za-z0-9][A-Za-z0-9 _.-]*[A-Za-z0-9_.-]$|^[A-Za-z0-9]$")
            self.assertIn(r.cat, gen_vendors.CATS)
            self.assertTrue(r.usbif, "%04X has no USB-IF registrant" % r.vid)

    def test_anker_is_anker(self):
        by = {r.vid: r for r in self.rows}
        self.assertEqual(by[0x291A].name, "Anker")
        self.assertEqual(by[0x2E99].name, "Hynetek")
        self.assertFalse(by[0x291A].weak)
        self.assertTrue(by[0x0451].weak)

    def test_committed_outputs_match_csv(self):
        for path, text in ((gen_vendors.HDR, gen_vendors.render(self.rows)), (gen_vendors.JS, gen_vendors.render_js(self.rows))):
            with open(path, encoding="utf-8") as f:
                self.assertEqual(f.read(), text, "stale %s: run python3 tools/gen_vendors.py" % os.path.basename(path))
        self.assertEqual(gen_vendors.main(["--check"]), 0)

    def test_cable_makers(self):
        by = {r.vid: r for r in self.rows}
        self.assertEqual(by[0x2CA1].name, "Veetone")
        self.assertTrue(by[0x2CA1].weak)
        self.assertEqual({by[v].name for v in (0x0DBF, 0x0766, 0x062E)}, {"JPC"})   # one company, three VIDs, one name
        self.assertEqual(by[0x3271].cat, "psu")
        self.assertFalse(by[0x2943].weak)

    def test_card_tool_uses_the_table(self):
        import report_card
        self.assertEqual(report_card.BRANDS[0x291A], "Anker")
        self.assertEqual(len(report_card.BRANDS), len(self.rows))

    def test_every_vid_is_on_the_usbif_list(self):
        if not os.path.exists(LIST):
            self.skipTest("runs/vendor-ids/vendor_ids.txt not fetched")
        with open(LIST, encoding="utf-8") as f:
            self.assertEqual(gen_vendors.verify(self.rows, f.read()), [])


class Verify(unittest.TestCase):
    TXT = "Anker Innovations Limited      10522\nAcme, Inc.   99\nnoise line\n"

    def test_parse(self):
        self.assertEqual(gen_vendors.parse_usbif(self.TXT), {10522: ["Anker Innovations Limited"], 99: ["Acme, Inc."]})

    def test_verify_flags_wrong_and_missing(self):
        R = gen_vendors.Row
        ok = R(10522, "Anker", "brand", "Anker Innovations Limited", "")
        self.assertEqual(gen_vendors.verify([ok], self.TXT), [])
        self.assertEqual(len(gen_vendors.verify([R(10522, "X", "brand", "Someone Else", "")], self.TXT)), 1)
        self.assertEqual(len(gen_vendors.verify([R(5, "X", "brand", "Gone", "")], self.TXT)), 1)
        self.assertEqual(gen_vendors.verify([R(99, "Acme", "brand", "Acme Inc.", "")], self.TXT), [])   # commas dropped in the csv


if __name__ == "__main__":
    unittest.main()
