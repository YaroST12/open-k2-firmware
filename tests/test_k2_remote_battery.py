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
        want = {0: 1, 1: 3, 2: 1, 3: 1, 4: 1, 5: 6, 6: 1, 7: 5}[op]
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

    def test_read_and_list(self):
        rec = fixture_record()
        link = FakeLink(records={7: rec})
        self.assertEqual(run(link, "list"), {"battery_runs": [7]})
        r = run(link, "read", "7")["battery_record"]
        self.assertEqual(r, br.decode_record(rec))
        self.assertEqual(sum(1 for c in link.calls if c[0] == 7), -(-len(rec) // kr.BATTERY_CHUNK))   # stops at `total`
        text = kr.render("battery", {"battery_record": r})
        self.assertIn("efficiency 80.0 %", text)   # 7.998 Wh / 10.001 Wh
        self.assertEqual(run(link, "read", "--all")["battery_records"], [r])
        with self.assertRaises(SystemExit):
            run(link, "read", "9")

    def test_corrupt_record_is_an_error(self):
        rec = bytearray(fixture_record())
        rec[20] ^= 1
        with self.assertRaises(SystemExit):
            run(FakeLink(records={7: bytes(rec)}), "read", "7")


if __name__ == "__main__":
    unittest.main()
