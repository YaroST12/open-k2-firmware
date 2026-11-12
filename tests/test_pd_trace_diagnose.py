# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
# pd_trace.py diagnose: the host build of the K2's Black Box diagnosis (host-build/bbdiag, from src/pd/diag.cpp)
# over the captured fixtures. The rules themselves are tested in tests/test_pd_diag.cpp; this checks the plumbing.
import json
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import pd_trace as pt  # noqa: E402

FIX = ROOT / "tests" / "fixtures" / "bbdiag"
BBDIAG = ROOT / "host-build" / "bbdiag"


def pt_windows(path):
    """The window fixture back as power.jsonl fields (the inverse of pd_trace.power_windows)."""
    b = Path(path).read_bytes()
    for off in range(0, len(b) - pt.POWER_WINDOW.size + 1, pt.POWER_WINDOW.size):
        t, span, n, mv, ma, vlo, vhi, ilo, ihi = pt.POWER_WINDOW.unpack_from(b, off)
        w = dict(n=n, vbus_mv=mv, ibus_ma=ma)
        if n > 1:
            w.update(span_ms=span, vbus_min=vlo, vbus_max=vhi, ibus_min=ilo, ibus_max=ihi)
        yield t, w


def bbdiag(*args):
    r = subprocess.run([str(BBDIAG), *map(str, args)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    return r.stdout


@unittest.skipUnless(BBDIAG.exists(), "host-build/bbdiag not built (make test builds it)")
class Diagnose(unittest.TestCase):
    def test_twin_args_from_identify_json(self):
        args = pt.twin_args(FIX / "legion_rear_identify.json", name="Legion rear port", assumed=True, host=True)
        self.assertEqual(args[:2], ["--twin-name", "Legion rear port"])
        self.assertEqual(args[3], "3601912C,9902D145")
        self.assertIn("--twin-vid", args)
        self.assertEqual(args[args.index("--twin-vid") + 1], "6127")
        self.assertIn("--assumed", args)
        self.assertIn("--twin-host", args)

    def test_case1_text(self):
        # the acceptance case: a laptop behind a 60 W hub, its 65 W need from an assumed twin
        out = bbdiag(FIX / "hub_rear_60w.raw",
                     *pt.twin_args(FIX / "legion_rear_identify.json", "Legion rear port", assumed=True, host=True))
        self.assertIn("PD link healthy, power short", out)
        self.assertIn("Laptop needs 65 W, source gives 60 W max", out)
        self.assertIn('need: assumed twin "Legion rear port", 20 V 3.25 A', out)
        self.assertIn("Contract 20 V 3 A, draws", out)
        self.assertIn("did not flag capability mismatch", out)
        self.assertIn("No resets, no retries, no rejects", out)
        self.assertIn("Data lanes pass through the K2, not observed", out)
        self.assertIn("agreed over PD: DP alt mode", out)
        self.assertNotIn("negotiat", out)

    def test_json_sessions(self):
        out = bbdiag(FIX / "splitter_rear_left.raw", "--json")
        sessions = json.loads(out)
        self.assertEqual(len(sessions), 6)
        rear = sessions[0]
        codes = {f["code"] for f in rear["findings"]}
        self.assertIn("contract_unseen", codes)
        self.assertIn("draw_reduced", codes)
        self.assertIn("cable_silent", codes)
        self.assertNotIn("vbus_lost", codes)
        self.assertEqual(rear["lines"][0], "PD link healthy, contract not observed")
        left = sessions[1]
        self.assertIn("swap_refused", {f["code"] for f in left["findings"]})

    def test_every_fixture_runs(self):
        for raw in sorted(FIX.glob("*.raw")):
            with self.subTest(raw=raw.name):
                out = bbdiag(raw)
                self.assertIn("== session 1", out)

    def test_power_windows_from_jsonl(self):
        # a single-sample row is its own min/max; a window row keeps its extremes
        import tempfile
        rows = [{"t_ms": 1000.0, "n": 1, "valid": True, "vbus_mv": 20000, "ibus_ma": 2000, "kind": "power"},
                {"t_ms": 2000.0, "n": 20, "valid": True, "vbus_mv": 19800, "ibus_ma": 900, "span_ms": 950,
                 "vbus_min": 18986, "vbus_max": 20465, "ibus_min": 0, "ibus_max": 5620, "kind": "power"},
                {"t_ms": 2050.0, "n": 1, "valid": False, "vbus_mv": 0, "ibus_ma": 0, "kind": "power"}]
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "power.jsonl"
            p.write_text("".join(json.dumps(r) + "\n" for r in rows))
            b = pt.power_windows(p)
        self.assertEqual(len(b), 2 * pt.POWER_WINDOW.size)
        self.assertEqual(pt.POWER_WINDOW.size, 20)   # tools/bbdiag_walk.hpp kWindowRecord
        self.assertEqual(pt.POWER_WINDOW.unpack_from(b, 0), (1000, 0, 1, 20000, 2000, 20000, 20000, 2000, 2000))
        self.assertEqual(pt.POWER_WINDOW.unpack_from(b, 20), (2000, 950, 20, 19800, 900, 18986, 20465, 0, 5620))

    def test_novoo_with_power_windows(self):
        # the fast min/max windows show what the ring's rate-limited VBUS records average away
        out = bbdiag(FIX / "novoo_legion_overdraw.raw", "--power", FIX / "novoo_legion_overdraw.power", "--json")
        codes = {f["code"]: f for f in json.loads(out)[0]["findings"]}
        self.assertEqual(codes["over_draw"]["sev"], "fail")
        self.assertIn("far above its contract: up to 5.62 A", codes["over_draw"]["title"])
        self.assertEqual(codes["input_cuts"]["sev"], "warn")
        self.assertIn("vbus_low", codes)
        self.assertNotIn("vbus_lost", codes)
        ring = {f["code"] for f in json.loads(bbdiag(FIX / "novoo_legion_overdraw.raw", "--json"))[0]["findings"]}
        self.assertNotIn("input_cuts", ring)

    def test_cli_power_next_to_trace(self):
        # a k2_stream run directory: power.jsonl next to trace.raw is picked up by itself
        import shutil
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            shutil.copy(FIX / "novoo_legion_overdraw.raw", Path(d) / "trace.raw")
            rows = []
            for t, w in pt_windows(FIX / "novoo_legion_overdraw.power"):
                rows.append(json.dumps(dict(t_ms=float(t), kind="power", valid=True, **w)))
            (Path(d) / "power.jsonl").write_text("\n".join(rows) + "\n")
            r = subprocess.run([sys.executable, str(ROOT / "tools" / "pd_trace.py"), "diagnose", str(Path(d) / "trace.raw")],
                               capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("input cuts out repeatedly", r.stdout)
        self.assertIn("power windows", r.stderr)

    def test_cli_diagnose(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "pd_trace.py"), "diagnose",
                            str(FIX / "stress_soft_resets.raw")], capture_output=True, text=True, timeout=300)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("10 soft resets on SOP", r.stdout)


if __name__ == "__main__":
    unittest.main()
