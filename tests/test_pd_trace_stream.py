# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host reader of `trace stream` (tools/pd_trace.py): frame parser, hole reporting, outputs."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import pd_trace as pt  # noqa: E402


def rec(rtype, ts, payload=b""):
    return bytes([6 + len(payload), rtype]) + struct.pack("<I", ts) + bytes(payload)


def frame(kind, payload):
    body = bytes([kind, len(payload)]) + payload
    return bytes([0xA6]) + body + struct.pack("<H", pt.crc16(body))


def start(all_=False, cursor=0, head=0, tail=0):
    return frame(1, bytes([1 if all_ else 0]) + struct.pack("<III", cursor, head, tail))


def data(offset, *records):
    return frame(2, struct.pack("<I", offset) + b"".join(records))


def lost(records, nbytes, restart=False):
    return frame(3, struct.pack("<IIB", records, nbytes, 1 if restart else 0))


class Collect:
    """A Sink stand-in: remembers events, notes and raw bytes."""

    def __init__(self):
        self.events, self.notes, self.markers, self.rawbytes = [], [], [], b""

    def event(self, ev):
        self.events.append(ev)

    def note(self, text, marker):
        self.notes.append(text)
        self.markers.append(marker)

    def raw(self, b):
        self.rawbytes += b


R1 = rec(0x10, 1000, b"\x01")      # an unknown type decodes as raw text, enough to count and order them
R2 = rec(0x10, 2000, b"\x02\x03")
R3 = rec(0x10, 3000)


class Crc(unittest.TestCase):
    def test_ccitt_false_check_value(self):
        self.assertEqual(pt.crc16(b"123456789"), 0x29B1)    # the standard check value, same as the firmware's


class Parser(unittest.TestCase):
    def test_frames_round_trip_and_text_is_skipped(self):
        stream = b"ok\r\n" + start() + b"trace\r\n" + data(0, R1, R2)
        frames, bad, rest = pt.parse_frames(stream)
        self.assertEqual([k for k, _ in frames], [1, 2])
        self.assertEqual((bad, rest), (0, b""))
        self.assertEqual(frames[1][1], struct.pack("<I", 0) + R1 + R2)

    def test_bad_crc_is_counted_and_the_next_frame_survives(self):
        f = bytearray(data(0, R1))
        f[-1] ^= 0xFF
        frames, bad, rest = pt.parse_frames(bytes(f) + data(len(R1), R2))
        self.assertEqual(bad, 1)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][1][4:], R2)

    def test_resync_after_a_stray_sync_byte(self):
        frames, bad, _ = pt.parse_frames(b"\xa6\x09\xa6\x07" + start() + lost(1, 2))
        self.assertEqual([k for k, _ in frames], [1, 3])

    def test_partial_frame_is_kept_until_the_rest_arrives(self):
        f = data(0, R1, R2)
        frames, bad, rest = pt.parse_frames(f[:-3])
        self.assertEqual((frames, bad, rest), ([], 0, f[:-3]))
        frames, bad, rest = pt.parse_frames(rest + f[-3:])
        self.assertEqual((len(frames), bad, rest), (1, 0, b""))

    def test_multiplexed_kinds_parse_and_the_trace_reader_ignores_them(self):
        hello = frame(4, bytes(10) + b"abc1234")
        beat = frame(10, bytes(50))
        frames, bad, _ = pt.parse_frames(hello + start() + beat + data(0, R1))
        self.assertEqual([k for k, _ in frames], [4, 1, 10, 2])
        sink = Collect()
        ts = pt.TraceStream(sink)
        ts.feed(hello + start() + beat + data(0, R1) + frame(5, b"\0\0\0\0\1K2B1"))
        self.assertEqual((ts.records, ts.bad), (1, 0))

    def test_oversize_length_is_not_a_frame(self):
        frames, bad, rest = pt.parse_frames(b"\xa6\x02\xff" + bytes(10))
        self.assertEqual((frames, rest), ([], b""))


class Reader(unittest.TestCase):
    def feed(self, *chunks, split=None):
        sink = Collect()
        ts = pt.TraceStream(sink)
        blob = b"".join(chunks)
        if split:
            for i in range(0, len(blob), split):
                ts.feed(blob[i:i + split])
        else:
            ts.feed(blob)
        return ts, sink

    def test_clean_stream_decodes_every_record_in_order(self):
        ts, sink = self.feed(start(True), data(0, R1, R2), data(len(R1) + len(R2), R3))
        self.assertEqual(ts.records, 3)
        self.assertEqual([e["t_us"] for e in sink.events], [1000, 2000, 3000])
        self.assertEqual(sink.rawbytes, R1 + R2 + R3)
        self.assertEqual((ts.bad, ts.gaps, ts.lost_records), (0, 0, 0))
        self.assertTrue(ts.started)

    def test_byte_at_a_time_gives_the_same_result(self):
        whole, _ = self.feed(start(), data(0, R1, R2), lost(5, 99), data(500, R3))
        trickle, sink = self.feed(start(), data(0, R1, R2), lost(5, 99), data(500, R3), split=1)
        self.assertEqual(trickle.records, whole.records)
        self.assertEqual(trickle.lost_records, 5)
        self.assertEqual(trickle.frames, whole.frames)

    def test_lost_frame_is_reported_with_its_counts_and_not_confused_with_a_gap(self):
        ts, sink = self.feed(start(), data(0, R1), lost(7, 300), data(400, R2))
        self.assertEqual((ts.lost_records, ts.lost_bytes, ts.gaps), (7, 300, 0))
        self.assertTrue(any("lost 7 records (300 bytes)" in n for n in sink.notes))
        self.assertEqual(ts.records, 2)

    def test_offset_gap_between_frames_is_reported(self):
        ts, sink = self.feed(start(), data(0, R1), data(len(R1) + 50, R2))
        self.assertEqual(ts.gaps, 1)
        self.assertTrue(any("50 ring bytes missing" in n for n in sink.notes))
        self.assertEqual(ts.records, 2)

    def test_ring_restart_resets_the_timeline_epoch(self):
        ts, sink = self.feed(start(), data(0, R2), lost(0, 0, restart=True), data(0, R1))
        self.assertEqual(ts.restarts, 1)
        self.assertEqual(ts.lost_records, 0)
        self.assertEqual([e["t_us"] for e in sink.events], [2000, 1000])    # no wrap compensation across the restart

    def test_a_corrupt_record_inside_a_good_crc_frame_is_dropped_not_decoded(self):
        ts, sink = self.feed(start(), frame(2, struct.pack("<I", 0) + bytes([3, 0x10, 0])))
        self.assertEqual((ts.records, ts.bad), (0, 1))

    def test_bad_crc_is_a_note(self):
        f = bytearray(data(0, R1))
        f[5] ^= 1
        ts, sink = self.feed(start(), bytes(f))
        self.assertEqual((ts.bad, ts.records), (1, 0))
        self.assertTrue(any("bad CRC" in n for n in sink.notes))

    def test_device_stats_line_after_stop(self):
        ts, _ = self.feed(start(), data(0, R1), b"ok\r\ntrace stats records=1 lost=0\r\n")
        self.assertEqual(ts.device_stats(), (1, 0))
        self.assertIn("device sent 1 records, reported 0 lost", ts.summary())

    def test_summary_without_stats_line_says_so(self):
        ts, _ = self.feed(start())
        self.assertIn("no `trace stats` line", ts.summary())


class Outputs(unittest.TestCase):
    def test_raw_and_jsonl_files(self):
        with tempfile.TemporaryDirectory() as d:
            raw, jl = Path(d) / "t.raw", Path(d) / "t.jsonl"
            lines = []
            sink = pt.Sink(str(jl), str(raw), out=lines.append)
            ts = pt.TraceStream(sink)
            ts.feed(start() + data(0, R1, R2) + lost(2, 80) + data(90, R3))
            sink.close()
            self.assertEqual(raw.read_bytes(), R1 + R2 + R3)
            rows = [json.loads(x) for x in jl.read_text().splitlines()]
            self.assertEqual([r.get("marker") for r in rows], ["start", None, None, "lost", None])
            self.assertEqual(rows[3]["records"], 2)
            evs, rest, _ = pt.decode_buffer(raw.read_bytes())    # the raw file is what `decode` reads
            self.assertEqual((len(evs), rest), (3, b""))
            self.assertTrue(any("lost 2 records" in x for x in lines))


if __name__ == "__main__":
    unittest.main()
