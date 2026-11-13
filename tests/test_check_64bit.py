# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Unit tests for tools/check_64bit.py (64-bit integer type guard)."""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import check_64bit as c


class StripAndDetect(unittest.TestCase):
    def test_detects_all_64bit_integer_types(self):
        src = "\n".join([
            "uint64_t a = 0;",
            "int64_t b = 0;",
            "std::uint64_t c = 0;",
            "std::int64_t d = 0;",
            "long long e = 0;",
            "unsigned long long f = 0;",
            "signed long long g = 0;",
            "uint_fast64_t h = 0;",
            "int_fast64_t i = 0;",
            "std::uint_fast64_t j = 0;",
            "std::int_fast64_t k = 0;",
            "uint_least64_t l = 0;",
            "int_least64_t m = 0;",
            "std::uint_least64_t n = 0;",
            "std::int_least64_t o = 0;",
        ])
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual([h.line for h in hits], list(range(1, 16)))

    def test_ignores_32bit_and_similar_identifiers(self):
        src = "\n".join([
            "uint32_t a = 0;",
            "int32_t b = 0;",
            "unsigned long c = 0;",
            "long d = 0;",
            "using my_uint64_t = int;",
            "int uint64_t_helper = 0;",
            "int long_long_var = 0;",
        ])
        self.assertEqual(c.scan_text("src/x.cpp", src), [])

    def test_ignores_comments_and_preserves_line_numbers(self):
        src = "\n".join([
            "// uint64_t in line comment",
            "// continued \\",
            "uint64_t still_in_line_comment = 0;",
            "/* block comment",
            "   std::int64_t inside block",
            "   long long inside block */",
            "int ok = 1;",
            "uint64_t real_hit = 2;",
        ])
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0].line, 8)
        self.assertEqual(hits[0].kind, "uint64_t")

    def test_ignores_string_and_char_literals_including_raw_strings(self):
        src = "\n".join([
            'const char* s1 = "uint64_t std::int64_t long long";',
            'const char* s2 = "escaped \\" uint64_t \\" end";',
            'const char* s3 = u8"uint_fast64_t";',
            r'const char* s4 = R"tag(uint64_t\nint64_t)tag";',
            "char q = '\\''; int after_char = 1;",
            'const char* s5 = "not a // comment"; uint64_t real_after_str = 0;',
        ])
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0].line, 6)

    def test_digit_separators_do_not_swallow_code(self):
        src = "const uint32_t x = 1'000'000u + static_cast<uint32_t>(uint64_t{a}) + 2'000u;\n"
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0].line, 1)
        self.assertEqual(hits[0].kind, "uint64_t")

    def test_deduplicates_multiple_types_on_same_line(self):
        src = "const uint64_t x = static_cast<uint64_t>(a) + std::int64_t(b);\n"
        hits = c.scan_text("src/x.cpp", src)
        self.assertEqual(len(hits), 1)


class AllowlistAndInlineOptIn(unittest.TestCase):
    def test_allowlist_ignores_whitespace_and_trailing_comments(self):
        allow = c.parse_allowlist(
            "# rationale comment\n\n"
            "src/a.cpp   std::uint64_t acc_ = 0;\n"
        )
        code = "    std::uint64_t   acc_   =   0;  // microjoules\n"
        self.assertEqual(c.scan_text("src/a.cpp", code, allow), [])
        # Different file or different variable must still fail
        self.assertEqual(len(c.scan_text("src/b.cpp", code, allow)), 1)
        self.assertEqual(len(c.scan_text("src/a.cpp", "std::uint64_t extra_ = 0;\n", allow)), 1)

    def test_malformed_allowlist_line_raises(self):
        with self.assertRaises(ValueError):
            c.parse_allowlist("src/a.cpp_without_code\n")

    def test_inline_allow_comment(self):
        src = "\n".join([
            "uint64_t ok1 = 0; // k2-allow-64bit",
            "uint64_t ok2 = 0; /* k2-allow-64bit */",
            'const char* decoy = "k2-allow-64bit"; uint64_t bad = 0;',
        ])
        hits = c.scan_text("src/a.cpp", src)
        self.assertEqual([h.line for h in hits], [3])


class TreeAndCliModes(unittest.TestCase):
    def _make_tree(self, root: Path):
        (root / "src/core").mkdir(parents=True)
        (root / "src/third_party/foo").mkdir(parents=True)
        (root / "include/k2/core").mkdir(parents=True)
        (root / "include/other").mkdir(parents=True)
        (root / "tests").mkdir(parents=True)
        (root / "tools").mkdir(parents=True)

        (root / "src/core/bad.cpp").write_text("uint64_t oops = 1;\n")
        (root / "src/third_party/foo/tp.c").write_text("uint64_t ignored_tp = 1;\n")
        (root / "include/k2/core/hdr.hpp").write_text("std::int64_t hdr_oops = 2;\n")
        (root / "include/other/ext.h").write_text("uint64_t ignored_other = 1;\n")
        (root / "tests/test_x.cpp").write_text("uint64_t ignored_test = 1;\n")
        (root / "tools/check_64bit_allow.txt").write_text("")

    def test_scan_tree_scope_and_exclusions(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._make_tree(root)
            hits = c.scan_tree(root)
            self.assertEqual([(h.path, h.line) for h in hits],
                             [("include/k2/core/hdr.hpp", 1), ("src/core/bad.cpp", 1)])

    def test_cli_modes_block_warn_off(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self._make_tree(root)

            err_block = io.StringIO()
            with contextlib.redirect_stderr(err_block):
                rc = c.main(["--mode", "block", "--root", str(root)])
            self.assertEqual(rc, 1)
            self.assertIn("src/core/bad.cpp:1: error:", err_block.getvalue())
            self.assertIn("include/k2/core/hdr.hpp:1: error:", err_block.getvalue())

            err_warn = io.StringIO()
            with contextlib.redirect_stderr(err_warn):
                rc = c.main(["--mode", "warn", "--root", str(root)])
            self.assertEqual(rc, 0)
            self.assertIn("src/core/bad.cpp:1: warning:", err_warn.getvalue())

            err_off = io.StringIO()
            with contextlib.redirect_stderr(err_off):
                rc = c.main(["--mode", "off", "--root", str(root)])
            self.assertEqual(rc, 0)
            self.assertEqual(err_off.getvalue(), "")

    def test_real_repo_passes_with_allowlist(self):
        allow = c.load_allowlist(c.DEFAULT_ALLOW)
        self.assertEqual(c.scan_tree(c.ROOT, allow), [])


if __name__ == "__main__":
    unittest.main()
