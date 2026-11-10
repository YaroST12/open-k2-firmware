# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import charger_summary  # noqa: E402

RUN3 = json.loads((Path(__file__).parent / "fixtures" / "charger_run3_baseus.json").read_text())


def test_run3_compact_lines():
    text = charger_summary.summarize(RUN3)
    assert "11 pass, 2 fail, 19 skip" in text
    assert "- PD: 5V 3A, 9V 3A, 12V 3A, 15V 3A, 20V 3.35A, all pass" in text
    assert "- PPS 5-11V 5A: 5V ✓, 5→8V ✓, 8→11V ✓, 11→8V ✗ aborted at 9.19 V, 8→5V skipped" in text
    assert "- PPS 5-21V 3.2A: 5V ✓, 5→13V ✓, 13→21V ✓, 21→13V ✗ aborted at 15.69 V, 13→5V skipped" in text
    assert "- QC2, QC3, QC3+: skipped, PD active: use QC port" in text
    assert "- Not advertised: EPR fixed, SPR AVS, EPR AVS" in text


def test_empty_run():
    assert "0 pass, 0 fail, 0 skip" in charger_summary.summarize(
        {"run_id": 0, "state": "idle", "reason": "none", "evidence": {}, "steps": []})


def _skip_run(protocols, reason="PD active: use QC port"):
    steps = [{"protocol": p, "pos": 0, "result": "skip", "reason": reason, "detail_reason": "none", "target_mv": 5000,
              "measured_mv": 0, "continuous": False, "raw_pdo": 0} for p in protocols]
    return {"run_id": 1, "state": "done", "reason": "none", "evidence": {}, "steps": steps}


def test_vendor_protocols_have_friendly_names():
    protos = ["vooc-5v-handshake", "svooc-5v-handshake", "svooc65-5v-handshake", "ufcs", "vivo"]
    text = charger_summary.summarize(_skip_run(protos))
    assert "- VOOC 5V, SVOOC, SVOOC 65W, UFCS, VIVO: skipped, PD active: use QC port" in text
    for raw in protos:
        assert raw not in text


def test_vendor_protocol_pass_rows_use_friendly_names():
    steps = [{"protocol": "vivo", "pos": 0, "result": "pass", "reason": "none", "detail_reason": "none",
              "target_mv": mv, "measured_mv": mv, "continuous": False, "raw_pdo": 0} for mv in (5000, 9000)]
    text = charger_summary.summarize({"run_id": 2, "state": "done", "reason": "none", "evidence": {}, "steps": steps})
    assert "- VIVO: 5V, 9V, all pass" in text


def test_card_summary():
    import report_card
    fix = Path(__file__).parent / "fixtures" / "report_card_sample.hex"
    text = charger_summary.summarize_card(report_card.decode(bytes.fromhex(fix.read_text().strip())))
    assert "Card 3 · Desk 65W · 5 PDOs · 2 runs · " in text.splitlines()[0]
    assert "1 pass, 2 fail, 1 skip of 4 groups · Complete" in text
    assert "Handover: fell back after 1.5 s" in text
    assert "- PD: ✓ up to 20 V" in text
    assert "- QC2: skipped, not advertised" in text
    assert "- AFC: ✓ up to 9 V, ✗ 12 V (no response)" in text
    assert "- SCP: ✗ no power monitor" in text      # no passing step, no failing target kept: the cause only
    assert "detail" not in text
    assert "- PPS 3.3-21V: ✗ at 15 V, reached 13 V" in text


def test_card_summary_folds_skips_and_lists_each_range():
    card = {"no": 1, "name": "", "pdos": 7, "runs": 1, "vid": 1, "pid": 2, "fw": "0", "state": "Complete", "reason": "",
            "fp": "1", "boots": 1, "uptime_s": 1, "handover": "not tried", "handover_ms": 0,
            "groups": {"PD": {"verdict": "pass", "max_mv": 19951},
                       "QC2": {"verdict": "skip", "reason": "PD active: use QC port"},
                       "QC3": {"verdict": "skip", "reason": "PD active: use QC port"}},
            "ranges": [{"alg": "PPS (PD3.x)", "lo_mv": 5000, "hi_mv": 11000, "max_mv": 11000, "fail_mv": 0},
                       {"alg": "PPS (PD3.x)", "lo_mv": 5000, "hi_mv": 21000, "max_mv": 21000, "fail_mv": 0}]}
    text = charger_summary.summarize_card(card)
    assert "- PD: ✓ up to 19.951 V" in text
    assert "- QC2, QC3: skipped, PD active: use QC port" in text
    assert "- PPS 5-11V: ✓ whole range" in text
    assert "- PPS 5-21V: ✓ whole range" in text


def _card(groups):
    return {"no": 1, "name": "Baseus", "pdos": 7, "runs": 1, "vid": 1, "pid": 2, "fw": "0", "state": "Complete", "reason": "",
            "fp": "1", "boots": 1, "uptime_s": 1, "handover": "not tried", "handover_ms": 0, "groups": groups, "ranges": []}


def test_card_summary_names_the_passing_ceiling_and_the_first_failure():
    # The clean Baseus run: QC2 5/9/12 pass and 20 fails, AFC 9 passes and 12 fails, VIVO only 5 passes.
    text = charger_summary.summarize_card(_card({
        "QC2": {"verdict": "fail", "max_mv": 12000, "fail_mv": 20000, "reason": "request failed", "detail": 8,
                "why": "voltage not reached"},
        "AFC": {"verdict": "fail", "max_mv": 9000, "fail_mv": 12000, "reason": "request failed", "detail": 7,
                "why": "no response"},
        "VIVO": {"verdict": "fail", "max_mv": 5000, "fail_mv": 9000, "reason": "request failed", "detail": 7,
                 "why": "no response"},
        "SCP": {"verdict": "fail", "fail_mv": 5000, "reason": "timeout"},
        "VOOC": {"verdict": "pass", "max_mv": 5000}}))
    assert "- QC2: ✓ up to 12 V, ✗ 20 V (voltage not reached)" in text
    assert "- AFC: ✓ up to 9 V, ✗ 12 V (no response)" in text
    assert "- VIVO: 5 V only, ✗ 9 V (no response)" in text
    assert "- SCP: ✗ 5 V (timeout)" in text            # a pure failure still names the target and a readable cause
    assert "- VOOC: ✓ 5 V only" in text
    assert "detail" not in text


def test_card_summary_of_a_card_without_failing_targets():
    # A card written before format 1 has the cause but no failing target.
    text = charger_summary.summarize_card(_card({"QC2": {"verdict": "fail", "max_mv": 12000, "reason": "request failed",
                                                          "detail": 8, "why": "voltage not reached"}}))
    assert "- QC2: ✓ up to 12 V, ✗ voltage not reached" in text


def test_run_header_names_the_cable_identity_of_the_dialog():
    run = {"run_id": 2, "state": "done", "reason": "none", "evidence": {"spoof_on": True}, "steps": []}
    assert "SPOOF ON" in charger_summary.summarize(run).splitlines()[0]          # armed by hand, identity unknown
    run["cable_identity"] = {"run": "builtin", "run_name": "EPR 50V 5A"}
    head = charger_summary.summarize(run).splitlines()[0]
    assert head.endswith(" · SPOOF: EPR 50V 5A") and "SPOOF ON" not in head
    run["cable_identity"] = {"run": "off", "run_name": ""}                        # the dialog said OFF
    assert "SPOOF" not in charger_summary.summarize({**run, "evidence": {}}).splitlines()[0]


def test_run_header_cable_wording_follows_the_source_offers():
    def head(cable):
        run = {"run_id": 1, "state": "done", "reason": "none", "evidence": {"physical_cable": cable}, "steps": []}
        return charger_summary.summarize(run).splitlines()[0]
    assert "cable not read, default limit 20 V/3 A" in head({"max_mv": 20000, "max_ma": 3000, "observed": 0})
    assert "cable not read; source offers 5 A" in head({"max_mv": 20000, "max_ma": 5000, "observed": 0})
    assert "cable 20 V/5 A" in head({"max_mv": 20000, "max_ma": 5000, "observed": 1})
    assert "default limit 50 V/5 A" in head({"max_mv": 50000, "max_ma": 5000, "observed": 0})   # EPR-proven, old wording


def test_card_header_names_the_spoofed_cable_identity():
    card = _card({"PD": {"verdict": "pass", "max_mv": 20000}})
    assert "SPOOF" not in charger_summary.summarize_card(card).splitlines()[0]
    card["cable"] = {"seq": 1, "vid": 0x2B01, "pid": 0x1234, "max_mv": 50000, "max_ma": 5000, "kind": 1, "observed": 0, "spoofed": 1}
    assert charger_summary.summarize_card(card).splitlines()[0].endswith(" · SPOOF: EPR 50V 5A")
    for kind, name in ((2, "CUKTECH 6A"), (3, "OnePlus 100W"), (4, "OnePlus 100W")):   # kind 3 = an older card, the retired mag id
        card["cable"]["kind"] = kind
        assert charger_summary.summarize_card(card).splitlines()[0].endswith(f" · SPOOF: {name}")
    card["cable"]["kind"] = 0
    assert charger_summary.summarize_card(card).splitlines()[0].endswith(" · SPOOF: clone 2B01:1234")
    card["cable"]["spoofed"] = 0                                                   # a physical e-marker only
    assert "SPOOF" not in charger_summary.summarize_card(card).splitlines()[0]


def test_card_header_says_when_the_charger_powered_the_k2():
    card = _card({"PD": {"verdict": "pass", "max_mv": 20000}})
    assert "self-powered" not in charger_summary.summarize_card(card).splitlines()[0]
    card["cable"] = {"seq": 0, "vid": 0, "pid": 0, "max_mv": 0, "max_ma": 0, "kind": 0, "observed": 0, "spoofed": 0}
    assert "self-powered" not in charger_summary.summarize_card(card).splitlines()[0]
    card["cable"]["self_powered"] = 1
    assert charger_summary.summarize_card(card).splitlines()[0].endswith(" · self-powered")
    card["cable"].update({"spoofed": 1, "kind": 1})                                 # both: the spoof suffix first
    assert charger_summary.summarize_card(card).splitlines()[0].endswith(" · SPOOF: EPR 50V 5A · self-powered")


def _epr_rows(attempts_36):
    rows = [{"protocol": "epr-fixed", "pos": 8, "result": "pass", "reason": "none", "detail_reason": "none",
             "target_mv": 28000, "measured_mv": 28000, "continuous": False, "raw_pdo": 0},
            {"protocol": "epr-fixed", "pos": 9, "result": "pass", "reason": "none", "detail_reason": "none",
             "target_mv": 36000, "measured_mv": 36010, "continuous": False, "raw_pdo": 0, "attempts": attempts_36}]
    return {"run_id": 7, "state": "done", "reason": "none", "evidence": {}, "steps": rows}


def test_a_row_that_passed_on_a_retry_says_so():
    # the firmware counts the first attempt too: 3 attempts = "retry 2" (pattern epr-entry-hard-reset)
    text = charger_summary.summarize(_epr_rows(3))
    assert "36V passed on retry 2" in text
    assert "28V passed on retry" not in text


def test_a_first_try_pass_has_no_retry_note():
    assert "retry" not in charger_summary.summarize(_epr_rows(0))
    assert "retry" not in charger_summary.summarize(_epr_rows(1))


def _mi_run(*rows):
    """Mi 6 A rows, each (result, reason, detail_reason, target_mv)."""
    steps = [{"result": res, "reason": why, "detail_reason": detail, "measured_mv": mv if res == "pass" else 0, "attempts": 1,
              "protocol": "pd3x-fixed", "pos": 5, "target_mv": mv, "request_ma": 6000, "raw_pdo": 0x641F4, "continuous": False}
             for res, why, detail, mv in rows]
    return {"run_id": 1, "state": "done", "reason": "none", "evidence": {}, "steps": steps}


def test_6a_rows_the_source_did_not_offer_read_not_unlocked():
    locked = ("skip", "not unlocked", "not unlocked (no OEM chain)")
    text = charger_summary.summarize(_mi_run(locked + (11000,), locked + (20000,)))
    assert "- Mi 11V 6A, Mi 20V 6A: skipped, not unlocked (no OEM chain)" in text
    assert "PPS" not in text


def test_a_mixed_mi_group_names_each_row_and_the_cause():
    text = charger_summary.summarize(_mi_run(("pass", "none", "none", 11000),
                                             ("skip", "not unlocked", "not unlocked (3 A cable)", 20000)))
    assert "- Mi 11V 6A ✓, Mi 20V 6A skipped: not unlocked (3 A cable)" in text


def test_a_spoofed_5a_identity_is_not_reported_as_a_3a_cable():
    # Spoofed Kowsi 5 A clone on a CUKTECH C1 (2026-11-07): the cause is the vendor not accepting the identity, never the cable.
    locked = ("skip", "not unlocked", "not unlocked (vendor not accepted)")
    text = charger_summary.summarize(_mi_run(locked + (11000,), locked + (20000,)))
    assert "- Mi 11V 6A, Mi 20V 6A: skipped, not unlocked (vendor not accepted)" in text
    assert "3 A cable" not in text


def test_a_refused_6a_row_shows_the_cause_the_firmware_classified():
    run = _mi_run(("fail", "request failed", "rejected", 20000))
    run["steps"][0]["hunt"] = {"cause": "apdo-stale", "explanation": "no caps after the auth"}
    assert "Mi 20V 6A ✗ rejected [apdo-stale: no caps after the auth]" in charger_summary.summarize(run)
    del run["steps"][0]["hunt"]
    assert "[apdo-stale" not in charger_summary.summarize(run)


def _qc3p_run(rows):
    """QC3+ rows, each (result, target_mv, reason)."""
    steps = [{"protocol": "qc3+", "pos": 0, "result": res, "reason": why, "detail_reason": "none", "target_mv": mv,
              "measured_mv": mv if res == "pass" else 0, "continuous": False, "raw_pdo": 0, "attempts": 1}
             for res, mv, why in rows]
    return {"run_id": 1, "state": "done", "reason": "none", "evidence": {}, "steps": steps}


def test_a_charger_without_qc3plus_reads_no_qc3plus_not_a_5v_pass():
    # Blade HD: the 5 V baseline passed, the raised rows are skipped "no QC3+ response" (QC3-sized steps / no movement)
    nq = "no QC3+ response"
    text = charger_summary.summarize(_qc3p_run([("pass", 5000, "none"), ("skip", 9000, nq), ("skip", 12000, nq),
                                                ("skip", 9000, nq), ("skip", 5000, nq)]))
    assert "- QC3+: no QC3+ (5V baseline ✓)" in text
    assert "QC3+: 5V ✓" not in text
    assert "skipped" not in text


def test_a_qc3plus_group_that_really_ran_is_unchanged():
    ok = charger_summary.summarize(_qc3p_run([("pass", 5000, "none"), ("pass", 9000, "none"), ("pass", 12000, "none")]))
    assert "- QC3+: 5V, 9V, 12V, all pass" in ok
    bad = charger_summary.summarize(_qc3p_run([("pass", 5000, "none"), ("fail", 9000, "voltage"), ("skip", 12000, "voltage")]))
    assert "- QC3+: 5V ✓, 9V ✗ voltage, 12V skipped" in bad and "no QC3+" not in bad


def test_card_summary_reads_a_qc3plus_skip_as_no_qc3plus():
    text = charger_summary.summarize_card(_card({"PD": {"verdict": "pass", "max_mv": 20000},
                                                 "QC3+": {"verdict": "skip", "reason": "no QC3+ response", "why": "no QC3+"}}))
    assert "- QC3+: no QC3+" in text
    assert "skipped" not in text


class CardSweepSummary(unittest.TestCase):
    """`cards show` must print a v10 card's deep-sweep summary on the range line (ranges[i].sweep, decoded by
    tools/report_card.py), in the same words the results page uses ("sweep peak ... at ..., folds back to ...")."""

    def test_a_range_with_a_sweep_block_prints_the_sweep_summary(self):
        card = _card({})
        card["ranges"] = [
            {"alg": "PPS (PD3.x)", "lo_mv": 3300, "hi_mv": 21000, "max_mv": 21000, "fail_mv": 0,
             "sweep": {"min_mv": -40, "max_mv": 25, "worst_mv": -40, "worst_at_mv": 20000,
                       "peak_mv": 21000, "peak_at_mv": 21000, "fold_mv": 19420, "fold_from_mv": 20000}},
            {"alg": "PPS (PD3.x)", "lo_mv": 3300, "hi_mv": 11000, "max_mv": 11000, "fail_mv": 0},
        ]
        text = charger_summary.summarize_card(card)
        self.assertIn("- PPS 3.3-21V: ✓ whole range, sweep peak 21.0 V at 21.0 V, folds back to 19.42 V from 20.0 V", text)
        self.assertIn("- PPS 3.3-11V: ✓ whole range", text)
        self.assertNotIn("sweep", text.splitlines()[-1])   # a range without a sweep block stays clean

    def test_a_failing_range_carries_the_sweep_after_its_verdict(self):
        card = _card({})
        card["ranges"] = [
            {"alg": "PPS (PD3.x)", "lo_mv": 3300, "hi_mv": 21000, "max_mv": 13000, "fail_mv": 15000,
             "sweep": {"min_mv": 0, "max_mv": 10, "worst_mv": -30, "worst_at_mv": 9000,
                       "peak_mv": 13000, "peak_at_mv": 13000}},
        ]
        text = charger_summary.summarize_card(card)
        self.assertIn("- PPS 3.3-21V: ✗ at 15 V, reached 13 V, sweep peak 13.0 V at 13.0 V", text)


class CardVerdictLineIsLabelled(unittest.TestCase):
    """Row B item 4 host side: a card's header counts group verdicts, the run summary counts steps -
    the card line says "of N groups" (mirrors report_card_json.cpp render_summary), so the two agree."""

    def test_verdict_line_says_of_groups(self):
        card = _card({"PD": {"verdict": "pass", "max_mv": 20000},
                      "QC2": {"verdict": "fail", "max_mv": 9000, "fail_mv": 12000, "reason": "request failed"},
                      "QC3": {"verdict": "skip", "reason": "not advertised"},
                      "AFC": {"verdict": "skip", "reason": "not advertised"}})
        text = charger_summary.summarize_card(card)
        self.assertIn("1 pass, 1 fail, 2 skip of 4 groups · Complete", text)
