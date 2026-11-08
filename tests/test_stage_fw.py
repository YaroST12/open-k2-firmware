# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/stage_fw.py: the fw/ tree the Pages site serves (open_k2.k2 + latest.json), from a release or a local build."""
import hashlib
import json
import sys
import tempfile
import unittest
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import k2_flasher  # noqa: E402
import stage_fw  # noqa: E402


def _app():
    """A minimal valid app image: Cortex-M vectors at the start, erased elsewhere."""
    buf = bytearray(b"\xff" * 4096)
    buf[0:8] = (0x20018000).to_bytes(4, "little") + (0x08003800 | 1).to_bytes(4, "little")
    return bytes(buf)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.build, self.rel, self.out = root / "build", root / "rel", root / "site"
        self.build.mkdir()
        self.rel.mkdir()
        self.blob = k2_flasher.Firmware.from_app(_app(), version_str="1.7").source


class LocalBuildTest(Base):
    def setUp(self):
        super().setUp()
        self.k2 = self.build / "open_k2.k2"
        self.k2.write_bytes(self.blob)
        (self.build / ".git-v1.7-3-gabc1234").touch()

    def test_stages_container_and_manifest_labelled_local(self):
        info = stage_fw.stage_local(self.build, self.out)
        self.assertEqual((self.out / "fw" / "open_k2.k2").read_bytes(), self.blob)
        self.assertEqual(json.loads((self.out / "fw" / "latest.json").read_text()), info)
        self.assertEqual(set(info), {"version", "git", "sha256", "size", "built"})
        self.assertEqual(info["version"], "local-v1.7-3-gabc1234")
        self.assertEqual(info["git"], "v1.7-3-gabc1234")
        self.assertEqual(info["sha256"], hashlib.sha256(self.blob).hexdigest())
        self.assertEqual(info["size"], len(self.blob))
        stamp = datetime.fromtimestamp(self.k2.stat().st_mtime, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.assertEqual(info["built"], stamp)

    def test_missing_stamp_is_unknown(self):
        next(self.build.glob(".git-*")).unlink()
        self.assertEqual(stage_fw.stage_local(self.build, self.out)["git"], "unknown")

    def test_refuses_a_file_that_is_not_a_k2_container(self):
        self.k2.write_bytes(b"not a firmware")
        with self.assertRaises(ValueError):
            stage_fw.stage_local(self.build, self.out)
        self.assertFalse((self.out / "fw").exists())

    def test_optional_without_a_build_stages_nothing(self):
        self.k2.unlink()
        self.assertEqual(stage_fw.main(["--build", str(self.build), "--out", str(self.out), "--optional"]), 0)
        self.assertFalse((self.out / "fw").exists())
        self.assertEqual(stage_fw.main(["--build", str(self.build), "--out", str(self.out)]), 1)


class ReleaseTest(Base):
    def setUp(self):
        super().setUp()
        (self.rel / "open_k2.k2").write_bytes(self.blob)
        self.sha = hashlib.sha256(self.blob).hexdigest()
        # sha256sum layout; the other files of a release are listed too
        (self.rel / "SHA256SUMS").write_text(f"{'0' * 64}  open_k2.bin\n{self.sha}  open_k2.k2\n{'1' * 64} *open_k2.elf\n")

    def stage(self):
        return stage_fw.stage_release(self.rel, "v1.7", "2026-11-08T12:34:56Z", self.out)

    def test_manifest_comes_from_the_release(self):
        info = self.stage()
        self.assertEqual((self.out / "fw" / "open_k2.k2").read_bytes(), self.blob)
        self.assertEqual(json.loads((self.out / "fw" / "latest.json").read_text()), info)
        self.assertEqual(info, {"version": "v1.7", "git": "v1.7", "sha256": self.sha, "size": len(self.blob),
                                "built": "2026-11-08T12:34:56Z"})

    def test_binary_mode_checksum_line_is_read(self):
        (self.rel / "SHA256SUMS").write_text(f"{self.sha} *open_k2.k2\n")
        self.assertEqual(self.stage()["sha256"], self.sha)

    def test_asset_that_differs_from_SHA256SUMS_is_refused(self):
        (self.rel / "SHA256SUMS").write_text(f"{'0' * 64}  open_k2.k2\n")
        with self.assertRaises(ValueError):
            self.stage()
        self.assertFalse((self.out / "fw").exists())

    def test_release_without_a_listed_container_is_refused(self):
        (self.rel / "SHA256SUMS").write_text(f"{self.sha}  open_k2.bin\n")
        with self.assertRaises(ValueError):
            self.stage()

    def test_cli(self):
        argv = ["--release-dir", str(self.rel), "--tag", "v1.7", "--published", "2026-11-08T12:34:56Z", "--out", str(self.out)]
        self.assertEqual(stage_fw.main(argv), 0)
        self.assertEqual(json.loads((self.out / "fw" / "latest.json").read_text())["version"], "v1.7")


if __name__ == "__main__":
    unittest.main()
