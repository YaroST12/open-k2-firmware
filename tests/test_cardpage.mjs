// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
// The results page's decoder and badge model (docs/site/r/card.js) against the firmware's own golden cards
// (tests/fixtures/qrlink.tsv: the record, the link the K2 shows and the card as the firmware's JSON renderer writes it).
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";

const require = createRequire(import.meta.url);
const K2Card = require("../docs/site/r/card.js");

const rows = readFileSync(new URL("./fixtures/qrlink.tsv", import.meta.url), "utf8")
  .split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t"));
const golden = Object.fromEntries(rows.map(([name, packed, hex, link, json]) =>
  [name, { packed: packed === "1", hex, link, json: json === "-" ? null : JSON.parse(json) }]));

const BASE = "https://yarost12.github.io/open-k2-firmware/r/#";
const fromHex = (h) => Uint8Array.from(h.match(/../g).map((x) => parseInt(x, 16)));
const toB64url = (b) => Buffer.from(b).toString("base64url");

// The firmware's JSON writes an EPR level's key as millivolts ("28000V"); tools/report_card.py and this page say "28V".
function normalise(card) {
  const c = JSON.parse(JSON.stringify(card));
  delete c.qc;   // the no-brand-without-VID rule (row B item 5) is landed in the firmware renderer but not yet in tools/report_card.py
  if (c.epr_levels) {
    c.epr_levels = Object.fromEntries(Object.entries(c.epr_levels).map(([k, v]) => [/^\d{5}V$/.test(k) ? `${k.slice(0, 2)}V` : k, v]));
  }
  return c;
}

function recordOf(payload) {   // 0xA6, type 1, length, 0, payload, CRC-16 little endian
  const r = new Uint8Array(4 + payload.length + 2);
  r.set([0xA6, 1, payload.length, 0]);
  r.set(payload, 4);
  const c = K2Card.crc16(r, r.length - 2);
  r[r.length - 2] = c & 0xFF;
  r[r.length - 1] = c >> 8;
  return r;
}

const payloadOf = (name) => { const rec = K2Card.splitLink(golden[name].link).record; return rec.slice(4, 4 + rec[2]); };   // the payload (its length byte says which format), from a version '1' or '2' link

// tests/fixtures/qrlink_v5.tsv is frozen: the links of the cards made before the UFCS modes (184 byte records, format 5).
const v5 = Object.fromEntries(readFileSync(new URL("./fixtures/qrlink_v5.tsv", import.meta.url), "utf8")
  .split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t"))
  .map(([name, packed, hex, link, json]) => [name, { packed: packed === "1", hex, link, json: JSON.parse(json) }]));

test("every golden link decodes to the card the firmware wrote", () => {
  for (const [name, g] of Object.entries(golden)) {
    assert.ok(g.link.startsWith(BASE), name);
    if (!g.json) continue;   // the other kinds: tests/test_recordspage.mjs
    const r = K2Card.parseLink(g.link);
    assert.equal(r.version, g.link.charAt(BASE.length), name);   // the version digit of the link: '1' or '2'
    assert.equal(r.kind, g.packed ? "z" : "c", name);
    assert.deepEqual(normalise(r.card), normalise(g.json), name);
  }
});

test("a version '2' card link shows the same card and the same view as the version '1' link of the same record", () => {
  for (const name of ["fh22", "allpass", "fh22_full", "ufcs_modes", "fh22_repeat"]) {
    const v2 = K2Card.parseLink(golden[name].link);
    assert.equal(v2.version, "2", name);
    const v1 = K2Card.parseLink(BASE + "1c." + toB64url(recordOf(payloadOf(name))));
    assert.equal(v1.version, "1", name);
    assert.deepEqual(v2.card, v1.card, name);
    assert.deepEqual(K2Card.view(v2.card), K2Card.view(v1.card), name);
  }
});

test("the fragment alone, with and without the address, reads the same", () => {
  const g = golden.fh22;
  const frag = g.link.slice(BASE.length);
  assert.deepEqual(K2Card.parseLink("#" + frag).card, K2Card.parseLink(g.link).card);
  assert.deepEqual(K2Card.parseLink(frag).card, K2Card.parseLink(g.link).card);
  assert.deepEqual(K2Card.parseLink("  " + g.link + "\n").card, K2Card.parseLink(g.link).card);
});

test("packed and plain links of one card are the same card", () => {
  assert.deepEqual(K2Card.parseLink(golden.fh22.link).card, K2Card.parseLink(golden.fh22_plain.link).card);
  assert.deepEqual(K2Card.parseLink(golden.fh22.link).card, K2Card.parseLink(golden.fh22_lz.link).card);
  const v1 = BASE + "1c." + toB64url(recordOf(payloadOf("allpass")));   // the version '1' link of the same record, built from its payload
  assert.deepEqual(K2Card.parseLink(golden.allpass.link).card, K2Card.parseLink(v1).card);
});

test("the FH22 card: fixed levels green, PPS amber and capped, the rest grey", () => {
  const b = K2Card.badges(K2Card.parseLink(golden.fh22.link).card);
  const by = (id) => b.find((x) => x.id === id);
  assert.equal(b[0].label, "PD 100W");
  assert.equal(b[0].tone, "pass");
  const pps = b.find((x) => x.kind === "range");
  assert.equal(pps.label, "PPS 5-19.4V");
  assert.equal(pps.tone, "partial");
  assert.match(pps.status, /capped \(21V offered\)/);
  assert.equal(by("epr").tone, "skip");
  assert.equal(by("grp:QC2").tone, "skip");
  assert.equal(by("grp:QC2").status, "not offered");
  // the achieved badges come first, the grey ones last, each with a glyph and a word besides the colour
  const rank = { pass: 0, partial: 1, fail: 2, skip: 3 };
  for (let i = 1; i < b.length; i++) assert.ok(rank[b[i - 1].tone] <= rank[b[i].tone]);
  for (const x of b) { assert.ok(x.glyph, x.id); assert.ok(x.word, x.id); }
});

// FH22 card #18 as the hardware run left it: the AVS 20 V step passed at 19.406 V (inside the PD tolerance), the PPS plateaued
// at 19.4 V, and the D+/D- protocols the charger does not carry never answered. The page takes the tone from the verdict and the
// reason, by the Results grid's table (result_layout::roll_up), never from "max reached < offered".
test("FH22 #18: the AVS pass stays green and shows the offered range", () => {
  const b = K2Card.badges(K2Card.parseLink(golden.fh22_full.link).card);
  const avs = b.find((x) => x.id.startsWith("range:PD3.2+ SPR AVS"));
  assert.equal(avs.tone, "pass");
  assert.equal(avs.label, "AVS 9-20V");
  assert.doesNotMatch(avs.status, /stopped|capped|fails/);
  const pps = b.find((x) => x.id.startsWith("range:PPS"));
  assert.equal(pps.tone, "partial");
  assert.equal(pps.label, "PPS 5-19.4V");
  assert.match(pps.status, /capped \(21V offered\)/);
  assert.equal(b[0].tone, "pass");   // PD, fixed levels
  assert.equal(b.find((x) => x.id === "pd").status, "fixed levels passed");
});

test("FH22 #18: a protocol the charger never answered is grey with its reason, never red", () => {
  const b = K2Card.badges(K2Card.parseLink(golden.fh22_full.link).card);
  const want = { AFC: "unsupported", FCP: "unsupported", SCP: "unsupported", VOOC: "needs load", SVOOC: "needs load",
    SVOOC65: "needs load", VIVO: "no response" };
  for (const [n, why] of Object.entries(want)) {
    const x = b.find((y) => y.id === "grp:" + n);
    assert.equal(x.tone, "skip", n);
    assert.equal(x.status, why, n);
    assert.ok(x.word && x.glyph, n);
  }
  assert.equal(b.filter((x) => x.tone === "fail").length, 0);
  const sec = K2Card.sections(K2Card.parseLink(golden.fh22_full.link).card);
  const afc = sec.flatMap((s) => s.groups).find((g) => g.title === "AFC");
  assert.equal(afc.tone, "skip");
  assert.equal(afc.rows[0].value, "not offered");
  assert.equal(afc.rows[0].tone, "skip");
  assert.ok(afc.rows.every((r) => r.tone !== "fail"));
});

test("a protocol that answered and then failed stays amber; an AVS range cut short of its top is amber too", () => {
  const card = JSON.parse(JSON.stringify(K2Card.parseLink(golden.fh22_full.link).card));
  card.groups.QC3 = { verdict: "fail", max_mv: 9000, fail_mv: 12000, reason: "timeout" };
  assert.equal(K2Card.badges(card).find((x) => x.id === "grp:QC3").tone, "partial");
  card.groups.QC3 = { verdict: "fail", max_mv: 0, fail_mv: 5000, reason: "timeout" };
  assert.equal(K2Card.badges(card).find((x) => x.id === "grp:QC3").tone, "skip");
  const avs = card.ranges.find((r) => r.alg.includes("AVS"));
  avs.max_mv = 14000;   // 36 V of 48 V in spirit: the top of the range was never exercised
  assert.equal(K2Card.badges(card).find((x) => x.id.startsWith("range:PD3.2+")).tone, "partial");
  avs.max_mv = 0;
  assert.equal(K2Card.badges(card).find((x) => x.id.startsWith("range:PD3.2+")).tone, "skip");
});

test("a partial range without the capped reason says where it fails instead", () => {
  const card = JSON.parse(JSON.stringify(K2Card.parseLink(golden.fh22.link).card));
  card.groups.PD.reason = "";
  const pps = K2Card.badges(card).find((x) => x.kind === "range");
  assert.equal(pps.tone, "partial");
  assert.match(pps.status, /fails at 21V/);
  assert.doesNotMatch(pps.status, /capped/);
});

test("the all-pass card: every badge green, nothing grey", () => {
  const card = K2Card.parseLink(golden.allpass.link).card;
  const b = K2Card.badges(card);
  assert.deepEqual(b.map((x) => x.label).sort(),
    ["EPR 36V", "PD 65W", "PPS 3.3-21V", "QC 2.0 9V", "QC 3.0 11.9V", "UFCS 19.9V"].sort());
  assert.ok(b.every((x) => x.tone === "pass"));
  const h = K2Card.header(card);
  assert.equal(h.title, "Desk 65W");
  assert.deepEqual(h.facts, ["65W", "Xiaomi", "Samsung SFC 3.0"]);   // the fixture's spec_rev is 3, which is not a revision a source can send
  assert.equal(h.fingerprint, "00C0FFEE");
  assert.match(h.ident, /^Card #\d+ · run \d+ · boot \d+ · up .* · fingerprint 00C0FFEE$/);
  assert.deepEqual(K2Card.header({ ...card, spec_rev: 2 }).facts, ["PD 3.x", "65W", "Xiaomi", "Samsung SFC 3.0"]);
  assert.deepEqual(K2Card.header({ ...card, spec_rev: 1, qc: "QC4+" }).facts, ["PD 2.0", "65W", "Xiaomi", "QC4+"]);
});

test("the passive OEM label follows the firmware's rule: Moto TurboPower, Samsung SFC 2.0 and 3.0", () => {
  const label = (edit) => {
    const p = payloadOf("allpass").slice();
    edit(p);
    return K2Card.decodePayload(p).qc;
  };
  const setRange = (p, loMv, ma) => { const lo = loMv / 100 | (ma / 50 << 9); p[106] = lo & 0xFF; p[107] = lo >> 8; };
  const setHi = (p, mv) => { p[108] = (mv / 100) & 0xFF; p[109] = (mv / 100) >> 8; };
  const setVid = (p, vid) => { p[4] = vid & 0xFF; p[5] = vid >> 8; };
  const pps = (p) => assert.equal(K2Card.decodePayload(p).ranges[0].alg, "PPS (PD3.x)");
  assert.equal(label((p) => { pps(p); setRange(p, 3300, 6250); setHi(p, 20000); setVid(p, 0x22B8); }), "Moto TurboPower");
  assert.equal(label((p) => { setRange(p, 3300, 6250); setHi(p, 20000); setVid(p, 0x17EF); }), "Moto TurboPower");
  assert.equal(label((p) => { setRange(p, 3300, 6250); setHi(p, 20000); setVid(p, 0x2717); }), "Samsung SFC 3.0");   // no VID, no Moto
  assert.equal(label((p) => { setRange(p, 3300, 3000); setHi(p, 16000); }), "Samsung SFC 2.0");
  assert.equal(label((p) => { setRange(p, 3300, 2000); setHi(p, 21000); }), undefined);
  assert.equal(label((p) => { setRange(p, 3300, 4050); setHi(p, 11000); }), "Samsung SFC 2.0");   // the fixture's cable is 5 A
  assert.equal(label((p) => { setRange(p, 3300, 4050); setHi(p, 11000); p[170] = 0xB8; p[171] = 0x0B; }), undefined);   // a 3 A cable
});

test("the full results hold the numbers the badges leave out", () => {
  const s = K2Card.sections(K2Card.parseLink(golden.fh22.link).card);
  const text = JSON.stringify(s);
  assert.match(text, /Power Delivery/);
  assert.match(text, /19\.4V/);
  assert.match(text, /output capped below the advertised range/);
  const run = s.find((x) => x.title === "Run");
  assert.ok(run.groups[0].rows.some((r) => r.label === "Fingerprint" && r.value === "6B8A1F3C"));
  assert.ok(!s.some((x) => x.title === "Cable"), "an all-zero cable block is not shown");
  const cable = K2Card.sections(K2Card.parseLink(golden.allpass.link).card).find((x) => x.title === "Cable");
  assert.ok(cable, "a real cable block is shown");
});

test("a repeated run shows how many times it was seen and when, a single run shows no repeat", () => {
  const card = K2Card.parseLink(golden.fh22_repeat.link).card;
  assert.equal(card.times, 4);
  assert.equal(card.last_boots, 44);
  assert.equal(card.last_uptime_s, 3725);
  const run = K2Card.sections(card).find((x) => x.title === "Run");
  assert.ok(run.groups[0].rows.some((r) => r.label === "Same result" && r.value === "4 runs, last at boot 44, up 1h 2m"));
  assert.match(K2Card.header(card).ident, /same result x4/);
  // The single run of the same charger carries no repeat keys and no row.
  const once = K2Card.parseLink(golden.fh22.link).card;
  assert.equal(once.times, undefined);
  assert.ok(!K2Card.sections(once).find((x) => x.title === "Run").groups[0].rows.some((r) => r.label === "Same result"));
});

test("older card formats decode: 144 bytes at format 3 has no EPR levels and no cable", () => {
  const p = payloadOf("allpass").slice(0, 144);
  p[33] = 3;
  const card = K2Card.parseLink(BASE + "1c." + toB64url(recordOf(p))).card;
  assert.equal(card.epr_levels, undefined);
  assert.equal(card.cable, undefined);
  assert.equal(card.pdp_w, 65);
  assert.equal(K2Card.badges(card).find((x) => x.id === "epr"), undefined);
});

test("bad fragments say what is wrong", () => {
  const code = (s) => { try { K2Card.parseLink(s); return "none"; } catch (e) { assert.equal(e.name, "CardError"); return e.code; } };
  const frag = golden.fh22.link.slice(BASE.length);
  assert.equal(code(""), "missing");
  assert.equal(code(BASE), "missing");
  assert.equal(code("https://example.org/r/"), "missing");
  assert.equal(code("hello"), "bad-link");
  assert.equal(code("K2:CARD1:ABC"), "bad-frame");   // a K2 data frame is read, and this one is damaged
  assert.equal(code(BASE + "9" + frag.slice(1)), "bad-version");
  assert.equal(code(BASE + "3c." + frag.slice(3)), "bad-version");
  assert.equal(code(BASE + "0c." + frag.slice(3)), "bad-version");
  assert.equal(code(BASE + "2t." + frag.slice(3)), "bad-version");   // kind 't' is a version '1' form only
  assert.equal(code(BASE + "2p." + frag.slice(3)), "bad-version");   // version '2' exists only for the card and the cable
  assert.equal(code(BASE + "1x." + frag.slice(3)), "bad-link");
  assert.equal(code(BASE + "2c." + toB64url(fromHex(golden.fh22_plain.hex))), "bad-record");   // a version '1' record is not schema 2
  const v2 = fromHex(golden.fh22.hex);
  v2[v2.length - 1] ^= 1;   // the v2 record carries the v1 CRC-16 at its end: it is checked like a version '1' link's
  assert.equal(code(BASE + "2c." + toB64url(v2)), "bad-crc");
  assert.equal(code(golden.pdcaps_spr.link), "not-a-card");   // a good link of another kind, handed to the card reader
  assert.equal(code(BASE + "1c.!!!"), "bad-base64");
  const zfrag = golden.fh22_lz.link.slice(BASE.length);   // a packed version '1' link, for the LZ4 cuts
  assert.equal(code(BASE + zfrag.slice(0, 41)), "bad-lz4");      // cut inside the first run of literals
  assert.equal(code(BASE + zfrag.slice(0, 40)), "bad-base64");    // a base64 text never ends one character into a group
  assert.equal(code(BASE + frag.slice(0, 41)), "bad-crc");        // a cut version '2' record: the carried CRC-16 no longer matches
  assert.equal(code(BASE + "1c.AAAA"), "bad-record");
  const plain = golden.fh22_plain.link.slice(BASE.length);
  assert.equal(code(BASE + plain.slice(0, 61)), "bad-record");   // cut short: the length no longer fits
  const flipped = fromHex(golden.fh22_plain.hex);
  flipped[20] ^= 1;
  assert.equal(code(BASE + "1c." + toB64url(flipped)), "bad-crc");
  const newer = payloadOf("fh22");
  newer[33] = 11;   // one past format 10
  assert.equal(code(BASE + "1c." + toB64url(recordOf(newer))), "newer-card");
});

test("the LZ4 reader refuses a back-reference before the start and a run past the end", () => {
  assert.throws(() => K2Card.lz4Inflate(Uint8Array.from([0x10, 0x41, 0x05, 0x00])), { code: "bad-lz4" });
  assert.throws(() => K2Card.lz4Inflate(Uint8Array.from([0x50, 0x41])), { code: "bad-lz4" });
  assert.throws(() => K2Card.lz4Inflate(new Uint8Array(0)), { code: "bad-lz4" });
  // an overlapping copy: one literal "a", then a 5-byte match at offset 1
  assert.equal(Buffer.from(K2Card.lz4Inflate(Uint8Array.from([0x11, 0x61, 0x01, 0x00]))).toString(), "aaaaaa");
});

test("the page loads nothing from the network", () => {
  for (const f of ["index.html", "card.js", "records.js", "view.js"]) {
    const src = readFileSync(new URL(`../docs/site/r/${f}`, import.meta.url), "utf8");
    assert.doesNotMatch(src, /https?:\/\/(?!yarost12\.github\.io|github\.com\/YaroST12)/, f);
    assert.doesNotMatch(src, /\b(fetch|XMLHttpRequest|WebSocket|sendBeacon)\b/, f);
  }
});

test("the brand table names ZTE and leaves out vendors usb.ids does not list", () => {
  assert.equal(K2Card.BRANDS[0x19D2], "ZTE");
  for (const vid of [0x339B, 0x2D95, 0x2B01]) assert.equal(K2Card.BRANDS[vid], undefined);
});

test("the OPPO SVID flag (src_flags bit 0 of the cable block) decodes as a bare key, only when set", () => {
  const p = payloadOf("allpass").slice();
  assert.equal(K2Card.decodePayload(p).oppo_svid, undefined);
  p[162 + 15] = 1;
  assert.equal(K2Card.decodePayload(p).oppo_svid, 1);
});

test("format 6 cards carry the UFCS modes: the badge and the detail show each mode's voltage span and current", () => {
  const card = K2Card.parseLink(golden.ufcs_modes.link).card;
  assert.deepEqual(card.ufcs, [{ lo_mv: 3300, hi_mv: 5500, max_ma: 5000 }, { lo_mv: 5000, hi_mv: 11000, max_ma: 3000 },
    { lo_mv: 5000, hi_mv: 20000, max_ma: 3000 }]);
  const badge = K2Card.badges(card).find((x) => x.id === "grp:UFCS");
  assert.equal(badge.tone, "pass");
  assert.equal(badge.label, "UFCS 20V");
  assert.equal(badge.status, "up to 20V (3.3-5.5V 5A, 5-11V 3A, 5-20V 3A)");
  const g = K2Card.sections(card).flatMap((s) => s.groups).find((x) => x.title === "UFCS");
  const modes = g.rows.filter((r) => /^Mode \d$/.test(r.label)).map((r) => `${r.label}: ${r.value}`);
  assert.deepEqual(modes, ["Mode 1: 3.3-5.5V 5A", "Mode 2: 5-11V 3A", "Mode 3: 5-20V 3A"]);
});

test("a UFCS mode with no current read is the span alone, and an unused slot ends the list", () => {
  const p = K2Card.splitLink(golden.ufcs_modes.link).record.slice(4, 4 + 190);   // a version '2' link: splitLink expands it to the version '1' record
  const mode = (lo, hi, units) => { const v = lo | hi << 8 | units << 17; return [v & 0xFF, v >> 8 & 0xFF, v >> 16 & 0xFF]; };   // u24 LE
  p.set(mode(50, 110, 0), 178 + 3);   // mode 2: 5-11 V, no current known
  p.fill(0, 178 + 6, 178 + 9);        // mode 3 unused
  const card = K2Card.decodePayload(p);
  assert.deepEqual(card.ufcs, [{ lo_mv: 3300, hi_mv: 5500, max_ma: 5000 }, { lo_mv: 5000, hi_mv: 11000 }]);
  assert.equal(K2Card.badges(card).find((x) => x.id === "grp:UFCS").status, "up to 20V (3.3-5.5V 5A, 5-11V)");
});

test("v5 cards still read: no UFCS modes, the badge shows volts only", () => {
  for (const [name, g] of Object.entries(v5)) {
    const r = K2Card.parseLink(g.link);
    assert.deepEqual(normalise(r.card), normalise(g.json), name);
    assert.equal(r.card.ufcs, undefined, name);
  }
  const allpass = K2Card.parseLink(v5.allpass.link).card;
  const badge = K2Card.badges(allpass).find((x) => x.id === "grp:UFCS");
  assert.equal(badge.label, "UFCS 19.9V");
  assert.equal(badge.status, "up to 19.9V");
  assert.ok(!K2Card.sections(allpass).flatMap((s) => s.groups).find((x) => x.title === "UFCS").rows.some((r) => /^Mode/.test(r.label)));
});

test("v6 cards still read: the UFCS modes stay, the CRC bytes where format 7 keeps its repeat block are never read as one", () => {
  const p = payloadOf("fh22_repeat").slice(0, 190);   // a 190 byte format 6 payload of a card that, as format 7, repeated
  p[33] = 6;
  const card = K2Card.parseLink(BASE + "1c." + toB64url(recordOf(p))).card;
  assert.equal(card.times, undefined);
  assert.equal(card.last_boots, undefined);
  assert.equal(card.runs, 5);
  // The same 190 bytes zero padded to 196 by a store that keeps the old CRC where the block is (nothing readable there).
  const q = new Uint8Array(196);
  q.set(p);
  q.set([0x12, 0x34, 0x56, 0x78], 190);
  const padded = K2Card.parseLink(BASE + "1c." + toB64url(recordOf(q.fill(6, 33, 34)))).card;
  assert.equal(padded.times, undefined);
});

test("the SCP current limit (src_flags bits 1..7, 50 mA units) decodes as scp_ma, only when set", () => {
  const p = payloadOf("allpass").slice();
  assert.equal(K2Card.decodePayload(p).scp_ma, undefined);
  p[162 + 15] = 120 << 1;
  const c = K2Card.decodePayload(p);
  assert.equal(c.scp_ma, 6000);
  assert.equal(c.oppo_svid, undefined);
  p[162 + 15] = (45 << 1) | 1;
  assert.equal(K2Card.decodePayload(p).scp_ma, 2250);
  assert.equal(K2Card.decodePayload(p).oppo_svid, 1);
});
