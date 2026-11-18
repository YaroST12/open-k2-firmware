# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""OEM power levels by their watts: "SVOOC 120W", "SCP 22.5W", "Mi 55W" instead of a protocol version ("SVOOC65").

Mirrors include/k2/chargertest/oem_watts.hpp; the golden vectors tests/fixtures/oem_watts.txt are checked against both. The K2
never draws current in a charger test: a level's watts are its volts x the current the protocol names (the SCP limit the K2
writes, the UFCS mode's advertised current, the SuperVOOC ask the adapter went on under), so every sentence that carries them
says "claimed". A protocol whose exchange names no current (AFC, FCP, VIVO, QC) keeps its volts.
"""
import step_label

# (family, word, mV, mA, watts, the ceiling stage can claim it). Sources: docs/CHARGING_PROTOCOL_MAP.md section 2 (inferred),
# the OnePlus 120 W adapter's rating (factory) and its 11.02 V x 10.9 A ceiling claim on hardware (2026-11-15).
LEVELS = [
    ("vooc", "VOOC", 5000, 4000, 20, False),
    ("vooc", "SVOOC", 10000, 5000, 50, True),
    ("vooc", "SVOOC", 10000, 6500, 65, True),
    ("vooc", "SVOOC", 10000, 10000, 100, True),
    ("vooc", "SVOOC", 11000, 11000, 120, True),
]
CLAIMED, NOT_REACHED, NOT_TESTED = "claimed", "not reached", "not tested"
# svooc_ceiling::End names ("cap" in k2_remote's readout, "time cap" in a card's Deep block); anything else above the claim is
# "not tested"
_RAN_ITS_COURSE = ("judged", "stopped", "cap", "time cap")


def watts(mw):
    """'120W', '22.5W' (rounded to 0.1 W, the tenth only when it is not 0; whole watts from 100 W, so at most 5 characters);
    '' for 0 mW."""
    if not mw:
        return ""
    dw = (mw + 50) // 100
    if dw >= 1000:
        return f"{(mw + 500) // 1000}W"
    return f"{dw // 10}" + (f".{dw % 10}" if dw % 10 else "") + "W"


def label(word, mw):
    """'SVOOC 120W', 'SCP 22.5W', 'Mi 55W'; the bare word for 0 mW."""
    w = watts(mw)
    return f"{word} {w}" if w else word


def claim_w(vmax_mv, word):
    """svooc_ceiling::claim_w: the top VBUS x the 0x86 ask (100 mA per count, 9 bits), in whole watts."""
    return (vmax_mv * min(word, 0x1FF) * 100 + 500000) // 1000000


def claim(level, end, vmax_mv, word):
    """What the SuperVOOC ceiling stage proves of one level (a LEVELS row): claimed / not reached / not tested."""
    w, ceiling = level[4], level[5]
    if not ceiling:
        return NOT_TESTED
    if word and vmax_mv and claim_w(vmax_mv, word) >= w:
        return CLAIMED
    return NOT_REACHED if end in _RAN_ITS_COURSE else NOT_TESTED


def vooc_levels(sv):
    """[(label, state)] of the VOOC family from a ceiling readout dict (k2_remote.charger_svooc_cap or the card's deep block:
    end, vmax_mv, word or ceiling_ma); every level 'not tested' without one."""
    sv = sv or {}
    word = sv.get("word")
    if word is None and isinstance(sv.get("ceiling"), dict):   # k2_remote.charger_svooc_cap: {"ceiling": {"word": ...}}
        word = sv["ceiling"].get("word")
    if word is None:
        word = (sv.get("ceiling_ma") or 0) // 100
    end, vmax = sv.get("end") or "none", sv.get("vmax_mv") or 0
    return [(label(lv[1], lv[4] * 1000), claim(lv, end, vmax, word)) for lv in LEVELS if lv[0] == "vooc"]


def ufcs_amps_ma(s):
    """The top current of the UFCS mode a step samples (charger_test.hpp pack_ufcs_mode, kept in raw_pdo); 0 without one."""
    pdo = s.get("raw_pdo") or 0
    return ((pdo >> 25) & 127) * 50 if s.get("protocol") == "ufcs" and pdo else 0


def step_mw(s):
    """A step's own watts: SCP the limit the K2 writes, UFCS its mode's advertised current; 0 for any other step."""
    p, mv = s.get("protocol"), s.get("target_mv") or 0
    ma = step_label.amps_ma(s) if p == "scp" else ufcs_amps_ma(s) if p == "ufcs" else 0
    return mv * ma // 1000 if ma and mv else 0


def chip(s):
    """'10V 22.5W', '9V 27W': the step's volts and its watts; '' when step_mw() is 0."""
    mw = step_mw(s)
    return f"{step_label.milli(s['target_mv'])}V {watts(mw)}" if mw else ""
