# Feature roadmap

Status of every feature and what is verified on hardware, checked against the code and the hardware notes on
2026-11-08. Protocol facts: `FASTCHARGE_PROTOCOLS.md`. Ideas: `agents/FEATURE_PROTOTYPES.md`. How to test each row on
the device: `HW_TEST_PROCEDURES.md`. Charger behaviour evidence: `CHARGER_QUIRKS.md`.

Two separate questions per row:

- **Status**: `done` = merged and built by `make gate`; `partial` = some of it; `planned`; `blocked: reason`.
- **Hardware**: `verified` = seen working on a real K2 with a real partner device (log lines or a screenshot exist,
  see `build/smoke/<hash>/` and the commit messages); `not verified` = code and host tests exist but nobody has
  watched it work on the device; `n/a` = pure host or build feature. A host test passing is not hardware
  verification.

Risk marks: **HV** = can raise or hold a voltage above 5 V; **FLASH** = writes flash outside the application region;
**PASSIVE** = cannot affect the link.

Everything is C++23; there are no C files left in `src/` (TinyUSB and lwIP under `third_party/` are C).

## Verified on hardware

- PD trigger fixed (request a PDO, PPS voltage) and PPS on a lone charger; PPS stepping up and down in small increments.
- Identify on a Pixel (identity, SVIDs, modes, sink caps).
- The charger test: PD fixed, PPS, AVS and EPR steps, and the legacy units, with report cards (CHARGER_TEST_FLOW,
  CHARGER_QUIRKS).
- QC 2.0 / 3.0 (continuous and Class B), Huawei FCP / SCP, Samsung AFC and UFCS on real chargers (per-charger passes
  in CHARGER_QUIRKS; UFCS on a Baseus FH22, 2026-11-07).
- Spoof of a saved cable on the top plug: a Baseus 140 W offered 5 A and EPR 28 V with no e-marked cable
  (2026-11-06).
- USB network link and the dashboard at http://192.168.7.1/ (K2_NET=1 builds only).
- E-marker read and save (library, `CABLES/*.JSN`).
- Black Box capture and persistence across power-off.
- Inline-load loop fix (PD detect while a load is inline).

## Not verified on hardware

- A direct large PPS step-down of a running contract: 21 V to 13 V aborts at the +10 % overvoltage guard
  (`HW_TEST_PROCEDURES.md`, T02).
- Pixel replug behaviour after Identify.
- Self-powered boot (PLUS held): the K2 still died on releasing the key with a charger on the bottom port. The
  current code routes both CC switches in every boot mode, gates every Rd carry-over on a pin mark that only a pinned
  boot writes (a HID-powered K2 stays passive on every reset) and resumes a charger run after a power loss; built and
  host-tested only. Procedures T23 and, as the regression check to run first, T24 in `HW_TEST_PROCEDURES.md`.
- The passive cable-resistance estimate against a reference cable, and Kelvin (8-wire) cable resistance, which needs
  a sense fixture on CC1, CC2 and D-.
- WebHID dashboard in Chrome (Linux hidraw path tested with the Python tools only).
- OPPO VOOC / SVOOC: no handshake seen yet (CHARGER_QUIRKS).
- The reboot-resume of a charger run: the self-powered case is unverified; a host-powered K2 waited forever for
  stable 5 V on 2026-11-07.

## Core platform

| Feature | Status | Hardware | Module | Risk |
|---|---|---|---|---|
| 240 MHz clock tree, HICK fallback | done | verified | `k2::crm` | |
| Layout invariants, flash map | done (gate) | verified | `startup.cpp`, `link.ld`, `flash_map.hpp` | |
| Pin map, SWJ disabled | done | verified | `k2::pins` | |
| ST7789V 240x240 LCD, screenshots | done | verified | `k2::lcd`, `gfx`, `capture` | |
| 4 buttons, long/hold events, hold-BACK reboot | done | verified | `buttons`, `ui_events` | |
| INA226/228/237 power monitor | done | verified (INA226) | `k2::power_monitor` | PASSIVE |
| 7-channel ADC1 + DMA1, temperature | done | verified | `k2::adc` | PASSIVE |
| Ripple capture on PA1 | done | not verified | `k2::adc`, `capture` | PASSIVE |
| Fault handler with `[FAULT]` log | done | not verified | `k2::fault` | |
| Reset-cause log, PD session record in BKP | done | verified | `session`, `session_store` | |
| Self-powered boot (PLUS / OK held) | partial: `bootmode`, `SELF-PWR` chip, both CC switches closed, automatic reboot-resume in every power mode with a 3-reboot budget per unit (`/jr`), bus-powered detection (`power_source`, host-tested only) | not verified (T23) | `bootmode` | |
| Boot-info (what a key-mode boot decided, readable after the fact): remote `bootinfo` (0x35), LCD QR screen, SWITCHES0 read back at ~50 ms / ~600 ms / ~3 s | done: K2B1 text (firmware identity first, same strings as PING), first-seconds power samples, a `qrshare` BootInfo QR code (QR_SHARE.md) plus three text lines once PLUS / OK has been held 3 s from the plug-in (kept 30 s after release; a shorter press goes straight to the UI), Source_Capabilities log block rate-limited to one per 10 s per source | not verified (built and host-tested only) | `bootinfo`, `bootscreen`, `qr`, `qrshare`, `qrview` | PASSIVE |
| QR share (long OK on a saved cable, the Connection Source tab, a report card, the charger test Results): the record as one to four QR codes with a `K2:<tag><schema>:<base45>` text, decoded by `tools/k2_qr.py` | done: frame, record builders, share view in all rotations, host decoder, golden vectors (QR_SHARE.md) | not verified (built and host-tested only; phone scan and encode time at version 10 to 12 not measured on the device) | `qrshare`, `qrshare_records`, `qrview`, `ui_share` | PASSIVE |
| Backlight strobe alarm on PA8 | blocked: backlight is plain GPIO PB7, not PA8 | n/a | | |

## USB, remote, flash

| Feature | Status | Hardware | Notes |
|---|---|---|---|
| HID log + remote channel | done | verified | off by default; Settings > USB Link; left port needs a C-to-A cable |
| USB link modes (HID, HID+Drive, HID+Network) | done | verified | runtime, kept in /usb.cfg on the littlefs volume, applied on replug; PMA budget in ARCHITECTURE |
| `K2 DRIVE` mass storage (status, log, saved cables) | done | verified | read-only FAT synthesis |
| CDC-NCM network, DHCP, HTTP dashboard | done, off by default | verified | lwIP + TinyUSB; `192.168.7.1`; compiled only with `K2_NET=1` (default build frees ~8 KB RAM and ~70 KB flash); the gate builds the variant |
| Hands-free DFU flasher (`--auto-dfu`), `.k2` packer | done | verified | MINUS at plug-in is the fallback |
| Remote protocol and `tools/k2_remote.py` | done | verified | |
| WebHID dashboard (`tools/web`) | done | not verified in Chrome | |
| `make smoke` hardware test | done | verified | writes `build/smoke/<hash>/` |

## Charger test

| Feature | Status | Hardware | Module |
|---|---|---|---|
| Test units: legacy first in one PD-silent attach, then PD, then the low programmable rows (CHARGER_TEST_FLOW) | done | verified | `chargertest`, `ui_charger_test` |
| Report cards, Results view, QR readout (card format versions in QR_SHARE.md) | done | verified (cards); QR phone scan not verified | `chargertest`, `qrshare_records`, `ui_share` |
| Reboot-resume journal (`/jr`) | done | partial: see Not verified | `session_store` |
| The 6 A hunt rows report "not unlocked" instead of failing when the source never offered 6 A | done | not verified on the CUKTECH 120 W | `chargertest` |

## USB-PD

| Feature | Status | Hardware | Module | Risk |
|---|---|---|---|---|
| FUSB302 init, CC orientation, FIFO receive | done | verified | `k2::fusb302`, `pd_phy` | |
| Passive sniffing, Source_Cap decode (Fixed, PPS, AVS, EPR) | done | verified | `k2::pd`, `pd_engine`, `pd_passive` | PASSIVE |
| Extended messages, VDMs, SVID table | done | verified | `pd_ext`, `pd_vdm` | PASSIVE |
| Attach/detach, inline-load detection and guard | done | verified | `pd_link`, `link`, `link_guard` | |
| PD trigger (Request, PPS/AVS voltage, voltage-cap safety) | done | verified (lone charger) | `pd_sink` | HV |
| PPS step-down of a running contract | done | partial: small steps verified, a direct 21 V to 13 V step aborts at the +10 % guard | `pd_sink` | HV |
| Identify (identity, SVIDs, modes, sink caps) | done (an action of the Connection page) | verified (Pixel) | `pd_identify` | |
| Connection page: Source, Cable and Sink tabs, Actions pop-up (Refresh, Share, Read cable, Save cable, Spoof as, Saved cables, Identify) | done | hardware pass of the page still open | `ui_connection_page`, `pdtabs`, `usblink` | |
| Pixel replug after Identify | open | not verified | `pd_link`, `pd_identify` | |
| Apple Discover Identity, 43-entry PID table | done | not verified | `pd_vdm` | PASSIVE |
| Rescan and CC choice | done | verified | `pd_rescan`, `pd_ccscan` | |
| Link summary (USB4/TBT/DP from sniffed PD, the `USB` and `Alt` rows) | done | not verified | `usblink` | PASSIVE |
| "AI..." charger recognition widget | done | not verified | `fastcharge_classify`, `ai_recog` | PASSIVE |
| "PD Compat." toggle | planned | n/a | | |
| SPR AVS 9-20 V requests | partial (request fields exist, no UI) | not verified | `pd_sink` | HV |
| Apple battery level and model, charger serial | planned | n/a | `pd_vdm` | PASSIVE |
| Xiaomi Mi Turbo auth replies | partial: observation only | n/a | `pd_vdm` | HV |
| UFCS 7-DO handshake and frames | handshake, Ping, Get_Output_Capabilities and Request/Accept hardware-verified on a Baseus FH22 charger (2026-11-07); Power_Ready wait, keep-alive and reset ladder rewritten to the standard, host-tested, not yet run on a source | n/a | `ufcs` | HV |

## Cables

| Feature | Status | Hardware | Module | Risk |
|---|---|---|---|---|
| E-marker read (VCONN mode, sniff mode) | done | verified | `pd_emarker_plan`, `fusb302` | VCONN 5 V on CC |
| Apple cable 6-chunk reader | done | not verified | `pd_engine` | |
| Record codec (`emrec`), JSON (`emjson`), names (`emname`) | done | verified | `emrec`, `emjson`, `emname` | |
| Cable library, 140 entries (/emk files) | done | verified | `emlib`, `flash_map.hpp` | **FLASH** |
| Save, list, rename, delete (device, remote, drive) | done | verified | `emlib`, `ui_emarker` | **FLASH** |
| Spoof (answer SOP' with a saved cable) | done; refused while a sink is inline | verified on the top plug (2026-11-06) | `emk_spoof` | HV (a faked 5 A / 50 V cable) |
| Spoof quirk table, K2E2 clones, `emarker learn` (power a real cable, replay DI / SVIDs / Modes / seeded vendor commands, record the replies) | done | not verified (host tests only) | `emk_quirk`, `emk_learn`, `emk_learn_hw` | PASSIVE (cable in the bottom port, no charger) |
| Passive cable resistance (idle vs load) | done | not verified against a reference | `cable_r`, `ui_cabler` | PASSIVE |
| Kelvin 8-wire cable resistance (`cable-r kelvin`, Mode on the Cable R page) | done | not verified (needs a sense fixture and at least 0.45 A of load) | `cable_r`, `ui_cabler` | |

## Black Box

| Feature | Status | Hardware | Module |
|---|---|---|---|
| RAM ring of raw PD/FUSB302 records, remote command 0x70, `tools/pd_trace.py` | done | verified | `pdtrace`, `pdtrace_link` |
| Trigger-frozen captures (contract change, hard reset, VBUS dip, detach) | done | verified | `pdtrace` |
| Captures as /bb files (32, oldest evicted), LZ4, written when the link is quiet | done | verified | `bbstore`, `bbpersist`, `lz` |
| Device viewer (Tools > Black Box) | done | verified | `ui_blackbox`, `pdtrace_view` |
| WebHID timeline viewer, MSC export of captures | planned | n/a | |

## Transition capture

| Feature | Status | Hardware | Module |
|---|---|---|---|
| ~1 kHz VBUS + CC1/CC2 ring (1.92 s) with PD markers, INA226 fast mode | done | not verified | `transcap`, `power_monitor`, `adc` |
| Charger test arms before non-5 V steps, keeps the capture of a failed step (4 files in /tcap) | done | not verified | `ui_charger_test`, `transcap_store` |
| Remote 0x71, `k2_remote.py transcap`, `pd_trace.py transition` (CSV, PNG) | done | not verified | `remote`, `tools/transcap.py` |
| VBUS range trace on the failed step's detail screen | done | not verified | `ui_charger_test`, `ui_widgets` |

## Fast-charge protocols

Per-charger results are in `CHARGER_QUIRKS.md`.

| Feature | Status | Hardware | Module | Risk |
|---|---|---|---|---|
| D+/D- drive and read primitives | done | verified | `dpdm` | HV |
| Passive classifier | done | verified | `fastcharge_classify` | PASSIVE |
| QC 2.0 / 3.0 / 3+ trigger | done | verified (QC 2.0, QC 3.0 continuous and Class B; QC 3+ 5 V only) | `qc`, `fctrig` | HV |
| Samsung AFC | done | verified (9 V) | `afc` | HV |
| Huawei FCP / SCP | done | verified | `accp`, `uart2` | HV |
| OPPO VOOC / Warp | done; stays "needs load": the PA8 burst was reproduced (`k2::load`) but draws no current, so the adapter clock is still missing. Follow-ups: a rising-edge clock input on PB0/EXTI0 (PA7 and PB0 both floating), and a passive phone sniff (`lab sniff arm`, `tools/vooc_edges.py`) | not verified | `vooc`, `load`, `dpsniff` | HV |
| One-click auto detector (FCP/SCP/AFC/QC2/QC3/VOOC/QC3+) | done; guard rechecked through reset phases | not verified | `fc_scan`, `fc_engine` | HV (up to 9 V) |
| Trigger safety rules | done | n/a (host tests) | `fc_safety` | |

## Ideas not started (FEATURE_PROTOTYPES)

All need the PD TX path (done) and are **HV**: P1 smart charge limiter, P2 programmable eFuse, P3 CC/CV bench
supply via PPS/AVS, P4 charger profiler, P5 e-marker booster. Cut-off actuators (CC open, hard reset, 5 V drop)
are the shared first step. P6 (cable resistance) is the passive estimate above.

## Next up, in order

A. **CUKTECH 10 / 120 W: Xiaomi 6 A mode.** It advertises PDP 120 W but offers 20 V 3 A per port, and sends
   Xiaomi (SVID 0x2717) VDMs at every attach. Goals:
   - reach and verify **20 V 6 A** (120 W) and **11 V 6 A** (PPS, 66 W);
   - work out what unlocks them: Mi Turbo VDM auth ("Xiaomi Mi Turbo auth replies" above, observation only so far)
     and/or a 6 A cable identity;
   - make the charger test's 6 A hunt rows report it. Done in code: a row sends its 6 A only against a list that
     advertises it, and a source that never listed SVID 0x2717 drops the rows quietly. A hunt the source refused or
     never answered reads "not unlocked" (a skip) unless the chain finished and a read 5 A cable was in place, then
     it stays a FAIL (Reason::NotUnlocked = 38, detail 155 no OEM chain / 156 chain unfinished / 157 cable not proven
     above 3 A / 158 spoofed 5 A identity not accepted by the vendor / 159 answered by a source Hard Reset). Not yet
     verified on the CUKTECH (6 A cable and 3 A cable).
B. **USB identity detection:**
   - Discover Identity (SOP) product types and VDOs;
   - USB data capability / version (UFP/DFP VDO highest speed, USB4/TBT);
   - Alt modes (Discover SVIDs/Modes: DisplayPort, Thunderbolt, vendor);
   - present it as one partner-identity view.
   - **Presentation built:** the Connection page with Source / Cable / Sink tabs, an Actions pop-up (Refresh,
     Share, Read cable, Save cable, Spoof as..., Saved cables, Identify), vendor protocol names from known SVIDs,
     and port-aware empty states from the passive `raguard::classify` topology (top plug / via cable; which side
     the ADC taps is unproven). Still open: reading Discover Identity / SVIDs for more partners, USB4/TBT detail,
     and a hardware pass of the page.
C. Small follow-ups from 2026-11-05:
   - card cable block should reflect a passing EPR step;
   - the CC scan treats an Ra-level line (0.2-0.8 V at 3 A Rp) as active_cc and puts Rd on the cable's line;
   - SPI clock DIV4/DIV2 plus a faster page tick (front A, device check).

Older list:

1. Self-powered boot: run T24 (HID-powered boot stays passive) and then T23 (`HW_TEST_PROCEDURES.md`) to check the CC routing fix and the automatic resume (parts G and H cover the bus-powered A-port run and the reboot budget).
2. Hardware-verify the Not verified list above, starting with the direct PPS step-down and a cable-resistance check against a reference cable.
3. Apple battery/serial readers and the "PD Compat." toggle (flash-backed settings need a storage decision).
4. Shorten `make gate`: the test suite is the long part (a cold run takes about 10 minutes in CI; incremental local runs are much faster).

## Parked: unlock the extended 224 KB SRAM

The AT32F403A has 224 KB SRAM; the default EOPB0 option byte maps 96 KB (the rest serves as the zero-wait flash
area). The linker and gate use 96 KB (`RAM_TOP` 0x20018000). After the RAM audit (Net/Drive overlay) the stack has
~12.8 KB free, so the extra 128 KB is not needed yet.

Before doing it: confirm on a spare device the EOPB0 encoding and the zero-wait size per setting; confirm the
bootloader and DFU still work with the extended map (its RAM use and option-byte handling); write a tool that
changes only EOPB0 and preserves read protection (FAP) and the user bytes, with a recovery plan. One write, with the
owner present. Use cases that would justify it: a longer Black Box ring, bigger screenshot strips.
