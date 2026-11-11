# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for tools/check_cmd_ids.py: remote command-id uniqueness across the three protocol files."""
import sys
import re
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_cmd_ids as c

CPP = """
enum class Cmd : uint8_t {
    Ping = 1, Button = 2, GetState = 3,  // comment, X = 9
    DebugPdos = 0x20,
    Fc = 0x32,
};
"""
PY = "CMD_PING, CMD_BUTTON, CMD_GET_STATE = 1, 2, 3\nCMD_DEBUG_PDOS = 0x20\nCMD_FC = 0x32\nCMD_SCREEN_DATA = 0x8A\n"
JS = "var CMD = { PING: 1, BUTTON: 2, GET_STATE: 3, DEBUG_PDOS: 0x20, FC: 0x32, SCREEN_DATA: 0x8A };"


class Parse(unittest.TestCase):
    def test_cpp(self):
        self.assertEqual(c.parse_cpp(CPP), {"PING": 1, "BUTTON": 2, "GET_STATE": 3, "DEBUG_PDOS": 0x20, "FC": 0x32})

    def test_py(self):
        self.assertEqual(c.parse_py(PY)["DEBUG_PDOS"], 0x20)
        self.assertEqual(c.parse_py(PY)["GET_STATE"], 3)

    def test_js(self):
        self.assertEqual(c.parse_js(JS)["SCREEN_DATA"], 0x8A)


class Check(unittest.TestCase):
    def tables(self):
        return {"cpp": c.parse_cpp(CPP), "py": c.parse_py(PY), "js": c.parse_js(JS)}

    def test_consistent_tables_pass(self):
        self.assertEqual(c.check(self.tables()), [])

    def test_duplicate_id_inside_one_file(self):
        t = self.tables(); t["py"]["NEW"] = 3
        self.assertTrue(any("py" in e and "0x03" in e for e in c.check(t)))

    def test_same_name_different_id_across_files(self):
        t = self.tables(); t["js"]["FC"] = 0x40
        self.assertTrue(any("FC" in e for e in c.check(t)))

    def test_same_id_different_name_across_files(self):
        t = self.tables(); t["cpp"]["FOO"] = 0x8A
        self.assertTrue(any("0x8A" in e for e in c.check(t)))


HDR = """
//   range core      0x01-0x1F
//   range debug     0x20-0x2F
//   range system    0x30-0x3F
//   range pd        0x40-0x4F
//   range tools     0x50-0x5F
//   range storage   0x60-0x6F
//   range blackbox  0x70-0x7F
"""


class Ranges(unittest.TestCase):
    def test_parse(self):
        r = c.parse_ranges(HDR)
        self.assertEqual(r[0], ("core", 0x01, 0x1F))
        self.assertEqual(len(r), 7)
        self.assertEqual(r[-1], ("blackbox", 0x70, 0x7F))

    def test_in_range_ok(self):
        self.assertEqual(c.check_ranges({"cpp": {"PING": 1, "FC": 0x32, "CABLE_R": 0x50}}, c.parse_ranges(HDR)), [])

    def test_out_of_range_reported(self):
        errs = c.check_ranges({"cpp": {"ZERO": 0x00, "HIGH": 0x90}}, c.parse_ranges(HDR))
        self.assertEqual(len(errs), 2)
        self.assertIn("ZERO", errs[0])
        self.assertIn("0x90", errs[1])

    def test_stream_frames_exempt(self):
        # response-only stream ids (0x80 and up) in py/js are not commands
        self.assertEqual(c.check_ranges({"py": {"SCREEN_DATA": 0x8A}}, c.parse_ranges(HDR)), [])

    def test_overlapping_ranges_rejected(self):
        with self.assertRaises(ValueError):
            c.parse_ranges("// range a 0x01-0x20\n// range b 0x20-0x2F\n")

    def test_no_ranges_declared_is_an_error(self):
        with self.assertRaises(ValueError):
            c.parse_ranges("// nothing here")

    def test_real_header_declares_ranges_and_all_ids_fit(self):
        root = Path(__file__).resolve().parent.parent
        r = c.parse_ranges((root / "include/k2/core/remote_proto.hpp").read_text())
        self.assertEqual([x[0] for x in r], ["core", "debug", "system", "pd", "tools", "storage", "blackbox"])
        tables = {f: {"cpp": c.parse_cpp, "py": c.parse_py, "js": c.parse_js}[f](p.read_text())
                  for f, p in c.FILES.items()}
        self.assertEqual(c.check_ranges(tables, r), [])
        self.assertEqual(c.check(tables), [])
        for f in [x for x in ("py", "js") if x in tables]:   # js only with K2_WEB=1
            for n, i in tables[f].items():
                if i < 0x80:
                    self.assertEqual(i, tables["cpp"][n])
            self.assertEqual(tables[f]["CHARGER_TEST"], tables["cpp"]["CHARGER_TEST"])
        listed = c.parse_cpp((root / "include/k2/core/remote_proto.hpp").read_text())
        header = (root / "include/k2/core/remote_proto.hpp").read_text()
        ids = re.search(r"kCmdIds\[\]\s*=\s*\{(.*?)\};", header, re.S).group(1)
        self.assertCountEqual([c._snake(n) for n in re.findall(r"Cmd::(\w+)", ids)], listed)


if __name__ == "__main__":
    unittest.main()
