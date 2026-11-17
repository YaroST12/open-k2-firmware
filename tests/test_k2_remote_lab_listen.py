# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote lab listen-attach` (CMD_LAB 0x51, op 2): the request bytes, the reply decode, the polling
until the window ends and the refusal text. The firmware side never transmits; this only checks the host tool."""
import argparse
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


class Reply:
    def __init__(self, payload):
        self.payload = payload


class FakeLink:
    """Answers start with state 1, then status polls with state 1 until `polls` are used, then state 0 / done."""

    def __init__(self, polls=1, refuse=None):
        self.calls, self.polls, self.refuse = [], polls, refuse

    def call(self, cmd, args):
        self.calls.append((cmd, bytes(args)))
        if args[1] == 1 and self.refuse is not None:
            raise kr.RemoteError(kr.STATUS_BUSY, cmd, bytes([self.refuse]))
        if args[1] == 1:
            return Reply(bytes([1, 0, args[2], args[2], 0, 0, args[3]]))
        self.polls -= 1
        if self.polls >= 0:
            return Reply(bytes([1, 0, 10, 3, 2, 0, 2]))
        return Reply(bytes([0, 1, 10, 0, 7, 1, 2]))


def ns(secs=10, port="auto"):
    return argparse.Namespace(lab_cmd="listen-attach", secs=secs, port=port)


class ListenAttach(unittest.TestCase):
    def setUp(self):
        self._sleep = kr.time.sleep
        kr.time.sleep = lambda s: None

    def tearDown(self):
        kr.time.sleep = self._sleep

    def test_start_bytes_and_final_status(self):
        link = FakeLink(polls=2)
        d = kr.cmd_lab(link, ns(secs=12, port="bottom"))
        self.assertEqual(link.calls[0], (kr.CMD_LAB, bytes([2, 1, 12, 2])))
        self.assertTrue(all(c == (kr.CMD_LAB, bytes([2, 2])) for c in link.calls[1:]))
        self.assertEqual(d["state"], "idle")
        self.assertEqual(d["end"], "done")
        self.assertEqual(d["frames"], 7 | 1 << 8)
        self.assertEqual(d["port"], "bottom")

    def test_default_port_is_auto(self):
        link = FakeLink(polls=0)
        kr.cmd_lab(link, ns())
        self.assertEqual(link.calls[0][1], bytes([2, 1, 10, 0]))

    def test_seconds_are_range_checked_before_any_request(self):
        for secs in (0, 31):
            link = FakeLink()
            with self.assertRaises(SystemExit):
                kr.cmd_lab(link, ns(secs=secs))
            self.assertEqual(link.calls, [])

    def test_refusal_is_named(self):
        for code, word in ((2, "busy"), (3, "inline"), (4, "pinned"), (5, "spoof"), (6, "already running")):
            with self.assertRaises(SystemExit) as cm:
                kr.cmd_lab(FakeLink(refuse=code), ns())
            self.assertIn(word, str(cm.exception))

    def test_parser_accepts_the_command(self):
        text = Path(kr.__file__).read_text()
        self.assertIn('"listen-attach"', text)


if __name__ == "__main__":
    unittest.main()
