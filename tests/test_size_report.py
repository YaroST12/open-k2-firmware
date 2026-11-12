# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for the pure parsing and grouping of tools/size_report.py (no docker, no ELF)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import size_report as sr

NM = """\
080325f8 000008a0 T k2::shell_hw::(anonymous namespace)::Hw::lab(k2::lab::Cmd const&) [clone .lto_priv.0]\t/workspace/src/core/shell_hw.cpp:951
080293a4 000047a4 T bool k2::remote::handle_request<k2::Remote>(std::span<unsigned char const, 4294967295u>, k2::Remote&)\t/workspace/include/k2/core/remote_proto.hpp:1206
0808a35a 00000a52 T _ZN2k212_GLOBAL__N_1L6bitmapE.lto_priv.3
20004928 00002018 B _ZN2k27pdtrace12_GLOBAL__N_16g_ringE.lto_priv.0
08004404 00000010 t lfs_dir_commit\t/workspace/third_party/littlefs/lfs.c:2000
"""

OBJDUMP = """\
build/open_k2.elf:     file format elf32-littlearm


Disassembly of section .text:

08004404 <k2::Ui::tick(unsigned long)>:
k2::Ui::tick(unsigned long):
/workspace/src/ui/ui.cpp:1070
 8004404:\tpush\t{r4, lr}
 8004406:\tmovs\tr0, #1
/workspace/include/k2/ui/kit.hpp:55 (discriminator 1)
 8004408:\tbl\t8004420 <foo>
 800440c:\tmovs\tr1, #2
/workspace/src/ui/ui.cpp:1071
 800440e:\tpop\t{r4, pc}
 8004410:\t00000001 \t.word\t0x00000001

08004414 <foo>:
foo:
/workspace/src/ui/x.cpp:3
 8004414:\tbx\tlr
"""


class Nm(unittest.TestCase):
    def test_parse_keeps_sizes_names_and_source(self):
        syms = sr.parse_nm(NM)
        self.assertEqual(len(syms), 5)
        h = syms[1]
        self.assertEqual((h.addr, h.size, h.kind), (0x080293A4, 0x47A4, "T"))
        self.assertTrue(h.name.startswith("bool k2::remote::handle_request<k2::Remote>"))
        self.assertEqual((h.path, h.line), ("include/k2/core/remote_proto.hpp", 1206))

    def test_symbol_without_source_has_no_path(self):
        s = sr.parse_nm(NM)[2]
        self.assertEqual(s.path, None)

    def test_code_is_flash_text_only(self):
        code = [s.name for s in sr.code_syms(sr.parse_nm(NM))]
        self.assertEqual(len(code), 4)          # the .bss symbol is not code

    def test_group_of_paths(self):
        self.assertEqual(sr.group_of("src/ui/ui.cpp"), "src/ui")
        self.assertEqual(sr.group_of("include/k2/core/remote_proto.hpp"), "include/k2/core")
        self.assertEqual(sr.group_of("third_party/littlefs/lfs.c"), "third_party/littlefs")
        self.assertEqual(sr.group_of(None), "(no source)")


class Inline(unittest.TestCase):
    def test_bytes_by_origin_file(self):
        per = sr.parse_objdump(OBJDUMP)
        tick = per[0x08004404]
        # 3 instructions before .word are 2+2+4+2 = 10 B up to 0x800440e, then pop 2 B + the .word 4 B
        self.assertEqual(tick["src/ui/ui.cpp"], 2 + 2 + 2 + 4)   # 0x4404..0x4408 and 0x440e..0x4414
        self.assertEqual(tick["include/k2/ui/kit.hpp"], 4 + 2)   # 0x4408..0x440e
        self.assertEqual(per[0x08004414]["src/ui/x.cpp"], 0)     # last instruction: no next address, size unknown

    def test_last_instruction_of_a_function_uses_nm_size(self):
        per = sr.parse_objdump(OBJDUMP, sizes={0x08004414: 2})
        self.assertEqual(per[0x08004414]["src/ui/x.cpp"], 2)


class Diff(unittest.TestCase):
    def test_diff_orders_by_size_change(self):
        a = {"f": 100, "g": 50, "h": 10}
        b = {"f": 40, "g": 50, "i": 7}
        d = sr.diff_sizes(a, b)
        self.assertEqual(d[0], ("f", 100, 40, -60))
        self.assertIn(("h", 10, 0, -10), d)
        self.assertIn(("i", 0, 7, 7), d)
        self.assertNotIn("g", [x[0] for x in d])


if __name__ == "__main__":
    unittest.main()
