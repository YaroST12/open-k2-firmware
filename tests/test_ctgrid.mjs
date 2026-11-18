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
    "PD3.x: 5V/3A:pass 9V/3A:pass 12V/3A:pass 15V/3A:pass 20V/3.35A:pass",
    "SPR AVS: None:none",
    "PPS: 5-11V/5A:partial",
    "PPS: 5-21V/3.2A:pass",
    "EPR AVS: None:none",
    "No reply: EPR:none FCP:none UFCS:none",
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
  assert.ok(got.includes("EPR AVS: None:none"));
  assert.ok(!got.some((l) => /^(-- )?EPR:/.test(l)), "an unoffered PD unit has no row of its own");
  assert.ok(got.includes("No reply: EPR:none"));
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
  assert.deepEqual(lines(s, { fold: false }), ["PPS: needs load:none", "-- QC3+: no QC3+:none", "QC2: n/a:none"]);
});

test("an older firmware's 6 A probes are chips of the one Xiaomi row, by their watts", () => {
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }),
             S("pd3x-fixed", 20000, "pass", { pos: 5, pdo: 0x6412c, ma: 6000 }),
             S("pd3x-fixed", 20000, "skip", { pos: 5, pdo: 0x6412c, ma: 6000, reason: "not unlocked", detail: 157 })];
  const got = lines(s, { fold: false });
  assert.deepEqual(got, ["PD3.x: 5V/3A:pass", "-- Xiaomi: 120W:pass 120W:skip"]);
});

test("the Xiaomi unit is one row: the chain outcome, then each unlocked mode by its watts", () => {
  const pd = S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c });
  const chain = (result, detail, reason) => S("pd3x-fixed", 0, result, { pos: 0, detail, reason: reason || "none", mi: true });
  const mode = (result, ma, extra) => S("pps", 11000, result, Object.assign({ pos: 6, pdo: 0xC0DC3264, ma, mi: true }, extra || {}));
  // a) the 20K bank: PPS 5-11V 3A -> 5A
  assert.deepEqual(lines([pd, chain("pass", 20), mode("pass", 5000)], { fold: false }), ["PD3.x: 5V/3A:pass", "-- Xiaomi: auth:pass 55W:pass"]);
  // b) FH22: done, nothing raised
  assert.deepEqual(lines([pd, chain("pass", 20)], { fold: false })[1], "-- Xiaomi: auth:pass none new:skip");
  // c) CUKTECH on a 6 A cable: the 66 W mode tested
  assert.deepEqual(lines([pd, chain("pass", 21), mode("pass", 6000)], { fold: false })[1], "-- Xiaomi: auth:pass 66W:pass");
  // no SVID: the chain row skipped, a word
  assert.deepEqual(lines([pd, chain("skip", 1, "Xiaomi chain")], { fold: false })[1], "-- Xiaomi: no SVID:skip");
  // the flag decides: a plain full-current row that asks 6 A (an over-5 A cable) stays a PPS row
  const plain = S("pps", 11000, "pass", { pos: 6, pdo: 0xC0DC3278, ma: 6000, mi: false });
  assert.equal(CtGrid.stepLabel(plain, null), "PPS 11V 6A");
});

test("a deep sweep is one range cell under its own Sweep label", () => {
  const s = [S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 7, pdo: 3248763456 }),
             S("pps", 5000, "pending", { pos: 7, pdo: 3248763456, detail: 161, sweep: true })];
  assert.deepEqual(lines(s, { fold: false, monitorMv: 36000 }), ["PPS: 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:pending"]);
});

// The firmware's sweep chip (src/chargertest/result_layout.cpp sweep_row, 33ecb488): a sweep carrying the fold caution is amber at any depth.
// The wire rows carry no per-point error (no track_err), so the web grades by the fold flag alone: amber, never green, never red for the fold.
test("a deep sweep that folds back is an amber chip, not a green one", () => {
  const swp = (extra) => S("pps", 5000, "pass", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, ...extra });
  const base = [S("pps", 5000, "pass", { pos: 7, pdo: 3248763456 }), S("pps", 21000, "pass", { pos: 7, pdo: 3248763456 })];
  const opts = { fold: false, monitorMv: 36000 };
  assert.deepEqual(lines([...base, swp({ reason: "output folds back" })], opts), ["PPS: 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:partial"]);
  // The caution on any part of a split sweep (the part from 5 V up, the part below) colours the one chip.
  const low = S("pps", 5000, "pass", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, reason: "output folds back" });
  assert.deepEqual(lines([...base, swp({}), low], opts), ["PPS: 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:partial"]);
  // No caution keeps the plain grade; a failed sweep stays red and a fold does not hide it.
  assert.deepEqual(lines([...base, swp({})], opts), ["PPS: 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:pass"]);
  assert.deepEqual(lines([...base, S("pps", 5000, "fail", { pos: 7, pdo: 3248763456, detail: 161, sweep: true, reason: "output folds back" })], opts), ["PPS: 5-21V/3.2A:pass", "Sweep: 5-21V/3.2A:fail"]);
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
  // one range per APDO slot, each its own row: never joined onto one line
  assert.equal(CtGrid.layout(s, { fold: false }).length, 3);
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
  assert.deepEqual(lines(s, { fold: false, live: true, current: 1 }), ["PD3.x: 5V/3A:pass 9V/3A:running 12V/3A:pending"]);
  assert.deepEqual(lines(s, { fold: false, live: false, current: 1 }), ["PD3.x: 5V/3A:pass 9V/3A:pending 12V/3A:pending"]);
  assert.deepEqual(lines(s, { fold: false, stopped: true }), ["PD3.x: 5V/3A:pass 9V/3A:stopped 12V/3A:stopped"]);
  // a probe step has a row of its own: the dot never lands on the fixed row
  const p = s.concat([S("pd3x-fixed", 20000, "pending", { pos: 5, pdo: 0x6412c, ma: 6000 })]);
  const got = lines(p, { fold: false, live: true, current: 3 });
  assert.equal(got[0], "PD3.x: 5V/3A:pass 9V/3A:pending 12V/3A:pending");
  assert.equal(got[1], "-- Xiaomi: 120W:running");
});

test("revision rows are no grid rows", () => {
  const s = [S("pd3.1-revision", 0, "pass"), S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c })];
  assert.deepEqual(lines(s, { fold: false }), ["PD3.x: 5V/3A:pass"]);
});

test("a row's tooltip says why: the skip reason or the measured voltage", () => {
  const rows = CtGrid.layout(baseus(), { fold: false });
  const pps = rows.find((r) => r.label === "PPS");
  assert.match(pps.cells[0].title, /5-11V 5A/);
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

test("a VOOC family that never answered folds as one word", () => {
  const fam = [S("vooc", 5000, "fail", { detail: 13 }), S("svooc", 5000, "skip", { reason: "request failed" }),
               S("svooc65", 5000, "skip", { reason: "request failed" }), S("fcp", 5000, "fail")];
  const got = lines(fam, { fold: true, oemWatts: true });
  assert.ok(got.some((l) => l === "No reply: VOOC:none FCP:none"), got.join(" | "));
  assert.ok(!got.some((l) => l.includes("SVOOC")), got.join(" | "));
});

test("SCP and UFCS chips read the level's claimed watts", () => {
  const s = [S("scp", 5000, "pass"), S("scp", 10000, "pass", { ma: 2250 })];
  assert.deepEqual(lines(s, { fold: false }), ["SCP: 5V:pass 10V/22.5W:pass"]);
  assert.equal(CtGrid.oemWatts(110250), "110W");
});
