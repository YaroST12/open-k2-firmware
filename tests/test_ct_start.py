# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""ct_start preflight, no device."""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import ct_start
import logdb


class Preflight(unittest.TestCase):
    def run_pf(self, states):
        it = iter(states)
        with patch.object(ct_start.kr, "cmd_state", lambda l, a: next(it)), \
             patch.object(ct_start.kr, "cmd_action", lambda l, a: None):
            return ct_start.preflight(None, sleep=lambda s: None)

    def test_low_vbus_inline_refused(self):
        self.assertIn("4.30 V", self.run_pf([{"vbus_mv": 4300, "pd_valid": True, "inline": True}]))

    def test_low_vbus_without_sink_lets_start_present_rd(self):
        # dark the whole wait: START still presents Rd (the note says so); the wait ran VBUS_WAIT_S worth of polls
        states = [{"vbus_mv": 0, "pd_valid": False, "inline": False}] * 12
        self.assertIsNone(self.run_pf(states))

    def test_good_source_passes(self):
        self.assertIsNone(self.run_pf([{"vbus_mv": 5000, "pd_valid": True}]))

    def test_rescan_then_vbus_gone_refused(self):
        r = self.run_pf([{"vbus_mv": 5000, "pd_valid": False}, {"vbus_mv": 0, "pd_valid": False}])
        self.assertIn("dropped", r)

    def test_legacy_only_proceeds(self):
        self.assertIsNone(self.run_pf([{"vbus_mv": 5000, "pd_valid": False}, {"vbus_mv": 5000, "pd_valid": False}]))


class VbusWait(unittest.TestCase):
    """The VBUS wait before START presents Rd: a sleeping source (pattern slow-rp-wake) answers 1.1-1.7 s after
    a steady Rd - the idle Wake sleeping port hold or an already-awake port brings VBUS up by itself. The wait
    must cover that (+ margin), and when it gives up with VBUS still dark its note names why (a standby-signature
    open CC line = the source looks asleep)."""
    def run_pf(self, states):
        it = iter(states)
        calls = []
        def state(link, a):
            calls.append(1)
            return next(it)
        err = io.StringIO()
        with patch.object(ct_start.kr, "cmd_state", state), \
             patch.object(ct_start.kr, "cmd_action", lambda l, a: None), \
             contextlib.redirect_stderr(err):
            r = ct_start.preflight(None, sleep=lambda s: None)
        return r, len(calls), err.getvalue()

    @staticmethod
    def dark(cc1=3000, cc2=40, vbus=0):
        return {"vbus_mv": vbus, "pd_valid": False, "inline": False, "cc1_mv": cc1, "cc2_mv": cc2}

    def test_a_source_that_answers_after_1_6s_is_waited_for(self):
        up = {"vbus_mv": 5000, "pd_valid": True, "inline": False, "cc1_mv": 200, "cc2_mv": 40}
        r, calls, _ = self.run_pf([self.dark(), self.dark(), self.dark(), up])   # up after ~1.5-2.0 s of 0.5 s polls
        self.assertIsNone(r)                      # the START proceeds on the fresh, up reading
        self.assertGreaterEqual(calls, 4)         # the wait covered the 1.1-1.7 s wake, not one poll

    def test_the_wait_gives_up_naming_the_asleep_source(self):
        r, calls, err = self.run_pf([self.dark(cc1=3000) for _ in range(12)])
        self.assertIsNone(r)                      # START will still present Rd and wait again
        self.assertGreaterEqual(calls, 10)        # the whole wait ran
        self.assertIn("asleep", err)              # the give-up line names why
        self.assertIn("3.00 V", err)              # the open CC reading
        self.assertIn("START", err)

    def test_a_waiting_charger_at_3_3v_is_not_called_asleep(self):
        _, _, err = self.run_pf([self.dark(cc1=3300) for _ in range(12)])
        self.assertNotIn("asleep", err)           # an open Rp reads the 3.3 V rail: outside the window

    def test_the_cc2_bias_net_is_not_called_asleep(self):
        _, _, err = self.run_pf([self.dark(cc1=40, cc2=1690) for _ in range(12)])
        self.assertNotIn("asleep", err)           # the CC2 bias net (1.69 V) is below the window


class StartRunWake(unittest.TestCase):
    def test_a_start_against_a_source_that_answers_after_1_6s_runs(self):
        # the K2's own Rd wait covers the wake (4 polls of 0.5 s = 2.0 s > 1.7 s): the run starts, no extra press
        k2 = FakeK2(wait_polls=4)
        slept = []
        r = ct_start.start_run(None, 5, press=k2.press, run_id=k2.run_id, stage=k2.stage_of, back=k2.back,
                               sleep=slept.append)
        self.assertEqual(r, ct_start.STARTED)
        self.assertEqual(k2.presses, 2)
        self.assertEqual(k2.presses_in_wait, 0)


class FakeK2:
    """The START screens as the firmware has them: Review -OK-> Review with the confirm window -OK-> (refuse | waiting |
    run); the run starts by itself when the wait ends (or, with `wait_fails`, the K2 is back on Review with a message).
    OK in `waiting` cancels the START (back to review, nothing started), as on the device. One stage poll = 0.5 s."""
    def __init__(self, wait_polls=0, refuse=False, silent=False, stuck_review=False, wait_fails=False):
        self.stage, self.run, self.wait_left = "review", 5, 0
        self.wait_polls, self.refuse, self.silent, self.stuck_review = wait_polls, refuse, silent, stuck_review
        self.wait_fails = wait_fails
        self.presses, self.presses_in_wait, self.backs, self.cancelled, self.confirm_open = 0, 0, 0, False, False

    def press(self, link):
        self.presses += 1
        if self.stage == "review" and not self.stuck_review:
            if not self.confirm_open:
                self.confirm_open = True        # the first OK: the window opens, the stage stays Review
            elif self.refuse:
                self.stage, self.confirm_open = "refuse", False
            elif self.wait_polls:
                self.stage, self.wait_left, self.confirm_open = "waiting", self.wait_polls, False
            else:
                self.stage, self.run, self.confirm_open = "running", self.run + 1, False
        elif self.stage == "waiting":
            self.presses_in_wait += 1
            self.stage, self.cancelled = "review", True

    def back(self, link):
        self.backs += 1
        self.stage = "review"

    def stage_of(self):
        if self.silent:
            return None
        if self.stage == "waiting":
            self.wait_left -= 1
            if self.wait_left <= 0:
                if self.wait_fails:
                    self.stage = "review"
                else:
                    self.stage, self.run = "running", self.run + 1
        return self.stage

    def run_id(self):
        return None if self.silent else self.run


class StartRun(unittest.TestCase):
    def go(self, k2, **kw):
        slept = []
        r = ct_start.start_run(None, 5, press=k2.press, run_id=k2.run_id, stage=k2.stage_of, back=k2.back,
                               sleep=slept.append, **kw)
        return r, sum(slept)

    def test_two_presses_without_a_wait(self):
        k2 = FakeK2()
        r, _ = self.go(k2)
        self.assertEqual(r, ct_start.STARTED)
        self.assertEqual(k2.presses, 2)    # open the confirmation, confirm: the run starts by itself

    def test_waits_for_the_charger_without_pressing(self):
        k2 = FakeK2(wait_polls=40)         # 20 s of "waiting"
        r, slept = self.go(k2)
        self.assertEqual(r, ct_start.STARTED)
        self.assertEqual(k2.presses_in_wait, 0)    # OK there would cancel the START
        self.assertFalse(k2.cancelled)
        self.assertEqual(k2.presses, 2)            # the wait is the last stage: no third press
        self.assertGreaterEqual(slept, 20)

    def test_a_start_cancelled_in_the_wait_is_never_made_by_us(self):
        # a press that lands as the wait ends must not be the one that cancels it: only review gets presses
        k2 = FakeK2(wait_polls=3)
        self.go(k2)
        self.assertEqual(k2.presses_in_wait, 0)

    def test_a_wait_that_ends_on_review_is_a_failure_not_a_retry(self):
        k2 = FakeK2(wait_polls=3, wait_fails=True)
        r, _ = self.go(k2)
        self.assertEqual(r, ct_start.FAILED)
        self.assertEqual(k2.presses, 2)    # the refusal is the K2's answer: no new confirmation, no second Rd

    def test_the_guard_refusal_is_reported_and_cancelled(self):
        k2 = FakeK2(refuse=True)
        r, _ = self.go(k2)
        self.assertEqual(r, ct_start.REFUSED)
        self.assertEqual(k2.backs, 1)
        self.assertEqual(k2.stage, "review")
        self.assertEqual(k2.presses, 2)    # confirmation opened and confirmed, nothing after the refusal

    def test_refusal_text_matches_the_firmware(self):
        header = (Path(__file__).resolve().parent.parent / "include" / "k2" / "pd" / "ra_guard.hpp").read_text()
        self.assertIn(ct_start.REFUSAL_TEXT, header)

    def test_gives_up_with_a_bounded_number_of_presses(self):
        k2 = FakeK2(stuck_review=True)     # a Review that never moves on
        r, slept = self.go(k2)
        self.assertEqual(r, ct_start.FAILED)
        self.assertEqual(k2.presses, ct_start.MAX_PRESSES)
        self.assertLessEqual(slept, ct_start.START_WAIT_S + 0.5)

    def test_silent_k2_is_not_a_start(self):
        k2 = FakeK2(silent=True)
        r, _ = self.go(k2)
        self.assertEqual(r, ct_start.FAILED)
        self.assertEqual(k2.presses, 1)

    def test_press_rpc_timing_out_is_tolerated(self):
        k2 = FakeK2()
        real = k2.press
        calls = []
        def press(link):
            calls.append(1)
            real(link)
            if len(calls) == 1:
                raise ct_start.kr.RemoteTimeout("no response")
        r, _ = ct_start.start_run(None, 5, press=press, run_id=k2.run_id, stage=k2.stage_of, back=k2.back,
                                  sleep=lambda s: None), 0
        self.assertEqual(r, ct_start.STARTED)


class PreflightRefusalPrint(unittest.TestCase):
    """The START preflight's refusal must reach ct_start's output: the K2 logs it as one [TEST] line and the log text
    rides the same HID IN endpoint as the responses (tapped while the polls run), so a run id that stays 0 is printed
    together with the reason from that line."""

    def test_the_refusal_reason_is_printed_with_the_run_id(self):
        frames = [b"[TEST] preflight refused: VBUS not safe 5V (7700 mV)\n" + b"\x00" * 20]
        text = ct_start.start_failure_text(5, frames)
        self.assertIn("run id still 5", text)
        self.assertIn("VBUS not safe 5V (7700 mV)", text)

    def test_packed_log_frames_decode_to_the_reason(self):
        fmt = "[TEST] preflight refused: %s (%d mV)\n"
        ids = [k for k, v in logdb.load().items() if v == fmt]
        if not ids:
            self.skipTest("the preflight-refusal format is not in tools/logdb.json yet")
        # the wire form of that line (msg#<id> args; a %s arg is %XX-escaped, a %d arg decimal), as the K2 sends it
        frames = [(f"msg#{ids[0]} VBUS%20not%20safe%205V 7700\n").encode()]
        self.assertIn("VBUS not safe 5V (7700 mV)", ct_start.start_failure_text(5, frames))

    def test_response_frames_are_ignored(self):
        frames = [b"\xA6\x4B\x01\x01\x00" + b"\x00" * 59, b"[TEST] preflight refused: no source (0 mV)\n"]
        self.assertIn("no source (0 mV)", ct_start.start_failure_text(7, frames))

    def test_without_a_refusal_line_the_message_stays(self):
        self.assertEqual(ct_start.start_failure_text(5, [b"[TEST] start selected=1 units=2 -> none\n"]),
                         "START did not start a run (run id still 5)")
        self.assertEqual(ct_start.start_failure_text(5, []), "START did not start a run (run id still 5)")

    def test_the_wrong_voltage_phrase_line_is_printed_verbatim(self):
        # the WrongVoltage preflight refusal (a live PDO 1 contract held outside vSafe5V, pattern shared-bus-split)
        # reads "VBUS 8.5 V on a 5 V contract (shared bus)" in the log; ct_start prints that line as the reason
        frames = [b"[TEST] preflight refused: VBUS 8.5 V on a 5 V contract (shared bus)\n"]
        text = ct_start.start_failure_text(0, frames)
        self.assertIn("run id still 0", text)
        self.assertIn("VBUS 8.5 V on a 5 V contract (shared bus)", text)

    def test_a_busy_preflight_line_names_the_holding_tool(self):
        # a "tool busy" refusal names the tool that holds the line (the busy_reason footer text), not a bare "busy"
        frames = [b"[TEST] preflight refused: tool busy: PD hold: release it (5100 mV)\n"]
        text = ct_start.start_failure_text(5, frames)
        self.assertIn("run id still 5", text)
        self.assertIn("tool busy: PD hold: release it (5100 mV)", text)

    def test_a_cable_arm_refusal_line_is_printed(self):
        # the cable/Ra arming refusal (a hold or a live contract while the identity arms, e.g. a re-read's leftover)
        # is logged as its own [TEST] line; a START that leaves the run id unchanged must print it, never stay
        # silent (hardware 2026-11-10, 72c2e28: "START did not start a run" with NO reason printed)
        frames = [b"[TEST] cable arm refused: Ra refused: contract live\n"]
        text = ct_start.start_failure_text(5, frames)
        self.assertIn("run id still 5", text)
        self.assertIn("cable arm refused: Ra refused: contract live", text)

    def test_packed_cable_arm_refusal_decodes_to_the_reason(self):
        fmt = "[TEST] cable arm refused: %s\n"
        ids = [k for k, v in logdb.load().items() if v == fmt]
        if not ids:
            self.skipTest("the cable-arm-refusal format is not in tools/logdb.json yet")
        # the wire form of that line (msg#<id> args; a %s arg is %XX-escaped), as the K2 sends it
        frames = [(f"msg#{ids[0]} Ra%20refused:%20contract%20live\n").encode()]
        self.assertIn("cable arm refused: Ra refused: contract live", ct_start.start_failure_text(5, frames))

    def test_a_start_rd_refusal_line_is_printed(self):
        frames = [b"[TEST] START: Rd refused (sink inline)\n"]
        text = ct_start.start_failure_text(3, frames)
        self.assertIn("START: Rd refused (sink inline)", text)


class RunId(unittest.TestCase):
    def test_retries_while_the_k2_is_busy_in_start(self):
        calls = iter([ct_start.kr.RemoteTimeout("no response"), ct_start.kr.RemoteTimeout("no response"), {"run_id": 7}])
        def status(link, args):
            r = next(calls)
            if isinstance(r, Exception):
                raise r
            return r
        with patch.object(ct_start.kr, "cmd_charger_test", status):
            self.assertEqual(ct_start.run_id_of(None, sleep=lambda s: None), 7)

    def test_gives_up_with_none_when_the_k2_never_answers(self):
        def status(link, args):
            raise ct_start.kr.RemoteTimeout("no response")
        with patch.object(ct_start.kr, "cmd_charger_test", status):
            self.assertIsNone(ct_start.run_id_of(None, tries=3, sleep=lambda s: None))


class UnitsOption(unittest.TestCase):
    def test_bad_unit_name_is_refused_before_any_device_access(self):
        self.assertEqual(ct_start.main(["--units", "qc2,bogus"]), 4)

    def test_duplicate_unit_is_refused(self):
        self.assertEqual(ct_start.main(["--units", "qc2,qc2"]), 4)


class CableOption(unittest.TestCase):
    def test_bad_cable_is_refused_before_any_device_access(self):
        self.assertEqual(ct_start.main(["--cable", "bogus"]), 4)
        self.assertEqual(ct_start.main(["--cable", "saved:0"]), 4)
        self.assertEqual(ct_start.main(["--cable", "saved"]), 4)
        self.assertEqual(ct_start.main(["--cable", "builtin:0"]), 4)     # the generic cable is remote-only, not a dialog row
        self.assertEqual(ct_start.main(["--cable", "builtin:5"]), 4)
        self.assertEqual(ct_start.main(["--cable", "builtin:nope"]), 4)

    def test_default_is_off_and_names_parse(self):
        self.assertEqual(ct_start.kr.parse_cable("off"), (0, 0))
        self.assertEqual(ct_start.kr.parse_cable(""), (0, 0))
        self.assertEqual(ct_start.kr.parse_cable("builtin"), (1, 1))
        self.assertEqual(ct_start.kr.parse_cable("builtin:3"), (1, 4))                # the retired id 3 is sent as 4
        self.assertEqual(ct_start.kr.parse_cable("builtin:oneplus-mag"), (1, 4))
        self.assertEqual(ct_start.kr.parse_cable("Builtin:CUKTECH-6A"), (1, 2))
        self.assertEqual(ct_start.kr.parse_cable("builtin:oneplus"), (1, 4))
        self.assertEqual(ct_start.kr.parse_cable("saved:3"), (2, 3))
        self.assertEqual(ct_start.kr.parse_cable("Clone:65535"), (2, 65535))


class LockReleaseAfterStart(unittest.TestCase):
    """ct_start must not hold the device lock for the whole run: once START is confirmed the lock is released
    and only re-taken around each poll, so other HID tools can use the K2 between the polls."""

    def test_lock_is_released_once_start_is_confirmed(self):
        events = []          # ("A"|"R"|"start"|"poll"|"call", lock depth after the event)
        depth = 0

        def acquire(wait_s=None, report=None):
            nonlocal depth
            depth += 1
            events.append(("A", depth))

        def release():
            nonlocal depth
            depth -= 1
            events.append(("R", depth))

        class FakeLink:
            def __init__(self, *a):
                self.tap = None

            def call(self, cmd, payload=b""):
                events.append(("call", depth))
                return ct_start.kr.Response(cmd, 1, 0, b"")

        class FakeWalk:
            def __init__(self, link):
                self.log = []

            def where(self):
                return ("home", 0)

            def to_algorithms(self):
                return True

            def pick_all(self):
                return True

            def press(self, *a):
                return True

        def fake_ct(link, a):
            events.append(("poll", depth))
            if a.test_cmd == "status":
                return {"state": "done", "run_id": 6}
            return {"state": "done", "run_id": 6, "steps": []}

        with patch.object(ct_start.k2_lock, "acquire", acquire), \
             patch.object(ct_start.k2_lock, "release", release), \
             patch.object(ct_start.kr, "RemoteLink", FakeLink), \
             patch.object(ct_start.kr, "LinuxHidTransport", lambda path: object()), \
             patch.object(ct_start, "Walk", FakeWalk), \
             patch.object(ct_start, "preflight", lambda link: None), \
             patch.object(ct_start, "run_id_of", lambda link: 5), \
             patch.object(ct_start, "start_run",
                          lambda *a, **k: events.append(("start", depth)) or ct_start.STARTED), \
             patch.object(ct_start.kr, "cmd_charger_test", fake_ct), \
             patch.object(ct_start.kr, "charger_cable", lambda link, choice=None: {"run": "off"}), \
             patch.object(ct_start.charger_summary, "summarize", lambda r: "SUMMARY"), \
             contextlib.redirect_stderr(io.StringIO()):
            rc = ct_start.main(["--wait", "5"])
        self.assertEqual(rc, 0)
        self.assertEqual(depth, 0)                          # every acquire was released
        kinds = [e for e, _ in events]
        start, first_poll = kinds.index("start"), kinds.index("poll")
        between = events[start + 1:first_poll]
        # the first thing after START is a full release (depth 0); the only later acquire before the
        # first poll is the poll's own one-exchange hold
        self.assertEqual(between[0], ("R", 0))
        self.assertEqual([e for e, _ in between[1:]], ["A"] if len(between) > 1 else [])


if __name__ == "__main__":
    unittest.main()
