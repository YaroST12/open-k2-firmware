// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The results page's decoders for the records that are not a charger test card (docs/site/r/records.js) against the same goldens
// as the card: tests/fixtures/qrlink.tsv (the links the firmware encodes) and tests/fixtures/qrlink_decoded.json (what the
// independent Python decoders, tools/k2_qr.py, give for each; tests/test_qrlink.py pins it).
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const K2Card = require("../docs/site/r/card.js");
const K2Records = require("../docs/site/r/records.js");

const rows = readFileSync(new URL("./fixtures/qrlink.tsv", import.meta.url), "utf8")
  .split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t"));
const golden = Object.fromEntries(rows.map(([name, packed, hex, link, json]) => [name, { link, card: json !== "-" }]));
const pinned = JSON.parse(readFileSync(new URL("./fixtures/qrlink_decoded.json", import.meta.url), "utf8"));
const others = Object.keys(golden).filter((n) => !golden[n].card);
const BASE = "https://yarost12.github.io/open-k2-firmware/r/#";

const plain = (x) => JSON.parse(JSON.stringify(x));   // undefined keys gone, as the pinned file has none

test("every non-card golden decodes to what the Python decoders give", () => {
  assert.equal(others.length, Object.keys(pinned).length);
  for (const name of others) {
    const r = K2Records.read(golden[name].link);
    assert.equal(Number(r.version), pinned[name].version, name);   // '1' rows stay version 1, the cable's v2 rows are '2', 't' is '1'
    assert.equal(r.kind, pinned[name].kind, name);
    assert.deepEqual(plain(r.data), pinned[name].data, name);
  }
});

test("every kind has a golden", () => {
  assert.deepEqual([...new Set(others.map((n) => K2Records.read(golden[n].link).kind))].sort(),
    ["boot", "emarker", "identity", "pdcaps", "text"]);
});

test("a version '2' cable link gives the same record as the version '1' packed link of the same cable", () => {
  const v2 = K2Records.read(golden.emk_rows4.link);
  const v1 = K2Records.read(golden.emk_rows4_lz.link);
  assert.equal(v2.version, "2");
  assert.equal(v1.version, "1");
  assert.deepEqual(plain(v2.data), plain(v1.data));
  assert.deepEqual(v2.view, v1.view);
});

test("the kind 't' row (the QR scan test) decodes to the trimmed text and renders one test tile", () => {
  const r = K2Records.read(golden.qrtest.link);
  assert.equal(r.version, "1");
  assert.equal(r.kind, "text");
  assert.deepEqual(plain(r.data), pinned.qrtest.data);
  assert.equal(r.data.text, "QR scan test v11 3px");   // the record is NUL padded; the text reads up to the first NUL
  assert.equal(r.view.header.title, "QR scan test");
  assert.equal(r.view.badges.length, 1);
  assert.equal(r.view.badges[0].label, "QR scan test");
  assert.equal(r.view.badges[0].status, "QR scan test v11 3px");
});

test("a card link reads through the same entry point as a card", () => {
  const r = K2Records.read(golden.fh22.link);
  assert.equal(r.kind, "card");
  assert.equal(r.view.header.title, K2Card.view(K2Card.parseLink(golden.fh22.link).card).header.title);
});

test("every view has the shape the page draws, and its words are plain", () => {
  for (const name of others) {
    const v = K2Records.read(golden[name].link).view;
    assert.ok(v.header.title && v.header.ident && v.header.facts.length, name);
    assert.ok(v.badges.length && v.sections.length && v.badges_title && v.full_title, name);
    for (const b of v.badges) {
      assert.ok(b.label && b.status !== undefined && ["info", "pass", "partial", "fail", "skip"].includes(b.tone), name);
    }
    for (const s of v.sections) {
      assert.ok(s.title, name);
      assert.ok(s.groups.length || s.empty, name + " " + s.title);
      for (const g of s.groups) for (const r of g.rows) assert.ok(r.label && typeof r.value === "string" && r.value !== "undefined", name);
    }
    const text = JSON.stringify(v);
    assert.doesNotMatch(text, /undefined|NaN|\[object/, name);
  }
});

test("the cable: current, voltage, speed and quirks as a phone shows them", () => {
  const v = K2Records.read(golden.emk_rows2.link).view;
  assert.equal(v.header.title, "OnePlus 100W mag");
  assert.ok(v.badges.some((b) => b.label === "Current"));
  assert.ok(v.badges.some((b) => b.label === "Voltage"));
  assert.ok(v.badges.some((b) => b.label === "USB speed"));
  assert.ok(v.badges.find((b) => b.label === "Response quirks").status.startsWith("2 rows"));
  assert.ok(v.sections.find((s) => s.title === "Response quirks").groups.length === 3);   // policy + 2 rows
  const plainCable = K2Records.read(golden.emk_plain.link).view;
  assert.equal(plainCable.badges.find((b) => b.label === "Response quirks").status, "none");
  assert.equal(plainCable.badges.find((b) => b.label === "Record check").tone, "pass");
});

test("the charger's offers: one tile per PDO, power worked out", () => {
  const d = K2Records.read(golden.pdcaps_epr.link);
  const live = d.data.pdos.filter((p) => p.kind !== "empty");
  assert.equal(d.view.badges.length, live.length);
  assert.ok(d.view.header.facts.includes("EPR"));
  const text = JSON.stringify(d.view);
  assert.match(text, /EPR AVS/);
  assert.match(text, /\b28V\b/);
  const spr = K2Records.read(golden.pdcaps_spr.link).view;
  assert.ok(spr.badges.some((b) => /^5V /.test(b.label)));
  assert.ok(!spr.header.facts.includes("EPR"));
});

test("the firmware and the boot report", () => {
  const id = K2Records.read(golden.identity.link).view;
  assert.ok(id.badges.some((b) => b.label === "Working tree" && b.tone === "partial"), "-dirty is shown as amber");
  const boot = K2Records.read(golden.boot.link);
  assert.equal(boot.data.fw, "1.0");
  assert.ok(boot.view.sections.some((s) => s.title === "Boot decision"));
});

test("a link of a kind this page does not know, a cut link and a damaged record say what is wrong", () => {
  const code = (s) => { try { K2Records.read(s); return "none"; } catch (e) { assert.equal(e.name, "CardError"); return e.code; } };
  const frag = golden.pdcaps_spr.link.slice(BASE.length);
  assert.equal(code(BASE + "1q." + frag.slice(3)), "bad-link");
  assert.equal(code(BASE + "2" + frag.slice(1)), "bad-version");   // version '2' exists only for the card and the cable
  assert.equal(code(BASE + "2t." + frag.slice(3)), "bad-version");  // kind 't' is a version '1' form
  assert.equal(code(BASE + "3" + frag.slice(1)), "bad-version");
  assert.equal(code(BASE + frag.slice(0, 19)), "bad-record");    // the length no longer fits the PDO count
  assert.equal(code(BASE + "1p.AAAA"), "bad-record");
  assert.equal(code(BASE + "1i."), "bad-record");
  assert.equal(code(BASE), "missing");
  assert.equal(code(BASE + "1e." + Buffer.from(new Uint8Array(40)).toString("base64url")), "bad-record");
  assert.equal(code(BASE + "2e." + Buffer.from(new Uint8Array(40)).toString("base64url")), "bad-record");   // not a schema 2 record
  const kinds = { ...K2Card.splitLink(golden.boot.link) };
  assert.equal(kinds.kind, "boot");
  assert.equal(K2Card.splitLink(golden.boot_lz.link).kind, "boot");
});

test("decodePdo agrees with the decoder of tools/pd_trace.py on every type", () => {
  const t = (raw) => K2Records.decodePdo(raw)[1];
  assert.equal(t(0x0001912C), "fixed 5 V 3 A");
  assert.equal(t(0), "empty (all-zero word, padding)");
  assert.equal(t(0xC0A0C8B4 >>> 0).startsWith("PPS"), true);
  assert.match(t(0xD0000000 >>> 0), /^EPR-AVS/);
  assert.match(t(0xE0000000 >>> 0), /^SPR-AVS/);
  assert.match(t(0xF0000000 >>> 0), /^APDO\(reserved\) 0xF0000000/);
  assert.match(t(0x4000A0C8), /^battery/);
  assert.match(t(0x8000A0C8 >>> 0), /^variable/);
});
