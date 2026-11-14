#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""The K2's vendor (WebUSB) pipe as a host transport: pyusb over libusb, same interface as LinuxHidTransport.

Firmware K2_WEBUSB=1 builds expose a vendor-class interface (0xFF) with one bulk IN and one bulk OUT endpoint that carry
the remote protocol's 64-byte frames (include/k2/core/remote_proto.hpp), the same bytes as the HID reports without the
report-ID byte. open_transport() picks it when the device has it and falls back to HID otherwise; K2_TRANSPORT=hid|usb
forces one (for A/B comparison). The bootloader flasher always uses HID (the bootloader is fixed).

Linux needs access to the USB node. Without a rule only root can open it:
    SUBSYSTEM=="usb", ATTR{idVendor}=="0716", ATTR{idProduct}=="5060", TAG+="uaccess"
in /etc/udev/rules.d/70-open-k2.rules, then `udevadm control --reload && udevadm trigger` and replug (MODE="0666" works too).
pyusb needs libusb 1.x (`pip install pyusb`). On Windows the firmware's Microsoft OS 2.0 descriptors bind WinUSB by itself.

Reads skip zero-length packets (the device sends a ZLP after a transfer that is a multiple of 64 bytes) and reassemble
frames if the host stack hands over short reads. A read with timeout 0 is a poll (1 ms), never "wait forever".
"""
import errno
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_lock  # noqa: E402

VID = 0x0716
PID = 0x5060
FRAME_SIZE = 64
CLASS_VENDOR = 0xFF
PATH_PREFIX = b"usb:"                       # the transport's .path: usb:<bus>:<address>
ENV_TRANSPORT = "K2_TRANSPORT"              # "hid", "usb" or unset/"auto"


class UsbTransportError(RuntimeError):
    """Raised for what k2_flasher.ProtocolError is on the HID side (the callers catch both as errors of the link)."""


def _load_pyusb():
    """(usb.core, usb.util), or raises ImportError. A seam: tests put a fake in its place."""
    import usb.core
    import usb.util
    return usb.core, usb.util


def _interfaces(dev):
    """Yield (config value, interface number, [endpoint addresses]) of every alt-0 interface of every configuration."""
    for cfg in dev:
        for itf in cfg:
            if getattr(itf, "bAlternateSetting", 0) != 0:
                continue
            yield cfg, itf


def vendor_interface(dev):
    """(interface number, bulk IN address, bulk OUT address) of the vendor-class interface of `dev`, or None."""
    for _cfg, itf in _interfaces(dev):
        if itf.bInterfaceClass != CLASS_VENDOR:
            continue
        ep_in = ep_out = None
        for ep in itf:
            bulk = (ep.bmAttributes & 0x03) == 0x02
            if not bulk:
                continue
            if ep.bEndpointAddress & 0x80:
                ep_in = ep_in if ep_in is not None else ep.bEndpointAddress
            else:
                ep_out = ep_out if ep_out is not None else ep.bEndpointAddress
        if ep_in is not None and ep_out is not None:
            return itf.bInterfaceNumber, ep_in, ep_out
    return None


def _path_of(dev):
    return PATH_PREFIX + f"{dev.bus}:{dev.address}".encode()


def _string(core_util, dev, index):
    try:
        return core_util.get_string(dev, index) if index else None
    except Exception:
        return None


def find_vendor_devices():
    """Every K2 that exposes the vendor interface, as dicts shaped like the HID enumeration (path, vendor_id, product_id,
    serial_number, product_string); [] when pyusb or libusb is missing. No traffic beyond the descriptor reads."""
    try:
        core, util = _load_pyusb()
    except ImportError:
        return []
    out = []
    try:
        devs = list(core.find(find_all=True, idVendor=VID, idProduct=PID))
    except Exception:
        return []
    for dev in devs:
        try:
            if vendor_interface(dev) is None:
                continue
        except Exception:
            continue
        out.append({"path": _path_of(dev), "vendor_id": VID, "product_id": PID,
                    "serial_number": _string(util, dev, getattr(dev, "iSerialNumber", 0)),
                    "manufacturer_string": _string(util, dev, getattr(dev, "iManufacturer", 0)),
                    "product_string": _string(util, dev, getattr(dev, "iProduct", 0))})
    return out


def preference():
    v = os.environ.get(ENV_TRANSPORT, "auto").strip().lower()
    return v if v in ("hid", "usb") else "auto"


def vendor_wanted(path=None):
    """True when the vendor pipe should carry this session: K2_TRANSPORT=usb, or auto and a K2 with the interface is
    attached. A HID device path (anything not "usb:...") always means HID."""
    pref = preference()
    if pref == "hid":
        return False
    if path is not None:
        p = path if isinstance(path, bytes) else str(path).encode()
        if not p.startswith(PATH_PREFIX):
            return False
    if pref == "usb":
        return True
    return bool(find_vendor_devices())


def _is_timeout(exc):
    return getattr(exc, "errno", None) in (errno.ETIMEDOUT, 110, -7) or "imeout" in type(exc).__name__


class UsbVendorTransport:
    """write(frame) / read(timeout_ms) / close() over the vendor bulk pair; `.path` like the HID transport."""

    def __init__(self, path=None, wait_sec=0.0):
        k2_lock.acquire()                    # every program that opens the K2 holds the device lock first
        self._locked = True
        self.dev = None
        self._rx = bytearray()
        try:
            self._open(path, wait_sec)
        except BaseException:
            self.close()
            raise

    def _pick(self, core, path):
        devs = []
        for dev in core.find(find_all=True, idVendor=VID, idProduct=PID):
            if vendor_interface(dev) is not None:
                devs.append(dev)
        if path is not None:
            want = path if isinstance(path, bytes) else str(path).encode()
            devs = [d for d in devs if _path_of(d) == want]
        return devs[0] if devs else None

    def _open(self, path, wait_sec):
        try:
            core, util = _load_pyusb()
        except ImportError as e:
            raise UsbTransportError("the vendor transport needs pyusb and libusb (pip install pyusb)") from e
        dev = self._pick(core, path)
        deadline = time.monotonic() + wait_sec
        while dev is None and time.monotonic() < deadline:
            time.sleep(0.1)
            dev = self._pick(core, path)
        if dev is None:
            raise UsbTransportError("No WITRN K2 with a vendor (WebUSB) interface detected (VID 0x0716, PID 0x5060).")
        self._util = util
        self._core = core
        try:
            dev.get_active_configuration()
        except Exception:
            dev.set_configuration()
        itf, self._ep_in, self._ep_out = vendor_interface(dev)
        try:
            if dev.is_kernel_driver_active(itf):
                dev.detach_kernel_driver(itf)
        except Exception:
            pass                             # no driver bound, or the platform has no such call
        try:
            util.claim_interface(dev, itf)
        except Exception as e:
            raise UsbTransportError(f"cannot claim the vendor interface ({e}); on Linux see the udev rule in "
                                    "tools/k2_usb.py") from e
        self.dev, self._itf, self.path = dev, itf, _path_of(dev)

    def _fail(self, what, exc):
        self.close()
        raise UsbTransportError(f"USB {what} failed: {exc}") from exc

    def write(self, frame: bytes) -> int:
        if len(frame) != FRAME_SIZE:
            raise UsbTransportError(f"Frame must be exactly {FRAME_SIZE} bytes")
        if self.dev is None:
            raise UsbTransportError("USB transport is closed")
        try:
            n = self.dev.write(self._ep_out, bytes(frame), 1000)
        except Exception as exc:
            self._fail("write", exc)
        if n != FRAME_SIZE:
            raise UsbTransportError(f"USB write failed (wrote {n} bytes)")
        return FRAME_SIZE

    def read(self, timeout_ms: int) -> bytes:
        if self.dev is None:
            raise UsbTransportError("USB transport is closed")
        deadline = time.monotonic() + max(timeout_ms, 1) / 1000.0
        first = True                         # at least one read, even for a poll (timeout 0)
        while len(self._rx) < FRAME_SIZE:
            left_ms = int((deadline - time.monotonic()) * 1000)
            if left_ms <= 0 and not first:
                return b""                   # a partial frame stays in _rx for the next call
            first = False
            try:
                chunk = self.dev.read(self._ep_in, FRAME_SIZE, max(left_ms, 1))
            except Exception as exc:
                if _is_timeout(exc):
                    return b""
                self._fail("read", exc)
            if len(chunk) == 0:
                continue                     # zero-length packet: the end of a transfer, not data
            self._rx += bytes(chunk)
        frame = bytes(self._rx[:FRAME_SIZE])
        del self._rx[:FRAME_SIZE]
        return frame

    def close(self):
        dev, self.dev = getattr(self, "dev", None), None
        if dev is not None:
            try:
                self._util.release_interface(dev, self._itf)
            except Exception:
                pass
            try:
                self._util.dispose_resources(dev)
            except Exception:
                pass
        if getattr(self, "_locked", False):
            self._locked = False
            k2_lock.release()
