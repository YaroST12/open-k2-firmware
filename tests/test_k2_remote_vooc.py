# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the inline VOOC monitor readout of k2_remote (CMD_FC 0x32, op 4 vooc)."""
import re
import sys
import types
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


def summary(flags=0x0F, ncmds=10, guard=(0, 0, 0, 0)):
    """The firmware's page 0 (include/k2/core/remote_proto.hpp, FC op 4); guard = storms, edges, ms, retry_ms."""
    b = bytearray()
    b += bytes([flags])
    b += (2).to_bytes(2, "little") + (1).to_bytes(2, "little") + (3).to_bytes(2, "little")
    b += (0x01020304).to_bytes(4, "little") + (111).to_bytes(4, "little") + (55).to_bytes(4, "little")
    b += bytes([26, 0xA8]) + (0x00D).to_bytes(2, "little")
    for v in (0x52, 0x4C, 0x7F, 18):
        b += v.to_bytes(2, "little")
    b += (43980).to_bytes(4, "little") + (46230).to_bytes(4, "little") + (16).to_bytes(2, "little")
    b += bytes([ncmds]) + (5).to_bytes(2, "little")
    storms, edges, ms, retry = guard
    b += storms.to_bytes(2, "little") + edges.to_bytes(2, "little") + ms.to_bytes(2, "little")
    b += retry.to_bytes(4, "little")
    return bytes(b)


def rows(items):
    return bytes([len(items)]) + b"".join(bytes([c]) + n.to_bytes(2, "little") + last.to_bytes(2, "little")
                                          for c, n, last in items)


class FakeLink:
    def __init__(self, page0, pages):
        self.page0, self.pages, self.sent = page0, pages, []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_FC
        self.sent.append(bytes(payload))
        page = payload[1]
        return types.SimpleNamespace(payload=self.page0 if page == 0 else self.pages[page - 1])


TABLE = [(0x80 + 2 * i, i + 1, 0x100 + i) for i in range(10)]


class Decode(unittest.TestCase):
    def test_summary_layout(self):
        s = kr.decode_vooc_summary(summary())
        self.assertEqual(s["enabled"], True)
        self.assertEqual(s["running"], True)
        self.assertEqual(s["session_active"], True)
        self.assertEqual(s["handshake"], True)
        self.assertEqual((s["sessions"], s["wakes"], s["partials"]), (2, 1, 3))
        self.assertEqual((s["edges"], s["frames"], s["pairs"], s["hs_pairs"]), (0x01020304, 111, 55, 26))
        self.assertEqual((s["ident"], s["ident_reply"]), ("0xA8", "0x00D"))
        self.assertEqual(s["r82"], {"last": "0x052", "min": "0x04C", "max": "0x07F", "n": 18})
        self.assertEqual(s["p82_us"], {"last": 43980, "mean": 46230, "n": 16})
        self.assertEqual((s["ncmds"], s["other"]), (10, 5))
        self.assertEqual(s["guard"], {"paused": False, "storms": 0, "last_edges": 0, "last_ms": 0, "retry_ms": 0})

    def test_guard_paused(self):
        s = kr.decode_vooc_summary(summary(flags=0x13, guard=(3, 33, 1, 4321)))
        self.assertEqual(s["guard"], {"paused": True, "storms": 3, "last_edges": 33, "last_ms": 1, "retry_ms": 4321})
        text = kr.render_vooc(dict(s, cmds=[]))
        self.assertIn("paused: data traffic", text)
        self.assertIn("storms 3 (last 33 edges in 1 ms)", text)
        self.assertIn("retry in 4.3 s", text)
        quiet = kr.render_vooc(dict(kr.decode_vooc_summary(summary(guard=(2, 4097, 342, 0))), cmds=[]))
        self.assertNotIn("paused", quiet)
        self.assertIn("storms 2 (last 4097 edges in 342 ms)", quiet)
        self.assertNotIn("storms", kr.render_vooc(dict(kr.decode_vooc_summary(summary()), cmds=[])))

    def test_sniff_end_storm(self):
        self.assertEqual(kr.SNIFF_END[6], "storm")

    def test_no_reply_is_none(self):
        b = bytearray(summary(flags=0x03))
        b[21:23] = (0xFFFF).to_bytes(2, "little")      # ident_reply
        b[23:25] = (0xFFFF).to_bytes(2, "little")      # r82_last
        b[25:27] = (0xFFFF).to_bytes(2, "little")      # r82_min
        s = kr.decode_vooc_summary(bytes(b))
        self.assertIsNone(s["ident_reply"])
        self.assertIsNone(s["r82"]["last"])
        self.assertIsNone(s["r82"]["min"])
        self.assertFalse(s["handshake"])

    def test_short_payload_raises(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_vooc_summary(summary()[:40])
        with self.assertRaises(kr.FrameError):
            kr.decode_vooc_rows(bytes([2, 0x80, 1, 0, 0, 1]))

    def test_rows(self):
        self.assertEqual(kr.decode_vooc_rows(rows(TABLE[:2])),
                         [{"cmd": "0x80", "count": 1, "last": "0x100"}, {"cmd": "0x82", "count": 2, "last": "0x101"}])
        self.assertEqual(kr.decode_vooc_rows(rows([(0x9E, 1, 0xFFFF)])), [{"cmd": "0x9E", "count": 1, "last": None}])


class Command(unittest.TestCase):
    def run_cmd(self, link, **kw):
        a = types.SimpleNamespace(fc_cmd="vooc", on=False, off=False, **kw)
        return kr.cmd_fc(link, a)

    def test_reads_all_pages_and_keeps_the_switch(self):
        link = FakeLink(summary(), [rows(TABLE[:8]), rows(TABLE[8:])])
        r = self.run_cmd(link)
        self.assertEqual(link.sent, [bytes([kr.FC_OP_VOOC, 0]), bytes([kr.FC_OP_VOOC, 1]), bytes([kr.FC_OP_VOOC, 2])])
        self.assertEqual(len(r["vooc"]["cmds"]), 10)
        self.assertEqual(r["vooc"]["cmds"][9], {"cmd": "0x92", "count": 10, "last": "0x109"})

    def test_switch_byte_goes_with_page_zero_only(self):
        link = FakeLink(summary(ncmds=0), [rows([]), rows([])])
        a =types.SimpleNamespace(fc_cmd="vooc", on=False, off=True)
        r = kr.cmd_fc(link, a)
        self.assertEqual(link.sent, [bytes([kr.FC_OP_VOOC, 0, 0])])   # no rows: pages 1 / 2 are not read
        self.assertEqual(r["vooc"]["cmds"], [])
        link.sent.clear()
        kr.cmd_fc(link, types.SimpleNamespace(fc_cmd="vooc", on=True, off=False))
        self.assertEqual(link.sent, [bytes([kr.FC_OP_VOOC, 0, 1])])

    def test_render_prints_raw_values_only(self):
        link = FakeLink(summary(), [rows(TABLE[:8]), rows(TABLE[8:])])
        text = kr.render("fc", self.run_cmd(link))
        self.assertIn("vooc monitor on, running", text)
        self.assertIn("0x82", text)
        self.assertIn("0x052", text)
        # No physical unit for a raw reply word: no V or A after a hex value.
        self.assertIsNone(re.search(r"0x[0-9A-F]+\s+(V|A|mV|mA)\b", text))
        self.assertNotRegex(text, r"(?<![0-9A-Fx])\d+(\.\d+)?\s?(V|A|mV|mA)\b")

    def test_cli_parses(self):
        p = kr.build_parser()
        a = p.parse_args(["fc", "vooc", "--off"])
        self.assertEqual((a.fc_cmd, a.on, a.off), ("vooc", False, True))


class Sync(unittest.TestCase):
    def test_op_and_size_match_firmware(self):
        src = (ROOT / "include/k2/core/remote_proto.hpp").read_text()
        self.assertEqual(int(re.search(r"kFcVooc\s*=\s*(\d+)", src).group(1)), kr.FC_OP_VOOC)
        self.assertEqual(int(re.search(r"kVoocSummaryBytes\s*=\s*(\d+)", src).group(1)), kr.VOOC_SUMMARY_BYTES)

    def test_web_table_has_the_op(self):
        js = (ROOT / "tools/web/k2proto.js").read_text()
        self.assertRegex(js, r"FC_OP_VOOC\s*=\s*4\b")


if __name__ == "__main__":
    unittest.main()
