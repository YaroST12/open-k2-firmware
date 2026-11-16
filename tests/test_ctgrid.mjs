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
                  continuous: flags.includes("c"), avs: flags.includes("a") };
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

test("a 6 A probe is a Mi 6A row of its own, the current under the volts", () => {
  const s = [S("pd3x-fixed", 5000, "pass", { pos: 1, pdo: 0x1912c }),
             S("pd3x-fixed", 20000, "pass", { pos: 5, pdo: 0x6412c, ma: 6000 }),
             S("pd3x-fixed", 20000, "skip", { pos: 5, pdo: 0x6412c, ma: 6000, reason: "not unlocked", detail: 157 })];
  const got = lines(s, { fold: false });
  assert.deepEqual(got, ["PD3.x: 5V/3A:pass", "-- Mi 6A: 20V/6A:pass 20V/6A:skip"].map((l) => l));
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

test("legacy levels roll the up and down steps into one cell; a ramp that stops fails its level", () => {
  const s = [S("qc3", 5000, "pass"), S("qc3", 9000, "pass"), S("qc3", 20000, "fail"), S("qc3", 9000, "pass", { cont: true })];
  assert.deepEqual(lines(s, { fold: false }), ["QC3: 5V:pass 9V:pass 20V:fail"]);
  const v = [S("vooc", 5000, "pass", { detail: 14 })];
  assert.deepEqual(lines(v, { fold: false }), ["VOOC: handshake ok:pass"]);
  const u = [S("ufcs", 9000, "pass", { pdo: (3000 / 50 << 25) | (3000 / 50 << 18) | (200 << 9) | 50 })];
  assert.deepEqual(lines(u, { fold: false }), ["UFCS: 9V/3A:pass"]);
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
  assert.equal(got[1], "-- Mi 6A: 20V/6A:running");
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
