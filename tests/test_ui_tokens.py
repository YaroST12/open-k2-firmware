# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The firmware colour tokens (include/k2/ui/ui_style.hpp) must equal the design table in
docs/ui_mockups/render_mockups.py (UI design v2, section 3.3), and kWatt stays out of warnings."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HDR = (ROOT / "include/k2/ui/ui_style.hpp").read_text()
MOCK = (ROOT / "docs/ui_mockups/render_mockups.py").read_text()

# firmware name -> mockup token
PAIRS = {
    "kBg": "bg", "kSurface": "surface", "kSurface2": "surface2", "kBar": "bar", "kRule": "rule", "kText": "text",
    "kDim": "dim", "kFaint": "faint", "kVolt": "volt", "kAmp": "amp", "kWatt": "watt", "kEnergy": "energy",
    "kCharge": "charge", "kOhm": "ohm", "kDPlus": "dplus", "kDMinus": "dminus", "kFlow": "flow", "kOk": "ok",
    "kCaution": "caution", "kError": "error", "kStale": "stale", "kFocus": "focus",
    "kVoltT": "volt_t", "kAmpT": "amp_t", "kWattT": "watt_t", "kEnergyT": "energy_t", "kOkT": "ok_t",
    "kChargeT": "charge_t", "kCautionT": "caution_t", "kErrorT": "error_t", "kStaleT": "stale_t",
    "kFocusT": "focus_t", "kDimT": "dim_t", "kDPlusT": "dplus_t", "kDMinusT": "dminus_t", "kFaintT": "faint_t",
}


def firmware_tokens():
    out = {}
    for m in re.finditer(r"constexpr uint16_t ([^;]+);", HDR):
        for d in m.group(1).split(","):
            n, _, v = d.partition("=")
            if v.strip().startswith("0x"):
                out[n.strip()] = int(v.strip(), 16)
    return out


def mockup_tokens():
    body = MOCK[MOCK.index("TOK = {"):MOCK.index("ADV = ")]
    return {k: int(v, 16) for k, v in re.findall(r'"(\w+)": (0x[0-9A-Fa-f]+)', body)}


class Tokens(unittest.TestCase):
    def test_match_design_table(self):
        fw, mk = firmware_tokens(), mockup_tokens()
        for name, tok in PAIRS.items():
            self.assertIn(name, fw, name)
            self.assertEqual(fw[name], mk[tok], f"{name} vs {tok}")

    def test_watt_only_for_power(self):
        pages = (ROOT / "src/ui/ui_pages.cpp").read_text()
        for line in pages.splitlines():
            if "kWatt" in line:
                self.assertTrue(any(w in line for w in ('"W"', "PPS", "w_.init", "power_mw", "power bar")), line.strip())


if __name__ == "__main__":
    unittest.main()
