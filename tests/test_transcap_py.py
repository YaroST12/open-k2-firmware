# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/transcap.py against the C++ encoder: tests/test_transcap dumps a C135-shaped capture
($K2_DUMP/transcap.bin, run through `make test`) and the decoder here must give back the same series.
The pure-Python parts (damage handling, CSV, summary, the remote ops) run without the dump."""
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import transcap as tc  # noqa: E402

DUMP = Path(os.environ.get("K2_DUMP", "host-build")) / "transcap.bin"


def expected(i):
    """Mirror of dump_for_python() in tests/test_transcap.cpp."""
    if i >= 300:
        return 0, 0, 0
    v = 5000 if i < 100 else 5000 - (i - 99) * 57 if i < 130 else 3300 if i < 265 else 0
    return v, 40 if i < 265 else 0, 1 if i % 50 == 0 else 0


class CppStream(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DUMP.exists():
            raise unittest.SkipTest("transcap.bin not dumped (run through make test)")
        cls.data = DUMP.read_bytes()
        cls.cap = tc.decode_stream(cls.data)

    def test_header_and_counts(self):
        c = self.cap
        self.assertEqual(c.reason_name, "shutoff")
        self.assertEqual(len(c.markers), 4)
        self.assertGreater(len(c.samples), 300)
        self.assertEqual(c.span_us, c.samples[-1][0] - c.samples[0][0])

    def test_series_match_the_encoder_input(self):
        t = 1_000_000
        for i in range(300):
            t += 6000 if i == 150 else 1000
            s = self.cap.samples[i]
            self.assertEqual(s, (t,) + expected(i), f"sample {i}")
        for s in self.cap.samples[300:]:
            self.assertEqual(s[1:], (0, 0, 0))

    def test_markers_on_the_same_clock(self):
        names = [tc.MARKS[m[1]] for m in self.cap.markers]
        self.assertEqual(names, ["request", "accept", "ps_rdy", "trigger"])
        by = {tc.MARKS[m[1]]: m for m in self.cap.markers}
        self.assertEqual(by["request"][2:], (2, 3300))
        self.assertEqual(by["accept"][0], self.cap.samples[100][0])
        self.assertEqual(self.cap.trigger_us, self.cap.samples[265][0])

    def test_accept_to_cut_is_165_ms(self):
        by = {tc.MARKS[m[1]]: m for m in self.cap.markers}
        self.assertAlmostEqual((self.cap.trigger_us - by["accept"][0]) / 1000.0, 165.0 + 5.0, places=3)   # 165 samples + the 6 ms hole

    def test_csv_and_summary(self):
        csv = tc.to_csv(self.cap).splitlines()
        self.assertEqual(csv[0], "t_ms,vbus_mv,cc1_mv,cc2_mv,event")
        self.assertEqual(len(csv) - 1, len(self.cap.samples) + len(self.cap.markers))
        self.assertTrue(any(row.endswith(",accept") for row in csv))
        times = [float(r.split(",")[0]) for r in csv[1:]]
        self.assertEqual(times, sorted(times))
        self.assertIn("reason shutoff", tc.summary(self.cap))

    def test_png_or_clean_refusal(self):
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "c.png"
            if tc.plot_png(self.cap, str(out)):
                self.assertGreater(out.stat().st_size, 5000)
            else:
                self.assertFalse(out.exists())


class Damage(unittest.TestCase):
    def test_rejects_bad_streams(self):
        for bad in (b"", b"K2TD", b"XXXX" + bytes(8), b"K2TD" + struct.pack("<HBBI", 5, 0, 0, 0)):
            with self.assertRaises(tc.StreamError):
                tc.decode_stream(bad)

    @unittest.skipUnless(DUMP.exists(), "no dump")
    def test_every_truncation_is_an_error_not_a_crash(self):
        data = DUMP.read_bytes()
        for n in range(0, len(data) - 1, 7):
            try:
                tc.decode_stream(data[:n])
            except tc.StreamError:
                pass

    def test_empty_capture(self):
        cap = tc.decode_stream(b"K2TD" + struct.pack("<HBBI", 0, 0, 0, 0))
        self.assertEqual(cap.samples, [])
        self.assertEqual(tc.to_csv(cap), "t_ms,vbus_mv,cc1_mv,cc2_mv,event\n")


class Names(unittest.TestCase):
    def test_cc_scale(self):
        self.assertEqual(tc.cc_mv(0), 0)
        self.assertEqual(tc.cc_mv(255), 3287)
        self.assertEqual(tc.cc_mv(100), 1289)

    def test_marker_text(self):
        self.assertEqual(tc.marker_text((0, 1, 2, 3300)), ("arm", "PDO 2 3300 mV"))
        self.assertEqual(tc.marker_text((0, 2, 2, 0)), ("request", "PDO 2"))
        self.assertEqual(tc.marker_text((0, 12, 0, 0x0041))[0], "rx")
        self.assertEqual(tc.marker_text((0, 99, 0, 0))[0], "mark99")


if __name__ == "__main__":
    unittest.main()
