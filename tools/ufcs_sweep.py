#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Sweep UFCS frame variants against a real source with the K2's 5 V probe, no reflashing.

For every variant the tool resets the tuner, applies the baseline (--set) and the variant's fields, starts
`ufcs probe` (handshake + opener exchange at 5 V; it never raises VBUS and never sends a Request), reads the
result blob, then pauses so the source's bus reset hold can finish and VBUS settles at 5 V before the next one.
Results go to runs/ufcs-sweep/<timestamp>/ (table.txt, results.jsonl); any variant with RX bytes beyond the
single 0x00 framing-error break the K2's own line release produces is marked "<<< RX".

    python3 tools/ufcs_sweep.py --dry-run                      # list the default variants
    python3 tools/ufcs_sweep.py                                # the default list (48 variants)
    python3 tools/ufcs_sweep.py --vary version=1,2 --vary baud=115200,57600
    python3 tools/ufcs_sweep.py --axes version,opener --set delay=50 --limit 6

Default axes, most likely first (variants are ordered by the sum of the value ranks, so the likeliest mixes
run first): version ([8:3]) 1,2,3,0; opener 0,6,1 (Ping, Get_Output_Capabilities, ACK); stop bits 1,2; crcsync 0,1
(CRC over the sync byte no/yes). The behaviour real sources show is the first variant of every axis.
--vary NAME=v1,v2,... replaces the axes (repeatable; the order given is the likelihood order). Field names are the
`k2_remote.py ufcs show` names. Refuses to run unless the K2 is idle at 5 V; stops on a reboot (uptime goes
backwards), on an inline / no-monitor refusal or on VBUS leaving the 5 V window. Uses /tmp/k2-device.lock.
"""
import argparse
import datetime
import itertools
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_lock
import k2_remote as kr

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_AXES = [("version", [1, 2, 3, 0]), ("opener", [0, 6, 1]), ("stop", [1, 2]), ("crcsync", [0, 1])]
VBUS_LO_MV, VBUS_HI_MV = 4500, 5500
POLL_S = 0.1
PROBE_TIMEOUT_S = 8.0       # the firmware caps a probe at 3 s
START_RETRY_S = 12.0        # a previous probe's bus reset hold ends by itself
VBUS_WAIT_S = 20.0


class SweepAbort(RuntimeError):
    pass


def parse_axis(text):
    """'field=1,2,3' -> ('field', [1, 2, 3]); values accept 0x hex. The field name is validated."""
    name, _, vals = text.partition("=")
    if not vals:
        raise argparse.ArgumentTypeError(f"{text!r}: expected NAME=v1,v2,...")
    if name not in [f[0] for f in kr.UFCS_FIELDS]:
        raise argparse.ArgumentTypeError(f"unknown ufcs field {name!r}")
    try:
        return name, [int(v, 0) for v in vals.split(",")]
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r}: values must be integers")


def parse_set(text):
    name, vals = parse_axis(text)
    if len(vals) != 1:
        raise argparse.ArgumentTypeError(f"{text!r}: --set takes one value")
    return name, vals[0]


def variants(axes):
    """Cartesian product ordered by the sum of the value ranks (likeliest mix first), ties by axis order.
    Each variant is a list of (field, value) in axes order."""
    ranked = itertools.product(*[list(enumerate(vals)) for _, vals in axes])
    rows = sorted(ranked, key=lambda combo: (sum(r for r, _ in combo), [r for r, _ in combo]))
    return [[(axes[i][0], v) for i, (_, v) in enumerate(combo)] for combo in rows]


def label(variant):
    return " ".join(f"{n}={v}" for n, v in variant) or "(defaults)"


def rx_bytes_hex(result):
    return "".join(e["hex"] for e in result["events"] if e["dir"] == "rx")


def is_break_only(result):
    """A lone 0x00 with a framing error is the line break the K2's own release makes, not the source."""
    if result["rx_bytes"] == 0:
        return True
    return result["rx_bytes"] == 1 and rx_bytes_hex(result) in ("", "00") and result["errors"]["framing"] >= 1


def interesting(result):
    """Any decodable or non-break RX bytes."""
    return result["verdict"] == "answered" or not is_break_only(result)


def wait_vbus(link, sleep, clock, timeout_s=VBUS_WAIT_S):
    """Block until VBUS is inside the 5 V window; SweepAbort on a timeout."""
    end = clock() + timeout_s
    mv = None
    while clock() < end:
        mv = kr.cmd_state(link, None)["vbus_mv"]
        if VBUS_LO_MV <= mv <= VBUS_HI_MV:
            return mv
        sleep(0.25)
    raise SweepAbort(f"VBUS {mv} mV is not inside {VBUS_LO_MV}..{VBUS_HI_MV} mV after {timeout_s:.0f} s")


def apply_tuner(link, baseline, variant):
    link.call(kr.CMD_UFCS, bytes([kr.UFCS_RESET]))
    for name, value in list(baseline) + list(variant):
        kr.ufcs_set(link, name, value)


def start_probe(link, sleep, clock):
    """Start one probe; waits out 'busy' / 'resetting' (the previous bus reset hold), aborts on a hard refusal."""
    end = clock() + START_RETRY_S
    while True:
        st = kr.decode_ufcs_status(link.call(kr.CMD_UFCS, bytes([kr.UFCS_PROBE])).payload)
        if st["start"] == "ok":
            return
        if st["start"] in ("inline", "no-monitor"):
            raise SweepAbort(f"probe refused: {st['start']}")
        if clock() >= end:
            raise SweepAbort(f"probe still refused ({st['start']}) after {START_RETRY_S:.0f} s")
        sleep(0.25)


def run_probe(link, sleep, clock):
    start_probe(link, sleep, clock)
    end = clock() + PROBE_TIMEOUT_S
    while True:
        st = kr.ufcs_status(link)
        if st["state"] != "running":
            break
        if clock() >= end:
            link.call(kr.CMD_UFCS, bytes([kr.UFCS_STOP]))
            raise SweepAbort("probe did not finish within 8 s (stopped)")
        sleep(POLL_S)
    return kr.decode_ufcs_blob(kr.ufcs_read_blob(link))


def table_row(i, variant, r):
    rx = rx_bytes_hex(r)
    tx = next((e["hex"] for e in r["events"] if e["dir"] == "tx"), "")
    mark = "<<< RX" if interesting(r) else ""
    return (f"{i:>3}  {label(variant):<46} {r['verdict']:<12} {r['class']:<8} tx={tx:<14} rx={r['rx_bytes']:<3} "
            f"fe={r['errors']['framing']:<2} {rx[:24]:<24} {mark}").rstrip()


def sweep(link, vlist, baseline, outdir, sleep=time.sleep, clock=time.monotonic, settle_s=1.5, out=print):
    """Run the variants; returns (rows, hits). Writes results.jsonl as it goes, table.txt at the end."""
    outdir.mkdir(parents=True, exist_ok=True)
    jsonl = (outdir / "results.jsonl").open("w")
    rows, hits = [], []
    uptime = kr.decode_ping(link.call(kr.CMD_PING).payload)["uptime_ms"]
    try:
        for i, v in enumerate(vlist, 1):
            wait_vbus(link, sleep, clock)
            apply_tuner(link, baseline, v)
            res = run_probe(link, sleep, clock)
            up = kr.decode_ping(link.call(kr.CMD_PING).payload)["uptime_ms"]
            if up < uptime:
                raise SweepAbort(f"K2 rebooted during variant {i} (uptime {up} ms < {uptime} ms): stop and investigate")
            uptime = up
            hit = interesting(res)
            rec = {"n": i, "variant": dict(v), "baseline": dict(baseline), "hit": hit, "result": res}
            jsonl.write(json.dumps(rec) + "\n")
            jsonl.flush()
            row = table_row(i, v, res)
            rows.append(row)
            if hit:
                hits.append(rec)
            out(row)
            sleep(settle_s)
    finally:
        jsonl.close()
        try:
            link.call(kr.CMD_UFCS, bytes([kr.UFCS_RESET]))      # leave the tuner at its defaults
        except Exception:
            pass
        head = f"{'#':>3}  {'variant':<46} {'verdict':<12} {'class':<8} {'tx':<17} {'rx':<6} {'fe':<5} first rx bytes"
        (outdir / "table.txt").write_text("\n".join([head] + rows) + "\n" +
                                          (f"\n{len(hits)} variant(s) with RX bytes beyond the break\n" if rows else ""))
    return rows, hits


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--vary", action="append", type=parse_axis, metavar="NAME=v1,v2",
                   help="replace the default axes (repeatable; value order = likelihood order)")
    p.add_argument("--axes", help="keep only these default axes (comma separated names)")
    p.add_argument("--set", action="append", type=parse_set, default=[], metavar="NAME=v",
                   help="fixed tuner value for every variant (e.g. delay=50, baud=57600)")
    p.add_argument("--limit", type=int, help="run only the first N variants")
    p.add_argument("--settle", type=float, default=1.5, help="pause between variants in seconds (default 1.5)")
    p.add_argument("--dry-run", action="store_true", help="print the variant list and exit (no device access)")
    p.add_argument("--out", help="output dir (default runs/ufcs-sweep/<timestamp>)")
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    return p


def pick_axes(a):
    if a.vary:
        return a.vary
    if a.axes:
        want = a.axes.split(",")
        bad = [w for w in want if w not in dict(DEFAULT_AXES)]
        if bad:
            raise SystemExit(f"--axes: unknown {bad}; default axes: " + ",".join(n for n, _ in DEFAULT_AXES))
        return [ax for ax in DEFAULT_AXES if ax[0] in want]
    return DEFAULT_AXES


def main(argv=None):
    a = build_parser().parse_args(argv)
    vlist = variants(pick_axes(a))
    if a.limit:
        vlist = vlist[:a.limit]
    if a.dry_run:
        if a.set:
            print("baseline: " + label(a.set))
        for i, v in enumerate(vlist, 1):
            print(f"{i:>3}  {label(v)}")
        print(f"{len(vlist)} variants", file=sys.stderr)
        return 0
    outdir = Path(a.out) if a.out else ROOT / "runs" / "ufcs-sweep" / datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    try:
        with k2_lock.hold():
            link = kr.RemoteLink(kr.LinuxHidTransport(a.path), 1.0)
            st = kr.ufcs_status(link)
            if st["state"] == "running":
                raise SweepAbort("a probe is already running")
            print(f"sweeping {len(vlist)} variants -> {outdir}")
            rows, hits = sweep(link, vlist, a.set, outdir, settle_s=a.settle)
    except SweepAbort as e:
        print(f"aborted: {e}", file=sys.stderr)
        return 3
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2
    print(f"\n{len(rows)} probes, {len(hits)} with RX bytes beyond the break; results in {outdir}")
    for h in hits:
        print("  HIT", label(list(h["variant"].items())), rx_bytes_hex(h["result"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
