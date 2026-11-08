# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Tests for entering the bootloader from the app (no device needed).

Covers the flasher state machine (k2_flasher.ensure_dfu) with a fake device and the
k2_remote `dfu --confirm` command (payload = the 4-byte magic "DFU!", same as
kDfuMagic in include/k2/core/remote_proto.hpp).
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))

import k2_flasher as kf
import k2_remote as kr


class FakeDevice:
    """Scripted device. `states` is consumed by state(); the last entry repeats."""

    def __init__(self, states, request_error=None):
        self.states = list(states)
        self.requests = 0
        self.t = 0.0
        self.request_error = request_error
        self.boot_seen_t = None

    def state(self):
        st = self.states.pop(0) if len(self.states) > 1 else self.states[0]
        if st == "bootloader" and self.boot_seen_t is None:
            self.boot_seen_t = self.t
        return st

    def request_dfu(self):
        self.requests += 1
        if self.request_error:
            raise self.request_error

    def sleep(self, s):
        self.t += s

    def now(self):
        return self.t


class EnsureDfu(unittest.TestCase):
    def test_already_in_bootloader_sends_nothing(self):
        d = FakeDevice(["bootloader"])
        self.assertEqual(kf.ensure_dfu(d, 5, 0.25), "already-dfu")
        self.assertEqual(d.requests, 0)

    def test_app_then_bootloader(self):
        d = FakeDevice(["app", "none", "none", "bootloader"])
        self.assertEqual(kf.ensure_dfu(d, 5, 0.25), "entered-dfu")
        self.assertEqual(d.requests, 1)

    def test_waits_for_disappear_then_appear_then_settles(self):
        d = FakeDevice(["app", "app", "none", "none", "none", "bootloader"])
        self.assertEqual(kf.ensure_dfu(d, 5, 0.25, settle_s=0.3), "entered-dfu")
        self.assertGreaterEqual(d.t - d.boot_seen_t, 0.3)       # settle before the first frame

    def test_bootloader_without_a_detach_is_not_trusted(self):
        # the host kept the app's enumeration: whatever answers is the stale device
        d = FakeDevice(["app", "bootloader"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        msg = str(cm.exception)
        self.assertIn("never disconnected", msg)
        self.assertIn("replug the K2 without holding MINUS", msg)
        self.assertIn("stays in DFU", msg)

    def test_nothing_connected_names_the_fallback(self):
        d = FakeDevice(["none"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 5, 0.25)
        self.assertIn("hold MINUS while plugging in", str(cm.exception))
        self.assertEqual(d.requests, 0)

    def test_timeout_when_device_vanishes(self):
        d = FakeDevice(["app", "none"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        self.assertIn("hold MINUS while plugging in", str(cm.exception))
        self.assertIn("did not enumerate", str(cm.exception))
        self.assertIn("replug the K2 without holding MINUS", str(cm.exception))
        self.assertLessEqual(d.t, 2.5)

    def test_app_still_answering_means_marker_ignored(self):
        d = FakeDevice(["app"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        msg = str(cm.exception)
        self.assertIn("still answering", msg)
        self.assertIn("commit-marker", msg)
        self.assertEqual(d.requests, 1)

    def test_silent_device_is_a_hang_not_an_app(self):
        # ping times out on a device that still enumerates as the app: firmware hung before detach
        d = FakeDevice(["app", "hung"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        msg = str(cm.exception)
        self.assertIn("hung", msg)
        self.assertIn("replug", msg)
        self.assertNotIn("still answering", msg)
        self.assertNotIn("commit-marker", msg)

    def test_transient_silence_before_the_detach_is_tolerated(self):
        d = FakeDevice(["app", "hung", "none", "bootloader"])
        self.assertEqual(kf.ensure_dfu(d, 5, 0.25), "entered-dfu")

    def test_hung_before_the_request_is_refused(self):
        d = FakeDevice(["hung"])
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        self.assertIn("hung", str(cm.exception))
        self.assertEqual(d.requests, 0)

    def test_request_failure_is_reported(self):
        d = FakeDevice(["app"], request_error=kf.DfuEntryError("DFU_ENTER refused"))
        with self.assertRaises(kf.DfuEntryError) as cm:
            kf.ensure_dfu(d, 2, 0.25)
        self.assertIn("refused", str(cm.exception))


class FakeTransport:
    def __init__(self):
        self.closed = False

    def close(self):
        self.closed = True


class OpenWithRetry(unittest.TestCase):
    """First handshake with a soft-reset bootloader: retry with growing settle delays."""

    def run_retry(self, failures, delays=(0.5, 1.0, 2.0)):
        opened, sleeps = [], []

        def open_fn():
            t = FakeTransport()
            opened.append(t)
            return t

        def handshake(t):
            if len(opened) <= failures:
                raise kf.ProtocolError("Timed out waiting for command 0x02")

        try:
            got = kf.open_with_retry(open_fn, handshake, delays=delays, sleep=sleeps.append)
        except kf.DfuEntryError as e:
            return opened, sleeps, e
        return opened, sleeps, got

    def test_first_attempt_ok(self):
        opened, sleeps, got = self.run_retry(0)
        self.assertIs(got, opened[0])
        self.assertEqual(sleeps, [0.5])

    def test_retries_close_the_failed_handle_and_back_off(self):
        opened, sleeps, got = self.run_retry(2)
        self.assertEqual(len(opened), 3)
        self.assertTrue(opened[0].closed and opened[1].closed)
        self.assertFalse(opened[2].closed)
        self.assertIs(got, opened[2])
        self.assertEqual(sleeps, [0.5, 1.0, 2.0])

    def test_gives_up_with_every_error_listed(self):
        opened, sleeps, err = self.run_retry(99)
        self.assertIsInstance(err, kf.DfuEntryError)
        self.assertTrue(all(t.closed for t in opened))
        self.assertEqual(len(opened), 3)
        self.assertIn("3 attempts", str(err))
        self.assertIn("replug the K2 without holding MINUS", str(err))

    def test_open_failure_counts_as_an_attempt(self):
        calls = []

        def open_fn():
            calls.append(1)
            if len(calls) < 2:
                raise RuntimeError("No WITRN K2 detected")
            return FakeTransport()

        got = kf.open_with_retry(open_fn, lambda t: None, delays=(0.1, 0.1), sleep=lambda s: None)
        self.assertIsInstance(got, FakeTransport)
        self.assertEqual(len(calls), 2)


class ErrorLog(unittest.TestCase):
    def test_tee_writes_both_and_flushes(self):
        import io
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "sub" / "k2.log"
            console = io.StringIO()
            with kf.tee_output(log, console) as t:
                print("Programming K2...", file=t)
                t.write("partial")
                self.assertIn("Programming K2", log.read_text())   # flushed before exit
            self.assertIn("Programming K2...", console.getvalue())
            self.assertIn("partial", log.read_text())

    def test_log_function_records_the_traceback(self):
        import tempfile
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "k2.log"
            try:
                raise kf.ProtocolError("boom")
            except kf.ProtocolError as e:
                kf.log_exception(log, e)
            text = log.read_text()
            self.assertIn("ProtocolError: boom", text)
            self.assertIn("Traceback", text)


BOOT = {"path": b"/dev/hidraw3", "product_string": "WITRN.K2", "serial_number": "20230727"}
APP = {"path": b"/dev/hidraw3", "product_string": "Open-K2 Logger", "serial_number": "0001"}


class Classify(unittest.TestCase):
    def test_strings_tell_app_from_bootloader(self):
        self.assertEqual(kf.classify_enumeration([]), "none")
        self.assertEqual(kf.classify_enumeration([BOOT]), "bootloader")
        self.assertEqual(kf.classify_enumeration([APP]), "app")
        self.assertEqual(kf.classify_enumeration([{"path": b"x"}]), "unknown")
        self.assertEqual(kf.classify_enumeration([{"product_string": None, "serial_number": None}]), "unknown")


class RealEnvState(unittest.TestCase):
    """RealDfuEnv.state() must never send traffic to an enumerated bootloader and never leak
    a transport exception (a write to a fresh soft-reset bootloader times out)."""

    def make(self, devs, ping=None):
        pings = []

        def do_ping():
            pings.append(1)
            if ping is not None:
                raise ping
        env = kf.RealDfuEnv(enumerate_fn=lambda: devs, ping_fn=do_ping)
        return env, pings

    def test_bootloader_strings_send_nothing(self):
        env, pings = self.make([BOOT], ping=kf.ProtocolError("HID write failed: Connection timed out"))
        self.assertEqual(env.state(), "bootloader")
        self.assertEqual(pings, [])

    def test_nothing_enumerated(self):
        env, pings = self.make([])
        self.assertEqual(env.state(), "none")
        self.assertEqual(pings, [])

    def test_app_pings(self):
        env, pings = self.make([APP])
        self.assertEqual(env.state(), "app")
        self.assertEqual(len(pings), 1)

    def test_app_that_does_not_answer_is_hung(self):
        env, _ = self.make([APP], ping=kf.ProtocolError("HID write failed: Connection timed out"))
        self.assertEqual(env.state(), "hung")
        env, _ = self.make([APP], ping=kr.RemoteTimeout("no response"))
        self.assertEqual(env.state(), "hung")

    def test_app_answering_with_an_error_is_app(self):
        env, _ = self.make([APP], ping=kr.RemoteError(2, 1))
        self.assertEqual(env.state(), "app")

    def test_unknown_strings_ping_and_transport_error_means_bootloader(self):
        env, pings = self.make([{"path": b"x"}], ping=kf.ProtocolError("HID write failed"))
        self.assertEqual(env.state(), "bootloader")
        self.assertEqual(len(pings), 1)

    def test_ensure_dfu_survives_the_stale_write_timeout(self):
        seq = [[APP], [], [BOOT]]
        env = kf.RealDfuEnv(enumerate_fn=lambda: seq.pop(0) if len(seq) > 1 else seq[0],
                            ping_fn=lambda: None, request_fn=lambda: None)
        env.sleep = lambda s: None
        self.assertEqual(kf.ensure_dfu(env, 2, 0.1, 0.0), "entered-dfu")


class RetryReport(unittest.TestCase):
    def test_every_attempt_is_reported_as_it_happens(self):
        lines = []
        calls = []

        def handshake(t):
            calls.append(1)
            if len(calls) < 3:
                raise kf.ProtocolError("HID write failed: Connection timed out")
        t = kf.open_with_retry(lambda: object(), handshake, (0, 0, 0), lambda s: None, report=lines.append)
        self.assertIsNotNone(t)
        self.assertEqual(len(lines), 3)
        self.assertIn("attempt 1", lines[0])
        self.assertIn("Connection timed out", lines[0])
        self.assertIn("attempt 3", lines[2])
        self.assertIn("ok", lines[2])


class TransportErrors(unittest.TestCase):
    """A hid exception that is not an OSError (\"HIDException: Success\") must not leave the
    handle half-open."""

    def make(self, exc):
        t = object.__new__(kf.LinuxHidTransport)
        t._mode = "cython"

        class Dev:
            closed = False

            def read(self, *a):
                raise exc

            def write(self, *a):
                raise exc

            def set_nonblocking(self, v):
                pass

            def close(self):
                Dev.closed = True

        t.device = Dev()
        t.path = b"x"
        return t, Dev

    def test_read_error_closes_and_raises(self):
        t, Dev = self.make(Exception("Success"))
        with self.assertRaises(kf.ProtocolError):
            t.read(100)
        self.assertTrue(Dev.closed)
        self.assertIsNone(t.device)
        with self.assertRaises(kf.ProtocolError):
            t.read(100)                      # closed transport stays an error, not a hang

    def test_write_error_closes_and_raises(self):
        t, Dev = self.make(Exception("Connection timed out"))
        with self.assertRaises(kf.ProtocolError):
            t.write(bytes(64))
        self.assertTrue(Dev.closed)


class RemoteFaultCommand(unittest.TestCase):
    def test_payload(self):
        self.assertEqual(kr.CMD_DEBUG_FAULT, 0x31)
        self.assertEqual(kr.fault_payload("udf"), b"BOOM\x00")
        self.assertEqual(kr.fault_payload("read"), b"BOOM\x01")
        self.assertEqual(kr.fault_payload("stack"), b"BOOM\x02")
        a = kr.build_parser().parse_args(["fault", "--confirm", "read"])
        self.assertEqual(a.kind, "read")
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["fault", "udf"])


class RemoteDfuCommand(unittest.TestCase):
    def test_constants_and_request_bytes(self):
        self.assertEqual(kr.CMD_DFU_ENTER, 0x30)
        self.assertEqual(kr.DFU_MAGIC, bytes([0x44, 0x46, 0x55, 0x21]))
        frame = kr.pack_request(kr.CMD_DFU_ENTER, 3, kr.DFU_MAGIC)
        self.assertEqual(frame[:9], bytes([0xA5, 0x4B, 0x30, 0x03, 0x04, 0x44, 0x46, 0x55, 0x21]))

    def test_parser_requires_confirm(self):
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["dfu"])
        a = kr.build_parser().parse_args(["dfu", "--confirm"])
        self.assertEqual(a.command, "dfu")
        self.assertEqual(a.reset, "sys")
        self.assertEqual(kr.build_parser().parse_args(["dfu", "--confirm", "--reset", "iwdg"]).reset, "iwdg")
        with self.assertRaises(SystemExit):
            kr.build_parser().parse_args(["dfu", "--confirm", "--reset", "nope"])

    def test_reset_mode_payload(self):
        self.assertEqual(kr.dfu_payload("sys"), kr.DFU_MAGIC)                    # 4 bytes: old form
        self.assertEqual(kr.dfu_payload("iwdg"), kr.DFU_MAGIC + b"\x01")
        sent = []

        class Link:
            def call(self, cmd, payload=b""):
                sent.append(payload)
        a = kr.build_parser().parse_args(["dfu", "--confirm", "--reset", "iwdg"])
        kr.COMMANDS["dfu"](Link(), a)
        self.assertEqual(sent, [kr.DFU_MAGIC + b"\x01"])

    def test_command_sends_the_magic_once(self):
        sent = []

        class Link:
            def call(self, cmd, payload=b""):
                sent.append((cmd, payload))
                return kr.parse_response(bytes([0xA6, 0x4B, cmd, 1, 0, 0]).ljust(64, b"\x00"))

        a = kr.build_parser().parse_args(["dfu", "--confirm"])
        self.assertEqual(kr.COMMANDS["dfu"](Link(), a), {"dfu": True})
        self.assertEqual(sent, [(kr.CMD_DFU_ENTER, kr.DFU_MAGIC)])
        self.assertIn("bootloader", kr.render("dfu", {"dfu": True}))


class FlasherCli(unittest.TestCase):
    def test_flash_accepts_auto_dfu(self):
        p = kf.build_parser()
        a = p.parse_args(["flash", "x.k2", "--auto-dfu"])
        self.assertTrue(a.auto_dfu)
        self.assertFalse(p.parse_args(["flash", "x.k2"]).auto_dfu)


if __name__ == "__main__":
    unittest.main()
