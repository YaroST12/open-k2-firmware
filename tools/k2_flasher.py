#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Native Linux firmware installer and maintenance utility for WITRN K2 USB-PD meter.

Supports:
- Device probing (bootloader info, installed version, commit marker)
- Offline container inspection (.k2 metadata, vector table, checksums)
- Packaging raw bare-metal ARM binaries (.bin) into encrypted .k2 containers
- Full 2-pass flash memory backup & recovery container generation
- Safe progressive flash programming with full read-back verification
- Reset / reboot into application mode
"""

import argparse
import contextlib
from dataclasses import dataclass, replace
from datetime import date, datetime, timezone
from functools import reduce
import hashlib
import importlib
import json
import math
import os
from pathlib import Path
import struct
import sys
import time
import traceback
import uuid
import zipfile

import k2_lock
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import logdb  # noqa: E402  K2LOG msg#id lines -> text (tools/logdb.py)
import fw_version  # noqa: E402  the version string and its major.minor nibbles (tools/fw_version.py)

# --- Hardware & Protocol Constants ---
VID = 0x0716
PID = 0x5060
FRAME_SIZE = 64
PAYLOAD_SIZE = 52
WRITE_SIZE = 40

HEADER_SIZE = 48
FLASH_START = 0x08000000
APP_START = 0x08003800
INFO_START = 0x08003000
NAME_START = 0x08001400
SECTOR_SIZE = 2048
SECTOR_COUNT = 447
APP_LIMIT = APP_START + SECTOR_SIZE * SECTOR_COUNT   # 0x080E3000: end of what the bootloader is known to serve
FLASH_END = 0x08100000                               # end of the 1 MB flash
NVRAM_START = 0x080E0000                             # the native device storage (settings, e-marker records, assets)
READ_CHUNK = 1024
FS_START = 0x080B3000                                # our littlefs volume (link.ld FLASH ends here)
MAX_CONTAINER = NVRAM_START - APP_START              # erase runs in 2 KB sectors from APP_START over the container length
SIG_OFFSET = 2064                                    # 0x08004010: the model signature word the bootloader checks
K200 = b"K200"
MARKER_OFFSET = 3072  # 0x08003800 + 3072 = 0x08004400
COMMIT_MARKER = b"\xfe\xff\xff\xff"
# The full firmware version string (`1.0.0-435-26b245ac`): 48 bytes at the very start of .text, 0x08004404, "K2FW" then NUL
# padded text (src/core/version.cpp). It sits in code space, so the bootloader never looks at it; the one-byte
# major.minor word at 0x08004000 stays what the bootloader reads. Firmware from before the record has no "K2FW" there.
FWID_OFFSET = 3076
FWID_SIZE = 48
FWID_MAGIC = b"K2FW"
K2_GUID = uuid.UUID("5a3f8522-366b-46cc-aacc-dbfbe2fee02a")

# 256-byte S-box decoding table for .k2 container
FIRMWARE_DECODE_TABLE = bytes([
    0x1a, 0xb6, 0xb7, 0xf1, 0x7b, 0x8c, 0xd5, 0x3c, 0x7c, 0x90, 0xd2, 0xf4, 0x35, 0xf2, 0x1b, 0xe9,
    0xdb, 0x12, 0x77, 0x5b, 0x3d, 0x7f, 0x3f, 0xbd, 0x2d, 0x47, 0x2a, 0x50, 0xdc, 0xc3, 0xff, 0x57,
    0x82, 0x83, 0x60, 0xfc, 0x9b, 0x4e, 0x88, 0xd0, 0x00, 0x5f, 0x2e, 0x59, 0x2b, 0xf9, 0xc6, 0x38,
    0xf0, 0xc8, 0x8e, 0x80, 0x44, 0xfd, 0x73, 0x7e, 0x0b, 0x29, 0x19, 0xaf, 0xd8, 0x96, 0x71, 0xb3,
    0x62, 0xe6, 0xe5, 0x11, 0x91, 0x9c, 0x46, 0x15, 0xc4, 0xc2, 0xa5, 0x52, 0xb9, 0x66, 0xac, 0xb2,
    0x22, 0x53, 0xee, 0x54, 0xaa, 0xb4, 0x74, 0xcf, 0xb0, 0x6c, 0x72, 0xa7, 0x92, 0x5a, 0x56, 0xe7,
    0x85, 0xec, 0xc1, 0x9f, 0xce, 0x6e, 0xae, 0x3b, 0x78, 0x8f, 0x5d, 0x4b, 0xe2, 0x64, 0x0c, 0x4c,
    0x13, 0x63, 0xc0, 0x41, 0x4f, 0xba, 0xe4, 0x10, 0x04, 0xa6, 0x3e, 0x31, 0xe1, 0xb8, 0x48, 0x67,
    0x05, 0xd3, 0x98, 0x69, 0xbf, 0x36, 0x65, 0xbe, 0x06, 0x51, 0x5c, 0xc7, 0x8b, 0x09, 0x37, 0xc5,
    0xa9, 0xa8, 0x70, 0xd4, 0x87, 0x1d, 0xda, 0x0d, 0xf3, 0x76, 0x30, 0xad, 0x17, 0x94, 0xe0, 0xb1,
    0x14, 0xed, 0x0e, 0x08, 0x42, 0x9a, 0x33, 0xb5, 0x55, 0xd9, 0x45, 0x39, 0xe3, 0x7d, 0x4d, 0x01,
    0xcb, 0x27, 0xef, 0x25, 0x6a, 0x3a, 0x7a, 0x79, 0x6d, 0x16, 0x40, 0x61, 0xa4, 0x02, 0xab, 0x86,
    0x0f, 0x34, 0x6f, 0xeb, 0x6b, 0xf7, 0x28, 0xde, 0xa3, 0xcc, 0x75, 0x2c, 0xca, 0x5e, 0x81, 0x0a,
    0xfa, 0x68, 0xf8, 0x07, 0x03, 0x24, 0xcd, 0x1f, 0xa2, 0x2f, 0x32, 0x43, 0x9d, 0xf5, 0xd7, 0xbc,
    0x9e, 0xbb, 0x49, 0x4a, 0xd6, 0x8a, 0x84, 0xa1, 0x21, 0xdf, 0x97, 0xfe, 0x20, 0xd1, 0x1e, 0x93,
    0x1c, 0x8d, 0xfb, 0x26, 0x18, 0x95, 0xf6, 0xc9, 0xdd, 0xea, 0x99, 0x23, 0xe8, 0xa0, 0x58, 0x89
])
FIRMWARE_ENCODE_TABLE = bytes(FIRMWARE_DECODE_TABLE.index(i) for i in range(256))


class ProtocolError(RuntimeError):
    pass


class UpgradeError(RuntimeError):
    def __init__(self, stage, message):
        self.stage = stage
        super().__init__(f"[{stage}] {message}")


# --- HID Transport (Compatible with ctypes 'hid' & cython 'hidapi') ---
class LinuxHidTransport:
    def __init__(self, path=None, wait_sec=0.0):
        # every program that opens the K2 holds the device lock first (see tools/k2_lock.py);
        # released by close(), or here when the open fails
        k2_lock.acquire()
        self._locked = True
        try:
            self._open(path, wait_sec)
        except BaseException:
            self.close()
            raise

    def _open(self, path, wait_sec):
        hid_mod = None
        for name in ("hid", "hidapi"):
            try:
                hid_mod = importlib.import_module(name)
                break
            except ImportError:
                continue

        if not hid_mod:
            raise RuntimeError("Neither 'hid' nor 'hidapi' python package is available.")

        self.hid = hid_mod
        devices = self.enumerate_k2()
        if not devices and wait_sec > 0:
            start_wait = time.time()
            while (time.time() - start_wait) < wait_sec:
                time.sleep(0.1)
                devices = self.enumerate_k2()
                if devices:
                    break

        if not devices:
            raise RuntimeError("No WITRN K2 detected (VID 0x0716, PID 0x5060).")

        selected = None
        if path:
            selected_path = path.encode() if isinstance(path, str) else path
            for d in devices:
                if d.get("path") == selected_path:
                    selected = d
                    break
            if not selected:
                raise RuntimeError(f"Requested device path {path} not found.")
        else:
            selected = devices[0]

        self.path = selected["path"]
        self.device = None

        if hasattr(self.hid, "device"):
            try:
                dev = self.hid.device()
                dev.open_path(self.path)
                dev.set_nonblocking(0)
                self.device = dev
                self._mode = "cython"
            except Exception:
                self.device = None

        if self.device is None and hasattr(self.hid, "Device"):
            self.device = self.hid.Device(path=self.path)
            self._mode = "ctypes"

        if self.device is None:
            raise RuntimeError(f"Failed to open HID device at {self.path}")

    @classmethod
    def enumerate_k2(cls):
        hid_mod = None
        for name in ("hid", "hidapi"):
            try:
                hid_mod = importlib.import_module(name)
                break
            except ImportError:
                continue
        if not hid_mod:
            return []
        devs = hid_mod.enumerate(VID, PID)
        normalized = []
        for d in devs:
            if isinstance(d, dict):
                normalized.append(d)
            else:
                normalized.append({k: getattr(d, k, None) for k in (
                    "path", "vendor_id", "product_id", "serial_number",
                    "manufacturer_string", "product_string"
                )})
        return normalized

    def _fail(self, what, exc):
        """Any hid error other than a vanished device (e.g. HIDException "Success" or "Connection
        timed out") leaves the handle in an unknown state: close it so the next open is clean."""
        self.close()
        raise ProtocolError(f"HID {what} failed: {exc}") from exc

    def write(self, frame: bytes) -> int:
        if len(frame) != FRAME_SIZE:
            raise ProtocolError(f"Frame must be exactly {FRAME_SIZE} bytes")
        if self.device is None:
            raise ProtocolError("HID transport is closed")
        try:
            count = self.device.write(b"\x00" + frame)
        except Exception as exc:
            self._fail("write", exc)
        if count not in (FRAME_SIZE, FRAME_SIZE + 1):
            raise ProtocolError(f"HID write failed (wrote {count} bytes)")
        return FRAME_SIZE

    def read(self, timeout_ms: int) -> bytes:
        if self.device is None:
            raise ProtocolError("HID transport is closed")
        try:
            if self._mode == "cython":
                if timeout_ms == 0:
                    self.device.set_nonblocking(1)
                    try:
                        raw = self.device.read(FRAME_SIZE + 1)
                    finally:
                        self.device.set_nonblocking(0)
                else:
                    raw = self.device.read(FRAME_SIZE + 1, timeout_ms)
            else:
                if timeout_ms == 0:
                    self.device.nonblocking = True
                    try:
                        raw = self.device.read(FRAME_SIZE + 1)
                    finally:
                        self.device.nonblocking = False
                else:
                    raw = self.device.read(FRAME_SIZE + 1, timeout=timeout_ms)
        except OSError:
            return b""
        except Exception as exc:
            self._fail("read", exc)

        if not raw:
            return b""
        raw = bytes(raw)
        if len(raw) == FRAME_SIZE + 1 and raw[0] == 0:
            return raw[1:]
        if len(raw) == FRAME_SIZE:
            return raw
        raise ProtocolError(f"Unexpected HID frame length: {len(raw)} bytes")

    def close(self):
        if getattr(self, "device", None):
            try:
                self.device.close()
            except Exception:
                pass
            self.device = None
        if getattr(self, "_locked", False):
            self._locked = False
            k2_lock.release()


# --- Frame Encoding & Protocol ---
def build_frame(command: int, payload: bytes = b"", *, reply: bool = True, tick: int = None) -> bytes:
    if not (0 <= command <= 255) or len(payload) > PAYLOAD_SIZE:
        raise ValueError(f"Invalid command ({command}) or payload length ({len(payload)})")
    t = (int(time.monotonic() * 1000) if tick is None else tick) & 0xFFFFFFFF
    header = (
        0xFF, 0x55,
        t // 1000, t % 1000,
        ((t // 30) | 0x80) if reply else ((t // 30) & 0x7F),
        t // 100, t % 100, t // 80
    )
    frame = bytearray(x & 0xFF for x in header)
    frame.extend(bytes((command, len(payload))) + payload.ljust(PAYLOAD_SIZE, b"\x00"))
    frame.append(sum(frame[8:62]) & 0xFF)
    frame.append(sum(frame[:62]) & 0xFF)
    return bytes(frame)


def is_log_report(pkt: bytes) -> bool:
    """True for an IN report carrying log text. Remote-control responses (tools/k2_remote.py)
    start with 0xA6, bootloader frames with 0xFF: a high first byte is never ASCII log text."""
    return bool(pkt) and pkt[0] < 0x80 and any(pkt)


@dataclass(frozen=True)
class Packet:
    command: int
    payload: bytes
    raw: bytes


def parse_frame(raw: bytes) -> Packet:
    if len(raw) != FRAME_SIZE:
        raise ProtocolError(f"Frame length {len(raw)} != {FRAME_SIZE}")
    if raw[:2] != b"\xff\x55":
        raise ProtocolError("Invalid magic header (expected FF 55)")
    if raw[9] > PAYLOAD_SIZE:
        raise ProtocolError("Payload length exceeds 52")
    if raw[62] != (sum(raw[8:62]) & 0xFF):
        raise ProtocolError("Inner checksum mismatch")
    if raw[63] != (sum(raw[:62]) & 0xFF):
        raise ProtocolError("Outer checksum mismatch")
    return Packet(raw[8], raw[10:10 + raw[9]], raw)


class Protocol:
    def __init__(self, transport: LinuxHidTransport, timeout_ms: int = 2000):
        self.transport = transport
        self.timeout_ms = timeout_ms

    def drain(self):
        for _ in range(256):
            raw = self.transport.read(0)
            if not raw:
                return
            packet = parse_frame(raw)
            if packet.command == 1:
                raise ProtocolError("Device reported error before next request")
            if packet.command != 21:
                raise ProtocolError(f"Unexpected unconsumed packet 0x{packet.command:02X}")
        raise ProtocolError("Receive buffer continuously producing data")

    def _send(self, command: int, payload: bytes = b"", reply: bool = True):
        frame = build_frame(command, payload, reply=reply)
        self.transport.write(frame)

    def _receive(self, expected: int, deadline: float) -> bytes:
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise ProtocolError(f"Timed out waiting for command 0x{expected:02X}")
            raw = self.transport.read(max(1, math.ceil(remaining * 1000)))
            if not raw:
                raise ProtocolError(f"Timed out waiting for command 0x{expected:02X}")
            packet = parse_frame(raw)
            if packet.command == 1:
                raise ProtocolError(f"Device rejected request: {packet.payload.hex()}")
            if packet.command == 21:
                continue
            if packet.command != expected:
                raise ProtocolError(f"Received command 0x{packet.command:02X}, expected 0x{expected:02X}")
            return packet.payload

    def command(self, command: int, payload: bytes = b""):
        self.drain()
        self._send(command, payload)
        self._receive(2, time.monotonic() + self.timeout_ms / 1000.0)

    def erase_sector(self, address: int):
        self.drain()
        self._send(8, struct.pack("<I", address), reply=False)

    def write_memory(self, address: int, data: bytes):
        for offset in range(0, len(data), WRITE_SIZE):
            self.drain()
            chunk = data[offset:offset + WRITE_SIZE]
            self._send(9, struct.pack("<IB", address + offset, len(chunk)) + chunk, reply=False)

    def read_memory(self, address: int, size: int) -> bytes:
        if not (1 <= size <= 1024):
            raise ValueError("Read size must be between 1 and 1024 bytes")
        self.drain()
        self._send(11, struct.pack("<IH", address, size))
        deadline = time.monotonic() + self.timeout_ms / 1000.0
        result = bytearray()
        while len(result) < size:
            chunk = self._receive(11, deadline)
            expected = min(WRITE_SIZE, size - len(result))
            if len(chunk) != expected:
                raise ProtocolError(f"Read chunk length {len(chunk)} != expected {expected}")
            result.extend(chunk)
        return bytes(result)

    def read_marker(self, address: int) -> bytes:
        self.drain()
        self._send(10, struct.pack("<IB", address, 4))
        result = self._receive(10, time.monotonic() + self.timeout_ms / 1000.0)
        if len(result) != 4:
            raise ProtocolError("Marker read length error")
        return result


def fwid_text(record):
    """The version string of a firmware-id record (FWID_SIZE bytes read at FWID_OFFSET), or None when the record is
    not there (old firmware, erased flash) or does not hold printable text."""
    if record is None or len(record) < len(FWID_MAGIC) + 1 or bytes(record[:len(FWID_MAGIC)]) != FWID_MAGIC:
        return None
    body = bytes(record[len(FWID_MAGIC):FWID_SIZE]).split(b"\x00", 1)[0]
    if not body or not all(0x20 < c < 0x7F for c in body):
        return None
    return body.decode("ascii")


def version_label(ver_byte, record):
    """The version to show for an image: the full string of its firmware-id record, else major.minor from the
    bootloader's version byte (firmware from before the record)."""
    return fwid_text(record) or f"{ver_byte >> 4}.{ver_byte & 15}"


def upgrade_line(old, new):
    return f"Device upgraded from v{old} to v{new}"


def recovery_app(app: bytes):
    """The application a recovery container carries, and whether the model signature had to be put back.

    Entering the bootloader from the app (DFU_ENTER) clears the signature word "K200" at SIG_OFFSET so the
    bootloader stays in DFU; a backup taken after that holds 00000000 there and an image built from it fails the
    bootloader's boot check. A zeroed word in an image with a valid vector table is put back; any other value is
    refused, not guessed. The dump itself (app.bin, flash.bin) stays as read.
    """
    word = app[SIG_OFFSET:SIG_OFFSET + 4]
    if word == K200:
        return app, False
    if word != b"\x00" * 4:
        raise ValueError(
            f"Model signature at 0x{APP_START + SIG_OFFSET:08X} is {word.hex()}, neither K200 nor 00000000: "
            "no recovery image built"
        )
    sp, reset = struct.unpack_from("<II", app)
    if not (0x20000000 < sp <= 0x20040000 and sp % 4 == 0) or not (reset & 1) \
            or not (APP_START <= (reset & ~1) < APP_START + len(app)):
        raise ValueError(
            f"Model signature is zero but the vector table (SP 0x{sp:08X}, reset 0x{reset:08X}) is not an "
            "application's: no recovery image built"
        )
    return app[:SIG_OFFSET] + K200 + app[SIG_OFFSET + 4:], True


# --- Firmware Container Management ---
@dataclass(frozen=True)
class Firmware:
    source: bytes
    app: bytes
    date_str: str
    version: str
    model_guid: uuid.UUID

    @classmethod
    def parse(cls, raw: bytes):
        if len(raw) < HEADER_SIZE:
            raise ValueError("File is smaller than 48-byte header")
        if len(raw) > SECTOR_SIZE * SECTOR_COUNT:
            raise ValueError("File exceeds maximum K2 flash sector capacity")
        if len(raw) > MAX_CONTAINER:
            raise ValueError(
                f"File ({len(raw)} B) would be erased into the native data area at 0x{NVRAM_START:08X} "
                f"(limit {MAX_CONTAINER} B)"
            )

        decoded = raw.translate(FIRMWARE_DECODE_TABLE)
        if decoded[:7] != b"gzutapp":
            raise ValueError("Invalid firmware magic (expected 'gzutapp')")

        year, month, day = struct.unpack_from("<HBB", decoded, 7)
        try:
            rel_date = date(year, month, day)
        except ValueError as exc:
            raise ValueError("Invalid firmware release date") from exc

        model = uuid.UUID(bytes_le=decoded[20:36])
        if model != K2_GUID:
            raise ValueError(f"Firmware GUID {model} does not match K2 GUID ({K2_GUID})")

        size, xor_val, checksum = struct.unpack_from("<III", decoded, 36)
        app = decoded[HEADER_SIZE:]
        if size != len(app):
            raise ValueError(f"Declared app size {size} != actual length {len(app)}")
        if size < MARKER_OFFSET + 4:
            raise ValueError("App payload lacks version or commit marker area")

        if reduce(int.__xor__, app, 0) != xor_val:
            raise ValueError("Firmware XOR verification failed")
        if (sum(app) & 0xFFFFFFFF) != checksum:
            raise ValueError("Firmware additive checksum verification failed")

        if app[MARKER_OFFSET:MARKER_OFFSET + 4] != b"\xff\xff\xff\xff":
            raise ValueError("Firmware commit marker is not uncommitted (FFFFFFFF)")

        sp, reset = struct.unpack_from("<II", app)
        if not (0x20000000 < sp <= 0x20040000 and sp % 4 == 0):
            raise ValueError(f"Invalid Cortex-M stack pointer: 0x{sp:08X}")
        if not (reset & 1) or not (APP_START <= (reset & ~1) < APP_START + size):
            raise ValueError(f"Reset vector 0x{reset:08X} outside flash application range")

        ver_str = version_label(app[2048], app[FWID_OFFSET:FWID_OFFSET + FWID_SIZE])
        return cls(bytes(raw), app, rel_date.isoformat(), ver_str, model)

    @classmethod
    def load(cls, path: str):
        raw = Path(path).read_bytes()
        # If raw binary starting with Cortex-M vector table, auto-convert to container
        if len(raw) >= 8:
            sp, reset = struct.unpack_from("<II", raw)
            if (0x20000000 < sp <= 0x20040000) and (reset & 1) and (APP_START <= (reset & ~1)):
                return cls.from_app(raw)
        return cls.parse(raw)

    @classmethod
    def from_app(cls, app: bytes, release: date = None, version_str: str = "1.0"):
        buf = bytearray(app)
        # Pad binary with 0xFF up to at least 4KB to accommodate version byte & commit marker
        min_required = MARKER_OFFSET + 4
        if len(buf) < min_required:
            buf.extend(b"\xff" * (min_required - len(buf)))

        # Align to 4-byte boundary
        if len(buf) % 4 != 0:
            buf.extend(b"\xff" * (4 - (len(buf) % 4)))

        # Version byte at offset 2048
        # (version_str may be the full 1.0.0-435-26b245ac: only its major.minor fit the byte)
        if buf[2048] == 0xFF:
            major, minor = fw_version.major_minor(version_str)
            buf[2048] = (major << 4) | minor

        # Commit marker offset must be uncommitted (0xFFFFFFFF) in package
        buf[MARKER_OFFSET:MARKER_OFFSET + 4] = b"\xff\xff\xff\xff"

        # Trim trailing 0xFF beyond MARKER_OFFSET + 4
        trimmed_len = max(MARKER_OFFSET + 4, (len(buf.rstrip(b"\xff")) + 3) // 4 * 4)
        clean_app = bytes(buf[:trimmed_len])

        rel = release or date.today()
        header = (
            b"gzutapp" + struct.pack("<HBB", rel.year, rel.month, rel.day)
            + b"\xff" * 9 + K2_GUID.bytes_le
            + struct.pack("<III", len(clean_app), reduce(int.__xor__, clean_app, 0), sum(clean_app) & 0xFFFFFFFF)
        )
        encoded = (header + clean_app).translate(FIRMWARE_ENCODE_TABLE)
        return cls.parse(encoded)

    @property
    def erase_addresses(self):
        count = (len(self.source) + SECTOR_SIZE - 1) // SECTOR_SIZE
        return tuple(APP_START + i * SECTOR_SIZE for i in range(count))

    def chunks(self, size=1024):
        for offset in range(0, len(self.app), size):
            yield APP_START + offset, self.app[offset:offset + size]

    def summary(self):
        return {
            "model": "K2",
            "model_guid": str(self.model_guid),
            "version": self.version,
            "release_date": self.date_str,
            "container_size": len(self.source),
            "app_size": len(self.app),
            "container_sha256": hashlib.sha256(self.source).hexdigest(),
            "app_sha256": hashlib.sha256(self.app).hexdigest(),
            "app_start": f"0x{APP_START:08X}",
            "app_end_exclusive": f"0x{APP_START + len(self.app):08X}",
            "erase_sectors": len(self.erase_addresses),
            "erase_end_exclusive": f"0x{self.erase_addresses[-1] + SECTOR_SIZE:08X}",
            "commit_address": f"0x{APP_START + MARKER_OFFSET:08X}",
            "commit_marker": COMMIT_MARKER.hex()
        }


# --- Device Operations ---
@dataclass(frozen=True)
class Identity:
    boot_strings: tuple
    info_sha256: str
    current_version: str = None
    current_marker_hex: str = None

    @property
    def confirmed_k2(self):
        return any(s.upper() in ("K2", "WITRN K2", "K2(C)WITRN") for s in self.boot_strings)

    def summary(self):
        return {
            "boot_strings": list(self.boot_strings),
            "info_sha256": self.info_sha256,
            "confirmed_k2": self.confirmed_k2,
            "current_version": self.current_version,
            "current_marker": self.current_marker_hex
        }


class K2Updater:
    def __init__(self, protocol: Protocol, progress_cb=None):
        self.protocol = protocol
        self.progress = progress_cb or (lambda stage, cur, tot: None)
        self.stage = "idle"
        self.used = False

    def _stage(self, name, cur=0, tot=0):
        self.stage = name
        self.progress(name, cur, tot)

    def _identify(self) -> Identity:
        self._stage("handshake")
        self.protocol.command(3)  # Upgrade mode handshake
        self._stage("identify")
        info = self.protocol.read_memory(INFO_START, 1024)
        names = self.protocol.read_memory(NAME_START, 256)
        parts = names.split(b"\x00", 3)
        if len(parts) != 4:
            raise ProtocolError("Bootloader strings missing terminators")
        try:
            strings = tuple(p.decode("utf-8").strip() for p in parts[:3])
        except UnicodeDecodeError as exc:
            raise ProtocolError("Bootloader strings are not valid UTF-8") from exc

        ver_byte = self.protocol.read_memory(APP_START + 2048, 1)[0]
        marker = self.protocol.read_marker(APP_START + MARKER_OFFSET)
        record = self.protocol.read_memory(APP_START + FWID_OFFSET, FWID_SIZE)

        return Identity(
            boot_strings=strings,
            info_sha256=hashlib.sha256(info).hexdigest(),
            current_version=version_label(ver_byte, record),
            current_marker_hex=marker.hex()
        )

    def probe(self) -> Identity:
        self._stage("probe")
        return self._identify()

    def _read_above_app(self, address: int, size: int):
        """Read a chunk at or above APP_LIMIT, where the bootloader may refuse.

        Returns (data, complete). A failed read is repeated once (a timeout is not yet a refusal); if it
        fails again the chunk is bisected (4-byte steps) to find how much of it is readable. `data` is
        the readable prefix of the chunk, `complete` is False when the chunk was cut short.
        """
        for _ in range(2):
            try:
                return self.protocol.read_memory(address, size), True
            except ProtocolError:
                pass
        # the refusal may leave the bootloader out of step: start a fresh session (best effort)
        with contextlib.suppress(ProtocolError):
            self.protocol.command(3)
        lo, best = 0, b""
        hi = size                       # known unreadable
        try:                            # the very first word: refused at the chunk start costs one probe
            best = self.protocol.read_memory(address, 4)
            lo = 4
        except ProtocolError:
            return b"", False
        while hi - lo > 4:
            mid = lo + ((hi - lo) // 2 & ~3)
            try:
                best = self.protocol.read_memory(address, mid)
                lo = mid
            except ProtocolError:
                hi = mid
        return best, False

    def backup(self, dest_dir: Path, full: bool = True) -> dict:
        """Two-pass flash backup.

        Default (full=True): the whole 1 MB (0x08000000..0x08100000) is requested, including the native
        device storage from 0x080E0000. The bootloader is only known to serve up to APP_LIMIT: if it refuses
        above that, the readable part is kept, compared twice, and the manifest names the last readable
        address (complete=False, backup_kind "partial"). With full=False only 0x08000000..APP_LIMIT
        (bootloader and application area) is read (backup_kind "app-only"). A failure below APP_LIMIT is
        always an error. The files also go into one K2_BACKUP_<version>_<UTC time>.zip next to dest_dir.
        """
        if dest_dir.exists() and any(dest_dir.iterdir()):
            raise ValueError(f"Destination directory {dest_dir} already exists and is not empty")
        dest_dir.mkdir(parents=True, exist_ok=True)

        self._stage("probe")
        ident = self._identify()
        if not ident.confirmed_k2:
            raise ProtocolError(f"Connected hardware not confirmed as K2: {ident.boot_strings!r}")

        want_end = FLASH_END if full else APP_LIMIT
        total_size = want_end - FLASH_START
        try:
            partial = dest_dir / "flash.bin.partial"
            read_end = want_end
            refused_at = None
            self._stage("backup-read", 0, total_size)
            with partial.open("xb") as f:
                for offset in range(0, total_size, READ_CHUNK):
                    addr = FLASH_START + offset
                    n = min(READ_CHUNK, total_size - offset)
                    if addr < APP_LIMIT:
                        chunk, ok = self.protocol.read_memory(addr, n), True
                    else:
                        chunk, ok = self._read_above_app(addr, n)
                    f.write(chunk)
                    if not ok:
                        read_end = addr + len(chunk)
                        refused_at = read_end
                        break
                    self._stage("backup-read", offset + len(chunk), total_size)

            read_size = read_end - FLASH_START
            self._stage("backup-verify", 0, read_size)
            with partial.open("rb") as f:
                for offset in range(0, read_size, READ_CHUNK):
                    expected = f.read(min(READ_CHUNK, read_size - offset))
                    actual = self.protocol.read_memory(FLASH_START + offset, len(expected))
                    if actual != expected:
                        raise ProtocolError(f"Backup verification mismatch at 0x{FLASH_START + offset:08X}")
                    self._stage("backup-verify", offset + len(actual), read_size)

            full_flash = partial.read_bytes()
            partial.rename(dest_dir / "flash.bin")

            prefix = full_flash[:APP_START - FLASH_START]
            app = full_flash[APP_START - FLASH_START:APP_LIMIT - FLASH_START]
            if len(full_flash) > NVRAM_START - FLASH_START:
                (dest_dir / "native_nvram.bin").write_bytes(full_flash[NVRAM_START - FLASH_START:])
            ident = replace(
                ident,
                current_version=version_label(app[2048], app[FWID_OFFSET:FWID_OFFSET + FWID_SIZE]),
                current_marker_hex=app[MARKER_OFFSET:MARKER_OFFSET + 4].hex()
            )

            (dest_dir / "bootloader.bin").write_bytes(prefix)
            (dest_dir / "app.bin").write_bytes(app)

            self._stage("backup-recovery")
            # the recovery container carries the application code only, APP_START..FS_START (link.ld FLASH).
            # Above it sit user data: the littlefs volume (kept in app.bin) and the native device storage
            # (0x080E0000 up, kept in native_nvram.bin). Once the volume has been used its last block is not
            # 0xFF, so trimming trailing 0xFF would not shorten the image below NVRAM_START, and a container
            # that long erases into the native storage; one that ends at FS_START also leaves the volume intact.
            rec_app, sig_restored = recovery_app(app[:FS_START - APP_START])
            rec_fw = Firmware.from_app(rec_app)
            rec_path = dest_dir / f"K2_RECOVERY_{rec_fw.version}.k2"
            rec_path.write_bytes(rec_fw.source)

            manifest = {
                "timestamp_utc": datetime.now(timezone.utc).isoformat(),
                "identity": ident.summary(),
                "mode": "full" if full else "app",
                "requested_range": f"0x{FLASH_START:08X}-0x{want_end:08X}",
                "flash_range": f"0x{FLASH_START:08X}-0x{read_end:08X}",
                "complete": refused_at is None,
                "last_readable_address": f"0x{read_end - 1:08X}",
                "refused_at": None if refused_at is None else f"0x{refused_at:08X}",
                "flash_size": len(full_flash),
                "flash_sha256": hashlib.sha256(full_flash).hexdigest(),
                "app_sha256": hashlib.sha256(app).hexdigest(),
                "two_pass_verified": True,
                "recovery_file": str(rec_path.name),
                "recovery_range": f"0x{APP_START:08X}-0x{FS_START:08X}",
                "signature_restored": sig_restored,
                "backup_kind": "app-only" if not full else ("full" if refused_at is None else "partial"),
            }
            if len(full_flash) > NVRAM_START - FLASH_START:
                manifest["native_nvram_file"] = "native_nvram.bin"
                manifest["native_nvram_sha256"] = hashlib.sha256(full_flash[NVRAM_START - FLASH_START:]).hexdigest()
            (dest_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
            self._stage("backup-zip")
            write_backup_zip(dest_dir, manifest, rec_path.name)
            self._stage("backup-complete")
            return manifest
        except (OSError, ProtocolError, ValueError) as exc:
            raise UpgradeError(self.stage, str(exc)) from exc

    def flash(self, fw: Firmware, auto_reboot: bool = True) -> Identity:
        if self.used:
            raise UpgradeError(self.stage, "Session already consumed")
        self._stage("validate")
        checked = Firmware.parse(fw.source)
        if checked != fw:
            raise ValueError("Firmware memory object does not match container")

        self.used = True
        try:
            ident = self._identify()
            if not ident.confirmed_k2:
                raise ProtocolError(f"Connected hardware not confirmed as K2: {ident.boot_strings!r}")

            self._stage("log-reset")
            self.protocol.command(22)

            # 1. Progressive Erase
            total_sectors = len(checked.erase_addresses)
            self._stage("erase", 0, total_sectors)
            self.protocol.command(5)
            for idx, addr in enumerate(checked.erase_addresses, 1):
                self.protocol.erase_sector(addr)
                self._stage("erase", idx, total_sectors)
            self.protocol.command(4)

            # 2. Program Chunks
            total_bytes = len(checked.app)
            self._stage("write", 0, total_bytes)
            self.protocol.command(5)
            for addr, chunk in checked.chunks():
                self.protocol.write_memory(addr, chunk)
                self._stage("write", addr - APP_START + len(chunk), total_bytes)
            self.protocol.command(4)

            # 3. Read-back Verification
            self._stage("verify", 0, total_bytes)
            for addr, expected in checked.chunks():
                actual = self.protocol.read_memory(addr, len(expected))
                if actual != expected:
                    diff_idx = next(i for i, (a, b) in enumerate(zip(actual, expected)) if a != b)
                    raise ProtocolError(f"Verification mismatch at flash address 0x{addr + diff_idx:08X}")
                self._stage("verify", addr - APP_START + len(expected), total_bytes)

            # 4. Commit Marker
            self._stage("commit")
            self.protocol.command(5)
            self.protocol.write_memory(APP_START + MARKER_OFFSET, COMMIT_MARKER)
            marker_readback = self.protocol.read_marker(APP_START + MARKER_OFFSET)
            if marker_readback != COMMIT_MARKER:
                raise ProtocolError(f"Commit marker verification failed: got {marker_readback.hex()}")
            self.protocol.command(4)

            # 5. Reboot / Reset
            if auto_reboot:
                self._stage("reboot")
                self.protocol.command(23)

            self._stage("complete")
            return ident
        except (OSError, ProtocolError) as exc:
            raise UpgradeError(self.stage, str(exc)) from exc


# --- CLI Interface ---
class DfuEntryError(RuntimeError):
    """The K2 could not be brought into the bootloader."""


FALLBACK_HINT = "hold MINUS while plugging in USB, then flash"
REPLUG_HINT = ("replug the K2 without holding MINUS; it stays in DFU "
               "(or hold MINUS while plugging in USB, then flash)")


def ensure_dfu(env, timeout_s=10.0, poll_s=0.1, settle_s=0.5):
    """App -> bootloader state machine over an env with state()/request_dfu()/sleep()/now().

    state() is "app" (remote ping answers), "hung" (enumerated as the app but silent),
    "bootloader" or "none" (nothing enumerated).
    After DFU_ENTER the app detaches from USB and resets: the old device must DISAPPEAR and a new
    one appear, then `settle_s` passes before the caller sends the first frame. A bootloader that
    answers without a prior disappearance is the host's stale enumeration of the app and is not
    trusted (its USB stack never got a fresh enumeration).
    Returns "already-dfu" or "entered-dfu"; raises DfuEntryError otherwise.
    """
    st = env.state()
    if st == "bootloader":
        return "already-dfu"
    if st == "hung":
        raise DfuEntryError(f"the K2 enumerates as the app but does not answer: it is hung; {REPLUG_HINT}")
    if st != "app":
        raise DfuEntryError(f"no K2 found; {FALLBACK_HINT}")
    env.request_dfu()
    deadline = env.now() + timeout_s
    gone = False
    last = "app"
    while env.now() < deadline:
        env.sleep(poll_s)
        last = env.state()
        if last == "none":
            gone = True
        elif last == "bootloader":
            if gone:
                env.sleep(settle_s)
                return "entered-dfu"
            raise DfuEntryError(
                "the K2 never disconnected from USB, so the host still holds the app's enumeration; "
                f"{REPLUG_HINT}")
    if last == "hung" and not gone:
        raise DfuEntryError(
            "the K2 stopped answering after DFU_ENTER but never disconnected from USB: the firmware "
            f"hung before the USB detach (check the log for [FAULT]); {REPLUG_HINT}")
    if last == "app" and not gone:
        raise DfuEntryError(
            "the app is still answering after DFU_ENTER: the bootloader ignored the cleared K200 "
            "marker (next try: switch kDfuMarkerAddr to the commit-marker 0x08004400); "
            f"for now {FALLBACK_HINT}")
    raise DfuEntryError(
        f"the K2 did not enumerate as the bootloader within {timeout_s:g} s; {REPLUG_HINT}")


BOOT_PRODUCT, BOOT_SERIAL = "WITRN.K2", "20230727"   # bootloader USB strings
APP_PRODUCT = "Open-K2"                               # prefix of the app's "Open-K2 Logger"


def classify_enumeration(devs):
    """none / bootloader / app / unknown from USB strings alone (no traffic: a write to a
    freshly enumerated soft-reset bootloader times out)."""
    if not devs:
        return "none"
    for d in devs:
        prod, ser = d.get("product_string") or "", d.get("serial_number") or ""
        if BOOT_PRODUCT in prod or ser == BOOT_SERIAL:
            return "bootloader"
    for d in devs:
        if (d.get("product_string") or "").startswith(APP_PRODUCT):
            return "app"
    return "unknown"


def app_running_message(devs, command, auto_dfu):
    """Hint for bootloader-only commands issued while the app runs (USB strings, no traffic), else None."""
    if command not in ("probe", "backup", "flash") or auto_dfu:
        return None
    if classify_enumeration(devs) != "app":
        return None
    return (f"the app is running, so `{command}` cannot talk to the bootloader "
            "(use `flash --auto-dfu`, or `python3 tools/k2_remote.py dfu --confirm`)")


class RealDfuEnv:
    """ensure_dfu environment on real hardware. The USB strings classify the device; only an app
    (or an unclassifiable device) is pinged. Any transport error counts as an answer, never as a
    crash. `reset` is the DFU_ENTER reset mode (sys or iwdg). The *_fn hooks are test seams."""

    def __init__(self, path=None, ping_timeout_s=0.6, reset="sys",
                 enumerate_fn=None, ping_fn=None, request_fn=None):
        self.path, self.ping_timeout_s, self.reset = path, ping_timeout_s, reset
        self.enumerate_fn = enumerate_fn or LinuxHidTransport.enumerate_k2
        self.ping_fn = ping_fn or self._ping
        self.request_fn = request_fn or self._request

    def _call(self, cmd, payload=b"", timeout_s=None):
        import k2_remote  # lazy: k2_remote imports this module
        transport = LinuxHidTransport(self.path)
        try:
            link = k2_remote.RemoteLink(transport, self.ping_timeout_s)
            if timeout_s:
                link.timeout_s = timeout_s
            link.call(cmd, payload)
        finally:
            transport.close()

    def _ping(self):
        import k2_remote
        self._call(k2_remote.CMD_PING)

    def _request(self):
        import k2_remote
        self._call(k2_remote.CMD_DFU_ENTER, k2_remote.dfu_payload(self.reset), 2.0)

    def state(self):
        import k2_remote
        try:
            kind = classify_enumeration(self.enumerate_fn())
        except Exception:
            kind = "unknown"
        if kind in ("none", "bootloader"):
            return kind
        try:
            self.ping_fn()
            return "app"
        except (k2_remote.RemoteError, k2_remote.FrameError):
            return "app"
        except Exception:   # BootloaderDetected, RemoteTimeout, ProtocolError, OSError, ...
            # a device that enumerates as the app and stays silent is hung, not a bootloader
            return "hung" if kind == "app" else "bootloader"

    def request_dfu(self):
        import k2_remote
        try:
            self.request_fn()
        except Exception as e:
            if isinstance(e, DfuEntryError):
                raise
            raise DfuEntryError(f"DFU_ENTER was not accepted ({type(e).__name__}: {e}); "
                                f"{FALLBACK_HINT}") from e

    def sleep(self, s):
        time.sleep(s)

    def now(self):
        return time.monotonic()


def open_with_retry(open_fn, handshake_fn, delays=(0.5, 1.0, 2.0), sleep=time.sleep, report=print):
    """Open the bootloader and run its first handshake, retrying with a growing settle delay.

    A bootloader that came out of a soft reset may need longer than a power-on one before it
    answers. Every failed handle is closed before the next attempt (fresh enumeration each time).
    Returns the open transport; raises DfuEntryError listing every error.
    """
    errors = []
    for n, delay in enumerate(delays, 1):
        sleep(delay)
        transport = None
        try:
            transport = open_fn()
            handshake_fn(transport)
            report(f"  bootloader attempt {n}/{len(delays)} (after {delay:g} s): ok")
            return transport
        except Exception as exc:
            errors.append(f"attempt {n}: {type(exc).__name__}: {exc}")
            report(f"  bootloader attempt {n}/{len(delays)} (after {delay:g} s): "
                   f"{type(exc).__name__}: {exc}")
            if transport is not None:
                try:
                    transport.close()
                except Exception:
                    pass
    raise DfuEntryError(
        f"the bootloader did not answer the first handshake in {len(delays)} attempts ("
        + "; ".join(errors) + f"); {REPLUG_HINT}")


DEFAULT_LOG = Path(__file__).resolve().parent.parent / "build" / "k2_flasher.log"


def default_log_path():
    return Path(os.environ.get("K2_FLASHER_LOG", DEFAULT_LOG))


class _Tee:
    def __init__(self, console, fh):
        self.console, self.fh = console, fh

    def write(self, text):
        self.console.write(text)
        self.fh.write(text)
        self.fh.flush()
        return len(text)

    def flush(self):
        self.console.flush()
        self.fh.flush()


@contextlib.contextmanager
def tee_output(path, console):
    """Mirror everything written to `console` into the log file `path` (append, flushed per write)
    so a truncated terminal never hides the failure."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as fh:
        fh.write(f"\n=== k2_flasher {datetime.now().isoformat(timespec='seconds')} {' '.join(sys.argv[1:])} ===\n")
        yield _Tee(console, fh)


def log_exception(path, exc):
    with Path(path).open("a", encoding="utf-8") as fh:
        fh.write("".join(traceback.format_exception(type(exc), exc, exc.__traceback__)))


def format_progress(stage, cur, tot):
    if tot > 0:
        pct = (cur / tot) * 100
        print(f"\r  [{stage.upper():14s}] {cur:6d}/{tot:6d} ({pct:5.1f}%)", end="", flush=True)
        if cur >= tot:
            print()
    else:
        print(f"  [{stage.upper():14s}] ...")


def backup_zip_path(dest_dir, manifest):
    """K2_BACKUP_<version>_<UTC time>.zip next to the backup directory, named from the manifest."""
    stamp = datetime.fromisoformat(manifest["timestamp_utc"]).astimezone(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    version = manifest["identity"]["current_version"]
    return Path(dest_dir).parent / f"K2_BACKUP_{version}_{stamp}.zip"


def write_backup_zip(dest_dir, manifest, recovery_name):
    """Pack the six backup files (recovery .k2, flash.bin, app.bin, bootloader.bin, native_nvram.bin when
    present, manifest.json) into one store-only ZIP; returns its path."""
    dest_dir = Path(dest_dir)
    names = [recovery_name, "flash.bin", "app.bin", "bootloader.bin", "native_nvram.bin", "manifest.json"]
    path = backup_zip_path(dest_dir, manifest)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
        for name in names:
            if (dest_dir / name).exists():
                z.write(dest_dir / name, name)
    return path


def default_backup_dir(now=None):
    """runs/k2-backup-<UTC time> under the repository root (runs/ is not tracked)."""
    now = now or datetime.now(timezone.utc)
    return DEFAULT_LOG.parent.parent / "runs" / f"k2-backup-{now.strftime('%Y%m%dT%H%M%SZ')}"


def backup_exit_code(manifest):
    """0 for a complete backup, 3 when the bootloader refused part of the requested range."""
    return 0 if manifest.get("complete", True) else 3


def backup_summary(manifest):
    kind = manifest.get("backup_kind")
    if kind == "app-only":
        return (
            "\nAPPLICATION AREA ONLY backup (--app-only): 0x08000000..0x080E3000 was read twice and compared.\n"
            "The device storage from 0x080E3000 up has NOT been saved."
        )
    if manifest.get("complete", True):
        return "\nFull backup completed: the whole 1 MB was read twice and compared." if kind == "full" \
            else "\nBackup completed successfully!"
    return (
        "\nPARTIAL backup: the device storage part is missing. The bootloader refused the read at "
        f"{manifest['refused_at']}; the last readable address is {manifest['last_readable_address']}.\n"
        "Everything it served was read twice and compared. The rest of the native area has NOT been saved.\n"
        "Next: flash our app (it never erases or programs above 0x080E0000 except its own old pages), then run\n"
        "  python3 tools/k2_remote.py flash-dump <outdir>\n"
        "to read 0x080E0000..0x08100000 from the running app, and keep both dumps outside the repo."
    )


def flash_with_backup(updater, fw, backup_dir, no_backup=False, auto_reboot=True, out=None):
    """Step 0 of every flash: a full backup into backup_dir, then the flash on the same session.

    A failed backup stops here, before anything is erased. A partial or app-only backup is reported and the
    flash goes on (the flasher never writes above the application area). no_backup=True skips the backup.
    """
    out = out if out is not None else sys.stdout
    if no_backup:
        print("WARNING: flashing without a backup. The flash erases the firmware that is on this K2 now, "
              "and without a backup it cannot be brought back.", file=out)
    else:
        print(f"Backing up this K2 first (full backup) -> {backup_dir}", file=out)
        try:
            manifest = updater.backup(Path(backup_dir), full=True)
        except (UpgradeError, ProtocolError, ValueError) as exc:
            stage = getattr(exc, "stage", "backup")
            raise UpgradeError(stage, f"the backup comes first and it failed, so nothing was erased or "
                                      f"written: {exc}") from exc
        print(backup_summary(manifest), file=out)
        if manifest.get("backup_kind") != "full":
            print("Flashing goes on: the device storage part of the backup is missing.", file=out)
        print(f"Backup ZIP: {backup_zip_path(backup_dir, manifest)}\n", file=out)
    return updater.flash(fw, auto_reboot=auto_reboot)


def build_parser():
    parser = argparse.ArgumentParser(description="WITRN K2 Native Linux Firmware Utility")
    subparsers = parser.add_subparsers(dest="command", required=True)

    # probe
    p_probe = subparsers.add_parser("probe", help="Probe connected K2 in DFU mode")
    p_probe.add_argument("--path", help="Target device path (e.g. /dev/hidraw8)")

    # inspect
    p_inspect = subparsers.add_parser("inspect", help="Inspect local .k2 firmware package")
    p_inspect.add_argument("file", help="Path to .k2 firmware file")

    # pack
    p_pack = subparsers.add_parser("pack", help="Pack raw application binary (.bin) into .k2 container")
    p_pack.add_argument("bin_file", help="Path to raw application .bin")
    p_pack.add_argument("k2_file", help="Destination path for .k2 container")
    p_pack.add_argument("--version", default="1.0",
                        help="Firmware version, e.g. 1.0.0-435-26b245ac (default: 1.0); only its major.minor go into the "
                             "bootloader's version byte, and only when the image left that byte erased. The full string "
                             "the package shows is the firmware's own record (see FWID_OFFSET)")

    # backup
    p_backup = subparsers.add_parser(
        "backup", help="Full 2-pass flash backup (the whole 1 MB), files plus one K2_BACKUP_*.zip next to outdir")
    p_backup.add_argument("outdir", help="Output directory to store backup files")
    kind = p_backup.add_mutually_exclusive_group()
    kind.add_argument("--full", action="store_true",
                      help="no-op: the backup is full by default (the whole 1 MB, 0x08000000..0x08100000, "
                           "including the native device storage from 0x080E0000); a refusal above 0x080E3000 "
                           "ends the dump cleanly (exit code 3, last readable address in manifest.json)")
    kind.add_argument("--app-only", action="store_true",
                      help="read the application area only (0x08000000..0x080E3000); the device storage "
                           "above it is NOT saved")
    p_backup.add_argument("--path", help="Target device path")

    # flash
    p_flash = subparsers.add_parser("flash", help="Flash .k2 or .bin firmware onto K2")
    p_flash.add_argument("file", help="Path to .k2 or .bin firmware file")
    p_flash.add_argument("--path", help="Target device path")
    p_flash.add_argument("--no-backup", action="store_true",
                         help="skip the full backup that every flash makes first (the flash erases the "
                              "firmware that is on the K2 now)")
    p_flash.add_argument("--backup-dir", help="where the pre-flash backup goes (default: runs/k2-backup-<UTC time>); "
                                              "the K2_BACKUP_*.zip lands next to it")
    p_flash.add_argument("--no-reboot", action="store_true", help="Do not reboot device after commit")
    p_flash.add_argument("--monitor", action="store_true", help="Immediately monitor device logs after flashing")
    p_flash.add_argument("--timeout", type=float, default=0, help="Stop monitoring after N seconds (0 for indefinite)")
    p_flash.add_argument("--auto-dfu", action="store_true",
                         help="If the app is running, ask it to enter the bootloader first (no button needed)")
    p_flash.add_argument("--dfu-reset", choices=["sys", "iwdg"], default="sys",
                         help="With --auto-dfu: how the app resets into the bootloader (default sys)")

    # reboot
    p_reboot = subparsers.add_parser("reboot", help="Reboot K2 out of DFU mode into meter mode")
    p_reboot.add_argument("--path", help="Target device path")

    # monitor
    p_mon = subparsers.add_parser("monitor", help="Stream real-time log messages from Open-K2")
    p_mon.add_argument("--path", help="Target device path")
    p_mon.add_argument("--timeout", type=float, default=0, help="Stop after N seconds (0 for indefinite)")

    return parser


def main():
    args = build_parser().parse_args()
    if args.command in ("inspect", "pack"):
        return run(args, None)
    log = default_log_path()
    with tee_output(log, sys.stdout) as out, tee_output(log, sys.stderr) as err, \
            contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        print(f"(full output also in {log})")
        try:
            # the lock spans the whole command: flash --auto-dfu keeps it across the app ->
            # bootloader -> app re-enumeration, not per USB call
            with k2_lock.hold():
                return run(args, log)
        except k2_lock.DeviceLockTimeout as e:
            print(f"K2 busy: {e}", file=sys.stderr)
            sys.exit(1)
        except Exception:
            traceback.print_exc()   # stderr is tee'd into the log
            sys.exit(1)


def run(args, log):
    if args.command == "inspect":
        try:
            fw = Firmware.load(args.file)
            print(json.dumps(fw.summary(), indent=2))
        except Exception as e:
            print(f"Inspection error: {e}", file=sys.stderr)
            sys.exit(1)
        return

    if args.command == "pack":
        try:
            raw = Path(args.bin_file).read_bytes()
            fw = Firmware.from_app(raw, version_str=args.version)
            Path(args.k2_file).write_bytes(fw.source)
            print(f"Successfully packaged {len(raw)} bytes -> {args.k2_file} (v{fw.version})")
            print(json.dumps(fw.summary(), indent=2))
        except Exception as e:
            print(f"Packaging error: {e}", file=sys.stderr)
            sys.exit(1)
        return

    if args.command in ("probe", "backup", "flash") and not (args.command == "flash" and args.auto_dfu):
        try:
            msg = app_running_message(LinuxHidTransport.enumerate_k2(), args.command, False)
        except Exception:
            msg = None
        if msg:
            print(msg, file=sys.stderr)
            sys.exit(1)

    transport = None
    if args.command == "flash" and args.auto_dfu:
        try:
            print("Checking whether the K2 runs the app or the bootloader...")
            how = ensure_dfu(RealDfuEnv(args.path, reset=args.dfu_reset))
            print("K2 is already in DFU mode." if how == "already-dfu"
                  else "K2 entered the bootloader (marker cleared by the app).")
            # first handshake with retries: a soft-reset bootloader can be slow to answer
            transport = open_with_retry(lambda: LinuxHidTransport(args.path, wait_sec=5.0),
                                        lambda t: Protocol(t).command(3))
        except DfuEntryError as e:
            print(f"Auto-DFU failed: {e}", file=sys.stderr)
            sys.exit(1)

    # All hardware commands require HID transport
    try:
        wait_sec = 5.0 if args.command == "monitor" else 0.0
        if transport is None:
            transport = LinuxHidTransport(args.path, wait_sec=wait_sec)
    except Exception as e:
        print(f"Device connection error: {e}", file=sys.stderr)
        sys.exit(1)

    try:
        proto = Protocol(transport)
        updater = K2Updater(proto, progress_cb=format_progress)

        if args.command == "probe":
            print(f"Connected to K2 at {transport.path.decode('latin1', errors='replace')}")
            ident = updater.probe()
            print(json.dumps(ident.summary(), indent=2))

        elif args.command == "backup":
            print(f"Starting 2-pass backup from K2 at {transport.path.decode('latin1', errors='replace')} -> {args.outdir}")
            manifest = updater.backup(Path(args.outdir), full=not args.app_only)
            print(backup_summary(manifest))
            print(f"Backup ZIP: {backup_zip_path(Path(args.outdir), manifest)}")
            print(json.dumps(manifest, indent=2))
            if backup_exit_code(manifest):
                sys.exit(backup_exit_code(manifest))

        elif args.command == "flash":
            print(f"Loading firmware: {args.file}")
            fw = Firmware.load(args.file)
            print(f"Package: v{fw.version} (released {fw.date_str}), {len(fw.app)} bytes, {len(fw.erase_addresses)} sectors")
            print(f"Programming K2 at {transport.path.decode('latin1', errors='replace')}...")
            ident = flash_with_backup(
                updater, fw, Path(args.backup_dir) if args.backup_dir else default_backup_dir(),
                no_backup=args.no_backup, auto_reboot=not args.no_reboot)
            print(f"\nFlashing completed and verified! {upgrade_line(ident.current_version, fw.version)}")

            if args.monitor and not args.no_reboot:
                transport.close()
                print("\nWaiting for K2 to boot into Open-K2...")
                time.sleep(1.5)
                mon_transport = LinuxHidTransport(args.path, wait_sec=5.0)
                try:
                    print(f"Monitoring Open-K2 logs on {mon_transport.path.decode('latin1', errors='replace')} (Ctrl+C to stop)...\n---")
                    start_t = time.time()
                    while True:
                        if args.timeout > 0 and (time.time() - start_t) > args.timeout:
                            break
                        pkt = mon_transport.read(timeout_ms=200)
                        if pkt and is_log_report(pkt):
                            text = logdb.decode_text(bytes(pkt).rstrip(b'\x00').decode('utf-8', errors='replace'))
                            if text:
                                print(text, end='', flush=True)
                finally:
                    mon_transport.close()

        elif args.command == "reboot":
            print(f"Sending reboot command to K2 at {transport.path.decode('latin1', errors='replace')}...")
            proto.command(23)
            print("Device rebooted.")

        elif args.command == "monitor":
            print(f"Monitoring Open-K2 logs on {transport.path.decode('latin1', errors='replace')} (Ctrl+C to stop)...\n---")
            start_t = time.time()
            try:
                while True:
                    if args.timeout > 0 and (time.time() - start_t) > args.timeout:
                        break
                    pkt = transport.read(timeout_ms=200)
                    if pkt and is_log_report(pkt):
                        text = logdb.decode_text(bytes(pkt).rstrip(b'\x00').decode('utf-8', errors='replace'))
                        if text:
                            print(text, end='', flush=True)
            except KeyboardInterrupt:
                print("\nMonitoring stopped.")

    except Exception as e:
        print(f"\nOperation failed: {e}", file=sys.stderr)
        if log:
            log_exception(log, e)
        sys.exit(1)
    finally:
        transport.close()


if __name__ == "__main__":
    main()
