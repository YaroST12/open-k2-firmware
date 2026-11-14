// Screenshots of the dashboard's Charger test tab against the fake K2: setup, running, done and cancelled, at a phone
// width and a desktop width, light and dark. Usage: node tests/support/shoot_ctgrid.mjs [outdir]  (default runs/web-ct-grid/web)
import path from "node:path";
import { launch, startServer, sleep, root } from "./chrome.mjs";
import { S, PD, REQ, NOOFFER, baseus, withIndex } from "./ctfixture.mjs";

const out = path.resolve(globalThis.process.argv[2] || path.join(root, "runs/web-ct-grid/web"));

// A QC / UFCS heavy plan: red fails beside passes and strikes, a unit with a current bracket (the device's UFCS render).
function rich() {
  const s = baseus().filter((r) => PD.includes(r.algorithm));
  for (const a of ["qc2", "qc3"]) {
    s.push(S(a, 5000, "pass")); s.push(S(a, 9000, "fail")); s.push(S(a, 12000, "skip", { reason: REQ }));
  }
  const u = (mv, res, extra) => S("ufcs", mv, res, { pdo: (60 << 25) >>> 0, ...extra });
  s.push(u(5000, "pass")); s.push(u(8000, "fail")); s.push(u(11000, "skip", { reason: REQ }));
  s.push(u(12500, "skip", { reason: REQ })); s.push(u(20000, "skip", { reason: REQ }));
  s.push(S("fcp", 5000, "pass")); s.push(S("fcp", 9000, "skip", { reason: REQ }));
  s.push(S("vooc", 5000, "skip", { reason: NOOFFER }));
  return withIndex(s);
}

const g = (id) => "document.getElementById(" + JSON.stringify(id) + ")";
const click = (page, id) => page.eval(g(id) + ".click()");
const cfg = [{ name: "phone", width: 390, height: 844, mobile: true }, { name: "desktop", width: 1100, height: 900, mobile: false }];
const srv = await startServer();
const plan = rich();
const files = [];
try {
  for (const c of cfg) {
    const page = await launch({ width: c.width, height: c.height, mobile: c.mobile });
    try {
      await page.goto(srv.url + "#/test");
      await page.until("!" + g("c-ctrun") + ".hidden", 10000, "the run card");
      await page.eval("__fake.plan = " + JSON.stringify(plan));
      const shot = async (theme, state) => {
        const f = path.join(out, c.name + "-" + theme + "-" + state + ".png");
        await page.shot(f); files.push(f);
      };
      const begin = async () => {
        const before = await page.eval("__fake.run ? __fake.run.id : 0");
        await click(page, "ctr-start"); await click(page, "ctr-go");
        await page.until("__fake.run && __fake.run.id > " + before + " && __fake.rowReads.length >= " + plan.length, 12000, "rows read"); };
      for (const theme of ["light", "dark"]) {
        await page.theme(theme);
        await page.eval("(function(){var a=" + g("ctr-again") + "; if (a && !a.hidden) a.click();})()");
        await sleep(300);
        await shot(theme, "1-setup");
        await begin();
        await page.eval("__fake.advance(" + (PD.length + 7) + ")");   // into the QC units
        await sleep(2600);
        await shot(theme, "2-running");
        if (c.name === "phone") await page.shot(path.join(out, "crop-" + theme + "-running.png"), { selector: "#ctr-gridbox" });
        await page.eval("__fake.end('done')");
        await page.until(g("ctr-summary") + ".textContent.indexOf('complete') >= 0", 12000, "done");
        await sleep(300);
        await shot(theme, "3-done");
        if (c.name === "phone") await page.shot(path.join(out, "crop-" + theme + "-done.png"), { selector: "#ctr-gridbox" });
        await click(page, "ctr-again");
        await begin();
        await page.eval("__fake.advance(" + (PD.length + 8) + ")");
        await sleep(2600);
        await click(page, "ctr-cancel");
        await page.until(g("ctr-summary") + ".textContent.indexOf('stopped') >= 0", 12000, "stopped");
        await sleep(300);
        await shot(theme, "4-cancelled");
      }
      if (page.errors.length) console.log("page errors", c.name, page.errors);
    } finally { await page.close(); }
  }
} finally { await srv.close(); }

// The comparison sheet: the device's own renders (docs/UI_SYSTEM.md pages, 2x PNGs from tools/panel2png.py) beside the
// page's grid in both themes, one row per state. The device PNGs come from runs/web-ct-grid/png (see the report).
const png = path.join(root, "runs/web-ct-grid/png");
const row = (title, dev, a, b) => '<tr><td class="cap">' + title + '</td><td><img src="file://' + path.join(png, dev) + '"></td>' +
  '<td><img src="file://' + path.join(out, a) + '"></td><td><img src="file://' + path.join(out, b) + '"></td></tr>';
const sheet = path.join(out, "compare.html");
const { writeFileSync } = await import("node:fs");
writeFileSync(sheet, '<!doctype html><meta charset="utf-8"><style>body{margin:0;padding:16px;background:#888;font:14px sans-serif;color:#111}' +
  'table{border-collapse:separate;border-spacing:12px}td{vertical-align:top}.cap{width:110px;font-weight:600}img{display:block}' +
  'th{text-align:left;padding-bottom:4px}</style><table><tr><th></th><th>K2 device (240x240, 2x)</th><th>Web, light</th><th>Web, dark</th></tr>' +
  row("Running", "flight_step24.png", "crop-light-running.png", "crop-dark-running.png") +
  row("Finished", "ufcs_cuktech_results.png", "crop-light-done.png", "crop-dark-done.png") + '</table>');
const cp = await launch({ width: 1400, height: 1000 });
try { await cp.goto("file://" + sheet); await sleep(600); await cp.shot(path.join(out, "compare.png")); files.push(path.join(out, "compare.png")); }
finally { await cp.close(); }
console.log(files.join("\n"));
