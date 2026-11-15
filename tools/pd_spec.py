#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""PD Spec Check host script: start a run on the attached charger, wait for it, save the result and a report (docs/PD_SPEC_CHECK.md).

    python3 tools/pd_spec.py run --charger baseus-100w-bank --confirm [--families transitions,pps-avs] [--pps-full] [--wait 1500]
    python3 tools/pd_spec.py status | stop
    python3 tools/pd_spec.py report runs/pd-spec-baseus-100w-bank-20261114-180000/result.json

`run` writes runs/pd-spec-<charger>-<stamp>/{result.json, report.txt, stream.log}. result.json carries the raw blob (hex) and its
decode: every check with its verdict, worst value, limits and penalty, the timer numbers, every transition and PPS/AVS pass, and the
time each family took (family_s, from the K2's run clock as the 2 s polls saw the family change). The charger is
stepped through its levels, Hard Reset and detached (inside the PD rules, no load, EPR only up to the K2's monitor ceiling): `run` needs
--confirm and never touches the output. Exit codes: 0 finished with stop "done", 2 K2 busy, 3 start refused, 4 timeout (the run is
stopped), 5 the run ended with another stop reason (the report is still written), 6 the return to 5 V was not confirmed (unplug),
7 the K2 rebooted during the run (its uptime went back, or it reads idle after the start): the result is lost; stop and investigate.
"""
import argparse
import json
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pdspec_blob as pb

ROOT = Path(__file__).resolve().parent.parent
FORMAT = 1


def saved_result(blob, charger=None, started=None):
    """The JSON object saved for one run: the raw blob as hex plus its decode."""
    return {"format": FORMAT, "charger": charger or "unnamed", "saved": started or datetime.now().isoformat(timespec="seconds"),
            "blob_hex": bytes(blob).hex(), "result": pb.decode_blob(blob)}


def load_saved(path):
    """A saved result; the decode is redone from the raw blob, so a file saved before the tables changed reads with today's names."""
    s = json.loads(Path(path).read_text())
    if s.get("blob_hex"):
        s["result"] = pb.decode_blob(bytes.fromhex(s["blob_hex"]))
    if "result" not in s:
        raise ValueError(f"{path}: not a PD Spec Check result")
    s.setdefault("charger", Path(path).parent.name or Path(path).stem)
    return s


def poll_state(st, up_prev, up_now):
    """What one poll says about the run: "reboot" when the K2's uptime went back or it reads idle after a start (a reboot comes back
    as a plain passive boot with no run and no result), "running", or "ended"."""
    if up_prev is not None and up_now is not None and up_now < up_prev:
        return "reboot"
    if st["state"] == "idle":
        return "reboot"
    return "running" if st["state"] == "running" else "ended"


def cmd_report(a):
    try:
        s = load_saved(a.file)
    except (OSError, ValueError, json.JSONDecodeError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    print(json.dumps(s["result"], indent=1) if a.json else pb.render_text(s["result"]) + (
        "\n" + family_text(s["family_s"]) if s.get("family_s") else ""))
    return 0


# ---- the device side: the same exchanges as `k2_remote pd-spec`, under the shared device lock ----
def _device():
    import k2_lock
    import k2_remote as kr
    return k2_lock, kr


def cmd_status(a):
    k2_lock, kr = _device()
    with k2_lock.hold():
        link = kr.RemoteLink(kr.open_transport(a.path), 1.0)
        print(kr.render_pdspec({"pdspec": kr.pdspec_status(link)}))
    return 0


def cmd_stop(a):
    k2_lock, kr = _device()
    with k2_lock.hold():
        link = kr.RemoteLink(kr.open_transport(a.path), 1.0)
        print(kr.render_pdspec({"pdspec": kr.pdspec_stop(link)}))
    return 0


def spec_lines(frames):
    """The [PDSPEC] log lines among the raw log frames (decoded from their msg# ids)."""
    import logdb
    text = b"".join(f for f in frames if f and f[0] < 0x80).replace(b"\x00", b"").decode("ascii", "replace")
    return [l for l in (logdb.decode_line(x) for x in text.splitlines()) if l.startswith("[PDSPEC]")]


def family_times(seen, end_s):
    """Seconds per family from the polls: seen = [(family, K2 elapsed_s at the first poll that showed it)] in order; a family runs
    until the next one is first seen, the last one until end_s. Resolution: the 2 s poll."""
    out = {}
    for i, (fam, t) in enumerate(seen):
        nxt = seen[i + 1][1] if i + 1 < len(seen) else end_s
        out[fam] = out.get(fam, 0) + max(0, nxt - t)
    return out


def family_text(times):
    return "time per family (2 s polls): " + ", ".join(f"{f} {s} s" for f, s in times.items()) if times else ""


def run_dir(a, now):
    if a.out:
        return Path(a.out)
    name = "".join(c if c.isalnum() or c in "-_" else "-" for c in a.charger)
    return ROOT / "runs" / f"pd-spec-{name}-{now:%Y%m%d-%H%M%S}"


def cmd_run(a):
    if not a.confirm:
        print("pd-spec run steps the charger through its levels, Hard Resets it and detaches (no load): add --confirm",
              file=sys.stderr)
        return 1
    k2_lock, kr = _device()
    families = kr.parse_families(a.families)
    now, t0 = datetime.now(), time.monotonic()
    frames = []
    try:
        with k2_lock.hold():
            link = kr.RemoteLink(kr.open_transport(a.path), 1.0)
            up = up0 = kr.cmd_ping(link, None)["uptime_ms"]
            st = kr.pdspec_status(link)
            if st["state"] == "running":
                print("a run is already in progress; `pd_spec.py stop` ends it", file=sys.stderr)
                return 2
            link.tap = frames.append
            try:
                st = kr.pdspec_start(link, families, True, a.pps_full)
            except kr.RemoteError as e:
                print(f"start refused: {e}", file=sys.stderr)
                return 2 if "busy" in str(e).lower() else 3
            finally:
                link.tap = None
            if st["start"] != "ok" or st["state"] != "running":
                print(f"start refused: {st['start']}", file=sys.stderr)
                return 3
            print(f"started: {st['families_text']}", file=sys.stderr)
        k2_lock.release()    # as tools/ct_start.py: the run goes on by itself, each poll below takes the lock for one exchange
        deadline = time.monotonic() + a.wait
        last, silent = None, 0
        seen = [(st["family"], st["elapsed_s"])]
        while time.monotonic() < deadline:
            time.sleep(2)
            with k2_lock.hold():
                link.tap = frames.append
                try:
                    up_now = kr.cmd_ping(link, None)["uptime_ms"]
                    st = kr.pdspec_status(link)
                    silent = 0
                except (kr.RemoteTimeout, kr.RemoteError, kr.FrameError):
                    st = None
                    silent += 1
                finally:
                    link.tap = None
            if st is None:
                if silent == 1 or silent % 15 == 0:
                    print(f"  K2 not answering ({silent})", file=sys.stderr)
                continue
            how = poll_state(st, up, up_now)
            up = up_now
            if how == "reboot":
                print(f"K2 REBOOTED during the run (uptime {up_now / 1000:.1f} s, {time.monotonic() - t0:.0f} s after the start at "
                      f"uptime {up0 / 1000:.1f} s): the result is lost; stop and investigate", file=sys.stderr)
                return 7
            if st["state"] == "running" and st["family"] != seen[-1][0]:
                seen.append((st["family"], st["elapsed_s"]))
            line = (st["state"], st["family"], st["item"])
            if line != last:
                print(f"  {st['state']}, {st['family']} {st['item']}/{st['items']}, fails {st['fails']}, {st['elapsed_s']} s",
                      file=sys.stderr)
                last = line
            if how == "ended":
                break
        else:
            print("timeout waiting for the run to finish; stopping it", file=sys.stderr)
            with k2_lock.hold():
                kr.pdspec_stop(link)
            return 4
        with k2_lock.hold():
            blob = kr.pdspec_read_blob(link)
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2
    try:
        s = saved_result(blob, a.charger, now.isoformat(timespec="seconds"))
    except ValueError as e:
        print(f"the result blob does not decode ({len(blob)} bytes): {e}", file=sys.stderr)
        return 7 if not blob else 5
    s["host_wall_s"] = round(time.monotonic() - t0, 1)
    s["k2_uptime_ms"] = {"start": up0, "end": up}       # one boot: end - start covers the run
    s["family_s"] = family_times(seen, s["result"]["elapsed_s"])
    out = run_dir(a, now)
    out.mkdir(parents=True, exist_ok=True)
    (out / "result.json").write_text(json.dumps(s, indent=1) + "\n")
    text = pb.render_text(s["result"]) + "\n" + family_text(s["family_s"])
    (out / "report.txt").write_text(text + "\n")
    (out / "stream.log").write_text("\n".join(spec_lines(frames)) + "\n")
    print(text)
    print(f"\nsaved in {out}")
    if s["result"]["release_unconfirmed"]:
        return 6
    return 0 if s["result"]["stop"] == "done" else 5


def build_parser():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0], formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run", help="start a run, wait, save result.json + report.txt + stream.log under runs/")
    r.add_argument("--charger", default="unnamed", help="name of the charger under test (used in the directory name)")
    r.add_argument("--confirm", action="store_true", help="required: the charger is stepped, Hard Reset and detached; nothing is "
                   "on the output")
    r.add_argument("--families", default="all", help="all (default), a mask (0x3f) or a comma list: transitions, pps-avs, "
                   "pps-timeout, epr-timeout, hard-reset, detach")
    r.add_argument("--pps-full", action="store_true", help="PPS 20 mV steps over the whole APDO window (default: three 10-step "
                   "windows)")
    r.add_argument("--wait", type=float, default=1500, help="give up after this many seconds (default 1500; the K2 caps a run at "
                   "20 min)")
    r.add_argument("--out", help="output directory (default runs/pd-spec-<charger>-<stamp>)")
    r.set_defaults(fn=cmd_run)
    sub.add_parser("status", help="state of the run on the K2").set_defaults(fn=cmd_status)
    sub.add_parser("stop", help="stop the run; the charger goes back to 5 V (never refused)").set_defaults(fn=cmd_stop)
    rp = sub.add_parser("report", help="print the report of a saved result")
    rp.add_argument("file")
    rp.add_argument("--json", action="store_true", help="the decoded result as JSON")
    rp.set_defaults(fn=cmd_report)
    return p


def main(argv=None):
    a = build_parser().parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
