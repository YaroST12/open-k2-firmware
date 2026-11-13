#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Start a full charger-test run from the UI and wait for it: preflight (VBUS >= 4.5 V or no sink inline: START presents Rd; PD caps rescanned),
Algorithms -> restore all selected -> Next (Spoof cable) -> Next (Review, START on top) -> OK (opens the confirmation window,
nothing is touched) -> OK (confirms: Ra guard, identity, then Rd as the last stage) -> the "waiting" stage (the K2 stays
responsive) -> the run starts by itself, then the run id must go up. The presses follow the stage, they are never
blind: OK in "waiting" would cancel the START. When the Ra guard refuses the chosen identity ("refuse": the plug
carries a cable, not a charger) the refusal is cancelled and the exit code is 6. When the run id stays 0 the START
preflight refused: its reason (the K2's "[TEST] preflight refused" log line, read from the USB log on the same HID
link) is printed with the run id, exit code 5. A summary is never printed for a run
that did not start. Then polls `charger-test status`
until the run ends (done, stopped, unsafe...) and prints `charger-test summary`. Run `k2_remote.py action pd-rescan` first:
a run with 0 PDOs is invalid.

    python3 tools/ct_start.py [--wait 300] [--units qc2,qc3,pd] [--cable off|builtin|builtin:N|saved:N]

--units sends the unit list (names and order) with `charger-units` before START: only those test units run, in that
order, each one detach -> attach -> rows -> detach. Without it the full default order runs: vendor modes (FCP, SCP, AFC, VOOC...), PD, then QC/UFCS.
--cable picks the Spoof cable step's identity (`charger-cable`): off (the default: no spoof, no Ra), builtin
(the built-in EPR 50 V / 5 A cable), builtin:<id|name> (1 epr-50v, 2 cuktech-6a, 4 oneplus; 3 is the retired oneplus-mag, read as 4: the list's
captured-cable rows) or saved:<library id>. Any identity arms that spoof and runs in Ra mode (PC13 high,
Rd on CC2 alone) until the run ends. The device remembers the last choice, so it is sent before every START.
"""
import argparse
import sys
import time
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import charger_summary
import k2_lock
import k2_remote as kr
import logdb
from ui_walk import NEXT_ROW, Walk

MIN_VBUS_MV = 4500
START_WAIT_S = 45.0     # the K2's own wait is <= ~21 s (VBUS 5-10 s, caps 2 s, one Rd re-present), plus the presses
VBUS_WAIT_S = 5.0       # the wait for VBUS to come up on a dark port before START presents Rd: a sleeping source
                        # answers 1.1-1.7 s after a steady Rd (pattern slow-rp-wake) - the idle Wake sleeping port
                        # hold or an already-awake port - so the wait covers that plus margin
# The standby-signature window of an open CC reading, the same bounds as pd::kStandbyOpenMv / kStandbyOpenHiMv
# (include/k2/pd/pd_rdseek.hpp): at/above the top an open Rp / VCONN reads the 3.3 V rail, below the bottom the
# CC2 bias net (1.69 V) or an Ra.
CC_STANDBY_LO_MV, CC_STANDBY_HI_MV = 2800, 3150
MAX_PRESSES = 3         # open the confirmation, confirm; one more is slack for a press lost on the way
STARTED, REFUSED, FAILED = "started", "refused", "failed"
# The same text as raguard::kRefusalText in include/k2/pd/ra_guard.hpp (the dialog's wrapped lines say it too).
REFUSAL_TEXT = ("spoof needs the charger plugged into the K2 directly (top plug); "
                "through a cable the cable itself answers")



# Every state in which the runner is no longer busy; "done" is the only success.
TERMINAL = ("done", "stopped", "unsafe", "aborted", "failed")


def preflight(link, sleep=time.sleep):
    """None when a run may start, else the refusal text. A PD charger whose caps were not read is rescanned once."""
    st = kr.cmd_state(link, None)
    if st["vbus_mv"] < MIN_VBUS_MV and not st.get("inline", False):
        # A charger alone on the bottom port gets no VBUS without Rd, and plain boot presents none. A sleeping
        # source (pattern slow-rp-wake) answers 1.1-1.7 s after a steady Rd, so wait for VBUS to come up on its
        # own first (the idle Wake sleeping port hold may already have the Rd out); only then does START present
        # Rd and wait again.
        waited = 0.0
        while st["vbus_mv"] < MIN_VBUS_MV and waited < VBUS_WAIT_S:
            sleep(0.5)
            waited += 0.5
            st = kr.cmd_state(link, None)
        if st["vbus_mv"] < MIN_VBUS_MV:
            # The give-up line names why: a standby-signature open CC line with VBUS dark reads as a sleeping source.
            cc = max(st.get("cc1_mv", -1), st.get("cc2_mv", -1))
            if CC_STANDBY_LO_MV <= cc < CC_STANDBY_HI_MV:
                print(f"note: VBUS is {st['vbus_mv'] / 1000:.2f} V after {waited:.1f} s and CC reads {cc / 1000:.2f} V open: "
                      f"the source looks asleep; START will present Rd and wait for VBUS", file=sys.stderr)
            else:
                print(f"note: VBUS is {st['vbus_mv'] / 1000:.2f} V: START will present Rd and wait for VBUS", file=sys.stderr)
            return None
        # VBUS came up while waiting: the normal checks below run on the fresh reading.
    if st["vbus_mv"] < MIN_VBUS_MV:
        return f"VBUS is {st['vbus_mv'] / 1000:.2f} V (< {MIN_VBUS_MV / 1000:.1f} V): no charger attached or it is not powering"
    if not st["pd_valid"]:
        kr.cmd_action(link, Namespace(name="pd-rescan"))
        sleep(3)
        st = kr.cmd_state(link, None)
        # A USB-A port with nothing drawing cuts VBUS every ~6.5 s and brings it back by itself (pattern
        # usb-a-no-load-auto-off): one reading inside a cut is not a lost source. Wait for VBUS like the START does.
        waited = 0.0
        while st["vbus_mv"] < MIN_VBUS_MV and waited < VBUS_WAIT_S:
            sleep(0.5)
            waited += 0.5
            st = kr.cmd_state(link, None)
        if st["vbus_mv"] < MIN_VBUS_MV:
            return f"VBUS dropped to {st['vbus_mv'] / 1000:.2f} V after the PD rescan"
        if not st["pd_valid"]:
            print("note: no PD source capabilities after a rescan: legacy-only run", file=sys.stderr)
    return None


def run_id_of(link, tries=8, sleep=time.sleep):
    """The current run id, or None when the K2 stays silent (it is busy in START's Rd wait or a handshake: ask again)."""
    for _ in range(tries):
        try:
            return kr.cmd_charger_test(link, Namespace(test_cmd="status"))["run_id"]
        except kr.RemoteTimeout:
            sleep(1)
    return None


def press_ok(link):
    kr.cmd_press(link, Namespace(button="ok", ms=0, long=False))


def press_back(link):
    kr.cmd_press(link, Namespace(button="back", ms=0, long=False))


def stage_of(link):
    """The charger-test stage name, or None when the K2 does not answer (a handshake keeps it busy for a moment)."""
    try:
        return kr.cmd_charger_test(link, Namespace(test_cmd="status"))["stage"]
    except kr.RemoteTimeout:
        return None


def drain_log(link, frames, seconds=1.0, clock=time.monotonic):
    """Read the K2's USB log text into `frames` for `seconds`: the log lines ride the same HID IN endpoint as the
    responses (ASCII frames; a response's first byte has the high bit set), so every read collects them."""
    deadline = clock() + seconds
    while clock() < deadline:
        raw = link.transport.read(200)
        if raw:
            frames.append(raw)


def last_preflight_refusal(frames):
    """The last START-refusal log line among the log frames (decoded from their msg# ids), or None.

    Every refusal the K2 logs while the START is refused counts, not only the preflight's: the
    "[TEST] preflight refused: ..." line (with the holding tool named on a busy refusal), the
    "[TEST] cable arm refused: ..." line (the identity arming refused, footer-only before 2026-11-10),
    "[TEST] START: Rd refused ..." - a run id that stays 0 is always printed with one of them (B3)."""
    text = b"".join(f for f in frames if f and f[0] < 0x80).replace(b"\x00", b"").decode("ascii", "replace")
    refused = [l for l in (logdb.decode_line(l) for l in text.splitlines())
               if l.startswith("[TEST] ") and "refused" in l]
    return refused[-1].rstrip("\n") if refused else None


def start_failure_text(before, frames):
    """The stderr line for a START that left the run id unchanged: with the preflight's refusal reason from the log
    when the log carries one, the plain message otherwise."""
    reason = last_preflight_refusal(frames)
    if reason:
        return f"START did not start a run (run id still {before}): {reason}"
    return f"START did not start a run (run id still {before})"


def start_run(link, before, press=press_ok, run_id=None, stage=None, back=press_back, sleep=time.sleep,
              wait_s=START_WAIT_S):
    """Press OK on Review START and follow the stage until the run id differs from `before`.

    STARTED, REFUSED (the Ra guard: the refusal is cancelled with BACK, the K2 is left on Review, nothing armed) or FAILED.

    START is two OKs on Review. The first opens the confirmation window (the stage stays "review", nothing is touched);
    the second runs the Ra guard ("refuse" when the plug carries a cable, not a charger), arms the identity and presents
    Rd as the last stage. The K2 then sits in "waiting" while VBUS and the source's capabilities arrive (5-21 s, the main
    loop keeps running) and the run starts by itself: OK there would cancel the wait, so nothing is pressed once the
    stage has been "waiting". Review showing up again after a "waiting" means the wait ended in a refusal (footer
    message): FAILED, never a retry. A stage poll that gets no answer is skipped."""
    run_id = run_id or (lambda: run_id_of(link, tries=2, sleep=sleep))
    stage = stage or (lambda: stage_of(link))
    def safe_press():
        try:
            press(link)
        except kr.RemoteTimeout:
            pass    # the K2 answers a press at once; a lost one is repeated by the loop below
    safe_press()
    presses, waited, was_waiting = 1, 0.0, False
    while waited < wait_s:
        sleep(0.5)
        waited += 0.5
        rid = run_id()
        if rid is not None and rid != before:
            return STARTED
        st = stage()
        if st == "refuse":
            try:
                back(link)
            except kr.RemoteTimeout:
                pass
            return REFUSED
        if st == "waiting":
            was_waiting = True
        elif st == "review":
            if was_waiting:
                return FAILED   # the wait ended without a run: the K2 refused on its final check
            if presses < MAX_PRESSES:
                safe_press()
                presses += 1
    return FAILED


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--wait", type=float, default=300)
    ap.add_argument("--path")
    ap.add_argument("--sweep", action="store_true",
                    help="also run the PPS deep sweep (opt-in: every 100 mV across each PPS APDO, 1 A; never in a preset)")
    ap.add_argument("--avs-sweep", action="store_true",
                    help="also run the AVS deep sweep (opt-in: every 100 mV across each AVS APDO, 1 A; never in a preset)")
    ap.add_argument("--svooc-cap", choices=["on", "usb-a"],
                    help="also run the SuperVOOC ceiling stage when a SuperVOOC charger is detected (opt-in; reads the 0x86 "
                         "ceiling from the protocol, never loads the charger). usb-a = USB-A charger, top port empty")
    ap.add_argument("--units", default="",
                    help="run only these test units, in this order, e.g. qc2,qc3 or pd,qc2 (names: " + ", ".join(kr.TEST_UNITS) +
                         "); default = the full order: vendor modes, PD, QC/UFCS")
    ap.add_argument("--cable", default="off",
                    help="cable identity of the run: off (default), builtin, builtin:<1|2|4|epr-50v|cuktech-6a|oneplus> "
                         "or saved:<library id>")
    a = ap.parse_args(argv)
    try:
        units = kr.parse_units(a.units)
    except kr.FrameError as e:
        print(f"bad --units: {e}", file=sys.stderr)
        return 4
    try:
        cable = kr.parse_cable(a.cable)
    except kr.FrameError as e:
        print(f"bad --cable: {e}", file=sys.stderr)
        return 4
    with k2_lock.hold():
        link = kr.RemoteLink(kr.LinuxHidTransport(a.path), 1.0)
        link.call(kr.CMD_SET_PAGE, bytes([2]))
        w = Walk(link)
        for _ in range(3):      # leave the results screen of a finished run
            if w.where()[0] != "results":
                break
            kr.cmd_press(link, Namespace(button="back", ms=0, long=False))
            time.sleep(0.8)
        why = preflight(link)
        if why:
            print(f"refusing to start: {why}", file=sys.stderr)
            return 4
        # The wizard advances on a pick: "All tests" -> Cable, the identity in force -> Review (no Next row).
        ok = (w.to_algorithms() and w.pick_all() and w.press("ok", ("review", NEXT_ROW)))
        if not ok:
            print("\n".join(w.log))
            print("could not reach Review START", file=sys.stderr)
            return 1
        if a.sweep:
            kr.charger_sweep(link, True)
            print("deep sweep: on (every 100 mV per PPS APDO, 1 A)", file=sys.stderr)
        if a.avs_sweep:
            kr.charger_avs_sweep(link, True)
            print("AVS deep sweep: on (every 100 mV per AVS APDO, 1 A)", file=sys.stderr)
        if a.svooc_cap:
            kr.charger_svooc_cap(link, a.svooc_cap)
            print(f"SuperVOOC ceiling stage: {a.svooc_cap} (protocol read, no load)", file=sys.stderr)
        if units:
            try:    # the list serves the next START only; the firmware refuses it while a run is active
                kr.charger_units(link, units)
            except kr.RemoteError as e:
                print(f"could not set the unit list: {e}", file=sys.stderr)
                return 1
            print("units: " + ",".join(kr.TEST_UNITS[u] for u in units), file=sys.stderr)
        try:    # the device remembers the last choice and the press loop below takes it: set it every time
            kr.charger_cable(link, cable)
        except kr.RemoteError as e:
            print(f"could not set the cable identity: {e}", file=sys.stderr)
            return 1
        print("cable identity: " + a.cable, file=sys.stderr)
        before = run_id_of(link)
        if before is None:
            print("the K2 does not answer charger-test status", file=sys.stderr)
            return 1
        log_frames = []
        link.tap = log_frames.append   # the log text frames read during the polls (the preflight refusal among them)
        try:
            started = start_run(link, before)
        finally:
            link.tap = None
        if started == REFUSED:
            print(f"refusing to start: {REFUSAL_TEXT} (use --cable off, or put the charger on the top plug)", file=sys.stderr)
            return 6
        if started != STARTED:
            drain_log(link, log_frames)   # the refusal line may still be queued on the K2
            print(start_failure_text(before, log_frames), file=sys.stderr)
            return 5
    # START is confirmed: the run proceeds by itself, so the device lock is released here instead of being
    # held for the whole run. Each poll below takes it again, for that one exchange only, so other HID
    # tools (k2_remote state, pd_trace dump, ...) can use the K2 between the polls.
    k2_lock.release()   # balance the transport's own acquire (LinuxHidTransport.__init__): lock depth 1 -> 0,
                        # so the flock is really dropped here; the HID fd stays open and usable
    deadline = time.monotonic() + a.wait
    while time.monotonic() < deadline:
        with k2_lock.hold():
            try:
                st = kr.cmd_charger_test(link, Namespace(test_cmd="status"))
            except kr.RemoteTimeout:    # the K2 is busy bit-banging a handshake: ask again
                st = None
        if st is not None and st["state"] in TERMINAL:
            break
        time.sleep(1 if st is None else 2)
    else:
        print("timeout waiting for done", file=sys.stderr)
        return 2
    with k2_lock.hold():
        r = kr.cmd_charger_test(link, Namespace(test_cmd="summary", out=None))
        r.pop("_summary", None)
        r["cable_identity"] = kr.charger_cable(link)
    print(charger_summary.summarize(r))
    if st["state"] != "done":
        print(f"run ended in state {st['state']}", file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
