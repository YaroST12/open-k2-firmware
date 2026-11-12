# PD Stress

**Compiled out by default since 2026-11-12** (`K2_PDSTRESS=0`; build it back with `make K2_PDSTRESS=1` or
`make docker-build K2_PDSTRESS=1`). It found no charger fault, only its own Soft_Reset bug (ROADMAP "Current bugs").
The host tests keep building it.

PD Stress renegotiates a charger as hard as USB-PD allows and scores what the charger did with it. It is a separate tool, not a charger-test unit and
not on the report card: the charger test is polite (one pass over what the source advertises), PD Stress is aggressive but stays inside the protocol.
A clean charger scores 0.00. Source of truth: `include/k2/pdstress/` (engine, plan, score, dist, result, journal), the session glue in
`include/k2/ui/pd_stress_session.hpp` and `src/ui/ui_pdstress.cpp`, the host script `tools/pd_stress.py`.

## What it is and is not

- A stress test of PD renegotiation: Requests back to back, mid-transition, between levels, with PPS and AVS steps, a Soft_Reset loop, a PPS request
  below 5 V and a Capability Mismatch bit with no mismatch. All legal PD; no timing violation, no malformed message, no Request before the sink may send.
- No load. Nothing is drawn from the output. A source that refuses a level without a load (some PPS supplies) is recorded as "needs load" and the
  sequence ends; the tool never adds load.
- Not a certification tool. It cannot see a second port's power reallocation (the K2 has one source port), and it cannot see captive-cable sources
  from other sources (see "Limits").
- One run owns the link. It is refused while the charger test or another session holds the ports, when no PD source has advertised (start result
  "no PD source"), and when the link is inline. EPR is opt-in and needs the cable proof of the charger test (P9).

## Safety rules (these are enforced by the engine, not by the operator)

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

## Sequences

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

## Stop rules

A run ends on the first of: `Done` (every selected sequence finished), `Cancelled` (stop), the run-time cap (`--max-s`, default 900 s), the transition
cap (`--max-trans`, default 1200), a second Hard Reset from the source, a second Hard Reset sent by the K2, `Overvoltage`, `Load`, `NoRecovery` (VBUS
below 1000 mV and not back above 4000 mV within 10 s: the charger needs a replug), `Stuck` (a step not resolved in 10 s, or the Request kept busy 3 s),
`NoPd` (no contract in 10 s, or the 5 V Request refused), `Inline` (a cable inline), `TooManyLosses` (the 4th reboot of a self-powered K2). Two
consecutive refusals of a PPS level end that sequence ("needs load"); one Reject never drops a PPS level.

## Score

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

### Retuning the weights

1. Run on the hardware; the result keeps raw counters (per sequence, per event kind), so nothing is lost by choosing the weights later.
2. Rescore saved results on the host, no rebuild: put `{"reject": 4, "dropout": 50}` into `w.json` and run
   `python3 tools/pd_stress.py rescore --weights w.json runs/pd-stress-*/result.json`. It prints the firmware's score and the new score side by
   side, per charger, and the table of each event's points. An unknown event name is an error (a typo must not score nothing).
3. When the numbers read right across the chargers on hand, copy the weights into `kWeights` and rebuild. `tests/test_pd_stress.py` parses
   `score.hpp` for the defaults of `pd_stress.py` and compares them with the compiled table, so the host and the firmware cannot drift.

## Distributions

Per sequence the result keeps four histograms of 8 bins (min, max and the bin counts): Accept latency (0.1 ms), PS_RDY latency (0.1 ms), time to enter
the +-5 % band (ms) and the largest overshoot (mV). Median and p95 are bin-resolution. They are shown in the report, not scored.

**Suspect timestamps.** An Accept latency under `kSuspectAcceptUs` (300 us) cannot be real (a GoodCRC and an Accept in flight take longer), so it means
the FUSB302 edge stamp was not the Request's TX_START. The count is in the result header (`stamp_suspect`) and in the report; non-zero means the
latency distributions of that run are not to be trusted.

## Reboot budget and resume (self-powered K2)

A K2 powered by the charger under test dies when the charger cuts VBUS. PD Stress then writes a journal (one small file, `journal.hpp`) at every
sequence boundary on a quiet link and, after the reboot, resumes by itself once VBUS has been steady:

- a loss is counted against the running sequence (`contract_lost` + 1); the sequence is repeated once and given up after the second loss
  (`kMaxSeqLosses` = 2); the run ends at the 4th reboot (`kMaxRunLosses` = 4, stop "too many K2 reboots");
- a journal only resumes on the same PDO list (a CRC of the source capabilities), never when the run is finished, and not with BACK held at boot;
- the distributions and the counters since the last boundary save are lost with the reboot; the counters otherwise carry on;
- a host-powered K2 never journals (a charger cut does not reboot it);
- the K2 presents no Rd of its own: a resume needs the Rd the boot already has, otherwise the 60 s wait ends and the journal is dropped.

`tools/pd_stress.py run` tolerates the K2's USB going away and coming back while it waits.

## Shell usage

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

## Result

`pd_stress.py run` saves `result.json` (the raw blob decoded, with the charger name and the start time), `report.txt` (header, each sequence with its
counters and distributions, the score per event kind) and `stream.log` under `runs/pd-stress-<charger>-<stamp>/`. The firmware blob
(`include/k2/pdstress/result.hpp`) is a 104-byte header (stop reason, options, transitions, elapsed, score, per-event points, sequence order and
flags) followed by the per-sequence records. It is raw material: `rescore` reads only this.

## Limits

- The K2 has no load and one source port: PPS supplies that need a load to regulate refuse some levels, and a charger with two ports that reallocates
  power when the other port changes cannot be provoked here.
- A K2 powered by the charger can observe a cut only as a reboot, so the dropout and no-recovery measurements have less resolution there.
- Starting PD Stress invalidates the last charger test's deep-sweep plot (the record buffer is shared).
- The score is only as good as the weights; they are provisional.

## First hardware pass

First run on a charger known to behave (PD 5/9/15/20 V 3 A + PPS 5-21 V), K2 powered from the PC, on its top plug directly in the source, nothing on
the output. Enable one sequence at a time first with `--mask`: P1 (0x1), then P3 (0x4), then P2 (0x2), P4 (0x8), P6 (0x20); then P10 to P12
(0x200, 0x400, 0x800); P5 and P7 only for chargers with PPS or AVS; P9 only for a cable-proof run. A clean charger scores 0.00. The full default
set is about 700 transitions, which the simulation puts at some minutes, not seconds (reasoned from the sim, not measured).
