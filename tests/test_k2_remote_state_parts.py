# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""GET_STATE parts (session, diag, strip labels) as `k2_remote --json state` shows them.

The byte vectors are the same literals as tests/test_remote_proto.cpp (test_state_session_part), so both
ends of the wire stay pinned to the layout in include/k2/core/remote_proto.hpp.
"""
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr

BASE = bytes([2, 2, 2, 4, 1, 2, 1, 1,
              0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0]) + bytes(29)

# Part 1, field by field (31 bytes): the same offsets the C++ test checks.
SESSION = bytes([0x02, 0x01, 0, 0,                # 258 mAh
                 0x04, 0x03, 0, 0,                # 772 s
                 1, 0x88, 0x13, 0x20, 0x4E, 0xB8, 0x0B,   # PD fixed, 5000 mA, 20000 mV, Type-C 3000 mA
                 4, 1, 3, 2, 1,                   # busy pd-hold, fc_release, direct receptacle, charger waiting, silent sink
                 0x54, 0x0B,                      # open CC 2900 mV
                 1, 10, 0xFF, 0xFF,               # depth 1, stack [connection]
                 0x05, 3, 7, 2, 0x14])            # sub 5 = Cable tab + popup, sel 3, count 7, window 10m, flags 0x14


# Part 2 (29 bytes): the same literal as test_state_diag_part in tests/test_remote_proto.cpp. The last three bytes are the
# HID-cable loop block (k2/core/loop_guard.hpp); a build before it answers the first 26.
DIAG = bytes([0x91, 0x04, 0x03, 0x02, 0x01, 5, 0, 0, 0, 0x0D, 0x0C, 0x0B, 0x0A, 0x2C, 0x01, 0, 0, 2, 0, 0, 0,
              17, 0, 0, 0, 1, 0x09, 0x02, 0x01])


# Part 3 (57 bytes): the same literal as test_state_keys_part in tests/test_remote_proto.cpp.
KEYS = bytes([0x12,
              66, 97, 99, 107, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,
              45, 0, 0, 0, 0, 0, 0,       80, 114, 101, 118, 0, 0, 0,
              82, 101, 115, 99, 97, 110, 0, 67, 104, 97, 114, 103, 101, 114,
              43, 0, 0, 0, 0, 0, 0,       78, 101, 120, 116, 0, 0, 0])


def frame_ok(payload):
    return kr.Response(kr.CMD_GET_STATE, 1, 0, payload)


class FakeLink:
    """GET_STATE parts as a firmware answers them; `parts` maps part byte -> payload or a status to refuse with."""

    def __init__(self, parts):
        self.parts, self.sent = parts, []

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, bytes(payload)))
        if cmd == kr.CMD_GET_STATE:
            key = payload[0] if payload else 0
            v = self.parts.get(key, kr.STATUS_BAD_ARG)
            if isinstance(v, int):
                raise kr.RemoteError(v, cmd)
            return frame_ok(v)
        raise kr.RemoteError(kr.STATUS_UNKNOWN_CMD, cmd)      # no Apple read on this fake


def state_json(parts):
    a = kr.build_parser().parse_args(["--json", "state"])
    link = FakeLink(parts)
    return kr.COMMANDS["state"](link, a), link


class SessionDecode(unittest.TestCase):
    def test_fields(self):
        s = kr.decode_session(SESSION)
        self.assertEqual((s["mah"], s["elapsed_s"]), (258, 772))
        self.assertEqual(s["proto"], {"kind": "pd-fixed", "limit_ma": 5000, "max_mv": 20000})
        self.assertEqual(s["typec_ma"], 3000)
        self.assertEqual((s["busy"], s["fc_release"]), ("pd-hold", True))
        self.assertEqual((s["topology"], s["wait_kind"], s["pd_none_why"]),
                         ("direct-receptacle", "charger-waiting", "sink-silent-source-offered"))
        self.assertEqual(s["open_cc_mv"], 2900)
        self.assertEqual(s["ui"], {"stack": ["connection"], "tab": "cable", "popup": True, "details": False,
                                   "sel": 3, "count": 7})
        self.assertEqual(s["home_window"], "10m")
        self.assertEqual({k for k, v in s["flags"].items() if v}, {"em_scanning", "em_quiet"})

    def test_sentinels_become_null(self):
        p = bytearray(SESSION)
        p[20:22] = b"\xFF\xFF"                  # open CC unknown
        p[27] = 0xFF                            # no list cursor
        s = kr.decode_session(bytes(p))
        self.assertIsNone(s["open_cc_mv"])
        self.assertIsNone(s["ui"]["sel"])

    def test_tab_is_only_for_connection(self):
        p = bytearray(SESSION)
        p[22:26] = bytes([2, 0, 1, 0xFF])       # stack [trigger, emarker]
        s = kr.decode_session(bytes(p))
        self.assertEqual(s["ui"]["stack"], ["trigger", "emarker"])
        self.assertIsNone(s["ui"]["tab"])
        p[22:26] = bytes([0, 0xFF, 0xFF, 0xFF])  # base page: empty stack
        self.assertEqual(kr.decode_session(bytes(p))["ui"]["stack"], [])

    def test_fc_label_names_a_live_vooc_session(self):
        # The firmware sends fastcharge::label(): Protocol::Svooc (18, fastcharge_classify.hpp) while a proven VOOC session
        # is live, where the passive verdict alone says USB DATA (22) or PD (21). The remote prints what Home shows.
        for raw, name in ((18, "SVOOC"), (22, "USB DATA"), (21, "PD")):
            p = bytearray(BASE)
            p[36] = raw
            self.assertEqual(kr.decode_state(bytes(p))["fc"], name)
        self.assertEqual(kr.FC_NAMES[18], "SVOOC")
        self.assertEqual(kr.SESSION_PROTO_KINDS[10], "svooc")      # the session proto block says it too (Home's source)

    def test_every_enum_has_a_name(self):
        # The widest value the firmware sends for each enum (include/k2/ui/ui_types.hpp and friends).
        self.assertEqual(len(kr.SESSION_PROTO_KINDS), 15)
        self.assertEqual(len(kr.SESSION_BUSY), 11)   # BusyWhy::PdStress = 10 is the last (TwinRun = 9 stays)
        self.assertEqual(kr.SESSION_BUSY[9], "twin-run")
        self.assertEqual(len(kr.SESSION_TOPOLOGY), 4)
        self.assertEqual(len(kr.SESSION_WAIT_KIND), 4)
        self.assertEqual(len(kr.SESSION_PD_NONE_WHY), 3)
        self.assertEqual(kr.MENU_NAMES[10], "connection")     # Menu::Connection

    def test_truncated_payload_is_rejected(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_session(SESSION[:30])


class DiagDecode(unittest.TestCase):
    def test_fields(self):
        d = kr.decode_diag(DIAG)
        self.assertEqual(d, {"fusb_id": 0x91, "pd_rx": 0x01020304, "usb_resets": 5, "usb_isrs": 0x0A0B0C0D,
                             "log_sent": 300, "log_dropped": 2, "poll_gap_max_ms": 17, "usb_configured": True,
                             "loop": {"locked": True, "reason": "certain", "pending": False, "host": True, "events": 0x0102}})

    def test_loop_block(self):
        # The HID-cable loop 5 V lock (k2/core/loop_guard.hpp): bits 1:0 the reason, bit 2 the suspicion window, bit 3 a USB host.
        loop = lambda b: kr.decode_diag(DIAG[:26] + bytes([b, 3, 0]))["loop"]
        self.assertEqual(loop(0x00), {"locked": False, "reason": "none", "pending": False, "host": False, "events": 3})
        self.assertEqual(loop(0x04), {"locked": False, "reason": "none", "pending": True, "host": False, "events": 3})
        self.assertEqual(loop(0x02), {"locked": True, "reason": "suspected", "pending": False, "host": False, "events": 3})
        self.assertEqual(loop(0x03)["reason"], "reason 3")   # not defined: named, not guessed

    def test_older_firmware_has_no_loop_block(self):
        d = kr.decode_diag(DIAG[:26])
        self.assertNotIn("loop", d)
        self.assertTrue(d["usb_configured"])

    def test_truncated_payload_is_rejected(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_diag(DIAG[:25])
        with self.assertRaises(kr.FrameError):
            kr.decode_diag(DIAG[:28])   # a cut loop block


class KeysDecode(unittest.TestCase):
    def test_fields(self):
        k = kr.decode_keys(KEYS)
        self.assertEqual(k, {"back": {"short": "Back", "long": "", "dim": False},
                             "minus": {"short": "-", "long": "Prev", "dim": True},
                             "ok": {"short": "Rescan", "long": "Charger", "dim": False},
                             "plus": {"short": "+", "long": "Next", "dim": False},
                             "hidden": True})

    def test_a_full_width_word_has_no_terminator(self):
        p = bytearray(KEYS)
        p[1:8] = b"Charger"                                    # all 7 bytes used, no NUL
        self.assertEqual(kr.decode_keys(bytes(p))["back"]["short"], "Charger")

    def test_truncated_payload_is_rejected(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_keys(KEYS[:56])


class StateJson(unittest.TestCase):
    def test_json_state_carries_the_keys_block(self):
        out, link = state_json({0: BASE, 1: SESSION, 2: DIAG, 3: KEYS})
        self.assertEqual(out["keys"]["ok"]["long"], "Charger")
        self.assertEqual(out["diag"]["fusb_id"], 0x91)        # the earlier parts are still merged in
        self.assertIn((kr.CMD_GET_STATE, b"\x03"), link.sent)

    def test_firmware_without_part_3_has_no_keys(self):
        out, _ = state_json({0: BASE, 1: SESSION, 2: DIAG})
        self.assertNotIn("keys", out)
        self.assertIn("diag", out)

    def test_old_firmware_refusing_part_3_by_length(self):
        out, _ = state_json({0: BASE, 1: SESSION, 2: DIAG, 3: kr.STATUS_BAD_LENGTH})
        self.assertNotIn("keys", out)

    def test_json_state_carries_the_diag_block(self):
        out, link = state_json({0: BASE, 1: SESSION, 2: DIAG})
        self.assertEqual(out["diag"]["fusb_id"], 0x91)
        self.assertEqual(out["diag"]["log_dropped"], 2)
        self.assertEqual(out["mah"], 258)                    # the session block is still merged in
        self.assertIn((kr.CMD_GET_STATE, b"\x02"), link.sent)

    def test_session_only_firmware_has_no_diag(self):
        out, _ = state_json({0: BASE, 1: SESSION})
        self.assertNotIn("diag", out)
        self.assertEqual(out["mah"], 258)

    def test_json_state_carries_the_session(self):
        out, link = state_json({0: BASE, 1: SESSION})
        self.assertEqual(out["page"], 2)                     # the base block is still there
        self.assertEqual(out["mah"], 258)
        self.assertEqual(out["proto"]["kind"], "pd-fixed")
        self.assertIn((kr.CMD_GET_STATE, b"\x01"), link.sent)

    def test_old_firmware_leaves_the_parts_out(self):
        for status in (kr.STATUS_BAD_LENGTH, kr.STATUS_BAD_ARG):
            out, _ = state_json({0: BASE, 1: status})
            self.assertEqual(out["page"], 2)
            self.assertNotIn("mah", out)
            self.assertNotIn("ui", out)

    def test_other_errors_are_not_swallowed(self):
        with self.assertRaises(kr.RemoteError):
            state_json({0: BASE, 1: kr.STATUS_BUSY})

    def test_plain_state_does_not_ask_for_parts(self):
        a = kr.build_parser().parse_args(["state"])
        link = FakeLink({0: BASE})
        out = kr.COMMANDS["state"](link, a)
        self.assertEqual(link.sent, [(kr.CMD_GET_STATE, b"")])
        self.assertNotIn("mah", out)


if __name__ == "__main__":
    unittest.main()
