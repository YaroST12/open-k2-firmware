// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// The Deep block of a format 11 charger test card (include/k2/chargertest/report_card_deep.hpp): the PD spec summary, the SVOOC
// ceiling stage and the per-range error-band graphs of the Deep PPS / AVS sweeps. The JavaScript twin of
// tools/report_card.py decode_deep(), key for key: tests/fixtures/deep_decoded.json is what the Python decoder gives for the
// golden Deep link and tests/test_cardpage.mjs requires this file to give the same. The tables below are the firmware's own
// (check list and labels from include/k2/pdspec/result.hpp and chargertest/spec_section.hpp, skip / stop / refusal texts from the
// same headers); tests/test_k2_qr.py runs this file and compares them with what tools/pdspec_blob.py parses from the headers.
// Logic only, no DOM: view.js draws plot(). Loaded by the page as a script (window.K2Deep, before card.js) and by Node.
var K2Deep = (function () {
  var MAX_BITS = 3273;        // report_card::deep::kMaxBits
  var MAX_RANGES = 4;
  var BUCKETS = 32;           // report_card::deep::kBuckets
  var NEVER = 99999999;       // pdspec::kNever: an event that never came
  var UNSEEN = -4096;         // the unseen value of the 13-bit signed cell
  var SPEC_CELLS = 27;

  // [key, label, unit, where the worst value came from]: 0 = a transition (PDO pair, or an APDO move when b is a level), 1 = an
  // APDO move, 2 = a PDO or the source. Grid order (specsec::kOrder).
  var CELLS = [
    ["accept_time", "Accept", "us", 0],
    ["ps_rdy_time", "PS_RDY time", "us", 0],
    ["src_start", "VBUS start", "us", 0],
    ["ps_rdy_early", "PS_RDY early", "mv", 0],
    ["ps_rdy_low", "PS_RDY low", "mv", 0],
    ["rise_time", "Rise time", "us", 0],
    ["fall_time", "Fall time", "us", 0],
    ["overshoot", "Overshoot", "mv", 0],
    ["accuracy", "Accuracy", "mv", 0],
    ["pps_monotonic", "PPS monotonic", "mv", 1],
    ["pps_accuracy", "PPS accuracy", "mv", 1],
    ["pps_settle", "PPS settle", "us", 1],
    ["pps_band", "PPS in band", "us", 1],
    ["pps_ps_rdy", "PPS PS_RDY", "us", 1],
    ["pps_overshoot", "PPS overshoot", "mv", 1],
    ["pps_transient", "PPS transient", "us", 1],
    ["pps_status", "PPS_Status", "count", 1],
    ["pps_status_time", "PPS_Status t", "us", 1],
    ["pps_track", "PPS tracking", "mv", 1],
    ["avs_monotonic", "AVS monotonic", "mv", 1],
    ["avs_accuracy", "AVS accuracy", "mv", 1],
    ["avs_band", "AVS in band", "us", 1],
    ["avs_ps_rdy", "AVS PS_RDY", "us", 1],
    ["avs_overshoot", "AVS overshoot", "mv", 1],
    ["avs_transient", "AVS transient", "us", 1],
    ["avs_track", "AVS tracking", "mv", 1],
    ["source_reset", "Source reset", "count", 2]
  ];
  var ENDS = ["not run","done","cancelled","fault","refused"];
  var VERDICTS = ["pending","pass","fail","skip","unverified"];
  var SKIPS = ["-","not offered","one fixed level","no PPS","no EPR","no EPR cable proof","capture busy","level refused","run stopped","not reached","K2 on the charger","no data","not selected","no AVS"];
  var STOPS = ["running","done","cancelled","VBUS over the limit","load on the output","source did not come back","step stuck","no PD contract","link inline","run time cap"];
  var STARTS = ["ok","ports busy","no PD source","link inline","K2 powered by the charger","no power monitor","already running"];
  var STEPS = {us: 100, mv: 10, ms: 1};                 // the unit of a quantised value
  var PLAN = {pps: [20, 100, 500], avs: [100, 500, 1000]};   // pdspec plan.hpp: the step of class 0..2 (class 3 = the slams)
  var PS_BITS = [[1, "Not_Supported"], [2, "no answer"], [4, "voltage"], [8, "current"], [16, "OMF"], [32, "flags"]];
  var SVOOC_ENDS = ["none", "refused", "no adapter", "aborted", "time cap", "judged", "cancelled", "stopped"];
  var SVOOC_ABORTS = ["", "over 12V", "current", "off-script", "host gone", "no monitor", "sink", "PD", "edge storm", "no adapter", "time cap",
    "stopped", "PD lost", "PD changed"];
  var SVOOC_REFUSALS = ["none", "not confirmed", "D+/D- busy", "no host link", "no power monitor", "sink on the bottom port", "inline mode",
    "PD contract not own 5 V", "current over 50 mA", "VBUS not 5 V", "no RAM: USB drive enabled", "USB-A waiver but PD is active"];

  function fail(message) {
    var e = new Error(message);
    e.code = "bad-deep";
    return e;
  }

  // LSB first, like the firmware's BitW (the same order as card.js BitReader).
  function Bits(bytes) {
    this.b = bytes; this.pos = 0;
  }
  Bits.prototype.left = function () { return this.b.length * 8 - this.pos; };
  Bits.prototype.get = function (n) {
    if (n > this.left()) throw fail("Deep block ends early");
    var v = 0;
    for (var i = 0; i < n; i++) {
      v += ((this.b[this.pos >> 3] >> (this.pos & 7)) & 1) * Math.pow(2, i);
      this.pos++;
    }
    return v;
  };
  Bits.prototype.sget = function (n) {
    var v = this.get(n);
    return v >= Math.pow(2, n - 1) ? v - Math.pow(2, n) : v;
  };

  function nameOf(list, i) { return i >= 0 && i < list.length ? list[i] : "#" + i; }

  function psText(bits) {
    var s = PS_BITS.filter(function (p) { return bits & p[0]; }).map(function (p) { return p[1]; }).join(", ");
    return s || "clean";
  }

  // A check's value in the K2's own words: us as ms with one decimal, ms, mV as V, a count.
  function fmtValue(unit, v) {
    if (v === null || v === undefined) return "-";
    if (v >= NEVER) return "not seen";
    if (unit === "us") return (v / 1000).toFixed(1) + " ms";
    if (unit === "ms") return v + " ms";
    if (unit === "mv") return (v / 1000).toFixed(3) + " V";
    return String(v);
  }

  // Where the worst value came from (pdspec_blob.py fmt_at, version 2 with no pass list).
  function fmtAt(cls, key, a, b) {
    if (cls === 0 && b < 16) return "PDO " + a + " to " + b;
    if (cls === 2) return a ? "PDO " + a : "the source";
    var kind = cls === 0 || key.indexOf("avs_") !== 0 ? "pps" : "avs";
    var c = (a >> 4) & 7;
    return "APDO " + (a & 15) + " " + (c >= 3 ? "slams" : PLAN[kind][c] + " mV") + " " + (a & 0x80 ? "down" : "up") + " to " + (b / 10).toFixed(1) + " V";
  }

  // The block (u16 LE bit count, then the bits) -> {svooc?, spec?, graphs?}; ranges are the card's decoded ranges (their algorithm
  // names the graph). Throws an Error with code "bad-deep" for a block this file cannot read.
  function decode(blob, ranges) {
    if (blob.length < 3) throw fail("Deep block is empty");
    var nbits = blob[0] | (blob[1] << 8);
    if (!(nbits > 0 && nbits <= MAX_BITS) || blob.length !== 2 + ((nbits + 7) >> 3)) throw fail("Deep block length does not match its bit count");
    var r = new Bits(blob.subarray ? blob.subarray(2) : blob.slice(2));
    var hasSpec = r.get(1), hasSv = r.get(1), mask = r.get(MAX_RANGES);
    var out = {}, i;
    if (hasSv) {
      var end = r.get(3), why = r.get(4), asks = r.get(3), word = r.get(8), vmax = r.get(9), stop = r.get(10);
      var sv = {end: SVOOC_ENDS[end], asks: asks, ceiling_ma: word * 100, vmax_mv: vmax * 100, stop_ms: stop * 100};
      if (end === 1) sv.why = nameOf(SVOOC_REFUSALS, why);
      else if (end === 3) sv.why = nameOf(SVOOC_ABORTS, why);
      out.svooc = sv;
    }
    if (hasSpec) {
      var send = r.get(3), swhy = r.get(6), unconfirmed = r.get(1), ncells = r.get(5);
      if (ncells !== SPEC_CELLS || send >= ENDS.length) throw fail("Deep spec section does not match this decoder's check list");
      var endName = ENDS[send], cells = [];
      CELLS.forEach(function (d) {
        var vi = r.get(3);
        if (vi >= VERDICTS.length) throw fail("Deep spec verdict out of range");
        var name = VERDICTS[vi];
        var cell = {key: d[0], label: d[1], unit: d[2], verdict: name};
        if (name === "skip") {
          cell.skip = nameOf(SKIPS, r.get(4));
        } else if (name !== "pending") {
          var raw = r.sget(13);
          if (raw === UNSEEN) {
            cell.value = NEVER; cell.value_text = "not seen";
          } else {
            var v = STEPS[d[2]] ? raw * STEPS[d[2]] : raw;
            cell.value = v;
            cell.value_text = d[0] === "pps_status" ? psText(v) : fmtValue(d[2], v);
            if (Math.abs(raw) >= 4094) cell.saturated = true;
          }
          if (name === "fail") {
            cell.fails = r.get(8);
            var a = r.get(8), b = r.get(8);
            cell.at_text = fmtAt(d[3], d[0], a, b);
          }
        }
        cells.push(cell);
      });
      var tally = {pass: 0, fail: 0, skip: 0, unverified: 0, pending: 0};
      cells.forEach(function (c) { tally[c.verdict]++; });
      out.spec = {end: endName, why: endName === "cancelled" || endName === "fault" ? nameOf(STOPS, swhy) :
        endName === "refused" ? nameOf(STARTS, swhy) : null, unconfirmed: !!unconfirmed, tally: tally, cells: cells};
    }
    var graphs = [];
    for (i = 0; i < MAX_RANGES; i++) {
      if (!((mask >> i) & 1)) continue;
      var lo = r.get(9), n = r.get(10), sc = r.get(3);
      var e = Math.pow(2, sc), nb = Math.min(BUCKETS, n), buckets = [];
      for (var k = 0; k < nb; k++) {
        var m = r.sget(8), dn = r.get(4), up = r.get(4);
        buckets.push(m === -128 ? null : {mean_mv: m * e, min_mv: (m - dn) * e, max_mv: (m + up) * e});
      }
      var g = {range: i, lo_mv: lo * 100, n: n, step_mv: 100, unit_mv: e, buckets: buckets};
      if (ranges && i < ranges.length) g.alg = ranges[i].alg;
      graphs.push(g);
    }
    if (graphs.length) out.graphs = graphs;
    if (r.left() >= 8) throw fail("Deep block has trailing data");
    return out;
  }

  // The geometry of one graph in a w x h box (margins l t r b): the error axis runs +-ymax so that the limit lines are always in
  // view, a run of buckets with data is one band (min..max, a polygon) and one mean line, an empty bucket breaks the run.
  // Returns numbers and SVG path strings; view.js turns them into elements. limits = the two error limits in mV.
  function plot(g, w, h, m, limits) {
    var ext = 0;
    g.buckets.forEach(function (b) { if (b) ext = Math.max(ext, Math.abs(b.min_mv), Math.abs(b.max_mv)); });
    var lim = limits.slice().sort(function (a, b) { return a - b; });
    var ymax = Math.max(lim[lim.length - 1] * 7 / 6, Math.ceil(ext * 1.1 / 50) * 50);
    var iw = w - m.l - m.r, ih = h - m.t - m.b, nb = g.buckets.length;
    var spanMv = Math.max(1, (g.n - 1) * g.step_mv);
    function px(mv) { return m.l + (mv - g.lo_mv) / spanMv * iw; }
    function py(v) { return m.t + (ymax - Math.max(-ymax, Math.min(ymax, v))) / (2 * ymax) * ih; }
    function f(v) { return (Math.round(v * 10) / 10).toString(); }
    var out = {ymax: ymax, y0: py(0), x0: px(g.lo_mv), x1: px(g.lo_mv + (g.n - 1) * g.step_mv), top: m.t, bottom: m.t + ih,
      lo_mv: g.lo_mv, hi_mv: g.lo_mv + (g.n - 1) * g.step_mv, lines: [], runs: []};
    limits.forEach(function (v) {
      out.lines.push({mv: v, y: py(v), cls: v === lim[0] ? "ok" : "caution"}, {mv: -v, y: py(-v), cls: v === lim[0] ? "ok" : "caution"});
    });
    var per = g.n / nb, run = [], i;
    function flush() {
      if (!run.length) return;
      var up = [], dn = [], mean = [];
      run.forEach(function (p) {
        var x0 = p.x, x1 = p.x;
        if (run.length === 1) { x0 = p.x - iw * per / 2 / g.n; x1 = p.x + iw * per / 2 / g.n; }
        up.push(f(x0) + " " + f(py(p.b.max_mv))); mean.push(f(x0) + " " + f(py(p.b.mean_mv)));
        dn.unshift(f(x0) + " " + f(py(p.b.min_mv)));
        if (x1 !== x0) { up.push(f(x1) + " " + f(py(p.b.max_mv))); mean.push(f(x1) + " " + f(py(p.b.mean_mv))); dn.unshift(f(x1) + " " + f(py(p.b.min_mv))); }
      });
      out.runs.push({band: "M" + up.concat(dn).join(" L") + " Z", mean: "M" + mean.join(" L")});
      run = [];
    }
    for (i = 0; i < nb; i++) {
      var b = g.buckets[i];
      if (!b) { flush(); continue; }
      run.push({x: px(g.lo_mv + ((i + 0.5) * per - 0.5) * g.step_mv), b: b});
    }
    flush();
    return out;
  }

  // The extremes of a graph for its rows: the lowest min, the highest max and the bucket with the largest mean error.
  function summary(g) {
    var lo = null, hi = null, worst = null;
    g.buckets.forEach(function (b, i) {
      if (!b) return;
      if (lo === null || b.min_mv < lo) lo = b.min_mv;
      if (hi === null || b.max_mv > hi) hi = b.max_mv;
      if (worst === null || Math.abs(b.mean_mv) > Math.abs(g.buckets[worst].mean_mv)) worst = i;
    });
    if (lo === null) return null;
    var per = g.n / g.buckets.length;
    return {min_mv: lo, max_mv: hi, worst_mv: g.buckets[worst].mean_mv,
      worst_at_mv: g.lo_mv + Math.round((worst + 0.5) * per - 0.5) * g.step_mv};
  }

  return {decode: decode, plot: plot, summary: summary, fmtValue: fmtValue, fmtAt: fmtAt, psText: psText, NEVER: NEVER,
    tables: {CELLS: CELLS, ENDS: ENDS, VERDICTS: VERDICTS, SKIPS: SKIPS, STOPS: STOPS, STARTS: STARTS, STEPS: STEPS, PLAN: PLAN,
      SVOOC_ENDS: SVOOC_ENDS, SVOOC_ABORTS: SVOOC_ABORTS, SVOOC_REFUSALS: SVOOC_REFUSALS, MAX_BITS: MAX_BITS, BUCKETS: BUCKETS}};
})();

if (typeof module !== "undefined" && module.exports) module.exports = K2Deep;
