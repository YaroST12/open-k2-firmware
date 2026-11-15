#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Audits the K2's answers to what a source asks, from Black Box traces (docs/PD_SINK_REPLIES.md has the table).

  pd_reply_audit.py runs/NAME [runs/OTHER/trace.jsonl ...] [--files LIST] [--limit-ms 10] [--examples 4] [--json]
  k2_stream.py replies runs/NAME [--limit-ms 10] [--json]        (the same, on one k2_stream session folder)

For every SOP message a source sent in a session the K2 owned - the K2's chip acknowledged it (I_GCRCSENT) and no other sink
spoke within 1.5 s (inline, the conversation is the device's) - the table says which reply the K2 owed, the trace says which
reply it gave and when: the latency runs from the frame's RX stamp (its INT_N edge) to the TX_START of the reply (to the TX
record when the trace has no register writes). Flagged: an owed reply missing, a reply of the wrong kind, a latency over
--limit-ms (default 10 ms; the spec's tReceiverResponse is 15 ms). The exit status is 1 when anything is flagged.

Not judged, by design (the table's exception rows): unstructured 0x2717 VDMs (the Xiaomi script owns that conversation), and
whatever a deliberately silent or lying session does (a PD-silent unit never acknowledges, so it is never owned; PD Stress
runs are reviewed by hand).
"""
import argparse
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

LINE = re.compile(r"(RX|TX)\s+(SOP\S*)\s")
CTL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY", 7: "Get_Source_Cap", 8: "Get_Sink_Cap",
       9: "DR_Swap", 10: "PR_Swap", 11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset", 14: "Data_Reset",
       15: "Data_Reset_Complete", 16: "Not_Supported", 17: "Get_Source_Cap_Extended", 18: "Get_Status", 19: "FR_Swap",
       20: "Get_PPS_Status", 21: "Get_Country_Codes", 22: "Get_Sink_Cap_Extended", 23: "Get_Source_Info", 24: "Get_Revision"}
DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities", 5: "Battery_Status", 6: "Alert",
        7: "Get_Country_Info", 8: "Enter_USB", 9: "EPR_Request", 10: "EPR_Mode", 11: "Source_Info", 12: "Revision",
        15: "Vendor_Defined"}
EXT = {1: "Source_Capabilities_Extended", 2: "Status", 3: "Get_Battery_Cap", 4: "Get_Battery_Status",
       5: "Battery_Capabilities", 6: "Get_Manufacturer_Info", 7: "Manufacturer_Info", 8: "Security_Request",
       9: "Security_Response", 10: "Firmware_Update_Request", 11: "Firmware_Update_Response", 12: "PPS_Status",
       13: "Country_Info", 14: "Country_Codes", 15: "Sink_Capabilities_Extended", 16: "Extended_Control",
       17: "EPR_Source_Capabilities", 18: "EPR_Sink_Capabilities", 30: "Vendor_Defined_Extended"}
ECDB = {1: "EPR_Get_Source_Cap", 2: "EPR_Get_Sink_Cap", 3: "EPR_KeepAlive", 4: "EPR_KeepAlive_Ack"}
VDM_CMD = {1: "Discover_Identity", 2: "Discover_SVIDs", 3: "Discover_Modes", 4: "Enter_Mode", 5: "Exit_Mode", 6: "Attention"}
CHUNK = 26          # data bytes in a full chunk
WINDOW_MS = 100     # a reply later than this is no reply (the source has given up long before)
OWN_MS = 1500       # another sink speaking this close means the K2 sits inline: the conversation is not the K2's
HARD_RESET = "Hard_Reset"


class Msg:
    """One SOP message from a trace record's raw bytes (RX: token, header, payload, CRC; TX: header, payload, token)."""

    def __init__(self, rec):
        raw = bytes.fromhex(rec.get("raw") or "")
        rx = rec["type"] == "RX"
        body = raw[1:-4] if rx else raw[:-1]
        if len(body) < 2:
            raise ValueError("short frame")
        h = body[0] | body[1] << 8
        self.t = float(rec["t_ms"])
        self.rx = rx
        self.payload = body[2:]
        self.ext = bool(h >> 15 & 1)
        self.ndo = h >> 12 & 7
        self.src = bool(h >> 8 & 1)          # power role: source (or a cable plug on SOP')
        self.pd3 = (h >> 6 & 3) >= 2
        self.type = h & 0x1F
        if self.ext:
            self.name = EXT.get(self.type, f"Ext_{self.type}")
        elif self.ndo:
            self.name = DATA.get(self.type, f"Data_{self.type}")
        else:
            self.name = CTL.get(self.type, f"Ctl_{self.type}")
        self.eh = None
        if self.ext and len(self.payload) >= 2:
            e = self.payload[0] | self.payload[1] << 8
            self.eh = {"size": e & 0x1FF, "request": bool(e >> 10 & 1), "chunk": e >> 11 & 0xF, "chunked": bool(e >> 15 & 1)}
        self.ecdb = self.payload[2] if self.ext and self.type == 16 and len(self.payload) > 2 else None
        self.vdm = None
        if not self.ext and self.type == 15 and len(self.payload) >= 4:
            w = int.from_bytes(self.payload[:4], "little")
            self.vdm = {"svid": w >> 16, "structured": bool(w >> 15 & 1), "ctype": w >> 6 & 3, "cmd": w & 0x1F}

    def key(self):
        if self.vdm:
            v = self.vdm
            if not v["structured"]:
                return f"VDM unstructured 0x{v['svid']:04X}"
            return f"VDM {VDM_CMD.get(v['cmd'], 'cmd ' + str(v['cmd']))} {['REQ', 'ACK', 'NAK', 'BUSY'][v['ctype']]} 0x{v['svid']:04X}"
        if self.ecdb is not None:
            return f"Extended_Control {ECDB.get(self.ecdb, self.ecdb)}"
        return self.name

    def more_chunks(self):
        e = self.eh
        return bool(e and e["chunked"] and not e["request"] and (e["chunk"] + 1) * CHUNK < e["size"])


def is_type(*names):
    return lambda m: m != HARD_RESET and m.name in names


def owed(m, epr):
    """What the K2 owes message m (a source's, PD revision of its header): None = nothing, else (label, accepts(reply))."""
    if m.ext:
        if not m.pd3 or (m.eh and m.eh["request"]):
            return None                                          # PD 2.0 has no extended messages; a chunk request is ours to serve
        if m.more_chunks():
            return ("Chunk request", lambda r: r != HARD_RESET and r.ext and r.type == m.type and r.eh and r.eh["request"])
        if m.type == 17:
            return ("EPR_Request", is_type("EPR_Request")) if epr else None   # SPR mode: the answer to our query, informational
        if m.type == 16:
            want = {1: ("EPR_Source_Capabilities",), 2: ("EPR_Sink_Capabilities",), 3: ("Extended_Control",)}.get(m.ecdb)
            if m.ecdb == 4:
                return None
            return ("/".join((want or ()) + ("Not_Supported",)), is_type(*((want or ()) + ("Not_Supported",))))
        answer = {3: "Battery_Capabilities", 4: "Battery_Status", 6: "Manufacturer_Info", 8: "Security_Response",
                  10: "Firmware_Update_Response"}
        if m.type in answer:
            return (f"{answer[m.type]}/Not_Supported", is_type(answer[m.type], "Not_Supported"))
        if m.type in EXT:
            return None                                          # replies and notifications
        return ("Not_Supported", is_type("Not_Supported"))       # reserved
    if m.ndo:
        if m.type == 1:
            return ("Request", lambda r: r == HARD_RESET or r.name in ("Request", "EPR_Request"))
        if m.type == 15:
            v = m.vdm
            if v is None or v["ctype"] != 0 and v["structured"] or v["structured"] and v["cmd"] == 6:
                return None                                      # ACK / NAK / BUSY, Attention
            if not v["structured"]:
                if v["svid"] == 0x2717:
                    return None                                  # the Xiaomi script owns it (exception row)
                return ("Not_Supported", is_type("Not_Supported")) if m.pd3 else None
            return ("VDM ACK/NAK/BUSY", lambda r: r != HARD_RESET and r.vdm is not None and r.vdm["structured"]
                    and r.vdm["svid"] == v["svid"] and r.vdm["cmd"] == v["cmd"] and r.vdm["ctype"] != 0)
        if m.type in (7, 8):
            want = "Country_Info" if m.type == 7 else "Accept"
            return (f"{want}/Not_Supported", is_type(want, "Reject", "Not_Supported")) if m.pd3 else None
        if m.type in DATA:
            return None
        return ("Not_Supported", is_type("Not_Supported")) if m.pd3 else None
    t = m.type
    if t == 8:
        return ("Sink_Capabilities", is_type("Sink_Capabilities"))
    if t == 13:
        return ("Accept", is_type("Accept"))
    if t in (9, 10, 11):
        return ("Reject/Accept/Wait", is_type("Reject", "Accept", "Wait", "Not_Supported"))
    if t == 7:
        return ("Not_Supported/Reject", is_type("Not_Supported", "Reject", "Source_Capabilities"))
    if not m.pd3:
        return None                                              # the rest does not exist in PD 2.0
    pd3 = {14: ("Accept", "Reject"), 17: ("Source_Capabilities_Extended",), 18: ("Status",), 19: ("Reject", "Accept"),
           20: ("PPS_Status",), 21: ("Country_Codes",), 22: ("Sink_Capabilities_Extended",), 23: ("Source_Info",),
           24: ("Revision",)}
    if t in pd3:
        names = pd3[t] + ("Not_Supported",)
        return ("/".join(names), is_type(*names))
    if t in CTL:
        return None                                              # replies and notifications
    return ("Not_Supported", is_type("Not_Supported"))           # reserved


def load(path):
    p = Path(path)
    if p.is_dir():
        p = p / "trace.jsonl"
    recs = []
    with open(p) as f:
        for line in f:
            try:
                r = json.loads(line)
            except ValueError:
                continue
            if "t_ms" in r and "type" in r:
                recs.append(r)
    return str(p), recs


def sop_msg(rec):
    if rec["type"] not in ("RX", "TX"):
        return None
    m = LINE.match(rec.get("text", ""))
    if not m or m.group(2) != "SOP":
        return None
    try:
        return Msg(rec)
    except ValueError:
        return None


def audit_records(name, recs, limit_ms):
    """One trace: a list of owed-reply findings (dicts)."""
    msgs = [(i, sop_msg(r)) for i, r in enumerate(recs)]
    has_irq = any(r["type"] == "IRQ" for r in recs)
    foreign = [m.t for _, m in msgs if m and m.rx and not m.src and m.name != "GoodCRC"]   # another sink's frames
    acks = [float(r["t_ms"]) for r in recs if r["type"] == "IRQ" and "I_GCRCSENT" in r.get("text", "")]
    out, epr = [], False
    for k, (i, m) in enumerate(msgs):
        if m is None:
            r = recs[i]
            if r["type"] == "RESET" and "Hard" in r.get("text", ""):
                epr = False
            elif r["type"] == "SINK":
                epr = r.get("text", "").rstrip().endswith(" EPR")   # the sink's own EPR-mode flag
            continue
        if m.name == "EPR_Mode" and len(m.payload) >= 4:
            if m.rx and m.payload[3] == 3:
                epr = True
            elif m.payload[3] == 5:
                epr = False
        if m.name == "Soft_Reset":
            epr = False
        if not m.rx or not m.src or m.name == "GoodCRC":
            continue
        need = owed(m, epr)
        if need is None:
            continue
        if any(abs(t - m.t) < OWN_MS for t in foreign):
            continue                                             # inline: the device's conversation
        # The chip's GoodCRC for this frame: an I_GCRCSENT read close to the frame's stamp (the IRQ record can land on either side
        # of the RX record: the RX carries its INT_N edge, the IRQ the time service() read it).
        nxt = next((x.t for _, x in msgs[k + 1:] if x and x.rx and x.src and x.name != "GoodCRC"), m.t + WINDOW_MS)
        acked = not has_irq or any(m.t - 2.0 <= t < min(nxt - 1.0, m.t + 30.0) for t in acks)
        if not acked:
            continue                                             # never acknowledged: the source heard nothing from the K2
        reply, start, first = None, None, None
        for j, r in enumerate(recs[i + 1:], i + 1):
            if float(r["t_ms"]) - m.t > WINDOW_MS:
                break
            if r["type"] == "RESET":
                if "Hard" in r.get("text", ""):
                    reply, start = HARD_RESET, float(r["t_ms"])
                break
            x = sop_msg(r)
            if x is None:
                continue
            if x.rx and x.src and x.name != "GoodCRC":
                break                                            # the source moved on
            if x.rx or x.name == "GoodCRC":
                continue
            first = first or x
            if need[1](x):                                       # the owed kind; anything else of ours is not the answer
                reply, start = x, x.t
                tx_start = next((float(rr["t_ms"]) for rr in recs[j + 1:j + 6]
                                 if rr["type"] == "REG" and "TX_START" in rr.get("text", "")), None)
                start = tx_start if tx_start is not None else start
                break
        if reply is None and first is not None and float(first.t) - m.t <= limit_ms:
            reply, start = first, first.t                        # an answer of the wrong kind, in time
        key = m.key() + (" (more chunks)" if m.more_chunks() else "")
        f = {"file": name, "t_ms": round(m.t, 2), "msg": key, "owed": need[0], "reply": None, "latency_ms": None, "flag": None}
        if reply is None:
            f["flag"] = "missing"
        else:
            f["reply"] = reply if reply == HARD_RESET else reply.key()
            f["latency_ms"] = round(start - m.t, 2)
            if not need[1](reply):
                f["flag"] = "wrong"
            elif f["latency_ms"] > limit_ms:
                f["flag"] = "late"
        out.append(f)
    return out


def summarize(findings):
    by = defaultdict(list)
    for f in findings:
        by[f["msg"]].append(f)
    rows = []
    for key in sorted(by):
        fs = by[key]
        lat = sorted(f["latency_ms"] for f in fs if f["latency_ms"] is not None)
        replies = defaultdict(int)
        for f in fs:
            replies[f["reply"] or "-"] += 1
        rows.append({"msg": key, "owed": fs[0]["owed"], "n": len(fs), "replies": dict(replies),
                     "latency_ms": [lat[0], statistics.median(lat), lat[-1]] if lat else None,
                     "missing": sum(f["flag"] == "missing" for f in fs), "wrong": sum(f["flag"] == "wrong" for f in fs),
                     "late": sum(f["flag"] == "late" for f in fs)})
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="session folders (k2_stream record --out) or trace.jsonl files")
    ap.add_argument("--files", help="a text file listing trace paths, one per line")
    ap.add_argument("--limit-ms", type=float, default=10.0, help="flag replies later than this (default 10)")
    ap.add_argument("--examples", type=int, default=4, help="flagged examples printed per message (default 4)")
    ap.add_argument("--ignore", action="append", default=[], metavar="TEXT",
                    help="leave out messages whose name contains TEXT (a session that silences them on purpose); repeatable")
    ap.add_argument("--json", action="store_true", help="print the summary and every flagged finding as JSON")
    a = ap.parse_args(argv)
    paths = list(a.paths)
    if a.files:
        paths += [ln.strip() for ln in open(a.files) if ln.strip()]
    if not paths:
        ap.error("no trace given")
    findings = []
    for p in paths:
        try:
            name, recs = load(p)
        except OSError as e:
            print(f"cannot read {p}: {e}", file=sys.stderr)
            return 2
        findings += audit_records(name, recs, a.limit_ms)
    findings = [f for f in findings if not any(s in f["msg"] for s in a.ignore)]
    rows = summarize(findings)
    flagged = [f for f in findings if f["flag"]]
    if a.json:
        print(json.dumps({"summary": rows, "flagged": flagged}, indent=1))
    else:
        print(f"{'message':42} {'owed':34} {'n':>4} {'min/med/max ms':>20} {'miss':>4} {'wrong':>5} {'late':>4}  replies")
        for r in rows:
            lat = "/".join(f"{x:.1f}" for x in r["latency_ms"]) if r["latency_ms"] else "-"
            rep = ", ".join(f"{k} x{v}" for k, v in sorted(r["replies"].items()))
            print(f"{r['msg']:42} {r['owed'][:34]:34} {r['n']:>4} {lat:>20} {r['missing']:>4} {r['wrong']:>5} {r['late']:>4}  {rep}")
        shown = defaultdict(int)
        for f in flagged:
            if shown[f["msg"]] >= a.examples:
                continue
            shown[f["msg"]] += 1
            what = f"{f['reply']} after {f['latency_ms']} ms" if f["reply"] else "no reply"
            print(f"  {f['flag'].upper():7} {f['msg']} at {f['t_ms']} ms in {f['file']}: {what} (owed {f['owed']})")
        print(f"{len(findings)} owed replies, {len(flagged)} flagged (limit {a.limit_ms:g} ms)")
    return 1 if flagged else 0


if __name__ == "__main__":
    sys.exit(main())
