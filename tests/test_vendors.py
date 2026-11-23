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
            self.assertLessEqual(len(r.name), gen_vendors.NAME_MAX, r.name)
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
        self.assertEqual({by[v].name for v in (0x0766, 0x062E)}, {"JPC"})   # both registrants carry JPC
        self.assertEqual(by[0x0DBF].name, "Jess")                          # "Jess-Link International" does not
        self.assertEqual(by[0x3271].cat, "psu")
        self.assertFalse(by[0x2943].weak)

    def test_card_tool_uses_the_table(self):
        import report_card
        self.assertEqual(report_card.BRANDS[0x291A], "ANKER")   # table names are shown in capitals
        self.assertEqual(len(report_card.BRANDS), len(self.rows))

    def test_results_page_names_are_capitals(self):
        text = gen_vendors.render_js(self.rows)
        self.assertIn('0x291A: "ANKER"', text)
        self.assertNotIn('"Anker"', text)

    def test_every_vid_is_on_the_usbif_list(self):
        if not os.path.exists(LIST):
            self.skipTest("runs/vendor-ids/vendor_ids.txt not fetched")
        with open(LIST, encoding="utf-8") as f:
            self.assertEqual(gen_vendors.verify(self.rows, f.read()), [])


class Pack(unittest.TestCase):
    """The 6-bit name pool: every csv name survives pack -> unpack as its capitals, whatever the neighbours."""

    def test_every_name_round_trips(self):
        rows = sorted(gen_vendors.load(gen_vendors.CSV), key=lambda r: r.vid)
        pool, offs = gen_vendors.pack_names([r.name for r in rows])
        self.assertEqual(len(offs), len(rows))
        for r, off in zip(rows, offs):
            name, end = gen_vendors.unpack_name(pool, off)
            self.assertEqual(name, r.name.upper())
        self.assertEqual(end, 6 * sum(len(r.name) + 1 for r in rows))   # nothing but names and terminators

    def test_alphabet_edges_and_sixteen_characters(self):
        for n in ("a", "z", "0", "9", "a b", "x-y_z.1", "abcdefghijklmnop", "a" * 16):
            pool, offs = gen_vendors.pack_names([n, "q"])
            self.assertEqual(gen_vendors.unpack_name(pool, offs[0])[0], n.upper())
            self.assertEqual(gen_vendors.unpack_name(pool, offs[1])[0], "Q")

    def test_rejects_what_the_alphabet_cannot_hold(self):
        R = gen_vendors.Row
        for bad in ("a" * 17, "", " lead", "trail ", "two  spaces", "a&b", "a+b", "caf\xe9"):
            with self.assertRaises(ValueError, msg=repr(bad)):
                gen_vendors.check_rows([R(0x1111, bad, "brand", "x", "")])
        gen_vendors.check_rows([R(0x1111, "Sixteen Chars Ok", "brand", "Sixteen Chars Ok", "")])   # exactly 16, mixed case

    def test_the_pool_fits_the_skip_index(self):
        self.assertLess(gen_vendors.pool_bits(gen_vendors.load(gen_vendors.CSV)), 1 << 16)   # kSkip is uint16


class Registrant(unittest.TestCase):
    """A name is a shortening of its USB-IF registrant, never a brand, parent or subsidiary (user decision 2026-11-23)."""

    def test_shortenings_and_acronyms_pass(self):
        for name, reg in (("Zimi", "Zimi Corporation"), ("FTDI", "Future Technology Devices International Limited"),
                          ("MSI", "Micro-Star International Co. Ltd."), ("STMicro", "STMicroelectronics"),
                          ("YC Cable", "Y.C. Cable U.S.A. Inc"), ("MornStar", "Morning Star Digital Connector Co. Ltd."),
                          ("Shinko", "Dong Guan Shinko Wire Co. Ltd."), ("E.U Conn", "E.U CONNECTOR(M) SDN BHD."),
                          ("ECT", "Electric Connector Technology Co. Ltd."), ("Qinheng", "Nanjing Qinheng Microelectronics Co. Ltd.")):
            self.assertTrue(gen_vendors.derives(name, reg), (name, reg))

    def test_swaps_fail(self):
        for name, reg in (("CUKTECH", "Zimi Corporation"), ("WCH", "Nanjing Qinheng Microelectronics Co. Ltd."),
                          ("JPC", "Jess-Link International"), ("Xiaomi", "Zimi Corporation"),
                          ("Cable Zimi", "Zimi Corporation Cable"),   # words out of the registrant's order
                          ("Zimix", "Zimi Corporation")):
            self.assertFalse(gen_vendors.derives(name, reg), (name, reg))

    def test_generation_refuses_a_swap(self):
        with self.assertRaises(ValueError):
            gen_vendors.check_rows([gen_vendors.Row(0x2B01, "CUKTECH", "shared", "Zimi Corporation", "")])

    def test_exceptions_are_few_and_exact(self):
        self.assertLessEqual(len(gen_vendors.SHORTHAND), 4)
        R = gen_vendors.Row
        for (vid, name, reg) in gen_vendors.SHORTHAND:
            gen_vendors.check_rows([R(vid, name, "cable", reg, "")])
            with self.assertRaises(ValueError):   # keyed on the registrant too: an edited registrant needs a new look
                gen_vendors.check_rows([R(vid, name, "cable", reg + " Holdings", "")])

    def test_zimi_vid_reads_zimi(self):
        by = {r.vid: r for r in gen_vendors.load(gen_vendors.CSV)}
        self.assertEqual(by[0x2B01].name, "Zimi")


class Verify(unittest.TestCase):
    TXT = "Anker Innovations Limited      10522\nAcme, Inc.   99\nnoise line\nLite\u2010On Corp.    1226\n"

    def test_parse(self):
        self.assertEqual(gen_vendors.parse_usbif(self.TXT), {10522: ["Anker Innovations Limited"], 99: ["Acme, Inc."],
                                                            1226: ["Lite-On Corp."]})   # the PDF's U+2010 hyphen folded

    def test_verify_flags_a_swap(self):
        R = gen_vendors.Row
        self.assertEqual(len(gen_vendors.verify([R(10522, "Brand", "brand", "Anker Innovations Limited", "")], self.TXT)), 1)

    def test_verify_flags_wrong_and_missing(self):
        R = gen_vendors.Row
        ok = R(10522, "Anker", "brand", "Anker Innovations Limited", "")
        self.assertEqual(gen_vendors.verify([ok], self.TXT), [])
        self.assertEqual(len(gen_vendors.verify([R(10522, "SE", "brand", "Someone Else", "")], self.TXT)), 1)
        self.assertEqual(len(gen_vendors.verify([R(5, "Gone", "brand", "Gone", "")], self.TXT)), 1)
        self.assertEqual(gen_vendors.verify([R(99, "Acme", "brand", "Acme Inc.", "")], self.TXT), [])   # commas dropped in the csv


if __name__ == "__main__":
    unittest.main()
