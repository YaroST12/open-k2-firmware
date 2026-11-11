# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The inline VOOC decoder never touches the pins.

include/k2/fastcharge/vooc_decode.hpp runs in the EXTI ISR while a phone charges through the K2, so it must not be able
to drive D+ / D-. It is pure: it includes only standard headers and k2/fastcharge/vooc.hpp (constants and the slave
templates, which need a Hw the caller passes), and its code names no driver, register, pin or Hw call. The firmware glue
(src/fastcharge/dpsniff.cpp) feeds it edge words and reads its summary; the pins stay floating inputs set by dpsniff.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "include/k2/fastcharge/vooc_decode.hpp"
ALLOWED = {"array", "cstddef", "cstdint", "k2/fastcharge/vooc.hpp"}
# driver namespaces, register access, and the vooc::Hw calls that move a line
FORBIDDEN = re.compile(r"\b(dpdm|pins|reg|systick|exti|gpio|volatile|reinterpret_cast|drive_dm|release_dm|tx_reply|rx_command"
                       r"|set_dp|set_dm|wait_frame|delay_us)\b")


def includes(text):
    return set(re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M))


def code(text):
    """The text without comments."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def violations(text):
    out = sorted(includes(text) - ALLOWED)
    out += sorted(set(m.group(1) for m in FORBIDDEN.finditer(code(text))))
    return out


class DecoderIsolation(unittest.TestCase):
    def test_header_includes_and_calls_nothing_that_drives(self):
        self.assertEqual(violations(HEADER.read_text()), [])

    def test_the_check_catches_a_driver(self):
        bad = '#include "k2/drivers/dpdm.hpp"\nvoid f() { k2::dpdm::set_dp(k2::dpdm::Drive::V0); }\n'
        self.assertEqual(violations(bad), ["k2/drivers/dpdm.hpp", "dpdm", "set_dp"])
        self.assertEqual(violations("// dpdm::set_dp in a comment is fine\n"), [])


if __name__ == "__main__":
    unittest.main()
