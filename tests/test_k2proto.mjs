// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Node tests for tools/web/k2proto.js (the WebHID dashboard's protocol layer).
// Byte vectors are the same literals as tests/test_k2_remote.py and tests/test_remote_proto.cpp.
// Run: node --test tests/test_k2proto.mjs   (make test does it when node is installed)
import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const P = require("../tools/web/k2proto.js");

const u8 = (...a) => Uint8Array.from(a);
const pad64 = (bytes) => { const o = new Uint8Array(64); o.set(bytes); return o; };
const rsp = (cmd, seq, status, payload = []) =>
  pad64([0xA6, 0x4B, cmd, seq, status, payload.length, ...payload]);
const ascii = (s) => pad64(Array.from(s, (c) => c.charCodeAt(0)));
const BOOT = Uint8Array.from(Buffer.from(
  "ff55d4149b4814da020000000000000000000000000000000000000000000000" +
  "0000000000000000000000000000000000000000000000000000020f", "hex")
);  // k2_flasher.build_frame(2)
const LOG = ascii("[SYS] Heartbeat: up=2s presses=0 page=0\n");

// ---- framing ------------------------------------------------------------------------------

test("button request bytes", () => {
  const f = P.packRequest(P.CMD.BUTTON, 7, u8(2, 100, 0));
  assert.equal(f.length, 64);
  assert.deepEqual([...f.slice(0, 8)], [0xA5, 0x4B, 0x02, 0x07, 0x03, 2, 100, 0]);
  assert.ok(f.slice(8).every((b) => b === 0));
});

test("empty payload request", () => {
  assert.deepEqual([...P.packRequest(P.CMD.PING, 255).slice(0, 5)], [0xA5, 0x4B, 0x01, 0xFF, 0x00]);
});

test("oversize payload rejected, 59 bytes fit", () => {
  P.packRequest(P.CMD.PING, 0, new Uint8Array(59));
  assert.throws(() => P.packRequest(P.CMD.PING, 0, new Uint8Array(60)), RangeError);
});

test("seq wraps to a byte", () => {
  assert.equal(P.packRequest(P.CMD.PING, 256)[3], 0);
});

test("debug pdos request bytes", () => {
  assert.deepEqual([...P.packRequest(P.CMD.DEBUG_PDOS, 2, u8(7)).slice(0, 6)],
    [0xA5, 0x4B, 0x20, 0x02, 0x01, 7]);
});

test("bootloader request magic differs from the remote one", () => {
  assert.notDeepEqual([...P.packRequest(1, 1).slice(0, 2)], [...BOOT.slice(0, 2)]);
});

// ---- response parsing and the log/response split --------------------------------------------

test("parse valid response", () => {
  const r = P.parseResponse(rsp(2, 9, 0, [2, 100, 0]));
  assert.deepEqual([r.cmd, r.seq, r.status], [2, 9, 0]);
  assert.deepEqual([...r.payload], [2, 100, 0]);
});

test("error status keeps an empty payload", () => {
  const r = P.parseResponse(rsp(2, 3, 3));
  assert.equal(r.status, P.STATUS.BAD_ARG);
  assert.equal(r.payload.length, 0);
});

test("log text is not a response", () => {
  assert.equal(P.isResponse(LOG), false);
  assert.throws(() => P.parseResponse(LOG), P.FrameError);
});

test("wrong length and oversize payload length rejected", () => {
  assert.throws(() => P.parseResponse(rsp(1, 1, 0).slice(0, 10)), P.FrameError);
  const bad = rsp(1, 1, 0); bad[5] = 59;
  assert.throws(() => P.parseResponse(bad), P.FrameError);
});

test("bootloader frame is recognised", () => {
  assert.equal(P.isBootloaderFrame(BOOT), true);
  assert.equal(P.isResponse(BOOT), false);
  assert.equal(P.isBootloaderFrame(rsp(1, 1, 0)), false);
});

test("classify splits responses, bootloader frames, log text and idle reports", () => {
  assert.equal(P.classify(rsp(1, 1, 0)), "response");
  assert.equal(P.classify(BOOT), "bootloader");
  assert.equal(P.classify(LOG), "log");
  assert.equal(P.classify(new Uint8Array(64)), "idle");
  assert.equal(P.classify(new Uint8Array(0)), "idle");
});

test("logText cuts at the first NUL", () => {
  assert.equal(P.logText(LOG), "[SYS] Heartbeat: up=2s presses=0 page=0\n");
  assert.equal(P.logText(ascii("[BOOT] hello\n")), "[BOOT] hello\n");
});

// ---- decoders ---------------------------------------------------------------------------------

const cpad = (s, n) => { const o = new Uint8Array(n); o.set(Array.from(s, (c) => c.charCodeAt(0))); return o; };

test("decode ping", () => {
  const p = Uint8Array.from([1, 4, 3, 2, 1, ...cpad("abc1234", 24), ...cpad("1.0", 3)]);
  assert.deepEqual(P.decodePing(p),
    { proto: 1, uptime_ms: 0x01020304, git: "abc1234", version: "1.0" });
});

test("ping keeps a dirty hash", () => {
  const p = Uint8Array.from([1, 0, 0, 0, 0, ...cpad("cef22c4-dirty", 24), ...cpad("1.0", 3)]);
  assert.equal(P.decodePing(p).git, "cef22c4-dirty");
});

test("decode state with negative current", () => {
  const s = P.decodeState(u8(1, 2, 2, 4, 1, 2, 1, 1,
    0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0));
  assert.equal(s.page, 1);
  assert.equal(s.page_name, "detail");
  assert.equal(s.active_cc, 2);
  assert.equal(s.spec_rev, "PD3.x");
  assert.equal(s.num_pdos, 4);
  assert.equal(s.chip, "INA226");
  assert.equal(s.pm_online, true);
  assert.equal(s.log_on, true);
  assert.deepEqual([s.vbus_mv, s.current_ma, s.power_mw, s.energy_mwh], [20000, -1500, 74565, 7]);
  assert.equal(s.direction, "down");
});

test("direction", () => {
  assert.equal(P.directionOf(1500), "up");
  assert.equal(P.directionOf(-1500), "down");
  assert.equal(P.directionOf(0), "idle");
});

test("short payloads are FrameErrors", () => {
  assert.throws(() => P.decodeState(new Uint8Array(23)), P.FrameError);
  assert.throws(() => P.decodePing(new Uint8Array(28)), P.FrameError);
  assert.throws(() => P.decodePdo(new Uint8Array(3)), P.FrameError);
  assert.throws(() => P.decodeEmarker(new Uint8Array(16)), P.FrameError);
});

test("decode fixed pdo", () => {
  const { head, pdo } = P.decodePdo(u8(1, 2, 2, 0, 1, 0x2C, 0x91, 0x01, 0x00, 0x88, 0x13, 0x88, 0x13,
    0xB8, 0x0B, 0x98, 0x3A, 0, 0));
  assert.deepEqual(head, { valid: true, spec_rev: "PD3.x", num_pdos: 2, index: 0 });
  assert.deepEqual(pdo, { type: "fixed", raw: 0x0001912C, min_mv: 5000, max_mv: 5000,
    max_ma: 3000, max_mw: 15000 });
});

test("pdo header only", () => {
  const { head, pdo } = P.decodePdo(u8(0, 0, 0, 0));
  assert.equal(head.valid, false);
  assert.equal(pdo, null);
});

test("raw pdo word above 2^31 stays unsigned", () => {
  const { pdo } = P.decodePdo(u8(1, 2, 1, 0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0));
  assert.equal(pdo.raw, 0xFFFFFFFF);
});

test("decode emarker with vendor, speed and VDOs", () => {
  const part0 = Uint8Array.from([1, 1, 0xAC, 0x05, 0x34, 0x12, 0x44, 0x33, 0x22, 0x11, 2, 3, 5, 50, 240, 0, 2,
    ...Array.from("Apple\0USB4", (c) => c.charCodeAt(0))]);
  const part1 = u8(0xDD, 0xCC, 0xBB, 0xAA, 0x04, 0x03, 0x02, 0x01);
  const e = P.decodeEmarker(part0, part1);
  assert.equal(e.vid, 0x05AC);
  assert.equal(e.pid, 0x1234);
  assert.equal(e.xid, 0x11223344);
  assert.deepEqual([e.max_current_a, e.max_voltage_v, e.max_power_w], [5, 50, 240]);
  assert.deepEqual([e.vendor, e.speed], ["Apple", "USB4"]);
  assert.equal(e.active, true);
  assert.deepEqual(e.vdos, [0xAABBCCDD, 0x01020304]);
});

// ---- Charger Test: schema 2 evidence and read-only legacy compatibility ------------------------

const TEST_HEADER = u8(1, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0);
const TEST_HEADER2 = u8(2, 4, 3, 2, 1, 8, 7, 6, 5, 3, 0, 3, 1, 0, 0,
  0xFF, 0xFF, 7, 0, 0x10, 0, 4, 0, 0x12, 0x32, 0xFF,
  0xA0, 0x8C, 0x50, 0xC3, 0x88, 0x13, 0x17, 0x44, 0x33, 0x22, 0x11, 0x81, 0x71, 7,
  5, 35);   // ps_rdy_timing (out of band + late), ps_rdy_max (35 * 20 ms)
const TEST_ROWS = [
  u8(6, 0, 1, 0x20, 0x4E, 0, 0, 0x58, 0x4D, 2, 0, 0, 0, 0, 0, 0),
  u8(4, 6, 1, 0x28, 0x23, 0xE8, 3, 0, 0, 1, 0, 0, 0x40, 0x32, 0xDC, 0xC0),
  u8(8, 0, 0, 0xE0, 0x2E, 0, 0, 0, 0, 3, 11, 7, 0, 0, 0, 0)
];
const testPage = (meta, start, rows) =>
  u8(...meta.slice(0, 13), start, rows.length, ...meta.slice(15), ...rows.flatMap((r) => [...r]));
function chargerRig(payloads) {
  const sent = [];
  const remote = new P.K2Remote({ call: async (cmd, payload = []) => {
    assert.equal(cmd, 0x0B, "results must never call an active command");
    sent.push([cmd, [...payload]]);
    const p = payloads.shift();
    if (p instanceof Error) throw p;
    return { payload: u8(...p) };
  } });
  return { remote, sent };
}

test("charger-test EPR AVS is a programmable test", () => {
  const meta = new Uint8Array(TEST_HEADER2);
  new DataView(meta.buffer).setUint32(15, 1 << 19, true);
  const row = new Uint8Array(16), v = new DataView(row.buffer);
  row[0] = 19; row[1] = 8; row[2] = 2; row[9] = 2;
  v.setUint16(3, 28000, true); v.setUint16(5, 1000, true); v.setUint16(7, 28000, true);
  v.setUint32(12, 0xD0000000 | (280 << 17) | (150 << 8) | 100, true);
  const step = P.decodeChargerTest(testPage(meta, 0, [row])).steps[0];
  assert.deepEqual([step.algorithm, step.group, step.protocol, step.result, step.explanation],
                   ["epr-avs", "pd", "epr-avs", "pass", "none"]);
});
// The setup stage is a 4-bit wire field (header byte 23, bits 2..5): the firmware enum is the source of truth and
// k2_remote.py and the dashboard each keep a copy. Stage 9 and up (resume, changes, ask, refuse, waiting) once failed
// to decode in the dashboard because its table stopped at "detail".
test("charger-test stage table matches the firmware enum and k2_remote.py", () => {
  const hpp = readFileSync(path.join(here, "../include/k2/ui/ui_pages.hpp"), "utf8");
  const fw = hpp.match(/enum class Stage : uint8_t \{ (Algorithms, Group[^}]*)\}/);
  assert.ok(fw, "ChargerTestPage::Stage enum not found in include/k2/ui/ui_pages.hpp");
  const firmware = fw[1].split(",").map((s) => s.trim().toLowerCase());
  const py = readFileSync(path.join(here, "../tools/k2_remote.py"), "utf8")
    .match(/^TEST_STAGES = \[([^\]]*)\]/m);
  assert.ok(py, "TEST_STAGES not found in tools/k2_remote.py");
  const remote = Array.from(py[1].matchAll(/"([^"]+)"/g), (m) => m[1]);
  assert.deepEqual(P.TEST_STAGES, firmware);
  assert.deepEqual(remote, firmware);
});
test("charger-test every firmware stage decodes", () => {
  P.TEST_STAGES.forEach((name, stage) => {
    const meta = new Uint8Array(TEST_HEADER2);
    meta[23] = (stage << 2) | 2;   // the wire revision bits stay PD3.x
    const r = P.decodeChargerTest(testPage(meta, 0, []));
    assert.equal(r.stage, name);
  });
});
test("charger-test exact schema2 selection and retained evidence", () => {
  const r = P.decodeChargerTest(TEST_HEADER2);
  assert.equal(r.schema, 2); assert.equal(r.stage, "review");
  assert.equal(r.selection.run_mask, 0x7FFFF);
  assert.equal(r.selection.run.length, 19);
  assert.deepEqual(r.selection.configured, ["pps", "pd3.2-revision"]);
  assert.deepEqual(r.evidence, {
    origin: "run-start", source_session: 0x11223344, raw_header: 0x7181, pdo_count: 7,
    wire_revision_code: 2, wire_revision: "PD3.x",
    source_rmdo: { revision: "3.2", version: "1.7", provenance: "source self-report" },
    advertised: { pps: true, epr_table: true, spr_avs: true, epr_capable: true, partial_epr: true },
    monitor_mv: 36000, physical_cable: { observed: true, max_mv: 50000, max_ma: 5000 },
    spoof_on: true, independent_revision_tested: false,
    ps_rdy: { out_of_band: true, inferred: false, late: true, worst_ms: 700 },
    pass_scope: "negotiation and measured voltage only; not rated-current/power or revision conformance"
  });
  const empty = new Uint8Array(42); empty[0] = 2;
  const e = P.decodeChargerTest(empty);
  assert.deepEqual(e.evidence.ps_rdy, { out_of_band: false, inferred: false, late: false, worst_ms: 0 });
  const quirky = new Uint8Array(empty); quirky[40] = 2; quirky[41] = 255;
  assert.deepEqual(P.decodeChargerTest(quirky).evidence.ps_rdy, { out_of_band: false, inferred: true, late: false, worst_ms: 5100 });
  const badFlag = new Uint8Array(empty); badFlag[40] = 8;
  assert.throws(() => P.decodeChargerTest(badFlag), P.FrameError);
  assert.equal(e.evidence.origin, "no-run"); assert.equal(e.evidence.source_rmdo, null);
  assert.deepEqual(e.selection.run, []); assert.deepEqual(e.steps, []);
});

// Firmware up to v1.0-400 sends a 40-byte schema-2 header (no ps_rdy timing bytes); the decoder tells the shapes
// apart by length (40/56 old, 42/58 new) and reports ps_rdy as null for the old one.
test("charger-test old 40-byte schema2 header still decodes", () => {
  const old = TEST_HEADER2.slice(0, 40);
  const r = P.decodeChargerTest(old);
  assert.equal(r.schema, 2); assert.equal(r.stage, "review");
  assert.equal(r.evidence.pdo_count, 7); assert.equal(r.evidence.monitor_mv, 36000);
  assert.equal(r.evidence.ps_rdy, null);
  const paged = P.decodeChargerTest(testPage(old, 0, [TEST_ROWS[0]]));
  const fresh = P.decodeChargerTest(testPage(TEST_HEADER2, 0, [TEST_ROWS[0]]));
  assert.equal(paged.count, 1);
  assert.deepEqual(paged.steps, fresh.steps);
  assert.equal(fresh.evidence.ps_rdy.worst_ms, 700);
  assert.throws(() => P.decodeChargerTest(TEST_HEADER2.slice(0, 41)), P.FrameError);
});

test("charger-test schema2 pages, error detail keeps algorithm identity", async () => {
  const { remote, sent } = chargerRig([TEST_HEADER2, ...TEST_ROWS.map((r, i) => testPage(TEST_HEADER2, i, [r])), TEST_HEADER2]);
  const r = await remote.chargerTest("results");
  assert.deepEqual(r.steps.map((s) => s.algorithm), ["qc3", "pps", "afc"]);
  assert.deepEqual(r.steps.map((s) => s.group), ["qc3", "pd", "afc"]);
  assert.deepEqual(r.steps.map((s) => s.protocol), ["qc3", "pps", "afc"]);
  assert.equal(r.steps[2].detail_reason, "no response");
  assert.equal(r.steps[2].explanation, "request failed");
  assert.deepEqual(sent, [[0x0B, [2]], ...[0, 1, 2].map((i) => [0x0B, [2, i, 4, 3, 2, 1, 8, 7, 6, 5]]), [0x0B, [2]]]);
  for (const code of [11, 12, 13]) {
    const row = new Uint8Array(TEST_ROWS[0]); row[0] = code;
    assert.match(P.decodeChargerTest(testPage(TEST_HEADER2, 0, [row])).steps[0].protocol, /-5v-handshake$/);
  }
});

// 0x0B req [3] (the charger-test view): the same bytes as tests/test_k2_remote_charger_test.py VIEW.
const TEST_VIEW = [4, 2, 1, 0x03, 0x30, 0xDC, 0x05, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 2, 0, 0, 0, 7, 0, 3, 0, 3, 0x84, 0x32,
  ...[..."Press again to start"].map((c) => c.charCodeAt(0))];
test("charger-test view decodes the screen as data", async () => {
  const v = P.decodeChargerTestView(u8(...TEST_VIEW));
  assert.deepEqual([v.stage, v.cursor, v.rows, v.on_screen, v.confirming, v.confirm_left_ms], ["review", 2, 1, true, true, 1500]);
  assert.equal(v.message, "Press again to start");
  assert.deepEqual(v.preview, { runnable: 9, skipped: 2, refusal: "none", refusal_mv: 0 });
  assert.deepEqual(v.sweep_points, { pps: 7, avs: 3 });
  assert.deepEqual(v.guard, { topology: "direct", run_guard: 0x84 });
  assert.deepEqual(v.wait, { elapsed_ms: 0, total_ms: 0, retry: false, paused: false });
  assert.equal(v.rmdo, "3.2"); assert.equal(v.armed_is_pick, true); assert.equal(v.cable_armed, false);
  assert.deepEqual(v.resume, { pending: false, auto: false, gate_ready: false, held: false, behind_cable: false, rd: false });
  const w = new Uint8Array(TEST_VIEW.slice(0, 30));
  w[0] = 13; new DataView(w.buffer).setUint16(3, 0x0C09, true); new DataView(w.buffer).setUint16(7, 4200, true);
  new DataView(w.buffer).setUint16(9, 15000, true); w[20] = 8; new DataView(w.buffer).setUint16(21, 8470, true);
  const d = P.decodeChargerTestView(w);
  assert.equal(d.stage, "waiting"); assert.deepEqual(d.wait, { elapsed_ms: 4200, total_ms: 15000, retry: true, paused: true });
  assert.equal(d.preview.refusal, "load attached"); assert.equal(d.preview.refusal_mv, 8470); assert.equal(d.self_supplied, true);
  assert.equal(d.message, "");
  assert.throws(() => P.decodeChargerTestView(u8(...TEST_VIEW.slice(0, 29))), P.FrameError);
  assert.throws(() => P.decodeChargerTestView(u8(...TEST_VIEW, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41)), P.FrameError);
  const badStage = new Uint8Array(TEST_VIEW); badStage[0] = 99;
  assert.throws(() => P.decodeChargerTestView(badStage), P.FrameError);
  const { remote, sent } = chargerRig([TEST_VIEW]);
  assert.equal((await remote.chargerTest("view")).stage, "review");
  assert.deepEqual(sent, [[0x0B, [3]]]);
});

test("charger-test view on older firmware reports the missing form", async () => {
  for (const status of [P.STATUS.BAD_ARG, P.STATUS.BAD_LENGTH]) {
    const { remote } = chargerRig([new P.RemoteError(status, 0x0B)]);
    await assert.rejects(() => remote.chargerTest("view"), (e) => e instanceof P.FrameError && /older than/.test(e.message));
  }
  const { remote } = chargerRig([new P.RemoteError(P.STATUS.BUSY, 0x0B)]);
  await assert.rejects(() => remote.chargerTest("view"), (e) => e instanceof P.RemoteError);
});

test("charger-test unavailable revisions and protocols only SKIP with concrete reasons", async () => {
  const meta = new Uint8Array(TEST_HEADER2); meta[11] = meta[12] = 5; meta[9] = 5;
  const rows = [16, 17, 18, 14, 15].map((a) => u8(a, 0, 0, 0, 0, 0, 0, 0, 0, 4, a >= 16 ? 20 : 4, 0, 0, 0, 0, 0));
  const { remote } = chargerRig([meta, ...rows.map((r, i) => testPage(meta, i, [r])), meta]);
  const r = await remote.chargerTest("results");
  assert.deepEqual(r.steps.map((s) => s.result), Array(5).fill("skip"));
  assert.deepEqual(r.steps.map((s) => s.explanation), ["No independent PD3.0 test", "No independent PD3.1 test",
    "No independent PD3.2 test", "not implemented", "not implemented"]);
  for (const row of rows) {
    const bad = new Uint8Array(row); bad[9] = 2; bad[10] = 0;
    assert.throws(() => P.decodeChargerTest(testPage(meta, 0, [bad])), P.FrameError);
    bad[9] = 4; bad[10] = row[10] === 20 ? 4 : 20;
    assert.throws(() => P.decodeChargerTest(testPage(meta, 0, [bad])), P.FrameError);
  }
  const wrongWire = new Uint8Array(TEST_ROWS[0]); wrongWire[0] = 0;
  assert.throws(() => P.decodeChargerTest(testPage(meta, 0, [wrongWire])), P.FrameError);
});

test("charger-test schema2: every EPR skip names its reason (mirrors ct::explanation)", () => {
  const meta = new Uint8Array(TEST_HEADER2); meta[15] = 4; meta[16] = 0; meta[17] = 8; meta[18] = 0;   // mask: EPR fixed (2) and EPR AVS (19)
  const cases = [[2, 8, 22, 0, "EPR: cable not 50V 5A"], [2, 9, 3, 144, "EPR: above K2's 36V monitor"],
    [2, 9, 3, 145, "EPR: above K2's 48V monitor"], [19, 10, 3, 144, "EPR: above K2's 36V monitor"],
    [2, 0, 27, 139, "EPR: refused (RDO)"], [2, 0, 27, 137, "EPR: source rejects cable"],
    [19, 0, 27, 0, "EPR: entry refused"], [2, 0, 28, 0, "EPR: no PD contract"], [2, 0, 2, 0, "EPR: not offered"]];
  for (const [code, pos, why, detail, text] of cases) {
    const row = u8(code, pos, 0, 0x90, 0x8C, 0, 0, 0, 0, 4, why, detail, 0, 0, 0, 0);
    const step = P.decodeChargerTest(testPage(meta, 0, [row])).steps[0];
    assert.equal(step.explanation, text, JSON.stringify([code, pos, why, detail]));
    if (detail >= 136) assert.equal(step.detail_reason, text);
  }
});

test("charger-test schema2: draft bit 31 is the PPS deep sweep flag, not a test", () => {
  const h = new Uint8Array(TEST_HEADER2);
  const plain = P.decodeChargerTest(h).selection;
  h[22] |= 0x80;   // remote_proto.hpp: draft_selected bit 31 = the PPS deep sweep on
  const sel = P.decodeChargerTest(h).selection;
  assert.equal(sel.pps_sweep, true);
  assert.equal(plain.pps_sweep, false);
  assert.equal(sel.configured_mask, plain.configured_mask);
  assert.deepEqual(sel.configured, plain.configured);
});

test("charger-test schema2: draft bit 30 is the AVS deep sweep flag, not a test", () => {
  const h = new Uint8Array(TEST_HEADER2);
  const plain = P.decodeChargerTest(h).selection;
  h[22] |= 0x40;   // remote_proto.hpp: draft_selected bit 30 = the AVS deep sweep on
  const sel = P.decodeChargerTest(h).selection;
  assert.equal(sel.avs_sweep, true);
  assert.equal(plain.avs_sweep, false);
  assert.equal(sel.configured_mask, plain.configured_mask);
  assert.deepEqual(sel.configured, plain.configured);
});

test("charger-test sweep row: flags bit 5, the detail byte is the recorded point count", () => {
  // remote_proto.hpp: a deep-sweep row sets flags bit 5; its detail byte is then the point count, not the
  // in-RAM kDetailSweep marker 161. 61 points must read as 61.
  const sweepRow = u8(4, 6, 0x20, 0x88, 0x13, 0x88, 0x13, 0xC2, 0x51, 2, 0, 61, 0x40, 0x32, 0xDC, 0xC0);
  const step = P.decodeChargerTest(testPage(TEST_HEADER2, 0, [sweepRow])).steps[0];
  assert.equal(step.sweep, true);
  assert.equal(step.detail, 61);
  assert.equal(step.detail_reason, "deep sweep (61 points)");
  const allFlags = u8(4, 6, 0x3F, 0x88, 0x13, 0x88, 0x13, 0xC2, 0x51, 2, 0, 61, 0x40, 0x32, 0xDC, 0xC0);
  assert.equal(P.decodeChargerTest(testPage(TEST_HEADER2, 0, [allFlags])).steps[0].sweep, true);
  const bit6 = u8(4, 6, 0x40, 0x88, 0x13, 0x88, 0x13, 0xC2, 0x51, 2, 0, 61, 0x40, 0x32, 0xDC, 0xC0);
  assert.throws(() => P.decodeChargerTest(testPage(TEST_HEADER2, 0, [bit6])), /invalid charger-test row/);
  // older firmware: no bit 5, the kDetailSweep marker 161 in the detail byte
  const oldRow = u8(0, 6, 0x00, 0x88, 0x13, 0x88, 0x13, 0xC2, 0x51, 2, 0, 161, 0x40, 0x32, 0xDC, 0xC0);
  const legacy = P.decodeChargerTest(testPage(TEST_HEADER, 0, [oldRow])).steps[0];
  assert.equal(legacy.sweep, false);
  assert.equal(legacy.detail_reason, "deep sweep");
});

test("charger-test schema2 rejects malformed shapes, unknown masks and unselected rows", () => {
  const good = testPage(TEST_HEADER2, 0, [TEST_ROWS[0]]);
  for (const [at, value] of [[0, 3], [11, 107], [23, 0x42], [39, 12], [18, 0x80], [22, 0x20],
                            [40, 8], [42, 20], [43, 12], [44, 0x80], [51, 5], [52, 47]]) {   // 47 = one past the last reason (46 = StepResolution); [22, 0x20] = draft bit 29, below the sweep flags
    const bad = new Uint8Array(good); bad[at] = value;
    assert.throws(() => P.decodeChargerTest(bad), P.FrameError, "offset " + at);
  }
  const unselected = new Uint8Array(good); unselected[15] &= ~(1 << 6);
  assert.throws(() => P.decodeChargerTest(unselected), P.FrameError);
  for (const bad of [TEST_HEADER2.slice(0, 39), u8(...TEST_HEADER2, 0), u8(...good, ...TEST_ROWS[0]), good.slice(0, -1)])
    assert.throws(() => P.decodeChargerTest(bad), P.FrameError);
});

test("charger-test reason 44 is 'needs load': a PPS row skipped with the collapse voltage decodes", () => {
  // pattern pps-off-at-no-load (splitter run 2026-11-10): Reason::NeedsLoad (#44) is a Skip, not a source-lost fail
  const row = new Uint8Array(TEST_ROWS[1]);
  row[7] = 1800 & 0xFF; row[8] = 1800 >> 8;   // measured 1.8 V
  row[9] = 4; row[10] = 44;                   // skip, why 44
  const step = P.decodeChargerTest(testPage(TEST_HEADER2, 0, [row])).steps[0];
  assert.equal(step.result, "skip"); assert.equal(step.reason, "needs load");
  assert.equal(step.explanation, "needs load"); assert.equal(step.measured_mv, 1800);
});

test("charger-test reasons 45/46 (PPS coverage): floor and step resolution decode", () => {
  // ROADMAP "PPS coverage": 45 = Floored (a low edge the source cannot reach), 46 = StepResolution (a +20 mV PPS
  // step not followed; the detail byte is the measured move in mV)
  assert.equal(P.TEST_REASONS[45], "floored above target");
  assert.equal(P.TEST_REASONS[46], "step resolution");
  const row = new Uint8Array(TEST_ROWS[1]);
  row[3] = 3300 & 0xFF; row[4] = 3300 >> 8;         // target 3.3 V
  row[7] = 5000 & 0xFF; row[8] = 5000 >> 8;          // measured 5.0 V (the floor)
  row[9] = 3; row[10] = 45;                          // fail, why 45
  let step = P.decodeChargerTest(testPage(TEST_HEADER2, 0, [row])).steps[0];
  assert.equal(step.result, "fail"); assert.equal(step.reason, "floored above target");
  assert.equal(step.explanation, "floored above target"); assert.equal(step.measured_mv, 5000);
  row[10] = 46; row[11] = 100;                       // why 46, detail 100 mV
  step = P.decodeChargerTest(testPage(TEST_HEADER2, 0, [row])).steps[0];
  assert.equal(step.reason, "step resolution"); assert.equal(step.explanation, "step resolution 100mV");
  assert.equal(step.detail, 100);
});

test("charger-test evidence, draft and final metadata races abort rather than mix snapshots", async () => {
  for (const at of [15, 19, 23, 24, 25, 26, 28, 30, 32, 33, 37, 39]) {
    const bad = testPage(TEST_HEADER2, 0, [TEST_ROWS[0]]); bad[at] ^= 1;
    const { remote } = chargerRig([TEST_HEADER2, bad]);
    await assert.rejects(remote.chargerTest("results"), P.FrameError, "offset " + at);
  }
  const end = new Uint8Array(TEST_HEADER2); end[19] ^= 1;
  const { remote } = chargerRig([TEST_HEADER2, ...TEST_ROWS.map((r, i) => testPage(TEST_HEADER2, i, [r])), end]);
  await assert.rejects(remote.chargerTest("results"), P.FrameError);
});

test("charger-test schema2 maximum and empty retrieval are bounded", async () => {
  const meta = new Uint8Array(TEST_HEADER2); meta[11] = meta[12] = 106;
  const { remote, sent } = chargerRig([meta, ...Array.from({ length: 106 }, (_, i) => testPage(meta, i, [TEST_ROWS[0]])), meta]);
  assert.equal((await remote.chargerTest("results")).steps.length, 106);
  assert.equal(sent.length, 108);
  const empty = new Uint8Array(42); empty[0] = 2;
  const e = chargerRig([empty, empty]);
  assert.deepEqual((await e.remote.chargerTest("results")).steps, []);
  assert.equal(e.sent.length, 2);
});

test("charger-test schema1 fallback preserves layout and labels missing evidence honestly", async () => {
  for (const status of [P.STATUS.BAD_LENGTH, P.STATUS.BAD_ARG]) {
    const { remote, sent } = chargerRig([new P.RemoteError(status, 0x0B), TEST_HEADER]);
    const r = await remote.chargerTest("status");
    assert.equal(r.schema, 1); assert.equal(r.selection, null); assert.equal(r.stage, null); assert.equal(r.evidence, null);
    assert.deepEqual(sent, [[0x0B, [2]], [0x0B, []]]);
  }
  const legacyRows = TEST_ROWS.map((r, i) => u8([2, 0, 4][i], ...r.slice(1)));
  const { remote, sent } = chargerRig([TEST_HEADER, testPage(TEST_HEADER, 0, legacyRows.slice(0, 2)),
    testPage(TEST_HEADER, 2, legacyRows.slice(2)), TEST_HEADER]);
  const r = await remote.chargerTest("results");
  assert.deepEqual(r.steps.map((s) => s.protocol), ["qc3", "pd-pps", "afc"]);
  assert.equal(r.evidence, null);
  assert.deepEqual(sent, [[0x0B, [2]], [0x0B, [0, 4, 3, 2, 1, 8, 7, 6, 5]],
    [0x0B, [2, 4, 3, 2, 1, 8, 7, 6, 5]], [0x0B, []]]);
});

test("charger-test never falls back on busy, timeout or corrupt data; no active operations", async () => {
  for (const err of [new P.RemoteError(P.STATUS.BUSY, 0x0B), new P.RemoteTimeout(), u8(1, 2)]) {
    const { remote, sent } = chargerRig([err]);
    await assert.rejects(remote.chargerTest("status"));
    assert.equal(sent.length, 1);
  }
  const { remote, sent } = chargerRig([]);
  for (const op of ["start", "run", "reset", "select", "spoof"])
    await assert.rejects(remote.chargerTest(op), RangeError);
  assert.equal(sent.length, 0);
});

// ---- RLE565 and screenshots -------------------------------------------------------------------

test("rle565 runs and literals", () => {
  assert.deepEqual([...P.rle565Decode(u8(0x02, 0x34, 0x12, 0x81, 0x01, 0x00, 0x02, 0x00), 5)],
    [0x1234, 0x1234, 0x1234, 1, 2]);
});

test("rle565 matches the firmware vector (100 equal pixels)", () => {
  const px = P.rle565Decode(u8(99, 5, 0), 100);
  assert.equal(px.length, 100);
  assert.ok(px.every((v) => v === 5));
});

test("rle565 rejects truncated tokens and wrong counts", () => {
  assert.throws(() => P.rle565Decode(u8(0x05, 0x34), 6), P.FrameError);
  assert.throws(() => P.rle565Decode(u8(0x82, 1, 0, 2, 0), 3), P.FrameError);
  assert.throws(() => P.rle565Decode(u8(0x00, 1, 0), 2), P.FrameError);
});

test("screen info", () => {
  assert.deepEqual(P.decodeScreenInfo(u8(240, 0, 240, 0, 16, 15, 1)),
    { width: 240, height: 240, strip_rows: 16, num_strips: 15 });
  assert.throws(() => P.decodeScreenInfo(u8(240, 0, 240, 0, 16, 14, 1)), P.FrameError);
  assert.throws(() => P.decodeScreenInfo(u8(240, 0, 240, 0, 16, 15, 2)), P.FrameError);
});

test("rgb565 to rgba expansion", () => {
  assert.deepEqual([...P.rgb565ToRgba([0xFFFF, 0x0000, 0xF800, 0x07E0, 0x001F])],
    [255, 255, 255, 255, 0, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255]);
});

// ---- link: seq matching, log skipping, bootloader, timeout, busy retry -------------------------

// A transport whose replies are scripted per request: replies(req) -> array of frames to feed.
// Functional rig: every reply is scripted, so the tests count work, not time -- the wall-clock watchdogs
// are off (timeoutMs/streamTimeoutMs 0) and cannot fire on a loaded host; the timeout tests below pass
// their own small values on purpose (they test the watchdog itself).
function rig(replies, opts = {}) {
  const sent = [];
  const logs = [];
  let boot = 0;
  const link = new P.Link({
    send: async (f) => {
      sent.push(f);
      const out = replies(f, sent.length) || [];
      for (const r of out) setImmediate(() => link.feed(r));
    },
  }, { timeoutMs: 0, streamTimeoutMs: 0, busyPollMs: 5, screenshotBusyMs: 20, ...opts });
  link.onLog = (t) => logs.push(t);
  link.onBootloader = () => { boot++; };
  return { link, sent, logs, boots: () => boot };
}

test("link skips log text and stale responses", async () => {
  const { link, sent, logs } = rig((f) => [LOG, rsp(1, 99, 0), LOG, rsp(1, f[3], 0, [1])]);
  const r = await link.call(P.CMD.PING);
  assert.equal(r.seq, 1);
  assert.deepEqual([...sent[0].slice(0, 5)], [0xA5, 0x4B, 0x01, 0x01, 0x00]);
  assert.equal(logs.length, 2);
  assert.equal(logs[0], "[SYS] Heartbeat: up=2s presses=0 page=0\n");
});

test("link seq increments and never hits 0", async () => {
  const { link, sent } = rig((f) => [rsp(1, f[3], 0)]);
  await link.call(P.CMD.PING);
  await link.call(P.CMD.PING);
  assert.deepEqual(sent.map((f) => f[3]), [1, 2]);
  link.seq = 255;
  await link.call(P.CMD.PING);
  assert.equal(sent[2][3], 1);
});

test("a response to another command with the same seq is ignored", async () => {
  const { link } = rig((f) => [rsp(3, f[3], 0, [9]), rsp(1, f[3], 0, [1])]);
  const r = await link.call(P.CMD.PING);
  assert.equal(r.cmd, 1);
});

test("bootloader reply rejects the call and fires onBootloader", async () => {
  const { link, boots } = rig(() => [BOOT]);
  await assert.rejects(link.call(P.CMD.PING), P.BootloaderDetected);
  assert.equal(boots(), 1);
});

test("timeout", async () => {
  const { link } = rig(() => [], { timeoutMs: 30 });
  await assert.rejects(link.call(P.CMD.PING), P.RemoteTimeout);
});

test("error status rejects with RemoteError", async () => {
  const { link } = rig((f) => [rsp(2, f[3], 4)]);
  await assert.rejects(link.call(P.CMD.BUTTON, u8(1, 100, 0)),
    (e) => e instanceof P.RemoteError && e.status === P.STATUS.BUSY);
});

test("busy is retried within the limit, then succeeds", async () => {
  let n = 0;
  const { link, sent } = rig((f) => [rsp(2, f[3], ++n < 3 ? 4 : 0)]);
  // the retry budget is wall-clock by design and covered by the give-up test below; this functional test
  // keeps a budget no host load can exhaust, so it cannot flake under `make gate`
  await link.call(P.CMD.BUTTON, u8(1, 100, 0), { busyMs: 60000 });
  assert.equal(sent.length, 3);
  assert.deepEqual(sent.map((f) => f[3]), [1, 2, 3]);
});

test("busy gives up after the limit", async () => {
  const { link } = rig((f) => [rsp(2, f[3], 4)]);
  await assert.rejects(link.call(P.CMD.BUTTON, u8(1, 100, 0), { busyMs: 20 }),
    (e) => e instanceof P.RemoteError && e.status === P.STATUS.BUSY);
});

test("calls are serialised: the second request waits for the first answer", async () => {
  const order = [];
  const { link } = rig((f) => { order.push(`send${f[3]}`); return [rsp(1, f[3], 0)]; });
  await Promise.all([link.call(P.CMD.PING).then(() => order.push("done1")),
    link.call(P.CMD.PING).then(() => order.push("done2"))]);
  assert.deepEqual(order, ["send1", "done1", "send2", "done2"]);
});

test("feeding frames without a pending call only routes log text", () => {
  const { link, logs } = rig(() => []);
  link.feed(LOG);
  link.feed(new Uint8Array(64));
  link.feed(rsp(1, 5, 0));
  assert.equal(logs.length, 1);
});

test("unsolicited bootloader frame fires onBootloader once per frame", () => {
  const { link, boots } = rig(() => []);
  link.feed(BOOT);
  assert.equal(boots(), 1);
});

// ---- the K2 commands over the link --------------------------------------------------------------

const STATE_PAYLOAD = [1, 2, 2, 4, 1, 2, 1, 1, 0x20, 0x4E, 0, 0, 0x24, 0xFA, 0xFF, 0xFF, 0x45, 0x23, 0x01, 0, 7, 0, 0, 0];

test("K2Remote.state / ping / setPage / debugPdos / button / action / log request bytes", async () => {
  const { link, sent } = rig((f) => [rsp(f[2], f[3], 0, f[2] === 3 ? STATE_PAYLOAD : f[2] === 9 ? [1] : [])]);
  const k = new P.K2Remote(link);
  assert.equal((await k.state()).vbus_mv, 20000);
  await k.setPage(2);
  await k.debugPdos(7);
  await k.button("plus", 700);
  await k.action("energy-reset");
  assert.equal(await k.log(false), true);
  assert.deepEqual([...sent[1].slice(0, 6)], [0xA5, 0x4B, 7, 2, 1, 2]);
  assert.deepEqual([...sent[2].slice(0, 6)], [0xA5, 0x4B, 0x20, 3, 1, 7]);
  assert.deepEqual([...sent[3].slice(0, 8)], [0xA5, 0x4B, 2, 4, 3, 3, 0xBC, 0x02]);   // plus, 700 ms
  assert.deepEqual([...sent[4].slice(0, 6)], [0xA5, 0x4B, 6, 5, 1, 3]);
  assert.deepEqual([...sent[5].slice(0, 6)], [0xA5, 0x4B, 9, 6, 1, 0]);
});

// GET_STATE [1]: the same 31-byte vector as tests/test_remote_proto.cpp and tests/test_k2_remote_state_parts.py.
const SESSION_PAYLOAD = [0x02, 0x01, 0, 0, 0x04, 0x03, 0, 0, 1, 0x88, 0x13, 0x20, 0x4E, 0xB8, 0x0B,
                         4, 1, 3, 2, 1, 0x54, 0x0B, 1, 10, 0xFF, 0xFF, 0x05, 3, 7, 2, 0x14];

test("decodeSession reads every field of GET_STATE part 1", () => {
  const s = P.decodeSession(Uint8Array.from(SESSION_PAYLOAD));
  assert.equal(s.mah, 258);
  assert.equal(s.elapsed_s, 772);
  assert.deepEqual(s.proto, { kind: "pd-fixed", limit_ma: 5000, max_mv: 20000 });
  assert.equal(s.typec_ma, 3000);
  assert.equal(s.busy, "pd-hold");
  assert.equal(s.fc_release, true);
  assert.equal(s.topology, "direct-receptacle");
  assert.equal(s.wait_kind, "charger-waiting");
  assert.equal(s.pd_none_why, "sink-silent-source-offered");
  assert.equal(s.open_cc_mv, 2900);
  assert.deepEqual(s.ui, { stack: ["connection"], tab: "cable", popup: true, details: false, sel: 3, count: 7 });
  assert.equal(s.home_window, "10m");
  assert.deepEqual(Object.keys(s.flags).filter((k) => s.flags[k]), ["em_scanning", "em_quiet"]);
});

test("decodeSession: sentinels are null, the tab is for Connection only, short payloads throw", () => {
  const p = Uint8Array.from(SESSION_PAYLOAD);
  p[20] = 0xFF; p[21] = 0xFF; p[27] = 0xFF; p[23] = 1;           // open CC unknown, no cursor, stack [emarker]
  const s = P.decodeSession(p);
  assert.equal(s.open_cc_mv, null);
  assert.equal(s.ui.sel, null);
  assert.equal(s.ui.tab, null);
  assert.throws(() => P.decodeSession(Uint8Array.from(SESSION_PAYLOAD.slice(0, 30))), P.FrameError);
});

test("K2Remote.stateSession sends the part byte; old firmware reads as null", async () => {
  const a = rig((f) => [rsp(3, f[3], 0, SESSION_PAYLOAD)]);
  assert.equal((await new P.K2Remote(a.link).stateSession()).mah, 258);
  assert.deepEqual([...a.sent[0].slice(0, 6)], [0xA5, 0x4B, 3, 1, 1, 1]);
  for (const status of [P.STATUS.BAD_LENGTH, P.STATUS.BAD_ARG]) {
    const b = rig((f) => [rsp(3, f[3], status)]);
    assert.equal(await new P.K2Remote(b.link).stateSession(), null);
  }
  const c = rig((f) => [rsp(3, f[3], P.STATUS.BUSY)]);
  await assert.rejects(new P.K2Remote(c.link).stateSession(), P.RemoteError);
});

test("request ids are unique and match the firmware table", () => {
  const ids = Object.values(P.CMD);
  assert.equal(new Set(ids).size, ids.length);
  assert.equal(P.CMD.DEBUG_FAULT, 0x31);
  assert.equal(P.CMD.FC, 0x32);
});

test("DFU_ENTER sends the confirm magic and nothing else", async () => {
  assert.equal(P.CMD.DFU_ENTER, 0x30);
  assert.deepEqual([...P.DFU_MAGIC], [0x44, 0x46, 0x55, 0x21]);   // "DFU!", u32 0x21554644 little endian
  const { link, sent } = rig((f) => [rsp(f[2], f[3], 0)]);
  await new P.K2Remote(link).dfuEnter();
  assert.equal(sent.length, 1);
  assert.deepEqual([...sent[0].slice(0, 9)], [0xA5, 0x4B, 0x30, 1, 4, 0x44, 0x46, 0x55, 0x21]);
});

test("FACTORY_WIPE sends the WIPE magic, waits long, decodes the reply and raises on a failure", async () => {
  assert.equal(P.CMD.FACTORY_WIPE, 0x33);
  assert.deepEqual([...P.FACTORY_WIPE_MAGIC], [0x57, 0x49, 0x50, 0x45]);   // "WIPE", u32 0x45504957 little endian
  const ok = [0, 152, 0, 152, 0, 152, 0, 0, 0, 0, 0, 0, 0, 0];
  const { link, sent } = rig((f) => [rsp(f[2], f[3], 0, ok)]);
  const r = await new P.K2Remote(link).factoryWipe();
  assert.deepEqual([...sent[0].slice(0, 9)], [0xA5, 0x4B, 0x33, 1, 4, 0x57, 0x49, 0x50, 0x45]);
  assert.deepEqual([r.err, r.sectors, r.erased, r.verified, r.failed, r.first_fail], [0, 152, 152, 152, 0, 0]);
  const bad = [1, 152, 0, 10, 0, 0, 0, 0, 0, 0x00, 0x80, 0x0B, 0x08, 6];   // erase failed at 0x080B8000
  const rig2 = rig((f) => [rsp(f[2], f[3], 0, bad)]);
  await assert.rejects(new P.K2Remote(rig2.link).factoryWipe(), /erase failed at 0x080B8000/);
  const short = rig((f) => [rsp(f[2], f[3], 0, [0, 1, 2])]);
  await assert.rejects(new P.K2Remote(short.link).factoryWipe());
});

test("button validates the press time like the CLI (30..10000 ms)", async () => {
  const { link } = rig((f) => [rsp(2, f[3], 0)]);
  const k = new P.K2Remote(link);
  await assert.rejects(k.button("ok", 29), RangeError);
  await assert.rejects(k.button("ok", 10001), RangeError);
  await assert.rejects(k.button("nope", 100), RangeError);
});

test("pdos asks every index until num_pdos", async () => {
  const pdo = (i) => [1, 2, 2, i, 1, 0x2C, 0x91, 0x01, 0, 0x88, 0x13, 0x88, 0x13, 0xB8, 0x0B, 0x98, 0x3A, 0, 0];
  const { link, sent } = rig((f) => [rsp(4, f[3], 0, pdo(f[5]))]);
  const r = await new P.K2Remote(link).pdos();
  assert.equal(r.pdos.length, 2);
  assert.deepEqual(sent.map((f) => f[5]), [0, 1]);
});

test("emarker reads part 0 and part 1 (part 2 is optional)", async () => {
  const part0 = [1, 1, 0xAC, 0x05, 0x34, 0x12, 0, 0, 0, 0, 2, 3, 5, 50, 240, 0, 1, ...Array.from("Apple\0USB4", (c) => c.charCodeAt(0))];
  const { link, sent } = rig((f) => [f[5] === 2 ? rsp(5, f[3], 3) : rsp(5, f[3], 0, f[5] === 0 ? part0 : [4, 3, 2, 1])]);
  const e = await new P.K2Remote(link).emarker();
  assert.deepEqual(sent.map((f) => f[5]), [0, 1, 2]);   // the core firmware answers BadArg for part 2
  assert.equal(e.detail, null);
  assert.deepEqual(e.vdos, [0x01020304]);
});

// ---- screenshot over the link --------------------------------------------------------------------

function runs(px) {                       // reference encoder: runs only
  const out = [];
  for (let i = 0; i < px.length;) {
    let j = i;
    while (j < px.length && px[j] === px[i] && j - i < 128) j++;
    out.push(j - i - 1, px[i] & 0xFF, px[i] >> 8);
    i = j;
  }
  return out;
}

function screenFrames(seq, screen, { noise = true, abortAt = -1 } = {}) {
  const W = 240, ROWS = 16;
  const frames = [rsp(10, seq, 0, [240, 0, 240, 0, 16, 15, 1])];
  for (let s = 0; s < 15; s++) {
    const enc = runs(screen.slice(s * W * ROWS, (s + 1) * W * ROWS));
    for (let i = 0; i < enc.length; i += 56) {
      const last = i + 56 >= enc.length;
      frames.push(rsp(0x8A, seq, 0, [s, last ? 1 : 0, ...enc.slice(i, i + 56)]));
      if (noise) frames.push(ascii("[LOG] noise\n"));
    }
    if (s === abortAt) frames.push(rsp(0x8A, seq, 0, [s + 1, 0x80]));
  }
  return frames;
}

test("screenshot: chunked strips with log noise decode to the right pixels", async () => {
  const screen = Array.from({ length: 240 * 240 }, (_, i) => ((i * 37) % 7) * 0x1111);
  const { link, sent, logs } = rig((f) => screenFrames(f[3], screen));
  const shot = await new P.K2Remote(link).screenshot();
  assert.deepEqual([sent[0][2], sent[0][4]], [10, 0]);
  assert.deepEqual([shot.width, shot.height], [240, 240]);
  assert.equal(shot.rgba.length, 240 * 240 * 4);
  const want = P.rgb565ToRgba(screen);
  assert.ok(Buffer.from(shot.rgba).equals(Buffer.from(want)), "RGBA differs from the source screen");
  assert.ok(logs.length > 0);
});

test("screenshot: whole strips arriving in reverse order still assemble", async () => {
  const screen = Array.from({ length: 240 * 240 }, (_, i) => (i >> 8) * 0x0101);
  const { link } = rig((f) => {
    const [head, ...rest] = screenFrames(f[3], screen, { noise: false });
    const groups = [[]];
    for (const fr of rest) { groups.at(-1).push(fr); if (fr[7] & 1) groups.push([]); }
    return [head, ...groups.reverse().flat()];
  });
  const shot = await new P.K2Remote(link).screenshot();
  assert.ok(Buffer.from(shot.rgba).equals(Buffer.from(P.rgb565ToRgba(screen))));
});

test("screenshot: abort frame raises busy", async () => {
  const screen = new Array(240 * 240).fill(0);
  const { link } = rig((f) => screenFrames(f[3], screen, { noise: false, abortAt: 2 }));
  await assert.rejects(new P.K2Remote(link).screenshot(),
    (e) => e instanceof P.RemoteError && e.status === P.STATUS.BUSY);
});

test("screenshot: a stalled stream times out", async () => {
  const screen = new Array(240 * 240).fill(0);
  const { link } = rig((f) => screenFrames(f[3], screen).slice(0, 5), { streamTimeoutMs: 30 });
  await assert.rejects(new P.K2Remote(link).screenshot(), P.RemoteTimeout);
});

// ---- the inline copy in k2.html must equal k2proto.js ----------------------------------------------

// ---- state with the analog front end and the link flags ---------------------------------------------

const STATE40 = [...STATE_PAYLOAD,
  0x10, 0x0E, 0x20, 0x03, 0x2C, 0x01, 0x90, 0x01, 0xE8, 0x03,    // dp 3600, dm 800, cc1 300, cc2 400, aux 1000
  0xF1, 0x00, 21, 0x0B,                                          // temp 24.1 C, fc 21 (PD), flags 0b1011
  0xC8, 0x01];                                                   // mcu 45.6 C
const LINK49 = [...STATE40, 0x05, 0x3C, 0, 0, 0, 0x0A, 0, 0, 0];  // stale + inline, age 60 s, cable age 10 s

test("state: the 24-byte form has no analog fields", () => {
  const s = P.decodeState(u8(...STATE_PAYLOAD));
  assert.equal(s.dp_mv, undefined);
  assert.equal(s.adc_valid, false);
});

test("state: 40-byte form carries analog lines, temperatures and the fast-charge verdict", () => {
  const s = P.decodeState(u8(...STATE40));
  assert.deepEqual([s.dp_mv, s.dm_mv, s.cc1_mv, s.cc2_mv, s.aux_mv], [3600, 800, 300, 400, 1000]);
  assert.equal(s.temp_c, 24.1);
  assert.equal(s.mcu_temp_c, 45.6);
  assert.equal(s.fc, "PD");
  assert.equal(s.fc_code, 21);
  assert.deepEqual([s.adc_valid, s.temp_valid, s.temp_calibrated, s.mcu_valid], [true, true, false, true]);
  assert.equal(s.link, null);
});

test("state: a 38-byte payload (no MCU temperature) still decodes", () => {
  const s = P.decodeState(u8(...STATE40.slice(0, 38)));
  assert.equal(s.cc2_mv, 400);
  assert.equal(s.mcu_temp_c, undefined);
  assert.equal(s.mcu_valid, false);
});

test("state: 49-byte form adds the link flags and ages", () => {
  const s = P.decodeState(u8(...LINK49));
  assert.deepEqual(s.link, { stale: true, waiting: false, inline: true, cable_stale: false,
                             cable_scanned: false, age_s: 60, cable_age_s: 10 });
});

test("state: unknown fast-charge code is named by number", () => {
  const b = [...STATE40]; b[36] = 99;
  assert.equal(P.decodeState(u8(...b)).fc, "99");
});

// ---- ADC raw -----------------------------------------------------------------------------------------

const ADC_PAYLOAD = [1,
  ...[0, 1, 2, 3, 4, 5, 6, 7, 16, 17].flatMap((c) => [c * 100 & 0xFF, c * 100 >> 8]),
  ...[1, 2, 3, 4, 5, 6, 1500].flatMap((v) => [v & 0xFF, v >> 8])];

test("adc raw: 35 bytes decode to channels and scan slots", () => {
  const r = P.decodeAdcRaw(u8(...ADC_PAYLOAD));
  assert.equal(r.ok, true);
  assert.deepEqual(Object.keys(r.channels).map(Number), [0, 1, 2, 3, 4, 5, 6, 7, 16, 17]);
  assert.equal(r.channels[16], 1600);
  assert.deepEqual(r.scan, { aux: 1, cc1: 2, cc2: 3, dp: 4, dm: 5, ntc: 6, vref: 1500 });
  assert.throws(() => P.decodeAdcRaw(u8(...ADC_PAYLOAD.slice(0, 34))), P.FrameError);
});

test("adc raw over the link: no payload, command 0x21", async () => {
  const { link, sent } = rig((f) => [rsp(f[2], f[3], 0, ADC_PAYLOAD)]);
  const r = await new P.K2Remote(link).adcRaw();
  assert.deepEqual([...sent[0].slice(0, 5)], [0xA5, 0x4B, 0x21, 1, 0]);
  assert.equal(r.scan.vref, 1500);
});

// ---- ripple -----------------------------------------------------------------------------------------

const rippleInfo = (n, code = 3) => [code, 0x10, 0x27, 0, 0, n & 0xFF, n >> 8, 5, 0, 0xF0, 0x0F, 0x00, 0x08];
const rippleFrame = (seq, off, vals) =>
  rsp(0x8B, seq, 0, [off & 0xFF, off >> 8, ...vals.flatMap((v) => [v & 0xFF, v >> 8])]);

test("ripple info decode", () => {
  const i = P.decodeRippleInfo(u8(...rippleInfo(512)));
  assert.deepEqual(i, { code: 3, period_ns: 10000, count: 512, min: 5, max: 4080, mean: 2048 });
  assert.throws(() => P.decodeRippleInfo(u8(1, 2, 3)), P.FrameError);
});

test("ripple: request carries the sample-time code, frames assemble in any order with log noise", async () => {
  const samples = Array.from({ length: 70 }, (_, i) => i * 50);
  const { link, sent } = rig((f) => {
    const frames = [rsp(0x22, f[3], 0, rippleInfo(70))];
    const chunks = [];
    for (let o = 0; o < 70; o += 28) chunks.push(rippleFrame(f[3], o, samples.slice(o, o + 28)));
    return [...frames, chunks[2], ascii("[LOG] x\n"), chunks[0], chunks[1], chunks[1]];   // reordered + duplicate
  });
  const r = await new P.K2Remote(link).ripple(5);
  assert.deepEqual([...sent[0].slice(0, 6)], [0xA5, 0x4B, 0x22, 1, 1, 5]);
  assert.equal(r.count, 70);
  assert.deepEqual(r.samples, samples);
});

test("ripple: Busy answer rejects without frames", async () => {
  const { link } = rig((f) => [rsp(0x22, f[3], 4)]);
  await assert.rejects(new P.K2Remote(link).ripple(0), (e) => e instanceof P.RemoteError && e.status === P.STATUS.BUSY);
});

test("ripple: a missing frame times out instead of hanging", async () => {
  const { link } = rig((f) => [rsp(0x22, f[3], 0, rippleInfo(40)), rippleFrame(f[3], 0, new Array(28).fill(1))],
    { streamTimeoutMs: 40 });
  await assert.rejects(new P.K2Remote(link).ripple(0), P.RemoteTimeout);
});

test("ripple: rejects a bad code before sending", async () => {
  const { link, sent } = rig(() => []);
  await assert.rejects(new P.K2Remote(link).ripple(8), RangeError);
  assert.equal(sent.length, 0);
});

test("ripple: a frame beyond the count is a frame error", async () => {
  const { link } = rig((f) => [rsp(0x22, f[3], 0, rippleInfo(4)), rippleFrame(f[3], 2, [1, 2, 3, 4])]);
  await assert.rejects(new P.K2Remote(link).ripple(0), P.FrameError);
});

// ---- device (DFU, confirm, probes) ------------------------------------------------------------------

test("NeedConfirm status is named and exposed", () => {
  assert.equal(P.STATUS.NEED_CONFIRM, 5);
  assert.match(new P.RemoteError(5, 6).message, /confirm/);
});

test("action: no confirm byte by default, [id, 1] when confirmed", async () => {
  const { link, sent } = rig((f) => [rsp(6, f[3], 0, [f[5]])]);
  const k = new P.K2Remote(link);
  await k.action("pd-rescan");
  await k.action("pd-rescan", true);
  assert.deepEqual([...sent[0].slice(0, 6)], [0xA5, 0x4B, 6, 1, 1, 1]);
  assert.deepEqual([...sent[1].slice(0, 7)], [0xA5, 0x4B, 6, 2, 2, 1, 1]);
});

test("action surfaces the inline-guard refusal as RemoteError NEED_CONFIRM", async () => {
  const { link } = rig((f) => [rsp(6, f[3], 5)]);
  await assert.rejects(new P.K2Remote(link).action("pd-rescan"),
    (e) => e instanceof P.RemoteError && e.status === P.STATUS.NEED_CONFIRM);
});

test("isUnknownCmd tells a missing command from other failures", () => {
  assert.equal(P.isUnknownCmd(new P.RemoteError(1, 0x24)), true);
  assert.equal(P.isUnknownCmd(new P.RemoteError(4, 0x24)), false);
  assert.equal(P.isUnknownCmd(new P.RemoteTimeout()), false);
});

test("probe: true when answered, false on UnknownCmd, other errors propagate", async () => {
  const k = new P.K2Remote(rig((f) => [rsp(f[2], f[3], 1)]).link);
  assert.equal(await k.probe(() => k.identity()), false);
  const ok = new P.K2Remote(rig((f) => [rsp(f[2], f[3], 0, IDENT_PAYLOAD)]).link);
  assert.equal(await ok.probe(() => ok.identity()), true);
  const bad = new P.K2Remote(rig((f) => [rsp(f[2], f[3], 4)]).link);
  await assert.rejects(bad.probe(() => bad.identity()), P.RemoteError);
});

// ---- identity (k2/pd-read, cmd 0x24) ----------------------------------------------------------------

const IDENT_PAYLOAD = [0x05, 1, 0xAC, 0x05, 0x02, 0x17, 0x34, 0x12, 2, 0xAC, 0x05, 0x01, 0xFF,
  ...Array.from("Apple\0Charger 30W", (c) => c.charCodeAt(0))];

test("identity decode: flags, ids, SVIDs, vendor and model", () => {
  const r = P.decodeIdentity(u8(...IDENT_PAYLOAD));
  assert.equal(r.valid, true);
  assert.deepEqual([r.epr, r.epr_partial, r.cuktech, r.xiaomi], [true, false, true, false]);
  assert.deepEqual([r.vid, r.pid, r.bcd], [0x05AC, 0x1702, 0x1234]);
  assert.deepEqual(r.svids, [0x05AC, 0xFF01]);
  assert.deepEqual([r.vendor, r.model], ["Apple", "Charger 30W"]);
  assert.throws(() => P.decodeIdentity(u8(1, 2, 3)), P.FrameError);
  assert.throws(() => P.decodeIdentity(u8(0, 1, 0, 0, 0, 0, 0, 0, 3, 1, 2)), P.FrameError);   // 3 SVIDs announced
});

test("identity request: command 0x24 without payload", async () => {
  const { link, sent } = rig((f) => [rsp(f[2], f[3], 0, IDENT_PAYLOAD)]);
  await new P.K2Remote(link).identity();
  assert.deepEqual([...sent[0].slice(0, 5)], [0xA5, 0x4B, 0x24, 1, 0]);
});

// ---- e-marker detail (k2/pd-read, part 2) -----------------------------------------------------------

test("emarker detail decode names the codes", () => {
  const d = P.decodeEmarkerDetail(u8(1, 2, 3, 2, 3, 0b00010101, 60, 80, 4, 2));
  assert.equal(d.sop, "SOP'");
  assert.equal(d.pd_rev, "3.x");
  assert.equal(d.vdo_version, 3);
  assert.equal(d.latency, "10-20 ns (~2 m)");
  assert.equal(d.termination, "Active (both ends powered)");
  assert.deepEqual([d.type_c_plug, d.vconn_required, d.epr_cable, d.vdo2_valid, d.usb4, d.usb_gen2, d.optical, d.thunderbolt],
    [true, false, true, false, true, false, false, false]);
  assert.deepEqual([d.max_temp_c, d.shutdown_temp_c, d.u3_power, d.usb_lanes], [60, 80, 4, 2]);
  assert.throws(() => P.decodeEmarkerDetail(u8(1, 2, 3)), P.FrameError);
});

test("emarker over the link attaches the detail when part 2 answers", async () => {
  const part0 = [1, 1, 0xAC, 0x05, 0x34, 0x12, 0, 0, 0, 0, 2, 3, 5, 50, 240, 0, 1, ...Array.from("Apple\0USB4", (c) => c.charCodeAt(0))];
  const { link } = rig((f) => [rsp(5, f[3], 0, f[5] === 0 ? part0 : f[5] === 1 ? [4, 3, 2, 1] : [1, 2, 3, 2, 3, 0, 60, 80, 4, 2])]);
  const e = await new P.K2Remote(link).emarker();
  assert.equal(e.detail.sop, "SOP'");
});

// ---- fast-charge trigger (k2/fc-trigger, cmd 0x32) --------------------------------------------------

const FC_PAYLOAD = [2, 1, 0, 0b10, 3, 0x28, 0x23, 0x10, 0x27, 2,   // active qc3, scan done step 3, target 9000, vbus 10000, 2 results
  0, 1, 0x28, 0x23,  5, 2, 0, 0];

test("fc decode: seventh QC3+ result survives", () => {
  const payload = [0, 0, 0, 2, 7, 0, 0, 0x88, 0x13, 7];
  for (let i = 0; i < 6; i++) payload.push(4, 2, 0, 0);
  payload.push(2, 1, 0x70, 0x17);
  const f = P.decodeFc(u8(...payload));
  assert.equal(f.scan.results.length, 7);
  assert.deepEqual(f.scan.results[6], { proto: "QC3+", outcome: "accepted", mv: 6000 });
});

test("fc decode: state, protocol, scan results", () => {
  const f = P.decodeFc(u8(...FC_PAYLOAD));
  assert.deepEqual([f.state, f.proto, f.fail], ["active", "QC3", "none"]);
  assert.deepEqual([f.scan.running, f.scan.done, f.scan.step], [false, true, 3]);
  assert.deepEqual([f.target_mv, f.vbus_mv], [9000, 10000]);
  assert.deepEqual(f.scan.results, [{ proto: "QC2", outcome: "accepted", mv: 9000 }, { proto: "SCP", outcome: "rejected", mv: 0 }]);
  assert.throws(() => P.decodeFc(u8(...FC_PAYLOAD.slice(0, 12))), P.FrameError);
});

const CABLE_PAYLOAD = [3, 1, 0x64, 0, 0xEC, 0x13, 0x28, 0, 0x24, 0x13, 0x02, 0x08, 0x32, 0, 0x46, 0];

test("cable-r decode and requests", async () => {
  const c = P.decodeCableR(u8(...CABLE_PAYLOAD));
  assert.deepEqual([c.idle, c.valid, c.grade, c.r_mohm], [true, true, "excellent", 100]);
  assert.deepEqual([c.idle_mv, c.idle_ma, c.load_mv, c.load_ma], [5100, 40, 4900, 2050]);
  assert.equal(P.decodeCableR(u8(...new Array(16).fill(0))).grade, "waiting");
  assert.equal(c.kelvin, false);
  const kd = P.decodeCableR(u8(0x06, 1, 0x78, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x32, 0, 0x46, 0));
  assert.deepEqual([kd.kelvin, kd.valid, kd.r_mohm, kd.pos_mohm, kd.gnd_mohm], [true, true, 120, 50, 70]);
  assert.throws(() => P.decodeCableR(u8(...CABLE_PAYLOAD.slice(0, 15))), P.FrameError);
  const { link, sent } = rig((f) => [rsp(0x50, f[3], 0, CABLE_PAYLOAD)]);
  const k = new P.K2Remote(link);
  await k.cableR();
  await k.cableR("reset");
  await k.cableR("kelvin");
  await k.cableR("passive");
  assert.deepEqual([...sent[0].slice(0, 6)], [0xA5, 0x4B, 0x50, 1, 1, 0]);
  assert.deepEqual([...sent[1].slice(0, 6)], [0xA5, 0x4B, 0x50, 2, 1, 1]);
  assert.deepEqual([...sent[2].slice(0, 6)], [0xA5, 0x4B, 0x50, 3, 1, 3]);
  assert.deepEqual([...sent[3].slice(0, 6)], [0xA5, 0x4B, 0x50, 4, 1, 2]);
});

test("fc requests: status, reset, scan, trigger bytes", async () => {
  const { link, sent } = rig((f) => [rsp(0x32, f[3], 0, FC_PAYLOAD)]);
  const k = new P.K2Remote(link);
  await k.fc("status");
  await k.fc("reset");
  await k.fc("scan", { confirm: true });
  await k.fc("trigger", { proto: "qc3", mv: 9000, confirm: true });
  assert.deepEqual([...sent[0].slice(0, 6)], [0xA5, 0x4B, 0x32, 1, 1, 0]);
  assert.deepEqual([...sent[1].slice(0, 6)], [0xA5, 0x4B, 0x32, 2, 1, 1]);
  assert.deepEqual([...sent[2].slice(0, 7)], [0xA5, 0x4B, 0x32, 3, 2, 2, 1]);
  assert.deepEqual([...sent[3].slice(0, 10)], [0xA5, 0x4B, 0x32, 4, 5, 3, 1, 0x28, 0x23, 1]);
});

test("fc: above 5 V without confirm is refused locally, like the CLI", async () => {
  const { link, sent } = rig(() => []);
  const k = new P.K2Remote(link);
  await assert.rejects(k.fc("trigger", { proto: "qc3", mv: 9000 }), RangeError);
  await assert.rejects(k.fc("scan", {}), RangeError);
  await assert.rejects(k.fc("trigger", { proto: "nope", mv: 5000 }), RangeError);
  assert.equal(sent.length, 0);
});

test("fc: allowInline sets flag bit 1 on scan and trigger, never on its own", async () => {
  const { link, sent } = rig((f) => [rsp(0x32, f[3], 0, FC_PAYLOAD)]);
  const k = new P.K2Remote(link);
  await k.fc("scan", { confirm: true, allowInline: true });
  await k.fc("trigger", { proto: "qc2", mv: 5000, allowInline: true });
  await k.fc("trigger", { proto: "qc2", mv: 9000, confirm: true, allowInline: true });
  assert.deepEqual([...sent[0].slice(4, 7)], [2, 2, 3]);       // confirm | allow-inline
  assert.deepEqual([...sent[1].slice(4, 10)], [5, 3, 0, 0x88, 0x13, 2]);
  assert.equal(sent[2][9], 3);
});

test("fc levels mirror the on-device trigger page (include/k2/ui/ui_trigger.hpp)", () => {
  assert.deepEqual(Object.keys(P.FC_LEVELS), P.FC_PROTOS);
  assert.deepEqual(P.FC_LEVELS.qc2.mv, [5000, 9000, 12000, 20000]);
  assert.equal(P.FC_LEVELS.qc2.def, 1);
  assert.deepEqual(P.FC_LEVELS.afc.mv, [9000, 12000]);
  assert.deepEqual(P.FC_LEVELS.scp.mv, [5000, 9000, 11000]);
  assert.deepEqual(P.FC_LEVELS.svooc65.mv, [5000]);
  assert.equal(P.FC_LEVELS.qc3.def, 2);
});

test("core ids: pages and the extended fast-charge names", () => {
  assert.deepEqual(P.PAGES, ["home", "detail", "chargertest", "lines", "tools", "settings"]);
  assert.equal(P.decodeState(u8(4, 0, 0, 0, 0, 0, 0, 0, ...new Array(16).fill(0))).page_name, "tools");
  assert.equal(P.decodeState(u8(5, 0, 0, 0, 0, 0, 0, 0, ...new Array(16).fill(0))).page_name, "settings");
  assert.equal(P.decodeState(u8(6, 0, 0, 0, 0, 0, 0, 0, ...new Array(16).fill(0))).page_name, "tools/trigger");
  assert.equal(P.decodeState(u8(7, 0, 0, 0, 0, 0, 0, 0, ...new Array(16).fill(0))).page_name, "tools/emarker");
  assert.equal(P.pageName(12), "settings/usblink");
  assert.equal(P.pageName(13), "settings/autorotate");
  assert.equal(P.pageName(14), "tools/cards");
  assert.equal(P.pageName(16), "tools/connection");
  assert.equal(P.pageName(17), "tools/diag");
  assert.equal(P.pageName(99), "99");
  assert.equal(P.decodeState(u8(0, 0, 0, 0, 0, 0, 0, 0, ...new Array(16).fill(0), ...new Array(14).fill(0).map((_, i) => i === 12 ? 22 : 0))).fc, "USB DATA");
});

// ---- device stats come from log lines (no command exists) --------------------------------------------

test("log stats: [USB] line", () => {
  assert.deepEqual(P.parseLogStats("[USB] gap=12ms isr=3456 rst=2 sent=100 drop=1\n"),
    { kind: "usb", gap_ms: 12, isr: 3456, resets: 2, sent: 100, dropped: 1 });
});

test("log stats: [RST] line", () => {
  assert.deepEqual(P.parseLogStats("[RST] CTRLSTS=14000000 PIN POR\n"),
    { kind: "rst", ctrlsts: 0x14000000, flags: "PIN POR" });
});

test("log stats: heartbeat", () => {
  assert.deepEqual(P.parseLogStats("[SYS] Heartbeat: up=125s presses=4 page=1\n"),
    { kind: "heartbeat", up_s: 125, presses: 4, page: 1 });
});

test("log stats: other lines and faults", () => {
  assert.equal(P.parseLogStats("[LCD] ok\n"), null);
  assert.equal(P.parseLogStats("[FAULT] HF PC=08004444 LR=08004445 CFSR=00000001 BFAR=00000000\n").kind, "fault");
});

// ---- formatting helpers the page uses ------------------------------------------------------------------

test("formatAgo", () => {
  assert.equal(P.formatAgo(0), "just now");
  assert.equal(P.formatAgo(45), "45 s ago");
  assert.equal(P.formatAgo(125), "2 min ago");
  assert.equal(P.formatAgo(7300), "2 h ago");
});

test("formatUptime", () => {
  assert.equal(P.formatUptime(5000), "5 s");
  assert.equal(P.formatUptime(3725000), "1 h 02 min 05 s");
  assert.equal(P.formatUptime(90061000), "1 d 01 h 01 min");
});

test("k2.html embeds k2proto.js byte for byte", () => {
  const html = readFileSync(path.join(here, "../tools/web/k2.html"), "utf8");
  const m = html.match(/<script id="k2proto">\n([\s\S]*?)<\/script>/);
  assert.ok(m, "k2.html has no <script id=\"k2proto\"> block");
  assert.equal(m[1], readFileSync(path.join(here, "../tools/web/k2proto.js"), "utf8"));
});


// ---- active PD (0x40-0x44) --------------------------------------------------------------------

test("match decodes the verdict, flags and row states", () => {
  const r = P.decodeMatch(u8(1, 1, 0, 2, 3, 2, 2, 0xE0, 0x2E, 0xB8, 0x0B, 0xB8, 0x0B, 0x84, 0x03, 0x98, 0x3A, 0x64, 0, 3, 1));
  assert.equal(r.verdict, "wants-higher-v");
  assert.deepEqual(r.flags, ["wants-higher-v"]);
  assert.deepEqual(r.rows, ["ok", "selected"]);
  assert.equal(r.want_mv, 12000); assert.equal(r.have_ma, 900); assert.equal(r.src_max_w, 100);
  assert.throws(() => P.decodeMatch(u8(1, 1, 0)), P.FrameError);
  assert.throws(() => P.decodeMatch(u8(1, 1, 0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1)), P.FrameError);
});

test("pd trigger status decodes 13 bytes", () => {
  const t = P.decodePdTrigger(u8(5, 1, 1, 0, 4, 0x20, 0x4E, 0xB8, 0x0B, 1, 0, 0, 0));
  assert.deepEqual(t, { state: "ready", result: "ok", held: true, epr: false, pos: 4, mv: 20000, ma: 3000, hard_resets: 1 });
  assert.throws(() => P.decodePdTrigger(u8(1, 2)), P.FrameError);
});

test("identify progress decodes masks and sink PDOs", () => {
  const p = P.decodeIdentifyProgress(u8(2, 6, 0x0B, 0x24, 1, 1, 0, 0, 0x20, 0x31, 0x2C, 0x91, 0x01, 0x00));
  assert.equal(p.state, "done");
  assert.deepEqual(p.answered, ["identity", "svids", "sink-cap"]);
  assert.deepEqual(p.no_answer, ["modes", "revision"]);
  assert.deepEqual(p.sink_pdos, [0x0001912C]);
  assert.equal(p.revision, 0x31200000);
});

test("RemoteError names the pd::Err byte of a PD command", () => {
  const e = new P.RemoteError(P.STATUS.BAD_ARG, P.CMD.PD_REQUEST, u8(5));
  assert.match(e.message, /inline/);
  assert.equal(new P.RemoteError(P.STATUS.NEED_CONFIRM, P.CMD.PD_REQUEST, u8(0)).message.includes("confirmation"), true);
  assert.equal(P.ACTIONS.identify, 6);
  assert.equal(P.ACTIONS["fc-scan"], 4);
  assert.equal(P.ACTIONS["fc-reset"], 5);
});

test("pdRequest sends the 6-byte payload and reads the trigger status", async () => {
  const sent = [];
  const link = { call: async (cmd, payload) => { sent.push([cmd, Array.from(payload || [])]);
    return { payload: cmd === P.CMD.PD_TRIGGER_STATUS ? u8(5, 1, 1, 0, 4, 0x20, 0x4E, 0xB8, 0x0B, 0, 0, 0, 0) : u8(0) }; } };
  const k = new P.K2Remote(link);
  const t = await k.pdRequest(4, { mv: 20000, ma: 3000, confirm: true });
  assert.deepEqual(sent[0], [P.CMD.PD_REQUEST, [4, 1, 0x20, 0x4E, 0xB8, 0x0B]]);
  assert.equal(t.mv, 20000);
});

test("pdAttach defaults to 5 V and sends the confirm flag only when asked", async () => {
  const sent = [];
  const link = { call: async (cmd, payload) => { sent.push([cmd, Array.from(payload || [])]);
    return { payload: u8(5, 1, 1, 0, 4, 0x88, 0x13, 0xB8, 0x0B, 0, 0, 0, 0) }; } };
  const k = new P.K2Remote(link);
  await k.pdAttach();                                         // no target = 5000 mV, no flag byte
  assert.deepEqual(sent[0], [P.CMD.PD_ATTACH, [0x88, 0x13]]);
  await k.pdAttach(9000, 0);                                  // above 5 V, not confirmed: the K2 answers NEED_CONFIRM
  assert.deepEqual(sent[1], [P.CMD.PD_ATTACH, [0x28, 0x23]]);
  await k.pdAttach(9000, 3000, { confirm: true });            // [mv, ma, flags]
  assert.deepEqual(sent[2], [P.CMD.PD_ATTACH, [0x28, 0x23, 0xB8, 0x0B, 1]]);
  await k.pdAttach(9000, 0, { confirm: true });               // ma rides along as 0
  assert.deepEqual(sent[3], [P.CMD.PD_ATTACH, [0x28, 0x23, 0, 0, 1]]);
});

test("RemoteError names the pd::Err byte of PD_ATTACH", () => {
  assert.match(new P.RemoteError(P.STATUS.BAD_ARG, P.CMD.PD_ATTACH, u8(5)).message, /inline/);
  assert.match(new P.RemoteError(P.STATUS.BUSY, P.CMD.PD_ATTACH, u8(6)).message, /busy/);
});

// ---- PD black box ---------------------------------------------------------------------------

const bbRec = (type, ts, ...pl) => [6 + pl.length, type, ts & 255, (ts >> 8) & 255, (ts >> 16) & 255, (ts >>> 24) & 255, ...pl];

test("black box capture list skips empty slots", () => {
  const p = u8(0x10, 0x27, 0, 0,  2, 0x20, 0x01, 0xE8, 0x03, 0, 0,  0, 0, 0, 0, 0, 0, 0,  4, 0x00, 0x02, 0x05, 0, 0, 0);
  const r = P.decodeBbCaps(p);
  assert.equal(r.now_ms, 10000);
  assert.deepEqual(r.captures, [{ slot: 0, trigger: "hard reset", bytes: 288, age_ms: 1000 },
                                { slot: 2, trigger: "detach", bytes: 512, age_ms: 5 }]);
  assert.throws(() => P.decodeBbCaps(u8(1, 2, 3)), /too short/);
});

test("black box capture read header", () => {
  const r = P.decodeBbCapRead(u8(8, 0x10, 0, 0, 0, 0x40, 0, 0, 0, 9, 9));
  assert.equal(r.filling, true); assert.equal(r.next, 16); assert.equal(r.len, 64);
  assert.deepEqual(Array.from(r.data), [9, 9]);
});

test("black box records decode to messages, VBUS, resets and CC events", () => {
  const bytes = Uint8Array.from([
    ...bbRec(1, 5000, 0xE0, 0x81, 0x11, 0x2C, 0x91, 0x01, 0x27, 0, 0, 0, 0),   // Rx Source_Capabilities, 1 object, crc
    ...bbRec(3, 14800, 0x82, 0x12, 0x00, 0x00, 0x00, 0x00),                   // Tx header 0x1282 = Request, no object bytes kept
    ...bbRec(12, 15000, 0x88, 0x13, 0x9C, 0xFF),                              // 5000 mV, -100 mA
    ...bbRec(5, 20000, 0),                                                    // hard reset rx
    ...bbRec(6, 21000, 0, 0, 0),                                              // detach
    ...bbRec(4, 22000, 1, 5, 0x10),                                           // TX no GoodCRC
    6, 1, 0,                                                                  // truncated tail
  ]);
  const ev = P.parseBbRecords(bytes);
  assert.equal(ev.length, 6);
  assert.equal(ev[0].kind, "msg"); assert.equal(ev[0].dir, "rx"); assert.equal(ev[0].name, "Source_Capabilities");
  assert.deepEqual(ev[0].objects, [0x2701912C]); assert.equal(ev[0].t_us, 0);
  assert.equal(ev[1].name, "Request"); assert.equal(ev[1].dir, "tx"); assert.equal(ev[1].t_us, 9800);
  assert.equal(ev[2].mv, 5000); assert.equal(ev[2].ma, -100);
  assert.equal(ev[3].name, "hard reset rx");
  assert.equal(ev[4].kind, "cc"); assert.equal(ev[4].attach, false);
  assert.equal(ev[5].name, "TX no GoodCRC (retries)");
  assert.equal(P.formatBbDelta(9800), "9.8 ms"); assert.equal(P.formatBbDelta(500000), "500 ms"); assert.equal(P.formatBbDelta(2000000), "2.00 s");
  assert.equal(JSON.parse(P.bbJson({ slot: 1, trigger: "detach", age_ms: 5, bytes: 9 }, ev)).events.length, 6);
});

test("black box JSON carries the device identity and the caps fingerprint", () => {
  const le = (v) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];
  const bytes = Uint8Array.from([
    // Rx Source_Capabilities from a source (header 0x2101): 5 V 3 A fixed, PPS 2.1-11 V 3 A
    ...bbRec(1, 1000, 0xE0, 0x01, 0x21, ...le(0x0001912C), ...le(0xC0DC213C), 0, 0, 0, 0),
    // Rx Discover Identity ACK on SOP from a sink-role device (0x400F): VID 18D1, PID 4EE1
    ...bbRec(1, 2000, 0xE0, 0x0F, 0x40, ...le(0xFF008041), ...le(0x000018D1), ...le(0), ...le(0x4EE10000), 0, 0, 0, 0),
    // the same from the source role (0x410F): Apple 05AC:1234
    ...bbRec(1, 3000, 0xE0, 0x0F, 0x41, ...le(0xFF008041), ...le(0x000005AC), ...le(0), ...le(0x12340000), 0, 0, 0, 0),
  ]);
  const ev = P.parseBbRecords(bytes);
  const id = P.bbIdentity(ev);
  assert.equal(id.fp, "33W 1PDO+1PPS");
  assert.equal(id.dev, "18D1:4EE1");
  assert.equal(id.src, "05AC:1234");
  assert.equal(id.key, "18D1:4EE1");
  const j = JSON.parse(P.bbJson({ slot: 0, trigger: "contract", age_ms: 1, bytes: 9 }, ev));
  assert.equal(j.identity.key, "18D1:4EE1"); assert.equal(j.identity.fp, "33W 1PDO+1PPS");
  assert.equal(P.bbIdentity([]).key, "?");
});

test("black box JSON carries the negotiated link, as the firmware decodes it", () => {
  const le = (v) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];
  const eudo = (2 << 28 | 4 << 21 | 3 << 19 | 3 << 17) >>> 0;
  const bytes = Uint8Array.from([
    ...bbRec(3, 10, 0x08, 0x11, ...le(eudo)),                                                   // Tx Enter_USB: USB4 Gen4, optical, 5 A
    ...bbRec(1, 20, 0xE0, 0x0F, 0x21, ...le(0x80878104), ...le(3 << 16 | 1 << 24), 0, 0, 0, 0), // Enter Mode TBT3 request
    ...bbRec(1, 30, 0xE0, 0x0F, 0x10, ...le(0x80878144), 0, 0, 0, 0),                           // ACK
    ...bbRec(1, 40, 0xE0, 0x0F, 0x11, ...le(0xFF018144), 0, 0, 0, 0),                           // DP Enter Mode ACK
    ...bbRec(1, 50, 0xE0, 0x0F, 0x21, ...le(0xFF018111), ...le(1 << 10), 0, 0, 0, 0),           // DP Configure: pin C
    ...bbRec(1, 60, 0xE0, 0x0F, 0x10, ...le(0xFF018151), 0, 0, 0, 0),                           // ACK
    ...bbRec(1, 70, 0xC0, 0x0F, 0x50, ...le(0xFF008041), ...le(4 << 27), ...le(0), ...le(0), ...le(4 | 2 << 5), 0, 0, 0, 0), // e-marker
    ...bbRec(1, 80, 0xE0, 0x0F, 0x40, ...le(0xFF008041), ...le((1 << 31 | 1 << 30) >>> 0), ...le(0), ...le(0), 0, 0, 0, 0),  // partner
  ]);
  const ev = P.parseBbRecords(bytes);
  const l = P.bbLink(ev);
  assert.equal(l.usb, "USB4 Gen4 80G"); assert.equal(l.tbt, "TBT3 40G act"); assert.equal(l.dp, "C");
  assert.equal(l.cable, "80G 5A act"); assert.equal(l.peer, "host+dev");
  assert.equal(l.text, "USB4 Gen4 80G | TBT3 40G act | DP alt C | cable 80G 5A act | partner USB host+dev");
  const j = JSON.parse(P.bbJson({ slot: 0, trigger: "contract", age_ms: 1, bytes: 9 }, ev));
  assert.equal(j.link.dp, "C"); assert.equal(j.link.text, l.text);
  const none = P.bbLink([]);
  assert.equal(none.text, "not seen"); assert.equal(none.usb, undefined);
  // a TBT3 request that was never acknowledged is not a negotiated link
  const nak = P.parseBbRecords(Uint8Array.from([...bbRec(1, 20, 0xE0, 0x0F, 0x21, ...le(0x80878104), ...le(3 << 16), 0, 0, 0, 0),
                                                ...bbRec(1, 30, 0xE0, 0x0F, 0x10, ...le(0x80878184), 0, 0, 0, 0)]));
  assert.equal(P.bbLink(nak).text, "not seen");
});

const emget = (seq, name, vid, pid) =>
  [0, seq & 255, seq >> 8, ...Array.from(name.padEnd(24, "\0"), (c) => c.charCodeAt(0)), vid & 255, vid >> 8, pid & 255, pid >> 8];

test("saved cables: list pages through the library and reads each name", async () => {
  const { link, sent } = rig((f) => [f[5] === 0 ? rsp(0x61, f[3], 0, [0, 2, 2, 5, 0, 9, 0])
    : rsp(0x61, f[3], 0, emget(f[6] | (f[7] << 8), f[6] === 5 ? "Apple-USB2-60W" : "b", 0x5AC, 2))]);
  const l = await new P.K2Remote(link).emList();
  assert.deepEqual(l.map((c) => c.seq), [5, 9]);
  assert.equal(l[0].name, "Apple-USB2-60W");
  assert.equal(l[0].vid, 0x5AC);
  assert.deepEqual(Array.from(sent[0].slice(5, 7)), [0, 0]);
});

test("saved cables: rename, delete confirm byte and spoof arm", async () => {
  const { link, sent } = rig((f) => [f[5] === 4 ? rsp(0x61, f[3], 0, [0, 7, 0])
    : f[5] === 1 ? rsp(0x61, f[3], 0, emget(7, "My cable", 0, 0))
    : f[5] === 3 ? rsp(0x61, f[3], 0, [0])
    : rsp(0x61, f[3], 0, [0, 1, 3, 0, 2, 0, 7, 0])]);
  const k = new P.K2Remote(link);
  assert.equal((await k.emRename(7, "My cable")).name, "My cable");
  assert.deepEqual(Array.from(sent[0].slice(5, 8)), [4, 7, 0]);
  await k.emDelete(7);
  assert.deepEqual(Array.from(sent[2].slice(5, 9)), [3, 7, 0, 1]);
  const s = await k.emSpoof(7);
  assert.deepEqual([s.active, s.requests, s.replies, s.seq], [true, 3, 2, 7]);
  assert.deepEqual(Array.from(sent[3].slice(5, 9)), [5, 1, 7, 0]);
  await assert.rejects(() => k.emRename(7, "bad/name"), RangeError);
});

test("saved cables: spoof a built-in cable with builtin:N", async () => {
  const { link, sent } = rig((f) => [rsp(0x61, f[3], 0, [0, 1, 3, 0, 2, 0, 0, 0])]);
  const k = new P.K2Remote(link);
  await k.emSpoof("builtin:3");
  assert.deepEqual(Array.from(sent[0].slice(5, 8)), [5, 3, 3]);
  await k.emSpoof("builtin:0");
  assert.deepEqual(Array.from(sent[1].slice(5, 8)), [5, 3, 0]);
  await assert.rejects(() => k.emSpoof("builtin:9"), RangeError);
  await assert.rejects(() => k.emSpoof("builtin:x"), RangeError);
});

test("saved cables: library error codes surface by name", async () => {
  const { link } = rig((f) => [rsp(0x61, f[3], 0, [6, 0, 0, 0, 0, 0, 0, 0])]);
  await assert.rejects(() => new P.K2Remote(link).emSpoof(7), /live sink/);
});

test("e-marker learn: run sends the options, then reads every transcript entry", async () => {
  const sum = [0, 1, 1, 4, 0, 0, 1, 2, 0, 0xD9, 0x22, 0x16, 0x61];
  const ent = [0, 0, 3, 2, 1, 1, 3, 0x4F, 0x01, 0x1F, 0xA0, 0xD9, 0x22, 0x5F, 0xA0, 0xD9, 0x22, 0x02, 0xCD, 0x01, 0xAB, 7, 0, 0, 0];
  const { link, sent } = rig((f) => [rsp(0x61, f[3], 0, f[6] === 1 ? sum : ent)]);
  const k = new P.K2Remote(link);
  const r = await k.emLearn([{ svid: 0x22D9, cmd: 31, vdos: [] }], true, false);
  assert.deepEqual(Array.from(sent[0].slice(5, 13)), [6, 1, 1, 1, 0xD9, 0x22, 31, 0]);
  assert.deepEqual(Array.from(sent[1].slice(5, 8)), [6, 2, 0]);
  assert.equal(r.summary.outcome, "learned");
  assert.equal(r.summary.pid, 0x6116);
  assert.equal(r.entries[0].step, "seed");
  assert.deepEqual(r.entries[0].request, [0x22D9A01F]);
  assert.deepEqual(r.entries[0].reply, [0x22D9A05F, 0xAB01CD02, 7]);
  await assert.rejects(() => k.emLearn([1, 2, 3, 4, 5].map(() => ({ svid: 1, cmd: 1 }))), RangeError);
  await assert.rejects(() => k.emLearn([{ svid: 1, cmd: 32 }]), RangeError);
});

test("e-marker learn: a busy refusal and the unmatched log", async () => {
  const bz = rig((f) => [rsp(0x61, f[3], 0, [7])]);
  await assert.rejects(() => new P.K2Remote(bz.link).emLearn([]), /busy/);
  const { link, sent } = rig((f) => [rsp(0x61, f[3], 0, f[6] === 255 ? [0, 5, 0, 1]
    : [0, 5, 0, 1, 2, 1, 0x8F, 0x14, 0x11, 0xA0, 0xD9, 0x22])]);
  const u = await new P.K2Remote(link).emUnmatched();
  assert.deepEqual(Array.from(sent[1].slice(5, 7)), [7, 0]);
  assert.equal(u.total, 5);
  assert.equal(u.entries[0].sop, "SOP''");
  assert.deepEqual(u.entries[0].words, [0x22D9A011]);
});

// ---- HTTP transport (the K2's own web server over USB networking) ---------------------------

const toHex = (b) => Buffer.from(b).toString("hex");
function httpRig(answer) {
  const calls = [];
  const fetch = async (url, init = {}) => {
    calls.push({ url, method: init.method || "GET", headers: init.headers || {} });
    return answer(url, calls[calls.length - 1]);
  };
  const t = P.httpTransport("", { fetch });
  const link = new P.Link(t, { timeoutMs: 0 });   // the fetch mock answers immediately; no wall-clock watchdog
  return { calls, link, t };
}
const jsonRes = (obj, status = 200) => ({ ok: status === 200, status, json: async () => obj, arrayBuffer: async () => new ArrayBuffer(0) });
const frameOf = (url) => Uint8Array.from(Buffer.from(new URL(url, "http://k2").searchParams.get("hex"), "hex"));

test("http: a request goes to /api/frame as hex with the X-K2 header and the response feeds the link", async () => {
  const { calls, link } = httpRig((url) => jsonRes({ rsp: toHex(rsp(3, frameOf(url)[3], 0, [1, 2, 3])) }));
  const r = await link.call(P.CMD.GET_STATE);
  assert.deepEqual(Array.from(r.payload), [1, 2, 3]);
  assert.equal(calls.length, 1);
  assert.equal(calls[0].method, "POST");
  assert.equal(calls[0].headers["X-K2"], "1");
  assert.match(calls[0].url, /^\/api\/frame\?hex=a54b03[0-9a-f]{122}$/);
  assert.ok(!/confirm/.test(calls[0].url));
});

test("http: VBUS-changing commands carry confirm=1, status reads do not", async () => {
  const { calls, link } = httpRig((url) => { const f = frameOf(url); return jsonRes({ rsp: toHex(rsp(f[2], f[3], 0, [0])) }); });
  const k2 = new P.K2Remote(link);
  await link.call(P.CMD.PD_RESET);
  await link.call(P.CMD.PD_RELEASE);
  await link.call(P.CMD.PD_REQUEST, [1, 1, 0x88, 0x13, 0xB8, 0x0B]);
  await link.call(P.CMD.FC, [0]);
  await link.call(P.CMD.FC, [2, 1]);
  await link.call(P.CMD.FC, [3, 0, 0x88, 0x13, 1]);
  await link.call(P.CMD.FC, [1]);
  await link.call(P.CMD.PING);
  await link.call(P.CMD.PD_ATTACH, [0x88, 0x13]);   // takes the Rd and asks for a contract: same gate as PD_REQUEST
  assert.deepEqual(calls.map((c) => /&confirm=1$/.test(c.url)), [true, true, true, false, true, true, true, false, true]);
  assert.ok(k2);
});

test("http: a 403 from the device rejects the call with the HTTP status", async () => {
  const { link } = httpRig(() => jsonRes({ error: "confirm required" }, 403));
  await assert.rejects(() => link.call(P.CMD.PD_RESET), /HTTP 403/);
});

test("http: a network failure rejects the call", async () => {
  const { link } = httpRig(() => { throw new Error("offline"); });
  await assert.rejects(() => link.call(P.CMD.PING), /offline/);
});

test("http: screenshot decodes the BMP (RGB565 top-down) into rgba", async () => {
  const w = 2, h = 2, hdr = new Uint8Array(66), dv = new DataView(hdr.buffer);
  hdr[0] = 0x42; hdr[1] = 0x4D; dv.setUint32(10, 66, true); dv.setUint32(18, w, true); dv.setInt32(22, -h, true); dv.setUint16(28, 16, true);
  const px = new Uint16Array([0xF800, 0x07E0, 0x001F, 0xFFFF]);
  const body = new Uint8Array(66 + 8); body.set(hdr); body.set(new Uint8Array(px.buffer), 66);
  const { calls, t } = httpRig(() => ({ ok: true, status: 200, arrayBuffer: async () => body.buffer }));
  const shot = await t.screenshot();
  assert.equal(calls[0].url, "/api/screenshot.bmp");
  assert.equal(shot.width, 2); assert.equal(shot.height, 2);
  assert.deepEqual(Array.from(shot.rgba.slice(0, 4)), [255, 0, 0, 255]);
  assert.deepEqual(Array.from(shot.rgba.slice(4, 8)), [0, 255, 0, 255]);
  assert.deepEqual(Array.from(shot.rgba.slice(8, 12)), [0, 0, 255, 255]);
  assert.deepEqual(Array.from(shot.rgba.slice(12, 16)), [255, 255, 255, 255]);
});

test("http: K2Remote.screenshot uses the transport's own path", async () => {
  const { link, t } = httpRig(() => jsonRes({}));
  t.screenshot = async () => ({ width: 1, height: 1, rgba: new Uint8ClampedArray(4) });
  assert.equal((await new P.K2Remote(link).screenshot()).width, 1);
});

test("http: the dashboard embeds the HTTP transport and picks it when served by the device", () => {
  const html = readFileSync(path.join(here, "../tools/web/k2.html"), "utf8");
  assert.match(html, /httpTransport/);
  assert.match(html, /\/api\/ping/);
});

test("report cards: list reads each card in three chunks and counts the verdicts", async () => {
  const payload = Uint8Array.from(Buffer.from(
    readFileSync(path.join(here, "fixtures", "report_card_sample.hex"), "utf8").trim(), "hex"));
  const { link, sent } = rig((f) => [f[5] === 0 ? rsp(0x62, f[3], 0, [0, 1, 1, 3, 0])
    : rsp(0x62, f[3], 0, [0, f[8], ...payload.slice(48 * f[8], 48 * f[8] + 48)])]);
  const l = await new P.K2Remote(link).cardsList();
  assert.equal(l.length, 1);
  assert.deepEqual([l[0].no, l[0].name, l[0].vid, l[0].pid, l[0].runs, l[0].pdos, l[0].state],
                   [3, "Desk 65W", 0x5AC, 0x1234, 2, 5, "Complete"]);
  assert.deepEqual([l[0].pass, l[0].fail, l[0].skip], [1, 2, 1]);
  assert.deepEqual(Array.from(sent[3].slice(5, 9)), [1, 3, 0, 2]);
});

test("report cards: a device error is reported by name", async () => {
  const { link } = rig((f) => [rsp(0x62, f[3], 0, [2])]);
  await assert.rejects(() => new P.K2Remote(link).cardsList(), /cards list: not found/);
});
