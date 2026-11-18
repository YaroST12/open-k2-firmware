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

import oem_watts
import pd_version
import report_card
import step_label

PROTO_NAMES = {"pd2-fixed": "PD2 fixed", "pd3x-fixed": "PD fixed", "epr-fixed": "EPR fixed", "spr-avs": "SPR AVS",
               "epr-avs": "EPR AVS", "pps": "PPS", "qc2": "QC2", "qc3": "QC3", "qc3+": "QC3+", "afc": "AFC",
               "fcp": "FCP", "scp": "SCP", "vooc-5v-handshake": "VOOC", "svooc-5v-handshake": "VOOC",
               "svooc65-5v-handshake": "VOOC", "ufcs": "UFCS", "vivo": "VIVO",
               "qc2b": "QC2 Class B", "qc3b": "QC3 Class B"}


NO_QC3PLUS = "no QC3+ response"   # the skip reason of a QC3+ group the charger does not do (Reason::NoQc3Plus)
# A Light run's rows above 5 V: a level the source lists (Reason::Advertised) or one nothing lists (Reason::LightLevel). They
# are not skips of a chosen test: the counts name them apart and the rows say "advertised" / "not asked".
ADVERTISED, NOT_ASKED = "advertised", "light run: not asked"
NOTED = (ADVERTISED, NOT_ASKED)
PORT_CUTS = "port cuts without load"   # Reason::PortCuts: the step lost VBUS to the port's own cut twice, so it is no verdict
FOLDED = "output folds back"   # Reason::Folded: a pass on the folded side of a voltage fold; a caution, neither pass nor fail
STEP_RESOLUTION, PROG_CHECK = "step resolution", "transition check"   # Reason::StepResolution / ProgCheck: more cautions on a pass
CAUTIONS = (FOLDED, STEP_RESOLUTION, PROG_CHECK)
WALK_STEP_MV = {"pps": 20}   # charger_test::walk_step_mv: a PPS walk moves in 20 mV, an AVS walk in 100 mV
# The VOOC family's units: one shared detection, so the summary reads them as one "VOOC" line and the family's levels in watts
# (oem_watts.LEVELS), never as three protocol versions. The card names its groups VOOC / SVOOC / SVOOC65 (report_card.GROUPS).
VOOC_FAMILY = ("vooc-5v-handshake", "svooc-5v-handshake", "svooc65-5v-handshake")
VOOC_CARD_GROUPS = ("VOOC", "SVOOC", "SVOOC65")


def _v(mv):
    return f"{mv / 1000:g}"


def _group_key(s):
    # PPS/AVS rows are one sequence per APDO position; the Xiaomi unit (its chain row and the modes the chain unlocked, or an
    # older firmware's 6 A probe rows) is one group; others group by protocol.
    if step_label.is_hunt(s):
        return ("xiaomi", 0, True)
    pos = s["pos"] if s["protocol"] in ("pps", "spr-avs", "epr-avs") and s["pos"] else 0
    return (s["protocol"], pos, step_label.is_hunt(s))


def _run_level(r):
    """The level the run ran at: ct_start's "level", else the page's selection.run_level (run_selected bits 28..29)."""
    return r.get("level") or (r.get("selection") or {}).get("run_level") or "medium"


def _caution(s):
    """True for a step the run marked a caution: a pass whose reason is a fold, a step walk off its quantum or a transition flag."""
    return s["result"] == "pass" and s["reason"] in CAUTIONS


def _caution_words(s):
    """What the caution says, as charger_test::caution_text does on the device ("steps 100mV, not 20mV", "overshoot, slow rise")."""
    if s["reason"] == STEP_RESOLUTION:
        mv = s["detail"]
        mv = (mv + 5) // 10 * 10 if mv >= 50 else mv
        return f"steps {mv}mV, not {WALK_STEP_MV.get(s['protocol'], 100)}mV" if mv else "steps ignored"
    return s["detail_reason"] if s["detail_reason"] != "none" else s["reason"]


def _retry(s):
    """'passed on retry N' for a PD row that took more than one attempt (the firmware counts the first attempt too), else ''."""
    n = s.get("attempts", 0) - 1
    pd_row = s["protocol"].startswith(("pd", "epr", "pps", "spr"))
    return f"passed on retry {n}" if s["result"] == "pass" and pd_row and n > 0 else ""


def _step_text(s, prev, amps=True):
    """One row of a group line: its step_label numbers ("5V 3A", "11→8V") and the verdict. `prev` = the plan row before it."""
    # A Xiaomi mode row carries its protocol word and watts ("Mi 55W"): the group has no heading of its own.
    label = step_label.label(s, prev) if step_label.is_hunt(s) else _params(s, prev, amps)
    if _caution(s) and s["reason"] != FOLDED:
        return f"{label} ⚠ {_caution_words(s)}"
    if _caution(s):   # a fixed row measured the folded level; a sweep row has no single level of its own
        at = "" if s.get("sweep") else f" to {report_card.dv_text(s['measured_mv'])}"
        return f"{label} ⚠ folds{at}" if at else f"{label} ⚠ folds back"
    if s["result"] == "pass":
        return (f"{label} ✓ handshake ok" if s["detail_reason"] == "handshake ok" else
                f"{label} ✓ detected" if s["detail_reason"].startswith("detected") else
                f"{label} ✓ {_retry(s)}".rstrip())
    if s["result"] == "fail":
        why = s["detail_reason"] if s["detail_reason"] != "none" else s["reason"]
        at = f" at {s['measured_mv'] / 1000:.2f} V" if s["measured_mv"] else ""
        # An older firmware's refused 6 A row: its classifier names the cause (k2_remote.attach_hunt_evidence).
        hunt = f" [{s['hunt']['cause']}: {s['hunt']['explanation']}]" if "hunt" in s else ""
        return f"{label} ✗ {why}{at}{hunt}"
    if s["reason"] in NOTED:
        return f"{label} advertised" if s["reason"] == ADVERTISED else f"{label} not asked"
    # An older firmware's 6 A row the source did not offer: its "not unlocked" skip says which link of the OEM chain was missing.
    # A Xiaomi mode skips for a reason worth naming (the cable limit, a light run).
    return f"{label} skipped: {_not_unlocked(s)}" if s["reason"] == "not unlocked" else \
        f"{label} skipped: {s['reason']}" if s["reason"] == PORT_CUTS or (step_label.is_hunt(s) and s["reason"] != "none") \
        else f"{label} skipped"


def _xiaomi_line(rows, prev_of):
    """The Xiaomi unit in one line, as the grid shows it: the modes the auth chain unlocked with their verdicts ("- Xiaomi: Mi 55W ✓");
    none unlocked: "- Xiaomi: auth ✓"; the chain did not run or failed: "- Xiaomi: auth skipped (no answer)"."""
    chain = next((s for s in rows if not s.get("target_mv")), None)
    modes = ", ".join(_step_text(s, prev_of[id(s)]) for s in rows if s.get("target_mv"))
    if chain is None:   # an older firmware: its 6 A probe rows, no chain row
        return f"- Xiaomi: {modes}"
    code = chain.get("detail", 0)
    if modes:
        return f"- Xiaomi: {modes}"
    if step_label.mi_chain_done(code):
        return "- Xiaomi: auth ✓"
    return f"- Xiaomi: auth skipped ({step_label.mi_chain_text(code)})"


def _params(s, prev=None, amps=True):
    """A row's numbers: SCP / UFCS the level's volts and claimed watts ("10V 22.5W", oem_watts.chip), else step_label's."""
    return oem_watts.chip(s) or step_label.params(s, prev, amps=amps)


def _vooc_levels_line(sv):
    """The VOOC family's levels by their watts, each claimed / not reached / not tested by the Deep ceiling stage (sv: the
    k2_remote.charger_svooc_cap readout or a card's Deep block; None = every level not tested)."""
    return ("- VOOC levels: " + ", ".join(f"{lbl} {state}" for lbl, state in oem_watts.vooc_levels(sv))
            + " (claimed = the protocol's ask, no load)")


def _vooc_family_lines(rows, sv, skipped, not_adv):
    """The VOOC family's step rows as one "VOOC" line plus its levels line when it answered; a family that never answered
    folds into the skip / not-advertised lists as one word."""
    if not any(s["result"] == "pass" for s in rows):
        if all(s["result"] == "skip" for s in rows):
            why = _skip_why(rows[0])
            (not_adv if why == "not advertised" else skipped.setdefault(why, [])).append("VOOC")
            return []
        return ["- VOOC: " + _step_text(next(s for s in rows if s["result"] != "skip"), None)]
    hit = next(s for s in rows if s["result"] == "pass")
    word = "handshake ok" if hit["detail_reason"] == "handshake ok" else "detected"
    return [f"- VOOC: ✓ {word}", _vooc_levels_line(sv)]


def _skip_why(s):
    """Why a skipped row did not run: the engine's words when the bare reason is "request failed" (a sibling skipped after
    its unit's first row failed carries that row's detail), else the reason."""
    if s["reason"] == "request failed" and s.get("detail_reason", "none") != "none":
        return s["detail_reason"]
    return s["reason"]


def _not_unlocked(s):
    return s["detail_reason"] if s["detail_reason"] != "none" else s["reason"]


def summarize(r):
    steps = r["steps"]
    ev = r.get("evidence", {})
    n = {k: sum(s["result"] == k for s in steps) for k in ("pass", "fail", "skip")}
    caution = sum(_caution(s) for s in steps)   # a voltage fold: neither a pass nor a failure (the device counts it with "capped")
    n["pass"] -= caution
    noted = sum(s["result"] == "skip" and s["reason"] in NOTED for s in steps)
    n["skip"] -= noted
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
    ver = pd_version.from_evidence(ev)   # the offer's version, or the Revision answer when it is as high (see pd_version.py)
    ver_txt = pd_version.label(ver) + (f" ({pd_version.conflict_text(ver)})" if ver and ver[3] else "")
    out = [f"Run {r['run_id']} · " + (f"{ver_txt} · " if ver else "") + f"{ev.get('pdo_count', '?')} PDOs · cable {cable_txt}" + spoof,
           f"{n['pass']} pass" + (f", {caution} caution" if caution else "") + f", {n['fail']} fail, {n['skip']} skip"
           + (f", {noted} not asked (Light run)" if noted else "")
           + f" · {r['state']}"
           + (f" ({r['reason']})" if r.get("reason") not in (None, "none") else ""),
           "Scope: Light run, nothing asked above 5 V, no load" if _run_level(r) == "light" else
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
    vooc_keys = [k for k in order if k[0] in VOOC_FAMILY]
    for k in order:
        rows = groups[k]
        if k[0] in VOOC_FAMILY:   # the family's block stands where the plan first runs one of its units
            if k == vooc_keys[0]:
                out += _vooc_family_lines([s for fk in vooc_keys for s in groups[fk]], r.get("svooc"), skipped, not_adv)
            continue
        name = PROTO_NAMES.get(k[0], k[0])
        if k[2]:
            out.append(_xiaomi_line(rows, prev_of))
            continue
        if any(s["result"] == "skip" and s["reason"] == NO_QC3PLUS for s in rows):
            # The charger does not do QC3+ (no movement, or QC3-sized steps): one neutral line; the 5 V baseline that
            # passed is not partial support, so it is a parenthesis and no ✓ row. The totals above still count it.
            base = " (5V baseline ✓)" if rows[0]["result"] == "pass" else ""
            out.append(f"- {name}: no QC3+{base}")
            continue
        if all(s["result"] == "skip" for s in rows):
            why = _skip_why(rows[0])
            if all(s["reason"] in NOTED for s in rows):   # a Light run's listing, not a skip
                rng = step_label.pdo_range(rows[0]["raw_pdo"]) if k[1] else ""
                head = step_label.protocol(rows[0]) + (f" {rng}" if rng else "")
                out.append(f"- {head}: " + ("advertised, not asked" if why == ADVERTISED else "not asked (Light run)"))
                continue
            if why == "not advertised":
                not_adv.append(name)
            else:
                skipped.setdefault(why, []).append(name)
            continue
        # The heading is the step_label protocol word; a PPS / AVS offer adds its range and current, so its rows drop the amps.
        name = step_label.protocol(rows[0])
        rng = step_label.pdo_range(rows[0]["raw_pdo"]) if k[1] else ""
        if rng:
            name += f" {rng}"
        amps = not rng
        head = f"{name}: "
        text = (lambda s, prev=None, amps=True: _params(s, prev, amps))
        if all(s["result"] == "pass" for s in rows) and not any(s["continuous"] or _caution(s) for s in rows):
            lo, hi = min(s["measured_mv"] for s in rows), max(s["measured_mv"] for s in rows)
            retried = "".join(f"; {text(s, amps=amps)} {_retry(s)}" for s in rows if _retry(s))
            out.append(f"- {head}{', '.join(text(s, prev_of[id(s)], amps) for s in rows)}, all pass "
                       f"({lo / 1000:.2f}–{hi / 1000:.2f} V measured{retried})")
            continue
        out.append(f"- {head}" + ", ".join(_step_text(s, prev_of[id(s)], amps) for s in rows))
    out += _svooc_lines(r.get("svooc"))
    out += _spec_lines(r.get("spec"))
    for why, names in skipped.items():
        out.append(f"- {', '.join(dict.fromkeys(names))}: skipped, {why}")
    if not_adv:
        out.append(f"- Not advertised: {', '.join(dict.fromkeys(not_adv))}")
    cuts = ev.get("cut_retries", 0)
    if cuts:   # the source dropped VBUS for a moment at 5 V with no load; a step lost to a cut is asked again, up to 3 times (Reason::PortCuts)
        out.append(f"- Port cut at 5 V: {cuts} retr{'ies' if cuts != 1 else 'y'} of steps lost to a cut")
    return "\n".join(out)


def _spec_lines(sp):
    """The Deep run's PD spec section (pdspec_blob.decode_section): the tally and how the stage ended, then each failed or
    unverified check with its worst value, then the skips grouped by reason. Nothing when the run had no section."""
    if not sp:
        return []
    tl = sp["tally"]
    head = f"- PD spec: {tl['pass']} pass, {tl['fail']} fail, {tl['skip']} skip" + (f", {tl['unverified']} unverified" if tl["unverified"] else "")
    if tl["pending"]:
        head += f", {tl['pending']} not judged"
    if sp["end"] != "done":
        head += f" · {sp['end']}" + (f" ({sp['why']})" if sp.get("why") else "")
    out = [head]
    for c in sp["cells"]:
        kept = f" (limit {c['limit_text']}, {c['count']} measured)" if c.get("limit_text") else ""   # the detail form, when the device kept it
        if c["verdict"] == "fail":
            out.append(f"  - {c['label']}: FAIL, worst {c['value_text']}, {c['fails']} over{kept}")
            out += _spec_row_lines(c)
        elif c["verdict"] == "unverified":
            out.append(f"  - {c['label']}: unverified (a K2 figure, no spec limit), worst {c['value_text']}{kept}")
    skips = {}
    for c in sp["cells"]:
        if c["verdict"] == "skip":
            skips.setdefault(c["skip"], []).append(c["label"])
    out += [f"  - skipped, {why}: {', '.join(names)}" for why, names in skips.items()]
    if sp.get("unconfirmed"):
        out.append("  - WARNING: the end-of-run return to 5 V was not confirmed: unplug the charger")
    return out


def _spec_row_lines(c):
    """The failing transitions of a failed check (the K2's per-transition list, pdspec_blob.decode_rows), one line each: the transition
    in volts, the value and how far past the limit. Nothing for a check the device kept no rows for."""
    import pdspec_blob
    out = []
    for r in c.get("rows", []):
        if not r["fail"]:
            continue
        over = f", {r['failing_steps']} steps over" if "failing_steps" in r else (f", {r['margin'] / 1000:+.3f} V past" if c["unit"] == "mv" and r["margin"] is not None
                                                                                    else f", {r['margin']:+g} past" if r["margin"] is not None else "")
        out.append(f"      {pdspec_blob.row_name(r)}: {r['value_text']}{over}")
    return out


def _svooc_lines(sv):
    """ct_start's VOOC-family readout (k2_remote.charger_svooc_cap): the ident the detection heard (every level), and the
    ceiling stage's words when it ran (Deep). Nothing for a run without either, or firmware without the readout."""
    if not sv:
        return []
    out = [f"- VOOC family ident 0x{sv['ident']:02X}"] if sv.get("ident") else []
    if sv.get("end") not in (None, "not run"):
        out.append(f"- SVOOC ceiling: {sv['text']} (protocol read, no load)")
    return out


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
CARD_BUILTIN_LABELS = {1: "EPR 50V 5A", 2: "Xiaomi 6A", 3: "OnePlus 100W", 4: "OnePlus 100W"}


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
    light = c.get("level") == "light"   # a Light run measured 5 V only: PD is "✓ advertised", a legacy group "detected"
    out = [f"Card {c['no']} · {c['name'] or '(unnamed)'} · {c['pdos']} PDOs · {c['runs']} runs · "
           f"{c.get('brand', '')} {c['vid']:04X}:{c['pid']:04X}".strip()
           + (f" · {c['pdp_w']} W" if c.get("pdp_w") else "") + f" · fw {c['fw']}" + _card_spoof(c.get("cable")),
           f"{n['pass']} {'detected' if light else 'pass'}, {n['fail']} fail, {n['skip']} skip of {len(g)} groups · {c['state']}"
           + (f" ({c['reason']})" if c["reason"] else ""),
           f"Identity {c['fp']} · boot {c['boots']} · uptime {c['uptime_s']} s"
           + (f" · same result x{c['times']}, last boot {c['last_boots']} uptime {c['last_uptime_s']} s" if c.get("times", 1) > 1 else "")
           + (f" · source rebalanced x{c['rebalanced']}" if c.get("rebalanced") else "")   # v8 card field (report_card_json.cpp puts the same suffix here)
           + (f" · {c['level'].capitalize()} run" if c.get("level") else "")]   # the run's level, Light / Deep only (Medium has no key)
    if c["handover"] != "not tried":
        out.append(f"Handover: {c['handover']} after {c['handover_ms'] / 1000:g} s")
    out.append("")
    skipped = {}
    fam = [n for n in VOOC_CARD_GROUPS if n in g]
    for name, e in g.items():
        if name in VOOC_CARD_GROUPS:   # one line for the family (one shared detection), then its levels in watts
            if name != fam[0]:
                continue
            hit = next((g[n] for n in fam if g[n]["verdict"] == "pass"), None)
            if hit:
                out.append("- VOOC: " + ("detected" if light else _group_text(hit)))
                out.append(_vooc_levels_line((c.get("deep") or {}).get("svooc")))
                continue
            e = next((g[n] for n in fam if g[n]["verdict"] == "fail"), e)
            name = "VOOC"
        if light and e["verdict"] == "pass":
            out.append(f"- {name}: " + ("✓ advertised" if name == "PD" else "detected"))   # PD is listed by the source (green); a legacy group only answered
        elif e["verdict"] in ("pass", "fail"):
            out.append(f"- {name}: " + _group_text(e))
        elif e.get("reason") == NO_QC3PLUS:
            out.append(f"- {name}: no QC3+")   # not folded into the "skipped" list; same words as the firmware's card text
        else:
            skipped.setdefault(e.get("reason", ""), []).append(name)
    for why, names in skipped.items():
        out.append(f"- {', '.join(names)}: skipped" + (f", {why}" if why else ""))
    out += _xiaomi_card_lines((c.get("deep") or {}).get("xiaomi"))
    for r in c["ranges"]:
        rng = f"- {step_label.range_protocol(r['alg'])} {step_label.range_text(r['lo_mv'], r['hi_mv'], r.get('max_ma', 0))}: "
        spread = _spread_text(r)   # v9 card: the setpoint error per passed level, e.g. ", err +10 / +20 / +8 / -12 mV"
        # v10 card: the deep-sweep summary of the APDO, same words as the results page ("sweep peak 21.0 V at 21.0 V, ...").
        note = report_card.fold_text(r.get("sweep")) if not r["fail_mv"] else ""   # a fold on a range that failed nothing: a caution
        sweep = f", sweep {report_card.sweep_text(r['sweep'], with_fold=not note)}" if r.get("sweep") else ""
        if note:
            out.append(rng + f"⚠ {note}" + spread + sweep)
        elif r["fail_mv"]:
            out.append(rng + f"✗ at {_v(r['fail_mv'])} V" + (f", reached {_v(r['max_mv'])} V" if r["max_mv"] else "") + spread + sweep)
        elif r["max_mv"] >= r["hi_mv"]:
            out.append(rng + "✓ whole range" + spread + sweep)
        else:
            out.append(rng + (f"reached {_v(r['max_mv'])} V" if r["max_mv"] else "✓ advertised" if light else "no step measured")
                       + spread + sweep)
    return "\n".join(out)


def _xiaomi_card_lines(x):
    """The card's Xiaomi section (format 12 block): the chain outcome, then one line per mode it unlocked, named by its watts with the
    offer before and after the chain and its verdict ("  - Mi 55W: PPS 5-11V 3A → PPS 5-11V 5A ✓")."""
    if not x:
        return []
    if not x["done"]:
        return [f"- Xiaomi: {x['chain_text']}" if x["chain"] == 1 else f"- Xiaomi auth: {x['chain_text']}"]
    if not x["modes"]:
        return ["- Xiaomi auth: done, no new modes"]
    out = [f"- Xiaomi: auth {x['chain_text']}"]
    for m in x["modes"]:
        v = ("✓" if m["verdict"] == "pass" else f"✗ {m['reason']}" if m["verdict"] == "fail" else
             f"skipped: {m['reason']}" if m["verdict"] == "skip" and m["reason"] != "none" else m["verdict"])
        out.append(f"  - {m['label']}: {m['before']} → {m['after']} {v}")
    return out


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
