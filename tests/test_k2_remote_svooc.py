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


def summary(stage=0, refusal=0, abort=0, accepted=False, exit_ok=False, kept=0, total=0, vmax=5100, vexit=0, usb_a=False, cause=None):
    p = bytearray(kr.SVOOC_SUMMARY_BYTES + (0 if cause is None else 1))   # cause None = the older 35-byte layout
    if cause is not None:
        p[kr.SVOOC_SUMMARY_CAUSE] = cause
    p[0], p[1], p[2], p[3] = stage, refusal, abort, 1
    p[4] = (1 if accepted else 0) | (2 if stage == 6 else 0) | (4 if exit_ok else 0) | (16 if usb_a else 0)
    p[18:20] = total.to_bytes(2, "little")
    p[20] = kept
    for off, v in ((21, 20000), (23, 4321), (25, 5100), (27, vmax), (29, vexit), (31, 3)):
        p[off:off + 2] = v.to_bytes(2, "little")
    return bytes(p)


def ext_summary(i86_first=5, i86_esc=5, r82=0x51, vtarget=0, esc_at=0, escape=0, esc_t=0, **kw):
    """The 45-byte summary: the base 36 bytes (cause byte included) and the responder parameters."""
    return summary(cause=0, **kw) + bytes([i86_first, i86_esc, r82]) + vtarget.to_bytes(2, "little") + bytes([esc_at, escape]) + \
        esc_t.to_bytes(2, "little")


def log_page(rows, first):
    """What the firmware answers to [5, 3, first]: kept(2) from(2) n(1) and up to 7 rows of cmd word t_ms vbus_mv."""
    page = rows[first:first + 7]
    out = bytearray(len(rows).to_bytes(2, "little") + first.to_bytes(2, "little") + bytes([len(page)]))
    for cmd, word, t_ms, vbus in page:
        out += bytes([cmd]) + word.to_bytes(2, "little") + t_ms.to_bytes(2, "little") + vbus.to_bytes(2, "little")
    return bytes(out)


class Link:
    """Answers op 5 from a script of status summaries and a list of pair rows; records every request."""

    def __init__(self, statuses, start_status=kr.STATUS_OK, start_payload=None, rows=None, timing=None):
        self.sent, self.statuses, self.rows = [], list(statuses), rows or []
        self.start_status, self.start_payload = start_status, start_payload
        self.timing = timing   # status page 1; None = a firmware without it (BadArg)

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
        assert payload[1] == kr.SVOOC_STATUS and payload[2] in (0, 1)
        if payload[2] == 1:
            if self.timing is None:
                raise kr.RemoteError(kr.STATUS_BAD_ARG, cmd, b"")
            return kr.Response(cmd, 1, 0, self.timing)
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
        self.assertIn("far-sink / inline check waived (USB-A)", kr.render("fc", r))
        # no --confirm: the waiver alone sends nothing
        link = Link([summary()])
        with self.assertRaises(kr.FcRefused):
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--usb-a"), sleep=lambda s: None)
        self.assertEqual(link.sent, [])

    def test_defaults_send_the_four_byte_start(self):
        link = Link([summary(stage=6)])
        kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm"), sleep=lambda s: None)
        self.assertEqual(link.sent[0], (kr.CMD_FC, bytes([5, 1, 1, 0])))

    def test_parameters_send_the_ten_byte_start(self):
        link = Link([ext_summary(stage=6, i86_first=0x0F, i86_esc=0x23, vtarget=9000, esc_at=4, escape=3, esc_t=4012)])
        a = args("svooc-trigger", "--confirm", "--cap", "9", "--i86-first", "0xF", "--i86", "35",
                 "--vtarget", "9000", "--i86-at", "4")
        r = kr.cmd_fc_svooc(link, a, sleep=lambda s: None)
        # confirm, cap 9, i86_first 0x0F, i86_esc 0x23, r82 default 0x51, vtarget 9000 = 0x2328 little endian, escape at 4 s
        self.assertEqual(link.sent[0], (kr.CMD_FC, bytes([5, 1, 1, 9, 0x0F, 0x23, 0x51, 0x28, 0x23, 4])))
        s = r["svooc"]
        self.assertEqual(s["params"], {"i86_first": "0x00F", "i86_esc": "0x023", "r82": "0x051", "vtarget_mv": 9000,
                                       "esc_at_s": 4})
        self.assertEqual((s["escape"], s["escape_t_ms"]), ("got 86", 4012))
        text = kr.render("fc", r)
        self.assertIn("closed loop to 9000 mV", text)
        self.assertIn("escape at 4 s (0x07F at 4012 ms), then 0x86 answered 0x023: got 86", text)

    def test_single_parameter_fills_the_rest_with_defaults(self):
        link = Link([summary(stage=6)])
        kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--r82", "0x40"), sleep=lambda s: None)
        self.assertEqual(link.sent[0][1], bytes([5, 1, 1, 0, 5, 5, 0x40, 0, 0, 0]))

    def test_parameter_ranges_and_conflicts(self):
        for bad in (("--i86-first", "0"), ("--i86-first", "36"), ("--i86", "0x24"), ("--r82", "0x7F"),
                    ("--vtarget", "11001"), ("--i86-at", "0"), ("--i86-at", "21")):
            with self.subTest(bad=bad), self.assertRaises(SystemExit):
                args("svooc-trigger", "--confirm", *bad)
        for argv in (("--i86", "9"), ("--vtarget", "9000", "--r82", "0x40")):   # nothing is sent
            link = Link([summary()])
            with self.subTest(argv=argv), self.assertRaises(kr.FcRefused):
                kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", *argv), sleep=lambda s: None)
            self.assertEqual(link.sent, [])

    def test_older_summary_has_no_parameters(self):
        s = kr.decode_svooc_summary(summary(stage=6))
        self.assertNotIn("params", s)
        s["pairs"], s["pairs_kept"] = [], 0
        self.assertNotIn("responder:", kr.render_svooc(s))

    def test_log_header_shows_the_parameters_and_escape(self):
        rows = [(0x82, 0x051, 100, 9000), (0x82, 0x07F, 143, 9100), (0x86, 0x005, 186, 9200)]
        link = Link([ext_summary(stage=6, r82=0x30, esc_at=2, escape=4, esc_t=143)], rows=rows)
        r = kr.cmd_fc_svooc(link, args("svooc-log"))
        self.assertEqual(r["svooc_log"]["escape"], "no 86")
        text = kr.render("fc", r)
        self.assertIn("constant 0x030", text)
        self.assertIn(": no 86", text)
        self.assertIn("    1    143 ms  9.10 V  82>07F", text)

    def test_diagnostics_decode_and_render(self):
        """Bytes 45..53: wire edges, last edge (ms after the wake), longest poll gap, the Black Box recording."""
        p = ext_summary(stage=6, kept=0) + (5000).to_bytes(2, "little") + (3351).to_bytes(2, "little") + \
            (212).to_bytes(2, "little") + (4096).to_bytes(2, "little") + bytes([1])
        s = kr.decode_svooc_summary(p)
        self.assertEqual(s["diag"], {"edges": 5000, "last_edge_ms": 3351, "poll_gap_max_ms": 212, "rec_edges": 4096,
                                     "rec_end": "full"})
        s["pairs"], s["pairs_kept"] = [], 0
        self.assertIn("  diag: 5000 edges to the wire, the last at 3351 ms after the wake; longest main-loop gap 212 ms; "
                      "Black Box recording 4096 edges, ended full", kr.render_svooc(s))
        older = kr.decode_svooc_summary(ext_summary(stage=6))
        self.assertNotIn("diag", older)
        older["pairs"], older["pairs_kept"] = [], 0
        self.assertNotIn("diag:", kr.render_svooc(older))

    def test_recording_ends_follow_the_firmware(self):
        text = (ROOT / "include/k2/fastcharge/edges.hpp").read_text()
        ends = re.search(r"enum class End : std::uint8_t \{([^}]*)\}", text).group(1)
        names = [w.split("=")[0].strip().lower() for w in ends.split(",")]
        self.assertEqual(names, ["none", "full", "idle", "detach", "timeout", "cancel", "storm"])
        self.assertEqual(kr.SVOOC_REC_ENDS, ["recording", "full", "idle", "detach", "timeout", "cancelled", "storm"])

    def test_waiver_refused_with_pd_names_the_reason(self):
        link = Link([summary()], start_status=kr.STATUS_BUSY, start_payload=summary(refusal=11))
        with self.assertRaises(kr.FcRefused) as cm:
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--usb-a"), sleep=lambda s: None)
        self.assertIn("USB-A waiver but PD is active", str(cm.exception))

    def test_inline_refusal_names_the_cause(self):
        inline = kr.SVOOC_REFUSALS.index("inline mode")
        link = Link([summary()], start_status=kr.STATUS_BUSY, start_payload=summary(refusal=inline, cause=5))
        with self.assertRaises(kr.FcRefused) as cm:
            kr.cmd_fc_svooc(link, args("svooc-trigger", "--confirm", "--usb-a"), sleep=lambda s: None)
        self.assertIn("inline mode (cause: far rd)", str(cm.exception))
        # the older 35-byte summary has no cause: the plain name
        s = kr.decode_svooc_summary(summary(refusal=inline))
        self.assertEqual((s["refusal"], s["inline_cause"]), ("inline mode", "none"))
        # a cause byte next to a refusal that is not the inline one adds nothing
        self.assertEqual(kr.decode_svooc_summary(summary(refusal=10, cause=1))["refusal"], "no RAM: USB drive enabled")

    def test_inline_abort_names_the_cause(self):
        sink = kr.SVOOC_ABORTS.index("sink appeared")
        s = kr.decode_svooc_summary(summary(stage=6, abort=sink, cause=1))
        self.assertEqual((s["abort"], s["inline_cause"]), ("sink appeared (cause: load)", "load"))
        s["pairs"], s["pairs_kept"] = [], 0
        self.assertIn("inline latch cause: load", kr.render_svooc(s))

    def test_full_log_is_paged_seven_at_a_time(self):
        rows = [(i & 255, i, i * 25, 5000 + i) for i in range(100)]
        link = Link([summary(stage=6, kept=255, total=100)], rows=rows)
        r = kr.cmd_fc_svooc(link, args("svooc-log"))
        log = r["svooc_log"]
        self.assertEqual((log["kept"], log["start"], len(log["pairs"])), (100, 0, 100))
        self.assertEqual(log["pairs"][99], {"i": 99, "t_ms": 2475, "vbus_mv": 5099, "cmd": "0x63", "word": "0x063"})
        self.assertEqual(len(link.sent), 16)   # ceil(100 / 7) pages, then one status read for the header
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
        self.assertEqual(len(link.sent), 2)   # the page, then the status for the header

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

    def test_timing_page_decode_and_render(self):
        """Status page 1: the main loop's worst stage since the wake (when, how long, the ms clock's advance) and who let
        D- go after the replies. A gap far longer than the clock moved = interrupts were masked."""
        ui = kr.STAGE_NAMES.index("ui")
        page = (169123).to_bytes(4, "little") + (3405).to_bytes(2, "little") + bytes([ui]) + (168).to_bytes(2, "little") + \
            (70).to_bytes(2, "little") + (1).to_bytes(2, "little") + (0).to_bytes(2, "little")
        link = Link([summary(stage=6)], timing=page)
        s = kr.cmd_fc_svooc(link, args("svooc-status"))["svooc"]
        self.assertEqual(link.sent, [(kr.CMD_FC, bytes([5, 0, 0])), (kr.CMD_FC, bytes([5, 0, 1]))])
        self.assertEqual(s["timing"], {"stall_us": 169123, "stall_at_ms": 3405, "stall_stage": "ui", "stall_ticked_ms": 168,
                                       "rel_timer": 70, "rel_loop": 1, "rel_edge": 0})
        text = kr.render_svooc(s)
        self.assertIn("  timing: longest main-loop stage ui 169.1 ms at 3405 ms after the wake (ms clock +168); "
                      "D- let go by the timer 70, the main loop 1, the next frame 0", text)
        self.assertNotIn("masked", text)
        masked = page[:7] + (2).to_bytes(2, "little") + page[9:]
        s = kr.cmd_fc_svooc(Link([summary(stage=6)], timing=masked), args("svooc-status"))["svooc"]
        self.assertIn("(ms clock +2: interrupts masked ~167 ms)", kr.render_svooc(s))
        older = kr.cmd_fc_svooc(Link([summary(stage=6)]), args("svooc-status"))["svooc"]   # BadArg: no page 1
        self.assertNotIn("timing", older)
        self.assertNotIn("timing:", kr.render_svooc(older))

    def test_stage_names_follow_the_firmware(self):
        text = (ROOT / "include/k2/core/stage_meter.hpp").read_text()
        body = re.search(r"constexpr const char\* name\(Id id\) \{(.*?)\n\}", text, re.S).group(1)
        self.assertEqual(re.findall(r'return "([^"]*)";', body)[:-1], kr.STAGE_NAMES)

    def test_status_and_stop_commands(self):
        link = Link([summary(stage=6)])
        kr.cmd_fc_svooc(link, args("svooc-status"))
        self.assertEqual(link.sent, [(kr.CMD_FC, bytes([5, 0, 0])), (kr.CMD_FC, bytes([5, 0, 1]))])
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

    def test_inline_causes_follow_the_firmware(self):
        text = (ROOT / "include/k2/pd/inline_cause.hpp").read_text()
        body = re.search(r"constexpr const char\* inline_cause_name\(InlineCause c\) \{(.*?)\n\}", text, re.S).group(1)
        self.assertEqual(re.findall(r'return "([^"]*)";', body)[:-1], kr.INLINE_CAUSES)

    def test_stages_and_phases(self):
        run = (ROOT / "include/k2/fastcharge/svooc_run.hpp").read_text()
        stages = re.search(r"enum class Stage : std::uint8_t \{(.*?)\};", run, re.S).group(1)
        self.assertEqual([w.lower() for w in re.findall(r"^\s*(\w+),", stages, re.M)], kr.SVOOC_STAGES)
        script = (ROOT / "include/k2/fastcharge/svooc_script.hpp").read_text()
        phases = re.search(r"enum class Phase : std::uint8_t \{([^}]*)\}", script).group(1)
        self.assertEqual([w.strip().lower() for w in phases.split(",")], kr.SVOOC_PHASES)


if __name__ == "__main__":
    unittest.main()
