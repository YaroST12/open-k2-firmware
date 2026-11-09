// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// The Open-K2 results viewer, logic only: no DOM, no network, no dependencies. view.js draws what this file works out.
//
// Reads the link a K2 shows as a QR code (docs/QR_SHARE.md, "Link form"):
//
//   <this page>#<version><kind>.<base64url record>
//     version "1": the record as the K2 data frame carries it; kind "c" the plain record, "z" its LZ4 block, the other
//                  letters the same (upper case = packed); kind "t" a plain ASCII text record, never packed
//     version "2": the bit-packed schema v2 record of the card ("c") and the cable ("e"), uncompressed; expandCardV2() /
//                  expandEmkV2() below turn it back into the exact version "1" record, so every decoder and view here reads
//                  a version "2" link exactly as the version "1" link of the same record
//
// (the other kinds, a cable, the source's PDOs, the firmware identity and the boot report, are read by records.js on top of
// splitLink() here) and turns the charger report card inside into (1) the card as plain data, the same keys tools/report_card.py and the
// firmware's CARDS.JSON write, and (2) a view model: the badges (one per capability, what was achieved) and the full
// results. Tables and rules mirror tools/report_card.py, tools/k2_qr.py and include/k2/chargertest/report_card.hpp;
// tests/test_cardpage.mjs checks the three against each other on golden cards (tests/fixtures/qrlink.tsv).
// Loaded by the page as a script (window.K2Card) and by Node as a module (module.exports).
var K2Card = (function () {
  var LINK_VERSION = "1";
  var PAYLOAD = 196;      // card format 7; formats 3, 4, 5 and 6 are 144, 162, 178 and 190 bytes
  var V6_PAYLOAD = 190;   // format 6 ends with the UFCS modes; format 7 adds the repeat block after them
  var V5_PAYLOAD = 178;   // format 5 ends with the cable block; format 6 adds the UFCS modes after it
  var REPEAT_AT = 190;    // format 7: times u8, last_boots u16, last_uptime_s u24 (a format 6 payload has its CRC here)
  var UFCS_AT = 178;      // format 6: 4 modes of u24 (lo 100 mV 8 bits, hi 100 mV 9 bits, current in 50 mA 7 bits); hi 0 = unused
  var MAX_UFCS = 4;
  var FORMAT = 7;         // the newest card format this file reads
  var MAX_RANGES = 4;
  var RECORD_MAGIC = 0xA6;
  var MAX_UNPACKED = 4096;

  var ALGORITHMS = ["PD2.0 fixed", "PD3.x fixed", "PD3.1+ EPR", "PD3.2+ SPR AVS", "PPS (PD3.x)",
    "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC 5V handshake", "SVOOC 5V handshake",
    "SVOOC65 5V handshake", "UFCS", "VIVO", "PD3.0 revision", "PD3.1 revision", "PD3.2 revision", "EPR AVS",
    "QC2 Class B", "QC3 Class B"];
  var GROUPS = ["PD", "QC2", "QC3", "QC3+", "AFC", "FCP", "SCP", "VOOC", "SVOOC", "SVOOC65", "UFCS", "VIVO"];
  var REASONS = ["", "disabled", "not advertised", "over voltage cap", "not implemented", "confirm no load",
    "no power monitor", "no source", "load attached", "tool busy", "fake offers",
    "request failed", "timeout", "voltage outside band", "hard reset", "source lost",
    "cancelled", "5V release failed", "offers changed", "PD active: use QC port",
    "revision cannot be forced", "select at least one test", "physical cable limit", "unsupported PDO layout",
    "charger stayed in PD", "rebooted during request", "rebooted during handover", "EPR entry refused", "no contract",
    "source shut off (needs replug)", "no PD after re-attach", "VBUS cut, source recovered", "blocked by PD",
    "no QC3+ response", "rebooted 3x", "needs external power", "PD gave up before START", "Class A only (12 V max)",
    "not unlocked", "20 V via QC3 only", "PD only (output off)", "capped below target"];
  var CAPPED = "capped below target";
  var STATES = ["Setup", "Verify 5V", "Negotiating", "Hold", "Return to 5V", "Complete", "Stopped",
    "UNSAFE: unplug source", "Detach charger", "Initializing..."];
  var HANDOVER = ["not tried", "fell back", "stayed PD", "fault", "reboot", "in PD contract"];
  var VERDICT = ["none", "pass", "fail", "skip"];
  var FC_FAILS = ["none", "need confirm", "over limit", "no power monitor", "busy", "unsupported", "no handshake",
    "no response", "voltage not reached", "overvoltage", "source lost", "aborted", "PD or load active",
    "needs load", "handshake ok", "detected"];
  var PD_ERRS = ["ok", "no source capabilities", "bad PDO position", "above the voltage cap", "bad voltage",
    "live sink attached", "busy", "PDO advertises no current"];
  var SINK_RESULTS = ["none", "ok", "rejected", "source busy", "timeout", "no GoodCRC", "hard reset", "soft reset",
    "dropped to 5V", "over the cap", "EPR refused", "bad choice"];
  var EPR_DETAILS = {136: "EPR: entry refused", 137: "EPR: source rejects cable", 138: "EPR: source has no VCONN",
    139: "EPR: refused (RDO)", 140: "EPR: source unable now", 141: "EPR: 5V PDO lacks EPR bit",
    144: "EPR: above K2's 36V monitor", 145: "EPR: above K2's 48V monitor", 146: "EPR: above the K2's monitor",
    147: "EPR offered: needs EPR cable", 148: "EPR list withheld: cable-gated",
    149: "EPR offered; entry: refused", 150: "EPR offered; entry: cable rejected",
    151: "EPR offered; entry: no VCONN", 152: "EPR offered; entry: refused (RDO)",
    153: "EPR offered; entry: unable now", 154: "EPR offered; entry: 5V PDO lacks EPR bit",
    155: "not unlocked (no OEM chain)", 156: "not unlocked (chain unfinished)", 157: "not unlocked (3 A cable)",
    158: "not unlocked (vendor not accepted)", 159: "not unlocked (source reset)"};
  var BRANDS = {0x05AC: "Apple", 0x04E8: "Samsung", 0x18D1: "Google", 0x2717: "Xiaomi", 0x12D1: "Huawei", 0x17EF: "Lenovo",
    0x413C: "Dell", 0x03F0: "HP", 0x2A70: "OnePlus", 0x22D9: "OPPO", 0x0B05: "ASUS", 0x050D: "Belkin",
    0x22B8: "Motorola", 0x19D2: "ZTE", 0x291A: "Anker"};
  var EPR_LEVELS = ["28V", "36V", "48V"];
  var QUALCOMM_VID = 0x05C6;
  var QC5_MIN_WATTS = 100;
  var MOTOROLA_VID = 0x22B8;
  var LENOVO_VID = 0x17EF;
  var GUARDS = ["unknown", "none", "cable", "direct"];
  var CABLE_KINDS = {0: "saved clone", 1: "EPR 50V 5A (built-in)", 2: "CUKTECH 6A (built-in)", 4: "OnePlus 100W (built-in)",
    255: "real cable"};

  // A reader's error: `code` is stable (tests and the page switch on it), `message` is for the person.
  function CardError(code, message) {
    var e = new Error(message);
    e.name = "CardError";
    e.code = code;
    return e;
  }

  // ---- bytes -----------------------------------------------------------------------------------------------------------

  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  // base64url, no padding -> bytes. "=" padding and "+" "/" are not part of the format and are refused.
  function base64urlDecode(s) {
    var out = [], acc = 0, bits = 0;
    if (s.length % 4 === 1) throw CardError("bad-base64", "The link is cut short: its last character is missing.");
    for (var i = 0; i < s.length; i++) {
      var v = B64.indexOf(s.charAt(i));
      if (v < 0) throw CardError("bad-base64", "The link has a character that is not part of a K2 code.");
      acc = (acc << 6) | v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push((acc >> bits) & 0xFF);
        acc &= (1 << bits) - 1;
      }
    }
    return Uint8Array.from(out);
  }

  // The standard LZ4 block format (what k2/core/lz.hpp writes; tools/k2_qr.py lz4_decompress is the Python twin).
  function lz4Inflate(data, maxOut) {
    maxOut = maxOut || MAX_UNPACKED;
    var out = [], i = 0, n = data.length;
    if (n === 0) throw CardError("bad-lz4", "The packed record is empty.");
    function ext(v) {
      for (;;) {
        if (i >= n) throw CardError("bad-lz4", "The packed record is cut short.");
        var b = data[i++];
        v += b;
        if (b !== 255) return v;
      }
    }
    while (i < n) {
      var tok = data[i++];
      var lit = (tok >> 4) === 15 ? ext(15) : tok >> 4;
      if (i + lit > n) throw CardError("bad-lz4", "The packed record is damaged (literals run past the end).");
      if (out.length + lit > maxOut) throw CardError("bad-lz4", "The packed record unpacks to more than a card holds.");
      for (var k = 0; k < lit; k++) out.push(data[i + k]);
      i += lit;
      if (i === n) break;
      if (i + 2 > n) throw CardError("bad-lz4", "The packed record is cut short.");
      var off = data[i] | (data[i + 1] << 8);
      i += 2;
      var m = ((tok & 15) === 15 ? ext(15) : tok & 15) + 4;
      if (off === 0 || off > out.length) throw CardError("bad-lz4", "The packed record is damaged (bad back-reference).");
      if (out.length + m > maxOut) throw CardError("bad-lz4", "The packed record unpacks to more than a card holds.");
      for (var j = 0; j < m; j++) out.push(out[out.length - off]);   // may overlap: byte by byte
    }
    return Uint8Array.from(out);
  }

  // CRC-16/CCITT-FALSE, the store's record check.
  function crc16(b, len) {
    var c = 0xFFFF;
    for (var i = 0; i < len; i++) {
      c ^= b[i] << 8;
      for (var k = 0; k < 8; k++) c = (c & 0x8000) ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    }
    return c;
  }

  function u(b, at, n) {   // little endian
    var v = 0;
    for (var i = n - 1; i >= 0; i--) v = v * 256 + b[at + i];
    return v;
  }

  // ---- schema v2 link records (version "2") ----------------------------------------------------------------------------
  // The JavaScript twins of tools/k2_qr.py expand_card_v2 / expand_emk_v2, key for key: a version "2" link carries the
  // bit-packed v2 record (LSB-first bit stream; varint = 7 value bits + 1 continuation bit) and expands back to the exact
  // version "1" record, so the v1 decoders above read it unchanged. The card's v2 record carries the v1 CRC-16 at its end,
  // which decodeRecord() then checks as before.

  function BitReader(b) { this.b = b; this.pos = 0; }
  BitReader.prototype.get = function (width) {   // LSB-first; past the end reads zero, as the Python twin
    var v = 0, p = 1;
    for (var i = 0; i < width; i++) {
      var at = this.pos + i;
      var byte = (at >> 3) < this.b.length ? this.b[at >> 3] : 0;
      if ((byte >> (at & 7)) & 1) v += p;
      p *= 2;
    }
    this.pos += width;
    return v;
  };
  BitReader.prototype.varint = function () {
    var v = 0, shift = 0;
    for (;;) {
      v += this.get(7) * Math.pow(2, shift);
      if (!this.get(1)) return v;
      shift += 7;
    }
  };

  var EPR_DV = [0, 280, 360, 480];                        // the v1 EPR level word (100 mV units) per 2-bit code
  var CABLE_KIND_V2 = {0: 0, 1: 1, 2: 2, 3: 4, 4: 255};    // the 3-bit cable kind code -> the v1 kind byte
  var SVDM_MASK = 0xFFFF8000 | (3 << 6) | 0x1F;            // the standard SVDM header mask, when a v2 row omits it

  // A schema v2 card record -> the 202-byte version "1" record (format 7), so decodeRecord() reads it as before.
  function expandCardV2(b) {
    var r = new BitReader(b);
    if (r.get(8) !== 2) throw CardError("bad-record", "This is not a version 2 card record.");
    var rec = new Uint8Array(4 + PAYLOAD + 2);
    rec[0] = RECORD_MAGIC; rec[1] = 1; rec[2] = PAYLOAD; rec[3] = 0;
    var p = 4;   // the payload starts at record offset 4; put16/put32 take absolute record offsets
    function put16(o, v) { rec[o] = v & 0xFF; rec[o + 1] = (v >> 8) & 0xFF; }
    function put32(o, v) { put16(o, v & 0xFFFF); put16(o + 2, (v >> 16) & 0xFFFF); }
    function group() {   // verdict u2, reason u6, detail u6, then the optional max / fail, each behind its own flag bit
      var verdict = r.get(2), reason = r.get(6), detail = r.get(6);
      var maxDv = r.get(1) ? r.get(9) : 0;
      var failDv = r.get(1) ? r.get(9) : 0;
      return [verdict, reason, detail, maxDv, failDv];
    }
    function putGroup(q, g) {
      rec[q] = (g[0] << 6) | (g[1] & 0x3F);
      var v = g[3] | (g[4] << 9) | (g[2] << 18);
      rec[q + 1] = v & 0xFF; rec[q + 2] = (v >> 8) & 0xFF; rec[q + 3] = (v >> 16) & 0xFF;
    }
    put32(p + 0, r.get(32));                              // fingerprint
    put16(p + 4, r.get(16));                              // vid
    put16(p + 6, r.get(16));                              // pid
    put16(p + 8, r.varint());                             // card number
    put16(p + 10, r.varint());                            // runs
    put32(p + 12, r.varint());                            // boots
    put32(p + 16, r.varint());                            // uptime_s
    put32(p + 20, r.get(32));                             // fw
    put16(p + 24, r.varint());                            // handover_ds
    rec[p + 26] = r.get(2);                               // spec_rev
    rec[p + 27] = r.get(4);                               // num_pdos
    rec[p + 28] = r.get(3);                               // handover
    rec[p + 29] = r.get(4);                               // state
    rec[p + 30] = r.get(6);                               // reason
    var nRanges = r.get(3);
    rec[p + 31] = nRanges;
    rec[p + 32] = r.get(8);                               // ev_flags
    rec[p + 33] = FORMAT;                                 // v2 only encodes format-7 cards
    var nl = r.get(5);
    if (nl > 20) throw CardError("bad-record", "The version 2 card record has a name that does not fit.");
    for (var i = 0; i < nl; i++) rec[p + 34 + i] = r.get(7);
    rec[p + 54] = r.get(8);                               // pdp_w
    rec[p + 55] = r.get(1) | (r.get(7) << 1);             // name_auto | pd_ma50 << 1
    var g, q, v;
    for (g = 0; g < GROUPS.length; g++) putGroup(p + 56 + 4 * g, group());
    for (i = 0; i < nRanges; i++) {
      q = p + 104 + 10 * i;
      rec[q] = r.get(5);                                  // alg
      rec[q + 1] = r.get(8);                              // pos
      var lo = r.get(9);
      var hi = r.get(9), mx = r.get(9), fail = r.get(9), ma50 = r.get(7);
      put16(q + 2, lo | (ma50 << 9));                     // lo_dv | ma50 << 9
      put16(q + 4, hi); put16(q + 6, mx); put16(q + 8, fail);
    }
    for (i = 0; i < EPR_LEVELS.length; i++) {
      q = p + 144 + 6 * i;
      var code = r.get(2);
      put16(q, code < EPR_DV.length ? EPR_DV[code] : 0);
      putGroup(q + 2, group());
    }
    if (r.get(1)) {                                       // a cable block follows
      q = p + 162;
      put32(q, r.varint());                               // seq
      put16(q + 4, r.get(16));                            // vid
      put16(q + 6, r.get(16));                            // pid
      put16(q + 8, r.get(16));                            // max_mv
      put16(q + 10, r.get(16));                           // max_ma
      var kindCode = r.get(3);
      rec[q + 12] = Object.prototype.hasOwnProperty.call(CABLE_KIND_V2, kindCode) ? CABLE_KIND_V2[kindCode] : 0;
      rec[q + 13] = r.get(8);                             // flags
      rec[q + 14] = r.get(8);                             // resumed
      rec[q + 15] = r.get(8);                             // src_flags
    }
    var nUfcs = r.get(3);
    for (i = 0; i < nUfcs; i++) {
      q = p + 178 + 3 * i;
      v = r.get(8) | (r.get(9) << 8) | (r.get(7) << 17);
      rec[q] = v & 0xFF; rec[q + 1] = (v >> 8) & 0xFF; rec[q + 2] = (v >> 16) & 0xFF;
    }
    var times = r.varint();
    rec[p + 190] = times;
    if (times > 1) {
      put16(p + 191, r.get(16));                          // last_boots
      var up = r.get(24);
      rec[p + 193] = up & 0xFF; rec[p + 194] = (up >> 8) & 0xFF; rec[p + 195] = (up >> 16) & 0xFF;
    }
    rec[200] = r.get(8);                                  // the v1 record's CRC-16, carried so the integrity check is unchanged
    rec[201] = r.get(8);
    return rec;
  }

  // A schema v2 e-marker record -> the version "1" record (flags, name[24], the 66-byte e-marker record with its reserved
  // zeros, the quirk block), so records.js decodeEmarker() reads it as before.
  function expandEmkV2(b) {
    var r = new BitReader(b);
    if (r.get(8) !== 2) throw CardError("bad-record", "This is not a version 2 cable record.");
    var i, k, v;
    var nl = r.get(5);
    if (nl > 24) throw CardError("bad-record", "The version 2 cable record has a name that does not fit.");
    var name = [];
    for (i = 0; i < nl; i++) name.push(r.get(7));
    var flags = r.get(8);
    var rl = r.get(5);
    if (rl > 20) throw CardError("bad-record", "The version 2 cable record has an e-marker name that does not fit.");
    var rname = [];
    for (i = 0; i < rl; i++) rname.push(r.get(7));
    var specRev = r.get(2);
    var vdos = [];
    for (i = 0; i < 5; i++) vdos.push(r.get(32));
    var checksum = r.get(8);
    var rec = [flags];
    for (i = 0; i < 24; i++) rec.push(i < nl ? name[i] : 0);
    for (i = 0; i < 66; i++) rec.push(0);                  // the e-marker record, its reserved bytes zero
    for (i = 0; i < rl; i++) rec[25 + i] = rname[i];
    rec[25 + 20] = specRev;
    for (i = 0; i < 5; i++) {
      v = vdos[i] >>> 0;
      rec[25 + 21 + 4 * i] = v & 0xFF; rec[25 + 22 + 4 * i] = (v >> 8) & 0xFF;
      rec[25 + 23 + 4 * i] = (v >> 16) & 0xFF; rec[25 + 24 + 4 * i] = (v >>> 24) & 0xFF;
    }
    rec[25 + 65] = checksum;
    function put32le(a, o, val) {
      a[o] = val & 0xFF; a[o + 1] = (val >> 8) & 0xFF; a[o + 2] = (val >> 16) & 0xFF; a[o + 3] = (val >>> 24) & 0xFF;
    }
    if (r.get(1)) {                                       // a quirk block follows
      var qflags = r.get(2), unmatched = r.get(2), count = r.get(4);
      rec.push(qflags, unmatched, count);
      for (var row = 0; row < count; row++) {
        var sop = r.get(2), kind = r.get(2), nreq = r.get(2), nrep = r.get(3);
        var full = r.get(1);
        var hdrVal = r.get(32);
        var hdrMask = full ? SVDM_MASK : r.get(32);
        var w = new Array(52).fill(0);
        w[0] = sop; w[1] = kind; w[2] = nreq; w[3] = nrep;
        put32le(w, 4, hdrVal); put32le(w, 8, hdrMask);
        for (k = 0; k < nreq; k++) { var rv = r.get(32); var rm = r.get(32); put32le(w, 12 + 4 * k, rv); put32le(w, 20 + 4 * k, rm); }
        for (k = 0; k < nrep; k++) put32le(w, 28 + 4 * k, r.get(32));
        for (i = 0; i < 52; i++) rec.push(w[i]);
      }
    }
    return Uint8Array.from(rec);
  }

  // The kinds a version "2" link can carry: the card and the cable, uncompressed (the same kind letters as version "1").
  var V2_EXPANDERS = {card: expandCardV2, emarker: expandEmkV2};

  // ---- the card --------------------------------------------------------------------------------------------------------

  function groupWhy(group, detail, reason) {
    if (reason === "no QC3+ response") return "no QC3+";
    if (detail) {
      if (group === "PD") {
        if (EPR_DETAILS[detail]) return EPR_DETAILS[detail];
        if (detail >= 128 && detail - 128 < PD_ERRS.length) return PD_ERRS[detail - 128];
        if (detail < SINK_RESULTS.length) return SINK_RESULTS[detail];
      } else if (detail < FC_FAILS.length) {
        return FC_FAILS[detail];
      }
    }
    return reason;
  }

  // Passive OEM label, mirrors report_card::qc_label() (never a behaviour key): QC4+/QC5 (Qualcomm VID and a PPS range),
  // "Moto TurboPower" (Motorola/Lenovo VID and a PPS APDO above 5 A), "Samsung SFC 3.0" (PPS to 20 V at 3 A), "Samsung SFC 2.0"
  // (PPS to 15 V at 3 A, or 3.3-11 V at 4.05 A behind a 5 A cable).
  function qcLabel(vid, pdpW, ranges, cableMa) {
    var pps = ranges.filter(function (r) { return r.alg === "PPS (PD3.x)"; });
    if (!pps.length) return "";
    if (vid === QUALCOMM_VID) {
      return pdpW >= QC5_MIN_WATTS || ranges.some(function (r) { return r.alg === "EPR AVS"; }) ? "QC5" : "QC4+";
    }
    var ma = function (r) { return r.max_ma || 0; };
    if ((vid === MOTOROLA_VID || vid === LENOVO_VID) && pps.some(function (r) { return ma(r) > 5000; })) return "Moto TurboPower";
    if (pps.some(function (r) { return r.hi_mv >= 20000 && ma(r) >= 3000; })) return "Samsung SFC 3.0";
    if (pps.some(function (r) { return r.hi_mv >= 15000 && ma(r) >= 3000; }) ||
        (cableMa >= 5000 && pps.some(function (r) { return r.lo_mv > 0 && r.lo_mv <= 3300 && r.hi_mv >= 11000 && ma(r) >= 4050; }))) {
      return "Samsung SFC 2.0";
    }
    return "";
  }

  function putVerdictFields(e, maxMv, failMv, reason, detail, group) {
    if (maxMv) e.max_mv = maxMv;
    if (failMv) e.fail_mv = failMv;
    if (reason) e.reason = REASONS[reason];
    if (detail) {
      e.detail = detail;
      e.why = groupWhy(group, detail, REASONS[reason]);
    }
  }

  // The card payload (144, 162, 178, 190 or 196 bytes) -> the card object tools/report_card.py decode() returns (and the firmware's
  // JSON renderer writes: key "seq" of the cable only when it is not 0).
  function decodePayload(b) {
    if (b.length !== 144 && b.length !== 162 && b.length !== V5_PAYLOAD && b.length !== V6_PAYLOAD && b.length !== PAYLOAD) {
      throw CardError("bad-card", "The card is " + b.length + " bytes; a card is 144, 162, " + V5_PAYLOAD + ", " + V6_PAYLOAD + " or " + PAYLOAD + ".");
    }
    var fmt = b[33];
    if (fmt > FORMAT) throw CardError("newer-card", "This card was made by a newer K2 than this page understands (format " + fmt + ").");
    if (b[28] >= HANDOVER.length || b[29] >= STATES.length || b[30] >= REASONS.length || b[31] > MAX_RANGES) {
      throw CardError("bad-card", "The card is damaged (a header field is out of range).");
    }
    var name = "";
    for (var i = 34; i < 54 && b[i] !== 0; i++) name += b[i] < 128 ? String.fromCharCode(b[i]) : "�";
    var groups = {};
    var g, q, e, v, detail, maxMv, failMv, reason, verdict;
    for (g = 0; g < GROUPS.length; g++) {
      q = 56 + 4 * g;
      verdict = b[q] >> 6;
      reason = b[q] & 0x3F;
      if (reason >= REASONS.length) throw CardError("bad-card", "The card is damaged (a reason is out of range).");
      if (verdict === 0) continue;
      if (fmt === 0) {
        detail = b[q + 1];
        maxMv = Math.floor((u(b, q + 2, 2) + 50) / 100) * 100;
        failMv = 0;
      } else {
        v = u(b, q + 1, 3);
        detail = (v >> 23 & 1) ? 128 + (v >> 18 & 0x1F) : v >> 18 & 0x1F;
        maxMv = (v & 0x1FF) * 100;
        failMv = (v >> 9 & 0x1FF) * 100;
      }
      e = {verdict: VERDICT[verdict]};
      if (g === 0 && fmt >= 3 && (b[55] >> 1)) e.max_ma = (b[55] >> 1) * 50;
      putVerdictFields(e, maxMv, failMv, reason, detail, GROUPS[g]);
      groups[GROUPS[g]] = e;
    }
    var ranges = [];
    for (i = 0; i < b[31]; i++) {
      q = 104 + 10 * i;
      if (b[q] >= ALGORITHMS.length) throw CardError("bad-card", "The card is damaged (a range type is out of range).");
      var lo = u(b, q + 2, 2);
      var r = {alg: ALGORITHMS[b[q]], lo_mv: (lo & 0x1FF) * 100, hi_mv: u(b, q + 4, 2) * 100,
        max_mv: u(b, q + 6, 2) * 100, fail_mv: u(b, q + 8, 2) * 100};
      if (fmt >= 3 && (lo >> 9)) r.max_ma = (lo >> 9) * 50;
      ranges.push(r);
    }
    var eprLevels = {};
    if (fmt >= 4) {
      for (i = 0; i < EPR_LEVELS.length; i++) {
        q = 144 + 6 * i;
        if (u(b, q, 2) === 0) continue;   // no EPR at this level
        verdict = b[q + 2] >> 6;
        reason = b[q + 2] & 0x3F;
        if (reason >= REASONS.length) throw CardError("bad-card", "The card is damaged (an EPR reason is out of range).");
        v = u(b, q + 3, 3);
        detail = (v >> 23 & 1) ? 128 + (v >> 18 & 0x1F) : v >> 18 & 0x1F;
        e = {verdict: VERDICT[verdict]};
        putVerdictFields(e, (v & 0x1FF) * 100, (v >> 9 & 0x1FF) * 100, reason, detail, "PD");
        eprLevels[EPR_LEVELS[i]] = e;
      }
    }
    var card = {no: u(b, 8, 2), name: name, fp: hex8(u(b, 0, 4)), vid: u(b, 4, 2), pid: u(b, 6, 2)};
    if (BRANDS[card.vid]) card.brand = BRANDS[card.vid];
    if (fmt >= 2) {
      if (b[54]) card.pdp_w = b[54];
      if ((b[55] & 1) && name) card.name_auto = true;
    }
    var label = qcLabel(card.vid, card.pdp_w || 0, ranges, fmt >= 5 && b.length >= V5_PAYLOAD ? u(b, 170, 2) : 0);
    if (label) card.qc = label;
    card.runs = u(b, 10, 2);
    card.boots = u(b, 12, 4);
    card.uptime_s = u(b, 16, 4);
    if (fmt >= 7 && b.length >= PAYLOAD && b[REPEAT_AT] > 1) {   // identical repeats of the stored run (keys only when there were any)
      card.times = b[REPEAT_AT];
      card.last_boots = u(b, REPEAT_AT + 1, 2);
      card.last_uptime_s = u(b, REPEAT_AT + 3, 3);
    }
    card.fw = hex8(u(b, 20, 4));
    card.state = STATES[b[29]];
    card.reason = REASONS[b[30]];
    card.pdos = b[27];
    card.spec_rev = b[26];
    card.handover = HANDOVER[b[28]];
    card.handover_ms = u(b, 24, 2) * 100;
    card.groups = groups;
    card.ranges = ranges;
    if (fmt >= 4) card.epr_levels = eprLevels;
    if (fmt >= 5 && b.length >= V5_PAYLOAD) {
      q = 162;
      var cable = {};
      var seq = u(b, q, 4);
      if (seq) cable.seq = seq;
      cable.vid = u(b, q + 4, 2);
      cable.pid = u(b, q + 6, 2);
      cable.max_mv = u(b, q + 8, 2);
      cable.max_ma = u(b, q + 10, 2);
      cable.kind = b[q + 12];
      cable.observed = b[q + 13] & 1;
      cable.spoofed = (b[q + 13] >> 1) & 1;
      if (b[q + 13] & 4) cable.self_powered = 1;
      var gd = (b[q + 13] >> 3) & 7;
      if (gd >= 1 && gd <= 4) cable.guard = GUARDS[gd - 1];
      if (b[q + 13] & 0x40) cable.guard_bypassed = 1;
      card.cable = cable;
      if (b[q + 15] & 1) card.oppo_svid = 1;   // src_flags: the source listed OPPO's SVID (key only then, as in the firmware's JSON)
      if (b[q + 15] >> 1) card.scp_ma = (b[q + 15] >> 1) * 50;   // bits 1..7: the adapter's SCP current limit, 50 mA units (key only when read)
    }
    if (fmt >= 6 && b.length >= V6_PAYLOAD) {   // the UFCS modes; older cards have none (their UFCS amps are unknown: volts only)
      var modes = [];
      for (i = 0; i < MAX_UFCS; i++) {
        v = u(b, UFCS_AT + 3 * i, 3);
        if ((v >> 8 & 0x1FF) === 0) break;   // an unused slot ends the list
        var m = {lo_mv: (v & 0xFF) * 100, hi_mv: (v >> 8 & 0x1FF) * 100};
        if (v >> 17) m.max_ma = (v >> 17) * 50;
        modes.push(m);
      }
      if (modes.length) card.ufcs = modes;
    }
    return card;
  }

  function hex8(n) {
    var s = (n >>> 0).toString(16).toUpperCase();
    return "00000000".slice(s.length) + s;
  }

  // The record the K2 stores (and the link carries): 0xA6, type, payload length, 0, payload, CRC-16 -> the card.
  function decodeRecord(rec) {
    if (rec.length < 6 || rec[0] !== RECORD_MAGIC) throw CardError("bad-record", "This is not a K2 report card.");
    var ln = rec[2];
    if (rec.length !== 4 + ln + 2) throw CardError("bad-record", "The card is the wrong length; the link may be cut short.");
    var crc = rec[rec.length - 2] | (rec[rec.length - 1] << 8);
    if (crc16(rec, rec.length - 2) !== crc) throw CardError("bad-crc", "The card's check value does not match: the link was changed or cut.");
    return decodePayload(rec.subarray(4, 4 + ln));
  }

  // The kind letters of a link: lowercase = the record as built, uppercase (and the card's "z") = the LZ4 block of it.
  // "t" is a plain ASCII text record (version "1" only, never packed), the QR scan test's code.
  var KIND_LETTERS = {c: "card", z: "card", e: "emarker", E: "emarker", p: "pdcaps", P: "pdcaps",
    i: "identity", I: "identity", b: "boot", B: "boot", t: "text"};
  var PACKED_LETTERS = "zEPIB";
  var LINK_VERSION_V2 = "2";

  // What the page is given: the address with "#...", the fragment, or just "1z.xxx". Returns {version, letter, kind, record}:
  // the record bytes of whatever the link carries, at the version "1" shape (a version "2" record expanded, a packed letter
  // inflated); records.js decodes the kinds that are not a card.
  function splitLink(text) {
    var s = String(text == null ? "" : text).trim();
    var at = s.indexOf("#");
    if (at >= 0) s = s.slice(at + 1);
    else if (/^https?:\/\//i.test(s)) s = "";   // an address with no "#": the code part never arrived
    if (s === "") throw CardError("missing", "There is nothing to show in this address.");
    if (/^K2:/i.test(s)) {
      throw CardError("data-frame", "That is the K2 data code, which is for the import tool. Scan the link code instead (OK on the K2 switches).");
    }
    var m = /^(\d)([a-zA-Z])\.([^\s]*)$/.exec(s);
    if (!m) throw CardError("bad-link", "This is not a K2 results link.");
    if (m[1] !== LINK_VERSION && m[1] !== LINK_VERSION_V2) {
      throw CardError("bad-version", "This link is version " + m[1] + "; this page reads versions " + LINK_VERSION + " and " +
        LINK_VERSION_V2 + ". Update the K2 firmware or reload the page.");
    }
    if (!Object.prototype.hasOwnProperty.call(KIND_LETTERS, m[2])) {
      throw CardError("bad-link", "This link has a form (" + m[2] + ") this page does not know. Reload the page, or update it.");
    }
    var bytes = base64urlDecode(m[3]);
    if (m[1] === LINK_VERSION_V2) {
      var expand = V2_EXPANDERS[KIND_LETTERS[m[2]]];
      if (!expand) {
        throw CardError("bad-version", "A version " + LINK_VERSION_V2 + " link exists only for the card and the cable, not for the form (" +
          m[2] + ") of this link.");
      }
      bytes = expand(bytes);
    } else if (PACKED_LETTERS.indexOf(m[2]) >= 0) bytes = lz4Inflate(bytes);
    return {version: m[1], letter: m[2], kind: KIND_LETTERS[m[2]], record: bytes};
  }

  // A report card's link: {version, kind, card}. Another kind's link is refused here (the page reads it through records.js).
  function parseLink(text) {
    var l = splitLink(text);
    if (l.kind !== "card") throw CardError("not-a-card", "This link is not a charger test card.");
    return {version: l.version, kind: l.letter, card: decodeRecord(l.record)};
  }

  // ---- the view model --------------------------------------------------------------------------------------------------

  var GLYPH = {pass: "✓", partial: "~", fail: "✕", skip: "–"};
  var TONE_WORD = {pass: "achieved", partial: "partly", fail: "failed", skip: "not offered"};

  function volts(mv) {   // 19400 -> "19.4V", 5000 -> "5V", 3300 -> "3.3V"
    var dv = Math.round(mv / 100);
    return (dv % 10 ? (dv / 10).toFixed(1) : String(dv / 10)) + "V";
  }
  function volts2(mv) { return volts(mv).slice(0, -1); }   // without the unit, for ranges: "5-19.4V"
  function amps(ma) { return ma % 1000 ? (ma / 1000).toFixed(2).replace(/0$/, "") + "A" : ma / 1000 + "A"; }
  function span(lo, hi) { return volts2(lo) + "-" + volts(hi); }

  var GROUP_NAMES = {PD: "PD", QC2: "QC 2.0", QC3: "QC 3.0", "QC3+": "QC 3+", AFC: "AFC", FCP: "FCP", SCP: "SCP",
    VOOC: "VOOC", SVOOC: "SVOOC", SVOOC65: "SVOOC 65W", UFCS: "UFCS", VIVO: "VIVO"};
  var RANGE_NAMES = {"PPS (PD3.x)": "PPS", "PD3.2+ SPR AVS": "AVS", "EPR AVS": "EPR AVS"};

  function rangeName(alg) { return RANGE_NAMES[alg] || alg; }

  // One range's tone, the Results grid's table (result_layout::roll_up; include/k2/chargertest/card_tone.hpp range()). The card
  // keeps no per-step verdicts, so the tally is rebuilt from the row: a failure with something reached is amber (capped or
  // stopped), a failure with nothing reached is red, no failure with the top reached (within the 5 % VBUS tolerance: a 20 V step
  // that measured 19.4 V passed) is green, no failure but the top never exercised is amber, nothing measured is grey.
  function rangeVerdict(r, capped) {
    if (r.fail_mv) return r.max_mv ? "partial" : "fail";
    if (!r.max_mv) return "skip";
    var maxDv = Math.round(r.max_mv / 100), hiDv = Math.round(r.hi_mv / 100);
    return maxDv + Math.floor(hiDv / 20) < hiDv ? "partial" : "pass";
  }

  // A legacy protocol that never answered (nothing passed) is "not offered": grey, never red.
  function groupTone(name, g) {
    if (g.verdict === "pass") return "pass";
    if (g.verdict !== "fail") return "skip";
    if (g.max_mv) return "partial";
    return name === "PD" ? "fail" : "skip";
  }

  function rangeBadge(r, capped) {
    var tone = rangeVerdict(r, capped), name = rangeName(r.alg), label, status;
    var amp = r.max_ma ? " " + amps(r.max_ma) : "";
    if (tone === "pass") {
      label = name + " " + span(r.lo_mv, r.hi_mv);
      status = (r.max_mv < r.hi_mv ? "up to " + volts(r.max_mv) : "whole range") + (amp ? ", up to" + amp : "");
    } else if (tone === "partial" && r.fail_mv) {
      label = name + " " + span(r.lo_mv, r.max_mv);
      status = capped ? "capped (" + volts(r.hi_mv) + " offered)" : "fails at " + volts(r.fail_mv) + " (" + volts(r.hi_mv) + " offered)";
    } else if (tone === "partial") {
      label = name + " " + span(r.lo_mv, r.max_mv);
      status = "stopped (" + volts(r.hi_mv) + " offered)";
    } else if (tone === "fail") {
      label = name + " " + span(r.lo_mv, r.hi_mv);
      status = "fails at " + volts(r.fail_mv) + ", none reached";
    } else {
      label = name + " " + span(r.lo_mv, r.hi_mv);
      status = "not run";
    }
    return {id: "range:" + r.alg + ":" + r.lo_mv + "-" + r.hi_mv, kind: "range", tone: tone, label: label, status: status};
  }

  // The fixed PDOs are fine when the group failed only through a range (the failing target is recorded as 0 and something
  // was reached): the PD group covers fixed levels and ranges together.
  function fixedPdOk(g, ranges) {
    if (g.verdict === "pass") return true;
    return g.verdict === "fail" && !g.fail_mv && !!g.max_mv && ranges.some(function (r) { return !!r.fail_mv; });
  }

  function pdBadge(card) {
    var g = card.groups.PD;
    if (!g) return {id: "pd", kind: "pd", tone: "skip", label: "PD", status: "not run"};
    var name = card.pdp_w ? "PD " + card.pdp_w + "W" : g.max_mv ? "PD " + volts(g.max_mv) : "PD";
    var cur = g.max_ma ? " " + amps(g.max_ma) : "";
    if (g.verdict === "pass") {
      return {id: "pd", kind: "pd", tone: "pass", label: name, status: g.max_mv ? "levels up to " + volts(g.max_mv) + cur : "all levels"};
    }
    // failed only through a range: the group's max is the range's, so it says nothing about the fixed levels
    if (fixedPdOk(g, card.ranges)) return {id: "pd", kind: "pd", tone: "pass", label: name, status: "fixed levels passed"};
    if (g.verdict === "fail") {
      var why = g.why || g.reason;
      if (g.max_mv) return {id: "pd", kind: "pd", tone: "partial", label: name, status: "fails at " + volts(g.fail_mv) + (why ? ": " + why : "")};
      return {id: "pd", kind: "pd", tone: "fail", label: "PD", status: why || "failed"};
    }
    if (g.verdict === "skip") return {id: "pd", kind: "pd", tone: "skip", label: "PD", status: g.why || g.reason || "skipped"};
    return {id: "pd", kind: "pd", tone: "skip", label: "PD", status: "not run"};
  }

  function eprBadge(card) {
    var lv = card.epr_levels;
    if (!lv) return null;   // formats before 4 did not store EPR
    var names = EPR_LEVELS.filter(function (n) { return lv[n]; });
    if (!names.length) return {id: "epr", kind: "epr", tone: "skip", label: "EPR", status: "none offered"};
    var passed = names.filter(function (n) { return lv[n].verdict === "pass"; });
    var failed = names.filter(function (n) { return lv[n].verdict === "fail"; });
    if (passed.length) {
      var top = passed[passed.length - 1];
      var bad = failed.length ? failed[0] : "";
      return {id: "epr", kind: "epr", tone: bad ? "partial" : "pass", label: "EPR " + top,
        status: bad ? bad + " fails" : "up to " + top};
    }
    if (failed.length) return {id: "epr", kind: "epr", tone: "fail", label: "EPR", status: failed[0] + " fails: " + (lv[failed[0]].why || lv[failed[0]].reason || "failed")};
    var first = lv[names[0]];
    return {id: "epr", kind: "epr", tone: "skip", label: "EPR", status: first.why || first.reason || "skipped"};
  }

  // One UFCS mode as the K2 shows it: the voltage span and the current the source advertises for it, "5-20V 3A". Cards before
  // format 6 carry no modes (their current is unknown: volts only), and a mode with no current read is the span alone.
  function ufcsModeText(m) { return span(m.lo_mv, m.hi_mv) + (m.max_ma ? " " + amps(m.max_ma) : ""); }

  function groupBadge(name, g, card) {
    var label = GROUP_NAMES[name] || name;
    var okDetail = g.detail === 14 || g.detail === 15;   // "handshake ok" / "detected": a pass with no voltage
    var why = g.why || g.reason;
    var modes = name === "UFCS" && card && card.ufcs ? card.ufcs.map(ufcsModeText).join(", ") : "";
    if (g.verdict === "pass") {
      return {id: "grp:" + name, kind: "group", tone: "pass", label: label + (g.max_mv && !okDetail ? " " + volts(g.max_mv) : ""),
        status: okDetail ? why || "handshake ok" : g.max_mv ? "up to " + volts(g.max_mv) + (modes ? " (" + modes + ")" : "") : "passed"};
    }
    if (g.verdict === "fail") {
      if (g.max_mv) return {id: "grp:" + name, kind: "group", tone: "partial", label: label + " " + volts(g.max_mv),
        status: (g.fail_mv ? "fails at " + volts(g.fail_mv) : why || "stopped") + (modes ? " (" + modes + ")" : "")};
      // never answered: not offered (grey, with the reason), the Results grid's "No reply" row
      return {id: "grp:" + name, kind: "group", tone: "skip", label: label, status: why || "not offered"};
    }
    return {id: "grp:" + name, kind: "group", tone: "skip", label: label,
      status: g.reason === "not advertised" || !why ? "not offered" : why};
  }

  // The badges: PD, EPR, each PPS / AVS range, each other protocol the run tried; achieved first, grey last.
  function badges(card) {
    var out = [];
    var capped = !!card.groups.PD && card.groups.PD.reason === CAPPED;
    out.push(pdBadge(card));
    var epr = eprBadge(card);
    if (epr) out.push(epr);
    card.ranges.forEach(function (r) { out.push(rangeBadge(r, capped)); });
    GROUPS.slice(1).forEach(function (n) { if (card.groups[n]) out.push(groupBadge(n, card.groups[n], card)); });
    var rank = {pass: 0, partial: 1, fail: 2, skip: 3};
    out.forEach(function (b, i) { b.order = i; b.glyph = GLYPH[b.tone]; b.word = TONE_WORD[b.tone]; });
    out.sort(function (a, b) { return rank[a.tone] - rank[b.tone] || a.order - b.order; });
    return out;
  }

  function pdRevision(n) { return n === 2 ? "PD 3.x" : n === 1 ? "PD 2.0" : ""; }

  function title(card) {
    if (card.name) return card.name;
    if (card.vid || card.pid) return hex4(card.vid) + ":" + hex4(card.pid);
    return "#" + card.fp;
  }
  function hex4(n) { var s = n.toString(16).toUpperCase(); return "0000".slice(s.length) + s; }

  function duration(s) {
    if (s >= 3600) return Math.floor(s / 3600) + "h " + Math.floor(s % 3600 / 60) + "m";
    if (s >= 60) return Math.floor(s / 60) + "m " + s % 60 + "s";
    return s + "s";
  }

  function header(card) {
    var facts = [];
    var rev = pdRevision(card.spec_rev);
    if (rev) facts.push(rev);
    if (card.pdp_w) facts.push(card.pdp_w + "W");
    if (card.brand) facts.push(card.brand);
    if (card.qc) facts.push(card.qc);
    return {title: title(card), auto_name: !!card.name_auto, facts: facts, fingerprint: card.fp, no: card.no,
      boot: card.boots, run: card.runs, uptime: duration(card.uptime_s),
      ident: "Card #" + card.no + " · run " + card.runs + " · boot " + card.boots + " · up " + duration(card.uptime_s) +
        (card.times > 1 ? " · same result x" + card.times : "") + " · fingerprint " + card.fp};
  }

  function row(label, value, tone) { return {label: label, value: value, tone: tone || ""}; }

  function groupRows(name, g, detailRow, extra) {
    var tone = groupTone(name, g);
    var rows = [row("Result", TONE_WORD[tone], tone)];
    var okDetail = g.detail === 14 || g.detail === 15;
    if (okDetail) rows.push(row("Max reached", g.why || "handshake ok"));
    else rows.push(row("Max reached", g.max_mv ? volts(g.max_mv) : "none"));
    if (g.fail_mv && tone !== "skip") rows.push(row("Failed at", volts(g.fail_mv), "fail"));
    if (g.max_ma) rows.push(row("Max current", amps(g.max_ma)));
    if (g.why && !okDetail && g.verdict !== "pass") rows.push(row("Why", g.why, tone === "skip" ? "" : "partial"));
    if (g.reason && g.reason !== g.why && g.reason !== "no QC3+ response") rows.push(row("Reason", g.reason));
    if (extra) extra.forEach(function (x) { rows.push(x); });   // e.g. the UFCS modes
    if (detailRow && g.detail) rows.push(row("Detail code", String(g.detail)));
    return rows;
  }

  function sections(card) {
    var out = [];
    var capped = !!card.groups.PD && card.groups.PD.reason === CAPPED;
    var pd = [];
    var g = card.groups.PD;
    if (g) {
      var fixed = groupRows("PD", g, true);
      if (g.verdict === "fail" && fixedPdOk(g, card.ranges)) {
        // the group's max, reason and detail describe the range that stopped early (shown under its own heading)
        fixed = [row("Result", "fixed levels passed; a range stopped early", "pass")];
        if (g.max_ma) fixed.push(row("Max current", amps(g.max_ma)));
      }
      pd.push({title: "PD fixed levels", tone: pdBadge(card).tone, rows: fixed});
    }
    card.ranges.forEach(function (r) {
      var b = rangeBadge(r, capped);
      var rows = [row("Result", b.status, b.tone), row("Offered", span(r.lo_mv, r.hi_mv)),
        row("Max reached", r.max_mv ? volts(r.max_mv) : "none")];
      if (r.fail_mv) rows.push(row("Failed at", volts(r.fail_mv), "fail"));
      if (capped && b.tone === "partial" && r.fail_mv) rows.push(row("Reason", "output capped below the advertised range", "partial"));
      if (r.max_ma) rows.push(row("Max current", amps(r.max_ma)));
      pd.push({title: rangeName(r.alg) + " " + span(r.lo_mv, r.hi_mv), tone: b.tone, rows: rows});
    });
    if (card.epr_levels) {
      EPR_LEVELS.forEach(function (n) {
        var e = card.epr_levels[n];
        if (!e) return;
        var tone = e.verdict === "pass" ? "pass" : e.verdict === "fail" ? (e.max_mv ? "partial" : "fail") : "skip";
        var rows = [row("Result", TONE_WORD[tone], tone), row("Max reached", e.max_mv ? volts(e.max_mv) : "none")];
        if (e.fail_mv) rows.push(row("Failed at", volts(e.fail_mv), "fail"));
        if (e.why) rows.push(row("Why", e.why, tone === "pass" ? "" : "partial"));
        pd.push({title: "EPR " + n, tone: tone, rows: rows});
      });
    }
    if (pd.length) out.push({title: "Power Delivery", groups: pd});

    var legacy = [];
    GROUPS.slice(1).forEach(function (n) {
      var lg = card.groups[n];
      if (!lg) return;
      var b = groupBadge(n, lg, card);
      var modeRows = n === "UFCS" && card.ufcs ? card.ufcs.map(function (m, i) { return row("Mode " + (i + 1), ufcsModeText(m)); }) : null;
      legacy.push({title: GROUP_NAMES[n] || n, tone: b.tone, rows: groupRows(n, lg, true, modeRows)});
    });
    out.push({title: "Other fast-charge protocols", groups: legacy, empty: "None were tested."});

    var c = card.cable;
    if (c && (c.vid || c.pid || c.max_mv || c.max_ma || c.kind || c.observed || c.spoofed || c.seq)) {
      var rows = [row("Cable", (CABLE_KINDS[c.kind] || "kind " + c.kind) + (c.kind === 0 && c.seq ? " #" + c.seq : ""))];
      if (c.max_mv || c.max_ma) rows.push(row("Limits used", (c.max_mv ? volts(c.max_mv) : "-") + " " + (c.max_ma ? amps(c.max_ma) : "-")));
      if (c.vid || c.pid) rows.push(row("Cable ID", hex4(c.vid) + ":" + hex4(c.pid)));
      rows.push(row("Cable seen on CC", c.observed ? "yes" : "no"));
      rows.push(row("K2 spoofed a cable", c.spoofed ? "yes" : "no"));
      if (c.self_powered) rows.push(row("Power", "the charger under test powered the K2"));
      if (c.guard) rows.push(row("Cable check", c.guard + (c.guard_bypassed ? " (started anyway)" : "")));
      out.push({title: "Cable", groups: [{title: "Cable of this run", tone: "", rows: rows}]});
    }

    var run = [row("State", card.state), row("Handover", card.handover + (card.handover_ms ? " (" + card.handover_ms + " ms)" : ""))];
    if (card.reason) run.push(row("Reason", card.reason));
    run.push(row("Source PDOs", String(card.pdos)), row("Card number", "#" + card.no), row("Run", "#" + card.runs + ", boot " + card.boots),
      row("K2 uptime", duration(card.uptime_s)), row("Firmware", card.fw), row("Fingerprint", card.fp));
    // Identical re-runs are counted on this card, not stored again: say how many and when the newest one was.
    if (card.times > 1) run.splice(run.length - 3, 0, row("Same result", card.times + " runs, last at boot " + card.last_boots + ", up " + duration(card.last_uptime_s)));
    if (card.vid || card.pid) run.splice(2, 0, row("USB ID", hex4(card.vid) + ":" + hex4(card.pid) + (card.brand ? " (" + card.brand + ")" : "")));
    out.push({title: "Run", groups: [{title: "The test run", tone: "", rows: run}]});
    return out;
  }

  function view(card) {
    return {header: header(card), badges: badges(card), sections: sections(card)};
  }

  return {
    LINK_VERSION: LINK_VERSION, LINK_VERSION_V2: LINK_VERSION_V2, CardError: CardError,
    base64urlDecode: base64urlDecode, lz4Inflate: lz4Inflate, crc16: crc16,
    expandCardV2: expandCardV2, expandEmkV2: expandEmkV2, BitReader: BitReader,
    decodePayload: decodePayload, decodeRecord: decodeRecord, parseLink: parseLink, splitLink: splitLink,
    badges: badges, sections: sections, header: header, view: view,
    rangeVerdict: rangeVerdict, volts: volts, BRANDS: BRANDS
  };
})();

if (typeof module !== "undefined" && module.exports) module.exports = K2Card;
