# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""ct_start preflight, no device."""
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import ct_start


class Preflight(unittest.TestCase):
    def run_pf(self, states):
        it = iter(states)
        with patch.object(ct_start.kr, "cmd_state", lambda l, a: next(it)), \
             patch.object(ct_start.kr, "cmd_action", lambda l, a: None):
            return ct_start.preflight(None, sleep=lambda s: None)

    def test_low_vbus_inline_refused(self):
        self.assertIn("4.30 V", self.run_pf([{"vbus_mv": 4300, "pd_valid": True, "inline": True}]))

    def test_low_vbus_without_sink_lets_start_present_rd(self):
        self.assertIsNone(self.run_pf([{"vbus_mv": 0, "pd_valid": False}]))

    def test_good_source_passes(self):
        self.assertIsNone(self.run_pf([{"vbus_mv": 5000, "pd_valid": True}]))

    def test_rescan_then_vbus_gone_refused(self):
        r = self.run_pf([{"vbus_mv": 5000, "pd_valid": False}, {"vbus_mv": 0, "pd_valid": False}])
        self.assertIn("dropped", r)

    def test_legacy_only_proceeds(self):
        self.assertIsNone(self.run_pf([{"vbus_mv": 5000, "pd_valid": False}, {"vbus_mv": 5000, "pd_valid": False}]))


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


if __name__ == "__main__":
    unittest.main()
