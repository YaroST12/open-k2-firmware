#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Decode a Black Box "D+/D- edges" capture into VOOC / SuperVOOC frames.

The K2 timestamps every D+ / D- edge with the 240 MHz cycle counter (64-cycle units, 0.267 us) and
stores them as Type::Edges records (include/k2/fastcharge/edges.hpp). This tool unpacks them and decodes the
framing observed on a OnePlus 15 + SUPERVOOC 120 W pair (docs/FASTCHARGE_PROTOCOLS.md):

  - D+ idles high; the adapter clocks the exchange with 9 low pulses (25-40 us wide), spaced
    0.46 / 0.54 ms alternately (about 2 kHz), frames separated by more than 2 ms;
  - D- carries one bit per pulse. The bit that pulse k latches is the D- level just before pulse
    k+1 falls (the last bit: 300 us after the last pulse rises); the level before pulse 1 is the idle;
  - frames come in pairs about 9 ms apart: an adapter command (D- changes inside the D+ pulse, idle
    level 0) and the phone reply (D- changes about 50 us after the pulse rises, idle level 1);
  - a command is the 8 bits followed by one trailing 0 (0xA8 ident, 0x98, 0x86 ... of section 6.2);
    a reply is a 9 bit word, shown as 0x000..0x1FF; all zeros means the phone did not answer.

Usage:
  python3 tools/vooc_edges.py capture SLOT     read a capture from the K2 (needs HID) and decode it
  python3 tools/vooc_edges.py file RAWFILE     decode raw capture bytes saved by pd_trace.py capture --raw
"""
import argparse
import struct
import sys
from pathlib import Path

UNIT_US = 64 / 240.0
UNIT_MASK = (1 << 26) - 1
T_EDGES = 15
FRAME_PULSES = 9
FRAME_GAP_US = 2000.0
LAST_BIT_US = 300.0
PAIR_MAX_US = 12000.0
SVOOC_CMDS = {0xA8: "ident", 0x98: "battery 1", 0x86: "battery 2", 0x80: "status", 0x92: "status", 0x8E: "status", 0x9E: "status",
              0x88: "cable R", 0x8A: "telemetry", 0x82: "current limit", 0x84: "voltage ramp",
              0x9A: "heartbeat", 0x94: "heartbeat", 0x8C: "voltage step"}
VOOC_CMDS = {0xA8: "ident", 0xAC: "ident", 0xA6: "ident"}


def decode_chunk(p):
    """One Edges payload -> (idx, [(units, state)]); units are absolute modulo 2^26, state = dm<<1|dp."""
    if len(p) < 5:
        return 0, []
    idx = p[0]
    t = struct.unpack_from("<I", p, 1)[0]
    out, i, first = [], 5, True
    while i < len(p):
        v, sh = 0, 0
        while True:
            if i >= len(p) or sh > 35:
                return idx, out
            b = p[i]
            i += 1
            v |= (b & 0x7F) << sh
            sh += 7
            if not b & 0x80:
                break
        if not first:
            t = (t + (v >> 2)) & UNIT_MASK
        first = False
        out.append((t, v & 3))
    return idx, out


def edges_from_records(buf):
    """Raw capture bytes -> [(t_us, dm, dp)] with the 26-bit time unwrapped across records."""
    out, base, last = [], 0, None
    i = 0
    while i < len(buf):
        n = buf[i]
        if n < 6 or i + n > len(buf):
            break
        if buf[i + 1] == T_EDGES:
            _, ev = decode_chunk(buf[i + 6:i + n])
            for u, st in ev:
                if last is not None and u < last - (1 << 25):
                    base += 1 << 26
                last = u
                out.append(((base + u) * UNIT_US, bool(st & 2), bool(st & 1)))
        i += n
    return out


def _dedupe(edges):
    """The two EXTI lines race; drop events that repeat the previous (dm, dp) state."""
    out, prev = [], None
    for t, dm, dp in edges:
        if (dm, dp) != prev:
            out.append((t, dm, dp))
            prev = (dm, dp)
    return out


def decode_frames(edges):
    """-> [{"t_us", "bits", "value", "complete", "idle", "role", "n"}] (see the module doc)."""
    ev = _dedupe(edges)
    ts = [e[0] for e in ev]
    pulses, fall, idle = [], None, False
    for k in range(1, len(ev)):
        if ev[k - 1][2] and not ev[k][2]:
            fall, idle = ev[k][0], ev[k - 1][1]
        elif not ev[k - 1][2] and ev[k][2] and fall is not None:
            pulses.append((fall, ev[k][0], idle))
            fall = None
    groups = []
    for p in pulses:
        if groups and p[0] - groups[-1][-1][0] <= FRAME_GAP_US:
            groups[-1].append(p)
        else:
            groups.append([p])

    def level(t):
        lo, hi = 0, len(ev)
        while lo < hi:
            mid = (lo + hi) // 2
            if ts[mid] <= t:
                lo = mid + 1
            else:
                hi = mid
        return 1 if lo and ev[lo - 1][1] else 0

    frames, prev = [], None
    for g in groups:
        bits = [level(g[k + 1][0] - 0.5) for k in range(len(g) - 1)]
        bits.append(level(g[-1][1] + LAST_BIT_US))
        v = 0
        for b in bits:
            v = (v << 1) | b
        f = {"t_us": g[0][0], "bits": bits, "n": len(g), "idle": 1 if g[0][2] else 0,
             "value": v if len(g) == FRAME_PULSES else None, "complete": len(g) == FRAME_PULSES}
        paired = prev is not None and prev["role"] == "cmd" and f["t_us"] - prev["t_us"] < PAIR_MAX_US
        f["role"] = "reply" if (f["idle"] or paired) else "cmd"
        frames.append(f)
        prev = f
    return frames


def annotate(frame):
    """Text for one frame: the 9 bits, then the command byte or the reply word."""
    bits = "".join(map(str, frame["bits"]))
    if not frame["complete"]:
        return f"{bits} ({frame['n']} clocks, wake burst or partial)"
    v = frame["value"]
    if frame["role"] == "cmd":
        cmd = v >> 1
        txt = f"cmd=0x{cmd:02X}"
        name = SVOOC_CMDS.get(cmd) or VOOC_CMDS.get(cmd)
        if name:
            txt += f" ({name})"
        if v & 1:
            txt += " trailing=1"
        return f"{bits} {txt}"
    return f"{bits} reply=0x{v:03X}" + (" (silent)" if v == 0 else "")


def report(edges):
    lines = [f"{len(edges)} edges"]
    if not edges:
        return lines
    t0 = edges[0][0]
    span = (edges[-1][0] - t0) / 1000.0
    lines.append(f"span {span:.3f} ms, D+ edges {sum(1 for i in range(1, len(edges)) if edges[i][2] != edges[i-1][2])}, "
                 f"D- edges {sum(1 for i in range(1, len(edges)) if edges[i][1] != edges[i-1][1])}")
    frames = decode_frames(edges)
    if not frames:
        lines.append("no clocked frames (no D+ low pulses)")
        return lines
    prev_txt, run, start = None, 0, 0.0
    for f in frames:
        txt = annotate(f)
        if txt == prev_txt:
            run += 1
            continue
        if run > 1:
            lines[-1] += f"   (x{run})"
        run, prev_txt, start = 1, txt, (f["t_us"] - t0) / 1000.0
        lines.append(f"{start:10.3f} ms  {txt}")
    if run > 1:
        lines[-1] += f"   (x{run})"
    return lines


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("slot", type=int)
    c.add_argument("--path")
    c.add_argument("--timeout", type=float, default=1.0)
    f = sub.add_parser("file")
    f.add_argument("raw")
    a = ap.parse_args(argv)
    if a.cmd == "file":
        data = Path(a.raw).read_bytes()
    else:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        import k2_lock
        import k2_remote
        import pd_trace
        with k2_lock.hold():
            link = pd_trace.open_link(a.path, a.timeout)
            try:
                data, _ = k2_remote.blackbox_capture_read(link, a.slot)
            finally:
                link.transport.close()
    print("\n".join(report(edges_from_records(data))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
