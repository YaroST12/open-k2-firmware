# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import sniff_stream  # noqa: E402
import vooc_edges  # noqa: E402


def frame(payload):
    return bytes([0xA5, len(payload)]) + payload + bytes([(len(payload) + sum(payload)) & 0xFF])


def chunk(idx, first_units, deltas_states):
    p = bytearray([idx]) + first_units.to_bytes(4, "little")
    for d, st in deltas_states:
        v = (d << 2) | st
        while v >= 0x80:
            p.append((v & 0x7F) | 0x80)
            v >>= 7
        p.append(v)
    return bytes(p)


def test_frames_resync_and_sum():
    a, b = chunk(0, 100, [(0, 1), (5, 3)]), chunk(1, 200, [(0, 0)])
    stream = b"ok\r\n" + frame(a) + b"\xa5\x03xx" + frame(b)[:-1] + b"\x00" + frame(b)
    got, bad, rest = sniff_stream.parse_frames(stream)
    assert got == [a, b] and bad >= 1 and rest == b""


def test_partial_frame_is_kept():
    f = frame(chunk(0, 1, [(0, 1)]))
    got, bad, rest = sniff_stream.parse_frames(f[:-2])
    assert got == [] and rest == f[:-2]
    got, _, rest = sniff_stream.parse_frames(rest + f[-2:])
    assert len(got) == 1 and rest == b""


def test_lost_frames_and_edges_roundtrip():
    ps = [chunk(i, 1000 * i, [(0, 1), (10, 0), (20, 2)]) for i in (254, 255, 1)]
    assert sniff_stream.lost_frames(ps) == 1   # 0 missing
    ev = vooc_edges.edges_from_records(sniff_stream.edges_file(ps))
    assert len(ev) == 9
