#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Closed-loop UI walker for the K2 charger-test setup: every press is checked against the
expected (stage, cursor) read back from `charger-test status`, so scripted navigation never guesses.

    python3 tools/ui_walk.py [--rounds 8]

Walks Tests -> Custom -> Group -> back -> back -> Spoof cable -> Review (and back) and reports each press
as ok / DROPPED (screen state unchanged) / WRONG (changed, but not as expected). It never presses OK
on the Review START row, and it never starts a test. Restores the "All tests" preset first.

The wizard advances on a pick: OK on a preset row (Tests) goes to Spoof cable, OK on an identity row (Spoof cable)
goes to Review. There is no Next row and every cursor is a 0-based row. Back from Spoof cable lands on the preset in force,
back from Review on the identity in force: those cursors are "whatever the device had" (None below).
"""
import argparse
import sys
import time
from argparse import Namespace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_lock
import k2_remote as kr

DEFAULT_MASK = 0x38FFFF     # charger_test::kDefaultAlgorithms: every algorithm selected except the three revision rows (Class B included)
# Setup lists, 0-based. The Tests step ("algorithms"): rows 0 All tests, 1 PD only, 2 Legacy, 3 Vendor, 4 Custom...,
# 5 Last results; a preset pick advances to Cable. The Custom list: rows 0..5 are the families, 6 Done.
# Review has one row, START (0): NEXT_ROW is kept as the name of that top row for callers (ct_start).
NEXT_ROW = 0
ALG_ALL_ROW, ALG_CUSTOM_ROW, ALG_FAMILY_ROW = 0, 4, 0   # Tests: All tests, Custom...; Custom: the first family (Huawei)
SETTLE_S = 0.8


class Walk:
    def __init__(self, link):
        self.link = link
        self.presses = 0
        self.dropped = 0
        self.wrong = 0
        self.log = []

    def where(self):
        st = kr.cmd_charger_test(self.link, Namespace(test_cmd="status"))
        return st["stage"], st["cursor"], st["selection"]["configured_mask"]

    @staticmethod
    def _landed(now, expect):
        """now = (stage, cursor, ...); a cursor of None in `expect` accepts any row (the device picks it)."""
        return now[0] == expect[0] and (expect[1] is None or now[1] == expect[1])

    def press(self, button, expect, long=False):
        """Press `button` (long = a long press); expect = (stage, cursor) after it. Returns True when it landed."""
        before = self.where()
        kr.cmd_press(self.link, Namespace(button=button, ms=0, long=long))
        self.presses += 1
        deadline, now = time.monotonic() + SETTLE_S, before
        while time.monotonic() < deadline:
            now = self.where()
            if self._landed(now, expect):
                self.log.append(f"ok       {button:5} {before[:2]} -> {now[:2]}")
                return True
            time.sleep(0.03)
        kind = "DROPPED" if now == before else "WRONG"
        if kind == "DROPPED":
            self.dropped += 1
        else:
            self.wrong += 1
        self.log.append(f"{kind:8} {button:5} {before[:2]} -> {now[:2]}, expected {expect}")
        return False

    def move_to(self, cursor):
        """+/- presses to a cursor on the current setup list (every step verified)."""
        while True:
            stage, cur, _ = self.where()
            if cur == cursor:
                return True
            nxt = cur + (1 if cursor > cur else -1)
            if not self.press("plus" if nxt > cur else "minus", (stage, nxt)):
                return False

    def to_algorithms(self):
        for _ in range(4):
            stage, cur, _ = self.where()
            if stage == "algorithms":
                return True
            if stage not in ("custom", "group", "cable", "identity", "review", "ask", "refuse", "waiting"):
                return False
            back = {"custom": ("algorithms", ALG_CUSTOM_ROW), "group": ("custom", None),
                    "cable": ("algorithms", None),         # the cursor is on the preset in force
                    "identity": ("cable", None), "review": ("cable", None),   # on the identity in force
                    "ask": ("review", NEXT_ROW),   # retired stage: never entered
                    "refuse": ("review", NEXT_ROW),   # the Ra guard's refusal ("Cable connected"): BACK returns to Review
                    "waiting": ("review", NEXT_ROW)}[stage]   # START's wait for the charger: BACK cancels it, Rd released
            if back[1] is None:
                kr.cmd_press(self.link, Namespace(button="back", ms=0, long=False))
                self.presses += 1
                time.sleep(0.4)
            elif not self.press("back", back):
                return False
        return self.where()[0] == "algorithms"

    def pick_all(self):
        """On the Tests step: pick "All tests" (OK advances to Cable) and verify the default mask is in force."""
        if not self.move_to(ALG_ALL_ROW):
            return False
        return self.press("ok", ("cable", None)) and self.where()[2] == DEFAULT_MASK

    def round(self):
        ok = self.to_algorithms() and self.pick_all()
        ok = ok and self.press("back", ("algorithms", ALG_ALL_ROW))     # Cable: BACK keeps the preset row
        ok = ok and self.move_to(ALG_CUSTOM_ROW) and self.press("ok", ("custom", NEXT_ROW))
        ok = ok and self.move_to(ALG_FAMILY_ROW) and self.press("ok", ("group", NEXT_ROW))
        ok = ok and self.press("back", ("custom", ALG_FAMILY_ROW))      # a family: BACK keeps the row
        ok = ok and self.press("back", ("algorithms", ALG_CUSTOM_ROW))  # Custom: BACK is the Tests step
        ok = ok and self.pick_all()                                      # a preset pick advances to Cable
        ok = ok and self.press("ok", ("review", NEXT_ROW))               # a pick on Cable advances to Review
        ok = ok and self.press("back", ("cable", None))                  # never OK on START
        return ok


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rounds", type=int, default=8)
    ap.add_argument("--path")
    ap.add_argument("--timeout", type=float, default=1.0)
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args(argv)
    with k2_lock.hold():
        link = kr.RemoteLink(kr.open_transport(a.path), a.timeout)
        link.call(kr.CMD_SET_PAGE, bytes([2]))                          # Charger test page
        w = Walk(link)
        good = 0
        for i in range(a.rounds):
            r = w.round()
            good += r
            print(f"round {i + 1}: {'ok' if r else 'FAILED'}")
        if a.verbose or good != a.rounds:
            print("\n".join(w.log))
        print(f"{good}/{a.rounds} rounds reached Review and back; {w.presses} presses, "
              f"{w.dropped} dropped, {w.wrong} wrong")
        link.call(kr.CMD_SET_PAGE, bytes([0]))
        return 0 if good == a.rounds and not w.dropped and not w.wrong else 1


if __name__ == "__main__":
    sys.exit(main())
