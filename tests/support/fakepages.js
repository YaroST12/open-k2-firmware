// The device pages' fake, injected after fakek2.js (tests/support/chrome.mjs). It scripts only what the PD, Fast charge, Spec check and
// Cables pages ask for and records every VBUS-affecting call in __fake.sent, so a test can see exactly what reached the "firmware".
(function () {
  var P = K2Proto, R = P.K2Remote.prototype, F = window.__fake;
  function clone(o) { return JSON.parse(JSON.stringify(o)); }
  F.sent = [];
  F.pd = { pdos: [
    { type: "fixed", min_mv: 5000, max_mv: 5000, max_ma: 3000, max_mw: 15000, raw: 0x0001912C },
    { type: "fixed", min_mv: 9000, max_mv: 9000, max_ma: 3000, max_mw: 27000, raw: 0x0002D12C },
    { type: "pps", min_mv: 3300, max_mv: 11000, max_ma: 3000, max_mw: 33000, raw: 0xC0DC213C }
  ], status: { state: "idle", result: "none", held: false, epr: false, pos: 0, mv: 0, ma: 0, hard_resets: 0, auto_hold: false } };
  F.fc = { state: "idle", proto: "NONE", fail: "none", target_mv: 0, vbus_mv: 5000, scan: { running: false, done: false, step: 0, results: [] } };
  F.spec = { state: "idle", stop: "done", start: "ok", family: "Transitions", item: 0, items: 0, families: 63, families_text: "all", fails: 0, passes: 0,
             skips: 0, release_unconfirmed: false, score: 0, elapsed_s: 0, blob_len: 0 };
  F.saved = [{ seq: 1, name: "Cable A", vid: 0x1111, pid: 0x2222 }];
  F.cards = [{ no: 1, name: "Baseus", pass: 3, fail: 0, skip: 1, vid: 0x2B1D, pid: 0x1234, pdos: 7, runs: 2, state: "done" }];
  F.spoof = { active: false, seq: 0, replies: 0 };

  R.pdos = async function () { return { valid: true, num_pdos: F.pd.pdos.length, pdos: clone(F.pd.pdos) }; };
  R.pdStatus = async function () { return clone(F.pd.status); };
  R.pdRequest = async function (pos, o) {
    o = o || {};
    F.sent.push({ call: "pdRequest", pos: pos, mv: o.mv || 0, ma: o.ma || 0, confirm: !!o.confirm });
    var p = F.pd.pdos[pos - 1];
    F.pd.status = { state: "ready", result: "ok", held: true, epr: false, pos: pos, mv: o.mv || p.max_mv, ma: o.ma || p.max_ma, hard_resets: 0, auto_hold: false };
    return clone(F.pd.status);
  };
  R.pdAttach = async function (mv, ma, o) { F.sent.push({ call: "pdAttach", mv: mv, confirm: !!(o && o.confirm) }); return clone(F.pd.status); };
  R.pdRelease = async function () { F.sent.push({ call: "pdRelease" }); F.pd.status.held = false; return clone(F.pd.status); };
  R.pdReset = async function () { F.sent.push({ call: "pdReset" }); return clone(F.pd.status); };
  R.stop = async function () { F.sent.push({ call: "stop" }); return { active: [], active_mask: 0, vbus_mv: 5000, fc_release_busy: false }; };
  R.identity = async function () {
    return { valid: true, epr: false, epr_partial: false, cuktech: false, xiaomi: false, vid: 0x2B1D, pid: 0x1234, bcd: 0x100, svids: [0xFF01],
             vendor: "Fake Maker", model: "F-1" };
  };
  R.identifyRun = async function (o) {
    F.sent.push({ call: "identifyRun", confirm: !!(o && o.confirm) });
    return { identify: { state: "done", answered: ["identity"], no_answer: [], retries: 0, sink_pdos: 3, revision: 3 }, identity: await R.identity() };
  };
  R.blackboxRead = async function () { return { overwritten: false, frozen: false, rebooted: false, next: 0, head: 0, events: [] }; };
  R.pdHandback = async function (c) { F.sent.push({ call: "pdHandback", confirm: !!c }); return { verdict: "ok", text: "handed back", allowed: true }; };
  R.appleRead = async function (arm) { F.sent.push({ call: "appleRead", arm: !!arm }); return { armed: !!arm, why: "ok", reads: 1, ok: true }; };
  R.fc = async function (op, o) {
    o = o || {};
    if (op === "trigger") {
      F.sent.push({ call: "fc", op: op, proto: o.proto, mv: o.mv, confirm: !!o.confirm, allowInline: !!o.allowInline });
      F.fc.state = "holding"; F.fc.proto = o.proto.toUpperCase(); F.fc.target_mv = o.mv;
    } else if (op === "scan") F.sent.push({ call: "fc", op: op, confirm: !!o.confirm });
    else if (op === "reset") { F.sent.push({ call: "fc", op: op }); F.fc.state = "idle"; F.fc.proto = "NONE"; }
    return clone(F.fc);
  };
  R.pdSpecStatus = async function () { return clone(F.spec); };
  R.pdSpecStart = async function (mask, confirm) {
    if (!confirm) throw new RangeError("confirm the spec check");
    F.sent.push({ call: "pdSpecStart", mask: mask, confirm: !!confirm });
    F.spec.state = "running"; F.spec.families = mask;
    return clone(F.spec);
  };
  R.pdSpecStop = async function () { F.sent.push({ call: "pdSpecStop" }); F.spec.state = "finished"; F.spec.stop = "user"; return clone(F.spec); };
  R.emarker = async function () {
    return { valid: true, vendor: "FakeCo", vid: 0x1111, pid: 0x2222, speed: "USB 3.2 Gen 2", max_voltage_v: 20, max_current_a: 5, max_power_w: 100,
             active: false, xid: 0, hw_rev: 1, fw_rev: 2, detail: null, num_vdos: 2, vdos: [1, 2] };
  };
  R.emList = async function () { return clone(F.saved); };
  R.emSpoof = async function (t) {
    if (t !== "status") F.sent.push({ call: "emSpoof", target: t });
    if (t === "off") F.spoof = { active: false, seq: 0, replies: 0 };
    else if (typeof t === "number") F.spoof = { active: true, seq: t, replies: 0 };
    return clone(F.spoof);
  };
  R.emDelete = async function (seq) { F.sent.push({ call: "emDelete", seq: seq }); F.saved = F.saved.filter(function (c) { return c.seq !== seq; }); };
  R.emRename = async function (seq, name) { F.sent.push({ call: "emRename", seq: seq, name: name }); F.saved.forEach(function (c) { if (c.seq === seq) c.name = name; }); };
  R.cardsList = async function () { return clone(F.cards); };
  R.cardsDelete = async function (no) { F.sent.push({ call: "cardsDelete", no: no }); F.cards = F.cards.filter(function (c) { return c.no !== no; }); };
  R.cardsRename = async function (no, name) { F.sent.push({ call: "cardsRename", no: no, name: name }); F.cards.forEach(function (c) { if (c.no === no) c.name = name; }); };
  R.emLearned = async function () {
    return { summary: { outcome: "none", vid: 0, pid: 0, entries: 0, rows: 0, attached: null, logFull: false, unmatchedPolicy: "hold" }, entries: [] };
  };
  R.emUnmatched = async function () { return { total: 0, held: 0, entries: [] }; };
  R.cableR = async function (op) {
    return { idle: false, valid: false, kelvin: op === "kelvin", pos_mohm: 0, gnd_mohm: 0, grade: "waiting", r_mohm: 0, idle_mv: 0, idle_ma: 0, load_mv: 0, load_ma: 0 };
  };
  R.action = async function (name, confirm) { F.sent.push({ call: "action", name: name, confirm: !!confirm }); return {}; };
})();
