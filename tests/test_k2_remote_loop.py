# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote.py loop-probe` (CMD_LOOP_PROBE 0x57): the chunked read of the stored loop probe file
(/lp/last.rec, include/k2/loopprobe/loop_probe.hpp) and its JSON decode via tools/loop_probe_rec.py."""
import json
import os
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_remote as kr  # noqa: E402
import loop_probe_rec as lp  # noqa: E402


def fixture_record():
    with open(os.path.join(HERE, "fixtures", "qrshare.tsv")) as f:
        for ln in f:
            c = ln.rstrip("\n").split("\t")
            if c[0] == "loop":
                return bytes.fromhex(c[3])
    raise AssertionError("no loop row in tests/fixtures/qrshare.tsv")


def file_of(rec):
    return b"LPR\x01" + len(rec).to_bytes(2, "little") + lp.crc16(rec).to_bytes(2, "little") + rec


class FakeLink:
    """Answers CMD_LOOP_PROBE like the firmware dispatcher (include/k2/core/remote_proto.hpp): off total n data."""

    def __init__(self, blob):
        self.blob = blob
        self.offs = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_LOOP_PROBE
        assert len(payload) == 2
        off = int.from_bytes(payload, "little")
        self.offs.append(off)
        assert off <= len(self.blob)
        chunk = self.blob[off:off + kr.LOOP_CHUNK]
        return type("R", (), {"payload": off.to_bytes(2, "little") + len(self.blob).to_bytes(2, "little")
                              + bytes([len(chunk)]) + chunk})()


class LoopProbeRead(unittest.TestCase):
    def test_command_id_matches_firmware(self):
        self.assertEqual(kr.CMD_LOOP_PROBE, 0x57)
        self.assertEqual(kr.LOOP_CHUNK, 48)

    def test_reads_the_whole_file_in_chunks(self):
        f = file_of(fixture_record())
        link = FakeLink(f)
        self.assertEqual(kr.loop_probe_read_file(link), f)
        self.assertEqual(link.offs, list(range(0, len(f), kr.LOOP_CHUNK)))

    def test_empty_store_reads_nothing(self):
        link = FakeLink(b"")
        self.assertEqual(kr.loop_probe_read_file(link), b"")
        self.assertEqual(link.offs, [0])

    def test_command_decodes_to_json(self):
        rec = fixture_record()
        r = kr.cmd_loop_probe(FakeLink(file_of(rec)), None)
        self.assertEqual(r, {"loop_probe": lp.decode_record(rec)})
        json.dumps(r)   # JSON-ready

    def test_command_without_a_result(self):
        self.assertEqual(kr.cmd_loop_probe(FakeLink(b""), None), {"loop_probe": None})
        self.assertIn("no loop probe result", kr.render("loop-probe", {"loop_probe": None}))

    def test_corrupt_file_is_an_error(self):
        f = bytearray(file_of(fixture_record()))
        f[-1] ^= 1
        with self.assertRaises(SystemExit):
            kr.cmd_loop_probe(FakeLink(bytes(f)), None)

    def test_cli_parses_and_runs(self):
        rec = fixture_record()
        a = kr.build_parser().parse_args(["loop-probe"])
        shown = []
        r = kr.run_command(FakeLink(file_of(rec)), a, show=shown.append)
        self.assertEqual(shown, [r])
        self.assertEqual(json.loads(json.dumps(r))["loop_probe"]["steps"], lp.decode_record(rec)["steps"])
        text = kr.render("loop-probe", r)
        self.assertIn("baseline", text)
        self.assertIn("1.0.0-515-67ce114d", text)


if __name__ == "__main__":
    unittest.main()
