# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/k2_qr.py against the golden vectors the firmware encoder produced (tests/fixtures/qrshare.tsv,
checked by tests/test_qrshare_records.cpp): the Python side decodes every fixture, re-encodes the record to the same
part texts byte for byte (an implementation independent of src/ui/qrshare.cpp), and reads the fields back."""
import base64
import io
import json
import os
import re
import sys
import unittest
from contextlib import redirect_stdout, redirect_stderr
from unittest import mock

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_qr as q  # noqa: E402
import report_card  # noqa: E402
import loop_probe_rec as lp  # noqa: E402


def load(path=os.path.join(HERE, "fixtures", "qrshare.tsv")):
    out = {}
    with open(path) as f:
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
                                    "log", "loop", "battery", "emk_rows_lz", "card_lz", "emk_rows_v2", "card_v2",
                                    "card_deep_v2", "card_mi_v2"})

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

    def test_v2_carries_the_run_level_and_keeps_the_crc(self):
        # The level (v8 spare byte 197: 1 Light, 2 Deep) rides in the v2 record as one trailing byte, written only when it is
        # not Medium; a Medium card packs exactly as before, so older links keep their bytes.
        import qr_corpus
        v1 = bytearray(FIX["card"]["record"])
        self.assertGreaterEqual(v1[4 + 33], 8)
        medium = qr_corpus.pack_card_v2(qr_corpus.parse_card(bytes(v1)))
        for level in (1, 2):
            with self.subTest(level=level):
                rec = bytearray(v1)
                rec[4 + 197] = level
                crc = qr_corpus.crc16_ccitt(bytes(rec[:-2]))
                rec[-2], rec[-1] = crc & 0xFF, crc >> 8
                v2 = qr_corpus.pack_card_v2(qr_corpus.parse_card(bytes(rec)))
                self.assertEqual(len(v2), len(medium) + 1)
                back = q.expand_card_v2(v2)
                self.assertEqual(back, bytes(rec))
                d = q.decode_card(back)
                self.assertTrue(d["_store_crc_ok"])
                self.assertEqual(d["level"], ["medium", "light", "deep"][level])
        self.assertEqual(q.expand_card_v2(medium), bytes(v1))   # Medium: no byte, still reads back whole

    def test_xiaomi_card_golden(self):
        # Format 12: a Medium card (the 20K bank) whose block holds only the Xiaomi section; the level byte 0 is written so the block
        # has its place. The chain verified and PPS 5-11V 3A became 5A, tested at 11 V 5 A ("Mi 55W").
        import charger_summary
        res, code = q.decode_texts(FIX["card_mi_v2"]["parts"])
        rec = res["record"]
        self.assertTrue(rec["_store_crc_ok"])
        self.assertNotIn("level", rec)
        x = rec["deep"]["xiaomi"]
        self.assertEqual((x["chain"], x["chain_text"], x["done"]), (20, "done, verified", True))
        self.assertEqual([(m["label"], m["before"], m["after"], m["mv"], m["ma"], m["verdict"]) for m in x["modes"]],
                         [("Mi 55W", "PPS 5-11V 3A", "PPS 5-11V 5A", 11000, 5000, "pass")])
        self.assertEqual(set(rec["deep"]), {"xiaomi"})   # no Deep parts on a Medium card
        text = charger_summary.summarize_card(rec)
        self.assertIn("- Xiaomi: auth done, verified\n  - Mi 55W: PPS 5-11V 3A → PPS 5-11V 5A ✓", text)

    def test_format_11_deep_link_still_decodes(self):
        # tests/fixtures/qrshare_v11.tsv: a Deep card link a format 11 firmware wrote. The reader expands it to format 12 and falls
        # back to the format 11 CRC; its block reads as before, with no Xiaomi section.
        old = load(os.path.join(HERE, "fixtures", "qrshare_v11.tsv"))["card_deep_v11"]
        res, code = q.decode_texts(old["parts"])
        rec = res["record"]
        self.assertTrue(rec["_store_crc_ok"])
        self.assertEqual(rec["level"], "deep")
        self.assertNotIn("xiaomi", rec["deep"])
        with open(os.path.join(HERE, "fixtures", "deep_decoded.json")) as f:
            self.assertEqual(rec["deep"], json.load(f))   # the same Deep block the format 12 golden carries

    def test_format_12_links_still_decode(self):
        # tests/fixtures/qrshare_v12.tsv: the Deep and Xiaomi card links a format 12 firmware wrote. The reader expands them to format 13
        # and falls back to the format 12 CRC; the Deep block reads as before (form 27: no range words), the Xiaomi section too.
        old = load(os.path.join(HERE, "fixtures", "qrshare_v12.tsv"))
        rec = q.decode_texts(old["card_deep_v12"]["parts"])[0]["record"]
        self.assertTrue(rec["_store_crc_ok"])
        self.assertNotIn("ranges", rec["deep"]["spec"])
        with open(os.path.join(HERE, "fixtures", "deep_decoded.json")) as f:
            self.assertEqual(rec["deep"], json.load(f))
        mi = q.decode_texts(old["card_mi_v12"]["parts"])[0]["record"]
        self.assertTrue(mi["_store_crc_ok"])
        self.assertEqual(len(mi["deep"]["xiaomi"]["modes"]), 1)

    def test_v10_links_keep_their_crc_and_have_no_deep_block(self):
        # A format-10 firmware's link carries the CRC of its format-10 record; the reader writes format 11 and falls back to the
        # CRC with the byte before it. A flipped bit still fails.
        import qr_corpus
        v1 = bytearray(FIX["card"]["record"])
        v1[4 + 33] = 10
        crc = qr_corpus.crc16_ccitt(bytes(v1[:-2]))
        v1[-2], v1[-1] = crc & 0xFF, crc >> 8
        v2 = qr_corpus.pack_card_v2(qr_corpus.parse_card(bytes(v1)))
        d = q.decode_card_v2(v2)
        self.assertTrue(d["_store_crc_ok"])
        self.assertNotIn("deep", d)
        bad = bytearray(v2)
        bad[-1] ^= 0x01                      # the CRC itself
        self.assertFalse(q.decode_card_v2(bytes(bad))["_store_crc_ok"])

    def test_deep_card_golden(self):
        # The row tests/test_qrshare_records.cpp builds from a Deep run (PPS error (i % 9) * 10 - 20 mV over 128 points from
        # 3.3 V, an AVS sweep of a steady +35 mV): the v2 stream carries the block after the level byte, the CRC holds.
        res, code = q.decode_texts(FIX["card_deep_v2"]["parts"])
        self.assertEqual(code, 0)
        self.assertNotIn("decode_error", res)
        card = res["record"]
        self.assertTrue(card["_store_crc_ok"])
        self.assertEqual(card["level"], "deep")
        deep = card["deep"]
        self.assertEqual(deep["svooc"], {"end": "stopped", "asks": 2, "ceiling_ma": 5500, "vmax_mv": 11200, "stop_ms": 3300})
        spec = deep["spec"]
        self.assertEqual(spec["end"], "done")
        self.assertEqual(spec["tally"], {"pass": 1, "fail": 1, "skip": 1, "unverified": 1, "pending": 23})
        by = {c["key"]: c for c in spec["cells"]}
        self.assertEqual(by["accept_time"]["verdict"], "pass")
        self.assertEqual(by["accept_time"]["value"], 123400)        # 1234 steps of 100 us
        self.assertEqual(by["ps_rdy_time"]["verdict"], "fail")
        self.assertEqual(by["ps_rdy_time"]["fails"], 7)
        self.assertEqual(by["ps_rdy_time"]["at_text"], "PDO 5 to 9")
        self.assertEqual(by["src_start"]["skip"], "no AVS")
        self.assertEqual(by["ps_rdy_early"]["value_text"], "not seen")
        # the graphs: the PPS range, then the AVS one (range index 1 of the card)
        g0, g1 = deep["graphs"]
        self.assertEqual((g0["range"], g0["lo_mv"], g0["n"], g0["step_mv"]), (0, 3300, 128, 100))
        self.assertEqual(len(g0["buckets"]), 32)
        e = g0["unit_mv"]
        self.assertEqual(e, 4)
        for b, got in enumerate(g0["buckets"]):
            pts = [i % 9 * 10 - 20 for i in range(b * 128 // 32, (b + 1) * 128 // 32)]
            self.assertLessEqual(abs(got["mean_mv"] - sum(pts) / len(pts)), e / 2 + 1e-9, b)
            self.assertLessEqual(got["min_mv"], min(pts))               # the band always covers the data
            self.assertGreaterEqual(got["max_mv"], max(pts))
            self.assertLessEqual(got["max_mv"] - max(pts), e)           # and by less than one unit
        self.assertEqual((g1["range"], g1["lo_mv"], g1["n"]), (1, 15000, 131))
        for got in g1["buckets"]:
            self.assertLessEqual(abs(got["mean_mv"] - 35), g1["unit_mv"] / 2)
            self.assertLessEqual(got["min_mv"], 35)
            self.assertGreaterEqual(got["max_mv"], 35)

    def test_deep_decode_is_pinned_for_the_web_page(self):
        # tests/fixtures/deep_decoded.json is what decode_deep gives for the golden Deep card; docs/site/r/deep.js (the web viewer)
        # must give the same and tests/test_cardpage.mjs checks it. K2_QRLINK_WRITE=1 rewrites the file.
        deep = q.decode_texts(FIX["card_deep_v2"]["parts"])[0]["record"]["deep"]
        path = os.path.join(HERE, "fixtures", "deep_decoded.json")
        text = json.dumps(deep, indent=1, sort_keys=True) + "\n"
        if os.environ.get("K2_QRLINK_WRITE"):
            with open(path, "w") as f:
                f.write(text)
        with open(path) as f:
            self.assertEqual(json.loads(text), json.load(f))
        # the same card as a version 2 link (what the K2's QR carries) reads the same through decode_link
        link = "https://yarost12.github.io/open-k2-firmware/r/#2c." + base64.urlsafe_b64encode(FIX["card_deep_v2"]["record"]).rstrip(b"=").decode()
        self.assertEqual(q.decode_link(link)["record"]["deep"], deep)

    def test_deep_block_is_only_read_after_the_level_byte(self):
        # The golden's block is the tail of the stream: cutting it leaves the card the older readers see (CRC unchanged).
        rec = FIX["card_deep_v2"]["record"]
        full_rec, blob = q.expand_card_v2_deep(rec)
        self.assertIsNotNone(blob)
        self.assertEqual(len(blob), 2 + (int.from_bytes(blob[:2], "little") + 7) // 8)
        plain = q.expand_card_v2_deep(FIX["card_v2"]["record"])
        self.assertIsNone(plain[1])
        self.assertEqual(q.expand_card_v2(rec), full_rec)

    def test_v2_card_json_matches_the_v1_card(self):
        a = q.decode_texts(FIX["card_v2"]["parts"])[0]["record"]
        b = q.decode_texts(FIX["card"]["parts"])[0]["record"]
        self.assertTrue(a.pop("_store_crc_ok"))
        self.assertTrue(b.pop("_store_crc_ok"))
        self.assertEqual(a, b)
        self.assertEqual(a["name"], "Desk 65W")


ROOT = os.path.join(HERE, "..")


def _switch_strings(path, marker):
    """The return strings of the switch in the function that starts with `marker`, in case order."""
    with open(os.path.join(ROOT, path)) as f:
        src = f.read()
    body = src[src.index(marker):]
    body = body[:body.index("\n}\n")]
    return re.findall(r'case [\w:]+: return "([^"]*)";', body)


def _blob(bits):
    """Bits (0/1 list) -> the Deep blob: u16 LE bit count, then the bytes, LSB first."""
    data = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        data[i >> 3] |= b << (i & 7)
    return bytes([len(bits) & 0xFF, len(bits) >> 8]) + bytes(data)


def _bits(value, width):
    return [(value >> i) & 1 for i in range(width)]


class DeepBlob(unittest.TestCase):
    """report_card.decode_deep(): the Deep block's own reader (the firmware's layout is in include/k2/chargertest/report_card_deep.hpp;
    the golden card above is the cross check against the firmware's builder)."""

    def test_svooc_tables_match_the_headers(self):
        # decode_deep names the ceiling stage from these lists; the headers own the order
        self.assertEqual(_switch_strings("include/k2/chargertest/svooc_ceiling.hpp", "constexpr const char* abort_word"),
                         report_card.SVOOC_ABORTS)
        self.assertEqual(_switch_strings("include/k2/fastcharge/svooc_run.hpp", "constexpr const char* name(Refusal r)"),
                         report_card.SVOOC_REFUSALS)
        with open(os.path.join(ROOT, "include/k2/chargertest/svooc_ceiling.hpp")) as f:
            enum = re.search(r"enum class End : std::uint8_t \{([^}]*)\}", f.read()).group(1)
        self.assertEqual([x.strip() for x in enum.split(",")],
                         ["None", "Refused", "NoAdapter", "Aborted", "Cap", "Judged", "Cancelled", "Stopped"])
        self.assertEqual(len(report_card.SVOOC_ENDS), 8)

    def test_svooc_only_blob_with_a_gap_in_the_sweep(self):
        # header: no spec, ceiling stage, sweep on range 1 only; the stage ended Aborted (3) because of "current" (2)
        bits = _bits(0, 1) + _bits(1, 1) + _bits(0b0010, 4)
        bits += _bits(3, 3) + _bits(2, 4) + _bits(1, 3) + _bits(109, 8) + _bits(112, 9) + _bits(33, 10)
        # 3 points from 5.0 V (lo 50 dV), scale 1 (E = 2 mV): bucket means +3, empty, -2 units; spreads 1/2, 0/0, 3/0
        bits += _bits(50, 9) + _bits(3, 10) + _bits(1, 3)
        bits += _bits(3, 8) + _bits(1, 4) + _bits(2, 4)
        bits += _bits(0x80, 8) + _bits(0, 4) + _bits(0, 4)
        bits += _bits(-2 & 0xFF, 8) + _bits(3, 4) + _bits(0, 4)
        d = report_card.decode_deep(_blob(bits), [{"alg": "x"}, {"alg": "Pps"}])
        self.assertNotIn("spec", d)
        self.assertEqual(d["svooc"], {"end": "aborted", "why": "current", "asks": 1, "ceiling_ma": 10900, "vmax_mv": 11200,
                                      "stop_ms": 3300})
        (g,) = d["graphs"]
        self.assertEqual((g["range"], g["lo_mv"], g["n"], g["unit_mv"], g["step_mv"]), (1, 5000, 3, 2, 100))
        self.assertEqual(g["buckets"][0], {"mean_mv": 6, "min_mv": 4, "max_mv": 10})
        self.assertIsNone(g["buckets"][1])                                  # nothing recorded there
        self.assertEqual(g["buckets"][2], {"mean_mv": -4, "min_mv": -10, "max_mv": -4})

    def test_web_viewer_tables_match_the_firmware_tables(self):
        # docs/site/r/deep.js carries the same tables as pdspec_blob.py (parsed from the firmware headers) and report_card.py
        import shutil
        import subprocess
        import pdspec_blob as pb
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        js = os.path.join(ROOT, "docs", "site", "r", "deep.js")
        out = subprocess.run([node, "-e", "process.stdout.write(JSON.stringify(require(process.argv[1]).tables))", js],
                             capture_output=True, text=True, timeout=30, check=True).stdout
        t = json.loads(out)
        s, tb = pb.section_tables(), pb.load_tables()
        keys = [d["key"] for d in tb["checks"]]
        ix = keys.index
        cells = []
        for d in s["cells"]:
            k = ix(d["key"])
            at = 0 if k < ix("pps_monotonic") else \
                1 if k < ix("pps_keepalive") or ix("pps_band") <= k <= ix("avs_transient") or k >= ix("pps_track") else 2
            cells.append([d["key"], d["label"], d["unit"], at])
        self.assertEqual(t["CELLS"], cells)
        self.assertEqual(t["ENDS"], s["ends"])
        self.assertEqual(t["VERDICTS"], tb["verdicts"])
        self.assertEqual(t["SKIPS"], tb["skips"])
        self.assertEqual(t["STOPS"], tb["stops"])
        self.assertEqual(t["STARTS"], tb["starts"])
        self.assertEqual(t["STEPS"], s["steps"])
        self.assertEqual(t["PLAN"], tb["steps"])
        self.assertEqual(t["SVOOC_ENDS"], report_card.SVOOC_ENDS)
        self.assertEqual(t["SVOOC_ABORTS"], report_card.SVOOC_ABORTS)
        self.assertEqual(t["SVOOC_REFUSALS"], report_card.SVOOC_REFUSALS)
        self.assertEqual(t["MAX_BITS"], report_card.DEEP_MAX_BITS)
        self.assertEqual(t["BUCKETS"], report_card.DEEP_BUCKETS)
        self.assertEqual(t["APDO"], {k: [None if d is None else [d["key"], d["label"]] for d in v] for k, v in s["apdo"].items()})
        self.assertEqual(t["RANGE_TONES"], s["range_tones"])
        self.assertEqual(t["GROUPS"], s["groups"])

    def test_form_28_ranges_decode_the_same_in_python_and_on_the_web_page(self):
        # tests/fixtures/deep_form28.hex is the block tests/test_report_card_deep.cpp builds (test_spec_ranges): PPS 5 fails its
        # overshoot (a shape word), AVS 7 passes, only the PPS block split (its range has a line); deep.js must read the same.
        import shutil
        import subprocess
        with open(os.path.join(HERE, "fixtures", "deep_form28.hex")) as f:
            blob = bytes.fromhex(f.read().strip())
        d = report_card.decode_deep(blob, [])
        spec = d["spec"]
        self.assertEqual(spec["split"], {"pps": True, "avs": False})
        self.assertEqual([(r["pos"], r["kind"], r["tone"], r["word"]) for r in spec["ranges"]],
                         [(5, "pps", "fail", "shape"), (7, "avs", "pass", "spec")])
        p, a = spec["ranges"]
        self.assertEqual(len(p["cells"]), 11)
        self.assertEqual(len(a["cells"]), 8)                                 # the AVS kind has no status, settle or status time
        by = {c["key"]: c["verdict"] for c in p["cells"]}
        self.assertEqual((by["pps_band"], by["pps_overshoot"], by["pps_status"]), ("pass", "fail", "pending"))
        self.assertEqual(len(d["xiaomi"]["modes"]), 1)
        node = shutil.which("node")
        if not node:
            self.skipTest("node is not installed")
        js = os.path.join(ROOT, "docs", "site", "r", "deep.js")
        out = subprocess.run([node, "-e", "process.stdout.write(JSON.stringify(require(process.argv[1]).decode(Buffer.from(process.argv[2], 'hex'), [])))",
                              js, blob.hex()], capture_output=True, text=True, timeout=30, check=True).stdout
        web = json.loads(out)
        web["xiaomi"].pop("modes"); d["xiaomi"].pop("modes")               # the reason names need the card's table (card.js)
        self.assertEqual(web, json.loads(json.dumps(d)))

    def test_a_bad_length_is_refused(self):
        good = _blob(_bits(0, 1) + _bits(1, 1) + _bits(0, 4) + _bits(0, 3 + 4 + 3 + 8 + 9 + 10))
        report_card.decode_deep(good, [])
        for bad in (b"", good[:-1], good + b"\0", bytes([0, 0, 0]), bytes([0xFF, 0x0F]) + good[2:]):
            with self.subTest(bad=bad.hex()), self.assertRaises(report_card.BadCard):
                report_card.decode_deep(bad, [])


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

    def test_battery(self):
        # tests/test_qrshare_records.cpp battery_record(): a same-port round trip, 10 W in for an hour, 8 W out for an hour
        self.assertEqual(len(FIX["battery"]["parts"]), 1)
        r = self.rec("battery")
        self.assertEqual((r["id"], r["mode"], r["wiring"], r["state"]), (7, "round-trip", "same-port", "done"))
        self.assertEqual([(p["kind"], p["end"]) for p in r["phases"]], [("charge", "auto"), ("discharge", "manual")])
        self.assertEqual(r["phases"][0]["time_s"], 3601)
        self.assertAlmostEqual(r["phases"][0]["in_uwh"] / 1e6, 10.0, delta=0.01)
        self.assertAlmostEqual(r["phases"][1]["out_uwh"] / 1e6, 8.0, delta=0.01)
        self.assertEqual(r["phases"][0]["out_uwh"], 0)
        self.assertIn(r["efficiency_permille"], range(798, 801))
        self.assertEqual(r["curve"][0], {"t_s": 0, "phase": 0, "mv": 5000, "ma": 2000})
        self.assertEqual(r["curve"][-1]["phase"], 1)
        self.assertEqual(r["curve"][-1]["ma"], -1600)


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


def emrec(vdos, spec=2):
    """A 66-byte e-marker record (include/k2/pd/emrec.hpp): name, PD spec rev, five VDOs, checksum."""
    r = bytearray(66)
    r[:4] = b"test"
    r[20] = spec
    for i, v in enumerate(vdos):
        r[21 + 4 * i:25 + 4 * i] = v.to_bytes(4, "little")
    r[65] = (sum(r[:65]) + 0xA5) & 0xFF
    return bytes(r)


class EmarkerSpecLayout(unittest.TestCase):
    """The cable VDO read as the firmware reads it (src/pd/pd.cpp, docs/PD_EMARKER_SPEC_REFERENCE.md): the deprecated 30 V /
    40 V codes and every SVDM 1.0 answer are 20 V; EPR needs bit 17 (passive v1.0, active v1.3 with VBUS through), 5 A, 50 V."""
    PASSIVE = 0x18002B01
    ACTIVE = 0x20002B01

    def test_deprecated_codes_read_20v(self):
        self.assertEqual(q.decode_emrec(emrec([0xFF00A041, self.PASSIVE, 0, 0, 0x80082A40]))["max_v"], 20)   # 30 V code
        self.assertEqual(q.decode_emrec(emrec([0xFF00A041, self.PASSIVE, 0, 0, 0x00000440]))["max_v"], 20)   # 40 V code

    def test_svdm1_has_no_50v(self):
        r = q.decode_emrec(emrec([0xFF008041, self.PASSIVE, 0, 0, 0x000A2640]))
        self.assertEqual((r["max_v"], r["epr"]), (20, False))

    def test_epr_bit(self):
        r = q.decode_emrec(emrec([0xFF00A041, self.PASSIVE, 0, 0, 0x000A2640]))
        self.assertEqual((r["max_v"], r["max_a"], r["epr"]), (50, 5, True))
        self.assertFalse(q.decode_emrec(emrec([0xFF00A041, self.PASSIVE, 0, 0, 0x00082640]))["epr"])   # no bit 17
        active = (3 << 21) | (1 << 17) | (3 << 9) | (2 << 5) | (1 << 4)
        self.assertTrue(q.decode_emrec(emrec([0xFF00A041, self.ACTIVE, 0, 0, active]))["epr"])
        self.assertFalse(q.decode_emrec(emrec([0xFF00A041, self.ACTIVE, 0, 0, active & ~(1 << 4)]))["epr"])   # VBUS not through
        self.assertFalse(q.decode_emrec(emrec([0xFF00A041, self.ACTIVE, 0, 0, (active & ~(7 << 21)) | (2 << 21)]))["epr"])   # v1.2


if __name__ == "__main__":
    unittest.main()
