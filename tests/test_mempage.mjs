// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The dashboard's Memory card on the Device tab in a real browser against a fake K2 (tests/support/fakek2.js): the card
// shows the K2 Memory rows with units, the FS free row turns caution under 20 % of the volume free and error under 10 %
// (colour plus a word), the footer takes the same tone, the stack rows stay plain numbers however low, it refreshes on its own while the Device tab is open and stays quiet on
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

test("the FS free row goes caution under 20 % free and error under 10 %, with a word; the stack never does", { skip }, async () => {
  await page.eval("__fake.mem.psp_low = 5312");          // the old stack error level: now a plain number
  await page.until(`/5312 B/.test(${$("mem")}.textContent)`, 9000, "the low stack read");
  let r = await rows();
  assert.equal(r.rows["Stack low"].v, "5312 B");
  assert.ok(!/k-(caution|error)/.test(r.rows["Stack low"].cls), "the stack row has no exception colour");
  assert.match(r.footCls, /k-foot-info/);
  await page.eval("__fake.mem.fs_used = 146 - 20");     // 20 of 146 blocks free: 13.7 %
  await page.until(`${$("mem")}.querySelector(".k-kv-row.k-caution")`, 9000, "caution row");
  r = await rows();
  assert.match(r.rows["FS free"].v, /^40 KB \(20 blk\)\s+LOW$/);
  assert.match(r.footCls, /k-foot-caution/);
  assert.equal(r.foot, "Storage low: delete cards");
  await page.eval("__fake.mem.fs_used = 146 - 5");      // 3.4 %
  await page.until(`${$("mem")}.querySelector(".k-kv-row.k-error")`, 9000, "error row");
  r = await rows();
  assert.match(r.rows["FS free"].v, /^10 KB \(5 blk\)\s+FULL$/);
  assert.match(r.footCls, /k-foot-error/);
  assert.equal(r.foot, "Storage full: delete cards");
  const only = await page.eval(`${$("mem")}.querySelectorAll(".k-caution, .k-error").length`);
  assert.equal(only, 1, "only the FS free row takes an exception colour");
  await page.eval("__fake.mem.fs_used = 40; __fake.mem.psp_low = 7400");
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
