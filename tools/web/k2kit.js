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
//   pdStep / pdSelection / contractView            the stepper buttons, the selected row, the contract KvTable rows
//   errorText(e, what)                             a failure in plain words for the footer (never a status code)
//   messageTableHtml / specResultHtml              the recent PD messages; the PD Spec Check blob as KvTable + ResultGrid
//   fcOffers / fcNeedsConfirm / fcDetected         what fast charge the page offers: QC at or below 12 V, never a VOOC family trigger
//   fcSelection / fcStepLevel                      the picked protocol and level, and one step through an offer's levels
//   ROUTES / routeFor / pageTitle                  the dashboard's pages; the old #triggers address opens PD
//   actCell / stepperHtml                          a page action as an ActionCell, and the stepper buttons (data-step)
//   SPEC_FAMILY_NOTES / specFamilyMask / specProgress   the PD Spec Check family boxes, the mask they send, the progress line
//   savedRows / cardRows / unmatchedView           the saved-cable and charger-card RowLists, the held-back e-marker replies
//   fmtV / fmtA / fmtW                             voltage, current and power text
//
// verdict is one of pending running pass fail none skip stopped partial (the device's Verdict enum), plus unverified (the PD
// Spec Check's inferred limits: a 1 px amber outline, no fill, and the word in its label). Colour never carries it alone: pass and
// fail differ in fill and ink, skip strikes the volts, running adds a dot, none and stopped have no fill.
var K2Kit = (function () {
  var VERDICTS = ["pending", "running", "pass", "fail", "none", "skip", "stopped", "partial", "unverified"];
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

  // One set point button of the stepper. dir: mv+ mv- (100 mV), mv++ mv-- (1 V), ma+ ma- (250 mA). Fixed PDOs only move mA;
  // nothing leaves the PDO's own range, 50 mA is the floor. cur is {mv, ma}; a new object comes back.
  var MA_STEP = 250, MA_FLOOR = 50;
  function pdStep(row, cur, dir) {
    var mv = cur.mv, ma = cur.ma;
    if (/^mv/.test(dir)) {
      if (!row.adjustable) return { mv: mv, ma: ma };
      var big = dir.length === 4, up = dir.charAt(2) === "+";
      mv = snapMv(row.type, mv + (up ? 1 : -1) * (big ? 1000 : 100), row.min_mv, row.max_mv);
    } else if (dir === "ma+" || dir === "ma-") {
      ma = Math.min(row.max_ma, Math.max(MA_FLOOR, Math.round((ma + (dir === "ma+" ? MA_STEP : -MA_STEP)) / MA_FLOOR) * MA_FLOOR));
    }
    return { mv: mv, ma: ma };
  }
  // Which PDO row is selected: the user's pick while it can still be requested, else the row the contract holds, else the
  // first row that can be requested, else none.
  function pdSelection(rows, contract, picked) {
    var ok = function (id) { return rows.some(function (r) { return r.id === id && !r.disabled; }); };
    if (picked !== null && picked !== undefined && ok(picked)) return picked;
    if (contract && contract.held && ok(String(contract.pos))) return String(contract.pos);
    for (var i = 0; i < rows.length; i++) if (!rows[i].disabled) return rows[i].id;
    return null;
  }

  // The KvTable rows and tone footer for the contract the K2 holds (PD_TRIGGER_STATUS decoded).
  function contractView(s) {
    if (!s) return { rows: [{ label: "State", value: "-" }], tone: "info", footer: "No PD status yet." };
    var held = !!s.held, req = held || s.mv > 0 ? fmtV(s.mv) + " V " + fmtA(s.ma) + " A" + (s.epr ? " EPR" : "") : "-";
    var rows = [
      { label: "State", value: s.state },
      { label: "PDO", value: s.pos ? String(s.pos) : "-" },
      { label: "Requested", value: held && s.mv ? req : "-" },
      { label: "Power", value: held && s.mv ? fmtW(s.mv, s.ma) + " W" : "-" },
      { label: "Hard resets", value: String(s.hard_resets) }
    ];
    if (s.hard_resets > 0) { rows[4].level = "caution"; rows[4].word = "SEEN"; }
    var tone = "info", footer = "No contract held.";
    if (held) { tone = "ok"; footer = "Holding " + req + "."; }
    else if (s.state === "failed" || ["rejected", "wait-exhausted", "timeout", "tx-failed", "hard-reset", "epr-failed", "over-cap", "bad-choice"].indexOf(s.result) >= 0) {
      tone = "error"; footer = "Last request " + s.result + ".";
    }
    return { rows: rows, tone: tone, footer: footer };
  }

  // A failure the page met, in plain words for the footer band. what names the thing that was being done.
  function errorText(e, what) {
    var n = e && e.name;
    if (n === "RemoteTimeout") return "The K2 did not answer (" + what + "). Check the cable and try again.";
    if (n === "BootloaderDetected") return "The K2 is in its bootloader (DFU). Flash the firmware or replug it.";
    if (n === "RangeError") return e.message;
    if (n === "RemoteError") {
      var reason = /\(([^)]+)\)\s*$/.exec(e.message || "");
      switch (e.status) {
        case 1: return "This firmware does not have that command (" + what + "). Update the K2.";
        case 2: return "The K2 does not understand this request (" + what + "). The firmware and the page differ in version.";
        case 3: return "The K2 refused the " + what + (reason ? ": " + reason[1] : "") + ".";
        case 4: return "The K2 is busy (" + what + "). Wait for the running job to finish.";
        case 5: return "The K2 wants a confirm for the " + what + ". Press again to confirm.";
      }
    }
    return what + ": " + (e && e.message ? e.message : String(e));
  }

  // ---- pages ----------------------------------------------------------------------------------------------------------------
  var ROUTES = ["live", "test", "pd", "fc", "spec", "cables", "device"];
  var ROUTE_ALIAS = { triggers: "pd" };
  var TITLES = { live: "Live", test: "Charger test", pd: "PD", fc: "Fast charge", spec: "PD Spec Check", cables: "Cables", device: "Device" };
  function routeFor(hash) {
    var h = String(hash || "").replace(/^#\/?/, "");
    if (ROUTE_ALIAS[h]) h = ROUTE_ALIAS[h];
    return ROUTES.indexOf(h) >= 0 ? h : "live";
  }
  function pageTitle(route) { return TITLES[route] || route; }

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
  // The offer the page has selected: {proto, mv, offer}. picked = {proto, mv} from the page (or null); a protocol that is not
  // offered falls back to the first offer, a level the protocol does not list to its default.
  function fcSelection(offers, picked) {
    if (!offers || !offers.length) return null;
    var offer = offers[0];
    if (picked) offers.forEach(function (o) { if (o.proto === picked.proto) offer = o; });
    var mv = picked && offer.mv.indexOf(picked.mv) >= 0 ? picked.mv : offer.mv[offer.def];
    return { proto: offer.proto, mv: mv, offer: offer };
  }
  // One step through an offer's levels (dir +1 / -1); an unlisted level steps to the nearest listed one in that direction.
  function fcStepLevel(offer, mv, dir) {
    var l = offer.mv, i;
    if (dir > 0) { for (i = 0; i < l.length; i++) if (l[i] > mv) return l[i]; return l[l.length - 1]; }
    for (i = l.length - 1; i >= 0; i--) if (l[i] < mv) return l[i];
    return l[0];
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
  // ActionCell: state idle | armed | busy | disabled. The armed bar is a width the page sets from leftMs on each tick (no CSS
  // animation); the page re-renders on expiry.
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
  // The recent PD messages (k2proto bbMessageRows): time from the first row, direction, name, the data objects in hex. A message
  // that is not on the source's own SOP (cable plug traffic) says so.
  function hex8(o) { return (o >>> 0).toString(16).toUpperCase().padStart(8, "0"); }
  function messageTableHtml(rows) {
    if (!rows || !rows.length) return '<div class="k-msg-empty">No PD messages yet.</div>';
    var t0 = rows[0].t_us, out = '<div class="k-msg">';
    rows.forEach(function (r) {
      out += '<div class="k-msg-row"><span class="k-msg-t">+' + (Math.round((r.t_us - t0) / 1000) / 1000).toFixed(3) + ' s</span>' +
        '<span class="k-msg-d k-msg-' + (r.dir === "tx" ? "tx" : "rx") + '">' + (r.dir === "tx" ? "TX" : "RX") + "</span>" +
        '<span class="k-msg-n">' + esc(r.name) + (r.sop === false ? ' <i class="k-msg-c">cable plug</i>' : "") + "</span>" +
        '<span class="k-msg-o">' + (r.objects || []).map(hex8).join(" ") + "</span></div>";
    });
    return out + "</div>";
  }

  // ---- PD Spec Check result ------------------------------------------------------------------------------------------------
  // The blob (k2proto decodePdSpecBlob) as a KvTable head, a ResultGrid with one line per check in the device's order
  // (ui_pdspec_page.cpp kOrder: the transitions, the PPS/AVS checks, the timeouts, Hard Reset, detach, source), one line per
  // programmable pass, and one tone footer. p is the k2proto namespace (value / limit formatting in the K2's own words). Inferred
  // checks carry no penalty and show as the outlined amber "unverified" cell.
  var SPEC_ORDER = ["accept_time", "ps_rdy_time", "src_start", "ps_rdy_early", "ps_rdy_low", "rise_time", "fall_time", "overshoot", "accuracy",
                    "pps_monotonic", "pps_accuracy", "pps_settle", "pps_band", "pps_ps_rdy", "pps_overshoot", "pps_transient", "pps_status",
                    "pps_status_time", "avs_monotonic", "avs_accuracy", "avs_band", "avs_ps_rdy", "avs_overshoot", "avs_transient",
                    "pps_keepalive", "epr_keepalive", "hr_safe0v", "hr_recover", "hr_turn_on", "hr_caps", "detach_safe0v", "reattach",
                    "reattach_caps", "source_reset"];
  var SPEC_SECTIONS = { accept_time: "Transitions", pps_monotonic: "PPS", avs_monotonic: "AVS", pps_keepalive: "Timeouts", hr_safe0v: "Hard Reset",
                        detach_safe0v: "Detach", source_reset: "Source" };
  var SPEC_CELL = { pass: "pass", fail: "fail", skip: "skip", pending: "pending", unverified: "unverified" };
  function specRank(key) { var i = SPEC_ORDER.indexOf(key); return i < 0 ? SPEC_ORDER.length : i; }
  function msText(v) { return v === null || v === undefined ? "not seen" : v.toFixed(1) + " ms"; }
  function failText(label, n) { return n ? label + ", " + n + " fail" : label; }
  // One programmable pass (k2proto passes[]): the band entry (the 20 mV class: the settle on the final level, unverified), PS_RDY,
  // the level (worst error; monotonic, accuracy, overshoot and transient fails) and PPS_Status (PPS only).
  function specPassLine(ps) {
    var f = ps.fails, settle = ps.kind === "pps" && ps["class"] === 0;
    var band = settle ? { verdict: "unverified", label: msText(ps.band_ms), current: "settle", title: "Settle on the final level: inferred, not counted" }
                      : { verdict: f.band || ps.band_ms === null ? "fail" : "pass", label: msText(ps.band_ms), current: "band",
                          title: failText("worst entry into the band of " + ps.steps + " moves", f.band) };
    var rdy = { verdict: f.ps_rdy || ps.psrdy_ms === null ? "fail" : "pass", label: msText(ps.psrdy_ms), current: "PS_RDY",
                title: failText("worst Accept to PS_RDY of " + ps.steps + " moves", f.ps_rdy) };
    var nLevel = f.monotonic + f.accuracy + f.overshoot + f.transient;
    var lvl = { verdict: nLevel ? "fail" : "pass", label: (ps.worst_err_mv >= 0 ? "+" : "") + ps.worst_err_mv + " mV", current: "level",
                title: "worst level error; fails: monotonic " + f.monotonic + ", accuracy " + f.accuracy + ", overshoot " + f.overshoot +
                       ", transient " + f.transient };
    var st = ps.kind === "pps" ? { verdict: ps.status ? "fail" : "pass", label: ps.status_text || "clean", current: "status" }
                               : { verdict: "none", label: "-", current: "status" };
    return { label: ps.kind.toUpperCase() + " " + ps.step, cells: [band, rdy, lvl, st] };
  }
  function specResultHtml(b, p) {
    var checks = b.checks.slice().sort(function (x, y) { return specRank(x.key) - specRank(y.key); });
    var lines = checks.map(function (c) {
      var v = SPEC_CELL[c.verdict] || "pending", unver = c.verdict === "unverified";
      var label = c.verdict === "skip" ? (c.skip || "skip") : c.verdict === "pending" || c.worst === null ? "-" : p.pdSpecValue(c.unit, c.worst);
      var cell = { verdict: v, label: label, title: unver ? "Inferred check: not counted in the penalty" : "" };
      if (v !== "skip" && v !== "pending") cell.current = p.pdSpecLimit(c);
      return { label: c.label + (unver ? " (unverified)" : ""), section: !!SPEC_SECTIONS[c.key], cells: [cell] };
    });
    var t = b.tally, running = b.state === "running";
    var head = [
      { label: "State", value: running ? "running" : b.state },
      { label: "Penalty", value: String(b.score), level: b.score > 0 ? "caution" : undefined, word: "PENALTY" },
      { label: "Checks", value: t.pass + " pass, " + t.fail + " fail, " + t.skip + " skip" + (t.unverified ? ", " + t.unverified + " unverified" : "") },
      { label: "Time", value: b.elapsed_s + " s" }
    ];
    if (b.caps_note) head.push({ label: "Caps note", value: b.caps_note });
    if (b.passes && b.passes.length) head.push({ label: "PPS 20 mV", value: b.full20 ? "whole window" : "three 10-step windows" });
    var parts = [], tone = "ok";
    var raise = function (x) { if (x === "error" || (x === "caution" && tone !== "error") || (x === "info" && tone === "ok")) tone = x; };
    if (running) { parts.push("Check running."); raise("info"); }
    if (t.fail > 0) { parts.push(t.fail + " check" + (t.fail > 1 ? "s" : "") + " failed."); raise("error"); }
    if (!running && b.stop !== "done" && b.stop !== "running") { parts.push("Stopped early: " + b.stop + "."); raise("caution"); }
    if (b.release_unconfirmed) { parts.push("The 5 V release was not confirmed."); raise("caution"); }
    if (!parts.length) parts.push("All checks passed.");
    var out = kvTableHtml(head, { footer: parts.join(" "), tone: tone }) + resultGridHtml(lines, { cls: "k-spec" });
    if (b.passes && b.passes.length) {
      var pl = b.passes.map(specPassLine), many = {};
      b.passes.forEach(function (ps) { many[ps.pos] = 1; });
      if (Object.keys(many).length > 1) pl.forEach(function (ln, i) { ln.label = "A" + b.passes[i].pos + " " + ln.label; });
      pl.forEach(function (ln, i) { ln.section = i === 0 || b.passes[i].pos !== b.passes[i - 1].pos; });
      out += '<div class="k-sub">Programmable passes</div>' + resultGridHtml(pl, { cls: "k-spec" });
    }
    return out;
  }

  // ---- the shared arm state as ActionCells, steppers, the spec picker and the cable lists ------------------------------------------
  // a: {id, label, armedLabel?, reason?}; arm is the one shared {id, at} or null; off is "busy", any other truthy value (disabled) or falsy.
  function actCell(a, arm, now, off) {
    var left = arm && arm.id === a.id ? armLeftMs(arm, now) : 0;
    var st = off ? (off === "busy" ? "busy" : "disabled") : left > 0 ? "armed" : "idle";
    return actionCellHtml({ id: a.id, label: a.label, armedLabel: a.armedLabel, reason: a.reason, state: st, leftMs: left });
  }
  // groups: [{name, value, dec: [{dir, text, off?}], inc: [...]}]; a button carries data-step with its direction.
  function stepperHtml(groups) {
    var btn = function (b) { return '<button type="button" class="k-btn" data-step="' + esc(b.dir) + '"' + (b.off ? " disabled" : "") + ">" + esc(b.text) + "</button>"; };
    return '<div class="k-steppers">' + groups.map(function (g) {
      return '<div class="k-step" role="group" aria-label="' + esc(g.name) + '"><span class="k-step-n">' + esc(g.name) + "</span>" +
        g.dec.map(btn).join("") + '<span class="k-step-v">' + esc(g.value) + "</span>" + g.inc.map(btn).join("") + "</div>";
    }).join("") + "</div>";
  }
  // One note per family, in the engine's order (Transitions, PPS/AVS, PPS timeout, EPR timeout, Hard Reset, Detach).
  var SPEC_FAMILY_NOTES = ["Steps through the fixed levels", "Sweeps each PPS / AVS range in 3 step sizes, then slams it", "Waits out the PPS keep-alive", "Waits out the EPR keep-alive",
                           "Hard Resets the source", "Detaches the source"];
  function specFamilyMask(checked) {
    var m = 0;
    checked.forEach(function (on, i) { if (on) m |= 1 << i; });
    return m;
  }
  function clock(s) { return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); }
  // The status block (k2proto decodePdSpecStatus) as one footer line and its tone.
  function specProgress(st) {
    if (st.start && st.start !== "ok") return { tone: "error", text: "The run did not start: " + st.start + "." };
    if (st.state === "running") {
      return { tone: "info", text: "Running: " + st.family + ", " + st.item + " of " + st.items + ", " + clock(st.elapsed_s) + ", " + st.fails + " failed." };
    }
    if (st.state === "finished") {
      if (st.stop !== "done") return { tone: "caution", text: "Stopped early: " + st.stop + "." };
      if (st.release_unconfirmed) return { tone: "caution", text: "Finished, but the 5 V release was not confirmed." };
      return st.fails > 0 ? { tone: "error", text: "Finished: " + st.fails + " check" + (st.fails > 1 ? "s" : "") + " failed." } : { tone: "ok", text: "Finished: all checks passed." };
    }
    return { tone: "info", text: "No run yet. Pick the families and start." };
  }
  function savedRows(list) {
    return list.map(function (c) {
      return { id: String(c.seq), label: c.name || "(unnamed)", value: hexw(c.vid, 4) + ":" + hexw(c.pid, 4) };
    });
  }
  function cardRows(list) {
    return list.map(function (c) {
      return { id: String(c.no), label: "#" + c.no + " " + (c.name || "(unnamed)"), value: c.pass + " pass " + c.fail + " fail " + c.skip + " skip",
               note: hexw(c.vid, 4) + ":" + hexw(c.pid, 4) + ", " + c.pdos + " PDOs, " + c.runs + " runs, " + c.state };
    });
  }
  function hexw(n, w) { return (n >>> 0).toString(16).toUpperCase().padStart(w, "0"); }
  // What the firmware could not match during a learn: SOP' / SOP'' header and data words, newest the page can read.
  function unmatchedView(t) {
    if (!t.entries || !t.entries.length) return { rows: [{ label: "Held", value: "none" }], footer: "Nothing held back." };
    return {
      rows: t.entries.map(function (e) { return { label: e.sop + " " + hexw(e.header, 4), value: (e.words || []).map(hex8).join(" ") || "-" }; }),
      footer: "Showing " + t.entries.length + " of " + t.total + " unmatched message" + (t.total === 1 ? "" : "s") + "."
    };
  }

  return {
    actCell: actCell, stepperHtml: stepperHtml, SPEC_FAMILY_NOTES: SPEC_FAMILY_NOTES, specFamilyMask: specFamilyMask, specProgress: specProgress,
    savedRows: savedRows, cardRows: cardRows, unmatchedView: unmatchedView,
    pdStep: pdStep, pdSelection: pdSelection, contractView: contractView, errorText: errorText, messageTableHtml: messageTableHtml,
    specResultHtml: specResultHtml,
    kvTableHtml: kvTableHtml, VERDICTS: VERDICTS, esc: esc, verdictCellHtml: verdictCellHtml, bracketRuns: bracketRuns, resultGridHtml: resultGridHtml,
    rowListHtml: rowListHtml, actionCellHtml: actionCellHtml, alertBandHtml: alertBandHtml, topBarHtml: topBarHtml,
    ARM_MS: ARM_MS, armPress: armPress, armExpire: armExpire, armLeftMs: armLeftMs, armCancel: armCancel, footerState: footerState,
    CEILING_MV: CEILING_MV, snapMv: snapMv, stepMv: stepMv, clampMa: clampMa, needsConfirm: needsConfirm, pdoRows: pdoRows, pdRequestFor: pdRequestFor,
    FC_MAX_QC_MV: FC_MAX_QC_MV, fcOffers: fcOffers, fcNeedsConfirm: fcNeedsConfirm, fcDetected: fcDetected,
    fcSelection: fcSelection, fcStepLevel: fcStepLevel, ROUTES: ROUTES, routeFor: routeFor, pageTitle: pageTitle,
    fmtV: fmtV, fmtA: fmtA, fmtW: fmtW
  };
})();
if (typeof module !== "undefined" && module.exports) module.exports = K2Kit;
