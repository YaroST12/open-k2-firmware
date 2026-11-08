# Source/sink detection: per-arm engineering map

How the firmware tells, for each connected arm (top captive male USB-C plug, bottom female
receptacle; `docs/K2_PORTS_SPEC.md`), what the partner is: power role, data role, CC orientation,
Rp/Rd advertisement, VCONN, the negotiated PD contract, e-marker identity, and the negotiated USB
data protocol. Complements `docs/ARCHITECTURE.md` ("PD driver model"); this document covers what
our firmware does.

> **Review status (2026-11-07):** reviewed against the code and the hardware results in
> `docs/agents/DETECTION_IMPLEMENTATION_PLAN.md` §1. Known corrections to the text below: Xiaomi 120 W needs the
> SVID 0x2717 auth chain (the 6 A APDOs appear only after it); VOOC clocks without a load (frames are 9 clocks, accept
> 0x04D); UFCS: D+/D- on USB-A (what the K2 implements, USART2) and CC/BMC on USB-C per
> `docs/CHARGING_PROTOCOL_MAP.md` §14; DR_Swap/PR_Swap are control types 9/10; several file:line citations moved.

## 0. Hardware facts that shape everything

- One FUSB302B at I2C 0x22 (`src/drivers/fusb302.cpp:55`), INT_N on PB12/EXTI12 (`include/k2/drivers/pins.hpp:38`,
  `src/drivers/fusb302.cpp:1562`). One chip, one BMC codec, one RX FIFO — the K2 can only *listen actively* on
  one CC line at a time.
- CC routing: PC14 = CC1 switch, PC15 = CC2 switch (high = closed), PC13 = VCONN/Ra control
  (`include/k2/drivers/pins.hpp:27-29`, `src/drivers/fusb302.cpp:198` `cc_switches()`). Each port's CC pair reaches the
  K2 nets; the switch mask (3/1/2, `include/k2/pd/pd_ccscan.hpp:12`) selects which port's live line the
  FUSB302 sees. `active_cc` is **not** an orientation label — it is whichever line the scan chose
  (`docs/K2_PORTS_SPEC.md`, "Electrical identity").
- ADC: PA4 = CC1, PA5 = CC2 (ADC1_IN4/5, no divider, 3.3 V full scale), PA2/PA3 = D-/D+
  (`include/k2/drivers/pins.hpp:49-54`, `include/k2/drivers/adc_calc.hpp:20`). A 5 V net reads as ~3.3 V (saturated).
- FUSB302 register bits used for sensing (`include/k2/pd/pd_phy.hpp:14-45`): SWITCHES0 PDWN1/2 (Rd),
  MEAS_CC1/2, VCONN_CC1/2; SWITCHES1 TX_CC + Auto-CRC; MEASURE MDAC 210 mV (`kMdac210mV`, :35) or
  2.6 V (`kMdacVconnMv`, :34); STATUS0 COMP (bit 5, line above MDAC) and BC_LVL (bits 1:0 = Rp current
  1 = 500 mA, 2 = 1.5 A, 3 = 3 A — `include/k2/pd/pd_passive.hpp:167-170` `typec_ma`); CONTROL1 ENSOP1/2.
- D+/D- go through a switch network (PA7, PB0, PB1, PB2 — `include/k2/drivers/pins.hpp:41-44`,
  `include/k2/drivers/dpdm.hpp`) used for BC1.2/proprietary probing and edge sniffing (`src/fastcharge/dpsniff.cpp:34`
  EXTI7 = PA7 = D+, EXTI2 = PB2 = D-). **SuperSpeed lanes are not wired to the K2 at all**
  (`include/k2/pd/usb_link.hpp:14-16`).

## 1. Per-arm detection pipeline

### 1.1 CC attach / orientation / Rp advertisement

| What | Where | How |
|---|---|---|
| Attach scan (~2 Hz while unattached) | `src/drivers/fusb302.cpp:1619` `attach_scan()`, called from `poll()` (`src/drivers/fusb302.cpp:1776`) | `detect_cc_orientation()` |
| CC line choice | `src/drivers/fusb302.cpp:436-530` `detect_cc_orientation()`; pure choice logic `include/k2/pd/pd_ccscan.hpp:12-56` (`kCcScanMasks`, `choose_cc`, `isolated`) | Samples every switch mask × FUSB302 line: MEASURE = 210 mV MDAC, SWITCHES0 = MEAS_CCn; STATUS0 COMP set = an Rp is on that line; BC_LVL gives its current. Lines above 2.6 V (MDAC `kMdacVconnMv`) are rejected as VCONN/unloaded-Rp, not attach (`src/drivers/fusb302.cpp:463-470`). Highest isolated BC_LVL wins, first breaks a tie; with both lines live (direct plug) a tie goes to CC1. Result: `g_active_cc` + `pdtrace::cc(Attach,...)` (`src/drivers/fusb302.cpp:526`) |
| Quick "is an Rp on the wire now" | `src/drivers/fusb302.cpp:414` `rp_on_line()` | Comparator look at both lines at 210 mV when no active CC known; passive |
| Rp current advertisement | `src/drivers/fusb302.cpp:1776-1781` (`g_bc_max` from STATUS0), `include/k2/pd/pd_passive.hpp:167-183` `typec_ma`/`typec_only_ma`; accessor `fusb302::typec_ma()` (`include/k2/drivers/fusb302.hpp:289`) | BC_LVL latched highest since attach; a source with no PD caps within 3 s (`kTypeCOnlyMs`) is reported as a plain Type-C source |
| Detach | `include/k2/pd/pd_passive.hpp:87-121` `DetachTracker` (VBUS < 3.0 V for 1 s, or no COMP on the active line for 500 ms, or inline sink-Rd gone for 500 ms); driven in `poll()` `src/drivers/fusb302.cpp:1778-1793`; `src/drivers/fusb302.cpp:1574` `pdtrace::cc(Detach,...)` | Passive; Rd then held off `kRdHoldMs` = 1.5 s (`pd_passive.hpp:20`, `RdHold`) |
| CC voltages (both nets) | `fusb302::sink_cc(cc1_mv, cc2_mv)` (`include/k2/drivers/fusb302.hpp:283`), fed from ADC; shown on the Lines page `src/ui/ui_pages.cpp:1369`; sink-band test `include/k2/pd/pd_passive.hpp:14-17` `sink_present` (~1.5 V ± 700 mV = a real sink's Rd against an Rp) | |

### 1.2 PD sniffing (passive decode — how a *foreign* contract is seen)

- Boot is fully passive: Auto-CRC off, no Rd, SOP/SOP'/SOP'' receive on
  (`src/drivers/fusb302.cpp:353` `configure_passive()`, `include/k2/pd/pd_phy.hpp:68` `passive_config`,
  `include/k2/pd/pd_passive.hpp:185-200` `CrcDemand`/`crc_demand`).
- RX path: INT_N ISR sets a flag (`src/drivers/fusb302.cpp:1562`); `service()` every 1 ms (`src/drivers/fusb302.cpp:1797-1868`)
  drains up to 4 frames per pass from the FIFO, records each to pdtrace (`pdtrace::rx`,
  `include/k2/pd/pdtrace.hpp:318`), and hands it to `handle_frame()` (`src/drivers/fusb302.cpp:651`) →
  `pd::Engine::on_message()` (`src/pd/pd_engine.cpp:33`) — the pure decoder — plus the transmit-side
  engines (`pd_sink`, `pd_identify`, `pd_probe`, `pd_reread`, `pd_vconn`).
- `poll()` every 20 ms (`src/drivers/fusb302.cpp:1755`) runs the attach scan, detach tracker, STATUS0 watch and `service()`.
- Decoders: PDO/APDO (`src/pd/pd.cpp:96` `decode_pdo`), RDO (`src/pd/pd.cpp:8` `decode_rdo`),
  Source_Capabilities incl. EPR chunked list (`src/pd/pd.cpp:190` `decode_source_caps`,
  `src/pd/pd_engine.cpp:165` `on_epr_caps_chunk`), e-marker Discover Identity (`src/pd/pd.cpp:206`
  `parse_emarker`), VDMs (`src/pd/pd_vdm.cpp`, `include/k2/pd/pd_vdm.hpp`), extended messages
  (`include/k2/pd/pd_ext.hpp`, `src/pd/pd_engine.cpp:147` `on_ext_message`), USB-link classification
  (`include/k2/pd/usb_link.hpp`, see §4).

### 1.3 Active PD as sink (`pd_sink`) and the "source" side

- Sink engine (the K2 *as sink*, i.e. the arm under test is a source): `include/k2/pd/pd_sink.hpp:110`
  `SinkState { Idle, EprEntering, EprWaitCaps, Requesting, Transition, Ready, Aborting, Failed }`,
  implemented in `src/pd/pd_sink.cpp` (1017 lines); driver entry points `fusb302::sink_request`
  (`src/drivers/fusb302.cpp:1873`), `sink_release` (:1902), `sink_hard_reset`, `epr_info_request`.
  It sends Request/EPR_Request, tracks Accept/PS_RDY, PPS keep-alives, the Xiaomi 0x2717 auth chain
  (`include/k2/pd/pd_sink.hpp:134-143`), answers Get_Sink_Cap/Get_Sink_Cap_Extended.
- `src/pd/pd_source.cpp` is **not** a source engine: the K2 never presents Rp (`docs/K2_PORTS_SPEC.md`
  capability table, "Present Rp: no"). The file only fills a fake `SourceCaps` for UI/debug
  (`src/pd/pd_source.cpp:6-40`, `fake_fill`). The K2's only source-side behaviour is the VCONN-swap
  experiment and the Ra/e-marker-spoof mode below.
- Identify / probe / re-read (active interrogation of the partner on SOP):
  `include/k2/pd/pd_identify.hpp:23` `IdStep { Identity, Svids, Modes, SinkCap, Status, Revision }`,
  `include/k2/pd/pd_probe.hpp:22` `ProbeStep { Identity, MfrInfo, CapsExt }` (feeds the report card,
  `include/k2/pd/pd_probe.hpp:50-58` `probe_result`), `include/k2/pd/pd_reread.hpp` (`reread_start`,
  `src/drivers/fusb302.cpp:1958`). All refused or confirm-gated while inline.

### 1.4 VCONN and e-marker

- E-marker mode choice: `include/k2/pd/pd_emarker_plan.hpp:13-19` — Vconn (nothing supplying VBUS:
  the K2 powers the cable and sends SOP' Discover Identity itself) vs Sniff (a source is attached:
  listen only, never fight it for VCONN).
- Vconn scan: `src/drivers/fusb302.cpp:295` `scan_orientation(sw0, sw1)` (SWITCHES0 0x24/0x25 then 0x18/0x26
  = VCONN on the non-active line, both orientations; SOP' Discover Identity, PD 3.0 then PD 2.0,
  80 ms per attempt), called from `read_emarker_impl` (`src/drivers/fusb302.cpp:1210`, `:1236-1238`).
  Cable answers land in `pd::Engine::cable` via `on_cable_message` (`src/pd/pd_engine.cpp:115`),
  decoded by `parse_emarker` (`src/pd/pd.cpp:206`); `fusb302::emarker()`, `cable_seq()`,
  `physical_cable()` (`include/k2/drivers/fusb302.hpp:274-277`).
- Passive cable sniffing: any SOP'/SOP'' frame on the wire is decoded whenever heard
  (`src/drivers/fusb302.cpp:333`, `:684`); `Emarker::sop` records whether it answered on SOP' or SOP''
  (`include/k2/pd/pd.hpp:76`).
- Spoof (answer a charger's SOP' Discover Identity with a saved identity): `include/k2/pd/emk_spoof.hpp`
  (`is_discover_identity_req` :30, hooked via `fusb302::set_sop_prime_hook`, `include/k2/drivers/fusb302.hpp:265-268`;
  ENSOP1 handling `src/drivers/fusb302.cpp:340` `ctl1()`), quirk tables `include/k2/pd/emk_quirk.hpp`,
  learner `include/k2/pd/emk_learn.hpp` + `fusb302::cable_session`/`cable_exchange` (`src/drivers/fusb302.cpp:1287`,
  `include/k2/drivers/fusb302.hpp:160-165`). Ra mode (PC13 high ≈ 1 kΩ on CC1, Rd on CC2): `include/k2/pd/emk_ra.hpp`,
  `fusb302::ra_set` (`src/drivers/fusb302.cpp:1994`), guard `include/k2/pd/ra_guard.hpp`.
- VCONN_Swap experiment (K2 takes the VCONN source role): `include/k2/pd/pd_vconn.hpp:22-31`
  `VconnState { Off, SwapSent, Settling, On, Releasing, Failed }`; glue `src/drivers/fusb302.cpp:1408-1460`
  (PC13 + `kVconnCc1/2` on the line opposite the active one, `:395-407` `apply_rd()`), remote-only.

### 1.5 USB data role / protocol on D+/D-

- PD-negotiated data role & alt modes: `usblink::Classifier` (`include/k2/pd/usb_link.hpp:85`),
  fed every frame by the engine (`include/k2/pd/pd_engine.hpp:82`); Enter_USB, Enter_Mode (TBT 0x8087,
  DP 0xFF01), DP Configure, Discover Identity UFP/DFP VDOs, cable VDOs. Displayed via
  `fusb302::usb_link(dp_mv, dm_mv)` (`src/drivers/fusb302.cpp:2069`) and `pdtrace::view::Link`
  (`include/k2/pd/pdtrace_link.hpp:14-44`, Enter_USB/TBT/DP/cable/peer fields).
- D+/D- idle levels decide only when no PD data spoke: D- pull-up = low speed, D+ pull-up =
  full speed ("USB 1.1 vs 2.0 chirp is not observable", `include/k2/pd/usb_link.hpp:29-33`);
  raw mV on the Lines page (`src/ui/ui_pages.cpp:61-62, 1369`).
- Edge sniffer for proprietary D+/D- protocols: `src/fastcharge/dpsniff.cpp` (EXTI on PA7/PB2, cycle-stamped
  edge ring, `include/k2/fastcharge/edges.hpp`), feeding `fc_engine`.
- BC1.2 + proprietary D+/D- protocols (QC2/QC3/QC3+, AFC, FCP, SCP, VOOC/SVOOC, UFCS) are *actively
  probed* by the charger-test runner, not passively detected: `src/fastcharge/fc_engine.cpp` (`Proto` enum
  :43-52, `Fail::Detected` :85), pin driving `include/k2/drivers/dpdm.hpp:75-132` (`set_dp/set_dm`,
  `play`, VOOC `rx_command`/`tx_reply`, `discharge`/`start_reset`). Passive D+/D- observation is
  limited to voltage levels and edge timing.

### 1.6 Where results surface

- PD trace / Black Box: binary record ring `include/k2/pd/pdtrace.hpp` (Rx/Tx/TxResult/Reset/Cc/Reg/
  Status0/Irq/Link/Sink/Vbus/Poll/Mark records, trigger captures), host-decoded by `tools/pd_trace.py`
  (remote cmd 0x70). `pdtrace::link` records the inline/auto-sink verdict (`pdtrace.hpp:321`).
- UI: Home/Detail/Charger/PD/Diag/Lines pages (`src/ui/ui_pages.cpp`; bar names :176) — PDO list,
  contract (`pd::Contract`), cable (`emarker_kv`, :703-711), Type-C current (:502), D+/D-/CC mV
  Lines page (:1369), Diag (`fusb302::rx_count()`, `device_id()`, `active_cc()` —
  `include/k2/drivers/fusb302.hpp:292-294`).
- Report cards: `include/k2/chargertest/report_card.hpp` — `Identity` (VID/PID/PDP), per-group verdicts,
  APDO `Range`s, per-EPR-level `EprLevel`s (28/36/48 V), and the v5 `Cable` block
  (VID/PID, max V/A, kind: real/clone/built-in, observed/spoofed/self-powered flags, :99-109).

## 2. "This arm is a source" vs "this arm is a sink"

The K2 is passive by default, so classification is evidence-based, per arm:

- **Arm is a source** when its CC line shows an Rp: STATUS0 COMP above 210 mV in the attach scan
  (`src/drivers/fusb302.cpp:436-530`), BC_LVL giving the advertised current. If Source_Capabilities arrive on
  that line, it is a PD source and `pd::Engine::caps` holds its offer (`src/pd/pd_engine.cpp:33-70`).
  If no PD traffic within 3 s of attach, it is a Type-C-only source (`typec_only_ma`,
  `include/k2/pd/pd_passive.hpp:176-183`) — a BC1.2-only charger looks exactly like this until the
  D+/D- probe runs.
- **Arm is a sink** when its CC line sits in the Rd band (~1.5 V ± 700 mV with none of our own Rd
  on it — `pd::sink_present`, `include/k2/pd/pd_passive.hpp:14-17`, fed by `sink_cc`) or when a
  Request the K2 did not send is sniffed (`contract.requests != g_requests_base`,
  `src/drivers/fusb302.cpp:76, 1871` `trigger_refusal()`). VBUS rising above 5.5 V without the K2 causing
  it, or > 50 mA flowing, latches **inline mode** (`include/k2/pd/pd_link.hpp:44-64`, `Link::inline_mode`;
  published to the guard via `link::set_inline_mode`, `src/ui/ui.cpp:790`, `src/pd/link.cpp:13`).
- Inline mode is the "source on one arm, sink on the other" verdict: while latched, every active
  operation (trigger, Identify, Get_Source_Cap, VCONN scan, rescan) refuses or needs a confirm
  (`pd::can_trigger`, `include/k2/pd/pd_passive.hpp:123-146`; `pd_rescan.hpp:26-28` — an inline rescan
  plan is empty by construction).
- **Both arms sources (charger-to-charger)**: each port's Rp reaches the nets; the scan picks one
  live line (`choose_cc`, ties to CC1), the other arm is visible only through its CC mV on the
  Lines page and the lack of VBUS/PD. Neither side sees an Rd, so no VBUS and no PD contract —
  the K2 reports "attached, Type-C source, no PD" per `typec_only_ma`, and any VBUS that *does*
  appear (a Try.SRC brick) shows up as VBUS + a foreign Request → inline. There is no explicit
  source-to-source state; quirks per charger are logged in `docs/CHARGER_QUIRKS.md`.
- **Both arms sinks**: no Rp anywhere → the scan finds nothing, `active_cc = 0` (`src/drivers/fusb302.cpp:523`),
  no attach, no PD. Only the D+/D- and VBUS readings remain.
- **Known quirks**: PLUS key galvanically coupled to CC2 (every press sags CC2 —
  `docs/K2_PORTS_SPEC.md` "Known port-specific quirk", `docs/CHARGER_QUIRKS.md` 2026-11-05 entry);
  Rd/Rd on both lines is the Debug Accessory signature and some sources (OnePlus 120 W) then never
  re-arm Rp (`include/k2/pd/pd_ccmode.hpp:8-10`); a source that sees its caps GoodCRC'd but unanswered
  hard-resets within ~30 ms (`pd_ccmode.hpp:81-90` `acked_caps_need_request`).

## 3. Negotiated contract: capture and display

- Passive capture: `pd::Engine` keeps `caps` (Source_Capabilities, incl. EPR chunked reassembly
  `src/pd/pd_engine.cpp:165-...`, `epr_partial` flag), `contract` (`include/k2/pd/pd_engine.hpp:47-60`:
  RDO position/current/voltage, `accepted`, `rejected`, `ready` = PS_RDY seen after the latest
  Request), `vdm`, `ext`, `cable`, `usb` — even when the K2 missed the caps, a sniffed Request +
  PS_RDY still shows the contract.
- Active capture: the sink engine's own `Plan`/`SinkStatus` (`include/k2/pd/pd_sink.hpp:158-170`) —
  pos/mv/ma, EPR mode, hard-reset count, Xiaomi auth state; logged to pdtrace as Sink records
  (`src/drivers/fusb302.cpp:638` `trace_sink()`).
- Display: Connection page and `pdtrace_view` (`src/pd/pdtrace_view.cpp`, message-name table :23), PD trace
  marks `kMarkCaps`/`kMarkAutoReq` (`include/k2/pd/pdtrace.hpp:52-53`), report-card group/range/EPR
  fields (`include/k2/chargertest/report_card.hpp:73-97`), plus `pdtrace::view::Link` for Enter_USB/TBT/DP
  (`include/k2/pd/pdtrace_link.hpp:14-44`).
- EPR: EPR_Source_Capabilities chunks are reassembled into `caps` with `epr = true` when complete
  (`include/k2/pd/pd.hpp:29-32`, `src/pd/pd_engine.cpp:165`); the informational SPR-mode
  EPR_Get_Source_Cap goes to `epr_info` only (`include/k2/pd/pd_engine.hpp:70-72`); the sink's real EPR
  entry is `SinkState::EprEntering/EprWaitCaps` with `EPR_Mode` Enter/Acknowledged/Enter_Failed data
  (`include/k2/pd/pd_sink.hpp:110, 168-170`); per-level 28/36/48 V results land in
  `report_card::EprLevel` (`include/k2/chargertest/report_card.hpp:88-93`).

## 4. State machines relevant to role/protocol detection

- `pd::CcChoice` attach scan: pure, `include/k2/pd/pd_ccscan.hpp` — masks {3,1,2}, isolated-COMP,
  highest BC_LVL; executed in `detect_cc_orientation()` `src/drivers/fusb302.cpp:436-530`. Transitions:
  none → live line (attach, `pdtrace::cc(Attach)`) / nothing (`active_cc = 0`).
- `pd::DetachTracker` (`include/k2/pd/pd_passive.hpp:87-121`): attached → detached on VBUS-low-1 s,
  CC-gone-500 ms, or inline sink-gone-500 ms; `RdHold` keeps Rd off 1.5 s.
- `pd::Link` (`include/k2/pd/pd_link.hpp:65-96`, `src/pd/pd_link.cpp`): attached/waiting/stale + the
  inline latch (> 50 mA, foreign Request, unexplained VBUS > 5.5 V; clears on detach or 2 s quiet).
- `pd::Engine` (`src/pd/pd_engine.cpp:33-...`): stateless per-frame decoder accumulating caps /
  contract / vdm / ext / cable / usb; `reset_source()` on new caps, hard reset, detach.
- `pd::Sink` (`include/k2/pd/pd_sink.hpp:110` + `src/pd/pd_sink.cpp`): Idle → EprEntering → EprWaitCaps →
  Requesting → Transition → Ready (keep-alives), Aborting/Failed; `take_resume()` re-requests
  PDO 1 after a source hard reset.
- `pd::Vconn` (`include/k2/pd/pd_vconn.hpp:22-115`): Off → SwapSent → Settling → On → Releasing → Off.
- `pd::Identify`/`pd::Probe`/`pd::Reread` step machines (`include/k2/pd/pd_identify.hpp:23`,
  `include/k2/pd/pd_probe.hpp:22`, `include/k2/pd/pd_reread.hpp`).
- `pd::TriggerPrep` (`include/k2/pd/pd_passive.hpp:236-270`): Open → Wait → Delay → Send, to revive
  a source whose caps went un-ACKed.
- `pd::AutoCrcGate`/`ContractLinger` (`include/k2/pd/pd_passive.hpp:185-234`): when the chip may ACK /
  when a contract expires (30 s idle linger).

## 5. Gaps — not detected or unreliable

1. **No DR_Swap / PR_Swap handling.** The decoders only *name* them (`src/pd/pdtrace_view.cpp:23`); the
   engines ignore control types 20/21, so a role swap mid-session is invisible except as raw
   frames. Data role after DR_Swap is not tracked (`usblink::Classifier` never sees a "we are now
   UFP" signal).
2. **SOP'' (cable plug 2) e-marker reads**: receive is enabled and decoded (`Emarker::sop = 2`,
   `include/k2/pd/pd.hpp:76`), but the active VCONN scan only ever drives one orientation pair
   (`src/drivers/fusb302.cpp:1236-1238`), and SOP''-only cables are only seen when a source queries them.
3. **Try.SRC / Try.SNK**: never emitted (the K2 is never a source; Try.SNK is implicit in Rd
   presentation) and not explicitly decoded when a partner sends them.
4. **VCONN swap**: implemented as a remote-only experiment (`pd_vconn`), never auto; partner
   VCONN_Swap while we are not the VCONN source is answered Not_Supported (`pd_vconn.hpp:99-104`).
5. **No SuperSpeed measurement**: USB3/USB4/TBT/DP results are *negotiated-over-PD* statements, not
   measured link rates (`include/k2/pd/usb_link.hpp:14-16`); USB 1.1 vs 2.0 is undecidable from D+/D-
   idle levels (:31-33). BC1.2 detection (DCP short, CDP, SDP) is only exercised by active probes.
6. **Single-arm listening**: only one CC line reaches the FUSB302 at a time; the *other* arm's PD
   traffic is invisible while the scan sits on the first (relevant for charger-to-charger and for
   a sink on the far port that speaks PD — only its Rd band / foreign Request / current are seen).
7. **Rp vs Rd ambiguity with our own Rd presented**: BC_LVL then reads the Rp/Rd divider; the code
   keeps the highest level since attach (`g_bc_max`) but a source/sink decision made while our Rd is
   on the line is heuristic. `active_cc` is not an orientation label (K2_PORTS_SPEC).
8. **E-marker sniffing through a cable is untested** whether the K2 hears the charger's SOP' on the
   shared CC wire when a real e-marked cable is on the bottom port (K2_PORTS_SPEC table row
   "Unknown"); blind-receiver bug history (ENSOP1 under Auto-CRC, fixed 2026-11-05) means old logs
   with Auto-CRC on cannot be trusted for SOP' counts.

### What a captured connection example should contain (test-matrix inputs)

For each scenario (charger alone on top plug; charger + cable on bottom; charger–K2–laptop inline;
charger-to-charger; sink-only; e-marked cable alone; EPR source) capture:

1. **PD trace ring or trigger capture** (`k2_remote` cmd 0x70 / `tools/pd_trace.py`; captures via
   `pdtrace::Captures`, `include/k2/pd/pdtrace.hpp:118-176`) — must include the Cc Attach/Detach
   records, every Rx/Tx with SOP token, Reg writes (SWITCHES0/1, MEASURE, CONTROL1), Sink records,
   and Mark records (caps/auto-req/VCONN).
2. **K2LOG lines**: `[FUSB302] Active CC detected` / `Detach:` (`src/drivers/fusb302.cpp:520, 1788`),
   `[PD] CC scan ...` (:512), `[EMK]`/`[VCONN]` lines (:1229-1330, 1419-1454), `[PD] Rd/Auto-CRC`
   (:380-384).
3. **Screens/JSON**: Connection page (caps, contract, VDM), Detail page (cable `emarker_kv`), Lines page
   (CC1/CC2/D+/D- mV, `src/ui/ui_pages.cpp:1369`), Diag page (`rx_count`, `device_id`, `active_cc`),
   and for runs the **report card JSON** (`include/k2/chargertest/report_card.hpp:99-140` incl. the v5 cable
   block and EPR levels).
4. **VBUS/current samples** (pdtrace Vbus records or `[PWR]` heartbeats) to distinguish source
   (VBUS up) from sink-only and to catch foreign contracts (inline latch evidence).

With those five artifacts per scenario, every row of §1-§2 can be pinned to observed records, and
the §5 gaps become either confirmed-absent (raw frames show the message was sent and ignored) or
confirmed-missed (decoder never fired).
