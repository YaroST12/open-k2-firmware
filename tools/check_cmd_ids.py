#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Remote command-id uniqueness across include/k2/core/remote_proto.hpp, tools/k2_remote.py and
tools/web/k2proto.js. Exit 0 = consistent. Rules:
  1. no two names share an id inside one file,
  2. one name has the same id in every file that defines it,
  3. every id of the C++ Cmd enum (and of py/js below 0x80) lies in one of the subsystem ranges declared
     in remote_proto.hpp as "//   range NAME 0xLO-0xHI" lines; py/js ids from 0x80 up are response-only
     stream frames and exempt,
  4. one id belongs to one name across all files (two branches adding different commands with
     the same id show up here after a merge).
Python and JS may carry names the firmware table lacks (stream frames, subset of commands)."""
import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FILES = {"cpp": ROOT / "include/k2/core/remote_proto.hpp", "py": ROOT / "tools/k2_remote.py",
         "js": ROOT / "tools/web/k2proto.js"}


def _snake(name):
    return re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name).upper()


def parse_cpp(text):
    m = re.search(r"enum\s+class\s+Cmd\b[^{]*\{(.*?)\};", text, re.S)
    if not m:
        raise ValueError("enum class Cmd not found")
    body = re.sub(r"//[^\n]*", "", m.group(1))
    out, nxt = {}, 0
    for item in body.split(","):
        item = item.strip()
        if not item:
            continue
        name, _, val = (s.strip() for s in item.partition("="))
        nxt = int(val, 0) if val else nxt
        out[_snake(name)] = nxt
        nxt += 1
    return out


def parse_py(text):
    out = {}
    for node in ast.parse(text).body:
        if not isinstance(node, ast.Assign):
            continue
        tgt, val = node.targets[0], node.value
        names = tgt.elts if isinstance(tgt, ast.Tuple) else [tgt]
        vals = val.elts if isinstance(val, ast.Tuple) else [val]
        if len(names) != len(vals):
            continue
        for n, v in zip(names, vals):
            if (isinstance(n, ast.Name) and n.id.startswith("CMD_") and isinstance(v, ast.Constant)
                    and isinstance(v.value, int)):
                out[n.id[4:]] = v.value
    return out


def parse_js(text):
    m = re.search(r"var\s+CMD\s*=\s*\{(.*?)\}", text, re.S)
    if not m:
        raise ValueError("var CMD not found")
    return {k: int(v, 0) for k, v in re.findall(r"([A-Z_0-9]+)\s*:\s*(0x[0-9A-Fa-f]+|\d+)", m.group(1))}


def parse_ranges(text):
    """[(name, lo, hi)] from the "range NAME 0xLO-0xHI" comment lines; they must not overlap."""
    out = [(n, int(lo, 0), int(hi, 0)) for n, lo, hi in
           re.findall(r"^//\s+range\s+(\w+)\s+(0x[0-9A-Fa-f]+)-(0x[0-9A-Fa-f]+)\s*$", text, re.M)]
    if not out:
        raise ValueError("no `range NAME 0xLO-0xHI` lines in remote_proto.hpp")
    srt = sorted(out, key=lambda r: r[1])
    for a, b in zip(srt, srt[1:]):
        if b[1] <= a[2]:
            raise ValueError(f"ranges {a[0]} and {b[0]} overlap")
    return out


def check_ranges(tables, ranges):
    errs = []
    for f, t in tables.items():
        for name, i in t.items():
            if f != "cpp" and i >= 0x80:
                continue
            if not any(lo <= i <= hi for _, lo, hi in ranges):
                decl = ", ".join(f"{n} 0x{lo:02X}-0x{hi:02X}" for n, lo, hi in ranges)
                errs.append(f"{f}: {name} id 0x{i:02X} is in no declared range ({decl})")
    return errs


def check(tables):
    errs, by_id, by_name = [], {}, {}
    for f, t in tables.items():
        seen = {}
        for name, i in t.items():
            if i in seen:
                errs.append(f"{f}: id 0x{i:02X} used by both {seen[i]} and {name}")
            seen[i] = name
            if name in by_name and by_name[name][1] != i:
                errs.append(f"{name}: id 0x{by_name[name][1]:02X} in {by_name[name][0]} but 0x{i:02X} in {f}")
            by_name.setdefault(name, (f, i))
            if i in by_id and by_id[i][1] != name:
                errs.append(f"id 0x{i:02X}: {by_id[i][1]} in {by_id[i][0]} but {name} in {f}")
            by_id.setdefault(i, (f, name))
    return errs


def main():
    parsers = {"cpp": parse_cpp, "py": parse_py, "js": parse_js}
    tables = {f: parsers[f](p.read_text()) for f, p in FILES.items()}
    errs = check(tables)
    try:
        errs += check_ranges(tables, parse_ranges(FILES["cpp"].read_text()))
    except ValueError as e:
        errs.append(str(e))
    for e in errs:
        print("command id conflict:", e, file=sys.stderr)
    if not errs:
        print(f"command ids ok ({', '.join(f'{f}={len(t)}' for f, t in tables.items())})")
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main())
