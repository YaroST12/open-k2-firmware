# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the PD Spec Check tool side: the result blob decoder (tools/pdspec_blob.py), `k2_remote pd-spec`
(CMD_PD_SPEC 0x5A) and tools/pd_spec.py.

The decoder is checked against the firmware's own bytes: a small host program built from include/k2/pdspec/result.hpp fills the
aggregates, timers and records, streams the blob through the firmware's BlobReader, and the decoder must read back exactly what was put
in. The name tables of k2_remote are checked against the ones parsed from the firmware headers."""
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
import pd_spec
import pdspec_blob as pb

HAVE_GXX = shutil.which("g++") is not None

DUMP_CPP = r"""
#include <array>
#include <cstdio>
#include "k2/pdspec/result.hpp"
using namespace k2::pdspec;
int main() {
    std::array<Agg, kChecks> agg{};
    for (auto &a : agg) a.skip = Skip::NotSelected;
    // Accept: two passing measurements, the slower one is the worst (closest to the limit)
    agg[std::size_t(Check::AcceptTime)].skip = Skip::None;
    agg[std::size_t(Check::AcceptTime)].add(Kind::Max, Meas{9000, 0, 0, 30000, 1, 2});
    agg[std::size_t(Check::AcceptTime)].add(Kind::Max, Meas{12500, 0, 0, 30000, 2, 3});
    // PS_RDY early: one down-step with VBUS 350 mV above the new level's band at PS_RDY
    agg[std::size_t(Check::PsRdyEarly)].skip = Skip::None;
    agg[std::size_t(Check::PsRdyEarly)].add(Kind::Max, Meas{9800, 0, 0, 9450, 3, 2});
    agg[std::size_t(Check::PsRdyEarly)].add(Kind::Max, Meas{5100, 0, 0, 5250, 2, 1});
    // a Range check failing low, and a skip with its reason
    agg[std::size_t(Check::HrRecover)].skip = Skip::None;
    agg[std::size_t(Check::HrRecover)].add(Kind::Range, Meas{500, 0, 660, 1000, 0, 0});
    agg[std::size_t(Check::PpsKeepAlive)].skip = Skip::NoPps;
    // an up-step's overshoot: a Range with the low side open
    agg[std::size_t(Check::PpsSettle)].skip = Skip::None;   // an inferred limit: over it, but unverified (no penalty)
    agg[std::size_t(Check::PpsSettle)].add(Kind::Max, Meas{40000, 0, 0, 25000, 0x05, 50});
    // a programmable check: where = APDO 5, class 2 (500 mV), down (a = 5 | 2 << 4 | 0x80), to 5.0 V (b = 50)
    agg[std::size_t(Check::PpsBand)].skip = Skip::None;
    agg[std::size_t(Check::PpsBand)].add(Kind::Max, Meas{3700, 0, 0, 275000, 0xA5, 50});
    agg[std::size_t(Check::Overshoot)].skip = Skip::None;
    agg[std::size_t(Check::Overshoot)].add(Kind::Range, Meas{21300, 0, -kOpenLimit, 21500, 2, 3});
    BlobHead h;
    h.state = 2; h.stop = 1; h.n_trans = 1; h.n_pass = 2;
    h.score = score(agg); h.elapsed_s = 321; h.flags = 1; h.caps_note = CapsNote::PpsNoAvs; h.opts = kOptFull20;
    Timers t;
    t.pps_ka_ms = 14100; t.hr_from_mv = 9000; t.hr_off_ms = 420; t.hr_dark_ms = 500; t.hr_on_ms = 290; t.det_from_mv = 5000;
    t.det_off_ms = 120; t.poll_gap_ms = 3;
    std::array<TransRec, 1> tr{};
    tr[0].from = 1; tr[0].to = 2; tr[0].flags = kTrEpr | kTrLiveBand; tr[0].accept_100us = 123; tr[0].psrdy_ms = 180;
    tr[0].start_ms = 30; tr[0].band_ms = 150; tr[0].psrdy_mv = 8950; tr[0].peak_mv = 9100; tr[0].final_mv = 9010; tr[0].gap_ms = 2;
    std::array<PassRec, 2> pp{};
    pp[0].pos = 5; pp[0].cls = 2; pp[0].steps = 770; pp[0].lo_mv = 3300; pp[0].hi_mv = 11000; pp[0].band_100us = 37;
    pp[0].psrdy_100us = 1200; pp[0].worst_err_mv = -45; pp[0].min_delta_mv = -2; pp[0].over_mv = 25; pp[0].out_100us = 4;
    pp[0].mono_fails = 1; pp[0].status = kPsOmf; pp[0].track = 1;
    pp[1].pos = 6; pp[1].cls = 3 | kPassAvs; pp[1].steps = 2; pp[1].lo_mv = 9000; pp[1].hi_mv = 15000;
    const BlobReader r(h, agg, t, tr, pp);
    std::array<std::uint8_t, 48> b{};
    for (std::size_t off = 0; off < r.size();) {
        const std::size_t n = r.read(off, std::span<std::uint8_t>(b));
        if (!n) break;
        for (std::size_t k = 0; k < n; k++) std::printf("%02x", b[k]);
        off += n;
    }
    std::printf("\n%u\n", unsigned(h.score));
}
"""

NAMES_CPP = r"""
#include <cstdio>
#include "k2/pdspec/engine.hpp"
using namespace k2::pdspec;
int main() {
    for (std::size_t i = 0; i < kChecks; i++) std::printf("check %s|%s|%u|%d\n", kDefs[i].key, kDefs[i].label, unsigned(kDefs[i].weight), int(kDefs[i].inferred));
    for (int i = 0; i <= int(Skip::NoAvs); i++) std::printf("skip %s\n", skip_name(Skip(i)));
    for (int i = 0; i <= int(CapsNote::AvsOnly); i++) std::printf("note %s\n", caps_note_name(CapsNote(i)));
    for (int k = 0; k < 2; k++)
        for (int c = 0; c < 3; c++) std::printf("step %d\n", int(step_mv(k ? ProgKind::Avs : ProgKind::Pps, std::uint8_t(c))));
    for (int i = 0; i <= int(Stop::Timeout); i++) std::printf("stop %s\n", stop_name(Stop(i)));
    for (int i = 0; i <= int(StartResult::Running); i++) std::printf("start %s\n", start_name(StartResult(i)));
    for (int i = 0; i < int(Family::Count); i++) std::printf("family %s\n", family_name(Family(i)));
    std::printf("const %ld %ld %u\n", long(kNever), long(kOpenLimit), unsigned(kBlobHeader));
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


class Tables(unittest.TestCase):
    def test_parsed_tables(self):
        t = pb.load_tables()
        self.assertEqual(len(t["checks"]), 36)
        self.assertEqual(t["checks"][0]["key"], "accept_time")
        self.assertEqual(t["checks"][pb.PSC1_CHECKS - 1]["key"], "source_reset")   # PSC1 blobs end here; the new checks are appended
        self.assertEqual(t["checks"][pb.PSC1_CHECKS - 1]["weight"], 20)
        self.assertEqual(t["checks"][-2:][0]["key"], "pps_track")
        self.assertEqual(t["checks"][-1]["key"], "avs_track")
        self.assertEqual(t["skips"][0], "-")
        self.assertEqual(t["skips"][-1], "no AVS")
        self.assertEqual(t["verdicts"], ["pending", "pass", "fail", "skip", "unverified"])
        self.assertEqual([c["key"] for c in t["checks"] if c["inferred"]], ["pps_settle"])
        self.assertEqual(t["steps"], {"pps": [20, 100, 500], "avs": [100, 500, 1000]})
        self.assertEqual(t["caps_notes"][1], "PPS without AVS: allowed before PD R3.3")

    @unittest.skipUnless(HAVE_GXX, "needs a host g++")
    def test_tables_match_the_compiled_firmware(self):
        out = build_and_run(NAMES_CPP, "names").splitlines()
        by = {}
        for l in out:
            k, v = l.split(" ", 1)
            by.setdefault(k, []).append(v)
        t = pb.load_tables()
        self.assertEqual(by["check"], [f"{c['key']}|{c['label']}|{c['weight']}|{int(c['inferred'])}" for c in t["checks"]])
        self.assertEqual(by["skip"], t["skips"])
        self.assertEqual(by["stop"], t["stops"])
        self.assertEqual(by["start"], t["starts"])
        self.assertEqual(by["family"], t["families"])
        self.assertEqual(by["note"], t["caps_notes"])
        self.assertEqual([int(s) for s in by["step"]], t["steps"]["pps"] + t["steps"]["avs"])
        self.assertEqual(by["const"], [f"{pb.NEVER} {pb.OPEN} {pb.HEADER}"])


@unittest.skipUnless(HAVE_GXX, "needs a host g++")
class Decode(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        hexline, score = build_and_run(DUMP_CPP, "dump").split()
        cls.blob = bytes.fromhex(hexline)
        cls.fw_score = int(score)
        cls.r = pb.decode_blob(cls.blob)

    def check(self, key):
        return next(c for c in self.r["checks"] if c["key"] == key)

    def test_header(self):
        r = self.r
        self.assertEqual((r["state"], r["stop"], r["elapsed_s"], r["release_unconfirmed"]), ("finished", "done", 321, True))
        self.assertEqual(r["score"], self.fw_score)
        # PS_RDY early 3 x 1 fail + HR off time 3 x 1 fail
        self.assertEqual(r["score"], 6)
        self.assertEqual(r["score"], sum(c["penalty"] for c in r["checks"]))
        self.assertEqual(r["tally"], {"pass": 3, "fail": 2, "skip": 30, "unverified": 1})
        self.assertEqual((r["version"], r["caps_note"], r["full20"]), (2, "PPS without AVS: allowed before PD R3.3", True))

    def test_checks(self):
        a = self.check("accept_time")
        self.assertEqual((a["verdict"], a["n"], a["fails"], a["worst"], a["hi"], a["a"], a["b"]), ("pass", 2, 0, 12500, 30000, 2, 3))
        e = self.check("ps_rdy_early")
        self.assertEqual((e["verdict"], e["n"], e["fails"], e["worst"], e["hi"], e["a"], e["b"], e["penalty"]),
                         ("fail", 2, 1, 9800, 9450, 3, 2, 3))
        h = self.check("hr_recover")
        self.assertEqual((h["verdict"], h["worst"], h["lo"], h["hi"]), ("fail", 500, 660, 1000))
        p = self.check("pps_keepalive")
        self.assertEqual((p["verdict"], p["skip"], p["worst"]), ("skip", "no PPS", None))
        self.assertEqual(self.check("detach_safe0v")["skip"], "not selected")
        u = self.check("pps_settle")
        self.assertEqual((u["verdict"], u["n"], u["fails"], u["worst"], u["hi"], u["penalty"]), ("unverified", 1, 1, 40000, 25000, 0))
        self.assertEqual(self.check("reattach")["skip"], "not selected")
        k = [d["key"] for d in pb.load_tables()["checks"]]
        b = self.check("pps_band")
        self.assertEqual(pb.fmt_at(k.index("pps_band"), b), "APDO 5 500 mV down to 5.0 V")
        self.assertEqual(pb.fmt_at(k.index("pps_settle"), u), "APDO 5 20 mV up to 5.0 V")
        self.assertEqual(pb.fmt_at(k.index("accept_time"), self.check("accept_time")), "PDO 2 to 3")
        # accept_time is shared: its worst may come from a programmable move (b = the level in 100 mV, never a PDO index)
        mv = {"key": "accept_time", "a": 0x96, "b": 106}
        self.assertEqual(pb.fmt_at(k.index("accept_time"), mv), "APDO 6 100 mV down to 10.6 V")
        self.assertEqual(pb.fmt_at(k.index("accept_time"), {"key": "accept_time", "a": 0x93, "b": 180},
                                   passes=[{"pos": 3, "kind": "avs"}]), "APDO 3 500 mV down to 18.0 V")

    def test_timers_and_records(self):
        t = self.r["timers"]
        self.assertEqual((t["pps_ka_ms"], t["epr_ka_ms"], t["hr_from_mv"], t["hr_off_ms"], t["hr_dark_ms"], t["hr_on_ms"]),
                         (14100, None, 9000, 420, 500, 290))
        self.assertEqual((t["hr_caps_ms"], t["det_from_mv"], t["det_off_ms"], t["det_back_ms"], t["poll_gap_ms"]),
                         (None, 5000, 120, None, 3))
        x = self.r["transitions"][0]
        self.assertEqual((x["from"], x["to"], x["flags"], x["accept_ms"], x["psrdy_ms"], x["start_ms"], x["band_ms"]),
                         (1, 2, ["epr", "live band"], 12.3, 180, 30, 150))
        self.assertEqual((x["psrdy_mv"], x["peak_mv"], x["final_mv"], x["gap_ms"]), (8950, 9100, 9010, 2))
        self.assertEqual(self.r["pps"], [])
        p, q = self.r["passes"]
        self.assertEqual((p["pos"], p["kind"], p["class"], p["step"], p["steps"], p["lo_mv"], p["hi_mv"]),
                         (5, "pps", 2, "500 mV", 770, 3300, 11000))
        self.assertEqual((p["band_ms"], p["psrdy_ms"], p["worst_err_mv"], p["min_delta_mv"], p["over_mv"], p["out_ms"]),
                         (3.7, 120.0, -45, -2, 25, 0.4))
        self.assertEqual((p["fails"]["monotonic"], p["fails"]["band"], p["status"], p["status_text"]), (1, 0, 16, "OMF"))
        self.assertEqual((q["kind"], q["class"], q["step"], q["steps"], q["status_text"]), ("avs", 3, "slams", 2, None))
        self.assertEqual((p["track"], q["track"]), (True, False))

    def test_report_text(self):
        text = pb.render_text(self.r)
        self.assertIn("score 6", text)
        self.assertIn("unplug", text)
        self.assertIn("ps_rdy_early    fail", text)
        self.assertIn("worst 9.800 V", text)
        self.assertIn("at PDO 3 to 2", text)
        self.assertIn("accept_time     pass", text)
        self.assertIn("worst 12.5 ms", text)
        self.assertIn("pps_keepalive   skip       no PPS", text)
        self.assertIn("pps_settle      unverified worst 40.0 ms", text)
        self.assertIn("unverified 1", text)
        self.assertIn("caps note (advisory, no verdict): PPS without AVS: allowed before PD R3.3", text)
        self.assertIn("20 mV over the whole window", text)
        self.assertIn("     5 pps  500 mV    3300..11000 mV   770", text)
        self.assertIn("1/0/0/0  OMF", text.replace("0/0/1/0/0/0", "1/0/0/0"))
        self.assertIn("     6 avs  slams", text)
        self.assertIn("limit <= 21.500 V", text)     # the overshoot's open low side is not printed
        self.assertIn("limit 660 ms .. 1000 ms", text)

    def test_psc1_blob_still_decodes(self):
        # A blob from before the sweep passes: magic PSC1, 21 checks, the 14-byte PPS points, bytes 13..15 zero.
        b = self.blob
        t_off = pb.HEADER + 36 * pb.AGG
        head = bytearray(b[:pb.HEADER])
        head[0:4] = b"PSC1"
        head[7] = 1
        head[13:16] = b"\0\0\0"
        point = bytes([5, 10, 1, 0]) + (5000).to_bytes(2, "little") + (-45).to_bytes(2, "little", signed=True) + \
            (-2).to_bytes(2, "little", signed=True) + (25).to_bytes(2, "little", signed=True) + (37).to_bytes(2, "little")
        old = bytes(head) + b[pb.HEADER:pb.HEADER + 21 * pb.AGG] + b[t_off:t_off + pb.TIMERS + pb.TRANS] + point
        r = pb.decode_blob(old)
        self.assertEqual((r["version"], len(r["checks"]), r["caps_note"], r["passes"]), (1, 21, None, []))
        p = r["pps"][0]
        self.assertEqual((p["pos"], p["steps"], p["mono_fails"], p["set_mv"], p["worst_err_mv"], p["max_settle_ms"]),
                         (5, 10, 1, 5000, -45, 3.7))
        self.assertEqual(r["transitions"], self.r["transitions"])
        text = pb.render_text(r)
        self.assertIn("APDO 5 at 5000 mV", text)
        self.assertIn("at APDO 5, point 50", text)      # PSC1 reads a programmable check's where as APDO + point

    def test_truncated_or_foreign_blob_is_refused(self):
        with self.assertRaises(ValueError):
            pb.decode_blob(self.blob[:100])
        with self.assertRaises(ValueError):
            pb.decode_blob(b"PSX1" + self.blob[4:])

    def test_saved_result_round_trips(self):
        s = pd_spec.saved_result(self.blob, charger="bank")
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "result.json"
            p.write_text(json.dumps(s))
            self.assertEqual(pd_spec.load_saved(p)["result"], self.r)
            out = subprocess.run([sys.executable, str(ROOT / "tools/pd_spec.py"), "report", str(p)], check=True,
                                 capture_output=True, text=True).stdout
        self.assertIn("PD Spec Check finished: done", out)


FIXTURES = ROOT / "tests" / "fixtures" / "pdspec"


class Fixtures(unittest.TestCase):
    """Blobs recorded on hardware (`pd_spec.py run` saves result.json with the raw blob): each must decode, and its score must be the
    sum of its penalties. Vacuous until the first recordings land."""

    def test_recorded_results_decode(self):
        for p in sorted(FIXTURES.glob("*.json")) if FIXTURES.is_dir() else []:
            with self.subTest(fixture=p.name):
                s = json.loads(p.read_text())
                blob = bytes.fromhex(s["blob_hex"])
                self.assertIn(blob[:4], pb.MAGICS)
                r = pb.decode_blob(blob)
                self.assertEqual(r["score"], min(0xFFFF, sum(c["penalty"] for c in r["checks"])))
                self.assertEqual(sum(r["tally"].values()), len(r["checks"]))
                self.assertIn("PD Spec Check", pb.render_text(r))


class Reboot(unittest.TestCase):
    def test_poll_state(self):
        run = {"state": "running"}
        self.assertEqual(pd_spec.poll_state(run, 1000, 3000), "running")
        self.assertEqual(pd_spec.poll_state({"state": "finished"}, 1000, 3000), "ended")
        self.assertEqual(pd_spec.poll_state(run, 90000, 800), "reboot")            # uptime went back
        self.assertEqual(pd_spec.poll_state({"state": "idle"}, 1000, 3000), "reboot")   # a run never reads idle once started
        self.assertEqual(pd_spec.poll_state(run, None, 3000), "running")

    def test_family_times(self):
        seen = [("Transitions", 0), ("PPS/AVS", 40), ("PPS timeout", 400), ("Detach", 430)]
        self.assertEqual(pd_spec.family_times(seen, 445), {"Transitions": 40, "PPS/AVS": 360, "PPS timeout": 30, "Detach": 15})
        self.assertIn("PPS/AVS 360 s", pd_spec.family_text(pd_spec.family_times(seen, 445)))


class Cli(unittest.TestCase):
    def test_run_needs_confirm_and_touches_no_device(self):
        r = subprocess.run([sys.executable, str(ROOT / "tools/pd_spec.py"), "run", "--charger", "x"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("--confirm", r.stderr)

    def test_remote_parser_takes_the_pd_spec_commands(self):
        p = kr.build_parser()
        a = p.parse_args(["pd-spec", "start", "--families", "transitions,detach", "--confirm"])
        self.assertEqual((a.command, a.pdspec_cmd, a.families, a.confirm), ("pd-spec", "start", "transitions,detach", True))
        self.assertFalse(a.pps_full)
        self.assertTrue(p.parse_args(["pd-spec", "start", "--pps-full", "--confirm"]).pps_full)
        self.assertTrue(pd_spec.build_parser().parse_args(["run", "--pps-full", "--confirm"]).pps_full)
        for c in ("status", "result", "stop"):
            self.assertEqual(p.parse_args(["pd-spec", c]).pdspec_cmd, c)

    def test_family_names_and_masks(self):
        self.assertEqual(kr.parse_families("all"), 0)
        self.assertEqual(kr.parse_families("0x3f"), 0x3F)
        self.assertEqual(kr.parse_families("transitions,detach"), 0x21)
        self.assertEqual(kr.parse_families("pps-steps, Hard Reset"), 0x12)   # the old name still reads
        self.assertEqual(kr.parse_families("PPS/AVS"), 0x02)
        self.assertEqual(kr.parse_families("pps-avs,detach"), 0x22)
        with self.assertRaises(SystemExit):
            kr.parse_families("0x40")
        with self.assertRaises(SystemExit):
            kr.parse_families("transitions,nope")


class FakeLink:
    """Answers CMD_PD_SPEC from a canned blob; records the requests."""

    def __init__(self, blob=b"\x01" * 100):
        self.blob = blob
        self.sent = []

    def call(self, cmd, payload=b""):
        assert cmd == kr.CMD_PD_SPEC
        self.sent.append(bytes(payload))
        if payload[0] == kr.PSC_READ:
            off = int.from_bytes(payload[1:3], "little")
            ch = self.blob[off:off + kr.PSC_CHUNK]
            return type("R", (), {"payload": off.to_bytes(2, "little") + len(self.blob).to_bytes(2, "little") + bytes([len(ch)]) + ch})()
        # running, stop 0, start ok, family PPS/AVS, item 4 of 12, families 0x23, fails 1 passes 2 skips 3, release unconfirmed
        st = bytes([1, 0, 0, 1, 4, 12, 0x23, 1, 2, 3, 1]) + (9).to_bytes(2, "little") + (75).to_bytes(2, "little") + \
            len(self.blob).to_bytes(2, "little")
        return type("R", (), {"payload": st})()


class Remote(unittest.TestCase):
    def test_command_id(self):
        self.assertEqual(kr.CMD_PD_SPEC, 0x5A)

    def test_name_tables_follow_the_firmware_headers(self):
        t = pb.load_tables()
        self.assertEqual(kr.PSC_STARTS, t["starts"])
        self.assertEqual(kr.PSC_STOPS, t["stops"])
        self.assertEqual(kr.PSC_FAMILIES, t["families"])
        self.assertEqual(kr.PSC_FAMILY_MASK, (1 << len(t["families"])) - 1)

    def test_status_decode_and_render(self):
        s = kr.pdspec_status(FakeLink())
        self.assertEqual((s["state"], s["start"], s["family"], s["item"], s["items"], s["families"]),
                         ("running", "ok", "PPS/AVS", 4, 12, 0x23))
        self.assertEqual(s["families_text"], "Transitions, PPS/AVS, Detach")
        self.assertEqual((s["fails"], s["passes"], s["skips"], s["score"], s["elapsed_s"], s["blob_len"]), (1, 2, 3, 9, 75, 100))
        self.assertTrue(s["release_unconfirmed"])
        text = kr.render_pdspec({"pdspec": s})
        self.assertIn("PPS/AVS 4/12", text)
        self.assertIn("score 9", text)
        self.assertIn("unplug", text)

    def test_status_too_short(self):
        class Short(FakeLink):
            def call(self, cmd, payload=b""):
                return type("R", (), {"payload": b"\x00" * 16})()
        with self.assertRaises(kr.FrameError):
            kr.pdspec_status(Short())

    def test_start_payload_and_confirm(self):
        l = FakeLink()
        kr.pdspec_start(l, families=0x21, confirm=True)
        self.assertEqual(l.sent[0], bytes([kr.PSC_START, 0x21, kr.PSC_FLAG_CONFIRM]))
        kr.pdspec_start(l, confirm=True)
        self.assertEqual(l.sent[1], bytes([kr.PSC_START, 0, kr.PSC_FLAG_CONFIRM]))
        kr.pdspec_start(l, families=0x02, confirm=True, full20=True)
        self.assertEqual(l.sent[2], bytes([kr.PSC_START, 0x02, kr.PSC_FLAG_CONFIRM | kr.PSC_FLAG_FULL20]))
        self.assertEqual(kr.PSC_FLAG_FULL20, 2)
        with self.assertRaises(SystemExit):
            kr.pdspec_start(FakeLink(), confirm=False)
        with self.assertRaises(SystemExit):
            kr.pdspec_start(FakeLink(), families=0x40, confirm=True)

    def test_read_blob_chunks(self):
        l = FakeLink(bytes(range(100)))
        self.assertEqual(kr.pdspec_read_blob(l), bytes(range(100)))
        self.assertEqual([int.from_bytes(s[1:3], "little") for s in l.sent], [0, 48, 96])

    def test_stop(self):
        l = FakeLink()
        kr.pdspec_stop(l)
        self.assertEqual(l.sent[0], bytes([kr.PSC_STOP]))


if __name__ == "__main__":
    unittest.main()
