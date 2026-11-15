// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The dashboard's PD, Fast charge, PD Spec Check and Cables pages in headless Chrome against a fake K2 (tests/support/fakek2.js and
// fakepages.js). No device and no manual browser session: this proves the page wiring (what a press sends, when it asks to confirm,
// what the footer says), not the firmware. Needs Chrome (K2_CHROME, ~/.local/bin/google-chrome, or one on PATH); skips without.
import { test, before, after } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { findChrome, launch, startServer } from "./support/chrome.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const html = readFileSync(path.join(here, "../tools/web/k2.html"), "utf8");
const skip = findChrome() ? false : "no chrome";
let srv, page;

before(async () => {
  if (skip) return;
  srv = await startServer();
  page = await launch({ width: 900, height: 900 });
  await page.goto(srv.url + "#/pd");
  await page.until('document.querySelectorAll("#pt-body .k-rl-row").length === 3', 10000, "the PDO list");
});
after(async () => {
  if (page) await page.close();
  if (srv) await srv.close();
});

const $ = (id) => `document.getElementById(${JSON.stringify(id)})`;
const go = async (route) => { await page.eval("location.hash = " + JSON.stringify("#/" + route)); };
const sent = () => page.eval("JSON.parse(JSON.stringify(__fake.sent))");
const foot = () => page.eval(`${$("k-foot")}.textContent`);
const press = (act) => page.eval("document.querySelector('.k-act[data-act=" + act + "]').click()");
const clear = () => page.eval("__fake.sent.length = 0");

test("the markup: seven routes, one tab each, every section on a known page, no confirm dialogs in the moved flows", () => {
  const routes = ["live", "test", "pd", "fc", "spec", "cables", "device"];
  const tabs = [...html.matchAll(/<a href="#([a-z]+)"/g)].map((m) => m[1]).filter((r) => routes.includes(r));
  assert.deepEqual(tabs, routes);
  const pages = [...html.matchAll(/<section[^>]*data-page="([a-z ]+)"/g)].flatMap((m) => m[1].split(" "));
  for (const p of pages) assert.ok(routes.includes(p), "unknown data-page " + p);
  const script = html.slice(html.lastIndexOf("<script>"));
  const kit = script.slice(script.indexOf("// ---- kit pages"), script.indexOf("// ---- PD black box"));
  assert.ok(kit.length > 5000, "the kit block is present");
  assert.ok(!/window\.confirm|\balert\(/.test(kit), "the kit pages confirm through the ActionCell, not a dialog");
});

test("no id the page script looks up is missing from the markup", () => {
  const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));
  const script = html.slice(html.lastIndexOf("<script>"));
  const used = [...script.matchAll(/\$\("([^"]+)"\)/g)].map((m) => m[1]);
  assert.deepEqual([...new Set(used)].filter((i) => !ids.has(i)), []);
});

test("every route opens without a page error, and its panels fill", { skip }, async () => {
  for (const r of ["live", "test", "pd", "fc", "spec", "cables", "device"]) {
    await go(r);
    await page.until("document.getElementById('main').getAttribute('data-route') === " + JSON.stringify(r), 3000, "route " + r);
    await page.until("document.getElementById('k-foot').hidden === " + (["pd", "fc", "spec", "cables"].includes(r) ? "false" : "true"), 3000, "footer on " + r);
  }
  await go("cables"); await page.until('document.querySelectorAll("#saved .k-rl-row").length === 1 && document.querySelectorAll("#cards .k-rl-row").length === 1', 8000, "cables lists");
  await go("fc"); await page.until('document.querySelectorAll("#fc-body .k-rl-row").length > 3', 8000, "the protocol list");
  await go("spec"); await page.until('!document.getElementById("c-spec").hidden', 8000, "the spec panel");
  assert.deepEqual([...await page.errors], []);
});

test("PD: REQUEST needs two presses; the first sends nothing and the footer says what is armed", { skip }, async () => {
  await go("pd");
  await page.until('document.querySelectorAll("#pt-body .k-rl-row").length === 3', 4000, "the PDO list");
  await page.eval("document.querySelectorAll('#pt-body .k-rl-row')[1].click()");
  await page.until(`/9\.0|9 V/.test(${$("pt-body")}.textContent)`, 3000, "the 9 V row selected");
  await clear();
  await press("request");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed footer");
  assert.deepEqual(await sent(), [], "nothing is sent on the first press");
  assert.match(await foot(), /request PDO 2/);
  await press("request");
  await page.until("__fake.sent.length === 1", 3000, "the request");
  const [req] = await sent();
  assert.equal(req.call, "pdRequest");
  assert.equal(req.pos, 2);
  assert.ok(req.ma > 0 && req.ma <= 3000, "mA inside the PDO: " + req.ma);
  await page.until(`/Requested/.test(${$("k-foot")}.textContent)`, 3000, "the result in the footer");
});

test("PD: a stepper press changes the set point and drops the arm", { skip }, async () => {
  await go("pd");
  await page.eval("document.querySelectorAll('#pt-body .k-rl-row')[2].click()");   // the PPS row
  await page.until(`!!${$("pt-body")}.querySelector('.k-btn[data-step="mv+"]')`, 3000, "voltage stepper on PPS");
  await clear();
  await press("request");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  await page.eval("document.querySelector('.k-btn[data-step=\"mv+\"]').click()");
  await page.until(`!/Armed/.test(${$("k-foot")}.textContent)`, 3000, "the arm is dropped by a step");
  assert.deepEqual(await sent(), []);
});

test("PD: release and back to 5 V are one press (the safe direction)", { skip }, async () => {
  await go("pd");
  await clear();
  await press("back5v");
  await page.until("__fake.sent.some(function (s) { return s.call === 'stop'; })", 3000, "stop sent at once");
});

test("PD: the identity panel and the contract table fill", { skip }, async () => {
  await go("pd");
  await page.until(`/Fake Maker/.test(${$("id-body")}.textContent)`, 6000, "identity");
  assert.match(await page.eval(`${$("pt-body")}.textContent`), /Contract/);
});

test("Fast charge: QC is never offered above 12 V, VOOC has no trigger button, ENGAGE needs two presses", { skip }, async () => {
  await go("fc");
  await page.until('document.querySelectorAll("#fc-body .k-rl-row").length > 3', 4000, "protocols");
  const rows = await page.eval(`Array.prototype.map.call(${$("fc-body")}.querySelectorAll('.k-rl-row'), function (r) { return r.getAttribute('data-id'); })`);
  assert.ok(rows.includes("qc2") && rows.includes("qc3"), rows.join(","));
  assert.ok(!rows.some((r) => /vooc/i.test(r)), "no VOOC row: " + rows.join(","));
  const body = await page.eval(`${$("fc-body")}.textContent + ${$("fd-body")}.textContent`);
  assert.ok(!/(?:QC[23]?)[^.]{0,20}(?:20|24|28) ?V/i.test(await page.eval(`${$("fc-body")}.querySelector('.k-rl-row[data-id="qc2"]').textContent`)), "QC2 row mentions no level above 12 V");
  assert.match(body, /never triggers them|read only/i);
  assert.ok(!(await page.eval("Array.prototype.some.call(document.querySelectorAll('.k-act'), function (b) { return /vooc/i.test(b.textContent + b.getAttribute('data-act')); })")));
  await page.eval("document.querySelector('#fc-body .k-rl-row[data-id=\"qc2\"]').click()");
  await page.until(`/ENGAGE QC2/.test(${$("fc-body")}.textContent)`, 3000, "QC2 selected");
  for (let i = 0; i < 8; i++) await page.eval("var b = document.querySelector('.k-btn[data-step=\"fc+\"]'); if (b && !b.disabled) b.click()");
  const label = await page.eval(`${$("fc-body")}.querySelector('.k-act[data-act="fctrigger"]').textContent`);
  assert.match(label, /12\.0+ V/, label);
  await clear();
  await press("fctrigger");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  assert.deepEqual(await sent(), []);
  await press("fctrigger");
  await page.until("__fake.sent.length === 1", 3000, "the trigger");
  const [t] = await sent();
  assert.deepEqual([t.call, t.op, t.proto, t.mv, t.confirm, t.allowInline], ["fc", "trigger", "qc2", 12000, true, false]);
});

test("PD Spec Check: START is armed and carries the family mask; an empty mask disables it", { skip }, async () => {
  await go("spec");
  await page.until('!!document.querySelector("#sp-body .k-act[data-act=specstart]")', 4000, "START");
  await clear();
  await press("specstart");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  assert.deepEqual(await sent(), []);
  await press("specstart");
  await page.until("__fake.sent.length === 1", 3000, "start");
  assert.deepEqual((await sent())[0], { call: "pdSpecStart", mask: 63, confirm: true });
  await page.eval("__fake.spec.state = 'idle'");
  await page.until("!document.querySelector('#sp-body .k-act[data-act=specstart]').disabled", 4000, "START free again");
  for (let i = 0; i < 6; i++) await page.eval("var c = document.querySelectorAll('#sp-body input[data-fam]')[" + i + "]; c.checked = false; c.dispatchEvent(new Event('change', { bubbles: true }))");
  await page.until("document.querySelector('#sp-body .k-act[data-act=specstart]').disabled", 3000, "START disabled with no family");
});

test("Cables: DELETE of a saved cable and of a report card each need two presses; rename is one", { skip }, async () => {
  await go("cables");
  await page.until('document.querySelectorAll("#saved .k-rl-row").length === 1', 6000, "saved list");
  await page.eval("document.querySelector('#saved .k-rl-row').click()");
  await clear();
  await press("svdel");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  assert.deepEqual(await sent(), []);
  await press("svdel");
  await page.until("__fake.sent.length === 1", 3000, "delete");
  assert.deepEqual((await sent())[0], { call: "emDelete", seq: 1 });
  await page.eval("document.querySelector('#cards .k-rl-row').click()");
  await clear();
  await press("cddel");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  assert.deepEqual(await sent(), []);
  await press("cddel");
  await page.until("__fake.sent.length === 1", 3000, "delete card");
  assert.deepEqual((await sent())[0], { call: "cardsDelete", no: 1 });
  assert.deepEqual([...await page.errors], []);
});

test("an armed press does not follow you to another page", { skip }, async () => {
  await go("pd");
  await page.eval("document.querySelectorAll('#pt-body .k-rl-row')[0].click()");
  await clear();
  await press("hardreset");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  await go("fc");
  await page.until(`!/Armed/.test(${$("k-foot")}.textContent)`, 3000, "the arm is dropped on the route change");
  await go("pd");
  await press("hardreset");   // a fresh first press, not the second: nothing is sent
  assert.deepEqual(await sent(), []);
});

test("a firmware refusal reads in plain words in the footer, and the arm is gone", { skip }, async () => {
  await go("pd");
  await page.eval("document.querySelectorAll('#pt-body .k-rl-row')[1].click()");
  for (const [fail, want] of [[{ call: "pdRequest", status: 4 }, /K2 is busy/], [{ call: "pdRequest", timeout: true }, /did not answer/]]) {
    await clear();
    await page.eval("__fake.fail = " + JSON.stringify(fail));
    await press("request");
    await press("request");
    await page.until(`/${want.source}/.test(${$("k-foot")}.textContent)`, 3000, "the error wording " + want);
    const f = await foot();
    assert.ok(!/command \d+ failed|RemoteError|RemoteTimeout|\bat \w+\.js/.test(f), "no raw code or exception text: " + f);
    assert.equal(await page.eval(`!!(${$("k-foot")}.querySelector(".k-foot-error") || ${$("k-foot")}.classList.contains("k-foot-error"))`), true, "the footer is in the error tone");
    assert.equal(await page.eval("document.querySelectorAll('.k-act.k-armed').length"), 0, "no cell is still armed");
  }
});

test("Identify: a NEED_CONFIRM answer escalates to an armed press, and the second press sends the confirmed call", { skip }, async () => {
  await go("pd");
  await clear();
  await page.eval("__fake.needConfirmOnce = true");
  await press("identify");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed after the firmware asked for a confirm");
  assert.deepEqual((await sent()).map((s) => [s.call, s.confirm]), [["identifyRun", false]]);
  await press("identify");
  await page.until("__fake.sent.length === 2", 3000, "the confirmed run");
  assert.deepEqual((await sent())[1], { call: "identifyRun", confirm: true });
});

test("VOOC: the monitor switch is one press and only listens; there is still no trigger", { skip }, async () => {
  await go("fc");
  await page.until(`/VOOC monitor/.test(${$("fd-body")}.textContent)`, 4000, "the monitor row");
  await clear();
  await press("voocmon");
  await page.until("__fake.sent.length === 1", 3000, "the switch");
  assert.deepEqual((await sent())[0], { call: "voocMonitor", on: true });
  await page.until(`/Handshake/.test(${$("fd-body")}.textContent) && /0x82 replies/.test(${$("fd-body")}.textContent)`, 3000, "monitor rows");
  assert.ok(!(await page.eval("Array.prototype.some.call(document.querySelectorAll('.k-act'), function (b) { return /vooc/i.test(b.getAttribute('data-act')) && /trigger|engage/i.test(b.textContent); })")));
  await press("voocmon");
  await page.until("__fake.sent.length === 2", 3000, "switch off");
  assert.deepEqual((await sent())[1], { call: "voocMonitor", on: false });
});

test("not connected: every control on the four pages is disabled, nothing is sent, the footer says why", { skip }, async () => {
  const p2 = await launch({ width: 900, height: 900 });
  try {
    await p2.goto(srv.url + "#/pd");
    await p2.until('document.querySelectorAll("#pt-body .k-rl-row").length === 3', 10000, "connected first");
    await p2.eval('document.getElementById("connect").click()');   // Disconnect
    await p2.until('document.getElementById("connect").textContent === "Connect"', 4000, "disconnected");
    await p2.eval("__fake.sent.length = 0");
    for (const r of ["pd", "fc", "spec", "cables"]) {
      await p2.eval("location.hash = " + JSON.stringify("#/" + r));
      await p2.until("document.getElementById('main').getAttribute('data-route') === " + JSON.stringify(r), 3000, "route " + r);
      await p2.until(`/Not connected/.test(document.getElementById("k-foot").textContent)`, 3000, "the footer on " + r);
      const live = await p2.eval(`Array.prototype.filter.call(document.querySelectorAll("section[data-page~='${r}'] .k-act"), function (b) { return !b.disabled && b.getAttribute("aria-disabled") !== "true"; }).length`);
      assert.equal(live, 0, "enabled controls on " + r);
      await p2.eval(`Array.prototype.forEach.call(document.querySelectorAll("section[data-page~='${r}'] .k-act"), function (b) { b.click(); b.click(); })`);
    }
    assert.deepEqual(await p2.eval("JSON.parse(JSON.stringify(__fake.sent))"), [], "no call reached the K2");
    assert.deepEqual([...await p2.errors], []);
  } finally { await p2.close(); }
});

test("an armed action expires by itself after five seconds and the footer returns to Ready", { skip }, async () => {
  await go("pd");
  await page.eval("document.querySelectorAll('#pt-body .k-rl-row')[0].click()");
  await press("hardreset");
  await page.until(`/Armed/.test(${$("k-foot")}.textContent)`, 3000, "armed");
  await page.until(`/Ready/.test(${$("k-foot")}.textContent)`, 7000, "expired");
});

// A phone-width layout: the voltage stepper (-1 V -0.1 value +0.1 +1 V) must stay on one line per button and inside its card.
test("PD at 360 px: every stepper button label is one line and no stepper row spills out of the card", { skip }, async () => {
  const p2 = await launch({ width: 360, height: 900, mobile: true });
  try {
    await p2.goto(srv.url + "#/pd");
    await p2.until('document.querySelectorAll("#pt-body .k-rl-row").length === 3', 10000, "the PDO list");
    await p2.eval("document.querySelectorAll('#pt-body .k-rl-row')[2].click()");   // the PPS row: both steppers, four voltage buttons
    await p2.until('document.querySelectorAll("#pt-body .k-btn[data-step]").length >= 6', 3000, "the stepper buttons");
    const m = await p2.eval(`(function () {
      var wrapped = [], spill = [];
      Array.prototype.forEach.call(document.querySelectorAll("#pt-body .k-btn[data-step]"), function (b) {
        var r = document.createRange(); r.selectNodeContents(b);
        if (r.getClientRects().length !== 1) wrapped.push(b.textContent);
      });
      Array.prototype.forEach.call(document.querySelectorAll("#pt-body .k-step"), function (s) {
        var box = s.getBoundingClientRect();
        Array.prototype.forEach.call(s.children, function (c) { if (c.getBoundingClientRect().right > box.right + 0.5) spill.push(c.textContent); });
        if (s.scrollWidth > s.clientWidth) spill.push("row " + s.getAttribute("aria-label"));
      });
      return { wrapped: wrapped, spill: spill, page: document.documentElement.scrollWidth, view: innerWidth };
    })()`);
    assert.deepEqual(m.wrapped, [], "a label broke onto two lines");
    assert.deepEqual(m.spill, [], "a stepper child sticks out of its row");
    assert.ok(m.page <= m.view, "the page scrolls sideways: " + m.page + " > " + m.view);
  } finally { await p2.close(); }
});
