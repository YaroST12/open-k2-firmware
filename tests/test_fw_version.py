# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/fw_version.py: the one firmware version string `<major>.<minor>.<patch>-<iteration>-<hash>`, derived from
`git describe --tags --long --always --dirty --match 'v[0-9]*'`."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import fw_version as fv

TOOL = Path(__file__).resolve().parent.parent / "tools" / "fw_version.py"


class FromDescribe(unittest.TestCase):
    def test_on_the_tag(self):
        self.assertEqual(fv.from_describe("v1.0-0-g26b245ac"), "1.0.0-0-26b245ac")

    def test_commits_after_the_tag(self):
        self.assertEqual(fv.from_describe("v1.0-435-g26b245ac"), "1.0.0-435-26b245ac")

    def test_dirty_tree_never_looks_clean(self):
        self.assertEqual(fv.from_describe("v1.0-435-g26b245ac-dirty"), "1.0.0-435-26b245ac-dirty")
        self.assertEqual(fv.from_describe("v1.0-0-g26b245ac-dirty"), "1.0.0-0-26b245ac-dirty")

    def test_two_number_tag_gets_a_zero_patch(self):
        self.assertEqual(fv.from_describe("v1.1-7-gabcdef12"), "1.1.0-7-abcdef12")

    def test_three_number_tag_is_kept(self):
        self.assertEqual(fv.from_describe("v1.2.3-12-gabcdef12"), "1.2.3-12-abcdef12")

    def test_one_number_tag(self):
        self.assertEqual(fv.from_describe("v2-1-gabcdef12"), "2.0.0-1-abcdef12")

    def test_trailing_newline_is_ignored(self):
        self.assertEqual(fv.from_describe("v1.0-435-g26b245ac\n"), "1.0.0-435-26b245ac")

    def test_no_tag_in_reach_keeps_the_hash(self):
        self.assertEqual(fv.from_describe("26b245ac"), "0.0.0-0-26b245ac")
        self.assertEqual(fv.from_describe("26b245ac-dirty"), "0.0.0-0-26b245ac-dirty")

    def test_not_a_version_tag_falls_back_to_the_hash(self):
        self.assertEqual(fv.from_describe("v1.2-rc1-3-gabcdef12"), "0.0.0-0-abcdef12")

    def test_empty_or_garbage_is_unknown(self):
        self.assertEqual(fv.from_describe(""), "0.0.0-0-unknown")
        self.assertEqual(fv.from_describe("fatal: not a git repository"), "0.0.0-0-unknown")

    def test_no_git_at_all(self):
        self.assertEqual(fv.from_describe(None), "0.0.0-0-unknown")


class FromRepo(unittest.TestCase):
    """The real git, in a throw-away repository."""

    def git(self, cwd, *args):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "commit.gpgsign=false", *args],
                       cwd=cwd, check=True, capture_output=True)

    def test_tag_commits_and_dirty(self):
        with tempfile.TemporaryDirectory() as d:
            self.git(d, "init", "-q")
            (Path(d) / "f").write_text("1")
            self.git(d, "add", "f")
            self.git(d, "commit", "-q", "-m", "one")
            self.git(d, "tag", "v1.0")                           # a lightweight tag counts too (describe --tags)
            on_tag = fv.derive(d)
            self.assertRegex(on_tag, r"^1\.0\.0-0-[0-9a-f]{7,}$")
            (Path(d) / "f").write_text("2")
            self.git(d, "commit", "-q", "-am", "two")
            (Path(d) / "f").write_text("3")
            self.git(d, "commit", "-q", "-am", "three")
            clean = fv.derive(d)
            self.assertRegex(clean, r"^1\.0\.0-2-[0-9a-f]{7,}$")
            (Path(d) / "f").write_text("4")
            self.assertEqual(fv.derive(d), clean + "-dirty")

    def test_annotated_tag_and_a_newer_tag_win(self):
        with tempfile.TemporaryDirectory() as d:
            self.git(d, "init", "-q")
            (Path(d) / "f").write_text("1")
            self.git(d, "add", "f")
            self.git(d, "commit", "-q", "-m", "one")
            self.git(d, "tag", "-a", "v1.0", "-m", "release")
            (Path(d) / "f").write_text("2")
            self.git(d, "commit", "-q", "-am", "two")
            self.git(d, "tag", "-a", "v1.1", "-m", "release")
            self.git(d, "tag", "nightly")                         # not a version tag: ignored
            self.assertRegex(fv.derive(d), r"^1\.1\.0-0-[0-9a-f]{7,}$")

    def test_no_repository_is_unknown(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(fv.derive(d, env={"GIT_CEILING_DIRECTORIES": str(Path(d).parent)}), "0.0.0-0-unknown")

    def test_git_missing_is_unknown(self):
        self.assertEqual(fv.derive(".", git="no-such-git-binary"), "0.0.0-0-unknown")

    def test_cli_prints_the_string(self):
        out = subprocess.run([sys.executable, str(TOOL), "--describe", "v1.0-435-g26b245ac"], capture_output=True,
                             text=True, check=True).stdout
        self.assertEqual(out, "1.0.0-435-26b245ac\n")


class Pieces(unittest.TestCase):
    def test_major_minor_nibbles(self):
        self.assertEqual(fv.major_minor("1.0.0-435-26b245ac"), (1, 0))
        self.assertEqual(fv.major_minor("1.2.3-0-abcdef12-dirty"), (1, 2))
        self.assertEqual(fv.major_minor("1.0"), (1, 0))          # firmware before the three-number form
        self.assertEqual(fv.major_minor("v1.2"), (1, 2))
        self.assertEqual(fv.major_minor("dev"), (1, 0))          # unparsable: the packer's old default

    def test_major_minor_is_a_nibble_each(self):
        self.assertEqual(fv.major_minor("17.18.0-0-abcdef12"), (1, 2))


if __name__ == "__main__":
    unittest.main()
