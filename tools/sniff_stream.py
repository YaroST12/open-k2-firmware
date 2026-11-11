#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Live D+/D- edge stream from the K2 over the CDC shell (passive: it never drives).

  python3 tools/sniff_stream.py [--port /dev/ttyACM0] [--seconds N] [--out PREFIX]
  python3 tools/sniff_stream.py --synth 3000 --seconds 5      synthetic source, no pins: measures the CDC path

It sends `sniff stream`, collects the framed records until N seconds pass or Ctrl-C, sends `sniff stop`,
then writes PREFIX.raw (every byte received) and PREFIX.edges (Edges records, the format of
`vooc_edges.py file`), and prints the vooc_edges summary (sessions, handshakes, commands). Frame format: 0xA5, len, payload, sum8(len+payload);
the payload is one include/k2/fastcharge/edges.hpp chunk (idx, first_units, varint edges).
"""
import argparse
import os
import re
import select
import sys
import termios
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vooc_edges  # noqa: E402

SYNC = 0xA5
MAX_PAYLOAD = 43


def parse_frames(buf):
    """bytes -> (payloads, bad, rest). Resyncs on SYNC; a frame with a wrong sum counts as bad.
    `rest` is the unparsed tail (a frame still arriving)."""
    out, bad, i = [], 0, 0
    while True:
        i = buf.find(bytes([SYNC]), i)
        if i < 0:
            return out, bad, b""
        if i + 2 > len(buf):
            return out, bad, bytes(buf[i:])
        n = buf[i + 1]
        if n < 5 or n > MAX_PAYLOAD:
            i += 1
            continue
        if i + n + 3 > len(buf):
            return out, bad, bytes(buf[i:])
        pl = bytes(buf[i + 2:i + 2 + n])
        if (n + sum(pl)) & 0xFF == buf[i + 2 + n]:
            out.append(pl)
            i += n + 3
        else:
            bad += 1
            i += 1


def lost_frames(payloads):
    """Frames missing between consecutive payload idx bytes (idx counts frames mod 256)."""
    lost, prev = 0, None
    for p in payloads:
        if prev is not None:
            lost += (p[0] - prev - 1) & 0xFF
        prev = p[0]
    return lost


def edges_file(payloads):
    """Payloads -> Edges records readable by vooc_edges.edges_from_records."""
    return b"".join(bytes([6 + len(p), vooc_edges.T_EDGES, 0, 0, 0, 0]) + p for p in payloads)


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def read_for(fd, seconds, sink):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            sink(os.read(fd, 4096))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--seconds", type=float, default=0, help="stop after N seconds (default: until Ctrl-C)")
    ap.add_argument("--out", default="sniff", help="output file prefix")
    ap.add_argument("--synth", type=int, default=0, help="synthetic source at this edges/s (no pins touched)")
    a = ap.parse_args(argv)

    fd = open_port(a.port)
    raw = bytearray()
    pend = bytearray()
    payloads, bad = [], 0

    def sink(b):
        nonlocal pend, bad
        raw.extend(b)
        pend.extend(b)
        got, nb, rest = parse_frames(pend)
        pend = bytearray(rest)
        payloads.extend(got)
        bad += nb

    os.write(fd, b"\r\n")
    read_for(fd, 0.3, lambda b: None)
    cmd = f"sniff synth {a.synth}" if a.synth else "sniff stream"
    os.write(fd, (cmd + "\r\n").encode())
    t0 = time.monotonic()
    try:
        while not a.seconds or time.monotonic() - t0 < a.seconds:
            read_for(fd, 0.1, sink)
    except KeyboardInterrupt:
        pass
    dt = time.monotonic() - t0
    os.write(fd, b"sniff stop\r\n")
    read_for(fd, 1.0, sink)
    os.close(fd)

    text = re.sub(rb"[^\x20-\x7e\r\n]", b"", bytes(raw)).decode()
    m = re.search(r"sniff stats edges=(\d+) overruns=(\d+) bytes=(\d+)", text)
    Path(a.out + ".raw").write_bytes(bytes(raw))
    Path(a.out + ".edges").write_bytes(edges_file(payloads))
    edges = vooc_edges.edges_from_records(edges_file(payloads))
    print(f"{dt:.2f} s, {len(payloads)} frames, {len(edges)} edges decoded, {bad} bad frames, "
          f"{lost_frames(payloads)} lost frames (idx gaps), {len(raw)} bytes received ({len(raw) / dt:.0f} B/s)")
    if m:
        print(f"device: {m.group(1)} edges queued, {m.group(2)} overruns, {m.group(3)} bytes framed")
    else:
        print("no 'sniff stats' line received (check the port, the shell and that nothing else owns the ports)")
    print("\n".join(vooc_edges.summary_lines(edges)))
    print(f"(every frame: vooc_edges.py file {a.out}.edges; one window: add --edges LO_MS HI_MS)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
