# K2 USB-C ports — per-port capability spec (2026-11-05)

Sources: our firmware (`src/pd/emk_spoof.cpp`, `src/drivers/fusb302.cpp`, `include/k2/drivers/pins.hpp`), hardware runs
2026-11-04/05 (`docs/CHARGER_QUIRKS.md`; top-port evidence `runs/emk-top/` in the main checkout, which is gitignored).

Evidence tags used below: **[measured]** hardware log or run, **[code]** our source, **[inferred]** reasoning with
no direct witness.

## Status: top-port e-marker spoof SOLVED (2026-11-06)

Verdict: PC13 puts an Ra of about 1 kOhm on CC1 (cc1 about 322 mV with the charger waiting and the K2
passive, as a real cable's Ra). With it high before Rd appears on CC2 alone, a source sees a sink plus a
powered cable, sends SOP' Discover Identity, and the armed e-marker spoof answers on the Rd line
[measured 2026-11-06, `runs/emk-ra/` in the main checkout, gitignored].
- Recipe (Ra mode: `fusb302::ra_set(true)`, then `rd_for_run(true)`): PC13 high, then Rd on CC2 only; the
  e-marker scan is skipped (VCONN would fight the Ra); spoof armed; the VCONN switches never close. Ra
  survives the units' detach/attach cycles until the run ends. `k2_remote pd-reread --ra on|off` is the
  manual route. Refused with a sink inline, in a contract, while busy, with a boot pin that reaches CC1 (OK key) or VCONN applied. A PLUS pin (Rd on CC2 alone) is the very Rd this mode presents, so a charger-powered run may spoof; the Ra guard cannot judge a PLUS boot (PLUS pulls CC2 down, the lines never both read Rp) and assumes a direct receptacle charger on the top plug.
- Evidence: Baseus 140 W run with the spoof armed passes "EPR fixed: 28 V, all pass" (28.28 V measured,
  `runs/emk-ra/c3/ct.txt`); the same trigger on the bottom/passive path negotiates 28 V 5 A
  (`runs/emk-ra/b/run4.txt`).
- UI, one identity list in the wizard's Cable step (step 3 of 5): rows Next, OFF, the four captured built-ins
  below (their chip labels), then one row per saved clone in the e-marker library. The built-ins are always
  there, even with an empty library. The chosen row wears the "USE" pill; OK on a row only selects it, nothing is
  armed. OFF is the old behaviour (no Ra, no spoof). START (Review) is two OKs: the first opens the confirm window
  and touches nothing; the second runs the Ra guard (a refusal opens the Refuse stage, nothing armed yet), arms the
  chosen identity, turns Ra mode on until the run ends and presents Rd with the wait for the charger as the last
  stage; the run then starts by itself. The last choice is the default (remembered across power cycles in `/cable.cfg`). Review shows it as the "Spoof"
  row and the card header shows "SPOOF: <name>". There is no dialog at START any more.
  Remote: `k2_remote charger-cable [off|builtin|builtin:<id|name>|saved:N]` (CMD 0x0D, kind Builtin carries
  the built-in id in seq) and `ct_start.py --cable` (default off).
- Ra guard (`include/k2/pd/ra_guard.hpp`, host-tested in `tests/test_ra_guard.cpp`; built, not yet on hardware): the
  Ra spoof is only meaningful with a receptacle charger plugged DIRECTLY into the top plug (the charger then sees
  Ra + Rd = sink on a powered cable). Through a cable the single CC wire carries one Rp: on CC1 the charger sees an
  Ra alone ("cable, no sink") and never turns VBUS on, on CC2 it cannot see our Ra and the e-marked cable answers
  SOP' itself. Field report 2026-11-06: hub PD output, USB 3.2 C-to-C cable on the bottom port, a saved clone chosen
  -> nothing came up, the K2 froze about 2 s in START and dropped back. The dialog therefore reads the topology
  BEFORE any Ra goes on (K2 passive, source waiting unattached) and classifies: Rp on both CC lines = direct
  receptacle (allowed), Rp on one line = cable (refused), none = no source (refused), no reading / not passive /
  VBUS up = unknown (never blocks). Thresholds [measured 2026-11-06]: Rp present at >= 2400 mV (waiting charger
  Rp 3.2 to 3.3 V on both lines; idle CC2 net 1.7 V, Ra 0.3 V, idle CC1 0 V), "already attached" at VBUS >= 1000 mV,
  sampled every 100 ms into a peak window of 8 samples (~0.8 s) so a PLUS press that sags a line never turns a
  direct charger into a "cable". Refusal: a full-width message ("Spoof needs the charger plugged in the K2 directly
  (top plug). Through a cable the cable itself answers.") with "Run with OFF" and "Cancel"; BACK is Cancel and
  leaves the refusal on Review. The guard now speaks on the SECOND OK (the first only opens the confirm window), still
  before anything is armed. OFF is never refused. `ct_start.py --cable ...` prints the same text and exits 6
  after cancelling the refusal.
- START wait (non-blocking): the second START OK with Rd needed (an identity, or a sink-less charger alone) used to
  block the main loop up to ~21 s (VBUS 5 to 10 s, caps 2 s, one Rd re-present). It is now the "Waiting for
  charger" stage, driven by `poll()`: `fusb302::service()` and the UI tick keep running, the screen shows
  "Rd on Ns" and the VBUS reading, and OK or BACK cancels the START with the Rd released (remote stage name
  `waiting`, index 13). A press in that stage cancels, so remote callers poll the stage and press OK only
  at `review` (`ct_start.py` does, twice). Waiting is the LAST stage before the tests: when it ends the final
  preflight runs on the fresh reading and the run starts by itself, with no third press; a refusal there lands on
  Review with the reason on the footer and a passive K2.
- Built-in cable identities (`k2::emrec::builtin_info`, `emarker spoof builtin:<id|name>`, spoof sub-command
  `[5, 3, id]`). All answer SOP' Discover Identity as PD spec rev 2 passive cables. Provenance: ids 1, 2 and 4
  are captured from the user's cables 2026-11-06 (every VDO word is byte-exact; id 1 is the set that passed EPR
  28 V on the Baseus 140 W); id 0 is a synthetic generic 20 V 5 A cable.

  | id | Label | Names accepted by `emarker spoof` | Rating | In the dialog |
  |---|---|---|---|---|
  | 0 | generic 20V 5A | `generic` | 20 V 5 A, no EPR | no (remote only) |
  | 1 | EPR 50V 5A | `epr-50v`, `generic-epr` | 50 V 5 A, EPR | yes (default built-in) |
  | 2 | CUKTECH 6A | `cuktech-6a`, `cuktech-240w` | 50 V 5 A, EPR | yes |
  | 3 | (retired alias of 4) | `oneplus-mag` | as id 4 | no |
  | 4 | OnePlus 100W | `oneplus` | 30 V 5 A | yes |

  Id 3 was a second capture of the OnePlus cable ("OnePlus 100W (mag)"); it duplicated id 4 (same VID, same
  ratings) and is retired. The id stays reserved so stored data keeps working: a remote `[5, 3, 3]`, a CHARGER_CABLE
  seq 3, an old `/cable.cfg` record or a journal-resumed run with 3 all read as id 4 (OnePlus 100W), and every reply,
  stored record and report card kind then carries 4. Ids are never renumbered.

  The OnePlus USB-C to USB-A SUPERVOOC cable has no e-marker (no answer to Discover Identity in either
  orientation), so identifying an A-to-C SUPERVOOC cable is out of scope; id 4 is the e-marked
  USB-C to USB-C OnePlus cable only. The report card's cable kind records the built-in id (0 = saved clone).
- The investigation below (parked 2026-11-05) is kept for its evidence; routes 0 to 3 are superseded.

Earlier verdict (2026-11-05, parked): with the chargers on hand (Baseus 140 W, CUKTECH 10/120 W) no
software route made a charger query the top captive plug's "cable" (SOP'), so the spoof had nothing to answer.
- Both chargers run cable discovery only after seeing Ra at attach. Baseus: EPR bit set,
  Enter_Failed data 2 (not VCONN source), VCONN_Swap Not_Supported. CUKTECH: no SOP' on a fresh
  attach, VCONN_Swap Accept then VBUS drop.
- No existing K2 configuration presented Ra; an MCU-driven pull-down is unsafe (C3 below).

Routes considered at the time:
0. **Ra-only adapter (best route, needs a cheap part, no K2 change)** [inferred]. Use a male-to-female USB-C
   pass-through with VBUS, GND and CC straight through, and **1 kΩ from the male plug's B5 (VCONN) to GND**.
   B5 is not passed to the K2, and the adapter has no e-marker chip. Charger → adapter → K2 (top plug, or a
   dumb cable to the bottom port).
   - The charger sees Rd from the K2 plus Ra, so it becomes VCONN source (about 5 mA into the resistor).
     Its VCONN never reaches the K2's CC nets or ADC pins.
   - It then sends SOP' Discover Identity on the CC wire. The K2 spoof is the only responder, so there is
     no chip to collide with.
   - Risks: the spoof's reply timing and content are untested against a real charger, and the charger may
     re-query or use SOP''. The spoofed cable VDO must claim EPR, 50 V and 5 A.
1. Bottom port, CUKTECH + a cable known to have no e-marker, spoof armed. This settles the unlogged
   2026-11-04 "CUKTECH sent SOP', spoof answered" report and proves the spoof end to end.
2. TRIED 2026-11-05, no route: the keys cannot present Ra. Baseus on the top plug, K2 passive, polled
   with raw ADC and VBUS (`runs/epr/buttons/hold2.tsv`) [measured]: at rest cc1 = cc2 = about 3.2 V (the
   3 A Rp). PLUS held about 5 s: CC2 steady 1.72 V, CC1 unchanged at 3.2 V, about 5.2 kOhm against the
   3 A Rp (330 uA), i.e. an Rd. The Baseus took it as a sink and raised VBUS to 5.12 V with no load, and
   fell back on release. OK held: no change on either net (minimum 3199 / 3200 mV). The earlier
   ~5.5 kOhm figure came from one tap with the K2's own Rd also on. An Rd gets a sink attach, never the
   cable query, so the top-port spoof still has no route short of a real Ra (route 3 below or an
   external Ra-only adapter).
3. Hardware: a switched ~1 kΩ Ra to GND on one CC net. This needs a board inspection first (ADC
   series resistor, switch rating).

## Connector gender

- **Top port: captive male USB-C plug** on the K2's own lead — the charger plugs into it directly, no
  intermediate cable. The "cable e-marker" the charger sees is whatever chip is in that captive plug.
- **Bottom port: female USB-C receptacle** — takes any cable; the charger sees that cable's e-marker.

The spoof rule below therefore applies with the captive top plug as the "cable" on the top port.

## Electrical identity of the two ports

Both USB-C ports are **electrically identical** from firmware's point of view:

- One port's CC pair maps to CC1, the other to CC2; the PC14/PC15 analog switch matrix
  (`pins.hpp:27-28`) routes the CC lines to the single FUSB302B [code]. **`active_cc` is not an orientation
  label** [measured 2026-11-05, top port]: it is whatever line the attach scan chose. With Rd on both lines
  a tie goes to CC1 by scan order (`include/k2/pd/pd_ccscan.hpp`, `detect_cc_orientation`) [code], and a reboot
  clears it. **Both CC pins of the top plug reach K2 nets**: an unattached charger's Rp shows on both,
  about 3.2 V each (Baseus 3208 mV, CUKTECH saturated at the 3.3 V ADC limit) [measured].
- PC13 is the VCONN supply control; used with `SWITCHES0` VCONN_CC1/2 to read a cable e-marker on the
  line that is not the active one [code]. Its physical function is not proven.
- **No firmware-controlled Ra path exists.** The FUSB302B can only pull down with Rd 5.1 kΩ (PDWN1/2); the
  PC13/14/15 pins only drive switch inputs, so no pin-mode setting makes a resistor on a CC net [code,
  `debug_cc_matrix`]. (Ra is about 1 kΩ, not 5.1 kΩ; older text in this repo called it 5.1 k.) The old
  claim that the 2026-11-04 `debug cc-matrix` sweep "verified" this is **withdrawn**: that sweep ran with
  SOP' receive (ENSOP1) off whenever Auto-CRC was on, so it could not have seen a charger SOP' request
  (see "Blind-receiver bug" below). It proved nothing either way.

### Blind-receiver bug (found and fixed 2026-11-05, k2/emk-top-port)

Before 2026-11-05, with Auto-CRC on, CONTROL1 had ENSOP1 clear even while the spoof hook was armed [code;
fixed in `636bdd3`, merged as `fa36a8e`: `pd_phy::control1(auto_crc, sop_prime)` sets bit 0 while a hook is
armed]. Every earlier "spoof requests=0" result from an *active sink* is therefore blind: the
2026-11-04 cc-matrix sweep (`9cbaa2a`) and the Baseus runs of build `e1692fa`. Passive sniffing had ENSOP1
on and is unaffected. New runs (c1..c4, build after the fix) show `CONTROL1 <= 0x05 ENSOP1` while a
contract is held [measured].

## Can we spoof the e-marker? Only in response, and only if the charger asks

The spoof responder is **port-agnostic** (`emk_spoof::on_sop_prime`, hooked for every received
SOP' frame on the attached line) [code]. It fires when a **charger-initiated SOP' Discover
Identity REQ** arrives on the active CC line.

A source sends SOP' Discover Identity only if, at attach, it saw **Ra** on the pin that is not Rd and
applied VCONN there [inferred from the Type-C rules; consistent with every observation below].

| What the charger sees between it and the K2 | Charger sends SOP' to K2? | Spoof fires? |
| :--- | :--- | :--- |
| Dumb: top captive plug, no Ra on its second CC pin | **No** [measured: Baseus, CUKTECH, fresh Rd attach, spoof armed, ENSOP1 on] | No |
| Real e-marked cable on the bottom port | The charger queries the cable chip; whether the K2 also hears it on the shared CC wire is **not tested** with the fixed receiver | Unknown |
| Bottom port + cable, PLUS/OK-held boot (2026-11-04 note) | Reported yes (CUKTECH), log not retained, cable type not recorded, not reproduced | Reported yes |

So the earlier table row "Dumb ... Yes, on re-attach (observed CUKTECH)" is **not established**: the one
report rests on a note whose evidence was not kept (`bc4d78a`, "PLUS/OK boot both engage a CC resistor"),
and on the top port the same charger was silent in today's runs. The two can both be true if the PLUS/OK
key coupling (below) produces an Ra-like level at attach, or if the cable used then was e-marked; neither
is shown.

### Top-port results, 2026-11-05 (post-fix receiver) [measured]

- **Baseus 140 W** (top plug): PDO1 has the EPR bit; EPR_Mode Enter -> Enter_Acknowledged, then
  **Enter_Failed data 2** (source failed to become VCONN source). VCONN_Swap -> **Not_Supported**. No SOP'
  at any time. So the charger *wants* to see the cable (EPR needs a VCONN-powered cable check) but there is
  no cable chip and the K2 refuses/cannot take the VCONN role.
- **CUKTECH 10/120 W** (top plug): Source_Caps max 20 V 3 A (PDO1 5 V 3 A ... PDO5 20 V 3 A, PPS 5-11 V,
  PPS 5-20 V 3 A, both power-limited) while its SCEDB PDP is 120 W. A fresh Rd attach with the spoof armed
  gave **no SOP'** (`c4-replug`, ENSOP1 on, Request 1.8 ms after caps). VCONN_Swap -> **Accept**; the K2
  then put VCONN on the non-active net (`SWITCHES0=0x25`, PC13 on). Within ~3 ms the charger's CC level
  dropped (BC_LVL 1) and VBUS fell to 0 within ~320 ms with no Hard Reset message; a later re-attach
  recovered. See "C3" for what this implies.

### Baseus 140 W on the top captive plug, earlier entry (2026-11-05, build e1692fa) — corrected

Spoof armed (saved cable 1 USB4G3-240W, then builtin `generic-epr`), then each provocation tried:
plug-in, Hard Reset (`pd reset`), Get_Caps reread (`pd-reread`), `debug detach`/`attach`,
`debug cc-matrix` CC pulses (both orientations, 0.3-1.5 s low), and a full QC2+QC3 run. Result every time:
spoof requests=0, replies=0. **That run was blind** (ENSOP1 off under Auto-CRC), so it does not show the
charger stayed silent; the post-fix runs above do. Its caps list of 6 PDOs (5/9/12/15/20 V 3 A + PPS
3.3-11 V 3 A) means **no EPR PDOs in the SPR capability list** — the old wording "no EPR" was wrong: the
charger sets the EPR bit in PDO1 and offers EPR (28 V 5 A, PDO8) over a path where it is satisfied about the
cable (`docs/CHARGER_QUIRKS.md`, Baseus 140W). 28 V is reached when the charger accepts the cable
check; with the captive plug it answers Enter_Failed data 2. QC2/QC3 over D+/D- pass at 5/9/12 V and do
not change the PD caps. Report card 28 (v5) records the cable block as not observed, default limit
20000 mV / 3000 mA.

## C3: Ra on the plug's second CC pin — safety and feasibility study (2026-11-05)

Goal: make the charger see Ra on the pin that is not Rd at attach, so it applies VCONN and asks the
"cable" (the K2) for SOP' Discover Identity, which the spoof answers. No schematic exists; every
component value below is therefore **[inferred]** unless tagged.

### Q1. What is on each CC net

- FUSB302B CC1/CC2 pins sit behind the PC14 (CC1) / PC15 (CC2) analog switches, high = closed; PC13
  `cc_vconn_gnd` is the VCONN supply control, our code drives it high for "VCONN on" [code,
  `fusb302.cpp:153-157`]. PC13..15 are the CC/VCONN switches: wrong levels disconnect or short
  the CC path; PC13's physical function is unproven. Switch part and voltage rating: unknown; it passes the
  K2's own VCONN, so it is at least rated for that (below).
- MCU: ADC1 channel 4 = **PA4 = CC1**, channel 5 = **PA5 = CC2**, 12-bit, full scale 3.3 V, converted
  `raw * 3300 / 4096` [code, `adc_calc.hpp:20`, `fusb302.cpp` `cc_net_mv`]. **No divider**: an attached Rd line reads about
  1.57-1.58 V = 330 µA x 5.1 kΩ [measured, c1]. A net can read at or above full scale (CUKTECH Rp raw 4092,
  3294 mV) [measured], so the ADC cannot show anything above 3.3 V. A series resistor in front of the pin
  cannot be ruled in or out.
- Which side of the switches the ADC taps is **not established**: all hardware runs had both switches closed
  (mask 3); the mask-0 data of the 2026-11-04 sweep was not retained.
  Consequence for the UI: the Connection page's port-aware empty-state hints ("top plug" / "via cable") are a
  **passive** classification of the Rp level on the two CC lines (`raguard::classify`; Rp >= 2400 mV on both =
  direct receptacle, on one = single-CC cable; Unknown unless passive and VBUS < 1000 mV). They are the likely
  reading of that pattern, not proof of which physical port or side is sensed, and the hint never drives an
  active CC operation.
- PA0 aux (ADC channel 0) sits behind an 11:1 divider and reads about 5.02 V: a 5 V board rail, the likely
  VCONN source [measured value, inferred role].
- PLUS (PA13) is hard-wired to the CC2 net; see "Known port-specific quirk".

### Q2. Voltage on a net during a VCONN event

- Our e-marker read: 5 V VCONN through `PC13=1`, `PC14/PC15` and `Switches0` VCONN_CC1/2 onto the
  non-active CC pin [code]. Bounds what the net tolerates: about 5 V from the board's own source.
- Our K2 did the same in the CUKTECH VCONN_Swap run (`SWITCHES0=0x25 PDWN1 MEAS_CC1 VCONN_CC2`, PC13 on)
  [measured, c4/s5]. The ADC read **3292-3294 mV** on that net before and after, i.e. at its ceiling,
  so the actual voltage is **unobservable** with this ADC [measured]. No K2 fault or reset shows in the
  dump [measured].
- Not 3.3 V: our reader uses a 5 V supply [code]; the CUKTECH reacted within 3 ms as if the
  net had risen above its own Rp level [measured, inferred cause].

### Q3. Risk if a charger puts 5 V VCONN onto the K2 net

- **FUSB302 pin and analog switch**: the same pin and switch carry the K2's own 5 V VCONN [code], so
  a 5 V source is within what they already see [inferred]. Charger VCONN is 4.75-5.5 V class [inferred]; a
  few hundred mV above our own rail is the unquantified margin.
- **MCU PA4/PA5**: analog mode, VDDA 3.3 V. 5 V is above the usual "VDD + 0.3 V" absolute rating, so the
  pin would conduct through its clamp diodes. Injection current = (5 V - ~3.6 V) / (series R + source
  impedance); with no series R evidenced it is limited only by the charger's VCONN current limit. The K2
  already exposes the pin to its own VCONN with no recorded damage [measured, one run], so the risk is
  real but already taken, not new [inferred].
- **Back-feed**: a charger-applied 5 V onto the net could reach the K2's VCONN source through the
  FUSB302 VCONN switch's body path [inferred]; consequence unknown.
- **Charger side, Ra about 1 kΩ**: 5 V / 1 kΩ = 5 mA, normal and far below a VCONN current limit
  (typically hundreds of mA) [inferred]. **Hard short**: OCP trips; the port shuts, VBUS drops, a replug or
  re-attach recovers [inferred; the CUKTECH VCONN event is the nearest evidence: BC_LVL 1 within 3 ms, VBUS
  0 within ~320 ms, no Hard Reset, recovered on re-attach, measured].

### Q4. Does any existing configuration present Ra-like load (< ~0.8 V at 3 A Rp)?

- **Matrix / pin modes: no.** The sweep logs (13 combos) are gone and it was blind, but by construction
  PC13/14/15 are control inputs of switches; changing their pin mode (including InPull-down) loads the
  control line and creates no resistor on a CC net [code, `debug_cc_matrix`].
- **PDWN1/2**: Rd 5.1 kΩ on a line, 1.68 V at 3 A Rp. Rd on both lines is a debug-accessory pattern, not
  Rd + Ra [code, inferred].
- **PLUS key (CC2 only)**: pressed with an Apple 60 W attached (1.5 A Rp, BC_LVL 2 before), CC2 sagged to
  477 mV (BC_LVL 1) [measured, `docs/CHARGER_QUIRKS.md`]. With a 180 µA source that is Rd || R = 2.65 kΩ,
  so the key's coupling is roughly 5.5 kΩ [inferred from one number], an extra Rd-class pull-down, **not
  1 kΩ**. Alone in parallel with PDWN2 it gives about 2.7 kΩ = 0.88 V at 330 µA, just above the Ra band
  of a 3 A source, so not a dependable Ra. A K2 held-PLUS boot on the bottom port stays powered "only while
  PLUS is held, with or without our Rd" [measured 2026-11-04], so
  the coupling is itself a pull-down the charger accepts as a sink.
- **OK key (CC1)**: the 2026-11-04 note says OK-boot also "engages a CC resistor"; no measurement kept.
- **MCU pin as pull-down** (PA4/PA5 output low): a hard low is below vRa so a source would read Ra, but it
  is a 25-50 Ω driver: 5 V VCONN would push ~100-200 mA into a pin rated for tens of mA [inferred]. Rejected.

### Q5. Verdict and test proposal

- **Any MCU-driven Ra: unsafe**, and with no resistive Ra not useful.
- **C3 as written (Ra from the existing hardware): not available**. No configuration presents a defined
  ~1 kΩ; a real Ra would be a board change (1 kΩ ±20 % to ground on the plug's second CC pin).
- **PLUS/OK key-assisted attach: safe with conditions, outcome unknown.** It creates no electrical state the
  board has not already had: PLUS-boot attach is a normal boot path and was run on
  2026-11-04 without damage. What is open is only whether the charger classifies ~0.9 V on the second pin as
  Ra. Conditions: (1) charger alone on the plug, no phone or laptop on the other port, no load; (2) K2 powered
  from the HID port (C-to-A) or the charger itself, not both; (3) Rd on **one** line only, the other pin left
  to the key coupling; (4) stop on any VBUS drop below 4 V for more than 100 ms, or any BC_LVL on the Rd
  line other than the Rp level; (5) CUKTECH only first (known recovery), never a charger that has latched
  off before; (6) no `fault`, no VCONN from our side during the test (swap refuse stays on).

Proposed sequence (a proposal; no device was used):

1. Boot in passive inline mode; arm the spoof (saved id 5 or builtin `generic-epr`), ENSOP1/ENSOP2 on with
   Auto-CRC on (post-fix receiver), `vconn` state forced `off`.
2. Pre-arm **before attach**: FUSB302 `SWITCHES0 = 0x02` (PDWN2 only), CC
   switches mask 3, PC13 low. Black Box trigger on the first received SOP'.
3. User holds PLUS, then plugs the top plug into the charger; release after 5 s.
4. Log lines: `[CCATT]` with BC_LVL/ADC raw for both lines at +1, +10, +100, +500 ms after attach; the
   `SWITCHES0/1`, `CONTROL1` values; count of SOP' RX and the first Source_Capabilities time; `[VCONN]`
   marks if any.
5. Pass: a charger-initiated SOP' Discover Identity is received and answered, VBUS stays up. Fail: no SOP'
   within 3 s, then record both nets' mV. Abort: condition 4.
6. Repeat once with OK held and Rd on CC1 (`0x01`); then record the null result in
   `docs/CHARGER_QUIRKS.md` so the question is closed.

Unknowns only a schematic or a probe on the board can close: series resistor in front of PA4/PA5, which
side of PC14/PC15 the ADC taps, the analog switch's rating, the PLUS coupling resistance, whether PC13
selects a 5 V source or ground.

## Per-port capability table (identical for top and bottom)

| Capability | Top port | Bottom port | Notes |
| :--- | :--- | :--- | :--- |
| PD sink (Rd presented, trigger/requests) | yes | yes | Boot modes: PLUS held = Rd on CC2, OK held = Rd on CC1; run START presents Rd on the Rp line |
| PD passive sniffing | yes | yes | FUSB302 on the active line |
| E-marker read (SOP'/SOP'' interrogation) | yes | yes | VCONN on the inactive CC (PC13). SOP'' is received (CONTROL1 0x67 for the whole read) but never answered. |
| Cable sniff (charger's own SOP' exchange, no VCONN) | untested | untested | S7 built the counters-free measurement: `pd_trace.py live` shows every Rx with its SOP kind. Procedure in `docs/DETECTION_IMPLEMENTATION_PLAN.md` S7; replace "untested" with the result |
| E-marker spoof | yes | yes | Only in response to a charger SOP'; on the top plug none seen (see above) |
| QC2/QC3/AFC/FCP/SCP/VOOC on D+/D- | yes | yes | `dpdm` switch network |
| VBUS/current measure | yes | yes | INA226 (36 V ceiling) / INA228/237 (48 V) |
| Test load | yes | yes | PA8 PWM, device-wide |
| Present Ra to a charger (provoke SOP', act as e-marker uninvited) | no firmware path | no firmware path | Rd-only pull-downs; see C3 |
| Present Rp | no | no | FUSB302 pull-up path not used |

## Known port-specific quirk

- The **PLUS button (PA13) is galvanically coupled to the CC2 net** — presses pull CC2 toward 0 V
  (hard low unplugged, ~480-660 mV sag with a charger attached). Runtime firmware has no PLUS-to-CC
  logic; CC2 readings glitch on every PLUS press (details + measurements: `docs/CHARGER_QUIRKS.md`,
  "PLUS key is coupled to CC2"). CC1 is unaffected.
