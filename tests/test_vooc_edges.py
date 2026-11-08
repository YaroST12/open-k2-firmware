# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import vooc_edges as ve  # noqa: E402


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def record(events, idx=0):
    """events: [(units, state)] -> one framed Edges record (same layout as k2/fastcharge/edges.hpp)."""
    p = bytearray([idx]) + struct.pack("<I", events[0][0])
    prev = events[0][0]
    for u, st in events:
        p += varint((((u - prev) & ve.UNIT_MASK) << 2) | st)
        prev = u
    return bytes([6 + len(p), ve.T_EDGES]) + struct.pack("<I", 0) + bytes(p)


def frame_events(t0_us, bits, phone=False):
    """One frame in the observed model: 9 D+ low pulses (30 us, 0.46/0.54 ms apart); D- carries one
    bit per pulse, changing inside the pulse (adapter) or 50 us after it rises (phone)."""
    dm = 1 if phone else 0
    ev, t = [(t0_us - 100, dm, 1)], t0_us
    for b in bits:
        ev.append((t, dm, 0))
        if not phone and b != dm:
            dm = b
            ev.append((t + 20, dm, 0))
        ev.append((t + 30, dm, 1))
        if phone and b != dm:
            dm = b
            ev.append((t + 80, dm, 1))
        t += 460 if len(ev) % 2 else 540
    return ev


def byte_bits(v, trailer=0):
    return [(v >> (7 - i)) & 1 for i in range(8)] + [trailer]


def to_units(ev):
    return [(int(round(t / ve.UNIT_US)), (dm << 1) | dp) for t, dm, dp in ev]


FIXTURE = Path(__file__).resolve().parent / "fixtures" / "oneplus15_svooc_init.edges"


class VoocEdges(unittest.TestCase):
    def test_chunk_golden(self):
        # same bytes as tests/test_edges.cpp test_golden
        p = bytes([7, 0x02, 0x01, 0, 0, 1, (3 << 2) | 3])
        idx, ev = ve.decode_chunk(p)
        self.assertEqual(idx, 7)
        self.assertEqual(ev, [(0x102, 1), (0x105, 3)])

    def test_pair_synthetic(self):
        reply = [0, 0, 0, 0, 0, 1, 1, 0, 1]
        ev = frame_events(100, byte_bits(0xA8)) + frame_events(9100, reply, phone=True) \
            + frame_events(40000, byte_bits(0x82))
        # duplicate events from the racing EXTI lines must not matter
        ev = [x for e in ev for x in (e, (e[0] + 0.4, e[1], e[2]))]
        frames = ve.decode_frames(ve.edges_from_records(record(to_units(ev))))
        self.assertEqual([f["role"] for f in frames], ["cmd", "reply", "cmd"])
        self.assertEqual(frames[0]["value"] >> 1, 0xA8)
        self.assertEqual(frames[1]["value"], 0x00D)
        self.assertIn("cmd=0xA8 (ident)", ve.annotate(frames[0]))
        self.assertIn("reply=0x00D", ve.annotate(frames[1]))
        self.assertTrue(all(f["complete"] for f in frames))

    def test_partial_frame(self):
        ev = frame_events(100, byte_bits(0x82))[:-5]
        frames = ve.decode_frames(ve.edges_from_records(record(to_units(ev))))
        self.assertEqual(len(frames), 1)
        self.assertFalse(frames[0]["complete"])
        self.assertIn("partial", ve.annotate(frames[0]))

    def test_oneplus15_golden(self):
        """Real capture: OnePlus 15 + SUPERVOOC 120 W initial handshake (build/sn/handshake2, 0.68-1.48 s)."""
        frames = ve.decode_frames(ve.edges_from_records(FIXTURE.read_bytes()))
        pairs = [(a["value"] >> 1, b["value"]) for a, b in zip(frames[::2], frames[1::2])]
        self.assertEqual([f["role"] for f in frames], ["cmd", "reply"] * (len(frames) // 2))
        self.assertTrue(all(f["complete"] for f in frames))
        self.assertEqual(pairs[:3], [(0xA8, 0x00D), (0x98, 0x073), (0x98, 0x023)])
        self.assertIn((0x82, 0x074), pairs)
        self.assertIn((0x88, 0x05D), pairs)
        self.assertIn((0x88, 0x04D), pairs)
        self.assertEqual(pairs[-1], (0x94, 0x000))    # the phone stays silent after the cable check
        self.assertEqual(len(pairs), 25)

    def test_unwrap_across_records(self):
        top = (1 << 26) - 40
        a = record([(top, 1), (top + 10, 0)], 0)
        b = record([(5, 1), (15, 0)], 1)      # wrapped counter
        edges = ve.edges_from_records(a + b)
        ts = [e[0] for e in edges]
        self.assertEqual(ts, sorted(ts))
        self.assertAlmostEqual(ts[2] - ts[1], (45 - 10 + 0) * ve.UNIT_US, delta=ve.UNIT_US)

    def test_empty(self):
        self.assertEqual(ve.report([]), ["0 edges"])
        self.assertEqual(ve.decode_frames([]), [])


if __name__ == "__main__":
    unittest.main()
