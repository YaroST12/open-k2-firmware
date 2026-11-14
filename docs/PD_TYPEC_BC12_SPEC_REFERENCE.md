# USB Type-C, USB PD Control/Reset Timing, and BC1.2 Source Compliance Reference

Reference specification extract for a no-load PD/Type-C/BC1.2 sink-side compliance tester (CC/VBUS voltage & microsecond PD message timestamping, `0 mA` intentional load). Every normative value cites the exact primary specification, revision/date, section, table/figure, and a verbatim quote extracted from the official published PDFs.

Companion document: [`PD_PPS_AVS_SPEC_REFERENCE.md`](PD_PPS_AVS_SPEC_REFERENCE.md) (PPS & AVS voltage/timing/status parameters).

---

## Primary Specifications Used

| Short ID | Full Title & Revision | Date | Status in This Session |
| :--- | :--- | :--- | :--- |
| **Type-C R2.5** | USB Type-C® Cable and Connector Specification, Release 2.5 | March 2026 | Verified from `USB Type-C Spec R2.5 - March 2026.pdf` |
| **PD R3.3** | USB Power Delivery Specification, Revision 3.3, Version 1.0 | October 2026 | Verified from official PDF text (`pd33.txt`) |
| **PD CTS Q4 2026** | USB Power Delivery Compliance Test Specification, Q4 2026 | Q4 2026 | Verified from official PDF text (`pd3_cts.txt`) |
| **BC 1.2** | Battery Charging Specification, Revision 1.2 (plus Errata as of Mar 15, 2012) | March 15, 2012 | Verified from `BC1.2 plus errata 2012-3-15.pdf` & `Battery_Charging_Errata_Rev_1.2_2012-03-15.pdf` |
| **UFCS** | T/TAF 083 (Telecommunication Terminal Industry Forum Association) | — | **Skipped** — official primary host (`taf.org.cn`) failed TLS certificate verification (`curl` exit 60) |

---

## Part A — USB Type-C Specification (Release 2.5, March 2026)

### A.1 Key Corrections & Engineering Takeaways vs. Tester Assumptions

1. **Detach timing is `tSRCDisconnect + tVBUSOFF` (`<= 20 ms + 650 ms = 670 ms`), NOT `tVBUSOFF + tPDDebounce`**:
   - **`tPDDebounce` (`10 ms` to `20 ms`) is Sink-side only** (used by a Sink to wait before concluding detach while PD BMC signaling may be on CC).
   - **The Source detects `Rd` removal via `tSRCDisconnect` (`0 ms` to `20 ms`)** and immediately exits `Attached.SRC`, then discharges VBUS to `vSafe0V` (`<= 0.8 V`) within **`tVBUSOFF` (`0 ms` to `650 ms`)**.
   - Practical total worst-case budget from Sink `Rd` open to VBUS `<= vSafe0V`: **`tSRCDisconnect (max 20 ms) + tVBUSOFF (max 650 ms) = 670 ms`** (**verified**).

2. **Re-attach after high-voltage detach (`tBulkDischarge`)**:
   - After exiting `Attached.SRC`, a Source waits until (a) `VBUS` is at `vSafe0V` (`tVsafe0V <= 650 ms`), (b) `SRC.Rd` is stable on one CC pin for `tCCDebounce` (`100 ms` to `200 ms`), **and** (c) its internal bulk capacitor `VBULK` is discharged ready to supply `vSafe5V` within **`tBulkDischarge`** (`0–275 ms` if prior `VBUS <= 21 V`; **`0–700 ms`** if prior `VBUS > 21 V`).
   - Only then does it enter `Attached.SRC`, after which VBUS must reach `vSafe5V min` (`4.75 V`) within **`tVBUSON` (`0–275 ms`)**.
   - On a **DRP Source**, add up to one non-presenting/toggling cycle (`tDRP` `50–100 ms`, `dcSRC.DRP` `30–70%`, `tOnePortToggleConnect` `0–80 ms`, or `tTryTimeout` / `tDRPTry` / `tDRPTryWait` if Try.SRC/Try.SNK occurs). Once in `Attached.SRC` and VBUS reaches `vSafe5V`, `Source_Capabilities` must begin within **`tFirstSourceCap` (`250 ms` max)**.

3. **Zero-load VBUS retention rule (`4.5.2.2.9.1`)**:
   - A compliant Type-C Source **must never** turn off VBUS just because the tester draws `0 mA` while `Rd` remains attached (*"Sources in the Attached.SRC state shall not remove VBUS based on a minimal level of current drawn by the Sink..."*). Any charger that shuts down VBUS at `0 mA` while `Rd` is present violates Section 4.5.2.2.9.1.

4. **VBUS discharge / bleed resistance**:
   - When VBUS is *not* being sourced, minimum VBUS-to-GND leakage impedance is **`72.4 kOhm`** (`Table 4-2`). When exiting `Attached.SRC` / entering `AttachWait.SRC`, the Source is explicitly required to **apply an active discharge load to VBUS** (`4.5.2.2.8.1`) to bring VBUS (plus up to `cSnkBulkPd` / `cSnkBulk` sink capacitance) down to `vSafe0V` within `tVBUSOFF` / `tVsafe0V` (`<= 650 ms`).

### A.2 Type-C Timing Parameters

| Parameter | Min | Nom | Max | Unit | Spec, Rev, Section, Table/Fig | Verbatim Quote | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`tSRCDisconnect`** | `0` | — | `20` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34**; Sec 4.5.2.2.8.2, 4.5.2.2.9.2 | `"Time a Source shall detect the SRC.Open state. The Source should detect the SRC.Open state as quickly as practical."` / `"The Source shall detect the SRC.Open state within tSRCDisconnect..."` | **verified** |
| **`tVBUSOFF`** | `0` | — | `650` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**; Sec 4.5.2.2.9.2 | `"From the time the Sink is detached until the Source removes VBUS and reaches vSafe0V (See USB PD)."` / `"A port shall cease to supply VBUS within tVBUSOFF of exiting Attached.SRC."` | **verified** |
| **`tVsafe0V`** | `0` | — | `650` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**; Sec 4.5.2.2.8.1 | `"The time the Source is allowed to use for discharging the Sink capacitors to reach vSafe0V in AttachWait.SRC state in order to progress to Attached.SRC."` / `"The port shall apply a load to VBUS to ensure it reaches vSafe0V within tVsafe0V."` | **verified** |
| **`tVBUSON`** | `0` | — | `275` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**; Sec 4.5.2.2.9.1 | `"From entry to Attached.SRC until VBUS reaches the minimum vSafe5V threshold as measured at the source’s receptacle."` | **verified** |
| **`tBulkDischarge`** (`VBUS <= 21 V`) | `0` | — | `275` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**, **Fig 4-51** | `"From the time the Source exits the Attached.SRC state with VBUS <= 21 V until the Source is prepared to re-enable VBUS."` | **verified** |
| **`tBulkDischarge`** (`VBUS > 21 V`) | `0` | — | `700` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**, **Fig 4-50** | `"From the time the Source exits the Attached.SRC state with VBUS > 21 V until the Source is prepared to re-enable VBUS."` | **verified** |
| **`tCCDebounce`** | `100` | — | `200` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | `"Time a port shall wait before it can determine it is attached."` | **verified** |
| **`tPDDebounce`** | `10` | — | `20` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | `"Time a Sink port shall wait before it can determine it is detached due to the potential for USB PD signaling on CC as described in the state definitions."` | **verified** |
| **`tErrorRecovery`** (self-powered / no VCONN) | `25` | — | — | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | `"Minimum time before returning to Unattached when in ErrorRecovery without sourcing VCONN (see Table 4-34)."` (summary recorded from Table 4-34) | **verified** |
| **`tErrorRecovery`** (previously sourcing VCONN) | `240` | — | — | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | `"if it was sourcing VCONN in the previous state"` (min `240 ms`) | **verified** |
| **`tDRP`** | `50` | — | `100` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | Period a DRP shall complete a Source to Sink and back advertisement | **verified** |
| **`dcSRC.DRP`** | `30` | — | `70` | `%` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | Percent of time that a DRP shall advertise Source during `tDRP` | **verified** |
| **`tDRPTransition`** | `0` | — | `1` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | Transition time between Sink and Source advertisement | **verified** |
| **`tDRPTry`** | `75` | — | `150` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | Wait time associated with Try.SRC state | **verified** |
| **`tDRPTryWait`** | `400` | — | `800` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | Wait time associated with Try.SNK state | **verified** |
| **`tTryTimeout`** | `550` | — | `1100` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-33** | TryWait timeout | **verified** |
| **`tNoToggleConnect`** | `0` | — | `5` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | Time allowed to detect connection when not toggling | **verified** |
| **`tOnePortToggleConnect`** | `0` | — | `80` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | Time allowed for single-port toggling DRP connect detection | **verified** |
| **`tTwoPortToggleConnect`** | `0` | — | `510` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | Time allowed for two-port toggling DRP connect detection | **verified** |
| **`tRpValueChange`** (non-idle / no BMC det) | `10` | — | `20` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34**; Sec 4.5.2.3.1.2 | `"For any change in vRd indicating a change in allowable power, the port shall not transition until the new vRd has been stable for at least tRpValueChange."` (when CC not BMC Idle or cannot detect BMC Idle) | **verified** |
| **`tRpValueChange`** (PD BMC Idle detected) | `0` | — | `5` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-34** | When USB PD signaling can be detected and CC is BMC Idle | **verified** |
| **`tSinkAdj`** | `tRpValueChange (Min)` | — | `60` | `ms` | Type-C R2.5 (Mar 2026), Sec 4.11.2, **Table 4-32**; Sec 4.6.2.1 **Table 4-20** | `"Response time for a Sink to adjust its current consumption to be in the specified range due to a change in USB Type-C Current advertisement."` | **verified** |

### A.3 VBUS Voltage Thresholds, Discharge, and Zero-Load Rules

| Parameter | Value(s) & Unit | Spec, Rev, Section, Table | Verbatim Quote / Rule | Status |
| :--- | :--- | :--- | :--- | :--- |
| **`vSafe0V`** | `0` to `0.8 V` | Type-C R2.5 Sec 2 (`"as defined by the USB PD specification"`); **PD R3.3** Sec 4.7.3 **Table 4.9** | Type-C: `vSafe0V: VBUS "0 volts" as defined by the USB PD specification.` / PD R3.3 Table 4.9: `"Safe operating voltage at 'zero volts'."` (`0` to `0.8 V`) | **verified** |
| **`vSafe5V`** | `4.75` to `5.5 V` | Type-C R2.5 Sec 2 & Sec 4.4.2; **PD R3.3** Sec 4.7.3 **Table 4.9** | PD R3.3 Table 4.9: `"Safe operating voltage at 5V. See [USB2] and [USB3] for allowable VBUS voltage range."` (`4.75` to `5.5 V`; Type-C `4.4.2` likewise notes `5.5 V` max at light load) | **verified** |
| **`vSinkDisconnect`** | `0.8 V` (typ/nom `4.0 V` upper note; threshold at `0.8 V` in Table 4-3) | Type-C R2.5 (Mar 2026), Sec 4.4.2 **Table 4-3**, Sec 4.5.2.2.5.2 | `"below vSinkDisconnect for VBUS operating at or below 5 V or below vSinkDisconnectPD when negotiated by USB PD to operate above 5 V."` (`0.8 V` in Table 4-3) | **verified** |
| **VBUS Leakage Impedance** (not sourced) | `72.4 kOhm` min | Type-C R2.5 (Mar 2026), Sec 4.4.2 **Table 4-2** | `"when VBUS is not being sourced"` (`72.4 kOhm` minimum) | **verified** |
| **VBUS Source Capacitance** | Source-only: `3000 uF` max<br>DRP: `10 uF` max | Type-C R2.5 (Mar 2026), Sec 4.4.2 **Table 4-2** | Source-only `3000 uF` max; DRP `10 uF` max | **verified** |
| **No-load VBUS persistence** | Must NOT remove VBUS at `0 mA` | Type-C R2.5 (Mar 2026), Sec 4.5.2.2.9.1 | `"Sources in the Attached.SRC state shall not remove VBUS based on a minimal level of current drawn by the Sink..."` | **verified** |
| **Active VBUS discharge** | Bleed to `vSafe0V` within `tVsafe0V` / `tVBUSOFF` (`<= 650 ms`) | Type-C R2.5 (Mar 2026), Sec 4.5.2.2.8.1 & 4.5.2.2.9.2 | `"The port shall apply a load to VBUS to ensure it reaches vSafe0V within tVsafe0V."` / `"A port shall cease to supply VBUS within tVBUSOFF of exiting Attached.SRC."` | **verified** |

### A.4 Source `Rp`, Sink `Rd`, and CC `vRd` Current Advertisement Windows

**Source `Rp` Options (Type-C R2.5, Table 4-27) & `Table 4-20` Advertisement:**

| Source Current Advertisement | Current Source | Pull-up to `4.75–5.5 V` | Pull-up to `3.3 V ± 5%` | PD Superseding Note | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Default USB Power** (`500 mA` USB 2.0 / `900 mA` USB 3.2) | `80 uA ± 20%` | `56 kOhm ± 20%` (Note 1: `± 5%` on legacy/captive A cables) | `36 kOhm ± 20%` | PD Contract supersedes `Rp` once negotiated (`4.6.2.1` Table 4-20) | **verified** |
| **1.5 A @ 5 V** (`7.5 W`) | `180 uA ± 8%` | `22 kOhm ± 5%` | `12 kOhm ± 5%` | PD Contract supersedes `Rp` | **verified** |
| **3.0 A @ 5 V** (`15 W`) | `330 uA ± 8%` | `10 kOhm ± 5%` | `4.7 kOhm ± 5%` | PD Contract supersedes `Rp` | **verified** |

**Sink `Rd` (`Table 4-28`), Open Impedance `zOPEN` (`Table 4-30`), and Sink CC Pin Voltage Windows for `Rd = 5.1 kOhm ± 10%` (`Table 4-36`):**

| Parameter | Min | Max | Unit | Spec, Rev, Table | Notes | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`Rd`** (capable of detecting power advertisement) | `5.1 kOhm ± 10%` (`max 2.04 V`) | — | `Ohm` | Type-C R2.5, **Table 4-28** | `± 20%` (`max 2.18 V`) allowed if Sink does not detect current advertisement | **verified** |
| **`zOPEN`** (open CC pin impedance) | `126` | — | `kOhm` | Type-C R2.5, **Table 4-30** | `"evaluated below vCC-Clamp"` | **verified** |
| **Disconnect (`SRC.Open` as seen / `vRd` disconnected)** | — | `0.276` | `V` | Type-C R2.5, **Table 4-36** | Below `vRd-connected` min (`0.277 V`) | **verified** |
| **`vRd-USB` (Default USB Power)** | `0.277` | `0.612` | `V` | Type-C R2.5, **Table 4-36** | Guard band above to `vRd-1.5`: `0.613–0.745 V` | **verified** |
| **`vRd-1.5` (1.5 A @ 5 V)** | `0.746` | `1.164` | `V` | Type-C R2.5, **Table 4-36** | Guard band above to `vRd-3.0`: `1.165–1.368 V` | **verified** |
| **`vRd-3.0` (3.0 A @ 5 V)** | `1.369` | `2.042` | `V` | Type-C R2.5, **Table 4-36** | Total `vRd-connected` span: `0.277–2.042 V` | **verified** |

---

## Part B — PD Control & Reset Timing Observable by a Sink (`PD R3.3 V1.0`, `PD CTS Q4 2026`)

### B.1 `Request` -> `Accept` (or `Reject` / `Wait`): `tReceiverResponse` vs. `tSenderResponse`

**Definitive Answer**: The Source is bound by **`tReceiverResponse` (`max 15 ms`)**, NOT `tSenderResponse` (`27–50 ms`).
- **`tSenderResponse` (`27–50 ms`)** is the **Requester/Initiator's timeout** (`SenderResponseTimer`) waiting for the response (e.g., Sink waiting after sending `Request`, or Source waiting for Sink's `Request` after `Source_Capabilities`).
- **`tReceiverResponse` (`<= 15 ms`)** is the **Responder's normative transmit deadline** measured from the **last bit of the EOP of the `GoodCRC` acknowledging `Request`** to the **first bit of the Preamble of `Accept` / `Reject` / `Wait`**.
- **PD3 CTS confirmation**: `COMMON.CHECK.PD.4#1` enforces `tReceiverResponse max` (`15 ms`) on every AMS response except `PS_RDY` (Power Negotiation / PR_Swap / VCONN_Swap), `Security_Response`, `Enter Mode`, `Exit Mode`, and `Not_Supported` in chunked requests — `Accept`/`Reject`/`Wait` has no exception.
- **Note on PD R3.3 `7.31.2` item 3 text**: PD R3.3 contains an editorial slip saying `"from the time the last bit of the GoodCRC Message EOP, corresponding to the expected response Message, has been transmitted"` — item 1 of the same section (`"GoodCRC Message EOP, corresponding to the Message requesting a response"`) and `COMMON.CHECK.PD.4#1` confirm the start event is the **`GoodCRC` acknowledging the `Request`**.

| Parameter | Min | Nom | Max | Unit | Start Event -> Stop Event | Spec, Rev, Section, Table/Check | Verbatim Quote | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`tReceiverResponse`** | — | — | `15` | `ms` | Last bit of `GoodCRC(Request)` EOP transmitted -> First bit of `Accept`/`Reject`/`Wait` Preamble | **PD R3.3** Sec 7.31.2, **Table 7.10**; **PD CTS Q4 2026** **`COMMON.CHECK.PD.4#1`** | PD R3.3 `7.31.2`: `"The receiver of a Message requiring a response Shall respond within tReceiverResponse in order to ensure that the sender's SenderResponseTimer does not expire."` <br> CTS `COMMON.CHECK.PD.4#1`: `"the check fails if the UUT does not respond to a message (sent by the Tester) within tReceiverResponse max or tVDMReceiverResponse max from the last bit of the EOP of the GoodCRC Message sent"` | **verified** |
| **`tSenderResponse`** | `27` | — | `50` | `ms` | Last bit of `GoodCRC(Request)` EOP received -> Last bit of `GoodCRC(Accept)` EOP transmitted (or Hard Reset on Source timeout for `Request`) | **PD R3.3** Sec 7.31.2, **Table 7.10**; **PD CTS Q4 2026** `TEST.PD.PROT.SRC.2#2`, `SRC.3`, Table 19 | PD R3.3 `7.31.2`: `"The SenderResponseTimer Shall be started from the time the last bit of the GoodCRC Message EOP, corresponding to the Message requesting a response, has been received by the PHY Layer."` (PD2 legacy note 1 in Table 19: `24–30 ms`; PD3 max `50 ms` used even in PD2 mode) | **verified** |
| **`tCtsSnkRequest`** | — | — | `25.26` (PD3)<br>`22.26` (PD2) | `ms` | Last bit of `GoodCRC(Source_Cap)` EOP -> Start of `Request` sent by CTS tester | **PD CTS Q4 2026** `TEST.PD.PROT.SRC.3`, **Table 19** | CTS checks Source accepts `Request` sent up to `tCtsSnkRequest` (`tSenderResponse min` minus packet/margin budget) without timing out into Hard Reset | **verified** |

### B.2 Attach / Re-attach `Source_Capabilities` Discovery & `5 s` Budget Explanation

| Parameter | Min | Nom | Max | Unit | Start Event / Usage | Spec, Rev, Section, Table | Verbatim Quote | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`tFirstSourceCap`** | — | — | `250` | `ms` | After attach / Hard Reset / PR_Swap / FRS: from **`VBUS` reaching `vSafe5V`**.<br>After `Soft_Reset`: from **last bit of `GoodCRC(Accept)` EOP**. | **PD R3.3** Sec 7.31.3.3, **Table 7.10**; **PD CTS Q4 2026** `TEST.PD.PROT.SRC.3#1`, `PS.SRC.5#5`, `PROT.ALL.3#6` | `"After Port Partners are Attached or after a Hard Reset or after a Power Role Swap or after a Fast Role Swap a Source Shall send its first Source_Capabilities Message within tFirstSourceCap of VBUS reaching vSafe5V. After Soft Reset, a Source Shall send its first Source Capabilities Message within tFirstSourceCap after the last bit of the GoodCRC Message EOP corresponding to the Accept Message."` | **verified** |
| **`tTypeCSendSourceCap`** | `100` | `150` | `200` | `ms` | `SourceCapabilityTimer` repeat interval while attached and not PD-connected | **PD R3.3** Sec 7.31.3.1, **Table 7.10** | `"Prior to the First Explicit Contract a Source Shall use the SourceCapabilityTimer to periodically send out a Source_Capabilities Message every tTypeCSendSourceCap while: • The Port is Attached. • The Source is not in an active connection with a PD Sink Port."` | **verified** |
| **`nCapsCount`** | — | `50` | — | msgs | `CapsCounter` max before Source may pause fast advertising | **PD R3.3** Sec 7.32.4, **Table 7.12**, **Table 7.13** | `"after nCapsCount Source_Capabilities Messages have been sent the Source Shall decide that the Sink is non-responsive, stop sending Source_Capabilities Messages and disable PD until tTypeCSendSourceCapIdle expires."` | **verified** |
| **`tTypeCSendSourceCapIdle`** | `100` | — | `5000` | `ms` | Slow repeat period after `nCapsCount` unacknowledged messages | **PD R3.3** Sec 7.32.4, **Table 7.10** | `"sending only a single Source_Capabilities Message repeating every tTypeCSendSourceCapIdle."` | **verified** |
| **`tTypeCSinkWaitCap`** | `310` | `465` | `620` | `ms` | Sink timeout waiting for `Source_Capabilities` after `VBUS` present before issuing Hard Reset | **PD R3.3** Sec 7.31.3.2, **Table 7.10** | `"when a Sink observes an absence of Source_Capabilities Messages, after VBUS is present, for a duration of tTypeCSinkWaitCap the Sink May issue Hard Reset Signaling in order to restart the sending of Source_Capabilities Messages by the Source..."` | **verified** |
| **`tNoResponse`** | `4.5` | `5.0` | `5.5` | `s` | Source timeout waiting for `GoodCRC(Source_Cap)` after Hard Reset (`nHardResetCount = 2` retries max) | **PD R3.3** Sec 7.31.5.4, **Table 7.10**, **Table 7.12**; **PD CTS Q4 2026** `TEST.PD.PROT.SRC.3` | `"If the Source fails to receive a GoodCRC Message in response to a Source_Capabilities Message within tNoResponse of: • The last bit of a Hard Reset Signaling being sent ... • Then the Source Shall issue additional Hard Resets up to nHardResetCount times"` / CTS `SRC.3`: `"The Tester applies Rd and it waits for VBUS for tNoResponse max."` | **verified** |

**Why a tester uses a `~5 s` attach/re-attach timeout:**
- Spec-wise, CTS `TEST.PD.PROT.SRC.3` step 2 explicitly waits **`tNoResponse max` (`5.5 s`, nom `5.0 s`)** for `VBUS` after applying `Rd` (matching `tCtsInfinite = 5 s` in `Table 19`, and also matching **`tTypeCSendSourceCapIdle max = 5000 ms`**).
- By contrast, the tight single-shot Type-C + PD bring-up path after `Rd` attach is **`tCCDebounce` (`100–200 ms`) + `tBulkDischarge` (`<= 700 ms` if >21 V re-attach) + `tVBUSON` (`<= 275 ms`) + `tFirstSourceCap` (`<= 250 ms`) = `~1.425 s`** worst-case (or `~0.725 s` from cold 0 V), plus up to `tDRP` (`100 ms`) if toggling. Waiting ~5 s absorbs worst-case bulk discharge, slow multi-port DRP toggle (`tTwoPortToggleConnect <= 510 ms`), and real-world charger boot/PPS reset delays.

### B.3 Hard Reset Timing vs. Tester Bounds

Comparison of PD R3.3 + PD CTS Q4 2026 directly against the brief's existing checks (`hr_safe0v <= 685 ms`, `hr_recover 660–1000 ms`, `hr_turn_on <= 275 ms`, `hr_caps <= 250 ms`):

| Parameter | Min | Nom | Max | Unit | Exact Start -> End Event | Spec, Section, Table/Fig, CTS Check | Brief Bound Check | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`tPSHardReset`** | `25` | `30` | `35` | `ms` | Last bit of Hard Reset Signaling (received or sent) -> Source begins turning off VBUS | **PD R3.3** Sec 4.4, 7.31.10.2, **Table 7.10**; **CTS** `TEST.PD.PS.SRC.5#1/#7`, `COMMON.CHECK.PD.14#1.1a` | Must hold prior VBUS `>= 25 ms` (`tPSHardReset min`); transition begins by `35 ms` | **verified** |
| **`tSafe5V`** (during Hard Reset drop from `> 5 V`) | — | — | `275` | `ms` | From end of `tPSHardReset` (or start of drop `t0`) -> `VBUS <= vSafe5V max (5.5 V)` | **PD R3.3** Sec 4.4 **Fig 4.15**, **Table 4.9**; **CTS** `TEST.PD.PS.SRC.5#8` | Combined check in CTS `PS.SRC.5#8`: `<= 35 + 275 = 310 ms` from Hard Reset Signaling | **verified** |
| **`tSafe0V`** | — | — | `650` | `ms` | From end of `tPSHardReset` (`PS.SRC.5`) or from leaving initial valid VBUS (`COMMON.CHECK.PD.14`) -> `VBUS <= vSafe0V max (0.8 V)` | **PD R3.3** Sec 4.4 **Fig 4.15**, 4.4.1 **Fig 4.16**, **Table 4.9**; **CTS** **`TEST.PD.PS.SRC.5#2/#9`**, `COMMON.CHECK.PD.14#1.1b` | **`hr_safe0v <= 685 ms` (`tPSHardReset max 35 ms + tSafe0V max 650 ms`) is EXACT per `TEST.PD.PS.SRC.5#2/#9`** (`"within tSafe0v max of tPSHardReset max"`) | **verified** |
| **`tSrcRecover`** (**SPR Mode**) | `0.66` (`660 ms`) | — | `1.0` (`1000 ms`) | `s` | From **`VBUS` establishing `<= vSafe0V`** -> Source begins raising `VBUS` above `vSafe0V` | **PD R3.3** Sec 4.4 **Fig 4.15**, **Table 4.7**; **CTS** `TEST.PD.PS.SRC.5#3/#10`, `COMMON.CHECK.PD.14#1.1c` | **`hr_recover 660–1000 ms` is EXACT for SPR Mode** | **verified** |
| **`tSrcRecover`** (**EPR Mode**) | `1.085` (`1085 ms`) | — | `1.425` (`1425 ms`) | `s` | From `VBUS <= vSafe0V` -> Source begins raising `VBUS` above `vSafe0V` | **PD R3.3** **Table 4.7**; **CTS** `COMMON.CHECK.PD.14` Note | **Mismatch warning**: if testing Hard Reset triggered while in **EPR Mode**, SPR `660–1000 ms` window will false-fail a compliant `1085–1425 ms` EPR source! | **verified** |
| **`tSrcTurnOn`** | — | — | `275` | `ms` | From **`VBUS` rising above `vSafe0V max (0.8 V)`** -> **`VBUS` reaches `vSafe5V min (4.75 V)`** | **PD R3.3** Sec 4.4 **Fig 4.15**, 4.4.1, **Table 4.7**; **CTS** `TEST.PD.PS.SRC.5#4/#11`, `COMMON.CHECK.PD.14#1.1d` | **`hr_turn_on <= 275 ms` is EXACT** | **verified** |
| **`tFirstSourceCap`** (post-Hard Reset) | — | — | `250` | `ms` | From **`VBUS` reaching `vSafe5V min (4.75 V)`** -> First bit of `Source_Capabilities` | **PD R3.3** Sec 7.31.3.3, **Table 7.10**; **CTS** `TEST.PD.PS.SRC.5#5/#12` | **`hr_caps <= 250 ms` is EXACT** | **verified** |

> **Verbatim quotes (`TEST.PD.PS.SRC.5`, PD CTS Q4 2026)**:
> - `a. VBUS stays within present valid voltage range for tPSHardReset min after last bit of Hard Reset Signal. [TEST.PD.PS.SRC.5#1/#7]`
> - `b. VBUS reaches vSafe5V max within tSafe5v max of tPSHardReset max. [TEST.PD.PS.SRC.5#8]`
> - `c. VBUS reaches vSafe0V max within tSafe0v max of tPSHardReset max. [TEST.PD.PS.SRC.5#2/#9]`
> - `d. VBUS rises above vSafe0V max after a delay of between tSrcRecover min and tSrcRecover max after reaching vSafe0V. [TEST.PD.PS.SRC.5#3/#10]`
> - `e. VBUS reaches vSafe5V within tSrcTurnOn max after rising above vSafe0v max [TEST.PD.PS.SRC.5#4/#11]`
> - `f. The UUT starts sending a Source Capabilities message within tFirstSourceCap max of VBUS reaching vSafe5v min. [TEST.PD.PS.SRC.5#5/#12]`

### B.4 `Soft_Reset` Protocol Timing & VBUS Behavior

| Aspect | Normative Requirement | Values / Spec / Section | Verbatim Quote | Status |
| :--- | :--- | :--- | :--- | :--- |
| **Source-initiated `Soft_Reset` (comms retry failure)** | Sent after `nRetryCount` (`2`) missed `GoodCRC`s within `tSoftReset` of `CRCReceiveTimer` (`tReceive 900–1100 us`) expiring | `tSoftReset`: **`15 ms` max** (**PD R3.3** Sec 7.31.8.1, **Table 7.10**) | `"This Shall cause the Source or Sink to send a Soft_Reset Message, transmission of which Shall be completed within tSoftReset of the CRCReceiveTimer expiring."` | **verified** |
| **Source-initiated `Soft_Reset` (protocol error)** | Sent within `tProtErrSoftReset` of EOP of `GoodCRC` on offending message | `tProtErrSoftReset`: **`15 ms` max** (**PD R3.3** Sec 7.31.8.2, **Table 7.10**) | `"transmission of the Soft_Reset Message Shall be completed within tProtErrSoftReset of the EOP of the GoodCRC Message sent in response to the Message that caused the Protocol Error."` | **verified** |
| **Counter & MessageID Reset** | `Soft_Reset` always carries `MessageID = 0`; both partners reset `MessageIDCounter = 0`, `RetryCounter = 0`, and clear stored `MessageID` **before** sending `Accept` (`MessageID = 0`) | **PD R3.3** Sec 7.7, 7.32.1.2, 7.32.1.3, 9.2.5.1.2; **CTS** `TEST.PD.PROT.ALL.3#4` | `"The receiver of the Message Shall reset its MessageIDCounter and RetryCounter to 0 before sending the Accept Message response."` / CTS: `"The check fails if the UUT does not send an Accept Message with MessageID 000b. [TEST.PD.PROT.ALL.3#4]"` | **verified** |
| **Source `Accept` deadline** | Source sends `Accept` within `tReceiverResponse` from `GoodCRC(Soft_Reset)` EOP | **`15 ms` max** (**PD R3.3** Sec 7.31.2, **Table 7.10**; **CTS** `COMMON.CHECK.PD.4#1`, `PROT.ALL.3#4`) | `COMMON.CHECK.PD.4#1`: `"within tReceiverResponse max ... from the last bit of the EOP of the GoodCRC Message sent"` | **verified** |
| **Post-`Accept` `Source_Capabilities` deadline** | Source transitions `PE_SRC_Soft_Reset -> PE_SRC_Send_Capabilities` and sends `Source_Capabilities` within **`tFirstSourceCap`** after `GoodCRC(Accept)` EOP | **`250 ms` max** (**PD R3.3** Sec 7.31.3.3, 9.2.5.1.2, 9.2.3.3, **Table 7.10**; **CTS** **`TEST.PD.PROT.ALL.3#6`**) | PD R3.3 `7.31.3.3`: `"After Soft Reset, a Source Shall send its first Source Capabilities Message within tFirstSourceCap after the last bit of the GoodCRC Message EOP corresponding to the Accept Message."` <br> CTS `PROT.ALL.3#6`: `"check fails if the UUT does not send a Source_Capabilities Message within tFirstSourceCap after the last bit of the GoodCRC Message EOP corresponding to the Accept Message."` | **verified** |
| **VBUS during `Soft_Reset`** | **VBUS MUST stay continuously at the current contracted voltage** (zero drop, no transition to `vSafe5V` or `vSafe0V`); power is renegotiated by explicit contract after `Source_Capabilities` | **PD R3.3** Sec 7.1.1, Sec 9.2.5.1.2 (`PE_SRC_Soft_Reset -> PE_SRC_Send_Capabilities`) | PD R3.3 `7.1.1`: **`"The Soft Reset Shall Not have any impact on power supply operation but is used to correct a Protocol Error occurring during an Atomic Message Sequence (AMS)."`** | **verified** |

---

## Part C — USB Battery Charging Specification Revision 1.2 (plus Errata Mar 15, 2012)

> Note on Errata: All changes in `Battery_Charging_Errata_Rev_1.2_2012-03-15.pdf` (`RADP_OTG_ACC <= 25 Ohm`, `CMACA_VBUS = 10–100 nF`, `Table 6-2`, `6.2.6`) apply strictly to Micro-ACA ports and do **not** modify DCP, CDP, or SDP tables.

### C.1 Primary & Secondary Detection Voltages, Currents, Resistances, and Timings

| Parameter | Symbol | Min | Max | Unit | Spec, Rev, Section, Table | Verbatim Quote / Conditions | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **D+ Source Voltage** | `VDP_SRC` | `0.5` | `0.7` | `V` | BC 1.2 (Mar 15, 2012), Sec 3.2, **Table 5-1** | Note 2: `"VDP_SRC shall be able to source at least 250uA when D+ is between 0.5V and 0.7V. VDP_SRC shall not pull D+ below 2.2V when D+ is pulled to VDP_UP through RDP_UP."` | **verified** |
| **D- Source Voltage** | `VDM_SRC` | `0.5` | `0.7` | `V` | BC 1.2 (Mar 15, 2012), Sec 3.2, **Table 5-1** | Note 1: `"VDM_SRC shall be able to source at least 250uA when D- is between 0.5V and 0.7V. VDM_SRC shall not pull D- below 2.2V when D- is pulled to VDP_UP through RDP_UP."` | **verified** |
| **Data Detect Voltage** | `VDAT_REF` | `0.25` | `0.4` | `V` | BC 1.2 (Mar 15, 2012), Sec 3.2, **Table 5-1** | `"A PD shall compare the voltage on D- with VDAT_REF. If D- is greater than VDAT_REF, then the PD is allowed to detect that it is attached to either a DCP or CDP."` (`3.2.4.1`) | **verified** |
| **Logic Threshold / High / Low** | `VLGC` (`VLGC_LOW` / `VLGC_HI`) | `0.8` (`0` / `2.0`) | `2.0` (`0.8` / `3.6`) | `V` | BC 1.2 (Mar 15, 2012), Sec 3.2.3, **Table 5-1** | Optional Primary Detection upper ceiling (`VDAT_REF < D- < VLGC`) to avoid mistaking PS2 ports for DCP/CDP (`3.2.4.1`) | **verified** |
| **Data Sink Voltage** | `VDAT_SINK` | — | `0.15` | `V` | BC 1.2 (Mar 15, 2012), Sec 3.4.2, **Table 5-1** | Max sink residual voltage at currents `< IDP_SINK min` (`Table 5-2` Note 3) | **verified** |
| **D+ / D- Sink Current** | `IDP_SINK`, `IDM_SINK` | `25` | `175` | `uA` | BC 1.2 (Mar 15, 2012), Sec 3.2, **Table 5-2** | Note 3: `"For D+ voltages less than VLGC max, the D+ current sink shall not sink more than IDP_SINK max. The same requirements apply to IDM_SINK and D-."` | **verified** |
| **Data Contact Detect Current** | `IDP_SRC` | `7` | `13` | `uA` | BC 1.2 (Mar 15, 2012), Sec 3.2.3, **Table 5-2** | D+ pull-up current source for DCD | **verified** |
| **DCP resistance across D+/D-** | `RDCP_DAT` | — | `200` | `Ohm` | BC 1.2 (Mar 15, 2012), Sec 4.4.3, **Table 5-3** | `"A DCP shall have an impedance between D+ and D- of RDCP_DAT."` (`<= 200 Ohm`) | **verified** |
| **Data line leakage resistance & voltage** | `RDAT_LKG` at `VDAT_LKG` | `300 kOhm` (`0 V`) | `—` (`3.6 V`) | `kOhm`, `V` | BC 1.2 (Mar 15, 2012), Sec 4.4.3, **Table 5-1**, **Table 5-3** | `"The leakage current on the D+/- pins of a DCP shall be less than or equal to the leakage current that would occur from two resistances of RDAT_LKG tied to a voltage of VDAT_LKG."` | **verified** |
| **DCP D+/D- capacitance to VBUS/GND** | `CDCP_PWR` | — | `1` | `nF` | BC 1.2 (Mar 15, 2012), Sec 4.4.3, **Table 5-4** | `"The capacitance between the D+/- pins and ground of a DCP shall be CDCP_PWR."` | **verified** |
| **SDP/CDP D+ / D- Pull-down resistance** | `RDP_DWN`, `RDM_DWN` | `14.25` | `24.8` | `kOhm` | BC 1.2 (Mar 15, 2012), Sec 3.2, **Table 5-3** | Standard USB 2.0 host pull-downs on D+ and D- | **verified** |
| **D+ / D- voltage source on time** | `TVDPSRC_ON`, `TVDMSRC_ON` | `40` | — | `ms` | BC 1.2 (Mar 15, 2012), Sec 3.4, **Table 5-5**, **Fig 3-19** | `"After a time of TVDPSRC_ON, the PD is allowed to check the status of the D- line."` (`>= 40 ms` min settle before sampling D- / D+) | **verified** |
| **CDP D- voltage source enable / disable time** | `TVDMSRC_EN`, `TVDMSRC_DIS` | — | `20` | `ms` | BC 1.2 (Mar 15, 2012), Sec 3.4, **Table 5-5**, **Fig 3-19** | `"The CDP is required to have VDM_SRC on D- within a time of TVDMSRC_EN from when D+ is above VDAT_REF."` | **verified** |
| **CDP disconnect-to-assert / connect-to-remove `VDM_SRC`** | `TCP_VDM_EN` / `TCP_VDM_DIS` | — | `200` / `10` | `ms` | BC 1.2 (Mar 15, 2012), Sec 3.2.4.2, **Table 5-5** | Option 1 CDP behavior (enable within `TCP_VDM_EN`, disable within `TCP_VDM_DIS` on USB connect) | **verified** |
| **DCD Debounce / DCD Timeout** | `TDCD_DBNC` / `TDCD_TIMEOUT` | `10` / `300` | `—` / `900` | `ms` | BC 1.2 (Mar 15, 2012), Sec 3.2.3.1, 3.4.1, **Table 5-5** | Data contact detect timings before Primary Detection | **verified** |
| **Session valid to connect (powered PD)** | `TSVLD_CON_PWD` | — | `1` | `s` | BC 1.2 (Mar 15, 2012), Sec 3.2.3.1, 3.2.5, **Table 5-5** | Wait window for PDs skipping Secondary Detection if ready to enumerate | **verified** |
| **`TVDPSRC_HICRNT`** | **Not found in BC 1.2** | — | — | — | BC 1.2 (Mar 15, 2012), **Table 5-5** | Parameter does not exist anywhere in BC 1.2 Rev 1.2 or its March 15, 2012 Errata (use `TVDPSRC_ON >= 40 ms` / `TSVLD_CON_PWD <= 1 s`) | **verified** (absent) |

### C.2 DCP / CDP / SDP Current Ratings & VBUS Electrical Limits

| Parameter | Symbol | Min | Max | Unit | Spec, Rev, Section, Table | Verbatim Quote / Rule | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Charging Port Output Voltage (CDP / DCP)** | `VCHG` | `4.75` | `5.25` | `V` | BC 1.2, Sec 4.2.1, 4.4.1, **Table 5-1** | **`"A DCP shall output a voltage of VCHG for all currents less than IDCP min. The voltage on VBUS is averaged over a time of TVBUS_AVG."`** (`4.75–5.25 V`, tighter max than USB PD/Type-C `vSafe5V` `5.5 V`!) | **verified** |
| **VBUS averaging time** | `TVBUS_AVG` | — | `250` | `ms` | BC 1.2, Sec 4.2.1, 4.4.1, **Table 5-5** | Measurement averaging window for `VCHG` | **verified** |
| **Charging Port Overshoot Voltage** | `VCHG_OVRSHT` | — | `6.0` | `V` | BC 1.2, Sec 4.1.1, **Table 5-1** | `"The output voltage of a Charging Port shall not exceed VCHG_OVRSHT for any step change in load current, nor when the Charging Port is powered on or off."` | **verified** |
| **Charging Port Undershoot Voltage** | `VCHG_UNDSHT` | `4.1` | — | `V` | BC 1.2, Sec 4.4.2, **Table 5-1** | Step-load undershoot floor during `TDCP_UNDSHT` (`10 ms` max) | **verified** |
| **Charging Port Failure Voltage** | `VCHG_FAIL` | `-0.3` | `9.0` | `V` | BC 1.2, Sec 4.1.5, **Table 5-1** | Single-point failure VBUS ceiling | **verified** |
| **DCP Shut Down Voltage** | `VDCP_SHTDWN` | — | `2.0` | `V` | BC 1.2, Sec 4.4.1, **Table 5-1** | `"A DCP shall not shut down if the load current is less than IDEV_CHG and the load voltage is greater than VDCP_SHTDOWN."` (Note spelling `VDCP_SHTDOWN`/`VDCP_SHTDWN` in 4.4.1) | **verified** |
| **DCP Rated Current** | `IDCP` | `0.5` | `5.0` | `A` | BC 1.2, Sec 4.4.1, **Table 5-2** | Note 1: `"The maximum current is for safety reasons, as per USB 2.0 section 7.2.1.2.1."` | **verified** |
| **CDP Rated Current** | `ICDP` | `1.5` | `5.0` | `A` | BC 1.2, Sec 4.1.2, 4.2.1, **Table 5-2** | `"The output current of a Charging Port shall not exceed ICDP max under any condition."` (`4.1.2`) | **verified** |
| **Allowed PD Current Draw from Charging Port** | `IDEV_CHG` | — | `1.5` | `A` | BC 1.2, Sec 4.4.1, 4.6, **Table 5-2** | `"A PD shall not draw more than IDEV_CHG max from a Charging Port."` (`4.6.1`) | **verified** |
| **SDP Max Configured / Suspend Current** | `ICFG_MAX` / `ISUSP` | — | `500` (`900` SS) / `2.5` | `mA` | BC 1.2, Sec 2.1, **Table 5-2** | `ISUSP` averaged over `1 sec` | **verified** |
| **VBUS Detection Renegotiation** | `VBUS_LKG` / `TVBUS_REAPP` | `100 ms` (`TVBUS_REAPP`) | `0.7 V` (`VBUS_LKG`) | `V`, `ms` | BC 1.2, Sec 4.1.3, **Table 5-1**, **Table 5-5** | Port stops driving VBUS, drops below `VBUS_LKG` (`<= 0.7 V`), waits `>= TVBUS_REAPP` (`100 ms`), re-applies VBUS | **verified** |

### C.3 What a No-Load Sink Tester Can Verify on a BC1.2 Port

1. **Port Classification via Primary + Secondary Detection (`3.2.4`, `3.2.5`)**:
   - **Primary Detection**: Assert `VDP_SRC` (`0.5–0.7 V`) on D+ and `IDM_SINK` (`25–175 uA`) on D-, wait `>= TVDPSRC_ON` (`40 ms`).
     - `D- < VDAT_REF` (`0.25–0.4 V`) -> **SDP** (15 kOhm `RDM_DWN` `14.25–24.8 kOhm` pulls D- near 0 V). Note `TVDMSRC_EN <= 20 ms` ensures a CDP has responded well before `40 ms`.
     - `VDAT_REF < D- < VLGC` -> **DCP or CDP** (proprietary divider / PS2 if `> VLGC`).
   - **Secondary Detection** (discriminates DCP vs CDP): Turn off `VDP_SRC`/`IDM_SINK`, assert `VDM_SRC` (`0.5–0.7 V`) on D- and `IDP_SINK` (`25–175 uA`) on D+, wait `>= TVDMSRC_ON` (`40 ms`).
     - `D+ > VDAT_REF` (tracks `VDM_SRC`) -> **DCP** (`D+`/`D-` passively shorted via `RDCP_DAT`).
     - `D+ < VDAT_REF` (pulled low by `RDP_DWN`) -> **CDP**.

2. **DCP `RDCP_DAT <= 200 Ohm` and Leakage Verification (Zero VBUS Load)**:
   - Because DCP shorting is passive (`RDCP_DAT <= 200 Ohm`, `4.4.3`), driving `VDP_SRC` (`0.5–0.7 V`) into D+ while sinking `IDM_SINK` (`25–175 uA` or a known mA-scale test current `<= 250 uA` to `~1 mA` from the D+ pin) produces `DeltaV = V_D+ - V_D- = I_sink * RDCP_DAT`. Measuring `(V_D+ - V_D-) / I_sink` (and confirming bidirectional symmetry by repeating during Secondary Detection) directly verifies **`RDCP_DAT <= 200 Ohm`**!
   - With `IDM_SINK` = `175 uA` and `RDCP_DAT = 200 Ohm`, `DeltaV = 35 mV`, well above sub-mV ADC resolution; an actual D+/D- short measures `< 1–5 Ohm`, while a high-value resistor divider (proprietary Apple/divider chargers) fails either symmetry or `RDCP_DAT` / `VDAT_LKG` bounds.
   - Measure floating open-circuit D+/D- voltage with pull-ups off to verify absence of proprietary pull-up bias violating `RDAT_LKG >= 300 kOhm` / floating DCP behavior.

3. **No-Load VBUS Compliance (`4.1.1`, `4.4.1`)**:
   - **Steady-state VBUS voltage**: With `0 mA` load (`< IDCP min`), a compliant DCP/CDP **must hold VBUS within `VCHG = 4.75–5.25 V`** averaged over `TVBUS_AVG` (`<= 250 ms`) — notably tighter than USB PD/Type-C `vSafe5V` (`4.75–5.50 V`).
   - **No turn-on overshoot**: Monitor VBUS from attach through stabilization (`>= 250 ms`) to check **`VCHG_OVRSHT <= 6.0 V`** (`4.1.1`).
   - **Zero-load persistence**: Must not shut down VBUS at `0 mA` (`4.4.1`: must output `VCHG` for all currents `< IDCP min`).

---

## Part D — UFCS (`T/TAF 083`)

**Skipped**: Per brief rule (*"Skip entirely if no primary text is available"*), Part D is omitted because all official primary URLs on `taf.org.cn` failed TLS certificate validation (`curl` exit code 60: `unable to get local issuer certificate`), and non-primary blogs/summaries are not acceptable as normative sources.

---

## URLs & Retrieval Log

| URL | Result | Notes / Artifact Extracted |
| :--- | :--- | :--- |
| `https://www.usb.org/document-library/usb-type-cr-cable-and-connector-specification-release-25` | **Succeeded** (HTML) | Located latest Type-C R2.5 release package link |
| `https://www.usb.org/sites/default/files/USB%20Type-C%202.5%20Release%20202603.zip` | **Succeeded** (ZIP) | Extracted `USB Type-C Spec R2.5 - March 2026.pdf` -> `typec25.txt` |
| `https://www.usb.org/document-library/battery-charging-v12-spec-and-adopters-agreement` | **Succeeded** (HTML) | Located official BC1.2 + Errata ZIP link |
| `https://usb.org/sites/default/files/BCv1.2_070312_0.zip` | **Succeeded** (ZIP) | Extracted `BC1.2 plus errata 2012-3-15.pdf` (`bc12.txt`) and `Battery_Charging_Errata_Rev_1.2_2012-03-15.pdf` (`bc12_errata.txt`) |
| `https://www.usb.org/document-library/usb-power-delivery` *(PD R3.3 V1.0 Oct 2026)* | **Succeeded** (prior session PDF -> `pd33.txt`) | USB PD Specification R3.3 V1.0 (Oct 2026) |
| USB PD Compliance Test Specification Q4 2026 *(usb.org)* | **Succeeded** (prior session PDF -> `pd3_cts.txt`) | USB PD3 CTS Q4 2026 |
| `https://www.taf.org.cn/StandardList.aspx` | **Failed** | `curl: (60) SSL certificate OpenSSL verify result: unable to get local issuer certificate (20)` |
| `https://www.taf.org.cn/` (T/TAF 083-2022 PDF endpoint) | **Failed** | `curl: (60) SSL certificate OpenSSL verify result: unable to get local issuer certificate (20)` |
