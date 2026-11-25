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

- Screen no longer turns off while navigating (2026-11-23, user, K2 1.2.0-100, Cable tab): `idle_hw::service` ticked with
  the loop's `now`, older than a press stamped in a wait loop; `idle::Machine::advance` read it as a 49-day idle
  (`idle_s` 4294967) and went Off. Fix ac081b55: an older stamp adds no time (`test_idle`).

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

- **Selftest slower than the 1.0.2 perf baseline** (2026-11-24, selftest images built with K2_SELFTEST=1 K2_PDSPEC=0 K2_WEBUSB=0 to fit). Against tools/perf_baseline.json (1.0.2-155, 2026-11-16): sweep_sum +60 %, fmt_line +31 %, gfx_fill +6 %, pd_sink +5-7 %, pd_codec +3 %, power_calc +2 %; rd_slow 19 % faster. The same image built at 30236902 (this morning) shows the same numbers, so the day's work (icons, V/A picker, flash carve, limiter, battery) is within ~1.3 % of it; the drift happened between 1.0.2 and 1.2.0-297. Open: bisect sweep_sum and fmt_line before refreshing the baseline. Cause found (not verified on hardware): lambdas of pinned/K2_HOT functions and strcmp, PixelSink, pd le32 and other callees sat past 0x08040000; pinned in 8531d244, gate step "hot" now fails on such lambdas (6599c925). Remeasure on the K2.

- **Volume budget understates the root pair** (2026-11-24, found on k2/battresume). `tests/test_volume_budget.cpp` fills each store alone, so the three `/bt<id>.rec` show 0 blocks; with the device's real root (settings, the six store directories, `/log.txt`) the root pair splits (+2 blocks) at the first compaction after the third run file, on the base firmware too (246 B records, write_file at phase boundaries). Addressed on k2/battresume: the runs moved to their own `/bt` pair (2 blocks, budget row updated) and `test_battery_runs_and_the_real_root` fills the real root. Open: a K2_PDSTRESS=1 build with every store at cap is now 2 blocks short (the largest `/jr` misses).

- **Battery recorder counts a moved-wiring discharge as "in"** (2026-11-24, 077dd1d9). Full bank on the K2 input, laptop on the output, `battery start discharge` with wiring moved: the monitor reads the bank-to-laptop flow as negative current (direction down, -862 mA at 20 V; the same sign as charger-to-bank earlier), so after 23 s the status read in 0.321 Wh, out 0.000 Wh. BATTERY_RECORDER.md says moved discharge = forward = out. Either the input/output port mapping or the sign rule is reversed. Also: the recorder stored a run with 0.000 Wh in and out; such runs should not be saved. Fixed by 05313146 (columns by kind) and c69d052e (empty runs); the version 1 records written before it now read their energy from the phase's own column (k2/batteff).
  Fixed on k2/battfix (05313146, c69d052e), host-tested, not verified on hardware. Root cause: the firmware's positive current is bottom port to top port (charger on the bottom); the recorder read it as charger-to-bank, so with the charger on the top port every column swapped. Each phase now takes its dominant direction as its own (charge = in, discharge = out). Empty runs (under 1 mWh both ways) are no longer saved, and the start no longer writes a file. A record from the old firmware: swap a phase's in/out (Wh and mAh) when its dominant column disagrees with its kind; the totals are exact.

- **PD limiter gives up when the sink requests a 0 mA object** (2026-11-24, 077dd1d9, 100 W charger -> K2 -> power bank). From a 30 W limit (9 V contract), `pd-limit set --fixed 15V/3A`: the bank requested position 4 (20 V, forged at 0 mA) with 0 mA (RDO 0x41000000) twice; the limiter counted "ignored" and turned off ("the sink ignores it"), leaving 20 V. The same 15 V limit from a 20 V contract earlier was accepted at 15 V. Fixed on k2/limfix-cut (44fd2cd3, ca9ccdd2; host tests only, not verified on hardware): the forged list always ends before the first object over the limit, so no 0 mA placeholder is ever offered and there is no 20 V / 0 mA step (here 5/9/15 V at once). APDOs after the cut are lost for that limit. Mid-list objects cannot be dropped: the sink's Request reaches the charger unchanged and is judged by position. Fixed off works only when the APDOs follow 5 V: otherwise the picker refuses it ("Fixed Off: not on charger") and the limiter stops on its own (`off (not on this charger)`). Replaces the re-offer cut of 0c158287. Open: why this sink prefers 20 V / 0 A over 15 V.

- **PD limiter follow-ups** (2026-11-24, k2/limfix review, not fixed): a Negotiating or Pending timeout falls back to Armed silently (status "armed" while the sink runs an unlimited contract until the next real list); the 20-list budget (`kLimMaxInjections`) spans replugs and renegotiations, so a long session ends the limit ("too many lists"); after a give-up nothing on the UI says Off is needed to hand the sink the real list; a warm K2 reset (flash, watchdog, reboot) while the sink holds a forged list forgets it (RAM only), so Off has nothing to undo and only a replug helps; shell off does not report a reset it could not send (read the `[UI] link reset` log line).

- **PD limiter: a strict charger refuses the re-offered Request, status still "limited"** (2026-11-24, 1.2.0-338-f7d2f0d6, charger with 5/9/12/15 V 3 A, 20 V 5 A, PPS 3.3-11 V 5 A -> K2 inline -> camera battery charger; traces runs/ui-pdlimit-va/canon-dump1.txt, canon-dump-9v.txt, canon-dump-off.txt). After a Hard Reset the charger lists twice (5 V first, the second list ~166 ms later, the sink takes 15 V both times); the forged list went out after the second PS_RDY with a fresh MessageID past the charger's (id 7 after the charger's 5); the sink requested 12 V (or 9 V) ~8 ms later and the charger answered Not_Supported ~2.6 ms after that Request (its usual Request-to-Accept time): no Accept, no PS_RDY, VBUS stayed 15.03 V. The limiter called the sink's compliant Request proof and reported "limited". Most likely the charger's policy engine took our Source_Capabilities (a source header it never expects) and then refused the Request; a charger that refuses every unsolicited Request in Ready would refuse PPS re-requests too. Separately, an arm with a contract already live and no list seen since boot (after a flash) stayed "armed". Fixed on k2/limstrict (f19e335b, 1593dc01; host tests only, not verified on hardware): Limited only after the charger's Accept and PS_RDY; the forged list now carries the sink's newest MessageID, which the charger holds and drops as a retry while the sink takes it (fresh ID when that one is unusable); a Reject / Not_Supported / Wait / silence after a compliant Request is a refusal, the other ID scheme is tried, the second refusal stops ("the charger refuses re-offers", toast "Limit off: charger refuses") and the UI sends the off path's inline Hard Reset so the sink gets the real list (not when bus-powered: Off does it). Arming over a joined contract (UI after its second OK, the shell at once) resets the link first and arms unseeded. Validate on this setup: arm 12 V and 9 V, read `[LIM] forged list MessageID` and the charger's answer in the trace.

- **Inline sniffer misses frames, Diag latches on them** (2026-11-24, 1.2.0-250; Baseus 140W -> cable -> K2 -> OnePlus 15 plugged straight into the K2, PPS 5-11 V up to ~9.5 V 5 A; `runs/op15-pps-2026-11-24/trace-live.txt`, `seq.txt`). The phone charged normally the whole time; the K2's capture did not keep up:
  - **Missed PPS Requests**: up to 8 phone-side MessageID gaps (some fall on resets). Note: most "Accept with no Request" in the log are answers to the phone's Soft_Reset (each attach: Discover_Identity, Data_Reset, Soft_Reset, Accept), not misses.
  - **Missed Accepts**: 4 PS_RDY with a Request but no Accept before them (191 Requests, 190 Accepts, 187 PS_RDY, no Reject).
  - **Missed source side**: a burst of 4 Requests (ids 7, 0, 1, 2) where the K2 saw the phone's Requests and the phone's GoodCRCs to the charger's messages, but none of the charger's GoodCRC / Accept / PS_RDY, until the 5th Request's Accept.
  - **Not CPU**: at the confirmed miss (Request PPS 8.72 V) service() latency was 1.34 ms and the frames after the lost Accept (the phone's GoodCRC for it, then PS_RDY) were captured; an RX FIFO overflow loses the tail, not one frame in the middle. Misses are mostly charger-side (MessageID gaps: about 36 of 520 charger messages vs 8 of 235 phone messages, attach resets included), so the FUSB302 did not decode the far end's frame (a bad-CRC frame is dropped with no interrupt). Next: a scope or a second sniffer on CC at the K2, and the charger-side vs phone-side miss rate with the K2 at the charger end.
  - **Ends swapped (K2 at the charger, cable to the phone), same session**: the K2 latched inline on CC2 (MEAS_CC2, BC_LVL flipping 1/3) and captured no PD frame at all, not even the phone's periodic PPS Requests, while VBUS sat at 8.5-8.7 V with the phone drawing 3.9-4.8 A (a PPS contract it never saw). Blind for the whole attach, not a dropped frame: the CC it listens on or the receiver setup in this orientation is the first suspect. Trace: the blind attach scanned CC2 at BC_LVL 1 (an Ra-like 200-660 mV, not the 3 A Rp-over-Rd level the working CC line read) and through the 5 V -> 8 V PPS step showed no Rx, no RxBad and no STATUS0 ACTIVITY on CC2: the BMC was on the other line. Partial: d5972123, 5841d8a5 (not verified on hardware): the scan picks the line by DC levels only and nothing moved it once VBUS was up; a passive K2 with VBUS up and no frame decoded now moves the receiver (MEAS/RX only) to the other line (1 s, 1 s, 3 s, then every 11 s; 4 moves at 5 V; a decoded frame keeps the line for the attach), log `[PD] Listen CCx -> CCy`. Check: both plug flips at both ends decode the PPS Requests; if the K2 still hears nothing on either line, the receiver setup is the cause.
  - **Diag**: showed "No accept" and kept it after later Requests were accepted; briefly showed "No GoodCRC" (likely that burst). A frame the sniffer missed must not become a verdict: a later Accept / PS_RDY (or the partner's GoodCRC to a message we did not see) clears it, and a miss is reported as a capture gap, not a protocol fault. Done: a6dbe27a, 4d8e5f90 (host tests; not verified on hardware): a GoodCRC to no decoded frame, or the sender's next MessageID, counts a capture gap (Info, "Capture gap"); no NoAccept / NoPsRdy / NoGoodCrc verdict spans one; the sink's GoodCRC after a Request stands for the Accept, then the PS_RDY; a retried frame left without GoodCRC stays NoGoodCrc.
  - Tool: `pd_trace.py live` printed every record with one frozen timestamp (4153787.251 ms, +0.000) after a "records lost" wrap, so the log has no timing. Fixed: 283a9661 (host test; not verified on hardware): the device ring clamped every stamp to the last one after a quiet over 2^31 us (35.8 min); the tool was right.
- **Inline: the charger's 5 A offer varies per attach** (same session and setup, cable between the Baseus and the K2): 7 caps lists with 3 A (fixed 20 V and both PPS ranges), 10 with 20 V 5 A / PPS 5 A, with no change of cable; the phone took 5 A every time it was offered. Cause not established (cable read by the charger through the passive K2 path is the first suspect).

- **Rail flips in the two side rotations** (2026-11-23, v1.2.0-192, user): the fixed rail order holds top/bottom, but left/right
  still flips. Fixed in k2/ui-native (ab89ae9d): both side rails read bottom-up (`rail_text_dir`), so rotation 2 has its title
  at the bottom like rotation 0; the button strip keeps `panel_text_dir`. Host tests `test_ui_layout` (side rails read alike),
  `test_ui_rotation` (a turned Ui draws the rail of a fresh one), `test_ui_capture`. Not verified on hardware. Verify: rotations
  0 and 2, the rail title at the bottom, cells reading upward.
- **Pills squeezed in landscape** (2026-11-23, user): chips (Charger tab hero PD3.x/EPR/PPS, Lines tags, verdict chips), the
  Numbers V/A/W badges and the toast lost rows and edges in rotations 1 and 3. Fixed in k2/ui-native (69cb1bc8, ce87a3e4,
  d6d8f348, 48b14a4a): `draw_pill_box` draws every pill at native size from its mapped origin (the toast centred, hanging from its band
  top); `erase_toast` and `kit::erase_chips` wipe the rows a native pill hangs below its mapped box. Host tests `test_kit`, `test_ui_rotation`,
  `test_ui_memory`, `test_ui_charger_test`. Not verified on hardware. Verify: rotations 1 and 3, chips and badges with four
  whole edges, a toast that clears without a red line under it (Memory page at low space).
- **LOG stamp cross glitched** (2026-11-23, user): the 16 px cross sampled to 12 px on one side only. Fixed in k2/ui-native
  (37ce915a): a 12 px mark is the icon table's glyph; smaller marks sample symmetrically; the label-less verdict cell stays
  native. Host tests `test_kit`, `test_ui_widgets`. Not verified on hardware. Verify: Log place, the stamp's X in 4 rotations.
- **Lines alignment** (2026-11-23, audit): VREF ended one cell short (the two-byte degree sign counted as two cells), the bars
  ended at x198 against the values' x202, and landscape leader dots came out 2 or 3 px wide. Fixed in k2/ui-native (12474667,
  82312fcb, 9156320a). Not verified on hardware. Open: in rotations 1 and 3 a right-aligned field starts at its mapped origin at
  native width, so values end about 15 px short of the stretched bars (217 against 232; kit-wide). Route: shift a right-aligned
  field's logical origin as the RowList tail does (`tail_origin`, kit_hot.cpp), so the ground holes stay consistent.
- **Battery rows mixed icons** (2026-11-23, user): the settings rows, Saved runs and Share had no marker. Fixed in k2/ui-native
  (5ff1c12e): Plug, Meter, Clock, Report, Phone. Superseded on k2/battfix (fd665c40): all Battery rows marker-less (user: icons
  "not needed at all"), Clock icon deleted. Host test `test_ui_battery`. Not verified on hardware.
- **PC-link socket icon too dim on the rail** (2026-11-23, user): brightened from `kInk3` to `kInk2` (`src/ui/kit_hot.cpp`), along with bezel-pinned telemetry rail and link icon across all 4 rotations (`include/k2/ui/ui_layout.hpp`, `src/ui/ui_layout.cpp`). Host-tested (`test_kit`, `test_ui_layout`, `test_ui_rotation`, `test_ui_capture`, `test_ui_extremes`); not verified on hardware. Verify: plug into a dumb charger/PC power port (Power state) across all 4 rotations; socket icon is readable and the rail/icon stay pinned to the physical bezel ends.

Open defects, newest first. A row leaves this list when its fix is merged and seen on hardware.

- **Phone sweep depth high window & fine-point accounting** (2026-11-24, read-only code/capture audit, not edited):
  (1) `ctsweep::kWinMv[1]` (`{14000, 19000}` mV in `include/k2/chargertest/sweep.hpp`) starts too high for 2:1 charging on a discharged 2S phone such as OnePlus 15 (`oplus_chg_pps.c` `target_vbus = ((vbat_mv * cp_ratio) / 100) * 100 + delta_vbus` with `batt_num = 2`, `delta_vbus = 450 mV` in 2:1; hardware capture `runs/op120-2026-11-18/full-oneplus/trace.jsonl` stepped `13.00 -> 13.10 V` in 20 mV steps, below `14.0 V`; pending live discharged OnePlus 15 run).
  (2) `plan_total()` (`src/chargertest/sweep.cpp`) accumulates fine 20 mV point count `n` into `total` instead of 100 mV record-cell count `c`, comparing against `kMaxPoints` (`640`), slightly under-estimating total cells across multi-APDO lists.
  (3) `sweep_point()` / `spec_feed()` (`src/chargertest/charger_test.cpp`): off-grid 20 mV points (`plan_cell < 0`) are not stored in `sweep_err_[]` (and `spec_feed()` skips moves `< 100 mV` unless last), and `sweep_close()` drops `Refiner` (`sweep_ref_`) results whenever `sum.fold_mv == 0` on the 100 mV cell grid.

- **Inline phone after a K2 re-read: Graphs "VBUS lost", LOG "Link unstable, K2 is the sink"** (2026-11-23, v1.2.0-222,
  charger asleep -> `pd-reread` -> Pixel on PPS behind the K2). Cause: CC never detaches, so the diagnosis kept the K2's own
  5 V sink session under the phone: its sink role stuck (`s.active`) and the phone's plug-in sag to 3.4 V on the K2's
  contract counted as VBUS lost (not the Rd release). Fixed in k2/inline-readouts (c9f1d5e7): the inline latch's Link record
  ends the K2's sink session and opens the device's, keeping the offer and a negotiation already started
  (`test_pd_diag` inline cases). Not verified on hardware. Verify: the same sequence; LOG shows the phone (no "K2 is the
  sink", no "unstable"), Graphs no "VBUS lost".
- **Lines D+/D- row showed the PD summary** ("PD 3.x 105W", same setup): the row printed the verdict's detail pill. Fixed in
  k2/inline-readouts (5f6c7436): the row shows `fastcharge::lines()`, the pair classified with PD left out (QC, DCP, Apple,
  USB data, none); the chips keep the PD summary. Not verified on hardware. Verify: Lines with the phone on PPS reads
  "USB data" or "none" there.

- **Black band left over the page after a toast** (2026-11-23, v1.2.0-196, user): on Who's attached > Charger (button strip at
  the top) the survey toasts ("Reading charger n/m", the summary) left a black band across the body. Cause: a toast that was
  cleared or replaced by a shorter one erased its band and nothing repainted the rows under it; screenshots hide it (capture
  repaints everything). Fixed in k2/toast-erase (3ce05955): `Ui::restore_under_toast` repaints the page, static chrome included,
  for every page; host test `test_ui_toast_erase` (pixels, 12 pages, 4 rotations). The host did NOT reproduce the Charger-tab
  survey itself (its toasts, with or without live values, left a clean page on the old code too); what reproduced was a toast
  cleared by its page, a toast replaced by a shorter one, and the static chrome of Lines after a lapse. Cost: a shrink or clear
  repaints the page like a page entry (about 56k px in 4 ticks on the Charger tab). Not verified on hardware. Verify: Read
  charger on the Charger tab in two rotations (strip at top and bottom); no black band, no lingering pill, no visible flash.

- **PD limit pick did not apply on a live contract** (2026-11-23, Anker 100W -> K2 -> phone): a level picked with the phone
  already in a contract needed a replug. Fixed in k2/pd-limit-fixes (reset first, unseeded arm, sniffer cleared on detach), host
  tests only; the cause is not proven on hardware. Verify: arm 15 W from the picker with the phone attached, OK twice.
- **Vendor database follow-ups** (2026-11-23): VID 0x0716 (this K2's own, Oxus Research on the USB-IF list) is left unlabeled;
  `rl::vendor_oem_name` is a separate table. Done: cable names are "<Brand> <W>W" (c23154ac), SVID names come from the one table
  (5f5f3f8c). Names are shortenings of the USB-IF registrant, never a brand (user 2026-11-23), enforced by
  `gen_vendors.derives` on every generation (bc288c9e): 0x2B01 CUKTECH->ZIMI, 0x1A86 WCH->QINHENG, 0x0DBF JPC->JESS,
  0x05BE TE CONN->TYCO; 0 B text. SVID 0x2E87 (Injoinic) now reads "Injoinic PD" in
  `protocol_name` (was the "Cuktech PD" swap). Not verified on the K2. 106 Asian cable makers, connector and wire
  houses added (k2/cable-vids). Verified 2026-11-23 (user, K2 1.2.0-100): the cable with ID header VID 0x2CA1 reads
  "Veetone". SVID 0x8087 on a cable with VID 0000 is Thunderbolt mode support, not a maker (SVID table).

- **USB 3.2 Gen 2x1 (10 Gbps) / USB4 pass-through cold-attach failure — hardware limit, not firmware** (2026-11-22,
  AMD USB4 `0000:c5:00.3` / `Bus 005` + `Bus 006`, 5 cm cable + Samsung PSSD T7 `04e8:4001`, plus ASMedia `246x` /
  XFANIC USB4 dock, `runs/usb4-2026-11-22/`). Direct `5 cm cable -> T7` holds 10.0 Gb/s with zero errors. Through the
  K2 on cold attach (no side HID), SuperSpeed Gen 2 drops into `SS.Inactive` (`0xa4002c0` / `0xb0202a0`) and fails warm
  reset (`0x2802a0`); the T7's USB2 fallback (`usb5-port1` `0x6e1`) then drops mid-reset (`0x6f1 -> 0x2202a0`) while
  `usb6-port1` is still cycling `SS.Inactive`, leaving the T7 unenumerated (`~287 mA`, `D+/D- = 0 V`) until VBUS is
  cut. 2026-11-22 night: a USB-C continuity tester through the K2 reads the SuperSpeed contacts at pins 10/11 of both rows
  open in both orientations (TX1 in one), with every SuperSpeed pin lit on the cable alone: damaged connector contacts on
  this unit's bottom receptacle: pulling the inserted cable sideways makes the open pins conduct (2026-11-23). Fix is a
  connector replacement; no firmware route. Ruled out as a firmware bug: (a) once SuperSpeed trained, a 16-config automated sweep toggling `PB13`, `PB15`,
  `FUSB302`/`PC13-15`, `dpdm`/`dpsniff`, global `2 MHz` vs `50 MHz` slew, `LCD/PWM`, `load`, and `ADC`/`INA226` passed
  16/16 consecutive `usb5/6/authorized 0->1` warm resets at `10.0 Gb/s` (`uas`, 0 `SS.Inactive`, 0 drops), including
  full Open-K2 default; (b) cold-boot failures reproduce identically in bootloader DFU, a pure `wfi` idle binary, and an
  all-peripherals-off build. Side note: holding `+` occasionally helped even in the `wfi`/all-off builds because `+`
  is galvanically coupled to `CC2` (~5.2 kOhm hardware `Rd`, `docs/K2_PORTS_SPEC.md`), altering plug-in attach timing
  independently of firmware.

- **A legacy-only run (USB-A port, no PD) fails a PD row and a red Xiaomi "auth"** (2026-11-21, 1.1.0-217-53dee143,
  UGREEN 65 W USB-A port through an A-to-C cable, Deep full run, `runs/hw-2026-11-21/153343-ugreen-deep/`). The preflight
  said "no PD source capabilities after a rescan: legacy-only run", yet the PD unit planned a row that ended
  `FAIL no PD after re-attach` and the Xiaomi row shows a red "auth" cell; on a port with no PD both should be a dim
  skip. `ct_start.py` then died reading the summary: `FrameError: charger-test row contradicts negotiated wire revision`
  (`k2_remote.py:1627`). The legacy units themselves were right: FCP 9/12 V, AFC 9/12 V, QC2 5/9/12 V, QC3 5-12-5 V,
  QC3B 5/12 V pass; SCP 10 V / 4.5 V "acked, no change"; VOOC, VIVO, UFCS, QC3+ no reply.
  **Fixed on host, not verified on hardware** (k2/ct-legacy-only; seen again 2026-11-23, 1.2.0-116, USB-A port, A-to-C cable,
  `runs/ct-usba-2026-11-23/rec1/`, row 22): the Xiaomi chain row stayed Pending through the no-caps re-plan and failed
  NoReattach; it now skips "no offer" with the other PD rows. `decode_charger_test` reports a ran-row/wire contradiction as a
  `warnings` entry instead of raising (the JS decoder, `tools/web/k2proto.js` and `k2.html`, still raises). Verify: a legacy-only
  run on a USB-A port ends with no PD row red, and `ct_start.py` reads the summary.
- **The first unit detach waits 10 s on a USB-A port** (2026-11-23, same run: later detaches took ~1.5 s). Fixed on host, not
  verified on hardware: with no source list ever heard and VBUS at 5 V at every sample of the detach, the first detach uses
  `kDetachQuickMs` too (host 2.1 s from START, was at least 10 s). Verify: A-port run, the first Rd off to attach about 1.5 s;
  a C charger without PD that drops VBUS slowly still waits (a drop later than 1.5 s with no sample off 5 V would be missed).
- **A source whose D+/D- read open gave one "no reply" per protocol** (2026-11-23, `runs/ct-usba-2026-11-23/rec1`: `dcp check ...
  result=1 (open)`, every D+/D- row failed; the flipped plug in `rec2-flipped` read short and QC2/QC3/UFCS passed). Fixed on
  host, not verified on hardware (k2/dpdm-open): on a source that never spoke PD, an open reading plus one silent D+/D- unit (the
  confirming attempt) turns the remaining D+/D- units into one reason, `LinesOpen` (wire id 54; schema 1 maps it to Request).
  Cell/card "lines open", Results fold "No D+/D-", footer "Open: flip or data cable", `k2_remote`/`ct_start` summary, JSON
  `"dpdm": "open"|"short"|"unknown"` (run-selected word bits 30..31; older firmware sends 0 = unknown). A PD source and a short reading are unchanged. Verify: A-to-C
  on a USB-A port with the plug the open way round: units end "lines open", footer names flip or data-less cable; flipped, the
  protocols run.
- **Back-to-back screenshots stall a charger-test run** (2026-11-21, 1.1.0-217-53dee143, Baseus 140 W). With
  `k2_remote.py screenshot` every ~0.65 s (each ~450 ms on the device) a Medium run sat after `Verify 5V` for ~7 min and
  went on within a second of the loop stopping (`runs/hw-2026-11-21/152248-qc2-fast2/`); one every ~3 s is harmless.
  Something in the step start waits for a quiet the screenshot keeps resetting; a run should never wait on the shell.

- ~~**b4d5ed89 (live-VBUS attach / inline release) breaks charger-test runs**~~ fixed in 8c7a981b, seen on hardware (2026-11-21, Baseus 140 W, nothing behind the
  K2; `runs/hw-2026-11-21/150819-deep-b4d5/`). A Deep run stopped at the PPS 5-11 V sweep (5.8 V Request accepted, no
  PS_RDY yet) with `FAIL load attached`, the rest skipped "load attached", `Sink downstream (inline=1 far request=0)`.
  Also on that build a START against the Baseus with its output off (it needs Rd to turn on) gave "no PD source
  capabilities after a rescan: legacy-only run". The same runs on 53dee143 (without b4d5ed89) woke the charger and passed
  32/0 (Deep) and 26/0 (Medium).
  **Fixed on host, not verified on hardware:** `b4d5ed89` switched `Ui::tick()` (`src/ui/ui_hot.cpp`) from the 128-sample
  `pm_avg.current_ma` to unfiltered single-sample `power::read_raw()` while `transcap::arm()` runs INA226 in 1-avg 140 us
  mode (`kIna226Fast`), so a single transient sample > 50 mA (150 uV) latched `inline_` on `439886 ms`. Added a 5-sample
  sliding median by magnitude (`pd::CurrentSpikeFilter`) before `LinkInput::current_ma` (rejects up to 2-tick spikes while
  passing boot tick 1 and dropping to 0 in 3 ticks on unplug). Note on `150305-deep-b4d5`: that capture started with
  `vbus=5010 mV`, `cc1=3313 mV`, `dp=2695/dm=2700` (same as `145031-deep-b4d9` before `b4d5ed89`), whereas dark-port
  `START` (`vbus < 4500 mV`) is unchanged and regression-tested with an inrush spike (`test_pd_link`,
  `test_ui_charger_test`). Needs hardware run on Baseus 140 W.
  **Hardware 2026-11-21 (1.1.0-226-8c7a981b, Joyroom 65 W, PPS 3.3-11 V / 3.3-21 V):** `tools/ct_soak.py` 2 Deep runs
  (pd, pd-low, qc2, both orders) 26 pass / 0 fail each through every PPS sweep, no "load attached", no reboot, stack low
  16392 B; 3 Medium runs over random unit subsets 0 fail (`runs/soak-1121-164810/`, `runs/soak-1121-165751/`). Baseus
  140 W, same build, output dark at START (VBUS 0 V, a 20 V / 5 A cable so EPR skipped): 2 Deep runs 24 pass / 0 fail,
  START woke the charger with Rd, every PPS sweep clean (`runs/soak-1121-172629/`). Inline latch on
  the same build (Joyroom 65 W on the bottom port, a phone on the top port charging at ~9 V 1.3 A): latched "sink request",
  START refused "load attached, latched by sink request"; after a K2 reboot under that load it latched "load" (the current
  path through the filter) by 6 s after boot, and START refused again. Phone unplugged: the latch was clear at the first read
  (~3 s later; CC2 back at open Rp 3.3 V, VBUS dark) and a START woke the Joyroom and ran (Light, pd: 1 pass, 0 fail).

- ~~**`make gate` fails at the tests on public since 3614728f** (2026-11-21, host).~~ Fixed in fb3f086f: the toast is re-raised only
  after a slice that covered it (Results scroll 49.5k px); Ripple/Connection/Diagnosis budgets re-measured. Host only. `test_ui_perf.cpp:516` (worst scroll over
  53600) and `test_ui_overdraw.cpp:227` (6 full-paint budgets) fail from 3614728f (place scrollbar to the bottom, toast
  above the scroll); b4d92f10 (Ripple native pixels) adds 2 more overdraw fails. 949c134c and earlier pass.

- **QC2 Deep testing animation does not advance to the next voltage badge** (2026-11-21, user hardware report). During a
  Deep charger test, QC2 changes VBUS voltage through its levels, but the Running-grid highlight/outline stays stuck on
  the first voltage badge instead of advancing to the active voltage badge.
  Not reproduced on the host (2026-11-21): a Medium and a Deep plan with VBUS following each Request put the dot on the
  5 / 9 / 12 V cells in turn; QC2 opens no paint-block window. Regression test 82910f13 (`test_ui_charger_test`).
  Hardware 2026-11-21 (1.1.0-217-53dee143, Baseus 140 W, Deep `pd,qc2,pd-low`, `runs/hw-2026-11-21/151301-deep-mine/`):
  QC2 5/9/12 V pass and the running outline moved 5 V -> 12 V with 5 V and 9 V filled green (screenshots every ~3 s, so the
  ~1 s 9 V step was not caught on its own). Not reproduced; the UGREEN 65 W USB-A port (Deep, same build, `153343-ugreen-deep`) showed the same: the outline went
  5 V -> 12 V on QC2 and followed QC3 5 -> 9 -> 12 -> 9 -> 5 V; the UGREEN USB-C port (`153757-ugreen-c-deep`) caught the outline on QC2 9 V.
  **A second cause on PD chargers found 2026-11-23** (Baseus 140 W, user photos; capture in `runs/qc2-baseus-2026-11-23/`); the
  USB-A sightings above (no PD, so no spec stage) stay open.
  The PD spec stage sweeps the PPS / AVS APDOs and fixed levels once the PD unit has run, and `Runner::next()` has by then put
  `index()` on the first Pending row of the next unit (QC2 5 V). The Running header (family, name, "target 5V"), the gauge
  target and the grid outline all read `index()`, so the QC2 badge was lit while the voltage swept. The counter was
  not wrong: 29/54 counts non-Skip rows (29 PD rows done, 25 QC2 / QC3 / UFCS rows still Pending); those 25 later skipped
  ("request failed"), so the run ended 29 pass / 42 skip of 71.
  Fix 7d7517fc, **partly verified on hardware 2026-11-23** (1.2.0-116-0d39b5c6, Baseus 140 W over a 3 A cable, Deep,
  `runs/ct-sweep-label-hw/`): through the spec stage the header read "PD 5V", "PD 15V", "PPS 5V" (USB PD), the outline sat
  on the PD chips and the PPS 5-21V row, the counter held at 30/55, and QC2 5V was named only after the stage. AVS, EPR
  and Hard Reset points not seen (no AVS offered; EPR blocked by the 3 A cable). `pdspec::Engine::point()` -> `Runner::spec_point()`; in the Spec state the page names the
  engine's point ("PPS 8.02V", "AVS 21V", "PD 9V", "Hard Reset"), its target and gauge follow it, and the outline sits on the
  matching chip of the compact PD row (`test_ui_charger_test` spec-stage walk, `test_charger_test`, `test_step_label`).
  Host walk reaches SPR and EPR AVS and fixed 28 V points (every tick checked) and the first tick after the stage.
  Follow-ups (host only, not verified on hardware): f2601bf1 names the SVOOC ceiling stage ("SVOOC ceiling") and the
  identity-probe window ("Charger ID") instead of the next unit, and a Hard Reset point keeps the grid window where the
  previous point had it; 1ed2c4a9 adds `0x0B` req `[10]` (now: interlude, spec point, name) that `k2_remote --json
  charger-test status/view` and the web page attach as `now` (an older firmware omits it); the web grid shows no running
  dot while a stage runs. runev Resume/Suspend/RowStart still record the row index.

- **PPS Deep range label disagrees with the range being tested** (2026-11-21, user hardware report). The UI displays a
  `5V-21V` PPS range while the engine is visibly stepping through a `3.3V-5V` range (check APDO min/max formatting vs
  sweep target bounds).
  Host check 2026-11-21: label, live range and swept points agree on one- and two-APDO lists (3.3-21V; 3.3-11V + 3.3-21V;
  3.3-5.9V + 5-21V). Found and fixed on the way (98068797, host-tested): the Running header name cell clipped the low part
  "PPS 3.3-4.9V" to "PPS 3.3-...", so only the 5-21V part showed a readable range. Hardware 2026-11-21 (same run): this Baseus port offers
  PPS 5-11 V and 5-21 V (no 3.3 V APDO); walks 13->21->13->9->5 V and the 5..21 V sweep ran under the "PPS 5-21V" chip
  and header, no mismatch. The UGREEN 65 W USB-C port (PPS 5-11 V, 5-21 V, SPR AVS 9-20 V;
  `153757-ugreen-c-deep`, 30 pass / 0 fail) matched too. Joyroom 65 W (PPS 3.3-11 V 5 A, 3.3-21 V 3 A), 2026-11-21: on b4d5ed89 the
  low part's header reads "PPS 3.3-..." (`155748-joyroom-low-old/sheet.png`); on 53dee143 (98068797) it reads
  "PPS 3.3-4.9V" with the grid chip on the swept APDO, Deep run 26 pass / 0 fail (`154914-joyroom-deep/sheet-low*.png`).
  Fixed and seen on hardware; leaves this list when 98068797 is merged. Side note from the same frames: the first sweep
  point shows a red "+1.8 V" error for ~1 s while VBUS is still at 5.1 V (the step down from the 5 V return), then passes.

- **`K2_PDSTRESS=1` does not pass the gate: `ChargerTestSession::poll` needs 1056 B of stack** (2026-11-14, found on
  k2/bugbatch; pre-existing: the base tree gives the same warning). The PD Stress build pulls more inlined code into
  `poll`, over `FRAME_MAX` (1024 B), so `-Wstack-usage` fails `make gate K2_PDSTRESS=1`; the default build is not
  affected. Fix with the tool's return: split the frame (a `noinline` helper for the stress branch), then the PD Stress
  row above can be validated.

- ~~**The Bench K2 row says "HID on / WebUSB on / HID off"**~~ obsolete 2026-11-21: the page ring replaced the Bench, so no screen shows that row (`bench::row` is called only from tests).

- **`test_ui_mockup` renders before the budgeted paint has settled** (2026-11-14, host tool). Its `render()` ticks twice
  and dumps, so pages that paint over several passes come out partial (`home_diag_*`, the USB link page with leftover
  rows). The new `*_after` renders settle on `Ui::painting()`; the others still need the same loop.

- **VOOC wake burst blocks the main loop 24 ms** (2026-11-12, k2/svooc-trigger; known, left as is by decision). The
  guarded SuperVOOC trigger (`src/fastcharge/svooc_trig.cpp` `wake()`) plays the 80 x 300 us D+ burst through
  `dpdm::play`, unmasked but blocking, the same as the engine's `vooc_wake()` (`src/fastcharge/fc_engine.cpp`). For
  those 24 ms nothing calls `fusb302::service()`, the guards do not sample and the LCD idle hook does not run, against
  the "never block the main loop over ~1 ms" rule. It runs only at the start of a try (up to 3 per run), with D- released
  and VBUS at 5 V. A fix would play the burst from a timer or as main-loop steps.

- **PD Stress "softreset" unplugs the charger** (2026-11-12, Xiaomi 20K bank on 1.0.0-670; parked with the tool, which is
  compiled out by default since the same day: `K2_PDSTRESS=0`). `fusb302::sink_soft_reset()` -> `Sink::soft_reset()` queues
  the Soft_Reset and sets the sink Idle, not held, at once. The demand block in `fusb302::service()` then sees no
  operation: `g_tx_allowed` drops in the same tick, so `transmit()` refuses the queued Soft_Reset (no Reset/TX record in
  the Black Box), and `kCrcLingerMs` (300 ms) later the Rd gate takes Rd off (`SWITCHES0 <= 0x08`); the source sees a
  detach and cuts VBUS. Black Box `runs/ps-review/xiaomi-full/bb.txt` at 796321 ms. Fix when the tool comes back: count
  the sink's soft-reset wait (`sr_wait_`, 400 ms to the new caps) as in flight in that demand, with a host test that
  the TX goes out and Rd stays. The unit hold's Soft_Reset (`fusb302.cpp` unit hold) keeps its own Rd and is not
  affected.
  **Fixed on k2/bugbatch (bcc533f9), host-tested, not seen on hardware:** `Sink::soft_reset_pending()` (`sr_wait_`, or
  the Get_Source_Cap fallback still waiting for its answer) counts as in flight in the demand, so the TX keeps its
  permission and Rd stays (`test_stress_soft_reset_goes_out_and_keeps_rd_when_the_source_answers`,
  `test_stress_soft_reset_to_a_silent_source_asks_for_the_caps_with_rd_kept`). Validate (hardware, a `K2_PDSTRESS=1`
  build, which first needs the frame fix below): the softreset sequence on the Xiaomi 20K bank keeps VBUS, and the Black
  Box shows the Soft_Reset TX and the new caps.

- ~~**The D+/D- passive idle put QC chargers into HVDCP and let VBUS jump to 12 V**~~ closed 2026-11-14 (2026-11-11, CUKTECH 10 GaN 120W
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
  (`APPLE 2.4A`).
  **Hardware 2026-11-14, 1.0.2-3-8fbf3624, OnePlus SVOOC charger** (`runs/dpdm-svooc/RESULT.md`): `[FC] idle dp=hiz
  dm=hiz mismatch=0x0` logged (every 10 s); `dm 0`, 200 ms, `dm hiz` kept VBUS at 5.02 V; VBUS 5.024-5.025 V for
  3 min (18 polls); the released pair reads D+ 34 mV, D- 32 mV on this charger.
  **Hardware 2026-11-14, Anker IQ4** (`runs/dpdm-svooc/iq4-qc.txt`): QC2 5/9/12 V and QC3 5→12→5 V pass (10 pass,
  0 fail), D+/D- at hiz throughout, VBUS back at 5 V; the full default run on 1.0.2-19-bb1a3234 passes QC2/QC3 again
  (`runs/ct-merge/ct-iq4.log`). Closed.
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
  parked by the user. On 1.0.0-594 (PC-powered full run) SCP 10 / 12 V were skipped `PD active: use QC port`, so no cut.
- ~~**FCP 12 V not released on the IQ4**~~ closed 2026-11-21 (2026-11-11, seen once, PC-powered `--units fcp,scp`: VBUS stayed at 12.1 V after the D+/D- reset). Not seen again after the D+/D- idle fix: FCP 5/9/12 V passed and the run went on to SCP on the IQ4 on 2026-11-14 (`runs/ct-merge/ct-iq4.log`).
- ~~**QC2 / QC3 fail on the IQ4 above 5 V**~~ fixed by the D+/D- idle fix (k2/idle-dpdm): on 1.0.0-594 (PC-powered,
  `runs/2026-11-11-iq4-idle/`) `--units qc2,qc3,fcp` 13 pass / 0 fail, and the full run 27 pass / 4 fail / 26 skip with
  QC2 5/9/12 V, QC3 5→12→5 V, QC3B 5/12 V all pass. History: (2026-11-11 self-powered full run: QC2 9 V `voltage not reached`, QC3
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
  **Hardware 2026-11-11, 1.0.0-594-cd569683:** inline false in 60 of 60 samples over ~8 cuts; `ct_start.py --units
  qc2,qc3` started from the PC and passed (10 pass, 0 fail; `runs/2026-11-11-integ594-hw/` in the integ worktree).
  **Resolved by policy 2026-11-15 (k2/svooc-120w, host-tested, hardware check pending):** the refusal came back on this
  port with the far-Rd latch. Finding: with the top port empty the K2 reads about 1.75 V on CC2 (its own level, the
  FUSB302 measuring the unused line), and the port's ~7 s no-load cuts kept the latch from clearing
  (`runs/cuktech-usba/cc-flip.txt` in the main checkout: `inline: True inline_cause: far rd` through every cut, VBUS
  5.1 V <-> 2 V, cc2 1.76 V / 3.3 V). The user's decision: the latch keeps only 100 %-certain evidence (a load, a sink's
  Request, a foreign contract); FarRd / Vbus / Rise no longer latch or refuse, and the charger test no longer refuses on
  them. Accepted tradeoff (the user, 2026-11-15): a K2 inserted into a live but silent link (no current, no sink message
  yet) is no longer protected; the START ticket's hint "other port must be empty" is the only reminder (the Review band was dropped 2026-11-21). Validate: this port, C-to-A
  cable, nothing behind the K2: `ct_start.py --units qc2,qc3` starts with no "load attached"; a phone behind the K2
  still latches (`load` / `sink request`).
  Open: the phone-behind negative check.

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
  **Hardware 1.0.0-600-0c0eb0ae (same port and cable, PC-powered, `runs/2026-11-11-integ600-hw/`): passes.** 60 s poll,
  346 samples, VBUS sagging to 4.04 V in 166 of them: all `APPLE 2.4A`, 0 `USB DATA` (`cut60.txt`; VBUS never reached 0
  this time, so no `NO INPUT`). `dm 0`, 200 ms, `dm hiz`: VBUS stayed at or below 5.12 V, label `APPLE 2.4A` throughout,
  never `AFC FCP` (`idle_check.txt`). Still open: a real data device behind the K2 reads `USB DATA`, and a QC / AFC unit
  still detects.
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
  refuses: 41 ms with the loop stopped, two captures (VBUS dip + detach) per cut. (1) The stall: fixed on
  `k2/card-256` (106f33d2), hardware check pending: `append` refuses before it compresses when the reserve cannot be met
  and it has no capture of its own to drop, and `fs::Fs::used_blocks` is cached until the next write, so a refusal reads
  no flash (`tests/test_bbstore.cpp`, `tests/test_bbpersist.cpp`: a refused capture is not retried by later polls).
  Validate: loop_survey on the auto-off port shows no 41 ms pass and `[BB] ... last=` near 0 ms on a refusal.
  **Hardware 1.0.0-606-d3bc07ea (CUKTECH USB-A, cutting): passes.** `[BB] skipped=12..19 last=2-3ms max=3ms`; Home
  PD-service gap max 87 ms -> 8 ms, gaps over 10 ms 8 -> 0 (`runs/loop-survey/v1.0-606-gd3bc07ea/`). (2) The
  space: open, decision pending. A 256 B inline card file does not solve it (64 inline cards still take 54-74 blocks
  after re-tests, the metadata pairs split); the caps sum to 215 of 90 blocks (docs/ARCHITECTURE.md "Volume budget",
  `tests/test_volume_budget.cpp`). Options: several cards per file, or lower caps. Also fixed there: v8/v9 card files
  (212/228 B) were not listed (b4c391fc); `cards list` may show more cards after the update. **That fix has a
  regression on 1.0.0-606:** 69 card files, and the index takes the first 64 in name order, so five of the newest cards
  (high-hex fingerprints) drop out of `cards list`, and three ids (53, 57, 58) are duplicated (they were reused while the old
  files were invisible); `[BB] free=0`. Nothing deleted. **Fixed** (fa248b6f, merged 3df20b8f): the index keeps the 64
  newest stamps and a duplicate number goes to the older card; on 1.0.0-611, 64 cards, no duplicates, #53/#57/#58 kept.
  All cards were then exported (outside the repo) and wiped from the K2 on 2026-11-12.
  `runs/2026-11-11-integ600-hw/cards-{before,after}.txt`. Evidence
  `runs/loop-survey/` (integ worktree).
  **The space, part one: the volume grows from 90 to 145 blocks** (2026-11-12, `k2/fs-grow`, merged 9fcfcc71;
  **grown on hardware, 1.0.0-617:** `[BBSTORE] free=229376` = 112 free blocks, past the old 90; the run's pages and
  the kept pages are byte-identical to the 2026-11-12 snapshot, since only the superblock in blocks 0/1 changed. The
  `[FS] grown` line fell before the start of the log ring the stream reads back. Checks 4 and 5 below are still open). After the legacy wipe, `fs::grow()` proves the 55 pages 0x080E4000..0x080FF800 blank, writes the intent
  `/.grow`, opens that run in the flash guard and runs `lfs_fs_grow`; on any non-blank page it stays at 90 blocks. The
  kept pages 0x080E3800 and 0x080FF800 are refused by every erase and program, the factory wipe included; a power cut
  leaves 90 or 145 blocks, never a format (`tests/test_fs_grow.cpp`: 976 cut points). 64 cards + the 20-block journal
  reserve + 32 Black Box captures fit 145 blocks; every store at its cap still does not (161 blocks before the reserve,
  ARCHITECTURE "Volume budget"). Validate (hardware): (1) the first boot logs `[FS] grown 90 -> 145 blocks used=N`,
  later boots `[FS] mounted blocks=145`, and no `[FS] new volume ... formatted`; on a unit with foreign data in the run
  the boot logs `[FS] grow skipped: 0x080XXXXX not blank` instead and nothing in the run changes; (2) `[BB] free=` grows
  by about 112 KB (55 blocks of 2 KB) against the same boot before the update; (3) `cards list` shows the same cards
  as before; (4) the Black Box saves again (`[BB] saves=` counts up after a VBUS cut, `entries>0`); (5) a factory wipe
  (0x33) leaves the kept pages untouched: `k2_flasher.py backup` before and after, the bytes of 0x080E3800 and
  0x080FF800 match, and the volume comes back at 145 blocks.
  **The space, part two: report cards and Black Box captures move to ring logs** (2026-11-12, `k2/ring-log`, built and
  host-tested only). littlefs keeps 111 blocks (settings, prefs, usbcfg, emk, twins, the charger and PD Stress journals,
  transcap, log snapshot: 98 blocks at every cap, 11 free); the rest of the run holds a 22-sector card ring (reserve 4) at
  0x080EE800 and a 12-sector Black Box ring at 0x080F9800 (resized 2026-11-12, the newest 16 listed) (ARCHITECTURE "Ring logs"). No save erases; ring maintenance
  erases one sector at a time only with no charger test, PD Stress run or port owner and 1 s without PD traffic; a save
  with no room is refused and logged. A 145-block unit (this K2 since 1.0.0-617) moves into the layout once at boot, in
  place: cards and captures are dropped, everything else is kept (`tests/test_fs_migrate.cpp`, a cut every 7th of ~480
  operations plus second cuts; `K2_FULL_SWEEP=1` cuts at every one). Export the cards first (`k2_remote.py cards export`).
  Validate (hardware): (1) the first boot logs `[FS] ring layout move done: ... err=ok` with `passes=1` or `2`, then
  `[FS] mounted blocks=111` and `[RING] cards: live=0 blank=22 dirty=0  bb: live=0 blank=12 dirty=0` (the move
  erases the ring range; only a power cut right after the shrink leaves dirty sectors, erased later while idle); later boots log no move line and `[FS] mounted blocks=111`;
  (2) `usb.cfg`, prefs, orientation, the saved cables (`emarker` library list) and twins are unchanged against a
  `--json` dump taken before the update; (3) a charger test saves its card (`[CARD] save #1 ...`), `cards list` shows it
  after a reboot; delete it and reboot: gone; (4) the Black Box saves after a VBUS cut (`[BB] saves=` counts up), and
  after a reboot the capture is listed as saved; (5) during a charger test no `[CARD] ring maintenance` or `[BB] ring
  erase` line appears between START and the end of Releasing, and the loop survey shows no erase stall in that window;
  (6) a factory wipe (0x33) erases both rings (`[RING] ... live=0` after it) and leaves 0x080E3800 and 0x080FF800
  byte-identical (`k2_flasher.py backup` before and after); (7) a unit whose kept files do not fit 111 blocks logs
  `deferred` and keeps working at 145 blocks with no cards or captures stored (host-tested only; no such unit on hand).
  Hardware 2026-11-12 (1.0.0-651-22bd3fee, integ with pdstress-ui and usb-enum): (2) twins, saved cables, settings and
  the store list identical to the dump taken before the update (`k2-flash-snapshots/2026-11-12/before-ring-639`, cards
  exported there first; the 3 old cards were dropped by the move as designed); (3) a PD charger test on an Anker IQ4
  saved `[CARD] save #1`, listed after a reboot, deleted and gone after the next reboot; (4) three captures saved after
  the run (7 ms each) and listed after a reboot; (5) no ring erase between START and the end of the run (`[BB] ring
  erase` only while idle afterwards). (1) not seen: the `[FS]` and `[RING]` boot lines were dropped from the binary log
  queue before USB came up (`[USB] drop=94`), so the move is proven only by its result. Open: with PD Stress results on
  the volume, every store at its cap leaves 2 free blocks of 111, below the 8 of `fs_migrate::kMarginBlocks`
  (`tests/test_volume_budget.cpp:210` fails; sizing decision pending).
- **PD Stress did not release Rd at its end** (open since before 2026-11-11; fixed on `k2/small-fixes`, hardware check
  pending): the run ended with the K2's Rd still on CC, the one a pd-reread presented and the run took over. Every end of a
  run (done, cancelled, a stop that skips the 5 V release, the reboot budget spent) now ends in `PdStressSession::on_finish`
  with `set_run_owner(false)` and `rd_for_run(false)`, the charger test's release path (a self-powered boot's own Rd pin
  comes back inside `rd_for_run`). Host tests: `tests/test_ui_pdstress.cpp` `test_a_*_gives_the_rd_back`.
  Validate (hardware): `k2_remote pd-reread`, start a PD Stress run, let it finish and again cancel one; afterwards CC1/CC2
  show no K2 Rd (charger-only attach: VBUS drops to 0, cc ~3.3 V) and the log has `[PD] Run start over: Rd off`.
  Hardware 2026-11-12 (1.0.0-651): NOT fixed for a run started from a pd-reread. The re-read's Rd is a sink operation
  (`[REREAD] attach taken over: Rd 1, contract 1 stay with the new owner`), not `rd_for_run`, so `on_finish`'s
  `rd_for_run(false)` is a no-op (no `Run start over` line); after done and after cancel alike the Rd stayed 30 s, at
  5 V, until `[PD] Contract released: Rd off, unused for 30 s`.
- **A refused e-marker read kept the last read's mode and acks** (2026-11-11, fixed in 472c40f9, host-tested;
  `tests/test_fusb302_scan.cpp` `test_refused_read_clears_the_last_read`). Validate (hardware): with a sink inline or
  Ra mode on, `k2_remote emarker --json` after a refused read shows `read_mode` none and 0 acks.
- ~~**Docs: `docs/CHARGER_QUIRKS.md` called the Baseus cable of the FH22 / CUKTECH notes a "plain cable"**~~ completed: reworded on `k2/small-fixes` (pattern `cable-chip-silent-alone`); the remaining "plain cable" at CHARGER_QUIRKS.md:509 is the A-to-C cable of 981c831, another cable.

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
  **Second shape: the base is a flat overshoot PLATEAU** (2026-11-15 UGREEN QA, k2/qa-fixes, hardware check pending): the
  same row 41 passed in run 1a and failed "steps ignored" (detail 0) in run 1b. The base request overshoots to 13091 mV and
  holds it flat ~540 ms, then drops to ~13010 mV and creeps up; the walk's earliest base read (~640 ms after the Request:
  500 ms stable hold + the steady window) lands inside the plateau, so the base is 80 mV high and the five +20 mV steps add
  up to ~0 (the later levels dwelled 2.54 s, 2.54 s, 0.98 s, 0.58 s, 0.58 s: the steady window exits early whenever the
  output is flat, that is by design). No dwell fits every plateau (row 35's held the same level over 1.1 s), so
  `Runner::walk_step` re-bases once on the signature: the first step's level a full quantum below the base
  makes that level the base (`walk_rebased_`, one extra step). Host tests: plateaus of 300, 540, 900 and 1400 ms pass,
  a source that ignores the steps still fails "steps ignored".
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
- **Both Rd for 0.13 ms at SW_RES** (2026-11-09, pinned path fixed in a605db3; plain-boot path fixed 2026-11-22,
  hardware check pending): the FUSB302 soft reset leaves the chip default `SWITCHES0 = 0x03` (both pull-downs) for ~0.13 ms
  until `SWITCHES0` is written again. On a pinned boot `init()` and `detach_reset()` use `PD_RESET` only; on a plain
  boot (`g_pin_rd == 0`), `init()` previously closed `PC14`/`PC15` before `SW_RES`, briefly exposing `0x03` onto the CC
  lines.
  - Fix: pinned/comeback boots keep `PD_RESET` only; plain boots now open `PC14`/`PC15` (`cc_switches(false, 0)`) before
    `SW_RES` and re-close them only after `wr(kSwitches0, 0)` clears both `PDWN` bits (`src/drivers/fusb302_cold.cpp`).
    `park()` (DFU teardown only) is unchanged.
  - Validate: host fusb302 scan tests asserting no `SWITCHES0` value with both `PDWN` bits on a pinned boot
    (`test_pinned_boot_never_soft_resets_so_no_rd_pair_at_sw_res`, green in a605db3) and no `PDWN` bit while `PC14`/`PC15`
    are closed on a plain cold or warm boot (`test_plain_boot_opens_cc_switches_during_sw_res`, not verified on hardware);
    `k2_remote bootinfo` notes on hardware show no `0x03`.
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

- **String packing (BPE-157 + `k2_strpack` section, 2026-11-24)**: replaced the parked `k2/strpack` dictionary+Huffman pilot (`PStr`/`Txt`, ~1.7 KB fixed overhead, ~140 cycles/char, +3.8 B/str callsite bloat) with byte-aligned, NUL-terminated recursive BPE-157 (`include/k2/core/strpack.hpp`, `src/core/strpack.cpp`, `include/k2/core/strpack_table.hpp`, `tools/strpack.py`, section `k2_strpack` in `link.ld`). `K2_P("...")` packs at compile time (optimal DP with first-byte bucketed rules, round-trip `static_assert`) and returns a plain `const char*`; low-level sinks (`gfx_text`, `gfx_text_rot`, `text_width`, `badge_text_bounds`, `fit_*`, `StrBufBase::put`, `Toast::show`, `diag::Out::s`/`cap`, `shell`/`shell_hw`, `fmt::vformat`, `logfmt::vencode`, `binlog`, `TextBuf`) detect packed pointers via `[__start_k2_strpack, __stop_k2_strpack)` (zero `Txt`/callsite type churn, ~24–28 cycles/char from Thumb-2 disassembly, 320 B table). Converted **1,362 unique literals** (`25,208 B` plain -> `15,094 B` packed + `320 B` table) across `diag`/`diag_screen`, `ui/text`, 19 UI `.cpp` files, `shell`/`shell_hw`/`lab`, `report_card_json`, `emjson`, `charger_test`, `drive`, `pd_interrogate`, `pd_sink`, plus trimmed `_sub_I_65535_0.0` (`2,064 -> 1,624 B`, `-440 B`) via `K2_BSS_ZERO`/`K2_BSS_INIT`: total net flash **`-5,556 B`** vs `7a3d0084` (`716,840 -> 711,284 B`, **`7,564 B` free** below `0x080B3000`; `303fab66` `-220 B` + `cc50dba3` `-4,380 B` + batch 3 `-956 B`; gate-verified including GCC & Clang ASan/UBSan, not verified on hardware).

- **Specs & offers: read a cable-limited EPR charger with the spoof** (2026-11-24). A Baseus 140W on the top plug (no cable) claims PDP 140 W but offers only 3 A (5/9/15/20 V, PPS 5-11 V and 5-21 V), so the page shows the bare-plug list, not the charger's capability. When PDO1 has the EPR bit, nothing is offered above 3 A, the PDP claim is well above the best offer (`cable-limited-offer`) and the K2 owns the contract (no device inline), the auto-read / Refresh arms the EPR e-marker spoof and does one Hard Reset re-read; the page marks the list as read with a spoofed cable. Inline: unchanged (do not disturb). Waits for flash headroom.

- ~~**Charger test "Offers" depth**~~ removed (user decision 2026-11-21, k2/test-layer-reads): the Offers level (3b66351c, dc9a2eda,
  f3693009) and its cable rule are gone; the Depth row is Light, Medium, Deep again. Light plus the PD pick lists the offers.
  Wire value 3 stays reserved: an old card/journal that carries it decodes (card "Old", journal replans as Medium).
  Inline rule (every level): with the inline latch set START is refused ("load attached"), a latch during START's wait
  cancels it and puts Rd and identity back; nothing transmits, presents Rd/Ra, turns on VCONN or enters EPR.
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
- **Battery efficiency recorder**: merged (k2/battery-eff, 2026-11-13), built and host-tested, NOT hardware-verified.
  Spec and behaviour: `docs/BATTERY_RECORDER.md`. (User decisions 2026-11-11 and 2026-11-12: three modes Charge /
  Discharge / Round trip, wirings same port or moved, running in the background; since the ring (2026-11-20) its screen is
  ring page 9 "Battery".)
  - Per phase Wh and mAh in and out plus time; Round trip efficiency = Wh out / Wh in. Its own signed integrator
    (`k2/battery/battery.hpp`), so the same-port discharge counts the reverse flow; `EnergyMeter` is unchanged.
  - Phase end: under 50 mA for 60 s (settable, RAM-only) or by hand; a 60-point downsampled V/I curve; a 246 B record.
  - The newest 3 runs on flash (`/bt/<id>.rec`, the `/bt` pair, 2 blocks; root files of older firmware move there at
    boot), written at phase boundaries and
    every 10 s while a phase measures (in place, `Fs::rewrite_file`; record v2, 250 B); a warm reset resumes from the
    `.noinit` checkpoint (preferred), a cold boot resumes the measuring phase from its last save (resumes + known gap
    seconds stored; the off time and up to 10 s before the cut are not measured). An end/finish is final once written.
    Built and host-tested on k2/battresume (932a34e7, 2026-11-24), NOT hardware-verified. Own directory
    (k2/battresume, 2026-11-24): 72 erases/h of the /bt pair, ~380 B/save, root never erased, 24 h: 5 blocks levelled,
    no superblock expansion. K2_PDSTRESS=1 builds with every store at cap are now 2 blocks short (only /jr misses).
  - Shell `k2_remote.py [--json] battery status|start|end|next|finish|config|list|read|continue` (CMD_BATTERY 0x58), QR share
    `BATT`, `k2_remote.py page battery`.
  - Continue (user 2026-11-25): `Continue run` (arm + confirm) under an ended run's facts and in Saved runs > the newest
    run, `battery continue [id]` (wire op 8): reopens the newest run's last phase, totals kept, one resume + gap, end rule
    disarmed until the current returns. k2/battcont (104bd71a, a8354d65), host-tested, NOT hardware-verified. Validate:
    end a charge by hand, Continue, Wh in keeps growing from the old total; power-cut after a continue resumes it.
  - Validate on hardware: a short charge of a power bank from the bench charger (Wh in against the Home Wh counter),
    a same-port round trip into a resistive load, a warm reboot mid-phase (`[BATT] boot resumed`), a power cut
    mid-phase (`[BATT] boot continued`, totals within 10 s of the cut, `battery read` shows the resume), heartbeat `stack_free` with the Battery page open.
  - **Battery efficiency test** (planned 2026-11-18, not run yet; no record exported). A real bank, two runs:
    1. Discharge, wiring moved: the full bank (10K / 100 W) on the K2 input, the laptop on the output (forward = out).
       Load: `runs/battery/load-loop.sh` (the sanitizer pass in a loop; `touch runs/battery/STOP` ends it). The phase
       ends by itself when the bank stops (|I| < 50 mA for 60 s); the K2 writes `/bt/<id>.rec`.
    2. Charge: the bench charger on the input, the emptied bank on the output (forward = in), until the bank stops.
    - Efficiency = Wh out (run 1) / Wh in (run 2), against the bank's rated Wh. An inline meter (UC96) on the HID cable
      checks the K2's own draw.
    - Export each run as soon as it ends: `k2_remote.py --json battery read <id>` into `runs/battery/` (the flash keeps
      only the newest 3 runs). Pass: both records complete (not `Cut short`), Wh within the meter's tolerance of the Home
      Wh counter, no reboot, `stack_free` above its floor.
    - Run 2026-11-24/25 (runs/battery): discharge id 3 (30.994 Wh out, v1 record with the energy in its "in" column),
      charge split over ids 4 + 5 (36.880 Wh in), a later charge id 6 (19.404 Wh). Separate runs, so no round-trip
      efficiency was shown.
  - Efficiency of separate runs (user 2026-11-25, k2/batteff): Saved runs heads `84.0%  #3-5` / `30.99Wh out / 36.88Wh in`
    (view::pair: the newest charge or discharge stretch, runs summed, against the other kind's stretch before it; a round
    trip ends the scan, a cut-short run voids it, empty runs are skipped); `battery list` / `read --all` add
    `battery_pair`. Version 1 records read each phase's energy from its own column (the pre-05313146 swap). Synthetic
    host test `test_battery_synth` (closed forms, real-curve replay within 1.5 %). Cut texts fixed: Saved runs note,
    measuring identity line, round trip between line (fit_data cuts were not failing the walk). Host-tested, NOT
    hardware-verified. Validate: `battery list` on the K2 with runs 3..5 kept -> 84.0 %; Saved runs top in all rotations.

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
  - Status 2026-11-18 (k2/idle-power, built only): PB7 is TMR4_CH2 PWM (inferred from the F1-family pin map; check that a
    brightness step changes the K2's own draw), Settings > Display > Brightness in 10 % steps, `k2_remote.py power` (0x37).
- **Screen timeout (powersave)**.
  - Do: Settings "Screen timeout" Off / 15 s / 30 s / 1 min / 5 min. After it, dim to a low level, then backlight off
    after a further period (document the choice). Any button wakes the screen and that press does nothing else. A running
    charger test only dims, never turns off.
  - Validate: host tests of the timeout state machine (dim, off, wake, the wake press swallowed, a running test never
    off); hardware: each timeout value observed, START is never pressed by a wake press.
  - Status 2026-11-18 (k2/idle-power, built only): `k2::idle` machine Active -> Dim (30 %, 60 s) -> Off (300 s), host-tested.
    With VBUS present it stops at Dim. Off = panel sleep, painting stopped, AHB /4 (60 MHz; /16 is excluded, APB would fall
    under the inferred USB minimum). Settings > Display sets brightness, Dim after and Screen off after; the dim level is
    remote-only. Remote 0x37: `k2_remote.py power [--brightness N] [--dim-brightness N] [--dim-after S] [--off-after S]
    [--force auto|active|dim|off] [--hclk auto|1|2|4|8] [--wake-on-remote on|off]`.
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

- **Who's attached: one survey owner, run on Refresh** (decided 2026-11-09: active queries only on Refresh; since 2026-11-23 one
  exception, the attach's auto read, see Status).
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
  - **Auto read on attach** (2026-11-23, branch `k2/auto-read`, **built, not verified on hardware**): once per attach,
    800 ms after Ready, only when the K2 owns the contract (it sent the Request; sink Ready, caps held this attach, no Xiaomi
    chain pending) and `Ui::auto_read_block()` is clear (no inline latch, tool, spoof, Ra mode, legacy or identify run). It
    runs the survey's Wire and EPR steps: Discover Identity, SVIDs, Modes, Source_Cap_Extended and Manufacturer_Info (PD 3),
    EPR_Get_Source_Cap in SPR mode. No Get_Source_Cap, no cable read, no reset, no EPR entry, VBUS never moves. The Charger
    tab rows fill as answers arrive. Load current or a foreign Request (inline latch) or a tool starting
    stops it at once; the rest reads `Inline: blocked` / `Charger test`. A skipped attach (passive link, tool, inline at the
    first Ready) keeps `Not asked` and the hint `Not asked: Reset & re-read`. A read cut by a session change blocks the next
    attach's auto read for 5 s (loop guard for a charger that resets on the questions; its behaviour is unverified).
    Pure `pd::AutoRead` (`test_pd_survey`), glue `core/survey.cpp`. Validate on hardware: plug a PD charger (K2 owns the
    contract): the Charger tab fills and shows the vendor, one pass per attach; plug a load behind it (inline) and confirm the
    read stops; a charger test started mid-read aborts it; a Reset & re-read press mid-read waits for it (Busy, as for any
    survey); no VBUS dip in the black box. The twin-rail chrome paints no bottom bar: the progress and the summary show as a
    toast (`Reading charger n/m`, then `Read: ...`), the rows show the words (`Asking...`, `Not asked`); the
    `Not asked: Reset & re-read` hint lives in the (unpainted) status line only.
  - **Wake a sleeping charger by itself on the Charger overview** (2026-11-23, branch `k2/wake-page`, **built, not verified on
    hardware**): seen with the K2 on the PC (HID) and a PD charger on the top port after a K2 reboot: Rp on the line, VBUS
    held off (standby signature), no Rd of ours (Wake sleeping port OFF), so no contract and no auto read; the Charger tab
    read `Ready / Not asked` until Reset & re-read. Now, with the Charger tab of the overview open and the debounced
    `PortSense::SourceAsleep` confirmed by the facts of that tick, and held 600 ms, the page runs Refresh's path once (`Ui::rescan_source()`:
    the CC cycle plus the survey hook; toast `Waking charger`); the woken attach is a new session, so the auto read runs
    once on it (`Reading charger n/m`). Only on that tab and view, never while `Ui::auto_read_block()` holds (inline, tool,
    spoof, Ra mode), not in Actions / Spec / other tabs. No loop: a wake that gets no VBUS is not retried until the port
    state leaves asleep and returns at least 15 s after the try (a return sooner is the cycle's own blip, swallowed), or the
    page is opened again (3 wakes in a row without a charger attaching, then only a reopened page); a manual Refresh / Reset & re-read takes the episode too. Global idle behaviour and the Wake
    sleeping port default are unchanged. Pure `conn::AutoWake` (`test_ui_connection`), page glue
    `ConnectionPage::auto_wake` (`test_ui_connection_page`). Validate on hardware: the setup above, reboot the K2, open
    the Charger overview: `Waking charger` then `Reading charger n/m`, one wake and one read, vendor shown; a charger that
    stays dark gets one try (no repeated cycles in the log: one `source asleep: waking it` line per episode); a load behind
    the K2 or a charger test blocks it. Open: a real replug within 15 s of the try is swallowed (reopen the page); a sub-page pop re-arms the wake (`enter()`);
    the CC cycle blocks inside one paint tick (as Refresh does); a refused rescan stays silent and spends the episode;
    the single-read after the woken attach is by reading only (no automated test); skipped while the direct-plug line trial is live.
    **Superseded in part by `k2/wake-any` below: the CC cycle this entry used (`rescan_source()`) cannot wake a power bank in DRP idle.**
  - **Wake anything dark on the Charger overview; Rd instead of a CC cycle** (2026-11-23, branch `k2/wake-any`, **built, not
    verified on hardware**): the Baseus 100 W bank idles at VBUS 0, CC1 8 mV, CC2 1.55 V, D+/D- 1.61/1.69 V: no standby
    signature, so it was `NoCharger` and never woken; and the wake-page wake (a CC cycle) could not wake it anyway, while the
    remote's `pd-reread` (`fusb302::reread_start`: presents Rd, then reads) wakes it every time. Now `conn::wake_candidate`:
    VBUS dark with a monitor, no inline / sink evidence, `NoCharger`, `SourceAsleep`, `ChargerWaiting` or `DualRolePort` (the bank idles as a DRP: our Rd makes it source) (so an empty port gets one try too: the
    empty bias net reads CC2 1.64-1.71 V, no level separates it from the bank), no idle Rd hold of Wake sleeping port on a line (`fusb302::idle_wake_line()`), and `Ui::auto_read_block()`
    free. `Ui::wake_source()` runs it: no contract + VBUS dark + not inline = `reread_start` (Rd) + the survey hook, else
    `rescan_source()`; every "wake + read" caller takes it (`Action::PdRescan`: the page's auto wake, Refresh, Reset & re-read, Tools Reset link, the remote's `action 1`, the Hard Reset fallback); `rescan_source()` is left only for a lit rail, a live contract (Get_Source_Cap), inline and a driver refusal. A K2-sent sink Hard Reset on a
    Ready contract arms `pd::HrRecovery` (`Ui::hr_recover_poll`): VBUS still dark after 1.96 s (tPSHardReset + tSafe0V + tSrcRecover +
    tSrcTurnOn; 2.39 s out of EPR) = one `wake_source()` (a bank answers the reset by dropping VBUS and sleeping). Inline untouched.
    One wake per page open, plus one per replug: `AutoWake` re-arms on an attach edge (empty port, then a waiting Rp / DRP toggle /
    standby signature / lit rail) seen outside the K2's own Rd, re-read attach + linger and a 3 s settle, so the release of a woken bank is
    no replug; at most 3 auto wakes per page open (a port that flips by itself). The survey's auto read waits while the driver's own re-read runs (`AutoIn::reread_running`). Tests: `test_ui_connection`
    (`wake_candidate`, `AutoWake` replug / own release / cap), `test_pd_passive` (`HrRecovery`), `test_pd_survey` (deferral), `test_ui_connection_page` (bank,
    empty port, no loop, ring pass, inline, HR fallback, Rd path vs rescan, each caller), `test_fusb302_scan` (real driver: Rd after a
    Hard Reset that took the Rp away, empty port bounded and released). Size +348 B text, +16 B bss vs e3b96127. Validate on hardware: bank on the
    top port, K2 on the PC: open the Charger overview: `Waking charger`, the bank wakes (Rd), 6 PDOs read; empty port: one toast, `NO
    CHARGER`, no repeated Rd; bank idling as dual-role-port and a charger-waiting brick: one `Waking charger`, PDOs read; after a wake that
    worked, the ~30 s release brings no second toast; pull and replug the bank within 15 s: one more wake; Reset & re-read on a K2-owned contract with the bank: Hard Reset, VBUS drops, wake within ~2 s
    (`[UI] link reset: VBUS did not come back`), the read completes. Open: the survey's auto read may be refused while the re-read's
    own run is active (`[SURVEY] wire refused`; the re-read itself still fills the rows), the Rd made by the wake is released by the
    re-read, ~30 s after it; a bank that flips between an empty-looking idle (CC1 0 / CC2 1.55 V) and its toggle by itself reads as a replug (capped at 3 per page open); a source that keeps its Rp through the K2's Hard
    Reset leaves the attach held, so `reread_start` refuses and the fallback is the rescan's CC cycle (it recovers by itself: the
    recovery only fires on a dark rail); a bank swapped for one that idles empty-looking (no toggle, no Rp) shows no edge and needs Refresh.

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
  above; fixed in 64c0ba5, hardware check pending). **The default from 2026-11-14 (user decision, branch k2/pps-sweep) until the test levels, which moved it to Deep (below):** the
  session's default run swept every PPS APDO, split at 5 V: the part from 5 V up right after the APDO's own
  min / max / step rows (Pd, also self-powered), the part below 5 V in PdLow (external power); opt-out: the Tests list line, `k2_remote charger-sweep off` (Cmd 0x0F), `ct_start.py --no-sweep`. The AVS sweep below
  stays opt-in. **Since the test levels (k2/ct-level): Deep only.** Medium, the default level, runs no deep sweep; Deep runs the PPS and the AVS sweep (open decision: the brief's "Deep = Medium + PPS sweep" over the 2026-11-14 default). Order and opt-out: `docs/CHARGER_TEST_FLOW.md`. Originally opt-in, never in the default preset: every 100 mV across each PPS
  APDO, 1 A (AVS: the row below), Accept + PS_RDY + measured VBUS per point, results as a compact error-vs-setpoint
  strip on the results tab and the card (min / max / worst error and where, plus the peak delivered level and a
  fold-back: a steady out-of-band point is recorded, not a stop - "peak 19.75 V at 19.9 V, folds back to 19.42 V
  from 20.0 V" on the FH22; the fold rule is the shared `k2/pd/prog_track.hpp`, also the PD Spec `pps_track` / `avs_track` checks). Long by design (about 77 points for 3.3-11 V, about 180 for 3.3-21 V), so the Tests
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
  Superseded (k2/pps-merge, built and host-tested only): the Sweep row is gone; the APDO's range chip and its sweep are ONE chip
  (worst verdict, small dim "S" mark), on Running and Results; the Steps view lists both. Hardware check pending.

## Not verified on hardware

- **Ring browse shows no focus** (k2/ringfocus, 2026-11-25; host tests and renders only: runs/ringfocus/png/sheet_rot0..3). A ring page that is browsed, not entered, draws no focused row (no tint, bar, note or 40 px row) and no ActionCell outline (`kit::set_browse`, replaces the muted grey focus); OK lights the remembered row again. Leave/enter repaints the focused row and those below it only (Log 15.6k -> 37.2k body px: the rows below move 14 px). Validate: Tools, Battery, Log, K2 and Link in the ring and entered, all four rotations; scroll K2 down, Long Back, OK: the same row lights.
- **All text literals packed (K2_P) + gate step 8 string guard** (k2/strpack-all, 2026-11-25; host tests only). `tools/check_strings.py` fails on plain text literals outside K2_P, `// k2-plain: <reason>` marks or `tools/check_strings_allow.txt`; byte copiers decode packed text (remote wire text, vendor names, card sheet, footer words, row labels, cable names, QR records). Fixed: packed group_why reasons garbled on the card sheet. Check on the K2: Cards sheet Reason/Why rows, Who's attached footer words, Charger-test Results row labels and chips (cable, refused, needs load), PD trigger identity rows, remote `identity`, cable save names, shell output, `stack_free`; also PD trace rows, USB link text, MSC `STATUS.TXT` and `NATIVE/LIBRARY.TXT`, `k2_remote` diag, FC pill on Home/Detail, SVOOC ceiling and Xiaomi chain notes on cards, Memory page labels, `bootinfo`/`boottrace` lines. Text 708488 -> 706180 B (-2308). Still plain (allowlist): opt-in lab files (twin, PD Stress, loop probe, OPPO probe, selftest, net). UI chunk (dd5e27f8): src/ui + include/k2/ui off the backlog; Fields decode via put_field_buf (Lines D+/D- word, PD trigger state), QR text lines, flag names on Sink rows; check those, Display/Battery/Bench rows and refusal footers on the K2.
- **PD limit: voltage, then its current** (k2/ui-noicons, 2026-11-24; host tests and renders only: runs/ui-pdlimit-va). Fixed, PPS and SPR AVS: OK on a Max V row opens that voltage's Max A list (Max, 1 A up in 0.25 A steps to the offer current, the exact maximum last when off the grid; nothing above the offer); OK commits both, BACK returns to the voltages. Offer current: Fixed that voltage's object, PPS the highest APDO covering it, SPR AVS the 9-15 V field at 15 V and the 15-20 V field at Max; no caps keeps the old ceiling. A stored current over the new offer reads Max. Ceilings now validate at 10 mA (`k2_remote pd-limit` too); labels print exact amps. Validate: 65 W charger (20 V 3.25 A list ends 3A, 3.25A), a 100 W PPS charger, an SPR AVS charger; arm 15V/3.25A and check the forged list and the sink's Request. Hardware 2026-11-24 (077dd1d9, 100 W charger -> K2 inline -> laptop, `k2_remote pd-limit set --fixed`): 15V, 12V (no 12 V offer: the sink took 9 V), 9V, 5V and 9V/2A each forged the list (higher objects at 0 mA, current cut to 2 A) and the laptop re-requested inside it (RDO 0x238320C8 = 9 V 2 A); 20V restored a 20 V contract. The laptop accepts 15 V and below but draws 0 mA there (its own floor, not the limiter). `pd-limit off` leaves the limited contract in place until a replug or a new arm (no reset on off from the shell). Fixed on k2/limfix (7013ecd1, cb187260, ea4fa660; not verified on hardware): off (shell and UI, UI after its confirm) sends the inline Hard Reset whenever the sink still holds a forged list, a give-up or a far-sink attach with no inline latch included; no forged list out, no reset. A seeded re-arm to a limit the charger's list already meets sends that list once. Drain run (sink: a power bank topping up through the K2, per the user): 20V/2A drew at most 1.97 A, 20V/1.25A 1.26 A, 45 W -> 15 V 2.29 A, 30 W -> 9 V 2.96 A (27 W). 20V/3A drew 2.47 A after its arming Hard Reset. UI picker on hardware still to check.
- **No row icons on Tools and Settings** (k2/ui-noicons, 2026-11-24; host renders only: runs/ui-noicons/{before,after}). Tools (incl. the PD limit picker), Settings, Home boxes, Display and USB link rows are marker-less (text at x28, radio rows keep their marker); six unused icons deleted (32 -> 26). Flash -396 B. Validate: Tools, PD limit picker, Settings and its sub-pages in all four rotations; no leftover icon pixels at x8 on scroll.
- **Charger tab: PDP as hero, offer groups** (k2/charger-offers, 2026-11-23; host tests and renders only: runs/charger-offers/png). Who's attached > Charger leads with the rated watts (PDP, else the best offer) and the rows SPR, EPR, PPS, AVS (dim `none` for an absent kind, `15-28V 140W` for AVS); Fixed is renamed SPR, the PD, Ext and EPR rows are folded in. Also fixes the landscape drift of two-colour value tails (`kit::TextRun::dx`). Validate: a bank (SPR + PPS), a 140 W EPR charger (EPR + AVS rows), a PD 2.0 charger and a non-PD charger in rot 0 and 1; the hero equals the charger's rating; no row cut. Overview slice keeps its own best-offer hero.
- **Icons native + stable rail + 4-char titles** (k2/icons-rail; host-tested only, not verified on hardware; renders in runs/icons-rail only). Every icon (`draw_icon`, the Home link compositor, the rail link icon) is a native 12x12 square in all four rotations (a landscape viewport no longer stretches it); 16 of the 32 bitmaps are redrawn, 16 kept (redrawn: bolt, therm, warn, cross, phone, chip, ohm, clock, pc, plug, anon, rotate, keys, info, socket, sand), all centred within 1 px and symmetric where the subject is (round socket with two pins; checked by `tests/test_gen_icons.py`). The telemetry rail stays pinned to the physical bezel in every rotation (`rail_text_dir` matches `panel_text_dir`, `opp_reversed` reverses Left Ccw and Bottom rails, `kit_hot.cpp` `text_rev` keeps horizontal cell text left-to-right): title at the female-port end, three cells toward the male-plug end, and the link icon at the male-plug corner (`kOppSlotLen {54,62,62,62}`, `opp_slot_rect`). Page titles are 4 characters at most from one consteval table (`rail::Title`); the ring masthead is the page's own name (no `n NAME` prefix). Check on hardware: Home, Tools and a rail page in all four rotations (title and link icon stay pinned to the physical bezel ends, icon square and crisp, no cut text); the link icon (none, power, host, shell) is upright on a side rail too (`test_rail_link_icon_upright`).

- **PC-link icon** (k2/shell-toast; host-tested only, not verified on hardware; renders in runs/pc-icon). A 12x12 two-colour icon at the male-plug bezel end of the opposite telemetry rail's last cell (upright on every rail, side rails included; the cell's text centres in the rest, the "D+ " tag loses its space if needed). Home at depth 0 has no rail: the icon sits in the info block's row 1 after the elapsed time (elapsed moved 14 px left, protocol value capped at 14 characters, the armed-limit text drops the current, then the range's unit; left out when a long power-meter notice fills the row). Repainted only on a state change. States: none (no icon), power (VBUS present, not enumerated: socket, `kInk2`), host (`tud_mounted()`, suspended counts as not using: monitor, text ink), shell (host + a CONTROL remote command in the last 5 s, or a charger test / tool run the shell started: hourglass, focus-cyan sand). `pclink::Tracker` (core/pc_link.hpp): aux 4.0 V on / 3.5 V off hysteresis, unknown reading keeps the verdict. `k2_remote state` shows `pc_link: none|power|host|shell` (session part 1 byte 31, 32 bytes; mirrored in k2proto.js, k2_remote.py, k2.html; old firmware reads none). One table `kShellRules` in remote_proto.hpp classifies every command id (static_assert; presses, page, actions, charger-test start/stop, PD/FC and store writes, reboot/DFU are control; reads never). The key strip keeps its full labels. Check on hardware: unplugged = no icon; HID cable to a PC = monitor; a `k2_remote press` = hourglass within a tick, monitor 5 s later; ct_start keeps the hourglass to the end of the run; all four rotations on Home and a rail page. Open: the HID-cable loop shows power (the aux rail is the PD bus); pages that hide the strip (and the Black Box) get no icon; QR pages are not excluded; Log is classified as a read (its `[1]` toggle is not).
- **Sweep depth** (k2/sweep-depth, 2026-11-23; host tests and renders only: runs/sweep-depth/png). Settings row `Sweep depth` (200 mV default,
  100 mV, Phone, 20 mV) sets the charger-test PPS/AVS deep sweeps' step: Phone is 100 mV plus 20 mV points in 6.8-11.0 V and
  14.0-19.0 V (PPS only, once per APDO on the one with the most current); 20 mV every PPS quantum; AVS stays 100 mV; the APDO top is always
  a point. A Fold or Break is stepped back from once (100 then 20 mV points). A steady point closes after 100 ms (was a fixed 500 ms):
  about 520 -> 120 ms a point, 180 ms with an 80 ms slew; kWalkSteadyTimeoutMs stays the ceiling. Review and the Running header name the
  step; the Deep block of the card carries a 2-bit step tail (old cards decode as 100 mV; `sweep_depth` in report_card.py and deep.js);
  the journal meta byte carries it; remote `charger-sweep-depth` (0x5D) and `ct_start.py --sweep-depth 100|200|phone|20`. Commits 
  d47ba035..4ce75d9d. Validate: cycle the row; FH22 PPS 20 V fold found at 200 mV and located by the step-back; Pixel / OnePlus 15 Phone
  windows; share and web-card decode; point time on a steady charger. Open: old byte-based card readers misread about 2 in 8 new
  tailed cards; a sweep span under 6.4 V can have empty one-cell buckets at 200 mV (the viewer shows gaps).
- **Phone low window 6.8-11 V** (k2/phone-window, 2026-11-23; host tests only). The Phone depth's low 20 mV window starts at 6.8 V
  (was 8.0 V): a Pixel 11 Pro charging from flat asked PPS 7.0-9.2 V, 9.6-9.8 V when fuller. +48 points on the 5-11 V APDO.
  Not verified on hardware. Verify: `ct_start.py --sweep-depth phone`, the first 20 mV point is 6.82 V.
- **Huawei/Samsung switch** (k2/ct-huawei-switch; host tests and renders only). Settings row `Huawei/Samsung`, OFF by default, hides FCP, SCP and AFC from the charger test (presets, Custom, plan, step counts, Results); ON is the old behaviour; `ct_start.py --units fcp,scp,afc` (or `charger-units`) still runs them; `k2_remote charger-test view` reports `FCP/SCP/AFC shown|hidden` (view flag bit 15, set = shown; firmware older than this always reads hidden). Check on hardware: fresh K2 shows OFF and the Tests step counts drop, Results has no Huawei/Samsung rows; ON restores them; the choice survives a reboot. Open: no remote setter for Settings toggles; the preset name "Legacy only" now means QC/UFCS/OPPO/vivo while hidden.
- **Cable place: Read e-marker opens the chip details** (k2/cable-read-merge; host tests only). The Chip details row is gone; a good read (immediate or after the passive listen) opens the chip sheet, a failed one opens it too when a cable record is held, with "Failed to read" in the footer; no record stays on Actions. Check on hardware: Cable Actions has no Chip details row; Read e-marker with a cable lands on the sheet; a failed read with a held cable lands on the sheet with the error footer; inline (listen only) does the same after the 5 s window.
- **Zero-wait hot block expansion & cold compaction (Round 1/2)** (`abdaf82e`, 2026-11-24; gate-verified, perf-check pending on hardware). Expanded zero-wait FLASH block (`link.ld`, `K2_HOT_BLOCK_BEGIN..END`) from `83,364 B` to `189,628 B` (`0x08004434..0x080328F0`, `55,056 B` zero-wait headroom below `0x08040000`), pinning UI fonts/icons (`kIconBits`, `font_*`), `pd::Link`/`Passive`/`Limiter`, `lz`, `ctsweep`, `StrBufBase`/`mix`/formatters, `fastcharge`/`dpsniff`/`fctrig::poll`/`battery`/`ripple`, `lcd`/`kit`, bezel helpers (`opp_slot_rect`, `build_opp_cells`), and live page painters (`HomePage`, `ConnectionPage`, `DetailPage`, `LinesPage`, `RipplePage`, `GraphsPage`, `BatteryPage`, `ChargerTestPage::paint_running`). Moved `src/pd/pd_interrogate.cpp`, `src/pd/transcap.cpp`, `src/core/bbpersist.cpp`, and `src/core/rescan.cpp` to `-Oz` `COLD_CXX_SRCS` with only their hot poll entry points pinned in `link.ld`; re-enabled `-Wl,--sort-section=alignment`; moved `k2::mix` and `pd::diag::detail::Out` methods out-of-line (`[[gnu::noinline]]`), table-drove `pd::diag::code_name`, deduplicated `pg::mix_str`, and moved zero-initialized UI/charger-test singletons to `K2_BSS_ZERO`/`K2_BSS_INIT`. Swept 17 additional `COLD_SIZE_FLAGS` candidates (all grew or saved <100 B; documented in `Makefile`). Result vs `0e99a1f0`: `.text` `713,944 -> 712,424 B` (`-1,520 B`), `.bss` `101,720 -> 101,452 B` (`-268 B`), `app_size` `716,704 -> 715,184 B` (`3,664 B` free below `0x080B3000`; `_etext=0x080b1834`), `k2_reset_main` `6872 B`. Validate on hardware: `tools/perf_baseline.json` / selftest benchmarks.
- **PD limiter** (k2/pd-limiter, host-tested only, not verified on hardware; `k2_remote.py pd-limit <watts>|off|status` plus per-tech options, Cmd 0x4E `kLimitArmCfg = 3`). Fits the default image again on k2/limiter-fit (1196 B free below 0x080B3000, see item D under Next up). Hardware test 1
  (2026-11-23, Anker 100W -> K2 -> 55W bank, `pd-limit 10`, replug) failed: a detach switched it off, the trigger wanted the inline
  latch, and arming mid-contract had no list to forge from. Fixed (not re-tested): armed waits (nothing attached, detach, replug
  keep the limit; only `pd-limit off`, EPR, the give-up rules or a reboot end it), it acts on a far sink's Request alone (no inline
  latch), and arming on an attach that already holds a contract (caps, sink Request above P, PS_RDY, quiet line) forges at once from the
  sniffed list. Charger -> K2 -> sink: the sink is sent the charger's own Source_Capabilities, spoken as the source, same PDO count and
  positions. Per-tech Fixed / PPS / SPR-AVS `LimConfig` (`LimTech` per technology: enable/disable, voltage cap, 100 mA current steps;
  PPS Vmax range narrowing e.g. 5-21 -> 5-11 V; SPR AVS 15 V / 20 V voltage ceiling and 100 mA current steps) alongside optional watt cap;
  2-pass `forge_caps` trims trailing unused objects, inserts 0 mA placeholders for non-trailing gaps (since k2/limfix-cut: ends the list at the first object over the limit, no placeholders), and enforces a 100 mA floor on
  mandatory PDO 1 when Fixed is off. Armed by the user; injects after PS_RDY once the line has been quiet `kLimQuietMs` (50 ms) with
  an empty RX FIFO (`!g_rx_nonempty`); the sink's next compliant Request (`request_complies`, checking forged PDO ceilings and
  `GiveBack`) is proof (`Limited`); skips injection (`forge_differs` false) when the real list already matches; re-injects after every
  real Source_Capabilities; a Hard Reset or Soft_Reset within 150 ms of a list counts as a rejection, two disarm it (`ChargerResets` /
  `SinkResets`, skipping `ChargerResets` blame when the sink sent `Get_Source_Cap`); `Injected` timeout (`kLimVerifyMs`) retries via
  `Pending` with advancing `MessageID` skip (`1 + inj_streak % 6`) instead of deadlocking in `Armed`; TX Hard Reset clears `iq().watch()`
  so stale contracts never seed after a reset; an EPR contract or `EPR_Source_Caps` disarms it (SPR only); at most 20 lists per arm.
  Bus-powered Hard Reset survival (`/lim.rec`, `k2::limcomeback`): when bus-powered UI `pd_limit_set` triggers an inline Hard Reset
  (`allow_bus_powered = true`), `/lim.rec` persists `LimConfig` (refuses the reset and arms seeded if save fails), `bootpd::early()`
  re-arms passive limit injection for a 1.5 s bring-up loop when `session::backup_alive()` confirms the short dark, and `Ui::tick()`
  runs a 5 s deferred `limcomeback::consume()` if the K2 stayed up in RAM (cancelled on `off`); `main.cpp` also consumes on ordinary boot.
  Latency: the sink draws at the charger's real contract until the clamp lands (PS_RDY + 50 ms quiet + the sink's Request response time,
  not measured); arming mid-contract with no contract in the sniffer waits for the next real Source_Capabilities. Refused outside
  valid configs or while another feature owns the port (wire refusal byte: BadLimit 1, Busy 2). `Cmd::Stop` does not disarm it.
  UI (`ui_pages.cpp`, `home_model.cpp`, `ui_home.cpp`, `ui_connection_page.cpp`; host-tested only, not verified on hardware): Tools
  "PD limit" per-tech Fixed/PPS/SPR-AVS editor + "Below current" presets, amber `LIM <label>` chip (`lim_label`) on the LINK CHARGER
  slice, Home protocol suffix `PD 9V3A lim...`, and a once-only "Limit off: <why>" result on give-up; `active_caps()` stays the
  charger's real list while the limiter forges. Picker fixes: a level, change of level, or Off on a live contract (inline, far sink seen)
  resets the link first (second OK within 5 s, "OK again: resets link"), then arms unseeded; no reset with nothing attached or only a
  charger. Offers come from the held list when none are live, else a "Reset link" row. The limit stays armed across a detach; the
  per-attach Request and forged-list data clear on detach; the LINK chip is replaced by LAST and the Home suffix is Live-only; held
  offers show grey. A join mid-contract with no Request heard arms without a seed ("No reset: replug to apply"); remote `pd-limit` arm
  stays seeded. Verify on hardware: (1) bus-powered inline Fixed/PPS/AVS cap triggers Hard Reset, survives VBUS cut via `/lim.rec` and
  clamps on re-attach (`[LIM] comeback boot`); (2) self-powered or held-up K2 scrubs `/lim.rec` after 5 s; (3) PPS 5-21 -> 5-11 V and
  SPR AVS 15 V/20 V caps clamp sink Requests (`[LIM]` lines); (4) `active_caps()` still reports the charger's real list.
- **Test layer reads, held data, depth-scaled preset counts** (k2/test-layer-reads: 2d2e9203, bae38bca, 7da8be65, 4e745924, cb4f51b4;
  host-tested only). Opening the Charger or Cable tab starts nothing; the offers come from a Light run with the PD pick, the cable
  from the Cable Actions row. Held data (same charger, same cable): offers stay until a different renegotiation replaces them,
  cable identity, SVIDs and modes until a different identity or SVID set is read; sleep and detach alone clear nothing and grey
  nothing. Empty states "No offers read" / "Tests: Light, PD" and "Not read" / "Actions: Read e-marker". The Tests step preset
  tails count runnable tests plus spec checks at the selected Depth (real plan with caps, nominal SPR list without); Custom
  family rows keep their own ON/OFF/PART counts. Check on hardware: the preset counts change with each Depth press; a fresh
  boot with a charger shows both empty states and K2 sends nothing on tab open (no Rd/VCONN edge on the lines); Light + PD
  fills the Charger tab; sleep and unplug keep both tabs' data; a different charger replaces the offers; inline, Read again
  says "Inline: listens only" and sends nothing. .bss +136 B, text -1936 B vs 82792207 (like-for-like worktree builds); the preset counts alone: .bss +8 B, text +552 B.
- **Light asks, never requests (PD)** (k2/light-caps, 24bff690; verified 2026-11-23 on v1.2.0-192, EPR 140 W charger + EPR cable: only the attach's PDO 1 5 V Request, one EPR_Get_Source_Cap, no EPR entry, offers listed, QC/UFCS 5 V handshakes answered; trace runs/auto-read-hw/trace-light.txt). Light sends no PD Request, not
  even the 5 V one: every PDO / APDO is a listed row, the EPR list is asked once with EPR_Get_Source_Cap in normal mode when PDO 1 is
  EPR-capable (never an EPR entry); the legacy units keep their 5 V handshake rows. Check on an EPR charger and on a PPS charger:
  `pd_trace.py dump` of a Light run shows no Request, the grid lists every offer as LISTED, the card's PD group reads Pass
  (advertised), the footer reads "PD: N listed", the legacy rows read "speaks it" / "no reply"; a non-EPR charger gets no EPR ask.
  Text +32 B, bss 0.
- **Charger-test levels** (k2/ct-level, built and host-tested only). Check on a PD charger with PPS (the Anker IQ4): a
  Light run (Review Level row, or `ct_start.py --level light`) keeps VBUS at 5 V for the whole run (`pd_trace.py dump`:
  no Request above 5 V, `vbus_fast` never above 5.5 V), the grid shows the listed PDOs/APDOs/EPR levels in green as advertised (the Pass look; no Request, no EPR entry, no Mi 6A hunt), legacy listings Noted (grey left bar), the card header reads "Light run"; a Deep run sweeps every PPS and AVS APDO; Medium has no sweep rows.
  Settings > Test level survives a reboot and sets the Review row; `stack_free` in the heartbeat stays above its
  current floor on Review and Results.

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
- **SuperVOOC phone responder and the charger-test ceiling stage** (k2/svooc-responder, built and host-tested only).
  The trigger's Phone and Probe profiles answer like a OnePlus 15 (escape timing, moving 0x82 word, every 0x86 reply
  recorded, session past 72 pairs); the charger test can run a `State::Ceiling` step after a VOOC-family pass and show the
  highest 0x86 ceiling the charger kept going under as `SV cap` in Results (`k2_remote.py charger-svooc-cap`,
  `ct_start.py --svooc-cap`). The ceiling is the K2's own ask read from the protocol, never a drawn current. Hardware check:
  OnePlus 120 W charger on USB-A, top port empty, HID enabled; confirm the guards accept the start from a charger-test
  legacy session and read the heartbeat `stack_free`.
  **Parked 2026-11-13 after the hardware check (1.0.1-98-ba258ee7, OnePlus 120 W on USB-A):** the Probe trigger still
  ends at ~3.3 s after the wake (70 pairs) and reached only two asks (1.5 A, 4.0 A), so its "ceiling" is the last ask
  before that stop, not a charger verdict. Every no-load run stops there whatever the replies, while the OP15 draws
  1.2 A by ~2.7 s: most likely the charger ends a session that draws no current (unprovable under the 50 mA guard).
  Open if resumed: the charger-test preflight refuses on USB-A ("load attached, latched by far rd": the A-to-C cable
  latches `InlineCause::FarRd`, which `svooc_run` waives for usb_a but the preflight does not), so the stage never ran;
  the trigger report header still prints "0x86 answer 0x005 / 0x82 constant 0x051" for the Phone and Probe profiles.
  **2026-11-15 (k2/svooc-deep, host-tested):** the stage waits for D+/D- after the family release (it was refused "D+/D-
  busy" 20 ms after the SVOOC pass, inside the engine's 500 ms 0 V hold; `runs/svooc-cap/run1.out`); usb-a mode waived
  the far-Rd / foreign-VBUS inline latch for the run (obsolete: since the latch policy of 2026-11-15 those never latch and
  usb-a is a no-op for the charger test); a quiet adapter ends the stage as "stopped at
  <t>, <A> claimed, <V> max". Open: the fast-charge engine still refuses the VOOC family on any inline reason, so the
  detection row on a USB-A cable can fail "Inline" before the stage is queued.
  **2026-11-15 phase 1b (host-tested):** the hardware run (OnePlus 120 W, C-to-C, `runs/svooc-cap/stream3`) never reached an
  ask: 25 pairs after wake 2 the adapter raised VBUS (4.93 -> 6.90 V), the Link latched inline with cause Rise (no
  contract in the PD-silent session), the Rd went off, the Runner stopped the trigger and the brick dropped VBUS, so the
  run ended "source lost". Fixed: the SuperVOOC trigger counts as the K2's own VBUS in the Link; a Runner-guard abort
  records its own cause ("aborted: sink ..."); a VBUS drop after the stage is recovered with an Rd cycle; the exit line
  logs the run's single `vmax` and says "VBUS gone" instead of "back at 5 V" for a dark output.
  **2026-11-15 phase 2 (host-tested):** levels wired in. Light and Medium stop at the VOOC family's detection and keep its
  ident (`VOOC id 0xAC` in Results, "VOOC family ident 0xAC" in the summaries); the ceiling stage runs at Deep only
  (`with_level`, and the Runner checks the level). The Results words follow the copy rules: "<=11.1V, 10.9A claimed",
  "stopped at 3.3s, <=9.1V, 4.0A claimed". Command 0x59 appends the ident byte; `ct_start.py` sends the mode on every
  deep run (`--svooc-cap` alone means deep) and prints both lines; the web dashboard's run summary reads them too. The
  card is unchanged (its payload is full). Validate: a Light and a Deep run on the OnePlus 120 W (no load) show the ident
  on both and the `SV cap` row on Deep only.
- **Inline interrogation** (k2/inline-interrogate, built and host-tested only; `docs/DEVICE_TWIN.md` 9.1, the one inline
  TX exception, `CONNECTION_MATRIX.md` X11). With a device inline behind its charger the K2 hard-resets the link (unless
  a fresh contract was sniffed), asks the device as its source (Discover Identity, Get_Sink_Cap, Get_Sink_Cap_Extended,
  Get_Revision, SVIDs, Modes), hard-resets again and learns a twin. Surfaces: Device twins > Learn (confirm), shell
  `twin learn inline confirm [dry] [name]`, `k2_remote twin learn inline --confirm`, lab `pd interrogate --inline
  --confirm`. Validate (hardware, a phone behind a bench charger, no load, K2 on HID power): (1) `pd interrogate
  --inline --confirm` prints asked / answered per question, both contracts and the run length; (2) `pd_trace.py dump`
  shows two K2 Hard Resets, the questions with the source roles and the expected MessageIDs, the device's answers;
  (3) the phone charges again after the final reset (`state`), `ping` uptime grows through the run (no reboot);
  (4) `twin learn inline --confirm --name X` saves a twin with the identity and sink PDOs; (5) the screen path:
  Learn row -> "Resets the link. Confirm" -> OK -> "Resetting the link" / "Asking the device" -> "Twin learned".
  Open: whether a device with a strict MessageID filter drops the charger's next message after our question.
  Hardware 2026-11-13 (runs/clone-canon2): the Canon behind a PD charger answered no question (cause not proven; the
  vendor messages now go last and every question logs `[IQ] ... Q <step> ...: <who answered>`); behind a laptop the
  source Hard Reset within 3 ms of our first question, so a reset within 150 ms of a question now ends the run at once
  ("Charger resets when asked", no loop). Re-run both setups and read the `Q` lines.
  Laptop recording clone-canon3 (built and host-tested only): the laptop re-sent its capabilities 130 ms after
  Get_Sink_Cap, which now ends the run ("Charger restarts when asked"); only an answer that matches its question
  counts; the TX wait is timed on micros() (was ~11.8 ms blocked); `[IQ]` log words are no longer cut at 12 characters.
- The HID-cable loop probe (Tools > Loop probe): built and host-tested only; the test plan is in `LOOP_PROBE.md`.
- **PD Spec Check** (`K2_PDSPEC=1` builds only, k2/pd-spec-check; engine, remote protocol and host tools host-tested, the
  page and its Ui binding compile-checked only until the next full test run; `docs/PD_SPEC_CHECK.md`). No-load
  conformance pass: fixed-level pairs up and down, PPS 20 mV steps, PPS and EPR keep-alive timeouts, Hard Reset and detach
  recovery, 21 checks judged against named spec limits, pass / fail / skip and a penalty score (0 = ideal); remote 0x5A,
  `k2_remote pd-spec`, `tools/pd_spec.py`. The polite part (Transitions, PPS / AVS, source reset: 27 checks) also runs as the
  stage of a Deep charger test (right after its PD unit) and fills a "PD spec" section of its results grid (k2/spec-in-deep, built only, host-tested;
  the Tools page is gone). Validate that on hardware with a Deep run (`ct_start.py --level deep`): the section appears, VBUS ends
  at 5 V, no Hard Reset in the Black Box. Validate
  (hardware, output empty, K2 powered from the PC, a PD charger with PPS on the bottom port, active sink via
  `k2_remote pd attach` before each run): (1) `pd_spec.py run --charger NAME --confirm` twice; `k2_remote ping` uptime covers each run
  (no reboot); every family ends at 5 V (`[PDSPEC] item` lines, VBUS back at 5 V after each); the second run's verdicts
  agree with the first's; (2) a run with only Transitions, BACK on the K2 mid-run -> footer `Stopped`, VBUS at 5 V,
  remaining checks `run stopped` / `not reached`; (3) a K2 powered by the charger is refused (`K2 runs on this
  charger`); (4) no PPS / no EPR -> those families skip with `no PPS` / `no EPR`; (5) heartbeat `stack_free` not lower
  than before; (6) screenshots of Results and Detail in all four rotations show no clipped text; (7) the saved blobs
  become host fixtures under `tests/fixtures/pdspec/`.
- **PD Stress page** (Tools > PD Stress, k2/pdstress-ui, built and host-tested only; `docs/UI_DESIGN.md` 4.5). Setup
  1/2 (P1..P12 checklist, Cycles, Pace, saved results), Review 2/2 (KvTable + two-press START), Running (48 px VBUS,
  counts, BACK stops), Results (score, clean / issues / skip, one row per sequence, Detail), Save result / Discard into
  the newest-8 store (`/ps/xxxx.psr`). Validate (hardware, output empty, a PD charger on the bottom port, active sink via
  `k2_remote pd-reread`): (1) `k2_remote page pdstress`, screenshot: `1/2 Sequences`, 11 of 12 picked (P9 EPR off),
  footer `11 of 12 picked`; (2) pick only P1 Fixed, Cycles 25 %, Done -> Review shows the charger's top fixed level and
  PPS top (`PD 20V5A PPS 21V` style) and the live Output; (3) OK arms (amber, 5 s bar), wait 5 s -> disarms; OK, OK ->
  Running: `seq 1/1`, the clock counts, VBUS hero follows the steps, `k2_remote ping` uptime grows through the run (no
  reboot), heartbeat `stack_free` not lower than before the run; (4) the run ends on Results, `Score` shown, P1 row
  `clean` or `N issues`; OK on it opens Detail, -/+ turn the two pages; (5) Save result -> footer `Result saved`, Setup
  lists `Result #1`; OK on it reopens the same score; (6) run again, BACK during Running -> `Stopping: back to 5V`,
  VBUS back at 5 V, Results with the remaining rows `not run`, Discard -> nothing new in the saved list; (7) close the
  page during a run and reopen it -> Running, not Setup; (8) on a passive K2 (no contract, charger
  waiting for a sink) START is refused in the footer (`No PD source`, Caution tone) and nothing changes on CC; (9)
  screenshots in all four rotations show no clipped text. Not built: `k2_remote pd-stress saved` (the saved list over the
  remote needs new remote ops; the page is the only reader today).
  Hardware 2026-11-12 (1.0.0-651, Anker IQ4): (1)-(8) pass: P1 at 25 % ran 16 transitions in 21 s, score 0, no reboot,
  stack_free 15,960 -> 15,896 B; save, reopen, discard, close and reopen during a run, BACK -> `Stopping: back to 5V`
  and VBUS 5.11 V, passive START refused with `No PD source` (Caution); (9) host-covered only. Screenshots in
  runs/integ-0312/ps. A re-read's Rd lasts about 15 s, so START must follow `pd-reread` at once. UI notes for review:
  Running has no `seq n/N` line and the right column (Rej / Wait, Out of band, Reboots) shows no values; Results paints
  `0 issues` amber; the focus of the Save result / Discard rows lands in the empty label column; a P1 cancelled at 12 of
  16 transitions reads `clean`; the Tools row on a passive K2 reads `NO PD` in red with a red footer.
- Home 2c and the OK + BACK reboot chord (2026-11-12, built and host-tested only). Validate: Home in all four
  rotations (screenshots; the 1 px row rule at y 136 must show in landscape), Long BACK cycles W -> V -> A and the
  hero survives a reboot, the cards rotate every 5 s with the lower cell 2.5 s behind, a live load shows PEAK and
  DROP + SAG, `stack_free` in the heartbeat; OK + BACK shows the HoldCard at 1 s and reboots at 3 s, a BACK hold
  alone does not reboot.
  Hardware 2026-11-12 (1.0.0-666, Anker IQ4, `pd-reread` for a live 5 V contract): idle, live and LAST render;
  Long BACK cycles W -> V -> A and back (`[UI] home hero=N`, saved) and the A hero survived a reboot; heartbeat
  `stack_free` 14,392 B. Found: LAST froze the session's tail (VBUS 3.323 V mid-decay, protocol `PD 3V` after the
  contract went), fixed by a steady-window snapshot (`home::steady_snapshot`, 500 ms, VBUS within 10 % and the same
  protocol row). A remote press during a screenshot transfer is held until the transfer ends (the screenshot stalls the
  loop), so the shell sequence press, wait, screenshot is the reliable one. Not checked on hardware: rotations, the card
  timing, PEAK / DROP under a load (load testing paused), the OK + BACK chord (`k2_remote press` holds one key).
  Attention on 1.0.0-668 (Anker IQ4, `pd attach`, then `pd request 2 --confirm` and `pd release`, no load): the
  5 -> 9 V and 9 -> 5 V steps each put DROP + SAG in the upper cell with the dot (runs/integ-home/att/hw-sheet2.png);
  no reboot, `stack_free` 13,976 B. A screenshot stalls the loop for about 6.6 s, which Attention reads as Home off
  screen (2 s quiet again), so a step right after a screenshot is learnt, not shown. With no load the DROP card only
  says `need 1A` / `--`, so it marks a VBUS step without showing it: a special case, left as is (user, 2026-11-12).
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
| Image size tracking: `tools/size_track.py` snapshots an ELF (symbols, line-table origin per file, DWARF inline records) and diffs two snapshots (functions, clone swaps, inlining state, defined vs originating file, objects; the flash delta reconciles exactly); `make size-track` diffs the build against HEAD's snapshot; `backfill REV` builds a commit in `runs/size-track/wt`; opt-in post-commit hook `make size-hook` | prototype (2026-11-24): snapshot 3 to 10 s, diff under 1 s; same-commit noise +16 B (the `-dirty` version string); a `noinline` on `StrBufBase::view()` read as 379 sites de-inlined, +1440 B | n/a (host tool) | `size_track.py`, `tools/hooks/post-commit` | |
| Pin map, SWJ disabled | done | verified | `k2::pins` | |
| Math selftest behind `K2_SELFTEST` (default 0; pre-release builds only; `make gate K2_SELFTEST=1` -> flash -> `make perf-check`, the perf step says SKIPPED on a default image) | done: text 713156 -> 696948 B (-16208 B, bss unchanged) | n/a (build switch; host-tested) | `math_selftest.cpp`, `shell_hw.cpp`, `perf_gate.py` | |
| ST7789V 240x240 LCD, screenshots | done | verified | `k2::lcd`, `gfx`, `capture` | |
| 4 buttons, long/hold events | done | verified | `buttons`, `ui_events` | |
| Reboot gesture: OK + BACK held 3 s (HoldCard from 1 s, replaces the 5 s BACK hold, 2026-11-12) | done | not verified (host-tested: `test_ui_calc`, `test_ui_pages`) | `RebootChord` (`ui_calc`), `Ui::feed_chord` | |
| Home layout reorg: 48 px hero (drops `font_64`, net `-172 B` flash; centered at `kHeroDigitX=43`, permanent hero flow arrow, `effective_drop_mv` DROP fallback in `7c17e4d8`), side-by-side 24 px sub-heroes, unified `W`/`V`/`A` graph headers (`Max/Peak` in hero, numerical `Max/Peak` or Peak-only in sub-heroes), 3x74 px configurable static/rotating boxes (`PD CHG/SNK`, `ENERGY`, `DROP`, `E-MARKER`, `CC1/2`, `D+/D-`, `BATT`, `LOAD R`, `Off`, `Rotating` excluding pinned + 3 s fading cyan attention border), regular-text sweep window in info row 1, `Settings > Home boxes` & `Tools > PD limit` dedicated radio-list pick-one submenus (`62cb5898`), persisted in `uiprefs` | done, host-tested (`test_home_model`, `test_home_cards`, `test_ui_home`, `test_ui_home_landscape`, `test_ui_pages`, `test_ui_no_truncation`, `test_uiprefs`) | partial (3b9df499 verified on K2; validate polish/DROP/radio pickers on hardware: all 4 rotations, `PD CHG/SNK` PPS fit, 3 s cyan fade, `Settings > Home boxes` reboot persistence) | `ui_home`, `home_cards`, `home_model`, `home_cell`, `ui_pages`, `uiprefs` | PASSIVE |
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
| WebUSB transport (vendor interface, `K2_WEBUSB=1`) | prototype, off by default | built only, needs hardware | replaces the HID remote channel; firmware, `k2_usb.py` (pyusb), `k2proto.js` + dashboard; modes WebUSB / +Shell / +Drive / +Drive+Shell (no HID) and the HID+WebUSB fallback. Validate: desktop Chrome on Linux (udev rule), Android Chrome via OTG, Windows WinUSB binding, ZLP-tolerant reads, DFU_ENTER over the vendor pipe |
| `make smoke` hardware test | done | verified | writes `build/smoke/<hash>/` |

## Charger test

| Feature | Status | Hardware | Module |
|---|---|---|---|
| Test units: legacy first in one PD-silent attach, then PD, then the low programmable rows (CHARGER_TEST_FLOW) | done | verified | `chargertest`, `ui_charger_test` |
| Report cards, Results view, QR readout (card format versions in QR_SHARE.md) | done | verified (cards); QR phone scan not verified | `chargertest`, `qrshare_records`, `ui_share` |
| Reboot-resume journal (`/jr`) | done | partial: see Not verified | `session_store` |
| ~~The 6 A hunt rows report "not unlocked" instead of failing when the source never offered 6 A~~ replaced 2026-11-18 by the Xiaomi unit (next row) | retired | - | `chargertest` |
| Xiaomi unit: the 0x2717 chain row, the before/after list diff, one row per unlocked mode named by watts ("Mi 55W"), "cable limit" skips, a Xiaomi section in the card block (format 12) and on the device card (CHARGER_TEST_FLOW "Xiaomi unit") | done | not verified (host-tested on the 20K, FH22 and CUKTECH 120 W lists) | `chargertest`, `mi_unlock`, `report_card_deep`, `ui_cards` |
| Test levels: Light (what is advertised, VBUS stays at 5 V), Medium (default, no deep sweeps), Deep (+ PPS and AVS deep sweeps); Review Level row, Settings > Test level, `charger-level` (0x5B), `ct_start.py --level`, level on the card and in the QR record; the Noted verdict for what a Light run did not measure | done | not verified | `chargertest`, `ui_charger_test`, `kit` |
| Idle power: backlight PWM (PB7), Active -> Dim -> Off with VBUS stopping at Dim, panel sleep, low AHB clock in Off, Settings > Display, remote 0x37 (`k2_remote.py power`) | done, host-tested (`test_idle`, `test_idle_cfg`, `test_ui_display`) | not verified. Validate: a brightness step changes the K2's own draw on the meter (PB7 timer mapping is inferred); Off with nothing attached falls well under the warm 0.52 W; USB and `k2_remote` still answer after the low clock; the wake press does nothing else | `idle`, `idle_cfg`, `idle_hw`, `lcd`, `crm`, `ui_display` |
| A Legacy row waits through a source output restart (up to 10 s) instead of failing it | done | not verified | `chargertest` |
| Vendor database: 271 USB-IF-verified VIDs (`tools/vendors.csv`, shortenings of the USB-IF registrant, no PIDs) feed one packed table (`pd::vendor_find`/`vendor_name`, `vendor_table.hpp`), the report-card auto name ("Anker 100W"), cable e-marker names, the PD page and twin auto names; mirrored in `docs/site/r/vendors.js` and `tools/report_card.py`; `tools/gen_vendors.py --check` keeps all three in step (`test_vendors.py`). Chip/cable/PSU makers are weak: a charger's own manufacturer text wins. +1.1 KB text (09d0020d, 0b770bd4, 91866fc4, 42233d9f); +106 cable-maker rows, +1.1 KB text (+0.16 %). k2/vendor-pack (541881ea, ad8e3fc3): names packed 6 bits per symbol (a-z 0-9 space - _ .), up to 16 characters, shown in capitals on the K2, the card tool and the results page (typed names keep their case); `pd::vendor_find` returns a `VendorName` value (no pointer into the pool); auto names trim the brand, never the watts or ordinal; +84 B text; 7 of the 21 ten-char names restored to full spelling; the identity line cap is its real 32 cells (names of 9-10 chars used to run past it). The Charger line drops a name wider than 8 chars whole (ID shown), the Sink head shows names up to 14. `test_pd_sink_oppo` (needs `K2_OPPO_PROBE=1`) and `world_sim` (HostPin) do not build in a plain host run, unrelated | done | not verified (host tests only; read the Identify card, the Source/Sink/Cable heads and the Charger Test card on the K2) | `pd`, `chargertest`, `emname` |

## USB-PD

| Feature | Status | Hardware | Module | Risk |
|---|---|---|---|---|
| FUSB302 init, CC orientation, FIFO receive | done | verified | `k2::fusb302`, `pd_phy` | |
| Passive sniffing, Source_Cap decode (Fixed, PPS, AVS, EPR) | done | verified | `k2::pd`, `pd_engine`, `pd_passive` | PASSIVE |
| Extended messages, VDMs, SVID table | done | verified | `pd_ext`, `pd_vdm` | PASSIVE |
| Attach/detach, inline-load detection and guard | done | verified | `pd_link`, `link`, `link_guard` | |
| PD trigger (Request, PPS/AVS voltage, voltage-cap safety) | done | verified (lone charger) | `pd_sink` | HV |
| PD limiter: forged Source_Capabilities clamped to P, inline | done (shell + Tools/LINK/Home UI, UI not verified on hardware) | verified 2026-11-23: Anker 100W -> K2 -> Xiaomi 55W bank, `pd-limit 10` mid-contract: 6 clamped PDOs sent, bank re-requested PDO3 15 V 0.66 A (9.9 W) 9 ms later, charger Accept + PS_RDY, no reset (runs/link-ui-2026-11-23/limiter2) | `pd_limiter`, `fusb302` | inline TX |
| PPS step-down of a running contract | done | partial: small steps verified, a direct 21 V to 13 V step aborts at the +10 % guard | `pd_sink` | HV |
| PD Spec Check: no-load conformance of the attached charger, 21 checks against named spec limits, score 0 = ideal; its polite 27 checks are a Deep charger-test section (`docs/PD_SPEC_CHECK.md`) | done, opt-in (`K2_PDSPEC=1`) | not verified (engine, protocol, tools and the Deep section host-tested; see Not verified) | `k2::pdspec`, `PdSpecSession`, `specsec` | HV |
| Identify (identity, SVIDs, modes, sink caps) | done (an action of the Connection page) | verified (Pixel) | `pd_identify` | |
| Connection page (`6 LINK`, reworked 2026-11-22) & `7 TOOLS` menu (`k2/link-ui-polish`): 10-stop ring (`1 HOME`..`6 LINK`..`7 TOOLS`..`10 K2`) with full-height 3-slice Overview (`CHARGER` / `CABLE` / `SINK`, consistent `W` hero + `W`/`V`/`A` including live inline power when attached mid-session, Direct Slice Focus, no permanent footer, provenance chips, bottleneck highlight, Cable `mΩ`) -> Slice Actions (merged `Read again` into pinned top `Reset & re-read` section handling mid-session inline 2-press Hard Reset, standalone K2-owned sink Hard Reset, and passive rescan + survey) -> Spec sheet; restored `7 TOOLS` (`Menu::Tools`, wire-ID compatible) with `Request voltage` and `Fast charge` moved to the top; backed by 800 ms offer settle gate + high-water offer latch (`pd_held`) and 600 ms debounced port sense | done (host tests & ARM `-Werror` build green) | not verified on hardware | `ui_connection_page`, `ui_connection`, `ui_ring`, `ui_tools`, `ui_pages`, `pd_held`, `held` | inline Hard Reset (2-press confirm, refused when bus-powered); follow-ups on `k2/link-source-tag` (not verified on hardware): S/P provenance letter fce4c350, no SELF with an inline phone e873a4ef, a new attach drops the last session a4331da9, stale slices with a LAST pill fdd119a8, PD3.x chip bdc9e463; 2026-11-23: the S/P letter and the LIVE/INLINE chips are dropped from the Overview (exceptions only, data and remote keep the source) |
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
| Full cable identity read (k2/cable-full-id): after SOP' Discover Identity, Discover SVIDs and Discover Modes per SVID, and on an active cable the SOP'' identity, SVIDs and modes (best effort); decoded: active VDO 2 extras (U3 power, optical / retimer, hub hops, asym), TBT3 cable mode, DP cable capabilities, other SVIDs raw hex. RAM only (88 B), runs only from the Cable Actions row (opening the tab reads nothing, k2/test-layer-reads) and stays until a different identity or SVID set is read (same cable), not cleared by sleep or detach; shown as more Cable tab rows (scrolls), `k2_remote emarker` / web (GET_EMARKER parts 3 and 4). Not in the saved record, QR or JSON (a format change, left for a decision) | partial (c443b46f, c3496a89, 4b2fcfb6, b8b9ea95) | not verified on hardware (host tests with a fake FUSB302 only; needs a TBT3 / USB4 cable and an active cable) | `cable_full`, `emk_learn`, `fusb302`, `ui_connection_page`, `remote_proto` | VCONN 5 V on CC, as the e-marker read |
| ~~Automatic reads on opening a tab~~ (k2/cable-full-id) removed (k2/test-layer-reads, bae38bca): opening the Charger or Cable tab starts nothing; the offers come from a Light run with the PD pick (the test layer), the cable from the Cable Actions row. Held offers and cable data stay for the same charger and cable (7da8be65). The manual rows keep the inline rule: with the latch set nothing is sent (Read again says "Inline: listens only", the cable read only listens). RAM only | partial (4e47727f, f7917f25, 64891d11, e5ad3108, a96deea4, 798a85f9, def8c130, b4de0174, bae38bca) | not verified on hardware (host tests with a fake FUSB302 only) | `pd_offers`, `offers_read`, `ui_connection_page`, `fusb302` | Rd on a source's CC when no contract is held; VCONN 5 V on the cable read |
| Apple cable 6-chunk reader | done | not verified | `pd_engine` | |
| Record codec (`emrec`), JSON (`emjson`), names (`emname`) | done | verified | `emrec`, `emjson`, `emname` | |
| Cable library, 140 entries (/emk files) | done | verified | `emlib`, `flash_map.hpp` | **FLASH** |
| Save, list, rename, delete (device, remote, drive) | done | verified | `emlib`, `ui_emarker` | **FLASH** |
| Spoof (answer SOP' with a saved cable) | done; refused while a sink is inline | verified on the top plug (2026-11-06) | `emk_spoof` | HV (a faked 5 A / 50 V cable) |
| Spoof quirk table, K2E2 clones, `emarker learn` (power a real cable, replay DI / SVIDs / Modes / seeded vendor commands, record the replies) | done | not verified (host tests only) | `emk_quirk`, `emk_learn`, `emk_learn_hw` | PASSIVE (cable in the bottom port, no charger) |
| Lab OPPO / OnePlus probe, **built only with `K2_OPPO_PROBE=1`** (default off: with the flag off the image is the one without it and 0x4D answers UnknownCmd; host tests `make test K2_OPPO_PROBE=1`) (`k2_remote pd oppo-probe [--skip-swap] [--steps mask] [--auth-words HEX x4] [--repeat N]`, then `pd oppo-power MV [MA] --confirm`): on a 5 V contract the sink sends its own DR_Swap (becomes DFP; Reject / Wait / Not_Supported keep UFP and the VDMs still go out), AUTH (structured cmd 31 + 4 host-drawn words, the answer recorded, never verified), POWER and EXAPDO (unstructured cmd 1 / 2, last word decoded: imax / vmax, "extra PPS current"), DR_Swap back, Get_Source_Cap with the caps diff; one message at a time, never while an AMS waits for its answer; hard reset / soft reset / detach end it and restore UFP. `oppo-power` asks only a point the list in force advertises | done | not verified (host tests only); planned run: OnePlus 120 W brick on the top plug, genuine cable dangling in the bottom port, spoof off, with and without `--skip-swap`, then with the cable unplugged | `oppo_probe`, `pd_sink`, `fusb302`, `remote` | ACTIVE sink at 5 V (no load); `oppo-power` above 5 V needs --confirm |
| Lab EEPROM dump, **`K2_EEPROM_READ`** (default 1; 0 = UnknownCmd) (`k2_remote debug eeprom probe`, `debug eeprom dump --out FILE [--size N]`): read-only 24Cxx at 0x50..0x57 on either I2C bus, probe + addressing guess + chunked dump, prints 0x000..0x0FF and 0x400..0x40F; plus `debug eeprom cal-clear --expect HEX24 --yes` (**`K2_EEPROM_CAL_CLEAR`**, default 0, lab build only): compare-and-swap zeroing of the 12 calibration trim bytes 0x04..0x0F of the 24C16 on the INA bus, nothing else can be written | done | not verified (host tests only): needs the board's EEPROM bus and width confirmed | `eeprom`, `i2c`, `remote` | passive; no VBUS, no load |
| Lab listen attach (`k2_remote lab listen-attach --secs 10 --port bottom`, then `k2_stream timeline`): forced detach first (Rd off until VBUS < 0.8 V or 1.5 s; `--no-detach` skips it), then Rd on, Auto-CRC off, passive receive of SOP / SOP' / SOP'', never transmits; hears what a brick says to its own cable (SOP' Discover Identity, the OPPO command 31 challenge and the cable's answer) | done | not verified: the first version (Rd blink, no detach) heard 0 frames in 15 s on the OnePlus 120 W brick (VBUS stayed 5.27 V, 2026-11-18); the detach version is host-tested only | `pd_listen`, `fusb302`, `remote` | PASSIVE (charger in the bottom or top port, no load) |
| Passive cable resistance (idle vs load) | done | not verified against a reference | `cable_r`, `ui_cabler` | PASSIVE |
| Kelvin 8-wire cable resistance (`cable-r kelvin`, Mode on the Cable R page) | done | not verified (needs a sense fixture and at least 0.45 A of load) | `cable_r`, `ui_cabler` | |

## Black Box

| Feature | Status | Hardware | Module |
|---|---|---|---|
| RAM ring of raw PD/FUSB302 records, remote command 0x70, `tools/pd_trace.py` | done | verified | `pdtrace`, `pdtrace_link` |
| Trigger-frozen captures (contract change, hard reset, VBUS dip, detach) | done | verified | `pdtrace` |
| Captures as /bb files (32, oldest evicted), LZ4, written when the link is quiet | done | verified | `bbstore`, `bbpersist`, `lz` |
| Device viewer (Tools > Black Box) | done | verified | `ui_blackbox`, `pdtrace_view` |
| Black Box saves deferred inside a charger test: rare kinds budgeted (`kRareBudget` 2 per run), waiting captures kept in their RAM slots, overflow counted (`drop=` / `defer=` in `[BB]`), one write per idle poll after the run (ARCHITECTURE "Flash writes during a charger test") | done, host-tested (`tests/test_run_writes.cpp`: 21 erases inside a 39-row run before, 2 now) | not verified. Validate: (1) a full charger-test run on a source that resets or refuses often (Xiaomi 6 A, FH22) with `tools/loop_survey.py` running: no PD-service gap over the 25 ms answer window inside the run beyond the journal's own writes, and at most two `[BB] ... held=1` saves (the rare ones) between START and the end of Releasing; (2) the `[BB] saves=... drop=N defer=M` lines land after Releasing, one per `poll()` pass, and `pd_trace.py captures` then lists the kept captures; (3) a run that ends by cancel or an unsafe exit flushes the same way | `bbpersist`, `pdtrace` |
| Diagnosis: ranked Fail / Warn / Info findings per attach session, passive (`pd_trace.py diagnose`, Cmd 0x72, shell `diag`) | done | not verified | `diag`, `diag_live`, `diag_wire` |
| Diagnosis on screen: Tools > PD diagnosis, Home row 1 `Diag <word>`, Who's attached Source footer | done | not verified | `diag_screen`, `ui_diagnosis`, `home_model` |
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
D. **Remaining flash compaction levers (`2,380 B` free below `0x080B3000` after `0a0dc5d7` + `pd::BatteryStatus` `.data -> .bss`):**
   - Done (`0a0dc5d7`, `-1,116 B`): moved `log_pdo`, `log_epr_info`, `log_ext`, `log_cable` out of `-Os` `src/drivers/fusb302.cpp` into `-Oz` `src/drivers/fusb302_cold.cpp`; marked multi-inlined non-hot helpers `[[gnu::noinline]]` (`steady_pair`, `udiv64_by_u16`, `exti_off`, `Runner::busy`, `ctcable::Choice::operator==`, `shell_hw::Line::Line`); added `K2_BSS_ZERO`/`K2_BSS_INIT` on `ufcs::Prober::res_` and `report_card::Store`.
   - Done (`pd::BatteryStatus::charging = 0`, `-1,568 B`): moved `g_engine` (`1,412 B`) and `g_ident_ext` (`144 B`) out of flash-backed `.data` (`2,448 -> 892 B`) into `.bss`.
   - Lever B (open): trim remaining startup `memset`s and zero-init constructors in `_sub_I_65535_0.0` (`~2.0 KB`).
   - Lever C (open): cold factoring in the largest `-Oz` UI/test files (`src/ui/ui_charger_test.cpp` `32.5 KB`, `src/chargertest/charger_test.cpp` `32.8 KB`, `src/ui/ui_connection_page.cpp` `26.0 KB`, `src/ui/ui_pages.cpp` `21.9 KB`).

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
