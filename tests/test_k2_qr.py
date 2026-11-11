# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/k2_qr.py against the golden vectors the firmware encoder produced (tests/fixtures/qrshare.tsv,
checked by tests/test_qrshare_records.cpp): the Python side decodes every fixture, re-encodes the record to the same
part texts byte for byte (an implementation independent of src/ui/qrshare.cpp), and reads the fields back."""
import io
import json
import os
import sys
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest import mock

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_qr as q  # noqa: E402
import loop_probe_rec as lp  # noqa: E402


def load():
    out = {}
    with open(os.path.join(HERE, "fixtures", "qrshare.tsv")) as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            c = ln.rstrip("\n").split("\t")
            out[c[0]] = {"type": int(c[1]), "schema": int(c[2]), "record": bytes.fromhex(c[3]), "parts": c[4:]}
    return out


FIX = load()


class Golden(unittest.TestCase):
    def test_every_fixture_present(self):
        self.assertEqual(set(FIX), {"boot", "identity", "emk_plain", "emk_rows", "pdcaps_spr", "pdcaps_epr", "card",
                                    "log", "loop", "emk_rows_lz", "card_lz", "emk_rows_v2", "card_v2"})

    def test_decode_joins_the_golden_record(self):
        for name, fx in FIX.items():
            with self.subTest(name):
                info, rec = q.assemble(fx["parts"])
                self.assertEqual(rec, fx["record"])
                self.assertTrue(info["complete"])
                self.assertEqual(info["parts"], len(fx["parts"]))

    def test_reencode_matches_the_firmware_text(self):
        for name, fx in FIX.items():
            with self.subTest(name):
                self.assertEqual(q.encode(fx["type"] & 0x7F, fx["schema"], fx["record"], packed=bool(fx["type"] & 0x80)),
                                 fx["parts"])

    def test_any_part_order(self):
        fx = FIX["emk_rows"]
        self.assertEqual(len(fx["parts"]), 2)
        _, rec = q.assemble(list(reversed(fx["parts"])))
        self.assertEqual(rec, fx["record"])

    def test_every_record_decodes_without_error(self):
        for name, fx in FIX.items():
            with self.subTest(name):
                res, code = q.decode_texts(fx["parts"])
                self.assertEqual(code, 0)
                self.assertNotIn("decode_error", res)


class SchemaV2(unittest.TestCase):
    """The schema 2 frames (the v2 rows of qrshare.tsv) carry the bit-packed record; the reader expands it to the v1 record
    and decodes it exactly like the schema 1 frame of the same record."""

    def test_v2_rows_are_schema_2(self):
        self.assertEqual(FIX["card_v2"]["schema"], 2)
        self.assertEqual(FIX["emk_rows_v2"]["schema"], 2)
        self.assertEqual(FIX["card_v2"]["record"][0], 2)
        self.assertEqual(FIX["emk_rows_v2"]["record"][0], 2)

    def test_v2_frame_decodes_like_the_v1_frame(self):
        for v2name, v1name in (("card_v2", "card"), ("emk_rows_v2", "emk_rows")):
            with self.subTest(v2name):
                a, code_a = q.decode_texts(FIX[v2name]["parts"])
                b, code_b = q.decode_texts(FIX[v1name]["parts"])
                self.assertEqual((code_a, code_b), (0, 0))
                self.assertNotIn("decode_error", a)
                self.assertTrue(a["v2_expanded"])
                self.assertEqual(a["record"], b["record"])
                self.assertEqual(a["record_hex"], b["record_hex"])

    def test_v2_frame_expands_to_the_plain_record(self):
        for v2name, v1name in (("card_v2", "card"), ("emk_rows_v2", "emk_rows")):
            with self.subTest(v2name):
                _, rec = q.assemble(FIX[v2name]["parts"])
                self.assertEqual(rec, FIX[v2name]["record"])
                expand = q.expand_card_v2 if FIX[v2name]["type"] == 4 else q.expand_emk_v2
                self.assertEqual(expand(rec), FIX[v1name]["record"])

    def test_v2_card_json_matches_the_v1_card(self):
        a = q.decode_texts(FIX["card_v2"]["parts"])[0]["record"]
        b = q.decode_texts(FIX["card"]["parts"])[0]["record"]
        self.assertTrue(a.pop("_store_crc_ok"))
        self.assertTrue(b.pop("_store_crc_ok"))
        self.assertEqual(a, b)
        self.assertEqual(a["name"], "Desk 65W")


class Links(unittest.TestCase):
    """decode_link: version '1' and '2' links of every kind, 't' too (the rows of tests/fixtures/qrlink.tsv)."""

    def rows(self):
        out = []
        with open(os.path.join(HERE, "fixtures", "qrlink.tsv")) as f:
            for ln in f:
                if ln.startswith("#") or not ln.strip():
                    continue
                name, packed, rec, link, js = ln.rstrip("\n").split("\t")
                out.append((name, link, js))
        return out

    def test_every_golden_link_decodes(self):
        for name, link, js in self.rows():
            with self.subTest(name):
                d = q.decode_link(link)
                self.assertIn(d["version"], (1, 2))
                self.assertLessEqual(len(link), 392)   # a format-10 plain card link is 392 characters: still one code (version 13)
                self.assertIn("record", d)

    def test_v2_links_decode_to_the_v1_json(self):
        rows = dict((n, (l, js)) for n, l, js in self.rows())
        for v2, v1 in (("fh22", "fh22_plain"), ("fh22", "fh22_lz")):
            with self.subTest((v2, v1)):
                a = q.decode_link(rows[v2][0])["record"]
                b = q.decode_link(rows[v1][0])["record"]
                self.assertTrue(a.pop("_store_crc_ok"))
                self.assertTrue(b.pop("_store_crc_ok"))
                self.assertEqual(a, b)

    def test_text_kind(self):
        rows = dict((n, l) for n, l, _ in self.rows())
        d = q.decode_link(rows["qrtest"])
        self.assertEqual((d["version"], d["kind"], d["record_kind"]), (1, "t", "text"))
        self.assertEqual(d["text"], "QR scan test v11 3px")

    def test_bad_links(self):
        for bad in ("https://yarost12.github.io/open-k2-firmware/r/#3c.AAAA",
                    "https://yarost12.github.io/open-k2-firmware/r/#2p.AAAA",
                    "https://yarost12.github.io/open-k2-firmware/r/#1x.AAAA",
                    "https://example.com/#1c.AAAA"):
            with self.subTest(bad):
                with self.assertRaises(q.QrError):
                    q.decode_link(bad)


class Packed(unittest.TestCase):
    """The LZ4 flag (type byte bit 7): the firmware packed these records, the reader inflates them."""

    def test_flag_is_in_the_fixture_type(self):
        for name in ("emk_rows_lz", "card_lz"):
            self.assertEqual(FIX[name]["type"] & 0x80, 0x80)
            self.assertEqual(len(FIX[name]["parts"]), 1)
        self.assertEqual(len(FIX["emk_rows"]["parts"]), 2)   # the plain record needs two parts, the packed one

    def test_assemble_reports_the_flag_and_the_block(self):
        info, rec = q.assemble(FIX["emk_rows_lz"]["parts"])
        self.assertTrue(info["packed"])
        self.assertEqual(rec, FIX["emk_rows_lz"]["record"])
        self.assertFalse(q.assemble(FIX["emk_rows"]["parts"])[0]["packed"])

    def test_decode_inflates_to_the_plain_record(self):
        for name in ("emk_rows", "card"):
            with self.subTest(name):
                packed, code = q.decode_texts(FIX[name + "_lz"]["parts"])
                plain, _ = q.decode_texts(FIX[name]["parts"])
                self.assertEqual(code, 0)
                self.assertEqual(packed["record_hex"], plain["record_hex"])
                self.assertEqual(packed["record"], plain["record"])
                self.assertEqual(packed["sent_len"], len(FIX[name + "_lz"]["record"]))
                self.assertNotIn("sent_len", plain)

    def test_reencode_with_the_flag_matches_the_firmware_text(self):
        fx = FIX["card_lz"]
        self.assertEqual(q.encode(fx["type"] & 0x7F, fx["schema"], fx["record"], packed=True), fx["parts"])

    def test_lz4_block_decoder(self):
        self.assertEqual(q.lz4_decompress(bytes([0x50]) + b"hello", 5), b"hello")                       # literals only
        self.assertEqual(q.lz4_decompress(bytes([0x14, 0x61, 0x01, 0x00, 0x50]) + b"bcdef", 14), b"a" * 9 + b"bcdef")   # overlapping match
        for bad in (b"", bytes([0x50]) + b"he", bytes([0x11, 0x61, 0x09, 0x00]), bytes([0x10, 0x61, 0x00, 0x00])):
            with self.assertRaises(q.QrError, msg=bad.hex()):
                q.lz4_decompress(bad, 99)
        with self.assertRaises(q.QrError):
            q.lz4_decompress(bytes([0x50]) + b"hello", 4)    # output longer than the header says

    def test_a_corrupt_block_is_an_error_not_a_crash(self):
        t = q.encode(4, 1, bytes([0x11, 0x61, 0x09, 0x00, 0x50]) + b"hello", packed=True)   # offset past the output
        res, code = q.decode_texts(t)
        self.assertEqual(code, 0)
        self.assertIn("decode_error", res)


class Fields(unittest.TestCase):
    def rec(self, name):
        return q.decode_texts(FIX[name]["parts"])[0]["record"]

    def test_identity(self):
        r = self.rec("identity")
        self.assertEqual((r["fw"], r["proto"], r["dirty"]), ("1.0", 1, True))
        self.assertEqual(r["git"], "fcb3128-dirty")

    def test_boot(self):
        r = self.rec("boot")
        self.assertEqual((r["fw"], r["proto"], r["git"]), ("1.0", 1, "fcb3128-dirty"))
        self.assertEqual(r["t_ms"], 12345)

    def test_emarker_plain(self):
        r = self.rec("emk_plain")
        self.assertEqual(r["name"], "EPR 50V 5A")
        self.assertTrue(r["record"]["checksum_ok"])
        self.assertEqual((r["record"]["vid"], r["record"]["pid"]), ("0x2B01", "0x4051"))
        self.assertEqual((r["record"]["max_v"], r["record"]["max_a"]), (50, 5))
        self.assertNotIn("quirks", r)

    def test_emarker_with_quirk_rows(self):
        r = self.rec("emk_rows")
        self.assertEqual(r["name"], "OnePlus 100W mag")
        self.assertEqual(r["record"]["vid"], "0x22D9")
        self.assertGreaterEqual(len(r["quirks"]["rows"]), 1)
        self.assertIn(r["quirks"]["rows"][0]["kind"], ("Ack", "Nak", "Busy", "Raw"))

    def test_pdcaps(self):
        r = self.rec("pdcaps_spr")
        self.assertEqual(len(r["pdos"]), 5)
        self.assertEqual(r["pdos"][0]["kind"], "fixed")
        self.assertEqual(r["pdos"][0]["mv"], 5000)
        r = self.rec("pdcaps_epr")
        self.assertEqual(len(r["pdos"]), 10)
        self.assertTrue(r["epr"])

    def test_card(self):
        r = self.rec("card")
        self.assertTrue(r["_store_crc_ok"])
        self.assertEqual(r["name"], "Desk 65W")

    def test_loop_probe(self):
        # the values tests/test_qrshare_records.cpp loop_record() put in, read back by the independent Python reader
        self.assertEqual(len(FIX["loop"]["parts"]), 2)
        r = self.rec("loop")
        self.assertEqual((r["fw"], r["start_ms"], r["in_progress"]), ("1.0.0-515-67ce114d", 61234, False))
        self.assertEqual(r["steps"]["drop"], {"status": "done", "why": "", "t_ms": 640, "dur_ms": 2810})
        self.assertEqual(r["steps"]["emarker"]["why"], "none sniffed")
        self.assertEqual(r["baseline"]["samples"], 16)
        self.assertEqual(r["baseline"]["vbus_mv"], {"avg_mv": 5061, "min_mv": 5048, "max_mv": 5072})
        self.assertEqual(r["baseline"]["ibus_ma"]["avg_ma"], -212)
        self.assertEqual(r["baseline"]["hid_start"]["frame"], 0x2A5)
        self.assertEqual((r["baseline"]["link"]["active_cc"], r["baseline"]["link"]["guard"]), ("CC1", "data_device"))
        self.assertEqual(r["baseline"]["bc_lvl"]["cc1"], 2)
        self.assertEqual(len(r["drop"]), 8)
        self.assertEqual(r["drop"][3]["load"], "backlight on + CPU busy")
        self.assertEqual((r["drop"][3]["vbus_avg_mv"], r["drop"][3]["ibus_avg_ma"], r["drop"][3]["aux_mv"]), (5057, -228, 5007))
        self.assertEqual([(x["drive_dp"], x["drive_dm"]) for x in r["dpdm"]["rows"]],
                         [("HiZ", "HiZ"), ("0V", "0V"), ("0.6V", "0V"), ("3.3V", "0V"), ("0V", "0.6V"), ("0V", "3.3V")])
        self.assertEqual(r["dpdm"]["rows"][3]["bus_dp_mv"], 3290)
        self.assertTrue(r["dpdm"]["rows"][3]["hid"]["rx_dp"])
        self.assertEqual(r["dpdm"]["pull_method"], "core 1.5k pull-up")
        self.assertEqual(r["dpdm"]["pull"][1], {"pullup_on": True, "bus_dp_mv": 3270, "bus_dm_mv": 4,
                                                "hid": {"core_running": False, "configured": False, "rx_dp": True,
                                                        "rx_dm": False, "pin_dp": False, "pin_dm": False}})
        self.assertFalse(r["emarker"]["valid"])
        self.assertEqual([x["rd"] for x in r["cc"]["rows"]], ["as found", "CC1", "CC2", "none", "as found"])
        self.assertEqual((r["cc"]["rows"][2]["cc2_raw"], r["cc"]["rows"][2]["vbus_mv"]), (1990, 5060))
        self.assertTrue(r["cc"]["restored"])
        self.assertEqual(r["cc"]["switches0_found"], "0x04")


class LoopProbeFile(unittest.TestCase):
    """The flash file `k2_remote.py loop-probe` reads: "LPR" + version, length, CRC-16, then the QR record's bytes."""

    @staticmethod
    def file_of(rec):
        return b"LPR\x01" + len(rec).to_bytes(2, "little") + lp.crc16(rec).to_bytes(2, "little") + rec

    def test_file_decodes_like_the_record(self):
        rec = FIX["loop"]["record"]
        self.assertEqual(lp.decode_file(self.file_of(rec)), lp.decode_record(rec))

    def test_bad_files(self):
        rec = FIX["loop"]["record"]
        good = self.file_of(rec)
        for bad in (b"", b"XYZ\x01" + good[4:], good[:3] + b"\x02" + good[4:], good[:-1],
                    good[:-1] + bytes([good[-1] ^ 1])):
            with self.subTest(bad[:8]):
                with self.assertRaises(lp.BadRecord):
                    lp.decode_file(bad)
        with self.assertRaises(lp.BadRecord):
            lp.decode_record(rec[:-1])
        with self.assertRaises(lp.BadRecord):
            lp.decode_record(b"\x02" + rec[1:])


DB = {2: "[ADC] raw sweep failed\n", 5: "[BBSTORE] entries=%d used=%u free=%u\n", 6: "[BOOT] %s\n", 8: "[BOOT] trace done: %d samples\n"}


class LogRecord(unittest.TestCase):
    """The LOG record: binary K2LOG entries + the version's id map -> 't=+<ms>ms message' lines."""

    def decode(self, db=DB, source="git:fcb3128", warning=None):
        with mock.patch.object(q, "logdb_for", return_value=(db, source, warning)):
            return q.decode_texts(FIX["log"]["parts"])[0]["record"]

    def test_head(self):
        r = self.decode()
        self.assertEqual((r["git"], r["older_dropped"], r["base_ms"], r["now_ms"]), ("fcb3128-dirty", True, 120, 4321))
        self.assertEqual(r["entries"], 4)
        self.assertEqual(r["logdb"], "git:fcb3128")
        self.assertNotIn("warning", r)

    def test_lines_have_the_times_and_the_rendered_messages(self):
        # the oldest of five entries was dropped: base 120 is its time; the rest are at 125, 900, 905 and 1500 ms
        self.assertEqual(self.decode()["lines"], [
            "t=+125ms [BBSTORE] entries=3 used=672 free=5376",
            "t=+900ms [BOOT] trace done: 1234 samples",
            "t=+905ms [ADC] raw sweep failed",
            "t=+1500ms [BOOT] a b%",
        ])

    def test_unknown_id_keeps_the_raw_bytes(self):
        r = self.decode({5: DB[5], 8: DB[8]})
        self.assertEqual(r["lines"][-2], "t=+905ms msg#2 ?")
        self.assertTrue(r["lines"][-1].startswith("t=+1500ms msg#6 ? "))

    def test_warning_is_carried(self):
        self.assertEqual(self.decode(warning="the build was dirty")["warning"], "the build was dirty")

    def test_signed_and_wide_values(self):
        for v in (0, -1, 1, -2147483648, 2147483647):
            zz = ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF
            vb = bytearray()
            while zz >= 0x80:
                vb.append(zz & 0x7F | 0x80)
                zz >>= 7
            vb.append(zz)
            body = bytes([1, 3]) + bytes(vb)               # dt 1, id 3
            out = q.decode_log_entries(bytes([len(body)]) + body, {3: "v=%d"}, 10)
            self.assertEqual(out, [(11, 3, f"v={v}")])

    def test_clock_wrap(self):
        body = bytes([0x20, 1])
        out = q.decode_log_entries(bytes([2]) + body, {1: "x"}, 0xFFFFFFF0)
        self.assertEqual(out[0][0], 0x10)

    def test_truncated_entry_is_an_error(self):
        with self.assertRaises(q.QrError):
            q.decode_log_entries(bytes([5, 1, 2]), DB, 0)
        res, code = q.decode_texts(q.encode(6, 1, bytes([2]) + b"gg" + bytes([0]) + bytes(8) + bytes([9, 1])))
        self.assertEqual(code, 0)
        self.assertIn("decode_error", res)

    def test_arg_kinds_match_the_firmware_signature(self):
        self.assertEqual(q.arg_kinds("a %d %5u %02X %x %c %s %% %08x"), ["d", "u", "u", "u", "u", "s", "u"])
        self.assertEqual(q.arg_kinds("no args\n"), [])

    def test_logdb_for_reads_the_commit_then_falls_back(self):
        ok = mock.Mock(returncode=0, stdout=json.dumps({"messages": {"7": "seven"}}))
        with mock.patch.object(q.subprocess, "run", return_value=ok) as run:
            db, src, warn = q.logdb_for("abc1234")
        self.assertEqual((db, src, warn), ({7: "seven"}, "git:abc1234", None))
        self.assertIn("abc1234:tools/logdb.json", run.call_args[0][0])
        with mock.patch.object(q.subprocess, "run", return_value=mock.Mock(returncode=128, stdout="")):
            db, src, warn = q.logdb_for("v1.0-3-gabc1234")
        self.assertEqual(src, "working-tree")
        self.assertIn("abc1234 is not in this repository", warn)
        with mock.patch.object(q.subprocess, "run") as run:    # a dirty build is never looked up
            _, src, warn = q.logdb_for("abc1234-dirty")
        run.assert_not_called()
        self.assertEqual(src, "working-tree")
        self.assertIn("dirty", warn)
        _, src, warn = q.logdb_for("unknown")
        self.assertIn("no commit hash", warn)

    def test_cli_lines(self):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), mock.patch.object(q, "logdb_for", return_value=(DB, "working-tree", "dirty build")):
            code = q.main(["decode", "--lines", *FIX["log"]["parts"]])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue().splitlines()[0], "t=+125ms [BBSTORE] entries=3 used=672 free=5376")
        self.assertIn("warning: dirty build", err.getvalue())


class Damage(unittest.TestCase):
    def test_bad_frame_crc(self):
        t = FIX["identity"]["parts"][0]
        bad = t[:-1] + ("0" if t[-1] != "0" else "1")
        with self.assertRaises(q.QrError):
            q.parse_part(bad)

    def test_not_a_k2_text(self):
        for t in ("hello", "K2:", "K2:XX1:AAAA", "K2:ID1"):
            with self.assertRaises(q.QrError, msg=t):
                q.parse_part(t)

    def test_incomplete_set_names_the_missing_part(self):
        res, code = q.decode_texts(FIX["emk_rows"]["parts"][1:])
        self.assertEqual(code, 1)
        self.assertFalse(res["complete"])
        self.assertEqual(res["missing"], [1])
        self.assertNotIn("record", res)

    def test_parts_of_two_records_do_not_mix(self):
        other = bytes([FIX["emk_rows"]["record"][0] ^ 1]) + FIX["emk_rows"]["record"][1:]
        with self.assertRaises(q.QrError):
            q.assemble([FIX["emk_rows"]["parts"][0], q.encode(2, 1, other)[1]])

    def test_duplicate_part_is_fine(self):
        p = FIX["emk_rows"]["parts"]
        _, rec = q.assemble([p[0], p[0], p[1]])
        self.assertEqual(rec, FIX["emk_rows"]["record"])

    def test_record_limits(self):
        with self.assertRaises(q.QrError):
            q.encode(2, 1, b"")
        with self.assertRaises(q.QrError):
            q.encode(2, 1, bytes(q.MAX_RECORD + 1))
        self.assertEqual(len(q.encode(2, 1, bytes(q.MAX_RECORD))), 4)


class Cli(unittest.TestCase):
    def run_main(self, argv, stdin=None):
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err), mock.patch("sys.stdin", io.StringIO(stdin or "")):
            code = q.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_decode_json(self):
        code, out, _ = self.run_main(["decode", *FIX["emk_rows"]["parts"]])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["type"], "EMK")

    def test_decode_from_stdin(self):
        code, out, _ = self.run_main(["decode", "-"], "\n".join(FIX["pdcaps_spr"]["parts"]) + "\n")
        self.assertEqual((code, json.loads(out)["type"]), (0, "PDC"))

    def test_encode_round_trip(self):
        code, out, _ = self.run_main(["encode", "--type", "EMK", "--hex", FIX["emk_rows"]["record"].hex()])
        self.assertEqual(code, 0)
        self.assertEqual(out.splitlines(), FIX["emk_rows"]["parts"])   # a space is a base45 digit: split on lines only

    def test_bad_input_exit_code(self):
        code, out, _ = self.run_main(["decode", "nonsense"])
        self.assertEqual(code, 2)
        self.assertIn("error", json.loads(out))

    def test_decode_image_skips_without_a_reader(self):
        with mock.patch.object(q, "scan_images", return_value=None):
            code, out, err = self.run_main(["decode-image", "x.png"])
        self.assertEqual(code, 3)
        self.assertIn("zbarimg", err)

    def test_decode_image_texts(self):
        with mock.patch.object(q, "scan_images", return_value=FIX["identity"]["parts"]):
            code, out, _ = self.run_main(["decode-image", "x.png"])
        self.assertEqual((code, json.loads(out)["record"]["proto"]), (0, 1))


if __name__ == "__main__":
    unittest.main()
