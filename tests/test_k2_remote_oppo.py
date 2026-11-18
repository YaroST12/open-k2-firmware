# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Host tests for k2_remote pd oppo-probe / pd oppo-power (CMD_OPPO_PROBE 0x4D): the request bytes, the part decode, the
POWER / EXAPDO decoders, the caps diff, the attach-on-no-contract retry and the refusal text."""
import argparse
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import k2_remote as kr

BEFORE = [0x00019145, 0x0002D145, 0xC1A43241]
AFTER = [0x00019145, 0x0002D145, 0xC1A43264]


class Reply:
    def __init__(self, payload):
        self.payload = payload


def words(part, ws):
    return bytes([part, len(ws)]) + b"".join(w.to_bytes(4, "little") for w in ws)


class FakeLink:
    """A finished run after the given number of running headers; the first starts can be refused with one reason byte."""

    def __init__(self, polls=1, refuse=None, refusals=0, status=kr.STATUS_BAD_ARG):
        self.calls, self.polls, self.refuse, self.refusals, self.status = [], polls, refuse, refusals, status

    def head(self, running):
        out = [5, 1, 4, 4, 5, 4]   # accept ack reply reply accept reply
        return bytes([0, 1 if running else 0, 0 if running else 1, 0x0F, 0, 0, 0, 0x04] + out)

    def call(self, cmd, args=b""):
        args = bytes(args)
        self.calls.append((cmd, args))
        if cmd == kr.CMD_PD_ATTACH:
            return Reply(bytes(13))
        if cmd == kr.CMD_PD_TRIGGER_STATUS:
            return Reply(bytes([4, 1, 1, 0, 2]) + (9000).to_bytes(2, "little") + (3000).to_bytes(2, "little") + bytes(4))
        if args[0] == 1:
            if self.refusals > 0:
                self.refusals -= 1
                raise kr.RemoteError(self.status, cmd, bytes([self.refuse]))
            return Reply(self.head(True))
        if args[0] == 3:
            return Reply(bytes([0, 0, 2]) + (9000).to_bytes(2, "little") + (3000).to_bytes(2, "little"))
        part = args[1]
        if part == 0:
            self.polls -= 1
            return Reply(self.head(self.polls >= 0))
        return Reply(words(part, [[0x22D9A15F, 1, 2, 3, 4], [0x22D90001, 0x0BB8003C], [0x22D90002, 0x47000000],
                                  BEFORE, AFTER][part - 1]))


def ns(**kw):
    base = dict(command="pd", pd_cmd="oppo-probe", skip_swap=False, steps=None, auth_words=None, repeat=1, no_attach=False,
                wait=5.0)
    base.update(kw)
    return argparse.Namespace(**base)


class Clock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class OppoProbe(unittest.TestCase):
    def test_start_bytes_poll_and_parts(self):
        link, clk = FakeLink(polls=2), Clock()
        r = kr.cmd_pd_oppo_probe(link, ns(skip_swap=True, steps=0x06, auth_words=["11", "22", "33", "DEADBEEF"]),
                                 sleep=clk.sleep, clock=clk)
        self.assertEqual(link.calls[0], (kr.CMD_OPPO_PROBE, bytes([1, 1, 0x06, 0x11, 0, 0, 0, 0x22, 0, 0, 0, 0x33, 0, 0, 0,
                                                                   0xEF, 0xBE, 0xAD, 0xDE])))
        parts = [c[1] for c in link.calls[1:]]
        self.assertEqual(parts[-6:], [bytes([0, p]) for p in range(6)])
        run = r["runs"][0]
        self.assertEqual(run["challenge"], [0x11, 0x22, 0x33, 0xDEADBEEF])
        self.assertEqual(run["outcomes"], {"swap": "accept", "auth": "ack", "power": "reply", "exapdo": "reply",
                                           "swap_back": "accept", "caps": "reply"})
        self.assertEqual(run["end"], "done")
        self.assertEqual(run["auth"], [0x22D9A15F, 1, 2, 3, 4])
        self.assertEqual(run["power_decoded"], {"imax_ma": 3000, "vmax_mv": 30000})
        self.assertEqual(run["exapdo_decoded"], {"num": 1, "bit": 7, "extra_pps": True})
        self.assertEqual(run["caps_changed"], [{"slot": 3, "before": 0xC1A43241, "after": 0xC1A43264}])
        text = kr.render("pd", r)
        self.assertIn("imax 3 A, vmax 30 V", text)
        self.assertIn("extra PPS current yes", text)
        self.assertIn("slot 3: 0xC1A43241", text)

    def test_random_challenge_and_repeat(self):
        link, clk = FakeLink(polls=0), Clock()
        draws = iter([bytes(range(16)), bytes(range(16, 32))])
        r = kr.cmd_pd_oppo_probe(link, ns(repeat=2), sleep=clk.sleep, clock=clk, rand=lambda n: next(draws))
        self.assertEqual(len(r["runs"]), 2)
        self.assertEqual(r["runs"][0]["challenge"][0], 0x03020100)
        self.assertEqual(r["runs"][1]["challenge"][0], 0x13121110)
        starts = [c for c in link.calls if c[1][0] == 1]
        self.assertEqual(len(starts), 2)
        self.assertEqual(starts[0][1][2], 0x0F)   # all steps by default

    def test_no_contract_attaches_at_5v_then_retries(self):
        link, clk = FakeLink(polls=0, refuse=kr.OPPO_REFUSAL_NO_CONTRACT, refusals=2), Clock()
        r = kr.cmd_pd_oppo_probe(link, ns(), sleep=clk.sleep, clock=clk, rand=lambda n: bytes(n))
        attaches = [c for c in link.calls if c[0] == kr.CMD_PD_ATTACH]
        self.assertEqual(attaches, [(kr.CMD_PD_ATTACH, (5000).to_bytes(2, "little"))])
        self.assertTrue(r["attached"])

    def test_no_attach_and_other_refusals_raise_with_the_reason(self):
        link, clk = FakeLink(refuse=kr.OPPO_REFUSAL_NO_CONTRACT, refusals=1), Clock()
        with self.assertRaises(kr.RemoteError) as e:
            kr.cmd_pd_oppo_probe(link, ns(no_attach=True), sleep=clk.sleep, clock=clk, rand=lambda n: bytes(n))
        self.assertIn("no contract", str(e.exception))
        self.assertFalse(any(c[0] == kr.CMD_PD_ATTACH for c in link.calls))
        link = FakeLink(refuse=1, refusals=1, status=kr.STATUS_BUSY)
        with self.assertRaises(kr.RemoteError) as e:
            kr.cmd_pd_oppo_probe(link, ns(), sleep=clk.sleep, clock=clk, rand=lambda n: bytes(n))
        self.assertIn("(busy)", str(e.exception))
        self.assertFalse(any(c[0] == kr.CMD_PD_ATTACH for c in link.calls))

    def test_attach_gives_up_when_the_contract_never_forms(self):
        link, clk = FakeLink(refuse=kr.OPPO_REFUSAL_NO_CONTRACT, refusals=1000), Clock()
        with self.assertRaises(kr.RemoteError):
            kr.cmd_pd_oppo_probe(link, ns(), sleep=clk.sleep, clock=clk, rand=lambda n: bytes(n))
        self.assertEqual(sum(1 for c in link.calls if c[0] == kr.CMD_PD_ATTACH), 1)

    def test_decoders(self):
        self.assertEqual(kr.oppo_decode_power(0x0BB80064), {"imax_ma": 5000, "vmax_mv": 30000})
        self.assertEqual(kr.oppo_decode_exapdo(0x87000000), {"num": 2, "bit": 7, "extra_pps": False})
        self.assertEqual(kr.oppo_decode_exapdo(0x46000000), {"num": 1, "bit": 6, "extra_pps": False})

    def test_steps_argument(self):
        self.assertEqual(kr._oppo_steps("power,exapdo"), 6)
        self.assertEqual(kr._oppo_steps("0x9"), 9)
        for bad in ("0", "16", "swap"):
            with self.assertRaises(argparse.ArgumentTypeError):
                kr._oppo_steps(bad)


class OppoPower(unittest.TestCase):
    def test_request_bytes_and_result(self):
        link = FakeLink()
        r = kr.cmd_pd_oppo_power(link, argparse.Namespace(mv=9000, ma=3000, confirm=True))
        self.assertEqual(link.calls[0], (kr.CMD_OPPO_PROBE, bytes([3, 1, 0x28, 0x23, 0xB8, 0x0B])))
        self.assertEqual((r["pos"], r["mv"], r["ma"], r["err"]), (2, 9000, 3000, "ok"))
        self.assertIn("oppo-power pdo=2 9 V 3 A", kr.render("pd", r))

    def test_refusal_text(self):
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_OPPO_PROBE, bytes([0, 7, 0, 0, 0, 0, 0]))
        self.assertIn("nothing advertised serves that point", str(e))
        e = kr.RemoteError(kr.STATUS_BAD_ARG, kr.CMD_OPPO_PROBE, bytes([5]))
        self.assertIn("not at 5 V", str(e))


if __name__ == "__main__":
    unittest.main()
