# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The results link a K2 shows (src/ui/qrshare_link.cpp, docs/QR_SHARE.md "Link form") read back in Python.

tests/fixtures/qrlink.tsv is written by tests/test_qrshare_link.cpp from the firmware's own encoder: record hex, the link text
and the card as the firmware's JSON renderer writes it. Here the link is decoded with tools/k2_qr.py (versions '1' and '2', every
kind, 't' too), an implementation independent of the firmware, and must give the same record and the same card. The web page's
decoder (docs/site/r/card.js) has the same check in tests/test_cardpage.mjs."""
import json
import os
import re
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_qr as q  # noqa: E402

BASE = "https://yarost12.github.io/open-k2-firmware/r/#"


DECODED = os.path.join(HERE, "fixtures", "qrlink_decoded.json")
# the link's kind letter: lower case is the record as built (or its schema v2 record on a version '2' link), upper case (and
# the card's z) an LZ4 block of it
KINDS = {"c": "card", "z": "card", "e": "emarker", "E": "emarker", "p": "pdcaps", "P": "pdcaps", "i": "identity",
         "I": "identity", "b": "boot", "B": "boot", "t": "text"}
QR_V11_BYTES = 321   # byte-mode capacity of a version 11 code at level L
QR_V12_BYTES = 370   # version 12 (65 modules)
QR_V13_BYTES = 425   # version 13 (69 modules): a format-10 plain card link is 392 characters, one code at version 13


def load(name="qrlink.tsv"):
    out = {}
    with open(os.path.join(HERE, "fixtures", name)) as f:
        for ln in f:
            if ln.startswith("#") or not ln.strip():
                continue
            name, packed, rec, link, js = ln.rstrip("\n").split("\t")
            out[name] = {"packed": packed == "1", "record": bytes.fromhex(rec), "link": link,
                         "json": None if js == "-" else json.loads(js)}
    return out


def read_link(text):
    """The link -> (kind letter, the record it carries, inflated when the letter says an LZ4 block)."""
    d = q.decode_link(text)
    rec = bytes.fromhex(d["record_hex"])
    if d.get("packed"):
        rec = q.lz4_decompress(rec)
    return d["kind"], rec, d


def decoded_kinds(golden):
    """Every golden that is not a card, decoded by tools/k2_qr.py: what the page's decoders must also give."""
    out = {}
    for name, g in golden.items():
        d = q.decode_link(g["link"])
        if d["record_kind"] != "card":
            out[name] = {"kind": d["record_kind"], "version": d["version"], "data": d["record"]}
    return out


def normalise(card):
    """Differences between the firmware's JSON and tools/report_card.py, none in the record: the firmware writes an EPR
    level's key in millivolts ("28000V") where the tool says "28V", and it leaves out cable.seq when it is 0 where the tool
    writes it. The web page follows the tool's EPR keys and the firmware's seq rule. The "qc" brand label is dropped while
    the no-brand-without-VID rule (row B item 5) is landed in the firmware renderer but not yet in tools/report_card.py."""
    card = json.loads(json.dumps(card))
    card.pop("qc", None)
    if card.get("cable", {}).get("seq") == 0:
        del card["cable"]["seq"]
    levels = card.get("epr_levels")
    if levels:
        card["epr_levels"] = {(k[:2] + "V" if re.fullmatch(r"\d{5}V", k) else k): v for k, v in levels.items()}
    return card


class QcLabelRule(unittest.TestCase):
    """Row B item 5: no Samsung SFC label without the brand's own VID (report_card.hpp kSamsungVid),
    in the goldens and in tools/report_card.py's own label."""

    def test_goldens_carry_no_sfc_label_without_the_samsung_vid(self):
        for name, g in load().items():
            if g["json"] is None:
                continue
            qc = g["json"].get("qc", "")
            if "SFC" in qc:
                self.assertEqual(g["json"].get("vid"), 0x04E8, name)

    def test_report_card_qc_label_needs_the_samsung_vid(self):
        import report_card
        pps = [{"alg": "PPS (PD3.x)", "lo_mv": 3300, "hi_mv": 21000, "max_ma": 3000}]
        self.assertEqual(report_card.qc_label(0x04E8, 100, pps), "Samsung SFC 3.0")
        self.assertEqual(report_card.qc_label(0, 100, pps), "")
        self.assertEqual(report_card.qc_label(0x2717, 65, pps), "")
        self.assertEqual(report_card.qc_label(0x05C6, 65, pps), "QC4+")


class QrLink(unittest.TestCase):
    def test_goldens(self):
        golden = load()
        self.assertGreaterEqual(len(golden), 4)
        for name, g in golden.items():
            with self.subTest(name):
                self.assertTrue(g["link"].startswith(BASE))
                self.assertLessEqual(len(g["link"]), QR_V13_BYTES)   # a format-10 plain card link is 392 characters: one code at version 13, 69 modules
                letter, rec, d = read_link(g["link"])
                self.assertEqual(letter.isupper() or letter == "z", g["packed"])
                self.assertEqual(KINDS[letter], d["record_kind"])
                if g["json"] is None:
                    continue
                self.assertEqual(d["record_kind"], "card")
                card = d["record"]
                self.assertTrue(card.pop("_store_crc_ok"))          # the v2 record carries the v1 CRC-16
                self.assertEqual(normalise(card), normalise(g["json"]))

    def test_v2_rows_carry_the_v2_record(self):
        """The card and cable rows are version '2' links: the record is the bit-packed v2 record (schema byte 2), not the v1
        one, and it decodes to the same card as the pinned version '1' row of the same card."""
        golden = load()
        for name in ("fh22", "allpass", "fh22_full", "ufcs_modes", "fh22_repeat", "emk_plain", "emk_clone", "emk_rows2",
                     "emk_rows4", "emk_rows8", "emk_req"):
            with self.subTest(name):
                d = q.decode_link(golden[name]["link"])
                self.assertEqual(d["version"], 2)
                self.assertEqual(d["kind"], "c" if d["record_kind"] == "card" else "e")
                self.assertFalse(golden[name]["packed"])
                self.assertEqual(golden[name]["record"][0], 2)      # the schema byte
                self.assertEqual(bytes.fromhex(d["record_hex"]), golden[name]["record"])
        # the same card through the v1 and the v2 link: one JSON
        for v2name, v1name in (("fh22", "fh22_plain"), ("fh22", "fh22_lz")):
            with self.subTest((v2name, v1name)):
                a = q.decode_link(golden[v2name]["link"])["record"]
                b = q.decode_link(golden[v1name]["link"])["record"]
                a.pop("_store_crc_ok"), b.pop("_store_crc_ok")
                self.assertEqual(normalise(a), normalise(b))

    def test_v5_cards_still_decode(self):
        """tests/fixtures/qrlink_v5.tsv is frozen: links made before the UFCS modes (184 byte records, format 5, version '1').
        They decode, the CRC holds, and the card has no "ufcs" key (its UFCS amps are unknown)."""
        golden = load("qrlink_v5.tsv")
        self.assertEqual(len(golden), 5)
        for name, g in golden.items():
            with self.subTest(name):
                letter, rec, d = read_link(g["link"])
                self.assertEqual(d["version"], 1)
                self.assertEqual(len(rec), 184)
                card = q.decode_card(rec)
                self.assertTrue(card.pop("_store_crc_ok"))
                self.assertNotIn("ufcs", card)
                self.assertEqual(normalise(card), normalise(g["json"]))

    def test_ufcs_modes_golden_carries_the_current(self):
        card = q.decode_link(load()["ufcs_modes"]["link"])["record"]
        self.assertEqual(card["ufcs"], [{"lo_mv": 3300, "hi_mv": 5500, "max_ma": 5000},
                                        {"lo_mv": 5000, "hi_mv": 11000, "max_ma": 3000},
                                        {"lo_mv": 5000, "hi_mv": 20000, "max_ma": 3000}])

    def test_worst_card_link_fits_one_code(self):
        golden = load()
        longest = max(len(g["link"]) for g in golden.values())
        self.assertEqual(longest, 392)   # the pinned version '1' plain row: the 256 byte record is 342 characters after the 50 character head
        self.assertLessEqual(longest, QR_V13_BYTES)
        # the links the Share view now shows (version '2' card rows) stay well under the cap: at most 85 % of the 321 characters
        v2_cards = [len(g["link"]) for n, g in golden.items()
                    if q.decode_link(g["link"])["version"] == 2 and q.decode_link(g["link"])["record_kind"] == "card"]
        self.assertTrue(v2_cards)
        self.assertLessEqual(max(v2_cards) * 100, 85 * QR_V11_BYTES)

    def test_the_8_row_cable_has_a_v2_link(self):
        """The cable with all eight quirk rows is 510 B raw (no version '1' link): its version '2' link is 302 characters."""
        d = q.decode_link(load()["emk_rows8"]["link"])
        self.assertEqual((d["version"], d["kind"], d["record_kind"]), (2, "e", "emarker"))
        self.assertEqual(len(d["link"] if "link" in d else load()["emk_rows8"]["link"]), 302)
        self.assertEqual(len(bytes.fromhex(d["record_hex"])), 189)
        self.assertLessEqual(len(load()["emk_rows8"]["link"]), QR_V11_BYTES)
        self.assertIn("quirks", d["record"])
        self.assertEqual(len(d["record"]["quirks"]["rows"]), 8)

    def test_kind_t_is_the_text_record(self):
        d = q.decode_link(load()["qrtest"]["link"])
        self.assertEqual((d["version"], d["kind"], d["record_kind"]), (1, "t", "text"))
        self.assertEqual(d["text"], "QR scan test v11 3px")     # the ASCII text up to the first NUL
        self.assertEqual(bytes.fromhex(d["record_hex"]), load()["qrtest"]["record"])

    def test_every_kind_has_a_golden(self):
        kinds = {q.decode_link(g["link"])["record_kind"] for g in load().values()}
        self.assertEqual(kinds, {"card", "emarker", "pdcaps", "identity", "boot", "text"})

    def test_packed_and_plain_are_one_card(self):
        golden = load()
        for a, b in (("fh22", "fh22_plain"), ("fh22", "fh22_lz")):
            with self.subTest((a, b)):
                ca = q.decode_link(golden[a]["link"])["record"]
                cb = q.decode_link(golden[b]["link"])["record"]
                ca.pop("_store_crc_ok"), cb.pop("_store_crc_ok")
                self.assertEqual(normalise(ca), normalise(cb))

    def test_packed_and_plain_are_one_record(self):
        golden = load()
        for a, b in (("pdcaps_epr", "pdcaps_epr_lz"), ("identity", "identity_lz"), ("boot", "boot_lz"),
                     ("emk_rows4", "emk_rows4_lz")):
            with self.subTest(a):
                # the v1 plain/packed pair carries the same bytes; the v2 row decodes to the same record as its v1 twin
                self.assertEqual(q.decode_link(golden[a]["link"])["record"], q.decode_link(golden[b]["link"])["record"])

    def test_decoded_kinds_are_pinned(self):
        """tests/fixtures/qrlink_decoded.json is what these decoders give for the non-card goldens; the web page's decoders
        (records.js) must give the same, and tests/test_cardpage.mjs checks it. K2_QRLINK_WRITE=1 rewrites the file."""
        text = json.dumps(decoded_kinds(load()), indent=1, sort_keys=True) + "\n"
        if os.environ.get("K2_QRLINK_WRITE"):
            with open(DECODED, "w") as f:
                f.write(text)
        with open(DECODED) as f:
            self.assertEqual(json.loads(text), json.load(f))

    def test_pdcaps_golden_reads(self):
        d = decoded_kinds(load())["pdcaps_epr"]["data"]
        self.assertTrue(d["epr"])
        self.assertEqual(d["pdos"][0]["kind"], "fixed")

    def test_capped_range_is_in_the_record(self):
        card = q.decode_link(load()["fh22"]["link"])["record"]
        self.assertEqual(card["groups"]["PD"]["reason"], "capped below target")
        r = card["ranges"][0]
        self.assertEqual((r["alg"], r["max_mv"], r["hi_mv"]), ("PPS (PD3.x)", 19400, 21000))

    def test_bad_links_are_refused(self):
        for bad in (BASE + "3c.AAAA", BASE + "2p.AAAA", BASE + "1x.AAAA", BASE + "1c", "https://example.com/#1c.AAAA"):
            with self.subTest(bad):
                with self.assertRaises(q.QrError):
                    q.decode_link(bad)

    def test_decode_link_cli(self):
        import io
        from contextlib import redirect_stdout
        out = io.StringIO()
        with redirect_stdout(out):
            code = q.main(["decode-link", load()["qrtest"]["link"]])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out.getvalue())[0]["text"], "QR scan test v11 3px")


if __name__ == "__main__":
    unittest.main()
