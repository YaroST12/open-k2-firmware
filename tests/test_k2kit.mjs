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

// ---- the embed in k2.html stays in sync ------------------------------------------------------------------------------------
test("k2.html embeds the current k2kit.js and k2kit.css", () => {
  const html = readFileSync(path.join(root, "tools/web/k2.html"), "utf8");
  for (const [tag, id, file] of [["script", "k2kit", "k2kit.js"], ["style", "k2kit", "k2kit.css"]]) {
    const m = new RegExp("<" + tag + ' id="' + id + '">\\n?([\\s\\S]*?)</' + tag + ">").exec(html);
    assert.ok(m, id);
    assert.equal(m[1].replace(/\n$/, ""), readFileSync(path.join(root, "tools/web", file), "utf8").replace(/\n$/, ""), id);
  }
});
