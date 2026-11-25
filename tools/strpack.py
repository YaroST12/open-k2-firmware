#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""BPE-157 constant string packer: build the global recursive byte-pair table from K2_P literals.

    python3 tools/strpack.py gen            # rewrite include/k2/core/strpack_table.hpp from every K2_P literal
    python3 tools/strpack.py gen --elf E    # bootstrap table from all printable strings in an ELF
    python3 tools/strpack.py stats [--elf E]

Format (include/k2/core/strpack.hpp):
  Byte-aligned, NUL-terminated strings stored in section `k2_strpack`.
  0x00           : NUL terminator
  0x20..0x7E     : literal printable ASCII
  0x09,0x0A,0x0D : literal '\\t', '\\n', '\\r'
  0x7F..0xFF     : BPE rule indices 0..128 (idx = b - 0x7F)
  0x01..0x1F     : BPE rule indices 129..159 (idx = 129 + b - 1; 0x09/0x0A/0x0D unused)
Each rule expands to two bytes (left, right) which may themselves be rules (max depth <= 14).
"""
import argparse
import collections
import os
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TABLE = ROOT / "include/k2/core/strpack_table.hpp"
MAX_DEPTH = 14

# Ordered code IDs: 0x7F..0xFF first (indices 0..128), then 0x01..0x1F skipping \t \n \r (indices 129..159).
CODE_IDS = list(range(0x7F, 0x100)) + [b for b in range(0x01, 0x20) if b not in (0x09, 0x0A, 0x0D)]

LIT_RE = re.compile(r'\bK2_P(?:SV)?\(\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\)')
PIECE_RE = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
ESC_RE = re.compile(rb'\\(x[0-9A-Fa-f]+|[0-7]{1,3}|.)')
SIMPLE = {b'n': 10, b't': 9, b'r': 13, b'0': 0, b'\\': 92, b'"': 34, b"'": 39, b'?': 63, b'a': 7}


def code_to_idx(b: int) -> int:
    return (b - 0x7F) if b >= 0x7F else (129 + b - 1)


def unescape(body: bytes) -> bytes:
    def rep(m):
        e = m.group(1)
        if e[:1] == b'x':
            return bytes([int(e[1:], 16) & 0xFF])
        if e[:1].isdigit() and e != b'0' or len(e) > 1:
            return bytes([int(e, 8) & 0xFF])
        return bytes([SIMPLE[e]])
    return ESC_RE.sub(rep, body)


def corpus_from_source():
    out = []
    for top in ("src", "include/k2"):
        for dp, _, fs in os.walk(ROOT / top):
            for f in sorted(fs):
                if not f.endswith((".cpp", ".hpp", ".h")):
                    continue
                text = (Path(dp) / f).read_bytes().decode("utf-8", "replace")
                for m in LIT_RE.finditer(text):
                    s = b"".join(unescape(p.encode("utf-8")) for p in PIECE_RE.findall(m.group(1)))
                    if all(0x20 <= b < 0x7F or b in (9, 10, 13) for b in s):
                        out.append(s)
    return sorted(set(out))


def elf_strings(path):
    d = open(path, "rb").read()
    shoff, = struct.unpack_from("<I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize) for i in range(shnum)]
    st = secs[shstrndx]
    name = lambda o: d[st[4] + o:d.index(b"\0", st[4] + o)].decode()
    blob = b"".join(d[s[4]:s[4] + s[5]] for s in secs
                    if s[1] == 1 and (s[2] & 2) and (name(s[0]) in (".text", ".rodata") or name(s[0]).startswith(".rodata")))
    return [m.group(0)[:-1] for m in re.finditer(rb"[\x20-\x7e]{4,}\x00", blob)]


def train_bpe(strs, max_rules=len(CODE_IDS)):
    seqs = [list(s) for s in strs]
    rules = {}   # code -> (a, b)
    depth = {b: 0 for b in range(256)}
    for code in CODE_IDS[:max_rules]:
        counts = collections.Counter()
        for seq in seqs:
            for i in range(len(seq) - 1):
                a, b = seq[i], seq[i + 1]
                if max(depth[a], depth[b]) + 1 <= MAX_DEPTH:
                    counts[(a, b)] += 1
        if not counts:
            break
        pair, freq = counts.most_common(1)[0]
        if freq <= 3:  # a table entry costs 2 bytes (+ margin)
            break
        a, b = pair
        rules[code] = pair
        depth[code] = max(depth[a], depth[b]) + 1
        new_seqs = []
        for seq in seqs:
            out = []
            i, n = 0, len(seq)
            while i < n:
                if i + 1 < n and seq[i] == a and seq[i + 1] == b:
                    out.append(code)
                    i += 2
                else:
                    out.append(seq[i])
                    i += 1
            new_seqs.append(out)
        seqs = new_seqs
    return rules


def expand_rules(rules):
    exp = {b: bytes([b]) for b in range(256)}
    # Rules were added in topological order in Python 3.7+ dict
    for code, (a, b) in rules.items():
        exp[code] = exp[a] + exp[b]
    return exp


def dp_encode(strs, rules):
    exp = expand_rules(rules)
    by_first = collections.defaultdict(list)
    for code in rules:
        w = exp[code]
        by_first[w[0]].append((w, code))
    out_strs = []
    for s in strs:
        n = len(s)
        best = [10**9] * (n + 1)
        pick = [None] * (n + 1)
        best[n] = 0
        for i in range(n - 1, -1, -1):
            best[i] = best[i + 1] + 1
            pick[i] = (1, s[i])
            for w, code in by_first.get(s[i], ()):
                wl = len(w)
                if s.startswith(w, i) and best[i + wl] + 1 < best[i]:
                    best[i] = best[i + wl] + 1
                    pick[i] = (wl, code)
        buf = bytearray()
        i = 0
        while i < n:
            wl, c = pick[i]
            buf.append(c)
            i += wl
        out_strs.append(bytes(buf))
    return out_strs


def table_array(rules):
    if not rules:
        return []
    max_idx = max(code_to_idx(c) for c in rules)
    arr = [(0, 0)] * (max_idx + 1)
    for c, pair in rules.items():
        arr[code_to_idx(c)] = pair
    return arr


def emit(rules, source):
    arr = table_array(rules)
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-or-later",
        "// Copyright (C) 2026 Yaraslau Furman",
        f"// Generated by tools/strpack.py gen from {source}: do not edit.",
        "// Global BPE-157 pair table (format: include/k2/core/strpack.hpp). Regenerating only changes the ratio.",
        "#pragma once",
        "#include <cstdint>",
        "",
        "namespace k2::strpack::table {",
        "",
        f"inline constexpr int kMaxDepth = {MAX_DEPTH};",
        f"inline constexpr int kTableLen = {len(arr)};",
        "inline constexpr std::uint8_t kPair[kTableLen ? kTableLen : 1][2] = {",
    ]
    row = []
    for a, b in (arr or [(0, 0)]):
        row.append(f"{{0x{a:02X},0x{b:02X}}}")
        if len(row) == 8:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    lines += ["};", "", "}  // namespace k2::strpack::table", ""]
    TABLE.write_text("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["gen", "stats"])
    ap.add_argument("--elf")
    ap.add_argument("--max-rules", type=int, default=len(CODE_IDS))
    a = ap.parse_args()
    strs = elf_strings(a.elf) if a.elf else corpus_from_source()
    if not strs:
        raise SystemExit("no K2_P literals found")
    rules = train_bpe(strs, a.max_rules)
    enc = dp_encode(strs, rules)
    plain = sum(len(s) + 1 for s in strs)
    packed = sum(len(s) + 1 for s in enc)
    tb = len(table_array(rules)) * 2
    if a.cmd == "gen":
        emit(rules, Path(a.elf).name if a.elf else "the K2_P corpus")
        print(f"{TABLE.relative_to(ROOT)}: {len(strs)} strings, {len(rules)} rules ({tb} B table), "
              f"packed {packed} B + table {tb} B = {packed + tb} B vs {plain} B plain "
              f"(saves {plain - packed - tb} B, {100 * (plain - packed - tb) / plain:.1f}%)")
    else:
        print(f"{a.elf or 'K2_P corpus'}: {len(strs)} strings, {plain} B plain")
        print(f"  {len(rules)} rules: packed {packed} + table {tb} = {packed + tb} B, "
              f"saves {plain - packed - tb} B ({100 * (plain - packed - tb) / plain:.1f}%)")


if __name__ == "__main__":
    main()
