# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The PD version a charger-test run shows ("PD3.0"). Mirrors include/k2/chargertest/pd_version.hpp; the golden cases are
tests/fixtures/pd_versions.txt (shared with the firmware test and tools/web/ctgrid.js).

The offer proves a version (SPR AVS APDO 3.2, any EPR 3.1, PPS 3.0, a 3.x header 3.0, a 2.0 header 2.0, no PD nothing). A
Get_Revision answer wins when it is as high; Revision is itself a 3.1 message, so an answer under 3.1 is under the proof too.
An answer under the proof shows the proof, marked "*" (docs/CHARGER_QUIRKS.md revision-below-offer)."""

_BASIS_TEXT = {"spr_avs": "offers SPR AVS", "epr": "offers EPR"}


def _code(major, minor):
    return major * 16 + (minor & 15)


def _parse(rev):
    major, _, minor = rev.partition(".")
    return int(major), int(minor or 0)


def derive(wire, rmdo, pps, epr, spr_avs):
    """(major, minor, basis, answer): wire = header spec rev code (0 unknown, 1 PD2.0, 2 PD3.x), rmdo = the Revision answer
    "M.m" or None. Returns None with no version. answer is set only when it is lower than the proof (the shown version is the proof)."""
    proof, basis = 0, None
    if spr_avs:
        proof, basis = _code(3, 2), "spr_avs"
    elif epr:
        proof, basis = _code(3, 1), "epr"
    elif pps:
        proof, basis = _code(3, 0), "pps"
    elif wire == 2:
        proof, basis = _code(3, 0), "header3"
    elif wire == 1:
        proof, basis = _code(2, 0), "header2"
    ans = _parse(rmdo) if rmdo else None
    if ans and proof < _code(3, 1):
        proof, basis = _code(3, 1), "message"
    if ans and _code(*ans) >= proof:
        return ans[0], ans[1], basis, None
    if not proof:
        return None
    return proof >> 4, proof & 15, basis, ans


def label(v):
    """"PD3.2", "PD3.2*" for a marked one, "" for none."""
    if v is None:
        return ""
    return f"PD{v[0]}.{v[1]}" + ("*" if v[3] else "")


def conflict_text(v):
    """What the summary says when an answer is under the proof: "reports Revision 3.1, offers SPR AVS (3.2)"; "" otherwise."""
    if v is None or not v[3]:
        return ""
    why = _BASIS_TEXT.get(v[2])
    return (f"reports Revision {v[3][0]}.{v[3][1]}, " +
            (f"{why} ({v[0]}.{v[1]})" if why else "Revision is a PD 3.1 message"))


def from_evidence(ev):
    """The version of a charger-test run's evidence dict (k2_remote.charger_results()['evidence'])."""
    adv = ev.get("advertised") or {}
    rmdo = ev.get("source_rmdo")
    return derive(ev.get("wire_revision_code", 0), rmdo["revision"] if rmdo else None, bool(adv.get("pps")),
                  bool(adv.get("epr_table")), bool(adv.get("spr_avs")))
