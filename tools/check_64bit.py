#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Guard against accidental 64-bit integer types in Cortex-M4 firmware sources.

64-bit multiplies/divisions on Cortex-M4 either pull libgcc helpers (__aeabi_ldivmod,
__aeabi_uldivmod, _udivmoddi4) or bloat register pressure and stack frames. Scans src/ and
include/k2/ (excluding third_party/, comments, and string/char literals) for 64-bit integer
types unless allowlisted in tools/check_64bit_allow.txt or inline via `// k2-allow-64bit`.
"""
import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ALLOW = ROOT / "tools/check_64bit_allow.txt"
SCAN_DIRS = ("include/k2", "src")
SCAN_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}

TYPE_RX = re.compile(
    r"\b(?:(?:std\s*::\s*)?u?int(?:_(?:fast|least))?64_t|long\s+long)\b"
)
RAW_STR_PREFIX_RX = re.compile(r'(?:u8|[uUL])?R"([^\s()\\]{0,16})\(')
STR_PREFIX_RX = re.compile(r'(?:u8|[uUL])?"')
CHAR_PREFIX_RX = re.compile(r"(?:u8|[uUL])?'")
IDENT_RX = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
NUM_RX = re.compile(r"[0-9](?:'[0-9A-Za-z_]|[0-9A-Za-z_.])*")


class Finding(NamedTuple):
    path: str
    line: int
    kind: str
    code: str


def _blank_preserving_newlines(s: str) -> str:
    return "".join("\n" if ch == "\n" else " " for ch in s)


def strip_cxx(text: str, *, strip_literals: bool = True) -> str:
    """Return `text` with C/C++ comments (and optionally string/char literals) blanked out.

    Newlines are preserved so 1-based line numbers remain exact. C++14 numeric digit
    separators (e.g. `1'000'000u`) are consumed as numeric tokens so `'` is never
    misinterpreted as a character literal delimiter.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]

        # Line comment // ... (including trailing backslash line continuations)
        if ch == "/" and i + 1 < n and text[i + 1] == "/":
            start = i
            i += 2
            while i < n:
                if text[i] == "\n":
                    if i > start and text[i - 1] == "\\":
                        i += 1
                        continue
                    if i >= 2 and text[i - 1] == "\r" and text[i - 2] == "\\":
                        i += 1
                        continue
                    break
                i += 1
            out.append(_blank_preserving_newlines(text[start:i]))
            continue

        # Block comment /* ... */
        if ch == "/" and i + 1 < n and text[i + 1] == "*":
            end = text.find("*/", i + 2)
            end = n if end == -1 else end + 2
            out.append(_blank_preserving_newlines(text[i:end]))
            i = end
            continue

        # Raw string literal (u8/u/U/L)?R"delim(...)delim"
        if ch in ("R", "u", "U", "L"):
            m = RAW_STR_PREFIX_RX.match(text, i)
            if m:
                delim = m.group(1)
                close_seq = f"){delim}\""
                end = text.find(close_seq, m.end())
                end = n if end == -1 else end + len(close_seq)
                seg = text[i:end]
                out.append(_blank_preserving_newlines(seg) if strip_literals else seg)
                i = end
                continue

        # Regular string literal (u8/u/U/L)?"..."
        if ch in ('"', "u", "U", "L"):
            m = STR_PREFIX_RX.match(text, i)
            if m:
                j = m.end()
                while j < n:
                    if text[j] == "\\":
                        j += 2
                    elif text[j] == '"':
                        j += 1
                        break
                    else:
                        j += 1
                seg = text[i:j]
                out.append(_blank_preserving_newlines(seg) if strip_literals else seg)
                i = j
                continue

        # Character literal (u8/u/U/L)?'...'
        if ch in ("'", "u", "U", "L"):
            m = CHAR_PREFIX_RX.match(text, i)
            if m:
                j = m.end()
                while j < n:
                    if text[j] == "\\":
                        j += 2
                    elif text[j] == "'":
                        j += 1
                        break
                    elif text[j] == "\n":
                        break
                    else:
                        j += 1
                seg = text[i:j]
                out.append(_blank_preserving_newlines(seg) if strip_literals else seg)
                i = j
                continue

        # Identifier (must come before NUM_RX so digits inside identifiers stay attached)
        if ch.isalpha() or ch == "_":
            m = IDENT_RX.match(text, i)
            assert m is not None
            out.append(m.group(0))
            i = m.end()
            continue

        # Numeric literal (consumes C++14 digit separators like 1'000'000u)
        if ch.isdigit():
            m = NUM_RX.match(text, i)
            assert m is not None
            out.append(m.group(0))
            i = m.end()
            continue

        out.append(ch)
        i += 1

    return "".join(out)


def normalize_code(line: str) -> str:
    return " ".join(line.split())


def parse_allowlist(text: str) -> set[tuple[str, str]]:
    """Parse allowlist entries of `<relpath> <normalized_code>` (blank / `#` lines ignored)."""
    entries: set[tuple[str, str]] = set()
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split(None, 1)
        if len(parts) != 2:
            raise ValueError(f"invalid allowlist line {lineno}: {raw!r}")
        relpath, code = parts[0], normalize_code(parts[1])
        entries.add((relpath, code))
    return entries


def load_allowlist(path: Path) -> set[tuple[str, str]]:
    if not path.exists():
        return set()
    return parse_allowlist(path.read_text(encoding="utf-8"))


def scan_text(relpath: str, text: str, allow: set[tuple[str, str]] | None = None) -> list[Finding]:
    """Return unallowlisted 64-bit integer type findings in a single file's text."""
    allow = allow or set()
    detect_text = strip_cxx(text, strip_literals=True)
    nocomment_lines = strip_cxx(text, strip_literals=False).splitlines()
    raw_lines = text.splitlines()

    findings: list[Finding] = []
    seen_lines: set[int] = set()
    for m in TYPE_RX.finditer(detect_text):
        lineno = detect_text.count("\n", 0, m.start()) + 1
        if lineno in seen_lines:
            continue
        seen_lines.add(lineno)

        raw_line = raw_lines[lineno - 1] if lineno - 1 < len(raw_lines) else ""
        nocomment_line = nocomment_lines[lineno - 1] if lineno - 1 < len(nocomment_lines) else ""

        # Allow inline `// k2-allow-64bit` or `/* k2-allow-64bit */` inside a comment on this line.
        if "k2-allow-64bit" in raw_line and "k2-allow-64bit" not in nocomment_line:
            continue

        norm = normalize_code(nocomment_line)
        if (relpath, norm) in allow:
            continue

        kind = " ".join(m.group(0).split())
        findings.append(Finding(relpath, lineno, kind, norm or normalize_code(raw_line)))
    return findings


def iter_source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel_dir in SCAN_DIRS:
        base = root / rel_dir
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if not p.is_file() or p.suffix not in SCAN_EXTS:
                continue
            rel_parts = p.relative_to(root).parts
            if "third_party" in rel_parts:
                continue
            files.append(p)
    files.sort(key=lambda p: p.relative_to(root).as_posix())
    return files


def scan_tree(root: Path, allow: set[tuple[str, str]] | None = None) -> list[Finding]:
    findings: list[Finding] = []
    for p in iter_source_files(root):
        relpath = p.relative_to(root).as_posix()
        text = p.read_text(encoding="utf-8", errors="replace")
        findings.extend(scan_text(relpath, text, allow))
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Check firmware sources for unallowlisted 64-bit integer types.")
    ap.add_argument("--mode", choices=("block", "warn", "off"), default="block",
                    help="block (default): exit 1 on hits; warn: print warnings and exit 0; off: skip check")
    ap.add_argument("--root", type=Path, default=ROOT, help="repository root directory")
    ap.add_argument("--allow", type=Path, default=None, help="allowlist file path")
    args = ap.parse_args(argv)

    if args.mode == "off":
        return 0

    allow_path = args.allow if args.allow is not None else args.root / "tools/check_64bit_allow.txt"
    try:
        allow = load_allowlist(allow_path)
    except ValueError as e:
        print(f"check_64bit: {allow_path}: {e}", file=sys.stderr)
        return 1

    findings = scan_tree(args.root, allow)
    if not findings:
        return 0

    sev = "warning" if args.mode == "warn" else "error"
    for f in findings:
        print(f"{f.path}:{f.line}: {sev}: unallowlisted 64-bit type '{f.kind}': {f.code}", file=sys.stderr)

    if args.mode == "block":
        print(
            f"check_64bit: {len(findings)} unallowlisted 64-bit integer use(s) "
            f"(prefer 32-bit / math_opt.hpp, or allowlist in tools/check_64bit_allow.txt; "
            f"bypass with K2_64BIT=warn or K2_64BIT=off)",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
