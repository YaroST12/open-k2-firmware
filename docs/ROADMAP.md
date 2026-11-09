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
- Self-powered 6 A by a planned re-attach (2026-11-09, 1e3b67b): PLUS boot on a CUKTECH 120 W C1, the built-in
  CUKTECH 6A identity, START blinks the K2 off once and the run restarts by itself; card "PPS 5-11V 6A: whole range,
  PPS 5-20V 6A: reached 19.9 V". The same build ran a self-powered Apple PD 2.0 charger to the end.
- Charger-test grid follows the run (2026-11-09, 971cac4 on the Anker splitter): rows listed in execution order,
  fixed PD rows first and the Mi 6A row after them (screenshot `runs/anker-split/s2.png`, local).
- One Request in flight and the verified 5 V release (2026-11-09, 3a37f22, Anker splitter re-announcing mid-run):
  a single PDO 1 Request after the re-announce, VBUS back at 5 V, run "done" instead of "unsafe (5V release
  failed)" (Black Box `runs/anker-split/bb12.txt`, local).

## Current bugs

Open defects, newest first. A row leaves this list when its fix is merged and seen on hardware.

Every row below carries **Fix** (what to change, where) and **Validate** (what proves it: host tests, `make gate`,
and the hardware check). A row leaves this list when its fix is merged and the hardware check has passed; move it to
"Verified on hardware" with the date and the evidence path.

- **A mid-run re-announce that drops one offer skips every remaining PD row** (2026-11-09, open, fix specified below).
  - Evidence: Anker 2-way splitter behind a Baseus 140 W, charger test "PD only", 3a37f22. First list: 5/9/15 V 1.5 A
    + 20 V 2.5 A. About 4 s into the run, the splitter re-split its budget with the dock on its other output and
    re-announced 5 V 1.5 A, 9 V 3 A, 15 V 3 A (20 V gone). The run: 5 V pass, 9 V pass, then 15 V AND both 20 V rows
    "skip (offers changed)". 15 V was still offered, now at 3 A instead of 1.5 A; only 20 V was withdrawn.
  - Root cause: `Runner::follow_list` (`src/chargertest/charger_test.cpp`, the block above `follow_list`) is
    all-or-nothing. It walks the pending PD rows, and the first row that `find_offer` cannot place (here a 20 V row)
    makes it `return` without following anything. The check after it in the poll loop (`offers_changed`, the
    `pdo_covers` test per row) then sets `Reason::CapsChanged`, and `skip_unit` ends every pending row of the unit,
    the still-offered 15 V row included.
  - The per-row behaviour already exists in `Runner::adopt_list`, used only after the K2's own 6 A hunt Reject
    (pattern `reject-then-reduced-list`): rows the new list still offers take the new slot and PDO word; rows it no
    longer offers are skipped as not offered; the run goes on against the new list.
  - Fix (user decision 2026-11-09: re-plan, do not give up):
    1. On a mid-run re-announce with the same wire revision, outside the excluded cases (VBUS dark, a Hard Reset's
       missing list within `kCapsReturnMs`, `preauth_tail`, EPR entry/exit with `epr_stage_ == 2` or a partial EPR
       list, the hunt-Reject path), apply `adopt_list` semantics per row instead of `follow_list`'s early return.
       Rows still offered follow the new slot and word (the 15 V row then asks 15 V at the new 3 A word); only rows
       with no offer are skipped.
    2. A row skipped this way gets a new `Reason` (suggested `Withdrawn`, card/UI text "withdrawn by source"), not
       `CapsChanged`, so the card says the source took that level away mid-run rather than "offers changed". Add the
       text to the reason table at the top of `charger_test.cpp`, the report card JSON and the Python/JS decoders
       (`tools/k2_qr.py`, `docs/site/r/card.js`) if the reason is carried in the card record; check
       `docs/QR_SHARE.md` for the reason code width before adding a value.
    3. The row in flight when the list changes: if its slot still offers it, let it finish; if not, end it as
       withdrawn and release to 5 V (the sink's single-Request gate from 3d8fa2c already serialises that release).
    4. Keep `CapsChanged` for the cases that really are a different charger: another wire revision, no list back
       after `kCapsReturnMs`, or a list where no pending PD row can be placed at all.
    5. The progress grid needs no change: it is built from the step list (98e2f39), so a re-planned list shows by
       itself. Check that `revision_` is bumped and `env_.plan_changed()` is called once per re-plan.
    6. General rebalancing (user requirement 2026-11-09: other Anker multi-port devices rebalance the same way; one more
       is tested on hardware the same evening). Every mid-run re-announce goes through the same per-row re-plan, not only
       a shrink: a list that RAISES an offer (more current, a higher voltage or an APDO coming back) re-plans too, and a
       PDO that comes back after it was withdrawn gets its rows back (pending again, not left as withdrawn) as long as the
       unit has not ended. Several re-announces in one run are normal (re-plan each time, no counter that gives up after
       N). A PPS/AVS APDO whose range or current changes keeps its row when the row's target still fits the new range;
       a target outside it is withdrawn. Note on the card how often the source rebalanced (e.g. "source rebalanced 2x")
       so a short result is explained, and log each re-plan (`[TEST] re-plan: <n> rows follow, <m> withdrawn,
       <k> back`). A rebalance never counts as a failure of the row that was running, and no Request is sent before the
       pending one finishes (the sink gate). Name the pattern `budget-reannounce-mid-run` (already in CHARGER_QUIRKS) and
       detect it by behaviour, not by vendor or VID (project rule: patterns, no VID:PID tables).
  - Tests (`tests/test_charger_test.cpp`): replay the splitter sequence (4-PDO list, rows 5 V/9 V pass, re-announce
    5 V 1.5 A / 9 V 3 A / 15 V 3 A while at 15 V or between rows) and expect 15 V run (pass at the 3 A word), both
    20 V rows `Withdrawn`, the unit not ended, and a single Request in flight throughout. Keep the existing
    `reannounce-after-cable-discovery`, `reject-then-reduced-list`, `preauth-list-after-reset` and EPR-exit tests green.
  - Docs: extend the `budget-reannounce-mid-run` row in `docs/CHARGER_QUIRKS.md` with the handling, and
    `docs/CHARGER_TEST_FLOW.md` where "offers changed" is described.
  - Hardware check: the same splitter setup, `python3 tools/ct_start.py --units pd --wait 540` (release any held PD
    contract first with `k2_remote pd release`, or START refuses with "PD hold: release it"). Expect 5/9/15 V pass and
    20 V "withdrawn by source".
- **Passive pick on the wrong CC line** (2026-11-09, Anker 2-way splitter behind a Baseus 140 W). Fixed in d615f3e,
  partly verified: on hardware the Rd seek moved CC2 -> CC1 -> CC2 -> CC1 and ended cleanly with no source attached,
  and the re-read on the splitter got its 4 PDOs; the exact case (passive CC2 pick, Rp only on CC1) has not been caught
  live since the fix.
  - Validate: HID-powered K2, plain boot, splitter on the bottom port with the cable plugged so the source sits on CC1;
    `k2_remote pd-reread` logs `attach CC2`, then `[FUSB302] Rd seek: ... Rd moves to CC1` within ~200 ms, then
    Source_Capabilities and no detach. A charger plugged straight into the top plug and an inline phone show no
    `Rd seek` line. Evidence into `runs/<date>-rdseek/`.
- **Charger-test spoof list loses selection borders on scroll** (2026-11-09, user report): in step 2/3 (cable / spoof
  choice), scrolling down makes the selection squares lose their bottom or top border line.
  - Fix: the scrolled list repaints row bodies but not the full selection frame; a row that moves under the viewport
    edge keeps a clipped frame. Find the list draw in `src/ui/ui_charger_test.cpp` (the Cable stage) and the shared kit
    list component (`k2-micro-gui` skill, `include/k2/ui/`); on a scroll step repaint every visible row's frame (or the
    whole list viewport) and clip the frame to the viewport, not the row body.
  - Validate: host render test that scrolls the list one row at a time to the end and back and checks that each visible
    row's frame has all four edges (pixel check in the host renderer); hardware: scroll the spoof list on the K2 end to
    end with `k2_remote press` and `k2_remote screenshot` each step; every frame closed.
- **Home screen fonts stretched or squeezed in landscape** (2026-11-09, user report): on the Home page in a landscape
  rotation the glyphs look stretched or squeezed compared with portrait.
  - Fix: compare the glyph path in landscape and portrait (`set_rotation`, the LCD MADCTL / address window in
    `src/drivers/lcd.cpp`, the font blit in the gfx layer, and Home's landscape layout in `src/ui/ui_home.cpp`). Suspects:
    a width/height swap in the address window or the blit stride, or a landscape layout that scales a font (a non-integer
    or non-square scale). Glyphs must render at the same pixel size in every rotation.
  - Validate: a host test that renders the same string in rotation 0 and 1 and compares the glyph bitmaps (equal after
    rotating by 90 degrees); hardware screenshots of Home in all four rotations, text the same size and shape.
- **Planned re-attach counts as a resume** (2026-11-09): the card's "resumed" counter is not masked after the planned
  reboot, so a clean 6 A run reads as resumed once.
  - Fix: the planned re-attach writes its intent marker (`/rd.rec`, `src/core/rd_comeback.cpp`); the run journal's
    resume path should read that the reboot was planned and not count it (keep a separate "planned re-attach" flag on
    the card if it helps the reader).
  - Validate: host test (journal resume after a planned re-attach: resumed stays 0, after an unplanned VBUS loss: 1);
    hardware: self-powered CUKTECH 6 A run (PLUS boot, built-in CUKTECH 6A identity) ends with a card showing no resume.
- **Stale Rd marker** (2026-11-09, accepted for now): a bare unplug after a self-powered run leaves `/rd.rec`; the next
  plain boot shows Rd for 0.5-1.5 s before it releases the marker.
  - Fix: add a `.noinit` SRAM canary written with the marker; a comeback boot trusts the flash marker only when the
    canary survived (a short VBUS loss keeps SRAM long enough? if not, bound the marker by a boot counter instead).
    Measure first whether SRAM survives the comeback's dark period on this board.
  - Validate: host tests of the decision table; hardware: (a) the CUKTECH 6 A planned re-attach still comes back with
    Rd from the first instruction, (b) unplug after a self-powered run, wait 10 s, plug into HID only: `k2_remote
    bootinfo` shows no Rd at any time (SWITCHES0 notes without PDWN bits).
- **Both Rd for 0.13 ms at SW_RES** (2026-11-09): the FUSB302 soft reset on a pinned boot leaves the chip default
  SWITCHES0 = 0x03 (both pull-downs) for 0.13 ms until the pin is written again. Below every debounce; not seen to matter.
  - Fix: on a pinned or comeback boot, skip SW_RES and use PD_RESET only, or write SWITCHES0 in the same I2C burst.
  - Validate: host fusb302 scan test asserting no SWITCHES0 value with both PDWN bits on a pinned boot;
    `k2_remote bootinfo` notes on hardware show no 0x03.
- **Self-powered source loss at the legacy -> PD boundary** (2026-11-09, Apple PD 2.0, step 16/49 on 091e80e): VBUS
  went away and the run stopped until a replug. The K2 takes no VBUS action there; the cause is unproven. Since 1e3b67b
  a self-powered run comes back by itself.
  - Fix: diagnosis first. Arm a trigger-frozen Black Box capture on VBUS loss (`pd_trace.py captures`), re-run the Apple
    charger self-powered, read what the K2 sent in the last 500 ms before the loss (a Hard Reset? a Request the source
    disliked? the legacy unit's D+/D- release?).
  - Validate: the capture names the trigger; then a fix that keeps VBUS up, or a CHARGER_QUIRKS pattern if it is the
    source's behaviour, and a self-powered Apple run that finishes with no comeback boot in `bootinfo`.
- **Flaky host tests** (2026-11-09): `tests/test_k2proto.mjs` "screenshot: chunked strips with log noise" and the C++
  `test_dbg_stream` (fails when its worst pass is over 5 ms on a loaded machine) failed once each in `make gate` and
  passed on re-runs.
  - Fix: remove wall-clock limits from functional tests (count work, not time), or move timing checks behind an
    opt-in flag outside `make gate`.
  - Validate: `make gate` passes 10 times in a row while the machine runs another build (`make -j` in a second tree).
- **Wrong comment** in `tools/k2_qr.py`: "a space can end a part" is false (the last base45 character of a part is at
  most index 32; a space is 36).
  - Fix: correct the comment. Validate: `make gate`.
- **Charger-test journal keeps its own row order** (2026-11-09): `charger_journal.hpp` `detail::plan_order` places the
  6 A hunt rows by its own rule when it rebuilds the steps on a resume: a third copy of the order the grid fix removed.
  - Fix: rebuild from the planner's step list (the same structure the grid uses since 98e2f39).
  - Validate: host test resuming a journal mid-PD with Mi 6A rows: resumed step order equals a fresh plan's order.
- **A PPS row shows a probe after all its APDOs** (2026-11-09, residual of the grid fix): a PPS row holds the chips of
  every APDO, so a Mi 6A probe the planner runs between APDO 1 and APDO 2 reads after the whole PPS row.
  - Fix: one grid row per APDO (or a probe chip inside the PPS row at its place).
  - Validate: host grid test with two APDOs and a probe between them: the grid order equals the executed order.

## Specified work (not started)

Features the user asked for on 2026-11-09, specified for a later implementer. Partial work of a stopped agent is on
local branch `k2/power-levels-wip` (f8226a4: clock/systick/crm/fusb302/power_monitor edits, unfinished, not built);
reuse it or start over. The measured ladder is from the lab op on local commit 5d83a3f.

Measured draw on the HID port at 5 V (inline meter, 2026-11-09):

| Level | State | Draw |
|---|---|---|
| 0 | full speed (240 MHz), panel on | 0.58 W (~114 mA) |
| 1 | panel off | 0.38 W |
| 2 | HCLK /512 on the PLL | 0.076 W |
| 3 | HICK /512, PLL and HEXT off | 0.053 W |
| 4 | + peripheral clocks off | 0.051 W |
| 5 | + FUSB302 POWER=0x01, INA226 power-down | 0.038 W (~7.5 mA) |

- **Idle sleep in the main loop** (largest saving: the 240 MHz core is ~0.3 W of the 0.58 W).
  - Do: when a main-loop pass finds nothing to do, `WFI` until the next SysTick or IRQ. The FUSB302 INT pin must wake
    the core (EXTI) so a Source_Capabilities is still answered inside tSenderResponse (24-30 ms); USB servicing and the
    1 ms button sampling must keep working.
  - Validate: host tests unchanged; hardware: meter on the HID port shows the full-on draw falling (target: well under
    0.5 W with the panel on); CUKTECH and Apple charger runs pass as before with Request latency in the Black Box still
    ~2 ms after the caps; USB screenshots and `k2_remote` keep working.
- **Backlight brightness** (PB7 `pins::lcd_bl` is a plain GPIO today).
  - Do: drive PB7 as TMR4_CH2 PWM (verify the pin mux in the repo's AT32 tables) at 20 kHz or more; Settings
    "Brightness" slider with 5-10 steps, stored in prefs, applied live; full = today's level.
  - Validate: host tests for the setting; hardware: each step visibly different, no flicker on a phone camera, no
    audible whine; meter shows draw falling with brightness.
- **Screen timeout (powersave)**.
  - Do: Settings "Screen timeout" Off / 15 s / 30 s / 1 min / 5 min. After it, dim to a low level, then backlight off
    after a further period (document the choice). Any button wakes the screen and that press does nothing else. A running
    charger test only dims, never turns off.
  - Validate: host tests of the timeout state machine (dim, off, wake, the wake press swallowed, a running test never
    off); hardware: each timeout value observed, START is never pressed by a wake press.
- **Low-power level API and lab op** (from the experiment's ladder).
  - Do: a `k2::lowpower` module with `enter(Level)` / `leave()` that restores clocks, peripheral clocks, FUSB302 POWER,
    INA226 config, LCD and USB; `k2_remote power-level N [--hold S]` and `--steps` (the old 6 x 6 s ladder for a meter).
    Document per level what still works (USB, I2C, PD response, measurement, ADC, buttons, display, flash writes,
    logging), each marked "derived from code" or "measured". Candidate uses for the user to pick from: power-bank
    auto-off threshold tests (the K2's own draw no longer keeps a bank awake), duty-cycled long logging, a standby
    state. Not a use: riding through a VBUS dark (hold-up was infeasible even at level 5; the comeback marker handles
    VBUS loss by rebooting).
  - Validate: `make gate`; hardware: each level entered and left 3 times with the meter reading the table above
    (+-10 %), USB and the display back after every `leave()`, PD contract still held at levels where PD is claimed to work.
- **CPU frequency plan** (no implementation yet).
  - Do: a costed plan in the docs. Facts to confirm: PLL input 4 MHz x up to 64, AHB /1 to /512; USB needs exactly
    48 MHz (today PLL 240 MHz / 5) or the HICK 48 MHz with ACC (`crm.hpp` mentions the gates), which would free SYSCLK.
    List the compile-time 240 MHz users that a runtime change must convert (`systick::kSysclkHz`: SysTick reload, the
    bit-banged I2C timing, the D+/D- DWT edge timing, LCD delays, flash wait states).
  - Validate: the plan names every `kSysclkHz` user (a `git grep kSysclkHz` count matches the list) and the USB clock
    option chosen, with the draw at 48/72/96/120 MHz to be measured when implemented.

- **Ripple: calibrate PA1, then show it** (the capture works; its scale is unknown).
  - Seen on hardware (2026-11-09, HID power only, no charger, `runs/2026-11-09-ripple/`): `k2_remote ripple` returns
    512 samples at every sample-time code; mid-scale ~2056 counts (AC-coupled bias), floor 44-47 counts p-p at code
    0 (1072 kS/s), 3 (366 kS/s) and 7 (60 kS/s). The floor is the same at every rate, so it is ADC/board noise.
  - Step 1, calibrate (hardware, no code): TOPADRE 65 W on the bottom port, HID connected, hold 20 V unloaded
    (`k2_remote pd-reread --wait 10`, then request the 20 V PDO) so the charger runs in burst mode
    (`burst-mode-ripple-at-20v`: ~1 V swing, INA sigma ~0.70 V). Take `ripple --code 7 --png` (8.6 ms window, whole
    burst cycles) and `--code 3` several times, and the INA samples over the same seconds (`k2_stream.py record
    --kinds power=50`). Counts per volt = PA1 p-p counts / INA p-p volts; repeat at 5 V and 9 V on a second charger
    to check it is linear and level-independent (AC coupling: the bias must not move with the level). Note the
    coupling's low corner if slow bursts look sagged (droop across the window). Record the factor and the floor in mV.
  - Step 2, firmware: a pure `k2::ripple` stats module (p-p, RMS, dominant period from zero crossings of the
    mean-removed signal, floor subtraction; mV from the step-1 factor as one named constant) with host tests on
    synthetic waveforms (sine, burst, noise-only -> "below floor").
  - Step 3, UI: on the Detail (or Lines) page a ripple readout (mV p-p, frequency) refreshed on demand or every few
    seconds (a capture pauses the ADC scan: keep it short and never during a transition capture); "< floor" when under
    the noise. The k2-micro-gui skill applies.
  - Step 4, charger test: a ripple figure per fixed PDO row, taken after the level settles, reported in the journal and
    card as information (no pass/fail until a threshold is agreed).
  - Validate: host tests for the stats (red before green), `make gate`; on hardware the TOPADRE 20 V reads a p-p within
    20 % of the INA's swing, HID-only reads "< floor", and a charger-test card shows a ripple figure per fixed PDO.

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
- The reboot-resume of a charger run after an unplanned VBUS loss on a self-powered K2 (the comeback marker of
  1e3b67b); a host-powered K2 waited forever for stable 5 V on 2026-11-07.

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
