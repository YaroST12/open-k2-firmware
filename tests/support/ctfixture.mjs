// Wire-row fixtures for the charger-test grid: the rows k2proto's decodeChargerTest returns, built by hand.
// Shared by tests/test_ctgrid.mjs (layout rules) and tests/test_ctpage.mjs (the page against a fake K2).

const PROTOCOL = { epr: "epr-fixed", vooc: "vooc-5v-handshake", svooc: "svooc-5v-handshake", svooc65: "svooc65-5v-handshake" };
export const PD = ["pd2-fixed", "pd3x-fixed", "epr", "spr-avs", "pps", "epr-avs", "pd3.0-revision", "pd3.1-revision", "pd3.2-revision"];

// One wire row: protocol, algorithm, result and reason are strings (as the decoder returns them).
export function S(alg, mv, result, o = {}) {
  return { algorithm: alg, protocol: PROTOCOL[alg] || alg, target_mv: mv, request_ma: o.ma ?? (PD.includes(alg) ? 1000 : 0),
           raw_pdo: o.pdo || 0, pos: o.pos || 0, result, reason: o.reason || "none", detail: o.detail || 0,
           continuous: !!o.cont, avs: !!o.avs, sweep: !!o.sweep, measured_mv: o.measured || 0 };
}

export const REQ = "request failed", NOOFFER = "not advertised", PDACTIVE = "PD active: use QC port";

// A PD brick with PPS, no EPR: five fixed levels, two PPS ranges (one failing point, one skipped), then the legacy units.
export function baseus() {
  const s = [];
  const fixed = [167874860, 184620, 246060, 307500, 409935], mv = [5000, 9000, 12000, 15000, 20000];
  for (let i = 0; i < 5; i++) s.push(S("pd3x-fixed", mv[i], "pass", { pos: i + 1, pdo: fixed[i] }));
  s.push(S("epr", 0, "skip", { reason: NOOFFER }));
  s.push(S("spr-avs", 0, "skip", { reason: NOOFFER }));
  for (const t of [5000, 8000, 11000]) s.push(S("pps", t, "pass", { pos: 6, pdo: 3235656292 }));
  s.push(S("pps", 8000, "fail", { pos: 6, pdo: 3235656292 }));
  s.push(S("pps", 5000, "skip", { pos: 6, pdo: 3235656292, reason: REQ }));
  for (const t of [5000, 13000, 21000]) s.push(S("pps", t, "pass", { pos: 7, pdo: 3248763456 }));
  s.push(S("epr-avs", 0, "skip", { reason: NOOFFER }));
  for (const a of ["qc2", "qc3"]) s.push(S(a, 5000, "skip", { reason: PDACTIVE }));
  s.push(S("fcp", 5000, "fail")); s.push(S("fcp", 9000, "skip", { reason: REQ }));
  s.push(S("ufcs", 5000, "fail"));
  return s;
}

// The decoder also gives every row its index, unit (group), attempts, explanation and detail words.
export function withIndex(rows) {
  return rows.map((r, i) => ({
    index: i, group: PD.includes(r.algorithm) ? "pd" : r.algorithm.replace(/-.*/, ""), attempts: 0,
    detail_reason: "none", explanation: r.reason === "none" ? "" : r.reason, ...r }));
}

// What the page paints for a run that has read nothing yet: a pending row has no verdict evidence.
export function pending(row) {
  return { ...row, result: "pending", reason: "none", explanation: "", measured_mv: 0, detail: 0 };
}
