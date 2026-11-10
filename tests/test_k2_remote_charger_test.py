# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Exact suite wire/output vectors shared with test_remote_proto.cpp; no device."""
import json
import struct
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import k2_remote as kr

HEADER = bytes([1, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0])
HEADER2 = bytes([2, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0,
                 0xFF, 0xFF, 7, 0, 0x10, 0, 4, 0, 0x12, 0x32, 0xFF,
                 0xA0, 0x8C, 0x50, 0xC3, 0x88, 0x13, 0x17, 0x44, 0x33, 0x22, 0x11, 0x81, 0x71, 0x97])
LEGACY = {"selection": None, "stage": None, "evidence": None, "resumed": 0}
ROW0 = bytes([2, 0, 1, 0x20, 0x4E, 0, 0, 0x58, 0x4D, 2, 0, 0, 0, 0, 0, 0])
ROW1 = bytes([0, 6, 1, 0x28, 0x23, 0xE8, 3, 0, 0, 1, 0, 0, 0x40, 0x32, 0xDC, 0xC0])
ROW2 = bytes([4, 0, 0, 0xE0, 0x2E, 0, 0, 0, 0, 3, 11, 7, 0, 0, 0, 0])
EXPECTED_ROWS = [
    {"index": 0, "group": "qc3", "protocol": "qc3", "pos": 0, "target_mv": 20000, "request_ma": 0,
     "measured_mv": 19800, "result": "pass", "reason": "none", "detail": 0, "detail_reason": "none",
     "raw_pdo": 0, "continuous": True, "avs": False},
    {"index": 1, "group": "pd", "protocol": "pd-pps", "pos": 6, "target_mv": 9000, "request_ma": 1000,
     "measured_mv": 0, "result": "running", "reason": "none", "detail": 0, "detail_reason": "none",
     "raw_pdo": 0xC0DC3240, "continuous": True, "avs": False},
    {"index": 2, "group": "afc", "protocol": "afc", "pos": 0, "target_mv": 12000, "request_ma": 0,
     "measured_mv": 0, "result": "fail", "reason": "request failed", "detail": 7, "detail_reason": "no response",
     "raw_pdo": 0, "continuous": False, "avs": False},
]


class Link:
    def __init__(self, payloads):
        self.payloads = list(payloads)
        self.sent = []

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, payload))
        assert cmd == 0x0B, "only read-only command allowed"
        p = self.payloads.pop(0)
        if isinstance(p, Exception):
            raise p
        return kr.Response(cmd, len(self.sent), 0, p)


def page(meta, start, rows):
    return meta[:13] + bytes([start, len(rows)]) + meta[15:] + b"".join(rows)


class ChargerTest(unittest.TestCase):
    def args(self, *args):
        return kr.build_parser().parse_args(["charger-test", *args])

    def test_exact_request(self):
        frame = kr.pack_request(kr.CMD_CHARGER_TEST, 9, bytes([0, 4, 3, 2, 1, 8, 7, 6, 5]))
        self.assertEqual(frame, bytes([0xA5, 0x4B, 0x0B, 9, 9, 0, 4, 3, 2, 1, 8, 7, 6, 5]) + bytes(50))

    def test_exact_status_and_wire(self):
        link = Link([HEADER])
        expected = {"schema": 1, "run_id": 0x01020304, "revision": 0x05060708, "state": "holding",
                    "reason": "none", "total": 3, "current": 1, "steps": [], **LEGACY}
        self.assertEqual(kr.cmd_charger_test(link, self.args("status")), expected)
        self.assertEqual(link.sent, [(0x0B, b"\x02")])
        wire = bytes([0xA6, 0x4B, 0x0B, 9, 0, 47]) + page(HEADER, 0, [ROW0, ROW1]) + bytes(11)
        self.assertEqual(len(wire), 64)
        decoded = kr.decode_charger_test(kr.parse_response(wire).payload)
        self.assertEqual(decoded, dict(expected, start=0, count=2, steps=EXPECTED_ROWS[:2]))

    def test_exact_results_and_export_in_all_states(self):
        for state in range(8):
            with self.subTest(state=state):
                meta = bytearray(HEADER)
                meta[9] = state
                meta[10] = 17 if state == 7 else 16 if state == 6 else 0
                meta = bytes(meta)
                link = Link([meta, page(meta, 0, [ROW0, ROW1]), page(meta, 2, [ROW2]), meta])
                with patch.object(Path, "write_text") as write:
                    result = kr.cmd_charger_test(link, self.args("results", "--out", "host-build/evidence.json"))
                    expected = {"schema": 1, "run_id": 0x01020304, "revision": 0x05060708,
                                "state": kr.TEST_STATES[state], "reason": kr.TEST_REASONS[meta[10]],
                                "total": 3, "current": 1, **LEGACY, "steps": EXPECTED_ROWS}
                    self.assertEqual(result, expected)
                    write.assert_called_once_with(json.dumps(expected, indent=2) + "\n")
                self.assertEqual(link.sent, [(0x0B, b"\x02"), (0x0B, bytes([0, 4, 3, 2, 1, 8, 7, 6, 5])),
                                            (0x0B, bytes([2, 4, 3, 2, 1, 8, 7, 6, 5])), (0x0B, b"")])
        self.assertEqual(kr.render("charger-test", expected),
                         "run 16909060 rev 84281096: unsafe (5V release failed); step 1/3\n"
                         "  PASS = negotiation / voltage hold, not power or revision conformance\n"
                         "  legacy schema: selection / source evidence unavailable; some entries omitted\n"
                         "  0: qc3 20000 mV -> 19800 mV pass (none; none)\n"
                         "  1: pd-pps 9000 mV -> 0 mV running (none; none)\n"
                         "  2: afc 12000 mV -> 0 mV fail (request failed; no response)")

    def test_empty_and_maximum_bounded_results(self):
        empty = bytes([1]) + bytes(14)
        link = Link([empty, empty])
        self.assertEqual(kr.cmd_charger_test(link, self.args("results"))["steps"], [])
        self.assertEqual(link.sent, [(0x0B, b"\x02"), (0x0B, b"")])
        meta = HEADER[:11] + bytes([95, 95, 0, 0])
        pages = [page(meta, n, [ROW0] * min(2, 95 - n)) for n in range(0, 95, 2)]
        link = Link([meta, *pages, meta])
        result = kr.cmd_charger_test(link, self.args("results"))
        self.assertEqual([r["index"] for r in result["steps"]], list(range(95)))
        self.assertEqual(len(link.sent), 50)

    def test_no_partial_export_on_mutation_or_errors(self):
        changed = bytearray(HEADER); changed[5] += 1
        for bad in (bytes(changed), page(HEADER, 0, []), page(HEADER, 1, [ROW0]),
                    kr.RemoteError(kr.STATUS_BUSY, kr.CMD_CHARGER_TEST),
                    kr.RemoteError(kr.STATUS_UNKNOWN_CMD, kr.CMD_CHARGER_TEST), kr.RemoteTimeout("timeout")):
            with self.subTest(bad=bad), patch.object(Path, "write_text") as write:
                link = Link([HEADER, bad])
                with self.assertRaises((kr.FrameError, kr.RemoteError, kr.RemoteTimeout)):
                    kr.cmd_charger_test(link, self.args("results", "--out", "host-build/evidence.json"))
                write.assert_not_called()
                self.assertEqual(len(link.sent), 2)
        link = Link([HEADER, page(HEADER, 0, [ROW0, ROW1]), page(HEADER, 2, [ROW2]), bytes(changed)])
        with patch.object(Path, "write_text") as write, self.assertRaises(kr.FrameError):
            kr.cmd_charger_test(link, self.args("results", "--out", "host-build/evidence.json"))
        write.assert_not_called()
        self.assertIn("retry charger-test results", str(kr.RemoteError(4, 0x0B)))

    def test_bad_wire_shape_and_enums(self):
        good = page(HEADER, 0, [ROW0, ROW1])
        for p in (b"", HEADER[:14], HEADER + b"\0", good[:-1], good + b"\0"):
            with self.assertRaises(kr.FrameError):
                kr.decode_charger_test(p)
        for offset, value in ((0, 2), (9, 10), (10, 20), (11, 96), (12, 4), (13, 4), (14, 3),
                              (15, 10), (17, 0x80), (24, 5), (25, 20)):
            bad = bytearray(good); bad[offset] = value
            with self.subTest(offset=offset), self.assertRaises(kr.FrameError):
                kr.decode_charger_test(bad)
        with self.assertRaises(kr.FrameError):
            kr.cmd_charger_test(Link([good]), self.args("status"))

    def test_protocol_and_driver_detail(self):
        for pdo, flags, expected in ((0x00019064, 0, "pd-fixed"), (0xD0000000, 2, "pd-avs"),
                                      (0x80000000, 0, "pd-other")):
            row = struct.pack("<BBBHHHBBBI", 0, 1, flags, 5000, 1000, 0, 3, 11, 128 + 6, pdo)
            step = kr.decode_charger_test(page(HEADER, 0, [row]))["steps"][0]
            self.assertEqual(step["protocol"], expected)
            self.assertEqual(step["detail_reason"], kr.PD_ERRS[6])

    def test_no_mutating_cli_operations(self):
        for command in ("start", "stop", "cancel", "clear", "reset"):
            with patch("sys.stderr"), self.assertRaises(SystemExit):
                self.args(command)
        for args in (("status", "--out", "file.json"), ("results", "--confirm"), ("results", "--allow-inline")):
            with patch("sys.stderr"), self.assertRaises(SystemExit):
                self.args(*args)

    def test_schema2_exact_origin_selection_and_pages(self):
        modern_rows = [bytes([6]) + ROW0[1:], bytes([4]) + ROW1[1:], bytes([8]) + ROW2[1:]]
        link = Link([HEADER2, *[page(HEADER2, i, [r]) for i, r in enumerate(modern_rows)], HEADER2])
        with patch.object(Path, "write_text") as write:
            r = kr.cmd_charger_test(link, self.args("results", "--out", "host-build/evidence.json"))
        write.assert_called_once_with(json.dumps(r, indent=2) + "\n")
        self.assertEqual(r["selection"]["run_mask"], 0x7ffff)
        self.assertEqual(len(r["selection"]["run"]), 19)
        self.assertEqual(r["selection"]["configured"], ["pps", "pd3.2-revision"])
        self.assertEqual(r["stage"], "review")
        self.assertNotIn("cursor", r)   # a UI detail: status only, never in the evidence export
        self.assertEqual(kr.decode_charger_test(HEADER2)["cursor"], 9)
        self.assertEqual(r["evidence"], {
            "origin": "run-start", "source_session": 0x11223344, "raw_header": 0x7181, "pdo_count": 7,
            "wire_revision_code": 2, "wire_revision": "PD3.x",
            "source_rmdo": {"revision": "3.2", "version": "1.7", "provenance": "source self-report"},
            "advertised": {"pps": True, "epr_table": True, "spr_avs": True, "epr_capable": True, "partial_epr": True},
            "monitor_mv": 36000, "physical_cable": {"observed": True, "max_mv": 50000, "max_ma": 5000},
            "spoof_on": True, "independent_revision_tested": False,
            "pass_scope": "negotiation and measured voltage only; not rated-current/power or revision conformance"})
        self.assertEqual([s["algorithm"] for s in r["steps"]], ["qc3", "pps", "afc"])
        self.assertEqual([s["group"] for s in r["steps"]], ["qc3", "pd", "afc"])
        self.assertEqual([s["protocol"] for s in r["steps"]], ["qc3", "pps", "afc"])
        self.assertEqual(link.sent, [(0x0B, b"\x02"),
            *[(0x0B, bytes([2, i, 4, 3, 2, 1, 8, 7, 6, 5])) for i in range(3)], (0x0B, b"\x02")])
        self.assertIn("RMDO 3.2 self-report", kr.render("charger-test", r))
        self.assertIn("not power or revision conformance", kr.render("charger-test", r))

    def test_schema2_skip_variants_never_become_pass(self):
        meta = bytearray(HEADER2); meta[11] = meta[12] = 5; meta[9] = 5
        selected = [16, 17, 18, 14, 15]
        rows = [struct.pack("<BBBHHHBBBI", a, 0, 0, 0, 0, 0, 4, 20 if a >= 16 else 4, 0, 0) for a in selected]
        r = kr.cmd_charger_test(Link([meta, *[page(meta, i, [row]) for i, row in enumerate(rows)], meta]), self.args("results"))
        self.assertEqual([s["result"] for s in r["steps"]], ["skip"] * 5)
        self.assertEqual([s["explanation"] for s in r["steps"]], [
            "No independent PD3.0 test", "No independent PD3.1 test", "No independent PD3.2 test",
            "not implemented", "not implemented"])
        for a in selected[:3]:  # UFCS/VIVO (14, 15) now run and may pass
            row = struct.pack("<BBBHHHBBBI", a, 0, 0, 0, 0, 0, 2, 0, 0, 0)
            with self.assertRaises(kr.FrameError):
                kr.decode_charger_test(page(meta, 0, [row]))
        bad = bytes([0]) + ROW0[1:]
        with self.assertRaises(kr.FrameError):
            kr.decode_charger_test(page(HEADER2, 0, [bad]))

    def test_unrun_pd_rows_do_not_trip_the_wire_revision_check(self):
        # A stopped run (power lost on a USB-A port) still lists its PD rows as pending; the wire says PD2 (code 1) while the
        # pd3x-fixed row never ran. Reading the results must work; a row that ran and contradicts the wire still fails.
        def meta_with(wire, state=6):   # state 6 = stopped
            m = bytearray(HEADER2); m[23] = (m[23] & ~3) | wire; m[9] = state
            return m
        def row(code, result, why=0):
            return struct.pack("<BBBHHHBBBI", code, 0, 0, 5000, 0, 0, result, why, 0, 0)
        for wire, code, result, why in ((1, 1, 0, 0), (1, 1, 1, 0), (1, 1, 4, 2), (2, 0, 0, 0), (2, 0, 1, 0)):
            with self.subTest(wire=wire, code=code, result=result):
                d = kr.decode_charger_test(page(meta_with(wire), 0, [row(code, result, why)]))
                self.assertEqual(d["steps"][0]["algorithm"], kr.TEST_ALGORITHMS[code])
        # The whole read over the wire: two pending PD rows, the wire says PD2.
        m = meta_with(1); m[11] = 2
        r = kr.cmd_charger_test(Link([bytes(m), page(m, 0, [row(1, 0)]), page(m, 1, [row(0, 0)]), bytes(m)]), self.args("results"))
        self.assertEqual([s["result"] for s in r["steps"]], ["pending", "pending"])
        for wire, code in ((2, 0), (1, 1)):   # a row that ran and contradicts the wire (pd2-fixed on a PD3 wire, pd3x on a PD2 one) still fails
            for result in (2, 3):
                with self.subTest(wire=wire, code=code, ran=result), self.assertRaises(kr.FrameError):
                    kr.decode_charger_test(page(meta_with(wire), 0, [row(code, result)]))

    def test_reason_42_withdrawn_by_source_decodes(self):
        # Hardware-run item 8 (2026-11-09, runs/2026-11-09-splitter): the re-plan row's new Reason::Withdrawn (#42)
        # must decode, not raise FrameError("invalid charger-test row") - ct_start.py crashed at the summary with it.
        self.assertEqual(len(kr.TEST_REASONS), 47)   # 43 reasons + WrongVoltage (#43) + NeedsLoad (#44) + Floored (#45) + StepResolution (#46)
        self.assertEqual(kr.TEST_REASONS[42], "withdrawn by source")
        row = struct.pack("<BBBHHHBBBI", 2, 4, 0, 20000, 0, 0, 4, 42, 0, 0)   # a pd3-fixed 20 V row, skip, why 42
        step = kr.decode_charger_test(page(HEADER2, 0, [row]))["steps"][0]
        self.assertEqual((step["result"], step["reason"]), ("skip", "withdrawn by source"))
        self.assertEqual(step["explanation"], "withdrawn by source")
        bad = bytearray(row); bad[10] = 47   # the why byte, one past the last reason, still fails the row check
        with self.assertRaises(kr.FrameError):
            kr.decode_charger_test(page(HEADER2, 0, [bytes(bad)]))

    def test_reason_43_wrong_voltage_on_5v_contract_decodes(self):
        # ROADMAP 2026-11-09 (runs/2026-11-09-splitter/streamB2): a 5 V contract (PS_RDY at PDO 1) held at the
        # wrong voltage (pattern shared-bus-split) fails the unit as Reason::WrongVoltage (#43), not "no PD
        # after re-attach" - the row must decode with the measured (highest) VBUS, not raise FrameError.
        self.assertEqual(kr.TEST_REASONS[43], "wrong voltage on 5V contract")
        row = struct.pack("<BBBHHHBBBI", 2, 1, 0, 5000, 0, 7600, 3, 43, 0, 0)   # a PD-row 5 V, fail, why 43, measured 7.6 V
        step = kr.decode_charger_test(page(HEADER2, 0, [row]))["steps"][0]
        self.assertEqual((step["result"], step["reason"]), ("fail", "wrong voltage on 5V contract"))
        self.assertEqual(step["explanation"], "wrong voltage on 5V contract")
        self.assertEqual(step["measured_mv"], 7600)
        bad = bytearray(row); bad[10] = 47   # the why byte, one past the last reason, still fails the row check
        with self.assertRaises(kr.FrameError):
            kr.decode_charger_test(page(HEADER2, 0, [bytes(bad)]))

    def test_reason_44_needs_load_decodes(self):
        # pattern pps-off-at-no-load (splitter run 2026-11-10): a PPS level the source accepted and then dropped at ~0 mA
        # is Reason::NeedsLoad (#44), a Skip - the row must decode with the measured collapse voltage, not raise FrameError.
        self.assertEqual(kr.TEST_REASONS[44], "needs load")
        row = struct.pack("<BBBHHHBBBI", 4, 6, 0, 11000, 3200, 1800, 4, 44, 0, 0)   # a PPS 11 V row, skip, why 44, measured 1.8 V
        step = kr.decode_charger_test(page(HEADER2, 0, [row]))["steps"][0]
        self.assertEqual((step["result"], step["reason"]), ("skip", "needs load"))
        self.assertEqual(step["explanation"], "needs load")
        self.assertEqual(step["measured_mv"], 1800)
        bad = bytearray(row); bad[10] = 47   # one past the last reason
        with self.assertRaises(kr.FrameError):
            kr.decode_charger_test(page(HEADER2, 0, [bytes(bad)]))

    def test_reasons_45_46_decode(self):
        # ROADMAP "PPS coverage": 45 = a low edge the source cannot reach (fail, measured = the floor),
        # 46 = a PPS step-walk whose +20 mV step was not followed (fail, detail = the measured move in mV)
        self.assertEqual(kr.TEST_REASONS[45], "floored above target")
        self.assertEqual(kr.TEST_REASONS[46], "step resolution")
        row = struct.pack("<BBBHHHBBBI", 4, 6, 0, 3300, 1000, 5000, 3, 45, 0, 0)   # a PPS 3.3 V row, fail, why 45, measured 5 V
        step = kr.decode_charger_test(page(HEADER2, 0, [row]))["steps"][0]
        self.assertEqual((step["result"], step["reason"]), ("fail", "floored above target"))
        self.assertEqual(step["measured_mv"], 5000)
        row = struct.pack("<BBBHHHBBBI", 4, 6, 0, 8000, 1000, 8100, 3, 46, 100, 0)   # the step walk, fail, why 46, detail 100 mV
        step = kr.decode_charger_test(page(HEADER2, 0, [row]))["steps"][0]
        self.assertEqual((step["result"], step["reason"], step["detail"]), ("fail", "step resolution", 100))
        self.assertEqual(step["explanation"], "step resolution 100mV")
        bad = bytearray(row); bad[10] = 47   # one past the last reason
        with self.assertRaises(kr.FrameError):
            kr.decode_charger_test(page(HEADER2, 0, [bytes(bad)]))

    def test_schema2_epr_avs_is_a_programmable_test(self):
        meta = bytearray(HEADER2)
        struct.pack_into("<I", meta, 15, 1 << 19)
        pdo = 0xD0000000 | (280 << 17) | (150 << 8) | 100
        row = struct.pack("<BBBHHHBBBI", 19, 8, 2, 28000, 1000, 28000, 2, 0, 0, pdo)
        step = kr.decode_charger_test(page(meta, 0, [row]))["steps"][0]
        self.assertEqual((step["algorithm"], step["group"], step["protocol"], step["result"]),
                         ("epr-avs", "pd", "epr-avs", "pass"))
        self.assertEqual(step["explanation"], "none")

    def test_schema2_every_epr_skip_names_its_reason(self):
        # (algorithm, pos, reason id, detail) -> wording; mirrors ct::explanation() in src/chargertest/charger_test.cpp
        cases = [
            (2, 8, 22, 0, "EPR: cable not 50V 5A"),
            (2, 9, 3, 144, "EPR: above K2's 36V monitor"),
            (2, 9, 3, 145, "EPR: above K2's 48V monitor"),
            (19, 10, 3, 144, "EPR: above K2's 36V monitor"),
            (2, 0, 27, 139, "EPR: refused (RDO)"),
            (2, 0, 27, 137, "EPR: source rejects cable"),
            (19, 0, 27, 0, "EPR: entry refused"),
            (2, 0, 28, 0, "EPR: no PD contract"),
            (2, 0, 2, 0, "EPR: not offered"),
        ]
        meta = bytearray(HEADER2)
        struct.pack_into("<I", meta, 15, (1 << 2) | (1 << 19))
        for code, pos, why, detail, text in cases:
            row = struct.pack("<BBBHHHBBBI", code, pos, 0, 36000, 0, 0, 4, why, detail, 0)
            step = kr.decode_charger_test(page(meta, 0, [row]))["steps"][0]
            self.assertEqual(step["explanation"], text, (code, pos, why, detail))
            if detail >= 136:   # never mislabelled as a pd::Err or a sink result
                self.assertEqual(step["detail_reason"], text)

    def test_schema2_shape_and_origin_races_leave_output_untouched(self):
        row = bytes([6]) + ROW0[1:]
        good = page(HEADER2, 0, [row])
        for offset, value in ((0, 3), (11, 113), (23, 0x3C), (39, 12), (18, 0x80), (22, 0x80),
                              (40, 20), (41, 12), (42, 0x80), (49, 5), (50, len(kr.TEST_REASONS))):
            bad = bytearray(good); bad[offset] = value
            with self.subTest(offset=offset), self.assertRaises(kr.FrameError):
                kr.decode_charger_test(bad)
        for offset in (15, 19, 23, 24, 25, 26, 28, 30, 32, 33, 37, 39):
            changed = bytearray(good); changed[offset] ^= 1
            with self.subTest(offset=offset), patch.object(Path, "write_text") as write:
                with self.assertRaises(kr.FrameError):
                    kr.cmd_charger_test(Link([HEADER2, changed]), self.args("results", "--out", "host-build/evidence.json"))
                write.assert_not_called()
        for bad in (HEADER2[:39], HEADER2 + b"\0", good + ROW0, good[:-1]):
            with self.assertRaises(kr.FrameError):
                kr.decode_charger_test(bad)

    def test_schema2_empty_and_bounded_maximum(self):
        empty = bytearray(HEADER2); empty[1:23] = bytes(22)
        r = kr.cmd_charger_test(Link([empty, empty]), self.args("results"))
        self.assertEqual(r["selection"]["run"], []); self.assertEqual(r["steps"], [])
        meta = bytearray(HEADER2); meta[11] = meta[12] = 112
        row = bytes([6]) + ROW0[1:]
        link = Link([meta, *[page(meta, i, [row]) for i in range(112)], meta])
        r = kr.cmd_charger_test(link, self.args("results"))
        self.assertEqual([s["index"] for s in r["steps"]], list(range(112)))
        self.assertEqual(len(link.sent), 114)

    def test_legacy_fallback_only_on_rejected_version(self):
        for status in (kr.STATUS_BAD_LENGTH, kr.STATUS_BAD_ARG):
            link = Link([kr.RemoteError(status, kr.CMD_CHARGER_TEST), HEADER])
            r = kr.cmd_charger_test(link, self.args("status"))
            self.assertEqual(r["schema"], 1); self.assertIsNone(r["evidence"])
            self.assertEqual(link.sent, [(0x0B, b"\x02"), (0x0B, b"")])
        for error in (kr.RemoteError(kr.STATUS_BUSY, kr.CMD_CHARGER_TEST), kr.RemoteTimeout(), b"bad"):
            link = Link([error])
            with self.assertRaises((kr.RemoteError, kr.RemoteTimeout, kr.FrameError)):
                kr.cmd_charger_test(link, self.args("status"))
            self.assertEqual(len(link.sent), 1)


class ChargerUnits(unittest.TestCase):
    """The unit list of the next run: names -> charger_test::Unit values, one CMD 0x0C frame [n, unit...]."""

    def test_names_follow_the_firmware_enum(self):
        import re
        hdr = (Path(__file__).resolve().parent.parent / "include" / "k2" / "chargertest" / "charger_test.hpp").read_text()
        body = re.search(r"enum class Unit : uint8_t \{(.*?)\}", hdr, re.S).group(1)
        names = [n.strip() for n in body.split(",") if n.strip() and n.strip() != "Count"]
        self.assertEqual(len(names), len(kr.TEST_UNITS))
        self.assertEqual(names[0], "Pd")
        self.assertEqual(names[-1], "PdLow")

    def test_parse(self):
        self.assertEqual(kr.parse_units("qc2,qc3"), [1, 2])
        self.assertEqual(kr.parse_units(" Qc3+ , pd , pd-low "), [3, 0, 12])
        self.assertEqual(kr.parse_units(""), [])
        self.assertEqual(kr.parse_units("default"), [])
        with self.assertRaises(kr.FrameError):
            kr.parse_units("qc2,nope")
        with self.assertRaises(kr.FrameError):
            kr.parse_units("qc2,qc3,qc2")

    def test_wire_frame(self):
        class UnitLink:
            def __init__(self):
                self.sent = []

            def call(self, cmd, payload=b""):
                self.sent.append((cmd, payload))
                return kr.Response(cmd, 1, 0, payload[:1])
        link = UnitLink()
        r = kr.charger_units(link, kr.parse_units("qc2,qc3"))
        self.assertEqual(link.sent, [(0x0C, bytes([2, 1, 2]))])
        self.assertEqual(r, {"units": ["qc2", "qc3"], "count": 2})
        link.sent.clear()
        self.assertEqual(kr.charger_units(link, [])["units"], ["default"])
        self.assertEqual(link.sent, [(0x0C, bytes([0]))])


class ChargerCable(unittest.TestCase):
    """CMD 0x0D: set or read the cable identity of the START dialog; the 30-byte reply names the identity of the run."""

    @staticmethod
    def reply(choice, seq, run, run_seq, name):
        return (bytes([choice]) + seq.to_bytes(2, "little") + bytes([run]) + run_seq.to_bytes(2, "little")
                + name.encode().ljust(24, b"\0"))

    class CableLink:
        def __init__(self, payload):
            self.sent, self.payload = [], payload

        def call(self, cmd, payload=b""):
            self.sent.append((cmd, payload))
            return kr.Response(cmd, 1, 0, self.payload)

    def test_set_sends_kind_and_id_and_decodes_the_reply(self):
        link = self.CableLink(self.reply(1, 0, 1, 0, "EPR 50V 5A"))
        r = kr.charger_cable(link, kr.parse_cable("builtin"))
        self.assertEqual(link.sent, [(0x0D, bytes([1, 1, 0]))])      # kind Builtin, seq = the built-in id (1 = EPR 50V 5A)
        self.assertEqual(r, {"choice": "builtin", "choice_seq": 0, "run": "builtin", "run_seq": 0, "run_name": "EPR 50V 5A"})
        link = self.CableLink(self.reply(1, 4, 1, 4, "OnePlus 100W"))
        r = kr.charger_cable(link, kr.parse_cable("builtin:oneplus-mag"))     # the retired spelling is sent as id 4
        self.assertEqual(link.sent, [(0x0D, bytes([1, 4, 0]))])
        self.assertEqual((r["choice"], r["choice_seq"], r["run_seq"], r["run_name"]), ("builtin", 4, 4, "OnePlus 100W"))

    def test_read_sends_no_payload_and_names_a_saved_clone(self):
        link = self.CableLink(self.reply(2, 3, 2, 3, "My clone"))
        r = kr.charger_cable(link)
        self.assertEqual(link.sent, [(0x0D, b"")])
        self.assertEqual((r["run"], r["run_seq"], r["run_name"]), ("saved", 3, "My clone"))
        self.assertEqual(kr.charger_cable(self.CableLink(self.reply(0, 0, 0, 0, "")))["run"], "off")

    def test_short_or_bad_reply_is_a_frame_error(self):
        with self.assertRaises(kr.FrameError):
            kr.charger_cable(self.CableLink(b"\0" * 29))
        with self.assertRaises(kr.FrameError):
            kr.charger_cable(self.CableLink(self.reply(3, 0, 0, 0, "")))

    def test_stage_names_include_the_dialog(self):
        self.assertEqual(kr.TEST_STAGES[10:], ["changes", "ask", "refuse", "waiting", "custom"])


class ChargerHunt(unittest.TestCase):
    """CMD 0x0E: the evidence of a refused 6 A hunt row (charger_test::HuntEvidence, field for field)."""

    @staticmethod
    def slot(step=3, cause=9, answer=2, last_step=10, caps=7, pos=6, ptype=4, flags=0x7F, apdo_ma=5000,
             req_mv=20000, req_ma=6000, cable_ma=5000, authed_ms=420, answer_ms=33, apdo=0xC0DC3240, rdo=0x1234ABCD):
        return struct.pack("<7B7H2I", step, cause, answer, last_step, caps, pos, ptype, flags, apdo_ma, req_mv, req_ma,
                           cable_ma, authed_ms, answer_ms, apdo, rdo)

    @staticmethod
    def reply(run_id, count, slot=b""):
        return struct.pack("<IB", run_id, count) + slot

    class HuntLink:
        def __init__(self, payloads):
            self.payloads, self.sent = list(payloads), []

        def call(self, cmd, payload=b""):
            self.sent.append((cmd, payload))
            p = self.payloads.pop(0)
            if isinstance(p, Exception):
                raise p
            return kr.Response(cmd, len(self.sent), 0, p)

    def test_exact_request_and_decode(self):
        link = self.HuntLink([self.reply(0x01020304, 2, self.slot())])
        e = kr.charger_hunt(link, 0)
        self.assertEqual(link.sent, [(0x0E, b"\x00")])
        self.assertEqual(e["run_id"], 0x01020304)
        self.assertEqual(e["count"], 2)
        self.assertEqual((e["step"], e["cause"], e["answer"], e["last_step"], e["caps"], e["pos"], e["type"]),
                         (3, "refused-offer", "reject", 10, 7, 6, "pps"))
        self.assertEqual((e["flags"], e["apdo_ma"], e["req_mv"], e["req_ma"], e["cable_ma"], e["authed_ms"], e["answer_ms"]),
                         (0x7F, 5000, 20000, 6000, 5000, 420, 33))
        self.assertEqual((e["apdo"], e["rdo"]), (0xC0DC3240, 0x1234ABCD))
        self.assertEqual(e["explanation"], "refused an offered 6 A")
        self.assertEqual(e["flag_names"], ["svid", "opener", "authed", "reannounced", "in-chain", "held", "sent"])

    def test_hard_reset_flag_decodes(self):
        # HfHardReset (1024): a hunt row answered by a source Hard Reset (Xiaomi 20K, hardware 2026-11-07); bit 11 is still unknown
        e = kr.charger_hunt(self.HuntLink([self.reply(1, 1, self.slot(flags=0x401))]), 0)
        self.assertEqual(e["flag_names"], ["svid", "hard-reset"])
        with self.assertRaises(kr.FrameError):
            kr.charger_hunt(self.HuntLink([self.reply(1, 1, self.slot(flags=0x800))]), 0)

    def test_flag_names_follow_the_firmware_enum(self):
        import re
        hpp = (Path(__file__).resolve().parents[1] / "include/k2/chargertest/charger_test.hpp").read_text()
        bits = [int(v) for v in re.findall(r"\bHf[A-Za-z]+ = (\d+)", hpp)]
        self.assertEqual(bits, [1 << i for i in range(len(bits))])
        self.assertEqual(len(kr.HUNT_FLAGS), len(bits))

    def test_slot_past_the_count_is_header_only(self):
        e = kr.charger_hunt(self.HuntLink([self.reply(7, 1)]), 1)
        self.assertEqual((e["run_id"], e["count"], e["slot"]), (7, 1, None))

    def test_bad_replies_are_frame_errors(self):
        for bad in (b"", self.reply(1, 1, self.slot())[:-1], self.reply(1, 1, self.slot()) + b"\0",
                    self.reply(1, 1, self.slot(cause=11)), self.reply(1, 1, self.slot(answer=4)),
                    self.reply(1, 1, self.slot(ptype=6)), self.reply(1, 5), self.reply(1, 0, self.slot())):
            with self.subTest(bad=bad), self.assertRaises(kr.FrameError):
                kr.charger_hunt(self.HuntLink([bad]), 0)

    def test_cause_names_follow_the_firmware_enum(self):
        import re
        hdr = (Path(__file__).resolve().parent.parent / "include" / "k2" / "chargertest" / "charger_test.hpp").read_text()
        body = re.search(r"enum class HuntCause : uint8_t \{(.*?)\n\};", hdr, re.S).group(1)
        enum = [re.match(r"\s*(\w+)", ln).group(1) for ln in body.splitlines() if re.match(r"\s*[A-Z]\w+,", ln)]
        self.assertEqual(len(enum), len(kr.HUNT_CAUSES))
        src = (Path(__file__).resolve().parent.parent / "src" / "chargertest" / "charger_test.cpp").read_text()
        for c, word in zip(enum, kr.HUNT_CAUSES):
            self.assertIn(f"case HuntCause::{c}: return \"{word}\";", src)

    def test_evidence_is_attached_to_the_matching_hunt_row_only(self):
        rows = [dict(r, index=i) for i, r in enumerate(EXPECTED_ROWS)]
        rows[2] = dict(rows[2], protocol="pps", request_ma=6000, result="fail")
        link = self.HuntLink([self.reply(0x01020304, 1, self.slot(step=2))])
        kr.attach_hunt_evidence(link, 0x01020304, rows)
        self.assertEqual(link.sent, [(0x0E, b"\x00")])
        self.assertNotIn("hunt", rows[0])
        self.assertEqual(rows[2]["hunt"]["cause"], "refused-offer")
        self.assertNotIn("run_id", rows[2]["hunt"])

    def test_a_failed_full_current_pps_row_is_not_a_hunt(self):
        # The wire has no hunt flag: the host separates the 6 A probe from the full-current PPS row (5 A at most) by the request.
        row = dict(EXPECTED_ROWS[2], index=2, protocol="pps", request_ma=5000, result="fail")
        link = self.HuntLink([])
        rows = [row]
        kr.attach_hunt_evidence(link, 1, rows)
        self.assertEqual(link.sent, [])      # no evidence call: nothing to classify
        self.assertNotIn("hunt", rows[0])

    def test_the_hunt_threshold_follows_the_firmware_plan(self):
        # Drift guard for tools/step_label.py: the probe asks kHuntMa, no plan row asks more than the cable ceiling.
        import re
        import step_label
        root = Path(__file__).resolve().parent.parent
        src = (root / "src" / "chargertest" / "charger_test.cpp").read_text()
        hdr = (root / "include" / "k2" / "chargertest" / "charger_test.hpp").read_text()
        probes = set(re.findall(r"h\.ma = (\d+);", src))
        self.assertEqual(probes, {"6000"})                      # every hunt row asks 6 A
        self.assertGreater(6000, step_label.HUNT_MA)
        cable = re.search(r"uint16_t cable_ma\(Input in\) \{.*?: std::min<unsigned>\(in\.cable_ma, in\.cable_observed \? (\d+) : (\d+)\)", src)
        self.assertEqual(int(cable.group(1)), step_label.HUNT_MA)   # the most a plan row can ask: the 5 A cable
        self.assertEqual(int(re.search(r"kPlanRowMa = (\d+);", hdr).group(1)), step_label.PLAN_ROW_MA)

    def test_a_not_unlocked_skip_is_a_refusal_too(self):
        rows = [dict(EXPECTED_ROWS[2], index=2, protocol="pps", request_ma=6000, result="skip", reason="not unlocked")]
        link = self.HuntLink([self.reply(5, 1, self.slot(step=2, cause=1, flags=0))])
        kr.attach_hunt_evidence(link, 5, rows)
        self.assertEqual(rows[0]["hunt"]["cause"], "no-chain")

    def test_no_hunt_fail_row_means_no_call(self):
        link = self.HuntLink([])
        kr.attach_hunt_evidence(link, 1, [dict(EXPECTED_ROWS[2], protocol="pps", request_ma=6000, result="pass")])
        kr.attach_hunt_evidence(link, 1, [dict(EXPECTED_ROWS[2], protocol="afc", request_ma=0)])
        self.assertEqual(link.sent, [])

    def test_older_firmware_or_other_run_attaches_nothing(self):
        row = dict(EXPECTED_ROWS[2], protocol="pps", request_ma=6000, result="fail")
        for p in (kr.RemoteError(kr.STATUS_UNKNOWN_CMD, 0x0E), self.reply(99, 1, self.slot(step=2))):
            rows = [dict(row, index=2)]
            kr.attach_hunt_evidence(self.HuntLink([p]), 1, rows)
            self.assertNotIn("hunt", rows[0])

    def test_results_export_and_summary_carry_the_cause(self):
        meta = bytearray(HEADER2); meta[11] = 1; meta[12] = 0
        meta = bytes(meta)
        # code 4 = the PPS row, pos 6, 20 V, 6 A asked, failed with "request failed" (a Mi 6 A probe)
        row = struct.pack("<BBBHHHBBBI", 4, 6, 0, 20000, 6000, 0, 3, 11, 7, 0xC0DC3240)
        link = self.HuntLink([meta, page(meta, 0, [row]), meta, self.reply(0x01020304, 1, self.slot(step=0))])
        # Calls: status probe, the page, the end-of-export check, then the hunt slot (read after the export is stable).
        r = kr.cmd_charger_test(link, self.args("results"))
        self.assertEqual(r["steps"][0]["hunt"]["cause"], "refused-offer")
        self.assertEqual([c for c, _ in link.sent], [0x0B, 0x0B, 0x0B, 0x0E])
        self.assertIn("refused-offer", kr._render("charger-test", r))

    def args(self, *args):
        return kr.build_parser().parse_args(["charger-test", *args])


if __name__ == "__main__":
    unittest.main()
