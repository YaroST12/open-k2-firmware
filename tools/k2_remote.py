#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Remote control of an Open-K2 over the USB HID interface.

Protocol: include/k2/core/remote_proto.hpp (requests A5 4B, responses A6 4B, 64-byte reports).
Device discovery and the HID transport come from tools/k2_flasher.py.
"""
import argparse
import json
import struct
import sys
import time
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import k2_flasher  # noqa: E402
import k2_lock  # noqa: E402
import charger_summary  # noqa: E402
import step_label  # noqa: E402
import report_card  # noqa: E402
from k2_flasher import FRAME_SIZE, LinuxHidTransport  # noqa: E402

REQ_MAGIC = b"\xA5\x4B"
RSP_MAGIC = b"\xA6\x4B"
REQ_HEADER, RSP_HEADER = 5, 6
MAX_REQ_PAYLOAD = FRAME_SIZE - REQ_HEADER

CMD_PING, CMD_BUTTON, CMD_GET_STATE, CMD_GET_PDOS, CMD_GET_EMARKER = 1, 2, 3, 4, 5
CMD_ACTION, CMD_SET_PAGE, CMD_REBOOT, CMD_LOG = 6, 7, 8, 9
CMD_SCREENSHOT = 10
CMD_CHARGER_TEST = 0x0B  # read-only RAM suite metadata / paged results
CMD_CHARGER_UNITS = 0x0C  # [n, unit...] the test units of the next run, in order (n = 0: default order)
CMD_CHARGER_CABLE = 0x0D  # [kind, seq(2)] the cable identity of the next START (no payload reads it back)
CMD_CHARGER_HUNT = 0x0E   # [slot] read-only: the evidence of one refused 6 A hunt row
CMD_GET_IDENTITY = 0x24  # source identity: VID/PID/bcd, SVIDs, vendor, Apple model
CMD_DEBUG_LINK = 0x25     # debug: 1 hold the link attached, 0 detached (until the next rescan)
CMD_DEBUG_PD_MSG = 0x23   # debug: feed one PD message (sop, header, data objects) to the parser
CMD_DEBUG_CC_MATRIX = 0x26  # debug: set the CC switch matrix PC13/14/15
CMD_DEBUG_VCONN = 0x27      # debug: VCONN_Swap experiment [op] 0 status, 1 swap, 2 off
CMD_DEBUG_EPR_INFO = 0x28   # debug: [op] 0 status, 1 ask EPR_Get_Source_Cap (gated), 2 ask regardless of the EPR bit
CMD_DEBUG_LCD = 0x29        # debug: LCD DMA state -> [flags, transfers u32, stalls u32]
CMD_DEBUG_CC_PROBE = 0x2A   # debug: [sw0, pc, ms u16 (, modes u16)] hold a SWITCHES0 value on a waiting charger, sample CC
CMD_DEBUG_PDOS = 0x20    # debug: load N synthetic PDOs into the PD state (0 = clear)
CMD_ADC_RAW = 0x21        # raw ADC dump (10 channels + the 7 scan slots)
CMD_IDENTIFY_DEVICE = 0x40  # [op] 0 status, 1 start, 2 start + confirm
CMD_PD_REQUEST = 0x41       # [pos, flags (bit0 confirm, bit1 over-load lab-only), mv u16, ma u16] -> [pd::Err]
CMD_PD_RESET = 0x42         # hard reset
CMD_PD_TRIGGER_STATUS = 0x43
CMD_PD_MATCH = 0x45  # passive wants-vs-offers verdict
CMD_USB_LINK = 0x46  # USB / Thunderbolt link from sniffed PD
CMD_PD_REREAD = 0x47  # [op, mask] source-cap re-read (PD mode): 0 status, 1 start + mask, 2 abort, 5 Ra mode on/off
CMD_PD_RELEASE = 0x44       # back to 5 V
CMD_PD_ATTACH = 0x48       # [target_mv u16] [target_ma u16] -> trigger snapshot; default 9000 mV
CMD_DFU_ENTER = 0x30      # clear the app marker and reset into the bootloader
CMD_DEBUG_FAULT = 0x31    # debug: provoke a CPU fault to exercise the [FAULT] handler
FAULT_MAGIC = b"BOOM"
FAULT_KINDS = {"udf": 0, "read": 1, "stack": 2}   # undefined instruction / read of SRAM above the first 96 KB / recursion into the stack guard
DFU_MAGIC = b"DFU!"       # confirm payload (u32 0x21554644 little endian)
CMD_FACTORY_WIPE = 0x33   # erase the file system + our own legacy pages, clear our backup registers, then DFU
FACTORY_WIPE_MAGIC = b"WIPE"   # confirm payload (u32 0x45504957 little endian)
WIPE_TIMEOUT_S = 30.0     # the sector erases block the firmware for several seconds before it replies
CMD_FLASH_DUMP = 0x34     # read-only window into the native device storage
FLASH_DUMP_BASE, FLASH_DUMP_END, FLASH_DUMP_CHUNK = 0x080E0000, 0x08100000, 48
CMD_BOOT_INFO = 0x35      # how this boot decided its mode: the K2B1 text line and the first-seconds power samples (RAM only)
BOOT_INFO_TEXT, BOOT_INFO_SAMPLES = 0, 1
WIPE_ERRS = ["ok", "erase failed", "verify failed"]   # k2::factory_wipe::Err
FLASH_STATUS = ["Ok", "Refused", "Busy", "KeyFail", "ProgramError", "WriteProtect", "VerifyFailed"]   # k2::flash::Status
CMD_FC = 0x32             # active fast-charge trigger (scan / trigger / reset / status)
CMD_EM_LIB = 0x61         # saved-cable library: list/get/save/delete/rename/spoof
CMD_CARDS = 0x62          # charger report cards: list / get (chunked) / rename / delete
CMD_STORE = 0x60          # flash store: list / save-emarker / save-log
CMD_BLACK_BOX = 0x70       # PD black box ring: read from a cursor / clear / freeze / status (tools/pd_trace.py)
BB_READ, BB_CLEAR, BB_FREEZE, BB_STATUS, BB_CAP_LIST, BB_CAP_READ, BB_CAP_CLEAR = 0, 1, 2, 3, 4, 5, 6
CMD_TRANS_CAP = 0x71      # transition capture: VBUS + CC at ~1 kHz around a PD step (tools/pd_trace.py transition)
TC_STATUS, TC_ARM, TC_TRIGGER, TC_DISARM, TC_SAVE, TC_LIVE, TC_LIST, TC_STORED, TC_CLEAR = range(9)
CMD_LAB = 0x51            # lab probes: PA8 load pulse
CMD_CABLE_R = 0x50        # passive cable resistance (two-point method), status / reset
CMD_UFCS = 0x52           # UFCS frame/timing tuner (RAM only) and the 5 V handshake + opener probe (tools/ufcs_sweep.py)
CMD_QRTEST = 0x53         # QR scan-test rig (k2/ui/ui_qrtest.hpp): fit table / full-screen test QR / stop; draws only
QRTEST_LIST, QRTEST_SHOW, QRTEST_STOP = 0, 1, 2
QRTEST_CHUNK = 48
UFCS_STATUS, UFCS_GET, UFCS_SET, UFCS_RESET, UFCS_PROBE, UFCS_STOP, UFCS_READ = range(7)
UFCS_CHUNK = 48
# Same order and ranges as k2::ufcs::kFields (include/k2/fastcharge/ufcs_tune.hpp); tests/test_k2_remote_ufcs.py checks the sync.
UFCS_FIELDS = [
    ("addr", 0, 7, 1), ("version", 0, 63, 1), ("ctl", 0, 7, 0), ("data", 0, 7, 1), ("opener", 0, 255, 0),
    ("keepalive", 0, 255, 0), ("retry", 1, 1000, 20), ("listen", 50, 5000, 500), ("crcsync", 0, 1, 0),
    ("crcinit", 0, 255, 0), ("crcpoly", 1, 255, 0x29), ("crclsb", 0, 1, 0), ("stop", 1, 2, 1),
    ("baud", 1200, 230400, 115200), ("delay", 0, 200, 10), ("dm", 0, 2, 0), ("dp", 0, 1, 0),
]
UFCS_STATES = ["idle", "running", "done"]
UFCS_VERDICTS = ["none", "no-handshake", "silent", "garbage", "answered", "overvoltage", "source-lost", "inline", "stopped"]
UFCS_CLASSES = ["none", "mirror", "delayed"]
UFCS_STARTS = ["ok", "busy", "inline", "not-5v", "no-monitor", "resetting"]
CABLE_OP_STATUS, CABLE_OP_RESET, CABLE_OP_PASSIVE, CABLE_OP_KELVIN = 0, 1, 2, 3
CABLE_GRADES = ["waiting", "excellent", "good", "fair", "poor"]
CMD_RIPPLE = 0x22         # ripple capture on PA1, samples follow as stream frames
CMD_SCREEN_DATA = 0x8A          # stream frames of a screenshot
CMD_RIPPLE_DATA = 0x8B          # stream frames of a ripple capture: offset(2) + u16 samples
SHOT_FLAG_LAST, SHOT_FLAG_ABORT = 0x01, 0x80
SHOT_FORMAT_RLE565 = 1

STATUS_OK, STATUS_UNKNOWN_CMD, STATUS_BAD_LENGTH, STATUS_BAD_ARG, STATUS_BUSY, STATUS_NEED_CONFIRM = 0, 1, 2, 3, 4, 5
STATUS_NAMES = {0: "ok", 1: "unknown command", 2: "bad length", 3: "bad argument", 4: "busy",
                5: "needs --confirm: a live sink is attached and this may interrupt charging"}

BUTTONS = {"back": 0, "minus": 1, "ok": 2, "plus": 3}
ACTIONS = {"pd-rescan": 1, "emarker": 2, "energy-reset": 3, "identify": 6}
SOPS = {"sop": 0, "sop1": 1, "sop2": 2}
# Rotation pages (settable with `page`), then Tools/Settings submenus: the state reports a
# showing submenu as 6 + its index. Connection and Diag can also be opened remotely.
PAGES = ["home", "detail", "chargertest", "lines", "tools", "settings", "trigger", "emarker", "pdtrigger", "cabler", "blackbox", "spoof", "usblink", "autorotate", "cards", "share", "connection", "diag", "qrtest"]
ROTATION_PAGES = 6
# Names the old numbering used, still accepted by `page`: they open Connection on the matching tab. The firmware
# reads kConnTabAlias + tab (include/k2/ui/ui_types.hpp) as "open Connection on that tab"; the state itself reports a
# showing Connection as its menu index (16), whatever the tab.
CONN_ALIAS_BASE = len(PAGES)   # kConnTabAlias = kMenuPageBase + Menu::Count
CONN_TABS = ["source", "cable", "sink"]
CONN_ALIASES = {"source": CONN_ALIAS_BASE, "pd": CONN_ALIAS_BASE,
                "cable": CONN_ALIAS_BASE + 1, "emarker": CONN_ALIAS_BASE + 1,
                "sink": CONN_ALIAS_BASE + 2, "identify": CONN_ALIAS_BASE + 2}
# Menu pages `page` can open directly (the firmware accepts kMenuPageBase + Menu for these two).
OPENABLE_MENUS = ("connection", "diag")


def page_name(i):
    """GET_STATE.page: 0..5 rotation pages, 6 + Menu while a submenu is open."""
    if i < ROTATION_PAGES:
        return PAGES[i]
    if i < len(PAGES):
        return ("settings/" if PAGES[i] in ("usblink", "autorotate") else "tools/") + PAGES[i]
    if CONN_ALIAS_BASE <= i < CONN_ALIAS_BASE + len(CONN_TABS):
        return "tools/connection/" + CONN_TABS[i - CONN_ALIAS_BASE]
    return str(i)


# include/k2/pd/pd_sink.hpp / pd_identify.hpp enums, in order
PD_ERRS = ["ok", "no source caps", "bad PDO position", "over the voltage cap", "voltage outside the PDO range",
           "inline: a live sink is attached", "busy (identify running)"]
SINK_STATES = ["idle", "epr-entering", "epr-wait-caps", "requesting", "transition", "ready", "aborting", "failed"]
SINK_RESULTS = ["none", "ok", "rejected", "wait-exhausted", "timeout", "tx-failed", "hard-reset", "soft-reset",
                "aborted", "over-cap", "epr-failed", "bad-choice"]
# Step.detail of an EPR skip (charger_test.hpp kDetailEprFail / kDetailMonitor*): above 128 + pd::Err, so no engine table names them.
EPR_DETAILS = {136: "EPR: entry refused", 137: "EPR: source rejects cable", 138: "EPR: source has no VCONN",
               139: "EPR: refused (RDO)", 140: "EPR: source unable now", 141: "EPR: 5V PDO lacks EPR bit",
               144: "EPR: above K2's 36V monitor", 145: "EPR: above K2's 48V monitor", 146: "EPR: above the K2's monitor",
               147: "EPR offered: needs EPR cable", 148: "EPR list withheld: cable-gated",
               149: "EPR offered; entry: refused", 150: "EPR offered; entry: cable rejected",
               151: "EPR offered; entry: no VCONN", 152: "EPR offered; entry: refused (RDO)",
               153: "EPR offered; entry: unable now", 154: "EPR offered; entry: 5V PDO lacks EPR bit",
               155: "not unlocked (no OEM chain)", 156: "not unlocked (chain unfinished)", 157: "not unlocked (3 A cable)",
               158: "not unlocked (vendor not accepted)", 159: "not unlocked (source reset)"}
# include/k2/chargertest/charger_test.hpp HuntCause (in order) and the sink's ReqAnswer (include/k2/pd/pd_sink.hpp)
HUNT_CAUSES = ["none", "no-chain", "chain-stalled", "inside-chain", "request-held", "fixed-pdo", "cable-cap",
               "apdo-stale", "capped-5a", "refused-offer", "unexplained"]
HUNT_ANSWERS = ["none", "accept", "reject", "wait"]
ID_STATES = ["idle", "running", "done", "aborted"]
ID_STEPS =["identity", "svids", "modes", "sink-cap", "status", "revision", "done"]
ID_POLL_S = 8.0
# include/k2/pd/pd_reread.hpp: RereadState, RereadStep (the mask bit of a step is 1 << its index)
REREAD_STATES = ["idle", "running", "done", "aborted"]
REREAD_STEPS = ["caps-ext", "identity", "svids", "get-cap", "get-cap-2", "modes", "mfr-info", "done"]   # the last two steps are the survey's, never asked by the lab command
REREAD_DEFAULT_MASK = 15   # caps-ext, identity, svids, one Get_Source_Cap
SINK_CAP_EXT_MODES = ["silent", "skedb", "not-supported"]   # pd::SinkCapExtMode, PD_REREAD op 3
REREAD_POLL_S = 14.0       # attach 8 s + settle 1 s + steps 2.5 s (include/k2/pd/pd_reread.hpp)
# fc::Proto, fc::State, fc::Fail, fc::Outcome in enum order (include/k2/fastcharge/fc_engine.hpp, fc_scan.hpp)
FC_PROTOS = ["qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo"]
FC_STATES = ["idle", "negotiating", "active", "failed"]
FC_FAILS = ["none", "need confirm", "over limit", "no power monitor", "busy", "unsupported", "no handshake",
            "no response", "voltage not reached", "overvoltage", "source lost", "aborted", "PD or load active",
            "needs load", "handshake ok", "detected"]
FC_OUTCOMES = ["-", "accepted", "rejected", "skipped"]
FC_OP_STATUS, FC_OP_RESET, FC_OP_SCAN, FC_OP_TRIGGER = 0, 1, 2, 3
FC_CONFIRM, FC_ALLOW_INLINE = 1, 2   # request flags: confirm the voltage / override the load guard
FC_SAFE_MV = 5000          # above this (and for the scan) a request needs --confirm
TEST_GROUPS = ["pd", "qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo"]
TEST_ALGORITHMS = ["pd2-fixed", "pd3x-fixed", "epr", "spr-avs", "pps", "qc2", "qc3", "qc3+", "afc",
                   "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo",
                   "pd3.0-revision", "pd3.1-revision", "pd3.2-revision", "epr-avs", "qc2b", "qc3b"]
TEST_ALGORITHM_GROUPS = [0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 1, 2]
TEST_STAGES = ["algorithms", "group", "cable", "identity", "review", "running", "results", "steps", "detail", "resume",
               "changes", "ask", "refuse", "waiting", "custom"]   # "algorithms" = the Tests step (presets), "custom" = its
                                             # per-family sub-list (still step 1/3, the "group" pages hang off it);
                                             # "identity"/"ask" are retired numbers (kept reserved on the wire);
                                             # "refuse" = the Ra guard said no (a cable is on the plug): run with OFF or cancel;
                                             # "waiting" = START presented Rd and waits for the charger (OK or BACK cancels it)
TEST_ALL = (1 << len(TEST_ALGORITHMS)) - 1
TEST_STATES = ["idle", "baseline", "requesting", "holding", "releasing", "done", "stopped", "unsafe", "detach charger", "initializing..."]
TEST_RESULTS = ["pending", "running", "pass", "fail", "skip"]
# charger_test::Unit in enum order (include/k2/chargertest/charger_test.hpp): Pd, the legacy protocols, then the low PPS/AVS rows
TEST_UNITS = ["pd", "qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo", "pd-low"]
TEST_REASONS = ["none", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
                "no power monitor", "no source", "load attached", "tool busy", "fake offers", "request failed",
                "timeout", "voltage outside band", "hard reset", "source lost", "cancelled", "5V release failed",
                "offers changed", "PD active: use QC port", "revision cannot be forced",
                "select at least one test", "physical cable limit", "unsupported PDO layout", "charger stayed in PD",
                "rebooted during request", "rebooted during handover", "EPR entry refused", "no contract",
                "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered", "blocked by PD",
                "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START", "Class A only (12 V max)",
                "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target", "withdrawn by source", "wrong voltage on 5V contract", "needs load"]
TEST_MAX_STEPS = 112  # Runner's bounded storage: pd::kMaxPdos * 6 + 46
# fastcharge::Protocol in enum order (include/k2/fastcharge/fastcharge_classify.hpp)
FC_NAMES = ["...", "NO INPUT", "NO QUICK", "DCP 5V", "QC3.0", "QC2.0 5V", "QC2.0 9V", "QC2.0 12V",
            "QC2.0 20V", "AFC FCP", "SCP", "APPLE 2.4A", "APPLE 2.1A", "APPLE 1.0A", "USB2.0 HI",
            "USB1.1 LOW", "SAM 2.0A", "VOOC WARP", "SVOOC", "PD PE", "PD MTK", "PD", "USB DATA", "PD?"]
ADC_CHANNELS = [0, 1, 2, 3, 4, 5, 6, 7, 16, 17]
ADC_SCAN = ["aux", "cc1", "cc2", "dp", "dm", "ntc", "vref"]
RIPPLE_SAMPLES = 512
RIPPLE_FULL_SCALE = 4095
CHIPS = {0: "none", 1: "INA228", 2: "INA226", 3: "INA237"}
SPEC_REVS = {0: "unknown", 1: "PD2.0", 2: "PD3.x", 3: "reserved"}
PDO_TYPES = {0: "unknown", 1: "fixed", 2: "battery", 3: "variable", 4: "pps", 5: "avs"}

SHORT_PRESS_MS = 100
LONG_PRESS_MS = 700    # firmware long-press threshold is 500 ms


class FrameError(ValueError):
    pass


class RemoteError(RuntimeError):
    def __init__(self, status, cmd, payload=b""):
        self.status, self.cmd, self.payload = status, cmd, payload
        text = f"command {cmd} failed: {STATUS_NAMES.get(status, status)}"
        if cmd == CMD_CHARGER_TEST and status == STATUS_BUSY:
            text += " (RAM results changed; retry charger-test results)"
        if cmd == CMD_DEBUG_CC_PROBE and status == STATUS_BUSY:
            text += " (the K2 is not passive: a charger test, a contract, Rd, VBUS at 1 V or more; the [CCPROBE] log line names the reason)"
        if cmd == CMD_DEBUG_CC_PROBE and status == STATUS_BAD_ARG:
            text += " (sw0 with a pull-up bit, a VCONN bit with PC13 high or with PDWN on the same line; ms 10..5000; pc 0..7)"
        if cmd in (CMD_PD_REQUEST, CMD_PD_RESET, CMD_PD_RELEASE) and payload and payload[0] < len(PD_ERRS) and payload[0]:
            text += f" ({PD_ERRS[payload[0]]})"
        if cmd == CMD_QRTEST and status == STATUS_BAD_ARG and payload:
            text += f" ({payload.decode('ascii', errors='replace')})"   # the show op's reason (range / box fit)
        super().__init__(text)


class RemoteTimeout(RuntimeError):
    pass


class BootloaderDetected(RuntimeError):
    pass


class Response:
    def __init__(self, cmd, seq, status, payload):
        self.cmd, self.seq, self.status, self.payload = cmd, seq, status, payload


def pack_request(cmd, seq, payload=b""):
    if len(payload) > MAX_REQ_PAYLOAD:
        raise ValueError(f"payload {len(payload)} > {MAX_REQ_PAYLOAD}")
    head = REQ_MAGIC + bytes([cmd & 0xFF, seq & 0xFF, len(payload)])
    return (head + bytes(payload)).ljust(FRAME_SIZE, b"\x00")


def is_response(raw):
    return len(raw) == FRAME_SIZE and raw[:2] == RSP_MAGIC


def is_bootloader_frame(raw):
    return len(raw) >= 2 and raw[:2] == b"\xFF\x55"


def parse_response(raw):
    if len(raw) != FRAME_SIZE:
        raise FrameError(f"frame length {len(raw)} != {FRAME_SIZE}")
    if raw[:2] != RSP_MAGIC:
        raise FrameError("not a remote response (bad magic)")
    n = raw[5]
    if n > FRAME_SIZE - RSP_HEADER:
        raise FrameError(f"payload length {n} exceeds frame")
    return Response(raw[2], raw[3], raw[4], bytes(raw[RSP_HEADER:RSP_HEADER + n]))


class RemoteLink:
    def __init__(self, transport, timeout_s=1.0):
        self.transport, self.timeout_s, self.seq = transport, timeout_s, 0
        self.tap = None                          # optional callback fed every frame read (debug dumps)

    def _read(self, ms):
        raw = bytes(self.transport.read(ms))
        if raw and self.tap:
            self.tap(raw)
        return raw

    def call(self, cmd, payload=b""):
        self.seq = (self.seq % 255) + 1          # 1..255, never 0, so a stale zero report cannot match
        seq = self.seq
        self.transport.write(pack_request(cmd, seq, payload))
        deadline = time.monotonic() + self.timeout_s
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise RemoteTimeout("no response")
            raw = self._read(max(1, int(left * 1000)))
            if not raw:
                continue
            if is_bootloader_frame(raw):
                raise BootloaderDetected("reply is a bootloader frame")
            if not is_response(raw):
                continue                        # log text
            rsp = parse_response(raw)
            if rsp.seq != seq or rsp.cmd != cmd:
                continue                        # stale answer to an earlier request
            if rsp.status != STATUS_OK:
                raise RemoteError(rsp.status, cmd, rsp.payload)
            return rsp

    def read_stream(self, seq, timeout_s=2.0, cmd=None, what="screenshot"):
        """Next stream frame (default: screenshot) carrying `seq`; log text and other frames are skipped."""
        cmd = CMD_SCREEN_DATA if cmd is None else cmd
        deadline = time.monotonic() + timeout_s
        while True:
            left = deadline - time.monotonic()
            if left <= 0:
                raise RemoteTimeout(f"{what} stream stalled")
            raw = self._read(max(1, int(left * 1000)))
            if not raw:
                continue
            if is_bootloader_frame(raw):
                raise BootloaderDetected("reply is a bootloader frame")
            if not is_response(raw):
                continue
            rsp = parse_response(raw)
            if rsp.cmd == cmd and rsp.seq == seq:
                return rsp


def _cstr(b):
    return b.split(b"\x00", 1)[0].decode("utf-8", errors="replace")


def _name(table, key):
    return table.get(key, str(key))


def _need(payload, n, what):
    if len(payload) < n:
        raise FrameError(f"{what} payload too short ({len(payload)} < {n})")


def decode_ping(p):
    _need(p, 29, "ping")
    return {"proto": p[0], "uptime_ms": int.from_bytes(p[1:5], "little"),
            "git": _cstr(p[5:29]), "version": _cstr(p[29:])}


def direction_of(current_ma):
    """The firmware zeroes |I| < 10 mA, so the sign is the whole answer. Positive current = power flowing bottom port -> top port ("up")."""
    return "up" if current_ma > 0 else "down" if current_ma < 0 else "idle"


RESET_FLAG_BITS = ((31, "LPWR"), (30, "WWDG"), (29, "IWDG"), (28, "SW"), (27, "POR"), (26, "PIN"))


def reset_cause_name(flags):
    """The one cause to report from CRM CTRLSTS bits 26..31; same priority as k2::sysreset::cause
    (PIN follows the NRST line on every reset, so the more specific flags win)."""
    return next((name for bit, name in RESET_FLAG_BITS if flags >> bit & 1), "none")


def decode_state(p):
    _need(p, 24, "state")
    i32 = lambda o: int.from_bytes(p[o:o + 4], "little", signed=True)
    st = {"page": p[0], "page_name": page_name(p[0]),
          "active_cc": p[1], "spec_rev": _name(SPEC_REVS, p[2]), "num_pdos": p[3],
          "pd_valid": bool(p[4]), "chip": _name(CHIPS, p[5]), "pm_online": bool(p[6]),
          "log_on": bool(p[7]), "vbus_mv": i32(8), "current_ma": i32(12),
          "power_mw": i32(16), "energy_mwh": i32(20),
          "direction": direction_of(i32(12))}
    if len(p) >= 40:                              # MCU die temperature, added after the NTC fields
        st.update({"mcu_temp_c": int.from_bytes(p[38:40], "little", signed=True) / 10,
                   "mcu_valid": bool(p[37] & 8)})
    if len(p) >= 38:                              # analog front end, absent on older firmware
        u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
        flags = p[37]
        st.update({"dp_mv": u16(24), "dm_mv": u16(26), "cc1_mv": u16(28), "cc2_mv": u16(30),
                   "aux_mv": u16(32),
                   "temp_c": int.from_bytes(p[34:36], "little", signed=True) / 10,
                   "fc": _name(dict(enumerate(FC_NAMES)), p[36]),
                   "adc_valid": bool(flags & 1), "temp_valid": bool(flags & 2),
                   "temp_calibrated": bool(flags & 4)})
    if len(p) >= 49:                              # link state: charger / cable data live or last seen
        lf = p[40]
        st.update({"stale": bool(lf & 1), "waiting": bool(lf & 2), "inline": bool(lf & 4),
                   "age_s": int.from_bytes(p[41:45], "little"),
                   "cable_scanned": bool(lf & 16), "cable_stale": bool(lf & 8),
                   "cable_age_s": int.from_bytes(p[45:49], "little")})
    if len(p) >= 53:                              # why this boot happened (CTRLSTS flags, cleared after capture)
        flags = int.from_bytes(p[49:53], "little")
        st.update({"reset_flags": flags, "reset_cause": reset_cause_name(flags)})
    return st


def decode_adc_raw(p):
    _need(p, 35, "adc-raw")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    return {"ok": bool(p[0]),
            "channels": {ch: u16(1 + 2 * i) for i, ch in enumerate(ADC_CHANNELS)},
            "scan": {name: u16(21 + 2 * i) for i, name in enumerate(ADC_SCAN)}}


def decode_cable_r(p):
    _need(p, 16, "cable-r")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    return {"idle": bool(p[0] & 1), "valid": bool(p[0] & 2), "kelvin": bool(p[0] & 4),
            "pos_mohm": u16(12), "gnd_mohm": u16(14),
            "grade": CABLE_GRADES[p[1]] if p[1] < len(CABLE_GRADES) else f"?{p[1]}",
            "r_mohm": u16(2), "idle_mv": u16(4), "idle_ma": u16(6), "load_mv": u16(8), "load_ma": u16(10)}


def decode_ripple_info(p):
    _need(p, 13, "ripple")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    return {"code": p[0], "period_ns": int.from_bytes(p[1:5], "little"), "count": u16(5),
            "min": u16(7), "max": u16(9), "mean": u16(11)}


def decode_pdo(p):
    _need(p, 4, "pdo")
    head = {"valid": bool(p[0]), "spec_rev": _name(SPEC_REVS, p[1]), "num_pdos": p[2], "index": p[3]}
    if len(p) < 19:
        return head, None
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    pdo = {"type": _name(PDO_TYPES, p[4]), "raw": int.from_bytes(p[5:9], "little"),
           "min_mv": u16(9), "max_mv": u16(11), "max_ma": u16(13),
           "max_mw": int.from_bytes(p[15:19], "little")}
    return head, pdo


LATENCY = ["n/a", "<10 ns (~1 m)", "10-20 ns (~2 m)", "20-30 ns (~3 m)", "30-40 ns (~4 m)",
           "40-50 ns (~5 m)", "50-60 ns (~6 m)", "60-70 ns (~7 m)", ">70 ns"]
TERMINATION = ["Passive (VCONN not required)", "Passive (VCONN required)",
               "Active (1 end powered)", "Active (both ends powered)"]


def decode_emarker_detail(p2):
    """Part 2: cable VDO detail. Codes are named here, the firmware sends numbers."""
    _need(p2, 10, "emarker detail")
    f = p2[5]
    lat = p2[3]
    return {"sop": {1: "SOP'", 2: "SOP''"}.get(p2[0], "none"), "pd_rev": {1: "2.0", 2: "3.x"}.get(p2[1]),
            "vdo_version": p2[2], "latency_code": lat,
            "latency": LATENCY[lat] if lat < len(LATENCY) else f"code {lat}",
            "termination": TERMINATION[p2[4] & 3],
            "type_c_plug": bool(f & 1), "vconn_required": bool(f & 2), "epr_cable": bool(f & 4),
            "vdo2_valid": bool(f & 8), "usb4": bool(f & 16), "usb_gen2": bool(f & 32),
            "optical": bool(f & 64), "thunderbolt": bool(f & 128),
            "max_temp_c": p2[6], "shutdown_temp_c": p2[7], "u3_power": p2[8], "usb_lanes": p2[9]}


def decode_identity(p):
    _need(p, 10, "identity")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    n = p[8]
    _need(p, 9 + 2 * n, "identity svids")
    vendor, _, model = p[9 + 2 * n:].partition(b"\x00")
    return {"valid": bool(p[1]), "epr": bool(p[0] & 1), "epr_partial": bool(p[0] & 2),
            "cuktech": bool(p[0] & 4), "xiaomi": bool(p[0] & 8),
            "vid": u16(2), "pid": u16(4), "bcd": u16(6),
            "svids": [u16(9 + 2 * i) for i in range(n)],
            "vendor": _cstr(vendor), "model": _cstr(model)}


def _name_of(table, i):
    return table[i] if i < len(table) else str(i)


def decode_pd_trigger(p):
    _need(p, 13, "pd trigger status")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    # source_hard_resets (u32, appended after hard_resets) is absent from older firmware: 0 then
    return {"state": _name_of(SINK_STATES, p[0]), "result": _name_of(SINK_RESULTS, p[1]), "held": bool(p[2]),
            "epr": bool(p[3]), "pos": p[4], "mv": u16(5), "ma": u16(7),
            "hard_resets": int.from_bytes(p[9:13], "little"),
            "source_hard_resets": int.from_bytes(p[13:17], "little") if len(p) >= 17 else 0}


MATCH_VERDICTS = ["no-caps", "no-request", "outside-caps", "wants-higher-v", "current-limited",
                  "mismatch-bit", "sink-unknown", "matches"]
MATCH_ROWS = ["unknown", "selected", "selected-low", "ok", "low", "no", "gone"]
MATCH_FLAGS = [(1, "mismatch-bit"), (2, "wants-higher-v"), (4, "current-limited"), (8, "outside-caps")]


def decode_match(p):
    """PD_MATCH: 21 bytes + one row state per PDO, see remote_proto.hpp."""
    _need(p, 19, "match")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    n = p[5]
    _need(p, 19 + n, "match rows")
    return {"caps_seen": bool(p[0]), "request_seen": bool(p[1]), "sink_ok": bool(p[2]),
            "flags": [name for bit, name in MATCH_FLAGS if p[3] & bit],
            "verdict": _name_of(MATCH_VERDICTS, p[4]), "want_pos": p[6], "want_mv": u16(7), "want_ma": u16(9),
            "need_ma": u16(11), "have_ma": u16(13), "src_max_mv": u16(15), "src_max_w": u16(17),
            "rows": [_name_of(MATCH_ROWS, b) for b in p[19:19 + n]]}


USB_FAMILIES = ["unknown", "low-speed", "full-speed", "usb2", "usb3.2", "usb4", "usb4v2"]
USB_CONF = ["none", "hint", "advertised", "negotiated"]
USB_SOURCES = ["none", "d+/d-", "identity", "cable", "enter_usb", "enter_mode_tbt", "dp_alt", "data_reset"]


def decode_usb_link(p):
    """USB_LINK: 9 bytes + the UI text, see remote_proto.hpp."""
    _need(p, 9, "usb link")
    return {"family": _name_of(USB_FAMILIES, p[0]), "tbt": [None, "TBT3", "TBT4", "TBT5"][p[1]] if p[1] < 4 else p[1],
            "confidence": _name_of(USB_CONF, p[2]), "source": _name_of(USB_SOURCES, p[3]),
            "cable_limited": bool(p[4] & 1), "dp_4lane": bool(p[4] & 2), "billboard": bool(p[4] & 4), "dp": bool(p[4] & 8),
            "mbps": int.from_bytes(p[5:9], "little"), "text": p[9:].decode("ascii", "replace")}


def cmd_usblink(link, a):
    return decode_usb_link(link.call(CMD_USB_LINK).payload)


def cmd_match(link, a):
    return decode_match(link.call(CMD_PD_MATCH).payload)


def decode_identify_progress(p):
    _need(p, 10, "identify status")
    n = p[5]
    _need(p, 10 + 4 * n, "identify sink pdos")
    u32 = lambda o: int.from_bytes(p[o:o + 4], "little")
    ok, skipped = p[2], p[3]
    return {"state": _name_of(ID_STATES, p[0]), "step": _name_of(ID_STEPS, p[1]),
            "answered": [ID_STEPS[i] for i in range(6) if ok & (1 << i)],
            "no_answer": [ID_STEPS[i] for i in range(6) if skipped & (1 << i)],
            "retries": p[4], "revision": u32(6), "sink_pdos": [u32(10 + 4 * i) for i in range(n)]}


def decode_reread(p):
    """PD_REREAD: state step mask ok skipped pdos_before pdos_last caps_rx flags, see remote_proto.hpp."""
    _need(p, 9, "pd-reread")
    steps = REREAD_STEPS[:5]
    return {"state": _name_of(REREAD_STATES, p[0]), "step": _name_of(REREAD_STEPS, p[1]), "mask": p[2],
            "answered": [steps[i] for i in range(5) if p[3] & (1 << i)],
            "skipped": [steps[i] for i in range(5) if p[4] & (1 << i)],
            "pdos_before": p[5], "pdos_last": p[6], "caps_rx": p[7], "avs": bool(p[8] & 1),
            "attaching": bool(p[8] & 2), "attach_failed": bool(p[8] & 4), "ra": bool(p[8] & 8)}


def cmd_pd_reread(link, a, sleep=time.sleep, clock=time.monotonic):
    """Starts the source-cap re-read (PD mode) with one step mask, waits for it, returns the outcome.
    Without a contract the firmware presents Rd and requests PDO 1 first (the attach phase)."""
    if getattr(a, "abort", False):
        return decode_reread(link.call(CMD_PD_REREAD, bytes([2])).payload)
    mask = getattr(a, "mask", None)
    mask = REREAD_DEFAULT_MASK if mask is None else mask
    mode = getattr(a, "sink_cap_ext", None)
    if mode is not None:   # how the sink answers Get_Sink_Cap_Extended during this run (RAM only)
        link.call(CMD_PD_REREAD, bytes([3, SINK_CAP_EXT_MODES.index(mode)]))
    pin = getattr(a, "pin", None)
    if pin is not None:   # lab: pin the Rd on CC1/CC2 (0 = release) so a HID-powered K2 stands in for a charger-powered one
        out = decode_reread(link.call(CMD_PD_REREAD, bytes([6, pin])).payload)
        if getattr(a, "no_run", False):
            return out
    glitch = getattr(a, "rd_glitch", None)
    if glitch is not None:   # lab: a controlled re-attach, the Rd off for this many ms (10 ms steps), Ra kept; the result is in the K2 log
        return decode_reread(link.call(CMD_PD_REREAD, bytes([8, max(1, min(255, glitch // 10))])).payload)
    if getattr(a, "relook", False):   # lab: the unit boundary's Soft_Reset (the source re-announces, the Rd stays)
        return decode_reread(link.call(CMD_PD_REREAD, bytes([7])).payload)
    ra = getattr(a, "ra", None)
    if ra is not None:   # Ra mode: PC13 high (an Ra on CC1) before Rd goes on CC2; held until detach, --abort, --ra off or reboot
        out = decode_reread(link.call(CMD_PD_REREAD, bytes([5, 1 if ra == "on" else 0])).payload)
        if ra == "off" or getattr(a, "no_run", False):
            return out
    line = getattr(a, "line", None)
    if line is not None:   # debug: the attach scan may take only this CC line (the passive scan can take a cable's Ra net);
        link.call(CMD_PD_REREAD, bytes([4, line]))   # it stays for later attaches (charger test START) until --line 0 / --abort
    out = decode_reread(link.call(CMD_PD_REREAD, bytes([1, mask])).payload)
    deadline = clock() + getattr(a, "wait", REREAD_POLL_S)
    while out["state"] == "running" and clock() < deadline:
        sleep(0.1)
        out = decode_reread(link.call(CMD_PD_REREAD, bytes([0])).payload)
    return out


def _reread_mask(text):
    try:
        v = int(text, 0)
    except ValueError:
        raise argparse.ArgumentTypeError(f"mask must be a number, got {text!r}")
    if not 1 <= v <= 31:
        raise argparse.ArgumentTypeError("mask is 1..31: 1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 second get-cap")
    return v


def decode_emarker(p0, p1=b"", p2=None):
    _need(p0, 17, "emarker")
    u16 = lambda o: int.from_bytes(p0[o:o + 2], "little")
    vendor, _, speed = p0[17:].partition(b"\x00")
    out = {"valid": bool(p0[0]), "active": bool(p0[1]), "vid": u16(2), "pid": u16(4),
            "xid": int.from_bytes(p0[6:10], "little"), "hw_rev": p0[10], "fw_rev": p0[11],
            "max_current_a": p0[12], "max_voltage_v": p0[13], "max_power_w": u16(14),
            "num_vdos": p0[16], "vendor": _cstr(vendor), "speed": _cstr(speed),
            "vdos": [int.from_bytes(p1[o:o + 4], "little") for o in range(0, len(p1) - 3, 4)]}
    if p2 is not None:
        out.update(decode_emarker_detail(p2))
    return out


# --- commands -------------------------------------------------------------------------------

def cmd_ping(link, a):
    return decode_ping(link.call(CMD_PING).payload)

def decode_charger_test(p):
    if len(p) < 15:
        raise FrameError("short charger-test metadata")
    schema, run_id, revision, state, reason, total, current, start, n = struct.unpack_from("<BIIBBBBBB", p)
    if schema not in (1, 2):
        raise FrameError(f"unsupported charger-test schema {schema}")
    modern = schema == 2
    header, page_size, maximum, reasons = (40, 1, TEST_MAX_STEPS, len(TEST_REASONS)) if modern else (15, 2, 95, 20)
    if (state >= len(TEST_STATES) or reason >= reasons or total > maximum or
            current > total or start > total or n > page_size or start + n > total or len(p) != header + 16 * n):
        raise FrameError("invalid charger-test page shape")
    out = {"schema": schema, "run_id": run_id, "revision": revision, "state": TEST_STATES[state],
           "reason": TEST_REASONS[reason], "total": total, "current": current, "start": start, "count": n,
           "selection": None, "stage": None, "evidence": None, "resumed": 0}
    if modern:
        run_mask, draft_mask = struct.unpack_from("<II", p, 15)
        stage, wire, flags = (p[23] >> 2) & 15, p[23] & 3, p[25]
        out["resumed"] = p[23] >> 6   # times this run was resumed after a reboot (saturates at 3)
        if (run_mask | draft_mask) & ~TEST_ALL or stage >= len(TEST_STAGES) or p[39] & 15 > 11:
            raise FrameError("invalid charger-test selection or evidence")
        out["selection"] = {"run_mask": run_mask, "configured_mask": draft_mask,
                            "run": [a for i, a in enumerate(TEST_ALGORITHMS) if run_mask & (1 << i)],
                            "configured": [a for i, a in enumerate(TEST_ALGORITHMS) if draft_mask & (1 << i)]}
        out["stage"] = TEST_STAGES[stage]
        out["cursor"] = p[39] >> 4   # setup list cursor (0-based row; a pick advances, there is no Next row), saturates at 15
        monitor, cable_mv, cable_ma = struct.unpack_from("<HHH", p, 26)
        out["evidence"] = {
            "origin": "run-start" if run_id else "no-run", "source_session": int.from_bytes(p[33:37], "little"),
            "raw_header": int.from_bytes(p[37:39], "little"), "pdo_count": p[39] & 15,
            "wire_revision_code": wire, "wire_revision": SPEC_REVS[wire],
            "source_rmdo": {"revision": f"{p[24] >> 4}.{p[24] & 15}",
                            "version": f"{p[32] >> 4}.{p[32] & 15}",
                            "provenance": "source self-report"} if flags & 1 else None,
            "advertised": {"pps": bool(flags & 2), "epr_table": bool(flags & 4),
                           "spr_avs": bool(flags & 8), "epr_capable": bool(flags & 16), "partial_epr": bool(flags & 64)},
            "monitor_mv": monitor, "physical_cable": {"observed": bool(flags & 32), "max_mv": cable_mv, "max_ma": cable_ma},
            "spoof_on": bool(flags & 128), "independent_revision_tested": False,
            "pass_scope": "negotiation and measured voltage only; not rated-current/power or revision conformance"}
    rows = []
    for i in range(n):
        code, pos, flags, mv, ma, measured, result, why, detail, pdo = struct.unpack_from("<BBBHHHBBBI", p, header + 16 * i)
        if (code >= (len(TEST_ALGORITHMS) if modern else 10) or pos > 11 or result >= len(TEST_RESULTS) or
                why >= reasons or flags & ~0x1F or (result == 4 and why == 0)):
            raise FrameError("invalid charger-test row")
        group = TEST_ALGORITHM_GROUPS[code] if modern else code
        protocol = TEST_GROUPS[group]
        if modern:
            if not run_mask & (1 << code):
                raise FrameError("charger-test row was not selected")
            if 16 <= code <= 18 and (result != 4 or why != 20):  # revision probes; UFCS/VIVO now run
                raise FrameError("unavailable charger-test algorithm must SKIP")
            # Only a row that ran (pass / fail) says anything about the wire revision. A stopped or in-progress run still lists its
            # unrun PD rows (pending / running, and the placeholder PD rows planned before the source list arrived): those carry
            # no evidence and must not make the whole results read fail.
            if code < 2 and result in (2, 3) and wire != code + 1:
                raise FrameError("charger-test row contradicts negotiated wire revision")
            protocol = ("pd2-fixed" if code == 0 else "pd3x-fixed" if code == 1 else
                        "epr-avs" if flags & 2 else "epr-fixed") if code < 3 else TEST_ALGORITHMS[code]
            if 11 <= code <= 13:
                protocol += "-5v-handshake"
        elif group == 0 and pos:
            kind = pdo >> 30
            protocol = "pd-fixed" if kind == 0 else ("pd-avs" if flags & 2 else "pd-pps") if kind == 3 else "pd-other"
        detail_reason = "none"
        if detail in EPR_DETAILS and group == 0:
            detail_reason = EPR_DETAILS[detail]
        elif detail:
            names, detail_code = (PD_ERRS, detail - 128) if group == 0 and detail >= 128 else (
                SINK_RESULTS if group == 0 else FC_FAILS, detail)
            detail_reason = names[detail_code] if detail_code < len(names) else f"unknown ({detail})"
            if (flags >> 2) & 7 > 1:
                detail_reason += f" ({(flags >> 2) & 7} tries)"
        row = {"index": start + i, "group": TEST_GROUPS[group], "protocol": protocol, "pos": pos,
               "target_mv": mv, "request_ma": ma, "measured_mv": measured,
               "result": TEST_RESULTS[result], "reason": TEST_REASONS[why],
               "detail": detail, "detail_reason": detail_reason, "raw_pdo": pdo,
               "continuous": bool(flags & 1), "avs": bool(flags & 2)}
        if (flags >> 2) & 7:
            row["attempts"] = (flags >> 2) & 7
        if modern:
            row["algorithm"] = TEST_ALGORITHMS[code]
            row["explanation"] = (
                f"No independent PD3.{code - 16} test" if 16 <= code <= 18 else
                f"Observed {SPEC_REVS[wire]}; revision cannot be forced" if why == 20 else
                # mirrors ct::explanation(): every EPR skip names its cause
                EPR_DETAILS.get(detail, "EPR: above the K2's monitor") if why == 3 and code in (2, 19) else
                "No allowed current at target" if why == 2 and pos else
                (EPR_DETAILS[detail] if detail in range(147, 155) else "EPR: cable not 50V 5A") if why == 22 and code in (2, 19) else
                "EPR: not offered" if why == 2 and code in (2, 19) else
                EPR_DETAILS.get(detail, "EPR: entry refused") if why == 27 else
                "EPR: no PD contract" if why == 28 else
                EPR_DETAILS.get(detail, TEST_REASONS[why]) if why == 38 else
                "SCP 5A modes need an observed 5A cable" if why == 22 and code == 10 else TEST_REASONS[why])
        rows.append(row)
    out["steps"] = rows
    return out


def parse_units(text):
    """'qc2,qc3,pd' -> [1, 2, 0] (charger_test::Unit values, in the order given); '' / 'default' = the firmware's default order."""
    names = [t.strip().lower() for t in text.split(",") if t.strip()]
    if names == ["default"]:
        return []
    out = []
    for n in names:
        if n not in TEST_UNITS:
            raise FrameError(f"unknown unit '{n}' (one of {', '.join(TEST_UNITS)})")
        if TEST_UNITS.index(n) in out:
            raise FrameError(f"unit '{n}' listed twice")
        out.append(TEST_UNITS.index(n))
    return out


def charger_units(link, units):
    """Set the unit list of the NEXT charger run (reset by every START). Empty list = the default order. Busy while a run is active."""
    r = link.call(CMD_CHARGER_UNITS, bytes([len(units), *units]))
    return {"units": [TEST_UNITS[u] for u in units] or ["default"], "count": r.payload[0] if r.payload else len(units)}


def cmd_charger_units(link, a):
    return charger_units(link, parse_units(a.units))


CABLE_KINDS = ["off", "builtin", "saved"]   # k2::ctcable::Kind in enum order

# The firmware's built-in cables (k2::emrec::builtin_cable ids): captured from real cables except 0.
# Id 3 ("OnePlus 100W (mag)") is retired: the firmware reads a 3 as 4 and always answers 4, so 3 has no label of its own.
BUILTIN_LABELS = {0: "generic 20V 5A", 1: "EPR 50V 5A", 2: "CUKTECH 6A", 3: "OnePlus 100W", 4: "OnePlus 100W"}
BUILTIN_NAMES = {"generic": 0, "epr-50v": 1, "generic-epr": 1, "cuktech-6a": 2, "cuktech-240w": 2,
                 "oneplus-mag": 4, "oneplus": 4}   # spellings; cuktech-240w and generic-epr are the older names, oneplus-mag the retired one
DIALOG_BUILTINS = (1, 2, 4)                        # the ids the charger-test START dialog lists (3 is a retired alias of 4)


def parse_builtin(text):
    """'4', 'oneplus-mag' ... -> the built-in id, or None when `text` is no built-in spelling (the id is not range-checked by name)."""
    t = text.strip().lower()
    if t in BUILTIN_NAMES:
        return BUILTIN_NAMES[t]
    return int(t) if t.isdigit() and int(t) in BUILTIN_LABELS else None


def parse_cable(text):
    """'off' -> (0, 0); 'builtin' (the default built-in, EPR 50V 5A), 'builtin:N' or 'builtin:<name>' -> (1, id);
    'saved:3' (a library id; 'clone:3' too) -> (2, 3). 'off' is the no-spoof default. Built-in ids 1, 2 and 4 are the dialog's rows (3 is the retired spelling of 4)."""
    t = text.strip().lower()
    if t in ("", "off", "none"):
        return 0, 0
    if t in ("builtin", "built-in"):
        return 1, 1
    kind, _, seq = t.partition(":")
    if kind in ("builtin", "built-in"):
        bid = parse_builtin(seq)
        if bid == 3:
            bid = 4                      # the retired id: the device reads it as 4 and answers 4
        if bid in DIALOG_BUILTINS:
            return 1, bid
        raise FrameError(f"unknown built-in cable '{seq}' (ids 1, 2, 4 or " + ", ".join(
            n for n, i in BUILTIN_NAMES.items() if i in DIALOG_BUILTINS) + ")")
    if kind in ("saved", "clone") and seq.isdigit() and 0 < int(seq) <= 0xFFFF:
        return 2, int(seq)
    raise FrameError(f"unknown cable identity '{text}' (off, builtin, builtin:<id|name> or saved:<library id>)")


def charger_cable(link, choice=None):
    """Set (choice = (kind, seq)) or read (None) the cable identity of the START dialog. Setting is refused (Busy) while a run is
    active and (BadArg) for a library id that is not saved. Reply: the choice, then the identity of the current / last run."""
    payload = b"" if choice is None else struct.pack("<BH", *choice)
    p = link.call(CMD_CHARGER_CABLE, payload).payload
    if len(p) != 30 or p[0] > 2 or p[3] > 2:
        raise FrameError("invalid charger-cable reply")
    name = p[6:30].split(b"\0", 1)[0].decode("ascii", "replace")
    return {"choice": CABLE_KINDS[p[0]], "choice_seq": int.from_bytes(p[1:3], "little"),
            "run": CABLE_KINDS[p[3]], "run_seq": int.from_bytes(p[4:6], "little"), "run_name": name}


def cmd_charger_cable(link, a):
    return charger_cable(link, parse_cable(a.cable) if a.cable else None)


# charger_test::HuntCause / HuntFlag / pd::ReqAnswer / pd::PdoType in enum order (names as the firmware's name(HuntCause) words).
HUNT_CAUSES = ["none", "no-chain", "chain-stalled", "inside-chain", "request-held", "fixed-pdo", "cable-cap", "apdo-stale",
               "capped-5a", "refused-offer", "unexplained"]
HUNT_EXPLANATIONS = ["not refused", "no OEM chain listed", "OEM chain stopped early", "asked inside the chain",
                     "request held back", "fixed PDO never offers 6 A", "cable under 5 A", "no caps after the auth",
                     "APDO still 5 A after auth", "refused an offered 6 A", "evidence incomplete"]
HUNT_FLAGS = ["svid", "opener", "authed", "reannounced", "in-chain", "held", "sent", "cable-read", "cable-inferred", "timeout", "hard-reset"]
HUNT_ANSWERS = ["none", "accept", "reject", "wait"]
HUNT_PDO_TYPES = ["unknown", "fixed", "battery", "variable", "pps", "avs"]
HUNT_SLOTS = 4
HUNT_SLOT_BYTES = 29


def charger_hunt(link, slot):
    """The evidence of refused hunt row `slot` (the firmware's side table, in the order the rows were refused). Reply: run_id, count of
    refused rows held, then the slot's snapshot when slot < count (else only the header: "slot" is None)."""
    p = link.call(CMD_CHARGER_HUNT, bytes([slot])).payload
    if len(p) not in (5, 5 + HUNT_SLOT_BYTES):
        raise FrameError("invalid charger-hunt reply")
    run_id, count = struct.unpack_from("<IB", p)
    has_slot = len(p) > 5
    if count > HUNT_SLOTS or has_slot != (slot < count):
        raise FrameError("invalid charger-hunt reply")
    out = {"run_id": run_id, "count": count, "slot": None}
    if not has_slot:
        return out
    step, cause, answer, last_step, caps, pos, ptype, flags, apdo_ma, req_mv, req_ma, cable_ma, authed_ms, answer_ms, apdo, rdo = \
        struct.unpack_from("<7B7H2I", p, 5)
    if cause >= len(HUNT_CAUSES) or answer >= len(HUNT_ANSWERS) or ptype >= len(HUNT_PDO_TYPES) or flags >> len(HUNT_FLAGS):
        raise FrameError("invalid charger-hunt evidence")
    out.update({
        "slot": slot, "step": step, "cause": HUNT_CAUSES[cause], "explanation": HUNT_EXPLANATIONS[cause], "answer": HUNT_ANSWERS[answer],
        "last_step": last_step, "caps": caps, "pos": pos, "type": HUNT_PDO_TYPES[ptype],
        "flags": flags, "flag_names": [n for i, n in enumerate(HUNT_FLAGS) if flags & (1 << i)],
        "apdo_ma": apdo_ma, "req_mv": req_mv, "req_ma": req_ma, "cable_ma": cable_ma,
        "authed_ms": authed_ms, "answer_ms": answer_ms, "apdo": apdo, "rdo": rdo})
    return out


def attach_hunt_evidence(link, run_id, rows):
    """Put the firmware's evidence on each refused 6 A hunt row (a FAIL, or the "not unlocked" skip, which is also a refusal the
    firmware recorded) as row["hunt"] (without run_id / count). Read only when such a row exists; a firmware without the
    command, or evidence of another run, attaches nothing."""
    if not any(step_label.is_hunt(s) and (s["result"] == "fail" or s["reason"] == "not unlocked") for s in rows):
        return
    by_index = {s["index"]: s for s in rows}
    try:
        slot, count = 0, 1   # the first reply says how many refused rows the firmware holds
        while slot < count:
            e = charger_hunt(link, slot)
            if e["run_id"] != run_id:
                return
            count = e["count"]
            if e["slot"] is None:
                return
            row = by_index.get(e["step"])
            if row is not None and step_label.is_hunt(row):
                row["hunt"] = {k: v for k, v in e.items() if k not in ("run_id", "count", "slot")}
            slot += 1
    except RemoteError:
        return


def cmd_charger_test(link, a):
    try:
        response = link.call(CMD_CHARGER_TEST, b"\x02")
    except RemoteError as e:
        if e.status not in (STATUS_BAD_LENGTH, STATUS_BAD_ARG):
            raise
        response = link.call(CMD_CHARGER_TEST)
    meta = decode_charger_test(response.payload)
    query = b"\x02" if meta["schema"] == 2 else b""
    if meta["start"] or meta["count"]:
        raise FrameError("charger-test metadata contains rows")
    result = {k: v for k, v in meta.items() if k not in ("start", "count") and (k != "cursor" or a.test_cmd == "status")}
    if a.test_cmd == "status":
        return result
    rows = []
    while len(rows) < meta["total"]:
        req = query + struct.pack("<BII", len(rows), meta["run_id"], meta["revision"])
        page = decode_charger_test(link.call(CMD_CHARGER_TEST, req).payload)
        if (any(page[k] != meta[k] for k in meta if k not in ("start", "count", "steps", "cursor")) or
                page["start"] != len(rows) or page["count"] != min(1 if query else 2, meta["total"] - len(rows))):
            raise FrameError("charger-test results changed or page did not advance; retry results")
        rows.extend(page["steps"])
    end = decode_charger_test(link.call(CMD_CHARGER_TEST, query).payload)
    if end != meta:
        raise FrameError("charger-test results changed during export; retry results")
    attach_hunt_evidence(link, meta["run_id"], rows)   # read after the export proved stable; the evidence is not part of that check
    result["steps"] = rows
    if a.test_cmd == "summary" and not getattr(a, "json", False):
        result["_summary"] = True
    if a.out:
        Path(a.out).write_text(json.dumps(result, indent=2) + "\n")
    return result


BUSY_POLL_S = 0.02      # retry step while the firmware still plays the previous virtual press
BUSY_MARGIN_S = 0.5     # grace on top of the press time the previous request may have asked for
ACTION_TIMEOUT_S = 15.0  # an e-marker scan blocks the firmware well past the 1 s default


def cmd_press(link, a, sleep=time.sleep):
    ms = a.ms if a.ms else (LONG_PRESS_MS if a.long else SHORT_PRESS_MS)
    if not 30 <= ms <= 10000:
        raise SystemExit("press duration must be 30..10000 ms")
    # A previous press of the same button answers Busy until it ends; scripts press back to back,
    # so wait for it (at most one press time plus a margin) instead of failing.
    waited, limit = 0.0, ms / 1000 + BUSY_MARGIN_S
    while True:
        try:
            link.call(CMD_BUTTON, bytes([BUTTONS[a.button]]) + ms.to_bytes(2, "little"))
            break
        except RemoteError as e:
            if e.status != STATUS_BUSY or waited >= limit:
                raise
            sleep(BUSY_POLL_S)
            waited += BUSY_POLL_S
    return {"button": a.button, "ms": ms}


def cmd_state(link, a):
    return decode_state(link.call(CMD_GET_STATE).payload)


def _link_fields(link, keys):
    """stale / age_s style fields from GET_STATE (older firmware: none)."""
    try:
        st = decode_state(link.call(CMD_GET_STATE).payload)
    except (RemoteError, FrameError):
        return {}
    return {k: st[k] for k in keys if k in st}


def cmd_pdos(link, a):
    head, first = decode_pdo(link.call(CMD_GET_PDOS, bytes([0])).payload)
    pdos = [first] if first else []
    for i in range(1, head["num_pdos"]):
        pdos.append(decode_pdo(link.call(CMD_GET_PDOS, bytes([i])).payload)[1])
    return {**head, "pdos": pdos, **_link_fields(link, ("stale", "waiting", "age_s", "inline"))}


EM_ERRS = ["Ok", "Full", "Invalid", "NotFound", "Flash", "Exists", "Inline", "Busy"]


class EmLibFailed(Exception):
    pass


def _emlib(link, payload, what):
    p = link.call(CMD_EM_LIB, bytes(payload)).payload
    _need(p, 1, what)
    if p[0]:
        e = EM_ERRS[p[0]] if p[0] < len(EM_ERRS) else str(p[0])
        raise EmLibFailed(f"emarker {what}: {e}")
    return p


def _em_get(link, seq):
    p = _emlib(link, [1, seq & 255, seq >> 8], "get")
    _need(p, 31, "emarker get")
    return {"seq": int.from_bytes(p[1:3], "little"), "name": _cstr(p[3:27]),
            "vid": int.from_bytes(p[27:29], "little"), "pid": int.from_bytes(p[29:31], "little")}


def _em_status(p):
    _need(p, 8, "emarker spoof")
    return {"active": bool(p[1]), "requests": int.from_bytes(p[2:4], "little"),
            "replies": int.from_bytes(p[4:6], "little"), "seq": int.from_bytes(p[6:8], "little")}


# ---- emarker learn / unmatched (k2/pd/emk_learn.hpp wire forms) ----
EM_LEARN_STATUS, EM_LEARN_RUN, EM_LEARN_ENTRY, EM_LEARN_SAVE, EM_UNMATCHED = 0, 1, 2, 3, 7
EM_SEEN_TOTALS = 255
LEARN_TIMEOUT_S = 10.0   # the K2 blocks for the VCONN scan, every probe and (with --sweep) up to ~2 s of silent ones
LEARN_OUTCOMES = ["none", "learned", "no-identity", "aborted"]
LEARN_STEPS = ["identity", "svids", "modes", "seed", "sweep"]
UNMATCHED_POLICIES = ["ignore", "nak", "not-supported"]
VDM_TYPES = ["REQ", "ACK", "NAK", "BUSY"]


def parse_seed(text):
    """'22D9:31' or '22D9:31:0x12345678[:0x...]' -> (svid, cmd, [vdo, ...]). SVID and VDOs are hex, the command decimal."""
    parts = text.split(":")
    try:
        svid, cmd = int(parts[0], 16), int(parts[1], 10)
        vdos = [int(v, 16) for v in parts[2:]]
    except (ValueError, IndexError):
        raise FcRefused(f"seed {text!r}: expected SVID:CMD[:VDO[:VDO]] (SVID and VDOs in hex, CMD 0..31)") from None
    if not (0 <= svid <= 0xFFFF and 0 <= cmd <= 31 and len(vdos) <= 2 and all(0 <= v <= 0xFFFFFFFF for v in vdos)):
        raise FcRefused(f"seed {text!r}: SVID 16 bit, CMD 0..31, at most two 32-bit VDOs")
    return svid, cmd, vdos


def encode_learn_options(seeds, sweep=False, confirm=False):
    """Options bytes of the learn run request: [flags, nseeds, (svid u16, cmd, nvdo, vdo u32 * nvdo) * nseeds]."""
    if len(seeds) > 4:
        raise FcRefused("at most 4 seeds")
    out = bytearray([(1 if sweep else 0) | (2 if confirm else 0), len(seeds)])
    for svid, cmd, vdos in seeds:
        out += struct.pack("<HBB", svid, cmd, len(vdos))
        for v in vdos:
            out += struct.pack("<I", v)
    return bytes(out)


def decode_learn_summary(p):
    _need(p, 13, "emarker learn")
    return {"outcome": LEARN_OUTCOMES[p[1]] if p[1] < len(LEARN_OUTCOMES) else str(p[1]), "svids": p[2], "probes": p[3],
            "silent": p[4], "truncated": bool(p[5] & 1), "log_full": bool(p[5] & 2), "entries": p[6], "rows": p[7],
            "unmatched_policy": UNMATCHED_POLICIES[p[8]] if p[8] < len(UNMATCHED_POLICIES) else str(p[8]),
            "vid": int.from_bytes(p[9:11], "little"), "pid": int.from_bytes(p[11:13], "little")}


def _words(p, at, n):
    return [struct.unpack_from("<I", p, at + 4 * i)[0] for i in range(n)]


def decode_learn_entry(p):
    """[err, i, step, spec_rev, req_n, got, rep_n, reply header u16, request words, reply words]."""
    _need(p, 9, "emarker learn entry")
    req_n, rep_n = p[4], p[6]
    _need(p, 9 + 4 * (req_n + rep_n), "emarker learn entry")
    return {"i": p[1], "step": LEARN_STEPS[p[2]] if p[2] < len(LEARN_STEPS) else str(p[2]), "spec_rev": p[3],
            "got": bool(p[5]), "reply_header": int.from_bytes(p[7:9], "little"),
            "request": _words(p, 9, req_n), "reply": _words(p, 9 + 4 * req_n, rep_n)}


def decode_unmatched(p):
    """Totals form [err, total u16, held]; an entry adds sop, n, header u16 and the words."""
    _need(p, 4, "emarker unmatched")
    out = {"total": int.from_bytes(p[1:3], "little"), "held": p[3]}
    if len(p) > 4:
        _need(p, 8 + 4 * p[5], "emarker unmatched")
        out.update({"sop": "SOP'" if p[4] == 1 else "SOP''" if p[4] == 2 else str(p[4]),
                    "header": int.from_bytes(p[6:8], "little"), "words": _words(p, 8, p[5])})
    return out


def _learn_call(link, payload, what):
    p = link.call(CMD_EM_LIB, bytes(payload)).payload
    _need(p, 1, what)
    if p[0]:   # every learn / unmatched answer starts with err (0 = Ok); a refusal is the lone err byte
        e = EM_ERRS[p[0]] if p[0] < len(EM_ERRS) else str(p[0])
        raise EmLibFailed(f"emarker {what}: {e}")
    return p


def _learn_entries(link, summary):
    return [decode_learn_entry(_learn_call(link, [6, EM_LEARN_ENTRY, i], "learn entry")) for i in range(summary["entries"])]


def cmd_emlearn(link, a):
    """Run (or read back) an e-marker learn: the cable in the BOTTOM port is powered and asked what a source asks."""
    if a.em_cmd == "learned":
        s = decode_learn_summary(_learn_call(link, [6, EM_LEARN_STATUS], "learn status"))
        return {"emlib": "learn", "ran": False, "summary": s, "entries": _learn_entries(link, s)}
    seeds = [parse_seed(t) for t in a.seed]
    opts = encode_learn_options(seeds, a.sweep, a.confirm)
    saved, link.timeout_s = getattr(link, "timeout_s", None), max(getattr(link, "timeout_s", 0) or 0, LEARN_TIMEOUT_S)
    try:
        s = decode_learn_summary(_learn_call(link, [6, EM_LEARN_RUN] + list(opts), "learn"))
    finally:
        if saved is not None:
            link.timeout_s = saved
    r = {"emlib": "learn", "ran": True, "summary": s, "entries": _learn_entries(link, s)}
    if a.save:
        if s["outcome"] != "learned":
            raise FcRefused(f"nothing to save: the run ended {s['outcome']}")
        p = _learn_call(link, [6, EM_LEARN_SAVE], "learn save")
        _need(p, 4, "emarker learn save")
        r["saved"] = {"seq": int.from_bytes(p[1:3], "little"), "existed": bool(p[3])}
    return r


def cmd_emunmatched(link, a):
    p = _learn_call(link, [EM_UNMATCHED, EM_SEEN_TOTALS], "unmatched")
    t = decode_unmatched(p)
    entries = []
    for i in range(t["held"]):
        entries.append(decode_unmatched(_learn_call(link, [EM_UNMATCHED, i], "unmatched")))
    return {"emlib": "unmatched", **t, "entries": entries}


def _words_text(ws):
    return " ".join(f"0x{w:08X}" for w in ws) if ws else "-"


def _learn_entry_text(e):
    req = e["request"]
    line = f"  #{e['i']:<2} {e['step']:<8} pd{'3' if e['spec_rev'] == 2 else '2'} req {_words_text(req)}  "
    if not e["got"]:
        return line + "-> (silent)"
    h = e["reply_header"]
    if e["reply"]:
        w0 = e["reply"][0]
        kind = VDM_TYPES[(w0 >> 6) & 3] if w0 & 0x8000 else "unstructured"
        return line + f"-> {kind} hdr=0x{h:04X} {_words_text(e['reply'])}"
    return line + f"-> control type {h & 0x1F} (hdr=0x{h:04X})"


def render_learn(r):
    s = r["summary"]
    if s["outcome"] == "none":
        return "no learn run yet"
    lines = [f"learn {s['outcome']}: vid=0x{s['vid']:04X} pid=0x{s['pid']:04X} svids={s['svids']} probes={s['probes']} "
             f"silent={s['silent']} rows={s['rows']} unmatched={s['unmatched_policy']}"
             + (" TRUNCATED (more answers than rows)" if s["truncated"] else "")
             + (" (transcript full)" if s["log_full"] else "")]
    lines += [_learn_entry_text(e) for e in r["entries"]]
    if r.get("saved"):
        lines.append(f"saved as id {r['saved']['seq']}" + (" (already saved)" if r["saved"]["existed"] else ""))
    return "\n".join(lines)


def render_unmatched(r):
    lines = [f"unmatched requests: {r['total']} seen, last {r['held']} held (newest first)"]
    for e in r["entries"]:
        lines.append(f"  {e['sop']} hdr=0x{e['header']:04X} {_words_text(e['words'])}")
    return "\n".join(lines)


CARD_ERRS = ["Ok", "Invalid", "NotFound", "Flash", "Full", "Busy"]
CARD_CHUNK = 48
# The newest payload the card store sends is the v8 one, 198 bytes (report_card.py owns the layout:
# 196 in v7, 190 in v6, ...; offset 196 is the re-balanced count + a spare). Deriving the chunk count from
# it keeps this reader from ever lagging the format again; the short last chunk still ends the read, so an
# older firmware's fewer chunks work too.
CARD_CHUNKS = (report_card.PAYLOAD + CARD_CHUNK - 1) // CARD_CHUNK   # 5: v8 198 B and v7 196 B (v6 190 B was 4)


class CardsFailed(Exception):
    pass


def _cards(link, payload, what):
    p = link.call(CMD_CARDS, bytes(payload)).payload
    _need(p, 1, what)
    if p[0]:
        e = CARD_ERRS[p[0]] if p[0] < len(CARD_ERRS) else str(p[0])
        raise CardsFailed(f"cards {what}: {e}" + (" (a charger test is running)" if e == "Busy" else ""))
    return p


def _card_numbers(link):
    out = []
    while True:
        p = _cards(link, [0, len(out)], "list")
        _need(p, 3, "cards list")
        total, n = p[1], p[2]
        _need(p, 3 + 2 * n, "cards list")
        out += [int.from_bytes(p[3 + 2 * i:5 + 2 * i], "little") for i in range(n)]
        if n == 0 or len(out) >= total:
            return out


def _card_get(link, no):
    raw = b""
    for k in range(CARD_CHUNKS):
        p = _cards(link, [1, no & 255, no >> 8, k], "get")
        _need(p, 2, "cards get")
        raw += bytes(p[2:])
        if len(p) - 2 < CARD_CHUNK:   # a short chunk is the last one: an older firmware has fewer chunks than CARD_CHUNKS
            break
    try:
        return report_card.decode(raw)
    except report_card.BadCard as e:
        raise CardsFailed(f"cards get: {e}") from None


def cmd_cards(link, a):
    """Charger report cards in the flash store; ids are the card numbers shown by list."""
    c = a.cards_cmd
    if c in ("list", "export"):
        cards = [_card_get(link, q) for q in _card_numbers(link)]
        if c == "list":
            return {"cards": "list", "list": cards}
        with open(a.out, "w") as f:
            f.write("[\n" + ",\n".join(report_card.dumps(x) for x in cards) + "\n]\n")
        return {"cards": "export", "file": a.out, "count": len(cards)}
    if c == "show":
        return {"cards": "show", "card": _card_get(link, a.id)}
    if c == "delete":
        if not a.confirm:
            raise FcRefused("delete removes a report card: add --confirm")
        _cards(link, [3, a.id & 255, a.id >> 8, 1], "delete")
        return {"cards": "delete", "no": a.id}
    if c == "rename":
        name = a.name.encode("ascii", errors="strict")
        if not 1 <= len(name) <= 20:
            raise FcRefused("name is 1..20 characters of A-Z a-z 0-9 space - _ .")
        _cards(link, [2, a.id & 255, a.id >> 8] + list(name), "rename")
        return {"cards": "rename", "no": a.id, "card": _card_get(link, a.id)}
    raise FcRefused(f"unknown cards command {c}")


def cmd_emarker(link, a):
    sc = getattr(a, "em_cmd", None)
    if sc in ("learn", "learned"):
        return cmd_emlearn(link, a)
    if sc == "unmatched":
        return cmd_emunmatched(link, a)
    if sc:
        return cmd_emlib(link, a)
    return cmd_emarker_data(link, a)


def cmd_emlib(link, a):
    """Saved-cable library: ids are the stable sequence numbers shown by list."""
    c = a.em_cmd
    if c == "list":
        seqs, start = [], 0
        while True:
            p = _emlib(link, [0, start], "list")
            _need(p, 3, "emarker list")
            total, n = p[1], p[2]
            _need(p, 3 + 2 * n, "emarker list")
            seqs += [int.from_bytes(p[3 + 2 * i:5 + 2 * i], "little") for i in range(n)]
            start += n
            if n == 0 or start >= total:
                break
        return {"emlib": "list", "cables": [_em_get(link, q) for q in seqs]}
    if c == "save":
        p = _emlib(link, [2], "save")
        _need(p, 4, "emarker save")
        r = {"emlib": "save", "existed": bool(p[3]), "seq": int.from_bytes(p[1:3], "little")}
        r["cable"] = _em_get(link, r["seq"])
        return r
    if c == "delete":
        if not a.confirm:
            raise FcRefused("delete removes a saved cable: add --confirm")
        _emlib(link, [3, a.id & 255, a.id >> 8, 1], "delete")
        return {"emlib": "delete", "seq": a.id}
    if c == "rename":
        name = a.name.encode("ascii", errors="strict")
        if not 1 <= len(name) <= 24:
            raise FcRefused("name is 1..24 characters of A-Z a-z 0-9 space - _ .")
        p = _emlib(link, [4, a.id & 255, a.id >> 8] + list(name), "rename")
        return {"emlib": "rename", "seq": a.id, "cable": _em_get(link, a.id)}
    if c == "spoof":
        t = a.target
        if t == "status":
            p = link.call(CMD_EM_LIB, bytes([5, 0])).payload
            _need(p, 1, "emarker spoof")
        elif t == "off":
            p = _emlib(link, [5, 2], "spoof")
        elif t in BUILTIN_NAMES or t.startswith(("builtin:", "built-in:")):
            bid = parse_builtin(t.partition(":")[2]) if ":" in t else BUILTIN_NAMES[t]
            if bid is None:
                raise FcRefused("built-in cable: builtin:0..4 or " + ", ".join(BUILTIN_NAMES))
            p = _emlib(link, [5, 3, bid], "spoof")
        else:
            try:
                q = int(t)
            except ValueError:
                raise FcRefused("spoof takes a saved-cable id, builtin:<id|name> (" + ", ".join(BUILTIN_NAMES) +
                                "), off or status") from None
            p = _emlib(link, [5, 1, q & 255, q >> 8], "spoof")
        return {"emlib": "spoof", **_em_status(p)}
    raise FcRefused(f"unknown emarker command {c}")


def cmd_emarker_data(link, a):
    p0 = link.call(CMD_GET_EMARKER, bytes([0])).payload
    p1 = link.call(CMD_GET_EMARKER, bytes([1])).payload
    p2 = link.call(CMD_GET_EMARKER, bytes([2])).payload
    out = decode_emarker(p0, p1, p2)
    link_f = _link_fields(link, ("cable_scanned", "cable_stale", "cable_age_s"))
    if link_f:
        out.update({"stale": link_f["cable_stale"], "age_s": link_f["cable_age_s"],
                    "scanned": link_f["cable_scanned"]})
    return out


def cmd_identity(link, a):
    out = decode_identity(link.call(CMD_GET_IDENTITY).payload)
    out.update(_link_fields(link, ("stale", "waiting", "age_s")))
    return out


def rle565_decode(data, expected):
    """Decode the firmware's RLE565 stream into a list of RGB565 values (see rle565.hpp)."""
    out, i, n = [], 0, len(data)
    while i < n:
        t = data[i]
        i += 1
        if t < 0x80:
            if i + 2 > n:
                raise FrameError("truncated run token")
            out.extend([data[i] | (data[i + 1] << 8)] * (t + 1))
            i += 2
        else:
            cnt = t - 0x7F
            if i + 2 * cnt > n:
                raise FrameError("truncated literal token")
            out.extend(data[i + 2 * k] | (data[i + 2 * k + 1] << 8) for k in range(cnt))
            i += 2 * cnt
    if len(out) != expected:
        raise FrameError(f"strip decodes to {len(out)} pixels, expected {expected}")
    return out


def decode_screen_info(p):
    _need(p, 7, "screen info")
    w, h, rows, strips, fmt = struct.unpack("<HHBBB", p[:7])
    if fmt != SHOT_FORMAT_RLE565:
        raise FrameError(f"unsupported screenshot format {fmt}")
    if rows * strips != h:
        raise FrameError(f"strips {strips} x {rows} rows do not cover height {h}")
    return {"width": w, "height": h, "strip_rows": rows, "num_strips": strips}


def rgb565_to_rgb888(px):
    out = bytearray(len(px) * 3)
    for i, v in enumerate(px):
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        out[3 * i] = (r << 3) | (r >> 2)
        out[3 * i + 1] = (g << 2) | (g >> 4)
        out[3 * i + 2] = (b << 3) | (b >> 2)
    return bytes(out)


def scale_rgb(rgb, w, h, n):
    if n == 1:
        return rgb, w, h
    rows = []
    for y in range(h):
        base = y * w * 3
        row = b"".join(rgb[base + 3 * x:base + 3 * x + 3] * n for x in range(w))
        rows.extend([row] * n)
    return b"".join(rows), w * n, h * n


def png_bytes(rgb, w, h):
    """Minimal 8-bit RGB PNG, no Pillow needed."""
    def chunk(tag, body):
        c = tag + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def assemble_screenshot(info, strips):
    """strips: {index: [rgb565, ...]} -> RGB565 list of the full screen."""
    px = []
    for i in range(info["num_strips"]):
        if i not in strips:
            raise FrameError(f"strip {i} missing")
        px.extend(strips[i])
    return px


def receive_screenshot(link, a, out):
    rsp = link.call(CMD_SCREENSHOT)
    info = decode_screen_info(rsp.payload)
    per_strip = info["width"] * info["strip_rows"]
    strips, buf, total, stats = {}, {}, 0, []
    while len(strips) < info["num_strips"]:
        f = link.read_stream(rsp.seq)
        if len(f.payload) < 2:
            raise FrameError("short stream frame")
        idx, flags, data = f.payload[0], f.payload[1], f.payload[2:]
        if flags & SHOT_FLAG_ABORT:
            raise RemoteError(STATUS_BUSY, CMD_SCREENSHOT)
        if idx >= info["num_strips"]:
            raise FrameError(f"strip index {idx} out of range")
        buf[idx] = buf.get(idx, b"") + data
        total += len(data)
        if flags & SHOT_FLAG_LAST:
            enc = buf.pop(idx)
            strips[idx] = rle565_decode(enc, per_strip)
            stats.append({"strip": idx, "pixels": len(strips[idx]), "colours": len(set(strips[idx])),
                          "bytes": len(enc)})
            if a.debug:
                out(f"[strip {idx:2}] {len(strips[idx])} px, {len(set(strips[idx]))} colours, {len(enc)} B")
    return info, strips, total, stats


def cmd_screenshot(link, a, out=print):
    t0 = time.monotonic()
    raw_log = [] if a.dump_raw else None
    link.tap = raw_log.append if raw_log is not None else None
    try:
        info, strips, total, stats = receive_screenshot(link, a, out)
    finally:
        link.tap = None
        if raw_log is not None:                  # written even when decoding failed
            Path(a.dump_raw).write_bytes(b"".join(raw_log))
    elapsed = time.monotonic() - t0
    w, h = info["width"], info["height"]
    rgb, ow, oh = scale_rgb(rgb565_to_rgb888(assemble_screenshot(info, strips)), w, h, a.scale)
    try:
        from PIL import Image
        import io
        bio = io.BytesIO()
        Image.frombytes("RGB", (ow, oh), rgb).save(bio, "PNG")
        png = bio.getvalue()
    except ImportError:
        png = png_bytes(rgb, ow, oh)
    Path(a.out).write_bytes(png)
    return {"file": a.out, "width": ow, "height": oh, "rle_bytes": total,
            "ms": round(elapsed * 1000), "strip_stats": stats}


def cmd_adc_raw(link, a):
    return decode_adc_raw(link.call(CMD_ADC_RAW).payload)


def ripple_png(samples, full_scale=RIPPLE_FULL_SCALE, height=256):
    """Plot of the capture: one column per sample, dark background, trace joined between samples."""
    w = len(samples)
    px = bytearray(b"\x14\x14\x1e" * (w * height))
    ys = [height - 1 - min(height - 1, v * (height - 1) // full_scale) for v in samples]
    for gy in range(0, height, height // 4):     # quarter-scale grid
        for x in range(w):
            i = (gy * w + x) * 3
            px[i:i + 3] = b"\x30\x30\x44"
    for x in range(w):
        lo, hi = sorted((ys[x], ys[x - 1] if x else ys[x]))
        for y in range(lo, hi + 1):
            i = (y * w + x) * 3
            px[i:i + 3] = b"\xff\xd0\x30"
    return png_bytes(bytes(px), w, height)


def cmd_ripple(link, a):
    payload = b"" if a.code is None else bytes([a.code])
    rsp = link.call(CMD_RIPPLE, payload)
    info = decode_ripple_info(rsp.payload)
    n = info["count"]
    samples, got = [None] * n, 0
    while got < n:
        f = link.read_stream(rsp.seq, timeout_s=max(2 * link.timeout_s, 0.05), cmd=CMD_RIPPLE_DATA,
                             what="ripple")
        _need(f.payload, 2, "ripple data")
        off = int.from_bytes(f.payload[:2], "little")
        data = f.payload[2:]
        for i in range(len(data) // 2):
            if off + i >= n:
                raise FrameError("ripple sample index out of range")
            if samples[off + i] is None:
                got += 1
            samples[off + i] = int.from_bytes(data[2 * i:2 * i + 2], "little")
    info["samples"] = samples
    if a.png:
        Path(a.png).write_bytes(ripple_png(samples))
        info["png"] = a.png
    return info


class FcRefused(Exception):
    """A request above 5 V or a scan without --confirm; nothing was sent."""


def decode_fc(p):
    _need(p, 10, "fc")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    n = p[9]
    _need(p, 10 + 4 * n, "fc results")
    name = lambda table, i: table[i] if i < len(table) else str(i)
    results = [{"proto": name(FC_PROTOS, p[10 + 4 * i]).upper(), "outcome": name(FC_OUTCOMES, p[11 + 4 * i]),
                "mv": u16(12 + 4 * i)} for i in range(n)]
    return {"state": name(FC_STATES, p[0]), "proto": name(FC_PROTOS, p[1]).upper(), "fail": name(FC_FAILS, p[2]),
            "target_mv": u16(5), "vbus_mv": u16(7),
            "scan": {"running": bool(p[3] & 1), "done": bool(p[3] & 2), "step": p[4], "results": results}}


def cmd_fc(link, a):
    if a.fc_cmd == "status":
        payload = bytes([FC_OP_STATUS])
    elif a.fc_cmd == "reset":
        payload = bytes([FC_OP_RESET])
    elif a.fc_cmd == "scan":
        if not a.confirm:
            raise FcRefused("the scan goes up to 9 V on the output: add --confirm "
                            "(known charger, nothing valuable attached)")
        payload = bytes([FC_OP_SCAN, FC_CONFIRM | (FC_ALLOW_INLINE if a.allow_inline else 0)])
    else:
        mv = round(a.volts * 1000)
        if not 0 < mv <= 0xFFFF:
            raise FcRefused(f"bad voltage {a.volts}")
        if mv > FC_SAFE_MV and not a.confirm:
            raise FcRefused(f"{a.volts:g} V is above 5 V: add --confirm "
                            "(known charger, nothing valuable attached)")
        payload = bytes([FC_OP_TRIGGER, FC_PROTOS.index(a.proto)]) + mv.to_bytes(2, "little") + bytes([(FC_CONFIRM if a.confirm else 0) | (FC_ALLOW_INLINE if a.allow_inline else 0)])
    try:
        return decode_fc(link.call(CMD_FC, payload).payload)
    except RemoteError as e:
        # A refusal reads BadArg; the status call names the reason (e.g. "PD or load active").
        if a.fc_cmd in ("scan", "trigger"):
            why = decode_fc(link.call(CMD_FC, bytes([FC_OP_STATUS])).payload)["fail"]
            if why != "none":
                raise FcRefused(f"{why}" + (" (a PD contract or load is behind the K2: add --allow-inline "
                                            "only if you accept the risk)" if why == "PD or load active" else "")) from e
        raise


STORE_ERRS = ["Ok", "Full", "BadSlot", "Empty", "Invalid", "TooBig", "Foreign", "FlashRefused", "FlashBusy",
              "FlashKey", "FlashProgram", "FlashWriteProtect", "FlashVerify"]   # k2::store::Err
class StoreFailed(Exception):
    pass


def cmd_store(link, a):
    """Explicit flash-store actions; the K2 only writes flash when asked here."""
    payload = bytes([{"list": 0, "save-emarker": 1, "save-log": 3}[a.store_cmd]])
    p = link.call(CMD_STORE, payload).payload
    _need(p, 8, "store")
    err = p[0]
    r = {"err": STORE_ERRS[err] if err < len(STORE_ERRS) else str(err), "count": p[1],
         "native_slot": bool(p[2] & 1), "foreign": bool(p[2] & 2), "saved_log": bool(p[2] & 4),
         "slot": p[3], "log_len": int.from_bytes(p[4:6], "little"), "bank": p[6], "native_records": p[7], "action": a.store_cmd}
    if err:
        raise StoreFailed(f"store {a.store_cmd}: {r['err']}")
    return r


def cmd_cable_r(link, a):
    op = {"reset": CABLE_OP_RESET, "passive": CABLE_OP_PASSIVE, "kelvin": CABLE_OP_KELVIN}.get(a.cable_cmd, CABLE_OP_STATUS)
    return decode_cable_r(link.call(CMD_CABLE_R, bytes([op])).payload)


def cmd_qrtest(link, a):
    """The QR scan-test rig (Cmd::QrTest): list the fit table, stop the test screen, or show one full-screen test QR."""
    args = a.args
    if args[0] == "list":
        if len(args) != 1:
            raise SystemExit("qrtest list takes no arguments")
        chunks, off, total = [], 0, None
        while total is None or off < total:
            p = link.call(CMD_QRTEST, bytes([QRTEST_LIST]) + off.to_bytes(2, "little")).payload
            _need(p, 5, "qrtest list")
            n = p[4]
            _need(p, 5 + n, "qrtest list")
            if int.from_bytes(p[0:2], "little") != off:
                raise FrameError("qrtest list: the reply answers a different window; the firmware does not match this tool")
            if total is not None and int.from_bytes(p[2:4], "little") != total:
                raise FrameError("qrtest list: the table length changed between chunks; run it again")
            total = int.from_bytes(p[2:4], "little")
            chunks.append(bytes(p[5:5 + n]))
            off += n
            if n == 0:
                break
        text = b"".join(chunks).decode("ascii")
        if len(text) != total:
            raise FrameError(f"qrtest list: got {len(text)} of {total} bytes")
        return {"qrtest_list": text}
    if args[0] == "stop":
        if len(args) != 1:
            raise SystemExit("qrtest stop takes no arguments")
        p = link.call(CMD_QRTEST, bytes([QRTEST_STOP])).payload
        _need(p, 1, "qrtest stop")
        return {"qrtest_stop": bool(p[0])}
    if len(args) not in (2, 3):
        raise SystemExit("usage: qrtest list | qrtest stop | qrtest VERSION PX [QUIET]")
    try:
        version, px = int(args[0]), int(args[1])
        quiet = int(args[2]) if len(args) == 3 else 4
    except ValueError:
        raise SystemExit("qrtest: VERSION, PX and QUIET are integers")
    if not (1 <= version <= 40 and 2 <= px <= 3 and 1 <= quiet <= 4):
        raise SystemExit("qrtest: version 1..40, px 2..3, quiet 1..4 (the K2 also checks the box fit and answers the reason)")
    p = link.call(CMD_QRTEST, bytes([QRTEST_SHOW, version, px, quiet])).payload
    _need(p, 3, "qrtest show")
    return {"qrtest_show": {"version": p[0], "px": p[1], "quiet": p[2]}}


def _name_of(table, i):
    return table[i] if i < len(table) else i


def ufcs_status(link):
    """Status block of CMD_UFCS: state, last verdict / class, the last probe start refusal, blob length."""
    return decode_ufcs_status(link.call(CMD_UFCS, bytes([UFCS_STATUS])).payload)


def decode_ufcs_status(p):
    _need(p, 7, "ufcs status")
    return {"state": _name_of(UFCS_STATES, p[0]), "verdict": _name_of(UFCS_VERDICTS, p[1]),
            "class": _name_of(UFCS_CLASSES, p[2]), "start": _name_of(UFCS_STARTS, p[3]),
            "blob_len": p[4] | p[5] << 8, "fields": p[6]}


def decode_ufcs_field(p):
    _need(p, 17, "ufcs field")
    u = lambda o: int.from_bytes(p[o:o + 4], "little")
    name = UFCS_FIELDS[p[0]][0] if p[0] < len(UFCS_FIELDS) else p[0]
    return {"field": name, "value": u(1), "lo": u(5), "hi": u(9), "default": u(13)}


def ufcs_field_index(name):
    for i, f in enumerate(UFCS_FIELDS):
        if f[0] == name:
            return i
    raise SystemExit(f"unknown ufcs field {name!r}; fields: " + " ".join(f[0] for f in UFCS_FIELDS))


def ufcs_set(link, name, value):
    return decode_ufcs_field(link.call(CMD_UFCS, bytes([UFCS_SET, ufcs_field_index(name)]) + int(value).to_bytes(4, "little")).payload)


def ufcs_get(link, name):
    return decode_ufcs_field(link.call(CMD_UFCS, bytes([UFCS_GET, ufcs_field_index(name)])).payload)


def ufcs_read_blob(link):
    """The probe result blob, read in kUfcsChunk pieces."""
    out = bytearray()
    total = None
    while total is None or len(out) < total:
        p = link.call(CMD_UFCS, bytes([UFCS_READ]) + len(out).to_bytes(2, "little")).payload
        _need(p, 5, "ufcs read")
        total = p[2] | p[3] << 8
        n = p[4]
        _need(p, 5 + n, "ufcs read")
        if n == 0:
            break
        out += p[5:5 + n]
    return bytes(out)


UFCS_BLOB_HEADER = 38


def decode_ufcs_blob(b):
    """Probe result blob (k2/fastcharge/ufcs_probe.hpp): header, then events {ms u16, kind u8 (0 TX, 1 RX), n u8, bytes}."""
    _need(b, UFCS_BLOB_HEADER, "ufcs blob")
    u = lambda o: int.from_bytes(b[o:o + 2], "little")
    s = lambda o: int.from_bytes(b[o:o + 2], "little", signed=True)
    r = {"version": b[0], "state": _name_of(UFCS_STATES, b[1]), "verdict": _name_of(UFCS_VERDICTS, b[2]),
         "class": _name_of(UFCS_CLASSES, b[3]),
         "hs_us": {"rise1": s(4), "fall1": s(6), "rise2": s(8), "fall2": s(10), "final": s(12)},
         "tx_frames": b[14], "rx_frames": b[15], "rx_bytes": u(16),
         "errors": {"framing": u(18), "noise": u(20), "overrun": u(22), "parity": u(24)},
         "vbus_mv": {"start": u(26), "min": u(28), "max": u(30)}, "total_ms": u(32), "log_bytes": u(34),
         "truncated": bool(b[36] & 1), "events": []}
    off = UFCS_BLOB_HEADER
    end = min(len(b), UFCS_BLOB_HEADER + r["log_bytes"])
    while off + 4 <= end:
        n = b[off + 3]
        if off + 4 + n > end:
            break
        r["events"].append({"ms": u(off), "dir": "tx" if b[off + 2] == 0 else "rx", "hex": b[off + 4:off + 4 + n].hex()})
        off += 4 + n
    return r


def render_ufcs(r):
    if "ufcs" in r:
        s = r["ufcs"]
        return (f"ufcs probe {s['state']}, verdict {s['verdict']}, class {s['class']}, last start {s['start']}, "
                f"blob {s['blob_len']} bytes")
    if "ufcs_field" in r:
        f = r["ufcs_field"]
        return f"{f['field']} = {f['value']} (default {f['default']}, range {f['lo']}..{f['hi']})"
    if "ufcs_fields" in r:
        return "\n".join(f"{f['field']:<10} {f['value']:>7}  default {f['default']:>7}  range {f['lo']}..{f['hi']}"
                         for f in r["ufcs_fields"])
    x = r["ufcs_result"]
    h, e = x["hs_us"], x["errors"]
    lines = [f"verdict {x['verdict']}, handshake {x['class']} (rise1 {h['rise1']} fall1 {h['fall1']} rise2 {h['rise2']} "
             f"fall2 {h['fall2']} final {h['final']} us), VBUS {x['vbus_mv']['start']}/{x['vbus_mv']['min']}/{x['vbus_mv']['max']} mV, "
             f"{x['total_ms']} ms",
             f"tx {x['tx_frames']} frames, rx {x['rx_bytes']} bytes in {x['rx_frames']} frames, errors framing {e['framing']} "
             f"noise {e['noise']} overrun {e['overrun']} parity {e['parity']}" + (" (log truncated)" if x["truncated"] else "")]
    lines += [f"  {v['ms']:>5} ms {v['dir']} {v['hex']}" for v in x["events"]]
    return "\n".join(lines)


def cmd_ufcs(link, a):
    c = a.ufcs_cmd
    if c == "status":
        return {"ufcs": ufcs_status(link)}
    if c == "show":
        return {"ufcs_fields": [ufcs_get(link, f[0]) for f in UFCS_FIELDS]}
    if c == "get":
        return {"ufcs_field": ufcs_get(link, a.field)}
    if c == "set":
        return {"ufcs_field": ufcs_set(link, a.field, a.value)}
    if c in ("reset", "probe", "stop"):
        op = {"reset": UFCS_RESET, "probe": UFCS_PROBE, "stop": UFCS_STOP}[c]
        return {"ufcs": decode_ufcs_status(link.call(CMD_UFCS, bytes([op])).payload)}
    if c == "result":
        return {"ufcs_result": decode_ufcs_blob(ufcs_read_blob(link))}
    raise SystemExit("unknown ufcs command")


LAB_REASONS = ["none", "timeout", "vbus_low", "vbus_high", "mcu_hot", "external", "done"]


def decode_lab(p):
    _need(p, 17, "lab")
    sg = lambda o: int.from_bytes(p[o:o + 2], "little", signed=True)
    u = lambda o: int.from_bytes(p[o:o + 2], "little")
    return {"reason": LAB_REASONS[p[0]] if p[0] < len(LAB_REASONS) else p[0], "duty": p[1], "ms": u(2),
            "idle_ma": sg(4), "peak_ma": sg(6), "min_vbus_mv": u(8), "vbus_before_mv": u(10),
            "temp_before_c": sg(12) / 10, "temp_after_c": sg(14) / 10, "pin_high_pct": p[16]}


SNIFF_END = {0: "none", 1: "full", 2: "idle", 3: "detach", 4: "timeout", 5: "cancel"}


def cmd_lab(link, a):
    if a.lab_cmd == "load":
        args = bytes([0, a.duty]) + a.ms.to_bytes(2, "little") + bytes([1 if a.confirm else 0])
        return decode_lab(link.call(CMD_LAB, args).payload)
    if a.lab_cmd == "sniff":
        sub = {"cancel": 0, "arm": 1, "status": 2, "selftest": 3}[a.sniff_cmd]
        args = bytes([1, sub]) + (bytes([1]) if sub == 3 and a.confirm else b"")
        p = link.call(CMD_LAB, args).payload
        _need(p, 5, "lab sniff")
        return {"state": ["idle", "armed", "recording"][min(p[0], 2)], "reason": SNIFF_END.get(p[1], p[1]),
                "overflow": bool(p[2] & 1), "stored": bool(p[2] & 2), "edges": p[3] | p[4] << 8}
    raise SystemExit("unknown lab command")


def blackbox_call(link, op, arg=None):
    """One BLACKBOX request. Returns (flags, next_cursor, head, records bytes); flags bit0 = the cursor
    was overwritten, bit1 = frozen, bit2 = the cursor was beyond head (device rebooted)."""
    payload = bytes([op])
    if op == BB_READ and arg is not None:
        payload += struct.pack("<I", arg & 0xFFFFFFFF)
    elif op == BB_FREEZE:
        payload += bytes([1 if arg else 0])
    p = link.call(CMD_BLACK_BOX, payload).payload
    _need(p, 9, "blackbox")
    return p[0], struct.unpack_from("<I", p, 1)[0], struct.unpack_from("<I", p, 5)[0], bytes(p[9:])


CAP_TRIGGERS = {0: "empty", 1: "contract", 2: "hard_reset", 3: "vbus_dip", 4: "detach", 5: "edges"}


def blackbox_captures(link):
    """The trigger-frozen captures: [{slot, trigger, bytes, age_ms}], empty slots omitted."""
    p = link.call(CMD_BLACK_BOX, bytes([BB_CAP_LIST])).payload
    _need(p, 4 + 3 * 7, "blackbox captures")
    out = []
    for k in range(3):
        trig, n, age = struct.unpack_from("<BHI", p, 4 + 7 * k)
        if trig:
            out.append({"slot": k, "trigger": CAP_TRIGGERS.get(trig, str(trig)), "bytes": n, "age_ms": age})
    return out


def blackbox_capture_read(link, slot):
    """All record bytes of one capture slot. -> (bytes, still_recording)"""
    data, off = bytearray(), 0
    while True:
        p = link.call(CMD_BLACK_BOX, bytes([BB_CAP_READ, slot]) + struct.pack("<H", off)).payload
        _need(p, 9, "blackbox capture")
        flags, nxt, ln = p[0], struct.unpack_from("<I", p, 1)[0], struct.unpack_from("<I", p, 5)[0]
        data += p[9:]
        if nxt == off or nxt >= ln:
            return bytes(data), bool(flags & 8)
        off = nxt


def cmd_blackbox(link, a):
    if a.bb_cmd == "captures":
        return blackbox_captures(link)
    if a.bb_cmd == "clear-captures":
        link.call(CMD_BLACK_BOX, bytes([BB_CAP_CLEAR]))
        return {"cleared": True}
    op, arg = {"status": (BB_STATUS, None), "clear": (BB_CLEAR, None),
               "freeze": (BB_FREEZE, 1), "resume": (BB_FREEZE, 0)}[a.bb_cmd]
    flags, oldest, head, _ = blackbox_call(link, op, arg)
    return {"frozen": bool(flags & 2), "oldest": oldest, "head": head, "used": head - oldest}


TC_STATES = ["off", "armed", "post", "frozen"]
TC_REASONS = ["none", "manual", "shutoff", "no-psrdy", "hardreset", "detach", "timeout", "voltage", "other"]
TC_ARM_RESULTS = ["ok", "busy", "no-monitor"]


def _tc_name(table, i):
    return table[i] if i < len(table) else str(i)


def transcap_status(link):
    p = link.call(CMD_TRANS_CAP, bytes([TC_STATUS])).payload
    _need(p, 26, "transcap status")
    samples, = struct.unpack_from("<H", p, 3)
    lost, span, gap, age, seq = struct.unpack_from("<IIIII", p, 6)
    return {"state": _tc_name(TC_STATES, p[0]), "reason": _tc_name(TC_REASONS, p[1]), "fast": bool(p[2] & 1),
            "keep": bool(p[2] & 2), "samples": samples, "markers": p[5], "lost": lost, "span_us": span,
            "max_gap_us": gap, "trigger_age_ms": age, "saved_seq": seq}


def transcap_list(link):
    """The stored captures, newest first: [{seq, run_id, step, reason}]."""
    p = link.call(CMD_TRANS_CAP, bytes([TC_LIST])).payload
    _need(p, 1, "transcap list")
    _need(p, 1 + 11 * p[0], "transcap list")
    out = []
    for i in range(p[0]):
        seq, run_id, step, reason = struct.unpack_from("<IIHB", p, 1 + 11 * i)
        out.append({"seq": seq, "run_id": run_id, "step": step, "reason": _tc_name(TC_REASONS, reason)})
    return out


def transcap_read(link, seq=None):
    """The whole encoded stream: the live frozen capture (seq None) or a stored one. Chunks of up to 58 bytes."""
    data = bytearray()
    while True:
        if seq is None:
            req = bytes([TC_LIVE]) + struct.pack("<I", len(data))
        else:
            req = bytes([TC_STORED]) + struct.pack("<II", seq, len(data))
        chunk = link.call(CMD_TRANS_CAP, req).payload
        if not chunk:
            return bytes(data)
        data += chunk


def cmd_transcap(link, a):
    c = a.tc_cmd
    if c == "status":
        return transcap_status(link)
    if c == "list":
        return transcap_list(link)
    if c == "arm":
        p = link.call(CMD_TRANS_CAP, bytes([TC_ARM])).payload
        _need(p, 1, "transcap arm")
        return {"armed": p[0] == 0, "result": _tc_name(TC_ARM_RESULTS, p[0])}
    if c == "trigger":
        p = link.call(CMD_TRANS_CAP, bytes([TC_TRIGGER])).payload
        _need(p, 1, "transcap trigger")
        return {"triggered": bool(p[0])}
    if c == "save":
        p = link.call(CMD_TRANS_CAP, bytes([TC_SAVE])).payload
        _need(p, 4, "transcap save")
        return {"seq": struct.unpack_from("<I", p)[0]}
    op = {"disarm": TC_DISARM, "clear": TC_CLEAR}[c]
    link.call(CMD_TRANS_CAP, bytes([op]))
    return {c: True}


def cmd_action(link, a):
    saved, link.timeout_s = link.timeout_s, max(link.timeout_s, ACTION_TIMEOUT_S)
    try:
        link.call(CMD_ACTION, bytes([ACTIONS[a.name], 1 if getattr(a, "confirm", False) else 0]))
    finally:
        link.timeout_s = saved
    return {"action": a.name}


def cmd_pd(link, a):
    """Active PD. `pd request` above 5 V needs --confirm; inline the K2 refuses all of them
    (--over-load lifts the refusal only while the far sink is a passive load, lab only)."""
    sub = a.pd_cmd
    if sub == "request":
        mv, ma = getattr(a, "mv", 0) or 0, getattr(a, "ma", 0) or 0
        flags = (1 if getattr(a, "confirm", False) else 0) | (2 if getattr(a, "over_load", False) else 0)
        link.call(CMD_PD_REQUEST, bytes([a.pos, flags]) + mv.to_bytes(2, "little") + ma.to_bytes(2, "little"))
    elif sub == "reset":
        link.call(CMD_PD_RESET)
    elif sub == "release":
        link.call(CMD_PD_RELEASE)
    elif sub == "attach":
        mv = getattr(a, "mv", 9000)
        ma = getattr(a, "ma", 0)
        # Clamp to 36000 mV for safety
        if mv > 36000:
            mv = 36000
        payload = mv.to_bytes(2, "little")
        if ma > 0:
            payload += ma.to_bytes(2, "little")
        st = decode_pd_trigger(link.call(CMD_PD_ATTACH, payload).payload)
        return {"pd_cmd": sub, "target_mv": mv, "target_ma": ma, **st}
    st = decode_pd_trigger(link.call(CMD_PD_TRIGGER_STATUS).payload)
    return {"pd_cmd": sub, **st}


def cmd_identify_device(link, a, sleep=time.sleep, clock=time.monotonic):
    """Starts the Identify sequence (idle CC only), waits for it, then reads the identity."""
    op = 2 if getattr(a, "confirm", False) else 1
    prog = decode_identify_progress(link.call(CMD_IDENTIFY_DEVICE, bytes([op])).payload)
    deadline = clock() + getattr(a, "wait", ID_POLL_S)
    while prog["state"] in ("running", "idle") and clock() < deadline:
        sleep(0.1)
        prog = decode_identify_progress(link.call(CMD_IDENTIFY_DEVICE, bytes([0])).payload)
    out = {"identify": prog}
    out["identity"] = decode_identity(link.call(CMD_GET_IDENTITY).payload)
    return out


def page_index(text):
    """Page to show, by name or index: a rotation page, `connection`/`diag`, or a Connection tab
    (`source` `cable` `sink`; the old names `pd` `emarker` `identify` map to the tabs). ValueError otherwise."""
    t = str(text).strip().lower()
    if t in PAGES[:ROTATION_PAGES]:
        return PAGES.index(t)
    if t in CONN_ALIASES:
        return CONN_ALIASES[t]
    if t in OPENABLE_MENUS:
        return PAGES.index(t)
    if t.isdigit() and int(t) < ROTATION_PAGES:
        return int(t)
    names = list(PAGES[:ROTATION_PAGES]) + list(OPENABLE_MENUS) + list(CONN_TABS)
    raise ValueError(f"page must be one of {', '.join(names)} (old names: pd emarker identify) or 0..{ROTATION_PAGES - 1}")


def _page_arg(text):
    try:
        return page_index(text)
    except ValueError as e:
        raise argparse.ArgumentTypeError(str(e))


def cmd_page(link, a):
    link.call(CMD_SET_PAGE, bytes([a.page]))
    return {"page": a.page, "page_name": page_name(a.page)}


def parse_pd_msg(text, sop_name="sop"):
    """`text` = hex of a PD message as on the wire: 2 header bytes then the data objects (an optional
    0xE0/0xC0/0xA0 SOP token first is dropped when --sop is not given). Raises ValueError."""
    try:
        raw = bytes.fromhex(text.replace(" ", "").replace(":", "").replace("0x", ""))
    except ValueError as e:
        raise ValueError(f"not hex: {e}") from None
    if len(raw) < 2:
        raise ValueError("need at least the 2 header bytes")
    header = raw[:2]
    objects = raw[2:]
    n = (int.from_bytes(header, "little") >> 12) & 7
    if len(objects) != 4 * n:
        raise ValueError(f"header says {n} data objects ({4 * n} bytes), got {len(objects)} bytes")
    return SOPS[sop_name], header, objects


def cmd_debug(link, a):
    if a.debug_cmd == "pdos":
        link.call(CMD_DEBUG_PDOS, bytes([a.fake]))
        return {"fake_pdos": a.fake}
    if a.debug_cmd == "pd-msg":
        sop, header, objects = parse_pd_msg(a.hex, a.sop)
        link.call(CMD_DEBUG_PD_MSG, bytes([sop]) + header + objects)
        return {"sop": sop, "header": int.from_bytes(header, "little"), "objects": len(objects) // 4}
    if a.debug_cmd in ("detach", "attach"):
        link.call(CMD_DEBUG_LINK, bytes([1 if a.debug_cmd == "attach" else 0]))
        return {"link": a.debug_cmd}
    if a.debug_cmd == "cc-matrix":
        payload = bytes([a.v]) + (a.modes.to_bytes(2, "little") if a.modes is not None else b"")
        link.call(CMD_DEBUG_CC_MATRIX, payload)
        return {"cc_matrix": a.v, "cc_modes": a.modes}
    if a.debug_cmd == "vconn":
        p = link.call(CMD_DEBUG_VCONN, bytes([{"status": 0, "swap": 1, "off": 2, "roundtrip": 3, "data-reset": 4}[a.op]])).payload
        st = ["off", "swap-sent", "settling", "on", "releasing", "failed", "give-sent", "give-wait"]
        why = ["none", "accepted", "rejected", "wait", "not-supported", "timeout", "released-by-partner", "refused", "abort"]
        return {"vconn": {"ok": bool(p[0]), "state": st[p[1]] if p[1] < len(st) else p[1],
                          "why": why[p[2]] if p[2] < len(why) else p[2], "pin_on": bool(p[3]), "swaps_rx": p[4],
                          "active_cc": p[5], "cc1_mv": p[6] | p[7] << 8, "cc2_mv": p[8] | p[9] << 8}}
    if a.debug_cmd == "epr-info":
        op = {"status": 0, "ask": 1, "force": 2}[a.op]
        return {"epr_info": decode_epr_info(link.call(CMD_DEBUG_EPR_INFO, bytes([op])).payload)}
    if a.debug_cmd == "lcd":
        return {"lcd": decode_lcd(link.call(CMD_DEBUG_LCD).payload)}
    if a.debug_cmd == "cc-probe":
        payload = bytes([a.sw0, a.pc]) + a.ms.to_bytes(2, "little") + (a.modes.to_bytes(2, "little") if a.modes is not None else b"")
        saved, link.timeout_s = link.timeout_s, max(link.timeout_s, a.ms / 1000.0 + 2.0)   # the K2 blocks for the hold
        try:
            p = link.call(CMD_DEBUG_CC_PROBE, payload).payload
        finally:
            link.timeout_s = saved
        return {"cc_probe": decode_cc_probe(p), "cc_probe_args": {"sw0": a.sw0, "pc": a.pc, "ms": a.ms, "modes": a.modes}}
    raise ValueError(a.debug_cmd)


def decode_cc_probe(p):
    """DebugCcProbe reply: flags n sw0 sw1 pc modes(u16), then n x [t_us u32, cc1_raw u16, cc2_raw u16, vbus_mv u16,
    st0_cc1, st0_cc2]. raw 0xFFFF / vbus 0xFFFF = no reading. st0: BC_LVL bits 1:0, COMP bit 5, VBUSOK bit 7."""
    _need(p, 7, "cc-probe")
    flags, n, sw0, sw1, pc = p[0], p[1], p[2], p[3], p[4]
    modes = p[5] | p[6] << 8
    _need(p, 7 + 12 * n, "cc-probe samples")
    samples = []
    for k in range(n):
        t_us, cc1, cc2, vbus = struct.unpack_from("<IHHH", p, 7 + 12 * k)
        st1, st2 = p[7 + 12 * k + 10], p[7 + 12 * k + 11]
        samples.append({
            "t_us": t_us,
            "cc1_raw": None if cc1 == 0xFFFF else cc1, "cc2_raw": None if cc2 == 0xFFFF else cc2,
            "cc1_mv": None if cc1 == 0xFFFF else cc1 * 3300 // 4096, "cc2_mv": None if cc2 == 0xFFFF else cc2 * 3300 // 4096,
            "vbus_mv": None if vbus == 0xFFFF else vbus,
            "meas_cc1": {"bc_lvl": st1 & 3, "comp": bool(st1 & 0x20), "vbusok": bool(st1 & 0x80)},
            "meas_cc2": {"bc_lvl": st2 & 3, "comp": bool(st2 & 0x20), "vbusok": bool(st2 & 0x80)},
        })
    return {"aborted": bool(flags & 1), "restored": bool(flags & 2),
            "snapshot": {"sw0": f"0x{sw0:02X}", "sw1": f"0x{sw1:02X}", "pc": pc, "modes": f"0x{modes:03X}"},
            "samples": samples}


def render_cc_probe(r):
    a, p = r["cc_probe_args"], r["cc_probe"]
    head = (f"cc-probe sw0=0x{a['sw0']:02X} pc={a['pc']} ms={a['ms']}: "
            + ("ABORTED, VBUS rose; " if p["aborted"] else "") + ("restored" if p["restored"] else "RESTORE NOT CONFIRMED")
            + f" (was sw0={p['snapshot']['sw0']} sw1={p['snapshot']['sw1']} pc={p['snapshot']['pc']} modes={p['snapshot']['modes']})")

    def mv(v):
        return "-" if v is None else f"{v}"
    rows = []
    for s in p["samples"]:
        m1, m2 = s["meas_cc1"], s["meas_cc2"]
        rows.append(f"  t={s['t_us'] / 1000.0:8.3f} ms  CC1 {mv(s['cc1_raw']):>4} ({mv(s['cc1_mv']):>4} mV)  "
                    f"CC2 {mv(s['cc2_raw']):>4} ({mv(s['cc2_mv']):>4} mV)  VBUS {mv(s['vbus_mv']):>5} mV  "
                    f"meas1 bc={m1['bc_lvl']} comp={int(m1['comp'])}  meas2 bc={m2['bc_lvl']} comp={int(m2['comp'])}")
    return "\n".join([head] + rows)


def hold_ms(text):
    v = int(text)
    if not 10 <= v <= 5000:
        raise argparse.ArgumentTypeError("hold time must be 10..5000 ms")
    return v


# pd::EprInfo in order; the ask's pd::Err differs from PD_ERRS only in how BadChoice reads for this command
EPR_INFO_STATES = ["none", "pending", "got", "refused", "timeout"]
EPR_INFO_ERRS = ["ok", "no source caps", "bad PDO position", "over the voltage cap", "bad voltage", "inline",
                 "busy (already asked, in EPR mode, or not ready)", "not asked: PDO 1 has no EPR bit (op force asks anyway)"]


def decode_epr_info(p):
    """DebugEprInfo reply: [ok, err, state, n] + n raw PDO words (positions 1..n; EPR ones from 8)."""
    _need(p, 4, "epr-info")
    ok, err, state, n = p[0], p[1], p[2], p[3]
    _need(p, 4 + 4 * n, "epr-info pdos")
    pdos = [int.from_bytes(p[4 + 4 * k:8 + 4 * k], "little") for k in range(n)]
    return {"ok": bool(ok), "err": EPR_INFO_ERRS[err] if err < len(EPR_INFO_ERRS) else err,
            "state": EPR_INFO_STATES[state] if state < len(EPR_INFO_STATES) else state,
            "num_pdos": n, "epr_pdos": [f"{k + 1}:0x{w:08X}" for k, w in enumerate(pdos) if k >= 7 and w],
            # every raw word by position; an all-zero word is padding (unused SPR slots up to 7), not a PDO
            "words": [f"{k + 1}:0x{w:08X}" + ("" if w else " (empty)") for k, w in enumerate(pdos)]}


def decode_lcd(p):
    """DebugLcd reply: [flags (bit0 DMA built in, bit1 DMA trusted), transfers u32, stalls u32]."""
    _need(p, 9, "lcd")
    state = "disabled" if not p[0] & 1 else ("ok" if p[0] & 2 else "stalled")
    return {"dma": state, "transfers": int.from_bytes(p[1:5], "little"), "stalls": int.from_bytes(p[5:9], "little")}


def cmd_reboot(link, a):
    link.call(CMD_REBOOT)
    return {"reboot": True}


def dfu_payload(reset="sys"):
    """magic, plus a reset-mode byte for iwdg (sys keeps the original 4-byte form)."""
    return DFU_MAGIC + (b"\x01" if reset == "iwdg" else b"")


def factory_wipe_payload(reset="sys"):
    return FACTORY_WIPE_MAGIC + (b"\x01" if reset == "iwdg" else b"")


class WipeFailed(Exception):
    pass


def decode_factory_wipe(p):
    _need(p, 14, "factory-wipe")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    err = p[0]
    return {"err": WIPE_ERRS[err] if err < len(WIPE_ERRS) else str(err), "err_code": err,
            "sectors": u16(1), "erased": u16(3), "verified": u16(5), "failed": u16(7),
            "first_fail": int.from_bytes(p[9:13], "little"),
            "status": FLASH_STATUS[p[13]] if p[13] < len(FLASH_STATUS) else str(p[13])}


def cmd_factory_wipe(link, a):
    saved, link.timeout_s = link.timeout_s, max(link.timeout_s, WIPE_TIMEOUT_S)
    try:
        p = link.call(CMD_FACTORY_WIPE, factory_wipe_payload(getattr(a, "reset", "sys"))).payload
    finally:
        link.timeout_s = saved
    r = decode_factory_wipe(p)
    if r["err_code"] == 1:
        raise WipeFailed(f"factory wipe: erase failed at 0x{r['first_fail']:08X} ({r['status']}) after "
                         f"{r['erased']} of {r['sectors']} sectors; the K2 is NOT in DFU and stays in the "
                         "app (its file system is gone until the next boot formats it)")
    if r["err_code"]:
        raise WipeFailed(f"factory wipe: {r['failed']} sectors do not read blank (first 0x{r['first_fail']:08X}); "
                         f"{r['verified']} of {r['sectors']} verified; the K2 is NOT in DFU and stays in the app")
    return r


class DumpFailed(Exception):
    pass


def flash_dump_payload(addr, n):
    return addr.to_bytes(4, "little") + bytes([n])


def _flash_dump_pass(link):
    """One read of the whole area, 48 bytes per request; every reply echoes address and length."""
    out = bytearray()
    addr = FLASH_DUMP_BASE
    while addr < FLASH_DUMP_END:
        n = min(FLASH_DUMP_CHUNK, FLASH_DUMP_END - addr)
        p = link.call(CMD_FLASH_DUMP, flash_dump_payload(addr, n)).payload
        _need(p, 5 + n, "flash-dump")
        if int.from_bytes(p[:4], "little") != addr or p[4] != n:
            raise DumpFailed(f"flash-dump: the reply for 0x{addr:08X} (+{n}) answers a different window; "
                             "the firmware does not match this tool")
        out += p[5:5 + n]
        addr += n
    return bytes(out)


def cmd_flash_dump(link, a):
    """Dump 0x080E0000..0x08100000 through the running app, twice, and keep native_nvram.bin."""
    import hashlib
    first = _flash_dump_pass(link)
    second = _flash_dump_pass(link)
    if first != second:
        raise DumpFailed("flash-dump: the two passes differ; nothing was written. Check the cable and run it again")
    outdir = Path(a.outdir)
    outdir.mkdir(parents=True, exist_ok=True)
    (outdir / "native_nvram.bin").write_bytes(first)
    return {"file": str(outdir / "native_nvram.bin"), "size": len(first),
            "range": f"0x{FLASH_DUMP_BASE:08X}-0x{FLASH_DUMP_END:08X}", "two_pass_verified": True,
            "sha256": hashlib.sha256(first).hexdigest(), "blank": first.count(0xFF) == len(first)}


def boot_info_payload(part, off):
    return bytes([part]) + off.to_bytes(2, "little")


KEY_PINS = ((1, "OK"), (2, "PLUS"), (4, "BACK"), (8, "MINUS"))   # k2::bootinfo::kPa* (1 = pin high = released)
KEY_MASK = ((1, "BACK"), (2, "MINUS"), (4, "OK"), (8, "PLUS"))   # k2::bootmode::k*


def parse_bootinfo_text(text):
    """The K2B1 line of k2/core/bootinfo.hpp as a dict; ValueError when it is not one."""
    toks = text.split()
    if not toks or toks[0] != "K2B1":
        raise ValueError(f"not a K2B1 boot-info line: {text[:40]!r}")
    f = dict(t.split("=", 1) for t in toks[1:] if "=" in t)

    def hx(v):
        return int(v, 16)

    pin = [int(x, 16) if i < 3 else x == "1" for i, x in enumerate(f["pin"].split(","))]
    pa, keys = hx(f["pa"]), hx(f["k"])
    out = {"fw": f["fw"], "git": f["g"], "proto": int(f["p"]),
           "t_ms": int(f["t"]), "vbus_mv": int(f["vb"]), "cc1_mv": int(f["c1"]), "cc2_mv": int(f["c2"]),
           "plus_held": f["pl"] == "1", "mode": f["m"], "keys": keys, "pa": pa,
           "keys_pressed": [n for bit, n in KEY_MASK if keys & bit], "pins_low": [n for bit, n in KEY_PINS if not pa & bit],
           "reset_flags": hx(f["rf"]), "warm": f["w"] == "1",
           "pin": {"initial": pin[0], "now": pin[1], "boot": pin[2], "inherited": pin[3]},
           "mark": None if f["mk"] == "-" else hx(f["mk"]), "move": None, "readbacks": [], "notes": [], "notes_not_shown": 0}
    if f["mv"] != "-":
        ms, pins, vbus, c1, c2 = f["mv"].split(":")
        frm, to = pins.split(">")
        out["move"] = {"ms": int(ms), "from": int(frm, 16), "to": int(to, 16), "vbus_mv": int(vbus), "cc1_mv": int(c1), "cc2_mv": int(c2)}
    if f["sw"] != "-":
        for item in f["sw"].split(","):
            v, ms = item.split("@")
            out["readbacks"].append({"ms": int(ms), "sw0": hx(v)})
    if f["n"] != "-":
        for item in f["n"].split(","):
            if item.startswith("+"):
                out["notes_not_shown"] = int(item[1:])
                continue
            i = max(item.rfind("s"), item.rfind("c"))
            out["notes"].append({"ms": int(item[:i]), "kind": "sw0" if item[i] == "s" else "cc", "val": hx(item[i + 1:])})
    return out


def cmd_bootinfo(link, a):
    """Read the K2B1 text (48 bytes per request) and the first-seconds power samples (5 per request)."""
    chunks, off, total = [], 0, None
    while total is None or off < total:
        p = link.call(CMD_BOOT_INFO, boot_info_payload(BOOT_INFO_TEXT, off)).payload
        _need(p, 6, "bootinfo")
        n = p[5]
        _need(p, 6 + n, "bootinfo")
        if p[0] != BOOT_INFO_TEXT or int.from_bytes(p[1:3], "little") != off:
            raise DumpFailed(f"bootinfo: the reply for text offset {off} answers a different window; the firmware does not match this tool")
        if total is not None and int.from_bytes(p[3:5], "little") != total:
            raise DumpFailed("bootinfo: the text length changed between chunks; run it again")
        total = int.from_bytes(p[3:5], "little")
        chunks.append(bytes(p[6:6 + n]))
        off += n
        if n == 0:
            break
    text = b"".join(chunks).decode("ascii")
    if len(text) != total:
        raise DumpFailed(f"bootinfo: got {len(text)} of {total} text bytes")
    samples, idx, count = [], 0, None
    while count is None or idx < count:
        p = link.call(CMD_BOOT_INFO, boot_info_payload(BOOT_INFO_SAMPLES, idx)).payload
        _need(p, 6, "bootinfo")
        n = p[5]
        _need(p, 6 + 9 * n, "bootinfo")
        if p[0] != BOOT_INFO_SAMPLES or int.from_bytes(p[1:3], "little") != idx:
            raise DumpFailed(f"bootinfo: the reply for sample {idx} answers a different window; the firmware does not match this tool")
        count = int.from_bytes(p[3:5], "little")
        for k in range(n):
            d = p[6 + 9 * k:6 + 9 * (k + 1)]
            samples.append({"ms": (idx + k) * 50, "vbus_mv": int.from_bytes(d[0:2], "little"), "cc1_mv": int.from_bytes(d[2:4], "little"),
                            "cc2_mv": int.from_bytes(d[4:6], "little"), "aux_mv": int.from_bytes(d[6:8], "little"), "plus": bool(d[8] & 1)})
        idx += n
        if n == 0:
            break
    return {"qr_text": text, "bootinfo": parse_bootinfo_text(text), "samples": samples}


def render_bootinfo(r):
    b = r["bootinfo"]
    pin = lambda v: {0: "none", 1: "CC1", 2: "CC2"}.get(v, f"0x{v:X}")
    lines = [f"boot-info for firmware {b['fw']} build {b['git']} (proto {b['proto']}): mode {b['mode']}, keys at reset {', '.join(b['keys_pressed']) or 'none'} "
             f"(mask 0x{b['keys']:X}; pins low: {', '.join(b['pins_low']) or 'none'}), "
             f"reset flags 0x{b['reset_flags']:08X} ({'warm' if b['warm'] else 'cold'})",
             f"Rd pin: first {pin(b['pin']['initial'])}, now {pin(b['pin']['now'])}, restore {pin(b['pin']['boot'])}"
             + (", inherited from the pin mark" if b["pin"]["inherited"] else "")
             + (f"; pin mark 0x{b['mark']:04X}" if b["mark"] is not None else "")]
    m = b["move"]
    if m:
        vb = "no power-monitor answer" if m["vbus_mv"] < 0 else f"VBUS {m['vbus_mv']} mV"
        lines.append(f"Rd moved CC{m['from']} -> CC{m['to']} at {m['ms']} ms ({vb}, CC1 {m['cc1_mv']} mV, CC2 {m['cc2_mv']} mV)")
    else:
        lines.append("Rd did not move")
    if b["readbacks"]:
        lines.append("SWITCHES0 read back: " + ", ".join(f"0x{x['sw0']:02X} at {x['ms']} ms" for x in b["readbacks"]))
    if b["notes"]:
        lines.append("first writes: " + ", ".join(f"{x['ms']} ms {x['kind']} 0x{x['val']:02X}" for x in b["notes"])
                     + (f" (+{b['notes_not_shown']} not shown)" if b["notes_not_shown"] else ""))
    lines.append(f"now: uptime {b['t_ms']} ms, VBUS {b['vbus_mv']} mV, CC1 {b['cc1_mv']} mV, CC2 {b['cc2_mv']} mV, PLUS {'held' if b['plus_held'] else 'up'}; "
                 f"{len(r['samples'])} power samples of the first seconds (use --json to read them)")
    lines.append(r["qr_text"])
    return "\n".join(lines)


def fault_payload(kind):
    return FAULT_MAGIC + bytes([FAULT_KINDS[kind]])


def cmd_fault(link, a):
    link.call(CMD_DEBUG_FAULT, fault_payload(a.kind))
    return {"fault": a.kind}


def cmd_dfu(link, a):
    link.call(CMD_DFU_ENTER, dfu_payload(getattr(a, "reset", "sys")))
    return {"dfu": True}


def cmd_log(link, a):
    payload = b"" if a.state is None else bytes([1 if a.state == "on" else 0])
    return {"log_on": bool(link.call(CMD_LOG, payload).payload[0])}


def fmt_readout(s):
    # Magnitudes only; the direction is a separate word (JSON keeps the signed current_ma).
    return (f"{s['vbus_mv'] / 1000:7.3f} V  {abs(s['current_ma']) / 1000:7.3f} A  "
            f"{abs(s['power_mw']) / 1000:7.3f} W  {s['direction']:7s}  {s['energy_mwh']} mWh  "
            f"page={s['page_name']}")


def cmd_watch(link, a, out=print):
    n = 0
    while a.count <= 0 or n < a.count:
        s = decode_state(link.call(CMD_GET_STATE).payload)
        out(json.dumps(s) if a.json else fmt_readout(s))
        n += 1
        time.sleep(a.interval / 1000)
    return None


def _emarker_detail_text(r):
    if "sop" not in r:
        return ""
    s = (f"\n  {r['sop']} PD {r['pd_rev']} VDO v{r['vdo_version']}  latency {r['latency']}  "
         f"{r['termination']}{'  EPR cable' if r['epr_cable'] else ''}")
    if r["vdo2_valid"]:
        s += (f"\n  temp {r['max_temp_c']}/{r['shutdown_temp_c']} C  U3 power {r['u3_power']}  "
              f"USB4={'yes' if r['usb4'] else 'no'}  {r['usb_lanes']} lane(s)"
              f"{'  optical' if r['optical'] else ''}")
    if r["thunderbolt"]:
        s += "\n  Thunderbolt SVID 0x8087 advertised"
    return s


def _ago(r):
    """Suffix for results that can be previous data: " (LAST, unplugged 1m 23s ago)"."""
    if not r.get("stale"):
        return ""
    s = r.get("age_s", 0)
    t = f"{s}s" if s < 60 else f"{s // 60}m {s % 60:02d}s" if s < 3600 else f"{s // 3600}h {s % 3600 // 60:02d}m"
    return f" (LAST, {t} ago)"


def render(name, r):
    """Human readable text for a command result."""
    text = _render(name, r)
    if name in ("emarker", "identity", "state") and r.get("stale"):   # not pdos: a detach empties that list
        first, _, rest = text.partition("\n")
        text = first + _ago(r) + ("\n" + rest if rest else "")
    return text


def _hunt_line(h):
    """The evidence of a refused 6 A row on one indented line: cause, the Request and what the source answered."""
    ans = h["answer"] + (f" after {h['answer_ms']} ms" if h["answer_ms"] else "")
    if "timeout" in h["flag_names"]:
        ans = "no answer"
    last = f"0x2717020{h['last_step'] & 15:X}" if h["last_step"] else "none"
    return (f"      hunt: {h['cause']} ({h['explanation']}); asked {h['req_mv'] / 1000:g} V {h['req_ma'] / 1000:g} A of pos {h['pos']} "
            f"({h['type']}, offered {h['apdo_ma'] / 1000:g} A, {h['caps']} caps); cable {h['cable_ma'] / 1000:g} A; "
            f"chain [{' '.join(h['flag_names'])}] last {last}; rdo {h['rdo']:#010x}; {ans}")


def _render(name, r):
    if name == "charger-test" and r.pop("_summary", False):
        return charger_summary.summarize(r)
    if name == "charger-test":
        head = f"run {r['run_id']} rev {r['revision']}: {r['state']} ({r['reason']}); step {r['current']}/{r['total']}"
        notes = ["  PASS = negotiation / voltage hold, not power or revision conformance"]
        if r["selection"] is None:
            notes.append("  legacy schema: selection / source evidence unavailable; some entries omitted")
        else:
            sel, e = r["selection"], r["evidence"]
            notes.append(f"  run-selected {len(sel['run'])}; draft-selected {len(sel['configured'])}; setup {r['stage']}")
            rmdo = e["source_rmdo"]
            notes.append(f"  header {e['wire_revision']}; RMDO {rmdo['revision'] + ' self-report' if rmdo else 'not observed'}; "
                         "no independent minor-revision tests")
            cable = e["physical_cable"]
            notes.append(f"  start guards: monitor {e['monitor_mv']} mV; physical cable {cable['max_mv']} mV / "
                         f"{cable['max_ma']} mA ({'observed claims' if cable['observed'] else 'unknown default'}); "
                         f"spoof {'on' if e['spoof_on'] else 'off'}")
        rows = []
        for s in r["steps"]:
            rows.append(f"  {s['index']}: {s['protocol']} {s['target_mv']} mV -> {s['measured_mv']} mV "
                        f"{s['result']} ({s.get('explanation', s['reason'])}; {s['detail_reason']})")
            if "hunt" in s:
                rows.append(_hunt_line(s["hunt"]))
        return "\n".join([head] + notes + rows)
    if name == "transcap" and isinstance(r, list):   # `transcap list`: the stored captures
        if not r:
            return "no stored captures"
        return "\n".join(f"  seq {c['seq']}: run {c['run_id']:#x} step {c['step']}, {c['reason']}" for c in r)
    if name == "blackbox" and isinstance(r, list):   # `blackbox captures`: the frozen captures
        if not r:
            return "no captures"
        return "\n".join(f"  slot {c['slot']}: {c['trigger']}, {c['bytes']} bytes, {c['age_ms'] / 1000:g} s ago"
                         for c in r)
    if not isinstance(r, dict):
        return json.dumps(r)
    if name == "match":
        head = f"match {r['verdict']}"
        if r["request_seen"]:
            head += f": PDO{r['want_pos']} {r['want_mv'] / 1000:g} V {r['want_ma'] / 1000:g} A"
        if r["caps_seen"]:
            head += f", charger max {r['src_max_mv'] / 1000:g} V {r['src_max_w']} W"
        rows = " ".join(f"{i + 1}:{s}" for i, s in enumerate(r["rows"]))
        return head + ("\n" + rows if rows else "")
    if name == "pd":
        return (f"trigger {r['state']} result={r['result']} held={r['held']} epr={r['epr']} "
                f"pdo={r['pos']} {r['mv'] / 1000:g} V {r['ma'] / 1000:g} A hard_resets={r['hard_resets']} source_hard_resets={r['source_hard_resets']}")
    if name == "pd-reread":
        extra = " attaching" if r.get("attaching") else (" no contract" if r.get("attach_failed") else "")
        extra += " ra" if r.get("ra") else ""
        return (f"reread {r['state']}{extra} mask=0x{r['mask']:02X} answered={','.join(r['answered']) or '-'} "
                f"skipped={','.join(r['skipped']) or '-'} pdos {r['pdos_before']} -> {r['pdos_last']} "
                f"caps_rx={r['caps_rx']} avs={'yes' if r['avs'] else 'no'}")
    if name == "identify-device":
        p, i = r["identify"], r["identity"]
        lines = [f"identify {p['state']} answered={','.join(p['answered']) or '-'} "
                 f"no_answer={','.join(p['no_answer']) or '-'} retries={p['retries']}"]
        if i["valid"]:
            lines.append(f"  vid=0x{i['vid']:04X} {i['vendor']} pid=0x{i['pid']:04X} bcd=0x{i['bcd']:04X}"
                         + (f" model={i['model']}" if i["model"] else ""))
        if i["svids"]:
            lines.append("  svids " + " ".join(f"0x{s:04X}" for s in i["svids"]))
        for n, raw in enumerate(p["sink_pdos"], 1):
            lines.append(f"  sink pdo {n}: 0x{raw:08X}")
        if p["revision"]:
            lines.append(f"  revision 0x{p['revision']:08X}")
        return "\n".join(lines)
    if name == "ping":
        return (f"K2 firmware {r['version']} git {r['git']} proto {r['proto']} "
                f"uptime {r['uptime_ms'] / 1000:.1f} s")
    if name == "state":
        return "\n".join(f"{k}: {v}" for k, v in r.items())
    if name == "pdos":
        lines = [f"source caps: valid={r['valid']} {r['spec_rev']} count={r['num_pdos']}"]
        for i, p in enumerate(r["pdos"]):
            lines.append(f"  [{i}] {p['type']:8} {p['min_mv'] / 1000:g}-{p['max_mv'] / 1000:g} V "
                         f"{p['max_ma'] / 1000:g} A {p['max_mw'] / 1000:g} W  raw=0x{p['raw']:08X}")
        return "\n".join(lines)
    if r.get("cards"):
        k = r["cards"]
        if k == "list":
            if not r["list"]:
                return "no report cards"
            return "\n".join(f"  {c['no']:4} {c['name'] or '(unnamed)':20} fp={c['fp']} {c['vid']:04X}:{c['pid']:04X} "
                             f"{c.get('brand', ''):8} {str(c['pdp_w']) + 'W' if c.get('pdp_w') else '':5} runs={c['runs']}"
                             + (f" same x{c['times']}, last boot {c['last_boots']} up {c['last_uptime_s']}s" if c.get("times", 1) > 1 else "")
                             + (f" rebalanced x{c['rebalanced']}" if c.get("rebalanced") else "")   # v8 field (offset 196)
                             for c in r["list"])
        if k == "show":
            return charger_summary.summarize_card(r["card"])
        if k == "export":
            return f"wrote {r['count']} cards to {r['file']}"
        if k == "delete":
            return f"deleted card {r['no']}"
        return f"card {r['no']} is now {r['card']['name']}"
    if r.get("emlib"):
        k = r["emlib"]
        if k == "learn":
            return render_learn(r)
        if k == "unmatched":
            return render_unmatched(r)
        if k == "list":
            if not r["cables"]:
                return "no saved cables"
            return "\n".join(f"  {c['seq']:4} {c['name']}  vid=0x{c['vid']:04X} pid=0x{c['pid']:04X}"
                             for c in r["cables"])
        if k == "save":
            return f"{'already saved' if r['existed'] else 'saved'} as {r['cable']['name']} (id {r['seq']})"
        if k == "delete":
            return f"deleted id {r['seq']}"
        if k == "rename":
            return f"id {r['seq']} is now {r['cable']['name']}"
        return (f"spoof {'ARMED' if r['active'] else 'off'} id {r['seq']} "
                f"requests={r['requests']} replies={r['replies']}")
    if name == "emarker":
        vdos = " ".join(f"0x{v:08X}" for v in r["vdos"])
        return (f"e-marker valid={r['valid']} active={r['active']} vendor={r['vendor']!r} "
                f"vid=0x{r['vid']:04X} pid=0x{r['pid']:04X} speed={r['speed']!r}\n"
                f"  {r['max_voltage_v']} V {r['max_current_a']} A {r['max_power_w']} W  VDOs: {vdos}"
                + _emarker_detail_text(r))
    if name == "identity":
        if not r["valid"]:
            return "no source identity (no Discover Identity ACK seen)"
        svids = " ".join(f"0x{s:04X}" for s in r["svids"]) or "none"
        flags = ",".join(k for k in ("epr", "epr_partial", "cuktech", "xiaomi") if r[k]) or "-"
        return (f"source vid=0x{r['vid']:04X} ({r['vendor']}) pid=0x{r['pid']:04X} bcd=0x{r['bcd']:04X}"
                f"{' model=' + repr(r['model']) if r['model'] else ''}\n  SVIDs: {svids}  flags: {flags}")
    if name == "adc-raw":
        lines = ["raw ADC channels (12 bit):" + ("" if r["ok"] else "  (sweep failed)")]
        lines += [f"  ch{ch:<2} {v:5}  0x{v:03X}" for ch, v in r["channels"].items()]
        lines.append("scan slots: " + "  ".join(f"{k}={v}" for k, v in r["scan"].items()))
        return "\n".join(lines)
    if name == "ripple":
        khz = 1e6 / r["period_ns"] if r["period_ns"] else 0
        text = (f"ripple code {r['code']}: {r['count']} samples, period {r['period_ns']} ns "
                f"({khz:.0f} kS/s), min {r['min']} max {r['max']} mean {r['mean']} "
                f"p-p {r['max'] - r['min']} counts")
        return text + (f"\nwrote {r['png']}" if "png" in r else "")
    if name == "debug" and "header" in r:
        return f"fed sop={r['sop']} header=0x{r['header']:04X} objects={r['objects']}"
    if name == "store":
        text = (f"store {r['action']} ok: {r['count']} e-marker records in the library"
                f"{', native slot filled' if r['native_slot'] else ''}"
                f"{', foreign data in the library page (kept)' if r['foreign'] else ''}"
                f"{', ' + str(r['native_records']) + ' native library records (NATIVE/ on the drive)' if r['native_records'] else ''}; "
                f"saved log {r['log_len']} bytes; flash bank {r['bank']}")
        return text + (f"\nsaved e-marker as slot {r['slot']}" if r["action"] == "save-emarker" else "")
    if name == "flash-dump":
        text = (f"flash dump ok: {r['size']} bytes of {r['range']} read twice, both passes identical, "
                f"saved to {r['file']} (sha256 {r['sha256']})")
        return text + ("\nWARNING: every byte is 0xFF; the native data area is blank" if r["blank"] else "")
    if name == "bootinfo":
        return render_bootinfo(r)
    if name == "factory-wipe":
        return (f"factory wipe done: {r['sectors']} sectors erased and verified blank "
                f"({r['verified']} verified; the volume plus any pages carrying our own old signature, "
                "the native area is never touched), the K2's own backup registers cleared. The K2 now enters DFU "
                "without booting the app again. Flash the next container now: "
                "python3 tools/k2_flasher.py flash FILE.k2 (no --auto-dfu; it is already in DFU).")
    if name == "fault":
        return f"fault ({r['fault']}) requested: watch the USB log for [FAULT]; the K2 then resets."
    if name == "dfu":
        return ("DFU_ENTER accepted: the K2 clears its app marker and resets into the "
                "bootloader. Flash with tools/k2_flasher.py flash FILE (or hold MINUS while "
                "plugging in if it does not show up).")
    if name == "debug" and "link" in r:
        if r["link"] == "detach":
            return ("link held detached (a rescan releases it); the chip dropped Rd for 1.5 s, so the charger "
                    "sees a real detach (not done inline or with a boot-pinned Rd)")
        return f"link held {r['link']}ed (a rescan releases it)"
    if name == "fc":
        s = r["scan"]
        lines = [f"fc {r['state']} {r['proto']} target {r['target_mv']} mV, VBUS {r['vbus_mv']} mV"
                 + (f" ({r['fail']})" if r["fail"] != "none" else "")]
        if s["running"]:
            lines.append(f"scan running, step {s['step'] + 1}")
        lines += [f"  {x['proto']} {x['outcome']}" + (f" {x['mv']} mV" if x["mv"] else "") for x in s["results"]]
        return "\n".join(lines)
    if name == "ufcs":
        return render_ufcs(r)
    if name == "qrtest":
        if "qrtest_list" in r:
            return r["qrtest_list"].rstrip("\n")
        if "qrtest_show" in r:
            s = r["qrtest_show"]
            return (f"qrtest: showing v{s['version']} at {s['px']} px, quiet {s['quiet']} as one full-screen QR "
                    "(BACK or `qrtest stop` leaves)")
        if "qrtest_stop" in r:
            return "qrtest: test screen popped" if r["qrtest_stop"] else "qrtest: no test screen was showing"
    if name == "cable-r" and r["kelvin"]:
        if not r["valid"]:
            return ("cable resistance (kelvin): waiting for sense wires on CC1/CC2/D- and a load of 0.45 A or more"
                    " - the K2 path is not included")
        return (f"cable resistance (kelvin): {r['r_mohm']} mOhm ({r['grade']}), pos {r['pos_mohm']} mOhm, "
                f"gnd {r['gnd_mohm']} mOhm - K2 path not included")
    if name == "cable-r":
        if not r["valid"]:
            return ("cable resistance: waiting " + ("for a load step of 1 A or more over the idle reference "
                    f"({r['idle_mv']} mV, {r['idle_ma']} mA)" if r["idle"] else "for an idle reference (< 0.2 A)")
                    + " - passive, uses your own load")
        return (f"cable resistance: {r['r_mohm']} mOhm ({r['grade']}), idle {r['idle_mv']} mV {r['idle_ma']} mA, "
                f"load {r['load_mv']} mV {r['load_ma']} mA - includes the K2 path up to the VBUS tap")
    if name == "debug":
        if "cc_matrix" in r:
            return f"cc-matrix set to {r['cc_matrix']}"
        if "cc_probe" in r:
            return render_cc_probe(r)
        if "vconn" in r:
            return "vconn " + " ".join(f"{k}={v}" for k, v in r["vconn"].items())
        if "epr_info" in r:
            return "epr-info " + " ".join(f"{k}={v}" for k, v in r["epr_info"].items())
        if "lcd" in r:
            return "lcd dma " + " ".join(f"{k}={v}" for k, v in r["lcd"].items())
        n = r["fake_pdos"]
        return f"fake PDOs: {n}" if n else "fake PDOs: off (live source caps)"
    if name == "screenshot":
        return (f"wrote {r['file']} ({r['width']}x{r['height']}), {r['rle_bytes']} bytes "
                f"compressed in {r['ms']} ms")
    return "ok " + " ".join(f"{k}={v}" for k, v in r.items())


COMMANDS = {"ping": cmd_ping, "press": cmd_press, "state": cmd_state, "pdos": cmd_pdos,
            "emarker": cmd_emarker, "identity": cmd_identity, "match": cmd_match, "usblink": cmd_usblink, "action": cmd_action, "page": cmd_page,
            "reboot": cmd_reboot, "dfu": cmd_dfu, "factory-wipe": cmd_factory_wipe, "flash-dump": cmd_flash_dump, "bootinfo": cmd_bootinfo, "fault": cmd_fault, "log": cmd_log, "watch": cmd_watch,
            "screenshot": cmd_screenshot, "debug": cmd_debug,
            "adc-raw": cmd_adc_raw, "ripple": cmd_ripple, "fc": cmd_fc,
            "pd": cmd_pd, "identify-device": cmd_identify_device, "pd-reread": cmd_pd_reread,
            "store": cmd_store, "blackbox": cmd_blackbox, "transcap": cmd_transcap,
            "cable-r": cmd_cable_r, "ufcs": cmd_ufcs, "lab": cmd_lab, "qrtest": cmd_qrtest, "charger-test": cmd_charger_test, "charger-units": cmd_charger_units,
            "charger-cable": cmd_charger_cable, "cards": cmd_cards}


def build_parser():
    p = argparse.ArgumentParser(description="Open-K2 remote control over USB HID")
    p.add_argument("--json", action="store_true", help="machine readable output")
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    p.add_argument("--timeout", type=float, default=1.0, help="response timeout in seconds")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("ping", help="firmware version, git hash, uptime")
    sp = sub.add_parser("press", help="inject a button press")
    sp.add_argument("button", choices=sorted(BUTTONS))
    g = sp.add_mutually_exclusive_group()
    g.add_argument("--long", action="store_true", help=f"hold {LONG_PRESS_MS} ms")
    g.add_argument("--ms", type=int, help="hold time in ms (30..10000); 5000 = reboot hold on back")
    sub.add_parser("state", help="page, power readings, PD summary")
    st = sub.add_parser("charger-test", help="read existing RAM suite results (never starts or stops tests)")
    sts = st.add_subparsers(dest="test_cmd", required=True)
    sts.add_parser("status", help="read run/state metadata only")
    strc = sts.add_parser("results", help="retrieve a consistent bounded result table")
    strc.add_argument("--out", help="write the complete evidence JSON to this host file")
    sms = sts.add_parser("summary", help="compact per-protocol summary of the RAM results")
    sms.add_argument("--out", help="also write the complete evidence JSON to this host file")
    cu = sub.add_parser("charger-units", help="set the test units (and their order) of the NEXT charger run; reset by every START")
    cu.add_argument("units", help="comma list, e.g. qc2,qc3,pd (names: " + ", ".join(TEST_UNITS) + "); 'default' = the default order")
    cc = sub.add_parser("charger-cable", help="cable identity the charger-test START dialog opens on (no argument: read it back)")
    cc.add_argument("cable", nargs="?", help="off | builtin | builtin:<id 1, 2, 4 | epr-50v | cuktech-6a | oneplus> | "
                    "saved:<library id>; Ra mode + spoof for the run unless off")
    sub.add_parser("pdos", help="decoded source capabilities")
    se = sub.add_parser("emarker", help="cable e-marker data; list/save/delete/rename/spoof manage saved cables")
    es = se.add_subparsers(dest="em_cmd")
    es.add_parser("list", help="saved cables")
    es.add_parser("save", help="save the cable now inline")
    ed = es.add_parser("delete", help="delete a saved cable")
    ed.add_argument("id", type=int)
    ed.add_argument("--confirm", action="store_true")
    er = es.add_parser("rename", help="rename a saved cable")
    er.add_argument("id", type=int)
    er.add_argument("name")
    ep = es.add_parser("spoof", help="arm a saved cable as e-marker, or off / status")
    ep.add_argument("target", help="saved-cable id, a built-in (builtin:0..4, or generic, epr-50v, cuktech-6a, oneplus; "
                    "older names generic-epr = epr-50v, cuktech-240w = cuktech-6a, oneplus-mag = oneplus, id 3 = id 4), off or status")
    el = es.add_parser("learn", help="power the cable in the BOTTOM port (no charger) and record what it answers: Discover "
                       "Identity / SVIDs / Modes plus the seeds; --save stores the clone with those replies")
    el.add_argument("--seed", action="append", default=[], metavar="SVID:CMD[:VDO[:VDO]]",
                    help="also send this structured request (SVID and VDOs hex, CMD 0..31), e.g. 22D9:31 for the OPPO "
                    "vendor command; repeatable, at most 4")
    el.add_argument("--sweep", action="store_true", help="also try vendor commands 16..31 on the cable's SVIDs and the seeds' SVIDs")
    el.add_argument("--confirm", action="store_true", help="allow the scan while a sink is attached inline (may interrupt charging)")
    el.add_argument("--save", action="store_true", help="save the learned clone into the cable library")
    es.add_parser("learned", help="the last learn run's summary and transcript (no run)")
    es.add_parser("unmatched", help="the SOP'/SOP'' requests the armed fake cable received and nothing answered")
    sc = sub.add_parser("cards", help="charger report cards: list / show / export / rename / delete")
    scs = sc.add_subparsers(dest="cards_cmd", required=True)
    scs.add_parser("list", help="saved cards, newest first")
    sh = scs.add_parser("show", help="one card in the run-summary format")
    sh.add_argument("id", type=int)
    sx = scs.add_parser("export", help="all cards as a JSON array (same text as CARDS.JSON on the USB drive)")
    sx.add_argument("--out", required=True)
    sn = scs.add_parser("rename", help="name a card (the only way to name one)")
    sn.add_argument("id", type=int)
    sn.add_argument("name")
    sd = scs.add_parser("delete", help="delete a card")
    sd.add_argument("id", type=int)
    sd.add_argument("--confirm", action="store_true")
    sub.add_parser("identity", help="source identity: VID/PID, SVIDs, Apple model")
    sub.add_parser("usblink", help="USB / Thunderbolt link seen in sniffed PD traffic (passive, see k2/pd/usb_link.hpp)")
    sub.add_parser("match", help="wants vs offers: the request against the charger caps (passive)")
    sa = sub.add_parser("action", help="run a page action")
    sa.add_argument("name", choices=sorted(ACTIONS))
    sa.add_argument("--confirm", action="store_true",
                    help="allow it while a live sink is attached (may interrupt charging)")
    spd = sub.add_parser("pd", help="active PD: request a contract, reset, release, status")
    spds = spd.add_subparsers(dest="pd_cmd", required=True)
    sq = spds.add_parser("request", help="request PDO n (above 5 V needs --confirm; refused inline unless --over-load)")
    sq.add_argument("pos", type=int, choices=range(0, 12), metavar="N", help="1-based PDO position (0 = read the EPR list)")
    sq.add_argument("--mv", type=int, default=0, help="PPS / AVS voltage in mV")
    sq.add_argument("--ma", type=int, default=0, help="current in mA")
    sq.add_argument("--confirm", action="store_true", help="allow a voltage above 5 V")
    sq.add_argument("--over-load", action="store_true",
                    help="lab only: let the K2 request while a far sink is attached, but only when that sink sent no PD "
                         "message this attach (a passive load: Rd only, no Request, no Get_Source_Cap); refused otherwise. "
                         "Never reachable from the UI or the charger test")
    spds.add_parser("reset", help="PD hard reset (the supply returns at 5 V)")
    spds.add_parser("release", help="back to 5 V (Request PDO 1)")
    sa = spds.add_parser("attach", help="attach as PD sink and request target voltage (default 9000 mV)")
    sa.add_argument("mv", type=int, nargs="?", default=9000, help="target voltage in mV (default 9000)")
    sa.add_argument("ma", type=int, nargs="?", default=0, help="target current in mA (default 0 = PDO max)")
    spds.add_parser("status", help="trigger engine state")
    si = sub.add_parser("identify-device", help="ask the attached device for its identity (idle CC only)")
    si.add_argument("--confirm", action="store_true", help="allow it while a live sink is attached")
    si.add_argument("--wait", type=float, default=ID_POLL_S, help="seconds to wait for the sequence")
    sr = sub.add_parser("pd-reread", help="source-cap re-read in PD mode: Get_Source_Cap_Extended, Discover Identity, SVIDs, Get_Source_Cap (see k2/pd/pd_reread.hpp)")
    sr.add_argument("--mask", type=_reread_mask, default=None, help="steps: 1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 second get-cap (default 15)")
    sr.add_argument("--wait", type=float, default=REREAD_POLL_S, help="seconds to wait for the run, attach phase included")
    sr.add_argument("--abort", action="store_true", help="stop a run and release the Rd the run presented")
    sr.add_argument("--line", type=int, choices=(0, 1, 2), default=None,
                    help="debug: the attach scan may take only this CC line (Rd then goes on that line alone); with another "
                         "line already taken and no contract the run forgets it first. Stays set for later attaches (a charger "
                         "test START too) until --line 0 (then the run goes on normally), --abort or a reboot")
    sr.add_argument("--ra", choices=("on", "off"), default=None,
                    help="Ra mode: PC13 high puts an Ra on CC1 before Rd goes on CC2 alone, so a source sees a sink plus a "
                         "powered cable and may probe the cable (SOP' Discover Identity, answered by an armed e-marker spoof). "
                         "Held until detach, --abort, --ra off or a reboot; refused inline with a sink, with a contract or "
                         "while another run is active. Stays on for a charger test START too")
    sr.add_argument("--no-run", action="store_true", help="with --ra on or --pin: only set the mode, start no run")
    sr.add_argument("--pin", type=int, choices=(0, 1, 2), default=None,
                    help="lab: pin the Rd on CC1 or CC2 (0 = release) the way a key-mode boot does, mark included, so a HID-powered "
                         "K2 acts like a charger-powered one and a warm reset (reboot) inherits the pin; refused inline. Use with --no-run")
    sr.add_argument("--rd-glitch", type=int, metavar="MS", default=None,
                    help="lab: drop the Rd for MS ms (10 ms steps, Ra kept) so the source sees a detach and a fresh attach; "
                         "VBUS min and the time below 4 V go to the K2 log ([RMT] rd glitch)")
    sr.add_argument("--relook", action="store_true",
                    help="lab: send the unit boundary's Soft_Reset (the source re-announces its caps, the Rd stays); nothing else runs")
    sr.add_argument("--sink-cap-ext", choices=SINK_CAP_EXT_MODES, default=None,
                    help="before the run, set how the sink answers Get_Sink_Cap_Extended: skedb (the firmware default, "
                         "Sink_Capabilities_Extended: AVS + PPS capable), not-supported or silent; RAM only, reboot = skedb")
    sg = sub.add_parser("page", help="switch page: home detail chargertest lines tools settings (or 0..5), connection [source cable sink], diag; "
                     "old names pd/emarker/identify open Connection's tab; closes any open submenu")
    sg.add_argument("page", type=_page_arg, metavar="PAGE")
    sf = sub.add_parser("fc", help="active fast-charge trigger (puts up to 20 V on the output)")
    sfs = sf.add_subparsers(dest="fc_cmd", required=True)
    sfs.add_parser("status", help="trigger state, VBUS and the last scan results")
    sfs.add_parser("reset", help="back to 5 V, then passive idle")
    sfc = sfs.add_parser("scan", help="auto-detect the charger (steps up to 9 V)")
    sfc.add_argument("--confirm", action="store_true", help="required: the scan raises VBUS")
    sfc.add_argument("--allow-inline", action="store_true", help="override the PD-contract / load guard")
    sft = sfs.add_parser("trigger", help="request one protocol at one voltage")
    sft.add_argument("proto", choices=FC_PROTOS)
    sft.add_argument("--volts", type=float, required=True)
    sft.add_argument("--confirm", action="store_true", help="required above 5 V")
    sft.add_argument("--allow-inline", action="store_true", help="override the PD-contract / load guard")
    sst = sub.add_parser("store", help="flash store behind the USB drive (writes only when asked)")
    sss = sst.add_subparsers(dest="store_cmd", required=True)
    sss.add_parser("list", help="records in the store")
    sss.add_parser("save-emarker", help="save the inline cable into the saved-cable library (same as emarker save)")
    sss.add_parser("save-log", help="save the recent log text to flash (shown as SAVEDLOG.TXT)")
    sub.add_parser("reboot", help="reset the K2")
    sdf = sub.add_parser("dfu", help="reset into the bootloader (clears the app marker)")
    sdf.add_argument("--confirm", action="store_true", required=True,
                     help="required: the K2 stays in DFU until it is flashed again")
    sdf.add_argument("--reset", choices=["sys", "iwdg"], default="sys",
                     help="reset type: sys = SYSRESETREQ (default), iwdg = watchdog reset")
    sfd = sub.add_parser("flash-dump", help="read-only dump of the native device storage "
                         "(0x080E0000..0x08100000) through the app, twice, to DIR/native_nvram.bin")
    sfd.add_argument("outdir", metavar="DIR", help="output directory (keep it outside the repo)")
    sub.add_parser("bootinfo", help="how THIS boot decided its mode (PLUS/OK key, Rd pin, move, SWITCHES0 readbacks, first-seconds "
                   "samples); read-only RAM; --json carries qr_text, the text the LCD shows as a QR code in a key-mode boot")
    sfw = sub.add_parser("factory-wipe", help="prepare for the factory firmware: erase the file system and the "
                         "legacy pages that carry our own signature, clear our backup registers, then enter DFU "
                         "(no reboot into the app; the native data area is never touched)")
    sfw.add_argument("--confirm", action="store_true", required=True,
                     help="required: our saved data on the K2 is erased and the K2 stays in DFU")
    sfw.add_argument("--reset", choices=["sys", "iwdg"], default="sys",
                     help="reset type for the DFU entry: sys = SYSRESETREQ (default), iwdg = watchdog reset")
    sfa = sub.add_parser("fault", help="debug: provoke a CPU fault; the K2 logs [FAULT] and resets")
    sfa.add_argument("--confirm", action="store_true", required=True, help="required: the K2 resets")
    sfa.add_argument("kind", choices=sorted(FAULT_KINDS))
    sl = sub.add_parser("log", help="USB text log on/off (no argument: query)")
    sl.add_argument("state", nargs="?", choices=["on", "off"])
    ss = sub.add_parser("screenshot", help="save the LCD content as a PNG")
    ss.add_argument("out", help="output .png path")
    ss.add_argument("--scale", type=int, default=1, choices=range(1, 9), help="integer upscale")
    ss.add_argument("--debug", action="store_true", help="print pixel and colour counts per strip")
    ss.add_argument("--dump-raw", metavar="FILE", help="save every received frame (64 B each) to FILE")
    sd = sub.add_parser("debug", help="debug helpers (firmware built with them)")
    sds = sd.add_subparsers(dest="debug_cmd", required=True)
    sdp = sds.add_parser("pdos", help="fake source capabilities, no charger needed")
    sdp.add_argument("--fake", type=int, required=True, choices=range(12), metavar="N",
                     help="load N synthetic PDOs (FIX/PPS/AVS mix), 0 = back to live caps")
    sds.add_parser("detach", help="pretend the charger was unplugged (stale data), until the next rescan")
    sds.add_parser("attach", help="pretend a charger is attached (live data), until the next rescan")
    sdm = sds.add_parser("pd-msg", help="feed a raw PD message (header + data objects, hex) to the parser")
    sdm.add_argument("hex", help="little-endian wire bytes, e.g. 4f10 01a000ff")
    sdm.add_argument("--sop", choices=sorted(SOPS), default="sop", help="sop = source, sop1 = SOP', sop2 = SOP''")
    scm = sds.add_parser("cc-matrix", help="debug: set the CC switch matrix (bit0 PC13, bit1 PC14, bit2 PC15)")
    scm.add_argument("v", type=int, choices=range(8), metavar="0-7")
    scm.add_argument("--modes", type=lambda x: int(x, 16), default=None, metavar="HEX",
                     help="CRH mode nibbles for PC13/14/15 (bits 0-3/4-7/8-11), e.g. 888 = all InPull")
    scp = sds.add_parser("cc-probe", help="debug: hold a raw FUSB302 SWITCHES0 value on a charger that waits for a sink "
                                          "(no Rd, VBUS 0) and sample both CC lines, then restore (refused unless passive)")
    scp.add_argument("--sw0", type=lambda x: int(x, 16), required=True, metavar="HEX",
                     help="SWITCHES0: bit0 PDWN1, bit1 PDWN2, bit2 MEAS_CC1, bit3 MEAS_CC2, bit4 VCONN_CC1, bit5 VCONN_CC2; "
                          "pull-up bits 6/7 are refused, VCONN bits need --pc with bit0 clear (PC13 low)")
    scp.add_argument("--pc", type=int, required=True, choices=range(8), metavar="0-7",
                     help="PC13/14/15 levels (bit0 PC13 = VCONN rail control, bit1 PC14, bit2 PC15), as cc-matrix")
    scp.add_argument("--ms", type=hold_ms, required=True, metavar="10-5000", help="hold time in ms")
    scp.add_argument("--modes", type=lambda x: int(x, 16), default=None, metavar="HEX",
                     help="CRH mode nibbles for PC13/14/15 (bits 0-3/4-7/8-11); default: leave them")
    svc = sds.add_parser("vconn", help="debug: VCONN_Swap experiment (swap = ask the source for the VCONN role, off = drop it)")
    svc.add_argument("op", choices=["status", "swap", "off", "roundtrip", "data-reset"])
    sde = sds.add_parser("epr-info", help="debug: informational EPR_Get_Source_Cap (ask = only when PDO 1 has the EPR bit, "
                                          "force = ask any source; needs an active sink, never starts an EPR contract)")
    sde.add_argument("op", choices=["status", "ask", "force"])
    sds.add_parser("lcd", help="debug: LCD DMA state (ok / stalled / disabled) and transfer count")
    sub.add_parser("adc-raw", help="raw ADC counts of every channel")
    sr = sub.add_parser("ripple", help="sample PA1 at high rate and plot the ripple")
    sr.add_argument("--code", type=int, choices=range(8), metavar="N",
                    help="ADC sample time code 0..7 (default: firmware default)")
    sr.add_argument("--png", metavar="FILE", help="plot the samples to FILE")
    sbb = sub.add_parser("blackbox", help="PD black box ring control (read it with tools/pd_trace.py)")
    sbbs = sbb.add_subparsers(dest="bb_cmd", required=True)
    sbbs.add_parser("status", help="ring fill and frozen state")
    sbbs.add_parser("clear", help="drop the recorded events")
    sbbs.add_parser("freeze", help="stop recording (keeps the ring for a later dump)")
    sbbs.add_parser("resume", help="record again")
    sbbs.add_parser("captures", help="list the trigger-frozen captures (read one with pd_trace.py capture N)")
    sbbs.add_parser("clear-captures", help="drop the trigger-frozen captures")
    stc = sub.add_parser("transcap", help="transition capture: arm / trigger / status (dump and plot: tools/pd_trace.py transition)")
    stcs = stc.add_subparsers(dest="tc_cmd", required=True)
    for nm, hp in (("status", "state, sample count, spacing"), ("arm", "start recording VBUS and CC (fast INA226 mode)"),
                   ("trigger", "freeze after the 400 ms post window"), ("disarm", "stop and drop the capture"),
                   ("save", "write the frozen capture to the volume"), ("list", "the stored captures"),
                   ("clear", "delete the stored captures")):
        stcs.add_parser(nm, help=hp)
    sl = sub.add_parser("lab", help="lab probes (PA8 test load); needs a charger on the bottom port at 5 V")
    sls = sl.add_subparsers(dest="lab_cmd", required=True)
    sll = sls.add_parser("load", help="pulse PA8 at DUTY percent (<= 95) for MS milliseconds (<= 200)")
    sll.add_argument("duty", type=int)
    sll.add_argument("ms", type=int)
    sll.add_argument("--confirm", action="store_true", help="required: acknowledges the PA8 pulse")
    sln = sls.add_parser("sniff", help="passive D+/D- edge capture into the Black Box (never drives; see tools/vooc_edges.py)")
    slns = sln.add_subparsers(dest="sniff_cmd", required=True)
    for nm, hp in (("arm", "start listening; the capture ends 150 ms after the last edge"), ("status", "state and edge count"),
                   ("cancel", "end the session and store what was recorded")):
        slns.add_parser(nm, help=hp)
    slst = slns.add_parser("selftest", help="no phone attached: toggles D+ to prove the EXTI path while armed")
    slst.add_argument("--confirm", action="store_true", help="required: nothing is attached to the ports")
    su = sub.add_parser("ufcs", help="UFCS frame/timing tuner (RAM only) and the 5 V handshake + opener probe; never raises VBUS")
    sus = su.add_subparsers(dest="ufcs_cmd", required=True)
    sus.add_parser("status", help="probe state, last verdict, last start refusal")
    sus.add_parser("show", help="every tuner field: value, range, default")
    sug = sus.add_parser("get", help="one tuner field")
    sug.add_argument("field", choices=[f[0] for f in UFCS_FIELDS])
    suset = sus.add_parser("set", help="set one tuner field (range checked on the K2)")
    suset.add_argument("field", choices=[f[0] for f in UFCS_FIELDS])
    suset.add_argument("value", type=lambda s: int(s, 0))
    sus.add_parser("reset", help="all fields back to the defaults")
    sus.add_parser("probe", help="start the probe (5 V only, refused inline or off 5 V); read it with `ufcs result`")
    sus.add_parser("stop", help="abort a running probe and release the lines")
    sus.add_parser("result", help="the last probe: handshake timings, UART errors, every TX frame and RX byte")
    sc = sub.add_parser("cable-r", help="cable resistance: passive two-point meter or Kelvin 8-wire (never drives VBUS)")
    scs = sc.add_subparsers(dest="cable_cmd", required=True)
    scs.add_parser("status", help="idle reference, loaded point, result")
    scs.add_parser("reset", help="clear the reference and start over")
    scs.add_parser("passive", help="select the passive two-point meter (includes the K2's own path)")
    scs.add_parser("kelvin", help="select the 8-wire Kelvin mode (sense fixture on CC1/CC2/D-, >= 0.45 A)")
    sq = sub.add_parser("qrtest", help="QR scan-test rig (draws a full-screen test QR only): list the fit table, "
                    "stop the test screen, or show one: qrtest VERSION PX [QUIET]")
    sq.add_argument("args", nargs="+", metavar="ARG",
                    help="list | stop | VERSION PX [QUIET] (version 1..40, px 2..3, quiet 1..4, default 4)")
    sw = sub.add_parser("watch", help="poll the state and print one V/A/W line")
    sw.add_argument("--interval", type=int, default=500, help="poll period in ms")
    sw.add_argument("--count", type=int, default=0, help="stop after N lines (0 = forever)")
    return p


def timeout_message(alive):
    if alive:
        return ("the K2 answered ping but the command did not finish in time; "
                "it may still be busy, check `state` or a screenshot")
    return "no response: the K2 may be in DFU, or runs firmware without remote support"


def main(argv=None):
    a = build_parser().parse_args(argv)
    try:
        with k2_lock.hold():     # one command = one hold; the transport open re-enters it
            return _main(a)
    except k2_lock.DeviceLockTimeout as e:
        print(f"K2 busy: {e}", file=sys.stderr)
        return 2


def _main(a):
    try:
        transport = LinuxHidTransport(a.path)
    except Exception as e:
        print(f"cannot open K2: {e}", file=sys.stderr)
        return 2
    link = RemoteLink(transport, a.timeout)
    try:
        result = COMMANDS[a.command](link, a)
        if result is not None:
            print(json.dumps(result) if a.json else render(a.command, result))
        return 0
    except KeyboardInterrupt:
        return 0
    except BootloaderDetected:
        print("The K2 answered with a bootloader frame: it is in DFU mode, not running the "
              "application. Flash with tools/k2_flasher.py or replug it without a button held.",
              file=sys.stderr)
        return 3
    except RemoteTimeout:
        try:
            link.timeout_s = 2.0
            link.call(CMD_PING)
            alive = True
        except (RemoteTimeout, RemoteError, FrameError, BootloaderDetected):
            alive = False
        print(timeout_message(alive), file=sys.stderr)
        return 4
    except FcRefused as e:
        print(f"refused: {e}", file=sys.stderr)
        return 5
    except (StoreFailed, CardsFailed, WipeFailed, DumpFailed) as e:
        print(f"error: {e}", file=sys.stderr)
        return 6
    except (RemoteError, FrameError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        transport.close()


if __name__ == "__main__":
    sys.exit(main())
