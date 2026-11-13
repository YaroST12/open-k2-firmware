# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for the pure checks in tools/gate.py (no docker, no device)."""
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import gate as g

OBJDUMP = """build/open_k2.elf:     file format elf32-littlearm

Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .isr_vector   00000130  08003800  08003800  00003800  2**2
                  CONTENTS, ALLOC, LOAD, READONLY, DATA
  1 .meta_version 00000004  08004000  08004000  00004000  2**2
  2 .meta_model   00000008  08004010  08004010  00004010  2**0
  3 .meta_commit  00000004  08004400  08004400  00004400  2**2
  4 .text         00014544  08004404  08004404  00004404  2**2
  9 .bss          00004a9c  20000100  08018a54  00020100  2**3
"""
NM = """0800b4ec T Reset_Handler
0800ef2c T USB_LP_CAN1_RX0_IRQHandler
08003800 R g_pfnVectors
08004010 R k2_model_signature
"""
SIZE = "   text\t   data\t    bss\t    dec\t    hex\tfilename\n  83848\t      8\t  20128\t 103984\t  19630\tbuild/open_k2.elf\n"


NM_STACK = """20015f4c B _ebss
20015f80 B _sguard
20016000 B _eguard
20017ac0 A _psp_top
20018000 R _estack
"""


class StackBudget(unittest.TestCase):
    SYM = g.parse_symbols(NM_STACK)      # the branch layout: 128 B guard, PSP 6848 B, MSP 1344 B

    def check(self, main=5472, isr=304, fault=1224, **sym):
        return g.check_stack({**self.SYM, **sym}, main, isr, fault)

    def test_parse_symbols(self):
        self.assertEqual(self.SYM["_psp_top"], 0x20017AC0)
        self.assertEqual(g.parse_symbols(NM)["Reset_Handler"], 0x0800B4EC)
        self.assertEqual(g.parse_symbols("         U extern\nzz T bad\n"), {})    # no address / not hex

    def test_current_layout_passes(self):
        self.assertEqual(self.check(), [])

    def test_psp_margin_is_exact(self):
        # PSP 6848 B: depth + 104 B exception frame + 1024 B margin
        self.assertEqual(self.check(main=6848 - 104 - 1024), [])
        errs = self.check(main=6848 - 104 - 1024 + 1)
        self.assertEqual(len(errs), 1)
        self.assertIn("short by 1", errs[0])

    def test_depth_over_the_stack_fails(self):
        # the pre-guard layout: one 4.5 KB stack, and the Charger page alone went past it
        errs = self.check(main=7000)
        self.assertEqual(len(errs), 1)
        self.assertIn("PSP stack 6848", errs[0])

    def test_msp_margin(self):
        self.assertEqual(self.check(fault=1344 - 64), [])
        self.assertEqual(len(self.check(fault=1344 - 64 + 1)), 1)
        self.assertEqual(len(self.check(isr=1400)), 1)

    def test_old_layout_fails(self):
        # beb4c04: 2300 B left above .bss, no split stacks, no guard symbols
        errs = g.check_stack({"_ebss": 0x20017704, "_sguard": 0x20017704, "_eguard": 0x20017B08,
                              "_psp_top": 0x20018000, "_estack": 0x20018000}, 3000, 300, 0)
        self.assertTrue(any("only 2300 bytes" in e for e in errs), errs)
        self.assertTrue(any("PSP stack" in e for e in errs), errs)

    def test_missing_symbols_are_an_error(self):
        errs = g.check_stack({"_ebss": 0x20015F4C}, 1, 1, 1)
        self.assertEqual(len(errs), 1)
        self.assertIn("_psp_top", errs[0])

    def test_guard_must_be_an_mpu_region(self):
        bad = self.check(_sguard=0x20015F40)          # 192 B: not a power of two
        self.assertTrue(any("MPU region" in e for e in bad), bad)
        unaligned = self.check(_sguard=0x20015F90, _eguard=0x20016010)   # 128 B but not 128-aligned
        self.assertTrue(any("MPU region" in e for e in unaligned), unaligned)

    def test_out_of_order_is_an_error(self):
        errs = self.check(_psp_top=0x20015000)
        self.assertTrue(any("out of order" in e for e in errs), errs)

    def test_estack_must_be_ram_top(self):
        self.assertTrue(any("top of the mapped RAM" in e for e in self.check(_estack=0x20018100)))


class NoPriorities(unittest.TestCase):
    def test_reports_priority_writes(self):
        files = {"src/a.cpp": "NVIC_SetPriority(USB_IRQn, 1);\nint x = 3;\n",
                 "src/b.cpp": "reg::nvic().IP[19] = 0x40;\n   SHPR[2] |= 1;\n",
                 "src/c.cpp": "nvic().ISER[0] = 1u << 3;\nscb().SHCSR |= 1;\n"}
        hits = g.check_no_priorities(files)
        self.assertEqual([h.split(":")[0] + ":" + h.split(":")[1] for h in hits], ["src/a.cpp:1", "src/b.cpp:1", "src/b.cpp:2"])

    def test_clean_sources(self):
        self.assertEqual(g.check_no_priorities({"src/x.cpp": "nvic().ICER[0] = 1;\n"}), [])


class Invariants(unittest.TestCase):
    def test_good_elf(self):
        self.assertEqual(g.check_invariants(OBJDUMP, NM), [])

    def test_moved_section(self):
        bad = OBJDUMP.replace("08004404  08004404", "08004408  08004408")
        errs = g.check_invariants(bad, NM)
        self.assertEqual(len(errs), 1)
        self.assertIn(".text", errs[0])

    def test_missing_symbol_and_mangled(self):
        errs = g.check_invariants(OBJDUMP, NM.replace("g_pfnVectors", "_ZL11g_pfnVectors"))
        self.assertTrue(any("g_pfnVectors" in e for e in errs))

    def test_missing_section(self):
        errs = g.check_invariants(OBJDUMP.replace(".meta_model", ".meta_modex"), NM)
        self.assertTrue(any(".meta_model" in e for e in errs))


class MapCheck(unittest.TestCase):
    def test_clean(self):
        self.assertEqual(g.check_map("Archive member included\nlibc.a(lib_a-memcpy.o)\n"), [])

    def test_forbidden(self):
        errs = g.check_map("libc.a(lib_a-mallocr.o)\n.text._Znwj\n__cxa_atexit\n")
        self.assertEqual(len(errs), 3)

    def test_lwip_pool_allocators_are_not_libc_malloc(self):
        # lwIP's mem_malloc/memp_malloc carve static arrays; only libc's malloc is forbidden.
        text = " .text.mem_malloc\n .text.memp_malloc\n .text.memp_malloc_pool\n"
        self.assertEqual(g.check_map(text), [])
        self.assertEqual(len(g.check_map(" .text.malloc\n")), 1)
        self.assertEqual(len(g.check_map("libc.a(libc_a-mallocr.o)\n")), 1)

    def test_discarded_section_ignored(self):
        text = "Discarded input sections\n .text.malloc\n .text.__aeabi_uldivmod\nLinker script and memory map\n .text.main\n"
        self.assertEqual(g.check_map(text), [])

    def test_forbidden_64bit_divmod_helpers(self):
        text = (
            "libgcc.a(_udivmoddi4.o)\n"
            " .text.__aeabi_ldivmod\n"
            "               0x08001234        __aeabi_uldivmod\n"
        )
        errs = g.check_map(text)
        self.assertEqual(len(errs), 3)
        self.assertTrue(any("_udivmoddi4" in e for e in errs), errs)
        self.assertTrue(any("__aeabi_ldivmod" in e for e in errs), errs)
        self.assertTrue(any("__aeabi_uldivmod" in e for e in errs), errs)

    def test_64bit_divmod_identifiers_do_not_false_positive(self):
        text = " .text.my_udivmoddi4_test\n .text.custom__aeabi_ldivmod_stub\n"
        self.assertEqual(g.check_map(text), [])


class Size(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(g.parse_size(SIZE), {"text": 83848, "data": 8, "bss": 20128})

    def test_report_within_limit(self):
        lines, warn = g.size_report({"text": 83848, "data": 8, "bss": 20128},
                                    {"text": 83000, "data": 8, "bss": 20000})
        self.assertFalse(warn)
        self.assertIn("+848", "\n".join(lines))

    def test_report_over_five_percent(self):
        _, warn = g.size_report({"text": 90000, "data": 8, "bss": 1}, {"text": 83848, "data": 8, "bss": 1})
        self.assertTrue(warn)

    def test_baseline_roundtrip(self):
        self.assertEqual(g.parse_baseline("# c\ntext=1 data=2 bss=3\n"), {"text": 1, "data": 2, "bss": 3})


class BuildOwnership(unittest.TestCase):
    def test_own_files_are_fine(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "sub").mkdir()
            (Path(d) / "sub" / "a.o").write_text("x")
            self.assertEqual(g.foreign_owned(Path(d), os.getuid()), [])

    def test_missing_dir_is_fine(self):
        self.assertEqual(g.foreign_owned(Path("/nonexistent-k2-build"), 1000), [])

    def test_other_owner_detected(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "a.o").write_text("x")
            found = g.foreign_owned(Path(d), os.getuid() + 1)
            self.assertIn(Path(d) / "a.o", found)

    def test_fix_runs_chown_via_docker_only_when_needed(self):
        calls = []
        msgs = []
        g.ensure_build_owned(Path("/b"), 1000, 1000, owned=lambda p, u: [], chown=calls.append, say=msgs.append)
        self.assertEqual(calls, [])
        g.ensure_build_owned(Path("/b"), 1000, 1000, owned=lambda p, u: [Path("/b/x.o"), Path("/b/y.o")],
                             chown=calls.append, say=msgs.append)
        self.assertEqual(calls, [Path("/b")])
        self.assertIn("not owned by uid 1000", msgs[0])
        self.assertIn("2", msgs[0])


class SanitizeLog(unittest.TestCase):
    def test_clean_log_passes(self):
        self.assertEqual(g.sanitize_errors("test_a: 12 passed\nmake: Leaving directory\n"), [])

    def test_src_report_and_failed_target_fail(self):
        log = ("src/ui/ui.cpp:1:2: runtime error: negation of -2147483648\n"
               "third_party/lwip/src/core/pbuf.c:9: runtime error: misaligned address\n"
               "make: *** [Makefile:293: host-asan/run/test_x.ok] Error 1\n")
        errs = g.sanitize_errors(log)
        self.assertEqual(len(errs), 2)
        self.assertIn("src/ui/ui.cpp", errs[0])
        self.assertIn("test_x", errs[1])

    def test_address_and_leak_reports_fail(self):
        log = ("==12==ERROR: AddressSanitizer: global-buffer-overflow on address 0x1\n"
               "==13==ERROR: LeakSanitizer: detected memory leaks\n")
        self.assertEqual(len(g.sanitize_errors(log)), 2)


class SanitizeStep(unittest.TestCase):
    @staticmethod
    def done(name, rc=0, n=262):
        """The closing line tools/static_check.sh appends to a pass's log."""
        return f"[sanitize] {name}: docker exit {rc}, {n} tests passed\n"

    @classmethod
    def clean(cls, name):
        # A warm incremental run prints no per-test result, so the closing line is all the log carries.
        return cls.done(name)

    def test_verdict_tags_the_pass_and_needs_the_closing_line(self):
        self.assertEqual(g.sanitize_verdict("gcc", self.clean("gcc")), [])
        errs = g.sanitize_verdict("clang", "src/x.cpp:1:2: runtime error: shift\n" + self.clean("clang"))
        self.assertEqual(len(errs), 1)
        self.assertTrue(errs[0].startswith("[clang] "))
        # a log without the closing line (docker never ran, the script died) is a failure, not a pass
        empty = g.sanitize_verdict("gcc", "docker: command not found\n")
        self.assertEqual(len(empty), 1)
        self.assertIn("did not finish", empty[0])
        # the closing line of the other pass does not count
        self.assertIn("did not finish", g.sanitize_verdict("gcc", self.clean("clang"))[0])

    def test_verdict_fails_when_no_test_passed(self):
        errs = g.sanitize_verdict("clang", self.done("clang", 0, 0))
        self.assertEqual(len(errs), 1)
        self.assertIn("no test passed", errs[0])

    def test_verdict_fails_on_a_docker_exit_without_a_report(self):
        errs = g.sanitize_verdict("gcc", self.done("gcc", 2, 262))
        self.assertEqual(len(errs), 1)
        self.assertIn("docker run exited 2 without a report", errs[0])
        # with a report in the log the report is the finding, not the exit code
        errs = g.sanitize_verdict("gcc", "src/x.cpp:1:2: runtime error: shift\n" + self.done("gcc", 2, 262))
        self.assertEqual(len(errs), 1)
        self.assertIn("runtime error", errs[0])

    def test_gate_runs_both_compilers_by_default(self):
        names = [fn.__name__ for _, fn in g.STEPS]
        self.assertIn("step_sanitize", names)
        self.assertLess(names.index("step_tests"), names.index("step_sanitize"))
        self.assertLess(names.index("step_sanitize"), names.index("step_size"))
        self.assertNotIn("step_static", names)          # --static is the slow report-only set
        self.assertEqual(g.SANITIZE_PASSES, ("gcc", "clang"))

    def _step(self, logs):
        """step_sanitize with static_check.sh faked: `logs` maps pass name -> log text (None: no log written)."""
        import tempfile, types
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "build/static").mkdir(parents=True)
            for name, text in logs.items():
                if text is not None:
                    (root / f"build/static/sanitize-{name}.log").write_text(text)
            old = g.ROOT, g.run
            g.ROOT, g.run = root, lambda cmd, **kw: types.SimpleNamespace(returncode=0, stdout="", stderr="")
            try:
                g.step_sanitize()
            finally:
                g.ROOT, g.run = old

    def test_step_passes_when_both_logs_are_clean(self):
        self._step({"gcc": self.clean("gcc"), "clang": self.clean("clang")})

    def test_step_fails_on_a_clang_only_report(self):
        with self.assertRaises(g.GateError) as cm:
            self._step({"gcc": self.clean("gcc"),
                        "clang": "tests/t.cpp:5:1: runtime error: implicit conversion\n" + self.clean("clang")})
        self.assertIn("[clang]", str(cm.exception))
        self.assertNotIn("[gcc]", str(cm.exception))

    def test_step_fails_when_a_log_is_missing(self):
        with self.assertRaises(g.GateError) as cm:
            self._step({"gcc": self.clean("gcc"), "clang": None})
        self.assertIn("[clang]", str(cm.exception))

    def test_script_runs_both_compilers_in_docker_and_all_leaves_sanitizers_out(self):
        text = (Path(g.__file__).resolve().parent / "static_check.sh").read_text()
        self.assertIn("docker run --rm --user", text)
        self.assertIn("sanitize_pass gcc g++ gcc", text)
        self.assertIn("sanitize_pass clang clang++ clang", text)
        self.assertIn("-fsanitize-ignorelist=tools/sanitize_ignorelist.txt", text)
        self.assertNotIn("host-asan", text)
        # the closing line the verdict reads, in the exact form sanitize_verdict matches
        self.assertIn('echo "[sanitize] $1: docker exit $rc, ', text)
        self.assertIn(" tests passed\" >>", text)
        all_line = next(l for l in text.splitlines() if l.strip().startswith("all)"))
        self.assertNotIn("sanitize", all_line)


NM_HOT = """08003800 R g_pfnVectors
08004434 T __hot_start
0801694c T __hot_end
0803ffe0 t some_cold_function
"""


class HotBlock(unittest.TestCase):
    def test_reports_size_and_headroom(self):
        info, errs = g.check_hot(NM_HOT)
        self.assertEqual(errs, [])
        self.assertIn("75032", info[0])          # 0x0801694c - 0x08004434
        self.assertIn("169652", info[0])         # 0x08040000 - 0x0801694c

    def test_block_past_the_fast_window_fails(self):
        errs = g.check_hot(NM_HOT.replace("0801694c T __hot_end", "08040004 T __hot_end"))[1]
        self.assertEqual(len(errs), 1)
        self.assertIn("0x08040004", errs[0])

    def test_missing_symbols_fail(self):
        self.assertEqual(len(g.check_hot("08003800 R g_pfnVectors\n")[1]), 1)


if __name__ == "__main__":
    unittest.main()
