# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the `lab sniff arm` refusal text of k2_remote (CMD_LAB 0x51, op 1): the firmware sends the reason
of a refused arm as one byte (remote::SniffRefusal), and the shared RAM held by the USB drive is not "busy"."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


class SniffRefusalText(unittest.TestCase):
    def test_no_ram_names_the_usb_drive(self):
        text = str(kr.RemoteError(kr.STATUS_BUSY, kr.CMD_LAB, bytes([2])))
        self.assertIn("no RAM: USB drive enabled", text)
        self.assertNotIn("busy", text)

    def test_ports_owner_stays_busy(self):
        text = str(kr.RemoteError(kr.STATUS_BUSY, kr.CMD_LAB, bytes([1])))
        self.assertIn("busy", text)
        self.assertIn("a trigger, scan or test owns D+/D-", text)

    def test_old_firmware_without_reason_is_plain_busy(self):
        self.assertEqual(str(kr.RemoteError(kr.STATUS_BUSY, kr.CMD_LAB)), f"command {kr.CMD_LAB} failed: busy")


if __name__ == "__main__":
    unittest.main()
