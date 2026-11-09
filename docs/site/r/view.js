// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Yaraslau Furman
"use strict";
// Draws the results page from K2Records.read (records.js, which uses card.js for the charger-test card). Every kind of record
// gives the same view shape: header, tiles, sections. Everything shown is set with textContent: the names come from the
// address, so they are data, never markup. Nothing here talks to a network.
(function () {
  var $ = function (id) { return document.getElementById(id); };

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined) e.textContent = text;
    return e;
  }

  function badge(b) {
    var li = el("li", "badge " + b.tone);
    li.appendChild(el("span", "glyph", b.glyph));
    var body = el("span", "body");
    body.appendChild(el("span", "label", b.label));
    body.appendChild(el("span", "status", b.status));
    li.appendChild(body);
    li.setAttribute("aria-label", b.label + ": " + (b.word ? b.word + ", " : "") + b.status);
    return li;
  }

  function rowsTable(rows) {
    var dl = el("dl", "rows");
    rows.forEach(function (r) {
      var row = el("div", "row " + r.tone);
      row.appendChild(el("dt", "", r.label));
      row.appendChild(el("dd", "", r.value));
      dl.appendChild(row);
    });
    return dl;
  }

  function group(g) {
    var box = el("div", "group " + (g.tone || ""));
    box.appendChild(el("h4", "", g.title));
    box.appendChild(rowsTable(g.rows));
    return box;
  }

  function section(s) {
    var d = el("details", "section");
    var sum = el("summary", "", s.title);
    d.appendChild(sum);
    if (!s.groups.length) d.appendChild(el("p", "note", s.empty || ""));
    s.groups.forEach(function (g) { d.appendChild(group(g)); });
    return d;
  }

  function show(v) {
    $("welcome").hidden = true;
    $("problem").hidden = true;
    $("card").hidden = false;
    var h = v.header;
    $("title").textContent = h.title;
    var facts = $("facts");
    facts.textContent = "";
    h.facts.forEach(function (f) { facts.appendChild(el("li", "", f)); });
    $("ident").textContent = h.ident;
    $("kicker").textContent = v.kicker || "Open-K2 charger test";
    $("badges-title").textContent = v.badges_title || "What this charger did";
    $("full-title").textContent = v.full_title || "Full results";
    document.title = h.title + " · Open-K2 results";
    var grid = $("badges");
    grid.textContent = "";
    v.badges.forEach(function (b) { grid.appendChild(badge(b)); });
    var full = $("full");
    full.textContent = "";
    v.sections.forEach(function (s, i) {
      var d = section(s);
      if (i === 0) d.open = true;
      full.appendChild(d);
    });
  }

  function problem(e) {
    $("card").hidden = true;
    var missing = e && e.code === "missing";
    $("welcome").hidden = !missing;
    $("problem").hidden = missing;
    if (!missing) $("problem-text").textContent = e && e.message ? e.message : "This link could not be read.";
    document.title = "Open-K2 results";
  }

  function render(text) {
    try {
      show(K2Records.read(text).view);
      return true;
    } catch (e) {
      problem(e);
      return false;
    }
  }

  function boot() {
    render(location.hash);
    window.addEventListener("hashchange", function () { render(location.hash); });
    $("paste-form").addEventListener("submit", function (ev) {
      ev.preventDefault();
      var text = $("paste").value;
      if (render(text)) {
        var at = text.indexOf("#"), one = text.trim();
        // keep the address in step with what is shown, so the page can be bookmarked or shared (a record of several
        // pasted K2 data lines has no single address)
        if (!/[\r\n]/.test(one)) {
          try { history.replaceState(null, "", "#" + (at >= 0 ? text.slice(at + 1) : text).trim()); } catch (x) { /* a file:// page may refuse */ }
        }
        window.scrollTo(0, 0);
      }
    });
  }

  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", boot);
  else boot();
})();
