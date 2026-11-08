# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for tools/ufcs_sweep.py: variant order, the break-only rule, and a sweep against a fake K2."""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import k2_remote as kr
import ufcs_sweep as sw
from test_k2_remote_ufcs import make_blob


def result(rx=(), framing=1, verdict="garbage"):
    events = [{"ms": 130, "dir": "rx", "hex": h} for h in rx]
    return {"verdict": verdict, "class": "delayed", "events": events, "rx_bytes": sum(len(h) // 2 for h in rx),
            "errors": {"framing": framing}}


class Order(unittest.TestCase):
    def test_default_list_size_and_first(self):
        v = sw.variants(sw.DEFAULT_AXES)
        self.assertEqual(len(v), 48)
        self.assertEqual(v[0], [("version", 1), ("opener", 0), ("stop", 1), ("crcsync", 0)])   # the standard first
        self.assertEqual(v[-1], [("version", 0), ("opener", 1), ("stop", 2), ("crcsync", 1)])

    def test_likelier_mixes_first(self):
        v = sw.variants(sw.DEFAULT_AXES)
        ranks = [sum(dict(sw.DEFAULT_AXES)[n].index(x) for n, x in row) for row in v]
        self.assertEqual(ranks, sorted(ranks))

    def test_parse_axis_and_set(self):
        self.assertEqual(sw.parse_axis("baud=115200,0x1c200"), ("baud", [115200, 115200]))
        with self.assertRaises(Exception):
            sw.parse_axis("nope=1")
        with self.assertRaises(Exception):
            sw.parse_set("baud=1,2")


class Rule(unittest.TestCase):
    def test_lone_break_is_not_interesting(self):
        self.assertFalse(sw.interesting(result(["00"])))
        self.assertFalse(sw.interesting(result([])))

    def test_other_bytes_are_interesting(self):
        self.assertTrue(sw.interesting(result(["00"], framing=0)))     # a zero without the framing error
        self.assertTrue(sw.interesting(result(["aa"])))
        self.assertTrue(sw.interesting(result(["00", "00"])))
        self.assertTrue(sw.interesting(result([], verdict="answered")))


class FakeDev:
    """A K2 that answers every probe with the break, except when the tuner has `version` 3 (then 5 RX bytes)."""

    def __init__(self, reboot_at=None):
        self.vals = [f[3] for f in kr.UFCS_FIELDS]
        self.state, self.starts, self.polls, self.uptime = 0, 0, 0, 1000
        self.last = b""
        self.reboot_at = reboot_at
        self.resets = 0

    def call(self, cmd, payload=b""):
        P = type("R", (), {})
        r = P()
        if cmd == kr.CMD_PING:
            self.uptime += 2000 if self.reboot_at != self.starts else -999
            r.payload = bytes([1]) + self.uptime.to_bytes(4, "little") + b"abc1234".ljust(24, b"\0")
            return r
        op = payload[0]
        status = lambda start=0: bytes([self.state, 2, 2, start]) + len(self.last).to_bytes(2, "little") + bytes([17])
        if op == kr.UFCS_RESET:
            self.vals = [f[3] for f in kr.UFCS_FIELDS]
            self.resets += 1
            r.payload = status()
        elif op == kr.UFCS_SET:
            self.vals[payload[1]] = int.from_bytes(payload[2:6], "little")
            _, lo, hi, de = kr.UFCS_FIELDS[payload[1]]
            r.payload = bytes([payload[1]]) + b"".join(v.to_bytes(4, "little") for v in (self.vals[payload[1]], lo, hi, de))
        elif op == kr.UFCS_PROBE:
            self.starts += 1
            if self.starts == 2 and not getattr(self, "_refused", False):    # one "resetting" refusal first
                self._refused = True
                self.starts -= 1
                r.payload = status(5)
                return r
            self.state = 1
            self.polls = 0
            hit = self.vals[1] == 3
            self.last = make_blob([(0, 0, bytes.fromhex("aa20080695"))] + ([(40, 1, b"\xaa\x20\x01\x02\x03")] if hit else [(130, 1, b"\x00")]),
                                  verdict=3, framing=0 if hit else 1)
            r.payload = status(0)
        elif op == kr.UFCS_STATUS:
            if self.state == 1:
                self.polls += 1
                if self.polls >= 2:
                    self.state = 2
            r.payload = status()
        elif op == kr.UFCS_STOP:
            r.payload = status()
        else:
            off = int.from_bytes(payload[1:3], "little")
            chunk = self.last[off:off + 48]
            r.payload = off.to_bytes(2, "little") + len(self.last).to_bytes(2, "little") + bytes([len(chunk)]) + chunk
        return r


class Run(unittest.TestCase):
    def run_sweep(self, dev, axes, baseline=()):
        out = Path(tempfile.mkdtemp()) / "run"
        lines = []
        with mock.patch.object(kr, "cmd_state", lambda link, a: {"vbus_mv": 5000}):
            try:
                rows, hits = sw.sweep(dev, sw.variants(axes), list(baseline), out, sleep=lambda s: None,
                                      clock=iter(range(10 ** 6)).__next__, out=lines.append)
            finally:
                pass
        return out, rows, hits

    def test_hit_is_marked_and_written(self):
        dev = FakeDev()
        out, rows, hits = self.run_sweep(dev, [("version", [1, 2, 3, 0]), ("opener", [6, 1])], [("delay", 40)])
        self.assertEqual(len(rows), 8)
        self.assertEqual(len(hits), 2)                     # version=3 with both openers
        self.assertTrue(all("<<< RX" in r for r in rows if "version=3" in r))
        self.assertFalse(any("<<< RX" in r for r in rows if "version=3" not in r))
        recs = [json.loads(l) for l in (out / "results.jsonl").read_text().splitlines()]
        self.assertEqual(len(recs), 8)
        self.assertEqual(recs[0]["baseline"], {"delay": 40})
        self.assertIn("2 variant(s)", (out / "table.txt").read_text())
        self.assertEqual(dev.vals, [f[3] for f in kr.UFCS_FIELDS])     # tuner left at defaults

    def test_start_refusal_resetting_is_waited_out(self):
        dev = FakeDev()
        out, rows, hits = self.run_sweep(dev, [("version", [1, 2])])
        self.assertEqual(len(rows), 2)

    def test_reboot_aborts(self):
        dev = FakeDev(reboot_at=2)
        with self.assertRaises(sw.SweepAbort):
            self.run_sweep(dev, [("version", [1, 2, 3])])

    def test_inline_refusal_aborts(self):
        class Inline(FakeDev):
            def call(self, cmd, payload=b""):
                r = super().call(cmd, payload)
                if cmd == kr.CMD_UFCS and payload[0] == kr.UFCS_PROBE:
                    r.payload = r.payload[:3] + bytes([2]) + r.payload[4:]
                return r
        with self.assertRaises(sw.SweepAbort):
            self.run_sweep(Inline(), [("version", [1])])


if __name__ == "__main__":
    unittest.main()
