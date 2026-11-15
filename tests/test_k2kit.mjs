// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The dashboard kit's interaction logic and renderers (tools/web/k2kit.js): the arm-then-confirm state machine with an
// injected clock, the footer's state priority, the RowList / ActionCell / AlertBand / TopBar strings, the PD set-point
// maths (clamped at the K2's 36 V monitor ceiling) and the fast-charge offer list (QC stays at or below 12 V, VOOC family
// is never offered as a trigger).
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const K = require(path.join(root, "tools/web/k2kit.js"));
const P = require(path.join(root, "tools/web/k2proto.js"));

// ---- arm then confirm ------------------------------------------------------------------------------------------------------
test("arm: first press arms, second press inside 5 s fires and disarms", () => {
  let r = K.armPress(null, "pd-request", 1000);
  assert.equal(r.fire, false);
  assert.deepEqual(r.st, { id: "pd-request", at: 1000 });
  r = K.armPress(r.st, "pd-request", 5999);
  assert.equal(r.fire, true);
  assert.equal(r.st, null);
});

test("arm: the window is 5000 ms; a press after it only re-arms", () => {
  assert.equal(K.ARM_MS, 5000);
  const a = K.armPress(null, "x", 0).st;
  const late = K.armPress(a, "x", 5001);
  assert.equal(late.fire, false);
  assert.deepEqual(late.st, { id: "x", at: 5001 });
  assert.equal(K.armPress(a, "x", 5000).fire, true, "exactly 5.000 s is still inside");
});

test("arm: pressing another action moves the arm and never fires it", () => {
  const a = K.armPress(null, "pd-request", 0).st;
  const b = K.armPress(a, "fc-go", 100);
  assert.equal(b.fire, false);
  assert.deepEqual(b.st, { id: "fc-go", at: 100 });
});

test("arm: expiry and time left", () => {
  const a = { id: "x", at: 1000 };
  assert.equal(K.armLeftMs(a, 1000), 5000);
  assert.equal(K.armLeftMs(a, 3500), 2500);
  assert.equal(K.armLeftMs(a, 9000), 0);
  assert.equal(K.armLeftMs(null, 0), 0);
  assert.deepEqual(K.armExpire(a, 5999), a);
  assert.equal(K.armExpire(a, 6001), null);
  assert.equal(K.armExpire(null, 0), null);
  assert.equal(K.armCancel(a), null);
});

// ---- footer: error beats armed beats info ---------------------------------------------------------------------------------
test("footer: not connected, error, armed and idle states in priority order", () => {
  assert.deepEqual(K.footerState({ connected: false }), { tone: "caution", text: "Not connected. Connect the K2 first." });
  assert.deepEqual(K.footerState({ connected: true, error: "The firmware refused: busy", armed: "Request PDO 2" }),
    { tone: "error", text: "The firmware refused: busy" });
  assert.deepEqual(K.footerState({ connected: true, armed: "Request PDO 2 at 9 V", leftMs: 3200 }),
    { tone: "caution", text: "Armed: Request PDO 2 at 9 V. Press again within 4 s to confirm." });
  assert.deepEqual(K.footerState({ connected: true, info: "Contract 9 V 3 A" }), { tone: "info", text: "Contract 9 V 3 A" });
  assert.deepEqual(K.footerState({ connected: true }), { tone: "info", text: "Ready." });
  // a disconnect wins over a stale error
  assert.equal(K.footerState({ connected: false, error: "x" }).tone, "caution");
});

// ---- renderers --------------------------------------------------------------------------------------------------------------
test("RowList: square markers, radio selection, a note line only under the focused row", () => {
  const html = K.rowListHtml([
    { id: "p1", label: "1", value: "5 V 3 A", note: "fixed, 15 W" },
    { id: "p2", label: "2", value: "9 V 3 A", note: "fixed, 27 W" },
    { id: "p3", label: "3", value: "48 V 5 A", disabled: true, note: "above the 36 V monitor" }
  ], { selected: "p2", focus: "p2", name: "pdo" });
  assert.match(html, /role="radiogroup"/);
  assert.equal((html.match(/class="k-rl-row/g) || []).length, 3);
  assert.equal((html.match(/k-sel/g) || []).length, 1);
  assert.match(html, /data-id="p2"[^>]*aria-checked="true"/);
  assert.match(html, /data-id="p1"[^>]*aria-checked="false"/);
  assert.match(html, /data-id="p3"[^>]*aria-disabled="true"/);
  assert.equal((html.match(/class="k-rl-note"/g) || []).length, 1);
  assert.match(html, /fixed, 27 W/);
  assert.doesNotMatch(html, /fixed, 15 W/);
  assert.match(html, /<i class="k-mk"/);
});

test("RowList escapes labels and values", () => {
  const html = K.rowListHtml([{ id: "a<b", label: "<x>", value: "&" }], {});
  assert.doesNotMatch(html, /<x>/);
  assert.match(html, /&lt;x&gt;/);
  assert.match(html, /&amp;/);
  assert.match(html, /data-id="a&lt;b"/);
});

test("ActionCell: idle, armed with a draining bar, busy and disabled", () => {
  const idle = K.actionCellHtml({ id: "go", label: "REQUEST", state: "idle" });
  assert.match(idle, /<button[^>]*data-act="go"/);
  assert.match(idle, /REQUEST/);
  assert.doesNotMatch(idle, /k-armed/);
  assert.doesNotMatch(idle, /k-drain/);
  const armed = K.actionCellHtml({ id: "go", label: "REQUEST", state: "armed", leftMs: 3000, armedLabel: "CONFIRM" });
  assert.match(armed, /k-armed/);
  assert.match(armed, /CONFIRM/);
  assert.match(armed, /<i class="k-drain" style="width:60%"><\/i>/, "3000 of 5000 ms left is 60 %");
  assert.match(K.actionCellHtml({ id: "go", label: "R", state: "armed" }), /style="width:100%"/);
  assert.match(K.actionCellHtml({ id: "go", label: "R", state: "armed", leftMs: 0 }), /style="width:0%"/);
  assert.match(armed, /aria-pressed="true"/);
  const dis = K.actionCellHtml({ id: "go", label: "REQUEST", state: "disabled", reason: "Not connected" });
  assert.match(dis, /disabled/);
  assert.match(dis, /title="Not connected"/);
  const busy = K.actionCellHtml({ id: "go", label: "REQUEST", state: "busy" });
  assert.match(busy, /disabled/);
  assert.match(busy, /k-busy/);
});

test("AlertBand: error and caution are alerts, info and ok are status", () => {
  assert.match(K.alertBandHtml({ tone: "error", text: "Link lost" }), /role="alert"[^>]*>Link lost</);
  assert.match(K.alertBandHtml({ tone: "caution", text: "x" }), /role="alert"/);
  assert.match(K.alertBandHtml({ tone: "ok", text: "x" }), /role="status"/);
  assert.match(K.alertBandHtml({ tone: "bogus", text: "x" }), /k-foot-info/);
  assert.equal(K.alertBandHtml({ tone: "ok", text: "" }), "");
});

test("TopBar: unit colours on the live volts and amps, a dash while there is no reading", () => {
  const h = K.topBarHtml({ title: "PD", mv: 9012, ma: 1500 });
  assert.match(h, /<span class="k-u-v">9\.01 V<\/span>/);
  assert.match(h, /<span class="k-u-a">1\.50 A<\/span>/);
  assert.match(h, /<span class="k-u-w">13\.5 W<\/span>/);
  const none = K.topBarHtml({ title: "PD" });
  assert.match(none, /<span class="k-u-v">- V<\/span>/);
  assert.doesNotMatch(none, /k-u-w/);
});

test("number formatting helpers", () => {
  assert.equal(K.fmtV(5000), "5.00");
  assert.equal(K.fmtV(20100), "20.10");
  assert.equal(K.fmtA(3000), "3.00");
  assert.equal(K.fmtA(2250), "2.25");
  assert.equal(K.fmtW(9000, 1500), "13.5");
  assert.equal(K.fmtW(20000, 5000), "100");
});

// ---- PD set point ----------------------------------------------------------------------------------------------------------
test("monitor ceiling is 36 V", () => assert.equal(K.CEILING_MV, 36000));

test("snapMv: PPS in 20 mV steps, AVS in 100 mV, always inside the PDO range and the ceiling", () => {
  assert.equal(K.snapMv("pps", 9013, 3300, 11000), 9020);
  assert.equal(K.snapMv("pps", 2000, 3300, 11000), 3300);
  assert.equal(K.snapMv("pps", 30000, 3300, 11000), 11000);
  assert.equal(K.snapMv("avs", 15040, 15000, 28000), 15000);
  assert.equal(K.snapMv("avs", 15060, 15000, 28000), 15100);
  assert.equal(K.snapMv("avs", 40000, 15000, 48000), 36000, "above the monitor ceiling is cut");
  assert.equal(K.snapMv("pps", 21000, 3300, 21000), 21000);
});

test("stepMv: one step up or down, coarse with the flag, never past the ends", () => {
  const pps = { type: "pps", min_mv: 3300, max_mv: 11000 };
  assert.equal(K.stepMv(pps, 5000, 1), 5100);
  assert.equal(K.stepMv(pps, 5000, -1), 4900);
  assert.equal(K.stepMv(pps, 5000, 1, true), 6000);
  assert.equal(K.stepMv(pps, 10950, 1), 11000);
  assert.equal(K.stepMv(pps, 11000, 1), 11000);
  assert.equal(K.stepMv(pps, 3300, -1, true), 3300);
  const avs = { type: "avs", min_mv: 15000, max_mv: 48000 };
  assert.equal(K.stepMv(avs, 35900, 1), 36000);
  assert.equal(K.stepMv(avs, 36000, 1), 36000, "stops at the 36 V ceiling");
  assert.equal(K.stepMv(avs, 15000, -1), 15000);
});

test("clampMa: stays in 1..max, 50 mA steps, a zero or bad value becomes the PDO max", () => {
  assert.equal(K.clampMa(2000, 3000), 2000);
  assert.equal(K.clampMa(4000, 3000), 3000);
  assert.equal(K.clampMa(0, 3000), 3000);
  assert.equal(K.clampMa(NaN, 3000), 3000);
  assert.equal(K.clampMa(1013, 3000), 1000);
  assert.equal(K.clampMa(10, 3000), 50);
});

test("pdoRows: kinds, adjustability, and the monitor ceiling", () => {
  const rows = K.pdoRows([
    { type: "fixed", min_mv: 5000, max_mv: 5000, max_ma: 3000, max_mw: 15000 },
    { type: "fixed", min_mv: 20000, max_mv: 20000, max_ma: 5000, max_mw: 100000 },
    { type: "pps", min_mv: 3300, max_mv: 11000, max_ma: 3000, max_mw: 33000 },
    { type: "avs", min_mv: 15000, max_mv: 48000, max_ma: 5000, max_mw: 240000 },
    { type: "fixed", min_mv: 48000, max_mv: 48000, max_ma: 5000, max_mw: 240000 },
    { type: "battery", min_mv: 5000, max_mv: 9000, max_ma: 0, max_mw: 18000 }
  ]);
  assert.deepEqual(rows.map((r) => r.id), ["1", "2", "3", "4", "5", "6"]);
  assert.equal(rows[0].adjustable, false);
  assert.equal(rows[0].value, "5.00 V 3.00 A");
  assert.equal(rows[0].disabled, false);
  assert.equal(rows[2].adjustable, true);
  assert.equal(rows[2].value, "3.30-11.00 V 3.00 A");
  assert.equal(rows[3].adjustable, true);
  assert.equal(rows[3].disabled, false, "an AVS range above 36 V stays selectable, the set point is capped");
  assert.equal(rows[3].top_mv, 36000);
  assert.equal(rows[4].disabled, true);
  assert.match(rows[4].note, /36 V/);
  assert.equal(rows[5].disabled, true, "battery PDOs are not requestable here");
  assert.match(rows[5].note, /battery/);
});

test("pdoRows: a fixed PDO's request is the PDO itself (mv 0 asks for it as advertised)", () => {
  const r = K.pdoRows([{ type: "fixed", min_mv: 9000, max_mv: 9000, max_ma: 3000, max_mw: 27000 }])[0];
  assert.equal(r.request.mv, 0);
  assert.equal(r.request.ma, 3000);
  assert.equal(r.request.confirm, true, "9 V is above 5 V");
  const five = K.pdoRows([{ type: "fixed", min_mv: 5000, max_mv: 5000, max_ma: 3000, max_mw: 15000 }])[0];
  assert.equal(five.request.confirm, false);
});

// ---- fast charge offers ----------------------------------------------------------------------------------------------------
test("fcOffers: QC never above 12 V, VOOC family is not a trigger", () => {
  const offers = K.fcOffers(P.FC_LEVELS, P.FC_PROTOS);
  const by = Object.fromEntries(offers.map((o) => [o.proto, o]));
  assert.deepEqual(by.qc2.mv, [5000, 9000, 12000]);
  assert.deepEqual(by.qc3.mv, [5000, 6000, 9000, 12000]);
  assert.deepEqual(by["qc3+"].mv, [5000, 6000, 9000, 12000]);
  for (const bad of ["vooc", "svooc", "svooc65"]) assert.equal(by[bad], undefined, bad);
  for (const ok of ["afc", "fcp", "scp", "ufcs"]) assert.ok(by[ok], ok);
  assert.ok(offers.every((o) => o.mv.length > 0 && o.def >= 0 && o.def < o.mv.length));
  const qc = offers.filter((o) => /^qc/.test(o.proto));
  assert.ok(qc.every((o) => o.mv.every((v) => v <= 12000)));
  assert.equal(by.qc2.def, 1);
});

test("fcOffers: a default that was cut falls back to the first level", () => {
  const o = K.fcOffers({ qc2: { mv: [20000, 9000], def: 0 } }, ["qc2"]);
  assert.deepEqual(o[0].mv, [9000]);
  assert.equal(o[0].def, 0);
});

test("fcOffers: a protocol whose only levels are cut is dropped", () => {
  assert.deepEqual(K.fcOffers({ qc2: { mv: [20000], def: 0 } }, ["qc2"]), []);
});

test("fcSafe: the fc ops the page may raise are the same list k2proto confirms", () => {
  assert.equal(K.FC_MAX_QC_MV, 12000);
  assert.equal(K.fcNeedsConfirm(5000), false);
  assert.equal(K.fcNeedsConfirm(5001), true);
  assert.equal(K.fcNeedsConfirm(9000), true);
});

test("fcDetected: the D+/D- readout, VOOC family is detect only", () => {
  assert.deepEqual(K.fcDetected("..."), { label: "reading", note: "", detectOnly: false });
  assert.deepEqual(K.fcDetected("NO QUICK"), { label: "NO QUICK", note: "", detectOnly: false });
  const v = K.fcDetected("VOOC WARP");
  assert.equal(v.detectOnly, true);
  assert.match(v.note, /detect only/i);
  assert.equal(K.fcDetected("SVOOC").detectOnly, true);
  assert.equal(K.fcDetected("QC3.0").detectOnly, false);
  assert.equal(K.fcDetected(undefined).label, "reading");
});

// ---- the set point stepper and the selection ---------------------------------------------------------------------------------
const PPS_ROW = K.pdoRows([{ type: "pps", min_mv: 3300, max_mv: 11000, max_ma: 3000, max_mw: 33000 }])[0];
const FIX_ROW = K.pdoRows([{ type: "fixed", min_mv: 9000, max_mv: 9000, max_ma: 3000, max_mw: 27000 }])[0];

test("pdStep: a programmable row steps mV by 100 (1000 coarse) and mA by 250, a fixed row only mA", () => {
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "mv+"), { mv: 5100, ma: 3000 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "mv-"), { mv: 4900, ma: 3000 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "mv++"), { mv: 6000, ma: 3000 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 3300, ma: 3000 }, "mv--"), { mv: 3300, ma: 3000 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "ma-"), { mv: 5000, ma: 2750 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "ma+"), { mv: 5000, ma: 3000 }, "never above the PDO's own limit");
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 100 }, "ma-"), { mv: 5000, ma: 50 }, "never below 50 mA");
  assert.deepEqual(K.pdStep(FIX_ROW, { mv: 0, ma: 3000 }, "mv+"), { mv: 0, ma: 3000 });
  assert.deepEqual(K.pdStep(FIX_ROW, { mv: 0, ma: 3000 }, "ma-"), { mv: 0, ma: 2750 });
  assert.deepEqual(K.pdStep(PPS_ROW, { mv: 5000, ma: 3000 }, "nope"), { mv: 5000, ma: 3000 });
});

test("pdSelection: the row the contract holds, else the first selectable row, else nothing", () => {
  const rows = K.pdoRows([
    { type: "fixed", min_mv: 5000, max_mv: 5000, max_ma: 3000, max_mw: 15000 },
    { type: "fixed", min_mv: 9000, max_mv: 9000, max_ma: 3000, max_mw: 27000 },
    { type: "fixed", min_mv: 48000, max_mv: 48000, max_ma: 5000, max_mw: 240000 }
  ]);
  assert.equal(K.pdSelection(rows, { held: true, pos: 2 }, null), "2");
  assert.equal(K.pdSelection(rows, { held: false, pos: 2 }, null), "1");
  assert.equal(K.pdSelection(rows, { held: true, pos: 3 }, null), "1", "a disabled row is never selected");
  assert.equal(K.pdSelection(rows, { held: false, pos: 0 }, "2"), "2", "the user's pick wins while it is still selectable");
  assert.equal(K.pdSelection(rows, { held: false, pos: 0 }, "3"), "1");
  assert.equal(K.pdSelection([], { held: false, pos: 0 }, null), null);
  assert.equal(K.pdSelection(rows, null, null), "1");
});

// ---- errors in plain words ---------------------------------------------------------------------------------------------------
test("errorText: every failure the page can meet reads as a sentence, never a status code", () => {
  const re = (st) => new P.RemoteError(st, 0x20, new Uint8Array(0));
  assert.match(K.errorText(new P.RemoteTimeout("x"), "PD request"), /did not answer/i);
  assert.match(K.errorText(new P.BootloaderDetected(), "PD request"), /DFU|bootloader/i);
  assert.match(K.errorText(re(P.STATUS.UNKNOWN_CMD), "PD request"), /does not have|no such/i);
  assert.match(K.errorText(re(P.STATUS.BUSY), "PD request"), /busy/i);
  assert.match(K.errorText(re(P.STATUS.BAD_ARG), "PD request"), /refused/i);
  assert.match(K.errorText(re(P.STATUS.BAD_LENGTH), "PD request"), /does not understand/i);
  assert.match(K.errorText(re(P.STATUS.NEED_CONFIRM), "PD request"), /confirm/i);
  assert.equal(K.errorText(new RangeError("name is 1..24 characters"), "rename"), "name is 1..24 characters");
  assert.match(K.errorText(new Error("boom"), "rename"), /rename.*boom/i);
  for (const e of [new P.RemoteTimeout("x"), re(P.STATUS.BAD_ARG), re(P.STATUS.BUSY), new Error("boom")])
    assert.doesNotMatch(K.errorText(e, "x"), /status|0x|undefined/i);
});

// ---- the PD contract view ----------------------------------------------------------------------------------------------------
test("contractView: the contract the K2 holds as KvTable rows with a tone footer", () => {
  const held = K.contractView({ state: "ready", result: "ok", held: true, epr: false, pos: 3, mv: 9000, ma: 2000, hard_resets: 0 });
  assert.deepEqual(held.rows.map((r) => r.label), ["State", "PDO", "Requested", "Power", "Hard resets"]);
  assert.equal(held.rows[1].value, "3");
  assert.equal(held.rows[2].value, "9.00 V 2.00 A");
  assert.equal(held.rows[3].value, "18.0 W");
  assert.equal(held.tone, "ok");
  assert.match(held.footer, /9\.00 V/);
  const epr = K.contractView({ state: "ready", result: "ok", held: true, epr: true, pos: 9, mv: 28000, ma: 5000, hard_resets: 0 });
  assert.match(epr.rows[2].value, /EPR/);
  const idle = K.contractView({ state: "idle", result: "none", held: false, epr: false, pos: 0, mv: 0, ma: 0, hard_resets: 0 });
  assert.equal(idle.rows[1].value, "-");
  assert.equal(idle.rows[2].value, "-");
  assert.equal(idle.tone, "info");
  const bad = K.contractView({ state: "failed", result: "rejected", held: false, epr: false, pos: 2, mv: 9000, ma: 3000, hard_resets: 2 });
  assert.equal(bad.tone, "error");
  assert.match(bad.footer, /rejected/);
  assert.equal(bad.rows.at(-1).level, "caution", "a hard reset seen is an exception");
  assert.equal(bad.rows.at(-1).word, "SEEN");
  assert.equal(K.contractView(null).tone, "info");
});

// ---- recent PD messages ------------------------------------------------------------------------------------------------------
test("messageTableHtml: time from the first row, direction, name and the data objects", () => {
  const html = K.messageTableHtml([
    { t_us: 1000000, dir: "rx", name: "Source_Capabilities", objects: [0x0801912C, 0x0002D12C], sop: true, msg_id: 0, header: 0x11A1 },
    { t_us: 1012500, dir: "tx", name: "Request", objects: [0x1004B12C], sop: true, msg_id: 1, header: 0x1082 }
  ]);
  assert.match(html, /class="k-msg"/);
  assert.match(html, />\+0\.000 s</);
  assert.match(html, />\+0\.013 s</);
  assert.match(html, />RX</);
  assert.match(html, />TX</);
  assert.match(html, /Source_Capabilities/);
  assert.match(html, /0801912C 0002D12C/);
  assert.match(html, /1004B12C/);
  assert.equal(K.messageTableHtml([]), '<div class="k-msg-empty">No PD messages yet.</div>');
});

test("messageTableHtml: escapes names, marks the SOP' / SOP'' traffic that is not the source's", () => {
  const html = K.messageTableHtml([{ t_us: 0, dir: "rx", name: "<b>x</b>", objects: [], sop: false, msg_id: 0, header: 0 }]);
  assert.doesNotMatch(html, /<b>x/);
  assert.match(html, /&lt;b&gt;x/);
  assert.match(html, /cable/i, "a non-SOP message is labelled as cable traffic");
});

// ---- the PD Spec Check result ------------------------------------------------------------------------------------------------
function specBlob(over) {
  const checks = P.PDSPEC_CHECKS.map((d) => ({ key: d.key, label: d.label, kind: d.kind, unit: d.unit, weight: d.weight,
    verdict: "pass", skip: null, n: 4, fails: 0, penalty: 0, worst: 12000, lo: -1000000, hi: 25000, a: 1, b: 2 }));
  return Object.assign({ state: "finished", stop: "done", score: 0, elapsed_s: 95, release_unconfirmed: false,
    tally: { pass: checks.length, fail: 0, skip: 0, unverified: 0 }, checks, timers: {}, transitions: [], pps: [], passes: [], version: 2,
    caps_note: null, full20: false }, over || {});
}

test("specResultHtml: the head is a KvTable, one grid line per check, a rule where the family changes", () => {
  const html = K.specResultHtml(specBlob(), P);
  assert.match(html, /class="k-kv/);
  assert.match(html, /Penalty/);
  assert.match(html, /class="k-rg/);
  assert.equal((html.match(/k-rg-row/g) || []).length, P.PDSPEC_CHECKS.length);
  assert.ok((html.match(/k-sec/g) || []).length >= 4, "family rules");
  assert.match(html, /PS_RDY time/);
  assert.match(html, /&lt;= 25\.0 ms/, "limit in the K2's words (escaped)");
});

test("specResultHtml: a fail is a fail cell with the worst value, a skip carries its reason", () => {
  const b = specBlob();
  b.checks[1] = Object.assign({}, b.checks[1], { verdict: "fail", fails: 2, penalty: 4, worst: 31000 });
  b.checks[3] = Object.assign({}, b.checks[3], { verdict: "skip", skip: "no PPS", n: 0, worst: null });
  b.score = 4; b.tally = { pass: 19, fail: 1, skip: 1, unverified: 0 };
  const html = K.specResultHtml(b, P);
  assert.match(html, /k-fail[^>]*aria-label="31\.0 ms fail"/);
  assert.match(html, /k-skip/);
  assert.match(html, /no PPS/);
  assert.match(html, /k-foot-error/, "any fail makes the footer an error band");
});

test("specResultHtml: inferred checks are shown unverified and never as a pass or fail", () => {
  const b = specBlob();
  const k = b.checks.findIndex((c) => c.key === "pps_settle");
  b.checks[k] = Object.assign({}, b.checks[k], { verdict: "unverified", fails: 3, penalty: 0 });
  const html = K.specResultHtml(b, P);
  const row = html.split('<div class="k-rg-row').find((r) => /PPS settle/.test(r));
  assert.ok(row, "pps_settle row");
  assert.doesNotMatch(row, /k-fail|k-pass/);
  assert.match(row, /unverified/i);
  assert.match(row, /class="k-vc k-unverified"/, "the outlined amber cell");
});

test("specResultHtml: the grid follows the device's order, the PPS/AVS checks right after the transitions", () => {
  const html = K.specResultHtml(specBlob(), P);
  const labels = [...html.matchAll(/<div class="k-rg-label">([^<]*)<\/div>/g)].map((m) => m[1]);
  assert.deepEqual(labels.slice(0, 12), ["Accept", "PS_RDY time", "VBUS start", "PS_RDY early", "PS_RDY low", "Rise time", "Fall time",
    "Overshoot", "Accuracy", "PPS monotonic", "PPS accuracy", "PPS settle"]);
  assert.deepEqual(labels.slice(12, 18), ["PPS in band", "PPS PS_RDY", "PPS overshoot", "PPS transient", "PPS_Status", "PPS_Status t"]);
  assert.deepEqual([labels[18], labels[19], labels[25]], ["PPS tracking", "AVS monotonic", "AVS tracking"]);
  assert.deepEqual(labels.slice(26), ["PPS timeout", "EPR timeout", "HR to 0 V", "HR off time", "HR turn-on", "HR caps", "Detach to 0 V",
    "Re-attach", "Reattach caps", "Source reset"]);
});

test("specResultHtml: one line per programmable pass with band, PS_RDY, level and PPS_Status cells", () => {
  const pass = (o) => Object.assign({ pos: 6, kind: "pps", class: 1, step: "100 mV", lo_mv: 3300, hi_mv: 11000, steps: 154, band_ms: 2.1,
    psrdy_ms: 30.9, worst_err_mv: 17, min_delta_mv: 98, over_mv: -140, out_ms: 0,
    fails: { band: 0, ps_rdy: 11, monotonic: 0, accuracy: 0, overshoot: 0, transient: 0 }, status: 0, status_text: "clean" }, o);
  const b = specBlob({ caps_note: "PPS without AVS: allowed before PD R3.3", passes: [
    pass({ class: 0, step: "20 mV", band_ms: 8.1, psrdy_ms: 25.5, fails: { band: 0, ps_rdy: 0, monotonic: 0, accuracy: 0, overshoot: 0, transient: 0 } }),
    pass({}),
    pass({ class: 3, step: "slams", status: 6, status_text: "no answer, voltage", fails: { band: 0, ps_rdy: 0, monotonic: 1, accuracy: 0, overshoot: 0, transient: 0 } }),
    pass({ pos: 7, kind: "avs", class: 2, step: "1000 mV", status_text: null, psrdy_ms: null }),
  ] });
  const html = K.specResultHtml(b, P);
  assert.match(html, /PPS without AVS/, "the caps note in the head");
  const rows = html.split("Programmable passes")[1].split('<div class="k-rg-row').slice(1);
  assert.equal(rows.length, 4);
  // 20 mV: the band cell is the settle on the final level, unverified (outlined amber); PS_RDY within the limit passes
  assert.match(rows[0], /k-rg-label">A6 PPS 20 mV</, "two APDOs: each line names its own");
  assert.match(rows[0], /class="k-vc k-unverified"[^>]*aria-label="8\.1 ms unverified"/);
  assert.match(rows[0], /class="k-vc k-pass"[^>]*aria-label="25\.5 ms pass"/);
  // 100 mV: PS_RDY late on 11 moves is a fail cell with its count
  assert.match(rows[1], /class="k-vc k-fail"[^>]*aria-label="30\.9 ms fail" title="worst Accept to PS_RDY of 154 moves, 11 fail"/);
  assert.match(rows[1], /aria-label="clean pass"/);
  // slams: a monotonic fail shows in the level cell, the PPS_Status fault bits in the status cell
  assert.match(rows[2], /k-rg-label">A6 PPS slams</);
  assert.match(rows[2], /class="k-vc k-fail"[^>]*aria-label="\+17 mV fail" title="[^"]*monotonic 1,/);
  assert.match(rows[2], /class="k-vc k-fail"[^>]*aria-label="no answer, voltage fail"/);
  // AVS: no PPS_Status, no PS_RDY seen
  assert.match(rows[3], /k-rg-label">A7 AVS 1000 mV</);
  assert.match(rows[3], /^ k-sec/, "a rule where the APDO changes");
  assert.match(rows[3], /aria-label="not seen fail"/);
  assert.doesNotMatch(rows[3], /clean/);
  assert.match(html, /band/);
  assert.match(html, /PS_RDY/);
});

test("specResultHtml: a run that stopped early or could not confirm its release says so in the footer", () => {
  const stopped = K.specResultHtml(specBlob({ stop: "load on the output" }), P);
  assert.match(stopped, /load on the output/);
  assert.match(stopped, /k-foot-(caution|error)/);
  const rel = K.specResultHtml(specBlob({ release_unconfirmed: true }), P);
  assert.match(rel, /release/i);
  assert.match(rel, /k-foot-caution/);
  const run = K.specResultHtml(specBlob({ state: "running", stop: "running" }), P);
  assert.match(run, /running/i);
});

// ---- action cells from the shared arm state --------------------------------------------------------------------------------
test("actCell: idle, armed with the draining bar, disabled and busy", () => {
  const a = { id: "pd-request", label: "REQUEST", reason: "" };
  assert.doesNotMatch(K.actCell(a, null, 0), /k-armed/);
  const arm = { id: "pd-request", at: 1000 };
  const armed = K.actCell(a, arm, 3500);
  assert.match(armed, /k-armed/);
  assert.match(armed, /CONFIRM/);
  assert.match(armed, /style="width:50%"/, "2.5 s of 5 s left");
  assert.doesNotMatch(K.actCell(a, arm, 7000), /k-armed/, "the window has run out");
  assert.doesNotMatch(K.actCell({ id: "other", label: "OTHER" }, arm, 3500), /k-armed/, "one arm at a time");
  assert.match(K.actCell(a, null, 0, "disabled"), /disabled/);
  assert.match(K.actCell(a, null, 0, "busy"), /k-busy/);
  assert.match(K.actCell(Object.assign({ armedLabel: "SEND NOW" }, a), arm, 1000), /SEND NOW/);
});

test("stepperHtml: one group per value with square buttons, a disabled end stops the press", () => {
  const html = K.stepperHtml([
    { name: "Voltage", value: "9.00 V", dec: [{ dir: "mv--", text: "-1" }, { dir: "mv-", text: "-0.1" }], inc: [{ dir: "mv+", text: "+0.1" }, { dir: "mv++", text: "+1", off: true }] },
    { name: "Current", value: "3.00 A", dec: [{ dir: "ma-", text: "-0.25" }], inc: [{ dir: "ma+", text: "+0.25" }] }
  ]);
  assert.match(html, /9\.00 V/);
  assert.match(html, /data-step="mv--"/);
  assert.match(html, /data-step="ma\+"/);
  assert.match(html, /data-step="mv\+\+"[^>]*disabled/);
  assert.doesNotMatch(html, /data-step="mv-"[^>]*disabled/);
  assert.match(html, /aria-label="Voltage"/);
});

// ---- PD Spec Check start: the family picker and the progress line ----------------------------------------------------------
test("specFamilyMask: one bit per family in the engine's order; none picked is refused by returning 0", () => {
  assert.equal(K.specFamilyMask([true, true, true, true, true, true]), 0x3f);
  assert.equal(K.specFamilyMask([true, false, false, false, true, false]), 0x11);
  assert.equal(K.specFamilyMask([false, false, false, false, false, false]), 0);
  assert.equal(K.SPEC_FAMILY_NOTES.length, P.PDSPEC_FAMILIES.length, "one note per family");
});

test("specProgress: the running line names the family, the item, the elapsed time and the fail count", () => {
  const st = { state: "running", stop: "running", family: "PPS steps", item: 2, items: 5, fails: 1, passes: 3, skips: 0, elapsed_s: 75, start: "ok", release_unconfirmed: false };
  const p = K.specProgress(st);
  assert.match(p.text, /PPS steps/);
  assert.match(p.text, /2 of 5/);
  assert.match(p.text, /1:15/);
  assert.equal(p.tone, "info");
  const refused = K.specProgress({ state: "idle", start: "no PD source" });
  assert.equal(refused.tone, "error");
  assert.match(refused.text, /no PD source/);
  assert.equal(K.specProgress({ state: "idle", start: "ok" }).tone, "info");
  assert.equal(K.specProgress({ state: "finished", stop: "done", start: "ok", fails: 0, release_unconfirmed: false }).tone, "ok");
  assert.equal(K.specProgress({ state: "finished", stop: "done", start: "ok", fails: 2, release_unconfirmed: false }).tone, "error");
  assert.equal(K.specProgress({ state: "finished", stop: "cancelled", start: "ok", fails: 0, release_unconfirmed: false }).tone, "caution");
});

// ---- cables page lists as RowLists -----------------------------------------------------------------------------------------
test("savedRows and cardRows: the id is the number the firmware renames or deletes by", () => {
  const rows = K.savedRows([{ seq: 7, name: "Anker 240W", vid: 0x2b1d, pid: 0x1234 }, { seq: 9, name: "", vid: 1, pid: 2 }]);
  assert.deepEqual(rows.map((r) => r.id), ["7", "9"]);
  assert.equal(rows[0].label, "Anker 240W");
  assert.match(rows[0].value, /2B1D:1234/i);
  assert.equal(rows[1].label, "(unnamed)");
  const cards = K.cardRows([{ no: 3, name: "IQ4", vid: 0x291a, pid: 0x4003, pdos: 6, runs: 2, pass: 40, fail: 1, skip: 3, state: "ok" }]);
  assert.equal(cards[0].id, "3");
  assert.match(cards[0].label, /#3/);
  assert.match(cards[0].label, /IQ4/);
  assert.match(cards[0].value, /40/);
  assert.match(cards[0].note, /6 PDOs/);
  assert.deepEqual(K.savedRows([]), []);
});

test("unmatchedView: what the firmware held back, with the counts, or an empty note", () => {
  const empty = K.unmatchedView({ total: 0, held: 0, entries: [] });
  assert.equal(empty.rows.length, 1);
  assert.match(empty.footer, /nothing/i);
  const some = K.unmatchedView({ total: 5, held: 2, entries: [{ sop: "SOP'", header: 0x4a1, words: [0xff008001] }, { sop: "SOP''", header: 0x4a2, words: [] }] });
  assert.equal(some.rows.length, 2);
  assert.match(some.rows[0].value, /FF008001/);
  assert.match(some.rows[0].label, /SOP'/);
  assert.match(some.footer, /2 of 5/);
});

// ---- pages and the fast-charge selection ----------------------------------------------------------------------------------
test("routes: the page list, the #triggers alias and the titles", () => {
  assert.deepEqual(K.ROUTES, ["live", "test", "pd", "fc", "spec", "cables", "device"]);
  assert.equal(K.routeFor("#pd"), "pd");
  assert.equal(K.routeFor("cables"), "cables");
  assert.equal(K.routeFor("#/test"), "test", "the slash form the Charger test opens with");
  assert.equal(K.routeFor("#/triggers"), "pd");
  assert.equal(K.routeFor("#triggers"), "pd", "the old Triggers address opens PD");
  assert.equal(K.routeFor("#nope"), "live");
  assert.equal(K.routeFor(""), "live");
  assert.equal(K.routeFor(undefined), "live");
  for (const r of K.ROUTES) assert.ok(K.pageTitle(r).length > 0, r);
  assert.equal(K.pageTitle("fc"), "Fast charge");
  assert.equal(K.pageTitle("spec"), "PD Spec Check");
});

test("fcSelection: the picked protocol and level, or the first offer at its default", () => {
  const offers = K.fcOffers(
    { qc2: { mv: [5000, 9000, 12000, 20000], def: 1 }, scp: { mv: [4500, 5000], def: 1 }, afc: { mv: [5000, 9000, 12000], def: 1 } },
    ["qc2", "scp", "afc"]);
  assert.deepEqual(K.fcSelection(offers, null), { proto: "qc2", mv: 9000, offer: offers[0] });
  assert.deepEqual(K.fcSelection(offers, { proto: "afc", mv: 12000 }).mv, 12000);
  assert.equal(K.fcSelection(offers, { proto: "afc", mv: 12000 }).proto, "afc");
  // a level the protocol does not offer snaps to its default
  assert.equal(K.fcSelection(offers, { proto: "scp", mv: 9000 }).mv, 5000);
  // an unknown protocol falls back to the first offer
  assert.equal(K.fcSelection(offers, { proto: "zzz", mv: 5000 }).proto, "qc2");
  assert.equal(K.fcSelection([], null), null);
});

test("fcStepLevel: walks the offered levels and stops at both ends", () => {
  const offer = { proto: "qc2", mv: [5000, 9000, 12000], def: 1 };
  assert.equal(K.fcStepLevel(offer, 9000, 1), 12000);
  assert.equal(K.fcStepLevel(offer, 9000, -1), 5000);
  assert.equal(K.fcStepLevel(offer, 12000, 1), 12000);
  assert.equal(K.fcStepLevel(offer, 5000, -1), 5000);
  assert.equal(K.fcStepLevel(offer, 7777, 1), 9000, "an unlisted level steps to the next one above");
  assert.equal(K.fcStepLevel(offer, 7777, -1), 5000);
});

// ---- the embed in k2.html stays in sync ------------------------------------------------------------------------------------
test("k2.html embeds the current k2kit.js and k2kit.css", () => {
  const html = readFileSync(path.join(root, "tools/web/k2.html"), "utf8");
  for (const [tag, id, file] of [["script", "k2kit", "k2kit.js"], ["style", "k2kit", "k2kit.css"]]) {
    const m = new RegExp("<" + tag + ' id="' + id + '">\\n?([\\s\\S]*?)</' + tag + ">").exec(html);
    assert.ok(m, id);
    assert.equal(m[1].replace(/\n$/, ""), readFileSync(path.join(root, "tools/web", file), "utf8").replace(/\n$/, ""), id);
  }
});
