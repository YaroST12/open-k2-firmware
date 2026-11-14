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
  return { VERDICTS: VERDICTS, esc: esc, verdictCellHtml: verdictCellHtml, bracketRuns: bracketRuns, resultGridHtml: resultGridHtml };
})();
if (typeof module !== "undefined" && module.exports) module.exports = K2Kit;
