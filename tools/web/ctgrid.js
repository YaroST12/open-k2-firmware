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
  var PD_FIXED = ["pd2-fixed", "pd3x-fixed", "epr"];                   // the fixed levels that share the PD block's first run
  var KIND_WORD = { pps: "PPS", "spr-avs": "AVS", "epr-avs": "AVS" };   // a range chip's word (result_layout kind_word), drawn before its volts
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
  // The wire's Xiaomi flag ("mi": the chain row and the modes it unlocked) decides; an older firmware's row (no key) that asks more
  // than the cable ceiling was its 6 A probe.
  function isHunt(s) { return s.mi !== undefined ? !!s.mi : has(PD_FAMILY, s.protocol) && (s.request_ma || 0) > HUNT_MA; }
  // A Xiaomi mode is named by its power level: volts asked times current asked, whole watts ("Mi 55W"; step_label.hpp mi_watts).
  function miWatts(s) { return isHunt(s) ? Math.floor(((s.target_mv || 0) * (s.request_ma || 0) + 500000) / 1000000) : 0; }
  function isSweep(s) { return has(PROGRAMMABLE, s.protocol) && (s.detail === SWEEP_DETAIL || s.sweep === true); }
  function isFullCurrent(s) { return s.protocol === "pps" && (s.request_ma || 0) > PLAN_ROW_MA && s.request_ma <= HUNT_MA && !isHunt(s); }
  function isAvs(s) { return s.protocol === "spr-avs" || s.protocol === "epr-avs" || (s.protocol === "epr-fixed" && !!s.avs); }

  function protocolWord(s) {
    var p = s.protocol, hasV = !!s.target_mv;
    if (isHunt(s)) return hasV ? "Mi" : "Xiaomi";
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
    if (isHunt(s)) return 0;   // a Xiaomi mode: its watts (miWatts)
    if (isFullCurrent(s)) return s.request_ma || 0;
    if (s.protocol === "pd2-fixed" || s.protocol === "pd3x-fixed" || s.protocol === "pps" || s.protocol === "epr-fixed")
      return isAvs(s) ? 0 : pdoMa(s.raw_pdo || 0);
    return s.protocol === "scp" ? (s.request_ma || 0) : 0;
  }
  // ---- OEM levels by their watts (include/k2/chargertest/oem_watts.hpp, tools/oem_watts.py; golden: tests/fixtures/oem_watts.txt) ----
  // The K2 draws no current in a charger test: a level's watts are its volts x the current the protocol names (the SCP limit the K2
  // writes, the UFCS mode's advertised current, the SuperVOOC ask the adapter went on under), so they are only ever claimed.
  var OEM_LEVELS = [
    { word: "VOOC", mv: 5000, ma: 4000, w: 20, ceiling: false },
    { word: "SVOOC", mv: 10000, ma: 5000, w: 50, ceiling: true },
    { word: "SVOOC", mv: 10000, ma: 6500, w: 65, ceiling: true },
    { word: "SVOOC", mv: 10000, ma: 10000, w: 100, ceiling: true },
    { word: "SVOOC", mv: 11000, ma: 11000, w: 120, ceiling: true }
  ];
  var VOOC_ALGS = ["vooc", "svooc", "svooc65"];   // one shared detection: the finished grid reads them as the levels above
  var RAN_ITS_COURSE = { judged: 1, stopped: 1, cap: 1, "time cap": 1 };   // svooc_ceiling::End names (readout, card)
  // "120W", "22.5W" (0.1 W, the tenth only when not 0; whole watts from 100 W, so at most 5 characters); "" for 0 mW.
  function oemWatts(mw) {
    if (!mw) return "";
    var dw = Math.floor((mw + 50) / 100);
    if (dw >= 1000) return Math.floor((mw + 500) / 1000) + "W";
    return Math.floor(dw / 10) + (dw % 10 ? "." + (dw % 10) : "") + "W";
  }
  function oemLabel(word, mw) { var w = oemWatts(mw); return w ? word + " " + w : word; }
  function claimW(vmaxMv, word) { return Math.floor((vmaxMv * Math.min(word, 0x1FF) * 100 + 500000) / 1000000); }
  // The ceiling readout's ask word: k2proto decodeChargerSvoocCap (word), k2_remote ({ceiling: {word}}), a card's Deep block (ceiling_ma).
  function svoocWord(sv) {
    if (!sv) return 0;
    if (sv.word !== undefined && sv.word !== null) return sv.word;
    if (sv.ceiling && sv.ceiling.word !== undefined) return sv.ceiling.word;
    return Math.floor((sv.ceiling_ma || 0) / 100);
  }
  // "claimed" / "not reached" / "not tested": what the Deep ceiling stage proves of one level (oem_watts::claim).
  function oemClaim(level, sv) {
    if (!level.ceiling) return "not tested";
    var word = svoocWord(sv), vmax = (sv && sv.vmax_mv) || 0;
    if (word && vmax && claimW(vmax, word) >= level.w) return "claimed";
    return sv && RAN_ITS_COURSE[sv.end] ? "not reached" : "not tested";
  }
  function voocLevels(sv) {
    return OEM_LEVELS.map(function (l) { return { word: l.word, w: l.w, label: oemLabel(l.word, l.w * 1000), state: oemClaim(l, sv) }; });
  }
  // A step's own watts: SCP the limit the K2 writes, UFCS its mode's advertised current; 0 for any other step.
  function stepMw(s) {
    var mv = s.target_mv || 0, pdo = (s.raw_pdo || 0) >>> 0;
    var ma = s.protocol === "scp" ? (s.request_ma || 0) : s.protocol === "ufcs" && pdo ? ((pdo >>> 25) & 127) * 50 : 0;
    return ma && mv ? Math.floor(mv * ma / 1000) : 0;
  }
  // "10V 22.5W", "9V 27W": the step's volts and its claimed watts; "" when stepMw() is 0.
  function oemChip(s) { var mw = stepMw(s); return mw ? milli(s.target_mv) + "V " + oemWatts(mw) : ""; }

  // "20V 5A", "12V", "11→8V" ("" with no voltage). prev = the row before: a continuous row of the same protocol and slot reads from→to.
  function params(s, prev, amps) {
    var mv = s.target_mv || 0;
    if (!mv) return "";
    if (s.continuous && prev && has(PROGRAMMABLE, s.protocol) && prev.protocol === s.protocol && prev.pos === s.pos &&
        !isHunt(prev) && !isHunt(s) && prev.target_mv && prev.target_mv !== mv)
      return milli(prev.target_mv) + "→" + milli(mv) + "V";
    if (isHunt(s)) return miWatts(s) + "W";
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
  // A pass the run marked a caution (result_layout caution): a voltage fold, a step walk the source did not follow at its quantum
  // (the detail is the measured move in mV, 0 = ignored) or a transition that broke a spec limit (the detail is progcheck's flag bits).
  var CAUTIONS = { "output folds back": 1, "step resolution": 1, "transition check": 1 };
  var PROG_FLAGS = ["overshoot", "slow rise", "transient", "level off"];
  function isCaution(s) { return s.result === "pass" && CAUTIONS[s.reason] === 1; }
  // The caution's words, as charger_test::caution_text says them on the device.
  function cautionText(s) {
    if (s.reason === "step resolution") {
      if (!s.detail) return "steps ignored";
      return "steps " + (s.detail >= 50 ? Math.round(s.detail / 10) * 10 : s.detail) + "mV, not " + (s.alg === "pps" ? 20 : 100) + "mV";
    }
    if (s.reason === "transition check") {
      var f = PROG_FLAGS.filter(function (_, i) { return (s.detail >> i) & 1; });   // progcheck::Flag bits, lowest first
      return (f[0] || s.reason) + (f.length > 1 ? " +" + (f.length - 1) : "");
    }
    return s.reason;
  }
  function count(t, s) {
    if (s.result === "pass") { if (isCaution(s)) t.capped++; else t.pass++; }   // a caution is counted with the capped rows, never a pass
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
  //   spec        the Deep run's PD spec section (k2proto decodeChargerSpecSection): its checks follow the unit lines
  //   oemWatts    the finished grid's VOOC family: two level rows ("VOOC" detection + 20W, "SVOOC" 50W..120W), each level claimed
  //               ("unverified" look), not reached ("none") or not tested (struck); svooc = the ceiling readout (decodeChargerSvoocCap)
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
      return xs[0].result === "fail";
    }
    // The source offered no PD at all: every PD step is "not advertised" (a block with no chip that is not this was skipped).
    function pdAbsent() {
      return steps.every(function (s) { return s.hunt || s.sweep || !isPd(s.alg) || REVISIONS[s.alg] || (s.result === "skip" && s.reason === "not advertised"); });
    }
    var skippedOnly = function (a) { var xs = of(a); return xs.length > 0 && xs.every(function (s) { return s.result === "skip"; }); };
    var classAOnly = function (a) { return of(a).some(function (s) { return s.result === "skip" && s.reason === "Class A only (12 V max)"; }); };
    var noQc3Plus = function (a) { return of(a).some(function (s) { return s.result === "skip" && s.reason === "no QC3+ response"; }); };

    // keys: one per row of the plan, in plan order. Every PD step (fixed, PPS, AVS) shares the one PD block key; a deep sweep keeps its own.
    var keys = [];
    steps.forEach(function (s) {
      if (REVISIONS[s.alg]) return;
      var pd = !s.hunt && !s.sweep && isPd(s.alg);
      var k = s.hunt ? { alg: "", probe: true, sweep: false, mv: 0, pd: false }   // the Xiaomi unit: one row
            : pd ? { alg: s.alg, probe: false, sweep: false, mv: 0, pd: true }
            : { alg: s.alg, probe: false, sweep: s.sweep, mv: isRange(s.alg) ? s.pos : 0, pd: false };
      if (!keys.some(function (o) { return pd ? o.pd : (!o.pd && o.alg === k.alg && o.probe === k.probe && o.sweep === k.sweep && o.mv === k.mv); })) keys.push(k);
    });

    var rows = [];
    function begin(text, key) {
      var r = { label: text, sec: key ? sectionOf(key) : null, chips: [], alg: key ? key.alg : "", probe: !!(key && key.probe),
                sweep: !!(key && key.sweep), pd: !!(key && key.pd), mv: key ? key.mv : 0, pos: 0 };
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

    // The fixed levels of every PD algorithm, ascending (volts, then plan order). A skipped EPR level that is no listing (cable, refused,
    // over the cap) has no chip under the finished grid's fold: its reason lives on the block's steps.
    function fixedRun(row) {
      var done = -1;
      for (;;) {
        var next = null, best = Infinity;
        steps.forEach(function (s, i) {
          if (!has(PD_FIXED, s.alg) || !s.mv || s.hunt) return;
          if (opts.fold && s.alg === "epr" && s.result === "skip" && s.reason !== "advertised" && s.reason !== "light run: not asked") return;
          var key = s.mv * 1024 + i;
          if (key > done && key < best) { best = key; next = s; }
        });
        if (!next) return;
        done = best;
        chip(row, params(next.raw), toneOf(next), [next], next.mv);
      }
    }
    // One chip for one APDO: its advertised window, the kind word drawn before the volts (kind), mv = its PDO slot.
    function rangeChip(row, a, pos) {
      var xs = steps.filter(function (s) { return s.alg === a && s.pos === pos && !s.hunt && !s.sweep; });
      var first = xs.filter(function (s) { return s.pdo; })[0];
      if (!first) return;
      var t = tally();
      xs.forEach(function (s) { count(t, s); });
      var tone = rollUp(t);
      if (opts.fold && a === "epr-avs" && tone === "skip") return;   // an EPR AVS range that was only skipped: no chip, like the EPR levels
      var c = chip(row, pdoRange(first.pdo) || params(first.raw), tone, xs, pos, { kind: KIND_WORD[a], alg: a });
      if (tone === "skip") c.tag = skipTag(first);
      else {
        var nl = xs.filter(function (s) { return s.result === "skip" && s.reason === "needs load"; })[0];
        c.tag = nl ? "needs load" : "";
      }
    }
    // The PD block: the fixed levels, then the PPS ranges, the SPR AVS ranges and the EPR AVS ranges, one chip per APDO in plan order.
    // A kind the source never offered has no chip (its steps carry no PDO).
    function pdRow(row) {
      fixedRun(row);
      RANGE_ALGS.forEach(function (a) {
        var seen = {};
        steps.forEach(function (s) {
          if (s.alg !== a || !s.pdo || s.hunt || s.sweep || seen[s.pos]) return;
          seen[s.pos] = true;
          rangeChip(row, a, s.pos);
        });
      });
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
        else c = chip(row, oemChip(x.raw) || (a === "ufcs" ? ufcsLevel(x.raw) : params(x.raw)), tone, at, x.mv);   // SCP / UFCS: watts
        if (classA && !t.pass && !failed) c.note = "Class A";
      });
    }
    // The deep sweep's range: the APDO's window, capped at the meter and on the step grid (20 mV), the APDO's own when that leaves nothing.
    function sweepRange(a, pos) {
      var st = steps.filter(function (s) { return s.alg === a && s.pos === pos && s.sweep; })[0];
      var pdo = (st && st.pdo) || (steps.filter(function (s) { return s.alg === a && s.pos === pos && s.pdo; })[0] || {}).pdo;
      var apdo = decodeApdo(pdo || 0);
      var parts = steps.filter(function (s) { return s.alg === a && s.pos === pos && s.sweep; });
      return { st: st, parts: parts, text: apdo ? rangeText(sweepLo(apdo), sweepHi(apdo), apdo.ma) : "sweep" };
    }
    function sweepLo(apdo) { var lo = Math.ceil(apdo.lo / 20) * 20, hi = sweepHi(apdo); return hi < lo ? apdo.lo : lo; }
    function sweepHi(apdo) {
      var hi = apdo.hi, cap = opts.monitorMv || 0;
      if (cap && hi > cap) hi = cap;
      hi = Math.floor(hi / 20) * 20;
      return hi < Math.ceil(apdo.lo / 20) * 20 ? apdo.hi : hi;
    }

    // The VOOC family as level rows (opts.oemWatts): a family that never answered folds as one word, never three versions.
    var voocDone = false, LEVEL_TONE = { claimed: "unverified", "not reached": "dim", "not tested": "skip" };
    function voocRows(key, a) {
      var fam = steps.filter(function (s) { return has(VOOC_ALGS, s.alg); });
      var pass = fam.some(function (s) { return s.result === "pass"; });
      var gone = VOOC_ALGS.some(function (x) { return of(x).length && isAbsent(x); });
      var skipOnly = VOOC_ALGS.every(function (x) { return !of(x).length || skippedOnly(x); });
      if (!pass && opts.fold && (gone || skipOnly)) { (gone ? absent : skipped)[a] = true; return; }
      var levels = voocLevels(opts.svooc), row = null;
      levels.forEach(function (l, i) {
        if (!row || levels[i - 1].word !== l.word) {
          row = begin(l.word, key); row.levels = true;
          if (i === 0) legacyRow(row, a);   // the shared detection leads the family's first row
        }
        chip(row, oemWatts(l.w * 1000), LEVEL_TONE[l.state], [], 0, { levelTitle: l.label + ": " + l.state + (l.state === "claimed" ? " (no load)" : "") });
      });
    }

    var absent = {}, skipped = {};
    keys.forEach(function (key) {
      var row, a = key.alg;
      if (opts.oemWatts && has(VOOC_ALGS, a)) {   // the family's rows stand where the plan first runs one of its units
        if (!voocDone) voocRows(key, a);
        voocDone = true;
        return;
      }
      if (key.probe) {
        // The Xiaomi row (result_layout probe_row): one chip per mode the chain unlocked, by its watts (an older firmware's 6 A probes
        // are chips of their own). With none, one "auth" chip: pass (nothing raised), skip (did not run or failed), else as it stands.
        row = begin("Xiaomi", key);
        var modes = steps.filter(function (s) { return s.hunt && s.mv; });
        modes.forEach(function (s) { chip(row, params(s.raw), toneOf(s), [s], s.mv); });
        if (!modes.length) steps.forEach(function (s) {
          if (s.hunt && !s.mv) chip(row, "auth", s.result === "skip" ? "skip" : toneOf(s), [s], 0);
        });
        return;
      }
      if (key.sweep) {
        row = begin("Sweep", key); row.pos = key.mv;
        // A PPS sweep split at 5 V is two steps (from 5 V up, the part below) in ONE row: the chip reads them together, as the firmware
        // sweep_row does. Pending while either part is pending or running, fail when either failed, skip only when every part skipped.
        var sw = sweepRange(a, key.mv), st = sw.st, parts = sw.parts, tone = "pending", skipPart = null;
        var isPending = false, failed = false, passed = false, folded = false;
        parts.forEach(function (s) {
          if (s.result === "pass") { passed = true; if (s.reason === "output folds back") folded = true; }
          else if (s.result === "fail") failed = true;
          else if (s.result === "skip") { if (!skipPart) skipPart = s; }
          else isPending = true;
        });
        // Tone: the verdicts alone. The wire rows carry no per-point error (no worst_err, no track_err), so the web cannot grade a
        // passing sweep by its error as the device does (green to 100 mV, amber above): a pass is green here, and amber only with the fold flag.
        if (isPending) tone = "pending";
        else if (failed) tone = "fail";
        else if (passed) tone = folded ? "partial" : "pass";
        else if (skipPart) tone = "skip";
        chip(row, sw.text, tone, parts, st ? st.mv : 0);
        if (skipPart && tone === "skip") row.chips[0].tag = skipTag(skipPart);
        return;
      }
      if (key.pd) {
        // The PD block (result_layout pd_row): one row labelled "PD", every PD kind in it. Without a chip the whole block folds.
        row = begin("PD", key);
        pdRow(row);
        if (!row.chips.length) { rows.pop(); (pdAbsent() ? absent : skipped)[a] = true; }
        return;
      }
      var legacy = true;
      if (isAbsent(a)) {
        if (opts.fold || !legacy) { absent[a] = true; return; }
        if (opts.absentCell) { row = begin(NAMES[a] || a, key); word(row, "no reply", "dim"); row.chips[0].steps = of(a); return; }
      }
      if (noQc3Plus(a)) { word(begin(NAMES[a] || a, key), "no QC3+", "dim"); return; }
      if (opts.fold && legacy && skippedOnly(a) && !classAOnly(a)) { skipped[a] = true; return; }
      row = begin(NAMES[a] || a, key);
      legacyRow(row, a);
      if (!row.chips.length && skippedOnly(a)) { rows.pop(); skipped[a] = true; }
    });
    function collapsed(set, text, tone) {
      var any = keys.some(function (k) { return !k.probe && set[k.alg]; });
      if (!any) return;
      var row = begin(text, null), said = {};
      keys.forEach(function (k) {
        if (k.probe || !set[k.alg]) return;
        var name = k.pd ? "PD" : has(VOOC_ALGS, k.alg) ? "VOOC" : NAMES[k.alg] || k.alg;   // the VOOC family folds as one word, PD as one
        if (!said[name]) { said[name] = true; word(row, name, tone); }
      });
    }
    collapsed(absent, "No reply", "dim");
    collapsed(skipped, "Skipped", "caution");

    // A lone cell that never ran says why in words (never a bare strike through "5V"); a unit that never answered is dim, not red.
    rows.forEach(function (r) {
      if (r.chips.length !== 1 || r.chips[0].word || r.chips[0].tone !== "skip" || (r.probe && r.chips[0].text === "auth")) return;   // the Xiaomi auth chip stays struck
      var c = r.chips[0], why = c.tag, st = c.steps[0] || {};
      if (!why) why = st.reason === "not advertised" ? "no reply" : st.reason === "PD active: use QC port" ? "PD active" :
                      st.reason === "not implemented" ? "n/a" : "skipped";
      c.text = (c.kind ? c.kind + " " : "") + why; c.word = true; c.tone = "dim";   // a range chip keeps its kind word: the row is labelled "PD"
    });

    // The step under test lights one pending cell: the one of its level in the row that owns the step.
    var dot = null;
    if (opts.live && opts.current !== undefined && opts.current !== null) {
      var cur = steps.filter(function (s) { return s.i === opts.current; })[0];
      if (cur && (cur.result === "running" || cur.result === "pending")) {
        for (var ri = 0; ri < rows.length && !dot; ri++) {
          var r = rows[ri], owns = r.probe ? cur.hunt :
                                   r.sweep ? cur.sweep && cur.pos === r.pos && cur.alg === r.alg :
                                   r.pd ? !cur.hunt && !cur.sweep && isPd(cur.alg) && !REVISIONS[cur.alg] :
                                   !!r.alg && !cur.hunt && !cur.sweep && r.alg === cur.alg;
          if (!owns) continue;
          var pend = r.chips.filter(function (c) { return !c.word && c.tone === "pending"; });
          var pick = pend.filter(function (c) { return r.pd && isRange(cur.alg) ? c.alg === cur.alg && c.mv === cur.pos : !c.alg && c.mv === cur.mv; })[0] || pend[0];
          if (pick) dot = pick;
        }
      }
    }

    function verdictOf(c) {
      if (c === dot) return "running";
      if (c.tone === "unverified") return "unverified";   // a claimed level: a number with no pass or fail look
      if (c.tone === "dim" || c.tone === "caution") return "none";
      if (c.tone === "pending" && opts.stopped) return "stopped";
      return c.tone;
    }
    var WORD = { pass: "pass", fail: "fail", skip: "skipped", pending: "pending", running: "running", stopped: "stopped", partial: "partial", none: "" };
    function whyOf(c) {
      var f = c.steps.filter(function (s) { return s.result === "fail"; })[0] || c.steps.filter(function (s) { return s.result === "skip"; })[0]
              || c.steps.filter(isCaution)[0];   // a caution says why too
      if (!f) return "";
      var dr = f.raw.detail_reason;
      var w = isCaution(f) ? cautionText(f) : f.result === "fail" && dr && dr !== "none" ? dr : f.reason !== "none" ? f.reason : "";
      return c.note ? (w ? w + ", " : "") + c.note : w;
    }
    function cellOf(c) {
      var v = verdictOf(c), lab = c.text, cur = "", kind = c.kind && !c.word ? c.kind + " " : "";   // a range chip's kind word leads its volts
      if (!c.word) { var sp = lab.indexOf(" "); if (sp >= 0) { cur = lab.slice(sp + 1); lab = lab.slice(0, sp); } }
      var w = whyOf(c), vw = WORD[v];
      return { verdict: v, label: kind + lab, current: cur, title: c.levelTitle || kind + c.text + (vw ? ": " + vw : "") + (w ? " (" + w + ")" : "") };
    }
    // The cell width rule: a chip counts its volt text, a word its whole text; a line breaks at 5 cells or where they stop fitting.
    function cellWidth(c) {
      var n = c.word ? c.text.length : (c.text.indexOf(" ") >= 0 ? c.text.indexOf(" ") : c.text.length);
      if (c.kind && !c.word) n += c.kind.length + 1;
      return n * ADV + 2 * PAD;
    }

    var lines = [], prevSec = null, first = true, lastSec = null;
    rows.forEach(function (r) {
      var sec = r.sec !== null ? r.sec : lastSec;    // a collapsed row sits in the section of the row before it
      var brk = !first && sec !== prevSec;
      var cur = [], widest = 0;
      var head = true, pendingLines = [], solo = false;
      r.chips.forEach(function (c) {
        var gw = cellWidth(c);
        var full = (cur.length + 1) * (Math.max(widest, gw) + GAP) - GAP > AVAIL;
        if (cur.length && (full || cur.length >= MAX_CELLS || c.kind || solo)) {   // a range chip has its line to itself
          pendingLines.push({ head: head, cells: cur });
          head = false; cur = []; widest = 0;
        }
        cur.push(c); widest = Math.max(widest, gw); solo = !!c.kind;
      });
      pendingLines.push({ head: head, cells: cur });
      pendingLines.forEach(function (pl) {
        lines.push({ label: pl.head ? r.label : "", section: pl.head && brk, cells: pl.cells.map(cellOf) });
      });
      if (r.sec !== null) lastSec = r.sec;
      prevSec = sec; first = false;
    });
    return lines.concat(specLines(opts.spec));
  }

  // The Deep run's PD spec section (k2proto decodeChargerSpecSection) as grid lines: one wide cell per check under its short label,
  // a rule above the first row and above each family block (the device's spec rows, src/ui/ui_charger_test.cpp). The cell reads like
  // spectext::cell_view: a skip says why, struck; a pending one is empty; an unverified one (a K2 figure, no spec limit) shows the
  // number with no pass or fail look; a judged one shows the worst value, then ", N fail" when it failed.
  function specLines(sp) {
    if (!sp || !sp.cells || sp.end === "not run") return [];
    return sp.cells.map(function (c) {
      var verdict = "pending", label = "";
      if (c.verdict === "skip") { verdict = "skip"; label = c.skip || "skipped"; }
      else if (c.verdict === "unverified") { verdict = "none"; label = c.value_text; }
      else if (c.verdict === "pass" || c.verdict === "fail") {
        label = c.value_text + (c.fails ? ", " + c.fails + " fail" : "");
        verdict = c.fails ? "fail" : "pass";
      }
      var word = c.verdict === "unverified" ? "unverified" : verdict === "none" ? "" : verdict === "skip" ? "skipped" : verdict;
      // The detail the device kept (limit, count, place; the second wire form): the same three the detail page shows, in the title.
      var kept = c.limit_text ? " · limit " + c.limit_text + " · " + c.count + " measured" + (c.at_text ? " · " + c.at_text : "") : "";
      return { label: c.short || c.label, section: !!c.section,
               cells: [{ verdict: verdict, label: label, current: "", title: c.label + (word ? ": " + word : "") + (label && verdict !== "skip" ? " (" + label + ")" : "") + kept }] };
    });
  }

  // ---- the PD version of a run (tools/pd_version.py, include/k2/chargertest/pd_version.hpp; golden: tests/fixtures/pd_versions.txt) ----
  // The offer proves a version (SPR AVS 3.2, EPR 3.1, PPS 3.0, a 3.x header 3.0, a 2.0 header 2.0); a Revision answer wins when it is as
  // high, and Revision is a 3.1 message itself. An answer under the proof shows the proof marked "*" and the conflict says both.
  function pdVersion(ev) {
    var adv = (ev && ev.advertised) || {}, wire = (ev && ev.wire_revision_code) || 0;
    var code = function (m, n) { return m * 16 + (n & 15); };
    var proof = 0, basis = "";
    if (adv.spr_avs) { proof = code(3, 2); basis = "offers SPR AVS"; }
    else if (adv.epr_table) { proof = code(3, 1); basis = "offers EPR"; }
    else if (adv.pps) proof = code(3, 0);
    else if (wire === 2) proof = code(3, 0);
    else if (wire === 1) proof = code(2, 0);
    var ans = ev && ev.source_rmdo ? ev.source_rmdo.revision.split(".").map(Number) : null;
    if (ans && proof < code(3, 1)) { proof = code(3, 1); basis = ""; }
    if (ans && code(ans[0], ans[1]) >= proof) return { major: ans[0], minor: ans[1], mark: false, label: "PD" + ans[0] + "." + ans[1], conflict: "" };
    if (!proof) return { major: 0, minor: 0, mark: false, label: "", conflict: "" };
    var v = { major: proof >> 4, minor: proof & 15, mark: !!ans };
    v.label = "PD" + v.major + "." + v.minor + (v.mark ? "*" : "");
    v.conflict = ans ? "reports Revision " + ans[0] + "." + ans[1] + ", " + (basis ? basis + " (" + v.major + "." + v.minor + ")" : "Revision is a PD 3.1 message") : "";
    return v;
  }
  // The run's version line: "PD3.0", or "PD3.2* (reports Revision 3.1, offers SPR AVS (3.2))"; empty without PD.
  function pdVersionText(ev) { var v = pdVersion(ev); return v.label + (v.conflict ? " (" + v.conflict + ")" : ""); }

  return { milli: milli, pdVersion: pdVersion, pdVersionText: pdVersionText, isHunt: isHunt, isSweep: isSweep, isFullCurrent: isFullCurrent, protocolWord: protocolWord, pdoMa: pdoMa,
           ampsMa: ampsMa, params: params, label: label, stepLabel: label, pdoRange: pdoRange, rangeText: rangeText,
           ufcsLevel: ufcsLevel, skipTag: skipTag, layout: layout, specLines: specLines, NAMES: NAMES,
           OEM_LEVELS: OEM_LEVELS, oemWatts: oemWatts, oemLabel: oemLabel, oemClaim: oemClaim, voocLevels: voocLevels, stepMw: stepMw,
           oemChip: oemChip };
})();
if (typeof module !== "undefined" && module.exports) module.exports = CtGrid;
