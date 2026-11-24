#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Image size tracking: snapshot an ELF, then explain what changed between two snapshots.

    python3 tools/size_track.py snap [--build build]        # snapshot build/open_k2.elf (a few seconds, host llvm tools)
    python3 tools/size_track.py diff [A] [B]                # A, B: commit, snapshot file or build dir; default HEAD~1 HEAD
    python3 tools/size_track.py diff --build build          # HEAD's snapshot vs the build dir as it is now
    python3 tools/size_track.py hook                        # post-commit: snapshot if the build is HEAD, diff vs parent
    python3 tools/size_track.py backfill REV [REV...]       # build REV in a scratch worktree and snapshot it

A snapshot holds every symbol in flash and RAM, the bytes of code per originating source file (DWARF line table:
an inlined body counts for the file it was written in), and one record per inlined callee (copies, bytes, hosts,
from DW_TAG_inlined_subroutine). The diff reconciles: the flash delta equals functions + flash objects + the bytes
no symbol covers (string literals, alignment padding), so every byte of growth is placed somewhere.

Snapshots live in <main checkout>/runs/size-track/ (shared by all worktrees), keyed by the image's version record
(commit, -dirty) and the build-config stamp. Needs llvm-nm, llvm-readelf, llvm-dwarfdump, llvm-cxxfilt on the host.
"""
import argparse
import bisect
import collections
import gzip
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLASH_LO, FLASH_HI = 0x08000000, 0x08100000
RAM_LO, RAM_HI = 0x20000000, 0x20020000
HOT_END = 0x08040000            # zero-wait window
APP_END = 0x080B3000            # link.ld: littlefs starts here
WORKSPACE = "/workspace/"
SCHEMA = 1


def git(*args, cwd=ROOT, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        raise SystemExit(f"git {' '.join(args)}: {r.stderr.strip()}")
    return r.stdout.strip()


def store_dir():
    common = Path(git("rev-parse", "--path-format=absolute", "--git-common-dir"))
    d = common.parent / "runs" / "size-track"
    d.mkdir(parents=True, exist_ok=True)
    return d


def rel(p):
    if not p:
        return p
    if p.startswith(WORKSPACE):
        return p[len(WORKSPACE):]
    if "/arm-none-eabi/" in p or p.startswith("/usr/"):
        return "(toolchain)/" + p.rsplit("/", 1)[-1]
    return p


CLONE_RE = re.compile(r"\.(lto_priv|constprop|isra|part|cold|localalias)\.\d+")
CLONE_DEM_RE = re.compile(r" \[clone \.(lto_priv|constprop|isra|part|cold|localalias)\.\d+\]")


def norm(name):
    """Renumbering of LTO/IPA clones is noise; the clone kind is kept."""
    name = CLONE_DEM_RE.sub(lambda m: f" [clone .{m.group(1)}]", name)
    return CLONE_RE.sub(lambda m: f".{m.group(1)}", name)


def base_name(name):
    """The function a clone was made from (an ' @file' qualifier dropped)."""
    return re.sub(r" \[clone [^\]]*\]", "", CLONE_RE.sub("", name.split(" @", 1)[0]))


def run(*cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode:
        raise SystemExit(f"{cmd[0]} failed: {r.stderr.strip()[:300]}")
    return r.stdout


def demangle(names):
    names = list(names)
    if not names:
        return {}
    tool = "c++filt" if subprocess.run(["sh", "-c", "command -v c++filt"], capture_output=True).returncode == 0 else "llvm-cxxfilt"
    r = subprocess.run([tool], input="\n".join(names) + "\n", capture_output=True, text=True)
    out = r.stdout.split("\n")
    return {n: (out[i] if i < len(out) and out[i] else n) for i, n in enumerate(names)}


# ---------------------------------------------------------------------------------------------------------- snapshot

SEC_RE = re.compile(r"^\s*\[\s*(\d+)\]\s+(\S+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+\S+\s+(\S*)\s")
SYM_RE = re.compile(r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+(\w+)\s+(\w+)\s+\w+\s+(\w+)\s+(\S+)$")
NM_RE = re.compile(r"^([0-9a-f]+) (?:[0-9a-f]+ )?\w (\S+)\t(\S+):(\d+)$")


def read_sections(elf):
    secs = {}
    for ln in run("llvm-readelf", "-S", "-W", str(elf)).splitlines():
        m = SEC_RE.match(ln)
        if m and "A" in m.group(7):
            idx, name, typ, addr, off, size, _ = m.groups()
            secs[int(idx)] = (name, int(addr, 16), int(size, 16), typ, int(off, 16))
    return secs


def read_symbols(elf, secs):
    raw = []
    for ln in run("llvm-readelf", "-s", "-W", str(elf)).splitlines():
        m = SYM_RE.match(ln)
        if not m:
            continue
        val, size, typ, bind, ndx, name = m.groups()
        if typ not in ("FUNC", "OBJECT") or not ndx.isdigit() or int(ndx) not in secs or int(size) == 0:
            continue
        raw.append((int(val, 16) & ~1, int(size), "F" if typ == "FUNC" else "O", bind[0], secs[int(ndx)][0], name))
    loc = {}
    for ln in run("llvm-nm", "-l", "--defined-only", str(elf)).splitlines():
        m = NM_RE.match(ln)
        if m:
            loc[(int(m.group(1), 16) & ~1, m.group(2))] = rel(m.group(3))
    dem = demangle({r[5] for r in raw})
    syms = []
    for addr, size, kind, bind, sec, mangled in raw:
        syms.append({"n": norm(dem[mangled]), "k": kind, "s": size, "a": addr, "sec": sec, "b": bind,
                     "f": loc.get((addr, mangled)), "m": mangled})
    return syms


def read_lines(elf):
    """Line-table rows -> [(start, end, file)] ranges of code, from the DWARF line program of every CU."""
    ranges = []
    dirs, files = {}, {}
    cur_file = None
    prev = None
    p = subprocess.Popen(["llvm-dwarfdump", "--debug-line", str(elf)], stdout=subprocess.PIPE, text=True, errors="replace")
    for ln in p.stdout:
        if ln.startswith("0x"):
            parts = ln.split()
            addr, fidx = int(parts[0], 16), int(parts[3])
            if prev is not None and addr > prev[0]:
                ranges.append((prev[0], addr, prev[1]))
            prev = None if "end_sequence" in ln else (addr, files.get(fidx, "?"))
        elif ln.startswith("include_directories["):
            i = int(ln[20:ln.index("]")])
            dirs[i] = ln.split("= ", 1)[1].strip().strip('"')
        elif ln.startswith("file_names["):
            cur_file = int(ln[11:ln.index("]")])
        elif cur_file is not None and ln.lstrip().startswith("name:"):
            files[cur_file] = ln.split(":", 1)[1].strip().strip('"')
        elif cur_file is not None and ln.lstrip().startswith("dir_index:"):
            d = dirs.get(int(ln.split(":", 1)[1]), "")
            n = files[cur_file]
            files[cur_file] = rel(n if n.startswith("/") or not d else f"{d}/{n}")
            cur_file = None
        elif ln.startswith("debug_line["):
            dirs, files, prev = {0: ""}, {}, None
    p.wait()
    return ranges


TAG_RE = re.compile(r"^0x([0-9a-f]+):(\s+)DW_TAG_(\w+)")
ORIGIN_RE = re.compile(r'DW_AT_abstract_origin\s+\(0x([0-9a-f]+) "([^"]*)"\)')
DECL_RE = re.compile(r'DW_AT_decl_file\s+\("([^"]*)"\)')
LOW_RE = re.compile(r"DW_AT_low_pc\s+\(0x([0-9a-f]+)\)")
HIGH_RE = re.compile(r"DW_AT_high_pc\s+\(0x([0-9a-f]+)\)")
RANGE_RE = re.compile(r"^\s+\[0x([0-9a-f]+), 0x([0-9a-f]+)\)\)?$")


def read_inlines(elf):
    """DW_TAG_inlined_subroutine instances: [(callee, first pc, exclusive bytes)]. Exclusive = the instance's
    ranges minus those of the instances nested in it, so the bytes sum without double counting. A callee DWARF
    names without a scope (a lambda's operator(), a local) is named by where it lives: 'operator() in f @file'."""
    p = subprocess.Popen(
        "llvm-dwarfdump --debug-info " + json.dumps(str(elf)) +
        " | grep -E 'DW_TAG_|DW_AT_(abstract_origin|low_pc|high_pc|ranges|specification|decl_file|linkage_name)|"
        "^ +\\[0x[0-9a-f]+, 0x[0-9a-f]+\\)\\)?$'",
        shell=True, stdout=subprocess.PIPE, text=True, errors="replace")
    out = []
    ctx = []            # every open DIE: [depth, offset, tag, name, file]
    inl_stack = []      # (depth, inlined record) for nesting
    where = {}          # subprogram offset -> (enclosing function name, file)
    spec = {}           # subprogram offset -> the DIE it specifies / is an instance of
    decl = {}           # any DIE offset -> its decl_file
    cur = None
    die = None
    in_ranges = False
    for ln in p.stdout:
        m = TAG_RE.match(ln)
        if m:
            in_ranges = False
            off, depth, tag = int(m.group(1), 16), len(m.group(2)), m.group(3)
            while ctx and ctx[-1][0] >= depth:
                ctx.pop()
            while inl_stack and inl_stack[-1][0] >= depth:
                inl_stack.pop()
            die = [depth, off, tag, None, None]
            if tag == "subprogram":
                outer = next((c[3] for c in reversed(ctx) if c[2] == "subprogram" and c[3]), None)
                fil = next((c[4] for c in reversed(ctx) if c[4]), None)
                where[off] = [outer, fil]
            ctx.append(die)
            cur = None
            if tag == "inlined_subroutine":
                cur = {"callee": None, "origin": None, "lo": None, "hi": None, "ranges": [],
                       "parent": inl_stack[-1][1] if inl_stack else None, "child": 0}
                inl_stack.append((depth, cur))
                out.append(cur)
            continue
        if die is None:
            continue
        if in_ranges:
            r = RANGE_RE.match(ln)
            if r:
                if cur is not None:
                    cur["ranges"].append((int(r.group(1), 16), int(r.group(2), 16)))
                continue
            in_ranges = False
        if "DW_AT_decl_file" in ln:
            mm = DECL_RE.search(ln)
            if mm:
                die[4] = rel(mm.group(1))
                decl[die[1]] = die[4]
                if die[1] in where and not where[die[1]][1]:
                    where[die[1]][1] = die[4]
        elif "DW_AT_specification" in ln or "DW_AT_linkage_name" in ln or ("DW_AT_abstract_origin" in ln and cur is None):
            mm = re.search(r'"([^"]*)"', ln)
            if mm and not die[3]:
                die[3] = mm.group(1)
            if die[1] in where and "DW_AT_linkage_name" not in ln:
                ref = re.search(r"\(0x([0-9a-f]+)", ln)
                if ref:
                    spec[die[1]] = int(ref.group(1), 16)
        if cur is None:
            continue
        if "DW_AT_abstract_origin" in ln:
            mm = ORIGIN_RE.search(ln)
            if mm:
                cur["origin"], cur["callee"] = int(mm.group(1), 16), mm.group(2)
        elif "DW_AT_low_pc" in ln:
            cur["lo"] = int(LOW_RE.search(ln).group(1), 16)
        elif "DW_AT_high_pc" in ln:
            cur["hi"] = int(HIGH_RE.search(ln).group(1), 16)
        elif "DW_AT_ranges" in ln:
            in_ranges = True
    p.wait()
    for rec in out:
        if rec["ranges"]:
            rec["incl"] = sum(b - a for a, b in rec["ranges"])
            rec["lo"] = min(a for a, _ in rec["ranges"])
        elif rec["lo"] is not None and rec["hi"] is not None:
            rec["incl"] = rec["hi"] - rec["lo"]
        else:
            rec["incl"] = 0
    for rec in out:
        if rec["parent"] is not None:
            rec["parent"]["child"] += rec["incl"]
    dem = demangle({w[0] for w in where.values() if w[0] and w[0].startswith("_Z")})
    res = []
    for r in out:
        if not (r["callee"] and r["incl"]):
            continue
        name = r["callee"]
        if not name.startswith("_Z") and "::" not in name:
            outer, fil = where.get(r["origin"], (None, None))
            o = r["origin"]
            for _ in range(4):
                if fil or o is None:
                    break
                fil = decl.get(o) or (where.get(o) or (None, None))[1]
                o = spec.get(o)
            if outer:
                outer = fn_path(dem.get(outer, outer)).replace("(anonymous namespace)::", "")
                name = f"{name} in {outer}"
            if fil:
                name = f"{name} @{fil}"
        res.append((name, r["lo"], max(0, r["incl"] - r["child"])))
    return res


def fn_path(name):
    """'ns::f<T>(args) const' -> 'ns::f<T>': cut at the first '(' outside template brackets."""
    depth = 0
    for i, ch in enumerate(name):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "(" and depth == 0 and not name.startswith("(anonymous", i):
            return name[:i]
    return name


def version_record(elf, secs):
    """The K2FW record at the start of .text: 'K2FW<version>' NUL-padded."""
    for name, addr, size, typ, off in secs.values():
        if name == ".text":
            with open(elf, "rb") as f:
                f.seek(off)
                rec = f.read(48)
            if rec[:4] == b"K2FW":
                return rec[4:].split(b"\0", 1)[0].decode(errors="replace")
    return "unknown"


def build_config(build_dir):
    stamps = sorted(Path(build_dir).glob(".cfg-*"), key=lambda p: p.stat().st_mtime)
    return stamps[-1].name[5:13] if stamps else "unknown"


def snapshot(build_dir):
    elf = Path(build_dir) / "open_k2.elf"
    if not elf.exists():
        raise SystemExit(f"{elf} not found: build first (make docker-build)")
    t0 = time.time()
    secs = read_sections(elf)
    syms = read_symbols(elf, secs)
    lines = read_lines(elf)
    inl = read_inlines(elf)
    ver = version_record(elf, secs)
    comment = run("llvm-readelf", "-p", ".comment", str(elf))
    tool = " ".join(sorted(set(re.findall(r"GCC: \([^)]*\) [\d.]+", comment)))) or "unknown"

    funcs = sorted((s for s in syms if s["k"] == "F" and FLASH_LO <= s["a"] < FLASH_HI), key=lambda s: s["a"])
    starts = [s["a"] for s in funcs]

    def owner(addr):
        i = bisect.bisect_right(starts, addr) - 1
        if i >= 0 and addr < funcs[i]["a"] + funcs[i]["s"]:
            return funcs[i]
        return None

    origin = collections.Counter()
    composition = collections.defaultdict(collections.Counter)
    for a, b, f in lines:
        origin[f] += b - a
        o = owner(a)
        if o is not None:
            composition[o["n"] + "|" + (o["f"] or "")][f] += b - a
    inline = {}
    dem = demangle({c for c, _, _ in inl if c.startswith("_Z")})
    for callee, lo, excl in inl:
        name = norm(dem.get(callee, callee))
        host = owner(lo)
        rec = inline.setdefault(name, {"copies": 0, "bytes": 0, "hosts": collections.Counter()})
        rec["copies"] += 1
        rec["bytes"] += excl
        rec["hosts"][host["n"] if host else "?"] += excl
    regions = {name: {"addr": addr, "size": size, "nobits": typ == "NOBITS"} for name, addr, size, typ, off in secs.values()}
    flash_end = max(a + s for n, a, s, t, o in secs.values() if FLASH_LO <= a < FLASH_HI and t != "NOBITS")
    data_init = sum(s for n, a, s, t, o in secs.values() if RAM_LO <= a < RAM_HI and t != "NOBITS")
    hot = sum(min(s["a"] + s["s"], HOT_END) - s["a"] for s in funcs if s["a"] < HOT_END)
    commit = re.search(r"-([0-9a-f]{7,})(-dirty)?$", ver)
    first = min(a for n, a, s, t, o in secs.values() if FLASH_LO <= a < FLASH_HI)
    snap = {
        "schema": SCHEMA,
        "meta": {"version": ver, "commit": commit.group(1) if commit else None, "dirty": bool(commit and commit.group(2)),
                 "cfg": build_config(build_dir), "toolchain": tool, "elf": str(elf), "taken": time.strftime("%Y-%m-%d %H:%M:%S"),
                 "elf_mtime": elf.stat().st_mtime,
                 "priv": (Path(build_dir).resolve().parent / "priv").is_dir()},
        "sections": regions,
        "totals": {"flash": flash_end - first + data_init, "flash_end": flash_end, "data_init": data_init, "hot_code": hot,
                   "free": APP_END - flash_end - data_init},
        "syms": [[s["n"], s["k"], s["s"], s["f"], s["sec"], s["a"]] for s in syms],
        "origin": dict(origin),
        "composition": {k: dict(v) for k, v in composition.items()},
        "inline": {k: {"copies": v["copies"], "bytes": v["bytes"], "hosts": dict(v["hosts"])} for k, v in inline.items()},
    }
    snap["meta"]["seconds"] = round(time.time() - t0, 1)
    return snap


def snap_path(snap):
    m = snap["meta"]
    return store_dir() / f"{m['version']}_{m['cfg']}.json.gz"


def save(snap, path=None):
    p = Path(path) if path else snap_path(snap)
    with gzip.open(p, "wt") as f:
        json.dump(snap, f, separators=(",", ":"))
    return p


def load(path):
    with gzip.open(path, "rt") as f:
        return json.load(f)


def resolve(ref):
    """A snapshot from: a .json.gz path, a build directory (snapshotted now), or a commit-ish (stored, clean)."""
    p = Path(ref)
    if p.suffix == ".gz" and p.exists():
        return load(p)
    if p.is_dir() and (p / "open_k2.elf").exists():
        return snapshot(p)
    sha = git("rev-parse", "--short=8", ref + "^{commit}", check=False)
    if not sha:
        raise SystemExit(f"{ref}: not a snapshot, build dir or commit")
    cands = sorted(store_dir().glob(f"*-{sha}_*.json.gz"), key=lambda q: q.stat().st_mtime)
    if not cands:
        cands = sorted(store_dir().glob(f"*-{sha}-ovf_*.json.gz"), key=lambda q: q.stat().st_mtime)
    if not cands:
        cands = sorted(store_dir().glob(f"*-{sha}-dirty_*.json.gz"), key=lambda q: q.stat().st_mtime)
        if cands:
            print(f"note: {ref} has only a dirty snapshot ({cands[-1].name})", file=sys.stderr)
    if not cands:
        raise SystemExit(f"no snapshot for {ref} ({sha}): run 'size_track.py backfill {ref}', or build it and 'snap'")
    return load(cands[-1])


# -------------------------------------------------------------------------------------------------------------- diff

def short(name, n=78):
    name = name.replace("(anonymous namespace)::", "")
    name = re.sub(r"std::span<([^>]*?), 4294967295u>", r"span<\1>", name)
    name = re.sub(r"\((?:[^()]|\([^()]*\))*\)", lambda m: "(...)" if len(m.group(0)) > 24 else m.group(0), name)
    return name if len(name) <= n else name[: n - 3] + "..."


def table(rows, head):
    if not rows:
        return []
    w = [max(len(str(r[i])) for r in rows + [head]) for i in range(len(head))]
    num = re.compile(r"[-+]?\d+%?|[-+]?\d+\.\d+%?")
    return ["  ".join(str(c).rjust(w[i]) if num.fullmatch(str(c)) else str(c).ljust(w[i]) for i, c in enumerate(r)).rstrip()
            for r in [head] + rows]


def sgn(v):
    return f"{v:+d}"


LOCAL_NUM_RE = re.compile(r"^((?:CSWTCH|C\.\d+\.\d+|[A-Za-z_]\w*))\.\d+((?:\.lto_priv)?)$")


def obj_key(name):
    """Compiler-numbered locals (CSWTCH.834, pps.4, C.0.0.lto_priv.3) renumber on unrelated edits: drop the number."""
    m = LOCAL_NUM_RE.match(name)
    return f"{m.group(1)}.N{m.group(2)}" if m else name


def keyed(snap, pred):
    out = {}
    for n, k, s, f, sec, a in snap["syms"]:
        if k == "O":
            n = obj_key(n)
        if pred(k, a):
            key = n + "|" + (f or "")
            if key in out:
                out[key] = (out[key][0] + s, f, sec, a)
            else:
                out[key] = (s, f, sec, a)
    return out


def is_flash(a):
    return FLASH_LO <= a < FLASH_HI


def changed_files(a, b):
    ca, cb = a["meta"].get("commit"), b["meta"].get("commit")
    if not ca or not cb:
        return None
    out = git("diff", "--name-only", f"{ca}..{cb}", check=False)
    files = set(out.split()) if out else set()
    if b["meta"]["dirty"]:
        files |= set(git("diff", "--name-only", "HEAD", check=False).split())
    return files


def diff(a, b, top=25):
    out = []
    ma, mb = a["meta"], b["meta"]
    out.append(f"size diff  {ma['version']} (cfg {ma['cfg']})  ->  {mb['version']} (cfg {mb['cfg']})")
    warn = []
    if ma["cfg"] != mb["cfg"]:
        warn.append("build configs differ (flags/Makefile switches): part of the delta is the config, not the code")
    if ma["toolchain"] != mb["toolchain"]:
        warn.append(f"toolchains differ: {ma['toolchain']} vs {mb['toolchain']}")
    for m, tag in ((ma, "old"), (mb, "new")):
        if m.get("overflow"):
            warn.append(f"{tag} does NOT fit: FLASH overflowed by {m['overflow']} B (measured with a +64 KB FLASH link)")
    if ma.get("priv") != mb.get("priv"):
        warn.append("one build has priv/ key tables and the other the zero stubs")
    for m, tag in ((ma, "old"), (mb, "new")):
        if m["dirty"]:
            warn.append(f"{tag} was built from a dirty tree")
    files = changed_files(a, b)
    if files is not None:
        build_files = sorted(f for f in files if f in ("Makefile", "link.ld") or f.startswith("tools/gen_") or f.endswith(".mk"))
        if build_files:
            warn.append("build inputs changed: " + ", ".join(build_files))
    out += [f"  !! {w}" for w in warn]
    ta, tb = a["totals"], b["totals"]
    out.append("")
    out.append("== totals")
    secs = sorted(set(a["sections"]) | set(b["sections"]), key=lambda n: (b["sections"].get(n) or a["sections"][n])["addr"])
    rows = [["flash image", ta["flash"], tb["flash"], sgn(tb["flash"] - ta["flash"])],
            ["  free to 0x080B3000", ta.get("free", 0), tb.get("free", 0), sgn(tb.get("free", 0) - ta.get("free", 0))],
            ["  hot code (<0x08040000)", ta["hot_code"], tb["hot_code"], sgn(tb["hot_code"] - ta["hot_code"])]]
    for n in secs:
        sa, sb = a["sections"].get(n, {}).get("size", 0), b["sections"].get(n, {}).get("size", 0)
        if sa or sb:
            rows.append([n, sa, sb, sgn(sb - sa) if sb != sa else ""])
    out += table(rows, ["region", "old", "new", "delta"])

    fa, fb = keyed(a, lambda k, ad: k == "F" and is_flash(ad)), keyed(b, lambda k, ad: k == "F" and is_flash(ad))
    oa, ob = keyed(a, lambda k, ad: k == "O" and is_flash(ad)), keyed(b, lambda k, ad: k == "O" and is_flash(ad))
    ra, rb = keyed(a, lambda k, ad: RAM_LO <= ad < RAM_HI), keyed(b, lambda k, ad: RAM_LO <= ad < RAM_HI)
    sfa, sfb = sum(v[0] for v in fa.values()), sum(v[0] for v in fb.values())
    soa, sob = sum(v[0] for v in oa.values()), sum(v[0] for v in ob.values())
    dflash = tb["flash"] - ta["flash"]
    unatt = dflash - (sfb - sfa) - (sob - soa)
    out.append("")
    out.append("== where the flash delta went (adds up exactly)")
    out += table([["functions", sfa, sfb, sgn(sfb - sfa)],
                  ["flash objects (tables, consts)", soa, sob, sgn(sob - soa)],
                  ["no symbol (literals, padding, .data init)", ta["flash"] - sfa - soa, tb["flash"] - sfb - sob, sgn(unatt)],
                  ["total", ta["flash"], tb["flash"], sgn(dflash)]], ["bucket", "old", "new", "delta"])

    def cause(f):
        if files is None or not f:
            return ""
        return "touched" if f in files else "knock-on"

    rows = []
    for key in set(fa) | set(fb):
        sa = fa.get(key, (0,))[0]
        sb = fb.get(key, (0,))[0]
        if sa != sb:
            name, f = key.split("|", 1)
            state = "NEW" if not sa else "GONE" if not sb else ""
            rows.append((sb - sa, sa, sb, state, name, f))
    rows.sort(key=lambda r: (-abs(r[0]), r[4]))
    grew = sum(r[0] for r in rows if r[0] > 0)
    shrank = sum(r[0] for r in rows if r[0] < 0)
    out.append("")
    out.append(f"== functions: {len(rows)} changed, +{grew} / {shrank} B   (knock-on = its file is not in the commit)")
    out += table([[sgn(d), sa, sb, st, cause(f), short(n), f or "-"] for d, sa, sb, st, n, f in rows[:top]],
                 ["delta", "old", "new", "state", "cause", "function", "defined in"])
    pairs = collections.defaultdict(list)
    for d, sa, sb, st, n, f in rows:
        pairs[(base_name(n), f)].append(d)
    swaps = [(sum(v), k) for k, v in pairs.items() if len(v) > 1]
    if swaps:
        out.append("   clone swaps (one function under two names), net: " + ", ".join(
            f"{short(k[0], 40)} {sgn(d)}" for d, k in sorted(swaps, key=lambda t: -abs(t[0]))[:5]))
    comp_a, comp_b = a["composition"], b["composition"]
    detail = []
    for d, sa, sb, state, name, f in [r for r in rows if r[0] > 0][:8]:
        key = name + "|" + (f or "")
        ca, cb = comp_a.get(key, {}), comp_b.get(key, {})
        parts = sorted(((cb.get(x, 0) - ca.get(x, 0), x) for x in set(ca) | set(cb)), key=lambda t: -abs(t[0]))
        parts = [p for p in parts if p[0]][:4]
        if parts:
            detail.append(f"  {sgn(d):>6}  {short(name, 60)}: " + ", ".join(f"{x} {sgn(v)}" for v, x in parts))
    if detail:
        out.append("")
        out.append("   inside the growers, bytes by the file the instructions come from:")
        out += detail

    def clones(fx):
        c = collections.defaultdict(list)
        for key, v in fx.items():
            name = key.split("|", 1)[0]
            if "[clone" in name or re.search(r"\.(constprop|isra|part|cold|lto_priv)\b", name):
                c[base_name(name)].append((name, v[0]))
        return c
    cla, clb = clones(fa), clones(fb)
    crow = []
    for base in set(cla) | set(clb):
        ka = sorted(n for n, _ in cla.get(base, []))
        kb = sorted(n for n, _ in clb.get(base, []))
        if ka != kb:
            crow.append([sgn(sum(s for _, s in clb.get(base, [])) - sum(s for _, s in cla.get(base, []))), len(ka), len(kb), short(base, 70)])
    if crow:
        crow.sort(key=lambda r: -abs(int(r[0])))
        out.append("")
        out.append("== clone sets changed (IPA made or dropped .constprop/.isra/.part copies)")
        out += table(crow[:15], ["delta", "old", "new", "function"])

    ia, ib = a["inline"], b["inline"]
    outline_a = {base_name(k.split("|", 1)[0]) for k in fa}
    outline_b = {base_name(k.split("|", 1)[0]) for k in fb}
    empty = {"copies": 0, "bytes": 0, "hosts": {}}
    irow = []
    for callee in set(ia) | set(ib):
        xa, xb = ia.get(callee, empty), ib.get(callee, empty)
        bc = base_name(callee)
        oa_, ob_ = bc in outline_a, bc in outline_b
        if xa["copies"] == xb["copies"] and xa["bytes"] == xb["bytes"] and oa_ == ob_:
            continue
        if not xa["copies"] and xb["copies"]:
            st = "now inlined" + (", out-of-line gone" if oa_ and not ob_ else "")
        elif xa["copies"] and not xb["copies"]:
            st = "not inlined now" + (", out-of-line now" if ob_ and not oa_ else "")
        elif oa_ != ob_:
            st = "out-of-line copy " + ("added" if ob_ else "gone")
        else:
            st = ""
        hn = sorted(set(xb["hosts"]) - set(xa["hosts"]))
        hg = sorted(set(xa["hosts"]) - set(xb["hosts"]))
        irow.append((xb["bytes"] - xa["bytes"], xa["copies"], xb["copies"], st, callee, hn, hg))
    irow.sort(key=lambda r: (-abs(r[0]), r[4]))
    big = [r for r in irow if abs(r[0]) >= 16 or r[3]]
    if big:
        out.append("")
        out.append(f"== inlining changes: {len(irow)} callees ({len(big)} shown: 16 B or more, or a state change)")
        out.append("   bytes = the callee's code inside other functions (exclusive of what is nested in it)")
        out += table([[sgn(d), ca, cb, st, short(c, 64)] for d, ca, cb, st, c, hn, hg in big[:top]],
                     ["bytes", "copies", "now", "state", "callee"])
        for d, ca, cb, st, c, hn, hg in big[:6]:
            hs = []
            if hn:
                hs.append("into " + ", ".join(short(h, 40) for h in hn[:3]) + (f" +{len(hn) - 3} more" if len(hn) > 3 else ""))
            if hg:
                hs.append("out of " + ", ".join(short(h, 40) for h in hg[:3]) + (f" +{len(hg) - 3} more" if len(hg) > 3 else ""))
            if hs:
                out.append(f"     {short(c, 40)}: " + "; ".join(hs))

    def by_def(fx):
        c = collections.Counter()
        for key, v in fx.items():
            c[v[1] or "(no source)"] += v[0]
        return c
    da, db = by_def(fa), by_def(fb)
    ga, gb = a["origin"], b["origin"]
    frows = []
    for f in set(da) | set(db) | set(ga) | set(gb):
        dd = db.get(f, 0) - da.get(f, 0)
        do = gb.get(f, 0) - ga.get(f, 0)
        if dd or do:
            frows.append((dd, do, cause(f), f))
    frows.sort(key=lambda r: -max(abs(r[0]), abs(r[1])))
    if frows:
        out.append("")
        out.append("== files: 'defined' = functions defined there; 'its code' = instructions written there, wherever")
        out.append("   they landed; 'inl' = its code - defined: inlining moved bytes into (+) or out of (-) other files")
        out += table([[sgn(dd), sgn(do), sgn(do - dd), m, f] for dd, do, m, f in frows[:top]],
                     ["defined", "its code", "inl", "cause", "file"])

    for title, xa, xb in (("flash objects (const tables, vtables)", oa, ob), ("RAM objects (.data/.bss/.noinit/overlays)", ra, rb)):
        orow = []
        for key in set(xa) | set(xb):
            sa, sb = xa.get(key, (0,))[0], xb.get(key, (0,))[0]
            if sa != sb:
                orow.append([sgn(sb - sa), sa, sb, short(key.split("|", 1)[0], 70)])
        if orow:
            orow.sort(key=lambda r: -abs(int(r[0])))
            out.append("")
            out.append(f"== {title}: {len(orow)} changed, {sgn(sum(int(r[0]) for r in orow))} B")
            out += table(orow[:15], ["delta", "old", "new", "object"])

    bits = [f"flash {sgn(dflash)} B"]
    net = collections.Counter()
    for d, sa, sb, st, n, f in rows:
        net[base_name(n)] += d
    if net:
        big1 = max(net.items(), key=lambda kv: abs(kv[1]))
        bits.append(f"biggest: {short(big1[0], 50)} {sgn(big1[1])}")
        if files is not None:
            ko = sum(r[0] for r in rows if r[5] and r[5] not in files)
            if ko:
                bits.append(f"knock-on in untouched files {sgn(ko)}")
    st = [r for r in irow if r[3]]
    if st:
        t = max(st, key=lambda r: abs(r[0]))
        sites = max(t[1], t[2])
        bits.append(f"{len(st)} inlining state changes, top: {short(t[4], 44)} {t[3].split(',')[0]} ({sites} sites, {sgn(t[0])} B inlined)")
    if unatt:
        bits.append(f"no-symbol bytes {sgn(unatt)}")
    out.insert(1, "  " + "; ".join(bits))
    return "\n".join(out)


# -------------------------------------------------------------------------------------------------------- commands

def cmd_snap(a):
    s = snapshot(a.build)
    p = save(s, a.out)
    m = s["meta"]
    print(f"snapshot {m['version']} cfg {m['cfg']}: flash {s['totals']['flash']} B, {len(s['syms'])} symbols, "
          f"{len(s['inline'])} inlined callees ({m['seconds']} s) -> {p}")


def cmd_diff(a):
    refs = a.refs
    if a.build:
        old = resolve(refs[0] if refs else "HEAD")
        new = snapshot(a.build)
    else:
        if len(refs) == 0:
            refs = ["HEAD~1", "HEAD"]
        elif len(refs) == 1:
            refs = [refs[0] + "~1", refs[0]]
        old, new = resolve(refs[0]), resolve(refs[1])
    text = diff(old, new, a.top)
    print(text)
    if a.out:
        Path(a.out).write_text(text + "\n")


def elf_matches_head(build):
    """True when build/open_k2.elf is HEAD: its version record names HEAD and is clean, or names HEAD's parent
    -dirty, the tree is clean now and the ELF is newer than every file the commit touched (built, then committed)."""
    elf = Path(build) / "open_k2.elf"
    if not elf.exists():
        return False, "no ELF"
    ver = version_record(elf, read_sections(elf))
    head = git("rev-parse", "--short=8", "HEAD")
    m = re.search(r"-([0-9a-f]{7,})(-dirty)?$", ver)
    if not m:
        return False, f"version record {ver!r}"
    if head.startswith(m.group(1)) and not m.group(2):
        return True, "version record"
    parent = git("rev-parse", "--short=8", "HEAD~1", check=False)
    if m.group(2) and parent.startswith(m.group(1)):
        if git("status", "--porcelain", "--untracked-files=no", check=False):
            return False, "built dirty and the tree is still dirty"
        mt = elf.stat().st_mtime
        tracked = git("ls-files", "src", "include", "Makefile", "link.ld", "tools").split()
        stale = [f for f in tracked if (ROOT / f).exists() and (ROOT / f).stat().st_mtime > mt]
        if stale:                       # edited (or reverted) after the build: the ELF may not be what was committed
            return False, f"{stale[0]} is newer than the ELF"
        return True, "built from the parent's dirty tree, committed unchanged"
    return False, f"ELF is {ver}, HEAD is {head}"


def cmd_hook(a):
    ok, why = elf_matches_head(a.build)
    if not ok:
        print(f"size-track: no snapshot ({why})")
        return
    s = snapshot(a.build)
    head = git("rev-parse", "--short=8", "HEAD")
    if s["meta"]["dirty"]:
        s["meta"]["version"] = re.sub(r"-[0-9a-f]{7,}-dirty$", f"-{head}", s["meta"]["version"])
        s["meta"]["commit"], s["meta"]["dirty"], s["meta"]["matched"] = head, False, why
    save(s)
    try:
        old = resolve("HEAD~1")
    except SystemExit:
        print(f"size-track: snapshot {head} saved; no parent snapshot to diff (backfill HEAD~1)")
        return
    text = diff(old, s, a.top)
    rep = store_dir() / f"{head}.txt"
    rep.write_text(text + "\n")
    print("size-track:" + text.split("\n")[1][1:] + f"\n  full report: {rep}")


def cmd_backfill(a):
    wt = store_dir() / "wt"
    main = store_dir().parent.parent
    if not wt.exists():
        git("worktree", "add", "--detach", str(wt), "HEAD")
    for sub in ("tinyusb", "lwip"):         # gitlinks are empty in a fresh worktree: copy the main checkout's trees
        if not (wt / "third_party" / sub / "src").exists() and (main / "third_party" / sub).exists():
            subprocess.run(["cp", "-a", "-T", str(main / "third_party" / sub), str(wt / "third_party" / sub)], check=True)
            gitfile = wt / "third_party" / sub / ".git"
            if gitfile.is_file():               # points into the main checkout's modules: breaks git describe here
                gitfile.unlink()
    for ref in a.refs:
        sha = git("rev-parse", "--short=8", ref + "^{commit}")
        if list(store_dir().glob(f"*-{sha}_*.json.gz")) or list(store_dir().glob(f"*-{sha}-ovf_*.json.gz")):
            print(f"{ref} ({sha}): already have a snapshot")
            continue
        git("checkout", "--detach", "--quiet", sha, cwd=wt)
        print(f"{ref} ({sha}): building in {wt} ...", flush=True)
        log = store_dir() / f"build-{sha}.log"
        with open(log, "w") as f:
            r = subprocess.run(["make", "docker-build"], cwd=wt, stdout=f, stderr=subprocess.STDOUT)
        probe = None
        if r.returncode:
            over = re.search(r"region .FLASH' overflowed by (\d+) bytes", log.read_text(errors="replace"))
            if not over:
                raise SystemExit(f"build of {sha} failed: {log}")
            # It does not fit: link once more with 64 KB more FLASH so the overflow can be explained, then restore.
            ld = wt / "link.ld"
            text = ld.read_text()
            ld.write_text(re.sub(r"(FLASH \(rx\)\s*:\s*ORIGIN = 0x[0-9A-Fa-f]+, LENGTH = )(\d+)K",
                                 lambda m: f"{m.group(1)}{int(m.group(2)) + 64}K", text, count=1))
            try:
                with open(log, "a") as f:
                    r = subprocess.run(["make", "docker-build"], cwd=wt, stdout=f, stderr=subprocess.STDOUT)
            finally:
                ld.write_text(text)
            if r.returncode:
                raise SystemExit(f"build of {sha} failed even with 64 KB more FLASH: {log}")
            probe = int(over.group(1))
            print(f"  overflows by {probe} B; linked with FLASH +64 KB to measure it")
        s = snapshot(wt / "build")
        if probe is not None:               # dirty only by the link.ld patch above
            s["meta"]["version"] = s["meta"]["version"].replace("-dirty", "")
            s["meta"]["dirty"] = False
            s["meta"]["overflow"] = probe
            s["meta"]["cfg"] += "-ovf"
            s["meta"]["version"] += "-ovf"
        if s["meta"]["commit"] is None or not sha.startswith(s["meta"]["commit"].split("-")[0]) or s["meta"]["dirty"]:
            raise SystemExit(f"{sha}: the image says {s['meta']['version']}, not this commit; not saved")
        print(f"  -> {save(s)}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("snap", help="snapshot a build directory")
    p.add_argument("--build", default=str(ROOT / "build"))
    p.add_argument("--out", help="write the snapshot here instead of the store")
    p = sub.add_parser("diff", help="explain the size change between two snapshots")
    p.add_argument("refs", nargs="*", help="old new (commit, snapshot .json.gz or build dir); one ref = its parent vs it")
    p.add_argument("--build", help="diff the stored snapshot of the ref (default HEAD) against this build dir")
    p.add_argument("--top", type=int, default=25)
    p.add_argument("--out", help="also write the report here")
    p = sub.add_parser("hook", help="post-commit: snapshot when the build is HEAD, print the one-line diff")
    p.add_argument("--build", default=str(ROOT / "build"))
    p.add_argument("--top", type=int, default=25)
    p = sub.add_parser("backfill", help="build revisions in a scratch worktree and snapshot them")
    p.add_argument("refs", nargs="+")
    a = ap.parse_args()
    {"snap": cmd_snap, "diff": cmd_diff, "hook": cmd_hook, "backfill": cmd_backfill}[a.cmd](a)


if __name__ == "__main__":
    main()
