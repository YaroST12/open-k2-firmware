#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Main-loop stall survey on the K2 (docs/ARCH_REWORK.md, phase 1): how long each page's paints hold the loop.

For every page: zero the firmware's stall counters (`debug loop --reset`), switch to the page (`page NAME`), sit there for
--dwell seconds while the page ticks, then read
  - `debug loop --page N`: the pass gap and PD service gap histograms, the page's longest pass and the pixels it sent;
  - `debug lcd`: the paint totals since boot (paint_px, paint_ms), as a delta over the dwell;
  - `state` (diag): poll_gap_max_ms, the USB poll gap since the last [USB] heartbeat line.
Ends back on Home. Writes runs/loop-survey/<git>/{survey.json,report.md}.

SAFETY: read-only plus page navigation. Remote calls go through `remote()`, which accepts only the allowlist below:
no trigger, no PD request, nothing that changes VBUS, no buttons. Every call is a subprocess with a timeout, and the
whole run holds the device lock (/tmp/k2-device.lock); the k2_remote subprocesses inherit it. A mid-run reboot (the
uptime at the end shorter than the wall-clock run) is reported and fails the run.

Needs a firmware with DebugLcd op 1 (k2/arch-rework or later) and USB HID on.

    python3 tools/loop_survey.py                      # all pages, 10 s each
    python3 tools/loop_survey.py --dwell 20 --pages home,detail
Exit codes: 0 done, 1 a check failed (reboot, a refused or failed read), 2 the K2 is busy or did not answer."""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import k2_lock
import k2_remote as kr

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable
REMOTE_TIMEOUT_S = 25
DEFAULT_PAGES = ("home", "detail", "chargertest", "lines", "tools", "settings", "connection", "diag")
CEILING_MS = 20     # one slow-task period (main_loop.cpp kLoopMs); the host surveys use the same ceiling
SVC_CEILING_MS = 10  # PD service gap: a third of tSenderResponse

# The only remote commands this script sends (the first one or two words of the k2_remote command line).
SAFE_COMMANDS = {("ping",), ("state",), ("page",), ("debug", "loop"), ("debug", "lcd")}


class SurveyError(Exception):
    pass


# ---- pure parts (tests/test_loop_survey.py) -------------------------------------------------------------

def allowed(args):
    """True when the k2_remote argument list starts with an allowlisted command."""
    a = [str(x) for x in args]
    return (a[0],) in SAFE_COMMANDS or tuple(a[:2]) in SAFE_COMMANDS


def page_index(name):
    """Ui::page_index of a page name, as `debug loop --page` and GET_STATE.page count it (6 + menu while a menu is open)."""
    return kr.PAGES.index(name)


def rebooted(uptime_start_ms, uptime_end_ms, wall_s, slack_s=5.0):
    """A reboot in between: the uptime went backwards, or grew far less than the wall clock did."""
    if uptime_end_ms < uptime_start_ms:
        return True
    return (uptime_end_ms - uptime_start_ms) / 1000.0 < wall_s - slack_s


def row(name, loop, lcd0, lcd1, diag):
    """One page's line of the survey from the reads around its dwell."""
    hist = loop["loop_hist"]
    over = sum(v for k, v in hist.items() if k in ("<=50", "<=100", ">100"))
    return {
        "page": name, "index": loop["page"], "elapsed_ms": loop["elapsed_ms"],
        "page_worst_ms": loop["page_ms"], "page_worst_px": loop["page_px"],
        "loop_max_ms": loop["loop_max_ms"], "svc_max_ms": loop["svc_max_ms"],
        "passes_over_20ms": over, "loop_hist": hist, "svc_hist": loop["svc_hist"],
        "paint_px": lcd1.get("paint_px", 0) - lcd0.get("paint_px", 0),
        "paint_ms": lcd1.get("paint_ms", 0) - lcd0.get("paint_ms", 0),
        "usb_poll_gap_max_ms": (diag or {}).get("poll_gap_max_ms"),
        "over": loop["loop_max_ms"] > CEILING_MS or loop["svc_max_ms"] > SVC_CEILING_MS,
    }


def report_md(head, dwell_s, rows, notes):
    md = [f"# K2 loop stall survey {head}", "",
          f"Dwell {dwell_s} s per page. Ceilings: pass gap {CEILING_MS} ms, PD service gap {SVC_CEILING_MS} ms "
          "(report only). Buckets <=1 <=2 <=5 <=10 <=20 <=50 <=100 >100 ms.", "",
          "| page | worst pass ms | its px | loop max ms | PD svc max ms | passes >20 ms | paint px | paint ms | "
          "USB poll gap ms | pass gap histogram | PD service gap histogram |",
          "|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in rows:
        md.append(f"| {r['page']}{' **OVER**' if r['over'] else ''} | {r['page_worst_ms']:.1f} | {r['page_worst_px']} | "
                  f"{r['loop_max_ms']} | {r['svc_max_ms']} | {r['passes_over_20ms']} | {r['paint_px']} | {r['paint_ms']} | "
                  f"{r['usb_poll_gap_max_ms']} | {' '.join(map(str, r['loop_hist'].values()))} | "
                  f"{' '.join(map(str, r['svc_hist'].values()))} |")
    if notes:
        md += ["", "## Notes", ""] + [f"- {n}" for n in notes]
    return "\n".join(md) + "\n"


# ---- device access (every call bounded) -------------------------------------------------------------------

def remote(*args, timeout=REMOTE_TIMEOUT_S):
    if not allowed(args):
        raise SurveyError(f"refused: {' '.join(map(str, args))} is not allowed in the loop survey")
    try:
        r = subprocess.run([PY, "tools/k2_remote.py", "--json", "--timeout", "3", *map(str, args)], cwd=ROOT,
                           capture_output=True, text=True, timeout=timeout, env=k2_lock.child_env())
    except subprocess.TimeoutExpired as e:
        raise SurveyError(f"timeout after {timeout}s: k2_remote {' '.join(map(str, args))}") from e
    if r.returncode:
        raise SurveyError(f"k2_remote {' '.join(map(str, args))} exit {r.returncode}: {(r.stderr or r.stdout).strip()}")
    return json.loads(r.stdout) if r.stdout.strip() else {}


def survey(a):
    ping0 = remote("ping")
    head = ping0.get("git", "unknown")
    t0 = time.monotonic()
    print(f"K2 {head} up {ping0.get('uptime_ms', 0) / 1000:.0f} s; dwell {a.dwell} s on {len(a.pages)} pages", flush=True)
    try:
        remote("debug", "loop", "--reset")
    except SurveyError as e:
        raise SurveyError(f"this firmware has no `debug loop` (DebugLcd op 1): {e}") from e
    rows, notes = [], []
    for name in a.pages:
        lcd0 = remote("debug", "lcd").get("lcd", {})
        remote("debug", "loop", "--reset")
        remote("page", name)
        time.sleep(a.dwell)
        loop = remote("debug", "loop", "--page", page_index(name))["loop"]
        lcd1 = remote("debug", "lcd").get("lcd", {})
        diag = remote("state").get("diag")
        r = row(name, loop, lcd0, lcd1, diag)
        rows.append(r)
        print(f"{name:12s} worst pass {r['page_worst_ms']:7.1f} ms ({r['page_worst_px']} px)  loop max {r['loop_max_ms']} ms  "
              f"PD svc max {r['svc_max_ms']} ms  >20 ms passes {r['passes_over_20ms']}{'  OVER' if r['over'] else ''}",
              flush=True)
    remote("page", "home")
    ping1 = remote("ping")
    wall = time.monotonic() - t0
    if ping1.get("git") != head:
        notes.append(f"the firmware changed during the run: {head} -> {ping1.get('git')}")
    reboot = rebooted(ping0.get("uptime_ms", 0), ping1.get("uptime_ms", 0), wall)
    if reboot:
        notes.append(f"MID-RUN REBOOT: uptime {ping0.get('uptime_ms')} -> {ping1.get('uptime_ms')} ms over {wall:.0f} s wall clock; "
                     "stop and investigate")
    return head, rows, notes, reboot


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--dwell", type=float, default=10.0, help="seconds on each page (default 10)")
    ap.add_argument("--pages", default=",".join(DEFAULT_PAGES), help="comma list of page names (k2_remote PAGES)")
    ap.add_argument("--out", default="runs/loop-survey", help="output directory (a <git> subdirectory is made)")
    a = ap.parse_args(argv)
    a.pages = [p.strip() for p in a.pages.split(",") if p.strip()]
    for p in a.pages:
        if p not in kr.PAGES[:kr.ROTATION_PAGES] and p not in kr.OPENABLE_MENUS:
            ap.error(f"page {p!r} cannot be opened remotely")
    try:
        with k2_lock.hold():
            head, rows, notes, reboot = survey(a)
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2
    except SurveyError as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 2 if "ping" in str(e) else 1
    out = ROOT / a.out / head
    out.mkdir(parents=True, exist_ok=True)
    (out / "survey.json").write_text(json.dumps({"git": head, "dwell_s": a.dwell, "rows": rows, "notes": notes}, indent=2))
    (out / "report.md").write_text(report_md(head, a.dwell, rows, notes))
    for n in notes:
        print(n)
    print(f"wrote {out / 'report.md'}")
    return 1 if reboot else 0


if __name__ == "__main__":
    sys.exit(main())
