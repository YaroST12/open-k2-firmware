# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote.py battery` (CMD_BATTERY 0x58): the status block, the start / config arguments on the wire,
a refusal, the chunked record read and its JSON decode via tools/battery_rec.py (the golden record of
tests/fixtures/qrshare.tsv, made by the firmware's encoder in tests/test_qrshare_records.cpp)."""
import json
import os
import struct
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import battery_rec as br  # noqa: E402
import k2_remote as kr  # noqa: E402


def fixture_record():
    with open(os.path.join(HERE, "fixtures", "qrshare.tsv")) as f:
        for ln in f:
            c = ln.rstrip("\n").split("\t")
            if c[0] == "battery":
                return bytes.fromhex(c[3])
    raise AssertionError("no battery row in tests/fixtures/qrshare.tsv")


def status_block(state=0, mode=0, wiring=0, phases=0, result=0, flags=4, rid=0, end_ma=50, end_s=60, run_s=0, phase_s=0,
                 uwh=(0, 0), uah=(0, 0), eff=0xFFFF, runs=0, next_wiring=0):
    return (bytes([state, mode, wiring, phases, result, flags]) + struct.pack("<3H6IH", rid, end_ma, end_s, run_s, phase_s,
            uwh[0], uwh[1], uah[0], uah[1], eff) + bytes([runs, next_wiring]))


class FakeLink:
    """Answers CMD_BATTERY like src/battery/battery_wire.cpp for the ops a test needs."""

    def __init__(self, status=None, records=None, refuse=None):
        self.status = status or status_block()
        self.records = records or {}
        self.refuse = refuse
        self.calls = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_BATTERY
        self.calls.append(bytes(payload))
        op = payload[0]
        want = {0: 1, 1: 3, 2: 1, 3: 1, 4: 1, 5: 6, 6: 1, 7: 5, 8: 3}[op]
        assert len(payload) == want, (op, payload)
        if op == 6:
            ids = sorted(self.records, reverse=True)
            out = bytes([len(ids)]) + b"".join(i.to_bytes(2, "little") for i in ids)
        elif op == 7:
            rid, off = struct.unpack_from("<HH", payload, 1)
            rec = self.records.get(rid, b"")
            chunk = rec[off:off + kr.BATTERY_CHUNK]
            out = struct.pack("<HHB", off, len(rec), len(chunk)) + chunk
        else:
            out = bytearray(self.status)
            if self.refuse is not None and op != 0:
                out[4] = self.refuse
            out = bytes(out)
        return type("R", (), {"payload": out})()


def run(link, *argv):
    a = kr.build_parser().parse_args(["battery", *argv])
    shown = []
    r = kr.run_command(link, a, show=shown.append)
    json.dumps(r)   # JSON-ready
    return r


class Battery(unittest.TestCase):
    def test_command_id_matches_firmware(self):
        self.assertEqual(kr.CMD_BATTERY, 0x58)
        self.assertEqual(kr.BATTERY_CHUNK, 48)
        self.assertEqual(br.STATUS_BYTES, 40)

    def test_status(self):
        link = FakeLink(status_block(state=1, mode=2, wiring=1, phases=2, rid=12, run_s=4000, phase_s=400,
                                     uwh=(10_000_000, 7_950_000), uah=(2_000_000, 1_590_000), eff=795, runs=3, next_wiring=1))
        s = run(link, "status")["battery"]
        self.assertEqual((s["state"], s["mode"], s["wiring"], s["phases"], s["id"]), ("measuring", "round-trip", "moved", 2, 12))
        self.assertEqual((s["in_uwh"], s["out_uwh"], s["in_uah"], s["out_uah"]), (10_000_000, 7_950_000, 2_000_000, 1_590_000))
        self.assertEqual((s["efficiency_permille"], s["efficiency_final"], s["runs_stored"]), (795, False, 3))
        text = kr.render("battery", {"battery": s})
        self.assertIn("79.5 % so far", text)
        self.assertIn("10.000 Wh", text)

    def test_empty_run_flag(self):
        s = run(FakeLink(status_block(state=3, rid=5, flags=4 | 16)), "end")["battery"]
        self.assertTrue(s["empty"])
        self.assertIn("empty run, not saved", kr.render("battery", {"battery": s}))
        s = run(FakeLink(status_block(state=3, rid=5)), "status")["battery"]
        self.assertFalse(s["empty"])

    def test_start_sends_mode_and_wiring(self):
        link = FakeLink(status_block(next_wiring=1))
        run(link, "start", "round-trip")                      # the K2's wiring setting
        self.assertEqual(link.calls[-1], bytes([1, 2, 1]))
        run(link, "start", "discharge", "--wiring", "same-port")
        self.assertEqual(link.calls[-1], bytes([1, 1, 0]))

    def test_config_keeps_omitted_values(self):
        link = FakeLink(status_block(end_ma=50, end_s=60, next_wiring=0))
        run(link, "config", "--end-ma", "100")
        self.assertEqual(link.calls[-1], bytes([5]) + struct.pack("<HHB", 100, 60, 0))

    def test_refusal_exits(self):
        link = FakeLink(refuse=1)
        with self.assertRaises(SystemExit) as e:
            run(link, "start", "charge")
        self.assertIn("busy", str(e.exception))

    def test_continue_sends_id(self):
        link = FakeLink(status_block(state=1, rid=12))
        s = run(link, "continue")["battery"]
        self.assertEqual(link.calls[-1], bytes([8, 0, 0]))     # 0 = the newest run
        self.assertEqual((s["state"], s["id"]), ("measuring", 12))
        run(link, "continue", "12")
        self.assertEqual(link.calls[-1], bytes([8, 12, 0]))
        for code, word in ((5, "no-run"), (6, "not-newest")):
            with self.assertRaises(SystemExit) as e:
                run(FakeLink(refuse=code), "continue", "3")
            self.assertIn(word, str(e.exception))

    def test_read_and_list(self):
        rec = fixture_record()
        link = FakeLink(records={7: rec})
        self.assertEqual(run(link, "list"), {"battery_runs": [7], "battery_pair": None})
        r = run(link, "read", "7")["battery_record"]
        self.assertEqual(r, br.decode_record(rec))
        link.calls.clear()
        run(link, "read", "7")
        self.assertEqual(sum(1 for c in link.calls if c[0] == 7), -(-len(rec) // kr.BATTERY_CHUNK))   # stops at `total`
        text = kr.render("battery", {"battery_record": r})
        self.assertIn("efficiency 80.0 %", text)   # 7.998 Wh / 10.001 Wh
        self.assertEqual(run(link, "read", "--all"), {"battery_records": [r], "battery_pair": None})
        with self.assertRaises(SystemExit):
            run(link, "read", "9")

    @staticmethod
    def as_version(rec, ver, resumes=0, gap_s=0):
        """The fixture's run as record version 1 (246 B) or 2 (250 B, the known gap after the curve)."""
        b = bytearray(rec[:br.HEAD_BYTES + 3 * br.POINTS])
        b[2] = ver
        b[5] = (b[5] & 3) | resumes << 2
        if ver == 2:
            b += struct.pack("<I", gap_s)
        return bytes(b) + struct.pack("<H", br.crc16(b))

    def test_resumes_and_gap(self):
        r = br.decode_record(self.as_version(fixture_record(), 2, resumes=2, gap_s=75))
        self.assertEqual((r["version"], r["resumes"], r["gap_s"]), (2, 2, 75))
        text = br.render_record(r)
        self.assertIn("resumed 2x", text)
        self.assertIn("at least 75 s not measured", text)
        self.assertIn("off time", text)
        old = br.decode_record(self.as_version(fixture_record(), 1, resumes=1))
        self.assertEqual((old["version"], old["resumes"], old["gap_s"]), (1, 1, 0))
        self.assertEqual(old["phases"], r["phases"])
        with self.assertRaises(br.BadRecord):
            br.decode_record(self.as_version(fixture_record(), 1)[:-1])

    def test_corrupt_record_is_an_error(self):
        rec = bytearray(fixture_record())
        rec[20] ^= 1
        with self.assertRaises(SystemExit):
            run(FakeLink(records={7: bytes(rec)}), "read", "7")


def make_record(rid, mode, kind, end, secs, uwh_in, uwh_out, uah_in, uah_out, ver=2):
    """A one-phase record as the firmware encodes it (battery::encode); ver 1 = the 246-byte format."""
    b = bytearray(br.HEAD_BYTES + 3 * br.POINTS)
    b[0:3] = b"BR" + bytes([ver])
    b[3] = br.MODES.index(mode)
    b[4] = 2                                                     # done
    b[5] = 1
    struct.pack_into("<4H", b, 6, rid, 50, 60, 10)
    b[15] = 0xFF
    struct.pack_into("<BBHIIIII", b, 16, br.KINDS.index(kind), br.ENDS.index(end), 0, secs, uwh_in, uwh_out, uah_in, uah_out)
    if ver == 2:
        b += struct.pack("<I", 0)
    return bytes(b) + struct.pack("<H", br.crc16(b))


# The runs of 2026-11-24/25 (runs/battery): ids 3..5 are version 1, the discharge from before the in/out fix with its energy
# in the "in" column; the same vectors as tests/test_battery_synth.cpp test_real_runs_pair.
REAL = {
    3: make_record(3, "discharge", "discharge", "auto", 3170, 30994272, 0, 1538604, 0, ver=1),
    4: make_record(4, "charge", "charge", "auto", 3006, 23804464, 0, 1205674, 0, ver=1),
    5: make_record(5, "charge", "charge", "auto", 1592, 13075722, 0, 673138, 0, ver=1),
    6: make_record(6, "charge", "charge", "auto", 3011, 19404226, 0, 980452, 0),
}


class TestBatteryPair(unittest.TestCase):
    def test_v1_discharge_reads_out(self):
        r = br.decode_record(REAL[3])
        self.assertEqual((r["totals"]["out_uwh"], r["totals"]["in_uwh"], r["totals"]["out_uah"]), (30994272, 0, 1538604))
        v2 = br.decode_record(make_record(3, "discharge", "discharge", "auto", 10, 5, 0, 5, 0))
        self.assertEqual(v2["totals"]["in_uwh"], 5)              # version 2 is taken as written

    def test_real_runs(self):
        recs = [br.decode_record(REAL[i]) for i in (5, 4, 3)]
        self.assertEqual(br.pair(recs), {"from_id": 3, "to_id": 5, "out_uwh": 30994272, "in_uwh": 36880186,
                                         "efficiency_permille": 840})
        self.assertEqual(br.pair(recs[1:])["efficiency_permille"], 1302)   # half the charge: id 4 alone
        self.assertIsNone(br.pair([br.decode_record(REAL[i]) for i in (6, 5, 4)]))   # the discharge pruned

    def test_rules(self):
        def rec(rid, mode, uwh, end="auto"):
            kind = "discharge" if mode == "discharge" else "charge"
            return br.decode_record(make_record(rid, mode, kind, end, 60, 0 if kind == "discharge" else uwh,
                                                uwh if kind == "discharge" else 0, 0, 0))
        self.assertEqual(br.pair([rec(9, "charge", 30000000), rec(8, "discharge", 24000000)])["efficiency_permille"], 800)
        p = br.pair([rec(9, "discharge", 8000000), rec(8, "discharge", 0), rec(7, "charge", 10000000), rec(6, "discharge", 9)])
        self.assertEqual((p["efficiency_permille"], p["from_id"], p["to_id"]), (800, 7, 9))
        self.assertIsNone(br.pair([rec(3, "discharge", 10**6), rec(2, "round-trip", 10**6), rec(1, "charge", 10**6)]))
        self.assertIsNone(br.pair([rec(3, "discharge", 10**6), rec(2, "charge", 10**6, end="lost")]))

    def test_list_shows_pair(self):
        link = FakeLink(records={i: REAL[i] for i in (3, 4, 5)})
        out = run(link, "list")
        self.assertEqual(out["battery_runs"], [5, 4, 3])
        self.assertEqual(out["battery_pair"]["efficiency_permille"], 840)
        self.assertIn("efficiency of runs 3..5: 84.0 %", kr.render("battery", out))
        self.assertEqual(run(link, "read", "--all")["battery_pair"]["efficiency_permille"], 840)


if __name__ == "__main__":
    unittest.main()
