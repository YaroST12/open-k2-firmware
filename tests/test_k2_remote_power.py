# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote power` (CMD_POWER 0x37). The byte vector is the same literal as tests/test_remote_proto.cpp test_power."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import k2_remote as kr


def rsp(cmd, seq, status, payload=b""):
    return (bytes([0xA6, 0x4B, cmd, seq, status, len(payload)]) + payload).ljust(64, b"\x00")


class FakeTransport:
    def __init__(self, frames):
        self.q, self.sent = list(frames), []

    def write(self, d):
        self.sent.append(bytes(d))

    def read(self, ms):
        return self.q.pop(0) if self.q else b""


PAYLOAD = bytes([1, 2, 30, 100, 30, 0x3C, 0x00, 0x2C, 0x01, 4, 0, 0x21, 1, 1, 0x04, 0x03, 0x02, 0x01])


class Power(unittest.TestCase):
    def test_decode(self):
        r = kr.decode_power(PAYLOAD)
        self.assertEqual((r["state"], r["force"], r["panel"]), ("dim", "dim", "asleep"))
        self.assertEqual((r["brightness_now"], r["brightness"], r["dim_brightness"]), (30, 100, 30))
        self.assertEqual((r["dim_after_s"], r["off_after_s"]), (60, 300))
        self.assertEqual((r["hclk_div"], r["hclk_mhz"], r["force_div"]), (4, 60, 0))
        self.assertEqual(r["blocked_by"], ["load", "hold"])
        self.assertTrue(r["wake_on_remote"])
        self.assertEqual(r["idle_s"], 0x01020304)
        with self.assertRaises(kr.FrameError):
            kr.decode_power(PAYLOAD[:-1])

    def test_set_request_bytes_match_the_firmware_vector(self):
        self.assertEqual(kr.encode_power_set(), b"")
        self.assertEqual(kr.encode_power_set(brightness=70, off_after=0x0E10, force="off", hclk="8"),
                         bytes([0x39, 70, 0x10, 0x0E, 3, 8]))
        self.assertEqual(kr.encode_power_set(dim_brightness=20, dim_after=30, off_after=600, force="auto", hclk="auto", wake=False,
                                             brightness=10),
                         bytes([0x7F, 10, 20, 0x1E, 0x00, 0x58, 0x02, 0, 0, 0]))

    def test_command_sends_nothing_for_a_read_and_renders(self):
        link = kr.RemoteLink(FakeTransport([rsp(0x37, 1, 0, PAYLOAD)]), 1.0)
        a = kr.build_parser().parse_args(["power"])
        r = kr.COMMANDS[a.command](link, a)
        self.assertEqual(link.transport.sent[0][:6], bytes([0xA5, 0x4B, 0x37, 1, 0, 0]))
        text = kr.render("power", r)
        self.assertIn("dim (forced dim)", text)
        self.assertIn("60 MHz (divider 4)", text)
        self.assertIn("load, hold", text)

    def test_command_with_options_sends_the_mask(self):
        link = kr.RemoteLink(FakeTransport([rsp(0x37, 1, 0, PAYLOAD)]), 1.0)
        a = kr.build_parser().parse_args(["power", "--brightness", "40", "--force", "off", "--hclk", "4", "--wake-on-remote", "off"])
        kr.COMMANDS[a.command](link, a)
        sent = link.transport.sent[0]
        self.assertEqual(sent[2:4], bytes([0x37, 1]))
        self.assertEqual(sent[4], 5)
        self.assertEqual(sent[5:10], bytes([0x01 | 0x10 | 0x20 | 0x40, 40, 3, 4, 0]))


if __name__ == "__main__":
    unittest.main()
