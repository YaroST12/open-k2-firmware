#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""compile_commands.json for clangd / IDE navigation, merged from the commands the Makefile really ran.

Every compile recipe writes its exact command line to <object>.cmd (make's $(file), from the same variable it
then runs, see the Makefile). This tool turns those fragments into one compilation database at the repo root:

    python3 tools/compdb.py [--out compile_commands.json] [--fw build] [--host host-build]

  - firmware dirs (--fw, default build/) come first; the host test dirs (--host, default host-build/) add the
    files the firmware does not compile (tests/*.cpp, ...). A source built in both is listed once, as firmware.
  - `directory` is the host repo path. The firmware builds in a container with the repo at /workspace, so run
    from there with K2_HOST_ROOT=<host repo path> (make docker-build passes it): that prefix is rewritten in
    every argument. All recorded paths are relative to the repo root anyway.
  - the ARM compiler is not on the host, so clangd finds no <cstdint>: `make nav-headers` copies its include
    directories out of the build image into .cache/arm-none-eabi/ (once), and they are added as -isystem to the
    arm-none-eabi-* entries. Without that copy the entries are exactly what was run.
  - fragments whose source no longer exists are dropped; the file is written atomically and only when its
    content changed, so editors do not reindex after a no-op build.
"""
import argparse
import json
import os
import re
import shlex
import sys
import tempfile
from pathlib import Path

SRC_EXT = (".c", ".cc", ".cpp", ".cxx")
# options whose next argument is a value, never a source file
VALUE_OPTS = {"-o", "-include", "-I", "-isystem", "-iquote", "-MF", "-MT", "-MQ", "-x", "-D", "-U", "-dumpdir"}


def _remap(arg, container_root, host_root):
    if container_root == host_root:
        return arg
    return re.sub(re.escape(container_root) + r"(?=/|$)", lambda _m: host_root, arg)


def _join(host_root, path):
    return path if os.path.isabs(path) else os.path.join(host_root, path)


def entry_from_command(cmd, container_root, host_root):
    """One compile_commands.json entry for a recorded command line; None when it names no source file."""
    args = [_remap(a, container_root, host_root) for a in shlex.split(cmd)]
    src = out = None
    skip = False
    for i, a in enumerate(args[1:], 1):
        if skip:
            skip = False
            continue
        if a in VALUE_OPTS:
            skip = True
            if a == "-o" and i + 1 < len(args):
                out = args[i + 1]
        elif not a.startswith("-") and a.endswith(SRC_EXT) and src is None:
            src = a
    if src is None:
        return None
    e = {"directory": host_root, "file": _join(host_root, src), "arguments": args}
    if out is not None:
        e["output"] = _join(host_root, out)
    return e


def _fragments(root, dirs):
    for d in dirs:
        base = Path(root) / d
        if base.is_dir():
            yield from sorted(base.rglob("*.cmd"))


def toolchain_isystem(root, cache, host_root):
    """-isystem arguments for the ARM compiler's include search list, from `make nav-headers` (empty without it).

    <cache>/dirs.txt lists the compiler's system include directories as paths inside the build image;
    <cache>/root/ holds a copy of them under the same paths."""
    dirs = Path(root) / cache / "dirs.txt"
    if not dirs.is_file():
        return []
    flags = []
    for line in dirs.read_text().split():
        flags += ["-isystem", os.path.join(host_root, cache, "root", line.lstrip("/"))]
    return flags


def collect(root, fw_dirs, host_dirs, host_root, arm_flags=()):
    """Entries from the firmware dirs, then from the host dirs; one per source file, first one wins.
    arm_flags are inserted after the compiler name of every arm-none-eabi-* entry."""
    root = Path(root)
    container_root = str(root)
    seen, db = set(), []
    for frag in _fragments(root, list(fw_dirs) + list(host_dirs)):
        text = frag.read_text().strip()
        e = entry_from_command(text, container_root, host_root) if text else None
        if e is None or e["file"] in seen:
            continue
        if e["arguments"][0].startswith("arm-none-eabi-"):
            e["arguments"][1:1] = arm_flags
        rel = os.path.relpath(e["file"], host_root)
        if not (root / rel).is_file():
            continue  # stale fragment of a deleted or renamed source
        seen.add(e["file"])
        db.append(e)
    return db


def render(db):
    return json.dumps(db, indent=2) + "\n"


def write_if_changed(path, text):
    """Atomic write (temp file + rename); returns False and leaves the file alone when the content is the same."""
    path = Path(path)
    if path.is_file() and path.read_text() == text:
        return False
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=path.name + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise
    return True


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default="compile_commands.json")
    ap.add_argument("--fw", action="append", default=None, help="firmware build dir (default: build)")
    ap.add_argument("--host", action="append", default=None, help="host test build dir (default: host-build)")
    ap.add_argument("--toolchain", default=".cache/arm-none-eabi",
                    help="ARM include dirs copied out of the build image by `make nav-headers`")
    a = ap.parse_args(argv)
    root = os.getcwd()
    host_root = os.environ.get("K2_HOST_ROOT") or root
    arm = toolchain_isystem(root, a.toolchain, host_root)
    db = collect(root, a.fw or ["build"], a.host or ["host-build"], host_root, arm)
    if write_if_changed(Path(root) / a.out, render(db)):
        print(f"  DB    {a.out} ({len(db)} entries)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
