#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""PD Stress host script: start a run on the attached charger, wait for it, save the raw result and a short report, and re-score saved
results under other weights (docs/PD_STRESS.md).

    python3 tools/pd_stress.py run --charger novoo-rg100 --confirm [--mask 0x7ff --cycles 100 --pace 0 --max-s 900 --max-trans 1200]
    python3 tools/pd_stress.py status | stop
    python3 tools/pd_stress.py report runs/pd-stress-novoo-rg100-20261112-101500/result.json
    python3 tools/pd_stress.py rescore a.json b.json [--weights w.json] [--json]
    python3 tools/pd_stress.py weights            # the firmware's table as JSON, a starting point for a weights file

`run` writes runs/pd-stress-<charger>-<stamp>/{result.json, report.txt, stream.log}. result.json carries the raw blob (hex), every event
counter per sequence, the transition counts and the latency / settle distributions, and the firmware's score per event kind, so a run
can be scored again later without the charger. A weights file is a flat JSON object with the firmware's event names as keys
(`weights` prints them); missing keys keep the firmware value. The charger is renegotiated hard (inside the PD rules, no load): `run`
needs --confirm, and it never touches the output. Exit codes: 0 finished clean of host errors, 2 K2 busy, 3 start refused,
4 timeout, 5 the run ended with a stop reason other than done (the report is still written).
"""
import argparse
import json
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pdstress_blob as pb

ROOT = Path(__file__).resolve().parent.parent
FORMAT = 1


def saved_result(blob, charger=None, started=None):
    """The JSON object saved for one run: the raw blob as hex plus its decode (raw counters, distributions, per-event points)."""
    r = pb.decode_blob(blob)
    return {"format": FORMAT, "charger": charger or "unnamed", "saved": started or datetime.now().isoformat(timespec="seconds"),
            "blob_hex": bytes(blob).hex(), "result": r}


def load_saved(path):
    """A saved result from a file; the decode is redone from the raw blob when it is there, so a file saved before the tables
    changed is read with today's names."""
    s = json.loads(Path(path).read_text())
    if s.get("blob_hex"):
        s["result"] = pb.decode_blob(bytes.fromhex(s["blob_hex"]))
    if "result" not in s:
        raise ValueError(f"{path}: not a PD Stress result")
    s.setdefault("charger", Path(path).parent.name or Path(path).stem)
    return s


def _unique(names):
    seen, out = {}, []
    for n in names:
        seen[n] = seen.get(n, 0) + 1
        out.append(n if seen[n] == 1 else f"{n}#{seen[n]}")
    return out


def rescore(saved, weights):
    """One row per saved run: the firmware's score and the score under `weights`, both in hundredths of a point per 100 transitions."""
    names = _unique([s["charger"] for s in saved])
    rows = []
    for name, s in zip(names, saved):
        r = s["result"]
        new = pb.score_cp(r["totals"], weights)
        rows.append({"charger": name, "transitions": r["totals"]["transitions"], "fw_cp": r["score_cp"], "new_cp": new,
                     "fw": r["score_cp"] / 100.0, "new": new / 100.0})
    return rows


def rescore_table(saved, weights):
    """charger x event kind: the count of each event in the run and its points under `weights`."""
    names = _unique([s["charger"] for s in saved])
    events = list(weights)
    cells = {e: {} for e in events}
    for name, s in zip(names, saved):
        for e in events:
            c = s["result"]["totals"]["counts"].get(e, 0)
            cells[e][name] = {"count": c, "points": c * weights[e]}
    return {"chargers": names, "events": events, "cells": cells, "weights": dict(weights)}


def format_rescore(rows, table, weights):
    default = pb.default_weights()
    lines = ["score per 100 transitions, 0 = ideal (weights provisional, see docs/PD_STRESS.md)"]
    changed = [f"{e} {default[e]}->{w}" for e, w in weights.items() if w != default[e]]
    lines.append("weights: " + (", ".join(changed) if changed else "the firmware's"))
    lines.append(f"{'charger':<24}{'trans':>7}{'firmware':>10}{'rescored':>10}{'delta':>9}")
    for r in rows:
        lines.append(f"{r['charger']:<24}{r['transitions']:>7}{r['fw']:>10.2f}{r['new']:>10.2f}{r['new'] - r['fw']:>+9.2f}")
    lines.append("")
    lines.append("event kind x charger: count (points under the rescored weights)")
    head = f"{'event':<14}{'w':>4}" + "".join(f"{c[:14]:>16}" for c in table["chargers"])
    lines.append(head)
    for e in table["events"]:
        cells = table["cells"][e]
        if not any(c["count"] for c in cells.values()):
            continue
        lines.append(f"{e:<14}{weights[e]:>4}" + "".join(f"{cells[c]['count']:>6} ({cells[c]['points']:>5})".rjust(16)
                                                           for c in table["chargers"]))
    if not any(c["count"] for e in table["events"] for c in table["cells"][e].values()):
        lines.append("(no events in any run: all clean)")
    return "\n".join(lines)


def read_weights_file(path):
    if not path:
        return pb.default_weights()
    return pb.merge_weights(json.loads(Path(path).read_text()))


def cmd_rescore(a):
    try:
        weights = read_weights_file(a.weights)
        saved = [load_saved(p) for p in a.files]
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    rows, table = rescore(saved, weights), rescore_table(saved, weights)
    if a.json:
        print(json.dumps({"weights": weights, "runs": rows, "table": table}, indent=1))
    else:
        print(format_rescore(rows, table, weights))
    return 0


def cmd_weights(a):
    print(json.dumps(pb.default_weights(), indent=1))
    return 0


def cmd_report(a):
    try:
        print(pb.render_text(load_saved(a.file)["result"]))
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    return 0


# ---- the device side: the same exchanges as `k2_remote pd-stress`, under the shared device lock ----
def _device():
    import k2_lock
    import k2_remote as kr
    return k2_lock, kr


def cmd_status(a):
    k2_lock, kr = _device()
    with k2_lock.hold():
        link = kr.RemoteLink(kr.LinuxHidTransport(a.path), 1.0)
        print(kr.render_pdstress({"pdstress": kr.pdstress_status(link)}))
    return 0


def cmd_stop(a):
    k2_lock, kr = _device()
    with k2_lock.hold():
        link = kr.RemoteLink(kr.LinuxHidTransport(a.path), 1.0)
        print(kr.render_pdstress({"pdstress": kr.pdstress_stop(link)}))
    return 0


def stress_lines(frames):
    """The [STRESS] log lines among the raw log frames (decoded from their msg# ids)."""
    import logdb
    text = b"".join(f for f in frames if f and f[0] < 0x80).replace(b"\x00", b"").decode("ascii", "replace")
    return [l for l in (logdb.decode_line(x) for x in text.splitlines()) if l.startswith("[STRESS]")]


def run_dir(a, now):
    if a.out:
        return Path(a.out)
    name = "".join(c if c.isalnum() or c in "-_" else "-" for c in a.charger)
    return ROOT / "runs" / f"pd-stress-{name}-{now:%Y%m%d-%H%M%S}"


def cmd_run(a):
    if not a.confirm:
        print("pd-stress run renegotiates the charger hard (inside the PD rules, no load): add --confirm", file=sys.stderr)
        return 1
    k2_lock, kr = _device()
    now = datetime.now()
    frames, flaky = [], 0
    try:
        with k2_lock.hold():
            link = kr.RemoteLink(kr.LinuxHidTransport(a.path), 1.0)
            st = kr.pdstress_status(link)
            if st["state"] in ("running", "resuming"):
                print("a run is already in progress; `pd_stress.py stop` ends it", file=sys.stderr)
                return 2
            link.tap = frames.append
            try:
                st = kr.pdstress_start(link, a.mask, a.cycles, a.pace, a.max_s, a.max_trans, True)
            except kr.RemoteError as e:
                print(f"start refused: {e}", file=sys.stderr)
                return 2 if "busy" in str(e).lower() else 3
            finally:
                link.tap = None
            if st["start"] != "ok" or st["state"] == "idle":
                print(f"start refused: {st['start']}", file=sys.stderr)
                return 3
            print(f"started: {st['seq_total']} sequences", file=sys.stderr)
        k2_lock.release()    # as tools/ct_start.py: the run goes on by itself, each poll below takes the lock for one exchange
        deadline = time.monotonic() + a.wait
        last = None
        while time.monotonic() < deadline:
            time.sleep(2)
            with k2_lock.hold():
                link.tap = frames.append
                try:
                    st = kr.pdstress_status(link)
                    flaky = 0
                except (kr.RemoteTimeout, kr.RemoteError, kr.FrameError):
                    st = None       # the K2 is busy with a step, or rebooted (a self-powered run: the budget resumes it)
                    flaky += 1
                finally:
                    link.tap = None
            if st is None:
                if flaky == 1 or flaky % 15 == 0:
                    print(f"  K2 not answering ({flaky}): rebooting? PD Stress resumes after a reboot on a self-powered K2",
                          file=sys.stderr)
                continue
            line = (st["state"], st["seq_idx"], st["reboots"])
            if line != last:
                print(f"  {st['state']}, sequence {st['seq_idx']}/{st['seq_total']}, {st['transitions']} transitions, "
                      f"{st['elapsed_ms'] / 1000:.0f} s, reboots {st['reboots']}", file=sys.stderr)
                last = line
            if st["state"] in ("finished", "idle"):
                break
        else:
            print("timeout waiting for the run to finish; stopping it", file=sys.stderr)
            with k2_lock.hold():
                kr.pdstress_stop(link)
            return 4
        with k2_lock.hold():
            blob = kr.pdstress_read_blob(link)
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2
    s = saved_result(blob, a.charger, now.isoformat(timespec="seconds"))
    out = run_dir(a, now)
    out.mkdir(parents=True, exist_ok=True)
    (out / "result.json").write_text(json.dumps(s, indent=1) + "\n")
    text = pb.render_text(s["result"])
    (out / "report.txt").write_text(text + "\n")
    (out / "stream.log").write_text("\n".join(stress_lines(frames)) + "\n")
    print(text)
    print(f"\nsaved in {out}")
    return 0 if s["result"]["stop"] == "done" else 5


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="start a run, wait, save result.json + report.txt + stream.log under runs/")
    r.add_argument("--charger", default="unnamed", help="name of the charger under test (used in the directory and the tables)")
    r.add_argument("--confirm", action="store_true", help="required: the charger is renegotiated hard, nothing is on the output")
    r.add_argument("--mask", type=lambda s: int(s, 0), default=0, help="sequence bit mask (0 = the default set)")
    r.add_argument("--cycles", type=int, default=0, help="percent of every sequence's cycles (0 = 100)")
    r.add_argument("--pace", type=int, default=0, choices=(0, 1, 2), help="0 per sequence, 1 all Settled, 2 all Ready")
    r.add_argument("--max-s", type=int, default=0, help="run time cap in seconds (0 = the K2's default)")
    r.add_argument("--max-trans", type=int, default=0, help="transition cap (0 = the K2's default)")
    r.add_argument("--wait", type=float, default=1500, help="give up after this many seconds (default 1500)")
    r.add_argument("--out", help="output directory (default runs/pd-stress-<charger>-<stamp>)")
    r.set_defaults(fn=cmd_run)
    sub.add_parser("status", help="state of the run on the K2").set_defaults(fn=cmd_status)
    sub.add_parser("stop", help="stop the run (never refused)").set_defaults(fn=cmd_stop)
    rp = sub.add_parser("report", help="print the report of a saved result")
    rp.add_argument("file")
    rp.set_defaults(fn=cmd_report)
    rs = sub.add_parser("rescore", help="score saved results under other weights, side by side with the firmware's score")
    rs.add_argument("files", nargs="+", metavar="result.json")
    rs.add_argument("--weights", metavar="w.json", help="flat JSON {event: weight}; default = the table parsed from score.hpp")
    rs.add_argument("--json", action="store_true")
    rs.set_defaults(fn=cmd_rescore)
    sub.add_parser("weights", help="print the firmware weights (parsed from score.hpp) as JSON").set_defaults(fn=cmd_weights)
    return p


def main(argv=None):
    a = build_parser().parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
