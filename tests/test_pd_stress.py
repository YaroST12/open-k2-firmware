# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the PD Stress tool side: the result blob decoder (tools/pdstress_blob.py), `k2_remote pd-stress`
(CMD_PD_STRESS 0x55) and `pd_stress.py rescore`.

The decoder is checked against the firmware's own bytes: a small host program built from include/k2/pdstress/result.hpp writes a
blob, and the decoder must read back exactly what was put in. The default weights of the rescore tool are parsed from score.hpp; the
drift test compiles score.hpp and compares the numbers the firmware really uses."""
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr
import pd_stress
import pdstress_blob as pb

HAVE_GXX = shutil.which("g++") is not None

DUMP_CPP = r"""
#include <array>
#include <cstdio>
#include "k2/pdstress/result.hpp"
using namespace k2::pdstress;
int main() {
    std::array<std::int16_t, 640> buf{};
    Record r{std::span<std::int16_t>(buf)};
    r.clear();
    // slot 1 (Slam): 40 transitions, a Reject, two late Accepts, a Wait; slot 5 (PpsSteps): 12 transitions, one soft reset
    for (int i = 0; i < 40; i++) bump_transition(r.rec(1));
    bump_event(r.rec(1), Event::RejectRx);
    bump_event(r.rec(1), Event::AcceptLate, 2);
    bump_event(r.rec(1), Event::WaitRx);
    for (int i = 0; i < 12; i++) bump_transition(r.rec(5));
    bump_event(r.rec(5), Event::SoftResetRx);
    bump_event(r.rec(5), Event::HardResetRx);
    add_sample(r.rec(1), Metric::Accept, 31);
    add_sample(r.rec(1), Metric::Accept, 33);
    add_sample(r.rec(1), Metric::Accept, 640);
    add_sample(r.rec(5), Metric::Over, 120);
    result::Info i;
    i.stop = Stop::Done; i.finished = true;
    i.n_order = 2; i.order[0] = 1; i.order[1] = 5; i.reboots = 1;
    i.opt.mask = 0x7FF; i.opt.cycles_pct = 100; i.opt.pace = 1; i.opt.max_ms = 600000; i.opt.max_transitions = 2000;
    i.transitions = 52; i.elapsed_ms = 91234; i.stamp_suspect = 3;
    i.seq_flags[1] = 0x01; i.seq_flags[5] = 0x01; i.seq_flags[8] = 0x02 | (7 << 4);
    std::array<std::uint8_t, 64> b{};
    for (std::size_t off = 0; off < result::kTotalBytes;) {
        const std::size_t n = result::read(i, r, off, std::span<std::uint8_t>(b));
        if (!n) break;
        for (std::size_t k = 0; k < n; k++) std::printf("%02x", b[k]);
        off += n;
    }
    std::printf("\n");
}
"""

WEIGHTS_CPP = r"""
#include <cstdio>
#include "k2/pdstress/score.hpp"
using namespace k2::pdstress;
int main() {
    for (std::size_t i = 0; i < kEventCount; i++) std::printf("%s %u\n", event_name(Event(i)), unsigned(kWeights[i]));
    std::printf("min_transitions %u\n", unsigned(kMinTransitions));
}
"""


def build_and_run(src, tag):
    with tempfile.TemporaryDirectory() as d:
        cpp = Path(d) / f"{tag}.cpp"
        exe = Path(d) / tag
        cpp.write_text(src)
        subprocess.run(["g++", "-std=c++23", "-I", str(ROOT / "include"), str(cpp), "-o", str(exe)], check=True,
                       capture_output=True)
        return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


class Weights(unittest.TestCase):
    def test_defaults_parsed_from_score_hpp(self):
        t = pb.load_tables()
        self.assertEqual(len(t["events"]), len(t["weights"]))
        self.assertEqual(t["events"][0], "accept_late")
        self.assertEqual(t["events"][-1], "no_recovery")
        self.assertEqual(t["weights"][-1], 100)

    @unittest.skipUnless(HAVE_GXX, "needs a host g++")
    def test_defaults_match_the_compiled_firmware_table(self):
        out = build_and_run(WEIGHTS_CPP, "weights").splitlines()
        fw = {l.split()[0]: int(l.split()[1]) for l in out}
        t = pb.load_tables()
        self.assertEqual(fw.pop("min_transitions"), t["min_transitions"])
        self.assertEqual(fw, dict(zip(t["events"], t["weights"])))
        self.assertEqual(pb.default_weights(), fw)

    def test_weights_file_must_use_known_keys(self):
        w = pb.merge_weights({"hard_reset": 7})
        self.assertEqual(w["hard_reset"], 7)
        self.assertEqual(w["accept_late"], pb.default_weights()["accept_late"])
        with self.assertRaises(ValueError):
            pb.merge_weights({"nope": 1})


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Decode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = bytes.fromhex(build_and_run(DUMP_CPP, "dump").strip())
        cls.r = pb.decode_blob(cls.blob)

    def test_header(self):
        r = self.r
        self.assertEqual((r["stop"], r["finished"], r["running"], r["reboots"]), ("done", True, False, 1))
        self.assertEqual(r["options"], {"mask": 0x7FF, "cycles_pct": 100, "pace": 1, "max_s": 600, "max_transitions": 2000})
        self.assertEqual((r["transitions"], r["elapsed_ms"], r["stamp_suspect"]), (52, 91234, 3))
        self.assertEqual(r["order"], [1, 5])

    def test_sequence_counters_and_flags(self):
        s = {x["id"]: x for x in self.r["sequences"]}
        self.assertEqual(s[1]["name"], "slam")
        self.assertEqual(s[1]["transitions"], 40)
        self.assertEqual(s[1]["counts"]["reject"], 1)
        self.assertEqual(s[1]["counts"]["accept_late"], 2)
        self.assertEqual(s[5]["name"], "ppssteps")
        self.assertEqual(s[5]["counts"]["hard_reset"], 1)
        self.assertTrue(s[1]["done"])
        self.assertTrue(s[8]["skipped"])
        self.assertEqual(s[8]["na"], "no EPR cable proof")

    def test_distributions(self):
        a = {x["id"]: x for x in self.r["sequences"]}[1]["dist"]["accept"]
        self.assertEqual((a["n"], a["min"], a["max"], a["unit"]), (3, 31, 640, "0.1ms"))
        self.assertEqual(sum(a["bins"]), 3)
        self.assertEqual(a["med"], 50)   # the upper edge of bin 0, bin resolution
        o = {x["id"]: x for x in self.r["sequences"]}[5]["dist"]["over"]
        self.assertEqual((o["n"], o["min"], o["max"]), (1, 120, 120))

    def test_firmware_score_and_points_match_the_host_recompute(self):
        r = self.r
        pts = {e: v["points"] for e, v in r["events"].items()}
        self.assertEqual(pts["reject"], 2)
        self.assertEqual(pts["hard_reset"], 20)
        self.assertEqual(r["events"]["accept_late"]["count"], 2)
        self.assertEqual(r["events"]["hard_reset"]["count"], 1)
        # the firmware's own score is in the blob and the host recompute agrees under the default weights
        self.assertEqual(r["score_cp"], pb.score_cp(r["totals"], pb.default_weights()))
        self.assertEqual(r["score_cp"], (2 + 2 + 2 + 5 + 20) * 10000 // 52)
        for e, v in r["events"].items():
            self.assertEqual(v["points"], v["count"] * pb.default_weights()[e])

    def test_truncated_blob_is_refused(self):
        with self.assertRaises(ValueError):
            pb.decode_blob(self.blob[:50])


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Rescore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.blob = bytes.fromhex(build_and_run(DUMP_CPP, "dump").strip())

    def saved(self, name):
        return pd_stress.saved_result(self.blob, charger=name)

    def test_same_weights_give_the_firmware_score(self):
        rows = pd_stress.rescore([self.saved("novoo")], pb.default_weights())
        self.assertEqual(rows[0]["fw_cp"], rows[0]["new_cp"])

    def test_other_weights_change_the_score(self):
        w = pb.merge_weights({"hard_reset": 40})
        rows = pd_stress.rescore([self.saved("novoo")], w)
        self.assertEqual(rows[0]["new_cp"], (2 + 2 + 2 + 5 + 40) * 10000 // 52)
        self.assertNotEqual(rows[0]["fw_cp"], rows[0]["new_cp"])

    def test_table_is_charger_by_event(self):
        a, b = self.saved("novoo"), self.saved("fh22")
        t = pd_stress.rescore_table([a, b], pb.default_weights())
        self.assertEqual(t["chargers"], ["novoo", "fh22"])
        self.assertEqual(t["cells"]["hard_reset"]["novoo"], {"count": 1, "points": 20})
        self.assertEqual(t["cells"]["reject"]["fh22"], {"count": 1, "points": 2})

    def test_saved_result_round_trips_through_json(self):
        s = json.loads(json.dumps(self.saved("novoo")))
        rows = pd_stress.rescore([s], pb.default_weights())
        self.assertEqual(rows[0]["fw_cp"], rows[0]["new_cp"])

    def test_cli_prints_side_by_side(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "novoo.json"
            p.write_text(json.dumps(self.saved("novoo")))
            w = Path(d) / "w.json"
            w.write_text(json.dumps({"hard_reset": 40}))
            out = subprocess.run([sys.executable, str(ROOT / "tools/pd_stress.py"), "rescore", str(p), "--weights", str(w)],
                                 check=True, capture_output=True, text=True).stdout
        self.assertIn("novoo", out)
        self.assertIn("hard_reset", out)
        self.assertIn("firmware", out)


class Cli(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(ROOT / "tools/pd_stress.py"), *args], capture_output=True, text=True)

    def test_weights_prints_the_firmware_table(self):
        r = self.run_cli("weights")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(json.loads(r.stdout), pb.default_weights())

    def test_run_needs_confirm_and_touches_no_device(self):
        r = self.run_cli("run", "--charger", "x")
        self.assertEqual(r.returncode, 1)
        self.assertIn("--confirm", r.stderr)

    def test_rescore_refuses_an_unknown_weight_name(self):
        with tempfile.TemporaryDirectory() as d:
            w = Path(d) / "w.json"
            w.write_text(json.dumps({"hard_rest": 3}))
            r = self.run_cli("rescore", str(w), "--weights", str(w))
        self.assertEqual(r.returncode, 1)
        self.assertIn("unknown event", r.stderr)

    def test_remote_parser_takes_the_pd_stress_commands(self):
        p = kr.build_parser()
        a = p.parse_args(["pd-stress", "start", "--mask", "0x3f", "--cycles", "50", "--confirm"])
        self.assertEqual((a.command, a.pdstress_cmd, a.mask, a.cycles, a.confirm), ("pd-stress", "start", 0x3F, 50, True))
        for c in ("status", "result", "stop"):
            self.assertEqual(p.parse_args(["pd-stress", c]).pdstress_cmd, c)


class FakeLink:
    """Answers CMD_PD_STRESS from a canned blob; records the requests."""

    def __init__(self, blob=b"\x01" * 100):
        self.blob = blob
        self.sent = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_PD_STRESS
        self.sent.append(bytes(payload))
        op = payload[0]
        if op == kr.PDS_READ:
            off = int.from_bytes(payload[1:3], "little")
            ch = self.blob[off:off + kr.PDS_CHUNK]
            return type("R", (), {"payload": off.to_bytes(2, "little") + len(self.blob).to_bytes(2, "little") + bytes([len(ch)]) + ch})()
        # state running, stop 0, start 0, seq 3 of 11 (id 2), reboots 1, 52 transitions, 91234 ms, score 321 cp, blob length
        st = bytes([2, 0, 0, 3, 11, 2, 1]) + (52).to_bytes(2, "little") + (91234).to_bytes(4, "little") + \
            (321).to_bytes(4, "little") + len(self.blob).to_bytes(2, "little")
        return type("R", (), {"payload": st})()


class Remote(unittest.TestCase):
    def test_command_id(self):
        self.assertEqual(kr.CMD_PD_STRESS, 0x55)

    def test_name_tables_follow_the_firmware_headers(self):
        t = pb.load_tables()
        self.assertEqual(kr.PDS_STARTS, t["starts"])
        self.assertEqual(kr.PDS_STOPS, t["stops"])

    def test_render_status_and_result(self):
        text = kr.render_pdstress({"pdstress": kr.pdstress_status(FakeLink())})
        self.assertIn("running", text)
        self.assertIn("3.21", text)

    def test_status_decode(self):
        s = kr.pdstress_status(FakeLink())
        self.assertEqual((s["state"], s["start"], s["seq_idx"], s["seq_total"], s["reboots"]), ("running", "ok", 3, 11, 1))
        self.assertEqual((s["transitions"], s["elapsed_ms"], s["score_cp"], s["blob_len"]), (52, 91234, 321, 100))
        self.assertAlmostEqual(s["score"], 3.21)

    def test_status_too_short(self):
        class Short(FakeLink):
            def call(self, cmd, payload=b""):
                return type("R", (), {"payload": b"\x00" * 5})()
        with self.assertRaises(kr.FrameError):
            kr.pdstress_status(Short())

    def test_start_payload_and_confirm(self):
        l = FakeLink()
        kr.pdstress_start(l, mask=0x3F, cycles_pct=50, pace=1, max_s=120, max_trans=200, confirm=True)
        self.assertEqual(l.sent[0], bytes([kr.PDS_START]) + (0x3F).to_bytes(2, "little") + (50).to_bytes(2, "little") + bytes([1]) +
                         (120).to_bytes(2, "little") + (200).to_bytes(2, "little") + bytes([1]))

    def test_start_defaults_are_zero(self):
        l = FakeLink()
        kr.pdstress_start(l, confirm=True)
        self.assertEqual(l.sent[0], bytes([kr.PDS_START]) + bytes(2 + 2 + 1 + 2 + 2) + bytes([1]))

    def test_start_needs_confirm_on_the_host_too(self):
        with self.assertRaises(SystemExit):
            kr.pdstress_start(FakeLink(), confirm=False)

    def test_read_blob_chunks(self):
        l = FakeLink(bytes(range(100)))
        self.assertEqual(kr.pdstress_read_blob(l), bytes(range(100)))
        self.assertEqual([int.from_bytes(s[1:3], "little") for s in l.sent], [0, 48, 96])

    def test_stop(self):
        l = FakeLink()
        kr.pdstress_stop(l)
        self.assertEqual(l.sent[0], bytes([kr.PDS_STOP]))


if __name__ == "__main__":
    unittest.main()
