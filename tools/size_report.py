#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Where the flash goes: the heaviest functions of build/open_k2.elf, grouped by source module, plus what is
inlined into them.

    python3 tools/size_report.py                    # report for build/open_k2.elf (docker nm / objdump, needs the build)
    python3 tools/size_report.py --top 60 --inline-top 25
    python3 tools/size_report.py --build build-exp  # an experiment build directory (BUILD_DIR=build-exp)
    python3 tools/size_report.py --diff runs/size/A/functions.tsv runs/size/B/functions.tsv

The report goes to stdout and into runs/size/<git describe>/ (gitignored):
  summary.txt    the same text as stdout
  functions.tsv  every code symbol: size, kind, name, source file:line (input of --diff)
  modules.tsv    text bytes per source directory and per source file (by where the function is defined)
  inline.tsv     per heavy function, bytes by the source file the instructions come from (DWARF line table of
                 the LTO output): what is NOT the function's own file is inlined or merged in

`nm -l` places a symbol at its definition; LTO merges callees into callers, so the module table is the honest
"who owns the bytes" only up to inlining. The inline table is the attribution of those bytes: a 14 KB function
that is 9 KB from three other files shows what an inline limit or a noinline would move. Function sizes include
literal pools. No ARM toolchain on the host: nm and objdump run in the build image like tools/gate.py does.
"""
import argparse
import collections
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGE = "open-k2-builder:latest"
WORKSPACE = "/workspace/"
FLASH_LO, FLASH_HI = 0x08000000, 0x08100000

NM_RE = re.compile(r"^([0-9a-f]+) ([0-9a-f]+) (\w) (.*?)(?:\t(\S+?):(\d+))?$")


class Sym:
    __slots__ = ("addr", "size", "kind", "name", "path", "line")

    def __init__(self, addr, size, kind, name, path, line):
        self.addr, self.size, self.kind, self.name, self.path, self.line = addr, size, kind, name, path, line


def rel(path):
    return path[len(WORKSPACE):] if path.startswith(WORKSPACE) else path


def parse_nm(text):
    """`nm -S -C -l` lines: `addr size kind name[\\tfile:line]`; symbols without a size are skipped."""
    out = []
    for ln in text.splitlines():
        m = NM_RE.match(ln)
        if not m:
            continue
        a, s, k, name, path, line = m.groups()
        out.append(Sym(int(a, 16), int(s, 16), k, name, rel(path) if path else None, int(line) if line else 0))
    return out


def code_syms(syms):
    """Symbols that are code in flash (T/t, and the weak ones), not data or bss."""
    return [s for s in syms if s.kind in "TtWw" and FLASH_LO <= s.addr < FLASH_HI]


def flash_syms(syms):
    """Every flash symbol (code and read-only data) by size, for the 'not code' tail of the report."""
    return [s for s in syms if FLASH_LO <= s.addr < FLASH_HI]


def group_of(path):
    """Source directory of a definition: src/ui, include/k2/core, third_party/littlefs, ..."""
    if not path:
        return "(no source)"
    parts = path.split("/")
    if parts[0] == "third_party":
        return "/".join(parts[:2])
    if parts[0] == "include":
        return "/".join(parts[:3]) if len(parts) > 3 else "include"
    return "/".join(parts[:2]) if len(parts) > 2 else parts[0]


def short(name, n=96):
    name = re.sub(r"\(anonymous namespace\)::", "", name)
    name = re.sub(r"std::span<([^>]*?), 4294967295u>", r"span<\1>", name)
    return name if len(name) <= n else name[: n - 3] + "..."


INSTR_RE = re.compile(r"^\s*([0-9a-f]+):\t")
HEAD_RE = re.compile(r"^([0-9a-f]{8}) <.*>:$")
LINE_RE = re.compile(r"^(/\S+?|[^\s:]\S*?):(\d+)(?: \(discriminator \d+\))?$")


def parse_objdump(text, sizes=None):
    """`objdump -d -l --no-show-raw-insn` -> {function start addr: {source file: bytes}}. An instruction belongs to
    the source file of the last line-table entry before it; an inlined body therefore shows under its own file."""
    sizes = sizes or {}
    per = collections.defaultdict(lambda: collections.defaultdict(int))
    func = None
    fname = None
    prev = None                    # (addr, file) of the last instruction, whose size is the gap to the next
    def close(end):
        nonlocal prev
        if prev is not None and func is not None:
            sz = sizes.get(func)
            last_end = func + sz if sz is not None else end
            per[func][prev[1]] += max(0, (last_end if last_end is not None else prev[0]) - prev[0])
        prev = None
    for ln in text.splitlines():
        m = HEAD_RE.match(ln)
        if m:
            addr = int(m.group(1), 16)
            close(addr)
            func, fname = addr, None
            per[func]                       # a function with no line info still exists
            continue
        if func is None:
            continue
        m = INSTR_RE.match(ln)
        if m:
            addr = int(m.group(1), 16)
            if prev is not None:
                per[func][prev[1]] += addr - prev[0]
            prev = (addr, fname or "(no line info)")
            continue
        m = LINE_RE.match(ln)
        if m:
            fname = rel(m.group(1))
    close(None)
    return {f: dict(v) for f, v in per.items()}


def diff_sizes(a, b):
    """[(name, size a, size b, delta)] for every name whose size changed, biggest change first."""
    rows = [(n, a.get(n, 0), b.get(n, 0), b.get(n, 0) - a.get(n, 0)) for n in set(a) | set(b)]
    return sorted((r for r in rows if r[3]), key=lambda r: (-abs(r[3]), r[0]))


def sizes_by_name(syms):
    out = collections.defaultdict(int)
    for s in code_syms(syms):
        out[re.sub(r" \[clone [^\]]*\]", "", s.name)] += s.size
    return out


def load_tsv(p):
    out = collections.defaultdict(int)
    for ln in Path(p).read_text().splitlines():
        f = ln.split("\t")
        if len(f) >= 4 and f[0] != "size":
            out[re.sub(r" \[clone [^\]]*\]", "", f[2])] += int(f[0])
    return out


def docker(*args):
    uid = subprocess.run(["id", "-u"], capture_output=True, text=True).stdout.strip()
    gid = subprocess.run(["id", "-g"], capture_output=True, text=True).stdout.strip()
    r = subprocess.run(["docker", "run", "--rm", "--user", f"{uid}:{gid}", "-v", f"{ROOT}:/workspace", "-w", "/workspace",
                        IMAGE, *args], capture_output=True, text=True, errors="replace")
    if r.returncode:
        sys.exit(f"{args[0]} failed: {r.stderr.strip()[:400]}")
    return r.stdout


def table(rows, head):
    w = [max(len(str(r[i])) for r in rows + [head]) for i in range(len(head))]
    # numbers right-aligned, text left-aligned
    fmt = lambda r: "  ".join(str(c).rjust(w[i]) if re.fullmatch(r"[-+]?[\d.]+%?|bytes|share|delta|old|new|copies", str(c))
                              else str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip()
    return [fmt(head)] + [fmt(r) for r in rows]


def report(syms, inline, top, inline_top, describe):
    code = code_syms(syms)
    total = sum(s.size for s in code)
    out = [f"size report {describe}: {len(code)} code symbols, {total} B of function text "
           f"(literal pools included; .rodata and the gaps are not in this sum)", ""]
    out.append(f"== top {top} functions")
    rows = [[s.size, f"{100 * s.size / total:.1f}%", short(s.name), f"{s.path}:{s.line}" if s.path else "-"]
            for s in sorted(code, key=lambda s: -s.size)[:top]]
    out += table(rows, ["bytes", "share", "function", "defined at"]) + [""]
    byg = collections.defaultdict(int)
    byf = collections.defaultdict(int)
    for s in code:
        byg[group_of(s.path)] += s.size
        byf[s.path or "(no source)"] += s.size
    out.append("== text by source directory (where the function is defined)")
    out += table([[v, f"{100 * v / total:.1f}%", k] for k, v in sorted(byg.items(), key=lambda kv: -kv[1])[:25]],
                 ["bytes", "share", "directory"]) + [""]
    out.append("== text by source file, top 30")
    out += table([[v, f"{100 * v / total:.1f}%", k] for k, v in sorted(byf.items(), key=lambda kv: -kv[1])[:30]],
                 ["bytes", "share", "file"]) + [""]
    dup = collections.defaultdict(list)
    for s in code:
        dup[re.sub(r" \[clone [^\]]*\]", "", s.name)].append(s.size)
    d = [(sum(v), len(v), k) for k, v in dup.items() if len(v) > 1 and sum(v) > 300]
    if d:
        out.append("== several copies of one function (clones and instances, total over 300 B)")
        out += table([[a, b, short(c)] for a, b, c in sorted(d, reverse=True)[:15]], ["bytes", "copies", "function"]) + [""]
    if inline:
        out.append(f"== what the {inline_top} heaviest functions are made of (bytes by source file of the instructions;")
        out.append("   the function's own file first, then what was inlined or merged in)")
        shown = 0
        for s in sorted(code, key=lambda s: -s.size):
            per = inline.get(s.addr)
            if not per or shown >= inline_top:
                continue
            shown += 1
            own = s.path or ""
            parts = sorted(per.items(), key=lambda kv: -kv[1])
            tot = sum(per.values()) or 1
            foreign = tot - per.get(own, 0)
            out.append(f"{s.size:6d} B  {short(s.name, 80)}  ({own}; {foreign} B = {100 * foreign // tot}% from other files)")
            out += [f"          {v:6d}  {k}" for k, v in parts[:5] if k != own]
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--build", default="build", help="build directory with open_k2.elf (default build)")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--inline-top", type=int, default=15, help="functions to break down by inlined origin")
    ap.add_argument("--no-inline", action="store_true", help="skip the objdump pass (about 15 s)")
    ap.add_argument("--out", help="output directory (default runs/size/<git describe>)")
    ap.add_argument("--nm", help="use this saved `nm -S -C -l` output instead of running docker")
    ap.add_argument("--diff", nargs=2, metavar=("OLD", "NEW"), help="compare two functions.tsv files and exit")
    a = ap.parse_args()
    if a.diff:
        old, new = load_tsv(a.diff[0]), load_tsv(a.diff[1])
        rows = diff_sizes(old, new)
        print(f"text of functions: {sum(old.values())} -> {sum(new.values())} ({sum(new.values()) - sum(old.values()):+d} B)")
        print("\n".join(table([[d, o, n, short(k)] for k, o, n, d in rows[:40]], ["delta", "old", "new", "function"])))
        return 0
    elf = f"{a.build}/open_k2.elf"
    if not a.nm and not (ROOT / elf).exists():
        sys.exit(f"{elf} not found: run `make docker-build` first")
    describe = subprocess.run(["git", "describe", "--always", "--dirty"], cwd=ROOT, capture_output=True,
                              text=True).stdout.strip() or "unknown"
    syms = parse_nm(Path(a.nm).read_text() if a.nm else docker("arm-none-eabi-nm", "-S", "-C", "-l", elf))
    inline = {}
    if not a.no_inline and not a.nm:
        od = docker("arm-none-eabi-objdump", "-d", "-l", "-C", "--no-show-raw-insn", "-j", ".text", elf)
        inline = parse_objdump(od, {s.addr: s.size for s in code_syms(syms)})
    text = report(syms, inline, a.top, a.inline_top, describe)
    out = Path(a.out) if a.out else ROOT / "runs/size" / describe
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.txt").write_text(text + "\n")
    code = sorted(code_syms(syms), key=lambda s: -s.size)
    (out / "functions.tsv").write_text("size\tkind\tname\tsource\n" + "".join(
        f"{s.size}\t{s.kind}\t{s.name}\t{s.path or '-'}:{s.line}\n" for s in code))
    mod = collections.defaultdict(int)
    for s in code:
        mod[(group_of(s.path), s.path or "-")] += s.size
    (out / "modules.tsv").write_text("bytes\tdirectory\tfile\n" + "".join(
        f"{v}\t{g}\t{p}\n" for (g, p), v in sorted(mod.items(), key=lambda kv: -kv[1])))
    if inline:
        rows = []
        for s in code[:200]:
            for f, v in sorted(inline.get(s.addr, {}).items(), key=lambda kv: -kv[1]):
                rows.append(f"{s.name}\t{s.size}\t{f}\t{v}\n")
        (out / "inline.tsv").write_text("function\tsize\tfrom_file\tbytes\n" + "".join(rows))
    print(text)
    print(f"written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
