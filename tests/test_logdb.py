# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/logdb.py: source scan, stable IDs, and decoding of the compact log lines.
The encoded lines are the literals of tests/test_logfmt.cpp."""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import logdb  # noqa: E402

DB = {1: "[USB] gap=%dms drop=%u\n", 2: "[EM] name=%s|%s\n", 3: "[PD] VID=%04X PID=%04x\n",
      4: "[X] 100%% %5d|%c|%03d\n", 5: "[SYS] alive\n"}


class Decode(unittest.TestCase):
    def d(self, line):
        return logdb.decode_line(line, DB)

    def test_ints(self):
        self.assertEqual(self.d("msg#1 -12 1F\n"), "[USB] gap=-12ms drop=31\n")

    def test_strings_unescape(self):
        self.assertEqual(self.d("msg#2 Anker%20735 50%25%0A\n"), "[EM] name=Anker 735|50%\n\n")

    def test_hex_width_upper(self):
        # k2::fmt prints %x upper-case too
        self.assertEqual(self.d("msg#3 5AC 1A\n"), "[PD] VID=05AC PID=001A\n")

    def test_width_percent_char(self):
        self.assertEqual(self.d("msg#4 42 41 -5\n"), "[X] 100%    42|A|-05\n")

    def test_no_args(self):
        self.assertEqual(self.d("msg#5\n"), "[SYS] alive\n")

    def test_unknown_id_and_plain_text_pass_through(self):
        self.assertEqual(self.d("msg#999 1 2\n"), "msg#999 1 2\n")
        self.assertEqual(self.d("[FAULT] HardFault pc=08001234\n"), "[FAULT] HardFault pc=08001234\n")

    def test_missing_args_and_cut_escape(self):
        self.assertEqual(self.d("msg#1 3\n"), "[USB] gap=3ms drop=?\n")
        self.assertEqual(self.d("msg#2 ab%2\n"), "[EM] name=ab?|?\n")

    def test_stream_of_reports(self):
        text = "msg#5\nmsg#1 1 2\n[BOOT] plain\n"
        self.assertEqual(logdb.decode_text(text, DB), "[SYS] alive\n[USB] gap=1ms drop=2\n[BOOT] plain\n")


class Scan(unittest.TestCase):
    def test_extracts_concatenated_literals_and_escapes(self):
        src = 'K2LOG("[A] x=%d\\n", x);\n  K2LOG("[B] one "\n        "two\\t%s\\n", s);\n log("not me");'
        self.assertEqual(logdb.scan_text(src), ["[A] x=%d\n", "[B] one two\t%s\n"])

    def test_ids_are_stable_and_append_only(self):
        db = logdb.assign({"1": "[A]\n", "2": "[B]\n"}, ["[C]\n", "[A]\n"])
        self.assertEqual(db, {"1": "[A]\n", "2": "[B]\n", "3": "[C]\n"})

    def test_header_maps_formats_to_ids(self):
        h = logdb.header({"1": "[A] \"q\"\n", "3": "[C]\\\n"})
        self.assertIn('{1, "[A] \\"q\\"\\n"}', h)
        self.assertIn('{3, "[C]\\\\\\n"}', h)


class Tree(unittest.TestCase):
    def test_every_k2log_format_has_an_id_in_the_committed_table(self):
        known = set(logdb.load().values())
        self.assertEqual([f for f in logdb.scan_tree() if f not in known], [])


if __name__ == "__main__":
    unittest.main()
