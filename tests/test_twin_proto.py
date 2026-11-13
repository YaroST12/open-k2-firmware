# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Device twin on the host: the record decoder, the Cmd::Twin response decoders, the texts that mirror
include/k2/twin/present.hpp, and the `k2_remote.py twin` command over a fake link. The two record vectors are what the
firmware's own encoder (include/k2/twin/library.hpp) writes."""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import k2_remote as kr  # noqa: E402
import twin_proto as tp  # noqa: E402

# Pixel 9: vid 18D1 pid 4EE1 id_hi 1A0 pd_rev 2, sink PDOs 5 V 3 A, 9 V 3 A, PPS 3.3-11 V; number 3, no Request.
PIXEL = bytes.fromhex("4b3254010300d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc8e465")
# The same with the sniffed Request pair (rdo 1004B12C, source PDO 0004B12C); number 5.
PIXEL_REQ = bytes.fromhex("4b3254010501d118e14ea0010203506978656c203900000000000000000000000000000000002c9101002cd102002c21dcc8"
                          "2cb104102cb10400a8b5")


def le16(v):
    return v.to_bytes(2, "little")


def le32(v):
    return v.to_bytes(4, "little")


def predict_payload(seq=1, kind=2, limiter=0, flags=0, pos=6, mv=11000, ma=3000, mw=33000, want=33000, claim=65, cable_ma=5000,
                    cable_mv=20000):
    return (bytes([0, seq, kind, limiter, flags, pos]) + le16(mv) + le16(ma) + le32(mw) + le32(want) + le16(claim) + le16(cable_ma)
            + le16(cable_mv))


def iq_status(running=0, pending=0, done=0, phase=0, step=6, why=0, match=0, rev=2, asked=0, ok=0, refused=0, silent=0, sent=0,
              acks=0, resets=0, seen=0, skipped=0, vid=0, pid=0, n_svids=0, n_modes=0, n_pdos=0, ext=0, revision=0,
              first=(0, 0, 0), second=(0, 0, 0), how=0, err=0, seq=0, existed=0, unchanged=0, ms=0, late=0, flags=0,
              queries=0, replies=0, n_epr=0):
    """The INLINE STATUS response, byte for byte as twin::wire::encode_iq_status lays it out (tests/test_twin_wire.cpp)."""
    p = (bytes([0, 0, running, pending, done, phase, step, why, match, rev, asked, ok, refused, silent, sent, acks, resets, seen,
                skipped]) + le16(vid) + le16(pid) + bytes([n_svids, n_modes, n_pdos, ext]) + le32(revision)
         + bytes(first[:2]) + le32(first[2]) + bytes(second[:2]) + le32(second[2])
         + bytes([how, err, seq, existed, unchanged]) + le16(ms) + bytes([late, flags, queries, replies, n_epr]))
    assert len(p) == tp.IQ_STATUS_BYTES
    return p


class Record(unittest.TestCase):
    def test_crc16_is_ccitt_false(self):
        self.assertEqual(tp.crc16(b"123456789"), 0x29B1)

    def test_decode_plain_record(self):
        r = tp.decode_record(PIXEL)
        self.assertEqual((r["seq"], r["version"], r["name"]), (3, 1, "Pixel 9"))
        self.assertEqual((r["vid"], r["pid"], r["id_hi"], r["pd_rev"]), (0x18D1, 0x4EE1, 0x1A0, 2))
        self.assertEqual(r["pdos"], [0x0001912C, 0x0002D12C, 0xC8DC212C])
        self.assertIsNone(r["request"])
        self.assertEqual(r["bytes"], 52)
        self.assertEqual(r["raw"], PIXEL.hex())

    def test_decode_record_with_request(self):
        r = tp.decode_record(PIXEL_REQ)
        self.assertEqual(r["seq"], 5)
        self.assertEqual(r["request"], {"rdo": 0x1004B12C, "pdo": 0x0004B12C})
        self.assertEqual(r["bytes"], 60)

    def test_bad_crc_magic_version_and_length(self):
        bad = bytearray(PIXEL)
        bad[20] ^= 1
        with self.assertRaisesRegex(tp.BadRecord, "crc"):
            tp.decode_record(bytes(bad))
        with self.assertRaisesRegex(tp.BadRecord, "magic"):
            tp.decode_record(b"X" + PIXEL[1:])
        with self.assertRaisesRegex(tp.BadRecord, "version"):
            tp.decode_record(PIXEL[:3] + bytes([9]) + PIXEL[4:])
        with self.assertRaisesRegex(tp.BadRecord, "short"):
            tp.decode_record(PIXEL[:20])
        with self.assertRaisesRegex(tp.BadRecord, "length"):
            tp.decode_record(PIXEL + b"\0")

    def test_version2_blocks(self):
        # A version 2 record: flags declared + observed. Declared block [n_pt=1, n_svids=1, n_modes=1] + 4 + 2 + 6 bytes,
        # observed block 18 bytes. Built here by hand, with the CRC the library would write.
        body = bytearray(PIXEL[:-2])
        body[3] = 2
        body[5] = 2 | 4
        decl = bytes([1, 1, 1]) + le32(0x11223344) + le16(0xFF01) + le16(0xFF01) + le32(0x55667788)
        obs = le16(0x18D1) + bytes([3, 1, 0x5, 2]) + le32(40000) + le32(0xAABBCCDD) + le32(0x01020304)
        self.assertEqual(len(decl), 15)
        self.assertEqual(len(obs), 18)
        raw = bytes(body) + decl + obs
        raw += le16(tp.crc16(raw))
        r = tp.decode_record(raw)
        self.assertEqual(r["version"], 2)
        self.assertEqual(r["declared"], {"pt": [0x11223344], "svids": [0xFF01], "modes": [{"svid": 0xFF01, "vdo": 0x55667788}]})
        self.assertEqual(r["observed"]["partner_vid"], 0x18D1)
        self.assertEqual((r["observed"]["family"], r["observed"]["tbt"], r["observed"]["flags"], r["observed"]["dp_pin"]), (3, 1, 5, 2))
        self.assertEqual(r["observed"]["mbps"], 40000)
        self.assertEqual(r["observed"]["enter_usb"], 0xAABBCCDD)
        self.assertEqual(r["observed"]["tbt_enter"], 0x01020304)
        self.assertEqual(r["bytes"], len(raw))

    def test_version3_revision_and_sink_ext(self):
        # What the firmware encoder writes for an anonymous device learned by an inline interrogation: two sink PDOs, the
        # sniffed Request, the Revision data object (PD 3.2, version 1.0) and four Sink_Capabilities_Extended bytes; number 6.
        raw = bytes.fromhex("4b3254030619000000000000020243616e6f6e000000000000000000000000000000000000002c9101002cb104002cb14443"
                            "2cb10400000010320401020304b427")
        r = tp.decode_record(raw)
        self.assertEqual((r["version"], r["seq"], r["name"]), (3, 6, "Canon"))
        self.assertEqual(r["request"], {"rdo": 0x4344B12C, "pdo": 0x0004B12C})
        self.assertEqual(r["revision"], 0x32100000)
        self.assertEqual(r["sink_ext"], "01020304")
        self.assertEqual(r["bytes"], 65)
        # a version 1 record says it read neither
        self.assertIsNone(tp.decode_record(PIXEL)["revision"])
        self.assertIsNone(tp.decode_record(PIXEL)["sink_ext"])
        # a version 3 flag in a version 2 record is damage; an extended block of length 0 too
        v2 = bytearray(raw[:-2])
        v2[3] = 2
        with self.assertRaisesRegex(tp.BadRecord, "invalid"):
            tp.decode_record(bytes(v2) + le16(tp.crc16(bytes(v2))))
        z = bytearray(raw[:-2])
        z[-5] = 0
        with self.assertRaisesRegex(tp.BadRecord, "invalid"):
            tp.decode_record(bytes(z) + le16(tp.crc16(bytes(z))))
        with self.assertRaisesRegex(tp.BadRecord, "version"):
            tp.decode_record(raw[:3] + bytes([4]) + raw[4:])

    def test_sink_pdo_text(self):
        self.assertEqual(tp.pdo_text(0x0001912C), "5.0 V 3.0 A")
        self.assertEqual(tp.pdo_text(0x0002D12C), "9.0 V 3.0 A")
        self.assertEqual(tp.pdo_text(0xC8DC212C), "PPS 3.3-11.0 V 2.2 A")


class Responses(unittest.TestCase):
    def test_list(self):
        self.assertEqual(tp.decode_list(bytes([0, 3, 1, 4, 9])), [1, 4, 9])
        self.assertEqual(tp.decode_list(bytes([0, 0])), [])
        with self.assertRaises(tp.TwinError):
            tp.decode_list(bytes([0, 3, 1]))

    def test_chunks_reassemble(self):
        parts = [bytes([0, 3, k, 2, len(PIXEL[48 * k:48 * (k + 1)])]) + PIXEL[48 * k:48 * (k + 1)] for k in range(2)]
        self.assertEqual(tp.chunk_info(parts[0]), (3, 0, 2, PIXEL[:48]))
        self.assertEqual(tp.chunk_info(parts[1]), (3, 1, 2, PIXEL[48:]))

    def test_error_byte_raises_named(self):
        with self.assertRaisesRegex(tp.TwinError, "no such twin"):
            tp.chunk_info(bytes([3]))
        with self.assertRaisesRegex(tp.TwinError, "busy"):
            tp.decode_predict(bytes([7]))
        with self.assertRaisesRegex(tp.TwinError, "no charger list"):
            tp.decode_predict(bytes([8]))

    def test_predict(self):
        p = tp.decode_predict(predict_payload(seq=2, kind=2, limiter=3, flags=3, pos=5, mv=11000, ma=3000, mw=33000, want=55000))
        self.assertEqual((p["seq"], p["kind"], p["limiter"]), (2, "pps", "cable"))
        self.assertTrue(p["mismatch"] and p["observed"])
        self.assertEqual((p["pos"], p["mv"], p["ma"], p["mw"], p["want_mw"]), (5, 11000, 3000, 33000, 55000))
        self.assertEqual((p["claim_w"], p["cable_ma"], p["cable_mv"]), (65, 5000, 20000))
        self.assertEqual(p["summary"], "33W of 55W")
        self.assertEqual(p["note"], "limited by the cable")

    def test_predict_none_and_five_v(self):
        n = tp.decode_predict(predict_payload(kind=0, mv=0, ma=0, mw=0, want=0, pos=0))
        self.assertEqual((n["kind"], n["summary"]), ("none", "no contract"))
        v = tp.decode_predict(predict_payload(kind=3, limiter=2, flags=1, pos=1, mv=5000, ma=3000, mw=15000, want=15000))
        self.assertEqual((v["summary"], v["note"]), ("5V only", "limited by this charger"))

    def test_status_slot_run_learn_usb(self):
        s = tp.decode_status(bytes([0, 1, 0, 3, 1, 2, 0]) + le16(0b101))
        self.assertEqual((s["running"], s["reason"], s["count"], s["done"], s["current"], s["restoring"]), (True, "done", 3, 1, 2, False))
        self.assertEqual(s["with_verdict"], [1, 3])
        # the run's end reasons are named on the wire in twin::live::Reason order (3: a woken charger that never listed)
        self.assertEqual([tp.decode_status(bytes([0, 0, r, 1, 0, 0, 0]) + le16(0))["reason"] for r in range(4)],
                         ["done", "cancelled", "ports taken", "no offer"])
        slot = tp.decode_slot(bytes([0, 4, 1, 0]) + le16(9012))
        self.assertEqual((slot["seq"], slot["verdict"], slot["skip"], slot["measured_mv"]), (4, "granted", "", 9012))
        sk = tp.decode_slot(bytes([0, 4, 8, 2]) + le16(0))
        self.assertEqual((sk["verdict"], sk["skip"]), ("skipped", "above the monitor ceiling"))
        self.assertEqual(tp.decode_run(bytes([0, 2])), {"started": True, "refusal": "ok", "n": 2})
        self.assertEqual(tp.decode_run(bytes([5, 2]))["refusal"], "no charger")
        lr = tp.decode_learn(bytes([0, 0, 6, 1, 0]) + le16(0x18D1) + le16(0x4EE1) + bytes([3, 0b101]))
        self.assertEqual((lr["seq"], lr["existed"], lr["unchanged"], lr["vid"], lr["pid"], lr["num_pdos"]), (6, True, False, 0x18D1, 0x4EE1, 3))
        self.assertEqual(lr["has"], ["request", "observed"])
        with self.assertRaisesRegex(tp.TwinError, "no identify"):
            tp.decode_learn(bytes([9, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0]))
        u = tp.decode_usb(bytes([0, 2, 0, 5]) + b"USB 3")
        self.assertEqual((u["seq"], u["which"], u["text"]), (2, "declared", "USB 3"))


class Texts(unittest.TestCase):
    """The same cases as tests/test_twin_present.cpp: the screen, the shell and this tool print one string."""

    def out(self, kind, mv, ma, want_w, limiter="none", mw=None):
        return {"kind": kind, "mv": mv, "ma": ma, "mw": mv * ma // 1000 if mw is None else mw, "want_mw": want_w * 1000, "limiter": limiter}

    def test_summary(self):
        self.assertEqual(tp.summary(self.out("pps", 9000, 3000, 27)), "PPS up to 27W")
        self.assertEqual(tp.summary(self.out("pps", 11000, 3000, 33, "device")), "PPS up to 33W")
        self.assertEqual(tp.summary(self.out("pps", 5000, 3000, 33, "charger")), "PPS up to 15W")
        seen = dict(self.out("pps", 9000, 3000, 27), observed=True)   # a Request the K2 saw: a fact, the old words
        self.assertEqual(tp.summary(seen), "27W PPS")
        self.assertEqual(tp.summary(dict(self.out("pps", 5000, 3000, 33, "charger"), observed=True)), "15W of 33W")
        self.assertEqual(tp.summary(self.out("fixed", 12000, 3000, 55, "cable")), "36W of 55W")
        self.assertEqual(tp.summary(self.out("fixed", 12000, 3000, 36)), "36W 12V")
        self.assertEqual(tp.summary(self.out("five_v", 5000, 3000, 15)), "5V only")
        self.assertEqual(tp.summary(self.out("none", 0, 0, 0)), "no contract")
        self.assertEqual(tp.summary(dict(self.out("pps", 21000, 5000, 240, "charger", mw=105000), observed=True)), "105W of 240W")
        self.assertEqual(tp.summary(self.out("pps", 21000, 5000, 240, "charger", mw=105000)), "PPS up to 105W")

    def test_note(self):
        self.assertEqual([tp.note(x) for x in ("none", "device", "charger", "cable")],
                         ["full", "the device takes no more", "limited by this charger", "limited by the cable"])

    def test_proof(self):
        o = self.out("pps", 9000, 3000, 27)
        self.assertEqual(tp.proof("granted", 9012, o), "PPS ok 9.0V")
        self.assertEqual(tp.proof("granted", 9012, dict(o, observed=True)), "27W PPS ok 9.0V")
        self.assertEqual(tp.proof("refused", 0, o), "refused")
        self.assertEqual(tp.proof("out of band", 7400, o), "out of band 7.4V")
        self.assertEqual(tp.proof("not run", 0, o), "not run")


class Fake:
    """A scripted K2: the Cmd::Twin payload in, the response payload out."""

    def __init__(self, replies):
        self.replies, self.sent = list(replies), []

    def call(self, cmd, payload=b""):
        self.sent.append((cmd, bytes(payload)))
        item = self.replies.pop(0)
        if isinstance(item, Exception):
            raise item

        class R:
            pass
        r = R()
        r.payload = item
        return r


def chunks_of(raw, seq):
    n = (len(raw) + 47) // 48
    return [bytes([0, seq, k, n, len(raw[48 * k:48 * (k + 1)])]) + raw[48 * k:48 * (k + 1)] for k in range(n)]


def run_cli(argv, replies):
    link = Fake(replies)
    a = kr.build_parser().parse_args(argv)
    return kr.COMMANDS[a.command](link, a), link


class Command(unittest.TestCase):
    def setUp(self):
        self._sleep = kr.time.sleep
        kr.time.sleep = lambda s: None

    def tearDown(self):
        kr.time.sleep = self._sleep

    def test_ids(self):
        self.assertEqual(kr.CMD_TWIN, 0x63)

    def test_list(self):
        r, link = run_cli(["twin", "list"], [bytes([0, 2, 3, 5])] + chunks_of(PIXEL, 3) + chunks_of(PIXEL_REQ, 5))
        self.assertEqual([t["name"] for t in r["list"]], ["Pixel 9", "Pixel 9"])
        self.assertEqual(link.sent[0], (0x63, bytes([0])))
        self.assertEqual(link.sent[1], (0x63, bytes([1, 3, 0])))
        self.assertEqual(link.sent[2], (0x63, bytes([1, 3, 1])))
        text = kr.render("twin", r)
        self.assertIn("Pixel 9", text)
        self.assertIn("18D1:4EE1", text)

    def test_list_empty(self):
        r, _ = run_cli(["twin", "list"], [bytes([0, 0])])
        self.assertEqual(kr.render("twin", r), "no twins: learn one with `twin learn identify`")

    def test_show_has_record_and_usb_text(self):
        r, link = run_cli(["twin", "show", "3"], chunks_of(PIXEL, 3) + [bytes([0, 3, 0, 0]), bytes([0, 3, 1, 0])])
        self.assertEqual(r["twin"], "show")
        self.assertEqual(r["record"]["name"], "Pixel 9")
        self.assertEqual(link.sent[-2], (0x63, bytes([9, 3, 0])))
        self.assertEqual(link.sent[-1], (0x63, bytes([9, 3, 1])))
        text = kr.render("twin", r)
        self.assertIn("PPS 3.3-11.0 V", text)
        self.assertIn("no declared USB data", text)

    def test_predict_all_prints_the_row(self):
        replies = [bytes([0, 1, 3])] + chunks_of(PIXEL, 3) + [predict_payload(seq=3, kind=2, limiter=1, flags=0, mw=33000, want=33000)]
        r, link = run_cli(["twin", "predict"], replies)
        self.assertEqual(link.sent[-1], (0x63, bytes([2, 3])))
        self.assertEqual(r["rows"][0]["summary"], "PPS up to 33W")
        text = kr.render("twin", r)
        self.assertIn("Pixel 9", text)
        self.assertIn("PPS up to 33W", text)
        self.assertIn("the device takes no more", text)
        # --json output stays one document with the numbers
        json.dumps(r)

    def test_predict_without_a_charger_list(self):
        replies = [bytes([0, 1, 3])] + chunks_of(PIXEL, 3) + [bytes([8])]
        r, _ = run_cli(["twin", "predict"], replies)
        self.assertIsNone(r["rows"][0]["prediction"])
        self.assertIn("no charger list", kr.render("twin", r))

    def test_run_needs_confirm_on_the_host_too(self):
        with self.assertRaises(kr.FcRefused):
            run_cli(["twin", "run"], [])

    def test_run_polls_until_done_and_reads_each_result(self):
        status_running = bytes([0, 1, 0, 1, 0, 3, 0]) + le16(0)
        status_done = bytes([0, 0, 0, 1, 1, 0, 0]) + le16(0b100)
        replies = ([bytes([0, 1, 3]), bytes([0, 1])]                  # list, run accepted
                   + [status_running, status_done]                    # status polls
                   + [bytes([0, 3, 1, 0]) + le16(9012)]               # result for twin 3
                   + chunks_of(PIXEL, 3)                              # record for the name
                   + [predict_payload(seq=3, kind=2, limiter=0, flags=0, mw=27000, mv=9000, ma=3000, want=27000)])
        r, link = run_cli(["twin", "run", "--confirm"], replies)
        self.assertEqual(link.sent[1], (0x63, bytes([6, 1, 1, 3])))
        self.assertEqual(r["rows"][0]["proof"], "PPS ok 9.0V")
        self.assertEqual(r["run"]["reason"], "done")
        self.assertIn("granted", json.dumps(r))

    def test_run_refused_names_the_reason(self):
        with self.assertRaisesRegex(kr.TwinFailed, "no charger"):
            run_cli(["twin", "run", "--confirm", "3"], [bytes([5, 0])])

    def test_busy_status_is_explained(self):
        e = kr.RemoteError(kr.STATUS_BUSY, kr.CMD_TWIN, bytes([7]))
        self.assertIn("twin run", str(e))

    def test_learn_dry(self):
        payload = bytes([0, 0, 0, 0, 0]) + le16(0x18D1) + le16(0x4EE1) + bytes([3, 0])
        r, link = run_cli(["twin", "learn", "identify", "--dry", "--name", "Pixel 9"], [payload])
        self.assertEqual(link.sent[0], (0x63, bytes([3, 0, 1]) + b"Pixel 9"))
        self.assertEqual(r["learn"]["vid"], 0x18D1)
        self.assertIn("dry", kr.render("twin", r))

    def test_learn_sniff_refused_names_why(self):
        with self.assertRaisesRegex(kr.TwinFailed, "not inline"):
            run_cli(["twin", "learn", "sniff"], [bytes([9, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0])])

    def test_rename_and_delete(self):
        r, link = run_cli(["twin", "rename", "3", "Pixel Nine"], [bytes([0, 3])])
        self.assertEqual(link.sent[0], (0x63, bytes([4, 3]) + b"Pixel Nine"))
        with self.assertRaises(kr.FcRefused):
            run_cli(["twin", "delete", "3"], [])
        r, link = run_cli(["twin", "delete", "3", "--confirm"], [bytes([0, 3])])
        self.assertEqual(link.sent[0], (0x63, bytes([5, 3, 1])))
        with self.assertRaisesRegex(kr.TwinFailed, "no such twin"):
            run_cli(["twin", "delete", "9", "--confirm"], [bytes([3, 9])])

    def test_usb_and_cancel_and_status(self):
        r, link = run_cli(["twin", "usb", "2", "observed"], [bytes([0, 2, 1, 3]) + b"TB4"])
        self.assertEqual(link.sent[0], (0x63, bytes([9, 2, 1])))
        self.assertIn("TB4", kr.render("twin", r))
        r, _ = run_cli(["twin", "cancel"], [bytes([0, 1])])
        self.assertTrue(r["was_running"])
        r, _ = run_cli(["twin", "status"], [bytes([0, 0, 0, 0, 0, 0, 0]) + le16(0)])
        self.assertFalse(r["status"]["running"])

    def test_learn_inline_needs_confirm_on_the_host_too(self):
        with self.assertRaises(kr.FcRefused):
            run_cli(["twin", "learn", "inline"], [])
        with self.assertRaises(kr.FcRefused):
            run_cli(["pd", "interrogate", "--confirm"], [])   # --inline is the only mode

    def test_learn_inline_refused_names_why(self):
        with self.assertRaisesRegex(kr.TwinFailed, "holds its own contract.*pd release"):
            run_cli(["twin", "learn", "inline", "--confirm"], [bytes([10, 1, 5])])

    def test_learn_inline_polls_then_reads_the_answers(self):
        running = iq_status(running=1, pending=1, phase=4)
        done = iq_status(done=1, phase=7, match=2, asked=0xBF, ok=0x1B, refused=0x04, silent=0x20, sent=7, acks=6, resets=2,
                         vid=0x04A9, pid=0x3254, n_pdos=2, ext=24, revision=0x32100000, first=(1, 4, 0x4344B12C),
                         second=(1, 1, 0x1304B12C), seq=3, ms=3500)
        answers = [bytes([0, 3, 0, 2]) + le32(0x2601912C) + le32(0x0002D12C),
                   bytes([0, 3, 1, 24]) + bytes(range(1, 25)),
                   bytes([0, 3, 2, 1, 2]) + le32(0xFF00A841) + le32(0x18000000),
                   bytes([0, 3, 3, 1]) + le16(0xFF01) + bytes([1]) + le16(0xFF01) + le32(0x405),
                   bytes([0, 3, 4, 0, 0])]
        r, link = run_cli(["twin", "learn", "inline", "--confirm", "--name", "Canon"], [bytes([0, 1, 0]), running, done] + answers)
        self.assertEqual(link.sent[0], (0x63, bytes([11, 1, 2]) + b"Canon"))
        self.assertEqual(link.sent[1], (0x63, bytes([11, 0])))
        self.assertEqual([s[1] for s in link.sent[3:]], [bytes([11, 3, k]) for k in range(5)])
        s = r["inline"]
        self.assertEqual((s["phase"], s["match"], s["vid"], s["pid"], s["ms"]), ("done", "contract changed", 0x04A9, 0x3254, 3500))
        self.assertEqual(s["asked"], [n for n in tp.IQ_STEPS if n != "done"])   # six questions and EPR_Get_Sink_Cap
        self.assertEqual(s["answered"], ["Discover Identity", "Get_Sink_Cap", "Get_Revision", "Discover SVIDs"])
        self.assertEqual((s["refused"], s["silent"]), (["Get_Sink_Cap_Extended"], ["Discover Modes"]))
        self.assertEqual(s["first"], {"ready": True, "pos": 4, "rdo": 0x4344B12C})
        self.assertEqual(r["answers"]["sink_pdos"], [0x2601912C, 0x0002D12C])
        self.assertEqual(r["answers"]["sink_ext"], bytes(range(1, 25)).hex())
        self.assertEqual(r["answers"]["svids"], [0xFF01])
        self.assertEqual(r["answers"]["modes"], [{"svid": 0xFF01, "vdo": 0x405}])
        text = kr.render("twin", r)
        for want in ("done after 3.5 s", "asked as PD 3.x", "refused: Get_Sink_Cap_Extended", "silent: Discover Modes",
                     "04A9:3254", "contract changed", "saved as twin 3", "Revision: 0x32100000"):
            self.assertIn(want, text)
        json.dumps(r)

    def test_learn_inline_spoof_start_payload(self):
        # --spoof takes what `emarker spoof` takes: a built-in spelling, builtin:<id|name>, or a saved library id
        for target, kind, ident in (("oneplus", 0, 4), ("builtin:cuktech-6a", 0, 2), ("builtin:1", 0, 1), ("7", 1, 7),
                                    ("300", 1, 300)):
            r, link = run_cli(["twin", "learn", "inline", "--confirm", "--spoof", target, "--name", "X"],
                              [bytes([0, 1, 0]), iq_status(done=1, phase=7)] + [bytes([0, 3, k, 0, 0][:4 + (k > 1)]) for k in range(5)])
            self.assertEqual(link.sent[0], (0x63, bytes([11, 1, 2 | 4, kind, ident & 255, ident >> 8]) + b"X"), target)
        for bad in ("nonsense", "builtin:9", "0", "70000"):
            with self.assertRaises(kr.FcRefused):
                run_cli(["twin", "learn", "inline", "--confirm", "--spoof", bad], [])
        with self.assertRaises(kr.FcRefused):   # the spoof is the interrogation's: nothing for identify or sniff
            run_cli(["twin", "learn", "identify", "--spoof", "oneplus"], [])
        with self.assertRaisesRegex(kr.TwinFailed, "spoof target is unknown"):
            run_cli(["twin", "learn", "inline", "--confirm", "--spoof", "9"], [bytes([10, 1, 8])])

    def test_learn_inline_reports_epr_late_spoof_and_the_contract(self):
        done = iq_status(done=1, phase=7, match=1, asked=0xBF, ok=0x9F, refused=0, silent=0, late=0x04, flags=1 | 2, queries=3,
                         replies=2, n_epr=2, first=(1, 1, 0x1304B12C), second=(1, 1, 0x1304B12C), sent=7, ms=21000)
        answers = [bytes([0, 3, 0, 0]), bytes([0, 3, 1, 0]), bytes([0, 3, 2, 0, 0]), bytes([0, 3, 3, 0, 0]),
                   bytes([0, 3, 4, 2, 1]) + le32(0x2601912C) + le32(0xC1C8E1F4)]
        r, link = run_cli(["twin", "learn", "inline", "--confirm", "--spoof", "oneplus"], [bytes([0, 1, 0]), done] + answers)
        s, x = r["inline"], r["answers"]
        self.assertEqual((s["late"], s["spoof"], s["sopp_queries"], s["sopp_replies"], s["epr_partial"], s["num_epr_pdos"]),
                         (["Get_Sink_Cap_Extended"], True, 3, 2, True, 2))
        self.assertEqual(x["epr_pdos"], [0x2601912C, 0xC1C8E1F4])
        self.assertTrue(x["epr_partial"])
        text = kr.render("twin", r)
        for want in ("late: Get_Sink_Cap_Extended", "EPR sink PDO 1:", "EPR sink PDO 2:", "raw=0xC1C8E1F4", "only the first chunk",
                     "cable spoof: SOP' cable queried 3 times (the charger's or the device's), 2 replies", "same contract"):
            self.assertIn(want, text)
        json.dumps(r)
        # a spoof run nobody queried says so (the Ra is not driven inline)
        r, _ = run_cli(["twin", "learn", "inline", "--confirm", "--spoof", "oneplus"],
                       [bytes([0, 1, 0]), iq_status(done=1, phase=7, flags=1, first=(1, 1, 5), second=(1, 1, 5))] + answers[:4]
                       + [bytes([0, 3, 4, 0, 0])])
        self.assertIn("nobody queried the cable", kr.render("twin", r))
        # a real e-marked cable answered the charger itself (hardware 2026-11-13): said plainly, not as "0 replies"
        r, _ = run_cli(["twin", "learn", "inline", "--confirm", "--spoof", "oneplus"],
                       [bytes([0, 1, 0]), iq_status(done=1, phase=7, flags=1 | 4, queries=2, replies=0,
                                                    first=(1, 1, 5), second=(1, 1, 5))] + answers[:4] + [bytes([0, 3, 4, 0, 0])])
        self.assertTrue(r["inline"]["sopp_real"])
        text = kr.render("twin", r)
        self.assertIn("a real e-marked cable answered the charger; the spoof cannot reach it, use a cable without a chip", text)
        self.assertNotIn("nobody queried", text)
        json.dumps(r)

    def test_pd_interrogate_is_a_dry_learn(self):
        done = iq_status(done=1, phase=8, why=4, how=3)
        answers = [bytes([0, 3, 0, 0]), bytes([0, 3, 1, 0]), bytes([0, 3, 2, 0, 0]), bytes([0, 3, 3, 0, 0]), bytes([0, 3, 4, 0, 0])]
        r, link = run_cli(["pd", "interrogate", "--inline", "--confirm"], [bytes([0, 1, 0]), done] + answers)
        self.assertEqual(link.sent[0], (0x63, bytes([11, 1, 3])))   # confirm + dry
        text = kr.render("pd", r)
        self.assertIn("aborted (no renegotiation)", text)
        self.assertIn("learned nothing: the device answered nothing to learn from", text)
        r, _ = run_cli(["pd", "interrogate", "--inline", "--confirm"], [bytes([0, 1, 0]), iq_status(phase=7, why=1)] + answers)
        self.assertIn("learned nothing (stopped)", kr.render("pd", r))
        # A source that Hard Resets (7) or renegotiates (8) when asked is named, not shown as a number.
        for why, words in ((7, "charger resets when asked"), (8, "charger restarts when asked")):
            r, _ = run_cli(["pd", "interrogate", "--inline", "--confirm"],
                           [bytes([0, 1, 0]), iq_status(done=1, phase=8, why=why, how=3)] + answers)
            self.assertIn("aborted (" + words + ")", kr.render("pd", r))

    def test_export_writes_decoded_records_with_raw_hex(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "twins.json")
            replies = [bytes([0, 1, 3])] + chunks_of(PIXEL, 3) + [bytes([0, 3, 0, 0]), bytes([0, 3, 1, 0])]
            r, _ = run_cli(["twin", "export", "--out", path], replies)
            data = json.load(open(path))
            self.assertEqual(r["count"], 1)
            self.assertEqual(data[0]["name"], "Pixel 9")
            self.assertEqual(data[0]["raw"], PIXEL.hex())
            self.assertIn("usb", data[0])


if __name__ == "__main__":
    unittest.main()
