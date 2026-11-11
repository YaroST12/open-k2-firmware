// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// Node tests for webhid-flasher/k2boot.js (the WebHID flasher's bootloader protocol + .k2 container).
// Byte vectors are generated from tools/k2_flasher.py (the reference implementation).
// Run: node --test tests/test_k2boot.mjs   (make test does it when node is installed)
import test from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, writeFileSync } from "node:fs";
import os from "node:os";

const require = createRequire(import.meta.url);
const here = path.dirname(fileURLToPath(import.meta.url));
const B = require("../webhid-flasher/k2boot.js");

const u8 = (...a) => Uint8Array.from(a);
const fromHex = (h) => Uint8Array.from(Buffer.from(h, "hex"));
const toHex = (b) => Buffer.from(b).toString("hex");

// k2_flasher.build_frame(cmd, payload, reply=..., tick=...) exact bytes:
const VEC = {
  boot_ack: "ff5540159a875929020000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000024e",
  boot_handshake: "ff5540169a875a290300000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000351",
  erase: "ff550641523f154f0804003800080000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004cdc",
  write: "ff550642523f164f09090038000804deadbeef000000000000000000000000000000000000000000000000000000000000000000000000000000000000008e20",
  read: "ff55f86fa3b10b9d0b06003000080004000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004d04",
  marker: "ff55f8dea7b2169e0a05004400080400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000005f96",
};
// k2_flasher.Firmware.from_app(3076-byte app, sp=0x20020000 reset=0x08003801, 2026-01-15, "2.3").source
// (full hex kept in tests/vectors_k2boot_fw.hex to stay readable here)
import { readFileSync } from "node:fs";
const FW_SOURCE_HEX = readFileSync(path.join(here, "vectors_k2boot_fw.hex"), "utf8").trim();

// ---- framing ------------------------------------------------------------------------------

test("bootloader ack frame bytes match k2_flasher.build_frame(2, tick=123456789)", () => {
  assert.equal(toHex(B.buildFrame(2, u8(), { reply: true, tick: 123456789 })), VEC.boot_ack);
});

test("handshake / erase / write / read / marker frames match k2_flasher.py", () => {
  assert.equal(toHex(B.buildFrame(3, u8(), { reply: true, tick: 123456790 })), VEC.boot_handshake);
  const addr = (a) => u8(a & 0xFF, (a >>> 8) & 0xFF, (a >>> 16) & 0xFF, (a >>> 24) & 0xFF);
  assert.equal(toHex(B.buildFrame(8, addr(0x08003800), { reply: false, tick: 987654321 })), VEC.erase);
  assert.equal(toHex(B.buildFrame(9, u8(...addr(0x08003800), 4, 0xDE, 0xAD, 0xBE, 0xEF), { reply: false, tick: 987654322 })), VEC.write);
  assert.equal(toHex(B.buildFrame(11, u8(...addr(0x08003000), 0x00, 0x04), { reply: true, tick: 555000111 })), VEC.read);
  assert.equal(toHex(B.buildFrame(10, u8(...addr(0x08004400), 4), { reply: true, tick: 555000222 })), VEC.marker);
});

test("parseFrame round-trips and rejects bad checksums/magic", () => {
  const f = B.buildFrame(9, u8(1, 2, 3), { reply: false, tick: 42 });
  const p = B.parseFrame(f);
  assert.equal(p.command, 9);
  assert.deepEqual([...p.payload], [1, 2, 3]);
  const bad = Uint8Array.from(f); bad[63] ^= 0xFF;
  assert.throws(() => B.parseFrame(bad), /checksum/i);
  const badMagic = Uint8Array.from(f); badMagic[0] = 0xA5;
  assert.throws(() => B.parseFrame(badMagic), /magic/i);
  assert.ok(B.isBootloaderFrame(f));
});

// ---- .k2 container --------------------------------------------------------------------------

test("S-box is the k2_flasher FIRMWARE_DECODE_TABLE (bijective, spot values)", () => {
  assert.equal(B.DECODE_TABLE.length, 256);
  assert.equal(new Set(B.DECODE_TABLE).size, 256);
  assert.equal(B.DECODE_TABLE[0], 0x1a);
  assert.equal(B.DECODE_TABLE[0x44], 0x91);
  assert.equal(B.DECODE_TABLE[0xff], 0x89);
});

test("Firmware.fromApp reproduces k2_flasher.Firmware.from_app byte for byte", () => {
  const app = new Uint8Array(3076).fill(0xFF);
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);   // sp=0x20020000, reset=0x08003801
  const fw = B.Firmware.fromApp(app, "2026-01-15", "2.3");
  assert.equal(toHex(fw.source), FW_SOURCE_HEX);
  assert.equal(fw.app.length, 3076);
  assert.equal(fw.version, "2.3");
  assert.equal(fw.dateStr, "2026-01-15");
  assert.deepEqual(fw.eraseAddresses().map((a) => "0x" + a.toString(16)), ["0x8003800", "0x8004000"]);
});

test("Firmware.parse validates and rejects tampered containers", () => {
  const fw = B.Firmware.parse(fromHex(FW_SOURCE_HEX));
  assert.equal(fw.version, "2.3");
  assert.equal(toHex(fw.source), FW_SOURCE_HEX);
  const tampered = fromHex(FW_SOURCE_HEX); tampered[100] ^= 0x01;
  assert.throws(() => B.Firmware.parse(tampered));
  assert.throws(() => B.Firmware.parse(fromHex("00".repeat(48))), /magic/i);
});

// The native data area (settings, e-marker records, assets) starts at NVRAM_START and is never
// erased or written: the erase span, counted in 2 KB sectors from APP_START over the container
// length, must end at or below it.
test("Firmware.parse refuses an image whose erase span reaches the native data area", () => {
  const limit = B.NVRAM_START - B.APP_START;   // container bytes that still end exactly at NVRAM_START
  assert.equal(limit, 0x080E0000 - 0x08003800);
  const make = (containerLen) => {
    const app = new Uint8Array(containerLen - B.HEADER_SIZE).fill(0x5A);   // no trailing 0xFF to trim
    app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
    app.set([0xFF, 0xFF, 0xFF, 0xFF], B.MARKER_OFFSET);
    app[2048] = 0xFF;
    return B.Firmware.fromApp(app, "2026-01-15", "1.0");
  };
  const fits = make(limit);
  assert.equal(fits.source.length, limit);
  const last = fits.eraseAddresses().at(-1);
  assert.equal(last + B.SECTOR_SIZE, B.NVRAM_START);
  assert.throws(() => make(limit + 4), /native data area/);
  assert.ok(B.FS_START < B.NVRAM_START && B.FS_START > B.APP_START);
  assert.equal(B.FS_START, 0x080B3000);
});

test("Firmware.load auto-detects a raw .bin by its vector table", () => {
  const app = new Uint8Array(3076).fill(0xFF);
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
  const fw = B.Firmware.load(app);
  assert.equal(fw.app.length, 3076);
  const k2 = B.Firmware.load(fromHex(FW_SOURCE_HEX));
  assert.equal(toHex(k2.source), FW_SOURCE_HEX);
});

// ---- link -----------------------------------------------------------------------------------

function fakeLink() {
  const sent = [];
  const link = new B.BootLink({ send: (f) => { sent.push(B.parseFrame(f)); } }, { timeoutMs: 50 });
  return { link, sent };
}

test("command() sends the request and resolves on the ack frame", async () => {
  const { link, sent } = fakeLink();
  const t = link.command(3);
  await new Promise((r) => setImmediate(r));          // let the send happen
  assert.equal(sent.length, 1);
  assert.equal(sent[0].command, 3);
  link.feed(B.buildFrame(2, u8(), { reply: true, tick: 1 }));   // the ack
  await t;
});

test("command() rejects on an error frame (cmd 1)", async () => {
  const { link } = fakeLink();
  const t = link.command(3);
  await new Promise((r) => setImmediate(r));
  link.feed(B.buildFrame(1, u8(9, 9), { reply: true, tick: 1 }));
  await assert.rejects(t, /rejected/i);
});

test("readMemory() sends one READ and reassembles the 40-byte chunks", async () => {
  const data = u8(...Array.from({ length: 100 }, (_, i) => i & 0xFF));
  const { link, sent } = fakeLink();
  const t = link.readMemory(0x08003000, 100);
  await new Promise((r) => setImmediate(r));
  assert.equal(sent.length, 1);
  assert.equal(sent[0].command, 11);
  link.feed(B.buildFrame(11, data.slice(0, 40), { reply: true, tick: 1 }));
  link.feed(B.buildFrame(11, data.slice(40, 80), { reply: true, tick: 2 }));
  link.feed(B.buildFrame(11, data.slice(80, 100), { reply: true, tick: 3 }));
  const got = await t;
  assert.deepEqual([...got], [...data]);
});

test("log frames (cmd 21) go to onLog and never answer a pending command", async () => {
  const { link } = fakeLink();
  const logs = [];
  link.onLog = (p) => logs.push(String.fromCharCode(...p));
  const t = link.command(5);
  await new Promise((r) => setImmediate(r));
  link.feed(B.buildFrame(21, u8(...Array.from("hello", (c) => c.charCodeAt(0)), 0), { reply: true, tick: 1 }));
  link.feed(B.buildFrame(2, u8(), { reply: true, tick: 2 }));
  await t;
  assert.deepEqual(logs, ["hello\0"]);
});

// ---- end-to-end flash against a fake bootloader ----------------------------------------------

test("flash() runs handshake->erase->write->verify->commit->reboot and lands the app in fake flash", async () => {
  const mem = new Map();   // addr -> byte
  const rd = (a) => (mem.has(a) ? mem.get(a) : 0xFF);
  const wr = (a, v) => mem.set(a, v);
  // bootloader identity strings at NAME_START (four NUL-separated parts)
  const names = "K2\0WITRN\0K2(C)WITRN\0x\0";
  for (let i = 0; i < names.length; i++) wr(B.NAME_START + i, names.charCodeAt(i));
  wr(B.APP_START + 2048, 0x11);   // installed version 1.1

  const sent = [];
  const link = new B.BootLink({
    send: (f) => {
      const p = B.parseFrame(f);
      sent.push(p.command);
      if (p.command === B.CMD.HANDSHAKE || p.command === B.CMD.LOG_RESET ||
          p.command === B.CMD.BEGIN_SESSION || p.command === B.CMD.END_SESSION || p.command === B.CMD.REBOOT) {
        link.feed(B.buildFrame(B.CMD.ACK, u8(), { reply: true, tick: 1 }));
      } else if (p.command === B.CMD.WRITE) {
        const addr = (p.payload[0] | p.payload[1] << 8 | p.payload[2] << 16 | p.payload[3] << 24) >>> 0;
        for (let i = 0; i < p.payload[4]; i++) wr(addr + i, p.payload[5 + i]);
      } else if (p.command === B.CMD.READ) {
        const addr = (p.payload[0] | p.payload[1] << 8 | p.payload[2] << 16 | p.payload[3] << 24) >>> 0;
        const size = p.payload[4] | p.payload[5] << 8;
        for (let off = 0; off < size; off += B.WRITE_SIZE) {
          const n = Math.min(B.WRITE_SIZE, size - off);
          link.feed(B.buildFrame(B.CMD.READ, u8(...Array.from({ length: n }, (_, i) => rd(addr + off + i))), { reply: true, tick: 1 }));
        }
      } else if (p.command === B.CMD.READ_MARKER) {
        const addr = (p.payload[0] | p.payload[1] << 8 | p.payload[2] << 16 | p.payload[3] << 24) >>> 0;
        link.feed(B.buildFrame(B.CMD.READ_MARKER, u8(rd(addr), rd(addr + 1), rd(addr + 2), rd(addr + 3)), { reply: true, tick: 1 }));
      }
      // ERASE (8) is fire-and-forget like the real bootloader
    },
  }, { timeoutMs: 500 });

  const app = new Uint8Array(3076).fill(0xFF);
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
  const fw = B.Firmware.fromApp(app, "2026-01-15", "2.3");
  const stages = [];
  const ident = await link.flash(fw, { progress: (s, c, t) => stages.push(s + ":" + c + "/" + t) });

  assert.equal(ident.currentVersion, "1.1");
  assert.deepEqual(ident.bootStrings, ["K2", "WITRN", "K2(C)WITRN"]);
  // the app image landed at APP_START, the commit marker was written
  for (let i = 0; i < fw.app.length; i++) {
    if (i >= B.MARKER_OFFSET && i < B.MARKER_OFFSET + 4) continue;   // overwritten by the commit marker below
    assert.equal(rd(B.APP_START + i), fw.app[i], "app byte " + i);
  }
  assert.deepEqual([rd(B.APP_START + B.MARKER_OFFSET), rd(B.APP_START + B.MARKER_OFFSET + 1),
                    rd(B.APP_START + B.MARKER_OFFSET + 2), rd(B.APP_START + B.MARKER_OFFSET + 3)],
                   [0xFE, 0xFF, 0xFF, 0xFF]);
  // command order: handshake, log-reset, session, 2 erases, end, session, writes, end, reads, session, write, marker, end, reboot
  assert.deepEqual(sent.filter((c) => c === B.CMD.ERASE).length, 2);
  assert.equal(sent[0], B.CMD.HANDSHAKE);
  assert.equal(sent[sent.length - 1], B.CMD.REBOOT);
  assert.ok(stages.includes("complete:" + fw.app.length + "/" + fw.app.length));
});

// ---- backup against a fake bootloader ---------------------------------------------------------

// A 1 MB fake flash behind the real frame format. Reads reaching past `refuseFrom` are not answered
// (a timeout, like a refusing bootloader) when `silent`, else answered with an error frame.
function fakeBootloader({ flash, refuseFrom = Infinity, silent = true }) {
  const le = (p) => (p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24) >>> 0;
  const reads = [];
  const link = new B.BootLink({
    send: (f) => {
      const p = B.parseFrame(f);
      if (p.command === B.CMD.HANDSHAKE) {
        link.feed(B.buildFrame(B.CMD.ACK, u8(), { reply: true, tick: 1 }));
      } else if (p.command === B.CMD.READ) {
        const addr = le(p.payload), size = p.payload[4] | p.payload[5] << 8;
        reads.push([addr, size]);
        if (addr + size > refuseFrom) {
          if (!silent) link.feed(B.buildFrame(B.CMD.ERROR, u8(1), { reply: true, tick: 1 }));
          return;
        }
        for (let off = 0; off < size; off += B.WRITE_SIZE) {
          const n = Math.min(B.WRITE_SIZE, size - off);
          const a = addr - B.FLASH_START + off;
          link.feed(B.buildFrame(B.CMD.READ, flash.slice(a, a + n), { reply: true, tick: 1 }));
        }
      } else if (p.command === B.CMD.READ_MARKER) {
        const a = le(p.payload) - B.FLASH_START;
        link.feed(B.buildFrame(B.CMD.READ_MARKER, flash.slice(a, a + 4), { reply: true, tick: 1 }));
      }
    },
  }, { timeoutMs: 20 });
  return { link, reads };
}

const SIG_OFFSET = 2064;                                         // 0x08004010 - APP_START: the model signature word
function makeFlash(sig = [0x4B, 0x32, 0x30, 0x30]) {             // "K200"; a DFU entry leaves 00000000 there
  const flash = new Uint8Array(B.FLASH_END - B.FLASH_START).fill(0xFF);
  flash.fill(0xB0, 0, 0x1000);                                   // bootloader bytes below the name block
  const names = "K2\0WITRN\0K2(C)WITRN\0x\0";
  for (let i = 0; i < names.length; i++) flash[B.NAME_START - B.FLASH_START + i] = names.charCodeAt(i);
  const app = new Uint8Array(B.NVRAM_START - B.APP_START).fill(0xFF);
  for (let i = 8; i < 0x6000; i++) app[i] = (i * 7) & 0xFF;      // some code
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);  // sp, reset
  app[2048] = 0x27;                                              // version 2.7
  app.set(sig, SIG_OFFSET);
  app.set([0xFE, 0xFF, 0xFF, 0xFF], B.MARKER_OFFSET);            // committed
  app.fill(0x6C, B.FS_START - B.APP_START);                      // a used littlefs volume, through its last block
  flash.set(app, B.APP_START - B.FLASH_START);
  for (let i = 0; i < 0x4000; i++) flash[B.NVRAM_START - B.FLASH_START + i] = 0xA5 ^ (i & 0xFF);   // native storage
  return { flash, app };
}
const sha = async (b) => toHex(new Uint8Array(await crypto.subtle.digest("SHA-256", b)));
const a8 = (a) => "0x" + a.toString(16).toUpperCase().padStart(8, "0");

test("backup() reads the app area twice, splits it and builds a flashable recovery container", async () => {
  const { flash, app } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const stages = new Set();
  const r = await link.backup({ appOnly: true, now: new Date("2026-11-08T10:00:00Z"), progress: (s) => stages.add(s) });

  assert.equal(r.flash.length, B.APP_LIMIT - B.FLASH_START);
  assert.deepEqual([...r.flash], [...flash.slice(0, r.flash.length)]);
  assert.equal(r.bootloader.length, B.APP_START - B.FLASH_START);
  assert.deepEqual([...r.app], [...flash.slice(B.APP_START - B.FLASH_START, B.APP_LIMIT - B.FLASH_START)]);
  assert.equal(r.nvram.length, B.APP_LIMIT - B.NVRAM_START);   // default mode: only the part below APP_LIMIT
  assert.equal(r.manifest.complete, true);
  assert.equal(r.manifest.mode, "app");
  assert.equal(r.manifest.backup_kind, "app-only");
  assert.equal(r.manifest.two_pass_verified, true);
  assert.equal(r.manifest.flash_range, "0x08000000-0x080E3000");
  assert.equal(r.manifest.app_sha256, await sha(r.app));
  assert.equal(r.manifest.flash_sha256, await sha(r.flash));
  assert.equal(r.manifest.identity.current_version, "2.7");
  assert.equal(r.manifest.identity.current_marker, "feffffff");
  assert.deepEqual(r.manifest.identity.boot_strings, ["K2", "WITRN", "K2(C)WITRN"]);
  assert.equal(r.manifest.native_nvram_file, "native_nvram.bin");
  assert.deepEqual([...stages].filter((s) => s.startsWith("backup")),
                   ["backup-read", "backup-verify", "backup-recovery", "backup-complete"]);

  // the recovery container re-parses, holds the application only (never the native area), uncommitted
  assert.equal(r.recoveryName, "K2_RECOVERY_2.7.k2");
  const fw = B.Firmware.parse(r.recovery.source);
  assert.equal(fw.version, "2.7");
  assert.equal(fw.dateStr, "2026-11-08");
  const want = app.slice(0, 0x6000);
  want.fill(0xFF, B.MARKER_OFFSET, B.MARKER_OFFSET + 4);
  assert.deepEqual([...fw.app.slice(0, 0x6000)], [...want]);
  // the used littlefs volume (non-0xFF up to NVRAM_START) must not lengthen it: it ends by FS_START
  assert.ok(B.APP_START + fw.source.length <= B.FS_START);
  assert.equal(r.app[B.NVRAM_START - B.APP_START - 1], 0x6C);   // the volume is in app.bin, just not in the container
  assert.equal(r.manifest.recovery_range, "0x08003800-0x080B3000");
  assert.equal(JSON.parse(r.manifestText).recovery_file, r.recoveryName);
});

test("backup() fails on a mismatch between the two passes", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const real = link.transport.send;
  let atStart = 0;
  link.transport.send = (f) => {
    const p = B.parseFrame(f);
    // reads of 0x08000000 in order: pass 1, then pass 2 (identify reads other addresses)
    if (p.command === B.CMD.READ && p.payload[0] === 0 && p.payload[1] === 0 && p.payload[2] === 0 && p.payload[3] === 0x08) {
      if (++atStart === 2) flash[5] ^= 0xFF;
    }
    return real(f);
  };
  await assert.rejects(link.backup({}), /verification mismatch at 0x08000005/);
});

test("backup() refuses when the bootloader strings are not a K2", async () => {
  const { flash } = makeFlash();
  flash.fill(0x00, B.NAME_START - B.FLASH_START, B.NAME_START - B.FLASH_START + 256);
  flash.set([...Buffer.from("X\0Y\0Z\0w\0")], B.NAME_START - B.FLASH_START);
  const { link } = fakeBootloader({ flash });
  await assert.rejects(link.backup({}), /not confirmed as K2/);
});

test("backup({full}) keeps the readable prefix when the bootloader refuses above APP_LIMIT", async () => {
  const { flash } = makeFlash();
  const edge = B.APP_LIMIT + B.READ_CHUNK + 8;   // one full chunk and 8 bytes of the next are served
  for (const silent of [true, false]) {
    const { link } = fakeBootloader({ flash, refuseFrom: edge, silent });
    const r = await link.backup({ full: true });
    assert.equal(r.manifest.complete, false);
    assert.equal(r.manifest.mode, "full");
    assert.equal(r.flash.length, edge - B.FLASH_START);
    assert.equal(r.manifest.refused_at, a8(edge));
    assert.equal(r.manifest.last_readable_address, a8(edge - 1));
    assert.deepEqual([...r.flash], [...flash.slice(0, r.flash.length)]);
    assert.equal(r.nvram.length, edge - B.NVRAM_START);
  }
});

test("backup({full}) reads the whole 1 MB when nothing is refused", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ full: true });
  assert.equal(r.manifest.complete, true);
  assert.equal(r.flash.length, B.FLASH_END - B.FLASH_START);
  assert.equal(r.nvram.length, B.FLASH_END - B.NVRAM_START);
  assert.equal(r.manifest.refused_at, null);
});

test("a refusal below APP_LIMIT is an error, not a partial backup", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash, refuseFrom: 0x08010000 });
  await assert.rejects(link.backup({}), /timed out/i);
});

// ---- the model signature: a DFU entry zeroes "K200" in the app, the recovery container must carry it ----

test("backup() restores K200 in the recovery container when a DFU entry zeroed it, and keeps the raw dump", async () => {
  const { flash } = makeFlash([0, 0, 0, 0]);
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({});
  assert.deepEqual([...r.app.slice(SIG_OFFSET, SIG_OFFSET + 4)], [0, 0, 0, 0]);          // app.bin is the faithful read
  const o = B.APP_START - B.FLASH_START + SIG_OFFSET;
  assert.deepEqual([...r.flash.slice(o, o + 4)], [0, 0, 0, 0]);
  const fw = B.Firmware.parse(r.recovery.source);
  assert.equal(Buffer.from(fw.app.slice(SIG_OFFSET, SIG_OFFSET + 4)).toString("latin1"), "K200");
  assert.equal(r.manifest.signature_restored, true);
  assert.equal(JSON.parse(r.manifestText).signature_restored, true);
  // nothing else in the image changed: code before and after the word is as read
  assert.deepEqual([...fw.app.slice(0, SIG_OFFSET)], [...r.app.slice(0, SIG_OFFSET)]);
  assert.deepEqual([...fw.app.slice(SIG_OFFSET + 4, B.MARKER_OFFSET)], [...r.app.slice(SIG_OFFSET + 4, B.MARKER_OFFSET)]);
});

test("backup() leaves an intact K200 alone", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({});
  const fw = B.Firmware.parse(r.recovery.source);
  assert.equal(Buffer.from(fw.app.slice(SIG_OFFSET, SIG_OFFSET + 4)).toString("latin1"), "K200");
  assert.equal(r.manifest.signature_restored, false);
});

test("backup() refuses to build a recovery when the signature word is neither K200 nor zero", async () => {
  const { flash } = makeFlash([0x4B, 0x32, 0x30, 0x31]);                                  // "K201"
  const { link } = fakeBootloader({ flash });
  await assert.rejects(link.backup({}), /signature.*0x08004010.*4B323031/i);
});

test("backup() will not restore K200 into an image whose vector table is not an app's", async () => {
  const { flash } = makeFlash([0, 0, 0, 0]);
  flash.fill(0xFF, B.APP_START - B.FLASH_START, B.APP_START - B.FLASH_START + 8);        // blank vector table
  const { link } = fakeBootloader({ flash });
  await assert.rejects(link.backup({}), /vector table/i);
});

// ---- the backup is full by default, one ZIP carries it ------------------------------------------

const NOW = new Date("2026-11-08T10:00:00Z");

test("backup() with no options is a full 1 MB backup", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ now: NOW });
  assert.equal(r.flash.length, B.FLASH_END - B.FLASH_START);
  assert.equal(r.manifest.mode, "full");
  assert.equal(r.manifest.backup_kind, "full");
  assert.equal(r.manifest.complete, true);
  assert.equal(r.nvram.length, B.FLASH_END - B.NVRAM_START);
});

test("backup({full: true}) is still accepted and means the same", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ full: true, now: NOW });
  assert.equal(r.manifest.backup_kind, "full");
});

test("backup({appOnly}) reads the application area only and says so", async () => {
  const { flash } = makeFlash();
  const { link, reads } = fakeBootloader({ flash });
  const r = await link.backup({ appOnly: true, now: NOW });
  assert.equal(r.manifest.backup_kind, "app-only");
  assert.equal(r.manifest.mode, "app");
  assert.ok(reads.every(([a, n]) => a + n <= B.APP_LIMIT));
});

test("a refusal above the app area gives backup_kind partial", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash, refuseFrom: B.APP_LIMIT + B.READ_CHUNK });
  const r = await link.backup({ now: NOW });
  assert.equal(r.manifest.backup_kind, "partial");
  assert.equal(r.manifest.complete, false);
});

test("the manifest carries the sha256 of native_nvram.bin when there is one", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ now: NOW });
  assert.equal(r.manifest.native_nvram_sha256, await sha(r.nvram));
  assert.equal(JSON.parse(r.manifestText).native_nvram_sha256, await sha(r.nvram));
});

// ---- ZIP writer / reader ----------------------------------------------------------------------

const PY_ZIP_CHECK = [
  "import sys, zipfile, hashlib, json",
  "z = zipfile.ZipFile(sys.argv[1])",
  "bad = z.testzip()",
  "print(json.dumps({'bad': bad, 'files': {i.filename: [hashlib.sha256(z.read(i)).hexdigest(), i.compress_type, i.file_size]",
  "                                       for i in z.infolist()}}))",
].join("\n");
const havePython = spawnSync("python3", ["-I", "-c", "pass"]).status === 0;
function pythonView(zipBytes) {
  const dir = mkdtempSync(path.join(os.tmpdir(), "k2zip-"));
  const p = path.join(dir, "t.zip");
  writeFileSync(p, zipBytes);
  const r = spawnSync("python3", ["-I", "-c", PY_ZIP_CHECK, p], { encoding: "utf8" });
  assert.equal(r.status, 0, r.stderr);
  return JSON.parse(r.stdout);
}
const text = (s) => new TextEncoder().encode(s);
const sha256Sync = (b) => createHash("sha256").update(b).digest("hex");

test("crc32 matches the standard check value", () => {
  assert.equal(B.crc32(text("123456789")), 0xCBF43926);
  assert.equal(B.crc32(new Uint8Array(0)), 0);
});

test("makeZip output reads back through readZip, names and bytes intact", async () => {
  const big = new Uint8Array(70000).map((_, i) => (i * 13) & 0xFF);
  const zip = B.makeZip([{ name: "a.bin", data: big }, { name: "empty.txt", data: new Uint8Array(0) },
                         { name: "manifest.json", data: text('{"k":"ü"}\n') }], NOW);
  const back = await B.readZip(zip);
  assert.deepEqual(back.map((e) => e.name), ["a.bin", "empty.txt", "manifest.json"]);
  assert.deepEqual([...back[0].data], [...big]);
  assert.equal(back[1].data.length, 0);
  assert.equal(new TextDecoder().decode(back[2].data), '{"k":"ü"}\n');
});

test("makeZip is a store-only ZIP with the layout the format asks for", () => {
  const zip = B.makeZip([{ name: "x", data: text("hello") }], NOW);
  const dv = new DataView(zip.buffer, zip.byteOffset, zip.byteLength);
  assert.equal(dv.getUint32(0, true), 0x04034B50);                 // local header
  assert.equal(dv.getUint16(8, true), 0);                          // method: stored
  assert.equal(dv.getUint32(14, true), B.crc32(text("hello")));
  assert.equal(dv.getUint32(18, true), 5);
  assert.equal(dv.getUint32(22, true), 5);
  assert.equal(dv.getUint32(zip.length - 22, true), 0x06054B50);   // end of central directory, no comment
  assert.equal(dv.getUint16(zip.length - 22 + 10, true), 1);       // entries
  const cdOffset = dv.getUint32(zip.length - 22 + 16, true);
  assert.equal(dv.getUint32(cdOffset, true), 0x02014B50);          // central directory header
  // 2026-11-08 10:00:00 as MS-DOS time/date
  assert.equal(dv.getUint16(10, true), (10 << 11) | (0 << 5) | 0);
  assert.equal(dv.getUint16(12, true), ((2026 - 1980) << 9) | (11 << 5) | 8);
});

test("Python's zipfile opens the ZIP and sees the same bytes", { skip: !havePython }, () => {
  const big = new Uint8Array(200000).map((_, i) => (i * 7 + (i >> 8)) & 0xFF);
  const view = pythonView(B.makeZip([{ name: "flash.bin", data: big }, { name: "manifest.json", data: text("{}\n") }], NOW));
  assert.equal(view.bad, null);
  assert.deepEqual(Object.keys(view.files), ["flash.bin", "manifest.json"]);
  assert.equal(view.files["flash.bin"][0], sha256Sync(big));
  assert.equal(view.files["flash.bin"][1], 0);                     // stored
  assert.equal(view.files["flash.bin"][2], 200000);
});

test("readZip also reads a deflated ZIP made by another tool", { skip: !havePython }, async () => {
  const dir = mkdtempSync(path.join(os.tmpdir(), "k2zip-"));
  const p = path.join(dir, "d.zip");
  const r = spawnSync("python3", ["-I", "-c",
    "import sys, zipfile\nwith zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:\n z.writestr('K2_RECOVERY_2.7.k2', b'abc' * 5000)\n z.writestr('manifest.json', '{}')",
    p], { encoding: "utf8" });
  assert.equal(r.status, 0, r.stderr);
  const back = await B.readZip(new Uint8Array(readFileSync(p)));
  assert.deepEqual(back.map((e) => e.name), ["K2_RECOVERY_2.7.k2", "manifest.json"]);
  assert.equal(back[0].data.length, 15000);
  assert.equal(new TextDecoder().decode(back[0].data.slice(0, 6)), "abcabc");
});

test("readZip rejects bytes that are not a ZIP and a damaged entry", async () => {
  await assert.rejects(B.readZip(text("not a zip file at all, just text")), /not a ZIP/i);
  const zip = B.makeZip([{ name: "x", data: text("hello world") }], NOW);
  zip[30 + 1 + 3] ^= 0x01;                                         // a data byte
  await assert.rejects(B.readZip(zip), /CRC/i);
});

test("backup() packs everything into one K2_BACKUP ZIP that Python can open", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ now: NOW });
  assert.equal(r.zipName, "K2_BACKUP_2.7_20261108T100000Z.zip");
  const back = await B.readZip(r.zip);
  const byName = Object.fromEntries(back.map((e) => [e.name, e.data]));
  assert.deepEqual(Object.keys(byName).sort(),
                   ["K2_RECOVERY_2.7.k2", "app.bin", "bootloader.bin", "flash.bin", "manifest.json", "native_nvram.bin"]);
  assert.deepEqual([...byName["flash.bin"]], [...r.flash]);
  assert.deepEqual([...byName["app.bin"]], [...r.app]);
  assert.deepEqual([...byName["bootloader.bin"]], [...r.bootloader]);
  assert.deepEqual([...byName["native_nvram.bin"]], [...r.nvram]);
  assert.deepEqual([...byName["K2_RECOVERY_2.7.k2"]], [...r.recovery.source]);
  assert.equal(new TextDecoder().decode(byName["manifest.json"]), r.manifestText);
  if (havePython) {
    const view = pythonView(r.zip);
    assert.equal(view.bad, null);
    assert.equal(view.files["flash.bin"][0], r.manifest.flash_sha256);
    assert.equal(view.files["native_nvram.bin"][0], r.manifest.native_nvram_sha256);
  }
});

test("a partial backup still packs what was read, with its manifest", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash, refuseFrom: B.APP_LIMIT + B.READ_CHUNK });
  const r = await link.backup({ now: NOW });
  const back = await B.readZip(r.zip);
  const m = JSON.parse(new TextDecoder().decode(back.find((e) => e.name === "manifest.json").data));
  assert.equal(m.backup_kind, "partial");
  assert.equal(back.find((e) => e.name === "flash.bin").data.length, r.flash.length);
});

test("recoveryFromBackupZip picks the recovery container out of a backup ZIP", async () => {
  const { flash } = makeFlash();
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ now: NOW });
  const got = await B.recoveryFromBackupZip(r.zip);
  assert.equal(got.name, "K2_RECOVERY_2.7.k2");
  assert.deepEqual([...got.data], [...r.recovery.source]);
  await assert.rejects(B.recoveryFromBackupZip(B.makeZip([{ name: "flash.bin", data: text("x") }], NOW)), /no K2_RECOVERY/);
});

test("isZip tells a ZIP from a .k2 container", () => {
  assert.equal(B.isZip(B.makeZip([], NOW)), true);
  assert.equal(B.isZip(B.makeZip([{ name: "a", data: text("b") }], NOW)), true);
  assert.equal(B.isZip(u8(0xDE, 0xAD, 0xBE, 0xEF)), false);
});

// ---- the full version string (tools/k2_flasher.py FWID_*, src/core/version.cpp) ---------------------------

const FULL = "1.0.0-435-26b245ac";
const record = (txt, magic = "K2FW") => {
  const r = new Uint8Array(B.FWID_SIZE);
  for (let i = 0; i < magic.length; i++) r[i] = magic.charCodeAt(i);
  for (let i = 0; i < txt.length; i++) r[4 + i] = txt.charCodeAt(i);
  return r;
};
const appWithRecord = (txt, verByte = 0x10) => {
  const app = new Uint8Array(3076 + B.FWID_SIZE).fill(0xFF);
  app.set([0x00, 0x00, 0x02, 0x20, 0x01, 0x38, 0x00, 0x08], 0);
  app[2048] = verByte;
  if (txt !== null) app.set(record(txt), B.FWID_OFFSET);
  return app;
};

test("the version record sits at the start of .text, 0x08004404", () => {
  assert.equal(B.APP_START + B.FWID_OFFSET, 0x08004404);
  assert.equal(B.FWID_SIZE, 48);
});

test("fwidText reads the record, and nothing else", () => {
  assert.equal(B.fwidText(record(FULL)), FULL);
  assert.equal(B.fwidText(record(FULL + "-dirty")), FULL + "-dirty");
  assert.equal(B.fwidText(new Uint8Array(48).fill(0xFF)), null);
  assert.equal(B.fwidText(new Uint8Array(48)), null);
  assert.equal(B.fwidText(record("1.0", "K2FX")), null);
  assert.equal(B.fwidText(record("")), null);
  assert.equal(B.fwidText(record("\x01\x02abc")), null);
  assert.equal(B.fwidText(u8(0x4B, 0x32, 0x46)), null);
  assert.equal(B.versionLabel(0x10, record(FULL)), FULL);
  assert.equal(B.versionLabel(0x10, new Uint8Array(48).fill(0xFF)), "1.0");       // old firmware: the nibbles
});

test("a package shows the full string when the image has the record, major.minor when it does not", () => {
  assert.equal(B.Firmware.fromApp(appWithRecord(FULL), "2026-11-11").version, FULL);
  assert.equal(B.Firmware.fromApp(appWithRecord(null, 0x27), "2026-11-11").version, "2.7");
});

test("fromApp with the full string puts only major.minor into an erased version byte", () => {
  const app = appWithRecord(null); app[2048] = 0xFF;
  assert.equal(B.Firmware.fromApp(app, "2026-11-11", FULL).app[2048], 0x10);
  assert.equal(B.Firmware.fromApp(app, "2026-11-11", "1.2.3-0-abcdef12-dirty").app[2048], 0x12);
  assert.equal(B.Firmware.fromApp(appWithRecord(FULL, 0x10), "2026-11-11", "3.4.0-1-abcdef12").app[2048], 0x10);   // never touches a set byte
});

test("backup() names the installed full version, and old firmware keeps its nibbles", async () => {
  const { flash } = makeFlash();
  flash.set(record(FULL), B.APP_START - B.FLASH_START + B.FWID_OFFSET);
  const { link } = fakeBootloader({ flash });
  const r = await link.backup({ appOnly: true, now: new Date("2026-11-11T10:00:00Z") });
  assert.equal(r.manifest.identity.current_version, FULL);
  assert.ok(r.zipName.startsWith("K2_BACKUP_" + FULL + "_"));
  const old = fakeBootloader({ flash: makeFlash().flash }).link;
  assert.equal((await old.backup({ appOnly: true, now: new Date("2026-11-11T10:00:00Z") })).manifest.identity.current_version, "2.7");
});
