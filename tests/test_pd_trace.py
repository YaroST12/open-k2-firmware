# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
import pd_trace as pt  # noqa: E402


def rec(rtype, ts, payload):
    return bytes([6 + len(payload), rtype]) + struct.pack("<I", ts) + bytes(payload)


def le32(*w):
    return b"".join(struct.pack("<I", x) for x in w)


CAPS_HDR = 0x2181      # type 1, 2 objects, id 0, PD3, power role source
REQ_HDR = 0x1282       # type 2, 1 object, id 1, PD3, sink
PDO_5V3A = 0x0001912C  # 100 * 50 mV, 300 * 10 mA
PDO_PPS = 0xC0DC213C   # 110*100 mV max, 33*100 mV min, 60*50 mA
RDO_PPS = 0x2001F414   # pos 2, 250*20 mV = 5 V, 20*50 mA = 1 A
CRC = bytes(4)


def rx(hdr, objs=b""):
    return bytes([0xE0, hdr & 0xFF, hdr >> 8]) + objs + CRC


class ParseTests(unittest.TestCase):
    def test_parse_and_leftover(self):
        a = rec(pt.T_VBUS, 0x01020304, struct.pack("<Hh", 5000, -20))
        b = rec(pt.T_LINK, 7, [3])
        recs, rest = pt.parse_records(a + b + b[:3])
        self.assertEqual(recs, [(pt.T_VBUS, 0x01020304, struct.pack("<Hh", 5000, -20)), (pt.T_LINK, 7, b"\x03")])
        self.assertEqual(rest, b[:3])

    def test_corrupt_length(self):
        with self.assertRaises(ValueError):
            pt.parse_records(bytes([2, 1, 0, 0]))

    def test_timestamp_unwrap(self):
        tl = pt.Timeline()
        e1 = tl.event(pt.T_LINK, 0xFFFFFF00, b"\x01")
        e2 = tl.event(pt.T_LINK, 0x100, b"\x01")
        self.assertEqual(e2["t_us"] - e1["t_us"], 0x200)
        self.assertEqual(e2["dt_ms"], 0.512)


class DecodeTests(unittest.TestCase):
    def test_pdos(self):
        self.assertEqual(pt.decode_pdo(PDO_5V3A)[1], "fixed 5 V 3 A")
        d, t = pt.decode_pdo(PDO_PPS)
        self.assertEqual((d["min_mv"], d["max_mv"], d["max_ma"]), (3300, 11000, 3000))
        self.assertEqual(t, "PPS 3.3-11 V 3 A")

    def test_header_names(self):
        self.assertEqual(pt.message_name(CAPS_HDR), "Source_Capabilities")
        self.assertEqual(pt.message_name(REQ_HDR), "Request")
        self.assertEqual(pt.message_name(0x0041), "GoodCRC")
        self.assertEqual(pt.message_name(0x0006), "PS_RDY")

    def test_rdo_resolved_by_caps(self):
        tl = pt.Timeline()
        tl.event(pt.T_RX, 0, rx(CAPS_HDR, le32(PDO_5V3A, PDO_PPS)))
        ev = tl.event(pt.T_TX, 1000, bytes([REQ_HDR & 0xFF, REQ_HDR >> 8]) + le32(RDO_PPS))
        self.assertIn("PDO 2 PPS 5 V 1 A", ev["detail"][0])
        self.assertEqual(ev["msg"]["rdo"]["pos"], 2)

    def test_epr_mode_data_byte(self):
        hdr = 0x108A    # type 10 EPR_Mode, 1 object, PD3
        tl = pt.Timeline()
        ev = tl.event(pt.T_RX, 0, rx(hdr, le32(0x04030000)))
        self.assertEqual(ev["msg"]["epr_action"], "Enter_Failed")
        self.assertEqual(ev["msg"]["epr_data"], 3)
        self.assertIn("EPR_Mode Enter_Failed (EPR Mode Capable bit not set in the RDO)", ev["detail"][0])
        for code, name in ((0, "unknown cause"), (1, "cable not EPR capable"), (2, "source failed to become VCONN source"),
                           (4, "source unable to enter EPR Mode"), (5, "EPR Mode Capable bit not set in the 5 V PDO"),
                           (9, "invalid reason 9")):
            ev = tl.event(pt.T_RX, 10, rx(hdr, le32(0x04000000 | code << 16)))
            self.assertIn(f"Enter_Failed ({name})", ev["detail"][0])
        ev = tl.event(pt.T_TX, 20, bytes([hdr & 0xFF, hdr >> 8]) + le32(0x01 << 24 | 140 << 16))
        self.assertIn("EPR_Mode Enter (EPR Sink Operational PDP 140 W)", ev["detail"][0])
        ev = tl.event(pt.T_RX, 30, rx(hdr, le32(0x02000000)))
        self.assertEqual(ev["detail"][0], "EPR_Mode Enter_Acknowledged")

    def test_request_after_caps_timing(self):
        tl = pt.Timeline()
        tl.event(pt.T_RX, 1000, rx(CAPS_HDR, le32(PDO_5V3A, PDO_PPS)))
        ev = tl.event(pt.T_TX, 13300, bytes([REQ_HDR & 0xFF, REQ_HDR >> 8]) + le32(RDO_PPS))
        self.assertIn("12.3 ms after RX Source_Capabilities", ev["text"])
        self.assertEqual(ev["dt_ms"], 12.3)

    def test_caps_period(self):
        tl = pt.Timeline()
        tl.event(pt.T_RX, 0, rx(CAPS_HDR, le32(PDO_5V3A)))
        ev = tl.event(pt.T_RX, 150000, rx(CAPS_HDR, le32(PDO_5V3A)))
        self.assertIn("period 150.0 ms", ev["text"])

    def test_vdm_discover_identity(self):
        h = 0xFF00 << 16 | 1 << 15 | 1   # SVID 0xFF00, structured, REQ, cmd 1
        d, t = pt.decode_vdm([h])
        self.assertEqual(t, "Discover_Identity REQ SVID 0xFF00")

    def test_tx_records_carry_their_sop(self):
        """TX records end with the SOP token (pdtrace.hpp tx()); the old byte-guess (any 1-DO
        0xFF00 VDM = SOP') mislabelled the sink's proactive Discover SVIDs REQ (trace5)."""
        sv_req = bytes([0x8F, 0x10]) + le32(0xFF008002)   # 1-DO Vendor_Defined, PD3, SVID 0xFF00
        tl = pt.Timeline()
        ev = tl.event(pt.T_TX, 1000, sv_req + bytes([0xE0]))
        self.assertEqual(ev["sop"], "SOP")
        self.assertIn("Discover_SVIDs REQ", ev["detail"][0])
        ev = tl.event(pt.T_TX, 2000, sv_req + bytes([0xC0]))
        self.assertEqual(ev["sop"], "SOP'")
        # dumps from older firmware have no token: keep the spoof-coloured fallback
        ev = tl.event(pt.T_TX, 3000, bytes([0x8F, 0x10]) + le32(0xFF008041))
        self.assertEqual(ev["sop"], "SOP'")
        # SOP'' token decodes too; a tokenless old-format dump stays heuristic (SOP' for 1-DO 0xFF00)
        ev = tl.event(pt.T_TX, 4000, sv_req)
        self.assertEqual(ev["sop"], "SOP'")
        ev = tl.event(pt.T_TX, 5000, sv_req + bytes([0xA0]))
        self.assertEqual(ev["sop"], "SOP''")

    def test_zero_pdo_is_padding(self):
        # USB PD R3.2 EPR_Source_Capabilities: unused SPR positions (up to 7) are all-zero words, EPR PDOs start at position 8
        d, t = pt.decode_pdo(0)
        self.assertEqual(d["kind"], "empty")
        self.assertIn("empty", t)

    def test_epr_source_capabilities_chunks_are_reassembled(self):
        # Baseus 140 W answer, 32 B in two chunks (26 B + 6 B): PDO 7 straddles the chunk boundary and is all zero, PDO 8 is 28 V 5 A
        words = [0x2601912C | (1 << 23), 0x0002D12C, 0x0003C12C, 0x0004B12C, 0x0005A12C, 0xC0DC213C, 0, 0x0008C1F4]
        data = le32(*words)
        self.assertEqual(len(data), 32)

        def chunk(n, pdu):
            eh = struct.pack("<H", 0x8000 | n << 11 | 32)
            payload = eh + pdu
            payload += bytes(-len(payload) % 4)
            return rx(0x8000 | (len(payload) // 4) << 12 | (2 << 6) | 0x0100 | 17, payload)
        tl = pt.Timeline()
        e0 = tl.event(pt.T_RX, 0, chunk(0, data[:26]))
        self.assertEqual(e0["msg"]["name"], "EPR_Source_Capabilities")
        self.assertTrue(any(x.startswith("PDO6 ") for x in e0["detail"]))
        self.assertFalse(any(x.startswith("PDO7 ") for x in e0["detail"]))   # only half of it is here
        e1 = tl.event(pt.T_RX, 10000, chunk(1, data[26:]))
        lines = [x for x in e1["detail"] if x.startswith("PDO")]
        self.assertEqual(len(lines), 2)
        self.assertTrue(lines[0].startswith("PDO7 ") and "empty" in lines[0], lines[0])
        self.assertEqual(lines[1], "PDO8 fixed 28 V 5 A")
        self.assertEqual([p["kind"] for p in tl.md.caps][6:], ["empty", "fixed"])

    def test_ext_header(self):
        e = pt.decode_ext_header(0x8000 | 24)
        self.assertEqual((e["chunked"], e["data_size"], e["chunk"]), (True, 24, 0))

    def test_source_capabilities_extended_is_decoded(self):
        # SCEDB (25 bytes): VID 0x2CC3 PID 0x0865 XID 0x11223344 FW 3 HW 2, regulation/holdup/compliance/touch current,
        # peak currents, touch temp, source inputs 0x01, batteries 0, Source PDP 100 W, EPR PDP 0
        scedb = (struct.pack("<HHIBB", 0x2CC3, 0x0865, 0x11223344, 3, 2) + bytes([0x07, 0x00, 0x01, 0x01])
                 + struct.pack("<HHH", 0x1234, 0x0000, 0x0000) + bytes([0x02, 0x01, 0x00, 100, 0]))
        self.assertEqual(len(scedb), 25)
        eh = struct.pack("<H", 0x8000 | 25)
        payload = eh + scedb + b"\x00"       # padded to 7 objects
        hdr = 0x8000 | (7 << 12) | 0x0100 | (2 << 6) | 1   # extended, 7 objects, source, PD3, type 1
        tl = pt.Timeline()
        ev = tl.event(pt.T_RX, 0, rx(hdr, payload))
        self.assertEqual(ev["msg"]["name"], "Source_Capabilities_Extended")
        self.assertEqual(ev["msg"]["ext_payload"], scedb.hex())
        e = ev["msg"]["scedb"]
        self.assertEqual((e["vid"], e["pid"], e["xid"], e["fw"], e["hw"]), (0x2CC3, 0x0865, 0x11223344, 3, 2))
        self.assertEqual((e["source_inputs"], e["batteries"], e["pdp_w"], e["epr_pdp_w"], e["touch_temp"]), (1, 0, 100, 0, 2))
        self.assertEqual(e["peak_current"], [0x1234, 0, 0])
        self.assertTrue(any("VID 2CC3 PID 0865" in x and "PDP 100 W" in x for x in ev["detail"]))

    def test_sink_capabilities_extended_is_decoded(self):
        skedb = bytes([0x16, 0x07, 0x60, 0x50]) + bytes(6) + bytes([1, 0, 0, 0, 0, 0, 0, 0x23, 1, 15, 33, 0, 0, 0])
        self.assertEqual(len(skedb), 24)
        payload = struct.pack("<H", 0x8000 | 24) + skedb + b"\x00\x00"
        hdr = 0x8000 | (7 << 12) | (2 << 6) | 15
        tl = pt.Timeline()
        ev = tl.event(pt.T_TX, 0, bytes([hdr & 0xFF, hdr >> 8]) + payload)
        self.assertEqual(ev["msg"]["name"], "Sink_Capabilities_Extended")
        s = ev["msg"]["skedb"]
        self.assertEqual((s["vid"], s["pid"], s["version"], s["sink_modes"]), (0x0716, 0x5060, 1, 0x23))
        self.assertEqual((s["spr_min_w"], s["spr_op_w"], s["spr_max_w"]), (1, 15, 33))
        self.assertTrue(any("AVS" in x and "PPS" in x for x in ev["detail"]))

    def test_registers(self):
        tl = pt.Timeline()
        ev = tl.event(pt.T_REG, 0, bytes([0x03, 0x25]))
        self.assertEqual(ev["reg"], "SWITCHES1")
        for bit in ("TXCC1", "AUTO_CRC", "SPECREV=PD2.0"):
            self.assertIn(bit, ev["text"])
        ev = tl.event(pt.T_REG, 0, bytes([0x80 | 0x0C, 0x02]))
        self.assertIn("PD_RESET", ev["text"])
        self.assertIn("WRITE FAILED", ev["text"])
        self.assertEqual(pt.reg_value_text("STATUS0", 0xA1).split()[0:3], ["COMP", "VBUSOK", "BC_LVL=1"])

    def test_tx_result_and_reset(self):
        tl = pt.Timeline()
        ev = tl.event(pt.T_TXRESULT, 0, bytes([1, 5, 0x10]))
        self.assertIn("RETRYFAIL", ev["text"])
        self.assertIn("I_RETRYFAIL", ev["text"])
        tl.event(pt.T_RESET, 0, b"\x00")
        ev = tl.event(pt.T_RESET, 500000, b"\x00")
        self.assertIn("500.0 ms since the previous hard reset", ev["text"])

    def test_misc_records(self):
        tl = pt.Timeline()
        self.assertIn("5.04 V 1500 mA", tl.event(pt.T_VBUS, 0, struct.pack("<Hh", 5040, 1500))["text"])
        self.assertIn("attach CC2", tl.event(pt.T_CC, 0, bytes([1, 2, 0x81]))["text"])
        self.assertIn("inline=yes auto_sink=no", tl.event(pt.T_LINK, 0, b"\x01")["text"])
        self.assertIn("2.00 ms", tl.event(pt.T_POLL, 0, struct.pack("<I", 2000))["text"])
        self.assertIn("malformed", tl.event(pt.T_VBUS, 0, b"\x01")["text"])

    def test_collapsed_source_capabilities_repeat(self):
        # type 16 follows the one Source_Capabilities frame the recorder kept: count, span in ms, stamped with the first repeat
        self.assertEqual(pt.T_RXREPEAT, 16)
        data = rec(pt.T_RX, 1_000_000, rx(CAPS_HDR, le32(PDO_5V3A))) + rec(pt.T_RXREPEAT, 1_002_200, struct.pack("<HH", 4, 195))
        evs, rest, _ = pt.decode_buffer(data)
        self.assertEqual(rest, b"")
        self.assertEqual([e["type"] for e in evs], ["RX", "RX_REPEAT"])
        self.assertEqual((evs[1]["count"], evs[1]["span_ms"]), (4, 195))
        self.assertIn("4 more identical Source_Capabilities over 195 ms", evs[1]["text"])
        self.assertEqual(evs[1]["dt_ms"], 2.2)
        # a short payload is reported, not raised
        self.assertIn("malformed", pt.Timeline().event(pt.T_RXREPEAT, 0, b"\x04")["text"])

    def test_start_request_marks(self):
        tl = pt.Timeline()
        sent = tl.event(pt.T_MARK, 0, bytes([2, 0, 0, 0, 0]))["text"]
        self.assertIn("start-request", sent)
        self.assertIn("PDO 1 Request queued -> ok", sent)
        # a = 0x100 | refusal bits: the start wanted a Request on these caps and did not make one
        why = tl.event(pt.T_MARK, 0, bytes([2, 0x0A, 0x01, 0, 0]))["text"]   # no CC attached (2) + already requested (8)
        self.assertIn("NOT queued", why)
        self.assertIn("no-cc", why)
        self.assertIn("already-requested", why)
        self.assertNotIn("inline", why)

    def test_reread_marks(self):
        tl = pt.Timeline()
        st = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 5, 15, 6))["text"]   # started: a = mask, b = PDOs held
        self.assertIn("reread", st)
        self.assertIn("started", st)
        self.assertIn("mask=0x0F", st)
        self.assertIn("6 PDOs", st)
        # done: a = 0x8000 | skipped << 8 | ok, b = avs << 15 | caps_rx << 8 | pdos_last
        done = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 5, 0x8000 | (3 << 8) | 8, 0x8000 | (1 << 8) | 7))["text"]
        self.assertIn("done", done)
        self.assertIn("ok=0x08", done)
        self.assertIn("skipped=0x03", done)
        self.assertIn("1 Source_Capabilities", done)
        self.assertIn("7 PDOs", done)
        self.assertIn("AVS APDO", done)
        none = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 5, 0x8000 | 15 << 8, 0))["text"]
        self.assertIn("no Source_Capabilities", none)
        self.assertNotIn("AVS", none)

    def test_start_wait_mark(self):
        tl = pt.Timeline()
        up = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 4, 4980, 0x8000 | 1200))["text"]
        self.assertIn("start-wait", up)
        self.assertIn("VBUS up", up)
        self.assertIn("4980 mV", up)
        self.assertIn("1200 ms", up)
        out = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 4, 620, 5003))["text"]
        self.assertIn("timed out", out)
        self.assertIn("620 mV", out)
        self.assertIn("5003 ms", out)

    def test_hunt_refusal_mark(self):
        tl = pt.Timeline()
        # cause 9 refused-offer, answer 2 reject, no timeout; targeted APDO limit 6000 mA
        rej = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 8, 9 | 2 << 8, 6000))["text"]
        self.assertIn("hunt-refusal", rej)
        self.assertIn("cause=refused-offer", rej)
        self.assertIn("answer=reject", rej)
        self.assertNotIn("timeout", rej)
        self.assertIn("6000 mA", rej)
        # cause 7 apdo-stale, the source never answered
        tmo = tl.event(pt.T_MARK, 0, struct.pack("<BHH", 8, 7 | 0x1000, 5000))["text"]
        self.assertIn("cause=apdo-stale", tmo)
        self.assertIn("answer=none (timeout)", tmo)


class OutputTests(unittest.TestCase):
    def test_jsonl_and_text(self):
        buf = rec(pt.T_RX, 1000, rx(CAPS_HDR, le32(PDO_5V3A, PDO_PPS))) + rec(pt.T_REG, 2000, bytes([0x0C, 1]))
        evs, rest, _ = pt.decode_buffer(buf)
        self.assertEqual(rest, b"")
        with tempfile.TemporaryDirectory() as d:
            lines = []
            s = pt.Sink(str(Path(d) / "o.jsonl"), out=lines.append)
            for e in evs:
                s.event(e)
            s.close()
            rows = [json.loads(x) for x in (Path(d) / "o.jsonl").read_text().splitlines()]
        self.assertEqual([r["type"] for r in rows], ["RX", "REG"])
        self.assertEqual(rows[0]["msg"]["name"], "Source_Capabilities")
        self.assertEqual(rows[1]["dt_ms"], 1.0)
        self.assertIn("Source_Capabilities", lines[0])
        self.assertIn("PDO2 PPS 3.3-11 V 3 A", lines[0])


class FakeLink:
    """Serves blackbox reads from a byte string, a few bytes per call, like the device."""

    def __init__(self, data):
        self.data = data


class StreamTests(unittest.TestCase):
    def test_stream_chunks_and_flags(self):
        import k2_remote
        a, b = rec(pt.T_LINK, 10, [1]), rec(pt.T_LINK, 20, [2])
        data = a + b
        calls = []
        orig = k2_remote.blackbox_call

        def fake(link, op, arg=None):
            calls.append((op, arg))
            if op == k2_remote.BB_STATUS:
                return 0, 0, len(data), b""
            cur = arg or 0
            chunk = data[cur:cur + 7]            # splits records across reads
            return (pt.FLAG_LOST if cur == 0 else 0), cur + len(chunk), len(data), chunk

        k2_remote.blackbox_call = fake
        try:
            out = []
            pt.run_stream(None, pt.Sink(out=out.append), follow=False, tail=False, interval=0)
        finally:
            k2_remote.blackbox_call = orig
        self.assertTrue(any("records lost" in x for x in out))
        self.assertEqual(sum("LINK" in x for x in out), 2)


class CaptureTests(unittest.TestCase):
    def test_capture_list_and_read(self):
        import k2_remote
        recs = rec(pt.T_RESET, 10, b"\x00") + rec(pt.T_LINK, 20, [1])
        list_payload = struct.pack("<I", 5000) + struct.pack("<BHI", 2, len(recs), 1500) + bytes(7) + struct.pack("<BHI", 4, 0, 9)

        class Resp:
            def __init__(self, payload):
                self.payload = payload

        class Link:
            def __init__(self):
                self.reads = []

            def call(self, cmd, payload):
                if payload[0] == k2_remote.BB_CAP_LIST:
                    return Resp(list_payload)
                slot, off = payload[1], struct.unpack("<H", payload[2:4])[0]
                self.reads.append((slot, off))
                n = 7 if off == 0 else len(recs) - off
                chunk = recs[off:off + n]
                return Resp(bytes([8]) + struct.pack("<II", off + n, len(recs)) + chunk)

        caps = k2_remote.blackbox_captures(Link())
        self.assertEqual(caps[0], {"slot": 0, "trigger": "hard_reset", "bytes": 14, "age_ms": 1500})
        self.assertEqual(caps[1], {"slot": 2, "trigger": "detach", "bytes": 0, "age_ms": 9})
        self.assertEqual([c["trigger"] for c in caps], ["hard_reset", "detach"])
        link = Link()
        data, filling = k2_remote.blackbox_capture_read(link, 0)
        self.assertEqual(data, recs)
        self.assertTrue(filling)
        self.assertEqual(link.reads, [(0, 0), (0, 7)])


if __name__ == "__main__":
    unittest.main()
