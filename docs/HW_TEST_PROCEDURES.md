# Hardware test procedures

Step-by-step procedures to test every charging protocol and active feature of the K2 on real hardware.
The person who owns the device runs them: **the K2 is a shared device with one owner.** Nothing runs unattended, and
nothing here changes VBUS without someone watching.

Every command below exists in the CLI (`python3 tools/k2_remote.py <command> --help`,
`python3 tools/pd_trace.py <command> --help`) and every menu name is the one on the device (`docs/UI_DESIGN.md`,
section 5, and `src/ui/ui_pages.cpp`). Where the CLI or menu does not exist, the section says so.
Status per feature is kept in [`ROADMAP.md`](ROADMAP.md); this file holds the "how" and the results log
(section 3). Protocol background: [`FASTCHARGE_PROTOCOLS.md`](FASTCHARGE_PROTOCOLS.md).

Shorthand: `R` = `python3 tools/k2_remote.py`, `T` = `python3 tools/pd_trace.py`.

## 0. Safety rules (read first)

1. **Known charger, nothing valuable on the output.** Every test marked **HV** can put up to 20 V on the
   top port (the Fast-charge scan goes to 9 V; PD AVS/EPR can go higher). Use a charger you know, and no phone,
   laptop or battery on the other port. A dummy load, or the K2 alone, is the sink.
2. **The INA226 caps the input at 36 V.** This K2 has an INA226 (`[PWR] chip=INA226`), which is rated
   36 V on VBUS. The firmware refuses to request more than the monitor can measure (`pd::request_cap_mv`:
   36 V for INA226, 48 V for INA228/INA237, whose VBUS rating is 85 V); that nominal ceiling bounds what may be
   REQUESTED, while the overvoltage abort sits 5 % above it (`pd::overcap_abort_mv`: 37.8 V on an INA226, 50.4 V on
   an INA228/237), so a source that lands a few hundred mV over its top level is not a fault. Do not feed a charger above 36 V into
   this K2 by any other route. The 36 V / 48 V EPR steps (T04) are therefore **not testable on this unit**.
3. **Release to 5 V after each step.** Never leave the output above 5 V between tests:
   `R pd release` for PD, `R fc reset` for D+/D- protocols, or unplug the charger. Check with `R state`
   that `vbus_mv` is about 5000 before touching the cabling.
4. **Triggers are refused with a device inline.** PD trigger shows `LOAD: Load attached: unplug it`
   (the tool is blocked); staged on-device FC refuses with `Load/PD active: refused`, with no bypass.
   Identify retains its explicit second-OK override. Remote FC retains explicit `--allow-inline`;
   do not use it unless the test says so: overriding means the load sees the raised voltage.
5. **A dummy load or the K2 alone is the sink.** The K2 is the sink on the port the charger is plugged into;
   leave the other port empty unless the test needs a load or a cable.
6. **Port naming** (screen upright, `UI_DESIGN.md` section 1): the **top** edge has a **male** USB-C plug,
   the **bottom** edge a **female** receptacle; both are in the power path. The **left** female USB-C is the PC
   link (HID, drive, network), not in the power path. Power flows bottom to top when the charger is on the bottom.
   The default charger position in this file is the **bottom port**, nothing on the top.
7. **Voltage rules enforced by firmware** (what to expect when you push): requests above 5 V need confirmation
   (`--confirm`, or OK twice); the PD sink guard (`Sink::on_vbus` in `src/pd/pd_sink.cpp`) aborts back to 5 V when measured VBUS stays above target + 10 % or above the monitor ceiling + 5 % (37.8 V on the 36 V INA226). After a step-down it tolerates VBUS that is still falling (>= 50 mV per 600 ms, up to 3 s more) and aborts only on a rise > 150 mV, a stall or the window ending (the suite's own checks are separate);
   `fc trigger` above 20 V is refused (`over limit`).
8. **Do not flash, replug the PC cable or change the USB link setting in the middle of a test.** Never run ad-hoc
   flash loops. Spoof, Self-power and anything touching flash (`emarker save/delete/rename`, `store save-log`)
   write flash: do not power off during them.

## 1. Setup

1. **USB link: HID on.** USB is **off by default**. On the K2: Settings, select `USB Link`, OK, then
   toggle the **HID** row with OK (footer `Replug to apply`), then replug. Tools and keys: see section 2 of
   each test for the exact presses; a `press` takes `back`, `minus`, `ok`, `plus`.
2. **Cable:** the left port needs a **USB C-to-A cable** to the PC. A C-to-C cable does not enumerate there.
3. **Check the link:**
   ```bash
   R ping            # firmware version, git hash, uptime
   R state           # page, VBUS/IBUS, chip, fc name, link state
   ```
   Record the git hash from `ping` in the results log (section 3). `R --json <cmd>` gives machine-readable output.
4. **Evidence directory** (once per session; keep it out of the repo):
   ```bash
   EV=~/k2-evidence/$(date +%F)-$(R ping --json | python3 -c 'import sys,json;print(json.load(sys.stdin).get("git","x"))')
   mkdir -p "$EV"
   ```
   If the `ping` JSON key differs, use the hash by hand. Evidence file names below are `$EV/<test-id>-*`.
5. **Before every test, start the Black Box clean** (the ring is RAM, the captures persist):
   ```bash
   T clear                      # empty the PD / FUSB302 / VBUS ring
   T status                     # ring fill, frozen or not
   T live --tail --raw "$EV/T0x.raw" > "$EV/T0x-live.txt" &   # optional: follow the test live
   ```
6. **After every test, save the evidence:**
   ```bash
   T dump --raw "$EV/T0x.raw" --jsonl "$EV/T0x.jsonl" > "$EV/T0x-trace.txt"
   R screenshot "$EV/T0x-screen.png" --scale 2
   R state > "$EV/T0x-state.txt"
   T captures                   # trigger-frozen captures (contract change, hard reset, VBUS dip, detach)
   ```
   Stop the background `live` (`kill %1`). `T decode FILE` re-reads a `--raw` dump later.
7. **Page and menu names.** Rotation pages: `R page home|detail|chargertest|pd|diag|lines|tools|settings` (or 0..7). Menus open from
   Tools: select with short `minus`/`plus` (Up/Down), `ok` opens, short `back` returns. Menu order:
   FC trigger, E-marker, Identify, PD trigger, Cable R, Black Box, Spoof (USB Link and Autorotate are under Settings).
8. **Remote vs on-device.** Each test lists both. The remote variants never replace looking at the screen once
   per release: take the screenshot.
9. **Equipment, minimum set:** one known USB-C PD charger with a PPS APDO; a Type-C-only 5 V source (plain
   5 V brick with C port, 1.5 A or 3 A Rp); chargers per protocol (see each test); a USB-C cable (e-marked and
   not); a Pixel (or another phone) only for Identify and only as the device on the **top** port;
   a USB C-to-A cable for the PC; optionally a resistive dummy load rated for the voltage under test.

## 2. Tests

### On-device charging suite

The **Charger test** main tab is between Detail and PD (`R page chargertest`, index 2).
It automates the voltage-negotiation portions of T01/T02/T03 and T06-T10; it does not replace
the setup, supervision or evidence instructions below. Inline-refusal, Type-C/passive
classification and the non-charging procedures remain manual. UFCS is implemented but not hardware-verified; VIVO is not implemented.

- Entering the tab always starts at Groups; prior selections remain, but confirmation is cleared.
  Groups open algorithm checklists with short +/- and OK; all 13 visible entries start selected.
  USB PD exposes only **PD fixed** (SPR/EPR fixed offers) and **PPS** (PPS/SPR AVS/EPR AVS).
  Selections govern execution. Empty selections are refused. Continue to Cable/Spoof, then Review.
  Saved identity selection does not arm spoof; only its explicit arm row does. Spoof defaults off
  at boot and persists across page exit. It never raises the physical-cable allowance.
  VIVO reports a reasoned SKIP (UFCS runs 5/9/12 V then 5 V). There are no independent minor-revision rows.
  Header revision 1 means PD2.0, 2 means PD3.x; only observed source RMDO reports minor revisions.
  Results show the version stored with the run, not a later live source's version.
- To proceed from a checklist, BACK returns to Groups; scroll to **Next: Cable / Spoof**.
  Pick an identity only if needed (picking does not arm), then select **Next: Review**.
  Scroll to **START tests** and use the two-OK confirmation below.
- Scroll to **START tests**, then press OK twice within five seconds, confirming that
  the output is empty and you are watching. Entering the tab or taking a screenshot never starts it.
  The first OK only opens the confirmation; the second arms the chosen identity and presents Rd (the "Waiting for
  charger" screen when VBUS is not up yet), and the run then starts by itself: no third press.
  A missing monitor/source, inline load, fake offers, another running tool or pending FC release refuses the suite.
  The source must already be measured in the 4.75-5.50 V vSafe5V range; release any previous trigger first.
- Fixed PD tests every advertised, requestable PDO below the monitor ceiling. Each PPS/AVS offer
  gets minimum, midpoint, maximum, midpoint, minimum targets, rounded to 20/100 mV, capped by the
  monitor. Requests use up to 1 A, capped by the offer: **this is not a load or rated-current test**.
  An advertised range above the monitor ceiling gets an explicit skipped maximum.
  SPR PPS may request up to 21 V on a 20 V cable; its current limit still follows physical-cable
  evidence (3 A if unknown). This exception does not change fixed-PD, legacy, or EPR limits.
  For a nominal 20 V cable, measured VBUS up to 21 V is allowed for the fixed 20 V tolerance;
  fixed and legacy request targets remain capped at 20 V.
  A valid, wire-observed PD3 passive-cable identity can establish a 30 V physical limit;
  that does not satisfy the separate 50 V / 5 A EPR requirement.
  EPR/AVS use already available offers; the suite does not discover absent EPR capabilities.
- QC2 tests 5/9/12 V (Class A rows; "QC2 Class B" (20 V row) and "QC3 Class B" (5/12/20/12/5 ramp) are in the default selection and run after them: a charger that stays at 12 V shows them as a neutral skip "Class A only (12 V max)", never a failure; a 20 V miss on a Class A charger is expected). QC3 and QC3+ test 5/9/12/9/5 V with
  retargeting on the same handshake, proving both directions (QC3 spec range 3.6-12 V).
  AFC tests 9/12 V, FCP 5/9/12 V, SCP handshake, 10 V 2.25 A, 4.5 V 5 A, 5 V 4.5 A (the suite draws no current, so the cable rating gates no row; a load above 100 mA aborts the row; the handshake counts as "detected"). VOOC/SVOOC/SVOOC65 test their
  existing 5 V responder modes, not claimed proprietary high-power operation.
- **PD-to-QC is not automatic.** A live PD capability table blocks legacy tests, reported
  `SKIP / PD active: use QC port`; the suite never uses `allow-inline`. Run the legacy selection
  separately with a QC-only output/cable setup, no PD contract, and nothing downstream.
- Each target must remain in band with the correct protocol state for one continuous second.
  QC2 uses +/-2%, QC3/QC3+ +/-300 mV, PD uses +/-5% (at least +/-100 mV, except fixed 5 V
  uses vSafe5V), and other legacy protocols use 85-110%. QC2 may take up to 12 s after its
  handshake to reach the engine band; the suite allows 15 s from request for QC2 negotiation and 12 s for every other protocol.
  Missing samples reset the hold; PPS keep-alive renegotiations are allowed, hard resets fail.
- Fixed/discrete targets and protocol boundaries release to 5 V. PPS and continuous QC/SCP targets
  retain their contract for up/down testing. After release, both driver quiescence and measured
  4.75-5.50 V must hold for 500 ms before continuing. Failure to release in 8 s stops the suite
  with **UNSAFE: unplug source**; it cannot resume tests automatically. While Releasing and Unsafe the release is re-issued every 250 ms.
- BACK or OK stops; leaving the page also stops. Cleanup continues while another main tab is
  shown. New tools and remote trigger/debug operations are blocked until cleanup completes.
  Remote `pd release` or `fc reset` also cancels. Never rewire until you have checked 5 V.
- Results show target, PASS/FAIL/SKIP and measured voltage; +/- scrolls and selects the explanatory
  reason below the table. A failed initial contract skips its later retargets, not the entire suite.
  Results and selection are RAM-only and are replaced by the next run. OK returns to setup.
  `[TEST]` log lines record targets, request current, voltage and explicit driver failures.
  Save the Black Box, screenshot and state evidence as in section 1.6.

**Read-only suite evidence** (after a human starts the run on the device):
```bash
R charger-test status
R --json charger-test results
R charger-test results --out "$EV/charger-test-results.json"
R ping > "$EV/charger-test-firmware.txt"
```
The host only retrieves the already stored rows, including during active, releasing, stopped,
complete or unsafe states. It never starts/advances/stops a run, requests voltage, changes pages
or writes device flash. The JSON includes schema, boot-local `run_id`, `revision`, state/reason,
total/current (zero-based index; equal to total when completed), and per-step protocol, PDO position/raw
word, target mV, requested mA, stored measured mV, result/reason and raw/decoded driver detail.
`measured_mv` is the existing stored integer, **not** a live reading. Zero is ambiguous:
no sample stored yet, or a non-positive/missing sample clamped to zero by the runner.
Pending/running/skipped rows are not passes; an unsafe release remains unsafe even if earlier rows passed.
Read `R state` and personally verify 5 V before rewiring; retrieval does not release anything.
Results are RAM-only, replaced by the next run and lost at reboot. Record firmware identity separately.
Each response carries at most two rows; a changed run/revision answers Busy. If the table changes
during export, retry `results`; `--out` is written only after the entire consistent table is received
and final metadata checked (an existing output file is untouched on retrieval failure).
Old firmware answers UnknownCmd rather than falling back to any active operation.

Host tests cover the plan, bands, up/down requests, missing samples, wraparound, refusal,
timeouts, abort/release failure, screenshot purity, exact retrieval bytes/JSON, paging errors
and read-only behavior in active/releasing/finished states. The grouped suite's fixed-PD and
PPS results have partial hardware coverage; see section 3. QC remains unverified. Flashing or
safe UI inspection alone is not a charging-protocol pass.

Status notes: **verified** = seen on the K2 with real log lines or a screenshot (with the date);
**not verified** = code and host tests exist, nobody has watched it work.

### T01 PD fixed: 5 / 9 / 12 / 15 / 20 V

- **Risk:** HV. **Status:** 5, 9, 12, 15 and 20 V voltage negotiation **verified** on
  `4d396cd` and `71ed6b5` with a 1 A request and no downstream load (2026-11-02);
  rated-current and rated-power operation remain untested.
- **Prerequisites:** a PD charger with fixed PDOs at the voltages under test, on the **bottom port**; nothing
  on the top port (no inline load); Black Box cleared; Who's attached page Source tab shows the PDO list (`R pdos`).
- **Steps, on-device:**
  1. Page PD (long `plus` from Home, or `R page pd`): the offers list shows the charger's PDOs.
  2. Tools, open `PD trigger`. OFFER step: `plus`/`minus` pick the PDO, OK next.
  3. CONFIRM step: summary card `9.00V 3.00A = 27.0W` with the above-5 V caution band. OK twice (above 5 V).
  4. LIVE card: banner `Requesting`, `Accepted`, `PS_RDY`; REQUESTED vs MEASURED V and A.
  5. Back: BACK from LIVE releases to 5 V. Repeat for the next PDO.
- **Steps, remote:**
  ```bash
  R pdos                                  # decoded source caps; note the position N of each fixed PDO
  R pd status                             # idle
  R pd request N --confirm                # N = 1-based PDO position; above 5 V needs --confirm
  R pd status                             # state ready, result ok, pos N, mv ...
  R state                                 # vbus_mv in the pass window
  R pd release                            # request PDO 1 (5 V)
  R pd status
  ```
- **Expected:** `pd status` goes `requesting`, `transition`, then `ready` with result `ok`; `state.vbus_mv`
  in the 5 V vSafe5V range or within +-5 % of higher PDO voltages (5 V: 4750..5500; 9 V: 8550..9450;
  12 V: 11400..12600; 15 V: 14250..15750; 20 V: 19000..21000). The 2026-11-01 reading of 9.34 V is a pass.
  Black Box (`T dump`): `TX Request`, `RX Accept`, `RX PS_RDY`, then `VBUS` samples at the new level; a
  trigger-frozen capture is created on the contract change (`T captures`, `T capture N`).
- **Pass:** every offered fixed PDO reaches `ready`/`ok` and VBUS in the window within 2 s of PS_RDY; release
  returns VBUS to 4.75..5.50 V. **Fail:** `rejected`, `timeout`, `hard-reset`, VBUS outside the window, or a
  VBUS dip capture during the hold.
- **Cleanup:** `R pd release`, confirm `vbus_mv` about 5000, then save the evidence (section 1.6).

### T02 PD PPS: up and down steps

- **Risk:** HV. **Status:** PPS stepping up and down in smaller increments **verified** up to
  20 V on `4d396cd` with no downstream load (2026-11-02). Direct 11-to-8 V and 20-to-12.5 V
  step-downs failed the existing overvoltage guard. The 21 V endpoint passed on `2aeff99`
  and `71ed6b5`; a direct 21-to-13 V step-down also failed that guard. See section 3 for scope and measurements.
- **Prerequisites:** charger with a PPS APDO (e.g. 3.3-21 V); bottom port; nothing on top.
- **Steps, on-device:** Tools, `PD trigger`; OFFER: pick the PPS row, OK. VOLTAGE step: big value with a
  range bar, short `plus`/`minus` 20 mV, long 1 V, OK. CURRENT step: 50 mV... 50 mA steps, long 0.5 A,
  capped at the offer, default offer max, OK. CONFIRM: OK twice above 5 V. LIVE: watch `PS_RDY` and
  MEASURED. **Step-down:** while LIVE, go BACK one step and re-send a lower voltage, or send again from the
  wizard with a lower value; do not exit via BACK from LIVE (that releases to 5 V).
- **Steps, remote:**
  ```bash
  R pdos                                          # PPS position N and its min/max
  R pd request N --mv 9000 --ma 1000 --confirm    # step to 9 V
  R pd status; R state
  R pd request N --mv 15000 --ma 1000 --confirm   # step up
  R pd status; R state
  R pd request N --mv 9000 --ma 1000 --confirm    # step DOWN (the unverified case)
  R pd status; R state
  R pd request N --mv 5000 --ma 1000 --confirm    # down to the PPS minimum area, then
  R pd release
  ```
- **Expected:** after each request `pd status` is `ready`/`ok` with `mv` equal to the request (rounded to
  20 mV); VBUS within +-5 % (PPS: +-100 mV is also acceptable) of the request. The step-down must lower VBUS
  without a hard reset: `hard_resets` in `pd status` stays unchanged.
- **Pass:** all up and down steps reach the requested voltage; no `hard-reset`; no VBUS dip capture.
  **Fail:** VBUS stays at the higher level after the step-down, `rejected`, `hard-reset`, `timeout`.
- **Cleanup:** `R pd release`; save evidence. If the step-down fails, keep the `T dump` and the capture
  (`T capture N`): this is the data the next fix needs.

### T03 PD AVS / EPR: 28 / 36 / 48 V

- **Risk:** HV. **Status:** **not verified.** SPR AVS (9-20 V) has request fields but no UI: **not verified**.
- **Equipment:** an EPR charger (140 W class) with EPR fixed PDOs and an AVS APDO. **This K2's INA226 cannot do
  more than 36 V**; the firmware caps requests at 36 V (T00 rule 2), so 28 V and an EPR AVS target at or
  below 36 V can be tried here, **36 V is the ceiling and 48 V cannot be tested on this unit**. A K2 with an
  INA228/INA237 can request up to 48 V (85 V VBUS rating).
- **Prerequisites:** bottom port, nothing on top, Black Box cleared.
- **Steps, remote:**
  ```bash
  R pd request 0                         # N = 0: only enter EPR mode and read the EPR source list
  R pdos                                 # EPR PDOs now listed (28 V fixed, AVS ...)
  R pd request N --confirm               # 28 V fixed (N = its position)
  R pd status; R state
  R pd request N2 --mv 33000 --ma 3000 --confirm    # AVS APDO: target at or below 36 V on the INA226
  R pd status; R state
  R pd release
  ```
  EPR needs keep-alives; `pd status` shows `epr` true while in EPR mode.
- **Steps, on-device:** `PD trigger`: the OFFER list includes the EPR rows after the EPR list is read;
  CONFIRM warns above 5 V; voltage steps for an AVS row are 100 mV.
- **Expected:** `pd status` `epr: true`, state `ready`, result `ok`; VBUS within +-5 % (28 V: 26.6..29.4).
  A request above the cap is refused by firmware (`result: over-cap` / error `over the voltage cap`): that is
  the **expected pass for 48 V on an INA226**, not a failure.
- **Pass:** 28 V fixed reaches `ready`, EPR keep-alive holds for 30 s with no `hard-reset`; a 48 V request is
  refused as `over-cap`. **Fail:** VBUS above 37.8 V at the INA (the 36 V ceiling + 5 %; a 28 V or 36 V level that lands a few hundred mV high is a pass), hard reset, EPR `failed` result.
- **Cleanup:** `R pd release` (the 5 V safety drop exits EPR), confirm 5 V.

### T04 Trigger refusal with a device inline

- **Risk:** the test is that nothing rises. **Status:** the inline-load loop fix is **verified**
  (2026-11-01); the refusal itself, **not verified** as a dedicated test.
- **Prerequisites:** charger on the bottom port and a **low-risk** load on the top port: a dummy resistive load
  or the lowest-value device you own. **No valuable device.** The rule is that the trigger refuses, but treat
  the first run as live.
- **Steps, on-device:** Tools shows `PD trigger` with `LOAD` and footer `Load attached: unplug it`; open it:
  the refused state shows a checklist with a red item and no way to send. `FC trigger` shows `LOAD`
  (`Load attached: confirm twice`): do **not** confirm twice in this test.
- **Steps, remote:**
  ```bash
  R state                                      # inline: true (link state)
  R pd request 2 --confirm                     # must fail with a refusal; VBUS must not move
  R fc trigger qc2 --volts 9 --confirm         # must be refused: "load attached"
  R state                                      # vbus_mv still about 5000
  ```
  Do not pass `--allow-inline`.
- **Expected:** both commands fail with a clear refusal (`inline`, `load attached`), VBUS stays at 5 V,
  no `TX Request` in `T dump`.
- **Pass:** refused, VBUS unchanged, no PD Request on the wire. **Fail:** any voltage change.
- **Cleanup:** remove the load, then replug if the link state looks stale (`R action pd-rescan`).

### T05 Type-C-only source (no PD, no data pairs)

- **Risk:** PASSIVE. **Status:** **not verified** as a dedicated test.
- **Prerequisites:** a plain Type-C 5 V source (Rp 1.5 A or 3 A, no PD, no D+/D-) on the bottom port.
- **Steps:** Home and Who's attached pages; `R state`; `R pdos`; Lines page (`R page lines`). On-device: Who's attached page empty
  state with the checklist, Home shows VBUS about 5 V.
- **Expected:** `state.pd_valid` false, `num_pdos` 0, `fc` a no-PD classification (`DCP 5V`, `NO QUICK`
  or `USB DATA`; `NO INPUT` only without VBUS). In the Black Box the CC line shows an attach
  and the `CC scan` Rp level (`1.5 A` or `3 A` band), no PD messages.
- **Pass:** VBUS about 5 V measured, no PD data, no crash, the page shows the empty state, not stale PDOs.
  **Fail:** stale PDOs from an earlier charger, or the K2 reports PD.
- **Cleanup:** none (passive).

### FC input/release acceptance (run before T06–T10 after flashing the staged UI)

**Not hardware-verified yet.** Use a known QC3 charger and empty other port. Confirm the flashed
git identity separately; these instructions do not supersede previous smoke results.

1. Tools → FC trigger: short +/- highlights QC3 in a visible list; long +/- cannot change protocols
   or main tabs. OK opens VOLTAGE. Tap changes200mV; a500ms long changes1V **once**. Keep holding:
   there must be no repeats, protocol switches or voltage requests. Check all four rotations;
   the label remains beside its physical key and actions never invert.
2. Set9.00V; OK opens REVIEW, without a request. A long OK sends nothing. Short OK arms;
   another short OK within3s sends the exact reviewed9.00V. Expire the arm, change draft,
   or use the +/- rotation chord: the next OK must only arm again, never send stale permission.
3. While live, PLUS drafts9.20V: HELD stays9.00V; MEASURED must remain the actual reading.
   OK reviews, then OK/OK sends. MINUS drafts9.00V; it requires the same review and fresh
   two-OK confirmation despite decreasing HV. Switching source/session invalidates the arm;
   unplugging during a local active request cancels rather than retargeting another source.
4. Test short BACK and, separately,500ms long BACK while requesting/live/editing/scanning:
   release starts immediately, without confirmation. OK also stops from LIVE. A subsequent
   short BACK leaves. Remote `page home` during a live trigger must also cancel.
5. Watch **Releasing; measuring VBUS**, then **5V verified** only after4.75–5.50V continuously500ms.
   During pending release, navigate Home/PD/Charger test and take screenshots/state readings:
   they must remain usable, but PD/FC requests, other active menus and suite START must refuse.
   A reset command alone must never manufacture a safe verdict.
6. A source that does not release, or a missing power measurement, must reach the8s unsafe
   warning and keep the interlock. Do not induce an unsafe supply on valuable equipment;
   verify actual VBUS independently and unplug. Recover only with measured safe5V.
7. Hold BACK toward reboot: cancellation happens at500ms; reboot still occurs at5s. Releasing
   the key cancels reboot, **not** the stop. Do not infer successful5V cleanup from a reboot.

Save screens for list/edit/review/live draft and post-exit release, plus `[KEY]`/`[FC]` logs and
measured VBUS. Host tests cover timeouts, unknown readings and source/session changes; hardware
coverage must demonstrate real measured transitions, not merely a reset log.

### T06 Qualcomm Quick Charge 2.0 (5 / 9 / 12 / 20 V)

- **Risk:** HV. **Status:** **not verified in this tree.**
- **Prerequisites:** a QC2/QC3 charger (HVDCP) on the bottom port, **no PD contract** (a charger that shows
  PD contracts first may refuse: unplug and replug); nothing on top; D+/D- free.
- **Steps, on-device:** Tools → `FC trigger`; short +/- picks `QC2`, OK opens the draft editor.
  Short +/- selects5/9/12/20V (long selects one offered level, not a protocol). OK opens REVIEW.
  At5V one review OK sends; above5V two short review OKs within3s send the exact value.
  LIVE shows REQUESTED vs MEASURED and the10s trace. Live +/- opens a retarget draft; review
  and confirm each above5V target anew. BACK short/long stops first; wait for measured5V verification.
- **Steps, remote:**
  ```bash
  R fc status
  R fc trigger qc2 --volts 9 --confirm
  R fc status                             # state active, vbus_mv
  R fc trigger qc2 --volts 12 --confirm   # retarget
  R fc trigger qc2 --volts 20 --confirm   # the highest QC2 level: only on a QC 20 V capable charger
  R fc reset                              # back to 5 V, then passive idle
  ```
- **Expected:** `fc status`: `negotiating`, then `active`; VBUS in the engine band 85 %..110 % of target
  (9 V: 7.65..9.9; 12 V: 10.2..13.2; 20 V: 17..22). Spec: D+/D- levels 3.3/0.6 V (9 V), 0.6/0.6 V (12 V),
  3.3/3.3 V (20 V). Log: `[FC] start QC2 9000mV confirmed=1 ... -> none`, `[FC] QC2 active vbus=...`.
  A 20 V request on a 9/12 V-only charger ends in `voltage not reached`.
- **Pass:** each supported level is within +/-2% of its target for at least 1 s; `fc reset` returns to
  4.75..5.50 V. **Fail:** `no handshake`, `no response`, `overvoltage`, VBUS above target + 10 % (the K2 aborts).
- **Cleanup:** `R fc reset`; confirm 5 V.

### T07 Quick Charge 3.0 (increment and decrement) and QC3+

- **Risk:** HV. **Status:** **not verified in this tree.**
- **Prerequisites:** a QC3.0 charger (Class A or B; QC3+ needs a QC3+/SCP-capable one); as T06.
- **Steps, on-device:** short +/- selects `QC3` (or `QC3+`), OK edits. QC3 range3.6–20V
  (above12V requires a Class-B source); QC3+ remains3.6–12V. The live trace scale is0–20V:
  tap200mV, long1V once (QC3+ still uses20mV pulses on the wire). OK reviews; OK/OK sends above5V.
  Live +/- opens a draft without changing VBUS. Review and freshly confirm both upward and
  downward HV targets; HELD/DRAFT/MEASURED remain distinct until apply.
- **Steps, remote:**
  ```bash
  R fc trigger qc3 --volts 9 --confirm        # enters continuous mode and pulses to 9 V
  R fc status
  R fc trigger qc3 --volts 9.6 --confirm      # + steps (200 mV per pulse)
  R fc trigger qc3 --volts 5 --confirm        # - steps
  R fc trigger qc3+ --volts 9 --confirm       # QC3+ (20 mV pulses)
  R fc reset
  ```
- **Expected:** VBUS follows the target within one step (+-0.3 V for QC3) in both directions; QC3 spec:
  D+ 0.6 V and D- 3.3 V baseline, +200 mV with D+ = 3.3 V for 2.25 ms, -200 mV with D- = 0.6 V for 2.25 ms.
- **Pass:** up and down moves both change VBUS in the expected direction and settle in the band; `fc status`
  `active`. **Fail:** VBUS jumps to the 20 V level, no change on decrement, `overvoltage`.
- **Cleanup:** `R fc reset`.

### T08 Samsung AFC

- **Risk:** HV. **Status:** **not verified in this tree.**
- **Prerequisites:** a Samsung AFC charger (9 V / 12 V) on the bottom port; as T06.
- **Steps, on-device:** `FC trigger`, short +/- picks `AFC`; OK edits, +/- chooses9/12V,
  OK reviews, then two short OKs confirm. Repeat review/confirmation for live retargets.
- **Steps, remote:**
  ```bash
  R fc trigger afc --volts 9 --confirm
  R fc status
  R fc trigger afc --volts 12 --confirm
  R fc reset
  ```
- **Expected:** `active` and VBUS at 9 V (7.65..9.9) / 12 V (10.2..13.2). The AFC wire exchange is on D-
  (command bytes, odd parity). `state.fc` classifies it as `AFC FCP` while up.
- **Pass:** target level reached and held 5 s; **Fail:** `no handshake` (no AFC reply), `no volt`
  (`voltage not reached`).
- **Cleanup:** `R fc reset`.

### T09 Huawei FCP and SCP

- **Risk:** HV. **Status:** **not verified in this tree.**
- **Prerequisites:** a Huawei FCP charger (5/9/12 V) and, separately, an SCP/SuperCharge charger (SCP target
  3-11 V). As T06.
- **Steps, on-device:** `FC trigger`, select `FCP` (levels 5, 9, 12 V) or `SCP` (3..11 V in 1 V steps),
  OK edits, +/- sets the target (SCP tap/long1V), OK reviews, then OK/OK above5V.
  SCP holds the link with a 100 ms keep-alive: leaving the page or BACK stops it (the charger
  drops to 5 V by itself after 500 ms without traffic).
- **Steps, remote:**
  ```bash
  R fc trigger fcp --volts 9 --confirm
  R fc status
  R fc trigger fcp --volts 12 --confirm
  R fc reset
  R fc trigger scp --volts 5 --confirm
  R fc trigger scp --volts 9 --confirm
  R fc reset
  ```
- **Expected:** FCP: `active`, VBUS 9 V / 12 V in the band; log `[FC] reg 0x.. = 0x..` lines for ACCP
  register reads. SCP: `active`, VBUS follows the target; the state holds for 30 s without `source lost`.
- **Pass:** target reached and held 30 s (SCP keep-alive proves itself here). **Fail:** `no handshake`,
  `lost` after 0.5 s (keep-alive broken).
- **Cleanup:** `R fc reset`.

### T10 OPPO VOOC / SuperVOOC

- **Risk:** HV. **Status:** **not verified in this tree.**
- **Prerequisites:** a genuine VOOC / Warp charger, and separately a SuperVOOC 1.0/2.0 charger (5 A or
  6.5 A). Genuine adapters only; the K2 pulses an internal load to wake them. Nothing on the top port.
- **Steps, on-device:** `FC trigger`, short +/- picks `VOOC`, `SVOOC` or `SVOOC65`. Fixed5V
  skips editing: OK opens REVIEW; one further short OK sends. Confirm the other port is empty.
- **Steps, remote:**
  ```bash
  R fc trigger vooc --volts 5 --confirm
  R fc status
  R fc reset
  R fc trigger svooc --volts 5 --confirm
  R fc reset
  ```
  `--volts` is required by the CLI; the on-device default for these three is 5 V (the adapter then drives
  its own step).
- **Expected:** the adapter's command frames are answered (VOOC: `0xC5` to `0xA8/0xAC/0xA6`; SuperVOOC:
  the table in the FC spec section 6.2); `fc status` `active`; `state.fc` `VOOC WARP` or `SVOOC`.
- **Pass:** `active` and the adapter does not drop out for 30 s. **Fail:** `no response` (no clock frames on D+).
- **Cleanup:** `R fc reset`.

### T11 UFCS (and VIVO FlashCharge)

- **Risk:** HV, up to 12 V in the suite, released to 5 V after every level. **Status:** UFCS engine **built only,
  not verified on hardware**. VIVO: frame codec only (`include/k2/fastcharge/vivo.hpp`), no engine; the suite SKIPs it.
- **Charger needed:** the Baseus EnerCore CJ21 67 W (PD 3.0, QC 3.0, AFC, FCP, PPS) has **no UFCS, no VIVO and
  no VOOC**, so it cannot verify any of these. Use a charger that lists UFCS (many Chinese 65 W+ bricks) for UFCS,
  a vivo FlashCharge brick (44/66/120 W) for VIVO, and an OPPO VOOC/SuperVOOC brick for T10.
  On the CJ21 the correct outcome of the UFCS rows is `no handshake` (D+ does not follow D-), with VBUS staying at 5 V.
- **Steps, on-device:** Charger test tab, select the UFCS group, review, confirm. Expect rows UFCS 5/9/12/5 V.
- **Expected on a UFCS source:** log `[FC] start UFCS ...`, then `Active` at 5 V (the first row), 9 V and 12 V
  within the capability list, VBUS within 10 % of the target, a ping about every 100 ms; the final row returns to 5 V.
  Capture the wire with the Black Box and compare against `docs/FASTCHARGE_PROTOCOLS.md` §4.
- **First things to check on hardware** (unconfirmed): the handshake reads (D+ polarity), the
  keepalive command number (0 in our sender), whether the source accepts 8N2, and that a 5 V Request
  followed by the bus reset releases the source.
- **Pass:** every offered level is reached and VBUS ends at 5 V. **Fail:** VBUS above target + 10 %, no return to 5 V.
- **Cleanup:** `R fc reset`.

### T12 Fast-charge auto-detect scanner

- **Risk:** HV, goes up to **9 V** and back to 5 V after every step. **Status:** **not verified in this tree.**
- **Prerequisites:** any charger (best: a multi-protocol one), bottom port, nothing on top, D+/D- free.
- **Steps, on-device:** Tools → `FC trigger`; short +/- picks `SCAN` (first entry), OK opens REVIEW
  with the HV warning, then two short OKs within3s start scanning. The scan card shows result tiles
  filling in (accepted / `no` / skipped). BACK short/long or OK stops; exit does not drop measured cleanup.
- **Steps, remote:**
  ```bash
  R fc scan --confirm
  R fc status                         # scan.running true, scan.step n
  R fc status                         # repeat until scan.done true; scan.results lists each protocol
  R fc reset
  ```
- **Expected:** the scan steps FCP, SCP, AFC, QC2, QC3, VOOC in turn; each step returns the output to 5 V; VBUS
  never above 9 V + 10 % (9.9 V). Results: protocols the charger supports show `accepted` with the voltage
  reached; others `rejected` or `skipped`. Log: `[FC] scan start confirmed=1 ...`, `[FC] scan <proto> accepted <mv>mV`.
  Black Box `VBUS` samples show the steps.
- **Pass:** all steps complete and the final VBUS is 5 V; the accepted list matches what the charger label
  claims. **Fail:** VBUS above 9.9 V, the scan hangs (`scan.running` for over 60 s), results contradict the
  charger label.
- **Cleanup:** `R fc reset`.

### T13 Passive legacy classifier (Apple / DCP / Samsung)

- **Risk:** PASSIVE. **Status:** classifier **verified** (it reads D+/D-, no driving); per-class rows
  Apple 2.4/2.1/1.0 A and SAM 2.0 A **not verified** with real chargers.
- **Prerequisites:** chargers with no PD contract: an Apple 5 V brick (2.4 A / 2.1 A / 1.0 A), a plain
  DCP (D+ and D- shorted), a Samsung 2.0 A charger. Bottom port, nothing on top.
- **Steps:** on-device: Home (protocol pill) and Lines page (`R page lines`: D+/D- bars against the 0.4 V and
  2.0 V ticks, verdict card). Remote:
  ```bash
  R state                             # fc: ... ; dp_mv, dm_mv
  R --json state
  R watch --count 5 --interval 500    # V/A/W line per poll
  ```
- **Expected (`state.fc`, FC spec section 2):** Apple 2.4 A: D+ and D- both about 2.7 V, `APPLE 2.4A`; 2.1 A:
  `APPLE 2.1A`; 1.0 A: `APPLE 1.0A`; plain DCP: `DCP 5V`; Samsung: `SAM 2.0A`. The classification must
  stay stable for 10 s.
- **Pass:** the name matches the charger. **Fail:** wrong name, or the name flips while nothing changes.
- **Cleanup:** none.

### T14 Identify (Pixel)

- **Risk:** low (PD VDMs only; idle CC). **Status:** **verified** on a Pixel (2026-11-01: VID 18D1,
  PID 4EE1). **Pixel replug behaviour after Identify: not verified.**
- **Prerequisites:** the device on the **top** port (the K2 is the source toward it: the K2 must be
  unpowered by a charger on the bottom port or powered from the PC; follow the Identify setup card). No charger
  inline. Use only a device you can afford to replug.
- **Steps, on-device:** Tools, `Identify`; setup checklist green; OK = `Run`; the result lists the identity,
  SVIDs, modes, sink caps.
- **Steps, remote:**
  ```bash
  R identify-device --wait 20     # runs the sequence (idle CC only); add --confirm only with a live sink
  R identity                      # VID/PID, SVIDs, Apple model
  ```
- **Expected:** `identify.state` `done`; steps `identity, svids, modes, sink-cap, status, revision`; for the
  Pixel VID `0x18D1`, PID `0x4EE1`. Log: `[ID] Identify started`. Black Box: `TX` Discover Identity, `RX` ACK.
- **Pass:** `done` with the expected VID/PID; **Fail:** `aborted`, no identity, `[ID] blocked:` lines.
- **Cleanup:** then unplug and **replug the Pixel** and note whether it charges again: that is the open item.

### T15 E-marker: read, save, rename, spoof

- **Risk:** read VCONN 5 V on CC; save/rename/delete **FLASH**; Spoof **HV** (a faked 5 A / 50 V cable).
  **Status:** read and save **verified** (2026-11-01). Rename and delete: library code
  **verified**, remote paths not run as a dedicated test. Spoof acceptance by a charger: **verified** on the top
  plug (2026-11-06: a Baseus 140 W offered 5 A and EPR 28 V with no e-marked cable; `K2_PORTS_SPEC.md`).
- **Prerequisites:** an e-marked 5 A USB-C cable and, for the Spoof part, a PD charger and a **non-e-marked**
  cable. Read: the cable alone, **no charger**.
- **Read, on-device:** Tools, `E-marker`; connect the cable between the two ports (the K2 powers VCONN);
  OK = `Scan` (or `Listen` when a charger is present: listen only). Result: `5A 50V 240W` plus pills.
  Long OK opens the `Menu`: `Save cable`, `Saved cables N`.
- **Read, remote:**
  ```bash
  R action emarker                   # run the scan (15 s timeout inside the CLI)
  R emarker                          # decoded cable e-marker data
  R emarker save                     # save the inline cable
  R emarker list                     # saved cables with their ids
  R emarker rename ID "Desk USB4"
  R store list                       # records in the store
  ```
- **Spoof, on-device:** Tools, `Spoof`: the list of saved cables, OK = `Arm` (or `Disarm`). The page shows
  `Asked N  Replied N` live. Refused while a live sink is inline (`LOAD`). **Remote:**
  ```bash
  R emarker spoof status
  R emarker spoof ID                 # arm a saved cable
  # plug the charger through the K2 with the non-e-marked cable, watch: T live --tail
  R emarker spoof off
  ```
  Delete when done: `R emarker delete ID --confirm`.
- **Expected:** read: `valid`, VID/PID, current/voltage rating match the cable label; save: `Saved as <name>`,
  a `CABLES/<name>.JSN` appears on the K2 DRIVE (T20). Spoof: log `[SPOOF] armed: answering SOP' ...`,
  then `[SPOOF] Discover Identity on SOP': reply msg_id=.. sent` per charger query, `[SPOOF] off: N requests,
  M replies`.
- **Pass:** read and save as above (done); spoof counts `Replied` equal to `Asked` and the charger offers
  its 5 A / higher PDOs (compare `R pdos` with and without Spoof). **Fail:** `Replied` 0 (an early reply-timing
  problem; the top-plug Ra mode in `K2_PORTS_SPEC.md` is the working recipe), or a charger Hard Reset after the reply.
- **Cleanup:** `R emarker spoof off`; delete test entries if you saved extras.

### T16 Cable resistance (passive and Kelvin)

- **Risk:** PASSIVE (never drives VBUS). **Status:** passive estimate **not verified** against a reference
  cable. Kelvin 8-wire is selectable (`cable-r kelvin`, Mode on the page) but needs a **Kelvin sense
  fixture** on CC1/CC2/D- and at least 0.45 A of load: **not verified**.
- **Prerequisites:** a charger on the bottom port, the cable under test and a load drawing 1 A or more on the
  top port (a dummy load), plus a reference cable of known resistance if you have one.
- **Steps, on-device:** Tools, `Cable R`. `plus` = Mode (Passive / Kelvin), OK = Reset (clears the idle
  reference). Steps: idle reference (no load), then attach the load.
- **Steps, remote:**
  ```bash
  R cable-r reset
  R cable-r passive
  R cable-r status               # idle reference, then loaded point, result
  R cable-r kelvin               # only with the sense fixture; below 0.45 A the page says ">= 0.45A"
  ```
- **Expected:** `status` shows the idle V and the loaded V/I and a milliohm result with a grade
  (EXCEL / GOOD / FAIR / POOR). Compare against the reference cable; the passive figure includes the K2's
  own path.
- **Pass:** repeat three times within +-10 % and ordered correctly across two cables; **Fail:** negative
  or wildly changing values. **Cleanup:** `R cable-r reset`.

### T17 Black Box capture and persistence

- **Risk:** PASSIVE (flash writes by the ring, when the link is quiet). **Status:** **verified**
  (2026-11-01): capture and persistence across power-off.
- **Steps:**
  ```bash
  T status                      # ring fill / frozen
  T clear
  # cause an event: T01 contract change, or plug and unplug the charger
  T captures                    # trigger-frozen captures: contract change, hard reset, VBUS dip, detach
  T capture 0 --jsonl "$EV/T17-cap0.jsonl"
  T freeze; T dump; T resume    # freeze keeps the ring for a later dump
  R blackbox status
  ```
  Persistence: do a T01 request, wait 30 s with the PC link idle, unplug the K2 completely, replug, then
  `T captures` and `T dump` again; on the device: Tools, `Black Box` (list, OK = Open; timeline, OK = Link).
- **Expected:** records `RX`, `TX`, `CC`, `LINK`, `SINK`, `VBUS` with timestamps; the capture slots survive
  power-off. `T clear-captures` / `R blackbox clear-captures` drop them.
- **Pass:** the contract change appears in a capture before and after the power cycle. **Fail:** empty after
  replug. **Cleanup:** `T clear-captures` if you need a clean state (it erases evidence: save it first).

  **Record a whole charger run (the ring wraps in seconds, HID is busy with `ct_start.py`):** the debug shell
  port carries the records live, so two terminals work in parallel (needs a USB build with the CDC shell: HID
  or HID+Drive mode, left port with a C-to-A cable; `T stream` takes no device lock and does not touch HID).
  ```bash
  # terminal 1, start first; Ctrl-C ends it and prints the loss summary
  python3 tools/pd_trace.py stream --jsonl "$EV/run.jsonl" --raw "$EV/run.raw"
  # terminal 2
  python3 tools/ct_start.py --wait 560
  ```
  `--port` (default `/dev/ttyACM0`), `--seconds N`, `--all` (also the records already in the ring). The last
  line reads `<n> frames, <n> records, <n> lost on the device, <n> wire gaps, <n> bad frames, ...; device sent
  <n> records, reported <n> lost`. **Pass:** `lost` 0 and no wire gaps over the run; any hole is printed inline
  (`--- device lost N records ---`) and as a `marker` row in the jsonl. `T decode "$EV/run.raw"` re-reads the raw
  file. Status: **built only** (host tests; not run on hardware yet).

### T18 USB link modes: HID / Drive / Network

- **Risk:** low (USB packet memory budget only). **Status:** HID and Network **verified** (2026-11-01);
  Drive (`K2 DRIVE`) **verified** per ROADMAP; Drive and Network cannot be combined.
- **There is no remote command to change the mode**: it is a setting, kept in flash, applied on replug.
- **Steps, on-device:** Settings, `USB Link`: three rows (HID, Drive, Network) with ON/OFF pills; OK toggles the
  selected row (Network turns HID on and Drive off, Drive on turns Network off, HID off turns Network off);
  `plus`/`minus` pick the row; footer `Replug to apply`. Replug the PC cable after each change.
- **Checks:**
  ```bash
  # HID: R ping works. Drive: a disk named K2 DRIVE appears with STATUS, the log, CABLES/*.JSN
  lsblk -o NAME,LABEL | grep -i K2
  # Network: the host gets 192.168.7.2 by DHCP; open the dashboard
  ip -4 addr | grep 192.168.7
  curl -sS --max-time 5 http://192.168.7.1/ | head
  ```
  In Network mode the dashboard serves at http://192.168.7.1/. If HID is on too, `R ping` still works.
- **Pass:** each mode enumerates and its check works after a replug; switching back to HID-only restores
  the plain log channel. **Fail:** no enumeration; C-to-C cable on the left port (use C-to-A).
- **Cleanup:** leave HID on.

### T19 Self-power (PLUS held at plug-in)

- **Risk:** low. **Status:** **known failing** (2026-11-01): with a charger alone on the bottom
  port the K2 runs only while PLUS is held and dies on release; `SELF-PWR` stays up only on a laptop USB-C
  port. Do **not** treat a repeat of this as a regression; see `ROADMAP.md` (Self-powered boot) and T23.
- **Prerequisites:** the K2 unplugged from everything; a charger on the **bottom** port; a PC cable
  with 5 V cut (data only) for log visibility if you have one.
- **Steps:** hold `PLUS` (Rd on CC2) or `OK` (Rd on CC1) while connecting the charger; the screen shows the
  `SELF-PWR` chip; release the key and watch for 30 s. Boot trace log (when the HID link is up):
  `[BOOT] t=.. vbus=.. cc1=.. cc2=..`, `[SESS] last boot trace`; there is no remote command for this test.
- **Expected today:** the K2 powers off a moment after the key is released. **Pass (target):** the K2 stays
  up for 30 s after release with the charger as the only supply. **Fail (today):** dies on release.
- **Cleanup:** unplug everything.

### T20 Charger test: test units (legacy first in one PD-silent attach, then Pd, then the low rows)

Charger: Baseus EnerCore CJ21 67 W on its built-in USB-C cable (PD 3.0, QC 3.0, AFC, FCP, PPS; no SCP listed), and the Lenovo C135 (QC2/QC3 pass only when no PD contract exists).
No load attached. A session never mixes PD and D+/D- in one attach, so the session emulates a detach only where the session mode changes: Rd off, VBUS dark, Rd on again.

1. Legacy units only: `python3 tools/ct_start.py --units fcp,scp,afc,qc2,qc3` (or the UI with the default order). The state reads "Detach charger" (Rd off, VBUS dark) then "Initializing..." (Rd on, PD-silent), then the rows of every listed unit in that one attach (no detach between legacy units), then "Detach charger" again.
2. Expected per legacy unit: caps are heard but never answered, the charger falls back to legacy within 20 s (one longer retry of 60 s), VBUS stays at 5 V for 500 ms after 2.5 s without PD frames, then the unit's rows run (never with an inline load).
3. Failure case: a charger that never falls back gets "blocked by PD" on that unit's rows; the run goes on with the next unit. A charger that does not come back after a Detach gets "no PD after re-attach" for a Pd unit.
4. Full run: `python3 tools/ct_start.py` runs every legacy unit, then the Pd unit, then the low PPS/AVS rows last. Compare K2 uptime with the run duration: it must not reboot.
5. Cancel during a Detach / Attach ends the run and leaves the sink PD-active (Rd as before). Replug or Rescan if the Who's attached page shows no caps.
Pass: legacy rows get a verdict on a charger that falls back; no step requests above 5 V before the Pd unit; exactly one Request goes out at START (Black Box: one Request id 0, no second one while it is pending).

#### T20b Reading a refused Mi 6 A row (hunt evidence)

Use when a Xiaomi / Redmi source leaves a Mi 6 A row FAIL (finished chain, 5 A cable, 6 A refused) or "not unlocked" and the cause is in question. Read only; it starts nothing and needs no extra run. Causes and what decides each: `docs/CHARGER_TEST_FLOW.md`, "Refused Mi 6 A rows". Evidence belongs to the run in RAM: read it before the next START, and not across a reboot (a resumed run keeps its rows but not the evidence).

1. On the device: Steps list, open the refused row (OK). The detail page shows `Why:` (the cause), `Chain svid/open/auth/re` (Y/n), `Caps N APDO mA cable mA` and `Ask #pos V mA <answer> ms`. A stored transition capture shrinks the page to `Why` and `Chain`.
2. Host, results with the evidence attached: `python3 tools/k2_remote.py charger-test results` (a `hunt:` line under the row; `--json` / `--out file.json` carry a `hunt` object per row), or `charger-test summary` (`Mi 20V 6A ✗ rejected [cause: why]`). An older firmware without command 0x0E just shows no `hunt` line.
3. The exchange itself: the refusal froze a Black Box capture (trigger `6 A refused`, mark 8 `hunt-refusal` with the cause, answer and the APDO limit). `python3 tools/pd_trace.py captures` lists it, `python3 tools/pd_trace.py capture N` dumps it (chain REQs, the re-announced Source_Capabilities, our Request, the answer). Save it before `clear-captures`.
4. Log: the `[HUNT]` line has the same fields (cause, answer and latency, last chain step, flags, caps, PDO and Request words, limit, cable, auth time).
5. Decide: `refused-offer` is the real FAIL: compare the Request word against the 6 A APDO in the capture (voltage, position, operating current). Any other cause says which link is missing; fix that first (e.g. `apdo-stale`: no Source_Capabilities after the auth, so the Request used the old 5 A list).

Pass: every refused hunt row has a cause other than `none`, and the capture holds the Request and the answer. Fail: a refused row with no evidence on a firmware that has command 0x0E, or `unexplained` on a run with a clean trace.

### T21 Charger test: reboot-resume journal

Purpose: a handover or PD reset can cut VBUS and reboot a K2 that is powered from VBUS. The run is journaled on the littlefs volume (/jr, CRC-framed record files, see docs/ARCHITECTURE.md) and offered for resume after boot.

Setup: Baseus EnerCore CJ21 (PD 3.0, QC 3.0, AFC, FCP, PPS; no SCP, so expect SCP skip/fail), K2 self-powered from the charger side (no separate supply), USB HID on, `python3 tools/k2_remote.py` for `charger-test results` / `screenshot`.

1. Run a short plan (PD fixed + PPS + QC3, "Try QC after PD" on). Log shows `[CJRN] used=...`; no `[CJRN] ... failed` line.
2. Force a reboot mid-step: while a step above 5 V is `Holding`, unplug the charger and replug within 3 s (or let the handover cut VBUS). Expected: after boot the K2 opens Charger test on "Resume run?", log `[TEST] journal: run N unfinished ... resume?`.
3. Check rows: "Run N: d/t done", the interrupted step still pending (it is asked again after a resume; a unit that takes the K2 down 3 times is skipped, see T23 part H), VBUS row "Waiting for stable 5V" then "VBUS 5V stable: ready" after 0.5 s. `k2_remote charger-test status` shows `stage: resume` (or already running) and `resumed: 0` before the automatic resume.
4. Touch nothing: a reboot always resumes by itself, in every power mode. Once VBUS has been steady 0.5 s the run continues at the next pending step, `[TEST] resume -> none`, `resumed: 1`, no request above 5 V until the runner reaches a step by itself. A handover interrupted run does not detach again. If VBUS never comes back the Resume screen keeps waiting. (Pressing OK on the Resume screen before the VBUS row is ready only shows "Waiting for stable 5V".)
5. Repeat step 2 and hold BACK while plugging in (BACK held at power-up cancels the resume): results stay (interrupted step Fail/Skip), `[TEST] journal: ... cancelled (BACK held)`, the next boot shows no resume prompt. BACK on the Resume screen while it waits for VBUS abandons the run the same way.
6. Torn record: unplug during a step record write is not directly repeatable; the host test (`make test`, test_charger_journal) cuts power after every half-word.
7. Fill test: run the full suite 4 or more times; erase appears only at a run start when the journal is full (no mid-run LCD stall, heartbeat unaffected).
8. Host-powered resume (built on the host only, 2026-11-07; hardware 2026-11-07 showed "Waiting for stable 5V" forever): HID cable on, charger on the TOP plug (Baseus 70W), start a run, reboot the K2 mid-run (warm reset). After boot the Resume screen reads "Rebooted mid-run" (never "Self-powered" on a host-powered K2) and the row "Rd on, waiting for charger"; log `[TEST] resume: Rd presented, waiting for the charger`, one `[TEST] START e-marker scan` before it. VBUS comes up, the row turns "VBUS 5V stable: ready" after 0.5 s and the run goes on by itself (`[TEST] resume -> none`). BACK while it waits drops the Rd (CC lines back to passive) and shows the results. A journaled cable identity is armed only for a charger directly on the plug (`[TEST] resume: lines say direct, identity armed`); behind a cable the row says "Identity needs a direct plug" and no Rd goes on.

Pass: every expectation above; Fail: a request above 5 V before VBUS has been steady 0.5 s, a resume after BACK held at power-up, a resume that waits for an OK press, or a lost result row.

### T23 Charger-powered K2: PLUS boot, bus power and automatic resume

Purpose: with no HID cable the charger alone powers the K2 (Rd pinned at boot, held through the whole run). Part A proves the boot, part B that a full run survives power losses by itself. Built on the host only (2026-11-06): the routing change (both CC switches closed) is inferred, not yet seen on the connector.

Setup: charger on the TOP plug, HID unplugged, K2 unpowered. Afterwards read the card over HID (left port, C-to-A cable) with `python3 tools/k2_remote.py`; the boot trace is the `[BOOT] t=<ms> sw0=0x..` / `cc=0x..` lines (every SWITCHES0 and CC-switch write of the boot with its time) and `[BOOT] cc write trace: ...` in the log, plus the `boottrace` report of the next boot.

A. Boot: hold PLUS, plug the charger, release PLUS after the logo. Pass: VBUS stays up after the release (no drop, no reboot), the K2 shows the meter, log `[BOOT] mode=...` then (after HID is plugged in later) the boottrace power trace shows no gap. Fail: VBUS falls when PLUS is released.
   A key mode presents ONE Rd (PLUS = CC2, OK = CC1) and never Rd on both lines: Rd/Rd is the Debug Accessory signature, which makes a OnePlus 120W brick turn its Rp off until a physical re-plug (hardware 2026-11-06; the first version widened to both lines 300 ms after reset). The boot trace must show SWITCHES0 0x02 (PLUS) or 0x01 (OK) in every `sw0=` write and never 0x03, and VBUS up within ~150-300 ms on a direct plug. If the charger is on the other orientation (cc on the pinned line ~0 V, Rp ~3.3 V on the other), 600 ms after reset the log shows `[BOOT] no VBUS after 600 ms, Rp only on the other line: Rd moved to CC<n>` once and VBUS follows; the move happens at most once per boot. Run with the HID cable attached (C-to-A) for the trace, with the brick on the top plug, both orientations of the plug.
   A2. `k2_remote pd-reread --wait 20` on the same brick afterwards: the caps are acked by Auto-CRC and a Request must follow within ~2 ms (`[PD] acked caps: request PDO 1 err=0` or the run-start / resume line, the Black Box shows the Request right after the caps), no Hard Reset (`python3 tools/pd_trace.py dump | grep -i hard`), VBUS stays at ~5.2 V throughout (no dip to 0.4 V).
B0. The 5 V contract at boot (host-tested only, see `docs/CHARGER_QUIRKS.md`, `caps-retry-window`): OnePlus 120W brick on the TOP plug, PLUS-boot as in part A, HID plugged only after 30 s (or read the Black Box afterwards from `pd_trace.py captures`). Expect, in the Black Box / log, the first Source_Capabilities, then a Request for PDO 1 within ~5 ms (GoodCRC from Auto-CRC), Accept, PS_RDY, `SINK state=ready`; no Hard Reset, VBUS ~5.2 V throughout, the brick never goes quiet: wait 30 s (the brick's give-up was 7-10 s) and run `k2_remote pdos`: the list is held (`pdo_count` > 0). A START (all units) then plans its PD rows from those caps. Pass: contract within one caps-to-Request gap, still Ready at 30 s, PD rows present. Fail: no Request, `pdo_count 0` at 30 s (the old fault), a Hard Reset, or VBUS below 4 V. Also run once with the brick unplugged and replugged WITHOUT PLUS (HID-powered K2): the boot must stay passive (no Request, no GoodCRC; T24).
   B0b. Gave up anyway: if a source stops announcing mid-attach, START with a PD unit shows the skip reason "PD gave up before START" (not "not advertised"); the card says the same. Needs a source that does that (the old OnePlus fault before this change); otherwise only the host test covers it.
   B0c. Boot Ra: run B on the brick with a cable identity chosen in the Cable list (pick a cable, then PLUS-boot: choose it in the wizard's Cable step or Tools -> Spoof, unplug, replug with PLUS held; the selection is remembered in `/cable.cfg` and read back at boot, so the boot arms it with no run started and no resume needed. A saved clone deleted since reads as Off, a HID-powered boot restores the selection but does not arm). Expect `[BOOTRA] arm cc1=<~3300> pin=2 journaled=1` and `[EMKRA] on` before the first `[PD]` caps line, the boot info text (Tools, boot QR, or `k2_remote bootinfo`) with `ra=1:arm`, and PC13 in the first boot notes (`n=` entry `c<mask+4>`, e.g. `c6`). Fail: `ra=0:noread` (no ADC reading at that moment, the resume's own re-arm then does it), `ra=0:cable` on a direct brick (the cc1 level read below 2400 mV), or VBUS dropping when the Ra goes on. Answered 2026-11-09 (CUKTECH 120 W C1, `runs/selfpwr-6a/`): the brick reads the cable only at its own attach and only when the Ra was already there, so a boot-armed Ra on a self-powered K2 gets no SOP' and no 6 A caps (pattern `cable-look-at-attach`). Still record, per brick, whether it sends SOP' Discover Identity after this boot arming.
   B0d. Planned re-attach (host-tested only; recipe measured by hand 2026-11-09, `runs/selfpwr-drop/e8/`): CUKTECH 10 GaN Ultra 120 W on the TOP plug, PLUS-boot, USB unplugged, the 6 A cable in the Cable list (the built-in "CUKTECH 6A"), no load. The Review confirmation footer reads "Press again, K2 blinks once". START (all units). Expect: `[TEST] journal armed in N ms`, then `[RDI] ...` / `[TEST] planned re-attach` lines, ONE blink-off of the K2 (the screen goes dark about 165 ms of VBUS dark and returns), then `[RDI] comeback boot: reattach pin=2 cable=...`, the charger's SOP' Discover Identity about 136 ms into the new uptime (Black Box: `pd_trace.py captures`), caps with PPS 6 A after the 0x2717 chain, the journal resuming the run (card "resumed 1") and the marker written back as a session (`[RDI] marker written: session`). Pass: one blink, the 6 A rows run, no second reboot. Also: a charger that offers all it claims (an Apple-like PD 2.0 charger, the ASUS 30 W) shows the plain "Press again to start", no blink, and a mid-run VBUS loss comes back pinned with the journal auto-resuming (`[RDI] comeback boot: session`); a bare unplug of a charger after a run leaves a stale marker that the next plain boot releases within 1.5 s (`[RDI] comeback released after N ms`); a short PLUS plug-in shows no boot-info QR, a 3 s hold does. Fail: a second blink, the K2 staying dark, or a plain boot that keeps an Rd for more than 1.5 s with nothing attached.
   B0e. Direct-plug line wake (host-tested only): a charger that wakes only on one CC line (NOVOO RG100 100 W), plain boot on HID power, charger on the top plug directly with no key held, charger test START. Expect the Waiting footer "CC1 picked. Waking charger", then "Now on CC2. Waking charger" (checklist row "Rd on CC2"), `[PD] Rd CC1 -> CC2: no VBUS after 1500 ms on a direct plug (try 1)`, then VBUS and a contract; `k2_remote --json pd status` shows `trial_line` and `trial_moves`. After four moves (1.5 s, 1.5 s, 5 s, 5 s) the K2 stays on its line and the footer reads "No VBUS. Hold + few secs". Never moved: a PLUS or OK boot (pinned line), `pd-reread --line`, Ra mode. Fail: an Rd on both lines at once, or a move after a contract is up.
B. Full run, VBUS kept up (built on the host only, 2026-11-06; the first hardware attempt died after the first unit because every unit began with a detach, and the re-powered K2 then read "stopped (source lost)"). Setup: the OnePlus 120W brick on the TOP plug, PLUS-boot as in part A, HID unplugged while the run goes, no load.
   1. Review shows the note "Self-powered: VBUS stays up" and the footer "No load! OK twice within 5 s". START (all units), do not touch the K2. Press nothing, plug HID only after the run ends (or watch the Black Box over HID from a separate session: one device tool at a time).
   2. Unit boundaries: no detach and no Rd drop. Black Box / log: `[TEST] unit hold: Rd kept, Soft_Reset` with `[PD] Unit hold: Soft_Reset, Rd kept` before the Pd unit, then one Get_Source_Cap and a Request for PDO 1 within ~25 ms of the caps; `[TEST] unit hold: Rd kept, PD-silent` with `[PD] Unit hold: PD-silent, Rd kept` before the first Legacy unit; NO hold between two Legacy units; one more Soft_Reset hold at the end of a run (or a Cancel) that ended after a Legacy unit. No `Detach` / `Attach` state, no `pd-reread`, no `[PD] attach` with CC open, and no Hard Reset (`python3 tools/pd_trace.py dump | grep -i hard`).
   3. VBUS never leaves ~5 V between units: the INA226 min over the run stays above 4.0 V (`k2_remote` state samples, or the Black Box VBUS column). K2 uptime from `ping` equals the wall-clock time since the PLUS boot (no reboot).
   4. The low-voltage programmable rows (PPS / SPR AVS / EPR AVS below 5 V) are skipped with the reason "needs external power" (card line "skipped, needs external power"); nothing else is skipped for that reason.
   5. Charger quirks to record, not to fix: a Legacy unit entered straight from a live PD contract goes PD-silent without a Soft_Reset (some sources Hard Reset there; the run survives it through suspend/resume, see B2), and some chargers disable D+/D- while a PD contract exists, so a Legacy row can skip as "blocked by PD" or fail. Write each into docs/CHARGER_QUIRKS.md as a signature, not a VID.
   Pass: the run reaches Done with no reboot, VBUS minimum above 4.0 V, every unit boundary as in step 2, the card header ends in "self-powered" and the Cards page stats line says "self-powered". Fail: any reboot, a `Detach` state, VBUS dark between units, or a Legacy unit skipped for any reason other than a charger behaviour that is also recorded in CHARGER_QUIRKS.md.
   B2. VBUS lost anyway: with a run going on the same setup, unplug the brick for 2 s and replug it (the pin goes with it, so this is a real power loss: hold PLUS at the replug). Pass: the page opens "Resume run?" with the status "Self-powered, paused" and resumes by itself once VBUS has been steady 0.5 s (no OK press); `k2_remote charger-test status` never reads "stopped (source lost)" for this run, only "resumed N" with N > 0 on the card; steps already done keep their verdicts, the interrupted one is asked again. If the loss does NOT reboot the K2 (a Hard Reset dip with the pin kept), the page shows the same prompt in place and resumes at 0.5 s of steady VBUS with no reboot. Fail: "stopped (source lost)", 35 steps skipped "source lost", or a resume that needs an OK press.
C. Result: read the card over HID. Pass: all steps present, same verdicts as an uninterrupted run, footer "resumed N" with N > 0 after real power losses. A unit that took the K2 down 3 times shows its remaining rows "skipped, rebooted 3x" and the run goes on with the next unit.
D. Cancel: after a reboot mid-run, hold BACK while plugging in: log `[TEST] journal: ... cancelled (BACK held)`, no resume, the next boot shows nothing to resume.
E. Cable identity on a PLUS boot (host-tested only: `emkra::check` accepts a pin of CC2 alone, refuses CC1 or both): charger on the top plug directly, hold PLUS at plug-in, pick a built-in cable on the Cable step, START. Expect no Ra-guard refusal (`[GUARD] ... passive 0 -> unknown`) and no warning screen at all (a K2 that boots from the charger cannot present the Ra before the charger's attach; the START just goes on, and against a source whose offer is held at 3 A below its claim (pattern `cable-limited-offer`, no SVID needed) it plans the re-attach of B0d, announced by the confirmation footer "Press again, K2 blinks once" only when the claim is already in hand at START; otherwise the claim is awaited in the run's arm step and the blink follows without the announcement): `[EMKRA] on: PC13 high (Ra on CC1), Rd on CC2 only` (never `refused: contract`: the self-hold is not a contract), and VBUS stays up through the START (the pinned Rd on CC2 is not dropped by the mode). Pass: the run starts, the card header names the cable and records the bypass; a charger that reads the cable at attach (CUKTECH 120 W) still offers only its unmarked-cable caps, which is the documented limit, not a failure. HID-powered check of the same brick (no load): enable HID, pick "CUKTECH 6A" at the Cable step, `k2_remote pdos` shows the PPS 6 A caps. Fail: `[EMKRA] refused: pinned-rd`, or VBUS drops when the Ra goes on. A reboot mid-run resumes with the same identity re-armed (`[TEST] cable identity: ... (Ra mode)` right after the resume).

F. Warm reset keeps the pin (the pin mark, DR21): after part A with the charger as the only supply, force a warm reset without a key (a charger-side glitch that resets the MCU, or the watchdog; `k2_remote reboot` needs HID, so only with a HID cable whose 5 V is cut). Pass: the next boot log shows `[BOOT] cc write trace: ... Rd pin 0x.. inherited 1` and VBUS never drops. If nothing resumes on it, `self_power_release` clears the mark and the following warm reset is passive. T24 is the guard for exactly this.
G. Bus-powered, no key (the 2026-11-06 run, built on the host only): the CUKTECH USB-A port with an A-to-C cable on the BOTTOM port, HID unplugged, plain boot (no PLUS/OK). The A port cuts VBUS about every 6.5 s with no load and stays on while the K2 draws. Pass: after ~2 s the log (read over HID afterwards, or `python3 tools/pd_trace.py dump`) shows `[PWR] supply: bus-powered (no host, VBUS at boot)`; Review shows the self-powered note; START with "Legacy only": no Detach / Attach, no PD row in the plan, a hold at the unit boundaries, and the run reaches Done or goes on after every power loss with no click. Fail: the run stops, a Resume screen that waits for OK, or a PD row in the Legacy plan. Plug HID before the run (a USB host configured makes the K2 HID-powered, `[PWR] supply: host or none`) only for the control run.
H. Reboot budget: on a charger whose unit repeatedly takes the K2 down (the UFCS unit on the A port above), the card shows that unit's rows "skipped, rebooted 3x" after its 3rd power loss and the next unit runs; the reason is read on the card (there is no log line for it). Fail: more than 3 resumes charged to one unit, or a run that stops instead of going on.

### T24 HID-powered K2: the boot must stay passive (regression check for the self-power gate)

Purpose: the self-power work (T23) pins Rd and keeps it across warm resets. A K2 powered over HID (left port, C-to-A cable) must never do that: its VBUS does not hang on its own Rd, and the brick on the top plug has to see a real detach on every reset. Hardware 2026-11-06 found three regressions in the first merge (Rd on both lines after a flash, Rd kept after `pd-reread --abort`, Rd inherited over `reboot`). Host tests now cover all three (`tests/test_fusb302_scan.cpp` `test_hid_*`, `tests/test_bootmode.cpp`). Run this on the device before any merge that touches `bootmode`, `self_power_*` or the CC switches. Not yet run on the gated build.

Setup: K2 on HID power only, USB HID enabled in Tools, charger brick (OnePlus 120 W) on the TOP plug, no load. One device tool at a time, with timeouts. Flash the committed build with `k2_flasher flash build/open_k2.k2 --auto-dfu`; `k2_remote ping` must show the hash without `-dirty`.

1. Right after the flash (a warm reset; the chip still holds what the old image left): read the CC levels (Lines page screenshot or `k2_remote state`). Pass: cc1 and cc2 both near 3.19 V and VBUS 0 V (passive, charger waiting for a sink). Fail: both near 1.6 V (Rd on both lines), or VBUS 5 V nobody asked for.
2. `k2_remote pd-reread --wait 20`: VBUS goes to 5 V and Rd shows on the attached line (expected while it runs). Then `k2_remote pd-reread --abort`. Pass: within 2 s Rd is off (cc back near 3.19 V, VBUS 0 V). Fail: Rd stays on CC2 or VBUS stays at 5.0 V.
3. Start `pd-reread --wait 20` again and, while Rd is on, `k2_remote reboot` (no abort). After the reboot (compare `ping` uptime with the wall clock) check cc1 and cc2. Pass: near 3.19 V on both, VBUS 0 V: a warm reset of a HID-powered K2 is a detach. Fail: Rd inherited, VBUS 5.27 V.
4. Log: the boot shows `[BOOT] mode=plain` and `Rd pin 0x0 inherited 0`.
5. Cold start: unplug HID and the brick, plug HID only, then the brick. Pass: the same passive state as step 1.
6. Then T23 part A once (PLUS held at plug-in, charger on the top plug, HID unplugged): VBUS must hold after PLUS is released. T24 pass with T23 fail means the gate is right and the pin routing still needs work; T24 fail is a regression of the gate.

Never run `k2_remote fault`, never drive CC or D+/D- by hand, and stop and investigate any reboot that is not planned.

### T22 Charger report card (save and delta)

Purpose: every finished charger-test run is saved to the littlefs volume (/cards/<fingerprint>.card, newest card per charger, at most 64 chargers; see docs/ARCHITECTURE.md). Built on the host only; the Tools page, remote commands and MSC file are not implemented yet.

Setup: a PD charger (CJ21), USB HID on, log via `k2_remote.py`.

1. Run a full charger test to Done. Log shows `[CARD] save #1 fp=XXXXXXXX runs=1 -> 0`; the Results footer reads "Card #1 saved; OK opens".
2. Run the same charger again: `runs=2`, same `fp`, same card number; no change row if nothing differs.
3. Change something (another cable, or "Try QC after PD") and run again: a row "vs last card: N changed" appears under "Edit setup"; OK opens "Since last card" with lines such as "PPS 13V: was FAIL, now PASS". BACK/OK returns.
4. Power-cycle the K2 and repeat step 2: the card survives, `runs` keeps counting.
5. A different charger gets card #2. Unplug during the save is covered by the host test (power cut at every 7th half-word).

Pass: card saved on every Done run, delta lines match the earlier run, nothing saved for Stopped runs or chargers without PD source caps.

### T25 Record everything while running a charger test (the debug stream)

Purpose: one recorded log of a run (run events with verdicts and reasons, K2LOG lines, every PD message, VBUS/IBUS/CC/D+/D- samples, heartbeat) that you read afterwards, instead of polling rings that wrap. Built on the host and in the firmware; hardware-verification pending (`docs/DEBUG_STREAMING.md`).

Setup: the charger test setup of T20, USB HID enabled in Tools and the K2 on the left port with a C-to-A cable (the debug shell, a serial port, exists in HID and HID+Drive modes). The recorder uses the CDC port and takes no device lock, so it runs in parallel with `ct_start.py` and `k2_remote.py` (HID). Output goes under `runs/` (gitignored). Never run a second reader on the CDC port (`sniff stream`, `trace stream`, `picocom`) at the same time: they share a 16 B endpoint.

1. Flash the committed build (`k2_flasher flash ... --auto-dfu`), `k2_remote ping` shows the hash without `-dirty` (the stream names the hash; a dirty build decodes log ids with the working tree and says so).
2. Terminal 1: `python3 tools/k2_stream.py record --out runs/ct-$(date +%F-%H%M) --until-run-end`. It prints the Hello (`START stream start fw=<hash> kinds=log,trace,power,run power=20Hz`) and then every run event and log line.
3. Terminal 2: `python3 tools/ct_start.py --wait 560` (or any `--units ...`). Compare `ping` uptime with the run duration afterwards: it must not reboot.
4. When the run ends the recorder stops 3 s later (`--settle`), sends `stream off` and prints the completeness summary. Pass: `COMPLETE: every device counter matches`, no `GAP` row that is not explained.
5. Read: `python3 tools/k2_stream.py view runs/<dir> --kind run` (rows and verdicts), `--grep 'row 7'` (one row across every kind), `--kind gap` (any loss), `--summary`. The folder also holds `trace.raw` for `pd_trace.py decode` and `meta.json`.

A busier recording (everything at 50 Hz, history from the oldest ring entry): `--kinds all,power=50 --old`. A quiet recording for a long soak: `--kinds run,power=5`.

Saturation: when the CDC pipe (about 16 KB/s) cannot carry everything, power samples fold into min/max windows first, trace and log fall behind in their rings, and a ring that wraps shows up as a `GAP ... DEVICE LOST` row with the count. A row that reads `INCOMPLETE` is a finding to report with the folder, not a retry reason.

### T26 QR scan-test rig (qrtest)

Purpose: check what a phone camera can actually read from this 240x240 panel, per QR version and per pixels-per-module, with the quiet zone as a parameter. The rig draws ONE full-screen test QR (the whole panel, button strip included, no caption) filled with the longest kind-'t' link that version carries, so a scan both proves readability and shows the label "QR scan test v<NN> <N>px" on the results page. It only draws: no VBUS, CC, D+/D- or load is touched, no confirm is needed; it is refused (busy) while a charger test runs.

Commands (USB HID, `tools/k2_remote.py`):

- `qrtest list` - the fit table from the device, computed there from the QR capacity table and the panel geometry (box = (modules + 2*quiet) * px <= 240). One text line per row, a header first: `# v m p q box fit c r`, then per (px, quiet) combo (px 3 then 2, quiet 4..1) the largest version that fits and the first that does not, e.g. `v13 m69 p3 q4 box231 fit c425 r281` (version, modules, px, quiet, box px, fits, byte-L chars, max record B) and `v14 m73 p3 q4 box243 no c458 r306` for the refusal. Chunked 48 bytes per reply; the tool reassembles and prints it.
- `qrtest <version> <px> [quiet]` - show the test screen (quiet defaults to 4). version 1..40, px 2..3, quiet 1..4; a box that does not fit the panel is refused with the reason, e.g. `qrtest 25 2 2` answers bad argument (box 242 px over the 240 px panel).
- `qrtest stop` - leave the test screen remotely (BACK on the device pops it too).

Owner scan matrix (geometry corrected: v25 at 2 px with a 2-module quiet zone needs 242 px and does NOT fit; the true maxima are v24 at 2 px quiet 2, box 234, and v25 only at quiet 1, box 238; full-screen v13 at 3 px quiet 4 needs the caption hidden, box 231):

| # | command | box | what to check |
|---|---|---|---|
| 1 | `qrtest 11 3 4` | 207 px | today's share geometry (v11 in the 224 area is 3 px) - the baseline a phone already reads |
| 2 | `qrtest 12 3 4` | 219 px | full-screen v12, quiet 4, no caption |
| 3 | `qrtest 13 3 4` | 231 px | full-screen v13, quiet 4, no caption (425-char link) |
| 4 | `qrtest 14 3 2` | 231 px | v14 at 3 px with a 2-module quiet zone |
| 5 | `qrtest 21 2 2` | 210 px | v21 at 2 px (the share area holds v21 at 2 px quiet 2 as well) |
| 6 | `qrtest 23 2 2` | 226 px | v23 full-screen at 2 px quiet 2 |
| 7 | `qrtest 24 2 2` | 234 px | v24 full-screen at 2 px quiet 2 - the largest at quiet 2 |
| 8 | `qrtest 25 2 1` | 238 px | v25 only at quiet 1 (`qrtest 25 2 2` is refused, box 242) |

For each row: run the command, scan with the phone, note whether it reads, what the results page labels it ("QR scan test v<NN> <N>px"), and from how far away it still reads; then BACK (or `qrtest stop`) to leave. Record one row per scan in section 3.

### T20 hardware plan summary

Handover (T20) and journal (T21) were built on the host only; nothing here is hardware-verified yet. First hardware run: T20 with the CJ21 (confirm the 300 ms CC detach makes the charger drop PD and offer QC3/AFC/FCP; measure the 20 s timeout), then T21 steps 1 to 5.

## 3. Results log

Copy one row per run. Evidence names follow `$EV/<test-id>-*` (section 1.6).

| Date | Firmware hash | Charger model | Test | Result (pass / fail / partial) | Evidence file |
|---|---|---|---|---|---|
| 2026-11-11 | host only | none | PPS / AVS down-step descent window (not yet hardware-verified) | The fixed 3 s `kDescentMs` window of the 2026-11-02 row is now the minimum: for PPS / AVS down-steps `descent_window_ms(drop_mv, programmable)` grows 2 s per volt dropped (`kDescentPerVoltMs`) up to 10 s (`kDescentMaxMs`); fixed-voltage steps keep 3 s, and the release verify keeps its 4.5 s limit. Host tests in `tests/test_pd_sink.cpp` and `tests/test_charger_test.cpp`. Needs a hardware re-run of the direct 20-to-5 V and 21-to-13 V PPS steps. | |
| 2026-11-02 | host only | none | PPS step-down guard change (not yet hardware-verified) | The +10% guard in `Sink::on_vbus` (`src/pd/pd_sink.cpp`) fired because VBUS was still falling after `kSettleMs`. It now waits while samples keep falling (>= 50 mV per 600 ms) for up to 3 s more (`kDescentMs`), and aborts on a rise > 150 mV, a stall, or the window ending. The `cap_mv_` ceiling, cable limit, inline guards and release checks are unchanged. Host tests in `tests/test_pd_sink.cpp`. Needs a re-run of the direct 11-to-8 V and 21-to-13 V step-downs on hardware. | |
| 2026-11-02 | `71ed6b5` | user-identified 120 W OnePlus, no load | PPS 21-to-13 V in 1 V steps | pass: 21 V and each 1 V decrement through 13 V remained within +/-5% for one second; cleanup reached 5.025 V. This does not pass the direct step-down or verify rated power. | recording not kept in the repository |
| 2026-11-02 | `71ed6b5` | user-identified 120 W OnePlus, no load | Direct 21-to-13 V PPS retarget | fail: 21 V settled at 21.020 V; the sink reported ready for 13 V while measured VBUS descended from 21 V, and the guard aborted at a sampled 15.436 V; cleanup reached 5.025 V. This is not a rated-power test. | recording not kept in the repository |
| 2026-11-02 | `71ed6b5` | user-identified 120 W OnePlus, no load | Grouped charger suite | partial: 8 pass, 1 fail, 32 skip; 5/9/12/15/20 V fixed and 5/13/21 V PPS passed; 21-to-13 V PPS failed the existing guard, QC skipped on PD traffic; final VBUS 5.025 V | recording not kept in the repository |
| 2026-11-02 | `2aeff99` | 67 W PD/PPS charger, no load | Grouped PPS, 21 V endpoint | partial: 21 V passed at 20.945 V with a 1 A request and 30 s hold; direct 11-to-8 V and 21-to-13 V step-downs failed the +10% guard; 6 pass, 2 fail, 4 skip overall | recording not kept in the repository |
| 2026-11-02 | `4d396cd` | 67 W PD/PPS charger, no load | Grouped PD fixed + PPS + QC2/3 | partial: 11 pass, 2 fail, 15 skip; fixed 5/9/12/15/20 V passed; 11-to-8 V and 20-to-12.5 V PPS step-downs tripped the +10% guard 1.5 s after PS_RDY; 21 V and QC skipped | recording not kept in the repository |
| 2026-11-02 | `4d396cd` | same | Supplemental PPS step-down | pass: 1 V down-steps 11-to-5 V and 20-to-13 V plus 13-to-12.5 V, with samples in the +/-5% band for 5.5 s per target; each range returned to 5 V | recording not kept in the repository |
| 2026-11-01 | not recorded | lone PD charger | T01 PD fixed 9 V | pass: PS_RDY, VBUS 9.34 V | (commit messages) |
| 2026-11-01 | not recorded | lone PD charger (PPS) | T02 PPS 9 V | pass | (commit messages) |
| 2026-11-01 | not recorded | Pixel on top | T14 Identify | pass: VID 18D1 PID 4EE1 | (commit messages) |
| 2026-11-01 | not recorded | none | T18 USB network + dashboard | pass | |
| 2026-11-01 | not recorded | none | T15 e-marker read + save | pass | |
| 2026-11-01 | not recorded | lone PD charger | T17 Black Box capture + persist | pass | |
| 2026-11-01 | not recorded | lone PD charger + load | inline loop fix | pass | |
| | | | | | |
