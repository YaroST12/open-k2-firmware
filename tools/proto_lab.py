#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Protocol lab client for the K2 CDC shell (docs/PROTOCOL_LAB.md).

  proto_lab.py list
  proto_lab.py script accp-dump [--port /dev/ttyACM0]
  proto_lab.py run "accp read 0x80" "scp 9000 3000 --confirm" ...

Every command is sent with --json (when the grammar allows it), the replies are parsed, and one log file per
run is written to build/lab/ (commands, replies, a VBUS/IBUS sample after each command). On exit or error
the client sends `fc reset`, `dp hiz` and `dm hiz`, so the charger is back at 5 V. Needs the K2 with the
shell enabled (Tools -> USB link) and a free port; nothing here flashes the device.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logdb  # noqa: E402  K2LOG msg#id lines -> text (tools/logdb.py)

ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "build" / "lab"
DEFAULT_PORT = "/dev/ttyACM0"


class LabError(Exception):
    pass


class SerialPort:
    """Minimal raw tty (same approach as tools/sniff_stream.py, no pyserial)."""

    def __init__(self, path):
        import termios
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = a[1] = a[3] = 0
        a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
        a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
        termios.tcsetattr(self.fd, termios.TCSANOW, a)

    def write(self, data):
        while data:
            try:
                n = os.write(self.fd, data)
                data = data[n:]
            except BlockingIOError:
                time.sleep(0.005)

    def read(self, timeout):
        import select
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def close(self):
        os.close(self.fd)


class Lab:
    def __init__(self, port, log_path=None, trace=True, clock=time.monotonic):
        self.port = port
        self.trace = trace
        self.clock = clock
        self.buf = b""
        self.log = None
        self.closed = False
        if log_path:
            Path(log_path).parent.mkdir(parents=True, exist_ok=True)
            self.log = open(log_path, "w")

    def _log(self, kind, text):
        if self.log:
            self.log.write("%.3f %s %s\n" % (self.clock(), kind, text))
            self.log.flush()

    def _line(self, deadline):
        while b"\n" not in self.buf:
            left = deadline - self.clock()
            if left <= 0:
                return None
            self.buf += self.port.read(min(left, 0.2))
        line, self.buf = self.buf.split(b"\n", 1)
        return logdb.decode_line(line.decode("utf-8", "replace")).strip()

    def cmd(self, line, timeout=15.0, sample=True):
        """Send one shell line. Returns the parsed reply objects; raises LabError on `err:` or timeout."""
        if "--json" not in line.split():
            line += " --json"
        self.buf = b""
        self._log("TX", line)
        self.port.write(line.encode() + b"\r\n")
        out = []
        deadline = self.clock() + timeout
        while True:
            raw = self._line(deadline)
            if raw is None:
                raise LabError("timeout waiting for the reply to: " + line)
            if not raw:
                continue
            self._log("RX", raw)
            if raw == "ok":
                break
            if raw.startswith("err"):
                raise LabError(raw)
            if raw.startswith("{"):
                try:
                    out.append(json.loads(raw))
                except ValueError:
                    out.append({"raw": raw})
        if sample and self.trace and not line.startswith("vbus"):
            try:
                v = self.cmd("vbus", timeout=3, sample=False)
                if v:
                    self._log("VI", json.dumps(v[0]))
            except LabError:
                pass
        return out

    def try_cmd(self, line, **kw):
        """Like cmd() but an `err:` reply becomes a result object, so a sweep keeps going."""
        try:
            return self.cmd(line, **kw)
        except LabError as e:
            self._log("ERR", str(e))
            return [{"cmd": line.split()[0], "error": str(e)}]

    def close(self):
        if self.closed:
            return
        self.closed = True
        for c in ("fc reset", "dp hiz", "dm hiz"):
            try:
                self.port.write(c.encode() + b"\r\n")
                self._log("TX", c)
            except OSError:
                break
        if self.log:
            self.log.close()
        if hasattr(self.port, "close"):
            self.port.close()

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


# ---- scripts: name -> function(lab, args) -> list of result objects ----

def s_accp_dump(lab, a):
    out = lab.try_cmd("accp ping")
    out += lab.try_cmd("accp dump 0x00 0xFF", timeout=60)
    return out


def s_scp_sweep(lab, a):
    # ACCP registers used here (0x80 status, 0xB8/0xB9 voltage, 0xC8 current).
    out = lab.try_cmd("accp ping")
    out += lab.try_cmd("accp read 0x80")
    out += lab.try_cmd("scp %d %d --confirm" % (a.mv, a.ma))
    t0 = lab.clock()
    while lab.clock() - t0 < 2.0:
        out += lab.try_cmd("vbus --watch 500", timeout=5)
    for reg in ("0xB8", "0xB9", "0xC8"):
        out += lab.try_cmd("accp read " + reg)
    return out


def s_fcp_steps(lab, a):
    out = lab.try_cmd("accp ping")
    for mv in (5000, 9000, 12000, 9000, 5000):
        out += lab.try_cmd("fcp %d --confirm" % mv)
        out += lab.try_cmd("vbus --watch 1500", timeout=6)
    return out


def s_qc3_steps(lab, a):
    out = lab.try_cmd("dpdm 0.6 3.3 --confirm")
    out += lab.try_cmd("vbus")
    for verb in ("up 3", "up 3", "down 6"):
        out += lab.try_cmd("qc3 %s --confirm" % verb, timeout=10)
        out += lab.try_cmd("vbus --watch 500", timeout=4)
    out += lab.try_cmd("dpdm hiz hiz --release")
    return out


def s_qc3_trace(lab, a):
    # Closed-loop QC3 to 9 V with the engine's tagged [QC3] per-batch log (vbus, pending, learned step, n).
    out = lab.try_cmd("qc3 set 9000 --confirm", timeout=15)
    out += lab.try_cmd("vbus --watch 4000", timeout=8)
    out += lab.try_cmd("dpdm hiz hiz --release")
    return out


def s_one(*cmds):
    def f(lab, a):
        out = []
        for c in cmds:
            out += lab.try_cmd(c, timeout=10)
        return out
    return f


SCRIPTS = {
    "accp-dump": (s_accp_dump, "ping, then dump registers 0x00-0xFF"),
    "scp-sweep": (s_scp_sweep, "ping, read 0x80, set V/I, poll VBUS 2 s, read 0xB8/0xB9/0xC8 (--mv --ma)"),
    "fcp-steps": (s_fcp_steps, "FCP 5/9/12/9/5 V with a VBUS read per step"),
    "qc3-trace": (s_qc3_trace, "QC3 closed loop to 9 V; read the [QC3] per-batch lines (vbus, pending, step, n)"),
    "qc3-steps": (s_qc3_steps, "QC3 continuous mode: up 3, up 3, down 6 from the 0.6/3.3 baseline"),
    "qc2": (s_one("qc2 9000 --confirm", "vbus --watch 1000", "fc reset"), "QC2 9 V request"),
    "afc": (s_one("afc 9000 1670 --confirm", "vbus --watch 1000", "fc reset"), "AFC 9 V request"),
    "vooc": (s_one("vooc listen --timeout-ms 1000", "vooc wake --confirm", "vbus --watch 1000", "fc reset"),
             "VOOC listen, then wake"),
    "ufcs": (s_one("ufcs ping", "ufcs request 9000 2000 --confirm", "fc reset"), "UFCS ping and 9 V request"),
    "vivo": (s_one("vivo request 9000 2000 --confirm", "fc reset"), "vivo request"),
    "pd": (s_one("pd caps", "vbus"), "PD source capabilities and VBUS"),
}


def new_log_path(name):
    return LOG_DIR / ("%s-%s.log" % (name, time.strftime("%Y%m%d-%H%M%S")))


def main(argv=None, port=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--no-trace", action="store_true", help="no VBUS sample after each command")
    sub = ap.add_subparsers(dest="mode", required=True)
    sub.add_parser("list")
    sp = sub.add_parser("script")
    sp.add_argument("name", choices=sorted(SCRIPTS))
    sp.add_argument("--mv", type=int, default=9000)
    sp.add_argument("--ma", type=int, default=2250)
    rp = sub.add_parser("run")
    rp.add_argument("commands", nargs="+", help='shell lines, e.g. "accp read 0x80"')
    a = ap.parse_args(argv)

    if a.mode == "list":
        for n in sorted(SCRIPTS):
            print("%-10s %s" % (n, SCRIPTS[n][1]))
        return 0

    name = a.name if a.mode == "script" else "run"
    path = new_log_path(name)
    try:
        pt = port or SerialPort(a.port)
    except OSError as e:
        print("cannot open %s: %s" % (a.port, e), file=sys.stderr)
        return 2
    rc = 0
    with Lab(pt, path, trace=not a.no_trace) as lab:
        try:
            if a.mode == "script":
                res = SCRIPTS[a.name][0](lab, a)
            else:
                res = []
                for c in a.commands:
                    res += lab.cmd(c, timeout=60)
            for r in res:
                print(json.dumps(r))
        except LabError as e:
            print("error: %s" % e, file=sys.stderr)
            rc = 1
        except KeyboardInterrupt:
            rc = 130
    print("log: %s" % path, file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
