#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Unattended hardware smoke test: the ONLY sanctioned way to validate a build on the K2 overnight.

Flash (hands-free auto-DFU), wait for the app, check the git hash, capture 20 s of logs, check the
state sanity, screenshot every page, exercise the remote, write build/smoke/<hash>/{summary.json,report.md}.

SAFETY: read-only plus page navigation. Remote calls go through `remote()`, which only accepts the
allowlist below. Never sent: fc (any trigger/scan), identify, dfu, factory-wipe, fault, reboot, ripple, action,
emarker scan, anything that can change VBUS. Every USB call runs in a subprocess with a timeout, so
a wedged hid call cannot hang the run.

USB LINK: since the USB-off-by-default change (k2/drivers/usbcfg.hpp) a fresh or erased device does not enumerate
at all. Enable HID once by hand (Settings > USB Link > HID > Toggle, then reboot); the setting is kept in
flash (0x080E6000) and survives flashing a new build, which does not touch that sector. A device with HID
off has no remote or log channel, so the smoke run fails at "wait for the app" with exit 2. DFU entry
(MINUS held while plugging in) works regardless, and so does flashing from DFU.

Exit codes: 0 pass, 1 a check failed, 2 flash failed or the device did not come back (recovery hint
printed), 3 preconditions (no .k2 file)."""
import argparse
import json
import re
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

import k2_lock

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable
FLASH_TIMEOUT_S = 300
REMOTE_TIMEOUT_S = 25
LOG_SECONDS = 20
MIN_HEARTBEATS = 3
BOOT_WAIT_S = 45

# What the smoke test may ask the K2 to do. Everything else is refused before any USB traffic.
SAFE_BUTTONS = {"plus", "minus", "back"}          # never "ok": OK starts actions (rescan, energy reset, trigger)
SAFE_COMMANDS = {"ping", "state", "adc-raw", "page", "screenshot", "pdos"}

RECOVERY_HINT = (
    "RECOVERY: the K2 did not come back running the app. The bootloader keeps it in DFU.\n"
    "  1. python3 tools/k2_flasher.py probe   (is it in DFU?)\n"
    "  2. re-flash the last good build: python3 tools/k2_flasher.py flash <good>.k2 --auto-dfu\n"
    "     (or replug USB while holding MINUS, then flash without --auto-dfu)\n"
    "  3. still dead: flash a recovery container (see README.md, Recovery).")


class SmokeError(Exception):
    pass


# ---- pure checks (unit-tested in tests/test_hw_smoke.py) --------------------------------------------

def check_logs(text, min_heartbeats=MIN_HEARTBEATS):
    """Failures found in a captured log: any [FAULT] line, too few heartbeats."""
    errs = [f"fault line: {l.strip()}" for l in text.splitlines() if "[FAULT]" in l]
    beats = len(re.findall(r"\[SYS\] Heartbeat", text))
    if beats < min_heartbeats:
        errs.append(f"heartbeat missing: {beats} seen, expected at least {min_heartbeats}")
    return errs


def vdd_mv(vref_raw):
    """The ADC supply from the VREFINT count: 4095 * 1.2 V / count (include/k2/drivers/adc_calc.hpp)."""
    return round(4095 * 1200 / vref_raw) if vref_raw else 0


def check_state(state, adc):
    errs = []
    if not state.get("pm_online") or state.get("chip") in (None, "none"):
        errs.append(f"power monitor not online (chip={state.get('chip')}, online={state.get('pm_online')})")
    if not adc.get("ok"):
        errs.append("ADC sweep failed")
    v = vdd_mv(adc.get("scan", {}).get("vref", 0))
    if not 3000 <= v <= 3600:
        errs.append(f"VDD {v} mV outside 3000..3600")
    if not state.get("mcu_valid"):
        errs.append("MCU temperature not valid")
    elif not 20 <= state.get("mcu_temp_c", -999) <= 80:
        errs.append(f"MCU temperature {state.get('mcu_temp_c')} C outside 20..80")
    return errs, v


def read_png(data):
    """(width, height, rows of RGB(A) bytes) of an 8-bit RGB/RGBA PNG, any filter, no Pillow."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, hdr = 8, b"", None
    while pos < len(data):
        n, tag = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat += body
        pos += 12 + n
    w, h, depth, ctype = hdr[:4]
    if depth != 8 or ctype not in (2, 6) or hdr[6] != 0:
        raise ValueError(f"unsupported PNG (depth {depth}, type {ctype})")
    bpp = 3 if ctype == 2 else 4
    stride, raw, rows, prev = w * bpp, zlib.decompress(idat), [], bytearray(w * bpp)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, rows, bpp


def colour_count(png_data):
    w, _, rows, bpp = read_png(png_data)
    seen = set()
    for r in rows:
        for x in range(w):
            seen.add(r[x * bpp:x * bpp + 3])
        if len(seen) > 2:
            break
    return len(seen)


def page_names():
    """Page enum names from include/k2/ui/ui_types.hpp, so a page added on any branch is covered."""
    try:
        m = re.search(r"enum class Page\b[^{]*\{([^}]*)\}", (ROOT / "include/k2/ui/ui_types.hpp").read_text())
        names = [n.strip().lower() for n in m.group(1).split(",") if n.strip()]
        return [n for n in names if n != "count"]
    except (OSError, AttributeError):
        return ["home", "detail", "chargertest", "pd", "diag", "lines", "tools", "settings"]


def git_matches(ping_git, head):
    """(ok, dirty) for the git string of PING (`git describe --always --dirty`) against rev-parse --short."""
    dirty = ping_git.endswith("-dirty")
    base = ping_git[:-6] if dirty else ping_git
    return bool(base) and (head.startswith(base) or base.startswith(head)), dirty


# ---- device access (every call bounded) -------------------------------------------------------------

def run(cmd, timeout):
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
                              env=k2_lock.child_env())
    except subprocess.TimeoutExpired as e:
        raise SmokeError(f"timeout after {timeout}s: {' '.join(map(str, cmd[:4]))}") from e


def remote(*args, timeout=REMOTE_TIMEOUT_S):
    """Run one allowlisted k2_remote.py command, return its JSON."""
    cmd = args[0]
    if cmd not in SAFE_COMMANDS:
        raise SmokeError(f"refused: remote command {cmd!r} is not allowed in the smoke test")
    return _remote(args, timeout)


def _remote(args, timeout):
    r = run([PY, "tools/k2_remote.py", "--json", "--timeout", "3", *map(str, args)], timeout)
    if r.returncode:
        raise SmokeError(f"k2_remote {' '.join(map(str, args))} exit {r.returncode}: {(r.stderr or r.stdout).strip()}")
    return json.loads(r.stdout) if r.stdout.strip() else {}


def press(button, long=False):
    if button not in SAFE_BUTTONS:
        raise SmokeError(f"refused: button {button!r} is not allowed in the smoke test")
    return _remote(["press", button] + (["--long"] if long else []), REMOTE_TIMEOUT_S)


def debug_fake_pdos(n):
    if not 0 <= n <= 7:
        raise SmokeError("fake pdo count out of range")
    return _remote(["debug", "pdos", "--fake", n], REMOTE_TIMEOUT_S)


# ---- the run ----------------------------------------------------------------------------------------

class Report:
    def __init__(self, head, outdir):
        self.head, self.outdir, self.checks, self.shots = head, outdir, [], []
        self.started = time.strftime("%Y-%m-%d %H:%M:%S")

    def add(self, name, ok, detail="", warn=False):
        status = "WARN" if ok and warn else "PASS" if ok else "FAIL"
        self.checks.append({"name": name, "status": status, "detail": detail})
        print(f"[{status}] {name}" + (f": {detail}" if detail else ""), flush=True)
        return ok

    @property
    def ok(self):
        return all(c["status"] != "FAIL" for c in self.checks)

    def write(self):
        summary = {"hash": self.head, "started": self.started, "ok": self.ok, "checks": self.checks,
                   "screenshots": self.shots}
        (self.outdir / "summary.json").write_text(json.dumps(summary, indent=2))
        md = [f"# K2 hardware smoke report {self.head}", "", f"Started {self.started}; result **{'PASS' if self.ok else 'FAIL'}**",
              "", "| check | status | detail |", "|---|---|---|"]
        md += [f"| {c['name']} | {c['status']} | {c['detail'].replace('|', '/').replace(chr(10), ' ')} |" for c in self.checks]
        md += ["", "## Screenshots", ""] + [f"![{s['page']}]({s['file']}) {s['page']}: {s['colours']} colours" for s in self.shots]
        (self.outdir / "report.md").write_text("\n".join(md) + "\n")
        return self.outdir / "report.md"


def wait_for_app(deadline_s):
    end, last = time.monotonic() + deadline_s, "no attempt"
    while time.monotonic() < end:
        try:
            return remote("ping")
        except SmokeError as e:
            last = str(e)
        time.sleep(1)
    raise SmokeError(f"app did not answer ping within {deadline_s}s ({last})")


def main(argv=None):
    """Hold the K2 device lock for the whole run (flash, re-enumeration, log capture, screenshots);
    the k2_flasher/k2_remote subprocesses inherit it through K2_LOCK_OWNER_PID."""
    try:
        with k2_lock.hold():
            return _main(argv)
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2


def _main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--firmware", default="build/open_k2.k2")
    ap.add_argument("--no-flash", action="store_true", help="test whatever runs now (still read-only)")
    ap.add_argument("--log-seconds", type=int, default=LOG_SECONDS)
    a = ap.parse_args(argv)

    fw = ROOT / a.firmware
    if not a.no_flash and not fw.exists():
        print(f"{fw} not found: run `make docker-build` (or `make smoke`) first", file=sys.stderr)
        return 3
    head = run(["git", "rev-parse", "--short", "HEAD"], 10).stdout.strip() or "unknown"
    outdir = ROOT / "build/smoke" / head
    outdir.mkdir(parents=True, exist_ok=True)
    rep = Report(head, outdir)
    code = 0
    try:
        if not a.no_flash:
            r = run([PY, "tools/k2_flasher.py", "flash", str(fw), "--auto-dfu"], FLASH_TIMEOUT_S)
            (outdir / "flash.log").write_text(r.stdout + r.stderr)
            if not rep.add("flash --auto-dfu", r.returncode == 0, f"exit {r.returncode}; log flash.log"):
                raise SmokeError("flash failed")
        try:
            ping = wait_for_app(BOOT_WAIT_S)
        except SmokeError as e:
            rep.add("app answers ping after flash", False, str(e))
            print(RECOVERY_HINT, file=sys.stderr)
            code = 2
            raise
        rep.add("app answers ping after flash", True, f"fw {ping.get('version')} up {ping.get('uptime_ms')} ms")
        ok, dirty = git_matches(ping.get("git", ""), head)
        rep.add("git hash matches HEAD", ok, f"device {ping.get('git')!r}, HEAD {head}"
                + ("; built from a dirty tree" if dirty else ""), warn=dirty)

        r = run([PY, "tools/k2_flasher.py", "monitor", "--timeout", str(a.log_seconds)], a.log_seconds + 40)
        (outdir / "log.txt").write_text(r.stdout)
        errs = check_logs(r.stdout, MIN_HEARTBEATS if a.log_seconds >= 10 else 1)
        rep.add(f"{a.log_seconds} s log: no [FAULT], heartbeat present", not errs and r.returncode == 0,
                "; ".join(errs) or f"{len(r.stdout.splitlines())} lines")

        state, adc = remote("state"), remote("adc-raw")
        errs, v = check_state(state, adc)
        rep.add("state sanity (INA online, VDD, MCU temp)", not errs,
                "; ".join(errs) or f"chip {state['chip']}, VDD {v} mV, MCU {state.get('mcu_temp_c')} C")

        for i, name in enumerate(page_names()):
            f = f"page{i}_{name}.png"
            try:
                remote("page", i)
                time.sleep(0.6)
                remote("screenshot", outdir / f)
                n = colour_count((outdir / f).read_bytes())
                rep.shots.append({"page": name, "file": f, "colours": n})
                rep.add(f"screenshot page {i} {name}", n > 2, f"{n}+ colours" if n > 2 else "two-colour: blank render")
            except (SmokeError, ValueError, OSError) as e:
                rep.add(f"screenshot page {i} {name}", False, str(e))

        try:
            names = page_names()
            remote("page", names.index("diag") if "diag" in names else 0)   # Diag: nothing to change there
            press("plus")                                     # short press
            press("minus", long=True)                         # long press: previous page, navigation only
            rep.add("remote short + long press", True, "plus short, minus long")
            debug_fake_pdos(7)
            n = remote("state").get("num_pdos")
            rep.add("debug pdos --fake 7", n == 7, f"num_pdos {n}")
            debug_fake_pdos(0)
            n = remote("state").get("num_pdos")
            rep.add("debug pdos --fake 0 clears", n != 7, f"num_pdos {n}")
            remote("page", 0)
            rep.add("back to home page", remote("state").get("page") == 0)
        except SmokeError as e:
            rep.add("remote exercise", False, str(e))
    except SmokeError as e:
        code = code or 1
        print(f"smoke aborted: {e}", file=sys.stderr)
    finally:
        path = rep.write()
        print(f"\nreport: {path}")
    return code if code else (0 if rep.ok else 1)


if __name__ == "__main__":
    sys.exit(main())
