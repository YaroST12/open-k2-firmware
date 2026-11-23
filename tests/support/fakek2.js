// A fake K2 for the dashboard page, injected into tools/web/k2.html right after the embedded k2proto block by
// tests/support/chrome.mjs. The page talks HTTP to the test server (S.net), which answers every frame with
// UNKNOWN_CMD; this script replaces only the charger-test calls with a scripted run, so no payload is encoded.
// Drive it from the test with page.eval("__fake.begin(rows)") / "__fake.advance(n)" / "__fake.end('done')".
(function () {
  var P = K2Proto, R = P.K2Remote.prototype;
  var F = window.__fake = { run: null, nextId: 41, plan: [], reads: 0, rowReads: [], rowDelayMs: 15, calls: [], cancels: 0 };

  function clone(o) { return JSON.parse(JSON.stringify(o)); }
  function pendingRow(r) {
    var p = clone(r);
    p.result = "pending"; p.reason = "none"; p.explanation = ""; p.measured_mv = 0; p.detail = 0;
    return p;
  }
  function sleep(ms) { return new Promise(function (res) { setTimeout(res, ms); }); }

  // run = { id, rev, state, reason, final: [rows], progress: n rows decided, running: bool }
  function shown(r) {
    return r.final.map(function (row, i) {
      if (i < r.progress) return clone(row);
      var p = pendingRow(row);
      if (i === r.progress && r.state !== "done" && r.state !== "stopped") p.result = "running";
      return p;
    });
  }
  function meta(steps) {
    var r = F.run, none = !r;
    return { schema: 2, run_id: none ? 0 : r.id, revision: none ? 0 : r.rev, state: none ? "idle" : r.state,
             reason: none ? "none" : r.reason, total: none ? 0 : r.final.length, current: none ? 0 : Math.min(r.progress, r.final.length),
             start: 0, count: steps ? steps.length : 0,
             selection: { run: [], configured: [], run_mask: 0, configured_mask: 0, pps_sweep: false, avs_sweep: false },
             stage: none || r.state === "done" || r.state === "stopped" ? "results" : "running",
             evidence: { origin: none ? "no-run" : "run-start", source_session: 7, raw_header: 0x1161, pdo_count: 7,
                         wire_revision: "PD 3.1", source_rmdo: null,
                         advertised: { pps: true, epr_table: false, spr_avs: false, epr_capable: false, partial_epr: false },
                         monitor_mv: 36000, physical_cable: { observed: false, max_mv: 20000, max_ma: 3000 },
                         spoof_on: false, ps_rdy: null },
             steps: steps || [] };
  }

  F.begin = function (rows) {
    F.run = { id: ++F.nextId, rev: 1, state: "baseline", reason: "none", final: clone(rows), progress: 0 };
    F.reads = 0; F.rowReads = [];
    return F.run.id;
  };
  F.advance = function (n) {
    var r = F.run;
    r.progress = Math.min(r.final.length, r.progress + (n === undefined ? 1 : n));
    r.state = "requesting"; r.rev++;
    return r.progress;
  };
  F.end = function (state, reason) {
    var r = F.run;
    if (state === "done") r.progress = r.final.length;
    r.state = state; r.reason = reason || "none"; r.rev++;
  };
  F.reset = function () { F.run = null; };
  F.show = function () { return F.run ? shown(F.run) : []; };

  // Memory card: a settable snapshot (F.mem merges over the defaults) so a test can put the stack low-water mark where it likes.
  F.mem = { ram_total: 98304, ram_static: 70656, psp_size: 10944, psp_now: 10768, psp_low: 7400, msp_size: 1344, msp_low: 900,
            fs_blocks: 146, fs_used: 40, image_bytes: 676864, flash_bytes: 1000000, fs_block_bytes: 2048, fs_valid: true, ok: true };
  F.memCalls = 0;
  R.memory = async function () {
    F.memCalls++;
    var m = JSON.parse(JSON.stringify(F.mem)), p = new Uint8Array(47), d = new DataView(p.buffer);
    ["ram_total", "ram_static", "psp_size", "psp_now", "psp_low", "msp_size", "msp_low", "fs_blocks", "fs_used", "image_bytes", "flash_bytes"]
      .forEach(function (k, i) { d.setUint32(4 * i, m[k], true); });
    d.setUint16(44, m.fs_block_bytes, true);
    p[46] = (m.fs_valid ? 1 : 0) | (m.ok ? 2 : 0);
    return P.decodeMemory(p);
  };
  R.state = async function () { return P.decodeState(new Uint8Array(24)); };   // an idle K2: the page's poll loop needs it to run its tasks
  R.ping = async function () { return { version: "1.0.2-fake", git: "fake", proto: 1, uptime_ms: 123456 }; };
  R.chargerTest = async function (kind) {
    F.calls.push("test:" + kind);
    if (kind === "view") {
      var r = F.run, live = !!r && r.state !== "done" && r.state !== "stopped";
      return { stage: live ? "running" : r ? "results" : "algorithms", cursor: 0, rows: r ? r.final.length : 0, on_screen: false,
               confirming: false, confirm_left_ms: 0, confirm_reattach: false,
               wait: { elapsed_ms: 0, total_ms: 0, retry: false, paused: false }, run_elapsed_s: r ? 20 + r.progress : 0,
               card_no: 0, delta_n: 0, delta_runs: 0, preview: { runnable: 0, skipped: 0, refusal: "none", refusal_mv: 0 },
               sweep_points: { pps: 0, avs: 0 }, guard: { topology: "direct", run_guard: 0 },
               resume: { pending: false, auto: false, gate_ready: false, held: false, behind_cable: false, rd: false },
               self_supplied: false, armed_is_pick: false, cable_armed: false, legacy_units: false, rmdo: null, message: "" };
    }
    if (kind === "results") { F.reads++; var all = F.run ? shown(F.run) : []; return meta(all); }
    return meta(null);
  };
  R.chargerTestRow = async function (m, i) {
    await sleep(F.rowDelayMs);
    var r = F.run;
    if (!r || m.run_id !== r.id || m.revision !== r.rev) throw new P.FrameError("charger-test row " + i + " moved; read the status again");
    F.rowReads.push(i);
    return shown(r)[i];
  };
  R.chargerTestRun = async function () {
    F.calls.push("run");
    await sleep(40);
    var before = F.run ? F.run.id : 0, id = F.begin(F.plan);
    return { started: true, run_id: id, previous_run_id: before };
  };
  R.chargerTestCancel = async function () {
    F.calls.push("cancel"); F.cancels++;
    var r = F.run;
    if (r && r.state !== "done" && r.state !== "stopped") { r.state = "stopped"; r.reason = "user"; r.rev++; return { stopped: "run" }; }
    return { stopped: "nothing" };
  };
})();
