# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/compdb.py: the .cmd fragments the Makefile records next to every object become compile_commands.json."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import compdb  # noqa: E402


def make_tree(files):
    """Write {relative path: text} under a fresh temp dir and return it."""
    root = Path(tempfile.mkdtemp())
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    return root


FW = "arm-none-eabi-g++ -Os -Iinclude -DK2_FW_VERSION='\"1.0\"' -c src/ui/ui.cpp -o build/ui/ui.o\n"
FW_C = "arm-none-eabi-gcc -Os -isystem third_party/tinyusb/src -c third_party/tinyusb/src/tusb.c -o build/tusb/tusb.o\n"
HOST_LIB = "g++ -std=c++23 -O0 -Iinclude -c src/ui/ui.cpp -o host-build/obj/lib/src/ui/ui.o\n"
HOST_TEST = "g++ -std=c++23 -O0 -Iinclude -Itests -c tests/test_a.cpp -o host-build/obj/tests/test_a.o\n"
SRC = {"src/ui/ui.cpp": "", "tests/test_a.cpp": "", "third_party/tinyusb/src/tusb.c": ""}


class Parse(unittest.TestCase):
    def test_entry_fields(self):
        e = compdb.entry_from_command(FW.strip(), "/host/repo", "/host/repo")
        self.assertEqual(e["directory"], "/host/repo")
        self.assertEqual(e["file"], "/host/repo/src/ui/ui.cpp")
        self.assertEqual(e["output"], "/host/repo/build/ui/ui.o")
        # shell quoting survives the split: one argument, quotes consumed
        self.assertIn('-DK2_FW_VERSION="1.0"', e["arguments"])
        self.assertEqual(e["arguments"][0], "arm-none-eabi-g++")

    def test_c_file_found(self):
        e = compdb.entry_from_command(FW_C.strip(), "/r", "/r")
        self.assertEqual(e["file"], "/r/third_party/tinyusb/src/tusb.c")

    def test_include_argument_is_not_the_source(self):
        cmd = "gcc -O0 -include k2/core/lfs_cfg.h -c third_party/littlefs/lfs.c -o x.o"
        self.assertEqual(compdb.entry_from_command(cmd, "/r", "/r")["file"], "/r/third_party/littlefs/lfs.c")

    def test_container_root_mapped_to_host(self):
        cmd = "arm-none-eabi-g++ -I/workspace/build/gen -c src/ui/ui.cpp -o build/ui/ui.o"
        e = compdb.entry_from_command(cmd, "/workspace", "/home/u/repo")
        self.assertEqual(e["directory"], "/home/u/repo")
        self.assertIn("-I/home/u/repo/build/gen", e["arguments"])
        self.assertEqual(e["file"], "/home/u/repo/src/ui/ui.cpp")

    def test_no_source_gives_none(self):
        self.assertIsNone(compdb.entry_from_command("ld -o x a.o b.o", "/r", "/r"))


class Merge(unittest.TestCase):
    def build(self, extra=None):
        files = dict(SRC)
        files.update({"build/ui/ui.o.cmd": FW, "build/tusb/tusb.o.cmd": FW_C,
                      "host-build/obj/lib/src/ui/ui.o.cmd": HOST_LIB, "host-build/obj/tests/test_a.o.cmd": HOST_TEST})
        files.update(extra or {})
        return make_tree(files)

    def test_firmware_wins_and_tests_come_from_host(self):
        root = self.build()
        db = compdb.collect(root, ["build"], ["host-build"], str(root))
        by_file = {Path(e["file"]).relative_to(root).as_posix(): e for e in db}
        self.assertEqual(sorted(by_file), ["src/ui/ui.cpp", "tests/test_a.cpp", "third_party/tinyusb/src/tusb.c"])
        self.assertEqual(by_file["src/ui/ui.cpp"]["arguments"][0], "arm-none-eabi-g++")
        self.assertEqual(by_file["tests/test_a.cpp"]["arguments"][0], "g++")

    def test_one_entry_per_file_when_host_has_several_sets(self):
        root = self.build({"host-build/obj/scan/src/ui/ui.o.cmd": HOST_LIB.replace("lib", "scan")})
        os.remove(root / "build/ui/ui.o.cmd")
        db = compdb.collect(root, ["build"], ["host-build"], str(root))
        ui = [e for e in db if e["file"].endswith("src/ui/ui.cpp")]
        self.assertEqual(len(ui), 1)
        self.assertIn("host-build/obj/lib/", ui[0]["output"])  # lib is the first set, so it wins

    def test_stale_fragment_of_a_deleted_source_is_dropped(self):
        root = self.build({"build/gone/old.o.cmd": "arm-none-eabi-g++ -c src/gone/old.cpp -o build/gone/old.o\n"})
        db = compdb.collect(root, ["build"], ["host-build"], str(root))
        self.assertFalse([e for e in db if e["file"].endswith("old.cpp")])

    def test_missing_build_dirs_are_fine(self):
        root = make_tree({})
        self.assertEqual(compdb.collect(root, ["build"], ["host-build"], str(root)), [])

    def test_output_is_deterministic(self):
        root = self.build()
        a = compdb.render(compdb.collect(root, ["build"], ["host-build"], str(root)))
        b = compdb.render(compdb.collect(root, ["build"], ["host-build"], str(root)))
        self.assertEqual(a, b)
        self.assertEqual(len(json.loads(a)), 3)


class Toolchain(unittest.TestCase):
    """The ARM compiler is not on the host: its include directories, copied out of the build image by
    `make nav-headers`, are added to the firmware entries so clangd finds <cstdint> and friends."""
    DIRS = "/usr/include/newlib/c++/14.2.1\n/usr/include/newlib\n"

    def tree(self):
        root = make_tree(dict(SRC, **{"build/ui/ui.o.cmd": FW, "host-build/obj/tests/test_a.o.cmd": HOST_TEST,
                                      ".cache/arm-none-eabi/dirs.txt": self.DIRS}))
        return root, compdb.toolchain_isystem(root, ".cache/arm-none-eabi", str(root))

    def test_dirs_are_mapped_under_the_cache_root(self):
        root, flags = self.tree()
        self.assertEqual(flags, ["-isystem", f"{root}/.cache/arm-none-eabi/root/usr/include/newlib/c++/14.2.1",
                                 "-isystem", f"{root}/.cache/arm-none-eabi/root/usr/include/newlib"])

    def test_missing_cache_adds_nothing(self):
        self.assertEqual(compdb.toolchain_isystem(make_tree({}), ".cache/arm-none-eabi", "/x"), [])

    def test_only_arm_entries_get_them(self):
        root, flags = self.tree()
        db = compdb.collect(root, ["build"], ["host-build"], str(root), flags)
        arm = next(e for e in db if e["file"].endswith("src/ui/ui.cpp"))
        host = next(e for e in db if e["file"].endswith("tests/test_a.cpp"))
        self.assertEqual(arm["arguments"][1:5], flags[:4])
        self.assertEqual(arm["arguments"][0], "arm-none-eabi-g++")
        self.assertNotIn("-isystem", host["arguments"])


class Write(unittest.TestCase):
    def test_unchanged_content_keeps_the_file(self):
        root = make_tree({})
        out = root / "compile_commands.json"
        self.assertTrue(compdb.write_if_changed(out, "[]\n"))
        os.utime(out, (1000, 1000))
        self.assertFalse(compdb.write_if_changed(out, "[]\n"))
        self.assertEqual(out.stat().st_mtime, 1000)
        self.assertTrue(compdb.write_if_changed(out, "[1]\n"))
        self.assertEqual(out.read_text(), "[1]\n")
        self.assertEqual([p.name for p in root.iterdir()], ["compile_commands.json"])  # no temp file left


if __name__ == "__main__":
    unittest.main()
