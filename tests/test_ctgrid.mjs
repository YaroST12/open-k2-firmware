// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The web charger-test grid: tools/web/ctgrid.js (the layout, a port of src/chargertest/result_layout.cpp and
// step_label.cpp over the rows the remote returns) and tools/web/k2kit.js (the device-token kit that draws it).
// Seams: CtGrid.stepLabel / CtGrid.layout (pure), K2Kit.resultGridHtml (pure string), the embeds in k2.html.
// The golden cases come from tests/test_result_layout.cpp (the Baseus run) and tests/fixtures/step_labels.txt.
import { test } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const require = createRequire(import.meta.url);
const root = path.join(path.dirname(fileURLToPath(import.meta.url)), "..");
const CtGrid = require(path.join(root, "tools/web/ctgrid.js"));
const K2Kit = require(path.join(root, "tools/web/k2kit.js"));

// ---- the wire rows (k2proto decodeChargerTest: protocol, algorithm, result, reason are strings) ------------------------
import { S, baseus, REQ, NOOFFER, PDACTIVE } from "./support/ctfixture.mjs";
// One grid line as text: "label: 5V/3A:pass 9V/3A:pass", a rule above the line is "--".
function fmt(rows) {
  return rows.map((r) => (r.section ? "-- " : "") + (r.label || "") + ": " +
    r.cells.map((c) => c.label + (c.current ? "/" + c.current : "") + ":" + c.verdict).join(" "));
}
const lines = (steps, opts) => fmt(CtGrid.layout(steps, opts));

// ---- the PD version: the golden vectors shared with the firmware and tools/pd_version.py -----------------------------------
test("PD versions match tests/fixtures/pd_versions.txt", () => {
  let n = 0;
  for (const line of readFileSync(path.join(root, "tests/fixtures/pd_versions.txt"), "utf8").split("\n")) {
    if (!line.trim() || line.trim().startsWith("#")) continue;
    const [wire, rmdo, offer, want, long] = line.split("|").map((f) => f.trim());
    const ev = { wire_revision_code: +wire, source_rmdo: rmdo === "-" ? null : { revision: rmdo },
                 advertised: { pps: offer.includes("p"), epr_table: offer.includes("e"), spr_avs: offer.includes("s") } };
    const v = CtGrid.pdVersion(ev);
    assert.equal(v.label || "-", want, line);
    assert.equal(v.conflict || "-", long, line);
    n++;
  }
  assert.ok(n >= 20);
});
test("PD version line: the answer, the marked proof with both said, nothing without PD", () => {
  const ev = (rmdo, adv) => ({ wire_revision_code: 2, source_rmdo: rmdo ? { revision: rmdo } : null, advertised: adv });
  assert.equal(CtGrid.pdVersionText(ev(null, { pps: true })), "PD3.0");
  assert.equal(CtGrid.pdVersionText(ev("3.1", { spr_avs: true })), "PD3.2* (reports Revision 3.1, offers SPR AVS (3.2))");
  assert.equal(CtGrid.pdVersionText(ev("3.2", { spr_avs: true })), "PD3.2");
  assert.equal(CtGrid.pdVersionText({ wire_revision_code: 0, source_rmdo: null, advertised: {} }), "");
  assert.equal(CtGrid.pdVersionText(null), "");
});

// ---- step labels: the golden vectors shared with the firmware ----------------------------------------------------------
test("step labels match tests/fixtures/step_labels.txt", () => {
  const WIRE = { pd2: "pd2-fixed", pd3: "pd3x-fixed", epr: "epr-fixed", pd30rev: "pd3.0-revision", pd31rev: "pd3.1-revision",
                 pd32rev: "pd3.2-revision", vooc: "vooc-5v-handshake", svooc: "svooc-5v-handshake", svooc65: "svooc65-5v-handshake" };
  let n = 0;
  for (const line of readFileSync(path.join(root, "tests/fixtures/step_labels.txt"), "utf8").split("\n")) {
    if (!line.trim() || line.trim().startsWith("#")) continue;
    const [alg, pos, mv, ma, pdo, flags, prevMv, want] = line.split("|").map((f) => f.trim());
    const row = { protocol: WIRE[alg] || alg, pos: +pos, target_mv: +mv, request_ma: +ma, raw_pdo: parseInt(pdo, 16),
                  continuous: flags.includes("c"), avs: flags.includes("a"), mi: flags.includes("h") };
    const prev = +prevMv ? { protocol: row.protocol, pos: row.pos, target_mv: +prevMv, request_ma: 1000, raw_pdo: row.raw_pdo, continuous: false } : null;
    assert.equal(CtGrid.stepLabel(row, prev), want, line);
    n++;
  }
  assert.ok(n > 40);
});

test("an APDO decodes with unsigned shifts (the kind bits are the top two)", () => {
  assert.equal(CtGrid.pdoRange(0xc1a42164), "3.3-21V 5A");
  assert.equal(CtGrid.pdoRange(0xd230968c), "15-28V 5A");
  assert.equal(CtGrid.pdoRange(0xe004b145), "9-20V 3.25A");
  assert.equal(CtGrid.pdoRange(0x0001912c), "");
  assert.equal(CtGrid.pdoMa(0xc1a42164), 5000);
});

// ---- the layout: the Baseus run, as the device's finished Results grid (fold) --------------------------------------------
test("the finished grid folds the silent and the skipped units into word rows", () => {
  assert.deepEqual(lines(baseus(), { fold: true }), [
    "PD: 5V/3A:pass 9V/3A:pass 12V/3A:pass 15V/3A:pass 20V/3.35A:pass",   // one PD block: the fixed levels, then a range chip per line
    ": PPS 5-11V/5A:partial",                                             // (the continuation lines carry no label)
    ": PPS 5-21V/3.2A:pass",
    "No reply: FCP:none UFCS:none",                                        // the kinds the source never offered are dropped, no "None" row
    "Skipped: QC2:none QC3:none",
  ]);
});

// A voltage fold is a caution: a pass with the reason "output folds back" tallies with the capped rows, so its cell is partial.
test("a folded pass reads partial in the grid, a clean one stays pass", () => {
  const steps = baseus();
  const row = steps.find((s) => s.algorithm === "pd3x-fixed" && s.target_mv === 20000);
  assert.ok(row && row.result === "pass");
  const clean = lines(steps, { fold: true })[0];
  assert.match(clean, /20V\/3\.35A:pass/);
  row.reason = "output folds back";
  const folded = lines(steps, { fold: true })[0];
  assert.match(folded, /20V\/3\.35A:partial/);
  assert.match(folded, /15V\/3A:pass/);   // its neighbours keep their verdict
});

test("sections: a 2 px rule where the unit's section changes, never on the first line", () => {
  const got = lines(baseus(), { fold: false });
  assert.ok(!got[0].startsWith("-- "));
  // PD -> QC (QC2) -> Huawei (FCP) -> QC (UFCS): three rules
  assert.equal(got.filter((l) => l.startsWith("-- ")).length, 3);
  assert.ok(got.find((l) => l.startsWith("-- QC2:")));
  assert.ok(got.find((l) => l.startsWith("-- FCP:")));
  assert.ok(got.find((l) => l.startsWith("-- UFCS:")));
  assert.ok(!got.find((l) => l.startsWith("-- QC3:")));   // QC2 and QC3 share a section
});

test("running grid keeps every unit in its place; an unoffered mode never shows red", () => {
  const got = lines(baseus(), { fold: false, absentCell: true });
  assert.ok(!got.some((l) => /None|EPR/.test(l)), "an unoffered PD kind has no row, no chip and no word");
  assert.equal(got.filter((l) => /^PD:/.test(l)).length, 1);   // PD is one block: its label on the first line only
  assert.ok(got.some((l) => /^QC2: PD active:none$/.test(l.replace("-- ", ""))));
  assert.ok(got.some((l) => /^FCP: no reply:none$/.test(l.replace("-- ", ""))));    // failed at 5 V: the charger does not speak it
  assert.ok(got.some((l) => /^UFCS: no reply:none$/.test(l.replace("-- ", ""))));
  // without the collapse the device's own chips stay: the failed 5 V, the skipped 9 V
  const plain = lines(baseus(), { fold: false });
  assert.ok(plain.some((l) => /^FCP: 5V:fail 9V:skip$/.test(l.replace("-- ", ""))));
});

test("a lone cell that never ran spells its reason", () => {
  const s = [S("pps", 5000, "skip", { pos: 6, pdo: 3248763456, reason: "needs load" }),
             S("pps", 21000, "skip", { pos: 6, pdo: 3248763456, reason: "needs load" }),
             S("qc3+", 5000, "pass"), S("qc3+", 9000, "skip", { reason: "no QC3+ response" }),
             S("qc2", 5000, "skip", { reason: "not implemented" })];
  assert.deepEqual(lines(s, { fold: false }), ["PD: PPS needs load:none", "-- QC3+: no QC3+:none", "QC2: n/a:none"]);
});

test("an older firmware's 6 A probes are chips of the one Xiaomi row, by their watts", () => {
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }),
             S("pd3x-fixed", 20000, "pass", { pos: 5, pdo: 0x6412c, ma: 6000 }),
             S("pd3x-fixed", 20000, "skip", { pos: 5, pdo: 0x6412c, ma: 6000, reason: "not unlocked", detail: 157 })];
  const got = lines(s, { fold: false });
  assert.deepEqual(got, ["PD: 5V/3A:pass", "-- Xiaomi: 120W:pass 120W:skip"]);
});

test("the Xiaomi unit is one row: the chain outcome, then each unlocked mode by its watts", () => {
  const pd = S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c });
  const chain = (result, detail, reason) => S("pd3x-fixed", 0, result, { pos: 0, detail, reason: reason || "none", mi: true });
  const mode = (result, ma, extra) => S("pps", 11000, result, Object.assign({ pos: 6, pdo: 0xC0DC3264, ma, mi: true }, extra || {}));
  // a) the 20K bank: PPS 5-11V 3A -> 5A
  assert.deepEqual(lines([pd, chain("pass", 20), mode("pass", 5000)], { fold: false }), ["PD: 5V/3A:pass", "-- Xiaomi: 55W:pass"]);
  // b) FH22: done, nothing raised: one passed auth chip
  assert.deepEqual(lines([pd, chain("pass", 20)], { fold: false })[1], "-- Xiaomi: auth:pass");
  // c) CUKTECH on a 6 A cable: the 66 W mode tested
  assert.deepEqual(lines([pd, chain("pass", 21), mode("pass", 6000)], { fold: false })[1], "-- Xiaomi: 66W:pass");
  // no SVID, no answer, stalled: one skipped auth chip (the reason stays on the steps page)
  for (const code of [1, 2, 5]) assert.deepEqual(lines([pd, chain("skip", code, "Xiaomi chain")], { fold: false })[1], "-- Xiaomi: auth:skip");
  // still waiting during a run: the pending chip
  assert.deepEqual(lines([pd, chain("pending", 0)], { fold: false })[1], "-- Xiaomi: auth:pending");
  // the flag decides: a plain full-current row that asks 6 A (an over-5 A cable) stays a PPS row
  const plain = S("pps", 11000, "pass", { pos: 6, pdo: 0xC0DC3278, ma: 6000, mi: false });
  assert.equal(CtGrid.stepLabel(plain, null), "PPS 11V 6A");
});

test("a deep sweep is one range cell under its own Sweep label", () => {
  const s = [S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 7, pdo: 3248763456 }),
             S("pps", 5000, "pending", { pos: 7, pdo: 3248763456, detail: 161, sweep: true })];
  assert.deepEqual(lines(s, { fold: false, monitorMv: 36000 }), ["PD: PPS 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:pending"]);
});

// Deep checks fold into the range chip as a caution (result_layout caution / step_caution): a walk the source did not follow at its
// quantum (Reason::StepResolution, detail = the measured move) or a transition that broke a limit (Reason::ProgCheck, detail = flag bits).
test("a step-walk miss or a transition flag turns the range chip amber and says why", () => {
  const pdo = 3248763456;
  const level = (mv, extra) => S("pps", mv, "pass", { pos: 7, pdo, ...extra });
  const clean = [level(5000), level(21000)];
  const chip = (steps) => CtGrid.layout(steps, { fold: false })[0].cells.find((c) => c.label.startsWith("PPS"));
  assert.equal(chip(clean).verdict, "pass");
  const walk = chip([...clean, level(11000, { reason: "step resolution", detail: 98 })]);
  assert.equal(walk.verdict, "partial");
  assert.match(walk.title, /partial \(steps 100mV, not 20mV\)/);
  assert.match(chip([...clean, level(11000, { reason: "step resolution", detail: 0 })]).title, /\(steps ignored\)/);
  const avs = S("spr-avs", 15000, "pass", { pos: 2, pdo: 3248763456, reason: "step resolution", detail: 100 });
  assert.match(CtGrid.layout([avs], { fold: false })[0].cells[0].title, /steps 100mV, not 100mV/);
  const flagged = chip([level(5000), level(21000, { reason: "transition check", detail: 3 })]);
  assert.equal(flagged.verdict, "partial");
  assert.match(flagged.title, /\(overshoot \+1\)/);
  // a failed level beside a caution: the chip is amber (some reached) with the fail's own reason, as the roll-up says for any mix
  assert.equal(chip([level(5000, { reason: "transition check", detail: 1 }), S("pps", 21000, "fail", { pos: 7, pdo, reason: "request failed" })]).verdict, "partial");
});

// The firmware's sweep chip (src/chargertest/result_layout.cpp sweep_row, 33ecb488): a sweep carrying the fold caution is amber at any depth.
// The wire rows carry no per-point error (no track_err), so the web grades by the fold flag alone: amber, never green, never red for the fold.
test("a deep sweep that folds back is an amber chip, not a green one", () => {
  const swp = (extra) => S("pps", 5000, "pass", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, ...extra });
  const base = [S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 7, pdo: 3248763456 })];
  const opts = { fold: false, monitorMv: 36000 };
  assert.deepEqual(lines([...base, swp({ reason: "output folds back" })], opts), ["PD: PPS 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:partial"]);
  // The caution on any part of a split sweep (the part from 5 V up, the part below) colours the one chip.
  const low = S("pps", 5000, "pass", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, reason: "output folds back" });
  assert.deepEqual(lines([...base, swp({}), low], opts), ["PD: PPS 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:partial"]);
  // No caution keeps the plain grade; a failed sweep stays red and a fold does not hide it.
  assert.deepEqual(lines([...base, swp({})], opts), ["PD: PPS 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:pass"]);
  assert.deepEqual(lines([...base, S("pps", 5000, "fail", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, reason: "output folds back" })], opts), ["PD: PPS 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:fail"]);
});

// A PPS sweep split at 5 V is two steps (the part from 5 V up, the part below it, PdLow) in ONE strip row. The firmware reads the
// two together (sweep_row: Fail when either failed, Pending while either is pending, Skip only when every part skipped, amber when
// either carries the fold caution); the web must not take the verdict of the first part alone.
test("a split deep sweep is graded over both parts, a failure in the second half is red", () => {
  const part = (result, extra) => S("pps", 5000, result, { pos: 7, pdo: 3248763456, detail: 161, sweep: true, ...extra });
  const base = [S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 7, pdo: 3248763456 })];
  const opts = { fold: false, monitorMv: 36000 };
  const sweep = (...parts) => lines([...base, ...parts], opts)[1];
  assert.equal(sweep(part("pass"), part("fail", { reason: "output folds back" })), "Sweep: 5-21V/3.2A:fail");
  assert.equal(sweep(part("pass"), part("fail")), "Sweep: 5-21V/3.2A:fail");
  assert.equal(sweep(part("fail"), part("pass")), "Sweep: 5-21V/3.2A:fail");
  assert.equal(sweep(part("pass"), part("pending")), "Sweep: 5-21V/3.2A:pending");
  assert.equal(sweep(part("pass"), part("running")), "Sweep: 5-21V/3.2A:pending");
  assert.equal(sweep(part("skip", { reason: "cable" }), part("pass")), "Sweep: 5-21V/3.2A:pass");
  assert.equal(sweep(part("pass"), part("pass", { reason: "output folds back" })), "Sweep: 5-21V/3.2A:partial");
  assert.equal(sweep(part("pass"), part("pass")), "Sweep: 5-21V/3.2A:pass");
  // The tooltip names the part that failed, not the first part.
  const cell = CtGrid.layout([...base, part("pass"), part("fail", { reason: "request failed" })], opts)[1].cells[0];
  assert.match(cell.title, /fail \(request failed\)/);
  // The running dot still sits on the one chip while the second part runs.
  const run = CtGrid.layout([...base, part("pass"), part("running")], { ...opts, live: true, current: 3 });
  assert.equal(run[1].cells[0].verdict, "running");
});

test("legacy levels roll the up and down steps into one cell; a ramp that stops fails its level", () => {
  const s = [S("qc3", 5000, "pass"), S("qc3", 9000, "pass"), S("qc3", 20000, "fail"), S("qc3", 9000, "pass", { cont: true })];
  assert.deepEqual(lines(s, { fold: false }), ["QC3: 5V:pass 9V:pass 20V:fail"]);
  const v = [S("vooc", 5000, "pass", { detail: 14 })];
  assert.deepEqual(lines(v, { fold: false }), ["VOOC: handshake ok:pass"]);
  const u = [S("ufcs", 9000, "pass", { pdo: (3000 / 50 << 25) | (3000 / 50 << 18) | (200 << 9) | 50 })];
  assert.deepEqual(lines(u, { fold: false }), ["UFCS: 9V/27W:pass"]);   // OEM watts: the volts x the mode's 3 A, claimed
});

test("more than five cells wrap onto a continuation line without a label", () => {
  const s = [5, 6, 7, 8, 9, 10, 11].map((v) => S("qc3+", v * 1000, "pass"));
  assert.deepEqual(lines(s, { fold: false }), ["QC3+: 5V:pass 6V:pass 7V:pass 8V:pass 9V:pass", ": 10V:pass 11V:pass"]);
});

test("a long label takes a wider cell, so fewer fit on a line (the device's width rule)", () => {
  const s = [S("pps", 5000, "pass", { pos: 6, pdo: 3235656292 }), S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }),
             S("pps", 5000, "pass", { pos: 8, pdo: 0xc1a42164 })];
  // one range chip per APDO slot, each on a line of its own (one PD block: the label on the first line only)
  const blk = CtGrid.layout(s, { fold: false });
  assert.equal(blk.length, 3);
  assert.deepEqual(blk.map((l) => l.label), ["PD", "", ""]);
  assert.ok(blk.every((l) => l.cells.length === 1 && l.cells[0].label.startsWith("PPS ")));
  const many = [3, 4, 5, 6].map((v) => S("scp", v * 1000, "pass", { ma: 2250 }));
  // "4.5V" style labels stay narrow: the width rule only bites past 5 characters
  assert.equal(CtGrid.layout(many, { fold: false }).length, 1);
  const wide = [9, 10, 11, 12].map((v) => S("qc3+", v * 1000 + 500, "pass"));   // "9.5V" .. "12.5V": 5 glyphs wide -> 3 per line
  assert.equal(CtGrid.layout(wide, { fold: false }).length, 2);
  assert.deepEqual(CtGrid.layout(wide, { fold: false }).map((l) => l.cells.length), [3, 1]);
});

test("the running dot sits on the pending cell of the step under test; stopped runs lose it", () => {
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }), S("pd3x-fixed", 9000, "pending", { pos: 2, pdo: 0x2d12c }),
             S("pd3x-fixed", 12000, "pending", { pos: 3, pdo: 0x3c12c })];
  assert.deepEqual(lines(s, { fold: false, live: true, current: 1 }), ["PD: 5V/3A:pass 9V/3A:running 12V/3A:pending"]);
  assert.deepEqual(lines(s, { fold: false, live: false, current: 1 }), ["PD: 5V/3A:pass 9V/3A:pending 12V/3A:pending"]);
  assert.deepEqual(lines(s, { fold: false, stopped: true }), ["PD: 5V/3A:pass 9V/3A:stopped 12V/3A:stopped"]);
  // a probe step has a row of its own: the dot never lands on the fixed row
  const p = s.concat([S("pd3x-fixed", 20000, "pending", { pos: 5, pdo: 0x6412c, ma: 6000 })]);
  const got = lines(p, { fold: false, live: true, current: 3 });
  assert.equal(got[0], "PD: 5V/3A:pass 9V/3A:pending 12V/3A:pending");
  assert.equal(got[1], "-- Xiaomi: 120W:running");
});

test("revision rows are no grid rows", () => {
  const s = [S("pd3.1-revision", 0, "pass"), S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c })];
  assert.deepEqual(lines(s, { fold: false }), ["PD: 5V/3A:pass"]);
});

test("a row's tooltip says why: the skip reason or the measured voltage", () => {
  const rows = CtGrid.layout(baseus(), { fold: false });
  const pps = rows.find((r) => r.cells[0].label === "PPS 5-11V");
  assert.match(pps.cells[0].title, /PPS 5-11V 5A/);
});

// ---- the PD block: one label, fixed levels first, then a chip per PPS / AVS range ----------------------------------------------
test("an EPR source: levels that ran stay volt chips, a skipped EPR level or AVS range is dropped, AVS is a range chip", () => {
  const apdo = 0xd230968c;
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }), S("pd3x-fixed", 20000, "pass", { pos: 5, pdo: 0x6412c }),
             S("epr", 28000, "pass", { pos: 6, pdo: 573940 }), S("epr", 36000, "skip", { pos: 7, pdo: 573940, reason: "over voltage cap", detail: 144 }),
             S("epr-avs", 15000, "pass", { pos: 8, pdo: apdo }), S("epr-avs", 28000, "pass", { pos: 8, pdo: apdo })];
  assert.deepEqual(lines(s, { fold: true }), ["PD: 5V/3A:pass 20V/3A:pass 28V/5A:pass", ": AVS 15-28V/5A:pass"]);
  // the running grid keeps the skipped level (its reason is a tag, the steps page says it)
  assert.deepEqual(lines(s, { fold: false }), ["PD: 5V/3A:pass 20V/3A:pass 28V/5A:pass 36V/5A:skip", ": AVS 15-28V/5A:pass"]);
  // an EPR AVS range that was only skipped has no chip at all
  const refused = s.slice(0, 4).concat([S("epr-avs", 15000, "skip", { pos: 8, pdo: apdo, reason: "EPR entry refused" })]);
  assert.deepEqual(lines(refused, { fold: true }), ["PD: 5V/3A:pass 20V/3A:pass 28V/5A:pass"]);
  // the tooltip names the kind
  assert.match(CtGrid.layout(s, { fold: true })[1].cells[0].title, /^AVS 15-28V 5A: pass/);
});

test("a source with no PD says it once: No reply (offered nothing) or Skipped (never ran)", () => {
  const none = [S("pd2-fixed", 0, "skip", { reason: NOOFFER }), S("epr", 0, "skip", { reason: NOOFFER }), S("pps", 0, "skip", { reason: NOOFFER })];
  assert.deepEqual(lines(none, { fold: true }), ["No reply: PD:none"]);
  const idle = [S("pd3x-fixed", 0, "skip", { reason: "no contract" }), S("pps", 0, "skip", { reason: "no contract" })];
  assert.deepEqual(lines(idle, { fold: true }), ["Skipped: PD:none"]);
});

test("a PD2.0 fixed-only source is one row of volt chips", () => {
  const s = [5, 9, 12].map((v, i) => S("pd2-fixed", v * 1000, "pass", { pos: i + 1, pdo: (v * 20 << 10) | 150 })).concat([S("epr", 0, "skip", { reason: NOOFFER })]);
  assert.deepEqual(lines(s, { fold: true }), ["PD: 5V/1.5A:pass 9V/1.5A:pass 12V/1.5A:pass"]);
});

test("the running dot lands on the range chip of the step under test", () => {
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }),
             S("pps", 5000, "pending", { pos: 6, pdo: 3235656292 }), S("pps", 11000, "pending", { pos: 6, pdo: 3235656292 }),
             S("pps", 5000, "pending", { pos: 7, pdo: 3248763456 })];
  assert.deepEqual(lines(s, { fold: false, live: true, current: 3 }), ["PD: 5V/3A:pass", ": PPS 5-11V/5A:pending", ": PPS 5-21V/3.2A:running"]);
  assert.deepEqual(lines(s, { fold: false, live: true, current: 1 })[1], ": PPS 5-11V/5A:running");
});

// ---- the kit ---------------------------------------------------------------------------------------------------------------
test("bracket runs: one per run of equal current, none where the current is empty", () => {
  const c = (l, cur) => ({ verdict: "pass", label: l, current: cur });
  assert.deepEqual(K2Kit.bracketRuns([c("5V", "3A"), c("9V", "3A"), c("12V", "3A"), c("15V", "3A"), c("20V", "3.25A")]),
    [{ from: 0, to: 3, text: "3A" }, { from: 4, to: 4, text: "3.25A" }]);
  assert.deepEqual(K2Kit.bracketRuns([c("QC2", ""), c("9V", undefined)]), []);
  assert.deepEqual(K2Kit.bracketRuns([c("5V", "3A"), c("9V", ""), c("12V", "3A")]),
    [{ from: 0, to: 0, text: "3A" }, { from: 2, to: 2, text: "3A" }]);
});

test("the grid html: label column, device verdict classes, brackets, rules, escaping", () => {
  const html = K2Kit.resultGridHtml([
    { label: "PD3.x", cells: [{ verdict: "pass", label: "5V", current: "3A" }, { verdict: "fail", label: "9V", current: "3A" },
                              { verdict: "skip", label: "12V", current: "3A" }, { verdict: "running", label: "15V", current: "3A" }] },
    { label: "QC3", section: true, cells: [{ verdict: "none", label: "<b>x</b>" }, { verdict: "stopped", label: "9V" },
                                         { verdict: "partial", label: "5-21V", current: "5A" }, { verdict: "pending", label: "5V" }] },
  ]);
  for (const k of ["k-pass", "k-fail", "k-skip", "k-running", "k-none", "k-stopped", "k-partial", "k-pending"])
    assert.ok(html.includes("k-vc " + k), k);
  assert.match(html, /class="k-rg-label">PD3\.x</);
  assert.match(html, /k-rg-row k-sec/);
  assert.equal((html.match(/k-rg-row k-sec/g) || []).length, 1);
  assert.match(html, /class="k-br"[^>]*>3A</);                // one bracket under the four 3 A cells
  assert.equal((html.match(/class="k-br"/g) || []).length, 2);  // the PD3.x run and the 5 A of the partial chip
  assert.ok(!html.includes("<b>x</b>") && html.includes("&lt;b&gt;x&lt;/b&gt;"));
  assert.match(html, /aria-label="9V fail"/);
  assert.ok(!html.includes("</script>"));
});

test("a strike is on the volts text only; a running cell carries a dot element, never an animation", () => {
  const html = K2Kit.verdictCellHtml({ verdict: "skip", label: "12V", current: "3A" });
  assert.match(html, /class="k-vl">12V</);
  const run = K2Kit.verdictCellHtml({ verdict: "running", label: "9V" });
  assert.match(run, /k-vc k-running/);
  const css = readFileSync(path.join(root, "tools/web/k2kit.css"), "utf8");
  assert.ok(!/animation|blink|@keyframes/.test(css));
  for (const tok of ["--k-ok", "--k-ok-t", "--k-error", "--k-caution", "--k-dim", "--k-faint", "--k-rule", "--k-focus"])
    assert.equal(css.split(tok + ":").length - 1, 2, tok + " is defined once for light and once for dark");
});

// ---- the embeds in k2.html (make web-sync) ------------------------------------------------------------------------------------
test("k2.html embeds the kit and the grid layout byte for byte", () => {
  const html = readFileSync(path.join(root, "tools/web/k2.html"), "utf8");
  for (const [tag, id, file] of [["script", "k2kit", "k2kit.js"], ["script", "ctgrid", "ctgrid.js"], ["style", "k2kit", "k2kit.css"]]) {
    const m = html.match(new RegExp(`<${tag} id="${id}">\\n([\\s\\S]*?)</${tag}>`));
    assert.ok(m, `k2.html has no <${tag} id="${id}"> block`);
    assert.equal(m[1], readFileSync(path.join(root, "tools/web", file), "utf8"), file + ": run make web-sync");
  }
});


// ---- the PD spec section joins the grid below the unit rows -----------------------------------------------------------------
test("the spec section is one wide cell per check, with rules above the family blocks", () => {
  const P = require("../tools/web/k2proto.js");
  const fx = JSON.parse(readFileSync(path.join(root, "tests/fixtures/ct_spec_section.json"), "utf8"));
  const sp = P.decodeChargerSpecSection(Uint8Array.from(Buffer.from(fx.wire, "hex")));
  const out = CtGrid.layout([], { spec: sp });
  assert.equal(out.length, 27);
  assert.deepEqual(out.filter((l) => l.section).map((l) => l.label), ["Accept", "PPS mono", "AVS mono", "Src rst"]);
  const cell = (i) => out[i].cells[0];
  assert.equal(out[0].cells.length, 1);
  assert.deepEqual([cell(0).verdict, cell(0).label], ["pass", "2.5 ms"]);
  assert.deepEqual([cell(1).verdict, cell(1).label], ["fail", "40.0 ms, 3 fail"]);
  assert.deepEqual([cell(2).verdict, cell(2).label], ["skip", "no PPS"]);
  assert.deepEqual([cell(3).verdict, cell(3).label], ["none", "1.200 V"]);
  assert.deepEqual([cell(5).verdict, cell(5).label], ["pending", ""]);
  assert.ok(out.every((l) => l.label.length <= 8));
  assert.ok(!/limit/.test(cell(0).title));   // the first form alone: no limit in the title
  // with the detail form (the device kept limit, count and place): the title carries them
  const full = P.decodeChargerSpecSection(Uint8Array.from(Buffer.from(fx.wire, "hex")), Uint8Array.from(Buffer.from(fx.detail, "hex")));
  const withDetail = CtGrid.layout([], { spec: full });
  assert.match(withDetail[0].cells[0].title, /limit <= 25\.0 ms · 12 measured · PDO 2 to 3/);
  assert.match(withDetail[1].cells[0].title, /limit <= 25\.0 ms · 300 measured/);
  assert.equal(withDetail[2].cells[0].title.includes("limit"), false);   // a skipped check keeps none
  // no section, a section that did not begin: no rows
  assert.deepEqual(CtGrid.layout([], {}), []);
  assert.deepEqual(CtGrid.specLines({ end: "not run", cells: [] }), []);
});

// Layout B (src/ui/ui_charger_test.cpp, a finished Deep run with its spec stage): a range line reads "PPS | 5-21V 3.2A | shape", the kind as
// its label and the range's spec word in a cell of its own, a rule above the block's first range line; a split kind's merged spec block
// leaves the check lines (its range lines carry it). While running, or with no spec stage, the range chip keeps its kind word.
test("layout B: each range line carries its spec word, and a split kind's merged block leaves the spec lines", () => {
  const P = require("../tools/web/k2proto.js");
  const fx = JSON.parse(readFileSync(path.join(root, "tests/fixtures/ct_spec_section.json"), "utf8"));
  const steps = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x0001912C }), S("pd3x-fixed", 9000, "pass", { pos: 2, pdo: 0x0002D12C }),
    S("pps", 5000, "pass", { pos: 5, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 5, pdo: 3248763456 }),
    S("spr-avs", 15000, "pass", { pos: 6, pdo: 3248763456 }), S("spr-avs", 15000, "pass", { pos: 9, pdo: 3248763456 })];
  const sp = P.decodeChargerSpecSection(Uint8Array.from(Buffer.from(fx.wire, "hex")));
  sp.split = { pps: true, avs: false };
  sp.ranges = [{ pos: 5, kind: "pps", tone: "fail", word: "shape", cells: [] }, { pos: 6, kind: "avs", tone: "caution", word: "timing", cells: [] },
               { pos: 9, kind: "avs", tone: "pass", word: "spec", cells: [] }];
  const got = lines(steps, { spec: sp });
  assert.deepEqual(got.slice(0, 4), ["PD: 5V/3A:pass 9V/3A:pass", "-- PPS: 5-21V/3.2A:pass shape:fail", "AVS: 5-21V/3.2A:pass timing:partial",
                                     "AVS: 5-21V/3.2A:pass spec:pass"]);
  const checks = got.slice(4);
  assert.equal(checks.length, 27 - 10);                                   // the PPS block (10 checks) left with the split
  assert.ok(!checks.some((l) => /PPS mono|PPS lvl|PPS trk/.test(l)));
  assert.ok(checks.some((l) => /^-- AVS mono:/.test(l)));
  // a range with no spec cells of its own: "-", no fill
  const none = lines(steps.slice(0, 4), { spec: Object.assign({}, sp, { ranges: [] }) });
  assert.equal(none[1], "-- PPS: 5-21V/3.2A:pass -:none");
  // running (live) or no spec stage: the plain chip
  assert.equal(lines(steps.slice(0, 4), { spec: sp, live: true })[1], ": PPS 5-21V/3.2A:pass");
  assert.equal(lines(steps.slice(0, 4), {})[1], ": PPS 5-21V/3.2A:pass");
  assert.equal(lines(steps.slice(0, 4), { spec: { end: "not run", cells: [] } })[1], ": PPS 5-21V/3.2A:pass");
  // the kit draws it: the bracket under the chip only, the word as a verdict cell of its own
  const html = K2Kit.resultGridHtml([CtGrid.layout(steps, { spec: sp })[1]]);
  assert.match(html, /class="k-rg-label">PPS</);
  assert.match(html, /class="k-br" style="grid-column:1 \/ 2">3\.2A</);
  assert.equal((html.match(/class="k-br"/g) || []).length, 1);
  assert.match(html, /k-vc k-fail[^>]*grid-column:2/);
  assert.match(html, /aria-label="shape fail"/);
});

// ---- OEM levels by their watts: the golden vectors shared with the firmware and tools/oem_watts.py ----------------------
test("OEM watts match tests/fixtures/oem_watts.txt", () => {
  let labels = 0, claims = 0;
  for (const line of readFileSync(path.join(root, "tests/fixtures/oem_watts.txt"), "utf8").split("\n")) {
    if (!line.trim() || line.trim().startsWith("#")) continue;
    const f = line.split("|").map((x) => x.trim());
    if (f[0] === "label") { assert.equal(CtGrid.oemLabel(f[1], +f[2]), f[3], line); labels++; }
    if (f[0] === "claim") {
      const level = { word: "X", w: +f[4], ceiling: f[5] === "y" };
      assert.equal(CtGrid.oemClaim(level, { end: f[1], vmax_mv: +f[2], word: parseInt(f[3], 16) }), f[6], line);
      claims++;
    }
  }
  assert.ok(labels >= 10 && claims >= 12);
});

test("the level table matches the firmware's", () => {
  const src = readFileSync(path.join(root, "include/k2/chargertest/oem_watts.hpp"), "utf8");
  for (const l of CtGrid.OEM_LEVELS) assert.ok(src.includes(`{Family::Vooc, "${l.word}", ${l.mv}, ${l.ma}, ${l.w}, ${l.ceiling}},`), l.word + l.w);
});

test("the finished grid reads the VOOC family as watts levels", () => {
  const fam = ["vooc", "svooc", "svooc65"].map((p) => S(p, 5000, "pass", { detail: 15 }));
  // Medium: the detection alone, every level not tested (struck)
  assert.deepEqual(lines(fam, { fold: true, oemWatts: true }),
    ["VOOC: detected:pass 20W:skip", "SVOOC: 50W:skip 65W:skip 100W:skip 120W:skip"]);
  // Deep, the hardware ceiling: 0x06D under 11.02 V = 120 W claimed
  const sv = { end: "stopped", word: 0x06D, vmax_mv: 11020 };
  assert.deepEqual(lines(fam, { fold: true, oemWatts: true, svooc: sv }),
    ["VOOC: detected:pass 20W:skip", "SVOOC: 50W:unverified 65W:unverified 100W:unverified 120W:unverified"]);
  // 69 W: the levels above it not reached
  assert.deepEqual(lines(fam, { fold: true, oemWatts: true, svooc: { end: "judged", word: 0x03F, vmax_mv: 11000 } })[1],
    "SVOOC: 50W:unverified 65W:unverified 100W:none 120W:none");
  const cell = CtGrid.layout(fam, { fold: true, oemWatts: true, svooc: sv })[1].cells[3];
  assert.equal(cell.title, "SVOOC 120W: claimed (no load)");
  // without the option (the Running grid): the plan's unit rows
  assert.equal(lines(fam, { fold: false }).length, 3);
});

test("a VOOC family that was skipped as 'no answer' folds as No reply, not Skipped", () => {
  const row = (alg) => ({ ...S(alg, 5000, "skip", { reason: "request failed", detail: 13 }), detail_reason: "no answer" });
  const got = lines([row("vooc"), row("svooc"), row("svooc65"), S("fcp", 5000, "fail")], { fold: true, oemWatts: true });
  assert.ok(got.some((l) => l === "No reply: VOOC:none FCP:none"), got.join(" | "));
  assert.ok(!got.some((l) => /^Skipped/.test(l)), got.join(" | "));
});

test("a silent VIVO / UFCS / AFC unit (skipped, no first answer) folds as No reply; a heard one stays a fail row", () => {
  const row = (alg, mv, detail_reason) => ({ ...S(alg, mv, "skip", { reason: "request failed", detail: 7 }), detail_reason });
  const got = lines([row("vivo", 5000, "no response"), row("vivo", 9000, "no response"), row("afc", 5000, "unsupported"), S("fcp", 5000, "fail")], { fold: true });
  assert.ok(got.some((l) => /^No reply: .*VIVO/.test(l) && /AFC/.test(l)), got.join(" | "));
  assert.ok(!got.some((l) => /^Skipped/.test(l)), got.join(" | "));
  const heard = lines([S("vivo", 5000, "pass"), { ...S("vivo", 9000, "fail", { reason: "request failed", detail: 7 }), detail_reason: "no response" }], { fold: true });
  assert.ok(!heard.some((l) => /^No reply/.test(l)), heard.join(" | "));
});

test("a VOOC family that never answered folds as one word", () => {
  const fam = [S("vooc", 5000, "fail", { detail: 13 }), S("svooc", 5000, "skip", { reason: "request failed" }),
               S("svooc65", 5000, "skip", { reason: "request failed" }), S("fcp", 5000, "fail")];
  const got = lines(fam, { fold: true, oemWatts: true });
  assert.ok(got.some((l) => l === "No reply: VOOC:none FCP:none"), got.join(" | "));
  assert.ok(!got.some((l) => l.includes("SVOOC")), got.join(" | "));
});

// The engine's own words ride a skipped sibling too (the family shares the first row's detail): the title says "no answer",
// never the bare reason "request failed" the unit never produced itself.
test("a skipped VOOC sibling reads the first row's detail, not 'request failed'", () => {
  const nl = { detail_reason: "no answer" };
  const row = (alg, res) => ({ ...S(alg, 5000, res, { reason: REQ, detail: 13 }), detail_reason: nl.detail_reason });
  const fam = [row("vooc", "fail"), row("svooc", "skip"), row("svooc65", "skip")];
  const cells = CtGrid.layout(fam, { fold: false }).flatMap((r) => r.cells);
  assert.equal(cells.length, 3);
  for (const c of cells) assert.ok(c.title.includes("no answer") && !c.title.includes("request failed"), c.title);
  // a plain skip with no engine detail keeps its reason
  const plain = CtGrid.layout([S("pps", 8000, "skip", { pos: 6, pdo: 3235656292, reason: REQ })], { fold: false })[0].cells[0];
  assert.match(plain.title, /request failed/);
});

test("SCP and UFCS chips read the level's claimed watts", () => {
  const s = [S("scp", 5000, "pass"), S("scp", 10000, "pass", { ma: 2250 })];
  assert.deepEqual(lines(s, { fold: false }), ["SCP: 5V:pass 10V/22.5W:pass"]);
  assert.equal(CtGrid.oemWatts(110250), "110W");
});
