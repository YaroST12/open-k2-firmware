#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Records and reads the K2's multiplexed live debug stream (docs/DEBUG_STREAMING.md).

The firmware pushes log entries, PD black box records, power/CC samples, charger-test run events and
a heartbeat down the CDC shell as framed records (include/k2/core/dbg_stream.hpp). The CDC port needs no
device lock, so this runs next to ct_start.py and k2_remote.py (they use the HID link).

  k2_stream.py record --out runs/NAME [--kinds all|log,trace,power=50,run] [--old] [--seconds N]
                      [--until-run-end] [--port /dev/serial/by-id/usb-...-ifNN] [--echo run,log,gap | --quiet]
  k2_stream.py view runs/NAME [--kind run,log] [--grep REGEX] [--from MS] [--to MS] [--wall] [--summary]

`record` writes one folder: one jsonl per kind (trace, log, power, run, beat, gaps, boot), trace.raw
(`pd_trace.py decode` reads it), meta.json (firmware hash, mapping of the K2 clock to wall time, the
device's own counters against what arrived) and timeline.txt (everything merged, by absolute K2
time in ms since the K2 booted). `view` rebuilds that timeline from the jsonl files and filters it.
Every timeline line starts with the time and the kind, so grep works:

    grep -E ' RUN | GAP ' runs/NAME/timeline.txt
"""
import argparse
import json
import os
import re
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pd_trace  # noqa: E402

K_HELLO, K_TEXT, K_LOG, K_POWER, K_RUN, K_GAP, K_BEAT, K_BOOT = 4, 5, 6, 7, 8, 9, 10, 11
RESET_BITS = ((26, "PIN"), (27, "POR"), (28, "SW"), (29, "IWDG"), (30, "WWDG"), (31, "LPWR"))   # CRM CTRLSTS flags
TRACE_KINDS = (1, 2, 3)
UNREAD = 0xFFFF
JSONL = ("boot", "trace", "log", "power", "run", "beat", "gaps")   # file stems; timeline order of equal times

FALLBACK_PORT = "/dev/ttyACM0"   # when no by-id entry exists (non-Linux hosts)
BY_ID_GLOB = "usb-WITRN_Open-K2_Logger_*-if*"   # the logger's product string; the CDC shell's control interface


def default_port(by_id_dir="/dev/serial/by-id"):
    """The K2 debug shell's stable /dev/serial/by-id path (the product string is "Open-K2 Logger", so the
    CDC interface is usb-WITRN_Open-K2_Logger_<serial>-ifNN regardless of enumeration order), falling back
    to /dev/ttyACM0 when the host has no by-id entries."""
    import glob
    hits = sorted(glob.glob(os.path.join(by_id_dir, BY_ID_GLOB)))
    return hits[0] if hits else FALLBACK_PORT


# ---- enum names from the firmware headers (the stream carries numbers) ---------------------------

def _strip_comments(s):
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", s, flags=re.S)


def parse_enum(text, name, needs=None):
    """Enumerator names of `enum class NAME ... { ... }` in order ('= n' honoured); `needs` picks one of several."""
    for m in re.finditer(r"enum\s+class\s+" + name + r"\b[^{;]*\{([^}]*)\}", text):
        body = _strip_comments(m.group(1))
        if needs and needs not in body:
            continue
        names, n = [], 0
        for part in body.split(","):
            part = part.strip()
            if not part:
                continue
            ident, _, val = part.partition("=")
            if val.strip():
                try:
                    n = int(val.strip(), 0)
                except ValueError:
                    pass
            while len(names) < n:
                names.append(str(len(names)))
            names.append(ident.strip())
            n += 1
        return names
    return []


class Names:
    """verdict / reason / state / group / algorithm / phase / stage names of the charger test; a number when unknown."""

    def __init__(self, root=ROOT):
        def read(p):
            try:
                return (root / p).read_text()
            except OSError:
                return ""
        ct, ui = read("include/k2/chargertest/charger_test.hpp"), read("include/k2/ui/ui_pages.hpp")
        self.tab = {n.lower(): parse_enum(ct, n) for n in ("Verdict", "Reason", "State", "Group", "Algorithm", "Phase", "Mode", "Unit")}
        self.tab["stage"] = parse_enum(ui, "Stage", needs="Algorithms")

    def get(self, kind, n):
        t = self.tab.get(kind, [])
        return t[n] if 0 <= n < len(t) else f"#{n}"


# ---- decoding of the kinds ---------------------------------------------------------------------

def reset_names(flags):
    """The reset-cause flags of a Boot frame as 'POR PIN' ('-' when none is set)."""
    return " ".join(n for b, n in RESET_BITS if flags >> b & 1) or "-"


class Unwrap32:
    """A 32-bit millisecond counter as an ever-growing number (the K2 wraps after 49 days; a session may not)."""

    def __init__(self):
        self.last = None
        self.base = 0

    def __call__(self, v):
        if self.last is not None and v < self.last and self.last - v > 1 << 31:
            self.base += 1 << 32
        self.last = v
        return self.base + v


def mv(v):
    return "-" if v == UNREAD else f"{v}"


def run_text(names, code, a, b, c, d):
    g = names.get
    return {
        1: lambda: f"run start units={a} rows={b}",
        2: lambda: f"stage {g('stage', a)}",
        3: lambda: f"row {a} start group={g('group', b)} algo={g('algorithm', c)} pos={d}",
        4: lambda: f"row {a} end {g('verdict', b)} reason={g('reason', c)} measured={d} mV",
        5: lambda: f"row {a} request tries={b} {c} mV {d} mA",
        6: lambda: f"unit attach mode={g('mode', a)}",
        7: lambda: "unit detach",
        8: lambda: f"unit hold mode={g('mode', a)}",
        9: lambda: f"identity {'armed' if a else 'disarmed'}",
        10: lambda: f"run suspended at row {a}",
        11: lambda: f"run resumed at row {a}",
        12: lambda: f"run end state={g('state', a)} rows_finished={b} reason={g('reason', c)}",
        13: lambda: f"phase {g('phase', a)} row {b}",
        # a burst of skipped rows with one reason is one event; the rows are contiguous, so the first is b - d + 1 (a clamps at 255)
        14: lambda: (f"row {b} end Skip reason={g('reason', c)}" if d <= 1 else f"rows {b - d + 1}..{b} end Skip reason={g('reason', c)} ({d} rows)"),
    }.get(code, lambda: f"event {code} a={a} b={b} c={c} d={d}")()


def parse_stats_line(tail):
    """The device's `stream stats` line after `stream off`: dict of counters, None when it never arrived."""
    m = re.search(rb"stream stats frames=(\d+) trace=(\d+)/(\d+) log=(\d+)/(\d+) run=(\d+)/(\d+) power=(\d+) folded=(\d+) same=(\d+) stalls=(\d+)", tail)
    if not m:
        return None
    k = ("frames", "trace", "trace_lost", "log", "log_lost", "run", "run_lost", "power", "power_folded", "power_same", "stalls")
    return dict(zip(k, (int(x) for x in m.groups())))


class _TraceSink:
    """Where pd_trace.TraceStream puts what it decodes: into the session's files."""

    def __init__(self, session):
        self.s = session

    def event(self, ev):
        self.s._row("trace", {"t_ms": ev["t_ms"], "text": ev["text"], "detail": ev["detail"], "dt_ms": ev["dt_ms"],
                              "type": ev["type"], "raw": ev["raw"]})

    def note(self, text, marker):
        self.s._gap("trace", text, marker)

    def raw(self, data):
        if self.s.raw_file:
            self.s.raw_file.write(data)
            self.s.raw_file.flush()


class Session:
    """Consumes the CDC bytes (feed), keeps the files of one recording (out dir) and the counts that prove completeness."""

    def __init__(self, out=None, db_for=None, names=None, wall=time.time, echo=None):
        self.out = Path(out) if out else None
        self.db_for = db_for or default_db_for
        self.names = names or Names()
        self.wall = wall
        self.echo = echo           # f(kind, line) for the live console, None = silent
        self.files, self.raw_file = {}, None
        if self.out:
            self.out.mkdir(parents=True, exist_ok=True)
            self.raw_file = open(self.out / "trace.raw", "wb")
        self.trace = pd_trace.TraceStream(_TraceSink(self))
        self.pend, self.tail = b"", b""
        self.frames = self.bad = 0
        self.hello, self.db, self.db_source, self.db_warning = None, {}, None, None
        self.wall0 = None          # wall time of K2 ms 0 as estimated from the Hello
        self.boot = bytearray()
        self.boot_id = self.reset_flags = None   # from the Boot frame (None on firmware that does not send one)
        self.boot_index = 1                      # which boot of this recording the rows belong to
        self.t_off = 0.0                         # K2 clocks restart at 0 on every boot; later boots are shifted to follow the last row
        self.prior = []                          # (index, boot id, ok, summary lines) of the boots already closed
        self.counts = self._fresh_counts()
        self.log_expect = None
        self.last_beat = None
        self.last_ms = 0.0
        self.unwrap = self._fresh_unwrap()
        self.closed_stats = None
        self.rows_written = 0

    @staticmethod
    def _fresh_counts():
        return dict(log=0, run=0, power=0, beat=0, gap_log=0, gap_run=0, wire_log=0, bad_frames=0, run_end=0)

    @staticmethod
    def _fresh_unwrap():
        return {k: Unwrap32() for k in ("log", "power", "run", "beat", "hello", "text")}

    # -- output
    def _row(self, kind, row):
        row = dict(row, kind=kind, boot_index=self.boot_index)
        if kind != "gap":              # gap rows reuse last_ms, which is already on the recording's clock
            row["t_ms"] = row["t_ms"] + self.t_off
        row["wall"] = round(self.wall(), 3)
        self.last_ms = max(self.last_ms, row["t_ms"])
        self.rows_written += 1
        stem = {"gap": "gaps", "hello": "boot", "boot": "boot"}.get(kind, kind)
        if self.out:
            f = self.files.get(stem)
            if f is None:
                f = self.files[stem] = open(self.out / f"{stem}.jsonl", "w")
            f.write(json.dumps(row) + "\n")
            f.flush()
        if self.echo:
            self.echo(kind, format_row(row))

    def _gap(self, src, text, marker):
        self._row("gap", {"t_ms": self.last_ms, "src": src, "text": text.strip("- "), "marker": marker})

    def _host(self, text, marker=None):
        self._gap("host", text, marker or {"marker": "host"})

    # -- input
    def feed(self, data):
        self.tail = (self.tail + data)[-512:]
        frames, bad, self.pend = pd_trace.parse_frames(self.pend + data)
        if bad:
            self.bad += bad
            self.counts["bad_frames"] += bad
            self._host(f"{bad} frame(s) dropped: bad CRC (bytes lost on the wire)", {"marker": "bad_crc", "frames": bad})
        for kind, pl in frames:
            self.frames += 1
            try:
                self._dispatch(kind, pl)
            except (struct.error, IndexError, ValueError) as e:
                self._host(f"kind {kind} frame of {len(pl)} bytes could not be read: {e}", {"marker": "bad_frame", "kind": kind})

    def _dispatch(self, kind, pl):
        if kind in TRACE_KINDS:
            self.trace.handle(kind, pl)
        elif kind == K_HELLO:
            self._hello(pl)
        elif kind == K_TEXT:
            self._text(pl)
        elif kind == K_LOG:
            self._log(pl)
        elif kind == K_POWER:
            self._power(pl)
        elif kind == K_RUN:
            self._run(pl)
        elif kind == K_GAP:
            self._gapframe(pl)
        elif kind == K_BEAT:
            self._beat(pl)
        elif kind == K_BOOT:
            self._boot(pl)

    def _boundary(self, why):
        """The K2 restarted inside this recording: close the boot's accounting, restart the clocks after the last row."""
        if self.boot:
            self._row("boot", {"t_ms": self.last_ms - self.t_off, "text": bytes(self.boot).decode("utf-8", "replace").rstrip("\r\n")})
            self.boot = bytearray()
        ok, lines = self.summary()
        self.prior.append((self.boot_index, self.boot_id, ok, lines))
        self.t_off = self.last_ms + 1.0
        self.boot_index += 1
        self.boot_id = self.reset_flags = None
        self.counts = self._fresh_counts()
        self.unwrap = self._fresh_unwrap()
        self.log_expect = self.last_beat = None
        self.tail, self.bad = b"", 0
        self.trace = pd_trace.TraceStream(_TraceSink(self))
        self._row("boot", {"t_ms": 0.0, "marker": "boot_boundary",
                           "text": f"BOOT BOUNDARY: the K2 restarted ({why}); its clock restarts here, rows below belong to boot {self.boot_index} of this recording"})

    def _boot(self, pl):
        boots, flags = struct.unpack_from("<HI", pl)
        if self.boot_id is not None and boots != self.boot_id:
            self._boundary(f"boot id {self.boot_id} -> {boots}")   # a restart whose clock did not go back
        self.boot_id, self.reset_flags = boots, flags
        self._row("boot", {"t_ms": float(self.hello["ms"]) if self.hello else 0.0, "boot_id": boots, "reset_flags": flags,
                           "text": f"boot #{boots} reset={reset_names(flags)} (CTRLSTS 0x{flags:08X})"})

    def _hello(self, pl):
        ver, mask, hz, ms, flags = struct.unpack_from("<BBHIB", pl)
        if self.hello is not None and ms < self.last_ms - self.t_off:
            self._boundary(f"its clock went back from {self.last_ms - self.t_off:.0f} ms to {ms} ms")
        fw = pl[9:].decode("ascii", "replace")
        self.hello = {"version": ver, "mask": mask, "power_hz": hz, "ms": ms, "history": bool(flags & 1), "firmware": fw}
        self.wall0 = self.wall() - ms / 1000.0
        self.db, self.db_source, self.db_warning = self.db_for(fw)
        kinds = [n for b, n in ((1, "log"), (2, "trace"), (4, "power"), (8, "run")) if mask & b]
        self._row("hello", {"t_ms": float(ms), "firmware": fw, "mask": mask, "power_hz": hz, "history": bool(flags & 1),
                            "logdb": self.db_source, "text": f"stream start fw={fw} kinds={','.join(kinds)}"
                            + (f" power={hz}Hz" if mask & 4 else "") + (" from the oldest" if flags & 1 else "")
                            + (f"  [{self.db_warning}]" if self.db_warning else "")})
        self.write_meta()

    def _text(self, pl):
        ms = self.unwrap["text"](struct.unpack_from("<I", pl)[0])
        self.boot += pl[5:]
        while b"\n" in self.boot:
            line, _, rest = bytes(self.boot).partition(b"\n")
            self.boot = bytearray(rest)
            self._row("boot", {"t_ms": float(ms), "text": line.decode("utf-8", "replace").rstrip("\r")})

    def _log(self, pl):
        base, seq = struct.unpack_from("<II", pl)
        entries = pl[8:]
        import k2_qr
        rows = k2_qr.decode_log_entries(entries, self.db, base)
        if self.log_expect is not None and seq != self.log_expect:
            missing = (seq - self.log_expect) & 0xFFFFFFFF
            self.counts["wire_log"] += missing
            self._host(f"{missing} log entries missing between frames (seq {self.log_expect} -> {seq}): lost on the wire",
                       {"marker": "log_wire_gap", "entries": missing})
        self.log_expect = (seq + len(rows)) & 0xFFFFFFFF
        u = self.unwrap["log"]
        for i, (t, mid, text) in enumerate(rows):
            self.counts["log"] += 1
            self._row("log", {"t_ms": float(u(t)), "seq": seq + i, "id": mid, "text": text})

    def _power(self, pl):
        ms, n, valid, vbus, ibus, cc1, cc2, dp, dm = struct.unpack_from("<IHBHhHHHH", pl)
        row = {"t_ms": float(self.unwrap["power"](ms)), "n": n, "valid": bool(valid), "vbus_mv": vbus, "ibus_ma": ibus,
               "cc1_mv": None if cc1 == UNREAD else cc1, "cc2_mv": None if cc2 == UNREAD else cc2,
               "dp_mv": None if dp == UNREAD else dp, "dm_mv": None if dm == UNREAD else dm}
        text = (f"VBUS {vbus} mV IBUS {ibus} mA CC1 {mv(cc1)} CC2 {mv(cc2)} D+ {mv(dp)} D- {mv(dm)}" if valid
                else f"no power reading CC1 {mv(cc1)} CC2 {mv(cc2)} D+ {mv(dp)} D- {mv(dm)}")
        if len(pl) >= 29:
            span, vlo, vhi, ilo, ihi = struct.unpack_from("<HHHhh", pl, 19)
            row.update(span_ms=span, vbus_min=vlo, vbus_max=vhi, ibus_min=ilo, ibus_max=ihi)
            text += f"  [{n} samples over {span} ms: VBUS {vlo}..{vhi} mV, IBUS {ilo}..{ihi} mA]"
        self.counts["power"] += 1
        row["text"] = text
        self._row("power", row)

    def _run(self, pl):
        ms, code, a, b, c, d = struct.unpack_from("<IBBHHH", pl)
        self.counts["run"] += 1
        if code == 12:
            self.counts["run_end"] += 1
        self._row("run", {"t_ms": float(self.unwrap["run"](ms)), "code": code, "a": a, "b": b, "c": c, "d": d,
                          "text": run_text(self.names, code, a, b, c, d)})

    def _gapframe(self, pl):
        src, count, nbytes, flags = struct.unpack_from("<BIIB", pl)
        what = {2: "log entries", 3: "run events"}.get(src, f"source {src} records")
        if src == 2:
            self.counts["gap_log"] += count
            if self.log_expect is not None:
                self.log_expect = (self.log_expect + count) & 0xFFFFFFFF
        elif src == 3:
            self.counts["gap_run"] += count
        self._row("gap", {"t_ms": self.last_ms, "src": {2: "log", 3: "run"}.get(src, str(src)), "lost": count, "bytes": nbytes,
                          "text": f"DEVICE LOST {count} {what} ({nbytes} bytes): the ring wrapped before they were sent",
                          "marker": {"marker": "device_gap", "src": src}})

    def _beat(self, pl):
        ms, stack, msp, page, flags, usbd = struct.unpack_from("<IHHBBI", pl)
        names = ("trace_records", "trace_lost", "log_entries", "log_lost", "run_events", "run_lost", "power_frames",
                 "power_folded", "stalls")
        ctr = dict(zip(names, struct.unpack_from("<9I", pl, 14)))
        self.last_beat = ctr
        self.counts["beat"] += 1
        self._row("beat", {"t_ms": float(self.unwrap["beat"](ms)), "stack_free": stack, "msp_free": msp, "page": page,
                           "flags": flags, "usb_dropped": usbd, "counters": ctr,
                           "text": f"stack_free={stack} msp_free={msp} page={page} usb_log_dropped={usbd} "
                                   f"lost: trace={ctr['trace_lost']} log={ctr['log_lost']} run={ctr['run_lost']} stalls={ctr['stalls']}"})

    # -- end of the session
    def summary(self):
        """Per kind: what the device says it sent against what arrived, and every loss that was reported."""
        st = parse_stats_line(self.tail)
        src = "stream stats line"
        if st is None and self.last_beat:
            b = self.last_beat
            st = {"trace": b["trace_records"], "trace_lost": b["trace_lost"], "log": b["log_entries"], "log_lost": b["log_lost"],
                  "run": b["run_events"], "run_lost": b["run_lost"], "power": b["power_frames"], "stalls": b["stalls"]}
            src = "last heartbeat (no `stream stats` line arrived)"
        c = self.counts
        got = {"trace": self.trace.records, "log": c["log"], "run": c["run"], "power": c["power"]}
        lines, ok = [], True
        for k in ("trace", "log", "run", "power"):
            dev = None if st is None else st.get(k)
            lost = (st or {}).get(k + "_lost")
            gap = {"trace": self.trace.lost_records, "log": c["gap_log"], "run": c["gap_run"]}.get(k)
            s = f"{k:<5} received {got[k]}"
            if dev is not None:
                s += f", device sent {dev}"
                if got[k] != dev:
                    ok = False
                    s += f"  <-- {abs(dev - got[k])} {'missing' if dev > got[k] else 'extra'} (lost on the wire or frames cut at the end)"
            if gap is not None:
                s += f", device reported {gap} lost"
                if lost is not None and lost != gap:
                    s += f" (device counted {lost})"
            lines.append(s)
        if c["wire_log"] or self.bad:
            ok = False
            lines.append(f"wire: {c['wire_log']} log entries missing between frames, {self.bad} frames with a bad CRC")
        if st and st.get("power_folded"):
            lines.append(f"power: {st['power_folded']} samples folded into min/max windows, {st.get('power_same', 0)} unchanged skipped")
        lines.insert(0, f"counters from the {src}" if st else "no device counters (no stats line, no heartbeat)")
        if self.prior:   # a restart inside the recording: each boot is judged against its own device counters
            head = []
            for idx, bid, pok, plines in self.prior:
                head.append(f"boot {idx}" + (f" (#{bid})" if bid is not None else "") + f": {'complete' if pok else 'INCOMPLETE'}")
                head += ["  " + x for x in plines]
                ok = ok and pok
            lines = head + [f"boot {self.boot_index}" + (f" (#{self.boot_id})" if self.boot_id is not None else "") + " (last):"] + ["  " + x for x in lines]
        return ok and st is not None, lines

    def write_meta(self):
        if not self.out:
            return
        ok, lines = self.summary()
        meta = {"hello": self.hello, "logdb": self.db_source, "logdb_warning": self.db_warning,
                "k2_ms0_wall": self.wall0, "frames": self.frames, "bad_frames": self.bad, "counts": self.counts,
                "complete": ok, "summary": lines, "boots": self.boot_index, "boot_id": self.boot_id,
                "reset_flags": self.reset_flags, "boot": bytes(self.boot).decode("utf-8", "replace")}
        (self.out / "meta.json").write_text(json.dumps(meta, indent=1) + "\n")

    def close(self):
        if self.boot:
            self._row("boot", {"t_ms": self.last_ms, "text": bytes(self.boot).decode("utf-8", "replace").rstrip("\r\n")})
            self.boot = bytearray()
        ok, lines = self.summary()
        self.write_meta()
        for f in list(self.files.values()) + ([self.raw_file] if self.raw_file else []):
            f.close()
        self.files, self.raw_file = {}, None
        if self.out:
            (self.out / "timeline.txt").write_text(timeline_text(self.out))
        return ok, lines


def default_db_for(fw):
    """(id -> format, source, warning) of the firmware named in the Hello."""
    import k2_qr
    return k2_qr.logdb_for(fw)


# ---- timeline and view -------------------------------------------------------------------------

def format_row(row, wall0=None):
    kind = {"gap": "GAP", "hello": "START"}.get(row["kind"], row["kind"].upper())
    t = f"{row['t_ms']:12.3f} ms"
    if wall0 is not None:
        t = time.strftime("%H:%M:%S", time.localtime(wall0 + row["t_ms"] / 1000)) + f".{int((wall0 + row['t_ms'] / 1000) % 1 * 1000):03d} " + t
    out = f"{t}  {kind:<5} {row['text']}"
    for d in row.get("detail") or []:
        out += "\n" + " " * 21 + "| " + d
    return out


def load_rows(d):
    rows = []
    for order, stem in enumerate(JSONL):
        p = Path(d) / f"{stem}.jsonl"
        if not p.exists():
            continue
        for n, line in enumerate(p.read_text().splitlines()):
            if line.strip():
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if "t_ms" in r:
                    rows.append((r["t_ms"], order, n, r))
    rows.sort(key=lambda x: x[:3])
    return [r for _, _, _, r in rows]


def load_meta(d):
    try:
        return json.loads((Path(d) / "meta.json").read_text())
    except (OSError, ValueError):
        return {}


def timeline_text(d, kinds=None, grep=None, t0=None, t1=None, wall=False):
    meta = load_meta(d)
    wall0 = meta.get("k2_ms0_wall") if wall else None
    rx = re.compile(grep) if grep else None
    if kinds and "start" in kinds:
        kinds = set(kinds) | {"hello"}
    head = []
    h = meta.get("hello")
    if h:
        w = meta.get("k2_ms0_wall")
        head.append(f"# firmware {h['firmware']}, K2 clock ms since boot"
                    + (f"; K2 time 0 = {time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(w))} local (estimated)" if w else ""))
    out = []
    for r in load_rows(d):
        if kinds and r["kind"] not in kinds and not (r["kind"] == "gap" and "gap" in kinds):
            continue
        if (t0 is not None and r["t_ms"] < t0) or (t1 is not None and r["t_ms"] > t1):
            continue
        line = format_row(r, wall0)
        if rx and not rx.search(line):
            continue
        out.append(line)
    return "\n".join(head + out) + "\n"


# ---- device access -----------------------------------------------------------------------------

def record(a, clock=time.monotonic):
    import fcntl
    import select
    import sniff_stream
    echo_kinds = set() if a.quiet else set(a.echo.split(","))
    echo_kinds |= {"start"}
    lines = []

    def echo(kind, line):
        k = {"hello": "start"}.get(kind, kind)
        if "all" in echo_kinds or k in echo_kinds or kind == "gap":
            print(line, flush=True)
        lines.append(1)

    sess = Session(a.out, echo=echo)
    fd = sniff_stream.open_port(a.port)
    try:
        fcntl.ioctl(fd, 0x540C)    # TIOCEXCL: a second reader on the port would steal bytes
    except OSError:
        pass

    def pump(duration):
        end = clock() + duration
        while clock() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                sess.feed(os.read(fd, 4096))

    kinds = [k for k in a.kinds.replace(" ", "").split(",") if k] or ["all"]
    cmd = "stream on " + " ".join(kinds) + (" old" if a.old else "") + "\r\n"
    # SIGTERM (a supervisor stopping the recorder) ends the session like Ctrl-C: "stream off" is
    # sent and meta.json written. Without this the device kept streaming and the next recorder
    # never got a fresh hello, so it had no logdb and printed "msg#N ?".
    import signal
    prev_term = signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        os.write(fd, b"\r\n")
        os.write(fd, b"stream off\r\n")   # a stream left on by a recorder that died: restart it for a new hello
        end = clock() + 0.5                 # drain the old stream's tail without recording it
        while clock() < end:
            if select.select([fd], [], [], 0.05)[0]:
                os.read(fd, 4096)
        os.write(fd, cmd.encode())
        t0, end_seen = clock(), None
        try:
            while a.seconds is None or clock() - t0 < a.seconds:
                pump(0.1)
                if a.until_run_end and sess.counts["run_end"]:
                    end_seen = end_seen or clock()
                    if clock() - end_seen > a.settle:
                        break
        except KeyboardInterrupt:
            pass
        os.write(fd, b"stream off\r\n")
        pump(1.0)
    finally:
        signal.signal(signal.SIGTERM, prev_term)
        os.close(fd)
    ok, summary = sess.close()
    return sess, ok, summary


def cmd_view(a):
    d = Path(a.dir)
    if not d.is_dir():
        print(f"no such session folder: {d}", file=sys.stderr)
        return 2
    if a.summary:
        meta = load_meta(d)
        print("\n".join(meta.get("summary", ["no meta.json"])))
        return 0
    kinds = set(a.kind.split(",")) if a.kind else None
    sys.stdout.write(timeline_text(d, kinds, a.grep, a.t_from, a.t_to, a.wall))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("record", help="stream everything from the CDC shell into a session folder")
    r.add_argument("--out", required=True, help="session folder, e.g. runs/ct-2026-11-07")
    r.add_argument("--port", default=None,
                   help="the K2 debug shell (default: the /dev/serial/by-id entry of the Open-K2 Logger's CDC "
                        "interface, else " + FALLBACK_PORT + ")")
    r.add_argument("--kinds", default="all", help="all, or a list of log,trace,power[=Hz],run (default all, power at 20 Hz)")
    r.add_argument("--old", action="store_true", help="also send what the rings already hold (history first)")
    r.add_argument("--seconds", type=float, default=None, help="stop after N seconds (default: until Ctrl-C)")
    r.add_argument("--until-run-end", action="store_true", help="stop --settle seconds after a charger-test run ends")
    r.add_argument("--settle", type=float, default=3.0, help="seconds to keep recording after the run end (default 3)")
    r.add_argument("--echo", default="run,log,start", help="kinds printed while recording (run,log,trace,power,beat,boot or all)")
    r.add_argument("--quiet", action="store_true", help="print only the summary")
    v = sub.add_parser("view", help="print / filter the timeline of a session folder")
    v.add_argument("dir")
    v.add_argument("--kind", help="comma list: start,boot,trace,log,power,run,beat,gap")
    v.add_argument("--grep", help="keep only lines matching this regex")
    v.add_argument("--from", dest="t_from", type=float, metavar="MS", help="K2 time in ms")
    v.add_argument("--to", dest="t_to", type=float, metavar="MS")
    v.add_argument("--wall", action="store_true", help="prefix the estimated wall-clock time")
    v.add_argument("--summary", action="store_true", help="the completeness summary only")
    a = ap.parse_args(argv)
    if a.cmd == "view":
        return cmd_view(a)
    if a.port is None:
        a.port = default_port()
    try:
        sess, ok, summary = record(a)
    except OSError as e:
        print(f"cannot use {a.port}: {e}", file=sys.stderr)
        return 2
    print(f"\n{sess.frames} frames, {sess.rows_written} rows in {a.out}/")
    print("\n".join(summary))
    print("COMPLETE: every device counter matches" if ok else "INCOMPLETE: see the lines above and the GAP rows in timeline.txt")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
