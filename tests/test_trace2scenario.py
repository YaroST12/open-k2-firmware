# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/trace2scenario.py on small synthetic recordings (the real ones live outside the tree)."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import trace2scenario as t2s  # noqa: E402

CAPS = bytes.fromhex("e06111" "2c9101" "08" "aabbccdd")   # SOP Source_Capabilities, 1 PDO
PS_RDY = bytes.fromhex("e00600" "11223344")
ACCEPT = bytes.fromhex("e00300" "55667788")
CABLE = bytes.fromhex("c04f11" "01800000" "99aabbcc")     # SOP' Vendor_Defined


def vbus(t, mv):
    return {"type": "VBUS", "t_ms": t, "raw": bytes([mv & 0xFF, mv >> 8]).hex()}


def rx(t, raw):
    return {"type": "RX", "t_ms": t, "raw": raw.hex()}


def reg(t, r, v):
    return {"type": "REG", "t_ms": t, "raw": bytes([r, v, 0, 0]).hex()}


def adc(t, cc1, cc2, mv=None):
    d = {"t_ms": t, "cc1_mv": cc1, "cc2_mv": cc2, "dp_mv": 0, "dm_mv": 0}
    if mv is not None:
        d["vbus_mv"] = mv
    return d


def write_rec(d: Path, trace, power, fw="v1.0-1-gabc"):
    d.mkdir(parents=True, exist_ok=True)
    (d / "trace.jsonl").write_text("".join(json.dumps(r) + "\n" for r in trace))
    (d / "power.jsonl").write_text("".join(json.dumps(r) + "\n" for r in power))
    (d / "meta.json").write_text(json.dumps({"hello": {"firmware": fw}}))


def rows(text, kind):
    return [ln.split() for ln in text.splitlines() if ln.startswith(kind + " ")]


class Trace2ScenarioTest(unittest.TestCase):
    def setUp(self):
        base = ROOT / "runs"
        base.mkdir(exist_ok=True)
        self._tmp = tempfile.TemporaryDirectory(dir=base, prefix="t2s-")
        self.tmp = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def test_names(self):
        self.assertEqual(t2s.msg_name(CAPS), "Source_Capabilities")
        self.assertEqual(t2s.msg_name(PS_RDY), "PS_RDY")
        self.assertEqual(t2s.sop_of(CAPS), "SOP")
        self.assertEqual(t2s.sop_of(CABLE), "SOP'")
        self.assertEqual(t2s.msg_name(b"\xe0"), "?")

    def test_one_rise_one_episode_on_the_adc_line(self):
        rec = self.tmp / "rec"
        trace = [vbus(0, 0), vbus(1000, 5000), rx(1100, CAPS), rx(1110, ACCEPT), rx(1200, PS_RDY)]
        power = [adc(t, 0, 1680) for t in range(500, 3000, 50)]
        write_rec(rec, trace, power)
        text, eps = t2s.build(rec, None, "syn")
        self.assertEqual(len(eps), 1)
        e = eps[0]
        self.assertAlmostEqual(e["rise"], 983.2, places=1)   # held to 980 ms, ramps to 5 V at 1000 ms, crosses 0.8 V
        self.assertEqual(e["name"], "rise1.0")
        self.assertEqual(e["line"], 2)                         # no attach record: the ADC's live line
        self.assertAlmostEqual(e["t0"], e["rise"] - 800, places=3)
        self.assertEqual(e["first_rec"], "Source_Capabilities")
        rp = rows(text, "rp")
        self.assertEqual(rp[0][2:], ["0", "1680"])             # Rp on CC2 at the ADC level, from the crossing
        fr = rows(text, "frame")
        self.assertEqual([f[3] for f in fr], [CAPS.hex(), ACCEPT.hex(), PS_RDY.hex()])
        self.assertTrue(all(f[2] == "2" and f[4] == "rec" for f in fr))
        self.assertIn("expect rise1.0 first_sop=Source_Capabilities caps>=1 verdict=present", text)
        self.assertIn("name syn", text)

    def test_rise_without_traffic_has_no_episode(self):
        rec = self.tmp / "quiet"
        write_rec(rec, [vbus(0, 0), vbus(1000, 5000)], [adc(t, 1680, 0) for t in range(500, 2000, 50)])
        _, eps = t2s.build(rec, None, "quiet")
        self.assertEqual(eps, [])
        self.assertEqual(t2s.main([str(rec), "-o", str(self.tmp / "quiet.scn")]), 1)

    def test_scan_reject_steps_rp_open_before_the_adc_shows_it(self):
        trace = [reg(1010, 0x02, 0x0B), reg(1012, 0x04, 0x3E), reg(1018, 0x04, 0x31)]
        self.assertEqual(t2s.scan_rejects(trace), [(1012.0, 2)])
        accepted = trace + [reg(1020, 0x03, 0x26)]
        self.assertEqual(t2s.scan_rejects(accepted), [])
        late_txcc = trace + [reg(1040, 0x03, 0x26)]            # a TXCC write long after is a later scan, not this one
        self.assertEqual(t2s.scan_rejects(late_txcc), [(1012.0, 2)])

        rec = self.tmp / "rej"
        tr = [vbus(0, 0), vbus(1000, 5000), rx(1100, CAPS), reg(1500, 0x02, 0x0B), reg(1502, 0x04, 0x3E),
              reg(1508, 0x04, 0x31)]
        write_rec(rec, tr, [adc(t, 0, 1680) for t in range(500, 3000, 50)])
        text, _ = t2s.build(rec, None, "rej")
        rp = [(float(r[1]), r[2], r[3]) for r in rows(text, "rp")]
        self.assertIn((1501.5, "0", str(t2s.OPEN_RP_MV)), rp)

    def test_power_sample_repeating_the_trace_holds_the_rise(self):
        # The trace caught 1.4 V at 1000 ms and 5 V at 1200 ms; the host sample at 1180 ms still saw 1.4 V.
        trace = [vbus(0, 0), vbus(1000, 1400), vbus(1200, 5000)]
        power = [adc(1180, 0, 1680, mv=1400), adc(1195, 0, 1680, mv=3000)]
        recs = t2s.vbus_records(trace, power)
        self.assertIn((1180.0, 1400), recs)
        self.assertNotIn((1195.0, 3000), recs)                 # near a trace record and not a repeat: dropped
        pts = t2s.vbus_timeline(recs)
        self.assertLess(t2s.vbus_at(pts, 1170), 1500)          # not ramped through the pause at the slew

    def test_rise_caught_midway_continues_at_the_slew(self):
        pts = t2s.vbus_timeline([(0.0, 0), (1000.0, 1000), (1300.0, 5000)])
        self.assertIn((1016.0, 5000), pts)                     # 4 V at 0.25 V/ms from 1 V

    def test_template_fills_a_late_attach(self):
        tpl = self.tmp / "tpl"
        write_rec(tpl, [vbus(0, 0), vbus(5000, 5000), rx(5100, CAPS), rx(5110, ACCEPT), rx(5200, PS_RDY)],
                  [adc(t, 0, 1680) for t in range(4500, 6000, 50)])
        rec = self.tmp / "late"
        write_rec(rec, [vbus(0, 0), vbus(1000, 5000), rx(1150, PS_RDY)],
                  [adc(t, 0, 1680) for t in range(500, 3000, 50)])
        text, eps = t2s.build(rec, tpl, "late")
        self.assertEqual(eps[0]["first_rec"], "PS_RDY")
        self.assertIn("template_offset -4050.0", eps[0]["note"])
        fr = rows(text, "frame")
        self.assertEqual([(float(f[1]), f[4]) for f in fr], [(1050.0, "tpl"), (1060.0, "tpl"), (1150.0, "rec")])
        self.assertIn("template ", text)

    def test_main_writes_the_scenario(self):
        rec = self.tmp / "rec"
        write_rec(rec, [vbus(0, 0), vbus(1000, 5000), rx(1100, CAPS)], [adc(t, 1680, 0) for t in range(500, 3000, 50)])
        out = self.tmp / "out" / "syn.scn"
        self.assertEqual(t2s.main([str(rec), "-o", str(out)]), 0)
        text = out.read_text()
        self.assertTrue(text.startswith("scenario 1\nname syn\n"))
        self.assertIn("fw v1.0-1-gabc", text)
        self.assertEqual(rows(text, "episode")[0][4:6], ["line", "1"])


if __name__ == "__main__":
    unittest.main()
