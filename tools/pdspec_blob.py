# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""PD Spec Check result blob: decoder, report text, and the firmware tables parsed from the sources.

The blob layout is documented in include/k2/pdspec/result.hpp ("the blob"). Nothing here is hard-coded that the firmware headers already
say: check keys, labels, kinds, units and weights come from kDefs, skip reasons from skip_name, stop reasons, start refusals and family
names from engine.hpp, so a table edited in the firmware is what this tool uses. tests/test_pd_spec.py compiles the firmware headers,
writes a blob with the firmware's own BlobReader and checks this decoder against it.
"""
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PD_DIR = ROOT / "include" / "k2" / "pdspec"

MAGICS = (b"PSC1", b"PSC2")   # PSC1: 21 checks and 14-byte PPS points; PSC2: n_checks in the header, 28-byte sweep passes
HEADER = 16
AGG = 20
TIMERS = 32
TRANS = 20
PPS = 14                # PSC1 PPS point record
PASS = 28               # PSC2 pass record (pdspec::PassRec)
PSC1_CHECKS = 21
PASS_AVS = 0x80         # pdspec::kPassAvs
PS_BITS = [(1, "Not_Supported"), (2, "no answer"), (4, "voltage"), (8, "current"), (16, "OMF"), (32, "flags")]   # pdspec::kPs*
NEVER = 99999999        # pdspec::kNever: an event that never came
OPEN = 1000000          # pdspec::kOpenLimit: the open side of a one-sided Range limit
NOT_SEEN16 = 0xFFFF
NOT_SEEN32 = 0xFFFFFFFF
STATES = ["idle", "running", "finished"]          # PdSpecSession::State (the status block and the blob header)
TR_FLAGS = [(1, "epr"), (2, "live band"), (4, "refused"), (8, "no capture"), (16, "suspect stamp")]   # pdspec::kTr*
_cache = {}


def _read(name):
    return (PD_DIR / name).read_text()


def _strip_comments(s):
    return re.sub(r"//[^\n]*", "", s)


def _enum_names(text, enum):
    m = re.search(r"enum class " + enum + r"\b[^{]*\{(.*?)\};", text, re.S)
    if not m:
        raise ValueError(f"enum {enum} not found")
    names = [n.strip() for n in _strip_comments(m.group(1)).replace("\n", " ").split(",")]
    return [n for n in names if n and n != "Count"]


def _case_strings(text, func, enum):
    """{enum value name: returned string} from the `case Enum::Name: return "text";` lines of constexpr function `func`."""
    m = re.search(r"const char\* " + func + r"\([^)]*\)\s*\{(.*?)\n\}", text, re.S)
    if not m:
        raise ValueError(f"function {func} not found")
    return dict(re.findall(r"case " + enum + r"::(\w+):\s*return\s*\"([^\"]*)\";", m.group(1)))


def load_tables():
    """The checks (key, label, kind, unit, weight, inferred) in wire order, the skip reasons, stop texts, start refusals and family names."""
    if "t" in _cache:
        return _cache["t"]
    res, eng = _read("result.hpp"), _read("engine.hpp")
    check_names = _enum_names(res, "Check")
    rows = re.search(r"kDefs\s*\{\{(.*?)\n\}\};", res, re.S).group(1)
    defs = [{"key": k, "label": l, "kind": kd.lower(), "unit": u.lower(), "weight": int(w), "inferred": inf == "true"}
            for k, l, kd, u, w, inf in
            re.findall(r"\{\"(\w+)\",\s*\"([^\"]*)\",\s*Kind::(\w+),\s*Unit::(\w+),\s*(\d+)(?:,\s*(true|false))?\}", rows)]
    if len(defs) != len(check_names):
        raise ValueError(f"result.hpp: {len(defs)} kDefs rows for {len(check_names)} checks")
    for d, n in zip(defs, check_names):
        d["enum"] = n
    skip_txt = _case_strings(res, "skip_name", "Skip")
    stop_txt = _case_strings(eng, "stop_name", "Stop")
    start_txt = _case_strings(eng, "start_name", "StartResult")
    fam_txt = _case_strings(eng, "family_name", "Family")
    note_txt = _case_strings(res, "caps_note_name", "CapsNote")
    plan = _read("plan.hpp")
    steps = {k: [int(x) for x in re.search(k + r"\[3\]\s*=\s*\{([^}]*)\}", plan).group(1).split(",")] for k in ("pps", "avs")}
    t = {
        "checks": defs,
        "skips": [skip_txt[n] for n in _enum_names(res, "Skip")],
        "verdicts": [n.lower() for n in _enum_names(res, "Verdict")],
        "stops": [stop_txt[n] for n in _enum_names(eng, "Stop")],
        "starts": [start_txt[n] for n in _enum_names(eng, "StartResult")],
        "families": [fam_txt[n] for n in _enum_names(eng, "Family")],
        "caps_notes": [note_txt[n] for n in _enum_names(res, "CapsNote")],
        "steps": steps,   # plan.hpp step_mv: the step of class 0..2 per kind (class 3 = the slams)
    }
    _cache["t"] = t
    return t


def _name(lst, i):
    return lst[i] if 0 <= i < len(lst) else f"#{i}"


def families_text(mask):
    fams = load_tables()["families"]
    picked = [f for i, f in enumerate(fams) if mask >> i & 1]
    return ", ".join(picked) if picked else "none"


def _t16(v):
    return None if v == NOT_SEEN16 else v


def _t32(v):
    return None if v == NOT_SEEN32 else v


def ps_text(bits):
    """The PPS_Status fault bits as words ("clean" for 0)."""
    return ", ".join(n for bit, n in PS_BITS if bits & bit) or "clean"


def step_text(kind, cls):
    t = load_tables()
    return "slams" if cls >= 3 else f"{t['steps'][kind][cls]} mV"


def decode_blob(b):
    """The whole blob as a dict: header, per-check verdicts with the worst value and its limits, the timer numbers, the transitions
    and the PPS points (PSC1) or sweep passes (PSC2). Raises ValueError on a wrong magic or a blob shorter than its own header says."""
    t = load_tables()
    b = bytes(b)
    if len(b) < HEADER or b[:4] not in MAGICS:
        raise ValueError("not a PD Spec Check blob (magic PSC1 / PSC2)")
    version = int(b[3:4].decode())
    state, stop, n_trans, n_pps = b[4], b[5], b[6], b[7]
    score, elapsed_s = struct.unpack_from("<HH", b, 8)
    flags = b[12]
    n_checks = PSC1_CHECKS if version == 1 else b[13]
    if n_checks > len(t["checks"]):
        raise ValueError(f"blob has {n_checks} checks, this tool knows {len(t['checks'])}")
    caps_note = None if version == 1 else _name(t["caps_notes"], b[14])
    opts = 0 if version == 1 else b[15]
    rec = PPS if version == 1 else PASS
    total = HEADER + n_checks * AGG + TIMERS + n_trans * TRANS + n_pps * rec
    if len(b) < total:
        raise ValueError(f"PD Spec Check blob is {len(b)} bytes, its header needs {total}")
    checks = []
    for k, d in enumerate(t["checks"][:n_checks]):
        o = HEADER + k * AGG
        verdict, skip, a, bb, n, fails, worst, lo, hi = struct.unpack_from("<BBBBHHiii", b, o)
        checks.append({"key": d["key"], "label": d["label"], "kind": d["kind"], "unit": d["unit"], "weight": d["weight"],
                       "verdict": _name(t["verdicts"], verdict), "skip": _name(t["skips"], skip) if skip else None,
                       "n": n, "fails": fails, "penalty": 0 if d["inferred"] else d["weight"] * fails,   # inferred: unverified
                       "worst": None if not n else worst, "lo": lo, "hi": hi, "a": a, "b": bb})
    o = HEADER + n_checks * AGG
    w = struct.unpack_from("<8I", b, o)
    timers = {"pps_ka_ms": _t32(w[0]), "epr_ka_ms": _t32(w[1]),
              "hr_from_mv": w[2] & 0xFFFF, "hr_off_ms": _t16(w[2] >> 16), "hr_dark_ms": _t16(w[3] & 0xFFFF),
              "hr_on_ms": _t16(w[3] >> 16), "hr_caps_ms": _t16(w[4] & 0xFFFF), "det_from_mv": w[4] >> 16,
              "det_off_ms": _t16(w[5] & 0xFFFF), "det_back_ms": _t16(w[5] >> 16), "poll_gap_ms": w[6]}
    o += TIMERS
    trans = []
    for i in range(n_trans):
        f, to, fl, _pad, acc, psrdy, start, band, psrdy_mv, peak, final, gap = struct.unpack_from("<BBBBHHHHHHHH", b, o + i * TRANS)
        trans.append({"from": f, "to": to, "flags": [n for bit, n in TR_FLAGS if fl & bit],
                      "accept_ms": None if acc == NOT_SEEN16 else acc / 10.0, "psrdy_ms": _t16(psrdy), "start_ms": _t16(start),
                      "band_ms": _t16(band), "psrdy_mv": psrdy_mv or None, "peak_mv": peak or None, "final_mv": final or None,
                      "gap_ms": gap})
    o += n_trans * TRANS
    pps, passes = [], []
    for i in range(n_pps if version == 1 else 0):
        pos, steps, mono, acc, set_mv, err, dmin, dmax, settle = struct.unpack_from("<BBBBHhhhH", b, o + i * PPS)
        pps.append({"pos": pos, "steps": steps, "mono_fails": mono, "acc_fails": acc, "set_mv": set_mv, "worst_err_mv": err,
                    "min_delta_mv": dmin, "max_delta_mv": dmax, "max_settle_ms": settle / 10.0})
    t01 = lambda v: None if v == NOT_SEEN16 else v / 10.0
    for i in range(n_pps if version == 2 else 0):
        (pos, cls, steps, lo, hi, band, psrdy, err, dmin, over, out, f_band, f_psrdy, f_mono, f_acc, f_over, f_out, status,
         _pad) = struct.unpack_from("<BBHHHHHhhhHBBBBBBBB", b, o + i * PASS)
        kind = "avs" if cls & PASS_AVS else "pps"
        c = cls & 0x7F
        passes.append({"pos": pos, "kind": kind, "class": c, "step": step_text(kind, c), "lo_mv": lo, "hi_mv": hi, "steps": steps,
                       "band_ms": t01(band), "psrdy_ms": t01(psrdy), "worst_err_mv": err,
                       "min_delta_mv": None if dmin == 0x7FFF else dmin, "over_mv": None if over == -0x7FFF else over,
                       "out_ms": t01(out), "fails": {"band": f_band, "ps_rdy": f_psrdy, "monotonic": f_mono, "accuracy": f_acc,
                                                     "overshoot": f_over, "transient": f_out},
                       "status": status, "status_text": ps_text(status) if kind == "pps" else None})
    tally = {v: sum(1 for c in checks if c["verdict"] == v) for v in ("pass", "fail", "skip", "unverified")}
    return {"version": version, "state": _name(STATES, state), "stop_code": stop, "stop": _name(t["stops"], stop), "score": score,
            "elapsed_s": elapsed_s, "release_unconfirmed": bool(flags & 1), "caps_note": caps_note, "full20": bool(opts & 1),
            "tally": tally, "checks": checks, "timers": timers, "transitions": trans, "pps": pps, "passes": passes}


def fmt_value(unit, v):
    """A check's value in the K2's own words: us as ms with one decimal, ms, mV as V, a count."""
    if v is None:
        return "-"
    if v >= NEVER:
        return "not seen"
    if unit == "us":
        return f"{v / 1000:.1f} ms"
    if unit == "ms":
        return f"{v} ms"
    if unit == "mv":
        return f"{v / 1000:.3f} V"
    return str(v)


def fmt_limit(c):
    u, lo, hi = c["unit"], c["lo"], c["hi"]
    if c["kind"] == "max" or (c["kind"] == "range" and lo <= -OPEN):
        return "<= " + fmt_value(u, hi)
    if c["kind"] == "min" or (c["kind"] == "range" and hi >= OPEN):
        return ">= " + fmt_value(u, lo)
    return f"{fmt_value(u, lo)} .. {fmt_value(u, hi)}"


def fmt_at(k, c, version=2):
    """Where the worst value came from: a PDO pair (transition checks), an APDO move (PSC2: position, step class, direction and the
    level asked for; PSC1: an APDO point), a PDO or the source (timers)."""
    keys = [d["key"] for d in load_tables()["checks"]]
    if k < keys.index("pps_monotonic"):
        return f"PDO {c['a']} to {c['b']}"
    prog = k < keys.index("pps_keepalive") or keys.index("pps_band") <= k <= keys.index("avs_transient")
    if prog and version == 1:
        return f"APDO {c['a']}, point {c['b']}"
    if prog:
        a = c["a"]
        kind = "avs" if c["key"].startswith("avs_") else "pps"
        return f"APDO {a & 15} {step_text(kind, (a >> 4) & 7)} {'down' if a & 0x80 else 'up'} to {c['b'] / 10:.1f} V"
    return f"PDO {c['a']}" if c["a"] else "the source"


def render_text(r):
    """A readable report: header, one line per check, then the timers, transitions and PPS points."""
    tl = r["tally"]
    lines = [f"PD Spec Check {r['state']}: {r['stop']}; {r['elapsed_s']} s",
             f"score {r['score']} (penalty, 0 = ideal; weights provisional, see docs/PD_SPEC_CHECK.md)  "
             f"pass {tl['pass']}  fail {tl['fail']}  skip {tl['skip']}  unverified {tl['unverified']}"]
    if r.get("caps_note"):
        lines.append(f"caps note (advisory, no verdict): {r['caps_note']}")
    if r["release_unconfirmed"]:
        lines.append("WARNING: the end-of-run return to 5 V was not confirmed: unplug the charger")
    lines.append("checks:")
    for k, c in enumerate(r["checks"]):
        if c["verdict"] == "skip":
            lines.append(f"  {c['key']:<15} skip       {c['skip'] or '-'}")
            continue
        worst = ps_text(c["worst"]) if c["key"] == "pps_status" else fmt_value(c["unit"], c["worst"])
        lines.append(f"  {c['key']:<15} {c['verdict']:<10} worst {worst:<11} limit {fmt_limit(c):<22} "
                     f"n {c['n']:<3} fails {c['fails']:<3} at {fmt_at(k, c, r.get('version', 2))}"
                     + (f"  penalty {c['penalty']}" if c["penalty"] else ""))
    tm = r["timers"]
    ms = lambda v: "-" if v is None else f"{v} ms"
    lines.append(f"timers: PPS timeout {ms(tm['pps_ka_ms'])}, EPR timeout {ms(tm['epr_ka_ms'])}; Hard Reset from "
                 f"{tm['hr_from_mv']} mV: off {ms(tm['hr_off_ms'])}, dark {ms(tm['hr_dark_ms'])}, on {ms(tm['hr_on_ms'])}, "
                 f"caps {ms(tm['hr_caps_ms'])}; detach from {tm['det_from_mv']} mV: off {ms(tm['det_off_ms'])}, "
                 f"back {ms(tm['det_back_ms'])}; poll gap {tm['poll_gap_ms']} ms")
    if r["transitions"]:
        lines.append("transitions (ms from Accept; mV):")
        lines.append("  from to  accept psrdy start  band  psrdy_mv peak_mv final_mv gap  flags")
        for x in r["transitions"]:
            f = lambda v: "-" if v is None else str(v)
            lines.append(f"  {x['from']:>4} {x['to']:>2}  {f(x['accept_ms']):>6} {f(x['psrdy_ms']):>5} {f(x['start_ms']):>5} "
                         f"{f(x['band_ms']):>5} {f(x['psrdy_mv']):>8} {f(x['peak_mv']):>7} {f(x['final_mv']):>8} {x['gap_ms']:>4}  "
                         + ",".join(x["flags"]))
    if r["pps"]:
        lines.append("PPS points (20 mV steps):")
        for p in r["pps"]:
            lines.append(f"  APDO {p['pos']} at {p['set_mv']} mV: {p['steps']} steps, worst error {p['worst_err_mv']} mV, step "
                         f"{p['min_delta_mv']}..{p['max_delta_mv']} mV, settle <= {p['max_settle_ms']:.1f} ms, "
                         f"fails monotonic {p['mono_fails']} accuracy {p['acc_fails']}")
    if r.get("passes"):
        f1 = lambda v: "-" if v is None else f"{v:.1f}"
        lines.append("programmable passes (ms from Accept; band = in vPpsNew/vAvsNew, settle on the final level for 20 mV; "
                     + ("20 mV over the whole window" if r.get("full20") else "20 mV in three 10-step windows each way") + "):")
        lines.append("  APDO kind step     window          moves band  ps_rdy err  dmin over out   fails band/rdy/mono/acc/ovs/out  status")
        for p in r["passes"]:
            fl = p["fails"]
            lines.append(f"  {p['pos']:>4} {p['kind']:<4} {p['step']:<8} {p['lo_mv']:>5}..{p['hi_mv']:<5} mV {p['steps']:>5} "
                         f"{f1(p['band_ms']):>5} {f1(p['psrdy_ms']):>6} {p['worst_err_mv']:>4} "
                         f"{'-' if p['min_delta_mv'] is None else p['min_delta_mv']:>5} {'-' if p['over_mv'] is None else p['over_mv']:>4} "
                         f"{f1(p['out_ms']):>5}   {fl['band']}/{fl['ps_rdy']}/{fl['monotonic']}/{fl['accuracy']}/{fl['overshoot']}/"
                         f"{fl['transient']}" + (f"  {p['status_text']}" if p["status_text"] else ""))
    return "\n".join(lines)
