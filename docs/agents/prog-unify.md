# Prog-unify: one pass over each PPS / AVS APDO in a Deep run

Status: implemented on branch k2/prog-unify (2026-11-19). Goal (user, 2026-11-19): a Deep charger-test run touches each PPS / AVS APDO once.

## Today

A Deep run on a K2_PDSPEC build moves every programmable APDO twice:

1. The charger test (PD unit): range rows (mid, hi, mid, low-mid, lo, full current) with the transition check
   (`chargertest/prog_check.hpp`), the step walk (5 quanta up from mid) and the deep sweep (every 100 mV, up, at 1 A).
2. The PD spec stage (`State::Spec`, mask 0x03): per SPR APDO four passes, each up and back down
   (PPS 20 mV in three 10-step windows / 100 / 500 mV / slams; AVS 100 / 500 / 1000 mV / slams), Get_PPS_Status at
   the top and the end of every PPS pass.

## Design

**Owner of the moves: the charger test.** The PD spec stage keeps the Transitions family as it is and, per SPR APDO the
charger test already swept, adds only the slams (min to max, max to min) and Get_PPS_Status at their top and end.

**One judge.** `pdspec::measure_prog` (new, `pdspec/prog_judge.hpp`, pure) turns one measured move (StepOut + the set
points + the class) into the per-move measurements: band (or settle, PPS 20 mV), overshoot, transient, accuracy. It
serves three callers:

- the engine's own passes (`Engine::judge_prog`, unchanged behaviour),
- the charger test's moves fed into the engine (`Engine::judge_test_move`, same path: latencies, the aggregates,
  monotonic and track against the previous judged level),
- the range-row caution flags (`progcheck::judge`: overshoot / slow rise / transient / level off are the failing
  overshoot / band / transient / accuracy measurements). Its duplicate limit code goes.

**Which charger-test move feeds which check** (only before the stage: the PD unit; SPR APDOs only):

| move | class (place byte) | checks |
|---|---|---|
| sweep point 2.. of the part at and above 5 V | PPS 1 (100 mV), AVS 0 (100 mV) | accept_time, *_ps_rdy, *_band, *_overshoot, *_transient, *_accuracy, *_monotonic, *_track |
| step-walk level 1..5 | 0 (PPS 20 mV: pps_settle instead of band, AVS 100 mV) | as above |
| range row (Deep transition check capture) | 3 (a jump, as the slams) | accept_time, *_ps_rdy, *_band, *_overshoot, *_transient, *_accuracy; no monotonic / track (each row is its own reference) |
| engine slams + Get_PPS_Status | 3 | as today, plus pps_status, pps_status_time |

The first point of a sweep part and the walk's base are landings (a jump from the previous contract): not judged and
not a reference, the engine's rule (UGREEN landing). Each sweep part and each walk is its own monotonic / track chain.

**Coverage per window.** The session keeps, per APDO position, the sweep points offered and judged; at the part's last
point (`kMoveLast`) the APDO is covered when 3/4 of them were judged. `start_in_test` hands the engine that mask: a
covered window runs the slams only (`first_class()` = kSlam); an uncovered one (sweep off, the 640-point budget spent,
captures refused) runs its four passes as today. So every window is swept once, never zero times.

**Capture.** The row's own Request arms the recorder (`request_pd`, unchanged). When a sweep point, walk level or range
row is recorded, the Runner calls `Env::spec_move(ProgMove, flags)` (flags: chain, sweep, last): the session triggers
the armed recorder (Manual, 1 us post window), or takes a range row's already frozen transition capture as it is. The
session's `poll` judges it (`find_edges`, `analyze_step`, `Engine::judge_test_move`) and releases a sweep / walk
capture; the Runner waits on `Env::spec_feeding()` for at most `kSpecFeedMaxMs` (300 ms), so a missing capture only
drops that point. Sink stamps (Accept, PS_RDY) more than 20 ms from the capture's own Accept are blanked. A chain
breaks after an unjudged move. A range row is fed only when the previous contract was a held Ready one on the same APDO
(its from-level is known). Judging runs in the session poll, never on the Runner's stack.

**Storage.** The engine already lives in `PdSpecSession` (resident .bss, 36 aggregates). It is emplaced at the run's
first fed move (keyed by the run id) and `start_in_test(now, run_id)` starts that same engine instead of a fresh one.
A fed but unstarted engine reads as Idle (status, blob, result read). No new aggregate RAM: +56 B .bss (the feed slot,
the per-APDO counters, the mask).

## Not fed (open decisions)

- The low sweep part (below 5 V, PdLow, last of the run) and the low range rows run after the stage: they stay out of the
  cells. The slams still reach the APDO minimum; small steps below 5 V are no longer judged (the engine judged them).
- Coverage the charger test's moves cannot give: small down-steps, the 500 mV PPS class (the band / PS_RDY boundary at
  vPpsSmallStep; the IQ4's 500 mV PS_RDY finding) and AVS 500 / 1000 mV, the 20 mV windows at the APDO ends (the walk is
  at mid only), the track break (needs 500 mV steps). Not filled here beyond the slams and the status queries.
- EPR AVS: the charger test sweeps it, the engine never judged it (SPR limits): it stays out of the avs_* cells.
- Self-powered runs: the stage is refused (as today); the moves are not fed.
- Resume: the aggregates are RAM only. A run resumed after a reboot judges only the moves after it; its finished
  sweeps are not in the coverage mask, so the engine sweeps those windows itself (twice, on a resumed run only).

## Run time (plan, fixture lists)

| list | engine prog moves before | after | charger-test sweep points (unchanged) |
|---|---|---|---|
| IQ4 (5-20 V, PPS 3.3-11 V) | 248 | 2 | 78 |
| FH22 (PPS 5-21 V, AVS 9-20 V) | 734 | 4 | 161 + 111 |
| UGREEN (PPS 3.3-21 V, AVS 9-20 V) | 776 | 4 | 178 + 111 |
| Baseus 140 W (PPS 3.3-11, 3.3-21 V) | 736 | 4 | 256 |
| Xiaomi bank (PPS 5-11 V) | 206 | 2 | 61 |

Hardware reference: the IQ4's PPS/AVS family took 80 s for 248 moves (2026-11-15), the Transitions 32 s.

Engine Sim, whole stage (`test_pdspec_engine` covered_run_time, Transitions + PPS/AVS):

| list | before | after |
|---|---|---|
| IQ4 | 95.7 s | 20.4 s |
| FH22 | 254.8 s | 34.2 s |
| Xiaomi bank | 86.7 s | 11.4 s |

Added in the charger test: one session poll (about one Ui tick, 20 to 50 ms) per fed move, so about +4 s (IQ4) to
+14 s (FH22) over the sweep. The Review preview's stage estimate was a flat 120 s; it is now plan-derived
(`specsec::estimate_ms`: transitions n(n-1) x 2.7 s, a covered window 4 s, else moves x 320 ms): IQ4 58 s,
FH22 / UGREEN / Baseus 62 s, Xiaomi bank 20 s (sweeps off: 133 / 289 / 302 / 290 / 82 s).

## Flash, RAM

Measured (`make docker-build`, vs tools/size_baseline.txt): text +1624 B (+0.23 %), bss +56 B. The shared judge
replaced the progcheck limit code; new are the engine's test-move intake, the covered-window plan, the session feed and
two Env hooks.

## Wire formats

Unchanged: the 27 cells, `/jr/sp.rec`, `/jr/spd.rec`, the card, the remote forms and the PSC2 blob (the Deep coverage
is not an option bit).

## Results layout

Unchanged (range chips + the PD spec cells, fed by the merged aggregates). Two unified alternatives (one PPS / AVS
check row per range) are rendered for the user in `runs/prog-unify/png`; see "Layout alternatives" below.

## Layout alternatives

Renders (host kit, real Results page behind, 2x): `runs/prog-unify/png/{results,alt_a,alt_b,alt_b_spec}_rot0..3.png`,
from `tests/test_ui_prog_unify_mockups.cpp` (`K2_DUMP=<dir>`, then `tools/panel2png.py`). The fixture has two PPS
ranges (no AVS); an AVS range draws the same row.

- **A, a spec row per range.** The spec section keeps Transitions and the observer; the PPS / AVS checks become one row
  per APDO (`PPS 21V`) with three grouped cells: Level (accuracy, monotonic, track, status), Timing (band / settle,
  PS_RDY, accept), Shape (overshoot, transient). Needs per-APDO aggregates: RAM (3 groups x APDOs) and a new
  journal / card / remote record, so a wire-format change.
- **B, spec on the chip.** Each range row gets a second cell beside its chip (`3.3-21V S | spec`, red with the
  worst group's word on a fail); the spec section keeps only Transitions and the observer (`alt_b_spec`). The chip
  label drops its `PPS` (the row label carries it) to fit two cells. Also needs per-APDO verdicts (smaller: one
  byte per APDO), so a format change as well, smaller than A.

Neither is built; both wait for the user's pick.

## Open decisions

- Which layout (A, B, or keep the current one).
- Fill the coverage the sweep lacks (500 mV PPS steps, AVS 500 / 1000 mV, down-steps, the 20 mV ends) with extra engine
  moves on covered windows, or accept the loss.
- Feed the low sweep part and EPR AVS (needs the stage after PdLow, and EPR limits).
