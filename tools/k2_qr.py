#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Decode and encode the K2 QR share records (docs/QR_SHARE.md), with no device and no USB.

  k2_qr.py decode TEXT...            parts in any order ("-" reads stdin lines, "@file" reads a file's lines); JSON out
                                     (a LOG record: --lines prints "t=+123ms [TAG] message", ids resolved with the
                                     tools/logdb.json of the record's git commit, the working tree's with a warning)
  k2_qr.py decode-image PNG...       scan QR images (pyzbar, else the zbarimg program), then decode as above
  k2_qr.py encode --type emk --hex H [--schema N]    one K2:... text per line (a record as hex, text or a file)

Exit codes: 0 ok, 1 incomplete set (the JSON names the missing parts), 2 bad input, 3 decode-image has no QR reader installed.
Python side of the frame format in include/k2/ui/qrshare.hpp (an independent implementation, cross-checked by the golden
vectors in tests/fixtures/qrshare.tsv).
"""
import argparse
import base64
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import urllib.parse
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logdb  # noqa: E402
import loop_probe_rec  # noqa: E402
import battery_rec  # noqa: E402
import pd_trace  # noqa: E402
import report_card  # noqa: E402

ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
TYPES = {1: "BI", 2: "EMK", 3: "PDC", 4: "CARD", 5: "ID", 6: "LOG", 7: "LOOP", 8: "BATT"}
TAGS = {v: k for k, v in TYPES.items()}
PACKED = 0x80   # bit 7 of the type byte: the record is an LZ4 block (inflated after the parts are joined)
MAX_UNPACKED = 1 << 16
HEADER, CRC = 12, 4
PART_PAYLOAD, MAX_PARTS, MAX_RECORD = 330, 4, 1320


class QrError(ValueError):
    pass


# ---- base45 (RFC 9285 alphabet and grouping) ----
def b45encode(data):
    out = []
    for i in range(0, len(data), 2):
        if i + 1 < len(data):
            n = data[i] * 256 + data[i + 1]
            out += [ALPHABET[n % 45], ALPHABET[n // 45 % 45], ALPHABET[n // 2025]]
        else:
            out += [ALPHABET[data[i] % 45], ALPHABET[data[i] // 45]]
    return "".join(out)


def b45decode(text):
    try:
        d = [ALPHABET.index(c) for c in text]
    except ValueError:
        raise QrError("character outside the QR alphanumeric set")
    out = bytearray()
    for i in range(0, len(d), 3):
        g = d[i:i + 3]
        if len(g) == 3:
            v = g[0] + 45 * g[1] + 2025 * g[2]
            if v > 0xFFFF:
                raise QrError("base45 group out of range")
            out += bytes([v >> 8, v & 255])
        elif len(g) == 2:
            v = g[0] + 45 * g[1]
            if v > 0xFF:
                raise QrError("base45 group out of range")
            out.append(v)
        else:
            raise QrError("base45 text has a stray character")
    return bytes(out)


# ---- frame ----
def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def chunk(rec_len, count):
    return (rec_len + count - 1) // count


def lz4_decompress(data, max_out=MAX_UNPACKED):
    """LZ4 block (the standard format, what k2/core/lz.hpp writes) -> bytes. QrError on a corrupt block or more than max_out."""
    out = bytearray()
    i, n = 0, len(data)
    if n == 0:
        raise QrError("empty LZ4 block")

    def ext(v):
        nonlocal i
        while True:
            if i >= n:
                raise QrError("LZ4 block cut short")
            v += data[i]
            i += 1
            if data[i - 1] != 255:
                return v

    while i < n:
        tok = data[i]
        i += 1
        lit = ext(15) if tok >> 4 == 15 else tok >> 4
        if i + lit > n:
            raise QrError("LZ4 literals run past the block")
        out += data[i:i + lit]
        i += lit
        if i == n:
            break
        if i + 2 > n:
            raise QrError("LZ4 block cut short")
        off = data[i] | data[i + 1] << 8
        i += 2
        m = (ext(15) if tok & 15 == 15 else tok & 15) + 4
        if off == 0 or off > len(out):
            raise QrError("LZ4 match offset outside the output")
        for _ in range(m):
            out.append(out[-off])
        if len(out) > max_out:
            raise QrError("LZ4 output longer than allowed")
    if len(out) > max_out:
        raise QrError("LZ4 output longer than allowed")
    return bytes(out)


def encode(type_, schema, record, packed=False):
    """Record bytes -> list of part texts (the firmware's split: ceil(len / 330) equal slices). `record` is sent as
    given; packed only sets the type byte's LZ4 flag (the caller made the block)."""
    record = bytes(record)
    if type_ not in TYPES:
        raise QrError(f"unknown type {type_}")
    if not 1 <= len(record) <= MAX_RECORD:
        raise QrError(f"record is {len(record)} bytes, 1 to {MAX_RECORD} allowed")
    n = (len(record) + PART_PAYLOAD - 1) // PART_PAYLOAD
    s = chunk(len(record), n)
    texts = []
    for i in range(n):
        payload = record[i * s:(i + 1) * s]
        f = b"K2" + bytes([type_ | (PACKED if packed else 0), schema, i, n]) + struct.pack("<HI", len(record), crc32(record)) + payload
        f += struct.pack("<I", crc32(f))
        texts.append(f"K2:{TYPES[type_]}{schema}:{b45encode(f)}")
    return texts


def parse_part(text):
    """One part text -> dict (type, schema, index, count, record_len, record_crc, payload)."""
    # A space is a base45 digit (index 36) but can never end a part: the last base45 character of a
    # frame is at most index 32 (65535 // 2025; 255 // 45 = 5 when the frame length is odd).
    text = text.strip("\r\n")   # so only line ends (scanner/copy artifacts) are trimmed
    if not text.startswith("K2:"):
        raise QrError("not a K2 share part (no K2: prefix)")
    try:
        head, body = text[3:].split(":", 1)
    except ValueError:
        raise QrError("part has no second colon")
    tag = head.rstrip("0123456789")
    digits = head[len(tag):]
    if tag not in TAGS or not digits or len(digits) > 3:
        raise QrError(f"bad tag/schema {head!r}")
    f = b45decode(body)
    if len(f) < HEADER + CRC + 1:
        raise QrError("part is too short")
    if f[:2] != b"K2":
        raise QrError("bad frame magic")
    if struct.unpack_from("<I", f, len(f) - 4)[0] != crc32(f[:-4]):
        raise QrError("part CRC mismatch (damaged scan)")
    type_, schema, index, count, rec_len, rec_crc = struct.unpack_from("<BBBBHI", f, 2)
    packed = bool(type_ & PACKED)
    type_ &= ~PACKED
    if type_ != TAGS[tag] or schema != int(digits):
        raise QrError("prefix does not match the frame")
    if not 1 <= count <= MAX_PARTS or index >= count or not 1 <= rec_len <= MAX_RECORD:
        raise QrError("frame header out of range")
    n = (rec_len + PART_PAYLOAD - 1) // PART_PAYLOAD
    if n != count:
        raise QrError("part count does not fit the record length")
    s = chunk(rec_len, count)
    want = s if index + 1 < count else rec_len - s * (count - 1)
    payload = f[HEADER:-4]
    if len(payload) != want:
        raise QrError("part payload length does not fit its index")
    return {"type": type_, "schema": schema, "packed": packed, "index": index, "count": count, "record_len": rec_len,
            "record_crc": rec_crc, "payload": payload}


def assemble(texts):
    """Part texts in any order -> (info dict, record bytes or None). Raises QrError on a bad or foreign part."""
    parts = {}
    first = None
    for t in texts:
        p = parse_part(t)
        key = (p["type"], p["schema"], p["packed"], p["count"], p["record_len"], p["record_crc"])
        if first is None:
            first = key
        elif key != first:
            raise QrError("parts belong to different records")
        if p["index"] in parts and parts[p["index"]] != p["payload"]:
            raise QrError(f"part {p['index'] + 1} appears twice with different data")
        parts[p["index"]] = p["payload"]
    if first is None:
        raise QrError("no parts given")
    type_, schema, packed, count, rec_len, rec_crc = first
    info = {"type": TYPES[type_], "schema": schema, "packed": packed, "parts": count, "have": sorted(i + 1 for i in parts),
            "record_len": rec_len}
    missing = [i + 1 for i in range(count) if i not in parts]
    if missing:
        info.update(complete=False, missing=missing)
        return info, None
    rec = b"".join(parts[i] for i in range(count))
    if crc32(rec) != rec_crc:
        raise QrError("record CRC mismatch after joining the parts")
    info["complete"] = True
    return info, rec


# ---- record decoders ----
def _ascii(b):
    return bytes(b).split(b"\0")[0].decode("ascii", errors="replace")


def decode_identity(rec):
    f = dict(t.split("=", 1) for t in rec.decode("ascii").split() if "=" in t)
    return {"fw": f.get("fw"), "git": f.get("g"), "dirty": f.get("g", "").endswith("-dirty"),
            "proto": int(f["p"]) if "p" in f else None}


def decode_boot(rec):
    import k2_remote
    return k2_remote.parse_bootinfo_text(rec.decode("ascii"))


SPEED = {0: "USB 2.0", 1: "USB 3.2 Gen1", 2: "USB 3.2 Gen2", 3: "USB4 Gen3", 4: "USB4 Gen4"}
PRODUCT_TYPE = {0: "undefined", 1: "PD hub", 2: "PD peripheral", 3: "passive cable", 4: "active cable",
                5: "alternate-mode adapter", 6: "VPD"}


def decode_emrec(r):
    if len(r) != 66:
        raise QrError("e-marker record is not 66 bytes")
    vdos = [int.from_bytes(r[21 + 4 * i:25 + 4 * i], "little") for i in range(5)]
    idh, product, cable = vdos[1], vdos[3], vdos[4]
    out = {"name": _ascii(r[:20]), "spec_rev": r[20], "vdos": [f"0x{v:08X}" for v in vdos],
           "checksum_ok": r[65] == (sum(r[:65]) + 0xA5) & 0xFF,
           "vid": f"0x{idh & 0xFFFF:04X}", "pid": f"0x{product >> 16:04X}",
           "product_type": PRODUCT_TYPE.get(idh >> 27 & 7, "reserved")}
    if (idh >> 27 & 7) in (3, 4):
        out.update(max_v=[20, 30, 40, 50][cable >> 9 & 3], max_a=5 if (cable >> 5 & 3) == 2 else 3,
                   speed=SPEED.get(cable & 7, f"code {cable & 7}"), termination=cable >> 11 & 3)
    return out


KIND = ["Ack", "Nak", "Busy", "Raw"]
UNMATCHED = ["Ignore", "Nak", "NotSupported"]


def decode_quirks(b):
    if len(b) < 3 or len(b) != 3 + 52 * b[2]:
        raise QrError("quirk block length does not fit its row count")
    flags, unm, count = b[0], b[1], b[2]
    rows = []
    for i in range(count):
        o = 3 + 52 * i
        sop, kind, nreq, nrep = b[o:o + 4]
        w = struct.unpack_from("<12I", b, o + 4)
        rows.append({"sop": ("P" if sop & 1 else "") + ("PP" if sop & 2 else ""),
                     "kind": KIND[kind] if kind < 4 else kind, "nreq": nreq, "nrep": nrep,
                     "hdr_val": f"0x{w[0]:08X}", "hdr_mask": f"0x{w[1]:08X}",
                     "req_val": [f"0x{x:08X}" for x in w[2:4]], "req_mask": [f"0x{x:08X}" for x in w[4:6]],
                     "rep": [f"0x{x:08X}" for x in w[6:6 + nrep]]})
    return {"flags": flags, "sop_pp": bool(flags & 1), "msgid_echo": bool(flags & 2),
            "unmatched": UNMATCHED[unm] if unm < 3 else unm, "rows": rows}


def decode_emarker(rec):
    if len(rec) < 91:
        raise QrError("e-marker record is shorter than 91 bytes")
    out = {"name": _ascii(rec[1:25]), "record": decode_emrec(rec[25:91])}
    if rec[0] & 1:
        out["quirks"] = decode_quirks(rec[91:])
    elif len(rec) != 91:
        raise QrError("e-marker record has trailing bytes without the quirk flag")
    return out


def decode_pdcaps(rec):
    if len(rec) < 6 or len(rec) != 6 + 4 * rec[0]:
        raise QrError("source-capabilities record length does not fit its PDO count")
    n, flags, spec, cc, hdr = rec[0], rec[1], rec[2], rec[3], int.from_bytes(rec[4:6], "little")
    pdos = []
    for i in range(n):
        raw = int.from_bytes(rec[6 + 4 * i:10 + 4 * i], "little")
        d, text = pd_trace.decode_pdo(raw)
        pdos.append({"pos": i + 1, "raw": f"0x{raw:08X}", "text": text, **d})
    return {"epr": bool(flags & 1), "epr_partial": bool(flags & 2), "spec_rev": spec,
            "active_cc": ["none", "CC1", "CC2"][cc] if cc < 3 else cc, "header": f"0x{hdr:04X}", "pdos": pdos}


def decode_card(rec):
    # A stored card: 0xA6, type, payload length, 0, payload, CRC. The payload grew with the formats (v5 178, v6 190, v7 196); the
    # length byte says which, and report_card.decode() accepts the lengths of every format.
    if len(rec) < 6 or rec[0] != 0xA6 or len(rec) != rec[2] + 6:
        raise QrError("report card record is not a stored card (0xA6, length byte, payload, CRC)")
    ln = int.from_bytes(rec[2:3], "little")
    crc = int.from_bytes(rec[-2:], "little")
    c = 0xFFFF
    for x in rec[:-2]:   # CRC-16/CCITT-FALSE
        c ^= x << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    out = report_card.decode(rec[4:4 + ln])
    out["_store_crc_ok"] = c == crc
    return out


def logdb_for(git):
    """(id -> format map, source, warning) for a firmware git string: tools/logdb.json as of that commit, else the working tree."""
    m = re.search(r"(?:^|-g)([0-9a-f]{7,40})(-dirty)?$", git)
    if m and not m.group(2):
        try:
            r = subprocess.run(["git", "-C", os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "show",
                                f"{m.group(1)}:tools/logdb.json"], capture_output=True, text=True, timeout=20)
            if r.returncode == 0:
                return {int(k): v for k, v in json.loads(r.stdout)["messages"].items()}, f"git:{m.group(1)}", None
        except (OSError, ValueError, KeyError, subprocess.SubprocessError):
            pass
        why = f"commit {m.group(1)} is not in this repository"
    else:
        why = "the build was dirty" if m else f"no commit hash in '{git}'"
    return logdb.load(), "working-tree", f"{why}: using the working tree's tools/logdb.json, ids it has changed or added may read wrong"


def arg_kinds(fmt):
    """Argument kinds of a K2LOG format, as k2::logfmt::signature: 'd' signed, 'u' unsigned (%u %x %X %c), 's' string."""
    kinds = []
    for m in re.finditer(r"%(\d*)(.?)", fmt, re.S):
        k = {"d": "d", "u": "u", "x": "u", "X": "u", "c": "u", "s": "s"}.get(m.group(2))
        if k:
            kinds.append(k)
    return kinds


def _varint(b, i):
    v = shift = 0
    while True:
        if i >= len(b) or shift > 28:
            raise QrError("log entry ends inside a number")
        v |= (b[i] & 0x7F) << shift
        i += 1
        shift += 7
        if not b[i - 1] & 0x80:
            return v, i


def decode_log_entries(entries, db, base_ms):
    """K2LOG entries (include/k2/core/binlog.hpp) -> [(t_ms, id, text)]; an unknown id prints as msg#<id> with the raw bytes."""
    out, i, t = [], 0, base_ms
    while i < len(entries):
        end = i + 1 + entries[i]
        if end > len(entries):
            raise QrError("log entry runs past the end of the record")
        body, j = entries[i + 1:end], 0
        dt, j = _varint(body, j)
        mid, j = _varint(body, j)
        t = (t + dt) & 0xFFFFFFFF
        fmt = db.get(mid)
        if fmt is None:
            out.append((t, mid, f"msg#{mid} ?" + (" " + body[j:].hex() if j < len(body) else "")))
        else:
            args = []
            try:
                for k in arg_kinds(fmt):
                    if j >= len(body):
                        break
                    if k == "s":
                        n = body[j]
                        args.append(urllib.parse.quote(bytes(body[j + 1:j + 1 + n]), safe=""))
                        j += 1 + n
                    else:
                        v, j = _varint(body, j)
                        args.append(str((v >> 1) ^ -(v & 1)) if k == "d" else f"{v:X}")   # zigzag / hex, as the text lines
            except QrError:
                pass
            out.append((t, mid, logdb._render(fmt, args).rstrip("\n")))
        i = end
    return out


def decode_log(rec):
    if len(rec) < 10 or len(rec) < 10 + rec[0]:
        raise QrError("log record is shorter than its head")
    glen = rec[0]
    git = rec[1:1 + glen].decode("ascii", errors="replace")
    older = bool(rec[1 + glen] & 1)
    base_ms, now_ms = struct.unpack_from("<II", rec, 2 + glen)
    db, source, warning = logdb_for(git)
    rows = decode_log_entries(rec[10 + glen:], db, base_ms)
    out = {"git": git, "older_dropped": older, "base_ms": base_ms, "now_ms": now_ms, "logdb": source, "entries": len(rows),
           "lines": [f"t=+{t}ms {text}" for t, _, text in rows]}
    if warning:
        out["warning"] = warning
    return out


def decode_loop(rec):
    """A LOOP record: the HID-cable loop probe result, every field with its unit (tools/loop_probe_rec.py)."""
    try:
        return loop_probe_rec.decode_record(rec)
    except loop_probe_rec.BadRecord as e:
        raise QrError(str(e))


def decode_battery(rec):
    """A BATT record: one battery recorder run, phases, totals, efficiency and curve (tools/battery_rec.py)."""
    try:
        return battery_rec.decode_record(rec)
    except battery_rec.BadRecord as e:
        raise QrError(str(e))


DECODERS = {"BI": decode_boot, "ID": decode_identity, "EMK": decode_emarker, "PDC": decode_pdcaps, "CARD": decode_card,
            "LOG": decode_log, "LOOP": decode_loop, "BATT": decode_battery}


# ---- schema v2: the bit-packed link records (the twin of k2/ui/qrshare_records.hpp pack_v2) ----
# LSB-first bit stream; varint = 7 value bits + 1 continuation bit. The v2 record expands back to the exact v1 record, so the
# v1 decoders above give the same JSON for a version '2' link as for the version '1' link of the same record.

class BitR:
    def __init__(self, data: bytes):
        self.data = data
        self.pos = 0

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


SVDM_MASK = 0xFFFF8000 | (3 << 6) | 0x1F
EPR_DV = [0, 280, 360, 480]
CABLE_KIND_DEC = {0: 0, 1: 1, 2: 2, 3: 4, 4: 0xFF}


def expand_card_v2(b: bytes) -> bytes:
    """A schema v2 card record -> the 256-byte v1 record (format 10), so decode_card() reads it as before.
    A v2 record packed before the rebalanced varint existed ends at the CRC: the varint is then absent and reads 0;
    likewise the v9 spread presence bit and the v10 sweep presence bit."""
    r = BitR(b)
    if r.get(8) != 2:
        raise QrError("not a schema v2 record")
    rec = bytearray(256)
    rec[0], rec[1], rec[2], rec[3] = 0xA6, 1, 250, 0
    rec[4 + 198:4 + 214] = b"\x80" * 16   # v9 spread slots default to "no level there"
    p = 4   # the payload starts at record offset 4; the put16/put32 below take absolute record offsets

    def put16(o, v):
        rec[o] = v & 0xFF
        rec[o + 1] = (v >> 8) & 0xFF

    def put32(o, v):
        put16(o, v & 0xFFFF)
        put16(o + 2, (v >> 16) & 0xFFFF)

    def group():
        verdict, reason, detail = r.get(2), r.get(6), r.get(6)
        max_dv = r.get(9) if r.get(1) else 0
        fail_dv = r.get(9) if r.get(1) else 0
        return verdict, reason, detail, max_dv, fail_dv

    put32(p + 0, r.get(32))
    put16(p + 4, r.get(16))
    put16(p + 6, r.get(16))
    put16(p + 8, r.varint())    # card_no
    put16(p + 10, r.varint())   # runs
    put32(p + 12, r.varint())   # boots
    put32(p + 16, r.varint())   # uptime_s
    put32(p + 20, r.get(32))    # fw
    put16(p + 24, r.varint())   # handover_ds
    rec[p + 26] = r.get(2)          # spec_rev
    rec[p + 27] = r.get(4)          # num_pdos
    rec[p + 28] = r.get(3)          # handover
    rec[p + 29] = r.get(4)          # state
    rec[p + 30] = r.get(6)          # reason
    n_ranges = r.get(3)
    rec[p + 31] = n_ranges
    rec[p + 32] = r.get(8)          # ev_flags
    rec[p + 33] = 10                # format 10 (v2 always expands to the current format; the v8 byte below)
    nl = r.get(5)
    rec[p + 34:p + 34 + nl] = bytes(r.get(7) for _ in range(nl))
    rec[p + 54] = r.get(8)                                   # pdp_w
    rec[p + 55] = r.get(1) | (r.get(7) << 1)                 # name_auto | pd_ma50 << 1
    for g in range(12):
        verdict, reason, detail, max_dv, fail_dv = group()
        q = p + 56 + 4 * g
        rec[q] = (verdict << 6) | (reason & 0x3F)
        v = max_dv | (fail_dv << 9) | (detail << 18)
        rec[q + 1], rec[q + 2], rec[q + 3] = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF
    for i in range(n_ranges):
        q = p + 104 + 10 * i
        rec[q] = r.get(5)          # alg
        rec[q + 1] = r.get(8)      # pos
        lo = r.get(9)
        hi, mx, fail, ma50 = r.get(9), r.get(9), r.get(9), r.get(7)
        put16(q + 2, lo | (ma50 << 9))   # lo_dv | ma50 << 9
        put16(q + 4, hi)           # hi_dv
        put16(q + 6, mx)           # max_dv
        put16(q + 8, fail)         # fail_dv
    for i in range(3):
        q = p + 144 + 6 * i
        code = r.get(2)
        put16(q, EPR_DV[code] if code < len(EPR_DV) else 0)
        verdict, reason, detail, max_dv, fail_dv = group()
        rec[q + 2] = (verdict << 6) | (reason & 0x3F)
        v = max_dv | (fail_dv << 9) | (detail << 18)
        rec[q + 3], rec[q + 4], rec[q + 5] = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF
    if r.get(1):   # a cable block follows
        q = p + 162
        put32(q, r.varint())       # seq
        put16(q + 4, r.get(16))    # vid
        put16(q + 6, r.get(16))    # pid
        put16(q + 8, r.get(16))    # max_mv
        put16(q + 10, r.get(16))   # max_ma
        rec[q + 12] = CABLE_KIND_DEC.get(r.get(3), 0)
        rec[q + 13] = r.get(8)     # flags
        rec[q + 14] = r.get(8)     # resumed
        rec[q + 15] = r.get(8)     # src_flags
    n_ufcs = r.get(3)
    for i in range(n_ufcs):
        q = p + 178 + 3 * i
        v = r.get(8) | (r.get(9) << 8) | (r.get(7) << 17)
        rec[q], rec[q + 1], rec[q + 2] = v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF
    times = r.varint()
    rec[p + 190] = times
    if times > 1:
        put16(p + 191, r.get(16))                  # last_boots
        up = r.get(24)
        rec[p + 193], rec[p + 194], rec[p + 195] = up & 0xFF, (up >> 8) & 0xFF, (up >> 16) & 0xFF
    rebalanced = r.varint() if len(b) * 8 - r.pos > 16 else 0   # v8 (absent in records packed before it)
    rec[p + 196] = rebalanced & 0xFF
    if len(b) * 8 - r.pos > 16:   # v9: the spread presence bit + 16 int8 bytes (absent in records packed before it)
        if r.get(1):
            for i in range(16):
                rec[p + 198 + i] = r.get(8)
    if len(b) * 8 - r.pos > 16:   # v10: the sweep presence bit (+ 4 x 9 bytes when set; absent before it)
        if r.get(1):
            for i in range(36):
                rec[p + 214 + i] = r.get(8)
    rec[254] = r.get(8)      # the v1 record's CRC-16, carried so the integrity check is unchanged
    rec[255] = r.get(8)
    return bytes(rec)


def expand_emk_v2(b: bytes) -> bytes:
    """A schema v2 e-marker record -> the v1 record (flags, name[24], the 66-byte emrec with its reserved zeros, the quirk
    block), so decode_emarker() reads it as before."""
    r = BitR(b)
    if r.get(8) != 2:
        raise QrError("not a schema v2 record")
    nl = r.get(5)
    name = bytes(r.get(7) for _ in range(nl))
    flags = r.get(8)
    rl = r.get(5)
    rname = bytes(r.get(7) for _ in range(rl))
    spec_rev = r.get(2)
    vdos = [r.get(32) for _ in range(5)]
    checksum = r.get(8)
    rec = bytearray([flags])
    nm = bytearray(24)
    nm[:len(name)] = name
    rec += nm
    em = bytearray(66)
    em[:len(rname)] = rname
    em[20] = spec_rev
    for i, v in enumerate(vdos):
        em[21 + 4 * i:25 + 4 * i] = v.to_bytes(4, "little")
    em[65] = checksum
    rec += em
    if r.get(1):
        qflags, unmatched, count = r.get(2), r.get(2), r.get(4)
        rec += bytes([qflags, unmatched, count])
        for _ in range(count):
            sop, kind, nreq, nrep = r.get(2), r.get(2), r.get(2), r.get(3)
            full = r.get(1)
            hdr_val = r.get(32)
            hdr_mask = SVDM_MASK if full else r.get(32)
            req = [(r.get(32), r.get(32)) for _ in range(nreq)]
            rep = [r.get(32) for _ in range(nrep)]
            row = bytearray(52)
            row[0], row[1], row[2], row[3] = sop, kind, nreq, nrep
            row[4:8] = hdr_val.to_bytes(4, "little")
            row[8:12] = hdr_mask.to_bytes(4, "little")
            for k, (val, mask) in enumerate(req):
                row[12 + 4 * k:16 + 4 * k] = val.to_bytes(4, "little")
                row[20 + 4 * k:24 + 4 * k] = mask.to_bytes(4, "little")
            for k, v in enumerate(rep):
                row[28 + 4 * k:32 + 4 * k] = v.to_bytes(4, "little")
            rec += row
    return bytes(rec)


def decode_card_v2(rec: bytes):
    return decode_card(expand_card_v2(rec))


def decode_emarker_v2(rec: bytes):
    return decode_emarker(expand_emk_v2(rec))


# ---- the link form (k2/ui/qrshare_link.hpp): <base>#<version><kind>.<base64url record> ----
LINK_BASE = "https://yarost12.github.io/open-k2-firmware/r/#"
LINK_KINDS = {"c": "card", "z": "card", "e": "emarker", "E": "emarker", "p": "pdcaps", "P": "pdcaps", "i": "identity",
              "I": "identity", "b": "boot", "B": "boot", "t": "text"}
LINK_DECODERS = {"card": decode_card, "emarker": decode_emarker, "pdcaps": decode_pdcaps, "identity": decode_identity,
                 "boot": decode_boot}


def decode_link(text: str) -> dict:
    """One link text -> {"version", "kind" (letter), "record_kind", "record"}: version '1' links carry the record as the K2
    data frame does (upper case letters and 'z' are LZ4 blocks), version '2' links the bit-packed schema v2 record of the card
    or the cable (decoded to the same JSON as version '1'), kind 't' a plain ASCII text record (read up to the first NUL)."""
    if LINK_BASE not in text:
        raise QrError("not a K2 results link (no known base address)")
    frag = text.split("#", 1)[1]
    m = re.fullmatch(r"(\d)([a-zA-Z])\.([A-Za-z0-9_-]*)", frag)
    if not m:
        raise QrError(f"bad link fragment {frag!r}")
    version, letter, raw = m.group(1), m.group(2), m.group(3)
    if letter not in LINK_KINDS:
        raise QrError(f"unknown kind letter {letter!r}")
    body = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))
    kind = LINK_KINDS[letter]
    out = {"version": int(version), "kind": letter, "record_kind": kind, "record_hex": body.hex()}
    if version == "2":
        if kind not in ("card", "emarker"):
            raise QrError(f"version '2' exists only for the card and the cable, not {kind!r}")
        out["record"] = (decode_card_v2 if kind == "card" else decode_emarker_v2)(body)
    elif version == "1":
        if letter in "zEPIB":
            body = lz4_decompress(body)
            out["packed"] = True
        if kind == "text":
            out["text"] = body.split(b"\0")[0].decode("ascii", errors="replace")
            out["record"] = {"text": out["text"]}
        else:
            out["record"] = LINK_DECODERS[kind](body)
    else:
        raise QrError(f"unknown link version {version!r}")
    return out


def decode_texts(texts):
    """Part texts -> (result dict, exit code)."""
    info, rec = assemble(texts)
    if rec is None:
        return info, 1
    try:
        if info["packed"]:
            info["sent_len"] = len(rec)
            rec = lz4_decompress(rec)
            info["record_len"] = len(rec)
        if info["schema"] == 2 and info["type"] in ("CARD", "EMK"):
            # a schema 2 frame carries the bit-packed v2 record: expand it to the v1 record and decode as before
            rec = expand_card_v2(rec) if info["type"] == "CARD" else expand_emk_v2(rec)
            info["v2_expanded"] = True
        info["record_hex"] = rec.hex()
        info["record"] = DECODERS[info["type"]](rec)
    except (QrError, ValueError, KeyError, report_card.BadCard) as e:
        info["decode_error"] = str(e)
    return info, 0


# ---- command line ----
def _read_texts(args):
    out = []
    for a in args:
        if a == "-":
            out += [ln.strip("\r\n") for ln in sys.stdin if ln.strip("\r\n")]
        elif a.startswith("@"):
            with open(a[1:]) as f:
                out += [ln.strip("\r\n") for ln in f if ln.strip("\r\n")]
        else:
            out.append(a.strip("\r\n"))
    return out


def scan_images(paths):
    """PNG paths -> QR texts. pyzbar when importable, else zbarimg; None when neither exists."""
    try:
        from pyzbar.pyzbar import decode as zdecode   # type: ignore
        from PIL import Image                         # type: ignore
        return [s.data.decode("ascii", errors="replace") for p in paths for s in zdecode(Image.open(p))]
    except ImportError:
        pass
    exe = shutil.which("zbarimg")
    if not exe:
        return None
    r = subprocess.run([exe, "--quiet", "--raw", *paths], capture_output=True, text=True, timeout=60)
    return [ln for ln in r.stdout.splitlines() if ln]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decode")
    d.add_argument("texts", nargs="+")
    d.add_argument("--lines", action="store_true", help="a LOG record: print its 't=+<ms>ms [TAG] message' lines, not JSON")
    dl = sub.add_parser("decode-link", help="a results link (the fragment after '#'): version '1' and '2', every kind, 't' too")
    dl.add_argument("links", nargs="+")
    di = sub.add_parser("decode-image")
    di.add_argument("images", nargs="+")
    e = sub.add_parser("encode")
    e.add_argument("--type", required=True, choices=sorted(TAGS))
    e.add_argument("--schema", type=int, default=1)
    g = e.add_mutually_exclusive_group(required=True)
    g.add_argument("--hex")
    g.add_argument("--text")
    g.add_argument("--file")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "decode":
            res, code = decode_texts(_read_texts(a.texts))
        elif a.cmd == "decode-link":
            res = [decode_link(t.strip("\r\n")) for t in _read_texts(a.links)]
            code = 0
        elif a.cmd == "decode-image":
            texts = scan_images(a.images)
            if texts is None:
                print("decode-image: neither pyzbar nor zbarimg is installed; decode the text with `decode` instead",
                      file=sys.stderr)
                return 3
            if not texts:
                print("decode-image: no QR code found", file=sys.stderr)
                return 2
            res, code = decode_texts(texts)
        else:
            if a.hex is not None:
                rec = bytes.fromhex(a.hex)
            elif a.text is not None:
                rec = a.text.encode("utf-8")
            else:
                with open(a.file, "rb") as f:
                    rec = f.read()
            print("\n".join(encode(TAGS[a.type], a.schema, rec)))
            return 0
    except (QrError, ValueError, OSError) as ex:
        print(json.dumps({"error": str(ex)}))
        return 2
    log = res.get("record", {}) if getattr(a, "lines", False) else {}
    if "lines" in log:
        if "warning" in log:
            print(f"warning: {log['warning']}", file=sys.stderr)
        print("\n".join(log["lines"]))
        return code
    print(json.dumps(res, indent=2))
    return code


if __name__ == "__main__":
    sys.exit(main())
