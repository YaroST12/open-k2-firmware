# Testing

`make gate` runs everything: the firmware build with `-Werror`, the layout invariants, the map and command-id checks,
the host tests (`make test`: C++ under `tests/test_*.cpp`, Python `tests/test_*.py`, node `tests/*.mjs`) and the
size check against `tools/size_baseline.txt`. Rules for writing host tests are in
[CODING_STANDARDS.md](CODING_STANDARDS.md); this file covers the world replay.

## World replay

`tests/test_world_replay.cpp` runs the real main loop (`src/core/main_loop.cpp`), the real FUSB302 and
power-monitor drivers and the whole UI against a modelled world (`tests/world_sim.{h,cpp}`) on a virtual clock. A
repaint costs the time it costs on the K2, so a frame that arrives during a long Home repaint is lost here as it
was on the bench. Structure: [ARCHITECTURE.md](ARCHITECTURE.md) "World replay (host)".

It has two modes:

- **Replay.** Every episode of `tests/scenarios/*.scn` (one VBUS rise with PD traffic) is played on Home and on
  Detail, each at ten loop phases (the world starts 0, 53, ..., 477 ms into the firmware's loop), and each settles
  for 2.5 s first. A run passes when the episode's expectations hold: the first SOP message decoded, the number of
  Source_Capabilities decoded, and the `[LINK]` verdict token.
- **Reboot.** The K2 lives on a modelled charger's VBUS. It dies below brown-out, boots cold through the real boot
  path, and the charger serves VBUS only to a valid Rd (see "Reboot world" below).

### Running it

```
make host-build/test_world_replay && ./host-build/test_world_replay        # also part of make test / make gate
K2_WORLD_ONLY=late-attach-cycles:rise420.8:Home ./host-build/test_world_replay   # pick runs (scenario[:episode[:page[:phase]]])
K2_WORLD_ONLY=hr-session:bpr-lost K2_WORLD_LOG=1 ./host-build/test_world_replay # a reboot case with its firmware log
```

- The report goes to `runs/world-replay/report.txt`. It has one line per run, plus the firmware log and the fate of
  every frame for runs picked with `K2_WORLD_LOG=1`.
- A replay line shows:
  - `first=` the first SOP message heard, with its time from the rise;
  - `attach=` when the driver attached;
  - `lost_unrouted` / `lost_other`: frames that never reached the RX FIFO;
  - `longest_pass=` the longest main-loop pass and when it ran;
  - `caps=`, and `verdict=` / `verdict_caps=` from the `[LINK]` line.
- `K2_WORLD_PX_CPU_US=<us>` overrides the calibrated rendering cost, to see how much margin an episode has.
- `K2_WORLD_SCENARIOS=<dir>` reads scenarios from another directory.

### Making a scenario (one command)

Record the bench session with `tools/k2_stream.py`. That gives a directory with `trace.jsonl`, `power.jsonl` and
`meta.json`. Then:

```
python3 tools/trace2scenario.py runs/<date>/<recording> -o tests/scenarios/<name>.scn [--template runs/<date>/<other>]
```

The scenario is the world, not what the K2 did:

- VBUS over time;
- the CC level the measure block saw on each line;
- the ADC levels of CC1/CC2/D+/D-;
- every PD frame on the wire, with its line and raw bytes.

The rules, from the tool's docstring, are conservative: they never make the world kinder to the K2 than the
recording shows.

- If the K2 attached late and missed the first frames of an attach, pass `--template`: a recording of the same
  charger and sink where it heard them. The tool fills the gap from the template, aligned on the first identical
  raw frame, and marks those frames `tpl`.
- Each episode gets an `expect` line (`first_sop=Source_Capabilities caps>=1 verdict=present`). Edit it by hand to
  what the K2 should have seen.

The test picks the new file up with no code change.

`tools/test_trace2scenario.py` covers the tool on synthetic recordings.

### Timing model

| Cost | Value | Source |
| :--- | :--- | :--- |
| FUSB302 I2C byte | 43 µs | trace: a 1-byte register write (3 bytes) takes ~130 µs on the bit-banged bus |
| INA I2C byte | 50 µs | the slower power-monitor bus (~200 kHz) |
| `delay_us(n)` | n × 2.79 | trace: the scan's `delay_ms(2)` + read + write = 5.85 ms |
| `millis()` / `micros()` read | 0.2 µs | keeps a polling loop moving |
| one main-loop pass | 15 µs | the work not modelled call by call |
| LCD wire, per pixel | 1.0667 µs | SPI at /8 (`lcd_timing.hpp` `wire_us`) |
| LCD per stream call / per window | 2 µs / 8 µs | CASET/RASET/RAMWR overhead |
| LCD CPU, per streamed pixel | 1.65 µs | calibrated, below |
| INA226 bus register | mean over the last conversion period | 4.48 ms at the driver's 0x4407, 280 µs in its fast mode |

The real LCD idle hook runs inside a timed paint, as `lcd.cpp` calls it: every 468 pixels inside a DMA wait, and
every 2048 pixels pushed by hand. So the PD service during a repaint is the firmware's own.

**Calibration.**

- The CPU share per pixel was fitted to the Home repaint at the VBUS rise of the 421.4 s attach in
  `runs/2026-11-11-late-attach/cycles`.
- The model gives 151.1 ms.
- The recording bounds that repaint to between about 132 ms and 152.5 ms. The bounds come from the frames the K2 did
  and did not hear around it.
- Residual: −1.4 ms against the upper bound, inside the bracket.
- The bracket is ~20 ms wide, so an episode that passes by less than that is marginal. `K2_WORLD_PX_CPU_US` shows
  the margin.

### Acceptance (how the replay was checked)

| Firmware | Expected | Result |
| :--- | :--- | :--- |
| `6cbb12bf^` (before the late-attach fix) | h-side-inline fails | fails (17 of 20 runs); 114 of 180 replay runs fail |
| `6cbb12bf` | the 421.4 s attach fails on Home (PS_RDY is the first SOP heard, as recorded) | fails on Home only; 27 of 180 replay runs fail, all on Home |
| `77a384e8` and later | all pass | 180 / 180, plus the 4 reboot cases |

The older firmware was checked by reverting the fix in the driver temporarily (`git apply -R` of the commits' driver
hunks, with `fusb302::scan_service` left as a no-op so `main_loop` still links), never in a commit.

### Sink and QC

Not modelled yet. The D+/D- side is stubbed: `fastcharge`, `dpsniff` and `fctrig` are no-ops in the world build. The
tool writes only `mode inline` scenarios. In the reboot world the K2 is a sink, so it is the first sink coverage: it
requests PDO 1 and gets Accept/PS_RDY, at 5 V only.

A sound sink/QC replay needs three things:

- a source model that answers the K2's Requests;
- a D+/D- network model (the QC/AFC/FCP handshakes);
- VBUS that follows the contract.

A recording alone cannot replay a sink, because the K2's own messages change what the source does next.

### Reboot world

`world::reboot_start()` and `RebootScenario` in `tests/world_sim.h`, with the cases in `reboot_cases()` in the test.

- **Lives.** Each firmware life is a fork of the run process. The life ends (`_exit(3)`) when the K2's rail falls
  below brown-out. The run process then runs the dead world until the rail can boot the next life, which starts
  from the same pristine image, as after a power-on reset.
- **What survives** is kept in shared memory:
  - the flash volume (the `/rd.rec` marker and the stores);
  - the backup registers (BPR): the session record in DR22..DR37 and the pin mark in DR21, while the dark is no
    longer than `PowerModel::bpr_hold_ms`; a longer dark zeros them. A host cannot reach BPR at 0x40006C00, so the
    world provides `session::init` / `backup_alive` over the same record (`session::encode` / `decode`) and
    `pinmark::read` / `write` over its own copy;
  - the FUSB302 registers, while its supply held (power-on defaults after);
  - the world itself (clock, charger, VBUS, the log, tagged with the life number).
- **What does not survive.** SRAM. `.noinit` holds only the 128 B diagnostic probe (`rdcomeback::kProbeWords`), and no
  SRAM survives a VBUS dark on this board (0 of 32 words after 1 s). Each life after a dark gets a fixed garbage
  pattern in `.noinit`, so `boot_look().sram_words` reads 0. `PowerModel::sram_kept = true` copies the bytes across
  instead (a switch for what-if runs; no case sets it).
- **The voucher.** The Rd marker (`/rd.rec`) is only a comeback's when `session::backup_alive()` is true: a valid
  session record in the backup registers, which every boot rewrites. So a short dark (the Hard Reset's) keeps the
  record and the marker is believed; a long one (an unplug and a wait) loses it and the marker is judged stale.
- **Boot path.** Each life boots through `bootmode::decide`, `bootpd::early`, `adc` / `power` / `fs` init,
  `bootpd::late`, `session::init`, `fastcharge::init`, the UI and `mainloop::begin`, in `main.cpp`'s order. Right after
  `early()` the test writes the marker decision (`rdcomeback::boot_look()`) into the world's log as a
  `[TEST] boot_look ...` line, so a boot the rail does not outlive still reports what it decided.

Model parameters. All are inferred; the hardware row says where a number comes from.

| Parameter | Value | Note |
| :--- | :--- | :--- |
| 3.3 V rail | min(max(VBUS, aux) − 300 mV, 3300) | aux = the HID port's VBUS through a C-to-C loop |
| MCU brown-out / cold boot | 2700 mV | |
| FUSB302 keeps its registers | ≥ 2000 mV | below: lost, power-on defaults on the way up |
| reset to `boot()` | 1 ms | |
| ADC first readings | 15 ms after the life's first `adc::init` | the first DMA ring |
| Backup registers across a dark | kept up to `bpr_hold_ms` = 5000 ms (the dark is counted from the brown-out to the next reset), zeros beyond; `hr-session` variants set 0 to lose them | hardware: kept over a 1 s dark, lost after a minutes-long unplug; the actual limit is unmeasured |
| SRAM (`.noinit`) across a dark | lost (`sram_kept` switch, off) | hardware: 0 of 32 probe words after 1 s (`runs/2026-11-11-iq4-bridge`) |
| Rd on the charger's line | the chip's PDWN bits (switch closed only while a life runs), the PLUS key on CC2 while held (~5.2 kΩ, CONNECTION_MATRIX), the HID port's Rd (loop variants) | |
| charger tCCDebounce | 150 ms Rd → VBUS on | |
| charger tPDDebounce | 15 ms without Rd → detach, VBUS off (counted from vSafe5V) | |
| VBUS slew | +250 mV/ms on, −20 mV/ms off (the K2 as load) | CHARGER_QUIRKS: 0 V ~300 ms after a cut |
| caps | first 100 ms after vSafe5V, every 150 ms, 50 times | |
| tSenderResponse | 27 ms, else Hard Reset | |
| Accept / PS_RDY | 6 ms / 90 ms after the Request | `runs/selfpwr-drop/e8`: 5.9 ms, 90.1 ms |
| Hard Reset | VBUS off 800 ms, back without a CC check | |
| K2 TX | TXSENT 1.2 ms after TX_START if the charger hears it, else RETRYFAIL at 4 ms | |

Not modelled:

- flash write time;
- protocol retries (one copy per message);
- GoodCRC in the RX FIFO;
- contracts above 5 V;
- the e-marker look of the charger's cable, and Ra and spoof timing.

`usb::configured()` is false, since no host is attached.

The scenario `hr-session`:

- PLUS is held from power-up to 1.5 s, a key-mode boot with a self-powered session.
- The charger sends a Hard Reset at 6 s, and the run ends at 10 s.
- The Hard Reset's dark is about 713 ms (brown-out to the next reset).
- It runs in four variants: backup registers kept or lost over that dark, each with and without the HID loop.

Each case asserts:

- the number of lives;
- whether the K2 is alive at the end;
- contracts made;
- the longest Rd gap against the charger's tPDDebounce (when alive);
- the log lines that must and must not appear.

| Variant | Outcome |
| :--- | :--- |
| `bpr-kept` | the record vouches (`canary=1`, `host=0`): life 2 is a comeback boot (`session pin=2`), contract again; Rd gap 9.2 ms of 15 |
| `bpr-lost` | `[RDI] record ... ignored: the backup registers did not survive this boot`, `canary=0`: the plain boot drops Rd, the charger detaches after 15 ms, the K2 stays dark (the rail is gone before `session::init`, so no `[SESS]` line) |
| `bpr-kept-hid-loop` | `canary=1`, `host=1`: the aux rail makes life 2 a host-powered boot: passive, alive on the loop's Rd, no new contract |
| `bpr-lost-hid-loop` | `canary=0`, `host=1`, `[SESS] no previous session record`: stale record and a host boot, passive either way, alive, no new contract |

Every case also asserts life 1: a key-mode boot never reads the marker (`looked=0`, no `[RDI] boot look` line) and the
backup domain is empty (`[SESS] no previous session record`). In all four, `sram_words=0`: SRAM decides nothing.

The 9.2 ms margin in `bpr-kept` depends on two inferred numbers: the 15 ms first ADC ring, and the gap counted
from vSafe5V.

Not modelled about the backup domain: its real retention limit between 1 s and minutes (so `bpr_hold_ms` = 5000 ms is a
choice, not a measurement), and the session record's content after the first boot (`session::poll` rewrites it as the PD
state changes; the world's record stays the boot's fresh one, which is all the voucher reads).

Not covered yet:

- an unplug/replug scenario;
- a planned re-attach with the E8 Ra and spoof timing;
- the charger-test journal resume across a reboot;
- the Rescan comeback;
- a page other than Home in the reboot cases.
