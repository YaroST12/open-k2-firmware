# Connection matrix: every way to hook up the K2, and what it must do

This is the ground-truth table for what the K2 does when something is plugged into it. Every claim carries
exactly one status. A claim with no witness is `unverified` and sits in the test plan (section 6) until someone
runs it. Where an older document disagrees with this one, this one wins and the disagreement is an entry in
section 5.

Written 2026-11-09 against build `v1.0-101-gb00178c` (the `public` branch). Evidence paths under `runs/` are
local capture directories that are not published; each verified row quotes its witness lines so the claim can be
checked without them. Where a captured file does not carry the firmware hash, the row says so.

Related: [`K2_PORTS_SPEC.md`](K2_PORTS_SPEC.md) (port capabilities), [`SOURCE_SINK_DETECTION.md`](SOURCE_SINK_DETECTION.md)
(how the firmware classifies a link), [`CHARGER_QUIRKS.md`](CHARGER_QUIRKS.md) (behaviour patterns),
[`HW_TEST_PROCEDURES.md`](HW_TEST_PROCEDURES.md) (step-by-step procedures), [`ROADMAP.md`](ROADMAP.md) (Current bugs).

## 1. Axes

The K2 has one FUSB302B (one BMC codec, one RX FIFO) behind two analog switches, so it can look at one CC line
at a time. It has no firmware Rp. Its only Rd is the FUSB302 pull-down (`SWITCHES0` PDWN1 = CC1, PDWN2 = CC2).
Its only Ra is a mode of the PC13 pin (Ra on CC1, Rd on CC2). CC1 is read on ADC PA4, CC2 on PA5; both saturate
at 3.3 V.

Ports. The **top plug** is a captive male Type-C connector and the **bottom port** is a Type-C receptacle. They
share one CC network and one VBUS rail, so a source or a sink behaves the same on either; the matrix collapses a
row to one port unless the evidence differs, and says so. The **left port** is the USB-C receptacle that carries
HID (a C-to-A cable to a host); it is not a CC port and is only a power and log input.

### 1.1 K2 power

| Value | Definition | How the K2 tells |
|---|---|---|
| `HID` | HID-powered: a USB host on the left port supplies aux 5 V; boot is plain (no key) and passive | `aux_mv` about 5.0 V, a configured host, `m=plain`, `SWITCHES0` read back 0x00 |
| `BUS` | VBUS-powered, plain boot, no key, no host | `power_source` latch: no host after 2 s and VBUS >= 4 V (`include/k2/core/power_source.hpp`); the latch is host-tested only |
| `PLUS` | PLUS held at plug-in: self-powered, Rd pinned on CC2 | `m=plus-rd-cc2`, `sw0` 0x02. OK held pins CC1 (`ok-rd-cc1`). Never both lines |
| `COME` | Comeback boot: a cold plain boot that reads a `/rd.rec` marker and keeps its Rd from the first write | `m=plain` plus `pin=<n>` in `bootinfo` with no key held; `[RDI] comeback boot: <kind> pin=<n>` |

Comeback marker kinds (`include/k2/core/rd_intent.hpp`): `Session` (a pinned K2 with no host was Ready), `Reattach`
(the charger test's planned re-attach, carries the cable identity, pin 2 only), `Rescan` (Connection page Refresh
bridged cycle, pin only). A comeback is judged in 1.5 s: Source_Capabilities seen = vouched; no Rp by 500 ms or no
caps by 1500 ms = released (Rd, Ra, spoof and the record dropped).

### 1.2 Bottom port partner (receptacle)

| Value | Definition | Wire signature the K2 sees |
|---|---|---|
| `none` | Nothing | CC1 and CC2 near 0 V, VBUS 0 |
| `PDsrc` | PD source: charger, PPS, EPR | Rp 3.2-3.3 V on a line while waiting (K2 passive), BC_LVL 1/2/3 = 0.5/1.5/3 A once Rd is shown, then Source_Capabilities |
| `DRPsrc` | PD DRP source: hub, power bank, laptop acting as source | As `PDsrc` but 5 V only at first, a Source_Capabilities with the DRP bit set, answers Get_Source_Cap |
| `Rpsrc` | Non-PD Type-C source, Rp only | Rp on a line, no PD traffic after 1 s (`typec_only`) |
| `legacy` | Fast-charge protocol on D+/D-: QC, AFC, FCP, SCP, UFCS, Mi, VOOC | 5 V on VBUS with no CC Rp (A-port) or a PD source that is PD-silent; the `dpdm` switch network answers |
| `sink` | A sink plugged into the bottom by mistake, nothing else | Nothing: a sink's Rd needs a source's Rp to show a level, the K2 offers none, so CC reads about 0 V and VBUS 0 (a DRP phone that toggles to source is a `DRPsrc`) |

### 1.3 Top plug partner

| Value | Definition |
|---|---|
| `none` | Nothing on the plug |
| `sink` | Phone (DRP), laptop, power bank as a sink, a dumb Rd-only load: each presents Rd 5.1 kOhm on one CC line, and the K2 has no Rp to read it against, so a sink alone is invisible to the K2 (CC about 0 V); behind a charger it shows as inline load |
| `src` | A source plugged into the top by mistake or by design (the charger is on the top plug in the spoof runs) |
| `via` | The top plug reaches a device through a cable (adds the cable axis) |

### 1.4 Cable between K2 and the far device

| Value | Definition | How the K2 can tell |
|---|---|---|
| `direct` | No cable, plug straight in | Rp on both lines on a receptacle (Ra guard) |
| `p3` | Passive 3 A, no e-marker | e-marker read returns `valid=False`, no SOP' answer |
| `e5` | E-marked 5 A (30 V, 100 W) | e-marker read: SOP' Discover Identity, cable VDO says 5 A |
| `epr` | EPR 50 V 5 A | e-marker read: 50 V 5 A 240 W, `EPR cable` |
| `act` | Active cable | e-marker read: active bit set, VCONN required |
| `c2a` | C-to-A or A-to-C, legacy Rp 56 kOhm | One line only, no PD traffic, 5 V without negotiation |
| `usb2` / `usb3` | Data lanes: USB 2 only vs USB 3 | Only the e-marker speed field; no CC behaviour difference |
| `c6` | The CUKTECH 6 A cable: 6 A only through the source's proprietary chain; no readable e-marker | e-marker read unreliable; the K2 uses its built-in id 2 spoof |
| `spoof` | The K2's own Ra: PC13 = Ra on CC1, Rd on CC2, answers the charger's SOP' with a built-in identity (id 0 generic 20 V 5 A, 1 EPR 50 V 5 A, 2 CUKTECH 6A, 4 OnePlus 100 W) | `[EMK] spoof ARMED id N requests=N replies=N` |

### 1.5 CC orientation and the PLUS coupling

- `CC1` / `CC2`: which line carries the far device's Rp (or Rd). A direct receptacle reads Rp on both lines; a cable
  reads it on one (`include/k2/pd/ra_guard.hpp`, host test `tests/test_ra_guard.cpp`).
- Float levels: waiting charger Rp 3.2-3.3 V; idle CC2 net about 1.7 V (PLUS coupling), Ra 0.3 V, idle CC1 0 V.
- The PLUS key is coupled to the CC2 net: idle 1.707 V, pressed with a charger attached it sags to 480-660 mV; held
  for 5 s against a 3 A Rp the coupling alone looks like an Rd (about 5.2 kOhm) and the source raised VBUS (row P3).
  The OK key has no effect.

## 2. The matrix

Reaction vocabulary: **Rd** = the K2 presents Rd on a line (CC1 / CC2 / none). **SOP** = the K2 answers or transmits
on SOP. **Ra** = Ra/spoof active. **Req** = the K2's own sink Request. A "passive" K2 changes none of Rd, Ra or Req and
only measures. "Refresh" is the Connection page's cycle; "Read cable" is the e-marker read through VCONN; "CT" is the
charger test.

Status: `V` verified, `VH` verified-host, `U` unverified, `EB` exception: bug, `EQ` exception: quirk, `ED` exception: disproven.
Evidence and witness lines are in section 3 by id.

### 2.1 HID-powered K2 (left port on a host, plain boot)

| Id | Bottom | Top | Cable | Must detect | Required reaction | Status |
|---|---|---|---|---|---|---|
| H1 | none | none | none | no source, no sink | Rd none, no TX; UI "no source"; Read cable / Refresh / CT refuse with a reason | U |
| H2 | PDsrc waiting | none | direct | a charger waiting for a sink (VBUS ~0, Rp ~3.3 V on a line) | Rd none, Req none, no TX; UI "waiting" not "nothing"; a pd-reread or CT START is the only path to an Rd | V |
| H3 | none | PDsrc (EPR, Baseus 140 W) | `spoof` (no cable) | a PD source with caps; the spoof active | Rd CC2 and Ra CC1 (PC13), one Req from the PD engine, SOP answers GoodCRC, SOP' answered with the built-in identity | V |
| H4 | PDsrc (port not recorded) | none | direct, dumb plug, no Ra | a source that does not read the cable | Rd on the Rp line only for the run; no SOP' arrives, cable "not read", limit 20 V / 3 A default | V |
| H5 | PDsrc behind a splitter | none | direct | the line the source sits on, after an Rd-less source shows Rp only on CC1 | passive pick may pick the wrong line; one Rd move after 600 ms, then caps, no detach | EB |
| H6 | PDsrc (charger) | sink (Pixel) | direct | inline: both a source and a sink, current > 50 mA or a foreign Request | passive: no Rd, no TX on SOP; LINK inline=yes; UI "Live sink" | EB |
| H7 | DRPsrc (hub 5 V 0.9 A) | sink (Pixel) | direct | inline with a DRP source | passive: no Rd, no TX, no VDM answers; inline never flaps | EB |
| H8 | PDsrc (charger) | sink (OnePlus 15) | direct | inline, 263 s of a live phone charge | passive: not one TX; RX Accept / PS_RDY with auto-CRC off; sniff only | V |
| H9 | none | laptop (sink) | direct or `e5` | a laptop sink on a live port | passive; no Rd on either line; no active PD; Read cable / Spoof / CT refuse | U |
| H10 | Rpsrc (0.5/1.5/3 A) | none | direct | Rp, no PD traffic for 1 s | `typec_only` flag; Rd stays none; UI "Type-C only (3 A Rp)" not "no source" | U |
| H11 | legacy (A-port) | none | `c2a` | 5 V without CC, legacy fast charge present | no Rd, no PD; D+/D- probing allowed in CT Legacy mode; Refresh never opens CC | V |
| H12 | legacy (a C-port charger, QC / UFCS / FCP / SCP / AFC) | none | direct | the legacy handshake on D+/D- | CT Legacy mode (PD-silent attach), legacy units first; `silent-sink-*` quirks handled by the CT | U |
| H13 | sink (phone) alone | none | direct | no source: a phone is not a source | passive: Rd none, VBUS 0 stays 0; if a DRP phone sources 5 V, it is a `DRPsrc` and the K2 follows H6 / H7 | U |
| H14 | DRPsrc (power bank, Xiaomi 20K) | none | direct | a DRP source as a source | pd-reread or CT START: Rd on the Rp line, Req within ~25 ms of caps, no PR_Swap offered | U |
| H15 | PDsrc (port not recorded) | none | `e5` (OnePlus 100 W C-C) | cable read by VCONN | Read cable: VCONN on the inactive CC (PC13), 30 V 5 A reported; VCONN off after | V |
| H16 | PDsrc (port not recorded) | none | `epr` | cable read, EPR 50 V 5 A | Read cable: 50 V 5 A, `EPR cable` | V |
| H17 | PDsrc (port not recorded) | none | `p3` or `c2a` | no e-marker | Read cable: `valid=False`, no VCONN left on | V |
| H18 | PDsrc | none | `act` | active cable | Read cable: active bit, VCONN required | U |
| H19 | PDsrc | none | `usb3` / `usb2` | lanes not visible on CC | no change in CC behaviour; e-marker speed field only | U |
| H20 | any | via | `spoof`, a refusing state (inline, live contract that is not the self-hold, Busy, CC1 pin, VCONN) | Ra refused | footers "Live sink: unplug load", "Ra refused: contract live"; no Ra | VH |
| H21 | PDsrc (Baseus 140 W) | none | `spoof` on the bottom receptacle | the spoof is only proven on the top plug | Ra on CC1, Rd on CC2, the charger reads the cable at attach | U |

### 2.2 VBUS-powered plain boot

| Id | Bottom | Top | Cable | Must detect | Required reaction | Status |
|---|---|---|---|---|---|---|
| V1 | legacy / A-port 5 V | none | `c2a` | no host, VBUS >= 4 V for 2 s: bus-powered | `power_source` latch bus-powered; plain boot passive; CT may run a Legacy attach; Refresh never cycles CC | VH |

### 2.3 PLUS-boot self-powered K2

| Id | Bottom | Top | Cable | Must detect | Required reaction | Status |
|---|---|---|---|---|---|---|
| P1 | none | PDsrc (CUKTECH 120 W, Xiaomi 6 A chain) | `spoof` id 2 | direct plug, charger answers Rd within 600 ms | Rd CC2 pinned from the first write, SOP answers GoodCRC, self-hold a PDO 1 Request at 1000 mA, then a run | V |
| P2 | none | PDsrc on CC1 side | direct | Rp on the other line, no VBUS at 600 ms | the single Rd moves once to the other line (`moved_pin`), never both | VH |
| P3 | PDsrc 3 A Rp | none | direct, PLUS held 5 s | the key coupling is an Rd (~5.2 kOhm) | the K2 does not act on the coupling: documented, no firmware handling | EQ |
| P4 | PDsrc | sink on the other port | direct | pinned K2 with a sink downstream | no self-hold (`self_hold_wanted` false when inline), Rd drops when inline latches | U |
| P5 | PDsrc (Apple PD 2.0) | none | direct | self-powered CT at the legacy -> PD boundary | K2 stays up through the unit boundary | EB |

### 2.4 Comeback boot

| Id | Bottom | Top | Cable | Must detect | Required reaction | Status |
|---|---|---|---|---|---|---|
| C1 | none | PDsrc (CUKTECH 120 W, Xiaomi 6 A chain) | `spoof` id 2 | planned re-attach (`Reattach`, pin 2) | Rd CC2 from 30 ms, Ra on CC1 plus spoof re-armed, charger reads the cable at the real attach, 6 A rows run | V |
| C2 | none | PDsrc | direct | `Session` after a charger Hard Reset | the same sink comes back, Rd from the first write, caps vouch it within 1.5 s | VH |
| C3 | none | PDsrc | direct | `Rescan` after a bridged Refresh | lands back on the Connection page, Rd kept, no identity | EB |
| C4 | none | none | none | a stale `/rd.rec` on a plain cold boot that has a USB host | no Rd: a K2 with a host never reads the marker | EB |
| C5 | none | none | none | a comeback with no Rp anywhere | released at 500 ms: Rd, Ra, spoof and record dropped | VH |

### 2.5 Orientation

| Id | Bottom | Top | Cable | Must detect | Required reaction | Status |
|---|---|---|---|---|---|---|
| O1 | any source | any | direct, flipped | the Rp line (CC1 or CC2) and which CC gets VCONN | orientation A / B both read; no cable found restores CC to its pre-scan state; VCONN needs PC13 and the far line | VH |

## 3. Evidence

Each verified row has a date, build hash (or "unstamped"), the file under `runs/`, and one or two witness lines.

### H2: a waiting charger, K2 passive (2026-11-09, build not stamped, before d615f3e)

`runs/anker-split/log1.txt`: `[ADC] dp=653mV dm=653mV cc1=3297mV cc2=1658mV aux=5051mV`, `[PWR] ... vbus=3mV ibus=0mA dir=idle`.
`runs/ccprobe/base-cc1.txt` (2026-11-06, probe, unstamped): `CC1 3981 (3207 mV) CC2 3983 (3208 mV) VBUS 39 mV`.

### H3: PD EPR charger, spoof on (2026-11-06, builds 923a7de / dbad52c)

`runs/emk-ra/a/run.txt` (923a7de): `reread done ra mask=0x0F answered=get-cap ...`, `spoof ARMED id 0 requests=1 replies=1`.
`runs/emk-ra/c3/pre.txt` (dbad52c): `vbus_mv: 1291`, `cc1_mv: 323`, `cc2_mv: 3206`: Ra on CC1, Rp on CC2.
`runs/emk-ra/b/run4.txt` (923a7de): `trigger ready result=ok held=True epr=True pdo=8 28 V 5 A hard_resets=0`.
`runs/emk-ra/c3/ct.txt`: `- EPR fixed: 28 V, all pass (28.28-28.28 V measured)`.

### H4: a dumb plug gets no SOP' (2026-11-05, build unstamped)

`runs/emk-dumb/03-spoof.txt`: `spoof ARMED id 0 requests=0 replies=0`; `runs/emk-dumb/04-dump.txt` carries zero `SOP'` lines
in 11.8 KB while `runs/emk-dumb/05-pdos.txt` shows the charger's 6 PDOs (`source caps: valid=True PD3.x count=6`). Which port
carried the charger is not recorded in the file.

### H8: a phone inline, K2 passive (2026-11-07, build 32a279f, HID-powered, OnePlus 15)

`runs/op15-inline/rec-32a279f/timeline.txt`: `877440.000 ms POWER VBUS 7435 mV IBUS 2418 mA CC1 1 CC2 1670`; no `TX` line from
877 s until the reread at 1140 s (first TX at 1141467 ms), 263 s of passive observation; `RX Accept id 2 (auto-crc OFF)`,
`RX PS_RDY id 3 (auto-crc OFF)`. `BOOT ... m=plain pin=0,0,0,0 mk=0`.

### H11: legacy via a C-to-A cable (2026-11-06, build unstamped; the source is not named in the file)

`runs/t23b/summary.txt`: `- QC2: 5V, 9V, 12V, all pass (5.03-12.05 V measured)`, `- QC3: 5V ok, 9V ok, 12V ok, 9V ok, 5V ok`,
`Run 1 · unknown · 0 PDOs · cable not read, default limit 20 V/3 A · SPOOF ON`. A second capture on an A-port
(`runs/cuktech-usba/summary.txt`, 2026-11-06) fails every row with `PD or load active at 5.11 V`; the two files do not
say what differed, see X9 in section 5.

### H15 / H16 / H17: e-marker reads (2026-11-06 02:23-02:35, builds unstamped)

`runs/cable-reads/oneplus-c2c-2.txt`: `e-marker valid=True ... vid=0x22D9 pid=0x6116 speed='USB 2.0 (480M)'`,
`30 V 5 A 100 W  VDOs: 0xFF00A041 0x180022D9 ...`, `SOP' PD 3.x VDO v0 ... Passive (VCONN required)`.
`runs/cable-reads/cuktech6a.txt`: `e-marker valid=True active=False ... vid=0x2B01 pid=0x4051`, `50 V 5 A 240 W ... EPR cable`
(the file is named after the identity that was saved; the cable it read is an EPR 5 A one; the brief's "CUKTECH 6 A cable has no readable e-marker" is
therefore about the 6 A chain, not about this cable. Listed as an open question in section 5).
`runs/cable-reads/oneplus-c2a-rot1.txt`: `e-marker valid=False active=False vid=0x0000 pid=0x0000`, `none PD None VDO v0`.

### P1: self-powered PLUS boot with spoof (2026-11-09, build v1.0-77-g1e3b67b)

`runs/selfpwr-6a-ship/card6.txt`: `Card 6 · (unnamed) · 7 PDOs · 2 runs · 0000:0000 · 120 W · fw 01E3B67B · SPOOF: CUKTECH 6A · self-powered`,
`PPS 5-11V 6A: ok whole range`, `PPS 5-20V 6A: reached 19.9 V`. `ct.txt`: `run 1 rev 60: done (none); step 48/48`.

### C1: planned re-attach, the legitimate comeback boot (2026-11-09, build v1.0-77-g1e3b67b)

`runs/selfpwr-6a-ship/bootinfo.json`: `m=plain k=0 pin=2,2,2,0 mk=- ra=0:cable sw=A@83,A@643,A@3002`: a plain cold boot, no key,
Rd on CC2 from the first write, no mark: the marker was read and consumed. Same signature as the inline finding in H6; see section 5.

### H6: charger -> K2 -> Pixel with the K2 hijacking the link (2026-11-09, build v1.0-101-gb00178c, HID-powered)

`runs/2026-11-09-rescan/bb1.txt` and `bootinfo1.json`: `REG SWITCHES0 <= 0x02 PDWN2` at 30.106 ms (a plain boot, no key, with an Rd);
`caps-seen: auto_sink=False inline=False held=False sink_state=idle` at 266.468 ms;
`272.433 ms ... TX SOP Request (id 0, PD3.x, snk/ufp) [1.6 ms after RX Source_Capabilities]`;
`300.174 ms RESET *** Hard Reset received ***`; eleven K2 Requests at inline=False between 1492 and 2915 ms;
`3071.055 ms RX SOP Request (id 0 ...) crc=25b3296a [155.8 ms after our TX Request]` (the Pixel's own Request) followed by
`3072.622 ms TX SOP Request (id 2 ...) [1.6 ms after RX Request]`; `5056.515 LINK inline=yes auto_sink=no` then `REG SWITCHES0 <= 0x08 MEAS_CC2`
(Rd dropped once inline latched). `bootinfo1.json` `qr_text`: `m=plain ... pin=2,2,2,0 mk=-`, `warm false`, `aux_mv ~5041`.

### H7: hub -> K2 -> Pixel (2026-11-09, same boot, `runs/2026-11-09-rescan/bb3.txt`)

`184267.033 Source_Capabilities`; `184268.821 TX Request`; `184308.881 RX SOP PS_RDY (id 2, PD3.x, src/dfp) ... [40.1 ms after our TX Request]`;
K2 `TX Vendor_Defined` answers at 184329.743, 184352.745, 184529.870 and 184551.874, `Not_Supported` from the hub at 184558.146; Hard Resets at
188187.180 and 189291.203; nine `LINK inline` transitions from `yes@5056.515` to `yes@188069.780`.

### H5: partial hardware evidence

`runs/anker-split/bb4.txt` (2026-11-09, before d615f3e, HID plain boot): `912723.366 ms CC attach CC2, STATUS0 COMP BC_LVL=3 >1230 mV (3 A)`,
`922193.677 ms CC detach CC2, STATUS0 BC_LVL=0 <200 mV (Ra/open)` (500 ms after the Rd on CC2), `948914.029 ms CC attach CC1, STATUS0 COMP BC_LVL=3`.
After d615f3e the Rd seek moved CC2 -> CC1 -> CC2 -> CC1 with no source attached; the exact case has not been caught live since the fix.

### Host-verified rows

- H20: `tests/test_emk_ra.cpp` and `tests/test_ra_guard.cpp` (Rp >= 2400 mV on both lines = direct receptacle, one line = cable, none = no source, VBUS < 1000 mV, passive).
- V1: `tests/test_power_source.cpp` (no host after 2 s with VBUS >= 4 V latches bus-powered).
- P2: `tests/test_bootmode.cpp` (`moved_pin(2, 600, 0, 3300, 0)` = 1, `moved_pin(1, 600, 0, 0, 3300)` = 2, `moved_pin(2, 599, ...)` = 2) and `tests/test_rdseek.cpp` (the passive seek's budget).
- C2, C5: `tests/test_rd_intent.cpp` (`test_the_comeback_judge`, `test_the_follower`, `test_the_boot_mode_decision`).
- O1: `tests/test_fusb302_scan.cpp` (`test_cable_on_cc1_orientation_a`, `test_cable_on_cc2_orientation_b`, `test_no_cable_scans_both_orientations_and_restores`, `test_vconn_needs_pc13_and_the_far_line`, `test_sniff_never_touches_cc_or_vconn`).
- C3 (logic only): `tests/test_rescan_bridge.cpp` (`test_the_decision_table`, `test_the_comeback_routing`); hardware pending.

## 4. Invariants

Each is a cross-row rule. "Confirms" lists the rows that witness it; "Violated by" lists rows that break it (each is an
exception in section 5).

| Id | Rule | Confirms | Violated by |
|---|---|---|---|
| I1 | A plain boot without a marker presents no Rd: `SWITCHES0` stays 0x00 until the first scan | H2, H8 (`pin=0,0,0,0`), `runs/selfpwr-drop/bootinfo.txt` (build v1.0-59-g264e416: `SWITCHES0 read back: 0x00 at 51 ms, 0x00 at 601 ms, 0x08 at 3016 ms`, `Rd pin: first none, now none`) | H6, H7, C4 |
| I2 | An inline K2 (a source and a live sink) never presents Rd and never transmits on SOP | H8 (263 s of passive sniff) | H6, H7 |
| I3 | An inline K2 never answers a sink's vendor messages (VDM) or Requests on behalf of the source | H8 | H7 |
| I4 | Refresh never opens CC while a real sink may be inline | H6, H7 (rule), `tests/test_rescan_bridge.cpp` | C3 (pending hardware) |
| I5 | Rd is never held on both lines (Debug Accessory latch, OnePlus 120 W, 2026-11-06) | P1, C1, P2 (`pin_mark` accepts exactly one line) | the 0.13 ms SW_RES window (section 5) |
| I6 | A direct-receptacle charger sees Ra only when no sink is inline, no contract is live (except the self-hold), nothing is Busy, no CC1 pin and no VCONN | H3, H20 | none known |
| I7 | The K2 never requests what is not advertised; the 6 A rows run only against an advertised 6 A or the Xiaomi chain | P1, C1 | none known |
| I8 | One Request owner (the PD engine) and one Request in flight | the ROADMAP Verified list cites `runs/anker-split/bb12.txt` (3a37f22); that file was not re-read for this document | H6 (the K2 sent 11 Requests alongside the Pixel's) |
| I9 | No VCONN toward a source | host only: `tests/test_fusb302_scan.cpp` (`test_inline_gate_refuses_unconfirmed_vconn`, `test_vconn_needs_pc13_and_the_far_line`) | none known |
| I10 | A pinned (self-powered) K2 answers GoodCRC and self-holds a PDO 1 Request at 1000 mA | host only: `tests/test_pd_ccmode.cpp` (`self_hold_wanted`); P1 ran self-powered end to end | P4 (unverified) |
| I11 | A comeback marker is bounded: judged at 500 / 1500 ms, released when no caps | C1, C2, C5 | C4, H6 (a marker that survives a host-powered boot) |
| I12 | A dumb Rd-only top plug gets no SOP' from a charger; only the spoof's Ra on CC1 draws a cable look | H4, H3 | `K2_PORTS_SPEC.md` "Present Ra: no firmware path" (disproven, section 5) |

## 5. Exceptions

Every exception has a disposition (fix, disprove, quirk) and a next action.

| Id | Rows | Claim vs evidence | Disposition | Next action |
|---|---|---|---|---|
| X1 | H6, H7, C4 | **A plain boot is passive** is contradicted by 2026-11-09 `bb1.txt`: Rd on CC2 from 30 ms on a plain HID-powered boot, then a K2 Request 1.6 ms after caps while a Pixel was behind it (the Pixel's own Request collided at 3071 ms). Mechanism (inferred, not proven): a `/rd.rec` marker survived, `reads_marker` is `Plain && !warm` with no host test (`src/core/rd_comeback.cpp:64-74` reads regardless of a USB host), and `bootinfo` shows `pin=2,2,2,0` with `mk=-`, the same signature as the legitimate comeback boot C1 | `exception: bug`. ROADMAP row "Stale Rd marker (accepted for now)" understates it: it said "Rd for 0.5-1.5 s then released", the capture shows 12 Requests over 3 s and a collision | Fix: a host-powered boot never reads the marker; if the marker is read, no Request while a foreign Request may arrive; owned by the `k2/inline-passive` work. Re-run H6 / H7 after |
| X2 | H7 | **An inline K2 never answers sink messages** is contradicted by `bb3.txt`: a held contract with PS_RDY, VDMs answered, `Not_Supported` from the hub, inline flapping nine times, source Hard Resets | `exception: bug` | Fix in the inline-passive work; a K2 that latched inline must stop its Requests and VDM answers |
| X3 | C3 | Refresh on a self-powered K2: the bridged cycle (`CycleBridged`) is the fix for the K2 killing itself | `exception: bug` ("Connection -> Source -> Refresh kills a self-powered K2", fixed, hardware check pending) | Run T23 B with the Rescan comeback (section 6) |
| X4 | H5 | Passive pick on the wrong CC line behind a source that shows Rp only on one line after an Rd | `exception: bug` ("Passive pick on the wrong CC line", fixed in d615f3e, partial hardware). Also a quirk: `rp-after-rd-on-cc1` in `docs/CHARGER_QUIRKS.md` | Catch the exact case live (section 6) |
| X5 | P5 | Self-powered source loss at the legacy -> PD boundary (Apple PD 2.0, step 16/49 on 091e80e) | `exception: bug` ("Self-powered source loss at the legacy -> PD boundary", cause unproven) | Black Box capture on VBUS loss |
| X6 | P3 | PLUS held with a charger waiting looks like an Rd (about 5.2 kOhm at 3 A Rp) and a source raised VBUS to 5.12 V at no load | `exception: quirk` ("Hardware quirk: PLUS key is coupled to CC2", `docs/CHARGER_QUIRKS.md`; evidence `runs/epr/buttons/hold2.tsv`, 2026-11-05, Baseus 140 W on the top plug) | None: documented hardware, no firmware handling |
| X7 | the Ra rows | `docs/K2_PORTS_SPEC.md` line "Present Ra to a charger ... no firmware path" and the line that says an e-marker spoof on the top plug sees no SOP' are older than the working Ra mode | `exception: disproven`: the K2 does present Ra (PC13 = Ra on CC1 plus Rd on CC2) and the top-plug spoof works (H3: `spoof ARMED id 0 requests=1 replies=1`). Both statements are true of different configurations: a dumb plug with no Ra still gets no SOP' (H4) | Edit `K2_PORTS_SPEC.md` (done in this commit: a pointer to this matrix) |
| X8 | `SW_RES` window | A 0.13 ms interval with both pull-downs (0x03) during the FUSB302 soft reset on a pinned boot, below every debounce | `exception: bug` ("Both Rd for 0.13 ms at SW_RES", not seen to matter) | Fix in the driver: skip `SW_RES` on a pinned boot or write `SWITCHES0` in the same burst |
| X9 | H11 | `runs/cuktech-usba/summary.txt` reads every row as `PD or load active at 5.11 V` on a C-to-A attach, while `runs/t23b/summary.txt` passes QC2 / QC3 on the same cable type. The A-port source and the K2's own state differ between the runs and the file does not say which | open question, not yet an exception | Re-run H11 and name the source and the K2 power state in the file |
| X10 | H16 | The brief calls the CUKTECH 6 A cable unreadable, but `runs/cable-reads/cuktech6a.txt` reads valid EPR 50 V 5 A for a cable saved under that label | open question | Re-run Read cable with the CUKTECH 6 A cable alone |

Related older claims and where they stand:

- `docs/SOURCE_SINK_DETECTION.md` says the K2 "never presents Rp"; true. It must also say the K2 does present Rd
  (passive boots excepted) and Ra; its line that links to this matrix was added in this commit.
- `docs/ROADMAP.md` "Stale Rd marker (accepted for now)": see X1.

## 6. Test plan

Each `unverified` row as a concrete procedure. Common: one device tool at a time (`/tmp/k2-device.lock`), never
`k2_remote fault`, no load, no active PD with the laptop inline, HID off by default (enable it in Tools -> USB link,
replug, left port needs the C-to-A cable). Save captures under `runs/<date>-connection/<id>/`.

| Row | Hardware | Ports and power | Commands | Witness lines that confirm |
|---|---|---|---|---|
| H1 | none | HID, nothing on either port | `k2_remote.py --json bootinfo`; screenshot `home` and `pd` | `SWITCHES0 read back: 0x00` three times; the page says no source; `pd_trace.py dump` has no `TX` line |
| H9 | Lenovo laptop (battery out) | laptop on the top plug, HID | `k2_remote.py --json state`; `pd_trace.py dump`; press into Tools -> E-marker and read | no `TX`; Read cable and Spoof refuse with a reason; `bootinfo` shows `pin=0,0,0,0` |
| H10 | a Type-C source that is Rp only (a USB-C port with no PD) | bottom port, HID | `bootinfo`; `pdos`; screenshot `pd` | `typec_only` after about 1 s; UI "Type-C only"; Rd none |
| H12 | FH22 100 W (UFCS), CUKTECH C1 60 W (UFCS), Baseus 40 W port, CJ11 | bottom port, HID, plain boot | `ct_start.py --wait 560` (all units); `pd_trace.py dump > runs/.../dump.txt` | legacy units first; no premature Hard Reset; card lists each protocol |
| H13 | Pixel phone alone | bottom port, HID | `bootinfo`; `pd_trace.py live` for 60 s | VBUS 0 stays 0, no TX, no Rd from the K2 |
| H14 | Xiaomi 20K power bank | bottom port, HID, plain boot | `k2_remote pd-reread`; `pd_trace.py dump` | `TX SOP Request` within 25 ms of the Source_Capabilities, no PR_Swap sent |
| H18 | an active cable (none on hand) | none | blocked: no cable on hand | none |
| H19 | USB 2 C-C cable and a USB 3 cable | top plug, HID | Read cable via Tools -> E-marker | the speed field differs, CC behaviour does not |
| H21 | Baseus 140 W | bottom port, HID | arm the spoof from the Tools menu (presses); `k2_remote pd-reread`; `pd_trace.py dump` | `spoof ARMED ... requests=1 replies=1`; `cc1_mv` about 300 mV |
| P4 | CUKTECH 120 W + Pixel | PLUS held at plug-in, sink on the other port | `bootinfo`; `pd_trace.py dump` | `LINK inline=yes`, `REG SWITCHES0 <= 0x08` after the latch; no 1000 mA self-hold Request after inline |
| C3 (hardware) | CUKTECH 120 W (self-powered) | PLUS boot, nothing else | Connection -> Source -> Refresh; `k2_remote --json bootinfo` after the cut | the K2 returns with `pin=2` and `[RDI] comeback boot: Rescan pin=2`, and then `[RDI] comeback vouched after N ms (caps seen)` |
| C4 | none | HID, a `/rd.rec` left by an unplug | unplug a self-powered run, wait 10 s, plug into HID only; `k2_remote --json bootinfo` | with the fix: `SWITCHES0` 0x00 at every read; today: Rd for up to 1.5 s then `[RDI] comeback released after N ms` |
| H6, H7 | CUKTECH 120 W, hub, Pixel | HID, plain boot | enable HID; `bootinfo`; `pd_trace.py dump` | after the inline-passive fix: no `TX SOP Request` while inline, `SWITCHES0` 0x00, the Pixel keeps its contract |
| H11 | USB 3 hub or an A-port charger via the C-to-A cable | K2 USB port | `ct_start.py --units qc2,qc3 --wait 560` | QC2 / QC3 rows pass with 5.0-12.1 V measured |

### Top 5 to run first

1. H6 / H7 plus C4 (one session): re-prove that a plain HID boot is passive with a live sink, after the inline-passive fix.
2. C3 on hardware: Refresh on a self-powered K2.
3. H1: the empty baseline, 30 s.
4. H12 with the UFCS chargers: legacy rows in a plain attach.
5. H21: the spoof on the bottom receptacle.
