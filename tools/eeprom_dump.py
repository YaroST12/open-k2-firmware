# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host side of the read-only 24Cxx EEPROM lab dump (remote command DEBUG_EEPROM 0x2B, firmware K2_EEPROM_READ=1).

The firmware only probes (address-only writes to 0x50..0x57, then four 1-byte-address reads at word address 0) and reads at
(bus, addressing, select, offset, length). Everything that interprets those is here, so it costs no firmware flash:
  - classify(): one-byte or two-byte word address, from whether the four read windows repeat;
  - dump(): the chunked read loop;
  - hexdump(): the printed view.
There is no write path in the firmware or here."""
import zlib

BASE7 = 0x50
CHUNK = 32                 # firmware kMaxChunk
BUSES = {"fusb": 0, "ina": 1}
BUS_NAMES = {v: k for k, v in BUSES.items()}
PROBE_READS, PROBE_BYTES = 4, 4
SPAN = {1: 0x800, 2: 0x10000}          # addressable bytes per word-address width (24C16 / 24C512)
DEFAULT_TWO_BYTE = 0x1000              # 24C32; a smaller chip would wrap, so a larger one is asked for with --size
ERR_NAMES = {0: "ok", 1: "nack", 2: "busy"}


def acks(mask):
    """7-bit device addresses of the set bits of the probe's ack mask."""
    return [BASE7 + i for i in range(8) if mask >> i & 1]


def decode_probe(p):
    """Probe reply: [bus, ack_mask, dev7, 4 windows of 4 bytes]."""
    if len(p) < 3 + PROBE_READS * PROBE_BYTES:
        raise ValueError(f"eeprom probe reply too short ({len(p)} bytes)")
    wins = [bytes(p[3 + i * PROBE_BYTES:3 + (i + 1) * PROBE_BYTES]) for i in range(PROBE_READS)]
    return {"bus": p[0], "ack_mask": p[1], "dev7": p[2], "windows": wins}


def classify(probe):
    """(word-address bytes or None, why). A one-byte chip returns the same window every time (the extra byte of a two-byte
    read is what moves a bigger chip's pointer); a two-byte chip takes our byte as the high address and keeps its old low
    pointer, so its windows advance. A blank chip (all one value) cannot be told apart: it is a guess from the ack mask
    (a 24C16 answers at all eight addresses) or None."""
    mask, w = probe["ack_mask"], probe["windows"]
    if not mask:
        return None, "nothing answered at 0x50..0x57"
    if len(set(w)) > 1:
        return 2, "the read windows advanced: a two-byte word address (24C32 or larger)"
    if len(set(b"".join(w))) > 1:
        return 1, "the same window came back four times: a one-byte word address (24C02..24C16)"
    if mask == 0xFF:
        return 1, "uniform content and all eight addresses answer: a one-byte chip (24C16 style), a guess"
    return None, "uniform content (a blank chip?): cannot tell the addressing, pass --addr-bytes"


def span_default(probe, abytes):
    """Default dump size: one-byte chips cover one 256-byte block per consecutive address from the lowest, two-byte chips
    DEFAULT_TWO_BYTE."""
    if abytes == 2:
        return DEFAULT_TWO_BYTE
    mask, run = probe["ack_mask"], 0
    mask >>= (mask & -mask).bit_length() - 1
    while mask & 1:
        run += 1
        mask >>= 1
    return 0x100 * run


def lowest_select(probe):
    return (probe["ack_mask"] & -probe["ack_mask"]).bit_length() - 1


def read_chunk(call, bus, abytes, sel, off, n):
    """One read request; the bytes, or None when the chip NACKed."""
    r = call(bytes([1, bus, abytes, sel]) + off.to_bytes(2, "little") + bytes([n]))
    if len(r) < 4:
        raise ValueError(f"eeprom read reply too short ({len(r)} bytes)")
    err, got_off, got_n = r[0], r[1] | r[2] << 8, r[3]
    if err != 0:
        return None
    if got_off != off or got_n != n or len(r) != 4 + n:
        raise ValueError(f"eeprom read reply does not match the request (off {got_off:#x}, n {got_n}, {len(r)} bytes)")
    return bytes(r[4:4 + n])


def dump(call, bus, abytes, sel, size, progress=None):
    """Read `size` bytes from offset 0 in CHUNK steps. (data, stopped_at): stopped_at is the offset where the chip stopped
    answering (a smaller chip than `size`), None when all of it was read."""
    if size < 1 or size > SPAN[abytes]:
        raise ValueError(f"size must be 1..{SPAN[abytes]:#x} for a {abytes}-byte word address")
    data = bytearray()
    while len(data) < size:
        n = min(CHUNK, size - len(data), 256 - (len(data) & 0xFF) if abytes == 1 else CHUNK)
        got = read_chunk(call, bus, abytes, sel, len(data), n)
        if got is None:
            return bytes(data), len(data)
        data += got
        if progress:
            progress(len(data), size)
    return bytes(data), None


def hexdump(data, base=0):
    lines = []
    for i in range(0, len(data), 16):
        row = data[i:i + 16]
        hexes = " ".join(f"{b:02X}" for b in row).ljust(47)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in row)
        lines.append(f"{base + i:04X}: {hexes}  {text}")
    return "\n".join(lines)


def probe_buses(call, buses):
    out = []
    for bus in buses:
        p = decode_probe(call(bytes([0, bus])))
        abytes, why = classify(p)
        out.append({"bus": BUS_NAMES.get(bus, bus), "bus_id": bus, "ack": [f"0x{a:02X}" for a in acks(p["ack_mask"])],
                    "dev7": p["dev7"], "windows": [w.hex() for w in p["windows"]], "addr_bytes": abytes, "why": why,
                    "_probe": p})
    return out


def run(call, a, progress=None):
    """`a`: .eeprom_cmd ('probe' | 'dump'), .bus (name or None = both), and for dump .out, .size, .sel, .addr_bytes.
    Returns the result dict (printable, JSON-safe) and writes the raw bin for a dump."""
    buses = [BUSES[a.bus]] if a.bus else sorted(BUS_NAMES)
    probes = probe_buses(call, buses)
    if a.eeprom_cmd == "probe":
        return {"mode": "probe", "probes": [{k: v for k, v in p.items() if k != "_probe"} for p in probes]}
    live = [p for p in probes if p["_probe"]["ack_mask"]]
    if not live:
        raise ValueError("no EEPROM answered at 0x50..0x57 on " + " or ".join(p["bus"] for p in probes))
    pr = live[0]
    p = pr["_probe"]
    abytes, how = (a.addr_bytes, f"forced with --addr-bytes {a.addr_bytes}") if a.addr_bytes else (pr["addr_bytes"], pr["why"])
    if abytes is None:
        raise ValueError(how)
    sel = a.sel if a.sel is not None else lowest_select(p)
    size = a.size if a.size else span_default(p, abytes)
    data, stopped = dump(call, pr["bus_id"], abytes, sel, size, progress)
    if not data:
        raise ValueError(f"the chip at 0x{BASE7 + sel:02X} on {pr['bus']} NACKed the first read (wrong --addr-bytes or --sel?)")
    with open(a.out, "wb") as f:
        f.write(data)
    return {"mode": "dump", "bus": pr["bus"], "ack": pr["ack"], "addr_bytes": abytes, "addressing": how, "sel": sel,
            "size_asked": size, "size_read": len(data), "stopped_at": stopped, "out": a.out,
            "crc32": f"{zlib.crc32(data):08X}", "other_buses": [q["bus"] for q in live[1:]],
            "head": hexdump(data[:0x100]), "mid": hexdump(data[0x400:0x410], 0x400) if len(data) >= 0x410 else None}


def render(r):
    if r["mode"] == "probe":
        lines = []
        for p in r["probes"]:
            lines.append(f"{p['bus']}: " + (("ack " + " ".join(p["ack"])) if p["ack"] else "no device at 0x50..0x57"))
            if p["ack"]:
                lines.append("  windows at word address 0 (1-byte reads): " + " ".join(p["windows"]))
            lines.append(f"  addressing: {p['addr_bytes'] if p['addr_bytes'] else 'unknown'} - {p['why']}")
        return "\n".join(lines)
    lines = [f"EEPROM on {r['bus']} (ack {' '.join(r['ack'])}), {r['addr_bytes']}-byte word address, select {r['sel']}: {r['addressing']}"]
    if r["other_buses"]:
        lines.append("also answering on: " + ", ".join(r["other_buses"]) + " (not read)")
    lines.append(f"read {r['size_read']} of {r['size_asked']} bytes -> {r['out']} (crc32 {r['crc32']})"
                 + (f"; the chip stopped answering at 0x{r['stopped_at']:X}" if r["stopped_at"] is not None else ""))
    lines += ["", "0x000..0x0FF:", r["head"]]
    if r["mid"]:
        lines += ["", "0x400..0x40F:", r["mid"]]
    return "\n".join(lines)
