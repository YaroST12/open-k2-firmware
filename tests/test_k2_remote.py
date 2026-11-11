# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Unit tests for the k2_remote framing and payload decoders (no device needed).

The byte vectors are the same literals as tests/test_remote_proto.cpp, so both ends of the
protocol are pinned to the layout documented in include/k2/core/remote_proto.hpp.
"""
import contextlib
import io
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_flasher
import k2_remote as kr


def rsp(cmd, seq, status, payload=b""):
    """A response frame as the firmware lays it out."""
    return (bytes([0xA6, 0x4B, cmd, seq, status, len(payload)]) + payload).ljust(64, b"\x00")


class CommandIds(unittest.TestCase):
    def test_ids_are_unique_and_match_the_firmware_table(self):
        ids = {n: v for n, v in vars(kr).items() if n.startswith("CMD_") and n != "CMD_SCREEN_DATA"
               and n != "CMD_RIPPLE_DATA" and isinstance(v, int)}
        self.assertEqual(len(set(ids.values())), len(ids), ids)
        self.assertEqual((kr.CMD_DEBUG_FAULT, kr.CMD_FC, kr.CMD_DFU_ENTER), (0x31, 0x32, 0x30))


class PackRequest(unittest.TestCase):
    def test_button_request_bytes(self):
        frame = kr.pack_request(kr.CMD_BUTTON, 7, bytes([2, 100, 0]))
        self.assertEqual(len(frame), 64)
        self.assertEqual(frame[:8], bytes([0xA5, 0x4B, 0x02, 0x07, 0x03, 2, 100, 0]))
        self.assertEqual(frame[8:], bytes(56))

    def test_empty_payload(self):
        frame = kr.pack_request(kr.CMD_PING, 255, b"")
        self.assertEqual(frame[:5], bytes([0xA5, 0x4B, 0x01, 0xFF, 0x00]))

    def test_oversize_payload_rejected(self):
        kr.pack_request(kr.CMD_PING, 0, bytes(59))
        with self.assertRaises(ValueError):
            kr.pack_request(kr.CMD_PING, 0, bytes(60))

    def test_seq_wraps_to_a_byte(self):
        self.assertEqual(kr.pack_request(kr.CMD_PING, 256, b"")[3], 0)

    def test_not_a_bootloader_frame(self):
        frame = kr.pack_request(kr.CMD_PING, 1, b"")
        self.assertEqual(frame[:2], b"\xA5\x4B")
        self.assertNotEqual(frame[:2], b"\xFF\x55")           # k2_flasher.build_frame magic
        self.assertNotEqual(frame[:2], k2_flasher.build_frame(1)[:2])


class DebugPdMsg(unittest.TestCase):
    def test_parse(self):
        sop, header, objects = kr.parse_pd_msg("4f10 01a000ff", "sop1")
        self.assertEqual((sop, header, objects), (1, b"\x4f\x10", b"\x01\xa0\x00\xff"))
        self.assertEqual(kr.parse_pd_msg("0x4f:10:01:a0:00:ff")[0], 0)

    def test_parse_errors(self):
        for bad in ("zz", "4f", "4f1001", "4f10"):
            with self.assertRaises(ValueError):
                kr.parse_pd_msg(bad)

    def test_parser_and_request(self):
        a = kr.build_parser().parse_args(["debug", "pd-msg", "4110", "--sop", "sop2"])
        self.assertEqual((a.debug_cmd, a.hex, a.sop), ("pd-msg", "4110", "sop2"))
        frame = kr.pack_request(kr.CMD_DEBUG_PD_MSG, 3, bytes([2]) + b"\x41\x00")
        self.assertEqual(frame[:8], bytes([0xA5, 0x4B, 0x23, 0x03, 0x03, 2, 0x41, 0x00]))


class DebugPdos(unittest.TestCase):
    def test_request_bytes(self):
        frame = kr.pack_request(kr.CMD_DEBUG_PDOS, 2, bytes([7]))
        self.assertEqual(frame[:6], bytes([0xA5, 0x4B, 0x20, 0x02, 0x01, 7]))

    def test_parser(self):
        a = kr.build_parser().parse_args(["debug", "pdos", "--fake", "11"])
        self.assertEqual((a.command, a.debug_cmd, a.fake), ("debug", "pdos", 11))
        for bad in ("12", "-1"):
            with self.assertRaises(SystemExit):
                kr.build_parser().parse_args(["debug", "pdos", "--fake", bad])

    def test_command_sends_count(self):
        sent = []

        class Link:
            def call(self, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, payload))

        a = kr.build_parser().parse_args(["debug", "pdos", "--fake", "5"])
        self.assertEqual(kr.COMMANDS["debug"](Link(), a), {"fake_pdos": 5})
        self.assertEqual(sent, [(kr.CMD_DEBUG_PDOS, bytes([5]))])
        self.assertIn("5", kr.render("debug", {"fake_pdos": 5}))
        self.assertIn("off", kr.render("debug", {"fake_pdos": 0}))


class ParseResponse(unittest.TestCase):
    def test_valid(self):
        r = kr.parse_response(rsp(2, 9, 0, bytes([2, 100, 0])))
        self.assertEqual((r.cmd, r.seq, r.status), (2, 9, 0))
        self.assertEqual(r.payload, bytes([2, 100, 0]))

    def test_error_status(self):
        r = kr.parse_response(rsp(2, 3, 3))
        self.assertEqual(r.status, kr.STATUS_BAD_ARG)
        self.assertEqual(r.payload, b"")

    def test_log_text_is_not_a_response(self):
        text = b"[SYS] Heartbeat: up=2s presses=0 page=0\n".ljust(64, b"\x00")
        self.assertFalse(kr.is_response(text))
        with self.assertRaises(kr.FrameError):
            kr.parse_response(text)

    def test_wrong_length_rejected(self):
        with self.assertRaises(kr.FrameError):
            kr.parse_response(rsp(1, 1, 0)[:10])

    def test_payload_length_beyond_frame_rejected(self):
        bad = bytearray(rsp(1, 1, 0))
        bad[5] = 59
        with self.assertRaises(kr.FrameError):
            kr.parse_response(bytes(bad))

    def test_bootloader_frame_is_recognised(self):
        boot = k2_flasher.build_frame(2)                    # what the bootloader answers with
        self.assertTrue(kr.is_bootloader_frame(boot))
        self.assertFalse(kr.is_response(boot))
        self.assertFalse(kr.is_bootloader_frame(rsp(1, 1, 0)))


class FlasherMonitorFilter(unittest.TestCase):
    def test_responses_are_not_log_reports(self):
        self.assertFalse(k2_flasher.is_log_report(rsp(1, 1, 0)))
        self.assertTrue(k2_flasher.is_log_report(b"[BOOT] hello\n".ljust(64, b"\x00")))

    def test_empty_and_zero_reports_are_not_logs(self):
        self.assertFalse(k2_flasher.is_log_report(b""))
        self.assertFalse(k2_flasher.is_log_report(bytes(64)))


class Decoders(unittest.TestCase):
    def test_ping(self):
        payload = (bytes([1, 0x04, 0x03, 0x02, 0x01]) + b"abc1234".ljust(24, b"\x00") + b"1.0")
        self.assertEqual(kr.decode_ping(payload),
                         {"proto": 1, "uptime_ms": 0x01020304, "git": "abc1234", "version": "1.0"})

    def test_ping_new_firmware_full_version_and_raw_git(self):
        # `version` is the one version string, `git` the raw describe (24 bytes, full: no NUL in the field)
        payload = (bytes([1, 0, 0, 0, 0]) + b"v1.0-435-g26b245ac-dirty" + b"1.0.0-435-26b245ac-dirty")
        self.assertEqual(kr.decode_ping(payload),
                         {"proto": 1, "uptime_ms": 0, "git": "v1.0-435-g26b245ac-dirty", "version": "1.0.0-435-26b245ac-dirty"})

    def test_ping_old_firmware_still_reads(self):
        # firmware before the version string: version "1.0", git is the bare hash
        payload = bytes([1, 0, 0, 0, 0]) + b"26b245ac".ljust(24, b"\x00") + b"1.0"
        r = kr.decode_ping(payload)
        self.assertEqual((r["version"], r["git"]), ("1.0", "26b245ac"))

    def test_ping_text_shows_the_full_version(self):
        r = {"proto": 1, "uptime_ms": 2000, "git": "v1.0-435-g26b245ac", "version": "1.0.0-435-26b245ac"}
        self.assertEqual(kr.render("ping", r), "K2 firmware 1.0.0-435-26b245ac git v1.0-435-g26b245ac proto 1 uptime 2.0 s")

    def test_ping_keeps_a_dirty_hash(self):
        payload = bytes([1, 0, 0, 0, 0]) + b"cef22c4-dirty".ljust(24, b"\x00") + b"1.0"
        self.assertEqual(kr.decode_ping(payload)["git"], "cef22c4-dirty")

    def test_state(self):
        payload = bytes([2, 2, 2, 4, 1, 2, 1, 1,
                         0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0])
        s = kr.decode_state(payload)
        self.assertEqual(s["page"], 2)
        self.assertEqual(s["page_name"], "chargertest")
        self.assertEqual(s["active_cc"], 2)
        self.assertEqual(s["spec_rev"], "PD3.x")
        self.assertEqual(s["num_pdos"], 4)
        self.assertEqual(s["chip"], "INA226")
        self.assertTrue(s["pm_online"])
        self.assertTrue(s["log_on"])
        self.assertEqual((s["vbus_mv"], s["current_ma"], s["power_mw"], s["energy_mwh"]),
                         (20000, -1500, 74565, 7))
        self.assertEqual(s["direction"], "down")

    def test_state_link_fields(self):
        base = bytes([1, 2, 2, 4, 1, 2, 1, 1,
                      0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0])
        base += bytes(17)  # fields up to the link block (zero filled)
        base = base[:40] + bytes([1 | 4 | 8 | 16]) + (83).to_bytes(4, "little") + (7).to_bytes(4, "little")
        s = kr.decode_state(base)
        self.assertTrue(s["stale"] and s["inline"] and s["cable_stale"] and s["cable_scanned"])
        self.assertFalse(s["waiting"])
        self.assertEqual((s["age_s"], s["cable_age_s"]), (83, 7))
        self.assertIn("LAST, 1m 23s ago", kr.render("state", s))

    def test_state_crossed_cc_flags(self):
        base = bytes(40)
        s = kr.decode_state(base + bytes([0]) + bytes(8))
        self.assertFalse(s["crossed"] or s["flip_hint"])
        s = kr.decode_state(base + bytes([32]) + bytes(8))        # kLinkCrossed
        self.assertTrue(s["crossed"])
        self.assertFalse(s["flip_hint"])
        s = kr.decode_state(base + bytes([64]) + bytes(8))        # kLinkFlipHint
        self.assertTrue(s["flip_hint"])
        self.assertFalse(s["crossed"])

    def test_state_no_pd_flag(self):
        base = bytes(40)
        s = kr.decode_state(base + bytes([0]) + bytes(8))
        self.assertFalse(s["no_pd"])
        s = kr.decode_state(base + bytes([128]) + bytes(8))       # kLinkNoPd: settled "no PD this attach"
        self.assertTrue(s["no_pd"])
        self.assertFalse(s["crossed"] or s["flip_hint"] or s["inline"])
        s = kr.decode_state(base + bytes([32 | 64]) + bytes(8))   # the neighbours do not leak into bit 7
        self.assertFalse(s["no_pd"])

    def test_state_reset_cause(self):
        base = bytes(40) + bytes([0]) + bytes(8)            # 49 B: everything up to the link block
        self.assertNotIn("reset_cause", kr.decode_state(base))   # older firmware: field absent
        por_pin = kr.decode_state(base + ((1 << 27) | (1 << 26)).to_bytes(4, "little"))
        self.assertEqual(por_pin["reset_flags"], (1 << 27) | (1 << 26))
        self.assertEqual(por_pin["reset_cause"], "POR")      # power-on also sets PIN; the name picks POR
        self.assertEqual(kr.decode_state(base + (1 << 26).to_bytes(4, "little"))["reset_cause"], "PIN")
        self.assertEqual(kr.decode_state(base + ((1 << 29) | (1 << 26)).to_bytes(4, "little"))["reset_cause"], "IWDG")
        self.assertEqual(kr.decode_state(base + (1 << 28).to_bytes(4, "little"))["reset_cause"], "SW")
        self.assertEqual(kr.decode_state(base + bytes(4))["reset_cause"], "none")
        self.assertIn("reset_cause: POR", kr.render("state", por_pin))

    def test_ago_suffix_only_when_stale(self):
        self.assertEqual(kr._ago({"stale": False, "age_s": 5}), "")
        self.assertEqual(kr._ago({"stale": True, "age_s": 5}), " (LAST, 5s ago)")
        self.assertEqual(kr._ago({"stale": True, "age_s": 3700}), " (LAST, 1h 01m ago)")

    def test_debug_detach_attach(self):
        sent = []

        class Link:
            def call(self, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, payload))

        for word, val in (("detach", 0), ("attach", 1)):
            a = kr.build_parser().parse_args(["debug", word])
            kr.COMMANDS["debug"](Link(), a)
            self.assertEqual(sent[-1], (kr.CMD_DEBUG_LINK, bytes([val])))

    def test_debug_epr_info_and_lcd(self):
        sent = []
        replies = {
            kr.CMD_DEBUG_EPR_INFO: bytes([1, 0, 2, 9]) + b"\0" * 28 + (0xA0000000).to_bytes(4, "little")
                                   + (0xCAFEF00D).to_bytes(4, "little"),
            kr.CMD_DEBUG_LCD: bytes([3]) + (1234).to_bytes(4, "little") + (0).to_bytes(4, "little"),
        }

        class Link:
            def call(self, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, replies[cmd]))

        a = kr.build_parser().parse_args(["debug", "epr-info", "force"])
        r = kr.COMMANDS["debug"](Link(), a)
        self.assertEqual(sent[-1], (kr.CMD_DEBUG_EPR_INFO, bytes([2])))
        self.assertEqual(r["epr_info"]["state"], "got")
        self.assertEqual(r["epr_info"]["epr_pdos"], ["8:0xA0000000", "9:0xCAFEF00D"])
        # every raw word by position, zero (padding) ones included
        self.assertEqual(len(r["epr_info"]["words"]), 9)
        self.assertEqual(r["epr_info"]["words"][6], "7:0x00000000 (empty)")
        self.assertEqual(r["epr_info"]["words"][8], "9:0xCAFEF00D")
        self.assertIn("state=got", kr.render("debug", r))
        r = kr.COMMANDS["debug"](Link(), kr.build_parser().parse_args(["debug", "lcd"]))
        self.assertEqual(sent[-1], (kr.CMD_DEBUG_LCD, b""))
        self.assertEqual(r["lcd"], {"dma": "ok", "transfers": 1234, "stalls": 0})
        self.assertEqual(kr.decode_lcd(bytes([1]) + bytes(8))["dma"], "stalled")
        self.assertEqual(kr.decode_lcd(bytes(9))["dma"], "disabled")
        # A newer firmware appends the paint totals since boot: pixels, ms inside lcd stream calls, ms inside fs flash writes.
        full = bytes([3]) + (1234).to_bytes(4, "little") + (0).to_bytes(4, "little") \
            + (987654).to_bytes(4, "little") + (1500).to_bytes(4, "little") + (42).to_bytes(4, "little")
        d = kr.decode_lcd(full)
        self.assertEqual(d["dma"], "ok")
        self.assertEqual(d["paint_px"], 987654)
        self.assertEqual(d["paint_ms"], 1500)
        self.assertEqual(d["fs_ms"], 42)
        # The old 9-byte reply stays readable and carries no paint keys.
        self.assertNotIn("paint_px", kr.decode_lcd(bytes(9)))
        # The divider byte comes last; a 21-byte reply (before the lab switch) carries no spi_div.
        self.assertNotIn("spi_div", d)
        self.assertEqual(kr.decode_lcd(full + bytes([4]))["spi_div"], 4)
        self.assertEqual(kr.decode_lcd(full + bytes([8]))["spi_div"], 8)
        self.assertIn("spi_div=4", kr.render("debug", {"lcd": kr.decode_lcd(full + bytes([4]))}))
        self.assertIn("paint_px=987654", kr.render("debug", {"lcd": d}))
        # The run's worst blocked-paint window (u16 ms) rides behind the divider; shorter replies carry no blocked_ms.
        self.assertNotIn("blocked_ms", kr.decode_lcd(full + bytes([4])))
        r = kr.decode_lcd(full + bytes([8]) + (3100).to_bytes(2, "little"))
        self.assertEqual(r["spi_div"], 8)
        self.assertEqual(r["blocked_ms"], 3100)
        self.assertEqual(kr.decode_lcd(full + bytes([8, 0, 0]))["blocked_ms"], 0)
        self.assertIn("blocked_ms=3100", kr.render("debug", {"lcd": r}))

    def test_debug_cc_probe(self):
        sent = []
        # snapshot: flags=restored, n=2, sw0=0x03, sw1=0x00, pc=7, modes=0x333
        reply = bytes([2, 2, 0x03, 0x00, 7]) + (0x333).to_bytes(2, "little")
        reply += (1000).to_bytes(4, "little") + (2048).to_bytes(2, "little") + (0xFFFF).to_bytes(2, "little") \
            + (0).to_bytes(2, "little") + bytes([0x21, 0x80])
        reply += (500000).to_bytes(4, "little") + (100).to_bytes(2, "little") + (4095).to_bytes(2, "little") \
            + (0xFFFF).to_bytes(2, "little") + bytes([0, 0])

        class Link:
            timeout_s = 1.0

            def call(self, cmd, payload=b""):
                sent.append((cmd, payload, self.timeout_s))
                return kr.parse_response(rsp(cmd, 1, 0, reply))

        link = Link()
        a = kr.build_parser().parse_args(["debug", "cc-probe", "--sw0", "28", "--pc", "0", "--ms", "500"])
        r = kr.COMMANDS["debug"](link, a)
        self.assertEqual(sent[-1][:2], (kr.CMD_DEBUG_CC_PROBE, bytes([0x28, 0]) + (500).to_bytes(2, "little")))
        self.assertGreaterEqual(sent[-1][2], 2.5)   # the K2 blocks for the hold
        self.assertEqual(link.timeout_s, 1.0)       # restored
        p = r["cc_probe"]
        self.assertTrue(p["restored"])
        self.assertFalse(p["aborted"])
        self.assertEqual(p["snapshot"], {"sw0": "0x03", "sw1": "0x00", "pc": 7, "modes": "0x333"})
        self.assertEqual(len(p["samples"]), 2)
        s0, s1 = p["samples"]
        self.assertEqual((s0["t_us"], s0["cc1_raw"], s0["cc1_mv"], s0["cc2_raw"], s0["vbus_mv"]), (1000, 2048, 1650, None, 0))
        self.assertEqual((s0["meas_cc1"]["bc_lvl"], s0["meas_cc1"]["comp"], s0["meas_cc2"]["vbusok"]), (1, True, True))
        self.assertEqual((s1["cc2_raw"], s1["cc2_mv"], s1["vbus_mv"]), (4095, 3299, None))
        text = kr.render("debug", r)
        self.assertIn("restored", text)
        self.assertIn("500.000 ms", text)
        # modes are appended when given
        a = kr.build_parser().parse_args(["debug", "cc-probe", "--sw0", "0", "--pc", "7", "--ms", "10", "--modes", "333"])
        kr.COMMANDS["debug"](link, a)
        self.assertEqual(sent[-1][1], bytes([0, 7]) + (10).to_bytes(2, "little") + (0x333).to_bytes(2, "little"))
        # hold time outside 10..5000 and pc above 7 are refused by the parser
        for bad in (["--ms", "9"], ["--ms", "5001"], ["--pc", "8"]):
            args = ["debug", "cc-probe", "--sw0", "0", "--pc", "0", "--ms", "100"]
            args[args.index(bad[0]) + 1] = bad[1]
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    kr.build_parser().parse_args(args)

    def test_direction_and_magnitude_readout(self):
        def st(ma, mw):
            return {"vbus_mv": 20000, "current_ma": ma, "power_mw": mw, "energy_mwh": 1,
                    "page_name": "home", "direction": kr.direction_of(ma)}
        self.assertEqual(kr.direction_of(1500), "up")
        self.assertEqual(kr.direction_of(-1500), "down")
        self.assertEqual(kr.direction_of(0), "idle")
        for ma, mw in ((-1500, -30000), (-2, 0), (1500, 30000), (0, 0)):
            self.assertNotIn("-", kr.fmt_readout(st(ma, mw)).split(" mWh")[0])
        self.assertIn("down", kr.fmt_readout(st(-1500, -30000)))
        self.assertIn("idle", kr.fmt_readout(st(0, 0)))

    def test_pdo_fixed(self):
        payload = bytes([1, 2, 2, 0, 1, 0x2C, 0x91, 0x01, 0x00, 0x88, 0x13, 0x88, 0x13,
                         0xB8, 0x0B, 0x98, 0x3A, 0, 0])
        head, pdo = kr.decode_pdo(payload)
        self.assertEqual(head, {"valid": True, "spec_rev": "PD3.x", "num_pdos": 2, "index": 0})
        self.assertEqual(pdo, {"type": "fixed", "raw": 0x0001912C, "min_mv": 5000, "max_mv": 5000,
                               "max_ma": 3000, "max_mw": 15000})

    def test_pdo_header_only(self):
        head, pdo = kr.decode_pdo(bytes([0, 0, 0, 0]))
        self.assertFalse(head["valid"])
        self.assertIsNone(pdo)

    def test_emarker(self):
        part0 = (bytes([1, 1, 0xAC, 0x05, 0x34, 0x12, 0x44, 0x33, 0x22, 0x11, 2, 3, 5, 50, 240, 0, 2])
                 + b"Apple\x00USB4")
        part1 = bytes([0xDD, 0xCC, 0xBB, 0xAA, 0x04, 0x03, 0x02, 0x01])
        e = kr.decode_emarker(part0, part1)
        self.assertEqual(e["vid"], 0x05AC)
        self.assertEqual(e["pid"], 0x1234)
        self.assertEqual(e["xid"], 0x11223344)
        self.assertEqual((e["max_current_a"], e["max_voltage_v"], e["max_power_w"]), (5, 50, 240))
        self.assertEqual((e["vendor"], e["speed"]), ("Apple", "USB4"))
        self.assertTrue(e["active"])
        self.assertEqual(e["vdos"], [0xAABBCCDD, 0x01020304])
        self.assertNotIn("sop", e)

    def test_emarker_detail_and_identity(self):
        part0 = bytes([1, 1, 0xAC, 0x05, 0x34, 0x12, 0, 0, 0, 0, 2, 3, 5, 50, 240, 0, 0]) + b"Apple\x00USB4"
        p2 = bytes([1, 2, 1, 3, 3, 0x8C, 60, 70, 2, 2])
        e = kr.decode_emarker(part0, b"", p2)
        self.assertEqual((e["sop"], e["pd_rev"], e["vdo_version"]), ("SOP'", "3.x", 1))
        self.assertEqual(e["latency"], "20-30 ns (~3 m)")
        self.assertTrue(e["epr_cable"] and e["vdo2_valid"] and e["thunderbolt"])
        self.assertFalse(e["usb4"] or e["optical"])
        self.assertEqual((e["max_temp_c"], e["shutdown_temp_c"], e["usb_lanes"]), (60, 70, 2))
        self.assertIn("Thunderbolt", kr.render("emarker", e))
        pid = (bytes([5, 1, 0xAC, 0x05, 0x34, 0x12, 0x00, 0x01, 2, 0x01, 0xFF, 0x87, 0x80])
               + b"Apple\x00iPad")
        i = kr.decode_identity(pid)
        self.assertEqual((i["vid"], i["pid"], i["bcd"]), (0x05AC, 0x1234, 0x0100))
        self.assertEqual(i["svids"], [0xFF01, 0x8087])
        self.assertEqual((i["vendor"], i["model"]), ("Apple", "iPad"))
        self.assertTrue(i["epr"] and i["cuktech"] and not i["xiaomi"])
        self.assertIn("iPad", kr.render("identity", i))


def rle_runs(px):
    """Reference encoder: runs only (valid RLE565)."""
    out, i = bytearray(), 0
    while i < len(px):
        j = i
        while j < len(px) and px[j] == px[i] and j - i < 128:
            j += 1
        out += bytes([j - i - 1, px[i] & 0xFF, px[i] >> 8])
        i = j
    return bytes(out)


class Screenshot(unittest.TestCase):
    def test_decode_runs_and_literals(self):
        data = bytes([0x02, 0x34, 0x12, 0x81, 0x01, 0x00, 0x02, 0x00])
        self.assertEqual(kr.rle565_decode(data, 5), [0x1234] * 3 + [1, 2])

    def test_decode_matches_firmware_vector(self):
        # Same bytes tests/test_capture.cpp produces for 100 equal pixels.
        self.assertEqual(kr.rle565_decode(bytes([99, 5, 0]), 100), [5] * 100)

    def test_decode_rejects_truncated_and_wrong_count(self):
        with self.assertRaises(kr.FrameError):
            kr.rle565_decode(bytes([0x05, 0x34]), 6)
        with self.assertRaises(kr.FrameError):
            kr.rle565_decode(bytes([0x82, 1, 0, 2, 0]), 3)
        with self.assertRaises(kr.FrameError):
            kr.rle565_decode(bytes([0x00, 1, 0]), 2)

    def test_screen_info(self):
        info = kr.decode_screen_info(bytes([240, 0, 240, 0, 16, 15, 1]))
        self.assertEqual(info, {"width": 240, "height": 240, "strip_rows": 16, "num_strips": 15})
        with self.assertRaises(kr.FrameError):
            kr.decode_screen_info(bytes([240, 0, 240, 0, 16, 14, 1]))
        with self.assertRaises(kr.FrameError):
            kr.decode_screen_info(bytes([240, 0, 240, 0, 16, 15, 2]))

    def test_colour_expansion(self):
        self.assertEqual(kr.rgb565_to_rgb888([0xFFFF, 0x0000, 0xF800, 0x07E0, 0x001F]),
                         bytes([255] * 3 + [0] * 3 + [255, 0, 0, 0, 255, 0, 0, 0, 255]))

    def test_png_roundtrip(self):
        import zlib, struct
        rgb = bytes([255, 0, 0, 0, 255, 0, 0, 0, 255, 9, 8, 7])
        png = kr.png_bytes(rgb, 2, 2)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        pos, idat = 8, b""
        while pos < len(png):
            n, tag = struct.unpack(">I4s", png[pos:pos + 8])
            body = png[pos + 8:pos + 8 + n]
            self.assertEqual(struct.unpack(">I", png[pos + 8 + n:pos + 12 + n])[0],
                             zlib.crc32(tag + body) & 0xFFFFFFFF)
            if tag == b"IDAT":
                idat += body
            pos += 12 + n
        raw = zlib.decompress(idat)
        self.assertEqual(raw, b"\x00" + rgb[:6] + b"\x00" + rgb[6:])

    def test_scale(self):
        rgb, w, h = kr.scale_rgb(bytes([1, 2, 3, 4, 5, 6]), 2, 1, 2)
        self.assertEqual((w, h), (4, 2))
        self.assertEqual(rgb, bytes([1, 2, 3] * 2 + [4, 5, 6] * 2) * 2)

    def test_scale_keeps_every_row(self):
        # 2x2 image, four different pixels: each source row must stay its own row (regression:
        # the upscaler once repeated row 0 for the whole image).
        rgb = bytes([1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4])
        out, w, h = kr.scale_rgb(rgb, 2, 2, 3)
        self.assertEqual((w, h), (6, 6))
        rows = [out[y * 18:(y + 1) * 18] for y in range(6)]
        self.assertEqual(rows[0], bytes([1] * 9 + [2] * 9))
        self.assertEqual(rows[2], rows[0])
        self.assertEqual(rows[3], bytes([3] * 9 + [4] * 9))
        self.assertEqual(rows[5], rows[3])

    def test_dump_raw_and_strip_stats(self):
        import tempfile, os
        frames = [rsp(kr.CMD_SCREENSHOT, 1, 0, bytes([240, 0, 240, 0, 16, 15, 1]))]
        for s in range(15):
            px = [s * 3 + (i % 3) for i in range(3840)]
            enc = b"".join(bytes([0x80 + len(px[i:i + 27]) - 1] + [x for v in px[i:i + 27] for x in (v & 0xFF, v >> 8)])
                           for i in range(0, 3840, 27))
            # split on token boundaries (55 byte literal tokens, one per frame)
            pos, toks = 0, []
            while pos < len(enc):
                n = (enc[pos] - 0x7F) * 2 + 1
                toks.append(enc[pos:pos + n]); pos += n
            for k, tk in enumerate(toks):
                frames.append(rsp(kr.CMD_SCREEN_DATA, 1, 0, bytes([s, 1 if k == len(toks) - 1 else 0]) + tk))

        class T:
            def __init__(self): self.q = list(frames)
            def write(self, d): pass
            def read(self, ms): return self.q.pop(0) if self.q else b""
        d = tempfile.mkdtemp()
        raw_path, png = os.path.join(d, "r.bin"), os.path.join(d, "s.png")
        args = kr.build_parser().parse_args(["screenshot", png, "--dump-raw", raw_path, "--debug"])
        lines = []
        res = kr.cmd_screenshot(kr.RemoteLink(T(), 1.0), args, out=lines.append)
        self.assertEqual(len(res["strip_stats"]), 15)
        self.assertEqual(res["strip_stats"][2], {"strip": 2, "pixels": 3840, "colours": 3, "bytes": res["strip_stats"][2]["bytes"]})
        self.assertEqual(len(lines), 15)
        dumped = Path(raw_path).read_bytes()
        self.assertEqual(len(dumped) % 64, 0)
        self.assertEqual(dumped[:64], frames[0])           # the response frame is recorded too
        self.assertEqual(len(dumped), 64 * len(frames))

    def test_end_to_end_with_chunked_strips_and_log_noise(self):
        import tempfile, os
        W, H, ROWS = 240, 240, 16
        screen = [(i * 37) % 7 * 0x1111 for i in range(W * H)]
        frames = [rsp(kr.CMD_SCREENSHOT, 1, 0, bytes([240, 0, 240, 0, 16, 15, 1]))]
        for s in range(15):
            enc = rle_runs(screen[s * W * ROWS:(s + 1) * W * ROWS])
            chunks = [enc[i:i + 56] for i in range(0, len(enc), 56)]
            for k, c in enumerate(chunks):
                flags = 1 if k == len(chunks) - 1 else 0
                frames.append(rsp(kr.CMD_SCREEN_DATA, 1, 0, bytes([s, flags]) + c))
            frames.append(b"[LOG] noise\n".ljust(64, b"\x00"))

        class T:
            def __init__(self): self.q = list(frames); self.sent = []
            def write(self, d): self.sent.append(bytes(d))
            def read(self, ms): return self.q.pop(0) if self.q else b""
        t = T()
        out = os.path.join(tempfile.mkdtemp(), "s.png")
        args = kr.build_parser().parse_args(["screenshot", out, "--scale", "2"])
        res = kr.cmd_screenshot(kr.RemoteLink(t, 1.0), args)
        self.assertEqual(t.sent[0][:5], bytes([0xA5, 0x4B, 10, 1, 0]))
        self.assertEqual((res["width"], res["height"]), (480, 480))
        data = Path(out).read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")
        self.assertGreater(res["rle_bytes"], 0)

    def test_abort_frame_raises(self):
        frames = [rsp(kr.CMD_SCREENSHOT, 1, 0, bytes([240, 0, 240, 0, 16, 15, 1])),
                  rsp(kr.CMD_SCREEN_DATA, 1, 0, bytes([3, 0x80]))]

        class T:
            def __init__(self): self.q = list(frames)
            def write(self, d): pass
            def read(self, ms): return self.q.pop(0) if self.q else b""
        args = kr.build_parser().parse_args(["screenshot", "/dev/null"])
        with self.assertRaises(kr.RemoteError):
            kr.cmd_screenshot(kr.RemoteLink(T(), 1.0), args)


class CableRCommands(unittest.TestCase):
    INFO = bytes([3, 1, 0x64, 0, 0xEC, 0x13, 0x28, 0, 0x24, 0x13, 0x02, 0x08, 0, 0, 0, 0])

    def run_cmd(self, argv, sent, info=None):
        class Link:
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, info or CableRCommands.INFO))
        a = kr.build_parser().parse_args(argv)
        return kr.COMMANDS["cable-r"](Link(), a)

    def test_status_and_reset_ops(self):
        sent = []
        r = self.run_cmd(["cable-r", "status"], sent)
        self.assertEqual(sent, [(0x50, b"\x00")])
        self.assertEqual((r["idle"], r["valid"], r["grade"], r["r_mohm"]), (True, True, "excellent", 100))
        self.assertEqual((r["idle_mv"], r["idle_ma"], r["load_mv"], r["load_ma"]), (5100, 40, 4900, 2050))
        sent.clear()
        self.run_cmd(["cable-r", "reset"], sent)
        self.assertEqual(sent, [(0x50, b"\x01")])
        sent.clear()
        self.run_cmd(["cable-r", "kelvin"], sent)
        self.run_cmd(["cable-r", "passive"], sent)
        self.assertEqual(sent, [(0x50, b"\x03"), (0x50, b"\x02")])

    def test_kelvin_decode_and_render(self):
        r = kr.decode_cable_r(bytes([6, 1, 120, 0, 0, 0, 0, 0, 0, 0, 0, 0, 50, 0, 70, 0]))
        self.assertEqual((r["kelvin"], r["valid"], r["r_mohm"], r["pos_mohm"], r["gnd_mohm"]), (True, True, 120, 50, 70))
        text = kr.render("cable-r", r)
        self.assertIn("kelvin", text)
        self.assertIn("pos 50 mOhm", text)
        self.assertIn("K2 path not included", text)
        w = kr.decode_cable_r(bytes([4, 0] + [0] * 14))
        self.assertIn("0.45 A", kr.render("cable-r", w))

    def test_render(self):
        r = kr.decode_cable_r(self.INFO)
        self.assertIn("100 mOhm (excellent)", kr.render("cable-r", r))
        w = kr.decode_cable_r(bytes(16))
        self.assertIn("waiting for an idle reference", kr.render("cable-r", w))
        w = kr.decode_cable_r(bytes([1, 0, 0, 0, 0xEC, 0x13, 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0]))
        self.assertIn("1 A or more", kr.render("cable-r", w))

    def test_short_payload(self):
        with self.assertRaises(Exception):
            kr.decode_cable_r(bytes(11))

    def test_blackbox_captures_list_renders(self):
        # Regression: `blackbox captures` returns a list and crashed on r.get("emlib").
        self.assertEqual(kr.render("blackbox", []), "no captures")
        text = kr.render("blackbox", [{"slot": 0, "trigger": "hard-reset", "bytes": 1234, "age_ms": 5000},
                                      {"slot": 2, "trigger": "detach", "bytes": 80, "age_ms": 61500}])
        self.assertIn("slot 0", text)
        self.assertIn("hard-reset", text)
        self.assertIn("1234 bytes", text)
        self.assertIn("61.5 s ago", text)
        self.assertEqual(len(text.splitlines()), 2)
        self.assertIn("frozen", kr.render("blackbox", {"frozen": True, "oldest": 0, "head": 10, "used": 10}))
        self.assertIn("cleared", kr.render("blackbox", {"cleared": True}))


class FcCommands(unittest.TestCase):
    """fc scan / trigger / reset: the confirm rules hold on the client too, before anything is sent."""
    INFO = bytes([2, 0, 0, 1, 3, 0x28, 0x23, 0xF6, 0x22, 2, 3, 1, 0x32, 0x23, 4, 2, 0, 0])

    def test_seven_results_include_qc3plus(self):
        payload = bytes([0, 0, 0, 2, 7, 0, 0, 0x88, 0x13, 7])
        payload += bytes([4, 2, 0, 0]) * 6 + bytes([2, 1, 0x70, 0x17])
        result = kr.decode_fc(payload)
        self.assertEqual(len(result["scan"]["results"]), 7)
        self.assertEqual(result["scan"]["results"][-1], {"proto": "QC3+", "outcome": "accepted", "mv": 6000})
        self.assertIn("QC3+ accepted 6000 mV", kr.render("fc", result))

    def link(self, sent):
        class Link:
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, FcCommands.INFO))
        return Link()

    def run_cmd(self, argv, sent):
        a = kr.build_parser().parse_args(argv)
        return kr.COMMANDS["fc"](self.link(sent), a)

    def test_ufcs_vivo_triggers_need_confirm_above_5v(self):
        for proto, idx in (("ufcs", 9), ("vivo", 10)):
            sent = []
            with self.assertRaises(kr.FcRefused):
                self.run_cmd(["fc", "trigger", proto, "--volts", "9"], sent)
            self.assertEqual(sent, [])
            self.run_cmd(["fc", "trigger", proto, "--volts", "9", "--confirm"], sent)
            self.assertEqual(sent[0][1][:2], bytes([3, idx]))

    def test_parser(self):
        a = kr.build_parser().parse_args(["fc", "trigger", "qc2", "--volts", "9", "--confirm"])
        self.assertEqual((a.command, a.fc_cmd, a.proto, a.volts, a.confirm), ("fc", "trigger", "qc2", 9.0, True))
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["fc", "trigger", "nope", "--volts", "9"])

    def test_trigger_bytes(self):
        sent = []
        r = self.run_cmd(["fc", "trigger", "qc2", "--volts", "9", "--confirm"], sent)
        self.assertEqual(sent, [(kr.CMD_FC, bytes([3, 0, 0x28, 0x23, 1]))])
        self.assertEqual((r["state"], r["proto"], r["target_mv"], r["vbus_mv"]), ("active", "QC2", 9000, 8950))
        sent.clear()
        self.run_cmd(["fc", "trigger", "vooc", "--volts", "5"], sent)         # 5 V: no confirm needed
        self.assertEqual(sent, [(kr.CMD_FC, bytes([3, 6, 0x88, 0x13, 0]))])

    def test_refused_without_confirm_sends_nothing(self):
        sent = []
        with self.assertRaises(kr.FcRefused):
            self.run_cmd(["fc", "trigger", "qc2", "--volts", "9"], sent)
        with self.assertRaises(kr.FcRefused):
            self.run_cmd(["fc", "scan"], sent)
        self.assertEqual(sent, [])

    def test_allow_inline_flag(self):
        sent = []
        with self.assertRaises(kr.FcRefused):                 # at 5 V too: both flags, nothing is sent
            self.run_cmd(["fc", "trigger", "vooc", "--volts", "5", "--allow-inline"], sent)
        self.assertEqual(sent, [])
        self.run_cmd(["fc", "trigger", "vooc", "--volts", "5", "--confirm", "--allow-inline"], sent)
        self.assertEqual(sent[-1], (kr.CMD_FC, bytes([3, 6, 0x88, 0x13, 3])))
        self.run_cmd(["fc", "trigger", "qc2", "--volts", "9", "--confirm", "--allow-inline"], sent)
        self.assertEqual(sent[-1], (kr.CMD_FC, bytes([3, 0, 0x28, 0x23, 3])))
        self.run_cmd(["fc", "scan", "--confirm", "--allow-inline"], sent)
        self.assertEqual(sent[-1], (kr.CMD_FC, bytes([2, 3])))
        with self.assertRaises(kr.FcRefused):                 # the override is no confirmation
            self.run_cmd(["fc", "trigger", "qc2", "--volts", "9", "--allow-inline"], sent)

    def test_guard_refusal_names_the_reason(self):
        calls = []
        refused = bytes([0, 0, kr.FC_FAILS.index("PD or load active"), 0, 0, 0x88, 0x13, 0, 0, 0])   # idle + reason
        class Link:
            def call(self_, cmd, payload=b""):
                calls.append(payload[0])
                if payload[0] == kr.FC_OP_STATUS:
                    return kr.parse_response(rsp(cmd, 1, 0, refused))
                raise kr.RemoteError(3, cmd)
        a = kr.build_parser().parse_args(["fc", "trigger", "vooc", "--volts", "5"])
        with self.assertRaises(kr.FcRefused) as cm:
            kr.COMMANDS["fc"](Link(), a)
        self.assertIn("PD or load active", str(cm.exception))
        self.assertIn("--allow-inline", str(cm.exception))
        self.assertEqual(calls, [kr.FC_OP_TRIGGER, kr.FC_OP_STATUS])

    def test_scan_reset_status(self):
        sent = []
        r = self.run_cmd(["fc", "scan", "--confirm"], sent)
        self.assertEqual(sent, [(kr.CMD_FC, bytes([2, 1]))])
        self.assertEqual(r["scan"]["results"], [{"proto": "AFC", "outcome": "accepted", "mv": 9010},
                                                 {"proto": "FCP", "outcome": "rejected", "mv": 0}])
        self.assertTrue(r["scan"]["running"])
        sent.clear()
        self.run_cmd(["fc", "reset"], sent)
        self.assertEqual(sent, [(kr.CMD_FC, bytes([1]))])
        sent.clear()
        self.run_cmd(["fc", "status"], sent)
        self.assertEqual(sent, [(kr.CMD_FC, bytes([0]))])

    def test_render(self):
        sent = []
        text = kr.render("fc", self.run_cmd(["fc", "status"], sent))
        self.assertIn("QC2", text)
        self.assertIn("AFC accepted 9010 mV", text)


class StoreCommands(unittest.TestCase):
    """store list / save-emarker / save-log; erase was retired with the native-page writer."""
    def link(self, sent, info):
        class Link:
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(rsp(cmd, 1, 0, info))
        return Link()

    def run_cmd(self, argv, sent, info=bytes([0, 3, 5, 2, 0x02, 0x01, 2, 4])):
        a = kr.build_parser().parse_args(argv)
        return kr.COMMANDS["store"](self.link(sent, info), a)

    def test_list(self):
        sent = []
        r = self.run_cmd(["store", "list"], sent)
        self.assertEqual(kr.CMD_STORE, 0x60)      # storage range 0x60-0x6F, 0x50 is CABLE_R
        self.assertEqual(sent, [(kr.CMD_STORE, bytes([0]))])
        self.assertEqual((r["count"], r["native_slot"], r["foreign"], r["saved_log"], r["log_len"], r["bank"]),
                         (3, True, False, True, 258, 2))
        text = kr.render("store", r)
        self.assertEqual(r["native_records"], 4)
        self.assertIn("4 native library records", text)
        self.assertIn("3 e-marker", text)
        self.assertIn("258 bytes", text)

    def test_save(self):
        sent = []
        r = self.run_cmd(["store", "save-emarker"], sent)
        self.assertEqual(sent, [(kr.CMD_STORE, bytes([1]))])
        self.assertEqual(r["slot"], 2)
        sent.clear()
        self.run_cmd(["store", "save-log"], sent)
        self.assertEqual(sent, [(kr.CMD_STORE, bytes([3]))])

    def test_erase_is_gone(self):
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["store", "erase", "2", "--confirm"])

    def test_device_error_is_reported(self):
        sent = []
        with self.assertRaises(kr.StoreFailed) as cm:
            self.run_cmd(["store", "save-emarker"], sent, bytes([2, 3, 0, 0, 0, 0, 2, 0]))   # err 2 = BadSlot
        self.assertIn("BadSlot", str(cm.exception))


class RealPages(unittest.TestCase):
    """Frames produced by the real Ui + capture_screen() on the host (tests/test_ui_capture.cpp)
    must decode to exactly the pixels the renderer drew, through the full scaled PNG path."""
    HOST = Path(__file__).resolve().parent.parent / "host-build"

    def png_rgb(self, data):
        try:                                          # Pillow writes filtered rows; let it decode
            from PIL import Image
            import io
            im = Image.open(io.BytesIO(data)).convert("RGB")
            return im.width, im.height, im.tobytes()
        except ImportError:
            pass
        import struct, zlib
        pos, idat, w, h = 8, b"", 0, 0
        while pos < len(data):
            n, tag = struct.unpack(">I4s", data[pos:pos + 8])
            if tag == b"IHDR":
                w, h = struct.unpack(">II", data[pos + 8:pos + 16])
            if tag == b"IDAT":
                idat += data[pos + 8:pos + 8 + n]
            pos += 12 + n
        raw = zlib.decompress(idat)
        return w, h, b"".join(raw[y * (w * 3 + 1) + 1:(y + 1) * (w * 3 + 1)] for y in range(h))

    def test_pages(self):
        found = 0
        for name in ("home", "chargertest", "pd", "diag", "tools",
                     "fc_setup", "fc_edit", "fc_edit_coarse", "fc_review", "fc_armed",
                     "fc_requesting", "fc_run", "fc_live_edit", "fc_live_review",
                     "fc_live_edit_down", "fc_releasing", "fc_release_after_exit", "fc_safe",
                     "fc_scanning", "fc_scan", "fc_refused"):
            rawf, panelf = self.HOST / f"k2_{name}.raw", self.HOST / f"k2_{name}.panel"
            if not (rawf.exists() and panelf.exists()):
                continue
            found += 1
            raw = rawf.read_bytes()
            frames = [raw[i:i + 64] for i in range(0, len(raw), 64)]
            frames.insert(0, rsp(kr.CMD_SCREENSHOT, 7, 0, bytes([240, 0, 240, 0, 4, 60, 1])))

            class T:
                def __init__(self): self.q = list(frames)
                def write(self, d): pass
                def read(self, ms): return self.q.pop(0) if self.q else b""
            link = kr.RemoteLink(T(), 1.0)
            link.seq = 6                              # next request carries seq 7
            panel = panelf.read_bytes()
            px = [int.from_bytes(panel[2 * i:2 * i + 2], "little") for i in range(240 * 240)]
            for scale in (1, 2):
                out = str(self.HOST / f"k2_{name}_scale{scale}.png")
                T.__init__ = lambda self: setattr(self, "q", list(frames))
                link.transport = T()
                link.seq = 6
                args = kr.build_parser().parse_args(["screenshot", out, "--scale", str(scale)])
                kr.cmd_screenshot(link, args)
                w, h, rgb = self.png_rgb(Path(out).read_bytes())
                want, _, _ = kr.scale_rgb(kr.rgb565_to_rgb888(px), 240, 240, scale)
                self.assertEqual((w, h), (240 * scale, 240 * scale), name)
                self.assertTrue(rgb == want, f"{name} scale {scale}: PNG differs from the rendered panel")
        if found == 0:
            self.skipTest("run `make test`: tests/test_ui_capture.cpp produces the frames")


class Session(unittest.TestCase):
    """RemoteLink against a scripted transport: seq matching, log skipping, bootloader detection."""

    class Fake:
        def __init__(self, incoming):
            self.incoming = list(incoming)
            self.sent = []

        def write(self, frame):
            self.sent.append(frame)

        def read(self, timeout_ms):
            return self.incoming.pop(0) if self.incoming else b""

    def test_skips_logs_and_stale_responses(self):
        log = b"[SYS] Heartbeat\n".ljust(64, b"\x00")
        t = self.Fake([log, rsp(1, 99, 0), log, rsp(1, 1, 0, b"\x01")])
        link = kr.RemoteLink(t, timeout_s=0.2)
        r = link.call(kr.CMD_PING)
        self.assertEqual(r.seq, 1)
        self.assertEqual(t.sent[0][:5], bytes([0xA5, 0x4B, 0x01, 0x01, 0x00]))

    def test_seq_increments(self):
        t = self.Fake([rsp(1, 1, 0), rsp(1, 2, 0)])
        link = kr.RemoteLink(t, timeout_s=0.2)
        link.call(kr.CMD_PING)
        link.call(kr.CMD_PING)
        self.assertEqual([f[3] for f in t.sent], [1, 2])

    def test_bootloader_reply_is_reported(self):
        t = self.Fake([k2_flasher.build_frame(1)])
        link = kr.RemoteLink(t, timeout_s=0.2)
        with self.assertRaises(kr.BootloaderDetected):
            link.call(kr.CMD_PING)

    def test_timeout(self):
        link = kr.RemoteLink(self.Fake([]), timeout_s=0.05)
        with self.assertRaises(kr.RemoteTimeout):
            link.call(kr.CMD_PING)

    def test_error_status_raises(self):
        t = self.Fake([rsp(2, 1, 4)])
        link = kr.RemoteLink(t, timeout_s=0.2)
        with self.assertRaises(kr.RemoteError) as ctx:
            link.call(kr.CMD_BUTTON, bytes([1, 100, 0]))
        self.assertEqual(ctx.exception.status, kr.STATUS_BUSY)


class ActivePd(unittest.TestCase):
    class Link:
        def __init__(self, replies):
            self.replies, self.calls = replies, []

        def call(self, cmd, payload=b""):
            self.calls.append((cmd, bytes(payload)))
            r = self.replies.get(cmd, b"")
            if isinstance(r, Exception):
                raise r
            return kr.Response(cmd, 0, 0, r)

    TRIG = bytes([5, 1, 1, 0, 4, 0x20, 0x4E, 0xB8, 0x0B, 1, 0, 0, 0])

    def args(self, **kw):
        import argparse
        return argparse.Namespace(**kw)

    def test_request_payload_and_status(self):
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        r = kr.cmd_pd(link, self.args(pd_cmd="request", pos=4, mv=20000, ma=3000, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_REQUEST, bytes([4, 1, 0x20, 0x4E, 0xB8, 0x0B])))
        self.assertEqual(r["state"], "ready")
        self.assertEqual((r["pos"], r["mv"], r["ma"], r["hard_resets"]), (4, 20000, 3000, 1))
        self.assertIn("20 V", kr.render("pd", r))

    def test_request_over_load_sets_flag_bit_1(self):
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        # each `pd request` call is a CMD_PD_REQUEST followed by a CMD_PD_TRIGGER_STATUS read
        # --over-load with --confirm: flags = 3
        kr.cmd_pd(link, self.args(pd_cmd="request", pos=6, mv=5500, ma=2000, confirm=True, over_load=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_REQUEST,
                                         bytes([6, 3]) + (5500).to_bytes(2, "little") + (2000).to_bytes(2, "little")))
        # --over-load alone: flags = 2 (bit 1, no confirm)
        kr.cmd_pd(link, self.args(pd_cmd="request", pos=6, mv=5500, ma=2000, over_load=True))
        self.assertEqual(link.calls[2], (kr.CMD_PD_REQUEST,
                                         bytes([6, 2]) + (5500).to_bytes(2, "little") + (2000).to_bytes(2, "little")))
        # without --over-load the flag stays clear
        kr.cmd_pd(link, self.args(pd_cmd="request", pos=6, mv=5500, ma=2000, confirm=True))
        self.assertEqual(link.calls[4][1], bytes([6, 1]) + (5500).to_bytes(2, "little") + (2000).to_bytes(2, "little"))

    def test_over_load_is_only_on_pd_request(self):
        a = kr.build_parser().parse_args(["pd", "request", "6", "--mv", "5500", "--ma", "2000",
                                          "--confirm", "--over-load"])
        self.assertTrue(a.over_load)
        self.assertTrue(a.confirm)
        for other in (["pd", "release"], ["pd", "reset"], ["pd", "status"], ["fc", "trigger", "qc2", "--volts", "9"]):
            with self.assertRaises(SystemExit):
                kr.build_parser().parse_args(other + ["--over-load"])


    def test_source_hard_resets_is_appended_and_optional(self):
        self.assertEqual(kr.decode_pd_trigger(self.TRIG)["source_hard_resets"], 0)   # older firmware: 13 bytes
        r = kr.decode_pd_trigger(self.TRIG + bytes([2, 0, 0, 0]))
        self.assertEqual((r["hard_resets"], r["source_hard_resets"]), (1, 2))
        self.assertIn("source_hard_resets=2", kr.render("pd", r))

    def test_pd_status_carries_the_rd_pin(self):
        # 18 bytes: ..., source_hard_resets u32, then the appended pin byte (0/1/2 = Rd pinned on CC1/CC2)
        r = kr.decode_pd_trigger(self.TRIG + bytes([2, 0, 0, 0]) + bytes([2]))
        self.assertEqual((r["source_hard_resets"], r["pin"]), (2, 2))
        self.assertIn("Rd pin CC2", kr.render("pd", r))
        self.assertEqual(kr.decode_pd_trigger(self.TRIG)["pin"], 0)                # older firmware: no pin byte
        self.assertNotIn("Rd pin", kr.render("pd", kr.decode_pd_trigger(self.TRIG)))

    def test_pd_status_carries_the_direct_plug_line_trial(self):
        # 20 bytes: ..., pin u8, then trial_line u8 (the line our single Rd is trying while the schedule is live) and trial_moves u8
        base = self.TRIG + bytes([2, 0, 0, 0]) + bytes([0])
        r = kr.decode_pd_trigger(base + bytes([2, 1]))
        self.assertEqual((r["pin"], r["trial_line"], r["trial_moves"]), (0, 2, 1))
        self.assertIn("Rd trying CC2", kr.render("pd", r))
        self.assertIn("line_moves=1", kr.render("pd", r))
        idle = kr.decode_pd_trigger(base + bytes([0, 3]))   # schedule over: no line, the moves stay readable
        self.assertEqual((idle["trial_line"], idle["trial_moves"]), (0, 3))
        self.assertNotIn("Rd trying", kr.render("pd", idle))
        old = kr.decode_pd_trigger(base)                    # firmware before the fallback: 18 bytes
        self.assertEqual((old["trial_line"], old["trial_moves"]), (0, 0))
        self.assertNotIn("line_moves", kr.render("pd", old))

    def test_request_without_confirm_sends_flag_zero(self):
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="request", pos=2, mv=0, ma=0, confirm=False))
        self.assertEqual(link.calls[0][1], bytes([2, 0, 0, 0, 0, 0]))

    def test_need_confirm_is_an_error_with_hint(self):
        link = self.Link({kr.CMD_PD_REQUEST: kr.RemoteError(kr.STATUS_NEED_CONFIRM, kr.CMD_PD_REQUEST)})
        with self.assertRaises(kr.RemoteError) as ctx:
            kr.cmd_pd(link, self.args(pd_cmd="request", pos=2, mv=0, ma=0, confirm=False))
        self.assertIn("--confirm", str(ctx.exception))

    def test_error_names_the_pd_err(self):
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_PD_REQUEST, bytes([5]))
        self.assertIn("inline", str(e))

    def test_identify_inline_refusal_names_inline(self):
        # an inline Identify refusal is BadArg + pd::Err::Inline on the wire: the tool must say
        # "refused: inline ...", not "busy" (ROADMAP row "Inline Identify refusal answers busy")
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_IDENTIFY_DEVICE, bytes([kr.PD_ERR_INLINE]))
        self.assertIn("refused: inline - the Sink tab shows the sniffed identity", str(e))

    def test_identify_busy_stays_busy(self):
        # a running charger test still answers Busy with no payload: plain "busy", no inline text
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_IDENTIFY_DEVICE)
        self.assertIn("busy", str(e))
        self.assertNotIn("refused: inline", str(e))

    def test_emlib_busy_refusal_names_the_owner(self):
        # a library write during a charger test is Busy + kEmErrBusy: the message says what owns the link
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_EM_LIB, bytes([7]))
        self.assertIn("busy", str(e))
        self.assertIn("charger test", str(e))

    def test_store_busy_refusal_names_the_owner(self):
        # a store save during a charger test or an FC release is Busy with no payload
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_STORE)
        self.assertIn("busy", str(e))
        self.assertIn("charger test", str(e))

    def test_action_busy_is_not_need_confirm(self):
        # a declined action (ports busy, negotiation in flight) is Busy, and the message says so
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_ACTION)
        self.assertIn("busy", str(e))
        self.assertIn("charger test", str(e))
        self.assertNotIn("confirm", str(e).lower())

    def test_identify_device_confirm_help_says_it_does_nothing(self):
        help_text = kr.build_parser()._subparsers._group_actions[0].choices["identify-device"].format_help()
        self.assertIn("no effect", help_text)

    def test_reset_release_status(self):
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="reset"))
        kr.cmd_pd(link, self.args(pd_cmd="release"))
        kr.cmd_pd(link, self.args(pd_cmd="status"))
        self.assertEqual([c[0] for c in link.calls],
                         [kr.CMD_PD_RESET, kr.CMD_PD_TRIGGER_STATUS, kr.CMD_PD_RELEASE, kr.CMD_PD_TRIGGER_STATUS,
                          kr.CMD_PD_TRIGGER_STATUS])

    def test_reset_without_flags_sends_empty_payload(self):
        # as today: no flags -> the old empty payload, an older firmware accepts it too
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="reset"))
        self.assertEqual(link.calls[0], (kr.CMD_PD_RESET, b""))

    def test_reset_inline_confirm_sends_flags_3(self):
        # lab-only inline Hard Reset: bit0 confirm | bit1 inline (kPdFlagResetInline) = 0x03
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="reset", inline=True, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_RESET, bytes([3])))

    def test_reset_inline_alone_sends_flags_2(self):
        # --inline without --confirm still reaches the firmware (it answers NeedConfirm), the tool does not gate it
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="reset", inline=True, confirm=False))
        self.assertEqual(link.calls[0], (kr.CMD_PD_RESET, bytes([2])))

    def test_reset_confirm_alone_sends_flags_1(self):
        link = self.Link({kr.CMD_PD_TRIGGER_STATUS: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="reset", inline=False, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_RESET, bytes([1])))

    def test_reset_parser_has_inline_and_confirm(self):
        p = kr.build_parser()
        a = p.parse_args(["pd", "reset"])
        self.assertEqual((a.pd_cmd, a.inline, a.confirm), ("reset", False, False))
        a = p.parse_args(["pd", "reset", "--inline", "--confirm"])
        self.assertEqual((a.inline, a.confirm), (True, True))
        # the flags exist only on the reset subparser, never on request/release/attach
        self.assertFalse(hasattr(p.parse_args(["pd", "request", "1"]), "inline"))
        self.assertFalse(hasattr(p.parse_args(["pd", "release"]), "inline"))

    def test_reset_inline_error_names_the_pd_err(self):
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_PD_RESET, bytes([5]))
        self.assertIn("inline: a live sink is attached", str(e))

    def test_handback_confirm_sends_flags_1(self):
        # phase-1 lab op: bit0 confirm only; the reply's payload byte is the verdict
        link = self.Link({kr.CMD_PD_HANDBACK: bytes([0])})
        r = kr.cmd_pd(link, self.args(pd_cmd="handback", confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_HANDBACK, bytes([1])))
        self.assertEqual(r["verdict"], "allowed")

    def test_handback_without_confirm_sends_empty_payload(self):
        link = self.Link({kr.CMD_PD_HANDBACK: bytes([2])})   # verdict 2 = confirm needed
        r = kr.cmd_pd(link, self.args(pd_cmd="handback", confirm=False))
        self.assertEqual(link.calls[0], (kr.CMD_PD_HANDBACK, b""))
        self.assertEqual(r["verdict"], "confirm needed")

    def test_handback_refusal_verdict_is_named(self):
        link = self.Link({kr.CMD_PD_HANDBACK: bytes([4])})   # verdict 4 = no contract owned
        r = kr.cmd_pd(link, self.args(pd_cmd="handback", confirm=True))
        self.assertEqual(r["verdict"], "no contract owned")

    def test_handback_parser_has_only_confirm(self):
        p = kr.build_parser()
        a = p.parse_args(["pd", "handback"])
        self.assertEqual((a.pd_cmd, a.confirm), ("handback", False))
        a = p.parse_args(["pd", "handback", "--confirm"])
        self.assertEqual(a.confirm, True)
        # the hand-back is lab-only: no --inline-style extra flags, and the other pd subcommands have no handback flag
        self.assertFalse(hasattr(p.parse_args(["pd", "reset"]), "confirm_only_marker"))
        for other in (["pd", "request", "1"], ["pd", "reset"], ["pd", "release"], ["pd", "status"]):
            with self.assertRaises(SystemExit):
                kr.build_parser().parse_args(other + ["--handback"])

    def test_handback_error_names_the_verdict(self):
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_PD_HANDBACK, bytes([4]))
        self.assertIn("no contract owned", str(e))

    def test_attach_default_5000mv(self):
        # no target = 5 V; nothing above 5 V without --confirm
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        r = kr.cmd_pd(link, self.args(pd_cmd="attach"))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([136, 19])))
        self.assertEqual(r["target_mv"], 5000)
        self.assertEqual(r["target_ma"], 0)
        self.assertEqual(r["state"], "ready")

    def test_attach_custom_mv_and_ma(self):
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        r = kr.cmd_pd(link, self.args(pd_cmd="attach", mv=12000, ma=2000))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([224, 46, 208, 7])))
        self.assertEqual(r["target_mv"], 12000)
        self.assertEqual(r["target_ma"], 2000)

    def test_attach_confirm_sends_the_flag_byte(self):
        # [mv, ma, flags]: bit0 confirm (the PD_REQUEST bit); ma rides along as 0 when only the flag is set
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="attach", mv=9000, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([40, 35, 0, 0, 1])))
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="attach", mv=9000, ma=3000, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([40, 35, 184, 11, 1])))

    def test_attach_without_confirm_sends_no_flag_byte(self):
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        kr.cmd_pd(link, self.args(pd_cmd="attach", mv=9000, confirm=False))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([40, 35])))

    def test_attach_refusals_name_the_reason(self):
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_PD_ATTACH, bytes([5]))
        self.assertIn("inline: a live sink is attached", str(e))     # inline: no Rd, no TX
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_PD_ATTACH, bytes([6]))
        self.assertIn("busy", str(e))
        e = kr.RemoteError(kr.STATUS_NEED_CONFIRM, kr.CMD_PD_ATTACH, bytes([0]))
        self.assertIn("needs --confirm", str(e))

    def test_attach_clamps_to_36000mv(self):
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        r = kr.cmd_pd(link, self.args(pd_cmd="attach", mv=40000))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([160, 140])))
        self.assertEqual(r["target_mv"], 36000)
        self.assertEqual(r["target_ma"], 0)

    def test_attach_only_mv_no_ma(self):
        link = self.Link({kr.CMD_PD_ATTACH: self.TRIG})
        r = kr.cmd_pd(link, self.args(pd_cmd="attach", mv=15000))
        self.assertEqual(link.calls[0], (kr.CMD_PD_ATTACH, bytes([152, 58])))
        self.assertEqual(r["target_mv"], 15000)
        self.assertEqual(r["target_ma"], 0)

    def test_parser(self):
        a = kr.build_parser().parse_args(["pd", "request", "3", "--mv", "9000", "--confirm"])
        self.assertEqual((a.pos, a.mv, a.confirm), (3, 9000, True))
        a = kr.build_parser().parse_args(["identify-device", "--confirm"])
        self.assertTrue(a.confirm)
        self.assertEqual(kr.ACTIONS["identify"], 6)
        a = kr.build_parser().parse_args(["pd", "attach"])
        self.assertEqual((a.pd_cmd, a.mv, a.ma, a.confirm), ("attach", 5000, 0, False))
        a = kr.build_parser().parse_args(["pd", "attach", "12000", "--confirm"])
        self.assertEqual((a.pd_cmd, a.mv, a.ma, a.confirm), ("attach", 12000, 0, True))
        a = kr.build_parser().parse_args(["pd", "attach", "15000", "2000"])
        self.assertEqual((a.pd_cmd, a.mv, a.ma, a.confirm), ("attach", 15000, 2000, False))

    IDENT_RUN = bytes([1, 1, 0, 0, 0, 0, 0, 0, 0, 0])
    IDENT_DONE = bytes([2, 6, 0x0B, 0x24, 1, 1, 0x00, 0x00, 0x20, 0x31, 0x2C, 0x91, 0x01, 0x00])
    IDENTITY = bytes([0, 1, 0xAC, 0x05, 0x34, 0x12, 0, 0, 0]) + b"Apple\x00iPad"

    def test_identify_polls_until_done(self):
        replies = [self.IDENT_RUN, self.IDENT_RUN, self.IDENT_DONE]

        class L(self.Link):
            def call(s, cmd, payload=b""):
                s.calls.append((cmd, bytes(payload)))
                if cmd == kr.CMD_IDENTIFY_DEVICE:
                    return kr.Response(cmd, 0, 0, replies.pop(0))
                return kr.Response(cmd, 0, 0, self.IDENTITY)
        link = L({})
        naps = []
        out = kr.cmd_identify_device(link, self.args(confirm=False, wait=5), sleep=naps.append)
        self.assertEqual(link.calls[0], (kr.CMD_IDENTIFY_DEVICE, bytes([1])))
        self.assertEqual(link.calls[1], (kr.CMD_IDENTIFY_DEVICE, bytes([0])))
        self.assertEqual(out["identify"]["state"], "done")
        self.assertEqual(out["identify"]["answered"], ["identity", "svids", "sink-cap"])
        self.assertEqual(out["identify"]["no_answer"], ["modes", "revision"][:0] + ["sink-cap", "revision"][:0] + ["modes", "revision"])
        self.assertEqual(out["identify"]["sink_pdos"], [0x0001912C])
        self.assertEqual(out["identity"]["vendor"], "Apple")
        self.assertEqual(len(naps), 2)
        self.assertIn("Apple", kr.render("identify-device", out))

    def test_identify_confirm_uses_op_2(self):
        link = self.Link({kr.CMD_IDENTIFY_DEVICE: self.IDENT_DONE, kr.CMD_GET_IDENTITY: self.IDENTITY})
        kr.cmd_identify_device(link, self.args(confirm=True, wait=1), sleep=lambda s: None)
        self.assertEqual(link.calls[0][1], bytes([2]))

    REREAD_RUN = bytes([1, 1, 15, 1, 0, 6, 0, 0, 0])
    REREAD_DONE = bytes([2, 7, 15, 15, 0, 6, 7, 1, 1])   # step 7 = RereadStep::Done

    def test_reread_decodes_the_status(self):
        d = kr.decode_reread(self.REREAD_DONE)
        self.assertEqual((d["state"], d["step"], d["mask"]), ("done", "done", 15))
        self.assertEqual(d["answered"], ["caps-ext", "identity", "svids", "get-cap"])
        self.assertEqual(d["skipped"], [])
        self.assertEqual((d["pdos_before"], d["pdos_last"], d["caps_rx"], d["avs"]), (6, 7, 1, True))
        r = kr.decode_reread(bytes([2, 7, 8, 0, 8, 6, 0, 0, 0]))
        self.assertEqual((r["answered"], r["skipped"], r["avs"]), ([], ["get-cap"], False))
        with self.assertRaises(kr.FrameError):
            kr.decode_reread(self.REREAD_DONE[:8])

    def test_reread_starts_with_the_mask_then_polls_until_done(self):
        replies = [self.REREAD_RUN, self.REREAD_RUN, self.REREAD_DONE]

        class L(self.Link):
            def call(s, cmd, payload=b""):
                s.calls.append((cmd, bytes(payload)))
                return kr.Response(cmd, 0, 0, replies.pop(0))
        link = L({})
        naps = []
        out = kr.cmd_pd_reread(link, self.args(mask=8, wait=5), sleep=naps.append)
        self.assertEqual(link.calls[0], (kr.CMD_PD_REREAD, bytes([1, 8])))
        self.assertEqual(link.calls[1], (kr.CMD_PD_REREAD, bytes([0])))
        self.assertEqual(out["state"], "done")
        self.assertEqual(len(naps), 2)
        text = kr.render("pd-reread", out)
        self.assertIn("pdos 6 -> 7", text)
        self.assertIn("avs=yes", text)

    def test_reread_lab_ops_pin_and_relook(self):
        class L(self.Link):
            def call(s, cmd, payload=b""):
                s.calls.append((cmd, bytes(payload)))
                return kr.Response(cmd, 0, 0, self.REREAD_DONE)
        link = L({})
        out = kr.cmd_pd_reread(link, self.args(mask=None, wait=1, pin=2, no_run=True), sleep=lambda s: None)
        self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([6, 2]))])   # op 6: pin the Rd on CC2, no run
        self.assertEqual(out["state"], "done")
        link = L({})
        kr.cmd_pd_reread(link, self.args(mask=None, wait=1, pin=0, no_run=True), sleep=lambda s: None)
        self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([6, 0]))])   # 0 releases it
        link = L({})
        kr.cmd_pd_reread(link, self.args(mask=None, wait=1, relook=True), sleep=lambda s: None)
        self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([7]))])      # op 7: Soft_Reset, Rd kept
        for ms, step in ((30, 3), (5, 1), (100000, 255)):                   # op 8: the Rd glitch in 10 ms steps, clamped to 1..255
            link = L({})
            kr.cmd_pd_reread(link, self.args(mask=None, wait=1, rd_glitch=ms), sleep=lambda s: None)
            self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([8, step]))])

    def test_reread_decodes_the_attach_flags(self):
        a = kr.decode_reread(bytes([1, 0, 15, 0, 0, 0, 0, 0, 2]))   # waiting for the contract
        self.assertEqual((a["state"], a["attaching"], a["attach_failed"], a["avs"]), ("running", True, False, False))
        self.assertIn("attaching", kr.render("pd-reread", a))
        f = kr.decode_reread(bytes([2, 7, 15, 0, 15, 0, 0, 0, 4]))   # no contract in time, nothing sent
        self.assertEqual((f["state"], f["attaching"], f["attach_failed"]), ("done", False, True))
        self.assertIn("no contract", kr.render("pd-reread", f))
        self.assertNotIn("no contract", kr.render("pd-reread", kr.decode_reread(self.REREAD_DONE)))

    def test_reread_keeps_polling_while_attaching_and_abort_sends_op_2(self):
        attach = bytes([1, 0, 15, 0, 0, 0, 0, 0, 2])
        replies = [attach, attach, self.REREAD_DONE]

        class L(self.Link):
            def call(s, cmd, payload=b""):
                s.calls.append((cmd, bytes(payload)))
                return kr.Response(cmd, 0, 0, replies.pop(0))
        link = L({})
        out = kr.cmd_pd_reread(link, self.args(mask=None, wait=30), sleep=lambda s: None)
        self.assertEqual(out["state"], "done")
        self.assertEqual(len(link.calls), 3)
        self.assertGreaterEqual(kr.REREAD_POLL_S, 12.0)   # attach 8 s + settle 1 s + steps 2.5 s
        link2 = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
        kr.cmd_pd_reread(link2, self.args(mask=None, wait=1, abort=True), sleep=lambda s: None)
        self.assertEqual(link2.calls, [(kr.CMD_PD_REREAD, bytes([2]))])
        self.assertTrue(kr.build_parser().parse_args(["pd-reread", "--abort"]).abort)

    def test_reread_default_mask_and_parser(self):
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
        kr.cmd_pd_reread(link, self.args(mask=None, wait=1), sleep=lambda s: None)
        self.assertEqual(link.calls[0][1], bytes([1, 15]))
        a = kr.build_parser().parse_args(["pd-reread", "--mask", "0x08"])
        self.assertEqual(a.mask, 8)
        a = kr.build_parser().parse_args(["pd-reread"])
        self.assertIsNone(a.mask)
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["pd-reread", "--mask", "32"])

    def test_reread_sets_the_sink_cap_ext_answer_before_the_run(self):
        for name, mode in (("silent", 0), ("skedb", 1), ("not-supported", 2)):
            link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
            kr.cmd_pd_reread(link, self.args(mask=8, wait=1, sink_cap_ext=name), sleep=lambda s: None)
            self.assertEqual(link.calls[0], (kr.CMD_PD_REREAD, bytes([3, mode])))
            self.assertEqual(link.calls[1], (kr.CMD_PD_REREAD, bytes([1, 8])))
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})   # not asked for: no op 3 call
        kr.cmd_pd_reread(link, self.args(mask=8, wait=1), sleep=lambda s: None)
        self.assertEqual(link.calls[0][1], bytes([1, 8]))
        a = kr.build_parser().parse_args(["pd-reread", "--sink-cap-ext", "skedb"])
        self.assertEqual(a.sink_cap_ext, "skedb")
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["pd-reread", "--sink-cap-ext", "bogus"])

    def test_reread_forces_the_cc_line_before_the_run_and_leaves_it_set(self):
        for line in (0, 1, 2):
            link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
            kr.cmd_pd_reread(link, self.args(mask=8, wait=1, line=line), sleep=lambda s: None)
            self.assertEqual(link.calls[0], (kr.CMD_PD_REREAD, bytes([4, line])))
            self.assertEqual(link.calls[1], (kr.CMD_PD_REREAD, bytes([1, 8])))
            self.assertEqual([c[1][0] for c in link.calls[2:]], [])   # stays set: for a charger test START as well
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})   # not asked for: no op 4 call at all
        kr.cmd_pd_reread(link, self.args(mask=8, wait=1), sleep=lambda s: None)
        self.assertEqual([c[1][0] for c in link.calls], [1])
        self.assertEqual(kr.build_parser().parse_args(["pd-reread", "--line", "1"]).line, 1)
        self.assertIsNone(kr.build_parser().parse_args(["pd-reread"]).line)
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["pd-reread", "--line", "3"])

    def test_reread_ra_mode_is_set_before_the_run_and_off_starts_nothing(self):
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
        kr.cmd_pd_reread(link, self.args(mask=8, wait=1, ra="on", line=2), sleep=lambda s: None)
        self.assertEqual(link.calls[0], (kr.CMD_PD_REREAD, bytes([5, 1])))
        self.assertEqual(link.calls[1], (kr.CMD_PD_REREAD, bytes([4, 2])))
        self.assertEqual(link.calls[2], (kr.CMD_PD_REREAD, bytes([1, 8])))
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})
        kr.cmd_pd_reread(link, self.args(mask=8, wait=1, ra="off"), sleep=lambda s: None)
        self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([5, 0]))])
        link = self.Link({kr.CMD_PD_REREAD: self.REREAD_DONE})   # --no-run: the mode only (charger test START follows)
        kr.cmd_pd_reread(link, self.args(mask=8, wait=1, ra="on", no_run=True), sleep=lambda s: None)
        self.assertEqual(link.calls, [(kr.CMD_PD_REREAD, bytes([5, 1]))])
        self.assertTrue(kr.decode_reread(bytes([2, 7, 8, 0, 8, 6, 0, 0, 8]))["ra"])
        self.assertIn(" ra", kr.render("pd-reread", kr.decode_reread(bytes([2, 7, 8, 0, 8, 6, 0, 0, 8]))))
        self.assertEqual(kr.build_parser().parse_args(["pd-reread", "--ra", "on"]).ra, "on")
        self.assertIsNone(kr.build_parser().parse_args(["pd-reread"]).ra)
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["pd-reread", "--ra", "maybe"])

    APPLE_HEAD_RUN = bytes([1, 0, 0, 0, 6])     # armed, why=running
    APPLE_HEAD_DONE = bytes([0, 1, 16, 16, 6])   # done, 16 reads, 16 ok
    APPLE_FIELDS = [b"Apple Inc.", b"30W USB-C Power Adapter", b"01030054", b"1.0", b"0x7003", b"TESTSERIAL0000000"]

    def _apple_link(self, head):
        link = self.Link({})
        def call(cmd, payload=b""):
            link.calls.append((cmd, bytes(payload)))
            if payload[:1] == b"\x01":
                return kr.Response(cmd, 0, 0, head)   # arm replies the header
            part = payload[1] if len(payload) > 1 else 0
            if part == 0:
                return kr.Response(cmd, 0, 0, head)
            return kr.Response(cmd, 0, 0, bytes([part]) + self.APPLE_FIELDS[part - 1] + b"\x00")
        link.call = call
        return link

    def test_apple_read_decodes_header_fields_and_renders(self):
        link = self._apple_link(self.APPLE_HEAD_DONE)
        out = kr.cmd_apple_read(link, self.args(wait=1), sleep=lambda s: None, clock=lambda: 100.0)
        self.assertEqual(link.calls[0], (kr.CMD_APPLE_READ, bytes([1])))          # arm first
        self.assertEqual(link.calls[1], (kr.CMD_APPLE_READ, bytes([0, 0])))        # then the header
        self.assertEqual((out["why"], out["reads"], out["ok"]), ("done", 16, 16))
        self.assertEqual(out["maker"], "Apple Inc.")
        self.assertEqual(out["product"], "30W USB-C Power Adapter")
        self.assertEqual(out["serial"], "TESTSERIAL0000000")                      # a made-up serial in tests only
        text = kr.render("apple-read", out)
        self.assertIn("done reads=16 ok=16", text)
        self.assertIn("product: 30W USB-C Power Adapter", text)
        d = kr.decode_apple_read_head(self.APPLE_HEAD_DONE)
        self.assertEqual((d["armed"], d["why"], d["nfields"]), (False, "done", 6))
        with self.assertRaises(kr.FrameError):
            kr.decode_apple_read_head(self.APPLE_HEAD_DONE[:4])
        with self.assertRaises(kr.FrameError):
            kr.decode_apple_read_field(bytes([3, b"x"[0], 0]), 2)   # wrong part
        self.assertEqual(kr.build_parser().parse_args(["apple-read"]).wait, kr.APPLE_POLL_S)

    def test_apple_read_polls_while_running_then_reads_the_fields(self):
        heads = [self.APPLE_HEAD_RUN, self.APPLE_HEAD_RUN, self.APPLE_HEAD_DONE]
        link = self._apple_link(self.APPLE_HEAD_DONE)
        def call(cmd, payload=b""):
            link.calls.append((cmd, bytes(payload)))
            if payload[:1] == b"\x01":
                return kr.Response(cmd, 0, 0, heads[0])
            if payload == bytes([0, 0]) and len(heads) > 1:
                return kr.Response(cmd, 0, 0, heads.pop(0))
            if payload == bytes([0, 0]):
                return kr.Response(cmd, 0, 0, self.APPLE_HEAD_DONE)
            part = payload[1]
            return kr.Response(cmd, 0, 0, bytes([part]) + self.APPLE_FIELDS[part - 1] + b"\x00")
        link.call = call
        ticks = iter([100.0, 100.1, 100.2, 100.3, 100.4, 100.5, 100.6, 100.7, 100.8, 100.9])
        out = kr.cmd_apple_read(link, self.args(wait=8), sleep=lambda s: None, clock=lambda: next(ticks))
        self.assertEqual(out["why"], "done")
        self.assertGreaterEqual(len([c for c in link.calls if c[1] == bytes([0, 0])]), 2)   # polled while running
        with self.assertRaises(kr.RemoteError) as cm:
            raise kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_APPLE_READ, bytes([1]))
        self.assertIn("inline", str(cm.exception))


class PageMap(unittest.TestCase):
    """The remote's page names and indices follow the firmware enums in include/k2/ui/ui_types.hpp."""

    @staticmethod
    def enum_names(name):
        import re
        src = (ROOT / "include" / "k2" / "ui" / "ui_types.hpp").read_text()
        body = re.search(r"enum class %s\s*:\s*uint8_t\s*\{([^}]*)\}" % name, src).group(1)
        return [n.strip().lower() for n in body.split(",") if n.strip() and n.strip() != "Count"]

    def test_rotation_pages_match_the_firmware_enum(self):
        pages = self.enum_names("Page")
        self.assertEqual(kr.PAGES[:kr.ROTATION_PAGES], pages)
        self.assertEqual(kr.ROTATION_PAGES, len(pages))

    def test_menus_follow_the_rotation_pages(self):
        menus = self.enum_names("Menu")
        self.assertEqual([m for m in kr.PAGES[kr.ROTATION_PAGES:]], menus)

    def test_page_name_and_index_agree(self):
        for i, n in enumerate(self.enum_names("Page")):
            self.assertEqual(kr.page_index(n), i)
            self.assertEqual(kr.page_index(str(i)), i)
            self.assertEqual(kr.page_name(i), n)

    def test_page_rejects_unknown(self):
        for bad in ("match", "8", "-1", "toolz"):
            with self.assertRaises(ValueError):
                kr.page_index(bad)

    def test_page_accepts_the_connection_tab_form(self):
        # "connection sink" is the long form of the Connection tab name "sink"; the short form keeps working.
        self.assertEqual(kr.page_index("connection source"), kr.CONN_ALIAS_BASE)
        self.assertEqual(kr.page_index("connection cable"), kr.CONN_ALIAS_BASE + 1)
        self.assertEqual(kr.page_index("connection sink"), kr.CONN_ALIAS_BASE + 2)
        self.assertEqual(kr.page_index("Connection  Sink"), kr.CONN_ALIAS_BASE + 2)
        self.assertEqual(kr.page_index("connection"), kr.PAGES.index("connection"))
        self.assertEqual(kr.page_index("sink"), kr.CONN_ALIAS_BASE + 2)
        with self.assertRaises(ValueError):
            kr.page_index("connection bogus")


class ChargerSweepCli(unittest.TestCase):
    """`charger-sweep on|off` and `charger-avs-sweep on|off`: turn the PPS / AVS deep sweep of the NEXT charger
    run on or off (the firmware commands 0x0F / 0x54, opt-in, never in a preset)."""

    class Link:
        def __init__(self, replies):
            self.replies, self.sent = list(replies), []

        def call(self, cmd, payload=b""):
            self.sent.append((cmd, bytes(payload)))
            return kr.Response(cmd, len(self.sent), 0, self.replies.pop(0))

    def invoke(self, argv, replies):
        link = self.Link(replies)
        a = kr.build_parser().parse_args(argv)
        return kr.COMMANDS[a.command](link, a), link

    def test_avs_sweep_on_sends_0x54_with_one(self):
        r, link = self.invoke(["charger-avs-sweep", "on"], [bytes([1])])
        self.assertEqual(link.sent, [(kr.CMD_CHARGER_AVS_SWEEP, b"\x01")])
        self.assertEqual(r, {"avs_sweep": True})

    def test_avs_sweep_off_sends_zero(self):
        r, link = self.invoke(["charger-avs-sweep", "off"], [bytes([0])])
        self.assertEqual(link.sent, [(kr.CMD_CHARGER_AVS_SWEEP, b"\x00")])
        self.assertEqual(r, {"avs_sweep": False})

    def test_pps_sweep_on_sends_0x0f_with_one(self):
        r, link = self.invoke(["charger-sweep", "on"], [bytes([1])])
        self.assertEqual(link.sent, [(kr.CMD_CHARGER_SWEEP, b"\x01")])
        self.assertEqual(r, {"pps_sweep": True})

    def test_bad_state_is_rejected_by_the_parser(self):
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["charger-avs-sweep", "maybe"])


class Scripts(unittest.TestCase):
    """Back-to-back presses and long actions (hardware check items 3 and 4)."""

    class Args:
        def __init__(self, **kw):
            self.__dict__.update(button="plus", ms=0, long=True, name="emarker")
            self.__dict__.update(kw)

    class Scripted:
        """Replies to each request from a queue of (status) values; records timeouts asked of read()."""
        def __init__(self, statuses):
            self.statuses, self.sent, self.reads = list(statuses), [], []
            self.pending = []

        def write(self, frame):
            self.sent.append(frame)
            st = self.statuses.pop(0) if self.statuses else 0
            self.pending.append(rsp(frame[2], frame[3], st))

        def read(self, timeout_ms):
            self.reads.append(timeout_ms)
            return self.pending.pop(0) if self.pending else b""

    def test_press_retries_while_busy(self):
        t = self.Scripted([kr.STATUS_BUSY, kr.STATUS_BUSY, kr.STATUS_OK])
        link = kr.RemoteLink(t, timeout_s=0.2)
        naps = []
        kr.cmd_press(link, self.Args(), sleep=naps.append)
        self.assertEqual(len(t.sent), 3)
        self.assertEqual(len(naps), 2)

    def test_press_gives_up_after_the_press_time_plus_margin(self):
        t = self.Scripted([kr.STATUS_BUSY] * 1000)
        link = kr.RemoteLink(t, timeout_s=0.2)
        naps = []
        with self.assertRaises(kr.RemoteError):
            kr.cmd_press(link, self.Args(), sleep=naps.append)
        self.assertGreaterEqual(sum(naps), kr.LONG_PRESS_MS / 1000)

    def test_action_waits_longer_than_a_plain_call(self):
        class Slow(self.Scripted):
            pass
        t = Slow([])
        link = kr.RemoteLink(t, timeout_s=1.0)
        kr.cmd_action(link, self.Args())
        self.assertEqual(link.timeout_s, 1.0)         # restored afterwards
        self.assertGreater(kr.ACTION_TIMEOUT_S, 5.0)

    def test_action_confirm_is_live_for_fc_scan(self):
        """fc-scan raises VBUS and drives D+/D-: the firmware wants the confirm byte, so --confirm must reach the wire."""
        self.assertEqual(kr.ACTIONS["fc-scan"], 4)
        self.assertEqual(kr.ACTIONS["fc-reset"], 5)
        t = self.Scripted([])
        link = kr.RemoteLink(t, timeout_s=1.0)
        kr.cmd_action(link, self.Args(name="fc-scan"))
        self.assertEqual(t.sent[-1][5:7], bytes([4, 0]))       # no --confirm: the K2 answers need-confirm
        kr.cmd_action(link, self.Args(name="fc-scan", confirm=True))
        self.assertEqual(t.sent[-1][5:7], bytes([4, 1]))

    def test_action_need_confirm_is_a_clean_refusal(self):
        t = self.Scripted([kr.STATUS_NEED_CONFIRM])
        link = kr.RemoteLink(t, timeout_s=1.0)
        with self.assertRaises(kr.RemoteError) as cm:
            kr.cmd_action(link, self.Args(name="fc-scan"))
        self.assertEqual(cm.exception.status, kr.STATUS_NEED_CONFIRM)
        self.assertIn("--confirm", str(cm.exception))

    def test_action_help_names_what_needs_confirm(self):
        import contextlib, io
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["action", "--help"])
        text = " ".join(out.getvalue().split())
        self.assertIn("fc-scan", text)
        self.assertIn("5 V", text)
        a = kr.build_parser().parse_args(["action", "fc-scan", "--confirm"])
        self.assertTrue(a.confirm)

    def test_action_timeout_on_a_live_device_is_not_a_dfu_hint(self):
        msg = kr.timeout_message(alive=True)
        self.assertNotIn("DFU", msg)
        self.assertIn("did not finish", msg)
        self.assertIn("DFU", kr.timeout_message(alive=False))


class FactoryWipe(unittest.TestCase):
    """factory-wipe --confirm: erase the file system and legacy stores, then DFU (reply comes first)."""
    OK = bytes([0]) + (152).to_bytes(2, "little") * 3 + bytes(2) + bytes(4) + bytes([0])

    def link(self, sent, info):
        class Link:
            timeout_s = 1.0
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload, self_.timeout_s))
                return kr.parse_response(rsp(cmd, 1, 0, info))
        return Link()

    def run_cmd(self, argv, sent, info=None):
        a = kr.build_parser().parse_args(argv)
        return kr.COMMANDS["factory-wipe"](self.link(sent, self.OK if info is None else info), a)

    def test_id_and_payload(self):
        self.assertEqual(kr.CMD_FACTORY_WIPE, 0x33)
        self.assertEqual(kr.FACTORY_WIPE_MAGIC, b"WIPE")           # u32 0x45504957 little endian
        self.assertEqual(kr.factory_wipe_payload(), b"WIPE")
        self.assertEqual(kr.factory_wipe_payload("iwdg"), b"WIPE\x01")

    def test_confirm_is_required(self):
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["factory-wipe"])

    def test_success_sends_magic_with_a_long_timeout(self):
        sent = []
        r = self.run_cmd(["factory-wipe", "--confirm"], sent)
        self.assertEqual(sent[0][:2], (kr.CMD_FACTORY_WIPE, b"WIPE"))
        self.assertGreaterEqual(sent[0][2], 20.0)                  # the erase blocks the firmware for seconds
        self.assertEqual((r["sectors"], r["erased"], r["verified"], r["failed"]), (152, 152, 152, 0))
        self.assertEqual(r["err"], "ok")
        text = kr.render("factory-wipe", r)
        self.assertIn("152 sectors erased and verified blank", text)
        self.assertIn("DFU", text)
        self.assertIn("k2_flasher.py flash", text)

    def test_iwdg_reset_byte(self):
        sent = []
        self.run_cmd(["factory-wipe", "--confirm", "--reset", "iwdg"], sent)
        self.assertEqual(sent[0][1], b"WIPE\x01")

    def test_timeout_is_restored(self):
        sent = []
        link = self.link(sent, self.OK)
        kr.cmd_factory_wipe(link, kr.build_parser().parse_args(["factory-wipe", "--confirm"]))
        self.assertEqual(link.timeout_s, 1.0)

    def test_erase_failure_raises_with_address_and_status(self):
        info = bytes([1]) + (152).to_bytes(2, "little") + (10).to_bytes(2, "little") + bytes(4) \
            + (0x080B3000 + 10 * 0x800).to_bytes(4, "little") + bytes([6])
        with self.assertRaises(kr.WipeFailed) as cm:
            self.run_cmd(["factory-wipe", "--confirm"], [], info)
        msg = str(cm.exception)
        self.assertIn("erase failed", msg)
        self.assertIn("0x080B8000", msg)
        self.assertIn("10 of 152", msg)
        self.assertIn("NOT in DFU", msg)

    def test_verify_failure_raises_with_count(self):
        info = bytes([2]) + (152).to_bytes(2, "little") * 2 + (150).to_bytes(2, "little") + (2).to_bytes(2, "little") \
            + (0x080FA000).to_bytes(4, "little") + bytes([0])
        with self.assertRaises(kr.WipeFailed) as cm:
            self.run_cmd(["factory-wipe", "--confirm"], [], info)
        self.assertIn("2 sectors do not read blank", str(cm.exception))
        self.assertIn("0x080FA000", str(cm.exception))

    def test_short_reply_is_an_error(self):
        with self.assertRaises(Exception):
            self.run_cmd(["factory-wipe", "--confirm"], [], bytes(5))


class FlashDump(unittest.TestCase):
    """flash-dump DIR: read-only dump of 0x080E0000..0x08100000 through the app (FLASH_DUMP 0x34)."""
    BASE, END = 0x080E0000, 0x08100000

    @staticmethod
    def data(addr, n):
        return bytes(((addr + i) * 7 + 1) & 0xFF for i in range(n))

    def link(self, sent, flip=None):
        data = self.data

        class Link:
            timeout_s = 1.0
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload))
                addr, n = int.from_bytes(payload[:4], "little"), payload[4]
                body = bytearray(data(addr, n))
                if flip and len(sent) in flip:
                    body[0] ^= 0xFF                      # a bit error on this call only
                return kr.parse_response(rsp(cmd, 1, 0, payload[:5] + bytes(body)))
        return Link()

    def run_cmd(self, outdir, sent, flip=None):
        a = kr.build_parser().parse_args(["flash-dump", str(outdir)])
        return kr.COMMANDS["flash-dump"](self.link(sent, flip), a)

    def test_id_and_payload(self):
        self.assertEqual(kr.CMD_FLASH_DUMP, 0x34)
        self.assertEqual(kr.flash_dump_payload(0x080E0000, 48), bytes([0, 0, 0x0E, 0x08, 48]))

    def test_dumps_the_whole_area_twice_and_writes_the_file(self):
        import tempfile
        sent = []
        with tempfile.TemporaryDirectory() as d:
            r = self.run_cmd(Path(d) / "dump", sent)
            blob = (Path(d) / "dump" / "native_nvram.bin").read_bytes()
        self.assertEqual(blob, self.data(self.BASE, self.END - self.BASE))
        self.assertEqual(r["size"], 0x20000)
        self.assertEqual(r["range"], "0x080E0000-0x08100000")
        self.assertTrue(r["two_pass_verified"])
        self.assertTrue(all(c == kr.CMD_FLASH_DUMP and p[4] <= 48 for c, p in sent))
        self.assertEqual(len(sent), 2 * -(-0x20000 // 48))      # two passes, the last chunk shorter
        self.assertEqual(int.from_bytes(sent[-1][1][:4], "little") + sent[-1][1][4], self.END)   # never past the end
        text = kr.render("flash-dump", r)
        self.assertIn("0x080E0000-0x08100000", text)
        self.assertIn(r["sha256"], text)

    def test_a_mismatch_between_the_passes_is_an_error(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(kr.DumpFailed) as cm:
                self.run_cmd(Path(d) / "dump", [], flip={2731 + 5})   # a bit error in the second pass
        self.assertIn("differ", str(cm.exception))

    def test_an_echo_of_the_wrong_address_is_an_error(self):
        class Link:
            timeout_s = 1.0
            def call(self_, cmd, payload=b""):
                return kr.parse_response(rsp(cmd, 1, 0, bytes(5) + bytes(48)))
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(kr.DumpFailed):
                kr.cmd_flash_dump(Link(), kr.build_parser().parse_args(["flash-dump", d]))


class BootInfo(unittest.TestCase):
    """bootinfo: how this boot decided its mode (BOOT_INFO 0x35): the K2B1 text in 48-byte chunks, then 9-byte power samples."""
    TEXT = ("K2B1 fw=1.0 g=a1b2c3d-dirty p=1 t=4321 vb=5012 c1=3290 c2=0 pl=1 m=plus k=8 pa=D rf=24000000 w=0 pin=2,1,1,0 "
            "mk=- mv=650:2>1:0:3300:150 sw=2@50,2@600,1@3001 n=3s2,4c3,652s1,+4")

    def samples(self, n=7):
        return [(5000 + i, 3300 + i, i, 9000 + i, i == 3) for i in range(n)]

    def link(self, text, samples, sent):
        class Link:
            timeout_s = 1.0
            def call(self_, cmd, payload=b""):
                sent.append((cmd, payload))
                part, off = payload[0], int.from_bytes(payload[1:3], "little")
                if part == 0:
                    raw = text.encode()
                    body = raw[off:off + 48]
                    head = bytes([0]) + payload[1:3] + len(raw).to_bytes(2, "little") + bytes([len(body)])
                    return kr.parse_response(rsp(cmd, 1, 0, head + body))
                chunk = samples[off:off + 5]
                body = b"".join(b"".join(x.to_bytes(2, "little") for x in s[:4]) + bytes([1 if s[4] else 0]) for s in chunk)
                head = bytes([1]) + payload[1:3] + len(samples).to_bytes(2, "little") + bytes([len(chunk)])
                return kr.parse_response(rsp(cmd, 1, 0, head + body))
        return Link()

    def test_id_and_payload(self):
        self.assertEqual(kr.CMD_BOOT_INFO, 0x35)
        self.assertEqual(kr.boot_info_payload(0, 0), bytes([0, 0, 0]))
        self.assertEqual(kr.boot_info_payload(1, 0x0105), bytes([1, 5, 1]))

    def test_parse_the_text_line(self):
        f = kr.parse_bootinfo_text(self.TEXT)
        self.assertEqual((f["t_ms"], f["vbus_mv"], f["cc1_mv"], f["cc2_mv"], f["plus_held"]), (4321, 5012, 3290, 0, True))
        self.assertEqual((f["fw"], f["git"], f["proto"]), ("1.0", "a1b2c3d-dirty", 1))
        self.assertEqual((f["mode"], f["keys"], f["pa"], f["reset_flags"], f["warm"]), ("plus", 8, 0xD, 0x24000000, False))
        self.assertEqual(f["keys_pressed"], ["PLUS"])
        self.assertEqual(f["pins_low"], ["PLUS"])
        self.assertEqual(f["pin"], {"initial": 2, "now": 1, "boot": 1, "inherited": False})
        self.assertIsNone(f["mark"])
        self.assertEqual(f["move"], {"ms": 650, "from": 2, "to": 1, "vbus_mv": 0, "cc1_mv": 3300, "cc2_mv": 150})
        self.assertEqual(f["readbacks"], [{"ms": 50, "sw0": 2}, {"ms": 600, "sw0": 2}, {"ms": 3001, "sw0": 1}])
        self.assertEqual(f["notes"], [{"ms": 3, "kind": "sw0", "val": 2}, {"ms": 4, "kind": "cc", "val": 3},
                                      {"ms": 652, "kind": "sw0", "val": 1}])
        self.assertEqual(f["notes_not_shown"], 4)

    def test_parse_no_move_no_readbacks_and_a_pin_mark(self):
        f = kr.parse_bootinfo_text("K2B1 fw=dev g=0000000 p=1 t=9 vb=20000 c1=0 c2=0 pl=0 m=plain k=0 pa=F rf=8000000 w=1 pin=2,2,2,1 mk=B502 mv=- sw=- n=-")
        self.assertEqual((f["mode"], f["warm"], f["mark"], f["move"], f["readbacks"], f["notes"], f["pins_low"]),
                         ("plain", True, 0xB502, None, [], [], []))
        self.assertEqual(f["notes_not_shown"], 0)

    def test_not_a_k2b1_line_is_an_error(self):
        with self.assertRaises(ValueError):
            kr.parse_bootinfo_text("hello")

    def test_reads_every_chunk_and_every_sample_page(self):
        sent = []
        a = kr.build_parser().parse_args(["bootinfo"])
        r = kr.COMMANDS["bootinfo"](self.link(self.TEXT, self.samples(), sent), a)
        self.assertEqual(r["qr_text"], self.TEXT)
        self.assertEqual(r["bootinfo"]["mode"], "plus")
        self.assertEqual(len(r["samples"]), 7)
        self.assertEqual(r["samples"][3], {"ms": 150, "vbus_mv": 5003, "cc1_mv": 3303, "cc2_mv": 3, "aux_mv": 9003, "plus": True})
        offs = [(p[0], int.from_bytes(p[1:3], "little")) for c, p in sent]
        n = len(self.TEXT.encode())
        self.assertEqual(offs, [(0, o) for o in range(0, n, 48)] + [(1, 0), (1, 5)])
        self.assertTrue(all(c == kr.CMD_BOOT_INFO for c, p in sent))

    def test_render_has_the_decision_and_the_json_flag_is_global(self):
        sent = []
        a = kr.build_parser().parse_args(["--json", "bootinfo"])
        self.assertTrue(a.json)
        r = kr.COMMANDS["bootinfo"](self.link(self.TEXT, self.samples(2), sent), a)
        text = kr.render("bootinfo", r)
        self.assertIn("mode plus", text)
        self.assertIn("firmware 1.0 build a1b2c3d-dirty (proto 1)", text)
        self.assertIn("Rd moved CC2 -> CC1 at 650 ms", text)
        self.assertIn("SWITCHES0 read back", text)
        self.assertIn(self.TEXT, text)

    def test_a_reply_for_another_window_is_an_error(self):
        class Link:
            timeout_s = 1.0
            def call(self_, cmd, payload=b""):
                return kr.parse_response(rsp(cmd, 1, 0, bytes([0, 9, 0, 5, 0, 1]) + b"x"))
        with self.assertRaises(Exception):
            kr.COMMANDS["bootinfo"](Link(), kr.build_parser().parse_args(["bootinfo"]))


class Match(unittest.TestCase):
    PAYLOAD =bytes([1, 1, 0, 2, 3, 2, 2, 0xE0, 0x2E, 0xB8, 0x0B, 0xB8, 0x0B, 0x84, 0x03, 0x98, 0x3A, 0x64, 0x00, 3, 1])

    def test_decode(self):
        r = kr.decode_match(self.PAYLOAD)
        self.assertEqual((r["verdict"], r["flags"], r["want_pos"]), ("wants-higher-v", ["wants-higher-v"], 2))
        self.assertEqual((r["want_mv"], r["want_ma"], r["have_ma"], r["src_max_mv"], r["src_max_w"]),
                         (12000, 3000, 900, 15000, 100))
        self.assertEqual(r["rows"], ["ok", "selected"])
        self.assertTrue(r["caps_seen"] and r["request_seen"] and not r["sink_ok"])

    def test_short_frames_raise(self):
        with self.assertRaises(Exception):
            kr.decode_match(self.PAYLOAD[:10])
        with self.assertRaises(Exception):
            kr.decode_match(self.PAYLOAD[:-1])

    def test_command_and_render(self):
        link = ActivePd.Link({kr.CMD_PD_MATCH: self.PAYLOAD})
        r = kr.cmd_match(link, None)
        self.assertEqual(link.calls, [(kr.CMD_PD_MATCH, b"")])
        self.assertIn("charger max 15 V 100 W", kr.render("match", r))


class StopCommand(unittest.TestCase):
    """`stop` (0x4B): no payload, no confirm; the reply is [active mask, vbus_mv i32 LE, fc_release_busy]."""

    PAYLOAD = bytes([0x0B]) + (9123).to_bytes(4, "little", signed=True) + bytes([1])

    def test_id(self):
        self.assertEqual(kr.CMD_STOP, 0x4B)

    def test_decode(self):
        r = kr.decode_stop(self.PAYLOAD)
        self.assertEqual(r["active"], ["test", "fc", "spoof"])
        self.assertEqual(r["active_mask"], 0x0B)
        self.assertEqual(r["vbus_mv"], 9123)
        self.assertIs(r["fc_release_busy"], True)
        idle = kr.decode_stop(bytes([0]) + (-1).to_bytes(4, "little", signed=True) + bytes([0]))
        self.assertEqual(idle["active"], [])
        self.assertEqual(idle["vbus_mv"], -1)
        with self.assertRaises(Exception):
            kr.decode_stop(self.PAYLOAD[:5])

    def test_command_sends_no_payload_and_needs_no_confirm(self):
        link = ActivePd.Link({kr.CMD_STOP: self.PAYLOAD})
        r = kr.cmd_stop(link, None)
        self.assertEqual(link.calls, [(kr.CMD_STOP, b"")])
        self.assertEqual(r["vbus_mv"], 9123)
        p = kr.build_parser()
        a = p.parse_args(["stop"])
        self.assertEqual(a.command, "stop")
        self.assertFalse(hasattr(a, "confirm"))
        self.assertIs(kr.COMMANDS["stop"], kr.cmd_stop)

    def test_render(self):
        text = kr.render("stop", kr.decode_stop(self.PAYLOAD))
        self.assertIn("stopped: test, fc, spoof", text)
        self.assertIn("9.12 V", text)
        self.assertIn("fc release running", text)
        idle = kr.render("stop", kr.decode_stop(bytes([0]) + (5004).to_bytes(4, "little") + bytes([0])))
        self.assertIn("nothing was active", idle)
        self.assertIn("5.00 V", idle)
        self.assertNotIn("fc release", idle)
        # the monitor not ready: no number is invented
        self.assertIn("VBUS n/a", kr.render("stop", kr.decode_stop(bytes([0]) + (-1).to_bytes(4, "little", signed=True) + bytes([0]))))


class LeaseCommand(unittest.TestCase):
    """The host dead-man lease (0x4C): `[0]` status, `[1, seconds u16 LE]` set; the reply is
    [armed, left_s u16 LE, length_s u16 LE, ended]. `--lease N` / `--hold` ride on the commands that can arm it."""

    ARMED = bytes([1]) + (17).to_bytes(2, "little") + (30).to_bytes(2, "little") + bytes([0])
    IDLE = bytes([0, 0, 0]) + (30).to_bytes(2, "little") + bytes([1])   # ended: expired

    class Link:
        """Replays a list of lease replies (the last one repeats); other commands answer empty or from `replies`."""
        def __init__(self, lease_replies, replies=None, on_status=None):
            self.lease_replies, self.replies, self.calls, self.on_status = list(lease_replies), replies or {}, [], on_status

        def call(self, cmd, payload=b""):
            self.calls.append((cmd, bytes(payload)))
            if cmd == kr.CMD_LEASE:
                if payload[:1] == b"\x00" and self.on_status:
                    self.on_status()
                r = self.lease_replies[0] if len(self.lease_replies) == 1 else self.lease_replies.pop(0)
                if isinstance(r, Exception):
                    raise r
                return kr.Response(cmd, 0, 0, r)
            r = self.replies.get(cmd, ActivePd.TRIG if cmd == kr.CMD_PD_ATTACH else b"")   # attach answers a trigger snapshot
            return kr.Response(cmd, 0, 0, r)

    def test_id(self):
        self.assertEqual(kr.CMD_LEASE, 0x4C)

    def test_decode(self):
        r = kr.decode_lease(self.ARMED)
        self.assertEqual(r, {"armed": True, "left_s": 17, "length_s": 30, "ended": "none"})
        r = kr.decode_lease(self.IDLE)
        self.assertEqual((r["armed"], r["left_s"], r["ended"]), (False, 0, "expired"))
        self.assertEqual([kr.decode_lease(bytes([0, 0, 0, 30, 0, e]))["ended"] for e in range(2, 6)],
                         ["host gone", "released", "settled", "test running"])
        with self.assertRaises(Exception):
            kr.decode_lease(self.ARMED[:5])

    def test_lease_command_reads_and_sets(self):
        link = self.Link([self.ARMED])
        p = kr.build_parser()
        self.assertEqual(kr.cmd_lease(link, p.parse_args(["lease"]))["left_s"], 17)
        self.assertEqual(link.calls[-1], (kr.CMD_LEASE, b"\x00"))
        kr.cmd_lease(link, p.parse_args(["lease", "45"]))
        self.assertEqual(link.calls[-1], (kr.CMD_LEASE, bytes([1, 45, 0])))
        kr.cmd_lease(link, p.parse_args(["lease", "600"]))
        self.assertEqual(link.calls[-1], (kr.CMD_LEASE, bytes([1, 0x58, 0x02])))
        kr.cmd_lease(link, p.parse_args(["lease", "0"]))                # 0 = the firmware default
        self.assertEqual(link.calls[-1], (kr.CMD_LEASE, bytes([1, 0, 0])))
        self.assertIs(kr.COMMANDS["lease"], kr.cmd_lease)

    def test_seconds_above_600_never_leave_the_tool(self):
        p = kr.build_parser()
        for argv in (["lease", "601"], ["lease", "-1"], ["pd", "attach", "9000", "--confirm", "--lease", "601"],
                     ["fc", "scan", "--confirm", "--lease", "9999"]):
            with self.assertRaises(SystemExit):
                with contextlib.redirect_stderr(io.StringIO()):
                    p.parse_args(argv)

    def test_the_arming_commands_take_lease_and_hold(self):
        p = kr.build_parser()
        for argv in (["pd", "request", "2", "--confirm"], ["pd", "attach", "9000", "--confirm"],
                     ["fc", "scan", "--confirm"], ["fc", "trigger", "qc3", "--volts", "9", "--confirm"],
                     ["action", "fc-scan", "--confirm"]):
            a = p.parse_args(argv)
            self.assertIsNone(a.lease, argv)
            self.assertFalse(a.hold, argv)
            a = p.parse_args(argv + ["--lease", "12", "--hold"])
            self.assertEqual(a.lease, 12, argv)
            self.assertTrue(a.hold, argv)
        for argv in (["pd", "release"], ["pd", "status"], ["fc", "status"], ["fc", "reset"], ["stop"], ["ping"]):
            self.assertFalse(hasattr(p.parse_args(argv), "hold"), argv)

    def run_cmd(self, argv, link, **kw):
        a = kr.build_parser().parse_args(argv)
        out = []
        r = kr.run_command(link, a, out=out.append, **kw)
        return a, r, out

    def test_lease_is_set_before_the_command_and_read_after(self):
        link = self.Link([bytes(6), self.ARMED], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "9000", "--confirm", "--lease", "7"], link)
        self.assertEqual([c for c, _ in link.calls],
                         [kr.CMD_LEASE, kr.CMD_PD_ATTACH, kr.CMD_LEASE])
        self.assertEqual(link.calls[0][1], bytes([1, 7, 0]))             # the set, 7 s
        self.assertEqual(link.calls[2][1], b"\x00")                       # then the status read
        self.assertEqual(r["lease"]["armed"], True)
        self.assertEqual(r["lease"]["length_s"], 30)
        self.assertEqual(len(out), 1)                                    # exactly one line, and it says what the K2 will do
        self.assertIn("lease armed", out[0])
        self.assertIn("30 s", out[0])
        self.assertIn("stop", out[0])

    def test_no_lease_option_sends_no_set_and_still_reports(self):
        link = self.Link([self.ARMED], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "9000", "--confirm"], link)
        self.assertEqual([c for c, _ in link.calls], [kr.CMD_PD_ATTACH, kr.CMD_LEASE])
        self.assertEqual(len(out), 1)

    def test_a_command_that_did_not_arm_prints_nothing(self):
        link = self.Link([self.IDLE], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "5000"], link)
        self.assertEqual(out, [])
        self.assertNotIn("lease", r)

    def test_non_arming_commands_never_touch_the_lease(self):
        link = self.Link([self.ARMED], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG, kr.CMD_PING: bytes(20)})
        a, r, out = self.run_cmd(["pd", "status"], link)
        self.assertEqual([c for c, _ in link.calls], [kr.CMD_PD_TRIGGER_STATUS])

    def test_an_older_firmware_without_the_lease_command_still_works(self):
        link = self.Link([kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_LEASE)], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "9000", "--confirm"], link)
        self.assertEqual(out, [])
        self.assertEqual(r["state"], "ready")
        # but an explicit --lease on such a firmware is an error the user must see
        with self.assertRaises(kr.RemoteError):
            self.run_cmd(["pd", "attach", "9000", "--confirm", "--lease", "10"], link)

    def test_hold_pings_until_ctrl_c_then_stops(self):
        stop = bytes([0x04]) + (5000).to_bytes(4, "little") + bytes([0])
        link = self.Link([self.ARMED], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG, kr.CMD_STOP: stop})
        sleeps = []

        def sleep(s):
            sleeps.append(s)
            if len(sleeps) == 4:
                raise KeyboardInterrupt

        a, r, out = self.run_cmd(["pd", "attach", "9000", "--confirm", "--hold"], link, sleep=sleep)
        self.assertEqual(sleeps, [10.0] * 4)                             # length 30 s: a third of it, at most 10 s
        pings = [c for c, p in link.calls if c == kr.CMD_LEASE]
        self.assertEqual(len(pings), 1 + 3)                              # the arm read, then one ping per completed sleep
        self.assertEqual(link.calls[-1], (kr.CMD_STOP, b""))             # Ctrl-C sends exactly one stop, last
        self.assertEqual(sum(1 for c, _ in link.calls if c == kr.CMD_STOP), 1)
        self.assertTrue(any("holding" in line for line in out))
        self.assertTrue(any("stop:" in line for line in out))

    def test_hold_interval_follows_a_short_lease(self):
        short = bytes([1]) + (6).to_bytes(2, "little") + (6).to_bytes(2, "little") + bytes([0])
        link = self.Link([short], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG, kr.CMD_STOP: bytes(6)})
        sleeps = []

        def sleep(s):
            sleeps.append(s)
            raise KeyboardInterrupt

        self.run_cmd(["pd", "attach", "9000", "--confirm", "--lease", "6", "--hold"], link, sleep=sleep)
        self.assertEqual(sleeps, [2.0])
        tiny = bytes([1]) + (2).to_bytes(2, "little") + (2).to_bytes(2, "little") + bytes([0])
        link = self.Link([tiny], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG, kr.CMD_STOP: bytes(6)})
        sleeps.clear()
        self.run_cmd(["pd", "attach", "9000", "--confirm", "--lease", "2", "--hold"], link, sleep=sleep)
        self.assertEqual(sleeps, [1.0])                                  # never faster than once a second

    def test_hold_ends_without_a_stop_when_the_lease_ended_by_itself(self):
        # the lease ended because a charger test took the ports: a stop now would cancel that test
        ended = bytes([0, 0, 0]) + (30).to_bytes(2, "little") + bytes([5])
        link = self.Link([self.ARMED, ended], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "9000", "--confirm", "--hold"], link, sleep=lambda s: None)
        self.assertNotIn(kr.CMD_STOP, [c for c, _ in link.calls])
        self.assertTrue(any("lease ended: test running" in line for line in out))

    def test_hold_on_a_command_that_armed_nothing_says_so(self):
        link = self.Link([self.IDLE], {kr.CMD_PD_TRIGGER_STATUS: ActivePd.TRIG})
        a, r, out = self.run_cmd(["pd", "attach", "5000", "--hold"], link, sleep=lambda s: self.fail("must not wait"))
        self.assertTrue(any("nothing to hold" in line for line in out))
        self.assertNotIn(kr.CMD_STOP, [c for c, _ in link.calls])

    def test_render(self):
        self.assertIn("armed", kr.render("lease", kr.decode_lease(self.ARMED)))
        self.assertIn("17 s left of 30 s", kr.render("lease", kr.decode_lease(self.ARMED)))
        idle = kr.render("lease", kr.decode_lease(self.IDLE))
        self.assertIn("not armed", idle)
        self.assertIn("expired", idle)


if __name__ == "__main__":
    unittest.main()
