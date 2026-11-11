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

- Reboot bridging works again (**was "Reboot bridging dead since the SRAM canary"**) (2026-11-11, IQ4 self-powered charger test, card 65): after a VBUS cut
  the K2 came back plain with no Rd, the charger saw no sink and the K2 stayed dark (found with Rp on CC2, VBUS 0).
  Cause: the `.noinit` SRAM canary of 71bd2345 (ROADMAP "Stale Rd marker") never survives a dark on this board. The
  retention probe of 29ba54a0 (`bootinfo` `sr=`) read 0 of 32 words after a 1 s PC-cable dark; the backup registers
  kept the session record (`bp=`). So every comeback judged `/rd.rec` stale. The HID-cable loop hid this, because it
  powers the K2 through the dark.
  - Fix (7c035f66): the voucher is `session::backup_alive()`, the checksummed session record in DR22..DR37 that every
    boot writes. A short dark keeps it; a real power loss zeroes it. The SRAM canary is gone; `ri=` in `bootinfo` shows
    the decision (kind, pin, voucher, host) for every cold boot.
  - Validate: a self-powered IQ4 charger test (PLUS boot, no HID loop, no PC) that cuts VBUS comes back by itself and
    finishes; afterwards `bootinfo` on a comeback boot reads `ri=s2yn` (or `a2yn`). Still to measure: how long the
    backup registers last in a dark (1 s kept, a minutes-long unplug lost them). A charger whose dark exceeds that limit
    still breaks the bridge.
  - Verified: the user's IQ4 self-powered full charger test came back after every VBUS cut and finished (`resumed 2`,
    `bootinfo` after it `ri=s2yn sr=0 bp=11:1`, 2026-11-11).
- The K2 attaches in time for the source's offers (late-attach bug, 2026-11-11; NOVOO -> K2 -> Legion side port, which
  failed 3 of 3 before). Cause: an attach latched on a line reading low while the other net sat in the Rd window, the
  unattached scan at only 2 Hz, and the scan not running at all during a long redraw (a ~150 ms Home repaint as VBUS
  appears). Fix: `pd::ScanPace` (`pd_scanpace.hpp`, kicked by a no-Rp detach, a VBUS rise or a new Rd-window line),
  `pd::rd_window_override` for the low attached line (6cbb12bf), and `fusb302::scan_service()` from the LCD idle hook
  (77a384e8). Evidence: 6cbb12bf 8 of 9 (the miss was the repaint stall), 77a384e8 10 of 10 on CC1, every attach decoding
  Source_Capabilities first, no reboots (`runs/2026-11-11-late-attach/`, verdicts by `tools/cycles.py` there).
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
- `pd attach` takes a fixed PDO at the target before a PPS / AVS range (2026-11-11, 27780f2a on 1.0.0-515, NOVOO RG100
  alone): a second `pd attach` 5000 had picked the PPS APDO (PDO 5) on 511; on 515 5 V -> PDO 1, 9 V -> PDO 2, 12 V ->
  PDO 5 PPS at 12 V (`runs/2026-11-11-novoo-alone/after-515.txt`, local; `pd::pick_attach_pdo`, `tests/test_attach_pick.cpp`).
- The lease settles on a held 5 V contract (2026-11-11, 13e6a618 on 1.0.0-515, NOVOO RG100 alone): after a twin run that
  ended on a held PDO 1 contract at 5.10 V the lease read "not armed ... ended: settled" instead of staying armed into a
  needless stop (`runs/2026-11-11-novoo-alone/after-515.txt`, local; `lease::safe_5v`, `tests/test_lease.cpp`).
- Abort after a twin run (2026-11-11, 1.0.0-511, NOVOO RG100 alone): `pd-reread --abort` after a twin run released the
  lab attach; the earlier "abort ignored" report did not reproduce once the attach-owner fix 4fd3c9ac was in.
- The driver's own PDO 1 answers no longer count as a user hold (2026-11-11, 04fe4644 on 1.0.0-518, NOVOO RG100
  alone): after a twin run `pd status` reads `held=True(auto)`; `pd attach` 9000 reads `held=True` with no `(auto)`;
  a charger-test START right after the auto hold ran PD + QC2 in 60 s (64 s with no hold before it) with no
  "release check not answering" fallback (`runs/2026-11-11-auto-hold/`, local; `Sink::answer_5v`, `pd::user_hold`,
  `pd::auto_own_hold`, `tests/test_charger_test.cpp` `test_an_auto_hold_counts_as_released`). Why `held` itself stays
  set: the sink answers a re-announce in Requesting / Transition only while held (tSenderResponse Hard Reset otherwise).

## Current bugs

Open defects, newest first. A row leaves this list when its fix is merged and seen on hardware.

- **The D+/D- passive idle put QC chargers into HVDCP and let VBUS jump to 12 V** (2026-11-11, CUKTECH 10 GaN 120W
  USB-A; fixed in 872baa31, host-tested; `tests/test_dpdm.cpp`, `tests/test_fastcharge_classify.cpp`
  `test_floating_short_needs_the_active_check`): the idle drove both lines to 0.6 V, the HVDCP entry level and the QC2
  12 V code; a shell `dm 0`, 200 ms, `dm 0.6 confirm` took VBUS 5.1 -> 11.75 V, and after a charger-test run VBUS sat
  at 12.13 V for ~40 s. The idle now releases both lines (CHARGER_QUIRKS `idle-0.6V-enters-hvdcp`). Validate
  (hardware): on the CUKTECH USB-A port, boot shows `[FC] idle dp=hiz dm=hiz mismatch=0x0` and VBUS stays ~5.1 V for
  minutes; `dm 0`, 200 ms, `dm hiz` keeps 5.1 V; after a charger-test run VBUS returns to 5 V; the QC2/QC3 units pass;
  the Lines page shows what a released pair reads alone and on a DCP / Apple / QC charger.
  **Hardware 2026-11-11, 1.0.0-564-eb320818, CUKTECH USB-A:** `dm 0`, 200 ms, `dm hiz` kept VBUS at 5.12 V (old idle:
  11.75 V); the K2-started QC run (self-powered) passed QC2 5/9/12 V, QC3 5→12→5 V, QC3B 5/12 V, UFCS 5/8/11/5 V
  (15 pass, 0 fail), VBUS back at 5.12 V after it. The released pair reads the port's own 2.70/2.68 V divider
  (`APPLE 2.4A`). Still open: the boot `[FC] idle` line, the pair alone and on DCP / QC-only chargers, the IQ4 rerun.
  Evidence `runs/2026-11-11-idle-dpdm-hw/` (integ worktree).

- **`[LINK]` verdict undercounts the caps during a long repaint** (2026-11-11, found by the host world replay on
  k2/pd-replay, not on hardware): when the attach and the caps decode both land inside one long Home repaint, the verdict
  reports `verdict_caps=0` while the driver decoded `caps=1`; the verdict itself is still `present`. Repro:
  `K2_WORLD_ONLY=late-attach-cycles:rise420.8:Home ./host-build/test_world_replay` (and `h-side-inline:rise2538.6:Home`).
  Part of the repaint rework.
- **SCP 10 V takes the K2 down on the Anker IQ4** (2026-11-11, self-powered full charger test on 1.0.0-524-7c035f66):
  SCP 5 V passed (`detected`); the SCP 10 V 2.25 A step took the K2 down three times, so the reboot budget skipped the
  rest of SCP (`rebooted 3x`) and the run finished (`resumed 2`). Cause, seen with the K2 on PC power (`--units scp`,
  `runs/2026-11-11-iq4-scp/`): the IQ4 does not follow SCP 10 V (row timeout at 5.0 V), then cuts VBUS to 0 for ~70 ms
  and restarts PD (CHARGER_QUIRKS `output-reset-after-unanswered-scp`). The charger's behaviour, handled by the budget;
  parked by the user.
- **FCP 12 V not released on the IQ4** (2026-11-11, PC-powered `--units fcp,scp`): the D+/D- reset left VBUS at 12.1 V,
  the run stopped Unsafe "5V release failed" after 8 s; a PD Soft_Reset then gave 5 V in ~150 ms
  (`fc-holds-after-bus-reset`). Seen once; the self-powered full run went on from FCP to SCP 5 V.
- **QC2 / QC3 fail on the IQ4 above 5 V** (2026-11-11 self-powered full run: QC2 9 V `voltage not reached`, QC3
  `timeout`), although QC2 12.1 V and QC3 11.8 V passed on 2026-11-04 (5c548f7). A state left by the vendor units or a
  regression. `--units qc2,qc3` alone (PC-powered, run 4, `runs/2026-11-11-iq4-scp/ct_qc.txt`) fails the same way: QC2 9 V
  at 5.10 V, QC3 / QC3B timeout at 5.10 V, so not a leftover state. Next: the same units on another charger that passed QC
  before, to split a firmware regression from an IQ4 change. CUKTECH 10 GaN 120W USB-A (QC2 12.13 V on 2026-11-07),
  2026-11-11 on 1.0.0-524: through the first cable the port held 5.12 V with no load and answered nothing (QC2/QC3 "no
  handshake": D- stayed at D+ 0.65 V, FCP rejected), likely a port or cable state, since another cable restored the
  usual no-load cycling. With that cable no run could start (next bug). `runs/2026-11-11-cuktech-qc/`.
  **Not a K2 regression:** bus-powered (HID off) the same port passed QC2 5/9.10/12.13 V, QC3 5→11.72→5 V, FCP 9.10 V,
  UFCS 5/8/11/5 V on 1.0.0-524 (run 1, `results_bus.json`), as on 2026-11-07. The IQ4 changed, or a state on its side.
  Found on the way: the K2's 0.6 V / 0.6 V passive D+/D- idle enters HVDCP and is the QC2 12 V code; a D- dip then
  raises VBUS to 12 V unrequested (reproduced with `dm 0` for 200 ms: 5.1 → 11.75 V; after the run it sat at 12.13 V
  ~40 s). Fix on k2/idle-dpdm. Possibly the cause of the IQ4 FCP hold above.
- **The charger test cannot start on a USB-A port that cuts its output with no load** (2026-11-11, CUKTECH 120W USB-A,
  HID-powered K2, 1.0.0-524): preflight refuses "load attached" at 0 mA. The port cuts VBUS every ~6.5 s
  (`usb-a-no-load-auto-off`). An attach does not start inline on this base; once the latch was set, each re-attach
  after a cut restarted the 10 s `kInlineQuietMs` window (the port gives ~6 s), and the collapsing VBUS-fed Rp sweeps
  the CC level through the sink band, which read as a far Rd. Fixed in dee4041c, hardware check pending: a cut (VBUS
  below 4.75 V within 3 s of being up) is neither proof nor evidence. The quiet window pauses through a cut or a short
  detach instead of restarting, a far Rd counts only while VBUS is up, and clearing waits 2 s after any pause. Current
  and sink Requests still latch everywhere (zero-TX inline rule kept). The refusal now logs `latched by <cause>`
  (9401cb5d). Same port, separate path: `ct_start.py` refused "VBUS dropped to 4.42 V after the PD rescan" on one
  sample landing in a cut; it now waits for VBUS (76fbc6b5). START itself already tolerated the sag.
  Validate: K2 on PC/HID power, CUKTECH USB-A port, C-to-A cable, nothing behind the K2: `k2_remote state` shows
  inline false within ~20 s and stays false through 5 or more cuts; `tools/ct_start.py --units qc2,qc3` starts (no
  "load attached", no "dropped to"). If refused, quote the `latched by` line. Then a phone behind the K2 on the same
  port: still inline (`latched by load`).

- **Fast-charge detection says `USB DATA` under a 20 V contract** (2026-11-11, same captures): with the offers missed
  the K2 falls back to the Type-C level. Probably a knock-on of the late attach. Not seen after that fix (10 attaches on
  1.0.0-520-77a384e8, no `USB DATA` while VBUS >= 8 V, `runs/2026-11-11-late-attach/cycles-520/`); the open part is the
  mismatch below. Also seen:
  `k2_remote state` said `USB DATA` while Home said `SVOOC` (the shell must say what the screen says). **Fixed on
  `k2/fc-equal-pair`, hardware check pending:** the shell, remote and heartbeat word (`fastcharge::label()`) and the Home
  row (`home::derive_proto`) now share one rule, `fastcharge::vooc_session_live(verdict, live)`: a live VOOC monitor
  session with a verdict other than `NoInput` reads SVOOC on both (host test
  `test_shell_label_and_home_row_agree_on_a_live_session`). Still open: the Detail / Connection pages take the raw
  `fastcharge::read()` (`src/ui/ui.cpp`, `ctx_.fc`); `fastcharge::label()` there would make every page agree. Validate:
  OP15 on SVOOC, `k2_remote state` and a Home screenshot say the same word.
- **Fast-charge verdict reads `AFC FCP` on an equal steady 0.4 V pair** (2026-11-11, CUKTECH 10 GaN 120W USB-A, no
  load, 1.0.0-564): after shell `dm 0` 200 ms then `dm hiz` the lines read 420 / 387 mV for about 3 s at VBUS 5.12 V and
  the label read `AFC FCP` for about 2 s. Cause: `band_5v` took bucket(D+)=1 with bucket(D-)=0 as AFC/FCP, and the two
  equal lines straddled the 0.3 V bucket edge. **Fixed on `k2/fc-equal-pair`, hardware check pending:**
  `afc_fcp_levels()` refuses an equal pair (also in the 9 V and 12 V bands); an equal 0.6 / 0.6 pair still reads
  `NO QUICK` without the active proof. Validate: on the CUKTECH USB-A port, `dm 0`, 200 ms, `dm hiz`: the label never
  reads `AFC FCP`, and a QC / AFC unit on the desk still detects.
- **Fast-charge verdict flaps during an output sag or cut** (2026-11-11, same port): `APPLE 2.4A` -> `USB DATA` (equal
  ~2 V lines while VBUS is 4.0 .. 4.8 V) -> `NO INPUT` on every ~6.5 s VBUS cut. **Fixed on `k2/fc-equal-pair`, hardware
  check pending:** `SagGuard` holds the last verdict while VBUS is below 4.75 V and for one ring refill after
  (bounded to 400 polls); `NO INPUT` still passes at once. Validate: through three VBUS cuts the label goes `APPLE 2.4A`
  -> `NO INPUT` with no `USB DATA` in between (`k2_remote state` polled every 100 ms); a real USB data device inline
  still reads `USB DATA`.
  **Hardware 1.0.0-594-cd569683 (A-to-C cable, so `cc_source_no_pd` is true; `runs/2026-11-11-integ594-hw/state60.txt`):
  not enough.** At each cut the lines collapsed to an equal ~1.9-2.2 V pair while VBUS was still ~5.0 V (5005 / 1950 /
  1954), the VBUS < 4.75 V trigger came too late, and the hold then kept the wrong `USB DATA` (22 of 60 samples; the
  steady state 5116 / 2699 / 2682 reads `APPLE 2.4A`; no `AFC FCP`). Cause: the mixed 2.7 / 1.95 V window (bucket 7,
  "pulsing") hit the CC data-host tail, and an A-to-C cable's Rp is in the plug of any USB-A charger port, so
  `cc_source_no_pd` is no proof of a data host. **Fixed on `k2/fc-equal-pair`, hardware check pending:**
  (1) `equal_pair_moving()` (a pair that moves together: every sample within 0.2 V, <= 3 stray samples, level >= 0.4 V,
  > 0.2 V change in <= 8 jumps) reads NO QUICK in `classify()` and starts the `SagGuard` hold even at 5.0 V; (2) a steady
  equal pair outside the bias window with `cc_source_no_pd` goes to the passive DCP rule (`DCP 5V`) instead of `USB DATA`;
  (3) `kSagHoldPolls` 250 -> 400 (the observed cut sags about 4 s). Host test: the recorded cut replayed poll by poll
  (`test_cut_replay_never_reads_usb_data_on_an_a_to_c_cable`) never leaves `APPLE 2.4A`; J/K traffic, SE0 and the 0.85 V
  pull-up still read `USB DATA` (`test_equal_pair_is_not_a_data_host_but_real_data_is`). Validate: repeat the 60-sample
  poll on the same port and cable: 0 `USB DATA` samples, `APPLE 2.4A` -> `NO INPUT` at each cut; then a PC port with a
  device behind the K2 (J/K traffic or the 0.7-1.0 V pull-up) still reads `USB DATA`.
- **The recorder stream has no boot id or reset cause** (2026-11-11): two boots in one recording look like one (the
  "Power reads 0" scare). Fixed on `k2/small-fixes`, hardware check pending: a new Boot frame (kind 11: session boot
  counter, CRM reset flags) follows the Hello; `tools/k2_stream.py` prints `BOOT BOUNDARY`, keeps the clock rising,
  tags rows with `boot_index` and judges the device counters per boot. Host: `tests/test_dbg_stream.cpp`,
  `tests/test_k2_stream.py` (`TestBootBoundary`). Validate: one K2 replug (or `reboot`) inside a recording shows two boot
  ids and the reset flags (`k2_stream.py view DIR --kind boot`: `boot #N reset=POR PIN` then `BOOT BOUNDARY`).
  Open: `k2_stream.py record` itself ends when the CDC port vanishes at the replug; the boundary logic runs in whatever
  reader keeps feeding one `Session` across the reboot (a reopening recorder is a follow-up).
- **Twin limiter text with no claimed power** (2026-11-11, twin-observed review): with no claimed source power,
  predict calls a 60 W hub "limited by the cable". The predict rework on `k2/twin-observed` changes this path;
  re-check after its merge.
- **The Black Box saves nothing once the cards fill the volume, and each refusal stalls the loop 41 ms** (2026-11-11,
  1.0.0-570-44e8dcb9, found by `tools/loop_survey.py`: an ~83 ms PD service gap on every page, 0 px painted, twice per
  VBUS cut of the CUKTECH USB-A auto-off port). 61 report cards (cap 64) leave 5 of 90 blocks free; the Black Box keeps
  the 20-block charger-journal reserve, so every capture is refused `Full` (`[BB] saves=0 skipped=N ... last=41ms
  entries=0 free=10240`). `bbstore::append` compresses the capture and walks the volume (`free_blocks()`) before it
  refuses: 41 ms with the loop stopped, two captures (VBUS dip + detach) per cut. Fix: (1) refuse before compressing
  when the reserve cannot be met and there is no own capture to drop (cache the free count); (2) a block budget so the
  64-card cap cannot starve the Black Box (decision pending with the user). Validate: loop_survey on the auto-off port
  shows no 41 ms pass; a Black Box capture saves with 61+ cards. Evidence `runs/loop-survey/` (integ worktree).
- **PD Stress did not release Rd at its end** (open since before 2026-11-11; fixed on `k2/small-fixes`, hardware check
  pending): the run ended with the K2's Rd still on CC, the one a pd-reread presented and the run took over. Every end of a
  run (done, cancelled, a stop that skips the 5 V release, the reboot budget spent) now ends in `PdStressSession::on_finish`
  with `set_run_owner(false)` and `rd_for_run(false)`, the charger test's release path (a self-powered boot's own Rd pin
  comes back inside `rd_for_run`). Host tests: `tests/test_ui_pdstress.cpp` `test_a_*_gives_the_rd_back`.
  Validate (hardware): `k2_remote pd-reread`, start a PD Stress run, let it finish and again cancel one; afterwards CC1/CC2
  show no K2 Rd (charger-only attach: VBUS drops to 0, cc ~3.3 V) and the log has `[PD] Run start over: Rd off`.
- **A refused e-marker read kept the last read's mode and acks** (2026-11-11, fixed in 472c40f9, host-tested;
  `tests/test_fusb302_scan.cpp` `test_refused_read_clears_the_last_read`). Validate (hardware): with a sink inline or
  Ra mode on, `k2_remote emarker --json` after a refused read shows `read_mode` none and 0 acks.
- **Docs: `docs/CHARGER_QUIRKS.md` called the Baseus cable of the FH22 / CUKTECH notes a "plain cable"** (2026-11-11;
  reworded on `k2/small-fixes`, docs only): that cable later answered as chipped (5 A normal). The FH22 section and the
  CUKTECH `5a-without-emarker` note now name it as the Baseus 100 W cable whose chip stays silent when read alone
  (pattern `cable-chip-silent-alone`). The A-to-C "plain cable" of the CUKTECH sag-fix verification (981c831) is another
  cable and was left. Nothing to validate on hardware.

- **Connection page ("Who's attached") flickers on a steady link** (2026-11-10, user report; handoff row 2c): the tab
  refreshed its info without anything new appearing. Fixed in bd9dd91b, hardware check pending.
  - Fix: rows have fixed slots -- a row-count change clears only the vacated slots and moves the closing hairline
    instead of wiping the whole body (the wipe is reserved for page entry, tab change, pop-up close). The volatile
    inputs (fast-charge verdict, D+/D-/CC ADC levels behind the usb-link family and the Rp reading, the Type-C
    row's VBUS) are debounced with a 1 s hold before they reach a row or a paint signature. Host: 0 body passes,
    0 body pixels and 0 match_live calls over 10 s per tab with a flapping fc/usb-link/ADC (tests/test_ui_conn_flicker.cpp,
    red first in b74ceb54); a 5->4->5 row-count change paints only the changed slots and ends pixel-identical to a
    forced resync.
  - Validate (host): tests/test_ui_conn_flicker.cpp. Hardware: the stream log shows 0 `[CONN] body` passes over 30 s
    on each tab with a phone inline on a live contract; replug: at most 1 full pass.
Every row below carries **Fix** (what to change, where) and **Validate** (what proves it: host tests, `make gate`,
and the hardware check). A row leaves this list when its fix is merged and the hardware check has passed; move it to
"Verified on hardware" with the date and the evidence path.

- **Connection page ("Who's attached"): a no-PD gate** (2026-11-10, handoff row 2b): tabs kept placeholder rows and
  offered Identify to a device/source that never talks PD. Fixed in bb0f93d, hardware check pending.
  - Fix: one pure "PD present" verdict per attach (`include/k2/pd/pd_verdict.hpp`, carried by `pd::Link`, Pending
    until attach+1.5 s; sink answered -> Present; Source_Caps repeated >=3 with no GoodCRC/Request -> None/offers
    unanswered; any other decoded message -> Present; else None/source never offered). With verdict None the Sink
    tab shows "No PD from this device" + why + the measured rows (Type-C, Draw), the Source tab "No PD from the
    source" + Type-C + Legacy only when the source never offered, Identify is refused with the same words (the
    alone path stays offered), and the remote state carries `kLinkNoPd`. The empty states (NO DEVICE) are
    unchanged.
  - Validate (host): `tests/test_pd_verdict.cpp` (verdict matrix + Link integration), `tests/test_ui_connection.cpp`
    (copy fits, Identify gating), `tests/test_ui_connection_page.cpp` (no-PD Sink/Source bodies in 4 rotations,
    pixel checks, greyed Identify, Present case unchanged), `tests/test_ui_no_truncation.cpp`; `make gate`.
  - Validate (hardware): Pixel -> K2 -> SSD: the Sink tab shows "No PD from this device" and "offers went
    unanswered", with the power level and the draw, and no placeholder rows. Laptop -> K2 -> SSD: the same.
    A PD charger alone, then with a phone inline: the tabs are unchanged from today, with no "No PD" message.

- ~~**An inline K2 hijacks the link**~~ completed: 026e516 (+5334c08, 122412f); plain boot passive and zero K2 TX inline on hardware (H6, P4, 2026-11-10).
- **Connection -> Source -> Refresh kills a self-powered K2** (2026-11-09, user report): fixed in 667bca2 (+ 7f73ddf,
  8cdc8c3), hardware check pending.
  - Evidence: PLUS-boot K2 powered only by the charger on the bottom port; Actions -> Refresh ran `fusb302::rescan()`,
    which opens both CC lines for 150 ms. The source dropped VBUS, the K2 died, and the next boot was a cold plain boot
    without Rd (the user had to hold PLUS again).
  - Fix, two layers (`src/core/rescan.cpp`, pure rules in `include/k2/core/rescan_bridge.hpp`):
    1. A live explicit contract (any non-inline K2, SPR mode) is refreshed with a Get_Source_Cap through the sink's one
       message gate (`pd::Sink::request_source_caps`): no CC change, no dark. Never sent while a Request or another
       message is outstanding, never in EPR mode (an unsolicited caps there is a hard reset). The answer is Requested
       like any Source_Capabilities (the source's tSenderResponse runs from its send, solicited or not, and it Hard
       Resets without a Request): an unchanged list gives the same RDO, so Accept + PS_RDY with no voltage change; a
       changed list is renegotiated. The 150 ms window covers the caps arriving only, not the PS_RDY. A source silent
       for 150 ms falls through to layer 2; a gate busy for 250 ms gives "Busy, retry" and sends and cycles nothing.
    2. The CC cycle (no contract, EPR mode, silent source). On a K2 that lives on the charger (pinned Rd, no USB host) the
       Rescan marker (`IntentKind::Rescan`, pin only, no identity) is written and read back BEFORE the lines open; a failed
       write refuses the cycle. The comeback keeps Rd from the first instruction and lands on Connection -> Source with
       "Rescanned (rebooted)". It is not a charger-test resume (`supports_run_resume`). A K2 that survives the gap
       hands the marker back (`end_reattach()`).
  - Callers: the Connection page and the remote `action` (same `Ui::run_action`) use both layers. The lab shell
    `pd rescan` (`shell_hw.cpp`) and the charger test's `pd_rescan()` still call `fusb302::rescan()` directly: both can
    still kill a self-powered K2; the charger test is covered by its run journal and Session marker.
  - Validate (host): `tests/test_rescan_bridge.cpp`, `tests/test_pd_sink_getcap.cpp`, `tests/test_rd_intent.cpp`,
    `tests/test_ui_connection_page.cpp`; `make gate`.
  - Validate (hardware, self-powered K2 on a Baseus or CUKTECH bottom port, no USB host): Connection -> Source ->
    Actions -> Refresh, twice.
    1. (a) PD source: `[RESCAN] live contract: Get_Source_Cap` then `[RESCAN] source capabilities refreshed, no CC
       change`; the caps list refreshes, no reboot, `k2_remote ping` uptime continuous, no `[PD] CC open`. The
       Black Box (`pd_trace.py dump`) shows Get_Source_Cap, Source_Capabilities, a Request with the same RDO, Accept,
       PS_RDY, and VBUS flat.
    2. (b) Fallback: a non-PD source or a source that ignores Get_Source_Cap (or Refresh in EPR mode): `[RESCAN] bridged
       cycle: Rescan marker written`, the K2 reboots once and returns on Connection -> Source with Rd and
       "Rescanned (rebooted)"; `[RDI] comeback boot: rescan pin=...` then `[RDI] comeback vouched`. Evidence into
       `runs/<date>-rescan/`.
- **A mid-run re-announce that drops one offer skips every remaining PD row** (2026-11-09, fixed in f6884c6 (re-plan in f63637d, restart rules in c6d45c9), hardware check pending).
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
  - Review of `k2/reannounce-replan` (f63637d, the rebased 1ff23a4, 2026-11-09): not merged. Gate passes; no unsafe wire behaviour in the
    branch itself. Required before merge (rebase onto `public` first):
    1. Only rows of the running unit (`unit_of(s) == unit_`) may be withdrawn or counted as placeable; today a PPS change
       during the Pd unit withdraws the PdLow rows for good (revival only looks at the running unit).
    2. Count `rebalanced`, bump `revision_` and call `plan_changed()` only when a row was withdrawn, came back or moved
       slot, never on a re-announce that changes nothing for the plan (cable discovery, Xiaomi post-auth 5 -> 6 A,
       pre-auth short list must leave the count at 0).
    3. Fix item 6 for PPS/AVS: a programmable row is still offered when the type matches, its target voltage is inside
       the new range and its current fits the new limit.
    4. **User decision 2026-11-09: a fixed offer whose current drops (15 V 3 A -> 15 V 1.5 A) keeps its row**: it runs
       at the new, lower current word and the card notes the reduced current. Only a voltage that is no longer offered
       is Withdrawn.
    5. Tests in their own commit, shown failing first; a test where a revived row actually runs and passes; `==` for the
       plan_changed count; a "between rows" case with a tick in Releasing/Baseline.
    Nits: a row whose offer only moved slot is re-queued, not Withdrawn; `rebalanced` belongs in the journal (survives a
    comeback) and the shared card; `report_card.hpp:75` says 44 values (43); commit body logdb id 409 not 401; subject
    names a pre-rebase hash; `&steps_[index_]` when `index_ == count_`.
  - Hardware runs of f63637d (the rebased 1ff23a4), 2026-11-09, splitter behind the Baseus 140 W, HID-powered K2 with
    the Rd pinned on CC2 (see `slow-rp-wake`), second device (OnePlus 15) plugged mid-run; `runs/2026-11-09-splitter/`:
    6. **A re-split during row 15 V withdrew rows that are still offered** (`A3-runlog.txt`): the list went 5/9/12/15 V
       3 A, 20 V 5 A, PPS 5 A, EPR 28 V 5 A -> 5/9/12/15 V 1.5 A, 20 V 4.1 A, EPR 28 V 4 A. The re-plan logged "0 rows
       follow, 9 withdrawn": 15 V, 20 V and EPR 28 V were Withdrawn although offered at a lower current (item 4), and the
       later EPR row ended `CapsChanged`. Expected: 15/20/28 V keep their rows at the new current; only PPS is Withdrawn.
    7. **A re-split delivered as EPR_Source_Capabilities ends the whole run** (`A6-runlog.txt`): during row 0 (5 V) the
       splitter pushed an unsolicited, complete 8-PDO EPR_Source_Capabilities with the reduced list. The runner took the
       old all-or-nothing path: row 0 Fail `CapsChanged` (5 V 1.5 A still covers the 1 A ask), rows 1..13 Skip
       `CapsChanged`, 0 pass. A complete EPR list outside the K2's own EPR entry/exit is a re-announce like any other and
       goes through the per-row re-plan; only a partial chunk or `epr_stage_ == 2` stays excluded.
    8. **`tools/k2_remote.py` cannot decode the new reason** (#42 `Withdrawn`): `ct_start.py` crashed at the summary with
       `FrameError: invalid charger-test row`. Add it to the decoder (and `card.js`/`k2_qr.py` if carried), with a test.
    9. A re-announce that changes nothing still logs "re-plan: N rows follow, 0 withdrawn, 0 back" on every run (item 2).
  - **Restart rules for a mid-run re-announce** (user decision 2026-11-09, from the runs above; they extend items 1-6 of
    the Fix and replace any all-or-nothing path left in the runner):
    1. Settle first: after any new list (Source_Capabilities, or a complete EPR_Source_Capabilities outside the K2's own
       EPR entry/exit) send no Request until the list has been unchanged for `kListSettleMs` (~1500 ms). Each list that
       arrives inside the window restarts it. The sink's single-Request gate still applies.
    2. The row in flight: if its voltage (fixed) or target (PPS/AVS range) is still offered, restart it at the new slot
       and current word; the restart is not a try (`tries` counts only source failures). Not offered: Withdrawn, then
       release to 5 V through the sink gate.
    3. Pending rows follow per row: still offered (also at a lower current, user decision above) -> keep at the new word;
       gone -> Withdrawn; offered again -> revived (pending), as long as the unit has not ended.
    4. End-of-unit sweep: before the PD unit closes, re-check the Withdrawn rows once against the current list (after a
       settle) and run the ones that came back. Withdrawn stays only for rows still not offered then.
    5. `CapsChanged` ("offers changed") only for a different charger: another wire revision, or no list back within
       `kCapsReturnMs` after a detach / Hard Reset. Never for a re-split while attached.
    6. Flapping is not a failure: no counter that gives up; keep settling and re-planning. The card and journal note
       "source rebalanced Nx" (count only re-plans that changed the plan, item 2 of the review).
    - Tests (red first, `tests/test_charger_test.cpp`): two lists 300 ms apart -> one Request, after the second settles;
      re-split during row 0 (5 V 3 A -> 1.5 A, as an EPR_Source_Capabilities) -> row 0 restarts and passes, tries=0;
      re-split during 15 V -> 15/20/28 V rows keep at the new words, PPS rows Withdrawn; list restored before the unit
      ends -> sweep runs the PPS rows; wire revision change -> `CapsChanged`.
    - Hardware: the splitter runs above (`ct_start.py --units pd`, Rd pinned on CC2 until `slow-rp-wake` lands), second
      device plugged at "Running" and unplugged later: no row ends `CapsChanged`, the card shows "rebalanced".
    Done in c6d45c9, hardware check pending: items 6-9 and the restart rules 1-6 in `src/chargertest/charger_test.cpp`
    (settle window kListSettleMs with window restart, in-flight row restarted at the new slot/word without counting a
    try, complete EPR_Source_Capabilities re-splits through the per-row re-plan with EPR rows placed on positions 8+,
    end-of-unit sweep in next(), CapsChanged only for another wire revision or no list back within kCapsReturnMs, no
    give-up counter, no-op re-announces not counted or logged); item 8's decoder entry ("withdrawn by source", #42)
    was already carried by f63637d in k2_remote.py / k2proto.js / qr_corpus.py / report_card.py / card.js, now locked
    by a test. Tests red first in 07e8965 (21 failing checks), green after.
    Not a K2 bug, for the quirks doc: on this splitter every PPS Request is Accepted, VBUS then falls 5 V -> 2 V instead
    of rising, PS_RDY is sent anyway and the Rp drops ~500 ms later (`pps-accept-then-collapse`, `A2-runlog.txt`; the
    card's "source lost" fail is correct). EPR 28 V passes through the splitter when its budget allows.
    Done in f6884c6 (+ b193e56): items 1-5 implemented in `src/chargertest/charger_test.cpp` (running-unit scoping,
    plan_moved-only counting, two-tier voltage-based find_offer, fixed current drop keeps its row), all nits
    (slot-move re-queue, rebalanced in the journal header byte 9 and card format v8, the 43-values comment, the
    logdb id 418 / post-rebase hashes in the branch commits, the guarded `&steps_[index_]`); tests red first in
    ff42512.
- **A source holding the wrong voltage on the 5 V contract fails the unit as "no reattach"** (2026-11-09, fixed in 77b8fe8,
  hardware check pending, found
  in the hardware run of a1da8c3; `runs/2026-11-09-splitter/streamB2/`, `ct-B2` output in the session log). Anker
  2-way splitter behind the Baseus 140 W, OnePlus 15 plugged into its other output during the unit detach (PPS, ~8.7 V).
  - Evidence: the unit attach got the re-split list 5/9/12/15 V 1.5 A + 20 V 2.5 A (the re-plan skipped PPS/EPR rows as
    NoOffer, correct), Requested PDO 1, Accept + PS_RDY, but VBUS read 7.55-7.60 V the whole time (149.57 s on). The
    attach waits for `safe5(vbus)` (`poll_attach`, `src/chargertest/charger_test.cpp` ~:1576), so it never settled;
    at 151.73 s the splitter cut VBUS and Rp (the K2 detached at 152.24 s, "no Rp"), and at 152.73 s
    `kAttachContractMs` (3000 ms from the caps) ran out: `fail_unit` failed all six PD rows `NoReattach`, tries=0,
    "measured 1633 mV" (a VBUS reading mid-decay). The card says "no PD after re-attach at 1.63 V", which is false: PD
    came back, the voltage was wrong.
  - Fix: tell the cases apart in `poll_attach` when the wait fails. A contract at PDO 1 whose PS_RDY was seen at any poll
    of the wait (latched, `contract_wrong_`) with VBUS outside vSafe5V is a new reason (e.g. `WrongVoltage`, "5 V contract
    held at 7.6 V") that records the highest
    VBUS seen during the wait, not the reading at the timeout; no caps at all stays `NoReattach`. Also a source-side
    drop (Rp gone) during the attach wait restarts the wait once (the splitter came back with its standby pull-up,
    `slow-rp-wake`, ~1.1-1.7 s after a steady Rd) instead of letting the timer run on. Decoder entries in
    `k2_remote.py`, `k2proto.js`, `qr_corpus.py`, `report_card.py`, `card.js` (same pattern as #42 Withdrawn).
  - Not a K2 bug, for `docs/CHARGER_QUIRKS.md`: pattern `shared-bus-split`: on this splitter, with a PPS sink on the
    other output, the K2's port carries that output's voltage (7.6 V) under a 5 V contract. The other device's PPS
    voltage sets both ports; the 5 V contract is not honoured.
  - Validate: host tests (red first) in `tests/test_charger_test.cpp`: PS_RDY at PDO 1 + VBUS 7600 mV for
    kAttachContractMs -> WrongVoltage with measured 7600; no caps -> NoReattach as before; Rp lost at 1.5 s then caps
    again at 3.2 s -> the unit runs. Hardware: the same setup, OnePlus on PPS before START: the card names the wrong
    voltage; OnePlus unplugged: the unit runs.
  - Hardware on be3f589 (2026-11-10, `runs/2026-11-09-splitter/`): (B3) OnePlus on PPS before START: START's own
    preflight refuses with "VBUS not safe 5V" on screen (VBUS 7.7 V under the PDO 1 contract), so the new attach-wait
    path is not reachable that way; the refusal reason was on screen only (no `[TEST]` log line, `ct_start.py` printed
    just "run id still 0"). Fixed in f074898, hardware check pending: the refusal is now one
    `[TEST] preflight refused: <reason> (<VBUS> mV)` log line (src/chargertest, logdb id 419) and `ct_start.py`
    prints it when the run id stays 0. (B4) OnePlus
    plugged around the unit detach: the attach got the split list at a clean 5 V, 5 pass / 4 skip (EPR, SPR AVS, PPS,
    EPR AVS not offered), card correct. WrongVoltage itself stays covered by the streamB2 replay test only; the splitter
    did not repeat the 7.6 V attach.
  - (B5, 05b1703, 2026-11-10, `runs/2026-11-10-splitter-wrongv/`) Same as B3: OnePlus charging on the other output,
    START woke the port, PS_RDY at PDO 1 with VBUS 8.47 V, START went back to Review with no log line ("START did not
    start a run"); the Baseus then cut the splitter (its load balancing), the second START refused "VBUS dropped to
    1.05 V after the PD rescan". The inline latch also fired once at VBUS 0.97 V with no current and no far Request
    while the port was dead (cleared later). So the live WrongVoltage path is unreachable from START: the preflight
    owns this case. Fix (folds into the preflight-reason item): the preflight refusal says "VBUS 8.5 V on a 5 V
    contract (shared bus)" in the log, on screen, and in `ct_start.py`, and the card records WrongVoltage with the
    measured value when a START is refused for it.
  - Verified on 05b1703 (2026-11-10, CUKTECH C3 -> splitter, `runs/2026-11-10-splitter-cuk/`): a re-split **while a
    row runs** (OnePlus plugged after row 0): re-announce with 4 PDOs (PPS gone, 15 V 1 A), 7 PPS rows Withdrawn, a
    Request 1.9 ms after the new caps, row 3 ran 15 V at 1000 mA and passed; no unrequested level. `slow-rp-wake`
    Rd-seek: a plain `pd-reread` on the sleeping port (CC2 2995 mV) held Rd on CC2 alone and got caps after 1648 ms.
    No shared-bus voltage on the CUKTECH (9.10/12.13/15.07 V with the phone charging), and PPS at no load passed
    through the splitter there and on the Baseus direct (run 4/5): `shared-bus-split` and the PPS collapse need the
    Baseus 140 W top port behind the splitter. The Baseus balances by cutting power to its ports 2-3 times.
- **PPS collapses on the splitter at no load (`pps-off-at-no-load`, suspected)** (2026-11-10, step 1 fixed in e38c42c
  (`pd request --over-load`, lab-only, passive-far-sink gate), k2-review fixes in 2a010ae (far-sink control messages
  count as a far PD message, /api/frame strips the over-load flag, the far-sink CC reading cleared on attach/detach),
  hardware check pending; `runs/2026-11-10-splitter-pps/`,
  `runs/2026-11-10-op15-sniff/`). Anker 2-way splitter behind the Baseus 140 W,
  list 5/9/12/15 V 3 A, 20 V 5 A, PPS 3.3-11 V 5 A, EPR 28 V 5 A.
  - Evidence: the charger test (run 9, be3f589) passes every fixed row and EPR 28 V, and fails all five PPS rows "source
    lost" (~1.8 V). On the wire: Request PDO 6 PPS -> Accept -> VBUS starts falling ~46 ms later, ~2 V within ~250 ms
    at 0 mA, no PS_RDY, port restarts ~2.3 s later (`bb.txt`). A OnePlus 15 on the same port (sniffed inline) charges
    on PPS: 5.5 V 2 A, 7.5, 8.0, 8.2 V, then +40 mV steps up to 9.24 V 3.5 A, every step Accept + PS_RDY.
  - Ruled out on hardware with manual `pd request 6 --mv ... --ma ... --confirm` (all collapse the same way): PPS at
    5.0 V 1 A (no voltage change), 7.2 V 3 A, 5.5 V 2 A (the OnePlus's exact first request); 5.5 V 2 A from a
    throwaway build with bit 22 (EPR Mode Capable) cleared, so the RDO matched the OnePlus's bit for bit
    (`bb-noepr.txt`); after a Discover_Identity ACK (Anker 0x291A, as the phone got); from a 5 V 3 A contract
    (`identity-first.txt`, `pdo1-3a.txt`). Left: the phone draws current, the K2 draws 0 mA.
  - Not testable today: every load on the K2's top port is a sink the inline guard sees (a 5-15 V PD device sends
    Get_Source_Cap; an Xbox controller and a 390 mA USB light show sink-level CC), so `pd request` refuses "inline: a
    live sink is attached" - correct behaviour. The PA8 lab load pulses only up to 200 ms and its guard refuses once
    VBUS sags (`reason=vbus_low` at 3.38 V ~100 ms after the request).
  - Step 1 (diagnosis, implemented in e38c42c, host-tested, hardware check pending): a lab-only
    `k2_remote.py pd request N ... --over-load` that lets the K2 request while a far
    sink is attached, only when that sink has sent no PD message this attach (a passive load: Rd only, no Request,
    no Get_Source_Cap); refused otherwise and never reachable from the UI or the charger test. The gate derives
    from the existing foreign-evidence tracking (`pd::over_load_allowed` over `fusb302::over_load_facts()`: inline
    latch / sink Rd on CC / foreign Request vs. a Request we did not send and the sink-role records the engine
    filed). Hardware: splitter ->
    K2 -> the USB light (Rd on both CC, ~390 mA at 5 V), `pd-reread --pin 2 --no-run`, then
    `pd request 6 --mv 5500 --ma 2000 --confirm --over-load`: holds -> no-load is the trigger; collapses -> look at
    what else the phone does (its Request timing is ~1.5 s after PS_RDY, the K2's is longer).
  - Step 2 (done ahead of step 1 on a user decision, 2026-11-10, 96a798d; hardware check pending): the charger test
    records a PPS row with the signature "Accept, VBUS falls at ~0 mA, no PS_RDY" as a skip "needs load" (`Reason::NeedsLoad`,
    #44, pattern `pps-off-at-no-load`), not a fail; catalogue entry in `docs/CHARGER_QUIRKS.md` (behaviour, no VID:PID).
    Rule: programmable row, Accepted, no PS_RDY, VBUS under 85 % of the level, current under 50 mA; fixed rows, a seen
    PS_RDY, current drawn, Hard Reset and Reject keep their reasons; no chain skip. Host-tested. Still open: the
    cause is unproven until step 1 (a load decides), and the same signature on another source would read "needs load"
    too, so step 1 may narrow the rule.
  - Validate: host tests (red first) for the `--over-load` gate (refused after any far PD message, refused with no
    far sink) and the signature rule; the hardware step above.
- ~~**The splitter's sleeping port still needs the Rd pin outside a charger-test START**~~ completed: opt-in Settings > Wake sleeping port (f0d79fb, c3f46d1), woke the sleeping splitter port on hardware 14f9d24 (IQ4 -> splitter, Rp answered 3.86 s after the Rd, 6 PDOs). Still open: the boot scan can latch the CC2 bias net first (wake ~3 s late); START's VBUS wait vs the 1.1-1.7 s wake and the "Source asleep" idle hint are batch 3 (k2/start-wake-wait) - fixed in 53da186, hardware check pending: ct_start.py's preflight waits VBUS_WAIT_S (5 s) for a dark port's VBUS before START presents Rd and names a standby-signature open line ("source looks asleep") when it gives up; the idle Source tab / bar say "Source asleep: START or Refresh wakes it" when an open line reads 2.8-3.15 V with VBUS dark and Wake sleeping port off. Host-tested (tests/test_ct_start.py, tests/test_ui_connection.cpp, tests/test_rdseek.cpp), red first in 8dba9c8.
- ~~**A leftover Rd pin makes an inline K2 Request at attach**~~ completed: b60c702 + ce6bbdb + df8e004; Home "Rd pin CC2" badge and `pd status` pin verified on c700284.
- ~~**`k2_remote.py cards` cannot read format v8 cards**~~ completed: 26f297f; `cards list` / `cards export` read the v9 cards on hardware 2026-11-10.
- **An inline latch leaves the far sink on Type-C current: hand the contract back with a Hard Reset** (2026-11-10,
  connection matrix P4, 05b1703, `runs/2026-11-10-connection/P4/`): PLUS-boot K2 on a Baseus 140 W, self-hold PDO 1
  Ready at 1291 ms; OnePlus 15 plugged on the bottom port at ~19.0 s; the inline latch fired ~300 ms after IBUS passed
  50 mA and dropped Rd in the same write (`SWITCHES0 <= 0x08`, `LINK inline=yes`), then no TX for 40 s. Correct, but
  the Baseus keeps its contract with the K2 and never re-sends Source_Capabilities, so the phone sat at 5 V ~2 A with no
  PD contract of its own. Pulling the top plug for ~1 s (the stand-in for a Hard Reset, `runs/2026-11-10-connection/
  P4-handback/`) proved the recovery: the K2 came back on a plain boot with `pin=0,0,0,0`, no marker (the follower had
  removed `/rd.rec` on the inline latch), no K2 TX, and the phone negotiated PPS itself (Requests every ~500 ms, 8.97 V
  3.37 A, 30 W).
  - Fix: when the inline latch fires while the K2 owns an explicit contract (self-hold, a request, a charger-test row),
    (1) wait until the comeback record is removed from the volume, (2) drop Rd, (3) send one Hard Reset (TX needs no
    Rd). A self-powered K2 dies in the VBUS gap and returns as a passive plain boot; a host-powered K2 rides through and
    stays passive. Never if the K2 holds no contract (nothing to hand back) or the far end already has its own (a far
    Request seen). One hand-back per attach. Order is the whole point: a reset before the record is gone brings the K2
    back with Rd from the first write and it fights the phone (X1 in `docs/CONNECTION_MATRIX.md`).
  - Caveat (2026-11-10, the Identify capture below): after a Hard Reset the OnePlus 15 came back on **PDO 1, 5 V**
    and stayed there for minutes, twice; the P4 stand-in was a full unplug (VBUS and CC gone), after which it chose
    PPS 9 V. So a Hard Reset hand-back may leave the phone at 5 V, which is no better than today. Step 0 before any
    code: with the lab reset, hand back once and watch the phone for 60 s; if it stays at 5 V, the hand-back needs a
    real detach (only the user can do that: show "replug the phone for fast charge" instead of resetting).
  - Lab: `k2_remote.py pd reset --inline --confirm` sends the same Hard Reset while inline (today `pd reset` refuses
    "inline: a live sink is attached" with no override), for the hardware check and for diagnosing the far sink's
    renegotiation. Same lab gating as `--over-load`: refused during a charger test, logged as a lab op.
    Lab part done in 988bc65 (`pd reset --inline --confirm`, flag byte on CMD_PD_RESET, `pd::lab_reset_verdict` gate:
    refused while a charger test runs, both flags required, logged `[RMT] pd hard reset lab inline flags=0x%02X`,
    logdb 420; host-tested `tests/test_pd_passive.cpp`, `tests/test_remote_proto.cpp`, `tests/test_k2_remote.py`),
    Hardware check done 2026-11-10 after e12dcee (the driver had dropped it): Pixel inline on the Anker IQ4, VBUS 9.09 ->
    5.37 V for ~1.5 s, then the Pixel re-requested 9 V by itself. Step 0 answer: phone-dependent (Pixel recovers,
    OnePlus 15 stayed at 5 V). Operator decision 2026-11-10: build the hand-back anyway, in two phases.
  - Phase 1, lab only (batch 3): `k2_remote.py pd handback --confirm` runs the whole Fix sequence above - record
    removed (wait for it), Rd dropped, one Hard Reset - only when the decision allows it (contract owned + inline, no far
    Request seen, first hand-back this attach); otherwise it refuses with the reason. Logs each step and, 60 s later,
    one `[LINK] handback result` line: the far sink's highest Request voltage seen (or "no Request: Type-C current").
    `pd reset --inline --confirm` stays the raw single reset. Fixed in c347bb3, hardware check pending: Cmd::PdHandback
    0x49 with the pure decision `pd::handback_verdict` (verdict on the wire and in the log, logdb 460-466), the ordered
    sequence in k2/core/handback (record gone before the reset is queued, 10 s record-wait timeout aborts without a
    reset, one reset via `fusb302::sink_hard_reset(lab_inline=true)`, one result line 60 s later); host-tested
    (truth table, step order, dispatch, driver-level reset-on-the-chip, CLI). Step 0 hardware check (hand back once,
    watch the phone 60 s) still owed to the k2-hw pass.
  - Phase 2, tester UI (after phase 1 is verified on hardware): when the decision fires on its own, a notice "Hand
    control back to the charger?" with a countdown; OK = hand back now, Back = keep as is (the far sink stays on Type-C
    current, the notice offers "Replug the phone for fast charge"). Timeout = hand back (operator decision
    2026-11-10: the devices get control back unless the user presses Back; the reset cuts the far sink's power for
    ~1.5 s, so the countdown is visible on screen). One notice per attach; never during a charger-test run.
  - Validate: host tests (red first) on the decision: contract owned + inline -> record removed before the reset is
    queued; no contract / far Request seen / second latch -> no reset. Hardware: repeat P4 HID-less, the black box of the
    comeback shows the phone's own Request above 5 V and no K2 TX; repeat with HID, the K2 stays up and passive.
- **Home footer shows "PD 0V5.82A" for a sniffed PPS contract when the K2 missed the caps** (2026-11-10,
  `runs/2026-11-10-connection/P4-handback/home.png`, 05b1703): fixed in 27255b3 (red tests in 39ca0e4), hardware check
  pending. The K2 booted ~1 s after VBUS returned, so the Baseus's
  Source_Capabilities came before the trace started (`num_pdos: 0`); the phone's Requests (PPS, every ~500 ms) were
  decoded with no PDO list, as a fixed RDO: no voltage (0 V) and the PPS bits read as max current (5.82 A, also on the
  IBUS "max" label). `src/ui/home_proto.cpp` `derive_proto` takes the `ctr.valid && pd_link` branch into `from_pd`.
  Seen again in H6 (Anker IQ4 100 W -> K2 -> OnePlus 15, plain HID boot, `runs/2026-11-10-connection/H6/`): "PD
  0V5.62A" at 9.09 V 2.45 A, and here the black box **did** sniff the Source_Capabilities (89508 ms) while
  `state` still says `num_pdos: 0`. So missing the caps at boot is not the whole cause: sniffed caps on an inline
  link are not stored where the decoder looks. Fix both: store the sniffed caps while inline, and never decode a
  Request without them.
  - Fix: a contract decoded without its Source_Capabilities has an unknown PDO type: show the measured level and no
    current ("PD 9V", tag only), never a value decoded from a guessed RDO layout. A Request repeated every <=10 s with
    no caps is the PPS keep-alive pattern: label it "PPS 9V" from the measured VBUS. `fc: PD MTK` in `state` for the
    same link is likely the same misread; check.
  - Validate: host test on `derive_proto` with a contract and no caps -> no current, measured voltage; with a periodic
    Request -> PPS tag. Hardware: repeat the P4-handback replug, footer reads "PPS 9V" or "PD 9V".
  - Hardware be71f10 (Anker IQ4 -> K2 -> Pixel, fixed 9 V, K2 rebooted mid-contract, `runs/2026-11-10-merge2-hw/
    pps-footer*.png`, `pps-sniff`): footer "CC 9V", no current, no "max" label - correct. `fc: PD MTK` fixed in
    138eebf: a settled fixed contract is silent (no PD frame in 14 s), so no Request is ever decoded and pd_active()
    stays false; the D+/D- classifier now applies the rule below - CC attached, VBUS above vSafe5V (> 5.5 V) and
    no D+/D- handshake seen this attach = "PD (not decoded)", never a legacy mode (host tests in
    tests/test_fastcharge_classify.cpp; the passive CC2 bias-net level ~1.69 V is deliberately not an input).
    Hardware check pending. The own-contract footer rule (row's last bullet) is fixed in 9106849 (red tests in
    94164a6), hardware check pending: when the K2's own sink holds the contract (SinkStatus ready/held, pos and
    PDO known), derive_proto uses that contract - AVS pos -> "AVS 12.6V", PPS -> "PPS 9V", fixed -> "PD 9V 3A"; the
    sniffed contract only for a far sink. The "PPS 9V" label is still
    unverified on hardware: needs a PPS sink (OnePlus 15 on the CUKTECH; it takes only PDO1 5 V on the Anker IQ4,
    with or without the K2).
- ~~**Sink Identify inline drops the phone from 9 V to 5 V**~~ completed: 6df54fa; inline Identify refused with and without --confirm, VBUS held 9.076 V, Sink tab "sniff 18D1:4EE1" (Pixel, be71f10). Fix item (2), active Identify of a phone alone on the K2, fixed in 7124847, hardware check pending:
  - Decided from the link state, not a confirm (`pd::identify_alone`, k2/pd/pd_link.hpp): alone = not inline, VBUS dark (known reading), no source caps this attach, and the link's waiting verdict is DualRolePort (an Rp toggling ~every 100 ms - the phone). A steady-Rp charger alone is ChargerWaiting there, so Identify stays NoDevice/refused as before; inline stays refused (batch 2, unchanged).
  - CC sequence chosen: plain boot with the phone alone the K2 is passive (no Rd, no Rp); Identify is offered on the Sink tab -> the driver presents Rd on both CC lines while no CC is known (then the active line), never Rp, so the K2 cannot become the phone's power source -> the phone settles as a source and drives 5 V, its Source_Capabilities arrive and the attach completes -> the Identify sequencer runs as its sink partner (Discover Identity, SVIDs, Modes, Sink caps, Status, Revision) with Auto-CRC on and Rd held on the active line -> when it ends the presented Rd is released (the phone sees a detach and stops sourcing; VBUS dark again).
  - Validate (host, done): test_pd_link predicate matrix (alone/inline/VBUS-up/caps/ChargerWaiting/Unknown), test_fusb302_scan alone-starts-and-presents-Rd-only, charger-alone refused as today, inline refused; test_ui_connection_page offers Identify when alone. Hardware: phone alone on the bottom port (HID-powered K2), Sink tab -> Identify shows the phone's VID/PID; the black box shows Rd presented (SWITCHES0 PDWN, never an Rp of ours), the Discover Identity answered, Rd released afterwards, VBUS at 5 V only while Identify runs; a charger alone still NoDevice/refused.
  - Also wrong when the K2 itself is the sink (2026-11-10, d6c90e7, UGREEN Nexode Air 65 W, `pd request 7` = SPR AVS
    9-20 V during the AVS sweep, `runs/2026-11-10-merge2-hw/avs-footer.png`): the footer reads "PD 13V" (earlier
    "PD 10-11V") for an AVS contract at 12.6 V. The footer takes the sniffed contract (fusb302::contract(), Requests
    heard on the wire); the K2 never hears its own TX, so it falls back to the capless "PD <measured>" text. Fix: when
    the K2's own sink holds the contract (SinkStatus ready/held, pos and PDO known), derive_proto uses that contract:
    AVS pos -> "AVS 12.6V", PPS -> "PPS 9V", fixed -> "PD 9V 3A"; the sniffed contract only for a far sink. Host test on
    derive_proto with an own AVS / PPS / fixed contract.
  - Row A rework (handoff 2026-11-10d, runs/2026-11-10-idalone-op15): fixed in 18be8bc, hardware check pending. The dual-role
    toggle classifier moved into the link layer (DrpTrace in ra_guard.hpp: >= 2 rising and >= 2 falling edges in 2.5 s,
    high >= 1200 mV / low <= 400 mV; a steady CC2 ~1.69 V bias or a steady Rp never reads DualRolePort); an alone-Identify
    presents Rd and waits for a Ready contract of this attach settled >= 200 ms (plain-stamp ready_at, no ready_at|1 wrap)
    with a bounded 4 s timeout before the first VDM; the line is handed back on every exit; the result stays on the Sink
    tab / identify-device until NoSource or ChargerWaiting for >= 2 s. Host: test_pd_link DrpTrace cases,
    test_fusb302_scan wait/timeout/release/stale-CC cases (red in 59af715, green in 18be8bc).
- **Charger test: "Cable OFF" says nothing about what it does** (2026-11-10, operator report): fixed in 2030c3b, hardware check pending. The wizard's step 2
  read "Cable" with the value "OFF", which said nothing about what it does (the e-marker identity the K2 presents to the
  charger, the same list as Tools -> Spoof). Wording only, nothing else in the flow changed (the Cable step still shows
  every time, remembered pick or not):
  - Step 2 title "Spoof cable"; its first row "Off (real cable)"; the built-ins and clones keep their names and ratings.
    Review row `Spoof  Off` / `Spoof  EPR 50V 5A`.
  - The guard's Refuse stage ("Cable on plug": a real cable between the charger and the K2) says why: "Cable connected:
    the spoof won't reach the charger." Two options: `Start anyway` and `Start, skip spoof` (this run without the
    identity; the remembered pick stays). `Cancel` goes: Back already returns to Review.
  - Docs (USER_GUIDE, CHARGER_TEST_FLOW "Wizard steps" and the Refuse stage, UI_DESIGN), `ct_start.py --cable` help and
    the remote/web stage names use the same words.
  - Validate: host tests on the step title, the first row, the Review row, the Refuse text and its two options, and
    that "Start, skip spoof" leaves the stored pick unchanged; hardware screenshots of step 2, Review and Refuse.
- **Inline Identify refusal answers "busy"** (2026-11-10, be71f10, CUKTECH C1 -> K2 -> Pixel inline): fixed in 29aae2f
  (red tests in 76d6a25), hardware check pending. Since 6df54fa an
  inline K2 refuses Identify (correct, no TX, VBUS held 9.076 V), but `k2_remote.py identify-device [--confirm]` prints
  "command 64 failed: busy": `Ui::IdResult::Refused` maps to `remote::Status::Busy` (src/core/remote.cpp
  identify_device). Give the inline refusal its own result (`IdResult::Inline` -> the pd::Err::Inline status the other
  inline refusals use) so the tool says "refused: inline - the Sink tab shows the sniffed identity"; a running charger
  test stays "busy". Host tests on the mapping and the k2_remote text.
- **A `pd-reread` leaves the contract held: START refuses "tool busy"** (2026-11-10, b14c1b9, UGREEN Nexode Air 65 W
  on the top plug, `runs/2026-11-10-merge2-hw/ugreen-pd.txt`; fixed in 3b3fb2c, hardware check pending): after a plain
  `k2_remote.py pd-reread` (reread done,
  7 PDOs) `pd status` reads `held=True pdo=1 5 V`, and `ct_start.py` START is refused "[TEST] preflight refused: tool
  busy (5100 mV)" - twice, also after `pd-reread --abort` (the run had already ended, so abort had nothing to release).
  Only `pd release` cleared `held`. Fix: a re-read that ends leaves no held contract behind (or START treats a re-read's
  own leftover PDO 1 hold as its to take over, like the self-hold); the refusal says which tool holds it ("re-read
  holds 5 V: pd release"). Host test: re-read done -> START allowed.
  Done in 3b3fb2c: a finished re-read's own leftover PDO 1 5 V hold (its linger, `pd::reread_own_hold`) is taken over
  like the self-hold - `Ui::busy_reason()` lets START through and the Ra arming treats it as self_hold, so both the
  preflight and `ct_start.py --cable ...` proceed; a user trigger's hold still blocks. Every START refusal now prints
  its reason: the busy preflight line names the holding tool, the cable-arm refusal is logged, and ct_start.py prints
  any `[TEST] ... refused` log line with the run id.
  Again on 72c2e28 (CUKTECH C1, top plug): after `pd-reread` + `pd release` (`pd status`: ready, held=False, pdo=1)
  `ct_start.py --cable builtin:cuktech-6a` ended "START did not start a run" with NO reason printed; a K2 reboot cleared
  it and the run passed 21 / 0 / 3. So a re-read's leftover contract blocks START even when `held` reads False, and the
  refusal can be silent - every START refusal must print its reason (B3 rule).
  Done in 3b3fb2c: the leftover Ready contract no longer refuses the Ra arming (emkra self_hold), and the arm refusal
  is logged ("[TEST] cable arm refused: %s") and printed by ct_start.py.
- **After that release the whole PD run failed "no PD after re-attach" at 5.10 V** (2026-11-10, same rig, right after
  re-read -> abort -> `pd release` -> `pd-reread --pin 0 --no-run`): 0 pass / 25 fail, VBUS 5.10 V the whole run, every
  row "no PD after re-attach" - the source never saw the K2's planned re-attach (VBUS never dropped). Not yet known
  whether the lab sequence left the K2 in a bad state or the charger does not re-announce: a clean replug run decides
  it (next step). The step-walk rows read "unknown (160)": the walk's Pending detail (kDetailStepWalk) printed as a
  sink result by k2_remote when the row never ran - print it as "not run".
- **The PPS step walk fails a good source: its base level is read before the output settles** (2026-11-10, aebb430,
  UGREEN Nexode Air 65 W, `runs/2026-11-10-merge2-hw/ugreen-pd3.txt`, `ugreen-walk.tsv`, fixed in 9cbd472, hardware
  check pending): the charger test's walk on
  PPS 5-21 V failed "steps ignored" (22 pass / 1 fail). A manual walk with 2 s per step shows the source follows
  every +20 mV step on both APDOs (+17..+21 mV), except the first step after the jump to the 13.0 V base: base read
  13007 mV (still settling down), first step 13009 mV (+2). The walk takes the base sample as soon as the base level
  is granted; a source that overshoots and settles makes the first move read ~0 or negative. Fix: the walk's base (and
  each step) waits until VBUS is steady (e.g. 3 samples within 3 mV, bounded by a timeout) before it records the level;
  the sum rule stays. Host test with this shape (base 13007 settling to 12990, then +20 mV steps) must pass. Done:
  walk_step records a granted level only once VBUS is steady (WalkSteady: 3 samples within 3 mV inside a 300 ms
  window, bounded by a 2 s timeout per level), sum rule unchanged; host test with the row's shape passes.
- ~~**Wake sleeping port ON breaks the charger test's unit re-attach**~~ completed: 1b0c62f (run-owner flag in the wake gate, Wake OFF releases the wake's Rd); UGREEN with Wake ON 23 pass / 0 fail and Wake OFF dropped VBUS to 0 on hardware 72c2e28. Open: the boot scan can still latch a dark CC2 first (released after 3 s).
- **Crossed CC through the K2: tell the user to flip a cable** (2026-11-10, fixed in 7d1ae17, hardware check pending, connection matrix H22, be71f10, `runs/2026-11-10-merge2-hw/`
  dead-chain, attach-noreact.png): Anker IQ4 -> C-C cable -> K2 -> C-C cable -> Pixel, VBUS never came up, the K2 sat at
  "No charger". The K2 passes CC1->CC1 and CC2->CC2, and each cable's orientation picks which pin its CC wire lands on,
  so the charger's CC and the phone's CC can end up on different pins and never meet. Evidence: Anker alone = steady
  3.30 V Rp on CC2; Anker + Pixel = CC2 steady 500 mV (the Anker's Rp loaded only by the Pixel cable's e-marker Ra,
  not a sink, so the Anker never enables VBUS) while CC1 cycled 0 / 1.2 / 1.8 / 3.3 V (the Pixel toggling alone).
  Flipping the Pixel's cable at the K2 fixed it at once; it had worked three times before by orientation luck.
  NOT REPRODUCED (2026-11-10 afternoon, 72c2e28): IQ4 direct on the K2 plug + Pixel on its cable in the K2 receptacle,
  IQ4 -> cable -> K2 -> Pixel direct, and IQ4 -> Anker splitter -> K2 plug + Pixel on its cable (19.7 V): every
  cable orientation charges (~9.4 V / 19.7 V) (the Pixel attaches on CC1 or CC2;
  the other line reads its cable's Ra, 0.5-0.6 V), the detector stays silent (correct). So the "CC lands on the other
  pin" explanation is unproven: the morning failure may need another cable, the IQ4's other port or another port
  state. Re-test with the morning's exact cable and port before trusting the cause; the detector is harmless meanwhile.
  - Detect, passively (no Rd, no Rp, no TX), with VBUS dark (< vSafe0V) for >= 1 s, from the per-tick ADC levels the
    waiting classifier already samples (raguard::RpTrace, pd::Link):
    1. Rp-vs-Ra on one line (Rp loaded only by a cable's Ra: steady 0.2-0.8 V, below the sink band) AND a dual-role
       toggle (RpTrace DualRolePort) on the other line = crossed (the case above).
    2. Steady charger Rp on one line AND a dual-role toggle on the other line = crossed (non-e-marked cable).
    3. Rp-vs-Ra on one line and nothing on the other = a cable with no device or a plain sink on the other pin:
       a plain sink's Rd is invisible without a pull-up, so this one says "no device seen; if one is plugged in,
       flip a cable" (a hint, not a verdict).
    Never when inline, a contract exists, the K2 holds Rd/Ra, or Wake sleeping port holds its line.
  - Show: Source tab / bottom bar "CC CROSSED - flip one cable" (case 1/2), "Flip a cable?" hint (case 3); Home
    footer the same; `state` gets a link flag (kLinkCrossed) and k2_remote prints it. Log one [LINK] line per edge.
  - Optional later, lab-gated: confirm case 3 with a < 5 ms FUSB302 pull-up measure on the silent pin (below
    tCCDebounce, so no sink attaches); only if the passive cases prove too weak on hardware.
  - Validation: host tests with recorded traces (this rig's levels; a non-e-marked cable; a charger alone must NOT
    read crossed; a DRP phone alone must NOT read crossed); hardware: Anker -> K2 -> Pixel both orientations of each
    cable (4 combos), the crossed ones show the message within 2 s, the working ones never do.
  - Implemented (7d1ae17): `raguard::CrossTrace` (`include/k2/pd/cc_crossed.hpp`), per-line statistics over 1.6 s,
    VBUS < 1 V for >= 1 s, steady Rp threshold 2.4 V; gated off while a line is held (`LinkInput.line_held`: Wake
    sleeping port or a boot pin). Home Idle row reads "CROSSED: flip one" (17-char field). Open: a DRP phone alone
    through an e-marked cable must not read crossed (needs a charger's pull-up for the 0.2-0.8 V line) - check it.
- ~~**A dual-role phone alone reads as "CHARGER WAITING"**~~ completed: 9189def; Source tab + bar read "Dual-role port" (Pixel alone) and "Charger waiting" (CUKTECH, splitter) on hardware 2026-11-10.
- **The sink re-requests a held contract by position on a re-announce** (2026-11-09, fixed in 1d45a76, hardware check
  pending). `src/pd/pd_sink.cpp` (Source_Capabilities while held, ~:717-725) re-requested `want_.pos`, and
  `plan_request` (~:128-139) ignores the held voltage for a fixed PDO. A source that re-announces 5/9/20 V while the
  K2 holds 15 V at PDO 3 makes the sink ask 20 V by itself: a legal Request (advertised) but a voltage nobody chose,
  with the cable limit unchecked. Also a Source_Capabilities that arrives in `Transition` while held, with no release
  pending, gets no Request (the source then Hard Resets after tSenderResponse).
  - Fix: on a re-announce while held, look the held level up by voltage (fixed) or range (programmable), not position:
    found -> Request it at its new slot and current word; not found -> Request PDO 1 (5 V) and report the hold lost to
    the owner (charger test: Withdrawn per the re-plan row above). In `Transition`, answer by the PD rule (the source
    restarts negotiation: treat it as a fresh list).
  - Validate: host tests in `tests/test_pd_sink*.cpp` (red first): 15 V held at PDO 3, re-announce 5/9/20 V -> Request
    PDO 1, never 20 V; re-announce 5/15/20 V -> Request 15 V at slot 2; re-announce in Transition -> a Request within
    tSenderResponse; `make gate`. Hardware: the splitter re-plan run above shows no unrequested level in the black box.
- ~~**Rd seek leaves before a slow source wakes (`slow-rp-wake`)**~~ completed: 5926abb; START and a plain `pd-reread` hold Rd on one line and wake the splitter (CUKTECH C3 and IQ4, 2026-11-10).
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
  - Status: fixed in b9119fc, hardware check pending. The selection squares were drawn with plain fills, which go
    through the ContentViewport mapping; in a landscape rotation the 240->224 y squeeze can map a 1 px border line to
    zero physical rows, and which rows lose their top/bottom line moved as the list scrolled (the open row's focus
    expansion shifts the rows below it). The marker square is now drawn at its native size clipped to the viewport
    (`gfx_native_rect`/`gfx_fill_panel`, like glyph boxes), so all four frame edges survive in every rotation;
    ListView already repaints every visible row on a scroll step. Host render test `test_ui_ct_list_frame` scrolls the
    Cable-step list one row at a time to the end and back in all four rotations and pixel-checks every visible row's
    frame has all four edges (red before: 7942 wrong edge pixels in the landscape rotations, green after).
  - Validate (host, done in b9119fc): `tests/test_ui_ct_list_frame.cpp`; hardware: scroll the spoof list on the K2 end
    to end with `k2_remote press` and `k2_remote screenshot` each step; every frame closed, in a landscape rotation
    (1 or 3) where the host render reproduced it. Review 2026-11-09: portrait already rendered whole frames at the
    base, so a gap seen in portrait is a different cause and reopens this row. Follow-ups: a landscape pixel pass over
    the Tests step's Box3/Check squares; clamp the marker sub-fills to the clipped square (`kit.cpp` fill lambda).
- **Home screen fonts stretched or squeezed in landscape** (2026-11-09, user report; fixed in 1ab56e3, hardware check
  pending): on the Home page in a landscape rotation the glyphs looked stretched or squeezed compared with portrait.
  Cause: in rotations 1/3 the ContentViewport maps the 224x240 logical content onto the 240x224 panel box with a
  non-integer, non-square stretch (x 240/224, y 224/240); `home::paint` streamed the composed glyph rows through that
  stretch, while `gfx_text` already escapes it (native_rect origin, native size, Pause). The LCD MADCTL / address window
  in `src/drivers/lcd.cpp` is not at fault (no width/height swap).
  - Fix (done in 1ab56e3, `src/ui/home_cell.cpp`): when the viewport is stretched, paint() splits — fills, sweep,
    arrows and the warning keep the stretch; each Text item is re-streamed at its native pixel size (full glyph box
    mapped once via `ContentViewport::native_rect`, clip offset added in native pixels so partial hero-digit repaints
    stay on the same advance grid, streamed under `ContentViewport::Pause`, blended against the same text-less row).
    Glyphs now render at the same pixel size in every rotation.
  - Validate (host, done in 1ab56e3): `tests/test_ui_glyph_rotation.cpp` renders the same string ("20.04", hero font)
    through the home-cell glyph path in all four rotations and compares the tight glyph bitmaps (equal in every
    rotation, hence equal after rotating by 90 degrees), with `gfx_text` as a control, and compares the real Home
    page's hero ink mask on the native-mapped box across rotations. Red on ff426f8 (home-cell bitmaps 165x33 vs 154x36
    in rotations 1/3; hero mask 1837/6240 pixels off), green after. Hardware, pending: screenshots of Home in all
    four rotations, text the same size and shape.
  - Side effect: the fix shrank `home::paint`'s frame by 80 B, moving the worst main-loop path onto the ChargerTest
    chain; ff6c833 adds the matching RESOLVE rule (ResultGrid's `result_row_fn` callback) to `tools/stack_depth.py`
    so the stack gate resolves it again.
- **Home landscape: the third rule is cut and the info bars are empty** (2026-11-10, user report; branch
  `k2/home-landscape-fill`, hardware check pending): in rotations 1/3 parts of the Home lower half were overwritten by
  black fields; the info row 1 showed a dash in every state but a live session with a known USB link.
  - Cause: the W / Wh / Ah band and the info rows were `Field`s (`gfx_text`/`Field::draw`, `src/ui/gfx.cpp`) that fill
    their whole native glyph box with the background. In landscape the y squeeze (224/240) makes that box taller than
    the logical row, so the box overran the third rule (y194) and the divider; `draw_band_rules()` redrew only the
    divider and the split line. Red: 72 of 240 px of rule3 not `kRule` in rotations 1 and 3, in every state.
  - Fix (`src/ui/ui_home.cpp`): the band and the info block are one `home::Cell` (y144..239) with the rule, divider and
    split line as fill items and every text as an item, repainted per zone (W, Wh, Ah, row 1, elapsed, row 2) with a
    signature each; text composes over the structure and the written region is clipped to the cell, so no repaint can
    cut a frame line in any rotation. The ten `Field`s and their colour bookkeeping are gone (bss unchanged: the page
    arena is sized by the largest page); text +340 B against the base.
  - Fill: info row 1 shows `CC1/2 1.62/0.07V` outside a live session and `D+/D- 0.60/0.58V` while live with an unknown
    USB link (`home::format_lines`, pure, tested); a known link still shows `USB <link>`. Row 2 stays the contract.
  - Validate (host): `tests/test_ui_home_landscape.cpp` (frame whole in four rotations x five states, width changes
    9.999 -> 10.00 V and 99.9 -> 100.0 W, badge/protocol text changes with incremental == `resync()`, row 1 carries
    data); red on the base, green after. Renders in `runs/ui-homefill/{before,after}`. Hardware, pending: Home in all
    four rotations, no black patch, CC1/2 or D+/D- on the first info row.
- **Planned re-attach counts as a resume** (2026-11-09, fixed in 49dacbb, hardware check pending): the card's "resumed"
  counter was not masked after the planned reboot, so a clean 6 A run read as resumed once.
  - Fix: the planned re-attach writes its intent marker (`/rd.rec`, `src/core/rd_comeback.cpp`); the run journal's
    resume path reads it (`rdcomeback::planned_reattach()` -> `Runner::restore()`'s last argument) and does not count
    that resume (a separate `reattach_` counter; the card carries the planned re-attach as cable flags bit 7
    `kCableReattach`, "re-attach" in the footer, `"reattach":1` in the JSON).
  - Validate: host test (journal resume after a planned re-attach: resumed stays 0, after an unplanned VBUS loss: 1);
    hardware: self-powered CUKTECH 6 A run (PLUS boot, built-in CUKTECH 6A identity) ends with a card showing no resume.
- **Stale Rd marker** (2026-11-09, re-rated from "accepted for now": a bug when a real sink is behind the K2; fixed in
  6fe5019, hardware check pending): a bare
  unplug after a self-powered run leaves `/rd.rec`; the next plain boot read it and showed Rd. Measured on build
  b00178c (`runs/2026-11-09-rescan/bb1.txt`, matrix rows H6 / C4): Rd on CC2 from 30 ms, 12 K2 Requests over 3 s and a
  collision with the Pixel's own Request, not "0.5-1.5 s of Rd". The host-powered half is fixed on `k2/inline-passive`
  (a boot with a USB host or a 5 V aux rail never reads the marker); what stays open is a hub-powered boot without HID
  (no aux rail to tell it from a comeback), covered only by the downstream gates and the Link's foreign-evidence latch.
  - Fix: add a `.noinit` SRAM canary written with the marker; a comeback boot trusts the flash marker only when the
    canary survived (a short VBUS loss keeps SRAM long enough? if not, bound the marker by a boot counter instead).
    Measure first whether SRAM survives the comeback's dark period on this board.
    Done in 6fe5019: the canary (`rdintent::Canary` in a `.noinit` section, `link.ld`) is armed on every `/rd.rec` save
    and a cold plain boot trusts the record only when it survived this reset and matches it (`boot_mode`'s `canary_ok`);
    the decision table is host-tested both ways. Hardware must still measure whether .noinit SRAM survives the
    comeback's ~165 ms dark on this board; if it does not, the bound falls back to a boot counter (open).
  - Validate: host tests of the decision table; hardware: (a) the CUKTECH 6 A planned re-attach still comes back with
    Rd from the first instruction, (b) unplug after a self-powered run, wait 10 s, plug into HID only: `k2_remote
    bootinfo` shows no Rd at any time (SWITCHES0 notes without PDWN bits).
- **Both Rd for 0.13 ms at SW_RES** (2026-11-09, fixed in a605db3, hardware check pending): the FUSB302 soft reset on a
  pinned boot leaves the chip default SWITCHES0 = 0x03 (both pull-downs) for 0.13 ms until the pin is written again.
  Below every debounce; not seen to matter.
  - Fix: on a pinned or comeback boot, skip SW_RES and use PD_RESET only, or write SWITCHES0 in the same I2C burst.
    Done as the first option: init() and detach_reset() write PD_RESET only when g_pin_rd != 0 (key modes, the
    inherited pin after a warm reset, the comeback marker); a plain boot keeps SW_RES at init and already used
    PD_RESET at detach. park() (DFU teardown only) is unchanged.
  - Validate: host fusb302 scan test asserting no SWITCHES0 value with both PDWN bits on a pinned boot
    (test_pinned_boot_never_soft_resets_so_no_rd_pair_at_sw_res, red in 166ca72, green in a605db3);
    `k2_remote bootinfo` notes on hardware show no 0x03.
- **Self-powered source loss at the legacy -> PD boundary** (2026-11-09, Apple PD 2.0, step 16/49 on 091e80e): VBUS
  went away and the run stopped until a replug. The K2 takes no VBUS action there; the cause is unproven. Since 1e3b67b
  a self-powered run comes back by itself.
  - Fix: diagnosis first. Arm a trigger-frozen Black Box capture on VBUS loss (`pd_trace.py captures`), re-run the Apple
    charger self-powered, read what the K2 sent in the last 500 ms before the loss (a Hard Reset? a Request the source
    disliked? the legacy unit's D+/D- release?).
  - Validate: the capture names the trigger; then a fix that keeps VBUS up, or a CHARGER_QUIRKS pattern if it is the
    source's behaviour, and a self-powered Apple run that finishes with no comeback boot in `bootinfo`.
- ~~**Flaky host tests**~~ completed: 1feba0e; `make gate` green on every merge since.
- ~~**Wrong comment**~~ completed: 8f4acaf (k2_qr.py base45 comment).
- **Charger-test journal keeps its own row order** (2026-11-09, fixed in 5a0220f, hardware check pending): `charger_journal.hpp`
  `detail::plan_order` placed the 6 A hunt rows by its own rule when it rebuilt the steps on a resume: a third copy of the
  order the grid fix removed. The header (format 3) now carries the plan's order, written from the planner's step list
  (`Runner::steps()`, the same structure the grid uses since 98e2f39), and replay applies it verbatim.
  - Fix: rebuild from the planner's step list (the same structure the grid uses since 98e2f39).
  - Validate: host test resuming a journal mid-PD with Mi 6A rows: resumed step order equals a fresh plan's order
    (`test_a_resume_mid_pd_keeps_the_planners_step_order`, green since 5a0220f).
- ~~**A PPS row shows a probe after all its APDOs**~~ completed: dfc5918; one result row per APDO with the Mi 6A probe between them on hardware c700284 (CUKTECH C1).

## Specified work (not started)

- **HID-cable loop: the loop probe** (user decision 2026-11-11). Topology: the K2 on a charger, a cable from its PD-bus
  port back into its own left (HID) port, so the HID port's VBUS follows the PD bus. The 5 V lock half is built (see
  "Not verified on hardware"); the probe is not started.
  - Probe: an on-device lab that, at 5 V, reads what the loop cable shows (VBUS and GND drop under a K2 current step,
    D+/D- continuity through the HID PHY's line state, the CC wire through the HID port's Rd, the e-marker) and shows
    the raw dump as a QR record for host decoding (`tools/k2_qr.py`), kept on flash for `k2_remote` later.
    **Built and host-tested (branch k2/loop-probe), not verified on hardware**: Tools > Loop probe, `k2_remote.py
    loop-probe` (0x57), the LOOP share type; docs/LOOP_PROBE.md (steps, readings, limits, hardware test plan). The HID
    port's own Rd is not switchable, so the CC step drops the K2's own Rd on the PD-bus side one line at a time instead.
- **Ripple viewer, on the K2 and on the web page** (user decision 2026-11-11: both views, fast and slow time scales).
  - Fast scales: the PA1 capture (AC-coupled, 512 samples, sample-time codes 0..7 = 1072..60 kS/s, a 0.48..8.6 ms
    window); a time/div selector maps to the codes; auto V/div; p-p, RMS and the dominant frequency from the `k2::ripple`
    stats module (step 2 of "Ripple: calibrate PA1" below).
  - Slow scales (10 ms..10 s): INA226 VBUS in the transition capture's fast mode (`power::fast_begin`, a value every
    ~280 us, the current reading is stale meanwhile), decimated to the screen. Does not need the INA226 sampling plan.
  - Needs first: the PA1 calibration (step 1 below, TOPADRE 65 W at 20 V) so the fast scales read mV.
  - UI: a Tools page (k2-micro-gui: lab first, the user picks the design); the web Ripple card gains time/div and stats.
  - Shell: the capture and stats reachable over `k2_remote --json` (shell-first rule).
- **Battery efficiency recorder** (user decision 2026-11-11: one Tools page, three modes Charge / Discharge / Round trip;
  both wirings selectable).
  - Counts per phase: Wh and mAh in, Wh and mAh out, time; Round trip shows efficiency = out / in.
  - Wiring "same port": the bank stays on one side, power flows forward while it charges and backward while it
    discharges; needs a reverse energy counter (`EnergyMeter` clamps negative power to 0 today).
  - Wiring "moved between ports": every phase flows forward; the selected phase says which counter takes it; Round
    trip asks the user to move the bank between the phases.
  - Open (user): how a phase ends (manual stop, or current under a threshold for N s), whether the V/A curve is saved,
    the save format (card-style record, shell readout).

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

- **Who's attached: one survey owner, run on Refresh** (decided 2026-11-09: active queries only on Refresh).
  - Today (mapped on be58a25): the page (`src/ui/ui_connection_page.cpp`) only shows what passes on the wire. Nothing
    it runs sends a source Discover Identity (only Identify, the lab `pd-reread` and the charger test's probe do);
    `ext()` (Manufacturer_Info, Source_Cap_Extended), `epr_info_list()`, Discover Modes and the cable's SVIDs/modes
    are collected elsewhere but never shown; `pd::Sink::request_epr_info` has no internal caller although its comment
    says "once per attach"; Refresh (`fusb302::rescan`) clears no caps, so the old list shows until the re-announce;
    the comment on `set_proactive_discover` (pd_sink.hpp) says default OFF while the member defaults ON. An empty row
    cannot say why it is empty.
  - Fix: a `pd::Survey` (pure plan + status table in a host-testable header; the driver runs it through the sink's
    single message gate, reusing `pd_reread`'s sequencer rather than a third copy). One ordered plan per Refresh:
    caps (Get_Source_Cap without a VBUS dark; the bridged CC cycle of `k2/rescan-bridge` as the fallback), source
    Discover Identity -> SVIDs -> Modes per known SVID (bounded), Get_Source_Cap_Extended and Get_Manufacturer_Info
    (PD 3 only), EPR_Get_Source_Cap in SPR mode (only when PDO 1 is EPR-capable), the cable (sniffed first; the VCONN
    SOP' read only standalone), the sink (passive; Identify stays a confirmed action). Every field carries a status:
    Got / NotSupported (Not_Supported or Reject) / NoAnswer / Blocked(reason: inline sink, charger test, Xiaomi chain,
    PD 2.0, no VCONN path) / NotAsked. Attach stays passive (no unsolicited VDMs). Refresh clears the source view
    first and fills it as answers arrive. Fix the proactive-discover comment to match the code.
  - UI: rows show the status instead of a blank ("not supported", "no answer", "inline: blocked"); while any field of
    the open tab is NotAsked, the status bar reads "OK > Refresh to read all" (one message, same on every tab); a
    Refresh shows progress ("Reading 3/7") and ends with "Read: 5 got, 2 not supported". New rows for Manufacturer
    (Source tab), Modes (Source/Cable Proto), EPR list; the page does not scroll today: fit them in tab rows or a
    second page per tab (k2-micro-gui rules).
  - Validate: host tests for the plan order, the gating (inline / charger test / Xiaomi chain / PD 2.0 skip their
    steps with the right Blocked reason), status mapping (Not_Supported, Reject, timeout), and that nothing active is
    sent before Refresh (red before green); render tests for the hint and status rows; `make gate`. Hardware: Baseus
    140 W, CUKTECH 10 and Apple on the bottom port, HID connected: open the page (no TX in the black box until
    Refresh), Refresh on each tab, every row Got or a named status, black box shows one message in flight at a time
    and no VBUS dark on the PD sources; Xiaomi charger: the 6 A chain still completes after a Refresh.
  - Status: **built, hardware check pending** (branch `k2/survey`: 98bd8e9 Reread gains Modes, Manufacturer_Info, survey
    order and pacing; c171604 `k2_remote pd-reread` numbering; 2930f1b pure `pd::Survey`; c9b9de5 glue through the sink
    gate; 50a2078 UI). Details: Source tab pop-up item "Details" opens the second page (Caps, Mfr, Ext, SVIDs, Modes,
    EPR); Refresh is the last item of the Cable and Sink pop-ups. Decided 2026-11-09 (user): the sink's once-per-attach
    proactive Discover SVIDs (`pd_sink.cpp`, default ON) stays, the one exception to "attach stays passive": it starts the
    Xiaomi 0x2717 chain that unlocks 6 A offers without a Refresh. Open: the Cable field is a check of the sniffed
    e-marker, not a VCONN read.

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

- ~~**PPS coverage: floor, 20 mV steps, accuracy spread**~~ completed: f733009 + 91a9839; step walk (CUKTECH C1, IQ4), spread (card 55: +10..-49 mV) and floor (IQ4 3.3 V -> 3285 mV) verified 2026-11-10. Open: the floor's fail shape needs a source that floors; `cards show` spread print fixed in 45f170e, hardware check pending. Step walk follow-up (2026-11-10, `runs/2026-11-10-pps13/`): one run failed a good charger "steps ignored" because its output creeps 20-36 mV for ~2 s after each step, so a single move can read <= 0; the walk is now judged by the 5-step total (100 +- 20 mV) and the slope over the six levels (10..30 mV/step), and only a jump of 40 mV or more fails a single step (pattern `pps-output-creep`): fixed in b1c3b7d, hardware check pending.
- **PPS deep sweep, a separate entry in the charger-test Tests list** (2026-11-10, user decision; after the coverage row
  above; fixed in 64c0ba5, hardware check pending). Opt-in, never in the default preset: every 100 mV across each PPS
  APDO, 1 A (AVS: the row below), Accept + PS_RDY + measured VBUS per point, results as a compact error-vs-setpoint
  strip on the results tab and the card (min / max / worst error and where, plus the peak delivered level and a
  fold-back: a steady out-of-band point is recorded, not a stop - "peak 19.75 V at 19.9 V, folds back to 19.42 V
  from 20.0 V" on the FH22). Long by design (about 77 points for 3.3-11 V, about 180 for 3.3-21 V), so the Tests
  list shows it as its own line with a point count. Stops at the first lost source (pattern handling as the other PPS
  rows). Validate: host tests for the point plan and the summary; hardware: one full sweep on a working PPS source,
  no reboot, journal intact. FH22 evidence: `docs/CHARGER_QUIRKS.md`,
  `runs/2026-11-10-merge2-hw/fh22-sweep.tsv`.
- **AVS deep sweep, its own entry next to the PPS sweep** (2026-11-10, user decision; built on the PPS sweep's engine;
  fixed in 9e10342, hardware check pending). Opt-in, never in a preset: every 100 mV (the AVS Request granularity) across the SPR AVS range (9-20 V) and, with
  EPR entered, the EPR AVS range (15 V up to the APDO top, capped at this K2's monitor ceiling - 36 V here, per-device;
  48 V is skipped as "above the K2's monitor"), current at or below the APDO's PDP-derived limit and never above 1 A
  for the sweep, Accept + PS_RDY + measured VBUS per point. EPR needs what the EPR rows already need (an EPR 50 V 5 A
  identity: the spoof or a real EPR cable, and EPR mode entry; refused otherwise with the existing EPR skip reasons).
  Same results strip and card summary as the PPS sweep (min / max / worst error and where), same stop-at-first-loss,
  journal resume and keep-alive rules (EPR mode needs its EPR_KeepAlive). Tests list line "AVS sweep" with the point
  count (about 111 points SPR, about 211 EPR to 36 V). Validate: host tests for the plan (SPR only, SPR + EPR capped at
  the ceiling, no AVS offered = the entry is hidden), the summary and the keep-alive; hardware: a full SPR AVS sweep on
  an AVS source (Baseus FH22, UGREEN Nexode Air 65 W) and an EPR AVS sweep to 28 V with the EPR spoof (Baseus 140 W).
  Same peak / fold-back reporting as the PPS sweep. Host-side reference (2026-11-10, Baseus FH22, AVS 9-20 V 5 A only
  with a 5 A identity, `runs/2026-11-10-merge2-hw/fh22-avs-sweep.tsv`): all 111 steps follow, offset -94 mV at 9.2 V
  growing to -150 mV at 19.9 V (~0.75 % low), peak 19.75 V, 20.0 V gives 19.66 V (fold onto its fixed-20 V level).
- **Sweep live view: the Running view of a deep sweep shows the swept range** (2026-11-10, operator finding on 7a1a5ed,
  CUKTECH C1 PPS sweep; fixed in 4e04668, hardware check pending). The badge names the range ("PPS 3.3-21V",
  "AVS 9-20V"), the gauge shows the range as dotted lines with a setpoint marker that moves point by point (plus the
  needle), and one line reads the last point ("req 12.30V  got 12.15V  err-150mV", caution colour when the point is
  outside the sweep's tolerance). Other rows render as before. Validate: host tests (`test_ui_sweep_live`,
  `test_sweep`, `test_kit`, `test_step_label`), `make gate` (stack stage); on hardware a PPS sweep on the C1 shows the
  marker walking along the band, the line following the points, and no flicker in the grid below.
  Follow-up (operator finding on hardware, fixed in 5e44447, hardware check pending): the grid's Sweep row listed one
  badge per volt with a dot that never moved; it is now one range chip ("3.3-21V", current underneath) like the PPS
  row above it, on the Running, Results and Steps views. Validate: `test_result_layout`, `test_ui_sweep_live`; on
  hardware the Sweep row shows one chip that goes green, amber or red when the sweep closes.

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
- The HID-cable loop probe (Tools > Loop probe): built and host-tested only; the test plan is in `LOOP_PROBE.md`.
- The reboot-resume of a charger run after an unplanned VBUS loss on a self-powered K2 (the comeback marker of
  1e3b67b); a host-powered K2 waited forever for stable 5 V on 2026-11-07.
- **HID-cable loop 5 V lock** (k2/loop-lock, built and host-tested only; ARCHITECTURE "HID-cable loop guard"). Certain at
  aux > 5.8 V; suspected ("the rail follows VBUS") when the HID rail and VBUS cross 4 V up within 100 ms of each other,
  or are both already up at boot, with no configured, awake USB host; cleared by VBUS < 1 V with the rail up (a separate
  supply), an empty HID port or an active host for 1 s. Under the lock every raise refuses (PD sink and request entry,
  PD trigger, legacy triggers and scanner, charger test `Reason::Loop`, PD Stress, twin, lab shell), a contract above
  5 V goes to PDO 1, then Hard Reset (600 ms above 6 V), then CC detach (1 s later). Validate at 5 V only, never request
  above 5 V while looped: (1) passive K2, plug charger + loop cable -> `[LOOP] lock suspected ... gap=-1` (or a small
  gap), the Caution footer "HID-cable loop: 5V lock" at once, charger test START refused (Loop), PD trigger above 5 V
  refused, legacy trigger refused; (2) PC on HID with the USB link OFF, then a charger, charger test START -> no lock;
  (3) charger first, then the PC -> no lock; (4) PC on HID + the CUKTECH USB-A auto-off port cycling VBUS -> never a
  lock; (5) PC on HID (link on) + charger -> `state --json` shows `loop.host` true and `loop.locked` false. Logs read
  over the PC afterwards.

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
| Apple charger readout, lab-only `apple-read` op (armed SVID 0x05AC modes, directory + string reads; maker/product/fw/hw/0x90/serial on Source tab, in `k2_remote apple-read` and `--json state source.apple`; off by default, refused inline/no-contract/not-partner) | fixed in a2ac534, hardware check pending | not verified (host tests only, red first in e37bc33) | `pd_apple_readout`, `apple_read` | PASSIVE |
| Rescan and CC choice | done | verified | `pd_rescan`, `pd_ccscan` | |
| Link summary (USB4/TBT/DP from sniffed PD, the `USB` and `Alt` rows) | done | not verified | `usblink` | PASSIVE |
| "AI..." charger recognition widget | done | not verified | `fastcharge_classify`, `ai_recog` | PASSIVE |
| "PD Compat." toggle | planned | n/a | | |
| SPR AVS 9-20 V requests | partial (request fields exist, no UI) | not verified | `pd_sink` | HV |
| Apple battery level and model, charger serial | planned | n/a | `pd_vdm` | PASSIVE |
| Xiaomi Mi Turbo auth replies | partial: observation only | n/a | `pd_vdm` | HV |
| UFCS 7-DO handshake and frames | handshake, Ping, Get_Output_Capabilities and Request/Accept hardware-verified on a Baseus FH22 charger (2026-11-07), with Power_Ready, keep-alive and the 5/9/11 V levels; a 20 V 3 A contract on a CUKTECH 10 charger; not yet seen on a source: Refuse, NCK, the source's own Ping | n/a | `ufcs` | HV |

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
