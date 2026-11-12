# Open-K2 firmware architecture

Bare-metal C++23 for the WITRN K2 (Artery AT32F403A, Cortex-M4F, no OS, no heap). Everything under
`src/` is C++; only TinyUSB, lwIP and littlefs (under `third_party/`) are C. Rules are in
[CODING_STANDARDS.md](CODING_STANDARDS.md), feature status in [ROADMAP.md](ROADMAP.md), the document index in
[README.md](README.md). This file says how the code is organised and where things go.

Contents: 1 At a glance, 2 Boot, linker script and memory, 3 Layers, 4 Data flows, 5 Common helpers,
6 Rules of the road, 7 How to add a page / protocol / remote command / module, then the detailed
reference sections (module map, USB, PD driver model, Black Box, drive and flash store, report cards,
remote control, UI model).

Paths are relative to the repo root. Sources live in `src/<group>/x.cpp` with the header at
`include/k2/<group>/x.hpp`; the groups are `drivers`, `pd`, `fastcharge`, `chargertest`, `ui` (fonts in
`src/ui/fonts`), `core` and `net` (K2_NET=1 only). Only `at32f403a_407.h`, `lwipopts.h`, `tusb_config.h` and
`arch/cc.h` stay at the `include/` root, where the vendor map, lwIP and TinyUSB find them. Items marked *(unverified)* were not
checked against the code or hardware when this was written.

## 1. At a glance

```
  tools/ (host: Python, JS)        k2_remote.py  k2_flasher.py  pd_trace.py  hw_smoke.py  web/k2.html
        |  HID reports (EP1), CDC shell, NCM + HTTP (:80), MSC drive
  ------+---------------------------------------------------------------------------------------
  usb.cpp / usb_descriptors.cpp / msc.cpp / net.cpp / shell_hw.cpp / http_server.cpp   (transport)
        |
  remote.cpp + remote_proto.hpp (dispatcher)   capture.cpp (screenshot)   http_api.hpp   shell.hpp
        |
  ui.cpp + ui_*.cpp  (Ui, pages, menus)  <--  charger_test (runner) / report_card / charger_journal
        |
  PD engine: pd, pd_engine, pd_vdm, pd_ext, pd_sink, pd_identify, pd_probe, pd_source, pdtrace
  Fast charge: dpdm, qc, afc, accp, vooc, ufcs, vivo, uart2, fc_engine, fc_scan, fctrig, fastcharge
        |
  Drivers: fusb302, power_monitor (INA226/228/237), adc, lcd, i2c, accelerometer, load, flash, crm
        |
  Storage: fs (littlefs) over flash; emlib, report_card_store, bbstore/bbpersist, usbcfg, uiprefs
        |
  Hardware: AT32F403A, FUSB302 (CC/PD), INA226 (VBUS/IBUS), ST7789V, 4 buttons, D+/D- switch network
```

* One image for every USB link mode; the mode is a stored setting applied at boot (`k2/drivers/usbcfg.hpp`).
* One cooperative main loop (`src/core/main.cpp`), a 1 kHz SysTick, a few small ISRs. No RTOS, no threads.
* Pure logic (codecs, state machines, layout, formatters) sits in headers or hardware-free `.cpp` files and
  is compiled into host tests; drivers are thin and are verified on the device.
* The device has no debugger (SWJ pins are buttons and LCD lines): diagnosis runs on `K2LOG` lines, the
  Black Box and screenshots.

## 2. Boot, linker script and memory

### 2.1 Boot sequence

1. The bootloader (0x08000000..0x08003800) checks the layout (`K200` at 0x08004010, commit word
   at 0x08004400) and jumps through the vector table at 0x08003800.
2. `Reset_Handler` (`src/drivers/startup.cpp`), in this order: VTOR = 0x08003800, CPACR (CP10/CP11, FPU on),
   clear NVIC ICER/ICPR, copy `.data` from flash, zero `.bss` (this includes the `.mode_*` overlay
   region; `.noinit` sits below `_sbss` and is deliberately NOT zeroed), paint both stacks with 0xA5A5A5A5 (the watermark behind `stack_free` / `msp_free`), switch
   thread mode to the PSP (CONTROL.SPSEL, top `_psp_top`; the naked trampoline then calls `k2_reset_main`),
   run `.preinit_array` / `.init_array`, call `main`. `main` enables the MPU stack guard in `fault::init()`.
3. `main` (`src/core/main.cpp`), in this order: `sysreset::capture_flags` (reset cause), VTOR again,
   `sample_boot_mode` (buttons: PLUS / OK held at reset select the self-powered boot modes), `crm::init`,
   `systick::init`, `i2c::init`, optional `fusb302::self_power_early`, `boottrace::init`, `lcd::init`,
   buttons, `dpdm::init`, `load::init`, `fault::init`, `board::init`, `fs::init` (mount or format the
   littlefs volume), `usbcfg::init`, `usb::init` only when a link mode is enabled (USB is off by
   default), `msc::init` (drive), `shell_hw::init`, `http_server::init` (Net, K2_NET=1 builds), a 50 ms delay, the `[BOOT]` /
   `[RST]` lines, `fusb302::init`, `boottrace::report_previous`, `session::init`, `bbpersist::init`,
   `uiprefs::stored` + `fs::wipe_legacy`, `adc::log_raw`, `fastcharge::init`, `Ui::init`,
   accelerometer init, then the LCD idle hook and the loop. The PD half of this (the Rd decision of a reset, the
   self-powered comeback, the bring-up loop) is `bootpd::early(keys, warm)` right after `i2c::init` and
   `bootpd::late(early)` where `fusb302::init` runs (`src/core/boot_pd.cpp`), so the host world replay boots its
   firmware lives through the same calls. `lcd.cpp` calls `usb::service()` and the hook once 1 ms
   (`lcd::kServiceUs`) has passed since the last round ended, on elapsed time (`lcd::ServicePacer` over the cycle counter):
   it checks after every stream call's pixels, every 256 px of a polled fill and inside every DMA wait, and the cadence
   runs on across stream windows (a new window does not reset it), so a redraw never leaves the bus unserved for more
   than about 1 ms plus one round. A round is never nested. The hook runs at most once per
   ms: `fusb302::service()` (drains PD frames), `fctrig::service_ufcs()` (the UFCS message exchange),
   `fctrig::service_hs()` (the QC2 / QC3 / QC3+ handshake steps, `fc::Engine::hs_step`; the engine's `busy_` flag keeps it
   from re-entering `poll()`, and it never logs or aborts) and `transcap::poll()`. Engine phases it does not serve stay
   in `Engine::paint_critical()`, and the charger-test page paints nothing while one is open.

### 2.2 Main loop

`main` loops forever and ends each pass with `wfi`; SysTick wakes it every millisecond. The pass body and the
LCD idle hook live in `src/core/main_loop.cpp` (`k2::mainloop::Context`, `install_hooks()`, `begin()`, `pass()`):
`main.cpp` only builds the context in `boot()` and calls `pass()` + `wfi` forever. The host world replay
(`tests/test_world_replay.cpp`, see "World replay" below) calls the same `pass()`, so it cannot drift from the firmware.

| Period | Work |
| :--- | :--- |
| every pass | `usb::poll()` (TinyUSB task, `net::poll`, log flush), `shell_hw::poll`, `msc::poll` (Drive modes), `http_server::service` (Net, K2_NET=1 builds), `Remote::poll()` (runs queued requests; returns Reboot / Screenshot / Dfu / Fault for `main` to carry out), `fctrig::poll` |
| 1 ms | `fusb302::service`, sample the four buttons through two `ButtonDetector` sets: the physical pins -> `Ui::on_button` (rotation mapping, +/- chord), and the remote-injected `VirtualButton` levels -> `Ui::on_remote_button` (logical keys: never remapped by the rotation or Rotate buttons, no chord) |
| 20 ms (`kLoopMs`) | `fusb302::poll`, `power::poll`, `dpsniff::poll`, `pdtrace::vbus_sample`, `adc::poll`, `loopguard::poll` (the HID-cable loop 5 V lock, one call, before `Ui::tick`), accelerometer (bounded; `kPollMs` 100, retry 1 s), `fastcharge::poll`, `boottrace::flush_log`, `session::poll`, `bbpersist::poll`, `Ui::tick` |
| 2000 ms (`kHeartbeatMs`) | `[SYS] Heartbeat` (with `stack_free`, `msp_free`), `[USB]`, `[NET]` / `[HTTP]` (Net), `[PWR]`, `[ADC]`, `[FC]`; `[FC] idle` repeats every 5th heartbeat |

Long blocking work (LCD streaming, `systick::delay_ms`, the e-marker scan, screenshot transfers) calls
`usb::service()` / `usb::poll()` so enumeration and log delivery keep running. `[USB] gap=` reports the
longest time between two `usb::poll()` calls; about 3 ms is the healthy baseline. Never block the loop
for long; add a state machine with a `poll()` instead.

The loop keeps stall counters (`include/k2/core/loop_stats.hpp`): the gap between passes, the gap between 1 ms PD
services (the 1 ms block and the LCD idle hook), and the longest pass per page with the pixels it sent. Read them with
`k2_remote.py debug loop [--page N] [--reset]` or `tools/loop_survey.py`. What paints cost the loop today and the
rework plan are in `docs/ARCH_REWORK.md`.

ISRs (vector table in `src/drivers/startup.cpp`): SysTick (`systick::tick` and hooks), USB LP and HP
(only feed TinyUSB's event queue), EXTI12 (FUSB302 INT_N), EXTI2 / EXTI9_5 (D- PB2 / D+ PA7 edges for
`dpsniff`). Every other handler is a weak default.

### 2.3 Flash map

| Address | Size | Use |
| :--- | :--- | :--- |
| 0x08000000..0x08003800 | 14 KB | bootloader (+ info page 0x08003000): never touched |
| 0x08003800..0x080B3000 | 702 KB | application (`link.ld` `FLASH`): `.isr_vector` 0x08003800, `.meta_version` 0x08004000, `.meta_model` ("K200") 0x08004010, `.meta_commit` 0x08004400, `.text` from 0x08004404 |
| 0x080B3000..0x080E0000 | 180 KB | littlefs volume, blocks 0..89 of 2 KB (`flash::kFsBase`..`kFsEnd`); always erasable and programmable |
| 0x080E0000..0x080E4000 | 16 KB | data that was on the unit before this firmware (settings 0x080E0000, e-marker records 0x080E3000, the kept record page 0x080E3800): never erased, never programmed, only read |
| 0x080E3800 | 2 KB | kept page (`flash::kKeptPages[0]`): refused by every erase and program, whatever it holds |
| 0x080E4000..0x080FF800 | 110 KB | the run (`flash::kFsRunBase`..`kFsRunEnd`, 55 pages): first the legacy raw stores of earlier firmware (USB config 0x080E6000, UI prefs, cards, Black Box ring, journal, e-marker library), imported once and erased by `fs::wipe_legacy()` page by page only if the page carries one of our old store signatures (`include/k2/core/legacy_proof.hpp`); then, once every page of the run reads blank, littlefs blocks 90..144 (`fs::grow()`, below). Writable for the volume only after that proof (`flash::open_fs_run()`) |
| 0x080FF800..0x08100000 | 2 KB | kept top page, unknown owner (`flash::kKeptPages[1]`): refused by every erase and program, even with one of our old signatures on it |

`k2::flash` refuses every erase/program outside these rules before a register is touched: the two kept
pages are refused first, always; a program is accepted only inside the volume (blocks 0..89, plus the run
once it is open), an erase only there or on a legacy page that `legacy::ours()` proves to be ours (magic
and, for the journal, record CRCs; a blank page is never ours). The run flag is set only by the blank proof
or by a mount that finds a 145-block superblock, and stays set until reset. The block map is
`flash::fs_block_addr()` (constexpr, static_asserted): block i < 90 sits at `kFsBase + i * 2 KB`, block
90 + j at `kFsRunBase + j * 2 KB`, so the volume jumps over the native pages 0x080E0000..0x080E4000.
The DFU marker word has its own allow-list. The factory-wipe command (0x33) follows the same rule and
covers the run's blocks when the volume is grown. A volume of an older firmware (96 blocks, ending at
0x080E3000) fails the mount by block count and is reformatted with a log line (`[FS] new volume ...
formatted`); there is no migration. The part of such an old volume that lies in 0x080E0000..0x080E3000
is not provably ours and stays.

Growing the volume (`include/k2/core/fs_grow.hpp`, called from `fs::grow()` after `fs::wipe_legacy()`
at boot): the mount tries 90 blocks, then 145; any other count formats. A grow needs `/.legacy` (the
legacy imports and the wipe are done, and the imports never run again), then the proof that every word of
the 55 run pages reads 0xFFFFFFFF. It writes the intent file `/.grow` inside the 90 blocks, opens the run,
runs `lfs_fs_grow` (one superblock commit) and removes `/.grow`. A power cut leaves either the 90-block
superblock (the next boot finds `/.grow`, skips the blank proof over its own leftovers and grows again) or
the 145-block one, never a format. Logs: `[FS] grown 90 -> 145 blocks used=N` (with "(resumed after a
cut)" when `/.grow` was found), `[FS] grow skipped: 0x080XXXXX not blank` (the first page that is not
blank; the volume stays at 90 blocks and the line repeats every boot), `[FS] grow skipped: legacy wipe
pending`, `[FS] grow failed: <err> (stays at 90 blocks, retried next boot)`. A fresh format (no volume,
or a foreign one) takes 145 blocks only when the run is proven blank, and then writes `/.legacy` at once;
otherwise it takes 90 and the grow decides on the next boots.
`include/k2/drivers/flash_map.hpp` lists every region and `static_assert`s that they are sector aligned, ordered,
disjoint and that the pre-existing device storage stays out of every writable range (`tests/test_flash_map.cpp`).
File layout of the littlefs volume: section "Flash map" below.

Read-only access to the pre-existing device storage: `tools/k2_flasher.py backup` (bootloader read, full by default) and the app
command FLASH_DUMP 0x34 (`tools/k2_remote.py flash-dump DIR`, 48 bytes per request); first-contact
procedure in `README.md`, section "Recovery".

### 2.4 RAM map

RAM is 96 KB at 0x20000000 (`link.ld`); never touch SRAM above it. Layout, low to high:

| Region | Content |
| :--- | :--- |
| `.data` | initialised data and `.ramfunc` (flash programming runs from RAM); about 0.9 KB |
| `.noinit` | the SRAM retention probe (`src/core/rd_comeback.cpp`, `bootinfo` `sr=`): 32 words, NOLOAD, **not zeroed at reset** (the zero loop starts at `_sbss`). SRAM does not survive a VBUS dark on this board (0 of 32 words after 1 s, 2026-11-11), so the Rd marker's stale-record voucher is the backup-register session record (`session::backup_alive`), not SRAM; 128 B |
| `OVERLAY` `.mode_net` / `.mode_drive` / `.mode_sniff` / `.mode_trans` | one shared region at the same address, sized to the largest tenant. Default build (`K2_NET=0`): 16 KB, the D+/D- edge buffer (`.mode_sniff`) is the largest, the transition recorder is 15.9 KB, the drive image about 8 KB. `K2_NET=1` build: about 24 KB, lwIP + NCM + DHCP + HTTP (`.mode_net`) is the largest, which is the ~8 KB the default build gets back |
| `.bss` | everything else (about 55 KB in the build inspected, 859f858) |
| `.stack_guard` | 128 B no-access MPU region (`_sguard`..`_eguard`, 128-aligned) right above `.bss` (`end`/`_end` symbols for newlib's `_sbrk`) |
| PSP stack | `_eguard`..`_psp_top`, the remainder above `.bss` (20160 B at b1f99de7; `make gate` prints the current figure): thread mode (main loop, UI, drivers), grows down from `_psp_top` |
| `.msp_stack` | `_psp_top`..`_estack` = 0x20018000, 1344 B: exceptions (ISRs) and the `[FAULT]` handler; MSP starts at `_estack` |

The numbers come from `arm-none-eabi-objdump -h build/open_k2.elf` on one build and drift with every
commit; read them from your own build, do not copy them.

**Mode overlay and lease API** (`include/k2/drivers/mode_ram.hpp`, `src/drivers/mode_ram.cpp`). The USB mode is fixed at
boot and `usbcfg::normalize()` makes Net and Drive exclusive, so their buffers share RAM. The linker
(`OVERLAY` in `link.ld`) places:

| Section | Tenant | What lives there |
| :--- | :--- | :--- |
| `.mode_net` | `Tenant::Net` | lwIP pools, NCM endpoint buffer, DHCP server, HTTP server (selected by object file name under any build directory in `link.ld`, no attribute needed). Empty in the default `K2_NET=0` build, which has none of those objects |
| `.mode_drive` | `Tenant::Drive` | objects marked `K2_MODE_RAM_DRIVE` (the MSC `drive::Drive`, `src/drivers/msc.cpp`) |
| `.mode_sniff` | `Tenant::Sniff` | objects marked `K2_MODE_RAM_SNIFF` (`dpsniff` edge buffer, `src/fastcharge/dpsniff.cpp`) |
| `.mode_trans` | `Tenant::Trans` | the transition-capture recorder (`k2::transcap`, 15.9 KB), claimed only while a capture is armed, recording or being written out |

Rules: every tenant calls `mode_ram::claim(Tenant)` before touching its buffer; the first claim wins for the
session and a second tenant is refused (a log line, not corrupted memory: Sniff is refused while Net or
Drive owns the region, i.e. it works only in HID-only mode). The region is inside the range `Reset_Handler`
zeroes, so the owner starts from zeros; overlay objects carry no initialiser and the owner constructs them
after `claim()`. Only buffers nothing touches outside their own mode belong here (TinyUSB's
`ncm_interface` and `_mscd_itf` are reset by `tud_init()` in every mode and stay in `.bss`). Host tests:
`tests/test_mode_ram.cpp`. `size` double counts the overlay (four sections at one address), so its `bss`
figure exceeds the real RAM use.

**Stack layout.** Memory above `.bss` is, from low to high: the MPU guard (`_sguard`..`_eguard`, 128 B), the PSP
stack (`_eguard`..`_psp_top`, the application) and the MSP stack (`.msp_stack`, `_psp_top`..`_estack`,
1344 B). `Reset_Handler` (a naked trampoline in `src/drivers/startup.cpp`) sets PSP = `_psp_top`, switches thread mode to it
and calls `k2_reset_main`; ISRs and the `[FAULT]` handler therefore run on the MSP, a stack the overflow cannot have
damaged. An exception taken from thread mode stacks its 104-byte frame (FP included) on the PSP.

**Stack budget and the stack gate.** An overflow runs down through the guard, which the MPU turns into a fault, not
into silent `.bss` corruption. Four layers:

* Trap: `fault::init()` (`mpu::guard`, `src/drivers/mpu.cpp`, pure region maths in `k2/drivers/mpu_calc.hpp`) makes the guard a
  no-access region (MPU CTRL ENABLE | PRIVDEFENA; HFNMIENA stays clear, so HardFault and NMI run unprotected). A
  push into it raises MemManage; `fault::classify` reports it as `[FAULT] SO PC= LR= ...` (DACCVIOL with MMFAR in the
  guard, or MSTKERR: the exception frame itself did not fit). The handler switches the MPU off first, logs on the
  MSP and resets. A single frame bigger than the guard (128 B) can jump over it; the frame limit
  (`-Wframe-larger-than=1024` in the Makefile, `-Werror` under the gate) keeps that unlikely but not impossible,
  which is why the gate also checks depth. `k2_remote.py fault --confirm stack` (`DEBUG_FAULT` kind 2) recurses
  until the guard trips: expect `[MPU] stack guard ... on` at boot and `[FAULT] SO` on the trigger.
* Runtime: both stacks are painted with 0xA5A5A5A5; `k2_stack_free_bytes()` / `k2_msp_free_bytes()`
  (`src/drivers/startup.cpp`) count the untouched words from the guard / the MSP bottom, printed as `stack_free=` and
  `msp_free=` in every `[SYS] Heartbeat` (`stack_free` counts the PSP stack only; it no longer includes the old
  1 KB gap). When `stack_free` falls below 1024 B a new low-water mark logs `[STACK] low: free=<n> page=<index>`
  once per new minimum, so a deep page path is named before it overflows.
* Build gate: `tools/gate.py` step "stack budget" reads the layout symbols (`nm`) and the worst call depth from the
  `.ci` call graphs of the last build (`tools/stack_depth.py`, the main loop root `k2_reset_main` with the deepest
  page path, the ISRs, `k2_fault_main`) and fails when the PSP stack is smaller than depth + 104 B exception frame +
  1024 B, or the MSP stack smaller than max(deepest ISR, `[FAULT]` handler) + 64 B, or the guard is not a valid MPU
  region, or fewer than `MIN_STACK_BYTES` = 8192 B lie between `.bss` and the RAM top. It also fails when the worst
  path holds an indirect call the analysis cannot resolve (the depth would be a lower bound) and when a source sets
  an interrupt priority (the analysis assumes no nesting). Fix by shrinking frames on the printed path or `.bss`
  (`nm --size-sort build/open_k2.elf`), not by lowering a number.
* Analysis: `make stack-report` (or `python3 tools/stack_depth.py` after a build) prints the worst path. The
  Makefile compiles with `-fcallgraph-info=su,da`, `-Wframe-larger-than=1024 -Wstack-usage=1024` (ratchet to 512 is a
  TODO), and `--param=large-stack-frame=256 --param=large-stack-frame-growth=100` so the optimiser does not merge
  big frames. The analyzer follows page virtuals, `ListView` row callbacks, the remote `handle_request`
  dispatch, littlefs / TinyUSB / lwIP callbacks through `RESOLVE` rules, with a per-class nesting limit
  (`LIMITS`); add a rule when a new callback table appears. Rule: no big arrays on the stack; transient
  buffers use the main-loop scratch arena (`include/k2/core/scratch.hpp`, one owner tag at a time).
  Three habits keep the deep paths shallow (worst main-loop depth 5192 B at b1f99de7, 4524 B after the trim):
  a TinyUSB callback never renders (the LCD waits and `systick::delay_ms` run `usb::service()` nested under a
  paint, so the MSC READ10 callback only stashes the request, `k2::msc::io()` serves it from the top of the main
  loop and completes it with `tud_msc_async_io_done`); a row of pixels is rendered and streamed in 80 px windows
  (`render_row_span`, `gfx_text`, `home::paint`), never through a full-width buffer; and a function that holds a
  big local is `noinline` when its callee chain is deep (`report_card::build_into`, `Store::read_entry`), so the
  local's slot is not kept in the outer frame while the callees run.

### 2.5 Invariants checked after every build

| Item | Address |
| :--- | :--- |
| `.isr_vector` (`g_pfnVectors`, 0x130 bytes) | 0x08003800 |
| `.meta_version` | 0x08004000 |
| `.meta_model` (`k2_model_signature` = "K200") | 0x08004010 |
| `.meta_commit` (0xFFFFFFFF) | 0x08004400 |
| `.text` | 0x08004404 |

`Reset_Handler`, `USB_LP_CAN1_RX0_IRQHandler`, `g_pfnVectors` and `k2_model_signature` stay unmangled
(`extern "C"`) with their `section`/`used` attributes. `make gate` checks all of this and that the map has
no `malloc`, `operator new` or `__cxa_*`. A wrong layout leaves the K2 in DFU.

### 2.6 The firmware version string

The version is ONE string, `<major>.<minor>.<patch>-<iteration>-<hash>`, for example `1.0.0-435-26b245ac`, with
`-dirty` appended on a dirty tree. `tools/fw_version.py` derives it from `git describe --tags --long --always
--dirty --match 'v[0-9]*'`: the newest `v*` tag (lightweight or annotated) normalised to three numbers (`v1.0` gives `1.0.0`, `v1.1` gives
`1.1.0`, `v1.2.3` stays), the commit count since the tag, the short hash without git's `g`. With no usable tag it
reads `0.0.0-0-<hash>`, with no git at all `0.0.0-0-unknown`. The Makefile calls the tool once (`K2_FW_VERSION`; an
environment or command-line value wins; `make docker-build` forwards it because the container has no git view) and
`src/core/version.cpp` bakes it into the image (`kFwVersion`, at most 29 characters, `static_assert`ed: that is what
fits PING's 58-byte payload after proto, uptime and git). The raw `git describe --always --dirty` stays as `kGitHash`
for traceability.

Where it shows: PING (`version` is the full string, `git` the raw describe in 24 bytes, so the raw form is cut at 1000
commits since the tag); the boot-screen line `fw <version> p<proto>` and the `K2B1 fw=<version> g=<git> p=<proto>`
text (`bootinfo::Snapshot::fw` holds 31 characters); the web dashboard; the Python and WebHID flashers (installed
version, "Device upgraded from vX to vY", backup names, `pack --version`); the msc drive README. Not carried: the
report card's firmware token (a fixed 4-byte commit-hash token, `fw_hash(kGitHash)`), the QR log record and the
stream hello (both carry the raw git).

The bootloader reads only the one-byte `.meta_version` (major and minor nibbles, `0x10` for 1.0), which stays as it
was. The full string also sits in a 48-byte record, `"K2FW"` + 44 NUL padded text bytes (`.fwid`,
`src/core/version.cpp`), placed by `link.ld` first in `.text` at 0x08004404 (app offset 3076). It lies in code space,
so the bootloader never looks at it; the flashers read it from a package and from the device (`FWID_OFFSET` in
`tools/k2_flasher.py` and `webhid-flasher/k2boot.js`). Firmware from before the record has no `K2FW` there and is
shown as `major.minor`.

## 3. Layers

| Layer | Files (`include/k2/<group>/*.hpp` + `src/<group>/*.cpp` unless noted) | Notes |
| :--- | :--- | :--- |
| Startup, clocks, time | `startup.cpp`, `crm`, `systick`, `sysreset`, `dfu`, `fault`, `board`, `bootmode`, `boottrace`, `boottrace_store` | `regs.hpp` is the typed register map, `pins.hpp` the one pin map |
| Bus and chip drivers | `i2c` (two bit-banged buses), `fusb302` (CC / PD PHY), `power_monitor` + `power_calc` (INA226 / 228 / 237), `adc` + `adc_calc` (DMA scan: D+, D-, CC1, CC2, aux, NTC, VREFINT), `lcd` (ST7789V on SPI3), `accelerometer`, `load` (PA8 test-load PWM), `dpdm` (D+/D- switch network; its passive idle `kIdle` releases both lines, never the HVDCP entry level or a QC2 code), `uart2` (UFCS / VIVO), `dpsniff` (edge capture) | Each owns its pins, clocks, DMA, IRQ; other code uses only the API |
| USB | `usb` + `usb_descriptors.cpp` + `usb_pma` + `usbcfg` (HID log / remote), `msc` + `vfat` + `drive` (FAT16 drive), `net` + `http_server` (CDC-NCM, lwIP, port 80; compiled only with `K2_NET=1`), `shell_hw` + `shell` (CDC-ACM debug shell) | TinyUSB and lwIP under `third_party/`; modes and packet-memory budget below |
| PD engine | `pd` (pure decode), `pd_phy`, `pd_passive`, `pd_link`, `link`, `link_guard`, `pd_engine` (read-only decoder), `pd_vdm`, `pd_ext`, `pd_sink` (trigger: Request, PPS/AVS), `pd_identify`, `pd_probe` (quick source probe for report cards), `pd_source`, `pd_rescan`, `pd_ccscan`, `pd_emarker_plan`, `pdtrace` (+ `_link`, `_view`) | "PD driver model" below |
| Fast-charge protocols | pure frames and step lists: `qc`, `afc`, `accp` (FCP / SCP), `vooc`, `ufcs`, `vivo`, `wire` (ACCP/AFC single-wire PHY), `fc_seq` (timed D+/D- step lists played by `dpdm::play`); engine: `fc_engine` (state machine over an `Env`), `fc_safety`, `fc_scan` (one-click detect), `fctrig` (binds the engine to hardware), `fastcharge` + `fastcharge_classify` (passive classifier); `lab` (protocol lab grammar) | Active triggers refused inline by `link_guard` |
| Charger test | `charger_test` (pure `Runner`: algorithms, steps, verdicts), `charger_journal` (+ `_hw`, reboot-resume), `report_card` (+ `_store`, `_hw`, `_delta`, `_json`, `_remote`), `ai_recog`, `result_layout`, `src/ui/ui_charger_test.cpp` (the page that owns the runner) | Reference: "Charging procedure runner", "Charger report cards" |
| Storage | `flash` (guarded controller), `fs` + `fs_flash` (littlefs wrapper bound to the real flash), `flash_map`, `store` + `storage` (native e-marker pages), `emlib` + `emrec` + `emname` + `emjson`, `usbcfg`, `uiprefs`, `bbstore` + `bbpersist` + `lz` (Black Box files), `session` + `session_store` (backup registers) | All persistent app data is files on the littlefs volume |
| Remote control | `include/k2/core/remote_proto.hpp` + `src/core/remote_proto.cpp` (frame codec and the dispatcher template), `include/k2/core/remote.hpp` + `src/core/remote.cpp` (firmware backend), `include/k2/ui/capture.hpp` + `src/ui/capture.cpp` (screenshot), `k2/net/http_api.hpp` (routes + JSON), `k2/core/shell.hpp`, `k2/core/lab.hpp` | Host side: `tools/k2_remote.py`, `tools/web/k2proto.js` |
| UI | `include/k2/ui/ui.hpp` + `src/ui/ui.cpp` (`Ui`: page stack, tick, capture), `ui_pages.hpp` + `src/ui/ui_pages.cpp` (Home, Detail, Lines, Tools, Settings, Diag, Trigger, PdTrigger, CableR, Autorotate, UsbLink pages), `ui_charger_test.cpp`, `ui_blackbox`, `ui_cards`, `ui_emarker`, `ui_spoof`, `ui_cabler`, `ui_connection(_page)` (Connection page + pure row model), `ui_match`, `ui_lines`, `ui_tools`, `ui_detail`, `ui_orientation`, `ui_pdstress_page`; renderer: `gfx`, `text`, `blend`, `font_aa.hpp` + `src/ui/fonts/` (Atkinson Hyperlegible Mono 12 / 16 / 24 px and 48 px digits, 16 coverage levels stored run-length coded in 8-row groups, token format in `font_aa.hpp`; generated by `tools/gen_font.py` from `third_party/fonts/atkinson-hyperlegible-mono`, or from the level cache `tools/font_levels.txt` when freetype is absent), `ui_icons` (`tools/gen_icons.py`), `ui_widgets`, `list_view`, `ui_viewport`, `ui_style`, `ui_labels`, `ui_layout`, `ui_events`, `buttons`, `virtual_button` | "Tools hub", "Input, pages and ListView" |
| Host tools (`tools/`) | `k2_remote.py` (remote client), `k2_flasher.py` (DFU flasher, `--auto-dfu`, `monitor`), `pd_trace.py` (Black Box), `hw_smoke.py` (`make smoke`), `gate.py` (`make gate`), `check_cmd_ids.py`, `logdb.py`, `stack_depth.py`, `static_check.sh`, `embed_web.py`, `gen_font.py`, `gen_icons.py`, `report_card.py`, `charger_summary.py`, `ct_start.py`, `ui_walk.py`, `proto_lab.py`, `sniff_stream.py`, `vooc_edges.py`, `panel2png.py`, `home_sheet.py`, `k2_lock.py`, `web/` (dashboard) | One owner of the device: `k2_lock.py` flock |

**Page indices.** `include/k2/ui/ui_types.hpp`: `Page` = Home 0, Detail 1, ChargerTest 2, Lines 3, Tools 4,
Settings 5 (`Count` = 6); `Menu` (modal submenus, never in the rotation) = Trigger, Emarker (the cable e-marker/saved-cables list, opened
from Connection), PdTrigger, CableR, BlackBox, Spoof, UsbLink, Autorotate, Cards, Share, Connection, Diag, QrTest, Twin, LoopProbe,
PdStress. `GET_STATE.page` reports `kMenuPageBase` (= `Page::Count` = 6) + `Menu` while a submenu is open, so Connection is 16,
Diag 17 and PdStress 21 (remote page name `pdstress`).
(The Connection page is shown on screen, in the Tools list and in its title bar, as "Who's attached"; the identifiers
`Menu::Connection`, `ConnectionPage`, the remote id `tools/connection` and the page name `connection` keep the old name.)
The Connection tabs have remote-only aliases `kConnTabAlias` (= 6 + `Menu::Count`) + 0 Source, 1 Cable, 2 Sink, today 22 / 23 / 24 (SET_PAGE opens
Connection on that tab; `k2_remote.py` maps pd/source, emarker/cable, identify/sink to them). The offers-versus-request comparison lives in
`ui_connection_page.cpp`; the pure row model
(tabs, actions, default tab, port-aware empty states, protocol names) is `include/k2/ui/ui_connection.hpp`.
Identify is an action of the Sink tab (`Ui::identify`) and the e-marker read an action of the Cable tab, neither
has a Tools row any more; Spoof opens Connection on its Cable tab. Page names live in four places that
`tests/test_page_names.py` keeps equal: `ui_types.hpp`, `tools/k2_remote.py` (`PAGES`), `tools/web/k2proto.js`
and the copy in `tools/web/k2.html` (`make web-sync`).

**Charger-test setup / wizard pages.** `ChargerTestPage` (`src/ui/ui_charger_test.cpp`) has a three-step setup body
(Tests, Cable, Review; Custom and Group are sub-lists of Tests) and a results view; `include/k2/ui/ui_setup_page.hpp` is
the shared look and button meaning of those pages (header "n/3  Title", footer hint, side-panel labels, list scroll
arithmetic, Next / START always row 0) and `include/k2/chargertest/result_layout.hpp` is the pure Results layout. The cable
identity list is one object, `CableIdentityList` (`include/k2/ui/ui_cable_list.hpp`, `UiContext::cables`): rows (Off,
built-in chips, saved clones), cached names, the selection, the armed marker and the one arm path. The wizard's Cable
step and Tools -> Spoof (`include/k2/ui/ui_spoof.hpp`) are two views of it. The PD trigger wizard model is
`include/k2/ui/ui_wizard.hpp` (OFFER, VOLTAGE, CURRENT, CONFIRM, LIVE).

**Page arena.** `Ui` does not hold its pages as members. The base (rotation) page and up to three open menus are built in
one `StackArena` (`include/k2/ui/page_arena.hpp`, `Ui::pages_`) when they are entered and destroyed when they are left;
arena index 0 is the base page, `stack_[i]` lives at index i + 1. The arena size is a constexpr maximum over the page
types (`kPageArenaBytes` in `ui.hpp`: the largest base plus the largest menu, or the deepest menu chain, whichever is
bigger, `static_assert`ed), so adding a page type grows it only if that page is the largest. Every visit builds a page
fresh: nothing a page paints from a cache survives, a full `invalidate()` repaints it, and `enter()` sets its defaults
(Connection starts on its default tab each time). A page switch keeps the old order: `leave()`, an erase pass over
the outgoing paint while the page still exists, destroy, build, `enter()`, paint. What must outlive a page is not in
it: the remembered cursors and the trend window and the FC trigger draft are `UiContext::keep` (`Remembered`), the QR
share record is `ShareRecord` (`Ui::share_rec_`, a `ShareRecord`), the charger test's runner, results and cursor are
`ChargerTestSession` (`Ui::charger_test_`, the page is only its view and binds itself with `bind_view`), the armed
spoof cable is `ctx_.cables` and the `emk_spoof` driver, and the black box is the `pdtrace` globals. Code that reaches
a page from outside (`share()`, `connection_page()`, the trigger poll) finds it on the stack and must tolerate its
absence; remote commands use the resident owners.

**Fonts.** All text is anti-aliased monospace (Atkinson Hyperlegible Mono, SIL OFL 1.1; the four arrows come
from JetBrains Mono), drawn row by row through one 240-pixel line buffer (`gfx.hpp`); `text.hpp` does UTF-8
and layout, `blend.hpp` the 4-bit alpha blend. There is no framebuffer. The glyph pixels are 4-bit
coverage levels, run-length coded as nibbles in 8-row groups (token format in `font_aa.hpp`): the level is the
blend LUT index, so the edge pixels blend against the bg of the cell the text is drawn on (`render_row` fills
`lut[0]` over the whole width). A glyph table entry is 6 B; the leading 0x20.. block of each font is found by
index (`Font::dense`), only the rest carries a sorted code point table. Cell sizes and line boxes are the layout
contract: advance 6/8/12/24 px, line box 12/16/24/30 rows. Do not edit `include/k2/ui/font_aa.hpp` or `src/ui/fonts/*`
by hand: change `tools/gen_font.py` and regenerate (needs `freetype-py` and `fonttools` in a venv;
`--check` without them only compares the input stamp). `--source spleen` regenerates the old 1 bpp
Spleen tables (`tools/font_overrides.py` holds its hand-drawn fill-ins).

## 4. Data flows

### 4.1 A charger-test run

```
 user: Charger test page, setup -> START (2x OK above 5 V)        ct_start.py / remote presses
   |
   v
 ChargerTestPage (src/ui/ui_charger_test.cpp)  --Options, Input-->  charger_test::Runner::start(caps, ...)
   |   Env callbacks (page implements charger_test::Env)             builds the step list from the
   |                                                                  source caps + selected algorithms
   | Ui::tick -> Runner::poll(Input{vbus, ibus, link}, caps)   (every 20 ms, even off-page)
   |        |
   |        +-- PD step  -> pd_sink (fusb302 Request, PPS/AVS)   -- hold 1 s in band --> release to 5 V
   |        +-- legacy   -> fctrig / fc_engine -> dpdm (D+/D-)   -- QC, AFC, FCP, SCP, VOOC, UFCS, VIVO
   |        +-- guards   -> link_guard (inline load), fc_safety, power_monitor ceiling, vSafe5V release
   |
   +-- before an above-5 V request / handover: charger_journal::hw::phase()  (littlefs /jr, resume after reboot)
   +-- per step: charger_journal::hw::step()
   +-- pd_probe at the start: Discover Identity etc. on SOP in the 5 V contract (evidence for the card)
   v
 Done / Stopped / Unsafe
   |-- report_card::build(steps, evidence, ...) -> report_card::hw::put()  ->  /cards/<fp>.card
   |-- report_card::delta(previous card, new card)  -> "what changed" lines on the Results view
   '-- remote: CHARGER_TEST 0x0B (read-only RAM view)  ->  k2_remote.py charger-test status|results|view
       USB drive: CARDS.JSON (generated from /cards on read)
```

The runner is pure (no hardware globals, no heap) and is driven only by `poll()`; rendering never advances it.
Safety gates are in the runner and the engines, not only in the UI.

### 4.2 A remote command over HID

```
 host: k2_remote.py  (Link, one request in flight, exclusive flock)
   |  64-byte request  A5 4B cmd seq len payload      EP1 OUT / SET_REPORT
   v
 usb ISR -> TinyUSB event queue                       (ISR only feeds the queue)
   v  usb::poll() in the main loop
 tud_hid_set_report_cb (src/drivers/usb.cpp) -> usb::on_remote_report()      queues the frame (queue depth kQueueDepth)
   v  Remote::poll() in the main loop (src/core/remote.cpp)
 remote::handle_request<Remote>(raw, backend, out)    include/k2/core/remote_proto.hpp: validates magic / length,
   |                                                  switch on Cmd, calls the Backend (Remote or a test fake)
   |  Backend -> Ui (button, page, state), fusb302, fctrig, pdtrace, emlib, report_card, ...
   v
 usb::send_report(out)   -> TX queue (tx_queue.hpp) -> EP1 IN    A6 4B cmd seq status len payload
   v
 host decodes; non-zero status -> "[RMT] cmd= seq= status=" log line
```

Side effects that must happen in the main loop but not inside the dispatcher come back as a
`RemoteEvent` (`Reboot`, `Screenshot`, `Dfu`, `Fault`) that `main.cpp` carries out. The same dispatcher serves
the HTTP bridge (`/api/frame?hex=`, `src/net/http_server.cpp`) and the CDC shell reuses `Remote::fc_cmd`.
Active commands (Fc, PdRequest, PdReset, IdentifyDevice, ...) are refused inline and above 5 V need the
confirm flag (`Status::NeedConfirm`).

### 4.3 A screenshot

```
 host: k2_remote.py screenshot out.png          (or HTTP GET /api/screenshot.bmp)
   v  Cmd::Screenshot -> Remote::poll sets shot_pending_ -> returns RemoteEvent::Screenshot
 main.cpp: capture_screen(ui, seq, now)                       src/ui/capture.cpp, one owner (g_busy) for HID and HTTP
   |  for strip in 0..14:                                      16-row strips, 7680-byte g_strip buffer
   |     PixelSink.attach(g_strip, ...)  +  lcd::set_sink(&kSink)
   |     Ui::render_for_capture(now)     -- re-renders the WHOLE screen through gfx -> lcd::stream_* -> PixelSink,
   |                                        keeping only this strip's rows (the panel is write-only, no readback)
   |     lcd::set_sink(nullptr)
   |     Rle565Encoder -> chunks of kShotChunk -> remote::build_stream_frame (cmd 0x8A) -> usb::send_report
   |        (waits for usb::tx_free(), servicing usb::poll())
   '-- capture_end: Ui::resync() repaints the real panel
 host: decodes RLE565 strips (flags bit0 = last chunk of strip, bit7 = abort) -> PNG
```

The panel content is untouched during a capture; only the USB IN queue is busy for a few hundred ms. The HTTP
path pulls one strip at a time (`capture_begin` / `capture_strip` / `capture_end`).

## 5. Common helpers

Reuse these before writing a new one. Headers under `include/k2/<group>/` unless a path is given.

| Helper | What it is, when to use it | Path |
| :--- | :--- | :--- |
| `Critical` | RAII PRIMASK save / `cpsid i` / restore; nests; no-op on host. Wrap a read-modify-write of a register an ISR also writes (GPIO CRL/CRH shared with the SysTick load guard) | `include/k2/core/critical.hpp` |
| `power::abs_sat`, `magnitude`, `apply_deadband`, `flow_of` | Saturating absolute value (INT32_MIN safe), unsigned magnitude, 10 mA idle deadband, flow direction. Use `abs_sat` instead of `std::abs` / `-x` on a signed mA or mV value | `include/k2/drivers/power_calc.hpp` |
| `power::max_vbus_mv`, `pd::request_cap_mv` | The monitor's VBUS ceiling (INA226 36 V, INA228/237 48 V) and the request cap derived from it; every trigger path must clamp to it | `include/k2/drivers/power_calc.hpp`, `include/k2/drivers/power_monitor.hpp`, `include/k2/pd/pd_sink.hpp` |
| `K2LOG("[TAG] fmt\n", args...)` | Compact log line: the format stays out of flash, the device sends `msg#<id> args`; IDs come from the append-only `tools/logdb.json`. Use for every fixed-format log line | `include/k2/core/log.hpp`, `include/k2/core/logfmt.hpp`, `tools/logdb.py` |
| `usb::log`, `usb::log_fmt` | Runtime-text log line (max 63 bytes, 16-slot queue); for computed strings only | `include/k2/drivers/usb.hpp` |
| `LogRing` | Fixed byte ring of recent log text feeding LOG.TXT and `store save-log`; fed before the on-device log gate | `include/k2/core/logring.hpp` |
| `fmt::format`, `fmt::vformat` | printf-style formatter (%d %u %x %X %s %c, width, 0 flag); no heap, no float, no stdio | `include/k2/core/fmt.hpp` |
| `TextBuf` | Append-only builder over a caller buffer on top of `fmt`; `truncated()` tells; for drive and card text | `include/k2/core/textbuf.hpp` |
| `StrBuf<N>`, `fmt_abs_fixed`, `Throttle`, `EnergyMeter`, `WattHistory` | UI number formatting (fixed-point, hms, age, never a '-'), redraw throttle, energy integration | `include/k2/ui/ui_calc.hpp` |
| `systick::millis / micros / ticks / delay_ms / delay_us`, `set_tick_hook`, `set_delay_hook` | 1 ms time base and DWT-cycle timing; `delay_ms` services USB. Wrap-safe: subtract readings | `include/k2/drivers/systick.hpp` |
| `lcd::Sink` + `PixelSink` | Divert every LCD write into a RAM strip; used by screenshots and host UI tests | `include/k2/drivers/lcd.hpp`, `include/k2/ui/pixel_sink.hpp` |
| `Rle565Encoder`, `rle565_decode` | RLE of RGB565 pixel streams for the screenshot transfer | `include/k2/ui/rle565.hpp`, `src/ui/rle565.cpp` |
| `TxQueue` | Ring of 64-byte IN reports shared by stream frames and log lines; a log line never evicts a frame | `include/k2/core/tx_queue.hpp` |
| `VirtualButton` | Remote-injected button level with a release gap, time in / level out | `include/k2/drivers/virtual_button.hpp` |
| `ButtonDetector` | Debounce plus Short / Long / Hold detection; pure | `include/k2/drivers/buttons.hpp`, `include/k2/ui/ui_events.hpp` |
| `mode_ram::claim`, `K2_MODE_RAM_DRIVE`, `K2_MODE_RAM_SNIFF` | Lease on the overlaid mode RAM; see 2.4 | `include/k2/drivers/mode_ram.hpp` |
| `flashmap::kRegions` | Every flash region above the app with compile-time overlap checks; add a new region here | `include/k2/drivers/flash_map.hpp` |
| `flash::store_allowed`, `region_end` | The guarded erase/program policy; the only way flash is written | `include/k2/drivers/flash.hpp` |
| `fs::Fs` (`read_file`, `write_file` = tmp + rename, `append_file`, `remove`, `rename`, `mkdir`, `list`), `fs_flash` | littlefs wrapper over a `BlockDevice` (real flash or RAM disk); use for all persistent app data | `include/k2/core/fs.hpp`, `include/k2/drivers/fs_flash.hpp` |
| `Store<Hw>` / `storage` | Read-only access to the native e-marker pages over a `view()` seam | `include/k2/pd/store.hpp`, `include/k2/core/storage.hpp` |
| `usbcfg`, `uiprefs`, `cablepref` | Stored settings files `/usb.cfg`, `/prefs.cfg`, `/cable.cfg` (magic + value + checksum, defaults when damaged); model for a new setting | `include/k2/drivers/usbcfg.hpp`, `include/k2/ui/uiprefs.hpp`, `include/k2/chargertest/cablepref.hpp` |
| `session` + `session_store` | 16-halfword record with CRC-16 in backup registers DR22..DR37, survives resets | `include/k2/chargertest/session.hpp` |
| `lz::compress / decompress` | LZ4 block codec for small buffers, bounds-checked, 512 B hash table on the stack | `include/k2/core/lz.hpp` |
| `link_guard::inline_reason` | Is a live sink or an inline load behind the K2? Every active operation asks first | `include/k2/pd/link_guard.hpp` |
| `load_guard` | Pure safety rules of the PA8 test-load pulse (duty <= 95 %, <= 200 ms, VBUS window, MCU <= 70 C) | `include/k2/drivers/load_guard.hpp` |
| `fc_safety` | Voltage band and abort guard shared by every fast-charge trigger; `clamp_to_ceiling` | `include/k2/fastcharge/fc_safety.hpp` |
| `fc_seq` step lists | Timed D+/D- steps built by `qc/afc/accp` and played by `dpdm::play`; tests replay them on a fake clock | `include/k2/fastcharge/fc_seq.hpp` |
| `edges` | D+/D- edge packing, fixed buffer, capture end rule, conversion to Black Box records | `include/k2/fastcharge/edges.hpp` |
| `pdtrace` records | Black Box binary records (PD message, event, VBUS sample) and capture slots | `include/k2/pd/pdtrace.hpp` |
| `dbg::Mux`, `runev::Queue`, `BinLog` | The multiplexed live debug stream (frames, priorities, loss accounting), the charger-test run event ring and the binary K2LOG ring; host reader `tools/k2_stream.py` | `include/k2/core/dbg_stream.hpp`, `include/k2/chargertest/run_events.hpp`, `include/k2/core/binlog.hpp`, `docs/DEBUG_STREAMING.md` |
| `remote::build_stream_frame`, `Backend` concept, `kCmdIds` | Frame builders and the duck-typed backend seam of the dispatcher; the command id table | `include/k2/core/remote_proto.hpp` |
| `http_api` | Pure routes and JSON for the HTTP link | `include/k2/net/http_api.hpp` |
| `shell`, `lab` | Pure parser of the CDC shell and the data-driven protocol-lab grammar | `include/k2/core/shell.hpp`, `include/k2/core/lab.hpp` |
| `ListState` / `ListView` | Scrolling list: fixed row height, clamped offset, scrollbar only on overflow, redraws changed rows | `include/k2/ui/list_view.hpp` |
| Setup page helpers | Shared header / footer / side labels / scroll arithmetic of the charger-test wizard pages | `include/k2/ui/ui_setup_page.hpp` |
| `CableIdentityList` | The one cable-identity list (Off, built-in chips, saved clones): cached names, selection, armed marker, arm / disarm; shown by the wizard Cable step and Tools -> Spoof | `include/k2/ui/ui_cable_list.hpp`, `src/ui/ui_cable_list.cpp` |
| Widgets | Rounded fills, pills, gauges, chevrons, icons, sparklines (row-streamed) | `include/k2/ui/ui_widgets.hpp`, `include/k2/ui/ui_icons.hpp` |
| `ui_style`, `ui_labels`, `ui_layout`, `ui_viewport` | Palette and fonts, button-panel labels, content / panel geometry for the four rotations, coordinate mapping | `include/k2/ui/ui_style.hpp`, `include/k2/ui/ui_labels.hpp`, `include/k2/ui/ui_layout.hpp`, `include/k2/ui/ui_viewport.hpp` |
| `gfx`, `text`, `blend` | Row-streamed text and fields, UTF-8 + layout, 4-bit alpha blend | `include/k2/ui/gfx.hpp`, `include/k2/ui/text.hpp`, `include/k2/ui/blend.hpp` |
| `result_layout` | Pure Results-screen layout of a charger-test run | `include/k2/chargertest/result_layout.hpp` |
| `emrec`, `emname`, `emjson`, `emlib` | E-marker record codec, auto names, JSON, saved-cable library | `include/k2/pd/emrec.hpp` and siblings |
| `report_card*` | Card record, fingerprint, store, delta, JSON | `include/k2/chargertest/report_card*.hpp` |
| `charger_test::Runner`, `Env` | The pure test runner and the hardware seam its page implements | `include/k2/chargertest/charger_test.hpp` |
| `web_asset` / `tools/embed_web.py` | Gzip the dashboard into flash at build time (K2_NET=1 builds only) | `include/k2/net/web_asset.hpp`, `tools/embed_web.py` |
| Host test kit | `CHECK_EQ`, `CHECK_STR` (`test_util.h`); `FakeLcd`; `ramdisk.hpp` (NOR-semantics RAM block device with power-cut injection: use for any littlefs-backed logic); `fake_remote_backend.hpp`; `fc_fake.hpp` (fake QC charger and clock); `ui_stubs.*` | `tests/` |

## 6. Rules of the road

**Language and memory**

* All new code is C++23 (`-fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit`). No `new` /
  `delete`, no heap, no `<vector>`, `<string>`, `<iostream>`, `<format>`, `std::function`. Use `<array>`,
  `<span>`, `<string_view>`, `<optional>`, `<expected>`, `<bit>`, `<algorithm>`. The gate fails on `malloc`,
  `operator new` or `__cxa_` in the map.
* ISRs, the vector table and meta objects keep `extern "C"` and their `section`/`used` attributes. Include
  `k2/` headers before, never inside, an `extern "C"` block.
* Big buffers are static (`.bss`) or leased from the mode overlay, not on the stack (stack gate, 2.4).
* Drivers are self-contained: one header, one `.cpp`, own namespace, own pins/registers/clocks/DMA/IRQ,
  small init/poll/read API. Reaching into another driver's registers or globals is a bug. Protocol logic is
  pure and host-tested.
* Never block the main loop; long operations are state machines with a `poll()`. Anything that waits
  calls `usb::service()`.
* Shared files (`src/ui/ui_pages.cpp`, the remote dispatcher, `k2_remote.py`, `k2proto.js`, `Makefile`): add
  entries, do not reorder or reformat existing ones, so parallel branches merge.

**Logging**

* Fixed-format lines use `K2LOG("[TAG] ...\n", ...)`; commit the regenerated `tools/logdb.json` with the source
  (IDs are append-only and never reused). Runtime strings use `usb::log`. Tag temporary debug lines
  (`[DEBUG-xxxx]`) and remove them before the commit.

**Hardware safety** (details: docs/HW_TEST_PROCEDURES.md, docs/PROTOCOL_LAB.md)

* **VBUS ceiling:** the INA226 on this unit is rated 36 V. Requests are capped by `pd::request_cap_mv` /
  `power::max_vbus_mv` (36 V INA226, 48 V INA228/237); the overvoltage abort (runner per-sample check and
  `Sink::on_vbus`) sits at that ceiling + 5 % (`pd::overcap_abort_mv`: 37.8 V / 50.4 V); never feed a charger above 36 V into this K2. The
  36 V / 48 V EPR steps are not testable here. QC2 / QC3 / QC3+ are 12 V at most, so a QC 20 V failure is
  expected, not a finding.
* **Phone attached:** never drive D+, D-, CC or PA8 with a phone (a live sink) attached. Every active
  operation goes through `link_guard::inline_reason`; do not add a bypass.
* **No load:** the PA8 test load is not a general tool. It is pulsed only through `k2::load` under
  `load_guard` (bounded duty, time, VBUS window, temperature); what sits behind PA8 is unverified.
* Active triggers above 5 V need a confirm (two OK presses on the device, `--confirm` / `confirm=1` remotely).
* Unattended runs send no trigger, Identify, DFU-failure path or fault test and do nothing that changes
  VBUS. Hardware validation only through `make smoke`. One owner of the K2: tools take the flock in
  `tools/k2_lock.py`. Flash only committed builds (a `-dirty` image fails `tools/hw_smoke.py`).
* The bootloader region (below 0x08003800), the info page and the native e-marker pages are never written.
  SRAM above the mapped 96 KB is never touched.

**Build, test, gate**

| Command | What it does |
| :--- | :--- |
| `make docker-build` | Firmware in the `open-k2-builder` image (the host has no ARM toolchain); output `build/open_k2.elf` / `.k2` |
| `make test` | Host unit tests: C++ (`tests/test_*.cpp`, native g++, run in parallel), Python (`tests/test_*.py`) and node (`tests/test_k2proto.mjs`) |
| `make gate` | `tools/gate.py`: build with -Werror, bootloader invariants, stack budget, map check, command-id uniqueness, all tests, size report against `tools/size_baseline.txt`. Text growth over 5 % is a warning: explain it in the commit body Web parts off by default: `make K2_WEB=1 gate` adds the node tests (tools/web, the WebHID flasher), the net host tests, the JS id table and the `K2_NET=1` variant build |
| `python3 tools/gate.py --static` | The gate plus `tools/static_check.sh sanitize`: every host test under ASan + UBSan, zero `src/` reports |
| `python3 tools/gate.py --update-baseline` | Rewrite `tools/size_baseline.txt` from the current build (do it deliberately, in its own commit) |
| `make stack-report` | Worst-case stack depth per entry point (2.4) |
| `make smoke` | Gate, then `--auto-dfu` flash and a read-only exercise of the device; results in `build/smoke/<hash>/` |

Write the host test first for pure logic (`tests/test_<name>.cpp`; add the source to `TEST_CXX_LIB` in the
`Makefile` if it is not header-only). Hardware-facing code is verified on the device. Commit format:
`<type>(<scope>): <summary>` plus Summary, Rationale, Key Changes sections ([agents/COMMITS.md](agents/COMMITS.md)).

## 7. How to add things

**A new page or submenu** (`src/ui/ui_pages.cpp` and `include/k2/ui/ui_pages.hpp` are shared: add, do not reorder)

1. Pure model first (selection, layout, text) in a header, with `tests/test_<name>.cpp`.
2. Page class: `class XPage final : public PageBase` (`draw_static`, `update`, `on_event`, `labels`,
   `list_state`, `leave`) in `include/k2/ui/ui_pages.hpp` or its own `include/k2/ui/ui_x.hpp` + `src/ui/ui_x.cpp`; add
   the `.cpp` to `TEST_CXX_LIB` if host UI tests link it.
3. Register: a member in `Ui` (`include/k2/ui/ui.hpp`), the `current()` / `menu_page()` / `list_state` switches in
   `src/ui/ui.cpp`.
4. Rotation page: add to `enum class Page` (before `Count`) in `include/k2/ui/ui_types.hpp`; submenu: add to
   `enum class Menu` and open it with `Ui::push_menu` (Tools entries: `tool_menu()` in `src/ui/ui_pages.cpp`; Settings: the
   `push_menu` call in `SettingsPage`).
5. Names: `PAGES` in `tools/k2_remote.py`, `PAGES` / `MENUS` in `tools/web/k2proto.js`, the copy in
   `tools/web/k2.html` (`make web-sync`); `tests/test_page_names.py` fails when they differ. Update the
   page counts in this file and `ROTATION_PAGES` in `k2_remote.py` if the rotation grows.
6. Use `ListView`, `ui_labels` and `ui_style` rather than new geometry; check the screen with
   `k2_remote.py screenshot` and the host panel dumps (`K2_DUMP`).

**A new fast-charge protocol**

1. Pure frames / step lists in `include/k2/fastcharge/<proto>.hpp` (timing from docs/FASTCHARGE_PROTOCOLS.md);
   tests replaying the steps on a fake clock (`tests/fc_fake.hpp`).
2. Add the enum value to `fc::Proto` (`include/k2/fastcharge/fc_engine.hpp`) and the engine state in `src/fastcharge/fc_engine.cpp`;
   voltage rules in `fc_safety`; the scan order in `fc_scan`.
3. Hardware binding in `fctrig` / `dpdm` (D+/D- levels) or `uart2` (UFCS / VIVO); keep the `link_guard` check.
4. Charger test: add `Algorithm` / `Group` values in `include/k2/chargertest/charger_test.hpp` and the step builder in
   `src/chargertest/charger_test.cpp`; extend `report_card` if the card records it (format version bumps with the magic).
5. Surfaces: trigger page (`ui_trigger.hpp`), remote `FC` payload (`remote_proto.hpp`), lab grammar (`lab.hpp`),
   docs/PROTOCOL_LAB.md.

**A new remote command**

1. Take the next free id in the subsystem range declared in `include/k2/core/remote_proto.hpp` (core 0x01-0x1F, debug
   0x20, system 0x30, pd 0x40, tools 0x50, storage 0x60, blackbox 0x70); add it to `enum class Cmd` and `kCmdIds`.
2. Encode / decode in `remote_proto.hpp` (pure, little endian, 64-byte frame: at most 59 request and 58 response
   payload bytes) and the `case` in `handle_request`; extend the `Backend` concept if it needs the device.
3. Implement the backend in `src/core/remote.cpp` (`Remote`), and in `tests/fake_remote_backend.hpp` so the dispatcher
   and HTTP tests compile; add `tests/test_remote_proto.cpp` cases.
4. Mirror the id and name in `tools/k2_remote.py` and `tools/web/k2proto.js` (`tools/web/sync_proto.py` helps);
   `tools/check_cmd_ids.py` (part of `make gate`) fails on a collision or an id outside every range.
5. Anything that drives VBUS, D+/D- is refused inline and above 5 V needs the confirm flag; anything that
   transmits on CC (PD messages, SOP' cable reads) is refused inline outright - a confirm does not override
   that, an inline K2 has no PD role of its own. Add the CLI subcommand with `--confirm`. Document it in the
   "Protocol" section below.

**A new driver or module**

1. Pure logic first: header or hardware-free `.cpp`, `tests/test_<name>.cpp` (`tests/test_util.h`), fail before it exists.
2. Hardware: `include/k2/<group>/<name>.hpp` (namespace `k2::<name>`, a header comment saying what and why) +
   `src/<group>/<name>.cpp`; registers through `k2::reg`, pins through `k2::pins` (new pins go into `pins.hpp` with their
   physical function). Call its `init()` from `main` in the right place and its `poll()` from the 20 ms block.
3. A new persistent file: pick a path on the littlefs volume, use `fs::Fs` with a magic + version + checksum, treat a
   missing or damaged file as defaults, document it in the file table below. A new raw flash region is almost
   never right; if it is, add it to `flash_map.hpp`.
4. A new log line: `K2LOG` with a `[TAG]`. A new RAM buffer over a few hundred bytes: check `make stack-report`
   and the size / stack gates.
5. `make gate`; record the `size` delta in the commit body.

---

# Reference

The sections below are the detailed reference for individual subsystems.

## Module map

One namespace per subsystem, headers in `include/k2/<group>/`, sources in `src/<group>/`.

| Namespace / file | Role | Depends on |
| :--- | :--- | :--- |
| `k2::reg` (`regs.hpp`) | Typed register blocks at fixed addresses (`Crm`, `Acc`, `Scb`, `Nvic`, `Spi`, GPIO `Pin`) | - |
| `k2::pins` (`pins.hpp`) | The one pin map, each pin commented with its physical function | `regs` |
| `startup.cpp` | Vector table, meta sections, `Reset_Handler` | `regs` |
| `k2::crm` | 240 MHz PLL, APB2 120 MHz, APB1 120 MHz | `regs` |
| `k2::systick` | 1 kHz tick, `millis`/`micros`, busy-wait delays | `usb` (service hook) |
| `k2::i2c` | Two bit-banged buses: FUSB302 (PB10/PB11), power monitor (PB8/PB9) | `pins`, `systick` |
| `k2::lcd` | ST7789V 240x240 on SPI3, streaming pixel writes | `pins`, `usb` (service hook) |
| `k2::accelerometer`, `k2::orientation` | Gravity sensor (LIS2DH12 / SC7A20), bounded I2C polling, pure debounced screen orientation | `i2c`; pure orientation has no hardware access |
| `k2::power` (`power_calc`, `power_monitor`) | INA228/226/237 detection and readout, unit conversion; display smoothing (`smoothing.hpp`, pure One Euro filter) | `i2c` |
| `k2::pd` (`pd.hpp`) | Pure PD decoding: PDOs, e-marker VDOs, message names | - |
| `k2::fusb302` | PD PHY driver: CC detection, Source_Capabilities, e-marker scan | `i2c`, `pd`, `usb` |
| `k2::fmt` (`fmt.hpp`) | printf-style formatter with width and zero pad, no libc stdio | - |
| `k2::usb` + `usb_descriptors.cpp` | TinyUSB glue, HID log channel, descriptors | `fmt`, `systick` |
| `k2::flash` (`flash.hpp`, `flash.cpp`) | Flash controller restricted to an allow-list of the DFU marker word; programming runs from RAM (`.ramfunc`) | `regs`-free (raw volatile) |
| `k2::dfu` (`dfu.hpp`, `dfu.cpp`) | `enter()`: log, flush, clear marker, verify readback, system reset | `flash`, `usb`, `systick` |
| `k2::fault` (`fault.hpp`, `fault.cpp`) | Fault handlers: `[FAULT]` log lines (stack overflow = `SO`), then detach + reset; `DEBUG_FAULT` trigger | `usb`, `crm`, `systick`, `mpu` |
| `k2::mpu` (`mpu.hpp`, `mpu.cpp`, pure maths in `mpu_calc.hpp`) | The MPU no-access stack guard region (`guard`, `disable`) | none |
| `k2::emrec` (`emrec.hpp`) | Pure: 66-byte e-marker record, library table codec, text decode | - |
| `k2::vfat`, `k2::drive` | Pure: FAT16 synthesizer and the file table / text of the USB drive | `emrec`, `textbuf`, `fmt` |
| `k2::store`, `k2::storage` | Guarded flash store over `flash` (header-only `Store<Hw>`; `storage.cpp` is the real-flash wrapper) | `flash`, `emrec`, `fusb302`, `usb` |
| `k2::emk_spoof` | E-marker emulation: pure `respond()` (quirk rows in order, first match wins, then the implicit Discover Identity answer, then the unmatched policy), plus arm/disarm over `fusb302::set_sop_prime_hook()` / `send_sop_prime_frame()`; unmatched SOP'/SOP'' requests are logged as `[EMK] unmatched ...` and the last 4 are kept (`emk_quirk::SeenLog`, remote EmLib op 7) | `emrec`, `emk_quirk`, `fusb302`, `usb` |
| `k2::emk_quirk` | Pure per-cable quirk table (Linux-kernel style): a `Row` is sop + request match (header value/mask, up to 2 data-word value/mask) + reply (Ack / Nak / Busy / Raw words); `Quirks` = flags (`kQSopPP`, `kQMsgIdEcho`), unmatched policy (Ignore / Nak / NotSupported), up to 8 rows; block codec (3-byte head + 52-byte rows, 419 B max); built-in rows for the two OnePlus cables | none |
| `k2::emk_learn`, `emk_learn_hw` | Learn: pure plan (Discover Identity, Discover SVIDs, Discover Modes per SVID, seeded vendor commands, optional header sweep) that records every reply as a quirk row; `emk_learn_hw` runs it over `fusb302::cable_session()` / `cable_exchange()` (32 ms reply timeout, blocks the main loop for the run) and keeps one transcript for `emarker learn` / `learned` / `save` (EmLib op 6). RAM: the learned record and quirk table live in the armed spoof's own buffers (`emk_spoof::scratch_record()` / `scratch_quirks()`; learn is refused while a spoof is armed, and arming afterwards invalidates `last()`, so save a clone before arming), and the transcript is a packed 256-byte buffer (`Result::entry(i)` decodes), so the tool costs about 280 bytes of static RAM; the stack budget gate has little slack | `emk_quirk`, `fusb302`, `storage` |
| `k2::msc` (Drive modes) | TinyUSB MSC callbacks served from `drive`; rebuilds the volume after a store change | `drive`, `storage`, `power`, `fusb302` |
| `k2::net` (HID+Net mode, `K2_NET=1` only) | lwIP netif over CDC-NCM, DHCP server, `[NET]` counters | `usb` |
| `k2::http_server` (HID+Net mode, `K2_NET=1` only) | lwIP raw-TCP HTTP on port 80 over the `k2/net/http_api.hpp` routes, gzip dashboard from flash (`tools/embed_web.py`), `[HTTP]` counters | `net`, `remote`, `capture` |
| `k2::pd_phy`, `pd_passive`, `pd_link`, `link`, `link_guard` | PD PHY register profiles, the passive-mode policy, attach/detach and inline-load detection (see "PD driver model") | `fusb302`, `pd`, `power` |
| `k2::pd_sink`, `pd_identify`, `pd_engine`, `pd_ext`, `pd_vdm`, `pd_source` | Trigger engine (Request, PPS/AVS, voltage-cap safety), Identify sequencer, read-only decoder, extended messages, VDMs and SVID table, source-cap storage | `fusb302`, `pd` |
| `k2::pd_rescan`, `pd_ccscan`, `pd_emarker_plan` | Rescan policy, CC pin choice, e-marker mode (Vconn or Sniff) | `pd`, `fusb302` |
| `k2::pdtrace`, `pdtrace_link`, `pdtrace_view` | Black Box: RAM ring of binary records, trigger-frozen captures, remote 0x70, link summary text | `fusb302`, `power` |
| `k2::bbstore`, `bbpersist`, `lz` | Black Box flash ring (pure sector/entry layout), quiet-link writer, LZ4 codec | `flash`, `pdtrace` |
| `k2::emlib`, `emrec`, `emname`, `emjson` | E-marker library (files in /emk), 66-byte record codec, auto names, JSON for the drive. Files are K2E1 (98 bytes: the 66-byte record) or K2E2 (the same 98-byte head with magic K2E2 followed by the quirk block of `emk_quirk`); old K2E1 files still load with no rows, firmware older than K2E2 skips K2E2 files. Native pages stay read-only | `store`, `flash` |
| `k2::usbcfg`, `usb_pma` | Stored USB link setting (/usb.cfg on the littlefs volume) and the packet-memory budget per mode | `flash`, `usb` |
| `k2::session`, `session_store` | PD session record kept in backup registers across resets (see "Session record") | `pd_identify` |
| `k2::bootmode` | Self-powered boot modes (PLUS / OK held at reset): Rd on CC2 / CC1 | `fusb302`, `buttons` |
| `k2::bootinfo`, `bootinfo_hw`, `bootscreen`, `bootscreen_hw`, `qr`, `qrshare`, `qrshare_records`, `qrview` | What this boot decided, as data: the K2B1 text (firmware identity first, then the key mask, pin levels, the one Rd move, SWITCHES0 readbacks at ~50 ms / ~600 ms / ~3 s, the first FUSB302 writes), the remote command 0x35 and the LCD screen (QR code + three lines). `qr` is a thin face on the vendored Nayuki generator (`third_party/qrcodegen`), `qrshare` the generic share frame (record types, CRC32, base45, multipart parts, and the host-side parser/assembler; `docs/QR_SHARE.md`), `qrview` the QR layout and drawing on the panel. See "Boot info" | `fusb302`, `boottrace`, `adc`, `power`, `lcd`, `scratch` |
| `k2::twin` (`twin`, `predict`, `library`, `learn`, `usbdata`, `present`, `live`, `wire`), `twin_hw` | Device twin, see `docs/DEVICE_TWIN.md`. Pure: the profile and `predict()` (the contract a stored device would get from a source list, and what limits it), the 40..129-byte "K2T" record (v1 and v2, CRC16) and learning/merge rules, the USB-data decoders, the one-line texts, the live-proof `Runner` over an `Env` seam, and the remote wire codec (`Cmd::Twin` 0x63). `twin_hw` is the glue: the flash library (`/twin/NN.twn`, 12 twins, via `k2::storage`), learning from `fusb302::identify_status()` or a sniffed Request, and the live proof over `fusb302` (the Get_Sink_Cap override seam, `sink_request`, `sink_release`). The page is `TwinPage` (`ui_twin`), the shell commands live in `shell.cpp` / `shell_hw.cpp`. A live run is a test for the stop report (`BusyWhy::TwinRun`) | `pd`, `pd_sink`, `pd_identify`, `usb_link`, `fusb302`, `storage`, `power` |
| `k2::cable_r` | Passive cable resistance estimate (idle vs load) | `power` |
| `k2::dpdm`, `qc`, `afc`, `accp`, `vooc`, `uart2`, `fc_engine`, `fc_scan`, `fc_safety`, `fctrig`, `fastcharge_classify` | Fast-charge: D+/D- matrix, QC/AFC/FCP/SCP/VOOC, one-click scan, trigger safety rules, passive classifier | `i2c`, `power` |
| `gfx`, `text`, `blend`, `ui*`, `buttons` | Renderer, fonts, UI pages, button state machine | `lcd`, drivers |
| `k2::pdstress` (`plan`, `engine`, `score`, `dist`, `result`, `journal`, `digest`, `store`) + `PdStressSession` + `PdStressPage` | PD Stress: renegotiation stress of the attached charger, scored (0 = ideal). Pure plan / engine / score / histograms / result blob / reboot journal over an `Env` seam; the session binds it to `fusb302::sink_request`, the power monitor and the file system, lends the charger test's record buffer and puts one footer line on the page that is up. `digest` reduces a result blob to per-sequence verdicts, `store` keeps the newest 8 results the user saved (`/ps/xxxx.psr`). The page (Tools > PD Stress, `Menu::PdStress`, `src/ui/ui_pdstress_page.cpp`) is a two-step wizard (Sequences, Review), then Running, Results (Save result / Discard) and a per-sequence Detail; the run stays in the session, so it goes on with the page closed. Shell first: remote command 0x55, CDC `pdstress`, host `tools/pd_stress.py` (run, rescore, report). See `docs/PD_STRESS.md` and `docs/UI_DESIGN.md` 4.5 | `pd`, `fusb302`, `power`, `bbstore`, `ChargerTestSession` (record buffer) |
| `k2::loopprobe` (`loop_probe`, `loop_cc`) + `loopprobe::hw` + `LoopProbePage` | HID-cable loop probe, a 5 V research lab (`docs/LOOP_PROBE.md`). Pure: the 399-byte record and its layout, the flash file (`LPR`, CRC-16), the boot check that closes a run a reboot cut in the CC step, and the step `Runner` over an `Env` seam. `loopprobe::hw` binds it to the power monitor, ADC, USB line state, backlight, `dpdm`, the FUSB302 sniff and `fusb302::loop_cc_hold`, and keeps `/lp/last.rec`. The page (Tools > Loop probe) owns the runner, `Ui::tick` polls it, `BusyWhy::LoopProbe` keeps other tools out; the result goes out as the LOOP share type and through remote 0x57 (read only) | `power`, `adc`, `usb`, `lcd`, `dpdm`, `fusb302`, `fs` |
| `main.cpp` | Bring-up order and the service loop | everything |

Pure logic (`pd`, `fmt`, `power_calc`, `net_frame`, `ui_calc`, `ui_layout`, `text`, `blend`,
`buttons`) has no hardware access and is covered by host tests.

## USB and logging path

- One build switch, `K2_NET` (Makefile, default `0`, `include/k2/core/build_config.h`): the stored USB link setting (`k2/drivers/usbcfg.hpp`, /usb.cfg, Settings -> USB Link, applied on replug) selects the descriptors at boot: off, HID, Drive, HID+Drive, and, only in a `make K2_NET=1` build, HID+Net. The default build has no network: lwIP, TinyUSB's NCM class, `net.cpp`, `http_server.cpp`, the DHCP server and the embedded dashboard are not compiled, which frees about 8 KB of the overlay (see the RAM map) and about 70 KB of flash. A stored config that asks for Net is normalized to off with a `[USBCFG] net not built` log line (the record stays on flash, so a later K2_NET=1 build honours it), and the USB Link page shows two rows instead of three. The web dashboard (`tools/web`) and the `/api/*` bridge below need `make docker-build K2_NET=1`. `make gate` builds that variant too, in `build-net/`, so it cannot rot; WebHID and the CDC shell are unaffected.
- `usb_descriptors.cpp`: VID 0x0716 / PID 0x5060, HID interface 0 with 64-byte IN and OUT reports on EP1 (the logger protocol the host tools expect). Drive appends a Bulk-Only MSC interface (EP2 OUT 0x02 / IN 0x82, 64 B bulk); `tests/test_usb_desc_msc1.cpp` / `_hidonly.cpp` compare the HID bytes with golden bytes captured from the pre-MSC build. Net appends a CDC-NCM function (IAD, interfaces 1 and 2, notification EP2 IN 0x82 16 B, data EP3 OUT 0x03 / IN 0x83 64 B) and changes the device class to EF/02/01; strings 4 (name) and 5 (MAC) exist only in this mode (`tests/test_usb_desc_net.cpp`). All of the Net descriptor code is under `#if K2_NET`; the host tests build it (`HOST_K2_NET = 1`) and `tests/test_usb_desc_nonet.cpp` checks the K2_NET=0 descriptors.
- Why NCM (and not RNDIS+ECM): Linux `cdc_ncm`, macOS and Windows 11 `usbncm.sys` bind to one NCM configuration with no BOS / MS OS 2.0 descriptor, no vendor request and no INF. RNDIS+ECM covers Windows 10 as well but needs two configurations and the BOS machinery. Windows 10 is the price.
- Network mode (K2_NET=1 builds only): the host gets 192.168.7.2 by DHCP (server 192.168.7.1, no router, no DNS, mDNS skipped: it needs IGMP). Port 80 serves the dashboard (gzip, in flash) and `/api/state`, `/api/pdos`, `/api/ping`, `/api/cables`, `/api/press`, `/api/blackbox`, `/api/fc`, `/api/pd/*`, `/api/screenshot.bmp` and the `/api/frame?hex=` bridge to the remote dispatcher. Every POST needs an `X-K2: 1` header (a cross-site form cannot set it); calls that change VBUS also need `confirm=1`, and the shared dispatcher keeps its own confirm rules. The page uses the HTTP transport of `k2proto.js` when loaded from the device, WebHID otherwise. Routing and JSON are pure code in `k2/net/http_api.hpp` (`tests/test_http_api.cpp`). RAM is static: 3 connections (request line plus a flag each), a 2 KB JSON buffer, lwIP pools (MEM_SIZE 6144), all in the `.mode_net` overlay member (present in every mode of a K2_NET=1 build, absent from the default build).
- Packet memory (512 B), per mode, checked by `static_assert` in `k2/drivers/usb_pma.hpp` and by `tests/test_usb_pma.cpp` (walks the served descriptor): HID 320 B (64 BTABLE + 128 EP0 + 128 EP1); Drive 320 B; HID+Drive 448 B; HID+Net 464 B (EP1 128 + notification 16 + 2 x 64); with the debug shell (below) HID+Shell 360 B and HID+Drive+Shell 488 B (+40 B). Net+Drive would need 592 B: `usbcfg::normalize()` turns it into off, and the USB link page never offers it.
- The USB ISRs only feed TinyUSB's event queue; callbacks run from `usb::poll()`, so the log queue is single-context and lock-free.
- `usb::log` / `usb::log_fmt` copy one line (max 63 bytes) into a 16-slot queue of 64-byte reports and call `flush()`. A full queue drops the oldest line (`[USB] drop=`).
- Fixed-format lines use `K2LOG("[TAG] fmt\n", args...)` (`include/k2/core/log.hpp`): the format stays out of flash, the device sends `msg#<id> args` (%d decimal, %u/%x/%c hex, %s %XX-escaped). The build (`tools/logdb.py gen`) gives each format a stable ID in the append-only `tools/logdb.json`; commit it with the source. `k2_flasher.py monitor` and `proto_lab.py` decode on the fly; for LOG.TXT, SAVEDLOG.TXT or a CDC shell `log on` terminal use `python3 tools/logdb.py decode FILE` (or pipe stdin). Unknown IDs stay `msg#<id> args`. Runtime text (`[FAULT]`, `[DFU]` say(), computed strings) still goes through `usb::log`.
- **Debug shell (CDC-ACM).** The HID and HID+Drive modes also carry a serial port (never Net or Drive-only; derived by `usbcfg::with_shell`, no setting of its own). Endpoints: notification EP4 IN 8 B, data EP5 OUT/IN 16 B bulk (the PMA budget forbids 32 B beside the drive); RAM about 450 B (TinyUSB CDC FIFOs). Connect: `picocom /dev/ttyACM0` (DTR must be asserted; the left port needs a C-to-A cable). The line parser is `k2/core/shell.hpp` (pure, `tests/test_shell.cpp`); `src/core/shell_hw.cpp` maps commands onto `Remote::fc_cmd`, `Remote::pd_hard_reset`, `fusb302` and `dpdm`. Commands: `help`, `state`, `log on|off`, `dp|dm hiz|0|0.6|3.3`, `cc open <ms>`, `pd silent <s>`, `pd reset`, `pd rescan`, `fc trigger <proto> <mV> [confirm]`, `fc reset`, `stop` (the remote STOP: test, FC, PD to 5 V, spoof; no confirm), `sniff stream|stop`, `trace stream [all]|stop` (Black Box records as binary frames, see "Black Box"). Safety as the remote: a target above 5000 mV and the raw 0.6/3.3 V levels need a trailing `confirm`; the engine enforces the monitor ceiling and the inline refusals. Closing the port, unplug or suspend resets a shell-started trigger, ends `pd silent` and returns raw D+/D- to HiZ plus a line reset (5 V, passive idle). `log on` streams each chip log line (dropped when the 256 B TX FIFO is full). The protocol lab (`k2/core/lab.hpp` grammar, `tools/proto_lab.py` client) adds the accp/qc/afc/fcp/scp/vooc/ufcs/vivo/dpdm/vbus/pd command family: see docs/PROTOCOL_LAB.md.
- Debug has no other channel: SWJ is disabled because PA13/PA14/PA15/PB3/PB4 are buttons and LCD lines. Add a `[TAG] ...` log line to make a hypothesis observable.

## PD driver model

The FUSB302 is passive by default: on the meter screen the chip only listens, with Auto-CRC off and SOP, SOP' and SOP'' receive enabled, so a charger-to-device conversation is decoded without the K2 answering and without disturbing it. The register profiles live in `pd_phy`; `pd_passive` holds the policy for that mode and `pd_link` the attach, detach and inline detection (a device load inline with a source) that decides what the K2 may do. `link` / `link_guard` refuse any active operation while a live sink or an inline load makes it unsafe.

**Passive mode means no CC transmit at all.** While passive, the driver never puts a frame on CC: no GoodCRC auto-reply (Auto-CRC stays off), no Reject/Accept/Soft_Reset answer, not even a Hard Reset — including the window before the inline/topology decision and every CC re-selection, where a link between two other devices is live. The only sends come from an active user feature (trigger, charger test, spoof, Read cable, Identify, probe, re-read, Get_Source_Cap, VCONN swap, self-powered sink mode). The choke point is the driver's `transmit()` / `send_sop_prime_frame()`, which refuse and log (`[FUSB302] TX refused (passive)`) whenever no active feature demands the wire: the flag is the same `pd::crc_demand` that drives Auto-CRC, recomputed in `active_tick()` (default false, cleared on detach/init).

**Who may keep Rd (CC pull-down) presented.** Rd is demand-driven (`pd::rd_demand`, `pd::rd_lines`); Auto-CRC is a wider demand (`pd::crc_demand` = Rd demand plus the armed e-marker spoof, which needs the GoodCRC but must never present Rd). Each owner has a lifetime and none may outlive it:

| Owner | Lifetime |
|---|---|
| Boot-mode pin (PLUS / OK held at boot) | the session; `cc_detach` refuses it. Written by `self_power_early` right after `i2c::init` (SWITCHES0 = the pin's Rd bits, both CC switches closed, PC13 low) |
| Inherited pin (plain boot after a warm reset, `sysreset::warm`, AND the pin mark says the previous boot held a pin) | the pin mark (backup register DR21, `k2/drivers/pin_mark.hpp`, encoding in `bootmode::pin_mark`) is written with `g_boot_pin_rd` by `set_boot_pin` and cleared by `self_power_release()` and the factory wipe. A charger-powered K2 that resets keeps its Rd; a HID-powered one (no pin, so no mark) boots passive after a cold start, a flash, `reboot` or a watchdog. The FUSB302's own SWITCHES0 is never read for this decision (after a flash it holds the power-on 0x03 or a reread's Rd). `self_power_release()` ends the pin when no run resumes on it (no journal, BACK held at boot, a resume abandoned). `self_power_early` writes SWITCHES0 before it closes the CC switches, so the chip's power-on Rd never reaches the connector |
| Run Rd (`rd_for_run`, unit attach, the re-read's own attach) | until `rd_for_run(false)` / unit detach / the re-read release (30 s after a finished run, at once on abort or failure). The re-read release only ends an attach nobody took over: `reread_take_over()` (called by `sink_request` on every accepted Request, a PD Stress run start and a twin live start; `rd_for_run`, `unit_*` and `cc_detach` clear the flags too) hands the attach to the new owner, and the timer never fires on it. It never fires while a run owns the line (`g_run_owner`) either; its linger starts over when the run ends (hardware 2026-11-11: it cut a PD Stress run at a Soft_Reset step) |
| Sink operation in flight (Request, transition, abort, EPR, Identify, probe, re-read, prepare, Get_Source_Cap, VCONN swap) | while active plus the 300 ms Auto-CRC linger |
| Held trigger contract | until `pd release` / detach; never times out. A user trigger on a contract a re-read made takes the re-read's attach over (`reread_take_over`), so the re-read's 30 s release is cancelled for good: after the `pd release` the 5 V contract and the Rd stay up until `pd-reread --abort` (see the paragraph below the table) or a detach |
| Ready, unheld contract | `kSinkIdleLingerMs` (30 s) after the last engaged pass, then `contract_expired`: release to 5 V if the contract is not 5 V SPR, else detach the sink (Rd off) |
| Armed e-marker spoof | Auto-CRC only, never Rd |
| Forced line, Ra mode, passive scan | scan restrictions, never an Rd owner outside their run; cleared by `reread_abort` / `cc_detach` |

`fusb302::reread_abort()` (`pd-reread --abort`, PdReread 0x47 op 2) is the one lab release: it stops a re-read and ends the attach whoever made it. The re-read's own attach goes on the housekeeping release path at once; an attach a lab command took over (`pd attach` / `pd request`, a finished re-read's Rd, also after `pd release`) has no owner flag left, so the abort adopts it (`[REREAD] abort: lab-held attach adopted`: the run-start Rd and the Ready contract are marked the re-read's and released the same way, back to 5 V first when the contract sits at another voltage). Refused with `false` (the remote answers Busy, nothing changed) while a run owns the line (`g_run_owner`, or `Ui::ports_busy()`: charger test, PD Stress, twin run) and inline (a real sink's Rd must not be touched). The boot-mode pin and a running Identify / probe are their own owners and stay. It was extended rather than adding `stop --detach`: STOP (0x4B) is the host's dead-man 5 V release and must not take Rd away from a link a tool is using, while "end the lab attach" is exactly what `--abort` already promised (host tests `test_abort_*` in `tests/test_fusb302_scan.cpp`).

`fusb302::cc_detach()` (`debug detach`) is the real CC-level detach: every owner above is dropped, the sink forgotten, and Rd stays off for `kRdHoldMs` (1.5 s, longer than the source's detach debounce) before the idle Rd returns.

**When the unattached CC scan runs** (`detect_cc_orientation`, paced by `pd::ScanPace` in `k2/pd/pd_scanpace.hpp`, host test `tests/test_scan_pace.cpp`). Every `kScanPeriodMs` (500 ms) while no line is attached, and on the next poll, at least `kScanKickMinMs` (60 ms) after the last scan, after a kick: a no-Rp detach (`DetachReason::Cc`), VBUS rising through 3.5 V (`sink_vbus`), or the ADC Rd window naming a new line (`sink_cc`); only edges seen while unattached count. Every detach first restarts the period (no scan runs while attached), so a VBUS-low or dark detach, whose Rp is still there, waits a steady 500 ms before the same line is latched again (the old 25-poll tick gave 0..500 ms by chance). A kick only moves the scan earlier; the Rd hold and the idle wake's reading window still gate it, and it never changes what the scan picks. Why: a source sends its offers within a few hundred ms of seeing a sink's Rd; at the old fixed 2 Hz the K2 routed its receiver after Source_Caps / Request / Accept had passed, decoded PS_RDY first, had no caps and fell back to "USB DATA" (NOVOO -> K2 -> Legion side port, 2026-11-11). The Rd-window re-check of an attached line (`cc_window_recheck`, `pd::rd_window_override`) also moves a line that itself reads low (no Rp) to the other net when that net sits in the window, confirmed by the comparator; the scan's own first choice (`pd::rd_window_line`) still treats a low other net as ambiguous. Inline, the sink's second Rd grounds the unused net through the K2, so "one net in the window, the other at 0 mV" is the normal look of a latch on the wrong line.

Active operations exist for three things only:

1. **Trigger** (`pd_sink`): Request for a PDO or a PPS/AVS voltage, GoodCRC and PS_RDY handling, release to 5 V, hard reset. The voltage cap is checked in the engine, not only in the UI. When the source advertises SVID 0x2717 (Xiaomi), `pd_sink` also handles the Xiaomi Mi Turbo Charge authentication handshake (unstructured VDM sequence 0x27170201..0x2717020A) and sends the correct responses, after which the charger unlocks extended PPS ranges (20 V 6 A / 120 W on CUKTECH 10, 20 V 10.5 A / 210 W on larger units).
2. **Identify** (`pd_identify`): a sequencer that asks the partner for Discover Identity, SVIDs, modes and Get_Sink_Cap and records which steps answered.

Everything else (`pd_engine`, `pd_ext`, `pd_vdm`) decodes what is seen. Message preambles matter: SOP (the partner) is used for the trigger and Identify, SOP' (the cable plug) for the e-marker. `pd_emarker_plan` picks the e-marker mode: Vconn (nothing attached or VBUS under 800 mV: the K2 powers the cable and sends SOP' Discover Identity) or Sniff (a source is attached: the K2 only listens for the identity it requests). Rescan policy and CC pin choice are in `pd_rescan` and `pd_ccscan`.

## HID-cable loop guard (the 5 V lock)

The hazard: the K2 on a charger, and a cable from its PD-bus port back into its own left (HID) port. The HID port is USB 2.0 and 5 V only, and its VBUS then follows the PD bus, so any contract or trigger above 5 V puts 9-36 V into it. The guard (`include/k2/core/loop_guard.hpp`, pure `Detector` and `Enforcer`, host test `tests/test_loop_guard.cpp`; the hardware half `src/core/loop_guard.cpp`) sees the loop from three signals the K2 already has, and while it holds the lock nothing may raise VBUS above 5 V. Not verified on hardware yet (ROADMAP).

**Signals, sampled every 20 ms** (`loopguard::poll(now)`, one call in `slow_tasks` of `src/core/main_loop.cpp` right after `adc::poll`, before `Ui::tick`, so the tools see the lock in the same pass): the aux rail (`adc::Snapshot::aux_mv`, PA0 behind the 11:1 divider: the HID port's VBUS), the PD-bus VBUS (`power::vbus_fast_mv()`), and `usb::host_active()` = started AND configured AND not suspended. TinyUSB has no VBUS sensing here, so after a PC is unplugged the device stays "mounted" but suspended; a stale "configured" must neither block the suspicion nor clear a lock. A negative reading (no snapshot, no monitor) counts as absent, never as a loop.

**Rules (`Detector`):**

| Rule | Condition | Effect |
|---|---|---|
| Certain | aux > `kCertainMv` 5.8 V (no USB 2.0 host gives that: 5.25 V max plus divider tolerance) | locks on the first sample |
| Suspected, "the rail follows VBUS" | aux crosses `kAuxPresentMv` 4 V up within `kRiseWindowMs` 100 ms of VBUS crossing `kVbusPresentMv` 4 V up (either order), no active host | locks at once (no waiting window) |
| Suspected at boot | the first sample with both readings known already shows both up, no active host (no order known) | locks at once (the safe side) |
| Clear: separate supply | aux >= 4 V while VBUS < `kVbusOffMv` 1 V for `kSeparateMs` 60 ms (VBUS cut, the rail stays: a loop cannot do that) | unlocks |
| Clear: rail gone | aux < `kAuxGoneMv` 3.5 V (`rdintent::kHostAuxMv`, the HID port is empty) for `kClearMs` 1 s | unlocks |
| Clear: host | an active host for 1 s with aux <= `kHostMaxMv` 5.5 V | unlocks; a host never clears a lock while aux is above what a host gives |

Why this rule: in a loop the HID rail is fed from VBUS, so the two rise and fall together. A passive K2 presents no Rd, so the charger only switches VBUS on when the loop cable brings the HID port's Rd: the K2 then boots with both rails up, which the boot rule takes. Never suspected: a rail that was up before VBUS appeared (a PC plugged in first, a K2 running from a PC and then a charger attached, a charger that cuts VBUS while the rail stays, such as a USB-A port that switches off every ~6.5 s with no load) and VBUS up long before the rail (a charger first, then a PC or a 5 V brick on HID). That holds with the K2's USB link off (`usbcfg`, the default), where a PC never becomes an active host, so an HID-powered K2 runs charger tests unlocked. An earlier rule (both up with no host for 3 s) locked exactly that setup and was replaced (user decision 2026-11-11).

Both rails use hysteresis (up at 4 V, down below 3.5 V), so a sag into the band is no fall and its return is no rise. A reading that is unknown (negative) is skipped: it neither moves the order nor proves a clear. The certain rule's 5.8 V sits above the host clear band's 5.5 V. `events()` counts the lock events (None -> locked) since boot; `rise_gap_ms()` is the time between the two rises at the last both-up edge (-1 = no order known); `last_clear()` names what ended the last lock.

Gaps, by design: a loop cable plugged into HID while the K2 already sinks VBUS (Rd presented by a trigger, a run or a boot-pinned key) rises long after VBUS, exactly as a PC does, and is not suspected; the certain rule takes the first raise above 5.8 V, so up to one source transition (<= ~0.6 s) of the raised level reaches the HID port before the 5 V order. A K2 that boots with both rails up from a PC plus a charger with a sink on its output locks until the PC enumerates (link on) or VBUS is cut; with the link off it stays locked until then (the safe side).

**Choke points.** Every raise goes through one of these, and each refuses under the lock:

| Path | Where | Under the lock |
|---|---|---|
| PD request builder (auto-sink, trigger, re-announce, EPR entry) | `pd::Sink::set_hv_lock` / `plan_for`, `src/pd/pd_sink.cpp` | any plan above 5 V is `Err::OverCap`; no EPR entry; a held re-announce falls back to PDO 1. The lock edge releases a contract or a Request in flight above 5 V to PDO 1 and drops an EPR entry |
| PD driver request entry (lab `pd request`, Ui trigger) | `fusb302::request_inner`, `src/drivers/fusb302.cpp` | position > 1 refused with OverCap and a `[LOOP]` line; `sink_set_hv_lock` forwards the lock edges to the sink and aborts a prepare |
| PD trigger page | `Ui::trigger`, `src/ui/ui.cpp` | above 5 V refused before the confirm prompt (OverCap) |
| Legacy triggers and the legacy scanner | `fc::Env::hv_locked` (`fctrig.cpp` HwEnv), `fc_engine.cpp` validate / poll step, `fc_scan.cpp` start / poll | a target above `fc::kLockMv` 5 V is `Fail::OverLimit`; a running session with a target above 5 V or VBUS above 5.5 V aborts (0 V on D+/D-, which every legacy protocol answers with 5 V) |
| Charger test | `ct::Input::loop_lock`, `src/chargertest/charger_test.cpp` | START (and resume) refused with `Reason::Loop`; a run in flight fails with `Reason::Loop` at the next fault check |
| PD Stress | `Ui::tick` / `foreign_busy`, `src/ui/ui.cpp` | start refused (Busy, the lock text), a run cancelled (released to 5 V) |
| Twin proof | `make_input`, `src/twin/twin_hw.cpp` | busy while locked: no start, a run cancelled |
| Lab shell (D+/D- drive, raw levels) | `lab_guard`, `drive`, `src/core/shell_hw.cpp` | refused with the lock text; raw levels forced back to Hi-Z |
| Remote | everything above | the remote reaches the K2 only through a USB host, which clears or prevents a lock; the gates above cover the 1 s clear window |

Left alone, with the reason: the UFCS probe (5 V only by design), handback (the K2 raises nothing; the far side of a loop is the HID port's Rd, which never negotiates), `request_epr_info` (information only), the restart-abort and release paths (PDO 1 only), `pd-reread` (a 5 V contract), `lab_load` (current, not voltage), Data_Reset and the VCONN ops (no voltage), and the rd_comeback boot (below).

**Enforcement (`Enforcer`).** On the lock edge: the 5 V order. Most of it is the lock itself (the sink's edge release, the legacy engine's abort, the charger test's Reason::Loop, PD Stress and twin cancelled); `act()` adds `fusb302::sink_release()` when VBUS is above `kSafeMv` 5.5 V. VBUS still above `kStuckMv` 6 V `kOrderMs` 600 ms later (the SPR source transition is 550 ms at most): `fusb302::sink_hard_reset`. Still above 6 V `kResetMs` 1 s after that (tSafe0V is 650 ms): `fusb302::cc_detach()` (refused with a boot-pinned Rd or inline; the refusal is logged and nothing more is tried). A VBUS that came back to 5.5 V or less and rises above 6 V again re-arms the sequence.

**Visibility** (no new page or component): a Caution footer `HID-cable loop: 5V lock` on whatever page is up (not on the trigger menu, which owns its footer); the same text is PD Stress's busy reason, the shell's lab refusal and the charger test's `Reason::Loop` name (#47, the card's 6-bit reason field now holds 48 values). `GET_STATE` part 2 (diag) grows to 29 bytes: `loop` (bits 0-1 reason none / certain / suspected, bit 2 reserved 0, bit 3 host active) and `loop_events` (u16); `k2_remote state --json` decodes it as `"loop": {"locked", "reason", "host", "events"}` and an older 26-byte reply has no key. `[LOOP]` log lines: `lock <reason> aux= vbus= host= gap= n=`, `clear <separate supply|rail gone|host> aux= vbus= host=`, every enforcer step, a refused detach, a refused request, a refused trigger.

World replay: `tests/test_world_replay.cpp` runs the real guard in the reboot world. The `hr-session` `*-hid-loop` variants assert `[LOOP] lock suspected` in both lives and a lab-style PDO 2 request in life 1 that is refused (`[LOOP] request pos 2 ... refused`) with no Request above PDO 1 reaching the charger; the variants without the loop assert that no lock appears.

**Boot and `rdintent::host_powered`.** `rdintent::host_powered(aux_mv)` reads "aux up early in boot" as a PC on the HID port, and a host-powered comeback stays passive (`rd_comeback` resumes no session). Through a loop the aux rail is the PD bus, so a comeback boot after a Hard Reset in a loop is judged host-powered and stays passive. That is the safe side, and the guard neither relies on it nor changes it: aux up at boot is no proof of a PC, which is why the guard's own clear needs an active (configured, awake) host, not just aux. A looped K2 that comes back passive boots with both rails up and is locked by the guard at its first main-loop sample (suspected, no order known).

## Boot info

A charger-powered K2 with no USB has no log, and the K2LOG lines of the first seconds are gone before a host attaches, so the self-powered boot decision (which key, which Rd, whether the single Rd moved, whether the Ra went on for a cable identity: `ra=<0|1>:<verdict>` from `bootra::last()` and `fusb302::ra_active()`, rules in `include/k2/pd/boot_ra.hpp`) is kept as data and shown two ways. The collector `bootinfo::collect` (`src/core/bootinfo_hw.cpp`) reads only the drivers' own records (`fusb302::boot_record` / `boot_note_at`, `boottrace::move_record` / `sample_count`, `sysreset`, `adc`, `power`); the pure formatter `bootinfo::format` (`src/core/bootinfo.cpp`) turns the `Snapshot` into one ASCII line, at most 240 characters, that starts with the firmware identity (`K2B1 fw=<version> g=<git describe, -dirty when dirty> p=<remote proto>`, the strings PING answers) and carries the fields documented in `include/k2/core/bootinfo.hpp`. The name K2B1 is also the Black Box file header's magic (`.bbe`); the two never meet (one is a text prefix, the other a file header).

- **Remote:** `BOOT_INFO` 0x35, `python3 tools/k2_remote.py bootinfo [--json]`. Request `part, off`; part 0 is the text in 48-byte chunks (the text is frozen at the first chunk so the chunks of one read agree), part 1 the first-seconds power samples (9 bytes each). The reply echoes part, off, total, count. The same ids are in `tools/web/k2proto.js`.
- **LCD:** in a key-mode boot (`bootmode::self_powered`, which a marker's comeback is not: it has no key) `main` starts a `bootscreen::Controller`, but the screen appears only when the key that chose the mode stays down for `kShowAfterMs` (3 s) from the reset. A key released sooner (an ordinary PLUS or OK plug-in) ends the Controller silently and the normal UI is on the panel from the start: no QR, no 30 s takeover. The key picks the mode at reset either way, and until it is up its own button events (Long, Hold = the reboot gesture) are swallowed in `main`, the other keys work at once. The first frame is drawn by the main loop (never from the boot path, so the SWITCHES0 write and the move rule keep their timing). Once shown, while the key is held, one frame every 500 ms; after release the last frame stays 30 s, then `Ui::repaint` hands the panel back. A frame is the K2B1 text wrapped as a `qrshare` BootInfo frame (`K2:BI1:<base45>`, see `docs/QR_SHARE.md`) and drawn by `qrview` as a QR code (alphanumeric mode, level L, automatic mask, version <= 10, at least 3 px per module, a 4-module quiet zone, black on white, centred) with three lines under it: `fw <version> p<proto>` (the version string carries the hash; the raw git is in the K2B1 text), `up <s> s  frame <n>`, `<mode>  Rd <from>><to> at <ms> ms` or `Rd <pin>, no move`. The K2B1 text, the frame text, the QR workspace and one pixel line (about 2.3 KB) come from the scratch arena for one draw. While the screen is up the UI neither ticks nor takes buttons, so nothing paints over the code; a short BACK press closes it early, and a remote screenshot request closes it first (a capture repaints the UI's fields over the code). The web dashboard's screenshot does not know about the screen: it shows the UI underneath.
- **Rate-limited log:** the `[PD] Source_Capabilities` K2LOG block of a source that repeats its caps every few hundred ms is logged once per 10 s per attach session (and at once on a change), with a count of the repeats held back (`pd::CapsLogGate`, `include/k2/pd/caps_log_gate.hpp`). The Black Box records every message regardless.

Host tests: `tests/test_bootinfo.cpp` (format), `tests/test_bootscreen.cpp` (layout rule, timeline, lines, and the whole path text -> QR -> panel pixels -> an independent reader, `tests/qr_decode.hpp`, -> the same text), `tests/test_qr.cpp`, `tests/test_caps_log_gate.cpp`, `tests/test_remote_proto.cpp`. The screen as the host test renders it: `docs/ui_mockups/bootscreen.png`.

## QR share

One generic way to hand a record to a phone or a host tool (`docs/QR_SHARE.md` has the frame, the schemas and the measured capacity).

- **Pure code:** `k2/ui/qrshare.hpp` (frame, CRC-32, base45, `Assembler` that joins parts in any order), `k2/ui/qrshare_records.hpp` (builders: `emarker`, `pdcaps`, `card`, `identity`, `boot`, each over an existing serialiser). Host-tested by `tests/test_qrshare.cpp` and `tests/test_qrshare_records.cpp`; `tools/k2_qr.py` is an independent Python reader checked against the golden vectors in `tests/fixtures/qrshare.tsv`.
- **Drawing:** `k2/ui/qrview.hpp` lays a symbol out inside an `Area` (`plan_in`, `draw_code_in`, `draw_line_in`), so the boot screen (whole panel) and the share view (content area beside the key labels, any rotation) use one drawer.
- **UI:** `SharePage` (`src/ui/ui_share.cpp`, `Menu::Share`, remote page 18) holds a static 510 byte record, splits it into at most 4 parts and shows one at a time (title, QR, `part i/n`). A multi part record cycles every 1.5 s; long [+] / [-] page, OK holds, BACK returns. Each paint takes the scratch arena, encodes the part, draws it and gives the arena back (the page keeps no encoder workspace, so it costs 510 bytes of RAM); nothing repaints between part changes, a capture or a resync repaints and encodes again. `Ui::open_share(type, schema, title, len, now)` is the only entry: the page that wants to share fills `Ui::share_buffer()` with a builder and calls it (long OK on the saved cable detail, the PD Source tab, a report card, the charger test Results).
- **Stack:** an opener sits on the deepest main-loop path (remote command, then `run_action`, then the page), so it keeps no big locals: the saved cable's quirk table is a static (about 420 bytes) and its entry is the page's `cur_`; a local copy added 584 bytes to the main-loop depth and broke the stack budget.

## Black Box

A recorder for the PD link, read-only with respect to it. `pdtrace` writes fixed binary records (PD messages with direction and SOP type, FUSB302 events, VBUS/IBUS samples, CC state) into a RAM ring; `tools/pd_trace.py` reads it over remote command 0x70 (`live`, `dump`, `status`, `clear`, `freeze`, `resume`, `captures`, `capture N`, `decode`). Triggers (contract change, Reject/Wait, hard or soft reset, VBUS dip, detach) freeze a window of records into a capture slot.

**Live stream over the CDC shell** (`trace stream [all]` / `trace stop`, `python3 tools/pd_trace.py stream`). A charger-test run keeps the HID port busy for minutes and the 8 KB ring wraps in seconds on a chatty source, so the new records are also pushed down the debug shell port, which is free. `k2/core/trace_stream.hpp` is pure code over a `pdtrace::Ring` (host test `tests/test_trace_stream.cpp`); it owns only a read cursor and a few counters (about 20 B of .bss, no buffer): a frame is built on the caller's stack straight from the ring bytes and the ring is the queue, so a slow host costs the device nothing. `shell_hw.cpp::poll` calls `Streamer::service` once per main-loop pass with the free room of the CDC TX FIFO: it sends whole frames only while a worst-case frame (91 B) fits and at most 4 per pass, never blocks, and does no work in an ISR (the record encoding and `pdtrace::push` are untouched; the ring only gained two 4-byte counters, `records_pushed` / `records_dropped`, so a loss is counted exactly). Frame: `0xA6, kind, len, payload, crc16` (CRC-16/CCITT-FALSE over kind, len, payload; shell text never contains 0xA6, a reader resyncs on it and drops a bad CRC). Kinds: Start (flags, cursor, head, tail), Data (ring offset, then whole records exactly as the ring lays them out, so the host decodes them with the `dump` / `live` decoder and `--raw` files go to `decode`), Lost (records, bytes, restart flag: the ring overwrote records that were never sent, the next Data frame resumes at the oldest survivor). A hole is therefore always explicit: Lost frame (device side), offset gap (frames missing on the wire) or bad CRC. The stream ends with `trace stop`, closing the port, unplug or suspend (`session_end`); `trace stop` prints `trace stats records=<n> lost=<n>`. The CDC data endpoint is a 16 B bulk pipe, so the sustained ceiling is about 16 KB/s (a busy PD link is a few KB/s); `sniff stream` and `trace stream` may run together but share that pipe.

**Multiplexed debug stream** (`stream on [log|trace|power[=Hz]|run|all] [old]` / `stream off`; `trace stream` is an alias of `stream on trace`; `python3 tools/k2_stream.py record`, full description and the inventory of every device-side source in `docs/DEBUG_STREAMING.md`). The same frame (`0xA6, kind, len, payload, crc16`) carries more kinds: Hello (firmware hash, start time), Boot (boot id and reset flags, so a recording shows where a reboot split it), boot Text, Log (binary K2LOG entries from `usb::log_bin_ring()`, decoded on the host with the `logdb.json` of the Hello's commit), Power (VBUS, IBUS, CC1, CC2, D+, D- at 1..100 Hz, folded into min/max windows when the FIFO is full), Run (charger-test events, `k2/chargertest/run_events.hpp`, emitted by `ui_charger_test.cpp` from the main loop into a 16-event ring), Gap (what a ring overwrote, per source), Beat (heartbeat plus the device's own counters). `k2/core/dbg_stream.hpp` is pure code over rings and a `Probe` seam (host test `tests/test_dbg_stream.cpp`): it owns cursors and counters only, builds a frame on the caller's stack and the ring is the queue, so a slow host costs nothing. `src/core/dbg_stream_hw.cpp` is the probe (power monitor, ADC, D+/D- levels, stack watermarks) and `shell_hw.cpp::poll` calls `service()` once per main-loop pass with the free room of the CDC FIFO: at most 2 frames per kind and 8 per pass, only while a worst-case frame fits, never blocking, nothing in an ISR. Priority: control, run, stale power, trace and log alternating, Beat, power; trace, log and run are never dropped without a Lost/Gap frame, power is decimated first. No compression (domain encoding, repeat collapsing and windowed power instead; LZ4 stays for batch records).

Captures persist: `bbstore` (pure layout) and `bbpersist` (writer) keep them LZ4-compressed as one file each in /bb on the littlefs volume (32 files at most, oldest evicted first). A write happens only after the link has been quiet for 300 ms, and not after 10 minutes without PD traffic. The newest three entries are loaded back into RAM at boot. A charger-test run holds most saves back until it ends (`bbpersist::hold_run`, see "Flash writes during a charger test"): the per-row kinds (Contract, VBUS dip, Edges) always wait, the rare kinds (hard reset, detach, refusal) go at once up to `kRareBudget` (2) per run, and a capture that waits is kept in its RAM slot (`pdtrace::keep_capture`, `Captures::Slot::keep`: a new capture takes another slot, only when all three wait is the oldest overwritten). The overwrites are counted (`Status::dropped`, `drop=` in the `[BB]` line, with `defer=` for the rare captures the budget held back); after the run the waiting captures are written one per `poll()`. The device viewer is Tools > Black Box (`ui_blackbox`, text from `pdtrace_view`).

## Transition capture

A flight recorder for PD voltage transitions (`k2::transcap`, `include/k2/pd/transcap.hpp`; pure code in `transcap_calc.hpp` (ring, delta codec, thumbnail) and `transcap_store.hpp` (files), host tests `test_transcap*.cpp`). Motivation: a charger that cuts VBUS ~165 ms after it Accepts a PPS 3.3 V request is invisible to the 4.5 ms averaged power monitor and to the Black Box's slow VBUS samples.

* **Arm**: `transcap::arm()` claims the mode overlay (`Tenant::Trans`; refused when Net, Drive or Sniff own it), switches the INA226 to the fast bus-voltage mode (`power_monitor::fast_begin`, CONFIG 0x0007: no averaging, 140 µs conversion, one value per about 280 µs) and starts a ring of 1920 samples (VBUS mV from the INA226, CC1 and CC2 from the ADC DMA ring as raw12>>4). The main loop, the LCD idle hook and every `delay_ms` millisecond (`systick::set_wait_hook`; the FUSB302 CC scan blocks ~17 ms twice a second) call `transcap::poll()` (guarded against re-entry); `transcap_calc::Pacer` takes a sample when 800 µs have passed since the start of the previous read (about 1 kHz, real `systick::micros` timestamps) so the ring covers 1.92 s. `arm()` always re-enters the fast mode, also over a held, frozen capture. Normal measurements are untouched: the INA226 CONFIG is restored on disarm.
* **Markers**: `pdtrace` forwards its events through a function pointer; the recorder keeps 64 markers on the same timeline (Arm, TX Request, Accept, PS_RDY, Reject, Wait, soft and hard reset, attach, detach, Trigger).
* **Trigger and persist**: `trigger(reason)` freezes the ring after a 400 ms post window. `keep(run, step)` marks the capture for storage; the main loop writes it after 300 ms without PD traffic (or 2 s after the freeze) as `/tcap/<seq>.tcd` (delta stream: zigzag-varint deltas, run-length for identical steps) and `/tcap/<seq>.tci` (212 B meta with a 96-column min/max VBUS thumbnail), keeps the newest 4 and releases the overlay. An arm nobody triggers lapses after 30 s.
* **Charger test**: `request_pd` arms before every step that is not the default 5 V fixed (`pos > 1 || mv > 5000`). A Pass disarms; a failed step (shut-off, no PS_RDY, hard reset, timeout, lost, voltage) triggers and keeps the capture under the run id and step index. The step detail screen draws the thumbnail (`ui_widgets::draw_range_trace`).
* **Remote**: command 0x71 (`TransCap`, ops status/arm/trigger/disarm/save/list/clear/live/stored); `tools/k2_remote.py transcap ...`; `tools/pd_trace.py transition ...` decodes with `tools/transcap.py` and writes CSV and a PNG (matplotlib optional).
* **Cost**: text +3.7 KB; the overlay member is 15 892 B, which `size` adds to `bss` (real RAM end `_ebss` is unchanged).

## Session record

`session` / `session_store` keep one 16-halfword record (magic 0x4B32, CRC-16) in backup registers DR22..DR37: boot count, CC, Identify state and step, last contract (mV, mA), PDO count and the first three PDOs. It is rewritten when it changes and logged at boot as `[SESS] previous session #N`, together with `[BOOT] reset=<cause>` (power-on, pin, software, watchdog, brown-out), so a restart in the middle of Identify or a trigger is no longer silent. Backup registers DR1..DR3 and DR11..DR18 are not touched.

## USB drive (MSC), flash store and FAT synthesis

The K2 shows up as a read-only 8 MB FAT16 drive ("K2 DRIVE") next to the HID logger. Nothing is stored in RAM as an image: `vfat::Image::read(lba)` computes each 512-byte sector on demand (boot sector, two FATs, root directory, subdirectories, file data pulled through a content callback). One contiguous cluster chain per entry, a directory is one cluster, at most 32 entries, superfloppy layout (no MBR), 16384 sectors, 2 sectors per cluster, 512 root entries.

| File | Content |
| :--- | :--- |
| `README.TXT` | firmware version, git hash, how to use the drive |
| `STATUS.TXT` | live V / A / W, source caps, cable, rendered at read time (fixed 1024 bytes) |
| `LOG.TXT` | recent USB log text from a 3 KB ring (fixed 3072 bytes); fed even when HID logging is off |
| `SAVEDLOG.TXT` | the log snapshot saved to flash (only when one exists) |
| `NATIVE/LIBRARY.BIN`, `LIBRARY.TXT` | the native library page (0x080E3800) raw and decoded, only when the unit has one; read-only |
| `CABLES/<name>.JSN` | one JSON file per saved cable of the e-marker library (spec `docs/EMARKER_JSON.md`); 8.3 short name plus a VFAT long name that follows the user name, so renaming a cable renames the file; long names are not stored, `Drive::lfn` renders them from the library entry when the host reads the directory (the table keeps one 16-bit cable number per file) |
| `RIPPLE.CSV`, `BLACKBOX/` | placeholders for later waves |

Because the layout is rebuilt after a save or erase (`storage::generation()` -> `msc::poll()`), a host that cached the old directory keeps showing it until the drive is re-mounted. Writes from the host are refused (WRITE PROTECT sense, `tud_msc_is_writable_cb` false).

### Flash map

All app-owned persistent data lives in one littlefs volume (v2.11.0, vendored in `third_party/littlefs`, options in `include/k2/core/lfs_cfg.h`: no malloc, no asserts, 256 B cache, 16 B lookahead, 2 KB blocks). `k2::fs` (`k2/core/fs.hpp`) is the thin wrapper (atomic whole-file write = tmp file + rename, append, list, read, remove, mkdir); `k2/drivers/fs_flash.hpp` binds it to the real flash and mounts it at boot (a mount failure formats the volume, logs `[FS] mounted (formatted)` and carries on). `k2::flash` guards every erase and program: the volume's first 90 blocks are erasable and programmable; the run 0x080E4000..0x080FF800 joins them only once `fs::grow()` has proven it blank (or the mount found a grown volume); before that a legacy-range page is erasable only when `legacy::ours()` proves it carries one of our old store signatures, and is never programmed; the kept pages 0x080E3800 and 0x080FF800 and everything else (bootloader, info page 0x08003000, application, the native pages 0x080E0000..0x080E4000) are refused before a register is touched. The DFU marker allow-list is separate and unchanged.

| Address | Size | Use |
| :--- | :--- | :--- |
| 0x08000000..0x08003800 | 14 KB | bootloader, info page 0x08003000 (never touched) |
| 0x08003800..0x080B3000 | ~704 KB | application (vector table 0x08003800, meta 0x08004000, code from 0x08004404; the linker script ends it at 0x080B3000) |
| 0x080B3000..0x080E0000 | 180 KB | littlefs volume: blocks 0..89 of 2 KB (`flash::kFsBase`..`kFsEnd`) |
| 0x080E0000..0x080E3000 | 12 KB | native user settings and related pages (read-only, never erased or written) |
| 0x080E3000 | 2 KB | native single copied-e-marker slot (read-only, never erased or written) |
| 0x080E3800 | 2 KB | native 10-slot e-marker library, a kept page (read-only; shown as `NATIVE/LIBRARY.BIN` and `LIBRARY.TXT` on the drive) |
| 0x080E4000..0x080FF800 | 110 KB | the run: legacy raw stores of earlier firmware (`flash::kStoreBase` up): read for the one-time import of the USB link and UI preference pages, then each page whose own old signature is proven is erased by `fs::wipe_legacy()` (marker `/.legacy`); once all 55 pages read blank, littlefs blocks 90..144 (`fs::grow()`, section 2.3) |
| 0x080FF800..0x08100000 | 2 KB | kept top page, unknown owner: never erased or written |

Files on the volume (compact binary records, each starts with a magic that carries the format version; the drive turns them into JSON/Markdown only when read):

| Path | Owner | Content |
| :--- | :--- | :--- |
| `/usb.cfg` | `usbcfg` | "K2US" + the USB link setting |
| `/cable.cfg` | `cablepref` | "K2CB" + kind (0 Off, 1 Builtin, 2 Saved) + seq (u16 LE) + check byte (kind ^ seq lo ^ seq hi ^ 0xA5); written only when the shared cable selection changes; damaged or naming a gone clone reads as Off |
| `/prefs.cfg` | `uiprefs` | "K2UP" + flags (Try QC after PD, Screenshot) + complement + smoothing byte (0 = Medium default, else 1 + preset index) + orientation byte (bit0..1 manual rotation, bit2 manual, bit3 rotate buttons, bit4 Wake sleeping port; 0 = automatic, keys as printed, no idle Rd) |
| `/emk/NNNN.emk` | `emlib` | one saved cable, 98 B ("K2E1", number, version, 24-char name, 66-byte e-marker record); lowest free number is reused; up to 140 |
| `/log.txt` | `storage` | the log snapshot (newest 512 B of the log ring, `kLogKeep`), RAM mirror for the drive |
| `/cards/<fp>.card` | `report_card_store` | one charger report card, 264 B for format 10 ("K2C1", stamp, 256-byte record; older files of 158 to 228 B, formats 3 to 9, still load and are rewritten as format 10; a repeat of the stored result is folded into it, see `docs/CHARGER_TEST_FLOW.md` "Repeated runs"), `<fp>` = 8 hex digits of the fingerprint; 64 cards at most, the lowest stamp is dropped for a new charger |
| `/jr/*.rec` | `charger_journal` | the running charger test: `h.rec` (header, written last), `sNNN.rec` (step results, replaced in place), `ph.rec` (phase), `rs.rec` (resume count), `mt.rec` (run meta: unit order, `unattended` flag = self-supplied run: key-mode boot or `power_source::bus_powered()`, cable identity kind and seq), `at.rec` (power losses of the unit the marker names; the record key is the unit number), `fin.rec`; each one CRC-framed record (0xA5, type, length, 0, payload, CRC-16), at most about 104 files of 36 B. Power-loss policy: a reboot always resumes, in every power mode, once VBUS has been steady 500 ms; BACK held at power-up drops the run. A unit may cost `kRebootBudget` (3) losses: losses 1 and 2 ask the same row again, the 3rd skips the unit's remaining rows ("rebooted 3x", `Reason::RebootLimit`) and the next unit runs; after 16 resumes a run is not offered again. The card carries the resume count (cable block byte 14, shown as "resumed N" in the card footer) |
| `/bb/NNNNNNNN.bbe` | `bbstore` | one Black Box capture, "K2B1" header of 20 B + LZ4 body, at most 2044 B; 32 files, the oldest is evicted first (also on a full volume) |
| `/rd.rec` | `rdintent` / `rdcomeback` | the persistent Rd intent marker (`k2/core/rd_intent.hpp`, pure record code; `k2/core/rd_comeback.hpp` is the boot glue): a session record (a self-powered or pinned run) or a planned re-attach record (written by `begin_reattach` before the charger test's one deliberate Rd glitch, read back and checked for pin 2). A boot that finds it keeps Rd from the first instruction and raises PC13 (Ra on CC1); the judge releases it after 500 ms with no Rp and gives up after 1500 ms |
| `/.legacy` | `fs` | marker: the legacy raw pages were wiped; the legacy imports never run again and the volume may grow |
| `/.grow` | `fsgrow` | intent: the run was proven blank and the grow to 145 blocks is under way; present only across a power cut in the grow |

#### Volume budget

What each store costs at its cap, in 2 KB blocks, measured by `tests/test_volume_budget.cpp` (every store filled alone on a large RAM disk; the test fails when a store grows past its row, so this table and the test move together). littlefs wear-levels dynamically over all blocks of the volume, 90 or, once grown, 145.

| Store | At its cap | Blocks |
| :--- | :--- | ---: |
| superblock | the root metadata pair | 2 |
| settings | `/usb.cfg`, `/cable.cfg`, `/prefs.cfg` (8 B each), `/rd.rec` (10 B), `/.legacy` (1 B), inline in the root pair | 0 |
| `/log.txt` | 512 B | 1 |
| `/lp/last.rec` | the loop probe record (`loopprobe::kFileBytes`) | 3 |
| `/ps/run.rec` | the PD Stress journal (`pdstress::journal::kMaxBytes`) | 3 |
| `/ps/xxxx.psr` | PD Stress saved results: 8 blobs of 1376 B (`pdstress::store::kFileBytes`, one data block each) + 1 in flight (a save writes before it drops the oldest) | 11 |
| `/twin` | 12 twins of 129 B | 4 |
| `/tcap` | 4 transition captures + 1 in flight (a save writes before it prunes), .tcd taken as 6 KB, .tci 212 B | 24 |
| `/emk` | 140 cables of 98 B (no quirk block) | 54 |
| `/emk`, worst case | 140 cables of 517 B (every quirk block full) | 152 |
| `/cards` | 64 cards of 264 B, after 100 re-tests | 70 |
| `/jr` | the largest plan (`kMaxSteps` rows over all five sections, every small record written) | 7, inside the reserve |
| journal reserve | `bbstore::kJournalReserveBlocks`: the Black Box leaves this many free for the next charger test | 20 |
| `/bb` | 32 captures of one block each | 34 |
| **Sum** | 2 + 170 (typical emk) + 20 + 34 | **226 of 90 (145 grown)** |

The caps do not fit the volume. Measured by the test on both sizes:

| Picture | 90 blocks | 145 blocks (grown) |
| :--- | :--- | :--- |
| every store at its cap, device order | runs out at `/emk`, 0 free, Black Box 0 of 32 | runs out at `/cards`, 0 free, Black Box 0 of 32 |
| 64 cards alone, then the Black Box | 18 free (under the 20-block reserve), Black Box 0 of 32 | 73 free, Black Box 32 of 32 |

On 90 blocks the cards by themselves starve the Black Box (the device reached this state with 61 cards, 5 blocks free). The grown volume gives the 64 cards, the journal reserve and all 32 captures room; every store at its cap at once still does not fit (the stores alone want 2 + 170 = 172 blocks before the reserve and the Black Box). Open: the card format or the caps still have to change for that (decision pending); until then `bbstore::Ring::append` refuses a capture before it compresses it and `fs::Fs::used_blocks` answers from memory between writes, so a full volume costs no loop time.

Rules for a new store or record:

- A small record stays at or below `fs::kInlineMax` (256 B, `include/k2/core/fs.hpp`) and says so with a static_assert (the test holds the ones for twins, `.tci`, `/rd.rec` and the e-marker file). One byte over takes a whole 2 KB data block per file: the format-10 card file is 264 B, 8 B over.
- Inline is cheaper than a block but not free. A littlefs metadata pair is two blocks and splits once its live data passes about half a block (1 KB), and splits never merge back; `write_file` also puts a `.tmp` beside the target. N inline files of S bytes cost about 4 x N x S bytes of blocks, more after churn (64 cards of 256 B: 40-46 blocks fresh, 54-74 after re-tests, measured on the RAM disk).
- A new store adds a row here and a filler to `tests/test_volume_budget.cpp`, and the sum is compared against 90 and 145.

RAM: littlefs state and its 256 B caches are static (`bss` +1.5 KB with the 2 KB Black Box read buffer), no heap. Stack: the deepest littlefs path (rename, orphaning commit, relocating commit, compact, traverse) uses about 0.9 KB, the largest caller frame (`emlib::Library::save`) about 0.7 KB.

The legacy ranges are described in `include/k2/drivers/flash_map.hpp`, which static_asserts at compile time that the regions are sector aligned, in ascending order, disjoint, that the erasable ones lie inside the volume or the legacy range and that the native pages stay outside both, plus the run and the two kept pages next to that table and their tie to `flash::fs_block_addr()` (checked again by `tests/test_flash_map.cpp`).

Black Box persistence (`k2/core/bbpersist.hpp`): a finished capture is written once, only when the link has been quiet for 300 ms (a program or erase stalls the main loop for up to a few tens of ms), and not at all after 10 minutes without PD traffic or VBUS change (`pdtrace::idle()`). At boot the newest three entries are reloaded into the RAM capture slots (shown as "saved" in the list); older ones stay in flash and count toward the used/free figure in the list footer. Nothing wipes the ring from the UI or remote.

#### Flash writes during a charger test

A littlefs erase runs from RAM with interrupts off and takes tens of ms per 2 KB sector, a program about 50 us per half-word; while one runs USB, the 1 ms PD service and SysTick stand still, and the sink must answer a source within ~25 ms. So between START and the end of Releasing a write is only allowed when power-cut safety needs it on flash at that moment; everything else waits for the end of the run. `tests/test_run_writes.cpp` plays a 39-row run (a Baseus-FH22-shaped plan, 17 PD rows, a hard reset every 4th row, a detach per unit, a refusal per hunt row) on a counting RAM disk and attributes every erase and program to its store and to "during the run" or "after it" (device cost model: 35 ms per erase, 0.2 ms per half-word).

| Writer | Trigger | Bytes / erases (39-row run) | Needed on flash mid-run? | Handling |
|---|---|---|---|---|
| Journal arm (`/jr` five section files, header, meta) | START, before any above-5 V request | 3 erases, 2020 half-words (about 0.5 s; a 57-row plan 7+ file ops, 1.5-2.2 s measured) | Yes: a resume needs the plan; the Runner holds in Baseline until the header is on flash | Unchanged (before the first above-5 V request, link at 5 V) |
| Journal phase marker (`ph.rec`) | before every above-5 V PD request (`Phase::Request`) and before a Legacy detach (`Handover`) | 1 erase, 53 programs over 17 markers (one small inline commit each, 12-17 ms, a compaction 130-210 ms) | Yes: it is the only record that a power loss during a request belongs to that row | Unchanged |
| Journal row records (`/jr/<section>`) | a final verdict (`record()` marks the section dirty, `journal_pump` writes one file per tick on a link quiet for 80 ms, waits at most 400 ms) | 13 erases, 11.6k half-words (a section over 256 B is rewritten into a fresh data block: one erase per verdict) | Needed for resume, not needed per row: see "Journal options" below | Unchanged here: list of options |
| Journal `mt.rec`, `at.rec`, `rs.rec`, `fin.rec` | unit loss counter, run meta, resume count, finish | small inline records, 0-1 erases | Yes (loss budget, closing the run) | Unchanged |
| Black Box captures | trigger-frozen, written once the link is quiet 300 ms | before: 20 saves, 21 erases, 9.2k half-words (2.6 s) inside the run; now 2 saves, 2 erases (0.17 s) inside the run, the 3 kept captures after it | No: RAM holds them (power loss loses them, as it always did for the one in flight) | Per-row kinds held, rare kinds budgeted (`kRareBudget` 2), waiting captures kept in their slots, overflow counted (`dropped`), one write per idle `poll()` after the run |
| Transition capture (`/tcap`) | one save per failed row that froze a capture, 300 ms after it or at 2 s | a delta-coded stream over 256 B (data-block erases, `kWriteChunk` 512 B programs), plus the index | Not needed mid-run | Unchanged; the stream is the 15 KB overlay ring, so parking it in RAM for the whole run is not possible without the ring; open option |
| Rd-intent marker (`/rd.rec`) | only when the follower's intent changes (a planned re-attach, a Rescan) | 1-3 small inline commits per run | Yes: the self-powered comeback reads it | Unchanged |
| Report card | the Done transition (`save_card`) | one 200-230 B card file | Not mid-run | Out of scope here: planned as an ask-on-finish save |
| Settings, logs, loop probe | explicit user or remote actions only | none during a run | n/a | n/a |

Journal options (not changed; the journal's reboot-bridging role is the reason it writes mid-run): (a) write a row's verdict as a tiny per-row append record that stays inline (about 12 ms) and fold the section into one data file at a unit boundary (journal format change, the per-row erase goes away, a resume replays the records); (b) write the section files only at unit boundaries and keep the in-flight unit's rows in the loss counter (a power cut inside a unit then repeats the whole unit, `kRebootBudget` stays the guard); (c) pre-erase spare blocks while the link is idle between runs so a section rewrite is a program only (needs a block allocator under littlefs, outside `flash.cpp`'s current contract); (d) an append-only file does not help: appending to a file over the 256 B inline limit copies its last block, an erase every time.

Wear: flash is written only by explicit remote commands (`store save-emarker | save-log`, `emarker save | delete | rename`; `store list` is read-only). The EmLib writes (save, delete, rename, learn-save) are refused by the dispatcher with status Busy + `kEmErrBusy` while `Ui::ports_busy()` (a charger test or an FC release) holds the link, before the backend runs: the test saves its own card at the end and the flash is shared. The sector registers for 0x08080000 and above are the bank-2 set; `flash::store_bank()` reports which set worked (2, 1 or 0 = no write yet) and every write failure ends in an error code, not a hang. Programming runs from RAM with interrupts masked and bounded waits.

## Charger report cards

Every Done charger-test run is saved as a card (a fixed-size record, format version in `report_card.hpp`) keyed by the charger's fingerprint: FNV-1a 32 over the little-endian raw PDO list plus the spec revision byte (`k2/chargertest/report_card.hpp`). While the charger sits in its 5 V contract the runner asks it once who it is (`k2/pd/pd_probe.hpp`, driven by `fusb302::probe_start`): Discover Identity, Get_Manufacturer_Info and Get_Source_Cap_Extended on SOP, one try each, 250 ms per answer, 900 ms in total (the runner waits at most `kProbeMaxMs` = 1000 ms); a NAK, Not_Supported or silence just skips that question, nothing is retried and no hard reset is sent. The card keeps VID/PID (Identify, else Manufacturer_Info or Source_Cap_Extended) and `pdp_w` (the Source_Cap_Extended PDP, else the largest PDO in watts). Format 2 stores `pdp_w` at payload byte 54 and a flags byte at 55 (bit0 name_auto) in the two bytes after the 20 name characters, so the record stays 150 B; older cards decode with both 0. A card without a user name gets `<Brand> <PDP>W` (`report_card::auto_name`: the small verified VID table, else the Manufacturer_Info text, else `VID xxxx`); a name set with rename is never replaced, an automatic name is replaced by a newer automatic one. The card holds per-group verdicts with reason codes, up to four PD/PPS ranges (100 mV units, max measured V), and (v4) per-EPR-level verdicts for the 28/36/48 V fixed supplies, the handover outcome and PD give-up time (100 ms units), final state, firmware token, boot count, uptime, run count and a 20 character name.

Storage (`k2/chargertest/report_card_store.hpp`): one file per charger, `/cards/<fingerprint>.card`, a fixed-size record (see "Flash map"); a save replaces the file atomically, a power cut leaves the old or the new card. Capacity 64 cards; a new charger beyond that drops the card with the lowest save stamp. The host test (`tests/test_report_card_store.cpp`) cuts power at every operation and holds 40+ cards.

Delta (`k2/chargertest/report_card_delta.hpp`): group verdict changes, PPS range failure voltage and max voltage changes between the new run and the charger's earlier card; the charger-test results page shows them on a "Since last card" page. Identity is captured at run start (and at resume), because the source caps go stale after a handover. Chargers without PD source caps cannot be keyed and get no card.

Tools > Report cards (`src/ui/ui_cards.cpp`, list, card, delete with confirm) shows the saved cards; see `docs/UI_DESIGN.md` 4.4.

Host access (read-only except the two explicit write commands):

- Remote command `0x62` CARDS (`k2/chargertest/report_card_remote.hpp`, validated in `remote_proto.hpp`, bound to `report_card::hw` in `Remote::cards`). First payload byte is the op: 0 list `[0]` -> `err,total,n,no u16*n` (newest first); 1 get `[1,no u16,chunk]` -> `err,chunk,bytes` (the 144 B v3 / 162 B v4 payload in 3/4 chunks of 48 B); 2 rename `[2,no u16,name 1..20]` -> `err,no u16`; 3 delete `[3,no u16,1]` (the 1 is the confirm byte) -> `err`. err 0 Ok, 1 Invalid, 2 NotFound, 3 Flash, 4 Full, 5 Busy (rename and delete are refused while a charger test runs). Rename is the only way to name a card; nothing writes flash except these two ops and the end-of-run save.
- `tools/k2_remote.py cards list | show <id> | export --out x.json | rename <id> <name> | delete <id> --confirm`. `tools/report_card.py` decodes the payload into the CARDS.JSON schema; `charger_summary.summarize_card` renders it in the run-summary format.
- USB drive: read-only `CARDS.JSON` (8.3 name `CARDS.JSN` with a long name) holding a JSON array of every card, generated from the store on read (`k2/chargertest/report_card_json.hpp`; fixed 2 KB slot per card so offsets are stable; the drive is rebuilt when the store changes).
- Web dashboard: a read-only "Report cards" section (`cardsList` in `tools/web/k2proto.js`).

Not yet implemented: on-device rename (no text editor; rename over the remote).

## Remote control, screenshots and the UI model

The Connection page (`ui_connection_page.cpp`; pure row model `include/k2/ui/ui_connection.hpp`; tab and match
arithmetic `k2::pdtabs`) shows the Source, Cable and Sink tabs. It is passive: it transmits only from an action
(Refresh, Read cable, Identify), never while merely open. The D+/D- scanner (`fc_scan`) is the SCAN entry of the
Trigger submenu: it calls `fctrig::scan_start(confirmed, false)` after the confirmation, probes
FCP/SCP/AFC/QC2/QC3/VOOC/QC3+, resets between protocols, and rechecks link, monitor, source presence and
voltage-ceiling guards throughout the scan, including reset waits. It never overrides the link guard or starts by
itself. Host tests: `tests/test_fc_scan.cpp` (scanner against a fake charger), `tests/test_ui_trigger.cpp`.
Remote FC results keep a four-byte record (protocol, outcome, VBUS); seven records occupy 38 of the 58
response-payload bytes. `FcInfo` has seven slots (wire capacity is twelve); Python/JS decode the transmitted count
and the dashboard iterates every decoded result. Detailed per-step failure reasons stay local to the UI and do not
extend the remote wire format.

The K2 can be driven from a PC over the same HID interface as the log (no new endpoint, descriptors unchanged). `tools/k2_remote.py` is the host client; `src/core/remote.cpp` the firmware side; `include/k2/core/remote_proto.hpp` / `src/core/remote_proto.cpp` the pure frame codec (host-tested with a fake backend in `tests/test_remote_proto.cpp`).

### Entering the bootloader (DFU)

The bootloader checks `K200` at 0x08004010 and the commit marker at 0x08004400 before jumping to
the app. `k2::dfu::enter()` clears the K200 word (0x08004010 and
0x08004012, pure bit-clear) through `k2::flash`:

- Allow-list: only those two half-words and only the value 0; everything else, including the
  bootloader region, returns `Status::Refused` before a register is touched (`static_assert`s pin the
  address above 0x08004000).
- Run from RAM: the app and the code that runs live in the same flash bank, and programming stalls
  fetches from that bank. The unlock/program/wait/lock sequence is one `.ramfunc` function (placed in
  `.data` by `link.ld`, copied by `Reset_Handler`) with PRIMASK set, calling nothing.
- Sequence: log `[DFU] entering bootloader`, flush USB logs, clear, read back, log `marker cleared,
  resetting`, 3 SRAM windows as `[RAM]` lines (all inside the 96 KB that is mapped by default), park the FUSB302 (software reset) and the LCD (display
  off, sleep in, backlight off), USB detach, `crm::deinit()`, then `sysreset::wipe_and_reset`: zero
  SRAM 0x20000000..0x20017FFF (96 KB) with a register-only loop (FAULTMASK + CCR.BFHFNMIGN), then SYSRESETREQ or an IWDG reset (PR /4, RLR 1, ~0.2 ms). A failed clear
  or readback logs and returns without a reset.
- Reset mode: `DFU_ENTER` takes an optional 5th payload byte (0 sys, default; 1 iwdg; else BadArg),
  `k2_remote.py dfu --confirm --reset sys|iwdg`, `k2_flasher.py flash --auto-dfu --dfu-reset sys|iwdg`.
  The app logs `[RST] CTRLSTS=... PIN POR SW ...` at boot (flags accumulate from power-on; never cleared).
- Triggers: remote `DFU_ENTER` (0x30, payload `DFU!` plus the optional mode byte; BadLength / BadArg
  otherwise; the response is sent first), `k2_remote.py dfu --confirm`, the WebHID button (with a confirm dialog),
  `k2_flasher.py flash --auto-dfu`. An on-device trigger is deliberately not built; if wanted, hold
  BACK+MINUS for 10 s on a dedicated screen.
- 96 KB limit: the AT32F403A maps 96 KB of SRAM unless EOPB0 selects 224 KB. A read above 0x20018000
  bus-faults; the first dump of ebf0db0 read 0x20037FC0 and hung the app (HardFault spin, USB attached).
  Nothing in dfu/sysreset touches SRAM above 96 KB. The EOPB0 value is not read at runtime.
- Fault handler (`k2::fault`, `fault.hpp`, `fault.cpp`, pure formatting in `k2/core/fault_calc.hpp`):
  `fault::init()` enables MemManage/BusFault/UsageFault. NMI/HardFault/MemManage/BusFault/UsageFault
  are naked entries that pick MSP/PSP and call `k2_fault_main`: logs `[FAULT] <HF|BF|MM|UF|NMI> PC= LR=
  CFSR= BFAR=` and `[FAULT] HFSR= MMFAR= SP= PSR=`, pumps USB by hand for ~80 ms (`usb::pump()`:
  interrupts are off), then `usb::detach()`, `crm::deinit()`, SYSRESETREQ. A fault inside the handler
  resets at once. The handler runs on the MSP stack (`.msp_stack`), not on the application's PSP stack, and
  switches the MPU off first, so a stack overflow trapped by the MPU guard is reported as `[FAULT] SO ...`
  (see "Stack layout").
- `DEBUG_FAULT` (0x31, payload `BOOM` + kind: 0 undefined instruction, 1 read of 0x20037FC0, 2 runaway recursion
  until the stack guard faults; else BadArg, BadLength if not 5 bytes), `k2_remote.py fault --confirm
  udf|read|stack`: the response is sent, then the fault.
- `FACTORY_WIPE` (0x33, payload `WIPE` [+ reset byte 0 sys / 1 iwdg]; BadLength unless 4 or 5 bytes, BadArg for a
  wrong magic or reset > 1; not offered over HTTP), `k2_remote.py factory-wipe --confirm`: erases everything this
  firmware owns and enters the bootloader (`k2/core/factory_wipe.hpp`, pure sequence over an Env seam, host-tested). Order: unmount littlefs,
  build the mask of legacy pages that carry our own old signature (before any erase), erase the 90 sectors of the
  volume (0x080B3000..0x080E0000) ascending and then those legacy pages descending (so the proof of a log tail
  survives its head) through the guarded `flash::store_erase`; every other page of the native area is not in the
  list and the guard refuses it. Read every listed sector back as 0xFF, zero this firmware's backup registers DR22..DR37
  (`session::wipe()`; DR1..DR3 / DR11..DR18 and the boot trace's stay), then queue the same DFU entry as
  DFU_ENTER (`dfu_now()`), so the app never boots again and cannot re-format the volume. The reply (14 bytes: err,
  sectors, erased, verified, failed u16 each, first_fail u32, flash status) is sent before the DFU entry and only
  after the whole wipe (seconds; the host waits up to 30 s). err 1 = erase failed, 2 = verify failed: no backup
  clearing, no DFU, the K2 stays in the app (volume unmounted until the next boot formats it); the CLI exits 6.
- `FLASH_DUMP` (0x34, request addr u32 LE + len u8, response addr, len, data; len 1..48, window inside
  0x080E0000..0x08100000 else BadArg, BadLength unless 5 bytes; read only, offered over HTTP),
  `k2_remote.py flash-dump DIR`: reads the device-storage area through the app twice, compares
  both passes and writes `DIR/native_storage.bin`. For a unit whose bootloader refuses reads above
  0x080E3000.
- Flasher states: `--auto-dfu` tells "app answering" (ping answered) from "hung" (the device enumerates
  as the app but ping times out): hung means replug in DFU, with a hint to check the log for `[FAULT]`.
- Recovery: hold MINUS while plugging in USB, then flash. Flashing rewrites the page, which restores
  the marker. If the bit-clear does not force DFU the app boots again; next candidate is the commit
  marker 0x08004400 (change `kDfuMarkerAddr`).

### Protocol

Every message is one 64-byte HID report on EP1 (OUT = request via SET_REPORT/EP1 OUT, IN = response).

| | Layout |
|---|---|
| Request | `A5 4B cmd seq len payload[<=59]` |
| Response | `A6 4B cmd seq status len payload[<=58]` |
| Screenshot stream frame | response-shaped, `cmd = 0x8A`, payload `strip, flags, RLE bytes[<=56]` |

Commands: 1 PING, 2 BUTTON, 3 GET_STATE, 4 GET_PDOS, 5 GET_EMARKER, 6 ACTION (`[id]` or `[id, confirm]`; the ids that can raise VBUS above 5 V or drive D+/D-, FcScan 4, answer NeedConfirm without the confirm byte, decided by `remote::action_needs_confirm`), 7 SET_PAGE, 8 REBOOT, 9 LOG, 10 SCREENSHOT, 0x20 DEBUG_PDOS, 0x30 DFU_ENTER (payload `44 46 55 21`), 0x31 DEBUG_FAULT (payload `BOOM` + kind), 0x32 FC (active fast-charge trigger), 0x33 FACTORY_WIPE (payload `WIPE`, see above), 0x35 BOOT_INFO (see "Boot info"), 0x40 IDENTIFY_DEVICE, 0x41 PD_REQUEST, 0x42 PD_RESET, 0x43 PD_TRIGGER_STATUS, 0x44 PD_RELEASE, 0x48 PD_ATTACH (`[mv u16]`, `[mv, ma u16]` or `[mv, ma, flags]`, flags bit0 = confirm, the PD_REQUEST bit, nothing else; no target = 5000 mV; the verdict is `pd::attach_verdict`: a charger test or FC release owns the ports -> Busy + `pd::Err::Busy`, a sink behind the K2 (inline, a sink Rd or a foreign Request) -> BadArg + `pd::Err::Inline` with no Rd and nothing transmitted, a target above 5000 mV without the flag -> NeedConfirm; a refusal replies status + one err byte and no snapshot; the HTTP raw bridge needs `confirm=1` for it like PD_REQUEST; the first call with no source caps in hand presents Rd only and takes the source's own 5 V, a call with caps picks the PDO with `pd::pick_attach_pdo` (`k2/pd/attach_pick.hpp`, host test `tests/test_attach_pick.cpp`): the target clamped to the monitor ceiling, then, best first, a fixed PDO at exactly the target, a PPS / AVS range holding it (requested at the target), the lowest fixed PDO at most 1 V above it, above 5 V the highest fixed PDO below it, else PDO 1 (until 2026-11-11 every range outranked every fixed level, so a 5 V attach on the NOVOO RG100 took its PPS APDO); the confirm rule applies to the chosen level, not the target. An accepted attach HOLDS its contract (`SinkStatus.held`), 5 V included: held means the host owns this contract and a re-announced list is answered with the same level (`lookup_held`), not that a keep-alive runs (only PPS, AVS and EPR send one). The charger test's 5 V steps rely on that flag (plateau and renewal checks), so a held 5 V contract is by design and every reader of `held` must accept it: the lease settles on it (below), `Ui::busy_why` reports PdHold (a charger-test START refused "PD hold: release it") only for a `pd::user_hold` on a K2 that is neither self- nor bus-powered and outside a run's Rd or a re-read's own leftover, `pd release` of a Ready 5 V hold asks PDO 1 again. The driver's own PDO 1 answers to caps (`Sink::answer_5v`: the run-start Request, acked caps including the self-power hold, the re-request after a Hard Reset) set `held` too, because the sink answers a re-announce during Requesting / Transition only while held, and mark it `SinkStatus.auto_hold`; any trigger clears the mark. `pd::user_hold` (held and not auto) is what "somebody owns this contract" means: the PdHold busy reason, the `stop` report's pd bit and the charger test's release and unsafe-exit checks read it, and `pd::auto_own_hold` (an auto hold Ready at PDO 1 at vSafe5V) lets the Ra arming take it over like the self-hold. PD_TRIGGER_STATUS appends it as byte 20, `k2_remote pd status` prints `held=True(auto)`; until 2026-11-11 a twin run's re-attach left `held=True` that looked like a user's and stalled each charger-test release 8 s) (active PD), 0x4B STOP (no payload, BadLength otherwise; no confirm and never gated over HTTP, it only goes to 5 V and idle; one backend call, `k2::stop::stop_all` in `k2/core/stop.hpp`, in this order: cancel a charger test, stop any FC trigger with the measured 5 V release, release PD to 5 V (a no-op at 5 V), disarm a spoof; reply 6 bytes `[active before: bit0 test, bit1 fc, bit2 pd, bit3 spoof; vbus_mv i32 LE, -1 = monitor not ready; fc_release_busy]`; the `[RMT] stop ...` log line and the CDC shell `stop` use the same path), 0x4C LEASE (the host dead-man lease, pure timing in `k2/core/lease.hpp`, host test `tests/test_lease.cpp`; request `[0]` status or `[1, seconds u16 LE]` set the length (0 = the default 30 s, above 600 BadArg, any other shape BadLength); reply 6 bytes `[armed, left_s u16 LE (rounded up), length_s u16 LE, ended]` with ended 0 none, 1 expired, 2 host gone, 3 released, 4 settled, 5 test running; no confirm and never gated over HTTP. The dispatcher ARMS it when a host command that can leave the output above 5 V is accepted: `pd request` / `pd attach` above 5 V, `fc trigger` above 5 V, `fc scan` (both the FC op and ACTION FcScan); every other valid request from the host, any transport, REFRESHES a running lease (it arms nothing, so a shell command never arms: only `Remote::lease_arm` called by the dispatcher does). `Remote::poll` ticks it once per pass: silence for the lease length runs `Remote::stop` (`[RMT] lease expired: stop`), and the USB link dropping while armed runs it at once (`[RMT] host gone: stop`); arming logs `[RMT] lease armed <s>`. It ends WITHOUT a stop on the host's own `stop` / `pd release` / `pd reset` / `fc reset` (released), on a charger test starting (a test owns the ports itself and is never host-owned voltage, so a lease never cancels it, even when the cable is pulled meanwhile) and on a verified 5 V for an unbroken 5 s (settled), the pure `lease::safe_5v` (pinned in `tests/test_lease.cpp`): no FC trigger or scan, the monitor reading 4.5..5.5 V, no release verification running, and either an idle sink with no hold or a Ready contract at 5.5 V or below, held or not; a sink still negotiating (Requesting, Transition, EPR entry, Aborting) never counts, and an attach still waiting for the source capabilities raised nothing, so it never armed one. Until 2026-11-11 only an idle unheld sink counted, so a twin run or a `pd attach` 5000 that ended on a held PDO 1 contract left the lease armed until it expired into a needless stop (NOVOO RG100, `runs/2026-11-11-novoo-alone/`). A set-length applies to the NEXT arm only within 5 s of the set, so a refused command cannot leave a long lease behind for an unrelated arm. Client side: `k2_remote.py --lease N` sends a set just before the arming command, `--hold` pings until Ctrl-C and then stops, `lease [seconds]` and the `"lease"` key of an arming command's `--json` result read it; the web page polls it (its own GET_STATE poll is the keep-alive) and shows the time left in the header), 0x50 CABLE_R, 0x70 BLACKBOX (ring read / clear / freeze / status / captures, see "Black Box"), 0x62 CARDS (charger report cards, see "Charger report cards"), 0x61 EM_LIB (saved cables; first byte is the op: 0 list `[0,start]` -> `err,total,n,seq u16*n`; 1 get `[1,seq u16]` -> `err,seq,name[24],vid u16,pid u16`; 2 save the inline cable `[2]` -> `err,seq,existed`; 3 delete `[3,seq u16,1]` (the 1 is the confirm byte); 4 rename `[4,seq u16,name 1..24]`; 5 spoof `[5,0]` status / `[5,1,seq u16]` arm / `[5,2]` disarm -> `err,active,requests u16,replies u16,seq u16`; err 0 Ok, 1 Full, 2 Invalid, 3 NotFound, 4 Flash, 5 Exists, 6 Inline = a live sink is attached, arming refused), 0x60 STORE (`[op]` 0 list / 1 save-emarker = save into the library like EM_LIB 2 / 3 save-log; op 2 (erase) is retired and answers BadArg; response `err, count, flags, slot, log_len u16, bank, native records`); all ids are checked unique by a static_assert in remote_proto.hpp). Status: 0 Ok, 1 UnknownCmd, 2 BadLength, 3 BadArg, 4 Busy. Buttons are 0 Back, 1 Minus, 2 Ok, 3 Plus; rotation pages 0 Home, 1 Detail, 2 Charger test, 3 Lines, 4 Tools, 5 Settings; menu pages 6 + Menu (16 Connection, 17 Diag; 15 Share is refused, it needs a record); tab aliases 18 Connection/Source, 19 Cable, 20 Sink (`SET_PAGE` is BadArg for index >= 21 and closes any open submenu; `GET_STATE.page` reports 6 + Menu while a submenu is open). PDO type bytes equal `k2::pd::PdoType` (1 fixed, 2 battery, 3 variable, 4 PPS, 5 AVS); power chip bytes equal `k2::power::Chip`. `usb::on_remote_report()` (called from the TinyUSB SET_REPORT callback) only queues the frame; `Remote::poll()` runs from the main loop and returns an action (`Reboot`, `Screenshot`) for `main.cpp` to carry out. The text log is switched off by `LOG off` so a client owns the IN endpoint (`usb::log_enable`, `usb::send_report`, `usb::tx_free`).

BUTTON injects a press into the same `ButtonDetector` path as the physical keys (30..10000 ms; the client's `--long` holds 700 ms, the firmware long-press threshold is 500 ms; `press back --ms 5000` triggers the reboot hold), so remote and hardware input are indistinguishable to the UI.

### Charger-suite RAM retrieval

`CHARGER_TEST` (`0x0B`) is read-only, including while the runner is active, releasing or unsafe.
No operation byte or mutating variant exists. All integers are little endian:

| Request | Response payload |
|---|---|
| empty (metadata) | `schema u8=1, run_id u32, revision u32, state u8, reason u8, total u8, current u8, start u8=0, n u8=0` (15 bytes) |
| `start u8, run_id u32, revision u32` (9 bytes) | same 15-byte header, then up to two 16-byte rows |
| row | `group u8, pos u8, flags u8, target_mv u16, request_ma u16, measured_mv u16, verdict u8, reason u8, detail u8, raw_pdo u32` |

Enums match `k2/chargertest/charger_test.hpp`; flags bit 0 is continuous and bit 1 AVS, other bits zero.
Indices are zero-based (`pos` remains the driver's one-based PDO position); `current==total`
means the plan is exhausted. Measured voltage is the runner's stored result, not a new/live
sample; zero means unrecorded or a clamped non-positive/missing sample. PD request detail >=128 encodes `pd::Err + 128`; other PD
details encode `SinkResult`, legacy details `fc::Fail`. Raw detail is retained in JSON.
Wrong request lengths answer BadLength; `start>total` BadArg; stale run/revision Busy with no rows.
`start==total` is a valid empty end page. An empty plan returns metadata, not an error.

`Remote` borrows a const span from `Ui`/`ChargerTestPage`/`Runner`; encoding uses the existing
bounded dispatcher response buffer, not a duplicate result table. Two 32-bit scalars track the
boot-local run and exported-state revision. Timer-only polls do not change the revision;
stored result/state/index/reason changes do. Reads neither poll the runner nor touch drivers.
`k2_remote.py charger-test status` reads metadata; `results [--out PATH]` pages the table and
checks final metadata, rejecting changes rather than exporting mixed evidence. It has a bound
of 50 calls for all 95 rows, no automatic retry loop and no fallback to active commands.
`--json` prints the same evidence object; `--out` writes host JSON only after successful retrieval.
The JS command table is synchronized, but no dashboard control is added.

**The screens as data (`req [3]`, the view).** A one-byte request `03` (read-only like the rest) answers with what the
charger-test screens show, so a host does not need a screenshot: `stage u8, cursor u8, rows u8, flags u16, confirm_left_ms u16,
wait_elapsed_ms u16, wait_total_ms u16, run_elapsed_s u16, card_no u16, delta_n u8, delta_runs u16, runnable u8, skipped u8,
refusal u8, refusal_mv u16, pps_points u16, avs_points u16, topology u8, run_guard u8, rmdo u8`, then the footer's words
(0 to 27 characters, exactly what the footer paints). Flag bits are `kCtView*` in `remote_proto.hpp`. The session half is read
from RAM (`ChargerTestSession::view()`); the Review preview is refreshed there with the same `update_summary()` the page tick
uses, so it is live with no page on screen; `rows` and `message` come from the page (`ChargerTestPage::describe()`) and are 0 /
empty while it is not on screen (`on_screen` clear). The Waiting clock (`wait_total_ms`, from `wait_total_ms()`, also the
paint's bar) and `run_elapsed_s` read 0 outside Waiting / Running. Timers saturate at 65535. An older firmware answers
BadArg (BadLength for a longer request), which the host reports as "no charger-test view". `k2_remote.py charger-test view`
(`--json` gives `stage, cursor, rows, on_screen, confirming, confirm_left_ms, confirm_reattach, wait{elapsed_ms, total_ms,
retry, paused}, run_elapsed_s, card_no, delta_n, delta_runs, preview{runnable, skipped, refusal, refusal_mv},
sweep_points{pps, avs}, guard{topology, run_guard}, resume{pending, auto, gate_ready, held, behind_cable, rd}, self_supplied,
armed_is_pick, cable_armed, rmdo, message`).

**What the host can read (shell parity).** One row per command that replaces a screenshot:

| Command | Reads | Not on the wire |
|---|---|---|
| `CHARGER_TEST` `req [3]` | the wizard stage, cursor, row count and footer words, the START confirmation clock, the Waiting and Running clocks, the Review preview and refusal, sweep point counts, resume and Ra-guard state | the row texts of the lists (their data is in the results / cable / units commands); pixels |
| `GET_STATE` `req [1]` (`k2_remote --json state`, merged into the state object) | session energy (mAh) and elapsed time (frozen at unplug), the Home protocol line (kind, limit mA, max mV, Type-C mA), the busy reason, 5 V release, topology, wait kind, why PD is absent, the open-CC level, the menu stack, the Connection tab / popup / details, the list cursor (`sel` / `count`), the Home trend window, and the hold / rotation / e-marker / identify flags | the row texts and values of the pages themselves; a list cursor on pages that do not bind the shared row list (`sel` is null there) |
| `GET_STATE` `req [2]` (`state.diag` in `--json state`) | the Diag page rows as numbers: FUSB302 device id, PD messages received, USB resets / interrupts, log reports sent / dropped, the longest main-loop gap since the last `[USB]` heartbeat line, USB configured. The other Diag rows are already in the base state (active CC, meter online and chip) and in `ping` (uptime) | the verdict marks and the footer message (derive them from these fields and `pm_online`) |
| `GET_STATE` `req [3]` (`state.keys` in `--json state`) | what the four buttons do on the page on screen: for `back` / `minus` / `ok` / `plus` the short- and long-press word (`short`, `long`, 7 characters at most, empty = nothing) and `dim`, plus `hidden` when the page covers the strip (the QR code) | the drawn text: a narrow slot drops the long word (the host can apply the slot widths), and the strip highlight of a held key |

### Screenshots

`SCREENSHOT` re-renders the page instead of reading the panel (the ST7789V is write-only on this board). `capture_screen()` (`src/ui/capture.cpp`) installs a `lcd::Sink` with `lcd::set_sink()`; `stream_begin/pixels/fill` then go to a `PixelSink` strip buffer instead of SPI3. The screen is cut into 60 strips of 4 rows; the UI re-renders the whole screen into the sink once per strip (`Ui::render_for_capture`), keeping only that strip's rows, and each strip is RLE565-encoded (`include/k2/ui/rle565.hpp`: token `0x00..0x7F` = run of 1..128 equal pixels, `0x80..0xFF` = 1..128 literals) and sent as stream frames, with `flags` bit 0 = last chunk of the strip and bit 7 = abort. Afterwards `Ui::resync()` redraws the real panel. Both HID and HTTP hold the same capture ownership flag for their entire transfer (including USB servicing and final resync), so an HTTP callback cannot steal the HID strip buffer. `k2_remote.py screenshot out.png --scale N` decodes and writes a PNG; `--debug` prints per-strip counts, `--dump-raw FILE` saves the raw frames. The RAM cost is the 1920 byte strip buffer (`bss`; it cannot live in the `.mode_*` overlay, because Net mode (K2_NET=1 builds) serves screenshots from that very region, and a stack buffer would eat 1.9 KB of the 8 KB stack budget). Settings > Screenshot (default on, `/prefs.cfg`) turns it off: the remote command answers `Busy`, the web route 503, and nothing renders.

### Rendering and temporary RAM

#### Gravity rotation

`accelerometer.cpp` drives the sensor on the PB8/PB9 bus (the INA bus) at address `0x18`:

- It writes `CTRL_REG1 (0x20)=0x27` (10 Hz, normal mode, XYZ enabled) and
  `CTRL_REG4 (0x23)=0x10` (±4 g), additionally sets BDU (`CTRL_REG4` = `0x90`),
  then verifies both registers. Only `WHO_AM_I (0x0F)=0x33` (LIS2DH12) or `0x11` (SC7A20)
  enables writes; unknown identities are rejected without writes. The installed silicon vendor
  still requires a hardware read.
- It reads X/Y/Z as one coherent six-byte read from `0x28 | 0x80` (I2C auto-increment) and
  converts signed normal-mode output with `mg = (word / 64) * 8` (±4 g, 10-bit left-justified).
  The same word gain (`2/16 = 8/64` mg per register count) applies to both sensors.
  Fake-I2C tests pin both identities' configuration and signed scaling, and reject nearby IDs.
- Screen-axis gravity is `x=-sensorX`, `y=-sensorY`; Z's magnitude rejects flat postures.

The pure tracker requires a dominant in-plane axis with a 250 mg margin, near-1 g magnitude,
and 300 ms of consecutive samples; a gap over 150 ms or invalid sample restarts debounce.
Unsigned timing works across tick wrap. Read errors discard stale data and retry after 1 s,
without probing unrelated addresses or touching PD or power-monitor configuration.
`Ui::feed_gravity` rotates only when unlocked. A physical +/- long chord locks, steps through
four postures, then returns to AUTO; it consumes only that chord and preserves individual events.
The seventh main page, Settings, owns USB Link and Autorotate. Autorotate offers Auto and
Manual 0/90/180/270 deg; choices are session-only and share the chord's lock state. Applying
a setting queues the requested angle until event rendering releases the old viewport, then
redraws once with the new layout. Auto catches up to the latest gravity posture tracked during
Manual. Home has no rotation-help footer; USB Link retains its existing stored/reboot behavior.

`ContentViewport` installs a scoped `lcd::Sink` around every page render/update and chains any
existing screenshot sink via `lcd::get_sink()`. It maps the fixed logical 224x240 grid into the
rotated physical content rectangle with integer pixel-edge scaling and one 240-pixel automatic
row (480 bytes of transient stack, no framebuffer). Text maps only its origin, keeping bitmap
pixels and cell advances native; partial fields offset cells after mapping the field origin.
Composed bands paint native-size text into the mapped band dimensions. Labels pause mapping
and remain beside their physical keys. Tool port diagrams and flow arrows are oriented separately.
Home has only a static arrow beside current, no port drawings, and 186-pixel gauges. Ordinary
readings still update fields, never call `Ui::redraw`; redraw occurs on a rotation/page change.
Fake-I2C tests exercise the real driver, configuration failures and four axis quadrants.
UI tests compare all page/menu captures against the LCD at all four rotations. Actual device
sensor identity remains unverified; the user confirms autorotation works on hardware.

Status/footer bands combine their layers in one bounded 240-pixel automatic row before streaming
to the LCD. Ordinary text and rotated labels also use automatic row/column buffers instead of
persistent globals (480 and 64 bytes respectively); there is no full-frame buffer.

`LogRing::view()` lends at most two line-aligned spans of the recent USB log. The synchronous
`storage::save_log()` writes those spans directly, including half-words crossing the ring boundary,
so the former 3072-byte duplicate snapshot allocation is gone. The flash writer does not service
USB or log while consuming the spans; the result is logged only after writing finishes.
`snapshot()` still copies for callers that need an independent buffer. Ripple samples belong to
the `Remote` instance rather than a separate namespace global.

Persistent driver/IRQ state, protocol rings, USB/network pools and the screenshot strip still need
static lifetime. Eliminating their storage or moving kilobyte arrays onto the stack would not be a
safe RAM optimization. The temporary-buffer changes above remove 3616 bytes of `.bss` before the
sensor/reader additions; use the final ELF size for the integrated total.
ARM `-Os -fstack-usage` checks put `gfx_text` at 568 bytes and rotated text at 160 bytes;
band/footer functions peak at 632 bytes per function (plus callees), below the 1024-byte
per-function warning threshold. This is not a whole-program stack high-water measurement.

### Input, pages and ListView

* `include/k2/ui/ui_events.hpp`: a debounced press becomes `Event{button, Short | Long | Hold}`. The global handler sees it first (long Plus = next page, long Minus = previous page, a 5 s hold of Back = reboot (`ButtonEvent::Hold`, fires once when `kHoldMs` = 5000 is crossed while the key is still down; a long Back at 500 ms is just Back); no wrap, so at the ends the press falls through to the page). Otherwise the active page gets `on_event()`; a declined event is unhandled. There is no auto-repeat.
* `include/k2/ui/ui_pages.hpp`: each screen is a `PageBase` (rotation pages Home, Detail, ChargerTest, Lines, Tools, Settings; twelve submenus incl. Connection and Diag, see section 3) with `draw_static()`, `update()`, `on_event()`, `labels()` and `list_state()`; `Ui` owns the pages and the current one. Interfaces have a protected non-virtual destructor and pages are static members.
* `include/k2/ui/list_view.hpp`: `ListState` (pure offset/thumb arithmetic, clamped, no wrap) plus `ListView` (row-draw callback, redraws only changed rows, scrollbar only when the list overflows). Pd, Tools, E-marker and Diag use it; short Plus/Minus scroll. A slot repaints when its index or highlight changes, or when its optional row signature (`set_row_sig`, a hash of what the row shows) changes: a page whose rows change content passes a signature instead of wiping the body and invalidating the list (the wipe plus a full repaint wrote every pixel twice and flashed black; the Charger page was the first to be fixed this way).
* Panel labels (`include/k2/ui/ui_labels.hpp`): each button slot shows `short/long` using full words in the 12 px panel font (7 px advance): `Back` on every page (reboot is the 5 s hold and is not printed), `-/Prev`, `+/Next` (dimmed at the first/last page), page actions in OK (`Rescan`, `Scan`, `Reset`). The slots tile the 240 px edge with `kSlotLenPx = {66, 54, 54, 66}`; the centres in `ui_style.hpp` derive from them. The real button centres are not measured, so to follow them change the lengths (sum 240, outer pair and inner pair equal). Each slot after the first starts with a 2 px light-grey divider (`kDividerPx`, `kPanelLine`), and the text is centred in the zone left after the divider and 4 px padding at both ends (`label_zone()`, `label_origin()`).
* Debug: `DEBUG_PDOS n` (`k2_remote.py debug pdos --fake n`, n = 0..7) makes `pd::active_caps()` return synthetic source capabilities (`pd::fake_fill`, in `src/pd/pd_source.cpp`) so the Connection page and `GET_PDOS` can be exercised without a charger. Refresh on the Connection page clears it.

### Power readout

* `k2/drivers/power_calc.hpp` (pure): `kDeadbandMa = 10`. `apply_deadband()` zeroes |I| < 10 mA (so 9 mA is idle, 10 mA is not), `flow_of()` gives `Flow::{Idle, Forward, Reverse}`, `magnitude()` the absolute value. `power_monitor::read()` applies it before computing power, so idle shows 0.000 A, 0 W and no arrow.
* Forward = power from the input (the side the charger plugs into) to the output (the side the load plugs into) = a positive `current_ma` everywhere above the driver. Measured on hardware (100 W PD charger, 19.7 V, ~1.9 A): the INA226 reports -1892 mA for this case, so `power_monitor::read()` applies `forward_ma()` (`kInaForwardSign = -1` in `power_calc.hpp`, the only place that knows the shunt polarity). `flow_of()`, `direction_of()` ("up" = positive), the WebHID arrow and the energy meter all treat positive as forward.
* Home, Diag, the `[PWR]` line (`... dir=up|down|idle`) and `k2_remote.py state/watch` print magnitudes only (`fmt_abs_fixed`, `StrBuf::put_abs_fixed`; never a '-'). The remote JSON keeps the signed `current_ma` and adds `direction` (`up|down|idle`). Home draws one green vertical arrow (U+2191 positive = bottom port to top port, U+2193 negative, blank idle) after the A row; ports are never called source or sink by position.
* `EnergyMeter` counts forward energy only (negative power is clamped to 0), no subtraction on reverse.
* Follow-up (not done): one-shot zero-offset calibration of the INA current.

#### Display smoothing

`power_monitor` keeps four views of the same sample, taken every ~20 ms by `poll()`:

| Call | What it is | Used by |
| :--- | :--- | :--- |
| `read_display()` | VBUS and current through the One Euro filter of the chosen preset; power = smoothed V x smoothed I; dead band applied after the filter | the readouts, the graphs and trends (`UiContext::pm`) |
| `read_raw()` | the latest sample as read (dead band only) | the energy meter (`EnergyMeter`), so Wh never depends on the preset |
| `read()` | 128-sample average (~2.5 s), unchanged | remote and web state, logs, link detection, the charger test's current, cable resistance (`UiContext::pm_avg`) |
| `vbus_fast_mv()` | latest VBUS, unfiltered, unchanged | every test and step decision (landed, band, dark, source lost, timeout) |

`k2/ui/smoothing.hpp` is pure code (host tests in `tests/test_smoothing.cpp`, no heap, float on the FPU, no transcendental functions). The One Euro filter (Casiez, Roussel, Vogel, CHI 2012) is an exponential low-pass whose cutoff rises with the signal's speed: `dx_hat = lowpass(dx/dt, d_cutoff)`, `fc = min_cutoff + beta * |dx_hat|`, `x_hat = lowpass(x, fc)`, `alpha = r / (1 + r)`, `r = 2 pi fc dt`. A steady rail is calmed by `min_cutoff`; a step makes the derivative large, the cutoff jumps far above the sample rate and the step passes at once. `dt` comes from `systick::millis()` (32-bit wrap safe). VBUS and current are filtered in volts and amps with the same parameters.

Presets (Settings > Smoothing, default Medium). Measured on the host with 50 Hz samples and +-20 mV uniform jitter on a 9 V rail: noise = output rms / input rms, step = fraction of a 0.5 V step on screen after the 1st / 2nd sample (20 / 40 ms):

| Preset | min_cutoff | beta | d_cutoff | noise left | 0.5 V step, 1st / 2nd sample |
| :--- | :--- | :--- | :--- | :--- | :--- |
| Off | - | - | - | 100 % (identity) | 100 / 100 % |
| Low | 2.0 Hz | 100 | 1.0 Hz | 65 % | 97 / 100 % |
| Medium | 0.5 Hz | 20 | 1.0 Hz | 43 % | 88 / 99 % |
| High | 0.3 Hz | 10 | 1.0 Hz | 35 % | 78 / 96 % |

A drop of 0.5 V therefore shows within one display refresh (one sample) at Low and Medium and within two at High, while the jitter is cut by more than half at Medium. A 1 V/s ramp trails the input by less than 0.1 V at every preset. The preset is applied with `power::set_smoothing()` (re-seeds the filter, the next sample is shown as read) and stored in `/prefs.cfg` byte 6 (see the file table: 0 = default, else 1 + preset index; older firmware ignores it).

Verified on-device 2026-11-04 (cf064ec, Anker Nano II 100W on the bottom port, 9 V contract held via `pd attach 9000 --confirm`): the Settings > Smoothing row cycles Off/Low/Medium/High with the hint text changing on screen, and the home readout at the held 9 V differs between Off and Medium (pixel diff in the readout region; raw VBUS 9.06-9.08 V over 10 s, no hard resets). Screenshots: /tmp/k2runs/smooth-settings-{0,off,medium}.png, smooth-home-{off,medium}-9v.png.

### k2_remote.py

```
python3 tools/k2_remote.py ping | state | pdos | emarker | reboot
python3 tools/k2_remote.py dfu --confirm
python3 tools/k2_remote.py press {back,minus,ok,plus} [--long | --ms N]
python3 tools/k2_remote.py page {home,detail,chargertest,lines,tools,settings,connection,diag} # or 0..5; old names pd/source, emarker/cable, identify/sink open Connection on that tab
python3 tools/k2_remote.py action {pd-rescan,emarker,energy-reset,fc-scan,fc-reset,identify} [--confirm]   # fc-scan needs --confirm
python3 tools/k2_remote.py log [on|off]
python3 tools/k2_remote.py screenshot out.png [--scale N] [--debug] [--dump-raw FILE]
python3 tools/k2_remote.py debug pdos --fake N
python3 tools/k2_remote.py emarker list | save | delete <id> --confirm | rename <id> "<name>" | spoof <id>|off|status
python3 tools/k2_remote.py store list | save-emarker | save-log
python3 tools/k2_remote.py watch [--interval MS] [--count N]
python3 tools/k2_remote.py bootinfo [--json]
```

`press` retries while the firmware answers Busy (the previous virtual press still runs) for the press time + 0.5 s, so presses chain in scripts. `action` waits up to 15 s (an e-marker scan blocks the main loop). On a timeout the client pings once: an answer means "did not finish in time", silence gives the DFU hint. The PING git field is 24 bytes.

The Python client is unit-tested against the same frame layouts (`tests/test_k2_remote.py`, run by `make test`).

### WebHID dashboard

`tools/web/k2.html` is a browser front end for the same remote protocol: one self-contained file (inline CSS and JS, no CDN, works offline), prototype status. It needs a Chromium browser (WebHID) and a secure context, which `http://localhost` is.

```
make web          # python3 -m http.server --bind 127.0.0.1 -d tools/web 8765, then open http://localhost:8765/k2.html
make web-sync     # after editing tools/web/k2proto.js: copy it into the <script id="k2proto"> block of k2.html
```

* Connect calls `navigator.hid.requestDevice` with a filter on 0716:5060; a device granted earlier reconnects on load and after re-enumeration. Reports are sent with `sendReport(0, frame)` (the descriptor has no report ID) and arrive through `inputreport`. A bootloader frame (`FF 55`) shows a DFU banner.
* `tools/web/k2proto.js` is the protocol layer (no DOM): `packRequest`, `parseResponse`, `classify` (response / bootloader / log / idle, the same split as `k2_flasher.is_log_report`), the decoders, `rle565Decode`, `Link` (seq 1..255, one request in flight, timeout, Busy retry, log and bootloader callbacks) and `K2Remote` (ping, state, pdos, emarker, adcRaw, ripple, identity, fc, probe, button, action, setPage, debugPdos, log, dfuEnter, screenshot). It mirrors `tools/k2_remote.py`; Python stays the reference. The same file is embedded in `k2.html`, and `tests/test_k2proto.mjs` fails if the two differ.
* UI: a 12-column card grid (folds to 6 and 1 columns, light and dark, numbers and hex in monospace). There is no virtual K2 and no screenshot on the page (`K2Remote.screenshot` stays for the tools). Cards:
  * Power: V, A (magnitude), W, flow arrow, mWh, a client-side timer (since connect, energy reset, or an energy counter that went down), Reset energy, the V/A/W chart (1..60 min).
  * USB-PD: spec revision, active CC, PDOs received (the PDO count; no PD message counter exists), the PDO table, link line (inline / waiting / stale, age) when GET_STATE is the 49-byte form, Refresh, Rescan, and a collapsed Debug block with the fake-PDO loader. A rescan on an inline link answers NeedConfirm (5): the page shows the refusal and a Rescan anyway button that resends with confirm.
  * Saved cables: list, save the current cable, rename (text field), spoof, delete (asks to confirm), Spoof off; refreshed on demand.
  * Cable / e-marker: all decoded fields, the part 2 detail when the firmware has it, Scan cable, "scanned X ago" (link age, else the local clock).
  * Lines / analog: D+, D-, CC1, CC2, aux in V, board NTC and MCU temperature in deg C, VREFINT raw and VDD (`4095 * 1200 / vref_raw`), the fast-charge verdict, and a collapsed ADC raw table (ADC_RAW, channels 0..7, 16, 17 and the scan slots).
  * Ripple: sample-time select (codes 0..7 = 1.5 .. 239.5 cycles), Capture (RIPPLE, 512 samples from 0x8B frames), canvas plot with min/max lines, statistics.
  * Device: firmware, git, protocol, uptime, chip, the firmware log switch, Reboot, Enter DFU (confirm dialog). There is no device-statistics command: USB resets / interrupts / TX / dropped / poll gap come from the `[USB]` log line, the reset cause from the boot-time `[RST]` line and the last `[FAULT]` line, all parsed by `parseLogStats`, so they need the log on. DEBUG_FAULT is not exposed.
  * Identity (GET_IDENTITY), Fast-charge trigger (FC) and PD trigger (PD_REQUEST/PD_RESET/PD_RELEASE/PD_TRIGGER_STATUS/PD_ATTACH 0x41-0x48, anything above 5 V asks a browser confirm and the firmware answers NeedConfirm without the flag; Attach defaults to 5 V and is refused inline): hidden until a probe on connect gets an answer other than "unknown command" (`K2Remote.probe`, the `PROBES` list in the page script). The Fast-charge card follows the on-device trigger page (`include/k2/ui/ui_trigger.hpp`): protocol select, Voltage -/+ stepping `FC_LEVELS[proto].mv` (default level per protocol), Engage (a second confirm above 5 V; `trigger` carries the confirm flag), Stop (`reset`), Scan (confirm). A BadArg refusal is explained by an FC status call (`fail` text); only "load attached" offers "Engage anyway", which resends with confirm plus allow-inline (flag bit 1) after another confirm, like the device's override arm. Firmware follow-up: a device-stats command (USB resets, interrupts, TX, drops, reset cause, last fault) would replace the log-line parsing.
  * Log console: follow switch, clear.
* Polling: GET_STATE every poll interval (default 500 ms); PDOs on a change of the state key and every 5 s, e-marker every 10 s or when the cable age shows a new scan, ADC raw and ping every 3 and 5 s, identity and fast-charge status every 5 and 3 s when present. The Link keeps one request in flight, so ripple and scans queue behind them.
* Linux: the hidraw node needs permission. The udev rule `/etc/udev/rules.d/99-witrn*.rules` has `SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0716", MODE="0666"`; reload with `sudo udevadm control --reload && sudo udevadm trigger` and replug. Only one program can use the device: close `k2_remote.py` and `k2_flasher.py --monitor` first.
* `node --test tests/test_k2proto.mjs` runs the protocol tests (byte vectors shared with `tests/test_k2_remote.py`); `make test` runs them when `node` exists and prints a skip line otherwise.

## Tools hub and modal submenus

* The page rotation is Home, Detail, Charger test, Lines, Tools, Settings.
* `Ui` keeps a stack of at most 3 `Menu` ids (`push_menu`, `pop_menu`, `menu_depth`, `menu`, `page_index`). Menus: see "Page indices". Submenu pages are `PageBase` objects with id `Page::Count`.
* While a submenu is open, `Ui::on_button` uses `dispatch_modal` (`include/k2/ui/ui_events.hpp`): long +/- do not page, Hold BACK is still Reboot, the page sees every other event first, and an unconsumed short BACK returns `Route::Pop`. Leaving a page always calls its `leave()`.
* Tools page: short +/- select a tool (clamped), OK opens it, long +/- page as usual. BACK inside a tool returns to the list.
* Trigger submenu (`include/k2/ui/ui_trigger.hpp`, pure `trig::Model`): short +/- step the target (QC2 levels, QC3 200 mV, SCP 1 V, ...) clamped to the protocol and `power::max_vbus_mv()`; long +/- cycle the protocol (wrap, SCAN is entry 0); OK engages. Above 5 V the first OK arms ("OK again: 9.0V", 3 s window), the second engages. While engaged, short +/- retarget live when the protocol allows it, otherwise stop and re-arm. BACK (and any other way of leaving) calls `fctrig::stop()` (dpdm reset to passive idle) whenever something was armed, engaged, failed, refused or scanned, and pops; if nothing was triggered the bus is left alone (no reset pulse).
* E-marker submenu: `pd::emarker_mode(attached, vbus_known, vbus_mv)` picks the mode per scan. Vconn (VBUS below 800 mV or unknown with nothing attached): the K2 powers the cable and sends SOP' Discover Identity. Sniff (a source is attached, VBUS up): the K2 does not inject; it listens for the source's own SOP' traffic. When a sniff scan decodes nothing the page shows "Listening... N" for `Ui::kListenWindowMs` (5 s, `UiContext::em_listen_until`); a new `cable_seq` ends the window early, otherwise it expires into "Nothing heard" with the replug-the-charger-end hint (`em_quiet`). OK is "Listen" in Sniff mode and "Scan" in Vconn mode.
* E-marker query variants (`k2/pd/emk_variant.hpp`): a Vconn scan tries two orientations (A: CC1 BMC, CC2 VCONN; B: CC2 BMC, CC1 VCONN), each with 40 ms of VCONN settle and then up to three Discover Identity attempts, in the order of one constexpr table of (PD header revision, SVDM version): PD 3.x + SVDM 2.0, PD 3.x + SVDM 1.0, PD 2.0 + SVDM 1.0. The first attempt is what a current source sends; the second finds cables that answer only a PD 3.x header carrying SVDM 1.0 (a Baseus 100 W C-to-C cable, see `docs/CHARGER_QUIRKS.md`); the third finds old PD 2.0 cables. Each attempt is one 2 ms flush plus an 80 ms window (about 235 ms measured per silent attempt), so a scan of a line with no cable now costs about 1.63 s (four attempts, 1.16 s, before): 2 x (40 + 3 x 235) ms plus the setup and VCONN-off overhead; the floor with the fake chip's fixed delays is 2 x (40 + 3 x 80) = 560 ms. The learn plan (`emk_learn`) walks the same table for its own Discover Identity, and every later request of the session (Discover SVIDs, Discover Modes, the seeds, the sweep) goes out in the variant that was answered, with that PD header revision and that SVDM version in the VDM header (before, the follow-ups used the header revision to choose the SVDM version: PD 3.x implied SVDM 2.0 and PD 2.0 implied SVDM 1.0, so a PD 3.x + SVDM 1.0 cable could not be learned at all). `fusb302::cable_exchange` sends the header revision and VDM words it is given, so it is transparent to the variant. The log line names the variant: `[EMK] sw0=0x24 attempt 1 (PD 3.0, SVDM 1.0): SOP' GoodCRC after 4 ms`. Callers that wait for a scan: the charger-test START, Identify (which does not use this scan), `emarker learn` (10 s remote timeout) and the spoof guard (refuses before any timing) all have budgets above 1.7 s; the remote `action` wait is 15 s. Where the answered combination is reported (`fusb302::cable_answered()`, an `emk_variant::Answered` of orientation, header revision, SVDM version and 1-based attempt; invalid until a cable answers): (1) the log line `[EMARK] Response on Orientation B (CC2 BMC, CC1 VCONN), PD 3.x header, SVDM 1.0, attempt 2`; (2) the Cable tab of the Connection page, one dim row `Answered  B, PD3, SVDM 1.0` after `Length` (`emk_variant::short_text`, width checked by a `static_assert` for every orientation and table row; with VCONN required it is the ninth and last row of the list); (3) the remote: GET_EMARKER part 2 grows from 10 to 14 bytes (tail: orientation 0 none / 1 A / 2 B, header revision 1 PD 2.0 / 2 PD 3.x, SVDM 1 or 2, attempt 1-based) and the EM_LIB learn summary from 13 to 17 bytes with the same tail; `k2_remote --json emarker learn|learned` and the web tool decode them into `orientation`, `header_rev`, `svdm`, `attempt`, null from older firmware, and the text render adds `answered: orientation A (CC1 BMC, CC2 VCONN), PD 3.x header, SVDM 2.0, attempt 1`. Not stored: the saved cable record (`emrec`) has no request-variant field (a bump would need a versioned tail), the report card has no spare byte outside its packed fields, and the CDC shell has no cable-read output (its `log on` shows the `[EMARK]` line).
* E-marker scan with a source attached (`k2/pd/emk_attached.hpp`, `fusb302.cpp read_emarker_impl`): a cable chip that lives on the charger's supply (the Baseus 100 W C-to-C cable: silent to a K2 that reads it alone, answers the charger) starves when the alone scan drops the K2's Rd, because SWITCHES0 0x24 (orientation A) and 0x18 (orientation B) carry no pull-down: the charger sees a detach and takes VBUS and VCONN away mid-scan. So when a source is attached the scan changes mode. "Attached" is `attached(Facts)`: the driver has a locked active line, SWITCHES0 before the scan has the pull-down of that line, and any one of the power monitor's VBUS at or above the vSafe5V minimum (`pd::kVSafe5MinMv`, the same source of truth as the rest of the firmware, fed to the driver by `sink_vbus()`), valid source caps of this attach, or a sink engine that is not idle; a charger that only shows Rp (VBUS 0, no caps, sink idle) is the cable-alone case. STATUS0 VBUSOK and the passive `Engine::contract.valid` are not inputs (hardware 2026-11-11: VBUSOK read 0 at 5.02 V, and the flag is only set from a Request seen on the wire, not from the K2's own); they are read and logged for diagnosis. The attached plan is one orientation, the one whose BMC line is the live CC line, with SWITCHES0 = pull-down | measure on the live line | VCONN on the other: active CC1 gives 0x25 (PDWN1 + MEAS_CC1 + VCONN_CC2, SWITCHES1 0x25, orientation A), active CC2 gives 0x1A (PDWN2 + MEAS_CC2 + VCONN_CC1, SWITCHES1 0x26, orientation B). VCONN is never on the live wire (with `active_cc=2` the old 0x24 put VCONN_CC2 on the live CC2 line and dropped the Rd; with `active_cc=1` orientation B did the same on CC1). The attempt order is unchanged (the three variants of the table), the scan stops at the first variant the cable GoodCRCs and waits the full response time for the answer; a variant that was acknowledged and then not answered is logged `[EMK] sw0=0x25 attempt 2 (PD 3.0, SVDM 1.0): acked, no answer` and the next variant is tried. PC13 (`pins::cc_vconn_gnd`, an Ra of about 1 kohm to ground on the CC1 net, `src/pd/emk_ra.hpp`) is driven high for the scan as before, except when the live line is CC1: there it would sit beside the K2's Rd on the live wire, so the attached plan keeps it low (one field, `Plan::pc13`; hardware check pending). The alone scan (no source) is unchanged: two orientations, 0x24 then 0x18, no Rd, PC13 high. The restore at the end of the scan is the same in both modes (the SWITCHES0, SWITCHES1 and POWER the scan found). What the last read did is kept per attach (`fusb302::last_cable_read()`, next to `physical_cable()`): mode none / alone / attached and the number of variants the chip acknowledged; log `[EMK] scan mode attached: active_cc=1 vbus_mv=5023 caps=1 sink=5 (diag vbus_ok=0 contract=0)` (sink = `pd::SinkState` number, 5 = Ready). Remote and web tool: GET_EMARKER part 2 grows from 14 to 16 bytes (tail: read mode 0 none / 1 alone / 2 attached, acknowledged-variant count) and the EM_LIB learn summary from 17 to 19 bytes with the same tail; the learn outcome has a fourth value, 4 `acked-no-answer` (the chip acknowledged the K2 but never answered), sent as outcome byte + 1 like the others; `k2_remote --json emarker ...` and the web tool decode them into `read_mode`, `attached` (true / false, null from older firmware or when no read ran) and `acks`, and the text render adds `read: source attached (Rd kept), 1 variant(s) acknowledged` or `read: cable alone`. This is how the "cable not read" facts tell silent alone from silent with the source attached. A read the driver refuses before it touches the wire (Ra mode on, a live sink inline) clears `last_cable_read()` to mode None and 0 acks first, so a refusal never reports the mode and acks of the read before it (fixed 2026-11-11, `tests/test_fusb302_scan.cpp` `test_refused_read_clears_the_last_read`); only the Sniff path returns earlier and keeps the last read. "Attached" never rests on the FUSB302 VBUSOK bit: it does not see the bottom receptacle's VBUS (STATUS0 0x23 / 0x33 at 5.1 V and 20 V, `docs/K2_PORTS_SPEC.md`, "Electrical identity"), so VBUS comes from the power monitor and VBUSOK is only an extra sign in `cc_probe.hpp` `vbus_abort`.
* Silent chip versus no chip (`k2/pd/ra_probe.hpp`, `ra_class.hpp`, `cable_fact.hpp`; pins and levels in `docs/K2_PORTS_SPEC.md`, "The CC front end"): an alone scan that finds no e-marker cannot tell a chipless cable from a cable whose chip is present but unpowered or silent, so the driver then runs a short probe of the chip's pull-down (Ra) on the cable's supply pin. It runs only when everything is restored and nothing else is on the wire (no source attached, no contract, no Rd, VBUS down); otherwise it refuses with a reason and the verdict stays "not probed". Three phases of three samples (about 330 ms in all): idle (nothing driven), VCONN on CC1, VCONN on CC2, each sampled at 2, 10 and 100 ms; the 2 ms sample is a transient and is never judged. A pull-down on CC2 collapses the K2's own CC2 idle bias (1.64-1.71 V) to 0 mV; under VCONN a driven pin with a pull-down reads about 2.64 V, a bare CC1 about 2.87 V and a bare CC2 about 3.27 V. Each driven line is judged against its own threshold (CC1 2775 mV, CC2 2960 mV, midway between its loaded and bare levels), never against the other line, and its 10 ms and 100 ms samples must sit on the same side; the line that is not driven is never judged. A verdict needs both signs, a single sample never decides, and anything else (a pin crossing its threshold between 10 and 100 ms, a held PLUS key, a missing sample, an aborted run) is `Unclear`. `fusb302::pull_down()` returns `PullDown` {NotProbed, None, Cc1, Cc2, Unclear}; it is reset at every scan and at detach. The pure `cablefact::classify(read, ReadInfo, PullDown)` folds that with the read outcome into `Fact` {Read, ChipSilent, NoChip, NotRead}: a read cable is `Read`; a chip that acknowledged the K2 without answering, or a pull-down on CC1 or CC2, is `ChipSilent`; no pull-down is `NoChip`; anything else (unclear, not probed, a source attached) stays `NotRead`. The Cable tab says "CHIP, NO ANSWER" / "NO CHIP FOUND" with the footer "Chip silent" / "No chip found", and the charger-test START log prints the fact word next to the limits. Display and log only: the limits still read an unread cable as unread (3 A, 20 V default), whatever the fact. The remote and web tool carry the verdict after the attached-scan fields, which keep their positions: GET_EMARKER part 2 grows from 16 to 17 bytes (byte 16 = pull-down), the EM_LIB learn summary from 19 to 20 bytes (byte 19), and `k2_remote emarker probe --json` (`EmarkerProbe`, 0x56) returns the raw samples of a fresh probe for fitting the thresholds. The thresholds are fitted to the 2026-11-11 hardware runs (empty port, chipless cable, a Baseus cable in both orientations, a CUKTECH 6 A cable; margins in `ra_class.hpp`, raw counts in `tests/test_ra_class.cpp`).
* PD trigger submenu (`PdTriggerPage`, pure model `TriggerSel` in `include/k2/ui/ui_pdtrig.hpp`): lists the source's PDOs. Short +/- adjust the target mV (PPS/AVS, in Adjust mode) or step the item (fixed PDO); long +/- move to the next/previous PDO; OK requests it (`Ui::device_trigger`), with a second confirm above 5 V (`trigger_needs_confirm`); long OK toggles V/A in Adjust mode. BACK releases the contract to 5 V (`fusb302::sink_release`) and pops. The submenu refuses outright when the link is inline (a live sink is attached) or there are no source caps, and leaves by itself if the link goes inline or detaches. A request made over the remote (PD_REQUEST) is not tied to the page: it stays until PD_RELEASE, a hard reset or detach.
* Identify (an entry of the Connection page's Actions pop-up, not a page): runs the Discover Identity / SVIDs / Modes / Get_Sink_Cap / Status / Revision sequence (`Action::Identify` = 6, remote IDENTIFY_DEVICE 0x40). The progress and result show in the footer and the answers fill the Sink tab rows (`ID`, `USB`, `Alt`, `Caps`); a skipped step reads "no answer". While a live sink is inline it is refused outright (no confirm): an inline K2 never transmits, and the Sink tab shows the sniffed identity marked "sniff" instead.
* Page names live in four places that `tests/test_page_names.py` keeps equal: `Page`/`Menu` in `include/k2/ui/ui_types.hpp`, `PAGES` in `tools/k2_remote.py`, `PAGES`/`MENUS` in `tools/web/k2proto.js` and the copy embedded in `tools/web/k2.html` (`make web-sync`). `GET_STATE.page` 8 + n decodes as `tools/<menu>` or `settings/<menu>`.

### Charging procedure runner

`ChargerTestSession` (resident in `Ui`; `ChargerTestPage` is only its view) owns `charger_test::Runner`, its bounded 95-step result array and protocol
selection. The pure runner uses the existing PD/FC drivers through a narrow environment;
it owns no hardware globals and allocates no heap. `Ui::tick` polls it with fresh power/link
state independently of rendering, including after page exit while releasing. Capture/redraw
never advances the test state. Text fields and a cached footer limit updates to changed pixels.

PD uses the real advertised PDOs (up to 1 A request, not a current-load test); PPS/AVS sweep
min/mid/max/mid/min on a retained contract. Each PPS APDO ends with one full-current row (top voltage at the APDO's
whole current, cable-clamped; docs/CHARGER_TEST_FLOW.md). Fixed/discrete tests reset between targets; continuous
QC/SCP retain the handshake for direction tests. Before advancing across a boundary, release
requires measured 4.75-5.50 V (vSafe5V) and a quiescent driver for 500 ms. Release timeout latches Unsafe,
blocks other tools, and requires a fresh safe baseline before clearing without resuming.
EPR release sends EPR_Mode Exit after the PDO1 PS_RDY, stopping keep-alives at 5 V.
PPS keep-alive renegotiation is allowed during a voltage hold, but hard resets or bad voltage fail.
Each target holds for one continuous second after reaching its required band; QC2 uses +/-2% and
has a bounded longer ramp timeout before the hold starts.
Legacy tests on a live PD table are skipped explicitly; no inline override is used.
QC3 Class B can request 20 V; QC3+ retains its 12 V ceiling and existing trigger UI ranges are unchanged.

The full setup, thresholds and exclusions are in `HW_TEST_PROCEDURES.md`, "On-device charging suite".

## E-marker menu and Spoof (device UI)

Tools -> E-marker: after a successful scan a long OK opens a menu (`include/k2/ui/ui_emmenu.hpp`, pure):
Save (`storage::lib_save_current()` saves the cable as /emk/NNNN.emk), View saved, Spoof on/off.

Spoof answers an SOP' Discover Identity request with the saved record (`emk_spoof::build_reply`: header
`0x518F`-style, 5 data objects, cable-plug bit, spec rev from the record, MessageID 0..7, FIFO token stream
without a TXON token, TX_START like every other transmit). Limits on this chip, none verified on hardware yet:
- The FUSB302 receives SOP' (CONTROL1 ENSOP1) and its Auto-CRC sends the GoodCRC itself; Auto-CRC is off in
  inline mode (live sink), so arming is refused there.
- The source only queries a cable when it sees Ra on the second CC. The FUSB302 cannot present Ra; whether the
  PC13 `cc_vconn_gnd` switch does is unknown. Without Ra a charger may never send the request.
- Replies are sent from `poll()` (20 ms); the source's tVDMSenderResponse is about 30 ms, so a late poll can miss it.

## Charger test: PD re-attach for the low programmable rows

A run is a list of independent test units (`charger_test::Unit`: Pd, one per legacy protocol, PdLow), default order is the company-family order of the wizard (2026-11-06; `kDefaultUnits`): the OEM vendor units FCP, SCP, AFC, VOOC, SVOOC, SVOOC65, VIVO (one shared PD-silent attach), then Pd (its Mi 6A hunt rows are the unit's tail, shown under Xiaomi), then QC2, QC3, QC3+, UFCS (a second PD-silent attach), then the low programmable rows last (`low_programmable()`: the C135's 3.3 V step latches the source until a mains replug). The probe order inside each group is fixed (`kDefaultUnits`); the PD rows are planned late when the caps are unknown at START (`Runner::replan_pd`). Every unit is self-contained and declares a session mode (`Mode::Pd` or `Mode::Legacy`), so the order can be rearranged (`Options::units`, remote `Cmd::ChargerUnits` 0x0C, `tools/ct_start.py --units qc2,qc3`; the list serves one START). The old single-sink handover / re-attach (`State::Handover`/`Reattach`, `Env::reattach`) is gone: states 8 and 9 are now `State::Detach` / `State::Attach`.

Unit lifecycle (`Runner::begin_detach` / `begin_attach`, `Env::detach()` / `Env::attach(Mode)`, `fusb302::unit_detach()` / `unit_attach(legacy)`):

1. Detach: sink forgotten (no release Request), Rd off, VBUS dark (below `pd::kVSafe0Mv` held `kDetachDarkHoldMs` = 300 ms, bounded by `kDetachDarkMs` = 10 s; a source that never lets go, e.g. a USB-A port, gets `kDetachQuickMs` from the second unit on). A USB-A port has no CC; the unit goes on after the bound.
2. Attach(mode): Rd on as a fresh attach. `Mode::Pd`: the sink is PD-active, the first Source_Capabilities get exactly one Request (PDO 1), later re-announces are answered; the Runner then waits with `pd::VbusWait` for the caps and up to `kAttachContractMs` (3 s) for a Ready 5 V contract (else every pending row of the unit is `NoReattach`, "no PD after re-attach"; a source with no PD at all gets the plan's NoOffer). `Mode::Legacy`: PD-silent, caps are heard and recorded but never acknowledged. The unit is ready when PD has been quiet for `kLegacyQuietMs` (2.5 s) and VBUS has been steady at 5 V for `kLegacySteadyMs`; Hard Reset VBUS cycles are allowed while the source gives up. No fallback after `kLegacyGiveUpMs` (20 s): detach and one longer retry (`kLegacyRetryMs` = 60 s); still none: the unit's rows are skipped with `Reason::BlockedByPd` ("blocked by PD") and the card's handover outcome says "stayed PD".
3. Rows run (Baseline 500 ms of vSafe5V first), VBUS judged on the latest sample (`power::vbus_fast_mv`), the transition capture armed per row. QC3 / QC3+ rows are a closed loop in `fc::Engine` (`qc::` helpers: `Settle`, `plan_pulses`, `batch_pulses`, `open_loop_limit_mv`): a batch is followed by a stable-VBUS wait (3 samples within 50 mV, bounded at 500 ms without movement), the last QC3 steps go out as single pulses, the aim is target - 100 mV inside a -300 / +150 mV band, and a level above the open-loop bound (expected pulse count plus one step) aborts as overvoltage. Every batch writes Black Box mark 5 (`kMarkQc3Batch`: VBUS in mV, signed pulse count; decoded as `qc3-batch` by `tools/pd_trace.py`). Both lines are held at 0 V for 500 ms after any QC release before the next detection (`Engine::stop`/`abort` stamp it, `start` waits it out). A QC3+ attempt that never leaves ~5 V ends with `fc::Fail::NoResponse`, which the runner records as `Reason::NoQc3Plus` ("no QC3+ response", reason 33 in the 6-bit card field, `kReasonCount` 35; reason 34 is `Reason::ClassAOnly`, "Class A only (12 V max)": the neutral Skip of a QC Class B 20 V row on a charger that stops at 12 V, no capture): a Skip for the row and the group, never a Fail; the reason is named in `charger_test.cpp`, `report_card.hpp` (`group_why`), `tools/report_card.py`, `tools/k2_remote.py` and the web UI. The card carries a passive label (`report_card::qc_label`: Qualcomm VID 0x05C6 plus a PPS range is "QC4+", at 100 W or more "QC5", otherwise none) shown in the legacy/QC section of the results and the card (`ui_cards.cpp`, `result_layout` `add_note`) and as an optional `"qc"` key of the card JSON; behaviour never keys on it.
4. Release, then detach only at a session boundary (the session mode changes: Legacy to Pd, Pd to Legacy, Legacy to PdLow, and the run's end). Consecutive Legacy units share one PD-silent attach (`Runner::continue_legacy`): the release drives D+/D- to 0 V for `kLegacyGapMs` (500 ms), the line must be PD-quiet at 5 V again, and the next handshake starts (AFC `kAfcPreMs` = 500 ms later; the first handshake of a session waits `kLegacyFirstWaitMs` = 1000 ms after the attach settled). PD traffic or a VBUS excursion between two Legacy units falls back to a detach plus a fresh PD-silent attach. A unit list run alone still attaches in its own mode.

A unit failure never stops the run; only `ShutOff` (a PD row cut below its target whose source never returns, "source shut off (needs replug)") skips the rest. A PD row cut whose source then returns is `Reason::CutRecovered` ("VBUS cut, source recovered"), is not retried, and the runner waits (bounded by `kCapsReturnMs`) for valid caps before the next PD row. The PD plan takes its source capabilities from the first Legacy attach (caps are heard there), else from the first Pd attach (`late_pd_`). The identity probe runs at the end of the first Pd unit, while the charger sits in its 5 V contract.

One owner of the Request in Pd mode: the START's first caps queue the single Request for PDO 1 (`handle_frame`, `start_request_blockers`); `pd::VbusWait` holds the START wait for up to `kStartContractWaitMs` (3 s) while that Request is Requesting/Transition/Aborting; `Sink::release` while a PDO 1 Request is in flight only clears `held` and sends nothing (hardware 2026-11-04, UGREEN X865: a release during the pending first Request sent a second Request 12 ms later, TX timeout, two Accept/PS_RDY pairs). The Runner itself calls `request_pd` only for the rows. A reboot inside a unit restores from the journal as Skip(RebootHandover) for a Legacy attach (`Phase::Handover`) or Stopped with the pending rows otherwise.

## World replay (host)

A host test that runs the real firmware loop in a modelled world, so timing bugs (a frame lost behind a long repaint,
a Rd gap across a reboot) fail in `make test` instead of on the bench. How to run it, the timing model with its
calibration, and how to add a scenario are in [TESTING.md](TESTING.md).

| Part | What it is | Where |
| :--- | :--- | :--- |
| Firmware under test | the real `mainloop::pass()` and LCD idle hook, `bootmode::decide`, `bootpd::early/late`, `rdcomeback`, the FUSB302 and power-monitor drivers, the whole UI, the fs volume | `src/core/main_loop.cpp`, `src/core/boot_pd.cpp`, `src/core/rd_comeback.cpp`, `src/drivers/fusb302.cpp`, ... |
| The world | chips and nets, never firmware: a FUSB302 register file with a routed RX FIFO and TX, an INA226, the ADC nets, VBUS, the CC levels, a virtual nanosecond clock with per-call costs | `tests/world_sim.{h,cpp}` |
| LCD cost | wire time per pixel plus a calibrated CPU share, with the real idle hook inside the paint | `tests/fake_lcd.{h,cpp}` (`LcdTiming`) |
| Scenarios | a bench recording turned into the world the K2 sat in (VBUS, Rp levels, ADC, every PD frame) with per-episode expectations | `tools/trace2scenario.py` → `tests/scenarios/*.scn` |
| Driver | one forked process per run (scenario × episode × page × loop phase); the reboot cases fork one process per firmware life | `tests/test_world_replay.cpp` |

Build: the world objects are compiled with `-DK2_HOST_PINS -DK2_WORLD_STUBS` and come first on the link line;
`tests/ui_stubs.cpp` leaves out its clock, FUSB302, power, ADC and Rd-comeback stand-ins under `K2_WORLD_STUBS`.
What a power loss keeps is modelled as the hardware keeps it: the flash volume and the backup registers (session record
and pin mark, up to a hold time per scenario; the host world supplies `session::init` / `backup_alive` over the same
record) survive a dark, SRAM does not (the `.noinit` probe reads back as garbage). The Rd marker's voucher
(`session::backup_alive`) is thus decided by the firmware itself, see TESTING.md "Reboot world".
The world must stay a model of hardware: any firmware logic the test needs is called, never copied. When a test needs a
firmware sequence that only exists inline in `main.cpp`, move it behind a function both call (as `main_loop` and
`boot_pd` were).
