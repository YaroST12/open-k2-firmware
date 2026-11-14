# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote memory` (CMD_MEMORY 0x36). The byte vector is the same literal as tests/test_remote_proto.cpp."""
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


PAYLOAD = bytes([
    0x00, 0x80, 0x01, 0x00, 0x00, 0x10, 0x01, 0x00, 0xC0, 0x2A, 0x00, 0x00, 0x10, 0x2A, 0x00, 0x00,
    0xC0, 0x14, 0x00, 0x00, 0x40, 0x05, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00,
    0x02, 0x01, 0x00, 0x00, 0x00, 0x50, 0x0A, 0x00, 0x34, 0x12, 0x0B, 0x00, 0x00, 0x08, 0x03])


def payload_with(low, flags=3, used=0x102):
    p = bytearray(PAYLOAD)
    p[16:20] = low.to_bytes(4, "little")
    p[32:36] = used.to_bytes(4, "little")   # fs_used, of 146 blocks
    p[46] = flags
    return bytes(p)


class Memory(unittest.TestCase):
    def test_decode(self):
        r = kr.decode_memory(PAYLOAD)
        self.assertEqual((r["ram_total"], r["ram_static"], r["psp_size"], r["psp_now"], r["psp_low"]),
                         (98304, 0x11000, 10944, 0x2A10, 5312))
        self.assertEqual((r["msp_size"], r["msp_low"], r["fs_blocks"], r["fs_used"]), (1344, 0x201, 146, 0x102))
        self.assertEqual((r["image_bytes"], r["flash_bytes"], r["fs_block_bytes"]), (0xA5000, 0xB1234, 2048))
        self.assertTrue(r["fs_valid"] and r["ok"])
        self.assertEqual(r["heap"], "none")
        self.assertEqual(r["fs_free"], 0)          # 0x102 used is more than the 146 blocks: clamped, never negative
        self.assertEqual(r["image_free"], 0xB1234 - 0xA5000)

    def test_levels(self):
        # the file storage only (k2::memstat kFsCautionPct / kFsErrorPct): free under 20 % / 10 % of the 146 blocks
        self.assertEqual((kr.MEM_FS_CAUTION_PCT, kr.MEM_FS_ERROR_PCT), (20, 10))
        self.assertEqual(kr.decode_memory(payload_with(7400, used=146 - 30))["level"], "ok")
        self.assertEqual(kr.decode_memory(payload_with(7400, used=146 - 29))["level"], "caution")
        self.assertEqual(kr.decode_memory(payload_with(7400, used=146 - 15))["level"], "caution")
        self.assertEqual(kr.decode_memory(payload_with(7400, used=146 - 14))["level"], "error")
        self.assertEqual(kr.decode_memory(payload_with(100, flags=0))["level"], "unknown")
        self.assertEqual(kr.decode_memory(payload_with(7400, flags=2))["level"], "ok")   # the volume not walked yet
        # the stack is a plain number however low (user decision 2026-11-14)
        self.assertEqual(kr.decode_memory(payload_with(10, used=40))["level"], "ok")

    def test_fs_not_measured(self):
        r = kr.decode_memory(payload_with(7500, flags=2))
        self.assertFalse(r["fs_valid"])
        self.assertIsNone(r["fs_free"])
        self.assertIn("not measured", kr.render("memory", r))

    def test_short_payload(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_memory(PAYLOAD[:-1])

    def test_command_and_render(self):
        link = kr.RemoteLink(FakeTransport([rsp(0x36, 1, 0, payload_with(7400, used=40))]))
        a = kr.build_parser().parse_args(["memory"])
        r = kr.COMMANDS[a.command](link, a)
        self.assertEqual(link.transport.sent[0][:5], bytes([0xA5, 0x4B, 0x36, 1, 0]))
        text = kr.render("memory", r)
        self.assertIn("7400 B free", text)
        self.assertIn("none (static memory only)", text)
        self.assertIn("firmware image", text)
        self.assertNotIn("ERROR", text)

    def test_render_marks_the_storage_level_never_the_stack(self):
        text = kr.render("memory", kr.decode_memory(PAYLOAD))   # stack low 5312 B, the volume full
        self.assertIn("(ERROR)", next(l for l in text.splitlines() if l.startswith("files free")))
        self.assertNotIn("(", next(l for l in text.splitlines() if l.startswith("stack low")))
        text = kr.render("memory", kr.decode_memory(payload_with(7400, used=146 - 20)))
        self.assertIn("(CAUTION)", text)
        self.assertIn("storage low: delete cards", text)


if __name__ == "__main__":
    unittest.main()
