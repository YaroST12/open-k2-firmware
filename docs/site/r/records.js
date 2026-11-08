// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// The Open-K2 results viewer for the records that are not a charger test card: a cable (e-marker record, with its response
// quirks), the source's PDOs, the firmware identity and the boot report. Logic only, like card.js (which it needs, and which
// splits the link and reads the card): no DOM, no network.
//
// Each decoder is the JavaScript twin of one in tools/k2_qr.py (decode_emarker, decode_pdcaps, decode_identity, decode_boot),
// key for key: tests/fixtures/qrlink_decoded.json is what the Python decoders give for every golden link of
// tests/fixtures/qrlink.tsv, and tests/test_cardpage.mjs requires this file to give the same. Record layouts are in
// docs/QR_SHARE.md and include/k2/ui/qrshare_records.hpp. The views at the end turn the decoded record into what a phone shows,
// in the shape of a card's view (header, tiles, sections) so view.js draws every kind the same way.
// Loaded by the page as a script (window.K2Records, after card.js) and by Node as a module (module.exports).
var K2Records = (function () {
  var C = typeof K2Card !== "undefined" ? K2Card : require("./card.js");

  var SPEED = {0: "USB 2.0", 1: "USB 3.2 Gen1", 2: "USB 3.2 Gen2", 3: "USB4 Gen3", 4: "USB4 Gen4"};
  var PRODUCT_TYPE = {0: "undefined", 1: "PD hub", 2: "PD peripheral", 3: "passive cable", 4: "active cable",
    5: "alternate-mode adapter", 6: "VPD"};
  var KIND = ["Ack", "Nak", "Busy", "Raw"];
  var UNMATCHED = ["Ignore", "Nak", "NotSupported"];
  var KEY_MASK = [[1, "BACK"], [2, "MINUS"], [4, "OK"], [8, "PLUS"]];
  var KEY_PINS = [[1, "OK"], [2, "PLUS"], [4, "BACK"], [8, "MINUS"]];
  var ACTIVE_CC = ["none", "CC1", "CC2"];
  var RESET_FLAG_BITS = [[31, "LPWR"], [30, "WWDG"], [29, "IWDG"], [28, "SW"], [27, "POR"], [26, "PIN"]];

  function fail(code, message) { return C.CardError(code, message); }

  // ---- decoders (twins of tools/k2_qr.py) -----------------------------------------------------------------------------------

  function hex(v, width) {
    var s = (v >>> 0).toString(16).toUpperCase();
    while (s.length < width) s = "0" + s;
    return "0x" + s;
  }
  function u16(b, o) { return b[o] | (b[o + 1] << 8); }
  function u32(b, o) { return (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0; }

  // bytes up to the first NUL as text; a byte above 127 becomes U+FFFD, as Python's decode("ascii", errors="replace")
  function asciiz(b, from, to) {
    var s = "";
    for (var i = from; i < to && b[i] !== 0; i++) s += b[i] > 127 ? "�" : String.fromCharCode(b[i]);
    return s;
  }

  function asciiText(rec, what) {
    var s = "";
    for (var i = 0; i < rec.length; i++) {
      if (rec[i] > 127) throw fail("bad-record", "The " + what + " record has a byte that is not text.");
      s += String.fromCharCode(rec[i]);
    }
    return s;
  }

  function fields(tokens) {
    var f = {};
    tokens.forEach(function (t) {
      var at = t.indexOf("=");
      if (at >= 0) f[t.slice(0, at)] = t.slice(at + 1);
    });
    return f;
  }

  function intOf(v, what) {
    if (!/^\d+$/.test(v === undefined ? "" : v)) throw fail("bad-record", "The " + what + " record has a number that does not read.");
    return parseInt(v, 10);
  }
  function hexOf(v, what) {
    if (!/^[0-9a-fA-F]+$/.test(v === undefined ? "" : v)) throw fail("bad-record", "The " + what + " record has a hex value that does not read.");
    return parseInt(v, 16);
  }

  function decodeIdentity(rec) {
    var f = fields(asciiText(rec, "identity").split(/\s+/).filter(function (t) { return t !== ""; }));
    var git = f.g === undefined ? null : f.g;
    return {fw: f.fw === undefined ? null : f.fw, git: git, dirty: (git || "").slice(-6) === "-dirty",
      proto: f.p === undefined ? null : intOf(f.p, "identity")};
  }

  // The K2B1 text of k2/core/bootinfo.hpp (tools/k2_remote.py parse_bootinfo_text is the Python twin). Fields the page does not
  // show (the Ra flag) are left out, as in Python.
  function decodeBoot(rec) {
    var toks = asciiText(rec, "boot").split(/\s+/).filter(function (t) { return t !== ""; });
    if (!toks.length || toks[0] !== "K2B1") throw fail("bad-record", "This is not a K2 boot report.");
    var f = fields(toks.slice(1));
    ["fw", "g", "p", "t", "vb", "c1", "c2", "pl", "m", "k", "pa", "rf", "w", "pin", "mk", "mv", "sw", "n"].forEach(function (k) {
      if (f[k] === undefined) throw fail("bad-record", "The boot report is missing its " + k + " field.");
    });
    var pinParts = f.pin.split(",");
    if (pinParts.length < 4) throw fail("bad-record", "The boot report's pin field is cut short.");
    var pa = hexOf(f.pa, "boot"), keys = hexOf(f.k, "boot");
    var out = {fw: f.fw, git: f.g, proto: intOf(f.p, "boot"), t_ms: intOf(f.t, "boot"), vbus_mv: intOf(f.vb, "boot"),
      cc1_mv: intOf(f.c1, "boot"), cc2_mv: intOf(f.c2, "boot"), plus_held: f.pl === "1", mode: f.m, keys: keys, pa: pa,
      keys_pressed: KEY_MASK.filter(function (k) { return keys & k[0]; }).map(function (k) { return k[1]; }),
      pins_low: KEY_PINS.filter(function (k) { return !(pa & k[0]); }).map(function (k) { return k[1]; }),
      reset_flags: hexOf(f.rf, "boot"), warm: f.w === "1",
      pin: {initial: hexOf(pinParts[0], "boot"), now: hexOf(pinParts[1], "boot"), boot: hexOf(pinParts[2], "boot"),
        inherited: pinParts[3] === "1"},
      mark: f.mk === "-" ? null : hexOf(f.mk, "boot"), move: null, readbacks: [], notes: [], notes_not_shown: 0};
    if (f.mv !== "-") {
      var m = f.mv.split(":");
      var ft = (m[1] || "").split(">");
      if (m.length !== 5 || ft.length !== 2) throw fail("bad-record", "The boot report's move field does not read.");
      out.move = {ms: intOf(m[0], "boot"), from: hexOf(ft[0], "boot"), to: hexOf(ft[1], "boot"),
        vbus_mv: parseInt(m[2], 10), cc1_mv: intOf(m[3], "boot"), cc2_mv: intOf(m[4], "boot")};
      if (isNaN(out.move.vbus_mv)) throw fail("bad-record", "The boot report's move field does not read.");
    }
    if (f.sw !== "-") {
      f.sw.split(",").forEach(function (item) {
        var p = item.split("@");
        if (p.length !== 2) throw fail("bad-record", "The boot report's readback field does not read.");
        out.readbacks.push({ms: intOf(p[1], "boot"), sw0: hexOf(p[0], "boot")});
      });
    }
    if (f.n !== "-") {
      f.n.split(",").forEach(function (item) {
        if (item.charAt(0) === "+") { out.notes_not_shown = intOf(item.slice(1), "boot"); return; }
        var i = Math.max(item.lastIndexOf("s"), item.lastIndexOf("c"));
        if (i < 1) throw fail("bad-record", "The boot report's notes do not read.");
        out.notes.push({ms: intOf(item.slice(0, i), "boot"), kind: item.charAt(i) === "s" ? "sw0" : "cc", val: hexOf(item.slice(i + 1), "boot")});
      });
    }
    return out;
  }

  function decodeEmrec(r) {
    if (r.length !== 66) throw fail("bad-record", "The e-marker record is not 66 bytes.");
    var vdos = [], sum = 0, i;
    for (i = 0; i < 5; i++) vdos.push(u32(r, 21 + 4 * i));
    for (i = 0; i < 65; i++) sum += r[i];
    var idh = vdos[1], product = vdos[3], cable = vdos[4], type = (idh >>> 27) & 7;
    var out = {name: asciiz(r, 0, 20), spec_rev: r[20], vdos: vdos.map(function (v) { return hex(v, 8); }),
      checksum_ok: r[65] === ((sum + 0xA5) & 0xFF), vid: hex(idh & 0xFFFF, 4), pid: hex(product >>> 16, 4),
      product_type: PRODUCT_TYPE.hasOwnProperty(type) ? PRODUCT_TYPE[type] : "reserved"};
    if (type === 3 || type === 4) {
      out.max_v = [20, 30, 40, 50][(cable >>> 9) & 3];
      out.max_a = ((cable >>> 5) & 3) === 2 ? 5 : 3;
      out.speed = SPEED.hasOwnProperty(cable & 7) ? SPEED[cable & 7] : "code " + (cable & 7);
      out.termination = (cable >>> 11) & 3;
    }
    return out;
  }

  function decodeQuirks(b) {
    if (b.length < 3 || b.length !== 3 + 52 * b[2]) throw fail("bad-record", "The cable's quirk block does not fit its row count.");
    var flags = b[0], unm = b[1], count = b[2], rows = [];
    for (var i = 0; i < count; i++) {
      var o = 3 + 52 * i, sop = b[o], kind = b[o + 1], nreq = b[o + 2], nrep = b[o + 3], w = [];
      for (var k = 0; k < 12; k++) w.push(u32(b, o + 4 + 4 * k));
      rows.push({sop: (sop & 1 ? "P" : "") + (sop & 2 ? "PP" : ""), kind: kind < 4 ? KIND[kind] : kind, nreq: nreq, nrep: nrep,
        hdr_val: hex(w[0], 8), hdr_mask: hex(w[1], 8),
        req_val: w.slice(2, 4).map(function (x) { return hex(x, 8); }), req_mask: w.slice(4, 6).map(function (x) { return hex(x, 8); }),
        rep: w.slice(6, 6 + nrep).map(function (x) { return hex(x, 8); })});
    }
    return {flags: flags, sop_pp: !!(flags & 1), msgid_echo: !!(flags & 2), unmatched: unm < 3 ? UNMATCHED[unm] : unm, rows: rows};
  }

  function decodeEmarker(rec) {
    if (rec.length < 91) throw fail("bad-record", "The cable record is shorter than it should be.");
    var out = {name: asciiz(rec, 1, 25), record: decodeEmrec(rec.subarray(25, 91))};
    if (rec[0] & 1) out.quirks = decodeQuirks(rec.subarray(91));
    else if (rec.length !== 91) throw fail("bad-record", "The cable record has trailing bytes without the quirk flag.");
    return out;
  }

  // Python's "{:g}" of mv / 1000 for the values a PDO holds
  function g(n) { return String(n / 1000); }

  // tools/pd_trace.py decode_pdo: [dict, text]
  function decodePdo(raw) {
    if (raw === 0) return [{kind: "empty"}, "empty (all-zero word, padding)"];
    var kind = raw >>> 30, d, t, flags;
    if (kind === 0) {
      var mv = ((raw >>> 10) & 0x3FF) * 50, ma = (raw & 0x3FF) * 10;
      flags = [[29, "DRP"], [28, "suspend-ok"], [27, "unconstrained"], [26, "usb-comms"], [25, "DRD"], [24, "unchunked"], [23, "EPR"]]
        .filter(function (f) { return (raw >>> f[0]) & 1; }).map(function (f) { return f[1]; });
      return [{kind: "fixed", mv: mv, ma: ma, flags: flags}, "fixed " + g(mv) + " V " + g(ma) + " A" + (flags.length ? " [" + flags.join(",") + "]" : "")];
    }
    if (kind === 1) {
      var bmx = ((raw >>> 20) & 0x3FF) * 50, bmn = ((raw >>> 10) & 0x3FF) * 50, bmw = (raw & 0x3FF) * 250;
      return [{kind: "battery", min_mv: bmn, max_mv: bmx, max_mw: bmw}, "battery " + g(bmn) + "-" + g(bmx) + " V " + g(bmw) + " W"];
    }
    if (kind === 2) {
      var vmx = ((raw >>> 20) & 0x3FF) * 50, vmn = ((raw >>> 10) & 0x3FF) * 50, vma = (raw & 0x3FF) * 10;
      return [{kind: "variable", min_mv: vmn, max_mv: vmx, max_ma: vma}, "variable " + g(vmn) + "-" + g(vmx) + " V " + g(vma) + " A"];
    }
    var sub = (raw >>> 28) & 3;
    if (sub === 0) {
      var pmx = ((raw >>> 17) & 0xFF) * 100, pmn = ((raw >>> 8) & 0xFF) * 100, pma = (raw & 0x7F) * 50, lim = !!((raw >>> 27) & 1);
      d = {kind: "pps", min_mv: pmn, max_mv: pmx, max_ma: pma, power_limited: lim};
      t = "PPS " + g(pmn) + "-" + g(pmx) + " V " + g(pma) + " A" + (lim ? " [power-limited]" : "");
      return [d, t];
    }
    if (sub === 1) {
      var emx = ((raw >>> 17) & 0x1FF) * 100, emn = ((raw >>> 8) & 0xFF) * 100, pdp = raw & 0xFF;
      return [{kind: "epr-avs", min_mv: emn, max_mv: emx, pdp_w: pdp}, "EPR-AVS " + g(emn) + "-" + g(emx) + " V " + pdp + " W"];
    }
    if (sub === 2) {
      var i9 = ((raw >>> 10) & 0x3FF) * 10, i15 = (raw & 0x3FF) * 10;
      return [{kind: "spr-avs", ma_9_15v: i9, ma_15_20v: i15}, "SPR-AVS 9-15 V " + g(i9) + " A, 15-20 V " + g(i15) + " A"];
    }
    return [{kind: "apdo?"}, "APDO(reserved) " + hex(raw, 8)];
  }

  function decodePdcaps(rec) {
    if (rec.length < 6 || rec.length !== 6 + 4 * rec[0]) throw fail("bad-record", "The source record's length does not fit its PDO count.");
    var n = rec[0], flags = rec[1], spec = rec[2], cc = rec[3], pdos = [];
    for (var i = 0; i < n; i++) {
      var raw = u32(rec, 6 + 4 * i), dt = decodePdo(raw), p = {pos: i + 1, raw: hex(raw, 8), text: dt[1]};
      Object.keys(dt[0]).forEach(function (k) { p[k] = dt[0][k]; });
      pdos.push(p);
    }
    return {epr: !!(flags & 1), epr_partial: !!(flags & 2), spec_rev: spec, active_cc: cc < 3 ? ACTIVE_CC[cc] : cc,
      header: hex(u16(rec, 4), 4), pdos: pdos};
  }

  var DECODERS = {emarker: decodeEmarker, pdcaps: decodePdcaps, identity: decodeIdentity, boot: decodeBoot};

  // ---- the views ---------------------------------------------------------------------------------------------------------------
  // A tile is {label, status, tone, glyph, word}; tone "info" is a neutral tile (a fact, not a verdict). Rows and groups are
  // those of card.js (label/value/tone; title/tone/rows).

  function tile(label, status, tone, glyph) {
    return {label: label, status: status, tone: tone || "info", glyph: glyph || "", word: ""};
  }
  function row(label, value, tone) { return {label: label, value: String(value), tone: tone || ""}; }
  function trim(n, digits) { return String(Math.round(n * Math.pow(10, digits)) / Math.pow(10, digits)); }
  function volts(mv) { return mv < 0 ? "none" : trim(mv / 1000, 2) + " V"; }
  function amps(ma) { return trim(ma / 1000, 2) + " A"; }
  function watts(mw) { return trim(mw / 1000, 1) + " W"; }
  function vrange(a, b) { return trim(a / 1000, 2) + "-" + trim(b / 1000, 2) + " V"; }
  function cap(s) { return s.charAt(0).toUpperCase() + s.slice(1); }
  function pdRev(n) { return n === 1 ? "PD 2.0" : n === 2 ? "PD 3.x" : "revision " + n; }

  function brandOf(vid) {
    var b = C.BRANDS[parseInt(vid, 16)];
    return b ? b + " (" + vid + ")" : vid;
  }

  var TERMINATION = ["passive, VCONN not needed", "passive, VCONN needed", "one end active", "both ends active"];

  function emarkerView(d) {
    var r = d.record, cable = r.max_v !== undefined, epr = cable && !!((parseInt(r.vdos[4], 16) >>> 17) & 1);
    var facts = [cap(r.product_type)];
    if (cable) facts.push(r.max_a + "A", r.max_v + "V", r.speed);
    var quirkRows = d.quirks ? d.quirks.rows.length : 0;
    var tiles = [tile(cable ? "Cable" : "Device", cap(r.product_type))];
    if (cable) {
      tiles.push(tile("Current", r.max_a + " A"), tile("Voltage", r.max_v + " V" + (epr ? " (EPR)" : "")), tile("USB speed", r.speed));
    }
    tiles.push(tile("Maker", brandOf(r.vid)));
    tiles.push(r.checksum_ok ? tile("Record check", "ok", "pass", "✓") : tile("Record check", "does not match", "fail", "✕"));
    tiles.push(tile("Response quirks", d.quirks ? quirkRows + (quirkRows === 1 ? " row" : " rows") : "none"));

    var ident = [row("Name", d.name || "(none)"), row("Type", cap(r.product_type)), row("Maker", brandOf(r.vid)),
      row("USB ID", r.vid + ":" + r.pid), row("PD revision", pdRev(r.spec_rev))];
    var groups = [{title: "Who it says it is", tone: "", rows: ident}];
    if (cable) {
      groups.push({title: "What it can carry", tone: "", rows: [row("Max voltage", r.max_v + " V"), row("Max current", r.max_a + " A"),
        row("USB speed", r.speed), row("EPR capable", epr ? "yes" : "no"),
        row("Termination", TERMINATION[r.termination] || "code " + r.termination)]});
    }
    var sections = [{title: "Cable details", groups: groups, empty: ""}];
    var labels = ["Response header", "ID header", "Cert stat", "Product", "Cable"];
    sections.push({title: "Identity words (VDOs)", empty: "", groups: [{title: "As the cable answers", tone: "", rows:
      r.vdos.map(function (v, i) { return row(labels[i], v); })}]});
    var qs = {title: "Response quirks", groups: [], empty: "This record has no response quirks: a K2 playing it back answers from the five words above only."};
    if (d.quirks) {
      var q = d.quirks;
      qs.groups.push({title: "Policy", tone: "", rows: [row("Answers on SOP''", q.sop_pp ? "yes" : "no"),
        row("Echoes the message ID", q.msgid_echo ? "yes" : "no"), row("Requests no row matches", q.unmatched)]});
      q.rows.forEach(function (x, i) {
        var rows = [row("Answers on", "SOP" + (x.sop === "P" ? "'" : x.sop === "PP" ? "''" : " (none)")), row("Answer", x.kind),
          row("Header", x.hdr_val + " / " + x.hdr_mask)];
        for (var k = 0; k < x.nreq; k++) rows.push(row("Request word " + (k + 1), x.req_val[k] + " / " + x.req_mask[k]));
        for (var m = 0; m < x.nrep; m++) rows.push(row("Reply word " + (m + 1), x.rep[m]));
        qs.groups.push({title: "Row " + (i + 1), tone: "", rows: rows});
      });
    }
    sections.push(qs);
    return {kind: "emarker", kicker: "Open-K2 cable e-marker", badges_title: "What this cable is", full_title: "Cable record",
      header: {title: d.name || "Cable", facts: facts, ident: "E-marker record · USB ID " + r.vid + ":" + r.pid + " · " + pdRev(r.spec_rev)},
      badges: tiles, sections: sections};
  }

  // The power a PDO offers, in mW, or 0 when it has no figure.
  function pdoMw(p) {
    switch (p.kind) {
      case "fixed": return p.mv * p.ma / 1000;
      case "pps": return p.max_mv * p.max_ma / 1000;
      case "variable": return p.max_mv * p.max_ma / 1000;
      case "battery": return p.max_mw;
      case "epr-avs": return p.pdp_w * 1000;
      case "spr-avs": return Math.max(15 * p.ma_9_15v, 20 * p.ma_15_20v);
      default: return 0;
    }
  }

  function pdoLabel(p) {
    switch (p.kind) {
      case "fixed": return volts(p.mv).replace(" V", "V") + " " + amps(p.ma).replace(" A", "A");
      case "pps": return vrange(p.min_mv, p.max_mv).replace(" V", "V") + " " + amps(p.max_ma).replace(" A", "A");
      case "variable": return vrange(p.min_mv, p.max_mv).replace(" V", "V") + " " + amps(p.max_ma).replace(" A", "A");
      case "battery": return vrange(p.min_mv, p.max_mv).replace(" V", "V");
      case "epr-avs": return vrange(p.min_mv, p.max_mv).replace(" V", "V");
      case "spr-avs": return "9-20V";
      default: return p.text;
    }
  }

  var PDO_KIND = {fixed: "Fixed", pps: "PPS", variable: "Variable", battery: "Battery", "epr-avs": "EPR AVS", "spr-avs": "SPR AVS"};

  function pdoStatus(p) {
    var w = pdoMw(p), kind = PDO_KIND[p.kind] || p.kind;
    if (p.kind === "spr-avs") return kind + " · " + amps(p.ma_9_15v) + " to 15 V, " + amps(p.ma_15_20v) + " to 20 V";
    return kind + (w ? " · " + watts(w) : "") + (p.power_limited ? " · power-limited" : "");
  }

  function pdcapsView(d) {
    var live = d.pdos.filter(function (p) { return p.kind !== "empty"; });
    var top = live.reduce(function (m, p) { return Math.max(m, pdoMw(p)); }, 0);
    var facts = [live.length + (live.length === 1 ? " offer" : " offers"), pdRev(d.spec_rev)];
    if (d.epr) facts.push(d.epr_partial ? "EPR (list cut short)" : "EPR");
    if (top) facts.push("up to " + watts(top));
    var groups = live.map(function (p) {
      var rows = [row("Position", p.pos), row("Type", PDO_KIND[p.kind] || p.kind)];
      if (p.kind === "fixed") rows.push(row("Voltage", volts(p.mv)), row("Current", amps(p.ma)));
      else if (p.kind === "pps" || p.kind === "variable") rows.push(row("Voltage", vrange(p.min_mv, p.max_mv)), row("Current", amps(p.max_ma)));
      else if (p.kind === "battery") rows.push(row("Voltage", vrange(p.min_mv, p.max_mv)));
      else if (p.kind === "epr-avs") rows.push(row("Voltage", vrange(p.min_mv, p.max_mv)));
      else if (p.kind === "spr-avs") rows.push(row("9-15 V", amps(p.ma_9_15v)), row("15-20 V", amps(p.ma_15_20v)));
      if (pdoMw(p)) rows.push(row("Power", watts(pdoMw(p))));
      if (p.flags && p.flags.length) rows.push(row("Flags", p.flags.join(", ")));
      if (p.power_limited) rows.push(row("Power-limited", "yes"));
      rows.push(row("Raw word", p.raw));
      return {title: "#" + p.pos + " " + pdoLabel(p), tone: "", rows: rows};
    });
    var src = [row("PD revision", pdRev(d.spec_rev)), row("Offers", live.length + " of " + d.pdos.length + " slots"),
      row("Extended power range (EPR)", d.epr ? (d.epr_partial ? "yes, list cut short" : "yes") : "no"), row("Seen on", d.active_cc),
      row("Header", d.header)];
    return {kind: "pdcaps", kicker: "Open-K2 charger offers", badges_title: "What this charger offers", full_title: "Power table",
      header: {title: "Charger offers", facts: facts, ident: "Source capabilities · seen on " + d.active_cc + " · header " + d.header},
      badges: live.map(function (p) { return tile(pdoLabel(p), pdoStatus(p)); }),
      sections: [{title: "Offers", groups: groups, empty: "The source offered nothing."},
        {title: "Source", groups: [{title: "The source", tone: "", rows: src}], empty: ""}]};
  }

  function identityView(d) {
    var facts = ["Firmware " + (d.fw || "?")];
    if (d.git) facts.push(d.git);
    if (d.proto !== null) facts.push("protocol " + d.proto);
    var tiles = [tile("Firmware", d.fw || "unknown"), tile("Build", d.git || "unknown")];
    if (d.dirty) tiles.push(tile("Working tree", "uncommitted changes", "partial", "~"));
    else if (d.git) tiles.push(tile("Working tree", "clean", "pass", "✓"));
    if (d.proto !== null) tiles.push(tile("Remote protocol", String(d.proto)));
    var rows = [row("Firmware version", d.fw || "unknown"), row("Build", d.git || "unknown"),
      row("Built from", d.dirty ? "a tree with uncommitted changes" : "a clean commit", d.dirty ? "partial" : "")];
    if (d.proto !== null) rows.push(row("Remote protocol", d.proto));
    return {kind: "identity", kicker: "Open-K2 firmware", badges_title: "This firmware", full_title: "Firmware details",
      header: {title: "K2 firmware", facts: facts, ident: "Firmware identity record"},
      badges: tiles, sections: [{title: "Firmware", groups: [{title: "The running build", tone: "", rows: rows}], empty: ""}]};
  }

  var MODE = {plain: "plain, no Rd", ok: "OK held, Rd on CC1", plus: "PLUS held, Rd on CC2"};
  var RD = {0: "none", 1: "CC1", 2: "CC2"};
  function rd(v) { return RD.hasOwnProperty(v) ? RD[v] : hex(v, 1); }

  function resetCause(flags) {
    for (var i = 0; i < RESET_FLAG_BITS.length; i++) if ((flags >>> RESET_FLAG_BITS[i][0]) & 1) return RESET_FLAG_BITS[i][1];
    return "none";
  }
  var CAUSE_WORD = {LPWR: "low-power reset", WWDG: "window watchdog", IWDG: "watchdog", SW: "software reset", POR: "power-on",
    PIN: "reset pin", none: "unknown"};

  function bootView(d) {
    var cause = resetCause(d.reset_flags), dog = cause === "IWDG" || cause === "WWDG";
    var mode = MODE.hasOwnProperty(d.mode) ? MODE[d.mode] : d.mode;
    var keys = d.keys_pressed.length ? d.keys_pressed.join(" + ") : "none";
    var tiles = [tile("Boot mode", mode), tile("Keys at reset", keys),
      dog ? tile("Reset", CAUSE_WORD[cause] + ", " + (d.warm ? "warm" : "cold"), "partial", "~")
        : tile("Reset", CAUSE_WORD[cause] + ", " + (d.warm ? "warm" : "cold")),
      tile("VBUS", volts(d.vbus_mv)), tile("CC1", volts(d.cc1_mv)), tile("CC2", volts(d.cc2_mv))];
    tiles.push(d.move ? tile("Rd moved", rd(d.move.from) + " to " + rd(d.move.to) + " at " + d.move.ms + " ms") : tile("Rd moved", "no"));
    var decision = [row("Mode", mode), row("Keys held at reset", keys), row("Pins low", d.pins_low.length ? d.pins_low.join(", ") : "none"),
      row("Rd at boot", rd(d.pin.initial)), row("Rd now", rd(d.pin.now)), row("Rd after boot", rd(d.pin.boot)),
      row("Rd carried over", d.pin.inherited ? "yes" : "no")];
    if (d.mark !== null) decision.push(row("Pin mark", hex(d.mark, 2)));
    var sections = [{title: "Boot decision", groups: [{title: "What the K2 decided", tone: "", rows: decision}], empty: ""}];
    var mv = {title: "The Rd move", groups: [], empty: "The Rd stayed where it started."};
    if (d.move) {
      mv.groups.push({title: "Rd " + rd(d.move.from) + " to " + rd(d.move.to), tone: "", rows: [row("When", d.move.ms + " ms after reset"),
        row("VBUS then", volts(d.move.vbus_mv)), row("CC1 then", volts(d.move.cc1_mv)), row("CC2 then", volts(d.move.cc2_mv))]});
    }
    sections.push(mv);
    var trail = [];
    d.readbacks.forEach(function (x) { trail.push(row(x.ms + " ms", "switches read back " + hex(x.sw0, 2))); });
    d.notes.forEach(function (x) { trail.push(row(x.ms + " ms", (x.kind === "sw0" ? "switches set " : "CC routing set ") + hex(x.val, 2))); });
    if (d.notes_not_shown) trail.push(row("More", d.notes_not_shown + " later writes not shown"));
    sections.push({title: "First seconds", groups: trail.length ? [{title: "Writes and readbacks", tone: "", rows: trail}] : [],
      empty: "Nothing was recorded."});
    sections.push({title: "Snapshot", empty: "", groups: [{title: "At the moment of the snapshot", tone: "", rows: [
      row("Uptime", d.t_ms + " ms"), row("VBUS", volts(d.vbus_mv)), row("CC1", volts(d.cc1_mv)), row("CC2", volts(d.cc2_mv)),
      row("PLUS held", d.plus_held ? "yes" : "no"), row("Reset", CAUSE_WORD[cause] + " (" + hex(d.reset_flags, 8) + ")"),
      row("Firmware", d.fw), row("Build", d.git), row("Remote protocol", d.proto)]}]});
    return {kind: "boot", kicker: "Open-K2 boot report", badges_title: "How this K2 booted", full_title: "Boot report",
      header: {title: "K2 boot report", facts: ["Firmware " + d.fw, d.git, mode], ident: "Boot report · " + d.t_ms + " ms after reset"},
      badges: tiles, sections: sections};
  }

  var VIEWS = {emarker: emarkerView, pdcaps: pdcapsView, identity: identityView, boot: bootView};

  function decode(kind, bytes) {
    if (kind === "card") return C.decodeRecord(bytes);
    if (!DECODERS.hasOwnProperty(kind)) throw fail("bad-link", "This page does not know that kind of record.");
    return DECODERS[kind](bytes);
  }

  // What the page is given (an address, a fragment or "1e.xxx") -> {version, letter, kind, data, view}.
  function read(text) {
    var l = C.splitLink(text);
    if (l.record.length === 0) throw fail("bad-record", "The link carries no record.");
    var data = decode(l.kind, l.record);
    return {version: l.version, letter: l.letter, kind: l.kind, data: data, view: l.kind === "card" ? C.view(data) : VIEWS[l.kind](data)};
  }

  return {read: read, decode: decode, decodePdo: decodePdo, VIEWS: VIEWS};
})();

if (typeof module !== "undefined" && module.exports) module.exports = K2Records;
