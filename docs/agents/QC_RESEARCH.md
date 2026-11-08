# Quick Charge (HVDCP) reverse-engineered notes vs the K2 implementation

Reverse-engineered data on Quick Charge sink behaviour (QC2, QC3, QC3+, QC4/QC4+/QC5), compared with our
implementation. Date 2026-11-04. Compared with: `docs/FASTCHARGE_PROTOCOLS.md` (§1), `docs/CHARGER_QUIRKS.md`,
`include/k2/fastcharge/qc.hpp`, `src/fastcharge/fc_engine.cpp`.

Tags: "RE" = reverse-engineered sink behaviour; "inferred" = reasoning, not observed; "unknown" = not recovered.

Scope limit: QC3 wire-level pulse timing was not recovered (a real sink generates the pulses autonomously in
hardware), and nothing wire-level was found for QC3+/QC4/QC4+/QC5: no SVID, VDM, vendor VID or
Source_Capabilities_Extended usage.

## 1. HVDCP detection as a sink does it

| Step | What happens | Tag |
|---|---|---|
| 1 | HVDCP adapter attached at 5 V, D+ and D- shorted | RE |
| 2 | Sink runs BC1.2 (DCD, then primary detection). DCD timeout about 300-900 ms; detection debounce >= 10 ms | RE |
| 3 | DCP detected: sink forces D+ to about 0.6 V (0.5-0.7 V). Data reference about 0.325 V (0.25-0.4 V); D- sink current tens of uA; D+ source on >= 100 ms | RE |
| 4 | The adapter starts a minimum 1 s timer and checks D+ stays high >= 1 s. If the sink releases early the adapter stays at 5 V | RE |
| 5 | The adapter removes the D+/D- short and pulls D- down, leaving D+ open | RE |
| 6 | Sink selects a voltage with DC levels: 5 V D+0.6/D-0; 9 V D+3.3/D-0.6; 12 V both 0.6; 20 V both 3.3 | RE |
| 7 | The adapter holds the requested voltage until D+ falls below about 0.325 V, then reverts to 5 V | RE |
| 8 | After detection the D lines go Hi-Z; the charger logic owns them while HVDCP is active | RE |
| 9 | Re-running detection after HVDCP: request 5 V first, wait 500 ms, then re-detect | RE |
| 10 | About 600 ms of floating D+/D- classifies a plain SDP/floating charger | RE |

Compare K2: handshake A/B in `qc.hpp` (1000 ms reset, 190 ms bias, D- window 200..1000 mV, wake 322/190/87/50 ms,
poll < 500 mV, finish 38 ms) fits steps 3-6. The 1 s D+ hold and the D+ < 0.325 V exit agree. The wake times
322/190/87/50 ms have no outside reference.

## 2. QC2 voltage table

| Voltage | D+ / D- (K2 `qc2_levels`) | Class | Note |
|---|---|---|---|
| 5 V | 0.6 / 0 | A | default |
| 9 V | 3.3 / 0.6 | A | Class A = 5/9/12 V |
| 12 V | 0.6 / 0.6 | A | |
| 20 V | 3.3 / 3.3 | B | Class B only; many sinks never request it |

Settle times: unknown. Sink-side bounds observed: input current limiting starts about 175 ms after high-current mode,
with a 175 ms glitch filter. Input UV/OV thresholds follow the selected range, so at 9 V a 5 V input reads as UV and
12 V as OV. Inferred: a K2 QC2 test needs no settle longer than the adapter's own slew; `kQc2TransitionMs` 12000 is a
timeout, not a settle, and is generous.

## 3. QC3 continuous mode

| Item | Value | Tag |
|---|---|---|
| Step | 200 mV | RE |
| Range | 3.6-12 V seen; some sinks limit to 5-12 V | RE |
| QC2 vs QC3 choice | the sink authenticates the adapter; failure falls back to QC2 | RE |
| Entry signalling | D+0.6/D-3.3 (`qc.hpp`) | inferred |
| Pulse width, minimum gap, max rate | unknown (hardware-generated in sinks) | - |
| Step confirmation | sinks count 200 mV steps and wait for a "change done" status before the next decision | RE |
| Sink commands | single increment / single decrement of 200 mV, plus forced 5/9/12 V | RE |
| Sink algorithms | a conservative one (targets about 3 A) and an optimising one (only the minimum voltage for the operating point) | RE |
| Limit | above 9 V a sink OVLO near 10.2 V blocks increments and forces decrements below it | RE |
| Exit | adapter removal or D+ low reverts to 5 V; sinks also run a taper timer (QC2 30 s-1 min, QC3 1-5 min) | RE |

K2 side: pulse 2250 us, gap 2250 us (`qc.hpp`: "not in the spec: set equal to pulse width"). Not confirmed.

## 4. QC3+ / QC3.5 / INOV / QC4 / QC4+ / QC5

| Version | Transport | Finding | Tag |
|---|---|---|---|
| QC3+ | D+/D- (20 mV steps, `FASTCHARGE_PROTOCOLS.md` §1) | newer sinks claim compatibility; older sinks count only 200 mV steps, so no 20 mV mode there | RE |
| QC3.5 | - | nothing found | unknown |
| INOV | sink-side charge-time optimiser | no wire-level component | RE |
| QC4 | USB PD with PPS | sink-side features only (dual charge, thermal balance) | RE |
| QC4+ | PD PPS plus INOV | PPS-only input, backward compatible | RE |
| QC5 | PD PPS | fine-step input regulation by the upstream supply | RE |
| Proprietary handshake | none found | no VDM, SVID, vendor PDO or extended message | RE |

Conclusion: QC4 and later are not D+/D- protocols; they are PD 3.0 PPS adapters plus sink-side optimisation. Inferred:
any Qualcomm-specific part would be a vendor VDM or a PPS-range property; nothing confirms one.

## 5. Causes of QC3 overshoot and QC3+ 9 V timeouts (all inferred)

| Observation (CHARGER_QUIRKS) | Candidate cause | Basis |
|---|---|---|
| QC3 9 V overshoots to 9.67 V, "timeout at 9.35 V" then "source lost at 9.67 V" (Baseus 140W) | adapter step is not 200 mV, or quantises coarsely; 670 mV is about 3.4 steps | `qc3-coarse-step` |
| Overshoot after a batch | VBUS keeps slewing after the last pulse; `kQc3SettleMs` 40 ms may be shorter than the adapter slew, so the next batch is computed from a stale sample | real sinks wait for "change done" |
| Protection trip at about 9.7 V | source OVP in the 9 V class; phone-class sinks tolerate about 10 V at most | RE |
| Step count mismatch | gap equal to pulse width (2250 us) may lose pulses (undershoot) or count glitches (overshoot) | inferred |
| QC3+ 9 V timeout at 5.10 V (Baseus 140W) | adapter ignores 20 mV mode | inferred |
| QC3+ 9 V "overvoltage at 9.99 V" (PPBLD100HD, earlier firmware) | a batch of 50 pulses at 40/50 us overshoots when the adapter is slower than the loop | `kQc3PlusBatchPulses` 50; inferred |

## 6. Labelling signatures for newer QC versions

The K2 acts as the sink, so the observable surface is D+/D- at rest, Source_Capabilities,
Source_Capabilities_Extended, Discover Identity and VBUS behaviour. Passive first.

| Label | Observable | Confidence | Passive or active |
|---|---|---|---|
| HVDCP-capable (any QC2/3) | D+/D- shorted at 5 V before any handshake, then D- pulled down after the 1 s D+ hold | high | passive if D+/D- can be sampled at rest; otherwise the existing active probe |
| QC3+ (20 mV) | accepts 20 mV pulses; no static observable | medium, behavioural | active (fctrig 9 V probe) |
| QC4 | PD 3.0 with a PPS APDO; may also keep the HVDCP D+/D- short at 5 V | low | passive: APDO list |
| QC4+ | QC4 with a PPS range down to 3.3 V-class output and fine steps | low | passive: APDO min/max V, max A, Source_Capabilities_Extended |
| QC5 | PD PPS with high power (100 W class), possibly EPR | low | passive: APDO max current, EPR AVS, PDP |
| INOV | none; sink-side only | none | not detectable |
| Qualcomm origin | vendor VID in Discover Identity or Source_Cap_Extended (guess) | low | passive: log VID/PID, no rule table |
| Combined | HVDCP D+/D- present alongside PD caps | medium as a flag; the K2 runs PD and D+/D- in separate sessions | passive PD, then a gated D+/D- probe; never request QC voltage while a PD contract is live |

Proposal (no code yet): a `qc_family` line on the caps/identity page that prints facts rather than guessed marketing
names: "PD PPS 3.3-21 V step 20 mV", "HVDCP D+/D- yes/no", "vendor VID yes/no". Show QC4+/QC5 names only as "likely"
when PPS and the HVDCP flag coincide.

## 7. Recommendations and mismatches

| # | Item | Suggestion | Basis |
|---|---|---|---|
| 1 | QC3 loop confirmation | after each batch wait until VBUS is stable (e.g. 3 samples within 50 mV) before the next batch, not a fixed 40 ms | inferred |
| 2 | QC3 final approach | single pulses for the last 1-2 steps, stop at target - 100 mV | inferred |
| 3 | Pass band | asymmetric, wider on undershoot (e.g. -300/+150 mV) instead of +-300 mV | inferred |
| 4 | Cross-check | expected pulse count (target - 5000) / learned step as an open-loop bound; abort if VBUS exceeds it by more than 1 step | RE |
| 5 | Re-detect after QC | return to 5 V, hold 500 ms before any further detection | RE |
| 6 | QC3+ 9 V timeout at 5.1 V | record as "no QC3+ response", not a failure | inferred |
| 7 | Gap | try gap > pulse width in a per-pulse trace on the Baseus before changing defaults | unknown timing |
| 8 | Ceiling | keep QC2/QC3 at 12 V; 20 V failures stay Class B expectations | project rule |

| # | Our code / spec | RE finding | Verdict |
|---|---|---|---|
| 1 | QC3 gap 2250 us = pulse width | pulse timing unknown | unverifiable |
| 2 | QC3+ pulse 40 us, gap 50 us | no 20 mV mode in older sinks | unverifiable |
| 3 | QC3 min 3.6 V (`kMinQc3Mv` 3600) | 3.6-12 V seen, some sinks 5-12 V | consistent |
| 4 | D+ hold and detect timings in `qc.hpp` | 1 s minimum D+ hold | consistent |
| 5 | poll up to 2000 ms for the D- drop | 1 s hold | harmless margin |
| 6 | pass band +-300 mV, deadband 150/30 mV | step 200 mV | 300 mV is 1.5 steps; wide for the 9 V check |
| 7 | QC3 entry D+0.6/D-3.3 (`qc.hpp`) | no outside source | unverified |
| 8 | step learning with 10 probe pulses | sinks count 200 mV units | consistent with 200 mV |
| 9 | QC 5 V 150 ms (auto scan) | no outside source | unverified |
| 10 | no wait before a new detection after QC | 5 V plus 500 ms | check `fc_engine` 9 V cleanup |
| 11 | QC4/QC4+/QC5 not labelled anywhere | PD/PPS-based | gap, see §6 |

Side issue: `fc_engine` has no per-pulse trace to settle the gap question.
