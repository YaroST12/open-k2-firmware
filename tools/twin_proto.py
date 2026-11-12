# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Device twin on the host: the pure decoders for Cmd::Twin (0x63) and the twin record, and the one-line texts.

Layouts: include/k2/twin/wire.hpp (the responses), include/k2/twin/library.hpp (the record), include/k2/twin/present.hpp (the
texts; `summary`, `note` and `proof` here print exactly what the screen and the shell print, tests/test_twin_present.cpp and
tests/test_twin_proto.py hold the same cases). Nothing here talks to the device; tools/k2_remote.py `twin` does.
"""

ERR_NAMES = {0: "ok", 1: "library full", 2: "invalid twin", 3: "no such twin", 4: "flash error", 5: "name in use",
             7: "busy (a twin run, a charger test or an FC release owns the link)",
             8: "no charger list (attach to a charger first)", 9: "learn failed"}
LEARN_NAMES = ["ok", "no identify", "not inline", "nothing sniffed", "not stored"]   # twin_hw::Learn
LEARN_HINTS = {"no identify": "run Identify with the device alone on the K2 first",
               "not inline": "sniffing needs the real device behind the K2",
               "nothing sniffed": "no Request with PS_RDY inside the charger's list seen yet"}
KINDS = ["none", "fixed", "pps", "five_v"]                       # twin::Kind
LIMITERS = ["none", "device", "charger", "cable"]                # twin::Limiter
VERDICTS = ["not run", "granted", "refused", "no PS_RDY", "out of band", "no list", "link reset", "overvoltage", "skipped",
            "cancelled"]                                          # twin::live::Verdict, name()
SKIPS = ["", "nothing usable", "above the monitor ceiling", "request not buildable", "twin unreadable"]   # twin::live::Skip
REASONS = ["done", "cancelled", "ports taken", "no offer"]      # twin::live::Reason
REFUSALS = ["ok", "confirm needed", "busy", "link inline", "no power monitor", "no charger", "no twins"]   # live::Refusal
USB_WHICH = ["declared", "observed"]

KAPACITY = 12
NAME_MAX = 24
MAX_SINK_PDOS = 7
MAX_PT, MAX_SVIDS, MAX_MODES = 2, 6, 2
HEADER_BYTES = 38
OBS_BYTES = 18
FLAG_REQUEST, FLAG_DECLARED, FLAG_OBSERVED = 1, 2, 4


class TwinError(Exception):
    """A response with a non-zero err byte, or one that cannot be read."""


class BadRecord(TwinError):
    """A twin record that fails the library's own checks (magic, version, length, CRC)."""


def crc16(data, init=0xFFFF):
    """CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, MSB first), as k2::bbstore::crc16."""
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def _u16(b, o):
    return int.from_bytes(b[o:o + 2], "little")


def _u32(b, o):
    return int.from_bytes(b[o:o + 4], "little")


# ---- the record --------------------------------------------------------------------------------------------------------------

def decode_record(raw):
    """One twin record (a whole /twin/NN.twn file) as a dict. Raises BadRecord with the library's reason."""
    raw = bytes(raw)
    if len(raw) < 4:
        raise BadRecord("record short")
    if raw[:3] != b"K2T":
        raise BadRecord("record magic")
    version = raw[3]
    if version == 0 or version > 2:
        raise BadRecord(f"record version {version}")
    if len(raw) < HEADER_BYTES + 2:
        raise BadRecord("record short")
    flags = raw[5]
    known = FLAG_REQUEST if version == 1 else FLAG_REQUEST | FLAG_DECLARED | FLAG_OBSERVED
    n_pdos = raw[13]
    if flags & ~known or n_pdos > MAX_SINK_PDOS:
        raise BadRecord("record invalid")
    req, decl, obs = bool(flags & FLAG_REQUEST), bool(flags & FLAG_DECLARED), bool(flags & FLAG_OBSERVED)
    decl_at = HEADER_BYTES + 4 * n_pdos + (8 if req else 0)
    dlen = 0
    if decl:
        if len(raw) < decl_at + 3:
            raise BadRecord("record short")
        npt, nsv, nmo = raw[decl_at], raw[decl_at + 1], raw[decl_at + 2]
        if npt > MAX_PT or nsv > MAX_SVIDS or nmo > MAX_MODES or (npt | nsv | nmo) == 0:
            raise BadRecord("record invalid")
        dlen = 3 + 4 * npt + 2 * nsv + 6 * nmo
    total = decl_at + dlen + (OBS_BYTES if obs else 0) + 2
    if len(raw) < total:
        raise BadRecord("record short")
    if len(raw) > total:
        raise BadRecord("record length")
    if crc16(raw[:-2]) != _u16(raw, total - 2):
        raise BadRecord("record crc")
    out = {"version": version, "seq": raw[4], "name": raw[14:14 + NAME_MAX].split(b"\0", 1)[0].decode("ascii", errors="replace"),
           "vid": _u16(raw, 6), "pid": _u16(raw, 8), "id_hi": _u16(raw, 10), "pd_rev": raw[12],
           "pdos": [_u32(raw, HEADER_BYTES + 4 * i) for i in range(n_pdos)], "request": None, "declared": None, "observed": None,
           "bytes": len(raw), "raw": raw.hex()}
    at = HEADER_BYTES + 4 * n_pdos
    if req:
        out["request"] = {"rdo": _u32(raw, at), "pdo": _u32(raw, at + 4)}
        at += 8
    if decl:
        npt, nsv, nmo = raw[at], raw[at + 1], raw[at + 2]
        at += 3
        pt = [_u32(raw, at + 4 * i) for i in range(npt)]
        at += 4 * npt
        svids = [_u16(raw, at + 2 * i) for i in range(nsv)]
        at += 2 * nsv
        modes = [{"svid": _u16(raw, at + 6 * i), "vdo": _u32(raw, at + 6 * i + 2)} for i in range(nmo)]
        at += 6 * nmo
        out["declared"] = {"pt": pt, "svids": svids, "modes": modes}
    if obs:
        out["observed"] = {"partner_vid": _u16(raw, at), "family": raw[at + 2], "tbt": raw[at + 3], "flags": raw[at + 4],
                           "dp_pin": raw[at + 5], "mbps": _u32(raw, at + 6), "enter_usb": _u32(raw, at + 10),
                           "tbt_enter": _u32(raw, at + 14)}
    return out


def pdo_text(pdo):
    """A sink PDO as the Sink_Capabilities message carries it: "9.0 V 3.0 A", "PPS 3.3-11.0 V 2.2 A"."""
    kind = pdo >> 30
    if kind == 0:
        return f"{((pdo >> 10) & 0x3FF) * 50 / 1000:.1f} V {(pdo & 0x3FF) * 10 / 1000:.1f} A"
    if kind == 1:
        return f"battery {((pdo >> 10) & 0x3FF) * 50 / 1000:.1f}-{((pdo >> 20) & 0x3FF) * 50 / 1000:.1f} V {(pdo & 0x3FF) * 0.25:g} W"
    if kind == 2:
        return f"variable {((pdo >> 10) & 0x3FF) * 50 / 1000:.1f}-{((pdo >> 20) & 0x3FF) * 50 / 1000:.1f} V {(pdo & 0x3FF) * 10 / 1000:.1f} A"
    if (pdo >> 28) & 3 == 0:
        return (f"PPS {((pdo >> 8) & 0xFF) * 100 / 1000:.1f}-{((pdo >> 17) & 0xFF) * 100 / 1000:.1f} V "
                f"{(pdo & 0x7F) * 50 / 1000:.1f} A")
    return f"APDO 0x{pdo:08X}"


# ---- responses ---------------------------------------------------------------------------------------------------------------

def _ok(p, n, what):
    """The err byte, then at least n bytes in all."""
    if not p:
        raise TwinError(f"twin {what}: empty response")
    if p[0]:
        raise TwinError(f"twin {what}: {ERR_NAMES.get(p[0], f'error {p[0]}')}")
    if len(p) < n:
        raise TwinError(f"twin {what}: response too short ({len(p)} < {n})")


check = _ok   # for callers that only need the err byte read (rename, delete)

# Printed with every USB line: what the K2 can and cannot know about the data lanes.
USB_HONESTY = ("The SuperSpeed and USB4 lanes are not wired to the K2, so a speed here is declared by the device or configured "
               "by PD messages, never a measured throughput. \"TB4 class\" and \"TB5 class\" are inferences from USB4 at 40G or "
               "more with the Thunderbolt mode listed, or from USB4 80G; the certification itself cannot be proven.")


def decode_list(p):
    _ok(p, 2, "list")
    if len(p) < 2 + p[1]:
        raise TwinError(f"twin list: response too short ({len(p)} < {2 + p[1]})")
    return list(p[2:2 + p[1]])


def chunk_info(p):
    """One GET part: (seq, part, parts, bytes)."""
    _ok(p, 5, "get")
    if len(p) < 5 + p[4]:
        raise TwinError("twin get: chunk shorter than it says")
    return p[1], p[2], p[3], bytes(p[5:5 + p[4]])


def decode_predict(p):
    _ok(p, 24, "predict")
    o = {"seq": p[1], "kind": KINDS[p[2]] if p[2] < len(KINDS) else f"?{p[2]}",
         "limiter": LIMITERS[p[3]] if p[3] < len(LIMITERS) else f"?{p[3]}", "mismatch": bool(p[4] & 1), "observed": bool(p[4] & 2),
         "pos": p[5], "mv": _u16(p, 6), "ma": _u16(p, 8), "mw": _u32(p, 10), "want_mw": _u32(p, 14),
         "claim_w": _u16(p, 18), "cable_ma": _u16(p, 20), "cable_mv": _u16(p, 22)}
    o["summary"] = summary(o)
    o["note"] = note(o["limiter"])
    return o


def decode_status(p):
    _ok(p, 9, "status")
    mask = _u16(p, 7)
    return {"running": bool(p[1]), "reason": REASONS[p[2]] if p[2] < len(REASONS) else f"?{p[2]}", "count": p[3], "done": p[4],
            "current": p[5], "restoring": bool(p[6]), "with_verdict": [s for s in range(1, KAPACITY + 1) if mask & (1 << (s - 1))]}


def decode_slot(p):
    _ok(p, 6, "result")
    return {"seq": p[1], "verdict": VERDICTS[p[2]] if p[2] < len(VERDICTS) else f"?{p[2]}",
            "skip": SKIPS[p[3]] if p[3] < len(SKIPS) else f"?{p[3]}", "measured_mv": _u16(p, 4)}


def decode_run(p):
    """[refusal, n]: refusal 0 = started. Not an error here: the caller words the refusal."""
    if len(p) < 2:
        raise TwinError("twin run: response too short")
    return {"started": p[0] == 0, "refusal": REFUSALS[p[0]] if p[0] < len(REFUSALS) else f"?{p[0]}", "n": p[1]}


def decode_learn(p):
    if p and p[0] == 9 and len(p) >= 2:   # kErrLearn: `how` says why
        how = LEARN_NAMES[p[1]] if p[1] < len(LEARN_NAMES) else f"?{p[1]}"
        hint = LEARN_HINTS.get(how)
        raise TwinError(f"twin learn: {how}" + (f" ({hint})" if hint else ""))
    _ok(p, 11, "learn")
    flags = p[10]
    return {"seq": p[2], "existed": bool(p[3]), "unchanged": bool(p[4]), "vid": _u16(p, 5), "pid": _u16(p, 7), "num_pdos": p[9],
            "has": [n for bit, n in ((FLAG_REQUEST, "request"), (FLAG_DECLARED, "declared"), (FLAG_OBSERVED, "observed")) if flags & bit]}


def decode_usb(p):
    _ok(p, 4, "usb")
    if len(p) < 4 + p[3]:
        raise TwinError("twin usb: text shorter than it says")
    return {"seq": p[1], "which": USB_WHICH[p[2]] if p[2] < len(USB_WHICH) else f"?{p[2]}",
            "text": bytes(p[4:4 + p[3]]).decode("ascii", errors="replace")}


# ---- texts (mirror include/k2/twin/present.hpp) -----------------------------------------------------------------------------

def _whole_w(mw):
    return (mw + 500) // 1000


def volts(mv):
    d = (mv + 50) // 100
    return f"{d // 10}.{d % 10}V"


def summary(o):
    """What the device would get: "27W PPS", "36W 12V", "60W of 96W", "5V only", "no contract"."""
    if o["kind"] == "none":
        return "no contract"
    if o["kind"] == "five_v":
        return "5V only"
    s = f"{_whole_w(o['mw'])}W"
    if _whole_w(o["want_mw"]) > _whole_w(o["mw"]):
        return f"{s} of {_whole_w(o['want_mw'])}W"
    return s + (" PPS" if o["kind"] == "pps" else f" {(o['mv'] + 500) // 1000}V")


def note(limiter):
    return {"none": "full", "device": "the device takes no more", "charger": "limited by this charger",
            "cable": "limited by the cable"}.get(limiter, "?")


def proof(verdict, measured_mv, o):
    """The live verdict line: "27W PPS ok 9.0V", "refused", "out of band 7.4V"."""
    if verdict == "granted":
        return f"{summary(o)} ok {volts(measured_mv)}"
    if verdict == "out of band" and measured_mv:
        return f"{verdict} {volts(measured_mv)}"
    return verdict
