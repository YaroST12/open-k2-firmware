#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""PD black box reader: streams or dumps the K2's raw FUSB302 / PD trace ring and decodes it.

The firmware (k2/pd/pdtrace.hpp) records binary events into a RAM ring with no formatting; this tool
reads it over the remote command 0x70 and prints a timeline with relative timing, plus an optional
JSONL file (one JSON object per event).

  pd_trace.py live  [--jsonl FILE] [--raw FILE] [--tail] [--interval S]   poll the ring, print as it comes
  pd_trace.py dump  [--jsonl FILE] [--raw FILE] [--clear]                  everything in the ring, once
  pd_trace.py decode FILE [--jsonl FILE]                                   decode a raw ring dump (--raw)
  pd_trace.py captures                                                     list the trigger-frozen captures
  pd_trace.py capture N [--jsonl FILE] [--raw FILE]                        decode capture slot N (contract change,
                                                                           hard reset, VBUS dip, detach)
  pd_trace.py status | clear | freeze | resume | clear-captures            ring control
  pd_trace.py transition arm|trigger|disarm|status|save|list|clear         transition capture control (the
                                                                           VBUS + CC flight recorder, 0x71)
  pd_trace.py transition dump [--seq N] [--png F] [--csv F] [--raw F]      read the frozen capture (or stored N,
                                                                           default the newest stored one)
  pd_trace.py transition decode FILE [--png F] [--csv F]                   decode a --raw dump

Record layout: see the header comment of include/k2/pd/pdtrace.hpp (this file mirrors it).
"""
import argparse
import json
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

# ---- record types (k2::pdtrace::Type) ------------------------------------------------------------
(T_RX, T_RXBAD, T_TX, T_TXRESULT, T_RESET, T_CC, T_REG, T_STATUS0, T_IRQ, T_LINK, T_SINK, T_VBUS, T_POLL, T_MARK, T_EDGES,
 T_RXREPEAT) = range(1, 17)
TYPE_NAMES = {T_RX: "RX", T_RXBAD: "RX_BAD", T_TX: "TX", T_TXRESULT: "TX_RESULT", T_RESET: "RESET", T_CC: "CC",
              T_REG: "REG", T_STATUS0: "STATUS0", T_IRQ: "IRQ", T_LINK: "LINK", T_SINK: "SINK", T_VBUS: "VBUS",
              T_POLL: "POLL", T_MARK: "MARK", T_EDGES: "EDGES", T_RXREPEAT: "RX_REPEAT"}
HEADER = 6
EPR_CHUNK_BYTES = 26   # data bytes of one extended-message chunk
FLAG_LOST, FLAG_FROZEN, FLAG_RESET = 1, 2, 4
TX_RESULTS = ["TXSENT (GoodCRC)", "RETRYFAIL (no GoodCRC)", "timeout (no TXSENT / RETRYFAIL)"]
RESET_KINDS = ["Hard Reset received", "Hard Reset sent", "Soft_Reset received", "Soft_Reset sent"]
CC_EVENTS = ["detach", "attach"]
MARKS = {1: "caps-seen", 2: "start-request", 3: "trace-start", 4: "start-wait", 5: "reread", 6: "qc3-batch", 7: "vconn", 8: "hunt-refusal"}
# pd::SinkState / SinkResult / Err names, as in tools/k2_remote.py
SINK_STATES = ["idle", "epr-entering", "epr-wait-caps", "requesting", "transition", "ready", "aborting", "failed"]
SINK_RESULTS = ["none", "ok", "rejected", "wait-exhausted", "timeout", "tx-failed", "hard-reset", "soft-reset"]
# charger_test.hpp HuntCause and pd_sink.hpp ReqAnswer, as in tools/k2_remote.py
HUNT_CAUSES = ["none", "no-chain", "chain-stalled", "inside-chain", "request-held", "fixed-pdo", "cable-cap",
               "apdo-stale", "capped-5a", "refused-offer", "unexplained"]
HUNT_ANSWERS = ["none", "accept", "reject", "wait"]
PD_ERRS = ["ok", "no source caps", "bad PDO position", "over the voltage cap", "voltage outside the PDO range",
           "current over the PDO limit", "busy", "inline", "epr not available"]
BC_LVL = ["<200 mV (Ra/open)", "200-660 mV (Rd, default USB)", "660-1230 mV (1.5 A)", ">1230 mV (3 A)"]


# ---- FUSB302 registers ---------------------------------------------------------------------------
def _bits(*names):
    """names[i] is bit i (None = unnamed)."""
    return {i: n for i, n in enumerate(names) if n}


REG_NAMES = {0x01: "DEVICE_ID", 0x02: "SWITCHES0", 0x03: "SWITCHES1", 0x04: "MEASURE", 0x05: "SLICE",
             0x06: "CONTROL0", 0x07: "CONTROL1", 0x08: "CONTROL2", 0x09: "CONTROL3", 0x0A: "MASK", 0x0B: "POWER",
             0x0C: "RESET", 0x0D: "OCPREG", 0x0E: "MASKA", 0x0F: "MASKB", 0x10: "CONTROL4",
             0x3C: "STATUS0A", 0x3D: "STATUS1A", 0x3E: "INTERRUPTA", 0x3F: "INTERRUPTB", 0x40: "STATUS0",
             0x41: "STATUS1", 0x42: "INTERRUPT", 0x43: "FIFOS"}
REG_BITS = {
    "SWITCHES0": _bits("PDWN1", "PDWN2", "MEAS_CC1", "MEAS_CC2", "VCONN_CC1", "VCONN_CC2", "PU_EN1", "PU_EN2"),
    "SWITCHES1": _bits("TXCC1", "TXCC2", "AUTO_CRC", None, "DATAROLE", None, None, "POWERROLE"),
    "CONTROL0": _bits("TX_START", "AUTO_PRE", None, None, None, "INT_MASK", "TX_FLUSH"),
    "CONTROL1": _bits("ENSOP1", "ENSOP2", "RX_FLUSH", None, "BIST_MODE2", "ENSOP1DB", "ENSOP2DB"),
    "CONTROL2": _bits("TOGGLE"),
    "CONTROL3": _bits("AUTO_RETRY", None, None, "AUTO_SOFTRESET", "AUTO_HARDRESET", "BIST_TMODE", "SEND_HARD_RESET"),
    "MASK": _bits("M_BC_LVL", "M_COLLISION", "M_WAKE", "M_ALERT", "M_CRC_CHK", "M_COMP_CHNG", "M_ACTIVITY", "M_VBUSOK"),
    "POWER": _bits("PWR_BANDGAP", "PWR_RX_REFS", "PWR_MEASURE", "PWR_OSC"),
    "RESET": _bits("SW_RES", "PD_RESET"),
    "STATUS0": _bits(None, None, "WAKE", "ALERT", "CRC_CHK", "COMP", "ACTIVITY", "VBUSOK"),
    "STATUS1": _bits("OCP", "OVRTEMP", "TX_FULL", "TX_EMPTY", "RX_FULL", "RX_EMPTY", "RXSOP1", "RXSOP2"),
    "INTERRUPTA": _bits("I_HARDRST", "I_SOFTRST", "I_TXSENT", "I_HARDSENT", "I_RETRYFAIL", "I_SOFTFAIL", "I_TOGDONE", "I_OCP_TEMP"),
    "INTERRUPTB": _bits("I_GCRCSENT"),
    "INTERRUPT": _bits("I_BC_LVL", "I_COLLISION", "I_WAKE", "I_ALERT", "I_CRC_CHK", "I_COMP_CHNG", "I_ACTIVITY", "I_VBUSOK"),
}
SPEC_REV_SW1 = ["PD1.0?", "PD2.0", "PD3.0", "?"]


def reg_name(r):
    return REG_NAMES.get(r, f"REG_0x{r:02X}")


def reg_value_text(name, v):
    """Bit names set in `v`, with the multi-bit fields spelled out."""
    field = {"STATUS0": 0x03, "SWITCHES1": 0x60, "CONTROL0": 0x0C, "CONTROL3": 0x06}.get(name, 0)   # multi-bit fields
    parts = [REG_BITS.get(name, {}).get(i, f"b{i}") for i in range(8) if v >> i & 1 and name in REG_BITS and not field >> i & 1]
    if name == "SWITCHES1":
        parts.append(f"SPECREV={SPEC_REV_SW1[v >> 5 & 3]}")
    elif name == "MEASURE":
        parts = ["MEAS_VBUS"] if v & 0x40 else []
        parts.append(f"MDAC={v & 0x3F} ({(v & 0x3F) * 42 + 42} mV)")
    elif name == "CONTROL0":
        parts.append(f"HOST_CUR={v >> 2 & 3}")
    elif name == "CONTROL3":
        parts.append(f"N_RETRIES={v >> 1 & 3}")
    elif name == "STATUS0":
        parts.append(f"BC_LVL={v & 3} {BC_LVL[v & 3]}")
    elif name == "POWER":
        pass
    return " ".join(parts) if parts else ("-" if name in REG_BITS else "")


def decode_reg(r, v):
    n = reg_name(r)
    t = reg_value_text(n, v)
    return n, t


# ---- PD message decoding -------------------------------------------------------------------------
CONTROL = {1: "GoodCRC", 2: "GotoMin", 3: "Accept", 4: "Reject", 5: "Ping", 6: "PS_RDY", 7: "Get_Source_Cap",
           8: "Get_Sink_Cap", 9: "DR_Swap", 10: "PR_Swap", 11: "VCONN_Swap", 12: "Wait", 13: "Soft_Reset",
           14: "Data_Reset", 15: "Data_Reset_Complete", 16: "Not_Supported", 17: "Get_Source_Cap_Extended",
           18: "Get_Status", 19: "FR_Swap", 20: "Get_PPS_Status", 21: "Get_Country_Codes",
           22: "Get_Sink_Cap_Extended", 23: "Get_Source_Info", 24: "Get_Revision"}
DATA = {1: "Source_Capabilities", 2: "Request", 3: "BIST", 4: "Sink_Capabilities", 5: "Battery_Status", 6: "Alert",
        7: "Get_Country_Info", 8: "Enter_USB", 9: "EPR_Request", 10: "EPR_Mode", 11: "Source_Info", 12: "Revision",
        15: "Vendor_Defined"}
EXTENDED = {1: "Source_Capabilities_Extended", 2: "Status", 3: "Get_Battery_Cap", 4: "Get_Battery_Status",
            5: "Battery_Capabilities", 6: "Get_Manufacturer_Info", 7: "Manufacturer_Info", 8: "Security_Request",
            9: "Security_Response", 10: "Firmware_Update_Request", 11: "Firmware_Update_Response", 12: "PPS_Status",
            13: "Country_Info", 14: "Country_Codes", 15: "Sink_Capabilities_Extended", 16: "Extended_Control",
            17: "EPR_Source_Capabilities", 18: "EPR_Sink_Capabilities", 30: "Vendor_Defined_Extended"}
SPEC = {0: "unknown", 1: "PD2.0", 2: "PD3.x", 3: "reserved"}
SOPS = {0xE0: "SOP", 0xC0: "SOP'", 0xA0: "SOP''", 0x80: "SOP'_Debug", 0x60: "SOP''_Debug"}
EPR_MODE_ACTIONS = {1: "Enter", 2: "Enter_Acknowledged", 3: "Enter_Succeeded", 4: "Enter_Failed", 5: "Exit"}
# EPR_Mode Data field (bits 23..16) of Enter_Failed, USB PD 3.2 table 6.28.
EPR_ENTER_FAILED = {0: "unknown cause", 1: "cable not EPR capable", 2: "source failed to become VCONN source",
                    3: "EPR Mode Capable bit not set in the RDO", 4: "source unable to enter EPR Mode",
                    5: "EPR Mode Capable bit not set in the 5 V PDO"}


def epr_mode_text(obj):
    """'EPR_Mode <action>[ (<data meaning>)]' for an EPR_Mode data object; Enter carries the sink's PDP in watts."""
    act, data = obj >> 24 & 0xFF, obj >> 16 & 0xFF
    text = f"EPR_Mode {EPR_MODE_ACTIONS.get(act, act)}"
    if act == 1:
        text += f" (EPR Sink Operational PDP {data} W)"
    elif act == 4:
        text += f" ({EPR_ENTER_FAILED.get(data, f'invalid reason {data}')})"
    return text


def split_header(h):
    return {"ext": bool(h >> 15 & 1), "num_do": h >> 12 & 7, "msg_id": h >> 9 & 7, "power_role": h >> 8 & 1,
            "spec": h >> 6 & 3, "data_role": h >> 5 & 1, "type": h & 0x1F}


def message_name(h):
    f = split_header(h)
    if f["ext"]:
        return EXTENDED.get(f["type"], f"Ext_0x{f['type']:02X}")
    if f["num_do"]:
        return DATA.get(f["type"], f"Data_0x{f['type']:02X}")
    return CONTROL.get(f["type"], f"Ctl_0x{f['type']:02X}")


def decode_pdo(raw):
    """Source (or sink) capability object. Returns (dict, text). An all-zero word is no PDO (padding)."""
    if raw == 0:
        return {"kind": "empty"}, "empty (all-zero word, padding)"
    kind = raw >> 30
    if kind == 0:
        mv, ma = (raw >> 10 & 0x3FF) * 50, (raw & 0x3FF) * 10
        flags = [n for b, n in ((29, "DRP"), (28, "suspend-ok"), (27, "unconstrained"), (26, "usb-comms"),
                                (25, "DRD"), (24, "unchunked"), (23, "EPR")) if raw >> b & 1]
        d = {"kind": "fixed", "mv": mv, "ma": ma, "flags": flags}
        return d, f"fixed {mv / 1000:g} V {ma / 1000:g} A" + (" [" + ",".join(flags) + "]" if flags else "")
    if kind == 1:
        mx, mn, mw = (raw >> 20 & 0x3FF) * 50, (raw >> 10 & 0x3FF) * 50, (raw & 0x3FF) * 250
        return ({"kind": "battery", "min_mv": mn, "max_mv": mx, "max_mw": mw},
                f"battery {mn / 1000:g}-{mx / 1000:g} V {mw / 1000:g} W")
    if kind == 2:
        mx, mn, ma = (raw >> 20 & 0x3FF) * 50, (raw >> 10 & 0x3FF) * 50, (raw & 0x3FF) * 10
        return ({"kind": "variable", "min_mv": mn, "max_mv": mx, "max_ma": ma},
                f"variable {mn / 1000:g}-{mx / 1000:g} V {ma / 1000:g} A")
    sub = raw >> 28 & 3
    if sub == 0:
        mx, mn, ma = (raw >> 17 & 0xFF) * 100, (raw >> 8 & 0xFF) * 100, (raw & 0x7F) * 50
        lim = bool(raw >> 27 & 1)
        return ({"kind": "pps", "min_mv": mn, "max_mv": mx, "max_ma": ma, "power_limited": lim},
                f"PPS {mn / 1000:g}-{mx / 1000:g} V {ma / 1000:g} A" + (" [power-limited]" if lim else ""))
    if sub == 1:
        mx, mn, pdp = (raw >> 17 & 0x1FF) * 100, (raw >> 8 & 0xFF) * 100, raw & 0xFF
        return ({"kind": "epr-avs", "min_mv": mn, "max_mv": mx, "pdp_w": pdp},
                f"EPR-AVS {mn / 1000:g}-{mx / 1000:g} V {pdp} W")
    if sub == 2:
        i9, i15 = (raw >> 10 & 0x3FF) * 10, (raw & 0x3FF) * 10
        return ({"kind": "spr-avs", "ma_9_15v": i9, "ma_15_20v": i15},
                f"SPR-AVS 9-15 V {i9 / 1000:g} A, 15-20 V {i15 / 1000:g} A")
    return {"kind": "apdo?"}, f"APDO(reserved) 0x{raw:08X}"


def decode_rdo(raw, pdo=None):
    """Request data object. `pdo` is the offered PDO at that position (dict from decode_pdo) or None."""
    pos = raw >> 28 & 7 if False else raw >> 28 & 0xF
    flags = [n for b, n in ((26, "cap-mismatch"), (25, "usb-comms"), (24, "no-suspend"), (23, "unchunked"),
                            (22, "EPR")) if raw >> b & 1]
    if raw >> 27 & 1:
        flags.insert(0, "give-back")
    kind = pdo["kind"] if pdo else None
    if kind in ("pps",):
        mv, ma = (raw >> 9 & 0xFFF) * 20, (raw & 0x7F) * 50
        d = {"pos": pos, "mv": mv, "ma": ma, "flags": flags}
        return d, f"PDO {pos} PPS {mv / 1000:g} V {ma / 1000:g} A" + (" [" + ",".join(flags) + "]" if flags else "")
    if kind in ("epr-avs", "spr-avs"):
        mv, ma = (raw >> 9 & 0xFFF) * 25, (raw & 0x7F) * 50
        d = {"pos": pos, "mv": mv, "ma": ma, "flags": flags}
        return d, f"PDO {pos} AVS {mv / 1000:g} V {ma / 1000:g} A" + (" [" + ",".join(flags) + "]" if flags else "")
    if kind == "battery":
        op, mx = (raw >> 10 & 0x3FF) * 250, (raw & 0x3FF) * 250
        d = {"pos": pos, "op_mw": op, "max_mw": mx, "flags": flags}
        return d, f"PDO {pos} battery {op / 1000:g} W (max {mx / 1000:g} W)" + (" [" + ",".join(flags) + "]" if flags else "")
    op, mx = (raw >> 10 & 0x3FF) * 10, (raw & 0x3FF) * 10
    d = {"pos": pos, "op_ma": op, "max_ma": mx, "flags": flags}
    unknown = "" if kind else " (offered PDO unknown)"
    return d, f"PDO {pos} operating {op / 1000:g} A (max {mx / 1000:g} A)" + (" [" + ",".join(flags) + "]" if flags else "") + unknown


def decode_vdm(objs):
    """Vendor_Defined message data objects (first is the VDM header)."""
    h = objs[0]
    svid = h >> 16
    if not h >> 15 & 1:
        return {"svid": svid, "structured": False}, f"unstructured VDM SVID 0x{svid:04X}"
    cmd, ctype = h & 0x1F, h >> 6 & 3
    cmds = {1: "Discover_Identity", 2: "Discover_SVIDs", 3: "Discover_Modes", 4: "Enter_Mode", 5: "Exit_Mode",
            6: "Attention"}
    cname = cmds.get(cmd, f"SVID-specific {cmd}")
    t = ["REQ", "ACK", "NAK", "BUSY"][ctype]
    d = {"svid": svid, "structured": True, "command": cname, "cmd_type": t, "vdm_version": f"{h >> 13 & 3}.{h >> 11 & 3}",
         "object_position": h >> 8 & 7}
    text = f"{cname} {t} SVID 0x{svid:04X}"
    if cmd == 1 and ctype == 1 and len(objs) >= 2:
        vid = objs[1] & 0xFFFF
        d["vid"] = vid
        text += f" VID 0x{vid:04X}"
        if len(objs) >= 4:
            d["pid"] = objs[3] >> 16
            text += f" PID 0x{d['pid']:04X}"
    elif cmd == 2 and ctype == 1:
        svids = [s for o in objs[1:] for s in (o >> 16, o & 0xFFFF) if s]
        d["svids"] = svids
        text += " [" + ",".join(f"0x{s:04X}" for s in svids) + "]"
    return d, text


def decode_ext_header(w):
    return {"data_size": w & 0x1FF, "request_chunk": bool(w >> 10 & 1), "chunk": w >> 11 & 0xF, "chunked": bool(w >> 15 & 1)}


def decode_scedb(d):
    """Source_Capabilities_Extended data block (USB PD 3.1 table 6.52): 24 bytes, 25 with the EPR source PDP."""
    peak = [d[14] | d[15] << 8, d[16] | d[17] << 8, d[18] | d[19] << 8]
    e = {"vid": d[0] | d[1] << 8, "pid": d[2] | d[3] << 8, "xid": int.from_bytes(d[4:8], "little"), "fw": d[8], "hw": d[9],
         "voltage_reg": d[10], "holdup": d[11], "compliance": d[12], "touch_current": d[13], "peak_current": peak,
         "touch_temp": d[20], "source_inputs": d[21], "batteries": d[22], "pdp_w": d[23],
         "epr_pdp_w": d[24] if len(d) > 24 else None}
    text = (f"SCEDB VID {e['vid']:04X} PID {e['pid']:04X} XID {e['xid']:08X} FW {e['fw']} HW {e['hw']}, PDP {e['pdp_w']} W"
            + (f", EPR PDP {e['epr_pdp_w']} W" if e["epr_pdp_w"] is not None else "")
            + f", regulation 0x{e['voltage_reg']:02X} holdup {e['holdup']} compliance 0x{e['compliance']:02X}"
            + f" touch current 0x{e['touch_current']:02X} touch temp {e['touch_temp']} inputs 0x{e['source_inputs']:02X}"
            + f" batteries 0x{e['batteries']:02X} peak {'/'.join(f'0x{x:04X}' for x in peak)}")
    return e, text


def decode_skedb(d):
    """Sink_Capabilities_Extended data block (USB PD 3.2 table 6.61, 24 bytes)."""
    s = {"vid": d[0] | d[1] << 8, "pid": d[2] | d[3] << 8, "xid": int.from_bytes(d[4:8], "little"), "fw": d[8], "hw": d[9],
         "version": d[10], "sink_modes": d[17], "spr_min_w": d[18], "spr_op_w": d[19], "spr_max_w": d[20],
         "epr_min_w": d[21], "epr_op_w": d[22], "epr_max_w": d[23]}
    modes = [n for bit, n in ((0x20, "AVS"), (0x10, "battery unlimited"), (0x08, "battery powered"), (0x04, "AC powered"),
                              (0x02, "VBUS powered"), (0x01, "PPS")) if s["sink_modes"] & bit]
    text = (f"SKEDB VID {s['vid']:04X} PID {s['pid']:04X} v{s['version']}, modes 0x{s['sink_modes']:02X} ({' + '.join(modes) or 'none'}),"
            f" SPR PDP {s['spr_min_w']}/{s['spr_op_w']}/{s['spr_max_w']} W, EPR PDP {s['epr_min_w']}/{s['epr_op_w']}/{s['epr_max_w']} W")
    return s, text


class MessageDecoder:
    """Decodes messages in stream order: Request objects need the Source_Capabilities that offered them."""

    def __init__(self):
        self.caps = []          # decoded PDO dicts of the last Source_Capabilities (EPR chunks append)
        self.epr_buf, self.epr_total = bytearray(), 0

    def reset(self):
        self.caps = []
        self.epr_buf, self.epr_total = bytearray(), 0

    def decode(self, sop, header, payload, direction):
        """-> (name, text, detail dict). payload = data bytes without token / CRC."""
        f = split_header(header)
        name = message_name(header)
        n = len(payload) // 4
        objs = list(struct.unpack_from("<" + "I" * n, payload)) if n else []
        extra = []
        detail = {"name": name, "msg_id": f["msg_id"], "spec": SPEC[f["spec"]], "num_do": f["num_do"], "sop": sop}
        if sop == "SOP":
            roles = f"{'src' if f['power_role'] else 'snk'}/{'dfp' if f['data_role'] else 'ufp'}"
        else:
            roles = "cable-plug" if f["power_role"] else "dfp/ufp"
        text = f"{name} (id {f['msg_id']}, {SPEC[f['spec']]}, {roles}"
        if f["ext"]:
            if len(payload) >= 2:
                eh = decode_ext_header(payload[0] | payload[1] << 8)
                detail["ext_header"] = eh
                text += f", ext: {eh['data_size']} B chunk {eh['chunk']}" + (" request" if eh["request_chunk"] else "")
            if f["type"] == 17 and len(payload) > 2:   # EPR_Source_Capabilities chunk: PDOs after the 2-byte ext header
                self.caps_extend(payload, detail["ext_header"], detail, extra)
            elif len(payload) > 2 and not detail["ext_header"]["request_chunk"] and detail["ext_header"]["chunk"] == 0:
                data = payload[2:2 + detail["ext_header"]["data_size"]]   # first chunk, without the pad bytes
                detail["ext_payload"] = data.hex()
                if f["type"] == 1 and len(data) >= 24:
                    detail["scedb"], line = decode_scedb(data)
                    extra.append(line)
                elif f["type"] == 15 and len(data) >= 24:
                    detail["skedb"], line = decode_skedb(data)
                    extra.append(line)
                else:
                    extra.append("data " + data.hex())
        text += ")"
        if not f["ext"] and f["num_do"]:
            if f["type"] in (1, 4):
                if f["type"] == 1:
                    self.caps = []
                pdos = []
                for i, o in enumerate(objs):
                    d, t = decode_pdo(o)
                    pdos.append(d)
                    extra.append(f"PDO{i + 1} {t}")
                if f["type"] == 1:
                    self.caps = pdos
                detail["pdos"] = pdos
            elif f["type"] == 2 and objs:
                pos = objs[0] >> 28 & 0xF
                offered = self.caps[pos - 1] if 1 <= pos <= len(self.caps) else None
                d, t = decode_rdo(objs[0], offered)
                detail["rdo"] = d
                extra.append(t)
            elif f["type"] == 9 and objs:
                pos = objs[0] >> 28 & 0xF
                offered = self.caps[pos - 1] if 1 <= pos <= len(self.caps) else None
                d, t = decode_rdo(objs[0], offered)
                detail["rdo"] = d
                extra.append(t)
                if len(objs) > 1:
                    extra.append("copy of PDO: " + decode_pdo(objs[1])[1])
            elif f["type"] == 10 and objs:
                act = objs[0] >> 24 & 0xFF
                extra.append(epr_mode_text(objs[0]))
                detail["epr_action"] = EPR_MODE_ACTIONS.get(act, act)
                detail["epr_data"] = objs[0] >> 16 & 0xFF
            elif f["type"] == 15 and objs:
                d, t = decode_vdm(objs)
                detail["vdm"] = d
                extra.append(t)
            elif f["type"] == 12 and objs:
                extra.append(f"PD {objs[0] >> 28 & 0xF}.{objs[0] >> 24 & 0xF}, version {objs[0] >> 20 & 0xF}.{objs[0] >> 16 & 0xF}")
            else:
                extra.append("objects " + " ".join(f"{o:08X}" for o in objs))
        detail["objects"] = [f"{o:08X}" for o in objs]
        return name, text, extra, detail

    def caps_extend(self, payload, eh, detail, extra):
        """One EPR_Source_Capabilities chunk (26 B of the PDO stream each). PDOs are cut from the reassembled stream, never from
        a chunk alone: PDO 7 straddles the chunk 0 / chunk 1 boundary. Zero words are padding and keep their position."""
        if eh["request_chunk"]:
            return   # the sink asking for the next chunk carries no data
        if eh["chunk"] == 0:
            self.caps, self.epr_buf, self.epr_total = [], bytearray(), eh["data_size"]
        if len(self.epr_buf) != eh["chunk"] * EPR_CHUNK_BYTES:
            return   # a chunk out of order: nothing to place it by
        self.epr_buf += payload[2:2 + max(0, min(EPR_CHUNK_BYTES, self.epr_total - len(self.epr_buf)))]
        pdos = []
        while 4 * (len(self.caps) + 1) <= len(self.epr_buf):
            d, t = decode_pdo(struct.unpack_from("<I", self.epr_buf, 4 * len(self.caps))[0])
            pdos.append(d)
            self.caps.append(d)
            extra.append(f"PDO{len(self.caps)} {t}")
        detail["pdos"] = pdos


# ---- record parsing ------------------------------------------------------------------------------
def parse_records(buf):
    """Whole records in `buf` -> ([(type, ts_us32, payload)], leftover bytes)."""
    out, i = [], 0
    while i < len(buf):
        n = buf[i]
        if n < HEADER:
            raise ValueError(f"corrupt ring data at byte {i}: record length {n}")
        if i + n > len(buf):
            break
        out.append((buf[i + 1], struct.unpack_from("<I", buf, i + 2)[0], bytes(buf[i + HEADER:i + n])))
        i += n
    return out, bytes(buf[i:])


class Timeline:
    """Turns records into events with absolute time (unwrapped microseconds), the gap to the previous
    event and, for messages, the time since the last message of the other direction."""

    def __init__(self):
        self.md = MessageDecoder()
        self.last_ts = None
        self.base = 0
        self.prev_us = None
        self.last_rx = None          # (name, t_us) of the last received non-GoodCRC message
        self.last_tx = None
        self.last_by_name = {}       # RX name -> t_us (for periods)
        self.last_reset = None

    def new_epoch(self):
        self.last_ts = None
        self.base = 0
        self.prev_us = None
        self.last_rx = self.last_tx = None
        self.last_by_name = {}
        self.md.reset()

    def _unwrap(self, ts):
        if self.last_ts is not None and ts < self.last_ts and self.last_ts - ts > 1 << 31:
            self.base += 1 << 32
        self.last_ts = ts
        return self.base + ts

    def event(self, rtype, ts, payload):
        t_us = self._unwrap(ts)
        dt_ms = None if self.prev_us is None else (t_us - self.prev_us) / 1000.0
        self.prev_us = t_us
        ev = {"t_us": t_us, "t_ms": round(t_us / 1000.0, 3), "dt_ms": None if dt_ms is None else round(dt_ms, 3),
              "type": TYPE_NAMES.get(rtype, f"TYPE_{rtype}"), "raw": payload.hex()}
        lines = self._decode(rtype, payload, ev, t_us)
        ev["text"] = lines[0]
        ev["detail"] = lines[1:]
        return ev

    def _after(self, t_us, ref, label):
        if ref is None:
            return ""
        return f" [{(t_us - ref[1]) / 1000.0:.1f} ms after {label} {ref[0]}]"

    def _decode(self, rtype, p, ev, t_us):
        try:
            return self._decode_inner(rtype, p, ev, t_us)
        except (struct.error, IndexError, KeyError):
            return [f"{ev['type']} (malformed payload {p.hex()})"]

    def _decode_inner(self, rtype, p, ev, t_us):
        if rtype in (T_RX, T_TX):
            if rtype == T_RX:
                tok, header, body = p[0], p[1] | p[2] << 8, p[3:]
                sop = SOPS.get(tok & 0xE0, f"token 0x{tok:02X}")
                payload, crc = body[:-4], body[-4:]
                ev["crc"] = crc.hex()
                direction = "RX"
            else:
                # TX record: header, data objects, and since the record carries it a trailing
                # SOP token (0xE0 SOP, 0xC0 SOP', 0xA0 SOP'' - rx()-style, from the token stream
                # fifo_write_tx got; pdtrace.hpp tx()). Dumps from older firmware lack the
                # token: there the old byte guess (a 1-DO 0xFF00 VDM can only be the SOP'
                # e-marker spoof) still applies, but it mislabelled sink SOP traffic like the
                # proactive Discover_SVIDs REQ (trace5), so it is the fallback only.
                header, direction = p[0] | p[1] << 8, "TX"
                body = p[2:]
                sop = "SOP"
                if len(p) >= 3 and (len(p) - 3) % 4 == 0:
                    sop = SOPS.get(p[-1] & 0xE0, f"token 0x{p[-1]:02X}")
                    body = body[:-1]
                elif len(body) == 4 and header & 0x1F == 0x0F and header >> 12 & 7 == 1 and body[3] & 0xFF == 0xFF:
                    sop = "SOP'"
                payload = body
            name, text, extra, detail = self.md.decode(sop, header, payload, direction)
            ev["msg"] = detail
            ev["header"] = f"{header:04X}"
            ev["sop"] = sop
            after = ""
            if direction == "RX":
                if name != "GoodCRC":
                    prev = self.last_by_name.get(name)
                    if prev is not None:
                        after += f" [period {(t_us - prev) / 1000.0:.1f} ms since the previous {name}]"
                    self.last_by_name[name] = t_us
                    self.last_rx = (name, t_us)
                    after += self._after(t_us, self.last_tx, "our TX") if self.last_tx else ""
                crc_note = f"  crc={crc.hex()}"
            else:
                after = self._after(t_us, self.last_rx, "RX")
                self.last_tx = (name, t_us)
                crc_note = ""
            return [f"{direction:<9} {sop:<4} {text}{crc_note}{after}"] + extra
        if rtype == T_RXBAD:
            return [f"RX_BAD    unknown FIFO token 0x{p[0]:02X} (next bytes {p[1]:02X} {p[2]:02X}), FIFO flushed"]
        if rtype == T_TXRESULT:
            res = TX_RESULTS[p[0]] if p[0] < len(TX_RESULTS) else f"result {p[0]}"
            ev["tx_result"] = p[0]
            ev["tx_wait_ms"] = p[1]
            ia = reg_value_text("INTERRUPTA", p[2])
            return [f"TX_RESULT {res} after {p[1]} ms, INTERRUPTA seen: {ia}"]
        if rtype == T_RESET:
            k = RESET_KINDS[p[0]] if p[0] < len(RESET_KINDS) else f"kind {p[0]}"
            ev["reset"] = k
            gap = ""
            if p[0] in (0, 1):
                if self.last_reset is not None:
                    gap = f" [{(t_us - self.last_reset) / 1000.0:.1f} ms since the previous hard reset]"
                self.last_reset = t_us
            return [f"RESET     *** {k} ***{gap}"]
        if rtype == T_CC:
            ev["cc"] = p[1]
            return [f"CC        {CC_EVENTS[p[0]] if p[0] < 2 else p[0]}" + (f" CC{p[1]}" if p[1] else "") +
                    f", STATUS0 {reg_value_text('STATUS0', p[2])}"]
        if rtype == T_REG:
            failed = bool(p[0] & 0x80)
            n, t = decode_reg(p[0] & 0x7F, p[1])
            ev["reg"], ev["value"], ev["write_failed"] = n, p[1], failed
            return [f"REG       {n} <= 0x{p[1]:02X}" + (f"  {t}" if t else "") + ("  (I2C WRITE FAILED)" if failed else "")]
        if rtype == T_STATUS0:
            ev["value"] = p[0]
            return [f"STATUS0   0x{p[0]:02X} {reg_value_text('STATUS0', p[0])}"]
        if rtype == T_IRQ:
            parts = [f"{n}={reg_value_text(n, v)}" for n, v in zip(("INTERRUPTA", "INTERRUPTB", "INTERRUPT"), p[:3]) if v]
            return ["IRQ       " + "  ".join(parts)]
        if rtype == T_LINK:
            ev["inline"], ev["auto_sink"] = bool(p[0] & 1), bool(p[0] & 2)
            return [f"LINK      inline={'yes' if p[0] & 1 else 'no'} auto_sink={'yes' if p[0] & 2 else 'no'}"]
        if rtype == T_SINK:
            st = SINK_STATES[p[0]] if p[0] < len(SINK_STATES) else str(p[0])
            rs = SINK_RESULTS[p[1]] if p[1] < len(SINK_RESULTS) else str(p[1])
            ev["sink"] = {"state": st, "result": rs, "held": bool(p[2] & 1), "epr": bool(p[2] & 2), "pos": p[3]}
            return [f"SINK      state={st} result={rs} held={'yes' if p[2] & 1 else 'no'} pos={p[3]}" + (" EPR" if p[2] & 2 else "")]
        if rtype == T_VBUS:
            mv, ma = struct.unpack_from("<Hh", p)
            ev["mv"], ev["ma"] = mv, ma
            return [f"VBUS      {mv / 1000:.2f} V {ma} mA"]
        if rtype == T_POLL:
            gap = struct.unpack_from("<I", p)[0]
            ev["gap_us"] = gap
            return [f"POLL      RX FIFO non-empty; previous service() entry {gap / 1000.0:.2f} ms earlier (latency <= that)"]
        if rtype == T_MARK:
            code, a, b = p[0], p[1] | p[2] << 8, p[3] | p[4] << 8
            nm = MARKS.get(code, f"mark {code}")
            ev["mark"] = nm
            if code == 1:
                st = SINK_STATES[b] if b < len(SINK_STATES) else b
                return [f"MARK      {nm}: auto_sink={bool(a & 1)} inline={bool(a & 2)} held={bool(a & 4)} sink_state={st}"]
            if code == 2:
                if a & 0x100:   # the run start wanted a Request on these caps and did not make one: why (firmware start_request_why)
                    why = [n for bit, n in ((1, "inline"), (2, "no-cc"), (4, "sink-busy"), (8, "already-requested")) if a & bit]
                    return [f"MARK      {nm}: PDO 1 Request NOT queued ({' '.join(why) or 'unknown'})"]
                err = PD_ERRS[a] if a < len(PD_ERRS) else str(a)
                return [f"MARK      {nm}: PDO 1 Request queued -> {err}"]
            if code == 4:   # the START's Rd wait ended: a = VBUS mV, b = elapsed ms | 0x8000 when VBUS came up
                return [f"MARK      {nm}: {'VBUS up' if b & 0x8000 else 'timed out'} at {a} mV after {b & 0x7FFF} ms"]
            if code == 5:   # source-cap re-read: start a = mask, b = PDOs held; done a = 0x8000 | skipped << 8 | ok, b = avs << 15 | caps_rx << 8 | pdos_last
                if not a & 0x8000:
                    return [f"MARK      {nm}: started mask=0x{a & 0xFF:02X} with {b} PDOs held"]
                caps_rx = (b >> 8) & 0x7F
                got = (f"{b & 0xFF} PDOs in the last of {caps_rx} Source_Capabilities" + (", SPR AVS APDO present" if b & 0x8000 else "")
                       if caps_rx else "no Source_Capabilities came back")
                return [f"MARK      {nm}: done ok=0x{a & 0xFF:02X} skipped=0x{(a >> 8) & 0x7F:02X}, {got}"]
            if code == 6:   # a QC3/QC3+ batch is about to play: a = VBUS mV before it, b = signed pulse count
                n = b - 65536 if b & 0x8000 else b
                ev["pulses"], ev["vbus_before_mv"] = n, a
                return [f"MARK      {nm}: {n:+d} pulses from {a} mV"]
            if code == 7:   # VCONN_Swap experiment: a = state | why << 4 | applied << 8 | active cc << 12, b = mV on the net VCONN goes on
                st = ["off", "swap-sent", "settling", "on", "releasing", "failed"]
                why = ["none", "accepted", "rejected", "wait", "not-supported", "timeout", "released-by-partner", "refused", "abort"]
                s, w = a & 15, (a >> 4) & 15
                return [f"MARK      {nm}: state={st[s] if s < len(st) else s} why={why[w] if w < len(why) else w} "
                        f"vconn_pin={'ON' if a & 0x100 else 'off'} active_cc={(a >> 12) & 3} other_net={b} mV"]
            if code == 8:   # a refused 6 A hunt row: a = HuntCause | answer << 8 | timeout << 12, b = targeted APDO limit (mA)
                cause, ans = a & 0xFF, (a >> 8) & 15
                cn = HUNT_CAUSES[cause] if cause < len(HUNT_CAUSES) else str(cause)
                an = HUNT_ANSWERS[ans] if ans < len(HUNT_ANSWERS) else str(ans)
                return [f"MARK      {nm}: cause={cn} answer={an}{' (timeout)' if a & 0x1000 else ''} apdo_limit={b} mA"]
            return [f"MARK      {nm}"]
        if rtype == T_RXREPEAT:
            # The firmware collapses a Source_Capabilities frame equal to the one recorded before (MessageID and CRC aside)
            # and the Poll / Irq / Mark records that came with it: here is how many there were and over how long.
            count, span_ms = p[0] | p[1] << 8, p[2] | p[3] << 8
            ev["count"], ev["span_ms"] = count, span_ms
            return [f"RX_REPEAT {count} more identical Source_Capabilities over {span_ms} ms (collapsed by the recorder; "
                    f"MessageID and CRC differ; this record is stamped with the first of them)"]
        if rtype == T_EDGES:
            import vooc_edges
            n = len(vooc_edges.decode_chunk(p)[1])
            ev["edges"] = n
            return [f"EDGES     chunk {p[0]}: {n} D+/D- edges (decode: tools/vooc_edges.py)"]
        return [f"{ev['type']} {p.hex()}"]


def format_event(ev, wide=True):
    dt = "        " if ev["dt_ms"] is None else f"+{ev['dt_ms']:8.3f}"
    out = [f"{ev['t_ms']:12.3f} ms {dt}  {ev['text']}"]
    out += [" " * 36 + "| " + d for d in ev["detail"]]
    return "\n".join(out)


def decode_buffer(buf, tl=None):
    tl = tl or Timeline()
    recs, rest = parse_records(buf)
    return [tl.event(*r) for r in recs], rest, tl


# ---- device access -------------------------------------------------------------------------------
def fetch(link, cursor):
    """Read until the ring is drained. -> (bytes, next cursor, head, flags OR-ed)."""
    import k2_remote
    data, flags = bytearray(), 0
    while True:
        f, nxt, head, recs = k2_remote.blackbox_call(link, k2_remote.BB_READ, cursor)
        flags |= f
        data += recs
        if f & FLAG_RESET:
            data.clear()    # a restarted ring: what we have is stale
        cursor = nxt
        if not recs:
            return bytes(data), cursor, head, flags


def transition_output(data, a):
    """Prints the decoded capture and writes the optional CSV / PNG. -> exit status."""
    import transcap
    try:
        cap = transcap.decode_stream(data)
    except transcap.StreamError as e:
        print(f"bad capture stream: {e}", file=sys.stderr)
        return 1
    print(transcap.summary(cap))
    if a.csv:
        Path(a.csv).write_text(transcap.to_csv(cap))
        print(f"wrote {a.csv}")
    if a.png:
        if transcap.plot_png(cap, a.png):
            print(f"wrote {a.png}")
        else:
            print("matplotlib is not installed: no PNG (the CSV has the same data)", file=sys.stderr)
            if not a.csv:
                csv = str(Path(a.png).with_suffix(".csv"))
                Path(csv).write_text(transcap.to_csv(cap))
                print(f"wrote {csv}")
    return 0


def run_transition(link, a):
    import k2_remote
    if a.tc_cmd != "dump":
        ns = argparse.Namespace(tc_cmd=a.tc_cmd)
        print(k2_remote.render("transcap", k2_remote.cmd_transcap(link, ns)) if a.tc_cmd == "list"
              else json.dumps(k2_remote.cmd_transcap(link, ns)))
        return 0
    seq = a.seq
    if seq is None:
        if k2_remote.transcap_status(link)["state"] != "frozen":   # nothing live: the newest stored capture
            stored = k2_remote.transcap_list(link)
            if not stored:
                print("no frozen capture and nothing stored (arm, run the step, trigger)", file=sys.stderr)
                return 1
            seq = stored[0]["seq"]
    data = k2_remote.transcap_read(link, seq)
    if not data:
        print("the device returned no data for that capture", file=sys.stderr)
        return 1
    if a.raw:
        Path(a.raw).write_bytes(data)
    return transition_output(data, a)


def open_link(path, timeout):
    import k2_remote
    return k2_remote.RemoteLink(k2_remote.LinuxHidTransport(path), timeout)


class Sink:
    def __init__(self, jsonl=None, raw=None, out=print):
        self.jf = open(jsonl, "w") if jsonl else None
        self.rf = open(raw, "wb") if raw else None
        self.out = out

    def event(self, ev):
        self.out(format_event(ev))
        if self.jf:
            self.jf.write(json.dumps(ev) + "\n")
            self.jf.flush()

    def note(self, text, marker):
        """A line about the stream itself (a hole, a restart); jsonl gets it as an object with a `marker` key."""
        self.out(text)
        if self.jf:
            self.jf.write(json.dumps(marker) + "\n")
            self.jf.flush()

    def raw(self, data):
        if self.rf:
            self.rf.write(data)
            self.rf.flush()

    def close(self):
        for f in (self.jf, self.rf):
            if f:
                f.close()


def run_stream(link, sink, follow, tail, interval, clock=time.monotonic, sleep=time.sleep, deadline_s=None):
    import k2_remote
    tl, cursor, carry = Timeline(), 0, b""
    if tail:
        cursor = k2_remote.blackbox_call(link, k2_remote.BB_STATUS)[2]
    start = clock()
    while True:
        data, cursor, _head, flags = fetch(link, cursor)
        if flags & FLAG_RESET:
            sink.out("--- device restarted: ring cursor reset ---")
            tl.new_epoch()
            carry = b""
        elif flags & FLAG_LOST:
            sink.out("--- records lost: the ring wrapped before they were read ---")
            carry = b""
        sink.raw(data)
        evs, carry, _ = decode_buffer(carry + data, tl)
        for ev in evs:
            sink.event(ev)
        if not follow or (deadline_s is not None and clock() - start >= deadline_s):
            return
        sleep(interval)


# ---- live stream over the CDC shell ---------------------------------------------------------------
# `trace stream` on the debug shell pushes the ring records as binary frames (include/k2/core/trace_stream.hpp):
#   0xA6, kind, len, payload, crc16 LE (CRC-16/CCITT-FALSE over kind, len, payload)
# The HID port stays free for ct_start.py, so this needs neither k2_lock nor the HID interface.
SYNC = 0xA6
K_START, K_DATA, K_LOST = 1, 2, 3
# 4..10 (Hello, Text, Log, Power, Run, Gap, Beat) belong to the multiplexed stream (include/k2/core/dbg_stream.hpp,
# tools/k2_stream.py); `trace stream` sends Hello/Text/Beat next to the trace frames, this reader skips them.
K_KINDS = range(1, 11)
MAX_FRAME_PAYLOAD = 91


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def parse_frames(buf):
    """bytes -> ([(kind, payload)], bad, rest). Resyncs on SYNC, a wrong CRC counts as bad and the scan
    continues one byte on. `rest` is the unparsed tail (a frame still arriving)."""
    out, bad, i = [], 0, 0
    while True:
        i = buf.find(bytes([SYNC]), i)
        if i < 0:
            return out, bad, b""
        if i + 3 > len(buf):
            return out, bad, bytes(buf[i:])
        kind, n = buf[i + 1], buf[i + 2]
        if kind not in K_KINDS or n > MAX_FRAME_PAYLOAD:
            i += 1
            continue
        if i + 5 + n > len(buf):
            return out, bad, bytes(buf[i:])
        if crc16(buf[i + 1:i + 3 + n]) == buf[i + 3 + n] | buf[i + 4 + n] << 8:
            out.append((kind, bytes(buf[i + 3:i + 3 + n])))
            i += 5 + n
        else:
            bad += 1
            i += 1


class TraceStream:
    """Consumes the bytes read from the shell port, decodes the Data frames with the same Timeline as
    dump / live and reports every hole: a Lost frame (the ring wrapped before the device sent), an offset
    gap (frames missing on the wire) and a bad CRC."""

    def __init__(self, sink, tl=None):
        self.sink = sink
        self.tl = tl or Timeline()
        self.pend = b""
        self.expect = None           # ring offset the next Data frame must start at
        self.tail = b""              # last shell text, for the `trace stats` line
        self.frames = self.records = self.bad = self.gaps = 0
        self.lost_records = self.lost_bytes = self.restarts = 0
        self.started = False

    def feed(self, data):
        self.tail = (self.tail + data)[-256:]
        frames, bad, self.pend = parse_frames(self.pend + data)
        self.bad += bad
        if bad:
            self.sink.note(f"--- {bad} frame(s) dropped: bad CRC ---", {"marker": "bad_crc", "frames": bad})
        for kind, pl in frames:
            self.frames += 1
            self.handle(kind, pl)

    def handle(self, kind, pl):
        """One parsed frame; the trace kinds only (the other kinds belong to tools/k2_stream.py)."""
        if kind == K_START and len(pl) == 13:
            self._start(pl)
        elif kind == K_DATA and len(pl) >= 4:
            self._data(pl)
        elif kind == K_LOST and len(pl) == 9:
            self._lost(pl)

    def _start(self, pl):
        cursor, head, tail = struct.unpack_from("<III", pl, 1)
        self.started = True
        self.expect = None
        self.sink.note(f"--- stream started {'from the oldest record' if pl[0] & 1 else 'at now'}: "
                       f"cursor {cursor}, ring {tail}..{head} ---", {"marker": "start", "cursor": cursor, "head": head, "tail": tail})

    def _data(self, pl):
        off = struct.unpack_from("<I", pl)[0]
        recs = pl[4:]
        if self.expect is not None and off != self.expect:
            missing = (off - self.expect) & 0xFFFFFFFF
            self.gaps += 1
            self.sink.note(f"--- {missing} ring bytes missing between frames (offset {self.expect} -> {off}) ---",
                           {"marker": "gap", "bytes": missing, "from": self.expect, "to": off})
        self.expect = off + len(recs)
        try:
            parsed, rest = parse_records(recs)
        except ValueError as e:
            self.bad += 1
            self.sink.note(f"--- frame at offset {off} dropped: {e} ---", {"marker": "bad_records", "offset": off})
            return
        if rest:    # whole records only, anything else is a protocol error
            self.bad += 1
            self.sink.note(f"--- frame at offset {off} dropped: partial record ---", {"marker": "bad_records", "offset": off})
            return
        self.sink.raw(recs)
        for r in parsed:
            self.records += 1
            self.sink.event(self.tl.event(*r))

    def _lost(self, pl):
        rec, nbytes, flags = struct.unpack("<IIB", pl)
        self.expect = None
        if flags & 1:
            self.restarts += 1
            self.tl.new_epoch()
            self.sink.note("--- ring restarted on the device: cursor reset ---", {"marker": "restart"})
        else:
            self.lost_records += rec
            self.lost_bytes += nbytes
            self.sink.note(f"--- device lost {rec} records ({nbytes} bytes): the ring wrapped before they were sent ---",
                           {"marker": "lost", "records": rec, "bytes": nbytes})

    def device_stats(self):
        """(records, lost) from the `trace stats` line the device prints at `trace stop`, else None."""
        import re
        m = re.search(rb"trace stats records=(\d+) lost=(\d+)", self.tail)
        return (int(m.group(1)), int(m.group(2))) if m else None

    def summary(self):
        s = (f"{self.frames} frames, {self.records} records, {self.lost_records} lost on the device, "
             f"{self.gaps} wire gaps, {self.bad} bad frames, {self.restarts} ring restarts")
        d = self.device_stats()
        return s + (f"; device sent {d[0]} records, reported {d[1]} lost" if d else "; no `trace stats` line received")


def run_trace_stream(port, sink, all_records=False, seconds=None, clock=time.monotonic):
    """Opens the shell port, starts the stream, reads until `seconds` or Ctrl-C, stops it. -> TraceStream."""
    import os
    import select
    import fcntl
    import sniff_stream
    fd = sniff_stream.open_port(port)
    try:
        fcntl.ioctl(fd, 0x540C)    # TIOCEXCL: a second reader on the same port would steal bytes
    except OSError:
        pass
    ts = TraceStream(sink)

    def pump(duration):
        end = clock() + duration
        while clock() < end:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                ts.feed(os.read(fd, 4096))

    try:
        os.write(fd, b"\r\n")
        pump(0.3)
        os.write(fd, b"trace stream all\r\n" if all_records else b"trace stream\r\n")
        t0 = clock()
        try:
            while seconds is None or clock() - t0 < seconds:
                pump(0.1)
        except KeyboardInterrupt:
            pass
        os.write(fd, b"trace stop\r\n")
        pump(1.0)
    finally:
        os.close(fd)
    return ts


def main(argv=None):
    p = argparse.ArgumentParser(description="PD black box: read and decode the K2's FUSB302 / PD trace ring")
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    p.add_argument("--timeout", type=float, default=1.0)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name, h in (("live", "poll the ring and print events as they arrive"), ("dump", "print everything in the ring once")):
        s = sub.add_parser(name, help=h)
        s.add_argument("--jsonl", metavar="FILE")
        s.add_argument("--raw", metavar="FILE", help="also save the raw ring bytes (decode them later)")
        if name == "live":
            s.add_argument("--tail", action="store_true", help="start at now instead of the oldest record")
            s.add_argument("--interval", type=float, default=0.05)
            s.add_argument("--seconds", type=float, help="stop after this long")
        else:
            s.add_argument("--clear", action="store_true", help="clear the ring after reading")
    st = sub.add_parser("stream", help="live records over the CDC shell port (HID stays free for ct_start.py)")
    st.add_argument("--port", default="/dev/ttyACM0", help="the K2 debug shell (default /dev/ttyACM0)")
    st.add_argument("--jsonl", metavar="FILE")
    st.add_argument("--raw", metavar="FILE", help="also save the ring records (decode them later with `decode`)")
    st.add_argument("--seconds", type=float, help="stop after this long (default: until Ctrl-C)")
    st.add_argument("--all", action="store_true", help="start at the oldest record in the ring instead of now")
    d = sub.add_parser("decode", help="decode a raw dump written with --raw")
    d.add_argument("file")
    d.add_argument("--jsonl", metavar="FILE")
    c = sub.add_parser("capture", help="decode one trigger-frozen capture")
    c.add_argument("slot", type=int)
    c.add_argument("--jsonl", metavar="FILE")
    c.add_argument("--raw", metavar="FILE")
    for name in ("status", "clear", "freeze", "resume", "captures", "clear-captures"):
        sub.add_parser(name)
    t = sub.add_parser("transition", help="transition capture: VBUS and CC at ~1 kHz around a PD step")
    ts = t.add_subparsers(dest="tc_cmd", required=True)
    for name in ("arm", "trigger", "disarm", "status", "save", "list", "clear"):
        ts.add_parser(name)
    td = ts.add_parser("dump", help="read a capture, print its events, write CSV / PNG")
    td.add_argument("--seq", type=int, help="stored capture number (default: the frozen live one, else the newest stored)")
    tc = ts.add_parser("decode", help="decode a stream saved with dump --raw")
    tc.add_argument("file")
    for q in (td, tc):
        q.add_argument("--png", metavar="FILE", help="plot VBUS / CC1 / CC2 with the PD messages (needs matplotlib)")
        q.add_argument("--csv", metavar="FILE")
    td.add_argument("--raw", metavar="FILE", help="also save the encoded stream")
    a = p.parse_args(argv)

    if a.cmd == "transition" and a.tc_cmd == "decode":
        return transition_output(Path(a.file).read_bytes(), a)

    if a.cmd == "decode":
        sink = Sink(a.jsonl)
        evs, rest, _ = decode_buffer(Path(a.file).read_bytes())
        for ev in evs:
            sink.event(ev)
        sink.close()
        return 0

    if a.cmd == "stream":      # the shell port, not the HID link: no device lock, runs next to ct_start.py
        sink = Sink(a.jsonl, a.raw)
        try:
            ts = run_trace_stream(a.port, sink, a.all, a.seconds)
        except OSError as e:
            print(f"cannot use {a.port}: {e}", file=sys.stderr)
            return 2
        finally:
            sink.close()
        print(ts.summary())
        return 0 if ts.started and not ts.bad else 1

    import k2_lock
    import k2_remote
    try:
        with k2_lock.hold():
            link = open_link(a.path, a.timeout)
            try:
                if a.cmd == "transition":
                    return run_transition(link, a)
                if a.cmd == "capture":
                    data, filling = k2_remote.blackbox_capture_read(link, a.slot)
                    sink = Sink(a.jsonl, a.raw)
                    sink.raw(data)
                    if filling:
                        sink.out("--- capture still recording: read it again in a moment for the rest ---")
                    evs, _, _ = decode_buffer(data)
                    for ev in evs:
                        sink.event(ev)
                    sink.close()
                    return 0
                if a.cmd in ("status", "clear", "freeze", "resume", "captures", "clear-captures"):
                    ns = argparse.Namespace(bb_cmd=a.cmd)
                    print(json.dumps(k2_remote.cmd_blackbox(link, ns)))
                    return 0
                sink = Sink(a.jsonl, a.raw)
                try:
                    run_stream(link, sink, a.cmd == "live", getattr(a, "tail", False), getattr(a, "interval", 0.05),
                               deadline_s=getattr(a, "seconds", None))
                    if a.cmd == "dump" and a.clear:
                        k2_remote.blackbox_call(link, k2_remote.BB_CLEAR)
                except KeyboardInterrupt:
                    pass
                finally:
                    sink.close()
                return 0
            finally:
                link.transport.close()
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
