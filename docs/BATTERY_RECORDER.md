# Battery efficiency recorder

What flows into and out of a power bank over hours, phase by phase, from the power monitor: Wh and mAh in, Wh and mAh out,
the time, and for a round trip the efficiency Wh out / Wh in. It runs in the background on every page; the screen is the
Battery place on the Bench.

| Part | Where |
| :--- | :--- |
| Pure logic: integrator, phase end rule, curve, record, totals | `include/k2/battery/battery.hpp`, `src/battery/battery.cpp`; host test `tests/test_battery.cpp` |
| Stored runs and the service (writes, SRAM checkpoint, boot) | `src/battery/battery_store.cpp` |
| K2 glue (volume, `.noinit` checkpoint, power monitor, `[BATT]` log lines) | `k2/battery/battery_hw.hpp`, `src/battery/battery_hw.cpp`; `main.cpp` (boot), `main_loop.cpp` (poll) |
| Remote protocol (CMD_BATTERY 0x58) | `k2/battery/battery_wire.hpp`, `src/battery/battery_wire.cpp`; host test `tests/test_remote_proto.cpp` |
| Screen (Bench > Battery) | `k2/ui/battery_view.hpp` (pure texts and rows), `k2/ui/ui_battery.hpp` (`BatteryPage`); host test `tests/test_ui_battery.cpp` |
| Host tools | `tools/k2_remote.py battery ...`, `tools/battery_rec.py` (record and status decoder), `tools/k2_qr.py` (BATT share) |

## Modes and wiring

| Mode | Phases |
| :--- | :--- |
| Charge | one charge phase |
| Discharge | one discharge phase |
| Round trip | a charge, a wait, then a discharge; the efficiency is Wh out / Wh in |

The wiring says where the bank sits, and so which direction of the current is "in" and which is "out":

| Wiring | Charge (bank takes energy) | Discharge (bank gives energy) |
| :--- | :--- | :--- |
| Same port | charger on the input, bank on the output: forward = in | the bank stays on the output and feeds a load on the input: reverse = out |
| Moved | charger on the input, bank on the output: forward = in | the bank moves to the input and feeds a load on the output: forward = out |

## Sign convention, integration and dead-band

- The samples keep the power monitor's sign: positive current = forward = input port to output port
  (`k2/drivers/power_calc.hpp`). The record's curve keeps that sign; only the totals are mapped to the bank's in and out.
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

## The record (246 bytes, little endian)

| Offset | Field |
| ---: | :--- |
| 0 | `'B' 'R'` |
| 2 | version (1) |
| 3 | mode, wiring << 4 |
| 4 | state: 0 measuring, 1 between, 2 done |
| 5 | phases (bits 0..1), resumes << 2 |
| 6 | id u16 |
| 8, 10 | end threshold mA u16, end time s u16 |
| 12 | curve interval s u16 |
| 14, 15 | points, split (0xFF none) |
| 16, 40 | phase 0, phase 1 (24 bytes each): kind, end (0 running, 1 auto, 2 manual, 3 lost), gaps u16, seconds u32, uWh in u32, uWh out u32, uAh in u32, uAh out u32 |
| 64 | curve points (60 x 3) |
| 244 | CRC-16/CCITT of bytes 0..243 |

The same bytes are the flash file, the remote's `read` payload and the QR share (type `BATT`, schema 1).

## Storage and writes

- One file per run, `/bt<id>.rec`, inline in the root directory's metadata pair; the newest 3 are kept (`store::kKeep`), the
  oldest is pruned before a new file is written. Budget: 0 blocks (`tests/test_volume_budget.cpp`, the volume table in
  `docs/ARCHITECTURE.md`); a fourth run would split the root pair (4 blocks).
- Writes happen only at a phase boundary (the start, a phase end, the discharge's start, the finish), and only when the
  main loop allows an erase (no charger test or PD Stress run owns the ports, the PD link quiet for the erase window). A
  phase that measures never writes. A failed write is retried every 10 s; the footer says `Run not saved to flash`.
- A new run waits for the previous run's write: `start` refuses with Busy while that record is unwritten and a write is not
  allowed now.

## Reboot behaviour

| Event | What happens |
| :--- | :--- |
| Warm reset (a pin, software or watchdog reset with the board powered: the reboot chord, `k2_remote reboot`) during a run | The SRAM checkpoint (`.noinit`, magic + CRC-16, rewritten every second and at every state change) still holds the recorder: the run goes on where it was; the reset gap is one counted gap, not integrated. `[BATT] boot resumed` |
| Warm reset after a run ended but before its write | The checkpoint has the ended run: it is written at the first allowed moment. `[BATT] boot closed` |
| Cold boot (power cut, VBUS dark; SRAM does not survive one on this board) while a phase measured | The newest file says measuring: its phase is marked Lost and the run is closed (totals up to the last write, which was the phase's start). The screen says `Cut short`. `[BATT] boot lost` |
| Cold boot while a round trip waited between its phases | The run comes back between its phases; with moved wiring the swap counts as seen (moving the bank is usually what cut the power). `[BATT] boot waiting` |
| Cold boot after a finished run | Nothing to resume; the run is on flash. |
| Firmware update (DFU) | The app wipes SRAM before it enters the bootloader: a cold boot as above. |

## Screen (Bench > Battery)

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
| `status` | state, mode, wiring, phases, the last op's result, flags (write failed, unwritten, monitor, final efficiency), id, settings, run and phase seconds, uWh / uAh in and out, efficiency, stored runs, the next run's wiring |
| `start charge\|discharge\|round-trip [--wiring same-port\|moved]` | starts a run (refusals: busy, no monitor) |
| `end`, `next`, `finish` | ends the phase, starts the discharge, ends the run |
| `config [--end-ma N] [--end-s N] [--wiring W]` | the end rule and the next run's wiring |
| `list` | stored run ids, newest first |
| `read [id] [--all]` | a record decoded (id 0 or none: the run in RAM), with the curve |

A refusal exits non-zero with the reason. The QR share decodes with `tools/k2_qr.py`.
