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
LONG_PULSE_US = 1000.0
SESSION_GAP_MS = 1000.0       # a session starts with the first frame after this much silence
HANDSHAKE_GAP_MS = 60.0       # observed: the handshake ends with a ~75 ms pause before the ramp
STEADY_CMD = 0x82
STEADY_MAX_MS = 200.0
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
    """Raw capture bytes -> [(t_us, dm, dp)] with the 26-bit time unwrapped across records.

    The ISR is the only producer and its timestamps never go back, so every backward step is one
    wrap (17.9 s). A half-range test would misread a quiet gap of more than 8.9 s as a step back."""
    out, base, last = [], 0, None
    i = 0
    while i < len(buf):
        n = buf[i]
        if n < 6 or i + n > len(buf):
            break
        if buf[i + 1] == T_EDGES:
            _, ev = decode_chunk(buf[i + 6:i + n])
            for u, st in ev:
                if last is not None and u < last:
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
    # Group by rise time: the first ident after the wake burst starts with D+ already low, so its
    # pulse 1 is only a rise, hundreds of ms after its fall.
    groups = []
    for p in pulses:
        if groups and p[1] - groups[-1][-1][1] <= FRAME_GAP_US:
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
        fall, rise, idle = g[0]
        if rise - fall > LONG_PULSE_US:     # D+ was held low before the frame: time it from the rise
            fall, idle = rise, level(rise - LONG_PULSE_US)
        f = {"t_us": fall, "bits": bits, "n": len(g), "idle": 1 if idle else 0,
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


def _pairs(frames):
    """Complete frames -> [(t_us, cmd, reply or None)]: a command and the reply that follows it."""
    out, i = [], 0
    while i < len(frames):
        f = frames[i]
        if f["role"] != "cmd":
            i += 1
            continue
        nxt = frames[i + 1] if i + 1 < len(frames) else None
        if nxt is not None and nxt["role"] == "reply" and nxt["t_us"] - f["t_us"] < PAIR_MAX_US:
            out.append((f["t_us"], f["value"] >> 1, nxt["value"]))
            i += 2
        else:
            out.append((f["t_us"], f["value"] >> 1, None))
            i += 1
    return out


def summary(edges):
    """-> {"wakes": [{"t_ms", "clocks"}], "sessions": [...]}, times in ms from the first edge.

    A session runs from the first complete frame after SESSION_GAP_MS of silence to its last frame.
    Its handshake is the command/reply pairs from an ident at the session start up to the first
    pause over HANDSHAKE_GAP_MS once the ident has been answered (times from the session start).
    The steady state is the STEADY_CMD poll: its median period and its reply series."""
    if not edges:
        return {"wakes": [], "sessions": []}
    t0 = edges[0][0]
    frames = decode_frames(edges)
    wakes = [{"t_ms": (f["t_us"] - t0) / 1000.0, "clocks": f["n"]} for f in frames if f["n"] > FRAME_PULSES]
    groups = []
    for f in (f for f in frames if f["complete"]):
        if groups and (f["t_us"] - groups[-1][-1]["t_us"]) / 1000.0 <= SESSION_GAP_MS:
            groups[-1].append(f)
        else:
            groups.append([f])
    sessions = []
    for g in groups:
        pairs = _pairs(g)
        start = g[0]["t_us"]
        hs = []
        if pairs and pairs[0][1] in VOOC_CMDS:
            answered = False
            for k, (t, cmd, rep) in enumerate(pairs):
                if answered and k and (t - pairs[k - 1][0]) / 1000.0 > HANDSHAKE_GAP_MS:
                    break
                hs.append(((t - start) / 1000.0, cmd, rep))
                answered = answered or (cmd in VOOC_CMDS and bool(rep))
        hist = {}
        for _, cmd, _ in pairs:
            hist[cmd] = hist.get(cmd, 0) + 1
        steady = pairs[len(hs):]
        gaps = sorted((b[0] - a[0]) / 1000.0 for a, b in zip(steady, steady[1:])
                      if a[1] == b[1] == STEADY_CMD and (b[0] - a[0]) / 1000.0 < STEADY_MAX_MS)
        sessions.append({"start_ms": (start - t0) / 1000.0, "end_ms": (g[-1]["t_us"] - t0) / 1000.0,
                         "frames": len(g), "handshake": hs, "histogram": hist,
                         "period_ms": gaps[len(gaps) // 2] if gaps else None,
                         "replies_82": [rep for _, cmd, rep in steady if cmd == STEADY_CMD and rep is not None]})
    return {"wakes": wakes, "sessions": sessions}


def _runs(values):
    out = []
    for v in values:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return out


def summary_lines(edges):
    s = summary(edges)
    lines = [f"{len(edges)} edges, {len(s['sessions'])} sessions (labels are guesses, values are raw)"]
    for w in s["wakes"]:
        lines.append(f"wake burst {w['clocks']} clocks at {w['t_ms']:.3f} ms")
    for n, ses in enumerate(s["sessions"], 1):
        lines.append(f"session {n}: {ses['start_ms']:.3f} .. {ses['end_ms']:.3f} ms, {ses['frames']} frames")
        if ses["handshake"]:
            lines.append("  handshake (ms from the session start):")
        else:
            lines.append("  handshake: not in the capture (the session starts without an ident)")
        for t, cmd, rep in ses["handshake"]:
            name = SVOOC_CMDS.get(cmd) or VOOC_CMDS.get(cmd) or ""
            r = "none" if rep is None else f"0x{rep:03X}" + (" (silent)" if rep == 0 else "")
            lines.append(f"  {t:9.3f}  0x{cmd:02X} -> {r:16s} {name}")
        hist = ", ".join(f"0x{c:02X} x{k}" for c, k in sorted(ses["histogram"].items(), key=lambda x: -x[1]))
        lines.append(f"  commands: {hist}")
        if ses["period_ms"] is not None:
            reps = ", ".join(f"0x{v:03X}" + (f" x{k}" if k > 1 else "") for v, k in _runs(ses["replies_82"]))
            lines.append(f"  steady 0x{STEADY_CMD:02X} every {ses['period_ms']:.1f} ms, replies: {reps}")
    return lines


def edge_window(edges, lo_ms, hi_ms):
    """Every raw edge between lo_ms and hi_ms (from the first edge), with the step from the one before."""
    if not edges:
        return []
    t0, out = edges[0][0], []
    for k, (t, dm, dp) in enumerate(edges):
        ms = (t - t0) / 1000.0
        if lo_ms <= ms <= hi_ms:
            dt = t - edges[k - 1][0] if k else 0.0
            out.append(f"{ms:12.4f} ms  dp={int(dp)} dm={int(dm)}  +{dt:.1f}us")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture")
    c.add_argument("slot", type=int)
    c.add_argument("--path")
    c.add_argument("--timeout", type=float, default=1.0)
    f = sub.add_parser("file")
    f.add_argument("raw")
    for p in (c, f):
        p.add_argument("--summary", action="store_true", help="sessions, handshakes, command histogram")
        p.add_argument("--edges", nargs=2, type=float, metavar=("LO_MS", "HI_MS"),
                       help="dump every edge in this window (ms from the first edge)")
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
    edges = edges_from_records(data)
    if a.edges:
        print("\n".join(edge_window(edges, *a.edges)))
    elif a.summary:
        print("\n".join(summary_lines(edges)))
    else:
        print("\n".join(report(edges)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
