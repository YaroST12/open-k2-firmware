# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Vendor (WebUSB) host transport (tools/k2_usb.py) over a fake pyusb, and the transport pick in k2_remote."""
import collections
import os
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock

TOOLS = Path(__file__).resolve().parent.parent / "tools"
sys.path.insert(0, str(TOOLS))
import k2_lock  # noqa: E402
import k2_usb  # noqa: E402

TIMEOUT_ERRNO = 110


class FakeTimeout(Exception):
    errno = TIMEOUT_ERRNO


class Ep:
    def __init__(self, addr, attrs=0x02):
        self.bEndpointAddress, self.bmAttributes = addr, attrs


class Itf(list):
    def __init__(self, number, klass, eps, alt=0):
        super().__init__(eps)
        self.bInterfaceNumber, self.bInterfaceClass, self.bAlternateSetting = number, klass, alt


class FakeDev:
    def __init__(self, itfs, bus=1, address=7, serial="K2-1", product="Open-K2 Logger"):
        self.cfg = [list(itfs)]
        self.bus, self.address = bus, address
        self.iSerialNumber, self.iManufacturer, self.iProduct = 1, 2, 3
        self.strings = {1: serial, 2: "Open-K2", 3: product}
        self.reads = collections.deque()      # bytes, or an Exception to raise
        self.writes = []
        self.claimed = []
        self.released = []
        self.kernel_detached = []
        self.write_return = None

    def __iter__(self):
        return iter(self.cfg)

    def get_active_configuration(self):
        return self.cfg[0]

    def is_kernel_driver_active(self, n):
        return n == 2

    def detach_kernel_driver(self, n):
        self.kernel_detached.append(n)

    def write(self, ep, data, timeout):
        self.writes.append((ep, bytes(data), timeout))
        return len(data) if self.write_return is None else self.write_return

    def read(self, ep, n, timeout):
        self.last_read = (ep, n, timeout)
        if not self.reads:
            raise FakeTimeout("timed out")
        item = self.reads.popleft()
        if isinstance(item, Exception):
            raise item
        return list(item)


def vendor_dev(**kw):
    """HID interface 0 plus a vendor interface 2 (EP 0x03 OUT / 0x83 IN), like the HID+WebUSB fallback mode."""
    return FakeDev([Itf(0, 3, [Ep(0x81, 0x03)]), Itf(2, 0xFF, [Ep(0x03), Ep(0x83)])], **kw)


def hid_only_dev(**kw):
    return FakeDev([Itf(0, 3, [Ep(0x81, 0x03)])], **kw)


def install(devs):
    """Put a fake usb.core/usb.util in place of pyusb; returns the util namespace (claim/release log)."""
    util = types.SimpleNamespace(
        claimed=[], released=[], disposed=[],
        get_string=lambda dev, i: dev.strings[i],
        claim_interface=lambda dev, n: util.claimed.append(n),
        release_interface=lambda dev, n: util.released.append(n),
        dispose_resources=lambda dev: util.disposed.append(dev))
    core = types.SimpleNamespace(find=lambda find_all=False, idVendor=None, idProduct=None:
                                 [d for d in devs] if (idVendor, idProduct) == (0x0716, 0x5060) else [])
    return mock.patch.object(k2_usb, "_load_pyusb", lambda: (core, util)), util


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        env = mock.patch.dict(os.environ, {"K2_LOCK_FILE": str(Path(self.dir.name) / "k2.lock")})
        env.start()
        self.addCleanup(env.stop)
        os.environ.pop("K2_LOCK_OWNER_PID", None)
        os.environ.pop("K2_TRANSPORT", None)


def frame(first=0xA6):
    return bytes([first, 0x4B]) + bytes(range(62))


class Discovery(Base):
    def test_vendor_interface_found_with_endpoints(self):
        self.assertEqual(k2_usb.vendor_interface(vendor_dev()), (2, 0x83, 0x03))

    def test_hid_only_device_has_no_vendor_interface(self):
        self.assertIsNone(k2_usb.vendor_interface(hid_only_dev()))

    def test_interrupt_endpoints_are_not_bulk(self):
        d = FakeDev([Itf(1, 0xFF, [Ep(0x01, 0x03), Ep(0x81, 0x03)])])
        self.assertIsNone(k2_usb.vendor_interface(d))

    def test_alternate_settings_ignored(self):
        d = FakeDev([Itf(1, 0xFF, [Ep(0x01), Ep(0x81)], alt=1)])
        self.assertIsNone(k2_usb.vendor_interface(d))

    def test_find_vendor_devices_lists_only_vendor_k2s(self):
        patch, _ = install([hid_only_dev(address=3), vendor_dev(address=9)])
        with patch:
            devs = k2_usb.find_vendor_devices()
        self.assertEqual([d["path"] for d in devs], [b"usb:1:9"])
        self.assertEqual(devs[0]["product_string"], "Open-K2 Logger")
        self.assertEqual(devs[0]["serial_number"], "K2-1")

    def test_find_without_pyusb_is_empty(self):
        def missing():
            raise ImportError("no usb")
        with mock.patch.object(k2_usb, "_load_pyusb", missing):
            self.assertEqual(k2_usb.find_vendor_devices(), [])


class Transport(Base):
    def open(self, dev=None, **kw):
        dev = dev or vendor_dev()
        patch, util = install([dev])
        patch.start()
        self.addCleanup(patch.stop)
        t = k2_usb.UsbVendorTransport(**kw)
        self.addCleanup(t.close)
        return t, dev, util

    def test_open_claims_interface_and_detaches_kernel_driver(self):
        t, dev, util = self.open()
        self.assertEqual(util.claimed, [2])
        self.assertEqual(dev.kernel_detached, [2])
        self.assertEqual(t.path, b"usb:1:7")

    def test_write_is_a_raw_64_byte_bulk_out_without_report_id(self):
        t, dev, _ = self.open()
        self.assertEqual(t.write(frame(0xA5)), 64)
        self.assertEqual(dev.writes, [(0x03, frame(0xA5), 1000)])

    def test_write_wrong_size_rejected(self):
        t, _, _ = self.open()
        with self.assertRaises(k2_usb.UsbTransportError):
            t.write(b"\x00" * 10)

    def test_short_write_is_an_error(self):
        t, dev, _ = self.open()
        dev.write_return = 10
        with self.assertRaises(k2_usb.UsbTransportError):
            t.write(frame())

    def test_read_returns_one_frame(self):
        t, dev, _ = self.open()
        dev.reads.append(frame())
        self.assertEqual(t.read(100), frame())
        self.assertEqual(dev.last_read[0], 0x83)

    def test_read_skips_zero_length_packets(self):
        t, dev, _ = self.open()
        dev.reads.extend([b"", b"", frame()])
        self.assertEqual(t.read(100), frame())

    def test_read_timeout_is_empty_bytes(self):
        t, _, _ = self.open()
        self.assertEqual(t.read(20), b"")

    def test_read_zero_timeout_polls_once_with_one_ms(self):
        t, dev, _ = self.open()
        self.assertEqual(t.read(0), b"")
        self.assertEqual(dev.last_read[2], 1)       # never libusb's 0 = wait forever

    def test_read_reassembles_short_chunks(self):
        t, dev, _ = self.open()
        f = frame()
        dev.reads.extend([f[:20], b"", f[20:]])
        self.assertEqual(t.read(100), f)

    def test_two_frames_in_one_chunk_come_out_one_at_a_time(self):
        t, dev, _ = self.open()
        a, b = frame(0xA6), frame(0xA7)
        dev.reads.append(a + b)
        self.assertEqual(t.read(100), a)
        self.assertEqual(t.read(100), b)

    def test_read_error_closes_and_raises(self):
        t, dev, util = self.open()
        dev.reads.append(OSError(19, "No such device"))
        with self.assertRaises(k2_usb.UsbTransportError):
            t.read(100)
        self.assertEqual(util.released, [2])
        with self.assertRaises(k2_usb.UsbTransportError):
            t.read(100)

    def test_close_releases_interface_and_lock(self):
        t, _, util = self.open()
        t.close()
        self.assertEqual(util.released, [2])
        with k2_lock.hold(wait_s=0.2):               # the lock is free again
            pass
        t.close()                                    # idempotent
        self.assertEqual(util.released, [2])

    def test_open_without_device_raises_and_frees_lock(self):
        patch, _ = install([hid_only_dev()])
        with patch:
            with self.assertRaises(k2_usb.UsbTransportError):
                k2_usb.UsbVendorTransport()
        with k2_lock.hold(wait_s=0.2):
            pass

    def test_open_by_path_selects_the_device(self):
        a, b = vendor_dev(address=5), vendor_dev(address=6)
        patch, _ = install([a, b])
        with patch:
            t = k2_usb.UsbVendorTransport(b"usb:1:6")
            self.addCleanup(t.close)
        self.assertEqual(t.path, b"usb:1:6")

    def test_open_with_unknown_path_raises(self):
        patch, _ = install([vendor_dev()])
        with patch:
            with self.assertRaises(k2_usb.UsbTransportError):
                k2_usb.UsbVendorTransport("usb:9:9")

    def test_claim_failure_names_the_udev_rule_and_frees_lock(self):
        dev = vendor_dev()
        patch, util = install([dev])

        def deny(d, n):
            raise OSError(13, "Access denied")
        util.claim_interface = deny
        with patch:
            with self.assertRaises(k2_usb.UsbTransportError) as cm:
                k2_usb.UsbVendorTransport()
        self.assertIn("udev", str(cm.exception))
        with k2_lock.hold(wait_s=0.2):
            pass

    def test_missing_pyusb_gives_an_install_hint(self):
        def missing():
            raise ImportError("no usb")
        with mock.patch.object(k2_usb, "_load_pyusb", missing):
            with self.assertRaises(k2_usb.UsbTransportError) as cm:
                k2_usb.UsbVendorTransport()
        self.assertIn("pyusb", str(cm.exception))


class Pick(Base):
    def test_auto_prefers_vendor_when_the_interface_is_there(self):
        patch, _ = install([vendor_dev()])
        with patch:
            self.assertTrue(k2_usb.vendor_wanted())

    def test_auto_falls_back_to_hid_when_no_vendor_interface(self):
        patch, _ = install([hid_only_dev()])
        with patch:
            self.assertFalse(k2_usb.vendor_wanted())

    def test_env_hid_forces_hid(self):
        patch, _ = install([vendor_dev()])
        with patch, mock.patch.dict(os.environ, {"K2_TRANSPORT": "hid"}):
            self.assertFalse(k2_usb.vendor_wanted())

    def test_env_usb_forces_vendor_even_without_a_probe(self):
        patch, _ = install([hid_only_dev()])
        with patch, mock.patch.dict(os.environ, {"K2_TRANSPORT": "usb"}):
            self.assertTrue(k2_usb.vendor_wanted())

    def test_hid_device_path_is_never_vendor(self):
        patch, _ = install([vendor_dev()])
        with patch:
            self.assertFalse(k2_usb.vendor_wanted(b"/dev/hidraw3"))
            self.assertTrue(k2_usb.vendor_wanted("usb:1:7"))

    def test_unknown_env_value_means_auto(self):
        patch, _ = install([hid_only_dev()])
        with patch, mock.patch.dict(os.environ, {"K2_TRANSPORT": "bogus"}):
            self.assertEqual(k2_usb.preference(), "auto")


class RemoteOpen(Base):
    """k2_remote.open_transport and k2_flasher.RealDfuEnv use the pick."""

    def setUp(self):
        super().setUp()
        import k2_remote
        self.kr = k2_remote

    def test_open_transport_returns_the_vendor_transport(self):
        patch, _ = install([vendor_dev()])
        with patch:
            t = self.kr.open_transport()
            self.addCleanup(t.close)
        self.assertIsInstance(t, k2_usb.UsbVendorTransport)

    def test_open_transport_falls_back_to_hid(self):
        made = []

        class FakeHid:
            def __init__(self, path=None, wait_sec=0.0):
                made.append((path, wait_sec))

        patch, _ = install([hid_only_dev()])
        with patch, mock.patch.object(self.kr, "LinuxHidTransport", FakeHid):
            t = self.kr.open_transport("/dev/hidraw1", wait_sec=2.0)
        self.assertIsInstance(t, FakeHid)
        self.assertEqual(made, [("/dev/hidraw1", 2.0)])

    def test_dfu_env_enumeration_is_the_union_and_classifies_app(self):
        import k2_flasher
        patch, _ = install([vendor_dev(product="Open-K2 Logger")])
        with patch, mock.patch.object(k2_flasher.LinuxHidTransport, "enumerate_k2", classmethod(lambda cls: [])):
            env = k2_flasher.RealDfuEnv(enumerate_fn=None)
            devs = env.enumerate_fn()
        self.assertEqual(k2_flasher.classify_enumeration(devs), "app")

    def test_dfu_env_bootloader_is_seen_on_hid_even_with_pyusb(self):
        import k2_flasher
        boot = {"path": b"/dev/hidraw0", "product_string": k2_flasher.BOOT_PRODUCT, "serial_number": ""}
        patch, _ = install([])
        with patch, mock.patch.object(k2_flasher.LinuxHidTransport, "enumerate_k2", classmethod(lambda cls: [boot])):
            env = k2_flasher.RealDfuEnv()
            self.assertEqual(k2_flasher.classify_enumeration(env.enumerate_fn()), "bootloader")


class DfuOverVendor(Base):
    """The DFU_ENTER request travels over the vendor pipe exactly as over HID."""

    def test_request_dfu_writes_the_confirmed_frame_on_bulk_out(self):
        import k2_flasher
        import k2_remote
        dev = vendor_dev()
        patch, _ = install([dev])
        with patch:
            env = k2_flasher.RealDfuEnv(ping_timeout_s=0.05)
            # the device answers any request with a plain OK response of the same command
            orig_write = dev.write

            def answering_write(ep, data, timeout):
                n = orig_write(ep, data, timeout)
                cmd, seq = data[2], data[3]
                resp = bytearray(64)
                resp[0:2] = k2_remote.RSP_MAGIC
                resp[2], resp[3], resp[4] = cmd, seq, 0
                dev.reads.append(bytes(resp))
                return n
            dev.write = answering_write
            env._request()
        sent = dev.writes[0][1]
        self.assertEqual(sent[:2], k2_remote.REQ_MAGIC)
        self.assertEqual(sent[2], k2_remote.CMD_DFU_ENTER)
        self.assertEqual(sent[5:9], b"DFU!")


if __name__ == "__main__":
    unittest.main()
