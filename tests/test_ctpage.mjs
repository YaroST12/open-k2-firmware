// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The dashboard's Charger test tab in a real browser against a fake K2 (tests/support/fakek2.js): the ResultGrid shows
// labelled cells from rows the page reads itself, paced and once, the results card does not repeat it, Cancel works,
// and the tab bar fits a phone. Needs Chrome (K2_CHROME, ~/.local/bin/google-chrome, or one on PATH); skips without.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { findChrome, launch, startServer, sleep } from "./support/chrome.mjs";
import { baseus, withIndex } from "./support/ctfixture.mjs";

const skip = findChrome() ? false : "no chrome";
const PLAN = withIndex(baseus());
let srv, page;

before(async () => {
  if (skip) return;
  srv = await startServer();
  page = await launch({ width: 360, height: 800, mobile: true });
  await page.goto(srv.url + "#/test");
  await page.until('!document.getElementById("c-ctrun").hidden', 10000, "the Charger test run card");
  await page.eval("__fake.plan = " + JSON.stringify(PLAN));
});
after(async () => {
  if (page) await page.close();
  if (srv) await srv.close();
});

const $ = (id) => `document.getElementById(${JSON.stringify(id)})`;
const text = (id) => page.eval(`${$(id)}.textContent`);

test("the five tabs fit at 360 px, and the page does not scroll sideways", { skip }, async () => {
  const m = await page.eval(`(function () {
    var t = ${$("tabs")}, tabs = Array.prototype.slice.call(t.querySelectorAll("a")), W = document.documentElement.clientWidth;
    return { n: tabs.length, over: t.scrollWidth - t.clientWidth, page: document.documentElement.scrollWidth - W,
             cut: tabs.filter(function (a) { var r = a.getBoundingClientRect(); return r.left < 0 || r.right > W; }).length };
  })()`);
  assert.equal(m.n, 5);
  assert.ok(m.over <= 0, "tab bar scrolls by " + m.over + " px");
  assert.ok(m.page <= 0, "page is " + m.page + " px wider than the screen");
  assert.equal(m.cut, 0, "tabs outside the screen");
});

test("a run: every row is read once, the grid has labelled cells, nothing says 'not read yet'", { skip }, async () => {
  await page.eval(`${$("ctr-start")}.click()`);
  await page.eval(`${$("ctr-go")}.click()`);
  await page.until(`!${$("ctr-live")}.hidden && __fake.rowReads.length >= ${PLAN.length}`, 10000, "all rows read");
  await page.until(`${$("ctr-grid")}.querySelectorAll(".k-vc").length > 0`, 4000, "the grid");
  const reads = await page.eval("__fake.rowReads");
  assert.deepEqual([...reads].sort((a, b) => a - b), PLAN.map((_, i) => i), "each row read exactly once at the start");
  const g = await page.eval(`(function () {
    var cells = Array.prototype.slice.call(${$("ctr-grid")}.querySelectorAll(".k-vc"));
    return { labels: cells.map(function (c) { return c.textContent.trim(); }),
             dots: ${$("ctr-grid")}.querySelectorAll(".k-dot").length,
             rows: ${$("ctr-grid")}.querySelectorAll(".k-rg-row").length,
             grids: document.querySelectorAll(".k-rg").length,
             body: document.body.innerText };
  })()`);
  assert.ok(g.labels.length >= 10 && g.labels.every((t) => t.length > 0), "every cell carries its label: " + g.labels.join("|"));
  assert.ok(g.labels.includes("5V") && g.labels.includes("20V"), g.labels.join("|"));
  assert.ok(!/not read yet/i.test(g.body), "'not read yet' text is gone");
  assert.equal(g.dots, 1, "exactly one running dot");
  assert.equal(g.grids, 1, "one place shows the grid");
  assert.ok(await text("ctr-gridhead") === "", "no reading banner once the plan is read");
});

test("progress re-reads only the rows that can still change", { skip }, async () => {
  await page.eval("__fake.advance(2)");
  await page.until('__fake.rowReads.length > ' + PLAN.length, 8000, "rows re-read");
  await sleep(2600);   // two more ticks: nothing else may be read
  const n = await page.eval("__fake.rowReads.length");
  const extra = n - PLAN.length;
  assert.ok(extra >= 1 && extra <= 3, "re-read " + extra + " rows after 2 steps; only rows up to the current step may be read");
  const done = await page.eval(`${$("ctr-grid")}.querySelectorAll(".k-pass").length`);
  assert.equal(done, 2, "the two finished steps show as pass cells");
  assert.equal(await page.eval(`${$("ctr-grid")}.querySelectorAll(".k-dot").length`), 1, "the dot moved to the step under test");
});

test("a finished run: one summary, the grid stays, the results card keeps the evidence table", { skip }, async () => {
  await page.eval("__fake.end('done')");
  await page.until(`${$("ctr-headline")}.textContent.indexOf("complete") >= 0 && ${$("ctr-summary")}.textContent.indexOf("pass") >= 0`,
                   12000, "the done headline and summary");
  const d = await page.eval(`(function () {
    var cells = ${$("ctr-grid")}.querySelectorAll(".k-vc");
    return { dots: ${$("ctr-grid")}.querySelectorAll(".k-dot").length, pass: ${$("ctr-grid")}.querySelectorAll(".k-pass").length,
             partial: ${$("ctr-grid")}.querySelectorAll(".k-partial").length, grids: document.querySelectorAll(".k-rg").length,
             details: ${$("ct-results")}.textContent, shown: !${$("ctr-gridbox")}.hidden, summary: ${$("ctr-summary")}.textContent };
  })()`);
  assert.equal(d.dots, 0, "no running dot after the end");
  assert.ok(d.pass >= 6 && d.partial >= 1, "pass " + d.pass + " partial " + d.partial);   // 5 PD levels + a PPS range; the other PPS range has a failed point
  assert.equal(d.grids, 1);
  assert.ok(d.shown);
  assert.match(d.details, /Every step with its evidence \(21\)/);
  assert.match(d.summary, /pass/);
  assert.doesNotMatch(d.summary, /Run #|complete/, "the summary does not repeat the headline");
});

test("Cancel stops the run through the K2 and the page lands on a stopped result", { skip }, async () => {
  const before = await page.eval("__fake.run.id");
  await page.eval(`${$("ctr-again")}.click()`);
  await page.eval(`${$("ctr-start")}.click()`);
  await page.eval(`${$("ctr-go")}.click()`);
  await page.until(`__fake.run.id > ${before} && !${$("ctr-cancel")}.hidden && __fake.rowReads.length >= ${PLAN.length}`, 10000, "second run live");
  await page.eval("__fake.advance(3)");
  await page.eval(`${$("ctr-cancel")}.click()`);
  await page.until("__fake.cancels === 1", 4000, "cancel reached the K2");
  await page.until(`${$("ctr-headline")}.textContent.indexOf("stopped") >= 0 && ${$("ctr-summary")}.textContent.indexOf("pass") >= 0`,
                   12000, "the stopped headline and summary");
  const s = await page.eval(`(function () {
    return { dots: ${$("ctr-grid")}.querySelectorAll(".k-dot").length, stopped: ${$("ctr-grid")}.querySelectorAll(".k-stopped, .k-none").length,
             pass: ${$("ctr-grid")}.querySelectorAll(".k-pass").length };
  })()`);
  assert.equal(s.dots, 0);
  assert.ok(s.pass >= 3 && s.stopped >= 1, "pass " + s.pass + " unfinished " + s.stopped);
  assert.deepEqual(page.errors, [], "no page errors");
});
