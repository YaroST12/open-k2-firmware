# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Mount host-build/drive.img (dumped by tests/test_drive) with the independent FAT16 reader."""
import json
import os
import shutil
import subprocess
import unittest

from test_vfat_image import Fat16

IMG = os.path.join(os.environ.get("K2_DUMP", "host-build"), "drive.img")


@unittest.skipUnless(os.path.exists(IMG), "no host-build/drive.img (run make test)")
class DriveImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMG, "rb") as f:
            cls.fs = Fat16(f.read())
        cls.files = dict(cls.fs.walk())

    def test_tree(self):
        self.assertEqual(sorted(self.files), [
            "BLACKBOX/", "CARDS.JSN", "LOG.TXT", "README.TXT", "RIPPLE.CSV", "SAVEDLOG.TXT", "STATUS.TXT", "SUMMARY.MD"])

    def test_readme_names_version_and_git(self):
        t = self.files["README.TXT"].decode()
        self.assertIn("9.9", t)
        self.assertIn("abc1234-dirty", t)

    def test_fixed_size_live_files(self):
        self.assertEqual(len(self.files["STATUS.TXT"]), 1024)
        self.assertEqual(len(self.files["LOG.TXT"]), 3072)
        self.assertTrue(self.files["LOG.TXT"].startswith(b"[BOOT] hello"))

    def test_cards_file_is_one_json_array(self):
        cards = json.loads(self.files["CARDS.JSN"].decode())
        self.assertEqual([c["no"] for c in cards], [1, 2])
        self.assertEqual(cards[1]["name"], "Second")
        self.assertEqual(cards[0]["groups"]["PD"]["verdict"], "pass")

    @unittest.skipUnless(shutil.which("fsck.fat"), "fsck.fat not installed")
    def test_fsck_clean(self):
        r = subprocess.run(["fsck.fat", "-n", IMG], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
