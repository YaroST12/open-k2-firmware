// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// Open-K2 remote protocol for the WebHID dashboard: plain JS, no dependencies, no DOM.
// Mirrors tools/k2_remote.py (the reference); layouts are in include/k2/core/remote_proto.hpp and
// docs/ARCHITECTURE.md. tools/web/k2.html embeds this file verbatim in <script id="k2proto">
// (`make web-sync` refreshes it); tests/test_k2proto.mjs loads it under Node.
var K2Proto = (function () {
  var FRAME_SIZE = 64, REQ_HEADER = 5, RSP_HEADER = 6;
  var MAX_REQ_PAYLOAD = FRAME_SIZE - REQ_HEADER;
  var DFU_MAGIC = [0x44, 0x46, 0x55, 0x21];   // "DFU!", u32 0x21554644 little endian (kDfuMagic)
  var FACTORY_WIPE_MAGIC = [0x57, 0x49, 0x50, 0x45];   // "WIPE", u32 0x45504957 little endian (kFactoryWipeMagic)
  var CMD = { PING: 1, BUTTON: 2, GET_STATE: 3, GET_PDOS: 4, GET_EMARKER: 5, ACTION: 6, SET_PAGE: 7,
              REBOOT: 8, LOG: 9, SCREENSHOT: 10, CHARGER_TEST: 0x0B, CHARGER_SWEEP: 0x0F, DEBUG_PDOS: 0x20, ADC_RAW: 0x21, RIPPLE: 0x22, DEBUG_PD_MSG: 0x23,
              GET_IDENTITY: 0x24, DEBUG_LINK: 0x25, DEBUG_CC_PROBE: 0x2A, DFU_ENTER: 0x30, DEBUG_FAULT: 0x31, FC: 0x32, FACTORY_WIPE: 0x33, BOOT_INFO: 0x35, CABLE_R: 0x50, LAB: 0x51, UFCS: 0x52, QRTEST: 0x53, CHARGER_AVS_SWEEP: 0x54, EM_LIB: 0x61, CARDS: 0x62, BLACK_BOX: 0x70, TRANS_CAP: 0x71,
              IDENTIFY_DEVICE: 0x40, PD_REQUEST: 0x41, PD_RESET: 0x42, PD_TRIGGER_STATUS: 0x43, PD_RELEASE: 0x44, PD_MATCH: 0x45, PD_ATTACH: 0x48, PD_HANDBACK: 0x49,
              SCREEN_DATA: 0x8A, RIPPLE_DATA: 0x8B };
  var STATUS = { OK: 0, UNKNOWN_CMD: 1, BAD_LENGTH: 2, BAD_ARG: 3, BUSY: 4, NEED_CONFIRM: 5 };
  var STATUS_NAMES = ["ok", "unknown command", "bad length", "bad argument", "busy", "needs confirmation"];
  var SHOT_FLAG_LAST = 0x01, SHOT_FLAG_ABORT = 0x80, SHOT_FORMAT_RLE565 = 1;
  var BUTTONS = { back: 0, minus: 1, ok: 2, plus: 3 };
  var ACTIONS = { "pd-rescan": 1, "emarker": 2, "energy-reset": 3, "identify": 6 };
  // include/k2/pd/pd_sink.hpp and include/k2/pd/pd_identify.hpp enums, in order
  var PD_ERRS = ["ok", "no source caps", "bad PDO position", "over the voltage cap", "voltage outside the PDO range",
                 "inline: a live sink is attached", "busy (identify running)"];
  var SINK_STATES = ["idle", "epr-entering", "epr-wait-caps", "requesting", "transition", "ready", "aborting", "failed"];
  var SINK_RESULTS = ["none", "ok", "rejected", "wait-exhausted", "timeout", "tx-failed", "hard-reset", "soft-reset",
                      "aborted", "over-cap", "epr-failed", "bad-choice"];
  // Step.detail of an EPR skip (charger_test.hpp kDetailEprFail / kDetailMonitor*), above 128 + pd::Err
  var EPR_DETAILS = { 136: "EPR: entry refused", 137: "EPR: source rejects cable", 138: "EPR: source has no VCONN",
                      139: "EPR: refused (RDO)", 140: "EPR: source unable now", 141: "EPR: 5V PDO lacks EPR bit",
                      144: "EPR: above K2's 36V monitor", 145: "EPR: above K2's 48V monitor",
                      146: "EPR: above the K2's monitor", 147: "EPR offered: needs EPR cable",
                      148: "EPR list withheld: cable-gated",
                      149: "EPR offered; entry: refused", 150: "EPR offered; entry: cable rejected",
                      151: "EPR offered; entry: no VCONN", 152: "EPR offered; entry: refused (RDO)",
                      153: "EPR offered; entry: unable now", 154: "EPR offered; entry: 5V PDO lacks EPR bit",
                      155: "not unlocked (no OEM chain)", 156: "not unlocked (chain unfinished)", 157: "not unlocked (3 A cable)",
                      158: "not unlocked (vendor not accepted)", 159: "not unlocked (source reset)" };
  var ID_STATES = ["idle", "running", "done", "aborted"];
  var ID_STEPS = ["identity", "svids", "modes", "sink-cap", "status", "revision", "done"];
  // Rotation pages 0..5; while a submenu is open GET_STATE.page is 6 + index into MENUS (include/k2/ui/ui_types.hpp).
  var PAGES = ["home", "detail", "chargertest", "lines", "tools", "settings"];
  var MENUS = ["trigger", "emarker", "pdtrigger", "cabler", "blackbox", "spoof", "usblink", "autorotate", "cards", "share", "connection", "diag", "qrtest"];
  var SETTINGS_MENUS = ["usblink", "autorotate"];
  function pageName(i) {
    if (i < PAGES.length) return PAGES[i];
    var k = i - PAGES.length;
    return k < MENUS.length ? (SETTINGS_MENUS.indexOf(MENUS[k]) >= 0 ? "settings/" : "tools/") + MENUS[k] : String(i);
  }
  // fastcharge::Protocol in enum order (include/k2/fastcharge/fastcharge_classify.hpp)
  var FC_NAMES = ["...", "NO INPUT", "NO QUICK", "DCP 5V", "QC3.0", "QC2.0 5V", "QC2.0 9V", "QC2.0 12V",
                  "QC2.0 20V", "AFC FCP", "SCP", "APPLE 2.4A", "APPLE 2.1A", "APPLE 1.0A", "USB2.0 HI",
                  "USB1.1 LOW", "SAM 2.0A", "VOOC WARP", "SVOOC", "PD PE", "PD MTK", "PD", "USB DATA", "PD?"];
  var ADC_CHANNELS = [0, 1, 2, 3, 4, 5, 6, 7, 16, 17];
  var ADC_SCAN = ["aux", "cc1", "cc2", "dp", "dm", "ntc", "vref"];
  var RIPPLE_SAMPLES = 512, RIPPLE_FULL_SCALE = 4095, RIPPLE_CHUNK = 28;
  var FC_ALLOW_INLINE = 2;
  // Mirrors the on-device trigger page (include/k2/ui/ui_trigger.hpp): selectable levels in mV and the default index.
  var FC_LEVELS = {
    qc2: { mv: [5000, 9000, 12000, 20000], def: 1 }, qc3: { mv: [5000, 6000, 9000, 12000], def: 2 },
    "qc3+": { mv: [5000, 6000, 9000, 12000], def: 2 }, afc: { mv: [9000, 12000], def: 0 },
    fcp: { mv: [5000, 9000, 12000], def: 1 }, scp: { mv: [5000, 9000, 11000], def: 0 },
    vooc: { mv: [5000], def: 0 }, svooc: { mv: [5000], def: 0 }, svooc65: { mv: [5000], def: 0 },
    ufcs: { mv: [5000, 9000, 12000], def: 0 }, vivo: { mv: [5000, 7000, 9000, 11000], def: 0 }
  };
  var FC_PROTOS = ["qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo"];
  var FC_STATES = ["idle", "negotiating", "active", "failed"];
  var FC_FAILS = ["none", "need confirm", "over limit", "no power monitor", "busy", "unsupported", "no handshake",
                  "no response", "voltage not reached", "overvoltage", "source lost", "aborted", "PD or load active",
            "needs load", "handshake ok", "detected"];
  var FC_OUTCOMES = ["-", "accepted", "rejected", "skipped"];
  var FC_OP = { status: 0, reset: 1, scan: 2, trigger: 3 };
  var FC_CONFIRM = 1, FC_SAFE_MV = 5000;
  var TEST_GROUPS = ["pd", "qc2", "qc3", "qc3+", "afc", "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo"];
  var TEST_ALGORITHMS = ["pd2-fixed", "pd3x-fixed", "epr", "spr-avs", "pps", "qc2", "qc3", "qc3+", "afc",
                         "fcp", "scp", "vooc", "svooc", "svooc65", "ufcs", "vivo",
                         "pd3.0-revision", "pd3.1-revision", "pd3.2-revision", "epr-avs", "qc2b", "qc3b"];
  var TEST_ALGORITHM_GROUPS = [0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 0, 0, 0, 1, 2];
  var TEST_STAGES = ["algorithms", "group", "cable", "identity", "review", "running", "results", "steps", "detail",
                     "resume", "changes", "ask", "refuse", "waiting", "custom"];
  var TEST_STATES = ["idle", "baseline", "requesting", "holding", "releasing", "done", "stopped", "unsafe", "detach charger", "attach charger"];
  var TEST_RESULTS = ["pending", "running", "pass", "fail", "skip"];
  var TEST_REASONS = ["none", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
                      "no power monitor", "no source", "load attached", "tool busy", "fake offers", "request failed",
                      "timeout", "voltage outside band", "hard reset", "source lost", "cancelled", "5V release failed",
                      "offers changed", "PD active: use QC port", "revision cannot be forced", "select at least one test",
                      "physical cable limit", "unsupported PDO layout",
                      "charger stayed in PD", "rebooted during request", "rebooted during handover", "EPR entry refused", "no contract", "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered", "blocked by PD", "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START", "Class A only (12 V max)", "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target", "withdrawn by source", "wrong voltage on 5V contract", "needs load", "floored above target", "step resolution"];
  var TEST_ALL = (1 << TEST_ALGORITHMS.length) - 1;
  var LATENCY = ["n/a", "<10 ns (~1 m)", "10-20 ns (~2 m)", "20-30 ns (~3 m)", "30-40 ns (~4 m)",
                 "40-50 ns (~5 m)", "50-60 ns (~6 m)", "60-70 ns (~7 m)", ">70 ns"];
  var TERMINATION = ["Passive (VCONN not required)", "Passive (VCONN required)",
                     "Active (1 end powered)", "Active (both ends powered)"];
  var CHIPS = ["none", "INA228", "INA226", "INA237"];
  var SPEC_REVS = ["unknown", "PD2.0", "PD3.x", "reserved"];
  var PDO_TYPES = ["unknown", "fixed", "battery", "variable", "pps", "avs"];
  var SHORT_PRESS_MS = 100, LONG_PRESS_MS = 700;   // the firmware long-press threshold is 500 ms
  var BUSY_MARGIN_MS = 500, ACTION_TIMEOUT_MS = 15000;

  function FrameError(msg) { this.name = "FrameError"; this.message = msg; this.stack = new Error(msg).stack; }
  FrameError.prototype = Object.create(Error.prototype);
  function RemoteError(status, cmd, payload) {
    this.name = "RemoteError"; this.status = status; this.cmd = cmd; this.payload = payload || new Uint8Array(0);
    this.message = "command " + cmd + " failed: " + (STATUS_NAMES[status] || status);
    if (cmd === CMD.CHARGER_TEST && status === STATUS.BUSY) this.message += " (RAM results changed; retry results)";
    if ((cmd === 0x41 || cmd === 0x42 || cmd === 0x44) && this.payload.length && this.payload[0] > 0 && this.payload[0] < PD_ERRS.length)
      this.message += " (" + PD_ERRS[this.payload[0]] + ")";
    this.stack = new Error(this.message).stack;
  }
  RemoteError.prototype = Object.create(Error.prototype);
  function RemoteTimeout(msg) { this.name = "RemoteTimeout"; this.message = msg || "no response"; this.stack = new Error(this.message).stack; }
  RemoteTimeout.prototype = Object.create(Error.prototype);
  function BootloaderDetected() {
    this.name = "BootloaderDetected"; this.message = "reply is a bootloader frame";
    this.stack = new Error(this.message).stack;
  }
  BootloaderDetected.prototype = Object.create(Error.prototype);

  // ---- framing ---------------------------------------------------------------------------------

  function packRequest(cmd, seq, payload) {
    payload = payload || [];
    if (payload.length > MAX_REQ_PAYLOAD) throw new RangeError("payload " + payload.length + " > " + MAX_REQ_PAYLOAD);
    var f = new Uint8Array(FRAME_SIZE);
    f[0] = 0xA5; f[1] = 0x4B; f[2] = cmd & 0xFF; f[3] = seq & 0xFF; f[4] = payload.length;
    f.set(payload, REQ_HEADER);
    return f;
  }

  function isResponse(raw) { return raw.length === FRAME_SIZE && raw[0] === 0xA6 && raw[1] === 0x4B; }
  function isBootloaderFrame(raw) { return raw.length >= 2 && raw[0] === 0xFF && raw[1] === 0x55; }

  // What an IN report is: a remote response, a bootloader frame, log text (first byte is
  // ASCII, never 0x80+) or an all-zero/empty idle report. Same split as k2_flasher.is_log_report.
  function classify(raw) {
    if (isResponse(raw)) return "response";
    if (isBootloaderFrame(raw)) return "bootloader";
    if (raw.length && raw[0] < 0x80) { for (var i = 0; i < raw.length; i++) if (raw[i]) return "log"; }
    return "idle";
  }

  function logText(raw) {
    var n = raw.indexOf(0);
    return new TextDecoder("utf-8").decode(n < 0 ? raw : raw.subarray(0, n));
  }

  function parseResponse(raw) {
    if (raw.length !== FRAME_SIZE) throw new FrameError("frame length " + raw.length + " != " + FRAME_SIZE);
    if (raw[0] !== 0xA6 || raw[1] !== 0x4B) throw new FrameError("not a remote response (bad magic)");
    var n = raw[5];
    if (n > FRAME_SIZE - RSP_HEADER) throw new FrameError("payload length " + n + " exceeds frame");
    return { cmd: raw[2], seq: raw[3], status: raw[4], payload: raw.slice(RSP_HEADER, RSP_HEADER + n) };
  }

  // ---- decoders --------------------------------------------------------------------------------

  function need(p, n, what) {
    if (p.length < n) throw new FrameError(what + " payload too short (" + p.length + " < " + n + ")");
  }
  function cstr(b) { var n = b.indexOf(0); return new TextDecoder("utf-8").decode(n < 0 ? b : b.subarray(0, n)); }
  function name(table, key) { return key < table.length ? table[key] : String(key); }
  function dv(p) { return new DataView(p.buffer, p.byteOffset, p.byteLength); }

  function directionOf(ma) { return ma > 0 ? "up" : ma < 0 ? "down" : "idle"; }

  function decodePing(p) {
    need(p, 29, "ping");
    return { proto: p[0], uptime_ms: dv(p).getUint32(1, true), git: cstr(p.subarray(5, 29)), version: cstr(p.subarray(29)) };
  }

  function decodeChargerTest(p) {
    need(p, 15, "charger-test");
    var v = dv(p), modern = p[0] === 2;
    if (p[0] !== 1 && !modern) throw new FrameError("unsupported charger-test schema " + p[0]);
    var header = modern ? 40 : 15, page = modern ? 1 : 2, reasons = modern ? TEST_REASONS.length : 20;
    var total = p[11], current = p[12], start = p[13], n = p[14];
    if (p[9] >= TEST_STATES.length || p[10] >= reasons || total > (modern ? 106 : 95) || current > total ||
        start > total || n > page || start + n > total || p.length !== header + 16 * n)
      throw new FrameError("invalid charger-test page shape");
    var out = { schema: p[0], run_id: v.getUint32(1, true), revision: v.getUint32(5, true),
                state: TEST_STATES[p[9]], reason: TEST_REASONS[p[10]], total: total, current: current,
                start: start, count: n, selection: null, stage: null, evidence: null };
    var runMask = 0, wire = 0;
    if (modern) {
      runMask = v.getUint32(15, true);
      var draft = v.getUint32(19, true), stage = p[23] >> 2, f = p[25];
      var ppsSweep = draft >= 0x80000000;   // draft bit 31: the PPS deep sweep is on (an Options flag, not a test)
      draft = draft & 0x7FFFFFFF;
      wire = p[23] & 3;
      if ((runMask | draft) & ~TEST_ALL || stage >= TEST_STAGES.length || (p[39] & 15) > 11)
        throw new FrameError("invalid charger-test selection or evidence");
      out.selection = { run_mask: runMask, configured_mask: draft, pps_sweep: ppsSweep,
                        run: TEST_ALGORITHMS.filter(function (_, i) { return runMask & (1 << i); }),
                        configured: TEST_ALGORITHMS.filter(function (_, i) { return draft & (1 << i); }) };
      out.stage = TEST_STAGES[stage];
      out.evidence = {
        origin: out.run_id ? "run-start" : "no-run", source_session: v.getUint32(33, true), raw_header: v.getUint16(37, true), pdo_count: p[39] & 15,
        wire_revision_code: wire, wire_revision: SPEC_REVS[wire],
        source_rmdo: f & 1 ? { revision: (p[24] >> 4) + "." + (p[24] & 15), version: (p[32] >> 4) + "." + (p[32] & 15),
                              provenance: "source self-report" } : null,
        advertised: { pps: !!(f & 2), epr_table: !!(f & 4), spr_avs: !!(f & 8), epr_capable: !!(f & 16), partial_epr: !!(f & 64) },
        monitor_mv: v.getUint16(26, true),
        physical_cable: { observed: !!(f & 32), max_mv: v.getUint16(28, true), max_ma: v.getUint16(30, true) },
        spoof_on: !!(f & 128), independent_revision_tested: false,
        pass_scope: "negotiation and measured voltage only; not rated-current/power or revision conformance"
      };
    }
    out.steps = [];
    for (var i = 0; i < n; i++) {
      var at = header + i * 16, code = p[at], pos = p[at + 1], flags = p[at + 2],
          verdict = p[at + 9], why = p[at + 10], detail = p[at + 11], raw = v.getUint32(at + 12, true);
      if (code >= (modern ? TEST_ALGORITHMS.length : 10) || pos > 11 || verdict >= TEST_RESULTS.length ||
          why >= reasons || flags & ~31 || (verdict === 4 && why === 0))
        throw new FrameError("invalid charger-test row");
      var group = modern ? TEST_ALGORITHM_GROUPS[code] : code, protocol = TEST_GROUPS[group];
      if (modern) {
        if (!(runMask & (1 << code))) throw new FrameError("charger-test row was not selected");
        if (code >= 14 && code <= 18 && (verdict !== 4 || why !== (code < 16 ? 4 : 20)))
          throw new FrameError("unavailable charger-test algorithm must SKIP");
        if (code < 2 && verdict !== 4 && wire !== code + 1)
          throw new FrameError("charger-test row contradicts negotiated wire revision");
        protocol = code === 0 ? "pd2-fixed" : code === 1 ? "pd3x-fixed" :
                   code === 2 ? (flags & 2 ? "epr-avs" : "epr-fixed") : TEST_ALGORITHMS[code];
        if (code >= 11 && code <= 13) protocol += "-5v-handshake";
      } else if (group === 0 && pos) {
        var kind = raw >>> 30;
        protocol = kind === 0 ? "pd-fixed" : kind === 3 ? (flags & 2 ? "pd-avs" : "pd-pps") : "pd-other";
      }
      var names = group === 0 ? (detail >= 128 ? PD_ERRS : SINK_RESULTS) : FC_FAILS;
      var detailCode = group === 0 && detail >= 128 ? detail - 128 : detail;
      var row = { index: start + i, group: TEST_GROUPS[group], protocol: protocol, pos: pos,
                  target_mv: v.getUint16(at + 3, true), request_ma: v.getUint16(at + 5, true),
                  measured_mv: v.getUint16(at + 7, true), result: TEST_RESULTS[verdict], reason: TEST_REASONS[why],
                  detail: detail, detail_reason: group === 0 && EPR_DETAILS[detail] ? EPR_DETAILS[detail] :
                    detail ? (names[detailCode] || "unknown (" + detail + ")") : "none",
                  raw_pdo: raw, continuous: !!(flags & 1), avs: !!(flags & 2), attempts: (flags >> 2) & 7 };
      if (modern) {
        row.algorithm = TEST_ALGORITHMS[code];
        row.explanation = code >= 16 && code <= 18 ? "No independent PD3." + (code - 16) + " test" :
          why === 20 ? "Observed " + SPEC_REVS[wire] + "; revision cannot be forced" :
          // mirrors ct::explanation(): every EPR skip names its cause
          why === 3 && (code === 2 || code === 19) ? (EPR_DETAILS[detail] || "EPR: above the K2's monitor") :
          why === 2 && pos ? "No allowed current at target" :
          why === 22 && (code === 2 || code === 19) ? (detail >= 147 && detail <= 154 ? EPR_DETAILS[detail] : "EPR: cable not 50V 5A") :
          why === 2 && (code === 2 || code === 19) ? "EPR: not offered" :
          why === 27 ? (EPR_DETAILS[detail] || "EPR: entry refused") :
          why === 28 ? "EPR: no PD contract" :
          why === 38 ? (EPR_DETAILS[detail] || TEST_REASONS[why]) :
          why === 46 ? (detail ? "step resolution " + detail + "mV" : "steps ignored") :
          why === 22 && code === 10 ? "SCP 5A modes need an observed 5A cable" : TEST_REASONS[why];
      }
      out.steps.push(row);
    }
    return out;
  }

  // 24 bytes: the power block. 38: + analog front end. 40: + MCU temperature. 49: + link flags and
  // ages (k2/pd-read). Older firmware sends the shorter forms, so every extension is optional.
  function decodeState(p) {
    need(p, 24, "state");
    var v = dv(p);
    var st = { page: p[0], page_name: pageName(p[0]), active_cc: p[1], spec_rev: name(SPEC_REVS, p[2]),
               num_pdos: p[3], pd_valid: !!p[4], chip: name(CHIPS, p[5]), pm_online: !!p[6], log_on: !!p[7],
               vbus_mv: v.getInt32(8, true), current_ma: v.getInt32(12, true), power_mw: v.getInt32(16, true),
               energy_mwh: v.getInt32(20, true), direction: directionOf(v.getInt32(12, true)),
               adc_valid: false, temp_valid: false, temp_calibrated: false, mcu_valid: false, link: null };
    if (p.length >= 38) {
      var f = p[37];
      st.dp_mv = v.getUint16(24, true); st.dm_mv = v.getUint16(26, true);
      st.cc1_mv = v.getUint16(28, true); st.cc2_mv = v.getUint16(30, true); st.aux_mv = v.getUint16(32, true);
      st.temp_c = v.getInt16(34, true) / 10;
      st.fc_code = p[36]; st.fc = name(FC_NAMES, p[36]);
      st.adc_valid = !!(f & 1); st.temp_valid = !!(f & 2); st.temp_calibrated = !!(f & 4); st.mcu_valid = p.length >= 40 && !!(f & 8);
    }
    if (p.length >= 40) st.mcu_temp_c = v.getInt16(38, true) / 10;
    if (p.length >= 49) {
      var lf = p[40];
      st.link = { stale: !!(lf & 1), waiting: !!(lf & 2), inline: !!(lf & 4), cable_stale: !!(lf & 8),
                  cable_scanned: !!(lf & 16), age_s: v.getUint32(41, true), cable_age_s: v.getUint32(45, true) };
    }
    return st;
  }

  function decodeAdcRaw(p) {
    need(p, 35, "adc raw");
    var v = dv(p), channels = {}, scan = {}, i;
    for (i = 0; i < ADC_CHANNELS.length; i++) channels[ADC_CHANNELS[i]] = v.getUint16(1 + 2 * i, true);
    for (i = 0; i < ADC_SCAN.length; i++) scan[ADC_SCAN[i]] = v.getUint16(21 + 2 * i, true);
    return { ok: !!p[0], channels: channels, scan: scan };
  }

  function decodeRippleInfo(p) {
    need(p, 13, "ripple");
    var v = dv(p);
    return { code: p[0], period_ns: v.getUint32(1, true), count: v.getUint16(5, true), min: v.getUint16(7, true),
             max: v.getUint16(9, true), mean: v.getUint16(11, true) };
  }

  // GET_IDENTITY (k2/pd-read): flags(1) valid(1) vid(2) pid(2) bcd(2) num_svids(1) svids(2 each) "vendor\0model"
  function decodeIdentity(p) {
    need(p, 10, "identity");
    var v = dv(p), n = p[8];
    need(p, 9 + 2 * n, "identity svids");
    var svids = [], i;
    for (i = 0; i < n; i++) svids.push(v.getUint16(9 + 2 * i, true));
    var rest = p.subarray(9 + 2 * n), nul = rest.indexOf(0);
    return { valid: !!p[1], epr: !!(p[0] & 1), epr_partial: !!(p[0] & 2), cuktech: !!(p[0] & 4), xiaomi: !!(p[0] & 8),
             vid: v.getUint16(2, true), pid: v.getUint16(4, true), bcd: v.getUint16(6, true), svids: svids,
             vendor: cstr(nul < 0 ? rest : rest.subarray(0, nul)), model: nul < 0 ? "" : cstr(rest.subarray(nul + 1)) };
  }

  // GET_EMARKER part 2 (k2/pd-read): cable VDO detail. The firmware sends numbers, they are named here.
  function decodeEmarkerDetail(p) {
    need(p, 10, "emarker detail");
    var f = p[5];
    return { sop: ["none", "SOP'", "SOP''"][p[0]] || "none", pd_rev: { 1: "2.0", 2: "3.x" }[p[1]] || null,
             vdo_version: p[2], latency_code: p[3], latency: LATENCY[p[3]] || "code " + p[3],
             termination: TERMINATION[p[4] & 3],
             type_c_plug: !!(f & 1), vconn_required: !!(f & 2), epr_cable: !!(f & 4), vdo2_valid: !!(f & 8),
             usb4: !!(f & 16), usb_gen2: !!(f & 32), optical: !!(f & 64), thunderbolt: !!(f & 128),
             max_temp_c: p[6], shutdown_temp_c: p[7], u3_power: p[8], usb_lanes: p[9] };
  }

  // FC (k2/fc-trigger): state proto fail scan_flags scan_step target_mv(2) vbus_mv(2) n { proto outcome mv(2) } x n
  function decodeFc(p) {
    need(p, 10, "fc");
    var v = dv(p), n = p[9];
    need(p, 10 + 4 * n, "fc results");
    var results = [];
    for (var i = 0; i < n; i++)
      results.push({ proto: name(FC_PROTOS, p[10 + 4 * i]).toUpperCase(), outcome: name(FC_OUTCOMES, p[11 + 4 * i]),
                     mv: v.getUint16(12 + 4 * i, true) });
    return { state: name(FC_STATES, p[0]), proto: name(FC_PROTOS, p[1]).toUpperCase(), fail: name(FC_FAILS, p[2]),
             target_mv: v.getUint16(5, true), vbus_mv: v.getUint16(7, true),
             scan: { running: !!(p[3] & 1), done: !!(p[3] & 2), step: p[4], results: results } };
  }

  // CABLE_R (k2/cable_r): flags grade r_mohm(2) idle_mv(2) idle_ma(2) load_mv(2) load_ma(2) pos_mohm(2) gnd_mohm(2)
  // flags bit0 idle taken, bit1 valid, bit2 Kelvin 8-wire mode (r_mohm = total, pos/gnd its legs)
  var CABLE_GRADES = ["waiting", "excellent", "good", "fair", "poor"];
  function decodeCableR(p) {
    need(p, 16, "cable-r");
    var v = dv(p);
    return { idle: !!(p[0] & 1), valid: !!(p[0] & 2), kelvin: !!(p[0] & 4),
             pos_mohm: v.getUint16(12, true), gnd_mohm: v.getUint16(14, true), grade: p[1] < CABLE_GRADES.length ? CABLE_GRADES[p[1]] : "?" + p[1],
             r_mohm: v.getUint16(2, true), idle_mv: v.getUint16(4, true), idle_ma: v.getUint16(6, true),
             load_mv: v.getUint16(8, true), load_ma: v.getUint16(10, true) };
  }

  // Device statistics have no command: the firmware only prints them as log lines. Returns null for
  // any other line.
  function parseLogStats(line) {
    var m = /^\[USB\] gap=(\d+)ms isr=(\d+) rst=(\d+) sent=(\d+) drop=(\d+)/.exec(line);
    if (m) return { kind: "usb", gap_ms: +m[1], isr: +m[2], resets: +m[3], sent: +m[4], dropped: +m[5] };
    m = /^\[RST\] CTRLSTS=([0-9A-Fa-f]+)\s*(.*?)\s*$/.exec(line);
    if (m) return { kind: "rst", ctrlsts: parseInt(m[1], 16), flags: m[2] };
    m = /^\[SYS\] Heartbeat: up=(\d+)s presses=(\d+) page=(\d+)/.exec(line);
    if (m) return { kind: "heartbeat", up_s: +m[1], presses: +m[2], page: +m[3] };
    if (/^\[FAULT\]/.test(line)) return { kind: "fault", text: line.replace(/\s+$/, "") };
    return null;
  }

  function formatAgo(s) {
    if (s < 5) return "just now";
    if (s < 60) return s + " s ago";
    if (s < 3600) return Math.floor(s / 60) + " min ago";
    return Math.floor(s / 3600) + " h ago";
  }

  function formatUptime(ms) {
    var t = Math.floor(ms / 1000), d = Math.floor(t / 86400), h = Math.floor(t % 86400 / 3600),
        m = Math.floor(t % 3600 / 60), s = t % 60, z = function (n) { return (n < 10 ? "0" : "") + n; };
    if (d) return d + " d " + z(h) + " h " + z(m) + " min";
    if (h) return h + " h " + z(m) + " min " + z(s) + " s";
    if (m) return m + " min " + z(s) + " s";
    return s + " s";
  }

  function decodePdo(p) {
    need(p, 4, "pdo");
    var head = { valid: !!p[0], spec_rev: name(SPEC_REVS, p[1]), num_pdos: p[2], index: p[3] };
    if (p.length < 19) return { head: head, pdo: null };
    var v = dv(p);
    return { head: head, pdo: { type: name(PDO_TYPES, p[4]), raw: v.getUint32(5, true), min_mv: v.getUint16(9, true),
             max_mv: v.getUint16(11, true), max_ma: v.getUint16(13, true), max_mw: v.getUint32(15, true) } };
  }

  // PD_TRIGGER_STATUS: [state, result, held, epr, pos, mv u16, ma u16, hard_resets u32] (13 bytes read here; a source_hard_resets u32 and a pin u8 follow, not shown).
  function decodePdTrigger(p) {
    need(p, 13, "pd trigger status");
    var v = dv(p);
    return { state: name(SINK_STATES, p[0]), result: name(SINK_RESULTS, p[1]), held: !!p[2], epr: !!p[3], pos: p[4],
             mv: v.getUint16(5, true), ma: v.getUint16(7, true), hard_resets: v.getUint32(9, true) };
  }

  // PD_MATCH: [caps_seen, request_seen, sink_ok, flags, verdict, rows, want_pos, want_mv u16, want_ma u16,
  // need_ma u16, have_ma u16, src_max_mv u16, src_max_w u16, row state per PDO].
  var MATCH_VERDICTS = ["no-caps", "no-request", "outside-caps", "wants-higher-v", "current-limited", "mismatch-bit", "sink-unknown", "matches"];
  var MATCH_ROWS = ["unknown", "selected", "selected-low", "ok", "low", "no", "gone"];
  var MATCH_FLAGS = [[1, "mismatch-bit"], [2, "wants-higher-v"], [4, "current-limited"], [8, "outside-caps"]];
  function decodeMatch(p) {
    need(p, 19, "match");
    var v = dv(p), n = p[5], rows = [];
    need(p, 19 + n, "match rows");
    for (var i = 0; i < n; i++) rows.push(name(MATCH_ROWS, p[19 + i]));
    return { caps_seen: !!p[0], request_seen: !!p[1], sink_ok: !!p[2],
             flags: MATCH_FLAGS.filter(function (f) { return p[3] & f[0]; }).map(function (f) { return f[1]; }),
             verdict: name(MATCH_VERDICTS, p[4]), want_pos: p[6], want_mv: v.getUint16(7, true), want_ma: v.getUint16(9, true),
             need_ma: v.getUint16(11, true), have_ma: v.getUint16(13, true), src_max_mv: v.getUint16(15, true),
             src_max_w: v.getUint16(17, true), rows: rows };
  }

  // IDENTIFY_DEVICE: [state, step, ok_mask, skipped_mask, retries, n_sink, revision u32, sink PDOs u32 each].
  function decodeIdentifyProgress(p) {
    need(p, 10, "identify status");
    var v = dv(p), n = p[5];
    need(p, 10 + 4 * n, "identify sink pdos");
    var answered = [], noAnswer = [], pdos = [];
    for (var i = 0; i < 6; i++) { if (p[2] & (1 << i)) answered.push(ID_STEPS[i]); if (p[3] & (1 << i)) noAnswer.push(ID_STEPS[i]); }
    for (var k = 0; k < n; k++) pdos.push(v.getUint32(10 + 4 * k, true));
    return { state: name(ID_STATES, p[0]), step: name(ID_STEPS, p[1]), answered: answered, no_answer: noAnswer,
             retries: p[4], revision: v.getUint32(6, true), sink_pdos: pdos };
  }

  function decodeEmarker(p0, p1, p2) {
    need(p0, 17, "emarker");
    p1 = p1 || new Uint8Array(0);
    var v = dv(p0), rest = p0.subarray(17), nul = rest.indexOf(0);
    var vendor = nul < 0 ? rest : rest.subarray(0, nul), speed = nul < 0 ? new Uint8Array(0) : rest.subarray(nul + 1);
    var vdos = [], v1 = dv(p1);
    for (var o = 0; o + 4 <= p1.length; o += 4) vdos.push(v1.getUint32(o, true));
    return { valid: !!p0[0], active: !!p0[1], vid: v.getUint16(2, true), pid: v.getUint16(4, true),
             xid: v.getUint32(6, true), hw_rev: p0[10], fw_rev: p0[11], max_current_a: p0[12],
             max_voltage_v: p0[13], max_power_w: v.getUint16(14, true), num_vdos: p0[16],
             vendor: cstr(vendor), speed: cstr(speed), vdos: vdos,
             detail: p2 ? decodeEmarkerDetail(p2) : null };
  }

  // ---- screenshots -----------------------------------------------------------------------------

  // RLE565 (include/k2/ui/rle565.hpp): token 0x00..0x7F = run of 1..128 equal pixels, 0x80..0xFF = 1..128 literals.
  function rle565Decode(data, expected) {
    var out = new Uint16Array(expected), n = data.length, i = 0, k = 0;
    while (i < n) {
      var t = data[i++], cnt, j;
      if (t < 0x80) {
        if (i + 2 > n) throw new FrameError("truncated run token");
        cnt = t + 1;
        if (k + cnt > expected) throw new FrameError("strip decodes to more than " + expected + " pixels");
        var px = data[i] | (data[i + 1] << 8);
        for (j = 0; j < cnt; j++) out[k++] = px;
        i += 2;
      } else {
        cnt = t - 0x7F;
        if (i + 2 * cnt > n) throw new FrameError("truncated literal token");
        if (k + cnt > expected) throw new FrameError("strip decodes to more than " + expected + " pixels");
        for (j = 0; j < cnt; j++) { out[k++] = data[i] | (data[i + 1] << 8); i += 2; }
      }
    }
    if (k !== expected) throw new FrameError("strip decodes to " + k + " pixels, expected " + expected);
    return out;
  }

  function decodeScreenInfo(p) {
    need(p, 7, "screen info");
    var v = dv(p), w = v.getUint16(0, true), h = v.getUint16(2, true), rows = p[4], strips = p[5], fmt = p[6];
    if (fmt !== SHOT_FORMAT_RLE565) throw new FrameError("unsupported screenshot format " + fmt);
    if (rows * strips !== h) throw new FrameError("strips " + strips + " x " + rows + " rows do not cover height " + h);
    return { width: w, height: h, strip_rows: rows, num_strips: strips };
  }

  function rgb565ToRgba(px) {
    var out = new Uint8ClampedArray(px.length * 4);
    for (var i = 0; i < px.length; i++) {
      var v = px[i], r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
      out[4 * i] = (r << 3) | (r >> 2);
      out[4 * i + 1] = (g << 2) | (g >> 4);
      out[4 * i + 2] = (b << 3) | (b >> 2);
      out[4 * i + 3] = 255;
    }
    return out;
  }

  // Frame handler for a SCREENSHOT request: ack with the geometry, then strip frames (cmd 0x8A),
  // each strip split over chunks, the last chunk flagged. Strips may arrive in any order.
  function screenshotHandler(cmd) {
    var info = null, per = 0, strips = [], buf = {}, got = 0;
    return function (rsp) {
      if (rsp.cmd === cmd) {
        if (rsp.status !== STATUS.OK) throw new RemoteError(rsp.status, cmd);
        info = decodeScreenInfo(rsp.payload);
        per = info.width * info.strip_rows;
        return { more: true };
      }
      if (rsp.cmd !== CMD.SCREEN_DATA || !info) return undefined;
      var p = rsp.payload;
      if (p.length < 2) throw new FrameError("short stream frame");
      var idx = p[0], flags = p[1], data = p.subarray(2);
      if (flags & SHOT_FLAG_ABORT) throw new RemoteError(STATUS.BUSY, cmd);
      if (idx >= info.num_strips) throw new FrameError("strip index " + idx + " out of range");
      (buf[idx] = buf[idx] || []).push(data);
      if (flags & SHOT_FLAG_LAST) {
        var parts = buf[idx], len = 0, o = 0, j;
        for (j = 0; j < parts.length; j++) len += parts[j].length;
        var enc = new Uint8Array(len);
        for (j = 0; j < parts.length; j++) { enc.set(parts[j], o); o += parts[j].length; }
        delete buf[idx];
        if (!strips[idx]) got++;
        strips[idx] = rle565Decode(enc, per);
      }
      if (got < info.num_strips) return { more: true };
      var px = new Uint16Array(info.width * info.height);
      for (var s = 0; s < info.num_strips; s++) px.set(strips[s], s * per);
      return { value: { width: info.width, height: info.height, rgba: rgb565ToRgba(px) } };
    };
  }

  // Frame handler for a RIPPLE request: the answer carries the statistics, then the samples follow as
  // stream frames (cmd 0x8B: offset(2) + <= 28 u16). Frames may repeat or arrive in any order.
  function rippleHandler(cmd) {
    var info = null, samples = null, have = null, got = 0;
    return function (rsp) {
      if (rsp.cmd === cmd) {
        if (rsp.status !== STATUS.OK) throw new RemoteError(rsp.status, cmd);
        info = decodeRippleInfo(rsp.payload);
        samples = new Array(info.count); have = new Array(info.count);
        if (info.count === 0) return { value: Object.assign({}, info, { samples: [] }) };
        return { more: true };
      }
      if (rsp.cmd !== CMD.RIPPLE_DATA || !info) return undefined;
      var p = rsp.payload;
      if (p.length < 2) throw new FrameError("short ripple frame");
      var off = p[0] | (p[1] << 8), n = (p.length - 2) >> 1, v = dv(p);
      for (var i = 0; i < n; i++) {
        if (off + i >= info.count) throw new FrameError("ripple sample index " + (off + i) + " out of range");
        if (!have[off + i]) { have[off + i] = true; got++; }
        samples[off + i] = v.getUint16(2 + 2 * i, true);
      }
      if (got < info.count) return { more: true };
      return { value: Object.assign({}, info, { samples: samples }) };
    };
  }

  function isUnknownCmd(e) { return e instanceof RemoteError && e.status === STATUS.UNKNOWN_CMD; }

  // ---- link ------------------------------------------------------------------------------------

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  // Request/response over a transport {send(Uint8Array) -> Promise|void}; the owner calls feed(report)
  // for every IN report. One request is in flight at a time (the firmware answers one at a time, and
  // a screenshot owns the IN endpoint while it streams). Log text goes to onLog, a bootloader
  // frame to onBootloader. timeoutMs/streamTimeoutMs are wall-clock watchdogs; pass 0 to disable
  // one (functional host tests count work, not time, so a loaded host cannot flake them).
  function Link(transport, opts) {
    opts = opts || {};
    this.transport = transport;
    this.timeoutMs = opts.timeoutMs === undefined ? 1000 : opts.timeoutMs;
    this.streamTimeoutMs = opts.streamTimeoutMs === undefined ? 2000 : opts.streamTimeoutMs;
    this.busyPollMs = opts.busyPollMs || 100;
    this.screenshotBusyMs = opts.screenshotBusyMs === undefined ? 1500 : opts.screenshotBusyMs;   // retry Busy/abort this long
    this.seq = 0;
    this.onLog = null;
    this.onBootloader = null;
    this.pending = null;
    this.queue = Promise.resolve();
    if (transport.attach) transport.attach(this);        // a transport that answers out of band feeds the link itself
  }

  Link.prototype.feed = function (raw) {
    var kind = classify(raw);
    if (kind === "log") { if (this.onLog) this.onLog(logText(raw)); return; }
    if (kind === "bootloader") {
      if (this.onBootloader) this.onBootloader();
      this._finish(null, new BootloaderDetected());
      return;
    }
    if (kind !== "response" || !this.pending) return;
    var e = this.pending, rsp = parseResponse(raw);
    if (rsp.seq !== e.seq) return;                       // stale answer to an earlier request
    var r;
    try { r = e.handler(rsp); } catch (err) { this._finish(e, err); return; }
    if (!r) return;
    if (r.more) { this._arm(e, this.streamTimeoutMs, "stream stalled"); return; }
    this._finish(e, null, r.value);
  };

  Link.prototype.abort = function (err) { this._finish(null, err); };

  Link.prototype._arm = function (e, ms, msg) {
    clearTimeout(e.timer);
    if (!(ms > 0)) return;            // watchdog disabled (0): the caller counts work, not time
    var self = this;
    e.timer = setTimeout(function () { self._finish(e, new RemoteTimeout(msg)); }, ms);
  };

  Link.prototype._finish = function (e, err, value) {
    e = e || this.pending;
    if (!e || this.pending !== e) return;
    clearTimeout(e.timer);
    this.pending = null;
    if (err) e.reject(err); else e.resolve(value);
  };

  Link.prototype._once = function (cmd, payload, o) {
    var self = this;
    this.seq = (this.seq % 255) + 1;                     // 1..255, never 0, so a stale zero report cannot match
    var seq = this.seq;
    var handler = o.handler || function (rsp) {
      if (rsp.cmd !== cmd) return undefined;
      if (rsp.status !== STATUS.OK) throw new RemoteError(rsp.status, cmd, rsp.payload);
      return { value: rsp };
    };
    return new Promise(function (resolve, reject) {
      var e = { seq: seq, handler: handler, resolve: resolve, reject: reject, timer: 0 };
      self.pending = e;
      self._arm(e, o.timeoutMs !== undefined ? o.timeoutMs : self.timeoutMs, "no response");
      Promise.resolve().then(function () { return self.transport.send(packRequest(cmd, seq, payload)); })
        .catch(function (err) { self._finish(e, err); });
    });
  };

  // opts: timeoutMs, busyMs (retry Busy answers for this long), handler (multi-frame replies).
  Link.prototype.call = function (cmd, payload, opts) {
    var self = this, o = opts || {};
    var run = async function () {
      var t0 = Date.now();
      for (;;) {
        try { return await self._once(cmd, payload, o); }
        catch (e) {
          var again = e instanceof RemoteError && e.status === STATUS.BUSY && o.busyMs && Date.now() - t0 < o.busyMs;
          if (!again) throw e;
          await sleep(self.busyPollMs);
        }
      }
    };
    var p = this.queue.then(run);
    this.queue = p.catch(function () {});
    return p;
  };

  // ---- the K2 commands (mirrors cmd_* in k2_remote.py) ----------------------------------------------

  function K2Remote(link) { this.link = link; }

  K2Remote.prototype.ping = async function () { return decodePing((await this.link.call(CMD.PING)).payload); };
  K2Remote.prototype.state = async function () { return decodeState((await this.link.call(CMD.GET_STATE)).payload); };

  K2Remote.prototype.pdos = async function () {
    var r = decodePdo((await this.link.call(CMD.GET_PDOS, [0])).payload);
    var pdos = r.pdo ? [r.pdo] : [];
    for (var i = 1; i < r.head.num_pdos; i++) pdos.push(decodePdo((await this.link.call(CMD.GET_PDOS, [i])).payload).pdo);
    return Object.assign({}, r.head, { pdos: pdos });
  };

  // Part 2 (cable VDO detail) exists only on newer firmware: BadArg or a short answer means "absent".
  K2Remote.prototype.emarker = async function () {
    var p0 = (await this.link.call(CMD.GET_EMARKER, [0])).payload;
    var p1 = (await this.link.call(CMD.GET_EMARKER, [1])).payload;
    var p2 = null;
    try { p2 = (await this.link.call(CMD.GET_EMARKER, [2])).payload; decodeEmarkerDetail(p2); }
    catch (e) { if (!(e instanceof RemoteError || e instanceof FrameError)) throw e; p2 = null; }
    return decodeEmarker(p0, p1, p2);
  };

  // Saved-cable library (command 0x61); ids are stable sequence numbers. Mirrors cmd_emlib in k2_remote.py.
  var EM_ERRS = ["ok", "library full", "invalid", "not found", "flash error", "exists", "a live sink is attached", "tool / 5V handover busy"];
  function emOk(p, what) {
    need(p, 1, what);
    if (p[0]) {
      var e = new RemoteError(STATUS.BAD_ARG, CMD.EM_LIB, p);
      e.message = what + ": " + (EM_ERRS[p[0]] || p[0]);
      throw e;
    }
    return p;
  }
  function u16(p, i) { return p[i] | (p[i + 1] << 8); }
  function decodeEmGet(p) {
    emOk(p, "emarker get"); need(p, 31, "emarker get");
    var n = 3; while (n < 27 && p[n]) n++;
    return { seq: u16(p, 1), name: String.fromCharCode.apply(null, Array.prototype.slice.call(p, 3, n)),
             vid: u16(p, 27), pid: u16(p, 29) };
  }
  function decodeEmSpoof(p) {
    emOk(p, "emarker spoof"); need(p, 8, "emarker spoof");
    return { active: !!p[1], requests: u16(p, 2), replies: u16(p, 4), seq: u16(p, 6) };
  }
  K2Remote.prototype.emGet = async function (seq) {
    return decodeEmGet((await this.link.call(CMD.EM_LIB, [1, seq & 255, seq >> 8])).payload);
  };
  K2Remote.prototype.emList = async function () {
    var seqs = [], start = 0;
    for (;;) {
      var p = emOk((await this.link.call(CMD.EM_LIB, [0, start])).payload, "emarker list");
      need(p, 3, "emarker list"); need(p, 3 + 2 * p[2], "emarker list");
      for (var i = 0; i < p[2]; i++) seqs.push(u16(p, 3 + 2 * i));
      start += p[2];
      if (!p[2] || start >= p[1]) break;
    }
    var out = [];
    for (var k = 0; k < seqs.length; k++) out.push(await this.emGet(seqs[k]));
    return out;
  };
  K2Remote.prototype.emSave = async function () {
    var p = emOk((await this.link.call(CMD.EM_LIB, [2])).payload, "emarker save"); need(p, 4, "emarker save");
    return { seq: u16(p, 1), existed: !!p[3], cable: await this.emGet(u16(p, 1)) };
  };
  K2Remote.prototype.emDelete = async function (seq) {
    emOk((await this.link.call(CMD.EM_LIB, [3, seq & 255, seq >> 8, 1])).payload, "emarker delete");
  };
  // name: 1..24 characters of A-Z a-z 0-9 space - _ .
  K2Remote.prototype.emRename = async function (seq, name) {
    if (!/^[A-Za-z0-9 _.\-]{1,24}$/.test(name)) throw new RangeError("name is 1..24 characters of A-Z a-z 0-9 space - _ .");
    var b = [4, seq & 255, seq >> 8];
    for (var i = 0; i < name.length; i++) b.push(name.charCodeAt(i));
    emOk((await this.link.call(CMD.EM_LIB, b)).payload, "emarker rename");
    return this.emGet(seq);
  };
  // Charger report cards (command 0x62), read-only here; rename and delete live in k2_remote.py cards. A card's
  // payload grew with the formats (162 bytes in v4, 214 in v9, 250 in v10) and is read as 48 byte chunks
  // (include/k2/chargertest/report_card.hpp; tools/report_card.py and docs/site/r/card.js decode all of it).
  var CARD_STATES = ["Setup", "Verify 5V", "Negotiating", "Hold", "Return to 5V", "Complete", "Stopped", "UNSAFE: unplug source", "Detach charger", "Attach charger"];
  var CARD_ERRS = ["ok", "invalid", "not found", "flash error", "full", "a charger test is running"];
  var CARD_CHUNKS = 6;   // v10: 250 bytes / 48 = 5.2 -> 6 chunks (the summary below only reads the header, older firmwares send fewer)
  function cardsOk(p, what) {
    need(p, 1, what);
    if (p[0]) {
      var e = new RemoteError(STATUS.BAD_ARG, CMD.CARDS, p);
      e.message = what + ": " + (CARD_ERRS[p[0]] || p[0]);
      throw e;
    }
    return p;
  }
  function u32(p, i) { return (p[i] | (p[i + 1] << 8) | (p[i + 2] << 16) | (p[i + 3] << 24)) >>> 0; }
  // The header and the verdict counts of one card payload (what the dashboard lists).
  function decodeCardSummary(b) {
    if (b.length < 144 || b.length > 250 || b[29] >= CARD_STATES.length) throw new FrameError("card payload is not a valid report card: length=" + b.length);
    var n = 34; while (n < 56 && b[n]) n++;
    var cnt = { pass: 0, fail: 0, skip: 0 };
    for (var g = 0; g < 12; g++) { var v = b[56 + 4 * g] >> 6; if (v === 1) cnt.pass++; else if (v === 2) cnt.fail++; else if (v === 3) cnt.skip++; }
    return { no: u16(b, 8), name: String.fromCharCode.apply(null, Array.prototype.slice.call(b, 34, n)), fp: u32(b, 0),
             vid: u16(b, 4), pid: u16(b, 6), runs: u16(b, 10), pdos: b[27], state: CARD_STATES[b[29]],
             pass: cnt.pass, fail: cnt.fail, skip: cnt.skip };
  }
  K2Remote.prototype.cardsList = async function () {
    var p = cardsOk((await this.link.call(CMD.CARDS, [0])).payload, "cards list");
    need(p, 3, "cards list"); need(p, 3 + 2 * p[2], "cards list");
    var out = [];
    for (var i = 0; i < p[2]; i++) {
      var no = u16(p, 3 + 2 * i), raw = [];
      for (var k = 0; k < CARD_CHUNKS; k++) {
        var c = cardsOk((await this.link.call(CMD.CARDS, [1, no & 255, no >> 8, k])).payload, "cards get");
        need(c, 2, "cards get");
        raw = raw.concat(Array.prototype.slice.call(c, 2));
      }
      out.push(decodeCardSummary(raw));
    }
    return out;
  };
  // target: "status" | "off" | saved-cable id | "builtin:N" (a built-in cable, N 0..4: generic, EPR 50V, CUKTECH 6A, 3 = retired alias of 4, OnePlus)
  K2Remote.prototype.emSpoof = async function (target) {
    var bi = typeof target === "string" ? /^builtin:(\d+)$/.exec(target) : null;
    if (typeof target === "string" && target.indexOf("builtin:") === 0 && !(bi && +bi[1] <= 4))
      throw new RangeError("built-in cable id must be 0..4");
    var b = target === "status" ? [5, 0] : target === "off" ? [5, 2] : bi ? [5, 3, +bi[1]] : [5, 1, target & 255, target >> 8];
    return decodeEmSpoof((await this.link.call(CMD.EM_LIB, b)).payload);
  };

  // e-marker learn and the unmatched-request log (EM_LIB ops 6 and 7). Mirrors cmd_emlearn / cmd_emunmatched in k2_remote.py.
  var LEARN_OUTCOMES = ["none", "learned", "no-identity", "aborted"];
  var LEARN_STEPS = ["identity", "svids", "modes", "seed", "sweep"];
  var LEARN_POLICIES = ["ignore", "nak", "not-supported"];
  function words(p, at, n) { var o = []; for (var i = 0; i < n; i++) o.push(u32(p, at + 4 * i)); return o; }
  function decodeLearnSummary(p) {
    emOk(p, "emarker learn"); need(p, 13, "emarker learn");
    return { outcome: LEARN_OUTCOMES[p[1]] || String(p[1]), svids: p[2], probes: p[3], silent: p[4], truncated: !!(p[5] & 1),
             logFull: !!(p[5] & 2), entries: p[6], rows: p[7], unmatchedPolicy: LEARN_POLICIES[p[8]] || String(p[8]),
             vid: u16(p, 9), pid: u16(p, 11) };
  }
  function decodeLearnEntry(p) {
    emOk(p, "emarker learn entry"); need(p, 9, "emarker learn entry"); need(p, 9 + 4 * (p[4] + p[6]), "emarker learn entry");
    return { i: p[1], step: LEARN_STEPS[p[2]] || String(p[2]), specRev: p[3], got: !!p[5], replyHeader: u16(p, 7),
             request: words(p, 9, p[4]), reply: words(p, 9 + 4 * p[4], p[6]) };
  }
  function decodeUnmatched(p) {
    emOk(p, "emarker unmatched"); need(p, 4, "emarker unmatched");
    var o = { total: u16(p, 1), held: p[3] };
    if (p.length > 4) { need(p, 8 + 4 * p[5], "emarker unmatched"); o.sop = p[4] === 1 ? "SOP'" : p[4] === 2 ? "SOP''" : String(p[4]); o.header = u16(p, 6); o.words = words(p, 8, p[5]); }
    return o;
  }
  // seeds: [{svid, cmd, vdos: [..]}], at most 4. The K2 blocks for seconds while it runs.
  K2Remote.prototype.emLearn = async function (seeds, sweep, confirm) {
    seeds = seeds || [];
    if (seeds.length > 4) throw new RangeError("at most 4 seeds");
    var b = [6, 1, (sweep ? 1 : 0) | (confirm ? 2 : 0), seeds.length];
    seeds.forEach(function (s) {
      var v = s.vdos || [];
      if (!(s.svid >= 0 && s.svid <= 0xFFFF && s.cmd >= 0 && s.cmd <= 31 && v.length <= 2)) throw new RangeError("seed: SVID 16 bit, cmd 0..31, at most two VDOs");
      b.push(s.svid & 255, s.svid >> 8, s.cmd, v.length);
      v.forEach(function (w) { b.push(w & 255, (w >>> 8) & 255, (w >>> 16) & 255, (w >>> 24) & 255); });
    });
    var summary = decodeLearnSummary((await this.link.call(CMD.EM_LIB, b, { timeoutMs: 10000 })).payload);
    return this.emLearned(summary);
  };
  K2Remote.prototype.emLearned = async function (summary) {
    summary = summary || decodeLearnSummary((await this.link.call(CMD.EM_LIB, [6, 0])).payload);
    var entries = [];
    for (var i = 0; i < summary.entries; i++) entries.push(decodeLearnEntry((await this.link.call(CMD.EM_LIB, [6, 2, i])).payload));
    return { summary: summary, entries: entries };
  };
  K2Remote.prototype.emLearnSave = async function () {
    var p = emOk((await this.link.call(CMD.EM_LIB, [6, 3])).payload, "emarker learn save"); need(p, 4, "emarker learn save");
    return { seq: u16(p, 1), existed: !!p[3] };
  };
  K2Remote.prototype.emUnmatched = async function () {
    var t = decodeUnmatched((await this.link.call(CMD.EM_LIB, [7, 255])).payload), entries = [];
    for (var i = 0; i < t.held; i++) entries.push(decodeUnmatched((await this.link.call(CMD.EM_LIB, [7, i])).payload));
    t.entries = entries;
    return t;
  };

  K2Remote.prototype.adcRaw = async function () { return decodeAdcRaw((await this.link.call(CMD.ADC_RAW)).payload); };

  // code 0..7 = ADC sample time 1.5 .. 239.5 cycles. Resolves with the statistics and all samples.
  K2Remote.prototype.ripple = function (code) {
    if (!(code >= 0 && code <= 7)) return Promise.reject(new RangeError("ripple sample-time code must be 0..7"));
    return this.link.call(CMD.RIPPLE, [code], { handler: rippleHandler(CMD.RIPPLE), timeoutMs: 3000 });
  };

  // op: "status" (default) | "reset" | "passive" | "kelvin" (mode select). Never drives VBUS or the load.
  K2Remote.prototype.cableR = async function (op) {
    var code = op === "reset" ? 1 : op === "passive" ? 2 : op === "kelvin" ? 3 : 0;
    return decodeCableR((await this.link.call(CMD.CABLE_R, [code])).payload);
  };
  K2Remote.prototype.identity = async function () { return decodeIdentity((await this.link.call(CMD.GET_IDENTITY)).payload); };

  // Active PD. Every call below can change what the source supplies: above 5 V the caller must have
  // asked the user to confirm (confirm = true); the K2 answers NEED_CONFIRM (5) otherwise.
  K2Remote.prototype.pdRequest = async function (pos, o) {
    o = o || {};
    var mv = o.mv || 0, ma = o.ma || 0;
    await this.link.call(CMD.PD_REQUEST, [pos, o.confirm ? 1 : 0, mv & 255, mv >> 8, ma & 255, ma >> 8]);
    return this.pdStatus();
  };
  K2Remote.prototype.pdReset = async function () { await this.link.call(CMD.PD_RESET); return this.pdStatus(); };
  K2Remote.prototype.pdRelease = async function () { await this.link.call(CMD.PD_RELEASE); return this.pdStatus(); };
  K2Remote.prototype.pdAttach = async function (target_mv, target_ma) {
    target_mv = target_mv || 9000;
    target_ma = target_ma || 0;
    if (target_mv > 36000) target_mv = 36000;
    var payload = [target_mv & 255, target_mv >> 8];
    if (target_ma > 0) payload = payload.concat([target_ma & 255, target_ma >> 8]);
    return decodePdTrigger((await this.link.call(CMD.PD_ATTACH, payload)).payload);
  };
  K2Remote.prototype.match = async function () { return decodeMatch((await this.link.call(CMD.PD_MATCH)).payload); };
  K2Remote.prototype.pdStatus = async function () { return decodePdTrigger((await this.link.call(CMD.PD_TRIGGER_STATUS)).payload); };
  // op 0 = status, 1 = start, 2 = start with confirmation (a live sink is attached).
  K2Remote.prototype.identifyDevice = async function (op) {
    return decodeIdentifyProgress((await this.link.call(CMD.IDENTIFY_DEVICE, [op || 0])).payload);
  };
  // Starts the sequence, polls until it ends or timeoutMs passes, then reads the source identity.
  K2Remote.prototype.identifyRun = async function (o) {
    o = o || {};
    var prog = await this.identifyDevice(o.confirm ? 2 : 1), end = Date.now() + (o.timeoutMs || 8000);
    while ((prog.state === "running" || prog.state === "idle") && Date.now() < end) {
      await sleep(100);
      prog = await this.identifyDevice(0);
    }
    return { identify: prog, identity: decodeIdentity((await this.link.call(CMD.GET_IDENTITY)).payload) };
  };

  // Feeds one PD message to the receive path; sop 0 = SOP, 1 = SOP', 2 = SOP''; `objects` = data object bytes.
  K2Remote.prototype.debugPdMsg = async function (sop, header, objects) {
    var body = Array.from(objects || []);
    await this.link.call(CMD.DEBUG_PD_MSG, [sop, header & 0xFF, header >> 8].concat(body));
  };
  K2Remote.prototype.debugLink = async function (on) { await this.link.call(CMD.DEBUG_LINK, [on ? 1 : 0]); };

  // op: "status" | "reset" | "scan" {confirm} | "trigger" {proto, mv, confirm}. Mirrors cmd_fc; allowInline overrides the load guard.
  K2Remote.prototype.fc = async function (op, o) {
    o = o || {};
    var payload = [FC_OP[op]];
    if (FC_OP[op] === undefined) throw new RangeError("unknown fc op " + op);
    if (op === "scan") {
      if (!o.confirm) throw new RangeError("the scan raises the output to 9 V: confirm it");
      payload.push(FC_CONFIRM | (o.allowInline ? FC_ALLOW_INLINE : 0));
    } else if (op === "trigger") {
      var pi = FC_PROTOS.indexOf(o.proto);
      if (pi < 0) throw new RangeError("unknown fast-charge protocol " + o.proto);
      if (!(o.mv > 0 && o.mv <= 0xFFFF)) throw new RangeError("bad voltage " + o.mv);
      if (o.mv > FC_SAFE_MV && !o.confirm) throw new RangeError(o.mv + " mV is above 5 V: confirm it");
      payload.push(pi, o.mv & 0xFF, o.mv >> 8, (o.confirm ? FC_CONFIRM : 0) | (o.allowInline ? FC_ALLOW_INLINE : 0));
    }
    return decodeFc((await this.link.call(CMD.FC, payload, { timeoutMs: 3000 })).payload);
  };

  // Section probe: true when the firmware answers fn(), false on "unknown command", else rethrows.
  K2Remote.prototype.probe = async function (fn) {
    try { await fn(); return true; } catch (e) { if (isUnknownCmd(e)) return false; throw e; }
  };

  // A remote press is a logical key (plus = next/down, minus = previous/up), never remapped by the device's
  // rotation or Rotate buttons setting.
  K2Remote.prototype.button = async function (nameOrIndex, ms) {
    ms = ms || SHORT_PRESS_MS;
    var b = typeof nameOrIndex === "number" ? nameOrIndex : BUTTONS[nameOrIndex];
    if (b === undefined) throw new RangeError("unknown button " + nameOrIndex);
    if (!(ms >= 30 && ms <= 10000)) throw new RangeError("press duration must be 30..10000 ms");
    // A previous press answers Busy until it ends: wait up to one press time plus a margin.
    await this.link.call(CMD.BUTTON, [b, ms & 0xFF, ms >> 8], { busyMs: ms + BUSY_MARGIN_MS });
    return { button: nameOrIndex, ms: ms };
  };

  // confirm=true sends [id, 1]: the PD rescan of an inline link refuses with NEED_CONFIRM until then.
  K2Remote.prototype.action = async function (nameOrIndex, confirm) {
    var a = typeof nameOrIndex === "number" ? nameOrIndex : ACTIONS[nameOrIndex];
    if (a === undefined) throw new RangeError("unknown action " + nameOrIndex);
    await this.link.call(CMD.ACTION, confirm ? [a, 1] : [a], { timeoutMs: ACTION_TIMEOUT_MS });   // an e-marker scan blocks the firmware
  };

  K2Remote.prototype.setPage = async function (n) { await this.link.call(CMD.SET_PAGE, [n]); };
  K2Remote.prototype.debugPdos = async function (n) { await this.link.call(CMD.DEBUG_PDOS, [n]); };
  K2Remote.prototype.reboot = async function () { await this.link.call(CMD.REBOOT); };
  // Clears the app marker and resets into the bootloader. The caller must have asked the
  // user to confirm; the firmware additionally checks the magic payload.
  K2Remote.prototype.dfuEnter = async function () { await this.link.call(CMD.DFU_ENTER, DFU_MAGIC); };
  // Wipes the file system and the legacy stores, then the K2 enters DFU. The erase
  // blocks the firmware for seconds, so the answer is waited for up to 30 s. Resolves with the counts on
  // success; a failed erase or verify throws (the K2 then stays in the app, not in DFU).
  var WIPE_STATUS = ["Ok", "Refused", "Busy", "KeyFail", "ProgramError", "WriteProtect", "VerifyFailed"];
  function hex8(v) { return "0x" + ("00000000" + v.toString(16).toUpperCase()).slice(-8); }
  K2Remote.prototype.factoryWipe = async function () {
    var p = (await this.link.call(CMD.FACTORY_WIPE, FACTORY_WIPE_MAGIC, { timeoutMs: 30000 })).payload;
    need(p, 14, "factory wipe");
    var r = { err: p[0], sectors: u16(p, 1), erased: u16(p, 3), verified: u16(p, 5), failed: u16(p, 7),
              first_fail: u32(p, 9), status: name(WIPE_STATUS, p[13]) };
    if (r.err === 1) throw new Error("factory wipe: erase failed at " + hex8(r.first_fail) + " (" + r.status + ") after " +
      r.erased + " of " + r.sectors + " sectors; the K2 is NOT in DFU");
    if (r.err) throw new Error("factory wipe: " + r.failed + " sectors do not read blank (first " + hex8(r.first_fail) +
      "); the K2 is NOT in DFU");
    return r;
  };

  // log(true|false) switches the text log, log() queries it; returns the log_on state.
  K2Remote.prototype.log = async function (on) {
    var r = await this.link.call(CMD.LOG, on === undefined ? [] : [on ? 1 : 0]);
    return !!r.payload[0];
  };

  K2Remote.prototype.screenshot = function () {
    if (this.link.transport.screenshot) return this.link.transport.screenshot();
    return this.link.call(CMD.SCREENSHOT, [], { handler: screenshotHandler(CMD.SCREENSHOT), busyMs: this.link.screenshotBusyMs });
  };

  // ---- PD Black Box (command 0x70): the trigger-frozen captures of the FUSB302/PD trace ------------------
  // Records: len(1) type(1) ts_us(4 LE) payload; see include/k2/pd/pdtrace.hpp. ts_us is the K2's micros()
  // (wraps after about 71 minutes), only differences inside one capture mean anything.
  var BB = { LIST: 4, READ: 5, CLEAR: 6, FLAG_FILLING: 8, SLOTS: 3 };
  var BB_TRIGGERS = ["", "contract", "hard reset", "vbus dip", "detach"];
  var BB_TYPES = ["", "rx", "rx_bad", "tx", "tx_result", "reset", "cc", "reg", "status0", "irq", "link", "sink", "vbus", "poll", "mark"];
  var PD_CTRL = ["", "GoodCRC", "GotoMin", "Accept", "Reject", "Ping", "PS_RDY", "Get_Source_Cap", "Get_Sink_Cap", "DR_Swap", "PR_Swap",
    "VCONN_Swap", "Wait", "Soft_Reset", "Data_Reset", "Data_Reset_Complete", "Not_Supported", "Get_Source_Cap_Ext", "Get_Status", "FR_Swap",
    "Get_PPS_Status", "Get_Country_Codes", "Get_Sink_Cap_Ext"];
  var PD_DATA = ["", "Source_Capabilities", "Request", "BIST", "Sink_Capabilities", "Battery_Status", "Alert", "Get_Country_Info", "Enter_USB",
    "EPR_Request", "EPR_Mode", "Source_Info", "Revision"];
  var BB_RESETS = ["hard reset rx", "hard reset tx", "soft reset rx", "soft reset tx"];
  var BB_TX_RESULTS = ["sent", "no GoodCRC (retries)", "timeout"];
  function pdMessageName(hdr) {
    var type = hdr & 0x1F, n = (hdr >> 12) & 7;
    if (hdr & 0x8000) return "Ext" + type;
    if (n === 0) return PD_CTRL[type] || "Ctrl" + type;
    if (type === 15) return "Vendor_Defined";
    return PD_DATA[type] || "Data" + type;
  }
  function rd32(b, i) { return (b[i] | b[i + 1] << 8 | b[i + 2] << 16 | b[i + 3] << 24) >>> 0; }
  function hex(b) { return Array.prototype.map.call(b, function (x) { return (x < 16 ? "0" : "") + x.toString(16); }).join(""); }

  // List response: now_ms(4), then 3 x [trigger(1) len(2) age_ms(4)]. Empty slots are omitted.
  function decodeBbCaps(p) {
    if (p.length < 4 + BB.SLOTS * 7) throw new FrameError("black box capture list too short");
    var caps = [];
    for (var k = 0; k < BB.SLOTS; k++) {
      var o = 4 + k * 7, t = p[o];
      if (t) caps.push({ slot: k, trigger: BB_TRIGGERS[t] || String(t), bytes: p[o + 1] | p[o + 2] << 8, age_ms: rd32(p, o + 3) });
    }
    return { now_ms: rd32(p, 0), captures: caps };
  }
  // Capture read response: flags(1) next(4) slot_len(4) records.
  function decodeBbCapRead(p) {
    if (p.length < 9) throw new FrameError("black box capture read too short");
    return { filling: !!(p[0] & BB.FLAG_FILLING), next: rd32(p, 1), len: rd32(p, 5), data: p.slice(9) };
  }
  // Whole records -> events with t_us relative to the first one. A truncated tail is dropped.
  function parseBbRecords(b) {
    var out = [], i = 0, t0 = null;
    while (i + 6 <= b.length && b[i] >= 6 && i + b[i] <= b.length) {
      var len = b[i], type = b[i + 1], ts = rd32(b, i + 2), pl = b.slice(i + 6, i + len);
      if (t0 === null) t0 = ts;
      var e = { t_us: (ts - t0) >>> 0, type: BB_TYPES[type] || String(type), hex: hex(pl) };
      if (type === 1 || type === 3) {           // Rx: token hdr(2) objs crc(4); Tx: hdr(2) objs
        var h = type === 1 ? 1 : 0, hdr = pl.length >= h + 2 ? pl[h] | pl[h + 1] << 8 : 0;
        var end = type === 1 ? pl.length - 4 : pl.length, objs = [];
        for (var o = h + 2; o + 4 <= end; o += 4) objs.push(rd32(pl, o));
        e.kind = "msg"; e.dir = type === 1 ? "rx" : "tx"; e.token = type === 1 ? pl[0] : 0xE0; e.sop = type === 3 || (pl[0] & 0xE0) === 0xE0; e.header = hdr; e.name = pdMessageName(hdr); e.objects = objs; e.msg_id = (hdr >> 9) & 7;
      } else if (type === 12 && pl.length >= 4) {
        e.kind = "vbus"; e.mv = pl[0] | pl[1] << 8; e.ma = (pl[2] | pl[3] << 8) << 16 >> 16;
      } else if (type === 5 && pl.length >= 1) {
        e.kind = "reset"; e.name = BB_RESETS[pl[0]] || "reset " + pl[0];
      } else if (type === 6 && pl.length >= 2) {
        e.kind = "cc"; e.attach = pl[0] === 1; e.cc = pl[1]; e.name = pl[0] === 1 ? "attach CC" + pl[1] : "detach";
      } else if (type === 4 && pl.length >= 1) {
        e.kind = "tx_result"; e.name = "TX " + (BB_TX_RESULTS[pl[0]] || pl[0]);
      } else e.kind = "other";
      out.push(e);
      i += len;
    }
    return out;
  }
  function formatBbDelta(us) {
    if (us < 10000) return (us / 1000).toFixed(1) + " ms";
    if (us < 1000000) return Math.round(us / 1000) + " ms";
    return (us / 1e6).toFixed(2) + " s";
  }
  // Best power and PDO mix of a Source_Capabilities, as the firmware prints it: "67W 5PDO+2PPS".
  function bbFingerprint(objs) {
    var best = 0, fixed = 0, pps = 0, avs = 0;
    objs.forEach(function (o) {
      var t = o >>> 30, mw = 0;
      if (o === 0) return;
      if (t === 0) { mw = ((o >>> 10) & 0x3FF) * 50 * (o & 0x3FF) * 10 / 1000; fixed++; }
      else if (t === 1) { mw = (o & 0x3FF) * 250; fixed++; }
      else if (t === 2) { mw = ((o >>> 20) & 0x3FF) * 50 * (o & 0x3FF) * 10 / 1000; fixed++; }
      else {
        var k = (o >>> 28) & 3;
        if (k === 0) { mw = ((o >>> 17) & 0xFF) * 100 * (o & 0x7F) * 50 / 1000; pps++; }
        else if (k === 1) { mw = (o & 0xFF) * 1000; avs++; }
        else if (k === 2) { var v20 = (o & 0x3FF) !== 0; mw = (v20 ? 20000 : 15000) * (v20 ? o & 0x3FF : (o >>> 10) & 0x3FF) * 10 / 1000; avs++; }
      }
      mw = Math.floor(mw);
      if (mw > best) best = mw;
    });
    return Math.floor((best + 500) / 1000) + "W " + fixed + "PDO" + (pps ? "+" + pps + "PPS" : "") + (avs ? "+" + avs + "AVS" : "");
  }
  // Who was attached: VID:PID from a Discover Identity ACK on SOP, split by the sender's power role
  // (sink-role sender = the device, source-role sender = the charger); else the caps fingerprint.
  function bbIdentity(events) {
    var id = { key: "?" }, hx = function (v, n) { return ("0000" + v.toString(16).toUpperCase()).slice(-n); };
    events.forEach(function (e) {
      if (e.kind !== "msg" || !e.sop) return;
      var type = e.header & 0x1F, source = !!((e.header >> 8) & 1), o = e.objects;
      if (type === 1 && source && !id.fp) { id.fp = bbFingerprint(o); return; }
      if (type !== 15 || e.dir !== "rx" || o.length < 4) return;
      var v = o[0];
      if ((v >>> 16) !== 0xFF00 || !((v >>> 15) & 1) || ((v >>> 6) & 3) !== 1 || (v & 0x1F) !== 1) return;
      var s = hx(o[1] & 0xFFFF, 4) + ":" + hx(o[3] >>> 16, 4), f = source ? "src" : "dev";
      if (!id[f]) id[f] = s;
    });
    id.key = id.dev || id.src || id.fp || "?";
    return id;
  }
  // The link that was negotiated, from sniffed messages only (mirror of src/pd/pdtrace_link.cpp): Enter_USB,
  // Enter Mode ACKs for TBT3 (0x8087) and DisplayPort (0xFF01), the DP Configure pin assignment, the
  // e-marker cable (SOP') and the partner's USB capability. The K2 sees CC, not the data pairs.
  function bbLink(events) {
    var L = {}, tbtReq = null, dpPin = null, eudo = null, cable = null, peer = null, dpEnter = false;
    var speed = function (c) { return { 0: "USB2", 1: "5G", 2: "10G", 3: "40G", 4: "80G" }[c] || "?"; };
    events.forEach(function (e) {
      if (e.kind !== "msg" || !e.objects.length || (e.header >> 15) & 1) return;
      var type = e.header & 0x1F, o = e.objects, sop = (e.token & 0xE0) === 0xE0, sop1 = (e.token & 0xE0) === 0xC0;
      if (type === 8 && sop && !eudo) { eudo = o[0]; return; }
      if (type !== 15 || !((o[0] >>> 15) & 1)) return;
      var svid = o[0] >>> 16, ct = (o[0] >>> 6) & 3, cmd = o[0] & 0x1F;
      if (svid === 0xFF00 && cmd === 1 && ct === 1 && e.dir === "rx") {
        if (sop1 && o.length >= 5) {
          var amps = (o[4] >>> 5) & 3;
          cable = { emarker: true, active: ((o[1] >>> 27) & 7) === 4, speed: o[4] & 7, amps: amps === 2 ? 5 : amps === 1 ? 3 : 0 };
        } else if (sop && o.length >= 2) {
          var sink = !((e.header >> 8) & 1);
          if (!peer || (sink && !peer.sink)) peer = { sink: sink, host: !!((o[1] >>> 31) & 1), dev: !!((o[1] >>> 30) & 1) };
        }
        return;
      }
      if (!sop) return;
      if (cmd === 4 && svid === 0x8087) {
        if (ct === 0) tbtReq = { speed: o.length >= 2 ? (o[1] >>> 16) & 7 : 0, active: o.length >= 2 && !!((o[1] >>> 24) & 1) };
        else if (ct === 1 && tbtReq) L.tbtInfo = tbtReq;
      } else if (cmd === 4 && svid === 0xFF01) {
        if (ct === 1) dpEnter = true;
      } else if (cmd === 17 && svid === 0xFF01) {
        if (ct === 0 && o.length >= 2) {
          var pins = (o[1] >>> 8) & 0xFF; dpPin = null;
          for (var b = 0; b < 6; b++) if ((pins >>> b) & 1) { dpPin = String.fromCharCode(65 + b); break; }
        } else if (ct === 1 && dpPin) { dpEnter = true; L.dp = dpPin; }
      }
    });
    var out = {};
    if (eudo !== null) {
      var mode = (eudo >>> 28) & 7, sp = (eudo >>> 21) & 7, ctype = (eudo >>> 19) & 3, cur = (eudo >>> 17) & 3;
      out.usb = mode === 0 ? "USB2" : mode === 1 ? "USB3.2" + (sp === 1 ? " Gen1 5G" : sp === 2 ? " Gen2 10G" : "")
        : mode === 2 ? "USB4" + ({ 2: " Gen2 20G", 3: " Gen3 40G", 4: " Gen4 80G" }[sp] || "") : "USB?";
      if (!cable) cable = { emarker: false, active: ctype !== 0, speed: sp, amps: cur === 3 ? 5 : cur === 2 ? 3 : 0, ctype: ctype };
    }
    if (L.tbtInfo) out.tbt = "TBT3" + (L.tbtInfo.speed === 3 ? " 40G" : "") + (L.tbtInfo.active ? " act" : "");
    if (L.dp) out.dp = L.dp;
    if (cable) {
      out.cable = speed(cable.speed) + (cable.amps ? " " + cable.amps + "A" : "") +
        (cable.emarker ? (cable.active ? " act" : "") : ({ 1: " retimer", 2: " redriver", 3: " optical" }[cable.ctype] || ""));
    }
    if (peer) out.peer = peer.host && peer.dev ? "host+dev" : peer.host ? "host" : peer.dev ? "dev" : "none";
    var parts = [];
    if (out.usb) parts.push(out.usb);
    if (out.tbt) parts.push(out.tbt);
    if (out.dp) parts.push("DP alt " + out.dp); else if (dpEnter) parts.push("DP");
    if (out.cable) parts.push("cable " + out.cable);
    if (peer) parts.push(peer.host || peer.dev ? "partner USB " + out.peer : "partner no USB");
    out.text = parts.length ? parts.join(" | ") : "not seen";
    return out;
  }
  function bbJson(meta, events) {
    return JSON.stringify({ source: "open-k2 PD black box", slot: meta.slot, trigger: meta.trigger, age_ms_at_read: meta.age_ms, bytes: meta.bytes, identity: bbIdentity(events), link: bbLink(events), events: events }, null, 1);
  }

  K2Remote.prototype.blackboxCaptures = async function () { return decodeBbCaps((await this.link.call(CMD.BLACK_BOX, [BB.LIST])).payload); };
  // All records of one slot. -> { events, filling }
  K2Remote.prototype.blackboxCapture = async function (slot) {
    var chunks = [], off = 0, filling = false;
    for (var guard = 0; guard < 64; guard++) {
      var r = decodeBbCapRead((await this.link.call(CMD.BLACK_BOX, [BB.READ, slot, off & 255, off >> 8])).payload);
      chunks.push(r.data); filling = r.filling;
      if (r.next === off || r.next >= r.len) break;
      off = r.next;
    }
    var all = new Uint8Array(chunks.reduce(function (n, c) { return n + c.length; }, 0)), at = 0;
    chunks.forEach(function (c) { all.set(c, at); at += c.length; });
    return { events: parseBbRecords(all), filling: filling };
  };
  K2Remote.prototype.blackboxClearCaptures = async function () { await this.link.call(CMD.BLACK_BOX, [BB.CLEAR]); };

  K2Remote.prototype.chargerTest = async function (mode) {
    if (mode === undefined) mode = "status";
    if (mode !== "status" && mode !== "results") throw new RangeError("charger-test supports status / results only");
    var r;
    try { r = await this.link.call(CMD.CHARGER_TEST, [2]); }
    catch (e) {
      if (!(e instanceof RemoteError) || (e.status !== STATUS.BAD_LENGTH && e.status !== STATUS.BAD_ARG)) throw e;
      r = await this.link.call(CMD.CHARGER_TEST);
    }
    var meta = decodeChargerTest(r.payload), query = meta.schema === 2 ? [2] : [];
    if (meta.start || meta.count) throw new FrameError("charger-test metadata contains rows");
    var out = Object.assign({}, meta);
    delete out.start; delete out.count;
    if (mode === "status") return out;
    function key(p) {
      return JSON.stringify(Object.keys(p).filter(function (k) { return k !== "start" && k !== "count" && k !== "steps"; })
        .map(function (k) { return p[k]; }));
    }
    var expected = key(meta), steps = [];
    while (steps.length < meta.total) {
      var req = new Uint8Array(query.length + 9), v = dv(req);
      req.set(query); req[query.length] = steps.length;
      v.setUint32(query.length + 1, meta.run_id, true); v.setUint32(query.length + 5, meta.revision, true);
      var page = decodeChargerTest((await this.link.call(CMD.CHARGER_TEST, req)).payload);
      if (key(page) !== expected || page.start !== steps.length || page.count !== Math.min(query.length ? 1 : 2, meta.total - steps.length))
        throw new FrameError("charger-test results changed or page did not advance; retry results");
      steps.push.apply(steps, page.steps);
    }
    var end = decodeChargerTest((await this.link.call(CMD.CHARGER_TEST, query)).payload);
    if (key(end) !== expected || end.start || end.count) throw new FrameError("charger-test results changed during export; retry results");
    out.steps = steps;
    return out;
  };

  // ---- HTTP transport: the K2's own web server over USB networking ---------------------------------
  // Same Link, different wire: each request frame is POSTed as hex to /api/frame and the 64-byte answer
  // (JSON {"rsp": hex}) is fed back to the link. The server wants an X-K2 header on every POST (a form on
  // another site cannot set it) and confirm=1 on the calls that change VBUS; the page's own dialogs and
  // K2Remote's checks already gate those, the flag only says the call was deliberate.
  var FC_VBUS_OPS = [1, 2, 3];                           // reset, scan, trigger (0 = status is a read)

  function frameNeedsConfirm(f) {
    var cmd = f[2];
    if (cmd === CMD.PD_REQUEST || cmd === CMD.PD_RESET || cmd === CMD.PD_RELEASE) return true;
    return cmd === CMD.FC && f[4] > 0 && FC_VBUS_OPS.indexOf(f[5]) >= 0;
  }

  function hex(bytes) {
    var s = "";
    for (var i = 0; i < bytes.length; i++) s += (bytes[i] < 16 ? "0" : "") + bytes[i].toString(16);
    return s;
  }

  function unhex(s) {
    if (typeof s !== "string" || s.length % 2 || /[^0-9a-fA-F]/.test(s)) throw new FrameError("bad hex in response");
    var out = new Uint8Array(s.length / 2);
    for (var i = 0; i < out.length; i++) out[i] = parseInt(s.substr(2 * i, 2), 16);
    return out;
  }

  // /api/screenshot.bmp: 66-byte header (pixel offset at 10, width at 18, height at 22, negative = top-down),
  // then RGB565 little-endian pixels.
  function decodeBmp565(buf) {
    var v = new DataView(buf);
    if (buf.byteLength < 66 || v.getUint16(0, false) !== 0x424D) throw new FrameError("not a BMP");
    if (v.getUint16(28, true) !== 16) throw new FrameError("BMP is not 16 bit");
    var off = v.getUint32(10, true), w = v.getUint32(18, true), h = v.getInt32(22, true), top = h < 0;
    if (top) h = -h;
    if (buf.byteLength < off + w * h * 2) throw new FrameError("BMP pixel data truncated");
    var px = new Uint16Array(w * h), y, x;
    for (y = 0; y < h; y++) {
      var row = top ? y : h - 1 - y;
      for (x = 0; x < w; x++) px[row * w + x] = v.getUint16(off + (y * w + x) * 2, true);
    }
    return { width: w, height: h, rgba: rgb565ToRgba(px) };
  }

  function httpTransport(base, opts) {
    opts = opts || {};
    base = base || "";
    var f = opts.fetch || (typeof fetch !== "undefined" ? fetch.bind(globalThis) : null);
    var t = { link: null };
    function check(res) {
      if (!res.ok) throw new Error("HTTP " + res.status);
      return res;
    }
    t.attach = function (link) { t.link = link; };
    t.send = async function (frame) {
      if (!f) throw new Error("fetch is not available");
      var url = base + "/api/frame?hex=" + hex(frame) + (frameNeedsConfirm(frame) ? "&confirm=1" : "");
      var res = check(await f(url, { method: "POST", headers: { "X-K2": "1" } }));
      var body = await res.json();
      if (t.link) t.link.feed(unhex(body.rsp));
    };
    t.screenshot = async function () {
      if (!f) throw new Error("fetch is not available");
      return decodeBmp565(await check(await f(base + "/api/screenshot.bmp")).arrayBuffer());
    };
    return t;
  }

  return {
    FRAME_SIZE: FRAME_SIZE, CMD: CMD, DFU_MAGIC: DFU_MAGIC, FACTORY_WIPE_MAGIC: FACTORY_WIPE_MAGIC, STATUS: STATUS, BUTTONS: BUTTONS, ACTIONS: ACTIONS, PAGES: PAGES, MENUS: MENUS, pageName: pageName,
    SHORT_PRESS_MS: SHORT_PRESS_MS, LONG_PRESS_MS: LONG_PRESS_MS,
    FrameError: FrameError, RemoteError: RemoteError, RemoteTimeout: RemoteTimeout, BootloaderDetected: BootloaderDetected,
    packRequest: packRequest, parseResponse: parseResponse, isResponse: isResponse,
    isBootloaderFrame: isBootloaderFrame, classify: classify, logText: logText,
    directionOf: directionOf, decodePing: decodePing, decodeState: decodeState, decodePdo: decodePdo,
    decodeChargerTest: decodeChargerTest, TEST_ALGORITHMS: TEST_ALGORITHMS, TEST_REASONS: TEST_REASONS, TEST_STAGES: TEST_STAGES,
    decodeEmarker: decodeEmarker, decodeEmarkerDetail: decodeEmarkerDetail, decodeAdcRaw: decodeAdcRaw,
    decodeRippleInfo: decodeRippleInfo, decodeIdentity: decodeIdentity, decodePdTrigger: decodePdTrigger, decodeMatch: decodeMatch, decodeIdentifyProgress: decodeIdentifyProgress, PD_ERRS: PD_ERRS,
    decodeFc: decodeFc, decodeCableR: decodeCableR, parseLogStats: parseLogStats,
    formatAgo: formatAgo, formatUptime: formatUptime, isUnknownCmd: isUnknownCmd, FC_PROTOS: FC_PROTOS, FC_LEVELS: FC_LEVELS, FC_NAMES: FC_NAMES,
    ADC_CHANNELS: ADC_CHANNELS, ADC_SCAN: ADC_SCAN, RIPPLE_SAMPLES: RIPPLE_SAMPLES, RIPPLE_FULL_SCALE: RIPPLE_FULL_SCALE,
    FC_SAFE_MV: FC_SAFE_MV, rle565Decode: rle565Decode, decodeScreenInfo: decodeScreenInfo,
    rgb565ToRgba: rgb565ToRgba, httpTransport: httpTransport, decodeBmp565: decodeBmp565, Link: Link, K2Remote: K2Remote,
    decodeBbCaps: decodeBbCaps, decodeBbCapRead: decodeBbCapRead, parseBbRecords: parseBbRecords, pdMessageName: pdMessageName,
    formatBbDelta: formatBbDelta, bbJson: bbJson, bbIdentity: bbIdentity, bbLink: bbLink
  };
})();
if (typeof module !== "undefined" && module.exports) module.exports = K2Proto;
