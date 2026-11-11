# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The firmware version as the flasher sees it (no device needed).

The bootloader's version word (0x08004000, one byte: major nibble, minor nibble) stays as it was. The full
`<major>.<minor>.<patch>-<iteration>-<hash>` string is a 48-byte record at the start of .text (0x08004404,
"K2FW" + NUL padded text, src/core/version.cpp). The flasher shows the full string when the record is there and the
`major.minor` nibbles when it is not (firmware from before the record existed)."""
import sys
import tempfile
import unittest
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import k2_flasher as kf
from test_flasher_backup import FakeProtocol, make_image

FULL = "1.0.0-435-26b245ac"
REC = kf.APP_START - kf.FLASH_START + kf.FWID_OFFSET


def with_record(img, text, magic=b"K2FW"):
    img = bytearray(img)
    img[REC:REC + kf.FWID_SIZE] = (magic + text.encode()).ljust(kf.FWID_SIZE, b"\x00")
    return bytes(img)


def app_with_record(text=None, ver_byte=0x10):
    app = bytearray(b"\xff" * (kf.MARKER_OFFSET + 4 + 64))
    app[0:8] = (0x20018000).to_bytes(4, "little") + (0x08003C01).to_bytes(4, "little")
    app[2048] = ver_byte
    if text is not None:
        o = kf.FWID_OFFSET
        app[o:o + kf.FWID_SIZE] = (b"K2FW" + text.encode()).ljust(kf.FWID_SIZE, b"\x00")
    return bytes(app)


class Record(unittest.TestCase):
    def test_offset_is_the_start_of_text(self):
        self.assertEqual(kf.APP_START + kf.FWID_OFFSET, 0x08004404)

    def test_the_text_of_a_record(self):
        self.assertEqual(kf.fwid_text((b"K2FW" + FULL.encode()).ljust(48, b"\x00")), FULL)
        self.assertEqual(kf.fwid_text((b"K2FW" + FULL.encode() + b"-dirty").ljust(48, b"\x00")), FULL + "-dirty")

    def test_no_magic_no_record(self):
        self.assertIsNone(kf.fwid_text(b"\xff" * 48))
        self.assertIsNone(kf.fwid_text(bytes(48)))
        self.assertIsNone(kf.fwid_text(b"K2FX" + b"1.0".ljust(44, b"\x00")))

    def test_an_empty_or_unprintable_record_is_ignored(self):
        self.assertIsNone(kf.fwid_text(b"K2FW".ljust(48, b"\x00")))
        self.assertIsNone(kf.fwid_text(b"K2FW" + b"\x01\x02abc".ljust(44, b"\x00")))

    def test_a_record_without_a_terminator_is_cut_at_its_size(self):
        self.assertEqual(kf.fwid_text(b"K2FW" + b"a" * 44), "a" * 44)

    def test_short_input(self):
        self.assertIsNone(kf.fwid_text(b"K2F"))

    def test_the_label_prefers_the_record_over_the_nibbles(self):
        rec = (b"K2FW" + FULL.encode()).ljust(48, b"\x00")
        self.assertEqual(kf.version_label(0x10, rec), FULL)
        self.assertEqual(kf.version_label(0x10, b"\xff" * 48), "1.0")      # old firmware: the nibbles
        self.assertEqual(kf.version_label(0x58, None), "5.8")


class Container(unittest.TestCase):
    def test_a_new_package_shows_the_full_string(self):
        fw = kf.Firmware.from_app(app_with_record(FULL))
        self.assertEqual(fw.version, FULL)

    def test_an_old_package_shows_the_nibbles(self):
        fw = kf.Firmware.from_app(app_with_record(None, ver_byte=0x10))
        self.assertEqual(fw.version, "1.0")

    def test_pack_version_with_the_full_string_sets_the_nibbles_of_an_image_that_has_none(self):
        app = bytearray(app_with_record(None))
        app[2048] = 0xFF                                       # an image that leaves the byte for the packer
        fw = kf.Firmware.from_app(bytes(app), version_str=FULL)
        self.assertEqual(fw.app[2048], 0x10)
        self.assertEqual(kf.Firmware.from_app(bytes(app), version_str="1.2.3-0-abcdef12-dirty").app[2048], 0x12)

    def test_pack_version_never_touches_a_byte_the_firmware_set(self):
        fw = kf.Firmware.from_app(app_with_record(FULL, ver_byte=0x10), version_str="3.4.0-1-abcdef12")
        self.assertEqual(fw.app[2048], 0x10)

    def test_the_commit_marker_and_record_survive_packing(self):
        fw = kf.Firmware.from_app(app_with_record(FULL))
        self.assertEqual(fw.app[kf.MARKER_OFFSET:kf.MARKER_OFFSET + 4], b"\xff\xff\xff\xff")
        self.assertEqual(kf.fwid_text(fw.app[kf.FWID_OFFSET:kf.FWID_OFFSET + kf.FWID_SIZE]), FULL)

    def test_a_package_round_trips_through_load(self):
        fw = kf.Firmware.from_app(app_with_record(FULL))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.k2"
            p.write_bytes(fw.source)
            self.assertEqual(kf.Firmware.load(str(p)).version, FULL)


class Installed(unittest.TestCase):
    def identify(self, img):
        return kf.K2Updater(FakeProtocol(img))._identify()

    def test_new_firmware_on_the_device(self):
        self.assertEqual(self.identify(with_record(make_image(), FULL)).current_version, FULL)

    def test_old_firmware_on_the_device_reads_as_before(self):
        self.assertEqual(self.identify(make_image()).current_version, "5.8")          # version byte 0x58, no record

    def test_erased_record_area_reads_as_before(self):
        self.assertEqual(self.identify(with_record(make_image(), "", magic=b"\xff" * 4)).current_version, "5.8")

    def test_the_backup_manifest_names_the_installed_version(self):
        img = with_record(make_image(), FULL)
        with tempfile.TemporaryDirectory() as d:
            manifest = kf.K2Updater(FakeProtocol(img)).backup(Path(d) / "dump", full=True)
        self.assertEqual(manifest["identity"]["current_version"], FULL)

    def test_the_backup_of_old_firmware_keeps_the_nibbles(self):
        with tempfile.TemporaryDirectory() as d:
            manifest = kf.K2Updater(FakeProtocol(make_image())).backup(Path(d) / "dump", full=True)
        self.assertEqual(manifest["identity"]["current_version"], "5.8")


class Message(unittest.TestCase):
    def test_upgrade_line_shows_both_full_strings(self):
        self.assertEqual(kf.upgrade_line("1.0", FULL), f"Device upgraded from v1.0 to v{FULL}")
        self.assertEqual(kf.upgrade_line("1.0.0-430-aaaaaaaa", FULL), f"Device upgraded from v1.0.0-430-aaaaaaaa to v{FULL}")


if __name__ == "__main__":
    unittest.main()
