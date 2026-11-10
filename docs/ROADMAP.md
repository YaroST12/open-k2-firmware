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

- **An inline K2 hijacks the link** (2026-11-09, user report, build b00178c): fixed in 026e516 (+ 5334c08, 122412f),
  hardware check pending. The report: hub or charger -> K2 -> USB 2 C-C cable ->
  Pixel, HID-powered plain boot, no PLUS. The K2 put Rd on CC2 from the first instruction, Requested 12 times in 3 s
  while the Pixel Requested too, answered the Pixel's VDMs as the device, flapped inline yes/no, and Refresh (which opens
  CC ~150 ms) rebooted the hub-powered K2. Dock -> K2 -> laptop (same day): no PD traffic after a clean boot, yet the page
  said "PD none" / "NO DEVICE" at 20 V. Matrix rows H6, H7, C3, C4 (`docs/CONNECTION_MATRIX.md`, exceptions X1 to X3).
  - Diagnosis (code reading plus host tests, hardware pending): a stale `/rd.rec` was read on a host-powered boot and
    converted to a key mode (`src/core/rd_comeback.cpp`); `pd::Link` is UI-tick fed, was blind during the slow init and
    a detach cleared its latch, so the Rd -> self-hold Request -> VBUS returns under load -> inline -> Rd off -> VBUS
    collapses loop ran; the Request and the VDM answers were gated on the latch only; Refresh's `CycleBridged` opens CC.
  - Fix: (1) a boot with a host or a 5 V aux rail ignores the marker (`rd_intent.hpp`, `rd_comeback.cpp`); (2) `pd::Link`
    latches inline on a load, a foreign Request, a VBUS rise, or foreign evidence held 1.5 s (VBUS above 5.5 V or a sink-band
    CC level with no Rd of ours, no contract of our own), clears only after a quiet window of proof, and survives a VBUS gap
    (`src/pd/pd_link.cpp`); (3) the driver mirrors it and also gates on `sink_downstream()` / `sink_cc_far()`: no Rd, no
    Request, no SOP answers (`src/drivers/fusb302.cpp`); (4) Refresh and the survey are passive and Blocked(inline)
    while inline or possibly inline (`src/core/rescan.cpp`, `src/core/survey.cpp`); (5) empty states say "in use", never
    "no device", and name no port (`include/k2/ui/ui_connection.hpp`).
  - Validate: host tests (`tests/test_pd_link.cpp`, `test_fusb302_scan.cpp`, `test_rd_intent.cpp`, `test_ui_connection*.cpp`),
    `make gate`; hardware, one device tool at a time, no active PD with a laptop inline: **H6** charger -> K2 -> Pixel,
    HID, plain boot: `bootinfo` shows `pin=0`, Black Box shows no K2 Request and no SOP TX, LINK `inline=yes`, UI "Live sink";
    **H7** hub 5 V 0.9 A -> K2 -> Pixel: same, no VDM answers, inline never flaps, hub keeps VBUS; **C4** plant a stale
    `/rd.rec` (self-powered run, bare unplug), plug into HID only: no Rd at any time; **C3** Connection -> Source ->
    Refresh with a sink behind: no CC open, K2 does not reboot, survey shows Blocked(inline); dock -> K2 -> laptop: with no
    PD traffic the Source / Sink / Cable tabs say "in use", never "NO DEVICE", and the K2 transmits nothing.

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
- **The splitter's sleeping port still needs the Rd pin outside a charger-test START** (2026-11-10, be3f589, fixed in
  f0d79fb as an opt-in setting, **Settings > Wake sleeping port, default off**; hardware verified 14f9d24, see the
  note at the end of this row): idle
  K2 on the splitter with nothing attached, VBUS 78 mV, "No source"; `pd-reread` without a pin answered "busy" and
  `pd attach 5000` did nothing; `pd-reread --pin 2 --no-run` woke it at once. 5926abb's slow-wake dwell only acts
  inside the Rd seek after a passive pick, and the open standby reading is wiped by `RdSeek::reset()` on attach, so an
  idle K2 never holds a steady Rd long enough. Fix: the idle/no-source path treats a line with the standby signature
  (open ~3.0-3.3 V, `kStandbyOpenMv`) as a sleeping source and holds Rd on it for `kSlowWakeDwellMs`; keep the open
  readings across `reset()`. Done: `pd::IdleWake` holds the Rd on the one signature line at plain passive idle
  (`idle_rd()` only when no other Rd owner exists), `RdSeek::reset()` keeps the open readings (`clear()` at boot),
  and the 500 ms no-Rp detach is held off inside the dwell window (`IdleWake::holding_slow`, ended by `on_rp_seen`);
  host-tested in `tests/test_rdseek.cpp`. Validate: host test on the decision; hardware: idle K2, plug into the sleeping splitter
  port, PD list on screen without any pin.
  Opt-in (the idle wake breaks I1 / H2 for every waiting charger, see the merge-pass note below): the hold only runs with
  the remembered setting **Settings > Wake sleeping port** (last Settings row, OFF by default; bit4 of byte 7 of the
  `/prefs.cfg` record, `pd::IdleWake::set_enabled`, `fusb302::set_idle_wake`, applied from main() at boot). With it
  off the idle K2 presents no Rd and `RdSeek::reset(false)` wipes the open readings on attach, exactly as on `public`
  (host-tested: `tests/test_fusb302_scan.cpp` off / on / off-again on a 3.3 V open CC2, `tests/test_rdseek.cpp`,
  `tests/test_uiprefs.cpp`, the Settings row in `tests/test_ui_rotation.cpp`). Hardware check pending: setting on,
  idle K2 into the sleeping splitter port (PD list without any pin), then a waiting charger and a DRP phone with the
  setting on (expected: the K2 becomes their sink for one dwell; this is the documented exception), and off again.
  Bench rig 2026-11-10 (Anker IQ4 -> splitter -> K2 top plug, be71f10): the setting never presented a visible Rd. Root
  cause found in code (`k2/idle-wake-hw`): the passive scan attaches on the dark port's Rp (chip CC2 BC_LVL 3, VBUS 0,
  no PD) and nothing detaches that attach (`DetachTracker` needs VBUS seen >= 3 V first, the comparator keeps reading
  the Rp), while the hold is gated on `g_active_cc == 0`: after any toggle off -> on, or any scan attach, the wake
  never ran again. Fix: with the setting on, an attach with VBUS < 3 V and no PD message for `kDarkAttachMs` (3 s) is
  released ("Detach: dark attach", `IdleWake::dark_attach`, same owner gate as the hold) so the hold takes the line;
  setting off is untouched. The hold now also writes SWITCHES0 at once (it used to reach the chip only with the next
  scan) and logs every dwell with the SWITCHES0 readback and the live ADC levels, because the hold line was printed only
  on change (the bench capture started after it). ADC cc1/cc2 vs chip CC1/CC2: no swap found (the bench scan reads
  CC2 BC_LVL 3 and the ADC cc2 rises to 1.63 V at the same moment); hardware check pending.
  Pixel ALONE on the K2 (same firmware): the stale dark attach also kept `pd::Link` inline for minutes, and a latched
  inline blocks `idle_owner_free()` (`downstream_now()`), so it would block the release too. Fix in the Link
  (`kDarkSinkMs`, 3 s): a sink's Rd on CC (`far_sink_cc`) counts as foreign-sink evidence only while VBUS has been at
  vSafe5V within that window (or since the attach); a dual-role phone toggling its Rp through the Rd level with no VBUS
  neither latches nor holds the latch (`test_a_dark_phone_alone_never_holds_the_inline_latch`). The host model keeps
  the Link attached with 900 mV VBUS spikes; a plain 0 V VBUS detaches the Link at 300 ms and releases the latch after
  `kInlineHoldMs`, so the exact bench cause of the 3-minute latch (spiking VBUS reads, or the latch surviving from
  the earlier charger run) is not proven; the rule covers both. Hardware check pending.
  Retest 2026-11-10 (48adfa1, `runs/2026-11-10-merge2-hw/wake2`, `k2/idle-wake-2`): the hold now presented Rd but the
  splitter still never woke. Root cause, three links: (1) the passive scan kept running during the hold and attached the
  board's CC2 bias net (~1.7 V open, see CHARGER_QUIRKS "PLUS key is coupled to CC2"), which reads comparator-true and
  inside the Rd window; the attach cleared the hold while a stale PDWN1 stayed in SWITCHES0; (2) `detach_reset` then wrote
  SWITCHES0 = 0 and the ADC sample fed one UI tick later (cc1 = 6 mV under the old Rd, cc2 = a 3284 mV measurement spike)
  was noted as the "open" level, so a level taken under our own Rd became the wake's evidence; (3) with `note_open` blind
  under Rd, an unanswered hold re-armed the same line forever. Fixes: the sweep is restricted to the held line while the hold
  owns the line and the scan is skipped in the re-read window (`detect_cc_orientation`, `poll()`); `RdSeek::feed_open` takes
  a level only with no Rd presented, 150 ms after the last SWITCHES0 pull-down change, and confirmed by a second sample from
  a distinct ADC refresh >= 20 ms later within 250 mV (`rd_changed` hooked in `wr()` and init); `IdleWake` counts
  unanswered dwells per line and after two on one line releases Rd for 600 ms ("Idle wake: no Rp on CCn after N dwells")
  so the open levels are re-read and the signature picks the line. The 1.69 V on CC2 is the board's own bias net, not our
  measure; by level alone it equals a 3 A Rp over a sink's Rd (330 uA x 5.1 kOhm), so it is explained, not filtered. A boot
  scan before any level is known can still attach it (dark attach, released after 3 s). Host-tested
  (`tests/test_rdseek.cpp`, `tests/test_fusb302_scan.cpp` wake2 cases); hardware check pending.
  Also seen twice on 2026-11-10 (a5bd83f, 05b1703): the first `ct_start.py` START within ~10 s of a boot on this
  splitter ends "START did not start a run" (VBUS 0 / 2.75 V, "START will present Rd and wait for VBUS"); the second
  START, with the port awake, runs. Check START's VBUS wait against the ~1.1-1.7 s wake and log the reason it gave up.
  - Merge pass 2026-11-10: `k2/sleeping-port-rd` (f11ea84, `pd::IdleWake`) was **held, not merged** as an always-on
    behaviour; it now ships only as the opt-in setting above (default off, I1 / H2 unchanged with it off). With it on it puts Rd on any
    idle line that reads >= 2800 mV open, which is also every ordinary waiting charger (Rp 3.2-3.3 V) and a dual-role
    phone's toggling Rp (H13). That breaks invariant I1 / row H2 in `docs/CONNECTION_MATRIX.md` (a plain boot presents
    no Rd until START or a re-read), makes the K2 the sink of record before anything is plugged behind it (the P4
    hand-back problem) and would have a DRP phone power the K2. The explicit paths already wake the splitter on
    hardware (START and a plain `pd-reread` held Rd 1648 ms on one line, 2026-11-10). Remaining fix, smaller: START's
    VBUS wait covers the ~1.1-1.7 s wake (the first-START miss above) and the idle UI says "source asleep: START or
    Refresh wakes it" on the standby signature. Decision for the user if the idle wake is still wanted.
  - Hardware 2026-11-10 (14f9d24, Anker IQ4 -> Anker 2-way splitter, other output empty -> C-C cable -> K2, HID-powered,
    `runs/2026-11-10-merge2-hw/wake3`): asleep = CC1 3.04 V, VBUS 0, K2 passively attached on the CC2 bias net (1.69 V).
    Wake ON -> the dark attach is released, Rd held on CC1 (sw0=0x01, the standby Rp collapses to 0 V under it) for two
    steady dwells; the splitter's Rp answered 3.86 s after the Rd, the scan attached CC1, 6 PDOs, VBUS 5.08 V. Two
    earlier hardware rounds failed (48adfa1 and before: the passive scan took the line during the hold after 0.28 s,
    then stale ADC samples picked the wrong line; fixed in c3f46d1 / e0a4ada). Still open: the boot scan can attach
    the CC2 bias net first, so the wake starts up to ~3 s late (dark-attach release).
- **A leftover Rd pin makes an inline K2 Request at attach** (2026-11-10, operator note, fixed in b60c702 + ce6bbdb + df8e004,
  hardware check pending): with `pd-reread --pin 2`
  still set, splitter -> K2 -> OnePlus 15: the K2 sent two Requests (one collided with the phone's) before the far
  Request made it go passive (~2.3 s). The pin is a lab option and did what it says; the gap is that nothing shows it
  is still set. Fix: show the pin on the Home status line (e.g. "Rd pin CC2") and in `pd status`; clear it on
  detach of the far end. Done: the Home status badge (the VBUS cell's pill) reads "Rd pin CCx", the rotation pages'
  chrome-bar badge does too (was the bare "SNK"), `pd status` carries the appended pin byte, and a lab pin (only
  `pd-reread --pin`) clears with its mark on the far end's detach; a boot / comeback pin keeps its Rd.
- **`k2_remote.py cards` cannot read format v8 cards** (2026-11-10, fixed in 26f297f, hardware check pending):
  "card payload is 198 bytes, expected 144, 162,
  178, 190 or 196" after b193e56 made the shared card v8 (rebalanced count). Add the v8 size and field to the host
  decoder (`tools/k2_remote.py`, same layout as `tools/report_card.py`), with a test. Done: the card read length is
  derived from `report_card.PAYLOAD` (198, v8) and `cards list` / `cards show` render the rebalanced count.
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
    hardware check pending. The automatic hand-back Fix above is still open (Step 0 first).
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
    pps-footer*.png`, `pps-sniff`): footer "CC 9V", no current, no "max" label - correct. Still open: `fc: PD MTK`.
    A settled fixed contract is silent (no PD frame in 14 s), so no Request is ever decoded and pd_active() stays
    false; the D+/D- classifier reads the phone's lines as MTK. Rule: CC attached, VBUS above vSafe5V (> 5.5 V) and
    no D+/D- handshake seen this attach = "PD (not decoded)", never a legacy mode. The "PPS 9V" label is still
    unverified on hardware: needs a PPS sink (OnePlus 15 on the CUKTECH; it takes only PDO1 5 V on the Anker IQ4,
    with or without the K2).
- **Sink Identify inline drops the phone from 9 V to 5 V** (2026-11-10, operator report, cause captured, 05b1703,
  Anker IQ4 100 W -> K2 -> OnePlus 15 on PPS PDO 6 at ~9 V 2.3 A, `runs/2026-11-10-connection/identify/bb.txt`).
  **Fixed in 6df54fa, hardware check pending** (fix items (1) and (3) done: inline Identify / Get_Source_Cap /
  cable read over SOP' / VDM probe all refuse inline with no confirm, the UI confirm paths are removed, and the
  Sink tab shows the sniffed identity marked "sniff"; item (2), active Identify of a phone alone on the K2, is a
  later batch).
  The confirmed inline Identify (`Action::Identify` -> `fusb302::identify_start(true)`, `include/k2/pd/pd_identify.hpp`)
  transmits with the K2's own sink header (**power role Sink, data role UFP**, MessageID from 0), i.e. it speaks as a
  second copy of the phone on a wire where the phone and the charger already have a contract.
  - Run 1 (239160 ms): Discover_Identity REQ -> **both** partners answer: the phone ACKs (VID 0x05C6 PID 0x209E) and
    the charger answers Not_Supported. Discover_SVIDs and Get_Sink_Cap each went out twice (RETRYFAIL, no GoodCRC:
    two receivers' GoodCRCs collide). The phone answered Get_Sink_Cap with Sink_Capabilities (5/9/12 V 3 A, PPS
    5-11 V 5 A); the charger, in its Ready state, got an unsolicited Sink_Capabilities from its sink and sent
    **Soft_Reset 9.4 ms later**, then Source_Capabilities, then a **Hard Reset** 2 ms after that (the ordered set
    carries no header: sender not identifiable from the trace).
  - Run 2 (263018 ms): one Discover_Identity REQ alone is enough: the phone's ACK went out three times (nobody
    GoodCRCs it), the charger answered Not_Supported twice, **Hard Reset 24.6 ms after the K2's TX**.
  - After each Hard Reset: VBUS 0 for ~0.85 s, the charger re-sends the same 6 PDOs, and the phone Requests **PDO 1,
    5 V 3 A**, and stays there (still 5.05 V 2.47 A minutes later). It went to PPS 9 V after a full unplug/replug
    earlier in the evening, so a Hard Reset does not bring this phone back to its fast contract.
  - Cause in one line: an inline K2 has no PD role of its own; any message it sends is a second sink talking out of
    sequence, so both partners answer, their GoodCRCs and MessageIDs collide, and one side resets the link.
  - Fix: (1) inline Identify never transmits: it shows what the charger's own Discover Identity and the phone's
    messages already revealed (sniffed: the phone's Discover Identity ACK, Sink_Capabilities and Requests are on the
    wire in normal operation), with "from sniff" on the result; the confirm path is removed, not just warned.
    (2) A phone alone on the K2 (H13/H14: the K2 shows Rd, the phone sources 5 V) is identified actively, as the
    phone's sink partner, with no charger to disturb: offer that path when no source is attached.
    (3) The same rule for every inline TX (Get_Source_Cap, Read cable over SOP, VDM probes): refused inline, no confirm.
  - Validate: host tests: inline -> `identify_start` refuses even with confirm; the sniffed identity fills the Sink
    tab. Hardware: Anker IQ4 -> K2 -> OnePlus at 9 V, Identify shows the phone's VID/PID from sniff, black box shows no
    K2 TX, the phone stays at 9 V.
- **Charger test: "Cable OFF" says nothing about what it does** (2026-11-10, operator report): the wizard's step 2
  reads "Cable" with the value "OFF", which says nothing about what it does (the e-marker identity the K2 presents to the
  charger, the same list as Tools -> Spoof). Operator decision, wording only, nothing else in the flow changes (the
  Cable step stays as it is, remembered pick or not):
  - Step 2 title "Spoof cable"; its first row "Off (real cable)"; the built-ins and clones keep their names and ratings.
    Review row `Spoof  Off` / `Spoof  EPR 50V 5A`.
  - The guard's Refuse stage ("Cable on plug": a real cable between the charger and the K2) says why: "Cable connected:
    the spoof won't reach the charger." Two options: `Start anyway` and `Start, skip spoof` (this run without the
    identity; the remembered pick stays). `Cancel` goes: Back already returns to Review.
  - Docs (USER_GUIDE, CHARGER_TEST_FLOW "Wizard steps" and the Refuse stage, UI_DESIGN), `ct_start.py --cable` help and
    the remote/web stage names use the same words.
  - Validate: host tests on the step title, the first row, the Review row, the Refuse text and its two options, and
    that "Start, skip spoof" leaves the stored pick unchanged; hardware screenshots of step 2, Review and Refuse.
- **Crossed CC through the K2: tell the user to flip a cable** (2026-11-10, fixed in 7d1ae17, hardware check pending, connection matrix H22, be71f10, `runs/2026-11-10-merge2-hw/`
  dead-chain, attach-noreact.png): Anker IQ4 -> C-C cable -> K2 -> C-C cable -> Pixel, VBUS never came up, the K2 sat at
  "No charger". The K2 passes CC1->CC1 and CC2->CC2, and each cable's orientation picks which pin its CC wire lands on,
  so the charger's CC and the phone's CC can end up on different pins and never meet. Evidence: Anker alone = steady
  3.30 V Rp on CC2; Anker + Pixel = CC2 steady 500 mV (the Anker's Rp loaded only by the Pixel cable's e-marker Ra,
  not a sink, so the Anker never enables VBUS) while CC1 cycled 0 / 1.2 / 1.8 / 3.3 V (the Pixel toggling alone).
  Flipping the Pixel's cable at the K2 fixed it at once; it had worked three times before by orientation luck. Not a
  K2 bug (any straight CC pass-through has it), but the K2 is the one thing that can see both pins.
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
- **A dual-role phone alone reads as "CHARGER WAITING"** (2026-11-10, connection matrix H13, 05b1703,
  `runs/2026-11-10-connection/H13/`, fixed in 9189def, hardware check pending): OnePlus 15 alone on the bottom port,
  HID-powered K2: behaviour correct (no Rd, no TX, VBUS max 1 mV over 60 s), but the Source tab said "CHARGER WAITING /
  start a charger test" while the bar said "No charger, via cable". The phone toggles Rp/open about every 100 ms
  looking for a partner (STATUS0 BC_LVL 3 <-> 0 in the trace); a charger's Rp is steady.
  - Fix: the waiting classifier takes Rp duty and period into account (raguard::RpTrace over the per-tick CC samples,
    fed by pd::Link): Rp present only part of the time with 50-100 ms toggles (>= 3 edges in 400 ms) =
    "DUAL-ROLE PORT", not a charger; steady Rp for >= 300 ms stays "charger waiting". One classifier, one result:
    the Source tab, the bottom bar and `state.waiting` (kLinkWaiting) all read pd::Link::wait_kind(). START on a
    dual-role port explains that the phone will source 5 V (H14).
  - Also: three readers of "waiting" disagree. On a real waiting charger (sleeping splitter port behind a CUKTECH, CC2
    2.99 V, `runs/2026-11-10-splitter-cuk/wake/pd.png`; Baseus 140 W direct on the top plug, CC1/CC2 3.20 V) the Source
    tab says "CHARGER WAITING", the bottom bar says "No charger, via cable" and `k2_remote state` says `waiting: False`.
    One classifier, one result: the tab, the bar and the `state` field read the same value.
  - Validate: host test on the classifier with a toggling and a steady trace; hardware: repeat H13, then the Baseus
    alone (still "CHARGER WAITING", bar and `state` agree).
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
- **Rd seek leaves before a slow source wakes (`slow-rp-wake`)** (2026-11-09, fixed in 5926abb, hardware check pending, Anker 2-way splitter behind a Baseus 140 W,
  dock on its other output, HID-powered K2 on 85b9ff0; `runs/2026-11-09-splitter/`). A re-read on a plain boot got
  nothing: the Rd seek put Rd on CC1, CC2, CC1 for ~200 ms each (`kRdSeekDwellMs`, `include/k2/pd/pd_rdseek.hpp`),
  every attach read BC_LVL=0, "no Source_Capabilities came back". With the Rd pinned on CC2 (`pd-reread --pin 2
  --no-run`) the splitter's 3 A Rp appeared **1072 ms** after the Rd, VBUS 5.1 V 28 ms later, Source_Capabilities at
  +1243 ms (5/9/12/15 V 3 A, 20 V 5 A, PPS 3.3-11 V 5 A; `bb-pin2b.txt`). A phone on the same port also wakes it late.
  - Signature (behaviour, no VID:PID): with no Rd of ours a CC line reads ~3.0-3.3 V (the source's standby sink-detect
    pull-up, high impedance), and under our Rd it falls below 200 mV (BC_LVL=0). A real Rp never does that: even the
    default-USB Rp holds ~0.4 V against 5.1 kOhm. So "high open, collapses under Rd" = a source that is there but asleep.
  - Fix: the scan keeps the open-level reading per line from before our Rd went on. When the Rd seek's line read the
    standby signature, stay on that line for `kSlowWakeDwellMs` (~2000 ms; measured 1072 ms) before moving, and if the
    other line had it, go there first. Lines without the signature keep the 200 ms dwell (the wrong-line case of
    d615f3e). The 500 ms no-Rp detach on the active line must not fire inside that window. Name the pattern
    `slow-rp-wake` in `docs/CHARGER_QUIRKS.md` (signature, the 1072 ms figure, handling). Apply the same dwell to the
    charger-test START attach and the rescan bridge, which use the same seek. Never Rd on both lines (Debug Accessory).
  - Tests (red first): `tests/test_rdseek.cpp`: standby signature on CC2, Rp after 1100 ms -> no move, attach on CC2;
    no signature -> unchanged 200 ms moves; standby on CC1 while the pick was CC2 -> CC1 first. `make gate`.
  - Validate: same setup (splitter port idle, dock on the other output), plain boot, `k2_remote pd-reread` with no pin:
    Source_Capabilities within ~1.5 s, no line move after the Rd lands on the standby line; then `ct_start.py --units pd`
    attaches without `--pin`.
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
- **Flaky host tests** (2026-11-09, fixed in 1feba0e, hardware check pending): `tests/test_k2proto.mjs` "screenshot: chunked strips with log noise" and the C++
  `test_dbg_stream` (fails when its worst pass is over 5 ms on a loaded machine) failed once each in `make gate` and
  passed on re-runs.
  - Fix: applied -- remove wall-clock limits from functional tests (count work, not time): the scripted link rigs run
    with their watchdogs disabled (`timeoutMs`/`streamTimeoutMs` 0, a Link option since this fix) and the per-pass
    cost CHECKs run only under `K2_TEST_TIMING=1`, outside `make gate`. `tools/flaky_repro.sh` (eef3f6a) reproduces
    the red state on demand; after the fix it is 10/10 green under a 128-hog load.
  - Validate: done -- `make gate` passed 10/10 in a row with all host tests re-run each round; runs 1-6 ran while a
    parallel `make -j32` build (`BUILD_DIR=build-load`) ran in the same worktree.
- **Wrong comment** in `tools/k2_qr.py`: "a space can end a part" is false (the last base45 character of a part is at
  most index 32; a space is 36).
  - Fix: correct the comment. Validate: `make gate`. Fixed in 8f4acaf, hardware check pending.
- **Charger-test journal keeps its own row order** (2026-11-09, fixed in 5a0220f, hardware check pending): `charger_journal.hpp`
  `detail::plan_order` placed the 6 A hunt rows by its own rule when it rebuilt the steps on a resume: a third copy of the
  order the grid fix removed. The header (format 3) now carries the plan's order, written from the planner's step list
  (`Runner::steps()`, the same structure the grid uses since 98e2f39), and replay applies it verbatim.
  - Fix: rebuild from the planner's step list (the same structure the grid uses since 98e2f39).
  - Validate: host test resuming a journal mid-PD with Mi 6A rows: resumed step order equals a fresh plan's order
    (`test_a_resume_mid_pd_keeps_the_planners_step_order`, green since 5a0220f).
- **A PPS row shows a probe after all its APDOs** (2026-11-09, residual of the grid fix, fixed in dfc5918, hardware check
  pending): a PPS row held the chips of every APDO, so a Mi 6A probe the planner runs between APDO 1 and APDO 2 read after the
  whole PPS row. Now one grid row per APDO (`Row::pos`), so the probe row stands between the APDO rows where it executes.
  - Validate: host grid test with two APDOs and a probe between them: the grid order equals the executed order
    (`tests/test_result_layout.cpp` `test_probe_between_two_apdos_stands_between_their_rows`, plus the per-APDO keys in the
    `grid_follows` tests of `tests/test_charger_test.cpp`); `make gate`. Hardware: on a two-APDO PPS source with a Mi 6 A
    probe between the APDOs (e.g. the CUKTECH 120 W), the Running/Results grid shows the "Mi 6A" row between the two "PPS"
    rows, in execution order.

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

- **PPS coverage: floor, 20 mV steps, accuracy spread** (2026-11-10, user decision; start after `k2/pps-needs-load`
  is merged - both touch the PPS rows in `src/chargertest/charger_test.cpp`; **fixed in f733009 (items 1-3),
  hardware check pending**). Today a PPS APDO gets 7.14 V, 11 V,
  the 7.14 <-> 11 V jumps, 7.14 -> 5.22 V and the full-current top row; the low edge (< 5 V) lives in the separate
  `PdLow` unit (patterns `latch-off-low-pps`, `cut-and-recover-low-pps`). All new rows stay polite: inside the APDO,
  at or below its current, 1 A like the other plan rows.
  1. Floor (done in f733009): the `PdLow` low-edge row must report the measured floor, not only pass/fail - a source that advertises
     3.3 V but regulates at 5 V is a fail "floor at 5.0 V" with measured_mv = the reached level (reuse `Capped`'s
     ceiling logic mirrored for a floor). Keep it in `PdLow` (it can cut the output); the results tab names the floor.
  2. Step resolution (done in f733009): one row per PPS APDO, from a mid level (e.g. 7.0 V), five Requests of +20 mV each: every step
     needs Accept + PS_RDY and a measured VBUS move of 20 mV +- the meter's resolution (state the tolerance from the
     INA226 LSB and the existing band rules); a source that rounds to 100 mV or ignores small steps fails "step
     resolution N mV". A phone's PPS loop walks in 20-40 mV steps (OnePlus 15 capture, `runs/2026-11-10-op15-sniff/`).
  3. Accuracy spread (done in f733009, card format v9): three or four levels across the APDO (low, mid, high, spaced by the range) record the setpoint
     error in mV per level on the card (not only pass/fail), so a converter that drifts at the ends shows. Card
     format bump if a field is needed; every host decoder (`k2_remote.py`, `k2proto.js`, `qr_corpus.py`,
     `report_card.py`, `card.js`) in the same change.
  - Validate: host tests (red first) for each new row's verdict, including the failing shapes (floor at 5 V, steps
    of 100 mV, steps ignored); hardware: a source with a working PPS (CUKTECH 10 or FH22 brick) passes all three,
    the splitter shows "needs load" (pattern `pps-off-at-no-load`).
  - Hardware 2026-11-10 (CUKTECH C1, `--units pd`, `runs/2026-11-10-merge2-hw/`): item 2 first failed both walks
    "step resolution 15mV" - the +-2 mV per-step band was the meter's LSB, but the source's own ripple at no load
    moves one reading by several mV. Changed in 91a9839: each step must move up and stay below 40 mV, the five must
    sum to 100 +- 20 mV (detail = mean move). Re-run: both walks pass (8000 -> 8109 mV, 12500 -> 12590 mV), PD
    19 pass / 0 fail / 5 skip. Item 3 verified the same day (`--units pd,pd-low`, card 55, fw be71f10,
    `runs/2026-11-10-merge2-hw/cards.json`): `err_mv` 5-11 V APDO +10 / +20 / +8 / -12 mV, 5-20 V APDO
    +6 / +3 / -9 / -49 mV. Gap: `k2_remote.py cards show` prints only "whole range", not the spread - print the four
    errors there too. Item 1 (floor) verified, pass shape (48adfa1, Anker IQ4 3.3-11 V 3 A, HID-powered K2,
    `runs/2026-11-10-merge2-hw/iq4-pdlow.txt`): 3300 mV -> 3285 mV pass; the same run's step walk 7140 -> 7245 mV
    pass, 12 pass / 0 fail / 3 skip. The fail shape ("floor at 5.0 V") still needs a source that floors.
- **PPS deep sweep, a separate entry in the charger-test Tests list** (2026-11-10, user decision; after the coverage row
  above). Opt-in, never in the default preset: every 100 mV across each PPS APDO (and SPR AVS range), 1 A, Accept +
  PS_RDY + measured VBUS per point, results as a compact error-vs-setpoint strip on the results tab and the card
  (min / max / worst error and where). Long by design (about 77 points for 3.3-11 V, about 180 for 3.3-21 V), so the
  Tests list shows it as its own line with a point count. Stops at the first lost source (pattern handling as the
  other PPS rows). Validate: host tests for the point plan and the summary; hardware: one full sweep on a working
  PPS source, no reboot, journal intact.

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
