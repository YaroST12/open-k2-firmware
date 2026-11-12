# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for `k2_remote fc svooc-trigger / svooc-status / svooc-stop` (CMD_FC 0x32, op 5): the confirm rule on
the host, the refusal text from the Busy reply's summary, the poll that holds the lease until the run is done, Ctrl-C
sending stop, and the name lists matching the firmware's (k2/fastcharge/svooc_run.hpp)."""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


def summary(stage=0, refusal=0, abort=0, accepted=False, exit_ok=False, kept=0, total=0, vmax=5100, vexit=0, usb_a=False):
    p = bytearray(kr.SVOOC_SUMMARY_BYTES)
    p[0], p[1], p[2], p[3] = stage, refusal, abort, 1
    p[4] = (1 if accepted else 0) | (2 if stage == 6 else 0) | (4 if exit_ok else 0) | (16 if usb_a else 0)
    p[18:20] = total.to_bytes(2, "little")
    p[20] = kept
    for off, v in ((21, 20000), (23, 4321), (25, 5100), (27, vmax), (29, vexit), (31, 3)):
        p[off:off + 2] = v.to_bytes(2, "little")
    return bytes(p)


def log_page(rows, first):
    """What the firmware answers to [5, 3, first]: kept(2) from(2) n(1) and up to 7 rows of cmd word t_ms vbus_mv."""
    page = rows[first:first + 7]
    out = bytearray(len(rows).to_bytes(2, "little") + first.to_bytes(2, "little") + bytes([len(page)]))
    for cmd, word, t_ms, vbus in page:
        out += bytes([cmd]) + word.to_bytes(2, "little") + t_ms.to_bytes(2, "little") + vbus.to_bytes(2, "little")
    return bytes(out)


class Link:
    """Answers op 5 from a script of status summaries and a list of pair rows; records every request."""

    def __init__(self, statuses, start_status=kr.STATUS_OK, start_payload=None, rows=None):
        self.sent, self.statuses, self.rows = [], list(statuses), rows or []
        self.start_status, self.start_payload = start_status, start_payload

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, bytes(payload)))
        assert cmd == kr.CMD_FC and payload[0] == kr.FC_OP_SVOOC
        if payload[1] == kr.SVOOC_START:
            if self.start_status != kr.STATUS_OK:
                raise kr.RemoteError(self.start_status, cmd, self.start_payload or b"")
            return kr.Response(cmd, 1, 0, summary(stage=1))
        if payload[1] == kr.SVOOC_STOP:
            return kr.Response(cmd, 1, 0, summary(stage=5, abort=11))
        if payload[1] == kr.SVOOC_LOG:
            return kr.Response(cmd, 1, 0, log_page(self.rows, payload[2] | payload[3] << 8))
        assert payload[1] == kr.SVOOC_STATUS and payload[2] == 0
        body = self.statuses.pop(0) if len(self.statuses) > 1 else self.statuses[0]
        return kr.Response(cmd, 1, 0, body)


def args(*argv):
    return kr.build_parser().parse_args(["fc", *argv])


class SvoocTrigger(unittest.TestCase):
    def test_without_confirm_nothing_is_sent(self):
        link = Link([summary()])
        with self.assertRaises(kr.FcRefused):
            kr.cmd_fc_svooc(link, args("svooc-trigger"), sleep=lambda s: None)
        self.assertEqual(link.sent, [])

    def test_cap_range(self):
        self.assertEqual(args("svooc-trigger", "--confirm", "--cap", "12").cap, 12)
        self.assertEqual(args("svooc-trigger", "--confirm").cap, 0)
        with self.assertRaises(SystemExit):
            args("svooc-trigger", "--confirm", "--cap", "21")

    def test_refusal_names_the_real_reason(self):
        link = Link([summary()], start_status=kr.STATUS_BUSY, start_payload=summary(refusal=10))
        with self.assertRaises(kr.FcRefused) as cm:
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm"), sleep=lambda s: None)
        self.assertIn("no RAM: USB drive enabled", str(cm.exception))
        self.assertEqual(link.sent, [(kr.CMD_FC, bytes([5, 1, 1, 0]))])

    def test_polls_until_done_then_reads_the_pairs(self):
        done = summary(stage=6, abort=1, accepted=True, kept=18, total=31, vmax=12001, vexit=5000, exit_ok=True)
        rows = ([(0xA8, 0x00D, 5, 5100)] + [(0x98, 0x073, 30 + 25 * i, 5110) for i in range(15)] +
                [(0x82, 0x051, 500, 9000), (0x84, kr.SVOOC_NO_WORD, 525, 0)])
        link = Link([summary(stage=2), summary(stage=4), summary(stage=5), done], rows=rows)
        sleeps = []
        r = kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--cap", "15"), sleep=sleeps.append)
        self.assertEqual(link.sent[0], (kr.CMD_FC, bytes([5, 1, 1, 15])))
        self.assertEqual(len(sleeps), 4)
        s = r["svooc"]
        self.assertEqual((s["stage"], s["abort"], s["accepted"], s["vbus_max_mv"]), ("done", "VBUS over 12 V", True, 12001))
        self.assertEqual(len(s["pairs"]), 18)
        self.assertEqual(s["pairs"][0], {"i": 0, "t_ms": 5, "vbus_mv": 5100, "cmd": "0xA8", "word": "0x00D"})
        self.assertEqual(s["pairs"][-1], {"i": 17, "t_ms": 525, "vbus_mv": 0, "cmd": "0x84", "word": None})
        self.assertFalse(s["usb_a_waiver"])
        text = kr.render("fc", r)
        self.assertIn("A8>00D", text)
        self.assertIn("84>---", text)
        self.assertIn("ended by VBUS over 12 V", text)

    def test_usb_a_waiver_flag_and_report(self):
        self.assertTrue(args("svooc-trigger", "--confirm", "--usb-a").usb_a)
        self.assertFalse(args("svooc-trigger", "--confirm").usb_a)
        link = Link([summary(stage=6, abort=10, usb_a=True)])
        r = kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--usb-a", "--cap", "9"), sleep=lambda s: None)
        self.assertEqual(link.sent[0], (kr.CMD_FC, bytes([5, 1, 5, 9])))   # confirm | usb-a
        self.assertTrue(r["svooc"]["usb_a_waiver"])
        self.assertIn("far-sink check waived (USB-A)", kr.render("fc", r))
        # no --confirm: the waiver alone sends nothing
        link = Link([summary()])
        with self.assertRaises(kr.FcRefused):
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--usb-a"), sleep=lambda s: None)
        self.assertEqual(link.sent, [])

    def test_waiver_refused_with_pd_names_the_reason(self):
        link = Link([summary()], start_status=kr.STATUS_BUSY, start_payload=summary(refusal=11))
        with self.assertRaises(kr.FcRefused) as cm:
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--usb-a"), sleep=lambda s: None)
        self.assertIn("USB-A waiver but PD is active", str(cm.exception))

    def test_full_log_is_paged_seven_at_a_time(self):
        rows = [(i & 255, i, i * 25, 5000 + i) for i in range(100)]
        link = Link([summary(stage=6, kept=255, total=100)], rows=rows)
        r = kr.cmd_fc_svooc(link, args("svooc-log"))
        log = r["svooc_log"]
        self.assertEqual((log["kept"], log["start"], len(log["pairs"])), (100, 0, 100))
        self.assertEqual(log["pairs"][99], {"i": 99, "t_ms": 2475, "vbus_mv": 5099, "cmd": "0x63", "word": "0x063"})
        self.assertEqual(len(link.sent), 15)   # ceil(100 / 7) pages
        self.assertEqual(link.sent[0][1], bytes([5, 3, 0, 0]))
        self.assertEqual(link.sent[1][1], bytes([5, 3, 7, 0]))
        self.assertIn("   99   2475 ms  5.10 V  63>063", kr.render("fc", r))

    def test_log_from_and_count(self):
        rows = [(i & 255, i, i, 5000) for i in range(300)]
        link = Link([summary()], rows=rows)
        r = kr.cmd_fc_svooc(link, args("svooc-log", "--start", "259", "--count", "10"))
        self.assertEqual([p["i"] for p in r["svooc_log"]["pairs"]], list(range(259, 269)))
        self.assertEqual(link.sent[0][1], bytes([5, 3, 3, 1]))   # index 259 = 0x0103: the high byte rides along
        link = Link([summary()], rows=rows[:5])
        r = kr.cmd_fc_svooc(link, args("svooc-log", "--start", "9"))   # past the end: empty, not an error
        self.assertEqual((r["svooc_log"]["kept"], r["svooc_log"]["pairs"]), (5, []))
        self.assertEqual(len(link.sent), 1)

    def test_exit_still_high_is_reported(self):
        link = Link([summary(stage=6, abort=10, vmax=9000, vexit=9000, exit_ok=False)])
        r = kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm"), sleep=lambda s: None)
        self.assertIn("STILL ABOVE 5.5 V", kr.render("fc", r))

    def test_ctrl_c_sends_stop_once_and_waits_for_the_end(self):
        link = Link([summary(stage=4), summary(stage=5), summary(stage=6, abort=11)])
        calls = []

        def sleep(s):
            calls.append(s)
            if len(calls) == 1:
                raise KeyboardInterrupt

        r = kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm"), sleep=sleep)
        stops = [p for c, p in link.sent if p[:2] == bytes([5, 2])]
        self.assertEqual(len(stops), 1)
        self.assertEqual(r["svooc"]["abort"], "stopped")

    def test_status_and_stop_commands(self):
        link = Link([summary(stage=6)])
        kr.cmd_fc_svooc(link, args("svooc-status"))
        self.assertEqual(link.sent, [(kr.CMD_FC, bytes([5, 0, 0]))])
        link = Link([summary(stage=6)])
        kr.cmd_fc_svooc(link, args("svooc-stop"))
        self.assertEqual(link.sent[0], (kr.CMD_FC, bytes([5, 2])))


class NamesMatchFirmware(unittest.TestCase):
    def names(self, enum):
        text = (ROOT / "include/k2/fastcharge/svooc_run.hpp").read_text()
        body = re.search(r"constexpr const char\* name\(" + enum + r" \w+\) \{(.*?)\n\}", text, re.S).group(1)
        return re.findall(r'return "([^"]*)";', body)[:-1]   # the last return is the "?" fallback

    def test_refusals(self):
        self.assertEqual(self.names("Refusal"), kr.SVOOC_REFUSALS)

    def test_aborts(self):
        self.assertEqual(self.names("Abort"), kr.SVOOC_ABORTS)

    def test_contracts_and_summary_decode(self):
        self.assertEqual(self.names("Contract"), kr.SVOOC_CONTRACTS)
        p = bytearray(summary())
        p[33], p[34] = 1, 0
        s = kr.decode_svooc_summary(bytes(p))
        self.assertEqual((s["contract_start"], s["contract_end"]), ("own 5 V", "none"))

    def test_stages_and_phases(self):
        run = (ROOT / "include/k2/fastcharge/svooc_run.hpp").read_text()
        stages = re.search(r"enum class Stage : std::uint8_t \{(.*?)\};", run, re.S).group(1)
        self.assertEqual([w.lower() for w in re.findall(r"^\s*(\w+),", stages, re.M)], kr.SVOOC_STAGES)
        script = (ROOT / "include/k2/fastcharge/svooc_script.hpp").read_text()
        phases = re.search(r"enum class Phase : std::uint8_t \{([^}]*)\}", script).group(1)
        self.assertEqual([w.strip().lower() for w in phases.split(",")], kr.SVOOC_PHASES)


if __name__ == "__main__":
    unittest.main()
