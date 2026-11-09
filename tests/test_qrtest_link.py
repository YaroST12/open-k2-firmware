# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Golden for the QR scan-test rig (k2/ui/ui_qrtest.hpp, remote Cmd::QrTest op 1): the max-length kind-'t' link the
QrTestPage encodes at v13 3 px quiet 4 and at v11 3 px quiet 4, produced by the firmware's qrtest::build_link()
(captured from the host build), decoded here with tools/k2_qr.py decode-link exactly like a phone scan of the test
screen: kind 't' at version '1' is a plain ASCII text record read up to the first NUL."""
import os
import sys
import unittest

HERE = os.path.dirname(__file__)
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import k2_qr as q  # noqa: E402

# qrtest::build_link(13, 3): kBase + '1' + 't' + '.' + base64url(record), record = "QR scan test v13 3px"
# NUL-padded to max_record(13) = 281 bytes, so the link is exactly the 425 characters a version 13 code carries.
LINK_V13_3PX = "https://yarost12.github.io/open-k2-firmware/r/#1t.UVIgc2NhbiB0ZXN0IHYxMyAzcHgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
# qrtest::build_link(11, 3): the same form at version 11 (321 characters, record 203 B) - today's share geometry.
LINK_V11_3PX = "https://yarost12.github.io/open-k2-firmware/r/#1t.UVIgc2NhbiB0ZXN0IHYxMSAzcHgAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"


class QrTestLink(unittest.TestCase):
    def test_v13_3px_golden_decodes_to_the_label(self):
        self.assertEqual(len(LINK_V13_3PX), 425)   # version 13, level L, byte mode (tools/qr_capacity.c)
        r = q.decode_link(LINK_V13_3PX)
        self.assertEqual(r["version"], 1)
        self.assertEqual(r["kind"], "t")
        self.assertEqual(r["record_kind"], "text")
        self.assertEqual(r["text"], "QR scan test v13 3px")
        self.assertEqual(len(r["record_hex"]) // 2, 281)   # the padded record, not a trimmed one
        self.assertTrue(r["record_hex"].endswith("00" * 261))

    def test_v11_3px_golden_decodes_to_the_label(self):
        self.assertEqual(len(LINK_V11_3PX), 321)   # version 11, level L, byte mode (the share link's limit)
        r = q.decode_link(LINK_V11_3PX)
        self.assertEqual((r["version"], r["kind"], r["record_kind"]), (1, "t", "text"))
        self.assertEqual(r["text"], "QR scan test v11 3px")
        self.assertEqual(len(r["record_hex"]) // 2, 203)

    def test_the_page_path_is_the_same_link(self):
        # The results page (docs/site/r/) labels a scanned 't' link with this text after the NUL trim.
        self.assertTrue(LINK_V13_3PX.startswith(q.LINK_BASE + "1t."))


if __name__ == "__main__":
    unittest.main()
