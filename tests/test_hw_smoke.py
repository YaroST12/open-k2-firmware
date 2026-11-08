# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for the pure checks and the safety allowlist of tools/hw_smoke.py (no device)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import hw_smoke as s
import k2_remote as kr


class Logs(unittest.TestCase):
    HB = "[SYS] Heartbeat: up=2s presses=0 page=0\n" * 4

    def test_good(self):
        self.assertEqual(s.check_logs(self.HB), [])

    def test_fault(self):
        errs = s.check_logs(self.HB + "[FAULT] HF PC=08004404\n")
        self.assertEqual(len(errs), 1)

    def test_no_heartbeat(self):
        self.assertTrue(any("heartbeat" in e for e in s.check_logs("[PWR] chip=INA226\n")))


class State(unittest.TestCase):
    ST = {"pm_online": True, "chip": "INA226", "mcu_valid": True, "mcu_temp_c": 41.5}
    ADC = {"ok": True, "scan": {"vref": 1500}}   # 4095*1200/1500 = 3276 mV

    def test_vdd(self):
        self.assertEqual(s.vdd_mv(1500), 3276)
        self.assertEqual(s.vdd_mv(0), 0)

    def test_good(self):
        self.assertEqual(s.check_state(self.ST, self.ADC)[0], [])

    def test_bad_values(self):
        errs, _ = s.check_state({**self.ST, "chip": "none", "mcu_temp_c": 95}, {"ok": True, "scan": {"vref": 1000}})
        self.assertEqual(len(errs), 3)


class Png(unittest.TestCase):
    def png(self, pixels, w, h):
        return kr.png_bytes(b"".join(bytes(p) for p in pixels), w, h)

    def test_blank_is_one_colour(self):
        self.assertEqual(s.colour_count(self.png([(0, 0, 0)] * 16, 4, 4)), 1)

    def test_two_colour(self):
        self.assertEqual(s.colour_count(self.png([(0, 0, 0), (255, 255, 255)] * 8, 4, 4)), 2)

    def test_real_ui_has_more(self):
        px = [(i * 10 % 256, 0, 0) for i in range(16)]
        self.assertGreater(s.colour_count(self.png(px, 4, 4)), 2)

    def test_filtered_png(self):                                  # Sub filter (1), as Pillow writes
        import struct, zlib
        raw = b"\x01" + bytes([10, 20, 30, 1, 1, 1]) + b"\x01" + bytes([0, 0, 0, 5, 5, 5])
        def chunk(t, b):
            c = t + b
            return struct.pack(">I", len(b)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 2, 2, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
        _, _, rows, _ = s.read_png(data)
        self.assertEqual(rows, [bytes([10, 20, 30, 11, 21, 31]), bytes([0, 0, 0, 5, 5, 5])])


class Git(unittest.TestCase):
    def test_match(self):
        self.assertEqual(s.git_matches("abc1234", "abc1234"), (True, False))
        self.assertEqual(s.git_matches("abc1234-dirty", "abc1234"), (True, True))
        self.assertEqual(s.git_matches("def5678", "abc1234")[0], False)
        self.assertEqual(s.git_matches("unknown", "abc1234")[0], False)


class Safety(unittest.TestCase):
    def test_dangerous_remote_commands_refused(self):
        for cmd in ("fc", "dfu", "fault", "reboot", "action", "emarker", "ripple", "press", "debug", "log"):
            with self.assertRaises(s.SmokeError, msg=cmd):
                s.remote(cmd)

    def test_ok_button_refused(self):
        with self.assertRaises(s.SmokeError):
            s.press("ok")

    def test_fake_pdo_range(self):
        with self.assertRaises(s.SmokeError):
            s.debug_fake_pdos(8)

    def test_pages_cover_the_enum(self):
        self.assertEqual(s.page_names()[:3], ["home", "detail", "chargertest"])
        self.assertNotIn("count", s.page_names())


if __name__ == "__main__":
    unittest.main()
