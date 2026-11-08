# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the UFCS tuner / probe part of k2_remote (CMD_UFCS 0x52)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


class FakeLink:
    """Answers CMD_UFCS from a canned blob and a RAM field array; records every request."""

    def __init__(self, blob=b""):
        self.blob = blob
        self.vals = [f[3] for f in kr.UFCS_FIELDS]
        self.sent = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_UFCS
        self.sent.append(bytes(payload))
        op = payload[0]
        if op in (kr.UFCS_STATUS, kr.UFCS_RESET, kr.UFCS_PROBE, kr.UFCS_STOP):
            return type("R", (), {"payload": bytes([2, 4, 2, 0]) + len(self.blob).to_bytes(2, "little") + bytes([len(self.vals)])})()
        if op in (kr.UFCS_GET, kr.UFCS_SET):
            i = payload[1]
            if op == kr.UFCS_SET:
                self.vals[i] = int.from_bytes(payload[2:6], "little")
            _, lo, hi, de = kr.UFCS_FIELDS[i]
            return type("R", (), {"payload": bytes([i]) + b"".join(v.to_bytes(4, "little") for v in (self.vals[i], lo, hi, de))})()
        off = int.from_bytes(payload[1:3], "little")
        chunk = self.blob[off:off + kr.UFCS_CHUNK]
        return type("R", (), {"payload": off.to_bytes(2, "little") + len(self.blob).to_bytes(2, "little") + bytes([len(chunk)]) + chunk})()


def make_blob(events, **kw):
    log = b"".join(ms.to_bytes(2, "little") + bytes([k, len(d)]) + d for ms, k, d in events)
    h = bytearray(kr.UFCS_BLOB_HEADER)
    h[0], h[1], h[2], h[3] = 1, 2, kw.get("verdict", 3), 2
    h[4:6] = (989).to_bytes(2, "little", signed=True)
    h[14], h[15] = kw.get("tx", 5), 1
    h[16:18] = (sum(len(d) for _, k, d in events if k == 1)).to_bytes(2, "little")
    h[18:20] = kw.get("framing", 1).to_bytes(2, "little")
    h[26:28] = (5000).to_bytes(2, "little")
    h[34:36] = len(log).to_bytes(2, "little")
    return bytes(h) + log


class Sync(unittest.TestCase):
    def test_fields_match_firmware_table(self):
        src = (ROOT / "include/k2/fastcharge/ufcs_tune.hpp").read_text()
        rows = re.findall(r'\{"(\w+)",\s*(\d+),\s*(\d+),\s*(0x[0-9A-Fa-f]+|\d+)\}', src)
        fw = [(n, int(lo), int(hi), int(d, 0)) for n, lo, hi, d in rows]
        self.assertEqual(fw, kr.UFCS_FIELDS)


class Decode(unittest.TestCase):
    def test_blob_header_and_events(self):
        b = make_blob([(0, 0, bytes.fromhex("aa200806 95".replace(" ", ""))), (130, 1, b"\x00")])
        r = kr.decode_ufcs_blob(b)
        self.assertEqual((r["verdict"], r["class"], r["hs_us"]["rise1"]), ("garbage", "delayed", 989))
        self.assertEqual(r["errors"]["framing"], 1)
        self.assertEqual(r["events"], [{"ms": 0, "dir": "tx", "hex": "aa20080695"}, {"ms": 130, "dir": "rx", "hex": "00"}])

    def test_truncated_log_stops_at_a_whole_event(self):
        b = make_blob([(0, 0, b"\xaa\x20")])
        self.assertEqual(kr.decode_ufcs_blob(b[:-1])["events"], [])

    def test_short_blob_raises(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_ufcs_blob(b"\x01\x02")


class Calls(unittest.TestCase):
    def test_set_sends_field_index_and_le_value(self):
        link = FakeLink()
        r = kr.ufcs_set(link, "baud", 57600)
        self.assertEqual(link.sent[0], bytes([kr.UFCS_SET, 13]) + (57600).to_bytes(4, "little"))
        self.assertEqual((r["field"], r["value"], r["default"]), ("baud", 57600, 115200))

    def test_unknown_field_exits(self):
        with self.assertRaises(SystemExit):
            kr.ufcs_set(FakeLink(), "nope", 1)

    def test_blob_is_read_in_chunks(self):
        blob = make_blob([(i, 1, bytes([i % 256] * 7)) for i in range(20)])
        link = FakeLink(blob)
        self.assertGreater(len(blob), kr.UFCS_CHUNK * 2)
        self.assertEqual(kr.ufcs_read_blob(link), blob)
        self.assertEqual(len(link.sent), -(-len(blob) // kr.UFCS_CHUNK))

    def test_status_names(self):
        s = kr.ufcs_status(FakeLink(b"x" * 10))
        self.assertEqual((s["state"], s["verdict"], s["class"], s["start"], s["blob_len"]), ("done", "answered", "delayed", "ok", 10))

    def test_render_result_lists_events(self):
        r = kr.decode_ufcs_blob(make_blob([(0, 0, b"\xaa"), (5, 1, b"\x00")]))
        text = kr.render_ufcs({"ufcs_result": r})
        self.assertIn("tx aa", text)
        self.assertIn("rx 00", text)


if __name__ == "__main__":
    unittest.main()
