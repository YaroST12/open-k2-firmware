# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Unit tests for tools/check_strings.py (plain string literal guard: text goes through K2_P)."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_strings as c


def lines(src):
    return [f.line for f in c.scan_text("src/x.cpp", src)]


class Detect(unittest.TestCase):
    def test_plain_text_literal_is_flagged(self):
        hits = c.scan_text("src/x.cpp", 'int a;\nconst char *s = "Charger test";\n')
        self.assertEqual([(h.line, h.text) for h in hits], [(2, "Charger test")])

    def test_wrapped_literals_pass(self):
        src = "\n".join([
            'const char *a = K2_P("Charger test");',
            'const char *b = K2_P("Cable "',
            '                     "read");',
            'X(Back, K2_P("Go back"), "")',
        ])
        self.assertEqual(lines(src), [])

    def test_adjacent_pieces_are_one_literal(self):
        self.assertEqual(lines('const char *s = "ab"\n   "cde";'), [1])

    def test_short_literals_below_break_even_pass(self):
        src = "\n".join(['f("Back");', 'f("%u mV");', 'f("\\n\\n\\n\\n");', 'f("");'])
        self.assertEqual(lines(src), [2])   # "%u mV" is 5 bytes: packed

    def test_comments_are_ignored_and_lines_kept(self):
        src = "\n".join([
            '// "a commented literal"',
            '/* "a block',
            '   comment" */',
            'f("a real literal");',
        ])
        self.assertEqual(lines(src), [4])

    def test_char_literals_and_digit_separators(self):
        src = "\n".join([
            "if (c == '\"') n = 1'000'000;",
            'f(c ? K2_P(": skipped, no reply") : K2_P(": skipped"));',
            "g('\\'', \"plain text\");",
        ])
        self.assertEqual(lines(src), [3])

    def test_raw_string_is_flagged(self):
        self.assertEqual(lines('const char *s = R"(a raw literal)";'), [1])


class Exemptions(unittest.TestCase):
    def test_static_assert_condition_and_message(self):
        src = 'static_assert(fits_px(6, "some label", 60), "the label fits its box");'
        self.assertEqual(lines(src), [])

    def test_k2log_format_passes_but_its_string_arguments_do_not(self):
        src = "\n".join([
            'K2LOG("[PD] request %u mV\\n", mv);',
            'K2LOG("[PD] cable "',
            '      "%s\\n", ok ? "accepted" : "rejected");',
        ])
        self.assertEqual(lines(src), [3, 3])

    def test_preprocessor_directives_pass_but_macro_bodies_do_not(self):
        src = "\n".join([
            '#include "k2/core/strpack.hpp"',
            '#pragma GCC diagnostic ignored "-Wformat"',
            '#error "not a firmware build"',
            '#define K2_TITLE "Charger test"',
            '#define K2_POOL(X) \\',
            '    X(Open, "Open file")',
        ])
        self.assertEqual(lines(src), [4, 6])

    def test_attributes_asm_linkage_sizeof_operator(self):
        src = "\n".join([
            '[[gnu::section(".rodata.hot")]] const int t = 1;',
            '__attribute__((section(".cold_probe"))) void f();',
            'asm volatile("nop; nop; nop" ::: "memory");',
            'extern "C" void Reset_Handler();',
            'constexpr int n = sizeof("seventeen chars!");',
            'int operator""_kib(unsigned long long);',
        ])
        self.assertEqual(lines(src), [])

    def test_literals_that_cannot_be_packed(self):
        src = "\n".join([
            'f("\xe2\x86\x92 arrow");',     # UTF-8 bytes: K2_P is ASCII-only
            'f("\\xC2\\xB0C hot");',
            'f("one\\0two\\0");',          # embedded NUL: packed strings are NUL-terminated
            'f(L"a wide literal");',
        ])
        self.assertEqual(lines(src), [])

    def test_marker_with_a_reason(self):
        src = "\n".join([
            'if (same(tok, "status")) {}   // k2-plain: shell token compared to input',
            'if (same(tok, "version")) {}  // k2-plain:',
        ])
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual([h.line for h in hits], [2])
        self.assertIn("reason", hits[0].why)

    def test_region_marker(self):
        src = "\n".join([
            '// k2-plain-begin: shell tokens compared to typed input',
            'constexpr const char *kCmds[] = {"status", "version", "memory"};',
            '// k2-plain-end',
            'f("after the region");',
        ])
        self.assertEqual(lines(src), [4])


class Allowlist(unittest.TestCase):
    def test_parse_needs_a_reason(self):
        allow = c.parse_allowlist("# comment\n\nsrc/net/http.cpp  K2_NET lab build, not in the image\n")
        self.assertEqual(allow, {"src/net/http.cpp": "K2_NET lab build, not in the image"})
        with self.assertRaises(ValueError):
            c.parse_allowlist("src/net/http.cpp\n")

    def test_tree_scan_skips_allowed_files_and_reports_stale_entries(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            (root / "include/k2").mkdir(parents=True)
            (root / "src/lab.cpp").write_text('f("a lab-only string");\n')
            (root / "src/ui.cpp").write_text('f("a shipping string");\n')
            (root / "src/clean.cpp").write_text('f(K2_P("a packed string"));\n')
            allow = {"src/lab.cpp": "lab", "src/clean.cpp": "no longer needed", "src/gone.cpp": "removed"}
            hits, stale = c.scan_tree(root, allow)
            self.assertEqual([(h.path, h.line) for h in hits], [("src/ui.cpp", 1)])
            self.assertEqual(sorted(stale), ["src/clean.cpp", "src/gone.cpp"])

    def test_main_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "src").mkdir()
            (root / "tools").mkdir()
            (root / "tools/check_strings_allow.txt").write_text("")
            (root / "src/a.cpp").write_text('f(K2_P("packed text"));\n')
            self.assertEqual(c.main(["--root", str(root)]), 0)
            (root / "src/a.cpp").write_text('f("plain text");\n')
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                self.assertEqual(c.main(["--root", str(root)]), 1)
            self.assertIn("src/a.cpp:1:", err.getvalue())
            self.assertIn("K2_P", err.getvalue())


class SharedScanner(unittest.TestCase):
    def test_wrapped_literals_feed_the_pair_table(self):
        src = 'f(K2_P("Cable "\n       "read")); g("plain text"); h(K2_P("x\\ty"));'
        self.assertEqual(c.wrapped_literals(src), [b"Cable read", b"x\ty"])


if __name__ == "__main__":
    unittest.main()
