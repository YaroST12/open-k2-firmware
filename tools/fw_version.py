#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The firmware version string: `<major>.<minor>.<patch>-<iteration>-<hash>[-dirty]`, e.g. 1.0.0-435-26b245ac.

  major.minor.patch  the newest `v*` tag, normalised to three numbers (v1.0 -> 1.0.0, v1.1 -> 1.1.0, v1.2.3 -> 1.2.3)
  iteration          commits since that tag (0 on the tag itself)
  hash               the short commit hash, git's `g` prefix dropped
  -dirty             appended when the work tree has uncommitted changes, so a dirty build never passes for a clean one
  no git / no tag    0.0.0-0-unknown (no git at all) or 0.0.0-0-<hash> (a hash but no usable tag)

The Makefile calls this once (`K2_FW_VERSION`); the same string goes into the image (src/core/version.cpp), PING's
`version`, the boot screen, the flasher's messages. PING's `git` stays the raw `git describe --always --dirty`.

  python3 tools/fw_version.py                      the version of the work tree the tool lives in
  python3 tools/fw_version.py --describe TEXT      the version for one `git describe --long` output (tests, CI logs)"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

UNKNOWN = "0.0.0-0-unknown"
# --tags: a lightweight `git tag v1.2` counts like an annotated one (a release must not read 0.0.0 for want of -a)
DESCRIBE_ARGS = ("describe", "--tags", "--long", "--always", "--dirty", "--match", "v[0-9]*")

_LONG = re.compile(r"^v(\d+(?:\.\d+){0,2})-(\d+)-g([0-9a-f]+)(-dirty)?$")
_BARE = re.compile(r"^(?:.*-g)?([0-9a-f]{7,40})(-dirty)?$")


def from_describe(text):
    """The version string for one `git describe --long --always --dirty` output (None or garbage: unknown)."""
    text = (text or "").strip()
    m = _LONG.match(text)
    if m:
        nums = (m.group(1).split(".") + ["0", "0"])[:3]
        return f"{'.'.join(str(int(n)) for n in nums)}-{int(m.group(2))}-{m.group(3)}{m.group(4) or ''}"
    m = _BARE.match(text)
    if m:                                    # a hash with no usable tag (shallow clone, odd tag like v1.2-rc1)
        return f"0.0.0-0-{m.group(1)}{m.group(2) or ''}"
    return UNKNOWN


def derive(cwd=None, git="git", env=None):
    """The version of the work tree at `cwd` (default: the repository this tool lives in)."""
    cwd = cwd if cwd is not None else Path(__file__).resolve().parent.parent
    try:
        r = subprocess.run([git, *DESCRIBE_ARGS], cwd=cwd, env=env, capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return UNKNOWN
    return from_describe(r.stdout) if r.returncode == 0 else UNKNOWN


def major_minor(version):
    """(major, minor), a nibble each: the two numbers the bootloader's one-byte version word holds. A version that
    does not start with numbers (or the pre-0.0.0 `1.0` form) gives what the packer always defaulted to."""
    m = re.match(r"^v?(\d+)(?:\.(\d+))?", version or "")
    if not m:
        return 1, 0
    return int(m.group(1)) & 0x0F, int(m.group(2) or 0) & 0x0F


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--describe", help="derive from this `git describe --long` text instead of running git")
    a = ap.parse_args(argv)
    print(from_describe(a.describe) if a.describe is not None else derive())
    return 0


if __name__ == "__main__":
    sys.exit(main())
