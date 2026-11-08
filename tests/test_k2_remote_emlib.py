# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import k2_remote as kr  # noqa: E402


def get_rsp(seq, name, vid, pid):
    return (bytes([0]) + seq.to_bytes(2, "little") + name.encode().ljust(24, b"\0")
            + vid.to_bytes(2, "little") + pid.to_bytes(2, "little"))


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


class EmLib(unittest.TestCase):
    def test_list(self):
        r, l = run(["emarker", "list"], [bytes([0, 2, 2, 5, 0, 9, 0]),
                                         get_rsp(5, "Apple-USB2-60W", 0x5AC, 0x1234),
                                         get_rsp(9, "x", 1, 2)])
        self.assertEqual([c["seq"] for c in r["cables"]], [5, 9])
        self.assertEqual(l.sent[0], (kr.CMD_EM_LIB, bytes([0, 0])))
        self.assertIn("Apple-USB2-60W", kr.render("emarker", r))
        self.assertEqual(kr.render("emarker", {"emlib": "list", "cables": []}), "no saved cables")

    def test_save_and_errors(self):
        r, _ = run(["emarker", "save"], [bytes([0, 3, 0, 0]), get_rsp(3, "n", 1, 2)])
        self.assertIn("saved as n", kr.render("emarker", r))
        with self.assertRaises(kr.EmLibFailed):
            run(["emarker", "save"], [bytes([1, 0, 0, 0])])

    def test_delete_needs_confirm(self):
        with self.assertRaises(kr.FcRefused):
            run(["emarker", "delete", "7"], [])
        r, l = run(["emarker", "delete", "7", "--confirm"], [bytes([0])])
        self.assertEqual(l.sent[0][1], bytes([3, 7, 0, 1]))
        self.assertEqual(kr.render("emarker", r), "deleted id 7")

    def test_rename(self):
        r, l = run(["emarker", "rename", "7", "My cable"], [bytes([0, 7, 0]), get_rsp(7, "My cable", 0, 0)])
        self.assertEqual(l.sent[0][1], bytes([4, 7, 0]) + b"My cable")
        self.assertIn("My cable", kr.render("emarker", r))
        with self.assertRaises(kr.FcRefused):
            run(["emarker", "rename", "7", ""], [])

    def test_spoof(self):
        st = bytes([0, 1, 3, 0, 2, 0, 7, 0])
        r, l = run(["emarker", "spoof", "7"], [st])
        self.assertEqual(l.sent[0][1], bytes([5, 1, 7, 0]))
        self.assertTrue(r["active"])
        self.assertIn("ARMED", kr.render("emarker", r))
        r, l = run(["emarker", "spoof", "off"], [bytes([0, 0, 0, 0, 0, 0, 0, 0])])
        self.assertEqual(l.sent[0][1], bytes([5, 2]))
        with self.assertRaises(kr.EmLibFailed):
            run(["emarker", "spoof", "7"], [bytes([6, 0, 0, 0, 0, 0, 0, 0])])
        with self.assertRaises(kr.FcRefused):
            run(["emarker", "spoof", "bogus"], [])

    def test_spoof_a_builtin_by_id_or_name(self):
        st = bytes([0, 1, 3, 0, 2, 0, 0, 0])
        for target, ident in (("builtin:3", 3), ("builtin:oneplus", 4), ("oneplus-mag", 4), ("oneplus", 4), ("cuktech-6a", 2),
                              ("cuktech-240w", 2), ("generic-epr", 1), ("epr-50v", 1), ("generic", 0), ("builtin:0", 0)):
            r, l = run(["emarker", "spoof", target], [st])
            self.assertEqual(l.sent[0][1], bytes([5, 3, ident]), target)
        for bad in ("builtin:5", "builtin:", "builtin:nope"):
            with self.assertRaises(kr.FcRefused):
                run(["emarker", "spoof", bad], [])

    def test_bare_emarker_still_data(self):
        a = kr.build_parser().parse_args(["emarker"])
        self.assertIsNone(a.em_cmd)


def summary(outcome=1, entries=2, rows=2):
    # [err, outcome, svids, probes, silent, flags, nlog, rows, policy, vid u16, pid u16]
    return bytes([0, outcome, 1, 4, 0, 0, entries, rows, 0]) + (0x22D9).to_bytes(2, "little") + (0x6116).to_bytes(2, "little")


def entry(i, step, req, rep, header=0x0F | (2 << 6) | (1 << 8)):
    out = bytes([0, i, step, 2, len(req), 1 if rep else 0, len(rep)]) + header.to_bytes(2, "little")
    return out + b"".join(w.to_bytes(4, "little") for w in req + rep)


class Learn(unittest.TestCase):
    def test_seed_text(self):
        self.assertEqual(kr.parse_seed("22D9:31"), (0x22D9, 31, []))
        self.assertEqual(kr.parse_seed("1234:17:0xAABBCCDD:1"), (0x1234, 17, [0xAABBCCDD, 1]))
        for bad in ("22D9", "22D9:32", "zz:1", "22D9:1:1:2:3", "10000:1"):
            with self.assertRaises(kr.FcRefused):
                kr.parse_seed(bad)

    def test_options_bytes(self):
        self.assertEqual(kr.encode_learn_options([], False, False), bytes([0, 0]))
        self.assertEqual(kr.encode_learn_options([(0x22D9, 31, [])], True, False), bytes([1, 1, 0xD9, 0x22, 31, 0]))
        self.assertEqual(kr.encode_learn_options([(0x1234, 17, [0x12345678])], False, True),
                         bytes([2, 1, 0x34, 0x12, 17, 1, 0x78, 0x56, 0x34, 0x12]))
        with self.assertRaises(kr.FcRefused):
            kr.encode_learn_options([(1, 1, [])] * 5)

    def test_run_reads_summary_then_every_entry(self):
        e0 = entry(0, 0, [0xFF00A001], [0xFF00A041, 0x6000203C, 0, 0x00016116, 0x11082040])
        e1 = entry(1, 3, [0x22D9A01F], [0x22D9A05F, 0xAB01CD02])
        r, l = run(["emarker", "learn", "--seed", "22D9:31", "--save"],
                   [summary(), e0, e1, bytes([0, 4, 0, 0])])
        self.assertEqual(l.sent[0], (kr.CMD_EM_LIB, bytes([6, 1, 0, 1, 0xD9, 0x22, 31, 0])))
        self.assertEqual(l.sent[1][1], bytes([6, 2, 0]))
        self.assertEqual(l.sent[2][1], bytes([6, 2, 1]))
        self.assertEqual(l.sent[3][1], bytes([6, 3]))
        self.assertEqual(r["summary"]["outcome"], "learned")
        self.assertEqual(r["summary"]["pid"], 0x6116)
        self.assertEqual(r["entries"][1]["reply"], [0x22D9A05F, 0xAB01CD02])
        self.assertEqual(r["saved"], {"seq": 4, "existed": False})
        text = kr.render("emarker", r)
        self.assertIn("learn learned", text)
        self.assertIn("ACK", text)
        self.assertIn("0xAB01CD02", text)
        self.assertIn("saved as id 4", text)

    def test_busy_and_nothing_to_save(self):
        with self.assertRaises(kr.EmLibFailed):
            run(["emarker", "learn"], [bytes([7])])
        with self.assertRaises(kr.FcRefused):
            run(["emarker", "learn", "--save"], [summary(outcome=2, entries=0, rows=0)])

    def test_silent_probe_and_control_reply_render(self):
        silent = entry(0, 4, [0x22D9A010], [])
        ctl = entry(1, 3, [0x22D9A01F], [], header=16)
        ctl = ctl[:5] + bytes([1]) + ctl[6:]   # got = 1, no data objects: a Not_Supported
        r, _ = run(["emarker", "learned"], [summary(entries=2), silent, ctl])
        text = kr.render("emarker", r)
        self.assertIn("(silent)", text)
        self.assertIn("control type 16", text)

    def test_learned_before_any_run(self):
        r, l = run(["emarker", "learned"], [bytes(13)])
        self.assertEqual(l.sent[0][1], bytes([6, 0]))
        self.assertEqual(kr.render("emarker", r), "no learn run yet")

    def test_unmatched(self):
        totals = bytes([0, 5, 0, 2])
        e0 = bytes([0, 5, 0, 2, 2, 1]) + (0x148F).to_bytes(2, "little") + (0x22D9A011).to_bytes(4, "little")
        e1 = bytes([0, 5, 0, 2, 1, 2]) + (0x128F).to_bytes(2, "little") + (0x22D9A01F).to_bytes(4, "little") + (2).to_bytes(4, "little")
        r, l = run(["emarker", "unmatched"], [totals, e0, e1])
        self.assertEqual([x[1] for x in l.sent], [bytes([7, 255]), bytes([7, 0]), bytes([7, 1])])
        self.assertEqual(r["total"], 5)
        self.assertEqual(r["entries"][0]["sop"], "SOP''")
        self.assertEqual(r["entries"][1]["words"], [0x22D9A01F, 2])
        text = kr.render("emarker", r)
        self.assertIn("5 seen", text)
        self.assertIn("0x22D9A01F", text)


if __name__ == "__main__":
    unittest.main()
