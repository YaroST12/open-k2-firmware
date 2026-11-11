#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Turn a bench recording (tools/k2_stream.py output dir: trace.jsonl, power.jsonl, log.jsonl) into a PD world
scenario for the host world-replay test (tests/test_world_replay.cpp, docs/TESTING.md "World replay").

A scenario is the world the K2 sat in, not what the K2 did: VBUS over time, the CC level the measure block saw on
each line (the comparator truth), the ADC levels of CC1/CC2/D+/D-, and every PD frame on the wire with its line and
raw bytes. Frames the K2 missed (it attached late) are filled in from a template recording of the same charger and
sink, found by an identical raw frame (usually PS_RDY), and marked `tpl`. One episode per VBUS rise that carries PD
traffic, each with editable expectations.

    python3 tools/trace2scenario.py RECORDING_DIR -o tests/scenarios/NAME.scn [--template DIR] [--name NAME]

Rules (all conservative: they never make the world kinder to the K2 than the recording shows):
- VBUS: the trace VBUS records (exact time) plus power.jsonl samples with no trace record within 30 ms, or that repeat
  the trace record before them (the value still held then). Records are
  gated on change, so a value is held until 20 ms before the next record, then ramps to it. A rise caught mid-way
  (0.8..4.5 V) whose next record is far away continues at 0.25 V/ms (a charger's slew) up to the next value.
- Rp (comparator truth): on the attach line from the moment VBUS crosses 0.8 V in a rise (or the recorded attach,
  if that is earlier), at that line's ADC level (1680 mV when unknown); off where VBUS falls back below 0.8 V.
  The other line reads 0. Later ADC changes of more than 150 mV step the level. Where the K2's own scan rejected the
  line as VCONN (MEASURE raised to 0x3E and restored with no TXCC write), the line was above 2646 mV there: an open
  Rp step at that moment, which is often tens of ms before the next ADC sample shows it.
- Rises closer than 1.5 s are one attach cycle (one episode), named after the first.
- Episode: a VBUS rise with an RX frame within 1.5 s; t0 = rise - 800 ms, t1 = rise + 2500 ms (the PD verdict
  lands about 1.5 s after the attach), clipped before the next rise.
"""
import argparse
import json
import sys
from pathlib import Path

RISE_MV = 800
SLEW_MV_PER_MS = 250
HOLD_LEAD_MS = 20
DEFAULT_RP_MV = 1680
OPEN_MV = 2646        # the scan's VCONN threshold (MEASURE 0x3E)
OPEN_RP_MV = 3300     # an open Rp (no Rd on the line) when no ADC sample says better

CTRL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY", 7: "Get_Source_Cap",
        8: "Get_Sink_Cap", 9: "DR_Swap", 10: "PR_Swap", 11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset",
        14: "Data_Reset", 15: "Data_Reset_Complete", 16: "Not_Supported", 17: "Get_Source_Cap_Extended",
        18: "Get_Status", 19: "FR_Swap", 20: "Get_PPS_Status", 21: "Get_Country_Codes",
        22: "Get_Sink_Cap_Extended", 23: "Get_Source_Info", 24: "Get_Revision"}
DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities", 5: "Battery_Status",
        6: "Alert", 7: "Get_Country_Info", 8: "Enter_USB", 9: "EPR_Request", 10: "EPR_Mode", 11: "Source_Info",
        12: "Revision", 15: "Vendor_Defined"}


def msg_name(raw: bytes) -> str:
    """Name of a frame given as token + header (LE) + data + CRC."""
    if len(raw) < 3:
        return "?"
    h = raw[1] | raw[2] << 8
    t, n, ext = h & 0x1F, (h >> 12) & 7, (h >> 15) & 1
    if ext:
        return f"Extended_{t}"
    return (DATA.get(t, f"Data_{t}") if n else CTRL.get(t, f"Control_{t}"))


def sop_of(raw: bytes) -> str:
    tok = raw[0] & 0xE0
    return {0xE0: "SOP", 0xC0: "SOP'", 0xA0: "SOP''"}.get(tok, "?")


def read_jsonl(path: Path):
    if not path.exists():
        return []
    out = []
    for line in path.read_text().splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


def load(rec: Path):
    trace = read_jsonl(rec / "trace.jsonl")
    power = read_jsonl(rec / "power.jsonl")
    meta = json.loads((rec / "meta.json").read_text()) if (rec / "meta.json").exists() else {}
    return trace, power, meta


def vbus_records(trace, power):
    pts = []
    for r in trace:
        if r.get("type") == "VBUS" and r.get("raw"):
            b = bytes.fromhex(r["raw"])
            pts.append((float(r["t_ms"]), b[0] | b[1] << 8))
    pts.sort()
    times = [t for t, _ in pts]
    trace_mv = [mv for _, mv in pts]
    import bisect
    for r in power:
        if "vbus_mv" not in r:
            continue
        t = float(r["t_ms"])
        i = bisect.bisect_left(times, t)
        near = any(0 <= j < len(times) and abs(times[j] - t) <= 30 for j in (i - 1, i))
        # Near a trace record, a sample that repeats the record before it is kept: the value still held then (a rise that
        # paused at 1.4 V must not be ramped up through the pause).
        held = i > 0 and abs(int(r["vbus_mv"]) - trace_mv[i - 1]) <= 50 and abs(int(r["vbus_mv"])) > 50
        if not near or held:
            pts.append((t, int(r["vbus_mv"])))
    pts.sort()
    return pts


def vbus_timeline(records):
    """Expand gated records into explicit points (linear in between, see the module doc)."""
    out = []
    for i, (t, mv) in enumerate(records):
        if i:
            pt, pmv = records[i - 1]
            if t - pt > HOLD_LEAD_MS and abs(mv - pmv) > 100:
                hold_end = t - HOLD_LEAD_MS
                if RISE_MV <= pmv < 4500 and mv > pmv:   # a rise caught mid-way: continue at the slew
                    reach = pt + (mv - pmv) / SLEW_MV_PER_MS
                    if reach < hold_end:
                        out.append((reach, mv))
                        out.append((t, mv))
                        continue
                if out and out[-1][0] < hold_end:
                    out.append((hold_end, pmv))
        out.append((t, mv))
    dedup = []
    for p in out:
        if dedup and abs(dedup[-1][0] - p[0]) < 1e-6:
            dedup[-1] = p
        else:
            dedup.append(p)
    return dedup


def vbus_at(points, t):
    if not points or t <= points[0][0]:
        return points[0][1] if points else 0
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if t0 <= t <= t1:
            return v0 if t1 == t0 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)
    return points[-1][1]


def crossings(points):
    """(t, 'up'|'down') where VBUS crosses RISE_MV."""
    out = []
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        if v0 < RISE_MV <= v1:
            out.append((t0 + (RISE_MV - v0) * (t1 - t0) / (v1 - v0), "up"))
        elif v0 >= RISE_MV > v1:
            out.append((t0 + (v0 - RISE_MV) * (t1 - t0) / (v0 - v1), "down"))
    return out


def rx_frames(trace):
    out = []
    for r in trace:
        if r.get("type") == "RX" and r.get("raw"):
            out.append((float(r["t_ms"]), bytes.fromhex(r["raw"])))
    return out


def attaches(trace):
    out = []
    for r in trace:
        if r.get("type") == "CC" and r.get("raw") and str(r.get("text", "")).split()[1:2] == ["attach"]:
            b = bytes.fromhex(r["raw"])
            out.append((float(r["t_ms"]), b[1]))
    return out


def scan_rejects(trace):
    """The K2's own CC scans as Rp evidence, sharper than the 50 ms ADC samples: a scan that raised MEASURE to the VCONN
    threshold (0x3E, 2646 mV) on a line and then restored it with no SWITCHES1 (TXCC) write in between rejected that line as
    VCONN, so its level was above 2646 mV at that moment (an open Rp: no sink Rd on it). An accepted line gets its
    SWITCHES1 write (apply_cc_choice) within about 10 ms of the raise. Returns (t_ms, line)."""
    regs = [(float(r["t_ms"]), bytes.fromhex(r["raw"])) for r in trace
            if r.get("type") == "REG" and r.get("raw") and len(r["raw"]) >= 4]
    out = []
    for i, (t, b) in enumerate(regs):
        if b[0] != 0x04 or b[1] != 0x3E:
            continue
        line = next((2 if p[1] & 0x08 else 1 for _, p in reversed(regs[:i]) if p[0] == 0x02 and p[1] & 0x0C), None)
        after = [p for tt, p in regs[i + 1:] if tt - t <= 15.0]
        restored = any(p[0] == 0x04 for p in after)
        if line and restored and not any(p[0] == 0x03 for p in after):
            out.append((t, line))
    return out


def adc_steps(power):
    out = []
    for r in power:
        if all(k in r for k in ("cc1_mv", "cc2_mv", "dp_mv", "dm_mv")):
            s = (int(r["cc1_mv"]), int(r["cc2_mv"]), int(r["dp_mv"]), int(r["dm_mv"]))
            if not out or out[-1][1] != s:
                out.append((float(r["t_ms"]), s))
    return out


def adc_at(steps, t):
    cur = None
    for st, s in steps:
        if st > t:
            break
        cur = s
    return cur


def template_fill(first_frame, before_ms, tpl_frames, max_back_ms=1000.0, max_gap_ms=600.0):
    """Frames of the template that precede `first_frame` (t, raw) there, shifted onto this recording's time.
    The template occurrence chosen is the first identical raw frame with a Source_Capabilities before it."""
    t_rec, raw = first_frame
    for i, (tt, traw) in enumerate(tpl_frames):
        if traw != raw:
            continue
        window = []
        j = i - 1
        last = tt
        while j >= 0 and tt - tpl_frames[j][0] <= max_back_ms and last - tpl_frames[j][0] <= max_gap_ms:
            window.append(tpl_frames[j])
            last = tpl_frames[j][0]
            j -= 1
        window.reverse()
        if not any(sop_of(w[1]) == "SOP" and msg_name(w[1]) == "Source_Capabilities" for w in window):
            continue
        off = t_rec - tt
        return off, [(w[0] + off, w[1]) for w in window if w[0] + off < before_ms]
    return None, []


def build(rec: Path, template: Path | None, name: str):
    trace, power, meta = load(rec)
    fw = meta.get("hello", {}).get("firmware", "?")
    points = vbus_timeline(vbus_records(trace, power))
    adc = adc_steps(power)
    frames = rx_frames(trace)
    atts = attaches(trace)
    rejects = scan_rejects(trace)
    tpl_frames = rx_frames(load(template)[0]) if template else frames
    cross = crossings(points)
    rises = [t for t, d in cross if d == "up"]
    downs = [t for t, d in cross if d == "down"]

    # Rises closer than 1.5 s are one attach cycle (a source that blips VBUS before it stays up).
    clusters = []
    for r in rises:
        if clusters and r - clusters[-1][-1] < 1500:
            clusters[-1].append(r)
        else:
            clusters.append([r])

    def line_of(t_on, t_off):
        att = [a for a in atts if t_on - 500 <= a[0] < t_off]
        if att:
            return att[0][1], att[0][0]
        lv = adc_at(adc, t_on + 30)
        return (2 if lv and lv[1] > lv[0] else 1), None

    def level(mv):   # the comparator truth from the ADC net level of the live line
        return mv if mv > 2400 else (mv if 1000 <= mv <= 2400 else DEFAULT_RP_MV)

    episodes, rp, all_frames = [], [], []
    for cl in clusters:
        first_rise, last_rise = cl[0], cl[-1]
        nxt = next((r for r in rises if r > last_rise), float("inf"))
        ep_frames = [f for f in frames if first_rise <= f[0] < min(last_rise + 1500, nxt)]
        cyc_line = None
        for r in cl:   # Rp per rise: on at the crossing (or the recorded attach, if earlier), off at the fall
            off = next((d for d in downs if d > r), float("inf"))
            line, att_t = line_of(r, min(off, nxt))
            cyc_line = cyc_line or (line if att_t is not None else None)
            on = min(r, att_t) if att_t is not None else r
            lv = adc_at(adc, on + 30)
            rp.append((on, line, level(lv[line - 1]) if lv else DEFAULT_RP_MV))
            # Later steps: the ADC samples, and the scan's VCONN rejects (the line open earlier than the next sample shows).
            ev = [(st, level(s[line - 1])) for st, s in adc if on + 30 < st < off]
            for st, sl in rejects:
                if sl == line and on < st < off:
                    nxt_adc = next((s for at, s in adc if at >= st), None)
                    ev.append((st - 0.5, nxt_adc[line - 1] if nxt_adc and nxt_adc[line - 1] > OPEN_MV else OPEN_RP_MV))
            for st, lv in sorted(ev):
                if abs(lv - rp[-1][2]) > 150:
                    rp.append((st, line, lv))
            if off != float("inf"):
                rp.append((off, line, 0))
        if not ep_frames:
            continue
        t0 = first_rise - 800
        if episodes and t0 < episodes[-1]["t1"]:
            t0 = episodes[-1]["t1"]
        t1 = min(last_rise + 2500, nxt - 900)
        line = cyc_line or line_of(first_rise, nxt)[0]   # no attach recorded (the K2 stayed attached): the live ADC line

        def frame_line(t):   # the newest attach of this episode before the frame (the K2 decodes on its routed line)
            a = [x for x in atts if t0 <= x[0] <= t]
            return a[-1][1] if a else line
        ep_all = [(t, frame_line(t), raw, "rec") for t, raw in frames if t0 <= t < t1]
        first = ep_frames[0]
        first_sop = next((f for f in ep_frames if sop_of(f[1]) == "SOP"), first)
        note = ""
        if msg_name(first_sop[1]) != "Source_Capabilities":
            off_ms, fill = template_fill(first_sop, first_sop[0], tpl_frames)
            if fill:
                ep_all += [(t, frame_line(first_sop[0]), raw, "tpl") for t, raw in fill if t >= t0]
                note = f" template_offset {off_ms:.1f}"
        ep_all.sort(key=lambda f: f[0])
        all_frames += ep_all
        episodes.append({"name": f"rise{first_rise / 1000:.1f}", "t0": t0, "t1": t1, "rise": first_rise,
                         "line": line, "note": note, "first_rec": msg_name(first_sop[1])})

    lines = [f"scenario 1", f"name {name}", f"recording {short(rec)} fw {fw}"]
    if template:
        lines.append(f"template {short(template)}")
    lines.append("mode inline")

    def kept(t):   # only the world around the episodes (each run settles from 3 s before its t0)
        return any(e["t0"] - 3500 <= t <= e["t1"] + 100 for e in episodes)
    lines.append("# vbus <t_ms> <mV>: linear between points")
    for i, (t, mv) in enumerate(points):
        nb = (points[i + 1][0] if i + 1 < len(points) else t)
        if kept(t) or kept(nb):
            lines.append(f"vbus {t:.3f} {int(mv)}")
    lines.append("# adc <t_ms> <cc1> <cc2> <dp> <dm> (mV, held until the next step)")
    for i, (t, (a, b, c, d)) in enumerate(adc):
        nb = (adc[i + 1][0] if i + 1 < len(adc) else t)
        if kept(t) or kept(nb):
            lines.append(f"adc {t:.3f} {a} {b} {c} {d}")
    lines.append("# rp <t_ms> <cc1_mv> <cc2_mv>: the level the measure block sees (comparator truth), held")
    for i, (t, line, lv) in enumerate(rp):
        nb = (rp[i + 1][0] if i + 1 < len(rp) else t)
        if kept(t) or kept(nb):
            lines.append(f"rp {t:.3f} {lv if line == 1 else 0} {lv if line == 2 else 0}")
    lines.append("# frame <t_ms> <line> <token+header+data+crc hex> rec|tpl <name>")
    for t, line, raw, src in all_frames:
        lines.append(f"frame {t:.3f} {line} {raw.hex()} {src} {sop_of(raw)}:{msg_name(raw)}")
    lines.append("# episode <name> <t0> <t1> line <n> rise <t>; expect <episode> <checks> (edit by hand)")
    for e in episodes:
        lines.append(f"episode {e['name']} {e['t0']:.1f} {e['t1']:.1f} line {e['line']} rise {e['rise']:.1f}"
                     f"  # first recorded frame {e['first_rec']}{e['note']}")
        lines.append(f"expect {e['name']} first_sop=Source_Capabilities caps>=1 verdict=present")
    return "\n".join(lines) + "\n", episodes


def short(p: Path) -> str:
    s = str(p.resolve())
    i = s.rfind("/runs/")
    return s[i + 1:] if i >= 0 else p.name


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("recording", type=Path)
    ap.add_argument("-o", "--out", type=Path, required=True)
    ap.add_argument("--template", type=Path, help="recording of the same charger and sink with the full exchange "
                                                  "(default: the recording itself, its passing attaches)")
    ap.add_argument("--name", help="scenario name (default: the output file stem)")
    a = ap.parse_args(argv)
    text, eps = build(a.recording, a.template, a.name or a.out.stem)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_text(text)
    for e in eps:
        print(f"{e['name']}: t0={e['t0']:.0f} t1={e['t1']:.0f} line CC{e['line']} first recorded {e['first_rec']}"
              f"{e['note']}")
    print(f"wrote {a.out} ({len(eps)} episodes)")
    return 0 if eps else 1


if __name__ == "__main__":
    sys.exit(main())
