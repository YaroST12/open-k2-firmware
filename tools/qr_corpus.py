#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""QR share payload measurement and the schema v2 check: the corpus of real records, the current link sizes, the candidate
encodings for more room in one QR code, and --v2-check, which runs the firmware's own v2 encoder (via the host build of
tests/test_qrshare_link.cpp, dumped with K2_QRCORPUS_DUMP) over the same corpus and asserts that

  - the C++ v2 bytes are byte-identical to this file's Python prototype (pack_card_v2 / pack_emk_v2) for every corpus record,
  - every firmware v2 link decodes (tools/k2_qr.py decode_link) to the same JSON as the v1 record decoded the v1 way.

The record bytes, the LZ4 blocks and the link texts all come from the firmware's own code, compiled into a
host driver (tools/qr_corpus_driver.cpp, built on the fly): qrshare::link::encode, lz::compress, qrshare::pack
and qr::encode. The candidates are prototyped here in Python over that same corpus:

  1. record schema v2: a bit-packed re-encoding of the card and the e-marker records (varints, nibble enums,
     delta-free but presence-flagged repeated structures, the derivable padding dropped), round-trip checked
     against the v1 parse of every corpus record;
  2. entropy coding beyond LZ4 for ~100-250 B records: a static-dictionary LZSS (heatshrink-style), a static
     Huffman table and a static rANS table, all built from the corpus;
  3-5. capacity: exact level-L byte/alphanumeric capacities of the vendored QR generator for versions 11..25
     (tools/qr_capacity.c, binary search over qrcodegen_encodeSegments*), a mixed byte+alphanumeric segmentation
     of the link, and the panel geometry (area x px x quiet zone -> max version -> byte capacity).

Usage:
  python3 tools/qr_corpus.py [--corpus DIR] [--markdown] [--quiet]
    --corpus DIR   also read the card captures in DIR (CARDS.JSON-style *.json, and a show-all text listing)
                   and re-encode them into card records (from the JSON fields; the JSON is the card's own
                   rendering, so the re-encode is exact for every field it carries).
    --markdown     print the report as markdown tables (for the measurement notes).

The default corpus is the in-tree golden vectors (tests/fixtures/qrshare.tsv, tests/fixtures/qrlink.tsv) plus
the built extras: a cable with all eight quirk rows, the all-limits report card, a 10-PDO EPR source.
"""
import argparse
import heapq
import json
import math
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_qr  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent

DRIVER_SOURCES = [
    "tools/qr_corpus_driver.cpp",
    "src/ui/qrshare.cpp",
    "src/ui/qrshare_link.cpp",
    "src/ui/qrshare_records.cpp",
    "src/ui/qr.cpp",
    "src/core/fmt.cpp",
    "src/core/bootinfo.cpp",
    "src/pd/emrec.cpp",
    "src/pd/emk_quirk.cpp",
]
QR_C = "third_party/qrcodegen/qrcodegen.c"

KIND_LETTERS = {4: ("c", "z"), 2: ("e", "E"), 3: ("p", "P"), 5: ("i", "I"), 1: ("b", "B")}
TYPE_NAMES = {1: "boot", 2: "emk", 3: "pdcaps", 4: "card", 5: "identity", 6: "log"}

# The enum tables of the card record and its JSON rendering (include/k2/chargertest/charger_test.hpp,
# src/chargertest/report_card_json.cpp; the same tables docs/site/r/card.js carries).
GROUPS = ["PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO"]
ALGORITHMS = ["PD2.0 fixed", "PD3.x fixed", "PD3.1+ EPR", "PD3.2+ SPR AVS", "PPS (PD3.x)",
              "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC 5V handshake", "SVOOC 5V handshake",
              "SVOOC65 5V handshake", "UFCS", "VIVO", "PD3.0 revision", "PD3.1 revision", "PD3.2 revision",
              "EPR AVS", "QC2 Class B", "QC3 Class B"]
REASONS = ["", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
           "no power monitor", "no source", "load attached", "tool busy", "fake offers",
           "request failed", "timeout", "voltage outside band", "hard reset", "source lost",
           "cancelled", "5V release failed", "offers changed", "PD active: use QC port",
           "revision cannot be forced", "select at least one test", "physical cable limit", "unsupported PDO layout",
           "charger stayed in PD", "rebooted during request", "rebooted during handover", "EPR entry refused", "no contract",
           "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered", "blocked by PD",
           "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START", "Class A only (12 V max)",
           "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target", "withdrawn by source", "wrong voltage on 5V contract", "needs load"]
STATES = ["Setup", "Verify 5V", "Negotiating", "Hold", "Return to 5V", "Complete", "Stopped",
          "UNSAFE: unplug source", "Detach charger", "Initializing..."]
HANDOVER = ["not tried", "fell back", "stayed PD", "fault", "reboot", "in PD contract"]
VERDICT = ["none", "pass", "fail", "skip"]
GUARDS = ["unknown", "none", "cable", "direct"]
EVIDENCE = {"rmdo": 1, "pps": 2, "epr_table": 4, "spr_avs": 8, "epr": 16}


def sh(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw).stdout


# --------------------------------------------------------------------------- build & run the firmware driver

def build_driver(binpath: Path):
    srcs = [ROOT / s for s in DRIVER_SOURCES]
    qr_o = binpath.parent / "qrcodegen.o"
    newest = max(s.stat().st_mtime for s in srcs + [ROOT / QR_C, ROOT / "third_party/qrcodegen/qrcodegen.h"])
    if binpath.exists() and qr_o.exists() and binpath.stat().st_mtime >= newest and qr_o.stat().st_mtime >= newest:
        return
    binpath.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["gcc", "-O1", "-DNDEBUG", "-w", "-I", str(ROOT / "third_party/qrcodegen"),
                    "-c", str(ROOT / QR_C), "-o", str(qr_o)], check=True)
    subprocess.run(["g++", "-std=c++23", "-O1", "-DNDEBUG", "-w",
                    "-I", str(ROOT / "include"), "-I", str(ROOT / "tests"),
                    "-I", str(ROOT / "third_party/littlefs"),
                    "-I", str(ROOT / "third_party/qrcodegen")] +
                   [str(s) for s in srcs] + [str(qr_o), "-o", str(binpath)], check=True)


def run_driver(binpath: Path, extra_hex: Path | None):
    cmd = [str(binpath), str(ROOT)]
    if extra_hex:
        cmd.append(str(extra_hex))
    out = sh(cmd)
    header, rows = {}, []
    for line in out.splitlines():
        if line.startswith("#"):
            for kv in line[1:].split():
                k, _, v = kv.partition("=")
                header[k] = v
        elif line.strip():
            f = line.split("\t")
            rows.append({
                "name": f[0], "type": int(f[1]), "raw": bytes.fromhex(f[3]),
                "lz4": int(f[4]), "pack": int(f[5]),
                "plain_len": int(f[6]), "packed_len": int(f[7]),
                "plain_link": None if f[8] == "-" else f[8],
                "packed_link": None if f[9] == "-" else f[9],
                "plain_mod": int(f[10]), "packed_mod": int(f[11]),
            })
    return header, rows


# --------------------------------------------------------------------------- card record codec (Python twin)

def crc16_ccitt(data: bytes, crc: int = 0xFFFF) -> int:
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def u16(b, o): return b[o] | (b[o + 1] << 8)
def u32(b, o): return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)


def group_pack(q):
    v = q[1] | (q[2] << 8) | (q[3] << 16)
    d = v >> 18
    detail = 128 + (d & 0x1F) if d & 0x20 else d & 0x1F
    return {"verdict": q[0] >> 6, "reason": q[0] & 0x3F, "detail": detail,
            "max_dv": v & 0x1FF, "fail_dv": (v >> 9) & 0x1FF}


def parse_card(rec: bytes) -> dict:
    """The card record (204 B with v8, 202 B with v7) as a dict, per include/k2/chargertest/report_card.hpp."""
    assert len(rec) in (202, 204) and rec[0] == 0xA6, "not a card record"
    p = rec[4:4 + (len(rec) - 6)]
    name = p[34:54].split(b"\0")[0]
    d = {
        "fp": u32(p, 0), "vid": u16(p, 4), "pid": u16(p, 6),
        "card_no": u16(p, 8), "runs": u16(p, 10),
        "boots": u32(p, 12), "uptime_s": u32(p, 16), "fw": u32(p, 20),
        "handover_ds": u16(p, 24), "spec_rev": p[26], "num_pdos": p[27],
        "handover": p[28], "state": p[29], "reason": p[30],
        "n_ranges": p[31], "ev_flags": p[32], "format": p[33],
        "name": name.decode("ascii", "replace"),
        "pdp_w": p[54], "name_auto": bool(p[55] & 1), "pd_ma50": p[55] >> 1,
        "groups": [group_pack(p[56 + 4 * g:56 + 4 * g + 4]) for g in range(12)],
        "ranges": [], "epr": [], "cable": None, "ufcs": [], "times": p[190],
        "last_boots": u16(p, 191), "last_uptime_s": p[193] | (p[194] << 8) | (p[195] << 16),
        "rebalanced": p[196] if len(rec) == 204 and p[33] >= 8 else 0,
        "crc16": u16(rec, len(rec) - 2),
    }
    for i in range(4):
        q = p[104 + 10 * i:104 + 10 * i + 10]
        lo = u16(q, 2)
        d["ranges"].append({"alg": q[0], "pos": q[1], "lo_dv": lo & 0x1FF, "ma50": lo >> 9,
                            "hi_dv": u16(q, 4), "max_dv": u16(q, 6), "fail_dv": u16(q, 8)})
    for i in range(3):
        q = p[144 + 6 * i:144 + 6 * i + 6]
        g = group_pack(bytes([q[2], q[3], q[4], q[5]]))
        d["epr"].append({"dv": u16(q, 0), **g})
    q = p[162:178]
    if any(q):
        d["cable"] = {"seq": u32(q, 0), "vid": u16(q, 4), "pid": u16(q, 6), "max_mv": u16(q, 8),
                      "max_ma": u16(q, 10), "kind": q[12], "flags": q[13], "resumed": q[14], "src_flags": q[15]}
    for i in range(4):
        v = p[178 + 3 * i] | (p[179 + 3 * i] << 8) | (p[180 + 3 * i] << 16)
        if v:
            d["ufcs"].append({"lo_dv": v & 0xFF, "hi_dv": (v >> 8) & 0x1FF, "ma50": v >> 17})
    return d


def card_payload_from_json(j: dict) -> bytes:
    """Re-encode a card from its CARDS.JSON rendering (what the captures hold). Fields the JSON does not
    carry (evidence flags detail, the APDO position, the reserved bytes) are approximated; everything
    else is exact."""
    p = bytearray(198)

    def put16(o, v): p[o] = v & 0xFF; p[o + 1] = (v >> 8) & 0xFF
    def put32(o, v): put16(o, v & 0xFFFF); put16(o + 2, (v >> 16) & 0xFFFF)

    put32(0, int(j.get("fp", "0"), 16)); put16(4, j.get("vid", 0)); put16(6, j.get("pid", 0))
    put16(8, j.get("no", 0)); put16(10, j.get("runs", 1))
    put32(12, j.get("boots", 0)); put32(16, j.get("uptime_s", 0)); put32(20, int(j.get("fw", "0"), 16))
    put16(24, j.get("handover_ms", 0) // 100)
    p[26] = j.get("spec_rev", 0); p[27] = j.get("pdos", 0)
    p[28] = HANDOVER.index(j.get("handover", "not tried")) if j.get("handover") in HANDOVER else 0
    p[29] = STATES.index(j["state"]) if j.get("state") in STATES else 5
    p[30] = REASONS.index(j["reason"]) if j.get("reason") in REASONS else 0
    ranges = j.get("ranges", [])
    p[31] = len(ranges)
    ev = 0
    for r in ranges:
        if r.get("alg") == "PPS (PD3.x)": ev |= EVIDENCE["pps"]
        if r.get("alg") == "PD3.2+ SPR AVS": ev |= EVIDENCE["spr_avs"]
    if j.get("epr_levels"): ev |= EVIDENCE["epr"]
    p[32] = ev
    p[33] = 7
    name = (j.get("name") or "").encode()[:20]
    p[34:34 + len(name)] = name
    p[54] = j.get("pdp_w", 0)
    pd_ma50 = 0
    pdg = j.get("groups", {}).get("PD", {})
    if pdg.get("max_ma"): pd_ma50 = min(pdg["max_ma"] // 50, 127)
    p[55] = (1 if j.get("name_auto") else 0) | (pd_ma50 << 1)
    for g, gname in enumerate(GROUPS):
        r = j.get("groups", {}).get(gname, {})
        verdict = VERDICT.index(r.get("verdict", "none"))
        reason = REASONS.index(r["reason"]) if r.get("reason") in REASONS else 0
        detail = r.get("detail", 0)
        pd = (0x20 | (detail - 128)) & 0x3F if detail >= 128 else detail & 0x3F
        v = min((r.get("max_mv", 0) + 50) // 100, 511) | (min((r.get("fail_mv", 0) + 50) // 100, 511) << 9) | (pd << 18)
        q = 56 + 4 * g
        p[q] = (verdict << 6) | (reason & 0x3F)
        p[q + 1] = v & 0xFF; p[q + 2] = (v >> 8) & 0xFF; p[q + 3] = (v >> 16) & 0xFF
    for i, r in enumerate(ranges[:4]):
        q = 104 + 10 * i
        alg = ALGORITHMS.index(r.get("alg", "PPS (PD3.x)")) if r.get("alg") in ALGORITHMS else 4
        lo = min((r.get("lo_mv", 0) + 50) // 100, 0x1FF) | (min(r.get("max_ma", 0) // 50, 127) << 9)
        p[q] = alg; p[q + 1] = i + 1
        put16(q + 2, lo); put16(q + 4, min((r.get("hi_mv", 0) + 50) // 100, 511))
        put16(q + 6, min((r.get("max_mv", 0) + 50) // 100, 511)); put16(q + 8, min((r.get("fail_mv", 0) + 50) // 100, 511))
    for i, (key, e) in enumerate(list(j.get("epr_levels", {}).items())[:3]):
        dv = int(key.removesuffix("V")) // 100
        q = 144 + 6 * i
        put16(q, dv)
        verdict = VERDICT.index(e.get("verdict", "none"))
        reason = REASONS.index(e["reason"]) if e.get("reason") in REASONS else 0
        detail = e.get("detail", 0)
        pd = (0x20 | (detail - 128)) & 0x3F if detail >= 128 else detail & 0x3F
        v = min((e.get("max_mv", 0) + 50) // 100, 511) | (min((e.get("fail_mv", 0) + 50) // 100, 511) << 9) | (pd << 18)
        p[q + 2] = (verdict << 6) | (reason & 0x3F)
        p[q + 3] = v & 0xFF; p[q + 4] = (v >> 8) & 0xFF; p[q + 5] = (v >> 16) & 0xFF
    cb = j.get("cable")
    if cb and (cb.get("vid") or cb.get("pid") or cb.get("seq") or cb.get("kind")):
        q = 162
        put32(q, cb.get("seq", 0)); put16(q + 4, cb.get("vid", 0)); put16(q + 6, cb.get("pid", 0))
        put16(q + 8, cb.get("max_mv", 0)); put16(q + 10, cb.get("max_ma", 0))
        p[q + 12] = cb.get("kind", 0)
        flags = (1 if cb.get("observed") else 0) | (2 if cb.get("spoofed") else 0) | (4 if cb.get("self_powered") else 0)
        if cb.get("guard") in GUARDS: flags |= (GUARDS.index(cb["guard"]) + 1) << 3
        if cb.get("guard_bypassed"): flags |= 0x40
        p[q + 13] = flags
        p[q + 14] = 0  # resumed is not in the JSON
        src = 1 if j.get("oppo_svid") else 0
        if j.get("scp_ma"): src |= min(j["scp_ma"] // 50, 127) << 1
        p[q + 15] = src
    for i, u in enumerate(j.get("ufcs", [])[:4]):
        if not u.get("hi_mv"): continue
        v = min((u["lo_mv"] + 50) // 100, 255) | (min((u["hi_mv"] + 50) // 100, 511) << 8) | (min(u.get("max_ma", 0) // 50, 127) << 17)
        q = 178 + 3 * i
        p[q] = v & 0xFF; p[q + 1] = (v >> 8) & 0xFF; p[q + 2] = (v >> 16) & 0xFF
    p[190] = j.get("times", 1)
    put16(191, j.get("last_boots", 0))
    up = j.get("last_uptime_s", 0) & 0xFFFFFF
    p[193] = up & 0xFF; p[194] = (up >> 8) & 0xFF; p[195] = (up >> 16) & 0xFF
    p[33] = 8   # v8: the re-balance count (captures predate it: 0)
    p[196] = j.get("rebalanced", 0) & 0xFF
    rec = bytearray(204)
    rec[0] = 0xA6; rec[1] = 1; rec[2] = 198
    rec[4:202] = p
    rec[202] = crc16_ccitt(bytes(rec[:202])) & 0xFF
    rec[203] = (crc16_ccitt(bytes(rec[:202])) >> 8) & 0xFF
    return bytes(rec)


def load_captures(corpus_paths):
    """Card captures at the given paths (a directory is scanned for *.json / *.txt; a file is read directly):
    *.json (a card object or a list of them) -> re-encoded records; a show-all text listing -> shape stats
    only. Returns (hex lines, stats)."""
    paths = []
    for c in corpus_paths:
        if c.is_dir():
            paths += sorted(c.glob("*.json")) + sorted(c.glob("*.txt"))
        else:
            paths.append(c)
    hexes, stats = [], {"cards": 0, "pdos": [], "runs": [], "files": []}
    for f in paths:
        if f.suffix != ".json":
            continue
        try:
            data = json.loads(f.read_text())
        except Exception:
            continue
        cards = data if isinstance(data, list) else [data]
        if not cards or not isinstance(cards[0], dict) or "fp" not in cards[0]:
            continue
        stats["files"].append(f.name)
        for c in cards:
            stats["cards"] += 1
            stats["pdos"].append(c.get("pdos", 0))
            stats["runs"].append(c.get("runs", 1))
            hexes.append(card_payload_from_json(c).hex())
    for f in paths:
        if f.suffix != ".txt":
            continue
        txt = f.read_text(errors="replace")
        n = len(re.findall(r"^##### \d+", txt, re.M))
        if n:
            stats["files"].append(f.name)
            stats["showall_cards"] = n
            stats["showall_pdos"] = [int(m) for m in re.findall(r"· (\d+) PDOs", txt)]
            stats["showall_spoof"] = len(re.findall(r"SPOOF:", txt))
    return hexes, stats


# --------------------------------------------------------------------------- candidate 1: schema v2 (bit-packed)

class BitW:
    """LSB-first bit writer."""
    def __init__(self):
        self.acc = 0
        self.nbits = 0
        self.out = bytearray()
    def put(self, value: int, width: int):
        self.acc |= (value & ((1 << width) - 1)) << self.nbits
        self.nbits += width
        while self.nbits >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.nbits -= 8
    def varint(self, v: int):
        # LEB128-style, but inside the bit stream: 7 value bits, then a continuation bit.
        while v >= 128:
            self.put(v & 0x7F, 7)
            self.put(1, 1)
            v >>= 7
        self.put(v, 7)
        self.put(0, 1)
    def bytes(self) -> bytes:
        if self.nbits:
            self.out.append(self.acc & 0xFF)
            self.acc = 0
            self.nbits = 0
        return bytes(self.out)


class BitR:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0  # bit position
    def get(self, width: int) -> int:
        v = 0
        for i in range(width):
            byte = self.data[(self.pos + i) >> 3] if ((self.pos + i) >> 3) < len(self.data) else 0
            v |= ((byte >> ((self.pos + i) & 7)) & 1) << i
        self.pos += width
        return v
    def varint(self) -> int:
        v, shift = 0, 0
        while True:
            v |= self.get(7) << shift
            if not self.get(1):
                return v
            shift += 7


def pack_group(w: BitW, g: dict):
    w.put(g["verdict"], 2); w.put(g["reason"], 6); w.put(g["detail"] & 0x3F if g["detail"] < 128 else 0x20 | (g["detail"] - 128), 6)
    if g["max_dv"]:
        w.put(1, 1); w.put(g["max_dv"], 9)
    else:
        w.put(0, 1)
    if g["fail_dv"]:
        w.put(1, 1); w.put(g["fail_dv"], 9)
    else:
        w.put(0, 1)


def unpack_group(r: BitR) -> dict:
    verdict = r.get(2); reason = r.get(6); d = r.get(6)
    detail = 128 + (d & 0x1F) if d & 0x20 else d
    g = {"verdict": verdict, "reason": reason, "detail": detail, "max_dv": 0, "fail_dv": 0}
    if r.get(1): g["max_dv"] = r.get(9)
    if r.get(1): g["fail_dv"] = r.get(9)
    return g


EPR_DV = [0, 280, 360, 480]
CABLE_KIND_CODE = {0: 0, 1: 1, 2: 2, 4: 3, 0xFF: 4}
CABLE_KIND_DEC = {v: k for k, v in CABLE_KIND_CODE.items()}


def pack_card_v2(c: dict) -> bytes:
    w = BitW()
    w.put(2, 8)                       # schema 2
    w.put(c["fp"], 32); w.put(c["vid"], 16); w.put(c["pid"], 16)
    w.varint(c["card_no"]); w.varint(c["runs"]); w.varint(c["boots"]); w.varint(c["uptime_s"])
    w.put(c["fw"], 32)
    w.varint(c["handover_ds"])
    w.put(c["spec_rev"], 2); w.put(c["num_pdos"], 4); w.put(c["handover"], 3); w.put(c["state"], 4); w.put(c["reason"], 6)
    w.put(c["n_ranges"], 3); w.put(c["ev_flags"], 8)
    name = c["name"].encode()[:20]
    w.put(len(name), 5)
    for ch in name: w.put(ch, 7)
    w.put(c["pdp_w"], 8); w.put(1 if c["name_auto"] else 0, 1); w.put(c["pd_ma50"], 7)
    for g in c["groups"]:
        pack_group(w, g)
    for r in c["ranges"][:c["n_ranges"]]:
        w.put(r["alg"], 5); w.put(r["pos"], 8); w.put(r["lo_dv"], 9); w.put(r["hi_dv"], 9)
        w.put(r["max_dv"], 9); w.put(r["fail_dv"], 9); w.put(r["ma50"], 7)
    for e in c["epr"]:
        w.put(EPR_DV.index(e["dv"]) if e["dv"] in EPR_DV else 0, 2)
        pack_group(w, e)
    cb = c["cable"]
    if cb:
        w.put(1, 1)
        w.varint(cb["seq"]); w.put(cb["vid"], 16); w.put(cb["pid"], 16)
        w.put(cb["max_mv"], 16); w.put(cb["max_ma"], 16)
        w.put(CABLE_KIND_CODE.get(cb["kind"], 0), 3)
        w.put(cb["flags"], 8); w.put(cb["resumed"], 8); w.put(cb["src_flags"], 8)
    else:
        w.put(0, 1)
    ufcs = c["ufcs"][:4]
    w.put(len(ufcs), 3)
    for u in ufcs:
        w.put(u["lo_dv"], 8); w.put(u["hi_dv"], 9); w.put(u["ma50"], 7)
    w.varint(c["times"])
    if c["times"] > 1:
        w.put(c["last_boots"], 16); w.put(c["last_uptime_s"], 24)
    w.varint(c.get("rebalanced", 0))   # v8: the source's mid-run re-balance count
    w.put(c["crc16"], 16)             # the v1 record's CRC-16, kept: the integrity check is unchanged
    return w.bytes()


def unpack_card_v2(b: bytes) -> dict:
    r = BitR(b)
    c = {"schema": r.get(8)}
    c["fp"] = r.get(32); c["vid"] = r.get(16); c["pid"] = r.get(16)
    c["card_no"] = r.varint(); c["runs"] = r.varint(); c["boots"] = r.varint(); c["uptime_s"] = r.varint()
    c["fw"] = r.get(32)
    c["handover_ds"] = r.varint()
    c["spec_rev"] = r.get(2); c["num_pdos"] = r.get(4); c["handover"] = r.get(3); c["state"] = r.get(4); c["reason"] = r.get(6)
    c["n_ranges"] = r.get(3); c["ev_flags"] = r.get(8)
    n = r.get(5)
    c["name"] = bytes(r.get(7) for _ in range(n)).decode("ascii", "replace")
    c["pdp_w"] = r.get(8); c["name_auto"] = bool(r.get(1)); c["pd_ma50"] = r.get(7)
    c["groups"] = [unpack_group(r) for _ in range(12)]
    c["ranges"] = []
    for _ in range(c["n_ranges"]):
        c["ranges"].append({"alg": r.get(5), "pos": r.get(8), "lo_dv": r.get(9), "hi_dv": r.get(9),
                            "max_dv": r.get(9), "fail_dv": r.get(9), "ma50": r.get(7)})
    c["epr"] = []
    for _ in range(3):
        dv = EPR_DV[r.get(2)]
        c["epr"].append({"dv": dv, **unpack_group(r)})
    if r.get(1):
        c["cable"] = {"seq": r.varint(), "vid": r.get(16), "pid": r.get(16), "max_mv": r.get(16), "max_ma": r.get(16),
                      "kind": CABLE_KIND_DEC[r.get(3)], "flags": r.get(8), "resumed": r.get(8), "src_flags": r.get(8)}
    else:
        c["cable"] = None
    c["ufcs"] = []
    for _ in range(r.get(3)):
        c["ufcs"].append({"lo_dv": r.get(8), "hi_dv": r.get(9), "ma50": r.get(7)})
    c["times"] = r.varint()
    c["last_boots"] = r.get(16) if c["times"] > 1 else 0
    c["last_uptime_s"] = r.get(24) if c["times"] > 1 else 0
    c["rebalanced"] = r.varint() if len(b) * 8 - r.pos > 16 else 0   # v8 (absent in records packed before it)
    c["crc16"] = r.get(16)
    return c


SVDM_MASK = 0xFFFF8000 | (3 << 6) | 0x1F


def parse_emk(rec: bytes) -> dict:
    assert len(rec) >= 91
    d = {"flags": rec[0], "name": rec[1:25].split(b"\0")[0].decode("ascii", "replace"),
         "emrec": {"name": rec[25:45].split(b"\0")[0].decode("ascii", "replace"), "spec_rev": rec[45],
                   "vdos": [u32(rec, 25 + 21 + 4 * i) for i in range(5)], "checksum": rec[25 + 65]},
         "quirks": None}
    if rec[0] & 1:
        q = rec[91:]
        assert len(q) == 3 + 52 * q[2]
        rows = []
        for i in range(q[2]):
            o = 3 + 52 * i
            rows.append({"sop": q[o], "kind": q[o + 1], "nreq": q[o + 2], "nrep": q[o + 3],
                         "hdr_val": u32(q, o + 4), "hdr_mask": u32(q, o + 8),
                         "req": [(u32(q, o + 12 + 4 * k), u32(q, o + 20 + 4 * k)) for k in range(q[o + 2])],
                         "rep": [u32(q, o + 28 + 4 * k) for k in range(q[o + 3])]})
        d["quirks"] = {"flags": q[0], "unmatched": q[1], "rows": rows}
    else:
        assert len(rec) == 91
    return d


def pack_emk_v2(e: dict) -> bytes:
    w = BitW()
    w.put(2, 8)
    name = e["name"].encode()[:24]
    w.put(len(name), 5)
    for ch in name: w.put(ch, 7)
    w.put(e["flags"], 8)
    r = e["emrec"]
    nm = r["name"].encode()[:20]
    w.put(len(nm), 5)
    for ch in nm: w.put(ch, 7)
    w.put(r["spec_rev"], 2)
    for v in r["vdos"]: w.put(v, 32)
    w.put(r["checksum"], 8)
    q = e["quirks"]
    if q:
        w.put(1, 1)
        w.put(q["flags"], 2); w.put(q["unmatched"], 2); w.put(len(q["rows"]), 4)
        for row in q["rows"]:
            w.put(row["sop"], 2); w.put(row["kind"], 2); w.put(row["nreq"], 2); w.put(row["nrep"], 3)
            full = row["hdr_mask"] == SVDM_MASK
            w.put(1 if full else 0, 1)
            w.put(row["hdr_val"], 32)
            if not full: w.put(row["hdr_mask"], 32)
            for val, mask in row["req"]:
                w.put(val, 32); w.put(mask, 32)
            for rep in row["rep"]:
                w.put(rep, 32)
    else:
        w.put(0, 1)
    return w.bytes()


def unpack_emk_v2(b: bytes) -> dict:
    r = BitR(b)
    e = {"schema": r.get(8)}
    n = r.get(5)
    e["name"] = bytes(r.get(7) for _ in range(n)).decode("ascii", "replace")
    e["flags"] = r.get(8)
    n = r.get(5)
    nm = bytes(r.get(7) for _ in range(n)).decode("ascii", "replace")
    e["emrec"] = {"name": nm, "spec_rev": r.get(2), "vdos": [r.get(32) for _ in range(5)], "checksum": r.get(8)}
    if r.get(1):
        rows = []
        flags = r.get(2); unmatched = r.get(2); count = r.get(4)
        for _ in range(count):
            sop = r.get(2); kind = r.get(2); nreq = r.get(2); nrep = r.get(3)
            full = r.get(1)
            hdr_val = r.get(32)
            hdr_mask = SVDM_MASK if full else r.get(32)
            req = [(r.get(32), r.get(32)) for _ in range(nreq)]
            rep = [r.get(32) for _ in range(nrep)]
            rows.append({"sop": sop, "kind": kind, "nreq": nreq, "nrep": nrep,
                         "hdr_val": hdr_val, "hdr_mask": hdr_mask, "req": req, "rep": rep})
        e["quirks"] = {"flags": flags, "unmatched": unmatched, "rows": rows}
    else:
        e["quirks"] = None
    return e


# --------------------------------------------------------------------------- candidate 2: entropy coding

def lzss_encode(data: bytes, window_bits: int, look_bits: int, dictionary: bytes) -> bytes:
    """Greedy LZSS, LSB-first bit stream: 0 = literal (8 bits), 1 = match (window_bits offset, look_bits
    length-minus-3). The window starts with `dictionary` (a preset of common patterns), so early matches may
    reference it. min match 3, max match 2^look_bits + 2."""
    w_size = 1 << window_bits
    max_len = (1 << look_bits) + 2
    win = bytearray(dictionary[-w_size:]) if dictionary else bytearray()
    out = BitW()
    i = 0
    n = len(data)
    while i < n:
        best_len, best_off = 0, 0
        start = max(0, len(win) - w_size)
        max_len_i = min(max_len, n - i)
        if max_len_i >= 3:
            # greedy search, newest first (small offsets win ties)
            j = len(win) - 3
            while j >= start:
                l = 0
                while l < max_len_i and i + l < n:
                    src = win[j + l] if j + l < len(win) else data[i + j + l - len(win)]  # overlapping match
                    if src != data[i + l]:
                        break
                    l += 1
                if l > best_len:
                    best_len, best_off = l, len(win) - j
                    if l == max_len_i:
                        break
                j -= 1
        if best_len >= 3:
            out.put(1, 1)
            out.put(best_off, window_bits)
            out.put(best_len - 3, look_bits)
            win += data[i:i + best_len]
            i += best_len
        else:
            out.put(0, 1)
            out.put(data[i], 8)
            win.append(data[i])
            i += 1
        if len(win) > w_size + max_len:
            del win[:len(win) - w_size - max_len]
    return out.bytes()


def lzss_decode(comp: bytes, window_bits: int, look_bits: int, dictionary: bytes, out_len: int) -> bytes:
    w_size = 1 << window_bits
    win = bytearray(dictionary[-w_size:]) if dictionary else bytearray()
    r = BitR(comp)
    out = bytearray()
    while len(out) < out_len:
        if r.get(1):
            off = r.get(window_bits)
            length = r.get(look_bits) + 3
            for _ in range(length):
                b = win[len(win) - off]
                out.append(b)
                win.append(b)
        else:
            b = r.get(8)
            out.append(b)
            win.append(b)
        if len(win) > w_size + 64:
            del win[:len(win) - w_size - 64]
    return bytes(out)


def build_huffman(freqs: list[int]):
    """Canonical Huffman over 256 symbols. Returns (code lengths, codes) or None."""
    heap = [(f, i, [i]) for i, f in enumerate(freqs) if f > 0]
    if len(heap) == 1:
        return [1 if i == heap[0][1] else 0 for i in range(256)], None
    heapq.heapify(heap)
    lengths = [0] * 256
    nodes = []
    while len(heap) > 1:
        f1, _, s1 = heapq.heappop(heap)
        f2, _, s2 = heapq.heappop(heap)
        for s in s1 + s2:
            lengths[s] += 1
        heapq.heappush(heap, (f1 + f2, min(s1[0], s2[0]), s1 + s2))
    # canonical codes
    bl_count = [0] * 33
    for l in lengths:
        if l: bl_count[l] += 1
    code = 0
    next_code = [0] * 33
    for bits in range(1, 33):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    codes = [0] * 256
    for s in range(256):
        l = lengths[s]
        if l:
            codes[s] = next_code[l]
            next_code[l] += 1
    return lengths, codes


def huffman_encode(data: bytes, lengths, codes) -> bytes:
    w = BitW()
    for b in data:
        l = lengths[b]
        w.put(codes[b], l)
    return w.bytes()


def huffman_decode(comp: bytes, lengths, out_len: int) -> bytes:
    # decode bit by bit (MSB-first codes, as canonical Huffman codes are read)
    r = BitR(comp)
    # build a map (length, code) -> symbol
    m = {}
    bl_count = [0] * 33
    for l in lengths:
        if l: bl_count[l] += 1
    code = 0
    next_code = [0] * 33
    for bits in range(1, 33):
        code = (code + bl_count[bits - 1]) << 1
        next_code[bits] = code
    for s in range(256):
        l = lengths[s]
        if l:
            m[(l, next_code[l])] = s
            next_code[l] += 1
    out = bytearray()
    while len(out) < out_len:
        l = 0
        c = 0
        while True:
            c = (c << 1) | r.get(1)
            l += 1
            if (l, c) in m:
                out.append(m[(l, c)])
                break
    return bytes(out)


def rans_tables(freqs: list[int], scale: int):
    """Quantize to sum `scale` (power of two), cumulative and slot->symbol. Returns (freq, cum, slot)."""
    total = sum(freqs)
    freq = [max(1, round(f * scale / total)) for f in freqs]
    # renormalize to exactly `scale`
    diff = scale - sum(freq)
    order = sorted(range(256), key=lambda s: -freqs[s])
    i = 0
    while diff != 0:
        s = order[i % 256]
        if diff > 0 and freq[s] < scale:
            freq[s] += 1; diff -= 1
        elif diff < 0 and freq[s] > 1:
            freq[s] -= 1; diff += 1
        i += 1
    cum = [0] * 256
    c = 0
    for s in range(256):
        cum[s] = c
        c += freq[s]
    assert c == scale
    slot = [0] * scale
    for s in range(256):
        for k in range(freq[s]):
            slot[cum[s] + k] = s
    return freq, cum, slot


def rans_encode(data: bytes, freq, cum, scale: int) -> int:
    """Big-int rANS (the bit count equals what a renormalizing 32-bit encoder emits). Returns the state."""
    x = scale
    for s in reversed(data):
        x = (x // freq[s]) * scale + cum[s] + (x % freq[s])
    return x


def rans_decode(x: int, freq, cum, slot, scale: int, out_len: int) -> bytes:
    out = bytearray()
    for _ in range(out_len):
        sl = x % scale
        s = slot[sl]
        out.append(s)
        x = (x // scale) * freq[s] + sl - cum[s]
    return bytes(out)


# --------------------------------------------------------------------------- candidate 3-5: capacity & geometry

def build_capacity(binpath: Path):
    subprocess.run(["gcc", "-O2", "-DNDEBUG", "-w", "-I", str(ROOT / "third_party/qrcodegen"),
                    str(ROOT / "tools/qr_capacity.c"), str(ROOT / QR_C), "-o", str(binpath)], check=True)


def run_capacity(binpath: Path, vmin: int, vmax: int, prefix: str):
    out = sh([str(binpath), str(vmin), str(vmax), prefix])
    table = {}
    for line in out.splitlines():
        if line.startswith("#") or not line.strip():
            continue
        f = line.split()
        v = int(f[0][1:])
        table[v] = {"modules": int(f[1].split("=")[1]), "byte": int(f[2].split("=")[1]),
                    "alnum": int(f[3].split("=")[1]), "mixed_chars": int(f[4].split("=")[1]),
                    "mixed_fits": int(f[5].split("=")[1])}
    return table


def b64_len(n: int) -> int:
    return (n * 4 + 2) // 3


def max_record_for(chars: int, head: int) -> int:
    n = 0
    while head + b64_len(n + 1) <= chars:
        n += 1
    return n


def geometry(cap: dict, head: int):
    """Panel geometry: for each (area side, text line under the code, px/module, quiet modules) the max
    version that fits and its byte-L capacity / max record. Mirrors qrview::plan_in:
    px = min(area, area - text_h) // (modules + 2*quiet), accepted when px >= requested."""
    rows = []
    for area in (240, 224):
        for text_h in (14, 0):
            for px in (3, 2):
                for quiet in (4, 2, 1):
                    cells = min(area, area - text_h) // px
                    max_mod = cells - 2 * quiet
                    v = (max_mod - 17) // 4 if max_mod >= 21 else 0
                    if v < 1 or v > max(cap):
                        rows.append((area, text_h, px, quiet, None, 0, 0, 0))
                        continue
                    box = (4 * v + 17 + 2 * quiet) * px
                    ok = box <= min(area, area - text_h)
                    rows.append((area, text_h, px, quiet, v if ok else None, cap[v]["byte"] if ok else 0,
                                 max_record_for(cap[v]["byte"], head) if ok else 0, box))
    return rows


# --------------------------------------------------------------------------- the schema v2 check: firmware encoder vs this prototype

def v2_check(work: Path, quiet: bool):
    """Build the host test_qrshare_link, dump its corpus rows (name, type, v1 hex, firmware v2 hex, v2 link) and assert:
    the firmware's v2 bytes equal pack_card_v2 / pack_emk_v2 here, byte for byte, and decode_link() of the firmware link
    gives the same JSON as the v1 record decoded by the v1 decoders."""
    host = work / "host-build"
    binpath = host / "test_qrshare_link"
    subprocess.run(["make", "-C", str(ROOT), "-j8", f"HOST_DIR={host}", str(binpath)], check=True,
                   capture_output=True)
    dump = work / "corpus-v2.tsv"
    env = dict(os.environ, K2_QRCORPUS_DUMP=str(dump))
    subprocess.run([str(binpath)], check=True, capture_output=True, cwd=str(ROOT), env=env)
    rows = [ln.rstrip("\n").split("\t") for ln in open(dump) if not ln.startswith("#") and ln.strip()]
    if not rows:
        print("v2-check: no corpus rows dumped", file=sys.stderr)
        return 1
    bad = 0
    for name, typ, v1hex, v2hex, link in rows:
        typ = int(typ)
        v1, fw = bytes.fromhex(v1hex), bytes.fromhex(v2hex)
        v1_json = None
        if typ == 4 and len(v1) in (202, 204):
            proto = pack_card_v2(parse_card(v1))
            try:
                v1_json = k2_qr.decode_card(v1)
            except Exception:
                pass   # a synthetic all-limits record the strict v1 decoder refuses: compare bytes only
        elif typ == 2 and len(v1) >= 91:
            proto = pack_emk_v2(parse_emk(v1))
            v1_json = k2_qr.decode_emarker(v1)
        else:
            continue
        if fw != proto:
            bad += 1
            print(f"v2-check MISMATCH (bytes) {name}: firmware {len(fw)} B vs prototype {len(proto)} B")
            continue
        if v1_json is None:
            # byte-level round trip only: the v2 record expands back to the exact v1 record
            expand = k2_qr.expand_card_v2 if typ == 4 else k2_qr.expand_emk_v2
            if expand(fw) != v1:
                bad += 1
                print(f"v2-check MISMATCH (expand) {name}")
                continue
            if not quiet:
                print(f"  {name:<14} v1 {len(v1):>3} B -> v2 {len(fw):>3} B, link {len(link):>3} chars — bytes identical, expands to v1")
            continue
        d = k2_qr.decode_link(link)
        if d["version"] != 2 or bytes.fromhex(d["record_hex"]) != fw:
            bad += 1
            print(f"v2-check MISMATCH (link) {name}")
            continue
        if d["record"] != v1_json:
            bad += 1
            print(f"v2-check MISMATCH (json) {name}")
            continue
        head = len(k2_qr.LINK_BASE) + 3
        if not quiet:
            print(f"  {name:<14} v1 {len(v1):>3} B -> v2 {len(fw):>3} B, link {len(link):>3} chars "
                  f"({100 * len(link) / 321:.1f}% of v11) — bytes identical, JSON equal")
    print(f"v2-check: {len(rows)} corpus rows, {bad} mismatches")
    return 1 if bad else 0


# --------------------------------------------------------------------------- report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", type=Path, nargs="+", default=None,
                    help="card captures: files or directories (a directory is scanned for *.json / *.txt)")
    ap.add_argument("--markdown", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--v2-check", action="store_true",
                    help="run the firmware v2 encoder over the corpus (host build of test_qrshare_link) and assert it is "
                         "byte-identical to the prototype here and decodes to the v1 JSON")
    args = ap.parse_args()

    work = Path(tempfile.gettempdir()) / "k2-qr-corpus"
    work.mkdir(parents=True, exist_ok=True)
    driver = work / "qr_corpus_driver"
    capacity = work / "qr_capacity"
    build_driver(driver)
    build_capacity(capacity)

    extra = None
    cap_stats = None
    if args.corpus:
        hexes, cap_stats = load_captures(args.corpus)
        if hexes:
            extra = work / "captures.hex"
            extra.write_text("\n".join(hexes) + "\n")

    if args.v2_check:
        return v2_check(work, args.quiet)

    header, records = run_driver(driver, extra)
    kbase = header["kBase"]
    head = len(kbase) + 3
    v11 = int(header["kQrBytesV11"])
    kmax_record = int(header["kMaxRecord"])

    cap = run_capacity(capacity, 11, 25, kbase + "1t.")

    # ---- per-record table -------------------------------------------------
    for r in records:
        r["kind"] = TYPE_NAMES.get(r["type"], "?")
        pl, pk = KIND_LETTERS.get(r["type"], ("?", "?"))
        r["letter_plain"], r["letter_packed"] = pl, pk
        r["fits_plain"] = r["plain_len"] != 0 and r["plain_len"] <= v11
        r["fits_packed"] = r["packed_len"] != 0 and r["packed_len"] <= v11

    cards = [r for r in records if r["type"] == 4]
    emks = [r for r in records if r["type"] == 2]
    worst_card_plain = max(cards, key=lambda r: r["plain_len"]) if cards else None
    worst_card_packed = max((c for c in cards if c["packed_len"]), key=lambda r: r["packed_len"], default=None)

    # ---- candidate 1: schema v2 over the corpus ----------------------------
    v2_rows = []
    for r in records:
        if r["type"] == 4 and len(r["raw"]) in (202, 204):
            c = parse_card(r["raw"])
            v2 = pack_card_v2(c)
            back = unpack_card_v2(v2)
            c2 = {k: v for k, v in c.items() if k != "format"}
            b2 = {k: v for k, v in back.items() if k not in ("schema", "format")}
            zero_range = {"alg": 0, "pos": 0, "lo_dv": 0, "hi_dv": 0, "max_dv": 0, "fail_dv": 0, "ma50": 0}
            b2["ranges"] = b2["ranges"] + [zero_range] * (4 - len(b2["ranges"]))   # v2 stores the used slots only
            assert c2 == b2, f"card v2 round-trip failed for {r['name']}"
            r["v2_card"] = len(v2)
            v2_rows.append((r["name"], len(r["raw"]), len(v2)))
        elif r["type"] == 2 and len(r["raw"]) >= 91 and r["raw"][0] != 2:
            # (a raw record starting with the schema byte 2 is a v2 record from the regenerated qrlink.tsv, not a v1 one)
            e = parse_emk(r["raw"])
            v2 = pack_emk_v2(e)
            back = {k: v for k, v in unpack_emk_v2(v2).items() if k != "schema"}
            assert e == back, f"emk v2 round-trip failed for {r['name']}"
            r["v2_emk"] = len(v2)
            v2_rows.append((r["name"], len(r["raw"]), len(v2)))
        else:
            r["v2_card"] = r["v2_emk"] = None

    v13_chars = cap[13]["byte"]
    v11_chars = cap[11]["byte"]
    assert v11_chars == v11, f"v11 byte-L capacity {v11_chars} != {v11}"

    # ---- candidate 2: entropy coders --------------------------------------
    corpus_bytes = b"".join(r["raw"] for r in records)
    freqs = [0] * 256
    for b in corpus_bytes:
        freqs[b] += 1
    lens, codes = build_huffman([f + 1 for f in freqs])
    rfreq, rcum, rslot = rans_tables(freqs, 4096)
    # preset dictionary for the LZSS: the constant heads and zero runs the records share
    dict_bytes = bytes([0xA6, 0x01, 0xC4, 0x00]) + bytes(60) + bytes([0x4B, 0x32, 0x42, 0x31]) + bytes(60) + \
        bytes.fromhex("ff00a041") + bytes(40)
    lz_params = [(8, 4), (11, 4), (12, 4)]
    for r in records:
        raw = r["raw"]
        r["huff"] = len(huffman_encode(raw, lens, codes))
        x = rans_encode(raw, rfreq, rcum, 4096)
        assert rans_decode(x, rfreq, rcum, rslot, 4096, len(raw)) == raw
        r["rans"] = (x.bit_length() + 7) // 8
        r["lzss"] = {f"w{wb}l{lb}": len(lzss_encode(raw, wb, lb, dict_bytes)) for wb, lb in lz_params}
        for wb, lb in lz_params:
            assert lzss_decode(lzss_encode(raw, wb, lb, dict_bytes), wb, lb, dict_bytes, len(raw)) == raw

    # ---- geometry ----------------------------------------------------------
    geo = geometry(cap, head)

    # ---- print -------------------------------------------------------------
    md = args.markdown
    def p(s=""):
        if not args.quiet:
            print(s)

    if md:
        p("## Measurement table (corpus records)")
        p()
        p("| record | kind | raw B | LZ4 B | pack B | link plain (letter) | link packed (letter) | fits v11 plain | fits v11 packed | v2 B | v2 link @v11 | v2 link @v13 | Huffman B | rANS B | LZSS w11l4 B |")
        p("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    else:
        p(f"corpus: {len(records)} records; kBase={kbase} (head {head} chars); v11-L byte = {v11} chars; "
          f"kMaxRecord = {kmax_record} B; v13-L byte = {v13_chars} chars")
        p(f"{'record':<22}{'kind':<8}{'raw':>5}{'lz4':>5}{'pack':>5}{'link_p':>8}{'link_z':>8}{'fit_p':>6}{'fit_z':>6}"
          f"{'v2':>5}{'v2l11':>7}{'v2l13':>7}{'huff':>6}{'rans':>6}{'lzss':>6}")
    for r in records:
        v2 = r.get("v2_card") if r.get("v2_card") is not None else r.get("v2_emk")
        v2l11 = head + b64_len(v2) if v2 else None
        v2l13 = head + b64_len(v2) if v2 else None
        if md:
            p(f"| {r['name']} | {r['kind']} | {len(r['raw'])} | {r['lz4']} | {r['pack']} | "
              f"{r['plain_len']} ({r['letter_plain']}) | {r['packed_len'] or '-'} ({r['letter_packed']}) | "
              f"{'yes' if r['fits_plain'] else 'NO'} | {'yes' if r['fits_packed'] else 'NO'} | {v2 or '-'} | "
              f"{v2l11 or '-'} | {v2l13 or '-'} | {r['huff']} | {r['rans']} | {r['lzss']['w11l4']} |")
        else:
            p(f"{r['name']:<22}{r['kind']:<8}{len(r['raw']):>5}{r['lz4']:>5}{r['pack']:>5}"
              f"{r['plain_len']:>8}{r['packed_len']:>8}{str(r['fits_plain']):>6}{str(r['fits_packed']):>6}"
              f"{str(v2):>5}{str(v2l11):>7}{str(v2l13):>7}{r['huff']:>6}{r['rans']:>6}{r['lzss']['w11l4']:>6}")

    if md:
        p()
        p("## Worst cases")
        p()
        p(f"- worst card link, plain (kind `{worst_card_plain['letter_plain']}`): **{worst_card_plain['plain_len']} of {v11} chars** "
          f"({worst_card_plain['name']}, {len(worst_card_plain['raw'])} B record); spare {100 * (v11 - worst_card_plain['plain_len']) / v11:.1f}%")
        if worst_card_packed:
            p(f"- worst card link, packed (kind `{worst_card_packed['letter_packed']}`): {worst_card_packed['packed_len']} of {v11} chars "
              f"({worst_card_packed['name']}, {worst_card_packed['pack']} B block)")
        p(f"- v11-L byte capacity from the vendored generator: {cap[11]['byte']} chars (kQrBytesV11 = {v11}); "
          f"v12 = {cap[12]['byte']}, v13 = {cap[13]['byte']}, v14 = {cap[14]['byte']}")
        p(f"- max record bytes for a link of at most C chars: v11 -> {max_record_for(cap[11]['byte'], head)} B, "
          f"v13 -> {max_record_for(cap[13]['byte'], head)} B, v14 -> {max_record_for(cap[14]['byte'], head)} B")
        p()
        p("## Capacity (vendored qrcodegen, level L, measured by binary search over encodeSegments)")
        p()
        p("| version | modules | byte-L chars | alnum-L chars | mixed byte(prefix)+alnum43(payload) chars |")
        p("|---|---|---|---|---|")
        for v in sorted(cap):
            p(f"| {v} | {cap[v]['modules']} | {cap[v]['byte']} | {cap[v]['alnum']} | {cap[v]['mixed_chars']} |")
        p()
        p("Mixed segmentation: prefix = kBase + `1t.` (50 byte-mode chars), payload = base-43 over the URL-safe QR "
          "alphanumeric subset (0-9 A-Z $ * + - . / :). Max record bytes per version: "
          + ", ".join(f"v{v}: {math.floor(cap[v]['mixed_chars'] * math.log2(43) / 8)} B" for v in range(11, 26)))
        p()
        p("All-alphanumeric link is INFEASIBLE: the published page path is case-sensitive, so an all-uppercase "
          f"address does not resolve; v11-L alnum capacity would be {cap[11]['alnum']} chars, noted for reference only.")
        p()
        p("## Geometry (box = (modules + 2*quiet) * px must fit min(area, area - text_h); mirrors qrview::plan_in)")
        p()
        p("| area px | text line | px/module | quiet | max version | byte-L chars | max record B | box px |")
        p("|---|---|---|---|---|---|---|---|")
        for area, text_h, px, quiet, v, chars, rec, box in geo:
            p(f"| {area} | {text_h or 'none'} | {px} | {quiet} | {f'v{v}' if v else 'none'} | {chars} | {rec} | {box} |")
        p()
        p("Handoff arithmetic check: 240 px at 2 px/module with a 2-module quiet zone holds (117+4)*2 = 242 px for "
          "v25, which does NOT fit; the true max is v24 (113+4)*2 = 234 px. v25 at 2 px needs quiet <= 1 module "
          "((117+2)*2 = 238 px), i.e. a 2 px quiet zone on the panel.")
    else:
        p()
        p(f"worst card link plain: {worst_card_plain['plain_len']} of {v11} chars ({worst_card_plain['name']}, "
          f"{len(worst_card_plain['raw'])} B) spare {100 * (v11 - worst_card_plain['plain_len']) / v11:.1f}%")
        if worst_card_packed:
            p(f"worst card link packed: {worst_card_packed['packed_len']} of {v11} ({worst_card_packed['name']}, "
              f"{worst_card_packed['pack']} B)")
        p(f"capacities byte-L: v11={cap[11]['byte']} v12={cap[12]['byte']} v13={cap[13]['byte']} v14={cap[14]['byte']} "
          f"v20={cap[20]['byte']} v25={cap[25]['byte']}; alnum-L v11={cap[11]['alnum']}")
        p(f"mixed (50-char byte prefix + alnum43 payload) chars: " +
          ", ".join(f"v{v}={cap[v]['mixed_chars']}" for v in range(11, 26)))
        p("geometry (area, text_h, px, quiet -> v, chars, record B, box):")
        for area, text_h, px, quiet, v, chars, rec, box in geo:
            p(f"  area={area} text_h={text_h} px={px} quiet={quiet} -> {f'v{v}' if v else 'none'} "
              f"chars={chars} record={rec}B box={box}")

    if cap_stats:
        p()
        p(f"captures in {[str(c) for c in args.corpus]}: {cap_stats}")

    # worst-case v2 summary numbers for the report
    v2_cards = [(r["name"], r["v2_card"]) for r in records if r.get("v2_card")]
    v2_emks = [(r["name"], r["v2_emk"]) for r in records if r.get("v2_emk")]
    if v2_cards:
        w = max(v2_cards, key=lambda t: t[1])
        p()
        p(f"v2 card: worst {w[0]} = {w[1]} B -> link {head + b64_len(w[1])} chars at v11 ({100 * (head + b64_len(w[1])) / v11:.1f}% of {v11}), "
          f"{head + b64_len(w[1])} at v13 vs {v13_chars}; max record for v13 = {max_record_for(v13_chars, head)} B")
    if v2_emks:
        w = max(v2_emks, key=lambda t: t[1])
        p(f"v2 emk: worst {w[0]} = {w[1]} B -> link {head + b64_len(w[1])} chars at v11 ({100 * (head + b64_len(w[1])) / v11:.1f}% of {v11})")
        r8 = next((r for r in records if r["name"] == "emk_rows8"), None)
        if r8 and r8.get("v2_emk"):
            p(f"emk_rows8 (510 B raw, {r8['lz4']} B LZ4, no link today): v2 = {r8['v2_emk']} B -> "
              f"link {head + b64_len(r8['v2_emk'])} chars, fits v11: {head + b64_len(r8['v2_emk']) <= v11}")
    ent = max(records, key=lambda r: r["rans"])
    p(f"entropy worst rANS: {ent['name']} {ent['rans']} B (raw {len(ent['raw'])}); worst huffman: "
      f"{max(r['huff'] for r in records)} B; worst lzss w11l4: {max(r['lzss']['w11l4'] for r in records)} B")
    return 0


if __name__ == "__main__":
    sys.exit(main())
