#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Stage the firmware the Pages flasher offers: <out>/fw/open_k2.k2 and <out>/fw/latest.json.

latest.json = {version, git, sha256, size, built}; webhid-flasher/index.html fetches it (same origin, GitHub release
assets carry no CORS headers) and checks the container against sha256 before it flashes.

  --release-dir DIR --tag vX.Y --published ISO   the assets of a GitHub Release (open_k2.k2 + SHA256SUMS), the
                                                 only source on the published site; version = the tag, and the
                                                 container must match its SHA256SUMS line
  --build DIR                                    a local build (`make site`); version = "local-<git>" so the page
                                                 never mistakes it for a release

--optional: a missing input is not an error (nothing is staged, exit 0)."""
import argparse
import hashlib
import json
import re
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_flasher  # noqa: E402

NAME = "open_k2.k2"


def _write(out, blob, info):
    fw = Path(out) / "fw"
    fw.mkdir(parents=True, exist_ok=True)
    (fw / NAME).write_bytes(blob)
    (fw / "latest.json").write_text(json.dumps(info, indent=2) + "\n")
    return info


def _info(blob, version, git, built):
    k2_flasher.Firmware.parse(blob)          # ValueError unless a well-formed K2 container
    return {"version": version, "git": git, "sha256": hashlib.sha256(blob).hexdigest(), "size": len(blob), "built": built}


def stage_local(build, out):
    """The container of a local build; the git label comes from the build/.git-<describe> stamp the Makefile leaves."""
    build = Path(build)
    blob = (build / NAME).read_bytes()
    stamps = sorted(p.name[len(".git-"):] for p in build.glob(".git-*"))
    git = stamps[0] if stamps else "unknown"
    built = datetime.fromtimestamp((build / NAME).stat().st_mtime, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    return _write(out, blob, _info(blob, f"local-{git}", git, built))


def listed_sha256(sums_text, name):
    """The digest `sha256sum` printed for `name` (text or binary mode line), or None."""
    for line in sums_text.splitlines():
        m = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line.strip())
        if m and m.group(2) == name:
            return m.group(1).lower()
    return None


def stage_release(release_dir, tag, published, out):
    """The container of a GitHub Release, verified against the release's SHA256SUMS."""
    release_dir = Path(release_dir)
    blob = (release_dir / NAME).read_bytes()
    want = listed_sha256((release_dir / "SHA256SUMS").read_text(), NAME)
    if want is None:
        raise ValueError(f"SHA256SUMS has no line for {NAME}")
    if hashlib.sha256(blob).hexdigest() != want:
        raise ValueError(f"{NAME} does not match its SHA256SUMS line")
    # a tag build stamps its own tag as the git label (git describe on an exact tag)
    return _write(out, blob, _info(blob, tag, tag, published))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True, help="site directory; fw/ is created inside")
    ap.add_argument("--build", help="local build directory")
    ap.add_argument("--release-dir", help="directory holding the downloaded release assets")
    ap.add_argument("--tag")
    ap.add_argument("--published", help="release publish time, ISO 8601 UTC")
    ap.add_argument("--optional", action="store_true")
    a = ap.parse_args(argv)
    if bool(a.build) == bool(a.release_dir):
        ap.error("give exactly one of --build and --release-dir")
    if a.release_dir and not (a.tag and a.published):
        ap.error("--release-dir needs --tag and --published")
    src = Path(a.build or a.release_dir)
    if not (src / NAME).is_file():
        print(f"stage_fw: no {src / NAME}" + ("; staging without fw/" if a.optional else ""), file=sys.stderr)
        return 0 if a.optional else 1
    try:
        info = (stage_local(src, a.out) if a.build else stage_release(src, a.tag, a.published, a.out))
    except ValueError as e:
        print(f"stage_fw: {e}", file=sys.stderr)
        shutil.rmtree(Path(a.out) / "fw", ignore_errors=True)
        return 1
    print(f"stage_fw: fw/{NAME} {info['version']} ({info['size']} B, sha256 {info['sha256'][:12]})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
