# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""PD Stress result blob: decoder, host score, and the firmware tables parsed from the sources.

The blob layout is documented in include/k2/pdstress/result.hpp. Nothing here is hard-coded that the firmware headers already say: event
names and weights come from score.hpp, metric names and bin edges from dist.hpp, sequence names from plan.hpp, stop reasons from
engine.hpp, so a table edited in the firmware is what this tool uses. tests/test_pd_stress.py compiles the firmware headers and compares.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PD_DIR = ROOT / "include" / "k2" / "pdstress"

SEQ_SLOTS = 12          # kMaxSeqs
METRIC_WORDS = 10       # min, max, 8 bins
METRICS = 4             # kMetricCount
BINS = 8
FIXED_HEADER = 32       # up to the per-event points
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
    """{enum value name: returned string} from the `case Enum::Name: return "text";` lines of constexpr function `func`.
    Cases that share one return (fall through) all get that string."""
    m = re.search(r"const char\* " + func + r"\([^)]*\)\s*\{(.*?)\n\}", text, re.S)
    if not m:
        raise ValueError(f"function {func} not found")
    out, pending = {}, []
    for line in m.group(1).splitlines():
        c = re.match(r"\s*case " + enum + r"::(\w+):\s*(return\s*\"([^\"]*)\";)?", line)
        if not c:
            continue
        pending.append(c.group(1))
        if c.group(3) is not None:
            for p in pending:
                out[p] = c.group(3)
            pending = []
    return out


def load_tables():
    """Everything the host needs from the firmware headers: events (token order = wire order) and weights, the score floor, the
    metric names, units and bin edges, the sequence names, stop texts and not-applicable reasons."""
    if "t" in _cache:
        return _cache["t"]
    score, dist = _read("score.hpp"), _read("dist.hpp")
    plan, engine = _read("plan.hpp"), _read("engine.hpp")
    ev_names = _enum_names(score, "Event")
    ev_tok = _case_strings(score, "event_name", "Event")
    wm = re.search(r"kWeights\s*=\s*\{(.*?)\};", score, re.S)
    weights = [int(x) for x in re.findall(r"^\s*(\d+)\s*,", wm.group(1), re.M)]
    if len(weights) != len(ev_names):
        raise ValueError(f"score.hpp: {len(weights)} weights for {len(ev_names)} events")
    mt = re.search(r"kMinTransitions\s*=\s*(\d+)", score)
    m_names = _enum_names(dist, "Metric")
    m_tok = _case_strings(dist, "metric_name", "Metric")
    units = _case_strings(dist, "metric_unit", "Metric")
    em = re.search(r"kEdges\b[^=]*=\s*(\{\{.*?\}\};)", _strip_comments(dist), re.S).group(1)
    edges = [[int(x) for x in row.split(",")] for row in re.findall(r"\{\s*([\d,\s]+?)\s*\}", em[1:])]
    seq_rows = re.search(r"kSeqs\s*=\s*\{\{(.*?)\n\}\};", plan, re.S).group(1)
    seq_names = re.findall(r"^\s*\{\"(\w+)\"", seq_rows, re.M)
    stops = _enum_names(engine, "Stop")
    stop_txt = _case_strings(engine, "stop_name", "Stop")
    start_names = _enum_names(engine, "StartResult")
    start_txt = _case_strings(engine, "start_name", "StartResult")
    na_names = _enum_names(plan, "Na")
    na_txt = _case_strings(plan, "na_name", "Na")
    t = {
        "events": [ev_tok[n] for n in ev_names],
        "weights": weights,
        "min_transitions": int(mt.group(1)),
        "metrics": [(m_tok[n], units[n]) for n in m_names],
        "edges": edges,
        "seqs": seq_names,
        "stops": [stop_txt[n] for n in stops],
        "na": [na_txt[n] for n in na_names],
        "starts": [start_txt[n] for n in start_names],
    }
    _cache["t"] = t
    return t


def default_weights():
    t = load_tables()
    return dict(zip(t["events"], t["weights"]))


def merge_weights(over):
    """The default table with `over` laid on top; an unknown key is an error (a typo must not silently score nothing)."""
    w = default_weights()
    for k, v in over.items():
        if k not in w:
            raise ValueError(f"unknown event {k!r}; events: " + " ".join(w))
        if not isinstance(v, int) or isinstance(v, bool) or v < 0:
            raise ValueError(f"weight of {k} must be a non-negative integer")
        w[k] = v
    return w


def score_cp(totals, weights):
    """Hundredths of a point per 100 transitions: the firmware's formula, integer division included."""
    t = load_tables()
    pts = sum(totals["counts"].get(e, 0) * weights[e] for e in t["events"])
    n = max(totals["transitions"], t["min_transitions"])
    return pts * 100 * 100 // n


def _u16(b, o):
    return b[o] | b[o + 1] << 8


def _u32(b, o):
    return _u16(b, o) | _u16(b, o + 2) << 16


def _summary(bins, mn, mx, edges):
    """n, min, med, p95, max from an 8-bin histogram: the firmware's bin-resolution rule (dist.hpp summary())."""
    n = sum(bins)
    if n == 0:
        return {"n": 0, "min": 0, "med": 0, "p95": 0, "max": 0}

    def at_rank(rank):
        cum = 0
        for i, c in enumerate(bins):
            cum += c
            if cum >= rank:
                v = edges[i] if i < len(edges) else mx
                return min(max(v, mn), mx)
        return mx
    return {"n": min(n, 0xFFFF), "min": mn, "med": at_rank((n + 1) // 2), "p95": at_rank((95 * n + 99) // 100), "max": mx}


def _fmt_dist(d):
    if not d["n"]:
        return "-"
    div = 10.0 if d["unit"] == "0.1ms" else 1.0
    unit = "ms" if d["unit"] == "0.1ms" else d["unit"]
    f = lambda v: f"{v / div:g}"
    return f"n={d['n']} min {f(d['min'])} med {f(d['med'])} p95 {f(d['p95'])} max {f(d['max'])} {unit}"


def render_text(r):
    """A short readable report of a decoded result: header, what each sequence did, the score per event kind."""
    o = r["options"]
    state = "running" if r["running"] else ("finished" if r["finished"] else "stopped")
    lines = [f"PD Stress {state}: {r['stop']}; {r['transitions']} transitions in {r['elapsed_ms'] / 1000:.1f} s, "
             f"{r['reboots']} K2 reboot(s)",
             f"options: mask 0x{o['mask']:x} cycles {o['cycles_pct']} % pace {o['pace']} cap {o['max_s']} s / {o['max_transitions']} transitions",
             f"score {r['score']:.2f} (0 = ideal, penalty per 100 transitions; weights provisional, see docs/PD_STRESS.md)"]
    if r["stamp_suspect"]:
        lines.append(f"suspect timestamps: {r['stamp_suspect']} Accept latencies under 300 us (stamp not trusted)")
    lines.append("sequences:")
    for sid in r["order"]:
        s = r["sequences"][sid]
        bad = ", ".join(f"{e} {v}" for e, v in s["counts"].items() if v)
        tag = " (given up)" if s["given_up"] else ""
        lines.append(f"  {s['name']:<10} {s['transitions']:>4} transitions{tag}  {bad or 'clean'}")
        for m, d in s["dist"].items():
            if d["n"]:
                lines.append(f"      {m:<7} {_fmt_dist(d)}")
    skipped = [f"{s['name']} ({s['na']})" for s in r["sequences"] if s["skipped"]]
    if skipped:
        lines.append("not run: " + ", ".join(skipped))
    lines.append("score per event kind (count x weight):")
    for e, v in r["events"].items():
        if v["count"]:
            lines.append(f"  {e:<14} {v['count']:>5} x {v['points'] // v['count']:>3} = {v['points']}")
    if not any(v["count"] for v in r["events"].values()):
        lines.append("  none: every transition was clean")
    return "\n".join(lines)


def decode_blob(b):
    """The whole result blob as a dict: header, per-sequence counters and distributions, per-event totals with the firmware's own
    points, and the firmware score. Raises ValueError on a blob that is shorter than its own header says."""
    t = load_tables()
    b = bytes(b)
    if len(b) < FIXED_HEADER:
        raise ValueError("PD Stress blob shorter than its header")
    words = _u16(b, 30)
    n_ev = words - 1 - METRICS * METRIC_WORDS
    if n_ev <= 0:
        raise ValueError("PD Stress blob: bad record width")
    ev_off = FIXED_HEADER
    order_off = ev_off + 4 * n_ev
    flags_off = order_off + SEQ_SLOTS
    head = flags_off + SEQ_SLOTS
    total = head + SEQ_SLOTS * words * 2
    if len(b) < total:
        raise ValueError(f"PD Stress blob is {len(b)} bytes, its header needs {total}")
    names = list(t["events"]) + [f"event{i}" for i in range(len(t["events"]), n_ev)]
    flags = b[5]
    stop = b[1]
    seqs, totals = [], {"transitions": 0, "counts": {e: 0 for e in names[:n_ev]}}
    for slot in range(SEQ_SLOTS):
        w = [_u16(b, head + (slot * words + i) * 2) for i in range(words)]
        counts = {names[i]: w[1 + i] for i in range(n_ev)}
        sf = b[flags_off + slot]
        dist = {}
        for m in range(METRICS):
            base = 1 + n_ev + m * METRIC_WORDS
            bins = w[base + 2:base + 2 + BINS]
            mn, mx = w[base], w[base + 1]
            s = _summary(bins, mn, mx, t["edges"][m])
            s["unit"] = t["metrics"][m][1]
            s["bins"] = bins
            dist[t["metrics"][m][0]] = s
        na = sf >> 4
        seqs.append({
            "id": slot, "name": t["seqs"][slot] if slot < len(t["seqs"]) else f"seq{slot}",
            "done": bool(sf & 1), "skipped": bool(sf & 2), "given_up": bool(sf & 4),
            "na": t["na"][na] if na and na < len(t["na"]) else None,
            "transitions": w[0], "counts": counts, "dist": dist,
        })
        totals["transitions"] = min(0xFFFF, totals["transitions"] + w[0])
        for e, v in counts.items():
            totals["counts"][e] = min(0xFFFF, totals["counts"][e] + v)
    events = {}
    for i in range(n_ev):
        events[names[i]] = {"count": totals["counts"][names[i]], "points": _u32(b, ev_off + 4 * i)}
    n_order = b[3]
    return {
        "version": b[0],
        "stop_code": stop, "stop": t["stops"][stop] if stop < len(t["stops"]) else f"stop{stop}",
        "seq_count": b[2], "reboots": b[4],
        "running": bool(flags & 1), "finished": bool(flags & 2), "release_unconfirmed": bool(flags & 4),
        "options": {"mask": _u16(b, 8), "cycles_pct": _u16(b, 10), "pace": b[6],
                    "max_s": _u32(b, 16) // 1000, "max_transitions": _u16(b, 12)},
        "transitions": _u16(b, 14), "elapsed_ms": _u32(b, 20),
        "score_cp": _u32(b, 24), "score": _u32(b, 24) / 100.0,
        "stamp_suspect": _u16(b, 28),
        "order": list(b[order_off:order_off + n_order]),
        "events": events, "totals": totals, "sequences": seqs,
    }
