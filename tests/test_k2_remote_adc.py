# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for the analog front end part of k2_remote: the extended GET_STATE, ADC_RAW and RIPPLE.

The byte vectors are the same literals as tests/test_remote_proto.cpp.
"""
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import k2_remote as kr


def rsp(cmd, seq, status, payload=b""):
    return (bytes([0xA6, 0x4B, cmd, seq, status, len(payload)]) + payload).ljust(64, b"\x00")


class FakeTransport:
    def __init__(self, frames):
        self.q, self.sent = list(frames), []

    def write(self, d):
        self.sent.append(bytes(d))

    def read(self, ms):
        return self.q.pop(0) if self.q else b""


STATE_HEAD = bytes([1, 2, 2, 4, 1, 3, 1, 1,
                    0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0])
STATE_TAIL = bytes([0x23, 0x01, 0x56, 0x04, 0x89, 0x07, 0xBC, 0x0A, 0x45, 0x23, 0xE7, 0xFF, 6, 11, 0x73, 0x01])


class State(unittest.TestCase):
    def test_tail_decoded(self):
        s = kr.decode_state(STATE_HEAD + STATE_TAIL)
        self.assertEqual((s["dp_mv"], s["dm_mv"], s["cc1_mv"], s["cc2_mv"]), (0x123, 0x456, 0x789, 0xABC))
        self.assertEqual(s["aux_mv"], 0x2345)
        self.assertEqual(s["temp_c"], -2.5)
        self.assertEqual(s["fc"], "QC2.0 9V")
        self.assertTrue(s["adc_valid"])
        self.assertTrue(s["temp_valid"])
        self.assertFalse(s["temp_calibrated"])
        self.assertEqual(s["mcu_temp_c"], 37.1)
        self.assertTrue(s["mcu_valid"])   # flags = 11: bit3 set

    def test_old_firmware_without_tail(self):
        s = kr.decode_state(STATE_HEAD)
        self.assertEqual(s["vbus_mv"], 20000)
        self.assertNotIn("dp_mv", s)

    def test_lines_page_name(self):
        self.assertEqual(kr.PAGES[3], "lines")
        self.assertEqual(kr.PAGES[:6], ["home", "detail", "chargertest", "lines", "tools", "settings"])
        self.assertEqual(kr.PAGES[6:], ["trigger", "emarker", "pdtrigger", "cabler", "blackbox", "spoof", "usblink", "autorotate", "cards", "share", "connection", "diag"])

    def test_unknown_verdict(self):
        s = kr.decode_state(STATE_HEAD + STATE_TAIL[:-4] + bytes([99, 0, 0, 0]))
        self.assertEqual(s["fc"], "99")
        self.assertFalse(s["adc_valid"])


class AdcRaw(unittest.TestCase):
    PAYLOAD = (bytes([1]) + b"".join((0x100 + i).to_bytes(2, "little") for i in range(10))
               + b"".join((0x200 + i).to_bytes(2, "little") for i in range(7)))

    def test_decode(self):
        r = kr.decode_adc_raw(self.PAYLOAD)
        self.assertTrue(r["ok"])
        self.assertEqual(list(r["channels"]), [0, 1, 2, 3, 4, 5, 6, 7, 16, 17])
        self.assertEqual(r["channels"][0], 0x100)
        self.assertEqual(r["channels"][17], 0x109)
        self.assertEqual(list(r["scan"]), ["aux", "cc1", "cc2", "dp", "dm", "ntc", "vref"])
        self.assertEqual(r["scan"]["aux"], 0x200)
        self.assertEqual(r["scan"]["vref"], 0x206)

    def test_short_payload(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_adc_raw(self.PAYLOAD[:-1])

    def test_command_and_render(self):
        link = kr.RemoteLink(FakeTransport([rsp(kr.CMD_ADC_RAW, 1, 0, self.PAYLOAD)]))
        a = kr.build_parser().parse_args(["adc-raw"])
        r = kr.COMMANDS[a.command](link, a)
        self.assertEqual(link.transport.sent[0][:5], bytes([0xA5, 0x4B, 0x21, 1, 0]))
        text = kr.render("adc-raw", r)
        self.assertIn("ch17", text)
        self.assertIn("0x109", text.lower().replace("0X", "0x"))


def ripple_frames(seq, samples, period_ns=933, code=0):
    head = bytes([code]) + period_ns.to_bytes(4, "little") + len(samples).to_bytes(2, "little") + \
        bytes([min(samples) & 0xFF, min(samples) >> 8, max(samples) & 0xFF, max(samples) >> 8,
               (sum(samples) // len(samples)) & 0xFF, (sum(samples) // len(samples)) >> 8])
    frames = [rsp(kr.CMD_RIPPLE, seq, 0, head)]
    for off in range(0, len(samples), 28):
        chunk = samples[off:off + 28]
        frames.append(rsp(kr.CMD_RIPPLE_DATA, seq, 0,
                          off.to_bytes(2, "little") + b"".join(v.to_bytes(2, "little") for v in chunk)))
    return frames


class Ripple(unittest.TestCase):
    SAMPLES = [2048 + int(500 * ((i % 16) - 8) / 8) for i in range(512)]

    def test_request_and_samples(self):
        link = kr.RemoteLink(FakeTransport(ripple_frames(1, self.SAMPLES, code=3)))
        a = kr.build_parser().parse_args(["ripple", "--code", "3"])
        r = kr.COMMANDS[a.command](link, a)
        self.assertEqual(link.transport.sent[0][:6], bytes([0xA5, 0x4B, 0x22, 1, 1, 3]))
        self.assertEqual(r["samples"], self.SAMPLES)
        self.assertEqual((r["code"], r["period_ns"], r["count"]), (3, 933, 512))
        self.assertEqual((r["min"], r["max"]), (min(self.SAMPLES), max(self.SAMPLES)))

    def test_default_code_sends_no_payload(self):
        link = kr.RemoteLink(FakeTransport(ripple_frames(1, self.SAMPLES)))
        a = kr.build_parser().parse_args(["ripple"])
        kr.COMMANDS[a.command](link, a)
        self.assertEqual(link.transport.sent[0][:5], bytes([0xA5, 0x4B, 0x22, 1, 0]))

    def test_missing_frame_is_an_error(self):
        frames = ripple_frames(1, self.SAMPLES)
        del frames[5]
        link = kr.RemoteLink(FakeTransport(frames), timeout_s=0.05)
        a = kr.build_parser().parse_args(["ripple"])
        with self.assertRaises((kr.FrameError, kr.RemoteTimeout)):
            kr.COMMANDS[a.command](link, a)

    def test_bad_code_rejected_by_parser(self):
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["ripple", "--code", "8"])

    def test_png_plot(self):
        png = kr.ripple_png(self.SAMPLES, 4095)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        w, h = int.from_bytes(png[16:20], "big"), int.from_bytes(png[20:24], "big")
        self.assertEqual((w, h), (512, 256))

    def test_png_written_by_command(self):
        d = tempfile.mkdtemp()
        out = os.path.join(d, "r.png")
        link = kr.RemoteLink(FakeTransport(ripple_frames(1, self.SAMPLES)))
        a = kr.build_parser().parse_args(["ripple", "--png", out])
        r = kr.COMMANDS[a.command](link, a)
        self.assertEqual(r["png"], out)
        self.assertTrue(Path(out).read_bytes().startswith(b"\x89PNG"))
        self.assertIn("period", kr.render("ripple", r))


if __name__ == "__main__":
    unittest.main()
