#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Decode a charger report card (the 144 byte payload of include/k2/chargertest/report_card.hpp) into the dict that
src/chargertest/report_card_json.cpp writes for CARDS.JSON, so `k2_remote.py cards export` and the USB drive agree.

Usage: report_card.py PAYLOAD.hex   (prints the JSON)
The name tables mirror src/chargertest/charger_test.cpp; tests/test_report_card_py.py parses that file and compares.
"""
import json
import sys

PAYLOAD = 198   # v8 size, kept for the QR expand path; v9 cards are 214, v10 250
PAYLOAD_V9 = 214   # v9: + the 16-byte spread block at V9_AT
PAYLOAD_V10 = 250   # v10: + the 36-byte deep-sweep summary at V10_AT
PAYLOAD_V7 = 196
PAYLOAD_V6 = 190
UFCS_AT = 178   # v6: 4 modes of u24 (lo_dv 8 bits, hi_dv 9 bits, current in 50 mA units 7 bits); hi_dv 0 = unused slot
REPEAT_AT = 190   # v7: times u8, last_boots u16, last_uptime_s u24 (a v6 payload has its CRC here: read only when b[33] >= 7)
V8_AT = 196     # v8: rebalanced u8 + spare (a v7 payload ends at 196: its CRC sits here and is not read)
V9_AT = 198     # v9: 4 ranges x 4 levels of int8 mV setpoint error, -128 = none (a v8 payload ends at 198: its CRC is not read)
V10_AT = 214    # v10: 4 ranges x 9 bytes deep-sweep summary (int8 min/max/worst mV, then 5 x 9-bit 100 mV fields in 6 bytes)
MAX_UFCS = 4
ALGORITHMS = ["PD2.0 fixed", "PD3.x fixed", "PD3.1+ EPR", "PD3.2+ SPR AVS", "PPS (PD3.x)",
              "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC 5V handshake", "SVOOC 5V handshake",
              "SVOOC65 5V handshake", "UFCS", "VIVO", "PD3.0 revision", "PD3.1 revision", "PD3.2 revision", "EPR AVS",
              "QC2 Class B", "QC3 Class B"]
GROUPS = ["PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO"]
REASONS = ["", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
           "no power monitor", "no source", "load attached", "tool busy", "fake offers",
           "request failed", "timeout", "voltage outside band", "hard reset", "source lost",
           "cancelled", "5V release failed", "offers changed", "PD active: use QC port",
           "revision cannot be forced", "select at least one test", "physical cable limit", "unsupported PDO layout",
           "charger stayed in PD", "rebooted during request", "rebooted during handover", "EPR entry refused", "no contract", "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered", "blocked by PD", "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START", "Class A only (12 V max)", "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target", "withdrawn by source", "wrong voltage on 5V contract", "needs load", "floored above target", "step resolution"]
STATES = ["Setup", "Verify 5V", "Negotiating", "Hold", "Return to 5V", "Complete", "Stopped",
          "UNSAFE: unplug source", "Detach charger", "Initializing..."]
HANDOVER = ["not tried", "fell back", "stayed PD", "fault", "reboot", "in PD contract"]
VERDICT = ["none", "pass", "fail", "skip"]
MAX_RANGES = 4
# What a group's detail byte means: pd::Err (stored as 128 + err) and pd::SinkResult for PD, fc::Fail for the rest.
# Mirrors fc::name(Fail), pd::err_name and pd::result_name; tests/test_report_card_py.py parses those and compares.
FC_FAILS = ["none", "need confirm", "over limit", "no power monitor", "busy", "unsupported", "no handshake",
            "no response", "voltage not reached", "overvoltage", "source lost", "aborted", "PD or load active",
            "needs load", "handshake ok", "detected"]
PD_ERRS = ["ok", "no source capabilities", "bad PDO position", "above the voltage cap", "bad voltage",
           "live sink attached", "busy", "PDO advertises no current"]
SINK_RESULTS = ["none", "ok", "rejected", "source busy", "timeout", "no GoodCRC", "hard reset", "soft reset",
                "dropped to 5V", "over the cap", "EPR refused", "bad choice"]
# Step.detail of an EPR skip, above 128 + pd::Err; mirrors ct::epr_detail_name(), tests/test_report_card_py.py compares.
EPR_DETAILS = {136: "EPR: entry refused", 137: "EPR: source rejects cable", 138: "EPR: source has no VCONN",
               139: "EPR: refused (RDO)", 140: "EPR: source unable now", 141: "EPR: 5V PDO lacks EPR bit",
               144: "EPR: above K2's 36V monitor", 145: "EPR: above K2's 48V monitor", 146: "EPR: above the K2's monitor",
               147: "EPR offered: needs EPR cable", 148: "EPR list withheld: cable-gated",
               149: "EPR offered; entry: refused", 150: "EPR offered; entry: cable rejected",
               151: "EPR offered; entry: no VCONN", 152: "EPR offered; entry: refused (RDO)",
               153: "EPR offered; entry: unable now", 154: "EPR offered; entry: 5V PDO lacks EPR bit",
               155: "not unlocked (no OEM chain)", 156: "not unlocked (chain unfinished)", 157: "not unlocked (3 A cable)",
               158: "not unlocked (vendor not accepted)", 159: "not unlocked (source reset)"}
FORMAT = 10   # payload byte 33: 0 = first cards (u16 mV max, plain detail), 1 = 9+9 bit 100 mV voltages, 6 bit detail, ... 7 = repeat counter, 8 = + the re-balance count at V8_AT, 9 = + the spread block at V9_AT, 10 = + the deep-sweep summary at V10_AT
             # 2 = byte 54 pdp_w and byte 55 flags (bit0 name_auto),
             # 3 = range lo bits 9..15 = APDO max current (50 mA), flags bits 1..7 = highest fixed PDO current (50 mA),
             # 4 = per-EPR-level verdicts (3 entries of 6 bytes: u16 dv + 4-byte group packing),
             # 5 = the 16-byte cable block at offset 162 (seq, vid/pid, max mV/mA, kind, flags),
             # 6 = the UFCS modes at offset 178 (4 x u24: lo 100 mV, hi 100 mV, current in 50 mA),
             # 7 = the repeat block at offset 190 (times, last boot, last uptime of identical re-runs)
# VID -> brand, mirrors report_card::brand(); tests/test_report_card_py.py parses the header and compares.
BRANDS = {0x05AC: "Apple", 0x04E8: "Samsung", 0x18D1: "Google", 0x2717: "Xiaomi", 0x12D1: "Huawei", 0x17EF: "Lenovo",
          0x413C: "Dell", 0x03F0: "HP", 0x2A70: "OnePlus", 0x22D9: "OPPO", 0x0B05: "ASUS", 0x050D: "Belkin",
          0x22B8: "Motorola", 0x19D2: "ZTE", 0x291A: "Anker"}


class BadCard(ValueError):
    pass


EPR_LEVELS = ["28V", "36V", "48V"]   # v4: per-EPR-level verdicts, target voltages in 100 mV units (280, 360, 480)


def _u(b, at, n):
    return int.from_bytes(b[at:at + n], "little")


def group_why(group, detail, reason):
    """The readable cause of a failed or skipped group: the engine's code when one was stored, else the reason."""
    if reason == "no QC3+ response":
        return "no QC3+"   # the engine code is "no response"; the group says "no QC3+" (report_card.hpp group_why)
    if detail:
        if group == "PD":
            if detail in EPR_DETAILS:   # an EPR skip: the monitor ceiling or Enter_Failed's reason (charger_test.hpp kDetailEprFail)
                return EPR_DETAILS[detail]
            if detail >= 128 and detail - 128 < len(PD_ERRS):
                return PD_ERRS[detail - 128]
            if detail < len(SINK_RESULTS):
                return SINK_RESULTS[detail]
        elif detail < len(FC_FAILS):
            return FC_FAILS[detail]
    return reason


QUALCOMM_VID = 0x05C6
QC5_MIN_WATTS = 100


MOTOROLA_VID, LENOVO_VID = 0x22B8, 0x17EF


def qc_label(vid, pdp_w, ranges, cable_ma=0):
    """Passive OEM label, mirrors report_card::qc_label() (never a behaviour key). A Qualcomm VID with a PPS range is "QC4+";
    100 W or more (or an EPR AVS range) makes it "QC5". Otherwise: "Moto TurboPower" (Motorola / Lenovo VID and a PPS APDO above
    5 A), "Samsung SFC 3.0" (PPS to 20 V at 3 A) or "Samsung SFC 2.0" (PPS to 15 V at 3 A, or 3.3-11 V at 4.05 A behind a 5 A
    cable); anything else has none."""
    pps = [r for r in ranges if r["alg"] == "PPS (PD3.x)"]
    if not pps:
        return ""
    if vid == QUALCOMM_VID:
        return "QC5" if pdp_w >= QC5_MIN_WATTS or any(r["alg"] == "EPR AVS" for r in ranges) else "QC4+"
    ma = lambda r: r.get("max_ma", 0)
    if vid in (MOTOROLA_VID, LENOVO_VID) and any(ma(r) > 5000 for r in pps):
        return "Moto TurboPower"
    if any(r["hi_mv"] >= 20000 and ma(r) >= 3000 for r in pps):
        return "Samsung SFC 3.0"
    if any(r["hi_mv"] >= 15000 and ma(r) >= 3000 for r in pps) or (
            cable_ma >= 5000 and any(0 < r["lo_mv"] <= 3300 and r["hi_mv"] >= 11000 and ma(r) >= 4050 for r in pps)):
        return "Samsung SFC 2.0"
    return ""


def decode(b):
    """Payload bytes -> card dict (key order and omissions as in the C++ renderer)."""
    b = bytes(b)
    if len(b) not in (144, 162, 178, PAYLOAD_V6, PAYLOAD_V7, PAYLOAD, PAYLOAD_V9, PAYLOAD_V10):   # accept v3..v10 payloads
        raise BadCard(f"card payload is {len(b)} bytes, expected 144, 162, 178, {PAYLOAD_V6}, {PAYLOAD_V7}, {PAYLOAD}, {PAYLOAD_V9} or {PAYLOAD_V10}")
    if b[33] > FORMAT:
        raise BadCard("card format is newer than this decoder")
    if b[28] >= len(HANDOVER) or b[29] >= len(STATES) or b[30] >= len(REASONS) or b[31] > MAX_RANGES:
        raise BadCard("card header field out of range")
    name = b[34:54].split(b"\0")[0].decode("ascii", errors="replace")
    groups = {}
    for g in range(len(GROUPS)):
        q = 56 + 4 * g
        verdict, reason = b[q] >> 6, b[q] & 0x3F
        if reason >= len(REASONS):
            raise BadCard("group reason out of range")
        if verdict == 0:
            continue
        if b[33] == 0:   # first cards: detail byte, u16 mV, no failing target
            detail, max_mv, fail_mv = b[q + 1], (_u(b, q + 2, 2) + 50) // 100 * 100, 0
        else:
            v = _u(b, q + 1, 3)
            detail = 128 + (v >> 18 & 0x1F) if v >> 23 & 1 else v >> 18 & 0x1F
            max_mv, fail_mv = (v & 0x1FF) * 100, (v >> 9 & 0x1FF) * 100
        e = {"verdict": VERDICT[verdict]}
        if g == 0 and b[33] >= 3 and b[55] >> 1:
            e["max_ma"] = (b[55] >> 1) * 50
        if max_mv:
            e["max_mv"] = max_mv
        if fail_mv:
            e["fail_mv"] = fail_mv
        if reason:
            e["reason"] = REASONS[reason]
        if detail:
            e["detail"] = detail
            e["why"] = group_why(GROUPS[g], detail, REASONS[reason])
        groups[GROUPS[g]] = e
    ranges = []
    for i in range(b[31]):
        q = 104 + 10 * i
        if b[q] >= len(ALGORITHMS):
            raise BadCard("range algorithm out of range")
        lo = _u(b, q + 2, 2)
        r = {"alg": ALGORITHMS[b[q]], "lo_mv": (lo & 0x1FF) * 100, "hi_mv": _u(b, q + 4, 2) * 100,
             "max_mv": _u(b, q + 6, 2) * 100, "fail_mv": _u(b, q + 8, 2) * 100}
        if b[33] >= 3 and lo >> 9:
            r["max_ma"] = (lo >> 9) * 50
        if b[33] >= 9 and len(b) >= PAYLOAD_V9:   # v9: the setpoint error in mV per passed level, low to high
            errs = []
            for k in range(4):
                e = b[V9_AT + 4 * i + k]
                if e == 0x80:
                    continue
                errs.append(e - 256 if e > 127 else e)
            if errs:
                r["err_mv"] = errs
        if b[33] >= 10 and len(b) >= PAYLOAD_V10:   # v10: the deep sweep of this APDO (all-zero block = none)
            q = V10_AT + 9 * i
            v = int.from_bytes(b[q + 3:q + 9], "little")
            peak_dv = v >> 9 & 0x1FF
            if peak_dv:
                sw = {"min_mv": b[q] - 256 if b[q] > 127 else b[q],
                      "max_mv": b[q + 1] - 256 if b[q + 1] > 127 else b[q + 1],
                      "worst_mv": b[q + 2] - 256 if b[q + 2] > 127 else b[q + 2],
                      "worst_at_mv": (v & 0x1FF) * 100, "peak_mv": peak_dv * 100, "peak_at_mv": (v >> 18 & 0x1FF) * 100}
                fold_dv, fold_from_dv = v >> 27 & 0x1FF, v >> 36 & 0x1FF
                if fold_dv:
                    sw["fold_mv"] = fold_dv * 100
                    sw["fold_from_mv"] = fold_from_dv * 100
                r["sweep"] = sw
        ranges.append(r)
    # V4: per-EPR-level verdicts
    epr_levels = {}
    if b[33] >= 4:
        for i in range(len(EPR_LEVELS)):
            q = 144 + 6 * i
            dv = _u(b, q, 2)
            if dv == 0:
                continue   # no EPR at this level
            verdict, reason = b[q + 2] >> 6, b[q + 2] & 0x3F
            if reason >= len(REASONS):
                raise BadCard("EPR level reason out of range")
            v = _u(b, q + 3, 3)
            detail = 128 + (v >> 18 & 0x1F) if v >> 23 & 1 else v >> 18 & 0x1F
            max_mv, fail_mv = (v & 0x1FF) * 100, (v >> 9 & 0x1FF) * 100
            e = {"verdict": VERDICT[verdict]}
            if max_mv:
                e["max_mv"] = max_mv
            if fail_mv:
                e["fail_mv"] = fail_mv
            if reason:
                e["reason"] = REASONS[reason]
            if detail:
                e["detail"] = detail
                e["why"] = group_why("PD", detail, REASONS[reason])
            epr_levels[EPR_LEVELS[i]] = e
    card = {"no": _u(b, 8, 2), "name": name, "fp": f"{_u(b, 0, 4):08X}", "vid": _u(b, 4, 2), "pid": _u(b, 6, 2)}
    if card["vid"] in BRANDS:
        card["brand"] = BRANDS[card["vid"]]
    if b[33] >= 2:
        if b[54]:
            card["pdp_w"] = b[54]
        if b[55] & 1 and name:
            card["name_auto"] = True
    label = qc_label(card["vid"], card.get("pdp_w", 0), ranges, _u(b, 170, 2) if b[33] >= 5 and len(b) >= 178 else 0)
    if label:
        card["qc"] = label
    card.update({"runs": _u(b, 10, 2), "boots": _u(b, 12, 4), "uptime_s": _u(b, 16, 4)})
    if b[33] >= 7 and len(b) >= PAYLOAD_V7 and b[REPEAT_AT] > 1:   # v7: identical repeats of the stored run (key only when there were any)
        card.update({"times": b[REPEAT_AT], "last_boots": _u(b, REPEAT_AT + 1, 2), "last_uptime_s": _u(b, REPEAT_AT + 3, 3)})
    if b[33] >= 8 and len(b) >= PAYLOAD and b[V8_AT]:   # v8: the source re-balanced its budget mid-run this often
        card["rebalanced"] = b[V8_AT]
    card.update({"fw": f"{_u(b, 20, 4):08X}",
            "state": STATES[b[29]], "reason": REASONS[b[30]], "pdos": b[27], "spec_rev": b[26],
            "handover": HANDOVER[b[28]], "handover_ms": _u(b, 24, 2) * 100, "groups": groups, "ranges": ranges})
    # Always include epr_levels for v4+ cards (even if empty) at the end for consistent ordering
    if b[33] >= 4:
        card["epr_levels"] = epr_levels
    if b[33] >= 5 and len(b) >= 178:   # v5: the cable block at offset 162
        q = 162
        card["cable"] = {"seq": _u(b, q, 4), "vid": _u(b, q + 4, 2), "pid": _u(b, q + 6, 2),
                         "max_mv": _u(b, q + 8, 2), "max_ma": _u(b, q + 10, 2), "kind": b[q + 12],
                         "observed": b[q + 13] & 1, "spoofed": (b[q + 13] >> 1) & 1}
        if b[q + 13] & 4:   # the charger under test powered the K2 (the key only appears then, as in the firmware's JSON)
            card["cable"]["self_powered"] = 1
        g = (b[q + 13] >> 3) & 7   # the Ra guard's reading + 1 (0 = not judged: the key is then absent, as in the firmware's JSON)
        if 1 <= g <= 4:
            card["cable"]["guard"] = ("unknown", "none", "cable", "direct")[g - 1]
        if b[q + 13] & 0x40:       # the user started anyway against a refusing reading
            card["cable"]["guard_bypassed"] = 1
        if b[q + 15] & 1:   # src_flags: the source listed OPPO's SVID (the key only appears then, as in the firmware's JSON)
            card["oppo_svid"] = 1
        if b[q + 15] >> 1:  # bits 1..7: the adapter's SCP current limit in 50 mA units (key only when it was read)
            card["scp_ma"] = (b[q + 15] >> 1) * 50
    if b[33] >= 6 and len(b) >= PAYLOAD_V6:   # v6: the UFCS modes (older cards: the key is absent, their amps are unknown)
        modes = []
        for i in range(MAX_UFCS):
            v = _u(b, UFCS_AT + 3 * i, 3)
            if (v >> 8 & 0x1FF) == 0:
                break   # an unused slot ends the list
            m = {"lo_mv": (v & 0xFF) * 100, "hi_mv": (v >> 8 & 0x1FF) * 100}
            if v >> 17:
                m["max_ma"] = (v >> 17) * 50
            modes.append(m)
        if modes:
            card["ufcs"] = modes
    return card


def dumps(card):
    """The same compact text the firmware renders (no spaces after , and :)."""
    return json.dumps(card, separators=(",", ":"), ensure_ascii=False)


def _mv2(mv):
    """One mV value at 10 mV resolution with one or two decimals: 19748 -> '19.75', 19900 -> '19.9', 20000 -> '20.0'."""
    r = (mv + 5) // 10 * 10
    frac = (r // 10) % 100
    if frac % 10:
        return f"{r // 1000}.{frac // 10}{frac % 10}"
    return f"{r // 1000}.{frac // 10}"


def sweep_text(sw):
    """A range's sweep dict -> the summary line: 'peak 19.75 V at 19.9 V, folds back to 19.42 V from 20.0 V'
    (the FH22 shape); 'peak 21.0 V at 21.0 V' without a fold-back. Mirrors ctsweep::format_summary."""
    if not sw or not sw.get("peak_mv"):
        return "no points"
    t = f"peak {_mv2(sw['peak_mv'])} V at {_mv2(sw['peak_at_mv'])} V"
    if sw.get("fold_mv"):
        t += f", folds back to {_mv2(sw['fold_mv'])} V from {_mv2(sw['fold_from_mv'])} V"
    return t


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print(dumps(decode(bytes.fromhex(open(sys.argv[1]).read().strip()))))
