// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// The charger-test grid as data: the web port of the device's Results grid (src/chargertest/result_layout.cpp: which
// rows, which cells, which verdicts; include/k2/chargertest/step_label.hpp and tools/step_label.py: the words of a
// cell), over the rows the remote returns (k2proto decodeChargerTest). Pure code, no DOM: layout() returns the rows the
// K2Kit renderer draws. The golden vectors in tests/fixtures/step_labels.txt are shared with the firmware and the
// Python tool, so the device, the host tools and this page word a step the same. Embedded into k2.html by
// tools/web/sync_proto.py; keep it free of a literal script-close tag.
var CtGrid = (function () {
  var PLAN_ROW_MA = 1000;     // a plan row requests at most this, except the full-current PPS row
  var HUNT_MA = 5000;         // above it a PD-family row is the OEM 6 A probe ("Mi")
  var SWEEP_DETAIL = 161;     // an older firmware's deep-sweep marker (newer: the row's sweep flag)
  var PD_FAMILY = ["pd2-fixed", "pd3x-fixed", "pps"];
  var PROGRAMMABLE = ["pps", "spr-avs", "epr-avs"];
  var REVISIONS = { "pd3.0-revision": "PD3.0 rev", "pd3.1-revision": "PD3.1 rev", "pd3.2-revision": "PD3.2 rev" };
  var LEGACY = { qc2: "QC2", qc3: "QC3", "qc3+": "QC3+", afc: "AFC", fcp: "FCP", scp: "SCP", vooc: "VOOC", svooc: "SVOOC",
                 svooc65: "SVOOC65", ufcs: "UFCS", vivo: "VIVO", qc2b: "QC2B", qc3b: "QC3B" };
  // The device's display_name per algorithm (the row label).
  var NAMES = { "pd2-fixed": "PD2.0", "pd3x-fixed": "PD3.x", epr: "EPR", "spr-avs": "SPR AVS", pps: "PPS", "epr-avs": "EPR AVS",
                qc2: "QC2", qc3: "QC3", "qc3+": "QC3+", qc2b: "QC2B", qc3b: "QC3B", afc: "AFC", fcp: "FCP", scp: "SCP",
                vooc: "VOOC", svooc: "SVOOC", svooc65: "SVOOC65", ufcs: "UFCS", vivo: "VIVO" };
  var PD_GROUP = ["pd2-fixed", "pd3x-fixed", "epr", "spr-avs", "pps", "epr-avs"];
  var RANGE_ALGS = ["pps", "spr-avs", "epr-avs"];
  // Sections (the device's order of families); a rule separates two runs of different sections.
  var SEC_HUAWEI = 0, SEC_SAMSUNG = 1, SEC_OPPO = 2, SEC_XIAOMI = 3, SEC_PD = 5, SEC_QC = 6;
  // The grid's metrics, from the device's Results grid: 8 px glyphs, 2 px padding, 1 px gap, 163 px of cells, 5 cells at most.
  var ADV = 8, PAD = 2, GAP = 1, AVAIL = 163, MAX_CELLS = 5;

  // ---- the words of a step (tools/step_label.py) -----------------------------------------------------------------
  function milli(v) {
    var frac = Math.floor((v % 1000) / 10), s = String(Math.floor(v / 1000));
    if (!frac) return s;
    return s + "." + (frac < 10 ? "0" + frac : String(frac)).replace(/0+$/, "");
  }
  function has(list, x) { return list.indexOf(x) >= 0; }
  function isHunt(s) { return has(PD_FAMILY, s.protocol) && (s.request_ma || 0) > HUNT_MA; }
  function isSweep(s) { return has(PROGRAMMABLE, s.protocol) && (s.detail === SWEEP_DETAIL || s.sweep === true); }
  function isFullCurrent(s) { return s.protocol === "pps" && (s.request_ma || 0) > PLAN_ROW_MA && s.request_ma <= HUNT_MA; }
  function isAvs(s) { return s.protocol === "spr-avs" || s.protocol === "epr-avs" || (s.protocol === "epr-fixed" && !!s.avs); }

  function protocolWord(s) {
    var p = s.protocol, hasV = !!s.target_mv;
    if (isHunt(s)) return "Mi";
    if (p === "pd2-fixed") return hasV ? "PD" : "PD2.0";
    if (p === "pd3x-fixed") return hasV ? "PD" : "PD3.x";
    if (p === "epr-fixed") return s.avs ? "AVS" : "EPR";
    if (p === "spr-avs" || p === "epr-avs") return "AVS";
    if (p === "pps") return "PPS";
    if (REVISIONS[p]) return REVISIONS[p];
    var base = p.split("-5v-handshake")[0];
    return LEGACY[base] || p;
  }
  // The advertised current of a fixed or PPS PDO in mA (0 for anything else). The kind bits are the top two: unsigned shifts.
  function pdoMa(raw) {
    raw = raw >>> 0;
    if (!raw) return 0;
    var kind = raw >>> 30;
    if (kind === 0) return (raw & 0x3FF) * 10;
    if (kind === 3 && ((raw >>> 28) & 3) === 0) return (raw & 0x7F) * 50;
    return 0;
  }
  function ampsMa(s) {
    if (isHunt(s) || isFullCurrent(s)) return s.request_ma || 0;
    if (s.protocol === "pd2-fixed" || s.protocol === "pd3x-fixed" || s.protocol === "pps" || s.protocol === "epr-fixed")
      return isAvs(s) ? 0 : pdoMa(s.raw_pdo || 0);
    return s.protocol === "scp" ? (s.request_ma || 0) : 0;
  }
  // "20V 5A", "12V", "11→8V" ("" with no voltage). prev = the row before: a continuous row of the same protocol and slot reads from→to.
  function params(s, prev, amps) {
    var mv = s.target_mv || 0;
    if (!mv) return "";
    if (s.continuous && prev && has(PROGRAMMABLE, s.protocol) && prev.protocol === s.protocol && prev.pos === s.pos &&
        !isHunt(prev) && !isHunt(s) && prev.target_mv && prev.target_mv !== mv)
      return milli(prev.target_mv) + "→" + milli(mv) + "V";
    var out = milli(mv) + "V", ma = (amps !== false || isFullCurrent(s)) ? ampsMa(s) : 0;
    return out + (ma ? " " + milli(ma) + "A" : "");
  }
  // An APDO: its voltage window and current. SPR PPS (100 mV / 50 mA), EPR AVS (PDP in watts), PD 3.2 SPR AVS (9 V up).
  function decodeApdo(raw) {
    raw = raw >>> 0;
    if (!raw || raw >>> 30 !== 3) return null;
    var sub = (raw >>> 28) & 3, lo, hi, ma;
    if (sub === 0) { lo = ((raw >>> 8) & 0xFF) * 100; hi = ((raw >>> 17) & 0xFF) * 100; ma = (raw & 0x7F) * 50; }
    else if (sub === 1) {
      hi = ((raw >>> 17) & 0x1FF) * 100; lo = ((raw >>> 8) & 0xFF) * 100;
      ma = hi ? Math.floor((raw & 0xFF) * 1000 * 1000 / hi) : 0;
    } else if (sub === 2) {
      var v20 = raw & 0x3FF;
      lo = 9000; hi = v20 ? 20000 : 15000; ma = (v20 ? v20 : (raw >>> 10) & 0x3FF) * 10;
    } else return null;
    return { lo: lo, hi: hi, ma: ma };
  }
  function rangeText(lo, hi, ma, mw) {
    var out = milli(lo) + "-" + milli(hi) + "V";
    return out + (ma ? " " + milli(ma) + "A" : mw ? " " + Math.floor(mw / 1000) + "W" : "");
  }
  function pdoRange(raw) {
    var a = decodeApdo(raw);
    return a ? rangeText(a.lo, a.hi, a.ma) : "";
  }
  function label(s, prev) {
    if (isSweep(s)) {
      var span = pdoRange(s.raw_pdo || 0).split(" ")[0];
      return protocolWord(s) + (span ? " " + span : "");
    }
    var p = params(s, prev);
    return protocolWord(s) + (p ? " " + p : "");
  }
  // UFCS: the mode's top current in the PDO's high field ("9V 3A"), "9V" before the list was read.
  function ufcsLevel(s) {
    var p = params(s), pdo = (s.raw_pdo || 0) >>> 0;
    var ma = s.protocol === "ufcs" && pdo ? ((pdo >>> 25) & 127) * 50 : 0;
    return p && ma ? p + " " + milli(ma) + "A" : p;
  }

  // ---- steps and tallies ---------------------------------------------------------------------------------------------
  function algOf(r) {
    if (r.algorithm) return r.algorithm;
    var p = r.protocol || "";   // older firmware: no algorithm on the wire
    return p === "pd-fixed" ? "pd3x-fixed" : p === "pd-pps" ? "pps" : p === "pd-avs" ? "spr-avs" : p.replace("-5v-handshake", "");
  }
  function norm(r, i) {
    return { i: r.index !== undefined ? r.index : i, alg: algOf(r), mv: r.target_mv || 0, pdo: (r.raw_pdo || 0) >>> 0, pos: r.pos || 0,
             result: r.result, reason: r.reason || "none", detail: r.detail || 0, hunt: isHunt(r), sweep: isSweep(r), raw: r };
  }
  function count(t, s) {
    if (s.result === "pass") { if (s.reason === "output folds back") t.capped++; else t.pass++; }   // a voltage fold is a caution: counted with the capped rows, never a pass
    else if (s.result === "fail") { if (s.reason === "capped below target") t.capped++; else t.fail++; }
    else if (s.result === "skip") t.skip++;
    else t.pending++;
  }
  function tally() { return { pass: 0, fail: 0, capped: 0, skip: 0, pending: 0 }; }
  function rollUp(t) {
    if (t.pending) return "pending";
    if (!t.pass && !t.fail && !t.capped) return t.skip ? "skip" : "pending";
    if (!t.fail && !t.capped && !t.skip) return "pass";
    return (t.pass || t.capped) ? "partial" : "fail";
  }
  function toneOf(s) { var t = tally(); count(t, s); return rollUp(t); }
  // Why a skipped step did not run, in a word that sits next to its cell (only a skip has one).
  function skipTag(s) {
    if (s.result !== "skip") return "";
    switch (s.reason) {
      case "over voltage cap": return s.detail === 144 ? ">36V" : s.detail === 145 ? ">48V" : ">cap";
      case "physical cable limit": return "cable";
      case "EPR entry refused": return "refused";
      case "no contract": return "no PD";
      case "Class A only (12 V max)": return "Class A";
      case "not unlocked": return s.detail === 157 ? "cable" : "locked";
      case "needs load": return "needs load";
      default: return "";
    }
  }
  function sectionOf(key) {
    if (key.probe) return SEC_XIAOMI;
    switch (key.alg) {
      case "fcp": case "scp": return SEC_HUAWEI;
      case "afc": return SEC_SAMSUNG;
      case "vooc": case "svooc": case "svooc65": case "vivo": return SEC_OPPO;
      default: return has(PD_GROUP, key.alg) || REVISIONS[key.alg] ? SEC_PD : SEC_QC;
    }
  }

  // ---- layout --------------------------------------------------------------------------------------------------------
  // rows: the wire rows (k2proto decodeChargerTest steps), in plan order. opts:
  //   fold        the device's finished-grid folding: silent legacy units into one "No reply" row, skipped ones into "Skipped"
  //   absentCell  a legacy unit that never answered (and is not folded) is one "no reply" cell instead of its failed chips
  //   live, current   the running dot goes on the cell of step `current` (a step index) while live
  //   stopped     a step that never ran reads stopped (no fill, dim), not pending
  //   monitorMv   the meter's ceiling: a deep sweep's range stops there
  // Returns lines: [{label, section, cells: [{verdict, label, current, title}]}], the K2Kit row spec.
  function layout(wire, opts) {
    opts = opts || {};
    var steps = (wire || []).map(norm);
    var of = function (a) { return steps.filter(function (s) { return s.alg === a; }); };
    var isPd = function (a) { return has(PD_GROUP, a) || !!REVISIONS[a]; };
    var isRange = function (a) { return has(RANGE_ALGS, a); };
    function isAbsent(a) {
      var xs = of(a);
      if (!xs.length || xs.some(function (s) { return s.result === "pass"; })) return false;
      if (xs.every(function (s) { return s.result === "skip" && s.reason === "not advertised"; })) return true;
      if (isPd(a)) return false;
      return xs[0].result === "fail";
    }
    var skippedOnly = function (a) { var xs = of(a); return xs.length > 0 && xs.every(function (s) { return s.result === "skip"; }); };
    var classAOnly = function (a) { return of(a).some(function (s) { return s.result === "skip" && s.reason === "Class A only (12 V max)"; }); };
    var noQc3Plus = function (a) { return of(a).some(function (s) { return s.result === "skip" && s.reason === "no QC3+ response"; }); };

    // keys: one per row of the plan, in plan order
    var keys = [];
    steps.forEach(function (s) {
      if (REVISIONS[s.alg]) return;
      var k = s.hunt ? { alg: "", probe: true, sweep: false, mv: s.mv }
                     : { alg: s.alg, probe: false, sweep: s.sweep, mv: isRange(s.alg) ? s.pos : 0 };
      if (!keys.some(function (o) { return o.alg === k.alg && o.probe === k.probe && o.sweep === k.sweep && o.mv === k.mv; })) keys.push(k);
    });

    var rows = [];
    function begin(text, key) {
      var r = { label: text, sec: key ? sectionOf(key) : null, chips: [], alg: key ? key.alg : "", probe: !!(key && key.probe),
                sweep: !!(key && key.sweep), mv: key ? key.mv : 0, pos: 0 };
      rows.push(r);
      return r;
    }
    function chip(row, text, tone, st, mv, extra) {
      var c = { text: text, tone: tone, word: false, mv: mv || 0, tag: st && st.length === 1 ? skipTag(st[0]) : "", steps: st || [], note: "" };
      if (extra) for (var k in extra) c[k] = extra[k];
      row.chips.push(c);
      return c;
    }
    function word(row, text, tone) { row.chips.push({ text: text, tone: tone, word: true, mv: 0, tag: "", steps: [], note: "" }); }

    function fixedRow(row, a) {
      steps.forEach(function (s) {
        if (s.alg !== a || !s.mv || s.hunt) return;
        chip(row, params(s.raw), toneOf(s), [s], s.mv);
      });
    }
    function rangeRow(row, a, pos) {
      var xs = steps.filter(function (s) { return s.alg === a && s.pos === pos && !s.hunt && !s.sweep; });
      var first = xs.filter(function (s) { return s.pdo; })[0];
      if (!first) return;
      var t = tally();
      xs.forEach(function (s) { count(t, s); });
      var tone = rollUp(t), c = chip(row, pdoRange(first.pdo) || params(first.raw), tone, xs, first.mv);
      if (tone === "skip") c.tag = skipTag(first);
      else {
        var nl = xs.filter(function (s) { return s.result === "skip" && s.reason === "needs load"; })[0];
        c.tag = nl ? "needs load" : "";
      }
    }
    function legacyRow(row, a) {
      var seen = {};
      steps.forEach(function (x) {
        if (x.alg !== a || !x.mv || seen[x.mv]) return;
        seen[x.mv] = true;
        var hsOk = false, detected = false, classA = false, t = tally(), at = [];
        steps.forEach(function (y) {
          if (y.alg !== a || y.mv !== x.mv) return;
          at.push(y);
          classA = classA || (y.result === "skip" && y.reason === "Class A only (12 V max)");
          hsOk = hsOk || (y.result === "pass" && y.detail === 14);
          detected = detected || (y.result === "pass" && y.detail === 15);
          count(t, y);
        });
        var tone = rollUp(t), failed = t.fail !== 0, c;
        if (hsOk && !failed) { c = chip(row, "handshake ok", tone, at, x.mv); c.word = true; }
        else if (detected && !failed) { c = chip(row, "detected", tone, at, x.mv); c.word = true; }
        else c = chip(row, a === "ufcs" ? ufcsLevel(x.raw) : params(x.raw), tone, at, x.mv);
        if (classA && !t.pass && !failed) c.note = "Class A";
      });
    }
    // The deep sweep's range: the APDO's window, capped at the meter and on the step grid (20 mV), the APDO's own when that leaves nothing.
    function sweepRange(a, pos) {
      var st = steps.filter(function (s) { return s.alg === a && s.pos === pos && s.sweep; })[0];
      var pdo = (st && st.pdo) || (steps.filter(function (s) { return s.alg === a && s.pos === pos && s.pdo; })[0] || {}).pdo;
      var apdo = decodeApdo(pdo || 0);
      return { st: st, text: apdo ? rangeText(sweepLo(apdo), sweepHi(apdo), apdo.ma) : "sweep" };
    }
    function sweepLo(apdo) { var lo = Math.ceil(apdo.lo / 20) * 20, hi = sweepHi(apdo); return hi < lo ? apdo.lo : lo; }
    function sweepHi(apdo) {
      var hi = apdo.hi, cap = opts.monitorMv || 0;
      if (cap && hi > cap) hi = cap;
      hi = Math.floor(hi / 20) * 20;
      return hi < Math.ceil(apdo.lo / 20) * 20 ? apdo.hi : hi;
    }

    var absent = {}, skipped = {}, rangeNone = {};
    keys.forEach(function (key) {
      var row, a = key.alg;
      if (key.probe) {
        row = begin("Mi 6A", key);
        steps.forEach(function (s) { if (s.hunt && s.mv === key.mv) chip(row, params(s.raw), toneOf(s), [s], s.mv); });
        return;
      }
      if (key.sweep) {
        row = begin("Sweep", key); row.pos = key.mv;
        var sw = sweepRange(a, key.mv), st = sw.st, tone = !st ? "pending" : st.result === "skip" ? "skip" :
                  st.result === "fail" ? "fail" : st.result === "pass" ? "pass" : "pending";
        chip(row, sw.text, tone, st ? [st] : [], st ? st.mv : 0);
        if (st && tone === "skip") row.chips[0].tag = skipTag(st);
        return;
      }
      var legacy = !isPd(a);
      if (isAbsent(a)) {
        if (isRange(a)) {
          if (rangeNone[a]) return;
          rangeNone[a] = true;
          word(begin(NAMES[a] || a, key), "None", "dim");
          return;
        }
        if (opts.fold || !legacy) { absent[a] = true; return; }
        if (opts.absentCell) { row = begin(NAMES[a] || a, key); word(row, "no reply", "dim"); row.chips[0].steps = of(a); return; }
      }
      if (noQc3Plus(a)) { word(begin(NAMES[a] || a, key), "no QC3+", "dim"); return; }
      if (opts.fold && legacy && skippedOnly(a) && !classAOnly(a)) { skipped[a] = true; return; }
      row = begin(NAMES[a] || a, key);
      if (isRange(a)) { row.pos = key.mv; rangeRow(row, a, key.mv); }
      else if (isPd(a)) fixedRow(row, a);
      else legacyRow(row, a);
      if (!row.chips.length && (skippedOnly(a) || isRange(a))) {
        rows.pop();
        if (skippedOnly(a)) skipped[a] = true;
      }
    });
    function collapsed(set, text, tone) {
      var any = keys.some(function (k) { return !k.probe && set[k.alg]; });
      if (!any) return;
      var row = begin(text, null), said = {};
      keys.forEach(function (k) { if (!k.probe && set[k.alg] && !said[k.alg]) { said[k.alg] = true; word(row, NAMES[k.alg] || k.alg, tone); } });
    }
    collapsed(absent, "No reply", "dim");
    collapsed(skipped, "Skipped", "caution");

    // A lone cell that never ran says why in words (never a bare strike through "5V"); a unit that never answered is dim, not red.
    rows.forEach(function (r) {
      if (r.chips.length !== 1 || r.chips[0].word || r.chips[0].tone !== "skip") return;
      var c = r.chips[0], why = c.tag, st = c.steps[0] || {};
      if (!why) why = st.reason === "not advertised" ? "no reply" : st.reason === "PD active: use QC port" ? "PD active" :
                      st.reason === "not implemented" ? "n/a" : "skipped";
      c.text = why; c.word = true; c.tone = "dim";
    });

    // The step under test lights one pending cell: the one of its level in the row that owns the step.
    var dot = null;
    if (opts.live && opts.current !== undefined && opts.current !== null) {
      var cur = steps.filter(function (s) { return s.i === opts.current; })[0];
      if (cur && (cur.result === "running" || cur.result === "pending")) {
        for (var ri = 0; ri < rows.length && !dot; ri++) {
          var r = rows[ri], owns = r.probe ? cur.hunt && cur.mv === r.mv :
                                   r.sweep ? cur.sweep && cur.pos === r.pos && cur.alg === r.alg :
                                   !!r.alg && !cur.hunt && !cur.sweep && r.alg === cur.alg && (!isRange(r.alg) || cur.pos === r.pos);
          if (!owns) continue;
          var pend = r.chips.filter(function (c) { return !c.word && c.tone === "pending"; });
          var pick = pend.filter(function (c) { return c.mv === cur.mv; })[0] || pend[0];
          if (pick) dot = pick;
        }
      }
    }

    function verdictOf(c) {
      if (c === dot) return "running";
      if (c.tone === "dim" || c.tone === "caution") return "none";
      if (c.tone === "pending" && opts.stopped) return "stopped";
      return c.tone;
    }
    var WORD = { pass: "pass", fail: "fail", skip: "skipped", pending: "pending", running: "running", stopped: "stopped", partial: "partial", none: "" };
    function whyOf(c) {
      var f = c.steps.filter(function (s) { return s.result === "fail"; })[0] || c.steps.filter(function (s) { return s.result === "skip"; })[0]
              || c.steps.filter(function (s) { return s.result === "pass" && s.reason === "output folds back"; })[0];   // a caution says why too
      if (!f) return "";
      var dr = f.raw.detail_reason;
      var w = f.result === "fail" && dr && dr !== "none" ? dr : f.reason !== "none" ? f.reason : "";
      return c.note ? (w ? w + ", " : "") + c.note : w;
    }
    function cellOf(c) {
      var v = verdictOf(c), lab = c.text, cur = "";
      if (!c.word) { var sp = lab.indexOf(" "); if (sp >= 0) { cur = lab.slice(sp + 1); lab = lab.slice(0, sp); } }
      var w = whyOf(c), vw = WORD[v];
      return { verdict: v, label: lab, current: cur, title: c.text + (vw ? ": " + vw : "") + (w ? " (" + w + ")" : "") };
    }
    // The cell width rule: a chip counts its volt text, a word its whole text; a line breaks at 5 cells or where they stop fitting.
    function cellWidth(c) {
      var n = c.word ? c.text.length : (c.text.indexOf(" ") >= 0 ? c.text.indexOf(" ") : c.text.length);
      return n * ADV + 2 * PAD;
    }

    var lines = [], prevSec = null, first = true, lastSec = null;
    rows.forEach(function (r) {
      var sec = r.sec !== null ? r.sec : lastSec;    // a collapsed row sits in the section of the row before it
      var brk = !first && sec !== prevSec;
      var cur = [], widest = 0;
      var head = true, pendingLines = [];
      r.chips.forEach(function (c) {
        var gw = cellWidth(c);
        var full = (cur.length + 1) * (Math.max(widest, gw) + GAP) - GAP > AVAIL;
        if (cur.length && (full || cur.length >= MAX_CELLS)) {
          pendingLines.push({ head: head, cells: cur });
          head = false; cur = []; widest = 0;
        }
        cur.push(c); widest = Math.max(widest, gw);
      });
      pendingLines.push({ head: head, cells: cur });
      pendingLines.forEach(function (pl) {
        lines.push({ label: pl.head ? r.label : "", section: pl.head && brk, cells: pl.cells.map(cellOf) });
      });
      if (r.sec !== null) lastSec = r.sec;
      prevSec = sec; first = false;
    });
    return lines;
  }

  return { milli: milli, isHunt: isHunt, isSweep: isSweep, isFullCurrent: isFullCurrent, protocolWord: protocolWord, pdoMa: pdoMa,
           ampsMa: ampsMa, params: params, label: label, stepLabel: label, pdoRange: pdoRange, rangeText: rangeText,
           ufcsLevel: ufcsLevel, skipTag: skipTag, layout: layout, NAMES: NAMES };
})();
if (typeof module !== "undefined" && module.exports) module.exports = CtGrid;
