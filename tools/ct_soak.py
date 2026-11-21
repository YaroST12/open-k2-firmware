#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Charger-test soak: repeated runs on the connected charger with a random unit subset in random order.

Each run records the device stream, starts the run with tools/ct_start.py and checks the device afterwards:
a reboot (uptime went back), an unsafe / fault / stopped run, the stack low-water mark and the per-unit time.
Stops at the first reboot (a mid-run reboot is a finding to read at once). One summary file per session.

    python3 tools/ct_soak.py 20                              # 20 runs, every unit, medium level
    python3 tools/ct_soak.py 30 --units fcp,scp,afc,qc2,qc3 --knobs   # legacy only, random legacy timing knobs
    python3 tools/ct_soak.py 5 --level deep --min 1 --max 3 --seed 7

Needs a charger on the K2 (docs/HW_TEST_PROCEDURES.md "Charger-test soak"); never part of make gate.
Output: runs/soak-<MMDD-HHMM>/ (summary.tsv, log, run-NN/{ct.log,stream/}).
"""
import argparse
import json
import os
import random
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import k2_remote as kr  # noqa: E402

PY = sys.executable
KR = [PY, os.path.join(ROOT, "tools", "k2_remote.py")]
# Random values per legacy knob (k2_remote LEGACY_KNOBS order), and the firmware defaults restored at the end.
KNOB_CHOICES = {"quiet_ms": [0, 0, 500, 1500, 2500], "steady_ms": [200, 500, 1000], "first_wait_ms": [300, 500, 1000, 2000],
                "gap_ms": [100, 250, 500, 1000], "hold_ms": [300, 500, 1000, 1000], "chain_level": [0, 1]}
KNOB_DEFAULTS = {"quiet_ms": 0, "steady_ms": 500, "first_wait_ms": 1000, "gap_ms": 500, "hold_ms": 1000, "chain_level": 1}
BAD_WORDS = ("UNSAFE", "[FAULT]", "Stopped", "5V release failed")


def kr_json(*args, timeout=30):
    """One k2_remote --json call; None on any failure (device away, busy, bad output)."""
    try:
        r = subprocess.run(KR + ["--json", *args], capture_output=True, text=True, timeout=timeout, cwd=ROOT)
        return json.loads(r.stdout) if r.returncode == 0 else None
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError):
        return None


def kr_call(*args, timeout=30):
    try:
        return subprocess.run(KR + list(args), capture_output=True, text=True, timeout=timeout, cwd=ROOT).returncode == 0
    except subprocess.TimeoutExpired:
        return False


def read_log(path):
    out = []
    try:
        with open(path) as f:
            for line in f:
                try:
                    out.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    except FileNotFoundError:
        pass
    return out


def summary_line(ct_text):
    for line in ct_text.splitlines():
        if " pass, " in line and " fail" in line:
            return line.strip()
    return "no summary"


def one_run(i, d, units, a):
    """Runs one charger test into d; returns a dict of what the checks found."""
    os.makedirs(d, exist_ok=True)
    before = kr_json("ping") or {}
    rec = subprocess.Popen([PY, os.path.join(ROOT, "tools", "k2_stream.py"), "record", "--out", os.path.join(d, "stream"),
                            "--quiet", "--seconds", str(int(a.wait) + 120)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=ROOT)
    time.sleep(2.5)
    cmd = [PY, os.path.join(ROOT, "tools", "ct_start.py"), "--units", ",".join(units), "--level", a.level,
           "--cable", a.cable, "--wait", str(a.wait)]
    t0 = time.time()
    with open(os.path.join(d, "ct.log"), "w") as f:
        rc = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, cwd=ROOT).returncode
    wall = time.time() - t0
    time.sleep(2)
    rec.send_signal(signal.SIGTERM)
    try:
        rec.wait(timeout=10)
    except subprocess.TimeoutExpired:
        rec.kill()
    after = kr_json("ping") or {}
    mem = kr_json("memory") or {}
    with open(os.path.join(d, "ct.log")) as f:
        ct_text = f.read()
    log = read_log(os.path.join(d, "stream", "log.jsonl"))
    texts = [x.get("text", "") for x in log]
    bad = [t for t in texts if any(w in t for w in BAD_WORDS)]
    split = next((t.split("unit ms", 1)[1].strip() for t in reversed(texts) if "[CT] unit ms" in t), "")
    up0, up1 = before.get("uptime_ms"), after.get("uptime_ms")
    return {
        "run": i, "units": ",".join(units), "exit": rc, "wall_s": round(wall, 1),
        "summary": summary_line(ct_text),
        "reboot": (up0 is not None and up1 is not None and up1 < up0) or not after,
        "bad": len(bad), "bad_first": bad[0][:120] if bad else "",
        "stack_low": mem.get("psp_low", ""), "irq_low": mem.get("msp_low", ""),
        "unit_ms": split, "dir": os.path.relpath(d, ROOT),
    }


def set_knobs(values):
    for k, v in values.items():
        if not kr_call("charger-legacy-tune", k, str(v)):
            print(f"knob {k}={v} not set", file=sys.stderr)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("runs", type=int)
    ap.add_argument("--units", default=",".join(kr.TEST_UNITS),
                    help="the pool to draw from (default: every unit: " + ",".join(kr.TEST_UNITS) + ")")
    ap.add_argument("--min", type=int, default=2, help="fewest units per run (default 2)")
    ap.add_argument("--max", type=int, default=0, help="most units per run (default: the whole pool)")
    ap.add_argument("--level", choices=kr.TEST_LEVELS, default="medium")
    ap.add_argument("--cable", default="off", help="cable identity per run (ct_start --cable; default off)")
    ap.add_argument("--knobs", action="store_true", help="random legacy timing knobs per run (restored to the defaults at the end)")
    ap.add_argument("--seed", type=int, default=None, help="random seed (printed, so a session can be replayed)")
    ap.add_argument("--wait", type=float, default=900, help="ct_start --wait per run, seconds")
    ap.add_argument("--keep-going", action="store_true", help="do not stop at a reboot")
    a = ap.parse_args(argv)

    pool = kr.parse_units(a.units)
    names = [kr.TEST_UNITS[u] for u in pool]
    hi = min(a.max or len(names), len(names))
    lo = max(1, min(a.min, hi))
    seed = a.seed if a.seed is not None else random.randrange(1 << 30)
    rng = random.Random(seed)
    out = os.path.join(ROOT, "runs", "soak-" + time.strftime("%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    ping = kr_json("ping")
    if not ping:
        print("no K2 answering ping", file=sys.stderr)
        return 2
    head = f"# soak {time.strftime('%Y-%m-%d %H:%M')} fw {ping.get('version')} seed {seed} level {a.level} pool {','.join(names)}"
    print(head)
    print("dir " + os.path.relpath(out, ROOT))
    cols = ["run", "units", "knobs", "exit", "wall_s", "summary", "reboot", "bad", "bad_first", "stack_low", "irq_low", "unit_ms", "dir"]
    worst = None
    with open(os.path.join(out, "summary.tsv"), "w") as tsv, open(os.path.join(out, "log"), "w") as lg:
        lg.write(head + "\n")
        tsv.write("\t".join(cols) + "\n")
        try:
            for i in range(1, a.runs + 1):
                units = rng.sample(names, rng.randint(lo, hi))
                knobs = {k: rng.choice(v) for k, v in KNOB_CHOICES.items()} if a.knobs else {}
                if knobs:
                    set_knobs(knobs)
                r = one_run(i, os.path.join(out, f"run-{i:02d}"), units, a)
                r["knobs"] = " ".join(f"{k}={v}" for k, v in knobs.items())
                tsv.write("\t".join(str(r[c]) for c in cols) + "\n")
                tsv.flush()
                line = (f"{i:3d} {r['summary']} | exit={r['exit']} reboot={int(r['reboot'])} bad={r['bad']} stack_low={r['stack_low']}"
                        f" | {r['units']}" + (f" | {r['knobs']}" if knobs else ""))
                print(line, flush=True)
                lg.write(line + ("\n    " + r["bad_first"] if r["bad_first"] else "") + "\n")
                lg.flush()
                if isinstance(r["stack_low"], int):
                    worst = r["stack_low"] if worst is None else min(worst, r["stack_low"])
                if r["reboot"] and not a.keep_going:
                    print("REBOOT: stopping (read " + r["dir"] + ")", flush=True)
                    lg.write("REBOOT: stopping\n")
                    break
        finally:
            if a.knobs:
                set_knobs(KNOB_DEFAULTS)
            end = f"done; stack low {worst} B" if worst is not None else "done"
            print(end)
            lg.write(end + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
