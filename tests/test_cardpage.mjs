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
  delete c.qc;   // row B item 5: the no-brand-without-VID rule is in the firmware renderer and tools/report_card.py,
                 // not yet in this page's own decoder (docs/site/r/card.js)
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
  const want = { AFC: "unsupported", FCP: "unsupported", SCP: "unsupported", VOOC: "no answer to either wake, unloaded", VIVO: "no response" };
  for (const [n, why] of Object.entries(want)) {
    const x = b.find((y) => y.id === "grp:" + n);
    assert.equal(x.tone, "skip", n);
    assert.equal(x.status, why, n);
    assert.ok(x.word && x.glyph, n);
  }
  assert.equal(b.filter((x) => x.tone === "fail").length, 0);
  assert.ok(!b.some((x) => x.id === "grp:SVOOC" || x.id === "grp:SVOOC65"));   // the VOOC family is one badge
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
  newer[33] = 13;   // one past format 12
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
  for (const f of ["index.html", "card.js", "deep.js", "records.js", "view.js"]) {
    const src = readFileSync(new URL(`../docs/site/r/${f}`, import.meta.url), "utf8");
    // the one other address is the SVG namespace name view.js gives createElementNS: a name, nothing is loaded from it
    assert.doesNotMatch(src, /https?:\/\/(?!yarost12\.github\.io|github\.com\/YaroST12|www\.w3\.org\/2000\/svg)/, f);
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

// include/k2/chargertest/card_tone.hpp: a Light run measured 5 V only and requested nothing else, so what the PD family advertises
// (the PD group, a listed range, a listed EPR level) reads green with the word "advertised"; a legacy group that answered at 5 V
// stays noted ("detected", no fill); the same card at Medium keeps its words.
test("a Light card: PD and listed ranges advertised in green, legacy groups detected and noted", () => {
  const base = K2Card.parseLink(golden.allpass.link).card;
  const card = { ...base, level: "Light", ranges: base.ranges.map((r) => ({ ...r, max_mv: 0, fail_mv: 0 })) };
  const b = K2Card.badges(card);
  const green = b.filter((x) => x.tone === "pass");
  assert.ok(green.some((x) => x.kind === "pd") && green.some((x) => x.kind === "range"));
  for (const x of green) assert.equal(x.word, "advertised");
  for (const x of green.filter((x) => x.kind === "range")) assert.equal(x.status, "advertised");
  const noted = b.filter((x) => x.tone === "noted");
  assert.ok(noted.length && noted.every((x) => x.kind === "group" && x.word === "detected"));
  const groups = K2Card.sections(card).flatMap((s) => s.groups);
  const pdRows = groups.find((g) => g.title === "PD fixed levels");
  assert.equal(pdRows.tone, "pass");
  assert.equal(pdRows.rows[0].value, "advertised");
  assert.ok(groups.filter((g) => g.title !== "PD fixed levels" && /^(PPS|AVS)/.test(g.title)).every((g) => g.tone === "pass" && g.rows[0].value === "advertised"));
  assert.ok(groups.filter((g) => noted.some((x) => x.label.startsWith(g.title))).every((g) => g.tone === "noted" && g.rows[0].value === "detected"));
  assert.ok(K2Card.badges(base).every((x) => x.tone === "pass"));   // Medium: unchanged
  assert.ok(K2Card.badges(base).every((x) => x.word === "achieved"));
  // an EPR level the Light run only listed (a skip with the reason "advertised") reads green too
  const epr = { ...card, epr_levels: { "28V": { verdict: "skip", reason: "advertised", max_mv: 0, fail_mv: 0 } } };
  const eb = K2Card.badges(epr).find((x) => x.kind === "epr");
  assert.ok(eb && eb.tone === "pass" && eb.word === "advertised" && eb.label === "EPR 28V");
});

// A voltage fold the deep sweep found on a range that failed nothing is a caution: amber, with the note in the words of the
// device card ("folds to 19.4V above 19.9V"); a failing range keeps its own verdict and never reads as a caution.
test("a sweep fold on a range that failed nothing is a caution, never a pass or a fail", () => {
  const base = JSON.parse(JSON.stringify(K2Card.parseLink(golden.allpass.link).card));
  const pps = base.ranges.find((r) => r.alg === "PPS (PD3.x)");
  assert.ok(pps && !pps.fail_mv);
  pps.sweep = { min_mv: -40, max_mv: 25, worst_mv: -40, worst_at_mv: 20000, peak_mv: 19800, peak_at_mv: 19900, fold_mv: 19400, fold_from_mv: 20000 };
  assert.equal(K2Card.rangeVerdict(pps, false), "partial");
  const b = K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)"));
  assert.equal(b.tone, "partial");
  assert.match(b.status, /^folds to 19\.4V above 19\.9V, sweep peak 19\.8V at 19\.9V$/);
  assert.doesNotMatch(b.status, /folds back/);
  const rows = K2Card.sections(base).flatMap((s) => s.groups).flatMap((g) => g.rows);
  const note = rows.find((r) => r.label === "Caution");
  assert.equal(note.value, "folds to 19.4V above 19.9V");
  assert.equal(note.tone, "partial");
  pps.sweep.fold_mv = 20000;   // a fold back to a whole volt reads without ".0", as the device does
  assert.equal(K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)")).status.startsWith("folds to 20V above 19.9V"), true);
  delete pps.sweep.fold_mv;    // no fold: the plain sweep summary and the old tone
  assert.equal(K2Card.rangeVerdict(pps, false), "pass");
  assert.doesNotMatch(K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)")).status, /folds/);
  pps.sweep.fold_mv = 19400;
  pps.fail_mv = 15000; pps.max_mv = 13000;   // a real failure outranks the caution and keeps the fold-back clause
  const failing = K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)"));
  assert.equal(failing.tone, "partial");
  assert.match(failing.status, /folds back to 19\.4V from 20V/);
  assert.ok(!K2Card.sections(base).flatMap((s) => s.groups).flatMap((g) => g.rows).some((r) => r.label === "Caution"));
});

// A sweep that tracked worse than 100 mV and folded nothing is a caution too (card_tone.hpp deviates()): the device's chip is amber
// on a passing sweep, never red, so the page reads the same. The card stores the worst error clamped to +/-127 mV.
test("a sweep that tracked worse than 100 mV is a caution, never red", () => {
  const base = JSON.parse(JSON.stringify(K2Card.parseLink(golden.allpass.link).card));
  const pps = base.ranges.find((r) => r.alg === "PPS (PD3.x)");
  assert.ok(pps && !pps.fail_mv);
  pps.sweep = { min_mv: -100, max_mv: 25, worst_mv: -100, worst_at_mv: 20000, peak_mv: 19800, peak_at_mv: 19900 };
  assert.equal(K2Card.rangeVerdict(pps, false), "pass");   // exactly 100 mV is inside the window
  pps.sweep.worst_mv = -101;
  assert.equal(K2Card.rangeVerdict(pps, false), "partial");
  pps.sweep.worst_mv = 127;                                 // the clamp of a 450 mV excess
  assert.equal(K2Card.rangeVerdict(pps, false), "partial");
  const b = K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)"));
  assert.equal(b.tone, "partial");
  assert.match(b.status, /^off >100mV, sweep peak 19\.8V at 19\.9V$/);
  const note = K2Card.sections(base).flatMap((s) => s.groups).flatMap((g) => g.rows).find((r) => r.label === "Caution");
  assert.equal(note.value, "sweep off >100mV");
  pps.sweep.fold_mv = 19400; pps.sweep.fold_from_mv = 20000;   // a fold keeps its own words
  assert.match(K2Card.badges(base).find((x) => x.id.startsWith("range:PPS (PD3.x)")).status, /^folds to 19\.4V above 19\.9V/);
  delete pps.sweep.fold_mv; delete pps.sweep.fold_from_mv;
  pps.fail_mv = 15000; pps.max_mv = 13000;                     // a failed range keeps its own verdict and no caution row
  assert.ok(!K2Card.sections(base).flatMap((s) => s.groups).flatMap((g) => g.rows).some((r) => r.label === "Caution"));
});


// ---- format 11: the Deep block (docs/site/r/deep.js), after the level byte of a version '2' card -----------------------------------
const qrshare = Object.fromEntries(readFileSync(new URL("./fixtures/qrshare.tsv", import.meta.url), "utf8")
  .split("\n").filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t")).map((c) => [c[0], fromHex(c[3])]));
const deepLink = BASE + "2c." + toB64url(qrshare.card_deep_v2);
const deepJson = JSON.parse(readFileSync(new URL("./fixtures/deep_decoded.json", import.meta.url), "utf8"));
const K2Deep = require("../docs/site/r/deep.js");

test("the golden Deep card's block decodes to what tools/k2_qr.py reads from the same link", () => {
  const card = K2Card.parseLink(deepLink).card;
  assert.equal(card.level, "Deep");
  assert.equal(card.deep_error, undefined);
  assert.deepEqual(JSON.parse(JSON.stringify(card.deep)), deepJson);
});

test("a card without a block shows no Deep section, and a Deep card is the same card plus the block", () => {
  const plain = K2Card.parseLink(BASE + "2c." + toB64url(qrshare.card_v2)).card;
  assert.equal(plain.deep, undefined);
  assert.equal(K2Card.sections(plain).find((s) => s.title === "Deep run details"), undefined);
  const deep = K2Card.parseLink(deepLink).card;
  const { deep: _d, ...rest } = deep;
  assert.equal(K2Card.sections(deep).filter((s) => s.title === "Deep run details").length, 1);
  assert.equal(rest.level, "Deep");
});

test("the Deep section lists the spec summary, the SVOOC ceiling and one error graph per swept range", () => {
  const s = K2Card.sections(K2Card.parseLink(deepLink).card).find((x) => x.title === "Deep run details");
  const titles = s.groups.map((g) => g.title);
  assert.deepEqual(titles, ["PD spec check", "SVOOC ceiling", "PPS sweep error", "AVS sweep error"]);
  const spec = s.groups[0];
  assert.equal(spec.tone, "fail");
  assert.match(spec.rows.map((r) => r.value).join("|"), /1 passed, 1 failed, 1 skipped, 1 unverified, 23 not run/);
  assert.match(spec.rows.map((r) => r.value).join("|"), /-32\.1 ms worst, 7 fails at PDO 5 to 9/);
  assert.deepEqual(s.groups[1].rows.map((r) => r.value), ["stopped", "5.5A", "11.2V", "2", "3.3 s"]);
  for (const g of s.groups.slice(2)) {
    assert.deepEqual(g.graph.limits, [100, 300]);
    assert.equal(g.graph.data.buckets.length, 32);
  }
  assert.match(s.groups[2].rows.map((r) => r.value).join("|"), /-20 mV to \+60 mV/);
});

test("the graph geometry keeps the limit lines in view, breaks a run at an empty bucket and stays inside the box", () => {
  const g = K2Card.parseLink(deepLink).card.deep.graphs[0];
  const m = { l: 38, t: 6, r: 6, b: 18 };
  const p = K2Deep.plot(g, 320, 150, m, [100, 300]);
  assert.ok(p.ymax >= 350);
  assert.equal(p.lines.length, 4);
  for (const l of p.lines) assert.ok(l.y >= m.t && l.y <= 150 - m.b);
  assert.ok(p.y0 > m.t && p.y0 < 150 - m.b);
  assert.equal(p.runs.length, 1);
  const holed = JSON.parse(JSON.stringify(g));
  holed.buckets[10] = null;
  assert.equal(K2Deep.plot(holed, 320, 150, m, [100, 300]).runs.length, 2);
  for (const run of p.runs) for (const n of (run.band + " " + run.mean).match(/-?\d+(\.\d+)?/g)) assert.ok(Number.isFinite(+n));
  const flat = { ...g, buckets: g.buckets.map(() => null) };
  assert.equal(K2Deep.plot(flat, 320, 150, m, [100, 300]).runs.length, 0);
  assert.equal(K2Deep.summary(flat), null);
});

test("a format 11 Deep link (tests/fixtures/qrshare_v11.tsv) keeps its CRC and its block, with no Xiaomi section", () => {
  const row = readFileSync(new URL("./fixtures/qrshare_v11.tsv", import.meta.url), "utf8").split("\n")
    .filter((l) => l && !l.startsWith("#")).map((l) => l.split("\t")).find((c) => c[0] === "card_deep_v11");
  const card = K2Card.parseLink(BASE + "2c." + toB64url(fromHex(row[3]))).card;
  assert.equal(card.level, "Deep");
  assert.equal(card.deep_error, undefined);
  assert.equal(card.deep.xiaomi, undefined);
  assert.deepEqual(JSON.parse(JSON.stringify(card.deep)), deepJson);
  assert.equal(K2Card.sections(card).find((s) => s.title === "Xiaomi"), undefined);
});

test("format 12: a Medium card's block carries only the Xiaomi section, read as tools/k2_qr.py reads it", () => {
  const card = K2Card.parseLink(BASE + "2c." + toB64url(qrshare.card_mi_v2)).card;
  assert.equal(card.level, undefined);
  assert.equal(card.deep_error, undefined);
  assert.deepEqual(Object.keys(card.deep), ["xiaomi"]);
  assert.deepEqual(JSON.parse(JSON.stringify(card.deep.xiaomi)), {chain: 20, chain_text: "done, verified", done: true, modes: [
    {label: "Mi 55W", watts: 55, mv: 11000, ma: 5000, before_pdo: 0xC0DC323C, after_pdo: 0xC0DC3264, before: "PPS 5-11V 3A",
      after: "PPS 5-11V 5A", verdict: "pass", reason: ""}]});
  const s = K2Card.sections(card);
  assert.equal(s.find((x) => x.title === "Deep run details"), undefined);
  const mi = s.find((x) => x.title === "Xiaomi");
  assert.deepEqual(mi.groups.map((g) => [g.title, g.tone]), [["Xiaomi auth", "pass"], ["Mi 55W", "pass"]]);
  assert.deepEqual(mi.groups[0].rows.map((r) => r.value), ["done, 1 new mode", "done, verified"]);
  assert.deepEqual(mi.groups[1].rows.map((r) => r.value), ["achieved", "PPS 5-11V 3A", "PPS 5-11V 5A", "11V 5A"]);
  assert.equal(K2Deep.pdoText(0x0001912C), "PD 5V 3A");
  assert.equal(K2Deep.pdoText(0x0008C1F4), "EPR 28V 5A");
});

test("a link of a format 10 firmware keeps its own CRC and has no block", () => {
  const rec = fromHex("024d3c2b1aac0534120302077b12efcdab0f570a10002296e76b90ad760500080022b30000000000e072d4120f0080cd00000000000000000000000108e1c60d00000108214d902500000000000000000100f01603");   // tools/k2_qr.py's test: the golden card at format 10, its CRC over the format 10 record
  const r = K2Card.parseLink(BASE + "2c." + toB64url(rec));
  assert.equal(r.card.deep, undefined);
  assert.equal(r.card.name, "Desk 65W");
  const bad = Uint8Array.from(rec);
  bad[bad.length - 1] ^= 1;
  assert.throws(() => K2Card.parseLink(BASE + "2c." + toB64url(bad)), (e) => e.code === "bad-crc");
});

test("a Deep block this page cannot read leaves the card and says why", () => {
  const blob = Uint8Array.from(K2Card.expandCardV2(qrshare.card_deep_v2).deep);
  const cut = blob.slice(0, blob.length - 20);
  assert.throws(() => K2Deep.decode(cut, []), (e) => e.code === "bad-deep");
  const lying = Uint8Array.from(blob);
  lying[0] = 0xFF; lying[1] = 0x0F;   // 4095 bits: more than the format allows
  assert.throws(() => K2Deep.decode(lying, []), (e) => e.code === "bad-deep");
  const card = JSON.parse(JSON.stringify(K2Card.parseLink(deepLink).card));
  delete card.deep;
  card.deep_error = "Deep block is cut short";
  const s = K2Card.sections(card).find((x) => x.title === "Deep run details");
  assert.equal(s.groups[0].title, "Not shown");
  assert.equal(s.groups[0].rows[0].value, "Deep block is cut short");
});

test("the VOOC family is one group; its levels read in watts, claimed only from the Deep ceiling", () => {
  const card = JSON.parse(JSON.stringify(K2Card.parseLink(deepLink).card));
  card.groups.VOOC = { verdict: "fail", max_mv: 0, reason: "no answer to either wake, unloaded" };
  card.groups.SVOOC = { verdict: "pass", max_mv: 5000, detail: 15 };
  card.groups.SVOOC65 = { verdict: "fail", max_mv: 0, reason: "no answer to either wake, unloaded" };
  // the golden's ceiling: stopped, 5.5 A at 11.2 V = 61.6 W
  assert.deepEqual(K2Card.voocLevels(card.deep.svooc).map((l) => l.label + ":" + l.state),
    ["VOOC 20W:not tested", "SVOOC 50W:claimed", "SVOOC 65W:not reached", "SVOOC 100W:not reached", "SVOOC 120W:not reached"]);
  const b = K2Card.badges(card);
  const fam = b.filter((x) => /^grp:(VOOC|SVOOC|SVOOC65)$/.test(x.id));
  assert.equal(fam.length, 1);
  assert.equal(fam[0].label, "VOOC");
  assert.equal(fam[0].tone, "pass");
  const lv = b.filter((x) => x.id.startsWith("lvl:"));
  assert.deepEqual(lv.map((x) => x.label), ["SVOOC 50W"]);
  assert.equal(lv[0].tone, "noted");   // framed, never green: no current was drawn
  assert.equal(lv[0].word, "claimed");
  const g = K2Card.sections(card).flatMap((s) => s.groups).filter((x) => /VOOC/.test(x.title) && x.title !== "SVOOC ceiling");   // the Deep block keeps its ceiling group
  assert.deepEqual(g.map((x) => x.title), ["VOOC"]);
  const rows = Object.fromEntries(g[0].rows.map((r) => [r.label, r.value]));
  assert.equal(rows["SVOOC 50W"], "claimed");
  assert.equal(rows["SVOOC 120W"], "not reached");
  assert.equal(rows["VOOC 20W"], "not tested");
  // without a ceiling run nothing is claimed and nothing is "not reached"
  delete card.deep.svooc;
  assert.ok(K2Card.voocLevels(undefined).every((l) => l.state === "not tested"));
  assert.equal(K2Card.badges(card).filter((x) => x.id.startsWith("lvl:")).length, 0);
});

test("the card page's OEM watts match tests/fixtures/oem_watts.txt and the firmware's level table", () => {
  let labels = 0, claims = 0;
  for (const line of readFileSync(new URL("./fixtures/oem_watts.txt", import.meta.url), "utf8").split("\n")) {
    if (!line.trim() || line.trim().startsWith("#")) continue;
    const f = line.split("|").map((x) => x.trim());
    if (f[0] === "label") { assert.equal(K2Card.oemLabel(f[1], +f[2]), f[3], line); labels++; }
    if (f[0] === "claim") {
      assert.equal(K2Card.oemClaim({ w: +f[4], ceiling: f[5] === "y" }, { end: f[1], vmax_mv: +f[2], word: parseInt(f[3], 16) }), f[6], line);
      claims++;
    }
  }
  assert.ok(labels >= 10 && claims >= 12);
  const src = readFileSync(new URL("../include/k2/chargertest/oem_watts.hpp", import.meta.url), "utf8");
  for (const l of K2Card.OEM_LEVELS) assert.ok(src.includes(`{Family::Vooc, "${l.word}", ${l.mv}, ${l.ma}, ${l.w}, ${l.ceiling}},`), l.word + l.w);
});
