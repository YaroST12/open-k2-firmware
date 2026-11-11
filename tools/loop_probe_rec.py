# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The HID-cable loop probe result (include/k2/loopprobe/loop_probe.hpp, docs/LOOP_PROBE.md) -> JSON-ready dict.

Two entry points: decode_record() for the 399-byte record a LOOP QR share carries (tools/k2_qr.py), decode_file() for the
flash file /lp/last.rec that `k2_remote.py loop-probe` reads ("LPR" + version, length u16, CRC-16 u16, the record).
Every value keeps its unit in the key name (_mv, _ma, _ms, _raw for 12-bit ADC counts). Raw numbers only, no verdicts.
"""
import struct

VERSION = 1
RECORD_BYTES = 399
FILE_HEADER = 8

STEPS = ["baseline", "drop", "dpdm", "emarker", "cc"]
STATUS = ["not_run", "done", "skipped", "aborted", "interrupted"]
WHY = ["", "no monitor", "VBUS above 5.5 V", "aux above 5.8 V", "no 5 V", "host on HID", "refused", "none sniffed",
       "cancelled", "store failed", "rebooted"]
LOAD = ["backlight on", "backlight off", "backlight off + CPU busy", "backlight on + CPU busy"]
DRIVE = ["HiZ", "0V", "0.6V", "2.0V", "3.3V", "RxPulldown", "Listen", "Sniff"]
PULL = ["none", "core 1.5k pull-up", "PA12 weak pull-up"]
GUARD = ["none", "pd_contract", "data_device"]
EMK_READ = ["none", "alone", "attached"]
CC_REFUSAL = ["none", "bus", "contract", "engine", "vconn", "vbus_high", "bad_plan"]
CHANNELS = ["vbus_mv", "ibus_ma", "aux_mv", "dp_mv", "dm_mv", "cc1_mv", "cc2_mv"]
# BC_LVL as a sink reads a source's Rp (the FUSB302 comparator bands).
BC_LVL = ["<200 mV", "200-660 mV", "660-1230 mV", ">1230 mV"]


class BadRecord(ValueError):
    pass


def crc16(data, crc=0xFFFF):
    """CRC-16/CCITT-FALSE, as k2::bbstore::crc16."""
    for x in data:
        crc ^= x << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class _R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def take(self, fmt):
        v = struct.unpack_from("<" + fmt, self.b, self.i)
        self.i += struct.calcsize("<" + fmt)
        return v if len(v) > 1 else v[0]


def _name(table, i):
    return table[i] if 0 <= i < len(table) else f"#{i}"


def _hid(bits, frame=None, resets=None):
    out = {"core_running": bool(bits & 1), "configured": bool(bits & 2), "rx_dp": bool(bits & 4), "rx_dm": bool(bits & 8),
           "pin_dp": bool(bits & 16), "pin_dm": bool(bits & 32)}
    if frame is not None:
        out["frame"] = frame
        out["bus_resets"] = resets
    return out


def _bc(bc):
    if not bc & 0x10:
        return None
    return {"cc1": bc & 3, "cc1_band": BC_LVL[bc & 3], "cc2": (bc >> 2) & 3, "cc2_band": BC_LVL[(bc >> 2) & 3]}


def _stat(r):
    a, lo, hi = r.take("hhh")
    return {"avg": a, "min": lo, "max": hi}


def _adc_mv(raw):
    """12-bit CC count to millivolts at a nominal 3.3 V reference (the record keeps the count; this is approximate)."""
    return None if raw == 0xFFFF else round(raw * 3300 / 4095)


def decode_record(rec):
    if len(rec) != RECORD_BYTES:
        raise BadRecord(f"loop probe record is {len(rec)} bytes, expected {RECORD_BYTES}")
    if rec[0] != VERSION:
        raise BadRecord(f"loop probe record version {rec[0]}, this reader knows {VERSION}")
    r = _R(rec)
    r.take("B")
    flags = r.take("B")
    fw = r.take("30s").split(b"\0", 1)[0].decode("ascii", errors="replace")
    start_ms = r.take("I")
    steps = {}
    for name in STEPS:
        st, why, t_ms, dur_ms = r.take("BBIH")
        steps[name] = {"status": _name(STATUS, st), "why": _name(WHY, why), "t_ms": t_ms, "dur_ms": dur_ms}
    base_n = r.take("B")
    base = {ch: {k + ch[ch.rfind("_"):]: v for k, v in _stat(r).items()} for ch in CHANNELS}
    hid0 = _hid(*r.take("BHH"))
    hid1 = _hid(*r.take("BHH"))
    active_cc, link_flags, guard, bc = r.take("BBBB")
    drop_n = r.take("B")
    drop = []
    for _ in range(8):
        load, n = r.take("BB")
        vb, ib = _stat(r), _stat(r)
        aux = r.take("h")
        drop.append({"load": _name(LOAD, load), "samples": n, "vbus_avg_mv": vb["avg"], "vbus_min_mv": vb["min"],
                     "vbus_max_mv": vb["max"], "ibus_avg_ma": ib["avg"], "ibus_min_ma": ib["min"], "ibus_max_ma": ib["max"],
                     "aux_mv": aux})
    dp_n = r.take("B")
    dp = []
    for _ in range(6):
        drive, dp_mv, dm_mv, hid, vmax = r.take("BhhBh")
        dp.append({"drive_dp": _name(DRIVE, drive >> 4), "drive_dm": _name(DRIVE, drive & 15), "bus_dp_mv": dp_mv,
                   "bus_dm_mv": dm_mv, "hid": _hid(hid), "vbus_max_mv": vmax})
    pull_method, pull_n = r.take("BB")
    pull = []
    for _ in range(2):
        on, dp_mv, dm_mv, hid = r.take("BhhB")
        pull.append({"pullup_on": bool(on), "bus_dp_mv": dp_mv, "bus_dm_mv": dm_mv, "hid": _hid(hid)})
    emk_valid, num_vdos = r.take("BB")
    vdos = [r.take("I") for _ in range(6)]
    emk_seq, emk_mode = r.take("HB")
    refusal, sw0, pc, cc_flags, cc_n = r.take("BBBBB")
    rows = []
    for _ in range(5):
        cfg, c1, c2, rbc, vb = r.take("BHHBH")
        rows.append({"rd": "as found" if cfg == 0x80 else {0: "none", 1: "CC1", 2: "CC2"}.get(cfg, f"#{cfg}"),
                     "cc1_raw": c1, "cc2_raw": c2, "cc1_approx_mv": _adc_mv(c1), "cc2_approx_mv": _adc_mv(c2),
                     "bc_lvl": _bc(rbc), "vbus_mv": None if vb == 0xFFFF else vb})
    assert r.i == RECORD_BYTES
    return {
        "version": VERSION, "in_progress": bool(flags & 1), "fw": fw, "start_ms": start_ms, "steps": steps,
        "baseline": {"samples": base_n, **base, "hid_start": hid0, "hid_end": hid1,
                     "link": {"active_cc": ["none", "CC1", "CC2"][active_cc] if active_cc < 3 else active_cc,
                              "inline": bool(link_flags & 1), "sink_downstream": bool(link_flags & 2),
                              "caps_seen": bool(link_flags & 4), "contract": bool(link_flags & 8),
                              "k2_sink_contract": bool(link_flags & 16), "guard": _name(GUARD, guard)},
                     "bc_lvl": _bc(bc)},
        "drop": drop[:drop_n],
        "dpdm": {"rows": dp[:dp_n], "pull_method": _name(PULL, pull_method), "pull": pull[:pull_n]},
        "emarker": {"valid": bool(emk_valid), "num_vdos": num_vdos, "vdos": [f"0x{v:08X}" for v in vdos[:min(num_vdos, 6)]],
                    "seq": emk_seq, "last_vconn_read": _name(EMK_READ, emk_mode)},
        "cc": {"refusal": _name(CC_REFUSAL, refusal), "switches0_found": f"0x{sw0:02X}", "cc_switch_pins": f"0x{pc:X}",
               "restored": bool(cc_flags & 1), "aborted_vbus": bool(cc_flags & 2), "disturbed": bool(cc_flags & 4),
               "rows": rows[:cc_n]},
    }


def decode_file(b):
    if len(b) < FILE_HEADER or b[:3] != b"LPR":
        raise BadRecord("not a loop probe file (no LPR magic)")
    if b[3] != VERSION:
        raise BadRecord(f"loop probe file version {b[3]}, this reader knows {VERSION}")
    n, crc = struct.unpack_from("<HH", b, 4)
    rec = bytes(b[FILE_HEADER:])
    if len(rec) != n:
        raise BadRecord(f"loop probe file holds {len(rec)} record bytes, its header says {n}")
    if crc16(rec) != crc:
        raise BadRecord("loop probe file CRC mismatch")
    return decode_record(rec)
