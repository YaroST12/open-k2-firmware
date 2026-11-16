# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The PD spec section of a Deep charger test on the tool side: the decoder (tools/pdspec_blob.py decode_section), the chunked read
(k2_remote.read_charger_spec, request 0x0B [6, off]), the export attach and the summary lines (tools/charger_summary.py).

The decoder is checked against the firmware's own bytes: a small host program built from include/k2/chargertest/spec_section.hpp
fills a Result, writes its wire form with specsec::to_wire, and the decoder must read back what was put in."""
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

import charger_summary
import k2_remote as kr
import pdspec_blob as pb
from test_k2_remote_charger_test import HEADER2, ROW0, page

HAVE_GXX = shutil.which("g++") is not None

DUMP_CPP = r"""
#include <cstdio>
#include "k2/chargertest/spec_section.hpp"
using namespace k2; using namespace k2::specsec;
int main() {
    Result r;
    r.end = End::Done; r.unconfirmed = true;
    const auto unit = [](std::size_t i) { return sc::kDefs[static_cast<std::size_t>(kOrder[i])].unit; };
    r.cells[0] = make_cell(sc::Verdict::Pass, sc::Skip::None, 0, quantize(unit(0), 2500));
    r.cells[1] = make_cell(sc::Verdict::Fail, sc::Skip::None, 3, quantize(unit(1), 40000));
    r.cells[2] = make_cell(sc::Verdict::Skip, sc::Skip::NoPps, 0);
    r.cells[3] = make_cell(sc::Verdict::Unverified, sc::Skip::None, 1, quantize(unit(3), 1200));
    r.cells[4] = make_cell(sc::Verdict::Pass, sc::Skip::None, 0, quantize(unit(4), sc::kNever));
    // The detail form: the limits, count and place the device keeps beside the worst value (a count 0 cell has none).
    r.cells[0].lo = quantize_limit(unit(0), -sc::kOpenLimit); r.cells[0].hi = quantize_limit(unit(0), 25000);
    r.cells[0].n = 12; r.cells[0].a = 2; r.cells[0].b = 3;
    r.cells[1].lo = quantize_limit(unit(1), 0); r.cells[1].hi = quantize_limit(unit(1), 25000);
    r.cells[1].n = 300; r.cells[1].a = 4; r.cells[1].b = 2;
    r.cells[3].lo = quantize_limit(unit(3), 0); r.cells[3].hi = quantize_limit(unit(3), 1000);
    r.cells[3].n = 7;
    std::array<std::uint8_t, kWireBytes> w{};
    to_wire(r, w);
    std::array<std::uint8_t, kDetailBytes> dw{};
    to_detail_wire(r, dw);
    std::printf("wire ");
    for (auto b : w) std::printf("%02x", b);
    std::printf("\ndetail ");
    for (auto b : dw) std::printf("%02x", b);
    std::printf("\ndetailsize %zu\nsize %zu\norder", kDetailBytes, kWireBytes);
    for (std::size_t i = 0; i < kCells; ++i) std::printf(" %s", sc::kDefs[static_cast<std::size_t>(kOrder[i])].key);
    std::printf("\nsteps %d %d %d\nunseen %d\n", int(unit_step(sc::Unit::Us)), int(unit_step(sc::Unit::Mv)),
                int(unit_step(sc::Unit::Ms)), int(kUnseen));
}
"""


def build_and_run(src, tag):
    with tempfile.TemporaryDirectory() as d:
        cpp = Path(d) / f"{tag}.cpp"
        exe = Path(d) / tag
        cpp.write_text(src)
        subprocess.run(["g++", "-std=c++23", "-DK2_PDSPEC=1", "-I", str(ROOT / "include"), str(cpp), "-o", str(exe)], check=True,
                       capture_output=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


_dump = {}


def fw_dump():
    if not _dump:
        for line in build_and_run(DUMP_CPP, "section").splitlines():
            k, v = line.split(" ", 1)
            _dump.setdefault(k, []).append(v)
    return _dump


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Decode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fw = fw_dump()
        cls.blob = bytes.fromhex(cls.fw["wire"][0])
        cls.detail = bytes.fromhex(cls.fw["detail"][0])
        cls.sec = pb.decode_section(cls.blob)
        cls.full = pb.decode_section(cls.blob, cls.detail)

    def test_tables_match_the_firmware(self):
        t = pb.section_tables()
        self.assertEqual(t["wire_bytes"], int(self.fw["size"][0]))
        self.assertEqual(t["detail_bytes"], int(self.fw["detailsize"][0]))
        self.assertEqual([c["key"] for c in t["cells"]], self.fw["order"][0].split())
        self.assertEqual(len(t["cells"]), 27)
        self.assertEqual((t["steps"]["us"], t["steps"]["mv"], t["steps"]["ms"]), tuple(int(x) for x in self.fw["steps"][0].split()))
        self.assertEqual(t["unseen"], int(self.fw["unseen"][0]))

    def test_header(self):
        s = self.sec
        self.assertEqual((s["end"], s["unconfirmed"], len(s["cells"])), ("done", True, 27))
        self.assertEqual([c["key"] for c in s["cells"]], self.fw["order"][0].split())
        self.assertEqual(s["tally"], {"pass": 2, "fail": 1, "skip": 1, "unverified": 1, "pending": 22})

    def test_cells(self):
        c = self.sec["cells"]
        self.assertEqual((c[0]["verdict"], c[0]["fails"], c[0]["skip"]), ("pass", 0, None))
        self.assertEqual((c[1]["verdict"], c[1]["fails"]), ("fail", 3))
        self.assertEqual((c[2]["verdict"], c[2]["skip"], c[2]["value"]), ("skip", "no PPS", None))
        self.assertEqual((c[3]["verdict"], c[3]["fails"]), ("unverified", 1))
        self.assertEqual(c[5]["verdict"], "pending")
        self.assertIsNone(c[5]["value"])
        # accept_time is a us check: 2500 us quantized to 0.1 ms, read back as 2.5 ms
        self.assertEqual(c[0]["unit"], "us")
        self.assertEqual(c[0]["value"], 2500)
        self.assertEqual(c[0]["value_text"], "2.5 ms")
        self.assertEqual(c[1]["value_text"], "40.0 ms")
        # the event that never came
        self.assertEqual((c[4]["value"], c[4]["value_text"]), (pb.NEVER, "not seen"))

    def test_the_shared_fixture_is_current(self):
        # tests/fixtures/ct_spec_section.json is read by the web tests too: the firmware's bytes and this decoder's reading of them
        fx = json.loads((ROOT / "tests" / "fixtures" / "ct_spec_section.json").read_text())
        self.assertEqual(fx["wire"], self.fw["wire"][0])
        self.assertEqual(fx["decoded"], self.sec)
        self.assertEqual(fx["detail"], self.fw["detail"][0])
        self.assertEqual(fx["decoded_detail"], self.full)

    def test_the_detail_form_gives_limit_count_and_place(self):
        c = self.full["cells"]
        # accept_time: a one-sided max 25 ms judged 12 times, the worst between PDO 2 and 3
        self.assertEqual((c[0]["count"], c[0]["limit_text"], c[0]["limit"], c[0]["at_text"]), (12, "<= 25.0 ms", [-pb.OPEN, 25000], "PDO 2 to 3"))
        self.assertEqual((c[1]["count"], c[1]["limit_text"]), (300, "<= 25.0 ms"))
        # ps_rdy_early (mV, a max) keeps a limit and its count; nothing for a pending or skipped cell
        self.assertEqual((c[3]["count"], c[3]["limit_text"]), (7, "<= 1.000 V"))
        self.assertEqual((c[2]["count"], c[2]["limit_text"], c[2]["at_text"]), (0, None, None))
        self.assertEqual((c[5]["count"], c[5]["limit_text"]), (0, None))
        self.assertTrue(self.full["detailed"])
        self.assertFalse(self.sec["detailed"])
        self.assertNotIn("count", self.sec["cells"][0])   # the first form alone adds no detail keys

    def test_a_measured_cell_with_no_count_has_no_detail(self):
        # a section restored from a journal written before the detail record: counts 0, the cells show nothing kept
        blank = pb.decode_section(self.blob, bytes(pb.section_tables()["detail_bytes"]))
        self.assertTrue(all(c["limit_text"] is None for c in blank["cells"]))
        self.assertFalse(blank["detailed"])

    def test_a_wrong_detail_size_is_rejected(self):
        with self.assertRaises(ValueError):
            pb.decode_section(self.blob, self.detail[:-1])

    def test_bad_blobs(self):
        with self.assertRaises(ValueError):
            pb.decode_section(self.blob[:-1])
        bad = bytearray(self.blob)
        bad[0] = 9   # an End past the last one
        with self.assertRaises(ValueError):
            pb.decode_section(bytes(bad))

    def test_why_text(self):
        w = bytearray(self.blob)
        w[0], w[1] = 3, 1   # Fault, a pdspec::Stop
        s = pb.decode_section(bytes(w))
        self.assertEqual(s["end"], "fault")
        self.assertEqual(s["why"], pb.load_tables()["stops"][1])


def wire_chunks(blob, run_id, total=None):
    """The 0x0B [6, off] (or [7, off]) replies the firmware gives: run_id u32, total u8, off u8, n u8, data (48 bytes a chunk)."""
    total = len(blob) if total is None else total
    out = []
    for off in range(0, max(total, 1), 48):
        data = blob[off:off + 48] if total else b""
        out.append(struct.pack("<IBBB", run_id, total, off, len(data)) + data)
    return out


class Link:
    def __init__(self, payloads):
        self.payloads, self.sent = list(payloads), []

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, payload))
        p = self.payloads.pop(0)
        if isinstance(p, Exception):
            raise p
        return kr.Response(cmd, len(self.sent), 0, p)


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Read(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = bytes.fromhex(fw_dump()["wire"][0])
        cls.detail = bytes.fromhex(fw_dump()["detail"][0])

    def test_chunked_read_and_exact_requests(self):
        link = Link(wire_chunks(self.blob, 0x01020304) + wire_chunks(self.detail, 0x01020304))
        sec = kr.read_charger_spec(link, 0x01020304)
        self.assertEqual(link.sent, [(0x0B, bytes([6, 0])), (0x0B, bytes([6, 48])), (0x0B, bytes([6, 96])),
                                     (0x0B, bytes([7, 0])), (0x0B, bytes([7, 48])), (0x0B, bytes([7, 96])), (0x0B, bytes([7, 144])),
                                     (0x0B, bytes([7, 192]))])
        self.assertEqual(sec, pb.decode_section(self.blob, self.detail))
        self.assertEqual(sec["cells"][1]["count"], 300)

    def test_firmware_without_the_detail_form_gives_the_section_alone(self):
        link = Link(wire_chunks(self.blob, 5) + [kr.RemoteError(kr.STATUS_BAD_LENGTH, 0x0B)])
        sec = kr.read_charger_spec(link, 5)
        self.assertEqual(sec, pb.decode_section(self.blob))
        self.assertEqual(link.sent[-1], (0x0B, bytes([7, 0])))

    def test_a_run_that_moves_during_the_detail_read_is_a_changed_result(self):
        link = Link(wire_chunks(self.blob, 5) + wire_chunks(self.detail, 6))
        with self.assertRaises(kr.ResultsChanged):
            kr.read_charger_spec(link, 5)

    def test_no_section_is_none(self):
        link = Link(wire_chunks(b"", 7, total=0))
        self.assertIsNone(kr.read_charger_spec(link, 7))   # nothing asked for the detail either
        self.assertEqual(link.sent, [(0x0B, bytes([6, 0]))])

    def test_a_moved_run_is_a_changed_result(self):
        link = Link(wire_chunks(self.blob, 8))
        with self.assertRaises(kr.ResultsChanged):
            kr.read_charger_spec(link, 7)

    def test_a_stuck_chunk_is_a_frame_error(self):
        link = Link([struct.pack("<IBBB", 7, 111, 0, 0)])
        with self.assertRaises(kr.FrameError):
            kr.read_charger_spec(link, 7)


def deep_meta():
    meta = bytearray(HEADER2)
    meta[11] = 1; meta[12] = 0     # total 1, current 0
    meta[18] |= 0x20               # run_mask bits 28..29 = 2: the run in hand ran at Deep
    return bytes(meta)


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Export(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = bytes.fromhex(fw_dump()["wire"][0])
        cls.detail = bytes.fromhex(fw_dump()["detail"][0])

    def args(self, *args):
        return kr.build_parser().parse_args(["charger-test", *args])

    def test_a_deep_export_carries_the_spec_section(self):
        meta = deep_meta()
        run_id = int.from_bytes(meta[1:5], "little")
        link = Link([meta, page(meta, 0, [ROW0]), meta, *wire_chunks(self.blob, run_id), *wire_chunks(self.detail, run_id)])
        r = kr.cmd_charger_test(link, self.args("results"))
        self.assertEqual(r["selection"]["run_level"], "deep")
        self.assertEqual([p for _, p in link.sent[3:]], [bytes([6, 0]), bytes([6, 48]), bytes([6, 96]), bytes([7, 0]), bytes([7, 48]),
                                                         bytes([7, 96]), bytes([7, 144]), bytes([7, 192])])
        self.assertEqual(r["spec"], pb.decode_section(self.blob, self.detail))
        text = kr._render("charger-test", dict(r, _summary=True))
        self.assertIn("PD spec", text)

    def test_a_medium_export_does_not_ask(self):
        meta = bytes(HEADER2[:11]) + bytes([1, 0]) + bytes(HEADER2[13:])
        link = Link([meta, page(meta, 0, [ROW0]), meta])
        r = kr.cmd_charger_test(link, self.args("results"))
        self.assertEqual(len(link.sent), 3)
        self.assertNotIn("spec", r)

    def test_a_deep_export_without_the_command_stands(self):
        meta = deep_meta()
        link = Link([meta, page(meta, 0, [ROW0]), meta, kr.RemoteError(kr.STATUS_BAD_LENGTH, 0x0B)])
        r = kr.cmd_charger_test(link, self.args("results"))
        self.assertNotIn("spec", r)


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Summary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sec = pb.decode_section(bytes.fromhex(fw_dump()["wire"][0]))
        cls.full = pb.decode_section(bytes.fromhex(fw_dump()["wire"][0]), bytes.fromhex(fw_dump()["detail"][0]))

    def lines(self, sec):
        return charger_summary._spec_lines(sec)

    def test_none_is_nothing(self):
        self.assertEqual(self.lines(None), [])

    def test_header_failures_and_unverified(self):
        text = "\n".join(self.lines(self.sec))
        self.assertIn("PD spec: 2 pass, 1 fail, 1 skip, 1 unverified", text)
        self.assertIn("return to 5 V was not confirmed", text)
        fail = pb.section_tables()["cells"][1]["label"]
        self.assertIn(f"{fail}: FAIL", text)
        self.assertIn("40.0 ms", text)
        self.assertIn("no PPS", text)

    def test_the_limit_and_count_join_the_failure_line_when_the_device_kept_them(self):
        text = "\n".join(self.lines(self.full))
        fail = pb.section_tables()["cells"][1]["label"]
        self.assertIn(f"{fail}: FAIL, worst 40.0 ms, 3 over (limit <= 25.0 ms, 300 measured)", text)
        self.assertNotIn("(limit", "\n".join(self.lines(self.sec)))   # a journal from before the detail: the line as it was

    def test_an_ended_early_stage_says_why(self):
        sec = dict(self.sec, end="cancelled", why="user stop")
        self.assertIn("cancelled", "\n".join(self.lines(sec)))


if __name__ == "__main__":
    unittest.main()
