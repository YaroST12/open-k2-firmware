// Headless Chrome over the DevTools protocol (a minimal client on Node's own WebSocket, no dependencies) and a tiny
// server that plays the K2's web server for tools/web/k2.html: it serves the page with tests/support/fakek2.js injected
// after the embedded k2proto block, answers /api/ping, and answers every /api/frame with UNKNOWN_CMD (the fake replaces
// the calls a test cares about). Used by tests/test_ctpage.mjs and tools/web/shoot_ctgrid.mjs.
import { spawn, spawnSync } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const root = path.join(here, "..", "..");

export function findChrome() {
  const home = globalThis.process.env.HOME || "";
  for (const c of [globalThis.process.env.K2_CHROME, path.join(home, ".local/bin/google-chrome"), "/usr/bin/google-chrome",
                   "/usr/bin/chromium", "/usr/bin/chromium-browser"]) {
    if (c && existsSync(c)) return c;
  }
  const w = spawnSync("sh", ["-c", "command -v google-chrome chromium chromium-browser | head -1"], { encoding: "utf8" });
  return w.stdout.trim() || null;
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// The page as the K2 would serve it, plus the fake. The injection point is the end of the k2proto block.
export function fakePage() {
  const html = readFileSync(path.join(root, "tools/web/k2.html"), "utf8");
  const at = html.indexOf('<script id="k2proto">');
  const end = at < 0 ? -1 : html.indexOf("</script>", at);
  if (end < 0) throw new Error("k2.html has no k2proto block");
  const fake = ["fakek2.js", "fakepages.js"].map((f) => readFileSync(path.join(here, f), "utf8")).join("\n");
  return html.slice(0, end + 9) + "\n<script>" + fake + "</script>\n" + html.slice(end + 9);
}

function unknownCmd(hex) {
  const f = Buffer.from(hex, "hex");
  const out = Buffer.alloc(64);
  out[0] = 0xA6; out[1] = 0x4B; out[2] = f[2]; out[3] = f[3]; out[4] = 1;   // status 1 = UNKNOWN_CMD, length 0
  return out.toString("hex");
}

export function startServer(html = fakePage()) {
  const server = http.createServer((req, res) => {
    const u = new URL(req.url, "http://x");
    if (u.pathname === "/") { res.writeHead(200, { "content-type": "text/html" }); res.end(html); }
    else if (u.pathname === "/api/ping") { res.writeHead(200, { "content-type": "application/json" }); res.end("{}"); }
    else if (u.pathname === "/api/frame") {
      res.writeHead(200, { "content-type": "application/json" });
      res.end(JSON.stringify({ rsp: unknownCmd(u.searchParams.get("hex") || "") }));
    } else { res.writeHead(404); res.end(); }
  });
  return new Promise((resolve) => server.listen(0, "127.0.0.1", () => resolve({
    url: "http://127.0.0.1:" + server.address().port + "/", close: () => new Promise((r) => server.close(r)) })));
}

// One CDP session on a fresh page. opts: width, height, dark (default light), mobile.
export async function launch(opts = {}) {
  const chrome = findChrome();
  if (!chrome) throw new Error("no chrome");
  const runs = path.join(root, "runs");
  mkdirSync(runs, { recursive: true });
  const prof = mkdtempSync(path.join(runs, "chrome-profile-"));
  const proc = spawn(chrome, ["--headless=new", "--no-sandbox", "--disable-gpu", "--hide-scrollbars", "--no-first-run",
    "--disable-extensions", "--remote-debugging-port=0", "--user-data-dir=" + prof, "about:blank"], { stdio: "ignore" });
  let alive = true;
  proc.on("exit", () => { alive = false; });
  const portFile = path.join(prof, "DevToolsActivePort");
  for (let i = 0; i < 100 && !existsSync(portFile); i++) await sleep(100);
  if (!existsSync(portFile)) { proc.kill(); rmSync(prof, { recursive: true, force: true }); throw new Error("chrome did not start"); }
  const [port, wsPath] = readFileSync(portFile, "utf8").trim().split("\n");
  const ws = new WebSocket("ws://127.0.0.1:" + port + wsPath);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = () => rej(new Error("devtools socket")); });
  let id = 0;
  const waiting = new Map();
  const listeners = [];
  ws.onmessage = (m) => {
    const d = JSON.parse(m.data);
    if (d.id && waiting.has(d.id)) {
      const w = waiting.get(d.id); waiting.delete(d.id);
      if (d.error) w.rej(new Error(d.error.message)); else w.res(d.result);
    } else if (d.method) for (const l of listeners) l(d);
  };
  const raw = (method, params = {}, sessionId) => new Promise((res, rej) => {
    waiting.set(++id, { res, rej });
    ws.send(JSON.stringify({ id, method, params, sessionId }));
  });
  const { targetId } = await raw("Target.createTarget", { url: "about:blank" });
  const { sessionId } = await raw("Target.attachToTarget", { targetId, flatten: true });
  const send = (method, params) => raw(method, params, sessionId);
  const page = {
    send, console: [], errors: [],
    async size(width, height) {
      await send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor: 1, mobile: !!opts.mobile });
    },
    async eval(expr) {
      const r = await send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) throw new Error("page eval: " + (r.exceptionDetails.exception?.description || r.exceptionDetails.text));
      return r.result.value;
    },
    async goto(url) { await send("Page.navigate", { url }); await sleep(200); },
    async until(expr, ms = 8000, what = expr) {
      const t0 = Date.now();
      while (Date.now() - t0 < ms) { if (await page.eval(expr)) return; await sleep(50); }
      throw new Error("timed out waiting for " + what);
    },
    async theme(t) { await page.eval(`document.documentElement.setAttribute("data-theme", ${JSON.stringify(t)})`); },
    // the whole page, not just the viewport
    async shot(file, o = {}) {
      const w = opts.width || 1200;
      const h = o.full === false ? opts.height || 900 : Math.max(opts.height || 900, await page.eval("document.documentElement.scrollHeight"));
      await page.size(w, h);
      await sleep(80);
      const clip = o.selector ? await page.eval(`(function(){var r=document.querySelector(${JSON.stringify(o.selector)}).getBoundingClientRect();return {x:r.left+scrollX,y:r.top+scrollY,width:r.width,height:r.height,scale:1};})()`) : undefined;
      const { data } = await send("Page.captureScreenshot", { format: "png", clip });
      mkdirSync(path.dirname(file), { recursive: true });
      writeFileSync(file, Buffer.from(data, "base64"));
      await page.size(w, opts.height || 900);
    },
    async close() {
      try { ws.close(); } catch (e) { /* closing anyway */ }
      if (alive) proc.kill();
      await sleep(150);
      rmSync(prof, { recursive: true, force: true });
    },
  };
  listeners.push((d) => {
    if (d.method === "Runtime.consoleAPICalled") page.console.push(d.params.args.map((a) => a.value ?? a.description).join(" "));
    if (d.method === "Runtime.exceptionThrown") page.errors.push(d.params.exceptionDetails.exception?.description || d.params.exceptionDetails.text);
  });
  await send("Runtime.enable");
  await send("Page.enable");
  await page.size(opts.width || 1200, opts.height || 900);
  await send("Emulation.setEmulatedMedia", { features: [{ name: "prefers-color-scheme", value: opts.dark ? "dark" : "light" }] });
  return page;
}
