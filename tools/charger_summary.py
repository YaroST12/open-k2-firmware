#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Compact, human-readable summary of a charger-test result export.

Usage: charger_summary.py RESULTS.json   (the file written by `k2_remote.py charger-test results --out`)
Also used by `k2_remote.py charger-test summary`. summarize_card() renders one report card (the dict of
tools/report_card.py) in the same style, for `k2_remote.py cards show`.
"""
import json
import sys

import report_card
import step_label

PROTO_NAMES = {"pd2-fixed": "PD2 fixed", "pd3x-fixed": "PD fixed", "epr-fixed": "EPR fixed", "spr-avs": "SPR AVS",
               "epr-avs": "EPR AVS", "pps": "PPS", "qc2": "QC2", "qc3": "QC3", "qc3+": "QC3+", "afc": "AFC",
               "fcp": "FCP", "scp": "SCP", "vooc-5v-handshake": "VOOC 5V", "svooc-5v-handshake": "SVOOC",
               "svooc65-5v-handshake": "SVOOC 65W", "ufcs": "UFCS", "vivo": "VIVO",
               "qc2b": "QC2 Class B", "qc3b": "QC3 Class B"}


NO_QC3PLUS = "no QC3+ response"   # the skip reason of a QC3+ group the charger does not do (Reason::NoQc3Plus)


def _v(mv):
    return f"{mv / 1000:g}"


def _group_key(s):
    # PPS/AVS rows are one sequence per APDO position, the Mi 6 A probe rows their own group; others group by protocol.
    pos = s["pos"] if s["protocol"] in ("pps", "spr-avs", "epr-avs") and s["pos"] else 0
    return (s["protocol"], pos, step_label.is_hunt(s))


def _retry(s):
    """'passed on retry N' for a PD row that took more than one attempt (the firmware counts the first attempt too), else ''."""
    n = s.get("attempts", 0) - 1
    pd_row = s["protocol"].startswith(("pd", "epr", "pps", "spr"))
    return f"passed on retry {n}" if s["result"] == "pass" and pd_row and n > 0 else ""


def _step_text(s, prev, amps=True):
    """One row of a group line: its step_label numbers ("5V 3A", "11→8V") and the verdict. `prev` = the plan row before it."""
    # A Mi 6 A probe row carries its protocol word ("Mi 20V 6A"): the group has no heading of its own.
    label = step_label.label(s, prev) if step_label.is_hunt(s) else step_label.params(s, prev, amps=amps)
    if s["result"] == "pass":
        return (f"{label} ✓ handshake ok" if s["detail_reason"] == "handshake ok" else
                f"{label} ✓ detected" if s["detail_reason"].startswith("detected") else
                f"{label} ✓ {_retry(s)}".rstrip())
    if s["result"] == "fail":
        why = s["detail_reason"] if s["detail_reason"] != "none" else s["reason"]
        at = f" at {s['measured_mv'] / 1000:.2f} V" if s["measured_mv"] else ""
        # A refused 6 A row: the firmware's classifier names the cause (k2_remote.attach_hunt_evidence).
        hunt = f" [{s['hunt']['cause']}: {s['hunt']['explanation']}]" if "hunt" in s else ""
        return f"{label} ✗ {why}{at}{hunt}"
    # A 6 A row the source did not offer: the firmware's "not unlocked" skip says which link of the OEM chain was missing.
    return f"{label} skipped: {_not_unlocked(s)}" if s["reason"] == "not unlocked" else f"{label} skipped"


def _not_unlocked(s):
    return s["detail_reason"] if s["detail_reason"] != "none" else s["reason"]


def summarize(r):
    steps = r["steps"]
    ev = r.get("evidence", {})
    n = {k: sum(s["result"] == k for s in steps) for k in ("pass", "fail", "skip")}
    cable = ev.get("physical_cable", {})
    offered_ma = max((step_label.pdo_ma(s.get("raw_pdo", 0)) for s in steps), default=0)
    limits = f"{_v(cable.get('max_mv', 0))} V/{cable.get('max_ma', 0) / 1000:g} A"
    if cable.get("observed"):
        cable_txt = limits
    elif cable.get("max_ma", 0) > 3000 and cable.get("max_mv", 0) <= 20000 and offered_ma > 3000:
        # The firmware assumed a 5 A cable because the source offers more than 3 A; the header names the real
        # highest offer (the steps' PDOs), never the assumed limit.
        cable_txt = f"not read; source offers {offered_ma / 1000:g} A"
    else:
        cable_txt = f"not read, default limit {limits}"
    # `cable_identity` = k2_remote.charger_cable() read after the run: the identity the START dialog's Ra mode presented.
    ident = r.get("cable_identity") or {}
    spoof = f" · SPOOF: {ident.get('run_name') or ident['run']}" if ident.get("run") not in (None, "off") else \
        " · SPOOF ON" if ev.get("spoof_on") else ""
    out = [f"Run {r['run_id']} · {ev.get('wire_revision', '?')} · {ev.get('pdo_count', '?')} PDOs · cable {cable_txt}" + spoof,
           f"{n['pass']} pass, {n['fail']} fail, {n['skip']} skip · {r['state']}"
           + (f" ({r['reason']})" if r.get("reason") not in (None, "none") else ""),
           "Scope: negotiation and measured voltage only, no load", ""]

    prev_of = {id(s): steps[i - 1] if i else None for i, s in enumerate(steps)}
    groups, order = {}, []
    for s in steps:
        k = _group_key(s)
        if k not in groups:
            groups[k] = []
            order.append(k)
        groups[k].append(s)

    not_adv, skipped = [], {}
    for k in order:
        rows = groups[k]
        name = PROTO_NAMES.get(k[0], k[0])
        if k[2] and all(s["result"] == "skip" for s in rows):
            # Mi 6 A rows skipped: each by its own cause, named like the rows of a run ("Mi 20V 6A").
            for s in rows:
                skipped.setdefault(_not_unlocked(s) if s["reason"] == "not unlocked" else s["reason"], []).append(
                    step_label.label(s, prev_of[id(s)]))
            continue
        if any(s["result"] == "skip" and s["reason"] == NO_QC3PLUS for s in rows):
            # The charger does not do QC3+ (no movement, or QC3-sized steps): one neutral line; the 5 V baseline that
            # passed is not partial support, so it is a parenthesis and no ✓ row. The totals above still count it.
            base = " (5V baseline ✓)" if rows[0]["result"] == "pass" else ""
            out.append(f"- {name}: no QC3+{base}")
            continue
        if all(s["result"] == "skip" for s in rows):
            why = rows[0]["reason"]
            if why == "not advertised":
                not_adv.append(name)
            else:
                skipped.setdefault(why, []).append(name)
            continue
        # The heading is the step_label protocol word; a PPS / AVS offer adds its range and current, so its rows drop the amps.
        name = step_label.protocol(rows[0])
        # A Mi 6 A probe group shows its own request, not the APDO it rides on (whose advertised current it exceeds):
        # every row reads "Mi 20V 6A", with no heading.
        hunt = k[2]
        rng = step_label.pdo_range(rows[0]["raw_pdo"]) if k[1] and not hunt else ""
        if rng:
            name += f" {rng}"
        amps = not rng
        head = "" if hunt else f"{name}: "
        text = (lambda s, prev=None, amps=True: step_label.label(s, prev)) if hunt else \
            (lambda s, prev=None, amps=True: step_label.params(s, prev, amps=amps))
        if all(s["result"] == "pass" for s in rows) and not any(s["continuous"] for s in rows):
            lo, hi = min(s["measured_mv"] for s in rows), max(s["measured_mv"] for s in rows)
            retried = "".join(f"; {text(s, amps=amps)} {_retry(s)}" for s in rows if _retry(s))
            out.append(f"- {head}{', '.join(text(s, prev_of[id(s)], amps) for s in rows)}, all pass "
                       f"({lo / 1000:.2f}–{hi / 1000:.2f} V measured{retried})")
            continue
        out.append(f"- {head}" + ", ".join(_step_text(s, prev_of[id(s)], amps) for s in rows))
    for why, names in skipped.items():
        out.append(f"- {', '.join(dict.fromkeys(names))}: skipped, {why}")
    if not_adv:
        out.append(f"- Not advertised: {', '.join(dict.fromkeys(not_adv))}")
    return "\n".join(out)


HANDSHAKE_MV = 5500   # a group whose highest passing voltage is 5 V got a handshake, not a charge level


def _group_text(e):
    """One group of a card: the ceiling that passed, then the first failing target and its cause."""
    mx, fail = e.get("max_mv", 0), e.get("fail_mv", 0)
    ok = (f"5 V only" if 0 < mx <= HANDSHAKE_MV else f"✓ up to {_v(mx)} V" if mx else "✓" if e["verdict"] == "pass" else "")
    if e["verdict"] == "pass" and e.get("why") in ("detected", "handshake ok"):
        return "✓ " + e["why"]
    if e["verdict"] == "pass":
        if ok.startswith("✓ up to") and e.get("max_ma"):
            ok += f", {_v(e['max_ma'])} A"
        return f"✓ {ok}" if ok == "5 V only" else ok
    why = e.get("why") or e.get("reason", "")
    bad = f"✗ {_v(fail)} V" + (f" ({why})" if why else "") if fail else f"✗ {why}".rstrip()
    return f"{ok}, {bad}" if ok else bad


# A card's cable kind: a built-in cable's id (k2::emrec::builtin_info labels), 0 = a saved clone, 0xFF = a real cable.
# The dialog lists ids 1, 2 and 4; id 0 (generic 20 V 5 A) is remote-only. Id 3 (the retired "OnePlus 100W (mag)")
# is the same identity as 4, so an older card with kind 3 reads as OnePlus 100W.
CARD_BUILTIN_LABELS = {1: "EPR 50V 5A", 2: "CUKTECH 6A", 3: "OnePlus 100W", 4: "OnePlus 100W"}


def _card_spoof(cable):
    """The header suffix of a card whose run presented a spoofed e-marker (the card keeps kind and VID:PID, not the name)."""
    if not cable:
        return ""
    own = " · self-powered" if cable.get("self_powered") else ""   # the charger under test powered the K2 for the whole run
    if not cable.get("spoofed"):
        return own
    if cable.get("kind") in CARD_BUILTIN_LABELS:
        return f" · SPOOF: {CARD_BUILTIN_LABELS[cable['kind']]}" + own
    return f" · SPOOF: clone {cable['vid']:04X}:{cable['pid']:04X}" + own


def summarize_card(c):
    """One report card in the run-summary format: a header, a verdict line, then one line per group."""
    g = c["groups"]
    n = {k: sum(e["verdict"] == k for e in g.values()) for k in ("pass", "fail", "skip")}
    out = [f"Card {c['no']} · {c['name'] or '(unnamed)'} · {c['pdos']} PDOs · {c['runs']} runs · "
           f"{c.get('brand', '')} {c['vid']:04X}:{c['pid']:04X}".strip()
           + (f" · {c['pdp_w']} W" if c.get("pdp_w") else "") + f" · fw {c['fw']}" + _card_spoof(c.get("cable")),
           f"{n['pass']} pass, {n['fail']} fail, {n['skip']} skip of {len(g)} groups · {c['state']}"
           + (f" ({c['reason']})" if c["reason"] else ""),
           f"Identity {c['fp']} · boot {c['boots']} · uptime {c['uptime_s']} s"
           + (f" · same result x{c['times']}, last boot {c['last_boots']} uptime {c['last_uptime_s']} s" if c.get("times", 1) > 1 else "")
           + (f" · source rebalanced x{c['rebalanced']}" if c.get("rebalanced") else "")]   # v8 card field (report_card_json.cpp puts the same suffix here)
    if c["handover"] != "not tried":
        out.append(f"Handover: {c['handover']} after {c['handover_ms'] / 1000:g} s")
    out.append("")
    skipped = {}
    for name, e in g.items():
        if e["verdict"] in ("pass", "fail"):
            out.append(f"- {name}: " + _group_text(e))
        elif e.get("reason") == NO_QC3PLUS:
            out.append(f"- {name}: no QC3+")   # not folded into the "skipped" list; same words as the firmware's card text
        else:
            skipped.setdefault(e.get("reason", ""), []).append(name)
    for why, names in skipped.items():
        out.append(f"- {', '.join(names)}: skipped" + (f", {why}" if why else ""))
    for r in c["ranges"]:
        rng = f"- {step_label.range_protocol(r['alg'])} {step_label.range_text(r['lo_mv'], r['hi_mv'], r.get('max_ma', 0))}: "
        spread = _spread_text(r)   # v9 card: the setpoint error per passed level, e.g. ", err +10 / +20 / +8 / -12 mV"
        # v10 card: the deep-sweep summary of the APDO, same words as the results page ("sweep peak 21.0 V at 21.0 V, ...").
        sweep = f", sweep {report_card.sweep_text(r['sweep'])}" if r.get("sweep") else ""
        if r["fail_mv"]:
            out.append(rng + f"✗ at {_v(r['fail_mv'])} V" + (f", reached {_v(r['max_mv'])} V" if r["max_mv"] else "") + spread + sweep)
        elif r["max_mv"] >= r["hi_mv"]:
            out.append(rng + "✓ whole range" + spread + sweep)
        else:
            out.append(rng + (f"reached {_v(r['max_mv'])} V" if r["max_mv"] else "no step measured") + spread + sweep)
    return "\n".join(out)


def _spread_text(r):
    """The v9 spread block of a card range as ', err +10 / +20 / +8 / -12 mV' (ranges[i].err_mv in report_card.py /
    k2proto.js; absent on older cards and when no level was measured)."""
    errs = r.get("err_mv")
    if not errs:
        return ""
    return ", err " + " / ".join(f"{e:+d}" for e in errs) + " mV"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print(summarize(json.load(open(sys.argv[1]))))
