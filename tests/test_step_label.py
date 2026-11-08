# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/step_label.py against the golden vectors shared with the firmware (tests/test_step_label.cpp)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import step_label  # noqa: E402

FIXTURE = Path(__file__).parent / "fixtures" / "step_labels.txt"
# fixture algorithm -> the protocol name of the wire row (k2_remote.parse_charger_test_results)
WIRE = {"pd2": "pd2-fixed", "pd3": "pd3x-fixed", "epr": "epr-fixed", "pd30rev": "pd3.0-revision",
        "pd31rev": "pd3.1-revision", "pd32rev": "pd3.2-revision", "vooc": "vooc-5v-handshake",
        "svooc": "svooc-5v-handshake", "svooc65": "svooc65-5v-handshake"}


def _vectors():
    for line in FIXTURE.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        alg, pos, mv, ma, pdo, flags, prev_mv, want = (f.strip() for f in line.split("|"))
        row = {"protocol": WIRE.get(alg, alg), "pos": int(pos), "target_mv": int(mv), "request_ma": int(ma),
               "raw_pdo": int(pdo, 16), "continuous": "c" in flags, "avs": "a" in flags}
        prev = None if not int(prev_mv) else {"protocol": row["protocol"], "pos": row["pos"], "target_mv": int(prev_mv),
                                              "request_ma": 1000, "raw_pdo": row["raw_pdo"], "continuous": False}
        yield line, row, prev, want


class StepLabelTest(unittest.TestCase):
    def test_the_golden_vectors(self):
        vectors = list(_vectors())
        assert len(vectors) > 40
        bad = [(line, step_label.label(row, prev), want) for line, row, prev, want in vectors
               if step_label.label(row, prev) != want]
        assert not bad, bad


    def test_amps_only_where_meaningful(self):
        pps = {"protocol": "pps", "pos": 6, "target_mv": 20000, "request_ma": 1000, "raw_pdo": 0xC1A42164}
        assert step_label.label(pps) == "PPS 20V 5A"
        assert step_label.label(dict(pps, raw_pdo=0)) == "PPS 20V"       # no offer known: no current is invented
        assert step_label.label({"protocol": "qc3", "pos": 0, "target_mv": 12000, "request_ma": 0}) == "QC3 12V"
        assert step_label.label({"protocol": "scp", "pos": 0, "target_mv": 10000, "request_ma": 2250}) == "SCP 10V 2.25A"


    def test_hunt_rows_read_mi(self):
        row = {"protocol": "pd3x-fixed", "pos": 5, "target_mv": 20000, "request_ma": 6000, "raw_pdo": 0x6412C}
        assert step_label.is_hunt(row)
        assert step_label.label(row) == "Mi 20V 6A"


    def test_the_full_current_pps_row_is_not_a_hunt(self):
        # The wire has no hunt flag: a plan row asks at most 5 A (the cable ceiling), the OEM probe always 6 A.
        full = {"protocol": "pps", "pos": 6, "target_mv": 11000, "request_ma": 5000, "raw_pdo": 0xC0DC3264}
        assert not step_label.is_hunt(full)
        assert step_label.label(full) == "PPS 11V 5A"
        assert step_label.label(dict(full, request_ma=3000)) == "PPS 11V 3A"       # the requested, not the advertised current
        assert step_label.label(dict(full, request_ma=1000)) == "PPS 11V 5A"       # a 1 A row still shows the advertised one
        assert step_label.is_hunt(dict(full, request_ma=6000))
        assert step_label.label(dict(full, request_ma=6000)) == "Mi 11V 6A"
        # A heading that states the offer drops the amps from its rows, but the full row always states what it asked.
        assert step_label.params(dict(full, request_ma=1000), amps=False) == "11V"
        assert step_label.params(dict(full, request_ma=3000), amps=False) == "11V 3A"

    def test_a_ramp_reads_from_the_previous_row_of_the_same_offer(self):
        a = {"protocol": "pps", "pos": 6, "target_mv": 11000, "request_ma": 1000, "raw_pdo": 0xC1A42164, "continuous": False}
        b = dict(a, target_mv=8000, continuous=True)
        assert step_label.labels([a, b]) == ["PPS 11V 5A", "PPS 11→8V"]
        other = dict(b, pos=7)   # another offer: no ramp
        assert step_label.labels([a, other]) == ["PPS 11V 5A", "PPS 8V 5A"]


    def test_ranges(self):
        assert step_label.pdo_range(0xC1A42164) == "3.3-21V 5A"
        assert step_label.pdo_range(0xC0DC213C) == "3.3-11V 3A"
        assert step_label.pdo_range(0xD230968C) == "15-28V 5A"
        assert step_label.pdo_range(0xE004B000) == "9-15V 3A"
        assert step_label.pdo_range(0x2D12C) == ""
        assert step_label.range_text(3300, 11000, 3000) == "3.3-11V 3A"
        assert step_label.range_text(9000, 20000, 0, 60000) == "9-20V 60W"


    def test_the_run_summary_and_the_card_text_use_the_labels(self):
        # tests/test_charger_summary.py is pytest-style, which `unittest discover` skips: the gate checks the wording here
        import charger_summary
        row = {"result": "pass", "reason": "none", "detail_reason": "none", "measured_mv": 0, "attempts": 1}
        steps = [dict(row, protocol="pd3x-fixed", pos=5, target_mv=20000, request_ma=1000, raw_pdo=0x641F4,
                      continuous=False, measured_mv=19900),
                 dict(row, protocol="pd3x-fixed", pos=5, target_mv=20000, request_ma=6000, raw_pdo=0x641F4,
                      continuous=False, measured_mv=19900),
                 dict(row, protocol="spr-avs", pos=4, target_mv=15000, request_ma=1000, raw_pdo=0xE004B000,
                      continuous=False, avs=True),
                 dict(row, protocol="spr-avs", pos=4, target_mv=9000, request_ma=1000, raw_pdo=0xE004B000,
                      continuous=True, avs=True)]
        text = charger_summary.summarize({"run_id": 1, "state": "done", "reason": "none", "evidence": {}, "steps": steps})
        self.assertIn("- PD: 20V 5A, all pass", text)
        self.assertIn("- Mi 20V 6A, all pass", text)
        self.assertIn("- AVS 9-15V 3A: 15V ✓, 15→9V ✓", text)
        # A PPS offer: the full-current row is last of its group and reads "11V 5A" (the heading carries the range and current).
        pps = dict(row, protocol="pps", pos=6, raw_pdo=0xC0DC3264, avs=False)
        steps = [dict(pps, target_mv=8000, request_ma=1000, continuous=False, measured_mv=8000),
                 dict(pps, target_mv=11000, request_ma=1000, continuous=True, measured_mv=11000),
                 dict(pps, target_mv=11000, request_ma=6000, continuous=False, measured_mv=11000),
                 dict(pps, target_mv=11000, request_ma=5000, continuous=False, measured_mv=11000)]
        text = charger_summary.summarize({"run_id": 1, "state": "done", "reason": "none", "evidence": {}, "steps": steps})
        self.assertIn("- PPS 5-11V 5A: 8V ✓, 8→11V ✓, 11V 5A ✓", text)
        self.assertIn("- Mi 11V 6A, all pass", text)
        card = {"no": 1, "name": "", "pdos": 7, "runs": 1, "vid": 1, "pid": 2, "fw": "0", "state": "Complete", "reason": "",
                "fp": "1", "boots": 1, "uptime_s": 1, "handover": "not tried", "handover_ms": 0, "groups": {},
                "ranges": [{"alg": "PD3.2+ SPR AVS", "lo_mv": 15000, "hi_mv": 28000, "max_mv": 20000, "fail_mv": 0, "max_ma": 3000}]}
        self.assertIn("- AVS 15-28V 3A: reached 20 V", charger_summary.summarize_card(card))


if __name__ == "__main__":
    unittest.main()
