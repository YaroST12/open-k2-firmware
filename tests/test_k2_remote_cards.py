# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import k2_remote as kr  # noqa: E402

HERE = os.path.dirname(__file__)
PAYLOAD = bytes.fromhex(open(os.path.join(HERE, "fixtures", "report_card_sample.hex")).read().strip())
SAMPLE = json.load(open(os.path.join(HERE, "fixtures", "report_card_sample.json")))


def chunks(payload=PAYLOAD):
    # v4: 162 bytes = 4 chunks of 48 bytes (48*4=192, but we only have 162, so the last chunk is shorter)
    n_chunks = (len(payload) + 47) // 48   # ceiling division
    return [bytes([0, k]) + payload[48 * k:48 * (k + 1)] for k in range(n_chunks)]


class Link:
    def __init__(self, replies):
        self.replies, self.sent = list(replies), []

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, bytes(payload)))
        p = self.replies.pop(0)

        class R:
            pass
        r = R()
        r.payload = p
        return r


def run(argv, replies):
    link = Link(replies)
    a = kr.build_parser().parse_args(argv)
    return kr.COMMANDS[a.command](link, a), link


class Cards(unittest.TestCase):
    def test_command_id(self):
        self.assertEqual(kr.CMD_CARDS, 0x62)

    def test_list(self):
        r, l = run(["cards", "list"], [bytes([0, 1, 1, 3, 0])] + chunks())
        self.assertEqual(r["list"][0], SAMPLE)
        self.assertEqual(l.sent[0], (0x62, bytes([0, 0])))
        n_chunks = len(chunks())
        self.assertEqual(l.sent[1:], [(0x62, bytes([1, 3, 0, k])) for k in range(n_chunks)])
        self.assertIn("Desk 65W", kr.render("cards", r))
        self.assertEqual(kr.render("cards", {"cards": "list", "list": []}), "no report cards")

    def test_show_uses_the_summary_format(self):
        r, _ = run(["cards", "show", "3"], chunks())
        text = kr.render("cards", r)
        self.assertIn("Card 3 · Desk 65W", text)
        self.assertIn("- SCP: ✗ no power monitor", text)
        self.assertIn("- AFC: ✓ up to 9 V, ✗ 12 V (no response)", text)

    def test_export_writes_a_json_array(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "x.json")
            r, _ = run(["cards", "export", "--out", out], [bytes([0, 1, 1, 3, 0])] + chunks())
            self.assertEqual(r["count"], 1)
            self.assertEqual(json.load(open(out)), [SAMPLE])

    def test_rename(self):
        r, l = run(["cards", "rename", "3", "Desk 65W"], [bytes([0, 3, 0])] + chunks())
        self.assertEqual(l.sent[0], (0x62, bytes([2, 3, 0]) + b"Desk 65W"))
        self.assertIn("is now Desk 65W", kr.render("cards", r))
        with self.assertRaises(kr.FcRefused):
            run(["cards", "rename", "3", "x" * 21], [])
        with self.assertRaises(kr.FcRefused):
            run(["cards", "rename", "3", ""], [])

    def test_delete_needs_confirm(self):
        with self.assertRaises(kr.FcRefused):
            run(["cards", "delete", "3"], [])
        r, l = run(["cards", "delete", "3", "--confirm"], [bytes([0])])
        self.assertEqual(l.sent, [(0x62, bytes([3, 3, 0, 1]))])
        self.assertEqual(kr.render("cards", r), "deleted card 3")

    def test_errors(self):
        with self.assertRaises(kr.CardsFailed) as e:
            run(["cards", "delete", "9", "--confirm"], [bytes([2])])
        self.assertIn("NotFound", str(e.exception))
        with self.assertRaises(kr.CardsFailed) as e:
            run(["cards", "delete", "9", "--confirm"], [bytes([5])])
        self.assertIn("charger test", str(e.exception))
        bad = bytearray(PAYLOAD)
        bad[29] = 99
        with self.assertRaises(kr.CardsFailed):
            run(["cards", "show", "3"], chunks(bytes(bad)))


if __name__ == "__main__":
    unittest.main()
