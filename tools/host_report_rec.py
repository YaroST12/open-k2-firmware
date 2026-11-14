# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The loop probe's Host report (include/k2/loopprobe/host_report.hpp, docs/LOOP_PROBE.md "Host report") -> JSON-ready dict.

decode_file() takes the flash file /lp/host.rec that `k2_remote.py loop-probe --host` reads ("LPH" + version, length u16,
CRC-16 u16, the 529-byte record); decode_record() the record alone. Raw numbers plus the firmware's names for its enums;
the host OS guess ("os") comes from the firmware's classifier, whose rules are UNVERIFIED guesses.
"""
import struct

import loop_probe_rec

VERSION = 1
RECORD_BYTES = 529
FILE_HEADER = 8
MAGIC = b"LPH"

STATE = ["none", "armed", "running", "done"]
QUERIES = ["id_ufp", "src_cap_ext", "revision", "mfr_info", "status", "dr_swap", "id_dfp", "svids", "modes1", "modes2",
           "dr_swap_back"]
ANS = ["not run", "ack", "nak", "busy", "not supported", "reject", "wait", "timeout", "no goodcrc", "skipped", "interrupted"]
SKIP = ["", "PD 2.0", "not DFP", "identity known", "no SVID", "not swapped", "hard reset", "rebooted", "ended"]
END = ["none", "done", "no contract", "cancelled", "foreign sink", "detached", "rebooted", "rebooted 3x", "VBUS high",
       "no loop"]
REFUSE = ["none", "USB link off", "no host on HID", "no CC", "no 5V", "VBUS above 5.5V", "load on output", "another sink",
          "own Rd", "PD tool busy", "contract held"]
OS = ["unknown", "charge-only", "Windows", "macOS", "Linux", "Android", "iOS", "ChromeOS"]
EVENTS = ["setup", "reset", "suspend", "resume"]
RP = ["unknown", "default USB", "1.5 A", "3 A"]
FLAG_SOF, FLAG_CONFIGURED, FLAG_OVERFLOW = 1, 2, 4

BadRecord = loop_probe_rec.BadRecord


def _name(table, i):
    return table[i] if 0 <= i < len(table) else f"#{i}"


class _R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.i)
        self.i += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]


def decode_record(rec):
    if len(rec) != RECORD_BYTES:
        raise BadRecord(f"record is {len(rec)} bytes, expected {RECORD_BYTES}")
    if rec[0] != VERSION:
        raise BadRecord(f"record version {rec[0]}, this reader knows {VERSION}")
    r = _R(rec)
    r.take("B")
    state, boots, end, refuse, os_ = r.take("5B")
    fw = r.take("30s").split(b"\0", 1)[0].decode("ascii", "replace")
    start_ms, cc, rp, vbus, dp, dm = r.take("IBBhhh")
    rev, n_pdos = r.take("BB")
    pdos = list(r.take("7I"))
    caps_seen, ready_ms = r.take("BH")
    queries = {}
    for name in QUERIES:
        ans, attempts, skip, t_ms = r.take("BBBH")
        queries[name] = {"answer": _name(ANS, ans), "attempts": attempts, "skip": _name(SKIP, skip), "t_ms": t_ms}
    n_id = r.take("B")
    ident = list(r.take("6I"))
    n_svids = r.take("B")
    svids = list(r.take("8H"))
    mode_svid = list(r.take("2H"))
    mode_n = list(r.take("2B"))
    mode_vdo = list(r.take("2I"))
    n_scx = r.take("B")
    scx = r.take("25s")
    revision, n_mfr = r.take("IB")
    mfr = r.take("26s")
    n_status = r.take("B")
    status = r.take("7s")
    dfp, hard, soft, tx_fails = r.take("4B")
    n_ev, total, flags = r.take("BHB")
    events = []
    for i in range(24):
        t_ms, kind, req_type, req, value, index, length = r.take("HBBBHHH")
        if i < n_ev:
            events.append({"t_ms": t_ms, "kind": _name(EVENTS, kind), "bmRequestType": req_type, "bRequest": req,
                           "wValue": value, "wIndex": index, "wLength": length})
    assert r.i == RECORD_BYTES
    return {
        "version": VERSION, "state": _name(STATE, state), "boots": boots, "end": _name(END, end),
        "refusal": _name(REFUSE, refuse), "os_guess": _name(OS, os_), "fw": fw, "start_ms": start_ms,
        "cc": cc, "rp": _name(RP, rp), "vbus_mv": vbus, "dp_mv": dp, "dm_mv": dm,
        "pd": {
            "rev_raw": rev, "pdos": [f"0x{x:08X}" for x in pdos[:min(n_pdos, 7)]], "caps_seen": caps_seen,
            "contract_ms": ready_ms, "queries": queries,
            "identity": [f"0x{x:08X}" for x in ident[:min(n_id, 6)]],
            "svids": [f"0x{x:04X}" for x in svids[:min(n_svids, 8)]],
            "modes": [{"svid": f"0x{mode_svid[k]:04X}", "n": mode_n[k], "vdo1": f"0x{mode_vdo[k]:08X}"}
                      for k in range(2) if mode_svid[k]],
            "src_cap_ext": scx[:min(n_scx, 25)].hex(), "revision": f"0x{revision:08X}" if revision else None,
            "mfr_info": mfr[:min(n_mfr, 26)].hex(), "status": status[:min(n_status, 7)].hex(),
            "dfp_at_end": bool(dfp), "hard_resets": hard, "soft_resets": soft, "tx_fails": tx_fails,
        },
        "usb": {"events_total": total, "sof": bool(flags & FLAG_SOF), "configured": bool(flags & FLAG_CONFIGURED),
                "overflow": bool(flags & FLAG_OVERFLOW), "events": events},
    }


def decode_file(b):
    if len(b) < FILE_HEADER or b[:3] != MAGIC:
        raise BadRecord("not a Host report file (no LPH magic)")
    if b[3] != VERSION:
        raise BadRecord(f"file version {b[3]}, this reader knows {VERSION}")
    n, crc = struct.unpack_from("<HH", b, 4)
    rec = bytes(b[FILE_HEADER:FILE_HEADER + n])
    if n != RECORD_BYTES or len(rec) != n:
        raise BadRecord(f"record length {n}, file holds {len(b) - FILE_HEADER}")
    if loop_probe_rec.crc16(rec) != crc:
        raise BadRecord("CRC mismatch")
    return decode_record(rec)
