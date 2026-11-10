# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Unit tests for `k2_remote.py sink` (Row B item 12): the Connection page Sink tab,
mirrored field for field from the same remote commands the tab's model reads
(src/ui/ui_connection_page.cpp sink_rows). No device needed: a fake Link serves the
canned CMD_* payloads. The pdtabs wording mirrors include/k2/pd/pd_tabs.hpp."""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr


def rsp(cmd, seq, status, payload=b""):
    return (bytes([0xA6, 0x4B, cmd, seq, status, len(payload)]) + payload).ljust(64, b"\x00")


def state_payload(vbus_mv=9000, current_ma=-3000, pd_valid=True, link_flags=4, num_pdos=3):
    p = bytearray(53)
    p[0] = 16                                   # connection page
    p[1] = 1                                    # active CC1
    p[2] = 2                                    # PD3.x
    p[3] = num_pdos
    p[4] = 1 if pd_valid else 0
    p[8:12] = vbus_mv.to_bytes(4, "little", signed=True)
    p[12:16] = current_ma.to_bytes(4, "little", signed=True)
    p[37] = 1                                   # adc_valid
    p[40] = link_flags                          # inline
    return bytes(p)


def pd_status_payload(pin=0, held=False):
    p = bytearray(18)
    p[0] = 5                                    # ready
    p[2] = 1 if held else 0
    p[4] = 3
    p[5:7] = (9000).to_bytes(2, "little")
    p[7:9] = (3000).to_bytes(2, "little")
    p[17] = pin                                 # pin_rd(): the pinned-Rd bits (self_powered())
    return bytes(p)


def match_payload(caps_seen=1, request_seen=1, flags=0, verdict=7, want_pos=3,
                  want_mv=9000, want_ma=3000, src_max_mv=20000, src_max_w=100, rows=()):
    p = bytearray(19 + len(rows))
    p[0] = caps_seen
    p[1] = request_seen
    p[2] = 1                                    # sink_ok
    p[3] = flags
    p[4] = verdict
    p[5] = len(rows)
    p[6] = want_pos
    p[7:9] = want_mv.to_bytes(2, "little")
    p[9:11] = want_ma.to_bytes(2, "little")
    p[15:17] = src_max_mv.to_bytes(2, "little")
    p[17:19] = src_max_w.to_bytes(2, "little")
    for i, b in enumerate(rows):
        p[19 + i] = b
    return bytes(p)


def pdo_payload(index, num, type_, min_mv=0, max_mv=9000, max_ma=3000, max_mw=27000):
    p = bytearray(19)
    p[0] = 1
    p[1] = 2
    p[2] = num
    p[3] = index
    p[4] = type_                                # PDO_TYPES: 1 fixed, 2 battery, 3 variable, 4 pps, 5 avs
    p[9:11] = min_mv.to_bytes(2, "little")
    p[11:13] = max_mv.to_bytes(2, "little")
    p[13:15] = max_ma.to_bytes(2, "little")
    p[15:19] = max_mw.to_bytes(4, "little", signed=True)
    return bytes(p)


def usb_link_payload(family=3, mbps=5000, dp=False, text="5G"):
    p = bytearray(9 + len(text))
    p[0] = family
    p[2] = 3                                    # negotiated
    p[3] = 2
    p[4] = 8 if dp else 0
    p[5:9] = mbps.to_bytes(4, "little")
    p[9:] = text.encode()
    return bytes(p)


def emarker_payloads(valid=1, vid=0x1234, max_power_w=60, vdos=(0, 0, 0, 0, 2)):
    p0 = bytearray(17)
    p0[0] = valid
    p0[1] = valid
    p0[2:4] = vid.to_bytes(2, "little")
    p0[12] = 3                                  # max_current_a
    p0[13] = 20                                 # max_voltage_v
    p0[14:16] = max_power_w.to_bytes(2, "little")
    p0[16] = len(vdos)
    p1 = b"".join(v.to_bytes(4, "little") for v in vdos)
    p2 = bytes(10)
    return p0, p1, p2


def identify_payload(state=2, sink_pdos=()):
    p = bytearray(10 + 4 * len(sink_pdos))
    p[0] = state                                # done
    p[1] = 6                                    # done step
    p[2] = 0b111111                             # answered
    p[5] = len(sink_pdos)
    for i, raw in enumerate(sink_pdos):
        p[10 + 4 * i:14 + 4 * i] = raw.to_bytes(4, "little")
    return bytes(p)


FIXED_5V3A = (100 << 10) | 300                  # 5 V, 3 A raw fixed PDO
FIXED_9V3A = (180 << 10) | 300                  # 9 V, 3 A raw fixed PDO


def sink_link(pin=0, match=None, pdos=None, usb=None, emarker=None, ident=None, state=None):
    """A fake Link serving the exact call set `sink` is allowed to make."""
    match = match if match is not None else match_payload()
    pdos = pdos if pdos is not None else [pdo_payload(0, 3, 1, max_mv=5000, max_ma=3000, max_mw=15000),
                                          pdo_payload(1, 3, 1, max_mv=9000, max_ma=3000, max_mw=27000),
                                          pdo_payload(2, 3, 1, max_mv=20000, max_ma=5000, max_mw=100000)]
    usb = usb if usb is not None else usb_link_payload()
    emarker = emarker if emarker is not None else emarker_payloads()
    ident = ident if ident is not None else identify_payload(sink_pdos=(FIXED_5V3A, FIXED_9V3A))
    state = state if state is not None else state_payload(num_pdos=len(pdos))
    responses = {
        (kr.CMD_GET_STATE, b""): state,
        (kr.CMD_PD_TRIGGER_STATUS, b""): pd_status_payload(pin=pin),
        (kr.CMD_PD_MATCH, b""): match,
        (kr.CMD_USB_LINK, b""): usb,
        (kr.CMD_GET_EMARKER, b"\x00"): emarker[0],
        (kr.CMD_GET_EMARKER, b"\x01"): emarker[1],
        (kr.CMD_GET_EMARKER, b"\x02"): emarker[2],
        (kr.CMD_IDENTIFY_DEVICE, b"\x00"): ident,      # op 0 = status only, never starts a run
    }
    for i, pdo in enumerate(pdos):
        responses[(kr.CMD_GET_PDOS, bytes([i]))] = pdo

    class Link:
        sent = []

        def call(self, cmd, payload=b""):
            Link.sent.append((cmd, bytes(payload)))
            key = (cmd, bytes(payload))
            if key not in responses:
                raise AssertionError(f"unexpected remote call {cmd:#04x} {bytes(payload).hex()}")
            return kr.parse_response(rsp(cmd, 1, 0, responses[key]))

    return Link()


def run_sink(link):
    a = kr.build_parser().parse_args(["sink"])
    return kr.COMMANDS["sink"](link, a)


class SinkCommand(unittest.TestCase):
    def test_registered_and_parses(self):
        self.assertIn("sink", kr.COMMANDS)
        a = kr.build_parser().parse_args(["sink"])
        self.assertEqual(a.command, "sink")

    def test_composes_only_existing_commands(self):
        link = sink_link()
        r = run_sink(link)
        self.assertEqual(r["tab"], "sink")
        cmds = {c for c, _ in link.sent}
        self.assertEqual(cmds, {kr.CMD_GET_STATE, kr.CMD_PD_TRIGGER_STATUS, kr.CMD_PD_MATCH,
                                kr.CMD_GET_PDOS, kr.CMD_USB_LINK, kr.CMD_GET_EMARKER, kr.CMD_IDENTIFY_DEVICE})
        # the identify call must be the read-only status op, never a start
        self.assertIn((kr.CMD_IDENTIFY_DEVICE, b"\x00"), link.sent)

    def test_request_row_from_match_and_pdos(self):
        r = run_sink(sink_link())               # want PDO3 9 V 3 A, PDO 3 is fixed
        req = r["request"]
        self.assertEqual((req["pos"], req["type"], req["mv"], req["ma"], req["mw"]),
                         (3, "fixed", 9000, 3000, 27000))
        self.assertEqual(req["text"], "PDO3 9V 3A")

    def test_request_row_pps_wording(self):
        pdos = [pdo_payload(0, 3, 1, max_mv=5000, max_ma=3000, max_mw=15000),
                pdo_payload(1, 3, 1, max_mv=9000, max_ma=3000, max_mw=27000),
                pdo_payload(2, 3, 4, min_mv=3300, max_mv=11000, max_ma=5000, max_mw=55000)]  # pps
        m = match_payload(want_pos=3, want_mv=8880, want_ma=2050)
        r = run_sink(sink_link(match=m, pdos=pdos))
        self.assertEqual(r["request"]["type"], "pps")
        self.assertEqual(r["request"]["text"], "PPS3 8.88V 2.05A")

    def test_no_request_seen(self):
        r = run_sink(sink_link(match=match_payload(request_seen=0, verdict=1)))
        self.assertIsNone(r["request"])
        self.assertIn("none seen", kr.render("sink", r))

    def test_power_line_mirrors_pdtabs(self):
        # source 100 W, cable 60 W (e-marker), sink asks 27 W -> "27W of 100W"
        r = run_sink(sink_link())
        self.assertEqual(r["pwr"], "27W of 100W")
        # sink asks 65 W at 20 V: the 60 W cable holds it down -> "60W by cable"
        m = match_payload(want_pos=3, want_mv=20000, want_ma=3250, src_max_w=100)
        r = run_sink(sink_link(match=m))
        self.assertEqual(r["pwr"], "60W by cable")

    def test_data_line_mirrors_pdtabs(self):
        # cable VDO bits 2 -> 10G, the device link is 5G -> "5G of 10G"
        r = run_sink(sink_link())
        self.assertEqual(r["data"], "5G of 10G")
        r = run_sink(sink_link(usb=usb_link_payload(mbps=10000, dp=True, text="10G")))
        self.assertEqual(r["data"], "10G +DP")  # +DP stays even when the rates agree
        r = run_sink(sink_link(emarker=emarker_payloads(vdos=(0, 0, 0, 0, 1))))   # cable 5G, device 5G
        self.assertIsNone(r["data"])            # plain rate repeats the USB row: dropped like the tab does

    def test_caps_from_identify_sink_pdos(self):
        r = run_sink(sink_link())
        self.assertEqual(r["caps"], ["5V 3A", "9V 3A"])
        self.assertEqual(r["caps_src"], "from Identify")
        r = run_sink(sink_link(ident=identify_payload(state=2, sink_pdos=())))
        self.assertEqual(r["caps"], [])
        self.assertIn("not seen", kr.render("sink", r))

    def test_k2_sink_case(self):
        r = run_sink(sink_link(pin=1))
        self.assertTrue(r["k2_sink"])
        self.assertTrue(r["k2_sink_ok"])
        text = kr.render("sink", r)
        self.assertIn("ID: K2", text)
        self.assertIn("K2 (sink mode OK)", text)
        r = run_sink(sink_link(pin=2))
        self.assertTrue(r["k2_sink"])
        self.assertFalse(r["k2_sink_ok"])
        self.assertIn("K2 (sink mode PLUS)", kr.render("sink", r))

    def test_gaps_are_named_not_faked(self):
        r = run_sink(sink_link())
        # fields the Sink tab renders from the firmware model that no remote command exposes today
        self.assertIsNone(r["id"]["sniffed"])       # sniffed sink VDM identity
        self.assertIsNone(r["state_line"])          # contract state + RDO flags
        self.assertIsNone(r["proto"])               # sink SVIDs
        self.assertIsNone(r["no_pd_verdict"])       # Row C verdict needs the wait-kind classifier
        joined = " ".join(r["not_exposed"])
        for gap in ("identity", "state", "SVIDs", "verdict"):
            self.assertIn(gap, joined)
        self.assertEqual(r["flags"], [])            # match flags are the exposed part of the row
        m = match_payload(flags=1 | 4)              # mismatch-bit | current-limited
        r = run_sink(sink_link(match=m))
        self.assertEqual(r["flags"], ["mismatch-bit", "current-limited"])

    def test_text_render_rows(self):
        r = run_sink(sink_link())
        text = kr.render("sink", r)
        self.assertIn("Request: PDO3 9V 3A", text)
        self.assertIn("Pwr: 27W of 100W", text)
        self.assertIn("Data: 5G of 10G", text)
        self.assertIn("USB: 5G", text)
        self.assertIn("Caps: 5V 3A 9V 3A", text)
        self.assertIn("ID: unknown", text)          # sniffed identity not exposed; asked state shown
        self.assertIn("done", text)

    def test_no_pd_contract_notes_budget_gap(self):
        r = run_sink(sink_link(match=match_payload(caps_seen=0, request_seen=0, verdict=0),
                               state=state_payload(pd_valid=False, vbus_mv=5000)))
        self.assertIsNone(r["pwr"])
        self.assertIn("typec_ma", " ".join(r["not_exposed"]))


class PdtabsMirror(unittest.TestCase):
    """The host copies of pdtabs::power_line / data_line / cable_mbps must word like the firmware."""

    def test_power_line(self):
        self.assertEqual(kr._power_line({"source": 100, "cable": -1, "sink": 18}, False), "18W of 100W")
        self.assertEqual(kr._power_line({"source": 100, "cable": 60, "sink": 65}, False), "60W by cable")
        self.assertEqual(kr._power_line({"source": 100, "cable": 60, "sink": 65}, True), "60W by cable?")
        self.assertEqual(kr._power_line({"source": 100, "cable": 100, "sink": 100}, False), "100W")
        self.assertEqual(kr._power_line({"source": -1, "cable": -1, "sink": -1}, False), "?")
        self.assertEqual(kr._power_line({"source": -1, "cable": 60, "sink": -1}, False), "60W")

    def test_data_line(self):
        self.assertEqual(kr._data_line({"cable": 10000, "sink": 5000}, False), "5G of 10G")
        self.assertEqual(kr._data_line({"cable": 5000, "sink": 10000}, False), "5G by cable")
        self.assertEqual(kr._data_line({"cable": 10000, "sink": -1}, False), "10G cable")
        self.assertEqual(kr._data_line({"cable": 480, "sink": 480}, False), "480M")
        self.assertEqual(kr._data_line({"cable": -1, "sink": -1}, False), "?")
        self.assertEqual(kr._data_line({"cable": 480, "sink": 480}, True), "480M +DP")

    def test_cable_mbps(self):
        self.assertEqual(kr._cable_mbps(2, 0x8087), 40000)   # Thunderbolt cable from Gen2 up
        self.assertEqual(kr._cable_mbps(1, 0), 5000)
        self.assertEqual(kr._cable_mbps(2, 0), 10000)
        self.assertEqual(kr._cable_mbps(0, 0), 480)


class SourceCableTabCoverage(unittest.TestCase):
    """Standing rule 2d: every Source/Cable tab row is reachable through existing commands."""

    def test_commands_cover_source_and_cable_rows(self):
        # Source tab: ID -> identity, PD/Fixed/PPS/AVS/Used -> pdos + match, Type-C -> state + match,
        # Legacy -> fc, USB/Alt/Proto -> usblink + identity svids. Cable tab: every row -> emarker.
        for name in ("pdos", "identity", "emarker", "match", "usblink", "state", "fc"):
            self.assertIn(name, kr.COMMANDS)


if __name__ == "__main__":
    unittest.main()
