#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Guard: constant text in firmware sources goes through K2_P (BPE-157 packing, include/k2/core/strpack.hpp).

Scans src/ and include/k2/ (no third_party/) for plain string literals. A literal passes when it is:
  - inside K2_P(...), static_assert(...), sizeof(...), asm(...), __attribute__((...)), [[...]] or _Pragma(...)
  - the format of K2LOG(fmt, ...) (stripped from flash) or K2_TPUT(buf, fmt, ...) (packs it); other arguments are checked
  - on a preprocessor line other than #define (#include, #pragma, #error, ...), or extern "C" / operator""
  - not packable: non-ASCII bytes, an embedded NUL, a wide/char16/char32 literal
  - shorter than MIN_LEN decoded bytes (below break-even: a 4-byte string packs to 4.7 B with its NUL, plain 5 B)
  - marked on one of its lines with `// k2-plain: <reason>`, or inside `// k2-plain-begin: <reason>` ...
    `// k2-plain-end`, or in a file listed in tools/check_strings_allow.txt (`<relpath>  <reason>`)
A stale allowlist entry (file gone, or nothing left to exempt) fails too, so the list stays small.
"""
import argparse
import re
import sys
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_64bit import NUM_RX, iter_source_files, strip_cxx  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MIN_LEN = 5
EXEMPT_CALLS = {"K2_P", "K2_PSV", "static_assert", "sizeof", "asm", "__asm__", "__asm", "__attribute__", "_Pragma",
                "alignof", "__has_include"}
FORMAT_CALLS = {"K2LOG": 0, "K2_TPUT": 1}   # macro -> argument that is its format (K2LOG: stripped; K2_TPUT packs it)
COMPARE_CALLS = {"same", "eq", "strcmp", "strncmp", "strcasecmp", "memcmp", "memcpy", "strcpy", "strncpy", "starts_with",
                 "ends_with"}
MARK = "k2-plain"

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_RAW = re.compile(r'(u8|[uUL])?R"([^\s()\\]{0,16})\(')
_STR = re.compile(r'(u8|[uUL])?"')
_ESC = re.compile(rb'\\(x[0-9A-Fa-f]+|[0-7]{1,3}|.)', re.S)
_SIMPLE = {b"n": 10, b"t": 9, b"r": 13, b"\\": 92, b'"': 34, b"'": 39, b"?": 63, b"a": 7, b"b": 8, b"f": 12,
           b"v": 11, b"e": 27}
_MARK_RX = re.compile(rf"{MARK}(-begin|-end)?(:\s*(\S.*)?)?")


class Finding(NamedTuple):
    path: str
    line: int
    text: str
    why: str


class Literal(NamedTuple):
    line: int          # line of the first piece (1-based)
    last_line: int
    value: bytes       # adjacent pieces joined, escapes resolved
    wide: bool
    context: str       # "" = plain code; else the reason it never reaches flash as text (call name, "pp", ...)
    start: int         # offsets of the first quote (prefix included) and past the last one in the source text
    end: int


def unescape(body: bytes) -> bytes:
    def rep(m):
        e = m.group(1)
        if e[:1] == b"x":
            return bytes([int(e[1:], 16) & 0xFF])
        if e[:1].isdigit():
            return bytes([int(e, 8) & 0xFF])
        return bytes([_SIMPLE.get(e, e[0])])
    return _ESC.sub(rep, body)


def packable(v: bytes) -> bool:
    return all(0x20 <= b < 0x7F or b in (9, 10, 13) for b in v)


def literals(text: str) -> list[Literal]:
    """Every string literal of a C++ source (comments ignored), adjacent pieces joined, with its context."""
    s = strip_cxx(text, strip_literals=False)
    n = len(s)
    out: list[Literal] = []
    stack: list[list] = []      # [call name or None, commas]
    attr = 0                    # inside [[ ... ]]
    line = 1
    bol = True                  # at the beginning of a line (only blanks so far)
    pp = None                   # directive name while inside a preprocessor line
    prev_ident = None           # identifier just before the current position (blanks skipped)
    pending: list = []          # pieces of the literal being joined: [line, value, wide, start, end]
    pending_ctx = ""

    def flush():
        nonlocal pending
        if pending:
            ctx = pending_ctx
            if not ctx and re.match(r"\s*[!=]=", s[pending[-1][4]:pending[-1][4] + 64]):
                ctx = "compare"
            out.append(Literal(pending[0][0], pending[-1][0], b"".join(p[1] for p in pending),
                               any(p[2] for p in pending), ctx, pending[0][3], pending[-1][4]))
            pending = []

    def context(at):
        if pp is not None and pp != "define":
            return "pp"
        if attr:
            return "attribute"
        if prev_ident in ("extern", "operator"):
            return prev_ident
        for name, commas in reversed(stack):
            if name in EXEMPT_CALLS:
                return name
        if stack and stack[-1][0] in FORMAT_CALLS and stack[-1][1] == FORMAT_CALLS[stack[-1][0]]:
            return stack[-1][0]
        if stack and stack[-1][0] in COMPARE_CALLS:   # compared to or copied as bytes: packing would change them
            return "compare"
        if re.search(r"[!=]=\s*$", s[max(0, at - 64):at]):
            return "compare"
        return ""

    i = 0
    while i < n:
        ch = s[i]
        if ch == "\n":
            if pp is not None and not (i > 0 and s[i - 1] == "\\"):
                pp = None
            line += 1
            bol = True
            i += 1
            continue
        if ch in " \t\r\f\v" or (ch == "\\" and i + 1 < n and s[i + 1] == "\n"):
            i += 1
            continue
        if bol and ch == "#":
            m = _IDENT.match(s, i + 1 + (len(s[i + 1:]) - len(s[i + 1:].lstrip(" \t"))))
            pp = m.group(0) if m else ""
            flush()
            bol = False
            i += 1
            continue
        bol = False
        m = _RAW.match(s, i) if ch in 'uULR' else None
        if m:
            close = ")" + m.group(2) + '"'
            end = s.find(close, m.end())
            end = n if end == -1 else end
            body = s[m.end():end].encode("utf-8")
            if not pending:
                pending_ctx = context(i)
            pending.append([line, body, m.group(1) in ("u", "U", "L"), i, end + len(close)])
            line += s.count("\n", i, end)
            i = end + len(close)
            prev_ident = None
            continue
        m = _STR.match(s, i) if ch in 'uUL"' else None
        if m:
            j = m.end()
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            body = unescape(s[m.end():j].encode("utf-8"))
            if not pending:
                pending_ctx = context(i)
            pending.append([line, body, m.group(1) in ("u", "U", "L"), i, j + 1])
            i = j + 1
            prev_ident = None
            continue
        flush()
        if ch == "'":   # char literal: skip it (a '"' must not open a string)
            j = i + 1
            while j < n and s[j] not in "'\n":
                j += 2 if s[j] == "\\" else 1
            i = j + 1
            prev_ident = None
            continue
        if ch.isdigit():   # numbers, digit separators included
            i = NUM_RX.match(s, i).end()
            prev_ident = None
            continue
        if ch.isalpha() or ch == "_":
            m = _IDENT.match(s, i)
            word = m.group(0)
            # asm volatile ( ... ): the qualifiers keep the call name
            if not (prev_ident in ("asm", "__asm__", "__asm") and word in ("volatile", "__volatile__", "inline", "goto")):
                prev_ident = word
            i = m.end()
            continue
        if ch == "(":
            stack.append([prev_ident, 0])
        elif ch == ")":
            if stack:
                stack.pop()
        elif ch == "," and stack:
            stack[-1][1] += 1
        elif ch == "[" and s.startswith("[[", i):
            attr += 1
            i += 2
            prev_ident = None
            continue
        elif ch == "]" and attr and s.startswith("]]", i):
            attr -= 1
            i += 2
            prev_ident = None
            continue
        prev_ident = None
        i += 1
    flush()
    return out


def wrapped_literals(text: str) -> list[bytes]:
    """Values of the K2_P(...) literals (and K2_TPUT formats) of a source, in order (the pair table's training corpus)."""
    return [lit.value for lit in literals(text) if lit.context in ("K2_P", "K2_PSV", "K2_TPUT")]


def _marks(raw_lines: list[str]) -> tuple[dict[int, str | None], set[int]]:
    """Per line: the k2-plain reason ("" when the marker has none); and the lines inside begin/end regions."""
    marks: dict[int, str | None] = {}
    region: set[int] = set()
    open_at = None
    for no, raw in enumerate(raw_lines, 1):
        m = _MARK_RX.search(raw)
        if not m:
            if open_at is not None:
                region.add(no)
            continue
        kind, reason = m.group(1), (m.group(3) or "").strip()
        if kind == "-end":
            open_at = None
        elif kind == "-begin":
            if reason:
                open_at = no
            else:
                marks[no] = ""
        else:
            marks[no] = reason
    return marks, region


def scan_text(relpath: str, text: str) -> list[Finding]:
    raw_lines = text.splitlines()
    marks, region = _marks(raw_lines)
    found: list[Finding] = []
    for lit in literals(text):
        if lit.context or lit.wide or not packable(lit.value) or len(lit.value) < MIN_LEN:
            continue
        span = range(lit.line, lit.last_line + 1)
        if any(no in region for no in span):
            continue
        reasons = [marks[no] for no in span if no in marks]
        if any(reasons):
            continue
        shown = lit.value.decode("ascii").encode("unicode_escape").decode("ascii")
        why = "k2-plain needs a reason after the colon" if reasons else "plain text literal: wrap it in K2_P(...)"
        found.append(Finding(relpath, lit.line, shown, why))
    return found


def parse_allowlist(text: str) -> dict[str, str]:
    """`<relpath>  <reason>` per line (blank and # lines ignored); a reason is required."""
    out: dict[str, str] = {}
    for lineno, raw in enumerate(text.splitlines(), 1):
        stripped = raw.strip()
        if not stripped or stripped.startswith("#"):
            continue
        parts = stripped.split(None, 1)
        if len(parts) != 2:
            raise ValueError(f"line {lineno}: '<relpath>  <reason>' expected: {raw!r}")
        out[parts[0]] = parts[1].strip()
    return out


def scan_tree(root: Path, allow: dict[str, str] | None = None) -> tuple[list[Finding], list[str]]:
    """Findings outside the allowed files, and the stale allowlist entries."""
    allow = allow or {}
    found: list[Finding] = []
    used: set[str] = set()
    for p in iter_source_files(root):
        rel = p.relative_to(root).as_posix()
        hits = scan_text(rel, p.read_text(encoding="utf-8", errors="replace"))
        if rel in allow:
            if hits:
                used.add(rel)
            continue
        found.extend(hits)
    return found, sorted(set(allow) - used)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Fail on plain string literals that should be K2_P-packed.")
    ap.add_argument("--root", type=Path, default=ROOT, help="repository root")
    ap.add_argument("--allow", type=Path, default=None, help="allowlist (default tools/check_strings_allow.txt)")
    args = ap.parse_args(argv)
    allow_path = args.allow or args.root / "tools/check_strings_allow.txt"
    try:
        allow = parse_allowlist(allow_path.read_text(encoding="utf-8")) if allow_path.exists() else {}
    except ValueError as e:
        print(f"check_strings: {allow_path}: {e}", file=sys.stderr)
        return 1
    found, stale = scan_tree(args.root, allow)
    for f in found:
        print(f'{f.path}:{f.line}: error: {f.why}: "{f.text}"', file=sys.stderr)
    for rel in stale:
        print(f"{allow_path.name}: stale entry {rel} (no plain literal left to exempt): remove it", file=sys.stderr)
    if found or stale:
        print(f"check_strings: {len(found)} plain literal(s), {len(stale)} stale allowlist entr(y/ies): wrap text in "
              f"K2_P(...), or mark a literal that must stay plain with // k2-plain: <reason>", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
