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
