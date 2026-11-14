# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The gate's performance step: the on-device `selftest` (docs/MATH_SELFTEST.md) against tools/perf_baseline.json.

The gate runs before any flash, so this step never flashes. It looks at the build that is on the K2 right now:

  - no K2 on the CDC port                       -> SKIPPED (no device)
  - the baseline file is unseeded               -> SKIPPED (unseeded; `make perf-baseline` seeds it)
  - the device runs another build than the one just built (version in the selftest header, `fw=`) -> SKIPPED
  - otherwise: `selftest` x3 under the device lock, per-check minimum cycles against the baseline.

It FAILS on a FAIL line, an unfinished run, a `zw` that went 1 -> 0, a missing check, or a check slower than
baseline + tolerance. Faster than baseline - tolerance only prints a notice (refresh the baseline). A skip is never a
failure, but gate.py repeats it after "GATE PASSED" so nobody reads a skipped step as a passed one.

With IRQs masked the cycle counts are deterministic to a few cycles, so the tolerance is small: per check the larger of
PCT % of the baseline value and MIN_CYCLES, and at `make perf-baseline` also twice the spread seen over its 5 runs.

Intended flow: `make gate` -> flash -> `make perf-check` (= `python3 tools/gate.py --perf-only`).

  python3 tools/perf_gate.py            check (what `make perf-check` runs)
  python3 tools/perf_gate.py --baseline re-take the baseline (5 runs, minimum) from the device"""
import contextlib
import datetime
import json
import math
import os
import re
import select
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BASELINE = ROOT / "tools/perf_baseline.json"
IMAGE_BIN = ROOT / "build/open_k2.bin"
FWID_OFFSET = 3076        # "K2FW" + version text, app offset (src/core/version.cpp; 0x08004404 - 0x08003800)
FWID_SIZE = 48
PORT = os.environ.get("K2_PORT", "/dev/ttyACM0")
GATE_RUNS = 3
BASELINE_RUNS = 5
PCT = 2.0                 # default tolerance: PCT % of the baseline value ...
MIN_CYCLES = 50           # ... or this many cycles, whichever is larger
SERIAL_TIMEOUT_S = 15.0

HEADER_RE = re.compile(r"^math selftest \((?P<kv>[^)]*)\):\s*$", re.M)
CHECK_RE = re.compile(r"^\s*\[(?P<st>PASS|FAIL|SKIP)\]\s+(?P<name>\w+)"
                      r"(?:\s+sum=0x(?P<sum>[0-9A-Fa-f]+)\s+cyc=(?P<cyc>\d+)\s+zw=(?P<zw>\d))?\s*$", re.M)
SUMMARY_RE = re.compile(r"^(?P<st>PASS|FAIL) (?P<p>\d+)/(?P<t>\d+) checks(?: \((?P<s>\d+) skipped\))?, "
                        r"total_cyc=(?P<cyc>\d+)\s*$", re.M)


class PerfError(Exception):
    pass


# ---- pure parts (unit-tested in tests/test_gate.py) ---------------------------------------------

def parse_selftest(text):
    """One `selftest` output -> {"fw", "hot", "arch", "checks": {name: {...}}, "order": [...], "summary": {...}|None}.
    Lines other than the header, the [PASS]/[FAIL]/[SKIP] rows and the summary (shell echo, `err:` lines, the
    `want=` row under a FAIL) are ignored."""
    text = text.replace("\r", "")
    res = {"fw": None, "hot": None, "arch": None, "checks": {}, "order": [], "summary": None}
    h = HEADER_RE.search(text)
    if h:
        kv = dict(p.split("=", 1) for p in h.group("kv").split() if "=" in p)
        res["fw"], res["arch"] = kv.get("fw"), kv.get("arch")
        res["hot"] = int(kv["hot"]) if kv.get("hot", "").isdigit() else None
    for m in CHECK_RE.finditer(text):
        c = {"status": m.group("st"), "sum": m.group("sum"),
             "cyc": int(m.group("cyc")) if m.group("cyc") else None,
             "zw": int(m.group("zw")) if m.group("zw") else None}
        res["checks"][m.group("name")] = c
        res["order"].append(m.group("name"))
    s = SUMMARY_RE.search(text)
    if s:
        res["summary"] = {"status": s.group("st"), "passed": int(s.group("p")), "total": int(s.group("t")),
                          "skipped": int(s.group("s") or 0), "total_cyc": int(s.group("cyc"))}
    return res


def merge_runs(runs):
    """Per check over several parsed runs: minimum cycles, maximum cycles (the spread), worst status
    (FAIL > SKIP > PASS), minimum zw."""
    out = {}
    for r in runs:
        for name in r["order"]:
            c = r["checks"][name]
            m = out.setdefault(name, {"status": "PASS", "cyc": None, "cyc_max": None, "zw": None, "n": 0})
            m["n"] += 1
            for worse in ("FAIL", "SKIP"):
                if c["status"] == worse or m["status"] == worse:
                    m["status"] = worse
                    break
            if c["cyc"] is not None:
                m["cyc"] = c["cyc"] if m["cyc"] is None else min(m["cyc"], c["cyc"])
                m["cyc_max"] = c["cyc"] if m["cyc_max"] is None else max(m["cyc_max"], c["cyc"])
            if c["zw"] is not None:
                m["zw"] = c["zw"] if m["zw"] is None else min(m["zw"], c["zw"])
    return out


def tolerance(entry):
    """Allowed slowdown in cycles for one baseline entry: its own `tol`, else max(PCT %, MIN_CYCLES)."""
    if entry.get("tol") is not None:
        return int(entry["tol"])
    return max(int(math.ceil(entry["cyc"] * PCT / 100.0)), MIN_CYCLES)


def compare(baseline, runs):
    """(errors, notices, info) for the parsed runs against a seeded baseline dict."""
    errs, notes, info = [], [], []
    for i, r in enumerate(runs, 1):
        s = r["summary"]
        if s is None:
            errs.append(f"run {i}: the selftest did not finish (no summary line); the K2 may have reset or the "
                        f"shell was busy")
        elif s["status"] != "PASS":
            errs.append(f"run {i}: selftest reports {s['status']} {s['passed']}/{s['total']}")
    merged = merge_runs(runs)
    for name, m in merged.items():
        if m["status"] == "FAIL":
            errs.append(f"{name}: FAIL line (a real host/target difference, or a broken production function)")
    base = baseline.get("checks", {})
    for name, b in base.items():
        m = merged.get(name)
        if m is None:
            errs.append(f"{name}: missing from the selftest output (in the baseline)")
            continue
        if m["status"] == "SKIP":
            errs.append(f"{name}: SKIP on the device, the baseline has numbers for it")
            continue
        if m["status"] == "FAIL" or m["cyc"] is None:
            continue
        if b.get("zw") == 1 and m["zw"] == 0:
            errs.append(f"{name}: zw 1 -> 0, a function slipped out of the zero-wait flash window (hot block)")
        tol = tolerance(b)
        d = m["cyc"] - b["cyc"]
        pct = 100.0 * d / b["cyc"] if b["cyc"] else 0.0
        line = f"  {name:13} {m['cyc']:8} cyc (baseline {b['cyc']:8}, {d:+d}, {pct:+.1f} %, tol {tol})"
        if d > tol:
            errs.append(f"{name}: slower than the baseline: {m['cyc']} cyc vs {b['cyc']} (+{d}, {pct:+.1f} %, tolerance {tol})")
            line += "  SLOWER"
        elif -d > tol:
            notes.append(f"{name}: faster than the baseline by {-d} cyc ({pct:+.1f} %): refresh with `make perf-baseline`")
            line += "  faster"
        info.append(line)
    for name in merged:
        if name not in base and merged[name]["status"] != "SKIP":
            notes.append(f"{name}: not in the baseline (a new check; `make perf-baseline` adds it)")
    return errs, notes, info


def make_baseline(runs, version, today=None):
    """A seeded baseline dict from parsed runs (per-check minimum, tolerance from the observed spread)."""
    merged = merge_runs(runs)
    checks = {}
    for name, m in merged.items():
        if m["status"] != "PASS" or m["cyc"] is None:
            continue
        spread = (m["cyc_max"] or m["cyc"]) - m["cyc"]
        e = {"cyc": m["cyc"], "zw": m["zw"]}
        floor = tolerance({"cyc": m["cyc"]})
        if 2 * spread > floor:
            e["tol"] = 2 * spread
        checks[name] = e
    first = runs[0]
    return {"status": "seeded", "version": version, "hot": first["hot"], "runs": len(runs),
            "taken": (today or datetime.date.today()).isoformat(),
            "tolerance_default": {"pct": PCT, "min_cycles": MIN_CYCLES},
            "note": "per-check minimum cycles of the default build; entries with `tol` override the default tolerance "
                    "(twice the spread seen over the runs). Refresh: make perf-baseline (needs the K2 on this build).",
            "checks": checks}


UNSEEDED = {"status": "unseeded", "version": None, "hot": None, "checks": {},
            "tolerance_default": {"pct": PCT, "min_cycles": MIN_CYCLES},
            "note": "No numbers yet: the perf step reports SKIPPED (unseeded). Flash the build, then `make perf-baseline`."}


def decide(baseline, built_version, device_version):
    """The reason to skip the perf step, or None to run it. `device_version` None = no device answered."""
    if baseline.get("status") != "seeded" or not baseline.get("checks"):
        return "unseeded: tools/perf_baseline.json has no numbers, run `make perf-baseline` on a flashed build"
    if device_version is None:
        return "no device: no K2 answered on the CDC port"
    if built_version is not None and device_version != built_version:
        return f"the K2 runs {device_version}, the image just built is {built_version}; flash it, then `make perf-check`"
    return None


def image_version(path=IMAGE_BIN):
    """The version text inside a built image (the 48-byte "K2FW" record at app offset 3076), or None."""
    try:
        with open(path, "rb") as f:
            f.seek(FWID_OFFSET)
            rec = f.read(FWID_SIZE)
    except OSError:
        return None
    if len(rec) != FWID_SIZE or rec[:4] != b"K2FW":
        return None
    return rec[4:].split(b"\0", 1)[0].decode("ascii", "replace") or None


# ---- the device ---------------------------------------------------------------------------------

def run_selftest(port=PORT, timeout=SERIAL_TIMEOUT_S):
    """Send `selftest` over the CDC shell and return the text; ends at the summary line. Raises PerfError when
    the port cannot be opened."""
    import termios
    try:
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    except OSError as e:
        raise PerfError(f"cannot open {port}: {e}")
    try:
        a = termios.tcgetattr(fd)
        a[0], a[1], a[3] = 0, 0, 0
        a[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(fd, termios.TCSANOW, a)
        time.sleep(0.3)
        while select.select([fd], [], [], 0.1)[0]:
            os.read(fd, 4096)
        os.write(fd, b"\r\nselftest\r\n")
        out, t0 = b"", time.monotonic()
        while time.monotonic() - t0 < timeout:
            if select.select([fd], [], [], 0.2)[0]:
                out += os.read(fd, 4096)
                if SUMMARY_RE.search(out.decode(errors="replace").replace("\r", "")):
                    break
        return out.decode(errors="replace")
    finally:
        os.close(fd)


def probe_device(port=PORT, runner=None):
    """(first parsed run, raw text) when a K2 answers `selftest`, else (None, reason). Never raises."""
    if runner is None:
        if not os.path.exists(port):
            return None, f"{port} not present"
        def runner():
            return run_selftest(port)
    try:
        text = runner()
    except PerfError as e:
        return None, str(e)
    r = parse_selftest(text)
    if r["fw"] is None:
        return None, "the shell answered without a selftest header (an older firmware, or no K2)"
    return r, text


def load_baseline(path=BASELINE):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError):
        return dict(UNSEEDED)


def collect(n, first=None, runner=None):
    """n parsed runs (the already taken `first` counts as one)."""
    runner = runner or run_selftest
    runs = [first] if first else []
    while len(runs) < n:
        runs.append(parse_selftest(runner()))
    return runs


class DeviceBusy(Exception):
    pass


LOCK_WAIT_S = 10.0       # a gate should not sit for minutes behind another device tool: give up and skip


@contextlib.contextmanager
def hold_device():
    """The exclusive K2 lock (tools/k2_lock.py). DeviceBusy when another tool still holds it after LOCK_WAIT_S."""
    sys.path.insert(0, str(ROOT / "tools"))
    import k2_lock
    try:
        cm = k2_lock.hold(wait_s=LOCK_WAIT_S)
        cm.__enter__()
    except k2_lock.DeviceLockTimeout as e:
        raise DeviceBusy(str(e))
    try:
        yield
    finally:
        cm.__exit__(None, None, None)


def perf_check(built_version=None, baseline_path=BASELINE, say=print, runner=None, lock=hold_device):
    """The gate step. Returns the skip reason, or None when the numbers were checked. Raises PerfError on a
    regression. Prints its findings. `runner` (a function returning selftest text) and `lock` are test seams."""
    baseline = load_baseline(baseline_path)
    if baseline.get("status") != "seeded" or not baseline.get("checks"):
        reason = decide(baseline, built_version, None)
        say(f"  SKIPPED ({reason})")
        return reason
    try:
        with lock():
            first, info = probe_device(runner=runner)
            if first is None:
                say(f"  SKIPPED (no device: {info})")
                return f"no device: {info}"
            reason = decide(baseline, built_version, first["fw"])
            if reason:
                say(f"  SKIPPED ({reason})")
                return reason
            runs = collect(GATE_RUNS, first, runner)
    except DeviceBusy as e:
        say(f"  SKIPPED (device busy: {e})")
        return f"device busy: {e}"
    errs, notes, lines = compare(baseline, runs)
    say(f"  device {first['fw']}, {len(runs)} runs, per-check minimum vs baseline {baseline.get('version')}")
    say("\n".join(lines))
    for n in notes:
        say(f"  NOTICE {n}")
    if errs:
        raise PerfError("selftest performance:\n  " + "\n  ".join(errs))
    return None


def perf_baseline(built_version=None, baseline_path=BASELINE, say=print, runner=None, lock=hold_device):
    """`make perf-baseline`: take the numbers from the K2, which must run the built image. Raises PerfError."""
    try:
        with lock():
            first, info = probe_device(runner=runner)
            if first is None:
                raise PerfError(f"no device to take the baseline from: {info}")
            if built_version is not None and first["fw"] != built_version:
                raise PerfError(f"the K2 runs {first['fw']}, the built image is {built_version}: flash it first")
            runs = collect(BASELINE_RUNS, first, runner)
    except DeviceBusy as e:
        raise PerfError(f"device busy: {e}")
    bad = [i for i, r in enumerate(runs, 1) if r["summary"] is None or r["summary"]["status"] != "PASS"]
    if bad:
        raise PerfError(f"runs {bad} did not PASS: not taking a baseline from a failing selftest")
    if any(r["fw"] != first["fw"] for r in runs):
        raise PerfError("the version changed between runs (the K2 rebooted or was reflashed)")
    b = make_baseline(runs, first["fw"])
    Path(baseline_path).write_text(json.dumps(b, indent=2) + "\n")
    say(f"  baseline written: {len(b['checks'])} checks from {first['fw']} ({len(runs)} runs, minimum)")
    return b


def main(argv):
    ver = image_version()
    try:
        if "--baseline" in argv:
            perf_baseline(ver)
        else:
            reason = perf_check(ver)
            if reason:
                print(f"PERF SKIPPED ({reason})")
    except PerfError as e:
        print(f"PERF FAILED: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
