# Detection implementation plan: review of the research notes, gap list, step plan

Date: 2026-11-07. Base: `main` 663b67e. Review of the two untracked research notes
`docs/agents/TRIGGER_RESEARCH_NOTES.md` and `docs/SOURCE_SINK_DETECTION.md` against the code and the
hardware record, then a ranked gap list and a step-by-step plan for the feasible items.

Evidence rules used here: a verdict cites a repo `file:line` at 663b67e, a hardware result in
`docs/CHARGER_QUIRKS.md` / `docs/agents/HANDOFF_2026-11-06.md`, or a public source. It cites no vendor
documents. Charger behaviour is detected by patterns (`docs/CHARGER_QUIRKS.md`), never
by VID:PID.

Verdict words: **correct** (matches code or a hardware result), **wrong** (contradicted by code
or a hardware result), **unverified** (plausible, nothing in the repo proves it), **stale**
(the fact is right, the cited line moved).

---

## 1. Claim-by-claim review

### 1.1 `docs/agents/TRIGGER_RESEARCH_NOTES.md`

| # | Claim (note section) | Verdict | Evidence |
|---|---|---|---|
| T1 | Apple USB-A bricks signal 1 A / 2.1 A / 2.4 A with fixed DC levels on D+/D- (2.0 V / 2.7 V pairs) (§1) | correct, classification only | `src/fastcharge/fastcharge_classify.cpp:27-29`: Apple24 = D+ bucket 2 (~2.7 V) and D- bucket 2; Apple21 = D+ ~2.7 V, D- 1.9-2.1 V; Apple10 = D+ bucket 8 (~2.0 V), D- 2.6-2.8 V. The K2 *classifies* these; it has no Apple brick emulation: `include/k2/drivers/dpdm.hpp:15` Drive enum `{HiZ, V0, V0p6, V2p0, V3p3, RxPulldown, Listen, Sniff}` has no 2.7 V primitive. |
| T2 | "What the K2 must apply to emulate an Apple brick" (§1 trigger sequence) | unverified and out of scope | No source-side D+/D- drive at 2.7 V exists (T1). Emulating a brick means acting as a *source*; the K2 is a sink/inline tester (`docs/K2_PORTS_SPEC.md` capability table, "Present Rp: no"). See gap G9. |
| T3 | Apple over USB-C is plain PD, nothing proprietary on CC (§2) | correct, public | Consistent with the repo: no Apple SVID handling anywhere (`grep -ri apple src include` hits only the classifier names). |
| T4 | VOOC: the charger is the clock master on D+, the tester answers on D- (§3) | correct | `docs/FASTCHARGE_PROTOCOLS.md` §3 OnePlus capture; `src/fastcharge/fc_engine.cpp` VOOC slave; `include/k2/fastcharge/vooc.hpp:14-19` frame constants. |
| T5 | "Genuine VOOC chargers only start clocking D+ after a VBUS load pulse; this is the K2 'needs load' failure" (§3 step 1, §3 gotchas, §8.2) | **wrong** | (a) `FASTCHARGE_PROTOCOLS.md` §3: on hardware the adapter clocked D+ on PA7 *without any load* after the wake train (not on every attempt). (b) `Fail::NeedsLoad` is the *no frames heard* outcome, not a measured cause: `src/fastcharge/fc_engine.cpp:779` and `:789` `abort(vooc_frames_ > 0 ? Fail::NoResponse : Fail::NeedsLoad)`; `include/k2/fastcharge/fc_engine.hpp:36` names a PA8 PWM load burst the K2 does not drive. (c) The PA8 load path exists (`include/k2/drivers/load.hpp`, bounded by `load_guard`) but drove no measurable INA226 current on 2026-11-02 (`FASTCHARGE_PROTOCOLS.md` §3), so a load pulse has never been shown to wake anything. The repo's "needs load" rows are a hypothesis label, and the note upgraded it to a fact. |
| T6 | VOOC frames are 10 clocks, `command = frame >> 2`, the slave samples 1 µs after each D+ fall (§3 step 3) | wrong on the frame length, unverified on the rest | `include/k2/fastcharge/vooc.hpp:14` `kFrameBits = 9`; `FASTCHARGE_PROTOCOLS.md` §3: 9 clocks/frame, the bit is sampled just before the *next* D+ fall, the reply comes 8.9-9.1 ms later. The note's "10 clocks" and "1 µs" come from a different decoder assumption than the hardware capture. |
| T7 | `0xA8` = SuperVOOC identification, ACK `0x0C` (§3 step 4) | partly wrong | Repo: 0x88 → `0x04D` accepted, `0x00D` rejected (`vooc.hpp:15-16`, `FASTCHARGE_PROTOCOLS.md` §3); the K2's reply for A8/AC/A6 is `0x00D` (`vooc.hpp:73`). No `0x0C` ACK is in the capture record. |
| T8 | "SVOOC high power is only granted with the original BBK high-current cable" (§3 gotchas) | unverified | The OnePlus C-to-A SUPERVOOC cable has no e-marker (`docs/K2_PORTS_SPEC.md`, built-in cable ids 3/4). Which side (cable ID resistor vs chip) gates it is not measured in the repo. |
| T9 | FCP: read 0x21, write 0x2C (100 mV units), 0x2D (100 mA units) (§4 step 3) | correct | `include/k2/fastcharge/accp.hpp:48` registers 0x21/0x2B/0x2C/0x2D. |
| T10 | SCP: 0x80 bit 4 supported, 0x90 max power, 0x92-0x97 V/I ranges, 0xA0 = 0xC0 then wait 22 ms, 0xA1 = 0x06 (§4 step 4) | correct | `include/k2/fastcharge/accp.hpp:61-69` (0x7E, 0x80, 0x90..0x97, 0xA0=0xC0 wait 22 ms, 0xA1=0x06, 0xB0/0xB2/0xB8/0xBA, 0xCA/0xCB). |
| T11 | SCP watchdog register 0xA2 drops the charger to 5 V if writes stop (§8.4) | unverified | 0xA2 is not used by the K2 (`accp.hpp` has no 0xA2); no hardware record of an SCP session timing out. |
| T12 | ACCP: D+ held at 0.6 V, master ping on D-, bit timings (§4 step 2) | correct in structure, timings unverified | CRC-8 poly 0x39 init 0 (`accp.hpp:12`), cmd read 0x0C / write 0x0B / ack 0x08. The µs values in the note are not in the repo; the K2's own ACCP timings live in `src/fastcharge/fc_engine.cpp` and have passed on hardware (FCP/SCP rows in CHARGER_QUIRKS per-charger sections). |
| T13 | AFC byte `(V<<4)|I`, 9 V = 0x46, 12 V = 0x79, sent 3x 10 ms apart (§5) | correct | `include/k2/fastcharge/afc.hpp:9-16`. |
| T14 | "AFC/FCP/SCP commands are sent 3x with 10 ms spacing" (§8.3) | correct for AFC only | AFC: `afc.hpp:9-10`. FCP/SCP: the ACCP engine retries per register read/write, not a fixed 3x10 ms (unverified for the note's wording). |
| T15 | SFC = PD PPS (§5) | correct, public | Nothing Samsung-proprietary over CC in the repo. |
| T16 | "USB-C 120 W HyperCharge is plain PD PPS, gated only by the 6 A e-marker; a non-Xiaomi phone gets standard PD/PPS only" (§6) | **wrong** | Repo proof: the 6 A APDOs appear only after the sink-initiated SVID 0x2717 VDM chain. Opener `kXiaomiOpenerVdo = 0x27170101` (`src/pd/pd_sink.cpp:30`), chain guard `xiaomi_chain_active` (`:335-341`), step-1 reply `kXiaomiStep1ReplyVdo = 0x27170102` (`include/k2/pd/pd_sink.hpp:321`, used `src/pd/pd_sink.cpp:933-935`), step 0x0A sets `xiaomi_authed_` and re-asks caps (`:984-993`). Hardware 2026-11-04/06 on CUKTECH C1: PPS APDOs rise 5 A → 6 A ~0.7 s after the contract, Mi 20 V 6 A and 11 V 6 A pass with the real 6 A cable (`docs/agents/HANDOFF_2026-11-06.md:201`; `docs/CHARGER_QUIRKS.md` xiaomi-120w-auth-implemented, auth-capped-by-cable, xiaomi-chain-killed-by-sink-request). The cable *also* caps (auth-capped-by-cable), so the note's cable half is right; "plain PPS" is not. |
| T17 | USB-A 120 W HyperCharge is proprietary on D+/D- with a 6 A A-to-C cable ID (§6) | unverified | No capture of a CUKTECH/Xiaomi USB-A port handshake in the repo; the USB-A port on CUKTECH 120W shows `usb-a-no-load-auto-off` only. |
| T18 | Reset: D+ = D- = 0 V for 500 ms returns HVDCP/AFC/FCP/SCP to 5 V (§4, §8.5) | correct | `src/drivers/dpdm.cpp:22` `kResetHoldMs = 500`, discharge at `:261-265`. |
| T19 | HVDCP entry = D+ 0.6 V first (§8.2) | correct | `src/fastcharge/fc_engine.cpp:10` (kDischargeMs 100 / kFloatDpMs 100 / kBiasMs 50), `include/k2/fastcharge/qc.hpp:32-33` handshake window 350-1150 mV, drop ≥ 350 mV. |
| T20 | EPR 240 W needs an EPR e-marked cable; sources read SOP' before offering > 3 A (§2, §8.1) | correct, public and hardware | Baseus 140W EPR with the e-marked EPR cable and the Ra-mode spoof (`docs/K2_PORTS_SPEC.md:10-72`, 2026-11-06). |
| T21 | "PD needs periodic PD traffic or the source may hard-reset" (§8.4) | wrong as stated | PD has no keepalive; an idle explicit contract is stable. The repo's hard-reset patterns are specific: `hard-reset-on-late-request`, `epr-entry-hard-reset`, `src-reset-after-pps` (`docs/CHARGER_QUIRKS.md:12-37`). |

### 1.2 `docs/SOURCE_SINK_DETECTION.md`

The citation audit (117 sub-cites): 95 match, 21 moved (correct fact, wrong line at 663b67e),
1 absent. Stale lines worth knowing: `pins.hpp` INT_N is `:33`; `pd_passive.hpp` typec_ma
`:126-127`, kTypeCOnlyMs `:130`, DetachTracker `:56-98` (kDetachVbusMs 1000 `:9`, kDetachCcMs 500
`:10`), kRdHoldMs 1500 `:14`, TriggerPrep `:212-254`, AutoCrcGate `:144-157`, kSinkIdleLingerMs
30000 `:188`; `fusb302.hpp` typec_ma `:273`, sink_cc `:194`, emarker `:261`, physical_cable `:263`,
cable_seq `:168`; `pd_vconn.hpp` Not_Supported `:80-82`; `pdtrace.hpp` link() `:333`; the "[PD] CC
scan" log is `fusb302.cpp:488`. The absent literal "[PD] Rd/Auto-CRC" is two lines in code
(`fusb302.cpp:375` "[PD] Rd %s (sink operation)", `:380` "[PD] Auto-CRC %s").

| # | Claim | Verdict | Evidence |
|---|---|---|---|
| S1 | One FUSB302 BMC codec, one active CC at a time; PC14/PC15 close the analog CC switches; PC13 is the VCONN/Ra path | correct | `include/k2/drivers/pins.hpp:26-28`; `src/drivers/fusb302.cpp:198-204` `cc_switches(vconn, mask)`; `include/k2/pd/pd_ccscan.hpp:12` `kCcScanMasks = {3, 1, 2}` (bit0 = CC1 switch, bit1 = CC2 switch). |
| S2 | `poll()` calls `attach_scan()` at ~2 Hz | **wrong** | `src/drivers/fusb302.cpp:1771` calls `detect_cc_orientation()` every 25th 20 ms poll while `g_active_cc == 0` and no Rd hold. `attach_scan()` (`:1619`) is the charger-test entry, called from `src/ui/ui_charger_test.cpp:427` and `:475`. |
| S3 | The K2 only ever presents Rd; it never presents Rp, so it cannot be a source or detect a far sink by Rp | correct | Rd writers all go through `apply_rd()` (`src/drivers/fusb302.cpp:395-406`) and `idle_rd()` (`:386-389`); `rp_on_line()` (`:422-427`) *reads* an Rp level and restores. `docs/K2_PORTS_SPEC.md` table: Present Rp "no". |
| S4 | Rd/Rd (both lines) is avoided because it is the Debug Accessory signature | correct, with one reachable exception | `include/k2/pd/pd_ccmode.hpp:7-9`; `start_rd_lines()` `:42-45` returns 3 (both) only while no CC is known at run start; test `tests/test_fusb302_scan.cpp:780-785` expects "W 02=03" then. CHARGER_QUIRKS `rd-rd-debug-accessory-latch` records the hardware cost. Inferred, untested: `apply_rd()` ORs `rd_lines()` with `key_rd_lines(run_rd_lines(...))` (`:397-398`); with Ra mode on (run part = CC2) and `g_rd_op` on CC1, the sum is 3. `ra_set` refuses on a contract or busy (`include/k2/pd/emk_ra.hpp:36-41`), but `g_rd_op` can rise later. Side issue, see §6. |
| S5 | Gap (1): DR_Swap / PR_Swap are "control types 20/21" and unhandled | numbering **wrong**, gap **correct** | Control numbering is positional in `src/pd/pdtrace_view.cpp:22-24` and `tools/pd_trace.py:114-116`: 9 DR_Swap, 10 PR_Swap, 11 VCONN_Swap, 19 FR_Swap, 20 Get_PPS_Status. The sink has no constant for 9/10/19 (`include/k2/pd/pd_sink.hpp:26-29`); a received DR_Swap/PR_Swap/FR_Swap falls to `default: break;` at `src/pd/pd_sink.cpp:706` (switch at `:629`): no Reject, no Not_Supported, no log. A partner that sends DR_Swap then waits tSenderResponse and may Soft_Reset. Data type 9 is EPR_Request (`pd_sink.hpp:31`), distinguished by num_do > 0 (`src/pd/pd_engine.cpp:51`). |
| S6 | Gap (4): VCONN_Swap is remote/debug only; a partner's VCONN_Swap gets Not_Supported | correct | `include/k2/pd/pd_vconn.hpp:78-84` (`swaps_rx_++; queue(kCtlNotSupported)`); `src/drivers/fusb302.cpp:696-703` feeds control messages to `g_vconn.on_control`. Tests `tests/test_pd_vconn.cpp`. |
| S7 | The K2 never sets the data-role or power-role header bits (always sink/UFP) | correct | `include/k2/pd/pd_sink.hpp:58-62` `make_header` sets type, rev, num_do, ext only; SOP' spoof replies set bit 8 (cable plug) in `src/pd/emk_respond.cpp:33`, `include/k2/pd/emk_spoof.hpp:39,69`. The RX power-role bit is kept only in `raw_header` (`src/pd/pd_engine.cpp:201`, `include/k2/pd/pd.hpp:30`) and `pdtrace_link` `peer_sink` (`src/pd/pdtrace_link.cpp:89-92`). |
| S8 | Gap (2): SOP'' is never read | correct | Only SOP and SOP' hooks exist (`fusb302.hpp:284` `set_sop_prime_hook`); no SOP'' enable in the FUSB302 CONTROL1 setup. Feasible but low value (gap G7). |
| S9 | Gap (3): no Try.SRC / Try.SNK | correct, never doable | Needs Rp (S3). |
| S10 | Gap (5): "no SuperSpeed measurement, no USB link classification" | half wrong | Measurement: correct, no SuperSpeed lanes are wired (`include/k2/pd/usb_link.hpp:9-10`). Classification: already implemented as a passive PD classifier: `include/k2/pd/usb_link.hpp` Family Unknown/LowSpeed/FullSpeed/Usb2/Usb32/Usb4/Usb4v2, Tbt None/3/4/5, Confidence None/Hint/Advertised/Negotiated, dp/tbt flags; tests `tests/test_usb_link.cpp`; remote `UsbLink = 0x46` (`include/k2/core/remote_proto.hpp:292`); `docs/ROADMAP.md` priority B. What is missing is the on-device partner-identity *view* (gap G2). |
| S11 | Gap (6): the K2 listens on one CC line and misses the other | correct by hardware, the mitigation exists | One codec; the orientation scan sweeps masks {3,1,2} only when pinned (`src/drivers/fusb302.cpp:452-458`) and ends at mask 3 (`:493`, `include/k2/pd/pd_ccscan.hpp:57`); `bootmode::cc_mask()` always returns 3 (`include/k2/core/bootmode.hpp:28`). After attach there is no alternation, which is right: the partner talks on its active CC only. |
| S12 | Gap (7): Rp/Rd ambiguity on an Ra-level line | correct, planned | ROADMAP C: a 0.2-0.8 V line is treated as no CC today; `test_vconn_level_line_is_not_an_active_cc_and_detach_never_soft_resets` pins the current behaviour. Gap G4. |
| S13 | Gap (8): e-marker sniff through a cable is untested | correct | `docs/K2_PORTS_SPEC.md` capability table and `ra_guard.hpp` (refuses VCONN through a cable). Gap G7. |
| S14 | `DetachTracker` 1 s VBUS / 500 ms CC, `kRdHoldMs` 1500 | correct, lines moved | `include/k2/pd/pd_passive.hpp:9-14`, `:56-98`. |
| S15 | The sink answers Get_Sink_Cap and Get_Sink_Cap_Extended (Sink Modes 0x23) | correct | `src/pd/pd_sink.cpp:189-223`, `:173` `s[17] = 0x23`; `kSinkCapPdos[2] = {0x0001900A, 0xC0DC213C}` (`pd_sink.hpp:314`). |

### 1.3 Contradictions called out

**C1. UFCS transport.** `docs/CHARGING_PROTOCOL_MAP.md:29` says UFCS is "CC-line, PD-like BMC
signalling", while the implementation and the same document's `:99` put it on D+/D-: USART2 PA2
TX (D-) / PA3 RX (D+), 115200 8N2, DMA1 ch6 (`include/k2/drivers/uart2.hpp:1-3,12`, `src/drivers/uart2.cpp:39,59-67`),
physical handshake in `src/fastcharge/fctrig.cpp:72-93` (both lines 0 V 50 ms; D- 3.3 V 2 ms / 0 V 8 ms twice
with D+ following; D- high, D+ must answer within 15 ms), `ufcs_open()` `:151-161` hands the lines to
USART2. Public UFCS material (the CAICT "Universal Fast Charging Specification", T/TAF 083-2022)
describes a half-duplex UART on D+/D- for USB-A/USB-C 2.0 bricks, with the Type-C alternative
reserved. The repo's hardware status: the handshake is "not yet confirmed on a UFCS source"
(`fctrig.cpp:75`) and the CUKTECH UFCS unit cut the A-port power (`ufcs-cuts-a-port-power`). Verdict:
the map line is wrong; the implementation direction is right but unproven. Step S5 fixes the doc
and makes the on-device attempt.

**C2. Xiaomi 120 W.** Note T16 vs repo proof: the 6 A APDOs are gated by the 0x2717 VDM chain
*and* the cable. See T16. The hunt rows must say "not unlocked" when the chain did not finish
(ROADMAP A), which is step S3.

**C3. VOOC wake.** Note T5 vs `FASTCHARGE_PROTOCOLS.md` §3 and `fc_engine.cpp:779/789`.
The repo's own label "needs load" is the thing the note copied; the label is a hypothesis. Step S6
renames the outcome and tests the slave timing from the capture.

**C4. SOURCE_SINK_DETECTION §(1) "types 20/21"** vs positional numbering (S5). The gap is real,
the numbers were a guess.

**C5. Bucket 8.** `src/fastcharge/fastcharge_classify.cpp:29` uses bucket 8 (D+ ~2.0 V) with D- 2.6-2.8 V
for Apple 1.0 A, which is the public Apple table (1 A: D+ 2.0 V / D- 2.7 V; 2.1 A: D+ 2.7 V / D- 2.0 V;
2.4 A: 2.7 / 2.7). No document carries a conflicting bucket label.

---

## 2. Gap list, ranked by tester value

Hardware limits that decide feasibility: one FUSB302 codec on one CC line at a time; analog CC
switches PC14/PC15 and the PC13 Ra/VCONN path; D+/D- drive levels HiZ/0/0.6/2.0/3.3 V plus ADC on
PA2/PA3 and digital sniff on PA7/PB2; INA226 to 36 V; PA8 load PWM with no measured current; no
SuperSpeed lanes; no Rp; no debugger.

| Rank | Gap | Why it matters to a tester | Feasible here | Step |
|---|---|---|---|---|
| G1 | Received DR_Swap / PR_Swap / FR_Swap are dropped silently | A real phone or dock inline sends DR_Swap right after the contract; silence costs it tSenderResponse and can trigger a Soft_Reset that the tester then records as a charger quirk. Also hides a real partner behaviour from the report. | Yes: pure sink logic + one log line | S1 |
| G2 | No partner-identity view on the device (Discover Identity product types / VDOs, USB data capability, alt modes) | The classifier exists (`usb_link.hpp`) but only the remote sees it; the user cannot read "USB4 Gen3, DP alt, TBT3 cable" on the K2. ROADMAP priority B. | Yes: UI page over existing data | S2 |
| G3 | Mi/CUKTECH 6 A hunt rows say Fail when the chain never unlocked | Misreports a charger that merely had a dumb cable; the user wants "not unlocked (cable 3 A)" vs "refused". ROADMAP A. | Yes: Reason enum + detail | S3 |
| G4 | An Ra-level CC line (0.2-0.8 V) is treated as "no CC" | Through some captive cables the active line reads low; the K2 then never attaches. ROADMAP C. | Yes: `pd_ccscan` classification + hardware check | S4 |
| G5 | UFCS unproven on hardware and the transport contradiction in the docs | A tester that lists UFCS must either pass it on a UFCS brick or label it "unverified". | Partly: the attempt is cheap; success depends on owning a UFCS source (CUKTECH C3/USB-A are the candidates) | S5 |
| G6 | VOOC "needs load" label and slave timing drift from the capture | OnePlus 120 W rows are marked with a cause nobody measured; the capture (§6.3) has the timings. | Yes: pure engine logic + Fake timing test; hardware on the OnePlus brick + its cables | S6 |
| G7 | SOP'' read and e-marker sniff through a cable | Active cables and cable-plug-2 fields; sniffing the charger's own SOP' exchange would give the cable identity without VCONN. | SOP'' enable: yes (register bit). Sniff through a cable: the FUSB302 receives SOP' only when its CC line is the one the source uses for VCONN, which is the far plug's line; expect to see the source's *request* but not the cable's reply on most topologies. Low value, measure first. | S7 |
| G8 | Far-side role observation on an inline sink (who is DFP, who sources VCONN, did a PR_Swap happen) | Shows "laptop became source after PR_Swap" on the lines page; today only `peer_sink` is kept. | Yes: trace already carries the header; add a small observer | S8 |
| G9 | Rp / source role, Try.SRC/SNK, Apple-brick emulation, VBUS sourcing | Would make the K2 a device-side tester. | **Never on this hardware**: no Rp, no VBUS source, no 2.7 V D+/D- drive. Say so in the UI docs. | none |
| G10 | SuperSpeed link rate measurement | "Is this cable really 10 Gbps". | **Never**: no SS lanes wired (`usb_link.hpp:9-10`). The PD-advertised classification (G2) is the ceiling. | none |
| G11 | VOOC wake by load without a real load | A PA8 load pulse is a candidate wake; the K2's PA8 path drew nothing on hardware (`FASTCHARGE_PROTOCOLS.md` §3). | **Not without a measured load path**; do not plan on it. | none |
| G12 | Ra other than the PC13 path (MCU-driven Ra on a CC line) | More spoof topologies. | Rejected as unsafe (`docs/K2_PORTS_SPEC.md` C3). | none |

---

## 3. Implementation plan

Each step is one branch (`k2/<topic>`) for one k2-dev agent, cut from `main`, merged by the
coordinator. Order is dependency order; S1-S3 are independent of each other and can run as
three parallel agents (cap: 4). Every step: pure logic + host tests first (red, then green),
`make gate`, then the hardware proof listed. Stack: no new frame over 1024 B; new `.bss` is
counted (it costs stack directly); the gate's depth check (`tools/stack_depth.py`, `STACK_MARGIN`
1024 in `tools/gate.py`) must stay green and the heartbeat `stack_free` must not drop below the
2026-11-06 figure (1.2 KB spare on the HTTP path, `docs/agents/HANDOFF_2026-11-06.md:219`). Flash: each
step states its expected `text` delta; anything over +5 % needs an explanation in the commit body.

### S1. Answer role-swap requests (DR_Swap / PR_Swap / FR_Swap) in the sink

- Files: `include/k2/pd/pd_sink.hpp` (constants `kCtlDrSwap = 9`, `kCtlPrSwap = 10`, `kCtlFrSwap = 19`
  next to `:26-29`; counters `swaps_rx_[3]`), `src/pd/pd_sink.cpp` (new cases before `:706`),
  `src/pd/pdtrace_view.cpp` (nothing: names exist), `tools/logdb.json` (regenerated by the build).
- Behaviour: DR_Swap → Reject (the K2 stays UFP; Reject is what a UFP-only sink sends in PD 2.0 and
  3.x); PR_Swap → Reject (the K2 cannot source VBUS); FR_Swap → Not_Supported (PD 3.x only; a PD 2.0
  partner never sends it). Log one line `[PD] swap rx %s -> %s` and count per type for the report.
  Rejecting is the only safe answer: Accept on PR_Swap would make the partner expect the K2 to
  drive VBUS and cut its own output (self-powered K2 dies). Never Accept.
- Tests first (`tests/test_pd_sink.cpp`, add at the end): control 9 with num_do 0 → TX Reject within
  the same `take_tx`; control 9 with num_do 1 still routes to EPR_Request (regression for
  `pd_engine.cpp:51`); PR_Swap → Reject; FR_Swap on a rev-3 contract → Not_Supported, on rev-2 →
  nothing (PD 2.0 has no type 19); counters increment. `test_logdb` passes after the regen.
- Hardware: inline between the Baseus 140W and a laptop that sends DR_Swap (most USB-C laptops do
  when they enumerate as host): expect `pd_trace.py live` to show `DR_Swap` then `Reject` from the
  K2 within 30 ms, and no Soft_Reset afterwards. Note: inline with a laptop means no active PD
  from the K2 (device rule); this step only *answers* the partner, which the rule allows because
  the K2 is the sink in that contract only when it is the attached sink itself. If the laptop is
  the sink and the K2 is passive, the swap is not addressed to the K2 and the test is simply
  "trace shows the laptop's DR_Swap and the charger's answer, the K2 stays silent".
- Evidence: `pd_trace.py capture N` with the two messages; `K2LOG` line; `swaps_rx` in the
  report card is **not** added here (card format bump is S8's job).
- Cost: ~150 B text, 0 bss beyond 3 bytes of counters, no new frame. Risk: none to VBUS.
- Dependencies: none.

### S2. Partner-identity view on the device (ROADMAP B)

- Files: new `src/ui/ui_identity.cpp` + `include/k2/ui/ui_identity.hpp` (page body only; data from
  `k2::usb_link` and `k2::pd_identify` / `pd_probe`), `src/ui/ui_pages.cpp` (register the page),
  `docs/UI_DESIGN.md` (the page spec), `tests/test_ui_identity.cpp` (string rendering over a
  fake `usb_link::Link`).
- Content, one screen: product type (UFP/DFP/cable, from the ID header VDO), VID/PID/XID, USB
  version and highest speed (`Family`), DP alt mode yes/no, TBT yes/no + generation, other SVIDs
  (up to 3), confidence word (Hint/Advertised/Negotiated), source of the data (sniffed vs own
  Discover). No new PD traffic: the page is a view over data the sink already collects.
- Tests first: render into a fixed-width buffer for each `Family`/`Tbt`/`Confidence` combination the
  classifier tests already build; "Unknown" and empty-SVID layouts.
- Hardware: `k2_remote page identity` + `screenshot` with (a) Baseus 140W alone (expect "Charger,
  USB 2.0, no alt modes, Advertised"), (b) e-marked EPR cable (cable VDO fields), (c) inline
  laptop + charger passive (expect the laptop's identity if it answers Discover Identity, else
  Hint). No camera; screenshot files in `runs/`.
- Cost: UI page ~1.5-2.5 KB text, 0 bss if rendering is on the stack with frames < 400 B; check
  `stack_depth.py` because page virtuals count. Risk: none.
- Dependencies: none (usb_link exists). Nice-to-have after S8: show "partner asked DR_Swap".

### S3. Mi 6 A hunt rows: "not unlocked" instead of Fail (ROADMAP A)

- Files: `include/k2/chargertest/charger_test.hpp` (`Reason::NotUnlocked` after `ClassAOnly` at `:31-35`;
  detail codes "chain absent" / "chain unfinished" / "cable cap"), `src/chargertest/charger_test.cpp`
  (`hunt_auth_wait` `:785-790` and the hunt rows `:387-393`, `:422-432`: when the wait expires with
  `in.pd.xiaomi_svid && !xiaomi_authed` → Skip/NotUnlocked "chain unfinished"; when
  `!xiaomi_svid` → Skip/NotUnlocked "no 0x2717"; when authed but the APDO stays 5 A and the cable
  is a 3 A one → NotUnlocked "cable cap"), `src/ui/ui_charger_test.cpp` (reason string),
  `include/k2/chargertest/report_card.hpp` (no format change: Reason is already in the step record),
  `docs/CHARGER_TEST_FLOW.md` ("what a row proves" table).
- Tests first (`tests/test_charger_test.cpp` with the `Fake` Env at `:16`): the three skip cases
  and the pass case (authed + 6 A offered + request accepted). Keep `test_pd_sink_xiaomi` green.
- Hardware: CUKTECH 120W C1 with the 6 A cable (pass, as on 2026-11-06), with a 3 A cable (expect
  NotUnlocked "cable cap"), Baseus 140W (expect NotUnlocked "no 0x2717"). Evidence: report card
  JSON (`k2_remote` card dump) showing the reason per hunt row, `pd_trace.py capture` of the chain.
- Cost: < 300 B text. Risk: none new (the hunt rows already request 6 A; they are bounded by
  `kXiaomiAuthWaitMs = 2000`).
- Dependencies: none.

### S4. Ra-level line in the CC scan (ROADMAP C)

- Files: `include/k2/pd/pd_ccscan.hpp` (classification of a 0.2-0.8 V line while the other line
  carries Rp: "active with VCONN-class level"), `src/drivers/fusb302.cpp` `detect_cc_orientation()`
  (`:441-526`), `tests/test_pd_ccscan.cpp`, `tests/test_fusb302_scan.cpp` (update
  `test_vconn_level_line_is_not_an_active_cc_and_detach_never_soft_resets` to the new rule and add
  a case where the Rp line is the active one and the Ra line is just reported).
- Rule to encode: the active CC is the line with an Rp level (0.2-0.8 V with Rd presented reads as
  Rp default/1.5/3 A per the FUSB302 BC_LVL); a line at the Ra level *without* Rp on the other line
  is not an attach. The ambiguity is only when both read low; then tie → CC1 (K2_PORTS_SPEC).
- Hardware: the e-marked EPR cable on the Baseus (the cable's Ra on VCONN line), both plug
  orientations, then `k2_remote state` shows `active_cc` and the `[PD] CC scan` line; compare with
  the 2026-11-06 Ra-mode run. The failure to avoid: attaching Rd to the Ra line (no PD, VBUS stays
  on, partner sees Rd/Ra = powered cable).
- Cost: < 200 B. Risk: a wrong rule turns a working attach into a non-attach; the PLUS-boot pinned
  path (`g_pin_rd`) is unchanged, so the recovery is a replug with PLUS held.
- Dependencies: none. Run after S1-S3 only because it is the one step that touches the attach path.

### S5. UFCS: doc reconciliation + on-device attempt

- Files: `docs/CHARGING_PROTOCOL_MAP.md:29` (transport column → "D+/D- half-duplex UART 115200 8N2
  (USB-A / USB-C 2.0 bricks); the Type-C CC variant is not implemented and not observed"),
  `docs/FASTCHARGE_PROTOCOLS.md` UFCS section (point to `uart2.hpp`), `src/fastcharge/fctrig.cpp:72-75`
  (keep the handshake description), add `K2LOG`
  tags in `ufcs_handshake()` for each phase result (`[UFCS] hs: dm-toggle %d ok, dp-follow %d, dp-high %d ms`).
- Tests: `tests/test_fc_ufcs.cpp` already covers the frame layer; add a handshake-sequencer test
  over `FakeCharger` (`tests/fc_fake.hpp:19`) that replays the D+ follow timing and the 15 ms
  timeout.
- Hardware: `ct_start.py --units ufcs --wait 120` on CUKTECH 120W C3 and on its USB-A port, K2
  HID-powered (not self-powered: `ufcs-cuts-a-port-power` says the A port drops VBUS during the
  attempt, and a self-powered K2 would reboot; `Reason::NeedsExternalPower` is the right skip).
  Capture: `pd_trace.py transition dump --png` (1 kHz VBUS), `[UFCS]` lines. Outcome is either a
  first UFCS handshake on hardware, or a documented "no answer, D+ stayed low" with the cable and
  port listed in `docs/CHARGER_QUIRKS.md`.
- Cost: < 200 B text. Risk: VBUS cut on the A port (known); never run self-powered.
- Dependencies: none, but the result decides whether a `ufcs-unverified` label goes on the unit.

### S6. VOOC slave timing from the capture + relabel "needs load"

- Files: `include/k2/fastcharge/vooc.hpp` (constants: sample point "just before the next D+ fall", reply
  delay 9 ms ± 0.1 from §6.3, 0x88 → 0x04D), `src/fastcharge/fc_engine.cpp` (slave sampling, the
  NeedsLoad outcome → `Fail::NoClock` with the text "no clock from the adapter"; keep the enum
  value for the card), `include/k2/fastcharge/fc_engine.hpp:36` comment, `docs/CHARGER_TEST_FLOW.md` "what a
  5 V legacy row proves" row for VOOC, `docs/CHARGER_QUIRKS.md` `needs-load` pattern → `no-clock`
  with the §6.3a observation.
- Tests first (`tests/test_fc_engine.cpp` with `FakeCharger`): replay the OnePlus 15 frame timing
  from §6.3 (9 clocks, bit valid until the next fall, reply window 8.9-9.1 ms); assert the engine
  decodes 0x88 and answers 0x04D in the window; assert three silent wake trains end in
  `Fail::NoClock`, not a load claim.
- Hardware: OnePlus 120W brick with its C-to-A cable and with the C-to-C cable, `ct_start.py
  --units vooc,supervooc,supervooc65`, then `pd_trace.py transition dump --png` and the
  `tools/vooc_edges.py` decode of the D+ edge capture. Pass = the adapter clocks and the K2's
  0x04D is accepted at least once in 3 tries (hardware says "not every attempt").
- Cost: ~0 text (constants), no bss. Risk: none to VBUS; the engine never drives PA8.
- Dependencies: none. Do not add the PA8 burst (G11) in this step.

### S7. SOP'' enable + cable-sniff measurement (measure first, small)

- Files: `src/drivers/fusb302.cpp` (CONTROL1 ENSOP2 bit while VCONN is up for the e-marker read, SOP''
  frames routed to the same SOP' hook with a flag), `include/k2/pd/pdtrace.hpp` (Rx record already
  carries the SOP type), `tests/test_fusb302_scan.cpp` (new case at the end, ends with a detach:
  SOP'' frame reaches the hook with sop=2).
- Hardware: e-marked EPR cable direct on Baseus (expect no SOP'' reply: a passive cable has one
  plug responder); then passive inline K2 between Baseus and a laptop with the EPR cable:
  `pd_trace.py live` for 10 s after plug-in, count SOP' frames seen. The expected result is "the
  source's Discover Identity SOP' is visible, the cable's reply is not", which decides whether a
  "cable sniff" feature exists at all. Record the result in `docs/K2_PORTS_SPEC.md` capability
  table, replacing "untested".
- Cost: < 100 B. Risk: none (receive only).
- Dependencies: none; low priority, run when the device is free.
- **Built (branch `k2/sop2-sniff`, host-tested, not yet measured).** What differs from the text above:
  the passive profile (CONTROL1 0x67) already received SOP'' and the Rx record already carried the
  SOP token, so no new enable was needed there. The real gap was the e-marker read with Auto-CRC on
  (armed spoof): CONTROL1 was 0x05 and the chip heard neither SOP' nor SOP''. `phy::control1(..., cable_read)`
  now forces 0x67 for the duration of `read_emarker`. ENSOP2 is never set while Auto-CRC is on
  outside a cable read, because the chip would GoodCRC SOP'' and pose as cable plug 2. The hook
  gets `(sop, header, payload)` with sop 1 or 2; the spoof answers only 1. No remote command was
  added: `pd_trace.py live` and the `[PD] SOP'' ...` log line already answer the question.
- **On-device measurement (the only thing left).** Needs the Baseus 140W and the e-marked EPR
  cable; USB HID enabled, K2 in default passive boot, nothing else on the output.
  1. Charger -> e-marked EPR cable -> K2 passive inline -> laptop. Start `python3 tools/pd_trace.py live`
     before plugging the cable into the charger; keep it running 10 s after VBUS appears.
  2. Count Rx records by SOP kind. Answers: (a) any SOP' with Discover_Identity REQ = the charger
     queries the cable and the K2 hears the request (yes/no); (b) any SOP' ACK carrying the
     cable's ID header = the cable's reply is visible, so a cable sniff feature exists (yes/no);
     (c) any SOP'' frame = the cable has a second plug that is queried (yes/no).
  3. Repeat on the Baseus top plug with no cable (expect all three: no, matches the ports spec).
  4. Own read, for the S7 fix: K2 off the charger, EPR cable on the bottom port, arm any spoof
     (Tools -> Spoof), then Tools -> E-marker. Before S7 this read with Auto-CRC on never received
     the reply; the pass criterion is an `[EMARK] SOP' PD...` line and a SOP' Rx in the trace.
  Write the (a)/(b)/(c) answers into the "Cable sniff" row of `docs/K2_PORTS_SPEC.md`.

### S8. Far-side role observer + report-card v7

- Files: `include/k2/pd/pdtrace_link.hpp` (add `peer_dfp`, `pr_swap_seen`, `dr_swap_seen`,
  `vconn_swap_seen`; the decoder at `src/pd/pdtrace_link.cpp:89-92` already reads bit 8; add bit 5),
  `src/ui/ui_lines.cpp` (one line "peer: SRC/SNK DFP/UFP, swaps: PR DR VC"), `include/k2/chargertest/report_card.hpp`
  (`kFormat = 7` (6 is taken by the UFCS modes): add the swap counters from S1 and the peer role byte; fix the stale "formats 0-2"
  comment at `:18`/`:33`), `tools/k2_remote.py` card decoder, `tests/test_pdtrace_link.cpp`,
  `tests/test_report_card.cpp`.
- Tests first: feed a header stream with DR_Swap/Accept, PR_Swap/Accept/PS_RDY and assert the
  observer flags; card round-trip v7 and v6-read compatibility.
- Hardware: passive inline between Baseus and a laptop (the laptop usually DR_Swaps to DFP);
  evidence: lines page screenshot + `pd_trace.py capture`.
- Cost: ~400 B text, ~8 B bss. Risk: none.
- Dependencies: S1 (counters), S2 (where the role line is shown).

### S9. Doc fixes (one small docs commit, no code)

- `docs/SOURCE_SINK_DETECTION.md` (fix S2 and S5, refresh the 21 moved lines, strip nothing else);
  `docs/agents/TRIGGER_RESEARCH_NOTES.md` (mark T5/T6/T7/T16/T21 with the verdicts above). Can run with any step.

### Order and parallelism

1. Wave 1 (3 agents): S1, S2, S3.
2. Wave 2 (2 agents + device): S4 (device for the scan check), S6 (device for the OnePlus runs).
3. Wave 3 (device): S5 (CUKTECH UFCS, HID-powered), S7 (sniff measurement).
4. Wave 4: S8 (after S1 + S2 merge), S9 any time.

### Scenario x evidence matrix

| Scenario | Charger / partner | Step | Evidence that proves it |
|---|---|---|---|
| Partner DR_Swap answered | laptop inline + Baseus | S1 | `pd_trace.py capture`: DR_Swap → Reject ≤ 30 ms; `[PD] swap rx` K2LOG; no Soft_Reset in the next 2 s |
| Partner PR_Swap refused, VBUS steady | laptop inline + Baseus | S1 | same capture + `transition dump` shows VBUS flat; `ping` uptime ≥ run time (no reboot) |
| Identity page: charger only | Baseus 140W, CUKTECH C1 | S2 | screenshot; `k2_remote` UsbLink snapshot matches the page text |
| Identity page: cable fields | e-marked EPR cable | S2 | screenshot; `k2_remote emarker` VDOs |
| Hunt row with a real 6 A cable | CUKTECH C1 + 6 A cable | S3 | card JSON: Mi 20V6A / 11V6A Pass; `pd_trace.py capture` with the chain and the 6 A APDO re-announce |
| Hunt row with a 3 A cable | CUKTECH C1 + 3 A cable | S3 | card JSON: Skip/NotUnlocked "cable cap" |
| Hunt row on a non-Xiaomi source | Baseus 140W, Lenovo C135 | S3 | card JSON: Skip/NotUnlocked "no 0x2717"; run time not extended by 2 s per row |
| Ra-level line attach | Baseus + EPR cable both orientations | S4 | `[PD] CC scan` line with the classified levels; `k2_remote state` active_cc; contract reached |
| UFCS handshake attempt | CUKTECH C3, CUKTECH USB-A | S5 | `[UFCS] hs:` lines; `transition dump --png` (VBUS cut on A port expected); card row reason |
| VOOC clock + accepted ident | OnePlus 120W + both cables | S6 | `vooc_edges.py` decode of the edge capture: 0x88 → 0x04D; card row Pass or `NoClock` |
| SOP'' / cable sniff | Baseus + EPR cable, direct and inline | S7 | `pd_trace.py live` frame count by SOP type; result row in K2_PORTS_SPEC |
| Far-side role line | laptop inline + Baseus | S8 | lines page screenshot; card v7 peer byte; `pd_trace.py capture` with the swap |
| Self-powered safety (every device step) | any | all | `k2_remote ping` uptime vs wall clock before and after; `-dirty` absent in the hash |

---

## 4. Decisions for the user

1. **DR_Swap answer: Reject vs Not_Supported.** Reject is the PD-correct answer for a port that
   can be UFP only (both revisions); Not_Supported is what the K2 already sends for VCONN_Swap.
   Plan assumes Reject for DR/PR and Not_Supported for FR. Say if you want one answer for all.
2. **Fix the two research notes or keep them local.** They have five wrong claims; the plan's S9
   either fixes them in-tree or they stay untracked with this review as the record.
3. **UFCS label.** If S5 finds no UFCS source among the chargers on hand, should the unit show
   "UFCS (unverified)" in Results, or be hidden from the default order until proven?
4. **Report-card format bump (S8).** v7 adds swap counters and the peer role; the web decoder
   and `k2_remote` need the matching change. Bump now, or hold until a bigger card change?
5. **S7 priority.** The sniff measurement answers a yes/no about a feature that may not exist;
   run it only if a free device slot appears, or drop it.

---

## 5. Side issues seen during the review (unfixed)

- `src/drivers/fusb302.cpp:397-398`: with Ra mode on and `g_rd_op` on CC1, `apply_rd()` can OR to Rd on
  both lines (inferred, untested). `test_key_mode_never_presents_rd_on_both_lines` covers the key
  path only.
- `src/pd/pd_sink.cpp:933-934` comment says the step-1 default is 0x27170106 "flip that one
  constant"; the constant is 0x27170102 (`pd_sink.hpp:321`). `src/pd/pd_sink.cpp:961-962` uses the
  literal `0x27170106u` for the 0x27170205 reply with no named constant.
- `include/k2/pd/pd_sink.hpp:216-219` says proactive Discover SVIDs is "Default OFF"; `:299` is
  `proactive_discover_ = true` and CHARGER_QUIRKS says default-on.
- `include/k2/chargertest/report_card.hpp:18` and `:33` list formats 0-2; `kFormat` was 5 when this was written (now 6, the UFCS modes).
- `src/fastcharge/dpsniff.cpp:218-229` `selftest()` drives D+ (V0/V3p3 twice) while `include/k2/fastcharge/dpsniff.hpp:9`
  says the sniffer never drives. Guarded by "only with nothing attached" in a comment, not in code.
- "~25 ms Request after Source_Capabilities" is documented (`docs/HW_TEST_PROCEDURES.md:696`) but
  has no code constant; the sink answers a re-announce immediately (`src/pd/pd_sink.cpp:591-606`).
- `docs/SOURCE_SINK_DETECTION.md`: 21 moved line citations (list in §1.2) and the `poll()` /
  `attach_scan()` error (S2).
