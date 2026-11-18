# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/pd_version.py against the golden cases shared with the firmware (tests/test_pd_version.cpp) and the run header line."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import charger_summary  # noqa: E402
import pd_version  # noqa: E402

FIXTURE = Path(__file__).parent / "fixtures" / "pd_versions.txt"


def _vectors():
    for line in FIXTURE.read_text(encoding="utf-8").splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        wire, rmdo, offer, want, long_text = (f.strip() for f in line.split("|"))
        yield line, int(wire), None if rmdo == "-" else rmdo, offer, want, long_text


def test_the_golden_versions():
    n = 0
    for line, wire, rmdo, offer, want, long_text in _vectors():
        v = pd_version.derive(wire, rmdo, "p" in offer, "e" in offer, "s" in offer)
        assert (pd_version.label(v) or "-") == want, line
        assert (pd_version.conflict_text(v) or "-") == long_text, line
        n += 1
    assert n >= 20


def _head(ev):
    run = {"run_id": 1, "state": "done", "reason": "none", "evidence": ev, "steps": []}
    return charger_summary.summarize(run).splitlines()[0]


def test_run_header_leads_with_the_version():
    ev = {"wire_revision_code": 2, "pdo_count": 6, "advertised": {"pps": True}}
    assert _head(ev).startswith("Run 1 · PD3.0 · 6 PDOs")
    assert "PD3.x" not in _head(ev)
    ev = {"wire_revision_code": 2, "pdo_count": 8, "advertised": {"epr_table": True, "spr_avs": True}}
    assert _head(ev).startswith("Run 1 · PD3.2 · 8 PDOs")
    ev = {"wire_revision_code": 1, "pdo_count": 5, "advertised": {}}
    assert _head(ev).startswith("Run 1 · PD2.0 · 5 PDOs")


def test_run_header_says_both_when_the_answer_is_under_the_offer():
    ev = {"wire_revision_code": 2, "pdo_count": 8, "source_rmdo": {"revision": "3.1"}, "advertised": {"spr_avs": True}}
    assert _head(ev).startswith("Run 1 · PD3.2* (reports Revision 3.1, offers SPR AVS (3.2)) · 8 PDOs")
    ev["source_rmdo"] = {"revision": "3.2"}   # equal: the answer, no mark
    assert _head(ev).startswith("Run 1 · PD3.2 · 8 PDOs")
    ev["source_rmdo"] = {"revision": "3.3"}   # higher: the answer
    assert _head(ev).startswith("Run 1 · PD3.3 · 8 PDOs")


def test_run_header_drops_the_version_without_pd():
    head = _head({"wire_revision_code": 0, "pdo_count": 0, "advertised": {}})
    assert head.startswith("Run 1 · 0 PDOs") and "PD" not in head.split(" PDOs")[0]
