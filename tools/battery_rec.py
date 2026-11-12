# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The battery efficiency recorder's run record (include/k2/battery/battery.hpp, docs/BATTERY_RECORDER.md) -> JSON-ready
dict, and the remote status block (CMD_BATTERY 0x58, include/k2/core/remote_proto.hpp).

The 246-byte record is what /bt/<id>.rec holds, what `k2_remote.py battery read` fetches and what a BATT QR share
carries. Every value keeps its unit in the key name (_uwh microwatt-hours, _uah microamp-hours, _s seconds, _mv, _ma).
Raw numbers only, no verdicts. Curve current keeps the power monitor's sign: positive = input port -> output port.
"""
import struct

VERSION = 1
RECORD_BYTES = 246
HEAD_BYTES = 64
PHASE_BYTES = 24
POINTS = 60

MODES = ["charge", "discharge", "round-trip"]
WIRINGS = ["same-port", "moved"]
RUN_STATES = ["measuring", "between", "done"]          # a record's state
STATES = ["idle", "measuring", "between", "done"]      # the status block's (the recorder in RAM)
KINDS = ["charge", "discharge"]
ENDS = ["running", "auto", "manual", "lost"]
RESULTS = ["ok", "busy", "no-monitor", "bad-arg", "not-now"]
NO_RATIO = 0xFFFF
STATUS_BYTES = 40


class BadRecord(ValueError):
    pass


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE, as k2::ringlog::crc16."""
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _name(table, i, what):
    if i >= len(table):
        raise BadRecord(f"{what} {i} out of range")
    return table[i]


def ratio_permille(out, inp):
    """k2::battery::ratio_permille: out/in in tenths of a percent, rounded; None when nothing went in."""
    if inp == 0:
        return None
    return min((out * 1000 + inp // 2) // inp, NO_RATIO - 1)


def unpack_point(b):
    """Curve::unpack: 12-bit 10 mV and signed 12-bit 4 mA fields in three bytes."""
    mv = (b[0] | (b[1] & 0xF) << 8) * 10
    i = b[1] >> 4 | b[2] << 4
    if i & 0x800:
        i -= 0x1000
    return mv, i * 4


def decode_record(rec):
    rec = bytes(rec)
    if len(rec) != RECORD_BYTES:
        raise BadRecord(f"record is {len(rec)} bytes, not {RECORD_BYTES}")
    if rec[0:2] != b"BR":
        raise BadRecord("not a battery record (magic)")
    if rec[2] != VERSION:
        raise BadRecord(f"record version {rec[2]}, this reader knows {VERSION}")
    want = struct.unpack_from("<H", rec, RECORD_BYTES - 2)[0]
    if crc16(rec[:RECORD_BYTES - 2]) != want:
        raise BadRecord("CRC mismatch")
    mode = _name(MODES, rec[3] & 0xF, "mode")
    wiring = _name(WIRINGS, rec[3] >> 4, "wiring")
    state = _name(RUN_STATES, rec[4], "state")
    nphases, resumes = rec[5] & 3, rec[5] >> 2
    rid, end_ma, end_s, interval_s = struct.unpack_from("<4H", rec, 6)
    points, split = rec[14], rec[15]
    if nphases > 2 or points > POINTS:
        raise BadRecord("phase or point count out of range")
    phases = []
    for i in range(nphases):
        o = 16 + i * PHASE_BYTES
        kind, end, gaps, secs, uwh_in, uwh_out, uah_in, uah_out = struct.unpack_from("<BBHIIIII", rec, o)
        phases.append({"kind": _name(KINDS, kind, "kind"), "end": _name(ENDS, end, "end"), "gaps": gaps, "time_s": secs,
                       "in_uwh": uwh_in, "out_uwh": uwh_out, "in_uah": uah_in, "out_uah": uah_out})
    curve = []
    for i in range(points):
        mv, ma = unpack_point(rec[HEAD_BYTES + 3 * i:HEAD_BYTES + 3 * i + 3])
        curve.append({"t_s": i * interval_s, "phase": 1 if split != 0xFF and i >= split else 0, "mv": mv, "ma": ma})
    tot = {k: sum(p[k] for p in phases) for k in ("in_uwh", "out_uwh", "in_uah", "out_uah", "time_s")}
    final = (mode == "round-trip" and nphases == 2 and phases[1]["end"] not in ("running", "lost") and tot["in_uwh"] > 0)
    return {
        "version": rec[2], "id": rid, "mode": mode, "wiring": wiring, "state": state, "resumes": resumes,
        "end_ma": end_ma, "end_s": end_s, "phases": phases, "totals": tot,
        "efficiency_permille": ratio_permille(tot["out_uwh"], tot["in_uwh"]) if final else None,
        "charge_ratio_permille": ratio_permille(tot["out_uah"], tot["in_uah"]) if final else None,
        "curve_interval_s": interval_s, "curve": curve,
    }


def decode_status(p):
    """The 40-byte status block every CMD_BATTERY op but list and read answers."""
    p = bytes(p)
    if len(p) < STATUS_BYTES:
        raise BadRecord(f"status block is {len(p)} bytes, not {STATUS_BYTES}")
    (rid, end_ma, end_s, run_s, phase_s, uwh_in, uwh_out, uah_in, uah_out, eff) = struct.unpack_from("<3H6IH", p, 6)
    flags = p[5]
    return {
        "state": _name(STATES, p[0], "state"), "mode": _name(MODES, p[1], "mode"), "wiring": _name(WIRINGS, p[2], "wiring"),
        "phases": p[3], "result": _name(RESULTS, p[4], "result"),
        "write_failed": bool(flags & 1), "unwritten": bool(flags & 2), "monitor": bool(flags & 4),
        "efficiency_final": bool(flags & 8),
        "id": rid, "end_ma": end_ma, "end_s": end_s, "run_s": run_s, "phase_s": phase_s,
        "in_uwh": uwh_in, "out_uwh": uwh_out, "in_uah": uah_in, "out_uah": uah_out,
        "efficiency_permille": None if eff == NO_RATIO else eff,
        "runs_stored": p[38], "next_wiring": _name(WIRINGS, p[39], "wiring"),
    }


def _wh(uwh):
    return f"{uwh / 1e6:.3f} Wh"


def _mah(uah):
    return f"{uah / 1e3:.0f} mAh"


def _hms(s):
    return f"{s // 3600}h{s // 60 % 60:02d}m{s % 60:02d}s"


def render_record(r):
    lines = [f"run {r['id']}: {r['mode']}, {r['wiring']}, {r['state']}"
             + (f", resumed {r['resumes']}x after a reset" if r["resumes"] else "")
             + f"; ends under {r['end_ma']} mA for {r['end_s']} s"]
    for i, p in enumerate(r["phases"]):
        lines.append(f"  phase {i + 1} {p['kind']} ({p['end']}): {_hms(p['time_s'])}, in {_wh(p['in_uwh'])} {_mah(p['in_uah'])}, "
                     f"out {_wh(p['out_uwh'])} {_mah(p['out_uah'])}" + (f", {p['gaps']} gaps" if p["gaps"] else ""))
    if r["efficiency_permille"] is not None:
        lines.append(f"  efficiency {r['efficiency_permille'] / 10:.1f} % (Wh out / Wh in), "
                     f"charge ratio {r['charge_ratio_permille'] / 10:.1f} % (mAh)")
    lines.append(f"  curve: {len(r['curve'])} points every {r['curve_interval_s']} s")
    return "\n".join(lines)


def render_status(s):
    head = f"battery recorder: {s['state']}"
    if s["state"] != "idle":
        head += f", run {s['id']} {s['mode']} ({s['wiring']}), phase {s['phases']}"
    lines = [head]
    if s["state"] != "idle":
        lines.append(f"  in {_wh(s['in_uwh'])} {_mah(s['in_uah'])}, out {_wh(s['out_uwh'])} {_mah(s['out_uah'])}, "
                     f"time {_hms(s['run_s'])} (phase {_hms(s['phase_s'])})")
    if s["efficiency_permille"] is not None:
        lines.append(f"  efficiency {s['efficiency_permille'] / 10:.1f} %" + ("" if s["efficiency_final"] else " so far"))
    lines.append(f"  end rule: under {s['end_ma']} mA for {s['end_s']} s; next run wiring {s['next_wiring']}; "
                 f"{s['runs_stored']} runs stored" + ("; MONITOR OFFLINE" if not s["monitor"] else "")
                 + ("; last record write FAILED" if s["write_failed"] else "") + ("; record not written yet" if s["unwritten"] else ""))
    if s["result"] != "ok":
        lines.append(f"  refused: {s['result']}")
    return "\n".join(lines)
