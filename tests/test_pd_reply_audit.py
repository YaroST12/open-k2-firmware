# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""tools/pd_reply_audit.py on a synthetic Black Box trace (tests/fixtures/pd_reply_audit/trace.jsonl): which replies the K2
owed, which it gave and how late; frames its chip never acknowledged and inline conversations are not the K2's; the k2_stream
`replies` command runs the same audit on a session folder."""
import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import k2_stream as ks  # noqa: E402
import pd_reply_audit as pra  # noqa: E402

FIXTURE = ROOT / "tests" / "fixtures" / "pd_reply_audit"


def findings():
    name, recs = pra.load(FIXTURE)
    return {(f["msg"], f["t_ms"]): f for f in pra.audit_records(name, recs, 10.0)}


class ReplyAudit(unittest.TestCase):
    def test_owed_replies_and_their_latency(self):
        f = findings()
        caps = f[("Source_Capabilities", 1000.0)]
        self.assertEqual((caps["reply"], caps["latency_ms"], caps["flag"]), ("Request", 2.8, None))   # to TX_START
        self.assertEqual(f[("Get_Status", 1800.0)]["reply"], "Not_Supported")
        nak = f[("VDM Discover_Modes REQ 0xFF01", 5300.0)]
        self.assertEqual((nak["reply"], nak["flag"]), ("VDM Discover_Modes NAK 0xFF01", None))
        epr = f[("EPR_Source_Capabilities", 5100.0)]
        self.assertEqual((epr["owed"], epr["reply"], epr["flag"]), ("EPR_Request", "EPR_Request", None))

    def test_late_and_missing_replies_are_flagged(self):
        f = findings()
        late = f[("Get_Sink_Cap", 1300.0)]
        self.assertEqual((late["flag"], late["latency_ms"]), ("late", 14.9))
        missing = f[("VDM Discover_Modes REQ 0x05AC", 1600.0)]   # its GoodCRC was read before the RX record
        self.assertEqual((missing["flag"], missing["reply"]), ("missing", None))

    def test_what_the_k2_does_not_owe_is_left_out(self):
        keys = {k[0] for k in findings()}
        self.assertNotIn("VDM Attention REQ 0xFF01", keys)            # never answered
        self.assertNotIn("Extended_Control EPR_KeepAlive_Ack", keys)  # a reply
        self.assertNotIn(("Get_Sink_Cap", 2100.0), findings())       # never acknowledged: the source heard nothing
        self.assertNotIn(("Get_Sink_Cap", 9005.0), findings())       # inline: a far sink spoke 5 ms before
        self.assertEqual(len(findings()), 6)

    def test_the_table_rules(self):
        class M:   # a message stand-in for pra.owed
            def __init__(self, name, type_, ndo=0, ext=False, pd3=True, vdm=None, eh=None, ecdb=None):
                self.name, self.type, self.ndo, self.ext, self.pd3, self.vdm, self.eh, self.ecdb = name, type_, ndo, ext, pd3, vdm, eh, ecdb

            def more_chunks(self):
                return False
        self.assertIsNone(pra.owed(M("Get_Status", 18, pd3=False), False))           # not a PD 2.0 type
        self.assertEqual(pra.owed(M("Get_Source_Cap", 7, pd3=False), False)[0], "Not_Supported/Reject")
        self.assertEqual(pra.owed(M("Ctl_27", 27), False)[0], "Not_Supported")       # reserved, PD 3
        self.assertIsNone(pra.owed(M("Vendor_Defined", 15, 1, vdm={"svid": 0x2717, "structured": False, "ctype": 0, "cmd": 1}), False))
        self.assertIsNone(pra.owed(M("EPR_Source_Capabilities", 17, 7, ext=True, eh={"request": False}), False))   # SPR: informational

    def test_the_command_line_exit_status_and_ignore(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(pra.main([str(FIXTURE)]), 1)
        self.assertIn("2 flagged", out.getvalue())
        with redirect_stdout(io.StringIO()):
            self.assertEqual(pra.main([str(FIXTURE), "--ignore", "Discover_Modes", "--limit-ms", "15"]), 0)
        out = io.StringIO()
        with redirect_stdout(out):
            pra.main([str(FIXTURE / "trace.jsonl"), "--json"])
        self.assertEqual(len(json.loads(out.getvalue())["flagged"]), 2)

    def test_k2_stream_replies_runs_the_audit_on_a_session_folder(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(ks.main(["replies", str(FIXTURE)]), 1)
        self.assertIn("MISSING VDM Discover_Modes REQ 0x05AC", out.getvalue())


if __name__ == "__main__":
    unittest.main()
