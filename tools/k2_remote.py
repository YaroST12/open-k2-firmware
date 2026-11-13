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
import twin_proto  # noqa: E402
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
CMD_CHARGER_SWEEP = 0x0F  # [on(1)] the PPS deep sweep of the next charger run (opt-in, never in a preset)
CMD_CHARGER_AVS_SWEEP = 0x54  # [on(1)] the AVS deep sweep of the next charger run (opt-in, never in a preset)
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
CMD_PD_RESET = 0x42         # optional [flags]: bit0 confirm, bit1 inline lab-only -> [pd::Err]
CMD_PD_TRIGGER_STATUS = 0x43
CMD_PD_MATCH = 0x45  # passive wants-vs-offers verdict
CMD_USB_LINK = 0x46  # USB / Thunderbolt link from sniffed PD
CMD_PD_REREAD = 0x47  # [op, mask] source-cap re-read (PD mode): 0 status, 1 start + mask, 2 abort, 5 Ra mode on/off
CMD_PD_RELEASE = 0x44       # back to 5 V
CMD_PD_ATTACH = 0x48       # [target_mv u16] [target_ma u16] [flags: bit0 confirm] -> trigger snapshot; default 5000 mV, above that needs the flag; refused inline / busy = status + [pd::Err]
CMD_PD_HANDBACK = 0x49     # optional [flags]: bit0 confirm -> [HandbackVerdict]; phase-1 lab hand-back
CMD_APPLE_READ = 0x4A      # lab-only Apple charger readout (SVID 0x05AC): [1] arm + re-request the current contract, [0, part] header (part 0) or one field (1..6)
CMD_STOP = 0x4B            # no payload, no confirm: cancel a charger test, stop FC (measured 5 V release), release PD, disarm a spoof -> [active mask, vbus_mv i32, fc_release_busy]
CMD_LEASE = 0x4C           # host dead-man lease: [0] status, [1, seconds u16] set the length (0 = default 30 s, max 600) -> [armed, left_s u16, length_s u16, ended]
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
CMD_TWIN = 0x63           # device twins: list / get (chunked) / predict / learn / rename / delete / run / status / result / usb / cancel
(TW_LIST, TW_GET, TW_PREDICT, TW_LEARN, TW_RENAME, TW_DELETE, TW_RUN, TW_STATUS, TW_RESULT, TW_USB, TW_CANCEL) = range(11)
TW_RUN_CONFIRM, TW_LEARN_DRY, TW_ERR_NO_CAPS = 1, 1, 8
TW_RUN_WAIT_S = 180       # how long `twin run` follows a run before it gives up waiting (the run itself goes on)
CMD_STORE = 0x60         # flash store: list / save-emarker / save-log
CMD_BLACK_BOX = 0x70       # PD black box ring: read from a cursor / clear / freeze / status (tools/pd_trace.py)
BB_READ, BB_CLEAR, BB_FREEZE, BB_STATUS, BB_CAP_LIST, BB_CAP_READ, BB_CAP_CLEAR = 0, 1, 2, 3, 4, 5, 6
CMD_TRANS_CAP = 0x71      # transition capture: VBUS + CC at ~1 kHz around a PD step (tools/pd_trace.py transition)
TC_STATUS, TC_ARM, TC_TRIGGER, TC_DISARM, TC_SAVE, TC_LIVE, TC_LIST, TC_STORED, TC_CLEAR = range(9)
CMD_DIAGNOSE = 0x72       # Black Box diagnosis (read-only): status / line / finding (also tools/pd_trace.py diagnose)
DG_STATUS, DG_LINE, DG_FINDING = range(3)
CMD_LAB = 0x51            # lab probes: PA8 load pulse
CMD_CABLE_R = 0x50        # passive cable resistance (two-point method), status / reset
CMD_UFCS = 0x52           # UFCS frame/timing tuner (RAM only) and the 5 V handshake + opener probe (tools/ufcs_sweep.py)
CMD_QRTEST = 0x53         # QR scan-test rig (k2/ui/ui_qrtest.hpp): fit table / full-screen test QR / stop; draws only
QRTEST_LIST, QRTEST_SHOW, QRTEST_STOP = 0, 1, 2
QRTEST_CHUNK = 48
CMD_EMARKER_PROBE = 0x56  # silent-cable-chip probe: optional [vconn_pc]; raw ADC samples of the idle and VCONN phases (k2/pd/ra_probe.hpp)
# raprobe::Refusal and raprobe::ArgError, index = the byte the K2 sends; the text is what the operator should read
RA_PROBE_REFUSALS = ["", "a sink is inline (a charger or load downstream)", "a PD contract is active",
                     "Rd is presented (a boot-mode pin or an engine holds it)", "the Ra mode is on",
                     "an identify / probe / re-read / VCONN experiment is running", "VBUS reading unavailable",
                     "VBUS is 1 V or more: something powers the port", "the chip bus did not answer",
                     "a source level sits on a CC line (a charger waiting for a sink?): VCONN was not applied"]
RA_PROBE_ARG_ERRORS = ["", "vconn_pc above 7", "vconn_pc has PC13 set (an Ra on the CC1 net, never together with VCONN)"]
RA_PROBE_PHASES = ["idle", "vconn_cc1", "vconn_cc2"]
RA_PROBE_SAMPLE_MS = [2, 10, 100]      # k2::raprobe::kSampleMs, per phase
RA_PROBE_TIMEOUT_S = 3.0               # the K2 blocks for the ~330 ms plan
# ---- PD Stress (docs/PD_STRESS.md): constants ----
CMD_PD_STRESS = 0x55      # PD Stress: status / start / result (chunked) / stop (k2/core/remote_proto.hpp, tools/pd_stress.py)
PDS_STATUS, PDS_START, PDS_READ, PDS_STOP = 0, 1, 2, 3
PDS_CHUNK = 48
PDS_FLAG_CONFIRM = 1 << 0
PDS_STATES = ["idle", "resuming", "running", "finished"]
# Same order as k2::pdstress::StartResult / Stop (include/k2/pdstress/engine.hpp); tests/test_pd_stress.py checks the sync.
PDS_STARTS = ["ok", "ports busy", "no PD source", "link inline", "no sequence applies", "no record buffer", "already running",
              "no power monitor"]
PDS_STOPS = ["running", "done", "cancelled", "run time cap", "transition cap", "second source hard reset", "second K2 hard reset",
             "VBUS over the limit", "load on the output", "no recovery without a replug", "step stuck", "no PD contract",
             "link inline", "too many K2 reboots"]
# ---- end PD Stress constants ----
# ---- Loop probe (docs/LOOP_PROBE.md): constants ----
CMD_LOOP_PROBE = 0x57     # loop probe: the stored result file /lp/last.rec, chunked read only (k2/loopprobe/loop_probe_hw.hpp)
LOOP_CHUNK = 48           # remote::kLoopProbeChunk
CMD_BATTERY = 0x58        # battery efficiency recorder: status / start / end / next / finish / config / list / read (k2/battery)
BATTERY_CHUNK = 48        # battery::wire::kChunk
# ---- end Loop probe constants ----
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
                5: "needs --confirm: this raises the output above 5 V or drives D+/D-"}

BUTTONS = {"back": 0, "minus": 1, "ok": 2, "plus": 3}
ACTIONS = {"pd-rescan": 1, "emarker": 2, "energy-reset": 3, "fc-scan": 4, "fc-reset": 5, "identify": 6}
# Actions the firmware refuses without the confirm byte (remote::action_needs_confirm): they raise VBUS above 5 V.
ACTIONS_NEED_CONFIRM = ("fc-scan",)
SOPS = {"sop": 0, "sop1": 1, "sop2": 2}
# Rotation pages (settable with `page`), then Tools/Settings submenus: the state reports a
# showing submenu as 6 + its index. Connection and Diag can also be opened remotely.
PAGES = ["home", "detail", "chargertest", "lines", "tools", "settings", "trigger", "emarker", "pdtrigger", "cabler", "blackbox", "spoof", "usblink", "autorotate", "cards", "share", "connection", "diag", "qrtest", "twin", "loopprobe", "pdstress", "diagnosis", "battery"]
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
PD_ERR_INLINE = PD_ERRS.index("inline: a live sink is attached")   # pd::Err::Inline, carried by refusals' payload byte 0
# pd::HandbackVerdict in order: the reply payload byte of CMD_PD_HANDBACK (allowed or the refusal reason)
# pd::InlineCause in order (include/k2/pd/inline_cause.hpp, inline_cause_name): what latched the inline state
INLINE_CAUSES = ["none", "load", "sink request", "vbus rise", "foreign vbus", "far rd"]
HANDBACK_VERDICTS = ["allowed", "charger test running", "confirm needed", "not inline", "no contract owned",
                     "far sink Request seen this attach", "already handed back this attach"]
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
FC_OP_VOOC = 4             # vooc-monitor: the passive inline VOOC monitor readout (see decode_vooc_summary)
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
                                             # "refuse" = the Ra guard said no (a cable is on the plug): start anyway, or skip the spoof for this run;
                                             # "waiting" = START presented Rd and waits for the charger (OK or BACK cancels it)
TEST_ALL = (1 << len(TEST_ALGORITHMS)) - 1
TEST_SWEEP_BIT = 1 << 31   # draft_selected bit 31 = the PPS deep sweep on (remote_proto.hpp)
TEST_AVS_SWEEP_BIT = 1 << 30   # draft_selected bit 30 = the AVS deep sweep on (remote_proto.hpp)
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
                "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target", "withdrawn by source", "wrong voltage on 5V contract", "needs load", "floored above target", "step resolution", "HID-cable loop: 5V lock"]
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


SNIFF_REFUSED_PORTS, SNIFF_REFUSED_NO_RAM, SNIFF_REFUSED_SVOOC = 1, 2, 3   # remote::SniffRefusal: the one byte of a refused `lab sniff`


class RemoteError(RuntimeError):
    def __init__(self, status, cmd, payload=b""):
        self.status, self.cmd, self.payload = status, cmd, payload
        text = f"command {cmd} failed: {STATUS_NAMES.get(status, status)}"
        if cmd == CMD_CHARGER_TEST and status == STATUS_BUSY:
            text += " (RAM results changed; retry charger-test results)"
        if cmd == CMD_PD_REREAD and status == STATUS_BUSY:
            text += " (a charger test, PD Stress or twin run owns the link, or a far sink is inline: nothing was changed; a run's end releases its own attach)"
        if cmd == CMD_DEBUG_CC_PROBE and status == STATUS_BUSY:
            text += " (the K2 is not passive: a charger test, a contract, Rd, VBUS at 1 V or more; the [CCPROBE] log line names the reason)"
        if cmd == CMD_DEBUG_CC_PROBE and status == STATUS_BAD_ARG:
            text += " (sw0 with a pull-up bit, a VCONN bit with PC13 high or with PDWN on the same line; ms 10..5000; pc 0..7)"
        if cmd == CMD_EMARKER_PROBE and status == STATUS_BUSY:
            text += (f" (refused, nothing was driven: {RA_PROBE_REFUSALS[payload[0]]})" if payload and 0 < payload[0] < len(RA_PROBE_REFUSALS)
                     else " (the K2 is not passive: a charger test is running, or the port is not idle)")
        if cmd == CMD_EMARKER_PROBE and status == STATUS_BAD_ARG and payload and 0 < payload[0] < len(RA_PROBE_ARG_ERRORS):
            text += f" ({RA_PROBE_ARG_ERRORS[payload[0]]})"
        if cmd in (CMD_PD_REQUEST, CMD_PD_RESET, CMD_PD_RELEASE, CMD_PD_ATTACH) and payload and payload[0] < len(PD_ERRS) and payload[0]:
            text += f" ({PD_ERRS[payload[0]]})"
        if cmd == CMD_EM_LIB and status == STATUS_BUSY:
            text += " (a charger test or an FC release owns the link: the library write waits for it)"
        if cmd == CMD_STORE and status == STATUS_BUSY:
            text += " (a charger test or an FC release owns the link: the flash save waits for it)"
        if cmd == CMD_TWIN and status == STATUS_BUSY:
            text += " (a twin run, a charger test or an FC release owns the link: the write waits for it; `twin status`, `twin cancel`)"
        if cmd == CMD_ACTION and status == STATUS_BUSY:
            text += " (a charger test or an FC release owns the ports, or a negotiation is in flight: the action did not run)"
        if cmd == CMD_IDENTIFY_DEVICE and payload and payload[0] == PD_ERR_INLINE:
            # the inline Identify refusal (BadArg + pd::Err::Inline), not "busy": a live sink is
            # attached, the K2 transmits nothing, the Sink tab shows the sniffed identity
            text = f"command {cmd} failed: refused: inline - the Sink tab shows the sniffed identity"
        if cmd == CMD_PD_HANDBACK and payload and payload[0] < len(HANDBACK_VERDICTS):
            text += f" ({HANDBACK_VERDICTS[payload[0]]})"
        if cmd == CMD_APPLE_READ and payload and payload[0] < len(APPLE_REFUSALS):
            text += f" ({APPLE_REFUSALS[payload[0]]})"   # the arm refusal reason (pd::AppleRefusal)
        if cmd == CMD_LAB and status == STATUS_BUSY and payload and payload[0] == SNIFF_REFUSED_NO_RAM:
            text = f"command {cmd} failed: refused: no RAM: USB drive enabled (switch the drive off in Tools > USB link and replug)"
        elif cmd == CMD_LAB and status == STATUS_BUSY and payload and payload[0] == SNIFF_REFUSED_PORTS:
            text += " (a trigger, scan or test owns D+/D-)"
        elif cmd == CMD_LAB and status == STATUS_BUSY and payload and payload[0] == SNIFF_REFUSED_SVOOC:
            text += " (the SuperVOOC trigger drives D-: `fc svooc-stop` or wait for it to end)"
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
                   "cable_age_s": int.from_bytes(p[45:49], "little"),
                   "crossed": bool(lf & 32), "flip_hint": bool(lf & 64),   # kLinkCrossed / kLinkFlipHint: flip one cable
                   "no_pd": bool(lf & 128)})   # kLinkNoPd: the settled "no PD this attach" verdict (Row C)
    if len(p) >= 53:                              # why this boot happened (CTRLSTS flags, cleared after capture)
        flags = int.from_bytes(p[49:53], "little")
        st.update({"reset_flags": flags, "reset_cause": reset_cause_name(flags)})
    if len(p) >= 54:                              # what latched the inline state (pd::InlineCause), after the reset flags
        st["inline_cause"] = INLINE_CAUSES[p[53]] if p[53] < len(INLINE_CAUSES) else str(p[53])
    return st


# GET_STATE part 1 (the session block): the enums follow include/k2/ui/ui_types.hpp, fc_proto.hpp, home.hpp and conn.hpp.
SESSION_PROTO_KINDS = ["none", "pd-fixed", "pd-epr", "pps", "avs", "qc2", "qc3", "fcp", "scp", "ufcs", "svooc",
                       "typec", "dcp", "unknown", "detecting"]
SESSION_BUSY = ["none", "test-running", "fc-release", "qc-trigger", "pd-hold", "epr", "pd-request", "em-scan", "identify", "twin-run", "pd-stress", "loop-probe"]
SESSION_TOPOLOGY = ["unknown", "no-source", "cable-single-cc", "direct-receptacle"]
SESSION_WAIT_KIND = ["unknown", "no-source", "charger-waiting", "dual-role-port"]
SESSION_PD_NONE_WHY = ["none", "sink-silent-source-offered", "source-never-offered"]
SESSION_TABS = ["source", "cable", "sink"]
SESSION_HOME_WINDOWS = ["10s", "60s", "10m"]
SESSION_FLAGS = ["hold_card", "rotation_notice", "em_scanning", "em_listening", "em_quiet", "id_no_device"]
SESSION_LEN = 31
MENU_NAMES = PAGES[ROTATION_PAGES:]                       # Menu order: the stack holds Menu indices


def decode_session(p):
    """GET_STATE [1]: energy / protocol / busy / topology / navigation (src/core/remote_proto.cpp encode_session)."""
    _need(p, SESSION_LEN, "state session")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    stack = []
    for m in p[23:23 + min(p[22], 3)]:
        stack.append(MENU_NAMES[m] if m < len(MENU_NAMES) else str(m))
    sub = p[26]
    ui = {"stack": stack, "tab": SESSION_TABS[sub & 3] if (sub & 3) < 3 else None,
          "popup": bool(sub & 4), "details": bool(sub & 8),
          "sel": None if p[27] == 0xFF else p[27], "count": p[28]}
    # The tab only means something on Connection; everywhere else it is zero and would read as "source".
    if not stack or stack[-1] != "connection":
        ui["tab"] = None
    return {"mah": int.from_bytes(p[0:4], "little", signed=True), "elapsed_s": int.from_bytes(p[4:8], "little"),
            "proto": {"kind": _name(dict(enumerate(SESSION_PROTO_KINDS)), p[8]), "limit_ma": u16(9),
                      "max_mv": u16(11)},
            "typec_ma": u16(13), "busy": _name(dict(enumerate(SESSION_BUSY)), p[15]), "fc_release": bool(p[16]),
            "topology": _name(dict(enumerate(SESSION_TOPOLOGY)), p[17]),
            "wait_kind": _name(dict(enumerate(SESSION_WAIT_KIND)), p[18]),
            "pd_none_why": _name(dict(enumerate(SESSION_PD_NONE_WHY)), p[19]),
            "open_cc_mv": None if u16(20) == 0xFFFF else u16(20),
            "ui": ui, "home_window": _name(dict(enumerate(SESSION_HOME_WINDOWS)), p[29]),
            "flags": {name: bool(p[30] >> i & 1) for i, name in enumerate(SESSION_FLAGS)}}


DIAG_LEN = 26        # a build before the HID-cable loop guard
DIAG_LOOP_LEN = 29   # + loop(1) loop_events(2)
LOOP_REASONS = {0: "none", 1: "certain", 2: "suspected"}   # k2::loopguard::Reason


def decode_diag(p):
    """GET_STATE [2]: the Diag page counters (src/core/remote_proto.cpp encode_diag). poll_gap_max_ms is the longest
    main-loop gap since the last [USB] heartbeat line, as the page shows it. "loop" is the HID-cable loop 5 V lock
    (k2/core/loop_guard.hpp), absent on a build before it."""
    _need(p, DIAG_LEN, "state diag")
    if len(p) > DIAG_LEN:
        _need(p, DIAG_LOOP_LEN, "state diag loop block")
    u32 = lambda o: int.from_bytes(p[o:o + 4], "little")
    out = {"fusb_id": p[0], "pd_rx": u32(1), "usb_resets": u32(5), "usb_isrs": u32(9), "log_sent": u32(13),
           "log_dropped": u32(17), "poll_gap_max_ms": u32(21), "usb_configured": bool(p[25])}
    if len(p) >= DIAG_LOOP_LEN:
        why = p[26] & 3
        out["loop"] = {"locked": why != 0, "reason": LOOP_REASONS.get(why, f"reason {why}"), "host": bool(p[26] & 8),
                       "events": int.from_bytes(p[27:29], "little")}   # bit 2 reserved
    return out


KEYS_LEN = 57
KEYS_BUTTONS = ("back", "minus", "ok", "plus")
KEY_CHARS = 7


def decode_keys(p):
    """GET_STATE [3]: what the four buttons do on the page on screen (src/core/remote_proto.cpp encode_keys). Per button the
    short- and long-press word (ASCII, NUL padded, empty = nothing) and `dim` (shown but unavailable). `hidden` = the
    page covers the strip (the QR code), so none of the four is drawn. The words are the page's labels, not the drawn
    text: a narrow slot drops the long word."""
    _need(p, KEYS_LEN, "state keys")
    word = lambda o: bytes(p[o:o + KEY_CHARS]).split(b"\0", 1)[0].decode("ascii", "replace")
    out = {name: {"short": word(1 + 14 * i), "long": word(8 + 14 * i), "dim": bool(p[0] >> i & 1)}
           for i, name in enumerate(KEYS_BUTTONS)}
    out["hidden"] = bool(p[0] >> 4 & 1)
    return out

STOP_ACTIVE = ((1, "test"), (2, "fc"), (4, "pd"), (8, "spoof"))   # k2/core/stop.hpp: what the stop found active


def decode_stop(p):
    """STOP reply: [active mask, vbus_mv i32 LE (-1 = monitor not ready), fc_release_busy]."""
    _need(p, 6, "stop")
    return {"active": [name for bit, name in STOP_ACTIVE if p[0] & bit], "active_mask": p[0],
            "vbus_mv": int.from_bytes(p[1:5], "little", signed=True), "fc_release_busy": bool(p[5])}


def cmd_stop(link, a):
    """Undo everything a host left holding the output: one call, no confirm (every step moves toward 5 V / idle)."""
    return decode_stop(link.call(CMD_STOP).payload)


# The host dead-man lease (k2/core/lease.hpp). A command that can leave the output above 5 V under host control (pd request /
# attach above 5 V, fc trigger above 5 V, fc scan) arms it; every later request refreshes it; when the host goes quiet for the
# lease (default 30 s) or the USB link drops, the K2 runs the `stop` path itself. `--lease N` sets the length for that command
# (on the wire: a separate lease-set request [1, N u16 LE] sent just before the command, applied only if the command arms
# within 5 s; N = 0 means the firmware default); `--hold` keeps this tool pinging until Ctrl-C, then sends `stop`.
LEASE_MAX_S = 600
LEASE_ENDED = ("none", "expired", "host gone", "released", "settled", "test running")


def decode_lease(p):
    """LEASE reply: [armed, left_s u16 LE, length_s u16 LE, ended]."""
    _need(p, 6, "lease")
    return {"armed": bool(p[0]), "left_s": int.from_bytes(p[1:3], "little"), "length_s": int.from_bytes(p[3:5], "little"),
            "ended": LEASE_ENDED[p[5]] if p[5] < len(LEASE_ENDED) else str(p[5])}


def _lease_secs(text):
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text!r}") from None
    if not 0 <= n <= LEASE_MAX_S:
        raise argparse.ArgumentTypeError(f"{n} is outside 0..{LEASE_MAX_S} (0 = the firmware default, 30 s)")
    return n


def cmd_lease(link, a):
    """Read the lease (no argument) or set its length in seconds (0 = the default)."""
    secs = getattr(a, "seconds", None)
    payload = b"\x00" if secs is None else bytes([1]) + secs.to_bytes(2, "little")
    return decode_lease(link.call(CMD_LEASE, payload).payload)


def lease_line(info):
    return (f"lease armed: {info['length_s']} s ({info['left_s']} s left); the K2 stops the output (same as `stop`) if no "
            f"request reaches it for that long or the USB link drops. Refresh: any command, or `k2_remote lease`; "
            f"end: `stop`, `pd release`, or --hold with Ctrl-C")


def _lease_status(link):
    """The lease after an arming command; None on a firmware without the lease command (it answers an error)."""
    try:
        return decode_lease(link.call(CMD_LEASE, b"\x00").payload)
    except RemoteError:
        return None


def hold_lease(link, info, sleep=time.sleep, out=print):
    """--hold: ping (a lease status read) until Ctrl-C, then `stop`. Returns the stop result, or None when the lease
    ended by itself (a stop then could cancel a charger test that took the ports)."""
    if info is None or not info["armed"]:
        out("nothing to hold: the command did not arm a lease (it left the output at 5 V or was refused)")
        return None
    interval = max(1.0, min(10.0, info["length_s"] / 3))
    out(f"holding the lease: a request every {interval:g} s; Ctrl-C sends stop")
    try:
        while True:
            sleep(interval)
            st = decode_lease(link.call(CMD_LEASE, b"\x00").payload)
            if not st["armed"]:
                out(f"lease ended: {st['ended']}; no stop sent")
                return None
    except KeyboardInterrupt:
        pass
    r = cmd_stop(link, None)
    out(render("stop", r))
    return r


def run_command(link, a, sleep=time.sleep, out=print, show=None):
    """Runs one parsed command. The commands that can arm the lease (they carry --lease / --hold) get the length set
    first, and the lease read back after: one `out` line, and a "lease" key in the result, when it armed. `show` prints
    the result before the lease line and the hold; `out` carries the human lines (stderr with --json)."""
    arming = hasattr(a, "hold")
    secs = getattr(a, "lease", None)
    if arming and secs is not None:
        link.call(CMD_LEASE, bytes([1]) + secs.to_bytes(2, "little"))
    result = COMMANDS[a.command](link, a)
    info = _lease_status(link) if arming else None
    if info is not None and info["armed"] and isinstance(result, dict):
        result = {**result, "lease": info}
    if show is not None and result is not None:
        show(result)
    if info is not None and info["armed"]:
        out(lease_line(info))
    if arming and a.hold:
        hold_lease(link, info, sleep, out)
    return result


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


ANSWERED_ORIENTATION = {1: "A", 2: "B"}
ANSWERED_ORIENTATION_TEXT = {"A": "A (CC1 BMC, CC2 VCONN)", "B": "B (CC2 BMC, CC1 VCONN)"}


def decode_answered(b):
    """The answered tail of part 2 and of the learn summary: [orientation (1 A, 2 B), PD header revision (1 PD 2.0,
    2 PD 3.x), SVDM (1, 2), attempt (1-based within the orientation)]. Fewer than 4 bytes (older firmware) or no
    orientation (nothing answered) gives every field None."""
    if b is None or len(b) < 4 or b[0] not in ANSWERED_ORIENTATION:
        return {"orientation": None, "header_rev": None, "svdm": None, "attempt": None}
    return {"orientation": ANSWERED_ORIENTATION[b[0]], "header_rev": {1: "PD 2.0", 2: "PD 3.x"}.get(b[1]),
            "svdm": {1: "1.0", 2: "2.0"}.get(b[2]), "attempt": b[3] or None}


READ_MODES = {0: "none", 1: "alone", 2: "attached"}


def decode_read(b):
    """The read tail of part 2 and of the learn summary: [read mode (0 none, 1 cable alone, 2 source attached with the
    K2's Rd kept), query variants the chip acknowledged]. Fewer than 2 bytes (older firmware) gives every field None;
    `attached` is None as well when no read ran (mode 0)."""
    if b is None or len(b) < 2:
        return {"read_mode": None, "attached": None, "acks": None}
    m = b[0]
    return {"read_mode": READ_MODES.get(m, str(m)), "attached": (m == 2) if m in (1, 2) else None, "acks": b[1]}


def read_line(r):
    """`read: source attached (Rd kept), 1 variant(s) acknowledged, acked, no answer`, or "" when not known."""
    if r.get("attached") is None:
        return ""
    s = "read: source attached (Rd kept)" if r["attached"] else "read: cable alone"
    if r["acks"]:
        s += f", {r['acks']} variant(s) acknowledged"
        if r.get("orientation") is None:
            s += ", acked, no answer"
    return s


PULL_DOWNS = {0: "not-probed", 1: "none", 2: "cc1", 3: "cc2", 4: "unclear"}


def decode_pull_down(b):
    """The pull-down verdict (last byte of part 2 and of the learn summary): what the Ra probe made of a chip's
    pull-down after a cable-alone read that found nothing. 0 not probed, 1 none, 2 on CC1, 3 on CC2, 4 unclear.
    Fewer than 1 byte (older firmware) gives None; an unknown code is kept as its number."""
    if b is None or len(b) < 1:
        return {"pull_down": None}
    return {"pull_down": PULL_DOWNS.get(b[0], str(b[0]))}


def pull_down_line(r):
    """The sentence for a silent cable: a chip's pull-down seen (the chip is there but did not answer) versus none
    found. "" when not probed, not known or nothing to add."""
    v = r.get("pull_down")
    if v in ("cc1", "cc2"):
        return (f"no answer, but a chip's pull-down is on {v.upper()}: read this cable with the charger attached, "
                "or try its other end")
    if v == "none":
        return "no chip found"
    if v == "unclear":
        return "pull-down unclear: no verdict on whether a chip is present (retry with the cable seated)"
    if v not in (None, "not-probed"):
        return f"pull-down verdict {v} (unknown to this tool)"
    return ""


def answered_line(r):
    """`answered: orientation A (CC1 BMC, CC2 VCONN), PD 3.x header, SVDM 1.0, attempt 2`, or "" when not known."""
    if r.get("orientation") is None:
        return ""
    return (f"answered: orientation {ANSWERED_ORIENTATION_TEXT[r['orientation']]}, {r['header_rev'] or '?'} header, "
            f"SVDM {r['svdm'] or '?'}, attempt {r['attempt'] or '?'}")


def decode_emarker_detail(p2):
    """Part 2: cable VDO detail. Codes are named here, the firmware sends numbers. 10 bytes from older firmware,
    14 with the answered tail, 16 with the read tail, 17 with the pull-down verdict."""
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
            "max_temp_c": p2[6], "shutdown_temp_c": p2[7], "u3_power": p2[8], "usb_lanes": p2[9],
            **decode_answered(p2[10:14]), **decode_read(p2[14:16]), **decode_pull_down(p2[16:17])}


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
    # source_hard_resets (u32, appended after hard_resets), pin (u8) and the direct-plug line trial (line, moves: u8 each)
    # are absent from older firmware: 0 then
    return {"state": _name_of(SINK_STATES, p[0]), "result": _name_of(SINK_RESULTS, p[1]), "held": bool(p[2]),
            "epr": bool(p[3]), "pos": p[4], "mv": u16(5), "ma": u16(7),
            "hard_resets": int.from_bytes(p[9:13], "little"),
            "source_hard_resets": int.from_bytes(p[13:17], "little") if len(p) >= 17 else 0,
            "pin": p[17] if len(p) >= 18 else 0,
            "trial_line": p[18] if len(p) >= 19 else 0,    # CC line our single Rd is trying while the direct-plug schedule is live
            "trial_moves": p[19] if len(p) >= 20 else 0,   # moves of the Rd by the direct-plug fallback this attach
            "auto_hold": bool(p[20]) if len(p) >= 21 else False}   # the hold is the K2's own PDO 1 answer, not a trigger


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


# --- the Connection page Sink tab, mirrored host-side (k2/pd/pd_tabs.hpp + ui_connection_page.cpp sink_rows) ---

def _fmt_watts(w):
    return "?" if w < 0 else f"{w}W"


def _fmt_rate(mbps):
    return "?" if mbps < 0 else f"{mbps // 1000}G" if mbps >= 1000 else f"{mbps}M"


def _terms_limit(t):
    """pdtabs::limit: the smallest known term and which party holds it below another known one."""
    v = (t.get("source", -1), t.get("cable", -1), t.get("sink", -1))
    known = [i for i, x in enumerate(v) if x >= 0]
    if not known:
        return -1, None
    lo = min(known, key=lambda i: v[i])
    who = None
    if len(known) > 1 and any(v[i] > v[lo] for i in known):
        who = ("source", "cable", "sink")[lo]
    return v[lo], who


def _power_line(t, cable_assumed):
    """pdtabs::power_line: "18W of 100W", "60W by cable" ("?" when the cable is only assumed), "100W", "?"."""
    value, who = _terms_limit(t)
    if value < 0:
        return "?"
    s = _fmt_watts(value)
    if who == "cable":
        s += " by cable" + ("?" if cable_assumed else "")
    elif who == "sink":
        s += " of " + _fmt_watts(t["source"] if t.get("source", -1) >= 0 else t.get("cable", -1))
    return s


def _data_line(t, dp):
    """pdtabs::data_line over the cable and sink terms only: "5G of 10G", "5G by cable", "10G cable", "480M"."""
    d = {"cable": t.get("cable", -1), "sink": t.get("sink", -1)}
    value, who = _terms_limit(d)
    if value < 0:
        return "?"
    s = _fmt_rate(value)
    if who == "cable":
        s += " by cable"
    elif who == "sink":
        s += " of " + _fmt_rate(d["cable"])
    elif d["sink"] < 0:
        s += " cable"
    return s + " +DP" if dp else s


def _cable_mbps(bits, vid):
    """pdtabs::cable_mbps: the e-marker's Cable VDO 1 speed bits; Thunderbolt cables from Gen2 up are 40G."""
    if vid == 0x8087 and bits >= 2:
        return 40000
    return {1: 5000, 2: 10000, 3: 40000, 4: 80000}.get(bits, 480)


def _raw_pdo_summary(raw):
    """One sink-cap PDO word as the tab's Caps row words it (ui_connection_page.cpp put_sink_pdos)."""
    kind = raw >> 30
    if kind == 0:
        return f"{(raw >> 10 & 0x3FF) * 50 / 1000:g}V {(raw & 0x3FF) * 10 / 1000:g}A"
    if kind == 1:
        return f"{(raw >> 10 & 0x3FF) * 50 / 1000:g}-{(raw >> 20 & 0x3FF) * 50 / 1000:g}V {(raw & 0x3FF) * 250 / 1000:g}W"
    if kind == 2:
        return f"{(raw >> 10 & 0x3FF) * 50 / 1000:g}-{(raw >> 20 & 0x3FF) * 50 / 1000:g}V {(raw & 0x3FF) * 10 / 1000:g}A"
    sub = raw >> 28 & 3
    if sub == 0:
        return f"{(raw >> 8 & 0xFF) * 100 / 1000:g}-{(raw >> 17 & 0xFF) * 100 / 1000:g}V {(raw & 0x7F) * 50 / 1000:g}A"
    if sub == 1:
        return f"{(raw >> 8 & 0xFF) * 100 / 1000:g}-{(raw >> 17 & 0x1FF) * 100 / 1000:g}V {raw & 0xFF}W"
    return f"AVS 0x{raw:08X}"


def _request_row(m, pdos):
    """ui_connection_page.cpp put_request: "PDO3 9V 3A", "PPS6 8.88V 2.05A", "BAT5 15V 45W"."""
    pos, mv, ma = m["want_pos"], m["want_mv"], m["want_ma"]
    p = pdos[pos - 1] if 0 < pos <= len(pdos) and pdos[pos - 1] else None
    ty = p["type"] if p else "fixed"
    head = {"pps": "PPS", "avs": "AVS", "battery": "BAT"}.get(ty, "PDO")
    mw = mv * ma // 1000
    tail = f"{mw // 1000}W" if ty == "battery" else f"{ma / 1000:g}A"
    return {"pos": pos, "type": ty, "mv": mv, "ma": ma, "mw": mw, "text": f"{head}{pos} {mv / 1000:g}V {tail}"}


def cmd_sink(link, a):
    """Connection page Sink tab, field for field, from the same model the tab renders (sink_rows).
    Composed from the existing read-only commands only; Identify is polled with op 0 (status, never starts)."""
    st = decode_state(link.call(CMD_GET_STATE).payload)
    pd = decode_pd_trigger(link.call(CMD_PD_TRIGGER_STATUS).payload)
    m = decode_match(link.call(CMD_PD_MATCH).payload)
    head, first = decode_pdo(link.call(CMD_GET_PDOS, bytes([0])).payload)
    pdos = [first] if first else []
    for i in range(1, head["num_pdos"]):
        pdos.append(decode_pdo(link.call(CMD_GET_PDOS, bytes([i])).payload)[1])
    ul = decode_usb_link(link.call(CMD_USB_LINK).payload)
    em = decode_emarker(link.call(CMD_GET_EMARKER, bytes([0])).payload,
                        link.call(CMD_GET_EMARKER, bytes([1])).payload,
                        link.call(CMD_GET_EMARKER, bytes([2])).payload)
    idp = decode_identify_progress(link.call(CMD_IDENTIFY_DEVICE, bytes([0])).payload)

    not_exposed = ["sniffed sink identity (ID row)", "contract state line + RDO flags (State row)",
                   "sink SVIDs (Proto row)", "Row C no-PD verdict (wait-kind classifier)"]
    caps = [s for raw in idp["sink_pdos"] for s in [_raw_pdo_summary(raw)] if raw]
    if not m["caps_seen"]:
        not_exposed.append("typec_ma (Pwr budget row without a PD contract)")

    # strip_rows: the Pwr / Data terms, worded by who holds the number down
    pwr = None
    if m["caps_seen"]:
        t = {"source": m["src_max_w"], "cable": em["max_power_w"] if em["valid"] else 60,
             "sink": m["want_mv"] * m["want_ma"] // 1000000 if m["request_seen"] else -1}
        pwr = _power_line(t, not em["valid"])
    dterms = {"cable": _cable_mbps(em["vdos"][4] & 7, em["vid"]) if em["valid"] and len(em["vdos"]) > 4 else -1,
              "sink": ul["mbps"] if ul["family"] != "unknown" and ul["mbps"] > 0 else -1}
    value, who = _terms_limit(dterms)
    data = None
    if m["caps_seen"] and value >= 0 and (who or ul["dp"] or (dterms["cable"] >= 0 and dterms["sink"] < 0)):
        data = _data_line(dterms, ul["dp"])

    return {"tab": "sink",
            "k2_sink": pd["pin"] != 0,          # fusb302::self_powered() == pin_rd() != 0
            "k2_sink_ok": pd["pin"] == 1,
            "link": {k: st.get(k) for k in ("inline", "waiting", "crossed", "flip_hint", "no_pd", "vbus_mv",
                                            "pd_valid", "spec_rev", "num_pdos", "direction")},
            "identify": idp,
            "id": {"sniffed": None, "asked": idp["state"]},
            "request": _request_row(m, pdos) if m["request_seen"] else None,
            "state_line": None, "flags": m["flags"], "verdict": m["verdict"],
            "pwr": pwr, "data": data,
            "usb": ul, "cable": em,
            "caps": caps, "caps_src": "from Identify" if caps else "",
            "proto": None, "no_pd_verdict": "none" if st.get("no_pd") else None,   # only the settled None is on the wire
            "not_exposed": not_exposed}


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
        out = decode_reread(link.call(CMD_PD_REREAD, bytes([4, line])).payload)   # it stays for later attaches (charger test START) until --line 0 / --abort
        if getattr(a, "no_run", False):
            return out
    out = decode_reread(link.call(CMD_PD_REREAD, bytes([1, mask])).payload)
    deadline = clock() + getattr(a, "wait", REREAD_POLL_S)
    while out["state"] == "running" and clock() < deadline:
        sleep(0.1)
        out = decode_reread(link.call(CMD_PD_REREAD, bytes([0])).payload)
    return out


# ---- lab-only Apple charger readout (SVID 0x05AC, k2/pd/apple_readout.hpp) ----
APPLE_WHYS = ["running", "done", "nak", "timeout", "range", "cap", "nomode"]
APPLE_REFUSALS = ["none", "inline", "no contract", "not partner", "busy"]
APPLE_FIELDS = ["maker", "product", "fw", "hw", "field90", "serial"]
APPLE_POLL_S = 8.0   # the source drives its 0x05AC discovery ~1.4 s after the re-request; a run is at most 16 reads x ~20 ms


def decode_apple_read_head(p):
    """APPLE_READ part 0: armed, why, reads, ok, nfields (see encode_apple_read)."""
    _need(p, 5, "apple-read")
    return {"armed": bool(p[0]), "why": _name_of(dict(enumerate(APPLE_WHYS)), p[1]),
            "reads": p[2], "ok": p[3], "nfields": p[4]}


def decode_apple_read_field(p, part):
    """APPLE_READ part 1..6: [part] + one NUL-terminated field."""
    _need(p, 2, "apple-read")
    if p[0] != part:
        raise FrameError(f"apple-read field part {p[0]} != asked {part}")
    return _cstr(p[1:])


def _apple_read_all(link):
    """The whole readout: the header plus every field chunk."""
    head = decode_apple_read_head(link.call(CMD_APPLE_READ, bytes([0, 0])).payload)
    out = {k: head[k] for k in ("armed", "why", "reads", "ok")}
    for part in range(1, head["nfields"] + 1):
        out[APPLE_FIELDS[part - 1]] = decode_apple_read_field(link.call(CMD_APPLE_READ, bytes([0, part])).payload, part)
    return out


def cmd_apple_read(link, a, sleep=time.sleep, clock=time.monotonic):
    """Lab-only, off by default: arm the Apple charger readout - our SVIDs ACK adds 0x05AC and the sink
    re-requests the CURRENT contract (never a different voltage), so the source re-runs its 0x05AC
    discovery - then poll until the run ends and read the fields back."""
    head = decode_apple_read_head(link.call(CMD_APPLE_READ, bytes([1])).payload)
    deadline = clock() + getattr(a, "wait", APPLE_POLL_S)
    while head["armed"] and head["why"] == "running" and clock() < deadline:
        sleep(0.2)
        head = decode_apple_read_head(link.call(CMD_APPLE_READ, bytes([0, 0])).payload)
    return _apple_read_all(link)


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
    header, page_size, maximum, reasons = (42, 1, TEST_MAX_STEPS, len(TEST_REASONS)) if modern else (15, 2, 95, 20)
    if modern and len(p) == 40 + 16 * n:
        header = 40   # firmware up to v1.0-400: no ps_rdy timing bytes (42 + 16n and 40 + 16n never collide for n <= 1)
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
        sweep = bool(draft_mask & TEST_SWEEP_BIT)   # draft bit 31: the PPS deep sweep is on (an Options flag, not a test)
        avs_sweep = bool(draft_mask & TEST_AVS_SWEEP_BIT)   # draft bit 30: the AVS deep sweep is on
        draft_mask &= ~(TEST_SWEEP_BIT | TEST_AVS_SWEEP_BIT)
        if (run_mask | draft_mask) & ~TEST_ALL or stage >= len(TEST_STAGES) or p[39] & 15 > 11 or (header == 42 and p[40] & ~7):
            raise FrameError("invalid charger-test selection or evidence")
        out["selection"] = {"run_mask": run_mask, "configured_mask": draft_mask, "pps_sweep": sweep,
                            "avs_sweep": avs_sweep,
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
            # PS_RDY timing quirks the engine tolerates and the verdicts ignore (TimingFlag bits; worst Accept -> PS_RDY in 20 ms units)
            "ps_rdy": {"out_of_band": bool(p[40] & 1), "inferred": bool(p[40] & 2), "late": bool(p[40] & 4),
                       "worst_ms": p[41] * 20} if header == 42 else None,   # None: this firmware does not report it
            "pass_scope": "negotiation and measured voltage only; not rated-current/power or revision conformance"}
    rows = []
    for i in range(n):
        code, pos, flags, mv, ma, measured, result, why, detail, pdo = struct.unpack_from("<BBBHHHBBBI", p, header + 16 * i)
        if (code >= (len(TEST_ALGORITHMS) if modern else 10) or pos > 11 or result >= len(TEST_RESULTS) or
                why >= reasons or flags & ~0x3F or (result == 4 and why == 0)):   # bits 0-4 + bit 5: a deep-sweep row
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
        if flags & 0x20 and group == 0:
            # a deep-sweep row: the detail byte is the recorded point count, not a code (remote_proto.hpp)
            detail_reason = f"deep sweep ({detail} points)"
        elif detail in EPR_DETAILS and group == 0:
            detail_reason = EPR_DETAILS[detail]
        elif detail == 161 and group == 0:   # older firmware: kDetailSweep marker, no flag bit, points not on the wire
            detail_reason = "deep sweep"
        elif detail and why == 46:   # StepResolution: the detail is the measured move in mV, not a code
            detail_reason = f"{detail} mV move"
        elif detail:
            if result == 0:
                # A pending step-walk row has not run: its detail byte is no sink ReqAnswer, print "not run".
                detail_reason = "not run"
            else:
                names, detail_code = (PD_ERRS, detail - 128) if group == 0 and detail >= 128 else (
                    SINK_RESULTS if group == 0 else FC_FAILS, detail)
                detail_reason = names[detail_code] if detail_code < len(names) else f"unknown ({detail})"
                if (flags >> 2) & 7 > 1:
                    detail_reason += f" ({(flags >> 2) & 7} tries)"
        row = {"index": start + i, "group": TEST_GROUPS[group], "protocol": protocol, "pos": pos,
               "target_mv": mv, "request_ma": ma, "measured_mv": measured,
               "result": TEST_RESULTS[result], "reason": TEST_REASONS[why],
               "detail": detail, "detail_reason": detail_reason, "raw_pdo": pdo,
               "continuous": bool(flags & 1), "avs": bool(flags & 2), "sweep": bool(flags & 0x20)}
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
                f"step resolution {detail}mV" if why == 46 and detail else "steps ignored" if why == 46 else
                "SCP 5A modes need an observed 5A cable" if why == 22 and code == 10 else TEST_REASONS[why])
        rows.append(row)
    out["steps"] = rows
    return out


CT_VIEW_FIXED, CT_VIEW_MESSAGE = 30, 27   # remote_proto.hpp kChargerTestViewFixed / kChargerTestViewMessage
CT_TOPOLOGIES = ["unknown", "none", "cable", "direct"]   # raguard::Topology, the firmware's name() words


def decode_charger_test_view(p):
    """0x0B req [3]: the charger-test screens as data (the wizard stage, the confirmation and wait clocks, the Review preview,
    the resume and guard state, the footer's words). Layout: remote_proto.cpp encode_charger_test_view."""
    if not CT_VIEW_FIXED <= len(p) <= CT_VIEW_FIXED + CT_VIEW_MESSAGE:
        raise FrameError("invalid charger-test view length")
    stage, cursor, rows, flags, confirm, w_el, w_tot, run_s, card, delta_n, delta_runs = struct.unpack_from("<BBBHHHHHHBH", p, 0)
    runnable, skipped, refusal, refusal_mv, pps, avs, topology, guard, rmdo = struct.unpack_from("<BBBHHHBBB", p, 18)
    if stage >= len(TEST_STAGES) or refusal >= len(TEST_REASONS) or topology >= len(CT_TOPOLOGIES) or flags & 0x8000:
        raise FrameError("invalid charger-test view")
    try:
        message = p[CT_VIEW_FIXED:].decode("ascii")
    except UnicodeDecodeError:
        raise FrameError("charger-test view message is not text") from None
    if not message.isprintable():
        raise FrameError("charger-test view message is not text")

    def f(bit):
        return bool(flags >> bit & 1)

    return {"stage": TEST_STAGES[stage], "cursor": cursor, "rows": rows, "on_screen": f(0),
            "confirming": f(1), "confirm_left_ms": confirm, "confirm_reattach": f(2),
            "wait": {"elapsed_ms": w_el, "total_ms": w_tot, "retry": f(11), "paused": f(10)},
            "run_elapsed_s": run_s, "card_no": card, "delta_n": delta_n, "delta_runs": delta_runs,
            "preview": {"runnable": runnable, "skipped": skipped, "refusal": TEST_REASONS[refusal], "refusal_mv": refusal_mv},
            "sweep_points": {"pps": pps, "avs": avs},
            "guard": {"topology": CT_TOPOLOGIES[topology], "run_guard": guard},
            "resume": {"pending": f(4), "auto": f(5), "gate_ready": f(6), "held": f(7), "behind_cable": f(8), "rd": f(9)},
            "self_supplied": f(3), "armed_is_pick": f(12), "cable_armed": f(14),
            "rmdo": f"{rmdo >> 4}.{rmdo & 15}" if f(13) else None,
            "message": message}


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


def charger_sweep(link, on):
    """Turn the PPS deep sweep of the next charger-test run on or off (opt-in; never in a preset)."""
    p = link.call(CMD_CHARGER_SWEEP, bytes([1 if on else 0])).payload
    _need(p, 1, "charger sweep")
    return bool(p[0])


def charger_avs_sweep(link, on):
    """Turn the AVS deep sweep of the next charger-test run on or off (opt-in; never in a preset)."""
    p = link.call(CMD_CHARGER_AVS_SWEEP, bytes([1 if on else 0])).payload
    _need(p, 1, "charger avs sweep")
    return bool(p[0])


def cmd_charger_sweep(link, a):
    return {"pps_sweep": charger_sweep(link, a.state == "on")}


def cmd_charger_avs_sweep(link, a):
    return {"avs_sweep": charger_avs_sweep(link, a.state == "on")}


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


class ResultsChanged(FrameError):
    """The firmware's RAM results moved under an export (a running suite rewrites rows and the revision)."""


RESULTS_ATTEMPTS = 4     # whole exports tried before the results are declared unsettled
RESULTS_RETRY_S = 0.3    # pause between exports, so a row boundary can pass


def cmd_charger_test(link, a, sleep=time.sleep):
    if a.test_cmd == "view":   # the screens as data; firmware without the form answers BadArg / BadLength
        try:
            return decode_charger_test_view(link.call(CMD_CHARGER_TEST, bytes([3])).payload)
        except RemoteError as e:
            if e.status not in (STATUS_BAD_LENGTH, STATUS_BAD_ARG):
                raise
            raise FrameError("this firmware has no charger-test view (older than the shell-parity build); "
                             "use charger-test status or a screenshot") from None
    if a.test_cmd == "status":
        return _charger_test_export(link, a)
    # results / summary read the whole table over many pages: a revision change under the read (Busy from the firmware, or a page
    # that no longer matches) is a moving table, not a failure. Read again, a bounded number of times, then say so.
    for attempt in range(1, RESULTS_ATTEMPTS + 1):
        try:
            return _charger_test_export(link, a)
        except (ResultsChanged, RemoteError) as e:
            if isinstance(e, RemoteError) and not (e.cmd == CMD_CHARGER_TEST and e.status == STATUS_BUSY):
                raise
            if attempt == RESULTS_ATTEMPTS:
                raise ResultsChanged(f"charger-test results kept changing in {RESULTS_ATTEMPTS} reads of {RESULTS_RETRY_S} s apart "
                                     f"(last: {e}); retry results when the run is between rows") from e
            sleep(RESULTS_RETRY_S)


def _charger_test_export(link, a):
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
            raise ResultsChanged("charger-test results changed or page did not advance; retry results")
        rows.extend(page["steps"])
    end = decode_charger_test(link.call(CMD_CHARGER_TEST, query).payload)
    if end != meta:
        raise ResultsChanged("charger-test results changed during export; retry results")
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
    out = decode_state(link.call(CMD_GET_STATE).payload)
    if getattr(a, "json", False):
        try:   # older firmware has no APPLE_READ: the fields stay absent
            out["source"] = {"apple": _apple_read_all(link)}
        except (RemoteError, FrameError):
            pass
        session = _state_part(link, 1, decode_session)
        if session is not None:
            out.update(session)
        diag = _state_part(link, 2, decode_diag)
        if diag is not None:
            out["diag"] = diag
        keys = _state_part(link, 3, decode_keys)
        if keys is not None:
            out["keys"] = keys
    return out


def _state_part(link, part, decode):
    """One GET_STATE part, decoded. Older firmware answers BadLength / BadArg to a part byte: None, the fields stay absent.
    Any other refusal and a malformed reply are real errors."""
    try:
        return decode(link.call(CMD_GET_STATE, bytes([part])).payload)
    except RemoteError as e:
        if e.status not in (STATUS_BAD_LENGTH, STATUS_BAD_ARG):
            raise
        return None


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
LEARN_OUTCOMES = ["none", "learned", "no-identity", "aborted", "acked-no-answer"]
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
    _need(p, 13, "emarker learn")   # 17 bytes with the answered tail, 19 with the read tail, 20 with the pull-down verdict
    return {**decode_answered(p[13:17]), **decode_read(p[17:19]), **decode_pull_down(p[19:20]), "outcome": LEARN_OUTCOMES[p[1]] if p[1] < len(LEARN_OUTCOMES) else str(p[1]), "svids": p[2], "probes": p[3],
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
    if answered_line(s):
        lines.append("  " + answered_line(s))
    if read_line(s):
        lines.append("  " + read_line(s))
    if pull_down_line(s):
        lines.append("  " + pull_down_line(s))
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
# The newest payload the card store sends is the v10 one, 250 bytes (report_card.py owns the layout:
# 214 in v9, 198 in v8, ...; offset 214 is the deep-sweep summary). Deriving the chunk count from it keeps
# this reader from ever lagging the format again; the short last chunk still ends the read, so an older
# firmware's fewer chunks work too.
CARD_CHUNKS = (report_card.PAYLOAD_V10 + CARD_CHUNK - 1) // CARD_CHUNK   # 6: v10 250 B (v9 214 B was 5, v8 198 B was 5)


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


TwinFailed = twin_proto.TwinError   # a twin command the K2 refused, or a record this tool cannot read


def _twin(link, payload):
    return link.call(CMD_TWIN, bytes(payload)).payload


def _twin_numbers(link):
    return twin_proto.decode_list(_twin(link, [TW_LIST]))


def _twin_record(link, seq):
    """The twin's record, read in parts (48 bytes each) and decoded."""
    raw, part = b"", 0
    while True:
        _, _, parts, data = twin_proto.chunk_info(_twin(link, [TW_GET, seq, part]))
        raw += data
        part += 1
        if part >= min(parts, 8):
            return twin_proto.decode_record(raw)


def _twin_usb(link, seq):
    """The twin's USB data lines: what it declared alone, what was sniffed inline ("" = nothing held)."""
    return {w: twin_proto.decode_usb(_twin(link, [TW_USB, seq, i]))["text"] for i, w in enumerate(twin_proto.USB_WHICH)}


def _twin_name(text):
    try:
        raw = text.encode("ascii")
    except UnicodeEncodeError:
        raw = b""
    if not 1 <= len(raw) <= twin_proto.NAME_MAX:
        raise FcRefused(f"name is 1..{twin_proto.NAME_MAX} characters of A-Z a-z 0-9 space - _ .")
    return raw


def _twin_row(rec, pred):
    return {"seq": rec["seq"], "name": rec["name"], "vid": rec["vid"], "pid": rec["pid"], "prediction": pred,
            "summary": pred["summary"] if pred else "no charger list", "note": pred["note"] if pred else ""}


def _twin_predict(link, rec):
    p = _twin(link, [TW_PREDICT, rec["seq"]])
    return None if p[:1] == bytes([TW_ERR_NO_CAPS]) else twin_proto.decode_predict(p)


def _twin_run(link, a):
    if not a.confirm:
        raise FcRefused("a live proof negotiates with the charger as each twin (the contract changes, the output moves to the twin's "
                        "voltage for about a second): add --confirm")
    seqs = a.ids or _twin_numbers(link)
    if not seqs:
        raise TwinFailed("twin run: no twins")
    r = twin_proto.decode_run(_twin(link, [TW_RUN, TW_RUN_CONFIRM, len(seqs)] + list(seqs)))
    if not r["started"]:
        hint = {"no charger": " (attach to a charger first: `pd attach`)", "link inline": " (a real sink is behind the K2)"}.get(r["refusal"], "")
        raise TwinFailed(f"twin run: {r['refusal']}{hint}")
    deadline = time.monotonic() + a.wait
    try:
        while True:
            st = twin_proto.decode_status(_twin(link, [TW_STATUS]))   # also keeps the dead-man lease fed
            if not st["running"]:
                break
            if time.monotonic() > deadline:
                raise TwinFailed(f"twin run: still running after {a.wait:g} s (it goes on; `twin status`, `twin cancel`)")
            time.sleep(0.5)
    except KeyboardInterrupt:
        _twin(link, [TW_CANCEL])
        raise
    rows = []
    for s in seqs:
        slot = twin_proto.decode_slot(_twin(link, [TW_RESULT, s]))
        rec = _twin_record(link, s)
        pred = _twin_predict(link, rec)
        row = _twin_row(rec, pred)
        row.update(verdict=slot["verdict"], skip=slot["skip"], measured_mv=slot["measured_mv"],
                   proof=twin_proto.proof(slot["verdict"], slot["measured_mv"], pred) if pred else slot["verdict"])
        rows.append(row)
    return {"twin": "run", "run": st, "rows": rows}


def cmd_twin(link, a):
    """Device twins: what a charger would give a learned device, and a live proof of it."""
    c = a.twin_cmd
    if c == "list":
        return {"twin": "list", "list": [_twin_record(link, s) for s in _twin_numbers(link)]}
    if c == "show":
        return {"twin": "show", "record": _twin_record(link, a.id), "usb": _twin_usb(link, a.id)}
    if c == "predict":
        rows = []
        for s in a.ids or _twin_numbers(link):
            rec = _twin_record(link, s)
            rows.append(_twin_row(rec, _twin_predict(link, rec)))
        return {"twin": "predict", "rows": rows}
    if c == "run":
        return _twin_run(link, a)
    if c == "status":
        return {"twin": "status", "status": twin_proto.decode_status(_twin(link, [TW_STATUS]))}
    if c == "result":
        return {"twin": "result", "result": twin_proto.decode_slot(_twin(link, [TW_RESULT, a.id]))}
    if c == "cancel":
        p = _twin(link, [TW_CANCEL])
        twin_proto.check(p, 2, "cancel")
        return {"twin": "cancel", "was_running": bool(p[1])}
    if c == "learn":
        name = _twin_name(a.name) if a.name else b""
        how = 0 if a.source == "identify" else 1
        r = twin_proto.decode_learn(_twin(link, [TW_LEARN, how, TW_LEARN_DRY if a.dry else 0] + list(name)))
        return {"twin": "learn", "source": a.source, "dry": a.dry, "learn": r}
    if c == "rename":
        twin_proto.check(_twin(link, [TW_RENAME, a.id] + list(_twin_name(a.name))), 2, "rename")
        return {"twin": "rename", "seq": a.id, "name": a.name}
    if c == "delete":
        if not a.confirm:
            raise FcRefused("delete removes a twin: add --confirm")
        twin_proto.check(_twin(link, [TW_DELETE, a.id, 1]), 2, "delete")
        return {"twin": "delete", "seq": a.id}
    if c == "usb":
        return {"twin": "usb", "seq": a.id, "usb": _twin_usb(link, a.id) if a.which is None else
                {a.which: twin_proto.decode_usb(_twin(link, [TW_USB, a.id, twin_proto.USB_WHICH.index(a.which)]))["text"]}}
    if c == "export":
        recs = []
        for s in _twin_numbers(link):
            rec = _twin_record(link, s)
            rec["usb"] = _twin_usb(link, s)
            recs.append(rec)
        with open(a.out, "w") as f:
            json.dump(recs, f, indent=1)
            f.write("\n")
        return {"twin": "export", "file": a.out, "count": len(recs)}
    raise FcRefused(f"unknown twin command {c}")


def _twin_flags(rec):
    return ", ".join(n for n, v in (("request", rec["request"]), ("declared", rec["declared"]), ("observed", rec["observed"])) if v) or "-"


def render_twin(r):
    k = r["twin"]
    if k == "list":
        if not r["list"]:
            return "no twins: learn one with `twin learn identify`"
        return "\n".join(f"  {t['seq']:3} {t['name'] or '(unnamed)':24} {t['vid']:04X}:{t['pid']:04X} {len(t['pdos'])} sink PDOs  "
                         f"{_twin_flags(t):28} {t['bytes']} B" for t in r["list"])
    if k == "show":
        t = r["record"]
        lines = [f"twin {t['seq']}: {t['name']}  {t['vid']:04X}:{t['pid']:04X}  id header 0x{t['id_hi']:04X}  PD rev {t['pd_rev']}  "
                 f"({t['bytes']} bytes, format {t['version']})"]
        lines += [f"  sink PDO {i + 1}: {twin_proto.pdo_text(p)}  raw=0x{p:08X}" for i, p in enumerate(t["pdos"])]
        if not t["pdos"]:
            lines.append("  no sink PDOs (made from a sniffed Request: the live proof speaks the K2's own capabilities)")
        if t["request"]:
            lines.append(f"  sniffed Request 0x{t['request']['rdo']:08X} against source PDO 0x{t['request']['pdo']:08X}")
        lines.append("  declared: " + (r["usb"]["declared"] or "no declared USB data (run Identify with the device alone)"))
        lines.append("  observed: " + (r["usb"]["observed"] or "no observed USB data (learn it from a sniff inline)"))
        lines.append(twin_proto.USB_HONESTY)
        return "\n".join(lines)
    if k in ("predict", "run"):
        if not r["rows"]:
            return "no twins: learn one with `twin learn identify`"
        lines = []
        for w in r["rows"]:
            tail = w["note"] + (" [mismatch]" if w["prediction"] and w["prediction"]["mismatch"] else "")
            if k == "run":
                tail = f"{w['proof']}" + (f" ({w['skip']})" if w["skip"] else "") + f"   predicted {w['summary']}, {w['note']}"
                lines.append(f"  {w['seq']:3} {w['name']:24} {tail}")
            else:
                lines.append(f"  {w['seq']:3} {w['name']:24} {w['summary']:14} {tail}")
        full = sum(1 for w in r["rows"] if w["prediction"] and w["note"] in ("full", "the device takes no more"))
        lines.append(f"{full} of {len(r['rows'])} full on this charger" if k == "predict" else f"run {r['run']['reason']}")
        return "\n".join(lines)
    if k == "status":
        s = r["status"]
        return (f"twin run: {'running' if s['running'] else 'idle'}, {s['done']} of {s['count']} done"
                + (f", now twin {s['current']}" if s["running"] else f", last {s['reason']}")
                + (", restoring" if s["restoring"] else "") + f"; verdicts held for {s['with_verdict'] or 'none'}")
    if k == "result":
        x = r["result"]
        return f"twin {x['seq']}: {x['verdict']}" + (f" ({x['skip']})" if x["skip"] else "") + (f" {twin_proto.volts(x['measured_mv'])}" if x["measured_mv"] else "")
    if k == "cancel":
        return "run cancelled" if r["was_running"] else "no run was going"
    if k == "learn":
        x = r["learn"]
        what = f"{x['vid']:04X}:{x['pid']:04X}, {x['num_pdos']} sink PDOs" + (f", {', '.join(x['has'])}" if x["has"] else "")
        if r["dry"]:
            return f"learned (dry run, nothing saved): {what}"
        return f"saved as twin {x['seq']}: {what}" + (" (already known" + (", unchanged)" if x["unchanged"] else ", merged)") if x["existed"] else "")
    if k == "rename":
        return f"twin {r['seq']} is now {r['name']}"
    if k == "delete":
        return f"deleted twin {r['seq']}"
    if k == "usb":
        lines = [f"  {w}: {t or 'nothing held'}" for w, t in r["usb"].items()]
        return "\n".join(lines + [twin_proto.USB_HONESTY])
    if k == "export":
        return f"wrote {r['count']} twins to {r['file']}"
    return "ok"


def cmd_emarker(link, a):
    sc = getattr(a, "em_cmd", None)
    if sc in ("learn", "learned"):
        return cmd_emlearn(link, a)
    if sc == "unmatched":
        return cmd_emunmatched(link, a)
    if sc == "probe":
        return cmd_emprobe(link, a)
    if sc:
        return cmd_emlib(link, a)
    return cmd_emarker_data(link, a)


def decode_ra_probe(p):
    """EmarkerProbe reply: refusal flags n sw0 sw1 pc vconn_pc (1 byte each) vbus_max_mv (u16), then n x [cc1_raw u16,
    cc2_raw u16]. Raw 0xFFFF = no reading; vbus_max 0xFFFF = no monitor reading. Samples run in phase order
    (idle x3, VCONN on CC1 x3, VCONN on CC2 x3) at RA_PROBE_SAMPLE_MS after each phase started."""
    _need(p, 9, "emarker probe")
    refusal, flags, n, sw0, sw1, pc, vconn_pc = p[:7]
    vbus_max = p[7] | p[8] << 8
    _need(p, 9 + 4 * n, "emarker probe samples")
    per = len(RA_PROBE_SAMPLE_MS)
    samples = []
    for k in range(n):
        cc1, cc2 = struct.unpack_from("<HH", p, 9 + 4 * k)
        samples.append({
            "phase": RA_PROBE_PHASES[k // per] if k // per < len(RA_PROBE_PHASES) else k // per,
            "t_ms": RA_PROBE_SAMPLE_MS[k % per],
            "cc1_raw": None if cc1 == 0xFFFF else cc1, "cc2_raw": None if cc2 == 0xFFFF else cc2,
            "cc1_mv": None if cc1 == 0xFFFF else cc1 * 3300 // 4096, "cc2_mv": None if cc2 == 0xFFFF else cc2 * 3300 // 4096,
        })
    return {"refusal": refusal, "aborted": bool(flags & 1), "restored": bool(flags & 2),
            "snapshot": {"sw0": f"0x{sw0:02X}", "sw1": f"0x{sw1:02X}", "pc": pc}, "vconn_pc": vconn_pc,
            "vbus_max_mv": None if vbus_max == 0xFFFF else vbus_max, "samples": samples}


def cmd_emprobe(link, a):
    """The silent-cable-chip probe: raw samples only, the K2 returns no verdict here. The K2 blocks ~330 ms."""
    payload = b"" if a.vconn_pc is None else bytes([a.vconn_pc])
    saved, link.timeout_s = link.timeout_s, max(link.timeout_s, RA_PROBE_TIMEOUT_S)
    try:
        p = link.call(CMD_EMARKER_PROBE, payload).payload
    finally:
        link.timeout_s = saved
    return {"emlib": "probe", "ra_probe": decode_ra_probe(p)}


def render_ra_probe(r):
    p = r["ra_probe"]
    head = ("emarker probe: " + ("ABORTED, VBUS rose (VCONN went off at once); " if p["aborted"] else "")
            + ("restored" if p["restored"] else "RESTORE NOT CONFIRMED")
            + f" (was sw0={p['snapshot']['sw0']} sw1={p['snapshot']['sw1']} pc={p['snapshot']['pc']}; "
            f"vconn levels pc={p['vconn_pc']}; vbus max {p['vbus_max_mv']} mV)")

    def mv(v):
        return "-" if v is None else f"{v}"
    rows = [f"  {s['phase']:9} t={s['t_ms']:3} ms  CC1 {mv(s['cc1_raw']):>4} ({mv(s['cc1_mv']):>4} mV)  "
            f"CC2 {mv(s['cc2_raw']):>4} ({mv(s['cc2_mv']):>4} mV)" for s in p["samples"]]
    return "\n".join([head] + rows)


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


# --- vooc-monitor (k2/vooc-detect) ---
# FC op 4: the inline VOOC monitor (passive, never drives D+/D-). Page 0 is the summary, pages 1 / 2 the command
# rows (8 each). Reply words are raw 10-bit values printed in hex; the command names in docs/FASTCHARGE_PROTOCOLS.md
# are guesses, and no reply word is turned into volts or amps here.
VOOC_SUMMARY_BYTES = 57
VOOC_ROWS_PER_PAGE = 8
VOOC_NO_REPLY = 0xFFFF


def _vooc_word(v, width=3):
    return None if v == VOOC_NO_REPLY else f"0x{v:0{width}X}"


def decode_vooc_summary(p):
    _need(p, VOOC_SUMMARY_BYTES, "vooc")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    u32 = lambda o: int.from_bytes(p[o:o + 4], "little")
    f = p[0]
    return {"enabled": bool(f & 1), "running": bool(f & 2), "session_active": bool(f & 4), "handshake": bool(f & 8),
            "sessions": u16(1), "wakes": u16(3), "partials": u16(5),
            "edges": u32(7), "frames": u32(11), "pairs": u32(15),
            "hs_pairs": p[19], "ident": f"0x{p[20]:02X}", "ident_reply": _vooc_word(u16(21)),
            "r82": {"last": _vooc_word(u16(23)), "min": _vooc_word(u16(25)), "max": _vooc_word(u16(27)), "n": u16(29)},
            "p82_us": {"last": u32(31), "mean": u32(35), "n": u16(39)},
            "ncmds": p[41], "other": u16(42),
            # the D+/D- edge-storm guard: paused = data traffic on the lines, the monitor waits retry_ms for a probe
            "guard": {"paused": bool(f & 16), "storms": u16(44), "last_edges": u16(46), "last_ms": u16(48),
                      "retry_ms": u32(50)},
            # the trailing run of one command the phone left unanswered; stalled = the firmware's threshold (flags bit5)
            "stalled": bool(f & 32),
            "stall": {"cmd": f"0x{p[54]:02X}" if u16(55) else None, "silent": u16(55)}}


def decode_vooc_rows(p):
    _need(p, 1, "vooc rows")
    n = p[0]
    _need(p, 1 + 5 * n, "vooc rows")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    return [{"cmd": f"0x{p[1 + 5 * i]:02X}", "count": u16(2 + 5 * i), "last": _vooc_word(u16(4 + 5 * i))}
            for i in range(n)]


def cmd_fc_vooc(link, a):
    head = bytes([FC_OP_VOOC, 0]) + (bytes([0]) if a.off else bytes([1]) if a.on else b"")
    s = decode_vooc_summary(link.call(CMD_FC, head).payload)
    cmds = []
    for page in range(1, 3):
        if len(cmds) >= s["ncmds"]:
            break
        cmds += decode_vooc_rows(link.call(CMD_FC, bytes([FC_OP_VOOC, page])).payload)
    s["cmds"] = cmds
    return {"vooc": s}


def render_vooc(v):
    state = ("on" if v["enabled"] else "off") + (", running" if v["running"] else ", idle")
    lines = [f"vooc monitor {state} (passive inline; raw words, names are guesses)",
             f"  sessions {v['sessions']}" + (" (one active)" if v["session_active"] else "")
             + f", wakes {v['wakes']}, partials {v['partials']}, edges {v['edges']}, frames {v['frames']}",
             f"  last session: handshake {'yes' if v['handshake'] else 'no'}"
             + (f" (ident {v['ident']} reply {v['ident_reply']}, {v['hs_pairs']} pairs)" if v["handshake"] else "")
             + f", pairs {v['pairs']}"]
    r, p = v["r82"], v["p82_us"]
    if r["n"]:
        lines.append(f"  0x82 replies n={r['n']} last {r['last']} min {r['min']} max {r['max']}")
    if p["n"]:
        lines.append(f"  0x82 period last {p['last'] / 1000:.1f} ms, mean {p['mean'] / 1000:.1f} ms (n={p['n']})")
    for c in v["cmds"]:
        lines.append(f"  {c['cmd']}  x{c['count']:<4} last {c['last'] or '-'}")
    if v.get("stalled"):
        st = v["stall"]
        lines.append(f"  stalled on {st['cmd']}, {st['silent']} silent replies")
    if v["other"]:
        lines.append(f"  other commands: {v['other']} pairs (table full)")
    g = v.get("guard", {})
    if g.get("paused"):
        lines.append(f"  paused: data traffic on D+/D-, retry in {g['retry_ms'] / 1000:.1f} s")
    if g.get("storms"):
        lines.append(f"  storms {g['storms']} (last {g['last_edges']} edges in {g['last_ms']} ms)")
    return "\n".join(lines)
# --- end vooc-monitor ---


# --- svooc-trigger ---
# FC op 5: the guarded NO-LOAD SuperVOOC trigger (k2/fastcharge/svooc_trig.hpp). The K2 stands in for a phone and
# answers the adapter's commands on D- with the words captured from a OnePlus 15; nothing draws current. The firmware
# refuses or aborts on its own guards (k2/fastcharge/svooc_run.hpp); this side only starts it, keeps the lease alive by
# polling, sends `stop` on Ctrl-C and prints the report. The name lists follow svooc_run.hpp / svooc_script.hpp in order.
FC_OP_SVOOC = 5
SVOOC_STATUS, SVOOC_START, SVOOC_STOP, SVOOC_LOG = 0, 1, 2, 3
SVOOC_USB_A = 4                 # start flags bit 2: waive the far-sink refusal (legacy A port, no PD anywhere)
SVOOC_SUMMARY_BYTES = 35        # the base layout; a 36th byte (inline cause) is read when the firmware sends it
SVOOC_SUMMARY_CAUSE = 35
SVOOC_SUMMARY_EXT = 36          # 36..44, read when the firmware sends them: i86_first i86_esc r82 vtarget(2) esc_at_s escape esc_t(2)
SVOOC_SUMMARY_EXT_BYTES = 45
SVOOC_SUMMARY_DIAG = 45         # 45..53, read when the firmware sends them: edges(2) last_edge_ms(2) poll_gap_max_ms(2) rec_edges(2) rec_end
SVOOC_SUMMARY_DIAG_BYTES = 54
SVOOC_REC_ENDS = ["recording", "full", "idle", "detach", "timeout", "cancelled", "storm"]   # edges::End
SVOOC_TIMING_BYTES = 15         # status page 1: stall_us(4) at_ms(2) stage(1) ticked_ms(2) rel_timer rel_loop rel_edge (2 each)
SVOOC_MASKED_MS = 5             # a stage this much longer than the ms clock moved ran with interrupts masked
STAGE_NAMES = ["none", "idle", "usb", "shell", "msc", "remote", "capture", "fctrig", "transcap", "service", "buttons",
               "fusb302", "power", "psource", "dpsniff", "pdtrace", "diaglive", "adc", "loopguard", "accel", "fastcharge",
               "boottrace", "session", "rdcomeback", "handback", "rescan", "survey", "bbpersist", "reportcard",
               "bootscreen", "ui", "stack", "heartbeat"]   # k2::stage::Id (include/k2/core/stage_meter.hpp)
SVOOC_START_EXT = 10            # the start request with the responder parameters (op 5 / 1 / flags / cap / 6 bytes)
SVOOC_I86_DEFAULT, SVOOC_I86_MIN, SVOOC_I86_MAX = 0x005, 1, 0x023   # the 0x86 answer, a count of about 100 mA
SVOOC_R82_DEFAULT, SVOOC_R82_MAX = 0x051, 0x07E                    # the steady 0x82 answer (0x07F is the escape word)
SVOOC_VTARGET_MAX_MV = 11000
SVOOC_ESCAPES = ["off", "armed", "sent", "got 86", "no 86"]
SVOOC_LOG_HEADER = 5            # kept(2) from(2) n(1), then n rows of cmd(1) word(2) t_ms(2) vbus_mv(2)
SVOOC_LOG_ROW = 7
SVOOC_SHOWN_PAIRS = 40          # svooc-trigger / svooc-status print the first pairs; `svooc-log` reads the whole list
SVOOC_CAP_MAX_S = 20
SVOOC_NO_WORD = 0xFFFF
SVOOC_STAGES = ["idle", "wake", "listen", "rest", "answer", "exit", "done"]
SVOOC_REFUSALS = ["none", "not confirmed", "D+/D- busy", "no host link", "no power monitor", "sink on the bottom port",
                  "inline mode", "PD contract not own 5 V", "current over 50 mA", "VBUS not 5 V", "no RAM: USB drive enabled",
                  "USB-A waiver but PD is active"]
SVOOC_ABORTS = ["none", "VBUS over 12 V", "current over 50 mA", "charger off-script", "host gone", "power monitor lost",
                "sink appeared", "PD contract appeared", "D+/D- edge storm", "no adapter answer", "time cap", "stopped", "PD contract lost", "PD contract changed"]
SVOOC_CONTRACTS = ["none", "own 5 V", "other"]
SVOOC_INLINE_REFUSAL = SVOOC_REFUSALS.index("inline mode")
SVOOC_INLINE_ABORT = SVOOC_ABORTS.index("sink appeared")
SVOOC_PHASES = ["ident", "handshake", "ramp"]


def _svooc_cap(text):
    try:
        n = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"not a number of seconds: {text!r}") from None
    if not 0 <= n <= SVOOC_CAP_MAX_S:
        raise argparse.ArgumentTypeError(f"{n} is outside 0..{SVOOC_CAP_MAX_S} (0 = the full {SVOOC_CAP_MAX_S} s)")
    return n


def _svooc_ranged(lo, hi, what):
    """argparse type: an integer (decimal or 0x hex) in lo..hi."""
    def parse(text):
        try:
            n = int(text, 0)
        except ValueError:
            raise argparse.ArgumentTypeError(f"not a number: {text!r}") from None
        if not lo <= n <= hi:
            raise argparse.ArgumentTypeError(f"{what} {n} is outside {lo}..{hi}")
        return n
    return parse


def svooc_start_payload(a):
    """The start request: the 4-byte original when every responder parameter is at its default (an older firmware keeps
    working), else the 10-byte extended one."""
    flags = FC_CONFIRM | (SVOOC_USB_A if a.usb_a else 0)
    first = getattr(a, "i86_first", None)
    esc = getattr(a, "i86", None)
    r82, vtarget, at = getattr(a, "r82", None), getattr(a, "vtarget", None), getattr(a, "i86_at", None)
    if all(v is None for v in (first, esc, r82, vtarget, at)):
        return bytes([FC_OP_SVOOC, SVOOC_START, flags, a.cap])
    first = SVOOC_I86_DEFAULT if first is None else first
    esc = SVOOC_I86_DEFAULT if esc is None else esc
    r82 = SVOOC_R82_DEFAULT if r82 is None else r82
    vtarget, at = vtarget or 0, at or 0
    return bytes([FC_OP_SVOOC, SVOOC_START, flags, a.cap, first, esc, r82, vtarget & 255, vtarget >> 8, at])


def decode_svooc_summary(p):
    _need(p, SVOOC_SUMMARY_BYTES, "svooc")
    u16 = lambda o: int.from_bytes(p[o:o + 2], "little")
    name = lambda names, i: names[i] if i < len(names) else str(i)
    cause = name(INLINE_CAUSES, p[SVOOC_SUMMARY_CAUSE]) if len(p) > SVOOC_SUMMARY_CAUSE else "none"
    refusal, abort = name(SVOOC_REFUSALS, p[1]), name(SVOOC_ABORTS, p[2])
    if cause != "none" and p[1] == SVOOC_INLINE_REFUSAL:   # what latched the inline state, e.g. "inline mode (cause: far rd)"
        refusal += f" (cause: {cause})"
    if cause != "none" and p[2] == SVOOC_INLINE_ABORT:
        abort += f" (cause: {cause})"
    ext = {}
    if len(p) >= SVOOC_SUMMARY_EXT_BYTES:   # a firmware that reports its responder parameters and the escape outcome
        o = SVOOC_SUMMARY_EXT
        ext = {"params": {"i86_first": f"0x{p[o]:03X}", "i86_esc": f"0x{p[o + 1]:03X}", "r82": f"0x{p[o + 2]:03X}",
                          "vtarget_mv": u16(o + 3), "esc_at_s": p[o + 5]},
               "escape": name(SVOOC_ESCAPES, p[o + 6]), "escape_t_ms": u16(o + 7)}
    if len(p) >= SVOOC_SUMMARY_DIAG_BYTES:   # a firmware that reports the wire's edges, the main-loop gap and the recording
        o = SVOOC_SUMMARY_DIAG
        ext["diag"] = {"edges": u16(o), "last_edge_ms": u16(o + 2), "poll_gap_max_ms": u16(o + 4), "rec_edges": u16(o + 6),
                       "rec_end": name(SVOOC_REC_ENDS, p[o + 8])}
    return {**ext, "stage": name(SVOOC_STAGES, p[0]), "refusal": refusal, "abort": abort, "inline_cause": cause,
            "wakes": p[3], "accepted": bool(p[4] & 1), "exit_checked": bool(p[4] & 2), "exit_ok": bool(p[4] & 4),
            "driving": bool(p[4] & 8), "usb_a_waiver": bool(p[4] & 16), "phase": name(SVOOC_PHASES, p[5]), "off_run": p[6],
            "last_cmd": f"0x{p[7]:02X}",
            "commands": u16(8), "replies": u16(10), "missed": u16(12), "cut": u16(14), "partials": u16(16),
            "pairs_total": u16(18), "pairs_kept": p[20], "cap_ms": u16(21), "elapsed_ms": u16(23),
            "vbus_start_mv": u16(25), "vbus_max_mv": u16(27), "vbus_exit_mv": u16(29), "ibus_max_ma": u16(31),
            "contract_start": name(SVOOC_CONTRACTS, p[33]), "contract_end": name(SVOOC_CONTRACTS, p[34])}


def decode_svooc_log(p):
    """One log page -> (kept, from, rows); each row is the pair's index, wake-relative time, VBUS and the two words."""
    _need(p, SVOOC_LOG_HEADER, "svooc log")
    kept, first, n = int.from_bytes(p[0:2], "little"), int.from_bytes(p[2:4], "little"), p[4]
    _need(p, SVOOC_LOG_HEADER + SVOOC_LOG_ROW * n, "svooc log")
    rows = []
    for i in range(n):
        o = SVOOC_LOG_HEADER + SVOOC_LOG_ROW * i
        word = int.from_bytes(p[o + 1:o + 3], "little")
        rows.append({"i": first + i, "t_ms": int.from_bytes(p[o + 3:o + 5], "little"),
                     "vbus_mv": int.from_bytes(p[o + 5:o + 7], "little"), "cmd": f"0x{p[o]:02X}",
                     "word": None if word == SVOOC_NO_WORD else f"0x{word:03X}"})
    return kept, first, rows


def svooc_log(link, start=0, count=None):
    """The run's command>reply pairs from index `start` (at most `count`), paged 7 rows per request."""
    kept, rows, i = 0, [], start
    while True:
        kept, first, page = decode_svooc_log(link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_LOG, i & 255, i >> 8])).payload)
        rows += page
        i = first + len(page)
        if not page or i >= kept or (count is not None and len(rows) >= count):
            break
    return kept, (rows if count is None else rows[:count])


def decode_svooc_timing(p):
    """Status page 1 (src/core/remote_proto.cpp encode_svooc_timing)."""
    u16 = lambda o: p[o] | p[o + 1] << 8
    stage = STAGE_NAMES[p[6]] if p[6] < len(STAGE_NAMES) else str(p[6])
    return {"stall_us": int.from_bytes(p[0:4], "little"), "stall_at_ms": u16(4), "stall_stage": stage,
            "stall_ticked_ms": u16(7), "rel_timer": u16(9), "rel_loop": u16(11), "rel_edge": u16(13)}


def _svooc_read(link):
    s = decode_svooc_summary(link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STATUS, 0])).payload)
    try:   # page 1, the timing diagnostics (a firmware without it answers BadArg)
        p = link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STATUS, 1])).payload
        if len(p) >= SVOOC_TIMING_BYTES:
            s["timing"] = decode_svooc_timing(p)
    except RemoteError as e:
        if e.status != STATUS_BAD_ARG:
            raise
    kept, s["pairs"] = svooc_log(link, 0, SVOOC_SHOWN_PAIRS) if s["pairs_kept"] else (0, [])
    s["pairs_kept"] = kept   # the summary's count saturates at 255
    return s


def cmd_fc_svooc(link, a, sleep=time.sleep, poll_s=0.25):
    if a.fc_cmd == "svooc-status":
        return {"svooc": _svooc_read(link)}
    if a.fc_cmd == "svooc-log":
        kept, pairs = svooc_log(link, a.start, a.count)
        head = decode_svooc_summary(link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STATUS, 0])).payload)   # for the header
        out = {"kept": kept, "start": a.start, "pairs": pairs}
        for k in ("params", "escape", "escape_t_ms"):
            if k in head:
                out[k] = head[k]
        return {"svooc_log": out}
    if a.fc_cmd == "svooc-stop":
        link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STOP]))
        return {"svooc": _svooc_read(link)}
    if not a.confirm:
        raise FcRefused("the SuperVOOC trigger answers the adapter on D- and the adapter may raise VBUS (up to the 12 V "
                        "guard): add --confirm (SUPERVOOC adapter alone, NO load, nothing else on the K2)")
    if getattr(a, "i86", None) is not None and not getattr(a, "i86_at", None):
        raise FcRefused("--i86 answers the 0x86 that follows the escape: add --i86-at S (--i86-first sets the post-handshake answer)")
    if getattr(a, "vtarget", None) and getattr(a, "r82", None) is not None:
        raise FcRefused("--vtarget (closed loop) replaces --r82 (constant): give one of them")
    try:
        link.call(CMD_FC, svooc_start_payload(a))
    except RemoteError as e:
        if e.status == STATUS_BUSY and len(e.payload) >= SVOOC_SUMMARY_BYTES:
            raise FcRefused(f"svooc trigger refused: {decode_svooc_summary(e.payload)['refusal']}") from e
        raise
    # Every status read is a request, so it holds the lease; the firmware ends the run itself (guards, cap, exit watch).
    stopped = False
    while True:
        try:
            sleep(poll_s)
            s = decode_svooc_summary(link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STATUS, 0])).payload)
            if s["stage"] in ("done", "idle"):
                break
        except KeyboardInterrupt:
            if stopped:
                raise
            link.call(CMD_FC, bytes([FC_OP_SVOOC, SVOOC_STOP]))
            stopped = True
    return {"svooc": _svooc_read(link)}


def _svooc_params_lines(s):
    """The responder's operator parameters and the escape outcome, when the firmware reported them."""
    pm = s.get("params")
    if not pm:
        return []
    r82 = (f"closed loop to {pm['vtarget_mv']} mV (0x82 word from the measured VBUS)" if pm["vtarget_mv"]
           else f"constant {pm['r82']}")
    lines = [f"  responder: 0x86 answer {pm['i86_first']} after the handshake, 0x82 answer {r82}"]
    if pm["esc_at_s"]:
        when = f" at {s['escape_t_ms']} ms" if s["escape_t_ms"] else ""
        lines.append(f"  escape at {pm['esc_at_s']} s (0x07F{when}), then 0x86 answered {pm['i86_esc']}: {s['escape']}")
    return lines


def render_svooc(s):
    lines = [f"svooc trigger {s['stage']}: " + (f"ended by {s['abort']}" if s["abort"] != "none" else "running")
             + f" after {s['elapsed_ms'] / 1000:.1f} s (cap {s['cap_ms'] / 1000:.0f} s), wakes {s['wakes']}"]
    lines.append(f"  handshake accepted: {'yes' if s['accepted'] else 'no'} (phase {s['phase']}); commands {s['commands']}, "
                 f"replies {s['replies']}, missed {s['missed']}, cut {s['cut']}, partials {s['partials']}")
    exit_txt = ("not checked yet" if not s["exit_checked"] else
                f"{s['vbus_exit_mv']} mV" + ("" if s["exit_ok"] else " - STILL ABOVE 5.5 V after the exit watch"))
    lines.append(f"  VBUS start {s['vbus_start_mv']} mV, max {s['vbus_max_mv']} mV, exit {exit_txt}; "
                 f"max |I| {s['ibus_max_ma']} mA")
    lines.append(f"  PD contract: start {s['contract_start']}, end {s['contract_end']}")
    lines += _svooc_params_lines(s)
    d = s.get("diag")
    if d:
        lines.append(f"  diag: {d['edges']} edges to the wire, the last at {d['last_edge_ms']} ms after the wake; "
                     f"longest main-loop gap {d['poll_gap_max_ms']} ms; Black Box recording {d['rec_edges']} edges, "
                     f"ended {d['rec_end']}")
    t = s.get("timing")
    if t:
        masked = t["stall_us"] // 1000 - t["stall_ticked_ms"]
        clock = f"ms clock +{t['stall_ticked_ms']}" + (f": interrupts masked ~{masked} ms" if masked > SVOOC_MASKED_MS else "")
        lines.append(f"  timing: longest main-loop stage {t['stall_stage']} {t['stall_us'] / 1000:.1f} ms at "
                     f"{t['stall_at_ms']} ms after the wake ({clock}); D- let go by the timer {t['rel_timer']}, "
                     f"the main loop {t['rel_loop']}, the next frame {t['rel_edge']}")
    if s["usb_a_waiver"]:
        lines.append("  far-sink / inline check waived (USB-A): the operator's waiver, no PD anywhere at the start")
    if s["inline_cause"] != "none":
        lines.append(f"  inline latch cause: {s['inline_cause']}")
    if s["pairs"]:
        words = [f"{p['cmd'][2:]}>{p['word'][2:] if p['word'] else '---'}" for p in s["pairs"]]
        more = "; the whole list: `fc svooc-log`" if s["pairs_kept"] > len(words) else ""
        lines.append(f"  pairs ({len(words)} of {s['pairs_total']}, cmd>reply, --- = not answered{more}):")
        lines += ["    " + " ".join(words[i:i + 8]) for i in range(0, len(words), 8)]
    return "\n".join(lines)


def render_svooc_log(r):
    lines = [f"svooc pairs: {len(r['pairs'])} shown of {r['kept']} kept (from {r['start']}); t = ms after the wake, "
             "VBUS when the pair closed, cmd>reply, --- = not answered"]
    lines += _svooc_params_lines(r)
    for p in r["pairs"]:
        vbus = f"{p['vbus_mv'] / 1000:5.2f} V" if p["vbus_mv"] else "   -   "
        lines.append(f"  {p['i']:4d} {p['t_ms']:6d} ms {vbus}  {p['cmd'][2:]}>{p['word'][2:] if p['word'] else '---'}")
    return "\n".join(lines)
# --- end svooc-trigger ---


def cmd_fc(link, a):
    if a.fc_cmd == "vooc":   # vooc-monitor
        return cmd_fc_vooc(link, a)
    if a.fc_cmd.startswith("svooc-"):   # svooc-trigger
        return cmd_fc_svooc(link, a)
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
        if a.allow_inline and not a.confirm:
            raise FcRefused("--allow-inline drives D+/D- with a sink possibly attached, at any voltage: "
                            "it needs --confirm too (both flags)")
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


# ---- PD Stress (docs/PD_STRESS.md): CMD_PD_STRESS 0x55 ----
def decode_pdstress_status(p):
    """19-byte status block: state stop start seq_idx seq_total seq_cur reboots (u8), transitions u16, elapsed_ms u32,
    score_cp u32 (hundredths of a point per 100 transitions), blob_len u16."""
    _need(p, 19, "pd-stress status")
    u16 = lambda o: p[o] | p[o + 1] << 8
    u32 = lambda o: u16(o) | u16(o + 2) << 16
    return {"state": _name_of(PDS_STATES, p[0]), "stop": _name_of(PDS_STOPS, p[1]), "start": _name_of(PDS_STARTS, p[2]),
            "seq_idx": p[3], "seq_total": p[4], "seq_cur": p[5], "reboots": p[6], "transitions": u16(7),
            "elapsed_ms": u32(9), "score_cp": u32(13), "score": u32(13) / 100.0, "blob_len": u16(17)}


def pdstress_status(link):
    return decode_pdstress_status(link.call(CMD_PD_STRESS, bytes([PDS_STATUS])).payload)


def pdstress_start(link, mask=0, cycles_pct=0, pace=0, max_s=0, max_trans=0, confirm=False):
    """Start a run. 0 in a numeric field means the K2's default. PD Stress sends many renegotiations, so it needs `confirm`."""
    if not confirm:
        raise SystemExit("pd-stress start renegotiates the charger hard (inside the PD rules): add --confirm")
    flags = PDS_FLAG_CONFIRM
    args = (int(mask).to_bytes(2, "little") + int(cycles_pct).to_bytes(2, "little") + bytes([int(pace)]) +
            int(max_s).to_bytes(2, "little") + int(max_trans).to_bytes(2, "little") + bytes([flags]))
    return decode_pdstress_status(link.call(CMD_PD_STRESS, bytes([PDS_START]) + args).payload)


def pdstress_stop(link):
    return decode_pdstress_status(link.call(CMD_PD_STRESS, bytes([PDS_STOP])).payload)


def pdstress_read_blob(link):
    """The result blob, read in PDS_CHUNK pieces (layout: include/k2/pdstress/result.hpp)."""
    out = bytearray()
    total = None
    while total is None or len(out) < total:
        p = link.call(CMD_PD_STRESS, bytes([PDS_READ]) + len(out).to_bytes(2, "little")).payload
        _need(p, 5, "pd-stress read")
        total = p[2] | p[3] << 8
        n = p[4]
        _need(p, 5 + n, "pd-stress read")
        if n == 0:
            break
        out += p[5:5 + n]
    return bytes(out)


def render_pdstress(r):
    if "pdstress" in r:
        s = r["pdstress"]
        return (f"pd-stress {s['state']}" + (f", stop: {s['stop']}" if s["state"] == "finished" else "") +
                f", start {s['start']}, sequence {s['seq_idx']}/{s['seq_total']} (id {s['seq_cur']}), reboots {s['reboots']}, "
                f"{s['transitions']} transitions, {s['elapsed_ms'] / 1000:.1f} s, score {s['score']:.2f} (0 = ideal, per 100 transitions)")
    import pdstress_blob
    return pdstress_blob.render_text(r["pdstress_result"])


def cmd_pd_stress(link, a):
    c = a.pdstress_cmd
    if c == "status":
        return {"pdstress": pdstress_status(link)}
    if c == "start":
        return {"pdstress": pdstress_start(link, a.mask, a.cycles, a.pace, a.max_s, a.max_trans, a.confirm)}
    if c == "stop":
        return {"pdstress": pdstress_stop(link)}
    if c == "result":
        import pdstress_blob
        return {"pdstress_result": pdstress_blob.decode_blob(pdstress_read_blob(link))}
    raise SystemExit("unknown pd-stress command")
# ---- end PD Stress ----


# ---- Loop probe (docs/LOOP_PROBE.md): CMD_LOOP_PROBE 0x57 ----
def loop_probe_read_file(link):
    """The stored result file /lp/last.rec, read in LOOP_CHUNK pieces; b"" when the K2 holds no result."""
    out = bytearray()
    total = None
    while total is None or len(out) < total:
        p = link.call(CMD_LOOP_PROBE, len(out).to_bytes(2, "little")).payload
        _need(p, 5, "loop-probe")
        total = p[2] | p[3] << 8
        n = p[4]
        _need(p, 5 + n, "loop-probe")
        if n == 0:
            break
        out += p[5:5 + n]
    return bytes(out)


# ---- Battery efficiency recorder (docs/BATTERY_RECORDER.md): CMD_BATTERY 0x58 ----
BATTERY_OPS = {"status": 0, "start": 1, "end": 2, "next": 3, "finish": 4, "config": 5, "list": 6, "read": 7}


def battery_status(link, op="status", args=b""):
    import battery_rec
    p = link.call(CMD_BATTERY, bytes([BATTERY_OPS[op]]) + args).payload
    _need(p, battery_rec.STATUS_BYTES, "battery " + op)
    return battery_rec.decode_status(p)


def battery_read_record(link, rid):
    """One run record (id 0 = the run in RAM), read in BATTERY_CHUNK pieces; b"" when the K2 has no such run."""
    out = bytearray()
    total = None
    while total is None or len(out) < total:
        p = link.call(CMD_BATTERY, bytes([BATTERY_OPS["read"]]) + rid.to_bytes(2, "little") + len(out).to_bytes(2, "little")).payload
        _need(p, 5, "battery read")
        total = p[2] | p[3] << 8
        n = p[4]
        _need(p, 5 + n, "battery read")
        if n == 0:
            break
        out += p[5:5 + n]
    return bytes(out)


def battery_list(link):
    p = link.call(CMD_BATTERY, bytes([BATTERY_OPS["list"]])).payload
    _need(p, 1, "battery list")
    _need(p, 1 + 2 * p[0], "battery list")
    return [p[1 + 2 * i] | p[2 + 2 * i] << 8 for i in range(p[0])]


def cmd_battery(link, a):
    import battery_rec
    op = a.battery_cmd
    if op == "status":
        return {"battery": battery_status(link)}
    if op == "list":
        return {"battery_runs": battery_list(link)}
    if op == "read":
        ids = battery_list(link) if a.all else [a.id]
        recs = []
        for rid in ids:
            raw = battery_read_record(link, rid)
            if not raw:
                if not a.all:
                    raise SystemExit(f"battery read: no run {rid}" if rid else "battery read: no run since boot")
                continue
            try:
                recs.append(battery_rec.decode_record(raw))
            except battery_rec.BadRecord as e:
                raise SystemExit(f"battery read {rid}: {e}")
        return {"battery_records": recs} if a.all else {"battery_record": recs[0]}
    if op == "start":
        wiring = a.wiring if a.wiring is not None else battery_status(link)["next_wiring"]
        args = bytes([battery_rec.MODES.index(a.mode), battery_rec.WIRINGS.index(wiring)])
        s = battery_status(link, "start", args)
    elif op == "config":
        cur = battery_status(link)
        end_ma = a.end_ma if a.end_ma is not None else cur["end_ma"]
        end_s = a.end_s if a.end_s is not None else cur["end_s"]
        wiring = a.wiring if a.wiring is not None else cur["next_wiring"]
        s = battery_status(link, "config", struct.pack("<HHB", end_ma, end_s, battery_rec.WIRINGS.index(wiring)))
    else:
        s = battery_status(link, op)
    if s["result"] != "ok":
        raise SystemExit(f"battery {op}: refused ({s['result']})")
    return {"battery": s}


def render_battery(r):
    import battery_rec
    if "battery_runs" in r:
        return "stored runs (newest first): " + (", ".join(str(i) for i in r["battery_runs"]) or "none")
    if "battery_record" in r:
        return battery_rec.render_record(r["battery_record"])
    if "battery_records" in r:
        return "\n".join(battery_rec.render_record(x) for x in r["battery_records"]) or "no runs stored"
    return battery_rec.render_status(r["battery"])
# ---- end Battery ----


def cmd_loop_probe(link, a):
    import loop_probe_rec
    f = loop_probe_read_file(link)
    if not f:
        return {"loop_probe": None}
    try:
        return {"loop_probe": loop_probe_rec.decode_file(f)}
    except loop_probe_rec.BadRecord as e:
        raise SystemExit(f"loop-probe: {e}")


def render_loop_probe(r):
    lp = r["loop_probe"]
    if lp is None:
        return "no loop probe result stored (run it on the K2: Tools > Loop probe)"
    lines = [f"loop probe v{lp['version']}, fw {lp['fw']}, started at {lp['start_ms']} ms uptime"
             + (", in progress (the K2 rebooted or is still running it)" if lp["in_progress"] else "")]
    for name, s in lp["steps"].items():
        lines.append(f"  {name:<9}{s['status']}" + (f" ({s['why']})" if s["why"] else "")
                     + (f", t {s['t_ms']} ms, {s['dur_ms']} ms" if s["status"] != "not_run" else ""))
    lines.append("raw values: --json, or the LOOP QR via tools/k2_qr.py decode")
    return "\n".join(lines)
# ---- end Loop probe ----


LAB_REASONS = ["none", "timeout", "vbus_low", "vbus_high", "mcu_hot", "external", "done"]


def decode_lab(p):
    _need(p, 17, "lab")
    sg = lambda o: int.from_bytes(p[o:o + 2], "little", signed=True)
    u = lambda o: int.from_bytes(p[o:o + 2], "little")
    return {"reason": LAB_REASONS[p[0]] if p[0] < len(LAB_REASONS) else p[0], "duty": p[1], "ms": u(2),
            "idle_ma": sg(4), "peak_ma": sg(6), "min_vbus_mv": u(8), "vbus_before_mv": u(10),
            "temp_before_c": sg(12) / 10, "temp_after_c": sg(14) / 10, "pin_high_pct": p[16]}


SNIFF_END = {0: "none", 1: "full", 2: "idle", 3: "detach", 4: "timeout", 5: "cancel",
             6: "storm"}   # vooc-monitor: the edge-storm guard ended it (data traffic on D+/D-)


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


DG_STATES = ["none", "open", "last"]
DG_SEV = ["fail", "warn", "info", "ok"]
DG_LINK = ["healthy", "degraded", "unstable"]
DG_KIND = ["verdict", "title", "note", "healthy", "lanes"]
DG_POWER = ["", "power short", "power OK", "need not known", "contract not observed", "no contract made",
            "K2 is the sink", "no PD", "draw over contract"]
DG_GROUP = ["power", "contract", "quality", "link", "roles"]


def diagnose_read(link):
    """The Black Box diagnosis the K2 shows: status, every line (read in chunks through `off`), every finding."""
    p = link.call(CMD_DIAGNOSE, bytes([DG_STATUS])).payload
    _need(p, 16, "diagnose status")
    sessions, start_us, dur_ms = struct.unpack_from("<HII", p, 6)
    out = {"state": _tc_name(DG_STATES, p[0]), "power": _tc_name(DG_POWER, p[1]), "link": _tc_name(DG_LINK, p[2]),
           "worst": _tc_name(DG_SEV, p[3]), "sessions": sessions, "start_us": start_us, "dur_ms": dur_ms,
           "lines": [], "findings": []}
    for i in range(p[5]):
        text, kind, sev, num, total = b"", 0, 0, 0, None
        while total is None or len(text) < total:
            q = link.call(CMD_DIAGNOSE, bytes([DG_LINE, i, len(text)])).payload
            _need(q, 5, "diagnose line")
            kind, sev, num, total = q[1], q[2], q[3], q[4]
            if len(q) == 5:
                break
            text += q[5:]
        out["lines"].append({"kind": _tc_name(DG_KIND, kind), "sev": _tc_name(DG_SEV, sev), "num": num,
                             "text": text.decode("utf-8", "replace")})
    for i in range(p[4]):
        q = link.call(CMD_DIAGNOSE, bytes([DG_FINDING, i])).payload
        _need(q, 24, "diagnose finding")
        t_ms, va, vb, vc, vd = struct.unpack_from("<Iiiii", q, 4)
        out["findings"].append({"sev": _tc_name(DG_SEV, q[1]), "group": _tc_name(DG_GROUP, q[2]),
                                "code": q[3], "name": q[24:].decode("ascii", "replace"), "t_ms": t_ms,
                                "a": va, "b": vb, "c": vc, "d": vd})
    return out


def cmd_diagnose(link, a):
    return diagnose_read(link)


def diagnose_text(r):
    """The diagnosis as the K2 lists it: the verdict, then each finding's title with its notes indented."""
    if r["state"] == "none":
        return "no PD session yet"
    rows = [f"session: {r['state']}, {r['dur_ms'] / 1000:.1f} s, {r['sessions']} seen"]
    for ln in r["lines"]:
        tag = {"fail": "FAIL ", "warn": "WARN ", "info": "     ", "ok": "     "}[ln["sev"]] if ln["kind"] == "title" else ""
        indent = "      " if ln["kind"] == "note" else "  "
        rows.append(f"{indent}{tag}{ln['text']}")
    return "\n".join(rows)


def cmd_action(link, a):
    saved, link.timeout_s = link.timeout_s, max(link.timeout_s, ACTION_TIMEOUT_S)
    try:
        link.call(CMD_ACTION, bytes([ACTIONS[a.name], 1 if getattr(a, "confirm", False) else 0]))
    finally:
        link.timeout_s = saved
    return {"action": a.name}


def cmd_pd(link, a):
    """Active PD. `pd request` above 5 V needs --confirm; inline the K2 refuses all of them
    (--over-load lifts the refusal only while the far sink is a passive load, lab only;
    the other exception is the lab-only `pd reset --inline --confirm`)."""
    sub = a.pd_cmd
    if sub == "request":
        mv, ma = getattr(a, "mv", 0) or 0, getattr(a, "ma", 0) or 0
        flags = (1 if getattr(a, "confirm", False) else 0) | (2 if getattr(a, "over_load", False) else 0)
        link.call(CMD_PD_REQUEST, bytes([a.pos, flags]) + mv.to_bytes(2, "little") + ma.to_bytes(2, "little"))
    elif sub == "reset":
        # flags byte: bit0 confirm, bit1 inline (lab-only Hard Reset while a live sink is attached; the
        # firmware refuses it unless both are set, and while a charger test runs). No flags -> the old empty
        # payload, so an older firmware still accepts the plain reset.
        flags = (1 if getattr(a, "confirm", False) else 0) | (2 if getattr(a, "inline", False) else 0)
        link.call(CMD_PD_RESET, bytes([flags]) if flags else b"")
    elif sub == "handback":
        # Phase-1 lab op (ROADMAP "An inline latch leaves the far sink on Type-C current"): the whole Fix
        # sequence - wait for /rd.rec to be gone, drop the Rd, ONE Hard Reset - runs on the K2 only when the
        # decision allows it (contract owned + inline, no far Request this attach, first hand-back this attach);
        # otherwise the reply's verdict byte says why. Refused during a charger test; needs --confirm.
        # `pd reset --inline --confirm` stays the raw single reset.
        flags = 1 if getattr(a, "confirm", False) else 0
        rsp = link.call(CMD_PD_HANDBACK, bytes([flags]) if flags else b"")
        return {"pd_cmd": sub, "verdict": HANDBACK_VERDICTS[rsp.payload[0]] if rsp.payload else "?"}
    elif sub == "release":
        link.call(CMD_PD_RELEASE)
    elif sub == "attach":
        # Default 5 V. Above 5 V the firmware needs the confirm flag (bit0 of the optional third field, the
        # PD_REQUEST bit), and it refuses outright while a sink is attached inline or a run owns the ports.
        mv = getattr(a, "mv", 5000)
        ma = getattr(a, "ma", 0)
        confirm = getattr(a, "confirm", False)
        # Clamp to 36000 mV for safety
        if mv > 36000:
            mv = 36000
        payload = mv.to_bytes(2, "little")
        if ma > 0 or confirm:
            payload += ma.to_bytes(2, "little")
        if confirm:
            payload += bytes([1])
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
    if t.startswith("connection") and len(t) > len("connection"):
        t = t[len("connection"):].strip()   # the long form "connection sink" = the Connection tab "sink"
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
    if a.debug_cmd == "loop":
        payload = bytes([1, a.page, 1 if a.reset else 0])
        return {"loop": decode_loop(link.call(CMD_DEBUG_LCD, payload).payload)}
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
    """DebugLcd reply: [flags (bit0 DMA built in, bit1 DMA trusted), transfers u32, stalls u32], then (newer firmware)
    paint totals since boot: [pixels sent to the panel u32, ms inside lcd stream calls u32, ms inside fs flash writes u32],
    then the LCD SPI3 clock divider u8 (4 at every boot since 2026-11-12, 8 on older firmware; 8 or 2 after the lab switch `lcd spi <n>`), then the longest time the
    charger-test page sat blocked from painting in the current or last run, u16 ms (0 = never; the key is absent on an
    older firmware)."""
    _need(p, 9, "lcd")
    state = "disabled" if not p[0] & 1 else ("ok" if p[0] & 2 else "stalled")
    r = {"dma": state, "transfers": int.from_bytes(p[1:5], "little"), "stalls": int.from_bytes(p[5:9], "little")}
    if len(p) >= 21:
        r["paint_px"] = int.from_bytes(p[9:13], "little")
        r["paint_ms"] = int.from_bytes(p[13:17], "little")
        r["fs_ms"] = int.from_bytes(p[17:21], "little")
    if len(p) >= 22:
        r["spi_div"] = p[21]
    if len(p) >= 24:
        r["blocked_ms"] = int.from_bytes(p[22:24], "little")
    return r


LOOP_BUCKETS = ["<=1", "<=2", "<=5", "<=10", "<=20", "<=50", "<=100", ">100"]   # ms, k2/core/loop_stats.hpp kEdgesMs


def decode_loop(p):
    """DebugLcd op 1 reply (58 bytes): elapsed ms u32; the gap between main-loop passes as max u16 + 8 bucket counts u16;
    the gap between 1 ms PD services (the pass or the LCD idle hook) the same way; the asked page's longest pass (page u8,
    us u32, px u32); the longest pass on any page (page u8, us u32, px u32). Counts saturate at 65535."""
    _need(p, 58, "loop")

    def u16(o):
        return int.from_bytes(p[o:o + 2], "little")

    def u32(o):
        return int.from_bytes(p[o:o + 4], "little")
    loop = [u16(6 + 2 * i) for i in range(8)]
    svc = [u16(24 + 2 * i) for i in range(8)]
    return {"elapsed_ms": u32(0), "loop_max_ms": u16(4), "loop_hist": dict(zip(LOOP_BUCKETS, loop)),
            "svc_max_ms": u16(22), "svc_hist": dict(zip(LOOP_BUCKETS, svc)),
            "page": p[40], "page_ms": u32(41) / 1000.0, "page_px": u32(45),
            "worst_page": p[49], "worst_ms": u32(50) / 1000.0, "worst_px": u32(54),
            "over_20ms": sum(loop[5:])}


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
    ri = f.get("ri")
    if ri is not None:   # the Rd intent marker as the boot read it (absent before 1.0.0-521)
        out["rd_marker"] = ({"read": False} if ri == "-" else {"read": True, "record": None} if ri == "0" else
                            {"read": True, "record": {"s": "session", "a": "reattach", "r": "rescan"}.get(ri[0], ri[0]),
                             "pin": int(ri[1]), "canary": ri[2] == "y", "host": ri[3] == "y"})
    if "sr" in f:
        out["sram_probe_words"] = int(f["sr"])
    if "bp" in f:
        b, prev = f["bp"].split(":")
        out["session"] = {"boots": int(b), "previous": prev == "1"}
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
    if answered_line(r):
        s += "\n  " + answered_line(r)
    if read_line(r):
        s += "\n  " + read_line(r)
    if pull_down_line(r):
        s += "\n  " + pull_down_line(r)
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
    if name == "charger-test" and "on_screen" in r:   # `charger-test view`
        pv, w = r["preview"], r["wait"]
        lines = [f"{r['stage']} (cursor {r['cursor']}/{r['rows']}{'' if r['on_screen'] else ', page not on screen'}): "
                 f"{r['message'] or '-'}"]
        if r["confirming"]:
            lines.append(f"  START confirmation open, {r['confirm_left_ms']} ms left" + (" (announces the re-attach)" if r["confirm_reattach"] else ""))
        if w["total_ms"]:
            lines.append(f"  waiting {w['elapsed_ms'] / 1000:g} of {w['total_ms'] / 1000:g} s" +
                         (", retry" if w["retry"] else "") + (", Rd off" if w["paused"] else ""))
        if r["run_elapsed_s"]:
            lines.append(f"  running {r['run_elapsed_s']} s")
        lines.append(f"  preview: {pv['runnable']} run, {pv['skipped']} skip; START refused: {pv['refusal']}" +
                     (f" at {pv['refusal_mv']} mV" if pv["refusal_mv"] else ""))
        lines.append(f"  guard {r['guard']['topology']}; self-supplied {'yes' if r['self_supplied'] else 'no'}; "
                     f"sweeps pps {r['sweep_points']['pps']} avs {r['sweep_points']['avs']}")
        return "\n".join(lines)
    if name == "charger-test":
        head =f"run {r['run_id']} rev {r['revision']}: {r['state']} ({r['reason']}); step {r['current']}/{r['total']}"
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
            q = e["ps_rdy"] or {"out_of_band": False, "inferred": False, "late": False, "worst_ms": 0}
            names = [n for k, n in (("out_of_band", "out of band"), ("inferred", "inferred"), ("late", "late")) if q[k]]
            if names:
                notes.append(f"  PS_RDY quirks: {', '.join(names)} (worst {q['worst_ms']} ms); no verdict reads them")
        rows = []
        for s in r["steps"]:
            rows.append(f"  {s['index']}: {s['protocol']} {s['target_mv']} mV -> {s['measured_mv']} mV "
                        f"{s['result']} ({s.get('explanation', s['reason'])}; {s['detail_reason']})")
            if "hunt" in s:
                rows.append(_hunt_line(s["hunt"]))
        return "\n".join([head] + notes + rows)
    if name == "diagnose" and isinstance(r, dict):
        return diagnose_text(r)
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
    if name == "sink":
        lines = ["sink tab (Connection):"]
        if r["k2_sink"]:   # the K2 draws the power itself: ID + Sink + the link rows only
            lines.append("  ID: K2")
            lines.append("  Sink: K2 (sink mode OK)" if r["k2_sink_ok"] else "  Sink: K2 (sink mode PLUS)")
        else:
            lines.append(f"  ID: {r['id']['sniffed'] or 'unknown'} "
                         f"(sniffed identity not exposed remotely; asked: {r['id']['asked']})")
            lines.append("  Request: " + (r["request"]["text"] if r["request"] else "none seen"))
            st = "  State: not exposed remotely"
            if r["flags"]:
                st += "; match flags: " + ", ".join(r["flags"])
            lines.append(st)
            lines.append("  Pwr: " + (r["pwr"] or "- (no PD contract; the Type-C budget row needs "
                                     "typec_ma, not exposed remotely)"))
            if r["data"]:
                lines.append(f"  Data: {r['data']}")
            caps = " ".join(r["caps"]) + (f" ({r['caps_src']})" if r["caps_src"] else "") if r["caps"] else "not seen"
            lines.append("  Caps: " + caps)
        lines.append("  USB: " + (r["usb"]["text"] or r["usb"]["family"]))
        if r["usb"]["tbt"]:
            lines.append(f"  Alt: {r['usb']['tbt']}")
        elif r["usb"]["dp"]:
            lines.append("  Alt: DP")
        return "\n".join(lines)
    if name == "pd":
        if r.get("pd_cmd") == "handback":
            return f"handback {r['verdict']}"   # the phase-1 lab op's verdict: allowed, or why it was refused
        return (f"trigger {r['state']} result={r['result']} held={r['held']}{'(auto)' if r.get('held') and r.get('auto_hold') else ''} epr={r['epr']} "
                f"pdo={r['pos']} {r['mv'] / 1000:g} V {r['ma'] / 1000:g} A hard_resets={r['hard_resets']} source_hard_resets={r['source_hard_resets']}"
                + (f" Rd pin CC{r['pin']}" if r.get("pin") else "")   # a leftover pinned Rd (pd-reread --pin)
                + (f" Rd trying CC{r['trial_line']}" if r.get("trial_line") else "")   # the direct-plug schedule is live on that line
                + (f" line_moves={r['trial_moves']}" if r.get("trial_moves") else ""))
    if name == "pd-reread":
        extra = " attaching" if r.get("attaching") else (" no contract" if r.get("attach_failed") else "")
        extra += " ra" if r.get("ra") else ""
        return (f"reread {r['state']}{extra} mask=0x{r['mask']:02X} answered={','.join(r['answered']) or '-'} "
                f"skipped={','.join(r['skipped']) or '-'} pdos {r['pdos_before']} -> {r['pdos_last']} "
                f"caps_rx={r['caps_rx']} avs={'yes' if r['avs'] else 'no'}")
    if name == "stop":
        what = "stopped: " + ", ".join(r["active"]) if r["active"] else "nothing was active"
        vbus = f"VBUS {r['vbus_mv'] / 1000:.2f} V" if r["vbus_mv"] >= 0 else "VBUS n/a"
        return f"stop: {what}; {vbus}" + (" (fc release running: VBUS not settled yet)" if r["fc_release_busy"] else "")
    if name == "lease":
        if r["armed"]:
            return f"lease armed: {r['left_s']} s left of {r['length_s']} s"
        return f"lease not armed (next one {r['length_s']} s; the last ended: {r['ended']})"
    if name == "apple-read":
        if not r.get("armed") and not r.get("reads"):
            return "apple-read: not armed (this command arms it; off by default)"
        lines = [f"apple-read {r['why']} reads={r['reads']} ok={r['ok']}"]
        lines += [f"  {k}: {r[k]}" for k in APPLE_FIELDS if r.get(k)]
        return "\n".join(lines)
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
    if r.get("twin"):
        return render_twin(r)
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
        if k == "probe":
            return render_ra_probe(r)
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
        if "vooc" in r:   # vooc-monitor
            return render_vooc(r["vooc"])
        if "svooc" in r:   # svooc-trigger
            return render_svooc(r["svooc"])
        if "svooc_log" in r:
            return render_svooc_log(r["svooc_log"])
        s = r["scan"]
        lines = [f"fc {r['state']} {r['proto']} target {r['target_mv']} mV, VBUS {r['vbus_mv']} mV"
                 + (f" ({r['fail']})" if r["fail"] != "none" else "")]
        if s["running"]:
            lines.append(f"scan running, step {s['step'] + 1}")
        lines += [f"  {x['proto']} {x['outcome']}" + (f" {x['mv']} mV" if x["mv"] else "") for x in s["results"]]
        return "\n".join(lines)
    if name == "pd-stress":    # PD Stress
        return render_pdstress(r)
    if name == "loop-probe":   # Loop probe
        return render_loop_probe(r)
    if name == "battery":      # Battery efficiency recorder
        return render_battery(r)
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
        if "loop" in r:
            return "loop " + " ".join(f"{k}={v}" for k, v in r["loop"].items())
        n = r["fake_pdos"]
        return f"fake PDOs: {n}" if n else "fake PDOs: off (live source caps)"
    if name == "screenshot":
        return (f"wrote {r['file']} ({r['width']}x{r['height']}), {r['rle_bytes']} bytes "
                f"compressed in {r['ms']} ms")
    return "ok " + " ".join(f"{k}={v}" for k, v in r.items())


COMMANDS = {"ping": cmd_ping, "press": cmd_press, "state": cmd_state, "pdos": cmd_pdos,
            "emarker": cmd_emarker, "identity": cmd_identity, "match": cmd_match, "usblink": cmd_usblink,
            "sink": cmd_sink, "action": cmd_action, "page": cmd_page,
            "reboot": cmd_reboot, "dfu": cmd_dfu, "factory-wipe": cmd_factory_wipe, "flash-dump": cmd_flash_dump, "bootinfo": cmd_bootinfo, "fault": cmd_fault, "log": cmd_log, "watch": cmd_watch,
            "screenshot": cmd_screenshot, "debug": cmd_debug,
            "adc-raw": cmd_adc_raw, "ripple": cmd_ripple, "fc": cmd_fc,
            "pd": cmd_pd, "identify-device": cmd_identify_device, "pd-reread": cmd_pd_reread, "apple-read": cmd_apple_read,
            "stop": cmd_stop, "lease": cmd_lease, "store": cmd_store, "blackbox": cmd_blackbox, "transcap": cmd_transcap,
            "diagnose": cmd_diagnose, "cable-r": cmd_cable_r, "ufcs": cmd_ufcs, "pd-stress": cmd_pd_stress, "loop-probe": cmd_loop_probe, "battery": cmd_battery, "lab": cmd_lab, "qrtest": cmd_qrtest, "charger-test": cmd_charger_test, "charger-units": cmd_charger_units,
            "charger-cable": cmd_charger_cable, "charger-sweep": cmd_charger_sweep, "charger-avs-sweep": cmd_charger_avs_sweep, "cards": cmd_cards,
            "twin": cmd_twin}


def _add_lease_args(parser):
    """--lease / --hold on a command that can leave the output above 5 V under host control (see the lease notes)."""
    parser.add_argument("--lease", type=_lease_secs, default=None, metavar="SECONDS",
                        help=f"dead-man lease length for this command, 0..{LEASE_MAX_S} (0 or omitted = the firmware default, 30 s). "
                             "If the K2 hears nothing from this host for that long, or the USB link drops, it runs `stop` itself. "
                             "On the wire: a separate lease-set request [1, SECONDS u16] sent just before the command")
    parser.add_argument("--hold", action="store_true",
                        help="after the command keep sending a request every few seconds to hold the lease, until Ctrl-C, then send `stop`")


def build_parser():
    p = argparse.ArgumentParser(description="Open-K2 remote control over USB HID")
    p.add_argument("--json", action="store_true", help="machine readable output")
    p.add_argument("--path", help="hidraw path (default: auto-detect)")
    p.add_argument("--timeout", type=float, default=1.0, help="response timeout in seconds")
    sub = p.add_subparsers(dest="command", required=True)
    sub.add_parser("ping", help="firmware version, git hash, uptime")
    sp = sub.add_parser("press", help="inject a button press (a logical key: plus = next/down, minus = previous/up, whatever the rotation)")
    sp.add_argument("button", choices=sorted(BUTTONS))
    g = sp.add_mutually_exclusive_group()
    g.add_argument("--long", action="store_true", help=f"hold {LONG_PRESS_MS} ms")
    g.add_argument("--ms", type=int, help="hold time in ms (30..10000); to reset the K2 use `reboot`")
    sub.add_parser("state", help="page, power readings, PD summary")
    st = sub.add_parser("charger-test", help="read existing RAM suite results (never starts or stops tests)")
    sts = st.add_subparsers(dest="test_cmd", required=True)
    sts.add_parser("status", help="read run/state metadata only")
    strc = sts.add_parser("results", help="retrieve a consistent bounded result table")
    strc.add_argument("--out", help="write the complete evidence JSON to this host file")
    sts.add_parser("view", help="what the charger-test screens show: stage, cursor, confirmation, wait / run clocks, preview, footer words")
    sms = sts.add_parser("summary", help="compact per-protocol summary of the RAM results")
    sms.add_argument("--out", help="also write the complete evidence JSON to this host file")
    cu = sub.add_parser("charger-units", help="set the test units (and their order) of the NEXT charger run; reset by every START")
    cu.add_argument("units", help="comma list, e.g. qc2,qc3,pd (names: " + ", ".join(TEST_UNITS) + "); 'default' = the default order")
    cc = sub.add_parser("charger-cable", help="cable identity the charger-test START dialog opens on (no argument: read it back)")
    cc.add_argument("cable", nargs="?", help="off | builtin | builtin:<id 1, 2, 4 | epr-50v | cuktech-6a | oneplus> | "
                    "saved:<library id>; Ra mode + spoof for the run unless off")
    for name, what in (("charger-sweep", "PPS"), ("charger-avs-sweep", "AVS")):
        cs = sub.add_parser(name, help=f"turn the {what} deep sweep of the NEXT charger run on or off "
                            "(opt-in: every 100 mV across each APDO, 1 A; never in a preset)")
        cs.add_argument("state", choices=["on", "off"])
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
    epb = es.add_parser("probe", help="silent-cable-chip probe, raw samples only: a cable ALONE in the BOTTOM port (no charger, "
                        "VBUS dark): idle levels and each CC line under the K2's own VCONN at 2 / 10 / 100 ms; the K2 refuses "
                        "(and says why) if anything but a bare cable is there")
    epb.add_argument("--vconn-pc", type=int, choices=range(0, 8), default=None, metavar="0..7",
                     help="PC13/14/15 levels during the VCONN phases (bit1 PC14, bit2 PC15; bit0 is refused); default 0, "
                     "the condition of the reference numbers; 6 closes both analog switches")
    sw = sub.add_parser("twin", help="device twins: what a charger would give a learned device, and a live proof of it")
    sws = sw.add_subparsers(dest="twin_cmd", required=True)
    sws.add_parser("list", help="saved twins")
    sx = sws.add_parser("show", help="one twin: sink PDOs, sniffed Request, USB data")
    sx.add_argument("id", type=int)
    sx = sws.add_parser("predict", help="what this charger would give each twin (no transmission)")
    sx.add_argument("ids", nargs="*", type=int, help="twin ids (default: all)")
    sx = sws.add_parser("run", help="live proof: negotiate with the charger AS each twin (needs `pd attach` first)")
    sx.add_argument("ids", nargs="*", type=int, help="twin ids (default: all)")
    sx.add_argument("--confirm", action="store_true", help="required: the contract changes while the proof runs")
    sx.add_argument("--wait", type=float, default=TW_RUN_WAIT_S, help=f"seconds to wait for the run (default {TW_RUN_WAIT_S})")
    _add_lease_args(sx)
    sws.add_parser("status", help="state of the live proof")
    sx = sws.add_parser("result", help="the live verdict held for one twin")
    sx.add_argument("id", type=int)
    sws.add_parser("cancel", help="stop a running live proof")
    sx = sws.add_parser("learn", help="make a twin from the last Identify (device alone) or a sniffed Request (inline)")
    sx.add_argument("source", choices=["identify", "sniff"])
    sx.add_argument("--name", help="1..24 characters")
    sx.add_argument("--dry", action="store_true", help="show what would be learned, save nothing")
    sx = sws.add_parser("rename", help="rename a twin")
    sx.add_argument("id", type=int)
    sx.add_argument("name")
    sx = sws.add_parser("delete", help="delete a twin")
    sx.add_argument("id", type=int)
    sx.add_argument("--confirm", action="store_true")
    sx = sws.add_parser("usb", help="a twin's USB data lines (declared / observed)")
    sx.add_argument("id", type=int)
    sx.add_argument("which", nargs="?", choices=list(twin_proto.USB_WHICH))
    sx = sws.add_parser("export", help="all twins as a JSON array (raw record hex plus USB text)")
    sx.add_argument("--out", required=True)
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
    sub.add_parser("sink", help="Connection page Sink tab, field for field (read-only; Identify status only, never starts a run)")
    sa = sub.add_parser("action", help="run a page action (fc-scan raises VBUS above 5 V and needs --confirm)")
    sa.add_argument("name", choices=sorted(ACTIONS))
    sa.add_argument("--confirm", action="store_true",
                    help="required for " + ", ".join(ACTIONS_NEED_CONFIRM) + " (the QC scan raises the output above 5 V and drives D+/D-); "
                         "refused with need-confirm without it. No other action takes it")
    _add_lease_args(sa)   # only fc-scan arms the lease
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
                         "Above 5 V it needs --confirm too (both flags). "
                         "Never reachable from the UI or the charger test")
    _add_lease_args(sq)
    sr = spds.add_parser("reset", help="PD hard reset (the supply returns at 5 V; refused while a live sink is attached)")
    sr.add_argument("--confirm", action="store_true", help="with --inline: confirm the lab-only Hard Reset while inline")
    sr.add_argument("--inline", action="store_true",
                    help="lab only: send one Hard Reset even while a live sink is attached inline (needs --confirm); "
                         "refused while a charger test runs and logged as a lab op on the K2. Never reachable from the "
                         "UI, the shell or the charger test")
    sh = spds.add_parser("handback", help="lab only (phase 1): hand the contract back - wait for /rd.rec to be gone, "
                                          "drop the Rd, send ONE Hard Reset; only when the K2 owns a contract while "
                                          "inline and the far sink sent no Request this attach; refused during a "
                                          "charger test. Never reachable from the UI, the shell or the charger test")
    sh.add_argument("--confirm", action="store_true", help="confirm the lab op (required)")
    spds.add_parser("release", help="back to 5 V (Request PDO 1)")
    sa = spds.add_parser("attach", help="attach as PD sink and request the target voltage (default 5000 mV; above 5 V needs "
                                         "--confirm; refused inline and during a charger test, with no Rd and nothing sent)")
    sa.add_argument("mv", type=int, nargs="?", default=5000, help="target voltage in mV (default 5000)")
    sa.add_argument("ma", type=int, nargs="?", default=0, help="target current in mA (default 0 = PDO max)")
    sa.add_argument("--confirm", action="store_true", help="allow a voltage above 5 V (the same flag as `pd request`)")
    _add_lease_args(sa)
    spds.add_parser("status", help="trigger engine state")
    si = sub.add_parser("identify-device", help="ask the attached device for its identity (idle CC only)")
    si.add_argument("--confirm", action="store_true",
                    help="no effect (kept so old scripts still parse): inline, with a live sink attached, the K2 refuses "
                         "Identify outright and the Sink tab shows the sniffed identity")
    si.add_argument("--wait", type=float, default=ID_POLL_S, help="seconds to wait for the sequence")
    sub.add_parser("stop", help="undo everything a host left holding the output, in one call and with no confirm: cancel a charger "
                                "test, stop a fast-charge trigger (measured 5 V release), release the PD contract to 5 V, disarm a "
                                "spoof. Prints what was active and VBUS (the same path as the shell `stop` line)")
    sl = sub.add_parser("lease", help="host dead-man lease: read it (no argument) or set its length in seconds (0 = default 30 s, max "
                                       f"{LEASE_MAX_S}). Armed by pd request / attach above 5 V, fc trigger above 5 V and fc scan; "
                                       "any request refreshes it; on expiry or a USB unplug the K2 runs `stop` itself. --json: "
                                       "{armed, left_s, length_s, ended}")
    sl.add_argument("seconds", type=_lease_secs, nargs="?", default=None, help="set the length (0 = default); omit to read")
    sa = sub.add_parser("apple-read", help="lab-only: read an Apple charger's register file (SVID 0x05AC). Arms the readout "
                                           "(our SVIDs ACK adds 0x05AC), re-requests the CURRENT contract and reads the result. "
                                           "Refused inline, without a contract, to a source that is not our partner, or while a run owns the link")
    sa.add_argument("--wait", type=float, default=APPLE_POLL_S, help="seconds to wait for the readout run")
    sr = sub.add_parser("pd-reread", help="source-cap re-read in PD mode: Get_Source_Cap_Extended, Discover Identity, SVIDs, Get_Source_Cap (see k2/pd/pd_reread.hpp)")
    sr.add_argument("--mask", type=_reread_mask, default=None, help="steps: 1 caps-ext, 2 identity, 4 svids, 8 get-cap, 16 second get-cap (default 15)")
    sr.add_argument("--wait", type=float, default=REREAD_POLL_S, help="seconds to wait for the run, attach phase included")
    sr.add_argument("--abort", action="store_true", help="stop a run and release the lab attach, whoever made it: the re-read's own, `pd attach` / "
                    "`pd request` (back to 5 V first), a finished re-read's Rd. Refused (busy, nothing changed) while a "
                    "charger test, PD Stress or twin run owns the link or a far sink is inline")
    sr.add_argument("--line", type=int, choices=(0, 1, 2), default=None,
                    help="debug: the attach scan may take only this CC line (Rd then goes on that line alone); with another "
                         "line already taken and no contract the run forgets it first. Stays set for later attaches (a charger "
                         "test START too) until --line 0 (then the run goes on normally), --abort or a reboot")
    sr.add_argument("--ra", choices=("on", "off"), default=None,
                    help="Ra mode: PC13 high puts an Ra on CC1 before Rd goes on CC2 alone, so a source sees a sink plus a "
                         "powered cable and may probe the cable (SOP' Discover Identity, answered by an armed e-marker spoof). "
                         "Held until detach, --abort, --ra off or a reboot; refused inline with a sink, with a contract or "
                         "while another run is active. Stays on for a charger test START too")
    sr.add_argument("--no-run", action="store_true", help="with --ra on, --pin or --line: only set the mode, start no run")
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
    sfc.add_argument("--allow-inline", action="store_true", help="override the PD-contract / load guard (with --confirm)")
    _add_lease_args(sfc)
    sft =sfs.add_parser("trigger", help="request one protocol at one voltage")
    sft.add_argument("proto", choices=FC_PROTOS)
    sft.add_argument("--volts", type=float, required=True)
    sft.add_argument("--confirm", action="store_true", help="required above 5 V, and with --allow-inline at any voltage")
    sft.add_argument("--allow-inline", action="store_true",
                     help="override the PD-contract / load guard (needs --confirm at any voltage)")
    _add_lease_args(sft)
    sfv = sfs.add_parser("vooc", help="inline VOOC monitor readout (passive, never drives; raw words)")   # vooc-monitor
    sfvs = sfv.add_mutually_exclusive_group()
    sfvs.add_argument("--on", action="store_true", help="enable the monitor (RAM only, on by default)")
    sfvs.add_argument("--off", action="store_true", help="disable it; the lines go back to the passive idle")
    # svooc-trigger
    sfsv = sfs.add_parser("svooc-trigger", help="guarded NO-LOAD SuperVOOC trigger: answers a SUPERVOOC adapter on D- "
                          "with the captured phone words, aborts above 12 V or 50 mA; refuses any PD contract but the K2's own "
                          "5 V one and aborts if it changes or is lost; polls until it ends, Ctrl-C stops")
    sfsv.add_argument("--confirm", action="store_true", help="required: the adapter may raise VBUS (up to the 12 V guard)")
    sfsv.add_argument("--cap", type=_svooc_cap, default=0, metavar="SECONDS",
                      help=f"run-time cap 1..{SVOOC_CAP_MAX_S} s (0 or omitted = {SVOOC_CAP_MAX_S} s)")
    sfsv.add_argument("--usb-a", dest="usb_a", action="store_true",
                      help="operator waiver for a legacy A port: skips ONLY the far-sink refusal (start and in-run abort), "
                      "for an A-to-C cable into the K2's bottom port; refused if any PD contract exists. Every other guard "
                      "stays (|I| 50 mA, VBUS 4.5-5.5 V at start, 12 V ceiling, off-script, host loss, cap). The report says "
                      "'far-sink check waived (USB-A)'")
    sfsv.add_argument("--i86-first", dest="i86_first", type=_svooc_ranged(SVOOC_I86_MIN, SVOOC_I86_MAX, "0x86 answer"),
                      default=None, metavar="N", help="the answer to the 0x86 poll after the handshake (a count of about "
                      f"100 mA), {SVOOC_I86_MIN}..0x{SVOOC_I86_MAX:X}; default 0x{SVOOC_I86_DEFAULT:03X}")
    sfsv.add_argument("--i86", dest="i86", type=_svooc_ranged(SVOOC_I86_MIN, SVOOC_I86_MAX, "0x86 answer"), default=None,
                      metavar="N", help="the answer to the 0x86 the charger sends after the escape (needs --i86-at); "
                      f"{SVOOC_I86_MIN}..0x{SVOOC_I86_MAX:X}; default 0x{SVOOC_I86_DEFAULT:03X}")
    sfsv.add_argument("--r82", dest="r82", type=_svooc_ranged(0, SVOOC_R82_MAX, "0x82 answer"), default=None, metavar="N",
                      help=f"constant answer to every 0x82 poll, 0..0x{SVOOC_R82_MAX:X} (default 0x{SVOOC_R82_DEFAULT:03X}); "
                      "VBUS settles near 20.35 mV * N + 6.9 V at 3.6 A")
    sfsv.add_argument("--vtarget", dest="vtarget", type=_svooc_ranged(0, SVOOC_VTARGET_MAX_MV, "VBUS target"), default=None,
                      metavar="MV", help="closed loop: compute each 0x82 answer from the measured VBUS so it settles at "
                      f"MV (at most {SVOOC_VTARGET_MAX_MV}); replaces --r82")
    sfsv.add_argument("--i86-at", dest="i86_at", type=_svooc_ranged(1, SVOOC_CAP_MAX_S, "escape time"), default=None,
                      metavar="S", help="after S seconds answer one 0x82 poll with 0x07F so the charger sends a fresh "
                      "0x86, answer it with --i86 and report whether it did")
    sfs.add_parser("svooc-status", help="the SuperVOOC trigger's last report and the first logged command/reply pairs")
    sfl = sfs.add_parser("svooc-log", help="the run's full command>reply list (up to 512 pairs of the latest wake), paged "
                         "from the device: index, ms since the wake, VBUS, command and reply words (--json for tools)")
    sfl.add_argument("--start", type=int, default=0, metavar="N", help="first pair index (default 0)")
    sfl.add_argument("--count", type=int, default=None, metavar="N", help="at most N pairs (default: all)")
    sfs.add_parser("svooc-stop", help="stop the SuperVOOC trigger: stop answering, lines to passive idle")
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
    sdl = sds.add_parser("loop", help="debug: main-loop stall counters (pass gap and PD service gap histograms, the longest "
                                      "pass per page); read-only apart from --reset, which zeroes them after the read")
    sdl.add_argument("--page", type=int, default=0, choices=range(256), metavar="N",
                     help="page index whose longest pass to report (Ui::page_index: 0 Home .. 5 Settings, 6+ menus)")
    sdl.add_argument("--reset", action="store_true", help="zero the counters after this read")
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
    sub.add_parser("diagnose", help="the Black Box diagnosis the K2 shows (verdict, findings, healthy lines; "
                                    "read-only; offline: tools/pd_trace.py diagnose)")
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
    # ---- PD Stress (docs/PD_STRESS.md): parser ----
    sps = sub.add_parser("pd-stress", help="PD Stress: renegotiation stress run on the attached charger (inside the PD rules, "
                         "no load); score 0 = ideal. Start, watch, read the raw result, stop; tools/pd_stress.py records and rescored runs")
    spss = sps.add_subparsers(dest="pdstress_cmd", required=True)
    spst = spss.add_parser("start", help="start a run (needs --confirm)")
    spst.add_argument("--confirm", action="store_true", help="required: the charger is renegotiated hard, nothing is on the output")
    spst.add_argument("--mask", type=lambda s: int(s, 0), default=0, help="sequence bit mask (0 = the default set)")
    spst.add_argument("--cycles", type=int, default=0, help="percent of every sequence's cycles (0 = 100)")
    spst.add_argument("--pace", type=int, default=0, choices=(0, 1, 2), help="0 per sequence, 1 all Settled, 2 all Ready")
    spst.add_argument("--max-s", type=int, default=0, help="run time cap in seconds (0 = default)")
    spst.add_argument("--max-trans", type=int, default=0, help="transition cap (0 = default)")
    spss.add_parser("status", help="state, sequence, transitions, score so far")
    spss.add_parser("result", help="the raw result: per-sequence counters and distributions, per-event points, the score")
    spss.add_parser("stop", help="stop the run (never refused)")
    # ---- end PD Stress parser ----
    sub.add_parser("loop-probe", help="the last HID-cable loop probe result stored on the K2 (read only; the probe "
                   "starts on the K2, Tools > Loop probe); raw values with --json")
    sbat = sub.add_parser("battery", help="battery efficiency recorder (Bench > Battery): status, start, end, next, finish, "
                          "config, list, read; raw values with --json (docs/BATTERY_RECORDER.md)")
    sbats = sbat.add_subparsers(dest="battery_cmd", required=True)
    sbats.add_parser("status", help="state, phase, Wh and mAh in/out, time, efficiency, end rule")
    sbst = sbats.add_parser("start", help="start a run: charge, discharge or round-trip (refused while one runs)")
    sbst.add_argument("mode", choices=("charge", "discharge", "round-trip"))
    sbst.add_argument("--wiring", choices=("same-port", "moved"), default=None,
                      help="same-port: the bank stays on the output port (discharge = reverse flow); moved: the bank "
                           "moves to the input port to discharge (default: the K2's setting)")
    sbats.add_parser("end", help="end the measuring phase by hand (a round trip then waits for its discharge)")
    sbats.add_parser("next", help="round trip: start the discharge now")
    sbats.add_parser("finish", help="end the run wherever it is")
    sbcf = sbats.add_parser("config", help="end rule and the next run's wiring (omitted values stay)")
    sbcf.add_argument("--end-ma", type=int, default=None, help="a phase ends under this current, 20..2000 mA (default 50)")
    sbcf.add_argument("--end-s", type=int, default=None, help="... held this long, 10..3600 s (default 60)")
    sbcf.add_argument("--wiring", choices=("same-port", "moved"), default=None)
    sbats.add_parser("list", help="ids of the stored runs, newest first")
    sbrd = sbats.add_parser("read", help="one run record with its curve (id 0 or omitted: the run in RAM)")
    sbrd.add_argument("id", type=int, nargs="?", default=0)
    sbrd.add_argument("--all", action="store_true", help="every stored run")
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
        def show(result):
            print(json.dumps(result) if a.json else render(a.command, result))

        # human lines (lease armed, holding) go to stderr with --json so stdout stays one JSON document
        out = (lambda text: print(text, file=sys.stderr)) if a.json else print
        run_command(link, a, out=out, show=show)
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
    except (StoreFailed, CardsFailed, TwinFailed, WipeFailed, DumpFailed) as e:
        print(f"error: {e}", file=sys.stderr)
        return 6
    except (RemoteError, FrameError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    finally:
        transport.close()


if __name__ == "__main__":
    sys.exit(main())
