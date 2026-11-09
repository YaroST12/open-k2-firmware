// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The results page reads the K2 data frame ("K2:<tag><schema>:<base45>", what the QR on the K2's boot screen and the Share
// view's "K2 data" code carry) as well as the link: docs/site/r/card.js splitLink() does the frame (the twin of
// tools/k2_qr.py parse_part / assemble / decode_texts) and records.js read() hands the record to the same decoders and views.
// The record bytes are held against the firmware's own golden frames, tests/fixtures/qrshare.tsv.
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { crc32 } from "node:zlib";

const require = createRequire(import.meta.url);
const K2Card = require("../docs/site/r/card.js");
const K2Records = require("../docs/site/r/records.js");

const fix = Object.fromEntries(readFileSync(new URL("./fixtures/qrshare.tsv", import.meta.url), "utf8")
  .split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t"))
  .map((c) => [c[0], { type: parseInt(c[1], 10), schema: parseInt(c[2], 10), record: Buffer.from(c[3], "hex"), parts: c.slice(4) }]));

// A boot screen as a phone scanner pasted it: one part, schema 1, plain K2B1 text of 211 bytes (tools/k2_qr.py decodes it).
const BOOT_SCAN = "K2:BI1:ZM9W50100GUQX%6IXBZM9PG8E44F3FZ96Z36E2DH.EY$59W5Y9612DGA7$ZCE56O445X7S446X71A6:97L/E9X7%96%K6DOCAX7B44 F6/96VF6O44YUDZ36K DP9E7%EJ44CX7O448DCPR8 KENX7646646646V444X7O44ZED6X7JQ5JQ5HQ5L443PDWV5VKE4X77H70ECTVDL44T.EWV5*QE6X7C58X96*F6C%6L966.DS56JF6C565L6$96E46FPEIQ5946667246$96IL6FPEIQ5A46C565L6$96+Q6JPEIQ5A46Y*6YA8$96SW6FOCIQ5B460H6AA8C$ER2";

// An exact one-part frame built here, so a schema the firmware does not make yet can be tried.
const ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
function frame(type, schema, rec) {
  const head = Buffer.alloc(12);
  head.write("K2");
  head.writeUInt8(type, 2); head.writeUInt8(schema, 3); head.writeUInt8(0, 4); head.writeUInt8(1, 5);
  head.writeUInt16LE(rec.length, 6); head.writeUInt32LE(crc32(rec), 8);
  const f = Buffer.concat([head, rec]);
  const tail = Buffer.alloc(4);
  tail.writeUInt32LE(crc32(f));
  const all = Buffer.concat([f, tail]);
  let out = "";
  for (let i = 0; i < all.length; i += 2) {
    if (i + 1 < all.length) { const n = all[i] * 256 + all[i + 1]; out += ALPHABET[n % 45] + ALPHABET[Math.floor(n / 45) % 45] + ALPHABET[Math.floor(n / 2025)]; }
    else out += ALPHABET[all[i] % 45] + ALPHABET[Math.floor(all[i] / 45)];
  }
  return out;
}

const code = (f) => { try { f(); return "none"; } catch (e) { assert.equal(e.name, "CardError"); return e.code; } };
const same = (a, b) => assert.deepEqual(Buffer.from(a), Buffer.from(b));
const KIND = {1: "boot", 2: "emarker", 3: "pdcaps", 4: "card", 5: "identity"};

test("the boot screen's data code pasted as scanned opens the boot report", () => {
  const r = K2Records.read(BOOT_SCAN);
  assert.equal(r.kind, "boot");
  assert.equal(r.data.fw, "1.0");
  assert.equal(r.data.git, "v1.0-61-g091e80e");
  assert.equal(r.data.t_ms, 2199);
  assert.equal(r.data.vbus_mv, 5113);
  assert.equal(r.data.mode, "plus");
  assert.deepEqual(r.data.keys_pressed, ["PLUS"]);
  assert.equal(r.view.header.title, "K2 boot report");
  assert.ok(r.view.badges.length && r.view.sections.length);
});

test("the same code as a fragment, with or without the address in front, opens the same report", () => {
  const want = K2Records.read(BOOT_SCAN).data;
  assert.deepEqual(K2Records.read("#" + BOOT_SCAN).data, want);
  assert.deepEqual(K2Records.read("https://yarost12.github.io/open-k2-firmware/r/#" + BOOT_SCAN).data, want);
});

test("a browser's %20 for a base45 space in the fragment is read back; a literal %20 pair still reads", () => {
  assert.equal(BOOT_SCAN.includes(" "), true);
  const frag = "#" + BOOT_SCAN.replace(/ /g, "%20");
  assert.deepEqual(K2Records.read(frag).data, K2Records.read(BOOT_SCAN).data);
});

test("blanks around a pasted part are ignored, the spaces inside it are kept", () => {
  const want = K2Records.read(BOOT_SCAN).data;
  for (const text of [BOOT_SCAN + "\n", BOOT_SCAN + "  \r\n", "  \n" + BOOT_SCAN, "#" + BOOT_SCAN + " ", "#" + BOOT_SCAN.replace(/ /g, "%20") + "\n"]) {
    assert.deepEqual(K2Records.read(text).data, want);
  }
  assert.equal(code(() => K2Records.read(BOOT_SCAN.replace(/ /g, ""))), "bad-frame");   // a space is a digit, not padding
});

test("every single-part golden frame gives the firmware's record and the page's own kind", () => {
  for (const [name, fx] of Object.entries(fix)) {
    if (fx.parts.length !== 1 || (fx.type & 0x7F) === 6) continue;
    const plainName = name.replace(/_lz$/, "").replace(/_v2$/, "");
    const l = K2Card.splitLink(fx.parts[0]);
    assert.equal(l.kind, KIND[fx.type & 0x7F], name);
    assert.equal(l.version, String(fx.schema), name);
    if (name.endsWith("_lz")) same(l.record, fix[plainName].record);              // packed: the inflated record
    else if (fx.schema === 2) same(l.record, fix[name.replace(/_v2$/, "")].record);   // schema 2: expanded to the schema 1 record
    else same(l.record, fx.record);
    const r = K2Records.read(fx.parts[0]);
    assert.equal(r.kind, l.kind, name);
    assert.ok(r.view.header.title, name);
  }
});

test("a schema 2 card frame shows the card of the schema 1 frame, and a packed one too", () => {
  const want = JSON.stringify(K2Records.read(fix.card.parts[0]).data);
  assert.equal(K2Records.read(fix.card.parts[0]).kind, "card");
  assert.equal(JSON.stringify(K2Records.read(fix.card_v2.parts[0]).data), want);
  assert.equal(JSON.stringify(K2Records.read(fix.card_lz.parts[0]).data), want);
});

test("a cable of two parts: both lines in any order, with blank lines and CRLF", () => {
  const [a, b] = fix.emk_rows.parts;
  const want = JSON.stringify(K2Records.read(fix.emk_rows_lz.parts[0]).data);
  for (const text of [a + "\n" + b, b + "\n" + a, "\n" + a + "\r\n\r\n" + b + "\r\n", a + "\n" + b + "\n" + a]) {
    const r = K2Records.read(text);
    assert.equal(r.kind, "emarker");
    assert.equal(JSON.stringify(r.data), want);
  }
});

test("a part of a longer record alone says how many parts it has and which are missing", () => {
  const [a, b] = fix.emk_rows.parts;
  assert.equal(code(() => K2Records.read(a)), "incomplete");
  assert.equal(code(() => K2Records.read(b)), "incomplete");
  assert.equal(code(() => K2Records.read(a + "\n" + a)), "incomplete");
  try { K2Records.read(a); } catch (e) {
    assert.match(e.message, /2 parts/);
    assert.match(e.message, /part 2/);
    assert.match(e.message, /k2_qr\.py/);
  }
  try { K2Records.read(b); } catch (e) { assert.match(e.message, /part 1/); }
});

test("the log is not a record the page shows, and says where to read it", () => {
  assert.equal(code(() => K2Records.read(fix.log.parts[0])), "unsupported-kind");
  try { K2Records.read(fix.log.parts[0]); } catch (e) { assert.match(e.message, /k2_qr\.py/); }
});

test("a damaged frame is refused with a plain reason", () => {
  const p = fix.identity.parts[0];
  const at = p.length - 6;
  const flipped = p.slice(0, at) + (p[at] === "A" ? "B" : "A") + p.slice(at + 1);
  assert.equal(code(() => K2Records.read(flipped)), "bad-frame");                       // frame CRC
  assert.equal(code(() => K2Records.read(p.slice(0, -3))), "bad-frame");                // cut short
  assert.equal(code(() => K2Records.read("K2:ID1:abc")), "bad-frame");                  // lower case is not base45
  assert.equal(code(() => K2Records.read("K2:CARD1:ABC")), "bad-frame");
  assert.equal(code(() => K2Records.read("K2:XX1:" + p.split(":")[2])), "bad-frame");   // unknown tag
  assert.equal(code(() => K2Records.read("K2:ID9:" + p.split(":")[2])), "bad-frame");   // prefix does not match the frame
  assert.equal(code(() => K2Records.read(p.replace("ID1", "BI1"))), "bad-frame");
  assert.equal(code(() => K2Records.read("K2:ID1")), "bad-frame");
});

test("parts of different records do not mix", () => {
  const [a] = fix.emk_rows.parts;
  const other = fix.emk_rows.parts[1];
  assert.equal(code(() => K2Records.read(a + "\n" + fix.emk_plain.parts[0])), "bad-frame");
  assert.equal(code(() => K2Records.read(a + "\n" + fix.identity.parts[0])), "bad-frame");
  assert.notEqual(code(() => K2Records.read(a + "\n" + other)), "bad-frame");
});

test("a schema the page does not read is named, not misread", () => {
  const rec = Buffer.from("fw=1.0 g=x p=1");
  assert.equal(K2Records.read("K2:ID1:" + frame(5, 1, rec)).kind, "identity");   // the helper builds what the firmware builds
  assert.equal(code(() => K2Records.read("K2:ID7:" + frame(5, 7, rec))), "bad-version");
  assert.equal(code(() => K2Records.read("K2:PDC2:" + frame(3, 2, rec))), "bad-version");   // schema 2 exists for the card and the cable only
  assert.equal(code(() => K2Records.read("K2:CARD3:" + frame(4, 3, rec))), "bad-version");
});

test("the link still opens and a link given a K2: prefix after other text is not mistaken for a frame", () => {
  assert.equal(code(() => K2Records.read("hello K2:ID1:x")), "bad-link");
});
