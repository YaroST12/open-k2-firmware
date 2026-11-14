# HID-cable loop probe

A research lab on the K2 (Tools > Loop probe). It reads what a cable looped from the K2's PD-bus port back into its own
HID port shows, at 5 V only, and keeps the raw numbers. It gives no verdict on the K2: the reading of the numbers is
done on the host, with this page as the guide.

Code:
- the record, its layout and the step runner (pure, host-tested): `include/k2/loopprobe/loop_probe.hpp`,
  `src/loopprobe/loop_probe.cpp`, `tests/test_loop_probe.cpp`
- the K2 binding (drivers, littlefs file): `include/k2/loopprobe/loop_probe_hw.hpp`, `src/loopprobe/loop_probe_hw.cpp`
- the CC hold in the FUSB302 driver: `include/k2/loopprobe/loop_cc.hpp`, `fusb302::loop_cc_hold`
- the page: `include/k2/ui/ui_loop_probe.hpp`, `src/ui/ui_loop_probe.cpp`, `tests/test_ui_loop_probe.cpp`
- the decoders: `tools/loop_probe_rec.py` (used by `tools/k2_qr.py` and `tools/k2_remote.py loop-probe`)

## Setup

```
 charger ──cable A──> [K2 PD-bus input]  meter  [K2 PD-bus output] ──loop cable──> [K2 left HID port]
```

- The K2 sits on a charger. The cable under test runs from the K2's PD-bus output port into its own left HID port
  (a USB 2.0 device port). No PC is in the loop: a PC would make the HID port a real USB device link, and the probe
  skips the steps that touch the lines when it sees a host (see "Host on HID").
- The K2 then draws its own supply through its own meter and through the loop cable: the IBUS reading is the K2's own
  draw, and both the VBUS and the GND conductor of the loop cable carry it.
- Default boot is passive inline (no Rd): the HID port's own Rd, through the loop cable's CC wire, is what makes the
  charger turn VBUS on. A charger that waits with VBUS at 0 V means the loop cable's CC wire is not reaching the HID
  port, or the charger needs more (inferred).
- The run never asks for more than 5 V. It refuses to start with VBUS above 5.5 V or the aux rail above 5.8 V, and
  ends the run the moment either rises later (`kMaxVbusMv`, `kMaxAuxMv`). The 5 V lock (branch k2/loop-lock) is a
  separate guard; the probe does not depend on it.

## Running it

1. Tools > Loop probe. The table lists the five steps; the footer says `Cable into own HID port`, or what the stored
   result is.
2. OK arms START (amber `CONFIRM`, a 5 s bar). A second OK within the window starts the run.
3. While it runs, the action area shows a progress bar, `Step n/5 <name>` and the elapsed time. BACK stops the run
   (every line and the backlight restored, the record saved as stopped). Leaving the page stops it the same way.
4. The run ends in the QR share view of the record (BACK returns to the table). Long OK on the table shares the last
   result again, also after a reboot.
5. `python3 tools/k2_remote.py loop-probe` reads the same record from flash as JSON (`--json` as with every command).

The step timers add up to about 7.5 s: baseline 16 x 40 ms, drop 8 x (300 ms + 6 x 40 ms), D+/D- 6 x 200 ms plus two
50 ms pull rows, e-marker at once, CC 1.5 s of warning plus 5 x 3 ms of hold. The UI tick adds a little to each.

## Steps

Each step records `status` (done, skipped, aborted, interrupted), `why`, its start time from START and its duration. A
step whose precondition fails is skipped with the reason and the run goes on; a VBUS or aux rise ends the run.

### 1 Baseline

- **Measures**: 16 samples, 40 ms apart, of VBUS and IBUS (power monitor) and aux, D+, D-, CC1, CC2 (ADC), each as
  avg/min/max; the HID port's line state and USB frame counter at the start and the end; the PD side's attach state
  (active CC, inline, downstream sink, caps, contract, the inline guard's reason); BC_LVL on each CC line with the K2's
  CC switches as found (one CC hold of the as-found state).
- **Hypothesis**: with an intact loop, aux (the HID-side supply, inferred) follows VBUS within the cable's drop, the
  HID PHY sees D+ / D- at the bus levels, and the active CC line sits at the charger's Rp against the HID port's Rd
  (about 0.41 V for default USB power, 0.92 V for 1.5 A, 1.68 V for 3 A, inferred from the Rp currents into 5.1 kΩ).
- **Host on HID**: the frame counter moving between the two HID snapshots (SOFs arriving) or a configured device means a
  real host is on the HID port. The D+/D- and CC steps then skip (`host on HID`): the far end is not this K2.

### 2 Drop (VBUS + GND)

- **Measures**: the K2's own draw stepped through 8 levels, forward then reverse: backlight on, off, off + CPU busy,
  on + CPU busy, then the same back. At each level, after 300 ms of settle, 6 samples of VBUS, IBUS (avg/min/max) and aux
  (avg). "CPU busy" spins the core (no WFI) for 40 ms before each sample while the PD chip and USB stay served every
  millisecond.
- **Why no "mid" backlight**: the backlight is a plain GPIO with no PWM (`include/k2/drivers/pins.hpp:19`), so on and
  off are the only levels.
- **Hypothesis**: VBUS − aux = I × (R_vbus + R_gnd + contacts) + a fixed offset of the two front ends. The offset
  cancels in the deltas between levels: ΔI from the backlight and the CPU against Δ(VBUS − aux) gives the loop
  resistance. The reverse half shows drift (warming, a charger's slow regulation) as a forward/reverse mismatch.
- **Good conductor**: Δ(VBUS − aux) between levels stays within a few mV, near the noise floor (see Limits).
- **Broken or poor conductor**: Δ(VBUS − aux) grows in step with ΔI (an ohm in the path turns a 40 mA step into
  40 mV). A GND conductor that is open returns the current some other way (shield, the charger cable's ground); the
  drop then follows that path's resistance and may jump between the forward and the reverse half (inferred).
- Skipped with `no 5V` when VBUS is below 4 V.

### 3 D+/D- through the HID PHY

- **Measures**: the bus-side D+ and D- driven through 6 states, each framed by 100 ms of 0 V on both lines and held
  100 ms: released/released, 0/0, 0.6/0, 3.3/0, 0/0.6, 0/3.3 V (`kDpPlan`). At the end of each hold: the bus-side D+ and
  D- in mV, the HID PHY's D+ and D- receiver state and the PA11 / PA12 input levels (`Hid::bits`), and the highest VBUS
  seen during the hold. Then the bus lines are released and the HID D+ pull-up is switched off and on (the USB core's
  1.5 kΩ pull-up, or a PA12 weak pull-up when the core is off), reading the bus-side D+ / D- each time.
- **The safe subset**: only plain levels (released, 0 V, 0.6 V, 3.3 V); none of the QC2 high-voltage codes (9, 12, 20 V)
  and not the QC3 entry (0.6 / 3.3); a released line only beside a released or 0 V line, because a charger's D+/D- short
  would carry the driven level onto it and could form one of those codes. A compile-time check rejects any plan with a
  state outside this subset (`dp_state_safe`, `static_assert` in `loop_probe.hpp`). Every hold is 100 ms, far below the
  1 s an HVDCP handshake needs, and every 0/0 frame ends any HVDCP mode. The step watches VBUS through every hold and ends
  the run above 5.5 V.
- **Hypothesis**: through an intact D+ wire, 3.3 V on the bus D+ reads high on the HID PHY's D+ receiver and 0 / 0.6 V
  reads low; the same for D-. The pull-up rows: the HID 1.5 kΩ pull-up switched on raises the bus-side D+ (towards 3.3 V,
  or to a divider against the charger's own D+ network); switched off, it falls back.
- **Good**: each receiver follows its own line, the other line stays put, and the pull-up shows up on the bus-side D+.
- **Broken**: a receiver that never changes (open wire); both receivers moving together (a short between D+ and D-, or a
  charger's DCP short seen through the loop: check the baseline D+ / D- for a charger network first); the pull-up not
  reaching the bus side.
- Skipped with `host on HID` when a host is on the HID port.

### 4 E-marker

- **Measures**: the cable identity the PD driver sniffed (header, cert stat, product, cable VDOs, as received), the
  driver's cable sequence number and the mode of its last VCONN read.
- **Only sniffed**: with VBUS up the K2 never sources VCONN or talks SOP' itself; injecting would remove Rd and make the
  source drop VBUS (`include/k2/pd/pd_emarker_plan.hpp:6-12`, the choice at `:20-24`). An active read is impossible in
  this topology at 5 V without risking the run.
- **Which cable**: the sniff sees what the charger asked over SOP'. In this chain that may be the charger's cable or the
  loop cable; the record cannot tell them apart (inferred).
- Skipped with `none seen` when nothing was sniffed.

### 5 CC wire (last)

- **Measures**: the K2's own Rd on its PD-bus side, per `kCcPlan`: as found, Rd on CC1 only, Rd on CC2 only, no Rd, as
  found again; each held 3 ms. Per state: CC1 and CC2 in raw 12-bit ADC counts, BC_LVL on each line, VBUS. Also the
  CC-switch pins and SWITCHES0 as found, and whether SWITCHES0 read back as found at the end.
- **The HID port's own Rd is not switchable**: it is a fixed part of the left port; the MCU has only D+ / D- of that port
  (`include/k2/drivers/pins.hpp:58-61`). So "drop the Rd one line at a time" is done on the K2's own (PD-bus side) Rd.
- **Hypothesis**: with the loop cable's CC wire intact, the active CC line stays at the charger's Rp against the HID
  port's Rd whatever the K2's own Rd does, with the K2's Rd in parallel halving it (2.55 kΩ); with the wire broken, the
  only Rd is the K2's own, so dropping it leaves the line open (Rp pulled to its open level) and the charger may start to
  drop VBUS, which the 3 ms hold keeps below a source's CC debounce.
- **Warning and reboot record**: the step is announced for 1.5 s first (`CC step: VBUS may drop`, row `running`) and
  only then holds the lines; a cancel in that window touches nothing. Right before the hold the record is saved with
  "in progress" set. If the K2 loses power in the hold, the next boot closes that record as CC `interrupted` /
  `rebooted` (`[LPRB] boot: closed the run interrupted in CC`); the page shows `rebooted` on the CC row and the footer
  `Last run rebooted in CC`. The run does not resume: the record reports the last completed step.
- Skipped with `no 5V` (VBUS below 4.5 V), `host on HID`, `save failed` (the in-progress record could not be written:
  the hold never runs without it) or `refused` (the driver's refusal is in the record: a contract, a PD engine busy,
  VCONN on, VBUS high, the I2C bus).

### 6 Timing and identity

The record carries the firmware version string, the uptime at START, and per step its start (ms from START) and
duration.

## Limits

- **ADC resolution against few-mV drops.** aux is read behind an 11:1 divider on a 12-bit, 3.3 V ADC: about 8.9 mV per
  count, averaged over 100 sweeps in the driver (inferred: about 1 to 2 mV effective). VBUS from the INA226 is about
  1.25 mV per count. A good 1 m cable (about 0.1 to 0.2 Ω round trip, inferred) under a 40 mA backlight step moves the
  drop by 4 to 8 mV: at the noise floor. Read the drop as "within noise" versus "tens of mV", and use the forward and
  reverse halves together; a single level pair is not a measurement.
- The VBUS − aux offset includes both front ends' gain and the HID-side input path; only deltas mean anything.
- IBUS at idle carries a couple of mA of offset; the K2's own draw is small (the backlight step is tens of mA), so the
  probe sees high resistances well and fine differences poorly.
- D+/D- receiver states are digital: they show continuity, not resistance.
- The CC hold is 3 ms per state, and the ADC values are the newest sweep at the end of the hold.

## Reading the result

- On the K2: Tools > Loop probe shows each step's state; the full numbers go out as the QR record.
- QR: the share view shows a `K2:` data frame of type LOOP (one or two parts; the record is LZ4-packed when that saves a
  quarter). Scan every part and run `python3 tools/k2_qr.py decode '<part 1>' '<part 2>'`; the JSON names every value
  with its unit (`_mv`, `_ma`, `_ms`, `_raw` for 12-bit counts). The layout is in `docs/QR_SHARE.md`, "Loop probe record".
- Flash: `python3 tools/k2_remote.py loop-probe` (command 0x57) reads `/lp/last.rec` and prints the same JSON;
  `{"loop_probe": null}` when there is no result.
- Log lines: `[LPRB] start result=<n>`, `[LPRB] end step=<name> status=<n> why=<text> ms=<n>`,
  `[LPRB] cancelled step=<name>`, `[LPRB] boot: closed the run interrupted in <step>`, `[LPRB] write failed ...`,
  `[LPRB] slow write ms=<n>`.

## Hardware test plan (for the coordinator)

All at 5 V, no load on the output. The probe runs from the K2's keys; USB HID only matters for step 1 and for reading
the record with `k2_remote` afterwards. Flash the committed build first.

1. **No loop, no charger** (K2 on a PC through the HID port, HID enabled): the run shows Drop `skip no 5V`, D+/D- and
   CC `skip host on HID`. `k2_remote.py loop-probe` returns the record. Proves the host detection.
2. **Loop on a 5 V-only charger** (or a PD charger with the K2 passive): a full run, every step done or e-marker
   `skip none seen`. Check the heartbeat's `stack_free` after the run, the uptime against the wall clock (no reboot), and
   that VBUS stayed at 5 V in the record (`dpdm.rows[*].vbus_max_mv`, `cc.rows[*].vbus_mv` in the JSON).
3. **Refusals**: on a contract above 5 V (without the loop), START says `VBUS above 5.5V: refused`.
4. **Stop**: BACK in the Drop step: backlight back on, `Stopped. Result saved`, the record shows Drop `aborted` /
   `cancelled`. BACK during the CC warning: no CC hold in the record (`cc.rows` empty, CC `aborted` / `cancelled`).
5. **Known-bad conductor**: the same run through a cable with added series resistance in VBUS or GND (or a damaged
   cable): the drop deltas grow with the backlight step.
6. **QR**: scan the share view with a phone, decode with `tools/k2_qr.py`, compare with `k2_remote.py loop-probe`.
7. **Reboot in CC**: only if the coordinator decides to provoke it (a charger that drops VBUS on the Rd change): after the
   comeback the CC row reads `rebooted` and the log carries `[LPRB] boot: closed the run interrupted in CC`.

## Host report (prototype, `K2_HOST_REPORT=1` builds only)

The same loop, with a USB-C host port (a phone, a PC) in place of the charger: the K2 asks the host's PD port what it is
and records how the host enumerates the K2's HID device. Off in the default build (flash); `make K2_HOST_REPORT=1`.

```
 host USB-C port ──cable A──> [K2 PD-bus input]  meter  [K2 PD-bus output] ──loop cable──> [K2 left HID port]
```

The HID port has a fixed Rd and no PD controller (only D+/D- reach the MCU, `pins.hpp`), so the host sees one sink,
becomes source + DFP, powers the K2 and enumerates its HID device. The K2's FUSB302 on the measured CC line is the only PD
speaker: the general rule "an inline K2 never transmits" stays, and this session is its one exception, on proof that the
far sink is the K2's own HID port (the start refusal below).

Code:
- pure, host-tested: `include/k2/loopprobe/host_report.hpp` / `src/loopprobe/host_report.cpp` (sequencer, refusal,
  record, journal, the page's rows), `tests/test_host_report.cpp`; `include/k2/loopprobe/usb_fp.hpp` /
  `src/loopprobe/usb_fp.cpp` (enumeration recorder and host classifier), `tests/test_usb_fp.cpp`
- the K2 binding: `src/loopprobe/host_report_hw.cpp`; the driver hooks: `fusb302::LoopHost` / `loop_host_*`,
  `usb::set_tap` (a SETUP peek from the packet memory in the USB interrupt)
- the reader: `tools/host_report_rec.py`, `k2_remote.py loop-probe --host` (command 0x57, selector 1),
  `tests/test_host_report_rec.py` (shares `tests/fixtures/host_report.hex` with the C++ test)

### Flow

1. HID on (Tools > USB link), the loop as above, the host powering the K2. Tools > Loop probe, MINUS: the Host view
   (rows Host, PD, Ident, DR swap, USB). PLUS twice arms it: the start refusal runs now (USB link on, a host enumerated
   the HID device or sends SOFs, VBUS 4.0-5.5 V, |IBUS| at most 400 mA, no other sink's Request, no Rd of the K2's own,
   no PD tool busy, no contract), and the journal `/lp/host.rec` is saved as Armed.
2. Replug the host end of the loop. A host that went unanswered stops sending caps after its retry count, and a sink
   cannot restart PD without a Hard Reset, so the session starts on the next boot instead: the K2 reboots on the host's
   VBUS, reads the journal, records the enumeration from the first request, waits up to 6 s for the transient refusals
   (no host yet, no CC attach, no VBUS) and takes the line.
3. The line, while the session owns it: sole sink (the HID port's Rd is not a sink behind the K2; another sink's Request
   still ends the run), no Rd of the K2's own (5.1k beside the HID port's 5.1k reads as 2.55k), Auto-CRC on, PDO 1 at
   5 V asked on every acked caps message, never a Hard Reset from the K2 (sink never_hard_reset; the Env has no Hard
   Reset; the loop guard's 5 V lock stays in force).
4. After the contract settles (200 ms), one question at a time on an idle line: Discover Identity as UFP (PD 3.x),
   Get_Source_Cap_Extended, Get_Revision, Get_Manufacturer_Info (chunked), Get_Status; then, only when no identity came
   as UFP, DR_Swap (journal saved first, after the host's enumeration has gone quiet), Discover Identity / SVIDs / Modes as
   DFP, and DR_Swap back. Answers, Not_Supported, Reject, NAK, BUSY / Wait (retried with backoff, 3 tries), timeouts
   (60 ms) and missing GoodCRC are all recorded. Missing GoodCRC 3 times = one Soft_Reset (at most 2 per run), then the
   question again on the new contract.
5. A host Hard Reset puts the roles back (UFP); the question in flight is never asked again. A reboot inside the run
   resumes it from the journal (the question that was due is marked interrupted); after 3 boots the run is closed.
6. After the run the line stays owned until the host goes away: dropping Auto-CRC under a live contract makes a host
   Hard Reset, which cuts VBUS and the K2 with it.

The record keeps the host's Rp (BC_LVL), VBUS and the idle D+/D- at the start (passive: the BC1.2 hint only; active
primary detection would disturb the live enumeration), all PDOs as advertised (PPS / EPR never requested; the EPR list
is not asked), and up to 24 enumeration events (SETUP fields, bus reset, suspend, resume; SOF as a flag).

### OS guess (UNVERIFIED)

`usbfp::classify` maps the trace to Windows / macOS / Linux / Android / iOS / ChromeOS / charge-only / unknown. Every
rule is a guess from commonly described enumeration habits, pinned by synthetic traces only; re-derive them from real
captures before trusting a label: MS OS string (0xEE) or 64-byte first device read + second reset + qualifier = Windows;
8-byte first read or short string reads first = Apple (configured: macOS, else iOS); 64-byte first read, no qualifier,
255-byte string reads = Linux, plus strings re-read after configuration = Android, plus a suspend = ChromeOS; no
requests and no SOF = charge-only.

### Reading it

- K2: Loop probe, MINUS. `python3 tools/k2_remote.py loop-probe --host` (`--json` for every field and the trace).
- Log: `[LPHR] boot: start|resume|closed`, `[LPHR] start boot=.. resume=.. cc=.. rp=.. vbus=..`, `[LPHR] line owned ...`,
  `[LPHR] host Hard Reset`, `[LPHR] saved state=.. end=..`, `[LPHR] done end=.. os=.. id=.. svids=.. hr=.. sr=..`,
  `[LPHR] not started: <refusal>`.

### Hardware test plan (for the coordinator; nothing here is hardware-proven)

Hosts: OP15 and a Pixel (phones as USB hosts through a C-to-C cable into the K2's PD-bus input), 10 runs each, against
the current Identify-alone path on the same phone. The device rules forbid driving CC with a phone attached unless the
coordinator decides otherwise: this plan needs that decision first.

1. Build `K2_HOST_REPORT=1`, commit, flash. HID on. Loop cable from the PD-bus output into the HID port.
2. Per run: arm (Loop probe, MINUS, PLUS twice), replug the phone end, wait for `[LPHR] done`, read
   `k2_remote.py loop-probe --host --json` into `runs/`, `k2_remote ping` uptime against the wall clock.
3. Pass per run: no VBUS drop the K2 caused (uptime continuous after the session start; `hard_resets` 0 or the host's
   own), contract at 5 V only, every question answered or refused with a reason, the DR_Swap back answered when a swap
   was made, the phone still charging-or-host afterwards.
4. Compare with Identify-alone on the same phone: identity VDOs equal, extra facts (revision, manufacturer info, status,
   source caps extended, SVIDs / modes as DFP) listed, failures (Hard Reset, reboot) counted for both paths.
5. Proof wanted from the hardware: the SETUP peek reads real requests (packet-memory layout), SOF and suspend flags,
   GoodCRC with the DFP data role after a swap, the boot timing (caps still coming when the session takes the line),
   and the journal resume after a pulled cable.
