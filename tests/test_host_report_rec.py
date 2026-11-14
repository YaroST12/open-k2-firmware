# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the loop probe's Host report reader (tools/host_report_rec.py) and `k2_remote.py loop-probe --host`
(CMD_LOOP_PROBE 0x57, selector 1). The fixture tests/fixtures/host_report.hex is the C++ sample record as its flash file
(tests/test_host_report.cpp, test_fixture): both sides read the same bytes."""
import json
import os
import sys
import types
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import host_report_rec as hr  # noqa: E402
import k2_remote as kr  # noqa: E402


def fixture_file():
    with open(os.path.join(HERE, "fixtures", "host_report.hex")) as f:
        return bytes.fromhex(f.read().strip())


class FakeLink:
    """CMD_LOOP_PROBE like the firmware dispatcher: off(2) [sel(1)] -> off total n data; one blob per selector."""

    def __init__(self, files):
        self.files = files
        self.calls = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_LOOP_PROBE
        assert len(payload) in (2, 3)
        off = int.from_bytes(payload[:2], "little")
        sel = payload[2] if len(payload) == 3 else 0
        self.calls.append((off, sel))
        blob = self.files.get(sel, b"")
        chunk = blob[off:off + kr.LOOP_CHUNK]
        return types.SimpleNamespace(payload=off.to_bytes(2, "little") + len(blob).to_bytes(2, "little")
                                     + bytes([len(chunk)]) + chunk)


class Decode(unittest.TestCase):
    def test_fixture_matches_the_cpp_sample(self):
        h = hr.decode_file(fixture_file())
        self.assertEqual(h["state"], "running")
        self.assertEqual(h["boots"], 2)
        self.assertEqual(h["end"], "done")
        self.assertEqual(h["os_guess"], "Linux")
        self.assertEqual(h["fw"], "1.0.1-130-abcdef12")
        self.assertEqual(h["start_ms"], 0x01020304)
        self.assertEqual((h["cc"], h["rp"], h["vbus_mv"], h["dp_mv"], h["dm_mv"]), (2, "3 A", 5070, 3300, -1))
        pd = h["pd"]
        self.assertEqual(pd["rev_raw"], 2)
        self.assertEqual(pd["pdos"], ["0x0801912C", "0x00000000"])
        self.assertEqual(pd["queries"]["dr_swap_back"], {"answer": "ack", "attempts": 1, "skip": "", "t_ms": 0x1234})
        self.assertEqual(pd["queries"]["id_ufp"]["answer"], "not run")
        self.assertEqual(pd["svids"], ["0xFF01"])
        self.assertEqual(pd["mfr_info"], "0000ab")
        self.assertEqual(pd["tx_fails"], 7)
        u = h["usb"]
        self.assertEqual(u["events_total"], 300)
        self.assertTrue(u["sof"])
        self.assertFalse(u["overflow"])
        self.assertEqual(u["events"], [{"t_ms": 12, "kind": "setup", "bmRequestType": 0x80, "bRequest": 6,
                                        "wValue": 0x0100, "wIndex": 0, "wLength": 64}])
        json.dumps(h)

    def test_bad_files(self):
        f = bytearray(fixture_file())
        f[-1] ^= 1
        with self.assertRaises(hr.BadRecord):
            hr.decode_file(bytes(f))
        with self.assertRaises(hr.BadRecord):
            hr.decode_file(b"LPR" + bytes(fixture_file()[3:]))   # the loop probe's own file is not a Host report
        with self.assertRaises(hr.BadRecord):
            hr.decode_file(fixture_file()[:-1])


class Remote(unittest.TestCase):
    def test_host_flag_reads_selector_1(self):
        f = fixture_file()
        link = FakeLink({1: f, 0: b"other"})
        r = kr.cmd_loop_probe(link, types.SimpleNamespace(host=True))
        self.assertEqual(r, {"host_report": hr.decode_file(f)})
        self.assertEqual(link.calls, [(off, 1) for off in range(0, len(f), kr.LOOP_CHUNK)])
        text = kr.render("loop-probe", r)
        self.assertIn("dr_swap_back", text)
        self.assertIn("OS guess Linux (unverified rules)", text)

    def test_nothing_stored(self):
        r = kr.cmd_loop_probe(FakeLink({}), types.SimpleNamespace(host=True))
        self.assertEqual(r, {"host_report": None})
        self.assertIn("no Host report stored", kr.render("loop-probe", r))

    def test_without_flag_reads_the_loop_probe_file(self):
        link = FakeLink({})
        self.assertEqual(kr.cmd_loop_probe(link, types.SimpleNamespace(host=False)), {"loop_probe": None})
        self.assertEqual(link.calls, [(0, 0)])

    def test_cli_parses_host(self):
        self.assertTrue(kr.build_parser().parse_args(["loop-probe", "--host"]).host)
        self.assertFalse(kr.build_parser().parse_args(["loop-probe"]).host)


if __name__ == "__main__":
    unittest.main()
