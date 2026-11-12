#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Re-encode a `pd_trace.py` text dump into Black Box records (the trace.raw format), for diagnosis fixtures.

A text dump is all that is left of a session once the ring has wrapped and no raw copy was kept. This tool only
inverts the decoder's text back into the record bytes the firmware would have written (record layout:
include/k2/pd/pdtrace.hpp); it does not judge anything. The analysis stays in C++ (k2/pd/diag.hpp).

What it re-encodes: RX / TX messages (header from name, id, revision and roles; the CRC as printed), with the data
objects the decoder printed in full: fixed / variable / battery / PPS PDOs, fixed and PPS Requests, structured VDM
headers (Discover Identity ACK VID / PID, Discover SVIDs lists), EPR_Mode actions and raw "objects ..." lines; VBUS, CC,
RESET, RX_BAD, TX_RESULT, LINK and MARK trace-start records. Words the text does not show (an ID header's product type,
a DP Configure pin) are written as 0, and the tool says so on stderr.

    python3 tools/bbdiag_fixture.py runs/.../a-dump3.txt -o tests/fixtures/bbdiag/hub_rear_60w.raw
"""
import argparse
import re
import struct
import sys

T_RX, T_RXBAD, T_TX, T_TXRESULT, T_RESET, T_CC = 1, 2, 3, 4, 5, 6
T_LINK, T_VBUS, T_MARK, T_RXREPEAT = 10, 12, 14, 16
CONTROL = {"GoodCRC": 1, "GotoMin": 2, "Accept": 3, "Reject": 4, "Ping": 5, "PS_RDY": 6, "Get_Source_Cap": 7,
           "Get_Sink_Cap": 8, "DR_Swap": 9, "PR_Swap": 10, "VCONN_Swap": 11, "Wait": 12, "Soft_Reset": 13,
           "Data_Reset": 14, "Data_Reset_Complete": 15, "Not_Supported": 16, "Get_Source_Cap_Extended": 17,
           "Get_Status": 18, "FR_Swap": 19, "Get_PPS_Status": 20, "Get_Country_Codes": 21,
           "Get_Sink_Cap_Extended": 22, "Get_Source_Info": 23, "Get_Revision": 24}
DATA = {"Source_Capabilities": 1, "Request": 2, "BIST": 3, "Sink_Capabilities": 4, "Battery_Status": 5, "Alert": 6,
        "Get_Country_Info": 7, "Enter_USB": 8, "EPR_Request": 9, "EPR_Mode": 10, "Source_Info": 11, "Revision": 12,
        "Vendor_Defined": 15}
SOPS = {"SOP": 0xE0, "SOP'": 0xC0, "SOP''": 0xA0}
SPEC = {"unknown": 0, "PD2.0": 1, "PD3.x": 2, "reserved": 3}
VDM_CMDS = {"Discover_Identity": 1, "Discover_SVIDs": 2, "Discover_Modes": 3, "Enter_Mode": 4, "Exit_Mode": 5,
            "Attention": 6}
VDM_TYPES = {"REQ": 0, "ACK": 1, "NAK": 2, "BUSY": 3}
EPR_ACTIONS = {"Enter": 1, "Enter_Acknowledged": 2, "Enter_Succeeded": 3, "Enter_Failed": 4, "Exit": 5}
RESETS = {"Hard Reset received": 0, "Hard Reset sent": 1, "Soft_Reset received": 2, "Soft_Reset sent": 3}
PDO_FLAGS = {"DRP": 29, "suspend-ok": 28, "unconstrained": 27, "usb-comms": 26, "DRD": 25, "unchunked": 24, "EPR": 23}
RDO_FLAGS = {"give-back": 27, "cap-mismatch": 26, "usb-comms": 25, "no-suspend": 24, "unchunked": 23, "EPR": 22}

LINE = re.compile(r"^\s*(\d+\.\d+) ms(?:\s+\+\s*[\d.]+)?\s+(\S+)\s+(.*)$")
MSG = re.compile(r"^(SOP''|SOP'|SOP)\s+(\w+) \(id (\d), (\w+\.?\w*), ([\w/-]+)\)(?:\s+crc=([0-9a-f]{8}))?")


def warn(msg):
    print(f"bbdiag_fixture: {msg}", file=sys.stderr)


def flags(text, table):
    m = re.search(r"\[([^\]]*)\]", text)
    v = 0
    for f in (m.group(1).split(",") if m else []):
        if f.strip() in table:
            v |= 1 << table[f.strip()]
    return v


def num(s):
    return float(s)


def pdo_word(t):
    m = re.match(r"fixed ([\d.]+) V ([\d.]+) A", t)
    if m:
        return round(num(m[1]) * 20) << 10 | round(num(m[2]) * 100) | flags(t, PDO_FLAGS)
    m = re.match(r"variable ([\d.]+)-([\d.]+) V ([\d.]+) A", t)
    if m:
        return 2 << 30 | round(num(m[2]) * 20) << 20 | round(num(m[1]) * 20) << 10 | round(num(m[3]) * 100)
    m = re.match(r"battery ([\d.]+)-([\d.]+) V ([\d.]+) W", t)
    if m:
        return 1 << 30 | round(num(m[2]) * 20) << 20 | round(num(m[1]) * 20) << 10 | round(num(m[3]) * 4)
    m = re.match(r"PPS ([\d.]+)-([\d.]+) V ([\d.]+) A", t)
    if m:
        return 3 << 30 | round(num(m[2]) * 10) << 17 | round(num(m[1]) * 10) << 8 | round(num(m[3]) * 20) | \
            (1 << 27 if "power-limited" in t else 0)
    m = re.match(r"empty", t)
    if m:
        return 0
    m = re.match(r"(?:raw|0x)([0-9A-Fa-f]{8})", t)
    if m:
        return int(m[1], 16)
    warn(f"PDO not re-encoded (written as 0): {t}")
    return 0


def rdo_word(t):
    m = re.match(r"PDO (\d+) operating ([\d.]+) A \(max ([\d.]+) A\)", t)
    if m:
        return int(m[1]) << 28 | round(num(m[2]) * 100) << 10 | round(num(m[3]) * 100) | flags(t, RDO_FLAGS)
    m = re.match(r"PDO (\d+) PPS ([\d.]+) V ([\d.]+) A", t)
    if m:
        return int(m[1]) << 28 | round(num(m[2]) * 50) << 9 | round(num(m[3]) * 20) | flags(t, RDO_FLAGS)
    warn(f"RDO not re-encoded (written as 0): {t}")
    return 0


def vdm_words(t):
    m = re.match(r"(\S+(?: \d+)?) (REQ|ACK|NAK|BUSY) SVID 0x([0-9A-Fa-f]{4})", t)
    if not m:
        m2 = re.match(r"unstructured VDM SVID 0x([0-9A-Fa-f]{4})", t)
        if m2:
            return [int(m2[1], 16) << 16]
        warn(f"VDM not re-encoded: {t}")
        return [0]
    name, ctype, svid = m[1], VDM_TYPES[m[2]], int(m[3], 16)
    cmd = VDM_CMDS.get(name)
    if cmd is None:
        sm = re.match(r"SVID-specific (\d+)", name)
        cmd = int(sm[1]) if sm else 0
    words = [svid << 16 | 1 << 15 | 1 << 13 | ctype << 6 | cmd]
    if cmd == 1 and ctype == 1:
        vm = re.search(r"\bVID 0x([0-9A-Fa-f]{4})", t)   # not the SVID
        pm = re.search(r"PID 0x([0-9A-Fa-f]{4})", t)
        words += [int(vm[1], 16) if vm else 0, 0, (int(pm[1], 16) << 16) if pm else 0]
        warn("Discover_Identity ACK: ID header product types and cert stat are not in the text (written as 0)")
    elif cmd == 2 and ctype == 1:
        lm = re.search(r"\[([^\]]*)\]", t)
        svids = [int(s, 16) for s in lm[1].split(",")] if lm and lm[1] else []
        if len(svids) % 2:
            svids.append(0)
        for i in range(0, len(svids), 2):
            words.append(svids[i] << 16 | svids[i + 1])
        if not svids:
            words.append(0)
    return words


def message(t_us, direction, head, detail):
    m = MSG.match(head)
    if not m:
        warn(f"message line not understood: {head}")
        return None
    sop, name, mid, spec, roles, crc = m.groups()
    objs = []
    ext = False
    if name in CONTROL:
        mtype = CONTROL[name]
    elif name in DATA:
        mtype = DATA[name]
        if mtype in (1, 4):
            objs = [pdo_word(re.sub(r"^PDO\d+ ", "", d)) for d in detail if re.match(r"PDO\d+ ", d)]
        elif mtype in (2, 9):
            objs = [rdo_word(d) for d in detail[:1]]
        elif mtype == 15:
            objs = vdm_words(detail[0]) if detail else [0]
        elif mtype == 10:
            am = re.match(r"EPR_Mode (\w+)", detail[0]) if detail else None
            objs = [EPR_ACTIONS.get(am[1], 0) << 24] if am else [0]
        else:
            om = re.match(r"objects ((?:[0-9A-F]{8} ?)+)", detail[0]) if detail else None
            objs = [int(w, 16) for w in om[1].split()] if om else [0]
    else:
        warn(f"message type not re-encoded (extended or unknown, skipped): {name}")
        return None
    hdr = mtype | SPEC.get(spec, 0) << 6 | int(mid) << 9 | len(objs) << 12 | (0x8000 if ext else 0)
    if sop == "SOP":
        pr, dr = roles.split("/") if "/" in roles else ("snk", "ufp")
        hdr |= (0x100 if pr == "src" else 0) | (0x20 if dr == "dfp" else 0)
    elif roles == "cable-plug":
        hdr |= 0x100
    body = struct.pack("<H", hdr) + b"".join(struct.pack("<I", o) for o in objs)
    if direction == "RX":
        return T_RX, bytes([SOPS[sop]]) + body + (bytes.fromhex(crc) if crc else b"\0\0\0\0")
    return T_TX, body + bytes([SOPS[sop]])


def other(kind, rest):
    if kind == "VBUS":
        m = re.match(r"([\d.]+) V (-?\d+) mA", rest)
        return T_VBUS, struct.pack("<Hh", round(num(m[1]) * 1000), int(m[2]))
    if kind == "CC":
        m = re.match(r"(attach|detach)(?: CC(\d))?", rest)
        return T_CC, bytes([1 if m[1] == "attach" else 0, int(m[2] or 0), 0])
    if kind == "RESET":
        m = re.search(r"\*\*\* (.*?) \*\*\*", rest)
        if m and m[1] in RESETS:
            return T_RESET, bytes([RESETS[m[1]]])
        warn(f"reset kind not understood: {rest}")
        return None
    if kind == "MARK" and rest.startswith("trace-start"):
        return T_MARK, bytes([3, 0, 0, 0, 0])
    if kind == "LINK":
        return T_LINK, bytes([(1 if "inline=yes" in rest else 0) | (2 if "auto_sink=yes" in rest else 0)])
    if kind == "TX_RESULT":
        m = re.match(r"(\w+) after (\d+) ms", rest)
        res = {"TxSent": 0, "RetryFail": 1, "Timeout": 2}.get(m[1], 0) if m else 0
        return T_TXRESULT, bytes([res, int(m[2]) if m else 0, 0])
    if kind == "RX_REPEAT":
        m = re.match(r"(\d+) more identical Source_Capabilities over (\d+) ms", rest)
        return T_RXREPEAT, struct.pack("<HH", int(m[1]), int(m[2]))
    if kind == "RX_BAD":
        return T_RXBAD, bytes([0, 0, 0])
    return None


def convert(lines):
    events = []   # (t_us, order, type, payload)
    i = 0
    while i < len(lines):
        m = LINE.match(lines[i])
        i += 1
        if not m:
            continue
        t_us = round(num(m[1]) * 1000)
        kind, rest = m[2], m[3]
        detail = []
        while i < len(lines) and re.match(r"^\s{20,}\| ", lines[i]):
            detail.append(lines[i].split("| ", 1)[1].rstrip())
            i += 1
        rec = message(t_us, kind, rest, detail) if kind in ("RX", "TX") else other(kind, rest)
        if rec:
            events.append((t_us, len(events), rec[0], rec[1]))
    seen, out = set(), bytearray()
    for t_us, _, rtype, payload in sorted(events):
        key = (t_us, rtype, payload)
        if key in seen:
            continue
        seen.add(key)
        out += bytes([6 + len(payload), rtype]) + struct.pack("<I", t_us & 0xFFFFFFFF) + payload
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dumps", nargs="+", help="pd_trace.py text dumps, merged in time order (duplicates dropped)")
    ap.add_argument("-o", "--out", required=True, help="raw record file to write")
    a = ap.parse_args()
    lines = []
    for f in a.dumps:
        with open(f, encoding="utf-8") as fh:
            lines += fh.read().splitlines()
    data = convert(lines)
    with open(a.out, "wb") as fh:
        fh.write(data)
    print(f"{a.out}: {len(data)} bytes")


if __name__ == "__main__":
    main()
