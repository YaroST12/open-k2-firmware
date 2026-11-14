// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The dashboard's Memory card on the Device tab in a real browser against a fake K2 (tests/support/fakek2.js): the card
// shows the K2 Memory rows with units, the stack low-water row turns caution under 7000 B and error under 6000 B (colour
// plus a word), the footer takes the same tone, it refreshes on its own while the Device tab is open and stays quiet on
// another tab. Needs Chrome (K2_CHROME, ~/.local/bin/google-chrome, or one on PATH); skips without.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { findChrome, launch, startServer, sleep } from "./support/chrome.mjs";

const skip = findChrome() ? false : "no chrome";
let srv, page;

before(async () => {
  if (skip) return;
  srv = await startServer();
  page = await launch({ width: 360, height: 800, mobile: true });
  await page.goto(srv.url + "#/device");
  await page.until('!document.getElementById("c-memory").hidden', 10000, "the Memory card");
});
after(async () => {
  if (page) await page.close();
  if (srv) await srv.close();
});

const $ = (id) => `document.getElementById(${JSON.stringify(id)})`;
const rows = () => page.eval(`(function () {
  var out = {};
  Array.prototype.forEach.call(${$("mem")}.querySelectorAll(".k-kv-row"), function (r) {
    out[r.querySelector(".k-kv-k").textContent] = { v: r.querySelector(".k-kv-v").textContent, cls: r.className };
  });
  var f = ${$("mem")}.querySelector(".k-foot");
  return { rows: out, foot: f ? f.textContent : null, footCls: f ? f.className : null };
})()`);
const settle = async (n) => { await page.until(`__fake.memCalls >= ${n}`, 9000, "a memory read"); await sleep(120); };

test("the card shows the K2 rows with units, and the heap line", { skip }, async () => {
  await page.until(`${$("mem")}.querySelectorAll(".k-kv-row").length === 9`, 6000, "nine rows");
  const r = await rows();
  assert.deepEqual(Object.keys(r.rows), ["Stack now", "Stack low", "IRQ low", "RAM static", "FS used", "FS free", "Image", "Image free", "Heap"]);
  assert.equal(r.rows["Stack low"].v, "7400 B");
  assert.equal(r.rows["RAM static"].v, "69.0 of 96.0 KB");
  assert.equal(r.rows["FS used"].v, "80 KB (40 blk)");
  assert.equal(r.rows["FS free"].v, "212 KB (106 blk)");
  assert.equal(r.rows["Heap"].v, "none");
  assert.equal(r.foot, "Stack never below 7400 B");
  assert.match(r.footCls, /k-foot-info/);
  assert.ok(!/k-(caution|error)/.test(r.rows["Stack low"].cls), "7400 B is normal: no exception colour");
});

test("the low-water row goes caution under 7000 B and error under 6000 B, with a word", { skip }, async () => {
  let n = await page.eval("__fake.memCalls");
  await page.eval("__fake.mem.psp_low = 6500");
  await page.until(`${$("mem")}.querySelector(".k-kv-row.k-caution")`, 9000, "caution row");
  let r = await rows();
  assert.match(r.rows["Stack low"].v, /^6500 B\s+TIGHT$/);
  assert.match(r.footCls, /k-foot-caution/);
  assert.match(r.foot, /^Stack tight: 6500 B$/);
  await page.eval("__fake.mem.psp_low = 5312");
  await page.until(`${$("mem")}.querySelector(".k-kv-row.k-error")`, 9000, "error row");
  r = await rows();
  assert.match(r.rows["Stack low"].v, /^5312 B\s+LOW$/);
  assert.match(r.footCls, /k-foot-error/);
  assert.match(r.foot, /^Stack critical: 5312 B$/);
  const only = await page.eval(`${$("mem")}.querySelectorAll(".k-caution, .k-error").length`);
  assert.equal(only, 1, "only the stack row takes an exception colour");
  await page.eval("__fake.mem.psp_low = 7400");
  await page.until(`!${$("mem")}.querySelector(".k-kv-row.k-error")`, 9000, "back to normal");
});

test("it refreshes while the Device tab is open and stops asking on another tab", { skip }, async () => {
  const a = await page.eval("__fake.memCalls");
  await page.until(`__fake.memCalls > ${a}`, 9000, "a refresh");
  await page.eval('location.hash = "#/live"');
  await sleep(500);
  const b = await page.eval("__fake.memCalls");
  await sleep(4500);
  assert.equal(await page.eval("__fake.memCalls"), b, "no reads on the Live tab");
  assert.ok(await page.eval(`${$("c-memory")}.getBoundingClientRect().height === 0`), "the card is not shown there");
  await page.eval('location.hash = "#/device"');
  await page.until(`__fake.memCalls > ${b}`, 9000, "reads resume");
  assert.deepEqual(page.errors, [], "no page errors");
});

test("the card fits a phone: no sideways scroll", { skip }, async () => {
  const over = await page.eval("document.documentElement.scrollWidth - document.documentElement.clientWidth");
  assert.ok(over <= 0, "page is " + over + " px wider than the screen");
});
