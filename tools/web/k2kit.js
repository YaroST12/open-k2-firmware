// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// K2Kit: the dashboard's share of the K2's own UI language (docs/UI_SYSTEM.md), as small self-contained renderers.
// Each returns an HTML string from plain data and knows no page: the look comes from tools/web/k2kit.css (custom
// properties named after the device tokens, light and dark in one place). Embedded into k2.html by
// tools/web/sync_proto.py; keep it free of a literal script-close tag.
//
//   verdictCellHtml({verdict, label, current?, title?})          one VerdictCell
//   resultGridHtml([{label, section?, cells: [cell...]}], opts?)  the ResultGrid: label column, cells, brackets
//   bracketRuns(cells)                                            the bracket spans under a row's cells
//   kvTableHtml([{label, value, level?}], {footer, tone})         the KvTable (hairline rows, dim key, mono value) and
//                                                                 its tone-filled footer band; level caution|error colours the value
//   rowListHtml(rows, {selected, focus, name})                    the RowList: square markers, one focused row shows its note line
//   actionCellHtml({id, label, state, leftMs, armedLabel, reason}) the ActionCell: idle, armed (amber, draining bar), busy, disabled
//   alertBandHtml({tone, text})                                   the AlertBand (the tone-filled band, role alert for error and caution)
//   topBarHtml({title, mv, ma})                                   the TopBar with the live V, A and W in the unit colours
//
// The interaction logic is pure and takes the clock as an argument (a test needs no timers):
//   armPress / armExpire / armLeftMs / armCancel   the device's arm-then-confirm: the first press arms (5 s window), the second fires
//   footerState({connected, error, armed, leftMs, info})  what the footer band says: not connected, error, armed, info, in that order
//   snapMv / stepMv / clampMa / pdoRows            the PD set point maths and the PDO list model, capped at the K2's 36 V monitor
//   fcOffers / fcNeedsConfirm / fcDetected         what fast charge the page offers: QC at or below 12 V, never a VOOC family trigger
//
// verdict is one of pending running pass fail none skip stopped partial (the device's Verdict enum). Colour never
// carries it alone: pass and fail differ in fill and ink, skip strikes the volts, running adds a dot, none and
// stopped have no fill.
var K2Kit = (function () {
  var VERDICTS = ["pending", "running", "pass", "fail", "none", "skip", "stopped", "partial"];
  function esc(s) {
    return String(s === undefined || s === null ? "" : s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
  }
  function verdictOf(c) { return VERDICTS.indexOf(c.verdict) >= 0 ? c.verdict : "pending"; }
  // One cell. The label is the volts (struck when skipped); the current belongs to the bracket row, not the cell.
  function verdictCellHtml(c, style) {
    var v = verdictOf(c), speak = v === "none" ? "" : " " + v;
    return '<div class="k-vc k-' + v + '" role="img" aria-label="' + esc(c.label + speak) + '"' +
      (c.title ? ' title="' + esc(c.title) + '"' : "") + (style ? ' style="' + style + '"' : "") + ">" +
      '<span class="k-vl">' + esc(c.label) + "</span>" + (v === "running" ? '<i class="k-dot"></i>' : "") + "</div>";
  }
  // One bracket per run of consecutive cells with the same non-empty current: [{from, to, text}] (cell indexes).
  function bracketRuns(cells) {
    var runs = [];
    for (var i = 0; i < cells.length; i++) {
      var cur = cells[i].current;
      if (!cur) continue;
      var last = runs[runs.length - 1];
      if (last && last.to === i - 1 && last.text === cur) last.to = i;
      else runs.push({ from: i, to: i, text: cur });
    }
    return runs;
  }
  // The ResultGrid. Lines: [{label, section, cells}]. A line with a section flag starts under a 2 px rule; a line
  // with no label continues the row above it. opts.cls adds a class to the grid (a page sizes it, the kit does not).
  function resultGridHtml(lines, opts) {
    var out = '<div class="k-rg' + (opts && opts.cls ? " " + esc(opts.cls) : "") + '">';
    lines.forEach(function (ln) {
      var cells = ln.cells || [], runs = bracketRuns(cells);
      out += '<div class="k-rg-row' + (ln.section ? " k-sec" : "") + (runs.length ? " k-two" : "") + '">' +
        '<div class="k-rg-label">' + esc(ln.label || "") + "</div>" +
        '<div class="k-rg-cells" style="--k-n:' + Math.max(cells.length, 1) + '">';
      cells.forEach(function (c, i) { out += verdictCellHtml(c, "grid-column:" + (i + 1)); });
      runs.forEach(function (r) {
        out += '<div class="k-br" style="grid-column:' + (r.from + 1) + " / " + (r.to + 2) + '">' + esc(r.text) + "</div>";
      });
      out += "</div></div>";
    });
    return out + "</div>";
  }
  // The KvTable: one hairline-ruled row per pair, the key dim, the value in the mono face. level is an exception colour only
  // (caution, error); the value also carries the level word (the row's word, else LOW / TIGHT) so colour is never the only cue.
  // The footer is the device's tone band (info, ok, caution, error).
  function kvTableHtml(rows, opts) {
    var out = '<div class="k-kv' + (opts && opts.cls ? " " + esc(opts.cls) : "") + '">';
    rows.forEach(function (r) {
      var lv = r.level === "caution" || r.level === "error" ? r.level : "";
      out += '<div class="k-kv-row' + (lv ? " k-" + lv : "") + '"><span class="k-kv-k">' + esc(r.label) + '</span>' +
        '<span class="k-kv-v">' + esc(r.value) + (lv ? ' <b class="k-kv-w">' + esc(r.word || (lv === "error" ? "LOW" : "TIGHT")) + "</b>" : "") + "</span></div>";
    });
    out += "</div>";
    if (opts && opts.footer) {
      var tone = ["ok", "caution", "error"].indexOf(opts.tone) >= 0 ? opts.tone : "info";
      out += '<div class="k-foot k-foot-' + tone + '">' + esc(opts.footer) + "</div>";
    }
    return out;
  }

  // ---- numbers ------------------------------------------------------------------------------------------------------------
  function fmtV(mv) { return (mv / 1000).toFixed(2); }
  function fmtA(ma) { return (ma / 1000).toFixed(2); }
  function fmtW(mv, ma) { var w = mv * ma / 1e6; return w >= 100 ? String(Math.round(w)) : w.toFixed(1); }

  // ---- arm then confirm ---------------------------------------------------------------------------------------------------
  // One arm at a time, like the device's single confirm window. st is null or {id, at}; every function returns a new state.
  var ARM_MS = 5000;
  function armPress(st, id, now) {
    if (st && st.id === id && now - st.at <= ARM_MS) return { st: null, fire: true };
    return { st: { id: id, at: now }, fire: false };
  }
  function armLeftMs(st, now) { return st ? Math.max(0, ARM_MS - (now - st.at)) : 0; }
  function armExpire(st, now) { return st && now - st.at > ARM_MS ? null : (st || null); }
  function armCancel() { return null; }

  // What the footer band says. A lost link wins, then an error, then an armed action, then plain information.
  function footerState(s) {
    if (!s.connected) return { tone: "caution", text: "Not connected. Connect the K2 first." };
    if (s.error) return { tone: "error", text: s.error };
    if (s.armed) {
      var sec = Math.max(1, Math.ceil((s.leftMs === undefined ? ARM_MS : s.leftMs) / 1000));
      return { tone: "caution", text: "Armed: " + s.armed + ". Press again within " + sec + " s to confirm." };
    }
    return { tone: "info", text: s.info || "Ready." };
  }

  // ---- PD set point -------------------------------------------------------------------------------------------------------
  var CEILING_MV = 36000;   // the K2's VBUS monitor reads up to 36 V; nothing above it is requested
  var SAFE_MV = 5000;
  var STEP_MV = { pps: 20, avs: 100 };   // the RDO resolution of a programmable request
  function snapMv(type, mv, min, max) {
    var hi = Math.min(max, CEILING_MV), step = STEP_MV[type] || 20;
    var v = Math.round(Math.min(Math.max(mv, min), hi) / step) * step;
    if (v > hi) v -= step;
    return Math.max(v, min);
  }
  function stepMv(pdo, cur, dir, coarse) {
    return snapMv(pdo.type, cur + (dir < 0 ? -1 : 1) * (coarse ? 1000 : 100), pdo.min_mv, pdo.max_mv);
  }
  function clampMa(ma, max) {
    if (!(ma > 0)) return max;
    return Math.min(max, Math.max(50, Math.round(ma / 50) * 50));
  }
  function needsConfirm(mv) { return mv > SAFE_MV; }
  // One RowList row per PDO. A programmable PDO is adjustable; a fixed PDO above the monitor ceiling and the PDO kinds the
  // remote does not request are disabled with the reason as the note. request is the default RDO to send for the row.
  function pdoRows(pdos) {
    return pdos.map(function (p, i) {
      var prog = p.type === "pps" || p.type === "avs", fixed = p.type === "fixed";
      var top = Math.min(p.max_mv, CEILING_MV), row = {
        id: String(i + 1), label: String(i + 1), type: p.type, adjustable: prog, disabled: false, note: "", top_mv: top,
        min_mv: p.min_mv, max_mv: p.max_mv, max_ma: p.max_ma,
        value: (p.min_mv === p.max_mv ? fmtV(p.max_mv) : fmtV(p.min_mv) + "-" + fmtV(p.max_mv)) + " V " + fmtA(p.max_ma) + " A"
      };
      if (!fixed && !prog) { row.disabled = true; row.note = p.type + " PDOs are not requested from here"; return row; }
      if (fixed && p.max_mv > CEILING_MV) { row.disabled = true; row.note = "Above the 36 V the K2 can monitor"; return row; }
      var mv = 0;
      if (prog) mv = snapMv(p.type, p.min_mv <= SAFE_MV && SAFE_MV <= top ? SAFE_MV : p.min_mv, p.min_mv, p.max_mv);
      row.request = { pos: i + 1, mv: mv, ma: p.max_ma, confirm: needsConfirm(fixed ? p.max_mv : mv) };
      row.note = fixed ? "fixed, " + fmtW(p.max_mv, p.max_ma) + " W"
        : (p.type === "pps" ? "programmable" : "adjustable") + ", up to " + fmtV(top) + " V" + (top < p.max_mv ? " (capped at 36 V)" : "") +
          ", " + fmtA(p.max_ma) + " A";
      return row;
    });
  }
  // The request for a row at a chosen set point: a fixed PDO is requested as advertised (mv 0).
  function pdRequestFor(row, mv, ma) {
    var m = row.adjustable ? snapMv(row.type, mv, row.min_mv, row.max_mv) : 0;
    return { pos: Number(row.id), mv: m, ma: clampMa(ma, row.max_ma), confirm: needsConfirm(row.adjustable ? m : row.max_mv) };
  }

  // ---- fast charge --------------------------------------------------------------------------------------------------------
  var FC_MAX_QC_MV = 12000;
  var FC_NO_TRIGGER = ["vooc", "svooc", "svooc65"];   // detect and status only, never a trigger button
  function fcNeedsConfirm(mv) { return mv > SAFE_MV; }
  // levels: k2proto FC_LEVELS ({proto: {mv: [..], def}}), protos: FC_PROTOS (the order to list in).
  function fcOffers(levels, protos) {
    var out = [];
    protos.forEach(function (proto) {
      var l = levels[proto];
      if (!l || FC_NO_TRIGGER.indexOf(proto) >= 0) return;
      var qc = proto.indexOf("qc") === 0;
      var mv = l.mv.filter(function (v) { return v <= CEILING_MV && (!qc || v <= FC_MAX_QC_MV); });
      if (!mv.length) return;
      var d = mv.indexOf(l.mv[l.def]);
      out.push({ proto: proto, mv: mv, def: d < 0 ? 0 : d });
    });
    return out;
  }
  // The D+/D- readout (the firmware's detected fast-charge name) as a label; the VOOC family is read, never triggered.
  function fcDetected(name) {
    if (!name || name === "...") return { label: "reading", note: "", detectOnly: false };
    var vooc = /VOOC/i.test(name);
    return { label: name, note: vooc ? "Detect only: the K2 does not trigger VOOC or SuperVOOC." : "", detectOnly: vooc };
  }

  // ---- renderers ----------------------------------------------------------------------------------------------------------
  // RowList: rows [{id, label, value, note?, disabled?}]. A radio group of square markers; the focused row's note follows it.
  function rowListHtml(rows, opts) {
    opts = opts || {};
    var out = '<div class="k-rl' + (opts.cls ? " " + esc(opts.cls) : "") + '" role="radiogroup"' + (opts.name ? ' aria-label="' + esc(opts.name) + '"' : "") + ">";
    rows.forEach(function (r) {
      var sel = opts.selected !== undefined && opts.selected === r.id;
      out += '<div class="k-rl-row' + (sel ? " k-sel" : "") + (r.disabled ? " k-dis" : "") + (opts.focus === r.id ? " k-focus" : "") +
        '" role="radio" tabindex="' + (r.disabled ? "-1" : "0") + '" data-id="' + esc(r.id) + '" aria-checked="' + (sel ? "true" : "false") + '"' +
        (r.disabled ? ' aria-disabled="true"' : "") + '><i class="k-mk"></i><span class="k-rl-l">' + esc(r.label) +
        '</span><span class="k-rl-v">' + esc(r.value) + "</span></div>";
      if (opts.focus === r.id && r.note) out += '<div class="k-rl-note">' + esc(r.note) + "</div>";
    });
    return out + "</div>";
  }
  // ActionCell: state idle | armed | busy | disabled. Armed drains over leftMs (CSS animation), the page re-renders on expiry.
  function actionCellHtml(a) {
    var st = a.state || "idle", armed = st === "armed", off = st === "busy" || st === "disabled";
    return '<button type="button" class="k-act' + (armed ? " k-armed" : "") + (st === "busy" ? " k-busy" : "") + '" data-act="' + esc(a.id) + '"' +
      (off ? " disabled" : "") + (armed ? ' aria-pressed="true"' : "") + (a.reason ? ' title="' + esc(a.reason) + '"' : "") + ">" +
      '<span class="k-act-t">' + esc(armed ? (a.armedLabel || "CONFIRM") : a.label) + "</span>" +
      (armed ? '<i class="k-drain" style="width:' + Math.round(100 * Math.min(ARM_MS, Math.max(0, a.leftMs === undefined ? ARM_MS : a.leftMs)) / ARM_MS) + '%"></i>' : "") + "</button>";
  }
  function alertBandHtml(a) {
    if (!a || !a.text) return "";
    var tone = ["ok", "caution", "error"].indexOf(a.tone) >= 0 ? a.tone : "info", urgent = tone === "error" || tone === "caution";
    return '<div class="k-foot k-alert k-foot-' + tone + '" role="' + (urgent ? "alert" : "status") + '">' + esc(a.text) + "</div>";
  }
  function topBarHtml(t) {
    var have = typeof t.mv === "number" && typeof t.ma === "number";
    return '<div class="k-top"><span class="k-top-t">' + esc(t.title) + '</span><span class="k-top-r">' +
      '<span class="k-u-v">' + (typeof t.mv === "number" ? fmtV(t.mv) : "-") + " V</span> " +
      '<span class="k-u-a">' + (typeof t.ma === "number" ? fmtA(t.ma) : "-") + " A</span>" +
      (have ? ' <span class="k-u-w">' + fmtW(t.mv, t.ma) + " W</span>" : "") + "</span></div>";
  }
  return {
    kvTableHtml: kvTableHtml, VERDICTS: VERDICTS, esc: esc, verdictCellHtml: verdictCellHtml, bracketRuns: bracketRuns, resultGridHtml: resultGridHtml,
    rowListHtml: rowListHtml, actionCellHtml: actionCellHtml, alertBandHtml: alertBandHtml, topBarHtml: topBarHtml,
    ARM_MS: ARM_MS, armPress: armPress, armExpire: armExpire, armLeftMs: armLeftMs, armCancel: armCancel, footerState: footerState,
    CEILING_MV: CEILING_MV, snapMv: snapMv, stepMv: stepMv, clampMa: clampMa, needsConfirm: needsConfirm, pdoRows: pdoRows, pdRequestFor: pdRequestFor,
    FC_MAX_QC_MV: FC_MAX_QC_MV, fcOffers: fcOffers, fcNeedsConfirm: fcNeedsConfirm, fcDetected: fcDetected,
    fmtV: fmtV, fmtA: fmtA, fmtW: fmtW
  };
})();
if (typeof module !== "undefined" && module.exports) module.exports = K2Kit;
