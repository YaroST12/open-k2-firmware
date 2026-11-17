# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote.py mi-cable-auth` (CMD_MI_CABLE_AUTH 0x5C): the read-back, the set payload on the wire, and the
argument checks that refuse a wrong count or an on/off word before anything is sent."""
import os
import sys
import types
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_remote as kr  # noqa: E402


class FakeLink:
    def __init__(self, reply=bytes([1, 7, 0x78, 0x56, 0x34, 0x12, 1, 0, 1])):
        self.reply = reply
        self.calls = []

    def call(self, cmd, payload):
        self.calls.append((cmd, bytes(payload)))
        return types.SimpleNamespace(payload=self.reply)


def run(argv, link):
    a = kr.build_parser().parse_args(["mi-cable-auth"] + argv)
    return kr.COMMANDS[a.command](link, a)


class MiCableAuth(unittest.TestCase):
    def test_read_back(self):
        link = FakeLink()
        r = run([], link)
        self.assertEqual(link.calls, [(kr.CMD_MI_CABLE_AUTH, bytes([0]))])
        self.assertEqual(r, {"on": True, "key": 7, "trailing": "0x12345678", "half": 1, "reply": 0, "payload": 1})

    def test_set_puts_the_fields_on_the_wire(self):
        link = FakeLink()
        run(["on", "7", "0x12345678", "1", "0", "1"], link)
        self.assertEqual(link.calls, [(kr.CMD_MI_CABLE_AUTH, bytes([1, 1, 7, 0x78, 0x56, 0x34, 0x12, 1, 0, 1]))])
        link = FakeLink()
        run(["off", "0", "0", "0", "1", "0"], link)
        self.assertEqual(link.calls[0][1], bytes([1, 0, 0, 0, 0, 0, 0, 0, 1, 0]))

    def test_a_wrong_argument_count_is_refused_before_any_call(self):
        for argv in (["on"], ["on", "7", "0", "0", "0"], ["on", "7", "0", "0", "0", "0", "0"]):
            link = FakeLink()
            with self.assertRaises(kr.FrameError, msg=str(argv)):
                run(argv, link)
            self.assertEqual(link.calls, [], argv)

    def test_the_switch_word_is_on_or_off_only(self):
        link = FakeLink()
        with self.assertRaises(kr.FrameError):
            run(["maybe", "7", "0", "0", "0", "0"], link)
        self.assertEqual(link.calls, [])


if __name__ == "__main__":
    unittest.main()
