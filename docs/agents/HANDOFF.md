<!-- SPDX-License-Identifier: GPL-3.0-or-later
     Copyright (C) 2026 Yaraslau Furman -->
# WITRN K2 (AT32F403A) - Handoff

Entry point for a new contributor: how to build, test, flash and debug, the hardware facts the firmware relies
on, and the open issues. Feature status and what is verified on hardware: [`ROADMAP.md`](ROADMAP.md). Code layout,
flash map, USB modes, PD driver model: [`ARCHITECTURE.md`](ARCHITECTURE.md). Screen design:
[`UI_DESIGN.md`](UI_DESIGN.md). Rules: [`CODING_STANDARDS.md`](CODING_STANDARDS.md).

## Hardware summary

| Item | Value |
|---|---|
| MCU | Artery AT32F403AVGT7, Cortex-M4F, 1 MB flash. 8 MHz HEXT through the PLL to 240 MHz (HICK fallback), APB2 120 MHz, APB1 60 MHz (`src/drivers/crm.cpp`) |
| Pins | `include/k2/drivers/pins.hpp` (the single pin map) |
| PD controller | FUSB302 on the bit-banged I2C bus PB10/PB11, address 0x22 (`include/k2/drivers/fusb302.hpp`) |
| Power monitor | INA226 at 0x40 on the bit-banged bus PB8/PB9; INA228 and INA237 are auto-detected (`include/k2/drivers/power_monitor.hpp`) |
| Accelerometer | LIS2DH12 (id 0x33) or SC7A20 (id 0x11) at 0x18 on the PB8/PB9 bus |
| Display | ST7789V 240x240 on SPI3, CS PA15, 8-bit simplex TX, backlight PB7 as a plain GPIO |
| D+/D- | analog switch matrix PA7/PB0 (D+) and PB1/PB2 (D-); USART2 PA2 (TX to D-) and PA3 (RX from D+) for UFCS/VIVO |
| USB | full-speed device, packet memory at 0x40006000, HID logger VID 0x0716 / PID 0x5060; off by default (runtime setting) |
| Buttons | PA13/PA14/PA15-class lines are shared with the debug port, so SWJ is disabled and there is no debugger |

Flash layout: the bootloader owns `0x08000000..0x08003800`; the application is linked at `0x08003800`
(vector table), with the layout markers in `link.ld`. The writable regions and the read-only device storage are in
`include/k2/drivers/flash_map.hpp` and `docs/ARCHITECTURE.md` ("Flash map").

## First contact with a new K2 (read before the first flash)

The area `0x080E0000..0x08100000` holds data that was on the unit before this firmware: settings, e-marker
records, assets. This firmware never erases or programs it, except for legacy pages that carry this project's own
old signature. Before flashing anything onto a new unit:

1. Put the K2 in DFU (hold MINUS while plugging in USB), then `python3 tools/k2_flasher.py probe` must report a
   confirmed K2.
2. `python3 tools/k2_flasher.py backup --full <DIR outside the repo>`. Exit 0 = the whole 1 MB was read twice and
   compared. Exit 3 = the bootloader refused above some address (`manifest.json`: `last_readable_address`,
   `refused_at`): the dump is partial.
3. If partial: `python3 tools/k2_flasher.py flash build/open_k2.k2` (this firmware never writes that area, so it
   is safe before the dump), enable USB HID in Tools > USB link, replug (left port, C-to-A cable), then
   `python3 tools/k2_remote.py flash-dump <DIR outside the repo>` (writes the device-storage dump, two passes
   compared).
4. Keep the dump safe (copy it off the machine). Only then use the firmware normally.

Whether the bootloader serves reads above 0x080E3000 is unverified, hence step 3.

## How to work

1. **Set up a worktree**: `git worktree add ../open-k2-<topic> -b k2/<topic> main`, then copy
   `third_party/tinyusb` and `third_party/lwip` from a populated checkout (they are submodule gitlinks and arrive
   empty; see Open issues). `third_party/fonts` is tracked. Without them the build fails.
2. **Build**: `make docker-build` (needs the `open-k2-builder` image, `make docker-image` once). The host has no
   ARM toolchain.
3. **Gate**: `make gate` = build with -Werror, layout invariants, map check, command-id uniqueness, all tests,
   size against `tools/size_baseline.txt`. About 20 s with parallel make. Docs-only commits do not need it.
4. **Smoke**: `make smoke` = gate + flash + `tools/hw_smoke.py`, results in `build/smoke/<hash>/`. It needs the K2
   on the HID port (C-to-A cable) with USB enabled (Settings > USB Link > HID), because the USB link is **off by
   default**. The K2 is a shared device with one owner, the coordinator: nobody flashes it or changes VBUS
   unattended, and there are no ad-hoc flash loops.
5. **Flash by hand**: `python3 tools/k2_flasher.py flash build/open_k2.k2 --auto-dfu` (app running, no button). If
   the app does not answer, hold MINUS while plugging in USB. Log: `build/k2_flasher.log`.
6. **UI check**: the `k2-qa` agent drives `tools/k2_remote.py` (press, `page home|pd|diag|lines|tools`,
   screenshots) against `UI_DESIGN.md`. It never flashes and never triggers voltages.
7. **Debugging PD**: there is no debugger. Use the Black Box: `python3 tools/pd_trace.py live` (or `dump`,
   `captures`, `capture N`, `decode FILE`) shows every PD message, FUSB302 event and VBUS/IBUS sample with
   timestamps; trigger-frozen captures survive power-off. Add a `[TAG] ...` log line to make a hypothesis
   observable; `[BOOT] reset=` and `[SESS] previous session` tell why and where the last run ended.
   **Fast transitions** (a charger that drops VBUS right after an Accept): `python3 tools/pd_trace.py transition
   arm` (or let the charger test arm it), reproduce, `transition trigger` when it happens, then
   `transition dump --png out.png` (CSV with `--csv`; `transition list` / `dump --seq N` for the files the charger
   test kept; `transition decode FILE` for a raw dump). About 1 kHz, 1.92 s window.
   **Whole-run record** (log, PD records, power samples, charger-test run events, heartbeat, in one timeline, no
   device lock, runs next to `ct_start.py`): `python3 tools/k2_stream.py record --out runs/NAME --until-run-end`,
   read with `k2_stream.py view runs/NAME`; see `docs/DEBUG_STREAMING.md` (also the inventory of every source the
   device holds) and `HW_TEST_PROCEDURES.md` T25. Hardware test procedures (safety rules, setup, one section per
   protocol or feature, results log): `docs/HW_TEST_PROCEDURES.md`.
8. **Commits**: `<type>(<scope>): <summary>`, sections Summary / Rationale / Key Changes, explicit `git add`.

## Verified on hardware vs not

Verified: PD trigger (fixed) and PPS on a lone charger; Identify on a Pixel; e-marker read and save; Black Box
capture and flash persistence; the inline-load loop fix; the charger test on the author's chargers (per-charger
evidence in `CHARGER_QUIRKS.md`).

Not verified: Spoof acceptance by a charger; Pixel replug after Identify; PPS step-down; WebHID in Chrome; Kelvin
cable resistance (not implemented; the passive estimate is unchecked).

## Open issues, each with the next step

| Issue | State | Next step |
|---|---|---|
| Self-powered boot (PLUS held at plug-in) | `bootmode` presents Rd on CC2 (PLUS) or CC1 (OK) and shows a `SELF-PWR` chip. Hardware result (2026-11-01, charger alone on the bottom port): the K2 runs only while PLUS is held and dies on release. `SELF-PWR` only stays up when the K2 is plugged into a laptop's USB-C port (which supplies 5 V on its own). A key mode pins one Rd and moves it once; Rd on both lines is never presented (it is the Debug Accessory signature, and a OnePlus brick latched its Rp off). A warm reset keeps the pin only when the pin mark (backup register DR21, written by a pinned boot) says the K2 was pinned; the chip register is never read. HID-powered regression check: T24. A charger run survives power loss by resuming from the `/jr` journal (`mt.rec` = self-supplied flag, units, cable, seq; `at.rec` = per-unit loss count; a reboot always auto-resumes after 0.5 s of steady VBUS, the 3rd loss in one unit skips it as "rebooted 3x" and the run goes on; BACK at boot cancels; a K2 running from the measured port's VBUS with no key is "bus-powered" (`power_source`, `[PWR] supply:`) and runs like a self-powered one). Procedure: `HW_TEST_PROCEDURES.md` T23. | Run the boot power trace (`[BOOT] t=.. vbus=.. cc1=.. cc2=..`, `[SESS] last boot trace`) with PLUS held, with the HID cable attached through a C-to-A cable whose 5 V is cut (data only), so the trace is visible while the charger is the only supply. Open question: whether the charger ever sees our Rd on the bottom port and whether the K2's supply runs from that port's VBUS at all. |
| Spoof timing | Replies are sent from the 20 ms `poll()`; a source's tVDMSenderResponse is about 30 ms, and the FUSB302 cannot present Ra on the second CC (unknown whether `cc_vconn_gnd` can) | Move the SOP' reply into the FUSB302 interrupt path or poll at 1 ms during an armed spoof; capture a real charger's query with the Black Box to see whether it ever asks without Ra. |
| `third_party` not in git | `tinyusb` and `lwip` are gitlinks (mode 160000) whose contents are absent in a fresh worktree; the build fails until they are copied in | `git submodule update --init` needs the submodule URLs recorded in `.gitmodules` and network; either record them and pin the commits, or vendor both trees. Until then, copy from a populated checkout. |
| Pixel replug after Identify | not verified | Replug a Pixel after Identify with `pd_trace.py live` running and check that the link guard releases. |
| RAM footprint ideas (reviewed 2026-11-07, not taken) | The default build (K2_NET=0) has about 17.7 KB free above `.bss` and 11.5 KB of PSP spare; the K2_NET=1 build about 9.3 KB free (floor 8192) and 2.3 KB of PSP spare, so RAM only matters for the network build. Rejected: a union of the Tools menu pages inside `Ui` (loses per-page selection state, skips `leave()` of stacked menus); shrinking the Black Box ring, captures, the HID log ring, the HID TX queue, BinLog, ADC sweeps and ripple samples (all take from the diagnostic channels); the lwIP pool shrink (about 3 KB, unverified against HTTP throughput); serving the dashboard from littlefs instead of `.rodata`. | If the net build runs short of RAM: the lwIP pool shrink first (measure a dashboard load with `pd_trace.py` idle), then move `pdtrace` captures into the overlay. |

## Protocol and format references

[`FASTCHARGE_PROTOCOLS.md`](FASTCHARGE_PROTOCOLS.md) (D+/D- and UFCS: decisions and measurements),
[`K2_PORTS_SPEC.md`](K2_PORTS_SPEC.md), [`EMARKER_JSON.md`](EMARKER_JSON.md),
[`FEATURE_PROTOTYPES.md`](FEATURE_PROTOTYPES.md), [`CHARGER_QUIRKS.md`](CHARGER_QUIRKS.md). No analysis of any
other firmware application is included; see [`PROVENANCE.md`](../PROVENANCE.md).
