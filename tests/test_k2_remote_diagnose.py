# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote.py diagnose` (CMD_DIAGNOSE 0x72): status, lines in chunks, findings."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr

CHUNK = 53   # the firmware's per-frame text room (58-byte payload minus the 5-byte line header)


class FakeLink:
    """Answers CMD_DIAGNOSE the way include/k2/pd/diag_wire.hpp encodes it; records every request."""

    def __init__(self, state=1, lines=(), findings=()):
        self.state, self.lines, self.findings = state, list(lines), list(findings)
        self.sent = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_DIAGNOSE
        self.sent.append(bytes(payload))
        op = payload[0]
        if op == kr.DG_STATUS:
            p = bytes([self.state, 1, 0, 2, len(self.findings), len(self.lines)]) + struct.pack("<HII", 3, 1000, 42500)
        elif op == kr.DG_LINE:
            i, off = payload[1], payload[2]
            if i >= len(self.lines):
                p = bytes([len(self.lines), 0, 0, 0, 0])
            else:
                kind, sev, text = self.lines[i]
                t = text.encode()
                p = bytes([len(self.lines), kind, sev, 0, len(t)]) + t[off:off + CHUNK]
        else:
            i = payload[1]
            sev, code, name, a = self.findings[i]
            p = bytes([len(self.findings), sev, 0, code]) + struct.pack("<Iiiii", 1234, a, 0, 0, 0) + name.encode()
        return type("R", (), {"payload": p})()


class Diagnose(unittest.TestCase):
    def test_reads_status_lines_and_findings(self):
        long = "Contract 20 V 3 A, draws 0.2 W; " + "x" * 40   # longer than one chunk
        link = FakeLink(lines=[(0, 2, "PD link healthy, power short"), (1, 2, "Laptop needs 65 W, source gives 60 W max"),
                               (2, 2, long)],
                        findings=[(2, 0, "power_short", 65000)])
        r = kr.diagnose_read(link)
        self.assertEqual(r["state"], "open")
        self.assertEqual(r["power"], "power short")
        self.assertEqual(r["link"], "healthy")
        self.assertEqual(r["sessions"], 3)
        self.assertEqual(r["dur_ms"], 42500)
        self.assertEqual([ln["text"] for ln in r["lines"]], [link.lines[0][2], link.lines[1][2], long])
        self.assertEqual(r["lines"][0]["kind"], "verdict")
        self.assertEqual(r["findings"][0]["name"], "power_short")
        self.assertEqual(r["findings"][0]["a"], 65000)
        self.assertEqual(r["findings"][0]["t_ms"], 1234)
        # the long line took two requests, the second from byte 53
        self.assertIn(bytes([kr.DG_LINE, 2, CHUNK]), link.sent)
        text = kr.diagnose_text(r)
        self.assertIn("PD link healthy, power short", text)
        self.assertIn(long, text)

    def test_no_session(self):
        r = kr.diagnose_read(FakeLink(state=0))
        self.assertEqual(r["state"], "none")
        self.assertEqual(kr.diagnose_text(r), "no PD session yet")

    def test_id_matches_firmware(self):
        src = (ROOT / "include/k2/core/remote_proto.hpp").read_text()
        self.assertIn("Diagnose = 0x72", src)
        self.assertEqual(kr.CMD_DIAGNOSE, 0x72)
        self.assertIn("diagnose", kr.COMMANDS)


if __name__ == "__main__":
    unittest.main()
