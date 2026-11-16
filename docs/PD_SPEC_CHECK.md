# PD Spec Check

**Opt-in build: `K2_PDSPEC=1`** (`make docker-build K2_PDSPEC=1`, `make gate K2_PDSPEC=1`; off by default because of the flash
budget, `include/k2/core/build_config.h`). The host tests always build it.

PD Spec Check runs a no-load conformance pass over the attached PD charger: it steps the charger through every pair of its fixed levels,
up and down, sweeps its PPS and SPR AVS outputs (PPS 20 / 100 / 500 mV steps, AVS 100 / 500 / 1000 mV, then the slams from the
window's minimum to its maximum and back) and asks Get_PPS_Status, lets the PPS and EPR keep-alives lapse, sends a Hard Reset and takes its Rd away, and
judges what VBUS and the charger's messages did against the USB PD timing and voltage limits. Each check ends as pass, fail or skip (with
the reason), and the run gets a penalty score, **0 = ideal**. It grew out of PD Stress (appendix below), which renegotiated hard and
counted events; PD Spec Check is polite and judges every measurement against a named limit.

Source of truth: `include/k2/pdspec/` (`plan.hpp` the levels and the walk, `analyze.hpp` the capture analysis, `engine.hpp` the
state machine over an `Env` seam, `result.hpp` the checks, limits, weights and the result blob), the limits in
`include/k2/pd/pd_spec_timing.hpp`, the session glue `include/k2/ui/pd_spec_session.hpp` + `src/ui/ui_pdspec.cpp`, the Deep charger-test section
`include/k2/chargertest/spec_section.hpp`, the host side `tools/pd_spec.py`, `tools/pdspec_blob.py` and `k2_remote pd-spec`. There is no
spec-check page on the K2 any more (removed 2026-11-16): the checks run as a stage of a Deep charger test, right after its PD unit ("Deep run" below).

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
| PPS/AVS | per SPR PPS and SPR AVS APDO (at most 4 windows) one pass per step size and one of slams, each from the window's minimum up to its maximum and back down (see "Programmable sweep" below); a PPS pass asks Get_PPS_Status at its top and at its end | `pps_monotonic` `pps_accuracy` `pps_settle` `pps_band` `pps_ps_rdy` `pps_overshoot` `pps_transient` `pps_track` `pps_status` `pps_status_time`; the `avs_*` set (no status) |
| PPS timeout | a PPS contract with the sink's re-Request held back: when does the source Hard Reset | `pps_keepalive` |
| EPR timeout | EPR mode with the sink's EPR_KeepAlive held back (needs EPR, a level at or below the ceiling and the cable proof) | `epr_keepalive` |
| Hard Reset | the K2 sends a Hard Reset from the top fixed level and follows VBUS down, dark, back up and the first capabilities | `hr_safe0v` `hr_recover` `hr_turn_on` `hr_caps` |
| Detach | the K2 takes its Rd away at the top fixed level, follows VBUS to 0 V, puts Rd back and waits for a 5 V contract | `detach_safe0v` `reattach` `reattach_caps` |

`source_reset` runs over the whole run: a Hard Reset sent by the source where none was due, that is outside a keep-alive wait
(the K2's own Hard Reset is not counted). The family it hits ends there and the run goes on once the source is back at 5 V.

### Limits

Every limit is a named figure of the USB PD R3.2 / R3.3 or USB Type-C specification, or a sum of named figures ("derived"), or a K2
figure where the specification names none ("inferred"). Only rows tagged verified in the two spec references are used:
`docs/PD_PPS_AVS_SPEC_REFERENCE.md` (PPS and AVS) and `docs/PD_TYPEC_BC12_SPEC_REFERENCE.md` (Type-C attach and detach). A check
whose whole limit is inferred (now only `pps_settle`; `inferred` in `kDefs`) is measured and reported but never judged: its verdict
reads **unverified**, with the value and the count over the inferred limit, and it adds nothing to the score. The numbers live in
`pd_spec_timing.hpp`; the reference point is always one of the K2's own edges: the Request's TX start, the Accept's or the PS_RDY's RX
edge, or the VBUS sample.

The PPS and AVS transition times start, in the specification, at the last bit of the GoodCRC that acknowledged the Accept. The K2 times
them from the Accept's RX edge, which comes before that GoodCRC: its figure is longer than the specification's by under 1 ms, so a pass
is sure and a fail can be over-reported by that much at most (conservative).

| Check | Measured | Limit |
|---|---|---|
| `accept_time` | Request TX start to the Accept | tReceiverResponse (15 ms) + 2 ms on the wire, derived from the frame lengths at 300 kbit/s: Request 189 bits 0.63 ms, the K2's tTransmit under 0.2 ms, GoodCRC 149 bits 0.50 ms, the Accept 0.50 ms, 1.8 ms rounded up. Conservative (the Anker IQ4 reads 7.3-7.6 ms). The diagnostics' late-Accept flag and PD Stress keep tSenderResponse (50 ms + 2) |
| `ps_rdy_time` | Accept to PS_RDY | tSrcTransReq: 325 ms SPR, 760 ms EPR |
| `src_start` | Accept to VBUS starting to move | no earlier than tSrcTransition min (25 ms) |
| `ps_rdy_early` | down-step: VBUS when PS_RDY arrives | at or under the new level's vSrcNew top (nominal + 5 %) |
| `ps_rdy_low` | up-step: VBUS when PS_RDY arrives | at or over the new level's vSrcNew bottom (nominal - 5 %) |
| `rise_time`, `fall_time` | Accept to VBUS inside vSrcNew | tSrcTransition max + tSrcSettle: 310 ms SPR, 735 ms EPR (derived); a fall that outlasts the capture window is followed live |
| `overshoot` | the peak (up) or the dip (down) after the Accept | inside vSrcNew widened by vSrcValid (0.5 V) |
| `accuracy` | the settled fixed level | inside vSrcNew (+-5 %) |
| `pps_monotonic`, `avs_monotonic` | each measured move: the new settled level against the last one | same or higher on a move up, same or lower on a move down; 8 mV the wrong way is allowed (monitor noise, inferred) |
| `pps_accuracy`, `avs_accuracy` | each settled level | inside vPpsNew (requested +-5 %) / vAvsNew (requested +-5 %) |
| `pps_settle` | 20 mV steps: Accept to the settled level, the first sample from which the mean of the last four samples stays within 10 mV of the final level | tPpsSrcTransSmall (25 ms): unverified (a 20 mV step is under the meter's resolution, see Measurement) |
| `pps_band`, `avs_band` | 100 / 500 mV PPS steps, all AVS steps, the slams: Accept to VBUS inside vPpsNew / vAvsNew | tPpsSrcTransSmall 25 ms for a step up to vPpsSmallStep (500 mV), tPpsSrcTransLarge 275 ms above; tAvsSrcTransSmall 50 ms up to vAvsSmallStep (1000 mV), tAvsSrcTransLarge 325 ms (SPR) above |
| `pps_ps_rdy`, `avs_ps_rdy` | Accept to PS_RDY on every measured move | the same small / large limit as the band (the source sends PS_RDY once the output is in vPpsNew / vAvsNew) + 2 ms on the wire: the K2 stamps PS_RDY once it is received, so the reading carries the PS_RDY frame (149 bits, 0.50 ms), the K2's turnaround (under 0.2 ms) and its GoodCRC (0.50 ms), 1.2 ms rounded up as for `accept_time` (derived, `kPsRdyWireOverheadMs`): 27 ms on a small PPS step, 277 ms on a large one. The band check keeps the bare limit (a meter sample, not a message) |
| `pps_overshoot`, `avs_overshoot` | the peak (up) or the dip (down) after the Accept | inside vPpsNew widened by vPpsValid (100 mV) / vAvsNew widened by vAvsValid (500 mV), on the side the move goes to |
| `pps_transient`, `avs_transient` | after VBUS first entered the band: the longest stretch it left it again | tPpsTransient 150 ms (load under 60 mA, which no load is) / tAvsTransient 5 ms |
| `pps_status` | each Get_PPS_Status answer (PPS passes only, never during the PPS timeout check): fault bits | 0. Output Voltage within +-3 % of the requested level (rounded to 20 mV) or 0xFFFF; Output Current 0..3 (no load, 50 mA units) or 0xFF; Real Time Flags bit 0 = 0, OMF = 0, bits 7..4 = 0 (PTF may read 0 or 1). Not_Supported and no answer are faults (Get_PPS_Status is required of a PPS source) |
| `pps_status_time` | Get_PPS_Status TX start to the PPS_Status | tReceiverResponse (15 ms) + 2 ms on the wire, as `accept_time` |
| `pps_keepalive` | the last PPS Request's TX start (the Accept's time less the Request-to-Accept span; the specification starts at that Request's GoodCRC, under 1 ms later) to the source's Hard Reset | tPPSTimeout, 12 to 15 s; no Hard Reset by 20 s fails ("not seen") |
| `epr_keepalive` | the last EPR_KeepAlive to the source's Hard Reset | tSourceEPRKeepAlive, 750 to 1000 ms; no Hard Reset by 3 s fails |
| `hr_safe0v` | the K2's Hard Reset to VBUS under vSafe0V (0.8 V) | tPSHardReset + tSafe0V: 685 ms (derived) |
| `hr_recover` | VBUS off time | tSrcRecover, 660 to 1000 ms (1085 to 1425 ms when the reset leaves EPR mode) |
| `hr_turn_on` | VBUS back to vSafe5V (4.75 V) | tSrcTurnOn, 275 ms |
| `hr_caps` | vSafe5V to the first Source_Capabilities | tFirstSourceCap, 250 ms |
| `detach_safe0v` | Rd away to VBUS under vSafe0V | tSRCDisconnect (20 ms) + tVBUSOFF (650 ms) = 670 ms (Type-C, derived) |
| `reattach` | Rd back to VBUS at vSafe5V | tCCDebounce max (200 ms) + tBulkDischarge (275 ms from a contract at or under 21 V, 700 ms above) + tVBUSON (275 ms) (Type-C, derived) |
| `reattach_caps` | vSafe5V after the re-attach to the first Source_Capabilities | tFirstSourceCap, 250 ms |
| `pps_track`, `avs_track` | per programmable pass of classes 0-2, the first upward step whose settled level fell behind its request: how far the offset (request - settled) grew over that step | min(step + 50, 150) mV, so a fold (settled falls by more than 50 mV on a move up) and a track break (offset jumps by more than 150 mV between neighbours) both fail; inferred noise margins, see "Programmable sweep" |
| `source_reset` | Hard Resets from the source that were not due | 0 |

A check is judged per measurement and aggregated: count, fails, and the worst value with the limit it was judged against and where it
came from (the PDO pair; for a programmable check the APDO, the step size, the direction and the level asked for, "APDO 5 500 mV down
to 5.0 V"). Verdict: skip when nothing was measured (with the reason: not offered, one fixed level, no PPS, no EPR, no EPR cable proof,
capture busy, level refused, run stopped, not reached, K2 on the charger, no data, not selected, no AVS),
fail when any
measurement failed, else pass; a measured check with an inferred limit is unverified (neither pass nor fail).

### Score

Penalty points: `score = sum over the judged checks of weight x fails`, **0 = ideal**; an unverified check adds nothing (its weight
applies once its limit has a source). The weights are **provisional** (`kDefs` in
`result.hpp`), to be settled after runs on several chargers; the result keeps every count, so a run can be rescored later.

| Weight | Checks |
|---|---|
| 1 | `accept_time` `src_start` `pps_status_time` `pps_settle` (unverified for now: 0) |
| 2 | `ps_rdy_time` `rise_time` `fall_time` `pps_accuracy` `pps_band` `pps_ps_rdy` `pps_transient` `avs_accuracy` `avs_band` `avs_ps_rdy` `avs_transient` `hr_caps` `reattach_caps` `pps_track` `avs_track` |
| 3 | `ps_rdy_early` `ps_rdy_low` `overshoot` `accuracy` `pps_monotonic` `pps_overshoot` `pps_status` `avs_monotonic` `avs_overshoot` `hr_recover` `hr_turn_on` |
| 5 | `pps_keepalive` `epr_keepalive` `hr_safe0v` `detach_safe0v` |
| 10 | `reattach` |
| 20 | `source_reset` |

## Measurement

Each fixed step and each measured programmable move arms the transition capture (`k2::transcap`, the 1 kHz VBUS ring with the PD edge
markers) and triggers it at the Request with a post window (1.5 s for a fixed step, 300 ms for a small programmable step, 700 ms for a
large one: over vPpsSmallStep / vAvsSmallStep, the slams); the analysis runs in place on the frozen ring. When the capture is held by someone else, the run goes on without it: the times come from the sink's edge stamps and live VBUS
polling, each widened by the monitor's sample age (5 ms) and the poll spacing, and the checks that need the waveform read "capture
busy". A run overwrites the last transition capture (`pd_trace.py transition`).

The PPS settle time is a measurement-floor case. A 20 mV step is two of the monitor's noise widths: single INA226 samples at 1 kHz
scatter by about 3.5 mV (sd) around a steady level and single ones by up to 13 mV. Judged sample by sample against +-10 mV, a step
"settled" at the last noise spike of its 300 ms window: the Anker IQ4 read 183-268 ms for a level that arrived 5 ms after the Accept
(`tests/fixtures/pdspec/iq4_pps_7000_7020.csv`). The settle is therefore judged on the mean of the last four samples (about 2 ms of
lag), and the check stays unverified while its limit is inferred.

Two figures the specification gives are not measured. The PPS slew rate is not measurable: with no load and a monitor whose noise is a
large part of a 20 mV step, the 1 kHz samples do not resolve a rate. And the band time of a small step is trivially 0 on most levels:
a step smaller than 5 % of its target (100 mV at 5 V, 500 mV above 10 V) starts inside vPpsNew, so `pps_band` reads 0 ms for it and
only the steps that start outside the band (the larger steps at low levels, the slams) measure a real entry time; `pps_ps_rdy` is
measured on every move.

### Programmable sweep

The windows come from the source's SPR PPS and SPR AVS APDOs (`plan.hpp` `Prog`, at most 4; EPR AVS is not swept). Each window
runs four passes, each from the window's minimum up to its maximum and back down to the minimum (`Sweep`):

| Pass (class) | PPS | AVS |
|---|---|---|
| 0 | 20 mV: three 10-step windows (low, middle, high of the APDO; 10 up, 10 down each, setup moves between them unmeasured), or the whole window with `--pps-full` | 100 mV (the AVS resolution) |
| 1 | 100 mV | 500 mV |
| 2 | 500 mV (vPpsSmallStep) | 1000 mV (vAvsSmallStep) |
| 3 | the slams: minimum to maximum and back (large steps) | the slams |

A step that does not fit whole at the top is cut to the maximum (and likewise at the bottom), so every pass touches both ends. The
floor is the APDO's own minimum (3.3 V on the Anker IQ4): the K2 is never powered by the charger in a run (refused at the start), so a
level under 5 V cannot brown it out. A source that latches its output off at the low end of its own window shows as a level refused or
a stuck step, and the run stops there; the floor is therefore the APDO's, not a K2 margin. Each measured move is a fresh Request, and
the sink's own PPS re-Request (every 5 s, under the 10 s the specification allows) keeps the contract alive through the waits.
A move whose VBUS is neither in the band nor still on a level at the end of its window (an unloaded fall slower than the window) waits
in place until it settles, and its level is not used for accuracy or as the reference for the next monotonic check.
The same holds for a point reached by a move that is not one of the pass's own steps: the start of a pass at the window's minimum and the
setup moves between the 20 mV windows (a large jump) are only places to start from. The first in-window step's result is the first
reference of the `*_monotonic` and `*_track` comparison, and that first step itself is not compared. Reason: a source may land a large
jump up to vPpsValid off and hold the landing until the next request (UGREEN Nexode Air: 10.80 V requested after a jump, 10.895 V held,
the next 20 mV request 10.82 V settles at 10.815 V), which is a landing, not a fold; comparing the first step with it failed both checks
on a clean source. A fold between two in-window points (FH22, 19.98 V to 20.00 V) and the 100 / 500 / 1000 mV passes are still judged.

Get_PPS_Status is asked twice per PPS pass, at the top (before the first move down) and at the end, never during the PPS timeout check.
The sink queues it like any other message; the answer is timed from the Get_PPS_Status TX start to the PPS_Status RX edge.

**Fold and track break** (`pps_track`, `avs_track`; rule and helper in `include/k2/pd/prog_track.hpp`, shared with the charger test's deep sweep). On an upward step of classes 0-2 the offset to the previous settled level is
`behind = (request - previous request) - (settled - previous settled)`; the check measures `behind` against `min(step + 50, 150)` mV, once per pass (the
first offender, so a plateau at a coarse step is not counted again). A *fold* is the case where the settled level itself fell by more than 50 mV on a move
up (the source dropped to another setpoint: Baseus FH22, every request of 20.0 V or more lands at 19.40-19.43 V); a *track break* is a jump of the offset
by more than 150 mV with the level still rising (only visible at steps of 500 mV and up). Downward steps and the slams are not judged; the per-step
checks (`*_accuracy`, `*_monotonic`) are unchanged, so a folding source fails `pps_monotonic` as well, and `pps_track` is the one that names the request.
The 20 mV and 100 mV classes fold at 20.0 V on the FH22 (hardware check pending), the 500 mV and 1000 mV classes break there. Fixture-tested on the FH22 PPS / AVS sweeps and the
UGREEN AVS sweep (clean within 70 mV).

AVS is **host-tested only**: no charger on the bench offers SPR AVS (the Anker IQ4 has PPS only), so the `avs_*` checks read "no AVS"
on hardware so far.

**Caps note** (advisory, no verdict, no score): the run reads the capabilities and names the programmable mix, "PPS without AVS:
allowed before PD R3.3", "PPS + AVS present", "AVS without PPS" or "no PPS, no AVS". It is printed with the result and carried in
the blob.

**Time budget.** The run caps itself at 20 minutes. The PPS/AVS family is the long one: a move costs its Request round trip plus its
capture window (300 / 700 ms), so a 3.3-11 V PPS window takes 60 moves at 20 mV (three windows), 154 at 100 mV, 32 at 500 mV and 2
slams; `--pps-full` takes 770 moves at 20 mV on that window and may not fit beside the other families. `pd_spec.py run` saves the
time each family took (`family_s` in result.json, the last line of report.txt).

**Hardware, Anker IQ4 (2026-11-15, build 1.0.2-45-09983f46, `runs/pps-settle/iq4-run1` and `iq4-run2`).** Fixed 5/9/12/15/20 V and
PPS 3.3-11 V at 3 A, no AVS, no EPR. Both runs finished in 129 s: Transitions 32 s, PPS/AVS 80 s (248 moves), PPS timeout 14 s,
Hard Reset 2 s, Detach 1 s. Each: 25 pass, 1 fail, 7 skip (EPR, AVS), 1 unverified; score 40 and 46.

| Check | Run 1 | Run 2 |
|---|---|---|
| `pps_ps_rdy` | fail: 20 of 248, worst 32.8 ms (500 mV up) | fail: 23 of 248, worst 31.0 ms (500 mV up) |
| PS_RDY per pass, 20 / 100 / 500 mV / slams | 25.5 / 30.9 / 32.7 / 91.9 ms | 22.0 / 29.7 / 31.0 / 102.3 ms |
| `pps_band` (VBUS inside vPpsNew) | 4.9 ms | 5.8 ms |
| `pps_settle` (20 mV, unverified) | 8.1 ms | 8.7 ms |
| `pps_monotonic`, `pps_overshoot`, `pps_transient` | pass (dmin 17 mV, no excursion) | pass (one 2 ms stretch) |
| `pps_status` / `pps_status_time` | 8 clean / 9.7 ms (6 timed) | 8 clean / 9.3 ms (7 timed) |
| `pps_keepalive` (source Hard Reset after the silence) | 13.3 s | 13.3 s |

The IQ4's level is in band within 2-9 ms of the Accept, but its PS_RDY comes 22-33 ms after it on small steps, over
tPpsSrcTransSmall (25 ms) on 100 and 500 mV steps in both runs. These runs judged PS_RDY against the bare 25 ms; since then the
limit carries the 2 ms wire allowance (27 ms, see the limits table). The 20 mV worst (25.5 ms, run 1) passes under it; the 100 and
500 mV worsts (29.7-32.7 ms) still fail: a real finding, pending a `pd_trace` cross-check (`late-ps-rdy-small-pps-step` in
`docs/CHARGER_QUIRKS.md`). The blob keeps the worst per pass, not every move, so the fail counts under 27 ms are not known. A Get_PPS_Status answer is timed only when both edge stamps are real; 1-2 of
the 8 per run were not, and are judged on content only. The `report.txt` files in these two directories are re-rendered with the fixed decoder
(`accept_time` at "APDO 6 20 mV down to 3.3 V", not "PDO 134 to 33"; the originals are `report.orig.txt`); the K2's Detail page had
the same label and reads "APDO 6 c0 dn 3.3V" since then. The web Spec check page decodes the same PSC2 blob (`tools/web/k2proto.js`,
checked against `pdspec_blob.py` on both runs' blobs) and shows the passes as their own grid.

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

On the K2 the checks are part of the charger test's **Deep** level (a K2_PDSPEC build; the default image has no spec stage and its Deep
run is unchanged). The full six-family run, with the keep-alive, Hard Reset and detach checks, is the standalone tooling below
(`pd_spec.py`, `k2_remote pd-spec`, the web Spec check page).

### Deep run

Light, Medium and Deep stay the three levels. A Deep run runs the engine once more, right after its PD unit (its contract back at a settled 5 V, the charger asked who it is, before the unit's detach; `State::Spec` in the charger-test
`Runner`, `Env::spec_start / spec_stop / spec_collect / spec_done`; the session glue is `ChargerTestSession` in
`src/ui/ui_charger_test.cpp`, which drives `PdSpecSession::start_in_test()`). The Review page of a Deep run names the stage and counts it
in the plan's steps; the Running footer reads "PD spec ..." while it goes.

It is a polite, no-load conformance run: nothing in it lies to the charger or asks above what the charger advertised.

- **Runs:** the Transitions family (fixed level pairs, up and down) and PPS / AVS (sweeps and slams inside each advertised APDO range),
  plus the Source reset observer (it only watches). The engine families mask is `0x03`. The result is 27 checks (`specsec::kOrder`).
- **Left out, on purpose:** PPS keep-alive and EPR keep-alive (the source Hard Resets by design), Hard Reset (the K2's own) and Detach
  (Rd off). They stay in the standalone tooling.
- **Session:** `start_in_test()` runs on the live link the test already owns: no foreign-busy check, no Rd take-over, no give-back, no
  footer of its own. The standalone `start()` (the remote `pd-spec` command) keeps all of those.
- **Results grid:** the section follows the units, one VerdictCell per check, rules above the PPS block, the AVS block and the
  observer. A cell's detail page shows the worst value, the limit, the count, the place and the verdict (all kept in the cell, live or resumed). A stage that did not run (no PD unit, a stop, a refusal) leaves the section out.
- **Record:** the journal keeps it (`Type::Spec`, `/jr/sp.rec`, written once when the stage ends) and the cells' limit, count and place (`Type::SpecDetail`,
  `/jr/spd.rec`, behind it; journal format 4; a format 3 journal still loads, with "-" for those). The report card
  does not carry the cells. The remote reads the section with `0x0B [6, off]` and the detail with `[7, off]` (`encode_charger_test_spec`); `ct_start.py` and the `k2_remote` charger-test
  export carry it in their summary and files; the web charger-test grid draws it (`tools/web/ctgrid.js`).
- **Owner:** while the stage runs the charger test is the owner (`BusyWhy::TestRunning`, which wins over `BusyWhy::PdSpec`); `PdSpecSession`
  stays out of the way of the standalone owner checks. `BusyWhy::PdSpec` (12) is the standalone run's.

While a standalone run (the remote `pd-spec start`) goes on, any other page shows a one-line footer ("PD Spec: Transitions 12/56", "PD Spec: PPS/AVS 3/16"), and the session block names the owner
(`BusyWhy::PdSpec`, 12; `pd-spec` in `k2_remote` and the web page). The dead-man lease leaves a run alone (`Ui::run_busy`): a host
that goes away leaves the run to finish within its 20 min cap.

From the computer:

```
python3 tools/pd_spec.py run --charger NAME --confirm [--families transitions,pps-avs,...] [--pps-full] [--wait 1500]
python3 tools/pd_spec.py status | stop | report runs/pd-spec-NAME-STAMP/result.json [--json]
python3 tools/k2_remote.py pd-spec start --confirm [--families all|0x3f|transitions,detach] [--pps-full]
python3 tools/k2_remote.py pd-spec status | result | stop        # --json for the raw values
```

`run` saves `result.json` (the raw blob in hex and its decode), `report.txt` and `stream.log` (the `[PDSPEC]` log lines) under
`runs/pd-spec-<charger>-<stamp>/`; result.json also carries `family_s`, the seconds each family took (from the 2 s polls). The
family names are `transitions`, `pps-avs` (`pps-steps` still reads), `pps-timeout`, `epr-timeout`, `hard-reset`, `detach`. The
report lists every check, then the transitions and one line per programmable pass: APDO, kind, step size (or slams), window, moves,
the worst band and PS_RDY times, the worst level error, the worst monotonic delta, the worst overshoot, the worst out-of-band stretch,
the fails per check and the PPS_Status faults.

The firmware logs only the start and the end of a run as `[PDSPEC]` lines; the per-check, per-pass and caps-note lines were dropped
to fit the PD Spec image in flash. Every value they carried is in the blob, so `report.txt` and `result.json` are the record.

### Remote protocol

Command 0x5A (`Cmd::PdSpec`, `include/k2/core/remote_proto.hpp`): op 0 status, 1 start (args: families mask, flags; bit 0 of the flags
is the confirm, required: NeedConfirm without it; bit 1 asks for the full 20 mV PPS sweep; a mask beyond 0x3F or an unknown flag is
BadArg; Busy when another tool owns the
ports, other refusals in the status block's `start` byte), 2 read (offset u16; reply offset, total, n, data, 48-byte chunks), 3 stop
(never refused). The 17-byte status block: state (0 idle, 1 running, 2 finished), stop, start, family, item, items, families, fails,
passes, skips (unverified checks are counted in none of the three; the blob has them), flags (bit 0: return to 5 V not confirmed),
score u16, elapsed s u16, blob length u16.

The result blob (`result.hpp`, magic `PSC2`): a 16-byte header (state, stop, transition and pass record counts, score, elapsed,
flags, then byte 13 the number of checks, byte 14 the caps note, byte 15 the options: bit 0 full 20 mV sweep), one 20-byte check
aggregate per check (36 now; verdict, skip reason, where, count, fails, worst, low and high limit), 32 bytes of timer numbers (PPS and
EPR timeout, the Hard Reset and detach times), then one 20-byte record per fixed transition (Accept, PS_RDY, start and band times,
VBUS at PS_RDY, peak, final, sample gap, flags) and one 28-byte record per programmable pass (`PassRec`, at most 16: APDO, class with
bit 7 for AVS, moves, window, worst band / PS_RDY / out-of-band times in 0.1 ms, worst level error, monotonic delta and overshoot in mV,
six fail counters, the PPS_Status fault bits, byte 27 the track flag: this pass already reported its track finding). `pdspec_blob.py` still reads `PSC1` blobs (21 checks, 14-byte PPS points). The K2 never holds the blob:
`BlobReader` streams it from the engine's state.

## Phase 2 (design hooks only, nothing built)

- **EPR AVS**: the EPR AVS windows (tAvsSrcTransLarge EPR) need EPR mode and a bench charger that offers EPR AVS; SPR AVS is in,
  host-tested.
- **PPS current limit** (needs a load: out of scope for a no-load tool, only the advertised flags can be read).
- **Source message content**: Source_Capabilities_Extended and Status answered within tReceiverResponse, with fields that agree with
  the capabilities (PPS_Status is in).
- **Soft_Reset**: Accept within tReceiverResponse, Source_Capabilities within 250 ms of the Accept's GoodCRC, VBUS unchanged. The sink
  can send one without a Hard Reset fallback (`Sink::soft_reset`: after 400 ms of silence it asks for the caps once), but it keeps
  no TX stamp of the Soft_Reset and no RX stamp of its Accept, which the timing needs; not built.
- **Hooks in place**: the `Family` and `Check` enums, `kDefs` and `Skip` are append-only (the blob carries them by number; a layout
  change bumps the magic, `PSC1` to `PSC2` with the sweep passes, and `pdspec_blob.py` keeps reading the older ones); the `Env` seam takes new actions without touching
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
- the wait for a resume presents no Rd of its own: it needs the Rd the boot already has, otherwise the 60 s wait ends and the journal is
  dropped; once resumed, the run presents its own Rd like a started one (below).

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
The release never fires while a run owns the line, so a run cannot be cut by it (hardware 2026-11-11 saw it at a Soft_Reset step). From the
start (and from a resume) the run presents its own Rd (`fusb302::rd_for_run(true)`, as the Deep spec stage and a charger-test START do): a run
started on a contract that was still up had Rd only through the sink's demand, which a source Hard Reset ends with the contract, so the
Rd went 300 ms later and the source never came back. At the end (`on_finish`) the run gives that Rd back.

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
