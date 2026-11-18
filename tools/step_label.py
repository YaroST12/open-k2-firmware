# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The words for one charger-test step: "PD 9V 3A", "PPS 11→8V", "AVS 15V", "Mi 55W", "QC3 12V".

Mirrors include/k2/chargertest/step_label.hpp (the firmware's Running / Steps rows, Results chips and card text); the golden
vectors tests/fixtures/step_labels.txt are checked against both, so the device, the host tools and the screenshots
word a step the same. A row here is the dict k2_remote.parse_charger_test_results() returns: protocol, pos,
target_mv, request_ma, continuous, avs, raw_pdo, mi. "mi" is the wire's Xiaomi-unit flag (the chain row and the modes
the 0x2717 chain unlocked), present only when set; a row without it (an older firmware, a saved export) falls back to the
old rule: a PD-family row that requests more than 5 A is a hunt row (the retired 6 A probe). A plan row requests at most
1 A, except the full-current PPS row (the last row of each PPS APDO: top voltage at the APDO's whole current, clamped to
the cable, so 5 A at most), which shows what it requests.
"""

PLAN_ROW_MA = 1000   # a plan row requests at most this, except the full-current PPS row (include/k2/chargertest/charger_test.hpp kPlanRowMa)
HUNT_MA = 5000       # without the wire flag (older firmware): above it the row is a hunt (Mi) row: the cable ceiling, the most a plan row can ask
SWEEP_DETAIL = 161   # Step.detail of a deep-sweep row (kDetailSweep): one row per APDO part, the points live in the sweep record
SWEEP_SPLIT_MV = 5000   # ctsweep::kSplitMv: a PPS sweep's part below it is the PdLow row, the part from it up the Pd row
_PD_FAMILY = ("pd2-fixed", "pd3x-fixed", "pps")
_REVISIONS = {"pd3.0-revision": "PD3.0 rev", "pd3.1-revision": "PD3.1 rev", "pd3.2-revision": "PD3.2 rev"}
# Legacy and vendor modes: the group word. The wire says "vooc-5v-handshake" for the VOOC family; the 5 V is the row's target.
_LEGACY = {"qc2": "QC2", "qc3": "QC3", "qc3+": "QC3+", "afc": "AFC", "fcp": "FCP", "scp": "SCP",
           "vooc": "VOOC", "svooc": "SVOOC", "svooc65": "SVOOC65", "ufcs": "UFCS", "vivo": "VIVO",
           "qc2b": "QC2B", "qc3b": "QC3B"}
_PROGRAMMABLE = ("pps", "spr-avs", "epr-avs")


def milli(v):
    """5000 -> '5', 3300 -> '3.3', 12140 -> '12.14' (volts or amps from milli-units, up to two decimals, truncated)."""
    frac = v % 1000 // 10
    return str(v // 1000) + (("." + f"{frac:02d}".rstrip("0")) if frac else "")


def is_hunt(s):
    if "mi" in s:
        return bool(s["mi"])
    return s["protocol"] in _PD_FAMILY and s.get("request_ma", 0) > HUNT_MA


def mi_watts(s):
    """The power level a Xiaomi mode row is named by ("Mi 55W"): volts asked times current asked, whole watts; 0 for any other
    row (step_label.hpp mi_watts)."""
    return (s.get("target_mv", 0) * s.get("request_ma", 0) + 500000) // 1000000 if is_hunt(s) else 0


def is_sweep(s):
    """The deep-sweep row: a PD programmable row whose detail is kDetailSweep (the marker survives the verdict)."""
    return s["protocol"] in _PROGRAMMABLE and s.get("detail") == SWEEP_DETAIL


def is_full_current(s):
    """The full-current PPS row: not a hunt, asks more than a plan row's 1 A (the APDO's whole current, cable-clamped)."""
    return s["protocol"] == "pps" and PLAN_ROW_MA < s.get("request_ma", 0) <= HUNT_MA and not is_hunt(s)


def _is_avs(s):
    return s["protocol"] in ("spr-avs", "epr-avs") or (s["protocol"] == "epr-fixed" and s.get("avs", False))


def protocol(s):
    """The protocol word: PD, PPS, EPR, AVS, Mi (a Xiaomi mode), Xiaomi (the chain row), QC3+, SVOOC, 'PD3.1 rev'; 'PD3.x' /
    'PD2.0' for a plan row with no offer."""
    p, has_v = s["protocol"], bool(s.get("target_mv"))
    if is_hunt(s):
        return "Mi" if has_v else "Xiaomi"
    if p == "pd2-fixed":
        return "PD" if has_v else "PD2.0"
    if p == "pd3x-fixed":
        return "PD" if has_v else "PD3.x"
    if p == "epr-fixed":
        return "AVS" if s.get("avs") else "EPR"
    if p in ("spr-avs", "epr-avs"):
        return "AVS"
    if p == "pps":
        return "PPS"
    if p in _REVISIONS:
        return _REVISIONS[p]
    return _LEGACY.get(p.split("-5v-handshake")[0], p)


def pdo_ma(raw):
    """The advertised current of a fixed or PPS PDO in mA (0 for anything else, or no PDO)."""
    if not raw:
        return 0
    kind = raw >> 30
    if kind == 0:
        return (raw & 0x3FF) * 10
    if kind == 3 and (raw >> 28) & 3 == 0:
        return (raw & 0x7F) * 50
    return 0


def amps_ma(s):
    """The current the label shows, in mA (0 = none): the offer's for a PD fixed / PPS row (none for a Xiaomi mode row: its watts), SCP's
    limit, and the requested one for the full-current PPS row."""
    if is_hunt(s):
        return 0   # a Xiaomi mode is named by its watts (mi_watts)
    if is_full_current(s):
        return s.get("request_ma", 0)
    if s["protocol"] in ("pd2-fixed", "pd3x-fixed", "pps", "epr-fixed"):
        return 0 if _is_avs(s) else pdo_ma(s.get("raw_pdo", 0))
    return s.get("request_ma", 0) if s["protocol"] == "scp" else 0


def params(s, prev=None, amps=True):
    """'20V 5A', '12V', '11→8V' ('' with no voltage). `prev` = the plan row before this one: a continuous row of the same
    protocol and PDO position reads from→to. amps=False drops the current (a heading that already states it), except
    on the full-current PPS row: its current is what it requested, which the heading's advertised one does not say."""
    mv = s.get("target_mv", 0)
    if not mv:
        return ""
    if (s.get("continuous") and prev and s["protocol"] in _PROGRAMMABLE and prev["protocol"] == s["protocol"]
            and prev.get("pos") == s.get("pos") and not is_hunt(prev) and not is_hunt(s)
            and prev.get("target_mv") and prev["target_mv"] != mv):
        return f"{milli(prev['target_mv'])}→{milli(mv)}V"
    if is_hunt(s):
        return f"{mi_watts(s)}W"
    out = milli(mv) + "V"
    ma = amps_ma(s) if amps or is_full_current(s) else 0
    return out + (f" {milli(ma)}A" if ma else "")


def label(s, prev=None):
    """protocol + ' ' + params ('PPS 20V 5A'), the bare protocol when there are no params. A deep-sweep row names the
    range it sweeps, not its first point: 'PPS 5-21V', 'AVS 9-20V'. A PPS sweep is split at 5 V (SWEEP_SPLIT_MV): the row
    below it (target under 5 V) reads 'PPS 3.3-4.9V'."""
    if is_sweep(s):
        span = pdo_range(s.get("raw_pdo", 0)).split(" ")[0]   # '3.3-21V' (the current stays out of the badge)
        lo, _, hi = span[:-1].partition("-") if span else ("", "", "")
        if lo and hi:
            lo_mv, hi_mv = round(float(lo) * 1000), round(float(hi) * 1000)
            low = 0 < s.get("target_mv", 0) < SWEEP_SPLIT_MV
            p_lo, p_hi = (lo_mv, min(hi_mv, SWEEP_SPLIT_MV - 100)) if low else (max(lo_mv, SWEEP_SPLIT_MV), hi_mv)
            if p_lo <= p_hi:
                span = f"{milli(p_lo)}-{milli(p_hi)}V"
        return protocol(s) + (" " + span if span else "")
    p = params(s, prev)
    return protocol(s) + (" " + p if p else "")


def mi_chain_text(code):
    """How the Xiaomi 0x2717 auth chain ended, from the chain row's detail (mi_unlock::chain_text in the firmware)."""
    if code == 1:
        return "no 0x2717 SVID"
    if code == 2:
        return "no answer"
    if 4 <= code <= 13:
        return f"failed at step {code - 3}"
    return {20: "done, verified", 21: "done via 020A"}.get(code, f"unknown ({code})")


PROG_FLAGS = ("overshoot", "slow rise", "transient", "level off")   # progcheck::Flag bits, lowest first


def prog_check_text(flags):
    """The flags of a transition-check row (Reason::ProgCheck; Step.detail holds the bits), joined."""
    return ", ".join(n for i, n in enumerate(PROG_FLAGS) if flags >> i & 1)


def mi_chain_done(code):
    return code in (20, 21)


def labels(steps):
    """The labels of a whole plan, each row ramping from the row before it (as the firmware does)."""
    return [label(s, steps[i - 1] if i else None) for i, s in enumerate(steps)]


def range_protocol(alg):
    """The protocol word of a report-card range, from its wire `alg` name ('PPS (PD3.x)' -> 'PPS', both AVS kinds -> 'AVS')."""
    return {"PPS (PD3.x)": "PPS", "PD3.2+ SPR AVS": "AVS", "EPR AVS": "AVS"}.get(alg, alg)


def range_text(lo_mv, hi_mv, ma=0, mw=0):
    """'3.3-21V 5A' (a power-only offer reads '9-20V 60W')."""
    out = f"{milli(lo_mv)}-{milli(hi_mv)}V"
    return out + (f" {milli(ma)}A" if ma else f" {mw // 1000}W" if mw else "")


def pdo_range(raw):
    """The advertised range of a PPS / AVS PDO ('3.3-21V 5A', '15-28V 5A', '9-15V 3A'); '' for anything else."""
    if not raw or raw >> 30 != 3:
        return ""
    sub = (raw >> 28) & 3
    if sub == 0:   # SPR PPS: 100 mV / 50 mA
        return range_text(((raw >> 8) & 0xFF) * 100, ((raw >> 17) & 0xFF) * 100, (raw & 0x7F) * 50)
    if sub == 1:   # EPR AVS: max 25:17, min 15:8 (100 mV), PDP 7:0 in watts
        hi = ((raw >> 17) & 0x1FF) * 100
        return range_text(((raw >> 8) & 0xFF) * 100, hi, (raw & 0xFF) * 1000 * 1000 // hi if hi else 0)
    if sub == 2:   # PD 3.2 SPR AVS: 9 V up; 20 V limit with the current in 9:0, else 15 V with the current in 19:10 (10 mA)
        v20 = raw & 0x3FF
        return range_text(9000, 20000 if v20 else 15000, (v20 if v20 else (raw >> 10) & 0x3FF) * 10)
    return ""
