// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The WebHID flasher page (webhid-flasher/index.html): its inline script runs here against a stub DOM and a fake
// bootloader behind a fake HID device, so the backup-first flow is tested as the page wires it, without a browser.
// Run: node --test tests/test_flasher_page.mjs   (make test does it when node is installed)
import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { readFileSync } from "node:fs";
import vm from "node:vm";

const require = createRequire(import.meta.url);
const B = require("../webhid-flasher/k2boot.js");
const P = require("../tools/web/k2proto.js");
const html = readFileSync(new URL("../webhid-flasher/index.html", import.meta.url), "utf8");
const script = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].pop()[1];

const u8 = (...a) => Uint8Array.from(a);
const tick = () => new Promise((r) => setTimeout(r, 0));
async function until(pred, what) {
  const end = Date.now() + 10000;   // the page's link waits 3 s for an answer that a refusing bootloader never sends
  while (Date.now() < end) { if (pred()) return; await tick(); }
  assert.fail("timed out waiting for " + what);
}

// ---- a fake K2 in its bootloader ---------------------------------------------------------------

function makeFlash() {
  const flash = new Uint8Array(B.FLASH_END - B.FLASH_START).fill(0xFF);
  flash.fill(0xB0, 0, 0x1000);
  const names = "K2\0WITRN\0K2(C)WITRN\0x\0";
  for (let i = 0; i < names.length; i++) flash[B.NAME_START - B.FLASH_START + i] = names.charCodeAt(i);
  const app = new Uint8Array(B.NVRAM_START - B.APP_START).fill(0xFF);
  for (let i = 8; i < 0x6000; i++) app[i] = (i * 7) & 0xFF;
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
  app[2048] = 0x27;
  app.set([0x4B, 0x32, 0x30, 0x30], 2064);
  app.set([0xFE, 0xFF, 0xFF, 0xFF], B.MARKER_OFFSET);
  app.fill(0x6C, B.FS_START - B.APP_START);
  flash.set(app, B.APP_START - B.FLASH_START);
  return flash;
}

// refuseFrom: reads reaching past it get no answer (a timeout), like a refusing bootloader.
function fakeDevice(flash, refuseFrom = Infinity) {
  const le = (p) => (p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24) >>> 0;
  const dev = {
    vendorId: 0x0716, productId: 0x5060, productName: "WITRN.K2", serialNumber: "20230727", opened: true,
    oninputreport: null,
    close: async () => {},
    sendReport: async (_id, f) => {
      const reply = (frame) => dev.oninputreport({ data: frame });
      const p = B.parseFrame(f);
      if (p.command === B.CMD.HANDSHAKE) reply(B.buildFrame(B.CMD.ACK, u8(), { reply: true, tick: 1 }));
      else if (p.command === B.CMD.READ) {
        const addr = le(p.payload), size = p.payload[4] | p.payload[5] << 8;
        if (addr + size > refuseFrom) return;
        for (let off = 0; off < size; off += B.WRITE_SIZE) {
          const n = Math.min(B.WRITE_SIZE, size - off), a = addr - B.FLASH_START + off;
          reply(B.buildFrame(B.CMD.READ, flash.slice(a, a + n), { reply: true, tick: 1 }));
        }
      } else if (p.command === B.CMD.READ_MARKER) {
        const a = le(p.payload) - B.FLASH_START;
        reply(B.buildFrame(B.CMD.READ_MARKER, flash.slice(a, a + 4), { reply: true, tick: 1 }));
      }
    },
  };
  return dev;
}

// ---- the page, with a stub DOM -------------------------------------------------------------------

function startPage(dev) {
  const events = [];
  const els = {};
  const make = (tag) => ({
    tagName: tag.toUpperCase(), children: [], style: {}, className: "", textContent: "", hidden: false, disabled: false,
    checked: false, files: [], clicks: 0, onclick: null, onchange: null,
    firstElementChild: { style: {} },
    appendChild(c) { this.children.push(c); return c; },
    click() { this.clicks++; },
  });
  for (const m of html.matchAll(/<(\w+)([^>]*)\bid="([^"]+)"([^>]*)>/g)) {
    const e = make(m[1]);
    e.hidden = /\shidden[\s>]|\shidden$/.test(m[2] + m[4]);   // the attribute as written; `checked` is left false on purpose
    els[m[3]] = e;
  }
  // a browser may restore an unticked box on reload: the page must tick it again itself
  els.backupFirst.checked = false;

  class Boot extends B.BootLink {
    async backup(o) { events.push("backup" + (o && o.appOnly ? ":app-only" : "")); return super.backup(o); }
    async flash(fw, o) {
      events.push("flash");
      return { currentVersion: "2.7", currentMarker: "feffffff" };
    }
  }
  const K2BootStub = Object.assign({}, B, { BootLink: Boot });
  const ctx = {
    document: { getElementById: (id) => els[id] || null, createElement: make },
    navigator: { hid: { addEventListener() {}, getDevices: async () => [dev], requestDevice: async () => [dev] } },
    K2Boot: K2BootStub, K2Proto: P,
    URL: { createObjectURL: () => "blob:x", revokeObjectURL() {} },
    FileReader: class { readAsArrayBuffer(f) { setTimeout(() => { this.result = f.bytes.buffer.slice(f.bytes.byteOffset, f.bytes.byteOffset + f.bytes.byteLength); this.onload(); }, 0); } },
    fetch: async () => ({ ok: false }),
    crypto: globalThis.crypto, TextDecoder, TextEncoder, Blob, Response, Promise, Uint8Array, Date, JSON,
    setTimeout, console,
  };
  vm.createContext(ctx);
  vm.runInContext(script, ctx);
  const logText = () => els.log.children.map((c) => c.textContent).join("\n");
  return { els, events, logText };
}

function k2File(name = "fw.k2") {
  const app = new Uint8Array(0x1000).fill(0xAB);
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
  app.fill(0xFF, B.MARKER_OFFSET, B.MARKER_OFFSET + 4);
  app[2048] = 0x30;
  return { name, bytes: B.Firmware.fromApp(app, "2026-11-08").source };
}
async function pick(page, file) {
  const before = page.els.log.children.length;
  page.els.file.files = [file];
  page.els.file.onchange.call(page.els.file);
  await until(() => page.els.log.children.length > before, "the file to load");
}
async function connected(dev) {
  const page = startPage(dev);
  await until(() => page.els.mode.textContent.startsWith("bootloader"), "the bootloader link");
  return page;
}

// ---- tests ---------------------------------------------------------------------------------------

test("the backup-first box is ticked on every page load and unticking it shows the warning", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  assert.equal(page.els.backupFirst.checked, true);
  assert.equal(page.els.noBackupWarn.hidden, true);
  page.els.backupFirst.checked = false;
  page.els.backupFirst.onchange();
  assert.equal(page.els.noBackupWarn.hidden, false);
  assert.match(html, /<input type="checkbox" id="backupFirst"[^>]*\bchecked\b/);          // ticked in the markup too
  assert.equal(page.els.backupAppOnly.checked, false);                                    // the advanced option is off
  assert.doesNotMatch(html.match(/<input[^>]*id="backupAppOnly"[^>]*>/)[0], /\bchecked\b/);
});

test("Flash makes the full backup first, offers the ZIP, then flashes", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  await pick(page, k2File());
  page.els.flash.onclick();
  await until(() => page.events.includes("flash"), "the flash");
  assert.deepEqual(page.events, ["backup", "flash"]);
  assert.equal(page.els.dlZip.clicks, 1);                                  // the ZIP download was started
  assert.match(page.els.dlZip.download, /^K2_BACKUP_2\.7_\d{8}T\d{6}Z\.zip$/);
  assert.equal(page.els.backupOut.hidden, false);
  assert.match(page.els.backupInfo.textContent, /^Full backup/);
});

test("with the box unticked Flash goes straight to the flash and says there is no way back", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  await pick(page, k2File());
  page.els.backupFirst.checked = false;
  page.els.backupFirst.onchange();
  page.els.flash.onclick();
  await until(() => page.events.includes("flash"), "the flash");
  assert.deepEqual(page.events, ["flash"]);
  assert.match(page.logText(), /without a backup/);
});

test("a failed backup stops the flash before anything is erased", async () => {
  const page = await connected(fakeDevice(makeFlash(), 0x08010000));    // refuses inside the application area
  await pick(page, k2File());
  page.els.flash.onclick();
  await until(() => /nothing was erased/.test(page.logText()), "the abort message");
  assert.deepEqual(page.events, ["backup"]);
});

test("a backup just made on this page is reused by the next flash, a second flash backs up again", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  page.els.backup.onclick();
  await until(() => page.events.length === 1 && /backup done/.test(page.logText()), "the standalone backup");
  assert.equal(page.els.dlZip.clicks, 0);                                  // backup only: a link, no automatic download
  await pick(page, k2File());
  page.els.flash.onclick();
  await until(() => page.events.includes("flash"), "the first flash");
  assert.deepEqual(page.events, ["backup", "flash"]);                      // no second backup
  await until(() => !page.els.flash.disabled, "the page to settle");
  page.els.flash.onclick();
  await until(() => page.events.filter((e) => e === "flash").length === 2, "the second flash");
  assert.deepEqual(page.events, ["backup", "flash", "backup", "flash"]);   // the K2 changed: a new backup
});

test("Restore this backup flashes the backup's recovery container without backing up again", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  page.els.backup.onclick();
  await until(() => /backup done/.test(page.logText()), "the backup");
  await until(() => !page.els.restoreBackup.disabled, "the restore button");
  page.els.restoreBackup.onclick();
  await until(() => page.events.includes("flash"), "the restore");
  assert.deepEqual(page.events, ["backup", "flash"]);
});

test("the advanced app-area-only option is passed to the backup", async () => {
  const page = await connected(fakeDevice(makeFlash()));
  page.els.backupAppOnly.checked = true;
  await pick(page, k2File());
  page.els.flash.onclick();
  await until(() => page.events.includes("flash"), "the flash");
  assert.deepEqual(page.events, ["backup:app-only", "flash"]);
  assert.match(page.els.backupInfo.textContent, /^APPLICATION AREA ONLY/);
});

test("a K2_BACKUP ZIP picked as the firmware flashes the recovery container inside it", async () => {
  const flash = makeFlash();
  const dev = fakeDevice(flash);
  const page = await connected(dev);
  const link = new B.BootLink({ send: (f) => dev.sendReport(0, f) }, { timeoutMs: 50 });
  dev.oninputreport = (ev) => link.feed(ev.data);
  const r = await link.backup({ now: new Date("2026-11-08T10:00:00Z") });
  dev.oninputreport = null;
  const page2 = await connected(fakeDevice(flash));
  await pick(page2, { name: r.zipName, bytes: r.zip });
  assert.match(page2.logText(), /K2_RECOVERY_2\.7\.k2/);
  assert.equal(page2.els.flash.disabled, false);
  await pick(page2, { name: "junk.zip", bytes: B.makeZip([{ name: "flash.bin", data: u8(1) }]) });
  assert.match(page2.logText(), /cannot use junk\.zip: no K2_RECOVERY/);
});
