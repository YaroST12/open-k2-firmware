# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host reader of the live debug stream (tools/k2_stream.py): every frame kind, the logdb decode by firmware hash,
loss accounting, the session folder and the view filters."""
import io
import json
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import k2_stream as ks  # noqa: E402
import pd_trace as pt  # noqa: E402


def frame(kind, payload):
    body = bytes([kind, len(payload)]) + payload
    return bytes([0xA6]) + body + struct.pack("<H", pt.crc16(body))


def varint(v):
    out = bytearray()
    while v >= 0x80:
        out.append((v & 0x7F) | 0x80)
        v >>= 7
    out.append(v)
    return bytes(out)


def zigzag(v):
    return varint(((v << 1) ^ (v >> 31)) & 0xFFFFFFFF)


def entry(dt, mid, *args):
    """One BinLog entry; args are ('d', n), ('u', n) or ('s', text)."""
    body = varint(dt) + varint(mid)
    for k, v in args:
        body += zigzag(v) if k == "d" else varint(v) if k == "u" else bytes([len(v)]) + v.encode()
    return bytes([len(body)]) + body


def hello(ms=1000, mask=15, hz=20, history=False, fw="e32bcda"):
    return frame(4, struct.pack("<BBHIB", 1, mask, hz, ms, 1 if history else 0) + fw.encode())


def text(ms, line, tag=1):
    return frame(5, struct.pack("<IB", ms, tag) + line.encode())


def log(base, seq, *entries):
    return frame(6, struct.pack("<II", base, seq) + b"".join(entries))


def power(ms, n=1, valid=1, vbus=5000, ibus=-120, cc1=0xFFFF, cc2=1650, dp=0xFFFF, dm=0xFFFF, window=None):
    p = struct.pack("<IHBHhHHHH", ms, n, valid, vbus, ibus, cc1, cc2, dp, dm)
    if window:
        p += struct.pack("<HHHhh", *window)
    return frame(7, p)


def run(ms, code, a=0, b=0, c=0, d=0):
    return frame(8, struct.pack("<IBBHHH", ms, code, a, b, c, d))


def gap(src, count, nbytes=0):
    return frame(9, struct.pack("<BIIB", src, count, nbytes, 0))


def beat(ms, ctr=(0,) * 9, stack=4000, msp=900, page=1, flags=0, dropped=0):
    return frame(10, struct.pack("<IHHBBI", ms, stack, msp, page, flags, dropped) + struct.pack("<9I", *ctr))


def trace_rec(rtype, ts, payload=b""):
    return bytes([6 + len(payload), rtype]) + struct.pack("<I", ts) + payload


DB = {7: "vbus=%d name=%s flags=%x", 9: "ping"}


def new_session(tmp=None, db=DB, echo=None):
    return ks.Session(tmp, db_for=lambda fw: (dict(db), f"test:{fw}", None), names=ks.Names(), wall=lambda: 5000.0, echo=echo)


def rows(path, stem):
    p = Path(path) / f"{stem}.jsonl"
    return [json.loads(x) for x in p.read_text().splitlines()] if p.exists() else []


class TestKinds(unittest.TestCase):
    def test_hello_loads_logdb_by_hash_and_maps_the_clock(self):
        seen = []
        s = ks.Session(None, db_for=lambda fw: (seen.append(fw) or {1: "x"}, "git:abc", None), names=ks.Names(), wall=lambda: 5000.0)
        s.feed(hello(ms=2000, fw="abc1234-dirty"))
        self.assertEqual(seen, ["abc1234-dirty"])
        self.assertEqual(s.hello["firmware"], "abc1234-dirty")
        self.assertEqual(s.hello["power_hz"], 20)
        self.assertAlmostEqual(s.wall0, 4998.0)

    def test_hello_warning_about_the_db_source_is_kept(self):
        s = ks.Session(None, db_for=lambda fw: ({}, "working-tree", "the build was dirty"), names=ks.Names(), wall=lambda: 1.0)
        out = []
        s.echo = lambda k, line: out.append(line)
        s.feed(hello())
        self.assertIn("the build was dirty", out[0])

    def test_log_entries_decode_with_the_hash_map(self):
        s = new_session()
        s.feed(hello())
        s.feed(log(1000, 0, entry(5, 7, ("d", -3), ("s", "ab"), ("u", 255)), entry(10, 9)))
        self.assertEqual(s.counts["log"], 2)
        out = io.StringIO()
        s2 = new_session(echo=lambda k, line: out.write(line + "\n"))
        s2.feed(hello())
        s2.feed(log(1000, 0, entry(5, 7, ("d", -3), ("s", "ab"), ("u", 255)), entry(10, 9)))
        self.assertIn("vbus=-3 name=ab flags=FF", out.getvalue())
        self.assertIn("1015.000 ms  LOG   ping", out.getvalue())

    def test_unknown_log_id_reads_as_a_placeholder(self):
        out = []
        s = new_session(db={}, echo=lambda k, line: out.append(line))
        s.feed(hello())
        s.feed(log(0, 0, entry(1, 99)))
        self.assertTrue(any("msg#99" in x for x in out))

    def test_power_short_and_windowed(self):
        with tempfile.TemporaryDirectory() as t:
            s = new_session(t)
            s.feed(hello())
            s.feed(power(1100))
            s.feed(power(1200, n=7, vbus=9000, ibus=1500, window=(300, 8800, 9100, 1400, 1650)))
            s.close()
            p = rows(t, "power")
            self.assertEqual(p[0]["vbus_mv"], 5000)
            self.assertEqual(p[0]["ibus_ma"], -120)
            self.assertIsNone(p[0]["cc1_mv"])
            self.assertEqual(p[0]["cc2_mv"], 1650)
            self.assertNotIn("vbus_min", p[0])
            self.assertEqual((p[1]["n"], p[1]["span_ms"], p[1]["vbus_min"], p[1]["vbus_max"], p[1]["ibus_max"]), (7, 300, 8800, 9100, 1650))
            self.assertIn("7 samples over 300 ms", p[1]["text"])

    def test_run_events_get_names_from_the_headers(self):
        names = ks.Names()
        self.assertIn("Pass", names.tab["verdict"])
        self.assertIn("Running", names.tab["stage"])
        self.assertIn("Done", names.tab["state"])
        self.assertEqual(names.get("verdict", names.tab["verdict"].index("Fail")), "Fail")
        self.assertEqual(names.get("verdict", 200), "#200")
        pass_ = names.tab["verdict"].index("Pass")
        self.assertEqual(ks.run_text(names, 4, 3, pass_, 0, 5100), f"row 3 end Pass reason={names.get('reason', 0)} measured=5100 mV")
        self.assertIn("rows_finished=4", ks.run_text(names, 12, names.tab["state"].index("Done"), 4, 0, 0))
        self.assertIn("event 77", ks.run_text(names, 77, 1, 2, 3, 4))

    def test_skipped_rows_burst_reads_as_one_range(self):
        names = ks.Names()
        inline = names.tab["reason"].index("Inline")
        self.assertEqual(ks.run_text(names, 14, 3, 12, inline, 10), "rows 3..12 end Skip reason=Inline (10 rows)")
        self.assertEqual(ks.run_text(names, 14, 7, 7, inline, 1), "row 7 end Skip reason=Inline")
        # the first row is b - d + 1, so a first row past 255 (a clamps there) still reads right
        self.assertEqual(ks.run_text(names, 14, 255, 300, inline, 50), "rows 251..300 end Skip reason=Inline (50 rows)")
        with tempfile.TemporaryDirectory() as t:
            s = new_session(t)
            s.feed(hello())
            s.feed(run(1500, 14, 43, 58, inline, 16) + run(1600, 12, 4, 60, inline))
            s.close()
            r = rows(t, "run")
            self.assertEqual([x["code"] for x in r], [14, 12])
            self.assertIn("rows 43..58 end Skip", r[0]["text"])

    def test_run_frames_land_in_run_jsonl(self):
        with tempfile.TemporaryDirectory() as t:
            s = new_session(t)
            s.feed(hello())
            s.feed(run(1500, 1, 2, 9) + run(1600, 3, 0, 1, 2, 0) + run(1700, 12, 4, 9))
            s.close()
            r = rows(t, "run")
            self.assertEqual([x["code"] for x in r], [1, 3, 12])
            self.assertEqual(s.counts["run_end"], 1)
            self.assertIn("run start units=2 rows=9", r[0]["text"])

    def test_beat_and_boot_text(self):
        with tempfile.TemporaryDirectory() as t:
            s = new_session(t)
            s.feed(hello())
            s.feed(text(1000, "[BOOT] reset=pin\nfw ") + text(1000, "e32bcda\n"))
            s.feed(beat(2000, ctr=(1, 2, 3, 4, 5, 6, 7, 8, 9), stack=3900))
            s.close()
            b = rows(t, "boot")
            self.assertEqual([x["text"] for x in b if x["kind"] == "boot"], ["[BOOT] reset=pin", "fw e32bcda"])
            h = rows(t, "beat")[0]
            self.assertEqual((h["stack_free"], h["counters"]["trace_lost"], h["counters"]["stalls"]), (3900, 2, 9))

    def test_trace_kinds_are_delegated_to_pd_trace(self):
        with tempfile.TemporaryDirectory() as t:
            s = new_session(t)
            s.feed(hello())
            s.feed(frame(1, bytes([0]) + struct.pack("<III", 0, 0, 0)))
            s.feed(frame(2, struct.pack("<I", 0) + trace_rec(1, 1_500_000) + trace_rec(1, 1_600_000)))
            s.feed(frame(3, struct.pack("<IIB", 4, 40, 0)))
            s.close()
            tr = rows(t, "trace")
            self.assertEqual(len([x for x in tr if x["kind"] == "trace" and "type" in x]), 2)
            self.assertEqual(s.trace.records, 2)
            self.assertEqual(s.trace.lost_records, 4)
            self.assertTrue((Path(t) / "trace.raw").stat().st_size > 0)
            g = rows(t, "gaps")
            self.assertTrue(any("device lost 4 records" in x["text"] for x in g))

    def test_frames_split_across_reads_and_noise_between_them(self):
        s = new_session()
        blob = b"\r\nnoise" + hello() + b"junk" + run(1, 1, 1, 1) + beat(5)
        for i in range(0, len(blob), 3):
            s.feed(blob[i:i + 3])
        self.assertEqual((s.hello is not None, s.counts["run"], s.counts["beat"]), (True, 1, 1))

    def test_bad_crc_is_reported_not_swallowed(self):
        s = new_session()
        bad = bytearray(run(1, 1, 1, 1))
        bad[-1] ^= 0xFF
        s.feed(hello() + bytes(bad) + run(2, 1, 1, 1))
        self.assertEqual(s.counts["run"], 1)
        self.assertEqual(s.bad, 1)

    def test_a_short_frame_is_reported_not_fatal(self):
        s = new_session()
        s.feed(hello())
        s.feed(frame(7, b"\x00\x01"))
        s.feed(run(5, 1, 1, 1))
        self.assertEqual(s.counts["run"], 1)


class TestLoss(unittest.TestCase):
    def test_device_gap_frames_are_counted_and_keep_the_log_sequence(self):
        s = new_session()
        s.feed(hello())
        s.feed(log(0, 0, entry(1, 9), entry(1, 9)))
        s.feed(gap(2, 5, 70))                         # the device dropped 5 entries
        s.feed(log(0, 7, entry(1, 9)))                # seq continues after the 5 lost
        self.assertEqual(s.counts["gap_log"], 5)
        self.assertEqual(s.counts["wire_log"], 0)
        s.feed(gap(3, 3, 36))
        self.assertEqual(s.counts["gap_run"], 3)

    def test_a_seq_jump_without_a_gap_frame_is_a_wire_loss(self):
        out = []
        s = new_session(echo=lambda k, line: out.append(line))
        s.feed(hello())
        s.feed(log(0, 0, entry(1, 9)))
        s.feed(log(0, 4, entry(1, 9)))                # entries 1..3 never arrived
        self.assertEqual(s.counts["wire_log"], 3)
        self.assertTrue(any("3 log entries missing" in x for x in out))
        ok, lines = s.summary()
        self.assertFalse(ok)

    def test_stats_line_parse(self):
        st = ks.parse_stats_line(b"junk\r\nstream stats frames=10 trace=5/1 log=4/2 run=3/0 power=7 folded=6 same=5 stalls=0\r\n> ")
        self.assertEqual((st["trace"], st["trace_lost"], st["log"], st["log_lost"], st["run"], st["power"], st["power_folded"]),
                         (5, 1, 4, 2, 3, 7, 6))
        self.assertIsNone(ks.parse_stats_line(b"no stats here"))

    def _session_with(self, n_run):
        s = new_session()
        s.feed(hello())
        for i in range(n_run):
            s.feed(run(100 + i, 7, 1))
        return s

    def test_summary_complete_when_counters_match(self):
        s = self._session_with(3)
        s.feed(b"stream stats frames=4 trace=0/0 log=0/0 run=3/0 power=0 folded=0 same=0 stalls=0\r\n")
        ok, lines = s.summary()
        self.assertTrue(ok, lines)

    def test_summary_flags_frames_lost_on_the_wire(self):
        s = self._session_with(2)
        s.feed(b"stream stats frames=4 trace=0/0 log=0/0 run=3/0 power=0 folded=0 same=0 stalls=0\r\n")
        ok, lines = s.summary()
        self.assertFalse(ok)
        self.assertTrue(any("1 missing" in x for x in lines), lines)

    def test_summary_without_stats_uses_the_last_heartbeat(self):
        s = self._session_with(2)
        s.feed(beat(5000, ctr=(0, 0, 0, 0, 2, 0, 0, 0, 0)))
        ok, lines = s.summary()
        self.assertTrue(ok, lines)
        self.assertIn("heartbeat", lines[0])

    def test_summary_without_any_counters_is_not_complete(self):
        s = self._session_with(1)
        ok, lines = s.summary()
        self.assertFalse(ok)

    def test_device_reported_loss_is_listed(self):
        s = self._session_with(1)
        s.feed(gap(3, 4, 48))
        s.feed(b"stream stats frames=3 trace=0/0 log=0/0 run=1/4 power=0 folded=0 same=0 stalls=0\r\n")
        ok, lines = s.summary()
        self.assertTrue(ok, lines)
        self.assertTrue(any("device reported 4 lost" in x for x in lines), lines)


class TestSessionFolder(unittest.TestCase):
    def _record(self, tmp):
        s = new_session(tmp)
        s.feed(hello(ms=1000))
        s.feed(text(1000, "[BOOT] ok\n"))
        s.feed(log(1000, 0, entry(10, 9)))
        s.feed(run(1020, 1, 1, 2))
        s.feed(power(1005, vbus=5100))
        s.feed(frame(2, struct.pack("<I", 0) + trace_rec(1, 1_030_000)))
        s.feed(gap(3, 2, 24))
        s.feed(run(1500, 12, 4, 2))
        s.feed(b"stream stats frames=7 trace=1/0 log=1/0 run=2/2 power=1 folded=0 same=0 stalls=0\r\n")
        return s.close()

    def test_files_meta_and_timeline(self):
        with tempfile.TemporaryDirectory() as t:
            ok, lines = self._record(t)
            self.assertTrue(ok, lines)
            names = {p.name for p in Path(t).iterdir()}
            self.assertTrue({"log.jsonl", "run.jsonl", "power.jsonl", "trace.jsonl", "boot.jsonl", "gaps.jsonl", "meta.json",
                             "timeline.txt", "trace.raw"} <= names, names)
            meta = json.loads((Path(t) / "meta.json").read_text())
            self.assertEqual(meta["hello"]["firmware"], "e32bcda")
            self.assertTrue(meta["complete"])
            tl = (Path(t) / "timeline.txt").read_text().splitlines()
            self.assertIn("e32bcda", tl[0])
            times = [float(x.split(" ms")[0]) for x in tl if " ms " in x[:20]]
            self.assertEqual(times, sorted(times))
            joined = "\n".join(tl)
            for needle in (" START ", " BOOT ", " LOG ", " RUN ", " POWER ", " GAP ", "ping"):
                self.assertIn(needle, joined)

    def test_every_row_has_a_wall_time_and_absolute_ms(self):
        with tempfile.TemporaryDirectory() as t:
            self._record(t)
            for stem in ("log", "run", "power", "trace"):
                for r in rows(t, stem):
                    self.assertIn("wall", r)
                    self.assertIn("t_ms", r)

    def test_view_filters(self):
        with tempfile.TemporaryDirectory() as t:
            self._record(t)
            self.assertEqual(ks.timeline_text(t, kinds={"run"}).count(" RUN "), 2)
            self.assertNotIn(" LOG ", ks.timeline_text(t, kinds={"run"}))
            self.assertIn("run end", ks.timeline_text(t, grep="run end"))
            self.assertNotIn("run start", ks.timeline_text(t, grep="run end"))
            late = ks.timeline_text(t, t0=1100)
            self.assertIn("run end", late)
            self.assertNotIn("run start", late)
            self.assertIn("GAP", ks.timeline_text(t, kinds={"gap"}))
            self.assertIn(" START ", ks.timeline_text(t, kinds={"start"}))

    def test_view_cli_summary_and_missing_dir(self):
        with tempfile.TemporaryDirectory() as t:
            self._record(t)
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(ks.main(["view", t, "--summary"]), 0)
            self.assertIn("run", out.getvalue())
            out = io.StringIO()
            with redirect_stdout(out):
                self.assertEqual(ks.main(["view", t, "--kind", "run", "--wall"]), 0)
            self.assertIn(" RUN ", out.getvalue())
            self.assertEqual(ks.main(["view", t + "/nope"]), 2)

    def test_unwrap32(self):
        u = ks.Unwrap32()
        self.assertEqual(u(0xFFFFFFF0), 0xFFFFFFF0)
        self.assertEqual(u(5), (1 << 32) + 5)


class TestRecordOverAPty(unittest.TestCase):
    """`record` against a pseudo terminal that answers like the K2's shell."""

    def test_record_until_run_end_sends_stream_off_and_reads_the_stats(self):
        import os
        import pty
        import threading
        master, slave = pty.openpty()
        sent = []

        def k2():
            buf = b""
            on = False
            while True:
                try:
                    d = os.read(master, 256)
                except OSError:
                    return
                buf += d
                if not on and b"stream on" in buf:
                    on = True
                    sent.append(buf.split(b"stream on")[1].split(b"\r")[0].strip())
                    os.write(master, hello() + run(1500, 1, 1, 2) + run(1600, 12, 4, 2))
                if on and b"stream off" in buf[buf.index(b"stream on"):]:   # the pre-start "stream off" is ignored
                    os.write(master, b"stream stats frames=3 trace=0/0 log=0/0 run=2/0 power=0 folded=0 same=0 stalls=0\r\n")
                    return

        th = threading.Thread(target=k2, daemon=True)
        th.start()
        with tempfile.TemporaryDirectory() as t:
            out = io.StringIO()
            with redirect_stdout(out):
                code = ks.main(["record", "--out", t + "/s", "--port", os.ttyname(slave), "--kinds", "run,power=50", "--old",
                                "--until-run-end", "--settle", "0.2", "--seconds", "10"])
            th.join(3)
            self.assertEqual(code, 0, out.getvalue())
            self.assertEqual(sent, [b"run power=50 old"])
            self.assertIn("COMPLETE", out.getvalue())
            self.assertIn("run end", (Path(t) / "s" / "timeline.txt").read_text())
        os.close(slave)
        os.close(master)


class TestDefaultPort(unittest.TestCase):
    """The record port defaults to the logger's stable /dev/serial/by-id name (the K2's product string is
    "Open-K2 Logger"), not to /dev/ttyACM0, whose number depends on enumeration order."""

    def test_the_by_id_entry_wins(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "usb-WITRN_Open-K2_Logger_0001-if02"
            p.touch()
            self.assertEqual(ks.default_port(d), str(p))

    def test_falls_back_to_ttyacm0_without_a_by_id_entry(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(ks.default_port(d), ks.FALLBACK_PORT)
            self.assertEqual(ks.FALLBACK_PORT, "/dev/ttyACM0")

    def test_only_the_loggers_cdc_interfaces_match(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / "usb-WITRN_Open-K2_Logger_0001-if02").touch()
            (Path(d) / "usb-SomeOther_Device_0001-if00").touch()
            self.assertEqual(ks.default_port(d), str(Path(d) / "usb-WITRN_Open-K2_Logger_0001-if02"))


class TestEnumParse(unittest.TestCase):
    def test_parse_enum_handles_comments_and_values(self):
        src = "enum class Foo : uint8_t { A, B = 3, /* x */ C, // tail\n D };"
        self.assertEqual(ks.parse_enum(src, "Foo"), ["A", "1", "2", "B", "C", "D"])

    def test_parse_enum_picks_by_member(self):
        src = "enum class Stage { Start, Stop }; enum class Stage { Algorithms, Running };"
        self.assertEqual(ks.parse_enum(src, "Stage", needs="Algorithms"), ["Algorithms", "Running"])


if __name__ == "__main__":
    unittest.main()
