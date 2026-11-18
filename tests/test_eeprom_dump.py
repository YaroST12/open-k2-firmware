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


BAD = bytes.fromhex("0060F0FFCFFEFFFFA24EFBFF")


class CalChip(Chip):
    """A 24C16 on the INA bus plus the firmware's op 2: compare-and-swap of 0x04..0x0F with zeros. err/after_override make
    the firmware report a failed write; the chip content is only changed when the compare passes and fail is None."""

    def __init__(self, data, fail=None):
        super().__init__(data, 1, bus=1)
        self.fail, self.cal_calls, self.cal_payloads = fail, 0, []

    def call(self, payload):
        if payload[0] != 2:
            return super().call(payload)
        self.cal_calls += 1
        self.cal_payloads.append(bytes(payload))
        assert len(payload) == 13
        before = self.data[4:16]
        if before != payload[1:]:
            return bytes([1, 1]) + before + bytes(12)
        if self.fail == "nack":
            return bytes([3, 3]) + before + before
        if self.fail == "verify":
            self.data = self.data[:4] + b"\x01" + self.data[5:]
        else:
            self.data = self.data[:4] + bytes(12) + self.data[16:]
        err = 5 if self.fail == "verify" else 0
        return bytes([err, 3]) + before + self.data[4:16]


def cal_args(expect=BAD, yes=True):
    return SimpleNamespace(eeprom_cmd="cal-clear", expect=expect, yes=yes)


class CalClear(unittest.TestCase):
    def chip(self, **kw):
        return CalChip(bytes(range(1, 5)) + BAD + bytes(range(0x20, 0x30)) + bytes(0x700), **kw)

    def test_expect_parsing(self):
        self.assertEqual(e.expect_bytes("0060F0FFCFFEFFFFA24EFBFF"), BAD)
        self.assertEqual(e.expect_bytes("0x00 60 f0 ff cf fe ff ff a2 4e fb ff"), BAD)
        for bad in ("", "00", BAD.hex()[:-2], BAD.hex() + "00", "zz" + BAD.hex()[2:]):
            with self.assertRaises(ValueError):
                e.expect_bytes(bad)

    def test_needs_yes_and_sends_nothing(self):
        c = self.chip()
        with self.assertRaises(ValueError):
            e.run(c.call, cal_args(yes=False))
        self.assertEqual(c.cal_calls, 0)
        self.assertEqual(c.data[4:16], BAD)

    def test_repair_clears_only_the_trims(self):
        c = self.chip()
        before = bytes(c.data)
        r = e.run(c.call, cal_args())
        self.assertEqual(c.cal_calls, 1)
        self.assertEqual(c.cal_payloads[0], bytes([2]) + BAD)
        self.assertEqual(c.data[4:16], bytes(12))
        self.assertEqual(c.data[:4] + c.data[16:], before[:4] + before[16:])
        self.assertEqual((r["head_before"][8:], r["head_after"][8:]), (BAD.hex().upper(), "00" * 12))
        txt = e.render(r)
        self.assertIn(BAD.hex().upper(), txt)
        self.assertIn("after", txt)

    def test_host_refuses_a_mismatch_without_calling_the_write_op(self):
        c = self.chip()
        other = bytes(12)
        with self.assertRaises(ValueError) as cm:
            e.run(c.call, cal_args(expect=other))
        self.assertIn("nothing written", str(cm.exception))
        self.assertEqual(c.cal_calls, 0)

    def test_firmware_refusal_is_an_error_with_what_it_saw(self):
        c = self.chip()
        # the chip changes between the host read and the write op: only the firmware compare can notice
        orig = c.call

        def racy(p):
            if p[0] == 2:
                c.data = c.data[:4] + b"\x55" + c.data[5:]
            return orig(p)
        with self.assertRaises(ValueError) as cm:
            e.run(racy, cal_args())
        self.assertIn("mismatch", str(cm.exception))
        self.assertEqual(c.data[4], 0x55)

    def test_firmware_write_failures_are_errors(self):
        for fail, word in (("nack", "NACK"), ("verify", "not all zero")):
            c = self.chip(fail=fail)
            with self.assertRaises(ValueError) as cm:
                e.run(c.call, cal_args())
            self.assertIn(word, str(cm.exception))

    def test_no_chip_is_an_error(self):
        c = Chip(bytes(16), 1, bus=0)
        with self.assertRaises(ValueError):
            e.run(c.call, cal_args())

    def test_reply_length_is_checked(self):
        with self.assertRaises(ValueError):
            e.decode_cal(bytes(5))

    def test_cli_wiring(self):
        if not hasattr(kr, "build_parser"):
            self.skipTest("no build_parser")
        p = kr.build_parser()
        a = p.parse_args(["debug", "eeprom", "cal-clear", "--expect", BAD.hex(), "--yes"])
        self.assertEqual((a.eeprom_cmd, a.expect, a.yes), ("cal-clear", BAD, True))
        with self.assertRaises(SystemExit):
            p.parse_args(["debug", "eeprom", "cal-clear", "--yes"])           # --expect is required
        self.assertFalse(p.parse_args(["debug", "eeprom", "cal-clear", "--expect", BAD.hex()]).yes)


class PageChip(Chip):
    """A 24C16 on the INA bus that also takes op 3 (compare-and-swap of one page), like the firmware's page_put."""

    def __init__(self, data, **kw):
        super().__init__(data, 1, bus=1, **kw)
        self.data = bytearray(self.data)
        self.page_calls, self.meddle = [], None

    def call(self, payload):
        if payload[0] != 3:
            return super().call(payload)
        off = payload[1] | payload[2] << 8
        exp, new = bytes(payload[3:19]), bytes(payload[19:35])
        self.page_calls.append(off)
        if self.meddle is not None and off == self.meddle:      # someone wrote the page after the host read it
            self.data[off] ^= 0xFF
        before = bytes(self.data[off:off + 16])
        if before != exp:
            return bytes([2, 1]) + before + bytes(16)
        if before == new:
            return bytes([1, 3]) + before + before
        self.data[off:off + 16] = new
        return bytes([0, 3]) + before + new


def restore_args(path, **kw):
    a = dict(eeprom_cmd="restore", input=path, yes=True, allow_bad_cal=False, backup=None, bus=None)
    a.update(kw)
    return SimpleNamespace(**a)


class Restore(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def image(self, data, name="img.bin"):
        p = os.path.join(self.tmp.name, name)
        with open(p, "wb") as f:
            f.write(bytes(data))
        return p

    def healthy(self):
        d = bytearray(pattern(0x800))
        d[4:16] = bytes(12)
        return d

    def test_writes_only_differing_pages_and_verifies(self):
        img = self.healthy()
        chip_data = bytearray(img)
        chip_data[0x512] ^= 0x55                                # two pages differ: 0x510 and 0x7F0
        chip_data[0x7FF] = 0xA5 if img[0x7FF] != 0xA5 else 0x00
        c = PageChip(chip_data)
        backup = os.path.join(self.tmp.name, "before.bin")
        r = e.run(c.call, restore_args(self.image(img), backup=backup))
        self.assertEqual(c.page_calls, [0x510, 0x7F0])
        self.assertEqual(bytes(c.data), bytes(img))
        self.assertTrue(r["verified"])
        self.assertEqual(pathlib.Path(backup).read_bytes(), bytes(chip_data))
        self.assertIn("wrote 2 page(s)", e.render(r))
        r = e.run(c.call, restore_args(self.image(img)))           # again: nothing to write
        self.assertEqual(c.page_calls, [0x510, 0x7F0])
        self.assertEqual(r["pages"], [])

    def test_dry_run_writes_nothing(self):
        img = self.healthy()
        chip_data = bytearray(img)
        chip_data[0x100] ^= 1
        c = PageChip(chip_data)
        r = e.run(c.call, restore_args(self.image(img), yes=False))
        self.assertEqual(c.page_calls, [])
        self.assertEqual(r["pages"], ["0x100"])
        self.assertIn("--yes", e.render(r))

    def test_bad_calibration_is_refused_unless_allowed(self):
        img = self.healthy()
        img[4:16] = BAD
        c = PageChip(self.healthy())
        with self.assertRaises(ValueError) as cm:
            e.run(c.call, restore_args(self.image(img)))
        self.assertIn("calibration", str(cm.exception))
        self.assertEqual(c.page_calls, [])
        e.run(c.call, restore_args(self.image(img), allow_bad_cal=True))
        self.assertEqual(bytes(c.data[4:16]), BAD)

    def test_blank_trims_are_not_suspect(self):
        self.assertEqual(e.cal_suspect(b"\xff" * 0x800), [])
        self.assertEqual(len(e.cal_suspect(bytes(4) + BAD + bytes(0x7F0))), 2)   # voltage gain and current gain, not the zero

    def test_a_page_that_changed_stops_the_restore(self):
        img = self.healthy()
        chip_data = bytearray(img)
        chip_data[0x200] ^= 1; chip_data[0x300] ^= 1
        c = PageChip(chip_data)
        c.meddle = 0x300
        with self.assertRaises(ValueError) as cm:
            e.run(c.call, restore_args(self.image(img)))
        self.assertIn("0x300", str(cm.exception))
        self.assertIn("0x200", str(cm.exception))             # what was written before it

    def test_wrong_size_image(self):
        c = PageChip(self.healthy())
        with self.assertRaises(ValueError):
            e.run(c.call, restore_args(self.image(bytes(0x400))))
        self.assertEqual(c.page_calls, [])

    def test_parser(self):
        p = kr.build_parser() if hasattr(kr, "build_parser") else None
        if p is None:
            self.skipTest("k2_remote has no build_parser")
        a = p.parse_args(["debug", "eeprom", "restore", "--in", "x.bin"])
        self.assertEqual((a.input, a.yes, a.allow_bad_cal, a.backup), ("x.bin", False, False, None))


if __name__ == "__main__":
    unittest.main()
