# Battery efficiency recorder

What flows into and out of a power bank over hours, phase by phase, from the power monitor: Wh and mAh in, Wh and mAh out,
the time, and for a round trip the efficiency Wh out / Wh in. It runs in the background on every page; the screen is the
Battery page of the ring (page 9).

| Part | Where |
| :--- | :--- |
| Pure logic: integrator, phase end rule, curve, record, totals | `include/k2/battery/battery.hpp`, `src/battery/battery.cpp`; host test `tests/test_battery.cpp` |
| Stored runs and the service (writes, SRAM checkpoint, boot) | `src/battery/battery_store.cpp` |
| K2 glue (volume, `.noinit` checkpoint, power monitor, `[BATT]` log lines) | `k2/battery/battery_hw.hpp`, `src/battery/battery_hw.cpp`; `main.cpp` (boot), `main_loop.cpp` (poll) |
| Remote protocol (CMD_BATTERY 0x58) | `k2/battery/battery_wire.hpp`, `src/battery/battery_wire.cpp`; host test `tests/test_remote_proto.cpp` |
| Screen (ring page 9 Battery) | `k2/ui/battery_view.hpp` (pure texts and rows), `k2/ui/ui_battery.hpp` (`BatteryPage`); host test `tests/test_ui_battery.cpp` |
| Host tools | `tools/k2_remote.py battery ...`, `tools/battery_rec.py` (record and status decoder), `tools/k2_qr.py` (BATT share) |

## Modes and wiring

| Mode | Phases |
| :--- | :--- |
| Charge | one charge phase |
| Discharge | one discharge phase |
| Round trip | a charge, a wait, then a discharge; the efficiency is Wh out / Wh in |

A charge counts energy into the bank ("in"), a discharge energy out of it ("out"), whichever port the bank is on. The wiring
only says which way the bank's outflow runs relative to its charge, which a round trip needs to see its discharge start:

| Wiring | Charge (bank takes energy) | Discharge (bank gives energy) |
| :--- | :--- | :--- |
| Same port | charger on one port, bank on the other | the bank stays put and feeds a load on the charger's port: the charge's opposite direction |
| Moved | charger on one port, bank on the other | the bank moves to the charger's port and feeds a load on the other: the charge's direction |

## Sign convention, integration and dead-band

- The samples keep the power monitor's sign: positive current = bottom port to top port (`k2/drivers/power_calc.hpp`,
  `direction` "up"). The record's curve keeps that sign; only the totals are mapped to the bank's in and out.
- The sign does not say which port the bank is on, so each phase takes the direction that carried more energy as its own:
  in for a charge, out for a discharge (the two columns swap, Wh and mAh together, when the other direction overtakes). A
  round trip's discharge waits for the outflow in the direction the charge and the wiring give (after a cold boot, the
  record's direction bit; a version 1 record's from the sign of its curve).
- Every main-loop pass takes the latest sample (`power::read_raw()`, the monitor's 10 mA dead band already applied) and
  integrates the step since the previous one: trapezoid over the real time (mW x ms = uJ, mA x ms = uC). A step whose
  current changes sign is split at the zero crossing, so the two directions never cancel.
- Dead-band: |I| under 20 mA (`kDeadbandMa`) counts as 0 (idle noise of an unloaded bank).
- A step longer than 5 s (`kGapMs`: a stalled loop, a reset) is not integrated; it is counted in the phase's `gaps`.
- A sample with the monitor offline integrates nothing; the next one primes.

## Phase end

- Automatic: once |I| has reached the end threshold (default 50 mA) and then stayed under it for the end time (default
  60 s), the phase ends; its time stops where the quiet began. Both are settings (Battery place rows `End below` 20 / 50 /
  100 / 200 / 500 mA, `End after` 10 s .. 10 min; the remote takes any value in 20..2000 mA and 10..3600 s). They apply to
  the running run at once and to the next one; they are RAM-only and come back to 50 mA / 60 s at boot.
- By hand: `End charge` / `End discharge` on the Battery place (OK, then OK again within 5 s), or `k2_remote.py battery end`.
- A round trip waits after its charge (nothing counted) until `Start discharge` (OK), or until the current flows out of the
  bank at the end threshold for 5 s after the swap was seen (same port: a quiet moment first; moved: VBUS under 1 V, the
  bank unplugged); those seconds count into the discharge. `Finish run` ends a round trip after its charge alone.

## The curve

60 points of 3 bytes (VBUS in 10 mV, 12 bits; the signed current in 4 mA, 12 bits), each the average of an interval that
starts at 10 s and doubles (pairs averaged) whenever the buffer fills, so any run length fits. Only measured time feeds
it, not the wait between two phases; `split` marks the discharge's first point.

## The record (250 bytes, little endian)

| Offset | Field |
| ---: | :--- |
| 0 | `'B' 'R'` |
| 2 | version (2; version 1, 246 bytes without the flag bits and the gap, still reads) |
| 3 | mode, wiring << 4 |
| 4 | state: 0 measuring, 1 between, 2 done; bit 7: the end rule is armed (measuring) |
| 5 | phases (bits 0..1), resumes << 2 |
| 6 | id u16 |
| 8, 10 | end threshold mA u16, end time s u16 |
| 12 | curve interval s u16 |
| 14, 15 | points, split (0xFF none) |
| 16, 40 | phase 0, phase 1 (24 bytes each): kind (bit 7: the bank's in is the reverse current), end (0 running, 1 auto, 2 manual, 3 lost), gaps u16, seconds u32, uWh in u32, uWh out u32, uAh in u32, uAh out u32 |
| 64 | curve points (60 x 3) |
| 244 | gap seconds u32: the time the resumes are known to have lost (a lower bound, see below) |
| 248 | CRC-16/CCITT of bytes 0..247 |

The same bytes are the flash file, the remote's `read` payload and the QR share (type `BATT`, schema 1).

## Storage and writes

- One file per run, `/bt/<id>.rec`, inline in the `/bt` directory's own metadata pair; the newest 3 are kept
  (`store::kKeep`), the oldest is pruned before a new file is written.
- Runs an older firmware kept in the root (`/bt<id>.rec`) move into `/bt` at boot, one rename each (littlefs renames
  across directories atomically; `Fs::mount` settles a rename a power cut interrupted, so a run is always in exactly one
  place, and the next boot moves the rest). The move deletes nothing; the next new run prunes to 3. A version 1 run that
  was measuring resumes when it holds energy (its direction from its curve, its end rule armed); one with none is closed
  Lost (an older firmware's start write).
- An empty run (under 1 mWh in and out: `0.000Wh` both ways) is not saved: no file, no kept run pruned for it. The footer
  says `Empty run, not saved`; `battery status` says `empty run, not saved` (status flag 16). Budget: 2 blocks, the `/bt` pair (`tests/test_volume_budget.cpp`, the volume
  table in `docs/ARCHITECTURE.md`).
- Writes happen at a phase boundary (a phase end, the discharge's start, the finish; not the start, so a run takes a
  slot only once it measured something) and every 10 s (`kSaveMs`) while a phase measures and holds energy, and only
  when the main loop allows an erase (no charger test or PD Stress run owns the ports, the PD link quiet for the erase
  window); a refused save goes out at the next allowed pass. A failed write is retried every 10 s; the footer says
  `Run not saved to flash`.
- Every write rewrites the run's file in place (`Fs::rewrite_file`: one metadata commit, no `.tmp` and rename; a power
  cut leaves the old record or the new one). Cost, measured in `tests/test_battery.cpp` (24 h of saves on the device's
  root: settings, the six store directories, `/log.txt`; two kept runs): 360 saves an hour, ~380 B programmed per save,
  72 erases an hour of the `/bt` pair (36 per 2 KB block), about 16 s of flash time an hour (~45 ms per save, more when
  the save compacts the pair).
- Wear levelling: the root (the superblock pair, blocks 0 and 1) is never erased by the saves. littlefs relocates the
  `/bt` pair every 500 erases (`block_cycles`, about 14 h of recording) onto free blocks: in 24 h 1728 erases over 5
  blocks, at most 501 on one, blocks in use unchanged (17 -> 17), no superblock expansion.
- A save waits while the main loop forbids an erase (a port owner, or PD traffic in the last second), so the flash copy
  can be older than 10 s at a cut. A run cut before its first save (the first 10 s, or under 1 mWh) is not on flash.
- A new run waits for the previous run's write: `start` refuses with Busy while that record is unwritten and a write is not
  allowed now.

## Reboot behaviour

| Event | What happens |
| :--- | :--- |
| Warm reset (a pin, software or watchdog reset with the board powered: the reboot chord, `k2_remote reboot`) during a run | The SRAM checkpoint (`.noinit`, magic + CRC-16, rewritten every second and at every state change) still holds the recorder; it is fresher than the flash, so it wins: the run goes on where it was; the reset gap is one counted gap, not integrated, and the end rule's quiet timer starts again. `[BATT] boot resumed` |
| Warm reset after a run ended but before its write | The checkpoint has the ended run: it is written at the first allowed moment. `[BATT] boot closed` |
| Cold boot (power cut, VBUS dark; SRAM does not survive one on this board) while a phase measured | The newest file says measuring: the same phase goes on from its last save (one resume and one gap counted; the end rule's quiet timer starts again, so a bank that stopped ends the phase `end_s` after the boot). Lost: the samples since the last save (up to 10 s) and the whole off time; the K2 keeps no clock while off, so the record's gap seconds only count the time from the boot to the first sample (a lower bound) and `battery read` says so. `[BATT] boot continued` |
| Cold boot while a phase measured, version 1 record (an older firmware) | No direction or end rule state to go on with: the phase is marked Lost and the run is closed. The screen says `Cut short`. `[BATT] boot lost` |
| Cold boot after an end or finish whose write had not gone out yet (a charger test or PD traffic held the flash) | The flash still says measuring: the run resumes. An end or finish is final only once written, normally the next main-loop pass. |
| Cold boot while a round trip waited between its phases | The run comes back between its phases; with moved wiring the swap counts as seen (moving the bank is usually what cut the power). `[BATT] boot waiting` |
| Cold boot after a finished run | Nothing to resume; the run is on flash. A finished or ended run never resumes. |
| Firmware update (DFU) | The app wipes SRAM before it enters the bootloader: a cold boot as above. |

## Screen (ring page 9 Battery)

- Bench row: `No run`, `Charging 1h05m, 12.34Wh`, `Charged 12.34Wh, waiting`, `Round trip 80.0%`, `Discharged 9.87Wh`,
  `Cut short, 12.34Wh`. A solid node while a run exists since boot.
- Place page: the identity block (headline `Charging` / `Discharging` / `Charged` / the efficiency / the Wh, a chip `1 of 2`,
  `Round trip` or `Cut short`, one line with the wiring, the time and the energy), then the rows: the facts (In, Out, Time,
  Efficiency) and what can be done now (Start charge / discharge / round trip, Wiring, End below, End after, Saved runs,
  Share as QR; while a run is in progress End charge or End discharge, Start discharge, Finish run). Saved runs lists the
  stored runs newest first; OK opens one (its identity, facts and Share as QR). BACK steps back out, then leaves the place.
- Running indicator: while a run measures or waits, a 5 x 5 accent dot sits in the content's top-right corner on every page
  but a QR code; when the run stops the page repaints whole once.

## Shell access

`k2_remote.py [--json] battery`:

| Command | Does |
| :--- | :--- |
| `status` | state, mode, wiring, phases, the last op's result, flags (write failed, unwritten, monitor, final efficiency, empty run not saved), id, settings, run and phase seconds, uWh / uAh in and out, efficiency, stored runs, the next run's wiring |
| `start charge\|discharge\|round-trip [--wiring same-port\|moved]` | starts a run (refusals: busy, no monitor) |
| `end`, `next`, `finish` | ends the phase, starts the discharge, ends the run |
| `config [--end-ma N] [--end-s N] [--wiring W]` | the end rule and the next run's wiring |
| `list` | stored run ids, newest first |
| `read [id] [--all]` | a record decoded (id 0 or none: the run in RAM), with the curve |

A refusal exits non-zero with the reason. The QR share decodes with `tools/k2_qr.py`.
