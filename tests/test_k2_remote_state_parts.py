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

    def test_every_enum_has_a_name(self):
        # The widest value the firmware sends for each enum (include/k2/ui/ui_types.hpp and friends).
        self.assertEqual(len(kr.SESSION_PROTO_KINDS), 15)
        self.assertEqual(len(kr.SESSION_BUSY), 9)
        self.assertEqual(len(kr.SESSION_TOPOLOGY), 4)
        self.assertEqual(len(kr.SESSION_WAIT_KIND), 4)
        self.assertEqual(len(kr.SESSION_PD_NONE_WHY), 3)
        self.assertEqual(kr.MENU_NAMES[10], "connection")     # Menu::Connection

    def test_truncated_payload_is_rejected(self):
        with self.assertRaises(kr.FrameError):
            kr.decode_session(SESSION[:30])


class StateJson(unittest.TestCase):
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
