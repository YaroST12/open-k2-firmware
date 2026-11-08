# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for `k2_flasher.py backup` and `backup --full` (no device needed).

A fake bootloader serves a 1 MB flash image. The real bootloader is only known to serve the application area
(up to APP_LIMIT 0x080E3000); the fake can refuse reads above any limit, so the clean handling of a refusal
(last readable address, partial dump, manifest) is covered here.
"""
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import k2_flasher as kf

FLASH_END = 0x08100000


SIG = kf.APP_START - kf.FLASH_START + 2064      # the model signature word, 0x08004010


def make_image(sig=b"K200"):
    img = bytearray((i * 7 + 3) & 0xFF for i in range(FLASH_END - kf.FLASH_START))
    off = kf.NAME_START - kf.FLASH_START
    img[off:off + 16] = b"K2\x00K2\x00K2\x00" + b"x" * 7
    a = kf.APP_START - kf.FLASH_START
    img[a:a + 8] = (0x20018000).to_bytes(4, "little") + (0x08003C01).to_bytes(4, "little")   # initial SP, reset vector
    img[a + 2048] = 0x58                                      # version byte
    img[SIG:SIG + 4] = sig                                    # a DFU entry leaves 00000000 here
    m = kf.APP_START - kf.FLASH_START + kf.MARKER_OFFSET
    img[m:m + 4] = kf.COMMIT_MARKER
    img[-1] = 0x42                                            # no trailing 0xFF anywhere
    return bytes(img)


class FakeProtocol:
    def __init__(self, image, readable_end=FLASH_END, flaky_at=None):
        self.image = image
        self.readable_end = readable_end        # reads that end above this are refused
        self.flaky_at = flaky_at                # {address: failures left} for transient errors
        self.reads = []
        self.handshakes = 0

    def command(self, command, payload=b""):
        if command == 3:
            self.handshakes += 1

    def read_memory(self, address, size):
        self.reads.append((address, size))
        if self.flaky_at and self.flaky_at.get(address, 0) > 0:
            self.flaky_at[address] -= 1
            raise kf.ProtocolError("Timed out waiting for command 0x0B")
        if address + size > self.readable_end:
            raise kf.ProtocolError("Timed out waiting for command 0x0B")
        o = address - kf.FLASH_START
        return self.image[o:o + size]

    def read_marker(self, address):
        o = address - kf.FLASH_START
        return self.image[o:o + 4]


def run_backup(proto, full):
    with tempfile.TemporaryDirectory() as d:
        dest = Path(d) / "dump"
        manifest = kf.K2Updater(proto).backup(dest, full=full)
        files = {p.name: p.read_bytes() for p in dest.iterdir()}
        return manifest, files


class AppBackup(unittest.TestCase):
    def test_default_backup_stops_at_app_limit(self):
        img = make_image()
        manifest, files = run_backup(FakeProtocol(img), full=False)
        n = kf.APP_LIMIT - kf.FLASH_START
        self.assertEqual(files["flash.bin"], img[:n])
        self.assertEqual(manifest["flash_range"], "0x08000000-0x080E3000")
        self.assertTrue(manifest["two_pass_verified"])
        self.assertEqual(manifest["mode"], "app")
        self.assertTrue(manifest["complete"])
        self.assertEqual(files["native_nvram.bin"], img[0xE0000:n])   # the part of it inside the app range


class FullBackup(unittest.TestCase):
    def test_full_reads_the_whole_flash_twice(self):
        img = make_image()
        proto = FakeProtocol(img)
        manifest, files = run_backup(proto, full=True)
        self.assertEqual(files["flash.bin"], img)
        self.assertEqual(manifest["flash_range"], "0x08000000-0x08100000")
        self.assertEqual(manifest["flash_size"], 0x100000)
        self.assertEqual(manifest["flash_sha256"], hashlib.sha256(img).hexdigest())
        self.assertEqual(manifest["mode"], "full")
        self.assertTrue(manifest["complete"])
        self.assertTrue(manifest["two_pass_verified"])
        self.assertEqual(manifest["last_readable_address"], "0x080FFFFF")
        self.assertIsNone(manifest["refused_at"])
        # the native device storage, 0x080E0000 up, kept as its own file
        self.assertEqual(files["native_nvram.bin"], img[0xE0000:])
        # the recovery container still carries the application area only
        app = img[kf.APP_START - kf.FLASH_START:kf.APP_LIMIT - kf.FLASH_START]
        self.assertEqual(files["app.bin"], app)
        self.assertEqual(files["bootloader.bin"], img[:kf.APP_START - kf.FLASH_START])
        self.assertTrue(any(n.startswith("K2_RECOVERY_") for n in files))
        # two passes: every 1 KB chunk above the application area is read twice
        full_reads = [r for r in proto.reads if r[1] == 1024 and r[0] >= 0x080E3000]
        self.assertEqual(len(full_reads), 2 * (0x20000 - 0x3000) // 1024)

    def test_refusal_at_the_app_limit_is_reported_cleanly(self):
        img = make_image()
        proto = FakeProtocol(img, readable_end=kf.APP_LIMIT)
        manifest, files = run_backup(proto, full=True)
        n = kf.APP_LIMIT - kf.FLASH_START
        self.assertEqual(files["flash.bin"], img[:n])
        self.assertFalse(manifest["complete"])
        self.assertEqual(manifest["mode"], "full")
        self.assertEqual(manifest["refused_at"], "0x080E3000")
        self.assertEqual(manifest["last_readable_address"], "0x080E2FFF")
        self.assertEqual(manifest["flash_range"], "0x08000000-0x080E3000")
        self.assertEqual(manifest["requested_range"], "0x08000000-0x08100000")
        self.assertTrue(manifest["two_pass_verified"])               # the readable part was compared twice
        self.assertEqual(files["native_nvram.bin"], img[0xE0000:n])
        self.assertTrue(any(f.startswith("K2_RECOVERY_") for f in files))
        self.assertEqual(json.loads(files["manifest.json"])["last_readable_address"], "0x080E2FFF")
        self.assertGreaterEqual(proto.handshakes, 2)                 # identify + one after the refusal

    def test_refusal_inside_a_chunk_finds_the_last_readable_word(self):
        img = make_image()
        proto = FakeProtocol(img, readable_end=0x080E3A04)
        manifest, files = run_backup(proto, full=True)
        n = 0x080E3A04 - kf.FLASH_START
        self.assertEqual(files["flash.bin"], img[:n])
        self.assertEqual(manifest["last_readable_address"], "0x080E3A03")
        self.assertEqual(manifest["refused_at"], "0x080E3A04")
        self.assertFalse(manifest["complete"])
        self.assertEqual(files["native_nvram.bin"], img[0xE0000:n])

    def test_a_transient_error_above_the_limit_is_retried(self):
        img = make_image()
        proto = FakeProtocol(img, flaky_at={0x080F0000: 1})
        manifest, files = run_backup(proto, full=True)
        self.assertTrue(manifest["complete"])
        self.assertEqual(files["flash.bin"], img)

    def test_a_failure_inside_the_application_area_is_an_error(self):
        img = make_image()
        proto = FakeProtocol(img, readable_end=0x08050000)
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(kf.UpgradeError):
                kf.K2Updater(proto).backup(Path(d) / "dump", full=True)

    def test_exit_code_tells_a_partial_dump_from_a_complete_one(self):
        self.assertEqual(kf.backup_exit_code({"complete": True}), 0)
        self.assertEqual(kf.backup_exit_code({"complete": False}), 3)

    def test_summary_names_the_next_step_after_a_refusal(self):
        text = kf.backup_summary({"complete": False, "last_readable_address": "0x080E2FFF", "refused_at": "0x080E3000"})
        self.assertIn("0x080E2FFF", text)
        self.assertIn("flash-dump", text)


class RecoveryImage(unittest.TestCase):
    """The recovery container: model signature, and where it stops."""

    def recovery(self, files):
        name = next(n for n in files if n.startswith("K2_RECOVERY_"))
        return kf.Firmware.parse(files[name])

    def test_zeroed_signature_is_restored_in_the_recovery_only(self):
        img = make_image(sig=b"\x00\x00\x00\x00")
        manifest, files = run_backup(FakeProtocol(img), full=False)
        fw = self.recovery(files)
        self.assertEqual(fw.app[2064:2068], b"K200")
        self.assertTrue(manifest["signature_restored"])
        self.assertTrue(json.loads(files["manifest.json"])["signature_restored"])
        # the dump stays as read
        self.assertEqual(files["app.bin"][2064:2068], b"\x00\x00\x00\x00")
        self.assertEqual(files["flash.bin"][SIG:SIG + 4], b"\x00\x00\x00\x00")
        # and nothing else in the image changed
        self.assertEqual(fw.app[:2064], files["app.bin"][:2064])
        self.assertEqual(fw.app[2068:kf.MARKER_OFFSET], files["app.bin"][2068:kf.MARKER_OFFSET])

    def test_intact_signature_is_left_alone(self):
        manifest, files = run_backup(FakeProtocol(make_image()), full=False)
        self.assertEqual(self.recovery(files).app[2064:2068], b"K200")
        self.assertFalse(manifest["signature_restored"])

    def test_foreign_signature_is_refused(self):
        proto = FakeProtocol(make_image(sig=b"K201"))
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(kf.UpgradeError) as ctx:
                kf.K2Updater(proto).backup(Path(d) / "dump")
        self.assertIn("4b323031", str(ctx.exception).lower())

    def test_zero_signature_without_a_vector_table_is_refused(self):
        img = bytearray(make_image(sig=b"\x00\x00\x00\x00"))
        a = kf.APP_START - kf.FLASH_START
        img[a:a + 8] = b"\xff" * 8
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(kf.UpgradeError) as ctx:
                kf.K2Updater(FakeProtocol(bytes(img))).backup(Path(d) / "dump")
        self.assertIn("vector table", str(ctx.exception))

    def test_recovery_stops_at_the_littlefs_volume(self):
        manifest, files = run_backup(FakeProtocol(make_image()), full=False)
        # the image the container carries ends at the volume (the test image has no trailing 0xFF to trim)
        self.assertEqual(len(self.recovery(files).app), kf.FS_START - kf.APP_START)
        self.assertEqual(manifest["recovery_range"], "0x08003800-0x080B3000")


class NativeAreaGuard(unittest.TestCase):
    def container(self, size):
        app = bytearray(b"\xa5" * size)
        app[0:8] = (0x20018000).to_bytes(4, "little") + (0x08003C01).to_bytes(4, "little")
        app[kf.MARKER_OFFSET:kf.MARKER_OFFSET + 4] = b"\xff\xff\xff\xff"
        app[2048] = 0x10
        return kf.Firmware.from_app(bytes(app)).source

    def test_the_largest_container_that_ends_below_the_native_area_parses(self):
        limit = kf.NVRAM_START - kf.APP_START
        kf.Firmware.parse(self.container(limit - kf.HEADER_SIZE))

    def test_a_container_whose_erase_span_reaches_the_native_area_is_refused(self):
        limit = kf.NVRAM_START - kf.APP_START
        with self.assertRaises(ValueError) as ctx:
            kf.Firmware.parse(self.container(limit - kf.HEADER_SIZE + 4))
        self.assertIn("native", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
