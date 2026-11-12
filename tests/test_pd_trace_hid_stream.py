# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host reader of `hid-stream` (tools/pd_trace.py): the 0xA7 report parser, hole reporting, the keepalive and the
clean stop. The device is a scripted fake transport; the report layout is written out here by hand, independent of
the firmware encoder (k2/core/trace_hid.hpp) and of the reader."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import k2_remote as kr  # noqa: E402
import pd_trace as pt  # noqa: E402


def rec(rtype, ts, payload=b""):
    return bytes([6 + len(payload), rtype]) + struct.pack("<I", ts) + bytes(payload)


def report(kind, seq, payload=b""):
    return (bytes([0xA7, kind, seq & 0xFF, len(payload)]) + payload).ljust(64, b"\x00")


def start(seq=0, all_=False, cursor=0, head=0, tail=0):
    return report(1, seq, bytes([1 if all_ else 0]) + struct.pack("<III", cursor, head, tail))


def data(seq, offset, *records):
    return report(2, seq, struct.pack("<I", offset) + b"".join(records))


def lost(seq, records, nbytes, restart=False):
    return report(3, seq, struct.pack("<IIB", records, nbytes, 1 if restart else 0))


def end(seq, reason=1):
    return report(0x10, seq, bytes([reason]))


class Collect:
    def __init__(self):
        self.events, self.notes, self.markers, self.rawbytes = [], [], [], b""

    def event(self, ev):
        self.events.append(ev)

    def note(self, text, marker):
        self.notes.append(text)
        self.markers.append(marker)

    def raw(self, b):
        self.rawbytes += b


R1 = rec(0x10, 1000, b"\x01")
R2 = rec(0x10, 2000, b"\x02\x03")
R3 = rec(0x10, 3000)


class Reader(unittest.TestCase):
    def feed(self, *reports):
        sink = Collect()
        hs = pt.HidTraceStream(sink)
        for r in reports:
            hs.feed_report(r)
        return hs, sink

    def test_clean_stream_decodes_every_record_in_order(self):
        hs, sink = self.feed(start(0, True), data(1, 0, R1, R2), data(2, len(R1) + len(R2), R3))
        self.assertEqual(hs.ts.records, 3)
        self.assertEqual([e["t_us"] for e in sink.events], [1000, 2000, 3000])
        self.assertEqual(sink.rawbytes, R1 + R2 + R3)
        self.assertEqual((hs.reports, hs.missed_reports, hs.ts.bad, hs.ts.gaps, hs.ts.lost_records), (3, 0, 0, 0, 0))
        self.assertEqual(hs.payload_bytes, 13 + 4 + len(R1 + R2) + 4 + len(R3))

    def test_other_traffic_is_ignored(self):
        hs, sink = self.feed(b"[LOG] text line".ljust(64, b"\0"), bytes([0xA6, 0x4B]) + bytes(62), b"", b"\xA7\x02",
                             start(0), data(1, 0, R1))
        self.assertEqual((hs.reports, hs.ts.records), (2, 1))

    def test_lost_report_is_reported_with_its_counts_and_is_not_a_gap(self):
        hs, sink = self.feed(start(0), data(1, 0, R1), lost(2, 7, 300), data(3, 400, R2))
        self.assertEqual((hs.ts.lost_records, hs.ts.lost_bytes, hs.ts.gaps, hs.missed_reports), (7, 300, 0, 0))
        self.assertTrue(any("lost 7 records (300 bytes)" in n for n in sink.notes))
        self.assertEqual(hs.ts.records, 2)

    def test_a_missing_report_is_counted_by_the_sequence_number(self):
        hs, sink = self.feed(start(0), data(1, 0, R1), data(4, len(R1) + 50, R2))   # reports 2 and 3 never arrived
        self.assertEqual(hs.missed_reports, 2)
        self.assertTrue(any("2 report(s) missing" in n for n in sink.notes))
        self.assertEqual(hs.ts.gaps, 1)                  # and the ring offset shows the hole as well

    def test_the_sequence_number_wraps_at_256(self):
        hs, _ = self.feed(start(254), data(255, 0, R1), data(0, len(R1), R2), data(1, len(R1) + len(R2), R3))
        self.assertEqual((hs.missed_reports, hs.ts.records), (0, 3))

    def test_a_new_start_restarts_the_sequence(self):
        hs, _ = self.feed(start(0), data(1, 0, R1), start(0), data(1, 0, R2))
        self.assertEqual((hs.missed_reports, hs.ts.records, hs.ts.restarts), (0, 2, 0))

    def test_partial_record_is_dropped_and_counted(self):
        hs, sink = self.feed(start(0), report(2, 1, struct.pack("<I", 0) + R1[:-2]))
        self.assertEqual((hs.ts.records, hs.ts.bad), (0, 1))

    def test_end_report_marks_the_stream_over(self):
        hs, sink = self.feed(start(0), data(1, 0, R1), end(2))
        self.assertTrue(hs.ended)
        self.assertEqual(hs.end_reason, 1)
        self.assertTrue(any("keepalive timeout" in n for n in sink.notes))

    def test_ring_restart_lost_report_starts_a_new_epoch(self):
        hs, sink = self.feed(start(0), lost(1, 0, 0, True))
        self.assertEqual(hs.ts.restarts, 1)


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t


class FakeDevice:
    """A K2 at the HID level: answers 0x73 requests, plays a script of reports after the start request."""

    def __init__(self, clock, script, interrupt_after=None):
        self.clock, self.script, self.queue = clock, script, []
        self.requests = []            # (op, payload) of every 0x73 request
        self.interrupt_after = interrupt_after
        self.reads = 0

    def rsp(self, cmd, seq, payload):
        return (bytes([0xA6, 0x4B, cmd, seq, 0, len(payload)]) + payload).ljust(64, b"\x00")

    def write(self, frame):
        assert frame[:2] == b"\xA5\x4B"
        cmd, seq, n = frame[2], frame[3], frame[4]
        payload = bytes(frame[5:5 + n])
        assert cmd == kr.CMD_TRACE_HID
        self.requests.append(payload)
        self.queue.append(self.rsp(cmd, seq, bytes([1 if payload[0] in (kr.TH_START, kr.TH_KEEPALIVE) else 0])
                                   + bytes(12)))
        if payload[0] == kr.TH_START:
            self.queue.extend(self.script)
        return 64

    def read(self, ms):
        self.reads += 1
        if self.interrupt_after is not None and self.reads > self.interrupt_after:
            self.interrupt_after = None          # Ctrl-C arrives once
            raise KeyboardInterrupt
        if self.queue:
            return self.queue.pop(0)
        self.clock.t += ms / 1000.0
        return b""

    def close(self):
        pass


class Run(unittest.TestCase):
    def run_stream(self, script, **kw):
        clock = Clock()
        dev = FakeDevice(clock, script, kw.pop("interrupt_after", None))
        link = kr.RemoteLink(dev, 1.0)
        sink = Collect()
        hs = pt.run_hid_stream(link, sink, clock=clock, **kw)
        return hs, dev, sink, clock

    def test_start_request_carries_the_flags_and_the_timeout(self):
        hs, dev, _, _ = self.run_stream([start(0), data(1, 0, R1)], seconds=0.2, timeout_ms=3000)
        self.assertEqual(dev.requests[0], bytes([kr.TH_START, 0]) + struct.pack("<H", 3000))
        hs, dev, _, _ = self.run_stream([start(0, True), data(1, 0, R1)], seconds=0.2, all_records=True, timeout_ms=1500)
        self.assertEqual(dev.requests[0], bytes([kr.TH_START, kr.TH_FLAG_ALL]) + struct.pack("<H", 1500))

    def test_records_arriving_with_the_start_answer_are_decoded(self):
        hs, dev, sink, _ = self.run_stream([start(0), data(1, 0, R1, R2)], seconds=0.2)
        self.assertEqual(hs.ts.records, 2)
        self.assertEqual(hs.ts.bad, 0)

    def test_a_report_read_while_waiting_for_the_stop_answer_is_not_lost(self):
        # the device queue holds a Data report ahead of the stop answer: call() skips it, the tap catches it
        clock = Clock()
        dev = FakeDevice(clock, [start(0)])
        link = kr.RemoteLink(dev, 1.0)
        sink = Collect()
        hs = pt.HidTraceStream(sink)
        link.tap = hs.feed_report
        link.call(kr.CMD_TRACE_HID, bytes([kr.TH_START, 0, 0, 0]))
        dev.queue.append(data(1, 0, R1))
        link.call(kr.CMD_TRACE_HID, bytes([kr.TH_STOP]))
        self.assertEqual(hs.ts.records, 1)

    def test_keepalive_is_sent_about_three_times_per_timeout_and_stop_ends_the_run(self):
        hs, dev, _, clock = self.run_stream([start(0)], seconds=10.0, timeout_ms=3000)
        ops = [r[0] for r in dev.requests]
        self.assertEqual(ops[0], kr.TH_START)
        self.assertEqual(ops[-1], kr.TH_STOP)
        keeps = ops.count(kr.TH_KEEPALIVE)
        self.assertTrue(8 <= keeps <= 11, keeps)       # one per second over ten seconds

    def test_ctrl_c_stops_the_stream_cleanly(self):
        hs, dev, _, _ = self.run_stream([start(0), data(1, 0, R1)], interrupt_after=5)
        self.assertEqual([r[0] for r in dev.requests][-1], kr.TH_STOP)
        self.assertEqual(hs.ts.records, 1)

    def test_a_device_that_ended_the_stream_is_not_asked_to_stop_twice_but_the_run_returns(self):
        hs, dev, _, _ = self.run_stream([start(0), data(1, 0, R1), end(2)], seconds=5.0)
        self.assertTrue(hs.ended)
        self.assertEqual(hs.ts.records, 1)

    def test_summary_names_the_holes(self):
        hs, _, _, _ = self.run_stream([start(0), data(1, 0, R1), lost(2, 3, 120)], seconds=0.2)
        s = hs.summary()
        self.assertIn("3 reports", s)
        self.assertIn("1 records", s)
        self.assertIn("3 lost on the device", s)
        self.assertIn("0 missing reports", s)


if __name__ == "__main__":
    unittest.main()
