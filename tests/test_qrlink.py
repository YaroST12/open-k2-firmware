# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The results link a K2 shows (src/ui/qrshare_link.cpp, docs/QR_SHARE.md "Link form") read back in Python.

tests/fixtures/qrlink.tsv is written by tests/test_qrshare_link.cpp from the firmware's own encoder: record hex, the link text
and the card as the firmware's JSON renderer writes it. Here the link is decoded with tools/k2_qr.py and tools/report_card.py,
an implementation independent of the firmware, and must give the same record and the same card. The web page's decoder
(docs/site/r/card.js) has the same check in tests/test_cardpage.mjs."""
import base64
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
# the link's kind letter: lower case is the record as built, upper case (and the card's z) an LZ4 block of it
KINDS = {"c": "card", "z": "card", "e": "emarker", "E": "emarker", "p": "pdcaps", "P": "pdcaps", "i": "identity",
         "I": "identity", "b": "boot", "B": "boot"}
DECODERS = {"emarker": q.decode_emarker, "pdcaps": q.decode_pdcaps, "identity": q.decode_identity, "boot": q.decode_boot}
QR_V11_BYTES = 321   # byte-mode capacity of a version 11 code at level L


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
    """The link -> (kind letter, record), the way the page reads it."""
    m = re.fullmatch(r"(\d)([a-zA-Z])\.([A-Za-z0-9_-]*)", text.split("#", 1)[1])
    assert m and m.group(1) == "1", text
    raw = m.group(3)
    body = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))
    letter = m.group(2)
    return letter, (q.lz4_decompress(body) if letter in "zEPIB" else body)


def decoded_kinds(golden):
    """Every golden that is not a card, decoded by tools/k2_qr.py: what the page's decoders must also give."""
    out = {}
    for name, g in golden.items():
        letter, rec = read_link(g["link"])
        if KINDS[letter] != "card":
            out[name] = {"kind": KINDS[letter], "data": DECODERS[KINDS[letter]](rec)}
    return out


def normalise(card):
    """Two differences between the firmware's JSON and tools/report_card.py, neither in the record: the firmware writes an EPR
    level's key in millivolts ("28000V") where the tool says "28V", and it leaves out cable.seq when it is 0 where the tool
    writes it. The web page follows the tool's EPR keys and the firmware's seq rule."""
    card = json.loads(json.dumps(card))
    if card.get("cable", {}).get("seq") == 0:
        del card["cable"]["seq"]
    levels = card.get("epr_levels")
    if levels:
        card["epr_levels"] = {(k[:2] + "V" if re.fullmatch(r"\d{5}V", k) else k): v for k, v in levels.items()}
    return card


class QrLink(unittest.TestCase):
    def test_goldens(self):
        golden = load()
        self.assertGreaterEqual(len(golden), 4)
        for name, g in golden.items():
            with self.subTest(name):
                self.assertTrue(g["link"].startswith(BASE))
                self.assertLessEqual(len(g["link"]), QR_V11_BYTES)   # QR version 11, 61 modules
                letter, rec = read_link(g["link"])
                self.assertEqual(letter.isupper() or letter == "z", g["packed"])
                if g["json"] is None:
                    continue
                self.assertEqual(KINDS[letter], "card")
                self.assertEqual(len(rec), 196)   # v6: 190 byte payload + 6 framing
                card = q.decode_card(rec)
                self.assertTrue(card.pop("_store_crc_ok"))
                self.assertEqual(normalise(card), normalise(g["json"]))

    def test_v5_cards_still_decode(self):
        """tests/fixtures/qrlink_v5.tsv is frozen: links made before the UFCS modes (184 byte records, format 5). They decode,
        the CRC holds, and the card has no "ufcs" key (its UFCS amps are unknown)."""
        golden = load("qrlink_v5.tsv")
        self.assertEqual(len(golden), 5)
        for name, g in golden.items():
            with self.subTest(name):
                rec = read_link(g["link"])[1]
                self.assertEqual(len(rec), 184)
                card = q.decode_card(rec)
                self.assertTrue(card.pop("_store_crc_ok"))
                self.assertNotIn("ufcs", card)
                self.assertEqual(normalise(card), normalise(g["json"]))

    def test_ufcs_modes_golden_carries_the_current(self):
        card = q.decode_card(read_link(load()["ufcs_modes"]["link"])[1])
        self.assertEqual(card["ufcs"], [{"lo_mv": 3300, "hi_mv": 5500, "max_ma": 5000},
                                        {"lo_mv": 5000, "hi_mv": 11000, "max_ma": 3000},
                                        {"lo_mv": 5000, "hi_mv": 20000, "max_ma": 3000}])

    def test_worst_card_link_fits_one_code(self):
        longest = max(len(g["link"]) for g in load().values())
        self.assertEqual(longest, 312)
        self.assertLessEqual(longest, QR_V11_BYTES)

    def test_every_kind_has_a_golden(self):
        kinds = {KINDS[read_link(g["link"])[0]] for g in load().values()}
        self.assertEqual(kinds, {"card", "emarker", "pdcaps", "identity", "boot"})

    def test_packed_and_plain_are_one_card(self):
        golden = load()
        for name in ("fh22", "allpass"):
            with self.subTest(name):
                self.assertEqual(read_link(golden[name]["link"])[1], read_link(golden[name + "_plain"]["link"])[1])

    def test_packed_and_plain_are_one_record(self):
        golden = load()
        for a, b in (("pdcaps_epr", "pdcaps_epr_lz"), ("identity", "identity_lz"), ("boot", "boot_lz")):
            with self.subTest(a):
                self.assertEqual(read_link(golden[a]["link"])[1], read_link(golden[b]["link"])[1])

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
        card = q.decode_card(read_link(load()["fh22"]["link"])[1])
        self.assertEqual(card["groups"]["PD"]["reason"], "capped below target")
        r = card["ranges"][0]
        self.assertEqual((r["alg"], r["max_mv"], r["hi_mv"]), ("PPS (PD3.x)", 19400, 21000))


if __name__ == "__main__":
    unittest.main()
