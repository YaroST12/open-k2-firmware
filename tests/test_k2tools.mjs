// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Node tests for the dashboard's tool clients in tools/web/k2proto.js: report-card rename/delete, the Black Box live read,
// the lab hand-back and Apple readout, and the PD Spec Check (status, start, stop, blob). Byte vectors follow
// include/k2/core/remote_proto.hpp and tools/k2_remote.py. Run: K2_WEB=1 node --test tests/test_k2tools.mjs
import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const P = require("../tools/web/k2proto.js");

const pad64 = (bytes) => { const o = new Uint8Array(64); o.set(bytes); return o; };
const rsp = (cmd, seq, status, payload = []) => pad64([0xA6, 0x4B, cmd, seq, status, payload.length, ...payload]);
function rig(replies) {
  const sent = [];
  const link = new P.Link({
    send: async (f) => {
      sent.push(f);
      for (const r of replies(f, sent.length) || []) setImmediate(() => link.feed(r));
    },
  }, { timeoutMs: 0, streamTimeoutMs: 0, busyPollMs: 5 });
  return { link, sent, remote: new P.K2Remote(link) };
}
const body = (f) => Array.from(f.slice(5, 5 + f[4]));
const le32 = (v) => [v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >>> 24) & 255];

// ---- report cards: rename and delete ----------------------------------------------------------------

test("cards: rename sends [2, no lo, no hi, name] and returns true", async () => {
  const { remote, sent } = rig((f) => [rsp(0x62, f[3], 0, [0])]);
  assert.equal(await remote.cardsRename(0x0103, "Desk 65W"), true);
  assert.deepEqual(body(sent[0]), [2, 3, 1, ...Array.from("Desk 65W", (c) => c.charCodeAt(0))]);
});

test("cards: rename checks the name before it sends anything", async () => {
  const { remote, sent } = rig(() => []);
  for (const bad of ["", "x".repeat(21), "tab\there", "Zażółć"]) {
    await assert.rejects(() => remote.cardsRename(3, bad), /name is 1\.\.20 characters/);
  }
  assert.equal(sent.length, 0);
});

test("cards: rename refused while a charger test runs says so", async () => {
  const { remote } = rig((f) => [rsp(0x62, f[3], 0, [5])]);
  await assert.rejects(() => remote.cardsRename(3, "Desk"), /cards rename: a charger test is running/);
});

test("cards: delete sends the confirm byte and names a missing card", async () => {
  const a = rig((f) => [rsp(0x62, f[3], 0, [0])]);
  assert.equal(await a.remote.cardsDelete(7), true);
  assert.deepEqual(body(a.sent[0]), [3, 7, 0, 1]);
  const b = rig((f) => [rsp(0x62, f[3], 0, [2])]);
  await assert.rejects(() => b.remote.cardsDelete(9), /cards delete: not found/);
});

// ---- Black Box live read ----------------------------------------------------------------------------

// Records: len type ts(4) payload. A TX GoodCRC (type 3, header 0x0001) and an RX Source_Capabilities (type 1: token, header, 1 PDO, crc).
const rec = (type, ts, pl) => [6 + pl.length, type, ...le32(ts), ...pl];
const RECS = [
  ...rec(3, 1000, [0x01, 0x00]),
  ...rec(1, 3500, [0xE0, 0x61, 0x11, 0x2C, 0x91, 0x01, 0x27, 0, 0, 0, 0]),
  ...rec(12, 4000, [0x88, 0x13, 0x2C, 0x01]),
];

test("blackbox: read without a cursor sends [0]; with one, the cursor LE", async () => {
  const a = rig((f) => [rsp(0x70, f[3], 0, [0, ...le32(5), ...le32(9)])]);
  await a.remote.blackboxRead();
  assert.deepEqual(body(a.sent[0]), [0]);
  await a.remote.blackboxRead(0x01020304);
  assert.deepEqual(body(a.sent[1]), [0, 4, 3, 2, 1]);
});

test("blackbox: read decodes flags, next, head and the records", async () => {
  const { remote } = rig((f) => [rsp(0x70, f[3], 0, [0b101, ...le32(77), ...le32(99), ...RECS])]);
  const r = await remote.blackboxRead(10);
  assert.deepEqual([r.next, r.head, r.overwritten, r.frozen, r.rebooted], [77, 99, true, false, true]);
  assert.equal(r.events.length, 3);
  assert.equal(r.events[0].name, "GoodCRC");
  assert.equal(r.events[1].name, "Source_Capabilities");
  assert.deepEqual([r.events[2].kind, r.events[2].mv, r.events[2].ma], ["vbus", 5000, 300]);
});

test("blackbox: a reply shorter than its header is a frame error", async () => {
  const { remote } = rig((f) => [rsp(0x70, f[3], 0, [0, 1, 2])]);
  await assert.rejects(() => remote.blackboxRead(), /too short/);
});

test("bbMessageRows: PD messages only, GoodCRC dropped, last N, with arrows and times", () => {
  const events = P.parseBbRecords(Uint8Array.from(RECS));
  const rows = P.bbMessageRows(events, { max: 5 });
  assert.equal(rows.length, 1);
  assert.deepEqual([rows[0].dir, rows[0].name, rows[0].objects.length], ["rx", "Source_Capabilities", 1]);
  assert.equal(P.bbMessageRows(events, { max: 5, goodCrc: true }).length, 2);
  assert.equal(P.bbMessageRows(events, { max: 1, goodCrc: true })[0].name, "Source_Capabilities");
  assert.deepEqual(P.bbMessageRows([], {}), []);
});

// ---- lab hand-back ----------------------------------------------------------------------------------

test("handback: without confirm sends nothing in the payload, with it [1]", async () => {
  const a = rig((f) => [rsp(0x49, f[3], 0, [2])]);
  const v = await a.remote.pdHandback(false);
  assert.deepEqual(body(a.sent[0]), []);
  assert.deepEqual([v.verdict, v.text, v.allowed], [2, "confirm needed", false]);
  const b = rig((f) => [rsp(0x49, f[3], 0, [0])]);
  const w = await b.remote.pdHandback(true);
  assert.deepEqual(body(b.sent[0]), [1]);
  assert.deepEqual([w.text, w.allowed], ["allowed", true]);
});

test("handback: every verdict has a name and an unknown one is shown as a number", () => {
  assert.deepEqual(P.HANDBACK_VERDICTS, ["allowed", "charger test running", "confirm needed", "not inline", "no contract owned",
    "far sink Request seen this attach", "already handed back this attach"]);
  assert.equal(P.handbackText(9), "verdict 9");
});

// ---- lab Apple readout ------------------------------------------------------------------------------

const cs = (s) => [...Array.from(s, (c) => c.charCodeAt(0)), 0];

test("apple read: arm, then header and the six fields", async () => {
  const fields = ["Apple Inc.", "Charger", "1.2.3", "A1", "00", "SN0001"];
  const { remote, sent } = rig((f) => {
    const b = body(f);
    if (b[0] === 1) return [rsp(0x4A, f[3], 0, [1, 0, 0, 0, 6])];
    if (b[1] === 0) return [rsp(0x4A, f[3], 0, [1, 1, 12, 6, 6])];
    return [rsp(0x4A, f[3], 0, [b[1], ...cs(fields[b[1] - 1])])];
  });
  const r = await remote.appleRead(true);
  assert.deepEqual(sent.map(body), [[1], [0, 0], [0, 1], [0, 2], [0, 3], [0, 4], [0, 5], [0, 6]]);
  assert.deepEqual([r.armed, r.why, r.reads, r.ok], [true, "done", 12, 6]);
  assert.deepEqual([r.maker, r.product, r.fw, r.hw, r.field90, r.serial], fields);
});

test("apple read: status only does not arm; a refusal is named", async () => {
  const a = rig((f) => [rsp(0x4A, f[3], 0, [0, 0, 0, 0, 0])]);
  const r = await a.remote.appleRead(false);
  assert.deepEqual(body(a.sent[0]), [0, 0]);
  assert.equal(r.armed, false);
  assert.equal(a.sent.length, 1);   // nothing armed: no field reads
  const b = rig((f) => [rsp(0x4A, f[3], 3, [3])]);
  await assert.rejects(() => b.remote.appleRead(true), /apple read refused: not partner/);
});

// ---- PD Spec Check ----------------------------------------------------------------------------------

test("pdspec: the 17-byte status block", () => {
  const p = Uint8Array.from([1, 0, 0, 2, 4, 9, 0x3F, 3, 12, 1, 1, ...[0x2C, 0x01], ...[0x5A, 0x00], ...[0x40, 0x03]]);
  const s = P.decodePdSpecStatus(p);
  assert.deepEqual([s.state, s.stop, s.start, s.family, s.item, s.items, s.families], ["running", "running", "ok", "PPS timeout", 4, 9, 0x3F]);
  assert.deepEqual([s.fails, s.passes, s.skips, s.release_unconfirmed, s.score, s.elapsed_s, s.blob_len], [3, 12, 1, true, 300, 90, 832]);
  assert.equal(s.families_text, "Transitions, PPS/AVS, PPS timeout, EPR timeout, Hard Reset, Detach");
  assert.throws(() => P.decodePdSpecStatus(new Uint8Array(16)), /too short/);
});

test("pdspec: start needs confirm, sends op families flags; stop and status are single ops", async () => {
  const st = [0, 0, 0, 0, 0, 0, 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
  const { remote, sent } = rig((f) => [rsp(0x5A, f[3], 0, st)]);
  await assert.rejects(() => remote.pdSpecStart(0x05, false), /confirm/);
  assert.equal(sent.length, 0);
  await assert.rejects(() => remote.pdSpecStart(0x40, true), /families/);
  await remote.pdSpecStart(0x05, true);
  await remote.pdSpecStart(0, true);
  await remote.pdSpecStart(0x02, true, true);      // PPS 20 mV over the whole window: flags bit 1 (kPdSpecFlagFull20)
  await remote.pdSpecStop();
  await remote.pdSpecStatus();
  assert.deepEqual(sent.map(body), [[1, 5, 1], [1, 0, 1], [1, 2, 3], [3], [0]]);
});

test("pdspec: start reports a refusal that rides in the status block", async () => {
  const { remote } = rig((f) => [rsp(0x5A, f[3], 0, [0, 0, 2, 0, 0, 0, 0x3F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0])]);
  const s = await remote.pdSpecStart(0, true);
  assert.equal(s.start, "no PD source");
});

// A blob with one transition and its records, the layout of tools/pdspec_blob.py: PSC1 (21 checks, 14-byte PPS points) or PSC2
// (n_checks, caps note and options in the header, 28-byte programmable passes).
const u16 = (v) => [v & 255, (v >> 8) & 255];
function makeBlob(version = 2) {
  const T = version === 1 ? P.PDSPEC_CHECKS.slice(0, 21) : P.PDSPEC_CHECKS, n = T.length;
  const out = [];
  const i32 = (v) => le32(v | 0);
  const nRec = version === 1 ? 1 : 3;
  out.push(...Array.from("PSC" + version, (c) => c.charCodeAt(0)), 2, 1, 1, nRec, ...u16(700), ...u16(95), 1,
    version === 1 ? 0 : n, version === 1 ? 0 : 1, version === 1 ? 0 : 1);
  T.forEach((d, k) => {
    const verdict = k === 0 ? 1 : k === 1 ? 2 : k === 11 ? 4 : 3;       // pass, fail, unverified, skip
    const skip = verdict === 3 ? 3 : 0;
    // accept_time's worst came from a PPS move (APDO 6, 100 mV class, down to 10.6 V), ps_rdy_time's from a fixed pair
    const a = k === 0 ? 0x96 : k === 1 ? 2 : 0, b = k === 0 ? 106 : k === 1 ? 3 : 0;
    out.push(verdict, skip, a, b, ...u16(verdict === 3 ? 0 : 4), ...u16(k === 1 ? 1 : 0),
      ...i32(k === 0 ? 20000 : 31000), ...i32(0), ...i32(k === 0 ? 25000 : 30000));
  });
  out.push(...le32(2900), ...le32(5500), ...le32(5000 | (123 << 16)), ...le32(80 | (700 << 16)), ...le32(150 | (5000 << 16)),
    ...le32(40 | (900 << 16)), ...le32(4), ...le32(0));
  out.push(1, 3, 1, 0, ...u16(150), ...u16(80), ...u16(300), ...u16(0), ...u16(5000), ...u16(5600), ...u16(9000), ...u16(2));
  if (version === 1) {
    out.push(2, 5, 0, 1, ...u16(9000), ...u16(0xFFFE), ...u16(0xFFEC), ...u16(40), ...u16(35));
  } else {
    const s16 = (v) => u16(v & 0xFFFF);
    // pos cls steps lo hi band psrdy err dmin over out | fails band rdy mono acc over out | status pad
    out.push(6, 1, ...u16(154), ...u16(3300), ...u16(11000), ...u16(21), ...u16(309), ...s16(17), ...s16(98), ...s16(-140), ...u16(0),
      0, 11, 0, 0, 0, 0, 0, 1);
    out.push(6, 0, ...u16(60), ...u16(3300), ...u16(11000), ...u16(80), ...u16(255), ...s16(-17), ...s16(17), ...s16(-144), ...u16(0),
      0, 1, 0, 0, 0, 0, 0x06, 0);
    out.push(7, 0x83, ...u16(2), ...u16(9000), ...u16(20000), ...u16(0xFFFF), ...u16(1000), ...s16(-5), ...s16(0x7FFF), ...s16(-0x7FFF),
      ...u16(20), 1, 0, 0, 0, 0, 1, 0, 0);
  }
  return { blob: Uint8Array.from(out), n };
}

test("pdspec: a PSC2 blob decodes to the header, checks, timers, transitions and programmable passes", () => {
  const { blob, n } = makeBlob();
  const r = P.decodePdSpecBlob(blob);
  assert.deepEqual([r.version, r.state, r.stop, r.score, r.elapsed_s, r.release_unconfirmed], [2, "finished", "done", 700, 95, true]);
  assert.deepEqual([r.caps_note, r.full20], ["PPS without AVS: allowed before PD R3.3", true]);
  assert.equal(r.checks.length, n);
  assert.equal(n, 36);
  assert.deepEqual([r.checks[0].key, r.checks[0].verdict, r.checks[0].worst, r.checks[0].hi], ["accept_time", "pass", 20000, 25000]);
  assert.deepEqual([r.checks[1].verdict, r.checks[1].fails, r.checks[1].penalty], ["fail", 1, 2]);   // ps_rdy_time weight 2
  assert.deepEqual([r.checks[11].verdict, r.checks[11].penalty], ["unverified", 0]);                 // inferred: no penalty
  assert.deepEqual([r.checks[2].verdict, r.checks[2].skip, r.checks[2].worst], ["skip", "no PPS", null]);
  assert.deepEqual(r.tally, { pass: 1, fail: 1, skip: n - 3, unverified: 1 });
  assert.equal(r.timers.pps_ka_ms, 2900);
  assert.deepEqual([r.timers.hr_from_mv, r.timers.hr_off_ms, r.timers.hr_dark_ms, r.timers.hr_on_ms], [5000, 123, 80, 700]);
  assert.equal(r.transitions[0].accept_ms, 15);
  assert.deepEqual([r.transitions[0].from, r.transitions[0].to, r.transitions[0].gap_ms, r.transitions[0].final_mv], [1, 3, 2, 9000]);
  assert.deepEqual(r.pps, []);
  assert.equal(r.passes.length, 3);
  const [p100, p20, slams] = r.passes;
  assert.deepEqual([p100.pos, p100.kind, p100.class, p100.step, p100.lo_mv, p100.hi_mv, p100.steps], [6, "pps", 1, "100 mV", 3300, 11000, 154]);
  assert.deepEqual([p100.band_ms, p100.psrdy_ms, p100.worst_err_mv, p100.min_delta_mv, p100.over_mv, p100.out_ms], [2.1, 30.9, 17, 98, -140, 0]);
  assert.deepEqual(p100.fails, { band: 0, ps_rdy: 11, monotonic: 0, accuracy: 0, overshoot: 0, transient: 0 });
  assert.equal(p100.status_text, "clean");
  assert.deepEqual([p100.track, p20.track, slams.track], [true, false, false]);
  assert.deepEqual([p20.step, p20.status, p20.status_text], ["20 mV", 6, "no answer, voltage"]);
  assert.deepEqual([slams.kind, slams.class, slams.step, slams.band_ms, slams.min_delta_mv, slams.over_mv, slams.status_text],
    ["avs", 3, "slams", null, null, null, null]);
  assert.deepEqual([slams.fails.band, slams.fails.transient], [1, 1]);
});

test("pdspec: a PSC1 blob (21 checks, PPS points) still decodes", () => {
  const { blob } = makeBlob(1);
  const r = P.decodePdSpecBlob(blob);
  assert.deepEqual([r.version, r.caps_note, r.full20, r.checks.length, r.passes.length], [1, null, false, 21, 0]);
  assert.deepEqual([r.pps[0].pos, r.pps[0].set_mv, r.pps[0].worst_err_mv, r.pps[0].min_delta_mv, r.pps[0].max_settle_ms], [2, 9000, -2, -20, 3.5]);
});

test("pdspec: a blob with a wrong magic or a short body is refused", () => {
  assert.throws(() => P.decodePdSpecBlob(Uint8Array.from(Array.from("XXXX00000000000000"), (c) => c.charCodeAt(0))), /PSC1 \/ PSC2/);
  const { blob } = makeBlob();
  assert.throws(() => P.decodePdSpecBlob(blob.slice(0, blob.length - 1)), /bytes/);
});

test("pdspec: pdSpecRead gathers the chunks until the blob is whole", async () => {
  const { blob } = makeBlob();
  const { remote, sent } = rig((f) => {
    const off = f[6] | f[7] << 8, n = Math.min(48, blob.length - off);
    return [rsp(0x5A, f[3], 0, [off & 255, off >> 8, blob.length & 255, blob.length >> 8, n, ...blob.slice(off, off + n)])];
  });
  const r = await remote.pdSpecRead();
  assert.equal(r.checks.length, P.PDSPEC_CHECKS.length);
  assert.equal(sent.length, Math.ceil(blob.length / 48));
  assert.deepEqual(body(sent[1]), [2, 48, 0]);
});

test("pdspec: formatting of a check's value, limit and origin", () => {
  const { blob } = makeBlob();
  const r = P.decodePdSpecBlob(blob);
  assert.equal(P.pdSpecValue("us", 15000), "15.0 ms");
  assert.equal(P.pdSpecValue("ms", 2900), "2900 ms");
  assert.equal(P.pdSpecValue("mv", 9000), "9.000 V");
  assert.equal(P.pdSpecValue("count", 3), "3");
  assert.equal(P.pdSpecValue("ms", 99999999), "not seen");
  assert.equal(P.pdSpecValue("ms", null), "-");
  assert.equal(P.pdSpecLimit(r.checks[0]), "<= 25.0 ms");
  assert.equal(P.pdSpecAt(r.checks[1], 1), "PDO 2 to 3");
  // accept_time is shared: a programmable move carries its level in b (33 and up), never a PDO index (tools/pdspec_blob.py fmt_at)
  assert.equal(P.pdSpecAt(r.checks[0], 0, r.version, r.passes), "APDO 6 100 mV down to 10.6 V");
  const k = (key) => P.PDSPEC_CHECKS.findIndex((d) => d.key === key);
  assert.equal(P.pdSpecAt({ key: "pps_band", a: 0x25, b: 50 }, k("pps_band")), "APDO 5 500 mV up to 5.0 V");
  assert.equal(P.pdSpecAt({ key: "avs_band", a: 0x17, b: 180 }, k("avs_band")), "APDO 7 500 mV up to 18.0 V");
  assert.equal(P.pdSpecAt({ key: "accept_time", a: 0x97, b: 180 }, 0, 2, [{ pos: 7, kind: "avs" }]), "APDO 7 500 mV down to 18.0 V");
  assert.equal(P.pdSpecAt({ key: "pps_settle", a: 2, b: 5 }, k("pps_settle"), 1), "APDO 2, point 5");
  assert.equal(P.pdSpecAt({ key: "hr_safe0v", a: 0, b: 0 }, k("hr_safe0v")), "the source");
  assert.equal(P.pdSpecAt({ key: "pps_keepalive", a: 6, b: 0 }, k("pps_keepalive")), "PDO 6");
});

test("pdspec: the check table is the firmware's own (result.hpp kDefs and engine.hpp names)", () => {
  const res = readFileSync(path.join(here, "../include/k2/pdspec/result.hpp"), "utf8");
  const rows = /kDefs\{\{([\s\S]*?)\n\}\};/.exec(res)[1];
  const defs = [...rows.matchAll(/\{"(\w+)",\s*"([^"]*)",\s*Kind::(\w+),\s*Unit::(\w+),\s*(\d+)(?:,\s*(true|false))?\}/g)]
    .map((m) => ({ key: m[1], label: m[2], kind: m[3].toLowerCase(), unit: m[4].toLowerCase(), weight: +m[5], inferred: m[6] === "true" }));
  assert.ok(defs.length > 10);
  assert.deepEqual(P.PDSPEC_CHECKS.map((c) => ({ ...c })), defs);
  const eng = readFileSync(path.join(here, "../include/k2/pdspec/engine.hpp"), "utf8");
  const cases = (fn, en) => {
    const body = new RegExp("const char\\* " + fn + "\\([^)]*\\)\\s*\\{([\\s\\S]*?)\\n\\}").exec(eng)[1];
    return [...body.matchAll(new RegExp("case " + en + "::\\w+:\\s*return\\s*\"([^\"]*)\";", "g"))].map((m) => m[1]);
  };
  assert.deepEqual(P.PDSPEC_STOPS, cases("stop_name", "Stop"));
  assert.deepEqual(P.PDSPEC_STARTS, cases("start_name", "StartResult"));
  assert.deepEqual(P.PDSPEC_FAMILIES, cases("family_name", "Family"));
  // the skip reasons and caps notes (result.hpp) and the step sizes per class (plan.hpp)
  const rcases = (fn, en) => {
    const body = new RegExp("const char\\* " + fn + "\\([^)]*\\)\\s*\\{([\\s\\S]*?)\\n\\}").exec(res)[1];
    return [...body.matchAll(new RegExp("case " + en + "::\\w+:\\s*return\\s*\"([^\"]*)\";", "g"))].map((m) => m[1]);
  };
  assert.deepEqual(P.PDSPEC_SKIPS, ["-"].concat(rcases("skip_name", "Skip").slice(1)));
  assert.deepEqual(P.PDSPEC_CAPS_NOTES, rcases("caps_note_name", "CapsNote"));
  const plan = readFileSync(path.join(here, "../include/k2/pdspec/plan.hpp"), "utf8");
  const steps = (kind) => new RegExp(kind + "\\[3\\]\\s*=\\s*\\{([^}]*)\\}").exec(plan)[1].split(",").map(Number);
  assert.deepEqual(P.PDSPEC_STEPS, { pps: steps("pps"), avs: steps("avs") });
});


// ---- the PD spec section of a Deep charger test (0x0B [6, off]; specsec::to_wire) -----------------------------------------
const specFx = JSON.parse(readFileSync(path.join(here, "fixtures/ct_spec_section.json"), "utf8"));
const specWire = Uint8Array.from(Buffer.from(specFx.wire, "hex"));
const specDetail = Uint8Array.from(Buffer.from(specFx.detail, "hex"));

test("charger spec section: the tables are the firmware's own (spec_section.hpp order, spec_text.hpp labels)", () => {
  const sec = readFileSync(path.join(here, "../include/k2/chargertest/spec_section.hpp"), "utf8");
  const keys = P.PDSPEC_CHECKS.map((c) => c.key);
  const enumNames = [...readFileSync(path.join(here, "../include/k2/pdspec/result.hpp"), "utf8").matchAll(/kDefs\{\{([\s\S]*?)\n\}\};/g)][0][1]
    .match(/\{"(\w+)"/g).map((s) => s.slice(2, -1));
  assert.deepEqual(enumNames, keys);
  const order = /kOrder\{\s*([\s\S]*?)\};/.exec(sec)[1].match(/C::(\w+)/g).map((s) => s.slice(3));
  // Check::PpsMonotonic is "pps_monotonic": the enumerators are the CamelCase of the keys
  const camel = (k) => k.split("_").map((w) => w[0].toUpperCase() + w.slice(1)).join("");
  assert.deepEqual(P.CT_SPEC_ORDER.map(camel), order);
  assert.equal(P.CT_SPEC_WIRE, 3 + 4 * order.length);
  assert.equal(P.CT_SPEC_DETAIL_WIRE, 8 * order.length);
  assert.match(sec, /kDetailBytes = 8 \* kCells/);
  const text = readFileSync(path.join(here, "../include/k2/ui/spec_text.hpp"), "utf8");
  const short = [...(/kCheckShort\[\]\[9\] = \{([\s\S]*?)\};/.exec(text)[1]).matchAll(/"([^"]*)"/g)].map((m) => m[1]);
  assert.equal(short.length, keys.length);
  keys.forEach((k, i) => { if (P.CT_SPEC_ORDER.includes(k)) assert.equal(P.CT_SPEC_SHORT[k], short[i], k); });
  const starts = [...sec.matchAll(/kOrder\[i\] == C::(\w+)/g)].map((m) => m[1]);
  const sections = P.decodeChargerSpecSection(specWire).cells.filter((c) => c.section).map((c) => camel(c.key));
  assert.deepEqual(sections, [camel(P.CT_SPEC_ORDER[0]), ...starts]);
});

test("charger spec section: decodes the firmware's own bytes like the Python tool", () => {
  const s = P.decodeChargerSpecSection(specWire), want = specFx.decoded;
  assert.deepEqual({ end: s.end, why: s.why, unconfirmed: s.unconfirmed, tally: s.tally }, { end: want.end, why: want.why, unconfirmed: want.unconfirmed, tally: want.tally });
  assert.deepEqual(s.cells.map((c) => ({ key: c.key, label: c.label, unit: c.unit, verdict: c.verdict, skip: c.skip, fails: c.fails, value: c.value, value_text: c.value_text })),
                   want.cells);
  assert.equal(s.cells[0].short, "Accept");
  assert.throws(() => P.decodeChargerSpecSection(specWire.subarray(1)), /expected 111/);
  const bad = Uint8Array.from(specWire); bad[0] = 9;
  assert.throws(() => P.decodeChargerSpecSection(bad), /not a PD spec section/);
});

test("charger spec section: the detail form decodes like the Python tool (limit, count, place)", () => {
  const s = P.decodeChargerSpecSection(specWire, specDetail), want = specFx.decoded_detail;
  assert.equal(s.detailed, want.detailed);
  assert.deepEqual(s.cells.map((c) => ({ key: c.key, count: c.count, limit: c.limit, limit_text: c.limit_text, at_text: c.at_text })),
                   want.cells.map((c) => ({ key: c.key, count: c.count, limit: c.limit, limit_text: c.limit_text, at_text: c.at_text })));
  assert.equal(s.cells[0].limit_text, "<= 25.0 ms");
  assert.equal(s.cells[1].count, 300);
  assert.equal(P.decodeChargerSpecSection(specWire).detailed, false);   // the first form alone: no detail keys
  assert.equal(P.decodeChargerSpecSection(specWire).cells[0].count, undefined);
  assert.throws(() => P.decodeChargerSpecSection(specWire, specDetail.subarray(1)), /expected 216/);
  // a measured cell with no count (a journal from before the detail record) keeps nothing
  const blank = P.decodeChargerSpecSection(specWire, new Uint8Array(216));
  assert.equal(blank.detailed, false);
  assert.ok(blank.cells.every((c) => c.limit_text === null));
});

test("charger spec section: summary lines", () => {
  const lines = P.specLines(P.decodeChargerSpecSection(specWire));
  assert.match(lines[0], /^PD spec: 2 pass, 1 fail, 1 skip, 1 unverified, 22 not judged$/);
  assert.ok(lines.some((l) => /FAIL, worst 40\.0 ms, 3 over/.test(l)));
  assert.ok(lines.some((l) => /skipped, no PPS/.test(l)));
  assert.ok(lines.some((l) => /WARNING: the end-of-run return to 5 V was not confirmed/.test(l)));
  assert.deepEqual(P.specLines(null), []);
  assert.ok(P.specLines(P.decodeChargerSpecSection(specWire, specDetail)).some((l) => /FAIL, worst 40\.0 ms, 3 over \(limit <= 25\.0 ms, 300 measured\)/.test(l)));
});

const specChunks = (runId, blob, detail) => (f) => {
  const form = body(f)[0], data = form === 7 ? detail : blob;
  if (!data) return [rsp(0x0B, f[3], 2, [])];   // BadLength: no such form (an older firmware)
  const off = body(f)[1], n = Math.min(48, data.length - off);
  return [rsp(0x0B, f[3], 0, [...le32(runId), data.length, off, n, ...data.subarray(off, off + n)])];
};

test("charger spec section: read in 48-byte chunks, 0x0B [6, off] then the detail form [7, off]", async () => {
  const { remote, sent } = rig(specChunks(0x01020304, specWire, specDetail));
  const s = await remote.chargerSpecSection(0x01020304);
  assert.deepEqual(sent.map(body), [[6, 0], [6, 48], [6, 96], [7, 0], [7, 48], [7, 96], [7, 144], [7, 192]]);
  assert.equal(s.cells.length, 27);
  assert.equal(s.cells[1].value_text, "40.0 ms");
  assert.equal(s.cells[1].count, 300);
  assert.equal(s.cells[0].limit_text, "<= 25.0 ms");
  assert.equal(s.detailed, true);
});

test("charger spec section: firmware without the detail form gives the cells alone", async () => {
  const { remote, sent } = rig(specChunks(0x01020304, specWire, null));
  const s = await remote.chargerSpecSection(0x01020304);
  assert.deepEqual(sent.map(body), [[6, 0], [6, 48], [6, 96], [7, 0]]);
  assert.equal(s.cells[1].value_text, "40.0 ms");
  assert.equal(s.detailed, false);
  assert.equal(s.cells[1].count, undefined);
});

test("charger spec section: none, a moved run and a stuck chunk", async () => {
  const none = rig((f) => [rsp(0x0B, f[3], 0, [...le32(7), 0, 0, 0])]);
  assert.equal(await none.remote.chargerSpecSection(7), null);
  const moved = rig(specChunks(8, specWire));
  await assert.rejects(() => moved.remote.chargerSpecSection(7), /another run/);
  const stuck = rig((f) => [rsp(0x0B, f[3], 0, [...le32(7), 111, 0, 0])]);
  await assert.rejects(() => stuck.remote.chargerSpecSection(7), /did not advance/);
});
