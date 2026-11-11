# Charger test flow: units, sessions, order

How the charger test is organised: independent units, each with a session mode (Pd or Legacy, PD-silent), the order they
run in, how the results grid and the flash journal work. Code: `src/chargertest/charger_test.cpp`; the unit lifecycle
is also summarised in `docs/ARCHITECTURE.md`. Hardware-verified on a Lenovo C135 (QC2/QC3 pass in the PD-silent session).
Exactly one owner of the Request exists in Pd mode, and all Legacy units share one PD-silent attach.

## Why

The old runner kept one sink alive across the whole run and switched its behaviour mid-way (PD contract for the
PD groups, a 5 V "in contract" state for the legacy groups, a re-attach for the low PPS rows). Chargers that disable
D+/D- protocols while a PD contract exists (Lenovo C135: QC2/QC3 pass only when no PD contract was ever made) fail
every legacy row. The PD reflexes (answer re-announced caps, re-request after Hard Reset, rescan nudge) fight the
legacy phase. Fix: split the run into independent **units**, each declaring the session mode it needs.

## Unit contract

A unit is one protocol group (PD fixed, PPS/AVS safe, PPS/AVS low, QC2, QC3, QC3+, AFC, FCP, SCP, VOOC, SVOOC,
SVOOC65, UFCS, VIVO, EPR, ...). Every unit:

1. **Starts in a session of its own mode**: detached first (Rd off, VBUS below vSafe0V, or the USB-A port idle) and
   freshly attached, except that a Legacy unit following another Legacy unit continues that unit's still-open
   PD-silent session (see "Sessions"). No state is inherited from the previous unit except the read-only run facts
   (heard caps, cable, identity).
2. **Declares its session mode** and attaches in it:
   - `Pd`: Rd on, sink active. Caps within ~2 s, Request from the PD engine within 24 ms (tSenderResponse), answer
     every caps (re-announce too), re-request after Hard Reset.
   - `Legacy`: Rd on, sink **PD-silent** (no GoodCRC, no Request, no re-request, no nudges). Caps may be heard and
     recorded but never acknowledged. Ready when PD traffic has been quiet for the settle time and VBUS is steady at
     5 V (the source gave up PD). USB-A port: no CC, ready once VBUS is steady at 5 V.
3. **Runs its rows**, judging VBUS on the latest sample (never the 2.5 s display average; a PD fixed row above 5 V is
   judged on the 250 ms running mean `VbusMean` instead, pattern `burst-mode-ripple-at-20v`, and the step walk and deep
   sweep record a window mean, see "PD row judging"), with a transition capture
   armed per row.
4. **Ends with a release**, and with a detach (Rd off, wait for VBUS dark, bounded) at a session boundary: back to 5 V
   (PDO 1 / D+/D- released). Always runs, including after a failure.
5. **Reports per row** (Pass/Fail/Skip + Reason) and a unit outcome. A unit failure never stops the run; only a
   source that stays dark past the recover limit (`ShutOff`) skips the remaining units.

Units can be reordered freely: the run order is just a list, and a list run alone attaches in each unit's own mode.
Iterating on one protocol = running a list with one unit.

## Sessions

PD and D+/D- handshakes are never mixed in one attach: all legacy probes run in one PD-silent attach, PD comes in
another. The runner does this:

- A **session boundary** (detach, then a fresh attach) happens only where the session mode changes: Legacy to Pd, and
  Pd to PdLow (the risky low PPS/AVS rows). Consecutive Legacy units share one PD-silent attach: no detach and no
  re-attach between them.
- Between two Legacy units: the release drives D+/D- to 0 V for 500 ms (`kLegacyGapMs`), then Hi-Z; VBUS must be back at 5 V and the line PD-quiet, then the next handshake starts. AFC waits a
  further 500 ms (`kAfcPreMs`) before it starts.
- The first handshake of a session waits about 1000 ms after the PD-silent attach has settled
  (`kLegacyFirstWaitMs`). The QC3 return-to-5 V (below 5.8 V for 300 ms) is the release settle; the 200 ms wait after a
  QC3 entry belongs to the fast-charge engine.
- Whatever starts a D+/D- detection (a unit, the remote trigger, a retry, a scan) finds both lines held at 0 V for at
  least 500 ms after the previous session's release: the engine itself stamps the release and lengthens the discharge
  wait, so this does not depend on the runner's gap.
- QC3 / QC3+ rows run a closed loop (docs/FASTCHARGE_PROTOCOLS.md section 1): VBUS must be stable after each batch,
  single pulses for the last QC3 steps, aim at target - 100 mV, pass band -300 / +150 mV, a level above the expected
  pulse count is an overvoltage abort. Every batch writes a Black Box `qc3-batch` mark.
- A QC3+ attempt where VBUS never leaves ~5 V (100 pulses, still within 150 mV of the entry level) is "no QC3+
  response": the row and the group are `Skip` (`Reason::NoQc3Plus`), not `Fail`; the group verdict is "skip, no
  response", and every reason table (card, JSON, k2_remote, web) carries the name.
- QC Class B (`Qc2ClassB`: one 20 V row; `Qc3ClassB`: the 5/12/20/12/5 ramp) is part of the default selection and runs
  inside the QC2 / QC3 units, after their Class A rows. The K2's own limits stay in front of the 20 V request (the
  run's voltage cap and the monitor ceiling: a cap below 20 V gives `OverCap` before any pulse). A Class A charger that
  stops at 12 V does not follow the 20 V request: the 20 V row (a request that times out or lands outside the band) is
  `Skip` with `Reason::ClassAOnly` ("Class A only (12 V max)", reason 35, after `PdGaveUp` = 34, schema 1 maps it to "not advertised"), the
  rows that follow it in the ramp skip with the same reason, and no transition capture is stored. Results and the
  card show the row dim with a neutral "Class A" chip, the group stays Pass on its Class A rows, and the host summary
  never counts it as a failure. A Class B charger that reaches 20 V shows a normal pass.
- The legacy/QC section of the results and of the report card shows a passive "QC4+" / "QC5" label from the card's facts
  (Qualcomm VID plus a PPS range; QC5 at 100 W or more). It never changes what the test does.
- The shared session is kept only while the line stays PD-quiet at 5 V. If PD traffic returns, or VBUS leaves 5 V,
  between two Legacy units, the runner falls back to a detach plus a fresh PD-silent attach: a unit never runs in a
  session the charger may have changed.
- A **Pd attach wait** is bounded: after the caps arrive, the 5 V contract (the engine's Request for PDO 1, Accept +
  PS_RDY) has `kAttachContractMs` (3000 ms). When it runs out, the cases are told apart: no caps at all means the
  unit's rows fail with `Reason::NoReattach` ("no PD after re-attach"), but a granted PDO 1 contract (PS_RDY seen)
  with VBUS outside vSafe5V fails with `Reason::WrongVoltage` ("wrong voltage on 5V contract", reason 43, pattern
  `shared-bus-split` in `docs/CHARGER_QUIRKS.md`) and the row's measured value is the HIGHEST VBUS seen during the
  wait, not the reading at the timeout. A source-side Rp drop during the wait (caps gone, no active CC) restarts the
  wait once (`slow-rp-wake`: the standby pull-up answers ~1.1-1.7 s after a steady Rd) instead of letting the timer
  run on.
- A **programmable row dropped at no load** is a skip, not a failure (pattern `pps-off-at-no-load`, suspected, in
  `docs/CHARGER_QUIRKS.md`). The runner latches per Request: the sink was Accepted (it waited in Transition), PS_RDY
  was never seen for the row's own PDO and level, and the highest current the port drew. A PPS / SPR AVS / EPR AVS row
  whose source is then lost (after the one retry) with VBUS under 85 % of the row's level and a current under
  `kNeedsLoadMaxMa` (50 mA) ends `Verdict::Skip` / `Reason::NeedsLoad` ("needs load", reason 44), measured = the
  collapsed VBUS. It is no chain skip: the other levels run once the source is back (Releasing waits for it, as for
  Lost). Everything else keeps its reason: a fixed row (Lost), a seen PS_RDY (Lost), a current over `kLoadAbortMa`
  (Inline), a Hard Reset (HardReset), a Reject, a low-edge cut (CutRecovered). On the results tab the PPS chip reads
  "needs load" in the skip style and the group summary does not count it as a fail.

## What a legacy 5 V row proves

5 V is what every source delivers with no protocol at all, so a legacy row never passes on VBUS alone. A row passes only
with the charger's own answer, and a unit whose first (5 V) row fails is shown as one "No reply" row in the Results grid
and the card (`result_layout`, `is_absent`), not as a full unit.

| Protocol | The 5 V row passes when | A plain 5 V source (no D+/D- reply) gives |
|---|---|---|
| QC2, QC3, QC3+ | the HVDCP handshake completed: D+ at 0.6 V, D- released, and D- fell by at least 350 mV (the charger opened the D+/D- short, about 1.25 s later); QC3/QC3+ also entered continuous mode | Fail, no handshake |
| FCP, SCP | the ACCP register exchange was answered (an unanswered read or write aborts the row; SCP at 5 V ends as "detected" once every identifying read answered) | Fail, no handshake / no response |
| AFC | the ping byte was answered | Fail, no response |
| UFCS | the charger acknowledged our frames and sent its output capabilities | Fail, no handshake / no response |
| VOOC, SVOOC, SVOOC65 | the adapter sent command frames after our wake train (identification decoded plus an SVOOC-only command is "detected"; accepted and ramped is a pass) | Fail, needs load |
| VIVO | the charger sent a reply frame (cmd 0x08) or its entry notice (cmd 0x55); any other frame, including the K2's own request echoed back through a D+/D- short, is ignored | Fail, no response |

QC2 detection reads the D+/D- state, not the voltage: a DCP or BC1.2 charger keeps the lines shorted and D- stays at
0.6 V, an Apple divider holds D- at 2.7 V, and a bare pin reads 0 V. None of them is a QC charger, and none of them lets
D- fall from 0.6 V after the 1.25 s wait, so the engine never reaches the QC states (`fc_engine.cpp` HsBias / HsWaitDrop,
`qc::handshake_initial_ok`, `qc::handshake_dropped`). QC3 and QC3+ 5 V prove the HVDCP handshake plus the entry into
continuous mode; the pulse response is proven by the later rows of the same unit (a QC3+ attempt where VBUS never moves
is `Reason::NoQc3Plus`, a Skip).

Host tests: `test_charger_test` (`test_silent_source_fails_every_handshake_row` runs every legacy unit against a silent
5 V source and expects no pass; `test_answered_handshake_passes_the_5v_row` expects a pass once the handshake is
answered), `test_fc_engine` (`plain_sources_are_not_qc`, `hvdcp_5v_needs_the_handshake_and_passes_with_it`) and
`test_fc_vivo` (`own_echo_is_not_a_charger`).

## SCP and UFCS rows follow what the charger reported (2026-11-08)

START plans the fixed rows of both units, because nothing is known of the charger yet (`Runner::build` / `preview` hand the
planner no `Input::fc_caps`). The unit's first row, the 5 V one, is the same either way and reads the limits: the SCP
session reads the adapter's voltage and current registers, the UFCS session receives Output_Capabilities (`fc::Caps`,
kept by the engine after the session ends, `fctrig::caps()`). When that row passes, `Runner::replan_fc` plans the unit
again with the reported limits, once per unit: it replaces the unit's block of rows in place (the cursor and the rows
before and behind it keep their place), records the new Skip rows, and calls `Env::plan_changed()` only when the rows
changed. A failed first row, a frozen plan, or a block that does not fit the plan's capacity leaves the fixed rows.

| Unit | Rows with no report (fixed) | Rows with the report |
|---|---|---|
| SCP | 5 V detect, 10 V at 2.25 A, 4.5 V at 5 A, 5 V at 4.5 A | 5 V detect, 10 V at min(2.25 A, reported max), the reported top level when it is above 5 V and not 10 V (within the monitor ceiling and the cable), 4.5 V at 5 A, 5 V at 4.5 A. A row whose voltage is outside the reported window, or whose current is above the reported maximum, is a Skip row (`no offer`): it is never requested |
| UFCS | 5, 9, 11, 5, 12 V, then 20 V at 3 A | 5 V, then per advertised mode (lowest first) its bottom, middle and top level, each at 1 A (the mode's smallest current when that is higher, never above its maximum), a level shared by two modes once, then 5 V again judged in the mode that reaches 5 V. The top is clipped to the monitor ceiling and the cable; a mode that lies wholly above it is one named Skip row (`over cap` / `cable limit`) |

Each row planned from the report names the mode it samples: `Step::pdo` (unused for UFCS otherwise) packs the mode's
voltage window and current range (`pack_ufcs_mode` / `ufcs_mode_of`, 9 + 9 + 7 + 7 bits in 100 mV and 50 mA units; 0 = no
mode, so the fixed rows and a journal written before this change read as before). The Results and Running grids word a
UFCS chip with the mode's top current, `5V 3A`, `12.5V 3.25A`, exactly like a PD fixed row, so the grid draws one bracket
per run of equal current (a charger with 5-11 V 3 A and 5-20 V 3 A shows `5V 8V 11V` and `12.5V 20V` under one `3A` each; a
mode at 4 A, the 44 W Baseus brick, shows `5V 5.5V 8.2V 11V` under `4A`). The Detail page of a UFCS step replaces its
`Header` row with `Mode`: the whole mode, `5-20V 1-3A` (the top current only when the range would not fit the 16-cell
value column). The Steps and Running name lines stay volts only. Report card format 6 carries the modes of the latest run
(`report_card::UfcsRange`, up to 4, in planner order: deduped, first appearance; a fifth mode is dropped): three bytes per
mode at payload offset 178, bits 0..7 the low voltage and bits 8..16 the high voltage in 100 mV units (the low saturates at
25.5 V), bits 17..23 the current in 50 mA units (saturates at 6.35 A); a slot with a high voltage of 0 is unused and ends the
list. The K2 card detail sheet of the UFCS row lists `Mode 1..4` (`5-20V 3A`), the web page shows the same per mode and its
UFCS badge names the top mode. Cards of format 1 to 5 carry no modes (`ufcs_modes()` is 0): they show volts only, and a run
that updates such a card rewrites it as format 7. A card update does not inherit modes from the old card.

The charger test stays polite: no row asks a voltage or a current that the adapter did not advertise. The SCP current
limit read this way is also kept as `Evidence::scp_ma` and shown as "SCP cap 20V 6A" in Results, on the open card (a
fact line under the handover row) and in the card JSON (`scp_ma`, mA). The card holds it in bits 1..7 of the cable block's
`src_flags` byte in 50 mA units (so it clamps at 6.35 A; bit 0 stays the OPPO SVID note). The voltage in the words is the
highest SCP level that passed, which is the adapter's own top level whenever the planned top row passed; a card whose SCP
group failed names the current alone. Cards written before the bits meant anything (format 4 and older) show no note. The
value is not journaled: a run resumed after a power loss keeps the rows (they are in the journal) but loses the note.

Host tests: `test_charger_test` (`test_scp_rows_follow_the_reported_capabilities`, `test_scp_replan_is_announced_once`,
`test_ufcs_levels_follow_the_reported_modes`), `test_accp` (`test_scp_caps`: the limit decode), `test_fc_engine` (the over-limit refusal),
`test_report_card` (`test_scp_capability_note`), `test_result_layout` (`test_scp_capability_note_row`).
The UFCS current words: `test_charger_test` (`test_ufcs_steps_carry_their_mode`), `test_step_label` (`test_the_ufcs_mode_words`,
`test_every_ufcs_mode_range_fits_the_detail_value`), `test_result_layout` (`test_ufcs_chips_*`), `test_ui_charger_test`
(`test_ufcs_results_and_detail_show_the_mode_current`).

## Repeated runs (report card format 7)

A charger has one card (`/cards/<fp>.card`, the newest run). When a finished run gives the same results as the stored card, no
second copy is written: the store folds the run into the stored card (`report_card::same_results`, `fold_repeat`, applied in
`Store::put`). The card keeps its rows and the boot / uptime of the run that was stored first; `runs` goes up (the charger was
tested again), and a repeat block records how often and when:

| Field | Payload offset | Meaning |
|---|---|---|
| `times` | 190, u8 | how many runs gave exactly these results (1 = seen once; saturates at 255) |
| `last_boots` | 191, u16 | boot counter of the newest of them (saturates at 65535) |
| `last_uptime_s` | 193, u24 | uptime at the end of the newest of them (saturates at 2^24 - 1 s) |

The K2 has no clock, so "when" is the boot counter plus the uptime in that boot. A card of format 6 or older reads as `times = 1`
(offset 190 held its CRC, the reader looks at the block only when the format byte is 7 or more) and is rewritten as format 7 the
next time it is updated.

**Identity rule.** Two runs are the same when every stored semantic result is equal: each group's verdict, reason, detail and
failing target; each PPS / AVS range's offer, failing step and reason; each EPR level; the UFCS modes; the PD offer facts (PDO
count, spec revision, evidence flags, highest fixed current, source flags); the handover outcome; the final state and reason; the
firmware build; the cable's identity (vid, pid, kind, limits). The measured maxima (group maximum, range maximum, EPR maximum)
compare within one stored step, 100 mV: the card keeps nothing finer, so a level on a rounding boundary that fell into the next
step is noise, not a change. Never compared: boot counter, uptime, the repeat block, card number, run count, the name, vid / pid /
rated power (a probe that answered once and not the next time is no change; the card learns what the repeat adds), the handover
time, the number of power-loss resumes, the cable's library number and flag byte. The comparison is always against the stored card,
never against the previous repeat, so a slow drift becomes a new run once it reaches two steps. A run that differs in any compared
field is stored as a new run, as before (the stored card is replaced and the Changes list shows the difference). A firmware update
changes `fw`, so the first run after it is always a new run.

A repeat produces an empty diff (`report_card::delta` of the stored card against the merged one): the Results screen has no
Changes row for it. The card shows the repeat: the Cards page note reads `x3 boot 9` (count and boot of the newest repeat), the
detail footer `run 4 boot 4 x3 last boot 9`, the web page a "Same result" row and `same result xN` in the header,
`tools/k2_remote.py cards list|show` and `tools/charger_summary.py` the count with the last boot and uptime. The JSON has
`times`, `last_boots`, `last_uptime_s` right after `uptime_s`, only when `times` is above 1. Host tests: `test_report_card`,
`test_report_card_store` (`test_identical_run_is_counted_not_stored`, `test_different_run_is_stored_and_resets_the_counter`,
`test_noise_within_the_stored_resolution_is_a_repeat`, `test_v6_file_loads_unchanged_and_takes_a_repeat`), `test_report_card_json`,
`test_ui_cards`, `test_report_card_py.py`, `test_k2_remote_cards.py`, `test_cardpage.mjs`.

## Default order (company families, 2026-11-06)

The default order is the order of the wizard's company families (the Custom list and the Results sections use the same
names). Before 2026-11-06 it was legacy-first (FCP, SCP, AFC, QC2, QC3, QC3+, VOOC, SVOOC, SVOOC65, UFCS, VIVO, then Pd,
then PdLow); the unit and wire IDs did not change, only the order.

1. **Vendor fast modes by company**, all Legacy (PD-silent) units sharing one attach: Huawei / Honor (FCP, SCP),
   Samsung (AFC), OPPO / OnePlus / vivo (VOOC, SVOOC, SVOOC65, VIVO). The probe order inside each
   family is fixed.
2. **USB PD**: fixed, EPR, AVS, PPS safe rows and the revision rows. The Xiaomi / Redmi Mi 6A probe rows run at the end
   of this unit (they need the PD session) and are presented under the Xiaomi family; they have no algorithm of their
   own and follow the PD selection. A probe is only sent against a list that advertises its 6 A (2026-11-07): a source that
   listed SVID 0x2717 gets the chain and the probe waits for the re-announced 6 A list; a source that never listed it and
   advertises no 6 A drops the rows from the plan at run time (nothing on the card or in the counts); one that listed it
   but stays at its own limit gets the skip below with no Request sent ("not unlocked", detail 155 to 158 as far as
   known, plain when the chain ran and the cable fits). A probe the source refused or never answered is a neutral skip, "not unlocked",
   with the missing link as its detail: no OEM chain (the source never listed SVID 0x2717), chain unfinished (listed,
   but the auth did not finish within 2 s), or 3 A cable (chain done, cable not proven above 3 A; a far-end K2 cannot
   read the cable). A probe the source answers with a Hard Reset (Xiaomi 20K) is the same neutral skip, sent once and
   not retried; the cable and chain reasons above stay, otherwise it reads "source reset". When the K2 itself presents a spoofed identity and nothing physical was read, that identity's cable
   VDO limits are the cable limit used: a 3 A identity is a 3 A cable, a 5 A identity makes the cause "vendor not
   accepted" (chain done, 5 A cable presented, 6 A still not offered). Only a refusal with a finished chain and a read
   5 A cable stays a FAIL. The run summary words a probe row like every other row ("Mi 20V 6A").
   **Full-current PPS row**: the PPS rows of an APDO request 1 A. The last row of each PPS APDO, after its Mi probe rows,
   requests the APDO's top voltage at its whole advertised current (11 V 5 A on a 5-11 V 5 A APDO, 20 V 2 A on a 5-20 V
   2 A one), clamped to the cable limit the planner uses for every row (`cable_ma`: 3 A when the cable was not read, 5 A
   read or EPR-proven; a spoofed identity does not change it). The row is an ordinary PPS row: same pass band, not
   continuous, not a probe. It is left out when it would repeat the 1 A row (an APDO of 1 A or less). Its label states
   the requested current ("PPS 11V 3A" on an unread cable, "11V 5A ✓" in the summary's PPS group); the other PPS rows
   keep showing the advertised one. A failing full row marks the PD group failed on the report card (it is not a
   continuous row, so the PPS range entries are unaffected). Host tools tell it from a probe by the request: a plan row
   asks 5 A at most, a probe 6 A (`tools/step_label.py` `HUNT_MA`); no wire field changed.
3. **Legacy / standard D+/D-**: QC2, QC3, QC3+ (with their Class B 20 V rows) and UFCS, a second PD-silent attach.
4. **Risky units last**: PPS/AVS rows below 5 V (`latch-off-low-pps` / cut-and-recover), after their own detach and Pd
   attach (the `PdLow` unit; it never runs before `Pd`).

Sessions: Legacy (1), Pd (2), Legacy (3), PdLow (4); a session boundary is only a mode change, so a custom order that
puts PD first, or drops a family, plans fewer or more attaches. The plan still needs the source caps before the PD
unit: they are taken from caps heard (unacknowledged) during the first Legacy attach, or from the first Pd attach.
Without caps at START the PD rows are planned late: one placeholder row stands in for the PD unit (and the Xiaomi probes) while
the vendor units run, and `Runner::replan_pd` rewrites that tail once the caps are heard (the journal records a `replan`).

QC Class B (the 20 V rows of QC2 / QC3) is part of the default selection and of `All tests` / `Legacy only`. A Class A
charger (12 V max) that does not reach 20 V gets a neutral `Class A only (12 V max)` skip, not a failure; the K2's own
voltage-ceiling check stays in front of the 20 V request.

## Refused Mi 6 A rows: hunt evidence and cause

A Mi 6 A row that stays a FAIL (the source finished the Xiaomi chain, the cable is known to be 5 A or more, and it still answers
Reject / Wait or nothing to the 6 A Request) is the one case that needs hardware to explain. The Runner keeps a compact snapshot
of what the sink saw (`charger_test::HuntEvidence`, a side table of four slots; diagnostics only: nothing reads it back and no
verdict depends on it) and `classify_hunt()` names the cause where the evidence decides it. The snapshot is taken for every
refused hunt row, the FAIL and the "not unlocked" skips alike (a skip's cause is the missing link), from the sink's Xiaomi trace (`pd::XiaomiTrace`) and the Request that row sent. It does not survive a reboot
resume (the journal does not hold it).

Evidence fields: the chain flags (SVID 0x2717 listed, opener seen, authed = 0x2717020A seen, re-announced = Source_Capabilities
after the auth, still inside the chain when the Request left, Request held by the chain guard, sent, cable read / inferred,
timeout), the last 0x2717020x word the source sent, the number of PDOs in force at the Request, the PDO asked (position, type,
the word and the current it advertised at the asked voltage), the Request word as sent, the cable limit the run used, the time
from the auth to the refusal and from the Request to the source's answer (Accept / Reject / Wait / none).

Causes, in the order `classify_hunt()` tests them (each is only reached when every one above it is ruled out):

| Cause (wire word) | What it means | Evidence that decides it |
|---|---|---|
| `no-chain` | The source never listed SVID 0x2717: nothing to unlock. | svid flag clear. |
| `inside-chain` | Our Request left while the chain was still running; that kills the chain. | sent and in-chain flags. |
| `request-held` | The chain guard still held our Request when the row ended: it never went out. | held set, sent clear. |
| `chain-stalled` | Listed, but the auth never reached 0x2717020A. | authed clear; `last_step` says where it stopped. |
| `unexplained` | Authed, but no Request of this row was found (evidence incomplete). | sent clear after the above. |
| `fixed-pdo` | The Request referred to a fixed PDO; a fixed PDO never offers 6 A. | PDO type fixed. |
| `refused-offer` | The APDO in force offered the asked current and the source still refused. The real FAIL to chase. | advertised current at or above the asked current. |
| `apdo-stale` | Authed, but no Source_Capabilities after the auth: the APDO in force was still the pre-auth 5 A. | re-announced clear. |
| `capped-5a` | Re-announced, the APDO is still 5 A and the cable is 5 A: the source capped it. | re-announced set, cable at or above 5 A. |
| `cable-cap` | The cable limit the run used (unread or 3 A) is below 5 A. | cable under 5 A. The row is the "not unlocked (3 A cable)" skip; it only reaches a FAIL here if the limit changed during the row. |

Where it shows: the Steps detail page of the row (lines `Why:`, `Chain svid:... open:... auth:... re:...`, `Caps N APDO mA cable
mA`, `Ask #pos V mA <answer> ms`; with a transition-capture thumbnail only `Why` and `Chain` fit), the `[HUNT]` log line, the
Black Box (mark 8 `hunt-refusal`, and a frozen capture, trigger `6 A refused`), `k2_remote charger-test results` (a `hunt:` line
under the row and a `hunt` object in the JSON) and the run summary (`Mi 20V 6A ✗ rejected [cause: why]`). Remote command 0x0E
(`ChargerHunt`, read only) carries the snapshot; the reading steps are in `docs/HW_TEST_PROCEDURES.md` (T20).

## Wizard steps and the cable identity

The setup wizard has three steps, shown as `1/3`, `2/3`, `3/3` in the kit top bar (title plus the three-segment progress
rule). It advances on a pick: there is no `Next` row, every cursor is a 0-based row, and BACK returns with the cursor on
the pick that was made.

1. **Tests** (stage 0). Four presets (`All tests`, `PD only`, `Legacy only`, `Vendor modes`), then `Custom...`, then
   `Last results` once a run exists (six rows). The solid square (hollow when not picked) marks the preset matching the mask (on `Custom...` when the
   mask matches no preset). OK on a preset selects it and goes to Cable; each preset carries a
   note on the focused row: `All tests` = "Everything, QC Class B 20V too", `PD only` = "PD fixed, PPS, Mi 6A (Xiaomi)", `Legacy only` =
   "D+/D-: vendor modes + QC/UFCS" (all the D+/D- families), `Vendor modes` = "Huawei, Samsung, OPPO, vivo"). Steps are
   named by one label, `<protocol> <V> <A>` (`PPS 20V 5A`,
   `PPS 11→8V`, `QC3 12V`, `Mi 20V 6A`), the same on rows, details, chips, summary and report card text. `Custom...` opens the per-family list (stage 14,
   "Custom tests": the six families in run order with `ON` / `OFF` / `PART` counts: `Huawei / Honor` (FCP, SCP),
   `Samsung` (AFC), `OPPO / OnePlus / vivo` (VOOC, SVOOC, SVOOC65, VIVO), `Xiaomi / Redmi` (Mi 6A; it mirrors `USB PD`, the
   probes run inside the PD unit, so the row is not toggleable), `USB PD`, `QC / UFCS` (QC2, QC3, QC3+, Class B, UFCS);
   the "select / clear all" row is gone); a short OK on a family toggles the whole family, a long OK
   opens its Group page (stage 1: `PD fixed`, `PPS`, or one row per algorithm; a short OK toggles a row there).
   The result rows follow the same family order. The rows (Running grid, Results, Steps filter) come from the step list
   alone (`result_layout::build`): the runner walks that list by index, so the list is the execution order, and a row
   stands where the plan first runs one of its steps. No copy of the order exists: QC2 is followed by QC2 Class B, QC3
   by QC3 Class B, a custom unit order (`charger-units`) moves the rows with it, and each Mi 6A probe level is a row of
   its own ("Mi 6A", the 6 A hunt steps at that voltage) at the place the planner runs it, after the PPS row it rides on
   (a re-plan that regrows the rows moves it with the plan). The text layouts (report card, `charger_summary.py`) separate them with
   short section headers (`Huawei`, `Samsung`, `OPPO`, `Xiaomi`, `USB PD`, `QC/UFCS`, plus `OEM / PD` for the
   vendor-label facts of a PD source; `Row.label` holds 8 characters); a section whose rows all fold into the `No reply` /
   `Skipped` summary rows drops its header. The on-device grid is units only (no header rows).
2. **Spoof cable** (stage 2). The shared cable-identity list (below); OK on a row picks it and goes to Review.
3. **Review** (stage 4). One START row, then the summary rows `Tests`, `Peak` (the caps PDO with the highest wattage),
   `Cap`, `Header`, `Spoof` (the short identity name plus its rating, plain `Off` when the pick is Off — a START with
   Off disarms whatever is armed) and one free row for status lines such as a self-powered charger. The first OK on
   START opens the confirm window, the second starts.

Back is always `Back` and retreats one level: Group -> Custom -> Tests, Cable -> Tests (cursor on the preset in force),
Review -> Cable (cursor on the identity in force), Results -> Tests. Running (5), Results (6), Steps (7), Detail (8),
Resume (9), Changes (10), Refuse (12) and Waiting (13) keep their numbers; their top bar carries no step count. They
are on the kit too: the Running stage is a hero readout (the step in flight, VBUS and target) over a pass-band gauge, the
Steps and Changes lists are kit rows with a verdict marker, Detail is a fixed six-row table, Waiting a hero readout with
a timeout bar.

The cable identity is the one list shared with Tools -> Spoof (`CableIdentityList`, `include/k2/ui/ui_cable_list.hpp`, held in
`UiContext::cables`): `Off (real cable)` (no rating column; the note under the focused row says "Nothing answers for the cable"), the three built-in chips (ids 1, 2, 4; id 3 is a retired alias of 4), then the saved clones, with the same names, the same
order, one selection and one armed marker. A row is a short name (`EPR`, `CUKTECH`, `OnePlus`) with its rating in a second
column (`50V 5A`, `6A`, `100W`); a saved clone shows its library name in data colour with a dim "saved" tail, so it reads
apart from the built-ins. The solid square marks the pick (there is no "USE" word). OK on a row of the Spoof cable step selects it
and goes to Review; nothing is armed while browsing. The tail pill depends on the screen the row is built for
(`CableIdentityList::fill_spec` builds the row for both): in Tools -> Spoof the identity answering right now wears the
amber "ARMED" pill; on the Cable step the pick wears "NEXT" and the identity answering now "CURR" (dim amber) while they
differ, and the shared row keeps "ARMED" when pick and armed are the same (with nothing armed the pick is the radio
alone). Review shows the choice as the "Spoof" row.
The selection is remembered across power cycles (`/cable.cfg`, `include/k2/chargertest/cablepref.hpp`): it is written only when it
changes, read at Ui init before the boot arming, so "pick a cable, then PLUS-boot" arms it on a self-powered boot (a
saved clone that is gone, or a damaged record, reads as Off; a HID-powered boot restores it but does not arm).
Arming stays explicit: the wizard arms at START, Tools -> Spoof arms on OK (OK on the armed row disarms; OK on Off
selects "no identity" and disarms). The Ra mode is the charger test's business and is never switched on from Tools.

START is two OK presses on Review, in this order (reordered 2026-11-06: the confirmation first, the Rd last):

1. **First OK**: opens the confirm window (`open_confirmation`, `confirm_live`; footer "No load! OK twice within 5 s"
   becomes "OK again within 5 seconds"). Nothing is touched: no Rd, no identity armed, no Ra guard, no e-marker scan.
   A preview refusal shows on the footer at once and opens no window, except "no source" / "not safe 5 V" while no sink
   is inline and VBUS is below 4.5 V: the Rd is what brings the source up, so the final check is left to the end of
   the wait. Back, a changed proposal or 5 s without the second OK close the window; nothing needs undoing.
2. **Second OK** (`confirmed_start`): the Ra guard (`guard_topology` / `raguard::spoof_allowed`; a cable or no charger
   refuses with the Refuse stage, still before anything is armed: "Start, skip spoof" goes on without asking again (this run without the identity; the stored pick stays), "Start anyway" arms the identity and records the reading, Back
   returns to Review), then the identity is armed — whatever was armed is disarmed first and the pick takes over (Off
   arms nothing, so a START with Off disarms a Tools -> Spoof arming too), then the
   **last stage**: the Rd is presented and the Waiting stage (13, "Waiting for charger") follows the VBUS and the source's
   capabilities (`start_begin`, `wait_step`). A charger that is already up (or a self-powered or bus-powered K2) passes
   through it at once.
3. **The run starts by itself** when the wait ends (`launch`: the final preview on the fresh reading, then `begin_run`),
   with no further press. OK or Back in Waiting cancels it (Rd off, nothing started, back on Review). A wait that times
   out, or a final check that refuses, ends on Review with the reason on the footer and a passive K2.
   **The + key in Waiting.** On this board the physical + key is wired to the CC pins: holding it for a few seconds wakes a
   charger that sleeps with no sink on the line. Waiting therefore swallows + and - in every press length (no paging, so a
   long + no longer cancels the wait; `dispatch()` takes a paging lock while the stage is Waiting; BACK, OK and the BACK hold
   work as before), and with no VBUS the footer says how to wake it ("No charger. Hold + few secs" with nothing on either
   CC, "No VBUS. Hold + few secs" with an Rp on a line). While the physical key is down, and for 2 s after, the session logs
   `[WAKE] + pressed: CC1 .. CC2 .. VBUS ..`, then `[WAKE] t=<ms since press> key=<0|1> CC1 .. CC2 .. VBUS ..` every
   ~100 ms, then `[WAKE] + released after <ms>` (mV, `%d` numbers only), so `k2_stream view --kind log` shows what the hold did
   to the lines; the CC cells show the raw mV instead of the Rp word while it is held. Pure parts: `k2/ui/wait_plus.hpp`.

The resume path never asks for a confirmation. There is no "Ask" chooser anywhere. Stage numbers 3 (Identity) and 11
(Ask) are retired and never entered; they stay reserved so the remote `stage` values do not move (the Custom list is
the new stage 14; remote and web clients show it as "custom"). Remote `charger-cable` (`ctcable::Choice`) sets the same
choice; the resume path re-arms the journaled identity. `tools/ct_start.py` picks "All tests" on Tests (OK advances to
Spoof cable), presses OK on Spoof cable (advances to Review), then OK on "review" (twice), never in "waiting", answers "refuse" with
Back, and treats Review after a "waiting" as the K2's refusal. `tools/ui_walk.py` expects a cursor of "whatever the
device had" after a pick (`None`), since Back lands on the pick in force.

The Refuse stage (12) explains the guard's refusal — the band reads "Cable connected" and the lines below "the spoof won't reach the charger." — and offers `Start anyway` and `Start, skip spoof` (BACK returns to Review; `Start, skip spoof` runs without the identity and leaves the stored pick in `/cable.cfg` unchanged).
It has one reason: the topology guard (a cable on the plug). There used to be a second one, a late "Self-powered" warning for a
START with an identity on a self-powered or bus-powered K2; it is gone. Such a K2 boots from the charger's VBUS, so the Ra
can only follow the charger's attach, and a charger that reads the cable at attach (`cable-look-at-attach` in
`CHARGER_QUIRKS.md`) has already decided without it, but the START now just goes on (nothing to answer). When it can pay, the
run begins with a **planned re-attach**: a self-powered START (pinned Rd on CC2, no host) with a spoof cable that claims
5 A or more, against a source that lists SVID 0x2717 (`rdintent::plan_reattach`). The confirmation footer then reads
"Press again, K2 blinks once"; no other message, no extra stage. After the journal's header and meta record (arm step 3) the
page writes the intent marker (`rdcomeback::begin_reattach`, written and read back; if that fails the blink is skipped and the
run goes on), drops the Rd for 30 ms with the Ra kept (`fusb302::rd_glitch_begin/end`) and holds the Runner in its Baseline
(nothing is judged). The K2 normally dies in the VBUS dark and the comeback boot resumes the journaled run with the same
identity armed; a K2 that survives (a host on the other port) goes on at the new contract, or after 5 s without one. BACK,
a stop or the end of the run give the Rd and the marker back at once (`reattach_drop`). The run-time marker
(`rdcomeback::set_run_cable`) also brings a K2 back pinned after any mid-run VBUS loss, planned or not.
`Start anyway` arms the identity against the guarded reading. The run records it: the
journal's meta byte 19 (`RunMeta.guard`: 0 = not judged, else topology + 1, bit 7 = started anyway), the report card's
`Cable.flags` (bits 3..5 = guard + 1, bit 6 = started anyway) and, in the card JSON, `"guard":"<unknown|none|cable|direct>"`
plus `"guard_bypassed":1` (both only when set; `tools/report_card.py` decodes the same bits). The log line is
`[GUARD] started anyway against a <reading> reading`.

## Cable identity per unit (Ra and spoof)

The Ra mode and the e-marker spoof belong to the Pd-mode units (Pd, PdLow) only. With an identity selected, the runner
asks the page for it while detached, right before `attach(mode)` with `mode != Legacy` (`Env::identity(bool)`):

| Boundary | Ra / spoof |
|---|---|
| Legacy unit (QC2 ... VIVO), HID-powered | off: Ra released (`fusb302::ra_run_own()` hands the mode back), forced scan line freed, spoof disarmed; the attach runs with plain Rd, as in a run with the spoof skipped |
| Pd or PdLow unit, HID-powered | on at its attach: Ra, then the spoof armed again (`arm_cable(.., in_run=true)`, the run's own busy state must not refuse it) |
| `end_legacy` closing Pd attach | no call: the run is over |
| Self-powered or bus-powered K2 | no call, ever: the pinned Rd cannot be opened between units, so the Ra armed at boot stays for the whole run |

Why: keep the identity scoped to the units it serves (PD). Note: the Baseus 70W evidence first read as `ra-disables-legacy` was really `legacy-off-on-direct-plug` (docs/CHARGER_QUIRKS.md);
the Baseus 70W run with the EPR identity for the whole run went from 27 pass to 1 pass. The card header and the journal keep
the chosen identity (`run_cable_`) for the whole run; only the electrical state follows the unit.

Limitation (decided, not a bug): a self-powered or bus-powered run keeps Ra for every unit, so D+/D- results on a source
with `ra-disables-legacy` are not valid there. Run those units with the spoof off, or from a HID-powered K2.

## Re-announced source list

A source may announce its capabilities again after the cable discovery (SOP' query), with different currents or extra
APDOs (pattern `reannounce-after-cable-discovery`). `Runner::follow_list` runs in `poll_impl` before the "offers changed"
check, in Pd mode only, when the caps signature changed at the same spec revision, outside the K2's own EPR entry
(`epr_stage_ == 2`) and with no partial EPR chunk. The new list is re-planned PER ROW (pattern `budget-reannounce-mid-run`:
a shared-port splitter re-splitting its budget mid-run drops one offer while the rest stay or grow):

- No PD row has started and the plan is not frozen: the PD unit is planned again (`replan_pd`). Extra APDOs get rows and the
  rows carry the new currents (the summary shows PPS 5 A).
- Otherwise: a Pending or Running row the new list still offers (`find_offer`: the same slot, or a slot of the same kind,
  SPR 1-7 or EPR 8+, carrying the planned PDO or one that covers it, extra APDOs allowed) takes the new slot and PDO word.
  What "covers" means depends on the row: a FIXED row is kept while its voltage is still offered, at ANY current (15 V 3 A
  becoming 15 V 1.5 A keeps the row, which then runs at the lower current word and the card notes the reduction; only a
  voltage that is gone withdraws it); a PPS / AVS row needs the same type, its target voltage inside the new range and a
  limit of at least its own current. The row in flight is looked up the same way anywhere on its side of the SPR / EPR
  boundary: an offer that only moved slot re-queues it (restarted at the new slot and current word, not a try), only an offer
  that is gone ends it as `Withdrawn` and releases to 5 V (the sink's single-Request gate serialises the release).
- A row the new list no longer offers is skipped with `Reason::Withdrawn` ("withdrawn by source") — not `CapsChanged` — and
  the unit goes on against the new list. A withdrawn PDO that comes back revives its rows (Pending again) while the unit has
  not ended. Each re-plan bumps `revision_`, calls `plan_changed()` once, logs `[TEST] re-plan: <n> rows follow, <m>
  withdrawn, <k> back` and counts into `Evidence::rebalanced` (noted "SOURCE rebalanced Nx" on the results card). A
  re-announce that changes nothing for the plan (a word-only current change, an identical list) is not counted and not
  logged.

A complete `EPR_Source_Capabilities` outside the K2's own EPR entry/exit (`epr_stage_ == 2`) is a re-announce like any
other; only a partial chunk stays excluded.

Restart rules for a mid-run re-announce (user decision 2026-11-09):

- Settle first: after any new list, no Request is sent until the list has been unchanged for `kListSettleMs` (1500 ms);
  each list that arrives inside the window restarts it. The sink's single-Request gate still applies.
- The row in flight is restarted at its new slot and current word once the list has settled (the restart is not a try:
  `tries` counts only source failures). Not offered: `Withdrawn`, then a release to 5 V through the sink gate.
- Before the PD unit closes, the Withdrawn rows are re-checked once against the list in hand (after a settle) and the ones
  that came back run (the end-of-unit sweep in `next()`).
- Flapping is not a failure: no counter gives up; keep settling and re-planning.

Limits: a list that places no PD row at all (pending or in flight), a `spec_rev` change, and no list back within
`kCapsReturnMs` are a genuinely different list and stay `CapsChanged` ("offers changed": the unit is skipped, the run
continues) — never a re-split while attached. The excluded cases (VBUS dark, the `preauth_tail` window, EPR entry/exit,
the hunt-Reject answer) keep their own handling. The cause of the Baseus re-announce (the SOP' query itself, or the Ra) is
not provable from the logs; the planner follows the list either way.

## PD row judging (review pass 2026-11-11, host-tested, hardware check pending)

How a PD row is judged and how long it waits. All of it is host-tested; none of it has run on hardware yet.

- **Band.** A fixed PD row above 5 V is judged on the 250 ms running mean (`VbusMean`, `kBandTauMs`); one sample beyond 15 %
  of the level fails at once. A programmable (PPS / SPR AVS / EPR AVS) row asked to RISE gets the same tight band as a fixed
  level (+-5 %, at least 100 mV): the source drives the output up itself, so a landing 8 % high is out of regulation. Only a
  DOWN-step keeps the wide unloaded ceiling (target + max(10 %, 600 mV)), because an unloaded source cannot sink its output.
  `Runner::from_mv_` (VBUS when the level was asked) tells the two apart; an unknown start keeps the wide ceiling.
- **No PS_RDY on a down-step.** A source that Accepts a lower PPS / AVS level and never sends PS_RDY (pattern
  `no-psrdy-on-downstep`) is taken as ready only after VBUS stayed steady inside [target - max(10 %, 600 mV), ceiling]
  (within `kRiseMv` of the first in-band sample) for `kDownSettleMs` (300 ms). One glitch sample, an output gone dark or a
  fall through the band no longer counts. VBUS that never gets there still times out at 4 s.
- **Hold guard descent window.** The sink's hold guard lets a falling VBUS finish its descent (stall rule: at least 50 mV of
  progress per 600 ms; rise rule: more than 150 mV up aborts). The window after the settle time scales with a PPS / AVS
  down-step: 2 s per volt of drop (a 0.5 V/s fall, the ~2 V/s drain of an unloaded source passes), at most 10 s, so a 20 V to
  5 V step holds; fixed targets and small steps keep 3 s (`descent_window_ms`).
- **Step walk and deep sweep record a mean.** Each walk level and sweep point is the mean of the VBUS samples of the last
  `kWalkSteadyWindowMs` (`WalkSteady::mean`), not the one sample that closes the wait: a burst-mode swing of tens of mV no
  longer reads as "step resolution" between neighbouring levels or as sweep error.
- **A capped row ends on its plateau.** A PPS / AVS row whose output stands steady below the band with the contract live
  (ready, not a sweep row, plateau held `kCappedPlateauMs` = 5 s, `capped()` true) ends `Capped` there instead of at the 12 s
  row timeout; the timeout path keeps the old rule.
- **PS_RDY timing is noted, never judged.** The sink stamps Accept and PS_RDY; per row the runner latches three flags in
  `Evidence::timing`: bit 0 PS_RDY while VBUS was outside the row's band (`early-ps-rdy`), bit 1 PS_RDY inferred by the sink
  (the no-PS_RDY down-step shortcut), bit 2 PS_RDY later than the spec maximum (550 ms SPR, 1020 ms EPR; the 1500 / 4000 ms
  tolerances of the engine stay). `Evidence::ps_rdy_max` holds the worst Accept-to-PS_RDY time in 20 ms units. One log line
  `[CT] ps_rdy row=<n> flags=<bits> ms=<ms>` per flagged row; the remote status carries both bytes in the schema-2 header
  (`evidence.ps_rdy` in `k2_remote.py --json`, a "PS_RDY quirks" line in its text view). No verdict reads them, the report card
  is unchanged, and a resume loses them (`Evidence` timing is not journaled). A card note would need the flags and the worst ms
  in the card record plus a note slot.
- **Resume.** The EPR proof and the late PD plan (a list first heard after START) are rebuilt from the journal on a resume, so
  a run that reboots between the cable read and the EPR rows plans and runs as the uninterrupted one would.
- **6 A hunt rows and the preview.** The hunt rows are planned for every source but only send a Request when the list in hand
  advertises 6 A (`hunt_advertised`); otherwise the row is dropped at request time, or ends as a "not unlocked" skip when the
  Xiaomi chain was seen. `Runner::preview` counts the same: an unadvertised hunt row is no row and no estimated time without the
  chain, a skip with no time with it.

## Results grid

The Results stage (stage 6) is the kit `ResultGrid` under a counters band (y 22..46: `N pass` in kOk, `N fail` in
kError, `N skip` in kDim, each led by its verdict shape; the word drops when the count does not fit 7 cells) and a one-line
summary strip (y 47..70, the best passing mode per family, or "No passing mode"). The grid starts at y 72: a 60 px
label strip with the unit name and up to five verdict cells per row (flex-filled, 1 px gaps). A row whose cells carry a
current is 40 px (cells 24 px plus one bracket per run of equal current), a volt-only row 26 px, and a family's first row
opens with a 2 px section rule. A unit that never ran spells the reason as its one cell. The focused unit lights its label
strip. The grid scrolls by pixel height (`list_state()` reports the grid's own first/visible values) and a last
pseudo-row, `Changes` (`N changed`), follows the units when the run saw the source change its offers.
`result_layout` turns the run's steps into rows and tokens (`build`; the on-device grid has no header rows, a skip tag
glued to a token is not drawn), `wrap` fits them to the cell area (`Metrics{adv, pad, gap, avail, mark, max_groups, cells}`:
the grid sets `cells` (equal-width cells, only the head's volt text counts) and `max_groups = 5` (163 px at 8 px advance);
the text layouts keep the defaults) and `summarize` picks, per protocol family, the passing mode
with the most watts (volts and amps from the step: the PDO's advertised current for PD, the requested one on a full-current
PPS row, the probed current for a Mi hunt).
Legacy D+/D- steps carry volts only. The footer follows the focus (`describe_unit`: pass / fail / skip counts of that
unit); OK opens its steps. The PD source revision no longer has a note line on this screen: Review's `Header` row keeps it.
Perf (host, `test_ui_perf`): first paint about 125 000 px (budget 138 000; the whole body once is about 45 000, then the
hero strips, counters, rules and the 26 / 40 px rows, each filled and drawn), a selection move inside the window about
32 000 px (budget 35 500: two rows plus the footer), a move that scrolls the window about 65 000 px (budget 72 000: the
grid has no blit). Between steps the worst tick (a unit boundary that shifts the grid window) is about 59 000 px (budget
64 500).

### Running stage in flight (paint budget, grid follow)

While a step is in flight (`Runner::quiet()`: Requesting or Holding) the page is capped per tick instead of silenced
(the old rule, commit 9cc1696, froze the screen for whole rows):

- `kFlightPx` = 6 000 px (about 13 ms at 2.13 us/px) shared by hero + needle (about 4 Hz), the clock, the progress bar, the
  step texts and the footer (when they change) and the gauge strip (once per target). The step dot (below) is charged to
  the same budget but never gated by it.
- `kFlightRowPx` = 224 x 42 + `kStrikeRowMaxPx` + `kDotRowMaxPx` px (about 20 ms): one grid row, alone, on a tick where
  nothing else painted.
  `ResultGrid::draw_some` paints the running row first and defers the rest (`pending()`).
- `lcd.cpp` services USB, the FUSB302 and the UFCS handshake every 2 048 px inside a stream, so none of this adds a
  blind spot above about 4.4 ms. The idle hook does not serve the engine windows (HS pulses, VOOC slices, UFCS / VIVO
  frames): `Runner::paint_blocked(fctrig::paint_critical())` is true there and the tick paints nothing.
- The verdict grid follows the step, anchored to the bottom (`ResultGrid::anchor_bottom`): the window's bottom edge sits
  on the row of the step that started, the finished rows stay above it, and scrolling counts pixels because the rows
  differ in height. Early in a run, with too few finished rows to fill the window, the window stays at the top (no empty
  space above). The result is within a tick or two, a row at a time. PLUS / MINUS scroll it by hand; the follow resumes
  5 s after the last press or at the next step. Caveat: while the window is scrolled (first row > 0) the slack under the
  running row is less than the height of the row above it, so a smaller row after it can still be fully visible.
- The cell under test keeps its Pending look and carries a solid `kFocus` dot, 5 x 5 px (`kRunDotPx` = 25 px), against
  the top-right corner of the cell, clear of the label ink. The dot is steady: it does not blink, so nothing repaints
  while a step runs. It is a shape on a plain cell, so it tells the cell from Pass, Fail, Partial, Skip (strike) and None
  without relying on colour alone.
- The gauge band and the dot are one pair, painted in the same tick when the step changes (the gauge strip is item 1 of
  `paint_running`, the layout rebuild and `ResultGrid::sync_dots` item 1b, before the hero, the texts and the grid rows).
  `sync_dots` erases the old dot and paints the new one with no budget gate (25 px each, or a cell's worth when the dot
  would touch the label); the row scroll and the row repaints that follow may lag by a tick or two. When the new cell is
  not on screen yet the dot paints as soon as its row does. The chip of the step in flight is found by its level
  (`Token::mv`), because a legacy chip merges the up and down steps of one level and stays pending until the last of them.
- Each step's end line carries the longest main-loop gap and the longest UI-tick gap during the step
  (`[CT] gap row=N loop=Xms ui=Yms`, `include/k2/core/gap_meter.hpp`).

## Legacy entry when the source never gives up PD

If a Legacy attach is still seeing caps or Hard Resets after the give-up limit, retry once with a longer wait; if it
still won't fall back, the unit's rows are `Skip` with reason "blocked by PD" (new reason).

## Self-powered and bus-powered K2

A K2 booted with PLUS or OK held pins one Rd and takes its power from the charger under test
(`fusb302::self_powered()`); a K2 that finds itself running from VBUS on the measured port is bus-powered and treated the
same (below). Every unit boundary above ("detach, wait dark, attach") would remove VBUS and the K2 with
it. The runner latches the mode at `start()` / `resume()` (`selfpwr_`; the live flag turns false after a detach) and
uses these boundaries instead. Nothing in this table touches CC, the pinned Rd or the e-marker identity.

| Boundary | HID-powered K2 (unchanged) | Self-powered K2 |
|---|---|---|
| Start of a Pd unit | detach (Rd off, wait for VBUS dark), attach Pd | `hold(Pd)`: Soft_Reset, then one Get_Source_Cap and a Request for PDO 1 at 5 V; never a Hard Reset |
| Start of a Legacy unit (QC2, QC3, QC3+, AFC, FCP, SCP, VOOC, SVOOC, SVOOC65, UFCS, VIVO) | detach, attach Legacy (PD-silent) | `hold(Legacy)`: the pinned Rd stays, the engine goes PD-silent (Auto-CRC off), VBUS stays up |
| Legacy to Legacy | one silent attach shared (`continue_legacy`) | the same: no hold between them |
| End of the run or Cancel after a Legacy unit | `legacy_only(false)` plus a PD rescan, or attach Pd | `hold(Pd)` (Soft_Reset); no rescan |
| PdLow (PPS / AVS rows below 5 V) | runs after its own detach and Pd attach | skipped, reason "needs external power" |
| Legacy retry after "blocked by PD" | one retry after a detach | none: the unit is skipped "blocked by PD" (a retry needs a detach) |

**Held caps and the PD give-up.** A self-powered K2 takes a plain 5 V contract as soon as the source's caps arrive and stays in
it (the self-hold, see `docs/CHARGER_QUIRKS.md`, `caps-retry-window`), so at START the Runner plans its PD rows from the caps the sink already holds
instead of asking a quiet source again, and the `hold(Pd)` Soft_Reset at a unit boundary works from that live contract. If the
list is gone at START although PD was heard earlier in this attach (`Input::pd_seen_attach`, `fusb302::pd_seen_attach()`), the
PD rows are skipped with the reason "PD gave up before START" (`Reason::PdGaveUp`; the source stopped announcing and only a
detach brings it back) instead of "not advertised". Without PD ever seen the reason stays "not advertised".

A step that inherently removes VBUS is skipped with the distinct reason "needs external power" (`Reason::NeedsExternalPower`;
schema 1 of the remote protocol maps it to "disabled"). PdLow is the only such step today: a source may latch its output off
below 5 V (pattern `latch-off-low-pps`), and a K2 on that output would reboot without a way back. Run it from a K2 on a
separate supply.

**Bus-powered counts as self-powered.** A K2 that runs from the tested charger through the measured port without a key-mode
boot (a USB-A port with an A-to-C cable on the bottom port, HID unplugged, plain boot) gets the same run:
`power_source::bus_powered()` (`src/pd/power_source.cpp`) decides once, at 2 s of uptime or as soon as a USB host is
configured (then never): no host and VBUS present (>= 4 V) means bus-powered. The decision is latched for the session and
logged as `[PWR] supply: ...`. The page feeds `fusb302::self_powered() || power_source::bus_powered()` into
`Input::self_powered`, so the table above applies unchanged. HID-powered runs (a USB host is configured) are untouched.
Limit: a host-powered K2 with HID disabled and a charger on the measured port cannot be told from a bus-powered one.

Loss of VBUS anyway (a Hard Reset dip longer than a second, a charger that cuts the output): below 1000 mV on a self-powered
run the Runner suspends (`suspend()`): the run is not finished, the journal stays open with its phase marker, the rows
stay Pending, and the page waits for VBUS to be steady for 0.5 s and then resumes by itself (`auto_resume_`). The Detach /
Attach states never suspend (a HID-powered run reaches them with VBUS dark by design).

**A reboot always resumes.** A run interrupted by a reboot or a power loss resumes by itself in every power mode: the next
boot with a pending journal waits for VBUS to be steady for 0.5 s (a Resume screen shows the wait; if VBUS never comes
back it keeps waiting) and resumes with no click. Holding BACK at power-up still cancels the resume. The first Baseline
after a resume adopts the new PD session instead of counting it as a loss.

**An ended run never comes back.** The rule is "interrupted resumes, ended does not". A run ENDS on a verdict, a cancel, a fault
or a lost source on a K2 that cannot wait (not on a suspend). When the Runner ends it for good it calls `Env::closing()` BEFORE it
skips the pending rows, and the page writes the journal's Finish record there: a reset in the middle of the skip rows leaves a
closed run, not a stopped run with Pending rows. Replay treats a Finish as final whatever the rows say, so an older journal
that was marked stopped (an earlier firmware wrote its Finish after the skips, or lost it) does not resume either, and
`Journal::finish` is tried even after an earlier write failed. The boot logs `[CJRN] run N is closed, not resumed` for a run it
finds closed. The Resume screen's "stopped (source lost)" is only the default reason of a restored run, not a record that the run
ended. BACK on the Resume screen, BACK held at power-up and a run that cannot resume all close the journal too.

**Reboot budget per unit.** A unit (protocol) may cost at most 3 reboots or power losses (`kRebootBudget`, counted per
unit in the journal's `at.rec` entry, across resumes). Losses 1 and 2 resume the same row. On the 3rd the unit's remaining
rows are skipped with the reason "rebooted 3x" (`Reason::RebootLimit`) and the run continues with the next unit; it never
stops because of this. A suspend with the K2 still running (VBUS dipped and came back) is not charged to the budget, it is
bounded by `kMaxResumes` (16 per run).

**A cable identity cannot reach a charger that reads it at attach.** A self-powered K2 boots from the charger's VBUS, which
exists only after the charger decided on its attach; the Ra (PC13 high, CC1) can only follow. Measured on the CUKTECH 10
GaN Ultra 120 W (`runs/selfpwr-6a`): the SOP' Discover Identity comes about 213 ms after the attach and only when the Ra
was there first; a Ra raised 160 ms after the Rd, 12 s into a live contract or after a Soft_Reset re-announce gives no
cable look, and the charger offers the 3 A caps of an unmarked cable. An automatic warm reboot (the pin is kept, so there
is no fresh attach) and a guided replug holding PLUS (the same order) cannot change that, so neither is built. What does work
is a real Rd edge with the Ra already on: the planned re-attach above (the K2 dies in the dark and the comeback boot answers
the charger's fresh attach), which a self-powered START with a 5 A+ identity against a 0x2717 source does by itself. The Ra is
still armed beside the self-hold contract (`emkra::Ctx::self_hold`, best effort: it serves a charger that reads the cable
later) and the footer of a refused arming states the real reason (`emkra::refuse_footer`). Without the pattern (an
Apple-like charger) there is no blink; a HID-powered K2 still presents the Ra before the Rd.

The Review note reads "Self-powered: VBUS stays up"; the card records it (cable flags bit 2), the Cards page shows
"self-powered" in its stats line, the JSON cable object gains `"self_powered":1`, and the card summary header ends in
" · self-powered".

## The journal on flash (format 2: five section files)

The run record lives in `/jr` on the littlefs volume (`include/k2/chargertest/charger_journal.hpp`). A flash write starves the
main loop (a file over 256 B costs about 35 ms of erase plus 0.2 ms per half-word, a metadata compaction 130 to 250 ms), so the
rule is one file write per tick and none inside a live exchange. Hardware 2026-11-08 (Baseus FH22): two replan rows ran the
whole remaining replan job in one tick (about 1.2 s) because `phase()` drained the journal. Nothing drains any more.

**Files.** `h.rec` (the run header: format byte 2, then per section the row count u16 and the layout hash u32), five section
files `pd.sec`, `pps.sec`, `qc.sec`, `oem.sec`, `ext.sec` (a header of id, row count, layout hash and crc16, then 18 bytes per
row), and the small records `ph.rec` (phase marker), `rs.rec` (resume), `mt.rec` (meta: unit order, guard, cable), `at.rec`
(attempt counters), `fin.rec` (Finish). A section is at most about one block. Before format 2 a run was one file per row (up to
112 files plus the header); now it is at most five section files plus the header.

**Unit to section.** The section follows the row's algorithm, so a replan of one unit touches one file:

| Section | Rows |
|---|---|
| `pd` | PD fixed (2.0 and 3.x), EPR fixed and the EPR levels, the PD revision rows |
| `pps` | PPS, SPR AVS and EPR AVS rows |
| `qc` | QC2, QC3, QC3+ (and the Class B rows), AFC, FCP, SCP |
| `oem` | VOOC and SuperVOOC (both), vivo, and every 6 A hunt row (Xiaomi 6 A, the "Mi 6A" rows, the fixed 20 V hunt) |
| `ext` | UFCS and anything else |

Rows are addressed by (section, ordinal). Dropping or inserting a row in `oem` rewrites `oem` only; the late AVS rows of the FH22
list rewrite `pps` only; a verdict rewrites the one section of its row. `Journal::note` marks a row's section dirty, `note_plan`
marks the sections whose row count or layout hash changed. The page pumps one dirty file per tick (`hw::pump`), the header goes
last, and only when a section's count or hash changed.

**Replay.** The sections hold the rows section by section. The header (format 3) also carries the plan's order - one byte per
row, `order[k]` = where the plan's k-th row sits in the section-by-section array - written from the planner's step list
(`Runner::steps()`, the same structure the grid uses since 98e2f39) when the plan is journaled or re-planned. Replay applies it
verbatim, so a resumed run executes exactly in the order it was planned in; no order (where the 6 A hunt rows sit) is re-derived
from the rows. An order that is not a permutation of the rows is a bad header: the run is not resumable. The phase marker
(section, ordinal, group, algorithm, position, voltage) is checked against the row it names; a marker for a row the plan moved
from under it is void.

**Crash safety.** An arm drops the header first, sweeps stale files, writes the sections, and writes the header last, so a cut
anywhere in it leaves no run at all. A cut in the middle of the rewrite of one section leaves the other four valid (each has its
own crc and hash). Replay checks every section against the five hashes of the header, and the order against the rows. A
mismatch, a bad crc or a missing file is
`Problem::BadSection`, a broken order `Problem::BadHeader`: the run is NOT resumable (chosen policy; it never resumes with rows
it cannot vouch for), the boot logs
`[CJRN] run not resumable: section X does not match its header, journal dropped` and drops the header. The one window is between a section's rename and the header write that follows it (one file write): a
cut there leaves a section newer than the header, which reads as a mismatch and costs the resume of that run. An old journal
(format 2 without the order, format 1, or any other format byte) is dropped at boot with a log line (`Problem::OldFormat`) and
never read; its stale `s###.rec` files are swept at the next arm.

**No drain, a wait instead.** A request above 5 V that starts a contract names its row in the phase marker, and the marker must
point at a row that is on flash. If the plan changed (a replan, a late AVS list) and its files are not yet written
(`plan_pending()`), the Runner parks the request in the Baseline (the contract stays at 5 V, polled again) until the journal job
is done, and logs `[CJRN] wait-for-replan ms=N` when the request goes. Only a verdict-only change never waits. Chained
retargets inside a contract are not gated; their marker is covered by the file written before the contract. The page writes in
the gaps (the link quiet for 80 ms, or 400 ms of waiting, one file per tick). The post-Finish skip rows are not journaled (a
closed run is never resumed).

**Log lines.** `[CJRN] begin|replan ok rows= pumps= busy= worst= elapsed= used=` after each job (pump count, busiest pump in ms,
blocks used), `[CJRN] slow ...` for a single pump over 150 ms, `[CJRN] wait-for-replan ms=`, and at boot the discard / not
resumable / closed lines. A section pump costs about 100 to 150 ms in the worst realistic case (a 112-row section about 250 ms in
the synthetic worst case), against 1.2 s in one tick before.

## Iteration hooks

- `k2_remote charger-test` / `ct_start.py --units qc2,pps_low` (or similar): run an explicit unit list.
- Each unit is a small object with `mode()`, `plan(caps)`, `step()`: no unit knows about any other unit.
