# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/oem_watts.py against the golden vectors shared with the firmware (tests/test_oem_watts.cpp)."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import oem_watts  # noqa: E402

FIXTURE = Path(__file__).parent / "fixtures" / "oem_watts.txt"


def _rows(kind):
    for line in FIXTURE.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        f = [x.strip() for x in line.split("|")]
        if f[0] == kind:
            yield f


class OemWattsTest(unittest.TestCase):
    def test_the_golden_labels(self):
        rows = list(_rows("label"))
        assert len(rows) >= 10
        bad = [(f, oem_watts.label(f[1], int(f[2]))) for f in rows if oem_watts.label(f[1], int(f[2])) != f[3]]
        assert not bad, bad

    def test_the_golden_claims(self):
        rows = list(_rows("claim"))
        assert len(rows) >= 12
        bad = []
        for f in rows:
            level = ("vooc", "X", 0, 0, int(f[4]), f[5] == "y")
            got = oem_watts.claim(level, f[1], int(f[2]), int(f[3], 16))
            if got != f[6]:
                bad.append((f, got))
        assert not bad, bad

    def test_the_table_matches_the_firmware(self):
        src = (Path(__file__).resolve().parents[1] / "include/k2/chargertest/oem_watts.hpp").read_text(encoding="utf-8")
        for fam, word, mv, ma, w, ceiling in oem_watts.LEVELS:
            row = f'{{Family::Vooc, "{word}", {mv}, {ma}, {w}, {"true" if ceiling else "false"}}},'
            assert row in src, row

    def test_the_hardware_ceiling_reads_every_supervooc_level_claimed(self):
        got = oem_watts.vooc_levels({"end": "stopped", "vmax_mv": 11020, "ceiling_ma": 10900})
        assert got == [("VOOC 20W", "not tested"), ("SVOOC 50W", "claimed"), ("SVOOC 65W", "claimed"),
                       ("SVOOC 100W", "claimed"), ("SVOOC 120W", "claimed")], got
        assert all(s == "not tested" for _, s in oem_watts.vooc_levels(None))

    def test_step_watts(self):
        scp = {"protocol": "scp", "target_mv": 10000, "request_ma": 2250}
        assert oem_watts.chip(scp) == "10V 22.5W"
        assert oem_watts.chip({"protocol": "scp", "target_mv": 5000, "request_ma": 0}) == ""
        ufcs = {"protocol": "ufcs", "target_mv": 9000, "raw_pdo": 50 | 110 << 9 | 10 << 18 | 60 << 25}
        assert oem_watts.chip(ufcs) == "9V 27W"
        assert oem_watts.step_mw({"protocol": "afc", "target_mv": 9000, "request_ma": 1670}) == 0


if __name__ == "__main__":
    unittest.main()
