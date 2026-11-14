# PD Spec Check

**Opt-in build: `K2_PDSPEC=1`** (`make docker-build K2_PDSPEC=1`, `make gate K2_PDSPEC=1`; off by default because of the flash
budget, `include/k2/core/build_config.h`). The host tests always build it.

PD Spec Check runs a no-load conformance pass over the attached PD charger: it steps the charger through every pair of its fixed levels,
up and down, steps its PPS output in 20 mV steps, lets the PPS and EPR keep-alives lapse, sends a Hard Reset and takes its Rd away, and
judges what VBUS and the charger's messages did against the USB PD timing and voltage limits. Each check ends as pass, fail or skip (with
the reason), and the run gets a penalty score, **0 = ideal**. It grew out of PD Stress (appendix below), which renegotiated hard and
counted events; PD Spec Check is polite and judges every measurement against a named limit.

Source of truth: `include/k2/pdspec/` (`plan.hpp` the levels and the walk, `analyze.hpp` the capture analysis, `engine.hpp` the
state machine over an `Env` seam, `result.hpp` the checks, limits, weights and the result blob), the limits in
`include/k2/pd/pd_spec_timing.hpp`, the session glue `include/k2/ui/pd_spec_session.hpp` + `src/ui/ui_pdspec.cpp`, the page
`src/ui/ui_pdspec_page.cpp`, the host side `tools/pd_spec.py`, `tools/pdspec_blob.py` and `k2_remote pd-spec`.

## What it is and is not

- In-protocol only. Every Request goes through the sink's own guards (TX-OK gate, one Request at a time, the monitor ceiling), only
  for advertised levels, never more current than advertised. The keep-alive checks stop sending a keep-alive the source asked for,
  which the spec allows a sink to do (the source answers with a Hard Reset); the sink bounds that silence itself.
- No load. Nothing is drawn from the output; a current above 100 mA for 5 polls stops the run ("load on the output").
- Not a certification tool. The figures are the K2's own measurements from its INA226 (about 4.5 ms per sample) and the transition
  capture; every timed value carries the uncertainty of its sample spacing, and a measurement only fails when the whole bracket lies
  outside the limit.
- One run owns the link. It is refused while another tool holds the ports.

## Families and checks

A run goes through the families in this order; the operator picks any subset (Setup on the K2, `--families` on the host). A family left
out reads "not selected"; every family ends back at a 5 V contract.

| Family | What the K2 does | Checks |
|---|---|---|
| Transitions | one walk over every ordered pair of fixed levels, starting and ending at 5 V (an Euler circuit: 4 levels give 12 steps, 7 give 42, at most 8 levels and 56 steps); EPR fixed levels at or below the monitor ceiling join with the cable proof | `accept_time` `ps_rdy_time` `src_start` `ps_rdy_early` `ps_rdy_low` `rise_time` `fall_time` `overshoot` `accuracy` |
| PPS steps | per SPR PPS APDO (at most 4) three set points (low, middle, high; never under 5 V), each stepped 5 x 20 mV up and 5 back down | `pps_monotonic` `pps_accuracy` `pps_settle` |
| PPS timeout | a PPS contract with the sink's re-Request held back: when does the source Hard Reset | `pps_keepalive` |
| EPR timeout | EPR mode with the sink's EPR_KeepAlive held back (needs EPR, a level at or below the ceiling and the cable proof) | `epr_keepalive` |
| Hard Reset | the K2 sends a Hard Reset from the top fixed level and follows VBUS down, dark, back up and the first capabilities | `hr_safe0v` `hr_recover` `hr_turn_on` `hr_caps` |
| Detach | the K2 takes its Rd away at the top fixed level, follows VBUS to 0 V, puts Rd back and waits for a 5 V contract | `detach_safe0v` `reattach` |

`source_reset` runs over the whole run: a Hard Reset sent by the source where none was due, that is outside a keep-alive wait
(the K2's own Hard Reset is not counted). The family it hits ends there and the run goes on once the source is back at 5 V.

### Limits

Every limit is a named figure of the USB PD R3.2 (or Type-C) specification, or a sum of named figures ("derived"), or a K2 figure where
the specification names none ("inferred"). A check whose whole limit is inferred (`pps_settle`, `detach_safe0v`, `reattach`; `inferred` in
`kDefs`) is measured and reported but never judged: its verdict reads **unverified**, with the value and the count over the inferred
limit, and it adds nothing to the score. The numbers live in `pd_spec_timing.hpp`; the reference point is always one of the K2's own
edges: the Request's TX start, the Accept's or the PS_RDY's RX edge, or the VBUS sample.

| Check | Measured | Limit |
|---|---|---|
| `accept_time` | Request TX start to the Accept | tSenderResponse max (50 ms) + 2 ms for the three frames on the wire (inferred) |
| `ps_rdy_time` | Accept to PS_RDY | tSrcTransReq: 325 ms SPR, 760 ms EPR |
| `src_start` | Accept to VBUS starting to move | no earlier than tSrcTransition min (25 ms) |
| `ps_rdy_early` | down-step: VBUS when PS_RDY arrives | at or under the new level's vSrcNew top (nominal + 5 %) |
| `ps_rdy_low` | up-step: VBUS when PS_RDY arrives | at or over the new level's vSrcNew bottom (nominal - 5 %) |
| `rise_time`, `fall_time` | Accept to VBUS inside vSrcNew | tSrcTransition max + tSrcSettle: 310 ms SPR, 735 ms EPR (derived); a fall that outlasts the capture window is followed live |
| `overshoot` | the peak (up) or the dip (down) after the Accept | inside vSrcNew widened by vSrcValid (0.5 V) |
| `accuracy` | the settled fixed level | inside vSrcNew (+-5 %) |
| `pps_monotonic` | each 20 mV PPS step | moves the requested way or not at all; 8 mV the wrong way is allowed (monitor noise, inferred) |
| `pps_accuracy` | each PPS level | inside vPpsNew (requested +-5 %) |
| `pps_settle` | Accept to the settled PPS level: the first sample from which the mean of the last four samples stays within 10 mV of the final level | tPpsSrcTransSmall (25 ms), taken from the Accept (inferred: the table names the set-point time only): unverified |
| `pps_keepalive` | the last PPS Request to the source's Hard Reset | tPPSTimeout, 12 to 15 s; no Hard Reset by 20 s fails ("not seen") |
| `epr_keepalive` | the last EPR_KeepAlive to the source's Hard Reset | tSourceEPRKeepAlive, 750 to 1000 ms; no Hard Reset by 3 s fails |
| `hr_safe0v` | the K2's Hard Reset to VBUS under vSafe0V (0.8 V) | tPSHardReset + tSafe0V: 685 ms (derived) |
| `hr_recover` | VBUS off time | tSrcRecover, 660 to 1000 ms (1085 to 1425 ms when the reset leaves EPR mode) |
| `hr_turn_on` | VBUS back to vSafe5V (4.75 V) | tSrcTurnOn, 275 ms |
| `hr_caps` | vSafe5V to the first Source_Capabilities | tFirstSourceCap, 250 ms |
| `detach_safe0v` | Rd away to VBUS under vSafe0V | tVBUSOFF (650 ms) + tPDDebounce max (20 ms), both inferred: unverified |
| `reattach` | Rd back to a 5 V contract | 5 s (a K2 figure, inferred): unverified |
| `source_reset` | Hard Resets from the source that were not due | 0 |

A check is judged per measurement and aggregated: count, fails, and the worst value with the limit it was judged against and where it
came from (the PDO pair, the APDO point). Verdict: skip when nothing was measured (with the reason: not offered, one fixed level, no
PPS, no EPR, no EPR cable proof, capture busy, level refused, run stopped, not reached, K2 on the charger, no data, not selected),
fail when any
measurement failed, else pass; a measured check with an inferred limit is unverified (neither pass nor fail).

### Score

Penalty points: `score = sum over the judged checks of weight x fails`, **0 = ideal**; an unverified check adds nothing (its weight
applies once its limit has a source). The weights are **provisional** (`kDefs` in
`result.hpp`), to be settled after runs on several chargers; the result keeps every count, so a run can be rescored later.

| Weight | Checks |
|---|---|
| 1 | `accept_time` `src_start` `pps_settle` (unverified for now: 0) |
| 2 | `ps_rdy_time` `rise_time` `fall_time` `pps_accuracy` `hr_caps` |
| 3 | `ps_rdy_early` `ps_rdy_low` `overshoot` `accuracy` `pps_monotonic` `hr_recover` `hr_turn_on` |
| 5 | `pps_keepalive` `epr_keepalive` `hr_safe0v` `detach_safe0v` (unverified for now: 0) |
| 10 | `reattach` (unverified for now: 0) |
| 20 | `source_reset` |

## Measurement

Each fixed step and each PPS step arms the transition capture (`k2::transcap`, the 1 kHz VBUS ring with the PD edge markers) and
triggers it at the Request with a post window (1.5 s for a fixed step, 300 ms for a PPS step); the analysis runs in place on the frozen
ring. When the capture is held by someone else, the run goes on without it: the times come from the sink's edge stamps and live VBUS
polling, each widened by the monitor's sample age (5 ms) and the poll spacing, and the checks that need the waveform read "capture
busy". A run overwrites the last transition capture (`pd_trace.py transition`).

The PPS settle time is a measurement-floor case. A 20 mV step is two of the monitor's noise widths: single INA226 samples at 1 kHz
scatter by about 3.5 mV (sd) around a steady level and single ones by up to 13 mV. Judged sample by sample against +-10 mV, a step
"settled" at the last noise spike of its 300 ms window: the Anker IQ4 read 183-268 ms for a level that arrived 5 ms after the Accept
(`tests/fixtures/pdspec/iq4_pps_7000_7020.csv`). The settle is therefore judged on the mean of the last four samples (about 2 ms of
lag), and the check stays unverified while its limit is inferred.

## Safety

Enforced by the engine and the session, not by the operator:

1. Only advertised levels at or below the K2's monitor ceiling (36 V on an INA226); EPR levels only with the cable proof, and never
   above the ceiling (48 V is left out on a 36 V K2, "not offered"). A measured VBUS above the ceiling + 5 % stops the run at once,
   and VBUS above the step's own highest level + 10 % + 1 V for 3 polls in a row stops it too ("VBUS over the limit"; a shorter
   overshoot spike is a measurement, not a stop).
2. No load: more than 100 mA on the output for 5 polls stops the run ("load on the output").
3. Refused at start: another tool on the ports ("ports busy": the charger test, PD Stress, a twin run, a trigger, a hold), no power
   monitor, an inline link (a sink behind the K2), no PD source, and a **K2 powered by the charger under test** ("K2 powered by the
   charger"): the Hard Reset and Detach families would cut its own supply.
4. The return to 5 V. Every family ends at a 5 V contract, and every stop (done, stop command, BACK on the K2, the one-call `stop`
   0x4B, the monitor going offline, the loop guard) does the same, in this order: the keep-alives come back, the Rd is
   back on, the capture is released, the sink is asked for PDO 1. The restore is confirmed when VBUS is at or under 5.5 V (12 s at
   most; a line that stays dark for 3 s ends the run as well). An unconfirmed restore is flagged: the K2 footer reads
   "Not back at 5V: unplug" (Error tone), the status and blob carry the flag, `pd_spec.py run` exits 6.
5. Stop rules besides the above: a step not resolved by the sink in 10 s ("step stuck"), no contract at the start ("no PD contract"),
   a source not back after a reset in 10 s ("source did not come back"), an inline link appearing ("link inline"), and the 20 min run cap.
6. Time bound: a run stops itself after 20 minutes ("run time cap").

### Reboot safety

The result lives in RAM only; there is no journal and no resume. A host-powered K2 does not reboot when the charger cuts VBUS (that is
why a self-powered K2 is refused). If the K2 reboots during a run anyway (a fault, a brown-out on the HID cable), it comes back in its boot mode, on a
default boot the passive one: no Rd, no Request, nothing silenced, and nothing of the run is kept (no journal, no BKP mark). The charger sees a detach and drops VBUS to 0 V; the next sink attach starts a
fresh 5 V contract. Nothing is left above 5 V, and the run's result is lost. A host script detects this from the K2's uptime against its
own wall clock: `pd_spec.py run` pings the K2 at every poll and exits 7 when the uptime goes back or the K2 reads idle after the
start, and it saves `host_wall_s` and the uptime at the start and the end with every result.

### Getting a contract first, and after the run

A run needs a PD contract on a host-powered K2: `k2_remote pd attach` (5 V) or `k2_remote pd-reread` just before the start. The
hold either one makes does not count as "ports busy"; a run started on a re-read's attach takes it over (its release timer is
cancelled). From the start the run presents its own Rd (`fusb302::rd_for_run(true)`, as a charger-test START does): a run
that ran on the contract alone lost its Rd 300 ms after a Hard Reset ended that contract, and the source never came back (Anker
IQ4, hardware 2026-11-14: `pd attach` on a 5 V contract still lingering from the previous run). At the end the run releases that
Rd (as PD Stress does since its Rd fix), so the CC lines go back to the boot mode: on a passive boot the charger then sees no sink
and leaves VBUS at 0 V, and a second run needs `pd attach` again (hardware 2026-11-14: "start refused: no PD source" without it).

## Use

On the K2: Tools > PD Spec Check (or the Charger place's "Spec check" row).

1. **Setup**: the six families as checkboxes (each with a one-line note), Done; "Last result" once a run exists.
2. **Review**: what will run (checks, charger, ceiling, EPR, output) and START with the two-press confirm. A refusal lands in the
   footer ("No PD source", "K2 runs on this charger", ...).
3. **Running**: family and item, elapsed time, a progress bar, the live VBUS and the fail and score counts. BACK stops; the footer
   says "Stopped" once the charger is back at 5 V.
4. **Results**: the score, the pass / fail / skip tallies (an unverified check is in none of them) and one ResultGrid row per check (worst value or the skip reason in the
   cell). OK opens a check's **Detail**: verdict, worst value, limit, count, fails, where (PDO pair or APDO point) and penalty.

While a run goes on, any other page shows a one-line footer ("PD Spec: Transitions 12/56"), and the session block names the owner
(`BusyWhy::PdSpec`, 12; `pd-spec` in `k2_remote` and the web page). The dead-man lease leaves a run alone (`Ui::run_busy`): a host
that goes away leaves the run to finish within its 20 min cap.

From the computer:

```
python3 tools/pd_spec.py run --charger NAME --confirm [--families transitions,pps-steps,...] [--wait 1500]
python3 tools/pd_spec.py status | stop | report runs/pd-spec-NAME-STAMP/result.json [--json]
python3 tools/k2_remote.py pd-spec start --confirm [--families all|0x3f|transitions,detach]
python3 tools/k2_remote.py pd-spec status | result | stop        # --json for the raw values
```

`run` saves `result.json` (the raw blob in hex and its decode), `report.txt` and `stream.log` (the `[PDSPEC]` log lines) under
`runs/pd-spec-<charger>-<stamp>/`. The family names are `transitions`, `pps-steps`, `pps-timeout`, `epr-timeout`, `hard-reset`,
`detach`.

### Remote protocol

Command 0x5A (`Cmd::PdSpec`, `include/k2/core/remote_proto.hpp`): op 0 status, 1 start (args: families mask, flags; bit 0 of the flags
is the confirm, required: NeedConfirm without it; a mask beyond 0x3F or an unknown flag is BadArg; Busy when another tool owns the
ports, other refusals in the status block's `start` byte), 2 read (offset u16; reply offset, total, n, data, 48-byte chunks), 3 stop
(never refused). The 17-byte status block: state (0 idle, 1 running, 2 finished), stop, start, family, item, items, families, fails,
passes, skips (unverified checks are counted in none of the three; the blob has them), flags (bit 0: return to 5 V not confirmed),
score u16, elapsed s u16, blob length u16.

The result blob (`result.hpp`, magic `PSC1`): a 16-byte header (state, stop, transition and PPS record counts, score, elapsed,
flags), 21 x 20-byte check aggregates (verdict, skip reason, where, count, fails, worst, low and high limit), 32 bytes of timer
numbers (PPS and EPR timeout, the Hard Reset and detach times), then one 20-byte record per fixed transition (Accept, PS_RDY, start
and band times, VBUS at PS_RDY, peak, final, sample gap, flags) and one 14-byte record per PPS set point. The K2 never holds the blob:
`BlobReader` streams it from the engine's state.

## Phase 2 (design hooks only, nothing built)

- **AVS steps**: the SPR and EPR AVS windows in 100 mV steps, judged against the AVS small / large step times already in
  `pd_spec_timing.hpp`; a new family and checks appended to `Family`, `Check` and `kDefs`.
- **Large PPS steps** (tPpsSrcTransLarge) and PPS current-limit reporting (needs a load: out of scope for a no-load tool, only the
  advertised flags can be read).
- **Source message content**: Source_Capabilities_Extended, Status and PPS_Status answered within tSenderResponse, with fields that
  agree with the capabilities; Soft_Reset recovery timing; the first capabilities after a cold attach.
- **Hooks in place**: the `Family` and `Check` enums, `kDefs` and `Skip` are append-only (the blob carries them by number; a layout
  change bumps the magic to `PSC2`, which `pdspec_blob.py` refuses until taught); the `Env` seam takes new actions without touching
  the firmware binding of the old ones; `pdspec_blob.py` reads every name and weight from the headers, so a new check appears in the
  report without a host change.
- **Recorded fixtures**: the hardware runs save the raw blob; saved blobs go to `tests/fixtures/pdspec/` and the decoder tests read them.

## Limits of the method

- One source port, no load: power reallocation on a multi-port charger cannot be provoked, and a PPS supply that needs a load to
  regulate shows as "level refused".
- The INA226 samples every ~4.5 ms; timings under ~10 ms rest on the PD edge stamps, not on VBUS.
- The EPR keep-alive and Hard Reset reference points are the K2's own stamps of its last message; the figures marked inferred above are
  the K2's reading of the specification, to be checked against more chargers.

## Appendix: PD Stress (parked, `K2_PDSTRESS=1`)

**Compiled out by default since 2026-11-12** (`K2_PDSTRESS=0`; build it back with `make K2_PDSTRESS=1` or
`make docker-build K2_PDSTRESS=1`). It found no charger fault, only its own Soft_Reset bug (ROADMAP "Current bugs").
The host tests keep building it.

PD Stress renegotiates a charger as hard as USB-PD allows and scores what the charger did with it. It is a separate tool, not a charger-test unit and
not on the report card: the charger test is polite (one pass over what the source advertises), PD Stress is aggressive but stays inside the protocol.
A clean charger scores 0.00. Source of truth: `include/k2/pdstress/` (engine, plan, score, dist, result, journal), the session glue in
`include/k2/ui/pd_stress_session.hpp` and `src/ui/ui_pdstress.cpp`, the host script `tools/pd_stress.py`.

### What it is and is not

- A stress test of PD renegotiation: Requests back to back, mid-transition, between levels, with PPS and AVS steps, a Soft_Reset loop, a PPS request
  below 5 V and a Capability Mismatch bit with no mismatch. All legal PD; no timing violation, no malformed message, no Request before the sink may send.
- No load. Nothing is drawn from the output. A source that refuses a level without a load (some PPS supplies) is recorded as "needs load" and the
  sequence ends; the tool never adds load.
- Not a certification tool. It cannot see a second port's power reallocation (the K2 has one source port), and it cannot see captive-cable sources
  from other sources (see "Limits").
- One run owns the link. It is refused while the charger test or another session holds the ports, when no PD source has advertised (start result
  "no PD source"), and when the link is inline. EPR is opt-in and needs the cable proof of the charger test (P9).

### Safety rules (these are enforced by the engine, not by the operator)

1. Only advertised levels: a fixed PDO, or a PPS/AVS window inside what Source_Capabilities lists. Never more current than advertised.
2. No load on the output. IBUS above `kLoadAbortMa` (100 mA) for 5 polls ends the run (stop "load on the output").
3. The monitor ceiling is the voltage cap: Requests stay at or below 36 V on an INA226 (the per-device ceiling), and a measured VBUS above
   `pd::overcap_abort_mv(cap)`, or settled more than 10 % above the target, aborts the run (stop "VBUS over the limit").
4. EPR only with the cable proof (P9, opt-in), never by default. 48 V is skipped on a 36 V K2.
5. Charger quirks found here are written as behaviour patterns into `docs/CHARGER_QUIRKS.md`, never as a VID:PID table; behaviour is detected by
   signature.
6. `stop` is never refused: any host can end a run at any time and the K2 releases to its normal state. The one-call `stop` (HID `Cmd::Stop`
   0x4B, the shell `stop`, `k2_remote stop`) ends a PD Stress run exactly like a charger test (`Ui::cancel_runs`) and reports it in the "test" bit
   of its mask; a run that waits to resume after a reboot is dropped. The dead-man lease treats a run like a charger test as well: while one runs the
   lease ends quietly and does not stop it (`Ui::run_busy`), so a host that goes away leaves the run to finish on its own caps (900 s, 1200 transitions).
   The session block names the owner with `BusyWhy::PdStress` (10, appended after `twin-run` 9; `pd-stress` in `k2_remote` and the web page).

### Sequences

Each sequence is an independent block with its own counters, cycle count and stop rules. The default mask runs P1 to P8 and P10 to P12; P9 (EPR) is
opt-in. `--mask` is a bit mask by sequence id (P1 = bit 0). `--cycles` scales every sequence's cycle count in percent (0 = 100); `--pace` overrides
the per-sequence pacing (0 = as listed, 1 = all Settled, 2 = all Ready).

| Id | Name | What it sends | Cycles | Pacing | Notes |
|---|---|---|---|---|---|
| P1 | `ladder` | every fixed level up, then down | 10 | Settled | needs 2 or more fixed levels |
| P2 | `slam` | 5 V and the top level alternately | 40 | Settled first half, Ready second half | the second half is the hard one: the next Request 100 ms after PS_RDY |
| P3 | `same` | the same Request repeated at 9 V, then at the top level | 50 | Ready (the first Request of each block is Settled) | a source must accept a repeat of its own contract |
| P4 | `reannounce` | Get_Source_Cap every 500 ms inside the contract | 20 | Spaced (500 ms) | counts Source_Capabilities nobody asked for |
| P5 | `fixpps` | fixed level and PPS at the same voltage alternately | 10 | Settled | PPS; "needs load" ends it |
| P6 | `ppssteps` | PPS in steps of 20, 100, 500, 2000 mV | 50 | Ready | PPS; "needs load" ends it |
| P7 | `avssteps` | SPR AVS in steps of 100, 1000, 2000 mV | 30 | Ready | 30 steps per size |
| P8 | `current` | half the advertised current and the full current at one voltage | 10 | Settled | needs 1 A or more advertised |
| P9 | `epr` | EPR 5 V / 20 V and 28 V alternately | 5 | Settled | opt-in, cable proof required |
| P10 | `softreset` | repeated Soft_Reset sent by the K2 | 10 | Settled | the source must answer, re-advertise and be back in a 5 V contract within 6 s |
| P11 | `ppslow` | PPS requests below 5 V | 10 | Ready | PPS; a request below 5 V is legal only where the PPS window allows it; skipped on a self-powered K2 (it would brown out) and when the PPS window starts at 5 V or more |
| P12 | `mismatch` | a Request with the Capability Mismatch bit set and no mismatch | 10 | Settled | legal: the sink may set it, the source should still grant a valid request |

Pacing:

- **Settled**: the next Request goes out when the contract is in place, VBUS is inside the +-5 % band and there has been 1000 ms of quiet.
- **Ready**: the next Request goes out 100 ms after PS_RDY, whatever VBUS is doing, and only at SinkTxOK (the Rp the source presents says the sink
  may send). This is the aggressive pacing; it is inside the spec (the sink may Request as soon as it has a contract).
- **Spaced**: 500 ms between messages, used for Get_Source_Cap.

### Stop rules

A run ends on the first of: `Done` (every selected sequence finished), `Cancelled` (stop), the run-time cap (`--max-s`, default 900 s), the transition
cap (`--max-trans`, default 1200), a second Hard Reset from the source, a second Hard Reset sent by the K2, `Overvoltage`, `Load`, `NoRecovery` (VBUS
below 1000 mV and not back above 4000 mV within 10 s: the charger needs a replug), `Stuck` (a step not resolved in 10 s, or the Request kept busy 3 s),
`NoPd` (no contract in 10 s, or the 5 V Request refused), `Inline` (a cable inline), `TooManyLosses` (the 4th reboot of a self-powered K2). Two
consecutive refusals of a PPS level end that sequence ("needs load"); one Reject never drops a PPS level.

### Score

Penalty, **0 = ideal**. Every event the run sees has one weight; `score = total points x 100 / max(transitions, 10)`, so the unit is penalty points per
100 transitions, stored as hundredths in the result (`score_cp`, 1234 = 12.34). A transition is a Request, a Get_Source_Cap or a Soft_Reset exchange that
the K2 sent. The denominator floor of 10 stops one event in a handful of transitions from reading as hundreds of points.

The weights are **provisional** (operator decision 2026-11-11, "provisional, see docs/PD_STRESS.md"): starting values, to be settled after validation
runs on several chargers.

| Event (report name) | Weight | Counted when |
|---|---|---|
| `accept_late` | 1 | Request to Accept slower than the spec limit |
| `psrdy_late` | 1 | Accept to PS_RDY slower than tPSTransition |
| `psrdy_oob` | 2 | PS_RDY arrived while VBUS was still outside the +-5 % band |
| `wait` | 2 | a Wait from the source for an advertised level |
| `reject` | 2 | a Reject for an advertised level |
| `caps_unsol` | 2 | Source_Capabilities nobody asked for, inside the contract |
| `vbus_band` | 3 | VBUS overshoot, or out of the band after the settle time |
| `soft_reset` | 5 | Soft_Reset received from the source |
| `hard_reset` | 20 | Hard Reset received from the source |
| `dropout` | 30 | VBUS fell below the dropout line during the run |
| `contract_lost` | 30 | the contract ended without a Hard Reset from the source (timeout, no GoodCRC, a K2-sent Hard Reset) |
| `no_recovery` | 100 | VBUS did not come back without a replug; the run stops here |

The table is one constexpr array, `kWeights` in `include/k2/pdstress/score.hpp`. Events are append-only (wire order).

#### Retuning the weights

1. Run on the hardware; the result keeps raw counters (per sequence, per event kind), so nothing is lost by choosing the weights later.
2. Rescore saved results on the host, no rebuild: put `{"reject": 4, "dropout": 50}` into `w.json` and run
   `python3 tools/pd_stress.py rescore --weights w.json runs/pd-stress-*/result.json`. It prints the firmware's score and the new score side by
   side, per charger, and the table of each event's points. An unknown event name is an error (a typo must not score nothing).
3. When the numbers read right across the chargers on hand, copy the weights into `kWeights` and rebuild. `tests/test_pd_stress.py` parses
   `score.hpp` for the defaults of `pd_stress.py` and compares them with the compiled table, so the host and the firmware cannot drift.

### Distributions

Per sequence the result keeps four histograms of 8 bins (min, max and the bin counts): Accept latency (0.1 ms), PS_RDY latency (0.1 ms), time to enter
the +-5 % band (ms) and the largest overshoot (mV). Median and p95 are bin-resolution. They are shown in the report, not scored.

**Suspect timestamps.** An Accept latency under `kSuspectAcceptUs` (300 us) cannot be real (a GoodCRC and an Accept in flight take longer), so it means
the FUSB302 edge stamp was not the Request's TX_START. The count is in the result header (`stamp_suspect`) and in the report; non-zero means the
latency distributions of that run are not to be trusted.

### Reboot budget and resume (self-powered K2)

A K2 powered by the charger under test dies when the charger cuts VBUS. PD Stress then writes a journal (one small file, `journal.hpp`) at every
sequence boundary on a quiet link and, after the reboot, resumes by itself once VBUS has been steady:

- a loss is counted against the running sequence (`contract_lost` + 1); the sequence is repeated once and given up after the second loss
  (`kMaxSeqLosses` = 2); the run ends at the 4th reboot (`kMaxRunLosses` = 4, stop "too many K2 reboots");
- a journal only resumes on the same PDO list (a CRC of the source capabilities), never when the run is finished, and not with BACK held at boot;
- the distributions and the counters since the last boundary save are lost with the reboot; the counters otherwise carry on;
- a host-powered K2 never journals (a charger cut does not reboot it);
- the K2 presents no Rd of its own: a resume needs the Rd the boot already has, otherwise the 60 s wait ends and the journal is dropped.

`tools/pd_stress.py run` tolerates the K2's USB going away and coming back while it waits.

### Shell usage

Everything is reachable over the remote link; the screen only shows a one-line footer on whatever page is up (`PD Stress: seq 3 of 12`).

```
python3 tools/pd_stress.py run --charger RG100 --confirm              # start, wait, save result.json, report.txt, stream.log
python3 tools/pd_stress.py run --charger RG100 --confirm --mask 0x3   # only P1 and P2
python3 tools/pd_stress.py status | stop | report result.json | weights
python3 tools/pd_stress.py rescore [--weights w.json] [--json] a/result.json b/result.json
python3 tools/k2_remote.py pd-stress start --confirm [--mask m --cycles pct --pace 0..2 --max-s s --max-trans n]
python3 tools/k2_remote.py pd-stress status | result | stop
```

The CDC shell has the same: `pdstress start [options] confirm | status | result [off] | stop`. The remote command id is 0x55 (`CMD_PD_STRESS`):
ops 0 status, 1 start, 2 read result (48-byte chunks), 3 stop. Start needs the confirm flag.

A run started on the attach of a lab `pd-reread` (the usual way to get a contract on a brick before the run) takes that attach over
(`fusb302::reread_take_over()` in `start_run`): the re-read's 30 s release timer is cancelled and the Rd and contract belong to the run.
The release never fires while a run owns the line, so a run cannot be cut by it (hardware 2026-11-11 saw it at a Soft_Reset step). After
the run the attach stays up until `pd-reread --abort` (refused with busy while the run lasts) or a detach.

### Result

`pd_stress.py run` saves `result.json` (the raw blob decoded, with the charger name and the start time), `report.txt` (header, each sequence with its
counters and distributions, the score per event kind) and `stream.log` under `runs/pd-stress-<charger>-<stamp>/`. The firmware blob
(`include/k2/pdstress/result.hpp`) is a 104-byte header (stop reason, options, transitions, elapsed, score, per-event points, sequence order and
flags) followed by the per-sequence records. It is raw material: `rescore` reads only this.

### Limits

- The K2 has no load and one source port: PPS supplies that need a load to regulate refuse some levels, and a charger with two ports that reallocates
  power when the other port changes cannot be provoked here.
- A K2 powered by the charger can observe a cut only as a reboot, so the dropout and no-recovery measurements have less resolution there.
- Starting PD Stress invalidates the last charger test's deep-sweep plot (the record buffer is shared).
- The score is only as good as the weights; they are provisional.

### First hardware pass

First run on a charger known to behave (PD 5/9/15/20 V 3 A + PPS 5-21 V), K2 powered from the PC, on its top plug directly in the source, nothing on
the output. Enable one sequence at a time first with `--mask`: P1 (0x1), then P3 (0x4), then P2 (0x2), P4 (0x8), P6 (0x20); then P10 to P12
(0x200, 0x400, 0x800); P5 and P7 only for chargers with PPS or AVS; P9 only for a cable-proof run. A clean charger scores 0.00. The full default
set is about 700 transitions, which the simulation puts at some minutes, not seconds (reasoned from the sim, not measured).
