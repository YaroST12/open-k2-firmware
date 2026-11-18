# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/eeprom_dump.py against simulated 24Cxx chips behind the firmware's two requests (probe, read): the addressing
classifier, the chunked dump (blocks of a one-byte chip, a smaller chip than asked, the NACK stop), the printed views."""
import os
import pathlib
import sys
import tempfile
import unittest
from types import SimpleNamespace

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import eeprom_dump as e  # noqa: E402
import k2_remote as kr  # noqa: E402


class Chip:
    """What the firmware's probe and read do on the bus, for one chip. abytes 1: blocks of 256 at 0x50+base+n (nblocks of them);
    abytes 2: one device 0x50+base. The probe's 1-byte-address reads move a two-byte chip's pointer like the real part."""

    def __init__(self, data, abytes, base=0, nblocks=8, bus=0):
        self.data, self.abytes, self.base, self.nblocks, self.bus = bytes(data), abytes, base, nblocks, bus
        self.ptr = 0x37       # a two-byte chip's low pointer survives the incomplete address
        self.writes = 0

    def devs(self):
        return range(self.base, self.base + (self.nblocks if self.abytes == 1 else 1))

    def call(self, payload):
        if payload[0] == 0:
            bus = payload[1]
            if bus != self.bus:
                return bytes([bus, 0, 0]) + bytes(16)
            mask = sum(1 << d for d in self.devs())
            dev = min(self.devs())
            wins = b""
            for _ in range(4):
                if self.abytes == 1:
                    wins += self.data[(dev - self.base) * 256:][:4]
                else:
                    wins += bytes(self.data[(self.ptr + i) % len(self.data)] for i in range(4))
                    self.ptr += 4
            return bytes([bus, mask, 0x50 + dev]) + wins
        _, bus, abytes, sel, off, n = payload[0], payload[1], payload[2], payload[3], payload[4] | payload[5] << 8, payload[6]
        assert abytes in (1, 2) and 0 < n <= 32 and sel < 8
        if abytes == 1:
            assert (off & 0xFF) + n <= 256, "chunk crossed a 256-byte block"
        if bus != self.bus or sel + (off >> 8 if abytes == 1 else 0) not in self.devs() or off + n > len(self.data):
            return bytes([1]) + off.to_bytes(2, "little") + bytes([0])
        return bytes([0]) + off.to_bytes(2, "little") + bytes([n]) + self.data[off:off + n]


def pattern(n):
    return bytes((i * 7 + (i >> 8) * 13 + 1) & 0xFF for i in range(n))


class Classify(unittest.TestCase):
    def probe(self, chip):
        return e.probe_buses(chip.call, [0])[0]

    def test_one_byte_chip(self):
        p = self.probe(Chip(pattern(0x800), 1))
        self.assertEqual(p["addr_bytes"], 1)
        self.assertEqual(len(p["ack"]), 8)

    def test_two_byte_chip(self):
        p = self.probe(Chip(pattern(0x1000), 2))
        self.assertEqual(p["addr_bytes"], 2)
        self.assertEqual(p["ack"], ["0x50"])

    def test_small_one_byte_chip_at_a_pin_address(self):
        p = self.probe(Chip(pattern(0x100), 1, base=2, nblocks=1))
        self.assertEqual((p["addr_bytes"], p["ack"]), (1, ["0x52"]))

    def test_nothing_answers(self):
        p = e.probe_buses(Chip(pattern(0x100), 1, bus=1).call, [0])[0]
        self.assertEqual((p["addr_bytes"], p["ack"]), (None, []))

    def test_blank_chip_is_a_guess_or_unknown(self):
        self.assertEqual(self.probe(Chip(b"\xFF" * 0x800, 1))["addr_bytes"], 1)        # all eight addresses answer
        self.assertIsNone(self.probe(Chip(b"\xFF" * 0x1000, 2))["addr_bytes"])         # cannot tell: asks for --addr-bytes

    def test_decode_probe_short(self):
        with self.assertRaises(ValueError):
            e.decode_probe(b"\x00\x01")


class Dump(unittest.TestCase):
    def run_cmd(self, chip, **kw):
        out = tempfile.mktemp(suffix=".bin")
        self.addCleanup(lambda: os.path.exists(out) and os.remove(out))
        a = SimpleNamespace(eeprom_cmd="dump", bus="fusb", out=out, size=0, addr_bytes=None, sel=None)
        for k, v in kw.items():
            setattr(a, k, v)
        r = e.run(chip.call, a)
        return r, pathlib.Path(out).read_bytes()

    def test_one_byte_dump_is_whole_and_byte_exact(self):
        data = pattern(0x800)
        r, got = self.run_cmd(Chip(data, 1))
        self.assertEqual(got, data)
        self.assertEqual((r["addr_bytes"], r["size_read"], r["stopped_at"]), (1, 0x800, None))
        self.assertIn("0400:", r["mid"])

    def test_two_byte_dump(self):
        data = pattern(0x2000)
        r, got = self.run_cmd(Chip(data, 2), size=0x2000)
        self.assertEqual((got, r["addr_bytes"]), (data, 2))

    def test_default_two_byte_size(self):
        r, got = self.run_cmd(Chip(pattern(0x2000), 2))
        self.assertEqual(len(got), 0x1000)

    def test_smaller_chip_stops_at_its_end(self):
        data = pattern(0x200)       # a 24C04: two blocks
        r, got = self.run_cmd(Chip(data, 1, nblocks=2), size=0x800)
        self.assertEqual((got, r["stopped_at"]), (data, 0x200))

    def test_default_size_follows_the_answering_blocks(self):
        r, got = self.run_cmd(Chip(pattern(0x400), 1, nblocks=4))
        self.assertEqual(len(got), 0x400)

    def test_forced_width_and_select(self):
        data = pattern(0x1000)
        r, got = self.run_cmd(Chip(data, 2, base=1), addr_bytes=2, sel=1)
        self.assertEqual((got, r["sel"]), (data, 1))

    def test_blank_two_byte_needs_the_flag(self):
        with self.assertRaises(ValueError):
            self.run_cmd(Chip(b"\xFF" * 0x1000, 2))
        r, got = self.run_cmd(Chip(b"\xFF" * 0x1000, 2), addr_bytes=2)
        self.assertEqual(len(got), 0x1000)

    def test_no_device_is_an_error(self):
        with self.assertRaises(ValueError):
            self.run_cmd(Chip(pattern(0x100), 1, bus=1))

    def test_size_limits(self):
        with self.assertRaises(ValueError):
            e.dump(Chip(pattern(0x800), 1).call, 0, 1, 0, 0x801)
        with self.assertRaises(ValueError):
            e.dump(Chip(pattern(0x800), 1).call, 0, 1, 0, 0)

    def test_reply_that_does_not_match_is_an_error(self):
        with self.assertRaises(ValueError):
            e.read_chunk(lambda p: bytes([0, 9, 0, 4, 1, 2, 3, 4]), 0, 1, 0, 0, 4)


class Views(unittest.TestCase):
    def test_hexdump(self):
        txt = e.hexdump(b"AB\x00" + bytes(range(0x30, 0x3F)), 0x400)
        self.assertTrue(txt.startswith("0400: 41 42 00 30"))
        self.assertTrue(txt.splitlines()[0].endswith("AB.0123456789:;<"))
        self.assertEqual(len(txt.splitlines()), 2)

    def test_render_dump_and_probe(self):
        chip = Chip(pattern(0x800), 1)
        a = SimpleNamespace(eeprom_cmd="probe", bus=None)
        txt = e.render(e.run(chip.call, a))
        self.assertIn("fusb: ack 0x50", txt)
        self.assertIn("ina: no device", txt)
        self.assertIn("one-byte word address", txt)

    def test_cli_wiring(self):
        self.assertEqual(kr.CMD_DEBUG_EEPROM, 0x2B)
        self.assertIn("K2_EEPROM_READ=1", str(kr.RemoteError(kr.STATUS_UNKNOWN_CMD, kr.CMD_DEBUG_EEPROM)))
        self.assertIn("charger test", str(kr.RemoteError(kr.STATUS_BUSY, kr.CMD_DEBUG_EEPROM)))
        a = kr.build_parser().parse_args(["debug", "eeprom", "dump", "--out", "x.bin", "--size", "0x400", "--bus", "ina"]) \
            if hasattr(kr, "build_parser") else None
        if a:
            self.assertEqual((a.eeprom_cmd, a.size, a.bus), ("dump", 0x400, "ina"))


if __name__ == "__main__":
    unittest.main()
