# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for `k2_flasher.py backup` (full by default, `--app-only`, the ZIP) and the backup step of `flash`
(no device needed).

A fake bootloader serves a 1 MB flash image. The real bootloader is only known to serve the application area
(up to APP_LIMIT 0x080E3000); the fake can refuse reads above any limit, so the clean handling of a refusal
(last readable address, partial dump, manifest) is covered here.
"""
import contextlib
import hashlib
import io
import json
import sys
import tempfile
import unittest
import zipfile
from datetime import datetime, timezone
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


SIX_FILES = {"flash.bin", "app.bin", "bootloader.bin", "native_nvram.bin", "manifest.json"}


class FullIsTheDefault(unittest.TestCase):
    def test_backup_without_options_reads_the_whole_flash(self):
        img = make_image()
        with tempfile.TemporaryDirectory() as d:
            manifest = kf.K2Updater(FakeProtocol(img)).backup(Path(d) / "dump")
            self.assertEqual((Path(d) / "dump" / "flash.bin").read_bytes(), img)
        self.assertEqual(manifest["backup_kind"], "full")
        self.assertEqual(manifest["mode"], "full")

    def test_the_kind_names_what_was_saved(self):
        img = make_image()
        m, _ = run_backup(FakeProtocol(img), full=True)
        self.assertEqual(m["backup_kind"], "full")
        m, _ = run_backup(FakeProtocol(img), full=False)
        self.assertEqual(m["backup_kind"], "app-only")
        m, _ = run_backup(FakeProtocol(img, readable_end=kf.APP_LIMIT), full=True)
        self.assertEqual(m["backup_kind"], "partial")

    def test_the_manifest_carries_the_sha256_of_the_native_storage_file(self):
        img = make_image()
        manifest, files = run_backup(FakeProtocol(img), full=True)
        self.assertEqual(manifest["native_nvram_sha256"], hashlib.sha256(files["native_nvram.bin"]).hexdigest())
        self.assertEqual(manifest["native_nvram_sha256"], hashlib.sha256(img[0xE0000:]).hexdigest())
        self.assertEqual(json.loads(files["manifest.json"])["native_nvram_sha256"], manifest["native_nvram_sha256"])
        self.assertEqual(json.loads(files["manifest.json"])["backup_kind"], "full")


class BackupZip(unittest.TestCase):
    def test_the_zip_path_is_derived_from_the_manifest_next_to_the_directory(self):
        manifest = {"timestamp_utc": "2026-11-08T10:00:01.123456+00:00", "identity": {"current_version": "5.8"}}
        p = kf.backup_zip_path(Path("/x/out/dump"), manifest)
        self.assertEqual(p, Path("/x/out/K2_BACKUP_5.8_20261108T100001Z.zip"))

    def test_one_zip_holds_all_six_files(self):
        img = make_image()
        with tempfile.TemporaryDirectory() as d:
            dest = Path(d) / "dump"
            manifest = kf.K2Updater(FakeProtocol(img)).backup(dest)
            zips = sorted(p.name for p in Path(d).glob("K2_BACKUP_*.zip"))
            self.assertEqual(zips, [kf.backup_zip_path(dest, manifest).name])
            self.assertRegex(zips[0], r"^K2_BACKUP_\d+\.\d+_\d{8}T\d{6}Z\.zip$")
            with zipfile.ZipFile(Path(d) / zips[0]) as z:
                self.assertIsNone(z.testzip())
                names = {i.filename for i in z.infolist()}
                self.assertEqual(len(z.infolist()), 6)
                self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist()))
                rec = next(n for n in names if n.startswith("K2_RECOVERY_") and n.endswith(".k2"))
                self.assertEqual(names - {rec}, SIX_FILES)
                for n in names:
                    self.assertEqual(z.read(n), (dest / n).read_bytes(), n)

    def test_a_partial_backup_is_packed_too(self):
        with tempfile.TemporaryDirectory() as d:
            dest = Path(d) / "dump"
            manifest = kf.K2Updater(FakeProtocol(make_image(), readable_end=kf.APP_LIMIT)).backup(dest)
            with zipfile.ZipFile(kf.backup_zip_path(dest, manifest)) as z:
                self.assertEqual(json.loads(z.read("manifest.json"))["backup_kind"], "partial")


class SummaryAndExit(unittest.TestCase):
    def test_a_full_backup_says_so(self):
        text = kf.backup_summary({"complete": True, "backup_kind": "full"})
        self.assertIn("Full backup", text)

    def test_an_app_only_backup_says_the_device_storage_is_missing(self):
        text = kf.backup_summary({"complete": True, "backup_kind": "app-only"})
        self.assertIn("APPLICATION AREA ONLY", text)
        self.assertIn("0x080E3000", text)
        self.assertIn("NOT", text)

    def test_a_partial_backup_says_the_device_storage_is_missing(self):
        text = kf.backup_summary({"complete": False, "backup_kind": "partial",
                                  "last_readable_address": "0x080E2FFF", "refused_at": "0x080E3000"})
        self.assertIn("PARTIAL", text)
        self.assertIn("device storage", text)

    def test_only_a_refusal_gives_a_non_zero_exit(self):
        self.assertEqual(kf.backup_exit_code({"complete": True, "backup_kind": "app-only"}), 0)
        self.assertEqual(kf.backup_exit_code({"complete": False, "backup_kind": "partial"}), 3)


class Options(unittest.TestCase):
    def parse(self, *argv):
        return kf.build_parser().parse_args(argv)

    def test_backup_is_full_unless_app_only_is_given(self):
        self.assertFalse(self.parse("backup", "out").app_only)
        self.assertTrue(self.parse("backup", "out", "--app-only").app_only)

    def test_full_is_still_accepted_and_changes_nothing(self):
        a = self.parse("backup", "out", "--full")
        self.assertFalse(a.app_only)

    def test_full_and_app_only_together_are_refused(self):
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                self.parse("backup", "out", "--full", "--app-only")

    def test_flash_backs_up_first_unless_told_not_to(self):
        a = self.parse("flash", "x.k2")
        self.assertFalse(a.no_backup)
        self.assertIsNone(a.backup_dir)
        a = self.parse("flash", "x.k2", "--no-backup", "--backup-dir", "somewhere")
        self.assertTrue(a.no_backup)
        self.assertEqual(a.backup_dir, "somewhere")

    def test_the_default_backup_directory_is_under_runs(self):
        p = kf.default_backup_dir(datetime(2026, 11, 8, 10, 0, 1, tzinfo=timezone.utc))
        self.assertEqual(p.name, "k2-backup-20261108T100001Z")
        self.assertEqual(p.parent.name, "runs")
        self.assertEqual(p.parent.parent, Path(kf.__file__).resolve().parent.parent)


class StubUpdater:
    """Records the order of calls; backup() can be told to fail or to return a partial manifest."""

    STAMP = {"timestamp_utc": "2026-11-08T10:00:01+00:00", "identity": {"current_version": "5.8"}}

    def __init__(self, manifest=None, fail=None):
        self.calls = []
        self.manifest = {**self.STAMP, **(manifest or {"complete": True, "backup_kind": "full"})}
        self.fail = fail

    def backup(self, dest_dir, full=True):
        self.calls.append(("backup", Path(dest_dir), full))
        if self.fail:
            raise self.fail
        return self.manifest

    def flash(self, fw, auto_reboot=True):
        self.calls.append(("flash", fw, auto_reboot))
        return "ident"


class FlashWithBackup(unittest.TestCase):
    def go(self, updater, no_backup=False):
        out = io.StringIO()
        with tempfile.TemporaryDirectory() as d:
            result = kf.flash_with_backup(updater, "fw", Path(d) / "bk", no_backup=no_backup, auto_reboot=True, out=out)
        return result, out.getvalue()

    def test_the_backup_comes_first_then_the_flash(self):
        u = StubUpdater()
        result, text = self.go(u)
        self.assertEqual([c[0] for c in u.calls], ["backup", "flash"])
        self.assertTrue(u.calls[0][2])                       # a full backup
        self.assertEqual(result, "ident")
        self.assertIn("Full backup", text)

    def test_no_backup_skips_it_and_says_so(self):
        u = StubUpdater()
        _, text = self.go(u, no_backup=True)
        self.assertEqual([c[0] for c in u.calls], ["flash"])
        self.assertIn("without a backup", text)

    def test_a_failed_backup_stops_before_the_flash(self):
        u = StubUpdater(fail=kf.UpgradeError("backup-read", "read failed"))
        with self.assertRaises(kf.UpgradeError) as ctx:
            self.go(u)
        self.assertEqual([c[0] for c in u.calls], ["backup"])
        self.assertIn("nothing was erased", str(ctx.exception))

    def test_a_partial_backup_warns_and_the_flash_goes_on(self):
        u = StubUpdater({"complete": False, "backup_kind": "partial",
                         "last_readable_address": "0x080E2FFF", "refused_at": "0x080E3000"})
        _, text = self.go(u)
        self.assertEqual([c[0] for c in u.calls], ["backup", "flash"])
        self.assertIn("device storage", text)


if __name__ == "__main__":
    unittest.main()
